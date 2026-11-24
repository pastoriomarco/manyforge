#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <manyforge_deployment/activation_content_collection.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "lite6_retained_consumer_fixture.hpp"
#include "program_root_fixture.hpp"

namespace {

namespace fs = std::filesystem;
using manyforge::core::ContentAddressedStoreKey;
using manyforge::core::ContentDigest;
using manyforge::core::ProgramRevision;
using manyforge::core::RetainedBundleId;
using manyforge::deployment::ActivationContentWriterLock;
using manyforge::deployment::ActiveActivationContent;
using manyforge::deployment::CanonicalValue;
using manyforge::deployment::canonical_json;
using manyforge::deployment::ActivationContentCollection;
using manyforge::deployment::ActivationContentReferences;
using manyforge::deployment::FilesystemCatalogArtifactStore;
using manyforge::deployment::FilesystemProgramRepository;
using manyforge::deployment::FilesystemRawArtifactStore;
using manyforge::deployment::FilesystemRetainedBundleRepository;
using manyforge::deployment::FilesystemVerifiedRetainedBundleSource;
using manyforge::deployment::FilesystemVerifiedRetainedModelMapSource;
using manyforge::deployment::RetainedModelMapQuery;
using manyforge::deployment::ProgramBindingContract;
using manyforge::deployment::RetainedBundle;
using manyforge::deployment::sha256_digest;
using manyforge::deployment::verify_program_binding_contract;
using manyforge::deployment::test::make_program_root;
using manyforge::deployment::test::materialize_lite6_composer_assistant_retained_consumer_fixture;
using manyforge::deployment::test::materialize_lite6_retained_consumer_fixture;
using manyforge::deployment::test::ProgramOptions;
using manyforge::deployment::test::with_program_revision;

class ScopedRoot final {
public:
  ScopedRoot()
      : path_(fs::temp_directory_path() /
              ("manyforge-content-collection-" + std::to_string(::getpid()) +
               "-" + std::to_string(sequence_.fetch_add(1U)))) {
    fs::remove_all(path_);
    fs::create_directory(path_);
  }
  ~ScopedRoot() {
    std::error_code error;
    fs::remove_all(path_, error);
  }
  [[nodiscard]] const fs::path &path() const noexcept { return path_; }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  fs::path path_;
};

/// Every committed content-store entry below `root`, root-relative.
[[nodiscard]] std::set<std::string> content_files(const fs::path &root) {
  std::set<std::string> files;
  for (const char *directory : {"retained-bundles", "programs",
                                "program-bindings", "raw-artifacts",
                                "catalog-artifacts"}) {
    if (!fs::exists(root / directory))
      continue;
    for (const auto &entry : fs::recursive_directory_iterator(root / directory))
      if (entry.is_regular_file() &&
          !entry.path().filename().string().starts_with("."))
        files.insert(fs::relative(entry.path(), root).string());
  }
  return files;
}

[[nodiscard]] std::string hex(std::string_view digest) {
  return std::string(digest.substr(digest.size() - 64U));
}

[[nodiscard]] std::shared_ptr<const ProgramBindingContract>
make_program(const fs::path &root, const RetainedBundle &bundle,
             const std::string &root_instance_id) {
  ProgramOptions options;
  options.catalog_content_version =
      bundle.value().catalogs.node_environment.content_version;
  auto members = make_program_root(options).as_object();
  // A Program has no name (OD-369): distinct Programs differ in their tree.
  for (auto &[field, value] : members) {
    if (field != "tree")
      continue;
    auto tree = value.as_object();
    for (auto &[tree_field, tree_value] : tree)
      if (tree_field == "instance_id")
        tree_value = CanonicalValue(root_instance_id);
    value = CanonicalValue(std::move(tree));
  }
  // The Program references the bundle's components, so verification takes the
  // bundle's exact model map and catalog artifacts from the same root.
  const auto &products = bundle.value().compiled_products.symbol_role_maps;
  if (products.size() != 1U)
    throw std::logic_error("Lite6 fixture bundle has no sole model map");
  const FilesystemVerifiedRetainedModelMapSource maps(root);
  const auto map = maps.get_verified(RetainedModelMapQuery{
      bundle.retained_bundle_id(), products.front().product_id});
  const FilesystemCatalogArtifactStore catalog(root);
  return verify_program_binding_contract(
      canonical_json(with_program_revision(CanonicalValue(std::move(members)))),
      bundle, map, catalog);
}

/// A collection that knows no earlier references, as the first one after a
/// start: it reads every live document.
[[nodiscard]] ActivationContentCollection
collect_unreferenced_activation_content(const fs::path &root,
                                        const ActiveActivationContent &active) {
  ActivationContentReferences fresh;
  return manyforge::deployment::collect_unreferenced_activation_content(
      root, [&] { return active; }, fresh);
}

[[nodiscard]] fs::path program_path(const fs::path &root,
                                    const ProgramBindingContract &program) {
  return root / "programs" / "sha256" /
         (hex(program.value().program_revision.value()) + ".json");
}

void write_file(const fs::path &path, std::string_view bytes) {
  fs::create_directories(path.parent_path());
  std::ofstream output(path, std::ios::binary);
  output << bytes;
}

/// A prepared activation as the collection reads it: a document naming what
/// it carries by digest.
void write_prepared(const fs::path &root, std::string_view name,
                    std::string_view body) {
  write_file(root / "prepared-activations" /
                 (hex(sha256_digest(name)) + ".json"),
             body);
}

[[nodiscard]] ActiveActivationContent
active(const RetainedBundle &bundle,
       const ProgramBindingContract *program = nullptr) {
  ActiveActivationContent result{{bundle.retained_bundle_id()}, {}, {}, {}};
  if (program != nullptr)
    result.program_revision = program->value().program_revision;
  return result;
}

struct TwoBundleRoot final {
  ScopedRoot root;
  std::shared_ptr<const RetainedBundle> older;
  std::shared_ptr<const RetainedBundle> current;
  std::set<std::string> older_files;
  std::set<std::string> current_files;
};

/// An activation root holding an older bundle and then the active one, as
/// two Loads leave it, plus each bundle's own complete file set.
[[nodiscard]] std::unique_ptr<TwoBundleRoot> make_two_bundle_root() {
  auto result = std::make_unique<TwoBundleRoot>();
  {
    ScopedRoot alone;
    static_cast<void>(
        materialize_lite6_composer_assistant_retained_consumer_fixture(alone.path()));
    result->older_files = content_files(alone.path());
  }
  {
    ScopedRoot alone;
    static_cast<void>(materialize_lite6_retained_consumer_fixture(alone.path()));
    result->current_files = content_files(alone.path());
  }
  result->older =
      materialize_lite6_composer_assistant_retained_consumer_fixture(
          result->root.path())
          .bundle;
  result->current =
      materialize_lite6_retained_consumer_fixture(result->root.path()).bundle;
  return result;
}

TEST(ActivationContentCollectionTest,
     KeepsTheActiveClosureAndSharedContentAndRemovesTheRest) {
  const auto fixture = make_two_bundle_root();
  const auto &root = fixture->root.path();
  ASSERT_NE(fixture->older->retained_bundle_id(),
            fixture->current->retained_bundle_id());
  std::set<std::string> shared;
  for (const auto &file : fixture->current_files)
    if (fixture->older_files.contains(file))
      shared.insert(file);
  ASSERT_FALSE(shared.empty()) << "the two bundles must share content";

  const auto program = make_program(root, *fixture->current, "active");
  const auto old_program = make_program(root, *fixture->current, "older");
  {
    FilesystemProgramRepository programs(root);
    ASSERT_TRUE(programs.put_if_absent(program, *fixture->current));
    ASSERT_TRUE(programs.put_if_absent(old_program, *fixture->current));
  }
  const std::string orphan_blob = "an orphaned raw artifact";
  const std::string orphan_catalog = "an orphaned catalog artifact";
  static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(orphan_blob));
  static_cast<void>(FilesystemCatalogArtifactStore(root).put_if_absent(
      ContentAddressedStoreKey("catalog/test/" + hex(sha256_digest(orphan_catalog))),
      ContentDigest(sha256_digest(orphan_catalog)), orphan_catalog));
  const auto before = content_files(root);

  const auto collected = collect_unreferenced_activation_content(
      root, active(*fixture->current, program.get()));
  EXPECT_TRUE(collected.collected);
  EXPECT_EQ(collected.examined_entries, before.size());
  EXPECT_GT(collected.read_bytes, 0U);
  const auto after = content_files(root);
  for (const auto &file : fixture->current_files)
    EXPECT_TRUE(after.contains(file)) << "active content removed: " << file;
  for (const auto &file : fixture->older_files) {
    if (!fixture->current_files.contains(file)) {
      EXPECT_FALSE(after.contains(file)) << "older content kept: " << file;
    }
  }
  EXPECT_TRUE(fs::exists(program_path(root, *program)));
  EXPECT_FALSE(fs::exists(program_path(root, *old_program)));
  EXPECT_EQ(after.size() + collected.removed_entries, before.size());
  EXPECT_EQ(after.size(), fixture->current_files.size() + 1U);

  // A restart reopens every store (each verifies all its entries) and loads
  // the active configuration from the collected root.
  EXPECT_NO_THROW({
    FilesystemRawArtifactStore raw(root);
    FilesystemCatalogArtifactStore catalogs(root);
    FilesystemRetainedBundleRepository bundles(root);
  });
  const auto reloaded = FilesystemVerifiedRetainedBundleSource(root).get_verified(
      fixture->current->retained_bundle_id());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_NE(FilesystemProgramRepository(root).get_verified(program->value().program_revision,
                                                       *reloaded),
            nullptr);
  EXPECT_EQ(FilesystemVerifiedRetainedBundleSource(root).get_verified(
                fixture->older->retained_bundle_id()),
            nullptr);

  // Collecting again changes nothing.
  EXPECT_EQ(collect_unreferenced_activation_content(
                root, active(*fixture->current, program.get()))
                .removed_entries,
            0U);
}

TEST(ActivationContentCollectionTest,
     RetainedPreparedActivationsAndTheReplacedBundleKeepTheirClosures) {
  for (const bool through_prepared : {true, false}) {
    const auto fixture = make_two_bundle_root();
    const auto &root = fixture->root.path();
    auto roots = active(*fixture->current);
    if (through_prepared)
      write_prepared(root, "older",
                     "{\"retained_bundle_id\":\"" +
                         fixture->older->retained_bundle_id().value() + "\"}");
    else
      roots.retained_bundle_ids.push_back(fixture->older->retained_bundle_id());

    const auto collected = collect_unreferenced_activation_content(root, roots);
    EXPECT_TRUE(collected.collected);
    EXPECT_EQ(collected.removed_entries, 0U);
    const auto after = content_files(root);
    for (const auto &file : fixture->older_files)
      EXPECT_TRUE(after.contains(file)) << "older content removed: " << file;
    for (const auto &file : fixture->current_files)
      EXPECT_TRUE(after.contains(file)) << "active content removed: " << file;
    EXPECT_NE(FilesystemVerifiedRetainedBundleSource(root).get_verified(
                  fixture->older->retained_bundle_id()),
              nullptr);
  }
}

TEST(ActivationContentCollectionTest,
     LaterCollectionsReadOnlyNewLiveDocumentsAndForgetRetiredOnes) {
  const auto fixture = make_two_bundle_root();
  const auto &root = fixture->root.path();
  const auto roots = active(*fixture->current);
  write_prepared(root, "older",
                 "{\"retained_bundle_id\":\"" +
                     fixture->older->retained_bundle_id().value() + "\"}");
  const auto collect = [&](ActivationContentReferences &known) {
    return manyforge::deployment::collect_unreferenced_activation_content(
        root, [&] { return roots; }, known);
  };

  // The first collection (a start) reads every live document.
  ActivationContentReferences known;
  const auto first = collect(known);
  ASSERT_TRUE(first.collected);
  EXPECT_GT(first.read_bytes, 0U);
  EXPECT_EQ(first.reused_documents, 0U);
  const auto all_live = known.tokens_by_path.size();
  ASSERT_GT(all_live, 0U);

  // Nothing new: nothing is read again.
  const auto unchanged = collect(known);
  EXPECT_EQ(unchanged.read_bytes, 0U);
  EXPECT_EQ(unchanged.reused_documents, all_live);
  EXPECT_EQ(unchanged.removed_entries, 0U);

  // An Apply's new content: only its documents are read.
  const std::string blob = "artifact of the new Apply";
  static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(blob));
  const std::string prepared = "{\"raw\":\"" + sha256_digest(blob) + "\"}";
  write_prepared(root, "new", prepared);
  const auto added = collect(known);
  EXPECT_EQ(added.read_bytes, blob.size() + prepared.size());
  EXPECT_EQ(added.reused_documents, all_live);
  EXPECT_EQ(added.removed_entries, 0U);
  EXPECT_EQ(known.tokens_by_path.size(), all_live + 2U);

  // Retired content is removed and forgotten, without reading anything.
  fs::remove_all(root / "prepared-activations");
  const auto retired = collect(known);
  EXPECT_EQ(retired.read_bytes, 0U);
  EXPECT_GT(retired.removed_entries, 0U);
  for (const auto &file : fixture->older_files) {
    if (!fixture->current_files.contains(file)) {
      EXPECT_FALSE(known.tokens_by_path.contains((root / file).string()))
          << "retired content still known: " << file;
    }
  }
  EXPECT_FALSE(known.tokens_by_path.contains(
      (root / "raw-artifacts" / "sha256" / (hex(sha256_digest(blob)) + ".blob"))
          .string()));

  // A start reads the remaining live documents in full again and agrees.
  ActivationContentReferences restarted;
  const auto full = collect(restarted);
  EXPECT_GT(full.read_bytes, 0U);
  EXPECT_EQ(full.removed_entries, 0U);
  const auto paths = [](const ActivationContentReferences &references) {
    std::set<std::string> result;
    for (const auto &[path, tokens] : references.tokens_by_path)
      result.insert(path);
    return result;
  };
  EXPECT_EQ(paths(restarted), paths(known));
}

TEST(ActivationContentCollectionTest, LoadsBeyondTheStoreBoundKeepSucceeding) {
  ScopedRoot scoped;
  const auto &root = scoped.path();
  const auto bundle = materialize_lite6_retained_consumer_fixture(root).bundle;
  constexpr std::size_t kBound = 3U;
  constexpr std::size_t kLoads = 4U * kBound;

  // Each Load stores a new Program and a new raw artifact named by its
  // prepared activation; each successful Apply then collects.
  std::shared_ptr<const ProgramBindingContract> program;
  for (std::size_t load = 0U; load < kLoads; ++load) {
    program = make_program(root, *bundle, "load_" + std::to_string(load));
    const std::string blob = "artifact of Load " + std::to_string(load);
    ASSERT_NO_THROW({
      FilesystemProgramRepository programs(root, kBound);
      ASSERT_TRUE(programs.put_if_absent(program, *bundle));
      FilesystemRawArtifactStore raw(root);
      static_cast<void>(raw.put_if_absent(blob));
    }) << "Load " << load;
    fs::remove_all(root / "prepared-activations");
    write_prepared(root, "load", "{\"raw\":\"" + sha256_digest(blob) + "\"}");
    ASSERT_TRUE(
        collect_unreferenced_activation_content(root, active(*bundle, program.get()))
            .collected);
    EXPECT_EQ(FilesystemProgramRepository(root, kBound).size(), 1U);
    EXPECT_TRUE(fs::exists(root / "raw-artifacts" / "sha256" /
                           (hex(sha256_digest(blob)) + ".blob")));
    EXPECT_FALSE(fs::exists(
        root / "raw-artifacts" / "sha256" /
        (hex(sha256_digest("artifact of Load " + std::to_string(load - 1U))) +
         ".blob")));
  }

  // Without collection the same bound refuses the next Loads.
  EXPECT_THROW(
      {
        FilesystemProgramRepository programs(root, kBound);
        for (std::size_t extra = 0U; extra < kBound; ++extra)
          static_cast<void>(programs.put_if_absent(
              make_program(root, *bundle, "uncollected_" + std::to_string(extra)),
              *bundle));
      },
      std::exception);
}

TEST(ActivationContentCollectionTest, APreparationInProgressDefersCollection) {
  ScopedRoot scoped;
  const auto &root = scoped.path();
  const auto bundle = materialize_lite6_retained_consumer_fixture(root).bundle;
  const std::string blob = "written by a preparation in progress";
  {
    const ActivationContentWriterLock writing(root);
    static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(blob));
    const auto deferred =
        collect_unreferenced_activation_content(root, active(*bundle));
    EXPECT_FALSE(deferred.collected);
    EXPECT_EQ(deferred.removed_entries, 0U);
    EXPECT_EQ(deferred.examined_entries, 0U);
    EXPECT_EQ(deferred.read_bytes, 0U);
    write_prepared(root, "in progress",
                   "{\"raw\":\"" + sha256_digest(blob) + "\"}");
  }
  // Published: its prepared activation now keeps it.
  EXPECT_EQ(collect_unreferenced_activation_content(root, active(*bundle))
                .removed_entries,
            0U);
  fs::remove_all(root / "prepared-activations");
  EXPECT_EQ(collect_unreferenced_activation_content(root, active(*bundle))
                .removed_entries,
            1U);
}

TEST(ActivationContentCollectionTest, NamedSceneMeshDigestsStayLive) {
  ScopedRoot scoped;
  const auto &root = scoped.path();
  const auto bundle = materialize_lite6_retained_consumer_fixture(root).bundle;
  // A scene mesh stored for an object an API scene call added: only the
  // current logical Scene names it.
  const std::string mesh = "a scene mesh captured by an API scene call";
  static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(mesh));
  const auto blob = root / "raw-artifacts" / "sha256" /
                    (hex(sha256_digest(mesh)) + ".blob");
  auto roots = active(*bundle);
  roots.scene_mesh_digests.push_back(ContentDigest(sha256_digest(mesh)));
  const auto kept = collect_unreferenced_activation_content(root, roots);
  ASSERT_TRUE(kept.collected);
  EXPECT_EQ(kept.removed_entries, 0U);
  EXPECT_TRUE(fs::exists(blob));
  // Once nothing names it, it is removed.
  EXPECT_EQ(collect_unreferenced_activation_content(root, active(*bundle))
                .removed_entries,
            1U);
  EXPECT_FALSE(fs::exists(blob));
}

} // namespace

TEST(ActivationContentCollectionTest,
     LiveContentIsReadOnlyOnceTheWriterLockIsHeldExclusively) {
  ScopedRoot scoped;
  const auto &root = scoped.path();
  const auto bundle = materialize_lite6_retained_consumer_fixture(root).bundle;
  const auto lock_path = root / ".activation-content.lock";
  // The logical Scene: what a commit names.
  std::vector<ContentDigest> scene;
  const auto live = [&] {
    // Evaluated only under the collector's exclusive hold: no writer can
    // hold the lock now, so nothing can be stored and committed unseen.
    const int probe = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);
    EXPECT_GE(probe, 0);
    EXPECT_NE(::flock(probe, LOCK_SH | LOCK_NB), 0);
    ::close(probe);
    auto roots = active(*bundle);
    roots.scene_mesh_digests = scene;
    return roots;
  };
  ActivationContentReferences known;
  const std::string mesh = "a scene mesh an API scene call adds";
  const auto digest = ContentDigest(sha256_digest(mesh));
  const auto blob = root / "raw-artifacts" / "sha256" / (hex(digest.value()) + ".blob");
  {
    // Headless stores the mesh and commits its object under one shared hold.
    const ActivationContentWriterLock hold(root);
    static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(mesh));
    // A collection meanwhile defers and reads nothing live.
    bool evaluated = false;
    EXPECT_FALSE(manyforge::deployment::collect_unreferenced_activation_content(
                     root, [&] { evaluated = true; return live(); }, known)
                     .collected);
    EXPECT_FALSE(evaluated);
    scene.push_back(digest);  // The commit names the mesh.
  }
  // The next collection reads the Scene after taking the lock: the mesh the
  // commit named stays.
  const auto collected =
      manyforge::deployment::collect_unreferenced_activation_content(root, live, known);
  ASSERT_TRUE(collected.collected);
  EXPECT_TRUE(fs::exists(blob));
  // A scene mesh is never read for references: a second, larger live mesh
  // adds nothing to what a fresh collection reads.
  ActivationContentReferences fresh_first;
  const auto first =
      manyforge::deployment::collect_unreferenced_activation_content(root, live, fresh_first);
  const std::string larger(1024U * 1024U, 'm');
  static_cast<void>(FilesystemRawArtifactStore(root).put_if_absent(larger));
  scene.push_back(ContentDigest(sha256_digest(larger)));
  ActivationContentReferences fresh_second;
  const auto second =
      manyforge::deployment::collect_unreferenced_activation_content(root, live, fresh_second);
  ASSERT_TRUE(first.collected && second.collected);
  EXPECT_EQ(second.read_bytes, first.read_bytes);
  EXPECT_EQ(second.removed_entries, 0U);
}

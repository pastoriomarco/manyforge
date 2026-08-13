#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include "catalog_artifact_store_internal.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "lite6_controllable_retention_fixture.hpp"
#include "lite6_structural_retention_fixture.hpp"
#include "planning_resource_map_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace {

std::atomic<bool> g_fail_directory_fsync{false};
std::atomic<std::uint64_t> g_fsync_device{0U};
std::atomic<std::uint64_t> g_fsync_inode{0U};
std::atomic<std::uint64_t> g_matching_fsync_count{0U};
std::atomic<std::uint64_t> g_fail_on_matching_fsync{1U};

class ScopedDirectoryFsyncFailure final {
 public:
  explicit ScopedDirectoryFsyncFailure(
      const std::filesystem::path& directory,
      std::uint64_t fail_on_matching_fsync = 1U) {
    if (fail_on_matching_fsync == 0U) {
      throw std::logic_error(
          "test fsync fault occurrence must be positive");
    }
    struct stat status {};
    if (::stat(directory.c_str(), &status) != 0 ||
        !S_ISDIR(status.st_mode)) {
      throw std::logic_error(
          "test could not stat directory for fsync fault");
    }
    g_fsync_device.store(
        static_cast<std::uint64_t>(status.st_dev));
    g_fsync_inode.store(
        static_cast<std::uint64_t>(status.st_ino));
    g_matching_fsync_count.store(0U);
    g_fail_on_matching_fsync.store(fail_on_matching_fsync);
    g_fail_directory_fsync.store(true);
  }

  ~ScopedDirectoryFsyncFailure() {
    g_fail_directory_fsync.store(false);
  }

  ScopedDirectoryFsyncFailure(
      const ScopedDirectoryFsyncFailure&) = delete;
  ScopedDirectoryFsyncFailure& operator=(
      const ScopedDirectoryFsyncFailure&) = delete;
};

}  // namespace

extern "C" int __real_fsync(int fd);

extern "C" int __wrap_fsync(int fd) {
  if (g_fail_directory_fsync.load()) {
    struct stat status {};
    if (::fstat(fd, &status) == 0 &&
        static_cast<std::uint64_t>(status.st_dev) ==
            g_fsync_device.load() &&
        static_cast<std::uint64_t>(status.st_ino) ==
            g_fsync_inode.load()) {
      const std::uint64_t occurrence =
          g_matching_fsync_count.fetch_add(1U) + 1U;
      if (occurrence == g_fail_on_matching_fsync.load()) {
        errno = EIO;
        return -1;
      }
    }
  }
  return __real_fsync(fd);
}

namespace manyforge::deployment {
namespace {

using namespace std::string_view_literals;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kProjectionSceneId =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kProjectionSceneContent =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

[[nodiscard]] std::shared_ptr<core::LogicalSceneSnapshotDTO>
make_projection_snapshot(const core::CellId& cell_id) {
  core::CollisionPolicySnapshot policy;
  policy.revision = {
      1U, std::string(core::kCanonicalEmptyCollisionPolicyDigest)};
  return std::make_shared<core::LogicalSceneSnapshotDTO>(
      core::LogicalSceneSnapshotDTO{
          core::SceneRevision{
              core::SceneId(std::string(kProjectionSceneId)), 7U,
              core::ContentDigest(std::string(kProjectionSceneContent))},
          cell_id,
          11U,
          3U,
          5U,
          std::move(policy),
          {},
          {},
      });
}

class AbsentPlanningModelSource final
    : public IVerifiedRetainedPlanningModelSource {
 public:
  [[nodiscard]] std::optional<VerifiedRetainedPlanningModel>
  get_verified(const RetainedPlanningModelQuery&) const override {
    ++call_count_;
    return std::nullopt;
  }

  [[nodiscard]] std::size_t call_count() const noexcept {
    return call_count_;
  }

 private:
  mutable std::size_t call_count_ = 0U;
};

class FixedPlanningModelSource final
    : public IVerifiedRetainedPlanningModelSource {
 public:
  explicit FixedPlanningModelSource(VerifiedRetainedPlanningModel model)
      : model_(std::move(model)) {}

  [[nodiscard]] std::optional<VerifiedRetainedPlanningModel>
  get_verified(const RetainedPlanningModelQuery&) const override {
    ++call_count_;
    return model_;
  }

  [[nodiscard]] std::size_t call_count() const noexcept {
    return call_count_;
  }

 private:
  VerifiedRetainedPlanningModel model_;
  mutable std::size_t call_count_ = 0U;
};

template <typename Source>
concept HasBundleInsertion =
    requires(Source& source, std::shared_ptr<const RetainedBundle> bundle) {
      source.put_if_absent(std::move(bundle));
    };

template <typename Store>
concept HasActiveAliasMutation =
    requires(Store& store, const core::RetainedBundleId& bundle_id) {
      store.set_active(bundle_id);
    };

static_assert(!HasBundleInsertion<IVerifiedRetainedBundleSource>);
static_assert(!HasActiveAliasMutation<FilesystemRawArtifactStore>);
static_assert(
    !HasActiveAliasMutation<FilesystemRetainedBundleRepository>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedBundleSource,
              FilesystemVerifiedRetainedBundleSource>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedProductArtifactSource,
              FilesystemVerifiedRetainedProductArtifactSource>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedModelMapSource,
              FilesystemVerifiedRetainedModelMapSource>);
static_assert(std::is_base_of_v<
              IVerifiedRetainedPlanningModelSource,
              FilesystemVerifiedRetainedPlanningModelSource>);
static_assert(!std::is_base_of_v<
              IVerifiedRetainedBundleSource,
              FilesystemRetainedBundleRepository>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedProductArtifact>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedRuntimeUnitProduct>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedModelMap>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedPlanningResource>);
static_assert(!std::is_default_constructible_v<
              VerifiedRetainedPlanningModel>);
static_assert(!std::is_aggregate_v<
              VerifiedRetainedPlanningResource>);
static_assert(!std::is_aggregate_v<
              VerifiedRetainedPlanningModel>);
static_assert(std::is_copy_constructible_v<
              VerifiedRetainedPlanningResource>);
static_assert(std::is_move_constructible_v<
              VerifiedRetainedPlanningResource>);
static_assert(std::is_copy_constructible_v<
              VerifiedRetainedPlanningModel>);
static_assert(std::is_move_constructible_v<
              VerifiedRetainedPlanningModel>);
static_assert(!std::is_aggregate_v<
              VerifiedRetainedProductArtifact>);
static_assert(!std::is_aggregate_v<VerifiedRetainedModelMap>);
static_assert(std::is_copy_constructible_v<VerifiedRetainedModelMap>);
static_assert(std::is_move_constructible_v<VerifiedRetainedModelMap>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedBundleSource,
              std::filesystem::path>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedProductArtifactSource,
              std::filesystem::path>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedModelMapSource,
              std::filesystem::path>);
static_assert(std::is_constructible_v<
              FilesystemVerifiedRetainedPlanningModelSource,
              std::filesystem::path>);
static_assert(!std::is_constructible_v<
              FilesystemVerifiedRetainedBundleSource,
              const FilesystemRetainedBundleRepository&,
              const FilesystemRawArtifactStore&>);

class ScopedTrustedRoot final {
 public:
  ScopedTrustedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-retention-store-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error("test could not create trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment qualify_empty(
    std::string_view cell_id) {
  const std::string document =
      "{\"api_version\":\"manyforge.deployment.v1\","
      "\"deployment_artifact_id\":\"i3_durable_store_fixture\","
      "\"deployment_revision\":\"" +
      std::string(kZeroDigest) + "\",\"cell_id\":\"" +
      std::string(cell_id) +
      "\",\"name\":\"I3 durable store fixture\","
      "\"catalogs\":{\"records\":[]},\"graph\":" +
      empty_graph() +
      ",\"desired_control_configurations\":[],"
      "\"capability_provider_bindings\":[]}";
  const auto deployment = compute_deployment_artifact_revision(
      parse_strict_json_model_yaml(document),
      std::span<const QualifiedRuntimeProfile>{});
  const auto composition = compose_cell_occurrences(
      deployment.graph(), std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment.applicability_inputs().value().site_overlay_ref,
      deployment.applicability_inputs().value().external_resources,
      nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, {}, {}, {}, {}, selected_overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
materialize_durable_bundle(
    const std::filesystem::path& root, std::string_view cell_id) {
  FilesystemBuildManifestRepository manifest_repository(
      root);
  detail::RejectingCatalogArtifactStore catalog_store;
  FilesystemRawArtifactStore raw_store(root);
  FilesystemRetainedBundleRepository bundle_repository(root);
  const detail::RejectingCompilerInputArtifactSource input_source;
  return detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          qualify_empty(cell_id), manifest_repository, input_source,
          {}, catalog_store, raw_store, bundle_repository);
}

struct DurableLite6PlanningFixture final {
  test::Lite6StructuralRetentionFixture input;
  std::shared_ptr<const RetainedBundle> bundle;
};

struct DurableControllableLite6Fixture final {
  test::Lite6ControllableRetentionFixture input;
  std::shared_ptr<const RetainedBundle> bundle;
};

[[nodiscard]] DurableControllableLite6Fixture
materialize_durable_controllable_lite6_bundle(
    const std::filesystem::path& root) {
  auto input = test::make_lite6_controllable_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(
      input);
  FilesystemBuildManifestRepository manifest_repository(root);
  FilesystemCatalogArtifactStore catalog_store(root);
  FilesystemRawArtifactStore raw_store(root);
  FilesystemRetainedBundleRepository bundle_repository(root);
  auto bundle = detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          input.qualified, manifest_repository, input_source,
          input.structural_preparations(), catalog_store, raw_store,
          bundle_repository);
  if (bundle == nullptr) {
    throw std::logic_error(
        "controllable Lite6 fixture materialization returned no bundle");
  }
  return {std::move(input), std::move(bundle)};
}

[[nodiscard]] DurableControllableLite6Fixture
materialize_durable_native_motion_lite6_bundle(
    const std::filesystem::path &root, bool two_instances = false) {
  auto input =
      two_instances
          ? test::make_two_lite6_same_type_native_motion_retention_fixture()
          : test::make_lite6_native_motion_retention_fixture();
  const test::Lite6ControllableCompilerInputArtifactSource input_source(input);
  FilesystemBuildManifestRepository manifest_repository(root);
  FilesystemCatalogArtifactStore catalog_store(root);
  FilesystemRawArtifactStore raw_store(root);
  FilesystemRetainedBundleRepository bundle_repository(root);
  auto bundle = detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(
          input.qualified, manifest_repository, input_source,
          input.structural_preparations(), catalog_store, raw_store,
          bundle_repository);
  if (bundle == nullptr) {
    throw std::logic_error(
        "native-motion Lite6 fixture materialization returned no bundle");
  }
  return {std::move(input), std::move(bundle)};
}

[[nodiscard]] DurableLite6PlanningFixture
materialize_durable_lite6_bundle(
    const std::filesystem::path& root) {
  auto input = test::make_lite6_structural_retention_fixture();
  if (input.verified_asset.verified_artifacts().size() !=
      input.source_artifacts.size()) {
    throw std::logic_error(
        "Lite6 fixture source declaration/proof count mismatch");
  }
  for (const auto& artifact : input.source_artifacts) {
    if (std::none_of(
            input.verified_asset.verified_artifacts().begin(),
            input.verified_asset.verified_artifacts().end(),
            [&artifact](const VerifiedModelSourceArtifact& proof) {
              return proof.logical_uri == artifact.logical_uri &&
                     proof.resolved_store_key ==
                         artifact.store_key &&
                     proof.content_digest ==
                         artifact.content_digest &&
                     proof.byte_size == artifact.bytes.size();
            })) {
      throw std::logic_error(
          "Lite6 fixture source artifact has no exact verified proof " +
          artifact.logical_uri);
    }
  }
  const test::Lite6CompilerInputArtifactSource input_source(input);
  FilesystemBuildManifestRepository manifest_repository(root);
  FilesystemCatalogArtifactStore catalog_store(root);
  FilesystemRawArtifactStore raw_store(root);
  FilesystemRetainedBundleRepository bundle_repository(root);
  auto bundle =
      detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              test::qualify_lite6_passive_deployment(input),
              manifest_repository, input_source,
              input.structural_preparations(), catalog_store, raw_store,
              bundle_repository);
  for (const auto& artifact : input.source_artifacts) {
    const auto retained = raw_store.get_verified(
        artifact.store_key, artifact.content_digest,
        static_cast<std::uint64_t>(artifact.bytes.size()));
    if (!retained.has_value() || *retained != artifact.bytes) {
      throw std::logic_error(
          "Lite6 fixture materialization omitted source artifact " +
          artifact.logical_uri);
    }
  }
  return {std::move(input), std::move(bundle)};
}

[[nodiscard]] std::shared_ptr<const RetainedBundle> make_sealed_bundle(
    std::string_view cell_id) {
  ScopedTrustedRoot root;
  return materialize_durable_bundle(root.path(), cell_id);
}

[[nodiscard]] std::filesystem::path raw_path(
    const std::filesystem::path& root,
    const StoredRawArtifact& stored) {
  return root / "raw-artifacts" / "sha256" /
         (stored.content_digest.value().substr(7U) + ".blob");
}

[[nodiscard]] std::filesystem::path raw_path(
    const std::filesystem::path& root,
    const core::RetainedArtifactRefDTO& artifact) {
  return root / "raw-artifacts" / "sha256" /
         (artifact.content_digest.value().substr(7U) + ".blob");
}

[[nodiscard]] std::filesystem::path raw_path(
    const std::filesystem::path& root,
    const test::Lite6InputArtifact& artifact) {
  return root / "raw-artifacts" / "sha256" /
         (artifact.content_digest.value().substr(7U) + ".blob");
}

[[nodiscard]] std::filesystem::path catalog_path(
    const std::filesystem::path& root,
    const core::ContentAddressedStoreKey& key) {
  const auto storage_hex =
      detail::catalog_artifact_storage_hex(key.value());
  if (!storage_hex.has_value()) {
    throw std::logic_error(
        "test catalog key violates the bounded store contract");
  }
  return root / detail::kCatalogArtifactNamespaceDirectory /
         detail::kCatalogArtifactDataDirectory /
         (*storage_hex +
          std::string(detail::kCatalogArtifactFileSuffix));
}

[[nodiscard]] std::filesystem::path bundle_path(
    const std::filesystem::path& root,
    const core::RetainedBundleId& bundle_id) {
  return root / "retained-bundles" / "sha256" /
         (bundle_id.value().substr(7U) + ".json");
}

void write_file(const std::filesystem::path& path,
                std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    throw std::logic_error("test could not open retention-store file");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::logic_error("test could not write retention-store file");
  }
}

void write_file_and_fsync(const std::filesystem::path& path,
                          std::string_view bytes) {
  write_file(path, bytes);
  const int fd =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    throw std::logic_error(
        "test could not reopen retention-store residue");
  }
  const int sync_result = ::fsync(fd);
  const int close_result = ::close(fd);
  if (sync_result != 0 || close_result != 0) {
    throw std::logic_error(
        "test could not fsync retention-store residue");
  }
}

struct ProcessRaceResult final {
  std::size_t inserted{0U};
  std::size_t idempotent{0U};
  std::size_t failed{0U};
};

template <typename Put>
[[nodiscard]] ProcessRaceResult run_process_race(
    std::size_t process_count, Put put) {
  int start_pipe[2]{-1, -1};
  if (::pipe(start_pipe) != 0) {
    throw std::runtime_error("test could not create process barrier");
  }
  std::vector<pid_t> children;
  children.reserve(process_count);
  for (std::size_t index = 0U; index < process_count; ++index) {
    const pid_t child = ::fork();
    if (child < 0) {
      throw std::runtime_error("test could not fork retention writer");
    }
    if (child == 0) {
      (void)::close(start_pipe[1]);
      char token = '\0';
      ssize_t read_count = 0;
      do {
        read_count = ::read(start_pipe[0], &token, 1U);
      } while (read_count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (read_count != 1) {
        ::_exit(22);
      }
      try {
        ::_exit(put(index) ? 20 : 21);
      } catch (...) {
        ::_exit(22);
      }
    }
    children.push_back(child);
  }
  (void)::close(start_pipe[0]);
  std::string tokens(process_count, 'x');
  std::size_t offset = 0U;
  while (offset < tokens.size()) {
    const ssize_t written =
        ::write(start_pipe[1], tokens.data() + offset,
                tokens.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      (void)::close(start_pipe[1]);
      throw std::runtime_error("test could not release process barrier");
    }
    offset += static_cast<std::size_t>(written);
  }
  (void)::close(start_pipe[1]);

  ProcessRaceResult result;
  for (const pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
      ++result.failed;
      continue;
    }
    switch (WEXITSTATUS(status)) {
      case 20:
        ++result.inserted;
        break;
      case 21:
        ++result.idempotent;
        break;
      default:
        ++result.failed;
        break;
    }
  }
  return result;
}

void expect_no_thread_failures(
    std::span<const std::exception_ptr> failures) {
  for (std::size_t index = 0U; index < failures.size(); ++index) {
    if (failures[index] == nullptr) {
      continue;
    }
    try {
      std::rethrow_exception(failures[index]);
    } catch (const std::exception& error) {
      ADD_FAILURE() << "worker " << index << " failed: " << error.what();
    } catch (...) {
      ADD_FAILURE() << "worker " << index
                    << " failed with a non-standard exception";
    }
  }
}

}  // namespace

TEST(FilesystemRawArtifactStoreTest,
     DurablePutReopenAndVerifiedReadUseExactDigestKey) {
  ScopedTrustedRoot root;
  StoredRawArtifact stored{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path());
    stored = store.put_if_absent("durable\0raw\0bytes"sv);
    EXPECT_TRUE(stored.inserted);
    EXPECT_FALSE(store.put_if_absent("durable\0raw\0bytes"sv).inserted);
    EXPECT_EQ(store.size(), 1U);
    EXPECT_EQ(store.retained_bytes(), stored.byte_size);
  }
  {
    FilesystemRawArtifactStore reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_EQ(reopened.retained_bytes(), stored.byte_size);
    EXPECT_EQ(reopened.get_verified(
                  stored.content_addressed_store_key,
                  stored.content_digest, stored.byte_size),
              "durable\0raw\0bytes"sv);
    EXPECT_FALSE(reopened.get_verified(
        stored.content_addressed_store_key, stored.content_digest,
        stored.byte_size + 1U));
    EXPECT_FALSE(reopened.get_verified(
        core::ContentAddressedStoreKey("sha256/" +
                                         std::string(64U, 'f')),
        stored.content_digest, stored.byte_size));
  }
}

TEST(FilesystemRawArtifactStoreTest,
     CrossProcessSameContentHasOneInsertionAndExactAccounting) {
  ScopedTrustedRoot root;
  {
    FilesystemRawArtifactStore initialize(root.path(), 1U, 1024U, 1024U);
  }
  const std::string bytes = "cross-process immutable raw artifact";
  const ProcessRaceResult race = run_process_race(
      8U, [&](std::size_t) {
        FilesystemRawArtifactStore store(
            root.path(), 1U, 1024U, 1024U);
        return store.put_if_absent(bytes).inserted;
      });
  EXPECT_EQ(race.inserted, 1U);
  EXPECT_EQ(race.idempotent, 7U);
  EXPECT_EQ(race.failed, 0U);

  FilesystemRawArtifactStore reopened(root.path(), 1U, 1024U, 1024U);
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), bytes.size());
}

TEST(FilesystemRawArtifactStoreTest,
     CrossProcessDistinctContentSerializesOneEntryBound) {
  ScopedTrustedRoot root;
  constexpr std::array<std::string_view, 2U> kValues{
      "distinct-process-A", "distinct-process-B"};
  {
    FilesystemRawArtifactStore initialize(
        root.path(), 1U, 64U, 64U);
  }

  int start_pipe[2]{-1, -1};
  int ready_pipe[2]{-1, -1};
  ASSERT_EQ(::pipe(start_pipe), 0);
  ASSERT_EQ(::pipe(ready_pipe), 0);
  std::array<pid_t, 2U> children{};
  for (std::size_t index = 0U; index < children.size(); ++index) {
    children[index] = ::fork();
    ASSERT_GE(children[index], 0);
    if (children[index] != 0) {
      continue;
    }
    (void)::close(start_pipe[1]);
    (void)::close(ready_pipe[0]);
    try {
      FilesystemRawArtifactStore store(
          root.path(), 1U, 64U, 64U);
      const char ready = 'r';
      if (::write(ready_pipe[1], &ready, 1U) != 1) {
        ::_exit(22);
      }
      (void)::close(ready_pipe[1]);
      char token = '\0';
      ssize_t read_count = 0;
      do {
        read_count = ::read(start_pipe[0], &token, 1U);
      } while (read_count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (read_count != 1) {
        ::_exit(22);
      }
      const StoredRawArtifact stored =
          store.put_if_absent(kValues[index]);
      ::_exit(stored.inserted ? 20 : 22);
    } catch (const RetainedBundleError& error) {
      ::_exit(std::string_view(error.what()) ==
                      "durable retention repository bound is exhausted"
                  ? 21
                  : 22);
    } catch (...) {
      ::_exit(22);
    }
  }
  (void)::close(start_pipe[0]);
  (void)::close(ready_pipe[1]);
  std::array<char, 2U> ready_tokens{};
  std::size_t ready_offset = 0U;
  while (ready_offset < ready_tokens.size()) {
    const ssize_t count =
        ::read(ready_pipe[0], ready_tokens.data() + ready_offset,
               ready_tokens.size() - ready_offset);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    ASSERT_GT(count, 0);
    ready_offset += static_cast<std::size_t>(count);
  }
  (void)::close(ready_pipe[0]);
  const std::array<char, 2U> tokens{'x', 'x'};
  ASSERT_EQ(::write(start_pipe[1], tokens.data(), tokens.size()),
            static_cast<ssize_t>(tokens.size()));
  (void)::close(start_pipe[1]);

  std::size_t inserted = 0U;
  std::size_t bounded = 0U;
  std::size_t failed = 0U;
  for (const pid_t child : children) {
    int status = 0;
    if (::waitpid(child, &status, 0) != child ||
        !WIFEXITED(status)) {
      ++failed;
      continue;
    }
    if (WEXITSTATUS(status) == 20) {
      ++inserted;
    } else if (WEXITSTATUS(status) == 21) {
      ++bounded;
    } else {
      ++failed;
    }
  }
  EXPECT_EQ(inserted, 1U);
  EXPECT_EQ(bounded, 1U);
  EXPECT_EQ(failed, 0U);

  FilesystemRawArtifactStore reopened(
      root.path(), 1U, 64U, 64U);
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_EQ(reopened.retained_bytes(), kValues.front().size());
  std::size_t present = 0U;
  for (const std::string_view value : kValues) {
    const std::string digest = sha256_digest(value);
    const auto bytes = reopened.get_verified(
        core::ContentAddressedStoreKey(
            "sha256/" + digest.substr(7U)),
        core::ContentDigest(digest),
        static_cast<std::uint64_t>(value.size()));
    if (bytes.has_value()) {
      ++present;
      EXPECT_EQ(*bytes, value);
    }
  }
  EXPECT_EQ(present, 1U);
}

TEST(FilesystemRawArtifactStoreTest,
     InheritedStoreRejectsAfterForkAndFreshChildStoreWorks) {
  ScopedTrustedRoot root;
  FilesystemRawArtifactStore inherited(root.path(), 2U, 1024U, 1024U);
  FilesystemRetainedBundleRepository inherited_repository(root.path());
  const StoredRawArtifact parent_artifact =
      inherited.put_if_absent("parent artifact");
  const pid_t child = ::fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto rejects_inherited_use = [](auto operation) {
      try {
        operation();
      } catch (const RetainedBundleError& error) {
        return std::string_view(error.what()) ==
               "durable retention store instances must be reopened "
               "after fork";
      } catch (...) {
        return false;
      }
      return false;
    };
    if (!rejects_inherited_use([&] {
          static_cast<void>(
              inherited.put_if_absent("inherited child write"));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited.get_verified(
              parent_artifact.content_addressed_store_key,
              parent_artifact.content_digest,
              parent_artifact.byte_size));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited.get_verified(
              core::ContentAddressedStoreKey("not-a-key"),
              core::ContentDigest(std::string(kZeroDigest)), 0U));
        }) ||
        !rejects_inherited_use(
            [&] { static_cast<void>(inherited.size()); }) ||
        !rejects_inherited_use(
            [&] { static_cast<void>(inherited.retained_bytes()); }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(
              inherited_repository.put_if_absent(nullptr));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(
              inherited_repository.get_verified_envelope(
                  core::RetainedBundleId("not-a-digest")));
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited_repository.size());
        }) ||
        !rejects_inherited_use([&] {
          static_cast<void>(inherited_repository.retained_bytes());
        })) {
      ::_exit(22);
    }
    try {
      FilesystemRawArtifactStore fresh(
          root.path(), 2U, 1024U, 1024U);
      FilesystemVerifiedRetainedBundleSource fresh_source(root.path());
      if (fresh_source.get_verified(
              core::RetainedBundleId("not-a-digest")) != nullptr) {
        ::_exit(22);
      }
      const StoredRawArtifact child_artifact =
          fresh.put_if_absent("fresh child write");
      const auto reloaded = fresh.get_verified(
          child_artifact.content_addressed_store_key,
          child_artifact.content_digest, child_artifact.byte_size);
      ::_exit(child_artifact.inserted && reloaded.has_value() &&
                      *reloaded == "fresh child write"
                  ? 20
                  : 22);
    } catch (...) {
      ::_exit(22);
    }
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 20);
  EXPECT_EQ(inherited.size(), 2U);
  EXPECT_EQ(inherited_repository.size(), 0U);
}

TEST(FilesystemRawArtifactStoreTest,
     VerifiedPointReadDoesNotRehashUnrelatedStoreContents) {
  ScopedTrustedRoot root;
  FilesystemRawArtifactStore store(root.path());
  const StoredRawArtifact selected =
      store.put_if_absent("selected immutable bytes");
  const StoredRawArtifact unrelated =
      store.put_if_absent("unrelated immutable bytes");
  write_file(raw_path(root.path(), unrelated),
             "corrupt unrelated bytes");

  EXPECT_EQ(
      store.get_verified(selected.content_addressed_store_key,
                         selected.content_digest, selected.byte_size),
      "selected immutable bytes");
  EXPECT_THROW(static_cast<void>(store.size()), RetainedBundleError);
}

TEST(FilesystemRawArtifactStoreTest,
     ConcurrentDistinctContentRetainsEveryEntryWithoutAccountingRace) {
  constexpr std::size_t kThreadCount = 12U;
  ScopedTrustedRoot root;
  std::vector<std::string> values;
  std::uint64_t expected_bytes = 0U;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    values.push_back("durable-distinct-" + std::to_string(index) +
                     std::string(index + 1U, 'x'));
    expected_bytes +=
        static_cast<std::uint64_t>(values.back().size());
  }
  std::vector<std::unique_ptr<FilesystemRawArtifactStore>> stores;
  stores.reserve(kThreadCount);
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    stores.push_back(std::make_unique<FilesystemRawArtifactStore>(
        root.path(), kThreadCount, expected_bytes, expected_bytes));
  }
  std::barrier<> start(
      static_cast<std::ptrdiff_t>(kThreadCount + 1U));
  std::vector<std::exception_ptr> failures(kThreadCount);
  std::vector<std::uint8_t> inserted(kThreadCount, 0U);
  std::vector<std::thread> workers;
  for (std::size_t index = 0U; index < kThreadCount; ++index) {
    workers.emplace_back([&, index] {
      start.arrive_and_wait();
      try {
        inserted[index] = stores[index]
                                  ->put_if_absent(values[index])
                                  .inserted
                              ? 1U
                              : 0U;
      } catch (...) {
        failures[index] = std::current_exception();
      }
    });
  }
  start.arrive_and_wait();
  for (auto& worker : workers) {
    worker.join();
  }
  expect_no_thread_failures(failures);
  EXPECT_TRUE(
      std::all_of(inserted.begin(), inserted.end(),
                  [](std::uint8_t value) { return value == 1U; }));

  FilesystemRawArtifactStore reopened(
      root.path(), kThreadCount, expected_bytes, expected_bytes);
  EXPECT_EQ(reopened.size(), kThreadCount);
  EXPECT_EQ(reopened.retained_bytes(), expected_bytes);
}

TEST(FilesystemRawArtifactStoreTest,
     RejectsInvalidBoundsUnsafeRootsAndCorruptCommittedBytes) {
  ScopedTrustedRoot root;
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 0U, 1U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 0U, 1U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 1U, 2U)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path() / "missing")),
      RetainedBundleError);

  const auto symlink = root.path() / "root-symlink";
  std::filesystem::create_directory(root.path() / "symlink-target");
  std::filesystem::create_directory_symlink(
      root.path() / "symlink-target", symlink);
  EXPECT_THROW(
      static_cast<void>(FilesystemRawArtifactStore(symlink)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(symlink / "")),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRetainedBundleRepository(symlink)),
      RetainedBundleError);
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRetainedBundleRepository(symlink / "")),
      RetainedBundleError);

  {
    ScopedTrustedRoot bounded_root;
    FilesystemRawArtifactStore bounded(
        bounded_root.path(), 1U, 1U, 1U);
    EXPECT_THROW(
        static_cast<void>(bounded.put_if_absent("too-large")),
        RetainedBundleError);
    EXPECT_EQ(bounded.size(), 0U);
  }

  StoredRawArtifact stored{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path(), 1U, 16U, 16U);
    stored = store.put_if_absent("abcd");
    EXPECT_THROW(static_cast<void>(store.put_if_absent("x")),
                 RetainedBundleError);
  }
  write_file(raw_path(root.path(), stored), "abce");
  EXPECT_THROW(
      static_cast<void>(
          FilesystemRawArtifactStore(root.path(), 1U, 16U, 16U)),
      RetainedBundleError);
}

TEST(FilesystemRawArtifactStoreTest,
     ReopenPreflightsCountAndBytesBeforeHashingOutOfBudgetEntry) {
  const auto expect_exact_bound_rejection =
      [](const std::filesystem::path& root, std::size_t max_count,
         std::uint64_t max_total_bytes,
         std::uint64_t max_single_bytes) {
        try {
          FilesystemRawArtifactStore reopened(
              root, max_count, max_total_bytes, max_single_bytes);
          static_cast<void>(reopened);
          FAIL() << "out-of-budget repository unexpectedly reopened";
        } catch (const RetainedBundleError& error) {
          EXPECT_EQ(
              std::string_view(error.what()),
              "persistent durable retention repository exceeds bounds");
        } catch (const std::exception& error) {
          FAIL() << "wrong rejection type: " << error.what();
        }
      };

  {
    ScopedTrustedRoot root;
    FilesystemRawArtifactStore store(root.path(), 2U, 64U, 32U);
    EXPECT_TRUE(
        store.put_if_absent(std::string(32U, 'a')).inserted);
    EXPECT_TRUE(
        store.put_if_absent(std::string(32U, 'b')).inserted);
    expect_exact_bound_rejection(root.path(), 1U, 64U, 32U);
  }

  {
    ScopedTrustedRoot root;
    StoredRawArtifact artifact{
        core::ContentDigest(std::string(kZeroDigest)),
        core::ContentAddressedStoreKey(
            "sha256/" + std::string(64U, '0')),
        0U,
        false,
    };
    {
      FilesystemRawArtifactStore store(root.path(), 1U, 32U, 32U);
      artifact =
          store.put_if_absent(std::string(32U, 'a'));
    }
    write_file(raw_path(root.path(), artifact),
               std::string(32U, 'c'));
    // The corrupt entry exceeds the tighter bound. Preflight must reject its
    // size before any SHA-256 pass can report byte corruption.
    expect_exact_bound_rejection(root.path(), 1U, 16U, 16U);
  }
}

TEST(FilesystemRetainedBundleRepositoryTest,
     DurableInsertionReopenAndReadOnlySourceReconstructSealedValue) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_bundle_reopen");
  {
    FilesystemRetainedBundleRepository repository(root.path());
    EXPECT_FALSE(repository.put_if_absent(bundle));
    EXPECT_EQ(repository.size(), 1U);
    EXPECT_EQ(repository.retained_bytes(),
              bundle->canonical_bytes().size());
    EXPECT_NE(
        repository.get_verified_envelope(
            bundle->retained_bundle_id()),
        nullptr);
  }
  {
    FilesystemVerifiedRetainedBundleSource verified_source(root.path());
    const IVerifiedRetainedBundleSource& source = verified_source;
    const auto loaded =
        source.get_verified(bundle->retained_bundle_id());
    ASSERT_NE(loaded, nullptr);
    EXPECT_NE(loaded.get(), bundle.get());
    EXPECT_EQ(loaded->retained_bundle_id(),
              bundle->retained_bundle_id());
    EXPECT_EQ(loaded->value(), bundle->value());
    EXPECT_EQ(loaded->canonical_bytes(), bundle->canonical_bytes());
    EXPECT_TRUE(verify_retained_bundle_identity(*loaded));
    EXPECT_EQ(source.get_verified(
                  core::RetainedBundleId("not-a-digest")),
              nullptr);
  }
}

TEST(FilesystemVerifiedRetainedBundleSourceTest,
     MissingOrCorruptReferencedRawArtifactFailsClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_bundle_missing_artifact");
    const auto& artifacts =
        bundle->value()
            .compiled_products.planning_collision_model.artifact_refs;
    ASSERT_EQ(artifacts.size(), 3U);
    FilesystemVerifiedRetainedBundleSource source(root.path());
    ASSERT_TRUE(std::filesystem::remove(
        raw_path(root.path(), artifacts.front())));
    EXPECT_THROW(
        static_cast<void>(
            source.get_verified(bundle->retained_bundle_id())),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_bundle_corrupt_artifact");
    const auto& artifacts =
        bundle->value()
            .compiled_products.planning_collision_model.artifact_refs;
    ASSERT_EQ(artifacts.size(), 3U);
    FilesystemVerifiedRetainedBundleSource source(root.path());
    write_file(raw_path(root.path(), artifacts.back()),
               "corrupt retained artifact");
    EXPECT_THROW(
        static_cast<void>(
            source.get_verified(bundle->retained_bundle_id())),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     ExactTupleReturnsEchoedMetadataAndIndependentlyVerifiedBytes) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_product_point_read");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  ASSERT_EQ(product.artifact_refs.size(), 3U);

  const RetainedProductArtifactQuery urdf_query{
      bundle->retained_bundle_id(), product.product_id,
      product.product_kind, core::ArtifactRoleId("urdf")};
  const FilesystemVerifiedRetainedProductArtifactSource source(
      root.path());
  const auto first = source.get_verified(urdf_query);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->query(), urdf_query);
  EXPECT_EQ(first->product_contract_id(),
            product.product_contract_id);
  EXPECT_EQ(first->product_contract_revision(),
            product.product_contract_revision);
  const auto expected_urdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "urdf";
      });
  ASSERT_NE(expected_urdf, product.artifact_refs.end());
  EXPECT_EQ(first->artifact_ref(), *expected_urdf);
  EXPECT_EQ(first->bytes().size(), expected_urdf->byte_size);
  EXPECT_EQ(sha256_digest(first->bytes()),
            expected_urdf->content_digest.value());
  EXPECT_NE(first->bytes().find("<robot"), std::string_view::npos);

  const FilesystemVerifiedRetainedProductArtifactSource reopened(
      root.path());
  const auto second = reopened.get_verified(urdf_query);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(second->query(), first->query());
  EXPECT_EQ(second->product_contract_id(),
            first->product_contract_id());
  EXPECT_EQ(second->product_contract_revision(),
            first->product_contract_revision());
  EXPECT_EQ(second->artifact_ref(), first->artifact_ref());
  EXPECT_EQ(second->bytes(), first->bytes());
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     ComposedSymbolMapPointReadReverifiesExactArtifactAndCorruption) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_lite6_bundle(root.path());
  ASSERT_EQ(materialized.bundle->value()
                .compiled_products.symbol_role_maps.size(),
            1U);
  const auto& product = materialized.bundle->value()
                            .compiled_products.symbol_role_maps.front();
  ASSERT_EQ(product.product_kind,
            core::RetainedProductKind::kSymbolRoleMap);
  ASSERT_EQ(product.artifact_refs.size(), 1U);
  EXPECT_EQ(product.artifact_refs.front().artifact_role_id.value(),
            detail::kComposedModelSymbolRoleMapArtifactRole);

  const RetainedProductArtifactQuery query{
      materialized.bundle->retained_bundle_id(), product.product_id,
      product.product_kind, product.artifact_refs.front().artifact_role_id};
  const FilesystemVerifiedRetainedProductArtifactSource source(root.path());
  const auto loaded = source.get_verified(query);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->query(), query);
  EXPECT_EQ(loaded->product_contract_id(), product.product_contract_id);
  EXPECT_EQ(loaded->product_contract_revision(),
            product.product_contract_revision);
  EXPECT_EQ(loaded->artifact_ref(), product.artifact_refs.front());
  EXPECT_EQ(sha256_digest(loaded->bytes()),
            product.artifact_refs.front().content_digest.value());
  EXPECT_NO_THROW(
      static_cast<void>(detail::parse_and_verify_composed_model_symbol_role_map(
          loaded->bytes())));

  write_file(raw_path(root.path(), product.artifact_refs.front()),
             "corrupt composed symbol map");
  const FilesystemVerifiedRetainedProductArtifactSource reopened(root.path());
  EXPECT_THROW(static_cast<void>(reopened.get_verified(query)),
               RetainedBundleError);
}

TEST(FilesystemVerifiedRetainedModelMapSourceTest,
     ControllableLite6SurvivesDurablePlanningAndTypedModelMapPointReads) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_controllable_lite6_bundle(root.path());

  const FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto reloaded = bundle_source.get_verified(
      materialized.bundle->retained_bundle_id());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->value(), materialized.bundle->value());
  ASSERT_EQ(reloaded->value().desired_control_configurations.size(), 1U);
  EXPECT_EQ(reloaded->value().catalogs.records.size(), 8U);
  EXPECT_EQ(reloaded->value().provenance.source_records.size(), 8U);
  ASSERT_EQ(reloaded->value().compiled_products.control_domain_products.size(),
            1U);
  const auto &control_group =
      reloaded->value().compiled_products.control_domain_products.front();
  EXPECT_EQ(control_group.control_domain_id.value(), test::kLite6ArmDomainId);
  ASSERT_EQ(control_group.products.size(), 1U);
  const auto &control_product = control_group.products.front();
  ASSERT_EQ(control_product.artifact_refs.size(), 1U);
  const RetainedProductArtifactQuery control_query{
      reloaded->retained_bundle_id(), control_product.product_id,
      control_product.product_kind,
      control_product.artifact_refs.front().artifact_role_id};
  const FilesystemVerifiedRetainedProductArtifactSource artifact_source(
      root.path());
  const auto control_artifact = artifact_source.get_verified(control_query);
  ASSERT_TRUE(control_artifact.has_value());
  EXPECT_NO_THROW(
      detail::require_control_domain_control_matches_retained_bundle(
          control_artifact->bytes(), reloaded->value(),
          control_group.control_domain_id));
  ASSERT_EQ(reloaded->value().compiled_products.runtime_unit_products.size(),
            1U);
  EXPECT_EQ(reloaded->value()
                .compiled_products.runtime_unit_products.front()
                .runtime_unit_id.value(),
            test::kLite6NativeExecutionRuntimeId);

  const auto& planning =
      reloaded->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedPlanningModelSource planning_source(
      root.path());
  const auto planning_model = planning_source.get_verified(
      {reloaded->retained_bundle_id(), planning.product_id});
  ASSERT_TRUE(planning_model.has_value());
  EXPECT_FALSE(planning_model->resources().empty());

  ASSERT_EQ(reloaded->value().compiled_products.symbol_role_maps.size(), 1U);
  const auto& symbol =
      reloaded->value().compiled_products.symbol_role_maps.front();
  ASSERT_EQ(symbol.artifact_refs.size(), 1U);
  const RetainedModelMapQuery query{
      reloaded->retained_bundle_id(), symbol.product_id};
  const FilesystemVerifiedRetainedModelMapSource symbol_source(
      root.path());
  const auto model_map = symbol_source.get_verified(query);
  ASSERT_TRUE(model_map.has_value());
  RetainedModelMapQuery unknown_query = query;
  unknown_query.retained_bundle_id =
      core::RetainedBundleId(std::string(kZeroDigest));
  EXPECT_FALSE(symbol_source.get_verified(unknown_query).has_value());
  unknown_query = query;
  unknown_query.product_id = core::ProductId("unknown_symbol_map");
  EXPECT_FALSE(symbol_source.get_verified(unknown_query).has_value());
  EXPECT_EQ(model_map->query(), query);
  EXPECT_NO_THROW(model_map->require_same_bundle_as(*planning_model));
  EXPECT_EQ(model_map->query().retained_bundle_id,
            planning_model->query().retained_bundle_id);
  EXPECT_EQ(model_map->cell_frame().cell_id,
            reloaded->value().cell_graph.cell_id);
  EXPECT_EQ(model_map->model_occurrences().size(), 1U);
  ASSERT_EQ(model_map->model_collision_bodies().size(), 2U);
  EXPECT_EQ(model_map->model_collision_bodies()[0]
                .collision_body_role_id.value(),
            "arm_collision");
  EXPECT_EQ(model_map->model_collision_bodies()[0]
                .compiled_link_names.size(),
            7U);
  EXPECT_EQ(model_map->model_collision_bodies()[1]
                .collision_body_role_id.value(),
            "gripper_collision");
  EXPECT_EQ(model_map->model_collision_bodies()[1]
                .compiled_link_names.size(),
            3U);
  EXPECT_FALSE(model_map->symbols().empty());
  EXPECT_FALSE(model_map->references().empty());
  EXPECT_FALSE(model_map->coordinates().empty());
  EXPECT_FALSE(model_map->fixed_connections().empty());
  EXPECT_FALSE(model_map->frame_paths().empty());
  ASSERT_EQ(model_map->kinematic_frame_paths().size(),
            model_map->frame_paths().size());
  const auto& cell_path = model_map->kinematic_frame_paths().front();
  const auto* cell_terminal =
      std::get_if<core::ResolvedCellFrameDTO>(&cell_path.terminal_frame);
  ASSERT_NE(cell_terminal, nullptr);
  EXPECT_EQ(cell_terminal->cell_id, reloaded->value().cell_graph.cell_id);
  EXPECT_TRUE(cell_path.segments.empty());
  EXPECT_EQ(model_map->find_kinematic_frame_path(cell_path.terminal_frame),
            &cell_path);
  const core::KinematicResourceCoordinateDependencyDTO*
      retained_dependency = nullptr;
  for (const auto& path : model_map->kinematic_frame_paths()) {
    for (const auto& segment : path.segments) {
      const auto* joint =
          std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
      if (joint != nullptr &&
          !joint->eligible_resource_dependencies.empty()) {
        retained_dependency =
            &joint->eligible_resource_dependencies.front();
      }
    }
  }
  ASSERT_NE(retained_dependency, nullptr);
  const auto retained_binding = std::find_if(
      reloaded->value().cell_graph.control_bindings.begin(),
      reloaded->value().cell_graph.control_bindings.end(),
      [retained_dependency](const core::ControlBindingDTO& candidate) {
        return candidate.control_binding_id ==
               retained_dependency->control_binding_id;
      });
  ASSERT_NE(retained_binding,
            reloaded->value().cell_graph.control_bindings.end());
  EXPECT_EQ(retained_dependency->control_binding_revision,
            retained_binding->control_binding_revision);
  EXPECT_EQ(retained_dependency->control_domain_id,
            retained_binding->control_domain_id);
  const auto retained_domain = std::find_if(
      reloaded->value().cell_graph.control_domains.begin(),
      reloaded->value().cell_graph.control_domains.end(),
      [retained_dependency](const core::ControlDomainDTO& candidate) {
        return candidate.control_domain_id ==
               retained_dependency->control_domain_id;
      });
  ASSERT_NE(retained_domain,
            reloaded->value().cell_graph.control_domains.end());
  EXPECT_EQ(retained_dependency->control_domain_revision,
            retained_domain->control_domain_revision);

  const core::ControllableComponentId component_id(
      std::string(test::kLite6ControllableComponentId));
  const core::CommandTargetId command_target_id(
      std::string(test::kLite6ArmTargetId));
  const auto* binding =
      model_map->find_binding(component_id, command_target_id);
  const auto* target =
      model_map->find_target(component_id, command_target_id);
  ASSERT_NE(binding, nullptr);
  ASSERT_NE(target, nullptr);
  EXPECT_EQ(binding->control_binding_id.value(),
            test::kLite6ArmBindingId);
  ASSERT_EQ(binding->resources.size(), 6U);
  for (std::size_t index = 0U; index < binding->resources.size(); ++index) {
    EXPECT_EQ(binding->resources[index].resource_id.value(),
              "lite6_joint_" + std::to_string(index + 1U));
  }
  EXPECT_FALSE(target->model_groups.empty());
  EXPECT_FALSE(target->frames.empty());
  EXPECT_FALSE(target->collision_bodies.empty());
  EXPECT_EQ(model_map->find_binding(
                core::ControllableComponentId("other"), command_target_id),
            nullptr);
  EXPECT_EQ(model_map->find_target(
                component_id, core::CommandTargetId("other")),
            nullptr);

  const auto other_bundle = materialize_durable_bundle(
      root.path(), "typed_model_map_other_bundle");
  const auto& other_product =
      other_bundle->value().compiled_products.planning_collision_model;
  const auto other_planning_model = planning_source.get_verified(
      {other_bundle->retained_bundle_id(), other_product.product_id});
  ASSERT_TRUE(other_planning_model.has_value());
  EXPECT_THROW(model_map->require_same_bundle_as(*other_planning_model),
               RetainedBundleError);
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     CorruptOrSubstitutedSemanticBundleFailsClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_corrupt_bundle");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, product.artifact_refs.front().artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());

    std::string corrupted(bundle->canonical_bytes());
    ASSERT_FALSE(corrupted.empty());
    corrupted[corrupted.size() / 2U] =
        corrupted[corrupted.size() / 2U] == 'x' ? 'y' : 'x';
    write_file(
        bundle_path(root.path(), bundle->retained_bundle_id()),
        corrupted);
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_substituted_bundle");
    const auto substitute =
        make_sealed_bundle("durable_product_substitute");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, product.artifact_refs.front().artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());

    write_file(
        bundle_path(root.path(), bundle->retained_bundle_id()),
        substitute->canonical_bytes());
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     UnknownBundleProductKindOrRoleIsAbsent) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_product_unknown_tuple");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedProductArtifactSource source(
      root.path());
  RetainedProductArtifactQuery query{
      bundle->retained_bundle_id(), product.product_id,
      product.product_kind, core::ArtifactRoleId("urdf")};

  query.retained_bundle_id =
      core::RetainedBundleId(std::string(kZeroDigest));
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.retained_bundle_id = bundle->retained_bundle_id();

  query.product_id = core::ProductId("unknown_product");
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.product_id = product.product_id;

  query.product_kind = core::RetainedProductKind::kViewerModel;
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.product_kind = product.product_kind;

  query.artifact_role_id = core::ArtifactRoleId("unknown_role");
  EXPECT_FALSE(source.get_verified(query).has_value());
}

TEST(FilesystemVerifiedRetainedProductArtifactSourceTest,
     MissingOrCorruptSelectedBytesFailClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_missing_bytes");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const auto& artifact = product.artifact_refs.front();
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, artifact.artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());
    ASSERT_TRUE(
        std::filesystem::remove(raw_path(root.path(), artifact)));
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_product_corrupt_bytes");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const auto& artifact = product.artifact_refs.back();
    const RetainedProductArtifactQuery query{
        bundle->retained_bundle_id(), product.product_id,
        product.product_kind, artifact.artifact_role_id};
    const FilesystemVerifiedRetainedProductArtifactSource source(
        root.path());
    write_file(raw_path(root.path(), artifact), "corrupt");
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     CanonicalEmptyModelIsExactOwnedAndCopyMoveSafe) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_empty_planning_model");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  const RetainedPlanningModelQuery query{
      bundle->retained_bundle_id(), product.product_id};
  const FilesystemVerifiedRetainedPlanningModelSource source(
      root.path());

  auto loaded = source.get_verified(query);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->query(), query);
  EXPECT_TRUE(loaded->resources().empty());
  EXPECT_NE(loaded->urdf_bytes().find("<robot"),
            std::string_view::npos);
  EXPECT_NE(loaded->srdf_bytes().find("<robot"),
            std::string_view::npos);

  const auto urdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "urdf";
      });
  const auto srdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "srdf";
      });
  ASSERT_NE(urdf, product.artifact_refs.end());
  ASSERT_NE(srdf, product.artifact_refs.end());
  EXPECT_EQ(loaded->urdf_bytes().size(), urdf->byte_size);
  EXPECT_EQ(loaded->srdf_bytes().size(), srdf->byte_size);
  EXPECT_EQ(sha256_digest(loaded->urdf_bytes()),
            urdf->content_digest.value());
  EXPECT_EQ(sha256_digest(loaded->srdf_bytes()),
            srdf->content_digest.value());

  VerifiedRetainedPlanningModel copied = *loaded;
  VerifiedRetainedPlanningModel moved = std::move(copied);
  EXPECT_EQ(moved.query(), query);
  EXPECT_EQ(moved.urdf_bytes(), loaded->urdf_bytes());
  EXPECT_EQ(moved.srdf_bytes(), loaded->srdf_bytes());
  EXPECT_TRUE(moved.resources().empty());
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     UnknownBundleOrPlanningProductIsAbsent) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "durable_unknown_planning_model");
  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedPlanningModelSource source(
      root.path());

  RetainedPlanningModelQuery query{
      core::RetainedBundleId(std::string(kZeroDigest)),
      product.product_id};
  EXPECT_FALSE(source.get_verified(query).has_value());
  query.retained_bundle_id = bundle->retained_bundle_id();
  query.product_id = core::ProductId("unknown_planning_product");
  EXPECT_FALSE(source.get_verified(query).has_value());
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     MissingOrCorruptCompiledPlanningBytesFailClosed) {
  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_planning_model_missing_product");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedPlanningModelQuery query{
        bundle->retained_bundle_id(), product.product_id};
    const FilesystemVerifiedRetainedPlanningModelSource source(
        root.path());
    ASSERT_TRUE(std::filesystem::remove(
        raw_path(root.path(), product.artifact_refs.front())));
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto bundle = materialize_durable_bundle(
        root.path(), "durable_planning_model_corrupt_product");
    const auto& product =
        bundle->value().compiled_products.planning_collision_model;
    const RetainedPlanningModelQuery query{
        bundle->retained_bundle_id(), product.product_id};
    const FilesystemVerifiedRetainedPlanningModelSource source(
        root.path());
    write_file(raw_path(root.path(), product.artifact_refs.back()),
               "corrupt planning XML");
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     NonEmptyModelReturnsOnlyItsExactCanonicalOwnedResourceClosure) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_lite6_bundle(root.path());
  const auto& product =
      materialized.bundle->value()
          .compiled_products.planning_collision_model;
  const RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(),
      product.product_id};

  std::map<std::string, const test::Lite6InputArtifact*,
           std::less<>>
      expected_resources;
  for (const auto& artifact :
       materialized.input.source_artifacts) {
    if (artifact.logical_uri == test::kLite6UrdfLogicalUri ||
        artifact.logical_uri == test::kLite6SrdfLogicalUri) {
      continue;
    }
    const std::string compiled_uri =
        manyforge::deployment::detail::compiled_planning_resource_uri(
            materialized.input.catalog_reference.record_id,
            materialized.input.catalog_reference.record_revision,
            artifact.logical_uri);
    ASSERT_TRUE(
        expected_resources.emplace(compiled_uri, &artifact).second);
  }
  ASSERT_FALSE(expected_resources.empty());

  const StoredRawArtifact ambient = [&]() {
    FilesystemRawArtifactStore raw_store(root.path());
    return raw_store.put_if_absent(
        "ambient bytes outside the selected model closure");
  }();

  const FilesystemVerifiedRetainedPlanningModelSource source(
      root.path());
  auto loaded = source.get_verified(query);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->query(), query);
  EXPECT_EQ(loaded->resources().size(),
            expected_resources.size());
  EXPECT_NE(loaded->urdf_bytes().find("<robot"),
            std::string_view::npos);
  EXPECT_NE(loaded->srdf_bytes().find("<robot"),
            std::string_view::npos);

  std::string previous_uri;
  for (const auto& resource : loaded->resources()) {
    EXPECT_LT(previous_uri, resource.logical_uri());
    previous_uri = resource.logical_uri();
    const auto found =
        expected_resources.find(resource.logical_uri());
    ASSERT_NE(found, expected_resources.end());
    const auto& expected = *found->second;
    EXPECT_EQ(resource.media_type(), expected.media_type);
    EXPECT_EQ(resource.byte_size(), expected.bytes.size());
    EXPECT_EQ(resource.content_digest(),
              expected.content_digest);
    EXPECT_EQ(resource.content_addressed_store_key(),
              expected.store_key);
    EXPECT_EQ(resource.bytes(), expected.bytes);
    EXPECT_NE(resource.content_digest(), ambient.content_digest);
    EXPECT_NE(resource.bytes(),
              "ambient bytes outside the selected model closure");
  }

  const auto urdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "urdf";
      });
  const auto srdf = std::find_if(
      product.artifact_refs.begin(), product.artifact_refs.end(),
      [](const core::RetainedArtifactRefDTO& artifact) {
        return artifact.artifact_role_id.value() == "srdf";
      });
  ASSERT_NE(urdf, product.artifact_refs.end());
  ASSERT_NE(srdf, product.artifact_refs.end());
  EXPECT_EQ(sha256_digest(loaded->urdf_bytes()),
            urdf->content_digest.value());
  EXPECT_EQ(sha256_digest(loaded->srdf_bytes()),
            srdf->content_digest.value());

  VerifiedRetainedPlanningResource copied =
      loaded->resources().front();
  const std::string copied_uri(copied.logical_uri());
  const std::string copied_bytes(copied.bytes());
  VerifiedRetainedPlanningModel copied_model = *loaded;
  loaded.reset();
  VerifiedRetainedPlanningResource moved =
      std::move(copied);
  EXPECT_EQ(moved.logical_uri(), copied_uri);
  EXPECT_EQ(moved.bytes(), copied_bytes);
  VerifiedRetainedPlanningModel moved_model =
      std::move(copied_model);
  EXPECT_EQ(moved_model.query(), query);
  EXPECT_EQ(moved_model.resources().size(),
            expected_resources.size());
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     MetadataOnlyAssetRevisionsReturnDistinctCompiledResourceUris) {
  ScopedTrustedRoot root;
  const auto revision_a =
      test::make_lite6_structural_retention_fixture();
  const auto revision_b =
      test::make_lite6_structural_retention_fixture(
          "arm_collision_revision_b");
  const std::array<test::Lite6PassiveModelSelection, 2U> selections{{
      {"lite6_a", &revision_a},
      {"lite6_b", &revision_b},
  }};
  const std::array<BuildQualifiedStructuralModelPreparation, 2U>
      preparations{revision_a.preparation, revision_b.preparation};
  const test::Lite6CompilerInputArtifactSource input_source(
      {&revision_a, &revision_b});
  FilesystemBuildManifestRepository manifest_repository(root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  FilesystemRawArtifactStore raw_store(root.path());
  FilesystemRetainedBundleRepository bundle_repository(root.path());
  const auto bundle =
      detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              test::qualify_lite6_passive_deployment(selections),
              manifest_repository, input_source, preparations,
              catalog_store, raw_store, bundle_repository);
  ASSERT_NE(bundle, nullptr);

  const auto& product =
      bundle->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedPlanningModelSource source(root.path());
  const auto loaded = source.get_verified(
      {bundle->retained_bundle_id(), product.product_id});
  ASSERT_TRUE(loaded.has_value());

  const std::size_t expected_per_revision =
      revision_a.source_artifacts.size() - 2U;
  ASSERT_EQ(loaded->resources().size(),
            expected_per_revision * 2U);
  std::map<std::string, const VerifiedRetainedPlanningResource*,
           std::less<>>
      by_uri;
  for (const auto& resource : loaded->resources()) {
    ASSERT_TRUE(
        by_uri.emplace(resource.logical_uri(), &resource).second);
    EXPECT_NE(
        loaded->urdf_bytes().find(resource.logical_uri()),
        std::string_view::npos);
  }
  for (const auto& artifact : revision_a.source_artifacts) {
    if (artifact.logical_uri == test::kLite6UrdfLogicalUri ||
        artifact.logical_uri == test::kLite6SrdfLogicalUri) {
      continue;
    }
    const std::string uri_a =
        detail::compiled_planning_resource_uri(
            revision_a.catalog_reference.record_id,
            revision_a.catalog_reference.record_revision,
            artifact.logical_uri);
    const std::string uri_b =
        detail::compiled_planning_resource_uri(
            revision_b.catalog_reference.record_id,
            revision_b.catalog_reference.record_revision,
            artifact.logical_uri);
    ASSERT_NE(uri_a, uri_b);
    const auto found_a = by_uri.find(uri_a);
    const auto found_b = by_uri.find(uri_b);
    ASSERT_NE(found_a, by_uri.end());
    ASSERT_NE(found_b, by_uri.end());
    EXPECT_EQ(found_a->second->content_digest(),
              found_b->second->content_digest());
    EXPECT_EQ(found_a->second->bytes(),
              found_b->second->bytes());
  }
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     MissingOrCorruptSelectedModelResourceFailsClosed) {
  {
    ScopedTrustedRoot root;
    const auto materialized =
        materialize_durable_lite6_bundle(root.path());
    const auto& product =
        materialized.bundle->value()
            .compiled_products.planning_collision_model;
    const RetainedPlanningModelQuery query{
        materialized.bundle->retained_bundle_id(),
        product.product_id};
    const auto selected = std::find_if(
        materialized.input.source_artifacts.begin(),
        materialized.input.source_artifacts.end(),
        [](const test::Lite6InputArtifact& artifact) {
          return artifact.logical_uri !=
                     test::kLite6UrdfLogicalUri &&
                 artifact.logical_uri !=
                     test::kLite6SrdfLogicalUri;
        });
    ASSERT_NE(selected,
              materialized.input.source_artifacts.end());
    ASSERT_TRUE(
        std::filesystem::remove(raw_path(root.path(), *selected)));
    const FilesystemVerifiedRetainedPlanningModelSource source(
        root.path());
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    const auto materialized =
        materialize_durable_lite6_bundle(root.path());
    const auto& product =
        materialized.bundle->value()
            .compiled_products.planning_collision_model;
    const RetainedPlanningModelQuery query{
        materialized.bundle->retained_bundle_id(),
        product.product_id};
    const auto selected = std::find_if(
        materialized.input.source_artifacts.begin(),
        materialized.input.source_artifacts.end(),
        [](const test::Lite6InputArtifact& artifact) {
          return artifact.logical_uri !=
                     test::kLite6UrdfLogicalUri &&
                 artifact.logical_uri !=
                     test::kLite6SrdfLogicalUri;
        });
    ASSERT_NE(selected,
              materialized.input.source_artifacts.end());
    write_file(raw_path(root.path(), *selected),
               "corrupt selected model resource");
    const FilesystemVerifiedRetainedPlanningModelSource source(
        root.path());
    EXPECT_THROW(
        static_cast<void>(source.get_verified(query)),
        RetainedBundleError);
  }
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     MissingFilteredSourceXmlEntrypointFailsClosed) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_lite6_bundle(root.path());
  const auto& product =
      materialized.bundle->value()
          .compiled_products.planning_collision_model;
  const RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(),
      product.product_id};
  const auto selected = std::find_if(
      materialized.input.source_artifacts.begin(),
      materialized.input.source_artifacts.end(),
      [](const test::Lite6InputArtifact& artifact) {
        return artifact.logical_uri ==
               test::kLite6UrdfLogicalUri;
      });
  ASSERT_NE(selected,
            materialized.input.source_artifacts.end());
  ASSERT_TRUE(
      std::filesystem::remove(raw_path(root.path(), *selected)));

  const FilesystemVerifiedRetainedPlanningModelSource source(
      root.path());
  EXPECT_THROW(
      static_cast<void>(source.get_verified(query)),
      RetainedBundleError);
}

TEST(FilesystemVerifiedRetainedPlanningModelSourceTest,
     CatalogEnvelopeSplicedAcrossOpaqueKeysFailsClosed) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_lite6_bundle(root.path());
  const auto& product =
      materialized.bundle->value()
          .compiled_products.planning_collision_model;
  const RetainedPlanningModelQuery query{
      materialized.bundle->retained_bundle_id(),
      product.product_id};
  const core::ContentAddressedStoreKey alternate_key(
      "opaque:test-only-cross-key-splice");
  {
    FilesystemCatalogArtifactStore catalog_store(root.path());
    const auto stored = catalog_store.put_if_absent(
        alternate_key,
        materialized.input.catalog_reference.content_digest,
        materialized.input.catalog_bytes);
    EXPECT_TRUE(stored.inserted);
  }
  const std::string alternate_envelope =
      test::read_lite6_fixture_file(
          catalog_path(root.path(), alternate_key));
  write_file(
      catalog_path(
          root.path(),
          materialized.input.catalog_reference
              .content_addressed_store_key),
      alternate_envelope);

  const FilesystemVerifiedRetainedPlanningModelSource source(
      root.path());
  EXPECT_THROW(
      static_cast<void>(source.get_verified(query)),
      RetainedBundleError);
}

TEST(FilesystemVerifiedRetainedViewerModelSourceTest,
     NonEmptyViewerReturnsExactUrdfAndSelectedResourceClosure) {
  ScopedTrustedRoot root;
  const auto materialized = materialize_durable_lite6_bundle(root.path());
  ASSERT_TRUE(
      materialized.bundle->value().compiled_products.viewer_model.has_value());
  const auto &viewer =
      *materialized.bundle->value().compiled_products.viewer_model;
  const RetainedViewerModelQuery query{
      materialized.bundle->retained_bundle_id(), viewer.product_id};
  const FilesystemVerifiedRetainedViewerModelSource source(root.path());

  auto loaded = source.get_verified(query);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(loaded->query(), query);
  EXPECT_EQ(loaded->product_contract_id(), viewer.product_contract_id);
  EXPECT_EQ(loaded->product_contract_revision(),
            viewer.product_contract_revision);
  EXPECT_EQ(loaded->urdf_artifact_ref().artifact_role_id.value(), "urdf");
  EXPECT_EQ(loaded->urdf_artifact_ref().content_digest.value(),
            sha256_digest(loaded->urdf_bytes()));
  EXPECT_NE(loaded->urdf_bytes().find("<robot"), std::string_view::npos);

  const FilesystemVerifiedRetainedPlanningModelSource planning_source(
      root.path());
  const auto &planning =
      materialized.bundle->value().compiled_products.planning_collision_model;
  const auto planning_model = planning_source.get_verified(
      {materialized.bundle->retained_bundle_id(), planning.product_id});
  ASSERT_TRUE(planning_model.has_value());
  EXPECT_EQ(loaded->urdf_bytes(), planning_model->urdf_bytes());
  ASSERT_EQ(loaded->resources().size(), planning_model->resources().size());
  for (std::size_t index = 0U; index < loaded->resources().size(); ++index) {
    EXPECT_EQ(loaded->resources()[index].logical_uri(),
              planning_model->resources()[index].logical_uri());
    EXPECT_EQ(loaded->resources()[index].content_digest(),
              planning_model->resources()[index].content_digest());
    EXPECT_EQ(loaded->resources()[index].bytes(),
              planning_model->resources()[index].bytes());
  }

  VerifiedRetainedViewerModel copied = *loaded;
  VerifiedRetainedViewerModel moved = std::move(copied);
  loaded.reset();
  EXPECT_EQ(moved.query(), query);
  EXPECT_EQ(moved.urdf_artifact_ref().content_digest.value(),
            sha256_digest(moved.urdf_bytes()));
  EXPECT_FALSE(moved.resources().empty());
}

TEST(FilesystemVerifiedRetainedViewerModelSourceTest,
     UnknownCoordinatesAndCorruptViewerClosureFailClosed) {
  ScopedTrustedRoot root;
  const auto materialized = materialize_durable_lite6_bundle(root.path());
  ASSERT_TRUE(
      materialized.bundle->value().compiled_products.viewer_model.has_value());
  const auto &viewer =
      *materialized.bundle->value().compiled_products.viewer_model;
  const FilesystemVerifiedRetainedViewerModelSource source(root.path());

  EXPECT_FALSE(
      source
          .get_verified({core::RetainedBundleId(std::string(kZeroDigest)),
                         viewer.product_id})
          .has_value());
  EXPECT_FALSE(source
                   .get_verified({materialized.bundle->retained_bundle_id(),
                                  core::ProductId("unknown_viewer_product")})
                   .has_value());

  const auto urdf =
      std::find_if(viewer.artifact_refs.begin(), viewer.artifact_refs.end(),
                   [](const core::RetainedArtifactRefDTO &artifact) {
                     return artifact.artifact_role_id.value() == "urdf";
                   });
  ASSERT_NE(urdf, viewer.artifact_refs.end());
  write_file(raw_path(root.path(), *urdf), "corrupt viewer XML");
  EXPECT_THROW(
      static_cast<void>(source.get_verified(
          {materialized.bundle->retained_bundle_id(), viewer.product_id})),
      RetainedBundleError);
}

TEST(VerifiedRetainedProjectionInputTest,
     ExactPlanningProductAndOwnerSnapshotYieldCompositeRevision) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "retained_projection_exact");
  const auto snapshot =
      make_projection_snapshot(bundle->value().cell_graph.cell_id);
  const core::SceneRevision scene_revision = snapshot->revision;
  const FilesystemVerifiedRetainedPlanningModelSource source(root.path());

  const auto verified = verify_retained_projection_input(
      *bundle, core::LogicalSceneSnapshotRoot(snapshot), source);

  EXPECT_EQ(
      verified.projection_revision(),
      (core::ProjectionRevisionDTO{
          bundle->retained_bundle_id(),
          bundle->value().cell_graph.graph_revision,
          scene_revision}));
  EXPECT_EQ(verified.logical_scene().shared(), snapshot);
  EXPECT_EQ(verified.planning_model().query(),
            (RetainedPlanningModelQuery{
                bundle->retained_bundle_id(),
                bundle->value()
                    .compiled_products.planning_collision_model.product_id}));

  VerifiedRetainedProjectionInput copied = verified;
  VerifiedRetainedProjectionInput moved = std::move(copied);
  EXPECT_EQ(moved.projection_revision(), verified.projection_revision());
  EXPECT_EQ(moved.logical_scene().shared(), snapshot);
  EXPECT_EQ(moved.planning_model().query(),
            verified.planning_model().query());
}

TEST(VerifiedRetainedProjectionInputTest,
     CrossCellOrMutatedInvalidSnapshotRejectsBeforeProductRead) {
  ScopedTrustedRoot root;
  const auto bundle = materialize_durable_bundle(
      root.path(), "retained_projection_reject_snapshot");

  {
    const auto snapshot = make_projection_snapshot(
        core::CellId("different_projection_cell"));
    AbsentPlanningModelSource source;
    EXPECT_THROW(
        static_cast<void>(verify_retained_projection_input(
            *bundle, core::LogicalSceneSnapshotRoot(snapshot), source)),
        RetainedProjectionInputError);
    EXPECT_EQ(source.call_count(), 0U);
  }

  {
    const auto snapshot =
        make_projection_snapshot(bundle->value().cell_graph.cell_id);
    core::LogicalSceneSnapshotRoot immutable_root(snapshot);
    snapshot->revision.revision_id = 0U;
    AbsentPlanningModelSource source;
    EXPECT_THROW(
        static_cast<void>(verify_retained_projection_input(
            *bundle, std::move(immutable_root), source)),
        RetainedProjectionInputError);
    EXPECT_EQ(source.call_count(), 0U);
  }
}

TEST(VerifiedRetainedProjectionInputTest,
     MissingOrCrossBundlePlanningReadFailsClosed) {
  ScopedTrustedRoot root;
  const auto first = materialize_durable_bundle(
      root.path(), "retained_projection_first");
  const auto second = materialize_durable_bundle(
      root.path(), "retained_projection_second");

  {
    AbsentPlanningModelSource source;
    EXPECT_THROW(
        static_cast<void>(verify_retained_projection_input(
            *second,
            core::LogicalSceneSnapshotRoot(make_projection_snapshot(
                second->value().cell_graph.cell_id)),
            source)),
        RetainedProjectionInputError);
    EXPECT_EQ(source.call_count(), 1U);
  }

  const auto& first_product =
      first->value().compiled_products.planning_collision_model;
  const FilesystemVerifiedRetainedPlanningModelSource durable_source(
      root.path());
  auto first_model = durable_source.get_verified({
      first->retained_bundle_id(), first_product.product_id});
  ASSERT_TRUE(first_model.has_value());
  FixedPlanningModelSource mismatched_source(std::move(*first_model));
  EXPECT_THROW(
      static_cast<void>(verify_retained_projection_input(
          *second,
          core::LogicalSceneSnapshotRoot(make_projection_snapshot(
              second->value().cell_graph.cell_id)),
          mismatched_source)),
      RetainedProjectionInputError);
  EXPECT_EQ(mismatched_source.call_count(), 1U);
}

TEST(FilesystemRetainedBundleRepositoryTest,
     CrossProcessSameBundleHasOneInsertionAndNoMutableAlias) {
  const auto bundle = make_sealed_bundle("durable_bundle_process_race");
  ScopedTrustedRoot root;
  {
    FilesystemRetainedBundleRepository initialize(root.path());
  }
  const ProcessRaceResult race = run_process_race(
      8U, [&](std::size_t) {
        FilesystemRetainedBundleRepository repository(root.path());
        return repository.put_if_absent(bundle);
      });
  EXPECT_EQ(race.inserted, 1U);
  EXPECT_EQ(race.idempotent, 7U);
  EXPECT_EQ(race.failed, 0U);

  FilesystemRetainedBundleRepository reopened(root.path());
  EXPECT_EQ(reopened.size(), 1U);
  EXPECT_NE(
      reopened.get_verified_envelope(
          bundle->retained_bundle_id()),
      nullptr);
}

TEST(FilesystemRetainedBundleRepositoryTest,
     ConcurrentDistinctBundlesRespectCountAndByteBounds) {
  const auto first = make_sealed_bundle("durable_bundle_distinct_a");
  const auto second = make_sealed_bundle("durable_bundle_distinct_b");
  const std::uint64_t total_bytes =
      static_cast<std::uint64_t>(first->canonical_bytes().size()) +
      static_cast<std::uint64_t>(second->canonical_bytes().size());
  const std::uint64_t maximum_bytes =
      std::max(first->canonical_bytes().size(),
               second->canonical_bytes().size());

  ScopedTrustedRoot root;
  FilesystemRetainedBundleRepository first_writer(
      root.path(), 2U, total_bytes, maximum_bytes);
  FilesystemRetainedBundleRepository second_writer(
      root.path(), 2U, total_bytes, maximum_bytes);
  std::barrier<> start(3);
  std::exception_ptr first_failure;
  std::exception_ptr second_failure;
  bool first_inserted = false;
  bool second_inserted = false;
  std::thread first_thread([&] {
    start.arrive_and_wait();
    try {
      first_inserted = first_writer.put_if_absent(first);
    } catch (...) {
      first_failure = std::current_exception();
    }
  });
  std::thread second_thread([&] {
    start.arrive_and_wait();
    try {
      second_inserted = second_writer.put_if_absent(second);
    } catch (...) {
      second_failure = std::current_exception();
    }
  });
  start.arrive_and_wait();
  first_thread.join();
  second_thread.join();
  const std::array failures{first_failure, second_failure};
  expect_no_thread_failures(failures);
  EXPECT_TRUE(first_inserted);
  EXPECT_TRUE(second_inserted);

  FilesystemRetainedBundleRepository reopened(
      root.path(), 2U, total_bytes, maximum_bytes);
  EXPECT_EQ(reopened.size(), 2U);
  EXPECT_EQ(reopened.retained_bytes(), total_bytes);

  ScopedTrustedRoot bounded_root;
  FilesystemRetainedBundleRepository count_bounded(
      bounded_root.path(), 1U, total_bytes, maximum_bytes);
  EXPECT_TRUE(count_bounded.put_if_absent(first));
  EXPECT_THROW(
      static_cast<void>(count_bounded.put_if_absent(second)),
      RetainedBundleError);

  ScopedTrustedRoot single_root;
  FilesystemRetainedBundleRepository single_bounded(
      single_root.path(), 1U, first->canonical_bytes().size(),
      first->canonical_bytes().size() - 1U);
  EXPECT_THROW(
      static_cast<void>(single_bounded.put_if_absent(first)),
      RetainedBundleError);
  EXPECT_EQ(single_bounded.size(), 0U);
}

TEST(FilesystemRetentionStoreTest,
     ReopenCleansSyntheticPrepublicationAndPostlinkResidue) {
  const auto bundle = make_sealed_bundle("durable_bundle_crash_residue");
  ScopedTrustedRoot root;
  StoredRawArtifact artifact{
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey(
          "sha256/" + std::string(64U, '0')),
      0U,
      false,
  };
  {
    FilesystemRawArtifactStore raw_store(root.path());
    FilesystemRetainedBundleRepository bundle_repository(root.path());
    artifact = raw_store.put_if_absent("crash-window-artifact");
    EXPECT_TRUE(bundle_repository.put_if_absent(bundle));
  }

  const auto raw_directory =
      root.path() / "raw-artifacts" / "sha256";
  const auto bundle_directory =
      root.path() / "retained-bundles" / "sha256";
  const auto raw_postlink =
      raw_directory /
      ("." + artifact.content_digest.value().substr(7U) +
       ".blob.tmp.crash.postlink");
  const auto raw_prepublication =
      raw_directory /
      ("." + std::string(64U, '1') +
       ".blob.tmp.crash.prepublication");
  const auto bundle_postlink =
      bundle_directory /
      ("." + bundle->retained_bundle_id().value().substr(7U) +
       ".json.tmp.crash.postlink");
  const auto bundle_prepublication =
      bundle_directory /
      ("." + std::string(64U, '2') +
       ".json.tmp.crash.prepublication");
  std::filesystem::create_hard_link(
      raw_path(root.path(), artifact), raw_postlink);
  write_file(raw_prepublication, "partial");
  std::filesystem::create_hard_link(
      bundle_path(root.path(), bundle->retained_bundle_id()),
      bundle_postlink);
  write_file(bundle_prepublication, "partial");

  {
    FilesystemRawArtifactStore raw_reopened(root.path());
    FilesystemRetainedBundleRepository bundle_reopened(root.path());
    EXPECT_EQ(raw_reopened.size(), 1U);
    EXPECT_EQ(bundle_reopened.size(), 1U);
    EXPECT_FALSE(std::filesystem::exists(raw_postlink));
    EXPECT_FALSE(std::filesystem::exists(raw_prepublication));
    EXPECT_FALSE(std::filesystem::exists(bundle_postlink));
    EXPECT_FALSE(std::filesystem::exists(bundle_prepublication));
    EXPECT_NE(bundle_reopened.get_verified_envelope(
                  bundle->retained_bundle_id()),
              nullptr);
  }
}

TEST(FilesystemRetentionStoreTest,
     ReopenAndReadsDurablyAdoptSyntheticFinalOnlyCrashResidue) {
  const auto bundle =
      make_sealed_bundle("durable_bundle_final_only_residue");
  constexpr std::string_view kRawBytes =
      "synthetic-final-only-raw-artifact";
  const std::string raw_digest = sha256_digest(kRawBytes);
  const StoredRawArtifact artifact{
      core::ContentDigest(raw_digest),
      core::ContentAddressedStoreKey(
          "sha256/" + raw_digest.substr(7U)),
      static_cast<std::uint64_t>(kRawBytes.size()),
      false,
  };

  ScopedTrustedRoot root;
  FilesystemRawArtifactStore raw_already_open(root.path());
  FilesystemRetainedBundleRepository bundle_already_open(root.path());
  const auto raw_final = raw_path(root.path(), artifact);
  const auto bundle_final =
      bundle_path(root.path(), bundle->retained_bundle_id());
  const auto raw_temporary =
      raw_final.parent_path() /
      ("." + raw_digest.substr(7U) +
       ".blob.tmp.synthetic-final-only");
  const auto bundle_temporary =
      bundle_final.parent_path() /
      ("." + bundle->retained_bundle_id().value().substr(7U) +
       ".json.tmp.synthetic-final-only");

  write_file_and_fsync(raw_temporary, kRawBytes);
  std::filesystem::create_hard_link(raw_temporary, raw_final);
  ASSERT_TRUE(std::filesystem::remove(raw_temporary));
  write_file_and_fsync(
      bundle_temporary, bundle->canonical_bytes());
  std::filesystem::create_hard_link(
      bundle_temporary, bundle_final);
  ASSERT_TRUE(std::filesystem::remove(bundle_temporary));

  // This deliberately simulates kernel-visible residue after link +
  // temporary unlink and before the data-directory fsync. Fault injection
  // makes the adoption barrier observable: no reopen, read, or idempotent
  // insertion may acknowledge the final when that barrier fails.
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(raw_already_open.get_verified(
            artifact.content_addressed_store_key,
            artifact.content_digest, artifact.byte_size)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            raw_already_open.put_if_absent(kRawBytes)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_raw_directory_fsync(
        raw_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRawArtifactStore(root.path())),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            bundle_already_open.get_verified_envelope(
                bundle->retained_bundle_id())),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            bundle_already_open.put_if_absent(bundle)),
        std::runtime_error);
  }
  {
    ScopedDirectoryFsyncFailure fail_bundle_directory_fsync(
        bundle_final.parent_path());
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRetainedBundleRepository(root.path())),
        std::runtime_error);
  }

  FilesystemRawArtifactStore raw_reopened(root.path());
  FilesystemRetainedBundleRepository bundle_reopened(root.path());
  EXPECT_EQ(raw_reopened.size(), 1U);
  EXPECT_EQ(
      raw_reopened.get_verified(
          artifact.content_addressed_store_key,
          artifact.content_digest, artifact.byte_size),
      kRawBytes);
  EXPECT_EQ(bundle_reopened.size(), 1U);
  const auto adopted =
      bundle_reopened.get_verified_envelope(
          bundle->retained_bundle_id());
  ASSERT_NE(adopted, nullptr);
  EXPECT_EQ(adopted->canonical_bytes(), bundle->canonical_bytes());
}

TEST(FilesystemRetentionStoreTest,
     NewPutFailsClosedWhenPostpublicationFsyncFailsAndReopenAdopts) {
  ScopedTrustedRoot root;
  constexpr std::string_view kRawBytes =
      "postpublication-fsync-raw-artifact";
  const std::string raw_digest = sha256_digest(kRawBytes);
  const StoredRawArtifact expected_artifact{
      core::ContentDigest(raw_digest),
      core::ContentAddressedStoreKey(
          "sha256/" + raw_digest.substr(7U)),
      static_cast<std::uint64_t>(kRawBytes.size()),
      false,
  };
  {
    FilesystemRawArtifactStore store(root.path());
    ScopedDirectoryFsyncFailure fail_postpublication_fsync(
        root.path() / "raw-artifacts" / "sha256", 2U);
    EXPECT_THROW(
        static_cast<void>(store.put_if_absent(kRawBytes)),
        std::runtime_error);
  }
  {
    FilesystemRawArtifactStore reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    EXPECT_EQ(
        reopened.get_verified(
            expected_artifact.content_addressed_store_key,
            expected_artifact.content_digest,
            expected_artifact.byte_size),
        kRawBytes);
    EXPECT_FALSE(reopened.put_if_absent(kRawBytes).inserted);
  }

  const auto bundle =
      make_sealed_bundle("postpublication_fsync_bundle");
  {
    FilesystemRetainedBundleRepository repository(root.path());
    ScopedDirectoryFsyncFailure fail_postpublication_fsync(
        root.path() / "retained-bundles" / "sha256", 2U);
    EXPECT_THROW(
        static_cast<void>(repository.put_if_absent(bundle)),
        std::runtime_error);
  }
  {
    FilesystemRetainedBundleRepository reopened(root.path());
    EXPECT_EQ(reopened.size(), 1U);
    const auto adopted =
        reopened.get_verified_envelope(
            bundle->retained_bundle_id());
    ASSERT_NE(adopted, nullptr);
    EXPECT_EQ(adopted->canonical_bytes(), bundle->canonical_bytes());
    EXPECT_FALSE(reopened.put_if_absent(bundle));
  }
}

TEST(FilesystemRetainedBundleRepositoryTest,
     CorruptionAndTighterReopenBoundsFailClosed) {
  const auto bundle = make_sealed_bundle("durable_bundle_corruption");
  {
    ScopedTrustedRoot root;
    {
      FilesystemRetainedBundleRepository repository(root.path());
      EXPECT_TRUE(repository.put_if_absent(bundle));
    }
    std::string corrupted(bundle->canonical_bytes());
    const auto cell = corrupted.find("durable_bundle_corruption");
    ASSERT_NE(cell, std::string::npos);
    corrupted[cell] = corrupted[cell] == 'd' ? 'e' : 'd';
    write_file(bundle_path(root.path(), bundle->retained_bundle_id()),
               corrupted);
    EXPECT_THROW(
        static_cast<void>(
            FilesystemRetainedBundleRepository(root.path())),
        RetainedBundleError);
  }

  {
    ScopedTrustedRoot root;
    {
      FilesystemRetainedBundleRepository repository(root.path());
      EXPECT_TRUE(repository.put_if_absent(bundle));
    }
    EXPECT_THROW(
        static_cast<void>(FilesystemRetainedBundleRepository(
            root.path(), 1U,
            bundle->canonical_bytes().size() - 1U,
            bundle->canonical_bytes().size() - 1U)),
        RetainedBundleError);
  }
}

TEST(FilesystemRetentionStoreTest,
     ReferenceRuntimeProductDurablyReloadsAndPointReadsAsOneExactPair) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  ScopedTrustedRoot root;
  FilesystemBuildManifestRepository manifest_repository(
      root.path());
  FilesystemCatalogArtifactStore catalog_store(root.path());
  FilesystemRawArtifactStore raw_store(root.path());
  FilesystemRetainedBundleRepository bundle_repository(root.path());
  const test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);

  const auto bundle =
      detail::RetainedBundleMaterializer::
          materialize_store_and_reload_production(
              fixture.qualified, manifest_repository, input_source, {},
              catalog_store, raw_store, bundle_repository);
  ASSERT_NE(bundle, nullptr);
  EXPECT_EQ(manifest_repository.size(), 3U);
  EXPECT_EQ(catalog_store.size(), 1U);
  EXPECT_EQ(bundle_repository.size(), 1U);
  ASSERT_EQ(bundle->value().compiled_products.runtime_unit_products.size(),
            1U);
  const auto& group =
      bundle->value().compiled_products.runtime_unit_products.front();
  ASSERT_EQ(group.products.size(), 1U);
  const auto& product = group.products.front();

  FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto reloaded =
      bundle_source.get_verified(bundle->retained_bundle_id());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->canonical_bytes(), bundle->canonical_bytes());

  FilesystemVerifiedRetainedProductArtifactSource artifact_source(
      root.path());
  const auto manifest = artifact_source.get_verified({
      bundle->retained_bundle_id(), product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(std::string(
          detail::kReferenceRuntimeManifestArtifactRole)),
  });
  const auto launchable = artifact_source.get_verified({
      bundle->retained_bundle_id(), product.product_id,
      core::RetainedProductKind::kRuntimeUnitConfiguration,
      core::ArtifactRoleId(std::string(
          detail::kReferenceRuntimeLaunchableArtifactRole)),
  });
  ASSERT_TRUE(manifest.has_value());
  ASSERT_TRUE(launchable.has_value());
  EXPECT_EQ(launchable->bytes(), detail::reference_runtime_launchable_script());
  EXPECT_NO_THROW(
      detail::require_reference_runtime_product_matches_retained_bundle(
          manifest->bytes(), launchable->bytes(), reloaded->value(),
          group.runtime_unit_id));
  const auto verified_runtime = verify_reference_runtime_unit_product(
      *reloaded, group.runtime_unit_id, artifact_source);
  ASSERT_TRUE(verified_runtime.has_value());
  EXPECT_EQ(verified_runtime->retained_bundle_id(),
            bundle->retained_bundle_id());
  EXPECT_EQ(verified_runtime->runtime_unit_id(), group.runtime_unit_id);
  EXPECT_EQ(verified_runtime->product_id(), product.product_id);
  EXPECT_EQ(verified_runtime->configuration_identity(),
            manifest->artifact_ref().content_digest);
  EXPECT_EQ(verified_runtime->launchable_bytes(), launchable->bytes());
  EXPECT_EQ(verified_runtime->operating_system(), "linux");
  EXPECT_EQ(verified_runtime->shell_contract(), "posix.1-2017");
  EXPECT_EQ(verified_runtime->runtime_role_id(), "manager_fixture");
  EXPECT_EQ(verified_runtime->status_endpoint_role_id(), "runtime_status");
}

TEST(FilesystemRetentionStoreTest,
     NativeRuntimeProductsDurablyReloadAndVerifyPerExactRuntimeUnit) {
  ScopedTrustedRoot root;
  const auto materialized =
      materialize_durable_native_motion_lite6_bundle(root.path(), true);
  ASSERT_NE(materialized.bundle, nullptr);
  ASSERT_EQ(materialized.bundle->value()
                .compiled_products.runtime_unit_products.size(),
            2U);

  FilesystemVerifiedRetainedBundleSource bundle_source(root.path());
  const auto reloaded =
      bundle_source.get_verified(materialized.bundle->retained_bundle_id());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->canonical_bytes(),
            materialized.bundle->canonical_bytes());

  FilesystemVerifiedRetainedProductArtifactSource artifact_source(root.path());
  std::set<std::string> product_ids;
  std::set<std::string> configuration_identities;
  for (const auto &group :
       reloaded->value().compiled_products.runtime_unit_products) {
    ASSERT_EQ(group.products.size(), 1U);
    const auto verified = verify_runtime_unit_product(
        *reloaded, group.runtime_unit_id, artifact_source);
    ASSERT_TRUE(verified.has_value());
    EXPECT_EQ(verified->retained_bundle_id(), reloaded->retained_bundle_id());
    EXPECT_EQ(verified->runtime_unit_id(), group.runtime_unit_id);
    EXPECT_EQ(verified->product_id(), group.products.front().product_id);
    EXPECT_EQ(verified->product_contract_id().value(),
              detail::kNativeExecutionRuntimeProductContractId);
    EXPECT_EQ(verified->product_contract_revision(),
              detail::native_execution_runtime_product_contract_revision());
    EXPECT_EQ(verified->launchable_bytes(),
              detail::native_execution_runtime_launch_descriptor());
    EXPECT_EQ(verified->operating_system(), "linux");
    EXPECT_EQ(verified->launch_environment_contract(),
              detail::kNativeExecutionRuntimeLaunchEnvironmentContract);
    EXPECT_EQ(verified->runtime_role_id(),
              detail::kNativeExecutionRuntimeRoleId);
    EXPECT_EQ(verified->status_endpoint_role_id(),
              detail::kNativeExecutionRuntimeStatusEndpointRoleId);
    EXPECT_EQ(verified->status_transport(), "adapter_private");
    EXPECT_EQ(verified->process_adapter_contract_id(),
              detail::kNativeExecutionRuntimeProcessAdapterContractId);
    EXPECT_TRUE(product_ids.insert(verified->product_id().value()).second);
    EXPECT_TRUE(configuration_identities
                    .insert(verified->configuration_identity().value())
                    .second);
    EXPECT_THROW(static_cast<void>(verify_reference_runtime_unit_product(
                     *reloaded, group.runtime_unit_id, artifact_source)),
                 RetainedBundleError);
  }
}

} // namespace manyforge::deployment

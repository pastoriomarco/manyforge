#include <gtest/gtest.h>

#include <signal.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/content_identity.hpp>

#include "canonical_empty_admission_internal.hpp"
#include "content_identity_internal.hpp"

namespace manyforge::deployment {
namespace {

using namespace std::chrono_literals;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kFixedCellId = "c1_build_authority_cell";
constexpr std::string_view kFixedDeploymentArtifactId =
    "c1_build_authority";

static_assert(std::is_final_v<CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_aggregate_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_default_constructible_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_copy_constructible_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_copy_assignable_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_move_constructible_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_move_assignable_v<
              CanonicalEmptyRetainedBundleAdmission>);
static_assert(!std::is_constructible_v<
              CanonicalEmptyRetainedBundleAdmission,
              core::ContentDigest, core::CellId,
              core::DeploymentArtifactId,
              core::DeploymentRevision, core::GraphRevision,
              core::RetainedBundleId, core::ContentDigest,
              core::ContentDigest, core::ContentDigest,
              std::shared_ptr<const RetainedBundle>, std::string>);
static_assert(std::is_same_v<
              decltype(std::declval<
                       const ICanonicalEmptyRetainedBundleSource&>()
                           .load_verified()),
              CanonicalEmptyRetainedBundleAdmission>);

class ScopedTrustedRoot final {
 public:
  explicit ScopedTrustedRoot(std::string_view label)
      : path_(
            std::filesystem::temp_directory_path() /
            ("manyforge-canonical-empty-" + std::string(label) + "-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error(
          "test could not create canonical-empty trusted root");
    }
  }

  ~ScopedTrustedRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  ScopedTrustedRoot(const ScopedTrustedRoot&) = delete;
  ScopedTrustedRoot& operator=(const ScopedTrustedRoot&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

class ScopedRootSymlinkSubstitution final {
 public:
  explicit ScopedRootSymlinkSubstitution(
      const std::filesystem::path& root)
      : root_(root),
        backing_(
            root.parent_path() /
            (root.filename().string() + "-substituted")) {
    std::error_code error;
    (void)std::filesystem::remove_all(backing_, error);
    std::filesystem::rename(root_, backing_);
    std::filesystem::create_directory_symlink(backing_, root_);
    active_ = true;
  }

  ~ScopedRootSymlinkSubstitution() {
    if (!active_) {
      return;
    }
    std::error_code error;
    (void)std::filesystem::remove(root_, error);
    error.clear();
    std::filesystem::rename(backing_, root_, error);
  }

  ScopedRootSymlinkSubstitution(
      const ScopedRootSymlinkSubstitution&) = delete;
  ScopedRootSymlinkSubstitution& operator=(
      const ScopedRootSymlinkSubstitution&) = delete;

  [[nodiscard]] const std::filesystem::path& backing() const noexcept {
    return backing_;
  }

 private:
  std::filesystem::path root_;
  std::filesystem::path backing_;
  bool active_{false};
};

[[nodiscard]] std::string read_file(
    const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    throw std::logic_error("test could not open package file");
  }
  return std::string(
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>());
}

void write_file(const std::filesystem::path& path,
                std::string_view bytes) {
  std::ofstream stream(
      path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    throw std::logic_error("test could not open package file for write");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::logic_error("test could not write package file");
  }
}

[[nodiscard]] const CanonicalValue& member(
    const CanonicalValue& value, std::string_view name) {
  const CanonicalValue* found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::logic_error(
        "test package record has no member " + std::string(name));
  }
  return *found;
}

[[nodiscard]] const std::string& text_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<std::string>(member(value, name).storage());
}

[[nodiscard]] const CanonicalValue::Array& array_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<CanonicalValue::Array>(
      member(value, name).storage());
}

[[nodiscard]] CanonicalValue replace_member(
    const CanonicalValue& value, std::string_view name,
    CanonicalValue replacement) {
  CanonicalValue::Object result = value.as_object();
  const auto found = std::find_if(
      result.begin(), result.end(),
      [name](const auto& row) { return row.first == name; });
  if (found == result.end()) {
    throw std::logic_error("test replacement target is absent");
  }
  found->second = std::move(replacement);
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue add_member(
    const CanonicalValue& value, std::string name,
    CanonicalValue addition) {
  CanonicalValue::Object result = value.as_object();
  result.emplace_back(std::move(name), std::move(addition));
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue remove_member(
    const CanonicalValue& value, std::string_view name) {
  CanonicalValue::Object result = value.as_object();
  const auto found = std::find_if(
      result.begin(), result.end(),
      [name](const auto& row) { return row.first == name; });
  if (found == result.end()) {
    throw std::logic_error("test removal target is absent");
  }
  result.erase(found);
  return CanonicalValue(std::move(result));
}

[[nodiscard]] CanonicalValue reidentify_admission(
    CanonicalValue candidate) {
  candidate = replace_member(
      candidate, "admission_digest",
      CanonicalValue(std::string(kZeroDigest)));
  const std::string api_version =
      text_member(candidate, "api_version");
  const detail::ContentIdentitySpec spec{
      "canonical-empty-admission",
      api_version,
      "admission_digest",
  };
  const std::string digest =
      detail::compute_content_identity(candidate, spec);
  return replace_member(
      candidate, "admission_digest", CanonicalValue(digest));
}

[[nodiscard]] CanonicalValue parse_file(
    const std::filesystem::path& path) {
  return parse_strict_json_model_yaml(read_file(path)).root();
}

[[nodiscard]] std::filesystem::path admission_path(
    const std::filesystem::path& root) {
  return root / "canonical-empty-admission.json";
}

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& root, BuildManifestKind kind,
    std::string_view digest) {
  if (!is_sha256_digest(digest)) {
    throw std::logic_error("test manifest digest is malformed");
  }
  return root / "build-manifests" / std::string(to_string(kind)) /
         (std::string(digest.substr(7U)) + ".json");
}

[[nodiscard]] std::filesystem::path bundle_path(
    const std::filesystem::path& root, std::string_view digest) {
  if (!is_sha256_digest(digest)) {
    throw std::logic_error("test bundle digest is malformed");
  }
  return root / "retained-bundles" / "sha256" /
         (std::string(digest.substr(7U)) + ".json");
}

[[nodiscard]] std::vector<std::filesystem::path> paths_with_suffix(
    const std::filesystem::path& root, std::string_view suffix) {
  std::vector<std::filesystem::path> paths;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (entry.is_regular_file() &&
        entry.path().filename().string().ends_with(suffix)) {
      paths.push_back(entry.path());
    }
  }
  std::sort(paths.begin(), paths.end());
  return paths;
}

void clone_tree(const std::filesystem::path& source,
                const std::filesystem::path& destination) {
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(source)) {
    const auto relative = entry.path().lexically_relative(source);
    const auto target = destination / relative;
    if (entry.is_directory()) {
      std::filesystem::create_directory(target);
    } else if (entry.is_regular_file()) {
      std::filesystem::copy_file(
          entry.path(), target,
          std::filesystem::copy_options::none);
    } else {
      throw std::logic_error(
          "generated base package contains a non-plain entry");
    }
  }
}

struct TreeEntry final {
  std::string relative_path;
  mode_t mode{0U};
  std::uint64_t byte_size{0U};
  std::int64_t modified_seconds{0};
  std::int64_t modified_nanoseconds{0};
  std::int64_t changed_seconds{0};
  std::int64_t changed_nanoseconds{0};
  std::string content;

  friend bool operator==(const TreeEntry&, const TreeEntry&) = default;
};

[[nodiscard]] std::vector<TreeEntry> snapshot_tree(
    const std::filesystem::path& root, bool include_times) {
  std::vector<TreeEntry> result;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(root)) {
    struct stat status {};
    if (::lstat(entry.path().c_str(), &status) != 0) {
      throw std::logic_error("test could not lstat package entry");
    }
    std::string content;
    if (S_ISREG(status.st_mode)) {
      content = read_file(entry.path());
    } else if (S_ISLNK(status.st_mode)) {
      std::array<char, 4096U> target{};
      const ssize_t length = ::readlink(
          entry.path().c_str(), target.data(), target.size());
      if (length < 0) {
        throw std::logic_error("test could not read package symlink");
      }
      content.assign(target.data(), static_cast<std::size_t>(length));
    }
    result.push_back(TreeEntry{
        entry.path().lexically_relative(root).generic_string(),
        status.st_mode,
        static_cast<std::uint64_t>(status.st_size),
        include_times ? status.st_mtim.tv_sec : 0,
        include_times ? status.st_mtim.tv_nsec : 0,
        include_times ? status.st_ctim.tv_sec : 0,
        include_times ? status.st_ctim.tv_nsec : 0,
        std::move(content),
    });
  }
  std::sort(
      result.begin(), result.end(),
      [](const TreeEntry& left, const TreeEntry& right) {
        return left.relative_path < right.relative_path;
      });
  return result;
}

enum class ChildLoadResult : int {
  kExpectedAdmissionError = 0,
  kUnexpectedSuccess = 20,
  kWrongException = 21,
  kSignalOrTimeout = 22,
};

[[nodiscard]] ChildLoadResult load_in_bounded_child(
    const std::filesystem::path& root,
    std::chrono::milliseconds timeout = 2s) {
  const pid_t child = ::fork();
  if (child < 0) {
    throw std::runtime_error("test could not fork package loader");
  }
  if (child == 0) {
    try {
      const FilesystemCanonicalEmptyRetainedBundleSource source(root);
      static_cast<void>(source.load_verified());
      ::_exit(static_cast<int>(ChildLoadResult::kUnexpectedSuccess));
    } catch (const CanonicalEmptyAdmissionError&) {
      ::_exit(
          static_cast<int>(
              ChildLoadResult::kExpectedAdmissionError));
    } catch (...) {
      ::_exit(static_cast<int>(ChildLoadResult::kWrongException));
    }
  }

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const pid_t waited = ::waitpid(child, &status, WNOHANG);
    if (waited == child) {
      if (WIFEXITED(status)) {
        return static_cast<ChildLoadResult>(WEXITSTATUS(status));
      }
      return ChildLoadResult::kSignalOrTimeout;
    }
    if (waited < 0) {
      throw std::runtime_error("test could not wait for package loader");
    }
    std::this_thread::sleep_for(5ms);
  }
  (void)::kill(child, SIGKILL);
  (void)::waitpid(child, &status, 0);
  return ChildLoadResult::kSignalOrTimeout;
}

[[nodiscard]] bool load_succeeds_in_fresh_process(
    const std::filesystem::path& root) {
  const pid_t child = ::fork();
  if (child < 0) {
    throw std::runtime_error("test could not fork fresh package loader");
  }
  if (child == 0) {
    try {
      const FilesystemCanonicalEmptyRetainedBundleSource source(root);
      const auto admission = source.load_verified();
      const bool valid =
          admission.cell_id().value() == kFixedCellId &&
          admission.deployment_artifact_id().value() ==
              kFixedDeploymentArtifactId &&
          admission.retained_bundle_id() ==
              admission.retained_bundle().retained_bundle_id();
      ::_exit(valid ? 0 : 1);
    } catch (...) {
      ::_exit(2);
    }
  }
  int status = 0;
  if (::waitpid(child, &status, 0) != child) {
    throw std::runtime_error(
        "test could not wait for fresh package loader");
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

[[nodiscard]] bool write_all(
    int descriptor, std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const ssize_t written = ::write(
        descriptor, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      return false;
    }
    offset += static_cast<std::size_t>(written);
  }
  return true;
}

class CanonicalEmptyAdmissionPackageTest : public ::testing::Test {
 protected:
  static void SetUpTestSuite() {
    base_ = std::make_unique<ScopedTrustedRoot>("base");
    static_cast<void>(
        detail::generate_canonical_empty_package(base_->path()));
  }

  static void TearDownTestSuite() { base_.reset(); }

  [[nodiscard]] std::unique_ptr<ScopedTrustedRoot> fresh_copy(
      std::string_view label = "copy") const {
    auto copy = std::make_unique<ScopedTrustedRoot>(label);
    clone_tree(base_->path(), copy->path());
    return copy;
  }

  [[nodiscard]] const std::filesystem::path& base_path() const {
    return base_->path();
  }

 private:
  inline static std::unique_ptr<ScopedTrustedRoot> base_;
};

TEST_F(CanonicalEmptyAdmissionPackageTest,
       PublicSourceReturnsTheFixedNonNullableSealedProof) {
  const FilesystemCanonicalEmptyRetainedBundleSource source(base_path());
  const auto admission = source.load_verified();

  EXPECT_EQ(
      admission.api_version(), kCanonicalEmptyAdmissionApiVersion);
  EXPECT_EQ(admission.cell_id().value(), kFixedCellId);
  EXPECT_EQ(
      admission.deployment_artifact_id().value(),
      kFixedDeploymentArtifactId);
  EXPECT_EQ(
      admission.retained_bundle_id(),
      admission.retained_bundle().retained_bundle_id());
  EXPECT_EQ(
      admission.deployment_revision(),
      admission.retained_bundle().value().deployment_revision);
  EXPECT_EQ(
      admission.graph_revision(),
      admission.retained_bundle().value().cell_graph.graph_revision);
  EXPECT_EQ(
      admission.environment_digest(),
      admission.retained_bundle()
          .value()
          .catalogs.node_environment.environment_digest);
  EXPECT_EQ(
      admission.compiler_contract_digest(),
      admission.retained_bundle()
          .value()
          .provenance.compiler_contract_digest);
  EXPECT_EQ(
      admission.compiler_build_digest(),
      admission.retained_bundle()
          .value()
          .provenance.compiler_build_digest);
  EXPECT_TRUE(is_sha256_digest(admission.admission_digest().value()));

  const CanonicalValue record =
      parse_strict_json_model_yaml(admission.canonical_bytes()).root();
  EXPECT_EQ(record.as_object().size(), 10U);
  EXPECT_EQ(
      canonical_json(record), admission.canonical_bytes());
  EXPECT_EQ(
      text_member(record, "admission_digest"),
      admission.admission_digest().value());
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       GeneratedPackageHasTheExactClosedSemanticFilesAndEmptyShape) {
  const auto manifest_files =
      paths_with_suffix(base_path() / "build-manifests", ".json");
  const auto raw_files =
      paths_with_suffix(base_path() / "raw-artifacts", ".blob");
  const auto bundle_files =
      paths_with_suffix(base_path() / "retained-bundles", ".json");
  const auto lock_files = paths_with_suffix(base_path(), ".lock");
  const auto all_files = paths_with_suffix(base_path(), "");

  ASSERT_EQ(manifest_files.size(), 3U);
  ASSERT_EQ(raw_files.size(), 3U);
  ASSERT_EQ(bundle_files.size(), 1U);
  ASSERT_EQ(lock_files.size(), 3U);
  ASSERT_EQ(all_files.size(), 11U);
  ASSERT_TRUE(std::filesystem::is_regular_file(
      admission_path(base_path())));
  std::size_t directory_count = 0U;
  for (const auto& entry :
       std::filesystem::recursive_directory_iterator(base_path())) {
    if (entry.is_directory()) {
      ++directory_count;
    }
    EXPECT_EQ(
        entry.path().filename().string().find(".tmp"),
        std::string::npos);
  }
  EXPECT_EQ(directory_count, 8U);

  const FilesystemCanonicalEmptyRetainedBundleSource source(base_path());
  const auto admission = source.load_verified();
  const auto& bundle = admission.retained_bundle().value();
  EXPECT_EQ(bundle.deployment_artifact_id.value(),
            kFixedDeploymentArtifactId);
  EXPECT_EQ(bundle.cell_graph.cell_id.value(), kFixedCellId);
  EXPECT_TRUE(bundle.cell_graph.runtime_units.empty());
  EXPECT_TRUE(bundle.cell_graph.model_instances.empty());
  EXPECT_TRUE(bundle.cell_graph.controllable_components.empty());
  EXPECT_TRUE(bundle.cell_graph.control_domains.empty());
  EXPECT_TRUE(bundle.cell_graph.control_bindings.empty());
  EXPECT_TRUE(bundle.cell_graph.component_attachments.empty());
  EXPECT_TRUE(bundle.catalogs.records.empty());
  EXPECT_TRUE(bundle.catalogs.schema_artifacts.empty());
  EXPECT_TRUE(bundle.desired_control_configurations.empty());
  EXPECT_TRUE(bundle.capability_provider_bindings.empty());
  EXPECT_FALSE(bundle.compiled_products.viewer_model.has_value());
  EXPECT_TRUE(
      bundle.compiled_products.control_domain_products.empty());
  EXPECT_TRUE(bundle.compiled_products.symbol_role_maps.empty());
  EXPECT_TRUE(bundle.compiled_products.runtime_unit_products.empty());
  EXPECT_TRUE(bundle.provenance.source_records.empty());
  EXPECT_TRUE(bundle.provenance.generator_records.empty());
  ASSERT_EQ(bundle.provenance.output_records.size(), 1U);

  const auto& planning =
      bundle.compiled_products.planning_collision_model;
  EXPECT_EQ(
      planning.product_kind,
      core::RetainedProductKind::kPlanningCollisionModel);
  ASSERT_EQ(planning.artifact_refs.size(), 3U);
  EXPECT_EQ(planning.artifact_refs[0].artifact_role_id.value(),
            "planning_resource_map");
  EXPECT_EQ(planning.artifact_refs[1].artifact_role_id.value(), "srdf");
  EXPECT_EQ(planning.artifact_refs[2].artifact_role_id.value(), "urdf");
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       GenerationAndIndependentReconstructionAreByteExact) {
  ScopedTrustedRoot first("repro-a");
  ScopedTrustedRoot second("repro-b");
  static_cast<void>(
      detail::generate_canonical_empty_package(first.path()));
  static_cast<void>(
      detail::generate_canonical_empty_package(second.path()));

  EXPECT_EQ(
      snapshot_tree(first.path(), false),
      snapshot_tree(second.path(), false));
  EXPECT_TRUE(load_succeeds_in_fresh_process(first.path()));
  EXPECT_TRUE(load_succeeds_in_fresh_process(second.path()));
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       IdempotentGenerationAndRepeatedReadOnlyLoadDoNotMutateTheTree) {
  auto package = fresh_copy("idempotent");
  const auto before_generation =
      snapshot_tree(package->path(), true);
  static_cast<void>(
      detail::generate_canonical_empty_package(package->path()));
  EXPECT_EQ(
      snapshot_tree(package->path(), true), before_generation);

  const auto before_load = snapshot_tree(package->path(), true);
  const FilesystemCanonicalEmptyRetainedBundleSource source(
      package->path());
  const auto first = source.load_verified();
  const auto second = source.load_verified();
  EXPECT_EQ(first.admission_digest(), second.admission_digest());
  EXPECT_TRUE(load_succeeds_in_fresh_process(package->path()));
  EXPECT_EQ(snapshot_tree(package->path(), true), before_load);
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       ReadOnlyLoaderNeitherRequiresNorRecreatesRepositoryLocks) {
  auto package = fresh_copy("no-locks");
  const auto locks = paths_with_suffix(package->path(), ".lock");
  ASSERT_FALSE(locks.empty());
  for (const auto& lock : locks) {
    ASSERT_TRUE(std::filesystem::remove(lock));
  }
  const auto before = snapshot_tree(package->path(), true);

  const FilesystemCanonicalEmptyRetainedBundleSource source(
      package->path());
  const auto admission = source.load_verified();
  EXPECT_EQ(admission.cell_id().value(), kFixedCellId);
  EXPECT_EQ(snapshot_tree(package->path(), true), before);
  EXPECT_TRUE(paths_with_suffix(package->path(), ".lock").empty());
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       AdmissionRecordRejectsMalformedNoncanonicalOpenAndWrongIdentity) {
  {
    auto package = fresh_copy("record-malformed");
    write_file(admission_path(package->path()), "{");
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
  {
    auto package = fresh_copy("record-noncanonical");
    write_file(
        admission_path(package->path()),
        "\n" + read_file(admission_path(package->path())));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
  {
    auto package = fresh_copy("record-missing-field");
    const CanonicalValue candidate = remove_member(
        parse_file(admission_path(package->path())), "cell_id");
    write_file(
        admission_path(package->path()), canonical_json(candidate));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
  {
    auto package = fresh_copy("record-extra-field");
    const CanonicalValue candidate = reidentify_admission(add_member(
        parse_file(admission_path(package->path())),
        "selection_hint", CanonicalValue("latest")));
    write_file(
        admission_path(package->path()), canonical_json(candidate));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
  {
    auto package = fresh_copy("record-self-digest");
    const CanonicalValue current =
        parse_file(admission_path(package->path()));
    const CanonicalValue candidate = replace_member(
        current, "admission_digest",
        CanonicalValue(std::string(kZeroDigest)));
    write_file(
        admission_path(package->path()), canonical_json(candidate));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
  {
    auto package = fresh_copy("record-oversize");
    write_file(
        admission_path(package->path()),
        std::string(
            static_cast<std::size_t>(
                kMaxCanonicalEmptyAdmissionBytes + 1U),
            'x'));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       EveryAdmissionIdentityFieldRejectsARehashedSubstitution) {
  struct Substitution final {
    std::string_view field;
    std::string value;
  };
  const std::string all_f_digest =
      "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";
  const std::array substitutions{
      Substitution{"api_version",
                   "manyforge.canonical_empty_admission.v2"},
      Substitution{"cell_id", "caller_selected_cell"},
      Substitution{"deployment_artifact_id",
                   "caller_selected_deployment"},
      Substitution{"deployment_revision", all_f_digest},
      Substitution{"graph_revision", all_f_digest},
      Substitution{"retained_bundle_id", all_f_digest},
      Substitution{"environment_digest", all_f_digest},
      Substitution{"compiler_contract_digest", all_f_digest},
      Substitution{"compiler_build_digest", all_f_digest},
  };

  for (const auto& substitution : substitutions) {
    SCOPED_TRACE(substitution.field);
    auto package = fresh_copy("record-substitution");
    const CanonicalValue candidate = reidentify_admission(
        replace_member(
            parse_file(admission_path(package->path())),
            substitution.field,
            CanonicalValue(substitution.value)));
    write_file(
        admission_path(package->path()), canonical_json(candidate));
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       MissingOrCorruptSelectedManifestBundleAndRawClosureFailClosed) {
  const CanonicalValue record =
      parse_file(admission_path(base_path()));
  const std::array manifest_targets{
      manifest_path(
          base_path(), BuildManifestKind::kNodeEnvironment,
          text_member(record, "environment_digest")),
      manifest_path(
          base_path(), BuildManifestKind::kCompilerContract,
          text_member(record, "compiler_contract_digest")),
      manifest_path(
          base_path(), BuildManifestKind::kCompilerBuild,
          text_member(record, "compiler_build_digest")),
  };
  std::vector<std::filesystem::path> selected_targets(
      manifest_targets.begin(), manifest_targets.end());
  selected_targets.push_back(bundle_path(
      base_path(), text_member(record, "retained_bundle_id")));
  const auto raw_targets =
      paths_with_suffix(base_path() / "raw-artifacts", ".blob");
  ASSERT_EQ(raw_targets.size(), 3U);
  selected_targets.insert(
      selected_targets.end(), raw_targets.begin(), raw_targets.end());

  for (const auto& selected : selected_targets) {
    const auto relative =
        selected.lexically_relative(base_path());
    {
      SCOPED_TRACE(relative.generic_string() + " missing");
      auto package = fresh_copy("selected-missing");
      ASSERT_TRUE(std::filesystem::remove(package->path() / relative));
      EXPECT_EQ(
          load_in_bounded_child(package->path()),
          ChildLoadResult::kExpectedAdmissionError);
    }
    {
      SCOPED_TRACE(relative.generic_string() + " corrupt");
      auto package = fresh_copy("selected-corrupt");
      write_file(package->path() / relative, "{}");
      EXPECT_EQ(
          load_in_bounded_child(package->path()),
          ChildLoadResult::kExpectedAdmissionError);
    }
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       UnrelatedCorruptEntriesDoNotBlockAndAreNeverSelected) {
  auto package = fresh_copy("unrelated");
  constexpr std::string_view kUnrelatedHex =
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee";
  write_file(
      package->path() / "build-manifests" / "node_environment" /
          (std::string(kUnrelatedHex) + ".json"),
      "not a manifest");
  write_file(
      package->path() / "retained-bundles" / "sha256" /
          (std::string(kUnrelatedHex) + ".json"),
      "not a bundle");
  write_file(
      package->path() / "raw-artifacts" / "sha256" /
          (std::string(kUnrelatedHex) + ".blob"),
      "not the bytes named by this digest");

  const FilesystemCanonicalEmptyRetainedBundleSource source(
      package->path());
  const auto admission = source.load_verified();
  EXPECT_EQ(admission.cell_id().value(), kFixedCellId);

  const CanonicalValue wrong_selection = reidentify_admission(
      replace_member(
          parse_file(admission_path(package->path())),
          "retained_bundle_id",
          CanonicalValue(std::string(kZeroDigest))));
  write_file(
      admission_path(package->path()),
      canonical_json(wrong_selection));
  EXPECT_EQ(
      load_in_bounded_child(package->path()),
      ChildLoadResult::kExpectedAdmissionError);
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       AValidButHistoricalManifestCannotReplaceTheCurrentBuildManifest) {
  auto package = fresh_copy("historical-manifest");
  CanonicalValue admission =
      parse_file(admission_path(package->path()));
  const std::string current_digest =
      text_member(admission, "environment_digest");
  const auto current_path = manifest_path(
      package->path(), BuildManifestKind::kNodeEnvironment,
      current_digest);
  CanonicalValue historical = parse_file(current_path);
  CanonicalValue catalog = member(historical, "catalog");
  catalog = replace_member(
      catalog, "content_version", CanonicalValue("9.9.9"));
  historical = replace_member(
      historical, "catalog", std::move(catalog));
  historical = replace_member(
      historical, "environment_digest",
      CanonicalValue(std::string(kZeroDigest)));
  const detail::ContentIdentitySpec identity_spec{
      "node-environment-digest",
      "manyforge.node_environment_manifest.v1",
      "environment_digest",
  };
  const std::string historical_digest =
      detail::compute_content_identity(historical, identity_spec);
  historical = replace_member(
      historical, "environment_digest",
      CanonicalValue(historical_digest));

  write_file(
      manifest_path(
          package->path(), BuildManifestKind::kNodeEnvironment,
          historical_digest),
      canonical_json(historical));
  admission = reidentify_admission(replace_member(
      admission, "environment_digest",
      CanonicalValue(historical_digest)));
  write_file(
      admission_path(package->path()), canonical_json(admission));

  EXPECT_EQ(
      load_in_bounded_child(package->path()),
      ChildLoadResult::kExpectedAdmissionError);
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       SelectedFilesRejectMissingSymlinkDirectoryAndFifoWithoutMutation) {
  const CanonicalValue record =
      parse_file(admission_path(base_path()));
  std::vector<std::filesystem::path> selected{
      admission_path(base_path()),
      manifest_path(
          base_path(), BuildManifestKind::kNodeEnvironment,
          text_member(record, "environment_digest")),
      bundle_path(
          base_path(), text_member(record, "retained_bundle_id")),
  };
  const auto raw_targets =
      paths_with_suffix(base_path() / "raw-artifacts", ".blob");
  ASSERT_EQ(raw_targets.size(), 3U);
  selected.push_back(raw_targets.front());

  enum class Mutation : std::uint8_t {
    kMissing = 0,
    kSymlink,
    kDirectory,
    kFifo,
  };
  for (const auto& base_target : selected) {
    const auto relative =
        base_target.lexically_relative(base_path());
    for (const Mutation mutation :
         {Mutation::kMissing, Mutation::kSymlink,
          Mutation::kDirectory, Mutation::kFifo}) {
      SCOPED_TRACE(relative.generic_string());
      SCOPED_TRACE(static_cast<int>(mutation));
      auto package = fresh_copy("selected-type");
      const auto target = package->path() / relative;
      const auto backing_directory =
          package->path() / "mutation-backing";
      std::filesystem::create_directory(backing_directory);
      const auto backing =
          backing_directory / target.filename();

      if (mutation == Mutation::kMissing) {
        ASSERT_TRUE(std::filesystem::remove(target));
      } else if (mutation == Mutation::kSymlink) {
        std::filesystem::rename(target, backing);
        std::filesystem::create_symlink(backing, target);
      } else if (mutation == Mutation::kDirectory) {
        ASSERT_TRUE(std::filesystem::remove(target));
        std::filesystem::create_directory(target);
      } else {
        ASSERT_TRUE(std::filesystem::remove(target));
        ASSERT_EQ(::mkfifo(target.c_str(), 0600), 0);
      }

      const auto before = snapshot_tree(package->path(), true);
      EXPECT_EQ(
          load_in_bounded_child(package->path()),
          ChildLoadResult::kExpectedAdmissionError);
      EXPECT_EQ(snapshot_tree(package->path(), true), before);
    }
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       EveryOwnedDirectoryRejectsSymlinkSubstitutionWithoutMutation) {
  const std::array<std::filesystem::path, 8U> owned_directories{
      "build-manifests",
      "build-manifests/node_environment",
      "build-manifests/compiler_contract",
      "build-manifests/compiler_build",
      "raw-artifacts",
      "raw-artifacts/sha256",
      "retained-bundles",
      "retained-bundles/sha256",
  };

  for (std::size_t index = 0U;
       index < owned_directories.size(); ++index) {
    SCOPED_TRACE(owned_directories[index].generic_string());
    auto package = fresh_copy("directory-symlink");
    const auto selected =
        package->path() / owned_directories[index];
    const auto backing =
        package->path() /
        ("directory-backing-" + std::to_string(index));
    std::filesystem::rename(selected, backing);
    std::filesystem::create_directory_symlink(backing, selected);

    const auto before = snapshot_tree(package->path(), true);
    EXPECT_EQ(
        load_in_bounded_child(package->path()),
        ChildLoadResult::kExpectedAdmissionError);
    EXPECT_EQ(snapshot_tree(package->path(), true), before);
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       MissingCommitRecordLeavesAReadOnlyIncompletePackage) {
  auto package = fresh_copy("commit-last");
  ASSERT_TRUE(
      std::filesystem::remove(admission_path(package->path())));
  const auto before = snapshot_tree(package->path(), true);
  EXPECT_EQ(
      load_in_bounded_child(package->path()),
      ChildLoadResult::kExpectedAdmissionError);
  EXPECT_EQ(snapshot_tree(package->path(), true), before);
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       SourceRejectsTrustedRootReplacementAfterConstruction) {
  auto package = fresh_copy("root-replacement");
  const FilesystemCanonicalEmptyRetainedBundleSource source(
      package->path());
  {
    ScopedRootSymlinkSubstitution substitution(package->path());
    const auto before = snapshot_tree(substitution.backing(), true);
    EXPECT_THROW(
        static_cast<void>(source.load_verified()),
        CanonicalEmptyAdmissionError);
    EXPECT_EQ(snapshot_tree(substitution.backing(), true), before);
  }
  const auto admission = source.load_verified();
  EXPECT_EQ(admission.cell_id().value(), kFixedCellId);
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       GeneratorNeverRepairsAnExistingCorruptOrStaleCommitRecord) {
  for (const bool stale_but_self_identified : {false, true}) {
    SCOPED_TRACE(stale_but_self_identified);
    auto package = fresh_copy("producer-existing-record");
    if (stale_but_self_identified) {
      const CanonicalValue stale = reidentify_admission(
          replace_member(
              parse_file(admission_path(package->path())), "cell_id",
              CanonicalValue("stale_cell")));
      write_file(
          admission_path(package->path()), canonical_json(stale));
    } else {
      write_file(admission_path(package->path()), "{");
    }
    const auto before = snapshot_tree(package->path(), true);
    EXPECT_THROW(
        static_cast<void>(
            detail::generate_canonical_empty_package(package->path())),
        CanonicalEmptyAdmissionError);
    EXPECT_EQ(snapshot_tree(package->path(), true), before);
  }
}

TEST(CanonicalEmptyAdmissionGeneration,
     ConcurrentWritersConvergeBeforeTheCommitRecordIsTrusted) {
  ScopedTrustedRoot package("producer-race");
  int start_pipe[2]{-1, -1};
  ASSERT_EQ(::pipe(start_pipe), 0);
  std::array<pid_t, 2U> children{};
  for (auto& child : children) {
    child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0) {
      (void)::close(start_pipe[1]);
      char token = '\0';
      ssize_t count = 0;
      do {
        count = ::read(start_pipe[0], &token, 1U);
      } while (count < 0 && errno == EINTR);
      (void)::close(start_pipe[0]);
      if (count != 1) {
        ::_exit(31);
      }
      try {
        static_cast<void>(
            detail::generate_canonical_empty_package(
                package.path()));
        ::_exit(0);
      } catch (...) {
        ::_exit(32);
      }
    }
  }
  (void)::close(start_pipe[0]);
  const bool released = write_all(start_pipe[1], "ab");
  (void)::close(start_pipe[1]);
  if (!released) {
    for (const pid_t child : children) {
      (void)::kill(child, SIGKILL);
      int status = 0;
      (void)::waitpid(child, &status, 0);
    }
    FAIL() << "test could not release producer race";
  }

  for (const pid_t child : children) {
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
  const FilesystemCanonicalEmptyRetainedBundleSource source(
      package.path());
  const auto admission = source.load_verified();
  EXPECT_EQ(admission.cell_id().value(), kFixedCellId);
  EXPECT_TRUE(load_succeeds_in_fresh_process(package.path()));
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       CompilerBuildEvidenceCoversProductionAndExcludesOutputsAndTests) {
  const CanonicalValue admission =
      parse_file(admission_path(base_path()));
  const CanonicalValue build = parse_file(manifest_path(
      base_path(), BuildManifestKind::kCompilerBuild,
      text_member(admission, "compiler_build_digest")));

  std::vector<std::string> input_ids;
  for (const std::string_view table :
       {"source_inputs", "recipe_inputs"}) {
    for (const auto& row : array_member(build, table)) {
      input_ids.push_back(text_member(row, "input_id"));
    }
  }
  const auto covers = [&input_ids](std::string_view expected) {
    return std::find(
               input_ids.begin(), input_ids.end(), expected) !=
           input_ids.end();
  };

  EXPECT_TRUE(covers(
      "cpp-public-header:"
      "manyforge_deployment.canonical_empty_admission"));
  EXPECT_TRUE(covers(
      "cpp-internal-header:canonical_empty_admission_internal"));
  EXPECT_TRUE(covers(
      "cpp-translation-unit:canonical_empty_admission"));
  EXPECT_TRUE(covers(
      "compiler-tool:generate_canonical_empty_package"));
  EXPECT_TRUE(covers(
      "compiler-resource:"
      "canonical_empty.c1_build_authority.deployment.json"));
  EXPECT_TRUE(covers(
      "build-template:embedded_canonical_empty_deployment"));
  EXPECT_TRUE(covers("build-recipe:manyforge_deployment"));
  for (const auto& input_id : input_ids) {
    EXPECT_EQ(input_id.find("test"), std::string::npos)
        << input_id;
    EXPECT_EQ(input_id.find("golden"), std::string::npos)
        << input_id;
    EXPECT_EQ(
        input_id.find("canonical-empty-admission.json"),
        std::string::npos)
        << input_id;
    EXPECT_EQ(input_id.find("/tmp/"), std::string::npos)
        << input_id;
  }
}

TEST_F(CanonicalEmptyAdmissionPackageTest,
       ReleasePackageMatchesTheCheckedQualificationGolden) {
  const FilesystemCanonicalEmptyRetainedBundleSource source(base_path());
  const auto admission = source.load_verified();

#if defined(MANYFORGE_CANONICAL_EMPTY_RELEASE_GOLDEN_FILE)
  const CanonicalValue build = parse_file(manifest_path(
      base_path(), BuildManifestKind::kCompilerBuild,
      admission.compiler_build_digest().value()));
  ASSERT_EQ(text_member(build, "build_profile"), "Release")
      << "the checked canonical-empty golden is valid only for the "
         "explicit clean Release qualification gate";
  std::string golden =
      read_file(MANYFORGE_CANONICAL_EMPTY_RELEASE_GOLDEN_FILE);
  if (golden.ends_with('\n')) {
    golden.pop_back();
  }
  EXPECT_EQ(
      admission.canonical_bytes(), golden);
#else
  GTEST_SKIP()
      << "exact Release golden verification is disabled because the "
         "qualification gate was not enabled";
#endif
  RecordProperty(
      "canonical_empty_admission_digest",
      admission.admission_digest().value());
  RecordProperty(
      "canonical_empty_retained_bundle_id",
      admission.retained_bundle_id().value());
}

}  // namespace
}  // namespace manyforge::deployment

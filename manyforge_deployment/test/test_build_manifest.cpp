#include <gtest/gtest.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/deployment_artifact_codec.hpp>
#include <manyforge_deployment/retention_qualified_deployment.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <algorithm>
#include <array>
#include <barrier>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <unistd.h>

#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "model_source_contract_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr std::string_view kExpectedNodeEnvironmentDigest =
    "sha256:ad3e1dbcb9f15fdb55599da846a8da9bf4c80a0cdf784786363c484008c5375b";
constexpr std::string_view kExpectedCompilerContractDigest =
    "sha256:5af892e007d2f839aa9ce3f7a77a73feaf41e397c8f085f1f86afbed88e5e6d2";
constexpr std::string_view kExpectedReleaseCompilerBuildDigest =
    "sha256:db25065c1c0036509ffcb9bd2518022ff46a6e344354b3774500b4653ddee5ac";
constexpr std::string_view kExpectedReleaseCanonicalEmptyBundleId =
    "sha256:ad9f9d13f940a94db1f84d7c692229c58fe0dfe6cd2d385f6ffdb501290a164d";

static_assert(!std::is_default_constructible_v<BuildQualifiedManifest>);
static_assert(!std::is_aggregate_v<BuildQualifiedManifest>);
static_assert(!std::is_constructible_v<
              BuildQualifiedManifest, BuildManifestKey, std::string>);
static_assert(!std::is_default_constructible_v<
              detail::BuildQualifiedRetentionContext>);

[[nodiscard]] const CanonicalValue& member(
    const CanonicalValue& value, std::string_view name) {
  const CanonicalValue* found = value.find_object_member(name);
  if (found == nullptr) {
    throw std::logic_error("test manifest has no member " +
                           std::string(name));
  }
  return *found;
}

[[nodiscard]] const std::string& text_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<std::string>(member(value, name).storage());
}

[[nodiscard]] const CanonicalValue::Array& array_member(
    const CanonicalValue& value, std::string_view name) {
  return std::get<CanonicalValue::Array>(member(value, name).storage());
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

[[nodiscard]] detail::ContentIdentitySpec identity_spec(
    BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return {
          "node-environment-digest",
          std::string(detail::kNodeEnvironmentManifestApiVersion),
          "environment_digest",
      };
    case BuildManifestKind::kCompilerContract:
      return {
          "compiler-contract-digest",
          std::string(detail::kCompilerContractManifestApiVersion),
          "compiler_contract_digest",
      };
    case BuildManifestKind::kCompilerBuild:
      return {
          "compiler-build-digest",
          std::string(detail::kCompilerBuildManifestApiVersion),
          "compiler_build_digest",
      };
  }
  throw std::logic_error("unknown manifest kind");
}

[[nodiscard]] std::string_view own_digest_field(
    BuildManifestKind kind) {
  switch (kind) {
    case BuildManifestKind::kNodeEnvironment:
      return "environment_digest";
    case BuildManifestKind::kCompilerContract:
      return "compiler_contract_digest";
    case BuildManifestKind::kCompilerBuild:
      return "compiler_build_digest";
  }
  throw std::logic_error("unknown manifest kind");
}

[[nodiscard]] CanonicalValue reidentify(
    CanonicalValue candidate, BuildManifestKind kind) {
  candidate = replace_member(
      candidate, own_digest_field(kind),
      CanonicalValue(std::string(kZeroDigest)));
  const std::string digest =
      detail::compute_content_identity(candidate, identity_spec(kind));
  return replace_member(candidate, own_digest_field(kind),
                        CanonicalValue(digest));
}

[[nodiscard]] CanonicalValue replace_array(
    const CanonicalValue& root, std::string_view name,
    CanonicalValue::Array replacement, BuildManifestKind kind) {
  return reidentify(
      replace_member(root, name, CanonicalValue(std::move(replacement))),
      kind);
}

[[nodiscard]] std::string empty_graph() {
  return "{\"runtime_units\":[],\"model_instances\":[],"
         "\"controllable_components\":[],\"control_domains\":[],"
         "\"control_bindings\":[],\"component_attachments\":[]}";
}

[[nodiscard]] RetentionQualifiedDeployment qualify_empty() {
  const std::string document =
      "{\"api_version\":\"manyforge.deployment.v1\","
      "\"deployment_artifact_id\":\"c1_build_authority\","
      "\"deployment_revision\":\"" +
      std::string(kZeroDigest) +
      "\",\"cell_id\":\"c1_build_authority_cell\","
      "\"name\":\"C1 build authority\","
      "\"catalogs\":{\"records\":[]},"
      "\"graph\":" +
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
      deployment.applicability_inputs().value().external_resources, nullptr);
  const auto applicability = qualify_cell_applicability(
      composition, deployment.applicability_inputs(), {}, {}, {}, {}, {},
      {}, selected_overlay);
  return qualify_deployment_for_retention(
      deployment, compile_cell_ir(applicability));
}

class CountingRawArtifactStore final : public IRawArtifactStore {
 public:
  [[nodiscard]] StoredRawArtifact put_if_absent(
      std::string_view bytes) override {
    ++put_calls_;
    return store_.put_if_absent(bytes);
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& store_key,
      const core::ContentDigest& content_digest,
      std::uint64_t byte_size) const override {
    ++get_calls_;
    return store_.get_verified(store_key, content_digest, byte_size);
  }

  [[nodiscard]] std::size_t put_calls() const noexcept {
    return put_calls_;
  }
  [[nodiscard]] std::size_t get_calls() const noexcept {
    return get_calls_;
  }

 private:
  InMemoryRawArtifactStore store_;
  std::size_t put_calls_{0U};
  mutable std::size_t get_calls_{0U};
};

class CountingManifestRepository final : public IBuildManifestRepository {
 public:
  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override {
    ++put_calls_;
    return repository_.put_if_absent(std::move(manifest));
  }

  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override {
    ++get_calls_;
    return repository_.get_verified(key);
  }

  [[nodiscard]] std::size_t put_calls() const noexcept {
    return put_calls_;
  }
  [[nodiscard]] std::size_t get_calls() const noexcept {
    return get_calls_;
  }
  [[nodiscard]] std::size_t size() const noexcept {
    return repository_.size();
  }

 private:
  InMemoryBuildManifestRepository repository_;
  std::size_t put_calls_{0U};
  mutable std::size_t get_calls_{0U};
};

enum class ManifestReloadFault : std::uint8_t {
  kMissing = 0,
  kWrongManifest,
};

class FaultingManifestRepository final : public IBuildManifestRepository {
 public:
  explicit FaultingManifestRepository(ManifestReloadFault fault)
      : fault_(fault) {}

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override {
    retained_.push_back(manifest);
    return repository_.put_if_absent(std::move(manifest));
  }

  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override {
    if (fault_ == ManifestReloadFault::kMissing) {
      return nullptr;
    }
    if (retained_.empty()) {
      throw std::logic_error("faulting repository has no manifest");
    }
    return retained_.back()->key() == key && retained_.size() > 1U
               ? retained_.front()
               : retained_.back();
  }

 private:
  ManifestReloadFault fault_;
  InMemoryBuildManifestRepository repository_;
  std::vector<std::shared_ptr<const BuildQualifiedManifest>> retained_;
};

class SecondPutFailsManifestRepository final
    : public IBuildManifestRepository {
 public:
  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override {
    if (++put_calls_ == 2U) {
      throw BuildManifestError(
          "injected persistent-manifest write failure");
    }
    return repository_.put_if_absent(std::move(manifest));
  }

  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override {
    return repository_.get_verified(key);
  }

  [[nodiscard]] std::size_t size() const noexcept {
    return repository_.size();
  }

 private:
  InMemoryBuildManifestRepository repository_;
  std::size_t put_calls_{0U};
};

class ScopedRepositoryDirectory final {
 public:
  ScopedRepositoryDirectory()
      : path_(
            std::filesystem::temp_directory_path() /
            ("manyforge-build-manifest-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(sequence_++))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  ~ScopedRepositoryDirectory() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::uint64_t sequence_{0U};
  std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path manifest_path(
    const std::filesystem::path& root,
    const BuildManifestKey& key) {
  constexpr std::string_view kDigestPrefix = "sha256:";
  return root / std::string(to_string(key.manifest_kind)) /
         (key.content_digest.value().substr(kDigestPrefix.size()) +
          ".json");
}

void write_file(const std::filesystem::path& path,
                std::string_view bytes) {
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream.good()) {
    throw std::logic_error("test could not open manifest file");
  }
  stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
  if (!stream.good()) {
    throw std::logic_error("test could not write manifest file");
  }
}

TEST(BuildManifestAuthority,
     ProducesTheClosedThreeManifestClosureDeterministically) {
  const auto first = detail::BuildQualifiedRetentionAuthority::qualify();
  const auto second = detail::BuildQualifiedRetentionAuthority::qualify();

  ASSERT_EQ(first.manifests().size(), 3U);
  ASSERT_EQ(second.manifests().size(), first.manifests().size());
  const std::array expected_kinds{
      BuildManifestKind::kNodeEnvironment,
      BuildManifestKind::kCompilerContract,
      BuildManifestKind::kCompilerBuild,
  };
  for (std::size_t index = 0U; index < expected_kinds.size(); ++index) {
    const auto& lhs = first.manifests()[index];
    const auto& rhs = second.manifests()[index];
    ASSERT_NE(lhs, nullptr);
    ASSERT_NE(rhs, nullptr);
    EXPECT_EQ(lhs->key().manifest_kind, expected_kinds[index]);
    EXPECT_EQ(lhs->key(), rhs->key());
    EXPECT_EQ(lhs->canonical_bytes(), rhs->canonical_bytes());
    EXPECT_TRUE(is_sha256_digest(lhs->key().content_digest.value()));
    const auto parsed =
        parse_strict_json_model_yaml(lhs->canonical_bytes()).root();
    EXPECT_EQ(canonical_json(parsed), lhs->canonical_bytes());
  }
  EXPECT_EQ(first.node_environment(),
            second.node_environment());
  EXPECT_EQ(first.compiler_contract_digest(),
            first.manifests()[1]->key().content_digest);
  EXPECT_EQ(first.compiler_build_digest(),
            first.manifests()[2]->key().content_digest);
  RecordProperty(
      "node_environment_digest",
      first.node_environment().environment_digest.value());
  RecordProperty(
      "compiler_contract_digest",
      first.compiler_contract_digest().value());
  RecordProperty(
      "compiler_build_digest",
      first.compiler_build_digest().value());
  const auto build = parse_strict_json_model_yaml(
      first.manifests()[2]->canonical_bytes()).root();
  EXPECT_EQ(text_member(build, "compiler_contract_digest"),
            first.compiler_contract_digest().value());
  EXPECT_EQ(first.node_environment().environment_digest.value(),
            kExpectedNodeEnvironmentDigest);
  EXPECT_EQ(first.compiler_contract_digest().value(),
            kExpectedCompilerContractDigest);
  if (text_member(build, "build_profile") == "Release") {
    EXPECT_EQ(first.compiler_build_digest().value(),
              kExpectedReleaseCompilerBuildDigest);
  }
}

TEST(BuildManifestAuthority,
     SourceMediaManifestIsTheLiveClosedClassificationRegistry) {
  const auto contract =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  const CanonicalValue& source_contract =
      member(contract, "source_media_contract");
  const auto& rows = array_member(source_contract, "artifact_rows");
  ASSERT_EQ(rows.size(), detail::kModelSourceMediaContractRows.size());
  for (std::size_t index = 0U; index < rows.size(); ++index) {
    const auto& expected =
        detail::kModelSourceMediaContractRows[index];
    EXPECT_EQ(text_member(rows[index], "artifact_kind"),
              expected.artifact_kind_id);
    EXPECT_EQ(text_member(rows[index], "media_type"),
              expected.media_type);
    EXPECT_EQ(text_member(rows[index], "source_role"),
              expected.source_role);
    EXPECT_EQ(
        detail::find_model_source_media_contract(
            expected.artifact_kind),
        &expected);
    if (expected.source_role == "dependency") {
      EXPECT_EQ(
          detail::find_model_source_dependency_media_contract(
              expected.media_type),
          &expected);
    }
  }
  EXPECT_EQ(
      detail::find_model_source_dependency_media_contract(
          "application/octet-stream"),
      nullptr);
}

TEST(BuildManifestAuthority,
     CompilerContractExcludesRepositoryCapacityPolicy) {
  const auto contract =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  const auto& limits = array_member(contract, "resource_limits");
  std::vector<std::string> limit_ids;
  limit_ids.reserve(limits.size());
  for (const auto& row : limits) {
    limit_ids.push_back(text_member(row, "limit_id"));
  }
  for (const std::string_view storage_policy_id : {
           "build_manifest.manifest_count",
           "build_manifest.manifest_total_bytes",
           "retained_bundle.blob_count",
           "retained_bundle.blob_total_bytes",
           "retained_bundle.bundle_count",
           "retained_bundle.bundle_total_bytes",
       }) {
    EXPECT_EQ(std::find(limit_ids.begin(), limit_ids.end(),
                        storage_policy_id),
              limit_ids.end())
        << storage_policy_id;
  }

  const auto before =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  InMemoryBuildManifestRepository manifest_repository(
      1U, kMaxQualifiedManifestBytes / 2U);
  InMemoryRawArtifactStore raw_store(1U, 1024U, 1024U);
  InMemoryRetainedBundleRepository bundle_repository(1U, 1024U);
  (void)manifest_repository;
  (void)raw_store;
  (void)bundle_repository;
  const auto after =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  EXPECT_EQ(before, after);
}

TEST(BuildManifestAuthority,
     SemanticRepositoryReloadsAndRequalifiesIndependentValues) {
  const auto context = detail::BuildQualifiedRetentionAuthority::qualify();
  InMemoryBuildManifestRepository repository;
  std::uint64_t expected_bytes = 0U;
  for (const auto& manifest : context.manifests()) {
    expected_bytes +=
        static_cast<std::uint64_t>(manifest->canonical_bytes().size());
    EXPECT_TRUE(repository.put_if_absent(manifest));
    EXPECT_FALSE(repository.put_if_absent(manifest));
    const auto first = repository.get_verified(manifest->key());
    const auto second = repository.get_verified(manifest->key());
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_NE(first.get(), manifest.get());
    EXPECT_NE(first.get(), second.get());
    EXPECT_EQ(first->key(), manifest->key());
    EXPECT_EQ(first->canonical_bytes(), manifest->canonical_bytes());
  }
  EXPECT_EQ(repository.size(), 3U);
  EXPECT_EQ(repository.retained_bytes(), expected_bytes);
  EXPECT_EQ(repository.get_verified(
                BuildManifestKey{
                    BuildManifestKind::kCompilerBuild,
                    core::ContentDigest(std::string(kZeroDigest))}),
            nullptr);
}

TEST(BuildManifestAuthority,
     PersistentSemanticRepositorySurvivesFreshInstanceAndRequalifies) {
  const auto context = detail::BuildQualifiedRetentionAuthority::qualify();
  ScopedRepositoryDirectory directory;
  std::uint64_t expected_bytes = 0U;
  {
    FilesystemBuildManifestRepository repository(directory.path());
    for (const auto& manifest : context.manifests()) {
      expected_bytes +=
          static_cast<std::uint64_t>(manifest->canonical_bytes().size());
      EXPECT_TRUE(repository.put_if_absent(manifest));
    }
    EXPECT_EQ(repository.size(), 3U);
    EXPECT_EQ(repository.retained_bytes(), expected_bytes);
  }

  {
    FilesystemBuildManifestRepository reloaded_repository(
        directory.path());
    EXPECT_EQ(reloaded_repository.size(), 3U);
    EXPECT_EQ(reloaded_repository.retained_bytes(), expected_bytes);
    for (const auto& original : context.manifests()) {
      const auto reloaded =
          reloaded_repository.get_verified(original->key());
      ASSERT_NE(reloaded, nullptr);
      EXPECT_NE(reloaded.get(), original.get());
      EXPECT_EQ(reloaded->key(), original->key());
      EXPECT_EQ(reloaded->canonical_bytes(),
                original->canonical_bytes());
      EXPECT_FALSE(reloaded_repository.put_if_absent(original));
    }
  }
}

TEST(BuildManifestAuthority,
     PersistentRepositoryRefreshesSnapshotsAfterExternalWriters) {
  const auto context = detail::BuildQualifiedRetentionAuthority::qualify();
  ASSERT_EQ(context.manifests().size(), 3U);
  ScopedRepositoryDirectory directory;
  FilesystemBuildManifestRepository first(directory.path());
  FilesystemBuildManifestRepository second(directory.path());

  EXPECT_TRUE(first.put_if_absent(context.manifests()[0]));
  EXPECT_EQ(second.size(), 1U);
  EXPECT_EQ(
      second.retained_bytes(),
      static_cast<std::uint64_t>(
          context.manifests()[0]->canonical_bytes().size()));

  EXPECT_TRUE(second.put_if_absent(context.manifests()[1]));
  EXPECT_EQ(first.size(), 2U);
  EXPECT_EQ(
      first.retained_bytes(),
      static_cast<std::uint64_t>(
          context.manifests()[0]->canonical_bytes().size() +
          context.manifests()[1]->canonical_bytes().size()));
}

TEST(BuildManifestAuthority,
     PersistentRepositoryAcceptsConcurrentFirstCreation) {
  ScopedRepositoryDirectory directory;
  const auto root = directory.path() / "nested" / "repository";
  std::barrier<> start{3};
  std::array<std::exception_ptr, 2U> failures{};
  std::array<std::thread, 2U> workers;
  for (std::size_t index = 0U; index < workers.size(); ++index) {
    workers[index] = std::thread([&, index] {
      start.arrive_and_wait();
      try {
        FilesystemBuildManifestRepository repository(root);
        if (repository.size() != 0U) {
          throw std::logic_error(
              "new persistent repository was not empty");
        }
      } catch (...) {
        failures[index] = std::current_exception();
      }
    });
  }
  start.arrive_and_wait();
  for (auto& worker : workers) {
    worker.join();
  }
  for (const auto& failure : failures) {
    if (failure != nullptr) {
      std::rethrow_exception(failure);
    }
  }
  FilesystemBuildManifestRepository repository(root);
  EXPECT_EQ(repository.size(), 0U);
}

TEST(BuildManifestAuthority,
     PersistentRepositoryCleansBoundedStaleTemporaryFiles) {
  ScopedRepositoryDirectory directory;
  {
    FilesystemBuildManifestRepository repository(directory.path());
  }
  const auto kind_directory =
      directory.path() / "node_environment";
  const auto temporary =
      kind_directory /
      (".0000000000000000000000000000000000000000000000000000000000000000"
       ".json.tmp.1.0");
  {
    std::ofstream stream(temporary, std::ios::binary);
    ASSERT_TRUE(stream.good());
    stream << "stale";
  }
  ASSERT_TRUE(std::filesystem::exists(temporary));
  {
    FilesystemBuildManifestRepository repository(directory.path());
    EXPECT_EQ(repository.size(), 0U);
  }
  EXPECT_FALSE(std::filesystem::exists(temporary));

  for (std::size_t index = 0U; index < 5U; ++index) {
    const auto path =
        kind_directory /
        (".0000000000000000000000000000000000000000000000000000000000000000"
         ".json.tmp.2." +
         std::to_string(index));
    std::ofstream stream(path, std::ios::binary);
    ASSERT_TRUE(stream.good());
  }
  EXPECT_THROW(
      {
        FilesystemBuildManifestRepository repository(
            directory.path(), 1U, kMaxQualifiedManifestBytes);
      },
      BuildManifestError);
}

TEST(BuildManifestAuthority,
     PersistentRepositoryPreservesSelfVerifiedHistoricalBuildEvidence) {
  ScopedRepositoryDirectory directory;
  {
    FilesystemBuildManifestRepository repository(directory.path());
  }

  const CanonicalValue current =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerBuild);
  const CanonicalValue historical = reidentify(
      replace_member(current, "build_profile",
                     CanonicalValue(
                         std::string("historical-release"))),
      BuildManifestKind::kCompilerBuild);
  const auto self_verified =
      detail::reload_self_verified_build_manifest(
          BuildManifestKind::kCompilerBuild,
          canonical_json(historical));
  EXPECT_THROW(
      static_cast<void>(
          detail::BuildManifestTestAccess::qualify_candidate(
              BuildManifestKind::kCompilerBuild, historical)),
      BuildManifestError);
  write_file(manifest_path(directory.path(), self_verified->key()),
             self_verified->canonical_bytes());

  FilesystemBuildManifestRepository reopened(directory.path());
  EXPECT_EQ(reopened.size(), 1U);
  const auto reloaded =
      reopened.get_verified(self_verified->key());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->key(), self_verified->key());
  EXPECT_EQ(reloaded->canonical_bytes(),
            self_verified->canonical_bytes());

  ScopedRepositoryDirectory copied_directory;
  {
    FilesystemBuildManifestRepository copied(
        copied_directory.path());
    EXPECT_TRUE(copied.put_if_absent(reloaded));
  }
  {
    FilesystemBuildManifestRepository copied(
        copied_directory.path());
    const auto copied_historical =
        copied.get_verified(self_verified->key());
    ASSERT_NE(copied_historical, nullptr);
    EXPECT_EQ(copied_historical->canonical_bytes(),
              self_verified->canonical_bytes());
  }

  InMemoryRawArtifactStore raw_store;
  const auto bundle =
      detail::RetainedBundleMaterializer::materialize_production(
          qualify_empty(), reopened, raw_store);
  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  EXPECT_EQ(reopened.size(), 4U);
}

TEST(BuildManifestAuthority,
     PersistentRepositoryRejectsCorruptHistoricalBytesOnFreshOpen) {
  const auto context =
      detail::BuildQualifiedRetentionAuthority::qualify();
  ASSERT_FALSE(context.manifests().empty());
  ScopedRepositoryDirectory directory;
  {
    FilesystemBuildManifestRepository repository(directory.path());
    EXPECT_TRUE(
        repository.put_if_absent(context.manifests().front()));
  }

  std::string corrupted(
      context.manifests().front()->canonical_bytes());
  const auto catalog_id = corrupted.find("\"catalog_id\":\"");
  ASSERT_NE(catalog_id, std::string::npos);
  const auto value_offset =
      catalog_id + std::string_view("\"catalog_id\":\"").size();
  ASSERT_LT(value_offset, corrupted.size());
  corrupted[value_offset] =
      corrupted[value_offset] == 'a' ? 'b' : 'a';
  write_file(
      manifest_path(directory.path(),
                    context.manifests().front()->key()),
      corrupted);
  EXPECT_THROW(
      {
        FilesystemBuildManifestRepository reopened(
            directory.path());
      },
      BuildManifestError);
}

TEST(BuildManifestAuthority,
     ProductionMaterializationReloadsFromFreshPersistentRepository) {
  ScopedRepositoryDirectory directory;
  std::optional<core::RetainedBundleId> first_id;
  {
    FilesystemBuildManifestRepository repository(directory.path());
    InMemoryRawArtifactStore raw_store;
    const auto first =
        detail::RetainedBundleMaterializer::materialize_production(
            qualify_empty(), repository, raw_store);
    ASSERT_NE(first, nullptr);
    first_id = first->retained_bundle_id();
    EXPECT_EQ(repository.size(), 3U);
  }
  {
    FilesystemBuildManifestRepository reopened(directory.path());
    InMemoryRawArtifactStore raw_store;
    const auto second =
        detail::RetainedBundleMaterializer::materialize_production(
            qualify_empty(), reopened, raw_store);
    ASSERT_NE(second, nullptr);
    ASSERT_TRUE(first_id.has_value());
    EXPECT_EQ(second->retained_bundle_id(), *first_id);
    EXPECT_EQ(reopened.size(), 3U);
  }
}

TEST(BuildManifestAuthority,
     OwnDigestIsNonCircularAndEveryCoveredBuildInputIsMandatory) {
  const auto current = detail::BuildManifestTestAccess::current_candidate(
      BuildManifestKind::kCompilerBuild);
  const auto spec = identity_spec(BuildManifestKind::kCompilerBuild);
  const CanonicalValue zero = replace_member(
      current, "compiler_build_digest",
      CanonicalValue(std::string(kZeroDigest)));
  const CanonicalValue other = replace_member(
      current, "compiler_build_digest",
      CanonicalValue(
          "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
  EXPECT_EQ(detail::compute_content_identity(zero, spec),
            detail::compute_content_identity(other, spec));

  for (const std::string_view table_name :
       {"source_inputs", "recipe_inputs"}) {
    const auto& rows = array_member(current, table_name);
    ASSERT_FALSE(rows.empty());
    for (std::size_t index = 0U; index < rows.size(); ++index) {
      CanonicalValue::Array omitted = rows;
      omitted.erase(omitted.begin() + static_cast<std::ptrdiff_t>(index));
      const CanonicalValue stale = replace_array(
          current, table_name, std::move(omitted),
          BuildManifestKind::kCompilerBuild);
      EXPECT_THROW(
          static_cast<void>(
              detail::BuildManifestTestAccess::qualify_candidate(
                  BuildManifestKind::kCompilerBuild, stale)),
          BuildManifestError)
          << table_name << " row " << index;

      CanonicalValue::Array changed = rows;
      changed[index] = replace_member(
          changed[index], "content_digest",
          CanonicalValue(
              "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
      const CanonicalValue modified = replace_array(
          current, table_name, std::move(changed),
          BuildManifestKind::kCompilerBuild);
      EXPECT_THROW(
          static_cast<void>(
              detail::BuildManifestTestAccess::qualify_candidate(
                  BuildManifestKind::kCompilerBuild, modified)),
          BuildManifestError)
          << table_name << " row " << index;
    }
  }
}

TEST(BuildManifestAuthority,
     BuildEvidenceUsesStableNonGeneratedIdsAndSafeCanonicalIntegers) {
  const auto build = detail::BuildManifestTestAccess::current_candidate(
      BuildManifestKind::kCompilerBuild);
  const std::string build_bytes = canonical_json(build);
  for (const std::string_view forbidden :
       {"/home/", "/workspace/", "\"branch\"", "\"tag\"",
        "refs/heads/", ".git/", "embedded_build_evidence.hpp\""}) {
    EXPECT_EQ(build_bytes.find(forbidden), std::string::npos)
        << forbidden;
  }
  bool covers_node_environment_freshness_check = false;
  for (const std::string_view table_name :
       {"source_inputs", "recipe_inputs"}) {
    std::string previous;
    for (const auto& row : array_member(build, table_name)) {
      const std::string& input_id = text_member(row, "input_id");
      if (input_id ==
          "build-recipe:node-environment-manifest-freshness-check") {
        covers_node_environment_freshness_check = true;
      }
      EXPECT_FALSE(input_id.empty());
      EXPECT_EQ(input_id.find('/'), std::string::npos);
      EXPECT_EQ(input_id.find(".."), std::string::npos);
      EXPECT_TRUE(previous.empty() || previous < input_id);
      previous = input_id;
      EXPECT_TRUE(
          is_sha256_digest(text_member(row, "content_digest")));
    }
  }
  EXPECT_TRUE(covers_node_environment_freshness_check);
  EXPECT_TRUE(is_sha256_digest(
      text_member(build, "build_configuration_digest")));
  const CanonicalValue changed_configuration = reidentify(
      replace_member(
          build, "build_configuration_digest",
          CanonicalValue(
              "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff")),
      BuildManifestKind::kCompilerBuild);
  EXPECT_NE(text_member(changed_configuration, "compiler_build_digest"),
            text_member(build, "compiler_build_digest"));
  EXPECT_THROW(
      static_cast<void>(
          detail::BuildManifestTestAccess::qualify_candidate(
              BuildManifestKind::kCompilerBuild,
              changed_configuration)),
      BuildManifestError);

  const auto contract =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  const auto& limits = array_member(contract, "resource_limits");
  ASSERT_FALSE(limits.empty());
  for (const auto& row : limits) {
    const double value =
        std::get<double>(member(row, "value").storage());
    EXPECT_TRUE(std::isfinite(value));
    EXPECT_EQ(value, std::trunc(value));
    EXPECT_GE(value, 0.0);
    EXPECT_LE(value, 9007199254740991.0);
    const std::string row_bytes = canonical_json(row);
    EXPECT_EQ(row_bytes.find(".0"), std::string::npos);
    EXPECT_EQ(parse_strict_json_model_yaml(row_bytes).root(), row);
  }
}

TEST(BuildManifestAuthority,
     RejectsAllUnicodeControlCharactersIncludingC1Controls) {
  const auto current = detail::BuildManifestTestAccess::current_candidate(
      BuildManifestKind::kNodeEnvironment);
  CanonicalValue catalog = member(current, "catalog");
  catalog = replace_member(
      catalog, "content_version",
      CanonicalValue(std::string("1.0.0\xc2\x85", 7U)));
  const CanonicalValue candidate = reidentify(
      replace_member(current, "catalog", std::move(catalog)),
      BuildManifestKind::kNodeEnvironment);
  EXPECT_THROW(
      static_cast<void>(
          detail::BuildManifestTestAccess::qualify_candidate(
              BuildManifestKind::kNodeEnvironment, candidate)),
      BuildManifestError);
}

TEST(BuildManifestAuthority,
     StaleAuthorityRejectsBeforeManifestRepositoryOrRawCasEffects) {
  const auto node = detail::BuildManifestTestAccess::current_candidate(
      BuildManifestKind::kNodeEnvironment);
  const auto contract =
      detail::BuildManifestTestAccess::current_candidate(
          BuildManifestKind::kCompilerContract);
  const auto build = detail::BuildManifestTestAccess::current_candidate(
      BuildManifestKind::kCompilerBuild);
  CanonicalValue::Array sources = array_member(build, "source_inputs");
  ASSERT_FALSE(sources.empty());
  sources.erase(sources.begin());
  const CanonicalValue stale = replace_array(
      build, "source_inputs", std::move(sources),
      BuildManifestKind::kCompilerBuild);

  CountingManifestRepository manifest_repository;
  CountingRawArtifactStore raw_store;
  const auto qualified = qualify_empty();
  EXPECT_THROW(
      static_cast<void>(
          detail::BuildManifestTestAccess::materialize_with_candidates(
              qualified, node, contract, stale, manifest_repository,
              raw_store)),
      BuildManifestError);
  EXPECT_EQ(manifest_repository.put_calls(), 0U);
  EXPECT_EQ(manifest_repository.get_calls(), 0U);
  EXPECT_EQ(manifest_repository.size(), 0U);
  EXPECT_EQ(raw_store.put_calls(), 0U);
  EXPECT_EQ(raw_store.get_calls(), 0U);
}

TEST(BuildManifestAuthority,
     UnsupportedDeploymentRejectsBeforeAuthorityRepositoryOrRawCasEffects) {
  const auto fixture = test::make_runtime_only_retention_fixture();
  CountingManifestRepository manifest_repository;
  CountingRawArtifactStore raw_store;
  EXPECT_THROW(
      static_cast<void>(
          detail::RetainedBundleMaterializer::materialize_production(
              fixture.qualified, manifest_repository, raw_store)),
      RetainedBundleError);
  EXPECT_EQ(manifest_repository.put_calls(), 0U);
  EXPECT_EQ(manifest_repository.size(), 0U);
  EXPECT_EQ(raw_store.put_calls(), 0U);
}

TEST(BuildManifestAuthority,
     FailedPersistentManifestReloadRejectsBeforeRawCasEffects) {
  for (const ManifestReloadFault fault :
       {ManifestReloadFault::kMissing,
        ManifestReloadFault::kWrongManifest}) {
    FaultingManifestRepository manifest_repository(fault);
    CountingRawArtifactStore raw_store;
    EXPECT_THROW(
        static_cast<void>(
            detail::RetainedBundleMaterializer::materialize_production(
                qualify_empty(), manifest_repository, raw_store)),
        BuildManifestError);
    EXPECT_EQ(raw_store.put_calls(), 0U);
    EXPECT_EQ(raw_store.get_calls(), 0U);
  }

  SecondPutFailsManifestRepository partial_repository;
  CountingRawArtifactStore raw_store;
  EXPECT_THROW(
      static_cast<void>(
          detail::RetainedBundleMaterializer::materialize_production(
              qualify_empty(), partial_repository, raw_store)),
      BuildManifestError);
  // One immutable manifest may have been retained, but it grants no
  // production context and no semantic product/CAS effect occurred.
  EXPECT_EQ(partial_repository.size(), 1U);
  EXPECT_EQ(raw_store.put_calls(), 0U);
  EXPECT_EQ(raw_store.get_calls(), 0U);
}

TEST(BuildManifestAuthority,
     CanonicalEmptyProductionPathRetainsExactAuthorityBeforeProducts) {
  CountingManifestRepository manifest_repository;
  CountingRawArtifactStore raw_store;
  const auto bundle =
      detail::RetainedBundleMaterializer::materialize_production(
          qualify_empty(), manifest_repository, raw_store);
  ASSERT_NE(bundle, nullptr);
  EXPECT_TRUE(verify_retained_bundle_identity(*bundle));
  RecordProperty(
      "canonical_empty_retained_bundle_id",
      bundle->retained_bundle_id().value());
  EXPECT_EQ(manifest_repository.put_calls(), 3U);
  EXPECT_EQ(manifest_repository.get_calls(), 3U);
  EXPECT_EQ(manifest_repository.size(), 3U);
  EXPECT_EQ(raw_store.put_calls(), 2U);
  EXPECT_EQ(raw_store.get_calls(), 2U);

  InMemoryRetainedBundleRepository bundle_repository;
  EXPECT_TRUE(bundle_repository.put_if_absent(bundle));
  const auto reloaded =
      bundle_repository.get_verified(bundle->retained_bundle_id());
  ASSERT_NE(reloaded, nullptr);
  EXPECT_EQ(reloaded->retained_bundle_id(), bundle->retained_bundle_id());
  EXPECT_EQ(reloaded->canonical_bytes(), bundle->canonical_bytes());
  EXPECT_TRUE(verify_retained_bundle_identity(*reloaded));

  const auto context = detail::BuildQualifiedRetentionAuthority::qualify();
  EXPECT_EQ(bundle->value().catalogs.node_environment.environment_digest,
            context.node_environment().environment_digest);
  EXPECT_EQ(bundle->value().provenance.compiler_contract_digest,
            context.compiler_contract_digest());
  EXPECT_EQ(bundle->value().provenance.compiler_build_digest,
            context.compiler_build_digest());
  const auto build = parse_strict_json_model_yaml(
      context.manifests()[2]->canonical_bytes()).root();
  if (text_member(build, "build_profile") == "Release") {
    EXPECT_EQ(bundle->retained_bundle_id().value(),
              kExpectedReleaseCanonicalEmptyBundleId);
  }
}

TEST(BuildManifestAuthority,
     RepositoryBoundsAreAppliedToCanonicalSemanticBytes) {
  const auto context = detail::BuildQualifiedRetentionAuthority::qualify();
  ASSERT_EQ(context.manifests().size(), 3U);
  const auto& first = context.manifests().front();
  InMemoryBuildManifestRepository byte_bounded(
      3U, static_cast<std::uint64_t>(first->canonical_bytes().size() - 1U));
  EXPECT_THROW(
      static_cast<void>(byte_bounded.put_if_absent(first)),
      BuildManifestError);
  EXPECT_EQ(byte_bounded.size(), 0U);

  InMemoryBuildManifestRepository count_bounded(
      1U, kMaxQualifiedManifestBytes);
  EXPECT_TRUE(count_bounded.put_if_absent(first));
  EXPECT_THROW(
      static_cast<void>(
          count_bounded.put_if_absent(context.manifests()[1])),
      BuildManifestError);
  EXPECT_EQ(count_bounded.size(), 1U);
}

}  // namespace
}  // namespace manyforge::deployment

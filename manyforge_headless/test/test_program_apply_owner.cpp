#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <unistd.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_headless/program_apply_owner.hpp>

#include "content_identity_internal.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
namespace deployment = manyforge::deployment;

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

enum class CompatibilityChange : std::uint8_t {
  kNone = 0,
  kAdmissible,
  kWrongLane,
  kMissingRequirement,
};

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-program-apply-owner-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("Program Apply test mkdtemp failed");
    }
    root_ = created;
    std::filesystem::create_directory(publications());
    std::filesystem::copy(MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT,
                          package(), std::filesystem::copy_options::recursive);
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path package() const {
    return root_ / "package";
  }
  [[nodiscard]] std::filesystem::path publications() const {
    return root_ / "publications";
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path root_;
};

class ScopedBlockedDirectory final {
public:
  explicit ScopedBlockedDirectory(std::filesystem::path path)
      : path_(std::move(path)), backup_(path_.string() + ".blocked-backup") {
    std::filesystem::rename(path_, backup_);
    std::ofstream marker(path_, std::ios::binary | std::ios::trunc);
    if (!marker) {
      throw std::runtime_error("Program Apply test cannot block directory");
    }
    marker << "blocked\n";
    marker.close();
    if (!marker) {
      throw std::runtime_error("Program Apply test cannot seal block marker");
    }
  }

  ~ScopedBlockedDirectory() {
    std::error_code error;
    std::filesystem::remove(path_, error);
    error.clear();
    std::filesystem::rename(backup_, path_, error);
  }

  ScopedBlockedDirectory(const ScopedBlockedDirectory &) = delete;
  ScopedBlockedDirectory &operator=(const ScopedBlockedDirectory &) = delete;

private:
  std::filesystem::path path_;
  std::filesystem::path backup_;
};

class ScopedReadOnlyTree final {
public:
  explicit ScopedReadOnlyTree(const std::filesystem::path &root) {
    paths_.emplace_back(root, std::filesystem::status(root).permissions());
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             root,
             std::filesystem::directory_options::skip_permission_denied)) {
      if (!entry.is_symlink()) {
        paths_.emplace_back(
            entry.path(), std::filesystem::status(entry.path()).permissions());
      }
    }
    for (const auto &[path, permissions] : paths_) {
      std::filesystem::permissions(
          path, permissions &
                    ~(std::filesystem::perms::owner_write |
                      std::filesystem::perms::group_write |
                      std::filesystem::perms::others_write),
          std::filesystem::perm_options::replace);
    }
  }

  ~ScopedReadOnlyTree() {
    for (auto it = paths_.rbegin(); it != paths_.rend(); ++it) {
      std::error_code error;
      std::filesystem::permissions(it->first, it->second,
                                   std::filesystem::perm_options::replace,
                                   error);
    }
  }

  ScopedReadOnlyTree(const ScopedReadOnlyTree &) = delete;
  ScopedReadOnlyTree &operator=(const ScopedReadOnlyTree &) = delete;

private:
  std::vector<std::pair<std::filesystem::path, std::filesystem::perms>> paths_;
};

[[nodiscard]] std::map<std::string, std::string>
PackageFiles(const std::filesystem::path &root) {
  std::map<std::string, std::string> result;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(root)) {
    if (!entry.is_regular_file() || entry.is_symlink()) {
      continue;
    }
    std::ifstream stream(entry.path(), std::ios::binary);
    if (!stream) {
      throw std::runtime_error("Program Apply test cannot read package file");
    }
    result.emplace(
        std::filesystem::relative(entry.path(), root).generic_string(),
        std::string(std::istreambuf_iterator<char>(stream),
                    std::istreambuf_iterator<char>()));
  }
  return result;
}

[[nodiscard]] std::filesystem::path RepositoryEntry(
    const std::filesystem::path &root, const std::string_view directory,
    const std::string_view digest) {
  if (!digest.starts_with("sha256:")) {
    throw std::runtime_error("Program Apply test received a non-digest ID");
  }
  return root / directory / "sha256" /
         (std::string(digest.substr(7U)) + ".json");
}

[[nodiscard]] const CanonicalValue &Member(const CanonicalValue &root,
                                           const std::string_view name) {
  const auto *value = root.find_object_member(name);
  if (value == nullptr) {
    throw std::runtime_error("missing Program Apply test field");
  }
  return *value;
}

[[nodiscard]] std::string Text(const CanonicalValue &root,
                               const std::string_view name) {
  return std::get<std::string>(Member(root, name).storage());
}

[[nodiscard]] std::string
CandidateProgram(const deployment::ScenarioProgramSelection &selection,
                 const std::string_view name,
                 const CompatibilityChange compatibility_change =
                     CompatibilityChange::kNone) {
  auto members = selection.normalized_program_root.as_object();
  for (auto &[field, value] : members) {
    if (field == "name") {
      value = CanonicalValue(std::string(name));
    } else if (field == "program_revision") {
      value = CanonicalValue(std::string(kZeroDigest));
    }
  }
  if (compatibility_change != CompatibilityChange::kNone) {
    CanonicalValue::Array lanes;
    CanonicalValue::Array requirements;
    if (compatibility_change == CompatibilityChange::kWrongLane) {
      lanes.emplace_back("isaac-sim");
    }
    if (compatibility_change == CompatibilityChange::kMissingRequirement) {
      requirements.emplace_back("scene.mutable");
    }
    members.emplace_back(
        "compatibility",
        CanonicalValue(CanonicalValue::Object{
            {"assistant", CanonicalValue("optional")},
            {"lanes", CanonicalValue(std::move(lanes))},
            {"requires", CanonicalValue(std::move(requirements))},
            {"tags", CanonicalValue(CanonicalValue::Array{
                         CanonicalValue("edited-program-metadata")})},
        }));
  }
  CanonicalValue root(std::move(members));
  const auto revision = deployment::detail::compute_content_identity(
      root, deployment::detail::ContentIdentitySpec{
                .identity_domain = "program-revision",
                .api_version = "manyforge.program.v1",
                .own_field = "program_revision",
            });
  members = root.as_object();
  for (auto &[field, value] : members) {
    if (field == "program_revision") {
      value = CanonicalValue(revision);
    }
  }
  return content_identity::canonical_json(CanonicalValue(std::move(members)));
}

[[nodiscard]] deployment::ScenarioProgramSelection
Baseline(const std::filesystem::path &package) {
  return deployment::FilesystemScenarioApplyCandidateSource(package,
                                                            "maintained")
      .LoadExactProgramSelection("program_apply_fixture");
}

TEST(ProgramApplyOwnerTest,
     PreparesCommitsReplaysAndReopensOneExactAppliedSelection) {
  TemporaryDirectory directory;
  const auto baseline = Baseline(directory.package());
  const auto package_before = PackageFiles(directory.package());
  ProgramApplyOwner owner(directory.package(), directory.publications(),
                          "headless-api.test.program-selection", "epoch.1");
  CanonicalValue replay;
  std::string preparation_bytes;
  {
    ScopedReadOnlyTree package_read_only(directory.package());
    const auto prepared = owner.Prepare(
        baseline.scenario_id, baseline.scenario_catalog_revision.value(),
        CandidateProgram(baseline, "Changed Program"));

    EXPECT_EQ(Text(prepared, "schema_version"),
              kProgramApplyPreparationSchemaVersion);
    EXPECT_EQ(Text(prepared, "baseline_program_binding_id"),
              baseline.program_binding_id.value());
    EXPECT_NE(Text(prepared, "program_revision"),
              baseline.program_revision.value());
    EXPECT_NE(Text(prepared, "program_binding_id"),
              baseline.program_binding_id.value());

    preparation_bytes = content_identity::canonical_json(prepared);
    const auto applied = owner.Commit(preparation_bytes, std::nullopt,
                                      baseline.retained_bundle_id.value(),
                                      baseline.graph_revision.value(), 10);
    EXPECT_EQ(Text(applied, "schema_version"),
              kAppliedProgramSelectionSchemaVersion);
    EXPECT_EQ(Text(applied, "program_revision"),
              Text(prepared, "program_revision"));
    EXPECT_EQ(Text(applied, "program_binding_id"),
              Text(prepared, "program_binding_id"));
    EXPECT_TRUE(std::filesystem::exists(RepositoryEntry(
        directory.publications(), "programs",
        Text(prepared, "program_revision"))));
    EXPECT_TRUE(std::filesystem::exists(RepositoryEntry(
        directory.publications(), "program-bindings",
        Text(prepared, "program_binding_id"))));
    EXPECT_FALSE(std::filesystem::exists(
        RepositoryEntry(directory.package(), "programs",
                        Text(prepared, "program_revision"))));
    EXPECT_FALSE(std::filesystem::exists(
        RepositoryEntry(directory.package(), "program-bindings",
                        Text(prepared, "program_binding_id"))));
    const auto &source = Member(applied, "source");
    const auto first_record = Text(source, "record_digest");

    replay = owner.Commit(preparation_bytes, std::nullopt,
                          baseline.retained_bundle_id.value(),
                          baseline.graph_revision.value(), 11);
    EXPECT_EQ(Text(Member(replay, "source"), "record_digest"), first_record);
    EXPECT_EQ(owner.ReadApplied(), replay);
    EXPECT_EQ(PackageFiles(directory.package()), package_before);
  }

  ASSERT_TRUE(std::filesystem::remove(directory.package() /
                                      "maintained.scenario-catalog.yaml"));
  const auto package_after_catalog_removal = PackageFiles(directory.package());
  ProgramApplyOwner restarted(directory.package(), directory.publications(),
                              "headless-api.test.program-selection", "epoch.2");
  EXPECT_EQ(restarted.ReadApplied(), replay);
  EXPECT_EQ(restarted.Commit(preparation_bytes, std::nullopt,
                             baseline.retained_bundle_id.value(),
                             baseline.graph_revision.value(), 12),
            replay);
  EXPECT_EQ(PackageFiles(directory.package()), package_after_catalog_removal);
}

TEST(ProgramApplyOwnerTest,
     ValidationActiveSelectionAndCasFailuresPreservePriorAppliedProgram) {
  TemporaryDirectory directory;
  const auto baseline = Baseline(directory.package());
  ProgramApplyOwner owner(directory.package(), directory.publications(),
                          "headless-api.test.program-selection", "epoch.1");
  const auto first = owner.Prepare(baseline.scenario_id,
                                   baseline.scenario_catalog_revision.value(),
                                   CandidateProgram(baseline, "First Program"));
  const auto first_applied = owner.Commit(
      content_identity::canonical_json(first), std::nullopt,
      baseline.retained_bundle_id.value(), baseline.graph_revision.value(), 10);
  const auto first_record =
      Text(Member(first_applied, "source"), "record_digest");

  EXPECT_THROW(owner.Prepare(baseline.scenario_id,
                             baseline.scenario_catalog_revision.value(), "{}"),
               ProgramApplyError);
  EXPECT_EQ(Text(Member(owner.ReadApplied(), "source"), "record_digest"),
            first_record);

  const auto second = owner.Prepare(
      baseline.scenario_id, baseline.scenario_catalog_revision.value(),
      CandidateProgram(baseline, "Second Program"));
  EXPECT_THROW(owner.Commit(content_identity::canonical_json(second),
                            first_record, "sha256:" + std::string(64U, 'f'),
                            baseline.graph_revision.value(), 20),
               ProgramApplyError);
  EXPECT_EQ(Text(Member(owner.ReadApplied(), "source"), "record_digest"),
            first_record);

  const auto second_applied = owner.Commit(
      content_identity::canonical_json(second), first_record,
      baseline.retained_bundle_id.value(), baseline.graph_revision.value(), 21);
  const auto second_record =
      Text(Member(second_applied, "source"), "record_digest");
  EXPECT_NE(second_record, first_record);

  const auto third = owner.Prepare(baseline.scenario_id,
                                   baseline.scenario_catalog_revision.value(),
                                   CandidateProgram(baseline, "Third Program"));
  EXPECT_THROW(owner.Commit(content_identity::canonical_json(third),
                            first_record, baseline.retained_bundle_id.value(),
                            baseline.graph_revision.value(), 30),
               ProgramApplyError);
  EXPECT_EQ(Text(Member(owner.ReadApplied(), "source"), "record_digest"),
            second_record);
}

TEST(ProgramApplyOwnerTest,
     ProgramOnlyApplyAdmitsEditedCompatibleMetadataAndRejectsMismatch) {
  TemporaryDirectory directory;
  const auto baseline = Baseline(directory.package());
  ProgramApplyOwner owner(directory.package(), directory.publications(),
                          "headless-api.test.program-selection", "epoch.1");
  const auto admissible = owner.Prepare(
      baseline.scenario_id, baseline.scenario_catalog_revision.value(),
      CandidateProgram(baseline, "Changed compatible metadata",
                       CompatibilityChange::kAdmissible));
  EXPECT_NE(Text(admissible, "program_revision"),
            baseline.program_revision.value());

  for (const auto mismatch : {CompatibilityChange::kWrongLane,
                              CompatibilityChange::kMissingRequirement}) {
    try {
      static_cast<void>(owner.Prepare(
          baseline.scenario_id, baseline.scenario_catalog_revision.value(),
          CandidateProgram(baseline, "Incompatible metadata", mismatch)));
      FAIL() << "Program Apply accepted deployment-incompatible metadata";
    } catch (const ProgramApplyError &error) {
      EXPECT_EQ(error.code(), "program_apply_compatibility_invalid");
    }
  }
}

TEST(ProgramApplyOwnerTest,
     RepositoryAndPublicationFailuresPreservePriorAppliedSelection) {
  TemporaryDirectory directory;
  const auto baseline = Baseline(directory.package());
  ProgramApplyOwner owner(directory.package(), directory.publications(),
                          "headless-api.test.program-selection", "epoch.1");
  const auto first = owner.Prepare(
      baseline.scenario_id, baseline.scenario_catalog_revision.value(),
      CandidateProgram(baseline, "Prior Applied Program"));
  const auto first_applied = owner.Commit(
      content_identity::canonical_json(first), std::nullopt,
      baseline.retained_bundle_id.value(), baseline.graph_revision.value(), 10);
  const auto first_record =
      Text(Member(first_applied, "source"), "record_digest");

  {
    ScopedBlockedDirectory blocked_program_repository(
        directory.publications() / "programs" / "sha256");
    try {
      static_cast<void>(owner.Prepare(
          baseline.scenario_id, baseline.scenario_catalog_revision.value(),
          CandidateProgram(baseline, "Repository Failure Program")));
      FAIL() << "Program Apply prepared through a blocked repository";
    } catch (const ProgramApplyError &error) {
      EXPECT_EQ(error.code(), "program_apply_preparation_failed");
    }
  }
  EXPECT_EQ(Text(Member(owner.ReadApplied(), "source"), "record_digest"),
            first_record);

  const auto second = owner.Prepare(
      baseline.scenario_id, baseline.scenario_catalog_revision.value(),
      CandidateProgram(baseline, "Publication Failure Program"));
  {
    ScopedBlockedDirectory blocked_publication(directory.publications());
    try {
      static_cast<void>(owner.Commit(content_identity::canonical_json(second),
                                     first_record,
                                     baseline.retained_bundle_id.value(),
                                     baseline.graph_revision.value(), 20));
      FAIL() << "Program Apply committed through a blocked publication root";
    } catch (const ProgramApplyError &error) {
      EXPECT_EQ(error.code(), "program_apply_commit_failed");
    }
  }
  EXPECT_EQ(Text(Member(owner.ReadApplied(), "source"), "record_digest"),
            first_record);
}

TEST(ProgramApplyOwnerTest,
     MissingOrMalformedPackageBaselineNeverFallsBackToCandidateRepository) {
  for (const bool malformed : {false, true}) {
    TemporaryDirectory directory;
    const auto baseline = Baseline(directory.package());
    const auto package_binding = RepositoryEntry(
        directory.package(), "program-bindings",
        baseline.program_binding_id.value());
    const auto candidate_binding = RepositoryEntry(
        directory.publications(), "program-bindings",
        baseline.program_binding_id.value());
    std::filesystem::create_directories(candidate_binding.parent_path());
    ASSERT_TRUE(std::filesystem::copy_file(
        package_binding, candidate_binding,
        std::filesystem::copy_options::overwrite_existing));
    if (malformed) {
      std::ofstream stream(package_binding, std::ios::binary | std::ios::trunc);
      ASSERT_TRUE(stream);
      stream << "{malformed";
      ASSERT_TRUE(stream);
    } else {
      ASSERT_TRUE(std::filesystem::remove(package_binding));
    }
    const auto package_before = PackageFiles(directory.package());
    ProgramApplyOwner owner(directory.package(), directory.publications(),
                            "headless-api.test.program-selection", "epoch.1");
    try {
      static_cast<void>(owner.Prepare(
          baseline.scenario_id, baseline.scenario_catalog_revision.value(),
          CandidateProgram(baseline, "Baseline source must be package-only")));
      FAIL() << "Program Apply accepted an unavailable package baseline";
    } catch (const ProgramApplyError &error) {
      EXPECT_EQ(error.code(), "program_apply_preparation_failed");
    }
    EXPECT_EQ(PackageFiles(directory.package()), package_before);
  }
}

} // namespace
} // namespace manyforge::headless

#include <gtest/gtest.h>

#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <unistd.h>

namespace manyforge::deployment {
namespace {

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-scenario-catalog-" +
             std::to_string(static_cast<long long>(::getpid())) + "-" +
             std::to_string(sequence_++));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() {
    std::error_code ignored;
    (void)std::filesystem::remove_all(path_, ignored);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  inline static std::size_t sequence_{};
  std::filesystem::path path_;
};

[[nodiscard]] std::string digest(char digit) {
  return "sha256:" + std::string(64U, digit);
}

[[nodiscard]] ScenarioCatalogRowDTO row(std::string scenario_id,
                                        char identity_digit) {
  return ScenarioCatalogRowDTO{
      .scenario_id = std::move(scenario_id),
      .description = std::string("Maintained scenario"),
      .aliases = {},
      .deployment_artifact_id = core::DeploymentArtifactId("cell_a"),
      .deployment_revision =
          core::DeploymentRevision(digest(identity_digit)),
      .retained_bundle_id =
          core::RetainedBundleId(digest(identity_digit + 1)),
      .program_binding_id =
          core::ProgramBindingId(digest(identity_digit + 2)),
      .deployment_compatibility =
          ScenarioDeploymentCompatibilityDTO{"manyforge-only", {}},
      .assistant_mode = ScenarioAssistantMode::kForbidden,
      .typed_parameter_overrides =
          CanonicalValue(CanonicalValue::Object{}),
  };
}

TEST(ScenarioCatalogTest, SealsAndReloadsTheExactCanonicalCatalog) {
  auto first = row("alpha", '1');
  first.aliases = {"a"};
  const auto catalog = build_scenario_catalog({first, row("beta", '4')});
  const auto regenerated =
      build_scenario_catalog({first, row("beta", '4')});

  ASSERT_NE(catalog, nullptr);
  EXPECT_EQ(catalog->rows().size(), 2U);
  EXPECT_NE(catalog->revision().value(), digest('0'));
  EXPECT_EQ(regenerated->revision(), catalog->revision());
  EXPECT_EQ(regenerated->canonical_bytes(), catalog->canonical_bytes());

  const auto reloaded =
      reload_verified_scenario_catalog(catalog->canonical_bytes());
  EXPECT_EQ(reloaded->revision(), catalog->revision());
  EXPECT_EQ(reloaded->rows(), catalog->rows());
  EXPECT_EQ(reloaded->canonical_bytes(), catalog->canonical_bytes());
}

TEST(ScenarioCatalogTest, RejectsNoncanonicalAndAmbiguousCatalogs) {
  auto candidate = row("alpha", '1');
  candidate.aliases = {"alpha"};
  EXPECT_THROW(build_scenario_catalog({candidate}), ScenarioCatalogError);

  const auto valid = build_scenario_catalog({row("alpha", '1')});
  const std::string noncanonical = std::string(valid->canonical_bytes()) + "\n";
  EXPECT_THROW(reload_verified_scenario_catalog(noncanonical),
               ScenarioCatalogError);

  std::string stale(valid->canonical_bytes());
  const std::size_t identity = stale.find(valid->revision().value());
  ASSERT_NE(identity, std::string::npos);
  stale.replace(identity, valid->revision().value().size(), digest('f'));
  EXPECT_THROW(reload_verified_scenario_catalog(stale), ScenarioCatalogError);
}

TEST(ScenarioCatalogRepositoryTest,
     PublishesThroughExactRevisionCompareAndSetAndReopens) {
  TemporaryDirectory root;
  const auto first = build_scenario_catalog({row("alpha", '1')});
  auto changed_row = row("alpha", '1');
  changed_row.description = "Changed presentation text";
  const auto second = build_scenario_catalog({changed_row});

  {
    FilesystemScenarioCatalogRepository repository(root.path(), "maintained");
    EXPECT_EQ(repository.compare_and_set(std::nullopt, first),
              ScenarioCatalogPublishResult::kPublished);
    EXPECT_EQ(repository.compare_and_set(std::nullopt, second),
              ScenarioCatalogPublishResult::kRevisionConflict);
    const auto unchanged = repository.get_verified();
    ASSERT_NE(unchanged, nullptr);
    EXPECT_EQ(unchanged->revision(), first->revision());
    EXPECT_EQ(repository.compare_and_set(first->revision(), first),
              ScenarioCatalogPublishResult::kAlreadyCurrent);
    EXPECT_EQ(repository.compare_and_set(first->revision(), second),
              ScenarioCatalogPublishResult::kPublished);
  }

  FilesystemScenarioCatalogRepository reopened(root.path(), "maintained");
  const auto current = reopened.get_verified();
  ASSERT_NE(current, nullptr);
  EXPECT_EQ(current->revision(), second->revision());
  EXPECT_EQ(current->rows(), second->rows());

  FilesystemScenarioApplyCandidateSource source(root.path(), "maintained");
  const auto retained = source.catalog_path_for_revision(first->revision());
  EXPECT_NE(retained, source.catalog_path());
  // The exact retired head survives reopening; no catalog enumeration is used.
  EXPECT_TRUE(std::filesystem::is_regular_file(retained));
  std::filesystem::remove(retained);
  EXPECT_THROW(
      static_cast<void>(source.catalog_path_for_revision(first->revision())),
      ScenarioApplyCandidateError);
}

TEST(ScenarioCatalogRepositoryTest,
     RetainsLegacyHeadAndRejectsCorruptRevision) {
  TemporaryDirectory root;
  const auto first = build_scenario_catalog({row("alpha", '1')});
  const auto second = build_scenario_catalog({row("alpha", '4')});
  FilesystemScenarioCatalogRepository repository(root.path(), "maintained");
  ASSERT_EQ(repository.compare_and_set(std::nullopt, first),
            ScenarioCatalogPublishResult::kPublished);
  const auto retained =
      root.path() / ("maintained.scenario-catalog." +
                     first->revision().value().substr(7U) + ".json");
  std::filesystem::remove(retained); // Existing packages had only a head.
  ASSERT_EQ(repository.compare_and_set(first->revision(), second),
            ScenarioCatalogPublishResult::kPublished);
  FilesystemScenarioApplyCandidateSource source(root.path(), "maintained");
  EXPECT_EQ(source.catalog_path_for_revision(first->revision()), retained);
  std::filesystem::copy_file(repository.catalog_path(), retained,
                             std::filesystem::copy_options::overwrite_existing);
  EXPECT_THROW(
      static_cast<void>(source.catalog_path_for_revision(first->revision())),
      ScenarioApplyCandidateError);
  EXPECT_THROW(
      static_cast<void>(repository.compare_and_set(second->revision(), first)),
      ScenarioCatalogError);
  EXPECT_EQ(repository.get_verified()->revision(), second->revision());
}

TEST(ScenarioApplyCandidateSourceTest,
     LoadsOneVerifiedCatalogAcrossMultipleProgramRows) {
  TemporaryDirectory root;
  auto first = row("alpha", '1');
  auto second = row("beta", '1');
  second.program_binding_id = core::ProgramBindingId(digest('4'));
  const auto catalog = build_scenario_catalog({first, second});
  {
    FilesystemScenarioCatalogRepository repository(root.path(), "maintained");
    ASSERT_EQ(repository.compare_and_set(std::nullopt, catalog),
              ScenarioCatalogPublishResult::kPublished);
  }

  FilesystemScenarioApplyCandidateSource source(root.path(), "maintained");
  const auto verified = source.LoadVerifiedCatalog();
  EXPECT_EQ(verified, catalog->normalized_root());
}

TEST(ScenarioApplyCandidateSourceTest,
     KeepsActiveCatalogMeaningSeparateFromPublishedCandidates) {
  TemporaryDirectory active_root;
  TemporaryDirectory candidate_root;
  const auto active_row = row("alpha", '1');
  const auto candidate_row = row("alpha", '4');
  const auto active_catalog = build_scenario_catalog({active_row});
  const auto candidate_catalog = build_scenario_catalog({candidate_row});
  {
    FilesystemScenarioCatalogRepository repository(active_root.path(),
                                                   "maintained");
    ASSERT_EQ(repository.compare_and_set(std::nullopt, active_catalog),
              ScenarioCatalogPublishResult::kPublished);
  }
  {
    FilesystemScenarioCatalogRepository repository(candidate_root.path(),
                                                   "maintained");
    ASSERT_EQ(repository.compare_and_set(std::nullopt, candidate_catalog),
              ScenarioCatalogPublishResult::kPublished);
  }

  FilesystemScenarioApplyCandidateSource source(
      active_root.path(), "maintained", candidate_root.path());

  // Unqualified observation always means the installed/active catalog. A
  // newly compiled candidate cannot silently become the current Scenario.
  EXPECT_EQ(source.LoadVerifiedCatalog(), active_catalog->normalized_root());
  EXPECT_EQ(source.catalog_path(),
            active_root.path() / "maintained.scenario-catalog.yaml");
  EXPECT_EQ(source.catalog_path_for_revision(active_catalog->revision()),
            active_root.path() / "maintained.scenario-catalog.yaml");
  EXPECT_EQ(source.catalog_path_for_revision(candidate_catalog->revision()),
            candidate_root.path() / "maintained.scenario-catalog.yaml");

}

}  // namespace
}  // namespace manyforge::deployment

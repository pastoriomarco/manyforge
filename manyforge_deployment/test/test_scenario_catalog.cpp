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
}

TEST(ScenarioApplyCandidateSourceTest,
     ResolvesOneExactBundleAcrossMultipleProgramRows) {
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
  const auto selected = source.ResolveExact({
      first.deployment_artifact_id,
      first.deployment_revision,
  });
  EXPECT_TRUE(selected.valid());
  EXPECT_EQ(selected.query.deployment_artifact_id,
            first.deployment_artifact_id);
  EXPECT_EQ(selected.scenario_catalog_revision, catalog->revision());
  EXPECT_EQ(selected.retained_bundle_id, first.retained_bundle_id);
  EXPECT_THROW(source.ResolveExact({core::DeploymentArtifactId("missing"),
                                    first.deployment_revision}),
               ScenarioApplyCandidateError);
}

}  // namespace
}  // namespace manyforge::deployment

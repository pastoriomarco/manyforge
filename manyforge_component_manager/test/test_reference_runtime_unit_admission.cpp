#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <manyforge_component_manager/reference_runtime_unit_admission.hpp>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {
namespace {

static_assert(!std::is_default_constructible_v<ReferenceRuntimeUnitAdmission>);
static_assert(!std::is_aggregate_v<ReferenceRuntimeUnitAdmission>);

class ScopedRuntimePackageRoot final {
public:
  ScopedRuntimePackageRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-manager-reference-runtime-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::logic_error(
          "could not create reference-runtime test package root");
    }
  }

  ~ScopedRuntimePackageRoot() {
    std::error_code error;
    (void)std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

struct RuntimePackage final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
};

class MissingRuntimeArtifactSource final
    : public deployment::IVerifiedRetainedProductArtifactSource {
public:
  [[nodiscard]] std::optional<deployment::VerifiedRetainedProductArtifact>
  get_verified(
      const deployment::RetainedProductArtifactQuery &) const override {
    return std::nullopt;
  }
};

[[nodiscard]] RuntimePackage
MaterializeRuntimePackage(const std::filesystem::path &root) {
  const auto fixture = deployment::test::make_runtime_only_retention_fixture();
  const deployment::test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore raw_artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(fixture.qualified, manifests,
                                              input_source, {}, catalogs,
                                              raw_artifacts, bundles);
  if (bundle == nullptr) {
    throw std::logic_error(
        "reference-runtime test package produced no retained bundle");
  }
  return {
      std::move(bundle),
      std::make_unique<
          deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

TEST(ReferenceRuntimeUnitAdmission,
     ExactProductProducesOnlyFixedI4AdapterInputs) {
  ScopedRuntimePackageRoot root;
  const auto package = MaterializeRuntimePackage(root.path());
  const auto admission = VerifyReferenceRuntimeUnitForHost(
      *package.bundle, core::RuntimeUnitId("reference_runtime"),
      *package.artifact_source, "linux", "posix.1-2017");

  ASSERT_TRUE(admission.has_value());
  EXPECT_EQ(admission->runtime_unit_id(), "reference_runtime");
  const core::RuntimeEpoch runtime_epoch("runtime-epoch-1");
  EXPECT_EQ(admission->launch_arguments(runtime_epoch),
            (std::vector<std::string>{
                "--runtime-unit-id", "reference_runtime", "--runtime-epoch",
                "runtime-epoch-1", "--product-identity",
                admission->product_identity(), "--configuration-identity",
                admission->configuration_identity()}));
  EXPECT_EQ(admission->expected_status_line(runtime_epoch, 1U),
            "manyforge.reference-runtime-status.v1 reference_runtime "
            "runtime-epoch-1 1 1 "
            "serving healthy " +
                admission->product_identity() + " " +
                admission->configuration_identity() +
                " manager_fixture closed_no_active_effect");
  EXPECT_EQ(admission->status_command(), "status\n");
  EXPECT_EQ(admission->shutdown_command(), "shutdown\n");
  EXPECT_EQ(admission->runtime_role_id(), "manager_fixture");
  EXPECT_EQ(admission->status_endpoint_role_id(), "runtime_status");
  EXPECT_EQ(admission->process_adapter_contract_id(),
            "manyforge.posix_sh.retained_script");
  EXPECT_EQ(admission->retrieval_contract_id(),
            "manyforge.retained_raw_artifact");
  EXPECT_FALSE(admission->launchable_bytes().empty());

  const RuntimeUnitExpectation expectation(
      admission->runtime_unit_id(), RuntimeLifecycleControl::kManagerManaged,
      admission->product_identity(), admission->configuration_identity(), 1U,
      1U, std::nullopt, {}, {admission->runtime_role_id()},
      ManagedCommandSurfaceState::kClosedNoActiveEffect);
  EXPECT_EQ(expectation.runtime_unit_id(), admission->runtime_unit_id());
  EXPECT_EQ(expectation.product_identity(), admission->product_identity());
  EXPECT_EQ(expectation.configuration_identity(),
            admission->configuration_identity());
  EXPECT_EQ(expectation.required_runtime_role_ids(),
            (std::vector<std::string>{"manager_fixture"}));
}

TEST(ReferenceRuntimeUnitAdmission,
     MissingSubjectAndIncompatibleHostFailWithoutAProcessSurface) {
  ScopedRuntimePackageRoot root;
  const auto package = MaterializeRuntimePackage(root.path());

  EXPECT_FALSE(VerifyReferenceRuntimeUnitForHost(
                   *package.bundle, core::RuntimeUnitId("absent_runtime"),
                   *package.artifact_source, "linux", "posix.1-2017")
                   .has_value());
  EXPECT_THROW(static_cast<void>(VerifyReferenceRuntimeUnitForHost(
                   *package.bundle, core::RuntimeUnitId("reference_runtime"),
                   *package.artifact_source, "darwin", "posix.1-2017")),
               ReferenceRuntimeUnitAdmissionError);
  EXPECT_THROW(static_cast<void>(VerifyReferenceRuntimeUnitForHost(
                   *package.bundle, core::RuntimeUnitId("reference_runtime"),
                   *package.artifact_source, "linux", "posix.1-2008")),
               ReferenceRuntimeUnitAdmissionError);

  const MissingRuntimeArtifactSource missing_artifacts;
  EXPECT_THROW(static_cast<void>(VerifyReferenceRuntimeUnitForHost(
                   *package.bundle, core::RuntimeUnitId("reference_runtime"),
                   missing_artifacts, "linux", "posix.1-2017")),
               deployment::RetainedBundleError);
}

} // namespace
} // namespace manyforge::component_manager

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/capability_definition.hpp>

#include "native_projection_transition_internal.hpp"

namespace manyforge::component_manager::detail {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

[[nodiscard]] CapabilityProviderEndpointBindingDTO
RuntimeEndpoint(const RuntimeUnitId &runtime_unit_id,
                std::string endpoint_role = "scene_projection") {
  return {EndpointRoleId(endpoint_role),
          CapabilityEndpointSourceKind::kRuntimeUnit, runtime_unit_id,
          EndpointRoleId(endpoint_role)};
}

[[nodiscard]] CapabilityProviderBindingDTO
NativeBinding(std::vector<CapabilityProviderEndpointBindingDTO> endpoints) {
  const auto *definition =
      find_builtin_capability_definition("planning.motion.joint_goal");
  if (definition == nullptr) {
    throw std::logic_error("joint-goal capability is absent");
  }
  return {
      CapabilityProviderBindingId("planner.binding"),
      CapabilityProviderBindingRevision("planner.binding.revision"),
      CapabilityId("planning.motion.joint_goal"),
      CapabilityDefinitionRevision(
          std::string(definition->capability_definition_revision)),
      CapabilityScope::kTargetBinding,
      TargetBindingCapabilityProviderTargetDTO{
          ControllableComponentId("robot.1"),
          ComponentTypeRevision("robot.revision"), CommandTargetId("arm"),
          ControlBindingId("control.binding"),
          ControlBindingRevision("control.binding.revision")},
      ProviderInstanceId("planner.provider"),
      ProviderManifestId("planner.manifest"),
      ProviderManifestRevision("planner.manifest.revision"),
      std::move(endpoints)};
}

[[nodiscard]] RuntimeUnitDTO RuntimeUnit(std::string id) {
  return {RuntimeUnitId(std::move(id)),
          RuntimeProfileId("native.execution.host"),
          RuntimeProfileRevision("native.execution.host.revision"), {}, {}};
}

[[nodiscard]] RetainedBundleDTO
Bundle(std::vector<RuntimeUnitDTO> runtime_units,
       std::vector<CapabilityProviderBindingDTO> bindings) {
  return {
      "manyforge.retained_bundle.test",
      RetainedBundleId("bundle.candidate"),
      DeploymentArtifactId("deployment.test"),
      DeploymentRevision("deployment.revision.test"),
      CellGraphDTO{"manyforge.cell_graph.test", GraphRevision("graph.candidate"),
                   CellId("cell.1"), std::move(runtime_units), {}, {}, {}, {},
                   {}, {}},
      RetainedCatalogDTO{
          "manyforge.catalog.test", ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)), {}, {},
          RetainedNodeEnvironmentDTO{
              "catalog.test", "content.test",
              ContentDigest(std::string(kDigestOne)),
              ContentDigest(std::string(kDigestTwo))}},
      {},
      std::move(bindings),
      {},
      RetainedCompiledProductsDTO{
          RetainedProductDTO{ProductId("product.test"),
                             RetainedProductKind::kPlanningCollisionModel,
                             ProductContractId("product.contract.test"),
                             ProductContractRevision(
                                 "product.contract.revision.test"),
                             {}},
          std::nullopt, {}, {}, {}, {}},
      RetainedBundleProvenanceDTO{
          ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)), {}, {}, {}, {}},
      std::nullopt,
      {},
      std::nullopt,
      {}};
}

[[nodiscard]] RuntimeUnitStagedReadyEvidenceDTO
StagedRuntime(std::string id, std::string epoch) {
  return {RuntimeUnitId(std::move(id)),
          RuntimeLifecycleControl::kManagerManaged,
          RuntimeUnitCandidateDisposition::kAdded,
          RuntimeEpoch(std::move(epoch)),
          "manager-launch-adapter-1",
          1U,
          1U,
          1U,
          ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)),
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kHealthy,
          {RuntimeRoleId("scene_projection")},
          ManagedCommandSurfaceState::kClosedNoActiveEffect,
          {},
          0U,
          0U,
          0U,
          RuntimeContractOfferDigest(std::string(kDigestOne))};
}

[[nodiscard]] ManagerApplyRuntimeStagedReadyDTO
StagedReady(std::vector<RuntimeUnitStagedReadyEvidenceDTO> runtimes) {
  std::sort(runtimes.begin(), runtimes.end(),
            [](const auto &left, const auto &right) {
              return left.runtime_unit_id < right.runtime_unit_id;
            });
  return {ApplyId("apply.1"),
          ApplyGeneration(1U),
          1U,
          ContentDigest(std::string(kDigestOne)),
          ActiveBundlePublicationGeneration(1U),
          ContentDigest(std::string(kDigestTwo)),
          1U,
          std::move(runtimes)};
}

[[nodiscard]] RetainedBundleDTO NativeBundle() {
  const RuntimeUnitId runtime_unit_id("runtime.native");
  return Bundle({RuntimeUnit(runtime_unit_id.value())},
                {NativeBinding({RuntimeEndpoint(runtime_unit_id)})});
}

[[nodiscard]] RuntimeObservationSnapshot
ActiveObservations(const std::string &epoch, const bool provide_evidence = true,
                   const RuntimeLifecycleControl lifecycle =
                       RuntimeLifecycleControl::kManagerManaged) {
  RuntimeObservationReducer reducer([] { return 100U; }, {"runtime.native"});
  if (provide_evidence) {
    if (!reducer.ObserveProcess(ProcessPresenceObservation{
            "runtime.native", "manager-observer", 1U,
            ProcessPresenceState::kPresent, epoch}) ||
        !reducer.IngestSourceReport(RuntimeUnitSourceReport{
            "runtime.native", epoch, 1U, 1U, RuntimeSourcePhase::kServing,
            RuntimeSourceHealth::kHealthy, "product.native", "config.native",
            {"scene_projection"},
            ManagedCommandSurfaceState::kClosedNoActiveEffect})) {
      throw std::logic_error("test runtime observations were rejected");
    }
  }
  return reducer.Evaluate({RuntimeUnitExpectation{
      "runtime.native", lifecycle, "product.native", "config.native", 100U,
      100U, std::nullopt, {}, {"scene_projection"},
      ManagedCommandSurfaceState::kClosedNoActiveEffect}});
}

TEST(NativeProjectionInitialization, ZeroNativeBundleProducesNoBindings) {
  EXPECT_TRUE(DeriveCandidateNativeProjectionBindings(Bundle({}, {}),
                                                       StagedReady({}))
                  .empty());
}

TEST(NativeProjectionInitialization,
     CandidateBindingUsesExactManagerStagedEpoch) {
  const auto bindings = DeriveCandidateNativeProjectionBindings(
      NativeBundle(),
      StagedReady({StagedRuntime("runtime.native", "epoch.candidate")}));
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings.front().runtime_unit_id, RuntimeUnitId("runtime.native"));
  EXPECT_EQ(bindings.front().runtime_epoch, RuntimeEpoch("epoch.candidate"));
}

TEST(NativeProjectionInitialization,
     MissingManagerStagedChildCannotMintCandidateEpoch) {
  EXPECT_THROW(DeriveCandidateNativeProjectionBindings(NativeBundle(),
                                                       StagedReady({})),
               std::invalid_argument);
}

TEST(NativeProjectionInitialization,
     ActiveBindingUsesOnlyExactConvergedManagerObservation) {
  const auto bundle = NativeBundle();
  const auto bindings =
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeBindings(
          bundle, ActiveObservations("epoch.manager-observed"));
  ASSERT_EQ(bindings.size(), 1U);
  EXPECT_EQ(bindings.front().runtime_epoch,
            RuntimeEpoch("epoch.manager-observed"));

  EXPECT_THROW(
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeBindings(
          bundle, ActiveObservations("", false)),
      std::invalid_argument);
  EXPECT_THROW(
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeBindings(
          bundle,
          ActiveObservations("epoch.external", true,
                             RuntimeLifecycleControl::kExternallyManaged)),
      std::invalid_argument);
}

TEST(NativeProjectionInitialization,
     NativeProviderMustResolveOneDeclaredRuntimeUnit) {
  const RuntimeUnitId first("runtime.native.1");
  const RuntimeUnitId second("runtime.native.2");
  const auto invalid =
      Bundle({RuntimeUnit(first.value()), RuntimeUnit(second.value())},
             {NativeBinding({RuntimeEndpoint(first, "scene_projection"),
                             RuntimeEndpoint(second, "planning")})});
  EXPECT_THROW(DeriveCandidateNativeProjectionBindings(
                   invalid,
                   StagedReady({StagedRuntime(first.value(), "epoch.1"),
                                StagedRuntime(second.value(), "epoch.2")})),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::component_manager::detail

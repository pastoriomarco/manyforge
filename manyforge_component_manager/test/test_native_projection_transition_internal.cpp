#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_component_manager/active_native_projection_runtime_set.hpp>
#include <manyforge_core/types/capability_definition.hpp>

#include "native_projection_transition_internal.hpp"

namespace manyforge::component_manager::detail {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

[[nodiscard]] ProjectionRevisionDTO PredecessorRevision() {
  return {
      RetainedBundleId("bundle.predecessor"),
      GraphRevision("graph.predecessor"),
      SceneRevision{SceneId(std::string(kSceneId)), 7U,
                    ContentDigest(std::string(kDigestOne))},
  };
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
          ComponentTypeRevision("robot.revision"),
          CommandTargetId("arm"),
          ControlBindingId("control.binding"),
          ControlBindingRevision("control.binding.revision"),
      },
      ProviderInstanceId("planner.provider"),
      ProviderManifestId("planner.manifest"),
      ProviderManifestRevision("planner.manifest.revision"),
      std::move(endpoints),
  };
}

[[nodiscard]] CapabilityProviderEndpointBindingDTO
RuntimeEndpoint(const RuntimeUnitId &runtime_unit_id,
                std::string endpoint_role = "scene_projection") {
  return {
      EndpointRoleId(endpoint_role),
      CapabilityEndpointSourceKind::kRuntimeUnit,
      runtime_unit_id,
      EndpointRoleId(endpoint_role),
  };
}

[[nodiscard]] RetainedBundleDTO
Bundle(std::string bundle_id, std::string graph_revision,
       std::vector<RuntimeUnitDTO> runtime_units,
       std::vector<CapabilityProviderBindingDTO> bindings) {
  return {
      "manyforge.retained_bundle.test",
      RetainedBundleId(std::move(bundle_id)),
      DeploymentArtifactId("deployment.test"),
      DeploymentRevision("deployment.revision.test"),
      CellGraphDTO{
          "manyforge.cell_graph.test",
          GraphRevision(std::move(graph_revision)),
          CellId("cell.1"),
          std::move(runtime_units),
          {},
          {},
          {},
          {},
          {},
      },
      RetainedCatalogDTO{
          "manyforge.catalog.test",
          ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)),
          {},
          {},
          RetainedNodeEnvironmentDTO{
              "catalog.test",
              "content.test",
              ContentDigest(std::string(kDigestOne)),
              ContentDigest(std::string(kDigestTwo)),
          },
      },
      {},
      std::move(bindings),
      {},
      RetainedCompiledProductsDTO{
          RetainedProductDTO{
              ProductId("product.test"),
              RetainedProductKind::kPlanningCollisionModel,
              ProductContractId("product.contract.test"),
              ProductContractRevision("product.contract.revision.test"),
              {},
          },
          std::nullopt,
          {},
          {},
          {},
      },
      RetainedBundleProvenanceDTO{
          ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)),
          {},
          {},
          {},
      },
      std::nullopt,
      {},
      std::nullopt,
      {},
  };
}

[[nodiscard]] RuntimeUnitDTO RuntimeUnit(std::string id) {
  return {
      RuntimeUnitId(std::move(id)),
      RuntimeProfileId("native.execution.host"),
      RuntimeProfileRevision("native.execution.host.revision"),
      {},
      {},
  };
}

[[nodiscard]] RuntimeUnitStagedReadyEvidenceDTO
StagedRuntime(std::string id, std::string epoch) {
  return {
      RuntimeUnitId(std::move(id)),
      manyforge::core::RuntimeLifecycleControl::kManagerManaged,
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
      RuntimeContractOfferDigest(std::string(kDigestOne)),
  };
}

[[nodiscard]] ManagerApplyRuntimeStagedReadyDTO
StagedReady(std::vector<RuntimeUnitStagedReadyEvidenceDTO> runtimes) {
  std::sort(runtimes.begin(), runtimes.end(),
            [](const auto &left, const auto &right) {
              return left.runtime_unit_id < right.runtime_unit_id;
            });
  return {
      ApplyId("apply.1"),
      ApplyGeneration(1U),
      1U,
      ContentDigest(std::string(kDigestOne)),
      ActiveBundlePublicationGeneration(1U),
      ContentDigest(std::string(kDigestTwo)),
      1U,
      std::move(runtimes),
  };
}

[[nodiscard]] ProjectionNativeRuntimeBindingDTO
NativeInstance(std::string epoch) {
  return {
      CapabilityProviderBindingId("planner.binding"),
      CapabilityProviderBindingRevision("planner.binding.revision"),
      RuntimeUnitId("runtime.native"),
      RuntimeEpoch(std::move(epoch)),
  };
}

[[nodiscard]] ProjectionHealthDTO HealthyPredecessor(std::string epoch) {
  return {
      NativeInstance(std::move(epoch)),
      1U,
      ProjectionHealthState::kHealthy,
      PredecessorRevision(),
      std::nullopt,
      1U,
      ContentDigest(std::string(kDigestOne)),
      std::nullopt,
  };
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
            "runtime.native",
            epoch,
            1U,
            1U,
            RuntimeSourcePhase::kServing,
            RuntimeSourceHealth::kHealthy,
            "product.native",
            "config.native",
            {"scene_projection"},
            ManagedCommandSurfaceState::kClosedNoActiveEffect})) {
      throw std::logic_error("test runtime observations were rejected");
    }
  }
  return reducer.Evaluate({RuntimeUnitExpectation{
      "runtime.native",
      lifecycle,
      "product.native",
      "config.native",
      100U,
      100U,
      std::nullopt,
      {},
      {"scene_projection"},
      ManagedCommandSurfaceState::kClosedNoActiveEffect}});
}

[[nodiscard]] RetainedBundleDTO NativeBundle(std::string bundle_id,
                                             std::string graph_revision) {
  const RuntimeUnitId runtime_unit_id("runtime.native");
  return Bundle(std::move(bundle_id), std::move(graph_revision),
                {RuntimeUnit(runtime_unit_id.value())},
                {NativeBinding({RuntimeEndpoint(runtime_unit_id)})});
}

TEST(NativeProjectionTransition, ZeroNativeBundlesProduceNoRows) {
  const auto predecessor =
      Bundle("bundle.predecessor", "graph.predecessor", {}, {});
  const auto candidate = Bundle("bundle.candidate", "graph.candidate", {}, {});

  const auto rows = DeriveNativeProjectionTransitionRows(
      predecessor, candidate, PredecessorRevision(), {}, StagedReady({}));

  EXPECT_TRUE(rows.empty());
}

TEST(NativeProjectionTransition,
     CandidateEpochComesFromManagerStagedReadyEvidence) {
  const auto predecessor =
      Bundle("bundle.predecessor", "graph.predecessor", {}, {});
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");

  const auto rows = DeriveNativeProjectionTransitionRows(
      predecessor, candidate, PredecessorRevision(), {},
      StagedReady({StagedRuntime("runtime.native", "epoch.candidate")}));

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().native_runtime_binding,
            NativeInstance("epoch.candidate"));
  EXPECT_EQ(rows.front().disposition, ProjectionTransitionDisposition::kAdded);
  EXPECT_FALSE(rows.front().expected_local_active.has_value());
}

TEST(NativeProjectionTransition,
     ActiveRuntimeSetUsesOnlyExactConvergedManagerObservation) {
  const auto bundle = NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto bindings =
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeBindings(
          bundle, ActiveObservations("epoch.manager-observed"));
  const auto runtime_set =
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeSet(
          bundle, PredecessorRevision(),
          ActiveObservations("epoch.manager-observed"));

  ASSERT_TRUE(runtime_set.valid());
  EXPECT_EQ(bindings, runtime_set.native_runtime_bindings);
  EXPECT_EQ(runtime_set.active_projection_revision, PredecessorRevision());
  ASSERT_EQ(runtime_set.native_runtime_bindings.size(), 1U);
  EXPECT_EQ(runtime_set.native_runtime_bindings.front(),
            NativeInstance("epoch.manager-observed"));

  EXPECT_THROW(
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeSet(
          bundle, PredecessorRevision(), ActiveObservations("", false)),
      std::invalid_argument);
  EXPECT_THROW(
      manyforge::component_manager::DeriveActiveNativeProjectionRuntimeSet(
          bundle, PredecessorRevision(),
          ActiveObservations("epoch.external", true,
                             RuntimeLifecycleControl::kExternallyManaged)),
      std::invalid_argument);
}

TEST(NativeProjectionTransition,
     EndpointFreeNativeProviderIsNotACompletedProcessJoin) {
  const RuntimeUnitId runtime_unit_id("runtime.native");
  const auto predecessor =
      Bundle("bundle.predecessor", "graph.predecessor", {}, {});
  const auto candidate =
      Bundle("bundle.candidate", "graph.candidate",
             {RuntimeUnit(runtime_unit_id.value())}, {NativeBinding({})});

  EXPECT_THROW(
      DeriveNativeProjectionTransitionRows(
          predecessor, candidate, PredecessorRevision(), {},
          StagedReady({StagedRuntime("runtime.native", "epoch.candidate")})),
      std::invalid_argument);
}

TEST(NativeProjectionTransition,
     MissingManagerStagedChildCannotMintCandidateEpoch) {
  const auto predecessor =
      Bundle("bundle.predecessor", "graph.predecessor", {}, {});
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");

  EXPECT_THROW(DeriveNativeProjectionTransitionRows(predecessor, candidate,
                                                    PredecessorRevision(), {},
                                                    StagedReady({})),
               std::invalid_argument);
}

TEST(NativeProjectionTransition,
     ExactCurrentOwnerHealthRetainsTheSameProcessLifetime) {
  const auto predecessor =
      NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");
  const auto predecessor_health = HealthyPredecessor("epoch.shared");

  const auto rows = DeriveNativeProjectionTransitionRows(
      predecessor, candidate, PredecessorRevision(),
      std::span<const ProjectionHealthDTO>(&predecessor_health, 1U),
      StagedReady({StagedRuntime("runtime.native", "epoch.shared")}));

  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows.front().native_runtime_binding,
            NativeInstance("epoch.shared"));
  EXPECT_EQ(rows.front().disposition,
            ProjectionTransitionDisposition::kRetained);
  EXPECT_EQ(rows.front().expected_local_active, PredecessorRevision());
}

TEST(NativeProjectionTransition,
     ReplacementProcessProducesRemovedAndAddedRows) {
  const auto predecessor =
      NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");
  const auto predecessor_health = HealthyPredecessor("epoch.old");

  const auto rows = DeriveNativeProjectionTransitionRows(
      predecessor, candidate, PredecessorRevision(),
      std::span<const ProjectionHealthDTO>(&predecessor_health, 1U),
      StagedReady({StagedRuntime("runtime.native", "epoch.new")}));

  ASSERT_EQ(rows.size(), 2U);
  const auto added =
      std::find_if(rows.begin(), rows.end(), [](const auto &row) {
        return row.disposition == ProjectionTransitionDisposition::kAdded;
      });
  const auto removed =
      std::find_if(rows.begin(), rows.end(), [](const auto &row) {
        return row.disposition == ProjectionTransitionDisposition::kRemoved;
      });
  ASSERT_NE(added, rows.end());
  ASSERT_NE(removed, rows.end());
  EXPECT_EQ(added->native_runtime_binding, NativeInstance("epoch.new"));
  EXPECT_FALSE(added->expected_local_active.has_value());
  EXPECT_EQ(removed->native_runtime_binding, NativeInstance("epoch.old"));
  EXPECT_EQ(removed->expected_local_active, PredecessorRevision());
  EXPECT_TRUE(CanonicalProjectionTransitionRows(rows));
}

TEST(NativeProjectionTransition,
     ExactWithdrawalReplacesOnlyTheRetiredEpochHealth) {
  const auto predecessor =
      NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");
  auto health = HealthyPredecessor("epoch.old");
  health.state = ProjectionHealthState::kUnavailable;
  health.error = ErrorDTO{"missing_reference", "old process is gone", {}};
  auto ready = StagedReady({StagedRuntime("runtime.native", "epoch.new")});
  auto &runtime = ready.runtime_units.front();
  runtime.candidate_disposition =
      RuntimeUnitCandidateDisposition::kRetainedExact;
  const auto derive = [&] {
    return DeriveNativeProjectionTransitionRows(
        predecessor, candidate, PredecessorRevision(),
        std::span<const ProjectionHealthDTO>(&health, 1U), ready);
  };
  EXPECT_THROW(static_cast<void>(derive()), std::invalid_argument);
  runtime.predecessor_withdrawal =
      ManagedRuntimeWithdrawalDTO{ready.apply_id,
                                  ready.apply_generation,
                                  ready.runtime_convergence_intent_digest,
                                  runtime.runtime_unit_id,
                                  RuntimeEpoch("epoch.old"),
                                  ContentDigest(std::string(kDigestOne)),
                                  ContentDigest(std::string(kDigestTwo)),
                                  "manager-supervisor",
                                  12U};
  const auto rows = derive();
  ASSERT_EQ(rows.size(), 2U);
  for (const auto &row : rows) {
    if (row.disposition == ProjectionTransitionDisposition::kRemoved)
      EXPECT_EQ(row.managed_withdrawal, runtime.predecessor_withdrawal);
    else
      EXPECT_FALSE(row.managed_withdrawal);
  }
  runtime.predecessor_withdrawal->runtime_epoch = RuntimeEpoch("epoch.other");
  EXPECT_THROW(static_cast<void>(derive()), std::invalid_argument);
}

TEST(NativeProjectionTransition,
     UnavailableNamedApplyAggregateUsesExactWithdrawalPerBinding) {
  const auto predecessor =
      NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");
  auto health = HealthyPredecessor("epoch.old");
  const ErrorDTO unavailable{"missing_reference", "old process is gone", {}};
  health.state = ProjectionHealthState::kUnavailable;
  health.error = unavailable;
  ProjectionAggregateHealthDTO aggregate{
      NamedApplyProjectionSetSourceDTO{
          ApplyId("apply.predecessor"), ApplyGeneration(1U),
          ProjectionStageGeneration(1U), PredecessorRevision()},
      ProjectionHealthState::kUnavailable,
      {health},
      unavailable,
  };
  ASSERT_TRUE(aggregate.valid());
  ASSERT_TRUE(aggregate.matches_named_apply(
      ApplyId("apply.predecessor"), ApplyGeneration(1U),
      ProjectionStageGeneration(1U), PredecessorRevision()));
  ASSERT_FALSE(aggregate.healthy_for_target(PredecessorRevision()));

  auto ready = StagedReady({StagedRuntime("runtime.native", "epoch.new")});
  auto &runtime = ready.runtime_units.front();
  runtime.candidate_disposition =
      RuntimeUnitCandidateDisposition::kRetainedExact;
  runtime.predecessor_withdrawal =
      ManagedRuntimeWithdrawalDTO{ready.apply_id,
                                  ready.apply_generation,
                                  ready.runtime_convergence_intent_digest,
                                  runtime.runtime_unit_id,
                                  RuntimeEpoch("epoch.old"),
                                  ContentDigest(std::string(kDigestOne)),
                                  ContentDigest(std::string(kDigestTwo)),
                                  "manager-supervisor",
                                  12U};

  const auto rows = DeriveNativeProjectionTransitionRows(
      predecessor, candidate, PredecessorRevision(),
      aggregate.native_projection_health, ready);
  ASSERT_EQ(rows.size(), 2U);
  const auto removed =
      std::find_if(rows.begin(), rows.end(), [](const auto &row) {
        return row.disposition == ProjectionTransitionDisposition::kRemoved;
      });
  ASSERT_NE(removed, rows.end());
  EXPECT_EQ(removed->managed_withdrawal, runtime.predecessor_withdrawal);
}

TEST(NativeProjectionTransition,
     PredecessorEpochMustComeFromExactFinalizedOwnerHealth) {
  const auto predecessor =
      NativeBundle("bundle.predecessor", "graph.predecessor");
  const auto candidate = NativeBundle("bundle.candidate", "graph.candidate");
  auto predecessor_health = HealthyPredecessor("epoch.old");
  predecessor_health.active_projection_revision = ProjectionRevisionDTO{
      RetainedBundleId("bundle.other"),
      GraphRevision("graph.other"),
      PredecessorRevision().scene_revision,
  };

  EXPECT_THROW(
      DeriveNativeProjectionTransitionRows(
          predecessor, candidate, PredecessorRevision(),
          std::span<const ProjectionHealthDTO>(&predecessor_health, 1U),
          StagedReady({StagedRuntime("runtime.native", "epoch.old")})),
      std::invalid_argument);
}

TEST(NativeProjectionTransition,
     NativeProviderCannotResolveAcrossTwoRuntimeUnits) {
  const RuntimeUnitId first("runtime.native.1");
  const RuntimeUnitId second("runtime.native.2");
  const auto predecessor =
      Bundle("bundle.predecessor", "graph.predecessor", {}, {});
  const auto candidate =
      Bundle("bundle.candidate", "graph.candidate",
             {RuntimeUnit(first.value()), RuntimeUnit(second.value())},
             {NativeBinding({RuntimeEndpoint(first, "scene_projection"),
                             RuntimeEndpoint(second, "planning")})});

  EXPECT_THROW(DeriveNativeProjectionTransitionRows(
                   predecessor, candidate, PredecessorRevision(), {},
                   StagedReady({StagedRuntime("runtime.native.1", "epoch.1"),
                                StagedRuntime("runtime.native.2", "epoch.2")})),
               std::invalid_argument);
}

} // namespace
} // namespace manyforge::component_manager::detail

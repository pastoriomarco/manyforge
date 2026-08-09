#include <gtest/gtest.h>

#include <cstdint>
#include <limits>
#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>
#include <manyforge_core/ports/imanager_kernel_apply_fence_port.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::core {
namespace {

constexpr const char* kDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";
constexpr const char* kOtherDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr const char* kSceneIdDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

static_assert(!std::is_same_v<ApplyId, BootstrapId>);
static_assert(!std::is_same_v<ApplyGeneration, BootstrapGeneration>);
static_assert(!std::is_same_v<ProjectionStageGeneration, ActiveBundlePublicationGeneration>);
static_assert(!std::is_constructible_v<ApplyGeneration, BootstrapGeneration>);
static_assert(!std::is_convertible_v<std::uint64_t, ApplyGeneration>);
static_assert(std::variant_size_v<ActiveBundlePublicationTransactionRefDTO> == 2U);
static_assert(
    std::is_same_v<decltype(std::declval<ApplyConfigurationFenceIntentDTO>().owner_transaction),
                   ApplyFenceOwnerDTO>);
static_assert(!std::is_default_constructible_v<LogicalSceneSnapshotRoot>);
static_assert(std::is_constructible_v<LogicalSceneSnapshotRoot,
                                      std::shared_ptr<const LogicalSceneSnapshotDTO>>);
static_assert(std::is_same_v<decltype(std::declval<NativeProjectionBootstrapRebuildResultDTO>()
                                          .planning_collision_model),
                             RetainedProductDTO>);
static_assert(std::is_same_v<decltype(std::declval<NativeProjectionBootstrapRebuildResultDTO>()
                                          .initialized_logical_scene),
                             LogicalSceneSnapshotRoot>);

using PublishSignature = ActiveBundlePublicationReceiptDTO (IActiveBundlePublicationStore::*)(
    const ActiveBundlePublicationRequestDTO&);
using ReadPublicationSignature = Result<ActiveBundlePublicationReceiptDTO> (
    IActiveBundlePublicationReader::*)(ActiveBundlePublicationGeneration) const;
static_assert(std::is_same_v<decltype(&IActiveBundlePublicationStore::PublishActiveBundle),
                             PublishSignature>);
static_assert(std::is_same_v<decltype(&IActiveBundlePublicationReader::ReadActiveBundlePublication),
                             ReadPublicationSignature>);
static_assert(std::is_base_of_v<IActiveBundlePublicationReader, IActiveBundlePublicationStore>);

template <typename T>
concept ExposesPublish = requires(T& value, const ActiveBundlePublicationRequestDTO& request) {
  value.PublishActiveBundle(request);
};

static_assert(!ExposesPublish<IActiveBundlePublicationReader>);
static_assert(ExposesPublish<IActiveBundlePublicationStore>);

using StageSignature = ProjectionApplyStageResultDTO (IManagerSceneProjectionHandoffPort::*)(
    const ProjectionApplyStageDTO&);
using ActivateSignature = ProjectionApplyActivationResultDTO (
    IManagerSceneProjectionHandoffPort::*)(const ProjectionApplyActivationDTO&);
using DiscardSignature = ProjectionApplyDiscardResultDTO (IManagerSceneProjectionHandoffPort::*)(
    const ProjectionApplyDiscardDTO&);
using FinalizeSignature = ProjectionApplyFinalizeResultDTO (IManagerSceneProjectionHandoffPort::*)(
    const ProjectionApplyFinalizeDTO&);
using ApplyStatusSignature = ProjectionApplyStatusDTO (IManagerSceneProjectionHandoffPort::*)(
    const ProjectionApplyStatusQueryDTO&) const;
using ReconcileActiveProjectionSignature =
    ActiveProjectionRuntimeRecoveryResultDTO (
        IManagerSceneProjectionHandoffPort::*)(
        const ActiveProjectionRuntimeSetDTO&);
using ActiveProjectionHealthSignature = ProjectionAggregateHealthDTO (
    IManagerSceneProjectionHandoffPort::*)() const;
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionHandoffPort::StageCandidateProjection),
                   StageSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionHandoffPort::ActivateStagedProjection),
                   ActivateSignature>);
static_assert(std::is_same_v<decltype(&IManagerSceneProjectionHandoffPort::DiscardStagedProjection),
                             DiscardSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionHandoffPort::FinalizeProjectionTransition),
                   FinalizeSignature>);
static_assert(std::is_same_v<decltype(&IManagerSceneProjectionHandoffPort::ProjectionApplyStatus),
                             ApplyStatusSignature>);
static_assert(std::is_same_v<
              decltype(&IManagerSceneProjectionHandoffPort::
                           ReconcileActiveProjectionRuntimes),
              ReconcileActiveProjectionSignature>);
static_assert(std::is_same_v<
              decltype(&IManagerSceneProjectionHandoffPort::
                           ActiveProjectionHealth),
              ActiveProjectionHealthSignature>);

using BeginBootstrapSignature = ProjectionBootstrapAcceptedDTO (
    IManagerSceneProjectionBootstrapPort::*)(const ProjectionBootstrapIntentDTO&);
using ObserveBootstrapPrerequisitesSignature = ProjectionBootstrapPrerequisiteStatusDTO (
    IManagerSceneProjectionBootstrapPort::*)(const ProjectionBootstrapPrerequisiteQueryDTO&) const;
using RebuildBootstrapSignature = ProjectionBootstrapRebuildResultDTO (
    IManagerSceneProjectionBootstrapPort::*)(const ProjectionBootstrapRebuildRequestDTO&);
using BootstrapStatusSignature = ProjectionBootstrapStatusDTO (
    IManagerSceneProjectionBootstrapPort::*)(const ProjectionBootstrapStatusQueryDTO&) const;
using FinalizeBootstrapSignature = ProjectionBootstrapFinalizeResultDTO (
    IManagerSceneProjectionBootstrapPort::*)(const ProjectionBootstrapFinalizeDTO&);
static_assert(
    std::is_same_v<
        decltype(&IManagerSceneProjectionBootstrapPort::ObserveProjectionBootstrapPrerequisites),
        ObserveBootstrapPrerequisitesSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionBootstrapPort::BeginProjectionBootstrap),
                   BeginBootstrapSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionBootstrapPort::RebuildProjectionBootstrap),
                   RebuildBootstrapSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionBootstrapPort::ProjectionBootstrapStatus),
                   BootstrapStatusSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerSceneProjectionBootstrapPort::FinalizeProjectionBootstrap),
                   FinalizeBootstrapSignature>);

using ObserveFenceAdmissionSignature = ConfigurationFenceAdmissionSnapshotDTO (
    IManagerKernelApplyFencePort::*)(const ConfigurationFenceAdmissionQueryDTO&) const;
using AcquireFenceSignature = ConfigurationFenceAcceptedDTO (IManagerKernelApplyFencePort::*)(const ApplyConfigurationFenceIntentDTO&);
using ReconcileFenceAttemptSignature = ApplyFenceAcquireAttemptReconcileResultDTO (
    IManagerKernelApplyFencePort::*)(const ApplyFenceAcquireAttemptQueryDTO&) const;
using RehydrateFenceSignature = ApplyFenceReconcileResultDTO (IManagerKernelApplyFencePort::*)(const ApplyFenceRehydrateDTO&);
using QueryFenceStatusSignature = ConfigurationFenceStatusDTO (IManagerKernelApplyFencePort::*)(
    const ConfigurationFenceStatusQueryDTO&) const;
using ReleaseFenceSignature = ConfigurationFenceReleasedDTO (IManagerKernelApplyFencePort::*)(
    const ConfigurationFenceReleaseDTO&);
static_assert(std::is_same_v<decltype(&IManagerKernelApplyFencePort::ObserveAdmission),
                             ObserveFenceAdmissionSignature>);
static_assert(std::is_same_v<decltype(&IManagerKernelApplyFencePort::Acquire),
                             AcquireFenceSignature>);
static_assert(
    std::is_same_v<decltype(&IManagerKernelApplyFencePort::ReconcileAcquireAttempt),
                   ReconcileFenceAttemptSignature>);
static_assert(std::is_same_v<decltype(&IManagerKernelApplyFencePort::Rehydrate),
                             RehydrateFenceSignature>);
static_assert(std::is_same_v <decltype(&IManagerKernelApplyFencePort::QueryStatus),
                             QueryFenceStatusSignature>);
static_assert(
    std::is_same_v<decltype( &IManagerKernelApplyFencePort::Release), ReleaseFenceSignature>);

template <typename T>
concept ExposesGenericStop = requires(T& value) { value.Stop(); };

static_assert(!ExposesGenericStop<IManagerKernelApplyFencePort>);

[[nodiscard]] ProjectionRevisionDTO Projection(const char* bundle_digest = kDigest,
                                               const char* graph_digest = kDigest,
                                               std::uint64_t scene_revision = 1U) {
  return ProjectionRevisionDTO{
      RetainedBundleId{bundle_digest},
      GraphRevision{graph_digest},
      SceneRevision{SceneId{kSceneIdDigest}, scene_revision, ContentDigest{kDigest}},
  };
}

[[nodiscard]] ProjectionNativeRuntimeBindingDTO NativeBinding(
    const char* provider_binding_id = "provider-binding.planner",
    const char* runtime_unit_id = "runtime.planner") {
  return ProjectionNativeRuntimeBindingDTO{
      CapabilityProviderBindingId{provider_binding_id},
      CapabilityProviderBindingRevision{kDigest},
      RuntimeUnitId{runtime_unit_id},
      RuntimeEpoch{"runtime-epoch-1"},
  };
}

[[nodiscard]] ProjectionHealthDTO HealthyProjection(
    const ProjectionRevisionDTO& projection,
    ProjectionNativeRuntimeBindingDTO binding = NativeBinding()) {
  return ProjectionHealthDTO{
      std::move(binding), 1U, ProjectionHealthState::kHealthy, projection,
      std::nullopt,       1U, ContentDigest{kDigest},          std::nullopt,
  };
}

[[nodiscard]] ProjectionTransitionEvidenceDTO TransitionEvidence(
    ProjectionTransitionEvidenceState state = ProjectionTransitionEvidenceState::kStaged) {
  const auto predecessor = Projection();
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  return ProjectionTransitionEvidenceDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{1U},
      predecessor,
      candidate,
      state,
      {},
      state == ProjectionTransitionEvidenceState::kUncertain
          ? std::optional<ErrorDTO>{ErrorDTO{"projection_failed", "test uncertainty", {}}}
          : std::nullopt,
  };
}

[[nodiscard]] ProjectionTransitionEvidenceDTO FinalizedTransitionEvidence() {
  auto evidence = TransitionEvidence(ProjectionTransitionEvidenceState::kFinalized);
  const auto predecessor = evidence.predecessor_projection_revision;
  const auto candidate = evidence.candidate_projection_revision;
  evidence.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          NativeBinding("provider-binding.a", "runtime.a"),
          ProjectionTransitionDisposition::kRetained,
          predecessor,
      },
      ProjectionTransitionRowEvidenceState::kCandidateActive,
      candidate,
      candidate,
      HealthyProjection(candidate, NativeBinding("provider-binding.a", "runtime.a")),
      std::nullopt,
  });
  evidence.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          NativeBinding("provider-binding.b", "runtime.b"),
          ProjectionTransitionDisposition::kAdded,
          std::nullopt,
      },
      ProjectionTransitionRowEvidenceState::kCandidateActive,
      candidate,
      candidate,
      HealthyProjection(candidate, NativeBinding("provider-binding.b", "runtime.b")),
      std::nullopt,
  });
  evidence.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          NativeBinding("provider-binding.c", "runtime.c"),
          ProjectionTransitionDisposition::kRemoved,
          predecessor,
      },
      ProjectionTransitionRowEvidenceState::kRemovedRetired,
      std::nullopt,
      std::nullopt,
      ProjectionHealthDTO{
          NativeBinding("provider-binding.c", "runtime.c"),
          1U,
          ProjectionHealthState::kUninitialized,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      },
      std::nullopt,
  });
  return evidence;
}

[[nodiscard]] ProjectionAggregateHealthDTO NamedAggregateHealth() {
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  return ProjectionAggregateHealthDTO{
      NamedApplyProjectionSetSourceDTO{
          ApplyId{"apply-1"},
          ApplyGeneration{1U},
          ProjectionStageGeneration{1U},
          candidate,
      },
      ProjectionHealthState::kHealthy,
      {
          HealthyProjection(candidate, NativeBinding("provider-binding.a", "runtime.a")),
          HealthyProjection(candidate, NativeBinding("provider-binding.b", "runtime.b")),
      },
      std::nullopt,
  };
}

[[nodiscard]] RetainedBundleDTO RetainedBundle(const ProjectionRevisionDTO& projection) {
  return RetainedBundleDTO{
      std::string{kRetainedBundleApiVersion},
      projection.retained_bundle_id,
      DeploymentArtifactId{"deployment-1"},
      DeploymentRevision{"deployment-revision-1"},
      CellGraphDTO{
          "manyforge.cell_graph.v1",
          projection.graph_revision,
          CellId{"cell-a"},
          {},
          {},
          {},
          {},
          {},
          {},
      },
      RetainedCatalogDTO{
          "manyforge.catalog.v1",
          ContentDigest{kDigest},
          ContentDigest{kDigest},
          {},
          {},
          RetainedNodeEnvironmentDTO{
              "catalog-1",
              "1",
              ContentDigest{kDigest},
              ContentDigest{kDigest},
          },
      },
      {},
      {},
      RetainedCompiledProductsDTO{
          RetainedProductDTO{
              ProductId{"planning-collision-model"},
              RetainedProductKind::kPlanningCollisionModel,
              ProductContractId{"planning-collision-contract"},
              ProductContractRevision{"1"},
              {},
          },
          std::nullopt,
          {},
          {},
          {},
      },
      RetainedBundleProvenanceDTO{
          ContentDigest{kDigest},
          ContentDigest{kDigest},
          {},
          {},
          {},
      },
      std::nullopt,
      {},
      std::nullopt,
  };
}

[[nodiscard]] ProjectionAggregateHealthDTO BootstrapAggregateHealth() {
  const auto target = Projection();
  return ProjectionAggregateHealthDTO{
      BootstrapProjectionSetSourceDTO{
          BootstrapId{"bootstrap-1"},
          BootstrapGeneration{1U},
          target,
      },
      ProjectionHealthState::kHealthy,
      {},
      std::nullopt,
  };
}

[[nodiscard]] ActiveBundlePublicationRequestDTO NamedPublicationRequest() {
  const auto projection = Projection(kOtherDigest, kOtherDigest, 2U);
  return ActiveBundlePublicationRequestDTO{
      NamedApplyPublicationTransactionDTO{
          ApplyId{"apply-1"},
          ApplyGeneration{1U},
          ProjectionStageGeneration{1U},
      },
      projection,
      projection.retained_bundle_id,
      projection.graph_revision,
      ActiveBundlePublicationGeneration{3U},
      ContentDigest{kDigest},
      ContentDigest{kOtherDigest},
  };
}

[[nodiscard]] ActiveBundlePublicationRequestDTO BootstrapPublicationRequest() {
  const auto projection = Projection();
  return ActiveBundlePublicationRequestDTO{
      BootstrapPublicationTransactionDTO{
          BootstrapId{"bootstrap-1"},
          BootstrapGeneration{1U},
      },
      projection,
      projection.retained_bundle_id,
      projection.graph_revision,
      std::nullopt,
      ContentDigest{kDigest},
      ContentDigest{kOtherDigest},
  };
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO NamedPublicationReceipt() {
  return ActiveBundlePublicationReceiptDTO{
      NamedPublicationRequest(),
      ActiveBundlePublicationGeneration{4U},
      ContentDigest{kDigest},
  };
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO BootstrapPublicationReceipt() {
  return ActiveBundlePublicationReceiptDTO{
      BootstrapPublicationRequest(),
      ActiveBundlePublicationGeneration{1U},
      ContentDigest{kDigest},
  };
}

[[nodiscard]] NativeProjectionBootstrapStatusDTO BootstrapNativeStatus(
    NativeProjectionBootstrapStatusState state) {
  const auto target = Projection();
  auto health = HealthyProjection(target);
  std::optional<ProjectionRevisionDTO> acknowledged_target;
  std::optional<ErrorDTO> error;
  switch (state) {
    case NativeProjectionBootstrapStatusState::kAbsent:
      health.state = ProjectionHealthState::kUninitialized;
      health.active_projection_revision.reset();
      break;
    case NativeProjectionBootstrapStatusState::kRebuilding:
      health.state = ProjectionHealthState::kRebuilding;
      health.active_projection_revision.reset();
      break;
    case NativeProjectionBootstrapStatusState::kRebuilt:
      acknowledged_target = target;
      break;
    case NativeProjectionBootstrapStatusState::kFailedProjectionUncertain:
      health.state = ProjectionHealthState::kUnavailable;
      health.active_projection_revision.reset();
      error = ErrorDTO{"projection_failed", "test uncertainty", {}};
      break;
  }
  return NativeProjectionBootstrapStatusDTO{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      NativeBinding(),
      target,
      state,
      acknowledged_target,
      1U,
      ContentDigest{kDigest},
      std::move(health),
      error,
  };
}

[[nodiscard]] ProjectionBootstrapEvidenceDTO BootstrapEvidence() {
  return ProjectionBootstrapEvidenceDTO{
      BootstrapId{"bootstrap-1"}, BootstrapGeneration{1U}, Projection(), {},
      BootstrapAggregateHealth(),
  };
}

[[nodiscard]] ConfigurationFenceScopeDTO FenceScope() {
  return ConfigurationFenceScopeDTO{
      {ControllableComponentId{"component.arm-a"}},
      {{ControlDomainId{"domain.arm-a"}, ControlDomainRevision{kDigest},
        ControlDomainRevision{kDigest}}},
      {{ControlBindingId{"binding.arm-a"}, ControlBindingRevision{kDigest},
        ControlBindingRevision{kDigest}}},
      {ResourceId{"arm-a-joint-1"}},
  };
}

[[nodiscard]] ApplyConfigurationFenceIntentDTO FenceIntent() {
  return ApplyConfigurationFenceIntentDTO{
      PublicRequestContextDTO{
          RequestId{"request-1"},
          RequestOwnerEpoch{"kernel-epoch-1"},
          "component-manager",
          std::nullopt,
      },
      ApplyFenceOwnerDTO{ApplyId{"apply-1"}, ApplyGeneration{1U}},
      FenceScope(),
      ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      RetainedBundleId{kDigest},
      GraphRevision{kOtherDigest},
      7U,
      ConfigurationFenceGeneration{0U},
      {{ControlDomainId{"domain.arm-a"}, ActiveControlConfigurationId{"active.arm-a"},
        ActiveControlConfigurationGeneration{9U}}},
      "candidate structure transition",
  };
}

[[nodiscard]] ApplyFenceRehydrateDTO FenceRehydrateRequest() {
  return ApplyFenceRehydrateDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      RequestId{"request-1"},
      RequestOwnerEpoch{"kernel-epoch-1"},
      ContentDigest{kDigest},
      RetainedBundleId{kDigest},
      GraphRevision{kDigest},
      RetainedBundleId{kOtherDigest},
      GraphRevision{kOtherDigest},
      ConfigurationFenceId{"fence-1"},
      ConfigurationFenceGeneration{1U},
      FenceScope(),
      "fence_acquired",
      RuntimeEpoch{"manager-epoch-2"},
  };
}

[[nodiscard]] ApplyFenceAcquireAttemptQueryDTO FenceAttemptQuery() {
  return ApplyFenceAcquireAttemptQueryDTO{
      FenceIntent(),
      ContentDigest{kDigest},
      RuntimeEpoch{"manager-epoch-2"},
  };
}

[[nodiscard]] ExecutionHostEpochDTO ExecutionHostEpoch() {
  return ExecutionHostEpochDTO{
      RuntimeUnitId{"runtime.kernel"},       RuntimeEpoch{"kernel-runtime-2"},
      RequestOwnerEpoch{"kernel-epoch-1"},   ContentDigest{kDigest},
      ActiveBundlePublicationGeneration{4U},
  };
}

[[nodiscard]] ConfigurationFenceAdmissionSnapshotDTO FenceAdmissionSnapshot() {
  const ConfigurationFenceAdmissionQueryDTO query{
      FenceScope(), RuntimeEpoch{"manager-epoch-2"}};
  return {
      query,
      ExecutionHostEpoch(),
      Projection(),
      ContentDigest{kOtherDigest},
      7U,
      ConfigurationFenceGeneration{0U},
      {{ControlDomainId{"domain.arm-a"},
        ActiveControlConfigurationId{"active.arm-a"},
        ActiveControlConfigurationGeneration{9U}}},
      0U,
      0U,
      ContentDigest{kDigest},
  };
}

[[nodiscard]] ConfigurationFenceReconciliationEvidenceDTO FenceReleaseEvidence() {
  return ConfigurationFenceReconciliationEvidenceDTO{
      ExecutionHostEpoch(),
      ApplyFenceOwnerDTO{ApplyId{"apply-1"}, ApplyGeneration{1U}},
      ConfigurationFenceId{"fence-1"},
      ConfigurationFenceGeneration{1U},
      FenceScope(),
      8U,
      GraphRevision{kOtherDigest},
      Projection(kOtherDigest, kOtherDigest, 2U),
      ActiveBundlePublicationGeneration{4U},
      ContentDigest{kOtherDigest},
      {{ControlDomainId{"domain.arm-a"}, ActiveControlConfigurationId{"active.arm-a-v2"},
        ActiveControlConfigurationGeneration{10U}}},
      0U,
      0U,
      ContentDigest{kDigest},
  };
}

[[nodiscard]] ConfigurationFenceReleaseDTO FenceReleaseRequest() {
  return {
      FenceReleaseEvidence(),
      RuntimeEpoch{"manager-epoch-2"},
  };
}

[[nodiscard]] ConfigurationFenceStatusQueryDTO FenceStatusQuery() {
  const auto evidence = FenceReleaseEvidence();
  return {
      evidence.owner_transaction,
      evidence.configuration_fence_id,
      evidence.configuration_fence_generation,
      evidence.reconciled_scope,
      RuntimeEpoch{"manager-epoch-2"},
  };
}

class RecordingPublicationStore final : public IActiveBundlePublicationStore {
 public:
  RecordingPublicationStore() = default;

  explicit RecordingPublicationStore(ActiveBundlePublicationGeneration current_generation)
      : current_generation_(current_generation) {}

  ActiveBundlePublicationReceiptDTO PublishActiveBundle(
      const ActiveBundlePublicationRequestDTO& request) override {
    if (!request.valid()) {
      throw std::invalid_argument("invalid publication request");
    }
    for (const auto& receipt : receipts_) {
      if (receipt.request == request) {
        return receipt;
      }
    }
    if (request.expected_previous_publication_generation != current_generation_) {
      throw std::logic_error("compare-and-swap conflict");
    }
    const auto next_generation =
        current_generation_.has_value()
            ? ActiveBundlePublicationGeneration{current_generation_->value() + 1U}
            : ActiveBundlePublicationGeneration{1U};
    receipts_.push_back(
        ActiveBundlePublicationReceiptDTO{request, next_generation, ContentDigest{kDigest}});
    current_generation_ = next_generation;
    return receipts_.back();
  }

  Result<ActiveBundlePublicationReceiptDTO> ReadActiveBundlePublication(
      ActiveBundlePublicationGeneration publication_generation) const override {
    for (const auto& receipt : receipts_) {
      if (receipt.publication_generation == publication_generation) {
        return Result<ActiveBundlePublicationReceiptDTO>::Ok(receipt);
      }
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Err(
        ErrorDTO{"missing_reference", "publication generation is not present", {}});
  }

 private:
  std::optional<ActiveBundlePublicationGeneration> current_generation_;
  std::vector<ActiveBundlePublicationReceiptDTO> receipts_;
};

TEST(ComponentManagerPortContracts, CompositeProjectionRevisionRejectsPartialIdentity) {
  const auto valid = Projection();
  EXPECT_TRUE(valid.valid());
  EXPECT_TRUE(valid.scene_revision.valid());

  const ProjectionRevisionDTO invalid{
      valid.retained_bundle_id,
      valid.graph_revision,
      SceneRevision{SceneId{kSceneIdDigest}, 0U, ContentDigest{kDigest}},
  };
  EXPECT_FALSE(invalid.valid());
}

TEST(ComponentManagerPortContracts, PublicationRejectsEmptyAndZeroTransactionIdentities) {
  auto named = NamedPublicationRequest();
  ASSERT_TRUE(named.valid());

  named.transaction_ref = NamedApplyPublicationTransactionDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{0U},
  };
  EXPECT_FALSE(named.valid());

  named = NamedPublicationRequest();
  named.expected_previous_publication_generation = ActiveBundlePublicationGeneration{0U};
  EXPECT_FALSE(named.valid());

  auto bootstrap = BootstrapPublicationRequest();
  ASSERT_TRUE(bootstrap.valid());
  bootstrap.transaction_ref =
      BootstrapPublicationTransactionDTO{BootstrapId{""}, BootstrapGeneration{0U}};
  EXPECT_FALSE(bootstrap.valid());
}

TEST(ComponentManagerPortContracts, PublicationPresenceAndCompareAndSwapAreTransactionExact) {
  auto named = NamedPublicationRequest();
  named.expected_previous_publication_generation.reset();
  EXPECT_FALSE(named.valid());

  auto bootstrap = BootstrapPublicationRequest();
  bootstrap.expected_previous_publication_generation = ActiveBundlePublicationGeneration{3U};
  EXPECT_FALSE(bootstrap.valid());

  RecordingPublicationStore store{ActiveBundlePublicationGeneration{3U}};
  const auto request = NamedPublicationRequest();
  const auto first = store.PublishActiveBundle(request);
  EXPECT_EQ(store.PublishActiveBundle(request), first);

  auto stale = request;
  stale.transaction_ref = NamedApplyPublicationTransactionDTO{
      ApplyId{"apply-2"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{1U},
  };
  stale.expected_previous_publication_generation = ActiveBundlePublicationGeneration{2U};
  EXPECT_THROW(store.PublishActiveBundle(stale), std::logic_error);

  const auto exact = store.ReadActiveBundlePublication(first.publication_generation);
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value(), first);
}

TEST(ComponentManagerPortContracts, StagePresenceValidatorRejectsPartialPostCaptureTuple) {
  const auto projection = Projection();
  const ErrorDTO error{"projection_failed", "test failure", {}};

  const ProjectionApplyStageResultDTO clean_rejection{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      projection,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ProjectionApplyStageResultState::kRejected,
      error,
  };
  EXPECT_TRUE(clean_rejection.presence_valid());

  const ProjectionApplyStageResultDTO partial_capture{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      projection,
      projection,
      ProjectionStageGeneration{1U},
      std::nullopt,
      ProjectionApplyStageResultState::kFailedProjectionUncertain,
      error,
  };
  EXPECT_FALSE(partial_capture.presence_valid());
}

TEST(ComponentManagerPortContracts, ApplyResultEvidencePhaseMustMatchClaimedPhase) {
  const auto predecessor = Projection();
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);

  ProjectionApplyActivationResultDTO activation{
      ApplyId{"apply-1"}, ApplyGeneration{1U},
      predecessor,        ProjectionStageGeneration{1U},
      candidate,          ProjectionApplyActivationResultState::kActivated,
      candidate,          TransitionEvidence(),
      std::nullopt,
  };
  EXPECT_FALSE(activation.presence_valid());
  activation.transition_evidence =
      TransitionEvidence(ProjectionTransitionEvidenceState::kActivatedAndRetired);
  EXPECT_TRUE(activation.presence_valid());

  ProjectionApplyDiscardResultDTO discard{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{1U},
      candidate,
      predecessor,
      ProjectionApplyDiscardResultState::kDiscarded,
      TransitionEvidence(),
      std::nullopt,
  };
  EXPECT_FALSE(discard.presence_valid());
  discard.transition_evidence = TransitionEvidence(ProjectionTransitionEvidenceState::kAborted);
  EXPECT_TRUE(discard.presence_valid());

  ProjectionApplyFinalizeResultDTO finalize{
      ApplyId{"apply-1"}, ApplyGeneration{1U},       ProjectionStageGeneration{1U},
      candidate,          NamedPublicationReceipt(), ProjectionApplyFinalizeResultState::kFinalized,
      candidate,          TransitionEvidence(),      NamedAggregateHealth(),
      std::nullopt,
  };
  EXPECT_FALSE(finalize.presence_valid());
  finalize.transition_evidence = FinalizedTransitionEvidence();
  EXPECT_TRUE(finalize.presence_valid());

  finalize.candidate_aggregate_health.native_projection_health.pop_back();
  EXPECT_FALSE(finalize.presence_valid());
  finalize.candidate_aggregate_health = NamedAggregateHealth();
  finalize.candidate_aggregate_health.native_projection_health.push_back(
      HealthyProjection(candidate, NativeBinding("provider-binding.z", "runtime.z")));
  EXPECT_FALSE(finalize.presence_valid());
  finalize.candidate_aggregate_health = NamedAggregateHealth();
  finalize.candidate_aggregate_health.native_projection_health.front().native_runtime_binding =
      NativeBinding("provider-binding.aa", "runtime.aa");
  ASSERT_TRUE(finalize.candidate_aggregate_health.valid());
  EXPECT_FALSE(finalize.presence_valid());
  finalize.candidate_aggregate_health = NamedAggregateHealth();

  finalize.transition_evidence.state = static_cast<ProjectionTransitionEvidenceState>(999);
  EXPECT_FALSE(finalize.presence_valid());
  finalize.state = static_cast<ProjectionApplyFinalizeResultState>(999);
  EXPECT_FALSE(finalize.presence_valid());
}

TEST(ComponentManagerPortContracts, TransitionEvidenceRowsCannotContradictLocalPhase) {
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  auto evidence = TransitionEvidence();
  auto staged_health = HealthyProjection(candidate);
  staged_health.active_projection_revision.reset();
  staged_health.staged_candidate = ProjectionStagedCandidateDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{1U},
      candidate,
  };
  evidence.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          NativeBinding(),
          ProjectionTransitionDisposition::kAdded,
          std::nullopt,
      },
      ProjectionTransitionRowEvidenceState::kCandidateActive,
      candidate,
      std::nullopt,
      staged_health,
      std::nullopt,
  });

  EXPECT_FALSE(evidence.phase_consistent(ProjectionTransitionEvidenceState::kStaged));
  auto& staged_row = evidence.transition_rows.front();
  staged_row.state = ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect;
  EXPECT_TRUE(evidence.phase_consistent(ProjectionTransitionEvidenceState::kStaged));

  staged_row.resulting_health.active_projection_revision = candidate;
  EXPECT_FALSE(evidence.phase_consistent(ProjectionTransitionEvidenceState::kStaged));
  staged_row.resulting_health.active_projection_revision.reset();
  staged_row.resulting_health.state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(evidence.phase_consistent(ProjectionTransitionEvidenceState::kStaged));

  const auto predecessor = Projection();
  auto aborted = TransitionEvidence(ProjectionTransitionEvidenceState::kAborted);
  aborted.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          NativeBinding(),
          ProjectionTransitionDisposition::kRemoved,
          predecessor,
      },
      ProjectionTransitionRowEvidenceState::kPredecessorUnchanged,
      candidate,
      predecessor,
      HealthyProjection(predecessor),
      std::nullopt,
  });
  EXPECT_FALSE(aborted.phase_consistent(ProjectionTransitionEvidenceState::kAborted));
  aborted.transition_rows.front().acknowledged_staged_candidate_revision.reset();
  EXPECT_TRUE(aborted.phase_consistent(ProjectionTransitionEvidenceState::kAborted));

  auto uncertain = TransitionEvidence(ProjectionTransitionEvidenceState::kUncertain);
  ASSERT_TRUE(uncertain.valid());
  uncertain.error.reset();
  EXPECT_FALSE(uncertain.valid());

  auto finalized = FinalizedTransitionEvidence();
  ASSERT_TRUE(finalized.valid());
  finalized.transition_rows.back().resulting_health.staged_candidate = ProjectionStagedCandidateDTO{
      finalized.apply_id,
      finalized.apply_generation,
      finalized.projection_stage_generation,
      finalized.candidate_projection_revision,
  };
  EXPECT_FALSE(finalized.valid());
}

TEST(ComponentManagerPortContracts, TransitionInputAndEvidenceRowsRequireCanonicalUniqueBindings) {
  const ProjectionTransitionRowDTO first{
      NativeBinding("provider-binding.a", "runtime.a"),
      ProjectionTransitionDisposition::kAdded,
      std::nullopt,
  };
  const ProjectionTransitionRowDTO second{
      NativeBinding("provider-binding.b", "runtime.b"),
      ProjectionTransitionDisposition::kAdded,
      std::nullopt,
  };
  std::vector<ProjectionTransitionRowDTO> input_rows{first, second};
  EXPECT_TRUE(CanonicalProjectionTransitionRows(input_rows));
  std::swap(input_rows[0], input_rows[1]);
  EXPECT_FALSE(CanonicalProjectionTransitionRows(input_rows));
  input_rows = {first, first};
  EXPECT_FALSE(CanonicalProjectionTransitionRows(input_rows));

  auto evidence = TransitionEvidence();
  const ProjectionStagedCandidateDTO staged_candidate{
      evidence.apply_id,
      evidence.apply_generation,
      evidence.projection_stage_generation,
      evidence.candidate_projection_revision,
  };
  const auto evidence_row = [&evidence,
                             &staged_candidate](const ProjectionTransitionRowDTO& transition_row) {
    return ProjectionTransitionRowEvidenceDTO{
        transition_row,
        ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect,
        evidence.candidate_projection_revision,
        transition_row.expected_local_active,
        ProjectionHealthDTO{
            transition_row.native_runtime_binding,
            1U,
            ProjectionHealthState::kUninitialized,
            transition_row.expected_local_active,
            staged_candidate,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        },
        std::nullopt,
    };
  };
  evidence.transition_rows = {
      evidence_row(first),
      evidence_row(second),
  };
  EXPECT_TRUE(evidence.valid());
  std::swap(evidence.transition_rows[0], evidence.transition_rows[1]);
  EXPECT_FALSE(evidence.valid());
  evidence.transition_rows = {
      evidence_row(first),
      evidence_row(first),
  };
  EXPECT_FALSE(evidence.valid());
}

TEST(ComponentManagerPortContracts,
     ActiveRuntimeRecoverySetIsCanonicalAndGenerationExact) {
  const auto target = Projection();
  const auto first = NativeBinding("provider-binding.a", "runtime.a");
  const auto second = NativeBinding("provider-binding.b", "runtime.b");
  ActiveProjectionRuntimeSetDTO runtime_set{target, {first, second}};
  ASSERT_TRUE(runtime_set.valid());

  std::swap(runtime_set.native_runtime_bindings[0],
            runtime_set.native_runtime_bindings[1]);
  EXPECT_FALSE(runtime_set.valid());
  runtime_set.native_runtime_bindings = {first, first};
  EXPECT_FALSE(runtime_set.valid());
  auto replacement_epoch = first;
  replacement_epoch.runtime_epoch = RuntimeEpoch("runtime-epoch-2");
  runtime_set.native_runtime_bindings = {first, replacement_epoch};
  EXPECT_FALSE(runtime_set.valid());
  auto replacement_runtime = first;
  replacement_runtime.runtime_unit_id = RuntimeUnitId("runtime.z");
  runtime_set.native_runtime_bindings = {first, replacement_runtime};
  EXPECT_FALSE(runtime_set.valid());

  runtime_set.native_runtime_bindings = {first, second};
  EXPECT_TRUE((ActiveProjectionRuntimeRecoveryResultDTO{
                  runtime_set,
                  ActiveProjectionRuntimeRecoveryState::kCurrent,
                  std::nullopt,
              }
                   .valid()));
  EXPECT_FALSE((ActiveProjectionRuntimeRecoveryResultDTO{
                   runtime_set,
                   ActiveProjectionRuntimeRecoveryState::kRecovered,
                   std::nullopt,
               }
                    .valid()));
  EXPECT_TRUE((ActiveProjectionRuntimeRecoveryResultDTO{
                  runtime_set,
                  ActiveProjectionRuntimeRecoveryState::kRecovered,
                  NativeProjectionRecoveryGeneration(1U),
              }
                   .valid()));
  EXPECT_FALSE((ActiveProjectionRuntimeRecoveryResultDTO{
                   runtime_set,
                   ActiveProjectionRuntimeRecoveryState::kCurrent,
                   NativeProjectionRecoveryGeneration(0U),
               }
                    .valid()));
}

TEST(ComponentManagerPortContracts, TransitionDispositionRequiresTheExactPredecessor) {
  const auto predecessor = Projection();
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  const auto binding = NativeBinding();

  ProjectionTransitionRowDTO row{
      binding,
      ProjectionTransitionDisposition::kAdded,
      std::nullopt,
  };
  EXPECT_TRUE(row.valid());
  EXPECT_TRUE(row.matches_predecessor(predecessor));
  row.expected_local_active = predecessor;
  EXPECT_FALSE(row.valid());

  row = ProjectionTransitionRowDTO{
      binding,
      ProjectionTransitionDisposition::kRetained,
      std::nullopt,
  };
  EXPECT_FALSE(row.valid());
  row.expected_local_active = candidate;
  EXPECT_TRUE(row.valid());
  EXPECT_FALSE(row.matches_predecessor(predecessor));
  row.expected_local_active = predecessor;
  EXPECT_TRUE(row.matches_predecessor(predecessor));

  row.disposition = ProjectionTransitionDisposition::kRemoved;
  EXPECT_TRUE(row.matches_predecessor(predecessor));
  row.expected_local_active.reset();
  EXPECT_FALSE(row.valid());

  ProjectionApplyStageDTO stage{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      predecessor,
      RetainedBundle(predecessor),
      RetainedBundle(candidate),
      {{
          binding,
          ProjectionTransitionDisposition::kRetained,
          predecessor,
      }},
  };
  EXPECT_TRUE(stage.valid());
  stage.native_transition_rows.front().expected_local_active = candidate;
  EXPECT_FALSE(stage.valid());

  auto evidence = TransitionEvidence();
  auto health = HealthyProjection(predecessor);
  health.staged_candidate = ProjectionStagedCandidateDTO{
      evidence.apply_id,
      evidence.apply_generation,
      evidence.projection_stage_generation,
      candidate,
  };
  evidence.transition_rows.push_back(ProjectionTransitionRowEvidenceDTO{
      ProjectionTransitionRowDTO{
          binding,
          ProjectionTransitionDisposition::kRetained,
          predecessor,
      },
      ProjectionTransitionRowEvidenceState::kStagedNoActiveEffect,
      candidate,
      predecessor,
      health,
      std::nullopt,
  });
  EXPECT_TRUE(evidence.valid());
  evidence.transition_rows.front().transition_row.expected_local_active = candidate;
  EXPECT_FALSE(evidence.structurally_valid());
}

TEST(ComponentManagerPortContracts, AggregateHealthForTargetRejectsContradictionsAndBadOrder) {
  const auto target = Projection(kOtherDigest, kOtherDigest, 2U);
  auto aggregate = NamedAggregateHealth();
  ASSERT_TRUE(aggregate.healthy_for_target(target));

  aggregate.state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.error = ErrorDTO{"projection_failed", "aggregate error", {}};
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.front().state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.front().error = ErrorDTO{"projection_failed", "row error", {}};
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.front().active_projection_revision = Projection();
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.front().staged_candidate = ProjectionStagedCandidateDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      ProjectionStageGeneration{1U},
      target,
  };
  EXPECT_FALSE(aggregate.healthy_for_target(target));

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.push_back(aggregate.native_projection_health.front());
  EXPECT_FALSE(aggregate.valid());

  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health = {
      HealthyProjection(target, NativeBinding("provider-binding.z", "runtime.z")),
      HealthyProjection(target, NativeBinding("provider-binding.a", "runtime.a")),
  };
  EXPECT_FALSE(aggregate.valid());

  aggregate = NamedAggregateHealth();
  aggregate.state = static_cast<ProjectionHealthState>(999);
  EXPECT_FALSE(aggregate.valid());
  aggregate = NamedAggregateHealth();
  aggregate.native_projection_health.front().state = static_cast<ProjectionHealthState>(999);
  EXPECT_FALSE(aggregate.valid());
}

TEST(ComponentManagerPortContracts, AggregateBindingEqualityRejectsOmissionExtraAndSubstitution) {
  const auto target = Projection(kOtherDigest, kOtherDigest, 2U);
  const auto expected = NamedAggregateHealth();
  auto observed = expected;
  observed.native_projection_health.front().source_sequence = 99U;
  EXPECT_TRUE(EqualCanonicalProjectionHealthBindings(expected, observed));

  observed.native_projection_health.pop_back();
  EXPECT_FALSE(EqualCanonicalProjectionHealthBindings(expected, observed));

  observed = expected;
  observed.native_projection_health.push_back(
      HealthyProjection(target, NativeBinding("provider-binding.z", "runtime.z")));
  EXPECT_FALSE(EqualCanonicalProjectionHealthBindings(expected, observed));

  observed = expected;
  observed.native_projection_health.front().native_runtime_binding =
      NativeBinding("provider-binding.aa", "runtime.aa");
  ASSERT_TRUE(observed.valid());
  EXPECT_FALSE(EqualCanonicalProjectionHealthBindings(expected, observed));
}

TEST(ComponentManagerPortContracts, ApplyStatusPresenceIsClosedForPublishedFinalizedAndDiscarded) {
  const auto predecessor = Projection();
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  const auto receipt = NamedPublicationReceipt();

  ProjectionApplyStatusDTO status{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      predecessor,
      ProjectionStageGeneration{1U},
      candidate,
      ProjectionApplyStatusState::kPublished,
      TransitionEvidence(),
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_FALSE(status.presence_valid());

  status.publication_receipt = receipt;
  EXPECT_FALSE(status.presence_valid());
  status.transition_evidence =
      TransitionEvidence(ProjectionTransitionEvidenceState::kActivatedAndRetired);
  EXPECT_TRUE(status.presence_valid());

  status.state = ProjectionApplyStatusState::kFinalized;
  EXPECT_FALSE(status.presence_valid());
  status.transition_evidence = FinalizedTransitionEvidence();
  EXPECT_FALSE(status.presence_valid());
  status.candidate_aggregate_health = NamedAggregateHealth();
  EXPECT_TRUE(status.presence_valid());
  status.candidate_aggregate_health->native_projection_health.pop_back();
  EXPECT_FALSE(status.presence_valid());
  status.candidate_aggregate_health = NamedAggregateHealth();
  status.candidate_aggregate_health->state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(status.presence_valid());

  status.state = ProjectionApplyStatusState::kStaged;
  EXPECT_FALSE(status.presence_valid());

  status = ProjectionApplyStatusDTO{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      predecessor,
      ProjectionStageGeneration{1U},
      candidate,
      ProjectionApplyStatusState::kStaged,
      TransitionEvidence(),
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(status.presence_valid());
  status.state = ProjectionApplyStatusState::kActivated;
  EXPECT_FALSE(status.presence_valid());

  status = ProjectionApplyStatusDTO{
      ApplyId{"apply-1"}, ApplyGeneration{1U},
      predecessor,        ProjectionStageGeneration{1U},
      std::nullopt,       ProjectionApplyStatusState::kDiscarded,
      std::nullopt,       std::nullopt,
      std::nullopt,       std::nullopt,
  };
  EXPECT_FALSE(status.presence_valid());
  status.candidate_projection_revision = candidate;
  status.transition_evidence = TransitionEvidence(ProjectionTransitionEvidenceState::kAborted);
  EXPECT_TRUE(status.presence_valid());
}

TEST(ComponentManagerPortContracts, ResultPresenceRejectsFabricatedSuccessEvidence) {
  const auto predecessor = Projection();
  const auto candidate = Projection(kOtherDigest, kOtherDigest, 2U);
  const ErrorDTO error{"projection_failed", "test failure", {}};

  ProjectionApplyActivationResultDTO activation{
      ApplyId{"apply-1"},
      ApplyGeneration{1U},
      predecessor,
      ProjectionStageGeneration{1U},
      candidate,
      ProjectionApplyActivationResultState::kActivated,
      candidate,
      TransitionEvidence(ProjectionTransitionEvidenceState::kActivatedAndRetired),
      std::nullopt,
  };
  EXPECT_TRUE(activation.presence_valid());
  activation.state = ProjectionApplyActivationResultState::kStale;
  activation.error = error;
  EXPECT_FALSE(activation.presence_valid());

  ProjectionBootstrapRebuildResultDTO rebuild{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      Projection(),
      ProjectionBootstrapRebuildResultState::kReadyToPublish,
      BootstrapEvidence(),
      std::nullopt,
  };
  EXPECT_TRUE(rebuild.presence_valid());
  rebuild.bootstrap_evidence->candidate_aggregate_health.native_projection_health.push_back(
      HealthyProjection(Projection()));
  EXPECT_FALSE(rebuild.presence_valid());
  rebuild.bootstrap_evidence = BootstrapEvidence();
  rebuild.bootstrap_evidence->candidate_aggregate_health.state =
      ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(rebuild.presence_valid());
  rebuild.bootstrap_evidence = BootstrapEvidence();
  rebuild.state = ProjectionBootstrapRebuildResultState::kFailedProjectionUncertain;
  rebuild.error = error;
  EXPECT_FALSE(rebuild.presence_valid());

  ProjectionBootstrapFinalizeResultDTO finalize{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      Projection(),
      BootstrapPublicationReceipt(),
      ProjectionBootstrapFinalizeResultState::kFinalized,
      BootstrapEvidence(),
      BootstrapAggregateHealth(),
      std::nullopt,
  };
  EXPECT_TRUE(finalize.presence_valid());
  finalize.candidate_aggregate_health->native_projection_health.push_back(
      HealthyProjection(Projection(), NativeBinding("provider-binding.z", "runtime.z")));
  EXPECT_FALSE(finalize.presence_valid());
  finalize.candidate_aggregate_health = BootstrapAggregateHealth();
  finalize.candidate_aggregate_health->state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(finalize.presence_valid());
  finalize.candidate_aggregate_health = BootstrapAggregateHealth();
  finalize.state = ProjectionBootstrapFinalizeResultState::kStale;
  finalize.error = error;
  EXPECT_FALSE(finalize.presence_valid());
}

TEST(ComponentManagerPortContracts, LogicalSceneRootRejectsNull) {
  EXPECT_THROW(LogicalSceneSnapshotRoot{std::shared_ptr<const LogicalSceneSnapshotDTO>{}},
               std::invalid_argument);
}

TEST(ComponentManagerPortContracts, BootstrapPrerequisiteQueryRequiresEveryAdmissionIdentity) {
  ProjectionBootstrapPrerequisiteQueryDTO query{
      CellId{"cell-a"},
      RetainedBundleId{kDigest},
      GraphRevision{kOtherDigest},
  };
  EXPECT_TRUE(query.valid());

  query.cell_id = CellId{""};
  EXPECT_FALSE(query.valid());
  query.cell_id = CellId{"cell-a"};
  query.canonical_empty_retained_bundle_id = RetainedBundleId{""};
  EXPECT_FALSE(query.valid());
  query.canonical_empty_retained_bundle_id = RetainedBundleId{kDigest};
  query.canonical_empty_graph_revision = GraphRevision{""};
  EXPECT_FALSE(query.valid());
}

TEST(ComponentManagerPortContracts,
     BootstrapPrerequisitePresenceExhaustsTheClosedStateCombinations) {
  const ProjectionBootstrapPrerequisiteQueryDTO query{
      CellId{"cell-a"},
      RetainedBundleId{kDigest},
      GraphRevision{kOtherDigest},
  };
  const ErrorDTO error{
      "bootstrap_prerequisite_unavailable",
      "test prerequisite failure",
      {},
  };
  for (const auto state : {ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
                           ProjectionBootstrapPrerequisiteState::kUnavailable,
                           ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection}) {
    for (const bool scene_writes_closed : {false, true}) {
      for (const bool projection_leases_closed : {false, true}) {
        for (const bool has_active_projection : {false, true}) {
          for (const bool has_error : {false, true}) {
            SCOPED_TRACE(static_cast<int>(state));
            SCOPED_TRACE(scene_writes_closed);
            SCOPED_TRACE(projection_leases_closed);
            SCOPED_TRACE(has_active_projection);
            SCOPED_TRACE(has_error);
            ProjectionBootstrapPrerequisiteStatusDTO status{
                query,
                state,
                scene_writes_closed,
                projection_leases_closed,
                has_active_projection ? std::optional<ProjectionRevisionDTO>{Projection()}
                                      : std::nullopt,
                has_error ? std::optional<ErrorDTO>{error} : std::nullopt,
            };

            bool expected = false;
            switch (state) {
              case ProjectionBootstrapPrerequisiteState::kReadyCommandFenced:
                expected = scene_writes_closed && projection_leases_closed &&
                           !has_active_projection && !has_error;
                break;
              case ProjectionBootstrapPrerequisiteState::kUnavailable:
                expected = !has_active_projection && has_error;
                break;
              case ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection:
                expected = has_active_projection && has_error;
                break;
            }
            EXPECT_EQ(status.presence_valid(), expected);
            EXPECT_EQ(
                status.ready_for(query),
                expected && state == ProjectionBootstrapPrerequisiteState::kReadyCommandFenced);
          }
        }
      }
    }
  }
}

TEST(ComponentManagerPortContracts, BootstrapPrerequisiteReadyForRequiresAnExactEchoAndKnownState) {
  const ProjectionBootstrapPrerequisiteQueryDTO query{
      CellId{"cell-a"},
      RetainedBundleId{kDigest},
      GraphRevision{kOtherDigest},
  };
  ProjectionBootstrapPrerequisiteStatusDTO status{
      query,        ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
      true,         true,
      std::nullopt, std::nullopt,
  };
  ASSERT_TRUE(status.presence_valid());
  EXPECT_TRUE(status.ready_for(query));

  auto mismatch = query;
  mismatch.cell_id = CellId{"cell-b"};
  EXPECT_FALSE(status.ready_for(mismatch));
  mismatch = query;
  mismatch.canonical_empty_retained_bundle_id = RetainedBundleId{kOtherDigest};
  EXPECT_FALSE(status.ready_for(mismatch));
  mismatch = query;
  mismatch.canonical_empty_graph_revision = GraphRevision{kDigest};
  EXPECT_FALSE(status.ready_for(mismatch));

  status.query.cell_id = CellId{"cell-b"};
  ASSERT_TRUE(status.presence_valid());
  EXPECT_FALSE(status.ready_for(query));
  status.query = query;

  status.active_projection = Projection();
  status.state = ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection;
  status.error = ErrorDTO{"bootstrap_projection_conflict", "test conflict", {}};
  ASSERT_TRUE(status.presence_valid());
  status.active_projection->retained_bundle_id = RetainedBundleId{""};
  EXPECT_FALSE(status.presence_valid());

  status.active_projection.reset();
  status.state = static_cast<ProjectionBootstrapPrerequisiteState>(999);
  EXPECT_FALSE(status.presence_valid());
  EXPECT_FALSE(status.ready_for(query));
}

TEST(ComponentManagerPortContracts,
     BootstrapStatusCarriesPartialObservationsWithoutCompleteEvidence) {
  auto absent_observation = BootstrapNativeStatus(NativeProjectionBootstrapStatusState::kAbsent);
  ASSERT_TRUE(absent_observation.presence_valid());
  absent_observation.resulting_health.state = ProjectionHealthState::kHealthy;
  absent_observation.resulting_health.active_projection_revision = Projection();
  EXPECT_FALSE(absent_observation.presence_valid());

  auto rebuilding_observation =
      BootstrapNativeStatus(NativeProjectionBootstrapStatusState::kRebuilding);
  ASSERT_TRUE(rebuilding_observation.presence_valid());
  rebuilding_observation.resulting_health.active_projection_revision = Projection();
  EXPECT_FALSE(rebuilding_observation.presence_valid());

  ProjectionBootstrapStatusDTO rebuilding{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      ProjectionBootstrapStatusState::kRebuilding,
      Projection(),
      {BootstrapNativeStatus(NativeProjectionBootstrapStatusState::kRebuilding)},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(rebuilding.presence_valid());

  rebuilding.state = ProjectionBootstrapStatusState::kFailedProjectionUncertain;
  rebuilding.error = ErrorDTO{"projection_failed", "bootstrap uncertain", {}};
  EXPECT_TRUE(rebuilding.presence_valid());

  rebuilding.bootstrap_evidence = BootstrapEvidence();
  EXPECT_FALSE(rebuilding.presence_valid());
}

TEST(ComponentManagerPortContracts, BootstrapStatusPresenceIsClosedByPhase) {
  const ProjectionBootstrapStatusDTO absent{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      ProjectionBootstrapStatusState::kAbsent,
      std::nullopt,
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(absent.presence_valid());

  const ProjectionBootstrapStatusDTO invalid_published{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      ProjectionBootstrapStatusState::kPublished,
      Projection(),
      {},
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_FALSE(invalid_published.presence_valid());

  ProjectionBootstrapStatusDTO ready{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      ProjectionBootstrapStatusState::kReadyToPublish,
      Projection(),
      {},
      BootstrapEvidence(),
      std::nullopt,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(ready.presence_valid());
  ready.bootstrap_evidence->candidate_aggregate_health.error =
      ErrorDTO{"projection_failed", "aggregate error", {}};
  EXPECT_FALSE(ready.presence_valid());

  ProjectionBootstrapStatusDTO finalized{
      BootstrapId{"bootstrap-1"},
      BootstrapGeneration{1U},
      ProjectionBootstrapStatusState::kFinalized,
      Projection(),
      {},
      BootstrapEvidence(),
      BootstrapPublicationReceipt(),
      BootstrapAggregateHealth(),
      std::nullopt,
  };
  EXPECT_TRUE(finalized.presence_valid());
  finalized.finalized_aggregate_health->native_projection_health.push_back(
      HealthyProjection(Projection(), NativeBinding("provider-binding.z", "runtime.z")));
  EXPECT_FALSE(finalized.presence_valid());
  finalized.finalized_aggregate_health = BootstrapAggregateHealth();
  finalized.finalized_aggregate_health->state = ProjectionHealthState::kUnavailable;
  EXPECT_FALSE(finalized.presence_valid());
  finalized.state = static_cast<ProjectionBootstrapStatusState>(999);
  EXPECT_FALSE(finalized.presence_valid());
}

TEST(ComponentManagerPortContracts, FenceScopeOwnerAndBundleGraphKeyRemainExplicitAndTyped) {
  auto intent = FenceIntent();

  EXPECT_TRUE(intent.valid());
  EXPECT_EQ(intent.owner_transaction.apply_id, ApplyId{"apply-1"});
  EXPECT_EQ(intent.expected_retained_bundle_id, RetainedBundleId{kDigest});
  EXPECT_EQ(intent.expected_graph_revision, GraphRevision{kOtherDigest});

  intent.expected_retained_bundle_id = RetainedBundleId{""};
  EXPECT_FALSE(intent.valid());

  intent.expected_retained_bundle_id = RetainedBundleId{kDigest};
  intent.owner_transaction.apply_generation = ApplyGeneration{0U};
  EXPECT_FALSE(intent.valid());
}

TEST(ComponentManagerPortContracts, FenceScopeAndActiveConfigurationsAreCanonicalAndExact) {
  EXPECT_TRUE(ConfigurationFenceScopeDTO{}.valid());

  auto scope = FenceScope();
  scope.resources = {
      ResourceId{"arm-a-joint-2"},
      ResourceId{"arm-a-joint-1"},
  };
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.controllable_component_ids.clear();
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.domains.front().predecessor_control_domain_revision.reset();
  scope.domains.front().candidate_control_domain_revision.reset();
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.domains.push_back(scope.domains.front());
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.domains = {
      {ControlDomainId{"domain.z"}, ControlDomainRevision{kDigest}, ControlDomainRevision{kDigest}},
      {ControlDomainId{"domain.a"}, ControlDomainRevision{kDigest}, ControlDomainRevision{kDigest}},
  };
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.bindings.push_back(scope.bindings.front());
  EXPECT_FALSE(scope.valid());

  scope = FenceScope();
  scope.bindings = {
      {ControlBindingId{"binding.z"}, ControlBindingRevision{kDigest},
       ControlBindingRevision{kDigest}},
      {ControlBindingId{"binding.a"}, ControlBindingRevision{kDigest},
       ControlBindingRevision{kDigest}},
  };
  EXPECT_FALSE(scope.valid());

  auto intent = FenceIntent();
  ASSERT_TRUE(intent.valid());
  intent.expected_active_control_configurations.front().active_control_configuration_generation =
      ActiveControlConfigurationGeneration{0U};
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.busy_disposition = static_cast<ApplyFenceBusyDispositionDTO>(999);
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.scope.domains.front().predecessor_control_domain_revision.reset();
  intent.scope.bindings.front().predecessor_control_binding_revision.reset();
  intent.expected_active_control_configurations.clear();
  EXPECT_TRUE(intent.valid());
  intent.expected_active_control_configurations.push_back(
      {ControlDomainId{"domain.arm-a"}, ActiveControlConfigurationId{"active.arm-a"},
       ActiveControlConfigurationGeneration{9U}});
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.expected_active_control_configurations.front().active_control_configuration_id =
      ActiveControlConfigurationId{""};
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.expected_active_control_configurations.front().control_domain_id =
      ControlDomainId{"domain.other"};
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.expected_active_control_configurations.clear();
  EXPECT_FALSE(intent.valid());

  intent = FenceIntent();
  intent.scope.domains = {
      {ControlDomainId{"domain.arm-a"}, ControlDomainRevision{kDigest},
       ControlDomainRevision{kDigest}},
      {ControlDomainId{"domain.arm-b"}, ControlDomainRevision{kDigest},
       ControlDomainRevision{kDigest}},
  };
  intent.expected_active_control_configurations = {
      {ControlDomainId{"domain.arm-b"}, ActiveControlConfigurationId{"active.arm-b"},
       ActiveControlConfigurationGeneration{10U}},
      {ControlDomainId{"domain.arm-a"}, ActiveControlConfigurationId{"active.arm-a"},
       ActiveControlConfigurationGeneration{9U}},
  };
  EXPECT_FALSE(intent.valid());
}

TEST(ComponentManagerPortContracts, FenceAcceptedAllowsOnlyAcceptedOrExactReplayStatus) {
  ConfigurationFenceAcceptedDTO accepted{
      ConfigurationFenceId{"fence-1"},      ConfigurationFenceGeneration{1U}, FenceScope(), 1,
      RequestReplayStatusDTO::kAcceptedNew,
  };
  EXPECT_TRUE(accepted.valid());
  accepted.replay_status = RequestReplayStatusDTO::kReplayedExact;
  EXPECT_TRUE(accepted.valid());
  accepted.replay_status = RequestReplayStatusDTO::kRequestResultExpired;
  EXPECT_FALSE(accepted.valid());
  accepted.replay_status = RequestReplayStatusDTO::kRequestIdConflict;
  EXPECT_FALSE(accepted.valid());
  accepted.replay_status = RequestReplayStatusDTO::kRequestLedgerFull;
  EXPECT_FALSE(accepted.valid());
  accepted.replay_status = RequestReplayStatusDTO::kStaleRequestOwnerEpoch;
  EXPECT_FALSE(accepted.valid());
  accepted.replay_status = static_cast<RequestReplayStatusDTO>(999);
  EXPECT_FALSE(accepted.valid());
}

TEST(ComponentManagerPortContracts,
     FenceAdmissionSnapshotIsReadOnlyCanonicalPredecessorEvidence) {
  const auto query = ConfigurationFenceAdmissionQueryDTO{
      FenceScope(), RuntimeEpoch{"manager-epoch-2"}};
  auto snapshot = FenceAdmissionSnapshot();
  EXPECT_TRUE(snapshot.presence_valid());
  EXPECT_TRUE(snapshot.matches(query));

  snapshot.active_control_configurations.clear();
  EXPECT_FALSE(snapshot.presence_valid());

  snapshot = FenceAdmissionSnapshot();
  snapshot.active_control_configurations.front().control_domain_id =
      ControlDomainId{"domain.other"};
  EXPECT_FALSE(snapshot.presence_valid());

  snapshot = FenceAdmissionSnapshot();
  snapshot.echoed_query.manager_runtime_epoch = RuntimeEpoch{"manager-epoch-3"};
  EXPECT_TRUE(snapshot.presence_valid());
  EXPECT_FALSE(snapshot.matches(query));
}

TEST(ComponentManagerPortContracts, FenceAcceptanceMatchesExactScopeAndSuccessorGeneration) {
  auto intent = FenceIntent();
  ConfigurationFenceAcceptedDTO accepted{
      ConfigurationFenceId{"fence-1"},      ConfigurationFenceGeneration{1U}, FenceScope(), 1,
      RequestReplayStatusDTO::kAcceptedNew,
  };
  EXPECT_TRUE(accepted.matches(intent));

  accepted.configuration_fence_generation = ConfigurationFenceGeneration{2U};
  EXPECT_FALSE(accepted.matches(intent));
  accepted.configuration_fence_generation = ConfigurationFenceGeneration{1U};
  accepted.captured_scope.resources = {ResourceId{"different-resource"}};
  EXPECT_FALSE(accepted.matches(intent));

  intent.expected_fence_generation =
      ConfigurationFenceGeneration{std::numeric_limits<std::uint64_t>::max()};
  accepted.configuration_fence_generation = ConfigurationFenceGeneration{0U};
  EXPECT_FALSE(accepted.matches(intent));
}

TEST(ComponentManagerPortContracts, FenceReleaseRequiresExactReconciledIdleCandidateState) {
  auto evidence = FenceReleaseEvidence();
  EXPECT_TRUE(evidence.valid());
  auto release = FenceReleaseRequest();
  EXPECT_TRUE(release.valid());

  evidence.active_graph_revision = GraphRevision{kDigest};
  EXPECT_FALSE(evidence.valid());
  evidence = FenceReleaseEvidence();
  evidence.execution_host_epoch.publication_generation = ActiveBundlePublicationGeneration{3U};
  EXPECT_FALSE(evidence.valid());
  evidence = FenceReleaseEvidence();
  evidence.active_control_configurations.clear();
  EXPECT_FALSE(evidence.valid());
  evidence = FenceReleaseEvidence();
  evidence.affected_active_operation_count = 1U;
  EXPECT_TRUE(evidence.valid());
  release.reconciliation_evidence = evidence;
  EXPECT_FALSE(release.valid());
}

TEST(ComponentManagerPortContracts, FenceStatusSeparatesHeldPendingAndExactReleasedEvidence) {
  const auto query = FenceStatusQuery();
  ConfigurationFenceStatusDTO status{
      query,
      FenceReleaseEvidence(),
      ConfigurationFenceState::kHeld,
      true,
      std::nullopt,
      "",
      ContentDigest{kDigest},
      std::nullopt,
  };
  EXPECT_TRUE(status.matches(query));

  status.state = ConfigurationFenceState::kReleasePending;
  EXPECT_FALSE(status.presence_valid());
  status.required_compensation_or_operator_direction =
      "keep admission fenced and reconcile the release ledger";
  status.error = ErrorDTO{"fence_release_pending", "release uncertain", {}};
  EXPECT_TRUE(status.presence_valid());

  status.state = ConfigurationFenceState::kReleased;
  status.admission_fenced = false;
  status.required_compensation_or_operator_direction.clear();
  status.error.reset();
  status.release_receipt = ConfigurationFenceReleasedDTO{
      FenceReleaseRequest(),       ExecutionHostEpoch(), 42, RequestReplayStatusDTO::kAcceptedNew,
      ContentDigest{kOtherDigest},
  };
  EXPECT_TRUE(status.presence_valid());
  status.release_receipt->request.manager_runtime_epoch = RuntimeEpoch{"foreign-manager"};
  EXPECT_TRUE(status.presence_valid());
  status.release_receipt->request.reconciliation_evidence.owner_transaction =
      ApplyFenceOwnerDTO{ApplyId{"foreign"}, ApplyGeneration{1U}};
  EXPECT_FALSE(status.presence_valid());
  status.release_receipt = ConfigurationFenceReleasedDTO{
      FenceReleaseRequest(),       ExecutionHostEpoch(), 42, RequestReplayStatusDTO::kAcceptedNew,
      ContentDigest{kOtherDigest},
  };
  status.release_receipt->request.reconciliation_evidence.active_public_pointer_digest =
      ContentDigest{kSceneIdDigest};
  EXPECT_TRUE(status.release_receipt->valid());
  EXPECT_FALSE(status.presence_valid());
  status.release_receipt = ConfigurationFenceReleasedDTO{
      FenceReleaseRequest(),       ExecutionHostEpoch(), 42, RequestReplayStatusDTO::kAcceptedNew,
      ContentDigest{kOtherDigest},
  };
  status.release_receipt->request.reconciliation_evidence.reconciled_scope.resources = {
      ResourceId{"different-resource"}};
  EXPECT_TRUE(status.release_receipt->valid());
  EXPECT_FALSE(status.presence_valid());
}

TEST(ComponentManagerPortContracts, FenceAttemptReconciliationHasOneRetryableNoResultState) {
  const auto query = FenceAttemptQuery();
  ApplyFenceAcquireAttemptReconcileResultDTO result{
      ExecutionHostEpoch(),   query,        ApplyFenceAcquireAttemptReconcileState::kNotSeen,
      std::nullopt,           false,        "",
      ContentDigest{kDigest}, std::nullopt,
  };
  EXPECT_TRUE(result.presence_valid());
  EXPECT_TRUE(result.matches(query));

  result.state = ApplyFenceAcquireAttemptReconcileState::kAcceptedExact;
  EXPECT_FALSE(result.presence_valid());
  result.accepted_fence = ConfigurationFenceAcceptedDTO{
      ConfigurationFenceId{"fence-1"},      ConfigurationFenceGeneration{1U}, FenceScope(), 1,
      RequestReplayStatusDTO::kAcceptedNew,
  };
  result.admission_fenced = true;
  EXPECT_TRUE(result.presence_valid());

  result.state = ApplyFenceAcquireAttemptReconcileState::kConflicting;
  result.accepted_fence.reset();
  EXPECT_FALSE(result.presence_valid());
  result.error = ErrorDTO{"fence_attempt_conflict", "conflict", {}};
  result.required_compensation_or_operator_direction =
      "inspect the conflicting Kernel request ledger entry";
  EXPECT_TRUE(result.presence_valid());

  result.state = ApplyFenceAcquireAttemptReconcileState::kUncertain;
  EXPECT_TRUE(result.presence_valid());
  result.admission_fenced = false;
  EXPECT_FALSE(result.presence_valid());

  result.state = static_cast<ApplyFenceAcquireAttemptReconcileState>(999);
  result.admission_fenced = true;
  EXPECT_FALSE(result.presence_valid());
}

TEST(ComponentManagerPortContracts, FenceRehydrateNewIdentityIsPresentOnlyWhenRecreated) {
  ApplyFenceReconcileEntryDTO entry{
      ConfigurationFenceId{"fence-1"},
      ConfigurationFenceGeneration{1U},
      ApplyFenceReconcileState::kMatched,
      std::nullopt,
      std::nullopt,
  };
  EXPECT_TRUE(entry.presence_valid());

  entry.state = ApplyFenceReconcileState::kRecreated;
  EXPECT_FALSE(entry.presence_valid());
  entry.reconciled_configuration_fence_id = ConfigurationFenceId{"fence-2"};
  entry.reconciled_configuration_fence_generation = ConfigurationFenceGeneration{2U};
  EXPECT_TRUE(entry.presence_valid());

  entry.state = ApplyFenceReconcileState::kConflicting;
  EXPECT_FALSE(entry.presence_valid());
  entry.reconciled_configuration_fence_id.reset();
  entry.reconciled_configuration_fence_generation.reset();
  EXPECT_TRUE(entry.presence_valid());

  entry.state = static_cast<ApplyFenceReconcileState>(999);
  EXPECT_FALSE(entry.presence_valid());
}

TEST(ComponentManagerPortContracts, FenceReconciliationCannotOpenOnMissingConflictOrError) {
  const auto request = FenceRehydrateRequest();
  const ApplyFenceReconcileEntryDTO matched{
      ConfigurationFenceId{"fence-1"},
      ConfigurationFenceGeneration{1U},
      ApplyFenceReconcileState::kMatched,
      std::nullopt,
      std::nullopt,
  };
  ApplyFenceReconcileResultDTO result{
      ExecutionHostEpoch(), {matched}, false, "", ContentDigest{kDigest}, std::nullopt,
  };
  EXPECT_TRUE(result.presence_valid());
  EXPECT_TRUE(result.matches(request));

  result.execution_host_epoch.request_owner_epoch =
      RequestOwnerEpoch{"different-kernel-owner-epoch"};
  EXPECT_TRUE(result.matches(request));

  result.fence_results.clear();
  EXPECT_FALSE(result.presence_valid());

  result.fence_results = {matched};
  result.error = ErrorDTO{"fence_reconcile_failed", "test error", {}};
  EXPECT_FALSE(result.presence_valid());

  result.fence_results.front().state = ApplyFenceReconcileState::kMissing;
  result.admission_fenced = false;
  result.required_compensation_or_operator_direction = "recreate or inspect the missing fence";
  EXPECT_FALSE(result.presence_valid());

  result.admission_fenced = true;
  EXPECT_TRUE(result.presence_valid());
  EXPECT_TRUE(result.matches(request));

  result.fence_results.front().state = ApplyFenceReconcileState::kConflicting;
  EXPECT_TRUE(result.presence_valid());
  result.error->code.clear();
  EXPECT_FALSE(result.presence_valid());
  result.error.reset();
  EXPECT_FALSE(result.presence_valid());
}

TEST(ComponentManagerPortContracts, FenceReconcileOrderingAndSingletonRequestMatchAreExplicit) {
  const auto request = FenceRehydrateRequest();
  const ApplyFenceReconcileEntryDTO first{
      ConfigurationFenceId{"fence-1"},
      ConfigurationFenceGeneration{1U},
      ApplyFenceReconcileState::kMatched,
      std::nullopt,
      std::nullopt,
  };
  const ApplyFenceReconcileEntryDTO second{
      ConfigurationFenceId{"fence-2"},
      ConfigurationFenceGeneration{1U},
      ApplyFenceReconcileState::kMatched,
      std::nullopt,
      std::nullopt,
  };
  ApplyFenceReconcileResultDTO result{
      ExecutionHostEpoch(), {second, first}, false, "", ContentDigest{kDigest}, std::nullopt,
  };
  EXPECT_FALSE(result.presence_valid());

  result.fence_results = {first, second};
  EXPECT_TRUE(result.presence_valid());
  EXPECT_FALSE(result.matches(request));

  result.fence_results = {first, first};
  EXPECT_FALSE(result.presence_valid());
}

}  // namespace
}  // namespace manyforge::core

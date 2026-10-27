#pragma once

#include <cstdint>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_core/ports/icclc_journal.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/controller_launch_group_drain.hpp>
#include <manyforge_core/types/lifecycle_target.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

enum class ManagerSelectedRuntimeCompositionPhase {
  kPrepare,
  kFinalize,
};

/// Exact Manager-to-managed-host request to install the already selected
/// product composition while its durable Kernel configuration fence is held.
/// This is a private Apply effect, not caller authorization or a command
/// session.
struct ManagerSelectedRuntimeCompositionRequestDTO final {
  manyforge::core::ContentDigest authoritative_scene_snapshot_digest;
  manyforge::core::ApplyId apply_id;
  manyforge::core::ApplyGeneration apply_generation;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration
      expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::ContentDigest retained_bundle_canonical_content_digest;
  std::uint64_t retained_bundle_canonical_byte_size{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  manyforge::core::ConfigurationFenceId configuration_fence_id;
  manyforge::core::ConfigurationFenceGeneration configuration_fence_generation;
  manyforge::core::ConfigurationFenceScopeDTO configuration_fence_scope;
  ManagerSelectedRuntimeCompositionPhase phase{
      ManagerSelectedRuntimeCompositionPhase::kFinalize};
  /// The exact per-component decision Manager persisted with the fence scope
  /// before any effect. The host changes only the non-retained components.
  ///
  /// kRetain keeps the component's controller lifetime: engines, controller
  /// connections, observed-state source, owner epoch, eligibility and
  /// lifecycle/route generations. Its product-bound participant bindings and
  /// Kernel registrations are still re-sealed to the candidate bundle and
  /// graph revision whenever the product identity changes; equal runtime
  /// identities prove that the re-seal changes no runtime-defining fact. The
  /// host derives the candidate bindings from the verified candidate bundle;
  /// the delta carries no second Deployment representation.
  manyforge::deployment::ApplyComponentDeltaDTO component_delta;
  /// The active product the retained controllers are currently sealed to.
  /// The host verifies its live registrations against it before re-sealing.
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionRequestDTO &,
             const ManagerSelectedRuntimeCompositionRequestDTO &) = default;
};

/// The host's live-owner evidence for one non-removed delta row after
/// installation. A retained component reports the predecessor runtime it
/// kept; an added or replaced one reports the runtime it installed.
struct ManagerRuntimeComponentReceiptDTO final {
  manyforge::core::ControllableComponentId controllable_component_id;
  manyforge::deployment::ApplyComponentDisposition disposition{
      manyforge::deployment::ApplyComponentDisposition::kRetain};
  std::optional<manyforge::core::ContentDigest> installed_runtime_identity;
  std::vector<manyforge::core::RuntimeUnitEpochBindingDTO> runtime_unit_epochs;
  bool predecessor_runtime_retained{false};
  /// For kRetain: the retained controller's product-bound bindings and
  /// registrations were re-sealed to the candidate product without engine
  /// reconstruction. Always false for added/replaced components, which are
  /// installed fresh against the candidate.
  bool product_binding_resealed{false};

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerRuntimeComponentReceiptDTO &,
                         const ManagerRuntimeComponentReceiptDTO &) = default;
};

/// Exact receipt check against the request: one receipt per non-removed
/// delta row in delta order, the planned identity, exactly the request's
/// managed runtime unit at its runtime epoch. Disposition echoes the authored
/// delta; retention reports the actual retained owner. A re-seal is required
/// exactly for actual retained owners when the product changes. Removed rows
/// have no receipt.
[[nodiscard]] bool ComponentReceiptsMatchRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request,
    const std::vector<ManagerRuntimeComponentReceiptDTO> &receipts) noexcept;

enum class ManagerSelectedRuntimeCompositionState {
  kPreparing,
  kInstalled,
};

struct ManagerSelectedRuntimeCompositionResultDTO final {
  ManagerSelectedRuntimeCompositionRequestDTO request;
  ManagerSelectedRuntimeCompositionState state;
  /// Canonical per-component receipts, sorted by component id. Required and
  /// checked by Manager once a finalize request reports kInstalled.
  std::vector<ManagerRuntimeComponentReceiptDTO> component_receipts;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedRuntimeCompositionResultDTO &,
             const ManagerSelectedRuntimeCompositionResultDTO &) = default;
};

/// Private selected Enable expectations. CCLC owns transaction/claim/fence
/// identities; these references and epochs never authorize an effect.
struct ManagerSelectedComponentBringUpRequestDTO final {
  manyforge::core::ComponentLifecycleIntentDTO intent;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedComponentBringUpRequestDTO &,
             const ManagerSelectedComponentBringUpRequestDTO &) = default;
};

struct ManagerSelectedComponentBringUpReadRequestDTO final {
  ManagerSelectedComponentBringUpRequestDTO request;

  [[nodiscard]] bool valid() const noexcept;

  friend bool
  operator==(const ManagerSelectedComponentBringUpReadRequestDTO &,
             const ManagerSelectedComponentBringUpReadRequestDTO &) = default;
};

struct ManagerSelectedComponentBringUpResultDTO final {
  ManagerSelectedComponentBringUpRequestDTO request;
  /// Absence is an unknown READ only. The SUBMIT caller/dispatcher must
  /// require a present result; absence never means unsupported or accepted.
  std::optional<manyforge::core::CCLCLifecycleRequestResultDTO> result;

  [[nodiscard]] bool valid() const;
};

/// Private Prepare/Enable delegation by an existing Apply parent. These
/// values are evidence references, not proof of a live held continuation.
struct ManagerApplyComponentBringUpRequestDTO final {
  manyforge::core::ComponentLifecycleIntentDTO intent;
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  manyforge::core::ApplyComponentBringUpAuthorityDTO parent_authority;

  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyComponentBringUpRequestDTO &,
                         const ManagerApplyComponentBringUpRequestDTO &) = default;
};

struct ManagerApplyComponentBringUpReadRequestDTO final {
  ManagerApplyComponentBringUpRequestDTO request;
  [[nodiscard]] bool valid() const noexcept;

  friend bool operator==(const ManagerApplyComponentBringUpReadRequestDTO &,
                         const ManagerApplyComponentBringUpReadRequestDTO &) = default;
};

struct ManagerApplyComponentBringUpResultDTO final {
  ManagerApplyComponentBringUpRequestDTO request;
  /// Absence is an unknown READ only; SUBMIT must require a present result.
  std::optional<manyforge::core::CCLCLifecycleRequestResultDTO> result;
  [[nodiscard]] bool valid() const;
};

/// Read-only candidate preflight. No Apply or CCLC identity is minted here.
/// Manager binds the genuine predecessor and original process epochs; the host
/// verifies the whole candidate and reports actual component process presence.
/// Current installed-owner read; independent of any candidate component list.
struct ManagerComponentProcessInventoryRequestDTO final {
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration
      expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  /// Apply-only observation; ordinary Close/process inventory is unchanged.
  bool include_component_lifecycles{false};
  [[nodiscard]] bool valid() const noexcept;
  friend bool
  operator==(const ManagerComponentProcessInventoryRequestDTO &,
             const ManagerComponentProcessInventoryRequestDTO &) = default;
};

/// Physical launch remains the default. The local kind is observation of an
/// actually installed strict pneumatic+None owner; it is never inferred from
/// missing process/feedback or candidate mode. Independent host/Manager joins
/// against the immutable predecessor remain required before any policy use.
enum class ManagerComponentProcessOwnerKindDTO : std::uint8_t {
  kControllerLaunch = 0,
  kPneumaticNoneLocal = 1,
};

struct ManagerPneumaticNoneProcessOwnerDTO final {
  manyforge::core::CommandTargetId command_target_id;
  manyforge::core::ControlBindingId control_binding_id;
  manyforge::core::ControlBindingRevision control_binding_revision;
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerPneumaticNoneProcessOwnerDTO &,
                         const ManagerPneumaticNoneProcessOwnerDTO &) = default;
};

struct ManagerComponentProcessOccurrenceDTO final {
  manyforge::core::ControllableComponentId controllable_component_id;
  std::string launch_identity;
  std::int32_t leader_pid{};
  /// Exact leader observation only; Absent alone is not a group-drain proof.
  ProcessPresenceState process_presence{ProcessPresenceState::kUnknown};
  manyforge::core::ControllerLaunchCleanupState cleanup_state{
      manyforge::core::ControllerLaunchCleanupState::kNotRequested};
  std::optional<manyforge::core::ControllerLaunchGroupDrainReceipt> receipt{};
  std::optional<manyforge::core::ContentDigest> installed_runtime_identity{};
  ManagerComponentProcessOwnerKindDTO process_owner_kind{
      ManagerComponentProcessOwnerKindDTO::kControllerLaunch};
  std::optional<ManagerPneumaticNoneProcessOwnerDTO> pneumatic_none_owner{};
  [[nodiscard]] bool valid() const noexcept;
  friend bool
  operator==(const ManagerComponentProcessOccurrenceDTO &,
             const ManagerComponentProcessOccurrenceDTO &) = default;
};

/// Uses the existing MFNSRC4 collection bound, never a truncated union.
inline constexpr std::size_t kMaximumComponentProcessInventoryEntries = 4096U;
/// Current installed CCLC facts, bound to the inventory's exact predecessor.
/// These are observation only, never permission to deactivate or replace.
struct ManagerComponentLifecycleObservationDTO final {
  manyforge::core::ControllableComponentId controllable_component_id;
  manyforge::core::ComponentLifecycleGeneration generation;
  manyforge::core::ComponentLifecycleStateDTO state;
  friend bool
  operator==(const ManagerComponentLifecycleObservationDTO &,
             const ManagerComponentLifecycleObservationDTO &) = default;
};
struct ManagerComponentProcessInventoryResultDTO final {
  ManagerComponentProcessInventoryRequestDTO request;
  std::vector<ManagerComponentProcessOccurrenceDTO> process_occurrences;
  /// Retains observed rows but makes completeness/absence UNKNOWN.
  std::optional<manyforge::core::ErrorDTO> error{};
  std::vector<ManagerComponentLifecycleObservationDTO> component_lifecycles{};
  [[nodiscard]] bool valid() const noexcept;
  friend bool
  operator==(const ManagerComponentProcessInventoryResultDTO &,
             const ManagerComponentProcessInventoryResultDTO &) = default;
};

struct ManagerComponentBringUpInspectionRequestDTO final {
  manyforge::core::ProjectionRevisionDTO candidate_projection_revision;
  manyforge::core::ContentDigest candidate_retained_bundle_digest;
  std::uint64_t candidate_retained_bundle_bytes{};
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  /// Read-only facts for exactly this verified candidate component, when requested.
  std::optional<manyforge::core::ControllableComponentId> planning_component_id{};
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerComponentBringUpInspectionRequestDTO &,
                         const ManagerComponentBringUpInspectionRequestDTO &) = default;
};

struct ManagerComponentProcessObservationDTO final {
  manyforge::core::ControllableComponentId controllable_component_id;
  ProcessPresenceState process_presence{ProcessPresenceState::kUnknown};
  ManagerComponentProcessOwnerKindDTO process_owner_kind{
      ManagerComponentProcessOwnerKindDTO::kControllerLaunch};
  std::optional<ManagerPneumaticNoneProcessOwnerDTO> pneumatic_none_owner{};
  /// Actual installed predecessor identity only; never a candidate promise.
  /// Required for local None, absent for physical observations.
  std::optional<manyforge::core::ContentDigest> installed_runtime_identity{};
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerComponentProcessObservationDTO &,
                         const ManagerComponentProcessObservationDTO &) = default;
};

/// Original owner preview and verified candidate compiler input only. This is
/// neither installed readback nor parent/fence/acceptance authority.
struct ManagerComponentBringUpPlanningFactsDTO final {
  manyforge::core::ControllableComponentId component_id;
  manyforge::core::RequestOwnerEpoch owner_epoch;
  manyforge::core::ApplyComponentGenerationSelectionDTO generation_selection;
  std::vector<manyforge::core::ExpectedControlDomainRevisionDTO> candidate_expected_domains;
  /// The actual requested target and its next permitted action are distinct
  /// from that action's postcondition and do not assert readiness.
  manyforge::core::LifecycleTargetStateDTO requested_target;
  manyforge::core::ComponentLifecycleIntentKindDTO next_intent;
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerComponentBringUpPlanningFactsDTO &,
                         const ManagerComponentBringUpPlanningFactsDTO &) = default;
};

struct ManagerComponentBringUpInspectionResultDTO final {
  ManagerComponentBringUpInspectionRequestDTO request;
  /// Exactly the verified candidate's configured components, sorted by ID.
  /// Presence is observation only; it grants no lifecycle or effect permission.
  std::vector<ManagerComponentProcessObservationDTO> configured_components;
  std::optional<ManagerComponentBringUpPlanningFactsDTO> planning_facts{};
  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerComponentBringUpInspectionResultDTO &,
                         const ManagerComponentBringUpInspectionResultDTO &) = default;
};

/// Operator request for the exact predecessor launch occurrence. The existing
/// host publication and Manager epochs authenticate the request before effects.
struct ManagerComponentProcessStopRequestDTO final {
  manyforge::core::PublicRequestContextDTO request_context;
  manyforge::core::ProjectionRevisionDTO predecessor_projection_revision;
  manyforge::core::ActiveBundlePublicationGeneration expected_predecessor_publication_generation;
  manyforge::core::ContentDigest expected_predecessor_public_pointer_digest;
  manyforge::core::RuntimeUnitId runtime_unit_id;
  manyforge::core::RuntimeEpoch runtime_epoch;
  manyforge::core::RuntimeEpoch manager_runtime_epoch;
  manyforge::core::ControllableComponentId controllable_component_id;
  std::string launch_identity;
  std::int32_t leader_pid;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const ManagerComponentProcessStopRequestDTO &,
                         const ManagerComponentProcessStopRequestDTO &) = default;
};

struct ManagerComponentProcessStopReadRequestDTO final {
  ManagerComponentProcessStopRequestDTO request;
  [[nodiscard]] bool valid() const noexcept { return request.valid(); }
  friend bool operator==(const ManagerComponentProcessStopReadRequestDTO &,
                         const ManagerComponentProcessStopReadRequestDTO &) = default;
};

/// Read-only cached cleanup state from the pinned original process owner.
/// Absent process/leader exit alone never grants kDrained.
struct ManagerComponentProcessStopResultDTO final {
  ManagerComponentProcessStopRequestDTO request;
  manyforge::core::ControllerLaunchCleanupState state;
  std::optional<manyforge::core::ControllerLaunchGroupDrainReceipt> receipt{};
  std::optional<manyforge::core::ErrorDTO> error{};
  [[nodiscard]] bool valid() const noexcept;
};

class IManagerSelectedRuntimeCompositionPort {
public:
  virtual ~IManagerSelectedRuntimeCompositionPort() = default;

  [[nodiscard]] virtual manyforge::core::Result<
      ManagerComponentProcessInventoryResultDTO>
  InspectInstalledComponentProcesses(
      const ManagerComponentProcessInventoryRequestDTO &) {
    return manyforge::core::Result<ManagerComponentProcessInventoryResultDTO>::
        Err({"installed_process_inspection_unavailable",
             "installed component process inspection is not composed",
             {}});
  }

  [[nodiscard]] virtual manyforge::core::Result<ManagerComponentProcessStopResultDTO>
  StopComponentProcess(const ManagerComponentProcessStopRequestDTO &) {
    return manyforge::core::Result<ManagerComponentProcessStopResultDTO>::Err(
        {"component_process_stop_unavailable", "component process Stop is not composed", {}});
  }
  [[nodiscard]] virtual manyforge::core::Result<ManagerComponentProcessStopResultDTO>
  ReadComponentProcessStop(const ManagerComponentProcessStopReadRequestDTO &) {
    return manyforge::core::Result<ManagerComponentProcessStopResultDTO>::Err(
        {"component_process_stop_unavailable", "component process Stop result is not composed", {}});
  }

  [[nodiscard]] virtual manyforge::core::Result<ManagerComponentBringUpInspectionResultDTO>
  InspectComponentBringUp(const ManagerComponentBringUpInspectionRequestDTO &) {
    return manyforge::core::Result<ManagerComponentBringUpInspectionResultDTO>::Err(
        {"component_bring_up_inspection_unavailable",
         "component process inspection is not composed", {}});
  }


  [[nodiscard]] virtual ManagerSelectedRuntimeCompositionResultDTO
  InstallOrObserve(
      const ManagerSelectedRuntimeCompositionRequestDTO &request) = 0;

  [[nodiscard]] virtual manyforge::core::Result<
      ManagerSelectedComponentBringUpResultDTO>
  SubmitSelectedComponentBringUp(
      const ManagerSelectedComponentBringUpRequestDTO &) {
    return manyforge::core::Result<ManagerSelectedComponentBringUpResultDTO>::
        Err({"selected_component_bring_up_unavailable",
             "selected component bring-up is not composed",
             {}});
  }

  [[nodiscard]] virtual manyforge::core::Result<
      ManagerSelectedComponentBringUpResultDTO>
  ReadSelectedComponentBringUp(
      const ManagerSelectedComponentBringUpReadRequestDTO &) {
    return manyforge::core::Result<ManagerSelectedComponentBringUpResultDTO>::
        Err({"selected_component_bring_up_unavailable",
             "selected component bring-up is not composed",
             {}});
  }

  [[nodiscard]] virtual manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>
  SubmitApplyComponentBringUp(const ManagerApplyComponentBringUpRequestDTO &) {
    return manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>::Err(
        {"apply_component_bring_up_unavailable",
         "Apply component bring-up is not composed", {}});
  }

  [[nodiscard]] virtual manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>
  ReadApplyComponentBringUp(const ManagerApplyComponentBringUpReadRequestDTO &) {
    return manyforge::core::Result<ManagerApplyComponentBringUpResultDTO>::Err(
        {"apply_component_bring_up_unavailable",
         "Apply component bring-up is not composed", {}});
  }

};

} // namespace manyforge::component_manager

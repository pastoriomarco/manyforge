#pragma once

#include <cstdint>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <manyforge_core/types/configuration_fence.hpp>
#include <manyforge_core/types/controller_launch_group_drain.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/projection_common.hpp>
#include <manyforge_deployment/apply_component_delta.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

/// The native host's clean refusal of one prepared candidate: composing it
/// found a data or setup inconsistency. The candidate was never installed or
/// published; `details.cause` carries the original message. Its effects are
/// known, so the Apply ends `failed`, never `unknown_effect`.
inline constexpr std::string_view kScenarioCompositionRefusedCode =
    "scenario_composition_refused";
/// The producer of that refusal (the execution host).
inline constexpr std::string_view kExecutionHostProducer = "execution_host";

/// CONTRACTS §15: the Manager's one Apply call to the execution host, made
/// while the Apply's Kernel configuration fence is held. The host refuses
/// `apply_requires_inactive_component` for an enabled replaced or removed
/// component (dead or faulted ones are accepted, and so is one whose Disable
/// failed, N28), stops removed and replaced
/// components, installs the candidate bundle, hands the new component set to
/// its component runtime and closes the routes of removed domains.
struct HostApplyRequest final {
  manyforge::core::NamedApplyPublicationTransactionDTO apply;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  /// Every candidate component, sorted.
  std::vector<manyforge::core::ControllableComponentId> components;
  /// The subset of `components` the Apply keeps (delta kRetain), sorted.
  std::vector<manyforge::core::ControllableComponentId> retained_components;
  /// Predecessor components the candidate drops, sorted.
  std::vector<manyforge::core::ControllableComponentId> removed_components;
  /// Predecessor control domains the candidate drops, sorted.
  std::vector<manyforge::core::ControlDomainId> removed_domains;

  [[nodiscard]] bool valid() const noexcept;
  friend bool operator==(const HostApplyRequest &, const HostApplyRequest &) = default;
};

enum class HostApplyOutcome : std::uint8_t {
  kSucceeded = 0,
  kFailed = 1,
};

struct HostApplyResult final {
  HostApplyOutcome outcome{HostApplyOutcome::kFailed};
  /// Present exactly when `outcome` is kFailed.
  std::optional<manyforge::core::ErrorDTO> error{};

  [[nodiscard]] bool valid() const noexcept {
    return (outcome == HostApplyOutcome::kSucceeded) != error.has_value() &&
           (!error || (!error->code.empty() && !error->message.empty()));
  }
  friend bool operator==(const HostApplyResult &, const HostApplyResult &) = default;
};

/// Derive the request from the exact sealed bundles and the Apply's durable
/// per-component delta.
[[nodiscard]] HostApplyRequest MakeHostApplyRequest(
    const manyforge::core::NamedApplyPublicationTransactionDTO &apply,
    const manyforge::deployment::RetainedBundle &predecessor,
    const manyforge::deployment::RetainedBundle &candidate,
    const manyforge::deployment::ApplyComponentDeltaDTO &delta);

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

/// CONTRACTS §11: a component whose process is gone is removed or replaced
/// without a clean Disable. Gone means it has launch processes and every one
/// is a drained process group (cleanup-proven, not merely an exited leader).
/// A component with no launch process or with a local owner is not gone.
[[nodiscard]] inline bool ComponentLaunchProcessesDrained(
    const manyforge::core::ControllableComponentId &component,
    const std::vector<ManagerComponentProcessOccurrenceDTO> &occurrences) noexcept {
  bool any = false;
  for (const auto &row : occurrences) {
    if (row.controllable_component_id != component) continue;
    if (row.process_owner_kind != ManagerComponentProcessOwnerKindDTO::kControllerLaunch ||
        row.cleanup_state != manyforge::core::ControllerLaunchCleanupState::kDrained)
      return false;
    any = true;
  }
  return any;
}
/// Current installed lifecycle facts, bound to the inventory's exact predecessor.
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

  /// Send one Apply to the execution host occurrence `runtime_unit_id`. A
  /// transport error or a lost reply is an Err (effects unknown); a host
  /// decision is an Ok result.
  [[nodiscard]] virtual manyforge::core::Result<HostApplyResult>
  ApplyOnHost(const manyforge::core::RuntimeUnitId &runtime_unit_id,
              const HostApplyRequest &request) = 0;

};

} // namespace manyforge::component_manager

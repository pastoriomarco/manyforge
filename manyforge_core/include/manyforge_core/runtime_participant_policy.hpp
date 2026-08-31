#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/runtime_policy.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <memory>
#include <span>
#include <utility>

namespace manyforge::core {

/// Exact private runtime-policy projection for one sealed participant.
///
/// Deployment independently qualifies the catalog bytes and then supplies the
/// complete immutable DTO. This seal rechecks the closed shape, exact binding
/// scope, and effective motion ceiling; it performs no catalog lookup and
/// mints no catalog identity.
struct RuntimeParticipantPolicySelectionDTO final {
  ControllableComponentId controllable_component_id;
  CommandTargetId command_target_id;
  RuntimePolicyCatalogRecordRefDTO source_artifact;
  RuntimePolicyDTO policy;
  RuntimeMotionSpeedLimitDTO selected_motion_speed_limit;
  double effective_maximum_tcp_linear_speed_mps{};

  friend bool operator==(const RuntimeParticipantPolicySelectionDTO&,
                         const RuntimeParticipantPolicySelectionDTO&) = default;
};

class RuntimeParticipantPolicySelection final {
 public:
  RuntimeParticipantPolicySelection(const RuntimeParticipantPolicySelection&) = default;
  RuntimeParticipantPolicySelection& operator=(const RuntimeParticipantPolicySelection&) = default;
  RuntimeParticipantPolicySelection(RuntimeParticipantPolicySelection&&) noexcept = default;
  RuntimeParticipantPolicySelection& operator=(RuntimeParticipantPolicySelection&&) noexcept =
      default;

  [[nodiscard]] const RuntimeParticipantPolicySelectionDTO& value() const noexcept {
    return *value_;
  }

 private:
  explicit RuntimeParticipantPolicySelection(RuntimeParticipantPolicySelectionDTO value)
      : value_(std::make_shared<const RuntimeParticipantPolicySelectionDTO>(std::move(value))) {}

  std::shared_ptr<const RuntimeParticipantPolicySelectionDTO> value_;

  friend Result<RuntimeParticipantPolicySelection> verify_runtime_participant_policy_selection(
      RuntimePolicyCatalogRecordRefDTO, RuntimePolicyDTO,
      const RuntimeParticipantBinding&);
};

/// Seal one already-qualified retained runtime policy against one exact
/// participant binding and derive the effective cell/target speed ceiling.
[[nodiscard]] Result<RuntimeParticipantPolicySelection> verify_runtime_participant_policy_selection(
    RuntimePolicyCatalogRecordRefDTO source_artifact, RuntimePolicyDTO policy,
    const RuntimeParticipantBinding& participant);

/// Apply the sealed runtime ceiling to an already public-boundary-validated
/// speed profile. The result always carries a finite positive maximum.
[[nodiscard]] Result<SpeedProfileDTO> apply_runtime_participant_motion_speed_policy(
    const RuntimeParticipantPolicySelection& selection, SpeedProfileDTO requested);

/// Require every requested move-scoped interrupt policy to belong to the same
/// sealed runtime-policy revision. No source lookup or arming occurs here.
[[nodiscard]] Result<void> validate_runtime_participant_interrupt_policy_selection(
    const RuntimeParticipantPolicySelection& selection,
    std::span<const MotionInterruptScopeDTO> scopes);

}  // namespace manyforge::core

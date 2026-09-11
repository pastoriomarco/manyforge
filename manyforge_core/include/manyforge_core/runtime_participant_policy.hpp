#pragma once

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/runtime_policy.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <memory>
#include <span>
#include <utility>

namespace manyforge::core {

/// Cold-resolved Deployment choice for one physical motion target. Source
/// import/preset resolution belongs to Deployment; no defaults are calculated
/// during admission. This is private policy, not caller-supplied motion intent.
struct OrdinaryMotionJerkPolicy final {
  bool enabled{};
  bool machine_required{};
  std::map<std::string, CoordinateJerkDTO, std::less<>> limits;

  friend bool operator==(const OrdinaryMotionJerkPolicy&,
                         const OrdinaryMotionJerkPolicy&) = default;
};

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
  std::optional<OrdinaryMotionJerkPolicy> ordinary_motion_jerk{};

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
      RuntimePolicyCatalogRecordRefDTO, RuntimePolicyDTO, const RuntimeParticipantBinding&);
  friend Result<RuntimeParticipantPolicySelection> apply_deployment_motion_speed_ceiling(
      const RuntimeParticipantPolicySelection&, double);
  friend Result<RuntimeParticipantPolicySelection> apply_deployment_motion_jerk_policy(
      const RuntimeParticipantPolicySelection&, const RuntimeParticipantBinding&,
      OrdinaryMotionJerkPolicy);
};

/// Seal one already-qualified retained runtime policy against one exact
/// participant binding and derive the effective cell/target speed ceiling.
[[nodiscard]] Result<RuntimeParticipantPolicySelection> verify_runtime_participant_policy_selection(
    RuntimePolicyCatalogRecordRefDTO source_artifact, RuntimePolicyDTO policy,
    const RuntimeParticipantBinding& participant);

/// Add one independently authored Deployment ceiling without changing the
/// identity or contents of the immutable runtime-policy artifact.  The result
/// can only tighten the catalog-selected ceiling.
[[nodiscard]] Result<RuntimeParticipantPolicySelection> apply_deployment_motion_speed_ceiling(
    const RuntimeParticipantPolicySelection& selection,
    double deployment_maximum_tcp_linear_speed_mps);

/// Seal the independently compiled ordinary-motion profile against its exact
/// target and source-joint scope. Required equipment limits cannot be disabled.
[[nodiscard]] Result<RuntimeParticipantPolicySelection> apply_deployment_motion_jerk_policy(
    const RuntimeParticipantPolicySelection& selection, const RuntimeParticipantBinding& binding,
    OrdinaryMotionJerkPolicy profile);

/// Shared Program/manual preview and execution admission. Omitted requested
/// caps acquire the enabled policy limit; a caller cannot weaken or bypass it.
[[nodiscard]] Result<PerJointDynamicsLimitsDTO> apply_runtime_participant_motion_jerk_policy(
    const RuntimeParticipantPolicySelection& selection, PerJointDynamicsLimitsDTO requested);

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

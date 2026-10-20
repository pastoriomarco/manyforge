#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/coordinate_state.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/joint_dynamics_limits.hpp>
#include <manyforge_core/types/motion_spec.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/resolved_frame.hpp>
#include <manyforge_core/types/speed_profile.hpp>
#include <manyforge_core/types/tolerances.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::roboplan_backend {

/// Exact adapter-private model/target projection selected by a retained
/// RuntimeParticipantBinding. Generated names stay behind this boundary.
struct RoboPlanCandidateBindingDTO final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  std::string compiled_group_name;
  std::vector<core::ResourceId> resources;
  std::vector<std::string> compiled_joint_names;
  /// Exact closed frame inventory admitted by the retained target binding.
  /// Individual operations still select base/TCP roles; the private worker
  /// cannot accept a generated frame name outside this owner-issued set.
  std::vector<core::ResolvedFrameDTO> permitted_frames;

  friend bool operator==(const RoboPlanCandidateBindingDTO&,
                         const RoboPlanCandidateBindingDTO&) = default;
};

/// Private resolved candidate-computation input. This is deliberately not a
/// public wire DTO and carries no caller-selected robot/group identity. The
/// final IPlannerBackend wrapper constructs it only from a Kernel-produced
/// addressed PlanRequestDTO.
struct RoboPlanCandidateRequestDTO final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  core::ModelInstanceId model_instance_id;
  core::ModelRevision model_revision;
  std::vector<core::ResourceId> resources;
  core::ResolvedFrameDTO base_frame;
  core::ResolvedFrameDTO tcp_frame;
  core::ControlledStateDTO start_state;
  core::ResolvedMotionSpecDTO motion;
  core::TolerancesDTO tolerances;
  core::SpeedProfileDTO speed_profile;
  core::PlanningProfileDTO planning_profile{
      core::PlanningProfileDTO::kFirstFeasible};
  core::PerResourceDynamicsLimitsDTO dynamics_limits;
  double planning_timeout_s{5.0};
  std::uint32_t planning_attempt_index{};
  std::optional<core::ActionSpanDTO> action_span;
  std::optional<core::MovingReplacementInputDTO> moving_replacement{};
};

/// Caller-owned bounded-control hooks. The production wrapper supplies these
/// from the Kernel clock and its killable worker lifecycle. RoboPlan 0.7 has no
/// cooperative cancellation hook inside every opaque numerical phase, so the
/// process wrapper remains the final cancellation authority.
struct RoboPlanCandidateControl final {
  std::function<bool()> cancellation_requested;
  std::function<bool()> deadline_expired;
  // Optional same-host transport deadline for the moving suffix only.
  // Final connector validation remains under deadline_expired (whole preparation).
  std::function<bool()> planning_deadline_expired{};
};

struct RoboPlanCandidateComputerConfig final {
  std::optional<std::uint32_t> rng_seed;
  std::size_t maximum_trajectory_samples{65'536U};
  double trajectory_sample_period_s{0.01};
};

/// Serialized native candidate computation over one exact worker-owned
/// retained model projection. The worker never shares this mutable RoboPlan
/// Scene with another computation; its process supervisor provides hard
/// cancellation for opaque planner calls. Planning, IK, shortcutting and
/// TOPP-RA are RoboPlan-owned; authoritative validation, collision admission
/// and dispatch remain Kernel responsibilities.
class RoboPlanCandidateComputer final {
 public:
  RoboPlanCandidateComputer(
      std::shared_ptr<roboplan::Scene> scene,
      RoboPlanCandidateBindingDTO binding,
      RoboPlanCandidateComputerConfig config = {});

  RoboPlanCandidateComputer(const RoboPlanCandidateComputer&) = delete;
  RoboPlanCandidateComputer& operator=(const RoboPlanCandidateComputer&) =
      delete;

  [[nodiscard]] core::Result<core::TrajectoryDTO> compute(
      const RoboPlanCandidateRequestDTO& request,
      const RoboPlanCandidateControl& control = {});

  [[nodiscard]] core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal,
      const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame,
      const core::ControlledStateDTO& seed_state,
      double timeout_s,
      const RoboPlanCandidateControl& control = {});

  [[nodiscard]] const RoboPlanCandidateBindingDTO& binding() const noexcept {
    return binding_;
  }

 private:
  std::shared_ptr<roboplan::Scene> scene_;
  RoboPlanCandidateBindingDTO binding_;
  RoboPlanCandidateComputerConfig config_;
  std::mutex computation_mutex_;
};

}  // namespace manyforge::roboplan_backend

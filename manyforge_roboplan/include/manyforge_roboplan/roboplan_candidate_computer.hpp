#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/types/action_span.hpp>
#include <manyforge_core/types/collision.hpp>
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
/// Collision clearance of one native worker-scene pair. The names are a
/// compiled frame or a projected geometry name, `first < second`.
struct RoboPlanPairMarginDTO final {
  std::string first;
  std::string second;
  double margin_m{};

  friend bool operator==(const RoboPlanPairMarginDTO&,
                         const RoboPlanPairMarginDTO&) = default;
};

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
  /// The motion's planner override over the config's Deployment default.
  core::PlannerOverrideDTO planner{};
  core::PerResourceDynamicsLimitsDTO dynamics_limits;
  double planning_timeout_s{5.0};
  std::uint32_t planning_attempt_index{};
  std::optional<core::ActionSpanDTO> action_span;
  std::optional<core::MovingReplacementInputDTO> moving_replacement{};
  /// Per-pair clearance margins the worker sets on its scene for this plan;
  /// pairs not listed keep margin 0. Filled by the process client from the
  /// selected retained projection.
  std::vector<RoboPlanPairMarginDTO> collision_pair_margins{};
  /// Native body-name pairs (`first < second`) the selected contact policy
  /// allows for this plan; the worker deactivates them on its scene. Filled
  /// by the process client from the selected retained projection.
  std::vector<std::pair<std::string, std::string>> contact_allowed_pairs{};
  core::PlannerCollisionMode collision_mode{core::PlannerCollisionMode::kEnforced};
  std::optional<core::ControlledStateDTO> diagnostic_qualified_target{};
  std::optional<std::int64_t> diagnostic_deadline_ns{};
};

/// Caller-owned bounded-control hooks. The production wrapper supplies these
/// from the Kernel clock and its killable worker lifecycle. RoboPlan 0.8 has no
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
  /// RRT and shortcut `collision_check_step_size` of the first attempt
  /// (Deployment `collision_check_step_rad`); retries use min(step, 0.01).
  double collision_check_step_rad{core::kDefaultCollisionCheckStepRad};
  /// The rest of the Deployment planner block (`planner_search`,
  /// `planner_optimize`, `planner_n`, `planner_max_nodes`); a request's
  /// `planner` override replaces single members.
  core::PlannerSearchDTO planner_search{core::kDefaultPlannerSearch};
  core::PlannerOptimizeDTO planner_optimize{core::kDefaultPlannerOptimize};
  std::uint32_t planner_n{core::kDefaultPlannerBestOfN};
  std::uint32_t planner_max_nodes{core::kDefaultPlannerMaxNodes};
  /// Deployment `cartesian_timing`: a Cartesian request without its own
  /// `speed_mode` uses this.
  core::CartesianSpeedModeDTO cartesian_timing{core::kDefaultCartesianTiming};
  /// Deployment `motion_timing`: how joint and pose goals are
  /// timed. Smooth: TOPP-RA without the TCP bound, then the final uniform
  /// stretch to the TCP target. Fast: the TCP bound inside TOPP-RA.
  core::MotionTimingDTO motion_timing{core::kDefaultMotionTiming};

  [[nodiscard]] core::PlannerSettingsDTO planner_defaults() const noexcept {
    return {planner_search, planner_optimize, planner_n,
            collision_check_step_rad, planner_max_nodes};
  }
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
      const RoboPlanCandidateControl& control = {},
      std::optional<core::ControlledStateDTO>* qualified_target = nullptr);

  [[nodiscard]] core::Result<std::vector<double>> solve_ik(
      const core::Pose3DTO& goal,
      const core::ResolvedFrameDTO& base_frame,
      const core::ResolvedFrameDTO& tcp_frame,
      const core::ControlledStateDTO& seed_state,
      double timeout_s,
      const RoboPlanCandidateControl& control = {},
      const core::TolerancesDTO* tolerances = nullptr);

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

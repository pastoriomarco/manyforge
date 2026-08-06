#pragma once

#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/plan_request.hpp>

namespace manyforge::core {

/// Single-shot IK request: resolve a Cartesian target pose to a joint target
/// without running a full plan(). Distinct from plan() to avoid the planning
/// ramp + collision-search budget; used by validated Robot Control candidate
/// Cartesian authoring.
struct IkRequestDTO final {
    ControllableComponentId controllable_component_id;
    CommandTargetId command_target_id;
    std::vector<PlannerModelBindingDTO> model_bindings;
    std::vector<ResourceId> resources;
    /// TCP pose-in-base to solve for.
    Pose3DTO goal_pose;
    ResolvedFrameDTO base_frame;
    ResolvedFrameDTO tcp_frame;
    ControlledStateDTO seed_state;
    ProjectionLeaseBindingDTO projection_binding;
    /// Optional caller budget for this IK request. A value <= 0 means the
    /// backend default applies. Interactive callers set this to a small budget
    /// so a slow IK attempt cannot stall the operator loop.
    double timeout_s = 0.0;
};

/// Result of a single-shot IK solve.
struct IkResultDTO {
    bool ok = false;
    /// Exact ordered compiler-owned resources represented by joint_names.
    std::vector<ResourceId> resources;
    /// Canonical joint names in dispatch order.
    std::vector<std::string> joint_names;
    /// Resolved joint positions in the same order.
    std::vector<double> positions_rad;
    /// Error details when !ok.
    std::optional<ErrorDTO> error;
};

/// Planner backend interface (spec 220 §2)
///
/// Responsibilities:
/// - Plan motions from a PlanRequestDTO and return a time-parameterized TrajectoryDTO
/// - Resolve single-shot IK queries (no planning, no trajectory shaping)
/// - Planner-specific knobs live in deployment configuration, not in the program artifact
///
/// Threading:
/// - The kernel MUST NOT issue concurrent Plan() calls to the same IPlannerBackend instance
/// - A Plan() call in progress MUST be cancelable by the kernel
/// - solve_ik() is not concurrency-safe against plan(); callers MUST serialize
class IPlannerBackend {
 public:
    virtual ~IPlannerBackend() = default;

    /// Plan a trajectory from the current (or specified) state to the goal.
    ///
    /// @param request The planning request with goal and configuration
    /// @return PlanResultDTO containing the trajectory or error
    virtual PlanResultDTO plan(const PlanRequestDTO& request) = 0;

    /// Request cancellation of the currently active Plan() call, if any.
    /// Backends that can interrupt planning work should honor this
    /// cooperatively at their natural polling/checkpoint boundaries. The
    /// method must be non-blocking, lock-light, and must not call back into the
    /// kernel: stop/reset/interrupt paths may invoke it while holding lifecycle
    /// state needed by a wedged plan() call.
    ///
    /// default no-op preserves source compatibility for simple deterministic
    /// test backends.
    virtual void request_cancel() {}

    /// Single-shot IK for Cartesian-target resolution. Returns a joint target
    /// matching the requested pose, or an IK error if no solution is
    /// reachable. Does NOT plan a trajectory, run collision checks, or honor
    /// planner ramps.
    ///
    /// Default implementation returns unsupported_capability so existing
    /// planner backends without IK keep building; backends that wrap a real
    /// IK solver override it.
    virtual IkResultDTO solve_ik(const IkRequestDTO& /*request*/) {
        IkResultDTO result;
        result.ok = false;
        result.error = ErrorDTO{
            std::string(to_string(ErrorCode::kUnsupportedCapability)),
            "single-shot IK not supported by this planner backend",
            {{"reason", "single_shot_ik_unsupported"},
             {"capability", "planning.ik.single_shot"}}};
        return result;
    }
};

}  // namespace manyforge::core

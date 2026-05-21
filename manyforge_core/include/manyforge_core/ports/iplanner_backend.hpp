#pragma once

#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/plan_request.hpp>
#include <manyforge_core/types/robot_state.hpp>

namespace manyforge::core {

/// Single-shot IK request used by the streaming-session execution mode
/// (ADR-0016). The kernel calls this to resolve cartesian streaming
/// targets to joint targets before forwarding to the execution backend.
/// Distinct from full plan() to avoid the planning ramp + collision-search
/// budget on every streamed frame.
struct IkRequestDTO {
    /// TCP pose-in-base to solve for.
    Pose3DTO goal_pose;
    /// Reference frame the pose is expressed in.
    std::string base_frame;
    /// End-effector / TCP frame.
    std::string tcp_frame;
    /// Seed state (previous-frame joint positions for continuity).
    /// Empty seed falls back to the planner backend's current state.
    RobotStateDTO seed_state;
    /// Optional caller budget for this IK request. A value <= 0 means the
    /// backend default applies. Streaming callers set this to the direct
    /// control budget so a slow IK attempt cannot stall the operator loop.
    double timeout_s = 0.0;
};

/// Result of a single-shot IK solve.
struct IkResultDTO {
    bool ok = false;
    /// Joint names in dispatch order (matches the planner's group order).
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
/// - Resolve single-shot IK queries (no planning, no trajectory shaping) for the
///   streaming-session execution mode (ADR-0016)
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

    /// Single-shot IK for streaming-session cartesian-target resolution
    /// (ADR-0016). Returns a joint target matching the requested pose, or
    /// an IK error if no solution is reachable. Does NOT plan a trajectory,
    /// run collision checks, or honor planner ramps.
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
            {}};
        return result;
    }
};

}  // namespace manyforge::core

#pragma once

#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/ports/iplanner_backend.hpp>
#include <manyforge_core/ports/irobot_kinematics.hpp>

namespace manyforge::core::testing {

enum class PlannerMotionCapability {
  kJoint,
  kPose,
  kCartesian,
};

struct PlannerCapabilityProfile {
  bool joint_goal = false;
  bool pose_goal = false;
  bool cartesian_path = false;
  bool single_shot_ik = false;
};

struct PlannerConformanceCase {
  std::string id;
  PlanRequestDTO request;
};

/// Builds the mandatory one-case-per-motion matrix. Requests must
/// already contain absolute, authority-resolved motion payloads; target mode
/// and references deliberately do not cross IPlannerBackend.
std::vector<PlannerConformanceCase> make_portable_planner_matrix(
    const PlanRequestDTO& joint_request,
    const PlanRequestDTO& pose_request,
    const PlanRequestDTO& cartesian_request);

struct PlannerExpectedFailureCase {
  std::string id;
  PlanRequestDTO request;
  std::string expected_code;
  std::string expected_reason;
};

struct PlannerIkConformanceCase {
  std::string id;
  IkRequestDTO request;
};

struct PlannerCancellationConformanceCase {
  std::string id;
  IPlannerBackend* planner = nullptr;
  PlanRequestDTO request;
  /// Adapter fixtures may expose a deterministic active-work gate. When this
  /// is absent, settle_before_cancel must be long enough for the adapter's
  /// plan-entry initialization and the harness verifies that plan() is still
  /// active before issuing cancellation.
  std::function<bool(std::chrono::milliseconds)> wait_until_active;
  std::chrono::milliseconds settle_before_cancel{0};
  /// A planner that misses this bound cannot be detached safely because it may
  /// still reference adapter-fixture state. The test-support runner therefore
  /// terminates its process on breach. Run cancellation conformance in an
  /// isolated CTest/GTest process, as the supplied targets do.
  std::chrono::milliseconds completion_timeout{1000};
};

struct PlannerConformanceViolation {
  std::string case_id;
  std::string invariant;
  std::string detail;
};

struct PlannerConformanceReport {
  std::vector<std::string> completed_cases;
  std::vector<PlannerConformanceViolation> violations;

  [[nodiscard]] bool passed() const noexcept { return violations.empty(); }
  [[nodiscard]] std::string summary() const;
};

using PlannerForwardKinematics =
    std::function<Result<Pose3DTO>(const FkRequestDTO&)>;

struct PlannerBackendConformanceInput {
  std::string adapter_id;
  IPlannerBackend* planner = nullptr;
  PlannerCapabilityProfile capabilities;
  std::vector<PlannerConformanceCase> plan_cases;
  PlannerForwardKinematics forward_kinematics;
  std::vector<PlannerExpectedFailureCase> expected_failures;
  std::optional<PlannerIkConformanceCase> ik_case;
  std::optional<PlannerCancellationConformanceCase> cancellation_case;
  std::size_t max_waypoint_count = 100'000;
  std::chrono::milliseconds max_plan_wall_time{10'000};
};

/// Runs planner-neutral contract checks only. It has no dependency on GTest,
/// RoboPlan, ROS, MoveIt, or any concrete planner implementation, so adapter
/// packages can reuse it without linking their planner into manyforge_core.
PlannerConformanceReport run_planner_backend_conformance(
    const PlannerBackendConformanceInput& input);

}  // namespace manyforge::core::testing

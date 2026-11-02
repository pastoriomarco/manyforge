#include <gtest/gtest.h>

#include <manyforge_core/testing/planner_backend_conformance.hpp>
#include <manyforge_core/testing/addressed_request_fixtures.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <string>

namespace manyforge::core::testing {
namespace {

constexpr auto kDigestA =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr auto kDigestB =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
constexpr auto kDigestC =
    "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
constexpr auto kDigestD =
    "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

ProjectionLeaseBindingDTO projection_binding() {
  return ProjectionLeaseBindingDTO{
      ProjectionRevisionDTO{
          RetainedBundleId{kDigestA}, GraphRevision{kDigestB},
          SceneRevision{SceneId{kDigestC}, 7U,
                        ContentDigest{kDigestD}}},
      ProjectionLeaseId{"lease-seven"}};
}

Pose3DTO pose_at(double x, double y) {
  Pose3DTO pose;
  pose.position_m = {x, y, 0.0};
  pose.orientation.w = 1.0;
  return pose;
}

ErrorDTO unsupported(std::string capability) {
  return ErrorDTO{kErrorUnsupportedCapability,
                  "deterministic planner capability is not available",
                  {{"reason", "planner_capability_unsupported"},
                   {"capability", std::move(capability)}}};
}

class DeterministicPlanner final : public IPlannerBackend {
 public:
  PlannerCapabilityProfile capabilities{true, true, true, true};
  bool corrupt_time = false;
  bool leak_private_joint_name = false;
  bool leak_private_ik_joint_name = false;
  bool ignore_cancellation = false;

  PlanResultDTO plan(const PlanRequestDTO& request) override {
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_requested_ = false;
    }
    if (!request.projection_binding.valid()) {
      return PlanResultDTO{
          std::nullopt,
          ErrorDTO{kErrorInvalidRequest,
                   "deterministic planner requires a retained scene binding",
                   {{"reason", "planner_projection_binding_invalid"}}},
          {}};
    }
    const auto capability = [&]() -> std::pair<bool, std::string> {
      if (std::holds_alternative<ResolvedJointGoalDTO>(request.motion)) {
        return {capabilities.joint_goal, "planning.motion.joint_goal"};
      }
      if (std::holds_alternative<PoseGoalDTO>(request.motion)) {
        return {capabilities.pose_goal, "planning.motion.pose_goal"};
      }
      if (std::holds_alternative<CartesianPathDTO>(request.motion)) {
        return {capabilities.cartesian_path,
                "planning.motion.cartesian_path"};
      }
      return {false, "planning.motion.named_target"};
    }();
    if (!capability.first) {
      return PlanResultDTO{std::nullopt, unsupported(capability.second), {}};
    }

    if (request.move_id.value() == "cancel_in_flight") {
      {
        std::lock_guard<std::mutex> lock(mutex_);
        active_ = true;
      }
      active_cv_.notify_all();
      std::unique_lock<std::mutex> lock(mutex_);
      cancel_cv_.wait(lock, [&] { return cancel_requested_; });
      active_ = false;
      return PlanResultDTO{
          std::nullopt,
          ErrorDTO{kErrorPlanningFailed,
                   "planning cancelled by interrupt",
                   {{"reason", "planning_cancelled_by_interrupt"},
                    {"failure_stage", "deterministic_wait"}}},
          {}};
    }

    TrajectoryDTO trajectory;
    trajectory.resources = request.resources;
    trajectory.joint_names = request.start_state.joint_names;
    if (leak_private_joint_name && !trajectory.joint_names.empty()) {
      trajectory.joint_names.front() = "backend_private_joint_0";
    }
    const auto append = [&](double time, const std::vector<double>& positions,
                            TrajectoryDTO& output) {
      output.waypoints.push_back(TrajectoryWaypointDTO{
          time, positions,
          std::vector<double>(positions.size(), 0.0),
          std::vector<double>(positions.size(), 0.0)});
    };
    const auto start_positions =
        angular_position_projection(request.start_state);
    if (!start_positions.ok()) {
      return PlanResultDTO{std::nullopt, start_positions.error(), {}};
    }
    append(0.0, start_positions.value(), trajectory);
    if (const auto* goal =
            std::get_if<ResolvedJointGoalDTO>(&request.motion)) {
      append(1.0, goal->positions_rad, trajectory);
    } else if (const auto* goal = std::get_if<PoseGoalDTO>(&request.motion)) {
      append(1.0,
             {goal->goal.position_m.x_m, goal->goal.position_m.y_m},
             trajectory);
    } else if (const auto* path =
                   std::get_if<CartesianPathDTO>(&request.motion)) {
      trajectory.waypoints.clear();
      for (std::size_t index = 0; index < path->waypoints.size(); ++index) {
        append(static_cast<double>(index),
               {path->waypoints[index].position_m.x_m,
                path->waypoints[index].position_m.y_m},
               trajectory);
      }
    }
    if (corrupt_time && trajectory.waypoints.size() > 1U) {
      trajectory.waypoints.back().time_from_start_s = 0.0;
    }
    return PlanResultDTO{std::move(trajectory), std::nullopt,
                         request.projection_binding.projection_revision};
  }

  void request_cancel() override {
    if (ignore_cancellation) {
      return;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      cancel_requested_ = true;
    }
    cancel_cv_.notify_all();
  }

  IkResultDTO solve_ik(const IkRequestDTO& request) override {
    if (!capabilities.single_shot_ik) {
      return IkResultDTO{false, {}, {}, {},
                         unsupported("planning.ik.single_shot")};
    }
    auto joint_names = request.seed_state.joint_names;
    if (joint_names.empty()) {
      joint_names = {"joint_x", "joint_y"};
    }
    if (leak_private_ik_joint_name && !joint_names.empty()) {
      joint_names.front() = "backend_private_ik_joint_0";
    }
    return IkResultDTO{true,
                       request.resources,
                       std::move(joint_names),
                       {request.goal_pose.position_m.x_m,
                        request.goal_pose.position_m.y_m},
                       std::nullopt};
  }

  bool wait_until_active(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return active_cv_.wait_for(lock, timeout, [&] { return active_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable active_cv_;
  std::condition_variable cancel_cv_;
  bool active_ = false;
  bool cancel_requested_ = false;
};

PlanRequestDTO base_request(std::string move_id) {
  auto request = make_test_plan_request(
      std::move(move_id),
      ResolvedJointGoalDTO{
          {ResourceId("resource:joint_x"), ResourceId("resource:joint_y")},
          {"joint_x", "joint_y"}, {0.0, 0.0}},
      {"joint_x", "joint_y"}, {0.0, 0.0});
  request.tolerances = {1e-6, 1e-6, 1e-6};
  request.speed_profile = {0.5, 0.5, std::nullopt};
  request.planning_timeout_s = 1.0;
  request.projection_binding = projection_binding();
  return request;
}

PlannerBackendConformanceInput make_input(DeterministicPlanner& planner) {
  auto joint = base_request("joint");
  joint.motion = ResolvedJointGoalDTO{
      joint.resources,
      {"joint_x", "joint_y"}, {0.2, -0.1}};

  auto pose = base_request("pose");
  pose.motion = PoseGoalDTO{pose_at(0.2, -0.1)};

  auto cartesian = base_request("cartesian");
  CartesianPathDTO path;
  path.waypoints = {pose_at(0.0, 0.0), pose_at(0.1, 0.1),
                    pose_at(0.2, -0.1)};
  cartesian.motion = path;

  PlannerBackendConformanceInput input;
  input.adapter_id = "planning.deterministic_fake";
  input.planner = &planner;
  input.capabilities = planner.capabilities;
  input.plan_cases =
      make_portable_planner_matrix(joint, pose, cartesian);
  input.forward_kinematics = [](const FkRequestDTO& request) {
    if (request.robot_state.positions_rad.size() != 2U) {
      return Result<Pose3DTO>::Err(ErrorDTO{
          kErrorInvalidRequest, "fake FK requires two joints",
          {{"reason", "fake_fk_shape_invalid"}}});
    }
    return Result<Pose3DTO>::Ok(
        pose_at(request.robot_state.positions_rad[0],
                request.robot_state.positions_rad[1]));
  };
  auto invalid = joint;
  invalid.move_id = MoveId("invalid_binding");
  invalid.projection_binding = {};
  input.expected_failures.push_back(PlannerExpectedFailureCase{
      "invalid_projection_binding", invalid, kErrorInvalidRequest,
      "planner_projection_binding_invalid"});
  input.ik_case = PlannerIkConformanceCase{
      "single_shot_ik",
      IkRequestDTO{joint.controllable_component_id, joint.command_target_id,
                   joint.model_bindings, joint.resources,
                   pose_at(0.15, -0.05), joint.base_frame, joint.tcp_frame,
                   joint.start_state, joint.projection_binding, 0.1}};
  auto cancellation = joint;
  cancellation.move_id = MoveId("cancel_in_flight");
  input.cancellation_case = PlannerCancellationConformanceCase{
      "cooperative_cancellation", &planner, cancellation,
      [&planner](std::chrono::milliseconds timeout) {
        return planner.wait_until_active(timeout);
      },
      std::chrono::milliseconds(0), std::chrono::milliseconds(250)};
  input.max_plan_wall_time = std::chrono::milliseconds(250);
  return input;
}

bool has_invariant(const PlannerConformanceReport& report,
                   const std::string& invariant) {
  return std::any_of(report.violations.begin(), report.violations.end(),
                     [&](const auto& violation) {
                       return violation.invariant == invariant;
                     });
}

TEST(PlannerBackendConformance,
     DeterministicReferencePassesPortableMatrixAndControlContracts) {
  DeterministicPlanner planner;
  const auto report = run_planner_backend_conformance(make_input(planner));
  EXPECT_TRUE(report.passed()) << report.summary();
  EXPECT_EQ(report.completed_cases.size(), 6U);
}

TEST(PlannerBackendConformance,
     ExplicitUnsupportedCapabilitiesRemainConformantAndStructured) {
  DeterministicPlanner planner;
  planner.capabilities.cartesian_path = false;
  planner.capabilities.single_shot_ik = false;
  auto input = make_input(planner);
  input.capabilities = planner.capabilities;

  const auto report = run_planner_backend_conformance(input);

  EXPECT_TRUE(report.passed()) << report.summary();
}

TEST(PlannerBackendConformance, DetectsMonotonicTimeRegression) {
  DeterministicPlanner planner;
  planner.corrupt_time = true;
  auto input = make_input(planner);
  input.cancellation_case.reset();

  const auto report = run_planner_backend_conformance(input);

  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(has_invariant(report, "trajectory.monotonic_time"))
      << report.summary();
}

TEST(PlannerBackendConformance, DetectsPrivateModelNameLeakage) {
  DeterministicPlanner planner;
  planner.leak_private_joint_name = true;
  auto input = make_input(planner);
  input.cancellation_case.reset();

  const auto report = run_planner_backend_conformance(input);

  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(has_invariant(report, "model.canonical_joint_set"))
      << report.summary();
}

TEST(PlannerBackendConformance, DetectsPrivateIkModelNameLeakage) {
  DeterministicPlanner planner;
  planner.leak_private_ik_joint_name = true;
  auto input = make_input(planner);
  input.cancellation_case.reset();

  const auto report = run_planner_backend_conformance(input);

  EXPECT_FALSE(report.passed());
  EXPECT_TRUE(has_invariant(report, "ik.canonical_joint_set"))
      << report.summary();
}

TEST(PlannerBackendConformance,
     NonCooperativeCancellationIsContainedByTheTestProcess) {
  EXPECT_EXIT(
      {
        DeterministicPlanner planner;
        planner.ignore_cancellation = true;
        auto input = make_input(planner);
        input.cancellation_case->completion_timeout =
            std::chrono::milliseconds(25);
        (void)run_planner_backend_conformance(input);
      },
      ::testing::ExitedWithCode(EXIT_FAILURE),
      "cancellation.bounded_completion");
}

class PlannerWithoutIk final : public IPlannerBackend {
 public:
  PlanResultDTO plan(const PlanRequestDTO&) override { return {}; }
};

TEST(PlannerBackendConformance, DefaultIkRejectionNamesPortableCapability) {
  PlannerWithoutIk planner;
  const auto plan = base_request("default_ik");
  const auto result = planner.solve_ik(IkRequestDTO{
      plan.controllable_component_id, plan.command_target_id,
      plan.model_bindings, plan.resources, pose_at(0.0, 0.0),
      plan.base_frame, plan.tcp_frame, plan.start_state,
      plan.projection_binding, 0.1});

  ASSERT_FALSE(result.ok);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, kErrorUnsupportedCapability);
  EXPECT_EQ(result.error->details.at("reason"),
            "single_shot_ik_unsupported");
  EXPECT_EQ(result.error->details.at("capability"),
            "planning.ik.single_shot");
}

}  // namespace
}  // namespace manyforge::core::testing

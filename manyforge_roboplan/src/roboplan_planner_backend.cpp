#include <manyforge_roboplan/roboplan_planner_backend.hpp>

#include <manyforge_roboplan/roboplan_focused_adapters.hpp>

#include <manyforge_roboplan/collision_diagnostics_capture.hpp>

#include <Eigen/Dense>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fmt/format.h>
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
bool planner_debug_enabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("MANYFORGE_ROBOPLAN_PLANNER_DEBUG");
    if (value == nullptr) {
      return false;
    }
    const std::string flag(value);
    return !(flag.empty() || flag == "0" || flag == "false" || flag == "FALSE");
  }();
  return enabled;
}

template <typename... Args>
void planner_debug(fmt::format_string<Args...> format, Args&&... args) {
  if (!planner_debug_enabled()) {
    return;
  }
  fmt::print(stderr, format, std::forward<Args>(args)...);
}

struct CollisionDiagnostics {
  std::vector<std::string> pairs;
  std::string raw_output;
};

struct TcpSpeedCheck {
  double max_speed_mps = 0.0;
  size_t segment_index = 0;
  double segment_dt_s = 0.0;
};

std::string join_strings(const std::vector<std::string>& values,
                         const std::string& separator) {
  std::string result;
  for (const auto& value : values) {
    if (!result.empty()) {
      result += separator;
    }
    result += value;
  }
  return result;
}

CollisionDiagnostics collect_collision_diagnostics(
    const std::shared_ptr<roboplan::Scene>& scene,
    const Eigen::VectorXd& q) {
  CollisionDiagnostics diagnostics;

  diagnostics.raw_output =
      manyforge::roboplan_backend::capture_collision_diagnostics_stdout([&]() {
        scene->hasCollisions(q, true);
      });
  if (!diagnostics.raw_output.empty()) {
    std::lock_guard<std::mutex> lock(
        manyforge::roboplan_backend::collision_diagnostics_stdout_mutex());
    std::cout << diagnostics.raw_output;
  }

  std::istringstream lines(diagnostics.raw_output);
  std::string line;
  const std::string prefix = "Collision detected between ";
  while (std::getline(lines, line)) {
    if (line.rfind(prefix, 0) != 0) {
      continue;
    }
    std::string pair = line.substr(prefix.size());
    const std::string needle = " and ";
    const auto split = pair.find(needle);
    if (split != std::string::npos) {
      pair.replace(split, needle.size(), " <-> ");
    }
    diagnostics.pairs.push_back(pair);
  }
  return diagnostics;
}

std::string collision_message_with_pairs(
    const std::string& message,
    const CollisionDiagnostics& diagnostics) {
  if (diagnostics.pairs.empty()) {
    return message;
  }
  return message + ": " + join_strings(diagnostics.pairs, "; ");
}

std::map<std::string, std::string> collision_details(
    std::string failure_stage,
    bool start_in_collision,
    bool goal_in_collision,
    const CollisionDiagnostics& diagnostics) {
  std::map<std::string, std::string> details;
  details["failure_stage"] = std::move(failure_stage);
  details["start_in_collision"] = start_in_collision ? "true" : "false";
  details["goal_in_collision"] = goal_in_collision ? "true" : "false";
  details["collision_pair_count"] = std::to_string(diagnostics.pairs.size());
  if (!diagnostics.pairs.empty()) {
    details["collision_pairs"] = join_strings(diagnostics.pairs, "; ");
  }
  for (size_t i = 0; i < diagnostics.pairs.size(); ++i) {
    details["collision_pair_" + std::to_string(i + 1)] = diagnostics.pairs[i];
  }
  return details;
}

TcpSpeedCheck compute_tcp_speed_check(
    const std::shared_ptr<roboplan::Scene>& scene,
    const std::string& group_name,
    const roboplan::JointTrajectory& trajectory,
    const std::string& tcp_frame) {
  TcpSpeedCheck check;
  Eigen::Vector3d prev_pos = Eigen::Vector3d::Zero();
  bool have_prev = false;

  for (size_t i = 0; i < trajectory.times.size(); ++i) {
    const Eigen::VectorXd q_full =
        scene->toFullJointPositions(group_name, trajectory.positions[i]);
    Eigen::Matrix4d tform = scene->forwardKinematics(q_full, tcp_frame);
    Eigen::Vector3d pos = tform.block<3, 1>(0, 3);

    if (have_prev && i > 0) {
      const double dt = trajectory.times[i] - trajectory.times[i - 1];
      if (dt > 1e-9) {
        const double speed = (pos - prev_pos).norm() / dt;
        if (speed > check.max_speed_mps) {
          check.max_speed_mps = speed;
          check.segment_index = i - 1;
          check.segment_dt_s = dt;
        }
      }
    }

    prev_pos = pos;
    have_prev = true;
  }

  return check;
}

// Rotation matrix to RPY (ZYX convention, in degrees)
void matrix_to_rpy_deg(const Eigen::Matrix4d& tform, double& roll, double& pitch, double& yaw) {
  pitch = std::atan2(-tform(2,0), std::sqrt(tform(0,0)*tform(0,0) + tform(1,0)*tform(1,0)));
  if (std::abs(std::cos(pitch)) > 1e-6) {
    roll  = std::atan2(tform(2,1), tform(2,2));
    yaw   = std::atan2(tform(1,0), tform(0,0));
  } else {
    roll  = std::atan2(-tform(1,2), tform(1,1));
    yaw   = 0.0;
  }
  roll  *= 180.0 / M_PI;
  pitch *= 180.0 / M_PI;
  yaw   *= 180.0 / M_PI;
}

double normalize_equivalent_revolute_angle_for_rrt(
    double value,
    double seed,
    double lower,
    double upper) {
  constexpr double kTwoPi = 2.0 * M_PI;
  constexpr double kWrapLimitTolerance = 1e-6;

  if (!std::isfinite(value) || !std::isfinite(seed) ||
      !std::isfinite(lower) || !std::isfinite(upper) || lower >= upper) {
    return value;
  }

  const double width = upper - lower;
  if (width < kTwoPi - kWrapLimitTolerance) {
    return value;
  }

  const int k_min = static_cast<int>(std::ceil((lower - value) / kTwoPi));
  const int k_max = static_cast<int>(std::floor((upper - value) / kTwoPi));
  if (k_min > k_max) {
    return value;
  }

  double best = value + static_cast<double>(k_min) * kTwoPi;
  double best_distance = std::abs(best - seed);
  for (int k = k_min + 1; k <= k_max; ++k) {
    const double candidate = value + static_cast<double>(k) * kTwoPi;
    const double distance = std::abs(candidate - seed);
    if (distance < best_distance) {
      best = candidate;
      best_distance = distance;
    }
  }
  return best;
}

void normalize_ik_goal_for_rrt_limits(
    const std::shared_ptr<roboplan::Scene>& scene,
    const std::string& group_name,
    const Eigen::VectorXd& start_positions,
    Eigen::VectorXd& goal_positions) {
  auto pos_limits = scene->getPositionLimitVectors(group_name, /*collapsed=*/false);
  if (!pos_limits.has_value()) {
    return;
  }

  const auto& [lower, upper] = pos_limits.value();
  if (lower.size() != goal_positions.size() ||
      upper.size() != goal_positions.size() ||
      start_positions.size() != goal_positions.size()) {
    return;
  }

  for (Eigen::Index i = 0; i < goal_positions.size(); ++i) {
    const double before = goal_positions[i];
    const double after = normalize_equivalent_revolute_angle_for_rrt(
        before, start_positions[i], lower[i], upper[i]);
    if (std::abs(after - before) > 1e-9) {
      planner_debug(
          "[PLANNER] normalized IK joint {} from {:.4f} to {:.4f} within [{:.4f}, {:.4f}]\n",
          i, before, after, lower[i], upper[i]);
      goal_positions[i] = after;
    }
  }
}

void anchor_trajectory_endpoints(
    roboplan::JointTrajectory& trajectory,
    const Eigen::VectorXd& start_positions,
    const Eigen::VectorXd& goal_positions) {
  if (trajectory.positions.empty()) {
    return;
  }

  auto assign_if_same_size = [](Eigen::VectorXd& target, const Eigen::VectorXd& value) {
    if (target.size() == value.size()) {
      target = value;
    }
  };
  auto zero_if_present = [](std::vector<Eigen::VectorXd>& values, size_t index) {
    if (index < values.size()) {
      values[index].setZero();
    }
  };

  assign_if_same_size(trajectory.positions.front(), start_positions);
  if (!trajectory.times.empty()) {
    trajectory.times.front() = 0.0;
  }
  zero_if_present(trajectory.velocities, 0);
  zero_if_present(trajectory.accelerations, 0);

  if (trajectory.positions.size() > 1) {
    const size_t last = trajectory.positions.size() - 1;
    assign_if_same_size(trajectory.positions.back(), goal_positions);
    zero_if_present(trajectory.velocities, last);
    zero_if_present(trajectory.accelerations, last);
  }
}
}  // namespace

namespace manyforge::roboplan_backend {

JointCapMeasurement measure_joint_cap_peaks(
    const roboplan::JointTrajectory& traj,
    const std::map<std::string, double>& vel_cap_by_name,
    const std::map<std::string, double>& acc_cap_by_name) {
  JointCapMeasurement m;
  const size_t n = traj.times.size();
  // Every capped joint MUST be present in the trajectory's joint set, otherwise
  // its cap is never measured and would falsely report as satisfied. A capped
  // joint absent from the trajectory cannot be verified -> fail closed. (The
  // planner also rejects non-active-group cap names up front; this is the
  // measurement-layer backstop against any trajectory/group-name divergence.)
  auto trajectory_has_joint = [&](const std::string& jn) {
    return std::find(traj.joint_names.begin(), traj.joint_names.end(), jn) !=
           traj.joint_names.end();
  };
  for (const auto& [jn, cap] : vel_cap_by_name) {
    (void)cap;
    if (!trajectory_has_joint(jn)) {
      m.samples_complete = false;
      m.incomplete_dimension = "velocity";
      m.incomplete_joint = jn;
      return m;
    }
  }
  for (const auto& [jn, cap] : acc_cap_by_name) {
    (void)cap;
    if (!trajectory_has_joint(jn)) {
      m.samples_complete = false;
      m.incomplete_dimension = "acceleration";
      m.incomplete_joint = jn;
      return m;
    }
  }
  for (size_t j = 0; j < traj.joint_names.size(); ++j) {
    const std::string& jn = traj.joint_names[j];
    const auto v_it = vel_cap_by_name.find(jn);
    const auto a_it = acc_cap_by_name.find(jn);
    const bool want_v = v_it != vel_cap_by_name.end();
    const bool want_a = a_it != acc_cap_by_name.end();
    if (!want_v && !want_a) {
      continue;  // joint not capped by this deployment (e.g. other group)
    }
    // A capped joint with no waypoints cannot be verified -> fail closed.
    if (n == 0) {
      m.samples_complete = false;
      m.incomplete_dimension = want_v ? "velocity" : "acceleration";
      m.incomplete_joint = jn;
      return m;
    }
    double peak_v = 0.0;
    double peak_a = 0.0;
    for (size_t w = 0; w < n; ++w) {
      if (want_v) {
        if (w >= traj.velocities.size() ||
            j >= static_cast<size_t>(traj.velocities[w].size())) {
          m.samples_complete = false;
          m.incomplete_dimension = "velocity";
          m.incomplete_joint = jn;
          return m;
        }
        peak_v = std::max(peak_v, std::abs(traj.velocities[w][j]));
      }
      if (want_a) {
        if (w >= traj.accelerations.size() ||
            j >= static_cast<size_t>(traj.accelerations[w].size())) {
          m.samples_complete = false;
          m.incomplete_dimension = "acceleration";
          m.incomplete_joint = jn;
          return m;
        }
        peak_a = std::max(peak_a, std::abs(traj.accelerations[w][j]));
      }
    }
    if (want_v && peak_v > v_it->second) {
      const double r = peak_v / v_it->second;
      if (r > m.worst_vel_ratio) {
        m.worst_vel_ratio = r;
        m.worst_vel_joint = jn;
        m.worst_vel_peak = peak_v;
      }
    }
    if (want_a && peak_a > a_it->second) {
      const double r = peak_a / a_it->second;
      if (r > m.worst_acc_ratio) {
        m.worst_acc_ratio = r;
        m.worst_acc_joint = jn;
        m.worst_acc_peak = peak_a;
      }
    }
  }
  return m;
}

RoboPlanPlannerBackend::RoboPlanPlannerBackend(
    std::shared_ptr<RoboPlanSceneProjection> projection,
    std::shared_ptr<roboplan::Scene> live_scene,
    const roboplan::RRTOptions& rrt_options,
    std::shared_ptr<std::mutex> scene_mutex,
    std::shared_ptr<roboplan::SimpleIk> single_shot_ik,
    std::optional<roboplan::SimpleIkOptions> ik_options,
    std::optional<roboplan::SimpleIkOptions> single_shot_ik_options)
    : projection_(std::move(projection)),
      live_scene_(std::move(live_scene)),
      rrt_options_(rrt_options),
      single_shot_ik_(std::move(single_shot_ik)),
      scene_mutex_(std::move(scene_mutex)) {
  if (!projection_) {
    throw std::invalid_argument("RoboPlan planner projection must not be null");
  }
  if (!live_scene_) {
    throw std::invalid_argument("RoboPlan live IK scene must not be null");
  }
  if (!scene_mutex_) {
    scene_mutex_ = std::make_shared<std::mutex>();
  }
  ik_options_ = ik_options.value_or([&] {
    roboplan::SimpleIkOptions opts;
    opts.group_name = rrt_options_.group_name;
    return opts;
  }());
  single_shot_ik_options_ =
      single_shot_ik_options.value_or(ik_options_);
}

void RoboPlanPlannerBackend::request_cancel() {
  cancel_requested_.store(true, std::memory_order_release);
}

core::PlanResultDTO RoboPlanPlannerBackend::plan(const core::PlanRequestDTO& request) {
  core::PlanResultDTO result;
  cancel_requested_.store(false, std::memory_order_release);
  const auto plan_started_at = std::chrono::steady_clock::now();
  const bool has_planning_deadline =
      std::isfinite(request.planning_timeout_s) && request.planning_timeout_s > 0.0;

  try {
    if (!request.scene_binding.valid()) {
      result.error = core::ErrorDTO{
          core::kErrorInvalidRequest,
          "Planning request does not carry a valid scene projection binding",
          {{"reason", "planner_scene_binding_invalid"}}};
      return result;
    }
    auto planning_scene =
        projection_->acquire_native_projection(request.scene_binding);
    if (!planning_scene) {
      result.error = core::ErrorDTO{
          core::kErrorSceneChangedDuringValidation,
          "The retained planner scene projection is unavailable",
          {{"reason", "planner_projection_lease_unavailable"},
           {"projection_lease_id",
            request.scene_binding.projection_lease_id},
           {"requested_revision_id",
            std::to_string(request.scene_binding.revision.revision_id)}}};
      return result;
    }
    auto planning_ik =
        std::make_shared<roboplan::SimpleIk>(planning_scene, ik_options_);
    auto planning_shortcutter =
        std::make_shared<roboplan::PathShortcutter>(
            planning_scene, rrt_options_.group_name);
    auto planning_toppra =
        std::make_shared<roboplan::PathParameterizerTOPPRA>(
            planning_scene, rrt_options_.group_name);

    const auto fail_if_cancelled = [&](const std::string& stage) {
      if (!cancel_requested_.load(std::memory_order_acquire)) {
        return false;
      }
      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          "planning cancelled by interrupt",
          {{"reason", "planning_cancelled_by_interrupt"},
           {"failure_stage", stage}}};
      return true;
    };
    const auto fail_if_timed_out = [&](const std::string& stage) {
      if (!has_planning_deadline) {
        return false;
      }
      const auto elapsed = std::chrono::duration<double>(
          std::chrono::steady_clock::now() - plan_started_at);
      if (elapsed.count() < request.planning_timeout_s) {
        return false;
      }
      result.error = core::ErrorDTO{
          core::kErrorPlanningTimeout,
          "planning timed out",
          {{"reason", "planning_timeout"},
           {"failure_stage", stage},
           {"timeout_s", std::to_string(request.planning_timeout_s)},
           {"elapsed_s", std::to_string(elapsed.count())}}};
      return true;
    };
    const auto fail_if_cancelled_or_timed_out = [&](const std::string& stage) {
      return fail_if_cancelled(stage) || fail_if_timed_out(stage);
    };

    planner_debug("[PLANNER] plan() called, move_id={}, motion_type={}\n",
            request.move_id,
            std::holds_alternative<core::JointGoalDTO>(request.motion) ? "joint" : "pose");

    // Extract start state
    const auto& start = request.start_state;
    Eigen::VectorXd start_positions(start.positions_rad.size());
    for (size_t i = 0; i < start.positions_rad.size(); ++i) {
      start_positions[i] = start.positions_rad[i];
    }

    planner_debug("[PLANNER] start joints: [");
    for (size_t i = 0; i < start.positions_rad.size(); ++i)
      planner_debug("{}{:.4f}", (i ? ", " : ""), start.positions_rad[i]);
    planner_debug("]\n");

    roboplan::JointConfiguration start_config;
    start_config.joint_names = start.joint_names;
    start_config.positions = start_positions;

    const Eigen::VectorXd start_positions_full =
        planning_scene->toFullJointPositions(rrt_options_.group_name,
                                             start_positions);

    // Check start for self-collision
    bool start_collision = planning_scene->hasCollisions(start_positions_full);
    bool goal_collision = false;
    if (fail_if_cancelled_or_timed_out("start_collision_check")) {
      return result;
    }

    // FK of start pose
    {
      Eigen::Matrix4d start_fk = planning_scene->forwardKinematics(
          start_positions_full, request.tcp_frame);
      double r, p, ya;
      matrix_to_rpy_deg(start_fk, r, p, ya);
      planner_debug(
          "[PLANNER] start pose:  pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f}) collision={}\n",
          start_fk(0, 3), start_fk(1, 3), start_fk(2, 3), r, p, ya,
          start_collision ? "YES" : "no");
    }

    if (start_collision) {
      auto diagnostics =
          collect_collision_diagnostics(planning_scene, start_positions_full);
      const std::string message = collision_message_with_pairs(
          "Planning preview rejected: start state is already in collision",
          diagnostics);
      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          message,
          collision_details("start_state", true, false, diagnostics),
      };
      return result;
    }

    // Determine goal configuration based on motion type
    roboplan::JointConfiguration goal_config;

    if (auto* joint_goal = std::get_if<core::JointGoalDTO>(&request.motion)) {
      // Joint goal — direct conversion
      Eigen::VectorXd goal_positions(joint_goal->positions_rad.size());
      for (size_t i = 0; i < joint_goal->positions_rad.size(); ++i) {
        goal_positions[i] = joint_goal->positions_rad[i];
      }
      goal_config.joint_names = joint_goal->joint_names;
      goal_config.positions = goal_positions;

    } else if (auto* pose_goal = std::get_if<core::PoseGoalDTO>(&request.motion)) {
      Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
      tform(0, 3) = pose_goal->goal.position_m.x_m;
      tform(1, 3) = pose_goal->goal.position_m.y_m;
      tform(2, 3) = pose_goal->goal.position_m.z_m;

      // Quaternion to rotation matrix
      double x = pose_goal->goal.orientation.x;
      double y = pose_goal->goal.orientation.y;
      double z = pose_goal->goal.orientation.z;
      double w = pose_goal->goal.orientation.w;
      tform(0, 0) = 1 - 2*y*y - 2*z*z;
      tform(0, 1) = 2*x*y - 2*z*w;
      tform(0, 2) = 2*x*z + 2*y*w;
      tform(1, 0) = 2*x*y + 2*z*w;
      tform(1, 1) = 1 - 2*x*x - 2*z*z;
      tform(1, 2) = 2*y*z - 2*x*w;
      tform(2, 0) = 2*x*z - 2*y*w;
      tform(2, 1) = 2*y*z + 2*x*w;
      tform(2, 2) = 1 - 2*x*x - 2*y*y;

      {
        double r, p, ya;
        matrix_to_rpy_deg(tform, r, p, ya);
        planner_debug(
            "[PLANNER] goal pose:   pos=({:.3f}, {:.3f}, {:.3f}) rpy=({:.1f}, {:.1f}, {:.1f})\n",
            tform(0, 3), tform(1, 3), tform(2, 3), r, p, ya);
      }

      roboplan::CartesianConfiguration cart_config;
      cart_config.base_frame = request.base_frame;
      cart_config.tip_frame = request.tcp_frame;
      cart_config.tform = tform;

      goal_config.joint_names = start.joint_names;
      goal_config.positions = start_positions;  // seed with current

      if (!planning_ik->solveIk(cart_config, start_config, goal_config)) {
        double r, p, ya;
        matrix_to_rpy_deg(tform, r, p, ya);
        auto str = fmt::format(
            "IK failed: goal pos=({:.3f},{:.3f},{:.3f}) rpy=({:.1f},{:.1f},{:.1f}) frame={}->{}",
            tform(0, 3), tform(1, 3), tform(2, 3), r, p, ya,
            request.base_frame, request.tcp_frame);
        result.error = core::ErrorDTO{core::kErrorIkFailed, str, {}};
        planner_debug("[PLANNER] IK FAILED\n");
        return result;
      }
      if (fail_if_cancelled_or_timed_out("ik")) {
        return result;
      }

      planner_debug("[PLANNER] IK solved. goal: [");
      for (int i = 0; i < goal_config.positions.size(); ++i)
        planner_debug("{}{:.4f}", (i ? ", " : ""), goal_config.positions[i]);
      planner_debug("]\n");

      normalize_ik_goal_for_rrt_limits(
          planning_scene, rrt_options_.group_name, start_config.positions,
          goal_config.positions);

      // Check goal for self-collision
      const Eigen::VectorXd goal_positions_full =
          planning_scene->toFullJointPositions(
              rrt_options_.group_name, goal_config.positions);
      goal_collision = planning_scene->hasCollisions(goal_positions_full);
      planner_debug("[PLANNER] goal in collision: {}\n",
                    goal_collision ? "YES" : "no");
      if (fail_if_cancelled_or_timed_out("goal_collision_check")) {
        return result;
      }

      if (goal_collision) {
        auto diagnostics =
            collect_collision_diagnostics(planning_scene, goal_positions_full);
        const std::string message = collision_message_with_pairs(
            "Planning preview rejected: goal state is in collision",
            diagnostics);
        result.error = core::ErrorDTO{
            core::kErrorPlanningFailed,
            message,
            collision_details("goal_state", false, true, diagnostics),
        };
        return result;
      }

      // Compute joint-space distance
      double jdist = (goal_config.positions - start_config.positions).norm();
      planner_debug("[PLANNER] joint-space distance start->goal: {:.4f} rad\n", jdist);

    } else {
      result.error = core::ErrorDTO{core::kErrorInvalidRequest,
                                     "Unsupported motion type for native planner", {}};
      return result;
    }

    // Per-joint dynamics caps (Phase 4-P5.1K). The deployment/operator caps in
    // request.per_joint_dynamics_limits are ONE source only; the conservative
    // effective limit is min(model, cap) per joint per dimension. This backend
    // does NOT feed per-joint vectors into TOPP-RA: TOPP-RA receives only scalar
    // velocity/acceleration scales. Instead we enforce the absolute caps with a
    // conservative GLOBAL retiming loop (below) that slows the whole trajectory
    // and then VERIFIES the result by sampling per-joint peak velocity and
    // acceleration against the caps. This is approximate-but-conservative
    // (the retiming can only make motion slower, never exceed a cap), and it
    // fails closed when a cap cannot be met or verified. We validate the cap
    // map up front and fail closed before spending the RRT budget.
    //
    // The active planning group's joint names: the kernel populates start_state
    // with exactly the joints this plan drives, and the produced trajectory is
    // over the same set. A cap keyed by a joint outside this set (a typo, or a
    // joint that belongs to a different group) cannot be enforced by this plan,
    // so we reject it rather than silently ignoring it.
    const std::vector<std::string>& group_joint_names = start.joint_names;
    auto is_active_group_joint = [&](const std::string& jn) {
      return std::find(group_joint_names.begin(), group_joint_names.end(), jn) !=
             group_joint_names.end();
    };
    std::map<std::string, double> vel_cap_by_name;  // rad/s, strictly positive
    std::map<std::string, double> acc_cap_by_name;  // rad/s^2, strictly positive
    for (const auto& [joint_name, limit] : request.per_joint_dynamics_limits) {
      // Unknown / non-active joint name -> fail closed (never a silent no-op).
      if (!is_active_group_joint(joint_name)) {
        result.error = core::ErrorDTO{
            core::kErrorUnsupportedCapability,
            "per-joint dynamics cap names a joint that is not in the active "
            "planning group",
            {{"reason", "unsupported_joint_dynamics_limit"},
             {"joint", joint_name},
             {"group", rrt_options_.group_name},
             {"failure_stage", "request_validation"}}};
        return result;
      }
      // Jerk caps: validate the value first, THEN report enforceability. A
      // malformed jerk (NaN/inf/<=0) is an invalid request regardless of
      // whether the active path could enforce it; only a finite, strictly
      // positive jerk cap is "well-formed but unsupported" on the discrete
      // TOPP-RA path (this backend constrains jerk for no joint).
      if (limit.max_jerk_rad_s3.has_value()) {
        const double j = *limit.max_jerk_rad_s3;
        if (!std::isfinite(j) || j <= 0.0) {
          result.error = core::ErrorDTO{
              core::kErrorInvalidRequest,
              "per-joint max_jerk_rad_s3 must be finite and strictly positive",
              {{"reason", "invalid_joint_dynamics_limit"},
               {"joint", joint_name},
               {"dimension", "jerk"}}};
          return result;
        }
        result.error = core::ErrorDTO{
            core::kErrorUnsupportedCapability,
            "per-joint jerk caps are not enforceable by the discrete TOPP-RA "
            "time-parameterization path",
            {{"reason", "unsupported_joint_dynamics_limit"},
             {"joint", joint_name},
             {"dimension", "jerk"},
             {"backend", "roboplan_toppra"}}};
        return result;
      }
      if (limit.max_velocity_rad_s.has_value()) {
        const double v = *limit.max_velocity_rad_s;
        if (!std::isfinite(v) || v <= 0.0) {
          result.error = core::ErrorDTO{
              core::kErrorInvalidRequest,
              "per-joint max_velocity_rad_s must be finite and strictly positive",
              {{"reason", "invalid_joint_dynamics_limit"},
               {"joint", joint_name},
               {"dimension", "velocity"}}};
          return result;
        }
        vel_cap_by_name[joint_name] = v;
      }
      if (limit.max_acceleration_rad_s2.has_value()) {
        const double a = *limit.max_acceleration_rad_s2;
        if (!std::isfinite(a) || a <= 0.0) {
          result.error = core::ErrorDTO{
              core::kErrorInvalidRequest,
              "per-joint max_acceleration_rad_s2 must be finite and strictly "
              "positive",
              {{"reason", "invalid_joint_dynamics_limit"},
               {"joint", joint_name},
               {"dimension", "acceleration"}}};
          return result;
        }
        acc_cap_by_name[joint_name] = a;
      }
    }
    const bool has_joint_dynamics_caps =
        !vel_cap_by_name.empty() || !acc_cap_by_name.empty();

    // Plan path with an RRT instance configured for this request's timeout.
    // This gives the native backend a hard, cooperative planning bound instead
    // of relying only on a post-hoc stop request in the kernel.
    auto request_rrt_options = rrt_options_;
    if (request.planning_timeout_s > 0.0) {
      if (request_rrt_options.max_planning_time > 0.0) {
        request_rrt_options.max_planning_time =
            std::min(request_rrt_options.max_planning_time,
                     request.planning_timeout_s);
      } else {
        request_rrt_options.max_planning_time = request.planning_timeout_s;
      }
    }
    roboplan::RRT request_rrt(planning_scene, request_rrt_options);

    // Plan path with RRT
    if (fail_if_cancelled_or_timed_out("before_rrt")) {
      return result;
    }
    planner_debug("[PLANNER] calling RRT...\n");
    auto path_result = request_rrt.plan(start_config, goal_config);
    if (fail_if_cancelled_or_timed_out("rrt")) {
      return result;
    }
    if (!path_result.has_value()) {
      planner_debug("[PLANNER] RRT FAILED: {}\n", path_result.error());

      // Build detailed error message
      std::string err_msg = "RRT failed: " + path_result.error();
      err_msg += " | start=[";
      for (int i = 0; i < start_config.positions.size(); ++i) {
        if (i) err_msg += ",";
        err_msg += fmt::format("{:.3f}", start_config.positions[i]);
      }
      err_msg += "] goal=[";
      for (int i = 0; i < goal_config.positions.size(); ++i) {
        if (i) err_msg += ",";
        err_msg += fmt::format("{:.3f}", goal_config.positions[i]);
      }
      err_msg += "]";

      result.error = core::ErrorDTO{
          core::kErrorPlanningFailed,
          err_msg,
          {
              {"failure_stage", "rrt"},
              {"start_in_collision", start_collision ? "true" : "false"},
              {"goal_in_collision", goal_collision ? "true" : "false"},
          },
      };
      return result;
    }
    planner_debug("[PLANNER] RRT succeeded\n");

    // Shortcut the path
    if (fail_if_cancelled_or_timed_out("before_shortcut")) {
      return result;
    }
    auto smoothed_path = planning_shortcutter->shortcut(path_result.value(), 0.05);
    if (fail_if_cancelled_or_timed_out("shortcut")) {
      return result;
    }

    // Time-parameterize with TOPPRA. TOPP-RA itself takes only SCALAR velocity
    // and acceleration scales — there is no per-joint vector-limit input on this
    // path. We therefore enforce the global TCP linear-speed limit and the
    // per-joint velocity/acceleration caps (Phase 4-P5.1K) with a conservative
    // GLOBAL retiming loop: each pass generates a trajectory, then VERIFIES it
    // by sampling per-joint peak velocity/acceleration (and TCP speed) and, if
    // any cap is exceeded, slows the whole trajectory and regenerates. TCP speed
    // and per-joint velocity caps pull down velocity_scale; per-joint
    // acceleration caps pull down acceleration_scale and (for the velocity²
    // centripetal component) velocity_scale by sqrt(ratio). velocity_scale
    // starts at the operator value and is only ever reduced, so the resolved
    // absolute caps bind regardless of the operator scale (cap resolved first;
    // velocity_scale applies on top); the acceleration cap is never multiplied
    // by the velocity scale. This is approximate-but-conservative (retiming only
    // ever slows motion) and fails closed if a cap cannot be met within the
    // iteration budget or cannot be verified for lack of samples.
    const double tcp_limit = request.speed_profile.max_tcp_linear_speed_mps.value_or(0.0);
    const std::string& tcp_frame = request.tcp_frame;
    const bool has_tcp_limit = tcp_limit > 0.0 && !tcp_frame.empty();
    // TOPPRA's spline samples can overshoot a nominal model limit by a small
    // interpolation/numerical amount at scale 1.0. Keep the producer inside
    // the authoritative post-plan validator instead of adding a permissive
    // validation tolerance. This margin applies to the model envelope before
    // any stricter request-level cap retiming below.
    constexpr double kModelLimitQualificationMargin = 0.995;
    double vel_scale = request.speed_profile.velocity_scale *
                       kModelLimitQualificationMargin;
    double acc_scale = request.speed_profile.acceleration_scale *
                       kModelLimitQualificationMargin;
    constexpr int kMaxRetimeIterations = 32;
    constexpr double kSpeedMargin = 0.99;  // aim slightly under each limit

    roboplan::JointTrajectory final_traj;
    TcpSpeedCheck final_tcp_check;
    int retime_iterations_used = 0;
    // Worst measured/cap ratios on the final pass (> 1.0 == still violating).
    double final_vel_cap_ratio = 1.0;
    double final_acc_cap_ratio = 1.0;
    std::string final_vel_cap_joint;
    std::string final_acc_cap_joint;
    double final_vel_cap_peak = 0.0;
    double final_acc_cap_peak = 0.0;

    for (int iter = 0; iter < kMaxRetimeIterations; ++iter) {
      if (fail_if_cancelled_or_timed_out("retime")) {
        return result;
      }
      retime_iterations_used = iter + 1;
      auto traj_result = planning_toppra->generate(
          smoothed_path, 0.01,
          roboplan::SplineFittingMode::Hermite,
          vel_scale,
          acc_scale);

      if (!traj_result.has_value()) {
        result.error = core::ErrorDTO{core::kErrorPlanningFailed,
                                       "TOPPRA failed: " + traj_result.error(), {}};
        return result;
      }
      if (fail_if_cancelled_or_timed_out("retime_generate")) {
        return result;
      }

      final_traj = traj_result.value();
      anchor_trajectory_endpoints(
          final_traj, start_config.positions, goal_config.positions);
      if (fail_if_cancelled_or_timed_out("retime_verify")) {
        return result;
      }

      // --- Global TCP linear speed ---
      bool tcp_ok = true;
      if (has_tcp_limit) {
        final_tcp_check = compute_tcp_speed_check(
            planning_scene, rrt_options_.group_name, final_traj, tcp_frame);
        if (fail_if_cancelled_or_timed_out("tcp_speed_verify")) {
          return result;
        }
        tcp_ok = final_tcp_check.max_speed_mps <= tcp_limit;
      }

      // --- Per-joint velocity / acceleration caps ---
      // Sample the generated trajectory and compare peaks against the caps. If a
      // capped dimension lacks samples for a capped joint, the cap cannot be
      // verified -> fail closed (do NOT treat the missing peak as zero, which
      // would falsely pass the cap).
      double vel_cap_ratio = 1.0;
      double acc_cap_ratio = 1.0;
      final_vel_cap_joint.clear();
      final_acc_cap_joint.clear();
      final_vel_cap_peak = 0.0;
      final_acc_cap_peak = 0.0;
      if (has_joint_dynamics_caps) {
        const JointCapMeasurement meas =
            measure_joint_cap_peaks(final_traj, vel_cap_by_name, acc_cap_by_name);
        if (fail_if_cancelled_or_timed_out("joint_dynamics_verify")) {
          return result;
        }
        if (!meas.samples_complete) {
          result.error = core::ErrorDTO{
              core::kErrorUnsupportedCapability,
              "trajectory lacks the samples required to verify a per-joint "
              "dynamics cap; cannot prove enforcement",
              {{"reason", "unsupported_joint_dynamics_limit"},
               {"failure_stage", "time_parameterization"},
               {"dimension", meas.incomplete_dimension},
               {"joint", meas.incomplete_joint}}};
          return result;
        }
        vel_cap_ratio = meas.worst_vel_ratio;
        acc_cap_ratio = meas.worst_acc_ratio;
        final_vel_cap_joint = meas.worst_vel_joint;
        final_acc_cap_joint = meas.worst_acc_joint;
        final_vel_cap_peak = meas.worst_vel_peak;
        final_acc_cap_peak = meas.worst_acc_peak;
      }
      final_vel_cap_ratio = vel_cap_ratio;
      final_acc_cap_ratio = acc_cap_ratio;
      const bool joints_ok = vel_cap_ratio <= 1.0 && acc_cap_ratio <= 1.0;

      if (tcp_ok && joints_ok) {
        break;
      }

      // Reduce scales conservatively so every constraint moves toward
      // satisfaction together; take the most aggressive factor on each lever.
      // TCP speed and per-joint velocity act ~linearly on vel_scale. Per-joint
      // acceleration has a tangential component (∝ acc_scale) and a centripetal
      // component (∝ traversal speed²), so an acceleration violation pulls down
      // acc_scale (tangential) AND vel_scale by sqrt(ratio) (centripetal) — the
      // square root because the velocity-squared term scales the peak by
      // vel_scale².
      double vel_factor = 1.0;
      double acc_factor = 1.0;
      if (has_tcp_limit && !tcp_ok) {
        vel_factor = std::min(
            vel_factor, (tcp_limit * kSpeedMargin) / final_tcp_check.max_speed_mps);
      }
      if (vel_cap_ratio > 1.0) {
        vel_factor = std::min(vel_factor, kSpeedMargin / vel_cap_ratio);
      }
      if (acc_cap_ratio > 1.0) {
        acc_factor = std::min(acc_factor, kSpeedMargin / acc_cap_ratio);
        vel_factor =
            std::min(vel_factor, kSpeedMargin / std::sqrt(acc_cap_ratio));
      }
      if (vel_factor < 1.0) {
        vel_scale *= vel_factor;
      }
      if (acc_factor < 1.0) {
        acc_scale *= acc_factor;
      }
      if (vel_scale < 0.001) vel_scale = 0.001;
      if (acc_scale < 0.001) acc_scale = 0.001;
    }

    if (has_tcp_limit && final_tcp_check.max_speed_mps > tcp_limit) {
      std::map<std::string, std::string> details;
      details["failure_stage"] = "time_parameterization";
      details["limit_type"] = "tcp_linear_speed";
      details["measured"] = std::to_string(final_tcp_check.max_speed_mps);
      details["limit"] = std::to_string(tcp_limit);
      details["units"] = "m/s";
      details["tcp_frame"] = tcp_frame;
      details["waypoint_count"] = std::to_string(final_traj.times.size());
      details["trajectory_duration_s"] =
          final_traj.times.empty() ? "0.000000" : std::to_string(final_traj.times.back());
      details["max_segment_index"] = std::to_string(final_tcp_check.segment_index);
      details["max_segment_dt_s"] = std::to_string(final_tcp_check.segment_dt_s);
      details["toppra_iterations"] = std::to_string(retime_iterations_used);
      details["final_velocity_scale"] = std::to_string(vel_scale);
      result.error = core::ErrorDTO{
          core::kErrorValidationLimitsViolation,
          "TCP linear speed exceeds limit after time-parameterization",
          details};
      return result;
    }

    if (has_joint_dynamics_caps &&
        (final_vel_cap_ratio > 1.0 || final_acc_cap_ratio > 1.0)) {
      std::map<std::string, std::string> details;
      details["failure_stage"] = "time_parameterization";
      details["reason"] = "unsupported_joint_dynamics_limit";
      details["toppra_iterations"] = std::to_string(retime_iterations_used);
      details["final_velocity_scale"] = std::to_string(vel_scale);
      details["final_acceleration_scale"] = std::to_string(acc_scale);
      details["trajectory_duration_s"] =
          final_traj.times.empty() ? "0.000000" : std::to_string(final_traj.times.back());
      if (final_vel_cap_ratio >= final_acc_cap_ratio) {
        details["limit_type"] = "joint_velocity";
        details["joint"] = final_vel_cap_joint;
        details["measured"] = std::to_string(final_vel_cap_peak);
        details["limit"] = std::to_string(vel_cap_by_name[final_vel_cap_joint]);
        details["units"] = "rad/s";
      } else {
        details["limit_type"] = "joint_acceleration";
        details["joint"] = final_acc_cap_joint;
        details["measured"] = std::to_string(final_acc_cap_peak);
        details["limit"] = std::to_string(acc_cap_by_name[final_acc_cap_joint]);
        details["units"] = "rad/s^2";
      }
      result.error = core::ErrorDTO{
          core::kErrorUnsupportedCapability,
          "per-joint dynamics cap could not be enforced by conservative "
          "retiming within the iteration budget",
          details};
      return result;
    }

    if (fail_if_cancelled_or_timed_out("trajectory_conversion")) {
      return result;
    }

    // Convert to TrajectoryDTO
    result.trajectory = convert_trajectory(final_traj);
    result.scene_revision = request.scene_binding.revision;

  } catch (const std::exception& e) {
    result.error = core::ErrorDTO{core::kErrorPlanningFailed, e.what(), {}};
  }

  return result;
}

core::TrajectoryDTO RoboPlanPlannerBackend::convert_trajectory(
    const roboplan::JointTrajectory& traj) {
  core::TrajectoryDTO dto;
  dto.joint_names = traj.joint_names;

  for (size_t i = 0; i < traj.times.size(); ++i) {
    core::TrajectoryWaypointDTO wp;
    wp.time_from_start_s = traj.times[i];

    wp.positions_rad.resize(traj.positions[i].size());
    for (Eigen::Index j = 0; j < traj.positions[i].size(); ++j) {
      wp.positions_rad[j] = traj.positions[i][j];
    }

    if (i < traj.velocities.size()) {
      wp.velocities_rad_s.resize(traj.velocities[i].size());
      for (Eigen::Index j = 0; j < traj.velocities[i].size(); ++j) {
        wp.velocities_rad_s[j] = traj.velocities[i][j];
      }
    }

    if (i < traj.accelerations.size()) {
      wp.accelerations_rad_s2.resize(traj.accelerations[i].size());
      for (Eigen::Index j = 0; j < traj.accelerations[i].size(); ++j) {
        wp.accelerations_rad_s2[j] = traj.accelerations[i][j];
      }
    }

    dto.waypoints.push_back(std::move(wp));
  }

  return dto;
}

// =================================================================
// Single-shot IK (IPlannerBackend::solve_ik).
//
// Uses a separate low-budget SimpleIk solver when configured. Full plan()
// keeps the planning IK budget; solve_ik() is bounded for interactive
// (e.g. validated candidate Cartesian authoring) responsiveness.
// Distinct from plan(): no trajectory shaping, no collision check, no
// RRT — just a pose-to-joint resolve with the previous-frame joints as
// seed for continuity across streamed frames.
// =================================================================
core::IkResultDTO RoboPlanPlannerBackend::solve_ik(
    const core::IkRequestDTO& request) {
  core::IkResultDTO result;

  if (!single_shot_ik_) {
    result.ok = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kUnsupportedCapability)),
        "no IK solver configured on this planner backend",
        {}};
    return result;
  }

  // Build the 4x4 target transform from the requested pose. Same conversion
  // used in plan() above; factored inline to avoid lifting a helper.
  Eigen::Matrix4d tform = Eigen::Matrix4d::Identity();
  tform(0, 3) = request.goal_pose.position_m.x_m;
  tform(1, 3) = request.goal_pose.position_m.y_m;
  tform(2, 3) = request.goal_pose.position_m.z_m;
  const double x = request.goal_pose.orientation.x;
  const double y = request.goal_pose.orientation.y;
  const double z = request.goal_pose.orientation.z;
  const double w = request.goal_pose.orientation.w;
  tform(0, 0) = 1 - 2 * y * y - 2 * z * z;
  tform(0, 1) = 2 * x * y - 2 * z * w;
  tform(0, 2) = 2 * x * z + 2 * y * w;
  tform(1, 0) = 2 * x * y + 2 * z * w;
  tform(1, 1) = 1 - 2 * x * x - 2 * z * z;
  tform(1, 2) = 2 * y * z - 2 * x * w;
  tform(2, 0) = 2 * x * z - 2 * y * w;
  tform(2, 1) = 2 * y * z + 2 * x * w;
  tform(2, 2) = 1 - 2 * x * x - 2 * y * y;

  roboplan::CartesianConfiguration cart_config;
  cart_config.base_frame = request.base_frame;
  cart_config.tip_frame = request.tcp_frame;
  cart_config.tform = tform;

  // Seed must come from the caller. The kernel populates request.seed_state
  // from the live observed-state provider before invoking solve_ik. Empty seeds
  // fail fast — the IK solver needs a starting configuration for continuity.
  if (request.seed_state.joint_names.empty() ||
      request.seed_state.joint_names.size() !=
          request.seed_state.positions_rad.size()) {
    result.ok = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
        "solve_ik requires a non-empty seed_state with matching joint names "
        "and positions; the kernel supplies the selected observed state",
        {}};
    return result;
  }

  roboplan::JointConfiguration start_config;
  roboplan::JointConfiguration goal_config;
  Eigen::VectorXd seed_positions(request.seed_state.positions_rad.size());
  for (size_t i = 0; i < request.seed_state.positions_rad.size(); ++i) {
    seed_positions[i] = request.seed_state.positions_rad[i];
  }
  start_config.joint_names = request.seed_state.joint_names;
  start_config.positions = seed_positions;
  goal_config.joint_names = start_config.joint_names;
  goal_config.positions = start_config.positions;  // seed with start

  std::shared_ptr<roboplan::Scene> ik_scene;
  {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    ik_scene = std::make_shared<roboplan::Scene>(*live_scene_);
  }
  auto single_shot_ik =
      std::make_shared<roboplan::SimpleIk>(ik_scene, single_shot_ik_options_);
  if (!single_shot_ik->solveIk(cart_config, start_config, goal_config)) {
    result.ok = false;
    result.error = core::ErrorDTO{
        std::string(core::to_string(core::ErrorCode::kIkFailed)),
        "IK failed for requested target",
        {{"base_frame", request.base_frame},
         {"tcp_frame", request.tcp_frame}}};
    return result;
  }

  result.ok = true;
  result.joint_names = goal_config.joint_names;
  result.positions_rad.resize(goal_config.positions.size());
  for (Eigen::Index i = 0; i < goal_config.positions.size(); ++i) {
    result.positions_rad[i] = goal_config.positions[i];
  }
  return result;
}

}  // namespace manyforge::roboplan_backend

#pragma once

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>

#include <roboplan/core/scene.hpp>
#include <roboplan/core/path_utils.hpp>
#include <roboplan/core/types.hpp>
#include <roboplan_rrt/rrt.hpp>
#include <roboplan_toppra/toppra.hpp>
#include <roboplan_simple_ik/simple_ik.hpp>

#include <manyforge_core/ports/iplanner_backend.hpp>

namespace manyforge::roboplan_backend {

class RoboPlanSceneProjection;

/// Result of sampling per-joint peak velocity/acceleration against requested
/// absolute caps (Phase 4-P5.1K). Consumed by the conservative retiming loop in
/// RoboPlanPlannerBackend::plan and exercised directly by unit tests.
struct JointCapMeasurement {
  /// False when a capped dimension lacked a complete set of samples for a capped
  /// joint (the cap cannot be verified, so the caller MUST fail closed rather
  /// than treat the missing peak as zero). `incomplete_dimension` /
  /// `incomplete_joint` name the first offending case.
  bool samples_complete = true;
  std::string incomplete_dimension;  // "velocity" | "acceleration"
  std::string incomplete_joint;
  /// Worst measured/cap ratio per dimension across capped joints; > 1.0 means a
  /// cap is still violated on this pass. 1.0 when satisfied (or no cap).
  double worst_vel_ratio = 1.0;
  double worst_acc_ratio = 1.0;
  std::string worst_vel_joint;
  std::string worst_acc_joint;
  double worst_vel_peak = 0.0;  // rad/s
  double worst_acc_peak = 0.0;  // rad/s^2
};

/// Optional qualification-only collision counter sampled at planner stage
/// boundaries. Production adapters leave the probe unavailable; benchmark
  /// builds can provide an interposed RoboPlan Scene/CollisionContext counter
  /// without changing planner behavior or linking two RoboPlan versions into
  /// one process.
struct RoboPlanCollisionProbeSnapshot {
  bool available = false;
  std::uint64_t query_count = 0;
  std::uint64_t query_duration_ns = 0;
};

struct RoboPlanAllocationProbeSnapshot {
  bool available = false;
  std::uint64_t allocation_count = 0;
  std::uint64_t allocation_bytes = 0;
};

struct RoboPlanPlanningStageMetrics {
  std::uint64_t duration_ns = 0;
  std::uint64_t collision_query_count = 0;
  std::uint64_t collision_query_duration_ns = 0;
  std::uint64_t allocation_count = 0;
  std::uint64_t allocation_bytes = 0;
};

/// One immutable planner invocation measurement. It intentionally contains
/// backend-stage facts only; dispatch, authoritative validation, and runtime
/// collision evidence remain owned by their respective ManyForge surfaces.
struct RoboPlanPlanningPipelineMetrics {
  std::map<std::string, RoboPlanPlanningStageMetrics> stages;
  std::uint64_t total_duration_ns = 0;
  std::uint64_t total_collision_query_count = 0;
  std::uint64_t total_collision_query_duration_ns = 0;
  bool collision_probe_available = false;
  bool measurement_complete = true;
  bool succeeded = false;
  std::string error_code;
  std::size_t waypoint_count = 0;
  double trajectory_duration_s = 0.0;
  std::optional<std::uint32_t> rng_base_seed;
  std::optional<std::uint32_t> rng_seed;
  std::optional<int> shortcut_rng_seed_argument;
  std::uint32_t planning_attempt_index = 0;
};

/// NRT qualification observer. Implementations must be thread-safe and must
/// not throw. The planner calls this surface only when enabled; production
/// factories do not install an observer.
class IRoboPlanPlanningPerformanceObserver {
 public:
  virtual ~IRoboPlanPlanningPerformanceObserver() = default;
  virtual RoboPlanCollisionProbeSnapshot collision_probe_snapshot()
      const noexcept = 0;
  /// Optional NRT qualification hook. Production factories do not install an
  /// observer, so allocation attribution adds no production-path work.
  virtual RoboPlanAllocationProbeSnapshot allocation_probe_snapshot()
      const noexcept {
    return {};
  }
  /// Atomically admit one invocation into the observer's lifetime. Returning
  /// false excludes the invocation from measurement after capture admission
  /// has closed, even if the planner retained an older observer snapshot.
  virtual bool try_begin_invocation(const core::PlanRequestDTO&) noexcept {
    return true;
  }
  virtual void record(
      const core::PlanRequestDTO& request,
      const core::PlanResultDTO& result,
      const RoboPlanPlanningPipelineMetrics& metrics) noexcept = 0;
};

/// Sample peak |velocity| / |acceleration| per capped joint from `traj` and
/// compare against the requested absolute caps. Caps are keyed by joint name;
/// only joints present in `traj.joint_names` are measured. If a capped
/// dimension is missing a sample for any waypoint of a capped joint, the
/// returned measurement has `samples_complete == false` and records the
/// offending dimension/joint (the trajectory cannot prove cap compliance).
JointCapMeasurement measure_joint_cap_peaks(
    const roboplan::JointTrajectory& traj,
    const std::map<std::string, double>& vel_cap_by_name,
    const std::map<std::string, double>& acc_cap_by_name);

/// Native C++ planner backend wrapping RoboPlan RRT + PathShortcutter + TOPPRA + IK.
/// No Python trampolines — all calls stay in C++.
class RoboPlanPlannerBackend final : public core::IPlannerBackend {
 public:
  RoboPlanPlannerBackend(
      std::shared_ptr<RoboPlanSceneProjection> projection,
      std::shared_ptr<roboplan::Scene> live_scene,
      const roboplan::RRTOptions& rrt_options,
      std::shared_ptr<std::mutex> scene_mutex = nullptr,
      std::optional<roboplan::SimpleIkOptions> ik_options = std::nullopt,
      std::optional<roboplan::SimpleIkOptions> single_shot_ik_options =
          std::nullopt,
      std::optional<std::uint32_t> rng_seed = std::nullopt,
      std::shared_ptr<IRoboPlanPlanningPerformanceObserver>
          performance_observer = nullptr);

  core::PlanResultDTO plan(const core::PlanRequestDTO& request) override;

  void request_cancel() override;

  void set_performance_observer(
      std::shared_ptr<IRoboPlanPlanningPerformanceObserver> observer) noexcept;

  core::IkResultDTO solve_ik(const core::IkRequestDTO& request) override;

 private:
  core::TrajectoryDTO convert_trajectory(const roboplan::JointTrajectory& traj);

  std::shared_ptr<RoboPlanSceneProjection> projection_;
  std::shared_ptr<roboplan::Scene> live_scene_;
  roboplan::RRTOptions rrt_options_;
  std::shared_ptr<std::mutex> scene_mutex_;
  roboplan::SimpleIkOptions ik_options_;
  roboplan::SimpleIkOptions single_shot_ik_options_;
  std::optional<std::uint32_t> rng_seed_;
  std::atomic<std::shared_ptr<IRoboPlanPlanningPerformanceObserver>>
      performance_observer_;
  std::atomic_bool cancel_requested_{false};
};

}  // namespace manyforge::roboplan_backend

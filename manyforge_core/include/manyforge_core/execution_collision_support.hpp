#pragma once

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

std::string comma_join_strings(const std::vector<std::string>& values);
std::string comma_join_doubles(const std::vector<double>& values);
std::string format_double(double value);

bool snapshot_too_old(std::int64_t now_ns,
                      std::int64_t timestamp_ns,
                      std::int64_t max_age_ns,
                      std::map<std::string, std::string>& details,
                      const std::string& timestamp_key,
                      const std::string& age_key,
                      const std::string& budget_key);
std::optional<std::string> validate_dynamic_body_freshness(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<CollisionWorldBodyFreshness>& body_freshness,
    std::int64_t now_ns,
    std::int64_t runtime_cap_ns,
    std::map<std::string, std::string>& details);

struct RuntimeCollisionStateProjection {
  std::vector<std::string> source_joint_names;
  std::vector<std::string> query_joint_names;
  std::vector<double> template_positions_rad;
  std::vector<std::size_t> source_to_query_indices;
};

Result<RuntimeCollisionStateProjection> build_runtime_collision_state_projection(
    const TrajectoryDTO& source_trajectory,
    const TrajectoryDTO& collision_trajectory);
Result<RobotStateDTO> project_runtime_collision_observed_state(
    const RuntimeCollisionStateProjection& projection,
    const std::vector<double>& observed_positions_rad);

struct PublishedRuntimeCollisionSnapshot {
  CollisionWorldPolicySnapshotBundle bundle;
  std::uint64_t epoch = 0;
};

class PublishedRuntimeCollisionSnapshotStore {
 public:
  void publish(CollisionWorldPolicySnapshotBundle bundle);
  std::uint64_t clear();
  [[nodiscard]] PublishedRuntimeCollisionSnapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  CollisionWorldPolicySnapshotBundle bundle_;
  std::uint64_t epoch_ = 0;
};

Result<CollisionWorldPolicySnapshotBundle>
load_collision_world_policy_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);
Result<CollisionWorldPolicySnapshotBundle>
load_published_runtime_collision_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);

void append_collision_failure_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle);
void append_legacy_collision_compare_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle,
    const TrajectoryDTO& trajectory,
    IPlanningContext& context);
CollisionCheckResultDTO collision_check_result_from_bundle(
    const CollisionValidationDecisionBundle& bundle);

bool collision_world_revision_is_set(const CollisionWorldRevision& revision);
bool collision_world_revisions_equal(const CollisionWorldRevision& lhs,
                                     const CollisionWorldRevision& rhs);
bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines);
bool runtime_monitor_may_hold_collision_engine(ExecutionState state);

}  // namespace manyforge::core

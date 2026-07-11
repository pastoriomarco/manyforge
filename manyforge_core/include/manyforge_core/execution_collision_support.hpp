#pragma once

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_diagnostic_comparator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/observed_robot_state_publication.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/robot_state.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>
#include <manyforge_core/types/validation.hpp>

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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

enum class RuntimeDynamicBodyFreshnessStatus {
  kFresh,
  kSidecarUnsorted,
  kMissingTimestamp,
  kInvalidTimestamp,
  kStale,
};

struct RuntimeDynamicBodyFreshnessCheck {
  RuntimeDynamicBodyFreshnessStatus status =
      RuntimeDynamicBodyFreshnessStatus::kFresh;
  const CollisionWorldBody* body = nullptr;
  const CollisionWorldBodyFreshness* freshness = nullptr;
  std::int64_t effective_budget_ns = 0;
  std::int64_t age_ns = 0;
};

RuntimeDynamicBodyFreshnessCheck check_runtime_dynamic_body_freshness(
    const CollisionWorldSnapshot& snapshot,
    const std::vector<CollisionWorldBodyFreshness>& body_freshness,
    std::int64_t now_ns,
    std::int64_t runtime_cap_ns,
    bool lookup_order_prevalidated = false) noexcept;
const char* runtime_dynamic_body_freshness_reason(
    RuntimeDynamicBodyFreshnessStatus status) noexcept;

struct RuntimeCollisionStateProjection {
  std::vector<std::string> source_joint_names;
  std::vector<std::string> query_joint_names;
  std::vector<double> template_positions_rad;
  std::vector<std::size_t> source_to_query_indices;
};

Result<RuntimeCollisionStateProjection> build_runtime_collision_state_projection(
    const TrajectoryDTO& source_trajectory,
    const TrajectoryDTO& collision_trajectory);

struct RuntimeCollisionStopCoverageSummary {
  static constexpr std::size_t kUnavailableReasonCount = 9;
  std::uint64_t generation = 0;
  std::uint64_t ticks_attempted = 0;
  std::uint64_t queries_executed = 0;
  std::uint64_t unavailable_ticks = 0;
  std::uint64_t longest_unavailable_ns = 0;
  std::array<std::uint64_t, kUnavailableReasonCount>
      unavailable_by_reason{};
};

enum class RuntimeCollisionUnavailableReason : std::size_t {
  kCollisionResources = 0,
  kCollisionSnapshot,
  kDynamicBodyFreshness,
  kObservedStateUnavailable,
  kObservedStateStale,
  kObservedStateIncomplete,
  kObservedStateProjection,
  kCollisionQuery,
  kOther,
};
static_assert(
    static_cast<std::size_t>(RuntimeCollisionUnavailableReason::kOther) + 1 ==
    RuntimeCollisionStopCoverageSummary::kUnavailableReasonCount);

RuntimeCollisionUnavailableReason classify_runtime_collision_unavailable_reason(
    std::string_view reason) noexcept;
const char* runtime_collision_unavailable_reason_name(
    RuntimeCollisionUnavailableReason reason) noexcept;

struct RuntimeCollisionStopCoverageMetrics {
  std::uint64_t windows_started = 0;
  std::uint64_t windows_completed = 0;
  std::uint64_t ticks_attempted = 0;
  std::uint64_t queries_executed = 0;
  std::uint64_t unavailable_ticks = 0;
  std::uint64_t longest_unavailable_ns = 0;
  std::uint64_t zero_check_windows = 0;
  RuntimeCollisionStopCoverageSummary last_window;
  std::uint64_t freshest_source_age_ns = 0;
  int freshest_source_kind = -1;
  std::uint64_t unavailable_events_suppressed = 0;
};

/// Wait-free tick-side accounting for one generation-bound controlled-stop
/// monitor window. Lifecycle start/complete calls only reset/read atomics; no
/// runtime collision tick takes a telemetry or lifecycle lock.
class RuntimeCollisionStopCoverageTracker final {
 public:
  void start(std::uint64_t generation) noexcept;
  void record_tick(std::uint64_t generation) noexcept;
  void record_observed_source(ObservedRobotStateSourceKind source_kind,
                              std::uint64_t source_age_ns) noexcept;
  void record_query(std::uint64_t generation,
                    std::int64_t now_ns) noexcept;
  void record_unavailable(std::uint64_t generation,
                          RuntimeCollisionUnavailableReason reason,
                          std::int64_t now_ns) noexcept;
  bool should_emit_unavailable(std::uint64_t reason_hash,
                               std::int64_t now_ns) noexcept;
  std::optional<RuntimeCollisionStopCoverageSummary> complete(
      std::uint64_t generation, std::int64_t now_ns) noexcept;
  [[nodiscard]] std::uint64_t active_generation() const noexcept;
  [[nodiscard]] RuntimeCollisionStopCoverageMetrics metrics() const noexcept;

 private:
  static void update_max(std::atomic<std::uint64_t>& target,
                         std::uint64_t value) noexcept;

  std::atomic<std::uint64_t> generation_{0};
  std::atomic<std::uint64_t> windows_started_{0};
  std::atomic<std::uint64_t> windows_completed_{0};
  std::atomic<std::uint64_t> total_ticks_{0};
  std::atomic<std::uint64_t> total_queries_{0};
  std::atomic<std::uint64_t> total_unavailable_{0};
  std::atomic<std::uint64_t> longest_unavailable_ns_{0};
  std::atomic<std::uint64_t> zero_check_windows_{0};
  std::atomic<std::uint64_t> current_ticks_{0};
  std::atomic<std::uint64_t> current_queries_{0};
  std::atomic<std::uint64_t> current_unavailable_{0};
  std::array<std::atomic<std::uint64_t>,
             RuntimeCollisionStopCoverageSummary::kUnavailableReasonCount>
      current_unavailable_by_reason_{};
  std::atomic<std::uint64_t> current_max_unavailable_ns_{0};
  std::atomic<std::int64_t> unavailable_since_ns_{-1};
  std::atomic<std::uint64_t> last_generation_{0};
  std::atomic<std::uint64_t> last_ticks_{0};
  std::atomic<std::uint64_t> last_queries_{0};
  std::atomic<std::uint64_t> last_unavailable_{0};
  std::atomic<std::uint64_t> last_max_unavailable_ns_{0};
  std::atomic<std::uint64_t> freshest_source_age_ns_{0};
  std::atomic<int> freshest_source_kind_{-1};
  std::atomic<std::uint64_t> unavailable_reason_hash_{0};
  std::atomic<std::int64_t> unavailable_last_event_ns_{-1};
  std::atomic<std::uint64_t> unavailable_events_suppressed_{0};
};

Result<CollisionWorldPolicySnapshotBundle>
load_collision_world_policy_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);
Result<RuntimeCollisionQueryBundlePtr>
load_published_runtime_collision_query_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);

RuntimeCollisionQueryBundlePtr build_runtime_collision_query_bundle(
    CollisionWorldPolicySnapshotBundle bundle, std::uint64_t epoch);
bool runtime_collision_lookup_order_is_valid(
    const CollisionWorldSnapshot& world,
    const std::vector<CollisionWorldBodyFreshness>& body_freshness) noexcept;

void append_collision_failure_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle);
std::string pre_dispatch_collision_validation_message(
    const std::map<std::string, std::string>& details,
    CollisionDecisionAction action);
void append_collision_compare_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle,
    const TrajectoryDTO& trajectory,
    const ICollisionDiagnosticComparator& comparator);
CollisionCheckResultDTO collision_check_result_from_bundle(
    const CollisionValidationDecisionBundle& bundle);

bool collision_world_revision_is_set(const CollisionWorldRevision& revision);
bool collision_world_revisions_equal(const CollisionWorldRevision& lhs,
                                     const CollisionWorldRevision& rhs);
bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines);
bool runtime_monitor_may_hold_collision_engine(ExecutionState state);

}  // namespace manyforge::core

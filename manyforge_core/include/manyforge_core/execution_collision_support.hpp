#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <manyforge_core/cell_model_state.hpp>
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

bool snapshot_too_old(std::int64_t now_ns, std::int64_t timestamp_ns, std::int64_t max_age_ns,
                      std::map<std::string, std::string>& details, const std::string& timestamp_key,
                      const std::string& age_key, const std::string& budget_key);

struct RuntimeCollisionStateProjection {
  std::vector<std::string> source_joint_names;
  std::vector<std::string> query_joint_names;
  std::vector<double> template_positions_rad;
  std::vector<std::size_t> source_to_query_indices;
  /// The other components of the cell whose observed positions are read at
  /// every check into the query coordinates they publish. Query coordinates
  /// that neither the source nor one of these writes keep the template value.
  std::vector<BoundCellModelStateSource> live_sources;
};

/// `cell_sources`: the cell's observed-state sources, bound here to the query
/// coordinates the source trajectory does not move. Null or empty: none.
Result<RuntimeCollisionStateProjection> build_runtime_collision_state_projection(
    const TrajectoryDTO& source_trajectory, const TrajectoryDTO& collision_trajectory,
    const CellModelStateSources* cell_sources = nullptr);

/// True when a moving replacement may keep the running execution's collision
/// checks: the same source and query coordinates, and the same template value
/// for every query coordinate that stays at its template. A coordinate the
/// source moves, or one `live` reads from another component at every check,
/// is not compared.
[[nodiscard]] bool runtime_collision_projection_continues(
    const RuntimeCollisionStateProjection& live,
    const RuntimeCollisionStateProjection& replacement) noexcept;

struct RuntimeCollisionStopCoverageSummary {
  static constexpr std::size_t kUnavailableReasonCount = 8;
  std::uint64_t generation = 0;
  std::uint64_t ticks_attempted = 0;
  std::uint64_t queries_executed = 0;
  std::uint64_t unavailable_ticks = 0;
  std::uint64_t longest_unavailable_ns = 0;
  std::array<std::uint64_t, kUnavailableReasonCount> unavailable_by_reason{};
};

enum class RuntimeCollisionUnavailableReason : std::size_t {
  kCollisionResources = 0,
  kCollisionSnapshot,
  kObservedStateUnavailable,
  kObservedStateStale,
  kObservedStateIncomplete,
  kObservedStateProjection,
  kCollisionQuery,
  kOther,
};
static_assert(static_cast<std::size_t>(RuntimeCollisionUnavailableReason::kOther) + 1 ==
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
  void record_query(std::uint64_t generation, std::int64_t now_ns) noexcept;
  void record_unavailable(std::uint64_t generation, RuntimeCollisionUnavailableReason reason,
                          std::int64_t now_ns) noexcept;
  bool should_emit_unavailable(std::uint64_t reason_hash, std::int64_t now_ns) noexcept;
  std::optional<RuntimeCollisionStopCoverageSummary> complete(std::uint64_t generation,
                                                              std::int64_t now_ns) noexcept;
  [[nodiscard]] std::uint64_t active_generation() const noexcept;
  [[nodiscard]] RuntimeCollisionStopCoverageMetrics metrics() const noexcept;

 private:
  static void update_max(std::atomic<std::uint64_t>& target, std::uint64_t value) noexcept;

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

Result<CollisionWorldPolicySnapshotBundle> load_collision_world_policy_snapshot_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);
Result<RuntimeCollisionQueryBundlePtr> load_published_runtime_collision_query_bundle(
    const ICollisionWorldSnapshotPublisher& publisher);

RuntimeCollisionQueryBundlePtr build_runtime_collision_query_bundle(
    CollisionWorldPolicySnapshotBundle bundle,
    std::optional<ProjectionRevisionDTO> projection_revision = std::nullopt);
bool runtime_collision_lookup_order_is_valid(const CollisionWorldSnapshot& world) noexcept;

/// `clearance_margins`: the move's clearance margins. With them a clearance
/// refusal names the rule that holds the pair (`collision_rule_id`, `clearance_m`).
void append_collision_failure_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle,
    const CollisionClearanceMargins* clearance_margins = nullptr);
/// Build the contact-violation payload for the selected failing record when
/// its failure is one of a pair under a contact or release rule: reason
/// (`contact_rule_violation`), `collision_rule_id`, `policy_mode` (`contact` or
/// `release`), the pair and the measured values. Returns nullopt for ordinary
/// collisions.
std::optional<std::map<std::string, std::string>> contact_rule_violation_details(
    const CollisionValidationDecisionBundle& bundle, const CollisionPolicySnapshot& policy,
    std::string_view response,
    std::optional<std::size_t> initial_contact_pair_count = std::nullopt);
std::string pre_dispatch_collision_validation_message(
    const std::map<std::string, std::string>& details, CollisionDecisionAction action);
void append_collision_compare_diagnostics(std::map<std::string, std::string>& details,
                                          const CollisionValidationDecisionBundle& bundle,
                                          const TrajectoryDTO& trajectory,
                                          const ICollisionDiagnosticComparator& comparator);
CollisionCheckResultDTO collision_check_result_from_bundle(
    const CollisionValidationDecisionBundle& bundle);

bool collision_world_revision_is_set(const CollisionWorldRevision& revision);
bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines);
bool runtime_monitor_may_hold_collision_engine(ExecutionState state);

/// True when a contact rule of the policy declares a spring
/// (`max_compression_step_m`, OD-356).
[[nodiscard]] bool collision_policy_has_spring_rule(const CollisionPolicySnapshot& policy) noexcept;

/// Validation samples for a move with a spring rule (OD-356): every segment
/// of `trajectory` is split into the same number of linear sub-steps, the
/// fewest that keep the tool's travel between two samples at most
/// `max_travel_m`, so each original waypoint keeps its fraction of the move.
/// `tcp_position` gives the tool position of a joint state. Fails with
/// `validation_collision` (`spring_sample_densification_failed` when a tool
/// position is unavailable, `spring_sample_budget_exceeded` above
/// `max_samples`).
Result<TrajectoryDTO> densify_for_spring_contact(
    const TrajectoryDTO& trajectory,
    const std::function<Result<Vector3DTO>(const RobotStateDTO&)>& tcp_position,
    double max_travel_m, std::size_t max_samples);

}  // namespace manyforge::core

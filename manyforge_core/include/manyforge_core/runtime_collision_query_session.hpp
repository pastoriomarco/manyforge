#pragma once

#include <array>
#include <cstdint>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {

enum class RuntimeCollisionTickStatus {
  kClear,
  kCollision,
  kInconclusive,
};

enum class RuntimeCollisionTickReason {
  kNone,
  kBundleUnavailable,
  kBundleCapacityExceeded,
  kBundleLookupOrderInvalid,
  kPolicyUnavailable,
  kPolicyIndexMismatch,
  kPolicyCapacityExceeded,
  kDistanceRequestModeUnsupported,
  kRequiredCapabilityUnsupported,
  /// A contact rule without tracked state (whole move without a limit, or
  /// with an end window) holds an invalid value.
  kContactRuleInvalid,
  kObservedStateShapeMismatch,
  kObservedStateNotFinite,
  /// Another component of the cell: its observed state is older than its own
  /// stream limit, or it has published none.
  kOtherComponentStateStale,
  kOtherComponentStateAbsent,
  kObservationCapacityExceeded,
  kEngineInconclusive,
  kBlockingCollision,
  kContactReleasePolicyInvalid,
  kContactReleaseMissingClearance,
  kContactReleaseNewContact,
  kContactReleaseInitialPenetrationExceeded,
  kContactReleaseRecontact,
  kContactReleaseRecontactPenetrationExceeded,
  kContactReleasePenetrationIncreased,
  /// A whole-move contact rule's depth limit was exceeded.
  kContactPenetrationExceeded,
  /// A spring's penetration grew by more than its step limit (OD-356).
  kContactCompressionStepExceeded,
};

/// Depth violation of a pair under a contact rule with an end window
/// (the monitor checks its depth only, not its window).
enum class RuntimeContactRuleViolation {
  kNone,
  kMissingClearance,
  kPenetrationExceeded,
};

const char* runtime_collision_tick_reason_name(RuntimeCollisionTickReason reason) noexcept;
const char* runtime_collision_query_failure_reason_name(
    RuntimeCollisionQueryFailureReason reason) noexcept;
const char* runtime_collision_query_prepare_status_name(
    RuntimeCollisionQueryPrepareStatus status) noexcept;
const char* runtime_collision_engine_status_name(CollisionEngineStatus status) noexcept;

struct RuntimeCollisionTickOutcome {
  RuntimeCollisionTickStatus status = RuntimeCollisionTickStatus::kInconclusive;
  RuntimeCollisionTickReason reason = RuntimeCollisionTickReason::kBundleUnavailable;
  RuntimeCollisionQueryFailureReason engine_failure_reason =
      RuntimeCollisionQueryFailureReason::kNone;
  CollisionEngineStatus engine_status = CollisionEngineStatus::kDiagnosticIncomplete;
  std::int64_t query_duration_ns = 0;
  bool query_executed = false;
  const CollisionPairId* pair = nullptr;
  CollisionPairClassification pair_classification =
      CollisionPairClassification::kDiagnosticIncomplete;
  std::optional<double> signed_clearance_m;
  std::optional<double> initial_penetration_m;
  std::optional<double> previous_penetration_m;
  std::optional<double> current_penetration_m;
  /// The contact or release rule of `pair` when the outcome is about one;
  /// owned by the bound policy index, like `pair`.
  const CollisionPairContactRule* contact_rule = nullptr;
  bool contact_present = false;
  std::size_t initial_contact_pair_count = 0;
  std::size_t current_contact_pair_count = 0;
  CollisionDistanceRequestMode distance_request_mode = CollisionDistanceRequestMode::kNone;
  std::size_t evaluated_distance_pair_count = 0;
  RuntimeContactRuleViolation contact_rule_violation = RuntimeContactRuleViolation::kNone;
  /// Set with kOtherComponentState*: the component, owned by the session.
  const ControllableComponentId* other_component = nullptr;
};

struct RuntimeContactReleaseStateEntry {
  const CollisionPairId* pair = nullptr;
  SupportContactEvaluationState support_state = SupportContactEvaluationState::kNotInContact;
  ContactReleaseEvaluationState release_state;
};

struct RuntimeContactReleaseStateSnapshot {
  std::array<RuntimeContactReleaseStateEntry, 64> entries{};
  std::size_t count = 0;
  bool valid = false;
  /// Pins the immutable policy index that owns every `entry.pair` pointer.
  /// The lookahead worker may outlive the immediate-query session generation.
  RuntimeCollisionQueryBundlePtr binding_owner;
};

/// The compression of one spring pair at the last clear check (OD-358): a
/// pair under a contact rule with `max_compression_step_m` that overlaps.
struct RuntimeSpringCompressionEntry {
  /// Owned by the bound policy index, like the pair.
  const CollisionPairContactRule* rule = nullptr;
  double depth_m = 0.0;
  /// Unit vector (world frame) that moves the rule's side a out of side b.
  std::array<double, 3> push_direction_a{};
};

/// Fixed-capacity copy of the spring compressions; `binding_owner` pins the
/// policy index that owns every `entry.rule`.
struct RuntimeSpringCompressionSnapshot {
  std::array<RuntimeSpringCompressionEntry, 64> entries{};
  std::size_t count = 0;
  RuntimeCollisionQueryBundlePtr binding_owner;
};

/// Execution-bound, single-consumer runtime collision query state. Construction
/// and prepare() are NRT monitor-arm operations. One session remains continuous
/// across motion-to-controlled-stop protection-owner generations so contact
/// history is not re-baselined. tick() owns fixed-capacity projection/contact
/// state and delegates to the engine's dedicated runtime surface; a clean warm
/// pass creates no framework-owned DTO/map/string storage. Every contact and
/// release rule of the bound policy is active.
class RuntimeCollisionQuerySession final {
 public:
  /// Bound on the tracked contact-rule pairs of one policy: release rules and
  /// whole-move contact rules with a depth limit.
  static constexpr std::size_t kMaxSupportContacts = 64;

  RuntimeCollisionQuerySession(
      RuntimeCollisionStateProjection projection, ICollisionWorldSnapshotPublisher& publisher,
      std::shared_ptr<ICollisionQueryEngine> engine, std::int64_t query_timeout_ns);

  /// With keep_admitted_policy, the session keeps initial_bundle's policy (the
  /// move's admitted operation-local policy) for its whole life, whatever the
  /// published policy revision; acquire_bundle() pairs it with the latest
  /// published world. Without it, every tick follows the published bundle.
  [[nodiscard]] std::optional<ErrorDTO> prepare(
      const RuntimeCollisionQueryBundlePtr& initial_bundle,
      bool keep_admitted_policy = false);
  /// `now_ns`: the caller's receiver-clock time, for the other components'
  /// observed positions the projection reads at this check.
  [[nodiscard]] RuntimeCollisionTickOutcome tick(const RuntimeCollisionQueryBundlePtr& bundle,
                                                 std::span<const double> observed_positions_rad,
                                                 std::int64_t state_time_ns,
                                                 std::int64_t now_ns) noexcept;
  /// Single consumer (the monitor tick).
  [[nodiscard]] RuntimeCollisionQueryBundlePtr acquire_bundle();
  [[nodiscard]] const CollisionEngineCapabilityDescriptor& engine_capability() const noexcept {
    return *capability_;
  }
  /// Single-consumer snapshot taken immediately after a clear tick. Pair
  /// pointers remain valid because the returned snapshot pins the immutable
  /// runtime bundle that owns the bound policy index.
  [[nodiscard]] RuntimeContactReleaseStateSnapshot contact_release_state_snapshot() const noexcept;
  /// Single-consumer, after a clear tick: every spring pair that overlaps at
  /// that check, with its depth and push-back direction. A pair the engine
  /// gave no normal for is left out.
  [[nodiscard]] RuntimeSpringCompressionSnapshot spring_compression_snapshot() const noexcept;

 private:
  struct ContactReleaseState {
    const CollisionPairId* pair = nullptr;
    const CollisionPairContactRule* policy = nullptr;
    ContactReleaseEvaluationState transition_state;
    bool observed_in_contact = false;
    std::optional<double> observed_clearance_m;
    std::optional<std::array<double, 3>> observed_normal_a_to_b;
  };

  bool bind_policy(const RuntimeCollisionQueryBundlePtr& bundle) noexcept;
  bool bind_distance_selection(const RuntimeCollisionQueryBundlePtr& bundle) noexcept;
  RuntimeCollisionTickOutcome project_observed_state(
      std::span<const double> observed_positions_rad, std::int64_t now_ns) noexcept;
  RuntimeCollisionTickOutcome classify(
      const RuntimeCollisionQueryResultView& query_result) noexcept;
  RuntimeCollisionTickOutcome update_contact_release(
      const RuntimeCollisionQueryResultView& query_result) noexcept;
  std::size_t current_contact_pair_count(
      const RuntimeCollisionQueryResultView& query_result) const noexcept;

  RuntimeCollisionStateProjection projection_;
  ICollisionWorldSnapshotPublisher* publisher_ = nullptr;
  std::shared_ptr<ICollisionQueryEngine> engine_;
  const CollisionEngineCapabilityDescriptor* capability_ = nullptr;
  std::int64_t query_timeout_ns_ = 0;
  std::vector<double> query_positions_rad_;
  // Pin the publisher's complete reclamation unit. Keeping only policy/index
  // sub-handles would hide their last-owner transition from NRT bundle
  // retirement and could move destruction onto RuntimeSafety during rebind.
  RuntimeCollisionQueryBundlePtr bound_bundle_;
  // Admitted operation-local policy and the last bundle built from it with the
  // latest published world. Empty for moves that follow the published policy.
  std::shared_ptr<const CollisionPolicySnapshot> admitted_policy_;
  RuntimeCollisionQueryBundlePtr admitted_policy_bundle_;
  const CollisionPolicySnapshot* bound_policy_ = nullptr;
  const CollisionPolicyIndex* bound_policy_index_ = nullptr;
  std::array<ContactReleaseState, kMaxSupportContacts> release_states_{};
  std::size_t release_state_count_ = 0;
  std::size_t initial_contact_pair_count_ = 0;
  bool initial_contact_pair_count_initialized_ = false;
  bool policy_binding_valid_ = false;
  RuntimeCollisionTickReason policy_binding_error_ = RuntimeCollisionTickReason::kPolicyUnavailable;
  const char* policy_binding_required_capability_ = nullptr;
};

}  // namespace manyforge::core

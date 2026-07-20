#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/execution_collision_support.hpp>
#include <manyforge_core/types/error_code.hpp>

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
  kActiveContactPolicyUnknown,
  kActiveContactPolicyInvalid,
  kObservedStateShapeMismatch,
  kObservedStateNotFinite,
  kObservationCapacityExceeded,
  kEngineInconclusive,
  kBlockingCollision,
  kContactReleasePolicyInvalid,
  kContactReleaseProcessUnsupported,
  kContactReleaseMissingClearance,
  kContactReleaseNewContact,
  kContactReleaseInitialPenetrationExceeded,
  kContactReleaseRecontact,
  kContactReleaseRecontactPenetrationExceeded,
  kContactReleasePenetrationIncreased,
  kBoundedContactPenetrationExceeded,
};

enum class RuntimeContactGraspViolation {
  kNone,
  kMissingClearance,
  kPenetrationExceeded,
};

const char* runtime_collision_tick_reason_name(
    RuntimeCollisionTickReason reason) noexcept;
const char* runtime_collision_query_failure_reason_name(
    RuntimeCollisionQueryFailureReason reason) noexcept;
const char* runtime_collision_query_prepare_status_name(
    RuntimeCollisionQueryPrepareStatus status) noexcept;
const char* runtime_collision_engine_status_name(
    CollisionEngineStatus status) noexcept;

struct RuntimeCollisionTickOutcome {
  RuntimeCollisionTickStatus status = RuntimeCollisionTickStatus::kInconclusive;
  RuntimeCollisionTickReason reason =
      RuntimeCollisionTickReason::kBundleUnavailable;
  RuntimeCollisionQueryFailureReason engine_failure_reason =
      RuntimeCollisionQueryFailureReason::kNone;
  CollisionEngineStatus engine_status =
      CollisionEngineStatus::kDiagnosticIncomplete;
  std::int64_t query_duration_ns = 0;
  bool query_executed = false;
  const CollisionPairId* pair = nullptr;
  CollisionPairClassification pair_classification =
      CollisionPairClassification::kDiagnosticIncomplete;
  std::optional<double> signed_clearance_m;
  std::optional<double> initial_penetration_m;
  std::optional<double> previous_penetration_m;
  std::optional<double> current_penetration_m;
  std::optional<double> max_contact_penetration_m;
  std::optional<CollisionContactPolicyMode> contact_policy_mode;
  std::optional<ContactReleaseEvidenceMode> release_evidence_mode;
  bool contact_present = false;
  std::size_t initial_contact_pair_count = 0;
  std::size_t current_contact_pair_count = 0;
  CollisionDistanceRequestMode distance_request_mode =
      CollisionDistanceRequestMode::kNone;
  std::size_t evaluated_distance_pair_count = 0;
  RuntimeContactGraspViolation contact_grasp_violation =
      RuntimeContactGraspViolation::kNone;
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

/// Execution-bound, single-consumer runtime collision query state. Construction
/// and prepare() are NRT monitor-arm operations. One session remains continuous
/// across motion-to-controlled-stop protection-owner generations so contact
/// history is not re-baselined. tick() owns fixed-capacity projection/contact
/// state and delegates to the engine's dedicated runtime surface; a clean warm
/// pass creates no framework-owned DTO/map/string storage.
class RuntimeCollisionQuerySession final {
 public:
  static constexpr std::size_t kMaxSupportContacts = 64;

  RuntimeCollisionQuerySession(
      RuntimeCollisionStateProjection projection,
      ICollisionWorldSnapshotPublisher& publisher,
      std::shared_ptr<ICollisionQueryEngine> engine,
      std::string active_contact_policy_id,
      std::int64_t query_timeout_ns);

  [[nodiscard]] std::optional<ErrorDTO> prepare(
      const RuntimeCollisionQueryBundlePtr& initial_bundle);
  [[nodiscard]] RuntimeCollisionTickOutcome tick(
      const RuntimeCollisionQueryBundlePtr& bundle,
      std::span<const double> observed_positions_rad,
      std::int64_t state_time_ns) noexcept;
  [[nodiscard]] RuntimeCollisionQueryBundlePtr acquire_bundle() const;
  [[nodiscard]] const CollisionEngineCapabilityDescriptor& engine_capability()
      const noexcept {
    return *capability_;
  }
  [[nodiscard]] const std::string& active_contact_policy_id() const noexcept {
    return active_contact_policy_id_;
  }
  /// Single-consumer snapshot taken immediately after a clear tick. Pair
  /// pointers remain valid because the returned snapshot pins the immutable
  /// runtime bundle that owns the bound policy index.
  [[nodiscard]] RuntimeContactReleaseStateSnapshot contact_release_state_snapshot() const noexcept;

 private:
  struct ContactReleaseState {
    const CollisionPairId* pair = nullptr;
    const SupportContactPolicyEntry* policy = nullptr;
    ContactReleaseEvaluationState transition_state;
    bool observed_in_contact = false;
    std::optional<double> observed_clearance_m;
  };

  bool bind_policy(const RuntimeCollisionQueryBundlePtr& bundle) noexcept;
  bool bind_distance_selection(
      const RuntimeCollisionQueryBundlePtr& bundle) noexcept;
  RuntimeCollisionTickOutcome project_observed_state(
      std::span<const double> observed_positions_rad) noexcept;
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
  std::string active_contact_policy_id_;
  std::int64_t query_timeout_ns_ = 0;
  std::vector<double> query_positions_rad_;
  // Pin the publisher's complete reclamation unit. Keeping only policy/index
  // sub-handles would hide their last-owner transition from NRT bundle
  // retirement and could move destruction onto RuntimeSafety during rebind.
  RuntimeCollisionQueryBundlePtr bound_bundle_;
  const CollisionPolicySnapshot* bound_policy_ = nullptr;
  const CollisionPolicyIndex* bound_policy_index_ = nullptr;
  const ContactGraspPolicyEntry* active_contact_policy_ = nullptr;
  std::array<ContactReleaseState, kMaxSupportContacts> release_states_{};
  std::size_t release_state_count_ = 0;
  std::size_t initial_contact_pair_count_ = 0;
  bool initial_contact_pair_count_initialized_ = false;
  bool policy_binding_valid_ = false;
  RuntimeCollisionTickReason policy_binding_error_ =
      RuntimeCollisionTickReason::kPolicyUnavailable;
  const char* policy_binding_required_capability_ = nullptr;
};

}  // namespace manyforge::core

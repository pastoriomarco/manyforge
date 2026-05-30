#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/interrupt.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>

namespace manyforge::core {

struct InterruptManagerConfig {
  std::size_t queue_depth = 16;
  /// Default for `arm_timeout_ns` when a
  /// `MotionInterruptScopeDTO` omits it. Spec/260 §9 deployment
  /// example proposes 50 ms.
  std::int64_t motion_scope_arm_timeout_ns = 50'000'000;
};

/// Diagnostics-only observer for armed move-scoped bindings
/// (spec/260 §B.3). BT logic MUST NOT poll this for control flow.
struct MotionInterruptScopeStatusDTO {
  std::string scope_id;
  std::string policy_id;
  std::string source_id;
  std::uint64_t execution_id = 0;
  std::string move_id;
  std::int64_t armed_at_ns = 0;
  MotionInterruptActivationMode activation_mode =
      MotionInterruptActivationMode::kRequireInactiveAtArm;
  std::optional<std::int64_t> baseline_sequence_id;
  bool baseline_active = false;
  std::optional<std::int64_t> last_signal_sequence_id;
};

struct InterruptRuntimeContext {
  InterruptExecutionPhase phase = InterruptExecutionPhase::kPlanning;
  std::optional<std::uint64_t> execution_id;
  std::optional<std::string> move_id;
  std::optional<CollisionWorldRevision> borrowed_collision_world_revision;
};

struct InterruptManagerEvent {
  std::string type;
  InterruptStructuredValue details;
  std::optional<InterruptLatch> latch;
};

struct InterruptIngestResult {
  bool accepted = false;
  bool latched = false;
  bool queued = false;
  bool suppressed = false;
  bool rejected = false;
  std::string reason;
  std::optional<InterruptLatch> active_latch;
  std::vector<InterruptManagerEvent> events;
};

class InterruptManager {
 public:
  explicit InterruptManager(InterruptManagerConfig config = {});

  Result<void> register_source(const InterruptSourceDefinition& source);
  Result<void> register_policy(const InterruptPolicyDefinition& policy);

  InterruptIngestResult ingest(
      const InterruptSignalUpdate& update,
      InterruptSourceClass attempted_lane,
      const InterruptRuntimeContext& context,
      std::int64_t kernel_now_ns);

  InterruptIngestResult ingest_batch(
      const std::vector<InterruptSignalUpdate>& updates,
      InterruptSourceClass attempted_lane,
      const InterruptRuntimeContext& context,
      std::int64_t kernel_now_ns);

  Result<InterruptIngestResult> apply_disposition(
      const InterruptDisposition& disposition,
      std::int64_t kernel_now_ns);

  Result<InterruptIngestResult> clear_active_latch(
      std::string clear_reason,
      std::int64_t kernel_now_ns);

  Result<InterruptIngestResult> mark_active_preemption_result(
      InterruptPreemptionResult result,
      std::int64_t preemption_latency_ns,
      std::int64_t kernel_now_ns);

  /// Atomically arm every `MotionInterruptScopeDTO` in `scopes` for the
  /// given `execution_id` / `move_id`. Fails closed: if any scope is
  /// rejected (unknown policy, wrong scope_kind, activation-mode
  /// baseline check fails, same-source safety-priority constraint
  /// violation, duplicate scope_id, etc.) NO bindings are armed and
  /// the failure reason names the first rejecting scope. Emits
  /// `motion_interrupt_scope_armed` events on success.
  Result<InterruptIngestResult> arm_motion_interrupt_scopes(
      std::uint64_t execution_id,
      const std::string& move_id,
      const std::vector<MotionInterruptScopeDTO>& scopes,
      std::int64_t kernel_now_ns);

  /// Reap every armed binding tied to `execution_id`. Emits one
  /// `motion_interrupt_scope_reaped` event per binding with the
  /// supplied `reap_reason` (e.g. `"execution_terminal"`, `"reset"`,
  /// `"backend_preempt_failed"`).
  InterruptIngestResult reap_motion_interrupt_scopes(
      std::uint64_t execution_id,
      const std::string& reap_reason,
      std::int64_t kernel_now_ns);

  /// Diagnostics-only observer surface (spec/260 §B.3).
  std::vector<MotionInterruptScopeStatusDTO> active_motion_interrupt_scopes()
      const;

  std::optional<InterruptLatch> active_latch() const;
  std::size_t queued_count() const;

  /// Return a copy of the policy whose latch is currently active.
  /// The kernel uses this to read the active fault policy's
  /// optional `stop_profile` field on the preempt-trajectory path
  /// without exposing the manager's internal `policies_by_id_` map.
  /// Returns nullopt when no latch is held or the latch's policy id
  /// is no longer registered (the latter is a defensive case — the
  /// manager never deletes registered policies during a session).
  std::optional<InterruptPolicyDefinition> active_policy() const;

 private:
  struct ResolvedPolicy {
    InterruptSourceDefinition source;
    InterruptPolicyDefinition policy;
    int effective_priority = 0;
    bool effective_maskable = false;
  };

  struct QueuedUpdate {
    InterruptSignalUpdate update;
    ResolvedPolicy resolved;
    InterruptRuntimeContext context;
    std::int64_t kernel_now_ns = 0;
    std::uint64_t order = 0;
    std::optional<InterruptTieBreak> tie_break;
  };

  Result<ResolvedPolicy> resolve_policy_locked(
      const InterruptSignalUpdate& update,
      const InterruptRuntimeContext& context) const;
  bool signal_requests_latch(const InterruptSourceDefinition& source,
                             const InterruptSignalUpdate& update) const;
  bool signal_requests_release(const InterruptSourceDefinition& source,
                               const InterruptSignalUpdate& update) const;
  std::optional<std::int64_t> freshness_ns(
      const InterruptSignalUpdate& update,
      std::int64_t kernel_now_ns) const;
  Result<void> validate_update_locked(
      const InterruptSignalUpdate& update,
      InterruptSourceClass attempted_lane,
      const ResolvedPolicy& resolved,
      std::int64_t kernel_now_ns) const;

  InterruptIngestResult ingest_resolved_locked(
      const InterruptSignalUpdate& update,
      const ResolvedPolicy& resolved,
      const InterruptRuntimeContext& context,
      std::int64_t kernel_now_ns,
      std::optional<InterruptTieBreak> tie_break);
  Result<InterruptIngestResult> clear_active_latch_locked(
      std::string clear_reason,
      std::int64_t kernel_now_ns);

  InterruptLatch make_latch_locked(
      const InterruptSignalUpdate& update,
      const ResolvedPolicy& resolved,
      const InterruptRuntimeContext& context,
      std::int64_t kernel_now_ns,
      std::optional<InterruptTieBreak> tie_break);

  void queue_update_locked(
      const InterruptSignalUpdate& update,
      const ResolvedPolicy& resolved,
      const InterruptRuntimeContext& context,
      std::int64_t kernel_now_ns,
      std::vector<InterruptManagerEvent>& events,
      InterruptIngestResult& result);
  std::optional<QueuedUpdate> pop_next_queued_locked();
  void append_latched_events_locked(InterruptIngestResult& result,
                                    const InterruptLatch& latch);
  bool disposition_allowed(const InterruptLatch& latch,
                           const InterruptDisposition& disposition) const;
  std::optional<InterruptPolicyDefinition> active_policy_locked() const;

  struct ActiveScopeBinding {
    std::string scope_id;
    std::string policy_id;
    std::string source_id;
    std::uint64_t execution_id = 0;
    std::string move_id;
    std::int64_t armed_at_ns = 0;
    MotionInterruptActivationMode activation_mode =
        MotionInterruptActivationMode::kRequireInactiveAtArm;
    std::optional<std::int64_t> baseline_sequence_id;
    bool baseline_active = false;
    /// Age of the baseline sample at arm time, computed as
    /// `armed_at_ns - sample.timestamp_ns`. Emitted on
    /// `motion_interrupt_scope_armed` (spec/260 §7) so observers can
    /// confirm the baseline was within `arm_timeout_ns` without
    /// reasoning about sequence IDs. Absent when arming under
    /// `kTripIfAlreadyActive` without any prior sample.
    std::optional<std::int64_t> arm_baseline_age_ns;
    std::optional<std::string> result_binding_key;
    std::optional<std::int64_t> last_signal_sequence_id;
  };

  struct LastSourceSample {
    std::int64_t sequence_id = 0;
    std::int64_t timestamp_ns = 0;
    bool active = false;
  };

  Result<void> validate_move_scoped_policy_locked(
      const InterruptPolicyDefinition& policy) const;
  Result<ActiveScopeBinding> build_scope_binding_locked(
      const MotionInterruptScopeDTO& scope,
      std::uint64_t execution_id,
      const std::string& move_id,
      std::int64_t kernel_now_ns) const;
  void update_source_sample_locked(const InterruptSignalUpdate& update);
  bool scope_binding_allows_latch_locked(
      const ActiveScopeBinding& binding,
      const InterruptSignalUpdate& update) const;

  mutable std::mutex mutex_;
  InterruptManagerConfig config_;
  std::map<std::string, InterruptSourceDefinition> sources_;
  std::map<std::string, InterruptPolicyDefinition> policies_by_id_;
  std::map<std::string, std::vector<std::string>> policy_ids_by_source_;
  std::map<std::string, std::int64_t> last_latch_time_by_source_;
  std::map<std::string, LastSourceSample> last_sample_by_source_;
  std::map<std::string, ActiveScopeBinding> active_scope_bindings_by_scope_id_;
  std::map<std::string, std::string> active_scope_id_by_policy_id_;
  std::optional<InterruptLatch> active_latch_;
  std::vector<QueuedUpdate> queue_;
  std::uint64_t next_latch_id_ = 1;
  std::uint64_t next_queue_order_ = 1;
};

}  // namespace manyforge::core

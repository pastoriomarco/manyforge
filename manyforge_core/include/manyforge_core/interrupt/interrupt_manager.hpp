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

namespace manyforge::core {

struct InterruptManagerConfig {
  std::size_t queue_depth = 16;
};

struct InterruptRuntimeContext {
  InterruptExecutionPhase phase = InterruptExecutionPhase::kPlanning;
  std::optional<std::uint64_t> execution_id;
  std::optional<std::string> move_id;
  std::optional<std::uint64_t> streaming_session_id;
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

  std::optional<InterruptLatch> active_latch() const;
  std::size_t queued_count() const;

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

  mutable std::mutex mutex_;
  InterruptManagerConfig config_;
  std::map<std::string, InterruptSourceDefinition> sources_;
  std::map<std::string, InterruptPolicyDefinition> policies_by_id_;
  std::map<std::string, std::vector<std::string>> policy_ids_by_source_;
  std::map<std::string, std::int64_t> last_latch_time_by_source_;
  std::optional<InterruptLatch> active_latch_;
  std::vector<QueuedUpdate> queue_;
  std::uint64_t next_latch_id_ = 1;
  std::uint64_t next_queue_order_ = 1;
};

}  // namespace manyforge::core

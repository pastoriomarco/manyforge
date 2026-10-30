#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

enum class RuntimeCollisionQueryPrepareStatus {
  kReady,
  kUnsupported,
  kJointCountExceeded,
  kInvalidJointOrder,
  kEngineBusy,
  kEngineError,
};

enum class RuntimeCollisionQueryCapabilityStatus {
  kAdmitted,
  kInvalidDistanceRequestModes,
  kUnsupportedMemoryModel,
  kMissingDimensionBound,
  kMissingWarmWcet,
  kPreallocatedDeclaresWarmAllocations,
  kMeasuredBoundedMissingWarmAllocationBound,
  kSnapshotChangeDeclaresAllocationsWhenDisabled,
  kSnapshotChangeMissingAllocationBound,
  kSnapshotChangeMissingWcet,
};

[[nodiscard]] inline RuntimeCollisionQueryCapabilityStatus
runtime_collision_query_capability_status(
    const CollisionEngineCapabilityDescriptor& capability) noexcept {
  if (!collision_distance_request_modes_valid(capability)) {
    return RuntimeCollisionQueryCapabilityStatus::
        kInvalidDistanceRequestModes;
  }
  if (capability.runtime_query_memory_model ==
      RuntimeCollisionQueryMemoryModel::kUnsupported) {
    return RuntimeCollisionQueryCapabilityStatus::kUnsupportedMemoryModel;
  }
  if (capability.runtime_query_max_joints == 0 ||
      capability.runtime_query_max_world_bodies == 0 ||
      capability.runtime_query_max_collision_pairs == 0) {
    return RuntimeCollisionQueryCapabilityStatus::kMissingDimensionBound;
  }
  if (capability.runtime_query_qualified_wcet_ns == 0) {
    return RuntimeCollisionQueryCapabilityStatus::kMissingWarmWcet;
  }
  if (capability.runtime_query_memory_model ==
          RuntimeCollisionQueryMemoryModel::kPreallocated &&
      capability.runtime_query_clean_allocation_upper_bound != 0) {
    return RuntimeCollisionQueryCapabilityStatus::
        kPreallocatedDeclaresWarmAllocations;
  }
  if (capability.runtime_query_memory_model ==
          RuntimeCollisionQueryMemoryModel::kMeasuredBounded &&
      capability.runtime_query_clean_allocation_upper_bound == 0) {
    return RuntimeCollisionQueryCapabilityStatus::
        kMeasuredBoundedMissingWarmAllocationBound;
  }
  if (!capability.runtime_query_snapshot_change_may_allocate &&
      capability.runtime_query_snapshot_change_allocation_upper_bound != 0) {
    return RuntimeCollisionQueryCapabilityStatus::
        kSnapshotChangeDeclaresAllocationsWhenDisabled;
  }
  if (capability.runtime_query_snapshot_change_may_allocate &&
      capability.runtime_query_snapshot_change_allocation_upper_bound == 0) {
    return RuntimeCollisionQueryCapabilityStatus::
        kSnapshotChangeMissingAllocationBound;
  }
  if (capability.runtime_query_snapshot_change_qualified_wcet_ns == 0) {
    return RuntimeCollisionQueryCapabilityStatus::kSnapshotChangeMissingWcet;
  }
  return RuntimeCollisionQueryCapabilityStatus::kAdmitted;
}

[[nodiscard]] constexpr const char*
runtime_collision_query_capability_status_name(
    RuntimeCollisionQueryCapabilityStatus status) noexcept {
  switch (status) {
    case RuntimeCollisionQueryCapabilityStatus::kAdmitted:
      return "admitted";
    case RuntimeCollisionQueryCapabilityStatus::kInvalidDistanceRequestModes:
      return "invalid_distance_request_modes";
    case RuntimeCollisionQueryCapabilityStatus::kUnsupportedMemoryModel:
      return "unsupported_memory_model";
    case RuntimeCollisionQueryCapabilityStatus::kMissingDimensionBound:
      return "missing_dimension_bound";
    case RuntimeCollisionQueryCapabilityStatus::kMissingWarmWcet:
      return "missing_warm_wcet";
    case RuntimeCollisionQueryCapabilityStatus::
        kPreallocatedDeclaresWarmAllocations:
      return "preallocated_declares_warm_allocations";
    case RuntimeCollisionQueryCapabilityStatus::
        kMeasuredBoundedMissingWarmAllocationBound:
      return "measured_bounded_missing_warm_allocation_bound";
    case RuntimeCollisionQueryCapabilityStatus::
        kSnapshotChangeDeclaresAllocationsWhenDisabled:
      return "snapshot_change_declares_allocations_when_disabled";
    case RuntimeCollisionQueryCapabilityStatus::
        kSnapshotChangeMissingAllocationBound:
      return "snapshot_change_missing_allocation_bound";
    case RuntimeCollisionQueryCapabilityStatus::kSnapshotChangeMissingWcet:
      return "snapshot_change_missing_wcet";
  }
  return "unknown";
}

enum class RuntimeCollisionQueryStatus {
  kClear,
  kCollision,
  kInconclusive,
};

enum class RuntimeCollisionQueryFailureReason {
  kNone,
  kUnsupported,
  kEngineBusy,
  kSnapshotBodyLimitExceeded,
  kSnapshotInvalid,
  kSnapshotSyncFailed,
  kObservedStateShapeMismatch,
  kObservedStateNotFinite,
  kPairDiagnosticsIncomplete,
  kObservationCapacityExceeded,
  kTimeout,
  kEngineException,
};

/// Non-owning pair evidence valid until the next runtime query on the same
/// engine instance. The pair points into revision-stable engine storage built
/// during snapshot ingestion, so a warm clean query does not copy body ids.
struct RuntimeCollisionPairObservation {
  const CollisionPairId* pair = nullptr;
  std::optional<double> signed_clearance_m;
};

struct RuntimeCollisionQueryRequest {
  std::span<const double> positions_rad;
  std::int64_t state_time_ns = 0;
  std::int64_t timeout_budget_ns = 0;
  CollisionDistanceRequestMode distance_request_mode =
      CollisionDistanceRequestMode::kNone;
  /// Sorted, unique canonical logical pairs prepared before the runtime hot
  /// loop. Used only with kSelectedCollidingPairs; the span's storage must
  /// remain valid for the duration of check_runtime().
  std::span<const CollisionPairId> selected_distance_pairs;
};

/// Typed, non-owning runtime result. Maps, formatted strings, and owning result
/// DTOs are deliberately absent; callers materialize diagnostics only after a
/// collision or inconclusive result is known.
struct RuntimeCollisionQueryResultView {
  RuntimeCollisionQueryStatus status =
      RuntimeCollisionQueryStatus::kInconclusive;
  RuntimeCollisionQueryFailureReason failure_reason =
      RuntimeCollisionQueryFailureReason::kUnsupported;
  CollisionEngineStatus engine_status =
      CollisionEngineStatus::kDiagnosticIncomplete;
  std::int64_t query_duration_ns = 0;
  std::span<const RuntimeCollisionPairObservation> collision_pairs;
  std::span<const RuntimeCollisionPairObservation> clearance_pairs;
};

/// Cancellation token observed by collision engines and by the
/// CollisionValidationCoordinator / ParallelCollisionValidationWorkerPool.
///
/// The kernel-native interrupt model
/// ([`docs/spec/260-kernel-native-interrupts.md`](docs/spec/260-kernel-native-interrupts.md)
/// §8) requires that mid-pass cancellation propagate to every worker so
/// the coordinator can produce a deterministic post-cancel
/// `CollisionValidationDecisionBundle`. Engines MUST observe this token at
/// waypoint boundaries and at every blocking I/O wait; an engine that
/// observes the token MUST return a `CollisionResult` with
/// `engine_status = kCancelled`, `decision =
/// CollisionDecisionResult::kInconclusiveFail`, and a structured
/// cancellation reason in the diagnostic payload — it MUST NOT mutate
/// scene state and MUST NOT push a mismatch-ledger record for the
/// cancelled query.
///
/// Implementations are free to share a single token instance across
/// workers in a pool; hot-path cancellation observation is lock-free.
/// The human-readable reason is protected by a mutex because it is not
/// part of the per-waypoint hot path.
class CollisionCancellationToken {
 public:
  CollisionCancellationToken() = default;

  CollisionCancellationToken(const CollisionCancellationToken&) = delete;
  CollisionCancellationToken& operator=(const CollisionCancellationToken&) =
      delete;

  /// Request cancellation. Idempotent; safe to call from any thread.
  void request_cancel(std::string reason = "") {
    if (cancelled_.load(std::memory_order_acquire)) {
      return;
    }
    std::scoped_lock lock(reason_mutex_);
    if (!cancelled_.load(std::memory_order_relaxed)) {
      reason_ = std::move(reason);
      cancelled_.store(true, std::memory_order_release);
    }
  }

  /// Observe cancellation. Engines MUST call this at waypoint boundaries
  /// and at every blocking-wait return.
  [[nodiscard]] bool is_cancel_requested() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

  /// Stable reason carried alongside the cancellation request. Set once
  /// at the first `request_cancel()` call; subsequent calls do not
  /// overwrite it. Bindings should treat the empty string as
  /// "no reason supplied".
  [[nodiscard]] std::string reason() const {
    std::scoped_lock lock(reason_mutex_);
    return reason_;
  }

 private:
  std::atomic<bool> cancelled_{false};
  mutable std::mutex reason_mutex_;
  std::string reason_;
};

/// Backend-agnostic collision-query engine port (kernel-architecture
/// sprint P-2-design, consuming the P-2-impl-snapshot DTO).
///
/// The `CollisionValidationCoordinator` and the
/// `ParallelCollisionValidationWorkerPool` consume implementations of
/// this interface as their production worker primitive. A planner-native
/// collision path may exist only behind the dedicated diagnostic-comparator
/// port; validation and runtime-monitor paths MUST use this interface.
///
/// Implementations are expected to declare an immutable model + immutable
/// `CollisionWorldSnapshot` shared across N worker instances, and to
/// own per-instance mutable state (e.g. per-worker Pinocchio `Data`,
/// Coal collision data, broadphase caches, contact buffers). The
/// thread-safety contract is published per-engine through the
/// `CollisionEngineCapabilityDescriptor`'s
/// `CollisionEngineThreadSafetyClass`. The parallel worker pool reads
/// that class plus `mutable_context_id` and may either:
///   - share one engine instance across workers when
///     `kImmutableModelWorkerData` allows it, or
///   - require one engine instance per worker when
///     `kSingleContextExclusive` is declared. Distinct wrapper instances with
///     the same non-empty mutable context id are still one exclusive resource.
///
/// Engine implementations MUST NOT depend on planner-native scene authority;
/// they receive a `CollisionWorldSnapshot` for scene state, a `CollisionQuery`
/// for the queried robot state + surface, and a
/// `CollisionPolicySnapshot` for classification policy. This decouples
/// the engine from planner internals (RoboPlan, MoveIt2, Isaac); each
/// planner becomes a snapshot publisher rather than a validator.
///
/// The concrete `CollisionWorldSnapshot` DTO lives in
/// `<manyforge_core/types/collision_world_snapshot.hpp>`; publisher
/// implementations remain behind
/// `ICollisionWorldSnapshotPublisher`. This port shape is defined in
/// spec/210 §3.8.2.
class ICollisionQueryEngine {
 public:
  virtual ~ICollisionQueryEngine() = default;

  /// Capability declaration. Read at construction by callers (pool,
  /// coordinator, compare-mode emitter) to decide thread-safety class,
  /// supported geometry kinds, and known limitations.
  ///
  /// The descriptor is immutable for the lifetime of the engine
  /// instance. Implementations MUST return the same value on every
  /// call.
  [[nodiscard]] virtual const CollisionEngineCapabilityDescriptor&
  capability() const = 0;

  /// Run one collision query.
  ///
  /// @param snapshot Active immutable collision-world snapshot. Engines
  ///   MUST NOT mutate the snapshot and MUST NOT cache it past the call
  ///   unless the snapshot's revision identity matches the cached one.
  /// @param query Robot state, surface, sampling mode, timeout budget,
  ///   required capabilities, and waypoint metadata. Engines that
  ///   cannot supply a required capability MUST fail closed with
  ///   `engine_status = kDiagnosticIncomplete` and `decision =
  ///   kInconclusiveFail`.
  /// @param policy Active policy snapshot. Engines MAY use it to
  ///   short-circuit known-allowed pair classifications (static-static,
  ///   touch-link) but MUST NOT modify the snapshot. The coordinator
  ///   remains the policy-evaluation authority; the engine returns raw
  ///   `normalized_pairs` and the coordinator classifies.
  /// @param cancel Cancellation token. Engines MUST observe at every
  ///   waypoint boundary and every blocking-wait return; on observed
  ///   cancellation MUST return a cancelled result without mutating
  ///   state.
  /// @return Fully populated `CollisionResult` carrying
  ///   `engine_id`, `engine_version`, `engine_status`, `decision`,
  ///   `normalized_pairs`, optional diagnostics, and
  ///   `query_duration_ns`. Engines MUST set
  ///   `collision_world_revision` from the snapshot.
  ///
  /// Threading: callers honor the engine's
  /// `CollisionEngineThreadSafetyClass`. For
  /// `kImmutableModelWorkerData`, multiple threads MAY call `check()`
  /// concurrently against the same instance only if each thread owns a
  /// distinct mutable-data scope; for `kSingleContextExclusive`,
  /// concurrent calls are undefined behavior and the pool MUST hold one
  /// instance per worker.
  [[nodiscard]] virtual CollisionResult check(
      const CollisionWorldSnapshot& snapshot,
      const CollisionQuery& query,
      const CollisionPolicySnapshot& policy,
      const CollisionCancellationToken& cancel) = 0;

  /// Prepare fixed-capacity single-state runtime-query storage. This is an NRT
  /// monitor-arm operation and may be called again when the canonical joint
  /// order changes. Runtime-capable engines must not defer joint-map creation to
  /// check_runtime().
  [[nodiscard]] virtual RuntimeCollisionQueryPrepareStatus prepare_runtime_query(
      std::span<const std::string> /*canonical_joint_names*/) noexcept {
    return RuntimeCollisionQueryPrepareStatus::kUnsupported;
  }

  /// Ingest one immutable world revision before its first clean warm query. This
  /// is where bounded geometry rebuilds and resource preparation belong. A
  /// measured-bounded engine may also accept a changed revision on its qualified
  /// cold path, but no runtime query may perform resource I/O.
  [[nodiscard]] virtual RuntimeCollisionQueryPrepareStatus
  prepare_runtime_snapshot(
      const CollisionWorldSnapshot& /*snapshot*/) noexcept {
    return RuntimeCollisionQueryPrepareStatus::kUnsupported;
  }

  /// Run one observed-state collision query through the dedicated runtime
  /// surface. Implementations must obey capability().runtime_query_memory_model,
  /// return promptly on internal resource contention, and never perform I/O.
  [[nodiscard]] virtual RuntimeCollisionQueryResultView check_runtime(
      const CollisionWorldSnapshot& /*snapshot*/,
      const RuntimeCollisionQueryRequest& /*request*/) noexcept {
    return {};
  }

  /// Stable engine id for telemetry / mismatch ledger correlation.
  /// Engines MAY set this once and reuse it across all results; the
  /// capability descriptor's `engine_id` is the authoritative source
  /// for cross-engine comparison.
  [[nodiscard]] virtual std::string engine_id() const = 0;

  /// Stable engine version for capability evolution tracking.
  [[nodiscard]] virtual std::string engine_version() const = 0;
};

/// Thin RAII pointer alias for engines that are owned by a worker pool.
using CollisionQueryEnginePtr = std::shared_ptr<ICollisionQueryEngine>;

}  // namespace manyforge::core

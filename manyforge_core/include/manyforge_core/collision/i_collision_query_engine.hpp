#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/collision_world_snapshot.hpp>

namespace manyforge::core {

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
/// this interface as their production worker primitive. The previous
/// `IPlanningContext::collision_check_normalized` worker boundary is
/// retained only for the transitional shim during Phase 2; new
/// validation paths MUST use this port.
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
/// Engine implementations MUST NOT depend on `IPlanningContext`; they
/// receive a `CollisionWorldSnapshot` for scene state, a `CollisionQuery`
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

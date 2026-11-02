#pragma once

#include <chrono>
#include <mutex>

namespace manyforge::scene::sqlite_lifecycle_internal {

// Canonical-empty scene-store construction and WAL-topology-changing
// Close/destruction share one process-local lifecycle gate. Cross-process
// teardown/startup is not qualified by this process-local primitive.
std::mutex& CanonicalEmptySceneStoreLifecycleMutex() noexcept;

using CanonicalEmptySceneStoreLifecycleLock =
    std::unique_lock<std::mutex>;

CanonicalEmptySceneStoreLifecycleLock
TryAcquireCanonicalEmptySceneStoreLifecycleLock(
    std::chrono::milliseconds timeout);
CanonicalEmptySceneStoreLifecycleLock
AcquireCanonicalEmptySceneStoreLifecycleLockForClose();

enum class CanonicalEmptySceneStoreLifecycleEvent {
  kStartupAttempt,
  kStartupAcquired,
  kCloseAttempt,
  kCloseAcquired,
};

using CanonicalEmptySceneStoreLifecycleEventObserver = void (*)(
    CanonicalEmptySceneStoreLifecycleEvent event,
    void* context) noexcept;

// Internal deterministic-test observation only. Set/Clear require a quiescent
// test boundary and must not race an acquisition. Attempt callbacks run before
// locking; acquired callbacks run synchronously while the gate is held. A
// callback must not call a lifecycle-gate function.
void SetCanonicalEmptySceneStoreLifecycleEventObserverForTesting(
    CanonicalEmptySceneStoreLifecycleEventObserver observer,
    void* context) noexcept;
void ClearCanonicalEmptySceneStoreLifecycleEventObserverForTesting()
    noexcept;

}  // namespace manyforge::scene::sqlite_lifecycle_internal

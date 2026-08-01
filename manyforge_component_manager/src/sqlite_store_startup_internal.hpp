#pragma once

#include <chrono>
#include <mutex>

namespace manyforge::component_manager::sqlite_startup_internal {

// Direct store constructors, WAL-topology-changing Close/destruction, and the
// composition root share one process-local lifecycle gate. A peer therefore
// cannot preflight either the transient rollback journal created before a
// fresh database reaches WAL mode or a WAL that last-close is removing.
// Recursion lets the composition root retain the gate across classification,
// ordered nested construction, and partial-construction cleanup. Cooperating
// cross-process root startup/classification additionally uses the
// stable-state-directory lock; overlapping cross-process teardown/startup is
// outside this process-local gate.
std::recursive_mutex& ManagerSqliteStoreStartupMutex() noexcept;

using ManagerSqliteStoreStartupLock =
    std::unique_lock<std::recursive_mutex>;

// Poll an ordinary recursive mutex against a steady-clock deadline. GCC 12's
// libstdc++ implements recursive_timed_mutex::try_lock_for with
// pthread_mutex_clocklock, which GCC 12 ThreadSanitizer does not intercept.
// Using the intercepted try_lock path keeps the real synchronization visible
// to TSan without weakening the bounded, recursive production gate.
ManagerSqliteStoreStartupLock
TryAcquireManagerSqliteStoreStartupLock(
    std::chrono::milliseconds timeout);

// Close and destruction must not abandon a live SQLite handle merely because
// a bounded acquisition expires. They therefore wait for the lifecycle gate;
// constructor acquisition remains bounded through the helper above.
ManagerSqliteStoreStartupLock
AcquireManagerSqliteStoreStartupLockForClose();

enum class ManagerSqliteStoreLifecycleEvent {
  kStartupAttempt,
  kStartupAcquired,
  kCloseAttempt,
  kCloseAcquired,
};

using ManagerSqliteStoreLifecycleEventObserver = void (*)(
    ManagerSqliteStoreLifecycleEvent event,
    void* context) noexcept;

// Internal deterministic-test observation only. Set/Clear require a quiescent
// test boundary and must not race an acquisition. Attempt callbacks run before
// locking; acquired callbacks run synchronously while the gate is held. A
// callback must not call a lifecycle-gate function.
void SetManagerSqliteStoreLifecycleEventObserverForTesting(
    ManagerSqliteStoreLifecycleEventObserver observer,
    void* context) noexcept;
void ClearManagerSqliteStoreLifecycleEventObserverForTesting() noexcept;

}  // namespace manyforge::component_manager::sqlite_startup_internal

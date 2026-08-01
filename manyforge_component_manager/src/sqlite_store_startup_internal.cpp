#include "sqlite_store_startup_internal.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

namespace manyforge::component_manager::sqlite_startup_internal {
namespace {

std::atomic<ManagerSqliteStoreLifecycleEventObserver> lifecycle_observer{};
std::atomic<void*> lifecycle_observer_context{};

void NotifyEvent(const ManagerSqliteStoreLifecycleEvent event) noexcept {
  const auto observer = lifecycle_observer.load();
  if (observer != nullptr) {
    observer(event, lifecycle_observer_context.load());
  }
}

}  // namespace

std::recursive_mutex& ManagerSqliteStoreStartupMutex() noexcept {
  static std::recursive_mutex mutex;
  return mutex;
}

ManagerSqliteStoreStartupLock TryAcquireManagerSqliteStoreStartupLock(
    const std::chrono::milliseconds timeout) {
  NotifyEvent(ManagerSqliteStoreLifecycleEvent::kStartupAttempt);
  ManagerSqliteStoreStartupLock lock(
      ManagerSqliteStoreStartupMutex(), std::defer_lock);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  constexpr auto kMaximumPollInterval = std::chrono::milliseconds(1);
  constexpr auto kMaximumSteadyPollInterval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          kMaximumPollInterval);

  // Preserve one immediate attempt even for a zero or negative timeout.
  if (lock.try_lock()) {
    NotifyEvent(ManagerSqliteStoreLifecycleEvent::kStartupAcquired);
    return lock;
  }
  while (true) {
    const auto now = std::chrono::steady_clock::now();
    if (now >= deadline) {
      return lock;
    }
    std::this_thread::sleep_for(
        std::min(deadline - now, kMaximumSteadyPollInterval));
    if (lock.try_lock()) {
      if (std::chrono::steady_clock::now() <= deadline) {
        NotifyEvent(ManagerSqliteStoreLifecycleEvent::kStartupAcquired);
        return lock;
      }
      lock.unlock();
      return lock;
    }
  }
}

ManagerSqliteStoreStartupLock
AcquireManagerSqliteStoreStartupLockForClose() {
  NotifyEvent(ManagerSqliteStoreLifecycleEvent::kCloseAttempt);
  ManagerSqliteStoreStartupLock lock(
      ManagerSqliteStoreStartupMutex());
  NotifyEvent(ManagerSqliteStoreLifecycleEvent::kCloseAcquired);
  return lock;
}

void SetManagerSqliteStoreLifecycleEventObserverForTesting(
    const ManagerSqliteStoreLifecycleEventObserver observer,
    void* const context) noexcept {
  lifecycle_observer_context.store(context);
  lifecycle_observer.store(observer);
}

void ClearManagerSqliteStoreLifecycleEventObserverForTesting() noexcept {
  lifecycle_observer.store(nullptr);
  lifecycle_observer_context.store(nullptr);
}

}  // namespace manyforge::component_manager::sqlite_startup_internal

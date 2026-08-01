#include "sqlite_scene_lifecycle_internal.hpp"

#include <algorithm>
#include <atomic>
#include <thread>

namespace manyforge::scene::sqlite_lifecycle_internal {
namespace {

std::atomic<CanonicalEmptySceneStoreLifecycleEventObserver>
    lifecycle_observer{};
std::atomic<void*> lifecycle_observer_context{};

void NotifyEvent(
    const CanonicalEmptySceneStoreLifecycleEvent event) noexcept {
  const auto observer = lifecycle_observer.load();
  if (observer != nullptr) {
    observer(event, lifecycle_observer_context.load());
  }
}

}  // namespace

std::mutex& CanonicalEmptySceneStoreLifecycleMutex() noexcept {
  static std::mutex mutex;
  return mutex;
}

CanonicalEmptySceneStoreLifecycleLock
TryAcquireCanonicalEmptySceneStoreLifecycleLock(
    const std::chrono::milliseconds timeout) {
  NotifyEvent(CanonicalEmptySceneStoreLifecycleEvent::kStartupAttempt);
  CanonicalEmptySceneStoreLifecycleLock lock(
      CanonicalEmptySceneStoreLifecycleMutex(), std::defer_lock);
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  constexpr auto kMaximumPollInterval = std::chrono::milliseconds(1);
  constexpr auto kMaximumSteadyPollInterval =
      std::chrono::duration_cast<std::chrono::steady_clock::duration>(
          kMaximumPollInterval);

  if (lock.try_lock()) {
    NotifyEvent(CanonicalEmptySceneStoreLifecycleEvent::kStartupAcquired);
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
        NotifyEvent(
            CanonicalEmptySceneStoreLifecycleEvent::kStartupAcquired);
        return lock;
      }
      lock.unlock();
      return lock;
    }
  }
}

CanonicalEmptySceneStoreLifecycleLock
AcquireCanonicalEmptySceneStoreLifecycleLockForClose() {
  NotifyEvent(CanonicalEmptySceneStoreLifecycleEvent::kCloseAttempt);
  CanonicalEmptySceneStoreLifecycleLock lock(
      CanonicalEmptySceneStoreLifecycleMutex());
  NotifyEvent(CanonicalEmptySceneStoreLifecycleEvent::kCloseAcquired);
  return lock;
}

void SetCanonicalEmptySceneStoreLifecycleEventObserverForTesting(
    const CanonicalEmptySceneStoreLifecycleEventObserver observer,
    void* const context) noexcept {
  lifecycle_observer_context.store(context);
  lifecycle_observer.store(observer);
}

void ClearCanonicalEmptySceneStoreLifecycleEventObserverForTesting()
    noexcept {
  lifecycle_observer.store(nullptr);
  lifecycle_observer_context.store(nullptr);
}

}  // namespace manyforge::scene::sqlite_lifecycle_internal

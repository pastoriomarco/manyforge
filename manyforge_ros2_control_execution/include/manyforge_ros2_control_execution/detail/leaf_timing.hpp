#pragma once

// This instrumentation exists only in the separately compiled timing test.
// Product builds expand the macros to ordinary standard-library locks.
#include <mutex>

#ifdef MANYFORGE_LEAF_TIMING
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>
#include <type_traits>

namespace manyforge::ros2_control_execution::detail {
enum class LeafEntity {
  kMotionGoal, kMotionCancel, kMotionAutomaticCancel, kMotionStopBarrier,
  kMotionScalePublish, kMotionScaleSnapshot, kGripperGoal, kGripperCancel, kGripperPublish,
  kGpioPublish, kGpioRecoveryCopy, kIoServiceSend,
};
struct LeafSample {
  LeafEntity entity{};
  std::int64_t wait_ns{};
  std::int64_t hold_ns{};
  bool acquired{};
};
inline constexpr std::size_t kLeafSampleCapacity = 131072;
inline std::array<LeafSample, kLeafSampleCapacity> leaf_samples{};
inline std::atomic<std::size_t> leaf_sample_count{0};
inline std::mutex leaf_samples_mutex;
// Test-only latch for the bounded scale-deadline regression. A hard local
// timeout and the test's release guard prevent a failed assertion hanging
// its worker; it is never enabled by the timing stress.
inline std::atomic<bool> block_scale_publish{false};
inline std::atomic<bool> scale_publish_entered{false};
inline std::atomic<bool> release_scale_publish{false};
inline std::atomic<bool> block_motion_cancel{false};
inline std::atomic<bool> motion_cancel_entered{false};
inline std::atomic<bool> release_motion_cancel{false};
inline std::atomic<bool> block_gripper_rebind_rollback{false};
inline std::atomic<bool> gripper_rebind_rollback_entered{false};
inline std::atomic<bool> release_gripper_rebind_rollback{false};
inline std::atomic<bool> gripper_rebind_rollback_cas_failed{false};
inline void HoldGripperRebindRollback() {
  if (!block_gripper_rebind_rollback.load()) return;
  gripper_rebind_rollback_entered.store(true);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!release_gripper_rebind_rollback.load() && std::chrono::steady_clock::now() < limit)
    std::this_thread::yield();
}
inline std::atomic<bool> block_gripper_publish{false};
inline std::atomic<bool> gripper_publish_entered{false};
inline std::atomic<bool> release_gripper_publish{false};
inline std::atomic<unsigned> gripper_publish_attempts{0};
inline std::atomic<unsigned> gripper_publish_sdk_entries{0};
inline std::atomic<bool> block_gripper_goal{false};
inline std::atomic<bool> gripper_goal_entered{false};
inline std::atomic<bool> release_gripper_goal{false};
inline std::atomic<bool> block_motion_goal{false};
inline std::atomic<bool> motion_goal_entered{false};
inline std::atomic<bool> release_motion_goal{false};
inline std::atomic<bool> block_stop_barrier{false};
inline std::atomic<bool> stop_barrier_entered{false};
inline std::atomic<bool> release_stop_barrier{false};
inline std::atomic<bool> stop_barrier_expired{false};
inline std::atomic<bool> block_gpio_publish{false};
inline std::atomic<bool> gpio_publish_entered{false};
inline std::atomic<bool> release_gpio_publish{false};
inline std::atomic<bool> gpio_recovery_closed{false};
inline std::atomic<bool> block_io_service_send{false};
inline std::atomic<bool> io_service_admitted_entered{false};
inline std::atomic<bool> release_io_service_send{false};
inline std::atomic<bool> io_service_waiting{false};
inline void HoldIoServiceAfterAdmission() {
  if (!block_io_service_send.load()) return;
  io_service_admitted_entered.store(true);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!release_io_service_send.load() && std::chrono::steady_clock::now() < limit)
    std::this_thread::yield();
}
inline void HoldGpioAfterAdmission() {
  if (!block_gpio_publish.load()) return;
  gpio_publish_entered.store(true);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!release_gpio_publish.load() && std::chrono::steady_clock::now() < limit)
    std::this_thread::yield();
}
inline std::atomic<bool> block_goal_admitted{false};
inline std::atomic<bool> goal_admitted_entered{false};
inline std::atomic<bool> goal_admitted_timed_out{false};
inline std::atomic<bool> release_goal_admitted{false};
inline void GoalAdmittedTestPoint() {
  if (!block_goal_admitted.load()) return;
  goal_admitted_entered.store(true);
  const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (!release_goal_admitted.load() && std::chrono::steady_clock::now() < limit)
    std::this_thread::yield();
  if (!release_goal_admitted.load()) goal_admitted_timed_out.store(true);
}

// Concurrent readers hold leaf_samples_mutex. Callers resetting the count
// still wait for the relevant test threads to finish. Overflow is explicit.
inline void RecordLeafSample(LeafSample sample) noexcept {
  std::lock_guard lock(leaf_samples_mutex);
  const auto index = leaf_sample_count.fetch_add(1, std::memory_order_relaxed);
  if (index < leaf_samples.size()) leaf_samples[index] = sample;
}

template <class Mutex> class TimedLeafLock final {
 public:
  using Clock = std::chrono::steady_clock;
  TimedLeafLock(Mutex& mutex, LeafEntity entity)
      : lock_(mutex, std::defer_lock), entity_(entity) { lock(); }
  TimedLeafLock(Mutex& mutex, LeafEntity entity, std::defer_lock_t)
      : lock_(mutex, std::defer_lock), entity_(entity) {}
  TimedLeafLock(const TimedLeafLock&) = delete;
  TimedLeafLock& operator=(const TimedLeafLock&) = delete;
  ~TimedLeafLock() {
    if (!attempted_) return;
    const auto end = Clock::now();
    const auto held = acquired_ ? end - acquired_at_ : Clock::duration::zero();
    if (acquired_) lock_.unlock();
    RecordLeafSample({entity_, ToNs(acquired_at_ - started_), ToNs(held), acquired_});
  }
  void lock() {
    started_ = Clock::now(); attempted_ = true;
    if (entity_ == LeafEntity::kGripperPublish)
      gripper_publish_attempts.fetch_add(1, std::memory_order_release);
    lock_.lock();
    acquired_at_ = Clock::now(); acquired_ = true;
    HoldForTest();
  }
  template <class ClockType, class Duration>
  bool try_lock_until(const std::chrono::time_point<ClockType, Duration>& deadline) {
    started_ = Clock::now(); attempted_ = true;
    acquired_ = lock_.try_lock_until(deadline);
    acquired_at_ = Clock::now();
    if (acquired_) HoldForTest();
    return acquired_;
  }
  bool owns_lock() const noexcept { return lock_.owns_lock(); }
 private:
  void HoldForTest() {
    std::atomic<bool>* entered = nullptr;
    std::atomic<bool>* release = nullptr;
    if (entity_ == LeafEntity::kMotionScalePublish && block_scale_publish.load()) {
      entered = &scale_publish_entered;
      release = &release_scale_publish;
    } else if (entity_ == LeafEntity::kMotionCancel && block_motion_cancel.load()) {
      entered = &motion_cancel_entered;
      release = &release_motion_cancel;
    } else if (entity_ == LeafEntity::kGripperPublish && block_gripper_publish.load()) {
      entered = &gripper_publish_entered;
      release = &release_gripper_publish;
    } else if (entity_ == LeafEntity::kGripperGoal && block_gripper_goal.load()) {
      entered = &gripper_goal_entered;
      release = &release_gripper_goal;
    } else if (entity_ == LeafEntity::kMotionGoal && block_motion_goal.load()) {
      entered = &motion_goal_entered;
      release = &release_motion_goal;
    } else if (entity_ == LeafEntity::kMotionStopBarrier && block_stop_barrier.load()) {
      entered = &stop_barrier_entered;
      release = &release_stop_barrier;
    }
    if (!entered) return;
    entered->store(true);
    const auto limit = Clock::now() + std::chrono::seconds(
        entity_ == LeafEntity::kMotionStopBarrier ? 5 : 3);
    while (!release->load() && Clock::now() < limit) std::this_thread::yield();
    if (entity_ == LeafEntity::kMotionStopBarrier && !release->load())
      stop_barrier_expired.store(true);
  }
  template <class Duration> static std::int64_t ToNs(Duration value) noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(value).count();
  }
  std::unique_lock<Mutex> lock_;
  LeafEntity entity_;
  Clock::time_point started_{};
  Clock::time_point acquired_at_{};
  bool attempted_{false};
  bool acquired_{false};
};
}  // namespace manyforge::ros2_control_execution::detail

#define MANYFORGE_LEAF_LOCK(name, mutex, entity) \
  ::manyforge::ros2_control_execution::detail::TimedLeafLock name(mutex, entity)
#define MANYFORGE_LEAF_DEFERRED_LOCK(name, mutex, entity) \
  ::manyforge::ros2_control_execution::detail::TimedLeafLock name(mutex, entity, std::defer_lock)
#define MANYFORGE_GOAL_ADMITTED_TEST_POINT() \
  ::manyforge::ros2_control_execution::detail::GoalAdmittedTestPoint()
#define MANYFORGE_GPIO_ADMITTED_TEST_POINT() \
  ::manyforge::ros2_control_execution::detail::HoldGpioAfterAdmission()
#define MANYFORGE_IO_SERVICE_ADMITTED_TEST_POINT() \
  ::manyforge::ros2_control_execution::detail::HoldIoServiceAfterAdmission()
#define MANYFORGE_GRIPPER_REBIND_ROLLBACK_TEST_POINT() (::manyforge::ros2_control_execution::detail::HoldGripperRebindRollback())
#define MANYFORGE_GRIPPER_REBIND_ROLLBACK_RESULT(success) (::manyforge::ros2_control_execution::detail::gripper_rebind_rollback_cas_failed.store(!(success)))
#define MANYFORGE_GRIPPER_PUBLISH_ENTERED() (::manyforge::ros2_control_execution::detail::gripper_publish_sdk_entries.fetch_add(1, std::memory_order_relaxed))
#else
#define MANYFORGE_LEAF_LOCK(name, mutex, entity) std::lock_guard name(mutex)
#define MANYFORGE_LEAF_DEFERRED_LOCK(name, mutex, entity) std::unique_lock name(mutex, std::defer_lock)
#define MANYFORGE_GOAL_ADMITTED_TEST_POINT() ((void)0)
#define MANYFORGE_GPIO_ADMITTED_TEST_POINT() ((void)0)
#define MANYFORGE_IO_SERVICE_ADMITTED_TEST_POINT() ((void)0)
#define MANYFORGE_GRIPPER_REBIND_ROLLBACK_TEST_POINT() ((void)0)
#define MANYFORGE_GRIPPER_REBIND_ROLLBACK_RESULT(success) ((void)0)
#define MANYFORGE_GRIPPER_PUBLISH_ENTERED() ((void)0)
#endif

#pragma once

// Priority-inheritance lock and condition variable (Linux, glibc).
//
// A lock that a priority lane shares with ordinary threads is a PiMutex, and
// a wait on it uses a PiConditionVariable bound to that same lock. While a
// thread waits for a PiMutex, its holder runs at the waiter's priority, so a
// medium-priority thread can no longer delay the waiter without bound.
// std::condition_variable accepts only std::mutex, and
// std::condition_variable_any hands every wait over through its own ordinary
// mutex; PiConditionVariable waits on the PiMutex directly.

#include <pthread.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <ctime>
#include <memory>
#include <mutex>
#include <stop_token>

#if defined(__SANITIZE_THREAD__)
#define MANYFORGE_PI_MUTEX_TSAN 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define MANYFORGE_PI_MUTEX_TSAN 1
#endif
#endif
#ifdef MANYFORGE_PI_MUTEX_TSAN
#include <sanitizer/tsan_interface.h>
#endif

namespace manyforge::core {

namespace detail {
[[noreturn]] void ThrowPiLockError(int error, const char* what);

/// An absolute CLOCK_MONOTONIC time (std::chrono::steady_clock's clock).
template <class Duration>
timespec MonotonicTimespec(
    const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline) noexcept {
  const auto since_epoch =
      std::chrono::ceil<std::chrono::nanoseconds>(deadline.time_since_epoch());
  if (since_epoch.count() <= 0) return {0, 0};
  const auto seconds = std::chrono::floor<std::chrono::seconds>(since_epoch);
  return {static_cast<std::time_t>(seconds.count()),
          static_cast<long>((since_epoch - seconds).count())};
}
}  // namespace detail

/// A pthread mutex with PTHREAD_PRIO_INHERIT. Meets Lockable and
/// TimedLockable (on std::chrono::steady_clock), so std::lock_guard and
/// std::unique_lock work, including their timed forms.
class PiMutex {
 public:
  /// Throws std::system_error when the OS refuses the mutex or its protocol.
  PiMutex();
  ~PiMutex();
  PiMutex(const PiMutex&) = delete;
  PiMutex& operator=(const PiMutex&) = delete;

  void lock() {
    const int error = pthread_mutex_lock(&mutex_);
    if (error != 0) detail::ThrowPiLockError(error, "PiMutex lock");
  }

  [[nodiscard]] bool try_lock() noexcept { return pthread_mutex_trylock(&mutex_) == 0; }

  /// Waits for the lock until `deadline`, like std::timed_mutex.
  template <class Duration>
  [[nodiscard]] bool try_lock_until(
      const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline) noexcept {
    const timespec until = detail::MonotonicTimespec(deadline);
#ifdef MANYFORGE_PI_MUTEX_TSAN
    // GCC's libtsan does not intercept pthread_mutex_clocklock (it does
    // intercept lock, trylock and unlock on this same address). Annotated as
    // a try-lock so TSan's lock-order detector sees the acquisition and the
    // later unlock pairs with it.
    __tsan_mutex_pre_lock(&mutex_, __tsan_mutex_try_lock);
#endif
    const bool acquired = pthread_mutex_clocklock(&mutex_, CLOCK_MONOTONIC, &until) == 0;
#ifdef MANYFORGE_PI_MUTEX_TSAN
    __tsan_mutex_post_lock(
        &mutex_,
        acquired ? __tsan_mutex_try_lock : __tsan_mutex_try_lock | __tsan_mutex_try_lock_failed,
        0);
#endif
    return acquired;
  }

  template <class Rep, class Period>
  [[nodiscard]] bool try_lock_for(const std::chrono::duration<Rep, Period>& timeout) noexcept {
    return try_lock_until(std::chrono::steady_clock::now() +
                          std::chrono::ceil<std::chrono::nanoseconds>(timeout));
  }

  void unlock() noexcept { (void)pthread_mutex_unlock(&mutex_); }

  pthread_mutex_t* native_handle() noexcept { return &mutex_; }

 private:
  pthread_mutex_t mutex_;
};

/// A pthread condition variable on CLOCK_MONOTONIC that waits on a PiMutex.
/// Same interface as std::condition_variable, plus the std::stop_token waits
/// of std::condition_variable_any. Deadlines are std::chrono::steady_clock.
///
/// Plain waits wait on the caller's PiMutex directly. The std::stop_token
/// waits are libstdc++'s std::condition_variable_any algorithm unchanged,
/// with its internal hand-over mutex a PiMutex: the stop callback notifies
/// under that internal mutex only, so request_stop() may be called while
/// holding the waited lock, exactly as with std::condition_variable_any.
class PiConditionVariable {
 public:
  /// Throws std::system_error when the OS refuses a condition variable.
  PiConditionVariable();
  ~PiConditionVariable();
  PiConditionVariable(const PiConditionVariable&) = delete;
  PiConditionVariable& operator=(const PiConditionVariable&) = delete;

  /// May also wake one stop-token waiter, a spurious wakeup to it.
  void notify_one() noexcept {
    (void)pthread_cond_signal(&cond_);
    if (stop_waiters_.load() != 0) {
      std::lock_guard internal(*stop_mutex_);
      (void)pthread_cond_signal(&stop_cond_);
    }
  }

  void notify_all() noexcept {
    (void)pthread_cond_broadcast(&cond_);
    if (stop_waiters_.load() != 0) {
      std::lock_guard internal(*stop_mutex_);
      (void)pthread_cond_broadcast(&stop_cond_);
    }
  }

  void wait(std::unique_lock<PiMutex>& lock) {
    (void)pthread_cond_wait(&cond_, lock.mutex()->native_handle());
  }

  template <class Predicate>
  void wait(std::unique_lock<PiMutex>& lock, Predicate pred) {
    while (!pred()) wait(lock);
  }

  template <class Duration>
  std::cv_status wait_until(
      std::unique_lock<PiMutex>& lock,
      const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline) {
    return TimedWait(&cond_, lock.mutex(), deadline);
  }

  template <class Duration, class Predicate>
  bool wait_until(std::unique_lock<PiMutex>& lock,
                  const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline,
                  Predicate pred) {
    while (!pred())
      if (wait_until(lock, deadline) == std::cv_status::timeout) return pred();
    return true;
  }

  template <class Rep, class Period>
  std::cv_status wait_for(std::unique_lock<PiMutex>& lock,
                          const std::chrono::duration<Rep, Period>& timeout) {
    return wait_until(lock, std::chrono::steady_clock::now() +
                                std::chrono::ceil<std::chrono::nanoseconds>(timeout));
  }

  template <class Rep, class Period, class Predicate>
  bool wait_for(std::unique_lock<PiMutex>& lock,
                const std::chrono::duration<Rep, Period>& timeout, Predicate pred) {
    return wait_until(lock,
                      std::chrono::steady_clock::now() +
                          std::chrono::ceil<std::chrono::nanoseconds>(timeout),
                      std::move(pred));
  }

  // std::stop_token waits: libstdc++ 13's std::condition_variable_any
  // (<condition_variable>, wait / wait_until with a stop_token) step for
  // step, with stop_mutex_ and stop_cond_ in place of its _M_mutex/_M_cond.

  template <class Predicate>
  bool wait(std::unique_lock<PiMutex>& lock, std::stop_token stop, Predicate pred) {
    if (stop.stop_requested()) return pred();
    const StopWaiter registered(stop_waiters_);
    std::stop_callback wake(stop, [this] { notify_all(); });
    const std::shared_ptr<PiMutex> internal_mutex = stop_mutex_;
    while (!pred()) {
      std::unique_lock internal(*internal_mutex);
      if (stop.stop_requested()) return false;
      // The internal lock is released before the caller's lock is retaken.
      const Unlock unlock(lock);
      std::unique_lock internal_wait(std::move(internal));
      (void)pthread_cond_wait(&stop_cond_, internal_mutex->native_handle());
    }
    return true;
  }

  template <class Duration, class Predicate>
  bool wait_until(std::unique_lock<PiMutex>& lock, std::stop_token stop,
                  const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline,
                  Predicate pred) {
    if (stop.stop_requested()) return pred();
    const StopWaiter registered(stop_waiters_);
    std::stop_callback wake(stop, [this] { notify_all(); });
    const std::shared_ptr<PiMutex> internal_mutex = stop_mutex_;
    while (!pred()) {
      bool stopped;
      {
        std::unique_lock internal(*internal_mutex);
        if (stop.stop_requested()) return false;
        const Unlock unlock(lock);
        std::unique_lock internal_wait(std::move(internal));
        const auto status = TimedWait(&stop_cond_, internal_mutex.get(), deadline);
        stopped = status == std::cv_status::timeout || stop.stop_requested();
      }
      if (stopped) return pred();
    }
    return true;
  }

  template <class Rep, class Period, class Predicate>
  bool wait_for(std::unique_lock<PiMutex>& lock, std::stop_token stop,
                const std::chrono::duration<Rep, Period>& timeout, Predicate pred) {
    return wait_until(lock, std::move(stop), std::chrono::steady_clock::now() + timeout,
                      std::move(pred));
  }

 private:
  /// Counts stop-token waiters, so a notify takes the internal mutex only
  /// when one may be waiting. Registered under the caller's lock before the
  /// first predicate check: a notifier that changed the state under that
  /// lock afterwards sees the count.
  class StopWaiter {
   public:
    explicit StopWaiter(std::atomic<unsigned>& count) : count_(count) { count_.fetch_add(1); }
    ~StopWaiter() { count_.fetch_sub(1); }
    StopWaiter(const StopWaiter&) = delete;
    StopWaiter& operator=(const StopWaiter&) = delete;

   private:
    std::atomic<unsigned>& count_;
  };

  /// libstdc++'s _Unlock: releases the caller's lock, retakes it on exit.
  class Unlock {
   public:
    explicit Unlock(std::unique_lock<PiMutex>& lock) : lock_(lock) { lock_.unlock(); }
    ~Unlock() { lock_.lock(); }
    Unlock(const Unlock&) = delete;
    Unlock& operator=(const Unlock&) = delete;

   private:
    std::unique_lock<PiMutex>& lock_;
  };

  template <class Duration>
  static std::cv_status TimedWait(
      pthread_cond_t* cond, PiMutex* mutex,
      const std::chrono::time_point<std::chrono::steady_clock, Duration>& deadline) {
    const timespec until = detail::MonotonicTimespec(deadline);
    return pthread_cond_timedwait(cond, mutex->native_handle(), &until) == ETIMEDOUT
               ? std::cv_status::timeout
               : std::cv_status::no_timeout;
  }

  pthread_cond_t cond_;
  pthread_cond_t stop_cond_;
  // Shared like libstdc++'s _M_mutex: a woken waiter may still release it
  // after the condition variable is destroyed.
  std::shared_ptr<PiMutex> stop_mutex_;
  std::atomic<unsigned> stop_waiters_{0};
};

}  // namespace manyforge::core

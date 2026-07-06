#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace manyforge::core {

/// Owns execute-worker completion and resume wakeups.
///
/// The execution worker, lifecycle APIs, and tests all wait on the same
/// condition variable. Keep the predicates protected by this class so teardown
/// and resume paths cannot regress to notify-without-state missed wakeups.
class ExecutionWorkerSignals {
 public:
  ExecutionWorkerSignals() = default;
  ExecutionWorkerSignals(const ExecutionWorkerSignals&) = delete;
  ExecutionWorkerSignals& operator=(const ExecutionWorkerSignals&) = delete;

  void mark_started();
  void mark_complete();
  void abandon();
  bool wait_for_completion(std::int64_t timeout_ns);

  void request_resume();
  bool consume_resume_request();
  void clear_resume_request();

  void notify();

  template <typename Predicate>
  bool wait_for_resume_or(std::chrono::nanoseconds timeout,
                          Predicate&& predicate) {
    std::unique_lock<std::mutex> lock(mutex_);
    return cv_.wait_for(lock, timeout, [this, &predicate]() {
      return resume_requested_ || predicate();
    });
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool execution_complete_{false};
  bool abandoned_{false};
  bool resume_requested_{false};
};

}  // namespace manyforge::core

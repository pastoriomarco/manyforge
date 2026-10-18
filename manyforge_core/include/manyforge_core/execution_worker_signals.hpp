#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace manyforge::core {

/// Owns execute-worker completion wakeups.
///
/// The execution worker, lifecycle APIs, and tests all wait on the same
/// condition variable. Keep the predicates protected by this class so teardown
/// paths cannot regress to notify-without-state missed wakeups.
class ExecutionWorkerSignals {
 public:
  ExecutionWorkerSignals() = default;
  ExecutionWorkerSignals(const ExecutionWorkerSignals&) = delete;
  ExecutionWorkerSignals& operator=(const ExecutionWorkerSignals&) = delete;

  void mark_started();
  void mark_complete();
  void abandon();
  bool wait_for_completion(std::int64_t timeout_ns);
  [[nodiscard]] bool complete() const;

  void notify();

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  bool execution_complete_{false};
  bool abandoned_{false};
};

}  // namespace manyforge::core

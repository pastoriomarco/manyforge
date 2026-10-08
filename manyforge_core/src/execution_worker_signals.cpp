#include <manyforge_core/execution_worker_signals.hpp>

namespace manyforge::core {

void ExecutionWorkerSignals::mark_started() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    execution_complete_ = false;
    abandoned_ = false;
  }
  cv_.notify_all();
}

void ExecutionWorkerSignals::mark_complete() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    execution_complete_ = true;
  }
  cv_.notify_all();
}

void ExecutionWorkerSignals::abandon() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    abandoned_ = true;
  }
  cv_.notify_all();
}

bool ExecutionWorkerSignals::wait_for_completion(std::int64_t timeout_ns) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout_ns > 0) {
    const bool ready = cv_.wait_for(
        lock, std::chrono::nanoseconds(timeout_ns),
        [this]() { return execution_complete_ || abandoned_; });
    return ready && execution_complete_;
  }
  cv_.wait(lock, [this]() { return execution_complete_ || abandoned_; });
  return execution_complete_;
}

void ExecutionWorkerSignals::notify() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
  }
  cv_.notify_all();
}

}  // namespace manyforge::core

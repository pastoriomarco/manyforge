#include <manyforge_core/execution_worker_signals.hpp>

namespace manyforge::core {

void ExecutionWorkerSignals::mark_started() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    execution_complete_ = false;
    resume_requested_ = false;
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

bool ExecutionWorkerSignals::wait_for_completion(std::int64_t timeout_ns) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (timeout_ns > 0) {
    return cv_.wait_for(lock, std::chrono::nanoseconds(timeout_ns), [this]() {
      return execution_complete_;
    });
  }
  cv_.wait(lock, [this]() { return execution_complete_; });
  return true;
}

void ExecutionWorkerSignals::request_resume() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resume_requested_ = true;
  }
  cv_.notify_all();
}

bool ExecutionWorkerSignals::consume_resume_request() {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool requested = resume_requested_;
  resume_requested_ = false;
  return requested;
}

void ExecutionWorkerSignals::clear_resume_request() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    resume_requested_ = false;
  }
  cv_.notify_all();
}

void ExecutionWorkerSignals::notify() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
  }
  cv_.notify_all();
}

}  // namespace manyforge::core

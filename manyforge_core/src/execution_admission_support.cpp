#include <manyforge_core/execution_admission_support.hpp>

namespace manyforge::core {

void WorkerStartGate::release() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    released_ = true;
  }
  cv_.notify_all();
}

bool WorkerStartGate::wait(std::stop_token st) {
  // The stop-token-aware wait is required here: a plain condition_variable
  // woken from a stop_callback that calls notify without holding this mutex can
  // lose the wakeup, because the stop request flips st.stop_requested() outside
  // the mutex and can slip between the waiter's predicate check and enqueue.
  std::unique_lock<std::mutex> lock(mutex_);
  cv_.wait(lock, st, [this]() { return released_; });
  return released_ && !st.stop_requested();
}

}  // namespace manyforge::core

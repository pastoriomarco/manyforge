#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>

namespace manyforge::core {

/// Process-wide "something an events reader sees may have changed" signal:
/// raised after every committed operation-journal write, after a global rule
/// state projection or catalog install, and by the execution host's
/// carried-object-lost store. The event stream writers wait on it instead of
/// polling. A spurious raise only costs one re-read.
class ExecutionEventChangeSignal final {
public:
  void Notify() noexcept {
    {
      std::lock_guard lock(mutex_);
      ++generation_;
    }
    changed_.notify_all();
  }

  [[nodiscard]] std::uint64_t Generation() const {
    std::lock_guard lock(mutex_);
    return generation_;
  }

  /// Waits until the generation differs from `seen`, `deadline`, or `stop`;
  /// returns the current generation.
  std::uint64_t WaitAfter(const std::uint64_t seen,
                          const std::chrono::steady_clock::time_point deadline,
                          const std::stop_token stop) {
    std::unique_lock lock(mutex_);
    changed_.wait_until(lock, stop, deadline,
                        [&] { return generation_ != seen; });
    return generation_;
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  std::uint64_t generation_{};
};

/// The one signal of this process.
inline ExecutionEventChangeSignal &execution_event_change_signal() {
  static ExecutionEventChangeSignal signal;
  return signal;
}

} // namespace manyforge::core

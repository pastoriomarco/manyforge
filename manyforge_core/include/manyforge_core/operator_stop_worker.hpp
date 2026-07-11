#pragma once

#include <manyforge_core/types/error_code.hpp>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

namespace manyforge::core {

/// Owns the cancellable worker used by operator controlled-stop.
///
/// The worker body is still supplied by ExecutionKernel in K11 so stop
/// semantics remain unchanged; this collaborator owns only the thread,
/// generation, active flag, wait/cancel lifecycle, and stop diagnostic slots.
class OperatorStopWorker {
 public:
  using WorkerBody =
      std::function<void(std::uint64_t generation, std::stop_token stop_token)>;
  using CompletionCallback = std::function<void()>;

  OperatorStopWorker() = default;
  OperatorStopWorker(const OperatorStopWorker&) = delete;
  OperatorStopWorker& operator=(const OperatorStopWorker&) = delete;
  ~OperatorStopWorker();

  bool start(WorkerBody body, CompletionCallback on_finished = nullptr);
  bool cancel(bool invalidate_generation);
  void join_finished();
  void stop_and_join();
  bool wait_for(std::chrono::steady_clock::duration timeout);
  bool is_current(std::uint64_t generation, std::stop_token stop_token) const;
  bool active() const;

  void set_terminal_error(ErrorDTO error);
  void set_intervention_error(ErrorDTO error);
  std::optional<ErrorDTO> terminal_error() const;
  std::optional<ErrorDTO> intervention_error() const;
  void clear_errors();

 private:
  void mark_finished();

  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::jthread thread_;
  std::uint64_t generation_{0};
  std::atomic<bool> active_{false};

  mutable std::mutex error_mutex_;
  std::optional<ErrorDTO> terminal_error_;
  std::optional<ErrorDTO> intervention_error_;
};

}  // namespace manyforge::core

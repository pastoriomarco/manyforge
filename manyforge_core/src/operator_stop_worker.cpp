#include <manyforge_core/operator_stop_worker.hpp>

#include <utility>

namespace manyforge::core {

OperatorStopWorker::~OperatorStopWorker() {
  stop_and_join();
}

bool OperatorStopWorker::start(WorkerBody body,
                               CompletionCallback on_finished) {
  if (!body) {
    return false;
  }
  for (;;) {
    join_finished();

    std::jthread stale_finished_thread;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      if (active_.load(std::memory_order_acquire)) {
        return false;
      }
      if (thread_.joinable()) {
        stale_finished_thread = std::move(thread_);
      } else {
        const auto generation = ++generation_;
        active_.store(true, std::memory_order_release);
        thread_ = std::jthread(
            [this, generation, body = std::move(body),
             on_finished = std::move(on_finished)](
                std::stop_token stop_token) mutable {
              struct ActiveGuard {
                OperatorStopWorker* self;
                CompletionCallback* on_finished;
                ~ActiveGuard() noexcept {
                  self->mark_finished();
                  if (*on_finished) {
                    try {
                      (*on_finished)();
                    } catch (...) {
                      // Completion refresh is best-effort telemetry. A
                      // callback defect must not escape the worker's teardown
                      // guard and terminate the process after a safe stop.
                    }
                  }
                }
              } guard{this, &on_finished};
              body(generation, stop_token);
            });
        return true;
      }
    }
    if (stale_finished_thread.joinable()) {
      stale_finished_thread.join();
    }
  }
}

bool OperatorStopWorker::cancel(bool invalidate_generation) {
  std::lock_guard<std::mutex> lock(mutex_);
  const bool was_active = active_.load(std::memory_order_acquire);
  if (was_active && thread_.joinable()) {
    thread_.request_stop();
  }
  if (invalidate_generation) {
    ++generation_;
  }
  return was_active;
}

void OperatorStopWorker::join_finished() {
  std::jthread finished;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!thread_.joinable() ||
        active_.load(std::memory_order_acquire)) {
      return;
    }
    finished = std::move(thread_);
  }
  finished.join();
}

void OperatorStopWorker::stop_and_join() {
  std::jthread worker;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    ++generation_;
    if (thread_.joinable()) {
      thread_.request_stop();
      worker = std::move(thread_);
    }
  }
  if (worker.joinable()) {
    worker.join();
  }
  cv_.notify_all();
}

bool OperatorStopWorker::wait_for(
    std::chrono::steady_clock::duration timeout) {
  std::unique_lock<std::mutex> lock(mutex_);
  if (!active_.load(std::memory_order_acquire)) {
    return true;
  }
  return cv_.wait_for(lock, timeout, [this]() {
    return !active_.load(std::memory_order_acquire);
  });
}

bool OperatorStopWorker::is_current(std::uint64_t generation,
                                    std::stop_token stop_token) const {
  if (stop_token.stop_requested()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  return active_.load(std::memory_order_acquire) &&
         generation == generation_;
}

bool OperatorStopWorker::active() const {
  return active_.load(std::memory_order_acquire);
}

void OperatorStopWorker::set_terminal_error(ErrorDTO error) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  terminal_error_ = std::move(error);
}

void OperatorStopWorker::set_intervention_error(ErrorDTO error) {
  std::lock_guard<std::mutex> lock(error_mutex_);
  intervention_error_ = std::move(error);
}

std::optional<ErrorDTO> OperatorStopWorker::terminal_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return terminal_error_;
}

std::optional<ErrorDTO> OperatorStopWorker::intervention_error() const {
  std::lock_guard<std::mutex> lock(error_mutex_);
  return intervention_error_;
}

void OperatorStopWorker::clear_errors() {
  std::lock_guard<std::mutex> lock(error_mutex_);
  terminal_error_.reset();
  intervention_error_.reset();
}

void OperatorStopWorker::mark_finished() {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    active_.store(false, std::memory_order_release);
  }
  cv_.notify_all();
}

}  // namespace manyforge::core

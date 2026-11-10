#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <unistd.h>

#include <manyforge_component_manager/execution_host_exit.hpp>

namespace manyforge::roboplan_backend::host_detail {

class HostShutdownWatchdog final {
public:
  std::atomic<const char *> phase{"surface_drain"};
  std::chrono::steady_clock::time_point deadline;

  void Start(std::chrono::steady_clock::time_point until) {
    deadline = until;
    worker_ = std::jthread([this](std::stop_token stop) {
      std::unique_lock lock(mutex_);
      if (!wake_.wait_until(lock, stop, deadline, [] { return false; }) &&
          !stop.stop_requested()) {
        char line[256];
        const int length = std::snprintf(
            line, sizeof(line),
            "{\"code\":\"execution_host_shutdown_incomplete\",\"phase\":\"%s\"}\n",
            phase.load(std::memory_order_acquire));
        if (length > 0) {
          const auto size = static_cast<std::size_t>(length) < sizeof(line)
                                ? static_cast<std::size_t>(length)
                                : sizeof(line) - 1U;
          const auto written = ::write(STDERR_FILENO, line, size);
          static_cast<void>(written);
        }
        std::_Exit(component_manager::kExecutionHostShutdownIncompleteExitStatus);
      }
    });
  }

  ~HostShutdownWatchdog() {
    worker_.request_stop();
    wake_.notify_all();
  }

private:
  std::mutex mutex_;
  std::condition_variable_any wake_;
  std::jthread worker_;
};

} // namespace manyforge::roboplan_backend::host_detail

#pragma once

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace manyforge::roboplan_backend {

// One worker coalesces notifications while its caller applies the latest Scene.
class CommittedToolPayloadFollower final {
public:
  explicit CommittedToolPayloadFollower(std::function<void()> apply)
      : apply_(std::move(apply)), worker_([this](std::stop_token stop) {
          std::uint64_t seen = 0U;
          for (;;) {
            {
              std::unique_lock lock(mutex_);
              if (!ready_.wait(lock, stop, [&] { return generation_ != seen; }))
                return;
              seen = generation_;
            }
            apply_();
          }
        }) {}

  void Notify() {
    {
      std::lock_guard lock(mutex_);
      ++generation_;
    }
    ready_.notify_one();
  }

private:
  std::function<void()> apply_;
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::uint64_t generation_{};
  std::jthread worker_;
};

} // namespace manyforge::roboplan_backend

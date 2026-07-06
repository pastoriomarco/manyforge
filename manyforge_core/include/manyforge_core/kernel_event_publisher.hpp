#pragma once

#include <manyforge_core/types/kernel_event.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

namespace manyforge::core {

/// Thread-safe event subscriber registry with copy-on-write dispatch snapshots.
///
/// Dispatch never holds the subscriber mutex while invoking callbacks. Throwing
/// callbacks are isolated from later subscribers and counted for the kernel's
/// metrics surface.
class KernelEventPublisher {
 public:
  SubscriptionHandle subscribe(EventCallback callback);
  void unsubscribe(SubscriptionHandle handle);

  /// Dispatch a prepared event. Returns the number of subscriber callbacks
  /// that threw and were isolated.
  std::uint64_t dispatch(const KernelEvent& event) const;

 private:
  void rebuild_snapshot_locked();

  mutable std::mutex mutex_;
  std::map<std::uint64_t, EventCallback> subscribers_;
  std::shared_ptr<const std::vector<std::pair<std::uint64_t, EventCallback>>>
      snapshot_ =
          std::make_shared<const std::vector<std::pair<std::uint64_t, EventCallback>>>();
  std::uint64_t next_subscription_id_ = 1;
};

}  // namespace manyforge::core

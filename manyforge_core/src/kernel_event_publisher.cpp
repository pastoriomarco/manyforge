#include <manyforge_core/kernel_event_publisher.hpp>

#include <manyforge_core/types/reason_codes.hpp>

#include <exception>
#include <iostream>
#include <utility>

namespace manyforge::core {

SubscriptionHandle KernelEventPublisher::subscribe(EventCallback callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  const std::uint64_t id = next_subscription_id_++;
  subscribers_[id] = std::move(callback);
  rebuild_snapshot_locked();
  return SubscriptionHandle{id};
}

void KernelEventPublisher::unsubscribe(SubscriptionHandle handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (subscribers_.erase(handle.id) > 0) {
    rebuild_snapshot_locked();
  }
}

std::uint64_t KernelEventPublisher::dispatch(const KernelEvent& event) const {
  std::shared_ptr<const std::vector<std::pair<std::uint64_t, EventCallback>>>
      callbacks;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    callbacks = snapshot_;
  }

  std::uint64_t subscriber_exceptions = 0;
  const auto record_subscriber_exception =
      [&subscriber_exceptions](std::uint64_t subscriber_id,
                               const char* exception_type,
                               const char* exception_message) {
        ++subscriber_exceptions;
        std::cerr << "manyforge_kernel_event_subscriber_exception"
                  << " reason_code=" << reason_codes::kEventSubscriberException
                  << " subscriber_id=" << subscriber_id
                  << " exception_type=" << exception_type
                  << " exception=" << exception_message << '\n';
      };

  for (const auto& [subscriber_id, callback] : *callbacks) {
    try {
      callback(event);
    } catch (const std::exception& e) {
      record_subscriber_exception(subscriber_id, "std_exception", e.what());
    } catch (...) {
      record_subscriber_exception(subscriber_id,
                                  reason_codes::kUnknownNonStdException,
                                  reason_codes::kUnknownNonStdException);
    }
  }
  return subscriber_exceptions;
}

void KernelEventPublisher::rebuild_snapshot_locked() {
  auto snapshot =
      std::make_shared<std::vector<std::pair<std::uint64_t, EventCallback>>>();
  snapshot->reserve(subscribers_.size());
  for (const auto& [id, callback] : subscribers_) {
    snapshot->emplace_back(id, callback);
  }
  snapshot_ = std::move(snapshot);
}

}  // namespace manyforge::core

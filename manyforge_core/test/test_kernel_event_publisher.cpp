#include <manyforge_core/kernel_event_publisher.hpp>

#include <gtest/gtest.h>

#include <stdexcept>
#include <vector>

namespace manyforge::core {
namespace {

KernelEvent make_event() {
  return KernelEvent{KernelEventType::kPlanStarted,
                     7,
                     123,
                     {},
                     {},
                     std::nullopt,
                     {{"key", "value"}}};
}

TEST(KernelEventPublisherTest, DispatchUsesCopyOnWriteSubscriberSnapshot) {
  KernelEventPublisher publisher;
  std::vector<std::uint64_t> seen;

  auto first = publisher.subscribe([&](const KernelEvent& event) {
    seen.push_back(event.execution_id);
  });
  publisher.subscribe([&](const KernelEvent& event) {
    seen.push_back(event.execution_id + 1);
  });

  EXPECT_EQ(publisher.dispatch(make_event()), 0u);
  ASSERT_EQ(seen.size(), 2u);
  EXPECT_EQ(seen[0], 7u);
  EXPECT_EQ(seen[1], 8u);

  publisher.unsubscribe(first);
  EXPECT_EQ(publisher.dispatch(make_event()), 0u);
  ASSERT_EQ(seen.size(), 3u);
  EXPECT_EQ(seen[2], 8u);
}

TEST(KernelEventPublisherTest, ThrowingSubscriberDoesNotBlockLaterSubscribers) {
  KernelEventPublisher publisher;
  bool later_subscriber_called = false;

  publisher.subscribe([](const KernelEvent&) {
    throw std::runtime_error("subscriber failed");
  });
  publisher.subscribe([&](const KernelEvent&) {
    later_subscriber_called = true;
  });

  EXPECT_EQ(publisher.dispatch(make_event()), 1u);
  EXPECT_TRUE(later_subscriber_called);
}

}  // namespace
}  // namespace manyforge::core

#include <manyforge_core/execution_worker_signals.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <thread>

namespace manyforge::core {
namespace {

TEST(ExecutionWorkerSignalsTest, CompletionWaitReturnsAfterMarkComplete) {
  ExecutionWorkerSignals signals;
  signals.mark_started();

  auto waiter = std::async(std::launch::async, [&]() {
    return signals.wait_for_completion(1'000'000'000);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  signals.mark_complete();

  ASSERT_EQ(waiter.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(waiter.get());
}

TEST(ExecutionWorkerSignalsTest, CompletionWaitTimesOutWhenIncomplete) {
  ExecutionWorkerSignals signals;
  signals.mark_started();

  EXPECT_FALSE(signals.wait_for_completion(1'000'000));
}

TEST(ExecutionWorkerSignalsTest, CompletionWaitReturnsFalseWhenAbandoned) {
  ExecutionWorkerSignals signals;
  signals.mark_started();

  auto waiter = std::async(std::launch::async, [&]() {
    return signals.wait_for_completion(0);
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  signals.abandon();

  ASSERT_EQ(waiter.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(waiter.get());
}

TEST(ExecutionWorkerSignalsTest, MarkStartedClearsPreviousAbandon) {
  ExecutionWorkerSignals signals;
  signals.mark_started();
  signals.abandon();
  EXPECT_FALSE(signals.wait_for_completion(1'000'000));

  signals.mark_started();
  signals.mark_complete();
  EXPECT_TRUE(signals.wait_for_completion(1'000'000));
}

}  // namespace
}  // namespace manyforge::core

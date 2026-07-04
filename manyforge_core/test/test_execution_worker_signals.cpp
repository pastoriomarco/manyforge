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

TEST(ExecutionWorkerSignalsTest, ResumeRequestWakesAndConsumesOnce) {
  ExecutionWorkerSignals signals;
  signals.mark_started();

  auto waiter = std::async(std::launch::async, [&]() {
    return signals.wait_for_resume_or(std::chrono::seconds(1), []() {
      return false;
    });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  signals.request_resume();

  ASSERT_EQ(waiter.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(waiter.get());
  EXPECT_TRUE(signals.consume_resume_request());
  EXPECT_FALSE(signals.consume_resume_request());
}

TEST(ExecutionWorkerSignalsTest, ExternalPredicateWakesOnNotify) {
  ExecutionWorkerSignals signals;
  std::atomic<bool> external_ready{false};

  auto waiter = std::async(std::launch::async, [&]() {
    return signals.wait_for_resume_or(std::chrono::seconds(1), [&]() {
      return external_ready.load(std::memory_order_acquire);
    });
  });
  std::this_thread::sleep_for(std::chrono::milliseconds(5));
  external_ready.store(true, std::memory_order_release);
  signals.notify();

  ASSERT_EQ(waiter.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_TRUE(waiter.get());
  EXPECT_FALSE(signals.consume_resume_request());
}

TEST(ExecutionWorkerSignalsTest, MarkStartedClearsStaleResume) {
  ExecutionWorkerSignals signals;
  signals.request_resume();
  ASSERT_TRUE(signals.consume_resume_request());

  signals.request_resume();
  signals.mark_started();

  EXPECT_FALSE(signals.consume_resume_request());
}

}  // namespace
}  // namespace manyforge::core

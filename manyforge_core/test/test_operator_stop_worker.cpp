#include <manyforge_core/operator_stop_worker.hpp>

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <stdexcept>
#include <thread>

namespace manyforge::core {
namespace {

struct JoinDelayBodyState {
  std::promise<void> entered;
  std::promise<void> destructor_entered;
  std::atomic<bool> destructor_notified{false};
  std::atomic<bool> allow_destructor_block{false};
  std::atomic<bool> release_destructor{false};
};

struct JoinDelayBody {
  std::shared_ptr<JoinDelayBodyState> state;

  void operator()(std::uint64_t, std::stop_token stop_token) const {
    state->entered.set_value();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
  }

  ~JoinDelayBody() {
    if (state == nullptr ||
        !state->allow_destructor_block.load(std::memory_order_acquire)) {
      return;
    }
    bool expected = false;
    if (state->destructor_notified.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
      state->destructor_entered.set_value();
    }
    while (!state->release_destructor.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
  }
};

TEST(OperatorStopWorkerTest, StartRunsBodyAndJoinFinishedReapsThread) {
  OperatorStopWorker worker;
  std::atomic<bool> ran{false};

  ASSERT_TRUE(worker.start([&](std::uint64_t generation,
                               std::stop_token stop_token) {
    EXPECT_EQ(generation, 1U);
    EXPECT_FALSE(stop_token.stop_requested());
    ran.store(true, std::memory_order_release);
  }));

  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  EXPECT_TRUE(ran.load(std::memory_order_acquire));
  EXPECT_FALSE(worker.active());
  worker.join_finished();
}

TEST(OperatorStopWorkerTest, RejectsSecondStartWhileActive) {
  OperatorStopWorker worker;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();

  ASSERT_TRUE(worker.start([&](std::uint64_t, std::stop_token) {
    entered.set_value();
    release_future.wait();
  }));
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  EXPECT_FALSE(worker.start([](std::uint64_t, std::stop_token) {}));
  release.set_value();
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  worker.join_finished();
}

TEST(OperatorStopWorkerTest, CancelRequestsStopAndWaitsForCompletion) {
  OperatorStopWorker worker;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::atomic<bool> saw_stop{false};

  ASSERT_TRUE(worker.start([&](std::uint64_t, std::stop_token stop_token) {
    entered.set_value();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
    saw_stop.store(true, std::memory_order_release);
  }));
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  EXPECT_TRUE(worker.cancel(/*invalidate_generation=*/false));
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  EXPECT_TRUE(saw_stop.load(std::memory_order_acquire));
  worker.join_finished();
}

TEST(OperatorStopWorkerTest, StopAndJoinDoesNotAllowRestartUntilBodyReturns) {
  OperatorStopWorker worker;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> saw_stop;
  auto saw_stop_future = saw_stop.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();

  ASSERT_TRUE(worker.start([&](std::uint64_t, std::stop_token stop_token) {
    entered.set_value();
    while (!stop_token.stop_requested()) {
      std::this_thread::yield();
    }
    saw_stop.set_value();
    release_future.wait();
  }));
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  auto join_future = std::async(std::launch::async, [&]() {
    worker.stop_and_join();
  });
  ASSERT_EQ(saw_stop_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  EXPECT_FALSE(worker.start([](std::uint64_t, std::stop_token) {}));
  release.set_value();
  ASSERT_EQ(join_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  ASSERT_TRUE(worker.start([](std::uint64_t, std::stop_token) {}));
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  worker.join_finished();
}

TEST(OperatorStopWorkerTest,
     StopAndJoinDoesNotClearReplacementStartedWhileOldThreadIsJoining) {
  OperatorStopWorker worker;
  auto state = std::make_shared<JoinDelayBodyState>();
  auto entered_future = state->entered.get_future();
  auto destructor_entered_future = state->destructor_entered.get_future();

  ASSERT_TRUE(worker.start(JoinDelayBody{state}));
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  state->allow_destructor_block.store(true, std::memory_order_release);

  auto join_future = std::async(std::launch::async, [&]() {
    worker.stop_and_join();
  });
  ASSERT_EQ(destructor_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  std::promise<void> replacement_entered;
  auto replacement_entered_future = replacement_entered.get_future();
  std::promise<void> release_replacement;
  auto release_replacement_future = release_replacement.get_future();
  ASSERT_TRUE(worker.start([&](std::uint64_t, std::stop_token) {
    replacement_entered.set_value();
    release_replacement_future.wait();
  }));
  ASSERT_EQ(replacement_entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  state->release_destructor.store(true, std::memory_order_release);
  ASSERT_EQ(join_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);
  EXPECT_TRUE(worker.active());

  release_replacement.set_value();
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  worker.join_finished();
}

TEST(OperatorStopWorkerTest, InvalidatingGenerationMakesOldWorkerNotCurrent) {
  OperatorStopWorker worker;
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto release_future = release.get_future();
  std::atomic<bool> current_before_cancel{false};
  std::atomic<bool> current_after_cancel{true};

  ASSERT_TRUE(worker.start([&](std::uint64_t generation,
                               std::stop_token stop_token) {
    current_before_cancel.store(worker.is_current(generation, stop_token),
                                std::memory_order_release);
    entered.set_value();
    release_future.wait();
    current_after_cancel.store(worker.is_current(generation, stop_token),
                               std::memory_order_release);
  }));
  ASSERT_EQ(entered_future.wait_for(std::chrono::seconds(1)),
            std::future_status::ready);

  EXPECT_TRUE(worker.cancel(/*invalidate_generation=*/true));
  release.set_value();
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  EXPECT_TRUE(current_before_cancel.load(std::memory_order_acquire));
  EXPECT_FALSE(current_after_cancel.load(std::memory_order_acquire));
  worker.join_finished();
}

TEST(OperatorStopWorkerTest, CompletionRunsAfterWorkerBecomesInactive) {
  OperatorStopWorker worker;
  std::atomic<bool> callback_ran{false};
  std::atomic<bool> callback_observed_inactive{false};

  ASSERT_TRUE(worker.start(
      [](std::uint64_t, std::stop_token) {},
      [&]() {
        callback_observed_inactive.store(!worker.active(),
                                         std::memory_order_release);
        callback_ran.store(true, std::memory_order_release);
      }));
  ASSERT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  worker.join_finished();

  EXPECT_TRUE(callback_ran.load(std::memory_order_acquire));
  EXPECT_TRUE(
      callback_observed_inactive.load(std::memory_order_acquire));
}

TEST(OperatorStopWorkerTest, CompletionExceptionDoesNotTerminateWorker) {
  OperatorStopWorker worker;

  ASSERT_TRUE(worker.start(
      [](std::uint64_t, std::stop_token) {},
      []() { throw std::runtime_error("completion callback failed"); }));
  EXPECT_TRUE(worker.wait_for(std::chrono::seconds(1)));
  worker.join_finished();
  EXPECT_FALSE(worker.active());
}

TEST(OperatorStopWorkerTest, StoresAndClearsTerminalDiagnostics) {
  OperatorStopWorker worker;
  worker.set_terminal_error(ErrorDTO{"terminal_code", "terminal", {}});
  worker.set_intervention_error(ErrorDTO{"intervention_code", "intervention", {}});

  ASSERT_TRUE(worker.terminal_error().has_value());
  EXPECT_EQ(worker.terminal_error()->code, "terminal_code");
  ASSERT_TRUE(worker.intervention_error().has_value());
  EXPECT_EQ(worker.intervention_error()->code, "intervention_code");

  worker.clear_errors();

  EXPECT_FALSE(worker.terminal_error().has_value());
  EXPECT_FALSE(worker.intervention_error().has_value());
}

}  // namespace
}  // namespace manyforge::core

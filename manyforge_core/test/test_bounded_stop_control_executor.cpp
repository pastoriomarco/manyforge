#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <future>
#include <manyforge_core/bounded_stop_control_executor.hpp>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <thread>
#include <vector>

namespace {

thread_local bool track_allocations = false;
thread_local std::size_t tracked_allocations = 0U;

void* Allocate(const std::size_t size) {
  if (track_allocations) {
    ++tracked_allocations;
  }
  if (void* memory = std::malloc(size == 0U ? 1U : size); memory != nullptr) {
    return memory;
  }
  throw std::bad_alloc();
}

}  // namespace

void* operator new(const std::size_t size) {
  return Allocate(size);
}
void* operator new[](const std::size_t size) {
  return Allocate(size);
}
void operator delete(void* memory) noexcept {
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
  std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
  std::free(memory);
}

namespace manyforge::core {
namespace {

using namespace std::chrono_literals;

class SteadyClock final : public IClock {
 public:
  TimePoint now() const override {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
  }
};

class RecordingReceiver final : public IStopControlWorkReceiver {
 public:
  void ProgressStop(const StopControlWorkToken token) noexcept override {
    std::unique_lock lock(mutex_);
    if (count_ < records_.size()) {
      records_[count_] = token;
    }
    ++count_;
    entered_.notify_all();
    if (blocked_token_.has_value() && token == *blocked_token_) {
      release_.wait(lock, [this]() noexcept { return released_; });
    }
  }

  void Block(const StopControlWorkToken token) {
    std::lock_guard lock(mutex_);
    blocked_token_ = token;
    released_ = false;
  }

  void Release() {
    std::lock_guard lock(mutex_);
    released_ = true;
    release_.notify_all();
  }

  [[nodiscard]] bool WaitForCount(const std::size_t expected,
                                  const std::chrono::milliseconds timeout = 1000ms) {
    std::unique_lock lock(mutex_);
    return entered_.wait_for(lock, timeout,
                             [this, expected]() noexcept { return count_ >= expected; });
  }

  [[nodiscard]] std::size_t Count(const StopControlWorkToken token) const {
    std::lock_guard lock(mutex_);
    const auto retained = std::min(count_, records_.size());
    return static_cast<std::size_t>(std::count(
        records_.begin(), records_.begin() + static_cast<std::ptrdiff_t>(retained), token));
  }

  [[nodiscard]] std::size_t count() const {
    std::lock_guard lock(mutex_);
    return count_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable entered_;
  std::condition_variable release_;
  std::array<StopControlWorkToken, 512U> records_{};
  std::size_t count_{};
  std::optional<StopControlWorkToken> blocked_token_;
  bool released_{};
};

TEST(BoundedStopControlExecutorTest, ConstructionPublishesReadinessAndEnforcesFixedCapacity) {
  SteadyClock clock;
  RecordingReceiver receiver;
  EXPECT_THROW(BoundedStopControlExecutor(1U, clock, receiver), std::invalid_argument);
  EXPECT_THROW(BoundedStopControlExecutor(kMaximumStopControlWorkSlots + 1U, clock, receiver),
               std::invalid_argument);

  BoundedStopControlExecutor executor(2U, clock, receiver);
  EXPECT_EQ(executor.capacity(), 2U);
  EXPECT_TRUE(executor.prequalified_ready());
  executor.CloseIngress();
  EXPECT_FALSE(executor.prequalified_ready());
  EXPECT_TRUE(executor.DrainFor(100ms));
}

TEST(BoundedStopControlExecutorTest,
     ArmBeforePublishHandsOffOnceAndStaleGenerationCannotReplaceNewerWork) {
  SteadyClock clock;
  RecordingReceiver receiver;
  BoundedStopControlExecutor executor(3U, clock, receiver);
  const StopControlWorkToken blocker{0U, 1U};
  const StopControlWorkToken current{1U, 2U};
  receiver.Block(blocker);
  executor.Publish(blocker);
  ASSERT_TRUE(receiver.WaitForCount(1U));

  executor.ArmDeadline(current, clock.now() - 1);
  executor.Publish(current);
  executor.Publish(StopControlWorkToken{1U, 1U});
  for (std::size_t repeat = 0; repeat < 1000U; ++repeat) {
    executor.Publish(current);
  }
  receiver.Release();
  ASSERT_TRUE(receiver.WaitForCount(2U));
  executor.CloseIngress();
  ASSERT_TRUE(executor.DrainFor(1s));
  EXPECT_EQ(receiver.Count(current), 1U);
  EXPECT_EQ(receiver.Count(StopControlWorkToken{1U, 1U}), 0U);
}

TEST(BoundedStopControlExecutorTest, DeadlineRearmCoalescesAndCancellationPreventsFutureCallback) {
  SteadyClock clock;
  RecordingReceiver receiver;
  BoundedStopControlExecutor executor(2U, clock, receiver);
  const StopControlWorkToken token{0U, 1U};
  executor.Publish(token);
  ASSERT_TRUE(receiver.WaitForCount(1U));

  const auto deadline = clock.now() + 30'000'000;
  for (std::size_t repeat = 0; repeat < 100U; ++repeat) {
    executor.ArmDeadline(token, deadline);
  }
  ASSERT_TRUE(receiver.WaitForCount(2U));
  EXPECT_EQ(receiver.Count(token), 2U);

  executor.ArmDeadline(token, clock.now() + 50'000'000);
  executor.CancelDeadline(token);
  EXPECT_FALSE(receiver.WaitForCount(3U, 100ms));
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
  EXPECT_EQ(receiver.Count(token), 2U);
}

TEST(BoundedStopControlExecutorTest, DedicatedFullStopSlotProgressesAtMaximumCorrelationCapacity) {
  SteadyClock clock;
  RecordingReceiver receiver;
  BoundedStopControlExecutor executor(kMaximumStopControlWorkSlots, clock, receiver);
  const StopControlWorkToken full_stop{
      static_cast<std::uint32_t>(kMaximumStopControlWorkSlots - 1U), 7U};
  executor.Publish(full_stop);
  ASSERT_TRUE(receiver.WaitForCount(1U));
  executor.CloseIngress();
  ASSERT_TRUE(executor.DrainFor(1s));
  EXPECT_EQ(receiver.Count(full_stop), 1U);
}

TEST(BoundedStopControlExecutorTest,
     ConcurrentDisjointPublishersProgressWithoutOrdinaryQueueCapacity) {
  SteadyClock clock;
  RecordingReceiver receiver;
  constexpr std::size_t producer_count = 8U;
  BoundedStopControlExecutor executor(producer_count + 1U, clock, receiver);
  std::vector<std::jthread> producers;
  producers.reserve(producer_count);
  for (std::size_t index = 0; index < producer_count; ++index) {
    producers.emplace_back([&executor, index]() {
      executor.Publish(StopControlWorkToken{static_cast<std::uint32_t>(index), 1U});
    });
  }
  producers.clear();

  ASSERT_TRUE(receiver.WaitForCount(producer_count));
  executor.CloseIngress();
  ASSERT_TRUE(executor.DrainFor(1s));
  for (std::size_t index = 0; index < producer_count; ++index) {
    EXPECT_EQ(receiver.Count(StopControlWorkToken{static_cast<std::uint32_t>(index), 1U}), 1U);
  }
}

TEST(BoundedStopControlExecutorTest, PublishArmAndCancelAllocateNothingOnTheCallingThread) {
  SteadyClock clock;
  RecordingReceiver receiver;
  BoundedStopControlExecutor executor(2U, clock, receiver);
  const StopControlWorkToken token{0U, 1U};

  tracked_allocations = 0U;
  track_allocations = true;
  executor.ArmDeadline(token, clock.now() + 1'000'000'000);
  executor.Publish(token);
  for (std::size_t repeat = 0; repeat < 1000U; ++repeat) {
    executor.Publish(token);
  }
  executor.CancelDeadline(token);
  track_allocations = false;

  EXPECT_EQ(tracked_allocations, 0U);
  ASSERT_TRUE(receiver.WaitForCount(1U));
  executor.CloseIngress();
  EXPECT_TRUE(executor.DrainFor(1s));
}

TEST(BoundedStopControlExecutorTest, CloseAndDrainNeverOutliveAnInFlightReceiverCallback) {
  SteadyClock clock;
  RecordingReceiver receiver;
  const StopControlWorkToken token{0U, 1U};
  receiver.Block(token);
  auto executor = std::make_unique<BoundedStopControlExecutor>(2U, clock, receiver);
  executor->Publish(token);
  ASSERT_TRUE(receiver.WaitForCount(1U));
  executor->CloseIngress();
  EXPECT_FALSE(executor->DrainFor(10ms));

  auto destruction = std::async(std::launch::async, [&executor]() { executor.reset(); });
  EXPECT_EQ(destruction.wait_for(20ms), std::future_status::timeout);
  receiver.Release();
  EXPECT_EQ(destruction.wait_for(1s), std::future_status::ready);
}

TEST(BoundedStopControlExecutorTest,
     CloseRaceRetainsWorkThatEnteredBeforeTheFenceAndRejectsLaterIngress) {
  SteadyClock clock;
  RecordingReceiver receiver;
  const StopControlWorkToken blocker{0U, 1U};
  const StopControlWorkToken before_close{1U, 1U};
  const StopControlWorkToken after_close{2U, 1U};
  receiver.Block(blocker);
  BoundedStopControlExecutor executor(4U, clock, receiver);
  executor.Publish(blocker);
  ASSERT_TRUE(receiver.WaitForCount(1U));
  executor.Publish(before_close);
  executor.CloseIngress();
  executor.Publish(after_close);
  executor.ArmDeadline(after_close, clock.now());
  receiver.Release();

  ASSERT_TRUE(executor.DrainFor(1s));
  EXPECT_EQ(receiver.Count(before_close), 1U);
  EXPECT_EQ(receiver.Count(after_close), 0U);
}

}  // namespace
}  // namespace manyforge::core

// Reuse the real ROS action/publisher Rig in an isolated, timing-only binary.
#include "test_ros2_control_execution_backend.cpp"
#include <manyforge_ros2_control_execution/detail/leaf_timing.hpp>
#include <algorithm>
#include <condition_variable>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <thread>

namespace manyforge::ros2_control_execution {
namespace {
// A stuck SDK call cannot be bounded by the caller's poll loop. Abort this
// isolated test process on the exact stuck iteration so its own core retains
// every thread; never detach a worker or retry an uncertain effect.
class IterationWatchdog final {
 public:
  IterationWatchdog(const char* name, std::size_t iteration)
      : worker_([this, name, iteration] {
          std::unique_lock lock(mutex_);
          if (!condition_.wait_for(lock, std::chrono::seconds(10), [this] { return done_; })) {
            std::fprintf(stderr, "LEAF_WATCHDOG %s iteration=%zu\n", name, iteration);
            std::fflush(stderr);
            std::abort();
          }
        }) {}
  ~IterationWatchdog() {
    { std::lock_guard lock(mutex_); done_ = true; }
    condition_.notify_one();
    worker_.join();
  }
  IterationWatchdog(const IterationWatchdog&) = delete;
  IterationWatchdog& operator=(const IterationWatchdog&) = delete;
 private:
  std::mutex mutex_;
  std::condition_variable condition_;
  bool done_{false};
  std::thread worker_;
};
std::size_t TestIterations() {
  const auto* value = std::getenv("MANYFORGE_LEAF_ITERATIONS");
  if (!value) return 1000;
  char* end = nullptr;
  const auto parsed = std::strtoul(value, &end, 10);
  if (*value == '\0' || !end || *end != '\0' || parsed == 0 || parsed > 1000)
    throw std::invalid_argument("MANYFORGE_LEAF_ITERATIONS must be in 1..1000");
  return static_cast<std::size_t>(parsed);
}
void ReportEntity(detail::LeafEntity entity, const char* label, std::size_t minimum) {
  const auto count = detail::leaf_sample_count.load(std::memory_order_relaxed);
  ASSERT_LE(count, detail::kLeafSampleCapacity) << "timing buffer overflow";
  std::vector<std::int64_t> waits;
  std::vector<std::int64_t> holds;
  for (std::size_t i = 0; i < count; ++i) {
    const auto& sample = detail::leaf_samples[i];
    if (sample.entity != entity) continue;
    ASSERT_TRUE(sample.acquired) << label;
    if (std::getenv("MANYFORGE_LEAF_RAW"))
      std::cout << "LEAF_SAMPLE " << label << ' ' << sample.wait_ns << ' ' << sample.hold_ns << '\n';
    waits.push_back(sample.wait_ns);
    holds.push_back(sample.hold_ns);
  }
  ASSERT_GE(waits.size(), minimum) << label;
  std::sort(waits.begin(), waits.end());
  std::sort(holds.begin(), holds.end());
  const auto p99 = (waits.size() * 99 + 99) / 100 - 1;
  std::cout << "LEAF_TIMING " << label << " count=" << waits.size()
            << " wait_ns_p50=" << waits[waits.size() / 2]
            << " wait_ns_p99=" << waits[p99] << " wait_ns_max=" << waits.back()
            << " hold_ns_p50=" << holds[holds.size() / 2]
            << " hold_ns_p99=" << holds[p99] << " hold_ns_max=" << holds.back()
            << std::endl;
}
TEST(Od55LeafTiming, MotionActionAndScale) {
  using detail::LeafEntity;
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto kIterations = TestIterations();
  std::atomic<bool> scaling_failed{false};
  {
    Rig rig;
    std::jthread scaling([&] {
      for (std::uint64_t i = 0; i < kIterations * 4; ++i) {
        IterationWatchdog watchdog("scale_publish", i);
        const auto result = rig.adapter->SetScale(i % 2 ? 0.8 : 0.9);
        if (result.dispatch != ExecutionPortDispatch::kDispatched)
          scaling_failed.store(true);
        std::this_thread::sleep_for(std::chrono::microseconds(100));
      }
    });
    for (std::uint64_t i = 1; i <= kIterations; ++i) {
      IterationWatchdog watchdog("motion_action_scale", i);
      const auto snapshot = rig.adapter->SnapshotScaleAfterFence(
          std::chrono::steady_clock::now() + std::chrono::seconds(3));
      ASSERT_NE(snapshot.status, core::ScaleSnapshot::Status::kDeadlineExpired) << i;
      ASSERT_EQ(rig.submit(key(i)).dispatch, ExecutionPortDispatch::kDispatched);
      ASSERT_TRUE(rig.accepted(key(i)));
      ASSERT_EQ(rig.adapter->Cancel(key(i)).dispatch, ExecutionPortDispatch::kDispatched);
      ASSERT_TRUE(wait_until([&] {
        const auto goal = rig.adapter->ReadGoal(key(i));
        return goal && goal->cancel_ack_local_receipt_stamp.has_value();
      }));
      const auto handle = rig.handle(i - 1);
      ASSERT_TRUE(wait_until([&] { return handle->is_canceling(); }));
      handle->canceled(std::make_shared<Action::Result>());
      ASSERT_TRUE(wait_until([&] {
        const auto goal = rig.adapter->ReadGoal(key(i));
        return goal && goal->phase == ExecutionGoalPhase::kTerminal;
      }));
    }
    scaling.join();
  }  // Join executor and all callback producers before reading the buffer.
  EXPECT_FALSE(scaling_failed.load());
  for (const auto& [entity, label] : {
           std::pair{LeafEntity::kMotionGoal, "motion_goal"},
           std::pair{LeafEntity::kMotionCancel, "motion_cancel"},
           std::pair{LeafEntity::kMotionStopBarrier, "motion_stop_barrier"},
           std::pair{LeafEntity::kMotionScalePublish, "motion_scale_publish"},
           std::pair{LeafEntity::kMotionScaleSnapshot, "motion_scale_snapshot"}}) {
    ReportEntity(entity, label, kIterations);
  }
}

TEST(Od55LeafTiming, CancelBeforeAcceptanceIsSentExactlyOnce) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto kIterations = TestIterations();
  for (std::size_t i = 0; i < kIterations; ++i) {
    IterationWatchdog watchdog("late_accept_cancel", i);
    Rig rig;
    rig.delay_next_response = true;
    ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched) << i;
    ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); })) << i;
    ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kEffectUncertain) << i;
    ASSERT_EQ(rig.cancel_count.load(), 0U) << i;
    rig.release_response();
    ASSERT_TRUE(rig.accepted(key(1))) << i;
    ASSERT_TRUE(wait_until([&] {
      const auto goal = rig.adapter->ReadGoal(key(1));
      return goal && goal->cancel_ack_local_receipt_stamp.has_value();
    })) << i;
    ASSERT_EQ(rig.cancel_count.load(), 1U) << i;
  }
  ReportEntity(detail::LeafEntity::kMotionGoal, "late_accept_goal", kIterations);
  ReportEntity(detail::LeafEntity::kMotionAutomaticCancel, "late_accept_auto_cancel", kIterations);
}

TEST(Od55LeafTiming, ScaleSnapshotDeadlineExpiresWhilePublishHoldsLeaf) {
  Rig rig;
  detail::scale_publish_entered.store(false);
  detail::release_scale_publish.store(false);
  detail::block_scale_publish.store(true);
  std::atomic<bool> published{false};
  std::jthread publisher([&] {
    const auto result = rig.adapter->SetScale(0.7);
    published.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_scale_publish.store(true);
      detail::block_scale_publish.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::scale_publish_entered.load(); }));
  const auto timed = rig.adapter->SnapshotScaleAfterFence(
      std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
  EXPECT_EQ(timed.status, core::ScaleSnapshot::Status::kDeadlineExpired);
  detail::release_scale_publish.store(true);
  publisher.join();
  ASSERT_TRUE(published.load());
  const auto after = rig.adapter->SnapshotScaleAfterFence(
      std::chrono::steady_clock::now() + std::chrono::seconds(3));
  ASSERT_EQ(after.status, core::ScaleSnapshot::Status::kFactor);
  EXPECT_DOUBLE_EQ(after.factor, 0.7);
}

TEST(Od55LeafTiming, CancelStampUsesClockAfterLeafSendReturns) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  detail::motion_cancel_entered.store(false);
  detail::release_motion_cancel.store(false);
  detail::block_motion_cancel.store(true);
  std::atomic<bool> sent{false};
  std::jthread cancel([&] {
    const auto result = rig.adapter->Cancel(
        key(1), std::chrono::steady_clock::now() + std::chrono::seconds(3));
    sent.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_motion_cancel.store(true);
      detail::block_motion_cancel.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::motion_cancel_entered.load(); }));
  ASSERT_TRUE(rig.adapter->ReadGoal(key(1)));
  EXPECT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  rig.clock.set(84);
  detail::release_motion_cancel.store(true);
  cancel.join();
  ASSERT_TRUE(sent.load());
  ASSERT_TRUE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  EXPECT_EQ(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp, TimePoint{84});
}

void ExerciseFencedUnsentGoal() {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  detail::motion_goal_entered.store(false);
  detail::release_motion_goal.store(false);
  detail::block_motion_goal.store(true);
  std::atomic<bool> dispatched{false};
  std::jthread submit([&] {
    const auto result = rig.adapter->Submit(key(1), trajectory());
    dispatched.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_motion_goal.store(true);
      detail::block_motion_goal.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::motion_goal_entered.load(); }));
  ASSERT_FALSE(rig.adapter->ReadGoalActivity().expected_end);
  const auto stopped = rig.adapter->Cancel(
      key(1), std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
  ASSERT_EQ(stopped.dispatch, ExecutionPortDispatch::kEffectUncertain);
  ASSERT_TRUE(rig.adapter->ReadGoal(key(1)));
  ASSERT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  ASSERT_EQ(rig.cancel_count.load(), 0U);
  detail::release_motion_goal.store(true);
  submit.join();
  ASSERT_FALSE(dispatched.load());
  ASSERT_EQ(rig.goal_count(), 0U);
  ASSERT_EQ(rig.cancel_count.load(), 0U);
}

void ExerciseAdmittedGoalLateCancel() {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return rig.adapter->ActionServerIsReady(); }));
  detail::goal_admitted_entered.store(false);
  detail::release_goal_admitted.store(false);
  detail::block_goal_admitted.store(true);
  std::atomic<bool> dispatched{false};
  std::jthread submit([&] {
    const auto result = rig.adapter->Submit(key(1), trajectory());
    dispatched.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_goal_admitted.store(true);
      detail::block_goal_admitted.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::goal_admitted_entered.load(); }));
  ASSERT_FALSE(rig.adapter->ReadGoalActivity().expected_end);
  const auto stopped = rig.adapter->Cancel(
      key(1), std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
  ASSERT_EQ(stopped.dispatch, ExecutionPortDispatch::kEffectUncertain);
  ASSERT_TRUE(rig.adapter->ReadGoal(key(1)));
  ASSERT_FALSE(rig.adapter->ReadGoal(key(1))->cancel_sent_local_stamp);
  ASSERT_EQ(rig.cancel_count.load(), 0U);
  detail::release_goal_admitted.store(true);
  submit.join();
  ASSERT_TRUE(dispatched.load());
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_TRUE(wait_until([&] { return rig.cancel_count.load() == 1U; }));
  ASSERT_EQ(rig.cancel_count.load(), 1U);
}

TEST(Od55LeafTiming, StopDeadlineDoesNotInventCancelWhileGoalSendIsHeld) {
  ExerciseFencedUnsentGoal();
}
TEST(Od55LeafTiming, AdmittedGoalIsCancelledOnceAfterStopDeadline) {
  ExerciseAdmittedGoalLateCancel();
}
TEST(Od55LeafTiming, FencedUnsentGoalRace1000) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto iterations = TestIterations();
  for (std::size_t i = 0; i < iterations; ++i) {
    IterationWatchdog watchdog("fenced_unsent_goal", i);
    SCOPED_TRACE(i);
    ExerciseFencedUnsentGoal();
    if (::testing::Test::HasFatalFailure()) return;
  }
  ReportEntity(detail::LeafEntity::kMotionGoal, "fenced_unsent_goal", iterations);
}
TEST(Od55LeafTiming, AdmittedGoalLateCancelRace1000) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto iterations = TestIterations();
  for (std::size_t i = 0; i < iterations; ++i) {
    IterationWatchdog watchdog("admitted_goal_cancel", i);
    SCOPED_TRACE(i);
    ExerciseAdmittedGoalLateCancel();
    if (::testing::Test::HasFatalFailure()) return;
  }
  ReportEntity(detail::LeafEntity::kMotionGoal, "admitted_goal", iterations);
  ReportEntity(detail::LeafEntity::kMotionAutomaticCancel, "admitted_auto_cancel", iterations);
}

void ExerciseReplaceStopRace(bool admitted) {
  Rig rig;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(rig.accepted(key(1)));
  const auto first = rig.handle(0)->get_goal_id();
  detail::motion_goal_entered.store(false);
  detail::release_motion_goal.store(false);
  detail::goal_admitted_entered.store(false);
  detail::release_goal_admitted.store(false);
  detail::block_motion_goal.store(!admitted);
  detail::block_goal_admitted.store(admitted);
  std::atomic<bool> dispatched{false};
  std::jthread replacement([&] {
    const auto result = rig.adapter->Replace(key(1), key(2), trajectory());
    dispatched.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_motion_goal.store(true);
      detail::release_goal_admitted.store(true);
      detail::block_motion_goal.store(false);
      detail::block_goal_admitted.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([&] {
    return admitted ? detail::goal_admitted_entered.load()
                    : detail::motion_goal_entered.load();
  }));
  const auto stopped = rig.adapter->Cancel(
      key(1), std::chrono::steady_clock::now() + std::chrono::milliseconds(25));
  ASSERT_EQ(stopped.dispatch, ExecutionPortDispatch::kEffectUncertain);
  ASSERT_EQ(rig.cancel_count.load(), 0U);
  detail::release_motion_goal.store(true);
  detail::release_goal_admitted.store(true);
  replacement.join();
  if (!admitted) {
    ASSERT_FALSE(dispatched.load());
    ASSERT_EQ(rig.goal_count(), 1U);
    ASSERT_EQ(rig.cancel_count.load(), 0U);
    return;
  }
  ASSERT_TRUE(dispatched.load());
  ASSERT_TRUE(rig.accepted(key(2)));
  ASSERT_TRUE(rig.adapter->ReadGoal(key(2)));
  ASSERT_FALSE(rig.adapter->ReadGoal(key(2))->cancel_sent_local_stamp);
  const auto cancelled = rig.cancel_ids();
  ASSERT_LE(cancelled.size(), 1U);
  for (const auto& id : cancelled) ASSERT_EQ(id, first);
  ASSERT_LE(std::count(cancelled.begin(), cancelled.end(), first), 1);
}

TEST(Od55LeafTiming, ReplaceBeforeAdmissionStopRace1000) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto iterations = TestIterations();
  for (std::size_t i = 0; i < iterations; ++i) {
    IterationWatchdog watchdog("replace_before_admission", i);
    SCOPED_TRACE(i);
    ExerciseReplaceStopRace(false);
    if (::testing::Test::HasFatalFailure()) return;
  }
  ReportEntity(detail::LeafEntity::kMotionGoal, "replace_fenced_goal", iterations);
}
TEST(Od55LeafTiming, ReplaceAfterAdmissionStopRace1000) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto iterations = TestIterations();
  for (std::size_t i = 0; i < iterations; ++i) {
    IterationWatchdog watchdog("replace_after_admission", i);
    SCOPED_TRACE(i);
    ExerciseReplaceStopRace(true);
    if (::testing::Test::HasFatalFailure()) return;
  }
  ReportEntity(detail::LeafEntity::kMotionGoal, "replace_admitted_goal", iterations);
}

void ExerciseLateAcceptanceWhileBarrierHeld() {
  Rig rig;
  ASSERT_TRUE(wait_until([&] { return JtcExecutionPortTestPeer::executor_is_spinning(*rig.epoch_resources); }));
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  detail::stop_barrier_entered.store(false);
  detail::stop_barrier_expired.store(false);
  detail::release_stop_barrier.store(false);
  detail::block_stop_barrier.store(true);
  std::jthread stop([&] {
    (void)rig.adapter->Cancel(key(1),
        std::chrono::steady_clock::now() + std::chrono::seconds(5));
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_stop_barrier.store(true);
      detail::block_stop_barrier.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::stop_barrier_entered.load(); }));
  rig.release_response();
  // Acceptance must be processed on the time-critical thread while the action
  // leaf is held; the queued cancel may wait, but the ROS callback must not.
  const auto callback_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  bool accepted = false;
  while (std::chrono::steady_clock::now() < callback_deadline) {
    const auto goal = rig.adapter->ReadGoal(key(1));
    if (goal && goal->accepted_stamp_ns.has_value()) { accepted = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  ASSERT_TRUE(accepted);
  ASSERT_TRUE(rig.state_arrives_within(1, std::chrono::seconds(1)))
      << "time-critical thread could not run another callback";
  ASSERT_FALSE(detail::stop_barrier_expired.load());
  ASSERT_FALSE(detail::release_stop_barrier.load());
  ASSERT_EQ(rig.cancel_count.load(), 0U);
  const auto first = rig.handle(0)->get_goal_id();
  detail::release_stop_barrier.store(true);
  stop.join();
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->cancel_ack_local_receipt_stamp.has_value();
  }));
  ASSERT_EQ(rig.cancel_count.load(), 1U);
  ASSERT_EQ(rig.cancel_ids(), (std::vector<rclcpp_action::GoalUUID>{first}));
}

TEST(Od55LeafTiming, LateAcceptanceReturnsWhileStopBarrierHeld) {
  ExerciseLateAcceptanceWhileBarrierHeld();
}
TEST(Od55LeafTiming, LateAcceptanceReturnsWhileStopBarrierHeld1000) {
  detail::leaf_sample_count.store(0, std::memory_order_relaxed);
  const auto iterations = TestIterations();
  for (std::size_t i = 0; i < iterations; ++i) {
    IterationWatchdog watchdog("late_accept_barrier", i);
    SCOPED_TRACE(i);
    ExerciseLateAcceptanceWhileBarrierHeld();
    if (::testing::Test::HasFatalFailure()) return;
  }
  ReportEntity(detail::LeafEntity::kMotionStopBarrier, "late_accept_barrier", iterations);
}

// OD-111 / DX-4018: k1's acceptance, with a retained cancel, arrives while
// Submit(k2) holds the action leaf. The goal-response callback returns at once
// on the time-critical thread; the cancel waits on the StopControl lane and
// is sent once, for k1, after the leaf is released.
TEST(Od55LeafTiming, LateAcceptanceReturnsWhileGoalSendHoldsLeaf) {
  IterationWatchdog watchdog("late_accept_goal_leaf", 0);
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kEffectUncertain);
  detail::motion_goal_entered.store(false);
  detail::release_motion_goal.store(false);
  detail::block_motion_goal.store(true);
  std::atomic<bool> dispatched{false};
  std::jthread submit([&] {
    const auto result = rig.adapter->Submit(key(2), trajectory());
    dispatched.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_motion_goal.store(true);
      detail::block_motion_goal.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::motion_goal_entered.load(); }));
  rig.release_response();
  ASSERT_TRUE(rig.accepted(key(1)));
  ASSERT_TRUE(rig.state_arrives_within(1, std::chrono::seconds(1)))
      << "goal-response callback did not return while the leaf was held";
  ASSERT_FALSE(detail::release_motion_goal.load());
  ASSERT_EQ(rig.cancel_count.load(), 0U);
  const auto first = rig.handle(0)->get_goal_id();
  detail::release_motion_goal.store(true);
  submit.join();
  ASSERT_TRUE(dispatched.load());
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->cancel_ack_local_receipt_stamp.has_value();
  }));
  ASSERT_TRUE(rig.accepted(key(2)));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_EQ(rig.cancel_ids(), (std::vector<rclcpp_action::GoalUUID>{first}));
  EXPECT_FALSE(rig.adapter->ReadGoalActivity().late_cancel_not_issued);
}

// The confirmed cycle at the backend interface: Submit(k2) is admitted and
// holds the leaf just before its SDK send while k1's acceptance (retained
// cancel) is delivered; then k2's send enters rclcpp. Before OD-111 the
// callback held rclcpp's goal-request lock waiting for the leaf, and k2's
// send waited for that lock. Now neither hangs: k2 is sent and k1 is
// cancelled exactly once.
TEST(Od55LeafTiming, LateAcceptanceDuringAdmittedGoalSendDoesNotDeadlock) {
  IterationWatchdog watchdog("late_accept_admitted_send", 0);
  Rig rig;
  rig.delay_next_response = true;
  ASSERT_EQ(rig.submit(key(1)).dispatch, ExecutionPortDispatch::kDispatched);
  ASSERT_TRUE(wait_until([&] { return rig.response_blocked.load(); }));
  ASSERT_EQ(rig.adapter->Cancel(key(1)).dispatch, ExecutionPortDispatch::kEffectUncertain);
  detail::goal_admitted_entered.store(false);
  detail::goal_admitted_timed_out.store(false);
  detail::release_goal_admitted.store(false);
  detail::block_goal_admitted.store(true);
  std::atomic<bool> dispatched{false};
  std::jthread submit([&] {
    const auto result = rig.adapter->Submit(key(2), trajectory());
    dispatched.store(result.dispatch == ExecutionPortDispatch::kDispatched);
  });
  struct ReleaseOnExit {
    ~ReleaseOnExit() {
      detail::release_goal_admitted.store(true);
      detail::block_goal_admitted.store(false);
    }
  } release;
  ASSERT_TRUE(wait_until([] { return detail::goal_admitted_entered.load(); }));
  rig.release_response();
  ASSERT_TRUE(rig.accepted(key(1)));
  // Let a callback that would wait for the leaf reach that wait.
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  const auto first = rig.handle(0)->get_goal_id();
  detail::release_goal_admitted.store(true);
  submit.join();
  ASSERT_FALSE(detail::goal_admitted_timed_out.load());
  ASSERT_TRUE(dispatched.load());
  ASSERT_TRUE(rig.accepted(key(2)));
  ASSERT_TRUE(wait_until([&] {
    const auto goal = rig.adapter->ReadGoal(key(1));
    return goal && goal->cancel_ack_local_receipt_stamp.has_value();
  }));
  EXPECT_EQ(rig.cancel_count.load(), 1U);
  EXPECT_EQ(rig.cancel_ids(), (std::vector<rclcpp_action::GoalUUID>{first}));
}
}  // namespace
}  // namespace manyforge::ros2_control_execution

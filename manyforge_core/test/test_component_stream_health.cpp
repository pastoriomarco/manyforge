#include <gtest/gtest.h>

#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::int64_t kMs = 1'000'000;

class ComponentStreamHealthTest : public ::testing::Test {
 protected:
  ComponentStreamHealthTest() {
    clock_.set(1'000 * kMs);
    token_ = health_.SetListener([this](const ControllableComponentId& component,
                                        const ComponentStreamVerdictDTO& verdict) {
      verdicts_[component.value()] = verdict;
      ++calls_;
    });
  }

  /// Runs every due Monitoring callback at the current time.
  void Run() { scheduler_.advance_to(clock_.now(), SchedulerPriority::Monitoring); }
  void Advance(std::int64_t ns) {
    clock_.advance(ns);
    Run();
  }

  std::unique_ptr<StreamRecorder> Declare(std::string component, StreamDeclarationDTO declaration) {
    auto declared = health_.Declare(ControllableComponentId(std::move(component)), std::move(declaration));
    EXPECT_TRUE(declared.ok());
    Run();
    return std::move(declared.value());
  }

  StreamHealthDTO Row(const std::string& component, std::size_t index = 0U) {
    const auto rows = health_.Read(ControllableComponentId(component));
    EXPECT_GT(rows.size(), index);
    return rows.at(index);
  }

  FakeClock clock_;
  ManualScheduler scheduler_;
  ComponentStreamHealth health_{clock_, scheduler_};
  std::uint64_t token_{};
  std::map<std::string, ComponentStreamVerdictDTO> verdicts_;
  int calls_{};
};

StreamDeclarationDTO Stream(std::string id, StreamSeverityDTO severity, StreamRecoveryDTO recovery,
                            StreamTimestampSourceDTO source = StreamTimestampSourceDTO::kMessageStamp) {
  return {std::move(id), 100 * kMs, severity, recovery, source};
}

TEST_F(ComponentStreamHealthTest, MessageStampMustAdvanceWithinBudget) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kBlocking,
                                        StreamRecoveryDTO::kAuto));
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kAbsent);
  EXPECT_FALSE(Row("arm").age_ns);
  recorder->Record(10);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_TRUE(verdicts_["arm"].empty());
  // Samples keep arriving with a frozen stamp (a paused simulator).
  for (int i = 0; i < 5; ++i) {
    Advance(30 * kMs);
    recorder->Record(10);
  }
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(Row("arm").error);
  EXPECT_EQ(Row("arm").error->code, kComponentStreamStaleCode);
  ASSERT_TRUE(verdicts_["arm"].motion_hold);
  EXPECT_EQ(verdicts_["arm"].motion_hold->details.at("reason"), kComponentStreamStaleCode);
  EXPECT_EQ(verdicts_["arm"].motion_hold->details.at("stream_id"), "measured_joints");
  EXPECT_FALSE(verdicts_["arm"].fault_loss);
  // Advancing again: `auto` clears by itself.
  recorder->Record(11);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_TRUE(verdicts_["arm"].empty());
}

TEST_F(ComponentStreamHealthTest, ASampleExactlyOneBudgetOldIsExpired) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kBlocking,
                                        StreamRecoveryDTO::kAuto));
  recorder->Record(10);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  Advance(99 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  // The deadline equals `now`: expired, not re-queued at `now` (that looped forever).
  Advance(1 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(verdicts_["arm"].motion_hold);
}

TEST_F(ComponentStreamHealthTest, AStampStepBackIsASampleAnEqualStampIsNot) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kBlocking,
                                        StreamRecoveryDTO::kAuto));
  recorder->Record(5'000 * kMs);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  // A simulator reset: sim time restarts near zero and keeps advancing.
  for (int i = 1; i <= 5; ++i) {
    Advance(60 * kMs);
    recorder->Record(i * kMs);
  }
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_TRUE(verdicts_["arm"].empty());
  // Paused after the reset: the same stamp repeats and the stream goes stale.
  for (int i = 0; i < 4; ++i) {
    Advance(30 * kMs);
    recorder->Record(5 * kMs);
  }
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(verdicts_["arm"].motion_hold);
}

TEST_F(ComponentStreamHealthTest, ArrivalStreamsAdvanceOnEverySample) {
  auto recorder = Declare("io", Stream("ready", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAuto,
                                       StreamTimestampSourceDTO::kArrival));
  for (int i = 0; i < 10; ++i) {
    recorder->Record(1);  // stamp ignored
    Advance(60 * kMs);
  }
  EXPECT_EQ(Row("io").state, StreamLivenessStateDTO::kLive);
  Advance(60 * kMs);
  EXPECT_EQ(Row("io").state, StreamLivenessStateDTO::kStale);
}

TEST_F(ComponentStreamHealthTest, AbsentIsClassifiedAfterOneBudget) {
  auto fault = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kFault, StreamRecoveryDTO::kAck));
  Advance(99 * kMs);
  EXPECT_TRUE(verdicts_["arm"].empty());
  EXPECT_FALSE(Row("arm").error);
  Advance(1 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kAbsent);
  ASSERT_TRUE(verdicts_["arm"].fault_loss);
  EXPECT_EQ(verdicts_["arm"].fault_loss->code, kComponentStreamAbsentCode);
  ASSERT_TRUE(verdicts_["arm"].motion_hold);
  // Startup absence never latches, even with `ack`.
  fault->Record(5);
  Run();
  EXPECT_TRUE(verdicts_["arm"].empty());
  EXPECT_FALSE(Row("arm").acknowledgement_required);
}

TEST_F(ComponentStreamHealthTest, SeverityDecidesTheVerdict) {
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault, StreamRecoveryDTO::kAck));
  auto blocking = Declare("b", Stream("s", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAuto));
  auto warning = Declare("c", Stream("s", StreamSeverityDTO::kWarning, StreamRecoveryDTO::kAuto));
  fault->Record(1);
  blocking->Record(1);
  warning->Record(1);
  Run();
  Advance(101 * kMs);
  EXPECT_TRUE(verdicts_["a"].fault_loss);
  EXPECT_TRUE(verdicts_["a"].motion_hold);
  EXPECT_FALSE(verdicts_["b"].fault_loss);
  EXPECT_TRUE(verdicts_["b"].motion_hold);
  EXPECT_FALSE(verdicts_.contains("c"));
  EXPECT_EQ(Row("c").state, StreamLivenessStateDTO::kStale);
  // A `fault` stream's latch is its incident (the driver's): fresh again
  // clears the stream condition itself.
  EXPECT_EQ(Row("a").recovery, StreamRecoveryDTO::kAck);
  fault->Record(2);
  Run();
  EXPECT_TRUE(verdicts_["a"].empty());
}

TEST_F(ComponentStreamHealthTest, FaultWithAutoRecoveryReportsAuto) {
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault, StreamRecoveryDTO::kAuto));
  fault->Record(1);
  Run();
  Advance(101 * kMs);
  EXPECT_TRUE(verdicts_["a"].fault_loss);
  EXPECT_EQ(Row("a").recovery, StreamRecoveryDTO::kAuto);
  EXPECT_FALSE(Row("a").acknowledgement_required);
  fault->Record(2);
  Run();
  EXPECT_TRUE(verdicts_["a"].empty());
  EXPECT_EQ(Row("a").state, StreamLivenessStateDTO::kLive);
}

TEST_F(ComponentStreamHealthTest, AStaleStreamIsReportedOnceUntilLiveAgain) {
  std::vector<std::string> lines;
  health_.SetLog([&](const std::string& line) { lines.push_back(line); });
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault, StreamRecoveryDTO::kAuto));
  fault->Record(1);
  Run();
  const auto calls_live = calls_;
  const auto lines_live = lines.size();
  Advance(101 * kMs);
  ASSERT_TRUE(verdicts_["a"].fault_loss);
  EXPECT_EQ(calls_, calls_live + 1);
  EXPECT_EQ(lines.size(), lines_live + 1U);
  // Many budgets later, still stale: no further verdict, log line or deadline.
  for (int i = 0; i < 10; ++i) Advance(100 * kMs);
  EXPECT_EQ(calls_, calls_live + 1);
  EXPECT_EQ(lines.size(), lines_live + 1U);
  // Live again clears it once; the next break is one new report.
  fault->Record(2);
  Run();
  EXPECT_TRUE(verdicts_["a"].empty());
  EXPECT_EQ(calls_, calls_live + 2);
  Advance(101 * kMs);
  EXPECT_TRUE(verdicts_["a"].fault_loss);
  EXPECT_EQ(calls_, calls_live + 3);
  EXPECT_EQ(lines.size(), lines_live + 3U);
  health_.SetLog({});
}

TEST_F(ComponentStreamHealthTest, AckLatchesUntilTheOperatorsRecover) {
  auto recorder = Declare("arm", Stream("s", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAck));
  recorder->Record(1);
  Run();
  Advance(101 * kMs);
  recorder->Record(2);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_TRUE(Row("arm").acknowledgement_required);
  ASSERT_TRUE(verdicts_["arm"].motion_hold);
  EXPECT_EQ(verdicts_["arm"].motion_hold->details.at("reason"),
            kComponentStreamAcknowledgementRequiredCode);
  health_.Acknowledge(ControllableComponentId("arm"));
  Run();
  EXPECT_TRUE(verdicts_["arm"].empty());
  EXPECT_FALSE(Row("arm").acknowledgement_required);
}

TEST_F(ComponentStreamHealthTest, FaultEventsArePushedAtOnceAndKeptUntilRecover) {
  auto recorder = Declare("arm", Stream("s", StreamSeverityDTO::kWarning, StreamRecoveryDTO::kAuto));
  recorder->Record(1);
  Run();
  recorder->ReportFault("xarm_joint_value_invalid", "joint 3 is exactly 0");
  recorder->ReportFault("second_event", "ignored until Recover");
  Run();
  ASSERT_TRUE(verdicts_["arm"].fault_event);
  EXPECT_EQ(verdicts_["arm"].fault_event->code, "xarm_joint_value_invalid");
  EXPECT_EQ(verdicts_["arm"].fault_event->details.at("stream_id"), "s");
  EXPECT_EQ(Row("arm").error->code, "xarm_joint_value_invalid");
  recorder->ReportFault("Not A Code", std::string(400U, 'x'));
  health_.Acknowledge(ControllableComponentId("arm"));
  Run();
  EXPECT_TRUE(verdicts_["arm"].empty());
  recorder->ReportFault("Not A Code", std::string(400U, 'x'));
  Run();
  ASSERT_TRUE(verdicts_["arm"].fault_event);
  EXPECT_EQ(verdicts_["arm"].fault_event->code, "component_stream_fault");
  EXPECT_EQ(verdicts_["arm"].fault_event->message.size(), kMaxStreamFaultDetailBytes);
}

TEST_F(ComponentStreamHealthTest, DeploymentOverridesReplaceDefaults) {
  auto recorder = Declare("map", Stream("map_updates", StreamSeverityDTO::kWarning, StreamRecoveryDTO::kAuto));
  recorder->Record(1);
  Run();
  ASSERT_TRUE(health_
                  .SetOverrides({{ControllableComponentId("map"), "map_updates", 300 * kMs,
                                  StreamSeverityDTO::kFault, std::nullopt}})
                  .ok());
  Run();
  EXPECT_EQ(Row("map").severity, StreamSeverityDTO::kFault);
  EXPECT_EQ(Row("map").budget_ns, 300 * kMs);
  Advance(250 * kMs);
  EXPECT_EQ(Row("map").state, StreamLivenessStateDTO::kLive);
  Advance(60 * kMs);
  EXPECT_TRUE(verdicts_["map"].fault_loss);
  // Invalid tables change nothing.
  EXPECT_FALSE(health_.SetOverrides({{ControllableComponentId("map"), "map_updates", 0, {}, {}}}).ok());
  EXPECT_FALSE(health_
                   .SetOverrides({{ControllableComponentId("map"), "x", {}, StreamSeverityDTO::kFault, {}},
                                  {ControllableComponentId("map"), "x", 1, {}, {}}})
                   .ok());
  EXPECT_EQ(Row("map").severity, StreamSeverityDTO::kFault);
  // Removing the override restores the default (a warning: no verdict).
  ASSERT_TRUE(health_.SetOverrides({}).ok());
  Run();
  EXPECT_TRUE(verdicts_["map"].empty());
  EXPECT_EQ(Row("map").budget_ns, 100 * kMs);
}

TEST_F(ComponentStreamHealthTest, OneTimerAndNoneWithoutLiveStreams) {
  std::vector<std::unique_ptr<StreamRecorder>> recorders;
  for (int i = 0; i < 20; ++i) {
    recorders.push_back(Declare("arm", Stream("s" + std::to_string(i), StreamSeverityDTO::kWarning,
                                              StreamRecoveryDTO::kAuto)));
    recorders.back()->Record(1);
  }
  Run();
  for (int i = 0; i < 5; ++i) {
    Advance(50 * kMs);
    for (auto& recorder : recorders) recorder->Record(i + 2);
    Run();
    EXPECT_LE(scheduler_.pending_count(SchedulerPriority::Monitoring), 1U);
  }
  Advance(200 * kMs);
  EXPECT_EQ(scheduler_.pending_count(SchedulerPriority::Monitoring), 0U);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
}

TEST_F(ComponentStreamHealthTest, RedeclarationReplacesAndDestructionUndeclares) {
  auto first = Declare("arm", Stream("s", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAuto));
  Advance(100 * kMs);
  EXPECT_TRUE(verdicts_["arm"].motion_hold);
  auto second = Declare("arm", Stream("s", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAuto));
  EXPECT_TRUE(verdicts_["arm"].empty());  // fresh grace for the new adapter
  first.reset();                          // the old handle does not undeclare the new one
  Run();
  EXPECT_EQ(health_.Read(ControllableComponentId("arm")).size(), 1U);
  EXPECT_TRUE(health_.RecordDeclared(ControllableComponentId("arm"), "s", 7));
  EXPECT_FALSE(health_.RecordDeclared(ControllableComponentId("arm"), "unknown", 7));
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  second.reset();
  Run();
  EXPECT_TRUE(health_.Read(ControllableComponentId("arm")).empty());
  EXPECT_FALSE(health_.Declare(ControllableComponentId("arm"), {"Bad Id", 1}).ok());
  EXPECT_FALSE(health_.Declare(ControllableComponentId("arm"), {"ok", 0}).ok());
}

TEST_F(ComponentStreamHealthTest, NewListenerReceivesCurrentVerdicts) {
  auto recorder = Declare("arm", Stream("s", StreamSeverityDTO::kBlocking, StreamRecoveryDTO::kAuto));
  Advance(100 * kMs);
  std::map<std::string, ComponentStreamVerdictDTO> replayed;
  const auto token = health_.SetListener(
      [&](const ControllableComponentId& component, const ComponentStreamVerdictDTO& verdict) {
        replayed[component.value()] = verdict;
      });
  Run();
  EXPECT_TRUE(replayed["arm"].motion_hold);
  health_.ClearListener(token_);  // stale token: no effect
  health_.ClearListener(token);
  recorder->Record(1);
  Run();
  EXPECT_TRUE(replayed["arm"].motion_hold);  // no listener any more
  EXPECT_TRUE(health_.Verdict(ControllableComponentId("arm")).empty());
}

}  // namespace
}  // namespace manyforge::core

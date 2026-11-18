#include <gtest/gtest.h>

#include <manyforge_core/component_stream_health.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::int64_t kMs = 1'000'000;

class ComponentStreamHealthTest : public ::testing::Test {
 protected:
  ComponentStreamHealthTest() {
    clock_->set(1'000 * kMs);
    // One line per verdict change:
    // "INFO manyforge component stream health: component=<id> condition=<message|none>\n".
    health_.SetLog([this](const std::string& line) {
      const auto component_at = line.find("component=") + 10U;
      const auto condition_at = line.find(" condition=", component_at);
      const auto shown = line.substr(condition_at + 11U, line.size() - condition_at - 12U);
      conditions_[line.substr(component_at, condition_at - component_at)] =
          shown == "none" ? std::nullopt : std::optional<std::string>(shown);
      ++calls_;
    });
  }

  /// Runs every due Monitoring callback at the current time.
  void Run() { scheduler_.advance_to(clock_->now(), SchedulerPriority::Monitoring); }
  void Advance(std::int64_t ns) {
    clock_->advance(ns);
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

  std::shared_ptr<FakeClock> clock_{std::make_shared<FakeClock>()};
  ManualScheduler scheduler_;
  ComponentStreamHealth health_{clock_, scheduler_};
  /// Last logged condition per component; nullopt = none.
  std::map<std::string, std::optional<std::string>> conditions_;
  int calls_{};
};

StreamDeclarationDTO Stream(std::string id, StreamSeverityDTO severity,
                            StreamTimestampSourceDTO source = StreamTimestampSourceDTO::kMessageStamp) {
  return {std::move(id), 100 * kMs, severity, source};
}

TEST(ComponentStreamHealthLifetime, DetachedRecorderMayOutliveSchedulerAndClock) {
  auto clock = std::make_shared<FakeClock>();
  auto scheduler = std::make_unique<ManualScheduler>();
  auto health = std::make_unique<ComponentStreamHealth>(clock, *scheduler);
  auto declared = health->Declare(
      ControllableComponentId("arm"),
      Stream("measured_joints", StreamSeverityDTO::kFault));
  ASSERT_TRUE(declared.ok());
  auto recorder = std::move(declared.value());
  recorder->RecordArrival();
  health->DetachScheduler();
  scheduler.reset();
  clock.reset();
  EXPECT_NO_THROW(recorder->RecordArrival());
  EXPECT_NO_THROW(static_cast<void>(health->Read(ControllableComponentId("arm"))));
  EXPECT_NO_THROW(health.reset());
  EXPECT_NO_THROW(recorder.reset());
}

TEST_F(ComponentStreamHealthTest, MessageStampMustAdvanceWithinBudget) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kFault));
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kAbsent);
  EXPECT_FALSE(Row("arm").age_ns);
  recorder->Record(10);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_FALSE(conditions_["arm"]);
  // Samples keep arriving with a frozen stamp (a paused simulator).
  for (int i = 0; i < 5; ++i) {
    Advance(30 * kMs);
    recorder->Record(10);
  }
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(Row("arm").error);
  EXPECT_EQ(Row("arm").error->code, kComponentStreamStaleCode);
  EXPECT_EQ(conditions_["arm"], "Component arm stream measured_joints is stale (budget 100 ms).");
  // Advancing again clears it by itself.
  recorder->Record(11);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  EXPECT_FALSE(conditions_["arm"]);
}

TEST_F(ComponentStreamHealthTest, ASampleExactlyOneBudgetOldIsExpired) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kFault));
  recorder->Record(10);
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  Advance(99 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kLive);
  // The deadline equals `now`: expired, not re-queued at `now` (that looped forever).
  Advance(1 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(conditions_["arm"]);
}

TEST_F(ComponentStreamHealthTest, AStampStepBackIsASampleAnEqualStampIsNot) {
  auto recorder = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kFault));
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
  EXPECT_FALSE(conditions_["arm"]);
  // Paused after the reset: the same stamp repeats and the stream goes stale.
  for (int i = 0; i < 4; ++i) {
    Advance(30 * kMs);
    recorder->Record(5 * kMs);
  }
  Run();
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kStale);
  ASSERT_TRUE(conditions_["arm"]);
}

TEST_F(ComponentStreamHealthTest, ArrivalStreamsAdvanceOnEverySample) {
  auto recorder = Declare("io", Stream("ready", StreamSeverityDTO::kFault,
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
  auto fault = Declare("arm", Stream("measured_joints", StreamSeverityDTO::kFault));
  Advance(99 * kMs);
  EXPECT_FALSE(conditions_["arm"]);
  EXPECT_FALSE(Row("arm").error);
  Advance(1 * kMs);
  EXPECT_EQ(Row("arm").state, StreamLivenessStateDTO::kAbsent);
  ASSERT_TRUE(Row("arm").error);
  EXPECT_EQ(Row("arm").error->code, kComponentStreamAbsentCode);
  EXPECT_EQ(conditions_["arm"], "Component arm stream measured_joints has no sample (budget 100 ms).");
  // The first sample clears it.
  fault->Record(5);
  Run();
  EXPECT_FALSE(conditions_["arm"]);
}

TEST_F(ComponentStreamHealthTest, AFirstSampleClassifiedAfterItsBudgetIsStale) {
  // The Monitoring lane runs late: the only sample is past its budget when classified.
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault));
  auto warning = Declare("c", Stream("s", StreamSeverityDTO::kWarning));
  fault->Record(1);
  warning->Record(1);
  Advance(100 * kMs);
  EXPECT_EQ(Row("a").state, StreamLivenessStateDTO::kStale);
  EXPECT_EQ(conditions_["a"], "Component a stream s is stale (budget 100 ms).");
  EXPECT_EQ(Row("c").state, StreamLivenessStateDTO::kStale);
  EXPECT_FALSE(conditions_.contains("c"));
  // The next sample in time is live again.
  fault->Record(2);
  Run();
  EXPECT_EQ(Row("a").state, StreamLivenessStateDTO::kLive);
  EXPECT_FALSE(conditions_["a"]);
}

TEST_F(ComponentStreamHealthTest, SeverityDecidesTheVerdict) {
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault));
  auto warning = Declare("c", Stream("s", StreamSeverityDTO::kWarning));
  fault->Record(1);
  warning->Record(1);
  Run();
  Advance(101 * kMs);
  EXPECT_TRUE(conditions_["a"]);
  EXPECT_FALSE(conditions_.contains("c"));
  EXPECT_EQ(Row("c").state, StreamLivenessStateDTO::kStale);
  // Fresh again clears the stream condition itself.
  fault->Record(2);
  Run();
  EXPECT_FALSE(conditions_["a"]);
}

TEST_F(ComponentStreamHealthTest, AStaleStreamIsReportedOnceUntilLiveAgain) {
  auto fault = Declare("a", Stream("s", StreamSeverityDTO::kFault));
  fault->Record(1);
  Run();
  const auto calls_live = calls_;
  Advance(101 * kMs);
  ASSERT_TRUE(conditions_["a"]);
  EXPECT_EQ(calls_, calls_live + 1);
  // Many budgets later, still stale: no further log line or deadline.
  for (int i = 0; i < 10; ++i) Advance(100 * kMs);
  EXPECT_EQ(calls_, calls_live + 1);
  // Live again clears it once; the next break is one new report.
  fault->Record(2);
  Run();
  EXPECT_FALSE(conditions_["a"]);
  EXPECT_EQ(calls_, calls_live + 2);
  Advance(101 * kMs);
  EXPECT_TRUE(conditions_["a"]);
  EXPECT_EQ(calls_, calls_live + 3);
}

TEST_F(ComponentStreamHealthTest, DeploymentOverridesReplaceTheBudget) {
  auto recorder = Declare("map", Stream("map_updates", StreamSeverityDTO::kFault));
  recorder->Record(1);
  Run();
  ASSERT_TRUE(health_
                  .SetOverrides({{ControllableComponentId("map"), "map_updates", 300 * kMs}})
                  .ok());
  Run();
  EXPECT_EQ(Row("map").budget_ns, 300 * kMs);
  Advance(250 * kMs);
  EXPECT_EQ(Row("map").state, StreamLivenessStateDTO::kLive);
  Advance(60 * kMs);
  EXPECT_TRUE(conditions_["map"]);
  // Invalid tables change nothing.
  EXPECT_FALSE(health_.SetOverrides({{ControllableComponentId("map"), "map_updates", 0}}).ok());
  EXPECT_FALSE(health_
                   .SetOverrides({{ControllableComponentId("map"), "x", std::nullopt},
                                  {ControllableComponentId("map"), "x", 1}})
                   .ok());
  EXPECT_EQ(Row("map").budget_ns, 300 * kMs);
  // Removing the override restores the default.
  ASSERT_TRUE(health_.SetOverrides({}).ok());
  Run();
  EXPECT_EQ(Row("map").budget_ns, 100 * kMs);
}

TEST_F(ComponentStreamHealthTest, OneTimerAndNoneWithoutLiveStreams) {
  std::vector<std::unique_ptr<StreamRecorder>> recorders;
  for (int i = 0; i < 20; ++i) {
    recorders.push_back(Declare("arm", Stream("s" + std::to_string(i), StreamSeverityDTO::kWarning)));
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
  auto first = Declare("arm", Stream("s", StreamSeverityDTO::kFault));
  Advance(100 * kMs);
  EXPECT_TRUE(conditions_["arm"]);
  auto second = Declare("arm", Stream("s", StreamSeverityDTO::kFault));
  EXPECT_FALSE(conditions_["arm"]);  // fresh grace for the new adapter
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

}  // namespace
}  // namespace manyforge::core

// The point-cloud source component (adapter family `pointcloud`): state,
// actions and settings, against a fake bridge and process launcher.
#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_roboplan/pointcloud_source_adapter.hpp>

#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;

class FakeLink final : public IPointCloudBridgeLink {
 public:
  PointCloudBridgeObservation observed;
  std::vector<bool> forwarding;
  void SetForwarding(const bool value) override { forwarding.push_back(value); }
  PointCloudBridgeObservation Observed() const override { return observed; }
};

struct ProcessLog final {
  std::vector<ComponentProcessCommand> started;
  int stopped{};
  std::optional<std::string> exit;
  bool refuse{};
};

class FakeProcess final : public IComponentProcess {
 public:
  explicit FakeProcess(std::shared_ptr<ProcessLog> log) : log_(std::move(log)) {}
  ~FakeProcess() override { ++log_->stopped; }
  std::optional<std::string> exited() override { return log_->exit; }

 private:
  std::shared_ptr<ProcessLog> log_;
};

PointCloudSourceSettings Settings() {
  return {"/front_stereo_camera/pointcloud", "universe", 200000, 0.1, 1.0};
}

struct Fixture {
  std::shared_ptr<ProcessLog> log = std::make_shared<ProcessLog>();
  std::shared_ptr<FakeLink> link = std::make_shared<FakeLink>();
  PointCloudSourceAdapter adapter{
      Settings(), "/manyforge/components/cloud", "cell",
      [log = log](const ComponentProcessCommand &command) -> std::unique_ptr<IComponentProcess> {
        if (log->refuse) throw std::runtime_error("ros2 not found");
        log->started.push_back(command);
        return std::make_unique<FakeProcess>(log);
      },
      link};
};

TEST(PointCloudSourceAdapter, NotPreparedUntilPrepareStartsTheBridge) {
  Fixture f;
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kNotPrepared);
  const auto result = f.adapter.Run(core::ComponentAction::kPrepare, {});
  EXPECT_EQ(result.outcome, core::ActionOutcome::kSucceeded);
  ASSERT_EQ(f.log->started.size(), 1U);
  const auto &command = f.log->started.front();
  EXPECT_EQ(command.package, "manyforge_ros_bringup");
  EXPECT_EQ(command.launch_file, "pointcloud_source.launch.py");
  EXPECT_EQ(command.arguments,
            (std::vector<std::string>{"namespace:=/manyforge/components/cloud",
                                      "topic:=/front_stereo_camera/pointcloud",
                                      "source_frame_id:=universe", "cell_id:=cell",
                                      "max_points:=200000", "minimum_period_s:=0.1"}));
  EXPECT_EQ(f.link->forwarding, std::vector<bool>{false});
  const auto state = f.adapter.State(0);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.activity, core::ComponentActivity::kIdle);
  EXPECT_TRUE(f.adapter.capabilities().empty());
}

TEST(PointCloudSourceAdapter, EnableAndDisableSwitchForwarding) {
  Fixture f;
  const auto refused = f.adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "component_not_prepared");
  ASSERT_EQ(f.adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kDisable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(f.link->forwarding, (std::vector<bool>{false, true, false}));
  const auto unsupported = f.adapter.Run(core::ComponentAction::kResetPosition, {});
  EXPECT_EQ(unsupported.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(unsupported.error->code, "component_action_unsupported");
}

TEST(PointCloudSourceAdapter, StateDoesNotWaitForForwardingPublish) {
  class BlockingLink final : public IPointCloudBridgeLink {
   public:
    std::promise<void> entered;
    std::promise<void> release;
    std::shared_future<void> gate{release.get_future().share()};
    void SetForwarding(bool value) override {
      if (!value) return;
      entered.set_value();
      gate.wait();
    }
    PointCloudBridgeObservation Observed() const override { return {}; }
  };
  auto log = std::make_shared<ProcessLog>();
  auto link = std::make_shared<BlockingLink>();
  PointCloudSourceAdapter adapter{
      Settings(), "/manyforge/components/cloud", "cell",
      [log](const ComponentProcessCommand &) -> std::unique_ptr<IComponentProcess> {
        return std::make_unique<FakeProcess>(log);
      }, link};
  ASSERT_EQ(adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  auto action = std::async(std::launch::async, [&] {
    return adapter.Run(core::ComponentAction::kEnable, {});
  });
  link->entered.get_future().wait();
  auto observation = std::async(std::launch::async, [&] { return adapter.State(0); });
  const auto readiness = observation.wait_for(std::chrono::milliseconds(500));
  link->release.set_value();
  const auto state = observation.get();
  EXPECT_EQ(readiness, std::future_status::ready);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(action.get().outcome, core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
}

TEST(PointCloudSourceAdapter, PublishExceptionPreservesExistingForwardingOutcome) {
  class ThrowingLink final : public IPointCloudBridgeLink {
   public:
    void SetForwarding(bool value) override {
      if (value) throw std::runtime_error("publish failed");
    }
    PointCloudBridgeObservation Observed() const override { return {}; }
  };
  auto log = std::make_shared<ProcessLog>();
  PointCloudSourceAdapter adapter{
      Settings(), "/manyforge/components/cloud", "cell",
      [log](const ComponentProcessCommand &) -> std::unique_ptr<IComponentProcess> {
        return std::make_unique<FakeProcess>(log);
      }, std::make_shared<ThrowingLink>()};
  ASSERT_EQ(adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_THROW(static_cast<void>(adapter.Run(core::ComponentAction::kEnable, {})),
               std::runtime_error);
  EXPECT_EQ(adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
}

TEST(PointCloudSourceAdapter, StaleStreamIsFreshnessOnlyNeverAFault) {
  Fixture f;
  ASSERT_EQ(f.adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  ASSERT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  auto state = f.adapter.State(10 * kSecond);
  ASSERT_EQ(state.streams.size(), 1U);
  EXPECT_EQ(state.streams.front().stream_id, "pointcloud");
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kNone);
  f.link->observed = {true, std::nullopt};
  EXPECT_EQ(f.adapter.State(10 * kSecond).streams.front().freshness,
            core::StreamFreshness::kStale);
  f.link->observed = {true, 9 * kSecond};
  state = f.adapter.State(10 * kSecond);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kFresh);
  EXPECT_EQ(state.streams.front().age_ns, kSecond);
  state = f.adapter.State(20 * kSecond);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kStale);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
}

TEST(PointCloudSourceAdapter, ProcessExitIsAFaultThatRecoverClears) {
  Fixture f;
  ASSERT_EQ(f.adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  f.log->exit = "exit code 1";
  auto state = f.adapter.State(5);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kProcessExited);
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  EXPECT_TRUE(state.fault->clearable);
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).error->code,
            "component_not_prepared");
  f.log->exit.reset();
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kRecover, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.log->stopped, 1);
  EXPECT_EQ(f.log->started.size(), 2U);
  EXPECT_EQ(f.adapter.State(5).lifecycle, core::ComponentLifecycle::kPrepared);
}

TEST(PointCloudSourceAdapter, PrepareStartFailureIsTyped) {
  Fixture f;
  f.log->refuse = true;
  const auto result = f.adapter.Run(core::ComponentAction::kPrepare, {});
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "component_process_start_failed");
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kNotPrepared);
}

TEST(PointCloudSourceSettings, ParsesTheMergedRowAndRefusesBadValues) {
  const auto row = [](const std::string &json) {
    return content_identity::parse_canonical_json(json);
  };
  const auto settings = pointcloud_source_settings(
      row(R"({"adapter":"pointcloud","max_points":1000,"minimum_period_s":0.5,)"
          R"("source_frame_id":"universe","stream_budget_s":1,"topic":"/cloud"})"));
  EXPECT_EQ(settings.topic, "/cloud");
  EXPECT_EQ(settings.source_frame_id, "universe");
  EXPECT_EQ(settings.max_points, 1000);
  EXPECT_DOUBLE_EQ(settings.minimum_period_s, 0.5);
  EXPECT_DOUBLE_EQ(settings.stream_budget_s, 1.0);
  for (const auto *bad : {
           R"({"max_points":1000,"minimum_period_s":0.5,"stream_budget_s":1,"topic":"/cloud"})",
           R"({"max_points":1000,"minimum_period_s":0.5,"source_frame_id":"u","stream_budget_s":1,"topic":"cloud"})",
           R"({"max_points":0,"minimum_period_s":0.5,"source_frame_id":"u","stream_budget_s":1,"topic":"/cloud"})",
           R"({"max_points":1.5,"minimum_period_s":0.5,"source_frame_id":"u","stream_budget_s":1,"topic":"/cloud"})",
           R"({"max_points":10,"minimum_period_s":11,"source_frame_id":"u","stream_budget_s":1,"topic":"/cloud"})",
           R"({"max_points":10,"minimum_period_s":0.5,"source_frame_id":"u","topic":"/cloud"})"})
    EXPECT_THROW(static_cast<void>(pointcloud_source_settings(row(bad))), std::invalid_argument)
        << bad;
}

}  // namespace
}  // namespace manyforge::roboplan_backend

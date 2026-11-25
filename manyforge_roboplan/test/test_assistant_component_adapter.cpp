// The assistant component (adapter family `assistant`): state, actions and
// settings, against a fake host link, process launcher, clock and pause.
#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_roboplan/assistant_component_adapter.hpp>

#include <chrono>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000;
constexpr std::int64_t kPollNs =
    std::chrono::duration_cast<std::chrono::nanoseconds>(kAssistantPollPeriod).count();
constexpr auto kUrl = "http://127.0.0.1:40123";

class FakeLink final : public IAssistantHostLink {
 public:
  AssistantHostObservation observed;
  std::vector<bool> serving;
  void SetServing(const bool value) override { serving.push_back(value); }
  AssistantHostObservation Observed() const override { return observed; }
};

struct ProcessLog final {
  std::vector<ComponentProcessCommand> started;
  int stopped{};
  std::vector<std::thread::id> stop_threads;
  int released{};
  std::optional<std::string> exit;
  /// Runs in exited() (e.g. to hold a State() reader there).
  std::function<void()> on_exited;
  std::string output_tail;
  bool refuse{};
};

class FakeProcess final : public IComponentProcess {
 public:
  explicit FakeProcess(std::shared_ptr<ProcessLog> log) : log_(std::move(log)) {}
  ~FakeProcess() override { ++log_->released; }
  void stop() override {
    ++log_->stopped;
    log_->stop_threads.push_back(std::this_thread::get_id());
  }
  std::optional<std::string> exited() override {
    if (log_->on_exited) log_->on_exited();
    return log_->exit;
  }
  std::string output_tail() const override { return log_->output_tail; }

 private:
  std::shared_ptr<ProcessLog> log_;
};

/// The host's side, driven by the pause: what it reports after the
/// adapter's `pauses`-th poll (none when unset).
struct Host final {
  int pauses{};
  std::function<void(int)> on_pause;
};

struct Fixture {
  std::shared_ptr<ProcessLog> log = std::make_shared<ProcessLog>();
  std::shared_ptr<FakeLink> link = std::make_shared<FakeLink>();
  std::shared_ptr<core::FakeClock> clock = std::make_shared<core::FakeClock>();
  std::shared_ptr<Host> host = std::make_shared<Host>();
  AssistantComponentAdapter adapter{
      AssistantSettings{"local_process", 1.0, 5.0},
      "/manyforge/components/assistant",
      [log = log](const ComponentProcessCommand &command) -> std::unique_ptr<IComponentProcess> {
        if (log->refuse) throw std::runtime_error("ros2 not found");
        log->started.push_back(command);
        return std::make_unique<FakeProcess>(log);
      },
      link,
      clock,
      [clock = clock, host = host] {
        clock->advance(kPollNs);
        ++host->pauses;
        if (host->on_pause) host->on_pause(host->pauses);
      }};

  /// The host serves after `polls` polls: endpoint and a `/health` answer.
  void ServeAfter(const int polls) {
    host->on_pause = [this, polls](const int pauses) {
      if (pauses != polls) return;
      link->observed.endpoint = kUrl;
      link->observed.last_health_ns = clock->now();
    };
  }

  void Prepare() {
    ASSERT_EQ(adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
              core::ActionOutcome::kSucceeded);
  }

  void Enable() {
    ServeAfter(1);
    ASSERT_EQ(adapter.Run(core::ComponentAction::kEnable, {}).outcome,
              core::ActionOutcome::kSucceeded);
  }
};

TEST(AssistantComponentAdapter, NotPreparedUntilPrepareStartsTheHost) {
  Fixture f;
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kNotPrepared);
  f.Prepare();
  ASSERT_EQ(f.log->started.size(), 1U);
  const auto &command = f.log->started.front();
  EXPECT_EQ(command.package, "manyforge_ros_bringup");
  EXPECT_EQ(command.launch_file, "assistant.launch.py");
  EXPECT_EQ(command.arguments,
            (std::vector<std::string>{"namespace:=/manyforge/components/assistant",
                                      "backend:=local_process", "health_period_s:=1"}));
  EXPECT_EQ(f.link->serving, std::vector<bool>{false});
  const auto state = f.adapter.State(0);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.activity, core::ComponentActivity::kIdle);
  EXPECT_TRUE(state.endpoints.empty());
  ASSERT_EQ(state.streams.size(), 1U);
  EXPECT_EQ(state.streams.front().stream_id, "bridge");
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kNone);
  EXPECT_TRUE(f.adapter.capabilities().empty());
}

TEST(AssistantComponentAdapter, EnableWaitsForTheEndpointThenPublishesIt) {
  Fixture f;
  const auto refused = f.adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(refused.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(refused.error.has_value());
  EXPECT_EQ(refused.error->code, "component_not_prepared");
  f.Prepare();
  f.ServeAfter(5);
  f.host->on_pause = [&, serve = f.host->on_pause](const int pauses) {
    // Prepared while the host hasn't answered yet.
    EXPECT_EQ(f.adapter.State(f.clock->now()).lifecycle, core::ComponentLifecycle::kPrepared);
    serve(pauses);
  };
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.host->pauses, 5);
  EXPECT_EQ(f.link->serving, (std::vector<bool>{false, true}));
  const auto state = f.adapter.State(f.clock->now());
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kEnabled);
  ASSERT_EQ(state.endpoints.size(), 1U);
  EXPECT_EQ(state.endpoints.front(), (core::ComponentEndpoint{"assistant_request", kUrl}));
}

// No time limit: a slow bridge is waited for; only a Stop (or the host's
// exit) ends the wait, and the unfinished Enable withdraws its request.
TEST(AssistantComponentAdapter, EnableWaitsWithoutATimeLimitAndAStopWithdrawsIt) {
  Fixture f;
  f.Prepare();
  std::stop_source stop;
  f.host->on_pause = [&](const int pauses) {
    if (pauses == 10'000) stop.request_stop();
  };
  const auto result = f.adapter.Run(core::ComponentAction::kEnable, stop.get_token());
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "component_action_interrupted_by_stop");
  EXPECT_EQ(f.host->pauses, 10'000);
  EXPECT_EQ(f.link->serving.back(), false);
  EXPECT_EQ(f.adapter.State(f.clock->now()).lifecycle, core::ComponentLifecycle::kPrepared);
}

TEST(AssistantComponentAdapter, EnableIgnoresTheLatchedEndpointOfAReplacedHost) {
  Fixture f;
  f.Prepare();
  // The old host's latched URL without a `/health` answer since the action.
  f.link->observed.endpoint = "http://127.0.0.1:1";
  f.link->observed.last_health_ns = -1;
  f.ServeAfter(3);
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.host->pauses, 3);
  EXPECT_EQ(f.adapter.State(f.clock->now()).endpoints.front().url, kUrl);
}

TEST(AssistantComponentAdapter, EnableInterruptedByAStop) {
  Fixture f;
  f.Prepare();
  std::stop_source stop;
  f.host->on_pause = [&](const int pauses) {
    if (pauses == 3) stop.request_stop();
  };
  const auto result = f.adapter.Run(core::ComponentAction::kEnable, stop.get_token());
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "component_action_interrupted_by_stop");
  EXPECT_EQ(f.host->pauses, 3);
  EXPECT_EQ(f.adapter.State(f.clock->now()).lifecycle, core::ComponentLifecycle::kPrepared);
}

TEST(AssistantComponentAdapter, HostExitDuringEnableIsNotObserved) {
  Fixture f;
  f.Prepare();
  f.host->on_pause = [&](const int pauses) {
    if (pauses == 2) f.log->exit = "exit code 1";
  };
  const auto result = f.adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(result.error->code, "component_action_result_not_observed");
  EXPECT_EQ(f.adapter.State(f.clock->now()).lifecycle, core::ComponentLifecycle::kFault);
}

TEST(AssistantComponentAdapter, BusyHostIsBusyActivity) {
  Fixture f;
  f.Prepare();
  f.link->observed.busy = true;
  EXPECT_EQ(f.adapter.State(f.clock->now()).activity, core::ComponentActivity::kIdle);
  f.Enable();
  EXPECT_EQ(f.adapter.State(f.clock->now()).activity, core::ComponentActivity::kBusy);
  f.link->observed.busy = false;
  EXPECT_EQ(f.adapter.State(f.clock->now()).activity, core::ComponentActivity::kIdle);
}

TEST(AssistantComponentAdapter, DisableWaitsForTheBridgeToStop) {
  Fixture f;
  f.Prepare();
  f.Enable();
  f.host->pauses = 0;
  f.host->on_pause = [&](const int pauses) {
    // Enabled until the host reports the bridge stopped.
    EXPECT_EQ(f.adapter.State(f.clock->now()).lifecycle, core::ComponentLifecycle::kEnabled);
    if (pauses == 4) f.link->observed.endpoint.clear();
  };
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kDisable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.host->pauses, 4);
  EXPECT_EQ(f.link->serving, (std::vector<bool>{false, true, false}));
  const auto state = f.adapter.State(f.clock->now());
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_TRUE(state.endpoints.empty());
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kNone);

  f.link->observed.endpoint = kUrl;
  std::stop_source stop;
  f.host->on_pause = [&](const int pauses) {
    if (pauses == 1'000) stop.request_stop();
  };
  const auto stuck = f.adapter.Run(core::ComponentAction::kDisable, stop.get_token());
  EXPECT_EQ(stuck.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(stuck.error->code, "component_action_interrupted_by_stop");
  const auto unsupported = f.adapter.Run(core::ComponentAction::kResetPosition, {});
  EXPECT_EQ(unsupported.outcome, core::ActionOutcome::kFailed);
  EXPECT_EQ(unsupported.error->code, "component_action_unsupported");
}

TEST(AssistantComponentAdapter, HostExitIsAFaultThatRecoverClears) {
  Fixture f;
  f.Prepare();
  f.Enable();
  f.log->exit = "exit code 3";
  auto state = f.adapter.State(f.clock->now());
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kFault);
  ASSERT_TRUE(state.fault.has_value());
  EXPECT_EQ(state.fault->code, core::FaultCode::kProcessExited);
  EXPECT_EQ(state.fault->message, "The assistant host exited: exit code 3");
  EXPECT_EQ(state.fault->operator_action, core::FaultOperatorAction::kRecover);
  EXPECT_TRUE(state.fault->clearable);
  EXPECT_TRUE(state.endpoints.empty());
  EXPECT_TRUE(state.fault->details.empty());
  // The host's last output lines, when it left any.
  f.log->output_tail = "assistant host crashed";
  EXPECT_EQ(f.adapter.State(f.clock->now()).fault->details,
            (std::map<std::string, std::string>{{"output_tail", "assistant host crashed"}}));
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kEnable, {}).error->code,
            "component_not_prepared");
  f.log->exit.reset();
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kRecover, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(f.log->stopped, 1);
  EXPECT_EQ(f.log->started.size(), 2U);
  EXPECT_EQ(f.link->serving, (std::vector<bool>{false, true, false}));
  // The old host's latched URL doesn't make the new one enabled.
  state = f.adapter.State(f.clock->now());
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_TRUE(state.endpoints.empty());
}

TEST(AssistantComponentAdapter, RecoverStopsTheOldHostOnItsThreadAStateReaderOnlyReleasesIt) {
  Fixture f;
  f.Prepare();
  // A State() reader holds the old host's handle (in exited()) while Recover
  // replaces it.
  std::promise<void> entered;
  std::promise<void> release;
  std::shared_future<void> gate{release.get_future().share()};
  std::once_flag hold;
  f.log->on_exited = [&] {
    std::call_once(hold, [&] {
      entered.set_value();
      gate.wait();
    });
  };
  auto reader = std::async(std::launch::async, [&] {
    static_cast<void>(f.adapter.State(0));
    return std::this_thread::get_id();
  });
  entered.get_future().wait();
  EXPECT_EQ(f.adapter.Run(core::ComponentAction::kRecover, {}).outcome,
            core::ActionOutcome::kSucceeded);
  // Stopped once, on the Recover caller's thread; the reader still holds it.
  EXPECT_EQ(f.log->stopped, 1);
  EXPECT_EQ(f.log->stop_threads, std::vector<std::thread::id>{std::this_thread::get_id()});
  EXPECT_EQ(f.log->released, 0);
  EXPECT_EQ(f.log->started.size(), 2U);
  release.set_value();
  EXPECT_NE(reader.get(), std::this_thread::get_id());
  // The reader's drop released it without stopping it again.
  EXPECT_EQ(f.log->released, 1);
  EXPECT_EQ(f.log->stopped, 1);
}

TEST(AssistantComponentAdapter, PrepareStartFailureIsTyped) {
  Fixture f;
  f.log->refuse = true;
  const auto result = f.adapter.Run(core::ComponentAction::kPrepare, {});
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "component_process_start_failed");
  EXPECT_EQ(f.adapter.State(0).lifecycle, core::ComponentLifecycle::kNotPrepared);
}

TEST(AssistantComponentState, BridgeStreamIsFreshStaleOrNone) {
  const AssistantHostObservation none{kUrl, false, std::nullopt};
  const AssistantHostObservation answered{kUrl, false, 9 * kSecond};
  const auto budget = 5 * kSecond;
  auto state =
      AssistantComponentState(true, std::nullopt, "", false, answered, budget, 10 * kSecond);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kNone);
  state = AssistantComponentState(true, std::nullopt, "", true, none, budget, 10 * kSecond);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kStale);
  state = AssistantComponentState(true, std::nullopt, "", true, answered, budget, 10 * kSecond);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kFresh);
  EXPECT_EQ(state.streams.front().age_ns, kSecond);
  state = AssistantComponentState(true, std::nullopt, "", true, answered, budget, 20 * kSecond);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kStale);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.fault.has_value());
  // Serving without an endpoint (the host stopped the bridge) is prepared.
  state = AssistantComponentState(true, std::nullopt, "", true, {"", false, 9 * kSecond}, budget,
                                  10 * kSecond);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kPrepared);
  EXPECT_EQ(state.streams.front().freshness, core::StreamFreshness::kNone);
}

TEST(AssistantSettings, ParsesTheMergedRowAndRefusesBadValues) {
  const auto row = [](const std::string &json) {
    return content_identity::parse_canonical_json(json);
  };
  const auto settings = assistant_settings(
      row(R"({"adapter":"assistant","assistant_profile_id":"assistant_scene_authoring",)"
          R"("backend":"local_process","health_period_s":0.5,"model_api_key_env":null,)"
          R"("model_base_url":"http://127.0.0.1:8000/v1","model_component_id":"assistant_model",)"
          R"("stream_budget_s":5})"));
  EXPECT_EQ(settings.backend, "local_process");
  EXPECT_DOUBLE_EQ(settings.health_period_s, 0.5);
  EXPECT_DOUBLE_EQ(settings.stream_budget_s, 5.0);
  for (const auto *bad : {
           R"({"health_period_s":1,"stream_budget_s":5})",
           R"({"backend":"","health_period_s":1,"stream_budget_s":5})",
           R"({"backend":"local process","health_period_s":1,"stream_budget_s":5})",
           R"({"backend":1,"health_period_s":1,"stream_budget_s":5})",
           R"({"backend":"local_process","health_period_s":0.01,"stream_budget_s":5})",
           R"({"backend":"local_process","health_period_s":61,"stream_budget_s":5})",
           R"({"backend":"local_process","health_period_s":1,"stream_budget_s":0})",
           R"({"backend":"local_process","health_period_s":1})"})
    EXPECT_THROW(static_cast<void>(assistant_settings(row(bad))), std::invalid_argument) << bad;
}

}  // namespace
}  // namespace manyforge::roboplan_backend

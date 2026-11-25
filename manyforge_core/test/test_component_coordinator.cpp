#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <manyforge_core/component_coordinator.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <thread>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {
namespace {

using namespace std::chrono_literals;

TEST(ComponentFaultCode, OwnerStalledRoundTripsWithItsTypedWireName) {
  EXPECT_EQ(to_string(FaultCode::kOwnerStalled), "component_owner_stalled");
  ASSERT_TRUE(parse_fault_code("component_owner_stalled"));
  EXPECT_EQ(*parse_fault_code("component_owner_stalled"), FaultCode::kOwnerStalled);
}

TEST(ComponentFaultCode, RosTransportFailedRoundTripsWithItsTypedWireName) {
  EXPECT_EQ(to_string(FaultCode::kRosTransportFailed), "fault_ros_transport_failed");
  ASSERT_TRUE(parse_fault_code("fault_ros_transport_failed"));
  EXPECT_EQ(*parse_fault_code("fault_ros_transport_failed"), FaultCode::kRosTransportFailed);
}

ActionResult Outcome(ActionOutcome outcome, std::optional<ActionError> error = std::nullopt) {
  ActionResult result;
  result.outcome = outcome;
  result.error = std::move(error);
  return result;
}

ActionError Error(std::string code, std::string message,
                  std::optional<std::string> vendor_code = std::nullopt,
                  std::optional<std::string> vendor_text = std::nullopt) {
  return ActionError{std::move(code), std::move(message), std::move(vendor_code),
                     std::move(vendor_text)};
}

ComponentState Observed(ComponentLifecycle lifecycle, ComponentActivity activity,
                        std::optional<Fault> fault = std::nullopt) {
  ComponentState state;
  state.lifecycle = lifecycle;
  state.activity = activity;
  state.fault = std::move(fault);
  return state;
}

/// The lifecycle a compliant adapter shows once `action` succeeded;
/// none for actions without a lifecycle target.
std::optional<ComponentLifecycle> ResultingLifecycle(const ComponentAction action) {
  switch (action) {
    case ComponentAction::kEnable:
      return ComponentLifecycle::kEnabled;
    case ComponentAction::kPowerOff:
      return ComponentLifecycle::kOffline;
    case ComponentAction::kPrepare:
    case ComponentAction::kDisable:
    case ComponentAction::kRecover:
    case ComponentAction::kReboot:
      return ComponentLifecycle::kPrepared;
    case ComponentAction::kResetPosition:
    case ComponentAction::kSimReset:
      return std::nullopt;
  }
  return std::nullopt;
}

/// Runs each action until the test finishes it or a Stop is requested. Like
/// a compliant adapter, a succeeded action's result shows in State() by the
/// time Run returns, unless `set_follows(false)`.
class FakeAdapter final : public IComponentAdapter {
 public:
  explicit FakeAdapter(std::vector<Capability> capabilities = {})
      : capabilities_(std::move(capabilities)) {}

  ComponentState State(std::int64_t now_ns) const override {
    std::lock_guard lock(mutex_);
    auto state = state_;
    state.observed_at_ns = now_ns;
    return state;
  }

  ActionResult Run(ComponentAction action, std::stop_token stop) override {
    std::unique_lock lock(mutex_);
    runs_.push_back(action);
    running_ = true;
    changed_.notify_all();
    changed_.wait(lock, stop, [&] { return finish_.has_value(); });
    running_ = false;
    if (finish_) {
      auto result = *finish_;
      finish_.reset();
      if (const auto shown = ResultingLifecycle(action);
          follows_ && shown && result.outcome == ActionOutcome::kSucceeded) {
        state_.lifecycle = *shown;
        if (action == ComponentAction::kRecover) state_.fault.reset();
      }
      return result;
    }
    return on_stop_;
  }

  std::vector<Capability> capabilities() const override { return capabilities_; }

  bool CanRecoverSharedOwner() const override {
    std::lock_guard lock(mutex_);
    return shared_owner_recovery_;
  }

  void set_shared_owner_recovery(bool enabled) {
    std::lock_guard lock(mutex_);
    shared_owner_recovery_ = enabled;
    prepared_recovery_ = enabled ? PreparedRecovery{[this](std::stop_token stop) {
      return Run(ComponentAction::kRecover, stop);
    }} : PreparedRecovery{};
  }

  PreparedRecovery PrepareSharedOwnerRecovery() const override {
    std::lock_guard lock(mutex_);
    return prepared_recovery_;
  }

  void set_state(ComponentState state) {
    std::lock_guard lock(mutex_);
    state_ = std::move(state);
  }
  void set_follows(const bool follows) {
    std::lock_guard lock(mutex_);
    follows_ = follows;
  }
  void set_on_stop(ActionResult result) {
    std::lock_guard lock(mutex_);
    on_stop_ = std::move(result);
  }
  void finish(ActionResult result) {
    std::lock_guard lock(mutex_);
    finish_ = std::move(result);
    changed_.notify_all();
  }
  bool wait_running() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] { return running_; });
  }
  bool wait_runs(std::size_t count) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] { return runs_.size() >= count && running_; });
  }
  std::vector<ComponentAction> runs() const {
    std::lock_guard lock(mutex_);
    return runs_;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable_any changed_;
  ComponentState state_ = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  std::vector<Capability> capabilities_;
  std::vector<ComponentAction> runs_;
  std::optional<ActionResult> finish_;
  ActionResult on_stop_ =
      Outcome(ActionOutcome::kFailed, Error("adapter_stopped", "stopped", "C2", "e-stop"));
  bool running_{};
  bool follows_{true};
  bool shared_owner_recovery_{};
  PreparedRecovery prepared_recovery_;
};

class Publications final {
 public:
  ComponentStatesSink sink() {
    return [this](const std::vector<ComponentState>& states) {
      std::lock_guard lock(mutex_);
      published_.push_back(states);
      changed_.notify_all();
    };
  }
  std::size_t count() const {
    std::lock_guard lock(mutex_);
    return published_.size();
  }
  std::vector<ComponentState> last() const {
    std::lock_guard lock(mutex_);
    return published_.back();
  }
  /// Waits until a publication shows `component_id` with a `last_action`.
  std::optional<ActionResult> wait_last_action(const std::string& component_id) {
    std::unique_lock lock(mutex_);
    std::optional<ActionResult> found;
    changed_.wait_for(lock, 5s, [&] {
      for (const auto& state : published_.back()) {
        if (state.component_id == component_id && state.last_action && !state.action) {
          found = state.last_action;
        }
      }
      return found.has_value();
    });
    return found;
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable changed_;
  std::vector<std::vector<ComponentState>> published_{{}};
};

struct Fixture {
  FakeClock clock;
  Publications publications;
  std::shared_ptr<FakeAdapter> robot = std::make_shared<FakeAdapter>(
      std::vector<Capability>{Capability::kResetPosition});
  std::shared_ptr<FakeAdapter> gripper = std::make_shared<FakeAdapter>();
  ComponentCoordinator coordinator{clock, publications.sink()};

  Fixture() {
    static_cast<void>(coordinator.SetComponents({{"robot", robot}, {"gripper", gripper}}));
  }
};

ActionResult Succeeded() { return Outcome(ActionOutcome::kSucceeded); }

TEST(ComponentCoordinator, RecoverCannotSucceedWhileStopRemainsObserved) {
  Fixture f;
  auto stopped = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  stopped.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 0};
  f.robot->set_state(std::move(stopped));
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kRecover).ok());
  ASSERT_TRUE(f.robot->wait_running());
  f.robot->finish(Succeeded());
  const auto result = f.publications.wait_last_action("robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(result->error);
  EXPECT_EQ(result->error->code, "component_action_result_not_observed");
}

TEST(ComponentCoordinator, RunsAnActionAndKeepsItsResultAsLastAction) {
  Fixture f;
  f.clock.set(100);
  const auto accepted = f.coordinator.Submit("robot", ComponentAction::kEnable);
  ASSERT_TRUE(accepted.ok());
  EXPECT_EQ(accepted.value(), (ComponentActionInProgress{ComponentAction::kEnable, 100}));
  ASSERT_TRUE(f.robot->wait_running());

  const auto running = f.coordinator.States();
  ASSERT_EQ(running.size(), 2U);
  EXPECT_EQ(running[0].action, accepted.value());

  f.clock.set(250);
  f.robot->finish(Succeeded());
  const auto last = f.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(*last, (ActionResult{ComponentAction::kEnable, ActionOutcome::kSucceeded,
                                 std::nullopt, 100, 250}));
  EXPECT_FALSE(f.coordinator.States()[0].action);
}

TEST(ComponentCoordinator, CloseAndDrainWaitsForActionAndRefusesNewActions) {
  Fixture f;
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kDisable).ok());
  ASSERT_TRUE(f.robot->wait_running());
  EXPECT_FALSE(f.coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 10ms));
  const auto refused = f.coordinator.Submit("robot", ComponentAction::kPrepare);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_not_ready");
  f.robot->finish(Succeeded());
  EXPECT_TRUE(f.coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 2s));
  EXPECT_TRUE(f.coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 2s));
}

TEST(ComponentCoordinator, DrainWaitsForPreparedRecoveryCaptureDestruction) {
  struct Capture final {
    std::promise<void>& entered;
    std::shared_future<void> release;
    ~Capture() { entered.set_value(); release.wait(); }
  };
  struct CaptureAdapter final : IComponentAdapter {
    std::promise<void>& entered;
    std::shared_future<void> release;
    CaptureAdapter(std::promise<void>& entered_value, std::shared_future<void> release_value)
        : entered(entered_value), release(std::move(release_value)) {}
    ComponentState State(std::int64_t) const override {
      return Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
    }
    ActionResult Run(ComponentAction, std::stop_token) override { return Succeeded(); }
    std::vector<Capability> capabilities() const override { return {}; }
    PreparedRecovery PrepareSharedOwnerRecovery() const override {
      auto capture = std::shared_ptr<Capture>(new Capture{entered, release});
      return [capture = std::move(capture)](std::stop_token) {
        static_cast<void>(capture);
        return Succeeded();
      };
    }
  };
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  std::promise<void> entered;
  auto entered_future = entered.get_future();
  std::promise<void> release;
  auto adapter = std::make_shared<CaptureAdapter>(entered, release.get_future().share());
  static_cast<void>(coordinator.SetComponents({{"io", adapter}}));
  const bool accepted = coordinator.Submit("io", ComponentAction::kRecover).ok();
  const bool cleanup_entered = entered_future.wait_for(2s) == std::future_status::ready;
  const bool drained_early = coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 10ms);
  release.set_value();
  const bool drained_after = coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 2s);
  EXPECT_TRUE(accepted);
  EXPECT_TRUE(cleanup_entered);
  EXPECT_FALSE(drained_early);
  EXPECT_TRUE(drained_after);
}

TEST(ComponentCoordinator, RunsOneActionAtATimePerComponent) {
  Fixture f;
  std::vector<std::string> reserved;
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kPrepare).ok());
  ASSERT_TRUE(f.robot->wait_running());
  EXPECT_EQ(f.coordinator.ReserveForApply({"gripper", "robot"}, reserved),
            std::optional<std::string>("robot"));

  const auto refused = f.coordinator.Submit("robot", ComponentAction::kEnable);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_action_in_progress");
  // Another component is independent.
  EXPECT_TRUE(f.coordinator.Submit("gripper", ComponentAction::kEnable).ok());

  f.robot->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("robot"));
  EXPECT_FALSE(f.coordinator.ReserveForApply({"robot"}, reserved).has_value());
  f.coordinator.ReleaseApplyReservation(reserved);
  EXPECT_TRUE(f.coordinator.Submit("robot", ComponentAction::kEnable).ok());
  f.robot->finish(Succeeded());
  f.gripper->finish(Succeeded());
}

TEST(ComponentCoordinator, ApplyReservationBlocksOnlyAffectedAndSurvivesReplacement) {
  Fixture f;
  std::vector<std::string> reserved;
  ASSERT_FALSE(f.coordinator.ReserveForApply({"robot"}, reserved).has_value());
  const auto blocked = f.coordinator.Submit("robot", ComponentAction::kPrepare);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, "component_action_in_progress");
  ASSERT_TRUE(f.coordinator.Submit("gripper", ComponentAction::kPrepare).ok());
  f.gripper->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("gripper"));
  auto replacement = std::make_shared<FakeAdapter>();
  auto retired = f.coordinator.SetComponents({{"robot", replacement}, {"gripper", f.gripper}});
  EXPECT_FALSE(f.coordinator.Submit("robot", ComponentAction::kPrepare).ok());
  f.coordinator.ReleaseApplyReservation(reserved);
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kPrepare).ok());
  replacement->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("robot"));
}

TEST(ComponentCoordinator, ReleasingApplyDoesNotClearAReservationForANewComponent) {
  Fixture f;
  std::vector<std::string> reserved;
  ASSERT_FALSE(f.coordinator.ReserveForApply({"robot", "new"}, reserved));
  ASSERT_EQ(reserved, (std::vector<std::string>{"robot"}));
  auto added = std::make_shared<FakeAdapter>();
  auto retired = f.coordinator.SetComponents({{"robot", f.robot}, {"gripper", f.gripper},
                                               {"new", added}});
  std::vector<std::string> other_reservation;
  ASSERT_FALSE(f.coordinator.ReserveForApply({"new"}, other_reservation));
  f.coordinator.ReleaseApplyReservation(reserved);
  const auto blocked = f.coordinator.Submit("new", ComponentAction::kPrepare);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, "component_action_in_progress");
  f.coordinator.ReleaseApplyReservation(other_reservation);
  ASSERT_TRUE(f.coordinator.Submit("new", ComponentAction::kPrepare).ok());
  added->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("new"));
}

TEST(ComponentCoordinator, StopInterruptsTheActionInProgress) {
  Fixture f;
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kRecover).ok());
  ASSERT_TRUE(f.robot->wait_running());

  f.coordinator.InterruptActions();
  const auto last = f.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(last->error);
  EXPECT_EQ(last->error->code, "component_action_interrupted_by_stop");
  EXPECT_EQ(last->error->vendor_code, "C2");
  // Actions stay available after the Stop.
  EXPECT_TRUE(f.coordinator.Submit("robot", ComponentAction::kRecover).ok());
  f.robot->finish(Succeeded());
}

// Stop state model PD-2: a Stop that names components interrupts only their
// actions; an action on a component it does not name keeps running.
TEST(ComponentCoordinator, NamedStopInterruptsOnlyTheNamedComponentsActions) {
  Fixture f;
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kRecover).ok());
  ASSERT_TRUE(f.coordinator.Submit("gripper", ComponentAction::kRecover).ok());
  ASSERT_TRUE(f.robot->wait_running());
  ASSERT_TRUE(f.gripper->wait_running());

  f.coordinator.InterruptActionsOf(
      [](const std::string_view component_id) { return component_id == "robot"; });
  const auto last = f.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(last->error);
  EXPECT_EQ(last->error->code, "component_action_interrupted_by_stop");
  // The gripper's action was not named: it ends with its own result.
  f.gripper->finish(Succeeded());
  const auto gripper = f.publications.wait_last_action("gripper");
  ASSERT_TRUE(gripper);
  EXPECT_EQ(gripper->outcome, ActionOutcome::kSucceeded);
}

TEST(ComponentCoordinator, UnknownOutcomeIsReportedAndNeverResent) {
  Fixture f;
  f.robot->set_on_stop(
      Outcome(ActionOutcome::kUnknown, Error("enable_response_missing", "no reply")));
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kEnable).ok());
  ASSERT_TRUE(f.robot->wait_running());
  f.coordinator.InterruptActions();

  const auto last = f.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kUnknown);
  EXPECT_EQ(last->error->code, "enable_response_missing");
  f.coordinator.Refresh();
  EXPECT_EQ(f.robot->runs(), std::vector<ComponentAction>{ComponentAction::kEnable});
}

TEST(ComponentCoordinator, RefusesUnsupportedActionsBeforeDispatch) {
  Fixture f;
  const auto refused = f.coordinator.Submit("gripper", ComponentAction::kResetPosition);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_action_unsupported");
  EXPECT_FALSE(f.coordinator.Submit("robot", ComponentAction::kPowerOff).ok());
  EXPECT_TRUE(f.gripper->runs().empty());

  const auto unknown = f.coordinator.Submit("camera", ComponentAction::kEnable);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().code, "component_not_found");

  // A declared capability is offered.
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kResetPosition).ok());
  f.robot->finish(Succeeded());
}

// A robot whose simulation isn't playing keeps its lifecycle
// with that reason; its position reset is refused before dispatch, its other
// actions are not.
TEST(ComponentCoordinator, RefusesAResetPositionWhileTheSimulationIsNotPlaying) {
  Fixture f;
  auto state = Observed(ComponentLifecycle::kEnabled, ComponentActivity::kIdle);
  state.reason = ComponentStateReason{"simulation_not_playing", "The simulation is not playing"};
  f.robot->set_state(state);
  const auto refused = f.coordinator.Submit("robot", ComponentAction::kResetPosition);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "simulation_not_playing");
  EXPECT_EQ(refused.error().details.at("action"), "reset_position");
  EXPECT_TRUE(f.robot->runs().empty());

  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kDisable).ok());
  ASSERT_TRUE(f.robot->wait_running());
  f.robot->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("robot").has_value());

  state.reason.reset();
  f.robot->set_state(state);
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kResetPosition).ok());
  ASSERT_TRUE(f.robot->wait_running());
  f.robot->finish(Succeeded());
  EXPECT_EQ(f.robot->runs(),
            (std::vector<ComponentAction>{ComponentAction::kDisable,
                                          ComponentAction::kResetPosition}));
}

TEST(ComponentCoordinator, PublishesStatesOnlyOnChange) {
  Fixture f;
  const auto initial = f.publications.count();
  const auto states = f.publications.last();
  ASSERT_EQ(states.size(), 2U);
  EXPECT_EQ(states[0].component_id, "robot");
  EXPECT_EQ(states[0].capabilities, std::vector<Capability>{Capability::kResetPosition});
  EXPECT_EQ(states[1].component_id, "gripper");

  f.clock.advance(1'000);  // only the observation time moved
  f.coordinator.Refresh();
  EXPECT_EQ(f.publications.count(), initial);

  f.gripper->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown,
                                Fault{FaultCode::kCommunication, "link lost", true,
                                      FaultOperatorAction::kRecover, std::nullopt, std::nullopt,
                                      {}, 0}));
  f.coordinator.Refresh();
  ASSERT_EQ(f.publications.count(), initial + 1U);
  EXPECT_EQ(f.publications.last()[1].lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(f.publications.last()[1].fault->code, FaultCode::kCommunication);
}

TEST(ComponentCoordinator, PublishesTheAdaptersDetailsAsReported) {
  Fixture f;
  const auto initial = f.publications.count();
  auto observed = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  observed.details = {{"mode", "position"}, {"state", "ready"}};
  f.robot->set_state(observed);
  f.coordinator.Refresh();
  ASSERT_EQ(f.publications.count(), initial + 1U);
  EXPECT_EQ(f.publications.last()[0].details, observed.details);
}

TEST(ComponentCoordinator, StampsTheUtcTimeOfAFaultAndAStopOnceAndKeepsIt) {
  Fixture f;
  const auto utc_now = [] {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
  };
  f.clock.set(5'000);
  auto observed = Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown,
                           Fault{FaultCode::kProcessExited, "exited", true,
                                 FaultOperatorAction::kRecover, std::nullopt, std::nullopt,
                                 {}, 4'000});
  observed.fault->details = {{"output_tail", "last line"}};
  observed.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 5'000};
  f.robot->set_state(observed);
  const auto before = utc_now();
  f.coordinator.Refresh();
  const auto after = utc_now();
  const auto first = f.publications.last()[0];
  ASSERT_TRUE(first.fault && first.stop);
  // The fault began 1 us before the read: its UTC time is 1 us before the
  // UTC clock read with it; the stop began at the read.
  EXPECT_GE(first.fault->since_utc_ns, before - 1'000);
  EXPECT_LE(first.fault->since_utc_ns, after - 1'000);
  EXPECT_GE(first.stop->since_utc_ns, before);
  EXPECT_LE(first.stop->since_utc_ns, after);
  EXPECT_EQ(first.fault->details, observed.fault->details);

  // The same fault and stop keep their UTC times: nothing new is published.
  const auto published = f.publications.count();
  f.clock.advance(1'000'000);
  f.coordinator.Refresh();
  EXPECT_EQ(f.publications.count(), published);
  EXPECT_EQ(f.coordinator.States()[0].fault->since_utc_ns, first.fault->since_utc_ns);
  EXPECT_EQ(f.coordinator.States()[0].stop->since_utc_ns, first.stop->since_utc_ns);

  // A new stop instant is stamped again.
  observed.stop->since_ns = f.clock.now();
  f.robot->set_state(observed);
  f.coordinator.Refresh();
  EXPECT_GE(f.publications.last()[0].stop->since_utc_ns, after);
  EXPECT_EQ(f.publications.last()[0].fault->since_utc_ns, first.fault->since_utc_ns);
}

TEST(ComponentCoordinator, RemovedComponentsLeaveTheCell) {
  Fixture f;
  ASSERT_TRUE(f.coordinator.Submit("gripper", ComponentAction::kDisable).ok());
  ASSERT_TRUE(f.gripper->wait_running());

  static_cast<void>(f.coordinator.SetComponents({{"robot", f.robot}}));
  const auto states = f.publications.last();
  ASSERT_EQ(states.size(), 1U);
  EXPECT_EQ(states[0].component_id, "robot");
  EXPECT_EQ(f.coordinator.Submit("gripper", ComponentAction::kEnable).error().code,
            "component_not_found");
}

/// An action that needs a lock the caller of SetComponents may hold (as the
/// host's route opening inside `enable` needs the composition lock).
class LockingAdapter final : public IComponentAdapter {
 public:
  explicit LockingAdapter(std::shared_mutex& lock) : lock_(lock) {}
  ComponentState State(std::int64_t) const override {
    return Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  }
  ActionResult Run(ComponentAction, std::stop_token) override {
    {
      std::lock_guard lock(mutex_);
      entered_ = true;
      changed_.notify_all();
    }
    std::shared_lock shared(lock_);
    return Outcome(ActionOutcome::kSucceeded);
  }
  std::vector<Capability> capabilities() const override { return {}; }
  bool wait_entered() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] { return entered_; });
  }

 private:
  std::shared_mutex& lock_;
  std::mutex mutex_;
  std::condition_variable changed_;
  bool entered_{};
};

TEST(ComponentCoordinator, RemovedActionsAreJoinedByTheCallerAfterItsLocks) {
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  std::shared_mutex composition;
  auto io = std::make_shared<LockingAdapter>(composition);
  static_cast<void>(coordinator.SetComponents({{"io", io}}));
  std::vector<std::jthread> removed;
  {
    std::unique_lock exclusive(composition);
    ASSERT_TRUE(coordinator.Submit("io", ComponentAction::kEnable).ok());
    ASSERT_TRUE(io->wait_entered());
    // Under the exclusive lock the removed action can't finish: Apply must not
    // wait for it here.
    removed = coordinator.SetComponents({});
    EXPECT_TRUE(coordinator.States().empty());
    EXPECT_FALSE(coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 10ms));
  }
  ASSERT_EQ(removed.size(), 1U);
  EXPECT_TRUE(removed[0].joinable());
  removed.clear();  // the action ends; its result is dropped
  EXPECT_TRUE(coordinator.CloseAndDrainActions(std::chrono::steady_clock::now() + 2s));
  EXPECT_TRUE(coordinator.States().empty());
}

/// A one-shot gate the test controls: once armed, the next `pass()` blocks
/// until `open()`.
class Gate final {
 public:
  void arm() {
    std::lock_guard lock(mutex_);
    armed_ = true;
  }
  void pass() {
    std::unique_lock lock(mutex_);
    if (!armed_) return;
    armed_ = false;
    entered_ = true;
    changed_.notify_all();
    // Bounded only so that a failing test ends instead of hanging.
    changed_.wait_for(lock, 30s, [&] { return opened_; });
  }
  bool wait_entered() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] { return entered_; });
  }
  void open() {
    std::lock_guard lock(mutex_);
    opened_ = true;
    changed_.notify_all();
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  bool armed_{};
  bool entered_{};
  bool opened_{};
};

/// A FakeAdapter whose State() blocks at its armed gate (an adapter read that
/// takes time, e.g. through its own locks or a system call).
class GatedAdapter final : public IComponentAdapter {
 public:
  explicit GatedAdapter(std::shared_ptr<FakeAdapter> inner) : inner_(std::move(inner)) {}
  ComponentState State(std::int64_t now_ns) const override {
    gate.pass();
    return inner_->State(now_ns);
  }
  ActionResult Run(ComponentAction action, std::stop_token stop) override {
    return inner_->Run(action, std::move(stop));
  }
  std::vector<Capability> capabilities() const override { return inner_->capabilities(); }
  bool CanRecoverSharedOwner() const override { return inner_->CanRecoverSharedOwner(); }
  PreparedRecovery PrepareSharedOwnerRecovery() const override {
    return inner_->PrepareSharedOwnerRecovery();
  }

  mutable Gate gate;

 private:
  std::shared_ptr<FakeAdapter> inner_;
};

// A Stop never waits for an action being admitted: the adapters' facts are
// read without the coordinator's lock. The Submit, released after the Stop,
// is admitted as in "Stop, then Submit": its action is not interrupted.
// (A Stop landing while the accepted action's thread is being created has no
// test seam; StoreWorker applies it.)
TEST(ComponentCoordinator, AStopDoesNotWaitForAnActionBeingAdmitted) {
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  auto robot = std::make_shared<FakeAdapter>();
  auto gated = std::make_shared<GatedAdapter>(robot);
  static_cast<void>(coordinator.SetComponents({{"robot", gated}}));
  gated->gate.arm();
  auto submitted = std::async(std::launch::async, [&] {
    return coordinator.Submit("robot", ComponentAction::kEnable);
  });
  const bool admitting = gated->gate.wait_entered();
  auto stopping = std::async(std::launch::async, [&] { coordinator.InterruptActions(); });
  const bool prompt = stopping.wait_for(5s) == std::future_status::ready;
  gated->gate.open();
  ASSERT_TRUE(admitting);
  EXPECT_TRUE(prompt);
  stopping.get();
  const auto accepted = submitted.get();
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(robot->wait_running());
  robot->finish(Succeeded());
  const auto last = publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kSucceeded);
  EXPECT_FALSE(last->error);
  EXPECT_EQ(robot->runs(), std::vector<ComponentAction>{ComponentAction::kEnable});
}

// A Submit being admitted holds nothing while it reads the adapters: another
// Submit for the same component is admitted meanwhile, and the first one,
// released, is refused as in "the other one, then this one".
TEST(ComponentCoordinator, ASubmitReadingTheAdaptersHoldsNothing) {
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  auto robot = std::make_shared<FakeAdapter>();
  auto gated = std::make_shared<GatedAdapter>(robot);
  auto gripper = std::make_shared<FakeAdapter>();
  static_cast<void>(coordinator.SetComponents({{"robot", gated}, {"gripper", gripper}}));
  gated->gate.arm();
  auto submitted = std::async(std::launch::async, [&] {
    return coordinator.Submit("robot", ComponentAction::kEnable);
  });
  const bool admitting = gated->gate.wait_entered();
  // The gate is one-shot: these read the adapters unblocked.
  const auto second = coordinator.Submit("robot", ComponentAction::kPrepare);
  const auto other = coordinator.Submit("gripper", ComponentAction::kPrepare);
  gated->gate.open();
  ASSERT_TRUE(admitting);
  EXPECT_TRUE(second.ok());
  EXPECT_TRUE(other.ok());
  const auto first = submitted.get();
  ASSERT_FALSE(first.ok());
  EXPECT_EQ(first.error().code, "component_action_in_progress");
  ASSERT_TRUE(robot->wait_running());
  robot->finish(Succeeded());
  gripper->finish(Succeeded());
  const auto last = publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->action, ComponentAction::kPrepare);
  ASSERT_TRUE(publications.wait_last_action("gripper"));
  EXPECT_EQ(robot->runs(), std::vector<ComponentAction>{ComponentAction::kPrepare});
}

// A refusal decided on the adapters' facts leaves no trace.
TEST(ComponentCoordinator, ARefusalFromTheAdaptersFactsLeavesNoTrace) {
  struct Case {
    FakeClock clock;
    Publications publications;
    std::shared_ptr<FakeAdapter> robot = std::make_shared<FakeAdapter>();
    std::shared_ptr<GatedAdapter> gated = std::make_shared<GatedAdapter>(robot);
    ComponentCoordinator coordinator{clock, publications.sink()};

    Case() { static_cast<void>(coordinator.SetComponents({{"robot", gated}})); }

    Result<ComponentActionInProgress> RefusedEnable() {
      auto stopped = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
      stopped.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 0};
      robot->set_state(std::move(stopped));
      gated->gate.arm();
      auto submitted = std::async(std::launch::async, [this] {
        return coordinator.Submit("robot", ComponentAction::kEnable);
      });
      EXPECT_TRUE(gated->gate.wait_entered());
      gated->gate.open();
      return submitted.get();
    }
  };
  {
    Case c;
    const auto refused = c.RefusedEnable();
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "component_action_refused");
    EXPECT_EQ(refused.error().message, "Recovery confirmation is required before Enable");
    EXPECT_TRUE(c.coordinator.CloseAndDrainActions(std::chrono::steady_clock::now()));
  }
  Case c;
  ASSERT_FALSE(c.RefusedEnable().ok());
  c.robot->set_state(Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle));
  ASSERT_TRUE(c.coordinator.Submit("robot", ComponentAction::kEnable).ok());
  ASSERT_TRUE(c.robot->wait_running());
  c.robot->finish(Succeeded());
  const auto last = c.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(c.robot->runs(), std::vector<ComponentAction>{ComponentAction::kEnable});
}

/// Runs until stopped; its stop callback blocks at `stop_gate` once armed (an
/// adapter whose stop takes time).
class SlowStopAdapter final : public IComponentAdapter {
 public:
  ComponentState State(std::int64_t) const override {
    return Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  }
  ActionResult Run(ComponentAction, std::stop_token stop) override {
    std::stop_callback slow(stop, [this] { stop_gate.pass(); });
    std::unique_lock lock(mutex_);
    running_ = true;
    changed_.notify_all();
    changed_.wait(lock, stop, [] { return false; });
    return Outcome(ActionOutcome::kFailed, Error("adapter_stopped", "stopped"));
  }
  std::vector<Capability> capabilities() const override { return {}; }
  bool wait_running() {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, 5s, [&] { return running_; });
  }

  Gate stop_gate;

 private:
  std::mutex mutex_;
  std::condition_variable_any changed_;
  bool running_{};
};

// The adapters' stop callbacks run after the coordinator's lock: a Stop
// callback that takes time holds up no other coordinator call.
TEST(ComponentCoordinator, AStopCallbackRunsWithoutTheCoordinatorsLock) {
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  auto slow = std::make_shared<SlowStopAdapter>();
  auto gripper = std::make_shared<FakeAdapter>();
  static_cast<void>(coordinator.SetComponents({{"robot", slow}, {"gripper", gripper}}));
  ASSERT_TRUE(coordinator.Submit("robot", ComponentAction::kRecover).ok());
  ASSERT_TRUE(slow->wait_running());
  slow->stop_gate.arm();
  auto stopping = std::async(std::launch::async, [&] { coordinator.InterruptActions(); });
  const bool in_callback = slow->stop_gate.wait_entered();
  auto meanwhile = std::async(std::launch::async, [&] {
    return coordinator.Submit("gripper", ComponentAction::kPrepare).ok() &&
           coordinator.States().size() == 2U;
  });
  const bool returned = meanwhile.wait_for(5s) == std::future_status::ready;
  slow->stop_gate.open();
  EXPECT_TRUE(in_callback);
  EXPECT_TRUE(returned);
  EXPECT_TRUE(meanwhile.get());
  stopping.get();
  const auto last = publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  ASSERT_TRUE(last->error);
  EXPECT_EQ(last->error->code, "component_action_interrupted_by_stop");
  gripper->finish(Succeeded());
  ASSERT_TRUE(publications.wait_last_action("gripper"));
}

/// A machine `lite6` with its arm and controller I/O members.
struct MachineFixture {
  FakeClock clock;
  Publications publications;
  std::shared_ptr<FakeAdapter> owner = std::make_shared<FakeAdapter>(
      std::vector<Capability>{Capability::kPowerOff, Capability::kReboot});
  std::shared_ptr<FakeAdapter> arm = std::make_shared<FakeAdapter>();
  std::shared_ptr<FakeAdapter> io = std::make_shared<FakeAdapter>();
  ComponentCoordinator coordinator{clock, publications.sink()};

  MachineFixture() {
    static_cast<void>(coordinator.SetComponents({{"lite6", owner, std::nullopt, std::nullopt, true},
                                                 {"lite6_robot", arm, "lite6", "arm"},
                                                 {"lite6_io", io, "lite6", "controller_io"}}));
  }
};

TEST(ComponentCoordinator, ApplyAndOwnerRecoverExcludeEachOtherForAffectedMember) {
  MachineFixture f;
  std::vector<std::string> reserved;
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kRecover).ok());
  ASSERT_TRUE(f.owner->wait_running());
  EXPECT_EQ(f.coordinator.ReserveForApply({"lite6_robot"}, reserved),
            std::optional<std::string>("lite6_robot"));
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("lite6"));

  ASSERT_FALSE(f.coordinator.ReserveForApply({"lite6_robot"}, reserved));
  const auto blocked = f.coordinator.Submit("lite6", ComponentAction::kRecover);
  ASSERT_FALSE(blocked.ok());
  EXPECT_EQ(blocked.error().code, "component_action_in_progress");
  f.coordinator.ReleaseApplyReservation(reserved);
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kRecover).ok());
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("lite6"));
}

Fault MachineFault() {
  return Fault{FaultCode::kVendorFault, "system real time error", true,
               FaultOperatorAction::kRecover, "C26", "System Real Time Error",
               {"lite6_robot", "lite6_io"}, 0};
}

TEST(ComponentCoordinator, RecoveryPreflightReadsMemberCauseWithoutOwnerMask) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  Fault member_fault{FaultCode::kStreamStale, "measured joints stale", true,
                     FaultOperatorAction::kRecover, std::nullopt, std::nullopt, {}, 0};
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, member_fault));
  f.coordinator.Refresh();
  ASSERT_FALSE(f.publications.last()[1].fault);
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok()) << preflight.error().message;
  ASSERT_TRUE(preflight.value().owner);
  ASSERT_TRUE(preflight.value().owner->fault);
  EXPECT_EQ(preflight.value().owner->fault->vendor_code, std::optional<std::string>("C26"));
  ASSERT_TRUE(preflight.value().member.fault);
  EXPECT_EQ(preflight.value().member.fault->code, FaultCode::kStreamStale);
  EXPECT_EQ(RecoveryFact(preflight.value().owner.value()).eligibility, "recover");
  EXPECT_EQ(RecoveryFact(preflight.value().member).eligibility, "recover");
  EXPECT_TRUE(f.owner->runs().empty());
  EXPECT_TRUE(f.arm->runs().empty());
}

TEST(ComponentCoordinator, RecoveryFactsExcludeSimulatorAndUnknownState) {
  auto state = Observed(ComponentLifecycle::kOffline, ComponentActivity::kUnknown);
  state.component_id = "sim";
  EXPECT_EQ(RecoveryFact(state).eligibility, "unknown");
  state.capabilities = {Capability::kSimReset};
  EXPECT_EQ(RecoveryFact(state).eligibility, "excluded");
  state.capabilities.clear();
  state.lifecycle = ComponentLifecycle::kFault;
  state.fault = MachineFault();
  state.fault->clearable = false;
  EXPECT_EQ(RecoveryFact(state).eligibility, "blocked");
  state.fault->clearable = true;
  state.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 0};
  const auto both = RecoveryFact(state);
  EXPECT_EQ(both.eligibility, "recover");
  EXPECT_EQ(both.stop_code, std::optional<std::string>("estop"));
  EXPECT_EQ(both.stop_vendor_code, std::optional<std::string>("C2"));
}

TEST(ComponentCoordinator, ConfirmedRecoveryEnableRunsOrderedPhasesOnce) {
  MachineFixture f;
  std::vector<std::string> reserved;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  Fault fault{FaultCode::kStreamStale, "stale", true, FaultOperatorAction::kRecover,
              std::nullopt, std::nullopt, {}, 0};
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, fault));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  std::atomic<bool> resnapshotted{false};
  const auto accepted = f.coordinator.SubmitRecoveryEnable(
      "lite6_robot", confirmed, [] { return true; }, [&] { resnapshotted.store(true); return true; });
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  ASSERT_TRUE(f.owner->wait_runs(1));
  EXPECT_EQ(f.coordinator.ReserveForApply({"lite6"}, reserved), std::optional<std::string>("lite6"));
  EXPECT_EQ(f.coordinator.ReserveForApply({"lite6_robot"}, reserved), std::optional<std::string>("lite6_robot"));
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.arm->wait_runs(1));
  f.arm->finish(Succeeded());
  ASSERT_TRUE(f.arm->wait_runs(2));
  f.arm->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_TRUE(resnapshotted.load());
  EXPECT_EQ(result->started_at_ns, accepted.value().started_at_ns);
  EXPECT_EQ(result->outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(result->phases.size(), 3U);
  EXPECT_EQ(result->phases[0].component_id, "lite6");
  EXPECT_EQ(result->phases[0].action, ComponentAction::kRecover);
  EXPECT_EQ(result->phases[1].action, ComponentAction::kRecover);
  EXPECT_EQ(result->phases[2].action, ComponentAction::kEnable);
  EXPECT_EQ(f.owner->runs().size(), 1U);
  EXPECT_EQ(f.arm->runs().size(), 2U);
}

TEST(ComponentCoordinator, ChangedPublicationAfterOwnerRecoverStopsBeforeMember) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  ASSERT_TRUE(f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; },
                                               [] { return false; }).ok());
  ASSERT_TRUE(f.owner->wait_runs(1));
  f.owner->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(result->error);
  EXPECT_EQ(result->error->code, "stale_generation");
  ASSERT_EQ(result->phases.size(), 1U);
  EXPECT_EQ(result->phases.front().outcome, ActionOutcome::kSucceeded);
  EXPECT_TRUE(f.arm->runs().empty());
}

TEST(ComponentCoordinator, UnknownOwnerRecoveryNeverEnablesMember) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  ASSERT_TRUE(f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; }).ok());
  ASSERT_TRUE(f.owner->wait_runs(1));
  f.owner->finish(Outcome(ActionOutcome::kUnknown));
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kUnknown);
  ASSERT_EQ(result->phases.size(), 1U);
  EXPECT_TRUE(f.arm->runs().empty());
}

TEST(ComponentCoordinator, PlainEnableRefusesAStoppedMachine) {
  MachineFixture f;
  auto stopped = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  stopped.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 0};
  f.owner->set_state(std::move(stopped));
  const auto accepted = f.coordinator.Submit("lite6_robot", ComponentAction::kEnable);
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().code, "owner_estop_active");
  EXPECT_TRUE(f.arm->runs().empty());
}

// 2026-11-08 (maintainer): an e-stop is normal: it gates motion only. A
// member of a machine that reports one still prepares (Load works with the
// e-stop pressed).
TEST(ComponentCoordinator, AStoppedMachinesMemberStillPrepares) {
  MachineFixture f;
  auto stopped = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
  stopped.stop = ComponentStop{StopCode::kEstop, "E-stop", "C2", std::nullopt, 0};
  f.owner->set_state(std::move(stopped));
  const auto accepted = f.coordinator.Submit("lite6_robot", ComponentAction::kPrepare);
  EXPECT_TRUE(accepted.ok()) << accepted.error().code;
}

TEST(ComponentCoordinator, UnknownMemberCanEnableOnlyAfterOwnerRecoveryRevealsNoCause) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  f.arm->set_state(Observed(ComponentLifecycle::kOffline, ComponentActivity::kUnknown));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  ASSERT_EQ(confirmed.back().eligibility, "unknown");
  ASSERT_TRUE(f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; }).ok());
  ASSERT_TRUE(f.owner->wait_runs(1));
  f.arm->set_state(Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle));
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.arm->wait_runs(1));
  EXPECT_EQ(f.arm->runs().front(), ComponentAction::kEnable);
  f.arm->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(result->phases.size(), 2U);
  EXPECT_EQ(result->phases[0].action, ComponentAction::kRecover);
  EXPECT_EQ(result->phases[1].action, ComponentAction::kEnable);
}

TEST(ComponentCoordinator, CompositeRefusesWhileSiblingActionIsRunning) {
  MachineFixture f;
  ASSERT_TRUE(f.coordinator.Submit("lite6_io", ComponentAction::kEnable).ok());
  ASSERT_TRUE(f.io->wait_running());
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  const auto accepted = f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; });
  ASSERT_FALSE(accepted.ok());
  EXPECT_EQ(accepted.error().code, "component_action_in_progress");
  EXPECT_TRUE(f.arm->runs().empty());
  f.io->finish(Succeeded());
}

// OD-312: changed causes refuse with their own code (the GUI re-reads and
// shows them); a changed configuration stays `stale_generation`.
TEST(ComponentCoordinator, ChangedCausesAtSubmitAreAConfirmationChangeNotAStaleGeneration) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  const auto stale = f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return false; });
  ASSERT_FALSE(stale.ok());
  EXPECT_EQ(stale.error().code, "stale_generation");
  Fault changed{FaultCode::kStreamStale, "stale", true, FaultOperatorAction::kRecover,
                std::nullopt, std::nullopt, {}, 0};
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, changed));
  const auto refused = f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; });
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_recovery_confirmation_changed");
  EXPECT_TRUE(f.owner->runs().empty());
  EXPECT_TRUE(f.arm->runs().empty());
}

TEST(ComponentCoordinator, ChangedMemberCauseAfterOwnerRecoveryNeverRunsMember) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  ASSERT_TRUE(f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; }).ok());
  ASSERT_TRUE(f.owner->wait_runs(1));
  Fault changed{FaultCode::kStreamStale, "stale", true, FaultOperatorAction::kRecover,
                std::nullopt, std::nullopt, {}, 0};
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, changed));
  f.owner->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(result->error);
  EXPECT_EQ(result->error->code, "component_recovery_confirmation_changed");  // OD-312
  EXPECT_TRUE(f.arm->runs().empty());
}

TEST(ComponentCoordinator, MemberCauseClearedByOwnerRecoveryNeedsNoMemberRecover) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  Fault fault{FaultCode::kStreamStale, "stale", true, FaultOperatorAction::kRecover,
              std::nullopt, std::nullopt, {}, 0};
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, fault));
  const auto preflight = f.coordinator.RecoveryPreflight("lite6_robot");
  ASSERT_TRUE(preflight.ok());
  const auto confirmed = std::vector{RecoveryFact(*preflight.value().owner), RecoveryFact(preflight.value().member)};
  ASSERT_TRUE(f.coordinator.SubmitRecoveryEnable("lite6_robot", confirmed, [] { return true; }).ok());
  ASSERT_TRUE(f.owner->wait_runs(1));
  f.arm->set_state(Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle));
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.arm->wait_runs(1));
  EXPECT_EQ(f.arm->runs().front(), ComponentAction::kEnable);
  f.arm->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->outcome, ActionOutcome::kSucceeded);
  ASSERT_EQ(result->phases.size(), 2U);
  EXPECT_EQ(result->phases[1].action, ComponentAction::kEnable);
}

TEST(ComponentCoordinator, MembersCarryTheirOwnerAndRole) {
  MachineFixture f;
  const auto states = f.publications.last();
  ASSERT_EQ(states.size(), 3U);
  EXPECT_FALSE(states[0].owner_id);
  EXPECT_FALSE(states[0].role);
  EXPECT_EQ(states[1].owner_id, "lite6");
  EXPECT_EQ(states[1].role, "arm");
  EXPECT_EQ(states[2].role, "controller_io");
  // The owner is prepared: members show their own state.
  EXPECT_EQ(states[1].lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_FALSE(states[1].reason);
}

TEST(ComponentCoordinator, MembersAreOfflineThroughAnUnusableOwner) {
  MachineFixture f;
  auto offline = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
  offline.reason = ComponentStateReason{"shutdown_by_operator", "shut down by the operator"};
  f.owner->set_state(offline);
  f.io->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle,
                           Fault{FaultCode::kStreamStale, "stale", true,
                                 FaultOperatorAction::kRecover, std::nullopt, std::nullopt, {},
                                 0}));
  f.coordinator.Refresh();
  auto states = f.publications.last();
  for (std::size_t index : {1U, 2U}) {
    EXPECT_EQ(states[index].lifecycle, ComponentLifecycle::kOffline);
    EXPECT_FALSE(states[index].fault);
    ASSERT_TRUE(states[index].reason);
    EXPECT_EQ(states[index].reason->code, "owner_unavailable");
    EXPECT_EQ(states[index].reason->message, "through lite6: shut down by the operator");
  }

  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  f.coordinator.Refresh();
  EXPECT_EQ(f.publications.last()[1].reason->message, "through lite6: not prepared");

  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  f.coordinator.Refresh();
  states = f.publications.last();
  EXPECT_EQ(states[0].fault->concerns, (std::vector<std::string>{"lite6_robot", "lite6_io"}));
  EXPECT_EQ(states[1].reason->message, "through lite6: system real time error");

  // Usable again: each member shows its own state, nothing to acknowledge.
  f.owner->set_state(Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle));
  f.coordinator.Refresh();
  states = f.publications.last();
  EXPECT_EQ(states[1].lifecycle, ComponentLifecycle::kPrepared);
  EXPECT_EQ(states[2].lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(states[2].fault->code, FaultCode::kStreamStale);
}

TEST(ComponentCoordinator, MemberActionsWaitForAPreparedOwner) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  const auto refused = f.coordinator.Submit("lite6_robot", ComponentAction::kEnable);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "owner_not_prepared");
  EXPECT_EQ(refused.error().details.at("owner_id"), "lite6");
  EXPECT_TRUE(f.arm->runs().empty());

  // The owner's own actions stay available.
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kPrepare).ok());
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("lite6"));
  f.owner->set_state(Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle));
  ASSERT_TRUE(f.coordinator.Submit("lite6_robot", ComponentAction::kEnable).ok());
  f.arm->finish(Succeeded());
}

TEST(ComponentCoordinator, SharedOwnerRecoverKeepsNormalActionBookkeeping) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  const auto ordinary = f.coordinator.Submit("lite6_robot", ComponentAction::kRecover);
  ASSERT_FALSE(ordinary.ok());
  EXPECT_EQ(ordinary.error().code, "owner_not_prepared");

  f.arm->set_shared_owner_recovery(true);
  const auto enable = f.coordinator.Submit("lite6_robot", ComponentAction::kEnable);
  ASSERT_FALSE(enable.ok());
  EXPECT_EQ(enable.error().code, "owner_not_prepared");

  const auto accepted = f.coordinator.Submit("lite6_robot", ComponentAction::kRecover);
  ASSERT_TRUE(accepted.ok());
  ASSERT_TRUE(f.arm->wait_running());
  const auto duplicate = f.coordinator.Submit("lite6_robot", ComponentAction::kRecover);
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.error().code, "component_action_in_progress");
  f.arm->finish(Succeeded());
  const auto result = f.publications.wait_last_action("lite6_robot");
  ASSERT_TRUE(result);
  EXPECT_EQ(result->action, ComponentAction::kRecover);
  EXPECT_EQ(result->outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(result->started_at_ns, accepted.value().started_at_ns);
  EXPECT_EQ(f.arm->runs(), std::vector{ComponentAction::kRecover});
}

TEST(ComponentCoordinator, SharedOwnerRecoverStillRequiresItsDeclaredOwner) {
  MachineFixture f;
  f.io->set_shared_owner_recovery(true);
  static_cast<void>(f.coordinator.SetComponents({{"io", f.io, "missing", "io"}}));
  const auto refused = f.coordinator.Submit("io", ComponentAction::kRecover);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "owner_not_prepared");
  EXPECT_TRUE(f.io->runs().empty());
}

TEST(ComponentCoordinator, RecoverableSharedOwnerFaultRemainsVisibleOnItsMember) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  auto fault = MachineFault();
  fault.code = FaultCode::kProcessExited;
  fault.message = "shared owner failed";
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, fault));
  f.arm->set_shared_owner_recovery(true);
  f.coordinator.Refresh();
  auto states = f.publications.last();
  ASSERT_EQ(states.size(), 3U);
  EXPECT_EQ(states[1].lifecycle, ComponentLifecycle::kFault);
  ASSERT_TRUE(states[1].fault);
  EXPECT_EQ(states[1].fault->code, FaultCode::kProcessExited);
  EXPECT_EQ(states[2].lifecycle, ComponentLifecycle::kOffline);

  f.arm->set_shared_owner_recovery(false);
  f.coordinator.Refresh();
  states = f.publications.last();
  EXPECT_EQ(states[1].lifecycle, ComponentLifecycle::kOffline);
  EXPECT_FALSE(states[1].fault);
}

TEST(ComponentCoordinator, StalledMemberKeepsItsOwnFaultWhenOwnerIsAlsoStalled) {
  MachineFixture f;
  auto owner_fault = MachineFault();
  owner_fault.code = FaultCode::kOwnerStalled;
  owner_fault.message = "owner /machine stalled in task one";
  auto member_fault = owner_fault;
  member_fault.message = "owner /epoch stalled in task two";
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, owner_fault));
  f.arm->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kUnknown, member_fault));
  f.arm->set_shared_owner_recovery(false);
  f.coordinator.Refresh();
  const auto states = f.publications.last();
  const auto arm = std::find_if(states.begin(), states.end(),
      [](const auto &state) { return state.component_id == "lite6_robot"; });
  ASSERT_NE(arm, states.end());
  ASSERT_TRUE(arm->fault);
  EXPECT_EQ(arm->lifecycle, ComponentLifecycle::kFault);
  EXPECT_EQ(arm->fault->code, FaultCode::kOwnerStalled);
  EXPECT_EQ(arm->fault->message, member_fault.message);
}

TEST(ComponentCoordinator, OwnersOfferNoEnableOrDisable) {
  MachineFixture f;
  for (const auto action : {ComponentAction::kEnable, ComponentAction::kDisable}) {
    const auto refused = f.coordinator.Submit("lite6", action);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "component_action_unsupported");
  }
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kPowerOff).ok());
  f.arm->finish(Succeeded());
  f.io->finish(Succeeded());
  f.owner->finish(Succeeded());
  ASSERT_TRUE(f.publications.wait_last_action("lite6"));
}

TEST(ComponentCoordinator, AnOwnerWithoutActiveMembersIsStillAnOwner) {
  FakeClock clock;
  Publications publications;
  ComponentCoordinator coordinator{clock, publications.sink()};
  // Every member deactivated: no member names it, the flag still does.
  static_cast<void>(coordinator.SetComponents(
      {{"lite6", std::make_shared<FakeAdapter>(), std::nullopt, std::nullopt, true}}));
  for (const auto action : {ComponentAction::kEnable, ComponentAction::kDisable})
    EXPECT_EQ(coordinator.Submit("lite6", action).error().code, "component_action_unsupported");
}

ActionResult PublishedLastAction(Publications& publications, const std::string& component_id) {
  for (const auto& state : publications.last())
    if (state.component_id == component_id && state.last_action) return *state.last_action;
  return {};
}

std::optional<ComponentActionInProgress> PublishedAction(Publications& publications,
                                                         const std::string& component_id) {
  for (const auto& state : publications.last())
    if (state.component_id == component_id) return state.action;
  return std::nullopt;
}

TEST(ComponentCoordinator, AnOwnersPowerActionDisablesItsMembersFirstAsTheirOwnActions) {
  MachineFixture f;
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kPowerOff).ok());
  ASSERT_TRUE(f.arm->wait_running());
  EXPECT_EQ(f.arm->runs(), std::vector<ComponentAction>{ComponentAction::kDisable});
  EXPECT_EQ(PublishedAction(f.publications, "lite6_robot")->action, ComponentAction::kDisable);
  EXPECT_EQ(PublishedAction(f.publications, "lite6_io")->action, ComponentAction::kDisable);
  EXPECT_EQ(f.coordinator.Submit("lite6_io", ComponentAction::kEnable).error().code,
            "component_action_in_progress");
  EXPECT_TRUE(f.owner->runs().empty());

  f.arm->finish(Succeeded());
  ASSERT_TRUE(f.io->wait_running());
  EXPECT_TRUE(f.owner->runs().empty());
  f.io->finish(Succeeded());
  ASSERT_TRUE(f.owner->wait_running());
  EXPECT_EQ(f.owner->runs(), std::vector<ComponentAction>{ComponentAction::kPowerOff});
  f.owner->finish(Succeeded());
  const auto owner = f.publications.wait_last_action("lite6");
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->action, ComponentAction::kPowerOff);
  EXPECT_EQ(owner->outcome, ActionOutcome::kSucceeded);
  const auto arm = PublishedLastAction(f.publications, "lite6_robot");
  EXPECT_EQ(arm.action, ComponentAction::kDisable);
  EXPECT_EQ(arm.outcome, ActionOutcome::kSucceeded);
  EXPECT_EQ(PublishedLastAction(f.publications, "lite6_io").action, ComponentAction::kDisable);
}

TEST(ComponentCoordinator, AFailedMemberDisableEndsTheOwnersPowerAction) {
  MachineFixture f;
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kReboot).ok());
  ASSERT_TRUE(f.arm->wait_running());
  f.arm->finish(Outcome(ActionOutcome::kFailed, Error("ros2_control_refused", "refused")));
  const auto owner = f.publications.wait_last_action("lite6");
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->action, ComponentAction::kReboot);
  EXPECT_EQ(owner->outcome, ActionOutcome::kFailed);
  EXPECT_EQ(owner->error->code, "ros2_control_refused");
  EXPECT_EQ(PublishedLastAction(f.publications, "lite6_robot").outcome, ActionOutcome::kFailed);
  // The remaining member and the owner were left as they are.
  EXPECT_TRUE(f.io->runs().empty());
  EXPECT_FALSE(PublishedAction(f.publications, "lite6_io"));
  EXPECT_TRUE(f.owner->runs().empty());
}

TEST(ComponentCoordinator, AnOwnersPowerActionWaitsForItsMembersActions) {
  MachineFixture f;
  ASSERT_TRUE(f.coordinator.Submit("lite6_io", ComponentAction::kDisable).ok());
  const auto refused = f.coordinator.Submit("lite6", ComponentAction::kPowerOff);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_action_in_progress");
  EXPECT_EQ(refused.error().details.at("member_id"), "lite6_io");
  EXPECT_TRUE(f.arm->runs().empty());
  f.io->finish(Succeeded());
}

// An action's result is the component's state; the
// coordinator publishes last_action together with that state.
TEST(ComponentCoordinator, ASucceededActionIsPublishedWithItsResultingState) {
  Fixture f;
  ASSERT_TRUE(f.coordinator.Submit("robot", ComponentAction::kEnable).ok());
  ASSERT_TRUE(f.robot->wait_running());
  f.robot->finish(Succeeded());
  const auto last = f.publications.wait_last_action("robot");
  ASSERT_TRUE(last);
  EXPECT_EQ(last->outcome, ActionOutcome::kSucceeded);
  const auto states = f.publications.last();
  EXPECT_EQ(states[0].lifecycle, ComponentLifecycle::kEnabled);
  EXPECT_EQ(states[0].last_action, last);
}

TEST(ComponentCoordinator, ASucceededActionWhoseStateDoesntShowItFails) {
  struct Case {
    ComponentAction action;
    ComponentState before;
  };
  const auto fault = Fault{FaultCode::kDriveFault, "drive", true, FaultOperatorAction::kRecover,
                           std::nullopt, std::nullopt, {}, 0};
  for (const auto& [action, before] :
       {Case{ComponentAction::kPrepare,
             Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle)},
        Case{ComponentAction::kEnable,
             Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle)},
        Case{ComponentAction::kDisable,
             Observed(ComponentLifecycle::kEnabled, ComponentActivity::kIdle)},
        Case{ComponentAction::kRecover,
             Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, fault)}}) {
    SCOPED_TRACE(std::string(to_string(action)));
    Fixture f;
    f.robot->set_follows(false);
    f.robot->set_state(before);
    ASSERT_TRUE(f.coordinator.Submit("robot", action).ok());
    ASSERT_TRUE(f.robot->wait_running());
    f.robot->finish(Succeeded());
    const auto last = f.publications.wait_last_action("robot");
    ASSERT_TRUE(last);
    EXPECT_EQ(last->action, action);
    EXPECT_EQ(last->outcome, ActionOutcome::kFailed);
    ASSERT_TRUE(last->error);
    EXPECT_EQ(last->error->code, "component_action_result_not_observed");
  }
  // An enabled component's prepare and a recover that leaves it not prepared
  // show their results; a position reset has no lifecycle target.
  for (const auto& [action, before] :
       {Case{ComponentAction::kPrepare,
             Observed(ComponentLifecycle::kEnabled, ComponentActivity::kIdle)},
        Case{ComponentAction::kRecover,
             Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle)},
        Case{ComponentAction::kResetPosition,
             Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle)}}) {
    SCOPED_TRACE(std::string(to_string(action)));
    Fixture f;
    f.robot->set_follows(false);
    f.robot->set_state(before);
    ASSERT_TRUE(f.coordinator.Submit("robot", action).ok());
    ASSERT_TRUE(f.robot->wait_running());
    f.robot->finish(Succeeded());
    const auto last = f.publications.wait_last_action("robot");
    ASSERT_TRUE(last);
    EXPECT_EQ(last->outcome, ActionOutcome::kSucceeded);
  }
}

// The owner's prepared state is there when its prepare ends: a member's
// action right after it is accepted. An owner whose state doesn't show it
// carries the failure itself.
TEST(ComponentCoordinator, MemberPrepareRightAfterOwnerPrepareIsAccepted) {
  {
    MachineFixture f;
    f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
    ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kPrepare).ok());
    ASSERT_TRUE(f.owner->wait_running());
    f.owner->finish(Succeeded());
    const auto owner = f.publications.wait_last_action("lite6");
    ASSERT_TRUE(owner);
    EXPECT_EQ(owner->outcome, ActionOutcome::kSucceeded);
    ASSERT_TRUE(f.coordinator.Submit("lite6_robot", ComponentAction::kPrepare).ok());
    f.arm->finish(Succeeded());
    ASSERT_TRUE(f.publications.wait_last_action("lite6_robot"));
  }
  MachineFixture f;
  f.owner->set_follows(false);
  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  ASSERT_TRUE(f.coordinator.Submit("lite6", ComponentAction::kPrepare).ok());
  ASSERT_TRUE(f.owner->wait_running());
  f.owner->finish(Succeeded());
  const auto owner = f.publications.wait_last_action("lite6");
  ASSERT_TRUE(owner);
  EXPECT_EQ(owner->outcome, ActionOutcome::kFailed);
  ASSERT_TRUE(owner->error);
  EXPECT_EQ(owner->error->code, "component_action_result_not_observed");
  const auto member = f.coordinator.Submit("lite6_robot", ComponentAction::kPrepare);
  ASSERT_FALSE(member.ok());
  EXPECT_EQ(member.error().code, "owner_not_prepared");
}

TEST(CellLatchCauses, AMachineIsOneCauseThroughItsOwner) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle, MachineFault()));
  f.coordinator.Refresh();
  const auto causes = cell_latch_causes(f.coordinator.States());
  ASSERT_EQ(causes.size(), 1U);
  EXPECT_EQ(causes[0].component_id, "lite6");
  EXPECT_EQ(causes[0].kind, CellLatchCauseKind::kFault);
  // The published fault carries the UTC time of its `since_ns`.
  ASSERT_TRUE(causes[0].fault);
  EXPECT_GT(causes[0].fault->since_utc_ns, 0);
  auto expected = MachineFault();
  expected.since_utc_ns = causes[0].fault->since_utc_ns;
  EXPECT_EQ(causes[0].fault, expected);

  // Members listed before their owner still yield the owner once.
  auto states = f.coordinator.States();
  std::rotate(states.begin(), states.begin() + 1, states.end());
  const auto rotated = cell_latch_causes(states);
  ASSERT_EQ(rotated.size(), 1U);
  EXPECT_EQ(rotated[0].component_id, "lite6");
  EXPECT_EQ(rotated[0].kind, CellLatchCauseKind::kFault);

  // An unexpectedly offline owner is one `offline` cause for its members.
  auto lost = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
  lost.reason = ComponentStateReason{"process_exited", "the driver exited"};
  f.owner->set_state(lost);
  f.coordinator.Refresh();
  const auto offline = cell_latch_causes(f.coordinator.States());
  ASSERT_EQ(offline.size(), 1U);
  EXPECT_EQ(offline[0].component_id, "lite6");
  EXPECT_EQ(offline[0].kind, CellLatchCauseKind::kOffline);
}

TEST(CellLatchCauses, ANotPreparedOwnerAndItsMembersAreNoCause) {
  MachineFixture f;
  f.owner->set_state(Observed(ComponentLifecycle::kNotPrepared, ComponentActivity::kIdle));
  f.coordinator.Refresh();
  const auto states = f.coordinator.States();
  EXPECT_TRUE(cell_latch_causes(states).empty());
  for (const auto& state : states) {
    if (state.owner_id) {
      EXPECT_TRUE(latch_exempt(state, states));
    }
  }
}

TEST(CellLatchCauses, OfflineByTheOperatorsPowerActionIsNoCause) {
  for (const auto* code : {"shutdown_by_operator", "component_rebooting"}) {
    SCOPED_TRACE(code);
    MachineFixture f;
    auto offline = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
    offline.reason = ComponentStateReason{code, "by the operator"};
    f.owner->set_state(offline);
    f.coordinator.Refresh();
    const auto states = f.coordinator.States();
    EXPECT_TRUE(cell_latch_causes(states).empty());
    for (const auto& state : states) EXPECT_TRUE(latch_exempt(state, states));
  }
  // The same component offline for any other reason is a cause.
  ComponentState lost;
  lost.component_id = "camera";
  lost.lifecycle = ComponentLifecycle::kOffline;
  lost.activity = ComponentActivity::kIdle;
  lost.reason = ComponentStateReason{"process_exited", "the driver exited"};
  const auto causes = cell_latch_causes({lost});
  ASSERT_EQ(causes.size(), 1U);
  EXPECT_EQ(causes[0].kind, CellLatchCauseKind::kOffline);
}

TEST(CellLatchCauses, SimResetCapabilityExemptsOnlyTheSimulatorState) {
  for (const auto lifecycle : {ComponentLifecycle::kPrepared, ComponentLifecycle::kEnabled,
                               ComponentLifecycle::kOffline, ComponentLifecycle::kFault}) {
    SCOPED_TRACE(std::string(to_string(lifecycle)));
    auto simulator = Observed(lifecycle, ComponentActivity::kUnknown);
    simulator.component_id = "isaac_sim";
    simulator.capabilities = {Capability::kSimReset};
    if (lifecycle == ComponentLifecycle::kOffline)
      simulator.reason = ComponentStateReason{"simulator_unreachable", "The simulator doesn't answer."};
    if (lifecycle == ComponentLifecycle::kFault) {
      Fault fault;
      fault.code = FaultCode::kProcessExited;
      fault.message = "The simulator process exited.";
      simulator.fault = fault;
    }
    EXPECT_TRUE(latch_exempt(simulator, {simulator}));
    EXPECT_TRUE(cell_latch_causes({simulator}).empty());
  }
  for (const auto* reason : {"simulation_paused", "simulation_stopped"}) {
    auto simulator = Observed(ComponentLifecycle::kPrepared, ComponentActivity::kIdle);
    simulator.component_id = "isaac_sim";
    simulator.capabilities = {Capability::kSimReset};
    simulator.reason = ComponentStateReason{reason, "The simulation isn't playing."};
    EXPECT_TRUE(cell_latch_causes({simulator}).empty());
  }

  auto owner = Observed(ComponentLifecycle::kFault, ComponentActivity::kIdle);
  owner.component_id = "robot_owner";
  owner.fault = MachineFault();
  auto arm = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
  arm.component_id = "robot_arm";
  arm.owner_id = "robot_owner";
  arm.reason = ComponentStateReason{std::string(kOwnerUnavailableReason), "through robot_owner: C2"};
  auto simulator = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
  simulator.component_id = "isaac_sim";
  simulator.capabilities = {Capability::kSimReset};
  simulator.reason = ComponentStateReason{"simulator_unreachable", "The simulator doesn't answer."};

  const std::vector states{simulator, arm, owner};
  EXPECT_TRUE(latch_exempt(simulator, states));
  EXPECT_FALSE(latch_exempt(arm, states));
  const auto causes = cell_latch_causes(states);
  ASSERT_EQ(causes.size(), 1U);
  EXPECT_EQ(causes[0].component_id, "robot_owner");
  EXPECT_EQ(causes[0].kind, CellLatchCauseKind::kFault);
}

TEST(CellLatchCauses, SimulatorExemptionDoesNotExemptOtherMembersOfSharedOwner) {
  for (const auto lifecycle : {ComponentLifecycle::kFault,
                               ComponentLifecycle::kOffline}) {
    SCOPED_TRACE(std::string(to_string(lifecycle)));
    auto owner = Observed(lifecycle, ComponentActivity::kIdle);
    owner.component_id = "robot_owner";
    if (lifecycle == ComponentLifecycle::kFault) {
      owner.fault = MachineFault();
    } else {
      owner.reason = ComponentStateReason{"process_exited", "The owner exited."};
    }

    auto simulator = Observed(ComponentLifecycle::kOffline,
                              ComponentActivity::kIdle);
    simulator.component_id = "isaac_sim";
    simulator.owner_id = owner.component_id;
    simulator.capabilities = {Capability::kSimReset};
    simulator.reason = ComponentStateReason{
        std::string(kOwnerUnavailableReason), "through robot_owner: unavailable"};

    auto arm = Observed(ComponentLifecycle::kOffline, ComponentActivity::kIdle);
    arm.component_id = "isaac_arm";
    arm.owner_id = owner.component_id;
    arm.reason = ComponentStateReason{
        std::string(kOwnerUnavailableReason), "through robot_owner: unavailable"};

    const std::vector states{simulator, arm, owner};
    EXPECT_TRUE(latch_exempt(simulator, states));
    EXPECT_FALSE(latch_exempt(arm, states));
    const auto causes = cell_latch_causes(states);
    ASSERT_EQ(causes.size(), 1U);
    EXPECT_EQ(causes[0].component_id, "robot_owner");
    EXPECT_EQ(causes[0].kind,
              lifecycle == ComponentLifecycle::kFault
                  ? CellLatchCauseKind::kFault
                  : CellLatchCauseKind::kOffline);
  }
}

TEST(ComponentCoordinator, MapsTheEightLifecycleStatesToFive) {
  using State = ComponentLifecycleStateDTO;
  EXPECT_EQ(MapComponentLifecycleState(State::kUndeclared, true), ComponentLifecycle::kNotPrepared);
  EXPECT_EQ(MapComponentLifecycleState(State::kDeclared, true), ComponentLifecycle::kNotPrepared);
  EXPECT_EQ(MapComponentLifecycleState(State::kDeclared, false), ComponentLifecycle::kOffline);
  EXPECT_EQ(MapComponentLifecycleState(State::kPrepared, true), ComponentLifecycle::kPrepared);
  EXPECT_EQ(MapComponentLifecycleState(State::kActive, true), ComponentLifecycle::kEnabled);
  EXPECT_EQ(MapComponentLifecycleState(State::kDegraded, true), ComponentLifecycle::kFault);
  EXPECT_EQ(MapComponentLifecycleState(State::kFaulted, true), ComponentLifecycle::kFault);
  EXPECT_EQ(MapComponentLifecycleState(State::kDisabling, true), ComponentLifecycle::kPrepared);
  EXPECT_EQ(MapComponentLifecycleState(State::kDisabled, true), ComponentLifecycle::kPrepared);
}

}  // namespace
}  // namespace manyforge::core

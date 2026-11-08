// The generic simulator component's state and actions.
#include <gtest/gtest.h>

#include <manyforge_roboplan/simulator_component_adapter.hpp>

#include <memory>
#include <optional>
#include <vector>

namespace manyforge::roboplan_backend {
namespace {

class FakeSimulation final : public ISimulationControl {
 public:
  std::optional<SimulationState> observed;
  core::ActionOutcome next{core::ActionOutcome::kSucceeded};
  std::vector<SimulationState> set;
  int resets{};

  std::optional<SimulationState> Observed() const override { return observed; }
  SimulationCall Read(std::stop_token) override { return answer(); }
  SimulationCall Set(const SimulationState state, std::stop_token) override {
    set.push_back(state);
    if (next == core::ActionOutcome::kSucceeded) observed = state;
    return answer();
  }
  SimulationCall Reset(std::stop_token) override {
    ++resets;
    return answer();
  }

 private:
  SimulationCall answer() const {
    if (next == core::ActionOutcome::kSucceeded) return {next, std::nullopt};
    return {next, core::ActionError{next == core::ActionOutcome::kUnknown
                                        ? "simulation_no_response"
                                        : "simulation_request_failed",
                                    "x", std::nullopt, std::nullopt}};
  }
};

TEST(SimulatorComponentState, PlayingIsEnabled) {
  const auto state = SimulatorComponentState(SimulationState::kPlaying, 7);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_FALSE(state.reason.has_value());
  EXPECT_FALSE(state.fault.has_value());
  EXPECT_EQ(state.activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(state.observed_at_ns, 7);
}

TEST(SimulatorComponentState, PausedAndStoppedArePreparedWithReasonNeverFault) {
  const auto paused = SimulatorComponentState(SimulationState::kPaused, 0);
  EXPECT_EQ(paused.lifecycle, core::ComponentLifecycle::kPrepared);
  ASSERT_TRUE(paused.reason.has_value());
  EXPECT_EQ(paused.reason->code, kSimulationPausedReason);
  EXPECT_FALSE(paused.fault.has_value());
  const auto stopped = SimulatorComponentState(SimulationState::kStopped, 0);
  EXPECT_EQ(stopped.lifecycle, core::ComponentLifecycle::kPrepared);
  ASSERT_TRUE(stopped.reason.has_value());
  EXPECT_EQ(stopped.reason->code, kSimulationStoppedReason);
}

TEST(SimulatorComponentState, UnreachableIsOffline) {
  const auto state = SimulatorComponentState(std::nullopt, 0);
  EXPECT_EQ(state.lifecycle, core::ComponentLifecycle::kOffline);
  ASSERT_TRUE(state.reason.has_value());
  EXPECT_EQ(state.reason->code, kSimulatorUnreachableReason);
  EXPECT_EQ(SimulatorComponentState(SimulationState::kPlaying, 0).activity,
            core::ComponentActivity::kIdle);
}

TEST(SimulatorComponentAdapter, ActionsDriveTheSimulator) {
  auto simulation = std::make_shared<FakeSimulation>();
  simulation->observed = SimulationState::kStopped;
  SimulatorComponentAdapter adapter(simulation);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(1).lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kDisable, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(simulation->set,
            (std::vector<SimulationState>{SimulationState::kPlaying, SimulationState::kPaused}));
  EXPECT_EQ(adapter.Run(core::ComponentAction::kSimReset, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(simulation->resets, 1);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kPrepare, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.capabilities(),
            (std::vector<core::Capability>{core::Capability::kSimReset}));
}

TEST(SimulatorComponentAdapter, RefusalIsATypedFailure) {
  auto simulation = std::make_shared<FakeSimulation>();
  simulation->observed = SimulationState::kPaused;
  simulation->next = core::ActionOutcome::kFailed;
  SimulatorComponentAdapter adapter(simulation);
  const auto result = adapter.Run(core::ComponentAction::kEnable, {});
  EXPECT_EQ(result.outcome, core::ActionOutcome::kFailed);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "simulation_request_failed");
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kResetPosition, {}).error->code,
            "component_action_unsupported");
}

TEST(SimulatorComponentAdapter, UnansweredPlayIsOnlyAnUnknownActionResultAndIsNotResent) {
  auto simulation = std::make_shared<FakeSimulation>();
  simulation->observed = SimulationState::kPaused;
  simulation->next = core::ActionOutcome::kUnknown;
  SimulatorComponentAdapter adapter(simulation);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kEnable, {}).outcome,
            core::ActionOutcome::kUnknown);
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(simulation->set.size(), 1U);  // never resent
  simulation->observed = SimulationState::kPlaying;
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
}

TEST(SimulatorComponentAdapter, UnansweredResetIsOnlyAnUnknownActionResult) {
  auto simulation = std::make_shared<FakeSimulation>();
  simulation->observed = SimulationState::kPlaying;
  simulation->next = core::ActionOutcome::kUnknown;
  SimulatorComponentAdapter adapter(simulation);
  EXPECT_EQ(adapter.Run(core::ComponentAction::kSimReset, {}).outcome,
            core::ActionOutcome::kUnknown);
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  simulation->next = core::ActionOutcome::kSucceeded;
  EXPECT_EQ(adapter.Run(core::ComponentAction::kRecover, {}).outcome,
            core::ActionOutcome::kSucceeded);
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
}

TEST(SimulatorComponentAdapter, FailedRecoverDoesNotInventAnUnknownState) {
  auto simulation = std::make_shared<FakeSimulation>();
  simulation->observed = SimulationState::kPlaying;
  simulation->next = core::ActionOutcome::kUnknown;
  SimulatorComponentAdapter adapter(simulation);
  ASSERT_EQ(adapter.Run(core::ComponentAction::kSimReset, {}).outcome,
            core::ActionOutcome::kUnknown);
  ASSERT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  simulation->next = core::ActionOutcome::kFailed;
  EXPECT_EQ(adapter.Run(core::ComponentAction::kRecover, {}).outcome,
            core::ActionOutcome::kFailed);
  EXPECT_EQ(adapter.State(0).activity, core::ComponentActivity::kIdle);
  EXPECT_EQ(adapter.State(0).lifecycle, core::ComponentLifecycle::kEnabled);
  EXPECT_EQ(simulation->resets, 1);
  EXPECT_TRUE(simulation->set.empty());
}

}  // namespace
}  // namespace manyforge::roboplan_backend

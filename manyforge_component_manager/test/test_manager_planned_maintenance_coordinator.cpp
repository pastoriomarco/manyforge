#include <gtest/gtest.h>

#include <manyforge_component_manager/manager_planned_maintenance_coordinator.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>

#include <map>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestThree =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

[[nodiscard]] RuntimeUnitConvergencePlanRowDTO
ManagedRow(const std::string &runtime_unit_id,
           std::vector<RuntimeUnitId> dependencies = {}) {
  const auto &registry =
      manyforge::deployment::runtime_contract_declaration_registry();
  if (registry.contracts().empty()) {
    throw std::runtime_error("active runtime test registry is empty");
  }
  const auto &qualified = registry.contracts().front();
  const auto requirement = MakeRuntimeContractRequirement(
      qualified.offer().runtime_implementation_id, {}, {});
  if (!requirement.ok()) {
    throw std::runtime_error("active runtime test requirement is invalid");
  }
  return {
      RuntimeUnitId(runtime_unit_id),
      qualified.runtime_profile_id(),
      qualified.runtime_profile_revision(),
      manyforge::core::RuntimeLifecycleControl::kManagerManaged,
      RuntimeUnitCandidateDisposition::kAdded,
      ProductId("runtime_product_" + runtime_unit_id),
      qualified.runtime_product_id(),
      qualified.runtime_product_revision(),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      std::move(dependencies),
      {RuntimeRoleId("manager_fixture")},
      EndpointRoleId("status"),
      "manyforge.reference-runtime-status",
      "status-revision-1",
      "private-controller",
      "manyforge.posix_sh.retained_script",
      "adapter-revision-1",
      "retained-raw-artifact",
      "retrieval-revision-1",
      ContentDigest(std::string(kDigestThree)),
      1U,
      "linux",
      "posix.1-2017",
      requirement.value(),
      qualified.offer().offer_digest,
  };
}

[[nodiscard]] ManagerActiveRuntimePlanDTO ThreeUnitPlan() {
  const ProjectionRevisionDTO projection{
      RetainedBundleId("bundle.active"),
      GraphRevision("graph.active"),
      SceneRevision{SceneId(std::string(kDigestThree)), 2U,
                    ContentDigest(std::string(kDigestOne))},
  };
  ManagerActiveRuntimePlanDTO plan{
      ActiveBundlePublicationReceiptDTO{
          ActiveBundlePublicationRequestDTO{
              NamedApplyPublicationTransactionDTO{
                  ApplyId("apply.active"), ApplyGeneration(1U),
                  ProjectionStageGeneration(1U)},
              projection,
              projection.retained_bundle_id,
              projection.graph_revision,
              ActiveBundlePublicationGeneration(1U),
              ContentDigest(std::string(kDigestTwo)),
              ContentDigest(std::string(kDigestThree)),
          },
          ActiveBundlePublicationGeneration(2U),
          ContentDigest(std::string(kDigestOne)),
      },
      ContentDigest(std::string(kDigestThree)),
      {
          ManagedRow("runtime.a"),
          ManagedRow("runtime.b", {RuntimeUnitId("runtime.a")}),
          ManagedRow("runtime.c", {RuntimeUnitId("runtime.b")}),
      },
  };
  if (!plan.valid()) {
    throw std::runtime_error("planned maintenance test plan is invalid");
  }
  return plan;
}

/// Returns the admitted plan on every read. `read_calls` proves how often the
/// transition rechecked the active configuration.
class FixedPlanSource final : public IManagerActiveRuntimePlanSource {
public:
  explicit FixedPlanSource(ManagerActiveRuntimePlanDTO plan)
      : plan_(std::move(plan)) {}

  [[nodiscard]] ManagerActiveRuntimePlanDTO ReadCurrentPlan() const override {
    ++read_calls;
    return plan_;
  }

  mutable int read_calls{0};

private:
  ManagerActiveRuntimePlanDTO plan_;
};

/// Returns a different plan after the first read, modelling a configuration
/// that changed between admission and teardown.
class PlanChangingAfterFirstReadSource final
    : public IManagerActiveRuntimePlanSource {
public:
  PlanChangingAfterFirstReadSource(ManagerActiveRuntimePlanDTO admitted,
                                   ManagerActiveRuntimePlanDTO changed)
      : admitted_(std::move(admitted)), changed_(std::move(changed)) {}

  [[nodiscard]] ManagerActiveRuntimePlanDTO ReadCurrentPlan() const override {
    return ++read_calls == 1 ? admitted_ : changed_;
  }

  mutable int read_calls{0};

private:
  ManagerActiveRuntimePlanDTO admitted_;
  ManagerActiveRuntimePlanDTO changed_;
};

class NoManagedProcessPort final : public IRuntimeUnitProcessPort,
                                   public IRuntimeUnitProgressPort {
public:
  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("observe:" + id);
    return Presence(query, id);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("start:" + id);
    ++start_calls[id];
    epochs[id] = "epoch-" + id;
    return Presence(query, id);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("stop:" + id);
    ++stop_calls[id];
    if (refuse_stop_for == id) {
      refuse_stop_for.clear();
      throw std::runtime_error("simulated refused stop");
    }
    epochs.erase(id);
    if (throw_after_stop_effect_for == id) {
      throw_after_stop_effect_for.clear();
      throw std::runtime_error("simulated lost stop response");
    }
    return Presence(query, id);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("progress:" + id);
    const auto found = epochs.find(id);
    if (found == epochs.end() || found->second != runtime_epoch.value()) {
      return std::nullopt;
    }
    std::vector<std::string> roles;
    for (const auto &role : query.plan.required_runtime_role_ids) {
      roles.push_back(role.value());
    }
    return RuntimeUnitSourceReport(
        id, found->second, 1U, 1U, RuntimeSourcePhase::kServing,
        RuntimeSourceHealth::kHealthy, query.plan.product_identity.value(),
        query.plan.configuration_identity.value(), std::move(roles),
        ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        query.plan.runtime_contract_offer_digest.value());
  }

  std::vector<std::string> events;
  std::map<std::string, int> start_calls;
  std::map<std::string, int> stop_calls;
  std::map<std::string, std::string> epochs;
  std::string refuse_stop_for;
  std::string throw_after_stop_effect_for;

private:
  [[nodiscard]] ProcessPresenceObservation
  Presence(const RuntimeUnitConvergenceQueryDTO &query, const std::string &id) {
    const auto found = epochs.find(id);
    return ProcessPresenceObservation(
        query.plan.runtime_unit_id.value(), "planned-maintenance-observer",
        ++observation_sequences[id],
        found == epochs.end() ? ProcessPresenceState::kAbsent
                              : ProcessPresenceState::kPresent,
        found == epochs.end() ? "" : found->second);
  }

  std::map<std::string, std::uint64_t> observation_sequences;
};

/// Scripted admission owner. `events` records the exact wall-clock-free order
/// in which the transition talks to admission and to the runtime.
class ScriptedGate final : public IPlannedMaintenanceGate {
public:
  explicit ScriptedGate(ManagementIdleState state) : state_(state) {}

  [[nodiscard]] ManagementIdleState ReadIdleState() const override {
    ++idle_reads;
    return state_;
  }

  [[nodiscard]] bool CloseAdmissionAndProveClosed() override {
    events->push_back("close_admission");
    ++close_calls;
    return close_succeeds;
  }

  ManagementIdleState state_;
  bool close_succeeds{true};
  mutable int idle_reads{0};
  int close_calls{0};
  std::vector<std::string> *events{nullptr};
};

[[nodiscard]] ManagerApplyIntentDTO Intent(
    std::string apply_id = "apply-1",
    const std::uint64_t generation = 1U) {
  return {
      PublicRequestContextDTO{
          RequestId("request-1"),
          RequestOwnerEpoch("owner-epoch-1"),
          "component-manager-test",
          std::optional<std::string>("correlation-1"),
      },
      ApplyId(std::move(apply_id)),
      ApplyGeneration(generation),
      CellId("cell-1"),
      ProjectionRevisionDTO{
          RetainedBundleId("bundle.active"),
          GraphRevision("graph.active"),
          SceneRevision{
              SceneId(std::string(kDigestThree)),
              2U,
              ContentDigest(std::string(kDigestOne)),
          },
      },
      RetainedBundleId("bundle.candidate"),
      GraphRevision("graph.candidate"),
      ManagerApplyPreparedActivationDTO{
          ContentDigest(std::string(kDigestOne)),
          core::DeploymentArtifactId("deployment-artifact-1"),
          core::DeploymentRevision(std::string(kDigestTwo)),
          core::ProgramBindingId(std::string(kDigestThree)),
          ContentDigest(std::string(kDigestTwo)),
      },
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
}

/// One complete planned-transition fixture. The runtime starts with every
/// affected content present so that a transition has something to stop.
class MaintenanceFixture final {
public:
  MaintenanceFixture()
      : plan_(ThreeUnitPlan()), source_(plan_),
        runtime_(source_, ports_, ports_, [this] { return ++now_; }),
        gate_(ManagementIdleState::kIdle),
        coordinator_(source_, runtime_, gate_) {
    gate_.events = &events;
    static_cast<void>(runtime_.Initialize());
    ports_.events.clear();
    events.clear();
  }

  [[nodiscard]] ManagerPlannedMaintenanceResult Enter(
      const manyforge::core::ProjectionRevisionDTO *predecessor = nullptr,
      const ManagerApplyIntentDTO *intent = nullptr) {
    const auto accepted = intent == nullptr ? Intent() : *intent;
    const auto &expected =
        predecessor == nullptr ? accepted.expected_active_projection_revision
                               : *predecessor;
    auto result = coordinator_.EnterIdleMaintenance(accepted, expected);
    for (const auto &event : ports_.events) {
      events.push_back(event);
    }
    return result;
  }

  ManagerActiveRuntimePlanDTO plan_;
  FixedPlanSource source_;
  NoManagedProcessPort ports_;
  ManagerActiveRuntimeCoordinator runtime_;
  ScriptedGate gate_;
  ManagerPlannedMaintenanceCoordinator coordinator_;
  std::vector<std::string> events;
  std::uint64_t now_{0};
};

/// The runtime is already converged when the fixture is constructed, so a
/// transition must stop all three contents in reverse dependency order.
TEST(ManagerPlannedMaintenanceCoordinator,
     IdleTransitionStopsInReverseDependencyOrderAndProvesAbsence) {
  MaintenanceFixture fixture;

  const auto result = fixture.Enter();

  EXPECT_TRUE(result.entered_idle_maintenance());
  EXPECT_TRUE(result.valid());
  ASSERT_EQ(result.final_process_observations.size(), 3U);
  for (const auto &observation : result.final_process_observations) {
    EXPECT_EQ(observation.state(), ProcessPresenceState::kAbsent);
  }
  EXPECT_TRUE(fixture.ports_.epochs.empty());
  EXPECT_EQ(fixture.events, (std::vector<std::string>{
                               "close_admission",
                               "observe:runtime.c",
                               "stop:runtime.c",
                               "observe:runtime.b",
                               "stop:runtime.b",
                               "observe:runtime.a",
                               "stop:runtime.a",
                           }));
  // Planned teardown never constructs a replacement on its own.
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
  EXPECT_EQ(fixture.gate_.close_calls, 1);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     ActiveExecutionIsRefusedBeforeAnyProcessEffect) {
  MaintenanceFixture fixture;
  fixture.gate_.state_ = ManagementIdleState::kActive;

  const auto result = fixture.Enter();

  EXPECT_EQ(result.outcome,
            ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect);
  EXPECT_EQ(result.failure_code, "apply_refused_active_execution");
  // Nothing happened, so every effect of the attempt is known.
  EXPECT_EQ(result.effect_disposition, ManagerApplyEffectDisposition::kKnown);
  EXPECT_TRUE(result.valid());
  EXPECT_TRUE(result.final_process_observations.empty());
  EXPECT_TRUE(fixture.events.empty());
  EXPECT_TRUE(fixture.ports_.stop_calls.empty());
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
  EXPECT_EQ(fixture.gate_.close_calls, 0);
  // The predecessor was not even re-read: refusal precedes every other step.
  EXPECT_EQ(fixture.source_.read_calls, 0);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     PreflightRefusalDoesNotAcquireOrCloseAdmission) {
  MaintenanceFixture fixture;
  fixture.gate_.state_ = ManagementIdleState::kActive;
  const auto intent = Intent();

  const auto refusal = fixture.coordinator_.Preflight(
      intent, intent.expected_active_projection_revision);

  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->failure_code, "apply_refused_active_execution");
  EXPECT_EQ(refusal->effect_disposition,
            ManagerApplyEffectDisposition::kKnown);
  EXPECT_EQ(fixture.gate_.close_calls, 0);
  EXPECT_TRUE(fixture.events.empty());
  EXPECT_TRUE(fixture.ports_.stop_calls.empty());
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     PausedStoppingAndResettingAreEachRefusedByName) {
  const std::map<ManagementIdleState, std::string> expected{
      {ManagementIdleState::kStarting, "apply_refused_starting_execution"},
      {ManagementIdleState::kPaused, "apply_refused_paused_execution"},
      {ManagementIdleState::kStopping, "apply_refused_stopping_execution"},
      {ManagementIdleState::kTerminalAwaitingReset,
       "apply_refused_terminal_awaiting_reset"},
      {ManagementIdleState::kInterventionOrRecovery,
       "apply_refused_intervention_or_recovery"},
      {ManagementIdleState::kResetting, "apply_refused_resetting_execution"},
      {ManagementIdleState::kRecoveryRequired,
       "apply_refused_recovery_required"},
      {ManagementIdleState::kUnavailable,
       "apply_refused_cycle_status_unavailable"},
  };
  for (const auto &[state, code] : expected) {
    MaintenanceFixture fixture;
    fixture.gate_.state_ = state;

    const auto result = fixture.Enter();

    EXPECT_EQ(result.failure_code, code) << ToString(state);
    EXPECT_EQ(result.effect_disposition,
              ManagerApplyEffectDisposition::kKnown)
        << ToString(state);
    EXPECT_TRUE(fixture.events.empty()) << ToString(state);
    EXPECT_TRUE(fixture.ports_.stop_calls.empty()) << ToString(state);
  }
}

TEST(ManagerPlannedMaintenanceCoordinator,
     AdmissionClosesOnlyAfterThePredecessorIsRechecked) {
  MaintenanceFixture fixture;

  const auto result = fixture.Enter();

  ASSERT_TRUE(result.entered_idle_maintenance());
  // The exact ordering: idle refusal, predecessor recheck, admission close,
  // then teardown. Each step completes before the next begins.
  ASSERT_GE(fixture.events.size(), 7U);
  EXPECT_EQ(fixture.events[0], "close_admission");
  EXPECT_EQ(fixture.events[1], "observe:runtime.c");
  EXPECT_EQ(fixture.gate_.idle_reads, 1);
  EXPECT_EQ(fixture.source_.read_calls, 1);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     StalePredecessorIsRejectedBeforeAnyEffect) {
  MaintenanceFixture fixture;
  const auto replacement = ThreeUnitPlan();
  PlanChangingAfterFirstReadSource changing(ThreeUnitPlan(), replacement);
  ManagerActiveRuntimeCoordinator runtime(changing, fixture.ports_,
                                          fixture.ports_,
                                          [&fixture] { return ++fixture.now_; });
  static_cast<void>(runtime.Initialize());
  ScriptedGate gate(ManagementIdleState::kIdle);
  gate.events = &fixture.events;
  ManagerPlannedMaintenanceCoordinator coordinator(changing, runtime, gate);
  fixture.events.clear();
  fixture.ports_.events.clear();

  const auto intent = Intent();
  const auto result = coordinator.EnterIdleMaintenance(
      intent, intent.expected_active_projection_revision);

  EXPECT_EQ(result.failure_code, "planned_maintenance_predecessor_stale");
  EXPECT_EQ(result.effect_disposition, ManagerApplyEffectDisposition::kKnown);
  EXPECT_TRUE(fixture.ports_.stop_calls.empty());
  EXPECT_EQ(gate.close_calls, 0);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     MismatchedIntentPredecessorIsRejectedWithoutAnyEffect) {
  MaintenanceFixture fixture;
  const auto intent = Intent();
  auto foreign = intent.expected_active_projection_revision;
  foreign.graph_revision = GraphRevision("graph.other");

  const auto result = fixture.Enter(&foreign, &intent);

  EXPECT_EQ(result.failure_code, "planned_maintenance_predecessor_mismatch");
  EXPECT_TRUE(fixture.events.empty());
  EXPECT_EQ(fixture.source_.read_calls, 0);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     AnInvalidIntentIsRejectedWithoutAnyEffect) {
  MaintenanceFixture fixture;
  auto intent = Intent();
  intent.candidate_graph_revision = GraphRevision("");

  const auto result = fixture.Enter(nullptr, &intent);

  EXPECT_EQ(result.failure_code, "planned_maintenance_intent_invalid");
  EXPECT_TRUE(fixture.events.empty());
  EXPECT_TRUE(fixture.ports_.stop_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     UnprovenAdmissionClosureLeavesTheRuntimeUntouched) {
  MaintenanceFixture fixture;
  fixture.gate_.close_succeeds = false;

  const auto result = fixture.Enter();

  EXPECT_EQ(result.failure_code, "planned_maintenance_admission_not_closed");
  EXPECT_EQ(result.effect_disposition, ManagerApplyEffectDisposition::kKnown);
  EXPECT_EQ(fixture.events, (std::vector<std::string>{"close_admission"}));
  EXPECT_TRUE(fixture.ports_.stop_calls.empty());
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     UncertainStopIsReportedAsUnknownEffectWithoutRetry) {
  MaintenanceFixture fixture;
  fixture.ports_.throw_after_stop_effect_for = "runtime.c";

  const auto result = fixture.Enter();

  EXPECT_EQ(result.outcome,
            ManagerPlannedMaintenanceOutcome::kUnknownEffect);
  EXPECT_EQ(result.failure_code, "planned_maintenance_stop_uncertain");
  EXPECT_EQ(result.effect_disposition,
            ManagerApplyEffectDisposition::kUnknown);
  EXPECT_TRUE(result.valid());
  // The stop may have taken effect, so absence is not claimed and no second
  // attempt, rollback or replacement is performed.
  EXPECT_TRUE(result.final_process_observations.empty());
  EXPECT_EQ(fixture.ports_.stop_calls.at("runtime.c"), 1);
  EXPECT_EQ(fixture.ports_.stop_calls.count("runtime.b"), 0);
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     FailedTeardownIsReportedAsUnknownEffect) {
  MaintenanceFixture fixture;
  fixture.ports_.refuse_stop_for = "runtime.c";

  const auto result = fixture.Enter();

  EXPECT_EQ(result.outcome,
            ManagerPlannedMaintenanceOutcome::kUnknownEffect);
  EXPECT_EQ(result.failure_code, "planned_maintenance_teardown_failed");
  EXPECT_EQ(result.effect_disposition,
            ManagerApplyEffectDisposition::kUnknown);
  EXPECT_EQ(fixture.ports_.stop_calls.at("runtime.c"), 1);
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     PlannedTeardownDoesNotInvokeCrashRecoveryOrReplacement) {
  MaintenanceFixture fixture;
  // A content observed absent outside a planned transition is contained fault
  // evidence: the read surface must report it and must never restart it.
  static_cast<void>(fixture.ports_.EnsureStopped(RuntimeUnitConvergenceQueryDTO{
      ApplyId("apply.active"),
      ApplyGeneration(1U),
      RetainedBundleId("bundle.active"),
      GraphRevision("graph.active"),
      ContentDigest(std::string(kDigestThree)),
      ManagedRow("runtime.c"),
  }));
  fixture.ports_.events.clear();

  const auto observed = fixture.runtime_.Observe();
  EXPECT_EQ(observed.state, ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_TRUE(fixture.ports_.start_calls.empty());

  // And the planned transition itself only ever stops: it never reaches crash
  // reconciliation, retry, rollback or replacement.
  const auto result = fixture.Enter();
  EXPECT_TRUE(result.entered_idle_maintenance());
  EXPECT_TRUE(fixture.ports_.start_calls.empty());
}

TEST(ManagerPlannedMaintenanceCoordinator,
     ReplacementConstructionUsesTheInitializationPath) {
  MaintenanceFixture fixture;

  const auto entered = fixture.Enter();
  ASSERT_TRUE(entered.entered_idle_maintenance());

  // After the transition proved absence, the replacement contents are
  // constructed through the effectful initialization path.
  const auto initialized = fixture.runtime_.Initialize();
  EXPECT_EQ(initialized.state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(fixture.ports_.start_calls.at("runtime.a"), 1);
  EXPECT_EQ(fixture.ports_.start_calls.at("runtime.b"), 1);
  EXPECT_EQ(fixture.ports_.start_calls.at("runtime.c"), 1);
}

TEST(ManagerPlannedMaintenanceCoordinator,
     ResultValidityMatchesEveryOutcome) {
  MaintenanceFixture fixture;
  const auto entered = fixture.Enter();
  EXPECT_TRUE(entered.valid());

  ManagerPlannedMaintenanceResult refused;
  refused.outcome = ManagerPlannedMaintenanceOutcome::kFailedBeforeEffect;
  refused.failure_code = "some_code";
  refused.failure_message = "some message";
  refused.effect_disposition = ManagerApplyEffectDisposition::kKnown;
  EXPECT_TRUE(refused.valid());
  // A known pre-effect failure may not claim unknown effects.
  refused.effect_disposition = ManagerApplyEffectDisposition::kUnknown;
  EXPECT_FALSE(refused.valid());
  refused.effect_disposition = ManagerApplyEffectDisposition::kKnown;
  refused.failure_message.clear();
  EXPECT_FALSE(refused.valid());

  ManagerPlannedMaintenanceResult uncertain;
  uncertain.outcome = ManagerPlannedMaintenanceOutcome::kUnknownEffect;
  uncertain.failure_code = "some_code";
  uncertain.failure_message = "some message";
  uncertain.effect_disposition = ManagerApplyEffectDisposition::kUnknown;
  EXPECT_TRUE(uncertain.valid());
  uncertain.effect_disposition = ManagerApplyEffectDisposition::kKnown;
  EXPECT_FALSE(uncertain.valid());

  // A successful entry carries no failure evidence.
  auto succeeded = entered;
  succeeded.failure_code = "unexpected";
  succeeded.failure_message = "unexpected";
  EXPECT_FALSE(succeeded.valid());

  // Success is evidence that every retired predecessor process is absent,
  // not merely that Shutdown returned a syntactically complete observation.
  auto predecessor_still_present = entered;
  ASSERT_FALSE(predecessor_still_present.final_process_observations.empty());
  const auto &observed =
      predecessor_still_present.final_process_observations.front();
  predecessor_still_present.final_process_observations.front() =
      ProcessPresenceObservation(
          observed.runtime_unit_id(), observed.observer_instance_id(),
          observed.observation_sequence(), ProcessPresenceState::kPresent,
          "predecessor-epoch");
  EXPECT_FALSE(predecessor_still_present.valid());

  EXPECT_EQ(std::string(ToString(ManagementIdleState::kIdle)), "idle");
  EXPECT_EQ(std::string(ToString(ManagementIdleState::kPaused)), "paused");
  EXPECT_EQ(std::string(ToString(ManagementIdleState::kResetting)),
            "resetting");
}

} // namespace
} // namespace manyforge::component_manager

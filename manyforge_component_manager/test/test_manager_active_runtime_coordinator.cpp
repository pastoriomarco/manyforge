#include <gtest/gtest.h>

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>

#include <algorithm>
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
    throw std::runtime_error("active runtime test plan is invalid");
  }
  return plan;
}

[[nodiscard]] ManagerActiveRuntimePlanDTO BootstrapHostPlan() {
  const ProjectionRevisionDTO projection{
      RetainedBundleId("bundle.bootstrap"), GraphRevision("graph.bootstrap"),
      SceneRevision{SceneId(std::string(kDigestThree)), 1U,
                    ContentDigest(std::string(kDigestOne))},
  };
  ManagerActiveRuntimePlanDTO plan{
      ActiveBundlePublicationReceiptDTO{
          ActiveBundlePublicationRequestDTO{
              BootstrapPublicationTransactionDTO{
                  BootstrapId("bootstrap.active"), BootstrapGeneration(1U)},
              projection,
              projection.retained_bundle_id,
              projection.graph_revision,
              std::nullopt,
              ContentDigest(std::string(kDigestTwo)),
              ContentDigest(std::string(kDigestThree)),
          },
          ActiveBundlePublicationGeneration(1U),
          ContentDigest(std::string(kDigestOne)),
      },
      ContentDigest(std::string(kDigestThree)),
      {ManagedRow("runtime.bootstrap")},
  };
  if (!plan.valid()) {
    throw std::runtime_error("bootstrap runtime test plan is invalid");
  }
  return plan;
}

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

/// Returns one admitted plan on its first read and a different plan on every
/// later read, proving the coordinator re-validates the active plan instead of
/// applying its result against a plan that changed underneath it.
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

class ScriptedLifecyclePorts final : public IRuntimeUnitProcessPort,
                                     public IRuntimeUnitProgressPort {
public:
  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("observe:" + id);
    observed_owners.push_back(query.owner);
    return Presence(query, id);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("start:" + id);
    ++start_calls[id];
    const auto next = ++epoch_generations[id];
    epochs[id] = "epoch-" + id + "-" + std::to_string(next);
    report_sequences[id] = 0U;
    if (throw_after_start_effect == id) {
      throw_after_start_effect.clear();
      throw std::runtime_error("simulated lost start response");
    }
    return Presence(query, id);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureStopped(const RuntimeUnitConvergenceQueryDTO &query) override {
    const auto id = query.plan.runtime_unit_id.value();
    events.push_back("stop:" + id);
    ++stop_calls[id];
    epochs.erase(id);
    if (throw_after_stop_effect == id) {
      throw_after_stop_effect.clear();
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
    auto &sequence = report_sequences[id];
    if (!replay_last_report || sequence == 0U) {
      ++sequence;
    }
    return RuntimeUnitSourceReport(
        id, found->second, sequence, 1U, RuntimeSourcePhase::kServing,
        RuntimeSourceHealth::kHealthy, query.plan.product_identity.value(),
        query.plan.configuration_identity.value(), std::move(roles),
        ManagedCommandSurfaceState::kClosedNoActiveEffect, {},
        child_offer_digest_override.value_or(
            query.plan.runtime_contract_offer_digest.value()));
  }

  std::vector<std::string> events;
  std::vector<RuntimeUnitConvergenceOwnerDTO> observed_owners;
  std::map<std::string, int> start_calls;
  std::map<std::string, int> stop_calls;
  std::map<std::string, std::string> epochs;
  std::string throw_after_start_effect;
  std::string throw_after_stop_effect;
  std::optional<std::string> child_offer_digest_override;
  bool replay_last_report{false};

private:
  [[nodiscard]] ProcessPresenceObservation
  Presence(const RuntimeUnitConvergenceQueryDTO &query, const std::string &id) {
    const auto found = epochs.find(id);
    return ProcessPresenceObservation(
        query.plan.runtime_unit_id.value(), "active-runtime-observer",
        ++observation_sequences[id],
        found == epochs.end() ? ProcessPresenceState::kAbsent
                              : ProcessPresenceState::kPresent,
        found == epochs.end() ? "" : found->second);
  }

  std::map<std::string, std::uint64_t> observation_sequences;
  std::map<std::string, std::uint64_t> report_sequences;
  std::map<std::string, std::uint64_t> epoch_generations;
};

TEST(ManagerActiveRuntimeCoordinator,
     BootstrapPlanStartsHostUnderBootstrapIdentityWithoutFabricatedApply) {
  FixedPlanSource source(BootstrapHostPlan());
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 50U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                               [&now]() { return ++now; });

  const auto result = coordinator.Initialize();
  EXPECT_EQ(result.state, ManagerActiveRuntimeConvergenceState::kConverged);
  ASSERT_FALSE(ports.observed_owners.empty());
  const auto *owner = std::get_if<RuntimeUnitConvergenceBootstrapOwnerDTO>(
      &ports.observed_owners.front());
  ASSERT_NE(owner, nullptr);
  EXPECT_EQ(owner->bootstrap_id, BootstrapId("bootstrap.active"));
  EXPECT_EQ(owner->bootstrap_generation, BootstrapGeneration(1U));
  EXPECT_EQ(ports.start_calls["runtime.bootstrap"], 1);
  static_cast<void>(coordinator.Shutdown());
}

TEST(ManagerActiveRuntimeCoordinator,
     ReconcilesDependenciesStopsInReverseAndRestartsNewEpochs) {
  FixedPlanSource source(ThreeUnitPlan());
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 100U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  const auto first = coordinator.Initialize();
  EXPECT_EQ(first.state, ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.a",
                              "start:runtime.a",
                              "progress:runtime.a",
                              "observe:runtime.b",
                              "start:runtime.b",
                              "progress:runtime.b",
                              "observe:runtime.c",
                              "start:runtime.c",
                              "progress:runtime.c",
                          }));
  const auto first_epochs = ports.epochs;

  ports.events.clear();
  const auto replay = coordinator.Initialize();
  EXPECT_EQ(replay.state, ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  EXPECT_EQ(ports.start_calls["runtime.b"], 1);
  EXPECT_EQ(ports.start_calls["runtime.c"], 1);

  ports.events.clear();
  const auto stopped = coordinator.Shutdown();
  ASSERT_EQ(stopped.final_process_observations.size(), 3U);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.c",
                              "stop:runtime.c",
                              "observe:runtime.b",
                              "stop:runtime.b",
                              "observe:runtime.a",
                              "stop:runtime.a",
                          }));
  EXPECT_TRUE(ports.epochs.empty());

  ports.events.clear();
  EXPECT_EQ(coordinator.Initialize().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_NE(ports.epochs, first_epochs);
  static_cast<void>(coordinator.Shutdown());
}

TEST(ManagerActiveRuntimeCoordinator,
     LostStartAndStopResponsesRecoverObserveFirstWithoutDuplicateEffects) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 200U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  ports.throw_after_start_effect = "runtime.a";
  try {
    static_cast<void>(coordinator.Initialize());
    FAIL() << "expected uncertain start response";
  } catch (const ManagerActiveRuntimeProcessEffectUncertainError &error) {
    EXPECT_EQ(error.effect(), ManagerActiveRuntimeProcessEffect::kStart);
    EXPECT_EQ(error.recovery_query().plan.runtime_unit_id,
              RuntimeUnitId("runtime.a"));
  }
  EXPECT_EQ(coordinator.Initialize().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);

  ports.throw_after_stop_effect = "runtime.a";
  try {
    static_cast<void>(coordinator.Shutdown());
    FAIL() << "expected uncertain stop response";
  } catch (const ManagerActiveRuntimeProcessEffectUncertainError &error) {
    EXPECT_EQ(error.effect(), ManagerActiveRuntimeProcessEffect::kStop);
    EXPECT_EQ(error.recovery_query().plan.runtime_unit_id,
              RuntimeUnitId("runtime.a"));
  }
  const auto recovered = coordinator.Shutdown();
  ASSERT_EQ(recovered.final_process_observations.size(), 1U);
  EXPECT_EQ(recovered.final_process_observations.front().state(),
            ProcessPresenceState::kAbsent);
  EXPECT_EQ(ports.stop_calls["runtime.a"], 1);
}

TEST(ManagerActiveRuntimeCoordinator,
     UnchangedPlanRetainsSourceReplayLedgerAcrossReconciliationCalls) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 250U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  EXPECT_EQ(coordinator.Initialize().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  ports.replay_last_report = true;
  EXPECT_THROW(static_cast<void>(coordinator.Initialize()),
               ManagerActiveRuntimeCoordinatorError);
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  static_cast<void>(coordinator.Shutdown());
}

TEST(ManagerActiveRuntimeCoordinator,
     ChildOfferMismatchStaysUnavailableWithoutDuplicateStartEffect) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  ports.child_offer_digest_override = std::string(kDigestTwo);
  std::uint64_t now = 275U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  const auto first = coordinator.Initialize();
  EXPECT_EQ(first.state, ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.a",
                              "start:runtime.a",
                              "progress:runtime.a",
                          }));
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  ASSERT_EQ(first.observations.observations().size(), 1U);
  const auto &first_observation = first.observations.observations().front();
  EXPECT_EQ(first_observation.derived_state(),
            RuntimeConvergenceState::kNonconverged);
  EXPECT_EQ(first_observation.runtime_epoch(), ports.epochs["runtime.a"]);
  EXPECT_EQ(first_observation.observed_runtime_contract_offer_digest(),
            std::string(kDigestTwo));
  const std::vector<std::string> expected_reasons{
      "runtime_contract_offer_digest_mismatch",
  };
  EXPECT_EQ(first_observation.reason_codes(), expected_reasons);
  ASSERT_TRUE(first_observation.command_surface_state().has_value());
  EXPECT_EQ(*first_observation.command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);

  ports.events.clear();
  const auto replay = coordinator.Initialize();
  EXPECT_EQ(replay.state, ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.a",
                              "progress:runtime.a",
                          }));
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  static_cast<void>(coordinator.Shutdown());
}

TEST(ManagerActiveRuntimeCoordinator,
     ExternallyManagedAbsenceIsObservedButNeverStartedOrStopped) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  auto &row = plan.runtime_units.front();
  row.lifecycle_control =
      manyforge::core::RuntimeLifecycleControl::kExternallyManaged;
  row.retrieval_contract_id.clear();
  row.retrieval_contract_revision.clear();
  row.launchable_content_digest.reset();
  row.launchable_byte_size = 0U;
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 300U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  EXPECT_EQ(coordinator.Observe().state,
            ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_TRUE(ports.start_calls.empty());
  const auto stopped = coordinator.Shutdown();
  ASSERT_EQ(stopped.final_process_observations.size(), 1U);
  EXPECT_EQ(stopped.final_process_observations.front().state(),
            ProcessPresenceState::kAbsent);
  EXPECT_TRUE(ports.stop_calls.empty());
}

TEST(ManagerActiveRuntimeCoordinator,
     DependencyCycleRejectsBeforeAnyProcessObservation) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units[0].dependency_runtime_unit_ids = {
      RuntimeUnitId("runtime.c")};
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 400U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  EXPECT_THROW(static_cast<void>(coordinator.Initialize()),
               ManagerActiveRuntimeCoordinatorError);
  EXPECT_TRUE(ports.events.empty());
}

TEST(ManagerActiveRuntimeCoordinator,
     DiagnosticImplementationFamilyDoesNotBlockAnAdmittedSemanticContract) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  plan.runtime_units.front().runtime_contract_requirement =
      MakeRuntimeContractRequirement(RuntimeImplementationFamilyId(
                                         "unregistered.runtime.family"),
                                     {}, {})
          .value();
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 500U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                               [&now]() { return ++now; });

  EXPECT_EQ(coordinator.Initialize().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  static_cast<void>(coordinator.Shutdown());
}

TEST(ManagerActiveRuntimeCoordinator,
     PreStartGuardFailurePropagatesWithoutAnUncertainStart) {
  auto plan = ThreeUnitPlan();
  plan.runtime_units.erase(plan.runtime_units.begin() + 1U,
                           plan.runtime_units.end());
  ASSERT_TRUE(plan.valid());
  FixedPlanSource source(std::move(plan));
  ScriptedLifecyclePorts ports;
  bool guard_called = false;
  std::uint64_t now = 600U;
  ManagerActiveRuntimeCoordinator coordinator(
      source, ports, ports, [&now] { return ++now; },
      [&guard_called] {
        guard_called = true;
        throw std::runtime_error("test package generation replacement");
      });

  EXPECT_THROW(static_cast<void>(coordinator.Initialize()), std::runtime_error);
  EXPECT_TRUE(guard_called);
  EXPECT_TRUE(ports.start_calls.empty());
}

TEST(ManagerActiveRuntimeCoordinator,
     ObservationOfAnAbsentManagedUnitHasZeroProcessEffects) {
  FixedPlanSource source(ThreeUnitPlan());
  ScriptedLifecyclePorts ports;
  bool guard_called = false;
  std::uint64_t now = 700U;
  ManagerActiveRuntimeCoordinator coordinator(
      source, ports, ports, [&now] { return ++now; },
      [&guard_called] { guard_called = true; });

  const auto observed = coordinator.Observe();
  EXPECT_EQ(observed.state, ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.a",
                              "observe:runtime.b",
                              "observe:runtime.c",
                          }));
  EXPECT_TRUE(ports.start_calls.empty());
  EXPECT_TRUE(ports.stop_calls.empty());
  EXPECT_TRUE(ports.epochs.empty());
  EXPECT_FALSE(guard_called);
  ASSERT_EQ(observed.observations.observations().size(), 3U);
  for (const auto &observation : observed.observations.observations()) {
    EXPECT_FALSE(observation.converged());
    EXPECT_EQ(observation.derived_state(), RuntimeConvergenceState::kLost);
    EXPECT_EQ(observation.process_presence(), ProcessPresenceState::kAbsent);
    EXPECT_NE(std::find(observation.reason_codes().begin(),
                        observation.reason_codes().end(), "process_absent"),
              observation.reason_codes().end());
  }
}

TEST(ManagerActiveRuntimeCoordinator,
     ObservationNeverRestartsAUnitLostAfterOrderlyShutdown) {
  FixedPlanSource source(ThreeUnitPlan());
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 800U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  EXPECT_EQ(coordinator.Initialize().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  EXPECT_EQ(ports.start_calls["runtime.b"], 1);
  EXPECT_EQ(ports.start_calls["runtime.c"], 1);
  static_cast<void>(coordinator.Shutdown());
  EXPECT_TRUE(ports.epochs.empty());

  ports.events.clear();
  const auto observed = coordinator.Observe();
  EXPECT_EQ(observed.state, ManagerActiveRuntimeConvergenceState::kPending);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.a",
                              "observe:runtime.b",
                              "observe:runtime.c",
                          }));
  EXPECT_EQ(ports.start_calls["runtime.a"], 1);
  EXPECT_EQ(ports.start_calls["runtime.b"], 1);
  EXPECT_EQ(ports.start_calls["runtime.c"], 1);
  for (const auto &observation : observed.observations.observations()) {
    EXPECT_EQ(observation.derived_state(), RuntimeConvergenceState::kLost);
  }
}

TEST(ManagerActiveRuntimeCoordinator,
     ObservationRejectsAChangedActivePlanWithoutProcessEffects) {
  PlanChangingAfterFirstReadSource source(BootstrapHostPlan(),
                                         ThreeUnitPlan());
  ScriptedLifecyclePorts ports;
  std::uint64_t now = 900U;
  ManagerActiveRuntimeCoordinator coordinator(source, ports, ports,
                                              [&now]() { return ++now; });

  EXPECT_THROW(static_cast<void>(coordinator.Observe()),
               ManagerActiveRuntimeCoordinatorError);
  EXPECT_EQ(ports.events, (std::vector<std::string>{
                              "observe:runtime.bootstrap",
                          }));
  EXPECT_TRUE(ports.start_calls.empty());
  EXPECT_TRUE(ports.stop_calls.empty());
}

} // namespace
} // namespace manyforge::component_manager

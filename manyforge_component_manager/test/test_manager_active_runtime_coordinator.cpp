#include <gtest/gtest.h>

#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_core/runtime_contract_manifest_codec.hpp>

#include <map>
#include <optional>
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
  const auto requirement = MakeRuntimeContractRequirement(
      RuntimeImplementationFamilyId("runtime-family"), {}, {});
  if (!requirement.ok()) {
    throw std::runtime_error("active runtime test requirement is invalid");
  }
  return {
      RuntimeUnitId(runtime_unit_id),
      RuntimeProfileId("profile.reference"),
      RuntimeProfileRevision("profile-revision-1"),
      manyforge::core::RuntimeLifecycleControl::kManagerManaged,
      RuntimeUnitCandidateDisposition::kAdded,
      ProductId("runtime_product_" + runtime_unit_id),
      ProductContractId("manyforge.runtime-product"),
      ProductContractRevision("runtime-product-revision-1"),
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
      RuntimeContractOfferDigest(std::string(kDigestOne)),
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
        query.plan.runtime_contract_offer_digest.value());
  }

  std::vector<std::string> events;
  std::vector<RuntimeUnitConvergenceOwnerDTO> observed_owners;
  std::map<std::string, int> start_calls;
  std::map<std::string, int> stop_calls;
  std::map<std::string, std::string> epochs;
  std::string throw_after_start_effect;
  std::string throw_after_stop_effect;
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

  const auto result = coordinator.Reconcile();
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

  const auto first = coordinator.Reconcile();
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
  const auto replay = coordinator.Reconcile();
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
  EXPECT_EQ(coordinator.Reconcile().state,
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
    static_cast<void>(coordinator.Reconcile());
    FAIL() << "expected uncertain start response";
  } catch (const ManagerActiveRuntimeProcessEffectUncertainError &error) {
    EXPECT_EQ(error.effect(), ManagerActiveRuntimeProcessEffect::kStart);
    EXPECT_EQ(error.recovery_query().plan.runtime_unit_id,
              RuntimeUnitId("runtime.a"));
  }
  EXPECT_EQ(coordinator.Reconcile().state,
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

  EXPECT_EQ(coordinator.Reconcile().state,
            ManagerActiveRuntimeConvergenceState::kConverged);
  ports.replay_last_report = true;
  EXPECT_THROW(static_cast<void>(coordinator.Reconcile()),
               ManagerActiveRuntimeCoordinatorError);
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

  EXPECT_EQ(coordinator.Reconcile().state,
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

  EXPECT_THROW(static_cast<void>(coordinator.Reconcile()),
               ManagerActiveRuntimeCoordinatorError);
  EXPECT_TRUE(ports.events.empty());
}

} // namespace
} // namespace manyforge::component_manager

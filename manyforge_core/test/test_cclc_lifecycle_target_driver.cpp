#include <gtest/gtest.h>

#include <functional>

#include <atomic>
#include <map>
#include <thread>
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::int64_t kSecond = 1'000'000'000LL;
const ControllableComponentId kComponent("component.arm");
const ControlDomainId kDomain("domain.arm");

[[nodiscard]] std::string Sha(char value) { return "sha256:" + std::string(64U, value); }

[[nodiscard]] LifecycleFactValueDTO Boolean(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt};
}

/// Observation-only plans of the retained ROS 2 control product: the
/// external controller establishes the state, CCLC only verifies it.
[[nodiscard]] std::vector<LifecycleIntentPlanDTO> ObservedControllerPlans() {
  const auto predicate = [](std::vector<ComponentLifecycleStateDTO> states, bool ready) {
    return LifecyclePredicateDTO{
        std::move(states),
        {{LifecycleFactId("external.controller_configuration_ready"), Boolean(ready)},
         {LifecycleFactId("external.fault_present"), Boolean(false)},
         {LifecycleFactId("external.transport_ready"), Boolean(true)}}};
  };
  const auto ready = predicate(
      {ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kActive}, true);
  const auto active = predicate({ComponentLifecycleStateDTO::kActive}, true);
  const auto disabled = predicate({ComponentLifecycleStateDTO::kDisabled}, false);
  return {
      {ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleStateDTO::kPrepared,
       std::nullopt, ready, 30 * kSecond, {}},
      {ComponentLifecycleIntentKindDTO::kEnable, ComponentLifecycleStateDTO::kActive,
       std::nullopt, active, 30 * kSecond, {}},
      {ComponentLifecycleIntentKindDTO::kDisable, ComponentLifecycleStateDTO::kDisabled,
       std::nullopt, disabled, 30 * kSecond, {}},
      {ComponentLifecycleIntentKindDTO::kRecover, ComponentLifecycleStateDTO::kDisabled, disabled,
       disabled, 30 * kSecond, {}},
  };
}

class FakeStore final : public ICCLCLifecycleTargetStore {
 public:
  Result<ComponentLifecycleTargetDTO> SetLifecycleTarget(const ControllableComponentId& component,
                                                         LifecycleTargetStateDTO state,
                                                         LifecycleTargetOriginDTO origin) override {
    ++writes;
    const auto found = targets.find(component);
    const ComponentLifecycleTargetDTO target{
        component, state, origin,
        (found == targets.end() ? 0U : found->second.target_generation) + 1U,
        found != targets.end() && found->second.reached,
        found == targets.end() ? std::nullopt : found->second.recovered_cause};
    targets.insert_or_assign(component, target);
    return Result<ComponentLifecycleTargetDTO>::Ok(target);
  }
  Result<std::vector<ComponentLifecycleTargetDTO>> LoadLifecycleTargets() override {
    std::vector<ComponentLifecycleTargetDTO> result;
    for (const auto& [id, target] : targets) result.push_back(target);
    return Result<std::vector<ComponentLifecycleTargetDTO>>::Ok(result);
  }
  Result<std::optional<ComponentLifecycleTargetDTO>> LoadLifecycleTarget(
      const ControllableComponentId& component) override {
    const auto found = targets.find(component);
    return Result<std::optional<ComponentLifecycleTargetDTO>>::Ok(
        found == targets.end() ? std::nullopt : std::optional(found->second));
  }
  Result<bool> RecordInterventionCause(const ControllableComponentId& component,
                                       const ControlDomainId& domain,
                                       std::optional<LifecycleBlockingCauseClassDTO> cause,
                                       std::int64_t at) override {
    const auto key = std::pair(component, domain);
    const auto found = causes.find(key);
    const std::optional<LifecycleBlockingCauseClassDTO> stored =
        found == causes.end() ? std::nullopt : std::optional(found->second.cause_class);
    if (stored == cause) return Result<bool>::Ok(false);
    ++writes;
    if (cause) {
      causes.insert_or_assign(key, ComponentLifecycleInterventionCauseDTO{component, domain,
                                                                          *cause, at});
    } else {
      causes.erase(key);
    }
    return Result<bool>::Ok(true);
  }
  Result<std::vector<ComponentLifecycleInterventionCauseDTO>> LoadInterventionCauses(
      const ControllableComponentId& component) override {
    std::vector<ComponentLifecycleInterventionCauseDTO> result;
    for (const auto& [key, cause] : causes) {
      if (key.first == component) result.push_back(cause);
    }
    return Result<std::vector<ComponentLifecycleInterventionCauseDTO>>::Ok(result);
  }
  Result<bool> RecordLifecycleTargetReached(const ControllableComponentId& component,
                                            bool reached) override {
    const auto found = targets.find(component);
    if (found == targets.end() || found->second.reached == reached) return Result<bool>::Ok(false);
    ++writes;
    found->second.reached = reached;
    if (reached) found->second.recovered_cause.reset();
    return Result<bool>::Ok(true);
  }
  ComponentIncidentRecordDTO* Open(const ControllableComponentId& component) {
    for (auto& [id, row] : incidents) {
      if (row.incident.controllable_component_id == component && !row.incident.acknowledged) {
        return &row;
      }
    }
    return nullptr;
  }
  Result<bool> ComponentInUnadoptedApplyPlan(const ControllableComponentId& component) override {
    return Result<bool>::Ok(applying.contains(component));
  }
  Result<ComponentIncidentRecordDTO> OpenComponentIncident(
      const ComponentIncidentRecordDTO& record) override {
    if (const auto* open = Open(record.incident.controllable_component_id)) {
      return Result<ComponentIncidentRecordDTO>::Ok(*open);
    }
    ++writes;
    incidents.insert_or_assign(record.incident.incident_id, record);
    return Result<ComponentIncidentRecordDTO>::Ok(record);
  }
  Result<std::optional<ComponentIncidentRecordDTO>> LoadOpenComponentIncident(
      const ControllableComponentId& component) override {
    const auto* open = Open(component);
    return Result<std::optional<ComponentIncidentRecordDTO>>::Ok(
        open == nullptr ? std::nullopt : std::optional(*open));
  }
  Result<bool> RecordComponentIncidentContainment(const std::string& id,
                                                  ComponentIncidentContainmentStateDTO state,
                                                  const std::string& json,
                                                  std::int64_t at) override {
    const auto found = incidents.find(id);
    if (found == incidents.end() || found->second.incident.acknowledged ||
        found->second.incident.containment_state != ComponentIncidentContainmentStateDTO::kPending) {
      return Result<bool>::Ok(false);
    }
    ++writes;
    found->second.incident.containment_state = state;
    found->second.record_json = json;
    events.push_back({events.size() + 1U, std::string(kComponentIncidentEventKind), id, at, json});
    return Result<bool>::Ok(true);
  }
  Result<bool> AcknowledgeComponentIncident(const ComponentLifecycleIncidentDTO& acknowledged,
                                            const std::string& payload) override {
    auto* open = Open(acknowledged.controllable_component_id);
    if (open == nullptr || open->incident.incident_id != acknowledged.incident_id) {
      return Result<bool>::Ok(false);
    }
    ++writes;
    open->incident = acknowledged;
    std::erase_if(causes, [&](const auto& row) {
      return row.first.first == acknowledged.controllable_component_id;
    });
    if (auto target = targets.find(acknowledged.controllable_component_id);
        target != targets.end()) {
      target->second.reached = false;
      target->second.recovered_cause = acknowledged.cause_class;
    }
    events.push_back({events.size() + 1U, std::string(kComponentIncidentAcknowledgedEventKind),
                      acknowledged.incident_id, *acknowledged.acknowledged_at_ns, payload});
    return Result<bool>::Ok(true);
  }
  Result<std::optional<ComponentIncidentRecoveryDTO>> LoadComponentIncidentRecovery(
      const ControllableComponentId& component, const RequestId& request_id) override {
    for (const auto& [id, row] : incidents) {
      const auto& incident = row.incident;
      if (incident.controllable_component_id == component && incident.acknowledged &&
          incident.acknowledgement_request_id == request_id) {
        return Result<std::optional<ComponentIncidentRecoveryDTO>>::Ok(
            ComponentIncidentRecoveryDTO{id, component, incident.cause_class,
                                         *incident.acknowledged_by,
                                         *incident.acknowledged_at_ns});
      }
    }
    return Result<std::optional<ComponentIncidentRecoveryDTO>>::Ok(std::nullopt);
  }
  Result<bool> UpdateOpenComponentIncident(const std::string& id,
                                           LifecycleBlockingCauseClassDTO cause,
                                           const std::vector<std::string>& codes,
                                           const std::string& json) override {
    const auto found = incidents.find(id);
    if (found == incidents.end() || found->second.incident.acknowledged) {
      return Result<bool>::Ok(false);
    }
    ++writes;
    found->second.incident.cause_class = cause;
    found->second.incident.vendor_codes = codes;
    found->second.record_json = json;
    return Result<bool>::Ok(true);
  }
  Result<std::vector<ComponentIncidentEventDTO>> LoadComponentIncidentEvents(
      std::uint64_t after, std::size_t limit) override {
    std::vector<ComponentIncidentEventDTO> result;
    for (const auto& event : events) {
      if (event.sequence > after && result.size() < limit) result.push_back(event);
    }
    return Result<std::vector<ComponentIncidentEventDTO>>::Ok(result);
  }

  std::set<ControllableComponentId> applying;
  std::map<std::string, ComponentIncidentRecordDTO> incidents;
  std::vector<ComponentIncidentEventDTO> events;
  std::map<ControllableComponentId, ComponentLifecycleTargetDTO> targets;
  std::map<std::pair<ControllableComponentId, ControlDomainId>,
           ComponentLifecycleInterventionCauseDTO>
      causes;
  int writes{};
};

enum class SubmitMode { kAccept, kReject, kError };

/// Deterministic CCLC/adapter stand-in. Observations and achieved state are
/// set by the test; nothing changes unless the test changes it.
class FakePorts final : public ICCLCLifecycleTargetDriverPorts {
 public:
  explicit FakePorts(IClock& clock)
      : clock_(clock),
        catalog_(MakeBrandLifecyclePlanCatalog(ProviderManifestId("manifest.arm"),
                                               ProviderManifestRevision(Sha('f')),
                                               ObservedControllerPlans())
                     .value()) {}

  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& component) override {
    const ProjectionRevisionDTO projection{RetainedBundleId("bundle.1"), GraphRevision("graph.1"),
                                           {SceneId(Sha('a')), 1U, ContentDigest(Sha('b'))}};
    ActiveBundlePublicationReceiptDTO receipt{
        {NamedApplyPublicationTransactionDTO{ApplyId("apply.1"), ApplyGeneration(1U)},
         projection, projection.retained_bundle_id, projection.graph_revision,
         ActiveBundlePublicationGeneration(1U), ContentDigest(Sha('c'))},
        ActiveBundlePublicationGeneration(2U),
        ContentDigest(Sha('d'))};
    const ExpectedControlBindingRevisionDTO binding{ControlBindingId("binding.arm"),
                                                    ControlBindingRevision("binding.rev.1"),
                                                    ControlRouteId("route.arm")};
    return Result<ComponentLifecycleCurrentSnapshotDTO>::Ok(
        {ContentDigest(Sha(static_cast<char>('0' + lifecycle_generation % 10U))),
         component,
         ComponentLifecycleGeneration(lifecycle_generation),
         achieved,
         receipt,
         {{kDomain, ControlDomainRevision("domain.rev.1"), ControllerTopologyId("topology.arm"),
           TopologyRevision("topology.rev.1"), std::nullopt, {binding},
           {ResourceId("resource.arm")}}},
         {Occurrence()}});
  }

  Result<BrandLifecyclePlanCatalogDTO> LoadLifecyclePlanCatalog(
      const ResolvedLifecycleProviderOccurrenceDTO&) override {
    return Result<BrandLifecyclePlanCatalogDTO>::Ok(catalog_);
  }

  Result<BrandLifecycleObservationDTO> Observe(
      const ResolvedLifecycleProviderOccurrenceDTO&,
      const BrandLifecycleObservationQueryDTO& query) override {
    ++observations;
    if (on_observe) on_observe();
    if (!reachable) {
      return Result<BrandLifecycleObservationDTO>::Err(
          {"external_lifecycle_source_unavailable", "controller status is unreachable", {}});
    }
    std::vector<LifecycleFactDTO> facts{
        {LifecycleFactId("external.controller_configuration_ready"), Boolean(ready)},
        {LifecycleFactId("external.fault_present"), Boolean(false)},
        {LifecycleFactId("external.transport_ready"), Boolean(true)}};
    if (cause) {
      facts.push_back({LifecycleFactId(std::string(kLifecycleBlockingCauseFactId)),
                       {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt,
                        std::string(to_string(*cause))}});
    }
    facts.insert(facts.end(), vendor_facts.begin(), vendor_facts.end());
    const auto now = clock_.now();
    return Result<BrandLifecycleObservationDTO>::Ok(
        {query.lifecycle_transaction_id, query.lifecycle_transaction_generation,
         query.controllable_component_id, query.control_domain_id, query.control_domain_revision,
         query.provider_instance_id, query.provider_manifest_id, query.provider_manifest_revision,
         query.provider_runtime_unit_id, query.provider_runtime_epoch, observation_generation,
         observation_generation, now, now, now + kSecond, observed, std::move(facts)});
  }

  // Runs inside Observe before the observation is stamped, like the real
  // adapter whose sampling takes time on the owner clock.
  std::function<void()> on_observe;

  AuthoringAdmissionDTO ReadCellAdmission() override {
    return {authoring_allowed, AdmissionGeneration(admission_generation)};
  }
  RequestOwnerEpoch ReadRequestOwnerEpoch() override { return RequestOwnerEpoch("cclc.owner.1"); }
  bool AcceptingLifecycleRequests() override { return accepting; }
  bool ComponentLifecycleSettling(const ControllableComponentId&) override { return settling; }

  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override {
    submitted.push_back(intent);
    ++submit_count;
    if (submit_mode == SubmitMode::kError) {
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          {"cclc_open_request_failure", "transport lost before the result", {}});
    }
    // An exact replay returns the original acceptance.
    for (const auto& [id, transaction] : accepted) {
      if (transaction.first == intent.request_context.request_id) {
        return Result<CCLCLifecycleRequestResultDTO>::Ok(
            {RequestReplayStatusDTO::kReplayedExact, Accepted(intent, id), std::nullopt});
      }
    }
    if (submit_mode == SubmitMode::kReject) {
      return Result<CCLCLifecycleRequestResultDTO>::Ok(
          {RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt,
           ErrorDTO{"component_lifecycle_busy", "another lifecycle request owns the component",
                    {}}});
    }
    const LifecycleTransactionId id("lifecycle." + std::to_string(accepted.size() + 1U));
    accepted.insert_or_assign(id, std::pair(intent.request_context.request_id, intent));
    statuses.insert_or_assign(id, Status(intent, id, ComponentLifecycleTransactionPhaseDTO::kExecuting,
                          std::nullopt, false));
    return Result<CCLCLifecycleRequestResultDTO>::Ok(
        {RequestReplayStatusDTO::kAcceptedNew, Accepted(intent, id), std::nullopt});
  }

  Result<ComponentIncidentContainmentReceiptDTO> RequestIncidentContainment(
      const ComponentIncidentContainmentIntentDTO& intent) override {
    containments.push_back(intent);
    if (!containment_accepts) {
      return Result<ComponentIncidentContainmentReceiptDTO>::Err(
          {"kernel_unreachable", "the Kernel did not answer", {}});
    }
    return Result<ComponentIncidentContainmentReceiptDTO>::Ok(
        {StopId("stop." + std::to_string(containments.size())),
         StopGeneration(containments.size()),
         {OperationId("operation.interrupted")},
         std::nullopt});
  }

  Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId& id) override {
    const auto found = statuses.find(id);
    return Result<std::optional<ComponentLifecycleStatusDTO>>::Ok(
        found == statuses.end() ? std::nullopt : std::optional(found->second));
  }

  /// CCLC records a terminal outcome for the latest accepted transaction;
  /// success establishes the requested state as the achieved state.
  void Finish(std::optional<ComponentLifecycleTerminalResultDTO> result,
              ComponentLifecycleTransactionPhaseDTO phase,
              bool reconciliation_required = false) {
    const LifecycleTransactionId id("lifecycle." + std::to_string(accepted.size()));
    const auto& intent = accepted.at(id).second;
    statuses.insert_or_assign(id, Status(intent, id, phase, result, reconciliation_required));
    if (result == ComponentLifecycleTerminalResultDTO::kSucceeded) {
      achieved = intent.intent == ComponentLifecycleIntentKindDTO::kPrepare
                     ? ComponentLifecycleStateDTO::kPrepared
                 : intent.intent == ComponentLifecycleIntentKindDTO::kEnable
                     ? ComponentLifecycleStateDTO::kActive
                     : ComponentLifecycleStateDTO::kDisabled;
      ++lifecycle_generation;
    }
  }

  static ResolvedLifecycleProviderOccurrenceDTO Occurrence() {
    return {kDomain,
            ControlDomainRevision("domain.rev.1"),
            ProviderInstanceId("provider.arm"),
            ProviderManifestId("manifest.arm"),
            ProviderManifestRevision(Sha('f')),
            RuntimeUnitId("runtime.arm"),
            RuntimeEpoch("epoch.1"),
            ContentDigest(Sha('e'))};
  }

  // Test-controlled world.
  ComponentLifecycleStateDTO achieved{ComponentLifecycleStateDTO::kDeclared};
  std::uint64_t lifecycle_generation{1U};
  ComponentLifecycleStateDTO observed{ComponentLifecycleStateDTO::kDeclared};
  bool ready{};
  bool reachable{true};
  std::optional<LifecycleBlockingCauseClassDTO> cause;
  std::uint64_t observation_generation{1U};
  bool authoring_allowed{true};
  bool accepting{true};
  bool settling{};
  std::uint64_t admission_generation{3U};
  SubmitMode submit_mode{SubmitMode::kAccept};
  std::vector<LifecycleFactDTO> vendor_facts;
  bool containment_accepts{true};
  std::vector<ComponentIncidentContainmentIntentDTO> containments;

  std::vector<ComponentLifecycleIntentDTO> submitted;
  std::map<LifecycleTransactionId, std::pair<RequestId, ComponentLifecycleIntentDTO>> accepted;
  std::map<LifecycleTransactionId, ComponentLifecycleStatusDTO> statuses;
  int observations{};
  std::atomic<int> submit_count{};

  /// Controller readiness arrives: the external owner has activated it.
  void BecomeReady() {
    ready = true;
    observed = ComponentLifecycleStateDTO::kActive;
    ++observation_generation;
  }

 private:
  static ComponentLifecycleAcceptedDTO Accepted(const ComponentLifecycleIntentDTO& intent,
                                                const LifecycleTransactionId& id) {
    return {intent.request_context.request_id,
            RequestReplayStatusDTO::kAcceptedNew,
            id,
            LifecycleTransactionGeneration(1U),
            ComponentLifecycleGeneration(intent.expected_component_lifecycle_generation.value() + 1U),
            0,
            30 * kSecond};
  }
  static ComponentLifecycleStatusDTO Status(const ComponentLifecycleIntentDTO& intent,
                                            const LifecycleTransactionId& id,
                                            ComponentLifecycleTransactionPhaseDTO phase,
                                            std::optional<ComponentLifecycleTerminalResultDTO> result,
                                            bool reconciliation_required) {
    return {Accepted(intent, id),
            intent,
            1U,
            RequestOwnerEpoch("cclc.owner.1"),
            std::nullopt,
            std::nullopt,
            {ComponentLifecycleRequestedStatePresenceDTO::kUnresolved, std::nullopt},
            phase,
            {},
            reconciliation_required,
            result,
            result && *result != ComponentLifecycleTerminalResultDTO::kSucceeded
                ? std::optional<ErrorDTO>(
                      ErrorDTO{"component_lifecycle_postcondition_not_satisfied", "", {}})
                : std::nullopt,
            0,
            0,
            0};
  }

  IClock& clock_;
  BrandLifecyclePlanCatalogDTO catalog_;
};

class LifecycleTargetDriverTest : public ::testing::Test {
 protected:
  LifecycleTargetDriverTest() : ports_(clock_) { clock_.set(100 * kSecond); }

  CCLCLifecycleTargetDriver& Driver() {
    if (!driver_) {
      driver_.emplace(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_, scheduler_);
    }
    return *driver_;
  }
  void Target(LifecycleTargetStateDTO state, LifecycleTargetOriginDTO origin) {
    static_cast<void>(store_.SetLifecycleTarget(kComponent, state, origin));
    store_.writes = 0;
  }
  ComponentLifecycleProgressDTO Progress() {
    const auto view = Driver().ReadTargetView(kComponent);
    EXPECT_TRUE(view.ok());
    EXPECT_TRUE(view.value().progress.has_value());
    return *view.value().progress;
  }
  ComponentLifecycleIntentDTO OperatorIntent(ComponentLifecycleIntentKindDTO kind) {
    auto intent = ports_.submitted.back();
    intent.request_context.source_label = "composer";
    intent.request_context.request_id = RequestId("operator.1");
    intent.intent = kind;
    intent.controllable_component_id = kComponent;
    return intent;
  }

  FakeClock clock_;
  ManualScheduler scheduler_;
  FakeStore store_;
  FakePorts ports_;
  std::optional<CCLCLifecycleTargetDriver> driver_;
};

TEST_F(LifecycleTargetDriverTest, LateReadinessIsFollowedByPrepareThenEnableWithoutAnOperator) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  EXPECT_EQ(Progress().next_intent, ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_TRUE(ports_.submitted.empty());

  clock_.advance(5 * kSecond);
  ports_.BecomeReady();
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  const auto& prepare = ports_.submitted.front();
  EXPECT_EQ(prepare.intent, ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(prepare.request_context.source_label, "manyforge_lifecycle_target");
  EXPECT_EQ(prepare.request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g1.s0.prepare.a1");
  EXPECT_EQ(prepare.expected_admission_generation, AdmissionGeneration(3U));
  EXPECT_EQ(prepare.attribution, RequestAttributionDTO{});
  EXPECT_TRUE(prepare.locally_valid());
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kTransitionInProgress);

  // While the transaction is open the driver only follows it.
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);

  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted.back().intent, ComponentLifecycleIntentKindDTO::kEnable);
  EXPECT_EQ(ports_.submitted.back().request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g1.s1.enable.a1");
  EXPECT_EQ(ports_.submitted.back().expected_component_lifecycle_generation,
            ComponentLifecycleGeneration(2U));

  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  Driver().RunOnce();
  const auto satisfied = Progress();
  EXPECT_EQ(satisfied.phase, LifecycleProgressPhaseDTO::kSatisfied);
  EXPECT_FALSE(satisfied.next_intent.has_value());
  EXPECT_FALSE(satisfied.waiting_since_ns.has_value());
  EXPECT_EQ(ports_.submitted.size(), 2U);
}

TEST_F(LifecycleTargetDriverTest, ObservationStampedAfterThePassStartedIsStillFresh) {
  // The real owner clock advances while the adapter samples; an observation
  // stamped after the pass began must not look as if it came from the future.
  ports_.on_observe = [this] { clock_.advance(1'000'000); };
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kTransitionInProgress);
}

TEST_F(LifecycleTargetDriverTest, UnmetPrerequisitesWaitWithoutTransactionsAndOnlyMarkDelay) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  const auto started = clock_.now();
  Driver().RunOnce();
  auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  EXPECT_EQ(progress.waiting_since_ns, started);
  EXPECT_FALSE(progress.delayed);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind("prerequisites_unmet:", 0U), 0U);

  for (int second = 0; second < 29; ++second) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_FALSE(Progress().delayed);
  clock_.advance(2 * kSecond);
  Driver().RunOnce();
  progress = Progress();
  // The readiness expectation only presents the delay; waiting continues.
  EXPECT_TRUE(progress.delayed);
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  EXPECT_EQ(progress.waiting_since_ns, started);
  EXPECT_TRUE(ports_.submitted.empty());
  EXPECT_EQ(store_.writes, 0);

  ports_.BecomeReady();
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
}

TEST_F(LifecycleTargetDriverTest, UncertainOutcomeIsReconciledByCCLCBeforeAnyRetry) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);

  // The command deadline expired: CCLC reconciles; the driver does not act.
  ports_.Finish(std::nullopt, ComponentLifecycleTransactionPhaseDTO::kOutcomeUnknown, true);
  for (int pass = 0; pass < 5; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  }
  EXPECT_EQ(ports_.submitted.size(), 1U);

  // Reconciliation records a known failure: no blind retry on the same facts.
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kFailedPartialEffect,
                ComponentLifecycleTransactionPhaseDTO::kFailed);
  Driver().RunOnce();
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);

  // A newer observation permits exactly one further attempt.
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted.back().request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g1.s0.prepare.a2");
}

TEST_F(LifecycleTargetDriverTest, UnknownSubmissionIsReplayedExactlyNeverReissued) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kError;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  ports_.submit_mode = SubmitMode::kAccept;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  // Same request identity and content: CCLC classifies it as replay.
  EXPECT_EQ(ports_.submitted[0], ports_.submitted[1]);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kTransitionInProgress);
}

TEST_F(LifecycleTargetDriverTest, RejectionBeforeAcceptanceWaitsForAChangedInput) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kReject;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  for (int pass = 0; pass < 3; ++pass) Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
  // Known no-admission conditions are waited out without any request.
  ports_.submit_mode = SubmitMode::kAccept;
  ++ports_.admission_generation;
  ports_.settling = true;
  Driver().RunOnce();
  ports_.settling = false;
  ports_.accepting = false;
  Driver().RunOnce();
  ports_.accepting = true;
  ports_.authoring_allowed = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_reason, "cell_admission_closed");
  EXPECT_EQ(ports_.submitted.size(), 1U);
  ports_.authoring_allowed = true;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted.back().request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g1.s0.prepare.a2");
}

TEST_F(LifecycleTargetDriverTest, UnexplainedRejectionIsReevaluatedOnlyAfterItsBound) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kReject;
  Driver().RunOnce();
  ports_.submit_mode = SubmitMode::kAccept;
  for (int second = 0; second < 9; ++second) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(ports_.submitted.size(), 1U);
  clock_.advance(2 * kSecond);
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 2U);
}

TEST_F(LifecycleTargetDriverTest, OperatorDisableCancelsProgressionAndIsNeverReEnabled) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);  // Prepare in flight.

  const auto recorded =
      Driver().RecordOperatorRequest(OperatorIntent(ComponentLifecycleIntentKindDTO::kDisable));
  ASSERT_TRUE(recorded.ok());
  ASSERT_TRUE(recorded.value().has_value());
  EXPECT_EQ(recorded.value()->requested_state, LifecycleTargetStateDTO::kDisabled);
  EXPECT_EQ(recorded.value()->origin, LifecycleTargetOriginDTO::kOperator);
  EXPECT_EQ(recorded.value()->target_generation, 2U);

  // The accepted Prepare settles under its own deadline; nothing is chained.
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  // The operator's own Disable is admitted by CCLC and succeeds.
  ports_.achieved = ComponentLifecycleStateDTO::kDisabled;
  for (int pass = 0; pass < 5; ++pass) {
    clock_.advance(kSecond);
    ++ports_.observation_generation;
    Driver().RunOnce();
  }
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
  for (const auto& intent : ports_.submitted) {
    EXPECT_NE(intent.intent, ComponentLifecycleIntentKindDTO::kEnable);
  }
  EXPECT_EQ(ports_.submitted.size(), 1U);
  // Recover and system-attributed requests never set a target.
  EXPECT_FALSE(Driver()
                   .RecordOperatorRequest(OperatorIntent(ComponentLifecycleIntentKindDTO::kRecover))
                   .value()
                   .has_value());
  auto system = OperatorIntent(ComponentLifecycleIntentKindDTO::kEnable);
  system.request_context.source_label = std::string(kLifecycleTargetSourceLabel);
  EXPECT_FALSE(Driver().RecordOperatorRequest(system).value().has_value());
  EXPECT_EQ(store_.targets.at(kComponent).requested_state, LifecycleTargetStateDTO::kDisabled);
}

TEST_F(LifecycleTargetDriverTest, SupersedingTargetCancelsWaitingAndRemovalStopsFollowing) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kError;
  Driver().RunOnce();  // unknown submission pending for g1
  ASSERT_EQ(ports_.submitted.size(), 1U);

  // A superseding Apply replaces the target: the g1 request is not re-sent.
  ports_.submit_mode = SubmitMode::kAccept;
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted.back().request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g2.s0.prepare.a1");

  // Removal cancels the target: no progress, no further requests.
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  store_.targets.clear();
  Driver().RunOnce();
  const auto view = Driver().ReadTargetView(kComponent);
  ASSERT_TRUE(view.ok());
  EXPECT_FALSE(view.value().target.has_value());
  EXPECT_FALSE(view.value().progress.has_value());
  EXPECT_EQ(ports_.submitted.size(), 2U);
}

TEST_F(LifecycleTargetDriverTest, InterventionCauseBlocksAndSurvivesCommunicationLoss) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.cause = LifecycleBlockingCauseClassDTO::kProtectiveStop;
  Driver().RunOnce();
  auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_EQ(progress.blocking_reason, "intervention_required:protective_stop");
  EXPECT_EQ(store_.causes.size(), 1U);

  // Communication is lost: the retained class still blocks, not availability.
  ports_.reachable = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
  // A fresh `availability` report does not clear it either.
  ports_.reachable = true;
  ports_.cause = LifecycleBlockingCauseClassDTO::kAvailability;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_TRUE(ports_.submitted.empty());

  // Fresh status proves the reset (wherever it was performed): continue.
  ports_.cause = LifecycleBlockingCauseClassDTO::kNone;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_TRUE(store_.causes.empty());
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kPrepare);
}

TEST_F(LifecycleTargetDriverTest, InterventionCausePermitsOnlyARequestedDisable) {
  Target(LifecycleTargetStateDTO::kDisabled, LifecycleTargetOriginDTO::kOperator);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  ports_.observed = ComponentLifecycleStateDTO::kDisabled;
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kDisable);
}

TEST_F(LifecycleTargetDriverTest, NothingIsSubmittedOrWrittenWhileNothingChanges) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  for (int pass = 0; pass < 10; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
  // A satisfied target is still observed (read only, for its cause class)
  // and submits nothing; it records only once that it reached its target.
  EXPECT_EQ(store_.writes, 1);
  EXPECT_TRUE(store_.targets.at(kComponent).reached);
  EXPECT_TRUE(ports_.submitted.empty());

  // A blocked component writes its retained cause once, then nothing.
  ports_.achieved = ComponentLifecycleStateDTO::kDeclared;
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  for (int pass = 0; pass < 10; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(store_.writes, 2);
  EXPECT_TRUE(ports_.submitted.empty());
}

[[nodiscard]] LifecycleFactDTO VendorToken(const std::string& segment, const std::string& token) {
  return {LifecycleFactId(std::string(kLifecycleVendorCodeFactPrefix) + segment),
          {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, token}};
}

[[nodiscard]] LifecycleFactDTO VendorInteger(const std::string& segment, std::int64_t value) {
  return {LifecycleFactId(std::string(kLifecycleVendorCodeFactPrefix) + segment),
          {LifecycleFactValueKindDTO::kInteger, std::nullopt, value, std::nullopt}};
}

class LifecycleIncidentTest : public LifecycleTargetDriverTest {
 protected:
  /// The component reaches its target on fresh status (Apply adopted it
  /// Active; the controller reports ready).
  void ReachTarget() {
    Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
    ports_.achieved = ComponentLifecycleStateDTO::kActive;
    ports_.BecomeReady();
    Driver().RunOnce();
    ASSERT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
    ASSERT_TRUE(store_.targets.at(kComponent).reached);
  }
  ComponentLifecycleIntentDTO OperatorRecover(const std::string& request_id) {
    const auto current = ports_.ReadCurrentComponent(kComponent).value();
    std::vector<ExpectedControlDomainRevisionDTO> domains;
    for (const auto& domain : current.domains) {
      domains.push_back({domain.control_domain_id, domain.control_domain_revision,
                         domain.controller_topology_id, domain.topology_revision,
                         {ExpectedActiveControlConfigurationPresenceDTO::kAbsent, std::nullopt,
                          std::nullopt},
                         domain.current_control_bindings});
    }
    ComponentLifecycleIntentDTO intent{
        {RequestId(request_id), RequestOwnerEpoch("cclc.owner.1"), "composer", std::nullopt},
        kComponent,
        ComponentLifecycleIntentKindDTO::kRecover,
        current.active_publication_receipt.request.candidate_graph_revision,
        current.component_lifecycle_generation,
        std::move(domains),
        "operator recovered the cell",
        RequestAttributionDTO{RequestTransportDTO::kHttp,
                              RequestAssuranceDTO::kExternallyAuthenticated,
                              std::string("operator.ada")},
        AdmissionGeneration(ports_.admission_generation),
        std::nullopt};
    return intent;
  }
  void ExpectNoMotionOrRecoverySubmitted() {
    // The driver submits lifecycle transactions only; it never issues an
    // operation, resumes motion or submits a Recover for an incident.
    for (const auto& intent : ports_.submitted) {
      EXPECT_NE(intent.intent, ComponentLifecycleIntentKindDTO::kRecover);
      EXPECT_EQ(intent.request_context.source_label, kLifecycleTargetSourceLabel);
    }
  }
};

TEST_F(LifecycleIncidentTest, StartupWaitingIsNeverAnIncident) {
  // Adopted Active before the controller ever reported fresh status.
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  ports_.reachable = false;
  for (int pass = 0; pass < 5; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_FALSE(Progress().incident.has_value());
  // A not yet Active component waits plainly as well.
  ports_.achieved = ComponentLifecycleStateDTO::kDeclared;
  ports_.reachable = true;
  ports_.cause = LifecycleBlockingCauseClassDTO::kAvailability;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_TRUE(ports_.containments.empty());
  EXPECT_FALSE(store_.targets.at(kComponent).reached);
}

TEST_F(LifecycleIncidentTest, ProviderLossAfterActiveIsOneIncidentWithOneContainmentStop) {
  ReachTarget();
  const auto detected_at = clock_.now();
  ports_.reachable = false;
  Driver().RunOnce();
  auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(progress.next_intent, ComponentLifecycleIntentKindDTO::kRecover);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  ASSERT_TRUE(progress.incident.has_value());
  EXPECT_EQ(progress.incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(progress.incident->detected_at_ns, detected_at);
  EXPECT_LE(progress.incident->detected_at_ns, kMaxComponentIncidentTimeNs);
  EXPECT_FALSE(progress.incident->acknowledged);
  EXPECT_FALSE(progress.incident->summary.empty());
  ASSERT_EQ(ports_.containments.size(), 1U);
  EXPECT_EQ(ports_.containments.front().incident_id, progress.incident->incident_id);

  // Further passes, readiness returning or `availability` reports change
  // nothing: still one incident, one containment Stop, no transaction.
  for (int pass = 0; pass < 5; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  ports_.reachable = true;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_EQ(Progress().incident->incident_id, progress.incident->incident_id);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(store_.incidents.size(), 1U);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());

  // The durable record carries the Stop reference and the last good status.
  ASSERT_EQ(store_.events.size(), 1U);
  EXPECT_EQ(store_.events.front().event_kind, kComponentIncidentEventKind);
  const auto& payload = store_.events.front().payload_json;
  EXPECT_NE(payload.find(R"("stop":{"stop_generation":1,"stop_id":"stop.1"})"), std::string::npos);
  EXPECT_NE(payload.find(R"("active_operation_ids":["operation.interrupted"])"),
            std::string::npos);
  EXPECT_NE(payload.find(R"("containment_state":"requested")"), std::string::npos);
  EXPECT_NE(payload.find(R"("last_good_observation":{"control_domain_id":"domain.arm")"),
            std::string::npos);
  EXPECT_NE(payload.find(R"("provider_instance_id":"provider.arm")"), std::string::npos);
  EXPECT_NE(payload.find(R"("acknowledgement_required":true)"), std::string::npos);
}

TEST_F(LifecycleIncidentTest, InterventionClassIsAnIncidentAndTheMostRecentCauseWins) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kExternalControl;
  ports_.vendor_facts = {VendorInteger("mode", 2), VendorInteger("state", 5)};
  ++ports_.observation_generation;
  Driver().RunOnce();
  auto progress = Progress();
  ASSERT_TRUE(progress.incident.has_value());
  EXPECT_EQ(progress.incident->cause_class, LifecycleBlockingCauseClassDTO::kExternalControl);
  EXPECT_EQ(progress.incident->vendor_codes, (std::vector<std::string>{"mode=2", "state=5"}));
  const auto id = progress.incident->incident_id;

  // A further unrequested change while it is open is appended to the same
  // incident and its cause becomes the incident's cause.
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ports_.vendor_facts = {VendorToken("error", "C2"), VendorInteger("mode", 0)};
  ++ports_.observation_generation;
  Driver().RunOnce();
  progress = Progress();
  EXPECT_EQ(progress.incident->incident_id, id);
  EXPECT_EQ(progress.incident->cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(progress.incident->vendor_codes,
            (std::vector<std::string>{"mode=2", "state=5", "C2", "mode=0"}));
  EXPECT_NE(store_.incidents.at(id).record_json.find(R"("timeline":[)"), std::string::npos);
  EXPECT_NE(store_.incidents.at(id).record_json.find(R"("cause_class":"emergency_stop")"),
            std::string::npos);
  EXPECT_EQ(store_.incidents.size(), 1U);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, IncidentIsRetainedAcrossCommunicationLoss) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  // Lost status keeps the retained intervention class; it is no new change.
  ports_.reachable = false;
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
  // Fresh status without a cause does not close it either.
  ports_.reachable = true;
  ports_.cause.reset();
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());

  // A replacement driver (in-session host replacement) shows the same open
  // incident from the durable store and never contains it twice.
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  replacement.RunOnce();
  const auto view = replacement.ReadTargetView(kComponent);
  ASSERT_TRUE(view.ok());
  ASSERT_TRUE(view.value().progress->incident.has_value());
  EXPECT_EQ(view.value().progress->phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, PendingContainmentIsCompletedByAReplacementDriver) {
  ReachTarget();
  ports_.containment_accepts = false;
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_EQ(ports_.containments.size(), 1U);
  // A refused containment is recorded, not retried by the same driver.
  ASSERT_EQ(store_.events.size(), 1U);
  EXPECT_NE(store_.events.front().payload_json.find(R"("containment_state":"failed")"),
            std::string::npos);
  Driver().RunOnce();
  EXPECT_EQ(ports_.containments.size(), 1U);

  // A host replaced between recording and requesting finds it pending.
  auto& record = store_.incidents.begin()->second;
  record.incident.containment_state = ComponentIncidentContainmentStateDTO::kPending;
  ports_.containment_accepts = true;
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  replacement.RunOnce();
  replacement.RunOnce();
  EXPECT_EQ(ports_.containments.size(), 2U);
  EXPECT_EQ(ports_.containments[0], ports_.containments[1]);
}

TEST_F(LifecycleIncidentTest, OperatorRecoverClosesTheIncidentAndTheDriverProceedsWithoutMotion) {
  ReachTarget();
  // A UFactory-style emergency stop: the adapter keeps reporting the cause
  // until the next recovery step clears it.
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ports_.vendor_facts = {VendorToken("error", "C2")};
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  const auto id = Progress().incident->incident_id;

  // The cell is latched: ordinary admission is closed. The operator's Recover
  // is still accepted, authorization only (no CCLC transaction).
  ports_.authoring_allowed = false;
  clock_.advance(kSecond);
  const auto recovered = Driver().RecoverIncident(OperatorRecover("operator.recover.1"));
  ASSERT_TRUE(recovered.ok()) << recovered.error().message;
  ASSERT_TRUE(recovered.value().has_value());
  const auto& result = *recovered.value();
  EXPECT_TRUE(result.locally_valid());
  EXPECT_EQ(result.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_FALSE(result.acceptance.has_value());
  ASSERT_TRUE(result.incident_recovery.has_value());
  EXPECT_EQ(result.incident_recovery->incident_id, id);
  EXPECT_EQ(result.incident_recovery->acknowledged_by, "operator.ada");
  EXPECT_EQ(result.incident_recovery->acknowledged_at_ns, clock_.now());
  // The snapshot shows no incident at once; the record keeps who and when.
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_TRUE(store_.causes.empty());
  ASSERT_EQ(store_.events.size(), 2U);
  EXPECT_EQ(store_.events.back().event_kind, kComponentIncidentAcknowledgedEventKind);
  EXPECT_NE(store_.events.back().payload_json.find(R"("acknowledged_by":"operator.ada")"),
            std::string::npos);
  EXPECT_NE(store_.events.back().payload_json.find(R"("vendor_codes":["C2"])"),
            std::string::npos);
  // An exact retransmission replays; a Recover without an incident is left
  // to ordinary CCLC admission.
  const auto replay = Driver().RecoverIncident(OperatorRecover("operator.recover.1"));
  ASSERT_TRUE(replay.ok() && replay.value().has_value());
  EXPECT_EQ(replay.value()->replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.value()->incident_recovery, result.incident_recovery);
  const auto other = Driver().RecoverIncident(OperatorRecover("operator.recover.2"));
  ASSERT_TRUE(other.ok());
  EXPECT_FALSE(other.value().has_value());

  // CCLC had verified the loss (Prepared); the sticky recovered cause does not
  // block the policy-permitted step, but the latched cell does.
  ports_.achieved = ComponentLifecycleStateDTO::kPrepared;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_EQ(Progress().blocking_reason, "cell_admission_closed");
  EXPECT_TRUE(ports_.submitted.empty());
  // The operator's exact cell recovery reopens admission: the driver
  // continues to the target, opening no new incident for the same cause.
  ports_.authoring_allowed = true;
  ++ports_.admission_generation;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kEnable);
  EXPECT_EQ(store_.incidents.size(), 1U);
  EXPECT_EQ(ports_.containments.size(), 1U);
  ExpectNoMotionOrRecoverySubmitted();

  // A different cause newly reported while recovering is a new incident.
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kFailedPartialEffect,
                ComponentLifecycleTransactionPhaseDTO::kFailed);
  ports_.cause = LifecycleBlockingCauseClassDTO::kCollision;
  ports_.vendor_facts = {VendorToken("error", "C31")};
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_NE(Progress().incident->incident_id, id);
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kCollision);
  EXPECT_EQ(ports_.containments.size(), 2U);
  ExpectNoMotionOrRecoverySubmitted();
}

TEST_F(LifecycleIncidentTest, ReachingTheTargetAgainEndsTheRecovery) {
  ReachTarget();
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  ASSERT_TRUE(Driver().RecoverIncident(OperatorRecover("operator.recover.1")).ok());
  EXPECT_EQ(store_.targets.at(kComponent).recovered_cause,
            LifecycleBlockingCauseClassDTO::kProviderLost);
  // Still lost after the Recover: plain waiting, no new incident.
  Driver().RunOnce();
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kAvailability);
  ports_.reachable = true;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_TRUE(store_.targets.at(kComponent).reached);
  EXPECT_FALSE(store_.targets.at(kComponent).recovered_cause.has_value());
  // Lost again after reaching the target: a new incident.
  clock_.advance(kSecond);
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(store_.incidents.size(), 2U);
  EXPECT_EQ(ports_.containments.size(), 2U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, ReplacementDriverWaitsForItsOwnFreshStatusBeforeALossIsAnIncident) {
  ReachTarget();
  // In-session host replacement: the retained component keeps `reached`, but
  // the new driver has not yet seen its provider; stale reads are waiting.
  ports_.reachable = false;
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    replacement.RunOnce();
  }
  auto view = replacement.ReadTargetView(kComponent).value();
  EXPECT_FALSE(view.progress->incident.has_value());
  EXPECT_EQ(view.progress->blocking_cause_class, LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_TRUE(ports_.containments.empty());
  EXPECT_TRUE(store_.targets.at(kComponent).reached);

  // Status re-established, then lost again: now a provider_lost incident.
  ports_.reachable = true;
  ++ports_.observation_generation;
  replacement.RunOnce();
  EXPECT_FALSE(replacement.ReadTargetView(kComponent).value().progress->incident.has_value());
  clock_.advance(kSecond);
  ports_.reachable = false;
  replacement.RunOnce();
  view = replacement.ReadTargetView(kComponent).value();
  ASSERT_TRUE(view.progress->incident.has_value());
  EXPECT_EQ(view.progress->incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, AFreshVendorStopIsAnIncidentEvenBeforeTheReplacementsBaseline) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ports_.vendor_facts = {VendorToken("error", "C2")};
  ++ports_.observation_generation;
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  replacement.RunOnce();
  const auto view = replacement.ReadTargetView(kComponent).value();
  ASSERT_TRUE(view.progress->incident.has_value());
  EXPECT_EQ(view.progress->incident->cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, NoIncidentWhileAnApplyPlanForTheComponentIsUnadopted) {
  ReachTarget();
  // The Apply tears the provider down and may report anything meanwhile: the
  // change is requested, so neither a loss nor a vendor class is an incident.
  store_.applying.insert(kComponent);
  ports_.reachable = false;
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  ports_.reachable = true;
  ports_.cause = LifecycleBlockingCauseClassDTO::kExternalControl;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_TRUE(ports_.containments.empty());

  // Once adopted (here: retained unchanged), a fresh vendor change is one.
  store_.applying.clear();
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kExternalControl);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST(LifecycleIncidentPolicyTest, EveryInterventionClassRequiresTheOperatorsRecoverByDefault) {
  const auto policy = DefaultLifecycleIncidentRecoveryPolicy();
  for (const auto cause :
       {LifecycleBlockingCauseClassDTO::kEmergencyStop, LifecycleBlockingCauseClassDTO::kProtectiveStop,
        LifecycleBlockingCauseClassDTO::kSafeguardStop, LifecycleBlockingCauseClassDTO::kFault,
        LifecycleBlockingCauseClassDTO::kConfiguration, LifecycleBlockingCauseClassDTO::kUnknown,
        LifecycleBlockingCauseClassDTO::kCollision, LifecycleBlockingCauseClassDTO::kExternalControl,
        LifecycleBlockingCauseClassDTO::kProviderLost}) {
    const auto row = LifecycleIncidentRecoveryPolicyFor(policy, cause);
    EXPECT_EQ(row.cause_class, cause);
    EXPECT_TRUE(row.acknowledgement_required) << to_string(cause);
    EXPECT_FALSE(row.recovery_steps.empty());
  }
  EXPECT_TRUE(LifecycleIncidentRecoveryPolicyFor(policy, LifecycleBlockingCauseClassDTO::kProviderLost)
                  .provider_restart_after_acknowledgement);
  // A table without the class fails closed.
  EXPECT_TRUE(LifecycleIncidentRecoveryPolicyFor({}, LifecycleBlockingCauseClassDTO::kFault)
                  .acknowledgement_required);
  EXPECT_EQ(ComponentIncidentVendorCodes({VendorToken("error", "C2"), VendorInteger("mode", 0),
                                          VendorToken("power_cycle", "power_cycle_required"),
                                          {LifecycleFactId("external.fault_present"),
                                           Boolean(true)}}),
            (std::vector<std::string>{"C2", "mode=0", "power_cycle_required"}));
}

TEST_F(LifecycleTargetDriverTest, SystemRequestIdentityIsDeterministicAndBounded) {
  EXPECT_EQ(LifecycleTargetRequestId(kComponent, 4U, 1U, ComponentLifecycleIntentKindDTO::kEnable,
                                     2U),
            RequestId("manyforge_lifecycle_target.component.arm.g4.s1.enable.a2"));
  const ControllableComponentId long_component(std::string(128U, 'c'));
  const auto first =
      LifecycleTargetRequestId(long_component, 1U, 0U, ComponentLifecycleIntentKindDTO::kPrepare, 1U);
  EXPECT_EQ(first, LifecycleTargetRequestId(long_component, 1U, 0U,
                                            ComponentLifecycleIntentKindDTO::kPrepare, 1U));
  EXPECT_LE(first.value().size(), kMaxLifecycleIdentityBytes);
  EXPECT_NE(first, LifecycleTargetRequestId(long_component, 1U, 0U,
                                            ComponentLifecycleIntentKindDTO::kPrepare, 2U));

  // A replacement driver over the same facts proposes the identical request.
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kError;
  Driver().RunOnce();
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  replacement.RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted[0], ports_.submitted[1]);
}

TEST_F(LifecycleTargetDriverTest, WorkerWakesOnEventsAndFallsBackOnTheScheduler) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ASSERT_TRUE(Driver().Start().ok());
  // The initial pass runs on the worker and arms the ~1 s fallback.
  for (int spin = 0; spin < 2000 && scheduler_.pending_count(SchedulerPriority::Lifecycle) == 0U;
       ++spin) {
    std::this_thread::yield();
  }
  EXPECT_EQ(scheduler_.pending_count(SchedulerPriority::Lifecycle), 1U);
  ports_.BecomeReady();
  scheduler_.advance_to(clock_.now() + kSecond);
  for (int spin = 0; spin < 2000000 && ports_.submit_count.load() == 0; ++spin) {
    std::this_thread::yield();
  }
  Driver().Stop();
  EXPECT_EQ(ports_.submitted.size(), 1U);
}

}  // namespace
}  // namespace manyforge::core

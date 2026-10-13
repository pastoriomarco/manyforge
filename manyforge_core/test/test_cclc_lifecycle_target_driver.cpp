#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/cclc_lifecycle_target_driver.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <thread>
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
    if (fail_load_targets) {
      return Result<std::vector<ComponentLifecycleTargetDTO>>::Err(Unavailable());
    }
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
  Result<ComponentLifecycleTargetFactsDTO> LoadLifecycleTargetFacts(
      const ControllableComponentId& component) override {
    if (fail_load_facts) {
      return Result<ComponentLifecycleTargetFactsDTO>::Err(Unavailable());
    }
    ComponentLifecycleTargetFactsDTO facts;
    if (const auto found = targets.find(component); found != targets.end()) {
      facts.target = found->second;
    }
    facts.recovery_generation = Generation(component);
    for (const auto& [key, cause] : causes) {
      if (key.first == component) facts.intervention_causes.push_back(cause);
    }
    if (const auto* open = Open(component)) {
      facts.open_incident = *open;
      if (omit_facts_diagnostic) {
        facts.open_incident->diagnostic.reset();
        facts.open_incident->diagnostic_snapshot.reset();
        facts.open_incident->incident.diagnostic.reset();
        facts.open_incident->incident.diagnostic_snapshot.reset();
        facts.open_incident->diagnostic_loaded = false;
      }
    }
    return Result<ComponentLifecycleTargetFactsDTO>::Ok(std::move(facts));
  }
  Result<bool> RecordInterventionCause(const ControllableComponentId& component,
                                       const ControlDomainId& domain,
                                       std::optional<LifecycleBlockingCauseClassDTO> cause,
                                       std::int64_t at, std::uint64_t generation) override {
    BeforeWrite();
    if (Generation(component) != generation) return Result<bool>::Err(StaleFacts());
    if (fail_record_cause) return Result<bool>::Err(Unavailable());
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
  Result<bool> RecordLifecycleTargetReached(const ControllableComponentId& component,
                                            bool reached, std::uint64_t generation) override {
    BeforeWrite();
    if (Generation(component) != generation) return Result<bool>::Err(StaleFacts());
    if (fail_record_reached) return Result<bool>::Err(Unavailable());
    const auto found = targets.find(component);
    if (found == targets.end() || found->second.reached == reached) return Result<bool>::Ok(false);
    ++writes;
    found->second.reached = reached;
    if (reached) found->second.recovered_cause.reset();
    return Result<bool>::Ok(true);
  }
  Result<bool> RecordLifecycleTargetReached(const ComponentLifecycleTargetExpectationDTO& expected,
                                            bool reached, std::uint64_t generation) override {
    BeforeWrite();
    const auto found = targets.find(expected.controllable_component_id);
    if (!expected.locally_valid() || found == targets.end() ||
        found->second.target_generation != expected.target_generation) {
      auto error = StaleFacts();
      error.details["reason"] = "target_retired_or_superseded";
      return Result<bool>::Err(std::move(error));
    }
    if (Generation(expected.controllable_component_id) != generation) {
      return Result<bool>::Err(StaleFacts());
    }
    if (fail_record_reached) return Result<bool>::Err(Unavailable());
    if (found->second.reached == reached) return Result<bool>::Ok(false);
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
  Result<ComponentIncidentRecordDTO> OpenComponentIncident(const ComponentIncidentRecordDTO& record,
                                                           std::uint64_t generation) override {
    BeforeWrite();
    if (Generation(record.incident.controllable_component_id) != generation) {
      return Result<ComponentIncidentRecordDTO>::Err(StaleFacts());
    }
    if (fail_open_incident) return Result<ComponentIncidentRecordDTO>::Err(Unavailable());
    if (const auto* open = Open(record.incident.controllable_component_id)) {
      return Result<ComponentIncidentRecordDTO>::Ok(*open);
    }
    ++writes;
    auto stored = record;
    stored.diagnostic =
        ComponentIncidentDiagnosticFromRecord(stored.record_json, &stored.diagnostic_snapshot);
    stored.incident.diagnostic = stored.diagnostic;
    stored.incident.diagnostic_snapshot = stored.diagnostic_snapshot;
    incidents.insert_or_assign(record.incident.incident_id, stored);
    return Result<ComponentIncidentRecordDTO>::Ok(record);
  }
  Result<std::optional<ComponentIncidentRecordDTO>> LoadOpenComponentIncident(
      const ControllableComponentId& component) override {
    const auto* open = Open(component);
    if (!open) return Result<std::optional<ComponentIncidentRecordDTO>>::Ok(std::nullopt);
    auto explicit_record = *open;
    if (explicit_record.incident.containment_state ==
        ComponentIncidentContainmentStateDTO::kRequested)
      explicit_record.incident.containment =
          ComponentIncidentContainmentReceiptFromRecord(explicit_record.record_json);
    return Result<std::optional<ComponentIncidentRecordDTO>>::Ok(std::move(explicit_record));
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
  /// Stand-in for the journal's Recover: exact replay by request id, then
  /// closing the open incident, clearing retained causes and the reached
  /// marker and remembering the recovered cause, atomically.
  Result<std::optional<CCLCLifecycleRequestResultDTO>> RecoverComponentIncident(
      const RequestOwnerEpoch& active_owner_epoch, const ComponentLifecycleIntentDTO& intent,
      const std::string& acknowledged_by, std::int64_t acknowledged_at_ns) override {
    using Value = std::optional<CCLCLifecycleRequestResultDTO>;
    const auto result = [](RequestReplayStatusDTO status, ComponentIncidentRecoveryDTO recovery) {
      CCLCLifecycleRequestResultDTO value{status, std::nullopt, std::nullopt, std::nullopt};
      value.incident_recovery = std::move(recovery);
      return value;
    };
    const auto& component = intent.controllable_component_id;
    if (intent.request_context.request_owner_epoch != active_owner_epoch) {
      return Result<Value>::Ok(std::nullopt);
    }
    for (const auto& [id, row] : incidents) {
      const auto& incident = row.incident;
      if (incident.controllable_component_id == component && incident.acknowledged &&
          incident.acknowledgement_request_id == intent.request_context.request_id) {
        return Result<Value>::Ok(result(
            RequestReplayStatusDTO::kReplayedExact,
            {id, component, incident.cause_class, *incident.acknowledged_by,
             *incident.acknowledged_at_ns}));
      }
    }
    auto* open = Open(component);
    if (open == nullptr) return Result<Value>::Ok(std::nullopt);
    ++writes;
    open->incident.acknowledged = true;
    open->incident.acknowledged_by = acknowledged_by;
    open->incident.acknowledged_at_ns = acknowledged_at_ns;
    open->incident.acknowledgement_request_id = intent.request_context.request_id;
    std::erase_if(causes, [&](const auto& row) { return row.first.first == component; });
    if (auto target = targets.find(component); target != targets.end()) {
      target->second.reached = false;
      target->second.recovered_cause = open->incident.cause_class;
    }
    ++recovery_generations[component];
    events.push_back({events.size() + 1U, std::string(kComponentIncidentAcknowledgedEventKind),
                      open->incident.incident_id, acknowledged_at_ns,
                      ComponentIncidentAcknowledgedPayload(open->record_json, open->incident)});
    return Result<Value>::Ok(result(RequestReplayStatusDTO::kAcceptedNew,
                                    {open->incident.incident_id, component,
                                     open->incident.cause_class, acknowledged_by,
                                     acknowledged_at_ns}));
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
  Result<bool> AppendComponentIncidentDiagnostic(
      const std::string& id, std::int64_t at, const ErrorDTO& diagnostic,
      std::shared_ptr<const content_identity::CanonicalValue> snapshot = {}) override {
    ++diagnostic_attempts;
    if (on_diagnostic) on_diagnostic();
    if (fail_diagnostic) return Result<bool>::Err(Unavailable());
    const auto found = incidents.find(id);
    if (found == incidents.end() || found->second.incident.acknowledged ||
        found->second.diagnostic_snapshot || (found->second.diagnostic && !snapshot))
      return Result<bool>::Ok(false);
    auto& record = found->second;
    const auto captured_error = record.diagnostic.value_or(diagnostic);
    const auto event =
        ComponentIncidentRecordWithChange(record.record_json, at, record.incident.cause_class,
                                          record.incident.vendor_codes, captured_error, snapshot);
    record.record_json =
        ComponentIncidentRecordWithDiagnostic(record.record_json, captured_error, snapshot);
    record.diagnostic = captured_error;
    record.diagnostic_snapshot = snapshot;
    record.incident.diagnostic = captured_error;
    record.incident.diagnostic_snapshot = snapshot;
    events.push_back({events.size() + 1U, std::string(kComponentIncidentEventKind), id, at, event});
    return Result<bool>::Ok(true);
  }
  bool fail_diagnostic{};
  bool omit_facts_diagnostic{};
  std::size_t diagnostic_attempts{};
  std::function<void()> on_diagnostic;
  Result<std::vector<ComponentIncidentEventDTO>> LoadComponentIncidentEvents(
      std::uint64_t after, std::size_t limit) override {
    std::vector<ComponentIncidentEventDTO> result;
    for (const auto& event : events) {
      if (event.sequence > after && result.size() < limit) result.push_back(event);
    }
    return Result<std::vector<ComponentIncidentEventDTO>>::Ok(result);
  }

  /// Stand-in for the journal's ordinary operator Recover acceptance (no
  /// open incident): clears the retained causes and fences older passes.
  void OrdinaryRecover(const ControllableComponentId& component) {
    std::erase_if(causes, [&](const auto& row) { return row.first.first == component; });
    ++recovery_generations[component];
  }
  std::uint64_t Generation(const ControllableComponentId& component) const {
    const auto found = recovery_generations.find(component);
    return found == recovery_generations.end() ? 0U : found->second;
  }
  static ErrorDTO Unavailable() {
    return {"component_lifecycle_journal_unavailable", "the journal could not be written", {}};
  }
  static ErrorDTO StaleFacts() {
    return {std::string(kLifecycleFactsStaleErrorCode), "recovered since the facts were read", {}};
  }
  /// Runs once, inside the next driver write and before its generation
  /// check: an operator Recover accepted between a pass's read and write.
  void BeforeWrite() {
    if (!before_write) return;
    auto hook = std::move(before_write);
    before_write = nullptr;
    hook();
  }

  /// Components the current unadopted Apply replaces or removes.
  std::set<ControllableComponentId> applying;
  bool fail_open_incident{};
  bool fail_load_facts{};
  bool fail_load_targets{};
  bool fail_record_cause{};
  bool fail_record_reached{};
  std::function<void()> before_write;
  std::map<ControllableComponentId, std::uint64_t> recovery_generations;
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
         observation_generation, now, now, now + kSecond, observed, std::move(facts), diagnostic,
         diagnostic_snapshot});
  }

  // Runs inside Observe before the observation is stamped, like the real
  // adapter whose sampling takes time on the owner clock.
  std::function<void()> on_observe;
  std::optional<ErrorDTO> diagnostic;
  std::shared_ptr<const content_identity::CanonicalValue> diagnostic_snapshot;

  AuthoringAdmissionDTO ReadCellAdmission() override {
    return {authoring_allowed, AdmissionGeneration(admission_generation)};
  }
  RequestOwnerEpoch ReadRequestOwnerEpoch() override { return RequestOwnerEpoch("cclc.owner.1"); }
  bool AcceptingLifecycleRequests() override { return accepting; }
  bool ComponentLifecycleSettling(const ControllableComponentId&) override { return settling; }
  bool RetiredTargetWorkSettled(const ControllableComponentId&) override {
    ++retired_settlement_reads;
    return !settling && owner_settled;
  }

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
    if (lose_accepted_reply) {
      lose_accepted_reply = false;
      return Result<CCLCLifecycleRequestResultDTO>::Err(
          {"cclc_open_request_failure", "accepted reply lost", {}});
    }
    return Result<CCLCLifecycleRequestResultDTO>::Ok(
        {RequestReplayStatusDTO::kAcceptedNew, Accepted(intent, id), std::nullopt});
  }

  Result<CCLCLifecycleRequestResultDTO> SubmitTargetLifecycle(
      const ComponentLifecycleIntentDTO& intent,
      const ComponentLifecycleTargetExpectationDTO& expected) override {
    submitted_targets.push_back(expected);
    if (on_target_submit) on_target_submit();
    if (target_is_current && !target_is_current(expected)) {
      return Result<CCLCLifecycleRequestResultDTO>::Ok(
          {RequestReplayStatusDTO::kAcceptedNew, std::nullopt, std::nullopt,
           ErrorDTO{"lifecycle_target_stale", "target retired before admission", {}}});
    }
    return SubmitLifecycle(intent);
  }
  Result<std::optional<CCLCLifecycleRequestResultDTO>> ReadLifecycleRequestResult(
      const ComponentLifecycleIntentDTO& intent) override {
    observed_requests.push_back(intent);
    using Value = std::optional<CCLCLifecycleRequestResultDTO>;
    if (fail_request_read) return Result<Value>::Err({"request_read_failed", "unavailable", {}});
    if (request_read_override) return Result<Value>::Ok(request_read_override);
    for (const auto& [id, transaction] : accepted) {
      if (transaction.first == intent.request_context.request_id && transaction.second == intent) {
        return Result<Value>::Ok(CCLCLifecycleRequestResultDTO{
            RequestReplayStatusDTO::kReplayedExact, Accepted(intent, id), std::nullopt});
      }
    }
    return Result<Value>::Ok(std::nullopt);
  }
  std::optional<bool> ConfiguredComponentMembership(const ControllableComponentId&) const override {
    if (throw_membership_read) throw std::runtime_error("membership read failed");
    return configured_membership;
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
  bool owner_settled{true};
  unsigned retired_settlement_reads{};
  std::uint64_t admission_generation{3U};
  SubmitMode submit_mode{SubmitMode::kAccept};
  std::vector<LifecycleFactDTO> vendor_facts;
  bool containment_accepts{true};
  std::vector<ComponentIncidentContainmentIntentDTO> containments;

  std::vector<ComponentLifecycleIntentDTO> submitted;
  std::vector<ComponentLifecycleTargetExpectationDTO> submitted_targets;
  std::vector<ComponentLifecycleIntentDTO> observed_requests;
  std::optional<CCLCLifecycleRequestResultDTO> request_read_override;
  bool fail_request_read{};
  bool lose_accepted_reply{};
  std::optional<bool> configured_membership;
  bool throw_membership_read{};
  std::function<void()> on_target_submit;
  std::function<bool(const ComponentLifecycleTargetExpectationDTO&)> target_is_current;
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
  LifecycleTargetDriverTest() : ports_(clock_) {
    clock_.set(100 * kSecond);
    ports_.target_is_current = [&](const ComponentLifecycleTargetExpectationDTO& expected) {
      const auto found = store_.targets.find(expected.controllable_component_id);
      return found != store_.targets.end() && found->second.target_generation == expected.target_generation;
    };
  }

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

  FakeClock clock_;
  ManualScheduler scheduler_;
  FakeStore store_;
  FakePorts ports_;
  std::optional<CCLCLifecycleTargetDriver> driver_;
};

[[nodiscard]] std::shared_ptr<const content_identity::CanonicalValue>
TestLaunchDiagnosticSnapshot() {
  return std::make_shared<
      const content_identity::CanonicalValue>(content_identity::parse_canonical_json(
      R"snapshot({"context":{"bundle_id":"bundle.1","component_id":"component.arm","generator_identity":null,"graph_revision":"graph.1","launch_identity":"sha256:0000000000000000000000000000000000000000000000000000000000000000","request_context":"Apply=apply.7","runtime_epoch":"epoch.1","runtime_unit_id":"robot_cell.ros2_control_execution"},"core_dump_link":null,"exit":{"core_dumped":false,"observed_utc":"2026-10-11T18:00:00Z","signal":11,"status":null},"generated":{"controllers_sha256":"sha256:2222222222222222222222222222222222222222222222222222222222222222","robot_description_sha256":"sha256:1111111111111111111111111111111111111111111111111111111111111111"},"output":{"dropped_forward_bytes":0,"dropped_forward_lines":0,"invalid_text_bytes":0,"line_count":1,"tail":"controller crashed\n","truncated":false},"readiness":{"arm_controller_active":null,"binding_epoch":null,"captured_utc":null,"controller_bound":null,"controller_loaded":null,"controller_manager_present":null,"controller_observed":null,"hardware_ready":null,"joint_sample_available":null,"known":null,"sample_age_ns":null},"settings":{"argv":["ros2","launch"],"arm_controller_name":"arm","controller_profile":"ur5","controllers_yaml":"arm: effective settings","frame_prefix":"","gripper_controller_name":null,"retained_configuration_json":"{}","runtime_namespace":"/robot"}})snapshot"));
}

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

  // The journal records the operator target in the commit that accepts the
  // Disable; the open-request port then only wakes the driver.
  const auto recorded = store_.SetLifecycleTarget(kComponent, LifecycleTargetStateDTO::kDisabled,
                                                  LifecycleTargetOriginDTO::kOperator);
  ASSERT_TRUE(recorded.ok());
  EXPECT_EQ(recorded.value().target_generation, 2U);
  Driver().Wake();

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
  EXPECT_EQ(store_.targets.at(kComponent).requested_state, LifecycleTargetStateDTO::kDisabled);
}

TEST_F(LifecycleTargetDriverTest, SupersedingTargetObservesOriginalUnknownRequestBeforeFollowingSuccessor) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kError;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  const auto original = ports_.submitted.front();
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.submit_mode = SubmitMode::kAccept;
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
  ASSERT_EQ(ports_.observed_requests.size(), 1U);
  EXPECT_EQ(ports_.observed_requests.back(), original);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);

  // Only exact readback proves this original request was rejected. A claim
  // still settling keeps the successor closed despite the retained rejection.
  ports_.request_read_override = CCLCLifecycleRequestResultDTO{
      RequestReplayStatusDTO::kReplayedExact, std::nullopt, std::nullopt,
      ErrorDTO{"lifecycle_target_stale", "original target retired", {}}};
  ports_.settling = true;
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  ports_.settling = false;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 2U);
  EXPECT_EQ(ports_.submitted.back().request_context.request_id.value(),
            "manyforge_lifecycle_target.component.arm.g2.s0.prepare.a1");
}

TEST_F(LifecycleTargetDriverTest, RemovedTargetKeepsAcceptedWorkUntilExactStatusAndOwnerSettle) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  const auto observations = ports_.observations;
  store_.targets.clear();
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kTransitionInProgress);
  EXPECT_FALSE(Driver().ReadTargetView(kComponent).value().target);
  EXPECT_EQ(ports_.observations, observations);
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  ports_.settling = true;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  ports_.settling = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  EXPECT_EQ(Progress().blocking_reason, "target_retired:original_work_settled");
  EXPECT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.observations, observations);
}

TEST_F(LifecycleTargetDriverTest, RetirementBetweenFactsAndAcceptanceUsesActualTargetExpectation) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.on_target_submit = [&] { store_.targets.clear(); };
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted_targets.size(), 1U);
  EXPECT_EQ(ports_.submitted_targets.front(), (ComponentLifecycleTargetExpectationDTO{kComponent, 1U}));
  EXPECT_TRUE(ports_.accepted.empty());
  EXPECT_TRUE(ports_.submitted.empty());
  Driver().RunOnce();
  EXPECT_FALSE(Driver().ReadTargetView(kComponent).value().target);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  EXPECT_EQ(ports_.submitted_targets.size(), 1U);
}

TEST_F(LifecycleTargetDriverTest, RetiredUnknownRequestsAreReadOnlyOnMissingErrorAndExpiredResults) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.submit_mode = SubmitMode::kError;
  Driver().RunOnce();
  const auto original = ports_.submitted.front();
  store_.targets.clear();
  for (unsigned mode = 0U; mode < 3U; ++mode) {
    ports_.fail_request_read = mode == 1U;
    if (mode == 2U) {
      ports_.request_read_override = CCLCLifecycleRequestResultDTO{
          RequestReplayStatusDTO::kRequestResultExpired, std::nullopt, std::nullopt};
    }
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
    EXPECT_FALSE(Driver().ReadTargetView(kComponent).value().target);
    EXPECT_EQ(ports_.submitted.size(), 1U);
    EXPECT_EQ(ports_.observed_requests.back(), original);
  }
}

TEST_F(LifecycleTargetDriverTest, RetiredLostAcceptanceReplyReadsOriginalTransactionWithoutResubmission) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.lose_accepted_reply = true;
  Driver().RunOnce();
  ASSERT_EQ(ports_.accepted.size(), 1U);
  const auto original = ports_.submitted.front();
  store_.targets.clear();
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kTransitionInProgress);
  ASSERT_EQ(ports_.observed_requests.size(), 1U);
  EXPECT_EQ(ports_.observed_requests.back(), original);
  EXPECT_EQ(ports_.submitted.size(), 1U);
  ports_.Finish(ComponentLifecycleTerminalResultDTO::kSucceeded,
                ComponentLifecycleTransactionPhaseDTO::kSucceeded);
  ports_.owner_settled = false;
  ports_.configured_membership = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  EXPECT_TRUE(Driver().ReadTargetView(kComponent).value().progress);
  ports_.owner_settled = true;
  ports_.configured_membership.reset();
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  EXPECT_EQ(ports_.submitted.size(), 1U);
}

TEST_F(LifecycleTargetDriverTest, MissingRetirementOwnerProofCannotResetASuccessor) {
  EXPECT_FALSE(ports_.ICCLCLifecycleTargetDriverPorts::RetiredTargetWorkSettled(kComponent));
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  Driver().RunOnce();
  Target(LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator);
  ports_.BecomeReady();
  ports_.owner_settled = false;
  Driver().RunOnce();
  EXPECT_TRUE(ports_.submitted.empty());
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  ASSERT_GT(ports_.retired_settlement_reads, 0U);
  ports_.owner_settled = true;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted_targets.size(), 1U);
  EXPECT_EQ(ports_.submitted_targets.back().target_generation, 2U);
}

TEST_F(LifecycleTargetDriverTest, CurrentTargetPursuitDoesNotRequireRetiredOwnerProof) {
  Target(LifecycleTargetStateDTO::kPrepared, LifecycleTargetOriginDTO::kOperator);
  ports_.BecomeReady();
  ports_.owner_settled = false;
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.retired_settlement_reads, 0U);
}

TEST_F(LifecycleTargetDriverTest, FinishedAbsentStateEvictsOnlyOnVerifiedPinnedMembershipExclusion) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  Driver().RunOnce();
  store_.targets.clear();
  for (const auto membership : {std::optional<bool>{}, std::optional<bool>{true}}) {
    ports_.configured_membership = membership;
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  }
  ports_.throw_membership_read = true;
  EXPECT_NO_THROW(Driver().RunOnce());
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  ports_.throw_membership_read = false;
  ports_.configured_membership = false;
  Driver().RunOnce();
  EXPECT_FALSE(Driver().ReadTargetView(kComponent).value().progress);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleTargetDriverTest, InterventionCauseIsRetainedUntilTheOperatorsRecover) {
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

  // Fresh normal status alone does not clear it (ADR-0026 §7): no Recover yet.
  ports_.cause = LifecycleBlockingCauseClassDTO::kNone;
  ++ports_.observation_generation;
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(store_.causes.size(), 1U);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_TRUE(ports_.submitted.empty());

  // The operator's Recover is accepted by CCLC; the journal clears the
  // retained class in that acceptance commit and the target is unchanged
  // (stand-in below). While that transaction settles, the adapter still
  // reports the stop the Recover step is about to clear; it is not retained
  // again.
  ports_.cause = LifecycleBlockingCauseClassDTO::kProtectiveStop;
  ++ports_.observation_generation;
  store_.causes.clear();
  ports_.settling = true;
  Driver().RunOnce();
  EXPECT_TRUE(store_.causes.empty());
  EXPECT_TRUE(ports_.submitted.empty());

  // The Recover step cleared the stop on fresh status: continue.
  ports_.settling = false;
  ports_.cause = LifecycleBlockingCauseClassDTO::kNone;
  ++ports_.observation_generation;
  Driver().RunOnce();
  EXPECT_TRUE(store_.causes.empty());
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kPrepare);
  EXPECT_EQ(store_.targets.at(kComponent).requested_state, LifecycleTargetStateDTO::kActive);
  EXPECT_EQ(store_.targets.at(kComponent).target_generation, 1U);
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
  void ExpectNoMotionOrRecoverySubmitted() {
    // The driver submits lifecycle transactions only; it never issues an
    // operation, resumes motion or submits a Recover for an incident.
    for (const auto& intent : ports_.submitted) {
      EXPECT_NE(intent.intent, ComponentLifecycleIntentKindDTO::kRecover);
      EXPECT_EQ(intent.request_context.source_label, kLifecycleTargetSourceLabel);
    }
  }
};

// Read-only test adapter over the real driver's public view and the retained
// journal facts. It exercises the same canonical status API the host serves.
class IncidentStatusPort final : public ICCLCOpenRequestPort {
 public:
  IncidentStatusPort(CCLCLifecycleTargetDriver& driver, FakePorts& ports, FakeStore& store)
      : driver_(driver), ports_(ports), store_(store) {}
  RequestOwnerEpoch ReadRequestOwnerEpoch() const override {
    return RequestOwnerEpoch("cclc.owner.1");
  }
  Result<CCLCLifecycleRequestResultDTO> SubmitLifecycle(
      const ComponentLifecycleIntentDTO& intent) override {
    return ports_.SubmitLifecycle(intent);
  }
  Result<std::optional<ComponentLifecycleStatusDTO>> LoadLifecycleStatus(
      const LifecycleTransactionId&) override {
    return Result<std::optional<ComponentLifecycleStatusDTO>>::Ok(std::nullopt);
  }
  Result<ComponentLifecycleCurrentSnapshotDTO> ReadCurrentComponent(
      const ControllableComponentId& id) const override {
    auto snapshot = ports_.ReadCurrentComponent(id);
    if (!snapshot.ok()) return snapshot;
    const auto digest = ComputeComponentLifecycleCurrentSnapshotDigest(snapshot.value());
    if (!digest.ok()) return Result<ComponentLifecycleCurrentSnapshotDTO>::Err(digest.error());
    snapshot.value().snapshot_digest = digest.value();
    return snapshot;
  }
  Result<ComponentLifecycleTargetViewDTO> ReadLifecycleTargetView(
      const ControllableComponentId& id) const override {
    return driver_.ReadTargetView(id);
  }
  Result<std::vector<ComponentIncidentEventDTO>> ReadIncidentEvents(std::uint64_t after,
                                                                    std::size_t limit) override {
    std::vector<ComponentIncidentEventDTO> events;
    for (const auto& event : store_.events)
      if (event.sequence > after && events.size() < limit) events.push_back(event);
    return Result<std::vector<ComponentIncidentEventDTO>>::Ok(std::move(events));
  }
  Result<CCLCRouteSelectionTemplateSetDTO> ReadRouteSelectionTemplates(
      const ControllableComponentId&) const override {
    return Result<CCLCRouteSelectionTemplateSetDTO>::Err({"unused", "unused", {}});
  }
  Result<CCLCRouteSelectionRequestResultDTO> SubmitRouteSelection(
      const RouteSelectionIntentDTO&) override {
    return Result<CCLCRouteSelectionRequestResultDTO>::Err({"unused", "unused", {}});
  }
  Result<std::optional<RouteSelectionStatusDTO>> LoadRouteSelectionStatus(
      const RouteSelectionTransactionId&) override {
    return Result<std::optional<RouteSelectionStatusDTO>>::Ok(std::nullopt);
  }

 private:
  CCLCLifecycleTargetDriver& driver_;
  FakePorts& ports_;
  FakeStore& store_;
};

TEST_F(LifecycleIncidentTest, LaunchExitDiagnosticSurvivesStatusJournalAndDriverReplacement) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  ports_.diagnostic = ErrorDTO{
      "controller_launch_exited",
      "component=component.arm signal=11 observed_utc=2026-10-11T18:00:00Z Apply=apply.7 "
      "cached controller: active=1 bound=1 sample_age_ns=23 log=/launch/controller_launch.log "
      "settings=sha256:abc tail=controller crashed",
      {{"log_path", "/launch/controller_launch.log"}}};
  ports_.diagnostic_snapshot = TestLaunchDiagnosticSnapshot();
  ++ports_.observation_generation;
  Driver().RunOnce();  // Primary incident/containment precede enrichment.
  Driver().RunOnce();
  const auto progress = Progress();
  ASSERT_TRUE(progress.incident);
  const auto id = progress.incident->incident_id;
  EXPECT_EQ(progress.incident->summary.find("signal=11"), std::string::npos);
  ASSERT_TRUE(progress.blocking_reason);
  EXPECT_NE(progress.blocking_reason->find("Apply=apply.7"), std::string::npos);
  EXPECT_LE(progress.blocking_reason->size(), kMaxLifecycleErrorMessageBytes);
  ASSERT_EQ(store_.events.size(), 2U);
  const auto original = content_identity::parse_canonical_json(store_.events.front().payload_json);
  ASSERT_NE(original.find_object_member("diagnostic"), nullptr);
  EXPECT_TRUE(original.find_object_member("diagnostic")->find_object_member("snapshot")->is_null());
  EXPECT_EQ(original.find_object_member("diagnostic")
                ->find_object_member("error")
                ->find_object_member("message")
                ->as_string(),
            ports_.diagnostic->message);
  const auto record = content_identity::parse_canonical_json(store_.events.back().payload_json);
  ASSERT_NE(record.find_object_member("diagnostic"), nullptr);
  EXPECT_EQ(record.find_object_member("diagnostic")
                ->find_object_member("error")
                ->find_object_member("message")
                ->as_string(),
            ports_.diagnostic->message);
  EXPECT_NE(store_.incidents.at(id).record_json.find("controller_launch_exited"),
            std::string::npos);
  ASSERT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());

  // Event retention is auxiliary: explicit status reads the durable row.
  store_.events.clear();
  CCLCLifecycleTargetDriver replacement({}, store_, ports_, clock_, scheduler_);
  IncidentStatusPort api(replacement, ports_, store_);
  const auto status = ExecuteCanonicalCCLCOpenRequest(
      R"({"api_version":"manyforge.cclc_open_request.v1","command":"component_snapshot","payload":{"controllable_component_id":"component.arm"}})",
      api, {});
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_NE(status.value().find(id), std::string::npos);
  EXPECT_NE(status.value().find("signal=11"), std::string::npos);
  EXPECT_NE(status.value().find("controller_launch.log"), std::string::npos);
  const auto events = ExecuteCanonicalCCLCOpenRequest(
      R"({"api_version":"manyforge.cclc_open_request.v1","command":"incident_events","payload":{"after_sequence":0,"limit":10}})",
      api, {});
  ASSERT_TRUE(events.ok()) << events.error().message;
  EXPECT_EQ(events.value().find(id), std::string::npos);
  EXPECT_NE(status.value().find("arm: effective settings"), std::string::npos);
  EXPECT_NE(status.value().find("controller crashed"), std::string::npos);
  EXPECT_NE(status.value().find("robot_description_sha256"), std::string::npos);
  EXPECT_NE(status.value().find("log_path"), std::string::npos);
  EXPECT_EQ(events.value().find("controller_launch_exited"), std::string::npos);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, OutputCounterReadIsExplicitAndInformational) {
  CCLCLifecycleTargetDriverConfig config;
  std::size_t reads{};
  bool unavailable{};
  config.read_output_drop_counts = [&](const ControllableComponentId& component)
      -> std::optional<std::pair<std::uint64_t, std::uint64_t>> {
    EXPECT_EQ(component, kComponent);
    ++reads;
    if (unavailable) throw std::runtime_error("optional counts unavailable");
    return std::pair<std::uint64_t, std::uint64_t>{0U, std::numeric_limits<std::uint64_t>::max()};
  };
  CCLCLifecycleTargetDriver driver(config, store_, ports_, clock_, scheduler_);
  driver.RunOnce();
  EXPECT_EQ(reads, 0U);
  const auto view = driver.ReadTargetView(kComponent);
  ASSERT_TRUE(view.ok());
  EXPECT_EQ(view.value().dropped_output_lines, 0U);
  EXPECT_EQ(view.value().dropped_output_bytes, std::numeric_limits<std::uint64_t>::max());
  unavailable = true;
  const auto unavailable_view = driver.ReadTargetView(kComponent);
  ASSERT_TRUE(unavailable_view.ok());
  EXPECT_FALSE(unavailable_view.value().dropped_output_lines);
  EXPECT_FALSE(unavailable_view.value().dropped_output_bytes);
  EXPECT_TRUE(ports_.containments.empty());
}

TEST_F(LifecycleIncidentTest, ShortExitErrorIsDurableWithoutRichCapture) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  ports_.diagnostic = ErrorDTO{
      "controller_launch_exited", "signal=11 Apply=apply.7", {{"component", "component.arm"}}};
  ports_.diagnostic_snapshot.reset();
  ++ports_.observation_generation;
  Driver().RunOnce();
  Driver().RunOnce();
  const auto original = store_.events.front();
  ASSERT_EQ(store_.events.size(), 1U);
  ASSERT_TRUE(Progress().incident && Progress().incident->diagnostic);
  EXPECT_EQ(Progress().incident->diagnostic, ports_.diagnostic);
  EXPECT_FALSE(Progress().incident->diagnostic_snapshot);
  CCLCLifecycleTargetDriver replacement({}, store_, ports_, clock_, scheduler_);
  IncidentStatusPort api(replacement, ports_, store_);
  const auto status = ExecuteCanonicalCCLCOpenRequest(
      R"({"api_version":"manyforge.cclc_open_request.v1","command":"component_snapshot","payload":{"controllable_component_id":"component.arm"}})",
      api, {});
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_NE(status.value().find("signal=11 Apply=apply.7"), std::string::npos);
  EXPECT_NE(status.value().find("\"snapshot\":null"), std::string::npos);
  EXPECT_EQ(store_.events.front(), original);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, LaterLaunchExitKeepsTheGuardIncidentAndSurvivesReplacement) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;  // Guard trips first.
  ++ports_.observation_generation;
  Driver().RunOnce();
  const auto initial = Progress();
  ASSERT_TRUE(initial.incident);
  const auto id = initial.incident->incident_id;
  const auto summary = initial.incident->summary;
  ASSERT_EQ(store_.events.size(), 1U);
  const auto original_event = store_.events.front();
  ports_.diagnostic =
      ErrorDTO{"controller_launch_exited", "signal=11 Apply=apply.7 tail=crash", {}};
  ports_.diagnostic_snapshot.reset();
  ++ports_.observation_generation;
  // Diagnostic journal failure leaves the primary incident/containment intact.
  store_.fail_diagnostic = true;
  Driver().RunOnce();
  EXPECT_EQ(Progress().incident->incident_id, id);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_EQ(store_.events.size(), 1U);
  store_.fail_diagnostic = false;
  Driver().RunOnce();
  ASSERT_EQ(store_.events.size(), 2U);
  const auto short_event = store_.events.back();
  EXPECT_TRUE(content_identity::parse_canonical_json(short_event.payload_json)
                  .find_object_member("diagnostic")
                  ->find_object_member("snapshot")
                  ->is_null());
  EXPECT_FALSE(Progress().incident->diagnostic_snapshot);
  const auto first_error = Progress().incident->diagnostic;
  ASSERT_TRUE(first_error);
  ports_.diagnostic = ErrorDTO{"controller_launch_exited", "later replacement message", {}};
  ports_.diagnostic_snapshot = TestLaunchDiagnosticSnapshot();
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_EQ(store_.events.size(), 3U);
  EXPECT_EQ(store_.events[1], short_event);
  EXPECT_EQ(Progress().incident->diagnostic, first_error);
  ASSERT_TRUE(Progress().incident->diagnostic_snapshot);
  EXPECT_EQ(store_.events.front(), original_event);
  EXPECT_EQ(Progress().incident->summary, summary);
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kFault);
  for (int pass = 0; pass < 4; ++pass) {
    ++ports_.observation_generation;
    Driver().RunOnce();
  }
  EXPECT_EQ(store_.events.size(), 3U);
  CCLCLifecycleTargetDriver replacement({}, store_, ports_, clock_, scheduler_);
  IncidentStatusPort api(replacement, ports_, store_);
  const auto status = ExecuteCanonicalCCLCOpenRequest(
      R"({"api_version":"manyforge.cclc_open_request.v1","command":"component_snapshot","payload":{"controllable_component_id":"component.arm"}})",
      api, {});
  ASSERT_TRUE(status.ok()) << status.error().message;
  EXPECT_NE(status.value().find(id), std::string::npos);
  EXPECT_NE(status.value().find("signal=11"), std::string::npos);
  EXPECT_NE(status.value().find(summary), std::string::npos);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, RoutineFactsReuseExactDiagnosticWhileRefreshingPrimaryMetadata) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  ports_.diagnostic = ErrorDTO{"controller_launch_exited", "signal=11", {}};
  ports_.diagnostic_snapshot = TestLaunchDiagnosticSnapshot();
  ++ports_.observation_generation;
  Driver().RunOnce();
  Driver().RunOnce();
  const auto snapshot = Progress().incident->diagnostic_snapshot;
  ASSERT_TRUE(snapshot);
  const auto attempts = store_.diagnostic_attempts;
  store_.omit_facts_diagnostic = true;
  auto& row = store_.incidents.begin()->second;
  row.incident.vendor_codes = {"new_vendor_fact"};
  row.incident.containment_state = ComponentIncidentContainmentStateDTO::kFailed;
  for (int pass = 0; pass < 4; ++pass) {
    ++ports_.observation_generation;
    Driver().RunOnce();
    EXPECT_EQ(Progress().incident->diagnostic_snapshot.get(), snapshot.get());
    EXPECT_EQ(Progress().incident->vendor_codes, row.incident.vendor_codes);
    EXPECT_EQ(Progress().incident->containment_state,
              ComponentIncidentContainmentStateDTO::kFailed);
  }
  EXPECT_EQ(store_.diagnostic_attempts, attempts);
  CCLCLifecycleTargetDriver replacement({}, store_, ports_, clock_, scheduler_);
  replacement.RunOnce();
  const auto persisted = replacement.ReadTargetView(kComponent).value().progress->incident;
  ASSERT_TRUE(persisted->diagnostic_snapshot);
  EXPECT_EQ(content_identity::canonical_json(*persisted->diagnostic_snapshot),
            content_identity::canonical_json(*snapshot));
  // A replacement must probe/load once; later routine passes reuse its progress.
  const auto replacement_attempts = store_.diagnostic_attempts;
  replacement.RunOnce();
  EXPECT_EQ(store_.diagnostic_attempts, replacement_attempts);
}

TEST_F(LifecycleIncidentTest, ReplacementContainsPendingIncidentBeforeDiagnosticWriter) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  ++ports_.observation_generation;
  Driver().RunOnce();
  auto& stored = store_.incidents.begin()->second;
  stored.incident.containment_state = ComponentIncidentContainmentStateDTO::kPending;
  ports_.containments.clear();
  ports_.diagnostic = ErrorDTO{"controller_launch_exited", "signal=11 tail=crash", {}};
  ports_.diagnostic_snapshot = TestLaunchDiagnosticSnapshot();
  ++ports_.observation_generation;
  store_.fail_diagnostic = true;
  store_.on_diagnostic = [&] {
    ASSERT_EQ(ports_.containments.size(), 1U);
    EXPECT_EQ(ports_.containments.front().incident_id, stored.incident.incident_id);
  };
  CCLCLifecycleTargetDriver replacement({}, store_, ports_, clock_, scheduler_);
  replacement.RunOnce();
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_FALSE(stored.diagnostic);
  EXPECT_EQ(replacement.ReadTargetView(kComponent).value().progress->phase,
            LifecycleProgressPhaseDTO::kBlocked);
}

TEST_F(LifecycleIncidentTest, InvalidLogTextCannotEraseThePrimaryIncidentRecord) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  const std::vector<std::string> invalid_text{std::string("bad\xFF", 4U), "e\u0301",
                                              std::string("truncated\xC3", 10U)};
  for (const auto& text : invalid_text) {
    ports_.diagnostic = ErrorDTO{"controller_launch_exited", text, {}};
    ports_.diagnostic_snapshot = TestLaunchDiagnosticSnapshot();
    ++ports_.observation_generation;
    Driver().RunOnce();
    Driver().RunOnce();  // Exercise the rejected auxiliary append too.
    const auto progress = Progress();
    ASSERT_TRUE(progress.incident);
    EXPECT_EQ(progress.incident->cause_class, LifecycleBlockingCauseClassDTO::kFault);
    EXPECT_EQ(store_.incidents.size(), 1U);
    EXPECT_EQ(ports_.containments.size(), 1U);
    EXPECT_FALSE(store_.incidents.begin()->second.diagnostic);
  }
  EXPECT_EQ(store_.events.size(), 1U);
}

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

TEST_F(LifecycleIncidentTest, ReachedStatusSurvivesInSessionReplacementSoALossIsAnIncident) {
  ReachTarget();
  store_.writes = 0;
  // In-session owner replacement: a new driver over the same durable store
  // sees the unchanged component at its target and rewrites nothing.
  CCLCLifecycleTargetDriver replacement(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                        scheduler_);
  replacement.RunOnce();
  auto view = replacement.ReadTargetView(kComponent).value();
  EXPECT_EQ(view.progress->phase, LifecycleProgressPhaseDTO::kSatisfied);
  EXPECT_EQ(store_.writes, 0);
  EXPECT_TRUE(store_.targets.at(kComponent).reached);

  // The durable reached status decides, not the new driver's own history:
  // an unexpected loss is an incident at once.
  clock_.advance(kSecond);
  ports_.reachable = false;
  CCLCLifecycleTargetDriver second(CCLCLifecycleTargetDriverConfig{}, store_, ports_, clock_,
                                   scheduler_);
  second.RunOnce();
  view = second.ReadTargetView(kComponent).value();
  ASSERT_TRUE(view.progress->incident.has_value());
  EXPECT_EQ(view.progress->incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, AFreshVendorStopSeenFirstByAReplacementDriverIsAnIncident) {
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

TEST_F(LifecycleIncidentTest, ApplyExplainedLossOfAReplacedComponentIsWaitingNotAnIncident) {
  ReachTarget();
  // The recorded, not yet adopted Apply replaces this component: its provider
  // is torn down. That unavailability is requested.
  store_.applying.insert(kComponent);
  ports_.reachable = false;
  for (int second = 0; second < 40; ++second) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  auto progress = Progress();
  EXPECT_FALSE(progress.incident.has_value());
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kWaitingForPrerequisites);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_EQ(progress.blocking_reason, "apply_replacement_in_progress");
  // The readiness expectation only presents the delay: no incident deadline
  // and nothing to acknowledge.
  EXPECT_TRUE(progress.delayed);

  // Neither healthy status during the window nor a later `availability`
  // report ends the exemption; only the Apply facts do.
  ports_.reachable = true;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ports_.cause = LifecycleBlockingCauseClassDTO::kAvailability;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ports_.cause.reset();
  ports_.reachable = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_reason, "apply_replacement_in_progress");
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_TRUE(ports_.containments.empty());
  EXPECT_TRUE(ports_.submitted.empty());

  // Adoption: the changed component gets the next startup target and starts
  // a fresh startup phase, in which loss is plain `availability` waiting.
  store_.applying.clear();
  auto& target = store_.targets.at(kComponent);
  target.reached = false;
  target.origin = LifecycleTargetOriginDTO::kStartup;
  ++target.target_generation;
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kAvailability);
  EXPECT_TRUE(ports_.containments.empty());

  // Once it reaches its new target, an unexpected loss is an incident again.
  ports_.reachable = true;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(store_.targets.at(kComponent).reached);
  clock_.advance(kSecond);
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, InterventionClassesOfAReplacedComponentAreRetainedNotIncidents) {
  ReachTarget();
  // The unadopted Apply replaces this component: it is starting over, so a
  // reported stop, fault or configuration class opens no incident, but it is
  // retained and blocks until the operator's Recover.
  store_.applying.insert(kComponent);
  for (const auto cause :
       {LifecycleBlockingCauseClassDTO::kEmergencyStop, LifecycleBlockingCauseClassDTO::kProtectiveStop,
        LifecycleBlockingCauseClassDTO::kSafeguardStop, LifecycleBlockingCauseClassDTO::kFault,
        LifecycleBlockingCauseClassDTO::kConfiguration}) {
    const std::string name(to_string(cause));
    clock_.advance(kSecond);
    ports_.cause = cause;
    ++ports_.observation_generation;
    Driver().RunOnce();
    EXPECT_FALSE(Progress().incident.has_value()) << name;
    EXPECT_EQ(Progress().blocking_cause_class, cause) << name;
    ASSERT_EQ(store_.causes.size(), 1U) << name;
    EXPECT_EQ(store_.causes.begin()->second.cause_class, cause) << name;
  }
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_TRUE(ports_.containments.empty());

  // Fresh normal status does not clear the retained class, during the window
  // or after adoption starts the replaced component's fresh startup phase.
  ports_.cause.reset();
  ++ports_.observation_generation;
  Driver().RunOnce();
  store_.applying.clear();
  auto& target = store_.targets.at(kComponent);
  target.reached = false;
  ++target.target_generation;
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kConfiguration);
  EXPECT_FALSE(store_.targets.at(kComponent).reached);
  EXPECT_TRUE(ports_.containments.empty());
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, ARetainedComponentFollowsTheIncidentRulesDuringAnotherApply) {
  ReachTarget();
  // The unadopted Apply replaces a different component only; this one is
  // retained unchanged and keeps its reached status.
  store_.applying.insert(ControllableComponentId("component.other"));
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, UnrelatedProviderLossDuringAnotherComponentsApplyIsAnIncident) {
  ReachTarget();
  // The unadopted Apply replaces a different component only.
  store_.applying.insert(ControllableComponentId("component.other"));
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, JournalFailureStillRequestsContainmentAndRetriesTheRecord) {
  ReachTarget();
  store_.fail_open_incident = true;
  ports_.reachable = false;
  const auto detected_at = clock_.now();
  Driver().RunOnce();
  // Containment does not wait for, or depend on, the record.
  ASSERT_EQ(ports_.containments.size(), 1U);
  const auto contained = ports_.containments.front();
  EXPECT_EQ(contained.controllable_component_id, kComponent);
  EXPECT_EQ(contained.cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_TRUE(store_.incidents.empty());
  auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind(
                "incident_record_unavailable:component_lifecycle_journal_unavailable", 0U),
            0U);

  // Later passes retry only the record, never the containment.
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());

  // The journal recovers while the provider is back: the detected incident
  // is still recorded, with its original identity and the Stop reference.
  store_.fail_open_incident = false;
  ports_.reachable = true;
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_EQ(store_.incidents.size(), 1U);
  const auto& record = store_.incidents.begin()->second;
  EXPECT_EQ(record.incident.incident_id, contained.incident_id);
  EXPECT_EQ(record.incident.detected_at_ns, detected_at);
  EXPECT_EQ(record.incident.containment_state, ComponentIncidentContainmentStateDTO::kRequested);
  ASSERT_EQ(store_.events.size(), 1U);
  EXPECT_NE(store_.events.front().payload_json.find(
                R"("stop":{"stop_generation":1,"stop_id":"stop.1"})"),
            std::string::npos);
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->incident_id, contained.incident_id);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, InvalidIncidentRecordStillRequestsContainment) {
  ReachTarget();
  // An owner clock outside the record's nonnegative range makes the record
  // invalid; the cell is contained regardless and the failure is reported.
  clock_.set(-kSecond);
  ports_.reachable = false;
  Driver().RunOnce();
  // Reads reject a negative owner clock as well; read with a valid one.
  EXPECT_FALSE(Driver().ReadTargetView(kComponent).ok());
  clock_.set(200 * kSecond);
  ASSERT_EQ(ports_.containments.size(), 1U);
  EXPECT_EQ(ports_.containments.front().cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_TRUE(store_.incidents.empty());
  const auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind(
                "incident_record_unavailable:lifecycle_incident_invalid", 0U),
            0U);
  Driver().RunOnce();
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
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

// --- Fail-closed lifecycle storage (R4 foundation corrections) ---

TEST_F(LifecycleTargetDriverTest, UnrecordedStartupFaultBlocksUntilRecordedThenUntilRecover) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  ports_.cause = LifecycleBlockingCauseClassDTO::kFault;
  store_.fail_record_cause = true;
  Driver().RunOnce();
  auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kFault);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind(
                "intervention_cause_record_unavailable:component_lifecycle_journal_unavailable",
                0U),
            0U);
  EXPECT_TRUE(store_.causes.empty());

  // Fresh normal status afterwards: the unrecorded fault still blocks; the
  // component never progresses past it.
  ports_.cause.reset();
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    ++ports_.observation_generation;
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
    EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kFault);
  }
  EXPECT_TRUE(ports_.submitted.empty());

  // The journal recovers: the retried record makes it durable, and it stays
  // retained until the operator's Recover.
  store_.fail_record_cause = false;
  Driver().RunOnce();
  ASSERT_EQ(store_.causes.size(), 1U);
  EXPECT_EQ(store_.causes.begin()->second.cause_class, LifecycleBlockingCauseClassDTO::kFault);
  EXPECT_EQ(Progress().blocking_reason, "intervention_required:fault");
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    Driver().RunOnce();
  }
  EXPECT_EQ(store_.causes.size(), 1U);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_TRUE(ports_.submitted.empty());

  store_.OrdinaryRecover(kComponent);
  Driver().RunOnce();
  ASSERT_EQ(ports_.submitted.size(), 1U);
  EXPECT_EQ(ports_.submitted.front().intent, ComponentLifecycleIntentKindDTO::kPrepare);
}

TEST_F(LifecycleTargetDriverTest, UnreadableRetainedFactsBlockProgression) {
  // A fault retained durably earlier.
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.BecomeReady();
  store_.causes.insert_or_assign(
      std::pair(kComponent, kDomain),
      ComponentLifecycleInterventionCauseDTO{kComponent, kDomain,
                                             LifecycleBlockingCauseClassDTO::kFault, 1});
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kFault);

  // The facts become unreadable: never read as "nothing retained".
  store_.fail_load_facts = true;
  for (int pass = 0; pass < 3; ++pass) {
    clock_.advance(kSecond);
    ++ports_.observation_generation;
    Driver().RunOnce();
  }
  const auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  EXPECT_EQ(progress.blocking_cause_class, LifecycleBlockingCauseClassDTO::kFault);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind(
                "lifecycle_facts_unavailable:component_lifecycle_journal_unavailable", 0U),
            0U);
  EXPECT_TRUE(ports_.submitted.empty());

  // Readable again without a cause: the ordinary step follows.
  store_.fail_load_facts = false;
  store_.causes.clear();
  Driver().RunOnce();
  EXPECT_EQ(ports_.submitted.size(), 1U);
}

TEST_F(LifecycleIncidentTest, UnrecordedReachedMarkerIsRetriedAndClassifiedAsReached) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  ports_.BecomeReady();
  store_.fail_record_reached = true;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
  ASSERT_TRUE(Progress().blocking_reason.has_value());
  EXPECT_EQ(Progress().blocking_reason->rfind("reached_record_unavailable", 0U), 0U);
  EXPECT_FALSE(store_.targets.at(kComponent).reached);

  // Meanwhile the component is classified as reached: an unexpected loss is
  // an incident (contained; only the operator's Recover proceeds), never
  // ordinary waiting that would progress on its own.
  clock_.advance(kSecond);
  ports_.reachable = false;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_FALSE(store_.targets.at(kComponent).reached);

  // The marker is retried every pass until it is durable.
  store_.fail_record_reached = false;
  Driver().RunOnce();
  EXPECT_TRUE(store_.targets.at(kComponent).reached);
  EXPECT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, RetiredUnrecordedCauseAndIncidentRemainVisibleWithoutATarget) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kProtectiveStop;
  store_.fail_record_cause = true;
  store_.fail_open_incident = true;
  Driver().RunOnce();
  ASSERT_EQ(ports_.containments.size(), 1U);
  const auto observations = ports_.observations;
  store_.targets.clear();
  Driver().RunOnce();
  EXPECT_EQ(Progress().blocking_cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_EQ(ports_.observations, observations);
  store_.fail_record_cause = false;
  store_.fail_open_incident = false;
  Driver().RunOnce();
  const auto view = Driver().ReadTargetView(kComponent);
  ASSERT_TRUE(view.ok());
  EXPECT_FALSE(view.value().target);
  ASSERT_TRUE(view.value().progress);
  ASSERT_TRUE(view.value().progress->incident);
  EXPECT_EQ(view.value().progress->incident->cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_EQ(store_.causes.size(), 1U);
  EXPECT_EQ(ports_.containments.size(), 1U);
  EXPECT_TRUE(ports_.submitted.empty());
  driver_.reset();
  const auto replacement = Driver().ReadTargetView(kComponent);
  ASSERT_TRUE(replacement.ok());
  EXPECT_FALSE(replacement.value().target);
  ASSERT_TRUE(replacement.value().progress);
  EXPECT_EQ(replacement.value().progress->incident, view.value().progress->incident);
  EXPECT_EQ(ports_.containments.size(), 1U);
}

TEST_F(LifecycleIncidentTest, RetiredReachedObservationCannotMarkASuccessorAndKeepsExactReason) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  ports_.BecomeReady();
  store_.fail_record_reached = true;
  Driver().RunOnce();
  EXPECT_FALSE(store_.targets.at(kComponent).reached);
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator);
  ports_.settling = true;
  store_.fail_record_reached = false;
  Driver().RunOnce();
  EXPECT_FALSE(store_.targets.at(kComponent).reached);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
  store_.targets.clear();
  ports_.settling = false;
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
  EXPECT_EQ(Progress().blocking_reason,
            "target_retired:retired_target_reached_observation_not_applied");
  EXPECT_FALSE(Driver().ReadTargetView(kComponent).value().target);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, AtomicReachedRefusalIdentifiesRetirementAfterFactsRead) {
  for (const bool retry_marker : {false, true}) {
    SCOPED_TRACE(retry_marker);
    driver_.reset();
    store_.targets.clear();
    Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
    ports_.achieved = ComponentLifecycleStateDTO::kActive;
    ports_.BecomeReady();
    if (retry_marker) {
      store_.fail_record_reached = true;
      Driver().RunOnce();
      store_.fail_record_reached = false;
    }
    const auto original_generation = store_.targets.at(kComponent).target_generation;
    store_.before_write = [&] {
      Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kOperator);
    };
    ports_.owner_settled = false;
    Driver().RunOnce();
    EXPECT_EQ(store_.targets.at(kComponent).target_generation, original_generation + 1U);
    EXPECT_FALSE(store_.targets.at(kComponent).reached);
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
    EXPECT_EQ(Progress().blocking_reason,
              "target_retired:retired_target_reached_observation_not_applied");
    store_.targets.clear();
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kOutcomeUncertain);
    ports_.owner_settled = true;
    Driver().RunOnce();
    EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kCancelled);
    EXPECT_EQ(Progress().blocking_reason,
              "target_retired:retired_target_reached_observation_not_applied");
    EXPECT_TRUE(ports_.submitted.empty());
  }
}

TEST_F(LifecycleIncidentTest, RecoveryOnlyReachedRefusalDoesNotRetireCurrentTarget) {
  Target(LifecycleTargetStateDTO::kActive, LifecycleTargetOriginDTO::kStartup);
  ports_.achieved = ComponentLifecycleStateDTO::kActive;
  ports_.BecomeReady();
  const auto original_generation = store_.targets.at(kComponent).target_generation;
  store_.before_write = [&] { store_.OrdinaryRecover(kComponent); };
  Driver().RunOnce();
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
  EXPECT_EQ(store_.targets.at(kComponent).target_generation, original_generation);
  EXPECT_FALSE(store_.targets.at(kComponent).reached);
  Driver().RunOnce();
  EXPECT_TRUE(store_.targets.at(kComponent).reached);
  EXPECT_EQ(Progress().phase, LifecycleProgressPhaseDTO::kSatisfied);
  EXPECT_EQ(ports_.retired_settlement_reads, 0U);
  EXPECT_TRUE(ports_.submitted.empty());
}

TEST_F(LifecycleIncidentTest, RecoverAcceptedDuringAPassIsNotUndoneByItsStaleWrites) {
  ReachTarget();
  ports_.cause = LifecycleBlockingCauseClassDTO::kEmergencyStop;
  ports_.vendor_facts = {VendorToken("error", "C2")};
  ++ports_.observation_generation;
  Driver().RunOnce();
  ASSERT_TRUE(Progress().incident.has_value());
  const auto id = Progress().incident->incident_id;
  ASSERT_EQ(store_.causes.size(), 1U);

  // The next pass reads the open incident and retained cause; the adapter
  // now reports a different stop. The operator's Recover is accepted after
  // that read and before the pass's first write.
  clock_.advance(kSecond);
  ports_.cause = LifecycleBlockingCauseClassDTO::kProtectiveStop;
  ++ports_.observation_generation;
  bool recovered = false;
  store_.before_write = [&] {
    const auto result = Driver().RecoverIncident(OperatorRecover("operator.recover.1"));
    recovered = result.ok() && result.value().has_value();
  };
  Driver().RunOnce();
  ASSERT_TRUE(recovered);
  // The stale pass neither re-retained a cause nor reopened or extended the
  // incident the Recover closed.
  EXPECT_TRUE(store_.causes.empty());
  EXPECT_EQ(store_.Open(kComponent), nullptr);
  EXPECT_EQ(store_.incidents.size(), 1U);
  EXPECT_EQ(store_.incidents.at(id).incident.cause_class,
            LifecycleBlockingCauseClassDTO::kEmergencyStop);
  EXPECT_FALSE(Progress().incident.has_value());
  EXPECT_EQ(ports_.containments.size(), 1U);

  // The next pass evaluates the Recover's facts: the still-reported stop is
  // a new cause while recovering, so it is retained and a new incident.
  Driver().RunOnce();
  ASSERT_EQ(store_.causes.size(), 1U);
  EXPECT_EQ(store_.causes.begin()->second.cause_class,
            LifecycleBlockingCauseClassDTO::kProtectiveStop);
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_NE(Progress().incident->incident_id, id);
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kProtectiveStop);
  EXPECT_EQ(ports_.containments.size(), 2U);
  ExpectNoMotionOrRecoverySubmitted();
}

TEST_F(LifecycleIncidentTest, UnreadableTargetsBlockWithTheNextIntentNamed) {
  ReachTarget();
  store_.fail_load_targets = true;
  clock_.advance(kSecond);
  Driver().RunOnce();
  const auto progress = Progress();
  EXPECT_EQ(progress.phase, LifecycleProgressPhaseDTO::kBlocked);
  ASSERT_TRUE(progress.blocking_reason.has_value());
  EXPECT_EQ(progress.blocking_reason->rfind("lifecycle_targets_unavailable:", 0U), 0U);
  // `blocked` never publishes a null next intent: the one toward the last
  // target read.
  EXPECT_EQ(progress.next_intent, ComponentLifecycleIntentKindDTO::kEnable);
  ExpectNoMotionOrRecoverySubmitted();
}

TEST_F(LifecycleIncidentTest, IncidentDetectedOnFactsARecoverReplacedIsNotRecorded) {
  ReachTarget();
  // A loss is detected; an ordinary operator Recover is accepted after the
  // pass read its facts and before it records the incident.
  ports_.reachable = false;
  store_.before_write = [&] { store_.OrdinaryRecover(kComponent); };
  Driver().RunOnce();
  EXPECT_TRUE(store_.incidents.empty());
  EXPECT_EQ(Progress().blocking_reason, "lifecycle_facts_changed");
  EXPECT_EQ(Progress().next_intent, ComponentLifecycleIntentKindDTO::kRecover);
  // Containment does not wait for the record and is not withdrawn.
  EXPECT_EQ(ports_.containments.size(), 1U);

  // The next pass re-evaluates on the Recover's facts: the loss persists at
  // the reached target, so it is recorded as an incident then.
  clock_.advance(kSecond);
  Driver().RunOnce();
  ASSERT_EQ(store_.incidents.size(), 1U);
  ASSERT_TRUE(Progress().incident.has_value());
  EXPECT_EQ(Progress().incident->cause_class, LifecycleBlockingCauseClassDTO::kProviderLost);
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

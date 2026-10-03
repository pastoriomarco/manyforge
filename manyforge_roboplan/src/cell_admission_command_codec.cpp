#include "cell_admission_command_codec.hpp"

#include <manyforge_content_identity/content_identity.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend::detail {
namespace cm = manyforge::component_manager::detail;
namespace ci = manyforge::content_identity;
namespace core = manyforge::core;
namespace {

using Value = ci::CanonicalValue;
constexpr double kMaximumSafeInteger = 9'007'199'254'740'991.0;
constexpr std::size_t kMaximumPacketBytes =
    core::kMaximumCellAdmissionJsonRequestBytes;

[[noreturn]] void Fail(std::string message) {
  throw std::invalid_argument(std::move(message));
}
void Require(bool condition, std::string message) {
  if (!condition)
    Fail(std::move(message));
}
const Value &Member(const Value &v, std::string_view name) {
  Require(v.is_object(), "cell-admission DTO must be an object");
  const auto *result = v.find_object_member(name);
  Require(result != nullptr,
          "cell-admission DTO has an unknown or missing field");
  return *result;
}
void Exact(const Value &v, std::size_t size) {
  Require(v.is_object() && v.as_object().size() == size,
          "cell-admission DTO has an unknown or missing field");
}
std::string Text(const Value &v, std::string_view name,
                 std::size_t maximum = core::kMaximumCellAdmissionIdentityBytes) {
  const auto &x = Member(v, name);
  Require(x.is_string(), "cell-admission text field has the wrong type");
  Require(x.as_string().size() <= maximum,
          "cell-admission text field is oversized");
  return x.as_string();
}
bool Bool(const Value &v, std::string_view name) {
  const auto &x = Member(v, name);
  Require(x.is_bool(), "cell-admission boolean field has the wrong type");
  return x.as_bool();
}
std::uint64_t Counter(const Value &v, std::string_view name,
                      std::uint64_t minimum = 0U,
                      std::uint64_t maximum =
                          static_cast<std::uint64_t>(kMaximumSafeInteger)) {
  const auto &x = Member(v, name);
  Require(x.is_number(), "cell-admission counter has the wrong type");
  const auto n = x.as_number();
  Require(std::isfinite(n) && std::floor(n) == n && n >= minimum &&
              n <= maximum && n <= kMaximumSafeInteger,
          "cell-admission counter is out of range");
  return static_cast<std::uint64_t>(n);
}
const Value::Array &Array(const Value &v, std::string_view name) {
  const auto &x = Member(v, name);
  Require(x.is_array(), "cell-admission collection has the wrong type");
  Require(x.as_array().size() <= core::kMaximumCellAdmissionClaims,
          "cell-admission collection is oversized");
  return x.as_array();
}
template <class T> std::optional<T> Optional(const Value &v,
                                             std::string_view name,
                                             auto decode) {
  const auto &x = Member(v, name);
  if (x.is_null())
    return std::nullopt;
  return decode(x);
}

core::CellAdmissionClaimKindDTO ClaimKind(std::string_view v) {
  for (auto x : {core::CellAdmissionClaimKindDTO::kProgramCycle,
                 core::CellAdmissionClaimKindDTO::kAuthoringMutation,
                 core::CellAdmissionClaimKindDTO::kManagerApply,
                 core::CellAdmissionClaimKindDTO::kComponentLifecycle,
                 core::CellAdmissionClaimKindDTO::kAuthoredScene,
                 core::CellAdmissionClaimKindDTO::kManualOperation,
                 core::CellAdmissionClaimKindDTO::kInterventionOrRecovery})
    if (core::to_string(x) == v)
      return x;
  Fail("unknown cell-admission claim_kind");
}
core::CellAdmissionOwnerRoleDTO OwnerRole(std::string_view v) {
  for (auto x : {core::CellAdmissionOwnerRoleDTO::kProgramRunner,
                 core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
                 core::CellAdmissionOwnerRoleDTO::kManager,
                 core::CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner,
                 core::CellAdmissionOwnerRoleDTO::kAuthoredSceneOwner,
                 core::CellAdmissionOwnerRoleDTO::kManualOperationOwner,
                 core::CellAdmissionOwnerRoleDTO::
                     kInterventionOrRecoveryOwner})
    if (core::to_string(x) == v)
      return x;
  Fail("unknown cell-admission owner_role");
}
core::ProgramCycleStateDTO CycleState(std::string_view v) {
  for (auto x : {core::ProgramCycleStateDTO::kIdleAfterReset,
                 core::ProgramCycleStateDTO::kStarting,
                 core::ProgramCycleStateDTO::kRunning,
                 core::ProgramCycleStateDTO::kStopping,
                 core::ProgramCycleStateDTO::kTerminalAwaitingReset,
                 core::ProgramCycleStateDTO::kInterventionOrRecovery,
                 core::ProgramCycleStateDTO::kResetting,
                 core::ProgramCycleStateDTO::kRecoveryRequired})
    if (core::to_string(x) == v)
      return x;
  Fail("unknown cell-admission program-cycle state");
}
core::CellAdmissionSettlementKindDTO SettlementKind(std::string_view v) {
  for (auto x : {core::CellAdmissionSettlementKindDTO::kOwnerCommit,
                 core::CellAdmissionSettlementKindDTO::kProvenNoEffect,
                 core::CellAdmissionSettlementKindDTO::kTerminalEffectsSettled,
                 core::CellAdmissionSettlementKindDTO::kExplicitReset,
                 core::CellAdmissionSettlementKindDTO::kReconciled})
    if (core::to_string(x) == v)
      return x;
  Fail("unknown cell-admission settlement_kind");
}

core::CellAdmissionScopeDTO Scope(const Value &v) {
  Exact(v, 2U);
  core::CellAdmissionScopeDTO result;
  for (const auto &x : Array(v, "resources")) {
    Require(x.is_string() &&
                x.as_string().size() <=
                    core::kMaximumCellAdmissionIdentityBytes,
            "cell-admission resource is invalid");
    result.resources.emplace_back(x.as_string());
  }
  for (const auto &x : Array(v, "configurations")) {
    Exact(x, 3U);
    result.configurations.push_back(
        {core::ControlDomainId(Text(x, "control_domain_id")),
         core::ActiveControlConfigurationId(
             Text(x, "active_configuration_id")),
         core::ActiveControlConfigurationGeneration(
             Counter(x, "active_configuration_generation", 1U))});
  }
  Require(result.resources.size() <= core::kMaximumCellAdmissionScopeEntries &&
              result.configurations.size() <=
                  core::kMaximumCellAdmissionScopeEntries,
          "cell-admission scope is oversized");
  return result;
}
core::CellAdmissionClaimIntentDTO Intent(const Value &v) {
  const bool program_node = v.is_object() && v.find_object_member("program_node_evidence") != nullptr;
  Exact(v, program_node ? 11U : 10U);
  core::CellAdmissionClaimIntentDTO result{
      ClaimKind(Text(v, "claim_kind")),
      OwnerRole(Text(v, "owner_role")),
      core::RequestOwnerEpoch(Text(v, "owner_epoch")),
      Text(v, "owner_transaction_id"),
      core::RequestId(Text(v, "request_id")),
      core::ContentDigest(Text(v, "request_payload_digest")),
      core::AdmissionGeneration(Counter(v, "expected_admission_generation")),
      Scope(Member(v, "scope")),
      std::nullopt,
      std::nullopt};
  result.program_cycle_evidence =
      Optional<core::ProgramCycleAdmissionEvidenceDTO>(
          v, "program_cycle_evidence", [](const Value &x) {
            Exact(x, 2U);
            return core::ProgramCycleAdmissionEvidenceDTO{
                core::ProgramRevision(Text(x, "draft_program_revision")),
                core::ProgramRevision(Text(x, "applied_program_revision"))};
          });
  result.intervention_evidence =
      Optional<core::CellAdmissionInterventionEvidenceDTO>(
          v, "intervention_evidence", [](const Value &x) {
            Exact(x, 5U);
            return core::CellAdmissionInterventionEvidenceDTO{
                OwnerRole(Text(x, "offering_owner_role")),
                core::RequestOwnerEpoch(Text(x, "offering_owner_epoch")),
                Text(x, "offer_transaction_id"),
                core::ContentDigest(Text(x, "offer_digest")),
                Optional<core::CellAdmissionClaimGeneration>(
                    x, "parent_program_cycle_claim_generation",
                    [](const Value &n) {
                      Require(n.is_number() && std::floor(n.as_number()) ==
                                                   n.as_number() &&
                                  n.as_number() >= 1.0 &&
                                  n.as_number() <= kMaximumSafeInteger,
                              "cell-admission claim generation is out of range");
                      return core::CellAdmissionClaimGeneration(
                          static_cast<std::uint64_t>(n.as_number()));
                    })};
          });
  if (program_node) {
    const auto& x = Member(v, "program_node_evidence");
    Exact(x, 6U);
    result.program_node_evidence = core::ProgramNodeAdmissionEvidenceDTO{
        core::RequestOwnerEpoch(Text(x, "program_runner_owner_epoch")),
        core::ProgramRunId(Text(x, "program_run_id")),
        core::CellAdmissionClaimGeneration(Counter(x, "parent_program_cycle_claim_generation", 1U)),
        Text(x, "node_instance_id"), Text(x, "node_attempt_id", 512U), Text(x, "owner_api")};
  }
  Require(core::ValidateCellAdmissionClaimIntent(result).ok(),
          "cell-admission claim intent is invalid");
  return result;
}
core::CellAdmissionClaimDTO Claim(const Value &v) {
  Exact(v, 4U);
  auto result = core::CellAdmissionClaimDTO{
      Intent(Member(v, "intent")),
      core::CellAdmissionClaimGeneration(Counter(v, "claim_generation", 1U)),
      core::AdmissionGeneration(
          Counter(v, "acquired_admission_generation", 1U)),
      ci::parse_nonnegative_int64(Member(v, "accepted_at_ns"))};
  return result;
}
core::CellAdmissionClaimReleaseDTO Release(const Value &v) {
  Exact(v, 8U);
  const auto &settlement = Member(v, "settlement");
  Exact(settlement, 2U);
  core::CellAdmissionClaimReleaseDTO result{
      OwnerRole(Text(v, "owner_role")),
      core::RequestOwnerEpoch(Text(v, "owner_epoch")),
      Text(v, "owner_transaction_id"),
      core::RequestId(Text(v, "request_id")),
      core::ContentDigest(Text(v, "request_payload_digest")),
      core::AdmissionGeneration(
          Counter(v, "acquired_admission_generation", 1U)),
      core::CellAdmissionClaimGeneration(
          Counter(v, "claim_generation", 1U)),
      {SettlementKind(Text(settlement, "settlement_kind")),
       core::ContentDigest(Text(settlement, "evidence_digest"))}};
  Require(core::ValidateCellAdmissionClaimRelease(result).ok(),
          "cell-admission claim release is invalid");
  return result;
}
core::CellAdmissionReadinessSnapshotDTO Readiness(const Value &v) {
  Exact(v, 10U);
  core::CellAdmissionReadinessSnapshotDTO result;
  result.program_cycle_status_current =
      Bool(v, "program_cycle_status_current");
  result.program_cycle_status = Optional<core::ProgramCycleStatusDTO>(
      v, "program_cycle_status", [](const Value &x) {
        Exact(x, 6U);
        return core::ProgramCycleStatusDTO{
            core::RequestOwnerEpoch(Text(x, "program_runner_owner_epoch")),
            Counter(x, "cycle_generation"),
            CycleState(Text(x, "state")),
            Optional<core::ProgramRunId>(
                x, "program_run_id", [](const Value &n) {
                  Require(n.is_string(), "program_run_id has the wrong type");
                  return core::ProgramRunId(n.as_string());
                }),
            Optional<core::CellAdmissionClaimGeneration>(
                x, "cycle_claim_generation", [](const Value &n) {
                  Require(n.is_number() && n.as_number() >= 1.0 &&
                              std::floor(n.as_number()) == n.as_number() &&
                              n.as_number() <= kMaximumSafeInteger,
                          "cycle_claim_generation is out of range");
                  return core::CellAdmissionClaimGeneration(
                      static_cast<std::uint64_t>(n.as_number()));
                }),
            Optional<core::ContentDigest>(
                x, "terminal_or_settlement_reference", [](const Value &n) {
                  Require(n.is_string(),
                          "terminal_or_settlement_reference has wrong type");
                  return core::ContentDigest(n.as_string());
                })};
      });
  result.required_component_status_current =
      Bool(v, "required_component_status_current");
  result.required_component_total = static_cast<std::uint32_t>(
      Counter(v, "required_component_total", 0U,
              std::numeric_limits<std::uint32_t>::max()));
  result.required_component_current_compatible_ready_converged =
      static_cast<std::uint32_t>(Counter(
          v, "required_component_current_compatible_ready_converged", 0U,
          std::numeric_limits<std::uint32_t>::max()));
  result.deployment_status_current = Bool(v, "deployment_status_current");
  result.deployment_converged = Bool(v, "deployment_converged");
  result.blocking_fault_count = static_cast<std::uint32_t>(
      Counter(v, "blocking_fault_count", 0U,
              std::numeric_limits<std::uint32_t>::max()));
  result.unresolved_owner_count = static_cast<std::uint32_t>(
      Counter(v, "unresolved_owner_count", 0U,
              std::numeric_limits<std::uint32_t>::max()));
  result.unsettled_effect_count = static_cast<std::uint32_t>(
      Counter(v, "unsettled_effect_count", 0U,
              std::numeric_limits<std::uint32_t>::max()));
  Require(result.required_component_current_compatible_ready_converged <=
              result.required_component_total &&
              (!result.program_cycle_status.has_value() ||
               core::ValidateProgramCycleStatus(*result.program_cycle_status)
                   .ok()),
          "cell-admission readiness snapshot is invalid");
  return result;
}
core::CellAdmissionReconcileDTO Reconcile(const Value &v) {
  Exact(v, 4U);
  core::CellAdmissionReconcileDTO result{
      core::RequestOwnerEpoch(Text(v, "kernel_owner_epoch")), {}, {},
      core::ContentDigest(Text(v, "reconciliation_evidence_digest"))};
  for (const auto &x : Array(v, "retained_claims"))
    result.retained_claims.push_back(Claim(x));
  for (const auto &x : Array(v, "current_owner_epochs")) {
    Exact(x, 2U);
    result.current_owner_epochs.push_back(
        {OwnerRole(Text(x, "owner_role")),
         core::RequestOwnerEpoch(Text(x, "owner_epoch"))});
  }
  return result;
}
core::CellAdmissionRecoveryReceiptQueryDTO RecoveryQuery(const Value &v) {
  Exact(v, 3U);
  auto result = core::CellAdmissionRecoveryReceiptQueryDTO{
      Intent(Member(v, "intent")),
      Optional<core::CellAdmissionClaimDTO>(
          v, "expected_claim", [](const Value &x) { return Claim(x); }),
      Optional<core::CellAdmissionClaimReleaseDTO>(
          v, "expected_release", [](const Value &x) { return Release(x); })};
  Require(core::ValidateCellAdmissionRecoveryReceiptQuery(result).ok(),
          "cell-admission recovery receipt query is invalid");
  return result;
}

core::OperatorAlarmAttentionSnapshotDTO
OperatorAlarmAttention(const Value &v) {
  Exact(v, 9U);
  Require(Text(v, "schema_version") == core::kOperatorAlarmAttentionSchemaVersion,
          "operator-alarm attention schema is unsupported");
  core::OperatorAlarmAttentionSnapshotDTO snapshot;
  snapshot.active_configuration =
      Optional<core::OperatorAlarmAttentionConfigurationDTO>(
          v, "active_configuration", [](const Value &x) {
            Exact(x, 2U);
            return core::OperatorAlarmAttentionConfigurationDTO{
                core::RetainedBundleId(
                    Text(x, "retained_bundle_id",
                         core::kMaximumOperatorAlarmAttentionTextBytes)),
                core::GraphRevision(
                    Text(x, "graph_revision",
                         core::kMaximumOperatorAlarmAttentionTextBytes))};
          });
  snapshot.cell_id = core::CellId(
      Text(v, "cell_id", core::kMaximumOperatorAlarmAttentionTextBytes));
  snapshot.owner_epoch =
      Text(v, "owner_epoch", core::kMaximumOperatorAlarmAttentionTextBytes);
  snapshot.owner_host_boot_id = Text(
      v, "owner_host_boot_id", core::kMaximumOperatorAlarmAttentionTextBytes);
  snapshot.owner_incarnation_us = Counter(v, "owner_incarnation_us", 1U);
  snapshot.generation = Counter(v, "generation");
  snapshot.reconciled = Bool(v, "reconciled");
  const auto &members = Member(v, "members");
  Require(members.is_array() &&
              members.as_array().size() <=
                  core::kMaximumOperatorAlarmAttentionMembers,
          "operator-alarm attention membership is malformed or oversized");
  for (const auto &row : members.as_array()) {
    Exact(row, 5U);
    core::OperatorAlarmAttentionMemberDTO member;
    member.condition_id =
        Text(row, "condition_id", core::kMaximumOperatorAlarmAttentionTextBytes);
    member.activation_id = Text(row, "activation_id",
                                core::kMaximumOperatorAlarmAttentionTextBytes);
    member.record_revision = Counter(row, "record_revision", 1U);
    const auto gate = Text(row, "gate");
    Require(gate == "condition_interlock" || gate == "acknowledgement",
            "operator-alarm attention gate is unsupported");
    member.gate = gate == "condition_interlock"
                      ? core::OperatorAlarmAttentionGateDTO::kConditionInterlock
                      : core::OperatorAlarmAttentionGateDTO::kAcknowledgement;
    const auto &subject = Member(row, "subject");
    Exact(subject, 3U);
    member.subject_kind = Text(subject, "subject_kind",
                               core::kMaximumOperatorAlarmAttentionTextBytes);
    member.subject_id = Text(subject, "subject_id",
                             core::kMaximumOperatorAlarmAttentionTextBytes);
    const auto &resources = Member(subject, "resource_ids");
    Require(resources.is_array() &&
                resources.as_array().size() <=
                    core::kMaximumOperatorAlarmSubjectResources,
            "operator-alarm subject resources are malformed or oversized");
    for (const auto &resource : resources.as_array()) {
      Require(resource.is_string(),
              "operator-alarm subject resource has the wrong type");
      member.resource_ids.emplace_back(resource.as_string());
    }
    snapshot.members.push_back(std::move(member));
  }
  return snapshot;
}

Value OptionalValue(const auto &v, auto encode) {
  return v.has_value() ? encode(*v) : Value(nullptr);
}
Value ScopeValue(const core::CellAdmissionScopeDTO &v) {
  Value::Array resources;
  for (const auto &x : v.resources)
    resources.emplace_back(x.value());
  Value::Array configurations;
  for (const auto &x : v.configurations)
    configurations.emplace_back(Value::Object{
        {"active_configuration_generation",
         Value(static_cast<double>(x.active_configuration_generation.value()))},
        {"active_configuration_id", Value(x.active_configuration_id.value())},
        {"control_domain_id", Value(x.control_domain_id.value())}});
  return Value(Value::Object{{"configurations", Value(std::move(configurations))},
                             {"resources", Value(std::move(resources))}});
}
Value IntentValue(const core::CellAdmissionClaimIntentDTO &v) {
  Value::Object fields{
      {"claim_kind", Value(std::string(core::to_string(v.claim_kind)))},
      {"expected_admission_generation",
       Value(static_cast<double>(v.expected_admission_generation.value()))},
      {"intervention_evidence",
       OptionalValue(v.intervention_evidence, [](const auto &x) {
         return Value(Value::Object{
             {"offer_digest", Value(x.offer_digest.value())},
             {"offer_transaction_id", Value(x.offer_transaction_id)},
             {"offering_owner_epoch", Value(x.offering_owner_epoch.value())},
             {"offering_owner_role",
              Value(std::string(core::to_string(x.offering_owner_role)))},
             {"parent_program_cycle_claim_generation",
              OptionalValue(x.parent_program_cycle_claim_generation,
                            [](const auto &n) {
                              return Value(static_cast<double>(n.value()));
                            })}});
       })},
      {"owner_epoch", Value(v.owner_epoch.value())},
      {"owner_role", Value(std::string(core::to_string(v.owner_role)))},
      {"owner_transaction_id", Value(v.owner_transaction_id)},
      {"program_cycle_evidence",
       OptionalValue(v.program_cycle_evidence, [](const auto &x) {
         return Value(Value::Object{
             {"applied_program_revision",
              Value(x.applied_program_revision.value())},
             {"draft_program_revision", Value(x.draft_program_revision.value())}});
       })},
      {"request_id", Value(v.request_id.value())},
      {"request_payload_digest", Value(v.request_payload_digest.value())},
      {"scope", ScopeValue(v.scope)}};
  if (v.program_node_evidence.has_value()) {
    const auto& x = *v.program_node_evidence;
    fields.emplace_back("program_node_evidence", Value(Value::Object{
        {"program_runner_owner_epoch", Value(x.program_runner_owner_epoch.value())},
        {"program_run_id", Value(x.program_run_id.value())},
        {"parent_program_cycle_claim_generation",
         Value(static_cast<double>(x.parent_program_cycle_claim_generation.value()))},
        {"node_instance_id", Value(x.node_instance_id)},
        {"node_attempt_id", Value(x.node_attempt_id)},
        {"owner_api", Value(x.owner_api)},
    }));
  }
  return Value(std::move(fields));
}
Value ClaimValue(const core::CellAdmissionClaimDTO &v) {
  return Value(Value::Object{
      {"accepted_at_ns", ci::nonnegative_int64_value(v.accepted_at_ns)},
      {"acquired_admission_generation",
       Value(static_cast<double>(v.acquired_admission_generation.value()))},
      {"claim_generation",
       Value(static_cast<double>(v.claim_generation.value()))},
      {"intent", IntentValue(v.intent)}});
}
Value ReleaseValue(const core::CellAdmissionClaimReleaseDTO &v) {
  return Value(Value::Object{
      {"acquired_admission_generation",
       Value(static_cast<double>(v.acquired_admission_generation.value()))},
      {"claim_generation",
       Value(static_cast<double>(v.claim_generation.value()))},
      {"owner_epoch", Value(v.owner_epoch.value())},
      {"owner_role", Value(std::string(core::to_string(v.owner_role)))},
      {"owner_transaction_id", Value(v.owner_transaction_id)},
      {"request_id", Value(v.request_id.value())},
      {"request_payload_digest", Value(v.request_payload_digest.value())},
      {"settlement",
       Value(Value::Object{
           {"evidence_digest", Value(v.settlement.evidence_digest.value())},
           {"settlement_kind",
            Value(std::string(core::to_string(v.settlement.settlement_kind)))}})}});
}
Value OwnerEpochValue(const core::CellAdmissionCurrentOwnerEpochDTO &v) {
  return Value(Value::Object{
      {"owner_epoch", Value(v.owner_epoch.value())},
      {"owner_role", Value(std::string(core::to_string(v.owner_role)))}});
}
Value ReconcileSettlementValue(
    const core::CellAdmissionReconcileSettlementDTO &v) {
  return Value(Value::Object{
      {"predecessor_claim", ClaimValue(v.predecessor_claim)},
      {"reconciliation_evidence_digest",
       Value(v.reconciliation_evidence_digest.value())},
      {"settlement_predicate",
       Value(std::string(core::to_string(v.settlement_predicate)))},
      {"successor_owner_epoch", Value(v.successor_owner_epoch.value())},
      {"successor_owner_role",
       Value(std::string(core::to_string(v.successor_owner_role)))}});
}
Value RecoveryReceiptValue(
    const core::CellAdmissionRecoveryReceiptDTO &v) {
  Require(core::ValidateCellAdmissionRecoveryReceipt(v).ok(),
          "cell-admission recovery receipt is invalid");
  Value::Array held;
  for (const auto &claim : v.held_claims)
    held.push_back(ClaimValue(claim));
  Value::Array owners;
  for (const auto &owner : v.current_owner_epochs)
    owners.push_back(OwnerEpochValue(owner));
  return Value(Value::Object{
      {"admission_generation",
       Value(static_cast<double>(v.admission_generation.value()))},
      {"current_owner_epochs", Value(std::move(owners))},
      {"held_claims", Value(std::move(held))},
      {"kernel_owner_epoch", Value(v.kernel_owner_epoch.value())},
      {"matched_claim",
       OptionalValue(v.matched_claim,
                     [](const auto &x) { return ClaimValue(x); })},
      {"matched_reconcile_settlement",
       OptionalValue(v.matched_reconcile_settlement, [](const auto &x) {
         return ReconcileSettlementValue(x);
       })},
      {"matched_release",
       OptionalValue(v.matched_release,
                     [](const auto &x) { return ReleaseValue(x); })},
      {"status", Value(std::string(core::to_string(v.status)))}});
}
Value AdmissionValue(const core::AuthoringAdmissionDTO &v) {
  return Value(Value::Object{
      {"admission_generation",
       Value(static_cast<double>(v.admission_generation.value()))},
      {"authoring_allowed", Value(v.authoring_allowed)}});
}
Value ErrorValue(const core::ErrorDTO &v) {
  Value::Object details;
  for (const auto &[key, value] : v.details)
    details.emplace_back(key, Value(value));
  return Value(Value::Object{{"code", Value(v.code)},
                             {"details", Value(std::move(details))},
                             {"message", Value(v.message)}});
}
bool IsCommand(std::string_view v) {
  return v == "cell_admission_get" ||
         v == "cell_admission_update_readiness" ||
         v == "cell_admission_acquire" || v == "cell_admission_release" ||
         v == "cell_admission_reconcile" ||
         v == "cell_admission_recovery_receipt" ||
         v == cm::kPublishOperatorAlarmAttentionCommand;
}
} // namespace

core::CellAdmissionClaimDTO DecodeCellAdmissionClaim(
    const std::string_view bytes) {
  Require(bytes.size() <= kMaximumPacketBytes,
          "cell-admission claim JSON is oversized");
  return Claim(ci::parse_canonical_json(bytes));
}

core::CellAdmissionClaimReleaseDTO DecodeCellAdmissionClaimRelease(
    const std::string_view bytes) {
  Require(bytes.size() <= kMaximumPacketBytes,
          "cell-admission release JSON is oversized");
  return Release(ci::parse_canonical_json(bytes));
}

std::optional<cm::KernelCellAdmissionRpcPacket>
TryDecodeCellAdmissionCommand(std::string_view bytes) {
  if (bytes.find("\"command\":\"cell_admission_") == std::string_view::npos)
    return std::nullopt;
  Require(!bytes.empty() && bytes.size() <= kMaximumPacketBytes,
          "execution command packet is empty or oversized");
  const auto root = ci::parse_canonical_json(
      bytes, {.maximum_bytes = kMaximumPacketBytes,
              .maximum_depth = ci::kMaximumCanonicalJsonDepth,
              .maximum_nodes = ci::kMaximumCanonicalJsonNodes});
  if (!root.is_object())
    return std::nullopt;
  const auto *command_value = root.find_object_member("command");
  if (command_value == nullptr || !command_value->is_string() ||
      !IsCommand(command_value->as_string()))
    return std::nullopt;
  Require(Text(root, "api_version") == kCellAdmissionCommandApiVersion,
          "execution command API version is unsupported");
  const auto command = command_value->as_string();
  if (command == "cell_admission_get") {
    Exact(root, 2U);
    return cm::KernelCellAdmissionRpcPacket{command, std::monostate{}};
  }
  Exact(root, 3U);
  const auto &request = Member(root, "request");
  if (command == "cell_admission_update_readiness")
    return cm::KernelCellAdmissionRpcPacket{command, Readiness(request)};
  if (command == "cell_admission_acquire")
    return cm::KernelCellAdmissionRpcPacket{command, Intent(request)};
  if (command == "cell_admission_release")
    return cm::KernelCellAdmissionRpcPacket{command, Release(request)};
  if (command == "cell_admission_recovery_receipt")
    return cm::KernelCellAdmissionRpcPacket{command, RecoveryQuery(request)};
  if (command == cm::kPublishOperatorAlarmAttentionCommand)
    return cm::KernelCellAdmissionRpcPacket{command,
                                            OperatorAlarmAttention(request)};
  return cm::KernelCellAdmissionRpcPacket{command, Reconcile(request)};
}

Value CellAdmissionResponseValue(
    std::string_view command,
    const cm::KernelCellAdmissionRpcResponse &response) {
  Require(IsCommand(command), "unknown cell-admission response command");
  if (const auto *error = std::get_if<core::ErrorDTO>(&response))
    return Value(Value::Object{
        {"api_version", Value(std::string(kCellAdmissionResponseApiVersion))},
        {"command", Value(std::string(command))},
        {"error", ErrorValue(*error)},
        {"ok", Value(false)}});
  Value::Object result{
      {"api_version", Value(std::string(kCellAdmissionResponseApiVersion))},
      {"command", Value(std::string(command))},
      {"ok", Value(true)}};
  if (const auto *claim = std::get_if<core::CellAdmissionClaimDTO>(&response))
    result.emplace_back("claim", ClaimValue(*claim));
  else if (const auto *receipt =
               std::get_if<core::CellAdmissionRecoveryReceiptDTO>(&response))
    result.emplace_back("recovery_receipt",
                        RecoveryReceiptValue(*receipt));
  else if (const auto *attention =
               std::get_if<core::OperatorAlarmAttentionAcceptedDTO>(&response))
    result.emplace_back(
        "operator_alarm_attention",
        Value(Value::Object{
            {"generation",
             Value(static_cast<double>(attention->generation))},
            {"owner_epoch", Value(attention->owner_epoch)}}));
  else
    result.emplace_back(
        "authoring_admission",
        AdmissionValue(std::get<core::AuthoringAdmissionDTO>(response)));
  return Value(std::move(result));
}

} // namespace manyforge::roboplan_backend::detail

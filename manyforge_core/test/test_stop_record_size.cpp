// Lean Stop record (operation journal v18): measured stored bytes per Stop.
//
// Every Stop here is written through the public SqliteOperationJournal API and
// measured with plain sqlite3 reads: the sum of length(payload_json) of the
// Stop's journal_records rows plus its stop_projections.accepted_payload and
// accepted_digest. The
// fixtures are realistic: an arm plan with K = 3 stages, six joints and an
// operator profile; a position gripper with K = 2 stages; one evidence item per
// stage. The acceptance carries no affected operation snapshots: those stay in
// the acceptance journal row, unchanged by the lean record, so they are left out
// of these bounds (plan §1.4). The measured bytes are printed and recorded as
// test properties as evidence.

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_core/resolved_stop_base_plan_codec.hpp>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/stop_scope_resolver.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/stop_terminal_state.hpp>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace manyforge::core {
namespace {

inline constexpr char kDigestA[] =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
inline constexpr char kDigestB[] =
    "sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";

inline constexpr std::size_t kKiB = 1024U;
inline constexpr std::size_t kArmJointCount = 6U;
inline constexpr std::size_t kArmStageCount = 3U;
inline constexpr std::size_t kGripperStageCount = 2U;

class ScopedTemporaryDirectory final {
 public:
  ScopedTemporaryDirectory() {
    static std::atomic<std::uint64_t> next{1U};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-stop-record-size-test-" +
             std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
             std::to_string(next.fetch_add(1U)));
    if (!std::filesystem::create_directory(path_)) {
      throw std::runtime_error("failed to create Stop record size test root");
    }
  }

  ~ScopedTemporaryDirectory() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }

  ScopedTemporaryDirectory(const ScopedTemporaryDirectory&) = delete;
  ScopedTemporaryDirectory& operator=(const ScopedTemporaryDirectory&) = delete;

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

enum class ParticipantKind { kArm, kGripper };

/// One prepared Stop participant: its base plan, resource bindings and the
/// exact prepared plan every attempt of this participant embeds in memory.
struct ParticipantFixture final {
  std::string name;
  ParticipantKind kind{ParticipantKind::kArm};
  ResolvedStopBasePlanDTO base;
  std::vector<RuntimeResourceBindingDTO> resources;
  std::vector<ResourceId> resource_ids;
  PreparedParticipantStopPlanDTO plan;
};

[[nodiscard]] ResolvedStopBasePlanDTO GoldenBasePlan() {
  const auto path =
      std::filesystem::path(__FILE__).parent_path() / "golden" / "resolved_stop_base_plan.v1.json";
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream bytes;
  bytes << stream.rdbuf();
  auto value = bytes.str();
  if (!value.empty() && value.back() == '\n') value.pop_back();
  auto decoded = DecodeResolvedStopBasePlan(value);
  if (!decoded.ok()) throw std::logic_error("golden base plan: " + decoded.error().message);
  return std::move(decoded.value());
}

[[nodiscard]] std::vector<RuntimeResourceBindingDTO> ParticipantResources(
    const std::string& name, const ParticipantKind kind) {
  std::vector<RuntimeResourceBindingDTO> resources;
  if (kind == ParticipantKind::kArm) {
    for (std::size_t joint = 1U; joint <= kArmJointCount; ++joint) {
      const auto suffix = "joint_" + std::to_string(joint);
      resources.push_back(RuntimeResourceBindingDTO{
          ResourceRoleId("arm_joints"), ResourceId(name + "." + suffix), suffix,
          "mf_" + name + "_" + suffix, CoordinateKind::kAngular});
    }
  } else {
    resources.push_back(RuntimeResourceBindingDTO{ResourceRoleId("fingers"),
                                                  ResourceId(name + ".finger"), "finger_joint",
                                                  "mf_" + name + "_finger_joint",
                                                  CoordinateKind::kLinear});
  }
  std::sort(resources.begin(), resources.end(), [](const auto& left, const auto& right) {
    return left.resource_id < right.resource_id;
  });
  return resources;
}

/// The golden base plan bound to this participant's resources, with the
/// qualified controlled-Stop contract and complete per-resource defaults on
/// the nominal stage. An arm gets a controlled-fallback stage, so K = 3; the
/// gripper keeps nominal + software hard, so K = 2.
[[nodiscard]] ResolvedStopBasePlanDTO ParticipantBasePlan(
    const ParticipantKind kind, const std::vector<RuntimeResourceBindingDTO>& resources) {
  auto base = GoldenBasePlan();
  auto& nominal = base.escalation_stages.front();
  const ProviderContractRefDTO contract{
      ProviderContractRoleId("stop_contract"),
      SchemaId(std::string(kMotionControlledStopContractSchemaId)),
      SchemaRevision(std::string(kMotionControlledStopContractSchemaRevision))};
  nominal.containment_command.provider_contract_ref = contract;
  if (nominal.command_qualification.controlled_substitution_contract) {
    nominal.command_qualification.controlled_substitution_contract->provider_contract_ref =
        contract;
  }
  JsonValueDTO::Array limits;
  std::vector<ResourceId> resource_ids;
  for (const auto& resource : resources) {
    const bool angular = resource.coordinate_kind == CoordinateKind::kAngular;
    resource_ids.push_back(resource.resource_id);
    limits.emplace_back(JsonObjectDTO{
        {"resource_id", JsonValueDTO{resource.resource_id.value()}},
        {"max_deceleration",
         JsonValueDTO{JsonObjectDTO{
             {"coordinate_kind", JsonValueDTO{angular ? "angular_rad_s2" : "linear_m_s2"}},
             {angular ? "value_rad_s2" : "value_m_s2", JsonValueDTO{4.0}}}}},
        {"settle_velocity_threshold",
         JsonValueDTO{JsonObjectDTO{
             {"coordinate_kind", JsonValueDTO{angular ? "angular_rad_s" : "linear_m_s"}},
             {angular ? "value_rad_s" : "value_m_s", JsonValueDTO{0.02}}}}},
    });
  }
  nominal.default_contract_parameters = JsonObjectDTO{
      {"stop_mode", JsonValueDTO{"soft_on_path"}},
      {"deceleration_time_s", JsonValueDTO{0.5}},
      {"jerk_limits_enabled", JsonValueDTO{false}},
      {"allow_natural_completion_on_short_path", JsonValueDTO{true}},
      {"per_resource_limits", JsonValueDTO{std::move(limits)}},
  };
  nominal.containment_scope =
      ResolvedStopResourceContainmentScopeDTO{base.activation_set_id, std::move(resource_ids)};
  if (kind == ParticipantKind::kArm) {
    auto intermediate = base.escalation_stages.front();
    intermediate.stage_index = 1U;
    intermediate.stage_kind = StopEscalationStageKind::kControlledFallback;
    intermediate.stop_escalation_stage_id = StopEscalationStageId("stage.intermediate");
    intermediate.command_qualification.controlled_substitution_contract.reset();
    intermediate.evidence_requirements.front().requirement.evidence_item_id =
        StopEvidenceItemId("evidence.intermediate.rest");
    base.escalation_stages.back().stage_index = 2U;
    base.escalation_stages.insert(base.escalation_stages.begin() + 1, std::move(intermediate));
  } else {
    base.gripper_actuation_kind = GripperActuationKind::kPositionCommanded;
  }
  auto made = MakeResolvedStopBasePlan(std::move(base));
  if (!made.ok()) throw std::logic_error("participant base plan: " + made.error().message);
  return std::move(made.value());
}

/// The arm's operator Stop profile: soft on path with a per-joint
/// deceleration map, as the Kernel prepares it for a trajectory participant.
[[nodiscard]] MotionInterruptStopProfileDTO ArmOperatorProfile(
    const std::vector<RuntimeResourceBindingDTO>& resources) {
  MotionInterruptStopProfileDTO profile;
  profile.profile_id = "operator_soft_on_path";
  profile.stop_mode = MotionInterruptStopMode::kSoftOnPath;
  profile.deceleration_time_s = 1.0;
  profile.allow_natural_completion_on_short_path = true;
  profile.require_backend_confirmation = true;
  profile.jerk_limits_enabled = false;
  for (const auto& resource : resources) {
    profile.per_joint_max_deceleration.emplace(*resource.source_joint_name,
                                               AngularAccelerationDTO{4.0});
  }
  return profile;
}

[[nodiscard]] ParticipantFixture Participant(const std::string& name, const ParticipantKind kind) {
  auto resources = ParticipantResources(name, kind);
  std::vector<ResourceId> resource_ids;
  std::vector<HardStopResourceLimitDTO> limits;
  for (const auto& resource : resources) {
    resource_ids.push_back(resource.resource_id);
    limits.push_back(HardStopResourceLimitDTO{
        resource.resource_id, resource.coordinate_kind == CoordinateKind::kAngular
                                  ? CoordinateVelocityDTO{AngularVelocityDTO{0.02}}
                                  : CoordinateVelocityDTO{LinearVelocityDTO{0.02}}});
  }
  auto base = ParticipantBasePlan(kind, resources);
  std::optional<MotionInterruptStopProfileDTO> profile;
  if (kind == ParticipantKind::kArm) profile = ArmOperatorProfile(resources);
  const auto purpose = profile.has_value() ? ParticipantStopProfilePurposeDTO::kOperator
                                           : ParticipantStopProfilePurposeDTO::kOrdinary;
  auto plan = MakePreparedParticipantStopPlan(
      base,
      PreparedParticipantStopPlanContextDTO{
          OperationId("operation." + name), OperationRevision(4U),
          OperationParticipantId("participant." + name), ProgramId("program.1"),
          ProgramRevision(kDigestA), ProgramBindingId(kDigestB), ProgramRunId("program-run.1"),
          std::move(profile), purpose, std::nullopt, MotionInterruptNodeCategory::kGeneric},
      resources, limits);
  if (!plan.ok()) throw std::logic_error("participant Stop plan: " + plan.error().message);
  const auto expected_stages =
      kind == ParticipantKind::kArm ? kArmStageCount : kGripperStageCount;
  if (plan.value().stage_commands.size() != expected_stages) {
    throw std::logic_error("participant Stop plan has an unexpected stage count");
  }
  return ParticipantFixture{name,         kind, std::move(base), std::move(resources),
                            std::move(resource_ids), std::move(plan.value())};
}

[[nodiscard]] std::vector<ParticipantFixture> Participants(const std::size_t arms,
                                                           const std::size_t grippers) {
  std::vector<ParticipantFixture> participants;
  for (std::size_t index = 1U; index <= arms; ++index) {
    participants.push_back(Participant("arm." + std::to_string(index), ParticipantKind::kArm));
  }
  for (std::size_t index = 1U; index <= grippers; ++index) {
    participants.push_back(
        Participant("gripper." + std::to_string(index), ParticipantKind::kGripper));
  }
  // Scope entries and attempts are canonical by operation ID.
  std::sort(participants.begin(), participants.end(), [](const auto& left, const auto& right) {
    return left.plan.operation_id < right.plan.operation_id;
  });
  return participants;
}

[[nodiscard]] StopScopeEntryKeyDTO ScopeKey(const ParticipantFixture& participant,
                                            const StopId& stop_id,
                                            const StopGeneration stop_generation) {
  return StopScopeEntryKeyDTO{
      stop_id, stop_generation,
      StopParticipantScopeKeyDTO{participant.plan.operation_id, participant.plan.participant_id}};
}

[[nodiscard]] StopScopeEntryDTO ScopeEntry(const ParticipantFixture& participant,
                                           const StopId& stop_id,
                                           const StopGeneration stop_generation,
                                           const std::int64_t accepted_at_ns) {
  const auto& base = participant.base;
  return StopScopeEntryDTO{
      ScopeKey(participant, stop_id, stop_generation),
      StopParticipantScopeFactsDTO{
          OperationRevision(participant.plan.operation_revision.value() + 1U),
          ControllableComponentId("component." + participant.name),
          CommandTargetId(participant.kind == ParticipantKind::kArm ? "arm" : "gripper"),
          CapabilityId(participant.kind == ParticipantKind::kArm ? "execution.trajectory.execute"
                                                                 : "tool.gripper.command"),
          base.control_domain_id, base.control_domain_revision, base.control_binding_id,
          base.control_binding_revision, ControllerGoalId("goal." + participant.name),
          ControllerGoalGeneration(3U), base.active_control_configuration_id,
          base.active_control_configuration_generation, base.controller_topology_id,
          base.topology_revision, base.control_route_id,
          base.escalation_stages.front().command_target.provider_runtime_epoch,
          participant.resource_ids, base.containment_granularity,
          StopParticipantScopeFactsDTO::TerminalSettlementState::kSettled},
      StopScopeEntryDispositionDTO::kStoppable,
      StopScopeDispatchStateDTO::kDispatched,
      base.stop_evidence_policy_id,
      base.stop_evidence_policy_revision,
      participant.plan.participant_stop_invocation_plan_digest,
      accepted_at_ns};
}

/// One authoritative stage result. A successful stage proves rest with each
/// required evidence item; an unsuccessful one reports the missing evidence
/// and escalates to the next stage.
[[nodiscard]] StopAttemptedStageResultDTO Attempt(const ParticipantFixture& participant,
                                                  const StopId& stop_id,
                                                  const StopGeneration stop_generation,
                                                  const std::size_t stage_index,
                                                  const bool succeeded,
                                                  const std::uint64_t result_sequence) {
  const auto& base = participant.base;
  const auto& selected = participant.plan.stage_commands.at(stage_index).resolved_stage;
  const auto provider_runtime_epoch = selected.command_target.provider_runtime_epoch;
  ResolvedParticipantStopStageDTO request{stop_id,
                                          stop_generation,
                                          participant.plan.operation_id,
                                          participant.plan.operation_revision,
                                          participant.plan.participant_id,
                                          ControllerGoalId("goal." + participant.name),
                                          ControllerGoalGeneration(3U),
                                          ControllableComponentId("component." + participant.name),
                                          CommandTargetId(participant.kind == ParticipantKind::kArm
                                                              ? "arm"
                                                              : "gripper"),
                                          CapabilityId(participant.kind == ParticipantKind::kArm
                                                           ? "execution.trajectory.execute"
                                                           : "tool.gripper.command"),
                                          base.control_domain_id,
                                          base.control_domain_revision,
                                          base.control_binding_id,
                                          base.control_binding_revision,
                                          base.active_control_configuration_id,
                                          base.active_control_configuration_generation,
                                          base.controller_topology_id,
                                          base.topology_revision,
                                          base.control_route_id,
                                          std::nullopt,
                                          std::nullopt,
                                          provider_runtime_epoch,
                                          participant.resource_ids,
                                          selected.containment_granularity,
                                          base.stop_evidence_policy_id,
                                          base.stop_evidence_policy_revision,
                                          participant.plan,
                                          stage_index,
                                          ContentDigest(kDigestA)};
  const bool controlled =
      selected.containment_command.command_kind == StopContainmentCommandKind::kMotionControlledStop;
  ParticipantStopStageResultDTO result{
      std::move(request),
      StopStageAttemptDispositionDTO::kDispatched,
      succeeded ? ParticipantStopStageOutcomeDTO::kSucceeded
                : ParticipantStopStageOutcomeDTO::kFailed,
      controlled ? std::optional<MotionControlledStopMode>(MotionControlledStopMode::kSoftOnPath)
                 : std::nullopt,
      std::nullopt,
      3U,
      provider_runtime_epoch,
      9U,
      10'000'000,
      {},
      succeeded ? std::nullopt
                : std::optional<ErrorDTO>(ErrorDTO{"stop_stage_rest_unproven",
                                                   "stage did not prove rest before its deadline",
                                                   {}})};
  for (const auto& requirement : selected.evidence_requirements) {
    if (succeeded) {
      result.evidence_items.push_back(StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
          StopEvidenceItemStateDTO::kSatisfied, requirement.resolved_source,
          requirement.resolved_source.runtime_target.provider_runtime_epoch, 9U, 1'000'000,
          JsonValueDTO{true}, std::nullopt});
    } else {
      result.evidence_items.push_back(StopEvidenceItemDTO{
          requirement.requirement.evidence_item_id, requirement.requirement.evidence_kind,
          StopEvidenceItemStateDTO::kMissing, requirement.resolved_source, std::nullopt,
          std::nullopt, std::nullopt, std::nullopt,
          ErrorDTO{"stop_evidence_missing", "rest evidence was not observed in time", {}}});
    }
  }
  return StopAttemptedStageResultDTO{ScopeKey(participant, stop_id, stop_generation),
                                     result_sequence,
                                     20 + static_cast<std::int64_t>(result_sequence),
                                     StopStageResultAuthorityDTO::kAuthoritative,
                                     std::move(result)};
}

struct StopFixture final {
  EmergencyStopAcceptanceRecordDTO acceptance;
  FullStopIntentDTO intent;
  EmergencyStopAffectedStateSnapshotDTO affected_state;
  StopAcceptedDTO accepted;
  StopJournalTerminalProjectionDTO terminal;
};

/// A Full Stop over `participants`. Participant i runs `attempts[i]` stages:
/// the last one succeeds, every earlier one fails and escalates.
[[nodiscard]] StopFixture BuildStop(const RequestOwnerEpoch& epoch,
                                    const std::vector<ParticipantFixture>& participants,
                                    const std::vector<std::size_t>& attempts) {
  if (attempts.size() != participants.size()) throw std::logic_error("attempt count mismatch");
  const StopId stop_id("stop.size");
  const StopGeneration stop_generation(1U);
  constexpr std::int64_t accepted_at_ns = 10;

  const FullStopIntentDTO intent{
      PublicRequestContextDTO{RequestId("request.stop.size"), epoch, "stop-record-size-test",
                              std::nullopt},
      StopGeneration(0U), RequestAttributionDTO{}};
  std::vector<ResourceId> resources;
  for (const auto& participant : participants) {
    resources.insert(resources.end(), participant.resource_ids.begin(),
                     participant.resource_ids.end());
  }
  std::sort(resources.begin(), resources.end());
  const EmergencyStopAcceptanceRecordDTO acceptance{
      epoch,
      intent.request_context.request_id,
      ComputeNormalizedStopIntentDigest(intent).value(),
      std::nullopt,
      RequestAttributionDTO{},
      RequestReplayStatusDTO::kAcceptedNew,
      stop_id,
      stop_generation,
      accepted_at_ns,
      StopResolutionDispositionDTO::kExact,
      ContentDigest(kDigestB),
      EmergencyStopEntryStateDTO::kActive,
      EmergencyStopAcceptancePathDTO::kFreshReserved,
      EmergencyStopProjectionStateDTO::kPending};
  const StopAcceptedDTO accepted{acceptance.request_id,
                                 acceptance.replay_status,
                                 stop_id,
                                 stop_generation,
                                 ComputeRequestedStopSelectorDigest(intent).value(),
                                 acceptance.resolution_disposition,
                                 StopScopeResolutionReasonDTO::kExplicitFullStop,
                                 {},
                                 acceptance.captured_scope_digest,
                                 {},
                                 {},
                                 accepted_at_ns};

  std::vector<StopScopeEntryDTO> entries;
  std::vector<StopAttemptedStageResultDTO> attempted;
  std::uint64_t result_sequence = 0U;
  for (std::size_t index = 0U; index < participants.size(); ++index) {
    const auto& participant = participants[index];
    if (attempts[index] == 0U || attempts[index] > participant.plan.stage_commands.size()) {
      throw std::logic_error("attempt count is outside the participant plan");
    }
    entries.push_back(ScopeEntry(participant, stop_id, stop_generation, accepted_at_ns));
    for (std::size_t stage = 0U; stage < attempts[index]; ++stage) {
      attempted.push_back(Attempt(participant, stop_id, stop_generation, stage,
                                  stage + 1U == attempts[index], ++result_sequence));
    }
  }

  const StopCertificateId certificate_id("certificate.stop.size");
  StopTerminalReductionInputDTO input{
      certificate_id,
      StopCertificateRevision(1U),
      stop_id,
      stop_generation,
      StopRequestedSelectorSnapshotDTO{StopRequestedSelectorKindDTO::kExplicitFull,
                                       accepted.requested_selector_digest, std::nullopt,
                                       std::nullopt},
      accepted.resolution_disposition,
      accepted.resolution_reason,
      {},
      {StopScopeCaptureHistoryDTO{1U,
                                  accepted.resolution_disposition,
                                  accepted.resolution_reason,
                                  accepted.captured_scope_digest,
                                  {},
                                  {},
                                  resources}},
      {},
      std::move(entries),
      std::move(attempted),
      std::nullopt,
      100,
      std::nullopt};
  auto certificate = ReduceStopTerminal(input);
  if (!certificate.ok()) throw std::logic_error("Stop certificate: " + certificate.error().message);

  // The status exactly as the Kernel hands it to the journal: the certificate
  // join plus the stored-only rest, with the terminal projection pending.
  const StopTerminalStatusRestDTO rest{
      accepted,
      {},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kRecorded, std::nullopt,
                                     std::nullopt},
      StopDurableProjectionStatusDTO{StopDurableProjectionStateDTO::kPending, std::nullopt,
                                     std::nullopt}};
  auto status = TerminalStopStatusFromCertificate(rest, certificate.value());
  if (const auto error = stop_terminal_projection_field_error(status, certificate.value());
      error.has_value()) {
    throw std::logic_error("Stop terminal status: " + std::string(*error));
  }
  return StopFixture{
      acceptance, intent, EmergencyStopAffectedStateSnapshotDTO{{}, std::move(resources)},
      accepted,
      StopJournalTerminalProjectionDTO{std::move(status), std::move(certificate.value())}};
}

struct StoredRow final {
  std::string record_kind;
  std::string payload_json;
};

struct StoredStop final {
  std::vector<StoredRow> rows;
  std::string accepted_payload;
  std::string accepted_digest;
  std::size_t total_bytes{};
  std::string terminal_payload;
  /// Every stored payload of the Stop, newline-separated, for occurrence counts.
  std::string all_bytes;
};

/// Records acceptance and terminal through the journal, then reads the Stop's
/// stored bytes with plain sqlite3.
void RecordAndMeasure(const StopFixture& stop, const std::vector<ResourceId>& inventory,
                      StoredStop* stored) {
  ScopedTemporaryDirectory temporary;
  const auto database = temporary.path() / std::string(kOperationJournalFilename);
  const RequestOwnerEpoch& epoch = stop.acceptance.owner_epoch;
  {
    SqliteOperationJournal journal(database, CellId("cell.1"), 8U);
    const auto begun = journal.BeginOwnerEpoch(epoch, 1, 8U, inventory);
    ASSERT_TRUE(begun.ok()) << begun.error().message;
    const auto accepted =
        journal.RecordStopAccepted(epoch, stop.acceptance, EmergencyStopProjectedIntentDTO(stop.intent),
                                   stop.accepted, stop.affected_state);
    ASSERT_EQ(accepted.projection_state, EmergencyStopProjectionStateDTO::kRecorded)
        << (accepted.error.has_value() ? accepted.error->code + ": " + accepted.error->message
                                       : std::string{});
    const auto& terminal = stop.terminal;
    const auto recorded =
        journal.RecordStopTerminal(epoch, terminal.stop_status, terminal.certificate);
    ASSERT_EQ(recorded.state, StopForensicProjectionStateDTO::kRecorded)
        << (recorded.error.has_value() ? recorded.error->code + ": " + recorded.error->message
                                       : std::string{});

    journal.Close();
  }

  sqlite3* raw = nullptr;
  ASSERT_EQ(sqlite3_open_v2(database.c_str(), &raw, SQLITE_OPEN_READWRITE, nullptr), SQLITE_OK);
  const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(raw, &sqlite3_close);
  const auto text = [](sqlite3_stmt* statement, const int column) {
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
    return std::string(bytes == nullptr ? "" : bytes,
                       static_cast<std::size_t>(sqlite3_column_bytes(statement, column)));
  };
  const auto& stop_id = stop.accepted.stop_id.value();
  const auto stop_generation = static_cast<sqlite3_int64>(stop.accepted.stop_generation.value());
  {
    sqlite3_stmt* raw_statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(),
                                 "SELECT record_kind,payload_json FROM journal_records "
                                 "WHERE stop_id=?1 AND stop_generation=?2 ORDER BY sequence",
                                 -1, &raw_statement, nullptr),
              SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> rows(raw_statement,
                                                                          &sqlite3_finalize);
    ASSERT_EQ(sqlite3_bind_text(rows.get(), 1, stop_id.c_str(), -1, SQLITE_TRANSIENT), SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(rows.get(), 2, stop_generation), SQLITE_OK);
    int step = SQLITE_ROW;
    while ((step = sqlite3_step(rows.get())) == SQLITE_ROW) {
      stored->rows.push_back(StoredRow{text(rows.get(), 0), text(rows.get(), 1)});
    }
    ASSERT_EQ(step, SQLITE_DONE);
  }
  {
    sqlite3_stmt* raw_statement = nullptr;
    ASSERT_EQ(sqlite3_prepare_v2(db.get(),
                                 "SELECT accepted_payload,accepted_digest FROM stop_projections "
                                 "WHERE stop_id=?1 AND stop_generation=?2",
                                 -1, &raw_statement, nullptr),
              SQLITE_OK);
    const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> projection(
        raw_statement, &sqlite3_finalize);
    ASSERT_EQ(sqlite3_bind_text(projection.get(), 1, stop_id.c_str(), -1, SQLITE_TRANSIENT),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_bind_int64(projection.get(), 2, stop_generation), SQLITE_OK);
    ASSERT_EQ(sqlite3_step(projection.get()), SQLITE_ROW);
    stored->accepted_payload = text(projection.get(), 0);
    stored->accepted_digest = text(projection.get(), 1);
    ASSERT_EQ(sqlite3_step(projection.get()), SQLITE_DONE);
  }

  stored->total_bytes = stored->accepted_payload.size() + stored->accepted_digest.size();
  stored->all_bytes = stored->accepted_payload;
  std::size_t terminal_rows = 0U;
  for (const auto& row : stored->rows) {
    stored->total_bytes += row.payload_json.size();
    stored->all_bytes += "\n" + row.payload_json;
    if (row.record_kind == "stop_terminal_projected") {
      ++terminal_rows;
      stored->terminal_payload = row.payload_json;
    }
  }
  ASSERT_EQ(terminal_rows, 1U);

  // The measured bytes are the lean record itself: they decode to the exact
  // certificate the Kernel handed over.
  const auto decoded = DecodeStopJournalTerminalProjection(stored->terminal_payload);
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().certificate, stop.terminal.certificate);
}

[[nodiscard]] std::size_t CountOccurrences(const std::string& haystack, const std::string& needle) {
  std::size_t count = 0U;
  for (auto position = haystack.find(needle); position != std::string::npos;
       position = haystack.find(needle, position + 1U)) {
    ++count;
  }
  return count;
}

/// Each distinct participant plan is stored exactly once in the Stop's bytes:
/// the terminal `plans` list holds one entry per plan digest, and that entry's
/// canonical JSON occurs once across every stored payload of the Stop.
void ExpectEachPlanStoredOnce(const StoredStop& stored,
                              const std::vector<ParticipantFixture>& participants) {
  namespace ci = manyforge::content_identity;
  ci::CanonicalJsonParseLimits limits;
  limits.maximum_depth = ci::kMaximumCanonicalJsonDepth;
  const auto root = ci::parse_canonical_json(stored.terminal_payload, limits);
  ASSERT_TRUE(root.is_array());
  const auto& fields = root.as_array();
  ASSERT_EQ(fields.size(), 4U);
  ASSERT_TRUE(fields.front().is_string());
  EXPECT_EQ(fields.front().as_string(), std::string(kStopJournalTerminalProjectionApiVersion));
  ASSERT_TRUE(fields.back().is_array());
  const auto& plans = fields.back().as_array();

  std::set<std::string> expected_digests;
  for (const auto& participant : participants) {
    expected_digests.insert(participant.plan.participant_stop_invocation_plan_digest.value());
  }
  ASSERT_EQ(plans.size(), expected_digests.size());
  std::set<std::string> stored_digests;
  for (const auto& plan : plans) {
    ASSERT_TRUE(plan.is_array());
    ASSERT_FALSE(plan.as_array().empty());
    ASSERT_TRUE(plan.as_array().front().is_string());
    stored_digests.insert(plan.as_array().front().as_string());
    const auto bytes = ci::canonical_json(plan);
    EXPECT_EQ(CountOccurrences(stored.all_bytes, bytes), 1U)
        << "plan " << plan.as_array().front().as_string() << " (" << bytes.size()
        << " B) is not stored exactly once";
  }
  EXPECT_EQ(stored_digests, expected_digests);
}

void Report(const std::string& label, const StoredStop& stored) {
  std::ostringstream line;
  line << "[stop-record-size] " << label << ": accepted_payload=" << stored.accepted_payload.size()
       << " accepted_digest=" << stored.accepted_digest.size();
  for (const auto& row : stored.rows) {
    line << " " << row.record_kind << "=" << row.payload_json.size();
  }
  line << " total=" << stored.total_bytes << " B (" << (stored.total_bytes + kKiB - 1U) / kKiB
       << " KiB)";
  std::cout << line.str() << std::endl;
  ::testing::Test::RecordProperty(label + ".total_bytes", static_cast<int>(stored.total_bytes));
  ::testing::Test::RecordProperty(label + ".terminal_bytes",
                                  static_cast<int>(stored.terminal_payload.size()));
}

[[nodiscard]] std::vector<ResourceId> Inventory(const std::vector<ParticipantFixture>& participants) {
  std::vector<ResourceId> inventory;
  for (const auto& participant : participants) {
    inventory.insert(inventory.end(), participant.resource_ids.begin(),
                     participant.resource_ids.end());
  }
  std::sort(inventory.begin(), inventory.end());
  return inventory;
}

[[nodiscard]] RequestOwnerEpoch Epoch() {
  return RequestOwnerEpoch("execution-host.epoch.stop-record-size");
}

TEST(StopRecordSizeTest, OneArmOneGripperNominalStaysWithin32KiB) {
  const auto participants = Participants(1U, 1U);
  const auto stop = BuildStop(Epoch(), participants, {1U, 1U});
  StoredStop stored;
  ASSERT_NO_FATAL_FAILURE(RecordAndMeasure(stop, Inventory(participants), &stored));
  Report("arm1_gripper1_nominal", stored);
  EXPECT_LE(stored.total_bytes, 32U * kKiB);
  ExpectEachPlanStoredOnce(stored, participants);
}

TEST(StopRecordSizeTest, TwoArmsTwoGrippersNominalStaysWithin64KiB) {
  const auto participants = Participants(2U, 2U);
  const auto stop = BuildStop(Epoch(), participants, std::vector<std::size_t>(4U, 1U));
  StoredStop stored;
  ASSERT_NO_FATAL_FAILURE(RecordAndMeasure(stop, Inventory(participants), &stored));
  Report("arm2_gripper2_nominal", stored);
  EXPECT_LE(stored.total_bytes, 64U * kKiB);
  ExpectEachPlanStoredOnce(stored, participants);
}

TEST(StopRecordSizeTest, TwoArmsTwoGrippersFullyEscalatedStaysWithin96KiB) {
  const auto participants = Participants(2U, 2U);
  // Every participant runs through all its stages (a = K): arms 3, grippers 2.
  std::vector<std::size_t> attempts;
  for (const auto& participant : participants) {
    attempts.push_back(participant.plan.stage_commands.size());
  }
  const auto stop = BuildStop(Epoch(), participants, attempts);
  ASSERT_EQ(stop.terminal.certificate.result, StopCertificateResultDTO::kSucceededEscalated);
  ASSERT_EQ(stop.terminal.certificate.attempted_stage_results.size(),
            2U * kArmStageCount + 2U * kGripperStageCount);
  StoredStop stored;
  ASSERT_NO_FATAL_FAILURE(RecordAndMeasure(stop, Inventory(participants), &stored));
  Report("arm2_gripper2_escalated", stored);
  EXPECT_LE(stored.total_bytes, 96U * kKiB);
  ExpectEachPlanStoredOnce(stored, participants);
}

TEST(StopRecordSizeTest, OneExtraAttemptAddsLessThan4KiBBecauseItsPlanIsNotRepeated) {
  const auto participants = Participants(1U, 1U);
  ASSERT_EQ(participants.front().kind, ParticipantKind::kArm);
  std::vector<StoredStop> measured(kArmStageCount);
  for (std::size_t arm_attempts = 1U; arm_attempts <= kArmStageCount; ++arm_attempts) {
    const auto stop = BuildStop(Epoch(), participants, {arm_attempts, 1U});
    auto& stored = measured[arm_attempts - 1U];
    ASSERT_NO_FATAL_FAILURE(RecordAndMeasure(stop, Inventory(participants), &stored));
    Report("arm1_gripper1_arm_attempts_" + std::to_string(arm_attempts), stored);
    ExpectEachPlanStoredOnce(stored, participants);
  }
  for (std::size_t index = 1U; index < measured.size(); ++index) {
    ASSERT_GT(measured[index].total_bytes, measured[index - 1U].total_bytes);
    const auto added = measured[index].total_bytes - measured[index - 1U].total_bytes;
    std::cout << "[stop-record-size] arm attempt " << index + 1U << " adds " << added << " B"
              << std::endl;
    ::testing::Test::RecordProperty("extra_attempt_" + std::to_string(index + 1U) + ".bytes",
                                    static_cast<int>(added));
    EXPECT_LT(added, 4U * kKiB);
  }
}

}  // namespace
}  // namespace manyforge::core

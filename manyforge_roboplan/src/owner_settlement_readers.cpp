#include "owner_settlement_readers.hpp"

#include "cell_admission_command_codec.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <sqlite3.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace manyforge::roboplan_backend::detail {
namespace ci = manyforge::content_identity;
namespace core = manyforge::core;
namespace {

constexpr int kBusyTimeoutMilliseconds = 5000;
// The only Program-cycle journal schema these readers accept. It equals
// `SCHEMA_VERSION` in manyforge_program_runner/cycle_journal.py (a runner test
// checks this); older and newer development journals are refused unchanged.
constexpr int kProgramCycleJournalSchema = 9;

class Statement final {
public:
  Statement(sqlite3 *database, const char *sql) {
    if (sqlite3_prepare_v2(database, sql, -1, &statement_, nullptr) !=
        SQLITE_OK)
      throw std::runtime_error("prepare read-only owner settlement query");
  }
  ~Statement() { sqlite3_finalize(statement_); }
  Statement(const Statement &) = delete;
  Statement &operator=(const Statement &) = delete;
  void Bind(int index, std::string_view value) {
    if (sqlite3_bind_text(statement_, index, value.data(),
                          static_cast<int>(value.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK)
      throw std::runtime_error("bind read-only owner settlement query");
  }
  bool Step() {
    const int result = sqlite3_step(statement_);
    if (result == SQLITE_ROW)
      return true;
    if (result == SQLITE_DONE)
      return false;
    throw std::runtime_error("step read-only owner settlement query");
  }
  [[nodiscard]] std::int64_t Integer(int column) const {
    return sqlite3_column_int64(statement_, column);
  }
  [[nodiscard]] std::string Text(int column) const {
    const auto *value = sqlite3_column_text(statement_, column);
    if (value == nullptr)
      throw std::runtime_error("owner settlement column is null");
    return {reinterpret_cast<const char *>(value),
            static_cast<std::size_t>(sqlite3_column_bytes(statement_, column))};
  }
  [[nodiscard]] bool IsNull(int column) const {
    return sqlite3_column_type(statement_, column) == SQLITE_NULL;
  }

private:
  sqlite3_stmt *statement_{};
};

[[nodiscard]] core::Result<void> Rejected(std::string owner,
                                          std::string reason) {
  return core::Result<void>::Err(
      core::ErrorDTO{"invariant_violation",
                     owner + " cell-admission settlement is not owner-proven",
                     {{"reason", std::move(reason)}}});
}

void Exec(sqlite3 *database, const char *sql) {
  char *message = nullptr;
  const int result = sqlite3_exec(database, sql, nullptr, nullptr, &message);
  if (message != nullptr)
    sqlite3_free(message);
  if (result != SQLITE_OK)
    throw std::runtime_error("configure read-only owner settlement query");
}

[[nodiscard]] std::unique_ptr<sqlite3, decltype(&sqlite3_close)>
OpenReadOnly(const std::filesystem::path &path) {
  sqlite3 *raw = nullptr;
  const int result = sqlite3_open_v2(
      path.string().c_str(), &raw,
      SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX | SQLITE_OPEN_NOFOLLOW,
      nullptr);
  if (result != SQLITE_OK) {
    if (raw != nullptr)
      sqlite3_close(raw);
    throw std::runtime_error("open owner settlement journal read-only");
  }
  std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw,
                                                              sqlite3_close);
  if (sqlite3_busy_timeout(database.get(), kBusyTimeoutMilliseconds) !=
      SQLITE_OK)
    throw std::runtime_error("configure owner settlement timeout");
  Exec(database.get(), "PRAGMA query_only=ON");
  Exec(database.get(), "BEGIN");
  return database;
}

[[nodiscard]] bool
ExactReleaseIdentity(const core::CellAdmissionClaimDTO &claim,
                     const core::CellAdmissionClaimReleaseDTO &release) {
  return core::ValidateCellAdmissionClaimIntent(claim.intent).ok() &&
         claim.claim_generation.nonzero() &&
         claim.acquired_admission_generation.nonzero() &&
         core::ValidateCellAdmissionClaimRelease(release).ok() &&
         release.owner_role == claim.intent.owner_role &&
         release.owner_epoch == claim.intent.owner_epoch &&
         release.owner_transaction_id == claim.intent.owner_transaction_id &&
         release.request_id == claim.intent.request_id &&
         release.request_payload_digest ==
             claim.intent.request_payload_digest &&
         release.acquired_admission_generation ==
             claim.acquired_admission_generation &&
         release.claim_generation == claim.claim_generation;
}

[[nodiscard]] const ci::CanonicalValue &Member(const ci::CanonicalValue &value,
                                               std::string_view name) {
  if (!value.is_object())
    throw std::runtime_error("owner settlement JSON is not an object");
  const auto *member = value.find_object_member(name);
  if (member == nullptr)
    throw std::runtime_error("owner settlement JSON field is absent");
  return *member;
}

[[nodiscard]] std::string StringMember(const ci::CanonicalValue &value,
                                       std::string_view name) {
  const auto &member = Member(value, name);
  if (!member.is_string())
    throw std::runtime_error("owner settlement JSON field is not text");
  return member.as_string();
}

[[nodiscard]] bool ProgramStartSettlementMatches(
    std::string_view encoded, const core::CellAdmissionClaimDTO &claim,
    std::string_view program_run_id, std::string_view reference) {
  const auto settlement = ci::parse_canonical_json(encoded);
  const auto kind = StringMember(settlement, "kind");
  const auto &prepared_scene = Member(settlement, "prepared_scene");
  const bool scene_matches =
      prepared_scene.is_object() &&
      (prepared_scene.as_object().empty() ||
       StringMember(prepared_scene, "schema_version") ==
           "manyforge.prepared_program_scene.v1");
  if (StringMember(settlement, "schema_version") !=
          "manyforge.program_cycle_start_settlement.v2" ||
      (kind != "proven_no_effect" && kind != "terminal_effects_settled") ||
      DecodeCellAdmissionClaim(
          ci::canonical_json(Member(settlement, "claim"))) != claim ||
      !scene_matches ||
      StringMember(settlement, "program_run_id") != program_run_id ||
      ci::sha256_digest(ci::canonical_json(settlement)) != reference)
    return false;
  if (kind == "proven_no_effect")
    return true;
  const auto &terminal = Member(settlement, "terminal_run");
  const auto state = StringMember(terminal, "state");
  return (state == "stopped" || state == "succeeded" || state == "failed") &&
         StringMember(Member(terminal, "capture"), "program_run_id") ==
             program_run_id;
}

[[nodiscard]] bool
ProgramTerminalSnapshotMatches(sqlite3 *database,
                               std::string_view program_run_id,
                               std::string_view reference) {
  Statement history(database,
                    "SELECT snapshot_json,terminal FROM program_run_history "
                    "WHERE program_run_id=?");
  history.Bind(1, program_run_id);
  if (!history.Step() || history.Integer(1) != 1)
    return false;
  const auto snapshot = ci::parse_canonical_json(history.Text(0));
  const auto state = StringMember(snapshot, "state");
  const auto retained_id =
      StringMember(Member(snapshot, "capture"), "program_run_id");
  const std::string preimage =
      "{\"program_run_id\":" +
      ci::canonical_json(ci::CanonicalValue(std::string(program_run_id))) +
      ",\"run_revision\":" +
      ci::canonical_json(Member(snapshot, "run_revision")) +
      ",\"state\":" + ci::canonical_json(ci::CanonicalValue(state)) + "}";
  return retained_id == program_run_id &&
         (state == "stopped" || state == "succeeded" || state == "failed") &&
         ci::sha256_digest(preimage) == reference && !history.Step();
}

struct RoleReplacement final {
  core::RequestOwnerEpoch predecessor;
  core::RequestOwnerEpoch successor;
};

[[nodiscard]] RoleReplacement ValidateRoleReplacementEnvelope(
    core::CellAdmissionOwnerRoleDTO role,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile) {
  if (held_claims != reconcile.retained_claims ||
      current_owner_epochs.size() != reconcile.current_owner_epochs.size() ||
      !std::is_sorted(held_claims.begin(), held_claims.end(),
                      [](const auto &left, const auto &right) {
                        return left.claim_generation < right.claim_generation;
                      }) ||
      std::adjacent_find(held_claims.begin(), held_claims.end(),
                         [](const auto &left, const auto &right) {
                           return left.claim_generation ==
                                  right.claim_generation;
                         }) != held_claims.end() ||
      !std::is_sorted(current_owner_epochs.begin(), current_owner_epochs.end(),
                      [](const auto &left, const auto &right) {
                        return std::tie(left.owner_role, left.owner_epoch) <
                               std::tie(right.owner_role, right.owner_epoch);
                      }) ||
      std::adjacent_find(current_owner_epochs.begin(),
                         current_owner_epochs.end(),
                         [](const auto &left, const auto &right) {
                           return left.owner_role == right.owner_role;
                         }) != current_owner_epochs.end() ||
      !std::is_sorted(reconcile.current_owner_epochs.begin(),
                      reconcile.current_owner_epochs.end(),
                      [](const auto &left, const auto &right) {
                        return std::tie(left.owner_role, left.owner_epoch) <
                               std::tie(right.owner_role, right.owner_epoch);
                      }) ||
      std::adjacent_find(reconcile.current_owner_epochs.begin(),
                         reconcile.current_owner_epochs.end(),
                         [](const auto &left, const auto &right) {
                           return left.owner_role == right.owner_role;
                         }) != reconcile.current_owner_epochs.end())
    throw std::runtime_error("owner reconciliation inventory is not exact");

  std::optional<RoleReplacement> replacement;
  for (const auto &current : current_owner_epochs) {
    const auto requested = std::find_if(
        reconcile.current_owner_epochs.begin(),
        reconcile.current_owner_epochs.end(), [&](const auto &candidate) {
          return candidate.owner_role == current.owner_role;
        });
    if (requested == reconcile.current_owner_epochs.end())
      throw std::runtime_error("owner reconciliation omitted a role");
    if (requested->owner_epoch == current.owner_epoch)
      continue;
    if (current.owner_role != role || replacement.has_value())
      throw std::runtime_error("owner reconciliation changes multiple roles");
    replacement = RoleReplacement{current.owner_epoch, requested->owner_epoch};
  }
  if (!replacement.has_value() || replacement->predecessor.empty() ||
      replacement->successor.empty())
    throw std::runtime_error("owner reconciliation is not one replacement");

  const auto expected =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          role, replacement->successor);
  if (!expected.ok() ||
      reconcile.reconciliation_evidence_digest != expected.value())
    throw std::runtime_error("owner reconciliation evidence differs");

  for (const auto &claim : held_claims) {
    const auto owner = std::find_if(
        reconcile.current_owner_epochs.begin(),
        reconcile.current_owner_epochs.end(), [&](const auto &candidate) {
          return candidate.owner_role == claim.intent.owner_role;
        });
    if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
        !claim.claim_generation.nonzero() ||
        !claim.acquired_admission_generation.nonzero() ||
        owner == reconcile.current_owner_epochs.end() ||
        (claim.intent.owner_role != role &&
         claim.intent.owner_epoch != owner->owner_epoch) ||
        (claim.intent.owner_role == role &&
         claim.intent.owner_epoch != replacement->predecessor))
      throw std::runtime_error("held claim owner inventory differs");
  }
  return *replacement;
}

// One read transaction joins cycle phase, original accepted Reset and its
// exact claim register. Called only from existing bounded read-only adapters;
// no Runner transaction remains open across the calling native RPC.
struct ProgramResetJournalSnapshot final {
  core::ProgramCycleStatusDTO status;
  std::string phase;
  std::optional<core::CellAdmissionClaimIntentDTO> effect_intent;
  std::optional<core::CellAdmissionClaimDTO> claim;
  std::optional<core::CellAdmissionClaimReleaseDTO> release;
  ci::CanonicalValue claim_value;
};

ProgramResetJournalSnapshot ReadResetSnapshot(sqlite3* database) {
  Statement metadata(database, "SELECT schema_version FROM program_cycle_metadata WHERE singleton=1");
  if (!metadata.Step() || metadata.Integer(0) != kProgramCycleJournalSchema || metadata.Step())
    throw std::runtime_error("Program-cycle journal schema differs");
  Statement row(database,
      "SELECT state,owner_epoch,cycle_generation,program_run_id,claim_json,terminal_reference,"
      "reconciliation_required,acquisition_pending_json,reset_phase,reset_effect_claim_json,"
      "start_settlement_json,release_intent_json "
      "FROM program_cycle WHERE singleton=1");
  if (!row.Step() || row.Text(0) != "resetting" || row.Integer(2) <= 0 ||
      row.Integer(6) != 0 || !row.IsNull(7) || row.IsNull(8))
    throw std::runtime_error("Program Reset is not current");
  const auto generation = static_cast<std::uint64_t>(row.Integer(2));
  ProgramResetJournalSnapshot result{
      {core::RequestOwnerEpoch(row.Text(1)), generation, core::ProgramCycleStateDTO::kResetting,
       std::nullopt, std::nullopt, std::nullopt}, row.Text(8), std::nullopt, std::nullopt,
       std::nullopt, ci::CanonicalValue(nullptr)};
  if (row.IsNull(4)) {
    if (row.IsNull(3)) {
      if (!row.IsNull(5) || !row.IsNull(10) || !row.IsNull(11))
        throw std::runtime_error("no-run Reset retains Program settlement");
    } else {
      // Existing failed-Start proof is explicit and digest pinned. A missing
      // Program-cycle claim alone grants no selected effect authority.
      if (row.IsNull(5) || row.IsNull(10) || !row.IsNull(11))
        throw std::runtime_error("claimless failed Start has no exact settlement");
      const auto settlement = ci::parse_canonical_json(row.Text(10));
      const auto& prepared = Member(settlement, "prepared_scene");
      if (StringMember(settlement, "schema_version") != "manyforge.program_cycle_start_settlement.v2" ||
          StringMember(settlement, "kind") != "proven_no_effect" ||
          !Member(settlement, "claim").is_null() ||
          StringMember(settlement, "program_run_id") != row.Text(3) ||
          !prepared.is_object() || (!prepared.as_object().empty() &&
            StringMember(prepared, "schema_version") != "manyforge.prepared_program_scene.v1") ||
          ci::sha256_digest(ci::canonical_json(settlement)) != row.Text(5))
        throw std::runtime_error("claimless failed Start settlement differs");
      result.status.program_run_id = core::ProgramRunId(row.Text(3));
      result.status.terminal_or_settlement_reference = core::ContentDigest(row.Text(5));
    }
    if (row.IsNull(9))
      throw std::runtime_error("idle Reset claim register is absent or mixed");
    const auto registered = ci::parse_canonical_json(row.Text(9));
    if (!registered.is_object() || registered.as_object().size() != 3U)
      throw std::runtime_error("Program Reset claim register shape differs");
    result.effect_intent = DecodeCellAdmissionClaimIntent(ci::canonical_json(Member(registered, "intent")));
    if (!Member(registered, "claim").is_null()) {
      result.claim_value = Member(registered, "claim");
      result.claim = DecodeCellAdmissionClaim(ci::canonical_json(result.claim_value));
      if (result.claim->intent != *result.effect_intent)
        throw std::runtime_error("Program Reset receipt intent differs");
    }
    if (!Member(registered, "release_intent").is_null())
      result.release = DecodeCellAdmissionClaimRelease(ci::canonical_json(Member(registered, "release_intent")));
    if (result.effect_intent->claim_kind != core::CellAdmissionClaimKindDTO::kProgramReset ||
        result.effect_intent->owner_epoch != result.status.program_runner_owner_epoch)
      throw std::runtime_error("Program Reset claim owner differs");
  } else {
    if (row.IsNull(3) || row.IsNull(5) || !row.IsNull(9))
      throw std::runtime_error("run Reset claim is absent or mixed");
    result.status.program_run_id = core::ProgramRunId(row.Text(3));
    result.status.terminal_or_settlement_reference = core::ContentDigest(row.Text(5));
    result.claim_value = ci::parse_canonical_json(row.Text(4));
    result.claim = DecodeCellAdmissionClaim(ci::canonical_json(result.claim_value));
    if (result.claim->intent.claim_kind != core::CellAdmissionClaimKindDTO::kProgramCycle)
      throw std::runtime_error("run Reset parent kind differs");
    result.status.cycle_claim_generation = result.claim->claim_generation;
  }
  if (row.Step()) throw std::runtime_error("duplicate Program-cycle row");
  Statement pending(database,
      "SELECT request_owner_epoch,request_id,intent_json,cycle_generation "
      "FROM program_cycle_inflight_commands WHERE request_kind='reset'");
  if (!pending.Step() || pending.Integer(3) != static_cast<std::int64_t>(generation))
    throw std::runtime_error("accepted Program Reset occurrence absent");
  const auto original = ci::parse_canonical_json(pending.Text(2));
  const auto& reload = Member(original, "reload_initial_scene");
  const auto& position = Member(original, "reset_robot_position");
  if (!reload.is_bool() || !position.is_bool() ||
      StringMember(original, "request_owner_epoch") != pending.Text(0) ||
      StringMember(original, "request_id") != pending.Text(1) ||
      !Member(original, "expected_cycle_generation").is_number() ||
      Member(original, "expected_cycle_generation").as_number() != static_cast<double>(generation))
    throw std::runtime_error("accepted Reset identity or selectors differ");
  if (result.status.program_run_id) {
    if (StringMember(original, "program_run_id") != result.status.program_run_id->value())
      throw std::runtime_error("accepted Reset run differs");
  } else if (!Member(original, "program_run_id").is_null() ||
             StringMember(original, "expected_owner_epoch") != result.status.program_runner_owner_epoch.value()) {
    throw std::runtime_error("accepted idle Reset owner differs");
  }
  const auto digest = ci::sha256_digest(ci::canonical_json(original));
  result.status.accepted_reset = core::AcceptedProgramResetDTO{
      core::RequestOwnerEpoch(pending.Text(0)), core::RequestId(pending.Text(1)),
      core::ContentDigest(digest), reload.as_bool(), position.as_bool(),
      result.effect_intent && result.claim
          ? std::optional(result.claim->claim_generation) : std::nullopt};
  if (!core::ValidateProgramCycleStatus(result.status).ok() || pending.Step())
    throw std::runtime_error("accepted Reset projection is invalid");
  if (result.effect_intent) {
    const auto identity = core::ProgramResetClaimRequestId(result.status);
    if (!identity.ok() || result.effect_intent->request_id != identity.value() ||
        result.effect_intent->owner_transaction_id != identity.value().value() ||
        result.effect_intent->request_payload_digest.value() != digest)
      throw std::runtime_error("accepted Reset claim identity differs");
  }
  return result;
}

} // namespace

core::Result<std::optional<ActiveProgramRunProductIdentity>>
ReadActiveProgramRunProductForReconciliation(
    const std::filesystem::path &database_path) {
  try {
    std::error_code status_error;
    const auto status =
        std::filesystem::symlink_status(database_path, status_error);
    if (status_error == std::errc::no_such_file_or_directory ||
        status.type() == std::filesystem::file_type::not_found) {
      return core::Result<std::optional<ActiveProgramRunProductIdentity>>::Ok(
          std::nullopt);
    }
    if (status_error || !std::filesystem::is_regular_file(status))
      throw std::runtime_error("Program-cycle journal is not a regular file");
    auto database = OpenReadOnly(database_path);
    Statement metadata(
        database.get(),
        "SELECT schema_version FROM program_cycle_metadata WHERE singleton=1");
    if (!metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    const auto schema_version = metadata.Integer(0);
    if (schema_version != kProgramCycleJournalSchema || metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");

    Statement row(database.get(),
                  "SELECT state,program_run_id,reconciliation_required "
                  "FROM program_cycle WHERE singleton=1");
    if (!row.Step())
      throw std::runtime_error("Program-cycle owner row is absent");
    const auto state = row.Text(0);
    const auto reconciliation_flag = row.Integer(2);
    if (reconciliation_flag != 0 && reconciliation_flag != 1)
      throw std::runtime_error("Program-cycle reconciliation flag is invalid");
    const bool reconciliation_required = reconciliation_flag == 1;
    if (!reconciliation_required || state == "idle_after_reset" ||
        state == "terminal_awaiting_reset") {
      if (row.Step())
        throw std::runtime_error("Program-cycle owner row is duplicated");
      Exec(database.get(), "COMMIT");
      return core::Result<std::optional<ActiveProgramRunProductIdentity>>::Ok(
          std::nullopt);
    }
    if ((state != "starting" && state != "running" && state != "stopping" &&
         state != "resetting" && state != "recovery_required") ||
        row.IsNull(1))
      throw std::runtime_error("Program-cycle recovery state is invalid");
    const auto program_run_id = row.Text(1);
    if (row.Step())
      throw std::runtime_error("Program-cycle owner row is duplicated");

    Statement history(database.get(),
                      "SELECT snapshot_json,terminal FROM program_run_history "
                      "WHERE program_run_id=?");
    history.Bind(1, program_run_id);
    if (!history.Step())
      throw std::runtime_error("active Program-run snapshot is absent");
    const auto snapshot = ci::parse_canonical_json(history.Text(0));
    const auto terminal_flag = history.Integer(1);
    const bool terminal = terminal_flag == 1;
    if (history.Step())
      throw std::runtime_error("active Program-run snapshot is duplicated");
    const auto &capture = Member(snapshot, "capture");
    const auto captured_run_id = StringMember(capture, "program_run_id");
    const auto retained_bundle_id = StringMember(capture, "retained_bundle_id");
    const auto graph_revision = StringMember(capture, "graph_revision");
    if (captured_run_id != program_run_id || retained_bundle_id.empty() ||
        graph_revision.empty())
      throw std::runtime_error("active Program-run capture is invalid");
    const auto run_state = StringMember(snapshot, "state");
    const bool terminal_state = run_state == "stopped" ||
                                run_state == "succeeded" ||
                                run_state == "failed";
    if ((terminal_flag != 0 && terminal_flag != 1) ||
        terminal != terminal_state ||
        (!terminal_state && run_state != "running" &&
         run_state != "intervention_required" && run_state != "full_stopping" &&
         run_state != "full_stopped" && run_state != "stopping" &&
         run_state != "finalizing"))
      throw std::runtime_error(
          "active Program-run state and terminal evidence disagree");
    Exec(database.get(), "COMMIT");
    if (terminal) {
      return core::Result<std::optional<ActiveProgramRunProductIdentity>>::Ok(
          std::nullopt);
    }
    return core::Result<std::optional<ActiveProgramRunProductIdentity>>::Ok(
        ActiveProgramRunProductIdentity{
            core::ProgramRunId(program_run_id),
            core::RetainedBundleId(retained_bundle_id),
            core::GraphRevision(graph_revision)});
  } catch (const std::exception &error) {
    return core::Result<std::optional<ActiveProgramRunProductIdentity>>::Err(
        core::ErrorDTO{"invariant_violation",
                       "active Program-run recovery product cannot be proven",
                       {{"reason", "program_run_recovery_product_unavailable"},
                        {"detail", error.what()}}});
  }
}

core::Result<void> ValidateProgramCycleSettlementReadOnly(
    const std::filesystem::path &database_path,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  if (!ExactReleaseIdentity(claim, release) ||
      claim.intent.claim_kind !=
          core::CellAdmissionClaimKindDTO::kProgramCycle ||
      claim.intent.owner_role !=
          core::CellAdmissionOwnerRoleDTO::kProgramRunner ||
      release.settlement.settlement_kind !=
          core::CellAdmissionSettlementKindDTO::kExplicitReset)
    return Rejected("Program-cycle", "program_cycle_release_identity_mismatch");
  try {
    auto database = OpenReadOnly(database_path);
    Statement metadata(
        database.get(),
        "SELECT schema_version FROM program_cycle_metadata WHERE singleton=1");
    if (!metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    const auto schema_version = metadata.Integer(0);
    if (schema_version != kProgramCycleJournalSchema || metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    Statement row(
        database.get(),
        "SELECT state,owner_epoch,program_run_id,claim_json,"
        "terminal_reference,start_settlement_json,release_intent_json,"
        "reconciliation_required,acquisition_pending_json,reset_phase "
        "FROM program_cycle WHERE singleton=1");
    if (!row.Step())
      throw std::runtime_error("Program-cycle owner row is absent");
    const auto retained_claim = DecodeCellAdmissionClaim(row.Text(3));
    const auto retained_release = DecodeCellAdmissionClaimRelease(row.Text(6));
    const auto evidence_matches =
        row.IsNull(5) ? ProgramTerminalSnapshotMatches(database.get(),
                                                       row.Text(2), row.Text(4))
                      : ProgramStartSettlementMatches(row.Text(5), claim,
                                                      row.Text(2), row.Text(4));
    const bool lifecycle_is_exact = row.Text(0) == "resetting" &&
                                    !row.IsNull(9) &&
                                    row.Text(9) == "releasing";
    const bool valid =
        lifecycle_is_exact && row.Text(1) == claim.intent.owner_epoch.value() &&
        retained_claim == claim && retained_release == release &&
        row.Text(4) == release.settlement.evidence_digest.value() &&
        evidence_matches && row.Integer(7) == 0 && row.IsNull(8) && !row.Step();
    Exec(database.get(), "COMMIT");
    if (!valid)
      return Rejected("Program-cycle",
                      "program_cycle_terminal_evidence_mismatch");
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    return Rejected("Program-cycle", "program_cycle_journal_read_failed");
  }
}

core::Result<core::ProgramCycleStatusDTO> ReadProgramResetStatusReadOnly(
    const std::filesystem::path& database_path, const core::ProgramResetJournalReadPurpose purpose,
    const core::CellAdmissionClaimIntentDTO& intent,
    const std::optional<core::CellAdmissionClaimDTO>& claim) {
  try {
    auto database = OpenReadOnly(database_path);
    const auto snapshot = ReadResetSnapshot(database.get());
    const bool acquiring = purpose == core::ProgramResetJournalReadPurpose::kAcquire;
    const auto& accepted = *snapshot.status.accepted_reset;
    if ((acquiring &&
         (snapshot.status.cycle_claim_generation || !snapshot.effect_intent ||
          *snapshot.effect_intent != intent || snapshot.claim || snapshot.release ||
          snapshot.phase != (accepted.reload_initial_scene ? "restoring_scene" : "restoring_position"))) ||
        (!acquiring &&
         ((purpose == core::ProgramResetJournalReadPurpose::kReloadInitialScene
               ? (!accepted.reload_initial_scene || snapshot.phase != "restoring_scene")
               : (!accepted.reset_robot_position || snapshot.phase != "restoring_position")) ||
          !snapshot.claim || !claim || *snapshot.claim != *claim ||
          snapshot.claim->intent != intent || snapshot.release)))
      throw std::runtime_error("Program Reset effect phase or authority differs");
    Exec(database.get(), "COMMIT");
    return core::Result<core::ProgramCycleStatusDTO>::Ok(snapshot.status);
  } catch (const std::exception&) {
    return core::Result<core::ProgramCycleStatusDTO>::Err(
        {"authoring_not_ready", "Program Reset journal is not current",
         {{"reason", "program_reset_journal_evidence_mismatch"}}});
  }
}

core::Result<void> ValidateProgramResetSettlementReadOnly(
    const std::filesystem::path& database_path,
    const core::CellAdmissionClaimDTO& claim,
    const core::CellAdmissionClaimReleaseDTO& release) {
  if (!ExactReleaseIdentity(claim, release) ||
      claim.intent.claim_kind != core::CellAdmissionClaimKindDTO::kProgramReset ||
      release.settlement.settlement_kind != core::CellAdmissionSettlementKindDTO::kExplicitReset)
    return Rejected("Program Reset", "program_reset_release_identity_mismatch");
  try {
    auto database = OpenReadOnly(database_path);
    const auto snapshot = ReadResetSnapshot(database.get());
    if (snapshot.status.cycle_claim_generation || snapshot.phase != "releasing" ||
        snapshot.claim != claim || snapshot.release != release)
      throw std::runtime_error("Program Reset settlement authority differs");
    const auto& accepted = *snapshot.status.accepted_reset;
    const auto evidence = ci::canonical_json(ci::CanonicalValue(ci::CanonicalValue::Object{
        {"schema_version", ci::CanonicalValue("manyforge.program_reset_effect_settlement.v1")},
        {"cycle_generation", ci::CanonicalValue(static_cast<double>(snapshot.status.cycle_generation))},
        {"request_owner_epoch", ci::CanonicalValue(accepted.request_owner_epoch.value())},
        {"request_id", ci::CanonicalValue(accepted.request_id.value())},
        {"request_payload_digest", ci::CanonicalValue(accepted.request_payload_digest.value())},
        {"claim", snapshot.claim_value}}));
    if (ci::sha256_digest(evidence) != release.settlement.evidence_digest.value())
      throw std::runtime_error("Program Reset settlement digest differs");
    Exec(database.get(), "COMMIT");
    return core::Result<void>::Ok();
  } catch (const std::exception&) {
    return Rejected("Program Reset", "program_reset_journal_evidence_mismatch");
  }
}

core::Result<core::CellAdmissionReconcileValidationDTO>
ValidateProgramRunnerOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile) {
  try {
    const auto replacement = ValidateRoleReplacementEnvelope(
        core::CellAdmissionOwnerRoleDTO::kProgramRunner, held_claims,
        current_owner_epochs, reconcile);
    auto database = OpenReadOnly(database_path);
    Statement metadata(
        database.get(),
        "SELECT schema_version FROM program_cycle_metadata WHERE singleton=1");
    if (!metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    const auto schema_version = metadata.Integer(0);
    if (schema_version != kProgramCycleJournalSchema || metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    Statement row(
        database.get(),
        "SELECT state,owner_epoch,program_run_id,claim_json,"
        "release_intent_json,terminal_reference,start_settlement_json,"
        "reconciliation_required,acquisition_pending_json,"
        "reconciliation_evidence_digest "
        "FROM program_cycle WHERE singleton=1");
    if (!row.Step() || row.Text(1) != replacement.predecessor.value() ||
        row.Integer(7) != 1 || !row.IsNull(8) || row.IsNull(9) ||
        row.Text(9) != reconcile.reconciliation_evidence_digest.value())
      throw std::runtime_error(
          "Program-cycle reconciliation intent is not exact");

    core::CellAdmissionReconcileValidationDTO validation{
        reconcile.current_owner_epochs, {}};
    const auto state = row.Text(0);
    if (state == "idle_after_reset") {
      if (!row.IsNull(2) || !row.IsNull(3) || !row.IsNull(4) ||
          !row.IsNull(5) || !row.IsNull(6))
        throw std::runtime_error("idle Program-cycle retains a claim");
    } else if (state == "terminal_awaiting_reset") {
      if (row.IsNull(2) || row.IsNull(3) || row.IsNull(5))
        throw std::runtime_error("terminal Program-cycle evidence is absent");
      const auto claim = DecodeCellAdmissionClaim(row.Text(3));
      if (claim.intent.claim_kind !=
              core::CellAdmissionClaimKindDTO::kProgramCycle ||
          claim.intent.owner_role !=
              core::CellAdmissionOwnerRoleDTO::kProgramRunner)
        throw std::runtime_error("terminal Program-cycle evidence differs");
      const auto evidence_matches =
          row.IsNull(6)
              ? ProgramTerminalSnapshotMatches(database.get(), row.Text(2),
                                               row.Text(5))
              : ProgramStartSettlementMatches(row.Text(6), claim, row.Text(2),
                                              row.Text(5));
      if (!evidence_matches)
        throw std::runtime_error("terminal Program-cycle outcome differs");

      const bool claim_is_held =
          std::find(held_claims.begin(), held_claims.end(), claim) !=
          held_claims.end();
      if (!row.IsNull(4)) {
        const auto release = DecodeCellAdmissionClaimRelease(row.Text(4));
        if (!ExactReleaseIdentity(claim, release) ||
            release.settlement.settlement_kind !=
                core::CellAdmissionSettlementKindDTO::kExplicitReset ||
            row.Text(5) != release.settlement.evidence_digest.value() ||
            claim_is_held)
          throw std::runtime_error("released Program-cycle evidence differs");
      } else if (claim_is_held) {
        if (claim.intent.owner_epoch != replacement.predecessor)
          throw std::runtime_error("held Program-cycle owner differs");
        validation.settlements.push_back(
            {claim, core::CellAdmissionOwnerRoleDTO::kProgramRunner,
             replacement.successor,
             core::CellAdmissionSettlementKindDTO::kReconciled,
             reconcile.reconciliation_evidence_digest});
      } else if (claim.intent.owner_epoch == replacement.predecessor) {
        throw std::runtime_error("terminal Program-cycle settlement is absent");
      }
    } else {
      throw std::runtime_error("active Program-cycle cannot rotate");
    }
    if (row.Step())
      throw std::runtime_error("Program-cycle owner row is not exact");
    const auto predecessor_claims = std::count_if(
        held_claims.begin(), held_claims.end(), [&](const auto &claim) {
          return claim.intent.owner_role ==
                     core::CellAdmissionOwnerRoleDTO::kProgramRunner &&
                 claim.intent.owner_epoch == replacement.predecessor;
        });
    if (predecessor_claims !=
        static_cast<std::ptrdiff_t>(validation.settlements.size()))
      throw std::runtime_error("Program-cycle held inventory differs");
    Exec(database.get(), "COMMIT");
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok(
        std::move(validation));
  } catch (const std::exception &) {
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
        core::ErrorDTO{
            "invariant_violation",
            "Program Runner owner reconciliation is not proven",
            {{"reason", "program_runner_reconciliation_evidence_invalid"}}});
  }
}

} // namespace manyforge::roboplan_backend::detail

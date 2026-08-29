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

[[nodiscard]] std::string
AuthoringEvidenceDigest(std::string_view owner_transaction_id,
                        std::string_view settlement_kind,
                        std::string_view outcome_json) {
  // Python's journal uses sorted compact JSON. These three sorted keys and the
  // already-canonical retained outcome let us reproduce its exact preimage
  // without interpreting or reformatting outcome numbers.
  const auto quoted_transaction =
      ci::canonical_json(ci::CanonicalValue(std::string(owner_transaction_id)));
  const auto quoted_kind =
      ci::canonical_json(ci::CanonicalValue(std::string(settlement_kind)));
  const std::string preimage =
      "{\"outcome\":" + std::string(outcome_json) +
      ",\"owner_transaction_id\":" + quoted_transaction +
      ",\"settlement_kind\":" + quoted_kind + "}";
  return ci::sha256_digest(preimage);
}

[[nodiscard]] bool ProgramStartSettlementMatches(
    std::string_view encoded, const core::CellAdmissionClaimDTO &claim,
    std::string_view program_run_id, std::string_view reference) {
  const auto settlement = ci::parse_canonical_json(encoded);
  const auto kind = StringMember(settlement, "kind");
  if (StringMember(settlement, "schema_version") !=
          "manyforge.program_cycle_start_settlement.v1" ||
      (kind != "proven_no_effect" && kind != "terminal_effects_settled") ||
      DecodeCellAdmissionClaim(
          ci::canonical_json(Member(settlement, "claim"))) != claim ||
      StringMember(Member(settlement, "prepared_scene"), "schema_version") !=
          "manyforge.prepared_program_scene.v1" ||
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

[[nodiscard]] core::CellAdmissionClaimReleaseDTO
AuthoringRelease(const core::CellAdmissionClaimDTO &claim,
                 const ci::CanonicalValue &settlement) {
  core::CellAdmissionClaimReleaseDTO release{
      claim.intent.owner_role,
      claim.intent.owner_epoch,
      claim.intent.owner_transaction_id,
      claim.intent.request_id,
      claim.intent.request_payload_digest,
      claim.acquired_admission_generation,
      claim.claim_generation,
      {core::CellAdmissionSettlementKindDTO::kOwnerCommit,
       core::ContentDigest(StringMember(settlement, "evidence_digest"))}};
  const auto kind = StringMember(settlement, "settlement_kind");
  if (kind == "proven_no_effect")
    release.settlement.settlement_kind =
        core::CellAdmissionSettlementKindDTO::kProvenNoEffect;
  else if (kind != "owner_commit")
    throw std::runtime_error("authoring reconciliation settlement differs");
  return release;
}

} // namespace

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
    if (!metadata.Step() || metadata.Integer(0) != 3 || metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    Statement row(
        database.get(),
        "SELECT state,owner_epoch,program_run_id,claim_json,terminal_reference,"
        "start_settlement_json,release_intent_json,reconciliation_required,"
        "acquisition_pending_json "
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
    const bool valid =
        row.Text(0) == "terminal_awaiting_reset" &&
        row.Text(1) == claim.intent.owner_epoch.value() &&
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

core::Result<void> ValidateAuthoringSettlementReadOnly(
    const std::filesystem::path &database_path,
    const core::CellAdmissionClaimDTO &claim,
    const core::CellAdmissionClaimReleaseDTO &release) {
  const auto settlement_kind = release.settlement.settlement_kind;
  if (!ExactReleaseIdentity(claim, release) ||
      claim.intent.claim_kind !=
          core::CellAdmissionClaimKindDTO::kAuthoringMutation ||
      claim.intent.owner_role !=
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner ||
      (settlement_kind != core::CellAdmissionSettlementKindDTO::kOwnerCommit &&
       settlement_kind !=
           core::CellAdmissionSettlementKindDTO::kProvenNoEffect))
    return Rejected("Composer authoring",
                    "authoring_release_identity_mismatch");
  try {
    auto database = OpenReadOnly(database_path);
    Statement version(database.get(), "PRAGMA user_version");
    if (!version.Step() || version.Integer(0) != 1 || version.Step())
      throw std::runtime_error("Composer authoring journal schema differs");
    Statement row(
        database.get(),
        "SELECT owner_epoch,claim_json,effect_intent_json,outcome_json,"
        "post_image_json,release_intent_json,release_receipt_json,"
        "json_valid(outcome_json),json_type(outcome_json) "
        "FROM transactions WHERE owner_transaction_id=?");
    row.Bind(1, claim.intent.owner_transaction_id);
    if (!row.Step())
      throw std::runtime_error("Composer authoring transaction is absent");
    const auto retained_claim = DecodeCellAdmissionClaim(row.Text(1));
    const auto outcome_json = row.Text(3);
    const auto release_intent = ci::parse_canonical_json(row.Text(5));
    const auto expected_kind = std::string(core::to_string(settlement_kind));
    const bool valid =
        row.Text(0) == claim.intent.owner_epoch.value() &&
        retained_claim == claim && release_intent.is_object() &&
        release_intent.as_object().size() == 2U &&
        StringMember(release_intent, "settlement_kind") == expected_kind &&
        StringMember(release_intent, "evidence_digest") ==
            release.settlement.evidence_digest.value() &&
        AuthoringEvidenceDigest(claim.intent.owner_transaction_id,
                                expected_kind, outcome_json) ==
            release.settlement.evidence_digest.value() &&
        (settlement_kind ==
             core::CellAdmissionSettlementKindDTO::kProvenNoEffect ||
         (!row.IsNull(2) && !row.IsNull(4))) &&
        row.IsNull(6) && row.Integer(7) == 1 && row.Text(8) == "object" &&
        !row.Step();
    Exec(database.get(), "COMMIT");
    if (!valid)
      return Rejected("Composer authoring",
                      "authoring_terminal_evidence_mismatch");
    return core::Result<void>::Ok();
  } catch (const std::exception &) {
    return Rejected("Composer authoring", "authoring_journal_read_failed");
  }
}

core::Result<core::CellAdmissionReconcileValidationDTO>
ValidateAuthoringOwnerReconciliationReadOnly(
    const std::filesystem::path &database_path,
    const std::vector<core::CellAdmissionClaimDTO> &held_claims,
    const std::vector<core::CellAdmissionCurrentOwnerEpochDTO>
        &current_owner_epochs,
    const core::CellAdmissionReconcileDTO &reconcile) {
  try {
    const auto replacement = ValidateRoleReplacementEnvelope(
        core::CellAdmissionOwnerRoleDTO::kAuthoringOwner, held_claims,
        current_owner_epochs, reconcile);
    auto database = OpenReadOnly(database_path);
    Statement version(database.get(), "PRAGMA user_version");
    if (!version.Step() || version.Integer(0) != 1 || version.Step())
      throw std::runtime_error("Composer authoring journal schema differs");
    Statement active(database.get(),
                     "SELECT owner_epoch FROM owner_epochs WHERE active=1");
    if (!active.Step() || active.Text(0) != replacement.successor.value() ||
        active.Step())
      throw std::runtime_error("Composer authoring successor is not active");

    core::CellAdmissionReconcileValidationDTO validation{
        reconcile.current_owner_epochs, {}};
    std::int64_t predecessor_unresolved = 0;
    Statement unresolved(
        database.get(), "SELECT count(*) FROM transactions WHERE owner_epoch=? "
                        "AND release_receipt_json IS NULL");
    unresolved.Bind(1, replacement.predecessor.value());
    if (!unresolved.Step())
      throw std::runtime_error("Composer authoring unresolved count is absent");
    predecessor_unresolved = unresolved.Integer(0);
    if (unresolved.Step())
      throw std::runtime_error("Composer authoring unresolved count differs");

    for (const auto &claim : held_claims) {
      if (claim.intent.owner_role !=
              core::CellAdmissionOwnerRoleDTO::kAuthoringOwner ||
          claim.intent.owner_epoch == replacement.successor)
        continue;
      Statement row(database.get(),
                    "SELECT release_intent_json FROM transactions "
                    "WHERE owner_transaction_id=? AND owner_epoch=? "
                    "AND release_receipt_json IS NULL");
      row.Bind(1, claim.intent.owner_transaction_id);
      row.Bind(2, replacement.predecessor.value());
      if (!row.Step())
        throw std::runtime_error("authoring predecessor settlement is absent");
      const auto release =
          AuthoringRelease(claim, ci::parse_canonical_json(row.Text(0)));
      if (row.Step() ||
          !ValidateAuthoringSettlementReadOnly(database_path, claim, release)
               .ok())
        throw std::runtime_error("authoring predecessor settlement differs");
      validation.settlements.push_back(
          {claim, core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
           replacement.successor,
           core::CellAdmissionSettlementKindDTO::kReconciled,
           reconcile.reconciliation_evidence_digest});
    }
    if (predecessor_unresolved !=
        static_cast<std::int64_t>(validation.settlements.size()))
      throw std::runtime_error("unresolved authoring inventory is not held");
    Exec(database.get(), "COMMIT");
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Ok(
        std::move(validation));
  } catch (const std::exception &) {
    return core::Result<core::CellAdmissionReconcileValidationDTO>::Err(
        core::ErrorDTO{
            "invariant_violation",
            "Composer authoring owner reconciliation is not proven",
            {{"reason", "authoring_owner_reconciliation_evidence_invalid"}}});
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
    if (!metadata.Step() || metadata.Integer(0) != 3 || metadata.Step())
      throw std::runtime_error("Program-cycle journal schema differs");
    Statement row(database.get(),
                  "SELECT state,owner_epoch,claim_json,release_intent_json,"
                  "terminal_reference,"
                  "reconciliation_required,acquisition_pending_json,"
                  "reconciliation_evidence_digest "
                  "FROM program_cycle WHERE singleton=1");
    if (!row.Step() || row.Text(1) != replacement.predecessor.value() ||
        row.Integer(5) != 1 || !row.IsNull(6) || row.IsNull(7) ||
        row.Text(7) != reconcile.reconciliation_evidence_digest.value())
      throw std::runtime_error(
          "Program-cycle reconciliation intent is not exact");

    core::CellAdmissionReconcileValidationDTO validation{
        reconcile.current_owner_epochs, {}};
    const auto state = row.Text(0);
    if (state == "idle_after_reset") {
      if (!row.IsNull(2) || !row.IsNull(3) || !row.IsNull(4))
        throw std::runtime_error("idle Program-cycle retains a claim");
    } else if (state == "terminal_awaiting_reset") {
      if (row.IsNull(2) || row.IsNull(3))
        throw std::runtime_error("terminal Program-cycle evidence is absent");
      const auto claim = DecodeCellAdmissionClaim(row.Text(2));
      const auto release = DecodeCellAdmissionClaimRelease(row.Text(3));
      if (claim.intent.owner_epoch != replacement.predecessor ||
          !ExactReleaseIdentity(claim, release) ||
          claim.intent.claim_kind !=
              core::CellAdmissionClaimKindDTO::kProgramCycle ||
          claim.intent.owner_role !=
              core::CellAdmissionOwnerRoleDTO::kProgramRunner ||
          release.settlement.settlement_kind !=
              core::CellAdmissionSettlementKindDTO::kExplicitReset ||
          row.IsNull(4) ||
          row.Text(4) != release.settlement.evidence_digest.value())
        throw std::runtime_error("terminal Program-cycle evidence differs");
      if (std::find(held_claims.begin(), held_claims.end(), claim) !=
          held_claims.end())
        throw std::runtime_error("released Program-cycle remains held");
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

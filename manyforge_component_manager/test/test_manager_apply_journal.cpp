

#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include <unistd.h>

#include <barrier>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "manyforge_component_manager/manager_apply_journal.hpp"
#include <manyforge_core/runtime_contract_manifest_codec.hpp>

namespace manyforge::component_manager {
namespace {

using core::ActiveBundlePublicationGeneration;
using core::ApplyConfigurationFenceIntentDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::CellId;
using core::ConfigurationFenceAcceptedDTO;
using core::ConfigurationFenceGeneration;
using core::ConfigurationFenceId;
using core::ConfigurationFenceScopeDTO;
using core::ContentDigest;
using core::ControlBindingId;
using core::ControlBindingRevision;
using core::ControlDomainId;
using core::ControlDomainRevision;
using core::ControllableComponentId;
using core::EndpointRoleId;
using core::GraphRevision;
using core::ProductContractId;
using core::ProductContractRevision;
using core::ProductId;
using core::ProjectionHealthDTO;
using core::ProjectionHealthState;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::PublicRequestContextDTO;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::ResourceId;
using core::RetainedBundleId;
using core::RuntimeEpoch;
using core::RuntimeProfileId;
using core::RuntimeProfileRevision;
using core::RuntimeRoleId;
using core::RuntimeUnitId;
using core::SceneId;
using core::SceneRevision;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-apply-journal-test-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneIdOne =
    "sha256:abababababababababababababababababababababababababababababababab";
constexpr std::string_view kSceneIdTwo =
    "sha256:cdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcdcd";

ManagerApplyIntentDTO Intent(std::string apply_id = "apply-1",
                             const std::uint64_t generation = 1U,
                             std::string request_id = "request-1",
                             std::string owner_epoch = "owner-epoch-1",
                             std::string candidate = "candidate-bundle-1") {
  return {
      PublicRequestContextDTO{
          RequestId(std::move(request_id)),
          RequestOwnerEpoch(std::move(owner_epoch)),
          "component-manager-test",
          std::optional<std::string>("correlation-1"),
      },
      ApplyId(std::move(apply_id)),
      ApplyGeneration(generation),
      CellId("cell-1"),
      ProjectionRevisionDTO{
          RetainedBundleId("predecessor-bundle"),
          GraphRevision("predecessor-graph"),
          SceneRevision{
              SceneId(std::string(kSceneIdOne)),
              7U,
              ContentDigest(std::string(kDigestOne)),
          },
      },
      RetainedBundleId(std::move(candidate)),
      GraphRevision("candidate-graph"),
      ManagerApplyPreparedActivationDTO{
          ContentDigest("sha256:1414141414141414141414141414141414141414141414141414141414141414"),
          core::DeploymentArtifactId("deployment-artifact-1"),
          core::DeploymentRevision(
              "sha256:1515151515151515151515151515151515151515151515151515151515151515"),
          core::ProgramBindingId(kDigestTwo),
          ContentDigest("sha256:1616161616161616161616161616161616161616161616161616161616161616"),
          R"({"schema_version":"test.prepared_activation_receipt.v1"})",
      },
      core::AdmissionGeneration(7U),
      std::nullopt,
  };
}

ManagerApplyIntentDTO ParentClaimIntent() {
  auto intent = Intent();
  intent.expected_admission_generation.reset();
  intent.parent_authoring_claim = core::CellAdmissionClaimDTO{
      {
          core::CellAdmissionClaimKindDTO::kAuthoringMutation,
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("composer-owner-1"),
          "composer-transaction-1",
          RequestId("composer-request-1"),
          ContentDigest(std::string(kDigestTwo)),
          core::AdmissionGeneration(7U),
          {},
          std::nullopt,
          std::nullopt,
      },
      core::CellAdmissionClaimGeneration(3U),
      core::AdmissionGeneration(7U),
      42,
  };
  return intent;
}

ApplyConfigurationFenceIntentDTO FenceIntent() {
  return {
      Intent().request_context,
      core::ApplyFenceOwnerDTO{ApplyId("apply-1"), ApplyGeneration(1U)},
      ConfigurationFenceScopeDTO{
          {ControllableComponentId("robot-1")},
          {{ControlDomainId("arm"), ControlDomainRevision("domain-r1"),
            ControlDomainRevision("domain-r2")}},
          {{ControlBindingId("arm-jtc"), ControlBindingRevision("binding-r1"),
            ControlBindingRevision("binding-r2")}},
          {ResourceId("joint-1"), ResourceId("joint-2")},
      },
      core::ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      RetainedBundleId("predecessor-bundle"),
      GraphRevision("predecessor-graph"),
      11U,
      ConfigurationFenceGeneration(4U),
      {{ControlDomainId("arm"),
        core::ActiveControlConfigurationId("trajectory-mode"),
        core::ActiveControlConfigurationGeneration(3U)}},
      "apply configuration transition",
  };
}

std::string FenceIdentifier(const char prefix, const std::size_t index) {
  auto ordinal = std::to_string(index);
  std::string value(1U, prefix);
  value.append(6U - ordinal.size(), '0');
  value.append(ordinal);
  value.append(128U - value.size(), 'x');
  return value;
}

ApplyConfigurationFenceIntentDTO MaximumFenceIntent() {
  auto intent = FenceIntent();
  intent.scope = {};
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceComponents;
       ++index) {
    intent.scope.controllable_component_ids.emplace_back(
        FenceIdentifier('c', index));
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceDomains;
       ++index) {
    intent.scope.domains.push_back({
        ControlDomainId(FenceIdentifier('d', index)),
        index % 2U == 0U ? std::optional<ControlDomainRevision>(
                               ControlDomainRevision(std::string(kDigestOne)))
                         : std::nullopt,
        index % 2U == 1U ? std::optional<ControlDomainRevision>(
                               ControlDomainRevision(std::string(kDigestTwo)))
                         : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceBindings;
       ++index) {
    intent.scope.bindings.push_back({
        ControlBindingId(FenceIdentifier('b', index)),
        index % 2U == 0U ? std::optional<ControlBindingRevision>(
                               ControlBindingRevision(std::string(kDigestOne)))
                         : std::nullopt,
        index % 2U == 1U ? std::optional<ControlBindingRevision>(
                               ControlBindingRevision(std::string(kDigestTwo)))
                         : std::nullopt,
    });
  }
  for (std::size_t index = 0U; index < kMaximumManagerApplyFenceResources;
       ++index) {
    intent.scope.resources.emplace_back(FenceIdentifier('r', index));
  }
  intent.expected_active_control_configurations.clear();
  for (std::size_t index = 0U; index < intent.scope.domains.size();
       index += 2U) {
    intent.expected_active_control_configurations.push_back(
        {intent.scope.domains[index].control_domain_id,
         core::ActiveControlConfigurationId(FenceIdentifier('a', index)),
         core::ActiveControlConfigurationGeneration(3U)});
  }
  return intent;
}

ManagerApplyAffectedScopeDTO AffectedScope(
    const ApplyConfigurationFenceIntentDTO &fence_intent = FenceIntent()) {
  return {
      fence_intent.owner_transaction.apply_id,
      fence_intent.owner_transaction.apply_generation,
      fence_intent.scope,
      fence_intent.busy_disposition,
  };
}

ManagerApplyFenceAttemptWriteResult EnsureAttempt(
    SqliteManagerApplyJournal &journal, const ContentDigest &intent_digest,
    const ApplyConfigurationFenceIntentDTO &fence_intent = FenceIntent()) {
  const auto affected =
      journal.EnsureAffectedScope(intent_digest, AffectedScope(fence_intent));
  return journal.EnsureFenceAttempt(
      intent_digest, affected.record.affected_scope_digest, fence_intent);
}

ConfigurationFenceAcceptedDTO
AcceptedFence(core::RequestReplayStatusDTO replay_status =
                  core::RequestReplayStatusDTO::kAcceptedNew) {
  return {
      ConfigurationFenceId("fence-1"),
      ConfigurationFenceGeneration(5U),
      FenceIntent().scope,
      123456789LL,
      replay_status,
  };
}

RuntimeUnitConvergencePlanRowDTO RuntimePlanRow() {
  const auto requirement = core::MakeRuntimeContractRequirement(
      core::RuntimeImplementationFamilyId("runtime-family"), {}, {});
  if (!requirement.ok()) {
    throw std::runtime_error("test runtime requirement is invalid");
  }
  return {
      RuntimeUnitId("runtime-1"),
      RuntimeProfileId("profile-1"),
      RuntimeProfileRevision("profile-r1"),
      core::RuntimeLifecycleControl::kManagerManaged,
      RuntimeUnitCandidateDisposition::kAdded,
      ProductId("runtime-product-1"),
      ProductContractId("manyforge.reference-runtime-product.v1"),
      ProductContractRevision("1"),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestTwo)),
      {},
      {RuntimeRoleId("runtime.role.reference")},
      EndpointRoleId("runtime.status"),
      "manyforge.reference-runtime-status.v2",
      "1",
      "stdout_line",
      "manyforge.process-adapter.posix-shell.v1",
      "1",
      "manyforge.retained-artifact-retrieval.v1",
      "1",
      ContentDigest(std::string(kDigestOne)),
      128U,
      "linux",
      "posix-sh",
      requirement.value(),
      core::RuntimeContractOfferDigest(std::string(kDigestOne)),
  };
}

ManagerApplyRuntimeConvergenceIntentDTO
RuntimeIntent(const ContentDigest &acceptance_digest,
              const bool empty = false) {
  return {
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      RetainedBundleId("candidate-bundle-1"),
      GraphRevision("candidate-graph"),
      acceptance_digest,
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestOne)),
      empty ? std::vector<RuntimeUnitConvergencePlanRowDTO>{}
            : std::vector<RuntimeUnitConvergencePlanRowDTO>{RuntimePlanRow()},
  };
}

ManagerApplyRuntimeStagedReadyDTO
RuntimeStagedReady(const ContentDigest &convergence_digest,
                   const std::uint64_t generation, const bool empty = false) {
  std::vector<RuntimeUnitStagedReadyEvidenceDTO> runtime_units;
  if (!empty) {
    runtime_units.push_back({
        RuntimeUnitId("runtime-1"),
        core::RuntimeLifecycleControl::kManagerManaged,
        RuntimeUnitCandidateDisposition::kAdded,
        RuntimeEpoch("runtime-epoch-1"),
        "observer-1",
        generation,
        generation,
        generation,
        ContentDigest(std::string(kDigestOne)),
        ContentDigest(std::string(kDigestTwo)),
        RuntimeSourcePhase::kServing,
        RuntimeSourceHealth::kHealthy,
        {RuntimeRoleId("runtime.role.reference")},
        ManagedCommandSurfaceState::kClosedNoActiveEffect,
        {},
        generation,
        generation + 1U,
        generation + 2U,
        core::RuntimeContractOfferDigest(std::string(kDigestOne)),
    });
  }
  return {
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      generation,
      convergence_digest,
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestOne)),
      100U + generation,
      std::move(runtime_units),
  };
}

ProjectionRevisionDTO CandidateProjection() {
  return {
      RetainedBundleId("candidate-bundle-1"),
      GraphRevision("candidate-graph"),
      SceneRevision{
          SceneId(std::string(kSceneIdTwo)),
          8U,
          ContentDigest(std::string(kDigestTwo)),
      },
  };
}

ManagerApplyPrivateMaterializationDTO
PrivateMaterialization() {
  const auto candidate = CandidateProjection();
  return {
      candidate.scene_revision.content_hash,
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      candidate,
      ContentDigest(std::string(kDigestTwo)),
      512U,
  };
}
ManagerApplyPublicationReceiptDTO PublicationReceipt(
    const PersistedManagerApplyPrivateMaterialization &materialization) {
  const auto &record = materialization.private_materialization;
  core::ActiveBundlePublicationRequestDTO request{
      core::NamedApplyPublicationTransactionDTO{
          record.apply_id,
          record.apply_generation,
      },
      record.candidate_projection_revision,
      record.candidate_projection_revision.retained_bundle_id,
      record.candidate_projection_revision.graph_revision,
      ActiveBundlePublicationGeneration(3U),
      materialization.private_materialization_content_digest,
  };
  return {
      materialization.private_materialization_content_digest,
      core::ActiveBundlePublicationReceiptDTO{
          std::move(request),
          ActiveBundlePublicationGeneration(4U),
          ContentDigest(std::string(kDigestOne)),
      },
  };
}

struct SelectedBundleTransitionRecords final {
  PersistedManagerApplyPrivateMaterialization materialization;
  PersistedManagerApplyPublicationReceipt publication;
};

ManagerApplyFenceReleaseIntentDTO
FenceReleaseIntent(const SelectedBundleTransitionRecords &records,
                   const PersistedManagerApplyFenceAcceptance &acceptance) {
  const auto &candidate =
      records.materialization.private_materialization.candidate_projection_revision;
  core::ConfigurationFenceReconciliationEvidenceDTO evidence{
      core::ExecutionHostEpochDTO{
          RuntimeUnitId("kernel-runtime"),
          RuntimeEpoch("kernel-epoch-1"),
          RequestOwnerEpoch("kernel-owner-epoch-1"),
          ContentDigest(std::string(kDigestTwo)),
          ActiveBundlePublicationGeneration(4U),
      },
      core::ApplyFenceOwnerDTO{ApplyId("apply-1"), ApplyGeneration(1U)},
      acceptance.accepted_fence.configuration_fence_id,
      acceptance.accepted_fence.configuration_fence_generation,
      acceptance.accepted_fence.captured_scope,
      12U,
      candidate.graph_revision,
      candidate,
      records.publication.publication.publication_receipt
          .publication_generation,
      records.publication.publication.publication_receipt
          .durable_public_pointer_content_digest,
      {{ControlDomainId("arm"),
        core::ActiveControlConfigurationId("trajectory-mode-v2"),
        core::ActiveControlConfigurationGeneration(4U)}},
      0U,
      0U,
      ContentDigest(std::string(kDigestOne)),
  };
  return {
      records.publication.publication_receipt_digest,
      acceptance.acceptance_digest,
      core::ConfigurationFenceReleaseDTO{std::move(evidence),
                                         RuntimeEpoch("manager-epoch-1")},
  };
}

ManagerApplyFenceReleasedDTO
FenceReleased(const PersistedManagerApplyFenceReleaseIntent &intent) {
  const auto &request = intent.fence_release_intent.release_request;
  return {
      intent.fence_release_intent_digest,
      core::ConfigurationFenceReleasedDTO{
          request,
          request.reconciliation_evidence.execution_host_epoch,
          987654321LL,
          core::RequestReplayStatusDTO::kAcceptedNew,
          ContentDigest(std::string(kDigestTwo)),
      },
  };
}

ManagerApplyTerminalOutcomeDTO
TerminalOutcome(const SelectedBundleTransitionRecords &records,
                const PersistedManagerApplyFenceReleased &released) {
  const auto &result = records.materialization.private_materialization;
  return {
      result.apply_id,
      result.apply_generation,
      result.candidate_projection_revision,
      records.publication.publication_receipt_digest,
      records.publication.publication_receipt_digest,
      released.fence_released_digest,
      ManagerApplyTerminalOutcomeState::kSucceeded,
  };
}

struct ApplyCompletionRecords final {
  PersistedManagerApplyFenceReleaseIntent release_intent;
  PersistedManagerApplyFenceReleased released;
  PersistedManagerApplyTerminalOutcome terminal;
};

ApplyCompletionRecords
EnsureApplyCompletion(SqliteManagerApplyJournal &journal,
                      const SelectedBundleTransitionRecords &records) {
  const auto acceptance =
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  if (!acceptance.has_value()) {
    throw std::runtime_error("test fixture lacks fence acceptance");
  }
  const auto release_intent = journal.EnsureFenceReleaseIntent(
      FenceReleaseIntent(records, *acceptance));
  const auto released =
      journal.EnsureFenceReleased(FenceReleased(release_intent.record));
  const auto terminal =
      journal.EnsureTerminalOutcome(TerminalOutcome(records, released.record));
  return {release_intent.record, released.record, terminal.record};
}

struct AcceptedApplyRecords final {
  PersistedManagerApplyIntent intent;
  PersistedManagerApplyFenceAcceptance acceptance;
};

AcceptedApplyRecords EnsureAcceptedApply(SqliteManagerApplyJournal &journal) {
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
  const auto acceptance = journal.EnsureFenceAcceptance(
      ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
      AcceptedFence());
  return {intent, acceptance.record};
}

struct SceneParentRecords final {
  PersistedManagerApplyRuntimeStagedReady staged_ready;
};

SceneParentRecords EnsureSceneParents(SqliteManagerApplyJournal &journal,
                                      const bool empty_runtime = false) {
  const auto accepted = EnsureAcceptedApply(journal);
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      accepted.intent.intent_digest,
      RuntimeIntent(accepted.acceptance.acceptance_digest, empty_runtime));
  const auto staged_ready = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
      runtime_intent.record.runtime_convergence_intent_digest, 1U,
      empty_runtime));
  return {staged_ready.record};
}

SelectedBundleTransitionRecords
EnsureSelectedBundleTransition(SqliteManagerApplyJournal &journal) {
  static_cast<void>(EnsureSceneParents(journal, true));
  const auto materialization =
      journal.EnsurePrivateMaterialization(PrivateMaterialization());
  const auto publication = journal.EnsurePublicationReceipt(
      PublicationReceipt(materialization.record));
  return {materialization.record, publication.record};
}

bool SameFenceIntent(const ApplyConfigurationFenceIntentDTO &lhs,
                     const ApplyConfigurationFenceIntentDTO &rhs) {
  return lhs.owner_request_context == rhs.owner_request_context &&
         lhs.owner_transaction == rhs.owner_transaction &&
         lhs.scope == rhs.scope &&
         lhs.busy_disposition == rhs.busy_disposition &&
         lhs.expected_retained_bundle_id == rhs.expected_retained_bundle_id &&
         lhs.expected_graph_revision == rhs.expected_graph_revision &&
         lhs.expected_operation_resource_generation ==
             rhs.expected_operation_resource_generation &&
         lhs.expected_fence_generation == rhs.expected_fence_generation &&
         lhs.expected_active_control_configurations ==
             rhs.expected_active_control_configurations &&
         lhs.reason == rhs.reason;
}

bool SameAcceptedFence(const ConfigurationFenceAcceptedDTO &lhs,
                       const ConfigurationFenceAcceptedDTO &rhs) {
  return lhs.configuration_fence_id == rhs.configuration_fence_id &&
         lhs.configuration_fence_generation ==
             rhs.configuration_fence_generation &&
         lhs.captured_scope == rhs.captured_scope &&
         lhs.accepted_at_ns == rhs.accepted_at_ns &&
         lhs.replay_status == rhs.replay_status;
}

void ExecSql(const std::filesystem::path &path, const std::string &sql) {
  sqlite3 *database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(path.string().c_str(), &database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr),
            SQLITE_OK);
  char *error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail =
      error == nullptr ? std::string{} : std::string(error);
  sqlite3_free(error);
  EXPECT_EQ(result, SQLITE_OK) << detail;
  EXPECT_EQ(sqlite3_close(database), SQLITE_OK);
}

std::string ReadFileBytes(const std::filesystem::path &path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open test file");
  }
  return {
      std::istreambuf_iterator<char>(input),
      std::istreambuf_iterator<char>(),
  };
}

void ExpectNoSqliteSidecars(const std::filesystem::path &path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

TEST(ManagerApplyJournalIntegrity,
     RejectsSchemaEmptyDatabaseWithForeignSqliteIdentity) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  ExecSql(path, "PRAGMA application_id=424242;"
                "PRAGMA user_version=77;");
  const auto before = ReadFileBytes(path);
  ExpectNoSqliteSidecars(path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsForeignSchemaAndCellWithoutChangingAtRestSource) {
  TemporaryDirectory directory;
  const auto schema_path = directory.path() / "foreign-schema.sqlite3";
  ExecSql(schema_path, "CREATE TABLE foreign_table(value TEXT);");
  const auto schema_before = ReadFileBytes(schema_path);
  EXPECT_THROW(SqliteManagerApplyJournal(schema_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(schema_path), schema_before);
  ExpectNoSqliteSidecars(schema_path);

  const auto cell_path = directory.path() / "foreign-cell.sqlite3";
  { SqliteManagerApplyJournal owner(cell_path, CellId("foreign-cell")); }
  ExpectNoSqliteSidecars(cell_path);
  const auto cell_before = ReadFileBytes(cell_path);
  EXPECT_THROW(SqliteManagerApplyJournal(cell_path, CellId("cell-1")),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(ReadFileBytes(cell_path), cell_before);
  ExpectNoSqliteSidecars(cell_path);
}

TEST(ManagerApplyJournalIntegrity, RejectsPreUa4JournalVersion) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "pre-ua4.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  ExecSql(path, "PRAGMA user_version=12;");
  const auto before = ReadFileBytes(path);
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), before);
  ExpectNoSqliteSidecars(path);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsOrphanShmWithoutWalWithoutChangingEitherFile) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "orphan-shm.sqlite3";
  {
    std::ofstream main(path, std::ios::binary);
    ASSERT_TRUE(main);
  }
  const auto shm_path = std::filesystem::path(path.string() + "-shm");
  {
    std::ofstream shm(shm_path, std::ios::binary);
    ASSERT_TRUE(shm);
    shm << "orphan-shm";
  }
  const auto database_before = ReadFileBytes(path);
  const auto shm_before = ReadFileBytes(shm_path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(shm_path), shm_before);
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
}

TEST(ManagerApplyJournalIntegrity,
     RejectsForeignCellFromWalCrashResidueWithoutChangingSource) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "wal-residue.sqlite3";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      auto *owner = new SqliteManagerApplyJournal(path, CellId("foreign-cell"));
      static_cast<void>(owner);
      _exit(0);
    } catch (...) {
      _exit(1);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);
  const auto wal_path = std::filesystem::path(path.string() + "-wal");
  const auto shm_path = std::filesystem::path(path.string() + "-shm");
  ASSERT_TRUE(std::filesystem::exists(wal_path));
  std::filesystem::remove(shm_path);
  const auto database_before = ReadFileBytes(path);
  const auto wal_before = ReadFileBytes(wal_path);

  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(ReadFileBytes(path), database_before);
  EXPECT_EQ(ReadFileBytes(wal_path), wal_before);
  EXPECT_FALSE(std::filesystem::exists(shm_path));

  EXPECT_NO_THROW({
    SqliteManagerApplyJournal recovered(path, CellId("foreign-cell"));
    EXPECT_EQ(recovered.cell_id(), CellId("foreign-cell"));
  });
}

TEST(ManagerApplyIntent, ValidityRejectsMissingRequiredIdentity) {
  auto intent = Intent();
  EXPECT_TRUE(intent.valid());

  intent.request_context.request_id = RequestId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.request_owner_epoch = RequestOwnerEpoch("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.source_label.clear();
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.request_context.correlation_id = "";
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.apply_generation = ApplyGeneration(0U);
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.expected_active_projection_revision.scene_revision.revision_id = 0U;
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.candidate_retained_bundle_id = RetainedBundleId("");
  EXPECT_FALSE(intent.valid());

  // The exact UA2A prepared activation is part of the operation identity: an
  // intent without it cannot name the input it accepted.
  intent = Intent();
  intent.prepared_activation.captured_input_digest = ContentDigest("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.deployment_artifact_id =
      core::DeploymentArtifactId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.deployment_revision =
      core::DeploymentRevision("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.program_binding_id = core::ProgramBindingId("");
  EXPECT_FALSE(intent.valid());
  intent = Intent();
  intent.prepared_activation.prepared_activation_id = ContentDigest("");
  EXPECT_FALSE(intent.valid());
}

TEST(ManagerApplyJournal,
     DerivesOneBoundedOperationStatusFromDurableMilestones) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "status.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // An unknown durable identity is not an operation.
  EXPECT_FALSE(
      journal.ReadStatus(ApplyId("apply-unknown"), ApplyGeneration(1U))
          .has_value());

  const auto accepted =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(accepted->intent.intent_digest, intent.intent_digest);
  EXPECT_FALSE(accepted->terminal_outcome.has_value());
  const auto accepted_progress = DeriveManagerApplyProgress(*accepted);
  EXPECT_EQ(accepted_progress.phase, ManagerApplyPhase::kAccepted);
  EXPECT_EQ(accepted_progress.record_digest, intent.intent_digest);
  EXPECT_EQ(std::string(ToString(accepted_progress.phase)), "accepted");

  // The phase advances only with the exact parent-linked durable record; a
  // heartbeat or any elapsed time changes nothing.
  const auto scope =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()).record;
  const auto fenced =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(fenced.has_value());
  const auto fenced_progress = DeriveManagerApplyProgress(*fenced);
  EXPECT_EQ(fenced_progress.phase, ManagerApplyPhase::kFenceAcquired);
  EXPECT_EQ(fenced_progress.record_digest, scope.affected_scope_digest);
  EXPECT_EQ(std::string(ToString(fenced_progress.phase)), "fence_acquired");
}

TEST(ManagerApplyTerminalOutcome, RequiresExactlyOneCompleteDisposition) {
  const auto projection = ProjectionRevisionDTO{
      RetainedBundleId("bundle.candidate"),
      GraphRevision("graph.candidate"),
      SceneRevision{
          SceneId(std::string(kSceneIdOne)),
          3U,
          ContentDigest(std::string(kDigestOne)),
      },
  };
  const auto failure = ManagerApplyFailureEvidenceDTO{
      ContentDigest(std::string(kDigestThree)),
      ManagerApplyPhase::kRuntimeConverged,
      "runtime_initialization_failed",
      "replacement initialization failed after teardown",
      ManagerApplyEffectDisposition::kKnown,
  };
  EXPECT_TRUE(failure.valid());

  const ManagerApplyTerminalOutcomeDTO full_success{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      projection,
      ContentDigest(std::string(kDigestTwo)),
      ContentDigest(std::string(kDigestThree)),
      ContentDigest(std::string(kDigestOne)),
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  };
  EXPECT_TRUE(full_success.valid());
  EXPECT_TRUE(full_success.succeeded());

  // A success carries the complete publication evidence and no failure.
  auto partial = full_success;
  partial.fence_released_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.scene_completion_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.publication_receipt_digest.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.candidate_projection_revision.reset();
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.publication_receipt_digest = ContentDigest(std::string());
  EXPECT_FALSE(partial.valid());
  partial = full_success;
  partial.failure = failure;
  EXPECT_FALSE(partial.valid());

  const ManagerApplyTerminalOutcomeDTO failed{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      failure,
  };
  EXPECT_TRUE(failed.valid());
  EXPECT_FALSE(failed.succeeded());

  // A failure carries no publication evidence at all: one stray success field
  // is rejected even though the complete success set is obviously absent.
  auto stray = failed;
  stray.fence_released_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.scene_completion_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.publication_receipt_digest = ContentDigest(std::string(kDigestOne));
  EXPECT_FALSE(stray.valid());
  stray = failed;
  stray.candidate_projection_revision = projection;
  EXPECT_FALSE(stray.valid());

  // A known failure and an unknown effect are distinct dispositions.
  auto mismatched = failed;
  mismatched.failure->effect_disposition =
      ManagerApplyEffectDisposition::kUnknown;
  EXPECT_FALSE(mismatched.valid());
  auto uncertain = failed;
  uncertain.state = ManagerApplyTerminalOutcomeState::kUnknownEffect;
  EXPECT_FALSE(uncertain.valid());
  uncertain.failure->effect_disposition =
      ManagerApplyEffectDisposition::kUnknown;
  EXPECT_TRUE(uncertain.valid());
  EXPECT_FALSE(uncertain.succeeded());
  uncertain.state = ManagerApplyTerminalOutcomeState::kSucceeded;
  EXPECT_FALSE(uncertain.valid());

  // A terminal failure retains exactly one valid, bounded, intent-bound failure.
  auto missing = failed;
  missing.failure.reset();
  EXPECT_FALSE(missing.valid());
  auto unbound = failed;
  unbound.failure->intent_digest = ContentDigest(std::string());
  EXPECT_FALSE(unbound.valid());
  auto empty_code = failed;
  empty_code.failure->failure_code.clear();
  EXPECT_FALSE(empty_code.valid());
  auto empty_message = failed;
  empty_message.failure->failure_message.clear();
  EXPECT_FALSE(empty_message.valid());
  auto oversized_code = failed;
  oversized_code.failure->failure_code =
      std::string(kMaximumManagerApplyFailureCodeBytes + 1U, 'c');
  EXPECT_FALSE(oversized_code.valid());
  auto oversized_message = failed;
  oversized_message.failure->failure_message =
      std::string(kMaximumManagerApplyFailureMessageBytes + 1U, 'm');
  EXPECT_FALSE(oversized_message.valid());

  // The persisted vocabulary never spells an unknown-effect failure as a
  // success or as clean absence.
  EXPECT_EQ(std::string(ToString(ManagerApplyEffectDisposition::kKnown)),
            "known");
  EXPECT_EQ(std::string(ToString(ManagerApplyEffectDisposition::kUnknown)),
            "unknown");
  EXPECT_EQ(std::string(ToString(ManagerApplyTerminalOutcomeState::kFailed)),
            "failed");
  EXPECT_EQ(
      std::string(ToString(ManagerApplyTerminalOutcomeState::kUnknownEffect)),
      "unknown_effect");
}

TEST(ManagerApplyJournal, RetainsOneImmutableTerminalFailurePerOperation) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "terminal-failure.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // Initialization failed after destructive teardown, so the effects are not
  // established and the operation must not claim clean absence.
  const auto failure = ManagerApplyFailureEvidenceDTO{
      intent.intent_digest,
      ManagerApplyPhase::kRuntimeConverged,
      "runtime_initialization_failed",
      "replacement initialization failed after teardown",
      ManagerApplyEffectDisposition::kUnknown,
  };
  const auto retained = journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kUnknownEffect,
      failure,
  });
  EXPECT_TRUE(retained.inserted);

  // Exact replay is a read of the same immutable terminal outcome.
  const auto replay = journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kUnknownEffect,
      failure,
  });
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record, retained.record);

  // A different failure for the same operation identity is rejected: the
  // terminal result is retained exactly once and is never rewritten.
  auto conflicting = failure;
  conflicting.failure_message = "another failure";
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kUnknownEffect,
                   conflicting,
               }),
               ManagerApplyJournalConflictError);

  const auto status =
      journal.ReadStatus(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(status.has_value());
  ASSERT_TRUE(status->terminal_outcome.has_value());
  const auto &terminal = status->terminal_outcome->terminal_outcome;
  EXPECT_FALSE(terminal.succeeded());
  EXPECT_EQ(terminal.state, ManagerApplyTerminalOutcomeState::kUnknownEffect);
  ASSERT_TRUE(terminal.failure.has_value());
  EXPECT_EQ(terminal.failure->failure_code, "runtime_initialization_failed");
  EXPECT_EQ(terminal.failure->effect_disposition,
            ManagerApplyEffectDisposition::kUnknown);
  EXPECT_EQ(terminal.failure->intent_digest, intent.intent_digest);
  EXPECT_FALSE(terminal.candidate_projection_revision.has_value());
  // The milestone projection is unaffected: a terminal failure is not progress.
  EXPECT_EQ(DeriveManagerApplyProgress(*status).phase,
            ManagerApplyPhase::kAccepted);

  journal.Close();

  // The retained first failure survives a Manager restart exactly.
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto stored = reopened.ReadTerminalOutcome(ApplyId("apply-1"),
                                                   ApplyGeneration(1U));
  ASSERT_TRUE(stored.has_value());
  EXPECT_EQ(stored->terminal_outcome, retained.record.terminal_outcome);
  EXPECT_EQ(stored->terminal_outcome_digest,
            retained.record.terminal_outcome_digest);
  EXPECT_EQ(reopened.VerifyIntegrity().terminal_outcome_count, 1);
}

TEST(ManagerApplyJournal, RejectsATerminalFailureThatIsNotBoundToItsIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "failure-binding.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;

  // An unknown operation is not a durable identity.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-unknown"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                   },
               }),
               ManagerApplyJournalConflictError);

  // A failure must name the exact accepted intent of the same generation.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       ContentDigest(std::string(kDigestThree)),
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                   },
               }),
               ManagerApplyJournalConflictError);

  // A mismatched state/disposition pair is rejected before any write.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "preflight_refused",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kUnknown,
                   },
               }),
               std::invalid_argument);

  // Empty and oversized retained failure text is rejected before any write.
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       "",
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                   },
               }),
               std::invalid_argument);
  EXPECT_THROW(journal.EnsureTerminalOutcome({
                   ApplyId("apply-1"),
                   ApplyGeneration(1U),
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   std::nullopt,
                   ManagerApplyTerminalOutcomeState::kFailed,
                   ManagerApplyFailureEvidenceDTO{
                       intent.intent_digest,
                       ManagerApplyPhase::kAccepted,
                       std::string(kMaximumManagerApplyFailureCodeBytes + 1U, 'c'),
                       "candidate validation failed before any effect",
                       ManagerApplyEffectDisposition::kKnown,
                   },
               }),
               std::invalid_argument);

  // None of the refusals left a terminal record behind.
  EXPECT_FALSE(journal.ReadTerminalOutcome(ApplyId("apply-1"),
                                           ApplyGeneration(1U))
                   .has_value());
}

TEST(ManagerApplyJournalValues,
     EnforcesEveryTextBoundaryAndSignedCounterBoundary) {
  TemporaryDirectory directory;
  using Setter =
      std::function<void(ManagerApplyIntentDTO &, const std::string &)>;
  const std::vector<std::pair<std::string, Setter>> identities{
      {"request_id",
       [](auto &value, const auto &text) {
         value.request_context.request_id = RequestId(text);
       }},
      {"request_owner_epoch",
       [](auto &value, const auto &text) {
         value.request_context.request_owner_epoch = RequestOwnerEpoch(text);
       }},
      {"apply_id",
       [](auto &value, const auto &text) { value.apply_id = ApplyId(text); }},
      {"predecessor_bundle",
       [](auto &value, const auto &text) {
         value.expected_active_projection_revision.retained_bundle_id =
             RetainedBundleId(text);
       }},
      {"predecessor_graph",
       [](auto &value, const auto &text) {
         value.expected_active_projection_revision.graph_revision =
             GraphRevision(text);
       }},
      {"candidate_bundle",
       [](auto &value, const auto &text) {
         value.candidate_retained_bundle_id = RetainedBundleId(text);
       }},
      {"candidate_graph",
       [](auto &value, const auto &text) {
         value.candidate_graph_revision = GraphRevision(text);
       }},
  };

  for (const auto &[label, set_value] : identities) {
    SCOPED_TRACE(label);
    auto exact = Intent();
    set_value(exact,
              std::string(kMaximumManagerApplyJournalIdentifierBytes, 'a'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-exact.sqlite3"), CellId("cell-1"));
      EXPECT_TRUE(journal.EnsureIntent(exact).inserted);
    }

    auto excessive = Intent();
    set_value(
        excessive,
        std::string(kMaximumManagerApplyJournalIdentifierBytes + 1U, 'a'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-excessive.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(excessive), std::invalid_argument);
    }

    auto control = Intent();
    set_value(control, std::string("a\0b", 3U));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-control.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(control), std::invalid_argument);
    }
  }

  {
    auto exact_scene = Intent();
    exact_scene.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(std::string(kSceneIdTwo));
    SqliteManagerApplyJournal journal(
        directory.path() / "predecessor-scene-exact.sqlite3", CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(exact_scene).inserted);
  }
  for (const auto &invalid_scene : {
           std::string(kMaximumManagerApplyJournalIdentifierBytes, 'a'),
           std::string("sha256:a\0b", 10U),
       }) {
    auto intent = Intent();
    intent.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(invalid_scene);
    SqliteManagerApplyJournal journal(
        directory.path() / (invalid_scene.find('\0') == std::string::npos
                                ? "predecessor-scene-malformed.sqlite3"
                                : "predecessor-scene-control.sqlite3"),
        CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(intent), std::invalid_argument);
  }

  const auto maximum_identity =
      std::string(kMaximumManagerApplyJournalIdentifierBytes, 'c');
  EXPECT_NO_THROW({
    SqliteManagerApplyJournal journal(directory.path() / "cell-exact.sqlite3",
                                      CellId(maximum_identity));
    auto intent = Intent();
    intent.cell_id = CellId(maximum_identity);
    EXPECT_TRUE(journal.EnsureIntent(intent).inserted);
  });
  EXPECT_THROW(SqliteManagerApplyJournal(
                   directory.path() / "cell-excessive.sqlite3",
                   CellId(std::string(
                       kMaximumManagerApplyJournalIdentifierBytes + 1U, 'c'))),
               std::invalid_argument);
  EXPECT_THROW(
      SqliteManagerApplyJournal(directory.path() / "cell-control.sqlite3",
                                CellId(std::string("c\0d", 3U))),
      std::invalid_argument);

  for (const auto &[label, maximum, set_value] :
       std::vector<std::tuple<std::string, std::size_t, Setter>>{
           {"source_label", kMaximumManagerApplySourceLabelBytes,
            [](auto &value, const auto &text) {
              value.request_context.source_label = text;
            }},
           {"correlation_id", kMaximumManagerApplyCorrelationIdBytes,
            [](auto &value, const auto &text) {
              value.request_context.correlation_id = text;
            }},
       }) {
    SCOPED_TRACE(label);
    auto exact = Intent();
    set_value(exact, std::string(maximum, 't'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-exact.sqlite3"), CellId("cell-1"));
      EXPECT_TRUE(journal.EnsureIntent(exact).inserted);
    }
    auto excessive = Intent();
    set_value(excessive, std::string(maximum + 1U, 't'));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-excessive.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(excessive), std::invalid_argument);
    }
    auto control = Intent();
    set_value(control, std::string("t\nx"));
    {
      SqliteManagerApplyJournal journal(
          directory.path() / (label + "-control.sqlite3"), CellId("cell-1"));
      EXPECT_THROW(journal.EnsureIntent(control), std::invalid_argument);
    }
  }

  auto excessive_generation = Intent();
  excessive_generation.apply_generation = ApplyGeneration(
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) +
      1U);
  {
    SqliteManagerApplyJournal journal(
        directory.path() / "generation-excessive.sqlite3", CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(excessive_generation),
                 std::invalid_argument);
  }
  auto maximum_scene_revision = Intent();
  maximum_scene_revision.expected_active_projection_revision.scene_revision
      .revision_id =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
  {
    SqliteManagerApplyJournal journal(
        directory.path() / "scene-revision-exact.sqlite3", CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(maximum_scene_revision).inserted);
  }
  auto excessive_scene_revision = Intent();
  excessive_scene_revision.expected_active_projection_revision.scene_revision
      .revision_id =
      static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1U;
  {
    SqliteManagerApplyJournal journal(directory.path() /
                                          "scene-revision-excessive.sqlite3",
                                      CellId("cell-1"));
    EXPECT_THROW(journal.EnsureIntent(excessive_scene_revision),
                 std::invalid_argument);
  }
}

TEST(ManagerApplyJournalIntegrity,
     IntentDigestIsSensitiveToEveryPersistedField) {
  TemporaryDirectory directory;
  const auto base = Intent();
  std::vector<ManagerApplyIntentDTO> variants{base};
  const auto add = [&](const auto &mutate) {
    auto value = base;
    mutate(value);
    variants.push_back(std::move(value));
  };
  add([](auto &value) {
    value.request_context.request_id = RequestId("request-2");
  });
  add([](auto &value) {
    value.request_context.request_owner_epoch =
        RequestOwnerEpoch("owner-epoch-2");
  });
  add([](auto &value) {
    value.request_context.source_label = "another-source";
  });
  add([](auto &value) { value.request_context.correlation_id.reset(); });
  add([](auto &value) {
    value.request_context.correlation_id = "correlation-2";
  });
  add([](auto &value) { value.apply_id = ApplyId("apply-2"); });
  add([](auto &value) { value.apply_generation = ApplyGeneration(2U); });
  add([](auto &value) { value.cell_id = CellId("cell-2"); });
  add([](auto &value) {
    value.expected_active_projection_revision.retained_bundle_id =
        RetainedBundleId("predecessor-bundle-2");
  });
  add([](auto &value) {
    value.expected_active_projection_revision.graph_revision =
        GraphRevision("predecessor-graph-2");
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.scene_id =
        SceneId(std::string(kSceneIdTwo));
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.revision_id = 8U;
  });
  add([](auto &value) {
    value.expected_active_projection_revision.scene_revision.content_hash =
        ContentDigest(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.candidate_retained_bundle_id = RetainedBundleId("candidate-bundle-2");
  });
  add([](auto &value) {
    value.candidate_graph_revision = GraphRevision("candidate-graph-2");
  });
  add([](auto &value) {
    value.prepared_activation.captured_input_digest =
        ContentDigest(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.prepared_activation.deployment_artifact_id =
        core::DeploymentArtifactId("deployment-artifact-2");
  });
  add([](auto &value) {
    value.prepared_activation.deployment_revision =
        core::DeploymentRevision(std::string(kDigestTwo));
  });
  add([](auto &value) {
    value.prepared_activation.program_binding_id =
        core::ProgramBindingId("program-binding-2");
  });
  add([](auto &value) {
    value.prepared_activation.prepared_activation_id =
        ContentDigest(std::string(kDigestTwo));
  });

  std::set<std::string> digests;
  for (std::size_t index = 0U; index < variants.size(); ++index) {
    const auto &intent = variants[index];
    SqliteManagerApplyJournal journal(
        directory.path() / ("digest-" + std::to_string(index) + ".sqlite3"),
        intent.cell_id);
    if (intent.apply_generation.value() == 2U) {
      EXPECT_TRUE(journal
                      .EnsureIntent(Intent(intent.apply_id.value(), 1U,
                                           "generation-predecessor-request",
                                           "generation-predecessor-owner"))
                      .inserted);
    }
    const auto result = journal.EnsureIntent(intent);
    ASSERT_TRUE(result.inserted);
    EXPECT_TRUE(digests.insert(result.record.intent_digest.value()).second);
  }
  EXPECT_EQ(digests.size(), variants.size());
}

TEST(ManagerApplyJournal, PersistsExactIntentAndReplaysAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  const auto intent = Intent();
  ContentDigest digest("unset");
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto inserted = journal.EnsureIntent(intent);
    EXPECT_TRUE(inserted.inserted);
    EXPECT_EQ(inserted.record.intent, intent);
    EXPECT_TRUE(inserted.record.intent_digest.value().starts_with("sha256:"));
    digest = inserted.record.intent_digest;

    const auto replay = journal.EnsureIntent(intent);
    EXPECT_FALSE(replay.inserted);
    EXPECT_EQ(replay.record, inserted.record);
    ASSERT_TRUE(journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U))
                    .has_value());
    EXPECT_EQ(journal
                  .ReadRequest(RequestOwnerEpoch("owner-epoch-1"),
                               RequestId("request-1"))
                  ->intent,
              intent);
    EXPECT_EQ(journal.ReadApplyGenerationHighWater(ApplyId("apply-1")),
              ApplyGeneration(1U));
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto record =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->intent, intent);
  EXPECT_EQ(record->intent_digest, digest);
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.cell_id, CellId("cell-1"));
  EXPECT_EQ(report.apply_intent_count, 1);
  EXPECT_EQ(report.distinct_apply_id_count, 1);
  EXPECT_EQ(report.distinct_request_identity_count, 1);
}

TEST(ManagerApplyJournal, PersistsExactParentAuthoringClaimAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "parent-claim-apply.sqlite3";
  const auto intent = ParentClaimIntent();
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto inserted = journal.EnsureIntent(intent);
    ASSERT_TRUE(inserted.inserted);
    EXPECT_EQ(inserted.record.intent, intent);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto record =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->intent, intent);
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal,
     AuthoringOwnerRotationSettlesOnlyClaimsWithoutPendingManagerEffects) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "authoring-owner-rotation.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto claim = *ParentClaimIntent().parent_authoring_claim;
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> current{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       RequestOwnerEpoch("composer-owner-1")}};
  const std::vector<core::CellAdmissionCurrentOwnerEpochDTO> successor{
      {core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
       RequestOwnerEpoch("composer-owner-2")}};
  const auto evidence =
      core::CellAdmissionOwnerEpochReconciliationEvidenceDigest(
          core::CellAdmissionOwnerRoleDTO::kAuthoringOwner,
          RequestOwnerEpoch("composer-owner-2"));
  ASSERT_TRUE(evidence.ok());
  const core::CellAdmissionReconcileDTO reconcile{
      RequestOwnerEpoch("kernel-owner-1"), {claim}, successor,
      evidence.value()};

  const auto local = ValidateManagerAuthoringOwnerReconciliationReadOnly(
      path, CellId("cell-1"), {claim}, current, reconcile);
  ASSERT_TRUE(local.ok());
  ASSERT_EQ(local.value().settlements.size(), 1U);
  EXPECT_EQ(local.value().settlements[0].predecessor_claim, claim);
  EXPECT_EQ(local.value().settlements[0].successor_owner_epoch,
            RequestOwnerEpoch("composer-owner-2"));

  static_cast<void>(journal.EnsureIntent(ParentClaimIntent()));
  const auto pending = ValidateManagerAuthoringOwnerReconciliationReadOnly(
      path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_FALSE(pending.ok());

  const auto durable_intent =
      journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable_intent.has_value());
  static_cast<void>(journal.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      ManagerApplyFailureEvidenceDTO{
          durable_intent->intent_digest,
          ManagerApplyPhase::kAccepted,
          "maintenance_preflight_refused",
          "maintenance was refused before any runtime effect",
          ManagerApplyEffectDisposition::kKnown},
  }));
  const auto refused_before_effect =
      ValidateManagerAuthoringOwnerReconciliationReadOnly(
          path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_TRUE(refused_before_effect.ok());

  const auto destructive_path =
      directory.path() / "authoring-owner-destructive-failure.sqlite3";
  SqliteManagerApplyJournal destructive(destructive_path, CellId("cell-1"));
  const auto destructive_intent =
      destructive.EnsureIntent(ParentClaimIntent()).record;
  static_cast<void>(destructive.EnsureTerminalOutcome({
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ManagerApplyTerminalOutcomeState::kFailed,
      ManagerApplyFailureEvidenceDTO{
          destructive_intent.intent_digest,
          ManagerApplyPhase::kRuntimeConverged,
          "runtime_initialization_failed",
          "replacement initialization failed after teardown",
          ManagerApplyEffectDisposition::kKnown},
  }));
  const auto destructive_failure =
      ValidateManagerAuthoringOwnerReconciliationReadOnly(
          destructive_path, CellId("cell-1"), {claim}, current, reconcile);
  EXPECT_FALSE(destructive_failure.ok());
}

TEST(ManagerApplyJournal,
     ReadsOneTransactionallyConsistentStatusForEveryDurableIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.ReadStatusInventory().empty());

  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto scope =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()).record;
  const auto records = journal.ReadStatusInventory();
  ASSERT_EQ(records.size(), 1U);
  EXPECT_EQ(records[0].intent, intent);
  ASSERT_TRUE(records[0].affected_scope.has_value());
  EXPECT_EQ(records[0].affected_scope->affected_scope_digest,
            scope.affected_scope_digest);
  EXPECT_FALSE(records[0].fence_attempt.has_value());
  EXPECT_FALSE(records[0].fence_acceptance.has_value());
  EXPECT_FALSE(records[0].runtime_convergence_intent.has_value());
  EXPECT_FALSE(records[0].latest_runtime_staged_ready.has_value());
  EXPECT_FALSE(records[0].private_materialization.has_value());
  EXPECT_FALSE(records[0].publication_receipt.has_value());
  EXPECT_FALSE(records[0].fence_release_intent.has_value());
  EXPECT_FALSE(records[0].fence_released.has_value());
  EXPECT_FALSE(records[0].terminal_outcome.has_value());
}

TEST(ManagerApplyJournal, CommittedChildWriteSurvivesProcessExit) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    try {
      SqliteManagerApplyJournal journal(path, CellId("cell-1"));
      static_cast<void>(journal.EnsureIntent(Intent()));
      _exit(0);
    } catch (...) {
      _exit(71);
    }
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto durable =
      reopened.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable.has_value());
  EXPECT_EQ(durable->intent, Intent());
}

TEST(ManagerApplyJournal, UncommittedChildWriteDisappearsOnRecovery) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal initialized(path, CellId("cell-1")); }
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    sqlite3 *database = nullptr;
    if (sqlite3_open_v2(path.string().c_str(), &database,
                        SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX,
                        nullptr) != SQLITE_OK) {
      _exit(72);
    }
    const std::string sql =
        "BEGIN IMMEDIATE;"
        "INSERT INTO manager_apply_intents ("
        "apply_id,apply_generation,request_id,request_owner_epoch,"
        "source_label,correlation_id,predecessor_retained_bundle_id,"
        "predecessor_graph_revision,predecessor_scene_id,"
        "predecessor_scene_revision_id,predecessor_scene_content_hash,"
        "candidate_retained_bundle_id,candidate_graph_revision,"
        "captured_input_digest,deployment_artifact_id,deployment_revision,"
        "program_binding_id,prepared_activation_id,"
        "prepared_activation_receipt_json,"
        "expected_admission_generation,parent_authoring_claim,intent_digest"
        ") VALUES ("
        "'apply-uncommitted',1,'request-uncommitted','owner-epoch-1',"
        "'child',NULL,'predecessor-bundle','predecessor-graph','" +
        std::string(kSceneIdOne) +
        "',"
        "7,'" +
        std::string(kDigestOne) +
        "','candidate-bundle','candidate-graph',"
        "'sha256:14141414141414141414141414141414141414141414141414141414"
        "14141414','deployment-artifact-1',"
        "'sha256:15151515151515151515151515151515151515151515151515151515"
        "15151515','sha256:17171717171717171717171717171717171717171717171717171717"
        "17171717',"
        "'sha256:16161616161616161616161616161616161616161616161616161616"
        "16161616','{\"schema_version\":\"test.prepared_activation_receipt.v1\"}',"
        "7,NULL,'sha256:"
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'"
        ");";
    if (sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr) !=
        SQLITE_OK) {
      sqlite3_close(database);
      _exit(73);
    }
    _exit(0);
  }
  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  ASSERT_EQ(WEXITSTATUS(status), 0);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_FALSE(
      reopened.ReadIntent(ApplyId("apply-uncommitted"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 0);
}

TEST(ManagerApplyJournal, RequiresContiguousGenerationPerApplyIdentity) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));

  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-1", 2U, "request-2", "owner-epoch-1")),
      ManagerApplyJournalConflictError);
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-1", 2U, "request-2",
                                       "owner-epoch-1", "candidate-bundle-2"))
                  .inserted);
  EXPECT_EQ(journal.ReadApplyGenerationHighWater(ApplyId("apply-1")),
            ApplyGeneration(2U));
  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-1", 4U, "request-4", "owner-epoch-1")),
      ManagerApplyJournalConflictError);

  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-2", 1U, "request-3",
                                       "owner-epoch-1", "candidate-bundle-3"))
                  .inserted);
  const auto report = journal.VerifyIntegrity();
  EXPECT_EQ(report.apply_intent_count, 3);
  EXPECT_EQ(report.distinct_apply_id_count, 2);
}

TEST(ManagerApplyJournal, PublicRequestIdentityCannotNameAnotherIntent) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);

  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-2", 1U, "request-1", "owner-epoch-1",
                                  "candidate-bundle-2")),
      ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadIntent(ApplyId("apply-2"), ApplyGeneration(1U)).has_value());

  // The same request ID in a new request-owner epoch is a new identity.
  EXPECT_TRUE(journal
                  .EnsureIntent(Intent("apply-2", 1U, "request-1",
                                       "owner-epoch-2", "candidate-bundle-2"))
                  .inserted);
}

TEST(ManagerApplyJournal, ApplyIdentityCannotAcquireConflictingPayload) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);

  auto conflicting = Intent();
  conflicting.candidate_retained_bundle_id =
      RetainedBundleId("different-candidate");
  EXPECT_THROW(journal.EnsureIntent(conflicting),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, RejectsForeignCellAndMalformedPersistedValues) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));

  auto foreign = Intent();
  foreign.cell_id = CellId("cell-2");
  EXPECT_THROW(journal.EnsureIntent(foreign), ManagerApplyJournalConflictError);

  auto malformed = Intent();
  malformed.expected_active_projection_revision.scene_revision.content_hash =
      ContentDigest("not-a-digest");
  EXPECT_THROW(journal.EnsureIntent(malformed), std::invalid_argument);
  journal.Close();
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-2")),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyJournal, ExactConcurrentDuplicateHasOneDurableEffect) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal first(path, CellId("cell-1"));
  SqliteManagerApplyJournal second(path, CellId("cell-1"));
  const auto intent = Intent();
  std::barrier start(2);
  auto write = [&](SqliteManagerApplyJournal &journal) {
    start.arrive_and_wait();
    return journal.EnsureIntent(intent);
  };
  auto first_result = std::async(std::launch::async, write, std::ref(first));
  auto second_result = std::async(std::launch::async, write, std::ref(second));
  const auto one = first_result.get();
  const auto two = second_result.get();
  EXPECT_NE(one.inserted, two.inserted);
  EXPECT_EQ(one.record, two.record);
  EXPECT_EQ(first.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, ConcurrentFirstOpenConvergesOneExactSchema) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::barrier open_together(2);
  const auto open = [&, start](const bool explicit_close) {
    start.get();
    open_together.arrive_and_wait();
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto report = journal.VerifyIntegrity();
    if (explicit_close) {
      journal.Close();
    }
    return report;
  };
  auto first = std::async(std::launch::async, open, true);
  std::future<ManagerApplyJournalIntegrityReport> second;
  try {
    second = std::async(std::launch::async, open, false);
  } catch (...) {
    const auto launch_error = std::current_exception();
    start_promise.set_exception(launch_error);
    try {
      static_cast<void>(first.get());
    } catch (...) {
    }
    std::rethrow_exception(launch_error);
  }
  start_promise.set_value();
  EXPECT_EQ(first.get().apply_intent_count, 0);
  EXPECT_EQ(second.get().apply_intent_count, 0);
  ExpectNoSqliteSidecars(path);

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.VerifyIntegrity().apply_intent_count, 0);
}

TEST(ManagerApplyJournal, ConcurrentConflictsDoNotCreateMixedIntent) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal first(path, CellId("cell-1"));
  SqliteManagerApplyJournal second(path, CellId("cell-1"));
  auto one = Intent();
  auto two = Intent();
  two.candidate_retained_bundle_id = RetainedBundleId("candidate-bundle-2");
  std::barrier start(2);
  auto write = [&](SqliteManagerApplyJournal &journal,
                   const ManagerApplyIntentDTO &intent) {
    start.arrive_and_wait();
    try {
      return std::optional<ManagerApplyIntentDTO>(
          journal.EnsureIntent(intent).record.intent);
    } catch (const ManagerApplyJournalConflictError &) {
      return std::optional<ManagerApplyIntentDTO>{};
    }
  };
  auto first_result =
      std::async(std::launch::async, write, std::ref(first), std::cref(one));
  auto second_result =
      std::async(std::launch::async, write, std::ref(second), std::cref(two));
  const auto stored_one = first_result.get();
  const auto stored_two = second_result.get();
  EXPECT_NE(stored_one.has_value(), stored_two.has_value());
  const auto durable =
      first.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(durable.has_value());
  EXPECT_TRUE(durable->intent == one || durable->intent == two);
  EXPECT_EQ(first.VerifyIntegrity().apply_intent_count, 1);
}

TEST(ManagerApplyJournal, DetectsPayloadTamperingWithRestoredSchemaShape) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_intents_no_update;"
          "UPDATE manager_apply_intents "
          "SET predecessor_scene_content_hash='" +
              std::string(kDigestTwo) +
              "';"
              "CREATE TRIGGER manager_apply_intents_no_update "
              "BEFORE UPDATE ON manager_apply_intents "
              "BEGIN "
              "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); "
              "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournal, DetectsUnexpectedSchemaObjects) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  { SqliteManagerApplyJournal journal(path, CellId("cell-1")); }
  ExecSql(path, "CREATE TABLE unexpected(value TEXT) STRICT;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournalIntegrity, DetectsRawGenerationDeletionOnReopen) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    EXPECT_TRUE(journal.EnsureIntent(Intent()).inserted);
    EXPECT_TRUE(journal
                    .EnsureIntent(Intent("apply-1", 2U, "request-2",
                                         "owner-epoch-1", "candidate-bundle-2"))
                    .inserted);
  }
  ExecSql(path, "DROP TRIGGER manager_apply_intents_no_delete;"
                "DELETE FROM manager_apply_intents "
                "WHERE apply_id='apply-1' AND apply_generation=1;"
                "CREATE TRIGGER manager_apply_intents_no_delete "
                "BEFORE DELETE ON manager_apply_intents "
                "BEGIN "
                "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); "
                "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyJournalIntegrity,
     RejectsOversizedPreexistingDatabaseBeforeOpeningIt) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "oversized.sqlite3";
  {
    std::ofstream stream(path);
    ASSERT_TRUE(stream);
  }
  std::filesystem::resize_file(
      path, static_cast<std::uintmax_t>(
                kMaximumManagerApplyJournalDatabasePageImageBytes) +
                1U);
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalLimitError);
}

TEST(ManagerApplyJournal, RejectsSymlinkAndClosedUse) {
  TemporaryDirectory directory;
  const auto real = directory.path() / "real.sqlite3";
  const auto link = directory.path() / "link.sqlite3";
  {
    SqliteManagerApplyJournal journal(real, CellId("cell-1"));
    journal.Close();
    EXPECT_THROW(journal.EnsureIntent(Intent()),
                 ManagerApplyJournalClosedError);
    EXPECT_THROW(journal.VerifyIntegrity(), ManagerApplyJournalClosedError);
  }
  std::filesystem::create_symlink(real, link);
  EXPECT_THROW(SqliteManagerApplyJournal(link, CellId("cell-1")),
               std::invalid_argument);
}

TEST(ManagerApplyJournal, IntentInventoryIsExhaustionBounded) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  std::optional<PersistedManagerApplyIntent> first_record;
  for (std::size_t index = 0U; index < kMaximumManagerApplyIntents; ++index) {
    const auto suffix = std::to_string(index);
    const auto result =
        journal.EnsureIntent(Intent("apply-" + suffix, 1U, "request-" + suffix,
                                    "owner-epoch-1", "candidate-" + suffix));
    EXPECT_TRUE(result.inserted);
    if (index == 0U) {
      first_record = result.record;
    }
  }
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count,
            static_cast<std::int64_t>(kMaximumManagerApplyIntents));
  EXPECT_THROW(
      journal.EnsureIntent(Intent("apply-overflow", 1U, "request-overflow",
                                  "owner-epoch-1", "candidate-overflow")),
      ManagerApplyJournalLimitError);
  ASSERT_TRUE(first_record.has_value());
  const auto replay = journal.EnsureIntent(
      Intent("apply-0", 1U, "request-0", "owner-epoch-1", "candidate-0"));
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record, *first_record);
  EXPECT_EQ(journal.VerifyIntegrity().apply_intent_count,
            static_cast<std::int64_t>(kMaximumManagerApplyIntents));
}

TEST(ManagerApplyFenceJournal,
     PersistsAttemptAndAcceptanceExactlyAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  ContentDigest intent_digest("unset");
  ContentDigest attempt_digest("unset");
  ContentDigest acceptance_digest("unset");
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    intent_digest = journal.EnsureIntent(Intent()).record.intent_digest;
    const auto attempt = EnsureAttempt(journal, intent_digest);
    EXPECT_TRUE(attempt.inserted);
    EXPECT_EQ(attempt.record.intent_digest, intent_digest);
    EXPECT_TRUE(SameFenceIntent(attempt.record.fence_intent, FenceIntent()));
    attempt_digest = attempt.record.fence_attempt_digest;
    const auto attempt_replay = EnsureAttempt(journal, intent_digest);
    EXPECT_FALSE(attempt_replay.inserted);
    EXPECT_EQ(attempt_replay.record.fence_attempt_digest, attempt_digest);

    const auto accepted =
        journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                      attempt_digest, AcceptedFence());
    EXPECT_TRUE(accepted.inserted);
    EXPECT_TRUE(
        SameAcceptedFence(accepted.record.accepted_fence, AcceptedFence()));
    acceptance_digest = accepted.record.acceptance_digest;
    const auto accepted_replay =
        journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                      attempt_digest, AcceptedFence());
    EXPECT_FALSE(accepted_replay.inserted);
    EXPECT_EQ(accepted_replay.record.acceptance_digest, acceptance_digest);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto attempt =
      reopened.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(attempt.has_value());
  EXPECT_EQ(attempt->intent_digest, intent_digest);
  EXPECT_EQ(attempt->fence_attempt_digest, attempt_digest);
  EXPECT_TRUE(SameFenceIntent(attempt->fence_intent, FenceIntent()));
  const auto accepted =
      reopened.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(accepted.has_value());
  EXPECT_EQ(accepted->fence_attempt_digest, attempt_digest);
  EXPECT_EQ(accepted->acceptance_digest, acceptance_digest);
  EXPECT_TRUE(SameAcceptedFence(accepted->accepted_fence, AcceptedFence()));
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.affected_scope_count, 1);
  EXPECT_EQ(report.fence_attempt_count, 1);
  EXPECT_EQ(report.fence_acceptance_count, 1);
}

TEST(ManagerApplyFenceJournal,
     AffectedScopeIsImmutableAndRequiredBeforeFenceAttempt) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto affected =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope());
  EXPECT_TRUE(affected.inserted);
  const auto replay =
      journal.EnsureAffectedScope(intent.intent_digest, AffectedScope());
  EXPECT_FALSE(replay.inserted);
  EXPECT_EQ(replay.record.affected_scope_digest,
            affected.record.affected_scope_digest);

  auto different = AffectedScope();
  different.scope.resources.push_back(ResourceId("joint-3"));
  EXPECT_THROW(journal.EnsureAffectedScope(intent.intent_digest, different),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(journal.EnsureFenceAttempt(
                   intent.intent_digest, ContentDigest(std::string(kDigestTwo)),
                   FenceIntent()),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().affected_scope_count, 1);
  EXPECT_EQ(journal.VerifyIntegrity().fence_attempt_count, 0);
}

TEST(ManagerApplyFenceJournal, DetectsAffectedScopePayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "affected-scope.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    static_cast<void>(
        journal.EnsureAffectedScope(intent.intent_digest, AffectedScope()));
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_affected_scopes_no_update;"
          "UPDATE manager_apply_affected_scopes SET resources_encoding='0;';"
          "CREATE TRIGGER manager_apply_affected_scopes_no_update "
          "BEFORE UPDATE ON manager_apply_affected_scopes BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_affected_scopes is immutable'); "
          "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyFenceJournal,
     RequiresExactParentAndRejectsDifferingReplayEvidence) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  EXPECT_THROW(EnsureAttempt(journal, ContentDigest(std::string(kDigestTwo)),
                             FenceIntent()),
               ManagerApplyJournalConflictError);
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;

  auto differing_attempt = FenceIntent();
  differing_attempt.reason = "different exact evidence";
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, differing_attempt),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   ContentDigest(std::string(kDigestTwo)), AcceptedFence()),
               ManagerApplyJournalConflictError);

  EXPECT_TRUE(
      journal
          .EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                 attempt.fence_attempt_digest, AcceptedFence())
          .inserted);
  auto differing_acceptance = AcceptedFence();
  differing_acceptance.replay_status =
      core::RequestReplayStatusDTO::kReplayedExact;
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   attempt.fence_attempt_digest, differing_acceptance),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyFenceJournal, RejectsAcceptanceWithDifferentCapturedScope) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
  auto accepted = AcceptedFence();
  accepted.captured_scope.resources.push_back(ResourceId("joint-3"));
  EXPECT_THROW(
      journal.EnsureFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U),
                                    attempt.fence_attempt_digest, accepted),
      ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  auto wrong_generation = AcceptedFence();
  wrong_generation.configuration_fence_generation =
      ConfigurationFenceGeneration(6U);
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-1"), ApplyGeneration(1U),
                   attempt.fence_attempt_digest, wrong_generation),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyFenceJournal,
     OneAcceptedFenceIdentityCannotBindTwoApplyGenerations) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto first_intent = journal.EnsureIntent(Intent()).record;
  const auto first_attempt =
      EnsureAttempt(journal, first_intent.intent_digest).record;
  EXPECT_TRUE(journal
                  .EnsureFenceAcceptance(
                      ApplyId("apply-1"), ApplyGeneration(1U),
                      first_attempt.fence_attempt_digest, AcceptedFence())
                  .inserted);

  const auto second_input =
      Intent("apply-2", 1U, "request-2", "owner-epoch-1", "candidate-bundle-2");
  const auto second_intent = journal.EnsureIntent(second_input).record;
  auto second_fence_intent = FenceIntent();
  second_fence_intent.owner_request_context = second_input.request_context;
  second_fence_intent.owner_transaction =
      core::ApplyFenceOwnerDTO{ApplyId("apply-2"), ApplyGeneration(1U)};
  const auto second_attempt =
      EnsureAttempt(journal, second_intent.intent_digest, second_fence_intent)
          .record;
  EXPECT_THROW(journal.EnsureFenceAcceptance(
                   ApplyId("apply-2"), ApplyGeneration(1U),
                   second_attempt.fence_attempt_digest, AcceptedFence()),
               ManagerApplyJournalConflictError);
  EXPECT_FALSE(
      journal.ReadFenceAcceptance(ApplyId("apply-2"), ApplyGeneration(1U))
          .has_value());
  EXPECT_EQ(journal.VerifyIntegrity().fence_acceptance_count, 1);
}

TEST(ManagerApplyFenceJournal, EnforcesCardinalityAndTextLimits) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "apply.sqlite3",
                                    CellId("cell-1"));
  const auto intent = journal.EnsureIntent(Intent()).record;
  auto too_many_resources = FenceIntent();
  too_many_resources.scope.resources.clear();
  for (std::size_t index = 0U; index <= kMaximumManagerApplyFenceResources;
       ++index) {
    too_many_resources.scope.resources.emplace_back("resource-" +
                                                    std::to_string(index));
  }
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, too_many_resources),
               ManagerApplyJournalLimitError);
  auto long_reason = FenceIntent();
  long_reason.reason.assign(kMaximumManagerApplyFenceReasonBytes + 1U, 'x');
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, long_reason),
               std::invalid_argument);
  auto invalid_identifier = FenceIntent();
  invalid_identifier.scope.resources = {ResourceId("bad\nresource")};
  EXPECT_THROW(EnsureAttempt(journal, intent.intent_digest, invalid_identifier),
               std::invalid_argument);
  EXPECT_EQ(journal.VerifyIntegrity().fence_attempt_count, 0);
}

TEST(ManagerApplyFenceJournal,
     PersistsAndReloadsTheExactQualifiedTransitionEnvelope) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "maximum-fence.sqlite3";
  const auto fence_intent = MaximumFenceIntent();
  ASSERT_TRUE(fence_intent.valid());
  std::optional<PersistedManagerApplyAffectedScope> expected_scope;
  std::optional<PersistedManagerApplyFenceAttempt> expected_attempt;
  std::optional<PersistedManagerApplyFenceAcceptance> expected_acceptance;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    expected_scope = journal
                         .EnsureAffectedScope(intent.intent_digest,
                                              AffectedScope(fence_intent))
                         .record;
    expected_attempt =
        journal
            .EnsureFenceAttempt(intent.intent_digest,
                                expected_scope->affected_scope_digest,
                                fence_intent)
            .record;
    expected_acceptance =
        journal
            .EnsureFenceAcceptance(
                ApplyId("apply-1"), ApplyGeneration(1U),
                expected_attempt->fence_attempt_digest,
                ConfigurationFenceAcceptedDTO{
                    ConfigurationFenceId("fence-maximum"),
                    ConfigurationFenceGeneration(5U), fence_intent.scope,
                    123456789LL, core::RequestReplayStatusDTO::kAcceptedNew})
            .record;
    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.affected_scope_count, 1);
    EXPECT_EQ(report.fence_attempt_count, 1);
    EXPECT_EQ(report.fence_acceptance_count, 1);
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  const auto stored_scope =
      reopened.ReadAffectedScope(ApplyId("apply-1"), ApplyGeneration(1U));
  const auto stored_attempt =
      reopened.ReadFenceAttempt(ApplyId("apply-1"), ApplyGeneration(1U));
  const auto stored_acceptance =
      reopened.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(stored_scope.has_value());
  ASSERT_TRUE(stored_attempt.has_value());
  ASSERT_TRUE(stored_acceptance.has_value());
  EXPECT_EQ(stored_scope->affected_scope.scope, fence_intent.scope);
  EXPECT_EQ(stored_scope->affected_scope_digest,
            expected_scope->affected_scope_digest);
  EXPECT_EQ(stored_attempt->fence_intent, fence_intent);
  EXPECT_EQ(stored_attempt->fence_attempt_digest,
            expected_attempt->fence_attempt_digest);
  EXPECT_EQ(stored_acceptance->accepted_fence.captured_scope,
            fence_intent.scope);
  EXPECT_EQ(stored_acceptance->acceptance_digest,
            expected_acceptance->acceptance_digest);
}

TEST(ManagerApplyFenceJournal,
     DetectsAttemptAndAcceptancePayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto attempt_path = directory.path() / "attempt.sqlite3";
  {
    SqliteManagerApplyJournal journal(attempt_path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    static_cast<void>(EnsureAttempt(journal, intent.intent_digest));
  }
  ExecSql(
      attempt_path,
      "DROP TRIGGER manager_apply_fence_attempts_no_update;"
      "UPDATE manager_apply_fence_attempts SET reason='tampered';"
      "CREATE TRIGGER manager_apply_fence_attempts_no_update "
      "BEFORE UPDATE ON manager_apply_fence_attempts BEGIN "
      "SELECT RAISE(ABORT, 'manager_apply_fence_attempts is immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(attempt_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);

  const auto acceptance_path = directory.path() / "acceptance.sqlite3";
  {
    SqliteManagerApplyJournal journal(acceptance_path, CellId("cell-1"));
    const auto intent = journal.EnsureIntent(Intent()).record;
    const auto attempt = EnsureAttempt(journal, intent.intent_digest).record;
    static_cast<void>(journal.EnsureFenceAcceptance(
        ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
        AcceptedFence()));
  }
  ExecSql(
      acceptance_path,
      "DROP TRIGGER manager_apply_fence_acceptances_no_update;"
      "UPDATE manager_apply_fence_acceptances SET accepted_at_ns=1;"
      "CREATE TRIGGER manager_apply_fence_acceptances_no_update "
      "BEFORE UPDATE ON manager_apply_fence_acceptances BEGIN "
      "SELECT RAISE(ABORT, 'manager_apply_fence_acceptances is immutable'); "
      "END;");
  EXPECT_THROW(SqliteManagerApplyJournal(acceptance_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyRuntimeJournal,
     PersistsIntentAndStagedReadyExactlyAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "runtime.sqlite3";
  PersistedManagerApplyRuntimeConvergenceIntent expected_intent{
      RuntimeIntent(ContentDigest(std::string(kDigestOne))),
      ContentDigest(std::string(kDigestOne)),
      ContentDigest(std::string(kDigestOne)),
  };
  PersistedManagerApplyRuntimeStagedReady expected_staged{
      RuntimeStagedReady(ContentDigest(std::string(kDigestOne)), 1U),
      ContentDigest(std::string(kDigestOne)),
  };
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    EXPECT_TRUE(runtime_intent.inserted);
    EXPECT_TRUE(runtime_intent.record.runtime_convergence_intent.valid());
    const auto runtime_replay = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    EXPECT_FALSE(runtime_replay.inserted);
    EXPECT_EQ(runtime_replay.record, runtime_intent.record);
    expected_intent = runtime_intent.record;

    const auto staged = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
        runtime_intent.record.runtime_convergence_intent_digest, 1U));
    EXPECT_TRUE(staged.inserted);
    EXPECT_TRUE(staged.record.staged_ready.valid());
    const auto staged_replay =
        journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
            runtime_intent.record.runtime_convergence_intent_digest, 1U));
    EXPECT_FALSE(staged_replay.inserted);
    EXPECT_EQ(staged_replay.record, staged.record);
    expected_staged = staged.record;
  }

  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.ReadRuntimeConvergenceIntent(ApplyId("apply-1"),
                                                  ApplyGeneration(1U)),
            expected_intent);
  EXPECT_EQ(reopened.ReadLatestRuntimeStagedReady(ApplyId("apply-1"),
                                                  ApplyGeneration(1U)),
            expected_staged);
  const auto report = reopened.VerifyIntegrity();
  EXPECT_EQ(report.runtime_convergence_intent_count, 1);
  EXPECT_EQ(report.runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeJournal,
     RequiresAcceptedParentAndRejectsConflictingReplay) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "runtime.sqlite3",
                                    CellId("cell-1"));
  const auto parent = journal.EnsureIntent(Intent()).record;
  EXPECT_THROW(journal.EnsureRuntimeConvergenceIntent(
                   parent.intent_digest,
                   RuntimeIntent(ContentDigest(std::string(kDigestOne)))),
               ManagerApplyJournalConflictError);

  const auto attempt = EnsureAttempt(journal, parent.intent_digest).record;
  const auto acceptance = journal.EnsureFenceAcceptance(
      ApplyId("apply-1"), ApplyGeneration(1U), attempt.fence_attempt_digest,
      AcceptedFence());
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      parent.intent_digest, RuntimeIntent(acceptance.record.acceptance_digest));

  auto conflicting_intent = RuntimeIntent(acceptance.record.acceptance_digest);
  conflicting_intent.runtime_units.front().product_identity =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsureRuntimeConvergenceIntent(parent.intent_digest,
                                                      conflicting_intent),
               ManagerApplyJournalConflictError);

  EXPECT_THROW(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
                   ContentDigest(std::string(kDigestTwo)), 1U)),
               ManagerApplyJournalConflictError);
  EXPECT_THROW(
      journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
          runtime_intent.record.runtime_convergence_intent_digest, 2U)),
      ManagerApplyJournalConflictError);

  const auto first = journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
      runtime_intent.record.runtime_convergence_intent_digest, 1U));
  auto conflicting_staged = first.record.staged_ready;
  conflicting_staged.runtime_units.front().process_observation_sequence = 2U;
  EXPECT_THROW(journal.EnsureRuntimeStagedReady(conflicting_staged),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().runtime_staged_ready_count, 1);
}

TEST(ManagerApplyRuntimeJournal,
     EmptyRuntimeSetIsExplicitAndStagedGenerationsAreBounded) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(directory.path() / "runtime.sqlite3",
                                    CellId("cell-1"));
  const auto accepted = EnsureAcceptedApply(journal);
  const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
      accepted.intent.intent_digest,
      RuntimeIntent(accepted.acceptance.acceptance_digest, true));
  ASSERT_TRUE(
      runtime_intent.record.runtime_convergence_intent.runtime_units.empty());

  for (std::uint64_t generation = 1U;
       generation <= kMaximumRuntimeStagedReadySetsPerApply; ++generation) {
    EXPECT_TRUE(journal
                    .EnsureRuntimeStagedReady(RuntimeStagedReady(
                        runtime_intent.record.runtime_convergence_intent_digest,
                        generation, true))
                    .inserted);
  }
  EXPECT_THROW(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
                   runtime_intent.record.runtime_convergence_intent_digest,
                   kMaximumRuntimeStagedReadySetsPerApply + 1U, true)),
               ManagerApplyJournalConflictError);
  EXPECT_EQ(journal.VerifyIntegrity().runtime_staged_ready_count,
            static_cast<std::int64_t>(kMaximumRuntimeStagedReadySetsPerApply));
}

TEST(ManagerApplyRuntimeJournal,
     DetectsConvergenceAndStagedReadyPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto intent_path = directory.path() / "runtime-intent.sqlite3";
  {
    SqliteManagerApplyJournal journal(intent_path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    static_cast<void>(journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest)));
  }
  ExecSql(intent_path,
          "DROP TRIGGER manager_apply_runtime_convergence_intents_no_update;"
          "UPDATE manager_apply_runtime_convergence_intents "
          "SET runtime_units_encoding='0;';"
          "CREATE TRIGGER manager_apply_runtime_convergence_intents_no_update "
          "BEFORE UPDATE ON manager_apply_runtime_convergence_intents BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_runtime_convergence_intents is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(intent_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);

  const auto staged_path = directory.path() / "runtime-staged.sqlite3";
  {
    SqliteManagerApplyJournal journal(staged_path, CellId("cell-1"));
    const auto accepted = EnsureAcceptedApply(journal);
    const auto runtime_intent = journal.EnsureRuntimeConvergenceIntent(
        accepted.intent.intent_digest,
        RuntimeIntent(accepted.acceptance.acceptance_digest));
    static_cast<void>(journal.EnsureRuntimeStagedReady(RuntimeStagedReady(
        runtime_intent.record.runtime_convergence_intent_digest, 1U)));
  }
  ExecSql(staged_path,
          "DROP TRIGGER manager_apply_runtime_staged_ready_sets_no_update;"
          "UPDATE manager_apply_runtime_staged_ready_sets "
          "SET runtime_evidence_encoding='0;';"
          "CREATE TRIGGER manager_apply_runtime_staged_ready_sets_no_update "
          "BEFORE UPDATE ON manager_apply_runtime_staged_ready_sets BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_runtime_staged_ready_sets is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(staged_path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplySelectedBundleJournal,
     PersistsCompleteForwardChainAndExactReplayAcrossRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "selected-bundle.sqlite3";
  std::optional<SelectedBundleTransitionRecords> expected;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    expected = EnsureSelectedBundleTransition(journal);

    const auto materialization =
        journal.EnsurePrivateMaterialization(PrivateMaterialization());
    EXPECT_FALSE(materialization.inserted);
    EXPECT_EQ(materialization.record, expected->materialization);
    const auto publication = journal.EnsurePublicationReceipt(
        PublicationReceipt(expected->materialization));
    EXPECT_FALSE(publication.inserted);
    EXPECT_EQ(publication.record, expected->publication);
    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.private_materialization_count, 1);
    EXPECT_EQ(report.publication_receipt_count, 1);
  }

  ASSERT_TRUE(expected.has_value());
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(reopened.ReadPrivateMaterialization(ApplyId("apply-1"),
                                                ApplyGeneration(1U)),
            expected->materialization);
  EXPECT_EQ(
      reopened.ReadPublicationReceipt(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->publication);
}

TEST(ManagerApplySelectedBundleJournal,
     RejectsConflictingReplayAtEveryForwardBoundary) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(
      directory.path() / "selected-bundle-conflict.sqlite3", CellId("cell-1"));
  const auto records = EnsureSelectedBundleTransition(journal);

  auto materialization = PrivateMaterialization();
  materialization.retained_bundle_canonical_byte_size += 1U;
  EXPECT_THROW(journal.EnsurePrivateMaterialization(materialization),
               ManagerApplyJournalConflictError);

  auto publication = PublicationReceipt(records.materialization);
  publication.publication_receipt.durable_public_pointer_content_digest =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsurePublicationReceipt(publication),
               ManagerApplyJournalConflictError);

}

TEST(ManagerApplySelectedBundleJournal,
     DetectsForwardChainPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "selected-bundle-corrupt.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    static_cast<void>(EnsureSelectedBundleTransition(journal));
  }
  ExecSql(path,
          "DROP TRIGGER manager_apply_private_materializations_no_update;"
          "UPDATE manager_apply_private_materializations "
          "SET retained_bundle_canonical_byte_size=513;"
          "CREATE TRIGGER manager_apply_private_materializations_no_update "
          "BEFORE UPDATE ON manager_apply_private_materializations BEGIN "
          "SELECT RAISE(ABORT, 'manager_apply_private_materializations is "
          "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

TEST(ManagerApplyCompletionJournal,
     PersistsReleaseAndTerminalChainWithExactRestartReplay) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply-completion.sqlite3";
  std::optional<SelectedBundleTransitionRecords> transition;
  std::optional<ApplyCompletionRecords> expected;
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    transition = EnsureSelectedBundleTransition(journal);
    expected = EnsureApplyCompletion(journal, *transition);

    const auto acceptance =
        journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
    ASSERT_TRUE(acceptance.has_value());
    const auto release_intent = journal.EnsureFenceReleaseIntent(
        FenceReleaseIntent(*transition, *acceptance));
    EXPECT_FALSE(release_intent.inserted);
    EXPECT_EQ(release_intent.record, expected->release_intent);
    const auto released =
        journal.EnsureFenceReleased(FenceReleased(expected->release_intent));
    EXPECT_FALSE(released.inserted);
    EXPECT_EQ(released.record, expected->released);
    const auto terminal = journal.EnsureTerminalOutcome(
        TerminalOutcome(*transition, expected->released));
    EXPECT_FALSE(terminal.inserted);
    EXPECT_EQ(terminal.record, expected->terminal);
    const auto status = journal.ReadStatus(ApplyId("apply-1"),
                                           ApplyGeneration(1U));
    ASSERT_TRUE(status.has_value());
    const auto progress = DeriveManagerApplyProgress(*status);
    EXPECT_EQ(progress.phase, ManagerApplyPhase::kSettled);
    EXPECT_EQ(progress.record_digest,
              expected->terminal.terminal_outcome_digest);

    const auto report = journal.VerifyIntegrity();
    EXPECT_EQ(report.fence_release_intent_count, 1);
    EXPECT_EQ(report.fence_released_count, 1);
    EXPECT_EQ(report.terminal_outcome_count, 1);
  }

  ASSERT_TRUE(expected.has_value());
  SqliteManagerApplyJournal reopened(path, CellId("cell-1"));
  EXPECT_EQ(
      reopened.ReadFenceReleaseIntent(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->release_intent);
  EXPECT_EQ(reopened.ReadFenceReleased(ApplyId("apply-1"), ApplyGeneration(1U)),
            expected->released);
  EXPECT_EQ(
      reopened.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U)),
      expected->terminal);
}

TEST(ManagerApplyCompletionJournal,
     DependencyReadsDoNotRetainAbsenceOrCorruptionAcrossCalls) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "read-scope.sqlite3";
  SqliteManagerApplyJournal journal(path, CellId("cell-1"));
  const auto transition = EnsureSelectedBundleTransition(journal);
  const auto read = [&] {
    return journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U));
  };
  EXPECT_FALSE(read().has_value());
  const auto completed = EnsureApplyCompletion(journal, transition);
  EXPECT_EQ(read(), completed.terminal);

  // A separate writer changes a deep predecessor without changing schema
  // shape. A completed earlier read must not hide it, and an exception must
  // not leave a stale read context behind after the database is restored.
  const auto rewrite_source = [&](const std::string &source) {
    ExecSql(
        path,
        "DROP TRIGGER manager_apply_intents_no_update;"
        "UPDATE manager_apply_intents SET source_label='" +
            source +
            "';"
            "CREATE TRIGGER manager_apply_intents_no_update "
            "BEFORE UPDATE ON manager_apply_intents BEGIN "
            "SELECT RAISE(ABORT, 'manager_apply_intents is immutable'); END;");
  };
  rewrite_source("tampered-source");
  EXPECT_THROW(static_cast<void>(read()), ManagerApplyJournalIntegrityError);
  EXPECT_THROW(static_cast<void>(journal.VerifyIntegrity()),
               ManagerApplyJournalIntegrityError);
  rewrite_source(Intent().request_context.source_label);
  EXPECT_EQ(read(), completed.terminal);
  EXPECT_EQ(journal.VerifyIntegrity().terminal_outcome_count, 1);
}

TEST(ManagerApplyCompletionJournal,
     RejectsPrematureTerminalAndConflictingCompletionRecords) {
  TemporaryDirectory directory;
  SqliteManagerApplyJournal journal(
      directory.path() / "apply-completion-conflict.sqlite3", CellId("cell-1"));
  const auto transition = EnsureSelectedBundleTransition(journal);
  const auto acceptance =
      journal.ReadFenceAcceptance(ApplyId("apply-1"), ApplyGeneration(1U));
  ASSERT_TRUE(acceptance.has_value());
  const auto release_intent = journal.EnsureFenceReleaseIntent(
      FenceReleaseIntent(transition, *acceptance));
  EXPECT_FALSE(
      journal.ReadTerminalOutcome(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  auto impossible_terminal = ManagerApplyTerminalOutcomeDTO{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      transition.materialization.private_materialization
          .candidate_projection_revision,
      transition.publication.publication_receipt_digest,
      transition.publication.publication_receipt_digest,
      ContentDigest(std::string(kDigestOne)),
      ManagerApplyTerminalOutcomeState::kSucceeded,
      std::nullopt,
  };
  EXPECT_THROW(journal.EnsureTerminalOutcome(impossible_terminal),
               ManagerApplyJournalConflictError);

  auto conflicting_intent = FenceReleaseIntent(transition, *acceptance);
  conflicting_intent.release_request.manager_runtime_epoch =
      RuntimeEpoch("manager-epoch-2");
  EXPECT_THROW(journal.EnsureFenceReleaseIntent(conflicting_intent),
               ManagerApplyJournalConflictError);

  const auto released =
      journal.EnsureFenceReleased(FenceReleased(release_intent.record));
  auto conflicting_release = FenceReleased(release_intent.record);
  conflicting_release.released_fence.released_at_ns += 1;
  EXPECT_THROW(journal.EnsureFenceReleased(conflicting_release),
               ManagerApplyJournalConflictError);

  static_cast<void>(journal.EnsureTerminalOutcome(
      TerminalOutcome(transition, released.record)));
  auto conflicting_terminal = TerminalOutcome(transition, released.record);
  conflicting_terminal.publication_receipt_digest =
      ContentDigest(std::string(kDigestTwo));
  EXPECT_THROW(journal.EnsureTerminalOutcome(conflicting_terminal),
               ManagerApplyJournalConflictError);
}

TEST(ManagerApplyCompletionJournal,
     DetectsReleaseRequestPayloadCorruptionOnRestart) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply-completion-corrupt.sqlite3";
  {
    SqliteManagerApplyJournal journal(path, CellId("cell-1"));
    const auto transition = EnsureSelectedBundleTransition(journal);
    static_cast<void>(EnsureApplyCompletion(journal, transition));
  }
  ExecSql(path, "DROP TRIGGER manager_apply_fence_release_intents_no_update;"
                "UPDATE manager_apply_fence_release_intents "
                "SET release_request_encoding='0;';"
                "CREATE TRIGGER manager_apply_fence_release_intents_no_update "
                "BEFORE UPDATE ON manager_apply_fence_release_intents BEGIN "
                "SELECT RAISE(ABORT, 'manager_apply_fence_release_intents is "
                "immutable'); END;");
  EXPECT_THROW(SqliteManagerApplyJournal(path, CellId("cell-1")),
               ManagerApplyJournalIntegrityError);
}

} // namespace
} // namespace manyforge::component_manager

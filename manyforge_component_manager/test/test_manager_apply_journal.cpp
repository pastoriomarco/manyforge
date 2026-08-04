#include "manyforge_component_manager/manager_apply_journal.hpp"

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

namespace manyforge::component_manager {
namespace {

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
using core::GraphRevision;
using core::ProjectionRevisionDTO;
using core::PublicRequestContextDTO;
using core::RequestId;
using core::RequestOwnerEpoch;
using core::ResourceId;
using core::RetainedBundleId;
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
  };
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
        "candidate_retained_bundle_id,candidate_graph_revision,intent_digest"
        ") VALUES ("
        "'apply-uncommitted',1,'request-uncommitted','owner-epoch-1',"
        "'child',NULL,'predecessor-bundle','predecessor-graph','" +
        std::string(kSceneIdOne) +
        "',"
        "7,'" +
        std::string(kDigestOne) +
        "','candidate-bundle','candidate-graph','sha256:"
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

} // namespace
} // namespace manyforge::component_manager

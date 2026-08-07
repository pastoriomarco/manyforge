#include "logical_scene_codec_internal.hpp"
#include "manyforge_scene/canonical_empty_scene_owner.hpp"
#include "manyforge_scene/scene_transaction_codec.hpp"
#include "sqlite_scene_lifecycle_internal.hpp"

#include <gtest/gtest.h>
#include <sqlite3.h>

#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_core/logical_scene_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::scene {
namespace {

using namespace std::chrono_literals;

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::BootstrapGeneration;
using core::BootstrapId;
using core::BootstrapPublicationTransactionDTO;
using core::CellId;
using core::ContentDigest;
using core::ErrorDTO;
using core::GraphRevision;
using core::IActiveBundlePublicationReader;
using core::NamedApplyPublicationTransactionDTO;
using core::ProjectionApplyActivationDTO;
using core::ProjectionApplyActivationResultState;
using core::ProjectionApplyDiscardDTO;
using core::ProjectionApplyDiscardResultState;
using core::ProjectionApplyFinalizeDTO;
using core::ProjectionApplyFinalizeResultState;
using core::ProjectionApplyStageDTO;
using core::ProjectionApplyStageResultState;
using core::ProjectionApplyStatusQueryDTO;
using core::ProjectionApplyStatusState;
using core::ProjectionBootstrapAcceptedDTO;
using core::ProjectionBootstrapFinalizeDTO;
using core::ProjectionBootstrapFinalizeResultState;
using core::ProjectionBootstrapIntentDTO;
using core::ProjectionBootstrapPrerequisiteQueryDTO;
using core::ProjectionBootstrapPrerequisiteState;
using core::ProjectionBootstrapRebuildRequestDTO;
using core::ProjectionBootstrapStatusQueryDTO;
using core::ProjectionBootstrapStatusState;
using core::ProjectionNativeRuntimeBindingDTO;
using core::ProjectionRevisionDTO;
using core::Result;

constexpr std::string_view kExpectedSceneId =
    "sha256:b6edf7957c79f97cabefc2c61056f0bb09e00d1cac1ff3080ed540b650346dad";
constexpr std::string_view kExpectedEmptySceneContentHash =
    "sha256:bb42a1086f1fc8a45d1bcee2edc2bfce5ee2494029fdeeb5b2066717420cdfe5";

static_assert(
    !std::is_constructible_v<CanonicalEmptySceneOwner, std::filesystem::path,
                             ProjectionBootstrapPrerequisiteQueryDTO,
                             const IActiveBundlePublicationReader &>);
static_assert(std::is_constructible_v<
              CanonicalEmptySceneOwner, std::filesystem::path,
              const deployment::CanonicalEmptyRetainedBundleAdmission &,
              const IActiveBundlePublicationReader &>);

class TemporaryDirectory final {
public:
  explicit TemporaryDirectory(std::string label = "scene-owner") {
    auto pattern = (std::filesystem::temp_directory_path() /
                    ("manyforge-" + std::move(label) + "-XXXXXX"))
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

void ExpectNoSqliteSidecars(const std::filesystem::path &path) {
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-wal"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-shm"));
  EXPECT_FALSE(std::filesystem::exists(path.string() + "-journal"));
}

deployment::CanonicalEmptyRetainedBundleAdmission LoadAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

ContentDigest Digest(const char digit) {
  return ContentDigest("sha256:" + std::string(64U, digit));
}

class ControlledPublicationReader final
    : public IActiveBundlePublicationReader {
public:
  void SetCurrent(ActiveBundlePublicationReceiptDTO receipt) {
    std::lock_guard lock(mutex_);
    current_ = std::move(receipt);
    returned_error_.reset();
    throw_on_read_ = false;
  }

  void Clear() {
    std::lock_guard lock(mutex_);
    current_.reset();
    returned_error_.reset();
    throw_on_read_ = false;
  }

  void ThrowOnRead() {
    std::lock_guard lock(mutex_);
    returned_error_.reset();
    throw_on_read_ = true;
  }

  void ReturnError(ErrorDTO error) {
    std::lock_guard lock(mutex_);
    returned_error_ = std::move(error);
    throw_on_read_ = false;
  }

  [[nodiscard]] std::size_t calls() const {
    std::lock_guard lock(mutex_);
    return calls_;
  }

  Result<ActiveBundlePublicationReceiptDTO> ReadActiveBundlePublication(
      const ActiveBundlePublicationGeneration generation) const override {
    std::lock_guard lock(mutex_);
    ++calls_;
    if (throw_on_read_) {
      throw std::runtime_error("publication transport unavailable");
    }
    if (returned_error_.has_value()) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(*returned_error_);
    }
    if (!current_.has_value() ||
        current_->publication_generation != generation) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_generation_not_current",
          "requested generation is not the current head",
          {},
      });
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(*current_);
  }

private:
  mutable std::mutex mutex_;
  mutable std::size_t calls_{};
  bool throw_on_read_{};
  std::optional<ErrorDTO> returned_error_;
  std::optional<ActiveBundlePublicationReceiptDTO> current_;
};

struct SceneLifecycleObservation final {
  SceneLifecycleObservation(
      const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
          attempt,
      const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
          acquired)
      : expected_attempt(attempt), expected_acquired(acquired),
        release_signal(release_after_acquired.get_future().share()) {}

  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
      expected_attempt;
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
      expected_acquired;
  std::promise<void> attempted;
  std::promise<void> acquired;
  std::promise<void> release_after_acquired;
  std::shared_future<void> release_signal;
  std::atomic<bool> attempt_notified{false};
  std::atomic<bool> acquired_notified{false};
};

void ObserveSceneLifecycleEvent(
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        event,
    void *const context) noexcept {
  auto &observation = *static_cast<SceneLifecycleObservation *>(context);
  try {
    if (event == observation.expected_attempt &&
        !observation.attempt_notified.exchange(true)) {
      observation.attempted.set_value();
      return;
    }
    if (event == observation.expected_acquired &&
        !observation.acquired_notified.exchange(true)) {
      observation.acquired.set_value();
      observation.release_signal.wait();
    }
  } catch (...) {
  }
}

class ScopedSceneLifecycleObserver final {
public:
  explicit ScopedSceneLifecycleObserver(
      SceneLifecycleObservation &observation) {
    sqlite_lifecycle_internal::
        SetCanonicalEmptySceneStoreLifecycleEventObserverForTesting(
            &ObserveSceneLifecycleEvent, &observation);
  }

  ~ScopedSceneLifecycleObserver() {
    sqlite_lifecycle_internal::
        ClearCanonicalEmptySceneStoreLifecycleEventObserverForTesting();
  }

  ScopedSceneLifecycleObserver(const ScopedSceneLifecycleObserver &) = delete;
  ScopedSceneLifecycleObserver &
  operator=(const ScopedSceneLifecycleObserver &) = delete;
};

template <typename Action>
void ExpectActionAcquiresSceneLifecycleGate(
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        expected_attempt,
    const sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent
        expected_acquired,
    Action action) {
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleLock held(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex());
  SceneLifecycleObservation observation(expected_attempt, expected_acquired);
  auto attempted = observation.attempted.get_future();
  auto acquired = observation.acquired.get_future();
  const ScopedSceneLifecycleObserver observer(observation);
  auto completion = std::async(
      std::launch::async, [action = std::move(action)]() mutable { action(); });

  const auto attempt_status = attempted.wait_for(2s);
  if (attempt_status != std::future_status::ready) {
    held.unlock();
    observation.release_after_acquired.set_value();
    ADD_FAILURE()
        << "production action did not attempt the scene lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  held.unlock();
  const auto acquired_status = acquired.wait_for(2s);
  if (acquired_status != std::future_status::ready) {
    observation.release_after_acquired.set_value();
    ADD_FAILURE()
        << "production action did not acquire the scene lifecycle gate";
    EXPECT_NO_THROW(completion.get());
    return;
  }

  auto &mutex =
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex();
  const bool acquired_by_test = mutex.try_lock();
  if (acquired_by_test) {
    mutex.unlock();
  }
  EXPECT_FALSE(acquired_by_test)
      << "production action reported acquisition without holding the gate";

  observation.release_after_acquired.set_value();
  EXPECT_NO_THROW(completion.get());
}

ProjectionBootstrapIntentDTO
Intent(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
       std::string bootstrap_id = "bootstrap-alpha") {
  return {
      BootstrapId(std::move(bootstrap_id)),
      BootstrapGeneration(1U),
      admission.cell_id(),
      admission.retained_bundle().value(),
      {},
  };
}

ProjectionBootstrapRebuildRequestDTO
RebuildRequest(const ProjectionBootstrapAcceptedDTO &accepted) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.canonical_empty_target,
  };
}

ActiveBundlePublicationReceiptDTO
PublicationReceipt(const ProjectionBootstrapAcceptedDTO &accepted) {
  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          accepted.bootstrap_id,
          accepted.bootstrap_generation,
      },
      accepted.canonical_empty_target,
      accepted.canonical_empty_target.retained_bundle_id,
      accepted.canonical_empty_target.graph_revision,
      std::nullopt,
      Digest('a'),
      Digest('b'),
  };
  return {
      std::move(request),
      ActiveBundlePublicationGeneration(1U),
      Digest('c'),
  };
}

ProjectionBootstrapFinalizeDTO
FinalizeRequest(const ProjectionBootstrapAcceptedDTO &accepted,
                ActiveBundlePublicationReceiptDTO receipt) {
  return {
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.canonical_empty_target,
      std::move(receipt),
  };
}

void ExecuteSql(const std::filesystem::path &database_path,
                const std::string &sql) {
  sqlite3 *database = nullptr;
  const auto open =
      sqlite3_open_v2(database_path.c_str(), &database,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX, nullptr);
  if (open != SQLITE_OK) {
    const std::string detail =
        database == nullptr ? sqlite3_errstr(open) : sqlite3_errmsg(database);
    if (database != nullptr) {
      sqlite3_close(database);
    }
    throw std::runtime_error("cannot open test database: " + detail);
  }
  char *error = nullptr;
  const auto result =
      sqlite3_exec(database, sql.c_str(), nullptr, nullptr, &error);
  const std::string detail =
      error == nullptr ? sqlite3_errmsg(database) : error;
  sqlite3_free(error);
  sqlite3_close(database);
  if (result != SQLITE_OK) {
    throw std::runtime_error("test SQL failed: " + detail);
  }
}

void Require(const bool condition, const std::string_view message) {
  if (!condition) {
    throw std::runtime_error(std::string(message));
  }
}

ProjectionBootstrapAcceptedDTO FinalizeCanonicalBootstrap(
    CanonicalEmptySceneOwner &owner,
    ControlledPublicationReader &publication_reader,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  static_cast<void>(owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
  const auto receipt = PublicationReceipt(accepted);
  publication_reader.SetCurrent(receipt);
  const auto finalized =
      owner.FinalizeProjectionBootstrap(FinalizeRequest(accepted, receipt));
  Require(finalized.state == ProjectionBootstrapFinalizeResultState::kFinalized,
          "canonical bootstrap did not finalize");
  return accepted;
}

ProjectionApplyStageDTO NamedStageRequest(
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
    const ProjectionRevisionDTO &predecessor,
    std::string apply_id = "apply-alpha") {
  return {
      ApplyId(std::move(apply_id)),
      ApplyGeneration(1U),
      predecessor,
      admission.retained_bundle().value(),
      admission.retained_bundle().value(),
      {},
  };
}

ProjectionApplyActivationDTO
ActivationRequest(const core::ProjectionApplyStageResultDTO &staged) {
  Require(staged.projection_stage_generation.has_value(), "stage has no ID");
  Require(staged.candidate_projection_revision.has_value(),
          "stage has no candidate");
  return {
      staged.apply_id,
      staged.apply_generation,
      staged.expected_active_projection_revision,
      *staged.projection_stage_generation,
      *staged.candidate_projection_revision,
  };
}

ProjectionApplyDiscardDTO
DiscardRequest(const core::ProjectionApplyStageResultDTO &staged) {
  Require(staged.projection_stage_generation.has_value(), "stage has no ID");
  Require(staged.candidate_projection_revision.has_value(),
          "stage has no candidate");
  return {
      staged.apply_id,
      staged.apply_generation,
      *staged.projection_stage_generation,
      *staged.candidate_projection_revision,
      staged.expected_active_projection_revision,
  };
}

ActiveBundlePublicationReceiptDTO
NamedPublicationReceipt(const core::ProjectionApplyStageResultDTO &staged,
                        const std::uint64_t expected_previous_generation = 1U,
                        const std::uint64_t publication_generation = 2U) {
  Require(staged.projection_stage_generation.has_value(), "stage has no ID");
  Require(staged.candidate_projection_revision.has_value(),
          "stage has no candidate");
  ActiveBundlePublicationRequestDTO request{
      NamedApplyPublicationTransactionDTO{
          staged.apply_id,
          staged.apply_generation,
          *staged.projection_stage_generation,
      },
      *staged.candidate_projection_revision,
      staged.candidate_projection_revision->retained_bundle_id,
      staged.candidate_projection_revision->graph_revision,
      ActiveBundlePublicationGeneration(expected_previous_generation),
      Digest('d'),
      Digest('e'),
  };
  return {
      std::move(request),
      ActiveBundlePublicationGeneration(publication_generation),
      Digest('f'),
  };
}

ProjectionApplyFinalizeDTO
NamedFinalizeRequest(const core::ProjectionApplyStageResultDTO &staged,
                     ActiveBundlePublicationReceiptDTO receipt) {
  Require(staged.projection_stage_generation.has_value(), "stage has no ID");
  Require(staged.candidate_projection_revision.has_value(),
          "stage has no candidate");
  return {
      staged.apply_id,
      staged.apply_generation,
      *staged.projection_stage_generation,
      *staged.candidate_projection_revision,
      std::move(receipt),
  };
}

void ExpectExactCanonicalEmptySnapshot(
    const ProjectionBootstrapAcceptedDTO &accepted,
    const deployment::CanonicalEmptyRetainedBundleAdmission &admission) {
  ASSERT_TRUE(accepted.valid());
  const auto &snapshot = *accepted.initialized_logical_scene.shared();
  EXPECT_EQ(snapshot.cell_id, admission.cell_id());
  EXPECT_EQ(snapshot.revision, accepted.canonical_empty_target.scene_revision);
  EXPECT_EQ(snapshot.revision.revision_id, 1U);
  EXPECT_EQ(snapshot.transaction_sequence, 0U);
  EXPECT_EQ(snapshot.topology_epoch, 0U);
  EXPECT_EQ(snapshot.policy_epoch, 0U);
  EXPECT_EQ(snapshot.configured_collision_policy.policy_id, "free_space");
  EXPECT_EQ(snapshot.configured_collision_policy.revision.revision_id, 1U);
  EXPECT_EQ(snapshot.configured_collision_policy.revision.content_hash,
            core::kCanonicalEmptyCollisionPolicyDigest);
  EXPECT_TRUE(
      snapshot.configured_collision_policy.allowed_static_static_pairs.empty());
  EXPECT_TRUE(
      snapshot.configured_collision_policy.allowed_touch_link_pairs.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.support_contacts.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.contact_grasps.empty());
  EXPECT_TRUE(snapshot.configured_collision_policy.require_full_diagnostics);
  EXPECT_TRUE(snapshot.objects.empty());
  EXPECT_TRUE(snapshot.carried_object_attachments.empty());
  EXPECT_EQ(
      core::logical_scene_identity::ComputeLogicalSceneContentHash(snapshot),
      snapshot.revision.content_hash);
}

TEST(CanonicalEmptySceneOwner, SealedAdmissionMintsGoldenAndHoldsGatesClosed) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);

  EXPECT_EQ(owner.fixed_query().cell_id, admission.cell_id());
  EXPECT_EQ(owner.fixed_query().canonical_empty_retained_bundle_id,
            admission.retained_bundle_id());
  EXPECT_EQ(owner.fixed_query().canonical_empty_graph_revision,
            admission.graph_revision());
  const auto observed =
      owner.ObserveProjectionBootstrapPrerequisites(owner.fixed_query());
  EXPECT_TRUE(observed.ready_for(owner.fixed_query()));

  auto foreign_query = owner.fixed_query();
  foreign_query.cell_id = CellId("foreign-cell");
  const auto foreign =
      owner.ObserveProjectionBootstrapPrerequisites(foreign_query);
  EXPECT_TRUE(foreign.presence_valid());
  EXPECT_EQ(foreign.state, ProjectionBootstrapPrerequisiteState::kUnavailable);

  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  ExpectExactCanonicalEmptySnapshot(accepted, admission);
  EXPECT_EQ(accepted.canonical_empty_target.retained_bundle_id,
            admission.retained_bundle_id());
  EXPECT_EQ(accepted.canonical_empty_target.graph_revision,
            admission.graph_revision());
  EXPECT_EQ(accepted.canonical_empty_target.scene_revision.scene_id.value(),
            kExpectedSceneId);
  EXPECT_EQ(accepted.canonical_empty_target.scene_revision.content_hash.value(),
            kExpectedEmptySceneContentHash);

  const auto access = owner.ReadAccessStatus();
  EXPECT_TRUE(access.valid());
  EXPECT_EQ(access.phase, CanonicalEmptySceneBootstrapPhase::kAccepted);
  EXPECT_FALSE(access.scene_writes_open);
  EXPECT_FALSE(access.projection_leases_open);
  EXPECT_FALSE(access.active_projection.has_value());
  EXPECT_FALSE(owner.ReadActiveSnapshot().has_value());
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 0);
  EXPECT_EQ(integrity.finalization_row_count, 0);
  EXPECT_EQ(integrity.fixed_query, owner.fixed_query());
}

TEST(CanonicalEmptySceneOwner,
     BeginAcceptsOnlyExactAdmittedBundleAndOneBootstrapLineage) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);

  auto cross_spliced = Intent(admission);
  cross_spliced.canonical_empty_retained_bundle.deployment_artifact_id =
      core::DeploymentArtifactId("forged-deployment");
  EXPECT_THROW(owner.BeginProjectionBootstrap(cross_spliced),
               CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);

  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  const auto replay = owner.BeginProjectionBootstrap(Intent(admission));
  EXPECT_EQ(replay.canonical_empty_target, accepted.canonical_empty_target);
  EXPECT_EQ(replay.initialized_logical_scene.shared()->revision,
            accepted.initialized_logical_scene.shared()->revision);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(admission, "bootstrap-beta")),
      CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner,
     RestartReconstructsCompleteSnapshotAndExactReplay) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ProjectionRevisionDTO target = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
    ExpectExactCanonicalEmptySnapshot(accepted, admission);
    owner.Close();
    return accepted.canonical_empty_target;
  }();

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status =
      reopened.ProjectionBootstrapStatus(ProjectionBootstrapStatusQueryDTO{
          BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kAccepted);
  EXPECT_EQ(status.canonical_empty_target, target);
  const auto replay = reopened.BeginProjectionBootstrap(Intent(admission));
  EXPECT_EQ(replay.canonical_empty_target, target);
  ExpectExactCanonicalEmptySnapshot(replay, admission);

  const auto absent =
      reopened.ProjectionBootstrapStatus(ProjectionBootstrapStatusQueryDTO{
          BootstrapId("bootstrap-foreign"), BootstrapGeneration(9U)});
  EXPECT_TRUE(absent.presence_valid());
  EXPECT_EQ(absent.state, ProjectionBootstrapStatusState::kAbsent);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeRebuildIsDurableIdempotentAndStatusComplete) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ProjectionBootstrapAcceptedDTO accepted = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    auto value = owner.BeginProjectionBootstrap(Intent(admission));
    const auto rebuilt =
        owner.RebuildProjectionBootstrap(RebuildRequest(value));
    Require(rebuilt.presence_valid(), "Rebuild result is invalid");
    Require(rebuilt.bootstrap_evidence.has_value(),
            "Rebuild result has no evidence");
    EXPECT_TRUE(rebuilt.bootstrap_evidence->rebuild_results.empty());
    EXPECT_TRUE(rebuilt.bootstrap_evidence->candidate_aggregate_health
                    .native_projection_health.empty());
    EXPECT_EQ(rebuilt.bootstrap_evidence->candidate_aggregate_health.state,
              core::ProjectionHealthState::kHealthy);
    const auto replay = owner.RebuildProjectionBootstrap(RebuildRequest(value));
    EXPECT_EQ(replay.canonical_empty_target, rebuilt.canonical_empty_target);
    owner.Close();
    return value;
  }();

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status = reopened.ProjectionBootstrapStatus(
      {accepted.bootstrap_id, accepted.bootstrap_generation});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kReadyToPublish);
  ASSERT_TRUE(status.bootstrap_evidence.has_value());
  EXPECT_TRUE(status.bootstrap_evidence->rebuild_results.empty());
  EXPECT_TRUE(status.native_rebuild_observations.empty());
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(reopened.ReadAccessStatus().projection_leases_open);
  const auto integrity = reopened.VerifyIntegrity();
  EXPECT_EQ(integrity.acceptance_row_count, 1);
  EXPECT_EQ(integrity.rebuild_row_count, 1);
  EXPECT_EQ(integrity.finalization_row_count, 0);
}

TEST(CanonicalEmptySceneOwner, NonzeroNativeBootstrapIsExplicitlyUnavailable) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  auto intent = Intent(admission);
  intent.native_runtime_bindings.push_back(ProjectionNativeRuntimeBindingDTO{
      core::CapabilityProviderBindingId("binding"),
      core::CapabilityProviderBindingRevision("binding-revision"),
      core::RuntimeUnitId("runtime"),
      core::RuntimeEpoch("epoch"),
  });
  EXPECT_THROW(owner.BeginProjectionBootstrap(intent),
               CanonicalEmptySceneOwnerConflictError);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwner,
     FinalizeRejectsStaleAndMismatchedHeadThenDurablyOpensGates) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ActiveBundlePublicationReceiptDTO exact_receipt = [&] {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
    static_cast<void>(
        owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
    auto receipt = PublicationReceipt(accepted);
    const auto finalize = FinalizeRequest(accepted, receipt);

    const auto stale = owner.FinalizeProjectionBootstrap(finalize);
    Require(stale.presence_valid(), "stale Finalize result is invalid");
    EXPECT_EQ(stale.state, ProjectionBootstrapFinalizeResultState::kStale);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

    auto mismatched = receipt;
    mismatched.durable_public_pointer_content_digest = Digest('d');
    publication_reader.SetCurrent(mismatched);
    const auto mismatch = owner.FinalizeProjectionBootstrap(finalize);
    Require(mismatch.presence_valid(), "mismatched Finalize result is invalid");
    EXPECT_EQ(mismatch.state, ProjectionBootstrapFinalizeResultState::kStale);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

    publication_reader.SetCurrent(receipt);
    const auto finalized = owner.FinalizeProjectionBootstrap(finalize);
    Require(finalized.presence_valid(),
            "successful Finalize result is invalid");
    EXPECT_EQ(finalized.state,
              ProjectionBootstrapFinalizeResultState::kFinalized);
    EXPECT_EQ(finalized.publication_receipt, receipt);
    const auto access = owner.ReadAccessStatus();
    EXPECT_TRUE(access.scene_writes_open);
    EXPECT_TRUE(access.projection_leases_open);
    Require(access.active_projection.has_value(),
            "finalized access has no active projection");
    EXPECT_EQ(*access.active_projection, accepted.canonical_empty_target);
    const auto snapshot = owner.ReadActiveSnapshot();
    Require(snapshot.has_value(), "finalized owner has no active snapshot");
    EXPECT_EQ(snapshot->shared()->identity(),
              accepted.initialized_logical_scene.shared()->identity());
    EXPECT_TRUE(snapshot->canonical_empty_for(
        accepted.initialized_logical_scene.shared()->identity()));
    EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 1);

    const auto observed =
        owner.ObserveProjectionBootstrapPrerequisites(owner.fixed_query());
    EXPECT_TRUE(observed.presence_valid());
    EXPECT_EQ(
        observed.state,
        ProjectionBootstrapPrerequisiteState::kConflictingActiveProjection);
    owner.Close();
    return receipt;
  }();

  publication_reader.Clear();
  const auto calls_before_reopen = publication_reader.calls();
  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  const auto status = reopened.ProjectionBootstrapStatus(
      {BootstrapId("bootstrap-alpha"), BootstrapGeneration(1U)});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionBootstrapStatusState::kFinalized);
  EXPECT_EQ(status.publication_receipt, exact_receipt);
  EXPECT_TRUE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(reopened.ReadAccessStatus().projection_leases_open);
  const auto reopened_snapshot = reopened.ReadActiveSnapshot();
  ASSERT_TRUE(reopened_snapshot.has_value());
  EXPECT_EQ(reopened_snapshot->shared()->revision,
            exact_receipt.request.candidate_projection_revision.scene_revision);
  ASSERT_TRUE(status.canonical_empty_target.has_value());
  const auto replay = reopened.FinalizeProjectionBootstrap({
      BootstrapId("bootstrap-alpha"),
      BootstrapGeneration(1U),
      *status.canonical_empty_target,
      exact_receipt,
  });
  EXPECT_EQ(replay.state, ProjectionBootstrapFinalizeResultState::kFinalized);
  EXPECT_EQ(publication_reader.calls(), calls_before_reopen);
}

TEST(CanonicalEmptySceneOwner,
     PublicationReadUncertaintyNeverOpensWritesOrLeases) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  static_cast<void>(owner.RebuildProjectionBootstrap(RebuildRequest(accepted)));
  const auto finalize = FinalizeRequest(accepted, PublicationReceipt(accepted));
  publication_reader.ReturnError(ErrorDTO{
      "internal_error",
      "durable active-bundle publication read failed",
      {},
  });
  const auto returned_error_result =
      owner.FinalizeProjectionBootstrap(finalize);
  EXPECT_TRUE(returned_error_result.presence_valid());
  EXPECT_EQ(returned_error_result.state,
            ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);

  publication_reader.ThrowOnRead();
  const auto thrown_result = owner.FinalizeProjectionBootstrap(finalize);
  EXPECT_TRUE(thrown_result.presence_valid());
  EXPECT_EQ(thrown_result.state,
            ProjectionBootstrapFinalizeResultState::kFailedProjectionUncertain);
  EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
  EXPECT_EQ(owner.VerifyIntegrity().finalization_row_count, 0);
}

TEST(CanonicalEmptySceneOwner,
     NamedApplyStageRequiresIndependentlyVerifiedBundleSource) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  const auto predecessor =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission)
          .canonical_empty_target;
  const auto request = NamedStageRequest(admission, predecessor);

  const auto result = owner.StageCandidateProjection(request);
  ASSERT_TRUE(result.presence_valid());
  EXPECT_EQ(result.state, ProjectionApplyStageResultState::kRejected);
  ASSERT_TRUE(result.error.has_value());
  EXPECT_EQ(result.error->code, "named_apply_retained_source_unavailable");
  EXPECT_FALSE(result.projection_stage_generation.has_value());
  EXPECT_FALSE(result.candidate_projection_revision.has_value());
  EXPECT_FALSE(result.transition_evidence.has_value());
  EXPECT_EQ(owner.VerifyIntegrity().named_apply_stage_count, 0);
  EXPECT_TRUE(owner.ReadAccessStatus().scene_writes_open);
  EXPECT_TRUE(owner.ReadAccessStatus().projection_leases_open);

  const auto status = owner.ProjectionApplyStatus(ProjectionApplyStatusQueryDTO{
      request.apply_id, request.apply_generation});
  EXPECT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionApplyStatusState::kAbsent);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeNamedApplyStageIsDurableReplayableAndClosesGates) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  ControlledPublicationReader publication_reader;
  std::optional<ProjectionApplyStageDTO> request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission, publication_reader,
                                   bundle_source);
    const auto predecessor =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    auto value = NamedStageRequest(admission, predecessor);
    request = value;

    const auto absent = owner.ProjectionApplyStatus(
        ProjectionApplyStatusQueryDTO{value.apply_id, value.apply_generation});
    ASSERT_TRUE(absent.presence_valid());
    EXPECT_EQ(absent.state, ProjectionApplyStatusState::kAbsent);

    const auto staged = owner.StageCandidateProjection(value);
    ASSERT_TRUE(staged.presence_valid());
    EXPECT_EQ(staged.state, ProjectionApplyStageResultState::kStaged);
    ASSERT_TRUE(staged.projection_stage_generation.has_value());
    EXPECT_EQ(staged.projection_stage_generation->value(), 1U);
    ASSERT_TRUE(staged.candidate_projection_revision.has_value());
    EXPECT_EQ(*staged.candidate_projection_revision, predecessor);
    ASSERT_TRUE(staged.transition_evidence.has_value());
    EXPECT_TRUE(staged.transition_evidence->transition_rows.empty());
    EXPECT_TRUE(staged.transition_evidence->matches_apply_phase(
        value.apply_id, value.apply_generation,
        *staged.projection_stage_generation, predecessor,
        *staged.candidate_projection_revision,
        core::ProjectionTransitionEvidenceState::kStaged));

    const auto access = owner.ReadAccessStatus();
    EXPECT_TRUE(access.valid());
    EXPECT_EQ(access.phase,
              CanonicalEmptySceneBootstrapPhase::kNamedApplyStaged);
    EXPECT_FALSE(access.scene_writes_open);
    EXPECT_FALSE(access.projection_leases_open);
    ASSERT_TRUE(access.active_projection.has_value());
    EXPECT_EQ(*access.active_projection, predecessor);
    EXPECT_EQ(owner.VerifyIntegrity().named_apply_stage_count, 1);

    const auto replay = owner.StageCandidateProjection(value);
    ASSERT_TRUE(replay.presence_valid());
    EXPECT_EQ(replay.state, ProjectionApplyStageResultState::kStaged);
    EXPECT_EQ(replay.projection_stage_generation,
              staged.projection_stage_generation);
    EXPECT_EQ(replay.candidate_projection_revision,
              staged.candidate_projection_revision);
    const auto conflict = owner.StageCandidateProjection(
        NamedStageRequest(admission, predecessor, "apply-beta"));
    EXPECT_EQ(conflict.state, ProjectionApplyStageResultState::kRejected);
    EXPECT_EQ(owner.VerifyIntegrity().named_apply_stage_count, 1);
    owner.Close();
  }

  ASSERT_TRUE(request.has_value());
  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source);
  const auto status =
      reopened.ProjectionApplyStatus(ProjectionApplyStatusQueryDTO{
          request->apply_id, request->apply_generation});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionApplyStatusState::kStaged);
  ASSERT_TRUE(status.projection_stage_generation.has_value());
  EXPECT_EQ(status.projection_stage_generation->value(), 1U);
  ASSERT_TRUE(status.candidate_projection_revision.has_value());
  ASSERT_TRUE(status.transition_evidence.has_value());
  EXPECT_TRUE(status.transition_evidence->transition_rows.empty());
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
  EXPECT_FALSE(reopened.ReadAccessStatus().projection_leases_open);
  const auto replay = reopened.StageCandidateProjection(*request);
  EXPECT_EQ(replay.state, ProjectionApplyStageResultState::kStaged);
  EXPECT_EQ(replay.projection_stage_generation,
            status.projection_stage_generation);
  EXPECT_EQ(replay.candidate_projection_revision,
            status.candidate_projection_revision);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeNamedApplyActivationAndFinalizeAreForwardOnlyAndDurable) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  ControlledPublicationReader publication_reader;
  std::optional<ProjectionApplyFinalizeDTO> finalize_request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission, publication_reader,
                                   bundle_source);
    const auto predecessor =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    const auto staged = owner.StageCandidateProjection(
        NamedStageRequest(admission, predecessor));
    ASSERT_EQ(staged.state, ProjectionApplyStageResultState::kStaged);
    const auto activation_request = ActivationRequest(staged);
    const auto activated = owner.ActivateStagedProjection(activation_request);
    ASSERT_TRUE(activated.presence_valid());
    EXPECT_EQ(activated.state,
              ProjectionApplyActivationResultState::kActivated);
    EXPECT_EQ(activated.acknowledged_candidate_projection_revision,
              staged.candidate_projection_revision);
    EXPECT_TRUE(activated.transition_evidence.transition_rows.empty());
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
    EXPECT_EQ(owner.ReadAccessStatus().phase,
              CanonicalEmptySceneBootstrapPhase::kNamedApplyActivated);
    const auto frozen_predecessor = owner.ReadActiveSnapshot();
    ASSERT_TRUE(frozen_predecessor.has_value());
    EXPECT_EQ(frozen_predecessor->shared()->revision,
              predecessor.scene_revision);

    const auto activation_replay =
        owner.ActivateStagedProjection(activation_request);
    EXPECT_EQ(activation_replay.state,
              ProjectionApplyActivationResultState::kActivated);
    const auto forbidden_discard =
        owner.DiscardStagedProjection(DiscardRequest(staged));
    ASSERT_TRUE(forbidden_discard.presence_valid());
    EXPECT_EQ(forbidden_discard.state,
              ProjectionApplyDiscardResultState::kFailedProjectionUncertain);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);

    const auto receipt = NamedPublicationReceipt(staged);
    finalize_request = NamedFinalizeRequest(staged, receipt);
    const auto stale = owner.FinalizeProjectionTransition(*finalize_request);
    ASSERT_TRUE(stale.presence_valid());
    EXPECT_EQ(stale.state, ProjectionApplyFinalizeResultState::kStale);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_EQ(owner.VerifyIntegrity().named_apply_finalization_count, 0);

    publication_reader.SetCurrent(receipt);
    const auto finalized =
        owner.FinalizeProjectionTransition(*finalize_request);
    ASSERT_TRUE(finalized.presence_valid());
    EXPECT_EQ(finalized.state, ProjectionApplyFinalizeResultState::kFinalized);
    EXPECT_EQ(finalized.publication_receipt, receipt);
    EXPECT_TRUE(finalized.transition_evidence.transition_rows.empty());
    EXPECT_TRUE(
        finalized.candidate_aggregate_health.native_projection_health.empty());
    const auto access = owner.ReadAccessStatus();
    EXPECT_TRUE(access.scene_writes_open);
    EXPECT_TRUE(access.projection_leases_open);
    EXPECT_EQ(access.phase,
              CanonicalEmptySceneBootstrapPhase::kNamedApplyFinalized);
    ASSERT_TRUE(access.active_projection.has_value());
    EXPECT_EQ(*access.active_projection, *staged.candidate_projection_revision);
    const auto selected_snapshot = owner.ReadActiveSnapshot();
    ASSERT_TRUE(selected_snapshot.has_value());
    EXPECT_EQ(selected_snapshot->shared()->revision,
              staged.candidate_projection_revision->scene_revision);
    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.named_apply_stage_count, 1);
    EXPECT_EQ(integrity.named_apply_activation_count, 1);
    EXPECT_EQ(integrity.named_apply_discard_count, 0);
    EXPECT_EQ(integrity.named_apply_finalization_count, 1);
    owner.Close();
  }

  ASSERT_TRUE(finalize_request.has_value());
  const auto calls_before_reopen = publication_reader.calls();
  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source);
  const auto status =
      reopened.ProjectionApplyStatus(ProjectionApplyStatusQueryDTO{
          finalize_request->apply_id, finalize_request->apply_generation});
  ASSERT_TRUE(status.presence_valid());
  EXPECT_EQ(status.state, ProjectionApplyStatusState::kFinalized);
  EXPECT_EQ(status.publication_receipt, finalize_request->publication_receipt);
  ASSERT_TRUE(status.candidate_aggregate_health.has_value());
  EXPECT_TRUE(
      status.candidate_aggregate_health->native_projection_health.empty());
  const auto replay = reopened.FinalizeProjectionTransition(*finalize_request);
  EXPECT_EQ(replay.state, ProjectionApplyFinalizeResultState::kFinalized);
  EXPECT_EQ(publication_reader.calls(), calls_before_reopen);
}

TEST(CanonicalEmptySceneOwner,
     PreselectionDiscardRestoresPredecessorAndAllowsNextApply) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  ControlledPublicationReader publication_reader;
  std::optional<ProjectionApplyStageDTO> second_request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission, publication_reader,
                                   bundle_source);
    const auto predecessor =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    const auto first = owner.StageCandidateProjection(
        NamedStageRequest(admission, predecessor));
    ASSERT_EQ(first.state, ProjectionApplyStageResultState::kStaged);
    const auto discard_request = DiscardRequest(first);
    const auto discarded = owner.DiscardStagedProjection(discard_request);
    ASSERT_TRUE(discarded.presence_valid());
    EXPECT_EQ(discarded.state, ProjectionApplyDiscardResultState::kDiscarded);
    EXPECT_TRUE(discarded.transition_evidence.transition_rows.empty());
    const auto discard_replay = owner.DiscardStagedProjection(discard_request);
    EXPECT_EQ(discard_replay.state,
              ProjectionApplyDiscardResultState::kDiscarded);
    const auto stale_activation =
        owner.ActivateStagedProjection(ActivationRequest(first));
    ASSERT_TRUE(stale_activation.presence_valid());
    EXPECT_EQ(stale_activation.state,
              ProjectionApplyActivationResultState::kStale);

    const auto access = owner.ReadAccessStatus();
    EXPECT_TRUE(access.scene_writes_open);
    EXPECT_TRUE(access.projection_leases_open);
    EXPECT_EQ(access.phase,
              CanonicalEmptySceneBootstrapPhase::kNamedApplyDiscarded);
    ASSERT_TRUE(access.active_projection.has_value());
    EXPECT_EQ(*access.active_projection, predecessor);

    second_request = NamedStageRequest(admission, predecessor, "apply-beta");
    const auto second = owner.StageCandidateProjection(*second_request);
    EXPECT_EQ(second.state, ProjectionApplyStageResultState::kStaged);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.named_apply_stage_count, 2);
    EXPECT_EQ(integrity.named_apply_activation_count, 0);
    EXPECT_EQ(integrity.named_apply_discard_count, 1);
    EXPECT_EQ(integrity.named_apply_finalization_count, 0);
    owner.Close();
  }

  ASSERT_TRUE(second_request.has_value());
  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source);
  const auto first_status = reopened.ProjectionApplyStatus(
      {ApplyId("apply-alpha"), ApplyGeneration(1U)});
  EXPECT_EQ(first_status.state, ProjectionApplyStatusState::kDiscarded);
  const auto second_status = reopened.ProjectionApplyStatus(
      {second_request->apply_id, second_request->apply_generation});
  EXPECT_EQ(second_status.state, ProjectionApplyStatusState::kStaged);
  EXPECT_FALSE(reopened.ReadAccessStatus().scene_writes_open);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsOrphanedNamedApplyPhaseRow) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission, publication_reader,
                                   bundle_source);
    const auto predecessor =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission)
            .canonical_empty_target;
    static_cast<void>(owner.StageCandidateProjection(
        NamedStageRequest(admission, predecessor)));
    owner.Close();
  }
  ExecuteSql(database_path,
             "INSERT INTO logical_scene_named_apply_activations("
             "apply_id,apply_generation,projection_stage_generation,"
             "transition_row_count,transition_evidence_state) "
             "VALUES('orphan-apply',1,1,0,'activated_and_retired')");
  EXPECT_THROW(CanonicalEmptySceneOwner(database_path, admission,
                                        publication_reader, bundle_source),
               CanonicalEmptySceneOwnerIntegrityError);
}

TEST(CanonicalEmptySceneOwner,
     LifecycleGateSerializesConstructionCloseAndDestruction) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  const auto database = directory.path() / "logical-scene.sqlite3";
  const auto make_owner = [&] {
    return std::make_unique<CanonicalEmptySceneOwner>(database, admission,
                                                      publication_reader);
  };

  ExpectActionAcquiresSceneLifecycleGate(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kStartupAttempt,
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kStartupAcquired,
      [&] {
        auto owner = make_owner();
        if (owner->VerifyIntegrity().acceptance_row_count != 0) {
          throw std::runtime_error(
              "fresh scene owner has an unexpected acceptance row");
        }
      });
  ExpectNoSqliteSidecars(database);

  auto explicitly_closed = make_owner();
  ExpectActionAcquiresSceneLifecycleGate(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kCloseAttempt,
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kCloseAcquired,
      [owner = explicitly_closed.get()] { owner->Close(); });
  ExpectNoSqliteSidecars(database);
  {
    auto reopened = make_owner();
    EXPECT_EQ(reopened->VerifyIntegrity().acceptance_row_count, 0);
  }

  auto destructed = make_owner();
  ExpectActionAcquiresSceneLifecycleGate(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kCloseAttempt,
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleEvent::
          kCloseAcquired,
      [owner = std::move(destructed)]() mutable { owner.reset(); });
  ExpectNoSqliteSidecars(database);
  {
    auto reopened = make_owner();
    EXPECT_EQ(reopened->VerifyIntegrity().acceptance_row_count, 0);
  }

  auto final_reopen = make_owner();
  EXPECT_EQ(final_reopen->VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwnerLifecycleGate,
     ContendedStartupDeadlineReturnsWithoutOwnership) {
  constexpr auto kTimeout = 50ms;
  sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleLock held(
      sqlite_lifecycle_internal::CanonicalEmptySceneStoreLifecycleMutex());
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  auto waiter = std::async(std::launch::async, [start] {
    start.get();
    const auto started = std::chrono::steady_clock::now();
    auto attempted = sqlite_lifecycle_internal::
        TryAcquireCanonicalEmptySceneStoreLifecycleLock(kTimeout);
    return std::pair{attempted.owns_lock(),
                     std::chrono::steady_clock::now() - started};
  });
  start_promise.set_value();

  const auto [owns_lock, elapsed] = waiter.get();
  held.unlock();
  EXPECT_FALSE(owns_lock);
  EXPECT_GE(elapsed, kTimeout);
}

TEST(CanonicalEmptySceneOwner, ConcurrentFirstOpenConvergesOneExactEmptyStore) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  const auto database = directory.path() / "logical-scene.sqlite3";
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  std::barrier open_together(4);
  std::vector<std::future<CanonicalEmptySceneStoreIntegrityReport>> openings;
  openings.reserve(4U);
  try {
    for (std::size_t index = 0U; index < 4U; ++index) {
      openings.push_back(std::async(std::launch::async, [&, index, start] {
        start.get();
        open_together.arrive_and_wait();
        CanonicalEmptySceneOwner owner(database, admission, publication_reader);
        const auto report = owner.VerifyIntegrity();
        if (index % 2U == 0U) {
          owner.Close();
        }
        return report;
      }));
    }
  } catch (...) {
    const auto launch_error = std::current_exception();
    start_promise.set_exception(launch_error);
    for (auto &opening : openings) {
      try {
        static_cast<void>(opening.get());
      } catch (...) {
      }
    }
    std::rethrow_exception(launch_error);
  }
  start_promise.set_value();

  std::string schema_digest;
  for (auto &opening : openings) {
    const auto report = opening.get();
    EXPECT_EQ(report.acceptance_row_count, 0);
    EXPECT_EQ(report.rebuild_row_count, 0);
    EXPECT_EQ(report.finalization_row_count, 0);
    if (schema_digest.empty()) {
      schema_digest = report.schema_artifact_digest;
    } else {
      EXPECT_EQ(report.schema_artifact_digest, schema_digest);
    }
  }
  ExpectNoSqliteSidecars(database);
  CanonicalEmptySceneOwner reopened(database, admission, publication_reader);
  EXPECT_EQ(reopened.VerifyIntegrity().acceptance_row_count, 0);
}

TEST(CanonicalEmptySceneOwner, ConcurrentExactBeginCommitsOneAcceptance) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner first(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  CanonicalEmptySceneOwner second(directory.path() / "logical-scene.sqlite3",
                                  admission, publication_reader);
  std::barrier start(3);
  const auto call = [&](CanonicalEmptySceneOwner &owner) {
    start.arrive_and_wait();
    return owner.BeginProjectionBootstrap(Intent(admission))
        .canonical_empty_target;
  };
  auto first_result = std::async(std::launch::async, call, std::ref(first));
  auto second_result = std::async(std::launch::async, call, std::ref(second));
  start.arrive_and_wait();
  EXPECT_EQ(first_result.get(), second_result.get());
  EXPECT_EQ(first.VerifyIntegrity().acceptance_row_count, 1);
  EXPECT_EQ(second.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner, ConcurrentConflictingBeginHasOneWinner) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner first(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  CanonicalEmptySceneOwner second(directory.path() / "logical-scene.sqlite3",
                                  admission, publication_reader);
  std::barrier start(3);
  const auto call = [&admission, &start](CanonicalEmptySceneOwner &owner,
                                         std::string bootstrap_id) {
    start.arrive_and_wait();
    try {
      static_cast<void>(owner.BeginProjectionBootstrap(
          Intent(admission, std::move(bootstrap_id))));
      return true;
    } catch (const CanonicalEmptySceneOwnerConflictError &) {
      return false;
    }
  };
  auto first_result =
      std::async(std::launch::async, call, std::ref(first), "bootstrap-alpha");
  auto second_result =
      std::async(std::launch::async, call, std::ref(second), "bootstrap-beta");
  start.arrive_and_wait();
  const auto first_won = first_result.get();
  const auto second_won = second_result.get();
  EXPECT_NE(first_won, second_won);
  EXPECT_EQ(first.VerifyIntegrity().acceptance_row_count, 1);
}

TEST(CanonicalEmptySceneOwner,
     StartupRejectsSemanticRowCorruptionWithIntactSchema) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    static_cast<void>(owner.BeginProjectionBootstrap(Intent(admission)));
    owner.Close();
  }
  ExecuteSql(
      database_path,
      "DROP TRIGGER canonical_empty_scene_acceptance_no_update;"
      "UPDATE canonical_empty_scene_acceptance "
      "SET scene_content_hash='sha256:"
      "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee';"
      "CREATE TRIGGER IF NOT EXISTS "
      "canonical_empty_scene_acceptance_no_update "
      "BEFORE UPDATE ON canonical_empty_scene_acceptance BEGIN "
      "SELECT RAISE(ABORT, 'canonical_empty_scene_acceptance is immutable');"
      "END;");
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerIntegrityError);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsOversizedDatabaseImage) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    owner.Close();
  }
  std::filesystem::resize_file(
      database_path,
      static_cast<std::uintmax_t>(kMaximumSceneStoreDatabasePageImageBytes) +
          1U);
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerError);
}

TEST(CanonicalEmptySceneOwner, StartupRejectsOversizedWalResidue) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto wal_path = std::filesystem::path(database_path.string() + "-wal");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    owner.Close();
  }
  {
    std::ofstream wal(wal_path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(wal.good());
  }
  std::filesystem::resize_file(
      wal_path, static_cast<std::uintmax_t>(kMaximumSceneStoreWalBytes) + 1U);
  EXPECT_THROW(
      CanonicalEmptySceneOwner(database_path, admission, publication_reader),
      CanonicalEmptySceneOwnerError);
}

TEST(CanonicalEmptySceneOwner, RestartsExactEmptyInitializationResidue) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  sqlite3 *empty_database = nullptr;
  ASSERT_EQ(sqlite3_open_v2(database_path.c_str(), &empty_database,
                            SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                                SQLITE_OPEN_FULLMUTEX,
                            nullptr),
            SQLITE_OK);
  ASSERT_NE(empty_database, nullptr);
  ASSERT_EQ(sqlite3_close(empty_database), SQLITE_OK);

  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(database_path, admission, publication_reader);
  const auto inventory = owner.VerifyIntegrity();
  EXPECT_EQ(inventory.acceptance_row_count, 0);
  EXPECT_EQ(inventory.rebuild_row_count, 0);
  EXPECT_EQ(inventory.finalization_row_count, 0);
  EXPECT_EQ(inventory.access.phase,
            CanonicalEmptySceneBootstrapPhase::kInitialCommandFenced);
  const auto accepted = owner.BeginProjectionBootstrap(Intent(admission));
  ExpectExactCanonicalEmptySnapshot(accepted, admission);
}

TEST(CanonicalEmptySceneOwner, RejectsOverBoundBootstrapIdentifier) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  EXPECT_THROW(
      owner.BeginProjectionBootstrap(Intent(
          admission, std::string(kMaximumSceneStoreIdentifierBytes + 1U, 'x'))),
      std::invalid_argument);
  EXPECT_EQ(owner.VerifyIntegrity().acceptance_row_count, 0);
}

core::SceneTransactionRequestDTO
AddBoxRequest(const ProjectionRevisionDTO &expected, const CellId &cell_id,
              std::string request_id = "scene-request-add-box") {
  core::Pose3DTO pose;
  pose.position_m = {0.25, -0.5, 0.75};
  return {
      core::PublicRequestContextDTO{
          core::RequestId(std::move(request_id)),
          core::RequestOwnerEpoch(expected.scene_revision.scene_id.value()),
          "scene-owner-test",
          std::nullopt,
      },
      {},
      expected,
      core::AddObjectRequestDTO{
          core::SceneResourceId("scene-box"),
          core::BoxShapeDTO{{0.1, 0.2, 0.3}},
          pose,
          core::CellSceneFrameRefDTO{cell_id},
      },
  };
}

TEST(LogicalSceneTransactionCodec,
     CanonicalInternalRequestRoundTripsAndRejectsWireDrift) {
  const ProjectionRevisionDTO expected{
      core::RetainedBundleId(Digest('b').value()),
      GraphRevision(Digest('c').value()),
      core::SceneRevision{
          core::SceneId(Digest('d').value()),
          1U,
          Digest('a'),
      },
  };
  auto request = AddBoxRequest(expected, CellId("cell-a"));
  request.attribution = {
      core::RequestTransportDTO::kHttp,
      core::RequestAssuranceDTO::kExternallyAuthenticated,
      std::string("operator.7"),
  };
  const auto encoded = logical_scene_codec_internal::EncodeRequest(request);
  const auto decoded = DecodeCanonicalSceneTransactionRequest(encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequest(decoded), encoded);
  EXPECT_EQ(decoded.request_context.request_id,
            request.request_context.request_id);
  EXPECT_EQ(decoded.attribution.principal_id, std::string("operator.7"));
  auto differently_attributed = request;
  differently_attributed.attribution = {
      core::RequestTransportDTO::kRos,
      core::RequestAssuranceDTO::kUnverified,
      std::nullopt,
  };
  EXPECT_NE(logical_scene_codec_internal::EncodeRequest(differently_attributed),
            encoded);
  EXPECT_EQ(logical_scene_codec_internal::EncodeRequestDigestInput(
                differently_attributed),
            logical_scene_codec_internal::EncodeRequestDigestInput(request));
  ASSERT_TRUE(
      std::holds_alternative<core::AddObjectRequestDTO>(decoded.operation));
  EXPECT_EQ(
      std::get<core::AddObjectRequestDTO>(decoded.operation).scene_resource_id,
      core::SceneResourceId("scene-box"));
  EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(" " + encoded),
               std::invalid_argument);
  auto unknown_field = encoded;
  ASSERT_EQ(unknown_field.back(), '}');
  unknown_field.pop_back();
  unknown_field += ",\"unknown\":true}";
  EXPECT_THROW(DecodeCanonicalSceneTransactionRequest(unknown_field),
               std::invalid_argument);
}

TEST(CanonicalEmptySceneOwner,
     ZeroNativeSceneCommitPublishesReplaysAndRestarts) {
  TemporaryDirectory directory;
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  ProjectionRevisionDTO committed_projection;
  std::optional<core::SceneTransactionRequestDTO> request;
  {
    CanonicalEmptySceneOwner owner(database_path, admission,
                                   publication_reader);
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    request =
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id());

    const auto committed = owner.Commit(*request);
    ASSERT_TRUE(committed.presence_valid());
    ASSERT_TRUE(committed.success);
    ASSERT_TRUE(committed.transaction_sequence.has_value());
    ASSERT_TRUE(committed.committed_projection_revision.has_value());
    ASSERT_TRUE(committed.update_status.has_value());
    EXPECT_EQ(*committed.transaction_sequence, 1U);
    EXPECT_EQ(*committed.update_status,
              core::SceneUpdateStatusDTO::kGeometryChanged);
    EXPECT_EQ(committed.projection_status,
              core::SceneProjectionCommitStatusDTO::kCommitted);
    EXPECT_EQ(
        committed.committed_projection_revision->scene_revision.revision_id,
        2U);
    committed_projection = *committed.committed_projection_revision;
    EXPECT_EQ(owner.CurrentProjectionRevision(), committed_projection);

    const auto objects = owner.GetSceneObjects();
    ASSERT_EQ(objects.size(), 1U);
    EXPECT_EQ(objects.front().scene_resource_id,
              core::SceneResourceId("scene-box"));
    const auto state = owner.GetObjectState(core::SceneResourceId("scene-box"));
    EXPECT_TRUE(state.exists);
    EXPECT_FALSE(state.attached);
    const auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
    ASSERT_TRUE(pose.ok());
    EXPECT_DOUBLE_EQ(pose.value().position_m.x_m, 0.25);

    auto relayed_request = *request;
    relayed_request.attribution = {
        core::RequestTransportDTO::kHttp,
        core::RequestAssuranceDTO::kExternallyAuthenticated,
        std::string("operator.7"),
    };
    const auto replay = owner.Commit(relayed_request);
    ASSERT_TRUE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(replay.transaction_sequence, committed.transaction_sequence);
    EXPECT_EQ(replay.committed_projection_revision,
              committed.committed_projection_revision);

    auto conflict = *request;
    std::get<core::AddObjectRequestDTO>(conflict.operation)
        .pose.position_m.x_m = 9.0;
    const auto rejected = owner.Commit(conflict);
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.replay_status,
              core::RequestReplayStatusDTO::kRequestIdConflict);
    ASSERT_TRUE(rejected.error.has_value());
    EXPECT_EQ(rejected.error->code, "request_id_conflict");

    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.transaction_intent_count, 1);
    EXPECT_EQ(integrity.request_outcome_count, 1);
    EXPECT_EQ(integrity.projection_attempt_count, 0);
    EXPECT_EQ(integrity.projection_result_count, 0);
    EXPECT_EQ(integrity.owner_event_count, 1);
    EXPECT_FALSE(integrity.projection_desynchronized);
    owner.Close();
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader);
  EXPECT_EQ(reopened.CurrentProjectionRevision(), committed_projection);
  const auto snapshot = reopened.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), 1U);
  EXPECT_EQ(snapshot->transaction_sequence, 1U);
  ASSERT_TRUE(request.has_value());
  const auto replay = reopened.Commit(*request);
  ASSERT_TRUE(replay.success);
  EXPECT_EQ(replay.replay_status, core::RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.committed_projection_revision, committed_projection);
}

TEST(CanonicalEmptySceneOwner, CommitsTheClosedPortableCollisionShapeSet) {
  TemporaryDirectory directory("scene-shape-set");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  auto projection = accepted.canonical_empty_target;
  const std::vector<std::pair<std::string, core::CollisionShapeDTO>> shapes{
      {"box", core::BoxShapeDTO{{0.1, 0.2, 0.3}}},
      {"sphere", core::SphereShapeDTO{0.05}},
      {"cylinder", core::CylinderShapeDTO{0.04, 0.2}},
      {"mesh", core::MeshShapeDTO{"package://manyforge_test/fixture.stl",
                                  {1.0, 1.0, 1.0}}},
  };
  for (const auto &[name, shape] : shapes) {
    auto request = AddBoxRequest(projection, admission.cell_id(),
                                 "scene-request-shape-" + name);
    auto &operation = std::get<core::AddObjectRequestDTO>(request.operation);
    operation.scene_resource_id = core::SceneResourceId("scene-" + name);
    operation.shape = shape;
    const auto result = owner.Commit(request);
    ASSERT_TRUE(result.success)
        << (result.error.has_value() ? result.error->message : "missing error");
    projection = *result.committed_projection_revision;
  }

  const auto snapshot = owner.LogicalSnapshot();
  ASSERT_EQ(snapshot->objects.size(), shapes.size());
  EXPECT_TRUE(std::holds_alternative<core::BoxShapeDTO>(
      snapshot->objects[0].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::CylinderShapeDTO>(
      snapshot->objects[1].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::MeshShapeDTO>(
      snapshot->objects[2].collision_shapes.front()));
  EXPECT_TRUE(std::holds_alternative<core::SphereShapeDTO>(
      snapshot->objects[3].collision_shapes.front()));
  EXPECT_EQ(snapshot->transaction_sequence, 4U);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 4);
}

core::PublicRequestContextDTO
SceneRequestContext(std::string request_id,
                    const ProjectionRevisionDTO &expected) {
  return {
      core::RequestId(std::move(request_id)),
      core::RequestOwnerEpoch(expected.scene_revision.scene_id.value()),
      "scene-owner-test",
      std::nullopt,
  };
}

class FakePlannerSceneProjection final : public core::IPlannerSceneProjection {
public:
  enum class Mode { kCommit, kRollbackProven, kUncertain, kMalformed };

  explicit FakePlannerSceneProjection(std::string identity)
      : binding_{
            core::CapabilityProviderBindingId("provider-" + identity),
            core::CapabilityProviderBindingRevision("binding-revision-" +
                                                    identity),
            core::RuntimeUnitId("runtime-" + identity),
            core::RuntimeEpoch("runtime-epoch-" + identity),
        } {}

  void Reset(const ProjectionRevisionDTO &target, const Mode mode) {
    mode_ = mode;
    calls_ = 0U;
    transactions_.clear();
    health_ = core::ProjectionHealthDTO{
        binding_,    1U,           core::ProjectionHealthState::kHealthy,
        target,      std::nullopt, 1U,
        Digest('8'), std::nullopt,
    };
  }

  [[nodiscard]] std::size_t calls() const noexcept { return calls_; }

  core::ProjectionCommitResultDTO
  ApplyTransaction(const core::SceneTransactionDTO &transaction) override {
    ++calls_;
    transactions_.push_back(transaction);
    ++health_.source_sequence;
    health_.native_projection_generation =
        *health_.native_projection_generation + 1U;
    if (mode_ == Mode::kCommit) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.proposed_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kCommitted,
          transaction.proposed_projection_revision,
          health_.native_projection_generation,
          false,
          health_,
          std::nullopt,
      };
    }
    const ErrorDTO rejected{
        mode_ == Mode::kRollbackProven ? "projection_rejected_rollback_proven"
                                       : "projection_state_uncertain",
        "injected native projection result",
        {},
    };
    if (mode_ == Mode::kRollbackProven) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.prior_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kRejectedRollbackProven,
          transaction.prior_projection_revision,
          health_.native_projection_generation,
          true,
          health_,
          rejected,
      };
    }
    if (mode_ == Mode::kMalformed) {
      health_.state = core::ProjectionHealthState::kHealthy;
      health_.active_projection_revision =
          transaction.prior_projection_revision;
      health_.error = std::nullopt;
      return {
          transaction.transaction_sequence,
          transaction.proposed_projection_revision,
          core::ProjectionCommitResultStateDTO::kCommitted,
          transaction.prior_projection_revision,
          health_.native_projection_generation,
          false,
          health_,
          std::nullopt,
      };
    }
    health_.state = core::ProjectionHealthState::kDesynchronized;
    health_.active_projection_revision = transaction.prior_projection_revision;
    health_.error = rejected;
    return {
        transaction.transaction_sequence,
        transaction.proposed_projection_revision,
        core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain,
        std::nullopt,
        health_.native_projection_generation,
        false,
        health_,
        rejected,
    };
  }

  core::ProjectionHealthDTO ProjectionHealth() const override {
    return health_;
  }

private:
  ProjectionNativeRuntimeBindingDTO binding_;
  Mode mode_{Mode::kCommit};
  core::ProjectionHealthDTO health_{
      binding_,
      1U,
      core::ProjectionHealthState::kUnavailable,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      ErrorDTO{"projection_not_initialized", "test projection unset", {}},
  };
  std::size_t calls_{};
  std::vector<core::SceneTransactionDTO> transactions_;
};

TEST(CanonicalEmptySceneOwner,
     ResolutionDependentRequestsFailBeforeJournalOrProjection) {
  TemporaryDirectory directory("scene-resolution-gate");
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  FakePlannerSceneProjection projection("resolution-gate");
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader, bundle_source,
                                 {&projection});
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  projection.Reset(accepted.canonical_empty_target,
                   FakePlannerSceneProjection::Mode::kCommit);

  auto component_frame =
      AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                    "scene-request-component-frame");
  std::get<core::AddObjectRequestDTO>(component_frame.operation).frame_ref =
      core::ComponentSceneFrameRefDTO{
          core::ControllableComponentId("component.1"),
          core::ModelInstanceId("model.1"), core::FrameRoleId("tool_frame")};
  const auto unresolved = owner.Commit(component_frame);
  ASSERT_FALSE(unresolved.success);
  ASSERT_TRUE(unresolved.error.has_value());
  EXPECT_EQ(unresolved.error->code, "scene_frame_resolution_unavailable");
  EXPECT_EQ(unresolved.error->details.at("reason"),
            "scene_frame_resolver_not_installed");

  auto foreign_cell =
      AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                    "scene-request-foreign-cell");
  std::get<core::AddObjectRequestDTO>(foreign_cell.operation).frame_ref =
      core::CellSceneFrameRefDTO{core::CellId("cell.foreign")};
  const auto missing = owner.Commit(foreign_cell);
  ASSERT_FALSE(missing.success);
  ASSERT_TRUE(missing.error.has_value());
  EXPECT_EQ(missing.error->code, "missing_reference");
  EXPECT_EQ(missing.error->details.at("reference_type"), "cell_frame");
  EXPECT_EQ(missing.error->details.at("reference_id"), "cell.foreign");

  core::SceneTransactionRequestDTO attach{
      SceneRequestContext("scene-request-attach",
                          accepted.canonical_empty_target),
      {},
      accepted.canonical_empty_target,
      core::AttachCarriedObjectRequestDTO{
          core::CarriedObjectAttachmentId("attachment.1"),
          core::SceneResourceId("scene-box"),
          core::ControllableComponentId("component.1"),
          core::CommandTargetId("arm"),
          core::FrameRoleId("tool_frame"),
          {},
          {},
      },
  };
  const auto attach_unavailable = owner.Commit(attach);
  ASSERT_FALSE(attach_unavailable.success);
  ASSERT_TRUE(attach_unavailable.error.has_value());
  EXPECT_EQ(attach_unavailable.error->code,
            "scene_frame_resolution_unavailable");

  auto policy = owner.LogicalSnapshot()->configured_collision_policy;
  policy.allowed_static_static_pairs.push_back(
      {core::CollisionBodyId("body.a"), core::CollisionBodyId("body.b")});
  core::SceneTransactionRequestDTO derived_policy{
      SceneRequestContext("scene-request-derived-policy",
                          accepted.canonical_empty_target),
      {},
      accepted.canonical_empty_target,
      core::SetCollisionPolicyRequestDTO{
          policy,
          owner.LogicalSnapshot()->configured_collision_policy.revision},
  };
  const auto policy_unavailable = owner.Commit(derived_policy);
  ASSERT_FALSE(policy_unavailable.success);
  ASSERT_TRUE(policy_unavailable.error.has_value());
  EXPECT_EQ(policy_unavailable.error->code,
            "scene_policy_derivation_unavailable");
  EXPECT_EQ(policy_unavailable.error->details.at("reason"),
            "collision_policy_resolver_not_installed");

  EXPECT_EQ(projection.calls(), 0U);
  const auto integrity = owner.VerifyIntegrity();
  EXPECT_EQ(integrity.transaction_intent_count, 0);
  EXPECT_EQ(integrity.request_outcome_count, 0);
  EXPECT_EQ(integrity.projection_attempt_count, 0);
}

TEST(CanonicalEmptySceneOwner,
     SceneUpdatesClassifyNoOpFreshnessGeometryAndPolicyEpochs) {
  TemporaryDirectory directory;
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                 admission, publication_reader);
  const auto accepted =
      FinalizeCanonicalBootstrap(owner, publication_reader, admission);
  const auto added = owner.Commit(
      AddBoxRequest(accepted.canonical_empty_target, admission.cell_id()));
  ASSERT_TRUE(added.success);
  auto projection = *added.committed_projection_revision;
  auto pose = owner.GetObjectPose(core::SceneResourceId("scene-box"));
  ASSERT_TRUE(pose.ok());

  const auto duplicate = owner.Commit(AddBoxRequest(
      projection, admission.cell_id(), "scene-request-duplicate"));
  ASSERT_FALSE(duplicate.success);
  ASSERT_TRUE(duplicate.error.has_value());
  EXPECT_EQ(duplicate.error->code, "object_already_exists");
  EXPECT_EQ(duplicate.error->details.at("scene_resource_id"), "scene-box");

  core::SceneTransactionRequestDTO missing_remove{
      SceneRequestContext("scene-request-missing-remove", projection),
      {},
      projection,
      core::RemoveObjectRequestDTO{core::SceneResourceId("missing-object")},
  };
  const auto missing_result = owner.Commit(missing_remove);
  ASSERT_FALSE(missing_result.success);
  ASSERT_TRUE(missing_result.error.has_value());
  EXPECT_EQ(missing_result.error->code, "object_not_found");
  EXPECT_EQ(missing_result.error->details.at("scene_resource_id"),
            "missing-object");

  core::SceneTransactionRequestDTO no_op{
      SceneRequestContext("scene-request-no-op", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          pose.value(),
          core::CellSceneFrameRefDTO{admission.cell_id()},
          std::nullopt,
          false,
      },
  };
  const auto unchanged = owner.Commit(no_op);
  ASSERT_TRUE(unchanged.success);
  EXPECT_EQ(unchanged.update_status, core::SceneUpdateStatusDTO::kNoOp);
  EXPECT_EQ(unchanged.committed_projection_revision, projection);
  auto snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 2U);
  EXPECT_EQ(snapshot->topology_epoch, 1U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  core::SceneObjectProviderEvidenceDTO evidence{
      core::ProviderInstanceId("provider-instance"),
      core::ProviderManifestId("provider-manifest"),
      core::ProviderManifestRevision(Digest('7').value()),
      "provider-epoch",
      1U,
      1000,
      900,
      5000,
  };
  core::SceneTransactionRequestDTO freshness{
      SceneRequestContext("scene-request-freshness", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          pose.value(),
          core::CellSceneFrameRefDTO{admission.cell_id()},
          evidence,
          false,
      },
  };
  const auto refreshed = owner.Commit(freshness);
  ASSERT_TRUE(refreshed.success)
      << (refreshed.error.has_value()
              ? refreshed.error->code + ": " + refreshed.error->message
              : "missing error");
  EXPECT_EQ(refreshed.update_status,
            core::SceneUpdateStatusDTO::kFreshnessOnly);
  projection = *refreshed.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 3U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 3U);
  EXPECT_EQ(snapshot->topology_epoch, 1U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  auto moved_pose = pose.value();
  moved_pose.position_m.y_m += 0.125;
  auto stale_evidence = evidence;
  core::SceneTransactionRequestDTO stale_provider{
      SceneRequestContext("scene-request-stale-provider", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          moved_pose,
          core::CellSceneFrameRefDTO{admission.cell_id()},
          stale_evidence,
          false,
      },
  };
  const auto stale_provider_result = owner.Commit(stale_provider);
  ASSERT_FALSE(stale_provider_result.success);
  ASSERT_TRUE(stale_provider_result.error.has_value());
  EXPECT_EQ(stale_provider_result.error->code, "scene_object_state_conflict");
  EXPECT_EQ(stale_provider_result.error->details.at("reason"),
            "provider_source_sequence_not_increasing");
  EXPECT_EQ(stale_provider_result.error->details.at(
                "previous_provider_source_sequence"),
            "1");
  EXPECT_EQ(stale_provider_result.error->details.at(
                "received_provider_source_sequence"),
            "1");

  auto next_evidence = evidence;
  next_evidence.provider_source_sequence = 2U;
  next_evidence.received_at_ns = 1100;
  core::SceneTransactionRequestDTO geometry{
      SceneRequestContext("scene-request-geometry", projection),
      {},
      projection,
      core::UpdateObjectPoseRequestDTO{
          core::SceneResourceId("scene-box"),
          moved_pose,
          core::CellSceneFrameRefDTO{admission.cell_id()},
          next_evidence,
          false,
      },
  };
  const auto moved = owner.Commit(geometry);
  ASSERT_TRUE(moved.success);
  EXPECT_EQ(moved.update_status, core::SceneUpdateStatusDTO::kGeometryChanged);
  projection = *moved.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 4U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 4U);
  EXPECT_EQ(snapshot->topology_epoch, 2U);
  EXPECT_EQ(snapshot->policy_epoch, 0U);

  auto policy = snapshot->configured_collision_policy;
  policy.policy_id = "strict-diagnostics";
  core::SceneTransactionRequestDTO policy_update{
      SceneRequestContext("scene-request-policy", projection),
      {},
      projection,
      core::SetCollisionPolicyRequestDTO{
          policy,
          snapshot->configured_collision_policy.revision,
      },
  };
  const auto policy_result = owner.Commit(policy_update);
  ASSERT_TRUE(policy_result.success);
  projection = *policy_result.committed_projection_revision;
  EXPECT_EQ(projection.scene_revision.revision_id, 5U);
  snapshot = owner.LogicalSnapshot();
  EXPECT_EQ(snapshot->transaction_sequence, 5U);
  EXPECT_EQ(snapshot->topology_epoch, 2U);
  EXPECT_EQ(snapshot->policy_epoch, 1U);
  EXPECT_EQ(snapshot->configured_collision_policy.policy_id,
            "strict-diagnostics");

  auto stale = geometry;
  stale.request_context.request_id = core::RequestId("scene-request-stale");
  const auto rejected = owner.Commit(stale);
  EXPECT_FALSE(rejected.success);
  ASSERT_TRUE(rejected.error.has_value());
  EXPECT_EQ(rejected.error->code, "stale_revision");
  EXPECT_EQ(rejected.projection_status,
            core::SceneProjectionCommitStatusDTO::kNotAttempted);
  EXPECT_EQ(owner.VerifyIntegrity().transaction_intent_count, 5);
}

TEST(CanonicalEmptySceneOwner,
     ProjectionCommitRollbackAndUncertaintyUseClosedDurableOutcomes) {
  const auto admission = LoadAdmission();
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);

  {
    TemporaryDirectory directory("scene-projection-commit");
    ControlledPublicationReader publication_reader;
    FakePlannerSceneProjection projection("commit");
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   {&projection});
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    projection.Reset(accepted.canonical_empty_target,
                     FakePlannerSceneProjection::Mode::kCommit);
    const auto request =
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                      "scene-request-native-commit");
    const auto result = owner.Commit(request);
    ASSERT_TRUE(result.success);
    EXPECT_EQ(projection.calls(), 1U);
    EXPECT_EQ(owner.VerifyIntegrity().projection_attempt_count, 1);
    EXPECT_EQ(owner.VerifyIntegrity().projection_result_count, 1);
    const auto replay = owner.Commit(request);
    EXPECT_TRUE(replay.success);
    EXPECT_EQ(projection.calls(), 1U);
  }

  {
    TemporaryDirectory directory("scene-projection-rollback");
    ControlledPublicationReader publication_reader;
    FakePlannerSceneProjection projection("rollback");
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   {&projection});
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    projection.Reset(accepted.canonical_empty_target,
                     FakePlannerSceneProjection::Mode::kRollbackProven);
    const auto request =
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                      "scene-request-native-rollback");
    const auto result = owner.Commit(request);
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kRejectedRollbackProven);
    EXPECT_TRUE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_TRUE(owner.GetSceneObjects().empty());
    const auto replay = owner.Commit(request);
    EXPECT_FALSE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(projection.calls(), 1U);
  }

  {
    TemporaryDirectory directory("scene-projection-uncertain");
    ControlledPublicationReader publication_reader;
    FakePlannerSceneProjection projection("uncertain");
    CanonicalEmptySceneOwner owner(directory.path() / "logical-scene.sqlite3",
                                   admission, publication_reader, bundle_source,
                                   {&projection});
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    projection.Reset(accepted.canonical_empty_target,
                     FakePlannerSceneProjection::Mode::kMalformed);
    const auto result = owner.Commit(
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                      "scene-request-native-uncertain"));
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain);
    EXPECT_FALSE(owner.ReadAccessStatus().scene_writes_open);
    EXPECT_FALSE(owner.ReadAccessStatus().projection_leases_open);
    EXPECT_TRUE(owner.VerifyIntegrity().projection_desynchronized);
    EXPECT_THROW(owner.LogicalSnapshot(),
                 CanonicalEmptySceneOwnerConflictError);
  }

  {
    TemporaryDirectory directory("scene-projection-partial");
    ControlledPublicationReader publication_reader;
    FakePlannerSceneProjection committed_projection("a-committed");
    FakePlannerSceneProjection rolled_back_projection("b-rolled-back");
    CanonicalEmptySceneOwner owner(
        directory.path() / "logical-scene.sqlite3", admission,
        publication_reader, bundle_source,
        {&rolled_back_projection, &committed_projection});
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    committed_projection.Reset(accepted.canonical_empty_target,
                               FakePlannerSceneProjection::Mode::kCommit);
    rolled_back_projection.Reset(
        accepted.canonical_empty_target,
        FakePlannerSceneProjection::Mode::kRollbackProven);
    const auto result = owner.Commit(
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                      "scene-request-native-partial"));
    ASSERT_FALSE(result.success);
    EXPECT_EQ(result.projection_status,
              core::SceneProjectionCommitStatusDTO::kFailedProjectionUncertain);
    EXPECT_EQ(committed_projection.calls(), 1U);
    EXPECT_EQ(rolled_back_projection.calls(), 1U);
    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.projection_attempt_count, 2);
    EXPECT_EQ(integrity.projection_result_count, 2);
    EXPECT_TRUE(integrity.projection_desynchronized);
  }
}

TEST(CanonicalEmptySceneOwner,
     RestartSettlesZeroNativeResidueAndFencesMissingNativeResult) {
  const auto admission = LoadAdmission();

  {
    TemporaryDirectory directory("scene-zero-native-recovery");
    const auto database_path = directory.path() / "logical-scene.sqlite3";
    ControlledPublicationReader publication_reader;
    std::optional<core::SceneTransactionRequestDTO> request;
    ProjectionRevisionDTO committed_projection;
    {
      CanonicalEmptySceneOwner owner(database_path, admission,
                                     publication_reader);
      const auto accepted =
          FinalizeCanonicalBootstrap(owner, publication_reader, admission);
      request =
          AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                        "scene-request-zero-recovery");
      const auto committed = owner.Commit(*request);
      ASSERT_TRUE(committed.success);
      committed_projection = *committed.committed_projection_revision;
      owner.Close();
    }
    ExecuteSql(database_path,
               "DROP TRIGGER logical_scene_request_outcomes_no_delete;"
               "DELETE FROM logical_scene_request_outcomes;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_request_outcomes_no_delete BEFORE DELETE ON "
               "logical_scene_request_outcomes BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_request_outcomes is immutable'); END;");

    CanonicalEmptySceneOwner recovered(database_path, admission,
                                       publication_reader);
    EXPECT_EQ(recovered.CurrentProjectionRevision(), committed_projection);
    ASSERT_TRUE(request.has_value());
    const auto replay = recovered.Commit(*request);
    ASSERT_TRUE(replay.success);
    EXPECT_EQ(replay.replay_status,
              core::RequestReplayStatusDTO::kReplayedExact);
    EXPECT_EQ(recovered.VerifyIntegrity().request_outcome_count, 1);
  }

  {
    TemporaryDirectory directory("scene-native-recovery");
    const auto database_path = directory.path() / "logical-scene.sqlite3";
    ControlledPublicationReader publication_reader;
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
    FakePlannerSceneProjection projection("recovery");
    {
      CanonicalEmptySceneOwner owner(database_path, admission,
                                     publication_reader, bundle_source,
                                     {&projection});
      const auto accepted =
          FinalizeCanonicalBootstrap(owner, publication_reader, admission);
      projection.Reset(accepted.canonical_empty_target,
                       FakePlannerSceneProjection::Mode::kCommit);
      const auto committed = owner.Commit(
          AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                        "scene-request-native-recovery"));
      ASSERT_TRUE(committed.success);
      owner.Close();
    }
    ExecuteSql(database_path,
               "DROP TRIGGER logical_scene_request_outcomes_no_delete;"
               "DROP TRIGGER logical_scene_projection_results_no_delete;"
               "DELETE FROM logical_scene_request_outcomes;"
               "DELETE FROM logical_scene_projection_results;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_request_outcomes_no_delete BEFORE DELETE ON "
               "logical_scene_request_outcomes BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_request_outcomes is immutable'); END;"
               "CREATE TRIGGER IF NOT EXISTS "
               "logical_scene_projection_results_no_delete BEFORE DELETE ON "
               "logical_scene_projection_results BEGIN SELECT RAISE(ABORT, "
               "'logical_scene_projection_results is immutable'); END;");

    CanonicalEmptySceneOwner recovered(database_path, admission,
                                       publication_reader, bundle_source,
                                       {&projection});
    const auto integrity = recovered.VerifyIntegrity();
    EXPECT_TRUE(integrity.projection_desynchronized);
    EXPECT_EQ(integrity.request_outcome_count, 1);
    EXPECT_EQ(integrity.projection_attempt_count, 1);
    EXPECT_EQ(integrity.projection_result_count, 0);
    EXPECT_FALSE(recovered.ReadAccessStatus().scene_writes_open);
    EXPECT_THROW(recovered.LogicalSnapshot(),
                 CanonicalEmptySceneOwnerConflictError);
  }
}

TEST(CanonicalEmptySceneOwner,
     SceneTransactionsAndNamedApplyReplayOneCompositeOrder) {
  TemporaryDirectory directory("scene-apply-interleave");
  const auto database_path = directory.path() / "logical-scene.sqlite3";
  const auto admission = LoadAdmission();
  ControlledPublicationReader publication_reader;
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  ProjectionRevisionDTO final_projection;
  {
    CanonicalEmptySceneOwner owner(database_path, admission, publication_reader,
                                   bundle_source);
    const auto accepted =
        FinalizeCanonicalBootstrap(owner, publication_reader, admission);
    const auto added = owner.Commit(
        AddBoxRequest(accepted.canonical_empty_target, admission.cell_id(),
                      "scene-request-before-apply"));
    ASSERT_TRUE(added.success);

    const auto staged = owner.StageCandidateProjection(
        NamedStageRequest(admission, *added.committed_projection_revision,
                          "apply-after-scene-edit"));
    ASSERT_EQ(staged.state, ProjectionApplyStageResultState::kStaged);
    const auto activated =
        owner.ActivateStagedProjection(ActivationRequest(staged));
    ASSERT_EQ(activated.state,
              ProjectionApplyActivationResultState::kActivated);
    const auto receipt = NamedPublicationReceipt(staged);
    publication_reader.SetCurrent(receipt);
    const auto finalized = owner.FinalizeProjectionTransition(
        NamedFinalizeRequest(staged, receipt));
    ASSERT_EQ(finalized.state, ProjectionApplyFinalizeResultState::kFinalized);

    core::SceneTransactionRequestDTO remove{
        SceneRequestContext("scene-request-after-apply",
                            finalized.candidate_projection_revision),
        {},
        finalized.candidate_projection_revision,
        core::RemoveObjectRequestDTO{core::SceneResourceId("scene-box")},
    };
    const auto removed = owner.Commit(remove);
    ASSERT_TRUE(removed.success);
    final_projection = *removed.committed_projection_revision;
    EXPECT_TRUE(owner.GetSceneObjects().empty());
    const auto integrity = owner.VerifyIntegrity();
    EXPECT_EQ(integrity.transaction_intent_count, 2);
    EXPECT_EQ(integrity.owner_event_count, 3);
    owner.Close();
  }

  CanonicalEmptySceneOwner reopened(database_path, admission,
                                    publication_reader, bundle_source);
  EXPECT_EQ(reopened.CurrentProjectionRevision(), final_projection);
  EXPECT_TRUE(reopened.GetSceneObjects().empty());
  EXPECT_EQ(reopened.LogicalSnapshot()->transaction_sequence, 2U);
  EXPECT_EQ(reopened.VerifyIntegrity().owner_event_count, 3);
}

} // namespace
} // namespace manyforge::scene

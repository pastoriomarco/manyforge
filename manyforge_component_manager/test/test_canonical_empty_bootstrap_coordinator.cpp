#include "manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp"

#include <gtest/gtest.h>

#include <barrier>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

using core::BootstrapGeneration;
using core::BootstrapId;
using core::ContentDigest;
using core::ErrorDTO;
using core::ProjectionBootstrapAcceptedDTO;
using core::ProjectionBootstrapFinalizeDTO;
using core::ProjectionBootstrapFinalizeResultDTO;
using core::ProjectionBootstrapIntentDTO;
using core::ProjectionBootstrapPrerequisiteQueryDTO;
using core::ProjectionBootstrapPrerequisiteState;
using core::ProjectionBootstrapPrerequisiteStatusDTO;
using core::ProjectionBootstrapRebuildRequestDTO;
using core::ProjectionBootstrapRebuildResultDTO;
using core::ProjectionBootstrapStatusDTO;
using core::ProjectionBootstrapStatusQueryDTO;
using core::ProjectionBootstrapStatusState;
using core::ProjectionRevisionDTO;
using core::SceneId;
using core::SceneRevision;

constexpr std::string_view kSceneDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kSceneIdDigest =
    "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
constexpr std::string_view kMaterializationDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

using BeginOrRecoverSignature =
    PersistedManagerBootstrapAcceptedTarget (
        CanonicalEmptyBootstrapCoordinator::*)(
        const deployment::CanonicalEmptyRetainedBundleAdmission&);
static_assert(std::is_same_v<
              decltype(
                  &CanonicalEmptyBootstrapCoordinator::BeginOrRecover),
              BeginOrRecoverSignature>);
static_assert(!std::is_constructible_v<
              ExternallyProvisionedManagerStores,
              std::filesystem::path,
              core::CellId>);
static_assert(std::is_constructible_v<
              ExternallyProvisionedManagerStores,
              std::filesystem::path,
              const deployment::
                  CanonicalEmptyRetainedBundleAdmission&>);

deployment::CanonicalEmptyRetainedBundleAdmission
LoadCanonicalEmptyAdmission() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_MANAGER_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return source.load_verified();
}

class TemporaryDirectory final {
 public:
  TemporaryDirectory() {
    auto pattern =
        (std::filesystem::temp_directory_path() /
         "manyforge-bootstrap-coordinator-test-XXXXXX")
            .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto* created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept {
    return path_;
  }

 private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

ProjectionBootstrapPrerequisiteQueryDTO Query(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  return {
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
}

ProjectionRevisionDTO Target(
    const ProjectionBootstrapPrerequisiteQueryDTO& query) {
  return {
      query.canonical_empty_retained_bundle_id,
      query.canonical_empty_graph_revision,
      SceneRevision{
          SceneId(std::string(kSceneIdDigest)),
          1U,
          ContentDigest(std::string(kSceneDigest)),
      },
  };
}

core::LogicalSceneSnapshotRoot CanonicalEmptyScene(
    const core::CellId& cell_id,
    const ProjectionRevisionDTO& target) {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {
      1U,
      std::string(core::kCanonicalEmptyCollisionPolicyDigest),
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              target.scene_revision,
              cell_id,
              0U,
              0U,
              0U,
              std::move(policy),
              {},
              {}}));
}

ManagerBootstrapIntentDTO DurableIntent(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  return {
      BootstrapId("bootstrap-0123456789abcdef0123456789abcdef"),
      BootstrapGeneration(1U),
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
      {},
  };
}

class InstrumentedSceneOwner final
    : public core::IManagerSceneProjectionBootstrapPort {
 public:
  enum class State {
    kAbsent,
    kAccepted,
    kRebuilding,
    kUncertain,
  };

  explicit InstrumentedSceneOwner(
      ProjectionBootstrapPrerequisiteQueryDTO expected_query)
      : expected_query_(std::move(expected_query)),
        target_(Target(expected_query_)) {}

  void SetUnavailableObservation(const std::size_t observation) {
    std::lock_guard lock(mutex_);
    unavailable_observation_ = observation;
  }

  void SetMismatchedObservation(const std::size_t observation) {
    std::lock_guard lock(mutex_);
    mismatched_observation_ = observation;
  }

  void SetConflictingObservation(const std::size_t observation) {
    std::lock_guard lock(mutex_);
    conflicting_observation_ = observation;
  }

  void SetTargetGraphMismatch() {
    std::lock_guard lock(mutex_);
    target_.graph_revision =
        core::GraphRevision("different-graph-revision");
  }

  void LoseNextAcceptedResponse() {
    std::lock_guard lock(mutex_);
    lose_next_accepted_response_ = true;
  }

  void FailNextBeginBeforeEffect() {
    std::lock_guard lock(mutex_);
    fail_next_begin_before_effect_ = true;
  }

  void SetBeginReturnHook(std::function<void()> hook) {
    std::lock_guard lock(mutex_);
    begin_return_hook_ = std::move(hook);
  }

  void SetStatusIdentityMismatch(const bool mismatch) {
    std::lock_guard lock(mutex_);
    status_identity_mismatch_ = mismatch;
  }

  void SetState(const State state) {
    std::lock_guard lock(mutex_);
    state_ = state;
  }

  void SetInitialObservationBarrier(std::barrier<>* barrier) {
    std::lock_guard lock(mutex_);
    initial_observation_barrier_ = barrier;
  }

  [[nodiscard]] std::size_t begin_calls() const {
    std::lock_guard lock(mutex_);
    return begin_calls_;
  }

  [[nodiscard]] std::size_t accept_effect_count() const {
    std::lock_guard lock(mutex_);
    return accept_effect_count_;
  }

  [[nodiscard]] std::size_t rebuild_calls() const {
    std::lock_guard lock(mutex_);
    return rebuild_calls_;
  }

  [[nodiscard]] std::size_t finalize_calls() const {
    std::lock_guard lock(mutex_);
    return finalize_calls_;
  }

  [[nodiscard]] std::size_t observation_calls() const {
    std::lock_guard lock(mutex_);
    return observation_calls_;
  }

  [[nodiscard]] std::vector<std::string> events() const {
    std::lock_guard lock(mutex_);
    return events_;
  }

  [[nodiscard]] ProjectionBootstrapPrerequisiteStatusDTO
  ObserveProjectionBootstrapPrerequisites(
      const ProjectionBootstrapPrerequisiteQueryDTO& query) const override {
    std::barrier<>* barrier = nullptr;
    std::size_t call = 0U;
    {
      std::lock_guard lock(mutex_);
      call = ++observation_calls_;
      events_.push_back("observe");
      if (call <= 2U) {
        barrier = initial_observation_barrier_;
      }
    }
    if (barrier != nullptr) {
      barrier->arrive_and_wait();
    }

    std::lock_guard lock(mutex_);
    auto echoed_query = query;
    if (mismatched_observation_ == call) {
      echoed_query = ProjectionBootstrapPrerequisiteQueryDTO{
          core::CellId("different-cell"),
          query.canonical_empty_retained_bundle_id,
          query.canonical_empty_graph_revision,
      };
    }
    if (unavailable_observation_ == call) {
      return {
          echoed_query,
          ProjectionBootstrapPrerequisiteState::kUnavailable,
          false,
          false,
          std::nullopt,
          ErrorDTO{
              "bootstrap_prerequisite_unavailable",
              "simulated unavailable prerequisite",
              {},
          },
      };
    }
    if (conflicting_observation_ == call) {
      return {
          echoed_query,
          ProjectionBootstrapPrerequisiteState::
              kConflictingActiveProjection,
          false,
          false,
          target_,
          ErrorDTO{
              "bootstrap_active_projection_conflict",
              "simulated active projection conflict",
              {},
          },
      };
    }
    return {
        echoed_query,
        ProjectionBootstrapPrerequisiteState::kReadyCommandFenced,
        true,
        true,
        std::nullopt,
        std::nullopt,
    };
  }

  [[nodiscard]] ProjectionBootstrapAcceptedDTO
  BeginProjectionBootstrap(
      const ProjectionBootstrapIntentDTO& intent) override {
    std::lock_guard lock(mutex_);
    ++begin_calls_;
    events_.push_back("begin");
    if (fail_next_begin_before_effect_) {
      fail_next_begin_before_effect_ = false;
      throw std::runtime_error("simulated Begin failure before effect");
    }
    if (!intent.valid() || !intent.native_runtime_bindings.empty() ||
        intent.cell_id != expected_query_.cell_id ||
        intent.canonical_empty_retained_bundle.retained_bundle_id !=
            expected_query_.canonical_empty_retained_bundle_id ||
        intent.canonical_empty_retained_bundle.cell_graph.graph_revision !=
            expected_query_.canonical_empty_graph_revision) {
      throw std::runtime_error("fake scene owner rejected invalid intent");
    }
    if (state_ != State::kAbsent &&
        (accepted_bootstrap_id_ != intent.bootstrap_id ||
         accepted_generation_ != intent.bootstrap_generation)) {
      throw std::runtime_error("fake scene owner rejected conflicting Begin");
    }
    if (state_ == State::kAbsent) {
      ++accept_effect_count_;
    }
    accepted_bootstrap_id_ = intent.bootstrap_id;
    accepted_generation_ = intent.bootstrap_generation;
    state_ = State::kAccepted;
    ProjectionBootstrapAcceptedDTO accepted{
        intent.bootstrap_id,
        intent.bootstrap_generation,
        intent.cell_id,
        target_,
        CanonicalEmptyScene(intent.cell_id, target_),
    };
    if (lose_next_accepted_response_) {
      lose_next_accepted_response_ = false;
      throw std::runtime_error("simulated accepted Begin response loss");
    }
    if (begin_return_hook_) {
      begin_return_hook_();
    }
    return accepted;
  }

  [[nodiscard]] ProjectionBootstrapRebuildResultDTO
  RebuildProjectionBootstrap(
      const ProjectionBootstrapRebuildRequestDTO&) override {
    std::lock_guard lock(mutex_);
    ++rebuild_calls_;
    events_.push_back("rebuild");
    throw std::logic_error("Rebuild is outside this bounded test slice");
  }

  [[nodiscard]] ProjectionBootstrapStatusDTO ProjectionBootstrapStatus(
      const ProjectionBootstrapStatusQueryDTO& query) const override {
    std::lock_guard lock(mutex_);
    events_.push_back("status");
    auto bootstrap_id = query.bootstrap_id;
    if (status_identity_mismatch_) {
      bootstrap_id = BootstrapId("bootstrap-fedcba9876543210fedcba9876543210");
    }
    switch (state_) {
      case State::kAbsent:
        return {
            bootstrap_id,
            query.bootstrap_generation,
            ProjectionBootstrapStatusState::kAbsent,
            std::nullopt,
            {},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        };
      case State::kAccepted:
        return {
            bootstrap_id,
            query.bootstrap_generation,
            ProjectionBootstrapStatusState::kAccepted,
            target_,
            {},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        };
      case State::kRebuilding:
        return {
            bootstrap_id,
            query.bootstrap_generation,
            ProjectionBootstrapStatusState::kRebuilding,
            target_,
            {},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            std::nullopt,
        };
      case State::kUncertain:
        return {
            bootstrap_id,
            query.bootstrap_generation,
            ProjectionBootstrapStatusState::kFailedProjectionUncertain,
            target_,
            {},
            std::nullopt,
            std::nullopt,
            std::nullopt,
            ErrorDTO{
                "bootstrap_projection_uncertain",
                "simulated uncertainty",
                {},
            },
        };
    }
    throw std::logic_error("unreachable fake scene-owner state");
  }

  [[nodiscard]] ProjectionBootstrapFinalizeResultDTO
  FinalizeProjectionBootstrap(
      const ProjectionBootstrapFinalizeDTO&) override {
    std::lock_guard lock(mutex_);
    ++finalize_calls_;
    events_.push_back("finalize");
    throw std::logic_error("Finalize is outside this bounded test slice");
  }

 private:
  ProjectionBootstrapPrerequisiteQueryDTO expected_query_;
  ProjectionRevisionDTO target_;
  mutable std::mutex mutex_;
  mutable std::size_t observation_calls_{0U};
  mutable std::size_t begin_calls_{0U};
  mutable std::size_t accept_effect_count_{0U};
  mutable std::size_t rebuild_calls_{0U};
  mutable std::size_t finalize_calls_{0U};
  mutable std::vector<std::string> events_;
  mutable State state_{State::kAbsent};
  mutable BootstrapId accepted_bootstrap_id_{"not-accepted"};
  mutable BootstrapGeneration accepted_generation_{0U};
  mutable std::size_t unavailable_observation_{
      std::numeric_limits<std::size_t>::max()};
  mutable std::size_t mismatched_observation_{
      std::numeric_limits<std::size_t>::max()};
  mutable std::size_t conflicting_observation_{
      std::numeric_limits<std::size_t>::max()};
  mutable bool lose_next_accepted_response_{false};
  mutable bool fail_next_begin_before_effect_{false};
  mutable bool status_identity_mismatch_{false};
  mutable std::function<void()> begin_return_hook_;
  mutable std::barrier<>* initial_observation_barrier_{nullptr};
};

core::ActiveBundlePublicationRequestDTO PublicationRequest(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  const auto target = Target(Query(admission));
  return {
      core::BootstrapPublicationTransactionDTO{
          BootstrapId("already-published"),
          BootstrapGeneration(1U),
      },
      target,
      admission.retained_bundle_id(),
      admission.graph_revision(),
      std::nullopt,
      ContentDigest(std::string(kMaterializationDigest)),
  };
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    FreshBeginUsesTwoPrerequisiteObservationsAndPersistsExactEmptyIntent) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  const auto result = coordinator.BeginOrRecover(admission);

  const auto snapshot = stores.bootstrap_journal().ReadSnapshot();
  ASSERT_EQ(
      snapshot.phase,
      ManagerBootstrapJournalPhase::kAcceptedTargetPersisted);
  ASSERT_TRUE(snapshot.intent.has_value());
  ASSERT_TRUE(snapshot.accepted_target.has_value());
  EXPECT_EQ(result, *snapshot.accepted_target);
  EXPECT_EQ(snapshot.intent->intent.bootstrap_generation.value(), 1U);
  EXPECT_TRUE(snapshot.intent->intent.native_runtime_bindings.empty());
  EXPECT_EQ(
      snapshot.intent->intent.bootstrap_id.value().size(),
      std::string_view("bootstrap-").size() + 32U);
  EXPECT_EQ(
      scene_owner.events(),
      (std::vector<std::string>{"observe", "status", "observe", "begin"}));
  EXPECT_EQ(scene_owner.rebuild_calls(), 0U);
  EXPECT_EQ(scene_owner.finalize_calls(), 0U);
  EXPECT_EQ(
      stores.StartupInventory().active_bundle_publication.receipt_count,
      0);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    ExactAcceptedReplayReturnsTheSameDurableTargetWithoutAnotherBegin) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);
  const auto first = coordinator.BeginOrRecover(admission);
  const auto begin_calls = scene_owner.begin_calls();

  const auto replay = coordinator.BeginOrRecover(admission);

  EXPECT_EQ(replay, first);
  EXPECT_EQ(scene_owner.begin_calls(), begin_calls);
  EXPECT_EQ(scene_owner.rebuild_calls(), 0U);
  EXPECT_EQ(scene_owner.finalize_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    AcceptedResponseLossRecoversOnlyThroughExactStatus) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.LoseNextAcceptedResponse();
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  const auto result = coordinator.BeginOrRecover(admission);

  EXPECT_EQ(
      result,
      *stores.bootstrap_journal().ReadSnapshot().accepted_target);
  EXPECT_EQ(
      scene_owner.events(),
      (std::vector<std::string>{
          "observe", "status", "observe", "begin", "status"}));
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    PrerequisiteFailureBeforeMintLeavesJournalEmpty) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.SetUnavailableObservation(1U);
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      CanonicalEmptyBootstrapCoordinatorError);

  EXPECT_EQ(
      stores.bootstrap_journal().ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kEmpty);
  EXPECT_EQ(scene_owner.begin_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    MismatchedEchoAndConflictingActivePrerequisitesLeaveJournalEmpty) {
  for (const bool conflicting : {false, true}) {
    TemporaryDirectory directory;
    const auto admission = LoadCanonicalEmptyAdmission();
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    InstrumentedSceneOwner scene_owner(Query(admission));
    if (conflicting) {
      scene_owner.SetConflictingObservation(1U);
    } else {
      scene_owner.SetMismatchedObservation(1U);
    }
    CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

    EXPECT_THROW(
        static_cast<void>(coordinator.BeginOrRecover(admission)),
        CanonicalEmptyBootstrapCoordinatorError);
    EXPECT_EQ(
        stores.bootstrap_journal().ReadSnapshot().phase,
        ManagerBootstrapJournalPhase::kEmpty);
    EXPECT_EQ(scene_owner.begin_calls(), 0U);
  }
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    PrerequisiteDriftAfterAbsentStatusPersistsIntentButDoesNotBegin) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.SetUnavailableObservation(2U);
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      CanonicalEmptyBootstrapCoordinatorError);

  EXPECT_EQ(
      stores.bootstrap_journal().ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
  EXPECT_EQ(scene_owner.begin_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    DurableIntentWithAbsentOwnerReobservesAndBeginsExactReplay) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  const auto intent = DurableIntent(admission);
  ASSERT_TRUE(stores.bootstrap_journal().EnsureIntent(intent).inserted);
  InstrumentedSceneOwner scene_owner(Query(admission));
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  const auto result = coordinator.BeginOrRecover(admission);

  EXPECT_EQ(result.accepted_target.bootstrap_id, intent.bootstrap_id);
  EXPECT_EQ(
      scene_owner.events(),
      (std::vector<std::string>{"status", "observe", "begin"}));
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    BeginFailureBeforeEffectKeepsUsefulContextAndLeavesIntentForRetry) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.FailNextBeginBeforeEffect();
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  try {
    static_cast<void>(coordinator.BeginOrRecover(admission));
    FAIL() << "expected Begin failure";
  } catch (const CanonicalEmptyBootstrapCoordinatorError& error) {
    EXPECT_NE(
        std::string(error.what()).find(
            "simulated Begin failure before effect"),
        std::string::npos);
  }

  EXPECT_EQ(
      stores.bootstrap_journal().ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    DurableRecordFailureAfterAcceptedReturnIsNotReclassifiedAsResponseLoss) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.SetBeginReturnHook(
      [&stores] { stores.bootstrap_journal().Close(); });
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      ManagerBootstrapJournalClosedError);

  EXPECT_EQ(
      scene_owner.events(),
      (std::vector<std::string>{"observe", "status", "observe", "begin"}));
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    MismatchedAcceptedTargetFailsWithoutDurableAcceptance) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.SetTargetGraphMismatch();
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      CanonicalEmptyBootstrapCoordinatorError);

  EXPECT_EQ(
      stores.bootstrap_journal().ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kIntentPersisted);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    LaterStateBeforeDurableAcceptanceFailsClosed) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  ASSERT_TRUE(
      stores.bootstrap_journal()
          .EnsureIntent(DurableIntent(admission))
          .inserted);
  InstrumentedSceneOwner scene_owner(Query(admission));
  scene_owner.SetState(InstrumentedSceneOwner::State::kRebuilding);
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      CanonicalEmptyBootstrapCoordinatorError);
  EXPECT_EQ(scene_owner.begin_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    DurableAcceptedTargetAllowsExactLaterNonUncertainStatus) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);
  const auto accepted = coordinator.BeginOrRecover(admission);
  scene_owner.SetState(InstrumentedSceneOwner::State::kRebuilding);

  const auto recovered = coordinator.BeginOrRecover(admission);

  EXPECT_EQ(recovered, accepted);
  EXPECT_EQ(scene_owner.begin_calls(), 1U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    StoreReopenRecoversExactDurableAcceptanceWithoutAnotherBegin) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  PersistedManagerBootstrapAcceptedTarget accepted = [&] {
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    InstrumentedSceneOwner scene_owner(Query(admission));
    CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);
    return coordinator.BeginOrRecover(admission);
  }();

  ExternallyProvisionedManagerStores reopened(directory.path(), admission);
  InstrumentedSceneOwner recovered_owner(Query(admission));
  recovered_owner.SetState(InstrumentedSceneOwner::State::kAccepted);
  CanonicalEmptyBootstrapCoordinator recovered(reopened, recovered_owner);

  const auto replay = recovered.BeginOrRecover(admission);

  EXPECT_EQ(replay, accepted);
  EXPECT_EQ(recovered_owner.begin_calls(), 0U);
  EXPECT_EQ(recovered_owner.rebuild_calls(), 0U);
  EXPECT_EQ(recovered_owner.finalize_calls(), 0U);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    DurableAcceptedTargetRejectsAbsentMismatchAndUncertainStatus) {
  const auto exercise = [](
                            const InstrumentedSceneOwner::State state,
                            const bool mismatch) {
    TemporaryDirectory directory;
    const auto admission = LoadCanonicalEmptyAdmission();
    ExternallyProvisionedManagerStores stores(directory.path(), admission);
    InstrumentedSceneOwner scene_owner(Query(admission));
    CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);
    static_cast<void>(coordinator.BeginOrRecover(admission));
    scene_owner.SetState(state);
    scene_owner.SetStatusIdentityMismatch(mismatch);
    EXPECT_THROW(
        static_cast<void>(coordinator.BeginOrRecover(admission)),
        CanonicalEmptyBootstrapCoordinatorError);
  };

  exercise(InstrumentedSceneOwner::State::kAbsent, false);
  exercise(InstrumentedSceneOwner::State::kAccepted, true);
  exercise(InstrumentedSceneOwner::State::kUncertain, false);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    NonEmptyPublicationHeadFailsBeforeAnySceneOwnerCall) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  static_cast<void>(
      stores.active_bundle_publication_store().PublishActiveBundle(
          PublicationRequest(admission)));
  InstrumentedSceneOwner scene_owner(Query(admission));
  CanonicalEmptyBootstrapCoordinator coordinator(stores, scene_owner);

  EXPECT_THROW(
      static_cast<void>(coordinator.BeginOrRecover(admission)),
      CanonicalEmptyBootstrapCoordinatorError);

  EXPECT_TRUE(scene_owner.events().empty());
  EXPECT_EQ(
      stores.bootstrap_journal().ReadSnapshot().phase,
      ManagerBootstrapJournalPhase::kEmpty);
}

TEST(
    CanonicalEmptyBootstrapCoordinator,
    ConcurrentEmptyReadersReuseOneDurableWinner) {
  TemporaryDirectory directory;
  const auto admission = LoadCanonicalEmptyAdmission();
  ExternallyProvisionedManagerStores stores(directory.path(), admission);
  InstrumentedSceneOwner scene_owner(Query(admission));
  std::barrier initial_observations(2);
  scene_owner.SetInitialObservationBarrier(&initial_observations);
  CanonicalEmptyBootstrapCoordinator first(stores, scene_owner);
  CanonicalEmptyBootstrapCoordinator second(stores, scene_owner);

  auto first_result = std::async(
      std::launch::async,
      [&] { return first.BeginOrRecover(admission); });
  auto second_result = std::async(
      std::launch::async,
      [&] { return second.BeginOrRecover(admission); });

  const auto first_value = first_result.get();
  const auto second_value = second_result.get();
  EXPECT_EQ(first_value, second_value);
  const auto snapshot = stores.bootstrap_journal().ReadSnapshot();
  ASSERT_TRUE(snapshot.intent.has_value());
  ASSERT_TRUE(snapshot.accepted_target.has_value());
  EXPECT_EQ(first_value, *snapshot.accepted_target);
  EXPECT_TRUE(snapshot.intent->intent.native_runtime_bindings.empty());
  EXPECT_EQ(scene_owner.accept_effect_count(), 1U);
  EXPECT_EQ(scene_owner.rebuild_calls(), 0U);
  EXPECT_EQ(scene_owner.finalize_calls(), 0U);
}

}  // namespace
}  // namespace manyforge::component_manager

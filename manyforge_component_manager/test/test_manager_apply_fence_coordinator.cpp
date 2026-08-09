

#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <future>
#include <limits>
#include <manyforge_deployment/apply_affected_scope.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "manyforge_component_manager/manager_apply_fence_coordinator.hpp"

#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kSceneDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kSceneIdDigest =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kEvidenceDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-apply-fence-coordinator-test-XXXXXX")
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

class SealedBundleFixture final {
public:
  SealedBundleFixture() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-apply-fence-bundle-test-XXXXXX")
                       .string();
    std::vector<char> buffer(pattern.begin(), pattern.end());
    buffer.push_back('\0');
    const auto *created = mkdtemp(buffer.data());
    if (created == nullptr) {
      throw std::runtime_error("bundle fixture mkdtemp failed");
    }
    path_ = created;
    const auto fixture =
        manyforge::deployment::test::make_runtime_only_retention_fixture();
    const manyforge::deployment::test::RuntimeOnlyCompilerInputArtifactSource
        input_source(fixture.runtime_profile);
    manyforge::deployment::FilesystemBuildManifestRepository manifests(path_);
    manyforge::deployment::FilesystemCatalogArtifactStore catalogs(path_);
    manyforge::deployment::FilesystemRawArtifactStore raw_artifacts(path_);
    manyforge::deployment::FilesystemRetainedBundleRepository bundles(path_);
    bundle_ = manyforge::deployment::detail::RetainedBundleMaterializer::
        materialize_store_and_reload_production(fixture.qualified, manifests,
                                                input_source, {}, catalogs,
                                                raw_artifacts, bundles);
    if (bundle_ == nullptr) {
      throw std::runtime_error("bundle fixture materialization failed");
    }
  }

  ~SealedBundleFixture() {
    bundle_.reset();
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const manyforge::deployment::RetainedBundle &bundle() const {
    return *bundle_;
  }

private:
  std::filesystem::path path_;
  std::shared_ptr<const manyforge::deployment::RetainedBundle> bundle_;
};

[[nodiscard]] const manyforge::deployment::RetainedBundle &TestBundle() {
  static const SealedBundleFixture fixture;
  return fixture.bundle();
}

[[nodiscard]] CellId TestCellId() {
  return TestBundle().value().cell_graph.cell_id;
}

[[nodiscard]] ManagerApplyIntentDTO
ApplyIntent(std::string apply_id = "apply-1",
            const std::uint64_t apply_generation = 1U,
            std::string request_id = "request-1") {
  const auto &bundle = TestBundle();
  const auto &graph = bundle.value().cell_graph;
  return ManagerApplyIntentDTO{
      PublicRequestContextDTO{
          RequestId(std::move(request_id)),
          RequestOwnerEpoch("manager-owner-epoch-1"),
          "component-manager-test",
          std::optional<std::string>("correlation-1"),
      },
      ApplyId(std::move(apply_id)),
      ApplyGeneration(apply_generation),
      graph.cell_id,
      ProjectionRevisionDTO{
          bundle.retained_bundle_id(),
          graph.graph_revision,
          SceneRevision{
              SceneId(std::string(kSceneIdDigest)),
              7U,
              ContentDigest(std::string(kSceneDigest)),
          },
      },
      bundle.retained_bundle_id(),
      graph.graph_revision,
  };
}

[[nodiscard]] ConfigurationFenceScopeDTO
NonemptyFenceScope(std::string component_id = "component.arm-a") {
  return ConfigurationFenceScopeDTO{
      {ControllableComponentId(std::move(component_id))},
      {{ControlDomainId("domain.arm-a"),
        ControlDomainRevision("domain-revision-1"),
        ControlDomainRevision("domain-revision-2")}},
      {{ControlBindingId("binding.arm-a"),
        ControlBindingRevision("binding-revision-1"),
        ControlBindingRevision("binding-revision-2")}},
      {ResourceId("arm-a-joint-1")},
  };
}

[[nodiscard]] ManagerApplyFenceExpectationDTO Expectation() {
  return {7U, ConfigurationFenceGeneration(0U), {}};
}

[[nodiscard]] ApplyConfigurationFenceIntentDTO ExpectedFenceIntent(
    const ManagerApplyIntentDTO &apply_intent,
    const ManagerApplyFenceExpectationDTO &expectation = Expectation()) {
  const auto scope =
      manyforge::deployment::derive_apply_configuration_fence_scope(
          TestBundle(), TestBundle());
  return ApplyConfigurationFenceIntentDTO{
      apply_intent.request_context,
      ApplyFenceOwnerDTO{
          apply_intent.apply_id,
          apply_intent.apply_generation,
      },
      scope,
      ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy,
      apply_intent.expected_active_projection_revision.retained_bundle_id,
      apply_intent.expected_active_projection_revision.graph_revision,
      expectation.expected_operation_resource_generation,
      expectation.expected_fence_generation,
      expectation.expected_active_control_configurations,
      "component-manager named Apply affected-scope admission",
  };
}

[[nodiscard]] ConfigurationFenceAcceptedDTO
AcceptedFence(ConfigurationFenceScopeDTO scope = {},
              const RequestReplayStatusDTO replay_status =
                  RequestReplayStatusDTO::kAcceptedNew) {
  return ConfigurationFenceAcceptedDTO{
      ConfigurationFenceId("fence-1"),
      ConfigurationFenceGeneration(1U),
      std::move(scope),
      123,
      replay_status,
  };
}

[[nodiscard]] ExecutionHostEpochDTO ExecutionHostEpoch() {
  return ExecutionHostEpochDTO{
      RuntimeUnitId("runtime.kernel"),
      RuntimeEpoch("kernel-runtime-2"),
      RequestOwnerEpoch("kernel-owner-epoch-2"),
      ContentDigest(std::string(kEvidenceDigest)),
      ActiveBundlePublicationGeneration(3U),
  };
}

enum class ReconcileScript {
  kAcceptedExact,
  kNotSeen,
  kConflicting,
  kUncertain,
};

class ScriptedFencePort final
    : public IManagerKernelApplyFencePort {
public:
  ScriptedFencePort(SqliteManagerApplyJournal &independent_reader,
                    ManagerApplyIntentDTO expected_intent)
      : acquire_response(AcceptedFence()),
        reconcile_acceptance(AcceptedFence()),
        independent_reader_(independent_reader),
        expected_intent_(std::move(expected_intent)) {}

  [[nodiscard]] ConfigurationFenceAdmissionSnapshotDTO
  ObserveAdmission(const ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::runtime_error("admission observation is outside this fixture");
  }

  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &intent) override {
    acquire_calls.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(observation_mutex_);
      last_acquire_ = intent;
    }
    const auto persisted_intent = independent_reader_.ReadIntent(
        expected_intent_.apply_id, expected_intent_.apply_generation);
    const auto persisted_scope = independent_reader_.ReadAffectedScope(
        expected_intent_.apply_id, expected_intent_.apply_generation);
    const auto persisted_attempt = independent_reader_.ReadFenceAttempt(
        expected_intent_.apply_id, expected_intent_.apply_generation);
    saw_exact_durable_intent_and_attempt_before_acquire.store(
        persisted_intent.has_value() &&
            persisted_intent->intent == expected_intent_ &&
            persisted_scope.has_value() &&
            persisted_scope->affected_scope.scope == intent.scope &&
            persisted_scope->affected_scope.busy_disposition ==
                ApplyFenceBusyDispositionDTO::kRejectIfAffectedBusy &&
            persisted_attempt.has_value() &&
            persisted_attempt->fence_intent == intent &&
            persisted_attempt->intent_digest == persisted_intent->intent_digest,
        std::memory_order_relaxed);
    if (acquire_hook) {
      acquire_hook();
    }
    if (throw_on_acquire) {
      throw std::runtime_error("simulated lost Acquire response");
    }
    return acquire_response;
  }

  [[nodiscard]] ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO &query) const override {
    reconcile_calls.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(observation_mutex_);
      last_query_ = query;
    }
    if (throw_on_reconcile) {
      throw std::runtime_error("simulated reconciliation transport failure");
    }

    auto echoed_query = query;
    if (mismatch_reconcile_query) {
      echoed_query.manager_runtime_epoch = RuntimeEpoch("wrong-manager-epoch");
    }

    auto state = ApplyFenceAcquireAttemptReconcileState::kNotSeen;
    std::optional<ConfigurationFenceAcceptedDTO> accepted;
    bool admission_fenced = false;
    std::string direction;
    std::optional<ErrorDTO> error;
    switch (reconcile_script) {
    case ReconcileScript::kAcceptedExact:
      state = ApplyFenceAcquireAttemptReconcileState::kAcceptedExact;
      accepted = reconcile_acceptance;
      admission_fenced = true;
      break;
    case ReconcileScript::kNotSeen:
      state = ApplyFenceAcquireAttemptReconcileState::kNotSeen;
      break;
    case ReconcileScript::kConflicting:
      state = ApplyFenceAcquireAttemptReconcileState::kConflicting;
      admission_fenced = true;
      direction = "inspect the conflicting request-ledger entry";
      error = ErrorDTO{"fence_attempt_conflict", "conflict", {}};
      break;
    case ReconcileScript::kUncertain:
      state = ApplyFenceAcquireAttemptReconcileState::kUncertain;
      admission_fenced = true;
      direction = "keep admission fenced and inspect the Kernel ledger";
      error = ErrorDTO{"fence_attempt_uncertain", "uncertain", {}};
      break;
    }

    return ApplyFenceAcquireAttemptReconcileResultDTO{
        ExecutionHostEpoch(),
        std::move(echoed_query),
        state,
        std::move(accepted),
        admission_fenced,
        std::move(direction),
        invalid_reconcile_result ? ContentDigest("")
                                 : ContentDigest(std::string(kEvidenceDigest)),
        std::move(error),
    };
  }

  [[nodiscard]] ApplyFenceReconcileResultDTO
  Rehydrate(const ApplyFenceRehydrateDTO &request) override {
    rehydrate_calls.fetch_add(1, std::memory_order_relaxed);
    {
      std::lock_guard lock(observation_mutex_);
      last_rehydrate_ = request;
    }
    if (throw_on_rehydrate) {
      throw std::runtime_error("simulated rehydration transport failure");
    }
    auto entry = ApplyFenceReconcileEntryDTO{
        request.persisted_configuration_fence_id,
        request.persisted_configuration_fence_generation,
        recreate_on_rehydrate ? ApplyFenceReconcileState::kRecreated
                              : ApplyFenceReconcileState::kMatched,
        recreate_on_rehydrate
            ? std::optional(ConfigurationFenceId("fence-recreated"))
            : std::nullopt,
        recreate_on_rehydrate ? std::optional(ConfigurationFenceGeneration(2U))
                              : std::nullopt,
    };
    if (mismatch_rehydrate_response) {
      entry.persisted_configuration_fence_id =
          ConfigurationFenceId("different-fence");
    }
    return ApplyFenceReconcileResultDTO{
        ExecutionHostEpoch(),
        {entry},
        false,
        "",
        invalid_rehydrate_result ? ContentDigest("")
                                 : ContentDigest(std::string(kEvidenceDigest)),
        std::nullopt,
    };
  }

  [[nodiscard]] ConfigurationFenceStatusDTO QueryStatus(
      const ConfigurationFenceStatusQueryDTO&) const override {
    throw std::runtime_error("release status is outside this fixture");
  }

  [[nodiscard]] ConfigurationFenceReleasedDTO Release(
      const ConfigurationFenceReleaseDTO&) override {
    throw std::runtime_error("release is outside this fixture");
  }

  [[nodiscard]] std::optional<ApplyConfigurationFenceIntentDTO>
  last_acquire() const {
    std::lock_guard lock(observation_mutex_);
    return last_acquire_;
  }

  [[nodiscard]] std::optional<ApplyFenceAcquireAttemptQueryDTO>
  last_query() const {
    std::lock_guard lock(observation_mutex_);
    return last_query_;
  }

  [[nodiscard]] std::optional<ApplyFenceRehydrateDTO> last_rehydrate() const {
    std::lock_guard lock(observation_mutex_);
    return last_rehydrate_;
  }

  mutable std::atomic<int> reconcile_calls{0};
  std::atomic<int> acquire_calls{0};
  std::atomic<int> rehydrate_calls{0};
  std::atomic<bool> saw_exact_durable_intent_and_attempt_before_acquire{false};
  bool throw_on_acquire{false};
  bool throw_on_reconcile{false};
  bool mismatch_reconcile_query{false};
  bool invalid_reconcile_result{false};
  bool throw_on_rehydrate{false};
  bool mismatch_rehydrate_response{false};
  bool invalid_rehydrate_result{false};
  bool recreate_on_rehydrate{false};
  ReconcileScript reconcile_script{ReconcileScript::kNotSeen};
  ConfigurationFenceAcceptedDTO acquire_response;
  ConfigurationFenceAcceptedDTO reconcile_acceptance;
  std::function<void()> acquire_hook;

private:
  SqliteManagerApplyJournal &independent_reader_;
  ManagerApplyIntentDTO expected_intent_;
  mutable std::mutex observation_mutex_;
  mutable std::optional<ApplyConfigurationFenceIntentDTO> last_acquire_;
  mutable std::optional<ApplyFenceAcquireAttemptQueryDTO> last_query_;
  mutable std::optional<ApplyFenceRehydrateDTO> last_rehydrate_;
};

[[nodiscard]] PersistedManagerApplyFenceAttempt
SeedAttempt(SqliteManagerApplyJournal &journal,
            const ManagerApplyIntentDTO &apply_intent) {
  const auto intent = journal.EnsureIntent(apply_intent).record;
  const auto fence_intent = ExpectedFenceIntent(apply_intent);
  const auto affected_scope = journal.EnsureAffectedScope(
      intent.intent_digest, ManagerApplyAffectedScopeDTO{
                                apply_intent.apply_id,
                                apply_intent.apply_generation,
                                fence_intent.scope,
                                fence_intent.busy_disposition,
                            });
  return journal
      .EnsureFenceAttempt(intent.intent_digest,
                          affected_scope.record.affected_scope_digest,
                          fence_intent)
      .record;
}

[[nodiscard]] ManagerApplyFenceAcquireResult RunAdmission(
    ManagerApplyFenceCoordinator &coordinator,
    const ManagerApplyIntentDTO &apply_intent,
    const ManagerApplyFenceExpectationDTO &expectation = Expectation()) {
  return coordinator.AcquireOrRecoverFence(apply_intent, TestBundle(),
                                           TestBundle(), expectation);
}

TEST(ManagerApplyFenceCoordinator,
     NewAcquireCommitsExactAttemptAndAcceptanceBeforeReturning) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  ScriptedFencePort kernel(independent_reader, apply_intent);
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-1"));

  const auto first = RunAdmission(coordinator, apply_intent);

  EXPECT_EQ(first.path, ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt);
  EXPECT_TRUE(first.durable_intent.inserted);
  EXPECT_TRUE(first.durable_affected_scope.inserted);
  EXPECT_TRUE(first.durable_attempt.inserted);
  EXPECT_TRUE(first.durable_acceptance.inserted);
  EXPECT_EQ(first.durable_attempt.record.fence_intent,
            ExpectedFenceIntent(apply_intent));
  EXPECT_EQ(first.durable_acceptance.record.accepted_fence,
            kernel.acquire_response);
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_EQ(kernel.reconcile_calls.load(), 0);
  EXPECT_EQ(kernel.rehydrate_calls.load(), 1);
  EXPECT_TRUE(
      kernel.saw_exact_durable_intent_and_attempt_before_acquire.load());

  const auto replay = RunAdmission(coordinator, apply_intent);
  EXPECT_EQ(replay.path,
            ManagerApplyFenceAcquirePath::kDurableAcceptanceReplay);
  EXPECT_FALSE(replay.durable_intent.inserted);
  EXPECT_FALSE(replay.durable_affected_scope.inserted);
  EXPECT_FALSE(replay.durable_attempt.inserted);
  EXPECT_FALSE(replay.durable_acceptance.inserted);
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_EQ(kernel.reconcile_calls.load(), 0);
  EXPECT_EQ(kernel.rehydrate_calls.load(), 2);
}

TEST(ManagerApplyFenceCoordinator,
     LostAcquireResponseRecoversAcceptedExactWithoutSecondAcquire) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  ScriptedFencePort kernel(independent_reader, apply_intent);
  kernel.throw_on_acquire = true;
  ManagerApplyFenceCoordinator first_coordinator(
      writer, kernel, RuntimeEpoch("manager-runtime-1"));

  try {
    static_cast<void>(RunAdmission(first_coordinator, apply_intent));
    FAIL() << "expected an effect-uncertain lost response";
  } catch (const ManagerApplyFenceEffectUncertainError &error) {
    EXPECT_EQ(error.phase(), ManagerApplyFenceUncertaintyPhase::kAcquireCall);
    EXPECT_TRUE(error.acquire_invoked_in_this_call());
    EXPECT_EQ(error.recovery_query().persisted_intent,
              ExpectedFenceIntent(apply_intent));
    EXPECT_FALSE(error.observed_acceptance().has_value());
    EXPECT_FALSE(error.observed_reconciliation().has_value());
  }
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_FALSE(writer
                   .ReadFenceAcceptance(apply_intent.apply_id,
                                        apply_intent.apply_generation)
                   .has_value());

  kernel.throw_on_acquire = false;
  kernel.reconcile_script = ReconcileScript::kAcceptedExact;
  kernel.reconcile_acceptance = AcceptedFence();
  ManagerApplyFenceCoordinator recovered_coordinator(
      writer, kernel, RuntimeEpoch("manager-runtime-2"));
  const auto recovered = RunAdmission(recovered_coordinator, apply_intent);

  EXPECT_EQ(recovered.path,
            ManagerApplyFenceAcquirePath::kRecoveredAcceptedExact);
  EXPECT_TRUE(recovered.durable_acceptance.inserted);
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_EQ(kernel.reconcile_calls.load(), 1);
  EXPECT_EQ(kernel.rehydrate_calls.load(), 1);
  ASSERT_TRUE(kernel.last_query().has_value());
  EXPECT_EQ(kernel.last_query()->manager_runtime_epoch,
            RuntimeEpoch("manager-runtime-2"));
}

TEST(ManagerApplyFenceCoordinator,
     AcceptedFenceMustRehydrateBeforeTheBoundaryReturns) {
  for (const int script : {0, 1, 2}) {
    SCOPED_TRACE(script);
    TemporaryDirectory directory;
    const auto path = directory.path() / "apply.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto apply_intent = ApplyIntent();
    ScriptedFencePort kernel(independent_reader, apply_intent);
    kernel.throw_on_rehydrate = script == 0;
    kernel.mismatch_rehydrate_response = script == 1;
    kernel.invalid_rehydrate_result = script == 2;
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-1"));

    try {
      static_cast<void>(RunAdmission(coordinator, apply_intent));
      FAIL() << "expected rehydration uncertainty";
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      EXPECT_EQ(error.phase(),
                script == 0
                    ? ManagerApplyFenceUncertaintyPhase::kRehydrateCall
                    : ManagerApplyFenceUncertaintyPhase::kRehydrateResponse);
      EXPECT_TRUE(error.acquire_invoked_in_this_call());
      EXPECT_TRUE(error.observed_acceptance().has_value());
      EXPECT_EQ(error.observed_rehydration().has_value(), script != 0);
    }
    EXPECT_EQ(kernel.acquire_calls.load(), 1);
    EXPECT_EQ(kernel.rehydrate_calls.load(), 1);
    EXPECT_TRUE(independent_reader
                    .ReadFenceAcceptance(apply_intent.apply_id,
                                         apply_intent.apply_generation)
                    .has_value());
  }
}

TEST(ManagerApplyFenceCoordinator,
     FreshKernelMayReturnOneExactRecreatedFenceMapping) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  ScriptedFencePort kernel(independent_reader, apply_intent);
  kernel.recreate_on_rehydrate = true;
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-1"));

  const auto result = RunAdmission(coordinator, apply_intent);
  ASSERT_EQ(result.rehydration.fence_results.size(), 1U);
  EXPECT_EQ(result.rehydration.fence_results.front().state,
            ApplyFenceReconcileState::kRecreated);
  EXPECT_EQ(result.rehydration.fence_results.front()
                .reconciled_configuration_fence_id,
            ConfigurationFenceId("fence-recreated"));
}

TEST(ManagerApplyFenceCoordinator,
     AuthoritativeNotSeenPermitsOneExactPersistedAcquireRetry) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  const auto attempt = SeedAttempt(writer, apply_intent);
  ScriptedFencePort kernel(independent_reader, apply_intent);
  kernel.reconcile_script = ReconcileScript::kNotSeen;
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-2"));

  const auto result = RunAdmission(coordinator, apply_intent);

  EXPECT_EQ(result.path,
            ManagerApplyFenceAcquirePath::kRetriedAfterAuthoritativeNotSeen);
  EXPECT_EQ(kernel.reconcile_calls.load(), 1);
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  ASSERT_TRUE(kernel.last_query().has_value());
  EXPECT_EQ(kernel.last_query()->persisted_attempt_digest,
            attempt.fence_attempt_digest);
  ASSERT_TRUE(kernel.last_acquire().has_value());
  EXPECT_EQ(*kernel.last_acquire(), attempt.fence_intent);
}

TEST(ManagerApplyFenceCoordinator,
     ConflictingOrUncertainReconciliationNeverRetriesAcquire) {
  const std::vector<ReconcileScript> scripts{
      ReconcileScript::kConflicting,
      ReconcileScript::kUncertain,
  };
  for (const auto script : scripts) {
    SCOPED_TRACE(static_cast<int>(script));
    TemporaryDirectory directory;
    const auto path = directory.path() / "apply.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto apply_intent = ApplyIntent();
    static_cast<void>(SeedAttempt(writer, apply_intent));
    ScriptedFencePort kernel(independent_reader, apply_intent);
    kernel.reconcile_script = script;
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-2"));

    try {
      static_cast<void>(RunAdmission(coordinator, apply_intent));
      FAIL() << "expected fail-closed recovered-attempt uncertainty";
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      EXPECT_EQ(
          error.phase(),
          ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse);
      EXPECT_FALSE(error.acquire_invoked_in_this_call());
      ASSERT_TRUE(error.observed_reconciliation().has_value());
      EXPECT_EQ(error.observed_reconciliation()->state,
                script == ReconcileScript::kConflicting
                    ? ApplyFenceAcquireAttemptReconcileState::kConflicting
                    : ApplyFenceAcquireAttemptReconcileState::kUncertain);
    }
    EXPECT_EQ(kernel.reconcile_calls.load(), 1);
    EXPECT_EQ(kernel.acquire_calls.load(), 0);
  }
}

TEST(ManagerApplyFenceCoordinator,
     InvalidOrMismatchedReconciliationEvidenceNeverRetriesAcquire) {
  for (const bool mismatch_query : {false, true}) {
    SCOPED_TRACE(mismatch_query ? "mismatched echo" : "invalid result");
    TemporaryDirectory directory;
    const auto path = directory.path() / "apply.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto apply_intent = ApplyIntent();
    static_cast<void>(SeedAttempt(writer, apply_intent));
    ScriptedFencePort kernel(independent_reader, apply_intent);
    kernel.reconcile_script = ReconcileScript::kNotSeen;
    kernel.mismatch_reconcile_query = mismatch_query;
    kernel.invalid_reconcile_result = !mismatch_query;
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-2"));

    try {
      static_cast<void>(RunAdmission(coordinator, apply_intent));
      FAIL() << "expected invalid reconciliation rejection";
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      EXPECT_EQ(
          error.phase(),
          ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationResponse);
      EXPECT_FALSE(error.acquire_invoked_in_this_call());
      ASSERT_TRUE(error.observed_reconciliation().has_value());
      EXPECT_EQ(error.observed_reconciliation()->echoed_query ==
                    error.recovery_query(),
                !mismatch_query);
    }
    EXPECT_EQ(kernel.acquire_calls.load(), 0);
  }
}

TEST(ManagerApplyFenceCoordinator,
     ReconciliationTransportFailureRetainsExactRecoveryQuery) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  const auto attempt = SeedAttempt(writer, apply_intent);
  ScriptedFencePort kernel(independent_reader, apply_intent);
  kernel.throw_on_reconcile = true;
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-2"));

  try {
    static_cast<void>(RunAdmission(coordinator, apply_intent));
    FAIL() << "expected reconciliation uncertainty";
  } catch (const ManagerApplyFenceEffectUncertainError &error) {
    EXPECT_EQ(error.phase(),
              ManagerApplyFenceUncertaintyPhase::kAttemptReconciliationCall);
    EXPECT_EQ(error.recovery_query().persisted_attempt_digest,
              attempt.fence_attempt_digest);
    EXPECT_FALSE(error.acquire_invoked_in_this_call());
    EXPECT_FALSE(error.observed_reconciliation().has_value());
  }
  EXPECT_EQ(kernel.acquire_calls.load(), 0);
}

TEST(ManagerApplyFenceCoordinator,
     InvalidOrMismatchedAcquireResponseIsExplicitlyEffectUncertain) {
  std::vector<ConfigurationFenceAcceptedDTO> bad_responses;
  auto invalid = AcceptedFence();
  invalid.configuration_fence_id = ConfigurationFenceId("");
  bad_responses.push_back(invalid);
  auto wrong_generation = AcceptedFence();
  wrong_generation.configuration_fence_generation =
      ConfigurationFenceGeneration(2U);
  bad_responses.push_back(wrong_generation);
  bad_responses.push_back(AcceptedFence(NonemptyFenceScope("component.arm-b")));

  for (const auto &bad_response : bad_responses) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "apply.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto apply_intent = ApplyIntent();
    ScriptedFencePort kernel(independent_reader, apply_intent);
    kernel.acquire_response = bad_response;
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-1"));

    try {
      static_cast<void>(RunAdmission(coordinator, apply_intent));
      FAIL() << "expected invalid acceptance uncertainty";
    } catch (const ManagerApplyFenceEffectUncertainError &error) {
      EXPECT_EQ(error.phase(),
                ManagerApplyFenceUncertaintyPhase::kAcquireResponse);
      EXPECT_TRUE(error.acquire_invoked_in_this_call());
      ASSERT_TRUE(error.observed_acceptance().has_value());
      EXPECT_EQ(*error.observed_acceptance(), bad_response);
    }
    EXPECT_EQ(kernel.acquire_calls.load(), 1);
    EXPECT_FALSE(independent_reader
                     .ReadFenceAcceptance(apply_intent.apply_id,
                                          apply_intent.apply_generation)
                     .has_value());
  }
}

TEST(ManagerApplyFenceCoordinator,
     JournalFailureAfterAcquireRetainsObservedAcceptanceAndAttempt) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  ScriptedFencePort kernel(independent_reader, apply_intent);
  kernel.acquire_hook = [&writer]() { writer.Close(); };
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-1"));

  try {
    static_cast<void>(RunAdmission(coordinator, apply_intent));
    FAIL() << "expected post-effect persistence uncertainty";
  } catch (const ManagerApplyFenceEffectUncertainError &error) {
    EXPECT_EQ(error.phase(),
              ManagerApplyFenceUncertaintyPhase::kAcceptancePersistence);
    EXPECT_TRUE(error.acquire_invoked_in_this_call());
    ASSERT_TRUE(error.observed_acceptance().has_value());
    EXPECT_EQ(*error.observed_acceptance(), AcceptedFence());
  }
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_TRUE(independent_reader
                  .ReadFenceAttempt(apply_intent.apply_id,
                                    apply_intent.apply_generation)
                  .has_value());
  EXPECT_FALSE(independent_reader
                   .ReadFenceAcceptance(apply_intent.apply_id,
                                        apply_intent.apply_generation)
                   .has_value());
}

TEST(ManagerApplyFenceCoordinator,
     PreEffectValidationAndJournalConflictsRemainDistinguishable) {
  {
    TemporaryDirectory directory;
    const auto path = directory.path() / "invalid.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto apply_intent = ApplyIntent();
    auto expectation = Expectation();
    expectation.expected_fence_generation =
        ConfigurationFenceGeneration(std::numeric_limits<std::uint64_t>::max());
    ScriptedFencePort kernel(independent_reader, apply_intent);
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-1"));

    EXPECT_THROW(
        static_cast<void>(RunAdmission(coordinator, apply_intent, expectation)),
        ManagerApplyFenceCoordinatorError);
    EXPECT_EQ(kernel.acquire_calls.load(), 0);
    EXPECT_FALSE(
        writer.ReadIntent(apply_intent.apply_id, apply_intent.apply_generation)
            .has_value());
  }

  {
    TemporaryDirectory directory;
    const auto path = directory.path() / "conflict.sqlite3";
    SqliteManagerApplyJournal writer(path, TestCellId());
    SqliteManagerApplyJournal independent_reader(path, TestCellId());
    const auto original = ApplyIntent();
    ASSERT_TRUE(writer.EnsureIntent(original).inserted);
    auto conflicting = ApplyIntent();
    conflicting.request_context.source_label = "different-source";
    ScriptedFencePort kernel(independent_reader, conflicting);
    ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                             RuntimeEpoch("manager-runtime-1"));

    EXPECT_THROW(static_cast<void>(RunAdmission(coordinator, conflicting)),
                 ManagerApplyJournalConflictError);
    EXPECT_EQ(kernel.acquire_calls.load(), 0);
    EXPECT_EQ(kernel.reconcile_calls.load(), 0);
  }
}

TEST(ManagerApplyFenceCoordinator,
     ConcurrentExactCallsSerializeToOneAcquireAndOneDurableReplay) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "apply.sqlite3";
  SqliteManagerApplyJournal writer(path, TestCellId());
  SqliteManagerApplyJournal independent_reader(path, TestCellId());
  const auto apply_intent = ApplyIntent();
  ScriptedFencePort kernel(independent_reader, apply_intent);
  ManagerApplyFenceCoordinator coordinator(writer, kernel,
                                           RuntimeEpoch("manager-runtime-1"));
  std::promise<void> start_promise;
  const auto start = start_promise.get_future().share();
  const auto invoke = [&]() {
    start.wait();
    return RunAdmission(coordinator, apply_intent);
  };
  auto first_future = std::async(std::launch::async, invoke);
  auto second_future = std::async(std::launch::async, invoke);
  start_promise.set_value();

  const auto first = first_future.get();
  const auto second = second_future.get();
  EXPECT_EQ(kernel.acquire_calls.load(), 1);
  EXPECT_EQ(kernel.reconcile_calls.load(), 0);
  EXPECT_EQ(kernel.rehydrate_calls.load(), 2);
  EXPECT_EQ(first.durable_acceptance.record.accepted_fence,
            second.durable_acceptance.record.accepted_fence);
  const bool paths_are_one_new_one_replay =
      (first.path == ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt &&
       second.path == ManagerApplyFenceAcquirePath::kDurableAcceptanceReplay) ||
      (second.path == ManagerApplyFenceAcquirePath::kAcquiredFromNewAttempt &&
       first.path == ManagerApplyFenceAcquirePath::kDurableAcceptanceReplay);
  EXPECT_TRUE(paths_are_one_new_one_replay);
}

} // namespace
} // namespace manyforge::component_manager

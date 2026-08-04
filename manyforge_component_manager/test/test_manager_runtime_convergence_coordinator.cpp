#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "manyforge_component_manager/manager_runtime_convergence_coordinator.hpp"
#include "manyforge_component_manager/reference_runtime_unit_admission.hpp"
#include "manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kSceneId =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";

class ScopedPackageRoot final {
public:
  ScopedPackageRoot() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-runtime-convergence-test-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("runtime-convergence fixture mkdtemp failed");
    }
    path_ = created;
  }

  ~ScopedPackageRoot() {
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

struct RuntimePackage final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedBundleSource>
      bundle_source;
  std::unique_ptr<deployment::FilesystemVerifiedRetainedProductArtifactSource>
      artifact_source;
};

[[nodiscard]] RuntimePackage
MaterializeRuntimePackage(const std::filesystem::path &root) {
  const auto fixture = deployment::test::make_runtime_only_retention_fixture();
  const deployment::test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  deployment::FilesystemBuildManifestRepository manifests(root);
  deployment::FilesystemCatalogArtifactStore catalogs(root);
  deployment::FilesystemRawArtifactStore raw_artifacts(root);
  deployment::FilesystemRetainedBundleRepository bundles(root);
  auto bundle = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(fixture.qualified, manifests,
                                              input_source, {}, catalogs,
                                              raw_artifacts, bundles);
  if (bundle == nullptr) {
    throw std::runtime_error(
        "runtime-convergence fixture produced no retained bundle");
  }
  return {
      std::move(bundle),
      std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(
          root),
      std::make_unique<
          deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

[[nodiscard]] ManagerApplyIntentDTO
ApplyIntent(const deployment::RetainedBundle &bundle) {
  const auto &graph = bundle.value().cell_graph;
  return {
      PublicRequestContextDTO{
          RequestId("request-1"),
          RequestOwnerEpoch("manager-owner-1"),
          "runtime-convergence-test",
          std::nullopt,
      },
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      graph.cell_id,
      ProjectionRevisionDTO{
          bundle.retained_bundle_id(),
          graph.graph_revision,
          SceneRevision{
              SceneId(std::string(kSceneId)),
              1U,
              ContentDigest(std::string(kDigestOne)),
          },
      },
      bundle.retained_bundle_id(),
      graph.graph_revision,
  };
}

[[nodiscard]] ActiveBundlePublicationReceiptDTO
PredecessorPublication(const ManagerApplyIntentDTO &apply_intent) {
  return {
      ActiveBundlePublicationRequestDTO{
          BootstrapPublicationTransactionDTO{
              BootstrapId("bootstrap-1"),
              BootstrapGeneration(1U),
          },
          apply_intent.expected_active_projection_revision,
          apply_intent.expected_active_projection_revision.retained_bundle_id,
          apply_intent.expected_active_projection_revision.graph_revision,
          std::nullopt,
          ContentDigest(std::string(kDigestOne)),
          ContentDigest(std::string(kDigestTwo)),
      },
      ActiveBundlePublicationGeneration(3U),
      ContentDigest(std::string(kDigestTwo)),
  };
}

[[nodiscard]] ExecutionHostEpochDTO HostEpoch() {
  return {
      RuntimeUnitId("runtime.kernel"),
      RuntimeEpoch("kernel-runtime-1"),
      RequestOwnerEpoch("kernel-owner-1"),
      ContentDigest(std::string(kDigestOne)),
      ActiveBundlePublicationGeneration(3U),
  };
}

class CurrentPublicationReader final : public IActiveBundlePublicationReader {
public:
  explicit CurrentPublicationReader(ActiveBundlePublicationReceiptDTO receipt)
      : receipt_(std::move(receipt)) {}

  [[nodiscard]] Result<ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      const ActiveBundlePublicationGeneration generation) const override {
    ++read_calls;
    if ((fail_on_call > 0 && read_calls == fail_on_call) ||
        generation != receipt_.publication_generation) {
      return Result<ActiveBundlePublicationReceiptDTO>::Err(ErrorDTO{
          "publication_not_current", "publication is not current", {}});
    }
    return Result<ActiveBundlePublicationReceiptDTO>::Ok(receipt_);
  }

  mutable int read_calls{0};
  int fail_on_call{0};

private:
  ActiveBundlePublicationReceiptDTO receipt_;
};

class ExactFencePort final
    : public IManagerKernelApplyFenceAcquireRehydratePort {
public:
  [[nodiscard]] ConfigurationFenceAcceptedDTO
  Acquire(const ApplyConfigurationFenceIntentDTO &intent) override {
    ++acquire_calls;
    return {
        ConfigurationFenceId("fence-1"),
        ConfigurationFenceGeneration(intent.expected_fence_generation.value() +
                                     1U),
        intent.scope,
        100,
        RequestReplayStatusDTO::kAcceptedNew,
    };
  }

  [[nodiscard]] ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const ApplyFenceAcquireAttemptQueryDTO &query) const override {
    ++reconcile_calls;
    return {
        HostEpoch(),
        query,
        ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  [[nodiscard]] ApplyFenceReconcileResultDTO
  Rehydrate(const ApplyFenceRehydrateDTO &request) override {
    ++rehydrate_calls;
    return {
        HostEpoch(),
        {ApplyFenceReconcileEntryDTO{
            request.persisted_configuration_fence_id,
            request.persisted_configuration_fence_generation,
            ApplyFenceReconcileState::kMatched,
            std::nullopt,
            std::nullopt,
        }},
        false,
        "",
        ContentDigest(std::string(kDigestOne)),
        std::nullopt,
    };
  }

  int acquire_calls{0};
  mutable int reconcile_calls{0};
  int rehydrate_calls{0};
};

class ScriptedRuntimePorts final : public IRuntimeUnitProcessPort,
                                   public IRuntimeUnitProgressPort {
public:
  explicit ScriptedRuntimePorts(SqliteManagerApplyJournal &independent_reader)
      : independent_reader_(independent_reader) {}

  [[nodiscard]] ProcessPresenceObservation
  ObserveProcess(const RuntimeUnitConvergenceQueryDTO &query) override {
    ++observe_calls;
    last_query = query;
    return Presence(query);
  }

  [[nodiscard]] ProcessPresenceObservation
  EnsureRunning(const RuntimeUnitConvergenceQueryDTO &query) override {
    ++ensure_calls;
    const auto durable = independent_reader_.ReadRuntimeConvergenceIntent(
        query.apply_id, query.apply_generation);
    saw_durable_intent_before_ensure =
        durable.has_value() &&
        durable->runtime_convergence_intent_digest ==
            query.runtime_convergence_intent_digest &&
        durable->runtime_convergence_intent.runtime_units.size() == 1U &&
        durable->runtime_convergence_intent.runtime_units.front() == query.plan;
    state_ = ProcessPresenceState::kPresent;
    observation_sequence_ = 2U;
    if (throw_after_ensure_effect) {
      throw std::runtime_error("simulated lost process-start response");
    }
    return Presence(query);
  }

  [[nodiscard]] std::optional<RuntimeUnitSourceReport>
  ObserveProgress(const RuntimeUnitConvergenceQueryDTO &query,
                  const RuntimeEpoch &runtime_epoch) override {
    ++progress_calls;
    if (!report_available) {
      return std::nullopt;
    }
    if (runtime_epoch.value() != process_incarnation_) {
      throw std::runtime_error("progress query has the wrong runtime epoch");
    }
    std::vector<std::string> roles;
    for (const auto &role : query.plan.required_runtime_role_ids) {
      roles.push_back(role.value());
    }
    return RuntimeUnitSourceReport(
        query.plan.runtime_unit_id.value(), process_incarnation_, 1U, 1U,
        RuntimeSourcePhase::kServing, RuntimeSourceHealth::kHealthy,
        query.plan.product_identity.value(),
        query.plan.configuration_identity.value(), std::move(roles),
        ManagedCommandSurfaceState::kClosedNoActiveEffect);
  }

  bool throw_after_ensure_effect{false};
  bool report_available{true};
  bool saw_durable_intent_before_ensure{false};
  int observe_calls{0};
  int ensure_calls{0};
  int progress_calls{0};
  std::optional<RuntimeUnitConvergenceQueryDTO> last_query;

private:
  [[nodiscard]] ProcessPresenceObservation
  Presence(const RuntimeUnitConvergenceQueryDTO &query) const {
    return ProcessPresenceObservation(
        query.plan.runtime_unit_id.value(), "process-observer-1",
        observation_sequence_, state_,
        state_ == ProcessPresenceState::kPresent ? process_incarnation_ : "");
  }

  SqliteManagerApplyJournal &independent_reader_;
  ProcessPresenceState state_{ProcessPresenceState::kAbsent};
  std::uint64_t observation_sequence_{1U};
  std::string process_incarnation_{"process-epoch-1"};
};

struct CoordinatorFixture final {
  explicit CoordinatorFixture(const std::filesystem::path &root)
      : package(MaterializeRuntimePackage(root)),
        apply_intent(ApplyIntent(*package.bundle)),
        journal(root / "apply.sqlite3",
                package.bundle->value().cell_graph.cell_id),
        independent_reader(root / "apply.sqlite3",
                           package.bundle->value().cell_graph.cell_id),
        publication(PredecessorPublication(apply_intent)),
        runtime_ports(independent_reader),
        fence_coordinator(journal, kernel, RuntimeEpoch("manager-runtime-1")),
        coordinator(fence_coordinator, journal, publication,
                    *package.artifact_source, runtime_ports, runtime_ports,
                    "linux", "posix.1-2017", [] { return 1'000U; }) {}

  [[nodiscard]] ManagerApplyRuntimeConvergenceResult Run() {
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ManagerApplyFenceExpectationDTO{
                                             0U,
                                             ConfigurationFenceGeneration(0U),
                                             {},
                                         },
                                         ActiveBundlePublicationGeneration(3U));
  }

  RuntimePackage package;
  ManagerApplyIntentDTO apply_intent;
  SqliteManagerApplyJournal journal;
  SqliteManagerApplyJournal independent_reader;
  CurrentPublicationReader publication;
  ExactFencePort kernel;
  ScriptedRuntimePorts runtime_ports;
  ManagerApplyFenceCoordinator fence_coordinator;
  ManagerApplyRuntimeConvergenceCoordinator coordinator;
};

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     PersistsBeforeStartStagesAndReplaysWithoutASecondEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());

  const auto first = fixture.Run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_TRUE(first.durable_runtime_intent.inserted);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  EXPECT_TRUE(first.durable_staged_ready->inserted);
  EXPECT_EQ(
      first.durable_staged_ready->record.staged_ready.staged_ready_generation,
      1U);
  ASSERT_EQ(first.durable_runtime_intent.record.runtime_convergence_intent
                .runtime_units.size(),
            1U);
  EXPECT_EQ(first.durable_runtime_intent.record.runtime_convergence_intent
                .runtime_units.front()
                .candidate_disposition,
            RuntimeUnitCandidateDisposition::kRetainedExact);
  EXPECT_TRUE(fixture.runtime_ports.saw_durable_intent_before_ensure);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.publication.read_calls, 3);

  const auto replay = fixture.Run();
  ASSERT_EQ(replay.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_FALSE(replay.durable_runtime_intent.inserted);
  ASSERT_TRUE(replay.durable_staged_ready.has_value());
  EXPECT_FALSE(replay.durable_staged_ready->inserted);
  EXPECT_EQ(replay.durable_staged_ready->record,
            first.durable_staged_ready->record);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
  EXPECT_EQ(fixture.kernel.rehydrate_calls, 2);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     LostStartResponseRecoversByObservationWithoutDuplicateStart) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.throw_after_ensure_effect = true;

  try {
    static_cast<void>(fixture.Run());
    FAIL() << "expected an effect-uncertain start result";
  } catch (const ManagerRuntimeProcessEffectUncertainError &error) {
    EXPECT_TRUE(error.recovery_query().valid());
    EXPECT_EQ(error.recovery_query().apply_id, fixture.apply_intent.apply_id);
  }
  EXPECT_TRUE(fixture.runtime_ports.saw_durable_intent_before_ensure);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());

  fixture.runtime_ports.throw_after_ensure_effect = false;
  const auto recovered = fixture.Run();
  EXPECT_EQ(recovered.state,
            ManagerRuntimeConvergenceResultState::kStagedReady);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_EQ(fixture.kernel.acquire_calls, 1);
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     MissingProgressRemainsPendingWithoutStagedReadyEvidence) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.runtime_ports.report_available = false;

  const auto result = fixture.Run();
  EXPECT_EQ(result.state, ManagerRuntimeConvergenceResultState::kPending);
  EXPECT_FALSE(result.durable_staged_ready.has_value());
  EXPECT_EQ(fixture.publication.read_calls, 2);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     PublicationDriftAfterStartPreventsStagedReadyCommit) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  fixture.publication.fail_on_call = 3;

  EXPECT_THROW(static_cast<void>(fixture.Run()),
               ManagerRuntimeConvergenceCoordinatorError);
  EXPECT_EQ(fixture.runtime_ports.ensure_calls, 1);
  EXPECT_FALSE(
      fixture.journal
          .ReadLatestRuntimeStagedReady(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     IncompatibleHostRejectsBeforeFenceOrProcessEffect) {
  ScopedPackageRoot root;
  CoordinatorFixture fixture(root.path());
  ManagerApplyRuntimeConvergenceCoordinator incompatible(
      fixture.fence_coordinator, fixture.journal, fixture.publication,
      *fixture.package.artifact_source, fixture.runtime_ports,
      fixture.runtime_ports, "darwin", "posix.1-2017", [] { return 1'000U; });

  EXPECT_THROW(static_cast<void>(incompatible.ConvergeOrRecover(
                   fixture.apply_intent, *fixture.package.bundle,
                   *fixture.package.bundle,
                   ManagerApplyFenceExpectationDTO{
                       0U,
                       ConfigurationFenceGeneration(0U),
                       {},
                   },
                   ActiveBundlePublicationGeneration(3U))),
               ReferenceRuntimeUnitAdmissionError);
  EXPECT_EQ(fixture.kernel.acquire_calls, 0);
  EXPECT_EQ(fixture.runtime_ports.observe_calls, 0);
  EXPECT_FALSE(
      fixture.journal.ReadIntent(ApplyId("apply-1"), ApplyGeneration(1U))
          .has_value());
}

TEST(ManagerApplyRuntimeConvergenceCoordinator,
     ConcreteRetainedProcessReachesDurableStagedReadyWithoutEpochReplacement) {
  ScopedPackageRoot root;
  auto package = MaterializeRuntimePackage(root.path());
  const auto runtime_root = root.path() / "manager-private-runtime";
  ASSERT_TRUE(std::filesystem::create_directory(runtime_root));
  const auto apply_intent = ApplyIntent(*package.bundle);
  SqliteManagerApplyJournal journal(root.path() / "concrete-apply.sqlite3",
                                    package.bundle->value().cell_graph.cell_id);
  CurrentPublicationReader publication(PredecessorPublication(apply_intent));
  ExactFencePort kernel;
  ManagerApplyFenceCoordinator fence_coordinator(
      journal, kernel, RuntimeEpoch("manager-runtime-1"));
  RetainedPosixRuntimeUnitAdapter runtime_adapter(
      runtime_root, *package.bundle_source, *package.artifact_source, "linux",
      "posix.1-2017");
  std::uint64_t now_ns = 1'000U;
  ManagerApplyRuntimeConvergenceCoordinator coordinator(
      fence_coordinator, journal, publication, *package.artifact_source,
      runtime_adapter, runtime_adapter, "linux", "posix.1-2017",
      [&now_ns] { return now_ns++; });
  const auto run = [&] {
    return coordinator.ConvergeOrRecover(apply_intent, *package.bundle,
                                         *package.bundle,
                                         ManagerApplyFenceExpectationDTO{
                                             0U,
                                             ConfigurationFenceGeneration(0U),
                                             {},
                                         },
                                         ActiveBundlePublicationGeneration(3U));
  };

  const auto first = run();
  ASSERT_EQ(first.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(first.durable_staged_ready.has_value());
  ASSERT_EQ(
      first.durable_staged_ready->record.staged_ready.runtime_units.size(), 1U);
  const auto first_evidence =
      first.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(first_evidence.source_report_sequence, 1U);

  const auto second = run();
  ASSERT_EQ(second.state, ManagerRuntimeConvergenceResultState::kStagedReady);
  ASSERT_TRUE(second.durable_staged_ready.has_value());
  ASSERT_EQ(
      second.durable_staged_ready->record.staged_ready.runtime_units.size(),
      1U);
  const auto &second_evidence =
      second.durable_staged_ready->record.staged_ready.runtime_units.front();
  EXPECT_EQ(second_evidence.runtime_epoch, first_evidence.runtime_epoch);
  EXPECT_EQ(second_evidence.source_report_sequence, 2U);
  EXPECT_EQ(
      second.durable_staged_ready->record.staged_ready.staged_ready_generation,
      2U);
  EXPECT_EQ(kernel.acquire_calls, 1);
}

} // namespace
} // namespace manyforge::component_manager

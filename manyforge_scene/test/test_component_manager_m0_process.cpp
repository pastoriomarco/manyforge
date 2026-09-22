#include <gtest/gtest.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <limits>
#include <manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_active_runtime_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_coordinator.hpp>
#include <manyforge_component_manager/manager_apply_fence_release_coordinator.hpp>
#include <manyforge_component_manager/manager_runtime_convergence_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_activation_coordinator.hpp>
#include <manyforge_component_manager/manager_scene_stage_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp>
#include <manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <map>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "manyforge_scene/canonical_empty_scene_owner.hpp"
#include "runtime_only_retention_fixture.hpp"

namespace manyforge::scene {
namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

constexpr std::string_view kKernelLedgerDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kKernelEvidenceDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";

class InstalledRuntimeCompositionPort final
    : public component_manager::IManagerSelectedRuntimeCompositionPort {
public:
  [[nodiscard]]
  component_manager::ManagerSelectedRuntimeCompositionResultDTO
  InstallOrObserve(
      const component_manager::ManagerSelectedRuntimeCompositionRequestDTO
          &request) override {
    if (!request.valid()) {
      throw std::runtime_error(
          "M0 composition fixture received an invalid request");
    }
    ++calls;
    return {
        request,
        component_manager::ManagerSelectedRuntimeCompositionState::kInstalled,
    };
  }

  int calls{0};
};

class M0Directory final {
public:
  M0Directory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-component-manager-m0-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("M0 fixture mkdtemp failed");
    }
    root_ = created;
    std::filesystem::copy(MANYFORGE_SCENE_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
                          package_root(),
                          std::filesystem::copy_options::recursive);
    std::filesystem::create_directory(manager_root());
    std::filesystem::create_directory(execution_state_root());
    std::filesystem::create_directory(execution_run_root());
  }

  ~M0Directory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path package_root() const {
    return root_ / "packages";
  }
  [[nodiscard]] std::filesystem::path manager_root() const {
    return root_ / "manager";
  }
  [[nodiscard]] std::filesystem::path execution_state_root() const {
    return root_ / "execution-state";
  }
  [[nodiscard]] std::filesystem::path execution_run_root() const {
    return root_ / "execution-run";
  }
  [[nodiscard]] std::filesystem::path scene_path() const {
    return root_ / "logical-scene.sqlite3";
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path root_;
};

[[nodiscard]] deployment::CanonicalEmptyRetainedBundleAdmission
LoadAdmission(const M0Directory &directory) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      directory.package_root());
  return source.load_verified();
}

[[nodiscard]] std::shared_ptr<const deployment::RetainedBundle>
MaterializeRuntimeCandidate(const M0Directory &directory,
                            const core::CellId &cell_id) {
  const auto fixture =
      deployment::test::make_runtime_only_retention_fixture(cell_id.value());
  const deployment::test::RuntimeOnlyCompilerInputArtifactSource input_source(
      fixture.runtime_profile);
  deployment::FilesystemBuildManifestRepository manifests(
      directory.package_root());
  deployment::FilesystemCatalogArtifactStore catalogs(directory.package_root());
  deployment::FilesystemRawArtifactStore raw_artifacts(
      directory.package_root());
  deployment::FilesystemRetainedBundleRepository bundles(
      directory.package_root());
  auto candidate = deployment::detail::RetainedBundleMaterializer::
      materialize_store_and_reload_production(fixture.qualified, manifests,
                                              input_source, {}, catalogs,
                                              raw_artifacts, bundles);
  if (candidate == nullptr) {
    throw std::runtime_error("M0 fixture produced no runtime candidate");
  }
  return candidate;
}

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO CurrentPublication(
    const component_manager::SqliteActiveBundlePublicationStore &store) {
  const auto inventory = store.VerifyIntegrity();
  if (!inventory.current_generation.nonzero()) {
    throw std::runtime_error("M0 fixture has no active publication");
  }
  const auto current =
      store.ReadActiveBundlePublication(inventory.current_generation);
  if (!current.ok()) {
    throw std::runtime_error("M0 fixture could not read its current head");
  }
  return current.value();
}

[[nodiscard]] component_manager::ManagerApplyIntentDTO
ApplyIntent(const deployment::CanonicalEmptyRetainedBundleAdmission &admission,
            const deployment::RetainedBundle &candidate,
            const core::ActiveBundlePublicationReceiptDTO &predecessor) {
  component_manager::ManagerApplyIntentDTO intent{
      core::PublicRequestContextDTO{
          core::RequestId("m0-request-1"),
          core::RequestOwnerEpoch("m0-manager-owner-1"),
          "external-manager-m0",
          std::nullopt,
      },
      core::ApplyId("m0-apply-1"),
      core::ApplyGeneration(1U),
      admission.cell_id(),
      predecessor.request.candidate_projection_revision,
      candidate.retained_bundle_id(),
      candidate.value().cell_graph.graph_revision,
      component_manager::ManagerApplyPreparedActivationDTO{
          core::ContentDigest(
              "sha256:1414141414141414141414141414141414141414141414141414141414141414"),
          core::DeploymentArtifactId("deployment-artifact-1"),
          core::DeploymentRevision(
              "sha256:1515151515151515151515151515151515151515151515151515151515151515"),
          core::ProgramBindingId(
              "sha256:1717171717171717171717171717171717171717171717171717171717171717"),
          core::ContentDigest(
              "sha256:1616161616161616161616161616161616161616161616161616161616161616"),
      },
      core::AdmissionGeneration(1U),
      std::nullopt,
  };
  if (!intent.valid()) {
    throw std::runtime_error("M0 fixture produced an invalid Apply intent");
  }
  return intent;
}

class NoResourceKernelFencePort final
    : public core::IManagerKernelApplyFencePort {
public:
  NoResourceKernelFencePort(
      component_manager::SqliteManagerApplyJournal &journal,
      const component_manager::SqliteActiveBundlePublicationStore
          &publication_store)
      : journal_(journal), publication_store_(publication_store) {}

  [[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO ObserveAdmission(
      const core::ConfigurationFenceAdmissionQueryDTO &) const override {
    throw std::runtime_error("admission observation is outside the M0 fixture");
  }

  [[nodiscard]] core::ConfigurationFenceAcceptedDTO
  Acquire(const core::ApplyConfigurationFenceIntentDTO &intent) override {
    if (!intent.valid() || !EmptyScope(intent.scope)) {
      throw std::runtime_error(
          "M0 no-resource Kernel received a nonempty fence");
    }
    return {
        core::ConfigurationFenceId("m0-empty-fence"),
        core::ConfigurationFenceGeneration(1U),
        intent.scope,
        1,
        core::RequestReplayStatusDTO::kAcceptedNew,
    };
  }

  [[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO
  ReconcileAcquireAttempt(
      const core::ApplyFenceAcquireAttemptQueryDTO &query) const override {
    return {
        HostEpoch(),
        query,
        core::ApplyFenceAcquireAttemptReconcileState::kNotSeen,
        std::nullopt,
        false,
        "",
        core::ContentDigest(std::string(kKernelLedgerDigest)),
        std::nullopt,
    };
  }

  [[nodiscard]] core::ApplyFenceReconcileResultDTO
  Rehydrate(const core::ApplyFenceRehydrateDTO &request) override {
    if (!request.valid() || !EmptyScope(request.persisted_scope)) {
      throw std::runtime_error(
          "M0 no-resource Kernel received invalid rehydration");
    }
    return {
        HostEpoch(),
        {core::ApplyFenceReconcileEntryDTO{
            request.persisted_configuration_fence_id,
            request.persisted_configuration_fence_generation,
            core::ApplyFenceReconcileState::kMatched,
            std::nullopt,
            std::nullopt,
        }},
        false,
        "",
        core::ContentDigest(std::string(kKernelLedgerDigest)),
        std::nullopt,
    };
  }

  [[nodiscard]] core::ConfigurationFenceStatusDTO QueryStatus(
      const core::ConfigurationFenceStatusQueryDTO &query) const override {
    const auto finalized =
        journal_.ReadSceneFinalized(query.owner_transaction.apply_id,
                                    query.owner_transaction.apply_generation);
    const auto publication = journal_.ReadPublicationReceipt(
        query.owner_transaction.apply_id,
        query.owner_transaction.apply_generation);
    if (!finalized.has_value() || !publication.has_value() ||
        !EmptyScope(query.expected_scope)) {
      throw std::runtime_error(
          "M0 no-resource Kernel lacks finalized empty-scope evidence");
    }
    const auto &candidate = finalized->scene_finalized.finalized_result
                                .candidate_projection_revision;
    const auto &receipt = publication->publication.publication_receipt;
    const core::ConfigurationFenceReconciliationEvidenceDTO evidence{
        HostEpoch(),
        query.owner_transaction,
        query.configuration_fence_id,
        query.configuration_fence_generation,
        query.expected_scope,
        1U,
        candidate.graph_revision,
        candidate,
        receipt.publication_generation,
        receipt.durable_public_pointer_content_digest,
        {},
        0U,
        0U,
        core::ContentDigest(std::string(kKernelEvidenceDigest)),
    };
    return {
        query,
        evidence,
        core::ConfigurationFenceState::kHeld,
        true,
        std::nullopt,
        "",
        core::ContentDigest(std::string(kKernelLedgerDigest)),
        std::nullopt,
    };
  }

  [[nodiscard]] core::ConfigurationFenceReleasedDTO
  Release(const core::ConfigurationFenceReleaseDTO &request) override {
    if (!request.valid() ||
        !EmptyScope(request.reconciliation_evidence.reconciled_scope)) {
      throw std::runtime_error(
          "M0 no-resource Kernel received invalid release");
    }
    return {
        request,
        request.reconciliation_evidence.execution_host_epoch,
        2,
        core::RequestReplayStatusDTO::kAcceptedNew,
        core::ContentDigest(std::string(kKernelEvidenceDigest)),
    };
  }

private:
  [[nodiscard]] static bool
  EmptyScope(const core::ConfigurationFenceScopeDTO &scope) noexcept {
    return scope == core::ConfigurationFenceScopeDTO{};
  }

  [[nodiscard]] core::ExecutionHostEpochDTO HostEpoch() const {
    const auto publication = CurrentPublication(publication_store_);
    return {
        core::RuntimeUnitId("runtime.kernel"),
        core::RuntimeEpoch("m0-kernel-runtime-1"),
        core::RequestOwnerEpoch("m0-kernel-owner-1"),
        core::ContentDigest(std::string(kKernelLedgerDigest)),
        publication.publication_generation,
    };
  }

  component_manager::SqliteManagerApplyJournal &journal_;
  const component_manager::SqliteActiveBundlePublicationStore
      &publication_store_;
};

void WriteAll(const int fd, const std::string_view bytes) {
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto written =
        ::write(fd, bytes.data() + offset, bytes.size() - offset);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      throw std::runtime_error("M0 child result write failed");
    }
    offset += static_cast<std::size_t>(written);
  }
}

[[nodiscard]] std::string
RunExternalManager(const std::function<std::string()> &phase) {
  int result_pipe[2]{-1, -1};
  if (::pipe2(result_pipe, O_CLOEXEC) != 0) {
    throw std::runtime_error("M0 result pipe creation failed");
  }
  const pid_t manager = ::fork();
  if (manager < 0) {
    static_cast<void>(::close(result_pipe[0]));
    static_cast<void>(::close(result_pipe[1]));
    throw std::runtime_error("M0 Manager fork failed");
  }
  if (manager == 0) {
    static_cast<void>(::close(result_pipe[0]));
    try {
      const auto result = phase();
      WriteAll(result_pipe[1], result + "\n");
      ::_exit(0);
    } catch (const std::exception &error) {
      try {
        WriteAll(result_pipe[1], "ERROR:" + std::string(error.what()) + "\n");
      } catch (...) {
      }
      ::_exit(70);
    } catch (...) {
      ::_exit(71);
    }
  }

  static_cast<void>(::close(result_pipe[1]));
  std::string result;
  std::array<char, 256U> buffer{};
  for (;;) {
    const auto count = ::read(result_pipe[0], buffer.data(), buffer.size());
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count < 0) {
      static_cast<void>(::close(result_pipe[0]));
      throw std::runtime_error("M0 parent result read failed");
    }
    if (count == 0) {
      break;
    }
    result.append(buffer.data(), static_cast<std::size_t>(count));
    if (result.size() > 4096U) {
      static_cast<void>(::close(result_pipe[0]));
      throw std::runtime_error("M0 child result exceeds its bound");
    }
  }
  static_cast<void>(::close(result_pipe[0]));
  int status = 0;
  if (::waitpid(manager, &status, 0) != manager || !WIFEXITED(status) ||
      WEXITSTATUS(status) != 0) {
    throw std::runtime_error("external M0 Manager failed: " + result);
  }
  if (result.empty() || result.back() != '\n') {
    throw std::runtime_error("external M0 Manager returned malformed output");
  }
  result.pop_back();
  if (result.starts_with("ERROR:")) {
    throw std::runtime_error(result);
  }
  return result;
}

[[nodiscard]] std::string ActiveEpoch(
    const component_manager::ManagerActiveRuntimeConvergenceResult &result) {
  if (result.state !=
          component_manager::ManagerActiveRuntimeConvergenceState::kConverged ||
      result.observations.observations().size() != 1U ||
      result.observations.observations().front().runtime_epoch().empty()) {
    throw std::runtime_error(
        "M0 active reconciliation lacks one converged runtime epoch");
  }
  return result.observations.observations().front().runtime_epoch();
}

[[nodiscard]] std::map<std::string, pid_t>
ReadRuntimePids(const std::filesystem::path &runtime_root) {
  std::filesystem::path state_path;
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(runtime_root)) {
    if (entry.path().filename() == "process.state") {
      if (!state_path.empty()) {
        throw std::runtime_error("M0 fixture found multiple process states");
      }
      state_path = entry.path();
    }
  }
  if (state_path.empty()) {
    throw std::runtime_error("M0 fixture found no process state");
  }
  std::ifstream stream(state_path);
  if (!stream.good()) {
    throw std::runtime_error("M0 fixture could not read process state");
  }
  std::map<std::string, pid_t> result;
  std::string line;
  while (std::getline(stream, line)) {
    for (const std::string key : {"controller_pid", "pid"}) {
      const auto prefix = key + "=";
      if (line.starts_with(prefix)) {
        const auto value = std::stoll(line.substr(prefix.size()));
        if (value <= 0 || value > std::numeric_limits<pid_t>::max()) {
          throw std::runtime_error("M0 process state PID is invalid");
        }
        result.emplace(key, static_cast<pid_t>(value));
      }
    }
  }
  if (result.size() != 2U) {
    throw std::runtime_error("M0 process state lacks exact PIDs");
  }
  return result;
}

[[nodiscard]] bool ProcessIsLive(const pid_t pid) {
  const auto path =
      std::filesystem::path("/proc") / std::to_string(pid) / "stat";
  std::ifstream stream(path);
  if (!stream.good()) {
    return false;
  }
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  const auto close = text.rfind(')');
  return close != std::string::npos && close + 2U < text.size() &&
         text[close + 2U] != 'Z' && text[close + 2U] != 'X';
}

void WaitForProcessPairToExit(const std::map<std::string, pid_t> &pids) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while ((ProcessIsLive(pids.at("pid")) ||
          ProcessIsLive(pids.at("controller_pid"))) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  if (ProcessIsLive(pids.at("pid")) ||
      ProcessIsLive(pids.at("controller_pid"))) {
    throw std::runtime_error("M0 runtime/controller did not exit in time");
  }
}

TEST(ComponentManagerM0Process,
     ExternalCrashRecoveryOwnsApplyRuntimeCrashStopAndRestartWithoutRobotPort) {
  M0Directory directory;
  const auto initial_admission = LoadAdmission(directory);
  const auto candidate =
      MaterializeRuntimeCandidate(directory, initial_admission.cell_id());
  const auto candidate_id = candidate->retained_bundle_id();

  EXPECT_EQ(RunExternalManager([&] {
              const auto admission = LoadAdmission(directory);
              component_manager::ExternallyProvisionedManagerStores stores(
                  directory.manager_root(), admission);
              CanonicalEmptySceneOwner scene_owner(
                  directory.scene_path(), admission,
                  stores.active_bundle_publication_store());
              component_manager::CanonicalEmptyBootstrapCoordinator bootstrap(
                  stores, scene_owner);
              static_cast<void>(bootstrap.BeginOrRecover(admission));
              static_cast<void>(bootstrap.AdvanceOrRecover(admission));
              const auto finalized = bootstrap.AdvanceOrRecover(admission);
              if (finalized.state !=
                  core::ProjectionBootstrapStatusState::kFinalized) {
                throw std::runtime_error("M0 bootstrap did not finalize");
              }
              return std::string("bootstrapped");
            }),
            "bootstrapped");

  const auto first_epoch = RunExternalManager([&] {
    const auto admission = LoadAdmission(directory);
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        directory.package_root());
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        directory.package_root());
    const auto runtime_candidate = bundle_source.get_verified(candidate_id);
    if (runtime_candidate == nullptr) {
      throw std::runtime_error("M0 Manager could not load runtime candidate");
    }
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_root(), admission);
    const auto predecessor =
        CurrentPublication(stores.active_bundle_publication_store());
    const auto intent = ApplyIntent(admission, *runtime_candidate, predecessor);
    NoResourceKernelFencePort kernel(stores.apply_journal(),
                                     stores.active_bundle_publication_store());
    component_manager::ManagerApplyFenceCoordinator fence(
        stores.apply_journal(), kernel,
        core::RuntimeEpoch("m0-manager-runtime-start"));
    component_manager::RetainedPosixRuntimeUnitAdapter adapter(
        directory.execution_state_root(), directory.execution_run_root(),
        bundle_source, artifact_source, "linux", "posix.1-2017");
    std::uint64_t now = 1'000U;
    component_manager::ManagerApplyRuntimeConvergenceCoordinator convergence(
        fence, stores.apply_journal(), stores.active_bundle_publication_store(),
        artifact_source, adapter, adapter, "linux", "posix.1-2017",
        [&now] { return ++now; });
    const auto result = convergence.ConvergeOrRecover(
        intent, admission.retained_bundle(), *runtime_candidate,
        component_manager::ManagerApplyFenceExpectationDTO{
            0U, core::ConfigurationFenceGeneration(0U), {}},
        predecessor.publication_generation);
    if (result.state !=
            component_manager::ManagerRuntimeConvergenceResultState::
                kStagedReady ||
        !result.durable_staged_ready.has_value() ||
        result.durable_staged_ready->record.staged_ready.runtime_units.size() !=
            1U) {
      throw std::runtime_error("M0 initial runtime did not stage ready");
    }
    return result.durable_staged_ready->record.staged_ready.runtime_units
        .front()
        .runtime_epoch.value();
  });

  const auto recovered_epoch = RunExternalManager([&] {
    const auto admission = LoadAdmission(directory);
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        directory.package_root());
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        directory.package_root());
    const auto runtime_candidate = bundle_source.get_verified(candidate_id);
    if (runtime_candidate == nullptr) {
      throw std::runtime_error("M0 recovery could not load runtime candidate");
    }
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_root(), admission);
    CanonicalEmptySceneOwner scene_owner(
        directory.scene_path(), admission,
        stores.active_bundle_publication_store(), bundle_source);
    const auto predecessor =
        CurrentPublication(stores.active_bundle_publication_store());
    const auto intent = ApplyIntent(admission, *runtime_candidate, predecessor);
    NoResourceKernelFencePort kernel(stores.apply_journal(),
                                     stores.active_bundle_publication_store());
    component_manager::ManagerApplyFenceCoordinator fence(
        stores.apply_journal(), kernel,
        core::RuntimeEpoch("m0-manager-runtime-recovery"));
    component_manager::RetainedPosixRuntimeUnitAdapter adapter(
        directory.execution_state_root(), directory.execution_run_root(),
        bundle_source, artifact_source, "linux", "posix.1-2017");
    std::uint64_t now = 2'000U;
    component_manager::ManagerApplyRuntimeConvergenceCoordinator convergence(
        fence, stores.apply_journal(), stores.active_bundle_publication_store(),
        artifact_source, adapter, adapter, "linux", "posix.1-2017",
        [&now] { return ++now; });
    const auto runtime = convergence.ConvergeOrRecover(
        intent, admission.retained_bundle(), *runtime_candidate,
        component_manager::ManagerApplyFenceExpectationDTO{
            0U, core::ConfigurationFenceGeneration(0U), {}},
        predecessor.publication_generation);
    if (runtime.state !=
            component_manager::ManagerRuntimeConvergenceResultState::
                kStagedReady ||
        !runtime.durable_staged_ready.has_value()) {
      throw std::runtime_error("M0 recovery did not restage runtime");
    }
    const auto epoch =
        runtime.durable_staged_ready->record.staged_ready.runtime_units.front()
            .runtime_epoch.value();

    component_manager::ManagerSceneStageCoordinator stage(
        stores.apply_journal(), bundle_source,
        stores.active_bundle_publication_store(), scene_owner);
    static_cast<void>(
        stage.StageOrRecover(intent.apply_id, intent.apply_generation));
    component_manager::ManagerSceneActivationCoordinator activation(
        stores.apply_journal(), scene_owner);
    static_cast<void>(
        activation.ActivateOrRecover(intent.apply_id, intent.apply_generation));
    InstalledRuntimeCompositionPort runtime_composition_port;
    component_manager::ManagerSelectedRuntimeCompositionCoordinator
        runtime_composition(stores.apply_journal(), bundle_source,
                            runtime_composition_port,
                            core::RuntimeEpoch("m0-manager-runtime-recovery"));
    const auto installed = runtime_composition.InstallOrRecover(
        intent.apply_id, intent.apply_generation);
    if (installed.state !=
            component_manager::ManagerSelectedRuntimeCompositionResultState::
                kInstalled ||
        !installed.durable_materialization.has_value() ||
        runtime_composition_port.calls != 1) {
      throw std::runtime_error(
          "M0 selected runtime composition did not materialize");
    }
    component_manager::ManagerSelectedBundleTransitionCoordinator selection(
        stores.apply_journal(), bundle_source,
        stores.active_bundle_publication_store(), scene_owner);
    static_cast<void>(
        selection.AdvanceOrRecover(intent.apply_id, intent.apply_generation));
    const auto finalized =
        selection.AdvanceOrRecover(intent.apply_id, intent.apply_generation);
    if (!finalized.durable_finalized.has_value()) {
      throw std::runtime_error("M0 named Apply did not finalize");
    }
    component_manager::ManagerApplyFenceReleaseCoordinator completion(
        stores.apply_journal(), kernel,
        core::RuntimeEpoch("m0-manager-runtime-recovery"));
    const auto terminal =
        completion.ReleaseOrRecover(intent.apply_id, intent.apply_generation);
    if (!terminal.durable_terminal_outcome.record.terminal_outcome.valid()) {
      throw std::runtime_error("M0 named Apply has no terminal success");
    }
    return epoch;
  });
  EXPECT_EQ(recovered_epoch, first_epoch);

  const auto active_epoch = RunExternalManager([&] {
    const auto admission = LoadAdmission(directory);
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        directory.package_root());
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        directory.package_root());
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_root(), admission);
    component_manager::DurableManagerActiveRuntimePlanSource plan_source(
        stores.active_bundle_publication_store(), stores.apply_journal());
    component_manager::RetainedPosixRuntimeUnitAdapter adapter(
        directory.execution_state_root(), directory.execution_run_root(),
        bundle_source, artifact_source, "linux", "posix.1-2017");
    std::uint64_t now = 3'000U;
    component_manager::ManagerActiveRuntimeCoordinator active(
        plan_source, adapter, adapter, [&now] { return ++now; });
    return ActiveEpoch(active.Initialize());
  });
  EXPECT_EQ(active_epoch, first_epoch);

  const auto crashed_pids = ReadRuntimePids(directory.execution_state_root());
  ASSERT_EQ(::kill(crashed_pids.at("pid"), SIGKILL), 0);
  WaitForProcessPairToExit(crashed_pids);

  const auto crash_restarted_epoch = RunExternalManager([&] {
    const auto admission = LoadAdmission(directory);
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        directory.package_root());
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        directory.package_root());
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_root(), admission);
    component_manager::DurableManagerActiveRuntimePlanSource plan_source(
        stores.active_bundle_publication_store(), stores.apply_journal());
    component_manager::RetainedPosixRuntimeUnitAdapter adapter(
        directory.execution_state_root(), directory.execution_run_root(),
        bundle_source, artifact_source, "linux", "posix.1-2017");
    std::uint64_t now = 4'000U;
    component_manager::ManagerActiveRuntimeCoordinator active(
        plan_source, adapter, adapter, [&now] { return ++now; });
    return ActiveEpoch(active.Initialize());
  });
  EXPECT_NE(crash_restarted_epoch, first_epoch);

  EXPECT_EQ(
      RunExternalManager([&] {
        const auto admission = LoadAdmission(directory);
        deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
            directory.package_root());
        deployment::FilesystemVerifiedRetainedProductArtifactSource
            artifact_source(directory.package_root());
        component_manager::ExternallyProvisionedManagerStores stores(
            directory.manager_root(), admission);
        component_manager::DurableManagerActiveRuntimePlanSource plan_source(
            stores.active_bundle_publication_store(), stores.apply_journal());
        component_manager::RetainedPosixRuntimeUnitAdapter adapter(
            directory.execution_state_root(), directory.execution_run_root(),
            bundle_source, artifact_source, "linux", "posix.1-2017");
        std::uint64_t now = 5'000U;
        component_manager::ManagerActiveRuntimeCoordinator active(
            plan_source, adapter, adapter, [&now] { return ++now; });
        const auto stopped = active.Shutdown();
        if (stopped.final_process_observations.size() != 1U ||
            stopped.final_process_observations.front().state() !=
                component_manager::ProcessPresenceState::kAbsent) {
          throw std::runtime_error("M0 orderly shutdown did not prove absence");
        }
        return std::string("stopped");
      }),
      "stopped");

  const auto orderly_restarted_epoch = RunExternalManager([&] {
    const auto admission = LoadAdmission(directory);
    deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
        directory.package_root());
    deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
        directory.package_root());
    component_manager::ExternallyProvisionedManagerStores stores(
        directory.manager_root(), admission);
    component_manager::DurableManagerActiveRuntimePlanSource plan_source(
        stores.active_bundle_publication_store(), stores.apply_journal());
    component_manager::RetainedPosixRuntimeUnitAdapter adapter(
        directory.execution_state_root(), directory.execution_run_root(),
        bundle_source, artifact_source, "linux", "posix.1-2017");
    std::uint64_t now = 6'000U;
    component_manager::ManagerActiveRuntimeCoordinator active(
        plan_source, adapter, adapter, [&now] { return ++now; });
    const auto epoch = ActiveEpoch(active.Initialize());
    static_cast<void>(active.Shutdown());
    return epoch;
  });
  EXPECT_NE(orderly_restarted_epoch, crash_restarted_epoch);

  bool final_state_exists = false;
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           directory.execution_state_root())) {
    final_state_exists =
        final_state_exists || entry.path().filename() == "process.state";
  }
  EXPECT_FALSE(final_state_exists);
}

} // namespace
} // namespace manyforge::scene

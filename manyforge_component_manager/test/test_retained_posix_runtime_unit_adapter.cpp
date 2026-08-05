#include <gtest/gtest.h>
#include <signal.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <manyforge_component_manager/reference_runtime_unit_admission.hpp>
#include <manyforge_component_manager/retained_posix_runtime_unit_adapter.hpp>
#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "runtime_only_retention_fixture.hpp"

namespace manyforge::component_manager {
namespace {

using namespace manyforge::core;

constexpr std::string_view kIntentDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

class ScopedRoot final {
public:
  explicit ScopedRoot(std::string label) {
    auto pattern = (std::filesystem::temp_directory_path() /
                    (std::move(label) + "-XXXXXX"))
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("retained POSIX adapter fixture mkdtemp failed");
    }
    path_ = created;
  }

  ~ScopedRoot() {
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
        "retained POSIX adapter fixture produced no retained bundle");
  }
  return {
      std::move(bundle),
      std::make_unique<deployment::FilesystemVerifiedRetainedBundleSource>(
          root),
      std::make_unique<
          deployment::FilesystemVerifiedRetainedProductArtifactSource>(root),
  };
}

[[nodiscard]] RuntimeUnitConvergenceQueryDTO
Query(const RuntimePackage &package) {
  const auto &unit = package.bundle->value().cell_graph.runtime_units.front();
  const auto admission = VerifyReferenceRuntimeUnitForHost(
      *package.bundle, unit.runtime_unit_id, *package.artifact_source, "linux",
      "posix.1-2017");
  if (!admission.has_value()) {
    throw std::runtime_error("retained POSIX adapter fixture has no admission");
  }
  RuntimeUnitConvergencePlanRowDTO plan{
      unit.runtime_unit_id,
      unit.runtime_profile_id,
      unit.runtime_profile_revision,
      manyforge::core::RuntimeLifecycleControl::kManagerManaged,
      RuntimeUnitCandidateDisposition::kRetainedExact,
      admission->product_id(),
      admission->product_contract_id(),
      admission->product_contract_revision(),
      ContentDigest(admission->product_identity()),
      ContentDigest(admission->configuration_identity()),
      unit.dependency_runtime_unit_ids,
      {RuntimeRoleId(admission->runtime_role_id())},
      EndpointRoleId(admission->status_endpoint_role_id()),
      admission->status_endpoint_contract_id(),
      admission->status_endpoint_contract_revision(),
      admission->status_transport(),
      admission->process_adapter_contract_id(),
      admission->process_adapter_contract_revision(),
      admission->retrieval_contract_id(),
      admission->retrieval_contract_revision(),
      ContentDigest(admission->launchable_content_digest()),
      static_cast<std::uint64_t>(admission->launchable_bytes().size()),
      admission->operating_system(),
      admission->shell_contract(),
  };
  RuntimeUnitConvergenceQueryDTO query{
      ApplyId("apply-1"),
      ApplyGeneration(1U),
      package.bundle->retained_bundle_id(),
      package.bundle->value().cell_graph.graph_revision,
      ContentDigest(std::string(kIntentDigest)),
      std::move(plan),
  };
  if (!query.valid()) {
    throw std::runtime_error("retained POSIX adapter fixture query is invalid");
  }
  return query;
}

[[nodiscard]] std::filesystem::path
FindMaterializedLaunchable(const std::filesystem::path &runtime_root) {
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(runtime_root)) {
    if (entry.path().filename() == "launchable") {
      return entry.path();
    }
  }
  throw std::runtime_error("retained POSIX adapter materialized no launchable");
}

[[nodiscard]] std::filesystem::path
FindProcessState(const std::filesystem::path &runtime_root) {
  for (const auto &entry :
       std::filesystem::recursive_directory_iterator(runtime_root)) {
    if (entry.path().filename() == "process.state") {
      return entry.path();
    }
  }
  throw std::runtime_error("retained POSIX adapter produced no process state");
}

[[nodiscard]] std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream.good()) {
    throw std::runtime_error("retained POSIX adapter fixture could not read " +
                             path.string());
  }
  return std::string((std::istreambuf_iterator<char>(stream)),
                     std::istreambuf_iterator<char>());
}

[[nodiscard]] std::string ReadAllFromFd(const int fd) {
  std::string result;
  char byte = 0;
  for (;;) {
    const auto count = ::read(fd, &byte, 1U);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0 || byte == '\n') {
      break;
    }
    result.push_back(byte);
  }
  return result;
}

[[nodiscard]] pid_t WaitForTracedFork(const pid_t tracee) {
  for (;;) {
    int status = 0;
    if (::waitpid(tracee, &status, __WALL) != tracee) {
      throw std::runtime_error("retained POSIX adapter trace wait failed");
    }
    if (!WIFSTOPPED(status)) {
      throw std::runtime_error(
          "retained POSIX adapter tracee exited before fork");
    }
    const auto event = static_cast<unsigned int>(status) >> 16U;
    if (event == PTRACE_EVENT_FORK) {
      unsigned long child = 0UL;
      if (::ptrace(PTRACE_GETEVENTMSG, tracee, nullptr, &child) != 0 ||
          child == 0UL ||
          child >
              static_cast<unsigned long>(std::numeric_limits<pid_t>::max())) {
        throw std::runtime_error(
            "retained POSIX adapter trace returned an invalid child");
      }
      const auto child_pid = static_cast<pid_t>(child);
      int child_status = 0;
      if (::waitpid(child_pid, &child_status, __WALL) != child_pid ||
          !WIFSTOPPED(child_status)) {
        throw std::runtime_error(
            "retained POSIX adapter traced child was not stopped");
      }
      return child_pid;
    }
    if (::ptrace(PTRACE_CONT, tracee, nullptr, nullptr) != 0) {
      throw std::runtime_error(
          "retained POSIX adapter trace could not continue to fork");
    }
  }
}

void SetTraceForkAndContinue(const pid_t tracee) {
  const auto options =
      reinterpret_cast<void *>(static_cast<std::uintptr_t>(PTRACE_O_TRACEFORK));
  if (::ptrace(PTRACE_SETOPTIONS, tracee, nullptr, options) != 0 ||
      ::ptrace(PTRACE_CONT, tracee, nullptr, nullptr) != 0) {
    throw std::runtime_error(
        "retained POSIX adapter could not arm fork tracing");
  }
}

void DetachTracedProcess(const pid_t tracee) {
  if (::ptrace(PTRACE_DETACH, tracee, nullptr, nullptr) != 0) {
    throw std::runtime_error(
        "retained POSIX adapter could not detach traced process");
  }
}

TEST(RetainedPosixRuntimeUnitAdapter,
     MaterializesExactBytesStartsOnceAndReturnsNewDirectStatusRecords) {
  ScopedRoot package_root("manyforge-retained-posix-package");
  ScopedRoot runtime_root("manyforge-retained-posix-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  const auto query = Query(package);

  RetainedPosixRuntimeUnitAdapter adapter(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  const auto absent = adapter.ObserveProcess(query);
  EXPECT_EQ(absent.state(), ProcessPresenceState::kAbsent);

  const auto started = adapter.EnsureRunning(query);
  ASSERT_EQ(started.state(), ProcessPresenceState::kPresent);
  ASSERT_FALSE(started.process_incarnation().empty());
  const auto replay = adapter.EnsureRunning(query);
  EXPECT_EQ(replay.state(), ProcessPresenceState::kPresent);
  EXPECT_EQ(replay.process_incarnation(), started.process_incarnation());

  const RuntimeEpoch epoch(started.process_incarnation());
  const auto initial = adapter.ObserveProgress(query, epoch);
  ASSERT_TRUE(initial.has_value());
  EXPECT_EQ(initial->source_report_sequence(), 1U);
  EXPECT_EQ(initial->critical_progress_sequence(), 1U);
  EXPECT_EQ(initial->runtime_unit_id(), query.plan.runtime_unit_id.value());
  EXPECT_EQ(initial->process_incarnation(), epoch.value());
  EXPECT_EQ(initial->product_identity(), query.plan.product_identity.value());
  EXPECT_EQ(initial->configuration_identity(),
            query.plan.configuration_identity.value());
  EXPECT_EQ(initial->ready_runtime_role_ids(),
            (std::vector<std::string>{"manager_fixture"}));
  EXPECT_EQ(initial->command_surface_state(),
            ManagedCommandSurfaceState::kClosedNoActiveEffect);

  const auto requested = adapter.ObserveProgress(query, epoch);
  ASSERT_TRUE(requested.has_value());
  EXPECT_EQ(requested->source_report_sequence(), 2U);

  const auto launchable = FindMaterializedLaunchable(runtime_root.path());
  std::ifstream stream(launchable, std::ios::binary);
  const std::string bytes((std::istreambuf_iterator<char>(stream)),
                          std::istreambuf_iterator<char>());
  const auto admission = VerifyReferenceRuntimeUnitForHost(
      *package.bundle, query.plan.runtime_unit_id, *package.artifact_source,
      "linux", "posix.1-2017");
  ASSERT_TRUE(admission.has_value());
  EXPECT_EQ(bytes, admission->launchable_bytes());

  RetainedPosixRuntimeUnitAdapter observer(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  const auto observed = observer.ObserveProcess(query);
  EXPECT_EQ(observed.state(), ProcessPresenceState::kPresent);
  EXPECT_EQ(observed.process_incarnation(), epoch.value());
  const auto reconciled = observer.EnsureRunning(query);
  EXPECT_EQ(reconciled.process_incarnation(), epoch.value());
  const auto reconnected = observer.ObserveProgress(query, epoch);
  ASSERT_TRUE(reconnected.has_value());
  EXPECT_EQ(reconnected->source_report_sequence(), 3U);

  EXPECT_EQ(observer.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
  EXPECT_EQ(observer.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
}

TEST(RetainedPosixRuntimeUnitAdapter,
     ManagerProcessExitLeavesRuntimeAndStatusChannelReconnectable) {
  ScopedRoot package_root("manyforge-retained-posix-crash-package");
  ScopedRoot runtime_root("manyforge-retained-posix-crash-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  const auto query = Query(package);

  int epoch_pipe[2]{-1, -1};
  ASSERT_EQ(::pipe(epoch_pipe), 0);
  const pid_t manager = ::fork();
  ASSERT_GE(manager, 0);
  if (manager == 0) {
    static_cast<void>(::close(epoch_pipe[0]));
    try {
      deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
          package_root.path());
      deployment::FilesystemVerifiedRetainedProductArtifactSource
          artifact_source(package_root.path());
      RetainedPosixRuntimeUnitAdapter adapter(runtime_root.path(),
                                              bundle_source, artifact_source,
                                              "linux", "posix.1-2017");
      const auto started = adapter.EnsureRunning(query);
      const auto report = adapter.ObserveProgress(
          query, RuntimeEpoch(started.process_incarnation()));
      if (!report.has_value()) {
        ::_exit(71);
      }
      const std::string message = started.process_incarnation() + "\n";
      std::size_t offset = 0U;
      while (offset < message.size()) {
        const auto written = ::write(epoch_pipe[1], message.data() + offset,
                                     message.size() - offset);
        if (written < 0 && errno == EINTR) {
          continue;
        }
        if (written <= 0) {
          ::_exit(72);
        }
        offset += static_cast<std::size_t>(written);
      }
      ::_exit(0); // Deliberately bypass adapter destruction.
    } catch (...) {
      ::_exit(73);
    }
  }

  static_cast<void>(::close(epoch_pipe[1]));
  const std::string old_epoch = ReadAllFromFd(epoch_pipe[0]);
  static_cast<void>(::close(epoch_pipe[0]));
  int manager_status = 0;
  ASSERT_EQ(::waitpid(manager, &manager_status, 0), manager);
  ASSERT_TRUE(WIFEXITED(manager_status));
  ASSERT_EQ(WEXITSTATUS(manager_status), 0);
  ASSERT_FALSE(old_epoch.empty());
  const auto durable_state_before_replacement =
      ReadFile(FindProcessState(runtime_root.path()));

  RetainedPosixRuntimeUnitAdapter replacement(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  const auto observed = replacement.ObserveProcess(query);
  ASSERT_EQ(observed.state(), ProcessPresenceState::kPresent);
  EXPECT_EQ(observed.process_incarnation(), old_epoch);
  const auto reconciled = replacement.EnsureRunning(query);
  EXPECT_EQ(reconciled.process_incarnation(), old_epoch);
  EXPECT_EQ(ReadFile(FindProcessState(runtime_root.path())),
            durable_state_before_replacement);
  const auto fresh = replacement.ObserveProgress(
      query, RuntimeEpoch(reconciled.process_incarnation()));
  ASSERT_TRUE(fresh.has_value());
  EXPECT_EQ(fresh->source_report_sequence(), 2U);

  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
  const auto restarted = replacement.EnsureRunning(query);
  ASSERT_EQ(restarted.state(), ProcessPresenceState::kPresent);
  EXPECT_NE(restarted.process_incarnation(), old_epoch);
  const auto restarted_report = replacement.ObserveProgress(
      query, RuntimeEpoch(restarted.process_incarnation()));
  ASSERT_TRUE(restarted_report.has_value());
  EXPECT_EQ(restarted_report->source_report_sequence(), 1U);
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
}

TEST(RetainedPosixRuntimeUnitAdapter,
     ManagerDeathBeforeDurableStateKeepsBootstrapPairLockedUntilAbsent) {
  ScopedRoot package_root("manyforge-retained-posix-bootstrap-crash-package");
  ScopedRoot runtime_root("manyforge-retained-posix-bootstrap-crash-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  const auto query = Query(package);

  const pid_t manager = ::fork();
  ASSERT_GE(manager, 0);
  if (manager == 0) {
    if (::ptrace(PTRACE_TRACEME, 0, nullptr, nullptr) != 0 ||
        ::raise(SIGSTOP) != 0) {
      ::_exit(74);
    }
    try {
      deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
          package_root.path());
      deployment::FilesystemVerifiedRetainedProductArtifactSource
          artifact_source(package_root.path());
      RetainedPosixRuntimeUnitAdapter adapter(runtime_root.path(),
                                              bundle_source, artifact_source,
                                              "linux", "posix.1-2017");
      static_cast<void>(adapter.EnsureRunning(query));
      ::_exit(75);
    } catch (...) {
      ::_exit(76);
    }
  }

  int manager_status = 0;
  ASSERT_EQ(::waitpid(manager, &manager_status, __WALL), manager);
  ASSERT_TRUE(WIFSTOPPED(manager_status));
  SetTraceForkAndContinue(manager);
  const pid_t controller = WaitForTracedFork(manager);

  ASSERT_EQ(::kill(manager, SIGKILL), 0);
  ASSERT_EQ(::waitpid(manager, &manager_status, __WALL), manager);
  ASSERT_TRUE(WIFSIGNALED(manager_status));
  ASSERT_EQ(WTERMSIG(manager_status), SIGKILL);

  SetTraceForkAndContinue(controller);
  const pid_t gated_runtime = WaitForTracedFork(controller);

  RetainedPosixRuntimeUnitAdapter replacement(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  bool rejected_while_bootstrap_pair_existed = false;
  try {
    static_cast<void>(replacement.EnsureStopped(query));
  } catch (const RetainedPosixRuntimeUnitAdapterError &error) {
    rejected_while_bootstrap_pair_existed = true;
    EXPECT_EQ(std::string(error.what()), "runtime adapter slot lock timed out");
  }

  DetachTracedProcess(gated_runtime);
  DetachTracedProcess(controller);
  EXPECT_TRUE(rejected_while_bootstrap_pair_existed)
      << "replacement reported absence while the abandoned bootstrap pair "
         "was stopped";
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
}

TEST(RetainedPosixRuntimeUnitAdapter,
     QueryMismatchRejectsBeforeRuntimeRootMutation) {
  ScopedRoot package_root("manyforge-retained-posix-mismatch-package");
  ScopedRoot runtime_root("manyforge-retained-posix-mismatch-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  auto query = Query(package);
  query.plan.status_endpoint_contract_revision = "foreign-revision";
  ASSERT_TRUE(query.valid());

  RetainedPosixRuntimeUnitAdapter adapter(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  EXPECT_THROW(static_cast<void>(adapter.EnsureRunning(query)),
               RetainedPosixRuntimeUnitAdapterError);
  EXPECT_TRUE(std::filesystem::is_empty(runtime_root.path()));
}

TEST(RetainedPosixRuntimeUnitAdapter,
     MaterializedByteConflictRejectsBeforeAnotherStart) {
  ScopedRoot package_root("manyforge-retained-posix-tamper-package");
  ScopedRoot runtime_root("manyforge-retained-posix-tamper-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  const auto query = Query(package);
  {
    RetainedPosixRuntimeUnitAdapter adapter(
        runtime_root.path(), *package.bundle_source, *package.artifact_source,
        "linux", "posix.1-2017");
    ASSERT_EQ(adapter.EnsureRunning(query).state(),
              ProcessPresenceState::kPresent);
    ASSERT_EQ(adapter.EnsureStopped(query).state(),
              ProcessPresenceState::kAbsent);
  }

  const auto launchable = FindMaterializedLaunchable(runtime_root.path());
  std::filesystem::permissions(launchable, std::filesystem::perms::owner_all,
                               std::filesystem::perm_options::replace);
  {
    std::fstream stream(launchable,
                        std::ios::binary | std::ios::in | std::ios::out);
    ASSERT_TRUE(stream.good());
    stream.seekp(0);
    stream.put('X');
  }

  RetainedPosixRuntimeUnitAdapter replacement(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  EXPECT_THROW(static_cast<void>(replacement.EnsureRunning(query)),
               RetainedPosixRuntimeUnitAdapterError);
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
}

TEST(RetainedPosixRuntimeUnitAdapter,
     CrashLeftFixedTemporaryIsRemovedWhileSurvivingEpochIsReconciled) {
  ScopedRoot package_root("manyforge-retained-posix-temp-package");
  ScopedRoot runtime_root("manyforge-retained-posix-temp-runtime");
  const auto package = MaterializeRuntimePackage(package_root.path());
  const auto query = Query(package);
  std::filesystem::path slot;
  std::string old_epoch;
  {
    RetainedPosixRuntimeUnitAdapter adapter(
        runtime_root.path(), *package.bundle_source, *package.artifact_source,
        "linux", "posix.1-2017");
    const auto started = adapter.EnsureRunning(query);
    ASSERT_EQ(started.state(), ProcessPresenceState::kPresent);
    old_epoch = started.process_incarnation();
    slot = FindMaterializedLaunchable(runtime_root.path()).parent_path();
  }

  const auto stale_temporary = slot / ".process.state.tmp";
  {
    std::ofstream stream(stale_temporary, std::ios::binary);
    ASSERT_TRUE(stream.good());
    stream << "interrupted-state-write";
  }

  RetainedPosixRuntimeUnitAdapter replacement(
      runtime_root.path(), *package.bundle_source, *package.artifact_source,
      "linux", "posix.1-2017");
  const auto restarted = replacement.EnsureRunning(query);
  ASSERT_EQ(restarted.state(), ProcessPresenceState::kPresent);
  EXPECT_EQ(restarted.process_incarnation(), old_epoch);
  EXPECT_FALSE(std::filesystem::exists(stale_temporary));
  EXPECT_TRUE(
      replacement
          .ObserveProgress(query, RuntimeEpoch(restarted.process_incarnation()))
          .has_value());
  EXPECT_EQ(replacement.EnsureStopped(query).state(),
            ProcessPresenceState::kAbsent);
}

} // namespace
} // namespace manyforge::component_manager

#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/native_execution_runtime_unit_admission.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "manyforge_headless/cell_admission_readiness_reducer.hpp"
#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

core::ProgramCycleStatusDTO IdleCycleStatus() {
  return {core::RequestOwnerEpoch("runner-owner"),
          4U,
          core::ProgramCycleStateDTO::kIdleAfterReset,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

TEST(ManagerCellAdmissionReadinessReducer,
     ProjectsOnlyOwnerCurrentHealthyConvergedSettledFacts) {
  const auto reduced = ReduceManagerCellAdmissionReadiness(
      {true, IdleCycleStatus(), true, 3U, 3U, true, true, 0U, 0U, 0U});

  EXPECT_TRUE(reduced.program_cycle_status_current);
  ASSERT_TRUE(reduced.program_cycle_status.has_value());
  EXPECT_EQ(reduced.program_cycle_status->state,
            core::ProgramCycleStateDTO::kIdleAfterReset);
  EXPECT_EQ(reduced.required_component_total, 3U);
  EXPECT_EQ(reduced.required_component_current_compatible_ready_converged, 3U);
  EXPECT_TRUE(reduced.deployment_status_current);
  EXPECT_TRUE(reduced.deployment_converged);
  EXPECT_EQ(reduced.blocking_fault_count, 0U);
  EXPECT_EQ(reduced.unresolved_owner_count, 0U);
  EXPECT_EQ(reduced.unsettled_effect_count, 0U);
}

TEST(ManagerCellAdmissionReadinessReducer,
     PreservesRunningStaleFaultedAndUnsettledClosureFacts) {
  auto running = IdleCycleStatus();
  running.state = core::ProgramCycleStateDTO::kRunning;
  running.program_run_id = core::ProgramRunId("run");
  running.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  const auto reduced = ReduceManagerCellAdmissionReadiness(
      {false, running, false, 3U, 2U, false, false, 1U, 1U, 1U});

  ASSERT_TRUE(reduced.program_cycle_status.has_value());
  EXPECT_EQ(reduced.program_cycle_status->state,
            core::ProgramCycleStateDTO::kRunning);
  EXPECT_FALSE(reduced.program_cycle_status_current);
  EXPECT_FALSE(reduced.required_component_status_current);
  EXPECT_NE(reduced.required_component_total,
            reduced.required_component_current_compatible_ready_converged);
  EXPECT_FALSE(reduced.deployment_status_current);
  EXPECT_FALSE(reduced.deployment_converged);
  EXPECT_EQ(reduced.blocking_fault_count, 1U);
  EXPECT_EQ(reduced.unresolved_owner_count, 1U);
  EXPECT_EQ(reduced.unsettled_effect_count, 1U);
}

TEST(ManagerCellAdmissionReadinessReducer,
     AcceptsOnlyTheCurrentProgramRunnerOwner) {
  const auto before = IdleCycleStatus();
  auto after = before;
  after.program_runner_owner_epoch =
      core::RequestOwnerEpoch("runner-successor");
  EXPECT_TRUE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));

  // Cycle progression is owned by the Runner and may happen between Manager's
  // probe and renewal exchange.
  after.cycle_generation += 1U;
  EXPECT_TRUE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));
  after = before;
  after.program_runner_owner_epoch =
      core::RequestOwnerEpoch("foreign-successor");
  EXPECT_FALSE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));
}

TEST(ManagerCellAdmissionReadinessReducer,
     IdleToCapturedRunDoesNotInventAnAdmissionOutage) {
  const auto before = ReduceManagerCellAdmissionReadiness(
      {true, IdleCycleStatus(), true, 3U, 3U, true, true, 0U, 0U, 0U});
  auto after = before;
  auto &cycle = *after.program_cycle_status;
  cycle.state = core::ProgramCycleStateDTO::kStarting;
  cycle.program_run_id = core::ProgramRunId("run");
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  cycle.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  cycle.state = core::ProgramCycleStateDTO::kRunning;
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  EXPECT_FALSE(IsSameProgramAdmissionProgress(after, before));
  for (const auto state : {core::ProgramCycleStateDTO::kStopping,
                           core::ProgramCycleStateDTO::kResetting,
                           core::ProgramCycleStateDTO::kTerminalAwaitingReset,
                           core::ProgramCycleStateDTO::kRecoveryRequired}) {
    auto changed = after;
    changed.program_cycle_status->state = state;
    EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  }
  auto changed = after;
  changed.program_cycle_status->program_runner_owner_epoch =
      core::RequestOwnerEpoch("foreign");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  ++changed.program_cycle_status->cycle_generation;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.blocking_fault_count = 1U;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.program_cycle_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
}

TEST(ManagerCellAdmissionReadinessReducer,
     StartingToRunningDoesNotInventAnAdmissionOutage) {
  auto starting = IdleCycleStatus();
  starting.state = core::ProgramCycleStateDTO::kStarting;
  starting.program_run_id = core::ProgramRunId("run");
  auto before = ReduceManagerCellAdmissionReadiness(
      {true, starting, true, 3U, 3U, true, true, 0U, 0U, 0U});
  auto after = before;
  after.program_cycle_status->state = core::ProgramCycleStateDTO::kRunning;
  after.program_cycle_status->cycle_claim_generation =
      core::CellAdmissionClaimGeneration(9U);
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  auto claimed_starting = after;
  claimed_starting.program_cycle_status->state =
      core::ProgramCycleStateDTO::kStarting;
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, claimed_starting));
  EXPECT_TRUE(IsSameProgramAdmissionProgress(claimed_starting, after));
  auto replaced_claim = claimed_starting;
  replaced_claim.program_cycle_status->cycle_claim_generation =
      core::CellAdmissionClaimGeneration(10U);
  EXPECT_FALSE(
      IsSameProgramAdmissionProgress(claimed_starting, replaced_claim));
  for (const auto state :
       {core::ProgramCycleStateDTO::kStopping,
        core::ProgramCycleStateDTO::kResetting,
        core::ProgramCycleStateDTO::kTerminalAwaitingReset}) {
    auto changed = after;
    changed.program_cycle_status->state = state;
    EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  }
  auto changed = after;
  changed.program_cycle_status->program_runner_owner_epoch =
      core::RequestOwnerEpoch("other");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.program_cycle_status->program_run_id = core::ProgramRunId("other");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  ++changed.program_cycle_status->cycle_generation;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.blocking_fault_count = 1;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.deployment_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  before.program_cycle_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, after));
}

constexpr std::string_view kDeploymentRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

const content_identity::CanonicalValue &
RequiredMember(const content_identity::CanonicalValue &value,
               const std::string_view name) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("Manager source fixture lacks " +
                             std::string(name));
  }
  return *member;
}

/// Whether one Manager source value pairs its active publication with an
/// Apply history of the same generation: publication 1 is the bootstrap and
/// each later one is exactly one committed named Apply (the invariant Headless
/// enforces as program_activation_history_incomplete).
bool ManagerSourceHistoryIsOneGeneration(
    const content_identity::CanonicalValue &payload) {
  const auto generation =
      RequiredMember(RequiredMember(RequiredMember(payload, "active"),
                                    "publication"),
                     "publication_generation")
          .as_number();
  double committed = 0.0;
  for (const auto &record :
       RequiredMember(RequiredMember(payload, "apply"), "records").as_array()) {
    if (RequiredMember(record, "committed").as_bool()) {
      committed += 1.0;
    }
  }
  return committed + 1.0 == generation;
}

const std::string &RequiredText(const content_identity::CanonicalValue &value,
                                const std::string_view name) {
  const auto *text =
      std::get_if<std::string>(&RequiredMember(value, name).storage());
  if (text == nullptr || text->empty()) {
    throw std::runtime_error("Manager Apply metadata has invalid " +
                             std::string(name));
  }
  return *text;
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
struct RuntimeOfferProjection final {
  std::string selected;
  std::string observed;
  std::string observed_epoch;
};

RuntimeOfferProjection
ReadRuntimeOfferProjection(const content_identity::CanonicalValue &payload) {
  const auto &runtime = RequiredMember(payload, "runtime");
  const auto &desired = RequiredMember(runtime, "desired_units").as_array();
  const auto &observed = RequiredMember(runtime, "observed_units").as_array();
  if (desired.size() != 1U || observed.size() != 1U) {
    throw std::runtime_error(
        "Manager source fixture must contain one runtime projection row");
  }
  return {
      RequiredText(desired.front(), "selected_runtime_contract_offer_digest"),
      RequiredText(observed.front(), "observed_runtime_contract_offer_digest"),
      RequiredText(observed.front(), "runtime_epoch"),
  };
}

struct ManagerApplyFixtureMetadata final {
  std::string admission_filename;
  std::string candidate_retained_bundle_id;
  std::string cell_id;
  std::string deployment_artifact_id;
  std::string deployment_revision;
  std::string physical_candidate_retained_bundle_id;
  std::string physical_deployment_artifact_id;
  std::string physical_deployment_revision;
  std::string program_binding_id;
  std::string program_id;
  std::string program_revision;
  std::string predecessor_graph_revision;
  std::string predecessor_retained_bundle_id;
  content_identity::CanonicalValue program_role_assignment;
};

ManagerApplyFixtureMetadata ReadManagerApplyFixtureMetadata(
    const std::filesystem::path &metadata_path =
        MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA) {
  std::ifstream stream(metadata_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Manager Apply fixture metadata is absent");
  }
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  const auto root = deployment::parse_strict_json_model_yaml(bytes).root();
  if (!root.is_object() || root.as_object().size() != 14U ||
      content_identity::canonical_json(root) != bytes) {
    throw std::runtime_error("Manager Apply fixture metadata is invalid");
  }
  return {
      RequiredText(root, "admission_filename"),
      RequiredText(root, "candidate_retained_bundle_id"),
      RequiredText(root, "cell_id"),
      RequiredText(root, "deployment_artifact_id"),
      RequiredText(root, "deployment_revision"),
      RequiredText(root, "physical_candidate_retained_bundle_id"),
      RequiredText(root, "physical_deployment_artifact_id"),
      RequiredText(root, "physical_deployment_revision"),
      RequiredText(root, "program_binding_id"),
      RequiredText(root, "program_id"),
      RequiredText(root, "program_revision"),
      RequiredText(root, "predecessor_graph_revision"),
      RequiredText(root, "predecessor_retained_bundle_id"),
      RequiredMember(root, "program_role_assignment"),
  };
}

struct GripperOnlyManagerApplyFixtureMetadata final {
  std::string admission_filename;
  std::string cell_id;
  std::string prepared_activation_id;
  std::string physical_candidate_retained_bundle_id;
  std::string physical_deployment_artifact_id;
  std::string physical_deployment_revision;
  std::string predecessor_graph_revision;
  std::string predecessor_retained_bundle_id;
};

GripperOnlyManagerApplyFixtureMetadata ReadGripperOnlyManagerApplyFixtureMetadata(
    const std::filesystem::path &metadata_path =
        MANYFORGE_HEADLESS_TEST_GRIPPER_ONLY_METADATA) {
  std::ifstream stream(metadata_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("gripper-only Manager Apply fixture metadata is absent");
  }
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  const auto root = deployment::parse_strict_json_model_yaml(bytes).root();
  if (!root.is_object() || root.as_object().size() != 11U ||
      content_identity::canonical_json(root) != bytes) {
    throw std::runtime_error(
        "gripper-only Manager Apply fixture metadata is invalid");
  }
  const auto &prepared = RequiredMember(root, "prepared_activations");
  if (!prepared.is_object() || prepared.as_object().empty()) {
    throw std::runtime_error(
        "gripper-only Manager Apply fixture has no prepared activations");
  }
  static_cast<void>(RequiredText(root, "physical_graph_revision"));
  static_cast<void>(RequiredText(root, "program_binding_id"));
  static_cast<void>(RequiredText(root, "program_revision"));
  const auto &apply_activation = RequiredMember(prepared, "program_apply_fixture");
  return {
      RequiredText(root, "admission_filename"),
      RequiredText(root, "cell_id"),
      RequiredText(apply_activation, "prepared_activation_id"),
      RequiredText(root, "physical_candidate_retained_bundle_id"),
      RequiredText(root, "physical_deployment_artifact_id"),
      RequiredText(root, "physical_deployment_revision"),
      RequiredText(root, "predecessor_graph_revision"),
      RequiredText(root, "predecessor_retained_bundle_id"),
  };
}


#endif

class DaemonDirectory final {
public:
  explicit DaemonDirectory(bool preserve_on_failure = false)
      : preserve_on_failure_(preserve_on_failure) {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-daemon-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("Manager daemon test mkdtemp failed");
    }
    root_ = created;
    std::filesystem::create_directory(manager_root());
    std::filesystem::create_directory(manager_run_root());
    std::filesystem::create_directory(execution_state_root());
    std::filesystem::create_directory(execution_run_root());
    std::filesystem::create_directory(publication_root());
    std::filesystem::create_directory(activation_root());
  }

  ~DaemonDirectory() {
    if (preserve_on_failure_ &&
        (::testing::Test::HasFailure() || std::uncaught_exceptions() > initial_uncaught_exceptions_)) {
      // One private tree per run. Never delete a previous failed run here.
      std::cerr << "Manager process failure evidence retained at " << root_
                << '\n';
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
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
  [[nodiscard]] std::filesystem::path manager_run_root() const {
    return root_ / "manager-run";
  }
  [[nodiscard]] std::filesystem::path publication_root() const {
    return root_ / "publications";
  }
  [[nodiscard]] std::filesystem::path scene_database() const {
    return root_ / "logical-scene.sqlite3";
  }
  [[nodiscard]] std::filesystem::path program_cycle_journal() const {
    return root_ / "program-cycle.sqlite3";
  }
  [[nodiscard]] std::filesystem::path socket_path() const {
    return manager_run_root() / "manager-command.sock";
  }
  [[nodiscard]] std::filesystem::path startup_stderr() const {
    return root_ / "manager-startup.stderr";
  }
  [[nodiscard]] std::filesystem::path execution_socket_path() const {
    return execution_run_root() / "execution-command.sock";
  }
  [[nodiscard]] std::filesystem::path
  program_runner_liveness_socket_path() const {
    return manager_run_root() / "program-runner-liveness.sock";
  }
  [[nodiscard]] std::filesystem::path program_runner_log_path() const {
    return root_ / "program-runner-process.stderr";
  }
  [[nodiscard]] std::filesystem::path installed_root() const {
    return root_ / "installed";
  }
  [[nodiscard]] std::filesystem::path apply_package_root() const {
    return root_ / "apply-package";
  }
  /// Trusted activation store: the Manager admits only prepared values
  /// published here (disjoint from every install/runtime root).
  [[nodiscard]] std::filesystem::path activation_root() const {
    return root_ / "activation";
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path root_;
  bool preserve_on_failure_;
  const int initial_uncaught_exceptions_{std::uncaught_exceptions()};
};

std::string SendCommand(const std::filesystem::path &path,
                        const std::string &request);
void SendCommandAndDisconnect(const std::filesystem::path &path,
                              const std::string &request);

class ManagerProcess final {
public:
  explicit ManagerProcess(
      const DaemonDirectory &directory,
      std::string package_root =
          MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
      std::string admission_filename = "canonical-empty-admission.json",
      std::string executable = MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON,
      std::string program_runner_liveness_socket = {},
      std::string projection_root = {}) {
    const auto manager_root = directory.manager_root().string();
    const auto manager_run_root = directory.manager_run_root().string();
    const auto execution_state_root = directory.execution_state_root().string();
    const auto execution_run_root = directory.execution_run_root().string();
    const auto scene_database = directory.scene_database().string();
    const auto program_cycle_journal =
        directory.program_cycle_journal().string();
    const auto publication_root = projection_root.empty()
                                      ? directory.publication_root().string()
                                      : projection_root;
    const auto startup_stderr = directory.startup_stderr().string();
    const auto activation_root = directory.activation_root().string();
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("Manager daemon test fork failed");
    }
    if (pid_ == 0) {
      const int stderr_fd =
          ::open(startup_stderr.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (stderr_fd < 0 || ::dup2(stderr_fd, STDERR_FILENO) < 0) {
        ::_exit(127);
      }
      static_cast<void>(::close(stderr_fd));
      if (program_runner_liveness_socket.empty()) {
        ::execl(executable.c_str(), executable.c_str(), "--install-root",
                package_root.c_str(), "--activation-root",
                activation_root.c_str(), "--canonical-empty-admission-filename",
                admission_filename.c_str(), "--manager-state-root",
                manager_root.c_str(), "--manager-run-root",
                manager_run_root.c_str(), "--execution-state-root",
                execution_state_root.c_str(), "--execution-run-root",
                execution_run_root.c_str(), "--logical-scene-database",
                scene_database.c_str(), "--program-cycle-journal",
                program_cycle_journal.c_str(), "--projection-root",
                publication_root.c_str(), "--reconciliation-period-ms", "10",
                static_cast<char *>(nullptr));
      } else {
        ::execl(
            executable.c_str(), executable.c_str(), "--install-root",
            package_root.c_str(), "--activation-root", activation_root.c_str(),
            "--canonical-empty-admission-filename",
            admission_filename.c_str(), "--manager-state-root",
            manager_root.c_str(), "--manager-run-root",
            manager_run_root.c_str(), "--execution-state-root",
            execution_state_root.c_str(), "--execution-run-root",
            execution_run_root.c_str(), "--logical-scene-database",
            scene_database.c_str(), "--program-cycle-journal",
            program_cycle_journal.c_str(), "--projection-root",
            publication_root.c_str(), "--program-runner-liveness-socket",
            program_runner_liveness_socket.c_str(),
            "--reconciliation-period-ms", "10", static_cast<char *>(nullptr));
      }
      ::_exit(127);
    }
  }

  ~ManagerProcess() {
    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(pid_, &status, 0));
    }
  }

  ManagerProcess(const ManagerProcess &) = delete;
  ManagerProcess &operator=(const ManagerProcess &) = delete;

  void WaitReady(const DaemonDirectory &directory) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        std::ifstream input(directory.startup_stderr(), std::ios::binary);
        const std::string detail{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
        throw std::runtime_error(
            "Manager daemon exited before readiness" +
            (detail.empty() ? std::string{} : ": " + detail));
      }
      if (std::filesystem::is_socket(directory.socket_path()) &&
          std::filesystem::is_regular_file(directory.publication_root() /
                                           "manager-source.json") &&
          std::filesystem::is_regular_file(directory.publication_root() /
                                           "logical-scene-source.json")) {
        try {
          const auto response =
              SendCommand(directory.socket_path(),
                          "{\"api_version\":\"manyforge.manager_command.v1\","
                          "\"command\":\"reconcile\"}");
          if (response.find("\"ok\":true") != std::string::npos) {
            return;
          }
        } catch (...) {
          // A stale socket may survive an ungraceful predecessor. Readiness
          // requires a response from the replacement, not path existence.
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Manager daemon readiness timed out");
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }

  int Stop(const int signal) {
    if (pid_ <= 0) {
      throw std::runtime_error("Manager daemon is not running");
    }
    if (::kill(pid_, signal) != 0) {
      throw std::runtime_error("Manager daemon signal failed");
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) != pid_) {
      throw std::runtime_error("Manager daemon wait failed");
    }
    pid_ = -1;
    return status;
  }

  int Wait() {
    if (pid_ <= 0) {
      throw std::runtime_error("Manager daemon is not running");
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) != pid_) {
      throw std::runtime_error("Manager daemon wait failed");
    }
    pid_ = -1;
    return status;
  }

private:
  pid_t pid_{-1};
};

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
class ProgramRunnerProcess final {
public:
  ProgramRunnerProcess(const DaemonDirectory &directory,
                       const std::string &cell_id)
      : socket_path_(directory.program_runner_liveness_socket_path()),
        log_path_(directory.program_runner_log_path()) {
    const auto execution_run_root = directory.execution_run_root().string();
    const auto journal = directory.program_cycle_journal().string();
    const auto socket = socket_path_.string();
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("Program Runner test fork failed");
    }
    if (pid_ == 0) {
      const auto log = log_path_.string();
      const int log_fd =
          ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                 S_IRUSR | S_IWUSR);
      if (log_fd < 0 || ::dup2(log_fd, STDOUT_FILENO) < 0 ||
          ::dup2(log_fd, STDERR_FILENO) < 0) {
        ::_exit(127);
      }
      static_cast<void>(::close(log_fd));
      const char *existing = ::getenv("PYTHONPATH");
      std::string python_path(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT);
      python_path.append(":")
          .append(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT)
          .append("/manyforge_behavior:")
          .append(MANYFORGE_HEADLESS_TEST_PYTHON_MODULE_ROOT);
      if (existing != nullptr && *existing != '\0') {
        python_path.append(":").append(existing);
      }
      if (::setenv("PYTHONPATH", python_path.c_str(), 1) != 0) {
        ::_exit(127);
      }
      ::execl(
          MANYFORGE_HEADLESS_TEST_PYTHON, MANYFORGE_HEADLESS_TEST_PYTHON, "-m",
          "manyforge_program_runner", "--host", "127.0.0.1", "--port", "0",
          "--headless-url", "http://127.0.0.1:1", "--execution-run-root",
          execution_run_root.c_str(), "--cycle-journal-path", journal.c_str(),
          "--manager-liveness-socket", socket.c_str(), "--cell-id",
          cell_id.c_str(), "--owner-id", "program-runner.manager-process-test",
          "--headless-timeout-s", "0.25", static_cast<char *>(nullptr));
      ::_exit(127);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        throw std::runtime_error("Program Runner exited before liveness ready" +
                                 LogDetail());
      }
      std::error_code error;
      if (std::filesystem::is_socket(socket_path_, error) && !error) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    static_cast<void>(::kill(pid_, SIGTERM));
    int status = 0;
    static_cast<void>(::waitpid(pid_, &status, 0));
    pid_ = -1;
    throw std::runtime_error("Program Runner liveness readiness timed out" +
                             LogDetail());
  }

  ~ProgramRunnerProcess() {
    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGTERM));
      int status = 0;
      static_cast<void>(::waitpid(pid_, &status, 0));
    }
  }

  ProgramRunnerProcess(const ProgramRunnerProcess &) = delete;
  ProgramRunnerProcess &operator=(const ProgramRunnerProcess &) = delete;

  [[nodiscard]] const std::filesystem::path &socket_path() const noexcept {
    return socket_path_;
  }

private:
  [[nodiscard]] std::string LogDetail() const {
    std::ifstream input(log_path_, std::ios::binary);
    const std::string detail{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    return detail.empty() ? std::string{} : std::string(": ") + detail;
  }

  std::filesystem::path socket_path_;
  std::filesystem::path log_path_;
  pid_t pid_{-1};
};
#endif

std::vector<pid_t> ReadDirectChildren(const pid_t parent) {
  std::vector<pid_t> children;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      "/proc", std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto pid_spelling = iterator->path().filename().string();
    if (!pid_spelling.empty() &&
        std::all_of(pid_spelling.begin(), pid_spelling.end(),
                    [](const unsigned char character) {
                      return std::isdigit(character) != 0;
                    })) {
      std::ifstream status(iterator->path() / "status");
      std::string field;
      pid_t observed_parent = 0;
      char process_state = '\0';
      while (status >> field) {
        if (field == "State:") {
          static_cast<void>(status >> process_state);
        } else if (field == "PPid:") {
          static_cast<void>(status >> observed_parent);
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      }
      if (observed_parent == parent && process_state != 'Z') {
        children.push_back(static_cast<pid_t>(std::stol(pid_spelling)));
      }
    }
    iterator.increment(error);
  }
  std::sort(children.begin(), children.end());
  return children;
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
std::filesystem::path PrepareManagerApplyInstalledTargets(
    const DaemonDirectory &directory,
    const std::filesystem::path &host_source,
    const std::filesystem::path &worker_source,
    std::string_view installed_host_id) {
  std::filesystem::create_directory(directory.installed_root());
  const auto manager = directory.installed_root() / "manyforge_manager_daemon";
  const auto host = directory.installed_root() / installed_host_id;
  const auto candidate_worker =
      directory.installed_root() / "manyforge_roboplan_candidate_worker";
  std::filesystem::copy_file(MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON, manager,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(host_source, host,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(worker_source,
                             candidate_worker,
                             std::filesystem::copy_options::overwrite_existing);
  constexpr auto protected_executable_permissions =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
      std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
      std::filesystem::perms::others_exec;
  std::filesystem::permissions(manager, protected_executable_permissions,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      host, std::filesystem::status(host_source).permissions(),
      std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      candidate_worker,
      std::filesystem::status(worker_source)
          .permissions(),
      std::filesystem::perm_options::replace);
  return manager;
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
std::filesystem::path PrepareManagerApplyInstalledTargets(
    const DaemonDirectory &directory,
    const std::filesystem::path &ros_host_source =
        MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST) {
  return PrepareManagerApplyInstalledTargets(
      directory, ros_host_source,
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER,
      "manyforge_ros2_control_execution_host");
}

struct NativePackageIdentity final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedRuntimeUnitProduct product;
  core::RuntimeContractOfferDTO offer;
  std::string executable_digest;
};

NativePackageIdentity
ReadNativePackageIdentity(const std::filesystem::path &package_root,
                          const std::string_view retained_bundle_id,
                          const std::filesystem::path &installed_host) {
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
      package_root);
  const auto bundle = bundle_source.get_verified(
      core::RetainedBundleId(std::string(retained_bundle_id)));
  if (bundle == nullptr ||
      bundle->value().cell_graph.runtime_units.size() != 1U) {
    throw std::runtime_error("native package identity lacks one runtime");
  }
  const auto runtime_id =
      bundle->value().cell_graph.runtime_units.front().runtime_unit_id;
  const auto product = deployment::verify_runtime_unit_product(
      *bundle, runtime_id, artifact_source);
  if (!product.has_value() ||
      product->product_contract_id().value() !=
          deployment::ros2_control_execution_runtime_contract::kProductContractId) {
    throw std::runtime_error("package identity lacks the selected ROS product");
  }
  const auto *qualified =
      deployment::runtime_contract_declaration_registry().find(
          bundle->value().cell_graph.runtime_units.front().runtime_profile_id,
          bundle->value()
              .cell_graph.runtime_units.front()
              .runtime_profile_revision,
          product->product_contract_id(), product->product_contract_revision());
  if (qualified == nullptr) {
    throw std::runtime_error(
        "native package identity lacks its declared runtime offer");
  }
  std::ifstream stream(installed_host, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  if (!stream && !stream.eof()) {
    throw std::runtime_error("native package identity target is unreadable");
  }
  return {bundle, *product, qualified->offer(),
          content_identity::sha256_digest(bytes)};
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
struct ExecutionReadyObservation final {
  std::string response;
  std::string owner_epoch;
  bool observed_closed{};
};

ExecutionReadyObservation WaitForExecutionReady(
    const DaemonDirectory &directory,
    const std::string_view expected_retained_bundle_id,
    const std::chrono::seconds timeout = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  ExecutionReadyObservation observation;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      observation.response = SendCommand(
          directory.execution_socket_path(),
          R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
      if (observation.response.find("\"code\":\"execution_owner_not_ready\"") !=
          std::string::npos) {
        observation.observed_closed = true;
      }
      if (observation.response.find("\"ok\":true") != std::string::npos &&
          observation.response.find(expected_retained_bundle_id) !=
              std::string::npos) {
        constexpr std::string_view kOwnerEpochPrefix = "\"owner_epoch\":\"";
        const auto owner_begin = observation.response.find(kOwnerEpochPrefix);
        if (owner_begin == std::string::npos) {
          throw std::runtime_error(
              "ready execution response has no owner epoch");
        }
        const auto value_begin = owner_begin + kOwnerEpochPrefix.size();
        const auto value_end = observation.response.find('"', value_begin);
        if (value_end == std::string::npos || value_end == value_begin) {
          throw std::runtime_error(
              "ready execution response has an invalid owner epoch");
        }
        observation.owner_epoch =
            observation.response.substr(value_begin, value_end - value_begin);
        return observation;
      }
    } catch (const std::exception &error) {
      observation.observed_closed = true;
      observation.response = std::string("transport_error:") + error.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::ifstream manager_log(directory.startup_stderr(), std::ios::binary);
  std::string manager_detail{std::istreambuf_iterator<char>(manager_log),
                             std::istreambuf_iterator<char>()};
  constexpr std::size_t kMaximumFailureDetailBytes = 16U * 1024U;
  if (manager_detail.size() > kMaximumFailureDetailBytes) {
    manager_detail.erase(
        0U, manager_detail.size() - kMaximumFailureDetailBytes);
  }
  throw std::runtime_error("execution owner did not reopen exact product: " +
                           observation.response +
                           (manager_detail.empty()
                                ? std::string{}
                                : std::string("; manager stderr: ") +
                                      manager_detail));
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
RuntimeOfferProjection WaitForRuntimeOfferProjection(
    const AtomicSourceSnapshotChannel &manager_source,
    const std::string_view expected_execution_owner_epoch,
    const std::chrono::seconds timeout = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const auto snapshot = manager_source.Read();
      if (snapshot.has_value()) {
        const auto projection = ReadRuntimeOfferProjection(snapshot->payload);
        if (!projection.selected.empty() && !projection.observed.empty() &&
            projection.selected == projection.observed &&
            expected_execution_owner_epoch ==
                "kernel-owner-" + projection.observed_epoch) {
          return projection;
        }
      }
    } catch (const std::exception &) {
      // Publication may still be converging to the execution owner epoch.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error(
      "Manager source did not publish the expected runtime offer projection");
}

pid_t WaitForSoleManagedChild(const pid_t manager,
                              const std::string_view observation_stage) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto children = ReadDirectChildren(manager);
    if (children.size() == 1U && ::kill(children.front(), 0) == 0) {
      return children.front();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Manager did not converge one child " +
                           std::string(observation_stage));
}
#endif

std::string SendCommand(const std::filesystem::path &path,
                        const std::string &request) {
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (client < 0) {
    throw std::runtime_error("Manager command client socket failed");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command path is too long");
  }
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  if (::connect(client, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client connect failed");
  }
  const auto sent =
      ::send(client, request.data(), request.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command send failed");
  }
  char probe{};
  ssize_t response_size = -1;
  do {
    response_size = ::recv(client, &probe, 1U, MSG_PEEK | MSG_TRUNC);
  } while (response_size < 0 && errno == EINTR);
  if (response_size <= 0 ||
      static_cast<std::size_t>(response_size) >
          core::kMaximumCellAdmissionJsonResponseBytes) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command response is invalid");
  }
  std::string response(static_cast<std::size_t>(response_size), '\0');
  ssize_t received = -1;
  do {
    received = ::recv(client, response.data(), response.size(), 0);
  } while (received < 0 && errno == EINTR);
  static_cast<void>(::close(client));
  if (received != response_size) {
    throw std::runtime_error("Manager command response changed during read");
  }
  return response;
}

void SendCommandAndDisconnect(const std::filesystem::path &path,
                              const std::string &request) {
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (client < 0) {
    throw std::runtime_error("Manager command client socket failed");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command path is too long");
  }
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  if (::connect(client, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client connect failed");
  }
  const auto sent =
      ::send(client, request.data(), request.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command send failed");
  }
  if (::shutdown(client, SHUT_RDWR) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client shutdown failed");
  }
  static_cast<void>(::close(client));
}

std::string ReadStartupStderr(const DaemonDirectory &directory) {
  std::ifstream input(directory.startup_stderr(), std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct AuthoringAdmissionProjection final {
  bool authoring_allowed{};
  std::uint64_t admission_generation{};
};

AuthoringAdmissionProjection
ReadAuthoringAdmissionProjection(const DaemonDirectory &directory) {
  const auto response = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v1","command":"cell_admission_get"})");
  const auto root = deployment::parse_strict_json_model_yaml(response).root();
  const auto *ok = std::get_if<bool>(&RequiredMember(root, "ok").storage());
  const auto &admission = RequiredMember(root, "authoring_admission");
  const auto *allowed = std::get_if<bool>(
      &RequiredMember(admission, "authoring_allowed").storage());
  const auto generation =
      RequiredMember(admission, "admission_generation").as_number();
  if (ok == nullptr || !*ok || allowed == nullptr || generation < 1.0 ||
      generation > 9007199254740991.0 ||
      static_cast<double>(static_cast<std::uint64_t>(generation)) !=
          generation) {
    throw std::runtime_error(
        "stable Kernel returned an invalid authoring-admission projection");
  }
  return {*allowed, static_cast<std::uint64_t>(generation)};
}

[[maybe_unused]] AuthoringAdmissionProjection WaitForAuthoringAdmission(
    const DaemonDirectory &directory,
    const std::chrono::seconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<AuthoringAdmissionProjection> last;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      last = ReadAuthoringAdmissionProjection(directory);
      if (last->authoring_allowed) {
        return *last;
      }
    } catch (...) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Kernel authoring admission did not become ready" +
                           (last.has_value()
                                ? std::string("; last generation=") +
                                      std::to_string(last->admission_generation)
                                : std::string{}));
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
/// Assemble and publish one real prepared activation for the exact candidate
/// this test applies, and return its Apply summary. The fixture Program is
/// re-verified against that candidate; if its roles do not resolve there the
/// value is honestly non-executable (H1a).
content_identity::CanonicalValue PublishTestPreparedActivation(
    const DaemonDirectory &directory,
    const deployment::RetainedBundle &candidate,
    const ManagerApplyFixtureMetadata &fixture) {
  const auto package_root = directory.apply_package_root();
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(package_root);
  const auto physical = bundles.get_verified(
      core::RetainedBundleId(fixture.physical_candidate_retained_bundle_id));
  if (physical == nullptr) {
    throw std::runtime_error("Manager Apply fixture lost its physical bundle");
  }
  const auto authored = deployment::FilesystemVerifiedProgramSource(package_root)
                            .get_verified(core::ProgramId(fixture.program_id),
                                          core::ProgramRevision(fixture.program_revision),
                                          *physical);
  if (authored == nullptr) {
    throw std::runtime_error("Manager Apply fixture Program is unavailable");
  }
  const auto program = deployment::verify_program_binding_contract(
      authored->canonical_program_bytes(), candidate);
  const auto &map_products =
      candidate.value().compiled_products.symbol_role_maps;
  if (map_products.size() > 1U) {
    throw std::runtime_error(
        "Manager Apply fixture candidate has multiple model maps");
  }
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  if (!map_products.empty()) {
    model_map =
        deployment::FilesystemVerifiedRetainedModelMapSource(package_root)
            .get_verified({candidate.retained_bundle_id(),
                           map_products.front().product_id});
  } else if (!candidate.value().cell_graph.controllable_components.empty() ||
             !candidate.value().cell_graph.model_instances.empty()) {
    throw std::runtime_error(
        "Manager Apply fixture candidate omits required model data");
  }
  if (program == nullptr) {
    throw std::runtime_error("Manager Apply fixture Program is unavailable");
  }
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  deployment::FilesystemRawArtifactStore raw_artifacts(package_root);
  const auto prepared = deployment::assemble_prepared_activation(
      {deployment::PreparedScenarioOrigin{
           "scenario.test",
           core::ContentDigest("sha256:1414141414141414141414141414141414141414141414"
                               "141414141414141414")},
       core::ContentDigest("sha256:2020202020202020202020202020202020202020202020"
                           "202020202020202020"),
       "optional"},
      candidate, model_map, *program, fixture.program_role_assignment,
      catalogs, raw_artifacts);
  static_cast<void>(
      deployment::FilesystemPreparedActivationRepository(directory.activation_root())
          .put_if_absent(prepared));
  content_identity::CanonicalValue::Object summary;
  for (const auto &[name, value] : prepared.normalized_root().as_object()) {
    if (name != "activation_payload") summary.emplace_back(name, value);
  }
  return content_identity::CanonicalValue(std::move(summary));
}
#endif

std::string ApplyRequest(
    const DaemonDirectory &directory,
    const std::string_view manager_owner_epoch,
    const std::uint64_t expected_admission_generation,
    const std::string_view deployment_artifact_id = "candidate",
    const std::string_view deployment_revision = kDeploymentRevision,
    const std::string_view expected_graph_revision = "current-graph",
    const std::string_view expected_retained_bundle_id = "current-bundle",
    const std::string_view request_id = "request-1",
    const std::string_view preview_owner_epoch = {},
    const bool preview_only = false,
    // "receipt" echoes the previewed impact digest (explicit acceptance), an
    // empty value accepts nothing, anything else is sent verbatim.
    const std::string_view accepted_impact = "receipt",
    const std::string_view prepared_activation_id_override = {},
    const deployment::CanonicalEmptyRetainedBundleAdmission *sealed_admission =
        nullptr) {
  using content_identity::CanonicalValue;
  std::string resolved_graph_revision(expected_graph_revision);
  std::string resolved_retained_bundle_id(expected_retained_bundle_id);
  if (expected_graph_revision == "current-graph" ||
      expected_retained_bundle_id == "current-bundle") {
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    const auto current = manager_source.Read();
    if (!current.has_value()) {
      throw std::runtime_error(
          "Manager current projection is unavailable in test setup");
    }
    const auto &active = RequiredMember(current->payload, "active");
    if (expected_graph_revision == "current-graph") {
      resolved_graph_revision = RequiredText(active, "graph_revision");
    }
    if (expected_retained_bundle_id == "current-bundle") {
      resolved_retained_bundle_id = RequiredText(active, "retained_bundle_id");
    }
  }
  const auto owner =
      preview_owner_epoch.empty() ? manager_owner_epoch : preview_owner_epoch;
  const auto expected = CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(resolved_graph_revision)},
      {"retained_bundle_id", CanonicalValue(resolved_retained_bundle_id)},
  });
  // Negative preview tests intentionally have no candidate package (or have
  // displaced it). Use well-formed absent identities so the Manager, not
  // fixture lookup, reports the expected rejection.
  std::string candidate_bundle = "sha256:" + std::string(64U, '1');
  std::string candidate_graph = "sha256:" + std::string(64U, '2');
  // Negative previews never reach impact derivation; they keep a
  // well-formed absent identity.
  std::string prepared_activation_id =
      "sha256:1616161616161616161616161616161616161616161616161616161616161616";
  CanonicalValue prepared_summary(nullptr);
  if (!preview_only) {
    candidate_bundle.clear();
    candidate_graph.clear();
    const deployment::FilesystemVerifiedRetainedBundleSource bundles(
        directory.apply_package_root());
    const auto retained_root =
        directory.apply_package_root() / "retained-bundles" / "sha256";
    for (const auto &entry : std::filesystem::directory_iterator(retained_root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const auto bundle = bundles.get_verified(core::RetainedBundleId(
          "sha256:" + entry.path().stem().string()));
      if (bundle != nullptr &&
          bundle->value().deployment_artifact_id.value() ==
              deployment_artifact_id &&
          bundle->value().deployment_revision.value() == deployment_revision) {
        candidate_bundle = bundle->retained_bundle_id().value();
        candidate_graph = bundle->value().cell_graph.graph_revision.value();
        if (prepared_activation_id_override.empty()) {
#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
          prepared_summary = PublishTestPreparedActivation(
              directory, *bundle, ReadManagerApplyFixtureMetadata());
#else
          // The Manager Apply fixture (and its prepared activation) exists
          // only when the selected ROS execution host fixture is configured.
          throw std::logic_error(
              "publishing a prepared activation needs the selected ROS execution "
              "host test fixture");
#endif
          prepared_activation_id =
              RequiredText(prepared_summary, "prepared_activation_id");
        } else {
          prepared_activation_id = prepared_activation_id_override;
          const auto prepared =
              deployment::FilesystemPreparedActivationRepository(
                  directory.activation_root())
                  .get_verified(core::ContentDigest(prepared_activation_id),
                                sealed_admission);
          if (prepared == nullptr) {
            throw std::runtime_error(
                "Manager Apply fixture prepared activation is unavailable");
          }
          CanonicalValue::Object summary;
          for (const auto &[name, value] :
               prepared->normalized_root().as_object()) {
            if (name != "activation_payload") {
              summary.emplace_back(name, value);
            }
          }
          prepared_summary = CanonicalValue(std::move(summary));
        }
        break;
      }
    }
    if (candidate_bundle.empty() || candidate_graph.empty()) {
      throw std::runtime_error("Manager retained fixture has no matching candidate");
    }
  }
  const auto preview_request =
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue("manyforge.manager_command.v1")},
          {"command", CanonicalValue("deployment_candidate_preview")},
          {"candidate_graph_revision", CanonicalValue(candidate_graph)},
          {"candidate_retained_bundle_id", CanonicalValue(candidate_bundle)},
          {"deployment_artifact_id",
           CanonicalValue(std::string(deployment_artifact_id))},
          {"deployment_revision",
           CanonicalValue(std::string(deployment_revision))},
          {"expected_admission_generation",
           CanonicalValue(static_cast<double>(expected_admission_generation))},
          {"expected_current", expected},
          {"prepared_activation_id", CanonicalValue(prepared_activation_id)},
          {"request_owner_epoch", CanonicalValue(std::string(owner))},
      }));
  const auto preview_response =
      SendCommand(directory.socket_path(), preview_request);
  if (preview_only) {
    return preview_response;
  }
  const auto preview =
      deployment::parse_strict_json_model_yaml(preview_response).root();
  const auto *ok = std::get_if<bool>(&RequiredMember(preview, "ok").storage());
  if (ok == nullptr || !*ok) {
    throw std::runtime_error("Manager candidate preview failed in test setup: " +
                             preview_response);
  }
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"admission_context",
       CanonicalValue(CanonicalValue::Object{
           {"expected_admission_generation",
            CanonicalValue(static_cast<double>(expected_admission_generation))},
           {"kind", CanonicalValue("direct")},
       })},
      {"accepted_impact_digest",
       accepted_impact == "receipt"
           ? RequiredMember(RequiredMember(preview, "candidate_receipt"),
                            "impact_digest")
       : accepted_impact.empty()
           ? CanonicalValue(nullptr)
           : CanonicalValue(std::string(accepted_impact))},
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"candidate_receipt", RequiredMember(preview, "candidate_receipt")},
      {"command", CanonicalValue("apply")},
      {"correlation_id", CanonicalValue("correlation-1")},
      {"deployment_artifact_id",
       CanonicalValue(std::string(deployment_artifact_id))},
      {"deployment_revision", CanonicalValue(std::string(deployment_revision))},
      {"expected_current", expected},
      {"prepared_activation", prepared_summary},
      {"reject_if_affected_busy", CanonicalValue(true)},
      {"scene_initialization", CanonicalValue("preserve")},
      {"request_id", CanonicalValue(std::string(request_id))},
      {"request_owner_epoch", CanonicalValue(std::string(manager_owner_epoch))},
      {"source_label", CanonicalValue("manager-process-test")},
  }));
}

[[maybe_unused]] std::string
ApplyStatusRequest(const std::string_view submission_response) {
  using content_identity::CanonicalValue;
  const auto response =
      deployment::parse_strict_json_model_yaml(submission_response).root();
  const auto *apply_id =
      std::get_if<std::string>(&RequiredMember(response, "apply_id").storage());
  const auto generation =
      RequiredMember(response, "apply_generation").as_number();
  if (apply_id == nullptr || apply_id->empty() || generation < 1.0 ||
      generation > 9007199254740991.0 ||
      static_cast<double>(static_cast<std::uint64_t>(generation)) !=
          generation) {
    throw std::runtime_error("Manager Apply submission receipt is invalid");
  }
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string("manyforge.manager_command.v1"))},
      {"apply_generation", CanonicalValue(generation)},
      {"apply_id", CanonicalValue(*apply_id)},
      {"command", CanonicalValue(std::string("apply_status"))},
  }));
}

std::string SceneAddRequest(
    const SourceSnapshot &scene_source, const std::string_view request_id,
    const std::uint64_t expected_admission_generation, const double box_x = 0.1,
    const std::string_view scene_resource_id = "scene-process-box") {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  const auto &cell_id = RequiredMember(
      RequiredMember(scene_source.payload, "snapshot"), "cell_id");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_commit")},
      {"transaction",
       CanonicalValue(CanonicalValue::Object{
           {"attribution", CanonicalValue(CanonicalValue::Object{
                               {"assurance", CanonicalValue("unverified")},
                               {"principal_id", CanonicalValue(nullptr)},
                               {"transport", CanonicalValue("http")},
                           })},
           {"expected_admission_generation",
            CanonicalValue(static_cast<double>(expected_admission_generation))},
           {"expected_projection_revision", projection},
           {"operation",
            CanonicalValue(CanonicalValue::Object{
                {"frame_ref", CanonicalValue(CanonicalValue::Object{
                                  {"cell_id", cell_id},
                                  {"kind", CanonicalValue("cell_frame")},
                              })},
                {"kind", CanonicalValue("add_object")},
                {"pose",
                 CanonicalValue(CanonicalValue::Object{
                     {"orientation", CanonicalValue(CanonicalValue::Object{
                                         {"w", CanonicalValue(1.0)},
                                         {"x", CanonicalValue(0.0)},
                                         {"y", CanonicalValue(0.0)},
                                         {"z", CanonicalValue(0.0)},
                                     })},
                     {"position_m", CanonicalValue(CanonicalValue::Object{
                                        {"x_m", CanonicalValue(0.0)},
                                        {"y_m", CanonicalValue(0.0)},
                                        {"z_m", CanonicalValue(0.15)},
                                    })},
                 })},
                {"scene_resource_id",
                 CanonicalValue(std::string(scene_resource_id))},
                {"shape",
                 CanonicalValue(CanonicalValue::Object{
                     {"box_dimensions_m", CanonicalValue(CanonicalValue::Object{
                                              {"x_m", CanonicalValue(box_x)},
                                              {"y_m", CanonicalValue(0.2)},
                                              {"z_m", CanonicalValue(0.3)},
                                          })},
                     {"type", CanonicalValue("box")},
                 })},
                {"support_contacts", CanonicalValue(CanonicalValue::Array{})},
            })},
           {"program_reset_scene_authorization", CanonicalValue(nullptr)},
           {"request_context",
            CanonicalValue(CanonicalValue::Object{
                {"correlation_id", CanonicalValue(nullptr)},
                {"request_id", CanonicalValue(std::string(request_id))},
                {"request_owner_epoch",
                 CanonicalValue(scene_source.source_owner_epoch)},
                {"source_label", CanonicalValue("manager-process-test")},
            })},
       })},
  }));
}

std::string ScenePoseRequest(const SourceSnapshot &scene_source,
                             const std::string_view scene_resource_id) {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_pose_query")},
      {"expected_projection_revision", projection},
      {"scene_resource_ids", CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue(std::string(scene_resource_id)),
                             })},
  }));
}

std::string
ScenePreviewRequest(const SourceSnapshot &scene_source,
                    const std::string_view request_id,
                    const std::uint64_t expected_admission_generation) {
  using content_identity::CanonicalValue;
  const auto transaction = deployment::parse_strict_json_model_yaml(
                               SceneAddRequest(scene_source, request_id,
                                               expected_admission_generation))
                               .root();
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"candidate_request",
       CanonicalValue(CanonicalValue::Object{
           {"frame_resolution", CanonicalValue(nullptr)},
           {"request", RequiredMember(transaction, "transaction")},
       })},
      {"command", CanonicalValue("scene_candidate_preview")},
  }));
}

const content_identity::CanonicalValue &
SceneRuntimeIdentityValue(const SourceSnapshot &scene_runtime_source) {
  return RequiredMember(RequiredMember(scene_runtime_source.payload, "state"),
                        "runtime_identity");
}

content_identity::CanonicalValue
DifferentSceneRuntimeIdentityValue(const SourceSnapshot &scene_runtime_source) {
  using content_identity::CanonicalValue;
  const auto &identity = SceneRuntimeIdentityValue(scene_runtime_source);
  return CanonicalValue(CanonicalValue::Object{
      {"deployment_generation",
       RequiredMember(identity, "deployment_generation")},
      {"graph_revision", RequiredMember(identity, "graph_revision")},
      {"program_generation", RequiredMember(identity, "program_generation")},
      {"retained_bundle_id", RequiredMember(identity, "retained_bundle_id")},
      {"runtime_generation",
       CanonicalValue(
           RequiredMember(identity, "runtime_generation").as_number() + 1.0)},
  });
}

std::string SceneRuntimeObserverOpenRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch,
    const std::string_view source_id = "isaac.observer",
    const std::string_view object_id = "robot",
    const std::string_view policy_id = "viewer.measured_robot",
    const bool eligible = false) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"eligible", CanonicalValue(eligible)},
               {"object_id", CanonicalValue(std::string(object_id))},
               {"policy_id", CanonicalValue(std::string(policy_id))},
           }),
       })},
      {"command", CanonicalValue("scene_runtime_observer_open")},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
  }));
}

std::string SceneRuntimeObserveRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch, const std::uint64_t source_sequence,
    const std::uint64_t lifetime_ns = 1000000000ULL,
    const double position = 0.25,
    const content_identity::CanonicalValue *expected_identity = nullptr,
    const std::string_view source_id = "isaac.observer",
    const std::string_view object_id = "robot",
    const std::string_view policy_id = "viewer.measured_robot",
    const bool eligible = false) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_observe")},
      {"eligible", CanonicalValue(eligible)},
      {"expected_runtime_identity",
       expected_identity == nullptr
           ? SceneRuntimeIdentityValue(scene_runtime_source)
           : *expected_identity},
      {"lifetime_ns", CanonicalValue(static_cast<double>(lifetime_ns))},
      {"object_id", CanonicalValue(std::string(object_id))},
      {"payload",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
           {"kind", CanonicalValue("joint_state")},
           {"positions", CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(position),
                         })},
       })},
      {"policy_id", CanonicalValue(std::string(policy_id))},
      {"quality", CanonicalValue("good")},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
      {"source_sequence", CanonicalValue(static_cast<double>(source_sequence))},
  }));
}

std::string SceneRuntimePoseObserveRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch, const std::uint64_t source_sequence,
    const std::uint64_t lifetime_ns = 1000000000ULL,
    const double position = 0.25,
    const std::string_view source_id = "isaac.planning-writer",
    const std::string_view object_id = "scene-process-box") {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_observe")},
      {"eligible", CanonicalValue(true)},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
      {"lifetime_ns", CanonicalValue(static_cast<double>(lifetime_ns))},
      {"object_id", CanonicalValue(std::string(object_id))},
      {"payload",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
           {"kind", CanonicalValue("simulation_entity_pose")},
           {"pose", CanonicalValue(CanonicalValue::Object{
                        {"frame_id", CanonicalValue("universe")},
                        {"frame_kind", CanonicalValue("cell_frame")},
                        {"orientation_xyzw",
                         CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(0.0), CanonicalValue(0.0),
                             CanonicalValue(0.0), CanonicalValue(1.0)})},
                        {"position_m",
                         CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(position), CanonicalValue(-0.5),
                             CanonicalValue(0.75)})},
                        {"source_stamp_ns", CanonicalValue(1250000000.0)},
                    })},
           {"source", CanonicalValue(CanonicalValue::Object{
                          {"entity", CanonicalValue("/box")}})},
       })},
      {"policy_id", CanonicalValue("before_planning")},
      {"quality", CanonicalValue("good")},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
      {"source_sequence", CanonicalValue(static_cast<double>(source_sequence))},
  }));
}

std::string SceneRuntimeBeforePlanningLatchRequest(
    const SourceSnapshot &scene_runtime_source,
    const SourceSnapshot &scene_source) {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_latch_before_planning")},
      {"expected_projection_revision", projection},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
  }));
}

TEST(ManagerDaemonProcess,
     RejectsPreManifestRootWithTypedStartupErrorBeforeIngress) {
  DaemonDirectory legacy;
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();
  const auto legacy_journal =
      legacy.manager_root() /
      std::string(component_manager::kManagerBootstrapJournalFilename);
  {
    component_manager::SqliteManagerBootstrapJournal journal(
        legacy_journal, admission.cell_id());
  }

  ManagerProcess manager(legacy);
  const auto status = manager.Wait();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 70);
  const auto startup_error = ReadStartupStderr(legacy);
  EXPECT_NE(
      startup_error.find("\"code\":\"runtime_compatibility_manifest_missing\""),
      std::string::npos);
  EXPECT_NE(
      startup_error.find("\"required_action\":\"clean_canonical_bootstrap\""),
      std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(legacy.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(legacy.execution_socket_path()));
  EXPECT_FALSE(std::filesystem::exists(legacy.publication_root() /
                                       "manager-source.json"));
  EXPECT_TRUE(std::filesystem::exists(legacy_journal));
  EXPECT_FALSE(std::filesystem::exists(
      legacy.manager_root() /
      std::string(component_manager::kManagerStateMarkerFilename)));

  DaemonDirectory clean;
  ManagerProcess supported(clean);
  supported.WaitReady(clean);
  EXPECT_TRUE(std::filesystem::exists(
      clean.manager_root() /
      std::string(component_manager::kManagerStateMarkerFilename)));
}

TEST(ManagerDaemonProcess,
     RejectsDisposableRunRootNestedInsideDurableProjectionRoot) {
  DaemonDirectory directory;
  ManagerProcess manager(
      directory, MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
      "canonical-empty-admission.json", MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON,
      {}, directory.publication_root().parent_path().string());
  const auto status = manager.Wait();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), 0);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "must be disjoint from disposable run storage"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(directory.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(directory.execution_socket_path()));
}

TEST(ManagerDaemonProcess, ProcessStopDecoderPreservesActualMultilineLaunchIdentity) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(), "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before);
  const std::string launches[]{
      "mock\nlite6_robot\nlite6_lite_gripper\n/manyforge/lite6_robot\nlite6_traj_controller\nlite_gripper_controller\n\nsha256:948e121935a4e7d66ca927d1945438e48332cce2bd820f60488fc049bb18d4b1\nsha256:61f01507a85d3a8d6539b8488d9a525dbaf115dca33338dc0d1702168d3b34a4\n",
      "mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n"};
  EXPECT_EQ(launches[0].size(), 250U);
  EXPECT_EQ(launches[1].size(), 257U);
  const auto base = deployment::parse_strict_json_model_yaml(
      R"({"request_context":{"request_id":"close.stop.1","request_owner_epoch":"caller.1","source_label":"composer"},"predecessor_projection_revision":{"retained_bundle_id":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","graph_revision":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","scene_revision":{"scene_id":"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","revision_id":1,"content_hash":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}},"expected_predecessor_publication_generation":1,"expected_predecessor_public_pointer_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","runtime_unit_id":"host.1","runtime_epoch":"host.epoch.1","manager_runtime_epoch":"foreign.manager.epoch","controllable_component_id":"placeholder","launch_identity":"placeholder","leader_pid":42})").root();
  const auto exchange = [&](const std::string &command, const std::string &launch,
                            const std::string &component, const std::string &runtime_unit) {
    auto fields = base.as_object();
    for (auto &[key, value] : fields) {
      if (key == "launch_identity") value = CanonicalValue(launch);
      if (key == "controllable_component_id") value = CanonicalValue(component);
      if (key == "runtime_unit_id") value = CanonicalValue(runtime_unit);
    }
    const auto packet = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.manager_command.v1")},
        {"command", CanonicalValue(command)}, {"request", CanonicalValue(std::move(fields))}}));
    return deployment::parse_strict_json_model_yaml(SendCommand(directory.socket_path(), packet)).root();
  };
  for (std::size_t i = 0U; i < 2U; ++i) {
    const std::string component = i == 0U ? "lite6_robot" : "ur5e_robot";
    for (const auto *command : {"stop_component_process", "component_process_stop_result"}) {
      const auto response = exchange(command, launches[i], component, "host.1");
      EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(false));
      const auto &error = RequiredMember(response, "error");
      EXPECT_EQ(RequiredText(error, "code"), "manager_command_rejected");
      // Reaching the original Manager occurrence guard proves complete decoding;
      // the foreign Manager epoch prevents forwarding or any Stop effect.
      EXPECT_EQ(RequiredText(error, "message"),
                "Manager daemon: component process Stop names another Manager occurrence");
    }
  }
  for (const auto &launch : {std::string(), launches[1]}) {
    const auto response = exchange("stop_component_process", launch, "ur5e_robot",
                                   launch.empty() ? "host.1" : std::string(257U, 'x'));
    EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(false));
    const auto message = RequiredText(RequiredMember(response, "error"), "message");
    EXPECT_NE(message.find(launch.empty() ? "command field launch_identity" : "command field runtime_unit_id"),
              std::string::npos);
  }
  const auto component_rejected = exchange("stop_component_process", launches[1], std::string(129U, 'x'), "host.1");
  EXPECT_NE(RequiredText(RequiredMember(component_rejected, "error"), "message").find(
      "component process Stop request is invalid"), std::string::npos);
  const auto after = manager_source.Read();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->source_owner_epoch, before->source_owner_epoch);
  EXPECT_EQ(RequiredMember(after->payload, "active"), RequiredMember(before->payload, "active"));
}

TEST(ManagerDaemonProcess, ProcessStopOfferReturnsKnownEmptyInventoryAndIsPassive) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(), "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before);
  const auto bytes = SendCommand(directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":"component.1"})");
  const auto response = deployment::parse_strict_json_model_yaml(bytes).root();
  ASSERT_EQ(response.as_object().size(), 4U);
  EXPECT_EQ(RequiredMember(response, "api_version"), CanonicalValue("manyforge.manager_command_response.v1"));
  EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(true)) << bytes;
  EXPECT_EQ(RequiredMember(response, "error"), CanonicalValue(nullptr));
  const auto &offer = RequiredMember(response, "result");
  ASSERT_EQ(offer.as_object().size(), 2U);
  EXPECT_EQ(RequiredMember(offer, "controllable_component_id"), CanonicalValue("component.1"));
  EXPECT_TRUE(RequiredMember(offer, "process_inventories").as_array().empty());
  for (const auto* request : {
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer"})",
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":false})",
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":"component.1","extra":true})"}) {
    const auto rejected_bytes = SendCommand(directory.socket_path(), request);
    const auto rejected = deployment::parse_strict_json_model_yaml(rejected_bytes).root();
    EXPECT_EQ(RequiredMember(rejected, "ok"), CanonicalValue(false)) << rejected_bytes;
    EXPECT_EQ(RequiredMember(RequiredMember(rejected, "error"), "code"),
        CanonicalValue("manager_command_rejected")) << rejected_bytes;
  }
  const auto after = manager_source.Read();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->source_owner_epoch, before->source_owner_epoch);
  EXPECT_EQ(RequiredMember(after->payload, "active"), RequiredMember(before->payload, "active"));
}

TEST(ManagerDaemonProcess, CanonicalEmptySelectionIsClosedAndPassive) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before.has_value());
  EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(before->payload));
  const auto bytes = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"canonical_empty_selection"})");
  const auto response = deployment::parse_strict_json_model_yaml(bytes).root();
  ASSERT_EQ(response.as_object().size(), 3U);
  EXPECT_EQ(RequiredMember(response, "api_version"),
            CanonicalValue("manyforge.manager_command_response.v1"));
  ASSERT_EQ(RequiredMember(response, "ok"), CanonicalValue(true)) << bytes;
  const auto &selection = RequiredMember(response, "canonical_empty_selection");
  ASSERT_EQ(selection.as_object().size(), 7U);
  EXPECT_EQ(RequiredMember(selection, "request_owner_epoch"),
            CanonicalValue(before->source_owner_epoch));
  EXPECT_EQ(RequiredMember(selection, "cell_id"),
            CanonicalValue(admission.cell_id().value()));
  EXPECT_EQ(RequiredMember(selection, "deployment_artifact_id"),
            CanonicalValue(admission.deployment_artifact_id().value()));
  EXPECT_EQ(RequiredMember(selection, "deployment_revision"),
            CanonicalValue(admission.deployment_revision().value()));
  EXPECT_EQ(RequiredMember(selection, "candidate_retained_bundle_id"),
            CanonicalValue(admission.retained_bundle_id().value()));
  EXPECT_EQ(RequiredMember(selection, "candidate_graph_revision"),
            CanonicalValue(admission.graph_revision().value()));
  EXPECT_EQ(RequiredMember(selection, "admission_digest"),
            CanonicalValue(admission.admission_digest().value()));
  EXPECT_EQ(
      SendCommand(
          directory.socket_path(),
          R"({"api_version":"manyforge.manager_command.v1","command":"canonical_empty_selection"})"),
      bytes);
  for (
      const std::string_view request :
      {std::string_view("{"),
       std::string_view(
           R"({"api_version":"manyforge.manager_command.v1","command":false})"),
       std::string_view(
           R"({"api_version":false,"command":"canonical_empty_selection"})"),
       std::string_view(
           R"({"api_version":"manyforge.manager_command.v1","command":"canonical_empty_selection","extra":true})")}) {
    const auto rejected_bytes =
        SendCommand(directory.socket_path(), std::string(request));
    const auto rejected =
        deployment::parse_strict_json_model_yaml(rejected_bytes).root();
    EXPECT_EQ(RequiredMember(rejected, "ok"), CanonicalValue(false))
        << rejected_bytes;
    EXPECT_EQ(RequiredMember(RequiredMember(rejected, "error"), "code"),
              CanonicalValue("manager_command_rejected"))
        << rejected_bytes;
  }
  const auto after = manager_source.Read();
  ASSERT_TRUE(after.has_value());
  EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(after->payload));
  EXPECT_EQ(after->source_owner_epoch, before->source_owner_epoch);
  EXPECT_EQ(RequiredMember(after->payload, "active"),
            RequiredMember(before->payload, "active"));
  EXPECT_EQ(RequiredMember(after->payload, "apply"),
            RequiredMember(before->payload, "apply"));
}

TEST(ManagerDaemonProcess,
     PublishesWithoutComposerAndSurvivesManagerReplacement) {
  DaemonDirectory directory;
  std::string first_manager_epoch;
  std::uint64_t first_manager_generation = 0U;
  std::string first_scene_epoch;
  std::string first_scene_digest;
  std::uint64_t first_scene_generation = 0U;
  std::string first_scene_runtime_epoch;
  std::uint64_t first_scene_runtime_generation = 0U;
  std::string unavailable_response;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto manager_snapshot = manager_source.Read();
    const auto scene_snapshot = scene_source.Read();
    const auto scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(manager_snapshot.has_value());
    ASSERT_TRUE(scene_snapshot.has_value());
    ASSERT_TRUE(scene_runtime_snapshot.has_value());
    EXPECT_EQ(manager_snapshot->source_owner_kind, "component_manager");
    EXPECT_EQ(scene_snapshot->source_owner_kind, "logical_scene");
    EXPECT_EQ(scene_runtime_snapshot->source_owner_kind, "scene_runtime");
    const auto manager_payload_bytes =
        content_identity::canonical_json(manager_snapshot->payload);
    EXPECT_EQ(manager_payload_bytes.find("node_environment"),
              std::string::npos);
    first_manager_epoch = manager_snapshot->source_owner_epoch;
    first_manager_generation = manager_snapshot->source_generation;
    first_scene_epoch = scene_snapshot->source_owner_epoch;
    first_scene_digest = scene_snapshot->semantic_content_digest;
    first_scene_generation = scene_snapshot->source_generation;
    first_scene_runtime_epoch = scene_runtime_snapshot->source_owner_epoch;
    first_scene_runtime_generation = scene_runtime_snapshot->source_generation;

    unavailable_response = ApplyRequest(
        directory, first_manager_epoch, 1U, "candidate", kDeploymentRevision,
        "current-graph", "current-bundle", "request-1", {}, true);
    EXPECT_EQ(ApplyRequest(directory, first_manager_epoch, 1U, "candidate",
                           kDeploymentRevision, "current-graph",
                           "current-bundle", "request-1", {}, true),
              unavailable_response);
    EXPECT_NE(unavailable_response.find("\"deployment_candidate_rejected\""),
              std::string::npos);
    EXPECT_EQ(unavailable_response.find("\"state\":\"accepted\""),
              std::string::npos);

    SendCommandAndDisconnect(
        directory.socket_path(),
        "{\"api_version\":\"manyforge.manager_command.v1\","
        "\"command\":\"reconcile\"}");
    const auto post_disconnect_response =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"reconcile\"}");
    EXPECT_NE(post_disconnect_response.find("\"ok\":true"), std::string::npos);
    EXPECT_EQ(::kill(manager.pid(), 0), 0);

    component_manager::SqliteManagerApplyJournal apply_journal(
        directory.manager_root() /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId("c1_build_authority_cell"));
    EXPECT_TRUE(apply_journal.ReadStatusInventory().empty());

    std::atomic<bool> read_failed{false};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
      readers.emplace_back([&] {
        try {
          AtomicSourceSnapshotChannel reader(directory.publication_root(),
                                             "manager-source.json");
          for (int read = 0; read < 100; ++read) {
            const auto snapshot = reader.Read();
            if (!snapshot.has_value() || !snapshot->valid()) {
              read_failed.store(true);
              return;
            }
          }
        } catch (...) {
          read_failed.store(true);
        }
      });
    }
    for (int reconcile = 0; reconcile < 20; ++reconcile) {
      const auto response =
          SendCommand(directory.socket_path(),
                      "{\"api_version\":\"manyforge.manager_command.v1\","
                      "\"command\":\"reconcile\"}");
      EXPECT_NE(response.find("\"ok\":true"), std::string::npos);
    }
    for (auto &reader : readers) {
      reader.join();
    }
    EXPECT_FALSE(read_failed.load());
    const auto reconciled_manager_snapshot = manager_source.Read();
    const auto reconciled_scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(reconciled_manager_snapshot.has_value());
    ASSERT_TRUE(reconciled_scene_runtime_snapshot.has_value());
    EXPECT_EQ(reconciled_manager_snapshot->source_generation,
              first_manager_generation);
    EXPECT_EQ(reconciled_scene_runtime_snapshot->source_generation,
              first_scene_runtime_generation);

    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }

  {
    ManagerProcess replacement(directory);
    replacement.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto manager_snapshot = manager_source.Read();
    const auto scene_snapshot = scene_source.Read();
    const auto scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(manager_snapshot.has_value());
    ASSERT_TRUE(scene_snapshot.has_value());
    ASSERT_TRUE(scene_runtime_snapshot.has_value());
    EXPECT_NE(manager_snapshot->source_owner_epoch, first_manager_epoch);
    EXPECT_EQ(manager_snapshot->source_generation, 1U);
    EXPECT_EQ(scene_snapshot->source_owner_epoch, first_scene_epoch);
    EXPECT_EQ(scene_snapshot->semantic_content_digest, first_scene_digest);
    EXPECT_EQ(scene_snapshot->source_generation, first_scene_generation);
    EXPECT_NE(scene_runtime_snapshot->source_owner_epoch,
              first_scene_runtime_epoch);
    EXPECT_EQ(scene_runtime_snapshot->source_generation, 1U);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_snapshot->payload)
                  .find("\"state\":\"open\""),
              std::string::npos);
    const auto stale_apply = ApplyRequest(
        directory, first_manager_epoch, 1U, "candidate", kDeploymentRevision,
        "current-graph", "current-bundle", "request-1", {}, true);
    EXPECT_NE(stale_apply.find("\"stale_request_owner_epoch\""),
              std::string::npos);
    const auto stale_generation = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*scene_snapshot, "replacement-stale-generation.1", 2U));
    EXPECT_NE(stale_generation.find("\"authoring_not_ready\""),
              std::string::npos);
    const auto replacement_unavailable =
        ApplyRequest(directory, manager_snapshot->source_owner_epoch, 1U,
                     "candidate", kDeploymentRevision, "current-graph",
                     "current-bundle", "request-1", {}, true);
    EXPECT_NE(replacement_unavailable.find("\"deployment_candidate_rejected\""),
              std::string::npos);

    const auto shutdown =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"shutdown_runtime\"}");
    EXPECT_NE(shutdown.find("\"ok\":true"), std::string::npos);
    const auto status = replacement.Wait();
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
}

TEST(ManagerDaemonProcess,
     PeriodicOwnerPublicationIsNotStarvedByCommandTraffic) {
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto before = scene_source.ReadObserved();
  ASSERT_TRUE(before.has_value());

  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> senders;
  for (int index = 0; index < 4; ++index) {
    senders.emplace_back([&] {
      try {
        while (!stop.load()) {
          const auto response =
              SendCommand(directory.socket_path(),
                          "{\"api_version\":\"manyforge.manager_command.v1\","
                          "\"command\":\"scene_owner_read\"}");
          if (response.find("\"ok\":true") == std::string::npos) {
            failed.store(true);
            return;
          }
        }
      } catch (...) {
        failed.store(true);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true);
  for (auto &sender : senders) {
    sender.join();
  }

  const auto after = scene_source.ReadObserved();
  ASSERT_TRUE(after.has_value());
  EXPECT_FALSE(failed.load());
  EXPECT_GT(after->file_modified_at_unix_ns, before->file_modified_at_unix_ns);
}

TEST(ManagerDaemonProcess,
     SceneRuntimeObservationIngressFencesIdentityReplacementAndFreshness) {
  DaemonDirectory directory;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    const auto initial = scene_runtime_source.Read();
    const auto initial_scene = scene_source.Read();
    ASSERT_TRUE(initial.has_value());
    ASSERT_TRUE(initial_scene.has_value());

    const auto owner_read = SendCommand(
        directory.socket_path(),
        R"({"api_version":"manyforge.manager_command.v1","command":"scene_runtime_owner_read"})");
    EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(owner_read.find("\"scene_runtime_owner_epoch\":"),
              std::string::npos);
    EXPECT_NE(owner_read.find("\"initialization_state\":\"open\""),
              std::string::npos);
    EXPECT_NE(owner_read.find("\"runtime_identity\":"), std::string::npos);
    EXPECT_EQ(owner_read.find("\"pose_sources\":"), std::string::npos);

    const auto opened_first = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserverOpenRequest(*initial, "observer-epoch-1"));
    EXPECT_NE(opened_first.find("\"source_epoch_order\":1"), std::string::npos)
        << opened_first;
    const auto changed_registration =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "observer-epoch-1", "isaac.observer",
                        "scene_object", "continuous_unattached", true));
    EXPECT_NE(
        changed_registration.find("scene_runtime_observer_request_conflict"),
        std::string::npos)
        << changed_registration;
    const auto monotonic_before_observation =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto accepted = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U));
    const auto monotonic_after_observation =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    EXPECT_NE(accepted.find("\"disposition\":\"observed_only\""),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"fresh\":true"), std::string::npos) << accepted;
    EXPECT_NE(accepted.find("\"scene_application_status\":\"not_requested\""),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_projection_revision\":null"),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_request_id\":null"), std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_update_status\":null"), std::string::npos)
        << accepted;
    const auto accepted_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(accepted_snapshot.has_value());
    const auto &accepted_pose_sources =
        RequiredMember(RequiredMember(accepted_snapshot->payload, "state"),
                       "pose_sources")
            .as_array();
    ASSERT_EQ(accepted_pose_sources.size(), 1U);
    const auto observed_at_ns =
        RequiredMember(accepted_pose_sources.front(), "observed_at_ns")
            .as_number();
    EXPECT_GE(observed_at_ns,
              static_cast<double>(monotonic_before_observation));
    EXPECT_LE(observed_at_ns, static_cast<double>(monotonic_after_observation));
    const auto duplicate = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U));
    EXPECT_EQ(duplicate, accepted);
    const auto changed_intent =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U,
                                               1000000000ULL, 0.5));
    EXPECT_NE(changed_intent.find("scene_runtime_observation_request_conflict"),
              std::string::npos)
        << changed_intent;
    const auto wrong_identity = DifferentSceneRuntimeIdentityValue(*initial);
    const auto crossed_runtime = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 2U,
                                   1000000000ULL, 0.25, &wrong_identity));
    EXPECT_NE(crossed_runtime.find("scene_runtime_identity_changed"),
              std::string::npos)
        << crossed_runtime;

    const auto writable_open =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "writer-epoch-1", "isaac.writer.a",
                        "scene_object", "continuous_unattached", true));
    EXPECT_NE(writable_open.find("\"ok\":true"), std::string::npos)
        << writable_open;
    const auto conflicting_writer =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "writer-epoch-1", "isaac.writer.b",
                        "scene_object", "before_planning", true));
    EXPECT_NE(conflicting_writer.find("scene_runtime_pose_writer_conflict"),
              std::string::npos)
        << conflicting_writer;
    const auto undeclared_observation =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserveRequest(*initial, "writer-epoch-1", 1U,
                                               1000000000ULL, 0.25, nullptr,
                                               "isaac.writer.a", "scene_object",
                                               "before_planning", true));
    EXPECT_NE(
        undeclared_observation.find("scene_runtime_observation_not_declared"),
        std::string::npos)
        << undeclared_observation;

    const auto before_planning_open =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "planner-epoch-1", "isaac.planning-writer",
                        "scene-process-box", "before_planning", true));
    EXPECT_NE(before_planning_open.find("\"ok\":true"), std::string::npos)
        << before_planning_open;
    const auto added_object = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial_scene, "scene-runtime-planning-object.1", 1U));
    EXPECT_NE(added_object.find("\"authoring_not_ready\""), std::string::npos)
        << added_object;
    const auto scene_before_latch = scene_source.Read();
    ASSERT_TRUE(scene_before_latch.has_value());
    EXPECT_EQ(scene_before_latch->source_generation,
              initial_scene->source_generation);
    const auto before_planning = SendCommand(
        directory.socket_path(),
        SceneRuntimePoseObserveRequest(*initial, "planner-epoch-1", 1U));
    EXPECT_NE(before_planning.find(
                  "\"scene_application_status\":\"deferred_before_planning\""),
              std::string::npos)
        << before_planning;
    EXPECT_NE(before_planning.find("\"scene_projection_revision\":null"),
              std::string::npos)
        << before_planning;
    const auto latched = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_before_latch));
    EXPECT_NE(latched.find("scene_runtime_pose_provider_invalid"),
              std::string::npos)
        << latched;
    const auto scene_after_latch = scene_source.Read();
    ASSERT_TRUE(scene_after_latch.has_value());
    const auto observed_pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*scene_after_latch, "scene-process-box"));
    EXPECT_NE(observed_pose.find("\"object_not_found\""), std::string::npos)
        << observed_pose;
    const auto repeated_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_after_latch));
    EXPECT_NE(repeated_latch.find("scene_runtime_pose_provider_invalid"),
              std::string::npos)
        << repeated_latch;
    const auto advanced_scene =
        SendCommand(directory.socket_path(),
                    SceneAddRequest(*scene_after_latch, "scene-runtime-fence.1",
                                    1U, 0.2, "scene-process-fence-box"));
    EXPECT_NE(advanced_scene.find("\"authoring_not_ready\""), std::string::npos)
        << advanced_scene;
    const auto scene_after_advance = scene_source.Read();
    ASSERT_TRUE(scene_after_advance.has_value());
    EXPECT_EQ(scene_after_advance->source_generation,
              initial_scene->source_generation);
    const auto expiring = SendCommand(
        directory.socket_path(),
        SceneRuntimePoseObserveRequest(*initial, "planner-epoch-1", 2U, 1U));
    EXPECT_NE(expiring.find("\"scene_application_status\":\"not_requested\""),
              std::string::npos)
        << expiring;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto stale_observation_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_after_advance));
    EXPECT_NE(
        stale_observation_latch.find("scene_runtime_pose_observation_stale"),
        std::string::npos)
        << stale_observation_latch;

    const auto observed = scene_runtime_source.Read();
    ASSERT_TRUE(observed.has_value());
    const auto observed_bytes =
        content_identity::canonical_json(observed->payload);
    EXPECT_NE(observed_bytes.find("\"source_id\":\"isaac.observer\""),
              std::string::npos);
    EXPECT_NE(observed_bytes.find("\"kind\":\"joint_state\""),
              std::string::npos);

    const auto opened_replacement = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserverOpenRequest(*initial, "observer-epoch-2"));
    EXPECT_NE(opened_replacement.find("\"source_epoch_order\":2"),
              std::string::npos)
        << opened_replacement;
    const auto stale = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 2U));
    EXPECT_NE(stale.find("scene_runtime_observer_session_stale"),
              std::string::npos)
        << stale;
    const auto replacement_accepted = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-2", 1U, 1U));
    EXPECT_NE(replacement_accepted.find("\"source_epoch_order\":2"),
              std::string::npos)
        << replacement_accepted;

    const auto freshness_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool observed_stale = false;
    while (std::chrono::steady_clock::now() < freshness_deadline) {
      const auto snapshot = scene_runtime_source.Read();
      if (snapshot.has_value() &&
          content_identity::canonical_json(snapshot->payload)
                  .find("\"fresh\":false") != std::string::npos) {
        observed_stale = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed_stale);
    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory);
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_runtime_source(directory.publication_root(),
                                                   "scene-runtime-source.json");
  const auto replacement_snapshot = scene_runtime_source.Read();
  ASSERT_TRUE(replacement_snapshot.has_value());
  EXPECT_EQ(content_identity::canonical_json(replacement_snapshot->payload)
                .find("\"source_id\":\"isaac.observer\""),
            std::string::npos);
  const auto stale_after_owner_loss =
      SendCommand(directory.socket_path(),
                  SceneRuntimeObserveRequest(*replacement_snapshot,
                                             "observer-epoch-2", 2U));
  EXPECT_NE(stale_after_owner_loss.find("scene_runtime_observer_session_stale"),
            std::string::npos)
      << stale_after_owner_loss;
  const auto reopened = SendCommand(
      directory.socket_path(), SceneRuntimeObserverOpenRequest(
                                   *replacement_snapshot, "observer-epoch-3"));
  EXPECT_NE(reopened.find("\"source_epoch_order\":1"), std::string::npos)
      << reopened;
  EXPECT_TRUE(WIFEXITED(replacement.Stop(SIGTERM)));
}

TEST(ManagerDaemonProcess,
     PackageRootReplacementLatchesTypedRestartWhileReadsAndStopRemainOpen) {
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto selected_root = directory.apply_package_root();
  const auto displaced_root = selected_root.string() + ".displaced";
  ManagerProcess manager(directory, selected_root.string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before.has_value());
  const auto children_before_replacement = ReadDirectChildren(manager.pid());
  std::filesystem::rename(selected_root, displaced_root);
  ASSERT_TRUE(std::filesystem::create_directory(selected_root));

  const auto selection_bytes = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"canonical_empty_selection"})");
  const auto selection =
      deployment::parse_strict_json_model_yaml(selection_bytes).root();
  EXPECT_EQ(RequiredMember(selection, "ok"),
            content_identity::CanonicalValue(false))
      << selection_bytes;
  EXPECT_EQ(RequiredMember(RequiredMember(selection, "error"), "code"),
            content_identity::CanonicalValue("full_stack_restart_required"))
      << selection_bytes;
  EXPECT_EQ(ReadDirectChildren(manager.pid()), children_before_replacement);

  const auto reconcile = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})");
  EXPECT_EQ(ReadDirectChildren(manager.pid()), children_before_replacement);
  EXPECT_NE(reconcile.find("\"code\":\"full_stack_restart_required\""),
            std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"reason\":\"snapshot_integrity_lost\""),
            std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"retryable\":\"false\""), std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"required_action\":\"complete_stack_stop_start\""),
            std::string::npos)
      << reconcile;

  const auto apply =
      ApplyRequest(directory, before->source_owner_epoch, 1U, "candidate",
                   kDeploymentRevision, "current-graph", "current-bundle",
                   "request-1", {}, true);
  EXPECT_NE(apply.find("\"code\":\"full_stack_restart_required\""),
            std::string::npos)
      << apply;

  const auto owner_read = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"scene_owner_read"})");
  EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos) << owner_read;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::optional<SourceSnapshot> blocked;
  while (std::chrono::steady_clock::now() < deadline) {
    blocked = manager_source.Read();
    if (blocked.has_value() &&
        content_identity::canonical_json(blocked->payload)
                .find("full_stack_restart_required") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(blocked.has_value());
  const auto blocked_payload =
      content_identity::canonical_json(blocked->payload);
  EXPECT_NE(blocked_payload.find("\"manager_ready\":false"), std::string::npos);
  EXPECT_NE(blocked_payload.find(
                "\"unavailable_reason_code\":\"full_stack_restart_required\""),
            std::string::npos);

  const auto shutdown = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"shutdown_runtime"})");
  EXPECT_NE(shutdown.find("\"ok\":true"), std::string::npos) << shutdown;
  const auto status = manager.Wait();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ManagerDaemonProcess, EnforcesOneProcessOwnerPerManagerStateRoot) {
  DaemonDirectory directory;
  ManagerProcess owner(directory);
  owner.WaitReady(directory);
  ManagerProcess contender(directory);
  const auto contender_status = contender.Wait();
  ASSERT_TRUE(WIFEXITED(contender_status));
  EXPECT_EQ(WEXITSTATUS(contender_status), 70);
  EXPECT_EQ(::kill(owner.pid(), 0), 0);
  const auto owner_status = owner.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(owner_status));
  EXPECT_EQ(WEXITSTATUS(owner_status), 0);
}

TEST(ManagerDaemonProcess,
     SceneCommandsFailClosedWithoutKernelAndReadsSurviveReplacement) {
  DaemonDirectory directory;
  std::string request;
  std::string committed_digest;
  std::uint64_t committed_generation = 0U;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    const auto initial = scene_source.Read();
    ASSERT_TRUE(initial.has_value());
    const auto owner_read =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"scene_owner_read\"}");
    EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(owner_read.find(initial->source_owner_epoch), std::string::npos);
    const auto preview = SendCommand(
        directory.socket_path(),
        ScenePreviewRequest(*initial, "scene-process-preview.1", 1U));
    EXPECT_NE(preview.find("\"ok\":false"), std::string::npos) << preview;
    EXPECT_NE(
        preview.find("\"reason\":\"scene_candidate_operation_unsupported\""),
        std::string::npos)
        << preview;
    const auto after_preview = scene_source.Read();
    ASSERT_TRUE(after_preview.has_value());
    EXPECT_EQ(after_preview->semantic_content_digest,
              initial->semantic_content_digest);
    EXPECT_EQ(after_preview->source_generation, initial->source_generation);
    request = SceneAddRequest(*initial, "scene-process-request.1", 1U);
    const auto committed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(committed.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(committed.find("\"authoring_not_ready\""), std::string::npos);
    EXPECT_NE(committed.find("\"replay_status\":\"accepted_new\""),
              std::string::npos);
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"authoring_not_ready\""), std::string::npos);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos);

    const auto published = scene_source.Read();
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->source_generation, initial->source_generation);
    const auto pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*published, "scene-process-box"));
    EXPECT_NE(pose.find("\"object_not_found\""), std::string::npos);
    const auto stale_pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*initial, "scene-process-box"));
    EXPECT_NE(stale_pose.find("\"object_not_found\""), std::string::npos);
    EXPECT_NE(stale_pose.find(
                  "\"details\":{\"scene_resource_id\":\"scene-process-box\"}"),
              std::string::npos)
        << stale_pose;
    const auto absent_pose = SendCommand(
        directory.socket_path(), ScenePoseRequest(*published, "absent-object"));
    EXPECT_NE(absent_pose.find("\"object_not_found\""), std::string::npos);
    EXPECT_NE(absent_pose.find(
                  "\"details\":{\"scene_resource_id\":\"absent-object\"}"),
              std::string::npos)
        << absent_pose;
    committed_digest = published->semantic_content_digest;
    committed_generation = published->source_generation;

    const auto conflict = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial, "scene-process-request.1", 1U, 0.15));
    EXPECT_NE(conflict.find("\"request_id_conflict\""), std::string::npos);
    const auto stale = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial, "scene-process-request.stale", 1U));
    EXPECT_NE(stale.find("\"authoring_not_ready\""), std::string::npos);

    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory);
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto recovered = scene_source.Read();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->semantic_content_digest, committed_digest);
  EXPECT_EQ(recovered->source_generation, committed_generation);
  const auto replayed = SendCommand(directory.socket_path(), request);
  EXPECT_NE(replayed.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(replayed.find("\"authoring_not_ready\""), std::string::npos);
  EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos);
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
TEST(ManagerDaemonProcess,
     NativeLivenessWarningsNameTheirScopeAndRearmAfterActualSuccess) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  const auto socket = directory.program_runner_liveness_socket_path();
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename,
                         installed_manager.string(), socket.string());
  manager.WaitReady(directory);

  const std::string prefix =
      "Manager native liveness renewal remains fail-closed:";
  const auto warning_count = [&] {
    const auto text = ReadStartupStderr(directory);
    std::size_t count = 0U;
    for (auto position = text.find(prefix); position != std::string::npos;
         position = text.find(prefix, position + prefix.size()))
      ++count;
    return count;
  };
  const auto wait_for_warning = [&](const std::size_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (warning_count() < expected &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(warning_count(), expected) << ReadStartupStderr(directory);
  };
  const auto heartbeat = std::chrono::nanoseconds(
      deployment::execution_runtime_contract::manager_liveness_policy()
          .heartbeat_period_ns);
  const auto expect_unchanged_over_heartbeats = [&](const std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 3 * heartbeat;
    while (std::chrono::steady_clock::now() < deadline) {
      EXPECT_EQ(warning_count(), expected) << ReadStartupStderr(directory);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  wait_for_warning(1U);
  EXPECT_NE(ReadStartupStderr(directory).find("cell=" + fixture.cell_id),
            std::string::npos);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "predicate=program_runner_liveness_probe "
                "scope=cell-role:program_runner"),
            std::string::npos);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "Program Runner liveness socket is not an exact same-user socket"),
            std::string::npos);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  expect_unchanged_over_heartbeats(1U);

  // A stale same-user socket changes the actual connection failure, without
  // introducing another server or modifying the renewal packet contract.
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = socket.string();
  ASSERT_LT(spelling.size(), sizeof(address.sun_path));
  const int stale = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(stale, 0);
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  const int bound = ::bind(stale, reinterpret_cast<sockaddr *>(&address),
                           sizeof(address));
  static_cast<void>(::close(stale));
  ASSERT_EQ(bound, 0);
  wait_for_warning(2U);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "connect Program Runner liveness socket"),
            std::string::npos);
  expect_unchanged_over_heartbeats(2U);
  std::filesystem::remove(socket);

  ProgramRunnerProcess runner(directory, fixture.cell_id);
  // Owner-issued admission proves the complete validated renewal/readiness
  // path succeeded; a probe or a retry-gate reset alone is not recovery.
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  const auto recovered_warnings = warning_count();
  expect_unchanged_over_heartbeats(recovered_warnings);
  const auto saved_socket = socket.string() + ".saved";
  std::filesystem::rename(socket, saved_socket);
  wait_for_warning(recovered_warnings + 1U);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  // Restore promptly, before spending the existing lease-expiry budget. No
  // changed intermediate cause may disguise a missing success-edge reset.
  std::filesystem::rename(saved_socket, socket);
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  expect_unchanged_over_heartbeats(recovered_warnings + 1U);
  std::filesystem::rename(socket, saved_socket);
  wait_for_warning(recovered_warnings + 2U);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  std::filesystem::rename(saved_socket, socket);
  const auto stopped = manager.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

TEST(ManagerDaemonProcess,
     GripperOnlyNativeApplyPublishesAControllableCellWithoutMotion) {
  const auto fixture = ReadGripperOnlyManagerApplyFixtureMetadata();
  DaemonDirectory directory(true);
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_GRIPPER_ONLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared = package_activations.get_verified(
      core::ContentDigest(fixture.prepared_activation_id));
  ASSERT_NE(prepared, nullptr);
  const deployment::FilesystemPreparedActivationRepository daemon_activations(
      directory.activation_root());
  ASSERT_TRUE(daemon_activations.put_if_absent(*prepared));
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename,
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);

  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto initial = manager_source.Read();
  ASSERT_TRUE(initial.has_value());
  const auto request = ApplyRequest(
      directory, initial->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      fixture.physical_deployment_artifact_id,
      fixture.physical_deployment_revision,
      fixture.predecessor_graph_revision,
      fixture.predecessor_retained_bundle_id,
      "manager-gripper-only-apply.1", {}, false, "receipt",
      fixture.prepared_activation_id);
  const auto submitted = SendCommand(directory.socket_path(), request);
  ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos)
      << submitted;
  auto applied = submitted;
  const auto status_request = ApplyStatusRequest(submitted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (applied.find("\"state\":\"succeeded\"") ==
             std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    applied = SendCommand(directory.socket_path(), status_request);
  }
  ASSERT_NE(applied.find("\"state\":\"succeeded\""),
            std::string::npos)
      << applied << "\nmanager stderr:\n" << ReadStartupStderr(directory);
  const auto ready = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_FALSE(ready.owner_epoch.empty());
  const auto status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
  ASSERT_NE(status.find("\"ok\":true"), std::string::npos) << status;
  EXPECT_NE(status.find("tool.gripper.command"), std::string::npos)
      << status;
  EXPECT_EQ(status.find("execution.trajectory.execute"), std::string::npos)
      << status;

  // The first physical Apply used the selected-product path, not the current
  // bootstrap builder. Empty installation must retain that exact ownership
  // through the interval before the public pointer switches to empty.
  const auto native_children = ReadDirectChildren(manager.pid());
  ASSERT_FALSE(native_children.empty());
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource empty_source(
      directory.apply_package_root(), fixture.admission_filename);
  const auto empty_admission = empty_source.load_verified();
  const deployment::FilesystemVerifiedCatalogArtifactSource empty_catalog(
      directory.apply_package_root());
  const deployment::FilesystemVerifiedRawArtifactSource empty_raw(
      directory.apply_package_root());
  const auto empty_prepared = deployment::assemble_sealed_canonical_empty_activation(
      empty_admission, empty_catalog, empty_raw);
  ASSERT_TRUE(daemon_activations.put_if_absent(empty_prepared));
  const auto empty_submitted = SendCommand(directory.socket_path(), ApplyRequest(
      directory, initial->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      empty_admission.deployment_artifact_id().value(),
      empty_admission.deployment_revision().value(), "current-graph",
      "current-bundle", "manager-gripper-only-empty.2", {}, false, "receipt",
      empty_prepared.prepared_activation_id().value(), &empty_admission));
  ASSERT_NE(empty_submitted.find("\"apply_id\""), std::string::npos)
      << empty_submitted;
  auto emptied = empty_submitted;
  const auto empty_status_request = ApplyStatusRequest(empty_submitted);
  const auto empty_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (emptied.find("\"state\":\"succeeded\"") == std::string::npos &&
         std::chrono::steady_clock::now() < empty_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    emptied = SendCommand(directory.socket_path(), empty_status_request);
  }
  ASSERT_NE(emptied.find("\"state\":\"succeeded\""), std::string::npos)
      << emptied << "\nmanager stderr:\n" << ReadStartupStderr(directory);
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  const auto empty_current = manager_source.Read();
  ASSERT_TRUE(empty_current.has_value());
  EXPECT_EQ(empty_current->source_owner_epoch, initial->source_owner_epoch);
  const auto &empty_active = RequiredMember(empty_current->payload, "active");
  EXPECT_EQ(RequiredMember(empty_active, "canonical_empty"),
            content_identity::CanonicalValue(true));
  EXPECT_EQ(RequiredText(empty_active, "retained_bundle_id"),
            empty_admission.retained_bundle_id().value());
  EXPECT_EQ(RequiredText(empty_active, "graph_revision"),
            empty_admission.graph_revision().value());
  EXPECT_EQ(ReadDirectChildren(manager.pid()), native_children);
  EXPECT_EQ(ReadStartupStderr(directory).find(
                "native composition phase: materializing runtime"),
            std::string::npos);

  // A fresh explicit Apply still owns the normal selected-product route.
  // Use the actual empty predecessor and a new request, never retry a mutation.
  const auto reapplied_submission = SendCommand(directory.socket_path(), ApplyRequest(
      directory, empty_current->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      fixture.physical_deployment_artifact_id,
      fixture.physical_deployment_revision,
      empty_admission.graph_revision().value(),
      empty_admission.retained_bundle_id().value(),
      "manager-gripper-only-reapply.3", {}, false, "receipt",
      fixture.prepared_activation_id));
  ASSERT_NE(reapplied_submission.find("\"apply_id\""), std::string::npos)
      << reapplied_submission;
  EXPECT_NE(ApplyStatusRequest(reapplied_submission), empty_status_request);
  auto reapplied = reapplied_submission;
  const auto reapply_status_request = ApplyStatusRequest(reapplied_submission);
  const auto reapply_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (reapplied.find("\"state\":\"succeeded\"") == std::string::npos &&
         std::chrono::steady_clock::now() < reapply_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    reapplied = SendCommand(directory.socket_path(), reapply_status_request);
  }
  ASSERT_NE(reapplied.find("\"state\":\"succeeded\""), std::string::npos)
      << reapplied << "\nmanager stderr:\n" << ReadStartupStderr(directory);
  const auto reopened = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_EQ(reopened.owner_epoch, ready.owner_epoch);
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  EXPECT_EQ(ReadDirectChildren(manager.pid()), native_children);
  EXPECT_EQ(ReadStartupStderr(directory).find(
                "native composition phase: materializing runtime"),
            std::string::npos);
}

TEST(
    ManagerDaemonProcess,
    PneumaticCommandedFeedbackAliasInitialApplyPublishesAControllableCellWithoutMotion) {
  const auto fixture = ReadGripperOnlyManagerApplyFixtureMetadata(
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_METADATA);
  DaemonDirectory directory(true);
  std::filesystem::copy(
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_PACKAGE_ROOT,
      directory.apply_package_root(), std::filesystem::copy_options::recursive);
  // Inspect the actual generated product: the command and feedback bindings
  // share one pneumatic target, so this exercises the scalar alias closure.
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(
      directory.apply_package_root());
  const auto candidate = bundles.get_verified(
      core::RetainedBundleId(fixture.physical_candidate_retained_bundle_id));
  ASSERT_NE(candidate, nullptr);
  const auto &graph = candidate->value().cell_graph;
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.control_bindings.size(), 2U);
  const auto configuration = deployment::
      ros2_control_execution_runtime_contract::require_configuration(
          *candidate, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.gripper_target_configurations.size(), 1U);
  const auto &gripper = configuration.gripper_target_configurations.front();
  EXPECT_EQ(gripper.actuation_kind, "pneumatic");
  EXPECT_EQ(gripper.output_mode, core::GripperOutputMode::kCommanded);
  ASSERT_EQ(configuration.controller_profiles.size(), 1U);
  EXPECT_EQ(configuration.controller_profiles.front().mock_profile,
            std::optional<std::string>("lite6_lite_gripper"));
  const auto command_binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "tool.gripper.command";
      });
  const auto feedback_binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "tool.gripper.feedback";
      });
  ASSERT_NE(command_binding, graph.control_bindings.end());
  ASSERT_NE(feedback_binding, graph.control_bindings.end());
  EXPECT_EQ(command_binding->controllable_component_id,
            graph.controllable_components.front().controllable_component_id);
  EXPECT_EQ(command_binding->controllable_component_id,
            gripper.controllable_component_id);
  EXPECT_EQ(command_binding->command_target_id, gripper.command_target_id);
  EXPECT_EQ(feedback_binding->controllable_component_id,
            command_binding->controllable_component_id);
  EXPECT_EQ(feedback_binding->command_target_id,
            command_binding->command_target_id);
  EXPECT_EQ(feedback_binding->control_domain_id,
            command_binding->control_domain_id);
  EXPECT_EQ(feedback_binding->provider_runtime_unit_id,
            command_binding->provider_runtime_unit_id);
  EXPECT_EQ(feedback_binding->resource_bindings,
            command_binding->resource_bindings);

  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared = package_activations.get_verified(
      core::ContentDigest(fixture.prepared_activation_id));
  ASSERT_NE(prepared, nullptr);
  const deployment::FilesystemPreparedActivationRepository daemon_activations(
      directory.activation_root());
  ASSERT_TRUE(daemon_activations.put_if_absent(*prepared));
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename, installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);

  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto initial = manager_source.Read();
  ASSERT_TRUE(initial.has_value());
  const auto request = ApplyRequest(
      directory, initial->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      fixture.physical_deployment_artifact_id,
      fixture.physical_deployment_revision, fixture.predecessor_graph_revision,
      fixture.predecessor_retained_bundle_id,
      "manager-pneumatic-commanded-feedback-apply.1", {}, false, "receipt",
      fixture.prepared_activation_id);
  const auto submitted = SendCommand(directory.socket_path(), request);
  ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
  auto applied = submitted;
  const auto status_request = ApplyStatusRequest(submitted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    applied = SendCommand(directory.socket_path(), status_request);
  }
  ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
      << applied << "\nmanager stderr:\n"
      << ReadStartupStderr(directory);
  const auto ready = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_FALSE(ready.owner_epoch.empty());
  const auto current = manager_source.Read();
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(RequiredText(RequiredMember(current->payload, "active"),
                         "retained_bundle_id"),
            fixture.physical_candidate_retained_bundle_id);
  const auto status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
  ASSERT_NE(status.find("\"ok\":true"), std::string::npos) << status;
  EXPECT_NE(status.find("tool.gripper.command"), std::string::npos) << status;
  EXPECT_EQ(status.find("execution.trajectory.execute"), std::string::npos)
      << status;
}

TEST(ManagerDaemonProcess,
     CompatibleNativeReplacementUsesFreshEpochAfterCleanStop) {
  const auto fixture_a = ReadManagerApplyFixtureMetadata();
  const auto fixture_b = ReadManagerApplyFixtureMetadata(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA);
  ASSERT_EQ(fixture_a.candidate_retained_bundle_id,
            fixture_b.candidate_retained_bundle_id);

  const std::filesystem::path package_a =
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT;
  const std::filesystem::path package_b =
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT;
  ASSERT_NE(package_a, package_b);

  DaemonDirectory directory_a;
  std::filesystem::copy(package_a, directory_a.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager_a = PrepareManagerApplyInstalledTargets(
      directory_a, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST);
  const auto identity_a = ReadNativePackageIdentity(
      package_a, fixture_a.physical_candidate_retained_bundle_id,
      directory_a.installed_root() / "manyforge_ros2_control_execution_host");
  ASSERT_TRUE(component_manager::VerifyNativeExecutionRuntimeUnitForHost(
                  *identity_a.bundle,
                  identity_a.bundle->value()
                      .cell_graph.runtime_units.front()
                      .runtime_unit_id,
                  deployment::FilesystemVerifiedRetainedProductArtifactSource(
                      package_a),
                  "linux",
                  deployment::ros2_control_execution_runtime_contract::
                      kLaunchEnvironmentContract)
                  .has_value());

  ProgramRunnerProcess program_runner_a(directory_a, fixture_a.cell_id);
  std::string manager_epoch_a;
  std::string runtime_epoch_a;
  {
    ManagerProcess manager(
        directory_a, package_a.string(), fixture_a.admission_filename,
        installed_manager_a.string(), program_runner_a.socket_path().string());
    manager.WaitReady(directory_a);
    AtomicSourceSnapshotChannel manager_source(directory_a.publication_root(),
                                               "manager-source.json");
    const auto initial = manager_source.Read();
    ASSERT_TRUE(initial.has_value());
    manager_epoch_a = initial->source_owner_epoch;
    const auto admission = WaitForAuthoringAdmission(directory_a);
    const auto request = ApplyRequest(directory_a, manager_epoch_a,
                                      admission.admission_generation,
                                      fixture_a.physical_deployment_artifact_id,
                                      fixture_a.physical_deployment_revision,
                                      fixture_a.predecessor_graph_revision,
                                      fixture_a.predecessor_retained_bundle_id,
                                      "manager-o1-4g-replacement-a.1");
    const auto submitted = SendCommand(directory_a.socket_path(), request);
    ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory_a.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    const auto status = deployment::parse_strict_json_model_yaml(applied).root();
    const auto &prepared = RequiredMember(status, "prepared_activation");
    ASSERT_TRUE(prepared.is_object());
    EXPECT_EQ(prepared.as_object().size(), 5U);
    const auto requested = deployment::parse_strict_json_model_yaml(request).root();
    EXPECT_EQ(RequiredText(prepared, "prepared_activation_id"),
              RequiredText(RequiredMember(requested, "prepared_activation"),
                           "prepared_activation_id"));
    ASSERT_NE(applied.find(fixture_a.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;
    const auto ready = WaitForExecutionReady(
        directory_a, fixture_a.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(ready.owner_epoch.empty());
    runtime_epoch_a = ready.owner_epoch;
    ASSERT_FALSE(runtime_epoch_a.empty());
    const auto stopped = manager.Stop(SIGTERM);
    ASSERT_TRUE(WIFEXITED(stopped));
    EXPECT_EQ(WEXITSTATUS(stopped), 0);
    EXPECT_FALSE(std::filesystem::exists(directory_a.socket_path()));
    EXPECT_FALSE(std::filesystem::exists(directory_a.execution_socket_path()));
  }

  DaemonDirectory directory_b;
  std::filesystem::copy(package_b, directory_b.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager_b = PrepareManagerApplyInstalledTargets(
      directory_b, MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST);
  const auto identity_b = ReadNativePackageIdentity(
      package_b, fixture_b.physical_candidate_retained_bundle_id,
      directory_b.installed_root() / "manyforge_ros2_control_execution_host");
  ASSERT_TRUE(component_manager::VerifyNativeExecutionRuntimeUnitForHost(
                  *identity_b.bundle,
                  identity_b.bundle->value()
                      .cell_graph.runtime_units.front()
                      .runtime_unit_id,
                  deployment::FilesystemVerifiedRetainedProductArtifactSource(
                      package_b),
                  "linux",
                  deployment::ros2_control_execution_runtime_contract::
                      kLaunchEnvironmentContract)
                  .has_value());
  EXPECT_NE(identity_a.executable_digest, identity_b.executable_digest);
  // Different compatible executables consume the same semantic package. Their
  // release identity is diagnostic and no longer package admission authority.
  EXPECT_EQ(identity_a.product.product_identity(),
            identity_b.product.product_identity());
  EXPECT_EQ(identity_a.product.configuration_identity(),
            identity_b.product.configuration_identity());
  EXPECT_EQ(identity_a.bundle->retained_bundle_id(),
            identity_b.bundle->retained_bundle_id());
  EXPECT_EQ(identity_a.offer, identity_b.offer);
  EXPECT_EQ(identity_a.offer.offer_digest, identity_b.offer.offer_digest);

  ProgramRunnerProcess program_runner_b(directory_b, fixture_b.cell_id);
  std::string manager_epoch_b;
  std::string runtime_epoch_b;
  {
    ManagerProcess manager(
        directory_b, package_b.string(), fixture_b.admission_filename,
        installed_manager_b.string(), program_runner_b.socket_path().string());
    manager.WaitReady(directory_b);
    AtomicSourceSnapshotChannel manager_source(directory_b.publication_root(),
                                               "manager-source.json");
    const auto initial = manager_source.Read();
    ASSERT_TRUE(initial.has_value());
    manager_epoch_b = initial->source_owner_epoch;
    const auto admission = WaitForAuthoringAdmission(directory_b);
    const auto request = ApplyRequest(directory_b, manager_epoch_b,
                                      admission.admission_generation,
                                      fixture_b.physical_deployment_artifact_id,
                                      fixture_b.physical_deployment_revision,
                                      fixture_b.predecessor_graph_revision,
                                      fixture_b.predecessor_retained_bundle_id,
                                      "manager-o1-4g-replacement-b.1");
    const auto submitted = SendCommand(directory_b.socket_path(), request);
    ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory_b.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    ASSERT_NE(applied.find(fixture_b.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;
    const auto ready = WaitForExecutionReady(
        directory_b, fixture_b.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(ready.owner_epoch.empty());
    runtime_epoch_b = ready.owner_epoch;
    ASSERT_FALSE(runtime_epoch_b.empty());
    EXPECT_NE(manager_epoch_a, manager_epoch_b);
    EXPECT_NE(runtime_epoch_a, runtime_epoch_b);
    const auto stopped = manager.Stop(SIGTERM);
    ASSERT_TRUE(WIFEXITED(stopped));
    EXPECT_EQ(WEXITSTATUS(stopped), 0);
    EXPECT_FALSE(std::filesystem::exists(directory_b.socket_path()));
    EXPECT_FALSE(std::filesystem::exists(directory_b.execution_socket_path()));
  }
}

TEST(ManagerDaemonProcess,
     DestructiveApplyImpactRequiresExplicitAcceptanceOfItsExactDigest) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, package_root, fixture.admission_filename,
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before.has_value());
  const auto admission =
      WaitForAuthoringAdmission(directory).admission_generation;
  // The runtime-only candidate has none of the fixture Program's components:
  // the Program becomes non-executable, a destructive impact (H1a).
  const auto submit = [&](const std::string_view request_id,
                          const std::string_view accepted) {
    const auto request =
        ApplyRequest(directory, before->source_owner_epoch, admission,
                     fixture.deployment_artifact_id, fixture.deployment_revision,
                     fixture.predecessor_graph_revision,
                     fixture.predecessor_retained_bundle_id, request_id, {},
                     false, accepted);
    const auto response = SendCommand(directory.socket_path(), request);
    return response;
  };

  const auto unaccepted = submit("impact-unaccepted", "");
  EXPECT_NE(unaccepted.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << unaccepted;
  const auto wrong = submit(
      "impact-wrong",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_NE(wrong.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << wrong;
  // Neither refusal recorded an operation or changed the active selection.
  const auto unchanged = manager_source.Read();
  ASSERT_TRUE(unchanged.has_value());
  const auto unchanged_payload =
      content_identity::canonical_json(unchanged->payload);
  EXPECT_EQ(unchanged_payload.find("impact-unaccepted"), std::string::npos);
  EXPECT_EQ(unchanged_payload.find("impact-wrong"), std::string::npos);

  const auto accepted = submit("impact-accepted", "receipt");
  EXPECT_EQ(accepted.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << accepted;
  EXPECT_NE(accepted.find("\"ok\":true"), std::string::npos) << accepted;
}

TEST(ManagerDaemonProcess,
     NamedApplyUsesTheRealManagedFenceAndSurvivesReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  const auto wait_execution_response = [&] {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string status;
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        status = SendCommand(
            directory.execution_socket_path(),
            R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
        if (status.find("\"ok\":true") != std::string::npos ||
            status.find("\"code\":\"execution_owner_not_ready\"") !=
                std::string::npos) {
          return status;
        }
      } catch (const std::exception &error) {
        status = std::string("transport_error:") + error.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return status;
  };
  std::string preserved_scene_snapshot;
  std::string first_manager_epoch;
  std::string request;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string(),
                           program_runner.socket_path().string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto before = manager_source.Read();
    const auto scene_before = scene_source.Read();
    const auto scene_runtime_before = scene_runtime_source.Read();
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(scene_before.has_value());
    const auto *const predecessor_snapshot =
        scene_before->payload.find_object_member("snapshot");
    ASSERT_NE(predecessor_snapshot, nullptr);
    ASSERT_TRUE(predecessor_snapshot->is_object());
    preserved_scene_snapshot = content_identity::canonical_json(*predecessor_snapshot);
    ASSERT_TRUE(scene_runtime_before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    EXPECT_NE(content_identity::canonical_json(before->payload)
                  .find("\"apply\":{\"mutation_available\":true"),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_before->payload)
                  .find(fixture.predecessor_retained_bundle_id),
              std::string::npos);
    request = ApplyRequest(
        directory, first_manager_epoch,
        WaitForAuthoringAdmission(directory).admission_generation,
        fixture.physical_deployment_artifact_id,
        fixture.physical_deployment_revision,
        fixture.predecessor_graph_revision,
        fixture.predecessor_retained_bundle_id, "manager-program-apply.1");

    // Model a browser disappearing after the kernel accepted its packet but
    // before it received any HTTP/RPC receipt. Manager must finish without
    // that client, and a replacement observer must reconstruct the result
    // from the authoritative inventory alone.
    SendCommandAndDisconnect(directory.socket_path(), request);
    std::string applied;
    // Every value Manager publishes while the Apply progresses is one
    // generation: a committed Apply is never shown beside the predecessor
    // publication (the transition lane once paired its last pre-Apply
    // runtime with a journal that already held the Apply's receipt).
    std::optional<std::string> split_generation;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < apply_deadline) {
      const auto observed = manager_source.Read();
      if (observed.has_value()) {
        applied = content_identity::canonical_json(observed->payload);
        if (!split_generation.has_value() &&
            !ManagerSourceHistoryIsOneGeneration(observed->payload)) {
          split_generation = applied;
        }
        if (applied.find(R"("request_id":"manager-program-apply.1")") !=
                std::string::npos &&
            applied.find(R"("terminal_state":"succeeded")") !=
                std::string::npos) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Apply status, the logical Scene, and the Scene-runtime identity are
    // separate owner publications. The Apply worker may publish its terminal
    // record before the Manager's bounded observation cycle has refreshed the
    // latter two projections. Configuration-only Apply preserves the actual
    // logical Scene; wait for the candidate runtime with that exact snapshot.
    const auto projection_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < projection_deadline) {
      const auto observed_scene = scene_source.Read();
      const auto observed_scene_runtime = scene_runtime_source.Read();
      if (observed_scene.has_value() && observed_scene_runtime.has_value() &&
          observed_scene->payload.find_object_member("snapshot") != nullptr &&
          content_identity::canonical_json(
              *observed_scene->payload.find_object_member("snapshot")) ==
              preserved_scene_snapshot &&
          content_identity::canonical_json(observed_scene_runtime->payload)
                  .find(fixture.physical_candidate_retained_bundle_id) !=
              std::string::npos) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(split_generation.has_value())
        << split_generation.value_or("");
    {
      const auto settled = manager_source.Read();
      ASSERT_TRUE(settled.has_value());
      EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(settled->payload))
          << content_identity::canonical_json(settled->payload);
    }
    SCOPED_TRACE(applied);
    EXPECT_NE(applied.find(R"("terminal_state":"succeeded")"),
              std::string::npos)
        << [&] {
             // The daemon's own diagnostics name the failed Apply phase.
             std::ifstream stderr_file(directory.startup_stderr());
             const std::string text{std::istreambuf_iterator<char>(stderr_file),
                                    std::istreambuf_iterator<char>()};
             return text.size() > 6000U ? text.substr(text.size() - 6000U) : text;
           }();
    EXPECT_NE(applied.find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto execution_status = wait_execution_response();
    EXPECT_NE(execution_status.find("\"ok\":true"), std::string::npos)
        << execution_status;
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos)
        << replayed;

    component_manager::SqliteManagerApplyJournal journal(
        directory.manager_root() /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId(fixture.cell_id));
    const auto inventory = journal.ReadStatusInventory();
    ASSERT_EQ(inventory.size(), 1U);
    ASSERT_TRUE(inventory.front().terminal_outcome.has_value());
    ASSERT_TRUE(inventory.front()
                    .terminal_outcome->terminal_outcome
                    .candidate_projection_revision.has_value());
    EXPECT_EQ(inventory.front()
                  .terminal_outcome->terminal_outcome
                  .candidate_projection_revision->retained_bundle_id.value(),
              fixture.physical_candidate_retained_bundle_id);

    const auto manager_after = manager_source.Read();
    const auto scene_after = scene_source.Read();
    const auto scene_runtime_after = scene_runtime_source.Read();
    ASSERT_TRUE(manager_after.has_value());
    ASSERT_TRUE(scene_after.has_value());
    ASSERT_TRUE(scene_runtime_after.has_value());
    EXPECT_NE(content_identity::canonical_json(manager_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto *const applied_snapshot = scene_after->payload.find_object_member("snapshot");
    ASSERT_NE(applied_snapshot, nullptr);
    EXPECT_EQ(content_identity::canonical_json(*applied_snapshot), preserved_scene_snapshot);
    EXPECT_EQ(content_identity::canonical_json(scene_after->payload).find("apply_fixture_marker"),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(
      directory, package_root, fixture.admission_filename,
      installed_manager.string(), program_runner.socket_path().string());
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime_source(directory.publication_root(),
                                                   "scene-runtime-source.json");
  const auto recovered = manager_source.Read();
  const auto recovered_scene = scene_source.Read();
  const auto scene_runtime_recovered = scene_runtime_source.Read();
  ASSERT_TRUE(recovered.has_value());
  ASSERT_TRUE(recovered_scene.has_value());
  ASSERT_TRUE(scene_runtime_recovered.has_value());
  const auto recovered_manager_bytes =
      content_identity::canonical_json(recovered->payload);
  EXPECT_NE(recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(recovered_manager_bytes.find(
                fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  EXPECT_NE(recovered_manager_bytes.find(
                R"("request_id":"manager-program-apply.1")"),
            std::string::npos);
  EXPECT_NE(
      recovered_manager_bytes.find(R"("incomplete":false,"intent_digest":)"),
      std::string::npos);
  EXPECT_NE(content_identity::canonical_json(scene_runtime_recovered->payload)
                .find(fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  const auto *const recovered_snapshot = recovered_scene->payload.find_object_member("snapshot");
  ASSERT_NE(recovered_snapshot, nullptr);
  EXPECT_EQ(content_identity::canonical_json(*recovered_snapshot), preserved_scene_snapshot);
  EXPECT_EQ(content_identity::canonical_json(recovered_scene->payload).find("apply_fixture_marker"),
            std::string::npos);
  const auto recovered_execution_status = wait_execution_response();
  // Reopening the Manager may read completed Apply evidence, but it must not
  // silently restart a physical execution owner after process loss.
  EXPECT_NE(recovered_execution_status.find("\"ok\":false"),
            std::string::npos)
      << recovered_execution_status;
  EXPECT_NE(recovered_execution_status.find(
                "\"code\":\"execution_owner_not_ready\""),
            std::string::npos)
      << recovered_execution_status;
  const auto replay = SendCommand(directory.socket_path(), request);
  EXPECT_NE(replay.find("\"state\":\"succeeded\""), std::string::npos)
      << replay;
  EXPECT_NE(replay.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos)
      << replay;
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

TEST(ManagerDaemonProcess,
     PhysicalCompositionStaysClosedAfterUnexpectedChildLossAndManagerReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  std::string first_manager_epoch;
  std::string apply_request;
  std::string applied_owner_epoch;
  RuntimeOfferProjection initial_offer;
  std::uint64_t advanced_scene_generation = 0U;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string(),
                           program_runner.socket_path().string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    const auto before = manager_source.Read();
    ASSERT_TRUE(before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    apply_request =
        ApplyRequest(directory, first_manager_epoch,
                     WaitForAuthoringAdmission(directory).admission_generation,
                     fixture.physical_deployment_artifact_id,
                     fixture.physical_deployment_revision,
                     fixture.predecessor_graph_revision,
                     fixture.predecessor_retained_bundle_id,
                     "manager-physical-recovery-apply.1");

    const auto submitted = SendCommand(directory.socket_path(), apply_request);
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    ASSERT_NE(applied.find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;

    const auto initial = WaitForExecutionReady(
        directory, fixture.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(initial.owner_epoch.empty());
    applied_owner_epoch = initial.owner_epoch;
    initial_offer =
        WaitForRuntimeOfferProjection(manager_source, initial.owner_epoch);
    EXPECT_EQ(initial_offer.selected, initial_offer.observed);
    EXPECT_EQ(initial.owner_epoch,
              "kernel-owner-" + initial_offer.observed_epoch);

    const auto advanced_scene_projection = scene_source.Read();
    ASSERT_TRUE(advanced_scene_projection.has_value());
    const auto advanced_scene = SendCommand(
        directory.socket_path(),
        SceneAddRequest(
            *advanced_scene_projection, "manager-physical-recovery-scene.1",
            WaitForAuthoringAdmission(directory).admission_generation));
    EXPECT_NE(advanced_scene.find("\"ok\":true"), std::string::npos)
        << advanced_scene;
    const auto scene_advance_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<SourceSnapshot> after_scene_advance;
    while (std::chrono::steady_clock::now() < scene_advance_deadline &&
           (!after_scene_advance.has_value() ||
            after_scene_advance->source_generation <=
                advanced_scene_projection->source_generation)) {
      after_scene_advance = scene_source.Read();
      if (after_scene_advance.has_value() &&
          after_scene_advance->source_generation >
              advanced_scene_projection->source_generation) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(after_scene_advance.has_value());
    EXPECT_GT(after_scene_advance->source_generation,
              advanced_scene_projection->source_generation);
    advanced_scene_generation = after_scene_advance->source_generation;

    const auto first_child =
        WaitForSoleManagedChild(manager.pid(), "before deliberate SIGKILL");
    ASSERT_EQ(::kill(first_child, SIGKILL), 0);

    // Unexpected loss of a required owner is contained fault evidence, not an
    // implicit replacement request. Steady-state observation and the public
    // reconcile command must never start the lost managed unit again; only the
    // Manager's own bootstrap may reinitialize it.
    const auto reap_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < reap_deadline &&
           !ReadDirectChildren(manager.pid()).empty()) {
      static_cast<void>(SendCommand(
          directory.socket_path(),
          R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})"));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(ReadDirectChildren(manager.pid()).empty())
        << "the exited managed child was not observed and reaped";
    for (int attempt = 0; attempt < 5; ++attempt) {
      const auto reconciled = SendCommand(
          directory.socket_path(),
          R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})");
      EXPECT_NE(reconciled.find("\"ok\":true"), std::string::npos)
          << reconciled;
      EXPECT_TRUE(ReadDirectChildren(manager.pid()).empty())
          << "steady-state observation restarted a lost managed child";
    }
    const auto manager_loss = manager_source.Read();
    ASSERT_TRUE(manager_loss.has_value());
    EXPECT_NE(content_identity::canonical_json(manager_loss->payload)
                  .find("\"manager_ready\":false"),
              std::string::npos)
        << "lost required owner must keep readiness closed";
    const auto status = manager.Stop(SIGKILL);
    ASSERT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(
      directory, package_root, fixture.admission_filename,
      installed_manager.string(), program_runner.socket_path().string());
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto recovered_scene = scene_source.Read();
  ASSERT_TRUE(recovered_scene.has_value());
  EXPECT_GE(recovered_scene->source_generation, advanced_scene_generation);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto manager_recovered = manager_source.Read();
  ASSERT_TRUE(manager_recovered.has_value());
  const auto manager_recovered_offer =
      ReadRuntimeOfferProjection(manager_recovered->payload);
  EXPECT_EQ(manager_recovered_offer.selected, manager_recovered_offer.observed);
  EXPECT_EQ(manager_recovered_offer.selected, initial_offer.selected);
  EXPECT_NE(manager_recovered_offer.observed_epoch,
            initial_offer.observed_epoch);
  EXPECT_NE("kernel-owner-" + manager_recovered_offer.observed_epoch,
            applied_owner_epoch);
  EXPECT_NE(manager_recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(content_identity::canonical_json(manager_recovered->payload)
                .find(fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  // An unexpected required-child loss is not a new Apply request. A Manager
  // restart may restore observation of the durable selection, but cannot
  // silently rebuild the old physical product or reopen control admission.
  const auto execution_status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
  EXPECT_NE(execution_status.find("\"code\":\"execution_owner_not_ready\""),
            std::string::npos)
      << execution_status;
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed)
      << SendCommand(directory.execution_socket_path(),
                     R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
  const auto replay = SendCommand(directory.socket_path(), apply_request);
  EXPECT_NE(replay.find("\"state\":\"succeeded\""), std::string::npos)
      << replay;
  EXPECT_NE(replay.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos)
      << replay;
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
  EXPECT_EQ(WEXITSTATUS(stopped), 0);
}
#endif

TEST(ManagerDaemonProcess,
     EmptyRosApplyConfiguresInactiveMockThenExplicitActivation) {
#if !defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
  if (std::getenv("MANYFORGE_ROS2_CONTROL_EXECUTION_HOST") != nullptr) {
    FAIL() << "Explicit ros-e2e lane did not register the production ROS host "
              "fixture";
  }
  GTEST_SKIP() << "Requires the production ROS host and sourced ros-e2e "
                  "container environment";
#else
  const auto *ros_host = std::getenv("MANYFORGE_ROS2_CONTROL_EXECUTION_HOST");
  if (ros_host == nullptr) {
    GTEST_SKIP() << "Requires the explicitly sourced ros-e2e lane";
  }
  ASSERT_TRUE(std::filesystem::equivalent(
      ros_host, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST));
  using content_identity::CanonicalValue;
  const auto read_file = [](const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) throw std::runtime_error("Fixture input is unavailable: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  // The same sourced ament lookup used by ros2 launch records the selected
  // overlay input. Refuse qualification against an older image launch file.
  const auto *launch_evidence = std::getenv("MANYFORGE_MANAGER_ROS_LAUNCH_EVIDENCE");
  ASSERT_NE(launch_evidence, nullptr);
  const auto launch_input = deployment::parse_strict_json_model_yaml(read_file(launch_evidence)).root();
  ASSERT_EQ(RequiredText(launch_input, "source_launch_file"),
            std::filesystem::canonical(MANYFORGE_HEADLESS_TEST_ROS_LAUNCH_SOURCE).string());
  const auto launch_hash = content_identity::sha256_digest(read_file(RequiredText(launch_input, "launch_file")));
  EXPECT_EQ(launch_hash, RequiredText(launch_input, "launch_sha256"));
  EXPECT_EQ(launch_hash, content_identity::sha256_digest(read_file(MANYFORGE_HEADLESS_TEST_ROS_LAUNCH_SOURCE)));
  EXPECT_EQ(launch_hash, RequiredText(launch_input, "source_sha256"));
  std::cerr << "actual Mock controller launch input="
            << content_identity::canonical_json(launch_input) << '\n';
  DaemonDirectory directory(true);
  const auto fixture_root =
      std::filesystem::path(MANYFORGE_HEADLESS_TEST_MANAGER_ROS_APPLY_FIXTURE);
  std::ifstream metadata_stream(fixture_root / "metadata.json",
                                std::ios::binary);
  ASSERT_TRUE(metadata_stream.good());
  const std::string metadata_bytes{
      std::istreambuf_iterator<char>(metadata_stream),
      std::istreambuf_iterator<char>()};
  const auto metadata =
      deployment::parse_strict_json_model_yaml(metadata_bytes).root();
  ASSERT_TRUE(metadata.is_object());
  ASSERT_EQ(metadata.as_object().size(), 9U);
  ASSERT_EQ(content_identity::canonical_json(metadata), metadata_bytes);
  const auto cell = RequiredText(metadata, "cell_id");
  const auto component = RequiredText(metadata, "component_id");
  const auto candidate_id =
      RequiredText(metadata, "candidate_retained_bundle_id");
  const auto predecessor_id =
      RequiredText(metadata, "predecessor_retained_bundle_id");
  std::filesystem::copy(fixture_root / "activation",
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  // The package and daemon activation repositories are distinct capabilities.
  // Reuse the genuine stored product exactly as the gripper-only fixture does.
  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared_activation = package_activations.get_verified(
      core::ContentDigest(RequiredText(metadata, "prepared_activation_id")));
  ASSERT_NE(prepared_activation, nullptr);
  const deployment::FilesystemPreparedActivationRepository daemon_activations(
      directory.activation_root());
  ASSERT_TRUE(daemon_activations.put_if_absent(*prepared_activation));
  // Reuse the real installed-target loader and Program runner with only the
  // selected ROS executable and its actual shared candidate worker.
  const auto installed_manager = PrepareManagerApplyInstalledTargets(
      directory, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST,
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER,
      "manyforge_ros2_control_execution_host");
  deployment::FilesystemVerifiedRetainedBundleSource bundles(
      directory.apply_package_root());
  const auto predecessor =
      bundles.get_verified(core::RetainedBundleId(predecessor_id));
  const auto candidate =
      bundles.get_verified(core::RetainedBundleId(candidate_id));
  ASSERT_NE(predecessor, nullptr);
  ASSERT_NE(candidate, nullptr);
  ASSERT_TRUE(predecessor->value().cell_graph.controllable_components.empty());
  ASSERT_EQ(candidate->value().cell_graph.controllable_components.size(), 1U);
  EXPECT_EQ(candidate->value()
                .cell_graph.controllable_components.front()
                .controllable_component_id.value(),
            component);
  for (const auto *bundle : {predecessor.get(), candidate.get()}) {
    ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
    EXPECT_EQ(bundle->value()
                  .cell_graph.runtime_units.front()
                  .runtime_profile_id.value(),
              "manyforge.ros2_control_execution_runtime");
  }
  ProgramRunnerProcess program_runner(directory, cell);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         RequiredText(metadata, "admission_filename"),
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  AtomicSourceSnapshotChannel runtime_source(directory.publication_root(),
                                             "scene-runtime-source.json");
  const auto before = manager_source.Read();
  const auto scene_before = scene_source.Read();
  const auto runtime_before = runtime_source.Read();
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(scene_before.has_value());
  ASSERT_TRUE(runtime_before.has_value());
  EXPECT_EQ(RequiredText(RequiredMember(before->payload, "active"),
                         "retained_bundle_id"),
            predecessor_id);
  const auto cclc_command = [&](const std::string &command,
                                CanonicalValue payload) {
    const auto bytes = SendCommand(
        directory.execution_run_root() / "cclc-command.sock",
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue("manyforge.cclc_open_request.v1")},
            {"command", CanonicalValue(command)},
            {"payload", std::move(payload)}})));
    const auto response =
        deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual CCLC read failed: " + bytes);
    return RequiredMember(response, "result");
  };
  const auto original_owner = RequiredText(
      cclc_command("owner_identity", CanonicalValue(CanonicalValue::Object{})),
      "request_owner_epoch");
  ASSERT_FALSE(original_owner.empty());
  const auto request =
      ApplyRequest(directory, before->source_owner_epoch,
                   WaitForAuthoringAdmission(directory).admission_generation,
                   RequiredText(metadata, "deployment_artifact_id"),
                   RequiredText(metadata, "deployment_revision"),
                   RequiredText(metadata, "predecessor_graph_revision"),
                   predecessor_id, "empty-ros-one-mock.1", {}, false, "receipt",
                   RequiredText(metadata, "prepared_activation_id"));
  const auto submitted = SendCommand(directory.socket_path(), request);
  const auto submission =
      deployment::parse_strict_json_model_yaml(submitted).root();
  ASSERT_EQ(RequiredMember(submission, "ok"), CanonicalValue(true))
      << submitted;

  // Reuse the existing process suite's bounded observation interval. A guard,
  // failure or unknown outcome never qualifies as a successful first Apply.
  const auto apply_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::string last;
  bool succeeded = false;
  while (std::chrono::steady_clock::now() < apply_deadline) {
    const auto status = manager_source.Read();
    if (status.has_value()) {
      last = content_identity::canonical_json(status->payload);
      EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(status->payload)) << last;
      if (last.find("\"request_id\":\"empty-ros-one-mock.1\"") !=
              std::string::npos &&
          last.find("\"terminal_state\":\"succeeded\"") != std::string::npos) {
        succeeded = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(succeeded) << last;
  component_manager::SqliteManagerApplyJournal journal(
      directory.manager_root() /
          std::string(component_manager::kManagerApplyJournalFilename),
      core::CellId(cell));
  const auto inventory = journal.ReadStatusInventory();
  ASSERT_EQ(inventory.size(), 1U);
  const auto &status = inventory.front();
  EXPECT_FALSE(status.bring_up_plan.has_value());
  EXPECT_TRUE(status.bring_up_child_proofs.empty());
  ASSERT_TRUE(status.fence_released.has_value());
  ASSERT_TRUE(status.publication_receipt.has_value());

  const auto read_component = [&] {
    return cclc_command(
        "component_snapshot",
        CanonicalValue(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue(component)}}));
  };
  auto snapshot = read_component();
  EXPECT_EQ(RequiredText(snapshot, "retained_bundle_id"), candidate_id);
  EXPECT_EQ(RequiredText(snapshot, "graph_revision"),
            candidate->value().cell_graph.graph_revision.value());
  EXPECT_EQ(RequiredText(snapshot, "component_lifecycle_state"), "declared");
  EXPECT_EQ(RequiredMember(snapshot, "lifecycle_target"),
            CanonicalValue(nullptr));
  EXPECT_EQ(RequiredText(cclc_command("owner_identity",
                                      CanonicalValue(CanonicalValue::Object{})),
                         "request_owner_epoch"),
            original_owner);

  const auto observe_ros = [&](const std::string &state, const std::string &label) {
    const auto output = directory.execution_run_root() / (label + ".ros-observation.json");
    const auto log = directory.execution_run_root() / (label + ".ros-observation.log");
    const auto execution_root = directory.execution_run_root().string();
    const auto helper = std::filesystem::path(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT) /
                        "manyforge_deployment/test/prepare_activation_tool.py";
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("ROS observation fork failed");
    if (child == 0) {
      const auto fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) _exit(126);
      ::close(fd);
      ::execl(MANYFORGE_HEADLESS_TEST_PYTHON, MANYFORGE_HEADLESS_TEST_PYTHON,
              helper.c_str(), "--observe-manager-ros", "--execution-root", execution_root.c_str(),
              "--component", component.c_str(), "--state", state.c_str(), "--label", label.c_str(),
              "--output", output.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int result = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto waited = ::waitpid(child, &result, WNOHANG);
      if (waited == child) {
        if (!WIFEXITED(result) || WEXITSTATUS(result) != 0)
          throw std::runtime_error("Actual ROS observation failed: " + read_file(log));
        std::cerr << "actual ROS observation=" << read_file(output) << '\n';
        return;
      }
      if (waited < 0 && errno != EINTR)
        throw std::runtime_error("ROS observation wait lost child ownership: " + read_file(log));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(child, SIGKILL);
    while (::waitpid(child, &result, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("Actual ROS observation timed out: " + read_file(log));
  };
  observe_ros("inactive", "after-apply");

  // This is the installed Kernel's actual per-component command gate, not
  // Manager convergence or process presence. Reading it never dispatches
  // motion.
  const auto read_command_gate = [&] {
    const auto bytes = SendCommand(
        directory.execution_socket_path(),
        R"({"api_version":"manyforge.execution_command.v1","command":"targets"})");
    const auto response =
        deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual execution targets read failed: " +
                               bytes);
    const auto &components =
        RequiredMember(RequiredMember(response, "targets"), "components")
            .as_array();
    if (components.size() != 1U ||
        RequiredText(components.front(), "controllable_component_id") !=
            component)
      throw std::runtime_error("Actual execution target membership mismatch: " +
                               bytes);
    return RequiredMember(components.front(), "availability");
  };
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"),
            CanonicalValue(false));

  // Produce the same intrinsic manual OperationIntent as the existing native
  // process test. Every identity, binding, role, axis and goal comes from the
  // installed owner's actual status; no Program-node/route authority is minted.
  const auto submit_current_joint_hold = [&](const std::string &request_id) {
    const auto bytes = SendCommand(directory.execution_socket_path(),
        R"({"api_version":"manyforge.execution_command.v1","command":"status"})");
    const auto response = deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual motion producer status unavailable: " + bytes);
    const auto &actual = RequiredMember(response, "status");
    const auto &identity = RequiredMember(actual, "identity");
    const auto &components = RequiredMember(RequiredMember(actual, "targets"), "components").as_array();
    if (components.size() != 1U || RequiredText(components.front(), "controllable_component_id") != component)
      throw std::runtime_error("Actual motion producer component closure differs");
    const auto &targets = RequiredMember(components.front(), "targets").as_array();
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
      return RequiredText(row, "presentation") == "motion";
    });
    if (target == targets.end()) throw std::runtime_error("Actual motion target is absent");
    const auto &command = RequiredMember(*target, "command");
    const auto &manual = RequiredMember(command, "manual_control");
    const auto &observed_targets = RequiredMember(RequiredMember(actual, "control_state"), "observed_targets").as_array();
    const auto observed_row = std::find_if(observed_targets.begin(), observed_targets.end(), [&](const auto &row) {
      return RequiredText(row, "controllable_component_id") == component &&
          RequiredText(row, "command_target_id") == RequiredText(*target, "command_target_id") &&
          RequiredText(row, "operation_capability_id") == RequiredText(*target, "operation_capability_id");
    });
    if (observed_row == observed_targets.end()) throw std::runtime_error("Actual motion observation is absent");
    const auto &observed = RequiredMember(*observed_row, "observed");
    if (!observed.is_object() || RequiredMember(observed, "complete") != CanonicalValue(true) ||
        RequiredText(observed, "source_epoch").empty() || RequiredMember(observed, "sequence").as_number() <= 0.0)
      throw std::runtime_error("Actual motion observation is incomplete");
    CanonicalValue::Array joint_names;
    for (const auto &joint : RequiredMember(command, "joints").as_array())
      joint_names.emplace_back(RequiredMember(joint, "source_joint_name"));
    if (CanonicalValue(joint_names) != RequiredMember(observed, "source_joint_names") ||
        joint_names.size() != RequiredMember(observed, "positions_rad").as_array().size())
      throw std::runtime_error("Actual motion axes differ from the compiled direct target");
    CanonicalValue payload(CanonicalValue::Object{
        {"kind", CanonicalValue("motion")}, {"move_id", CanonicalValue(request_id + ".hold")},
        {"base_frame_role_id", RequiredMember(command, "base_frame_role_id")},
        {"tcp_frame_role_id", RequiredMember(command, "tcp_frame_role_id")},
        {"tolerances", CanonicalValue(CanonicalValue::Object{
            {"joint_position_tol_rad", CanonicalValue(0.01)}, {"translation_tol_m", CanonicalValue(0.001)},
            {"rotation_tol_rad", CanonicalValue(0.01)}})},
        {"speed_profile", CanonicalValue(CanonicalValue::Object{
            {"velocity_scale", CanonicalValue(0.5)}, {"acceleration_scale", CanonicalValue(0.5)},
            {"max_tcp_linear_speed_mps", CanonicalValue(nullptr)}})},
        {"timeouts", CanonicalValue(CanonicalValue::Object{
            {"planning_timeout_s", CanonicalValue(2.0)}, {"execution_timeout_s", CanonicalValue(10.0)}})},
        {"cache_mode", CanonicalValue("default")}, {"planning_profile", CanonicalValue("first_feasible")},
        {"target_mode", CanonicalValue("absolute")}, {"target_reference", CanonicalValue(nullptr)},
        {"offset_reference", CanonicalValue(nullptr)},
        {"motion", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("joint_goal")}, {"joint_names", CanonicalValue(std::move(joint_names))},
            {"positions_rad", RequiredMember(observed, "positions_rad")}})},
        {"contact_policy_id", CanonicalValue(nullptr)},
        {"per_joint_dynamics_limits", CanonicalValue(CanonicalValue::Object{})},
        {"motion_interrupt_scopes", CanonicalValue(CanonicalValue::Array{})},
        {"runtime_object_bindings", CanonicalValue(CanonicalValue::Array{})}});
    CanonicalValue operation(CanonicalValue::Object{
        {"request_context", CanonicalValue(CanonicalValue::Object{
            {"request_id", CanonicalValue(request_id)}, {"request_owner_epoch", RequiredMember(identity, "owner_epoch")},
            {"source_label", CanonicalValue("manager_ros_apply_fixture")}, {"correlation_id", CanonicalValue(nullptr)}})},
        {"program_id", RequiredMember(manual, "program_id")},
        {"program_revision", RequiredMember(manual, "program_revision")},
        {"program_binding_id", RequiredMember(manual, "program_binding_id")},
        {"program_run_id", RequiredMember(manual, "program_run_id")},
        {"expected_graph_revision", RequiredMember(identity, "graph_revision")},
        {"expected_retained_bundle_id", RequiredMember(identity, "retained_bundle_id")},
        {"expected_scene_revision", RequiredMember(identity, "scene_revision")},
        {"participants", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"component_role_id", RequiredMember(command, "component_role_id")},
            {"target_role_id", RequiredMember(command, "target_role_id")},
            {"required_capability_id", RequiredMember(*target, "operation_capability_id")},
            {"action_span", CanonicalValue(nullptr)}, {"payload", std::move(payload)}})})},
        {"coordination_policy", CanonicalValue("independent")}});
    const auto submitted_motion = SendCommand(directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue("manyforge.execution_command.v1")},
            {"command", CanonicalValue("submit")}, {"operation", std::move(operation)},
            {"attribution", CanonicalValue(CanonicalValue::Object{
                {"assurance", CanonicalValue("unverified")}, {"principal_id", CanonicalValue(nullptr)},
                {"transport", CanonicalValue("http")}})}})));
    return deployment::parse_strict_json_model_yaml(submitted_motion).root();
  };

  const auto explicit_lifecycle = [&](const std::string &intent) {
    const auto current = read_component();
    CanonicalValue::Array expected_domains;
    for (const auto &domain : RequiredMember(current, "domains").as_array()) {
      const auto &active =
          RequiredMember(domain, "active_control_configuration");
      CanonicalValue expected_active(
          CanonicalValue::Object{{"presence", CanonicalValue("absent")}});
      if (active.is_object()) {
        expected_active = CanonicalValue(CanonicalValue::Object{
            {"presence", CanonicalValue("present")},
            {"active_control_configuration_id",
             RequiredMember(active, "active_control_configuration_id")},
            {"active_control_configuration_generation",
             RequiredMember(active,
                            "active_control_configuration_generation")}});
      }
      expected_domains.emplace_back(CanonicalValue::Object{
          {"control_domain_id", RequiredMember(domain, "control_domain_id")},
          {"control_domain_revision",
           RequiredMember(domain, "control_domain_revision")},
          {"controller_topology_id",
           RequiredMember(domain, "controller_topology_id")},
          {"topology_revision", RequiredMember(domain, "topology_revision")},
          {"expected_active_control_configuration", std::move(expected_active)},
          {"expected_control_bindings",
           RequiredMember(domain, "current_control_bindings")}});
    }
    const auto result = cclc_command(
        "lifecycle_submit",
        CanonicalValue(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue(component)},
            {"intent", CanonicalValue(intent)},
            {"expected_graph_revision",
             RequiredMember(current, "graph_revision")},
            {"expected_component_lifecycle_generation",
             RequiredMember(current, "component_lifecycle_generation")},
            {"expected_control_domains",
             CanonicalValue(std::move(expected_domains))},
            {"expected_admission_generation",
             CanonicalValue(static_cast<double>(
                 WaitForAuthoringAdmission(directory).admission_generation))},
            {"request_context",
             CanonicalValue(CanonicalValue::Object{
                 {"request_id", CanonicalValue("empty-ros-one-mock." + intent)},
                 {"request_owner_epoch", CanonicalValue(original_owner)},
                 {"source_label",
                  CanonicalValue("manager_ros_apply_fixture")}})}}));
    const auto &acceptance = RequiredMember(result, "acceptance");
    if (!acceptance.is_object())
      throw std::runtime_error("Explicit lifecycle was not accepted: " +
                               content_identity::canonical_json(result));
    const auto transaction_id =
        RequiredText(acceptance, "lifecycle_transaction_id");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    CanonicalValue actual(nullptr);
    while (std::chrono::steady_clock::now() < deadline) {
      actual = cclc_command(
          "lifecycle_status",
          CanonicalValue(CanonicalValue::Object{
              {"lifecycle_transaction_id", CanonicalValue(transaction_id)}}));
      if (RequiredMember(actual, "terminal_result") != CanonicalValue(nullptr))
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (RequiredMember(actual, "terminal_result") !=
        CanonicalValue("succeeded"))
      throw std::runtime_error("Explicit lifecycle did not succeed: " +
                               content_identity::canonical_json(actual));
    EXPECT_EQ(RequiredText(actual, "status_owner_epoch"), original_owner);
    EXPECT_EQ(RequiredMember(actual, "acceptance"), acceptance);
    return actual;
  };
  const auto prepared = explicit_lifecycle("prepare");
  EXPECT_EQ(RequiredText(prepared, "current_state"), "prepared");
  // The whole authored component includes its gripper domain. An already
  // active gripper must not be hidden by the aggregate component state.
  const auto &prepared_domains = RequiredMember(prepared, "domains").as_array();
  ASSERT_EQ(prepared_domains.size(),
            candidate->value().desired_control_configurations.size());
  for (const auto &domain : prepared_domains) {
    ASSERT_EQ(RequiredText(domain, "current_state"), "prepared");
    EXPECT_TRUE(
        std::any_of(candidate->value().desired_control_configurations.begin(),
                    candidate->value().desired_control_configurations.end(),
                    [&](const auto &desired) {
                      return desired.control_domain_id.value() ==
                             RequiredText(domain, "control_domain_id");
                    }));
  }
  snapshot = read_component();
  EXPECT_EQ(RequiredText(snapshot, "component_lifecycle_state"), "prepared");
  EXPECT_EQ(RequiredText(RequiredMember(snapshot, "lifecycle_target"),
                         "requested_state"),
            "prepared");
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"),
            CanonicalValue(false));

  const auto refused_motion = submit_current_joint_hold("empty-ros-one-mock.prepared-motion");
  ASSERT_EQ(RequiredMember(refused_motion, "ok"), CanonicalValue(false))
      << content_identity::canonical_json(refused_motion);
  EXPECT_EQ(RequiredText(RequiredMember(RequiredMember(refused_motion, "error"), "details"), "reason"),
            "component_command_not_eligible") << content_identity::canonical_json(refused_motion);

  const auto enabled = explicit_lifecycle("enable");
  EXPECT_EQ(RequiredText(enabled, "current_state"), "active");
  observe_ros("active", "after-enable");
  snapshot = read_component();
  EXPECT_EQ(RequiredText(snapshot, "component_lifecycle_state"), "active");
  EXPECT_EQ(RequiredText(RequiredMember(snapshot, "lifecycle_target"),
                         "requested_state"),
            "active");
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  CanonicalValue command_gate(nullptr);
  do {
    command_gate = read_command_gate();
    if (RequiredMember(command_gate, "command_eligible") ==
        CanonicalValue(true))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < ready_deadline);
  EXPECT_EQ(RequiredMember(command_gate, "command_eligible"),
            CanonicalValue(true));
  // These private commands require the genuine managed cell above. The
  // standalone CLI cannot construct this stable-cell/CCLC owner relationship.
  const auto execution_command = [&](CanonicalValue::Object request) {
    const auto bytes = SendCommand(directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(std::move(request))));
    return deployment::parse_strict_json_model_yaml(bytes).root();
  };
  const CanonicalValue::Object capability_request{
      {"api_version", CanonicalValue("manyforge.execution_command.v1")},
      {"command", CanonicalValue("reset_robot_position_capability")}};
  const auto capability = execution_command(capability_request);
  ASSERT_EQ(capability.as_object().size(), 5U);
  ASSERT_EQ(RequiredMember(capability, "ok"), CanonicalValue(true));
  EXPECT_EQ(RequiredText(capability, "api_version"), "manyforge.execution_command_response.v1");
  EXPECT_EQ(RequiredText(capability, "command"), "reset_robot_position_capability");
  const auto &position_capability = RequiredMember(capability, "reset_robot_position");
  ASSERT_EQ(position_capability.as_object().size(), 2U);
  EXPECT_EQ(RequiredMember(position_capability, "available"), CanonicalValue(true));
  EXPECT_EQ(RequiredMember(position_capability, "reason"), CanonicalValue(nullptr));
  const auto &position_identity = RequiredMember(capability, "identity");
  EXPECT_EQ(RequiredText(position_identity, "schema_version"), "manyforge.execution_identity.v2");
  const CanonicalValue::Object reset_authorization{
      {"program_runner_owner_epoch", CanonicalValue("absent.runner")},
      {"cycle_generation", CanonicalValue(1.0)},
      {"request_owner_epoch", CanonicalValue("absent.request-owner")},
      {"request_id", CanonicalValue("absent.reset")},
      {"request_payload_digest", CanonicalValue("sha256:" + std::string(64U, '1'))},
      {"program_run_id", CanonicalValue(nullptr)},
      {"cycle_claim_generation", CanonicalValue(nullptr)},
      {"terminal_or_settlement_reference", CanonicalValue(nullptr)},
      {"reset_claim_generation", CanonicalValue(1.0)}};
  CanonicalValue::Object expected_deployment;
  for (const auto *key : {"owner_epoch", "active_publication_generation", "retained_bundle_id", "graph_revision"})
    expected_deployment.emplace_back(key, RequiredMember(position_identity, key));
  const auto reset_command = [](CanonicalValue::Object authorization, CanonicalValue::Object expected) {
    return CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.execution_command.v1")},
        {"command", CanonicalValue("reset_robot_position")},
        {"authorization", CanonicalValue(std::move(authorization))},
        {"expected_deployment_identity", CanonicalValue(std::move(expected))}};
  };
  const auto refused_reset = execution_command(reset_command(reset_authorization, expected_deployment));
  ASSERT_EQ(RequiredMember(refused_reset, "ok"), CanonicalValue(false));
  const auto &reset_error = RequiredMember(refused_reset, "error");
  EXPECT_EQ(RequiredText(reset_error, "code"), "authoring_not_ready");
  const auto &reset_details = RequiredMember(reset_error, "details");
  EXPECT_EQ(RequiredText(reset_details, "reason"), "program_reset_claim_not_exact");
  EXPECT_EQ(RequiredText(reset_details, "effect_disposition"), "known_no_effect");
  std::vector<CanonicalValue::Object> malformed_reset_commands;
  auto extra_authorization = reset_authorization;
  extra_authorization.emplace_back("trusted", CanonicalValue(true));
  malformed_reset_commands.push_back(reset_command(std::move(extra_authorization), expected_deployment));
  auto caller_targets = expected_deployment;
  caller_targets.emplace_back("joint_positions_rad", CanonicalValue(CanonicalValue::Array{CanonicalValue(0.0)}));
  malformed_reset_commands.push_back(reset_command(reset_authorization, std::move(caller_targets)));
  for (const auto *field : {"cycle_claim_generation", "reset_claim_generation"}) {
    auto wrong_claim = reset_authorization;
    for (auto &[key, value] : wrong_claim)
      if (key == field)
        value = key == "cycle_claim_generation" ? CanonicalValue(1.0) : CanonicalValue(nullptr);
    malformed_reset_commands.push_back(reset_command(std::move(wrong_claim), expected_deployment));
  }
  auto extra_capability = capability_request;
  extra_capability.emplace_back("unexpected", CanonicalValue(true));
  malformed_reset_commands.push_back(std::move(extra_capability));
  for (auto &request : malformed_reset_commands) {
    const auto invalid = execution_command(std::move(request));
    ASSERT_EQ(RequiredMember(invalid, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(invalid, "error"), "code"), "execution_command_invalid");
  }
  EXPECT_EQ(RequiredText(cclc_command("owner_identity",
                                      CanonicalValue(CanonicalValue::Object{})),
                         "request_owner_epoch"),
            original_owner);
  const auto &domains = RequiredMember(snapshot, "domains").as_array();
  ASSERT_EQ(domains.size(),
            candidate->value().desired_control_configurations.size());
  const auto &enabled_domains = RequiredMember(enabled, "domains").as_array();
  ASSERT_EQ(enabled_domains.size(), domains.size());
  for (const auto &desired :
       candidate->value().desired_control_configurations) {
    const auto domain =
        std::find_if(domains.begin(), domains.end(), [&](const auto &row) {
          return RequiredText(row, "control_domain_id") ==
                 desired.control_domain_id.value();
        });
    ASSERT_NE(domain, domains.end());
    // Lifecycle state belongs to the actual Enable transaction status;
    // component snapshot domains contain the current route/configuration closure.
    const auto enabled_domain = std::find_if(
        enabled_domains.begin(), enabled_domains.end(), [&](const auto &row) {
          return RequiredText(row, "control_domain_id") ==
                 desired.control_domain_id.value();
        });
    ASSERT_NE(enabled_domain, enabled_domains.end());
    EXPECT_EQ(RequiredText(*enabled_domain, "current_state"), "active");
    const auto &configuration =
        RequiredMember(*domain, "active_control_configuration");
    ASSERT_TRUE(configuration.is_object());
    EXPECT_EQ(RequiredText(configuration, "state"), "reconciled");
    EXPECT_GT(
        RequiredMember(configuration, "active_control_configuration_generation")
            .as_number(),
        0.0);
    EXPECT_FALSE(RequiredText(configuration, "source_runtime_epoch").empty());
    EXPECT_EQ(RequiredText(configuration, "control_domain_revision"),
              desired.control_domain_revision.value());
    EXPECT_EQ(RequiredText(configuration, "activation_set_id"),
              desired.activation_set_id.value());
    const auto &routes =
        RequiredMember(configuration, "binding_routes").as_array();
    ASSERT_EQ(routes.size(), desired.binding_routes.size());
    for (const auto &route : desired.binding_routes) {
      EXPECT_TRUE(
          std::any_of(routes.begin(), routes.end(), [&](const auto &row) {
            return RequiredText(row, "control_binding_id") ==
                       route.control_binding_id.value() &&
                   RequiredText(row, "control_route_id") ==
                       route.control_route_id.value() &&
                   RequiredText(row, "control_binding_revision") ==
                       route.control_binding_revision.value();
          }));
    }
  }
  EXPECT_FALSE(
      RequiredMember(snapshot, "provider_occurrences").as_array().empty());
  // Submit, rather than preview, consumes the real current route/readiness,
  // ordinary manual Cell claim and configured resource admission gate.
  static_cast<void>(WaitForAuthoringAdmission(directory));
  const auto accepted_motion = submit_current_joint_hold("empty-ros-one-mock.active-motion");
  ASSERT_EQ(RequiredMember(accepted_motion, "ok"), CanonicalValue(true))
      << content_identity::canonical_json(accepted_motion);
  const auto operation_id = RequiredText(RequiredMember(accepted_motion, "accepted"), "operation_id");
  const auto motion_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
  CanonicalValue actual_motion(nullptr);
  while (std::chrono::steady_clock::now() < motion_deadline) {
    const auto bytes = SendCommand(directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue("manyforge.execution_command.v1")},
            {"command", CanonicalValue("read")}, {"operation_id", CanonicalValue(operation_id)}})));
    const auto actual = deployment::parse_strict_json_model_yaml(bytes).root();
    ASSERT_EQ(RequiredMember(actual, "ok"), CanonicalValue(true)) << bytes;
    actual_motion = RequiredMember(actual, "operation");
    const auto state = RequiredText(actual_motion, "state");
    if (state == "completed" || state == "failed" || state == "intervention_required" ||
        state == "stopped" || state == "cancelled_pre_dispatch") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(actual_motion.is_object());
  ASSERT_EQ(RequiredText(actual_motion, "state"), "completed")
      << content_identity::canonical_json(actual_motion);
  const auto &motion_participants = RequiredMember(actual_motion, "participants").as_array();
  ASSERT_EQ(motion_participants.size(), 1U);
  const auto &motion_participant = motion_participants.front();
  EXPECT_EQ(RequiredText(motion_participant, "controllable_component_id"), component);
  EXPECT_EQ(RequiredText(motion_participant, "state"), "completed");
  EXPECT_EQ(RequiredMember(motion_participant, "terminal_error"), CanonicalValue(nullptr));
  const auto arm = std::find_if(candidate->value().cell_graph.control_bindings.begin(),
      candidate->value().cell_graph.control_bindings.end(), [](const auto &binding) {
        return binding.capability_id.value() == "execution.trajectory.execute";
      });
  ASSERT_NE(arm, candidate->value().cell_graph.control_bindings.end());
  EXPECT_EQ(RequiredText(motion_participant, "control_binding_id"), arm->control_binding_id.value());
  EXPECT_EQ(RequiredText(motion_participant, "control_domain_id"), arm->control_domain_id.value());
  const auto execution = WaitForExecutionReady(directory, candidate_id);
  EXPECT_FALSE(execution.owner_epoch.empty());
  const auto scene_after = scene_source.Read();
  const auto runtime_after = runtime_source.Read();
  ASSERT_TRUE(scene_after.has_value());
  ASSERT_TRUE(runtime_after.has_value());
  EXPECT_NE(
      content_identity::canonical_json(scene_after->payload).find(candidate_id),
      std::string::npos);
  EXPECT_NE(content_identity::canonical_json(runtime_after->payload)
                .find(candidate_id),
            std::string::npos);
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "objects"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "objects"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "carried_object_attachments"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "carried_object_attachments"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "revision"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "revision"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "access"),
                           "projection_leases_open"),
            CanonicalValue(true));
  EXPECT_EQ(RequiredText(
                RequiredMember(RequiredMember(scene_after->payload, "access"),
                               "active_projection"),
                "retained_bundle_id"),
            candidate_id);
  static_cast<void>(explicit_lifecycle("disable"));
  observe_ros("inactive", "after-disable");
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"), CanonicalValue(false));

  // Exact config replay does not introduce activation or child work.
  EXPECT_NE(SendCommand(directory.socket_path(), request)
                .find("\"replay_status\":\"replayed_exact\""),
            std::string::npos);
  const auto replay_inventory = journal.ReadStatusInventory();
  ASSERT_EQ(replay_inventory.size(), 1U);
  EXPECT_FALSE(replay_inventory.front().bring_up_plan.has_value());
  EXPECT_TRUE(replay_inventory.front().bring_up_child_proofs.empty());

  // Let the actual Manager child owner settle normal runtime shutdown before
  // the successful fixture removes the controller launch artifact directory.
  const auto stopped = manager.Stop(SIGTERM);
  ASSERT_TRUE(WIFEXITED(stopped));
  EXPECT_EQ(WEXITSTATUS(stopped), 0);
  EXPECT_FALSE(std::filesystem::exists(directory.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(directory.execution_socket_path()));
#endif
}

} // namespace
} // namespace manyforge::headless

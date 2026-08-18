#include <gtest/gtest.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

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

#if defined(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST)
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

struct ManagerApplyFixtureMetadata final {
  std::string admission_filename;
  std::string candidate_retained_bundle_id;
  std::string cell_id;
  std::string deployment_artifact_id;
  std::string deployment_revision;
  std::string physical_candidate_retained_bundle_id;
  std::string physical_deployment_artifact_id;
  std::string physical_deployment_revision;
  std::string predecessor_graph_revision;
  std::string predecessor_retained_bundle_id;
};

ManagerApplyFixtureMetadata ReadManagerApplyFixtureMetadata() {
  std::ifstream stream(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA,
                       std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Manager Apply fixture metadata is absent");
  }
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  const auto root = deployment::parse_strict_json_model_yaml(bytes).root();
  if (!root.is_object() || root.as_object().size() != 10U ||
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
      RequiredText(root, "predecessor_graph_revision"),
      RequiredText(root, "predecessor_retained_bundle_id"),
  };
}

#endif

class DaemonDirectory final {
public:
  DaemonDirectory() {
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
    std::filesystem::create_directory(runtime_root());
    std::filesystem::create_directory(publication_root());
  }

  ~DaemonDirectory() {
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path manager_root() const {
    return root_ / "manager";
  }
  [[nodiscard]] std::filesystem::path runtime_root() const {
    return root_ / "runtime";
  }
  [[nodiscard]] std::filesystem::path publication_root() const {
    return root_ / "publications";
  }
  [[nodiscard]] std::filesystem::path scene_database() const {
    return root_ / "logical-scene.sqlite3";
  }
  [[nodiscard]] std::filesystem::path socket_path() const {
    return manager_root() / "manager-command.sock";
  }
  [[nodiscard]] std::filesystem::path execution_socket_path() const {
    return runtime_root() / "execution-command.sock";
  }
  [[nodiscard]] std::filesystem::path installed_root() const {
    return root_ / "installed";
  }
  [[nodiscard]] std::filesystem::path apply_package_root() const {
    return root_ / "apply-package";
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path root_;
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
      std::string executable = MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON) {
    const auto manager_root = directory.manager_root().string();
    const auto runtime_root = directory.runtime_root().string();
    const auto scene_database = directory.scene_database().string();
    const auto publication_root = directory.publication_root().string();
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("Manager daemon test fork failed");
    }
    if (pid_ == 0) {
      ::execl(executable.c_str(), executable.c_str(), "--package-root",
              package_root.c_str(), "--canonical-empty-admission-filename",
              admission_filename.c_str(), "--manager-state-root",
              manager_root.c_str(), "--runtime-state-root",
              runtime_root.c_str(), "--logical-scene-database",
              scene_database.c_str(), "--publication-root",
              publication_root.c_str(), "--reconciliation-period-ms", "10",
              static_cast<char *>(nullptr));
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
        throw std::runtime_error("Manager daemon exited before readiness");
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

#if defined(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST)
std::filesystem::path
PrepareManagerApplyInstalledTargets(const DaemonDirectory &directory) {
  std::filesystem::create_directory(directory.installed_root());
  const auto manager = directory.installed_root() / "manyforge_manager_daemon";
  const auto native_host =
      directory.installed_root() / "manyforge_native_execution_host";
  const auto candidate_worker =
      directory.installed_root() / "manyforge_roboplan_candidate_worker";
  std::filesystem::copy_file(MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON, manager,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST,
                             native_host,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_WORKER,
                             candidate_worker,
                             std::filesystem::copy_options::overwrite_existing);
  constexpr auto protected_executable_permissions =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
      std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
      std::filesystem::perms::others_exec;
  std::filesystem::permissions(manager, protected_executable_permissions,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(native_host, protected_executable_permissions,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(candidate_worker,
                               protected_executable_permissions,
                               std::filesystem::perm_options::replace);
  return manager;
}

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
      if (observation.response.find(
              "\"code\":\"execution_owner_not_ready\"") !=
          std::string::npos) {
        observation.observed_closed = true;
      }
      if (observation.response.find("\"ok\":true") != std::string::npos &&
          observation.response.find(expected_retained_bundle_id) !=
              std::string::npos) {
        constexpr std::string_view kOwnerEpochPrefix = "\"owner_epoch\":\"";
        const auto owner_begin =
            observation.response.find(kOwnerEpochPrefix);
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
  throw std::runtime_error("execution owner did not reopen exact product: " +
                           observation.response);
}

std::vector<pid_t> ReadDirectChildren(const pid_t parent) {
  std::ifstream stream(std::filesystem::path("/proc") /
                       std::to_string(parent) / "task" /
                       std::to_string(parent) / "children");
  std::vector<pid_t> children;
  pid_t child = 0;
  while (stream >> child) {
    if (child > 0) {
      children.push_back(child);
    }
  }
  return children;
}

pid_t WaitForSoleManagedChild(const pid_t manager,
                              const pid_t excluded_child = -1) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto children = ReadDirectChildren(manager);
    if (children.size() == 1U && children.front() != excluded_child &&
        ::kill(children.front(), 0) == 0) {
      return children.front();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Manager did not converge one replacement child");
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
  std::vector<char> response(16U * 1024U + 1U);
  const auto received =
      ::recv(client, response.data(), response.size(), MSG_TRUNC);
  static_cast<void>(::close(client));
  if (received <= 0 || static_cast<std::size_t>(received) >= response.size()) {
    throw std::runtime_error("Manager command response is invalid");
  }
  return {response.data(), static_cast<std::size_t>(received)};
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

std::string ApplyRequest(
    const std::string_view manager_owner_epoch,
    const std::string_view deployment_artifact_id = "candidate",
    const std::string_view deployment_revision = kDeploymentRevision,
    const std::string_view expected_graph_revision = "current-graph",
    const std::string_view expected_retained_bundle_id = "current-bundle",
    const std::string_view request_id = "request-1") {
  return std::string("{\"api_version\":\"manyforge.manager_command.v1\","
                     "\"command\":\"apply\",\"correlation_id\":"
                     "\"correlation-1\",\"deployment_artifact_id\":"
                     "\"") +
         std::string(deployment_artifact_id) + "\",\"deployment_revision\":\"" +
         std::string(deployment_revision) +
         "\",\"expected_current\":{\"graph_revision\":\"" +
         std::string(expected_graph_revision) + "\",\"retained_bundle_id\":\"" +
         std::string(expected_retained_bundle_id) +
         "\"},\"reject_if_affected_busy\":true,\"request_id\":\"" +
         std::string(request_id) +
         "\","
         "\"request_owner_epoch\":\"" +
         std::string(manager_owner_epoch) +
         "\",\"source_label\":\"manager-process-test\"}";
}

std::string SceneAddRequest(
    const SourceSnapshot &scene_source, const std::string_view request_id,
    const double box_x = 0.1,
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
      {"scene_resource_ids",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(std::string(scene_resource_id)),
       })},
  }));
}

std::string ScenePreviewRequest(const SourceSnapshot &scene_source,
                                const std::string_view request_id) {
  using content_identity::CanonicalValue;
  const auto transaction = deployment::parse_strict_json_model_yaml(
                               SceneAddRequest(scene_source, request_id))
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
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v1")},
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
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v1")},
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
                        {"source_stamp_s", CanonicalValue(1.25)},
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

    unavailable_response =
        SendCommand(directory.socket_path(), ApplyRequest(first_manager_epoch));
    EXPECT_EQ(
        SendCommand(directory.socket_path(), ApplyRequest(first_manager_epoch)),
        unavailable_response);
    EXPECT_NE(unavailable_response.find("\"kernel_owner_unavailable\""),
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
    const auto stale_apply =
        SendCommand(directory.socket_path(), ApplyRequest(first_manager_epoch));
    EXPECT_NE(stale_apply.find("\"stale_request_owner_epoch\""),
              std::string::npos);
    const auto replacement_unavailable =
        SendCommand(directory.socket_path(),
                    ApplyRequest(manager_snapshot->source_owner_epoch));
    EXPECT_NE(replacement_unavailable.find("\"kernel_owner_unavailable\""),
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
    EXPECT_LE(observed_at_ns,
              static_cast<double>(monotonic_after_observation));
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
        SceneAddRequest(*initial_scene, "scene-runtime-planning-object.1"));
    EXPECT_NE(added_object.find("\"ok\":true"), std::string::npos)
        << added_object;
    const auto scene_before_latch = scene_source.Read();
    ASSERT_TRUE(scene_before_latch.has_value());
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
    EXPECT_NE(observed_pose.find("\"x\":0"), std::string::npos)
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
                                    0.2, "scene-process-fence-box"));
    EXPECT_NE(advanced_scene.find("\"ok\":true"), std::string::npos)
        << advanced_scene;
    const auto scene_after_advance = scene_source.Read();
    ASSERT_TRUE(scene_after_advance.has_value());
    const auto stale_projection_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_before_latch));
    EXPECT_NE(
        stale_projection_latch.find("scene_runtime_pose_projection_changed"),
        std::string::npos)
        << stale_projection_latch;
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
     SceneCommandCommitsPublishesReplaysAndSurvivesReplacement) {
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
    const auto owner_read = SendCommand(
        directory.socket_path(),
        "{\"api_version\":\"manyforge.manager_command.v1\","
        "\"command\":\"scene_owner_read\"}");
    EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(owner_read.find(initial->source_owner_epoch), std::string::npos);
    const auto preview = SendCommand(
        directory.socket_path(),
        ScenePreviewRequest(*initial, "scene-process-preview.1"));
    EXPECT_NE(preview.find("\"ok\":false"), std::string::npos) << preview;
    EXPECT_NE(preview.find(
                  "\"reason\":\"scene_candidate_operation_unsupported\""),
              std::string::npos)
        << preview;
    const auto after_preview = scene_source.Read();
    ASSERT_TRUE(after_preview.has_value());
    EXPECT_EQ(after_preview->semantic_content_digest,
              initial->semantic_content_digest);
    EXPECT_EQ(after_preview->source_generation, initial->source_generation);
    request = SceneAddRequest(*initial, "scene-process-request.1");
    const auto committed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(committed.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(committed.find("\"replay_status\":\"accepted_new\""),
              std::string::npos);
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos);

    const auto published = scene_source.Read();
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->source_generation, initial->source_generation + 1U);
    EXPECT_NE(content_identity::canonical_json(published->payload)
                  .find("\"scene_resource_id\":\"scene-process-box\""),
              std::string::npos);
    const auto pose = SendCommand(
        directory.socket_path(),
        ScenePoseRequest(*published, "scene-process-box"));
    EXPECT_NE(pose.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(pose.find("\"scene_resource_id\":\"scene-process-box\""),
              std::string::npos);
    EXPECT_NE(pose.find("\"position_m\":{\"x\":0,\"y\":0,\"z\":0.15}"),
              std::string::npos);
    const auto stale_pose = SendCommand(
        directory.socket_path(),
        ScenePoseRequest(*initial, "scene-process-box"));
    EXPECT_NE(stale_pose.find("\"scene_projection_changed\""),
              std::string::npos);
    EXPECT_EQ(stale_pose.find("\"details\":"), std::string::npos)
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

    const auto conflict =
        SendCommand(directory.socket_path(),
                    SceneAddRequest(*initial, "scene-process-request.1", 0.15));
    EXPECT_NE(conflict.find("\"request_id_conflict\""), std::string::npos);
    const auto stale =
        SendCommand(directory.socket_path(),
                    SceneAddRequest(*initial, "scene-process-request.stale"));
    EXPECT_NE(stale.find("\"stale_revision\""), std::string::npos);

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
  EXPECT_NE(replayed.find("\"ok\":true"), std::string::npos);
  EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos);
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

#if defined(MANYFORGE_HEADLESS_TEST_NATIVE_EXECUTION_HOST)
TEST(ManagerDaemonProcess,
     RuntimeOnlyNamedApplyUsesTheRealManagedFenceAndSurvivesReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
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
  std::string first_manager_epoch;
  std::string request;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto before = manager_source.Read();
    const auto scene_runtime_before = scene_runtime_source.Read();
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(scene_runtime_before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    EXPECT_NE(content_identity::canonical_json(before->payload)
                  .find("\"apply\":{\"mutation_available\":true"),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_before->payload)
                  .find(fixture.predecessor_retained_bundle_id),
              std::string::npos);
    request = ApplyRequest(
        first_manager_epoch, fixture.deployment_artifact_id,
        fixture.deployment_revision, fixture.predecessor_graph_revision,
        fixture.predecessor_retained_bundle_id, "manager-runtime-only-apply.1");

    auto applied = SendCommand(directory.socket_path(), request);
    EXPECT_NE(applied.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(applied.find("\"replay_status\":\"accepted_new\""),
              std::string::npos);
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory.socket_path(), request);
    }
    SCOPED_TRACE(applied);
    EXPECT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos);
    EXPECT_NE(applied.find(fixture.candidate_retained_bundle_id),
              std::string::npos);
    const auto execution_status = wait_execution_response();
    EXPECT_NE(execution_status.find("\"ok\":false"), std::string::npos)
        << execution_status;
    EXPECT_NE(execution_status.find(
                  "\"code\":\"execution_owner_not_ready\""),
              std::string::npos)
        << execution_status;
    EXPECT_NE(execution_status.find("\"details\":{\"owner_epoch\":\"kernel-owner-"),
              std::string::npos)
        << execution_status;
    ASSERT_TRUE(std::filesystem::remove(directory.apply_package_root() /
                                        "maintained.scenario-catalog.yaml"));
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos);

    component_manager::SqliteManagerApplyJournal journal(
        directory.manager_root() /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId(fixture.cell_id));
    const auto inventory = journal.ReadStatusInventory();
    ASSERT_EQ(inventory.size(), 1U);
    ASSERT_TRUE(inventory.front().terminal_outcome.has_value());
    EXPECT_EQ(inventory.front()
                  .terminal_outcome->terminal_outcome
                  .candidate_projection_revision.retained_bundle_id.value(),
              fixture.candidate_retained_bundle_id);

    const auto manager_after = manager_source.Read();
    const auto scene_after = scene_source.Read();
    const auto scene_runtime_after = scene_runtime_source.Read();
    ASSERT_TRUE(manager_after.has_value());
    ASSERT_TRUE(scene_after.has_value());
    ASSERT_TRUE(scene_runtime_after.has_value());
    EXPECT_NE(content_identity::canonical_json(manager_after->payload)
                  .find(fixture.candidate_retained_bundle_id),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_after->payload)
                  .find(fixture.candidate_retained_bundle_id),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_after->payload)
                  .find(fixture.candidate_retained_bundle_id),
              std::string::npos);
    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory, package_root,
                             fixture.admission_filename,
                             installed_manager.string());
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  AtomicSourceSnapshotChannel scene_runtime_source(directory.publication_root(),
                                                   "scene-runtime-source.json");
  const auto recovered = manager_source.Read();
  const auto scene_runtime_recovered = scene_runtime_source.Read();
  ASSERT_TRUE(recovered.has_value());
  ASSERT_TRUE(scene_runtime_recovered.has_value());
  EXPECT_NE(recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(content_identity::canonical_json(recovered->payload)
                .find(fixture.candidate_retained_bundle_id),
            std::string::npos);
  EXPECT_NE(content_identity::canonical_json(scene_runtime_recovered->payload)
                .find(fixture.candidate_retained_bundle_id),
            std::string::npos);
  const auto recovered_execution_status = wait_execution_response();
  EXPECT_NE(recovered_execution_status.find("\"ok\":false"),
            std::string::npos)
      << recovered_execution_status;
  EXPECT_NE(recovered_execution_status.find(
                "\"code\":\"execution_owner_not_ready\""),
            std::string::npos)
      << recovered_execution_status;
  EXPECT_NE(recovered_execution_status.find(
                "\"details\":{\"owner_epoch\":\"kernel-owner-"),
            std::string::npos)
      << recovered_execution_status;
  const auto stale = SendCommand(directory.socket_path(), request);
  EXPECT_NE(stale.find("\"stale_request_owner_epoch\""), std::string::npos);
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

TEST(ManagerDaemonProcess,
     PhysicalCompositionReopensAfterChildAndManagerReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  std::string first_manager_epoch;
  std::string apply_request;
  std::string child_recovered_owner_epoch;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    const auto before = manager_source.Read();
    ASSERT_TRUE(before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    apply_request = ApplyRequest(
        first_manager_epoch, fixture.physical_deployment_artifact_id,
        fixture.physical_deployment_revision,
        fixture.predecessor_graph_revision,
        fixture.predecessor_retained_bundle_id,
        "manager-physical-recovery-apply.1");

    auto applied = SendCommand(directory.socket_path(), apply_request);
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory.socket_path(), apply_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    ASSERT_NE(
        applied.find(fixture.physical_candidate_retained_bundle_id),
        std::string::npos)
        << applied;

    const auto initial = WaitForExecutionReady(
        directory, fixture.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(initial.owner_epoch.empty());
    const auto first_child = WaitForSoleManagedChild(manager.pid());
    ASSERT_EQ(::kill(first_child, SIGKILL), 0);

    const auto recovered = WaitForExecutionReady(
        directory, fixture.physical_candidate_retained_bundle_id);
    EXPECT_TRUE(recovered.observed_closed) << recovered.response;
    EXPECT_NE(recovered.owner_epoch, initial.owner_epoch);
    child_recovered_owner_epoch = recovered.owner_epoch;
    const auto replacement_child =
        WaitForSoleManagedChild(manager.pid(), first_child);
    EXPECT_NE(replacement_child, first_child);

    const auto after = manager_source.Read();
    ASSERT_TRUE(after.has_value());
    EXPECT_NE(content_identity::canonical_json(after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto status = manager.Stop(SIGKILL);
    ASSERT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory, package_root,
                             fixture.admission_filename,
                             installed_manager.string());
  replacement.WaitReady(directory);
  const auto recovered = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_NE(recovered.owner_epoch, child_recovered_owner_epoch);
  EXPECT_NE(recovered.response.find(
                fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto manager_recovered = manager_source.Read();
  ASSERT_TRUE(manager_recovered.has_value());
  EXPECT_NE(manager_recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(content_identity::canonical_json(manager_recovered->payload)
                .find(fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  const auto stale = SendCommand(directory.socket_path(), apply_request);
  EXPECT_NE(stale.find("\"stale_request_owner_epoch\""), std::string::npos);
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
  EXPECT_EQ(WEXITSTATUS(stopped), 0);
}
#endif

} // namespace
} // namespace manyforge::headless

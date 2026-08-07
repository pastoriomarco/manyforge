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
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_content_identity/content_identity.hpp>
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
  explicit ManagerProcess(const DaemonDirectory &directory) {
    const std::string executable = MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON;
    const std::string package_root =
        MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT;
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
              package_root.c_str(), "--manager-state-root",
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

std::string ApplyRequest(const std::string_view manager_owner_epoch) {
  return std::string("{\"api_version\":\"manyforge.manager_command.v1\","
                     "\"command\":\"apply\",\"correlation_id\":"
                     "\"correlation-1\",\"deployment_artifact_id\":"
                     "\"candidate\",\"deployment_revision\":\"") +
         std::string(kDeploymentRevision) +
         "\",\"expected_current\":{\"graph_revision\":\"current-graph\","
         "\"retained_bundle_id\":\"current-bundle\"},"
         "\"reject_if_affected_busy\":true,\"request_id\":\"request-1\","
         "\"request_owner_epoch\":\"" +
         std::string(manager_owner_epoch) +
         "\",\"source_label\":\"manager-process-test\"}";
}

std::string SceneAddRequest(const SourceSnapshot &scene_source,
                            const std::string_view request_id,
                            const double box_x = 0.1) {
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
                {"scene_resource_id", CanonicalValue("scene-process-box")},
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

} // namespace
} // namespace manyforge::headless

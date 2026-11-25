// Gateway core: owner link, command tables, attribution and scene commit
// rules, against fake SEQPACKET owners in a private temp run root. No sleeps:
// each fake owner serves one connection on its own thread and the test joins
// it after the client call returned. The reply versions and the Manager reply
// bound are checked against the owners' sources in this checkout.

#include <gtest/gtest.h>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>

#include <manyforge_content_identity/canonical_value.hpp>

#include "manyforge_headless/gateway_core.hpp"

namespace {

namespace gw = manyforge::headless::gateway;
namespace ci = manyforge::content_identity;
using ci::CanonicalValue;

constexpr const char *kHostOk =
    R"({"api_version":"manyforge.execution_command_response.v6","command":"identity","ok":true})";

/// A private run root: a fresh 0700 directory removed at the end.
class RunRoot {
 public:
  RunRoot() {
    const char *base = std::getenv("TMPDIR");
    std::string pattern = std::string(base != nullptr ? base : "/tmp") +
                          "/gateway-core-XXXXXX";
    char *made = ::mkdtemp(pattern.data());
    EXPECT_NE(made, nullptr);
    char resolved[PATH_MAX];
    EXPECT_NE(::realpath(made, resolved), nullptr);
    path_ = resolved;
  }
  ~RunRoot() { std::filesystem::remove_all(path_); }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

 private:
  std::filesystem::path path_;
};

/// One listening socket leaf; `Serve` answers one connection on a thread.
class FakeOwner {
 public:
  FakeOwner(const std::filesystem::path &root, const std::string &leaf,
            mode_t mode = 0600, int backlog = 4) {
    listener_ = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    const auto path = (root / leaf).string();
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    EXPECT_EQ(::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                     sizeof(address)),
              0);
    EXPECT_EQ(::chmod(path.c_str(), mode), 0);
    EXPECT_EQ(::listen(listener_, backlog), 0);
  }
  ~FakeOwner() {
    if (server_.joinable()) server_.join();
    ::close(listener_);
  }

  /// Accepts one connection, reads the request, then sends `reply` (or
  /// closes without one when nullopt).
  void Serve(std::optional<std::string> reply) {
    server_ = std::thread([this, reply = std::move(reply)] {
      const int connection = ::accept4(listener_, nullptr, nullptr, SOCK_CLOEXEC);
      if (connection < 0) return;
      std::string buffer(2U * 1024U * 1024U, '\0');
      const auto received = ::recv(connection, buffer.data(), buffer.size(), 0);
      if (received > 0) request_.assign(buffer.data(), static_cast<std::size_t>(received));
      if (reply.has_value()) {
        int size = static_cast<int>(reply->size() + 4096U);
        ::setsockopt(connection, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size));
        ::send(connection, reply->data(), reply->size(), MSG_NOSIGNAL);
      }
      ::close(connection);
    });
  }
  /// The request bytes, after the server thread ended.
  const std::string &Request() {
    if (server_.joinable()) server_.join();
    return request_;
  }

 private:
  int listener_{-1};
  std::thread server_;
  std::string request_;
};

gw::Gateway MakeGateway(const RunRoot &root, gw::AttributionTransport transport) {
  return gw::Gateway(gw::GatewayConfig{root.path(), root.path(), transport,
                                       std::chrono::milliseconds(2000)});
}

std::string CodeOf(const std::function<void()> &call) {
  try {
    call();
  } catch (const gw::GatewayError &error) {
    return error.code();
  }
  return "no error";
}

CanonicalValue Parse(const std::string &text) { return ci::parse_canonical_json(text); }

const CanonicalValue *Member(const CanonicalValue &value, std::string_view name) {
  return value.find_object_member(name);
}

TEST(GatewayTables, NamesAreUniqueAndSceneCommitIsSpecial) {
  std::set<std::string_view> names;
  for (const auto &spec : gw::HostCommands()) EXPECT_TRUE(names.insert(spec.name).second) << spec.name;
  for (const auto &spec : gw::ManagerCommands()) EXPECT_TRUE(names.insert(spec.name).second) << spec.name;
  EXPECT_EQ(gw::FindManagerCommand("scene_commit"), nullptr);
  EXPECT_EQ(gw::FindHostCommand("release_halt")->lane, gw::HostLane::kCommand);
  EXPECT_EQ(gw::FindHostCommand("read_stop")->lane, gw::HostLane::kControl);
  EXPECT_EQ(gw::FindHostCommand("read_component_model")->lane, gw::HostLane::kComponent);
}

TEST(GatewayOwnerLink, CanonicalRoundTrip) {
  RunRoot root;
  FakeOwner owner(root.path(), "execution-command.sock");
  owner.Serve(kHostOk);
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  const auto reply = gateway.Host("identity", {});
  EXPECT_EQ(owner.Request(),
            R"({"api_version":"manyforge.execution_command.v4","command":"identity"})");
  EXPECT_EQ(ci::canonical_json(reply), kHostOk);
}

TEST(GatewayOwnerLink, OwnerRefusalIsAReply) {
  RunRoot root;
  FakeOwner owner(root.path(), "execution-command.sock");
  const std::string refusal =
      R"({"api_version":"manyforge.execution_command_response.v6","error":{"code":"x","details":{},"message":"m","producer":"kernel"},"ok":false})";
  owner.Serve(refusal);
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_EQ(ci::canonical_json(gateway.Host("identity", {})), refusal);
}

TEST(GatewayOwnerLink, NonCanonicalReplyIsRefused) {
  RunRoot root;
  FakeOwner owner(root.path(), "execution-command.sock");
  owner.Serve(R"({"ok":true, "api_version":"manyforge.execution_command_response.v6"})");
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("set_rule_active", {}); }),
            "execution_command_response_invalid");
}

TEST(GatewayOwnerLink, WrongVersionAndNonBooleanOkAreRefused) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  {
    // v5 is the cell-admission reply version, not set_rule_active's.
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v5","ok":true})");
    EXPECT_EQ(CodeOf([&] { (void)gateway.Host("set_rule_active", {}); }),
              "execution_command_response_invalid");
  }
  std::filesystem::remove(root.path() / "execution-command.sock");
  {
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v6","command":"cell_admission_get","ok":true})");
    EXPECT_EQ(CodeOf([&] { (void)gateway.Host("cell_admission_get", {}); }),
              "execution_command_response_invalid");
  }
  std::filesystem::remove(root.path() / "execution-command.sock");
  {
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v6","ok":1})");
    EXPECT_EQ(CodeOf([&] { (void)gateway.Host("set_rule_active", {}); }),
              "execution_command_response_invalid");
  }
  std::filesystem::remove(root.path() / "manager-command.sock");
  {
    FakeOwner owner(root.path(), "manager-command.sock");
    owner.Serve(R"({"api_version":"manyforge.manager_command_response.v2","ok":true})");
    EXPECT_EQ(CodeOf([&] { (void)gateway.Manager("apply_status", {}); }),
              "manager_command_response_invalid");
  }
}

/// A canonical Manager reply of exactly `size` bytes.
std::string ManagerReplyOfSize(std::size_t size) {
  const std::string head =
      R"({"api_version":"manyforge.manager_command_response.v3","ok":true,"pad":")";
  const std::string tail = R"("})";
  return head + std::string(size - head.size() - tail.size(), 'x') + tail;
}

TEST(GatewayOwnerLink, CellAdmissionRepliesAreV5) {
  // The host answers every cell-admission command with its cell-admission
  // codec's v5 envelope, on the command or the component socket.
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  for (const auto &[command, leaf] :
       std::map<std::string, std::string>{
           {"cell_admission_get", "execution-command.sock"},
           {"cell_admission_acquire", "execution-command.sock"},
           {"cell_admission_release", "execution-command.sock"},
           {"cell_admission_recovery_receipt", "execution-command.sock"},
           {"cell_admission_publish_operator_alarm_attention",
            "execution-component.sock"}}) {
    for (const auto *ok : {"true", "false"}) {
      std::filesystem::remove(root.path() / leaf);
      FakeOwner owner(root.path(), leaf);
      const std::string reply =
          R"({"api_version":"manyforge.execution_command_response.v5","command":")" +
          command + R"(","ok":)" + ok + "}";
      owner.Serve(reply);
      EXPECT_EQ(ci::canonical_json(gateway.Host(command, {})), reply) << command;
    }
  }
}

TEST(GatewayOwnerLink, ManagerReplyUpToTheSceneSnapshotBound) {
  // The Manager's scene_owner_read reply carries a whole logical scene: up
  // to the snapshot bound plus envelope; every Manager command shares it.
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  for (const auto *command : {"scene_owner_read", "apply_status"}) {
    std::filesystem::remove(root.path() / "manager-command.sock");
    FakeOwner owner(root.path(), "manager-command.sock");
    const auto reply = ManagerReplyOfSize(gw::kMaximumManagerResponseBytes);
    owner.Serve(reply);
    EXPECT_EQ(ci::canonical_json(gateway.Manager(command, {})), reply) << command;
  }
  std::filesystem::remove(root.path() / "manager-command.sock");
  FakeOwner owner(root.path(), "manager-command.sock");
  owner.Serve(ManagerReplyOfSize(gw::kMaximumManagerResponseBytes + 1U));
  EXPECT_EQ(CodeOf([&] { (void)gateway.Manager("scene_owner_read", {}); }),
            "manager_command_response_invalid");
}

TEST(GatewayOwnerLink, ConnectLimitEndsAWaitForAFullBacklog) {
  // A listener that never accepts, its backlog full: the connect waits for
  // the connect limit only and the owner is unavailable.
  RunRoot root;
  FakeOwner owner(root.path(), "execution-command.sock", 0600, 0);
  // Backlog 0 holds one pending connection; this one fills it.
  const int pending = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto path = (root.path() / "execution-command.sock").string();
  std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
  ASSERT_EQ(::connect(pending, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  const gw::Gateway gateway(gw::GatewayConfig{root.path(), std::nullopt,
                                              gw::AttributionTransport::kInternal,
                                              std::chrono::milliseconds(50)});
  const auto started = std::chrono::steady_clock::now();
  try {
    (void)gateway.Host("identity", {});
    ADD_FAILURE() << "the connect to a full backlog succeeded";
  } catch (const gw::GatewayError &error) {
    EXPECT_EQ(error.code(), "execution_owner_unavailable");
    EXPECT_NE(std::string(error.what()).find("connect"), std::string::npos) << error.what();
  }
  EXPECT_GE(std::chrono::steady_clock::now() - started, std::chrono::milliseconds(50));
  ::close(pending);
}

TEST(GatewayOwnerLink, ManagerCallLimitEndsAWaitForASilentManager) {
  // A Manager that never answers (the connection waits in its backlog): a
  // call with a limit ends after it, as the pose sources' former socket
  // timeout; the Manager is unavailable.
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  FakeOwner manager(root.path(), "manager-command.sock");
  const auto limit = std::chrono::milliseconds(100);
  const auto started = std::chrono::steady_clock::now();
  try {
    (void)gateway.Manager("scene_runtime_owner_read", {}, limit);
    ADD_FAILURE() << "a call to a silent Manager returned";
  } catch (const gw::GatewayError &error) {
    EXPECT_EQ(error.code(), "manager_command_unavailable");
    EXPECT_NE(std::string(error.what()).find("recvmsg"), std::string::npos) << error.what();
    // The expiry names itself; the caller maps it to its own outcome.
    ASSERT_EQ(error.details().size(), 1U);
    EXPECT_EQ(error.details()[0].first, "reason");
    EXPECT_EQ(error.details()[0].second.as_string(), "limit_expired");
  }
  const auto elapsed = std::chrono::steady_clock::now() - started;
  EXPECT_GE(elapsed, limit);
  EXPECT_LT(elapsed, std::chrono::milliseconds(2000));
}

TEST(GatewayOwnerLink, PeerCloseWithoutReply) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  FakeOwner host(root.path(), "execution-command.sock");
  host.Serve(std::nullopt);
  // As `execution._decode_execution_response`: an empty read.
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("identity", {}); }),
            "execution_command_response_invalid");
  FakeOwner manager(root.path(), "manager-command.sock");
  manager.Serve(std::nullopt);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Manager("apply_status", {}); }),
            "manager_command_unavailable");
}

TEST(GatewayOwnerLink, MissingOrUnsafeEndpoints) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("identity", {}); }),
            "execution_owner_unavailable");
  EXPECT_EQ(CodeOf([&] { (void)gateway.Manager("apply_status", {}); }),
            "manager_command_unavailable");
  FakeOwner loose(root.path(), "execution-control.sock", 0666);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("stop", {}); }),
            "execution_owner_unavailable");
  EXPECT_EQ(CodeOf([&] {
              gw::Gateway missing(gw::GatewayConfig{root.path() / "absent",
                                                    std::nullopt});
            }),
            "execution_owner_unavailable");
  EXPECT_EQ(CodeOf([&] {
              gw::Gateway missing(gw::GatewayConfig{std::nullopt,
                                                    root.path() / "absent"});
            }),
            "manager_command_unavailable");
  ASSERT_EQ(::chmod(root.path().c_str(), 0770), 0);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("identity", {}); }),
            "execution_owner_unavailable");
  ASSERT_EQ(::chmod(root.path().c_str(), 0700), 0);
}

TEST(GatewayOwnerLink, UnknownCommandsAndEnvelopeMembersAreRefused) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_EQ(CodeOf([&] { (void)gateway.Host("no_such", {}); }),
            "execution_command_invalid");
  EXPECT_EQ(CodeOf([&] { (void)gateway.Manager("scene_commit", {}); }),
            "manager_command_invalid");
  EXPECT_EQ(CodeOf([&] {
              (void)gateway.Host("identity", {{"command", CanonicalValue("x")}});
            }),
            "execution_command_invalid");
  EXPECT_EQ(CodeOf([&] {
              (void)gateway.Host(
                  "set_io_output",
                  {{"request", CanonicalValue(std::string(
                                   gw::kMaximumExecutionPacketBytes, 'a'))}});
            }),
            "execution_command_oversized");
}

TEST(GatewayCommandKind, SuccessMustNameTheCommand) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  {
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v6","command":"targets","ok":true})");
    EXPECT_EQ(CodeOf([&] { (void)gateway.Host("identity", {}); }),
              "execution_command_response_invalid");
  }
  std::filesystem::remove(root.path() / "execution-command.sock");
  {
    // A refusal need not name it.
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v6","ok":false})");
    EXPECT_FALSE(Member(gateway.Host("identity", {}), "ok")->as_bool());
  }
  std::filesystem::remove(root.path() / "execution-command.sock");
  {
    // A command the Python never checked.
    FakeOwner owner(root.path(), "execution-command.sock");
    owner.Serve(R"({"api_version":"manyforge.execution_command_response.v6","ok":true})");
    EXPECT_TRUE(Member(gateway.Host("set_rule_active", {}), "ok")->as_bool());
  }
}

TEST(GatewayManager, SceneOwnerReadIsTheBareCommand) {
  // The runner's narrow scene read at the owner: no members, the reply
  // returned unchanged.
  RunRoot root;
  FakeOwner owner(root.path(), "manager-command.sock");
  const std::string reply =
      R"({"api_version":"manyforge.manager_command_response.v3","logical_scene":{},"ok":true,"projection_revision":{}})";
  owner.Serve(reply);
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_NE(gw::FindManagerCommand("scene_owner_read"), nullptr);
  EXPECT_EQ(ci::canonical_json(gateway.Manager("scene_owner_read", {})), reply);
  EXPECT_EQ(owner.Request(),
            R"({"api_version":"manyforge.manager_command.v2","command":"scene_owner_read"})");
}

std::string AttributionSent(gw::AttributionTransport transport,
                            const std::string &command,
                            std::optional<std::string> principal,
                            const std::string &leaf = "execution-command.sock") {
  RunRoot root;
  FakeOwner owner(root.path(), leaf);
  owner.Serve(std::string(R"({"api_version":"manyforge.execution_command_response.v6","command":")") +
              command + R"(","ok":true})");
  const auto gateway = MakeGateway(root, transport);
  (void)gateway.Host(command, {}, std::move(principal));
  const auto request = Parse(owner.Request());
  const auto *attribution = Member(request, "attribution");
  return attribution == nullptr ? "none" : ci::canonical_json(*attribution);
}

TEST(GatewayAttribution, PerPolicy) {
  using T = gw::AttributionTransport;
  EXPECT_EQ(AttributionSent(T::kInternal, "submit", std::nullopt),
            R"({"assurance":"unverified","principal_id":null,"transport":"internal"})");
  EXPECT_EQ(AttributionSent(T::kHttp, "submit", std::nullopt),
            R"({"assurance":"unverified","principal_id":null,"transport":"http"})");
  EXPECT_EQ(AttributionSent(T::kHttp, "submit", "operator.7"),
            R"({"assurance":"externally_authenticated","principal_id":"operator.7","transport":"http"})");
  EXPECT_EQ(AttributionSent(T::kHttp, "set_mock_io_input", std::nullopt),
            R"({"assurance":"unverified","principal_id":null,"transport":"internal"})");
  EXPECT_EQ(AttributionSent(T::kHttp, "identity", std::nullopt), "none");
  EXPECT_EQ(AttributionSent(T::kInternal, "stop_program_run", std::nullopt,
                            "execution-control.sock"),
            R"({"assurance":"unverified","principal_id":null,"transport":"internal"})");
}

TEST(GatewayAttribution, Refusals) {
  RunRoot root;
  const auto http = MakeGateway(root, gw::AttributionTransport::kHttp);
  const auto internal = MakeGateway(root, gw::AttributionTransport::kInternal);
  for (const auto *command : {"stop_program_run", "release_halt", "read_stop", "cell_stop"})
    EXPECT_EQ(CodeOf([&] { (void)http.Host(command, {}); }),
              "execution_request_attribution_invalid")
        << command;
  EXPECT_EQ(CodeOf([&] { (void)internal.Host("submit", {}, "operator.7"); }),
            "execution_request_attribution_invalid");
  EXPECT_EQ(CodeOf([&] { (void)http.Host("submit", {}, ""); }),
            "execution_request_attribution_invalid");
  EXPECT_EQ(CodeOf([&] { (void)http.Host("submit", {}, std::string(257, 'a')); }),
            "execution_request_attribution_invalid");
  // "e" + combining acute: valid UTF-8, not NFC.
  EXPECT_EQ(CodeOf([&] { (void)http.Host("submit", {}, "e\xCC\x81"); }),
            "execution_request_attribution_invalid");
  EXPECT_EQ(CodeOf([&] { (void)http.Host("identity", {}, "operator.7"); }),
            "execution_request_attribution_invalid");
}

CanonicalValue::Object Transaction(std::string kind, bool evidence, bool admission,
                                   bool reset) {
  CanonicalValue::Object transaction{
      {"request_context", CanonicalValue(CanonicalValue::Object{})},
      {"operation", CanonicalValue(CanonicalValue::Object{
                        {"kind", CanonicalValue(std::move(kind))},
                        {"mass_kg", CanonicalValue()},
                        {"carrier_component_id", CanonicalValue("c")}})},
      {"program_node_evidence", evidence ? CanonicalValue(CanonicalValue::Object{})
                                         : CanonicalValue()},
  };
  if (admission) transaction.emplace_back("expected_admission_generation", CanonicalValue(3.0));
  transaction.emplace_back("program_reset_scene_authorization",
                           reset ? CanonicalValue(CanonicalValue::Object{}) : CanonicalValue());
  return transaction;
}

TEST(GatewaySceneCommit, CrossFieldRules) {
  RunRoot root;
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kHttp);
  const auto code = [&](std::string kind, bool evidence, bool admission, bool reset) {
    return CodeOf([&] {
      (void)gateway.SceneCommit(Transaction(std::move(kind), evidence, admission, reset));
    });
  };
  EXPECT_EQ(code("attach_object", true, false, false), "invalid_request");
  EXPECT_EQ(code("attach_object", true, true, true), "invalid_request");
  EXPECT_EQ(code("add_object", false, false, false), "authoring_not_ready");
  EXPECT_EQ(code("update_object_pose", false, true, true), "authoring_not_ready");
  EXPECT_EQ(code("attach_object", false, false, true), "invalid_request");
  EXPECT_EQ(CodeOf([&] {
              (void)gateway.SceneCommit(Transaction("add_object", false, true, false), "");
            }),
            "scene_request_attribution_invalid");
  const auto internal = MakeGateway(root, gw::AttributionTransport::kInternal);
  EXPECT_EQ(CodeOf([&] {
              (void)internal.SceneCommit(Transaction("add_object", false, true, false),
                                         "operator.7");
            }),
            "scene_request_attribution_invalid");
}

TEST(GatewaySceneCommit, WireBody) {
  RunRoot root;
  FakeOwner owner(root.path(), "manager-command.sock");
  const std::string reply =
      R"({"api_version":"manyforge.manager_command_response.v3","ok":true})";
  owner.Serve(reply);
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kHttp);
  auto transaction = Transaction("add_object", false, false, true);
  EXPECT_EQ(ci::canonical_json(gateway.SceneCommit(std::move(transaction), "operator.7")),
            reply);
  EXPECT_EQ(
      owner.Request(),
      R"({"api_version":"manyforge.manager_command.v2","command":"scene_commit","transaction":{)"
      R"("attribution":{"assurance":"externally_authenticated","principal_id":"operator.7","transport":"http"},)"
      R"("expected_admission_generation":null,)"
      R"("operation":{"carrier_component_id":"c","kind":"add_object"},)"
      R"("program_reset_scene_authorization":{},"request_context":{}}})");
}

TEST(GatewaySceneCommit, InternalGatewaySendsTheHttpAttribution) {
  // The Program runner's commits are recorded as when they went through
  // Headless's scene route: http, unverified, no principal.
  RunRoot root;
  FakeOwner owner(root.path(), "manager-command.sock");
  owner.Serve(R"({"api_version":"manyforge.manager_command_response.v3","ok":false})");
  const auto gateway = MakeGateway(root, gw::AttributionTransport::kInternal);
  (void)gateway.SceneCommit(Transaction("attach_object", true, true, false));
  const auto request = Parse(owner.Request());
  const auto &transaction = *Member(request, "transaction");
  EXPECT_EQ(ci::canonical_json(*Member(transaction, "attribution")),
            R"({"assurance":"unverified","principal_id":null,"transport":"http"})");
  EXPECT_NE(Member(transaction, "program_node_evidence"), nullptr);
}

// ---------------------------------------------------------------------------
// The tables against the owners' sources in this checkout.
// ---------------------------------------------------------------------------

/// The checkout root: this file is <root>/manyforge_headless/test/.
std::filesystem::path SourceRoot() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::string ReadSource(const std::filesystem::path &relative) {
  std::ifstream file(SourceRoot() / relative);
  EXPECT_TRUE(file.good()) << (SourceRoot() / relative);
  std::ostringstream text;
  text << file.rdbuf();
  return text.str();
}

/// The string literal assigned to `name` (`name = "..."`).
std::string ConstantText(const std::string &text, const std::string &name) {
  std::smatch match;
  EXPECT_TRUE(std::regex_search(text, match,
                                std::regex(name + R"(\s*=\s*"([^"]+)\")")))
      << name;
  return match.size() > 1 ? match[1].str() : std::string();
}

TEST(GatewayTables, HostReplyVersionsMatchTheHost) {
  // manyforge_roboplan/src/execution_host_config.hpp:28 (every reply but cell
  // admission) and cell_admission_command_codec.hpp:14 (cell admission).
  const auto host_version = ConstantText(
      ReadSource("manyforge_roboplan/src/execution_host_config.hpp"),
      "kResponseApiVersion");
  const auto admission_version = ConstantText(
      ReadSource("manyforge_roboplan/src/cell_admission_command_codec.hpp"),
      "kCellAdmissionResponseApiVersion");
  EXPECT_EQ(host_version, gw::kExecutionResponseApiVersion);
  EXPECT_EQ(admission_version, gw::kCellAdmissionResponseApiVersion);

  // No third reply version anywhere in the host's sources.
  std::set<std::string> versions;
  const std::string prefix = "\"manyforge.execution_command_response.v";
  for (const auto &entry : std::filesystem::recursive_directory_iterator(
           SourceRoot() / "manyforge_roboplan/src")) {
    if (!entry.is_regular_file()) continue;
    const auto extension = entry.path().extension();
    if (extension != ".cpp" && extension != ".hpp") continue;
    const auto text = ReadSource(std::filesystem::relative(entry.path(), SourceRoot()));
    for (auto at = text.find(prefix); at != std::string::npos;
         at = text.find(prefix, at + 1U)) {
      const auto end = text.find('"', at + 1U);
      versions.insert(text.substr(at + 1U, end - at - 1U));
    }
  }
  EXPECT_EQ(versions, (std::set<std::string>{host_version, admission_version}));

  // The commands the host answers with the cell-admission codec: the
  // candidates of its packet loop (execution_host_surface.cpp:4371-4375,
  // answered by CellAdmissionResponseValue, cell_admission_command_codec.cpp:641-653).
  const auto surface =
      ReadSource("manyforge_roboplan/src/execution_host_surface.cpp");
  const auto loop = surface.find("for (const auto candidate :");
  ASSERT_NE(loop, std::string::npos);
  const auto list = surface.substr(loop, surface.find("})", loop) - loop);
  std::set<std::string> cell_admission;
  const std::regex literal(R"re("([a-z_]+)")re");
  for (std::sregex_iterator it(list.begin(), list.end(), literal), end; it != end; ++it)
    cell_admission.insert((*it)[1].str());
  ASSERT_TRUE(cell_admission.contains("cell_admission_get"));

  for (const auto &spec : gw::HostCommands()) {
    const bool admission = cell_admission.contains(std::string(spec.name));
    EXPECT_EQ(spec.response_version, admission ? admission_version : host_version)
        << spec.name;
  }
}

TEST(GatewayTables, ManagerReplyBoundIsTheManagers) {
  // manager_daemon.cpp:136-137: every reply is bounded by the snapshot bound
  // plus 16 KiB (TrySendResponse, the one send of a command reply).
  const auto daemon = ReadSource("manyforge_headless/src/manager_daemon.cpp");
  EXPECT_TRUE(std::regex_search(
      daemon, std::regex(R"(kMaximumResponseBytes\s*=\s*scene::kMaximumLogicalSceneSnapshotBytes\s*\+\s*16U\s*\*\s*1024U;)")));
  EXPECT_TRUE(std::regex_search(
      daemon, std::regex(R"(AtomicResponseCapacity\(client,\s*kMaximumResponseBytes\))")));
  // manyforge_scene/include/manyforge_scene/logical_scene_owner.hpp:36.
  EXPECT_TRUE(std::regex_search(
      ReadSource("manyforge_scene/include/manyforge_scene/logical_scene_owner.hpp"),
      std::regex(R"(kMaximumLogicalSceneSnapshotBytes\s*=\s*384U\s*\*\s*1024U;)")));
  EXPECT_EQ(gw::kMaximumManagerResponseBytes, 384U * 1024U + 16U * 1024U);
}

}  // namespace

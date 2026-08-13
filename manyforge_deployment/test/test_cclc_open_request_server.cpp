#include <gtest/gtest.h>

#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/cclc_open_request.hpp>
#include <manyforge_deployment/cclc_open_request_server.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace manyforge::deployment {
namespace {

using content_identity::CanonicalValue;

class ScopedRoot final {
public:
  ScopedRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-cclc-open-server-" +
               std::to_string(static_cast<long long>(::getpid())) + "-" +
               std::to_string(sequence_.fetch_add(1U)))) {
    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(path_, error));
    if (!std::filesystem::create_directory(path_, error) || error) {
      throw std::runtime_error("cannot create CCLC server test root");
    }
  }

  ~ScopedRoot() {
    std::error_code error;
    static_cast<void>(std::filesystem::remove_all(path_, error));
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  inline static std::atomic<std::uint64_t> sequence_{0U};
  std::filesystem::path path_;
};

class RecordingOpenPort final : public core::ICCLCOpenRequestPort {
public:
  [[nodiscard]] core::Result<core::CCLCLifecycleRequestResultDTO>
  SubmitLifecycle(const core::ComponentLifecycleIntentDTO &intent) override {
    {
      std::unique_lock lock(mutex_);
      requests_.push_back(intent);
      condition_.notify_all();
      if (intent.request_context.request_id ==
          core::RequestId("request.browser-lost")) {
        condition_.wait(lock, [&] { return release_lost_response_; });
      }
    }
    return core::Result<core::CCLCLifecycleRequestResultDTO>::Ok(
        {core::RequestReplayStatusDTO::kRequestIdConflict, std::nullopt,
         std::nullopt});
  }

  [[nodiscard]] core::Result<std::optional<core::ComponentLifecycleStatusDTO>>
  LoadLifecycleStatus(
      const core::LifecycleTransactionId &transaction_id) override {
    {
      std::lock_guard lock(mutex_);
      status_reads_.push_back(transaction_id);
    }
    return core::Result<std::optional<core::ComponentLifecycleStatusDTO>>::Ok(
        std::nullopt);
  }

  [[nodiscard]] core::Result<core::ComponentLifecycleCurrentSnapshotDTO>
  ReadCurrentComponent(const core::ControllableComponentId &) const override {
    return core::Result<core::ComponentLifecycleCurrentSnapshotDTO>::Err(
        {"unused", "component read is unused", {}});
  }

  [[nodiscard]] core::Result<core::CCLCRouteSelectionTemplateSetDTO>
  ReadRouteSelectionTemplates(
      const core::ControllableComponentId &) const override {
    return core::Result<core::CCLCRouteSelectionTemplateSetDTO>::Err(
        {"unused", "route template read is unused", {}});
  }

  [[nodiscard]] core::Result<core::CCLCRouteSelectionRequestResultDTO>
  SubmitRouteSelection(const core::RouteSelectionIntentDTO &) override {
    return core::Result<core::CCLCRouteSelectionRequestResultDTO>::Err(
        {"unused", "route submission is unused", {}});
  }

  [[nodiscard]] core::Result<std::optional<core::RouteSelectionStatusDTO>>
  LoadRouteSelectionStatus(const core::RouteSelectionTransactionId &) override {
    return core::Result<std::optional<core::RouteSelectionStatusDTO>>::Err(
        {"unused", "route status is unused", {}});
  }

  [[nodiscard]] bool WaitForRequests(const std::size_t count) {
    std::unique_lock lock(mutex_);
    return condition_.wait_for(lock, std::chrono::seconds(3),
                               [&] { return requests_.size() >= count; });
  }

  [[nodiscard]] std::vector<core::ComponentLifecycleIntentDTO>
  Requests() const {
    std::lock_guard lock(mutex_);
    return requests_;
  }

  [[nodiscard]] std::vector<core::LifecycleTransactionId> StatusReads() const {
    std::lock_guard lock(mutex_);
    return status_reads_;
  }

  void ReleaseLostResponse() {
    {
      std::lock_guard lock(mutex_);
      release_lost_response_ = true;
    }
    condition_.notify_all();
  }

private:
  mutable std::mutex mutex_;
  std::condition_variable condition_;
  std::vector<core::ComponentLifecycleIntentDTO> requests_;
  std::vector<core::LifecycleTransactionId> status_reads_;
  bool release_lost_response_{false};
};

[[nodiscard]] CanonicalValue Context(const std::string &request_id,
                                     const std::string &source_label) {
  return CanonicalValue(CanonicalValue::Object{
      {"request_id", CanonicalValue(request_id)},
      {"request_owner_epoch", CanonicalValue(source_label + ".epoch.1")},
      {"source_label", CanonicalValue(source_label)}});
}

[[nodiscard]] std::string LifecycleRequest(const std::string &request_id,
                                           const std::string &source_label) {
  const auto payload = CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue("component.arm")},
      {"expected_component_lifecycle_generation", CanonicalValue(4.0)},
      {"expected_control_domains",
       CanonicalValue(
           CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"control_domain_id", CanonicalValue("domain.arm")},
               {"control_domain_revision", CanonicalValue("domain.revision.1")},
               {"controller_topology_id", CanonicalValue("topology.arm")},
               {"expected_active_control_configuration",
                CanonicalValue(CanonicalValue::Object{
                    {"presence", CanonicalValue("absent")}})},
               {"expected_control_bindings",
                CanonicalValue(
                    CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                        {"control_binding_id", CanonicalValue("binding.arm")},
                        {"control_binding_revision",
                         CanonicalValue("binding.revision.1")},
                        {"control_route_id", CanonicalValue("route.arm")}})})},
               {"topology_revision",
                CanonicalValue("topology.revision.1")}})})},
      {"expected_graph_revision", CanonicalValue("graph.1")},
      {"intent", CanonicalValue("enable")},
      {"request_context", Context(request_id, source_label)}});
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kCCLCOpenRequestApiVersion))},
      {"command", CanonicalValue("lifecycle_submit")},
      {"payload", payload}}));
}

[[nodiscard]] std::string StatusRequest() {
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kCCLCOpenRequestApiVersion))},
      {"command", CanonicalValue("lifecycle_status")},
      {"payload", CanonicalValue(CanonicalValue::Object{
                      {"lifecycle_transaction_id",
                       CanonicalValue("lifecycle.lost-client")}})}}));
}

[[nodiscard]] int Connect(const std::filesystem::path &socket_path) {
  const int descriptor = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (descriptor < 0) {
    throw std::runtime_error("cannot create CCLC client socket");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = socket_path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("CCLC client socket path is too long");
  }
  std::memcpy(address.sun_path, spelling.c_str(), spelling.size() + 1U);
  if (::connect(descriptor, reinterpret_cast<const sockaddr *>(&address),
                sizeof(address)) != 0) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("cannot connect CCLC client socket");
  }
  return descriptor;
}

[[nodiscard]] std::string Exchange(const std::filesystem::path &socket_path,
                                   const std::string &request) {
  const int descriptor = Connect(socket_path);
  if (::send(descriptor, request.data(), request.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(request.size())) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("cannot send CCLC request");
  }
  std::vector<char> response(core::kMaximumCCLCOpenResponseBytes);
  const auto count = ::recv(descriptor, response.data(), response.size(), 0);
  static_cast<void>(::close(descriptor));
  if (count <= 0) {
    throw std::runtime_error("CCLC response is absent");
  }
  return {response.data(), static_cast<std::size_t>(count)};
}

[[nodiscard]] int SubmitWithoutReading(const std::filesystem::path &socket_path,
                                       const std::string &request) {
  const int descriptor = Connect(socket_path);
  if (::send(descriptor, request.data(), request.size(), MSG_NOSIGNAL) !=
      static_cast<ssize_t>(request.size())) {
    static_cast<void>(::close(descriptor));
    throw std::runtime_error("cannot send dropped CCLC request");
  }
  return descriptor;
}

TEST(CCLCOpenRequestServer,
     ConcurrentClientsAndLostResponseNeverBecomeLifecycleOwners) {
  ScopedRoot root;
  RecordingOpenPort port;
  auto server = CCLCOpenRequestUnixServer::CreateAndStart(
      root.path(), port, core::RequestTransportDTO::kHttp);
  ASSERT_TRUE(server.ok()) << server.error().message;
  const auto socket_path = server.value()->socket_path();

  const std::array clients{
      std::pair{"request.composer", "composer"},
      std::pair{"request.cli", "cli"},
      std::pair{"request.plc", "plc-gateway"},
  };
  std::vector<std::future<std::string>> responses;
  responses.reserve(clients.size());
  for (const auto &[request_id, source_label] : clients) {
    responses.push_back(std::async(std::launch::async, [socket_path, request_id,
                                                        source_label] {
      return Exchange(socket_path, LifecycleRequest(request_id, source_label));
    }));
  }
  for (auto &response : responses) {
    const auto decoded = content_identity::parse_canonical_json(response.get());
    ASSERT_TRUE(decoded.find_object_member("ok")->as_bool());
  }
  ASSERT_TRUE(port.WaitForRequests(clients.size()));

  const int lost_client = SubmitWithoutReading(
      socket_path, LifecycleRequest("request.browser-lost", "composer"));
  ASSERT_TRUE(port.WaitForRequests(clients.size() + 1U));
  static_cast<void>(::close(lost_client));
  port.ReleaseLostResponse();
  const auto status = content_identity::parse_canonical_json(
      Exchange(socket_path, StatusRequest()));
  ASSERT_TRUE(status.find_object_member("ok")->as_bool());
  EXPECT_TRUE(status.find_object_member("result")->is_null());

  auto ros_server = CCLCOpenRequestUnixServer::CreateAndStart(
      root.path(), port, core::RequestTransportDTO::kRos);
  ASSERT_TRUE(ros_server.ok()) << ros_server.error().message;
  const auto ros_response = content_identity::parse_canonical_json(
      Exchange(ros_server.value()->socket_path(),
               LifecycleRequest("request.ros", "ros-service")));
  ASSERT_TRUE(ros_response.find_object_member("ok")->as_bool());
  ASSERT_TRUE(port.WaitForRequests(clients.size() + 2U));

  const auto recorded = port.Requests();
  ASSERT_EQ(recorded.size(), clients.size() + 2U);
  std::vector<std::string> source_labels;
  source_labels.reserve(recorded.size());
  for (const auto &request : recorded) {
    source_labels.push_back(request.request_context.source_label);
    EXPECT_EQ(request.attribution.transport,
              request.request_context.source_label == "ros-service"
                  ? core::RequestTransportDTO::kRos
                  : core::RequestTransportDTO::kHttp);
    EXPECT_EQ(request.attribution.assurance,
              core::RequestAssuranceDTO::kUnverified);
  }
  std::sort(source_labels.begin(), source_labels.end());
  EXPECT_EQ(source_labels,
            (std::vector<std::string>{"cli", "composer", "composer",
                                      "plc-gateway", "ros-service"}));
  EXPECT_EQ(port.StatusReads(),
            (std::vector<core::LifecycleTransactionId>{
                core::LifecycleTransactionId("lifecycle.lost-client")}));
}

} // namespace
} // namespace manyforge::deployment

#pragma once

// The execution-path gateway core (EXECUTION_PATH plan S1/S2, OD-380).
//
// One ROS-free C++ client for the Manager command socket and the execution
// host sockets, one function per effect, and the followers, mirrors and the
// run wake set the Program runner and Headless share. The Program runner calls
// it in-process through `manyforge_headless_py`; Headless wraps it as a thin
// HTTP/WebSocket adapter. The wire protocols are the existing ones: RFC 8785
// canonical JSON over same-user Unix SEQPACKET sockets, one request per
// connection.
//
// Rules (AGENTS.md concurrency rules):
// - Owners decide admission, replay and refusals; the core translates and
//   attributes, it does not re-validate requests. A reply is checked for its
//   transport envelope (canonical bytes, version, boolean `ok`, the command
//   kind it answers); an owner refusal (`ok:false`) is returned to the caller
//   as the owner's reply, never re-interpreted.
// - No lock is held across a socket call or a wait on another thread; every
//   mirror and the wake set have one leaf lock held for a memory copy.
// - The core calls no Python; Python threads pull from it in blocking calls
//   with the GIL released.
// - A sent request has no time limit (OD-213, OD-214): only the connect is
//   limited, because a listener that does not accept is the only sign of a
//   lost peer there. A host exit closes the connection and ends the wait.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

namespace manyforge::headless::gateway {

using content_identity::CanonicalValue;

/// A typed failure of the core itself (transport, envelope, a refused local
/// precondition). Owner refusals are replies, not errors.
class GatewayError : public std::runtime_error {
 public:
  GatewayError(std::string code, std::string message,
               CanonicalValue::Object details = {},
               std::string producer = "headless_api");
  [[nodiscard]] const std::string &code() const noexcept { return code_; }
  [[nodiscard]] const CanonicalValue::Object &details() const noexcept {
    return details_;
  }
  [[nodiscard]] const std::string &producer() const noexcept {
    return producer_;
  }

 private:
  std::string code_;
  CanonicalValue::Object details_;
  std::string producer_;
};

// ---------------------------------------------------------------------------
// Owner link: one request per connection.
// ---------------------------------------------------------------------------

/// The private run root of one owner, pinned by (device, inode) at
/// construction, with the checks of `manyforge_headless_api/private_transport.py`:
/// an absolute, canonical, same-user directory (owner rwx, not group/other
/// writable) whose identity never changes, and socket leaves that are exactly
/// one same-user socket of mode 0600 with one link. Every use re-opens the
/// directory and re-checks it; nothing is held open between uses.
class PinnedRunRoot {
 public:
  /// Throws GatewayError(`unavailable_code`) when the root is absent or unsafe.
  explicit PinnedRunRoot(
      std::filesystem::path path,
      std::string unavailable_code = "execution_owner_unavailable");
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  /// Connects a SEQPACKET socket (close-on-exec, blocking) to `leaf`, reached
  /// through /proc/self/fd/<dirfd>/<leaf> of the re-checked directory, and
  /// checks that the peer runs as this user (SO_PEERCRED). Only the connect is
  /// limited, by `connect_limit`. `receive_buffer_bytes` > 0 sets SO_RCVBUF
  /// before the connect. Returns the connected descriptor (the caller closes
  /// it). Throws GatewayError(`unavailable_code`) on any failure.
  [[nodiscard]] int Connect(std::string_view leaf,
                            std::chrono::milliseconds connect_limit,
                            int receive_buffer_bytes = 0) const;

 private:
  std::filesystem::path path_;
  std::string unavailable_code_;
  std::uint64_t device_{0};
  std::uint64_t inode_{0};
};

/// Which owner protocol an exchange speaks (bounds, error codes).
enum class OwnerProtocol {
  kExecutionHost,  ///< manyforge.execution_command.v4
  kManager,        ///< manyforge.manager_command.v2 / _response.v3
};

inline constexpr std::string_view kExecutionCommandApiVersion =
    "manyforge.execution_command.v4";
/// The host's reply version for every command except cell admission.
inline constexpr std::string_view kExecutionResponseApiVersion =
    "manyforge.execution_command_response.v6";
/// The host's reply version for the cell-admission commands (the host's
/// cell-admission codec answers them).
inline constexpr std::string_view kCellAdmissionResponseApiVersion =
    "manyforge.execution_command_response.v5";
inline constexpr std::string_view kManagerCommandApiVersion =
    "manyforge.manager_command.v2";
inline constexpr std::string_view kManagerResponseApiVersion =
    "manyforge.manager_command_response.v3";
inline constexpr std::size_t kMaximumExecutionPacketBytes = 1024U * 1024U;
inline constexpr std::size_t kMaximumExecutionResponseBytes =
    4U * 1024U * 1024U;
inline constexpr std::size_t kMaximumManagerPacketBytes =
    128U * 1024U + 4U * 1024U;
/// The Manager's one reply bound, for every command: the largest logical
/// scene snapshot (384 KiB, `scene::kMaximumLogicalSceneSnapshotBytes`) plus
/// 16 KiB of envelope (`kMaximumResponseBytes` of manager_daemon.cpp). No
/// command's reply has a smaller bound the Manager keeps: refusals carry
/// owner messages and details, and a reply over it is replaced by the
/// Manager's own `manager_command_response_oversized` refusal.
inline constexpr std::size_t kMaximumManagerResponseBytes =
    384U * 1024U + 16U * 1024U;
inline constexpr std::string_view kManagerSocketFile = "manager-command.sock";

/// Encodes `command` canonically, sends it on `leaf`, waits for the one reply
/// (with no time limit, or with `limit` on the connect, the send and the
/// receive each, the former socket timeout of the pose sources' Manager
/// client) and returns it after the envelope checks (canonical
/// bytes, object root, `response_version`, boolean `ok`). Throws GatewayError
/// with the protocol's existing codes:
///   host:    execution_command_invalid, execution_command_oversized,
///            execution_command_transport_failure,
///            execution_owner_unavailable, execution_command_response_invalid
///   manager: manager_command_invalid, manager_command_oversized,
///            manager_command_transport_failure,
///            manager_command_unavailable, manager_command_response_invalid
[[nodiscard]] CanonicalValue Exchange(const PinnedRunRoot &root,
                                      std::string_view leaf,
                                      OwnerProtocol protocol,
                                      std::string_view response_version,
                                      const CanonicalValue &command,
                                      std::chrono::milliseconds connect_limit,
                                      std::optional<std::chrono::milliseconds>
                                          limit = std::nullopt);

// ---------------------------------------------------------------------------
// Callers and attribution.
// ---------------------------------------------------------------------------

/// Which caller a gateway instance serves; it decides the wire attribution.
/// The Program runner is `kInternal`; Headless is `kHttp` (and `kInternal`
/// for its own Halt on a full disk).
enum class AttributionTransport { kHttp, kInternal };

// ---------------------------------------------------------------------------
// The command tables: one entry per wire command.
// ---------------------------------------------------------------------------

enum class HostLane { kCommand, kControl, kComponent };

[[nodiscard]] std::string_view HostLaneSocketFile(HostLane lane) noexcept;

enum class Attribution {
  kNone,           ///< the command carries no attribution member
  kCaller,         ///< from the gateway's transport and the caller's principal
  kInternalOnly,   ///< internal transport only; refused on an HTTP gateway
  kFixedInternal,  ///< always {internal, unverified, null} (Mock input)
};

struct HostCommandSpec {
  std::string_view name;  ///< wire `command` and Python method name
  HostLane lane;
  Attribution attribution;
  /// A successful reply must name this command in its `command` member
  /// (the check the Python clients made for this command).
  bool reply_names_command;
  /// The reply version the host answers this command with.
  std::string_view response_version{kExecutionResponseApiVersion};
};

struct ManagerCommandSpec {
  std::string_view name;  ///< wire `command` and Python method name
};

/// Every execution host request/reply command the runner or Headless sends.
[[nodiscard]] const std::vector<HostCommandSpec> &HostCommands();
/// Every Manager command the runner, Headless or the pose publisher sends,
/// except `scene_commit` (Gateway::SceneCommit).
[[nodiscard]] const std::vector<ManagerCommandSpec> &ManagerCommands();
[[nodiscard]] const HostCommandSpec *FindHostCommand(std::string_view name);
[[nodiscard]] const ManagerCommandSpec *FindManagerCommand(
    std::string_view name);

struct GatewayConfig {
  std::optional<std::filesystem::path> execution_run_root;
  std::optional<std::filesystem::path> manager_run_root;
  AttributionTransport attribution{AttributionTransport::kInternal};
  std::chrono::milliseconds connect_limit{10000};
};

/// The one client. Thread-safe and lock-free: it keeps no per-call state;
/// each call opens its own connection.
class Gateway {
 public:
  /// Pins the configured run roots (GatewayError execution_owner_unavailable
  /// / manager_command_unavailable when one is unsafe or absent).
  explicit Gateway(GatewayConfig config);
  ~Gateway();
  Gateway(const Gateway &) = delete;
  Gateway &operator=(const Gateway &) = delete;

  /// One host command: builds {api_version, command, attribution?, ...members}
  /// and returns the host's reply (ok true or false). `principal_id` is only
  /// valid for a `kCaller` command on an HTTP gateway.
  [[nodiscard]] CanonicalValue Host(
      std::string_view command, CanonicalValue::Object members,
      std::optional<std::string> principal_id = {}) const;

  /// One Manager command: {api_version, command, ...members}; returns the
  /// Manager's reply (ok true or false). Without `limit` only the connect is
  /// limited; with it the connect, the send and the receive each end within
  /// `limit` (manager_command_unavailable), for a caller whose only detector
  /// of a silent Manager it is (the pose sources, OD-214 (2)).
  [[nodiscard]] CanonicalValue Manager(
      std::string_view command, CanonicalValue::Object members,
      std::optional<std::chrono::milliseconds> limit = std::nullopt) const;

  /// The scene commit (`scene_commit`): today's cross-field rules of
  /// `HAPI/service.py` scene_commit (program node evidence, expected
  /// admission generation, Reset authorization), the wire body of
  /// `manager._transaction_body` plus the caller's attribution in the form
  /// `manager.commit_scene_transaction` sent (transport `http`, whatever the
  /// gateway's own attribution; an internal gateway has no principal), the
  /// exchange.
  /// Returns the Manager's reply (ok true or false) unchanged.
  [[nodiscard]] CanonicalValue SceneCommit(
      CanonicalValue::Object transaction,
      std::optional<std::string> principal_id = {}) const;

  [[nodiscard]] const GatewayConfig &config() const noexcept { return config_; }

 private:
  GatewayConfig config_;
  std::unique_ptr<PinnedRunRoot> execution_root_;
  std::unique_ptr<PinnedRunRoot> manager_root_;
};

}  // namespace manyforge::headless::gateway

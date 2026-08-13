#pragma once

#include <filesystem>
#include <manyforge_core/cclc_open_request.hpp>
#include <memory>
#include <string_view>

namespace manyforge::deployment {

inline constexpr std::string_view kCCLCOpenRequestSocketFilename =
    "cclc-command.sock";
inline constexpr std::string_view kCCLCOpenRequestRosSocketFilename =
    "cclc-ros-command.sock";

/// Bounded same-user SOCK_SEQPACKET adapter for the headless HTTP owner.
///
/// The server stamps every request as unverified HTTP ingress, owns no CCLC
/// state, and never interprets lifecycle or route policy. The trusted state
/// root and semantic port must outlive the server. Destruction closes ingress,
/// drains the worker, and removes only the exact socket inode it created.
class CCLCOpenRequestUnixServer final {
public:
  [[nodiscard]] static core::Result<std::unique_ptr<CCLCOpenRequestUnixServer>>
  CreateAndStart(const std::filesystem::path &trusted_state_root,
                 core::ICCLCOpenRequestPort &port,
                 core::RequestTransportDTO transport);

  ~CCLCOpenRequestUnixServer();

  CCLCOpenRequestUnixServer(const CCLCOpenRequestUnixServer &) = delete;
  CCLCOpenRequestUnixServer &
  operator=(const CCLCOpenRequestUnixServer &) = delete;

  [[nodiscard]] const std::filesystem::path &socket_path() const noexcept;

private:
  class Impl;
  explicit CCLCOpenRequestUnixServer(std::unique_ptr<Impl> impl);
  std::unique_ptr<Impl> impl_;
};

} // namespace manyforge::deployment

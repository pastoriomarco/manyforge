#pragma once

#include "execution_host_config.hpp"
#include <atomic>
#include <signal.h>
#include <sys/stat.h>
#include <stop_token>
#include <utility>

namespace manyforge::roboplan_backend::host_detail {

class FileDescriptor final {
public:
  explicit FileDescriptor(int value = -1) noexcept;
  ~FileDescriptor();
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept;
  FileDescriptor &operator=(FileDescriptor &&other) noexcept;
  [[nodiscard]] int get() const noexcept;
  [[nodiscard]] explicit operator bool() const noexcept;

private:
  int value_;
};

class HostLock final {
public:
  explicit HostLock(const std::filesystem::path &run_root);

private:
  FileDescriptor descriptor_;
};

class CommandSocket final {
public:
  CommandSocket(const std::filesystem::path &run_root,
                std::string_view socket_file_name);

  ~CommandSocket();

  [[nodiscard]] int get() const noexcept;

private:
  std::filesystem::path path_;
  FileDescriptor descriptor_;
  struct stat bound_status_ {};
};

extern volatile sig_atomic_t g_stop_requested;
extern std::atomic<int> g_stop_signal_wake_fd;
void HandleStopSignal(int);

[[nodiscard]] std::string SelectedExecutionOfferDigest();

[[noreturn]] void Fail(std::string detail);

[[noreturn]] void FailErrno(std::string_view context);

void Require(bool condition, std::string detail);

[[nodiscard]] std::filesystem::path
ExistingDirectory(std::filesystem::path path, std::string_view label);

[[nodiscard]] std::filesystem::path
ExistingPrivateDirectory(std::filesystem::path path,
                         const std::string_view label);

void RequireDisjointDirectories(const std::filesystem::path &left,
                                const std::filesystem::path &right,
                                const std::string_view labels);

void RequirePathOutsideDirectory(const std::filesystem::path &path,
                                 const std::filesystem::path &directory,
                                 const std::string_view labels);

[[nodiscard]] std::filesystem::path
ExistingRegularFile(std::filesystem::path path, std::string_view label,
                    bool executable);

[[nodiscard]] std::string RandomToken(std::string_view prefix);

[[nodiscard]] component_manager::IsaacSiteSettings
LoadManagedIsaacSiteSettings(const std::filesystem::path &path);

[[nodiscard]] bool
IsPortableRuntimeIdentity(const std::string_view value) noexcept;

[[nodiscard]] int ParseFileDescriptor(const std::string_view value);

[[nodiscard]] ManagedRuntimeChildConfig
ParseManagedRuntimeArguments(const int argc, char **argv);

void SendManagedRuntimePacket(const int fd, const std::string_view bytes);

[[nodiscard]] std::optional<std::string>
ReceiveManagedRuntimePacket(const int fd);

[[nodiscard]] CanonicalValue
ExchangeManagerCommand(const std::filesystem::path &manager_run_root,
                       CanonicalValue command,
                       const std::stop_token stop_token = {});

[[nodiscard]] HostConfig ParseArguments(int argc, char **argv);

void InstallSignalHandlers();
} // namespace manyforge::roboplan_backend::host_detail

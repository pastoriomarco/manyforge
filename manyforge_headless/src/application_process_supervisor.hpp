#pragma once

#include <manyforge_core/types/application_identity.hpp>

#include <sys/types.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::headless {

/// `manyforge.application_processes.v1` (build spec 3.3, S3; contract 1.1):
/// the application processes the Manager starts, watches and stops. The
/// Manager does not know what they are; it runs the list it is given.
inline constexpr std::string_view kApplicationProcessesApiVersion =
    "manyforge.application_processes.v1";

/// The Manager's exit status when an application process or the execution
/// host exits outside the stop sequence (OD-122 (1); contract 1.1, P-11).
inline constexpr int kApplicationComponentExitStatus = 72;

/// The Manager's exit status after an orderly stop in which a step did not
/// complete: a process was killed, the execution host's exit was not clean
/// or the planned Halt did not complete (OD-187 (2)). 0 when every step
/// completed.
inline constexpr int kApplicationStopIncompleteExitStatus = 74;

struct ApplicationProcessSpec final {
  manyforge::core::ApplicationRole role{};
  std::vector<std::string> argv;
  /// Added to the Manager's own environment for this child.
  std::vector<std::pair<std::string, std::string>> environment;
  /// Launched on the first observation tick after the Manager published its
  /// first converged baseline (build spec 4.7 option a).
  bool after_baseline{};

  friend bool operator==(const ApplicationProcessSpec &,
                         const ApplicationProcessSpec &) = default;
};

/// Strict decode; unknown members, roles other than `headless`,
/// `program_runner`, `composer`, a repeated role and every bound of contract
/// 1.1 are refused with an exception naming the field.
[[nodiscard]] std::vector<ApplicationProcessSpec>
ParseApplicationProcessList(std::string_view document);

/// One application process exit, as the supervisor observed it.
struct ApplicationProcessExit final {
  manyforge::core::ApplicationRole role{};
  /// True when `value` is the terminating signal, false when it is the exit
  /// status.
  bool signaled{};
  int value{};
  pid_t pid{};
  /// The kernel reported a core dump for the terminating signal (WCOREDUMP).
  bool core_dumped{};
};

/// Starts, watches and stops the listed processes. Used only from the
/// Manager's main thread, which lives as long as the Manager, so the
/// children's PR_SET_PDEATHSIG stays bound to the Manager (build spec 3.2
/// caveat). No lock is held anywhere in it.
class ApplicationProcessSupervisor final {
public:
  /// `core_dump_limit_bytes`: the soft core-file size limit each child
  /// starts with (0, core dumps off, unless the site settings enable them),
  /// capped at the hard limit, which is kept.
  ApplicationProcessSupervisor(std::vector<ApplicationProcessSpec> specs,
                               manyforge::core::ApplicationStartId application_start_id,
                               std::uint64_t core_dump_limit_bytes = 0U);
  ~ApplicationProcessSupervisor();

  ApplicationProcessSupervisor(const ApplicationProcessSupervisor &) = delete;
  ApplicationProcessSupervisor &
  operator=(const ApplicationProcessSupervisor &) = delete;

  /// Launches every `immediate` entry, in list order, with no readiness wait.
  /// Each child runs with the core-file size limit's soft value at
  /// `core_dump_limit_bytes` (0 by default: core dumps off).
  void StartImmediate();
  /// Launches every `after_baseline` entry, in list order, once.
  void StartAfterBaseline();
  [[nodiscard]] bool after_baseline_started() const noexcept {
    return after_baseline_started_;
  }
  /// One `waitpid(WNOHANG)` per running child; the first exit found.
  [[nodiscard]] std::optional<ApplicationProcessExit> PollExit();
  /// Reverse list order: SIGTERM, then wait for the exit with no time limit
  /// (OD-187 (1)), logging the wait and the exit. While `force_requested`
  /// answers true, each process still running is killed (SIGKILL) with one
  /// typed line. Returns true when it killed a process.
  [[nodiscard]] bool
  StopAll(const std::function<bool()> &force_requested) noexcept;

private:
  struct Running final {
    std::size_t spec_index;
    pid_t pid;
  };

  void Launch(std::size_t spec_index);

  std::vector<ApplicationProcessSpec> specs_;
  std::string application_start_text_;
  std::uint64_t core_dump_limit_bytes_{};
  std::vector<Running> running_;
  bool after_baseline_started_{};
};

}  // namespace manyforge::headless

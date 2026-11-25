#pragma once

#include <cstdint>
#include <ctime>
#include <sstream>
#include <string>
#include <string_view>
#include <sys/types.h>
#include <utility>

/// Console lines and fatal-signal evidence of the ManyForge C++ processes
/// (Manager, execution host, candidate worker). POSIX only, no ROS.
///
/// A line is one JSON object with the keys the Python processes use:
/// {"timestamp":"<UTC ISO-8601, microseconds, Z>","mono_ns":<CLOCK_MONOTONIC>,
///  "level":"INFO|WARNING|ERROR","process":"<role>","pid":<pid>,
///  "event":"<text>","code":"<typed code>"} (`code` only when one is given).
/// Each line is written to stderr with one write(2) call. Not for
/// time-critical paths that do not already print.
namespace manyforge::core {

enum class LogSeverity { kInfo, kWarning, kError };

/// Names this process on every line ("manager", "execution_host", ...).
/// Called once, first in main, before any other thread starts.
void SetProcessRole(std::string_view role);

/// Writes one line to stderr.
void LogLine(LogSeverity severity, std::string_view event,
             std::string_view code = {});

/// The text of one line, newline included (what LogLine writes).
[[nodiscard]] std::string FormatLogLine(LogSeverity severity,
                                        std::string_view event,
                                        std::string_view code,
                                        std::string_view role, pid_t pid,
                                        const timespec &realtime,
                                        std::int64_t mono_ns);

/// Joins the parts as `std::ostream <<` prints them (the event text of a line
/// that used to be written with `std::cerr <<`).
template <typename... Parts>
[[nodiscard]] std::string LogText(const Parts &...parts) {
  std::ostringstream text;
  (text << ... << parts);
  return std::move(text).str();
}

/// Installs the fatal-signal and terminate evidence: on SIGSEGV, SIGABRT,
/// SIGBUS, SIGFPE or SIGILL one line and a backtrace go to stderr, then the
/// signal is raised again with its default action, so the process ends as it
/// would without the handler (same signal, same core dump policy). The
/// terminate handler prints the active exception, then calls std::abort()
/// as the default handler does. Called once, first in main, after
/// SetProcessRole.
void InstallCrashEvidence();

} // namespace manyforge::core

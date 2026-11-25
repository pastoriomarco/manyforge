#include <manyforge_core/process_diagnostics.hpp>

#include <algorithm>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <cxxabi.h>
#include <exception>
#include <execinfo.h>
#include <typeinfo>
#include <unistd.h>

namespace manyforge::core {
namespace {

// The role as given (truncated), for LogLine. Written once in main before
// any other thread starts, read afterwards.
constexpr std::size_t kRoleCapacity = 64U;
char g_role[kRoleCapacity] = {};
std::size_t g_role_size = 0U;

// What the fatal-signal handler needs, prepared at install time: the line's
// start up to the pid, with the escaped role.
char g_crash_prefix[64U + 6U * kRoleCapacity] = {};
std::size_t g_crash_prefix_size = 0U;
// The handler runs on its own stack, so a stack overflow on the main thread
// still leaves room to print.
alignas(16) char g_signal_stack[64U * 1024U];

constexpr int kFatalSignals[] = {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL};

/// Appends into a fixed buffer; async-signal-safe. Text that does not fit is
/// cut.
struct FixedText final {
  char *data;
  std::size_t capacity;
  std::size_t size = 0U;

  void Append(std::string_view text) noexcept {
    const auto count = std::min(text.size(), capacity - size);
    std::memcpy(data + size, text.data(), count);
    size += count;
  }
  void AppendUnsigned(std::uint64_t value, int width = 0) noexcept {
    char digits[20];
    int count = 0;
    do {
      digits[count++] = static_cast<char>('0' + value % 10U);
      value /= 10U;
    } while (value != 0U);
    for (int pad = count; pad < width; ++pad) {
      Append("0");
    }
    while (count > 0) {
      const char digit = digits[--count];
      Append(std::string_view(&digit, 1U));
    }
  }
  void AppendSigned(std::int64_t value) noexcept {
    if (value < 0) {
      Append("-");
      AppendUnsigned(static_cast<std::uint64_t>(-(value + 1)) + 1U);
    } else {
      AppendUnsigned(static_cast<std::uint64_t>(value));
    }
  }
};

/// UTC ISO-8601 with microseconds and `Z`, from arithmetic only (no gmtime,
/// so the signal handler can use it). Days to civil date: H. Hinnant's
/// days_from_civil inverse.
void AppendTimestamp(FixedText &out, const timespec &realtime) noexcept {
  const std::int64_t seconds = realtime.tv_sec;
  std::int64_t days = seconds / 86400;
  std::int64_t of_day = seconds % 86400;
  if (of_day < 0) {
    of_day += 86400;
    --days;
  }
  days += 719468;
  const std::int64_t era = (days >= 0 ? days : days - 146096) / 146097;
  const auto day_of_era = static_cast<std::uint64_t>(days - era * 146097);
  const std::uint64_t year_of_era =
      (day_of_era - day_of_era / 1460U + day_of_era / 36524U -
       day_of_era / 146096U) /
      365U;
  const std::uint64_t day_of_year =
      day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
  const std::uint64_t month_index = (5U * day_of_year + 2U) / 153U;
  const std::uint64_t day = day_of_year - (153U * month_index + 2U) / 5U + 1U;
  const std::uint64_t month =
      month_index < 10U ? month_index + 3U : month_index - 9U;
  const std::int64_t year = static_cast<std::int64_t>(year_of_era) +
                            era * 400 + (month <= 2U ? 1 : 0);
  out.AppendUnsigned(static_cast<std::uint64_t>(year), 4);
  out.Append("-");
  out.AppendUnsigned(month, 2);
  out.Append("-");
  out.AppendUnsigned(day, 2);
  out.Append("T");
  out.AppendUnsigned(static_cast<std::uint64_t>(of_day / 3600), 2);
  out.Append(":");
  out.AppendUnsigned(static_cast<std::uint64_t>(of_day % 3600 / 60), 2);
  out.Append(":");
  out.AppendUnsigned(static_cast<std::uint64_t>(of_day % 60), 2);
  out.Append(".");
  out.AppendUnsigned(static_cast<std::uint64_t>(realtime.tv_nsec / 1000), 6);
  out.Append("Z");
}

void AppendEscaped(std::string &out, std::string_view text) {
  static constexpr char kHex[] = "0123456789abcdef";
  for (const char character : text) {
    switch (character) {
    case '"':
      out += "\\\"";
      break;
    case '\\':
      out += "\\\\";
      break;
    case '\n':
      out += "\\n";
      break;
    case '\r':
      out += "\\r";
      break;
    case '\t':
      out += "\\t";
      break;
    default:
      if (static_cast<unsigned char>(character) < 0x20U) {
        out += "\\u00";
        out += kHex[static_cast<unsigned char>(character) >> 4U];
        out += kHex[static_cast<unsigned char>(character) & 0x0fU];
      } else {
        out += character;
      }
    }
  }
}

std::string_view LevelText(LogSeverity severity) {
  switch (severity) {
  case LogSeverity::kInfo:
    return "INFO";
  case LogSeverity::kWarning:
    return "WARNING";
  case LogSeverity::kError:
    return "ERROR";
  }
  return "ERROR";
}

std::string_view SignalName(int number) noexcept {
  switch (number) {
  case SIGSEGV:
    return "SIGSEGV";
  case SIGABRT:
    return "SIGABRT";
  case SIGBUS:
    return "SIGBUS";
  case SIGFPE:
    return "SIGFPE";
  case SIGILL:
    return "SIGILL";
  default:
    return "signal";
  }
}

void WriteStderr(const char *data, std::size_t size) noexcept {
  // One write; diagnostics never retry or alter the outcome.
  const auto written = ::write(STDERR_FILENO, data, size);
  static_cast<void>(written);
}

/// Async-signal-safe: fixed buffers, clock_gettime, getpid, write,
/// backtrace_symbols_fd (backtrace's libgcc is loaded at install), raise.
void OnFatalSignal(int number) {
  timespec realtime{};
  timespec monotonic{};
  ::clock_gettime(CLOCK_REALTIME, &realtime);
  ::clock_gettime(CLOCK_MONOTONIC, &monotonic);
  char buffer[sizeof(g_crash_prefix) + 256U];
  FixedText line{buffer, sizeof(buffer)};
  line.Append("{\"timestamp\":\"");
  AppendTimestamp(line, realtime);
  line.Append("\",\"mono_ns\":");
  line.AppendSigned(static_cast<std::int64_t>(monotonic.tv_sec) *
                        1000000000 +
                    monotonic.tv_nsec);
  line.Append(std::string_view(g_crash_prefix, g_crash_prefix_size));
  line.AppendSigned(::getpid());
  line.Append(",\"event\":\"fatal signal ");
  line.AppendUnsigned(static_cast<std::uint64_t>(number));
  line.Append(" (");
  line.Append(SignalName(number));
  line.Append("), backtrace follows\"}\n");
  WriteStderr(line.data, line.size);
  void *frames[64];
  const int depth = ::backtrace(frames, 64);
  ::backtrace_symbols_fd(frames, depth, STDERR_FILENO);
  // SA_RESETHAND restored the default action; the signal is delivered again
  // as soon as this handler returns, so the process ends as it would have.
  ::raise(number);
}

[[noreturn]] void OnTerminate() noexcept {
  std::string event = "terminate called";
  if (const std::type_info *type = abi::__cxa_current_exception_type()) {
    int status = 0;
    char *demangled =
        abi::__cxa_demangle(type->name(), nullptr, nullptr, &status);
    event += " after throwing an instance of '";
    event += status == 0 && demangled != nullptr ? demangled : type->name();
    event += "'";
    std::free(demangled);
    try {
      throw;
    } catch (const std::exception &error) {
      event += ": ";
      event += error.what();
    } catch (...) {
    }
  }
  LogLine(LogSeverity::kError, event);
  std::abort();
}

} // namespace

void SetProcessRole(std::string_view role) {
  g_role_size = std::min(role.size(), kRoleCapacity);
  std::memcpy(g_role, role.data(), g_role_size);
}

std::string FormatLogLine(LogSeverity severity, std::string_view event,
                          std::string_view code, std::string_view role,
                          pid_t pid, const timespec &realtime,
                          std::int64_t mono_ns) {
  char head[96];
  FixedText time{head, sizeof(head)};
  AppendTimestamp(time, realtime);
  std::string line;
  line.reserve(128U + role.size() + event.size() + code.size());
  line += "{\"timestamp\":\"";
  line.append(time.data, time.size);
  line += "\",\"mono_ns\":";
  line += std::to_string(mono_ns);
  line += ",\"level\":\"";
  line += LevelText(severity);
  line += "\",\"process\":\"";
  AppendEscaped(line, role);
  line += "\",\"pid\":";
  line += std::to_string(pid);
  line += ",\"event\":\"";
  AppendEscaped(line, event);
  line += '"';
  if (!code.empty()) {
    line += ",\"code\":\"";
    AppendEscaped(line, code);
    line += '"';
  }
  line += "}\n";
  return line;
}

void LogLine(LogSeverity severity, std::string_view event,
             std::string_view code) {
  timespec realtime{};
  timespec monotonic{};
  ::clock_gettime(CLOCK_REALTIME, &realtime);
  ::clock_gettime(CLOCK_MONOTONIC, &monotonic);
  const auto line = FormatLogLine(
      severity, event, code, std::string_view(g_role, g_role_size), ::getpid(),
      realtime,
      static_cast<std::int64_t>(monotonic.tv_sec) * 1000000000 +
          monotonic.tv_nsec);
  WriteStderr(line.data(), line.size());
}

void InstallCrashEvidence() {
  std::string prefix = ",\"level\":\"ERROR\",\"process\":\"";
  AppendEscaped(prefix, std::string_view(g_role, g_role_size));
  prefix += "\",\"pid\":";
  g_crash_prefix_size = std::min(prefix.size(), sizeof(g_crash_prefix));
  std::memcpy(g_crash_prefix, prefix.data(), g_crash_prefix_size);

  // backtrace() loads libgcc on first use; do it now, not in the handler.
  void *frame = nullptr;
  static_cast<void>(::backtrace(&frame, 1));

  stack_t stack{};
  stack.ss_sp = g_signal_stack;
  stack.ss_size = sizeof(g_signal_stack);
  stack.ss_flags = 0;
  static_cast<void>(::sigaltstack(&stack, nullptr));

  struct sigaction action {};
  action.sa_handler = OnFatalSignal;
  sigemptyset(&action.sa_mask);
  action.sa_flags = SA_RESETHAND | SA_ONSTACK;
  for (const int number : kFatalSignals) {
    static_cast<void>(::sigaction(number, &action, nullptr));
  }
  std::set_terminate(OnTerminate);
}

} // namespace manyforge::core

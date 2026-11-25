#include <gtest/gtest.h>
#include <manyforge_core/process_diagnostics.hpp>

#include <csignal>
#include <stdexcept>
#include <sys/resource.h>

namespace manyforge::core {
namespace {

TEST(ProcessDiagnostics, FormatsOneJsonLineWithTheSharedKeys) {
  EXPECT_EQ(FormatLogLine(LogSeverity::kWarning, "Manager apply pending",
                          "manager_apply_pending", "manager", 4242,
                          timespec{1795515072, 1000}, 987654321),
            "{\"timestamp\":\"2026-11-24T10:11:12.000001Z\","
            "\"mono_ns\":987654321,\"level\":\"WARNING\","
            "\"process\":\"manager\",\"pid\":4242,"
            "\"event\":\"Manager apply pending\","
            "\"code\":\"manager_apply_pending\"}\n");
}

TEST(ProcessDiagnostics, OmitsAnEmptyCode) {
  EXPECT_EQ(FormatLogLine(LogSeverity::kInfo, "ready", {}, "execution_host", 7,
                          timespec{0, 123456789}, 0),
            "{\"timestamp\":\"1970-01-01T00:00:00.123456Z\",\"mono_ns\":0,"
            "\"level\":\"INFO\",\"process\":\"execution_host\",\"pid\":7,"
            "\"event\":\"ready\"}\n");
}

TEST(ProcessDiagnostics, FormatsCalendarEdges) {
  const auto timestamp = [](timespec realtime) {
    const auto line =
        FormatLogLine(LogSeverity::kError, "x", {}, "r", 1, realtime, 1);
    return line.substr(14U, 27U);
  };
  EXPECT_EQ(timestamp({1709251199, 999999999}), "2024-02-29T23:59:59.999999Z");
  EXPECT_EQ(timestamp({4107542400, 0}), "2100-03-01T00:00:00.000000Z");
}

TEST(ProcessDiagnostics, EscapesEventCodeAndRole) {
  const auto line =
      FormatLogLine(LogSeverity::kError,
                    "a \"q\" \\ b\nc\td\x01" "e\x1f",
                    "c\"", "r\\", 1, timespec{0, 0}, 1);
  EXPECT_NE(line.find(R"("event":"a \"q\" \\ b\nc\td\u0001e\u001f")"),
            std::string::npos)
      << line;
  EXPECT_NE(line.find(R"("code":"c\"")"), std::string::npos) << line;
  EXPECT_NE(line.find(R"("process":"r\\")"), std::string::npos) << line;
  EXPECT_EQ(line.find('\n'), line.size() - 1U) << line;
}

TEST(ProcessDiagnostics, LogTextJoinsAsAStreamPrints) {
  EXPECT_EQ(LogText("exited with ", "signal ", 9, ' ', 1.5),
            "exited with signal 9 1.5");
}

void CrashWith(int signal) {
  rlimit no_core{0U, 0U};
  static_cast<void>(::setrlimit(RLIMIT_CORE, &no_core));
  SetProcessRole("crash_test");
  InstallCrashEvidence();
  std::raise(signal);
}

TEST(ProcessDiagnosticsDeathTest, FatalSignalPrintsALineAndABacktraceThenDies) {
  EXPECT_EXIT(CrashWith(SIGSEGV), testing::KilledBySignal(SIGSEGV),
              "\"process\":\"crash_test\",\"pid\":[0-9]+,"
              "\"event\":\"fatal signal 11 \\(SIGSEGV\\), backtrace follows\"\\}"
              "\n.*\\[0x[0-9a-f]+\\]");
  EXPECT_EXIT(CrashWith(SIGBUS), testing::KilledBySignal(SIGBUS),
              "fatal signal [0-9]+ \\(SIGBUS\\)");
}

[[noreturn]] void Throw() { throw std::runtime_error("boom \"quoted\""); }

// An exception leaving a noexcept function calls std::terminate.
void ThrowThroughNoexcept() noexcept {
  rlimit no_core{0U, 0U};
  static_cast<void>(::setrlimit(RLIMIT_CORE, &no_core));
  SetProcessRole("crash_test");
  InstallCrashEvidence();
  Throw();
}

TEST(ProcessDiagnosticsDeathTest, TerminatePrintsTheExceptionThenAborts) {
  EXPECT_EXIT(ThrowThroughNoexcept(), testing::KilledBySignal(SIGABRT),
              "\"level\":\"ERROR\",\"process\":\"crash_test\",\"pid\":[0-9]+,"
              "\"event\":\"terminate called after throwing an instance of "
              "'std::runtime_error': boom \\\\\"quoted\\\\\"\"\\}\n"
              ".*fatal signal 6 \\(SIGABRT\\), backtrace follows");
}

} // namespace
} // namespace manyforge::core

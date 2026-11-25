#include "application_process_supervisor.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/resource.h>
#include <unistd.h>

namespace manyforge::headless {
namespace {

namespace core = manyforge::core;

constexpr core::ApplicationStartId kStartId{0x3f9a00000000c0deULL};

std::string Document(const std::string &processes) {
  return R"({"api_version":"manyforge.application_processes.v1","processes":[)" +
         processes + "]}";
}

std::string Entry(const std::string &role, const std::string &argv,
                  const std::string &start = "immediate",
                  const std::string &environment = "{}") {
  return R"({"role":")" + role + R"(","argv":)" + argv +
         R"(,"environment":)" + environment + R"(,"start":")" + start +
         R"("})";
}

std::filesystem::path TemporaryDirectory() {
  std::string pattern =
      (std::filesystem::temp_directory_path() / "application-processes-XXXXXX")
          .string();
  if (::mkdtemp(pattern.data()) == nullptr)
    throw std::runtime_error("temporary directory");
  return pattern;
}

std::optional<ApplicationProcessExit>
WaitForExit(ApplicationProcessSupervisor &supervisor) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    if (auto exit = supervisor.PollExit()) return exit;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return std::nullopt;
}

TEST(ApplicationProcessList, DecodesTheThreeRolesInListOrder) {
  const auto specs = ParseApplicationProcessList(Document(
      Entry("headless", R"(["/usr/bin/headless","--port","1"])") + "," +
      Entry("program_runner", R"(["/usr/bin/runner"])") + "," +
      Entry("composer", R"(["/usr/bin/composer"])", "after_baseline",
            R"({"MANYFORGE_STATE_DIR":"/data"})")));
  ASSERT_EQ(specs.size(), 3U);
  EXPECT_EQ(specs[0].role, core::ApplicationRole::kHeadless);
  EXPECT_EQ(specs[0].argv,
            (std::vector<std::string>{"/usr/bin/headless", "--port", "1"}));
  EXPECT_FALSE(specs[0].after_baseline);
  EXPECT_EQ(specs[1].role, core::ApplicationRole::kProgramRunner);
  EXPECT_EQ(specs[2].role, core::ApplicationRole::kComposer);
  EXPECT_TRUE(specs[2].after_baseline);
  ASSERT_EQ(specs[2].environment.size(), 1U);
  EXPECT_EQ(specs[2].environment.front().first, "MANYFORGE_STATE_DIR");
}

TEST(ApplicationProcessList, RefusesEveryOutOfContractDocument) {
  const std::vector<std::string> invalid{
      R"({"api_version":"manyforge.application_processes.v0","processes":[]})",
      R"({"api_version":"manyforge.application_processes.v1","processes":[],"extra":1})",
      Document(R"({"role":"headless","argv":["/x"],"environment":{},"start":"immediate","name":"h"})"),
      Document(Entry("manager", R"(["/x"])")),
      Document(Entry("execution_host", R"(["/x"])")),
      Document(Entry("headless", R"(["/x"])") + "," + Entry("headless", R"(["/y"])")),
      Document(Entry("headless", R"(["relative"])")),
      Document(Entry("headless", R"([])")),
      Document(Entry("headless", R"(["/x",""])")),
      Document(Entry("headless", R"(["/x"])", "later")),
      Document(Entry("headless", R"(["/x"])", "immediate", R"({"lower":"v"})")),
  };
  for (const auto &document : invalid) {
    EXPECT_ANY_THROW(static_cast<void>(ParseApplicationProcessList(document)))
        << document;
  }
}

TEST(ApplicationProcessSupervisor, HandsTheStartIdentityAndEnvironmentAtLaunch) {
  const auto root = TemporaryDirectory();
  const auto output = root / "arguments";
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kHeadless,
        {"/bin/sh", "-c",
         "printf '%s\\n' \"$@\" \"$EXTRA\" > " + output.string(), "sh"},
        {{"EXTRA", "added"}},
        false}},
      kStartId);
  supervisor.StartImmediate();
  const auto exit = WaitForExit(supervisor);
  ASSERT_TRUE(exit.has_value());
  EXPECT_EQ(exit->role, core::ApplicationRole::kHeadless);
  EXPECT_FALSE(exit->signaled);
  EXPECT_EQ(exit->value, 0);
  EXPECT_GT(exit->pid, 0);
  EXPECT_FALSE(exit->core_dumped);
  std::ifstream stream(output);
  std::stringstream content;
  content << stream.rdbuf();
  EXPECT_EQ(content.str(),
            "--application-start-id\n3f9a00000000c0de\nadded\n");
  std::filesystem::remove_all(root);
}

TEST(ApplicationProcessSupervisor, StartsEveryProcessWithCoreDumpsOff) {
  const auto root = TemporaryDirectory();
  const auto output = root / "core-limit";
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kHeadless,
        {"/bin/sh", "-c", "ulimit -c > " + output.string() + "; kill -TERM $$"},
        {},
        false}},
      kStartId);
  supervisor.StartImmediate();
  const auto exit = WaitForExit(supervisor);
  ASSERT_TRUE(exit.has_value());
  EXPECT_TRUE(exit->signaled);
  EXPECT_EQ(exit->value, SIGTERM);
  EXPECT_FALSE(exit->core_dumped);
  std::ifstream stream(output);
  std::stringstream content;
  content << stream.rdbuf();
  EXPECT_EQ(content.str(), "0\n");
  std::filesystem::remove_all(root);
}

TEST(ApplicationProcessSupervisor, StartsEveryProcessWithTheSiteCoreDumpLimit) {
  const auto root = TemporaryDirectory();
  const auto output = root / "core-limit";
  constexpr std::uint64_t kLimit = 1048576U;
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kHeadless,
        {"/bin/sh", "-c",
         "grep '^Max core file size' /proc/self/limits > " + output.string()},
        {},
        false}},
      kStartId, kLimit);
  supervisor.StartImmediate();
  const auto exit = WaitForExit(supervisor);
  ASSERT_TRUE(exit.has_value());
  EXPECT_FALSE(exit->signaled);
  EXPECT_EQ(exit->value, 0);
  struct rlimit own {};
  ASSERT_EQ(::getrlimit(RLIMIT_CORE, &own), 0);
  // Only the soft value changes, and never above the hard limit.
  const auto soft = std::min(static_cast<rlim_t>(kLimit), own.rlim_max);
  const std::string hard = own.rlim_max == RLIM_INFINITY
                               ? std::string("unlimited")
                               : std::to_string(own.rlim_max);
  std::ifstream stream(output);
  std::string max, core, file, size, soft_text, hard_text;
  stream >> max >> core >> file >> size >> soft_text >> hard_text;
  EXPECT_EQ(soft_text, std::to_string(soft));
  EXPECT_EQ(hard_text, hard);
  std::filesystem::remove_all(root);
}

TEST(ApplicationProcessSupervisor, ReportsAnExitAndStartsAfterBaselineOnlyOnce) {
  const auto root = TemporaryDirectory();
  const auto marker = root / "composer.started";
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kHeadless, {"/bin/sh", "-c", "exit 3"}, {}, false},
       {core::ApplicationRole::kComposer,
        {"/bin/sh", "-c", "echo x >> " + marker.string() + "; sleep 30"},
        {},
        true}},
      kStartId);
  supervisor.StartImmediate();
  EXPECT_FALSE(std::filesystem::exists(marker));
  const auto exit = WaitForExit(supervisor);
  ASSERT_TRUE(exit.has_value());
  EXPECT_EQ(exit->role, core::ApplicationRole::kHeadless);
  EXPECT_EQ(exit->value, 3);
  supervisor.StartAfterBaseline();
  supervisor.StartAfterBaseline();
  EXPECT_TRUE(supervisor.after_baseline_started());
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(marker) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // A process that ends on SIGTERM is not killed.
  EXPECT_FALSE(supervisor.StopAll([] { return false; }));
  std::ifstream stream(marker);
  std::stringstream content;
  content << stream.rdbuf();
  EXPECT_EQ(content.str(), "x\n");
  EXPECT_FALSE(supervisor.PollExit().has_value());
  std::filesystem::remove_all(root);
}

bool WaitForFile(const std::filesystem::path &path) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!std::filesystem::exists(path)) {
    if (std::chrono::steady_clock::now() >= deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return true;
}

std::string ReadFile(const std::filesystem::path &path) {
  std::ifstream stream(path);
  std::stringstream content;
  content << stream.rdbuf();
  return content.str();
}

// OD-187 (1): no time limit; only a second stop request (the force
// predicate) kills a process that ignores SIGTERM, with one typed line.
TEST(ApplicationProcessSupervisor,
     StopsInReverseListOrderAndKillsOnlyWhenForced) {
  const auto root = TemporaryDirectory();
  const auto order = root / "order";
  const auto headless_ready = root / "headless.ready";
  const auto runner_ready = root / "runner.ready";
  // The first ignores SIGTERM: it is stopped last and ends only when forced.
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kHeadless,
        {"/bin/sh", "-c",
         "trap 'echo headless >> " + order.string() + "' TERM; : > " +
             headless_ready.string() + "; while :; do sleep 0.05; done"},
        {},
        false},
       {core::ApplicationRole::kProgramRunner,
        {"/bin/sh", "-c",
         "trap 'echo program_runner >> " + order.string() +
             "; exit 0' TERM; : > " + runner_ready.string() +
             "; while :; do sleep 0.05; done"},
        {},
        false}},
      kStartId);
  supervisor.StartImmediate();
  ASSERT_TRUE(WaitForFile(headless_ready));
  ASSERT_TRUE(WaitForFile(runner_ready));
  // The second request comes once Headless has seen its SIGTERM and stayed.
  testing::internal::CaptureStderr();
  const bool killed = supervisor.StopAll(
      [&] { return ReadFile(order) == "program_runner\nheadless\n"; });
  const auto log = testing::internal::GetCapturedStderr();
  EXPECT_TRUE(killed);
  EXPECT_EQ(ReadFile(order), "program_runner\nheadless\n");
  EXPECT_NE(log.find("application stop: waiting at program_runner: pid="),
            std::string::npos);
  EXPECT_NE(log.find("application stop: program_runner exited pid="),
            std::string::npos);
  EXPECT_NE(log.find("{\"code\":\"application_process_killed\",\"role\":"
                     "\"headless\",\"pid\":"),
            std::string::npos);
  EXPECT_EQ(log.find("\"role\":\"program_runner\""), std::string::npos);
  EXPECT_NE(log.find("application stop: headless exited pid="),
            std::string::npos);
  EXPECT_NE(log.find(" signal=9"), std::string::npos);
  std::filesystem::remove_all(root);
}

TEST(ApplicationProcessSupervisor, AForcedStopKillsWithoutSigterm) {
  const auto root = TemporaryDirectory();
  const auto order = root / "order";
  const auto ready = root / "runner.ready";
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kProgramRunner,
        {"/bin/sh", "-c",
         "trap 'echo program_runner >> " + order.string() +
             "; exit 0' TERM; : > " + ready.string() +
             "; while :; do sleep 0.05; done"},
        {},
        false}},
      kStartId);
  supervisor.StartImmediate();
  ASSERT_TRUE(WaitForFile(ready));
  EXPECT_TRUE(supervisor.StopAll([] { return true; }));
  EXPECT_FALSE(std::filesystem::exists(order));
  EXPECT_FALSE(supervisor.PollExit().has_value());
  std::filesystem::remove_all(root);
}

TEST(ApplicationProcessSupervisor, AFailedExecRefusesTheLaunch) {
  ApplicationProcessSupervisor supervisor(
      {{core::ApplicationRole::kComposer, {"/nonexistent/composer"}, {}, false}},
      kStartId);
  EXPECT_THROW(supervisor.StartImmediate(), std::runtime_error);
  EXPECT_FALSE(supervisor.PollExit().has_value());
}

}  // namespace
}  // namespace manyforge::headless

#include <gtest/gtest.h>

#include "execution_host_posix.hpp"

#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::roboplan_backend::host_detail {
namespace {

// The Manager hands the application-start identity `A` to the execution host
// at launch as `--application-start-id <16 hex>` (OD-122 (3), (7); contract
// 1.1). A missing or invalid value is a start-up usage error.

std::vector<std::string> Arguments(const std::string &start_id_option,
                                   const std::string &start_id) {
  const std::string digest = "sha256:" + std::string(64U, 'a');
  std::vector<std::string> arguments{
      "manyforge_execution_host",
      "--manyforge-managed-runtime-channel-fd", "3",
      "--manyforge-managed-liveness-channel-fd", "4",
      "--manyforge-managed-package-root-fd", std::to_string(kManagedPackageRootFd),
      "--manyforge-managed-program-artifact-root-fd",
      std::to_string(kManagedProgramArtifactRootFd),
      "--manyforge-managed-manager-run-root", "/nonexistent/manager-run",
      "--manyforge-managed-execution-state-root", "/nonexistent/execution-state",
      "--manyforge-managed-execution-run-root", "/nonexistent/execution-run",
      "--manyforge-managed-candidate-worker", "/nonexistent/worker",
      "--manyforge-managed-cell-id", "cell.1",
      "--manyforge-managed-retained-bundle-id", digest,
      "--manyforge-managed-graph-revision", "graph.1",
      "--runtime-unit-id", "robot_cell.ros2_control_execution",
      "--product-identity", digest,
      "--configuration-identity", digest};
  if (!start_id_option.empty()) {
    arguments.push_back(start_id_option);
    arguments.push_back(start_id);
  }
  return arguments;
}

std::string ParseError(std::vector<std::string> arguments) {
  std::vector<char *> argv;
  for (auto &argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);
  try {
    static_cast<void>(ParseManagedRuntimeArguments(
        static_cast<int>(arguments.size()), argv.data()));
  } catch (const std::runtime_error &error) {
    return error.what();
  }
  return {};
}

TEST(ExecutionHostArguments, AcceptsTheApplicationStartIdentity) {
  // Parsing passes every identity check and stops only at the inherited
  // package descriptors, which this test does not provide.
  const auto error =
      ParseError(Arguments("--application-start-id", "3f9a00000000c0de"));
  EXPECT_NE(error.find("managed package-root descriptor"), std::string::npos)
      << error;
}

TEST(ExecutionHostArguments, RefusesAnInvalidApplicationStartIdentity) {
  for (const auto *value : {"0000000000000000", "3F9A00000000C0DE",
                            "3f9a00000000c0d", "3f9a00000000c0dee",
                            "execution-host-3f9a00000000c0de"}) {
    SCOPED_TRACE(value);
    const auto error = ParseError(Arguments("--application-start-id", value));
    EXPECT_NE(error.find("application-start identity is invalid"),
              std::string::npos)
        << error;
  }
}

TEST(ExecutionHostArguments, RefusesAMissingApplicationStartIdentity) {
  EXPECT_NE(ParseError(Arguments("", "")).find("managed runtime identity is invalid"),
            std::string::npos);
}

TEST(ExecutionHostArguments, RefusesTheRetiredRuntimeEpochOption) {
  EXPECT_NE(ParseError(Arguments("--runtime-epoch", "3f9a00000000c0de"))
                .find("unknown managed runtime option --runtime-epoch"),
            std::string::npos);
}

// OD-139: the host never reads the Program runner's cycle journal, so it has
// no option naming it.
TEST(ExecutionHostArguments, RefusesTheRetiredProgramCycleJournalOption) {
  auto arguments = Arguments("--application-start-id", "3f9a00000000c0de");
  arguments.push_back("--manyforge-managed-program-cycle-journal");
  arguments.push_back("/nonexistent/program-cycle.sqlite3");
  EXPECT_NE(ParseError(std::move(arguments))
                .find("unknown managed runtime option "
                      "--manyforge-managed-program-cycle-journal"),
            std::string::npos);
}

// OD-206: the host never reads the Manager's state folder, so it has no
// option naming it.
TEST(ExecutionHostArguments, RefusesTheRetiredManagerStateRootOption) {
  auto arguments = Arguments("--application-start-id", "3f9a00000000c0de");
  arguments.push_back("--manyforge-managed-manager-state-root");
  arguments.push_back("/nonexistent/manager-state");
  EXPECT_NE(ParseError(std::move(arguments))
                .find("unknown managed runtime option "
                      "--manyforge-managed-manager-state-root"),
            std::string::npos);
}

}  // namespace
}  // namespace manyforge::roboplan_backend::host_detail

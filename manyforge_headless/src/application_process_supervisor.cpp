#include "application_process_supervisor.hpp"

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/process_diagnostics.hpp>
#include <manyforge_deployment/artifact_codec.hpp>

#include <fcntl.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <iostream>
#include <set>
#include <stdexcept>
#include <system_error>
#include <thread>

extern char **environ;

namespace manyforge::headless {
namespace {

namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;
using content_identity::CanonicalValue;

constexpr std::size_t kMaximumArguments = 256U;
constexpr std::size_t kMaximumArgumentBytes = 4096U;
constexpr std::size_t kMaximumEnvironmentMembers = 64U;
constexpr std::size_t kMaximumEnvironmentValueBytes = 4096U;
constexpr int kChildSetupFailureStatus = 127;

[[noreturn]] void Invalid(const std::string &detail) {
  throw std::invalid_argument("application process list: " + detail);
}

[[noreturn]] void FailErrno(const std::string &context, const int error) {
  throw std::runtime_error(context + ": " +
                           std::system_error(error, std::generic_category())
                               .what());
}

void RequireExactMembers(const CanonicalValue &object,
                         const std::set<std::string_view> &allowed,
                         const std::string &label) {
  if (!object.is_object()) Invalid(label + " must be an object");
  for (const auto &[name, value] : object.as_object()) {
    static_cast<void>(value);
    if (!allowed.contains(name)) Invalid(label + " has unknown member " + name);
  }
}

const CanonicalValue &RequiredMember(const CanonicalValue &object,
                                     const std::string_view name,
                                     const std::string &label) {
  const auto *member = object.find_object_member(name);
  if (member == nullptr) Invalid(label + " lacks " + std::string(name));
  return *member;
}

bool PlainText(const std::string &text, const std::size_t maximum) {
  return !text.empty() && text.size() <= maximum &&
         text.find('\0') == std::string::npos;
}

bool EnvironmentName(const std::string &name) {
  if (name.empty()) return false;
  const auto first = name.front();
  if (!((first >= 'A' && first <= 'Z') || first == '_')) return false;
  return std::all_of(name.begin(), name.end(), [](const char character) {
    return (character >= 'A' && character <= 'Z') ||
           (character >= '0' && character <= '9') || character == '_';
  });
}

std::string RoleText(const core::ApplicationRole role) {
  return std::string(core::ApplicationRoleText(role));
}

}  // namespace

std::vector<ApplicationProcessSpec>
ParseApplicationProcessList(const std::string_view document) {
  const auto parsed = deployment::parse_strict_json_model_yaml(document);
  const auto &root = parsed.root();
  const std::string document_label = "document";
  RequireExactMembers(root, {"api_version", "processes"}, document_label);
  const auto &version = RequiredMember(root, "api_version", document_label);
  if (!version.is_string() ||
      version.as_string() != kApplicationProcessesApiVersion)
    Invalid("api_version must be " +
            std::string(kApplicationProcessesApiVersion));
  const auto &processes = RequiredMember(root, "processes", document_label);
  if (!processes.is_array()) Invalid("processes must be an array");
  std::vector<ApplicationProcessSpec> result;
  std::set<core::ApplicationRole> roles;
  for (const auto &entry : processes.as_array()) {
    const std::string label = "process " + std::to_string(result.size());
    RequireExactMembers(entry, {"role", "argv", "environment", "start"}, label);
    ApplicationProcessSpec spec;
    const auto &role = RequiredMember(entry, "role", label);
    const auto parsed_role =
        role.is_string() ? core::ParseApplicationRole(role.as_string())
                         : std::nullopt;
    if (!parsed_role || (*parsed_role != core::ApplicationRole::kHeadless &&
                         *parsed_role != core::ApplicationRole::kProgramRunner &&
                         *parsed_role != core::ApplicationRole::kComposer))
      Invalid(label + " role must be headless, program_runner or composer");
    if (!roles.insert(*parsed_role).second)
      Invalid(label + " repeats role " + RoleText(*parsed_role));
    spec.role = *parsed_role;
    const auto &argv = RequiredMember(entry, "argv", label);
    if (!argv.is_array() || argv.as_array().empty() ||
        argv.as_array().size() > kMaximumArguments)
      Invalid(label + " argv must hold 1 to 256 strings");
    for (const auto &argument : argv.as_array()) {
      if (!argument.is_string() ||
          !PlainText(argument.as_string(), kMaximumArgumentBytes))
        Invalid(label + " argv holds an empty, oversized or NUL argument");
      spec.argv.push_back(argument.as_string());
    }
    if (!std::filesystem::path(spec.argv.front()).is_absolute())
      Invalid(label + " argv[0] must be an absolute path");
    const auto &environment = RequiredMember(entry, "environment", label);
    if (!environment.is_object() ||
        environment.as_object().size() > kMaximumEnvironmentMembers)
      Invalid(label + " environment must be an object of at most 64 members");
    for (const auto &[name, value] : environment.as_object()) {
      if (!EnvironmentName(name) || !value.is_string() ||
          value.as_string().size() > kMaximumEnvironmentValueBytes ||
          value.as_string().find('\0') != std::string::npos)
        Invalid(label + " environment member " + name + " is invalid");
      spec.environment.emplace_back(name, value.as_string());
    }
    const auto &start = RequiredMember(entry, "start", label);
    if (!start.is_string() || (start.as_string() != "immediate" &&
                               start.as_string() != "after_baseline"))
      Invalid(label + " start must be immediate or after_baseline");
    spec.after_baseline = start.as_string() == "after_baseline";
    result.push_back(std::move(spec));
  }
  return result;
}

ApplicationProcessSupervisor::ApplicationProcessSupervisor(
    std::vector<ApplicationProcessSpec> specs,
    const core::ApplicationStartId application_start_id,
    const std::uint64_t core_dump_limit_bytes)
    : specs_(std::move(specs)), core_dump_limit_bytes_(core_dump_limit_bytes) {
  const auto text = core::FormatApplicationStartId(application_start_id);
  application_start_text_.assign(text.data(), text.size());
  running_.reserve(specs_.size());
}

ApplicationProcessSupervisor::~ApplicationProcessSupervisor() {
  static_cast<void>(StopAll([] { return false; }));
}

void ApplicationProcessSupervisor::StartImmediate() {
  for (std::size_t index = 0U; index < specs_.size(); ++index)
    if (!specs_[index].after_baseline) Launch(index);
}

void ApplicationProcessSupervisor::StartAfterBaseline() {
  if (after_baseline_started_) return;
  after_baseline_started_ = true;
  for (std::size_t index = 0U; index < specs_.size(); ++index)
    if (specs_[index].after_baseline) Launch(index);
}

void ApplicationProcessSupervisor::Launch(const std::size_t spec_index) {
  const auto &spec = specs_[spec_index];
  // Everything the child needs is built before fork: between fork and exec
  // the child makes only async-signal-safe calls (as EnterChild does).
  std::vector<std::string> arguments = spec.argv;
  arguments.emplace_back("--application-start-id");
  arguments.push_back(application_start_text_);
  std::vector<char *> argv;
  argv.reserve(arguments.size() + 1U);
  for (auto &argument : arguments) argv.push_back(argument.data());
  argv.push_back(nullptr);

  std::vector<std::string> environment_storage;
  for (char **entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view variable(*entry);
    const auto name = variable.substr(0U, variable.find('='));
    if (std::none_of(spec.environment.begin(), spec.environment.end(),
                     [&](const auto &added) { return added.first == name; }))
      environment_storage.emplace_back(variable);
  }
  for (const auto &[name, value] : spec.environment)
    environment_storage.push_back(name + "=" + value);
  std::vector<char *> envp;
  envp.reserve(environment_storage.size() + 1U);
  for (auto &variable : environment_storage) envp.push_back(variable.data());
  envp.push_back(nullptr);

  std::array<int, 2> exec_pipe{-1, -1};
  if (::pipe2(exec_pipe.data(), O_CLOEXEC) != 0)
    FailErrno("create the launch status pipe of " + RoleText(spec.role), errno);
  const pid_t parent = ::getpid();
  const pid_t pid = ::fork();
  if (pid < 0) {
    const int error = errno;
    ::close(exec_pipe[0]);
    ::close(exec_pipe[1]);
    FailErrno("fork " + RoleText(spec.role), error);
  }
  if (pid == 0) {
    ::close(exec_pipe[0]);
    int error = 0;
    struct sigaction default_action {};
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigset_t empty_mask;
    sigemptyset(&empty_mask);
    // The core-file size limit's soft value is the site's (0, core dumps
    // off, by default), at most the hard limit, which is kept.
    struct rlimit core_limit {};
    const bool core_limit_read = ::getrlimit(RLIMIT_CORE, &core_limit) == 0;
    core_limit.rlim_cur =
        std::min(static_cast<rlim_t>(core_dump_limit_bytes_), core_limit.rlim_max);
    if (::prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 ||
        ::sigaction(SIGPIPE, &default_action, nullptr) != 0 ||
        ::sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0 ||
        !core_limit_read || ::setrlimit(RLIMIT_CORE, &core_limit) != 0) {
      error = errno;
    } else if (::getppid() != parent) {
      error = ESRCH;
    } else {
      ::execve(argv[0], argv.data(), envp.data());
      error = errno;
    }
    // A pipe write of one int is atomic; only EINTR is retried. If it still
    // fails, the parent sees exec success and then this child's setup-failure
    // exit status.
    ssize_t written = -1;
    do {
      written = ::write(exec_pipe[1], &error, sizeof(error));
    } while (written < 0 && errno == EINTR);
    ::_exit(kChildSetupFailureStatus);
  }
  ::close(exec_pipe[1]);
  int child_error = 0;
  ssize_t count = -1;
  do {
    count = ::read(exec_pipe[0], &child_error, sizeof(child_error));
  } while (count < 0 && errno == EINTR);
  ::close(exec_pipe[0]);
  if (count != 0) {
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    FailErrno("start " + RoleText(spec.role),
              count == static_cast<ssize_t>(sizeof(child_error)) ? child_error
                                                                 : EIO);
  }
  running_.push_back({spec_index, pid});
  core::LogLine(core::LogSeverity::kInfo, core::LogText("started ", RoleText(spec.role),
                                   " pid=", pid));
}

std::optional<ApplicationProcessExit> ApplicationProcessSupervisor::PollExit() {
  for (auto child = running_.begin(); child != running_.end(); ++child) {
    int status = 0;
    pid_t result = -1;
    do {
      result = ::waitpid(child->pid, &status, WNOHANG);
    } while (result < 0 && errno == EINTR);
    if (result == 0) continue;
    if (result < 0) FailErrno("observe " + RoleText(specs_[child->spec_index].role), errno);
    ApplicationProcessExit exit{
        specs_[child->spec_index].role, WIFSIGNALED(status),
        WIFSIGNALED(status) ? WTERMSIG(status) : WEXITSTATUS(status),
        child->pid, WIFSIGNALED(status) && WCOREDUMP(status) != 0};
    running_.erase(child);
    return exit;
  }
  return std::nullopt;
}

bool ApplicationProcessSupervisor::StopAll(
    const std::function<bool()> &force_requested) noexcept {
  bool killed_any = false;
  // Reverse list order: Composer, the runner, Headless (build spec 3.3).
  while (!running_.empty()) {
    const auto child = running_.back();
    running_.pop_back();
    const auto role = core::ApplicationRoleText(specs_[child.spec_index].role);
    bool killed = false;
    const auto kill_child = [&] {
      static_cast<void>(::kill(child.pid, SIGKILL));
      std::cerr << "{\"code\":\"application_process_killed\",\"role\":\""
                << role << "\",\"pid\":" << child.pid << "}\n";
      killed = true;
      killed_any = true;
    };
    if (force_requested()) {
      kill_child();
    } else {
      static_cast<void>(::kill(child.pid, SIGTERM));
      core::LogLine(core::LogSeverity::kInfo, core::LogText("application stop: waiting at ", role,
                                      ": pid=", child.pid, " to exit"));
    }
    // No time limit: the exit (waitpid) ends the wait, or a second stop
    // request, which kills the process.
    while (true) {
      int status = 0;
      const auto result = ::waitpid(child.pid, &status, WNOHANG);
      if (result == child.pid) {
        core::LogLine(core::LogSeverity::kInfo,
                      core::LogText("application stop: ", role,
                                    " exited pid=", child.pid,
                                    WIFSIGNALED(status) ? " signal=" : " status=",
                                    WIFSIGNALED(status) ? WTERMSIG(status)
                                                        : WEXITSTATUS(status)));
        break;
      }
      if (result < 0 && errno != EINTR) break;
      if (!killed && force_requested()) {
        kill_child();
        continue;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
  return killed_any;
}

}  // namespace manyforge::headless

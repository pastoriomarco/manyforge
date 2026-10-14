#include <manyforge_roboplan/controller_launch_process.hpp>

#include <manyforge_core/time/ischeduler.hpp>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <iomanip>
#include <poll.h>
#include <pthread.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include <algorithm>
#include <set>
#include <sstream>

extern char **environ;

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &message) {
  throw ControllerLaunchProcessError("controller launch: " + message);
}

constexpr std::string_view kRos2 = "ros2";
constexpr std::string_view kBringupPackage = "manyforge_ros_bringup";
constexpr std::string_view kMockLaunchFile = "mock_control.launch.py";
constexpr std::string_view kIsaacLaunchFile =
    "isaac_topic_based_control.launch.py";

[[nodiscard]] int parent_death_signal(const ControllerLaunchCommand &command) {
  const auto &argv = command.argv;
  // Q116 applies only to these qualified ROS launch roles. Cell launches and
  // arbitrary controller commands retain their existing SIGTERM policy.
  if (argv.size() >= 4U && argv[0] == kRos2 && argv[1] == "launch" &&
      argv[2] == kBringupPackage &&
      ((command.lane == ControllerLaunchLane::kMock &&
        argv[3] == kMockLaunchFile) ||
       (command.lane == ControllerLaunchLane::kIsaac &&
        argv[3] == kIsaacLaunchFile)))
    return SIGINT;
  return SIGTERM;
}

[[nodiscard]] std::string argument_name(const std::string &argument) {
  const auto separator = argument.find(":=");
  // `ros2 launch` rejects an empty value before anything starts: an absent
  // value is an omitted argument, never `name:=`.
  if (separator == std::string::npos || separator == 0U ||
      separator + 2U == argument.size()) {
    fail("launch argument '" + argument + "' is not one name:=value pair");
  }
  return argument.substr(0U, separator);
}

[[nodiscard]] std::string
identity_of(const ControllerLaunchArtifacts &artifacts,
            const std::vector<std::string> &further) {
  std::ostringstream out;
  out << (artifacts.lane == ControllerLaunchLane::kMock ? "mock" : "isaac")
      << '\n'
      << artifacts.controllable_component_id << '\n'
      << artifacts.controller_profile << '\n'
      << artifacts.runtime_namespace << '\n'
      << artifacts.arm_controller_name << '\n'
      << artifacts.gripper_controller_name.value_or(std::string()) << '\n'
      << artifacts.frame_prefix << '\n'
      << artifacts.robot_description_sha256 << '\n'
      << artifacts.controllers_sha256 << '\n';
  for (const auto &argument : further) out << argument << '\n';
  return out.str();
}

void require_artifacts(const ControllerLaunchArtifacts &artifacts,
                       const ControllerLaunchLane lane) {
  if (artifacts.lane != lane || artifacts.controllable_component_id.empty() ||
      artifacts.runtime_namespace.empty() ||
      artifacts.robot_description_file.empty() ||
      artifacts.controllers_file.empty() ||
      artifacts.robot_description_sha256.empty() ||
      artifacts.controllers_sha256.empty()) {
    fail("component '" + artifacts.controllable_component_id +
         "' has no complete written artifacts for this lane");
  }
}

[[nodiscard]] std::optional<std::string>
resolve_executable(const std::string &name) {
  if (name.find('/') != std::string::npos) {
    return ::access(name.c_str(), X_OK) == 0 ? std::optional(name)
                                             : std::nullopt;
  }
  const char *path = std::getenv("PATH");
  if (path == nullptr) return std::nullopt;
  std::string_view rest(path);
  while (true) {
    const auto colon = rest.find(':');
    const auto directory = rest.substr(0U, colon);
    if (!directory.empty()) {
      auto candidate = std::string(directory) + "/" + name;
      struct stat status {};
      if (::stat(candidate.c_str(), &status) == 0 &&
          S_ISREG(status.st_mode) && ::access(candidate.c_str(), X_OK) == 0) {
        return candidate;
      }
    }
    if (colon == std::string_view::npos) break;
    rest.remove_prefix(colon + 1U);
  }
  return std::nullopt;
}

[[nodiscard]] std::string describe_status(const int status,
                                         const bool cell = false) {
  const std::string subject = cell ? "cell launch" : "controller launch";
  if (WIFEXITED(status)) {
    return subject + " exited with status " +
           std::to_string(WEXITSTATUS(status));
  }
  if (WIFSIGNALED(status)) {
    return subject + " was terminated by signal " +
           std::to_string(WTERMSIG(status));
  }
  return subject + " ended";
}


[[nodiscard]] std::string observed_utc_now() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto fraction =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds)
          .count();
  const auto whole = std::chrono::system_clock::to_time_t(seconds);
  std::tm utc{};
  if (::gmtime_r(&whole, &utc) == nullptr)
    return "unavailable";
  std::ostringstream out;
  out << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0')
      << std::setw(9) << fraction << 'Z';
  return out.str();
}

[[nodiscard]] std::string log_field(const std::string_view value) {
  std::string escaped;
  for (const char character : value) {
    switch (character) {
    case '\n':
      escaped += "\\n";
      break;
    case '\r':
      escaped += "\\r";
      break;
    case '\t':
      escaped += "\\t";
      break;
    default:
      escaped += character;
      break;
    }
  }
  std::ostringstream out;
  out << std::quoted(escaped);
  return out.str();
}

[[nodiscard]] std::string cell_launch_fields(const CellLaunchCommand &command) {
  return " cell=" + log_field(command.cell_id.value()) +
         " role=" + (command.role == CellLaunchRole::kSceneBarrier
                           ? "scene_barrier" : "pose_observer") +
         (command.observer_source_id.has_value()
              ? " source=" + log_field(*command.observer_source_id)
              : std::string());
}

[[nodiscard]] std::string bounded_text(std::string text,
                                       const std::size_t limit) {
  constexpr std::string_view marker = "[truncated]";
  if (text.size() <= limit)
    return text;
  if (limit <= marker.size())
    return std::string(marker.substr(0U, limit));
  auto end = limit - marker.size();
  // Preserve a complete UTF-8 prefix; NFC input stays NFC after truncation.
  while (end > 0U && (static_cast<unsigned char>(text[end]) & 0xc0U) == 0x80U)
    --end;
  text.resize(end);
  text += marker;
  return text;
}

// Bound the encoded quoted field without splitting an escape or losing its quote.
[[nodiscard]] std::string bounded_log_field(const std::string_view value,
                                           const std::size_t limit) {
  auto rendered = log_field(value);
  if (rendered.size() <= limit) return rendered;
  std::size_t low = 0U;
  std::size_t high = value.size();
  std::string result = log_field("[truncated]");
  while (low <= high) {
    const auto middle = low + (high - low) / 2U;
    auto end = middle;
    while (end > 0U && end < value.size() &&
           (static_cast<unsigned char>(value[end]) & 0xc0U) == 0x80U)
      --end;
    auto candidate = log_field(std::string(value.substr(0U, end)) + "[truncated]");
    if (candidate.size() <= limit) {
      result = std::move(candidate);
      low = middle + 1U;
    } else {
      if (middle == 0U) break;
      high = middle - 1U;
    }
  }
  return result;
}

[[nodiscard]] std::uint64_t output_lines(const std::string_view text) {
  if (text.empty())
    return 0U;
  return static_cast<std::uint64_t>(
             std::count(text.begin(), text.end(), '\n')) +
         (text.back() == '\n' ? 0U : 1U);
}

// A zombie leader pins the group identity but does not mean a launch node is
// still running. Only used during the existing bounded Linux shutdown helper.
// Failure to inspect a member is conservatively treated as a live group.
[[nodiscard]] bool group_has_running_members(const int group_id) {
  std::error_code error;
  std::filesystem::directory_iterator entries("/proc", error);
  if (error)
    return true;
  const std::filesystem::directory_iterator end;
  for (; entries != end; entries.increment(error)) {
    if (error)
      return true;
    const auto name = entries->path().filename().string();
    if (name.empty() ||
        !std::all_of(name.begin(), name.end(), [](const char value) {
          return value >= '0' && value <= '9';
        })) {
      continue;
    }
    std::ifstream input(entries->path() / "stat");
    std::string line;
    if (!std::getline(input, line)) {
      // A process that disappeared during the scan is harmless. An existing
      // unreadable process leaves membership indeterminate.
      if (std::filesystem::exists(entries->path(), error) || error)
        return true;
      continue;
    }
    const auto command_end = line.rfind(')');
    if (command_end == std::string::npos)
      return true;
    std::istringstream fields(line.substr(command_end + 1U));
    char state = 0;
    int parent_id = 0;
    int process_group = 0;
    if (!(fields >> state >> parent_id >> process_group))
      return true;
    if (process_group == group_id && state != 'Z' && state != 'X')
      return true;
  }
  return static_cast<bool>(error);
}

void prepare_upstream_log_directory(
    const std::filesystem::path &artifacts_root) {
  if (artifacts_root.empty() || !artifacts_root.is_absolute())
    fail("the artifacts root must be a nonempty absolute directory");
  const auto upstream_logs = artifacts_root / "ros";
  std::error_code directory_error;
  std::filesystem::create_directories(upstream_logs, directory_error);
  if (directory_error)
    fail("cannot prepare upstream log directory " + upstream_logs.string() +
         ": " + directory_error.message());
  for (const auto &directory : {artifacts_root, upstream_logs}) {
    if (!std::filesystem::is_directory(directory, directory_error) ||
        directory_error)
      fail("not a usable artifact directory: " + directory.string());
    if (::faccessat(AT_FDCWD, directory.c_str(), W_OK | X_OK, AT_EACCESS) != 0)
      fail("artifact directory is not writable/searchable: " +
           directory.string() + ": " + std::strerror(errno));
  }
}

struct SpawnResult final {
  int pid{-1};
  int output_read_fd{-1};
};

struct SpawnRequest final {
  std::vector<std::string> argv;
  std::filesystem::path artifacts_root;
  int parent_death_signal;
  std::promise<SpawnResult> result;
};

/// fork+exec on one long-lived thread: PR_SET_PDEATHSIG fires when the
/// thread that forked the child ends, so a short-lived builder thread must
/// never be that parent.
[[nodiscard]] SpawnResult
spawn_now(const std::vector<std::string> &argv,
          const std::filesystem::path &artifacts_root,
          const int parent_death_signal) {
  if (argv.empty()) fail("empty command");
  const auto executable = resolve_executable(argv.front());
  if (!executable.has_value()) {
    fail("'" + argv.front() + "' is not an executable on PATH");
  }
  const auto upstream_logs = artifacts_root / "ros";
  // Prepare complete private exec inputs before fork; never mutate host
  // environ.
  std::vector<std::string> environment;
  for (auto entry = environ; entry != nullptr && *entry != nullptr; ++entry) {
    const std::string_view value(*entry);
    if (value.substr(0U, 12U) != "ROS_LOG_DIR=")
      environment.emplace_back(value);
  }
  environment.push_back("ROS_LOG_DIR=" + upstream_logs.string());
  std::vector<char *> environment_args;
  for (auto &entry : environment)
    environment_args.push_back(entry.data());
  environment_args.push_back(nullptr);
  int output[2];
  if (::pipe2(output, O_CLOEXEC) != 0)
    fail("cannot prepare the child's output pipe");
  // Only the read end is nonblocking. Child writes retain normal ROS semantics.
  if (::fcntl(output[0], F_SETFL, O_NONBLOCK) < 0) {
    ::close(output[0]);
    ::close(output[1]);
    fail("cannot prepare the child's output reader");
  }
  const int null_fd = ::open("/dev/null", O_RDONLY | O_CLOEXEC);
  int report[2];
  if (null_fd < 0 || ::pipe2(report, O_CLOEXEC) != 0) {
    ::close(output[0]);
    ::close(output[1]);
    if (null_fd >= 0) ::close(null_fd);
    fail("cannot prepare the child's descriptors");
  }
  std::vector<char *> args;
  for (const auto &argument : argv) {
    args.push_back(const_cast<char *>(argument.c_str()));
  }
  args.push_back(nullptr);
  const pid_t parent = ::getpid();
  const pid_t child = ::fork();
  if (child == 0) {
    // Async-signal-safe calls only until exec.
    sigset_t all;
    ::sigemptyset(&all);
    ::sigprocmask(SIG_SETMASK, &all, nullptr);
    for (const int signal_number : {SIGINT, SIGTERM, SIGPIPE, SIGCHLD,
                                    SIGHUP, SIGQUIT}) {
      ::signal(signal_number, SIG_DFL);
    }
    int failure = 0;
    if (::setpgid(0, 0) != 0 ||
        ::prctl(PR_SET_PDEATHSIG, parent_death_signal) != 0) {
      failure = errno;
    } else if (::getppid() != parent) {
      failure = ESRCH;
    } else if (::dup2(null_fd, STDIN_FILENO) < 0 ||
               ::dup2(output[1], STDOUT_FILENO) < 0 ||
               ::dup2(output[1], STDERR_FILENO) < 0) {
      failure = errno;
    }
    if (failure == 0) {
      // Keep only stdio and the CLOEXEC report pipe: the host's private
      // channels and sockets never reach the launch.
      for (int fd = 3; fd < 65536; ++fd) {
        if (fd != report[1]) ::close(fd);
      }
      ::execve(executable->c_str(), args.data(), environment_args.data());
      failure = errno;
    }
    const auto ignored = ::write(report[1], &failure, sizeof(failure));
    static_cast<void>(ignored);
    ::_exit(127);
  }
  const int fork_errno = errno;
  ::close(report[1]);
  ::close(output[1]);
  ::close(null_fd);
  if (child < 0) {
    ::close(output[0]);
    ::close(report[0]);
    fail(std::string("fork failed: ") + std::strerror(fork_errno));
  }
  int child_errno = 0;
  ssize_t read_bytes = 0;
  do {
    read_bytes = ::read(report[0], &child_errno, sizeof(child_errno));
  } while (read_bytes < 0 && errno == EINTR);
  ::close(report[0]);
  if (read_bytes > 0) {
    ::close(output[0]);
    while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR) {
    }
    fail("'" + *executable + "' could not be started: " +
         std::strerror(child_errno));
  }
  return SpawnResult{child, output[0]};
}

} // namespace

ControllerLaunchCommand
mock_controller_launch_command(const ControllerLaunchArtifacts &artifacts) {
  require_artifacts(artifacts, ControllerLaunchLane::kMock);
  if (artifacts.launch_arguments.empty()) {
    fail("mock component '" + artifacts.controllable_component_id +
         "' has no launch arguments");
  }
  ControllerLaunchCommand command;
  command.controllable_component_id = artifacts.controllable_component_id;
  command.lane = ControllerLaunchLane::kMock;
  command.argv = {std::string(kRos2), "launch", std::string(kBringupPackage),
                  std::string(kMockLaunchFile)};
  std::set<std::string> names;
  for (const auto &argument : artifacts.launch_arguments) {
    if (!names.insert(argument_name(argument)).second) {
      fail("mock launch argument repeated: " + argument);
    }
    command.argv.push_back(argument);
  }
  command.identity = identity_of(artifacts, {});
  command.robot_description_sha256 = artifacts.robot_description_sha256;
  command.controllers_sha256 = artifacts.controllers_sha256;
  command.controller_profile = artifacts.controller_profile;
  command.runtime_namespace = artifacts.runtime_namespace;
  command.arm_controller_name = artifacts.arm_controller_name;
  command.gripper_controller_name = artifacts.gripper_controller_name;
  command.frame_prefix = artifacts.frame_prefix;
  command.controllers_yaml = artifacts.controllers_yaml;
  command.keep_across_apply = true;
  return command;
}

ControllerLaunchCommand
isaac_controller_launch_command(const ControllerLaunchArtifacts &artifacts,
                                const IsaacControllerLane &lane,
                                const IsaacControllerLaunchInputs &inputs) {
  require_artifacts(artifacts, ControllerLaunchLane::kIsaac);
  const auto required = [](const std::string &value, const char *name) {
    if (value.empty()) {
      fail(std::string("Isaac launch input '") + name + "' is required");
    }
    return value;
  };
  if (lane.joint_commands_topic.empty() || lane.joint_states_topic.empty()) {
    fail("Isaac lane facts have no joint topics");
  }
  if (inputs.manager_run_root.has_value() !=
      inputs.pose_observer_site_config.has_value()) {
    fail("manager_run_root and pose_observer_site_config are supplied together");
  }
  std::vector<std::string> host_owned{
      "robot_description_file:=" + artifacts.robot_description_file.string(),
      "controllers_file:=" + artifacts.controllers_file.string(),
      "isaac_profile:=" + artifacts.controller_profile,
      "runtime_namespace:=" + artifacts.runtime_namespace,
      "controller_manager_name:=" + std::string(kControllerManagerNodeName),
      "arm_controller_name:=" + artifacts.arm_controller_name,
      // The generated URDF's TopicBasedSystem was written from these lane
      // facts; the launch only validates readiness on them, so they agree.
      "joint_commands_topic:=" + lane.joint_commands_topic,
      "joint_states_topic:=" + lane.joint_states_topic,
  };
  if (!artifacts.frame_prefix.empty()) {
    host_owned.push_back("frame_prefix:=" + artifacts.frame_prefix);
  }
  std::vector<std::string> caller{
      "world_uri:=" + required(inputs.world_uri, "world_uri"),
      "expected_world_name:=" +
          required(inputs.expected_world_name, "expected_world_name"),
      "scene_startup_timeout_s:=" +
          required(inputs.scene_startup_timeout_s, "scene_startup_timeout_s"),
      "scene_service_timeout_s:=" +
          required(inputs.scene_service_timeout_s, "scene_service_timeout_s"),
  };
  if (inputs.manager_run_root.has_value()) {
    caller.push_back("manager_run_root:=" + inputs.manager_run_root->string());
    caller.push_back("pose_observer_site_config:=" +
                     inputs.pose_observer_site_config->string());
  }
  if (inputs.manager_cell_world_owned) {
    caller.push_back("manager_cell_world_owned:=true");
  }
  for (const auto &argument : inputs.additional_arguments) {
    if (argument_name(argument) == "manager_cell_world_owned") {
      fail("Isaac cell launch ownership is a host-owned input");
    }
    caller.push_back(argument);
  }
  ControllerLaunchCommand command;
  command.controllable_component_id = artifacts.controllable_component_id;
  command.lane = ControllerLaunchLane::kIsaac;
  command.argv = {std::string(kRos2), "launch", std::string(kBringupPackage),
                  std::string(kIsaacLaunchFile)};
  std::set<std::string> names;
  for (const auto *group : {&host_owned, &caller}) {
    for (const auto &argument : *group) {
      if (!names.insert(argument_name(argument)).second) {
        fail("Isaac launch argument repeated or overrides a host-owned "
             "argument: " + argument);
      }
      command.argv.push_back(argument);
    }
  }
  command.identity = identity_of(artifacts, caller);
  command.robot_description_sha256 = artifacts.robot_description_sha256;
  command.controllers_sha256 = artifacts.controllers_sha256;
  command.controller_profile = artifacts.controller_profile;
  command.runtime_namespace = artifacts.runtime_namespace;
  command.arm_controller_name = artifacts.arm_controller_name;
  command.gripper_controller_name = artifacts.gripper_controller_name;
  command.frame_prefix = artifacts.frame_prefix;
  command.controllers_yaml = artifacts.controllers_yaml;
  // DX224.3: unchanged Isaac identity keeps the launch and avoids an
  // unintended world reload on Apply.
  command.keep_across_apply = true;
  return command;
}

namespace {

// Q80 INTERIM, TO BE DELETED: until the Components panel drives the
// per-component bring-up request, a Deployment Apply that activates a
// component whose launch has exited brings it up again (a new launch through
// the normal activation path). Delete this function and its single call as
// soon as the Components panel works; the exited case then stays failed until
// the explicit bring-up request.
[[nodiscard]] ControllerLaunchDecision
q80_interim_apply_brings_up_an_exited_launch() {
  return ControllerLaunchDecision::kRestart;
}

} // namespace

ControllerLaunchDecision decide_controller_launch(
    const std::optional<ControllerLaunchObservation> &current,
    const ControllerLaunchCommand &next) {
  if (!current.has_value()) return ControllerLaunchDecision::kStart;
  if (!current->running) return q80_interim_apply_brings_up_an_exited_launch();
  if (next.keep_across_apply &&
      current->keep_across_apply && current->identity == next.identity) {
    return ControllerLaunchDecision::kKeep;
  }
  return ControllerLaunchDecision::kRestart;
}

struct ControllerLaunchProcess::ForwardingCounters final {
  std::atomic<std::uint64_t> lines{0U};
  std::atomic<std::uint64_t> bytes{0U};

  ControllerLaunchForwardingCounters read() const {
    return {lines.load(std::memory_order_relaxed),
            bytes.load(std::memory_order_relaxed)};
  }
};

struct ControllerLaunchProcess::Output final {
  static constexpr std::size_t kTailBytes = 128U * 1024U;
  static constexpr std::uint64_t kTailLines = 2000U;
  static constexpr std::size_t kPendingBytes = 16U * 1024U;
  mutable std::mutex mutex;
  ControllerLaunchOutputSnapshot snapshot;
  std::deque<std::string> pending;
  std::size_t pending_bytes{0U};
  std::uint64_t tail_newlines{0U};
  bool producer_finished{false};
  bool wake_unavailable{false};
  int wake_fd{-1};
  const std::string tag;
  const std::shared_ptr<ForwardingCounters> totals;

  Output(const std::string &component, const int pid, const bool host = false,
         std::shared_ptr<ForwardingCounters> counters = {})
      : tag(host ? "manyforge_host_notice pid=" + std::to_string(pid) + " "
                 : "manyforge_controller_output component=" +
                       log_field(component) + " pid=" + std::to_string(pid) +
                       " "),
        totals(std::move(counters)) {
    snapshot.tail.reserve(kTailBytes);
  }

  explicit Output(std::string cell_tag)
      : tag(std::move(cell_tag)), totals() {
    snapshot.tail.reserve(kTailBytes);
  }

  void record_loss_locked(const std::uint64_t lines,
                          const std::uint64_t bytes) {
    snapshot.dropped_forward_bytes += bytes;
    snapshot.dropped_forward_lines += lines;
    if (totals) {
      totals->bytes.fetch_add(bytes, std::memory_order_relaxed);
      totals->lines.fetch_add(lines, std::memory_order_relaxed);
    }
  }

  void dropped_locked(const std::string_view bytes) {
    record_loss_locked(output_lines(bytes), bytes.size());
  }

  ~Output() {
    if (wake_fd >= 0)
      ::close(wake_fd);
  }

  void attach_wake(const int descriptor) {
    // Each output owns a dup of the existing eventfd, never the Spawner or
    // process. A concurrent append cannot write a closed/reused descriptor.
    const auto duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 3);
    std::lock_guard lock(mutex);
    if (duplicate < 0) {
      // An optional wake failure must not turn a started launch into a failed
      // activation. Keep draining its pipe/ring, but count forwarding loss.
      wake_unavailable = true;
      for (const auto &packet : pending)
        dropped_locked(packet);
      pending.clear();
      pending_bytes = 0U;
      return;
    }
    wake_fd = duplicate;
  }

  void notify(const int descriptor) const {
    if (descriptor >= 0) {
      const std::uint64_t value = 1U;
      const auto written = ::write(descriptor, &value, sizeof(value));
      if (written == static_cast<ssize_t>(sizeof(value)))
        return;
      if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        return; // The eventfd is already readable; no additional wake is
                // needed.
      // Intentionally preserve the existing ignored unexpected-error outcome.
      // A different wake-failure policy requires a separate maintainer
      // decision.
    }
  }

  void erase_prefix_locked(std::size_t bytes) {
    // A byte limit must not leave half of a UTF-8 character at the front.
    while (bytes < snapshot.tail.size() &&
           (static_cast<unsigned char>(snapshot.tail[bytes]) & 0xc0U) == 0x80U)
      ++bytes;
    tail_newlines -= static_cast<std::uint64_t>(
        std::count(snapshot.tail.begin(), snapshot.tail.begin() + bytes, '\n'));
    snapshot.tail.erase(0U, bytes);
    snapshot.truncated = true;
  }

  void append(const std::string_view text, const bool wake = true) {
    if (text.empty())
      return;
    int descriptor = -1;
    {
      std::lock_guard lock(mutex);
      if (text.size() > kTailBytes) {
        auto begin = text.size() - kTailBytes;
        while (begin < text.size() &&
               (static_cast<unsigned char>(text[begin]) & 0xc0U) == 0x80U)
          ++begin;
        snapshot.tail.assign(text.substr(begin));
        tail_newlines = static_cast<std::uint64_t>(
            std::count(snapshot.tail.begin(), snapshot.tail.end(), '\n'));
        snapshot.truncated = true;
      } else {
        if (snapshot.tail.size() + text.size() > kTailBytes)
          erase_prefix_locked(snapshot.tail.size() + text.size() - kTailBytes);
        snapshot.tail.append(text);
        tail_newlines += static_cast<std::uint64_t>(
            std::count(text.begin(), text.end(), '\n'));
      }
      auto lines =
          tail_newlines +
          (!snapshot.tail.empty() && snapshot.tail.back() != '\n' ? 1U : 0U);
      if (lines > kTailLines) {
        std::size_t remove = 0U;
        for (auto excess = lines - kTailLines; excess > 0U; --excess)
          remove = snapshot.tail.find('\n', remove) + 1U;
        erase_prefix_locked(remove);
        lines =
            tail_newlines +
            (!snapshot.tail.empty() && snapshot.tail.back() != '\n' ? 1U : 0U);
      }
      snapshot.line_count = lines;
      std::size_t begin = 0U;
      while (begin < text.size()) {
        const auto newline = text.find('\n', begin);
        const auto end =
            newline == std::string_view::npos ? text.size() : newline + 1U;
        const auto segment = text.substr(begin, end - begin);
        if (wake_unavailable ||
            tag.size() + segment.size() + (segment.back() == '\n' ? 0U : 1U) >
                kPendingBytes) {
          record_loss_locked(1U, tag.size() + segment.size() +
                                     (segment.back() == '\n' ? 0U : 1U));
          begin = end;
          continue;
        }
        auto packet = tag + std::string(segment);
        if (packet.back() != '\n')
          packet += '\n';
        // No waiting for space. The recent ring and forwarding loss are
        // separate.
        while (pending_bytes + packet.size() > kPendingBytes) {
          dropped_locked(pending.front());
          pending_bytes -= pending.front().size();
          pending.pop_front();
        }
        pending_bytes += packet.size();
        pending.push_back(std::move(packet));
        begin = end;
      }
      descriptor = wake ? wake_fd : -1;
    }
    // Only a nonblocking wake write, after releasing every diagnostic lock.
    notify(descriptor);
  }

  void finish_producer() {
    int descriptor = -1;
    {
      std::lock_guard lock(mutex);
      producer_finished = true;
      descriptor = wake_fd;
    }
    notify(descriptor);
  }

  bool has_pending() const {
    std::lock_guard lock(mutex);
    return !pending.empty();
  }

  bool can_retire() const {
    std::lock_guard lock(mutex);
    // With no wake descriptor every future fragment is already counted as
    // lost, so retaining the collector owner after EOF serves no purpose.
    return (producer_finished || wake_unavailable) && pending.empty();
  }

  void discard_pending() {
    std::lock_guard lock(mutex);
    for (const auto &packet : pending)
      dropped_locked(packet);
    pending.clear();
    pending_bytes = 0U;
  }

  std::optional<std::string> take(const std::size_t limit) {
    std::lock_guard lock(mutex);
    if (pending.empty() || pending.front().size() > limit)
      return std::nullopt;
    auto packet = std::move(pending.front());
    pending.pop_front();
    pending_bytes -= packet.size();
    return packet;
  }

  void dropped(const std::string_view bytes) {
    std::lock_guard lock(mutex);
    dropped_locked(bytes);
  }

  ControllerLaunchOutputSnapshot read() const {
    std::lock_guard lock(mutex);
    return snapshot;
  }

  ControllerLaunchForwardingCounters counters() const {
    std::lock_guard lock(mutex);
    return {snapshot.dropped_forward_lines, snapshot.dropped_forward_bytes};
  }
};

ControllerLaunchProcess::ControllerLaunchProcess(
    ControllerLaunchCommand command, const int pid,
    std::filesystem::path upstream_log_directory, std::string context,
    std::shared_ptr<ForwardingCounters> counters)
    : command_(std::move(command)), pid_(pid),
      upstream_log_directory_(std::move(upstream_log_directory)),
      start_context_(std::move(context)),
      current_request_context_(start_context_) {
  output_ = std::make_shared<Output>(this->command().controllable_component_id, pid_,
                                     false, std::move(counters));
}

ControllerLaunchProcess::ControllerLaunchProcess(
    CellLaunchCommand command, const int pid,
    std::filesystem::path upstream_log_directory, std::string context)
    : command_(std::move(command)), pid_(pid),
      upstream_log_directory_(std::move(upstream_log_directory)),
      start_context_(std::move(context)),
      current_request_context_(start_context_) {
  output_ = std::make_shared<Output>(
      "manyforge_cell_output" +
      cell_launch_fields(std::get<CellLaunchCommand>(command_)) +
      " pid=" + std::to_string(pid_) + " ");
}

void ControllerLaunchProcess::append_start_header() {
  if (const auto *cell = std::get_if<CellLaunchCommand>(&command_)) {
    output_->append("manyforge_cell_launch_start observed_utc=" +
                    observed_utc_now() + cell_launch_fields(*cell) +
                    " pid=" + std::to_string(pid_) + " request_context=" +
                    log_field(start_context_) + " launch_identity=" +
                    bounded_log_field(cell->identity, 4096U) + "\n");
    return;
  }
  const auto line =
      "manyforge_controller_launch_start observed_utc=" + observed_utc_now() +
      " component=" + log_field(command().controllable_component_id) +
      " pid=" + std::to_string(pid_) +
      " request_context=" + log_field(start_context_) +
      " launch_identity=" + bounded_log_field(command().identity, 4096U) + "\n";
  output_->append(line);
}

ControllerLaunchProcess::~ControllerLaunchProcess() {
  stop(std::chrono::seconds(10));
  output_->finish_producer();
}

void ControllerLaunchProcess::observe_exit_locked() {
  if (!group_owned_)
    return;
  const std::string subject = std::holds_alternative<CellLaunchCommand>(command_)
                                  ? "cell launch" : "controller launch";
  siginfo_t status{};
  int result = 0;
  do {
    result = ::waitid(P_PID, static_cast<id_t>(pid_), &status,
                      WEXITED | WNOHANG | WNOWAIT);
  } while (result < 0 && errno == EINTR);
  if (result == 0 && status.si_pid == pid_) {
    if (!exit_.has_value()) {
      if (status.si_code == CLD_EXITED) {
        exit_ = subject + " exited with status " +
                std::to_string(status.si_status);
      } else {
        exit_ = subject + " was terminated by signal " +
                std::to_string(status.si_status);
      }
    }
    if (!stopping_)
      unexpected_exit_observed_ = true;
    capture_exit_locked(
        status.si_code == CLD_EXITED ? std::optional<int>(status.si_status)
                                     : std::nullopt,
        status.si_code == CLD_EXITED ? std::nullopt
                                     : std::optional<int>(status.si_status),
        status.si_code == CLD_DUMPED);
  } else if (result < 0 && errno == ECHILD) {
    // Another owner has reaped the leader. Its PID/group can be reused, so
    // there is no longer an identity proof for group signalling.
    group_owned_ = false;
    if (!stopping_)
      unexpected_exit_observed_ = true;
    if (!exit_.has_value()) {
      exit_ = subject + " process is no longer a child of this host";
    }
    capture_exit_locked(std::nullopt, std::nullopt, false);
  }
}

void ControllerLaunchProcess::capture_exit_locked(
    const std::optional<int> exit_status, const std::optional<int> signal,
    const bool core_dumped) {
  if (exit_evidence_.has_value() || cell_exit_evidence_.has_value())
    return;
  if (const auto *cell = std::get_if<CellLaunchCommand>(&command_)) {
    cell_exit_evidence_ = CellLaunchExitEvidence{
        pid_, exit_status, signal, core_dumped, observed_utc_now(), *cell,
        upstream_log_directory_, exit_.value_or("cell launch ended"),
        current_request_context_};
    return;
  }
  exit_evidence_ =
      ControllerLaunchExitEvidence{pid_,
                                   exit_status,
                                   signal,
                                   core_dumped,
                                   observed_utc_now(),
                                   command().controllable_component_id,
                                   command().identity,
                                   command().robot_description_sha256,
                                   command().controllers_sha256,
                                   upstream_log_directory_,
                                   exit_.value_or("controller launch ended"),
                                   current_request_context_,
                                   std::nullopt};
}

std::optional<ControllerLaunchExitEvidence>
ControllerLaunchProcess::exit_evidence() {
  std::lock_guard lock(mutex_);
  observe_exit_locked();
  return exit_evidence_;
}

std::optional<CellLaunchExitEvidence> CellLaunchProcess::exit_evidence() {
  std::lock_guard lock(occurrence_->mutex_);
  occurrence_->observe_exit_locked();
  return occurrence_->cell_exit_evidence_;
}

bool CellLaunchProcess::group_drained() const {
  std::lock_guard lock(occurrence_->mutex_);
  // stop_complete_ publishes the retirement lane's scan before the same
  // mutex release; unfinished retirement never reads its in-flight scan.
  return occurrence_->stop_complete_ && occurrence_->exact_leader_reaped_ &&
         occurrence_->stop_scanned_drained_;
}

void ControllerLaunchProcess::update_request_context(std::string context) {
  std::lock_guard lock(mutex_);
  current_request_context_ = std::move(context);
}

bool ControllerLaunchProcess::append_incident_correlation(
    const std::string &incident_id, const std::string &context,
    const std::string &summary) {
  if (incident_id.empty())
    return false;
  std::string line;
  {
    std::lock_guard lock(mutex_);
    if (last_incident_id_ == incident_id)
      return false;
    const auto correlation_context =
        current_request_context_ +
        (context.empty() ? std::string() : " " + context);
    last_incident_id_ = incident_id;
    last_incident_context_ = correlation_context;
    line = "manyforge_controller_launch_incident incident_id=" +
           log_field(incident_id) + " observed_utc=" + observed_utc_now() +
           " component=" + log_field(command().controllable_component_id) +
           " pid=" + std::to_string(pid_) +
           " request_context=" + log_field(correlation_context) +
           " summary=" + log_field(summary) + "\n";
  }
  output_->append(line);
  return true;
}

ControllerLaunchOutputSnapshot
ControllerLaunchProcess::output_snapshot() const {
  return output_->read();
}

ControllerLaunchForwardingCounters
ControllerLaunchProcess::output_counters() const {
  return output_->counters();
}

std::optional<std::string> ControllerLaunchProcess::exited() {
  std::lock_guard lock(mutex_);
  observe_exit_locked();
  return exit_;
}

ControllerLaunchCleanupObservation
ControllerLaunchProcess::cleanup_observation() {
  std::lock_guard lock(mutex_);
  return cleanup_;
}

void ControllerLaunchProcess::begin_stop(
    const std::chrono::milliseconds grace) noexcept {
  std::lock_guard retirement_lock(retirement_mutex_);
  std::lock_guard lock(mutex_);
  if (stop_deadline_.has_value() || stop_complete_)
    return;
  observe_exit_locked();
  stopping_ = true;
  // A Background cleanup and aggregate shutdown share this exact first signal
  // time. Neither another caller nor another poll can extend the grace.
  if (group_owned_) {
    stop_signal_sent_ = ::kill(-pid_, SIGINT) == 0;
    stop_context_ = current_request_context_;
    stop_incident_id_ = last_incident_id_;
  }
  stop_deadline_ = std::chrono::steady_clock::now() + grace;
}

bool ControllerLaunchProcess::poll_stop() noexcept {
  std::lock_guard retirement_lock(retirement_mutex_);
  const auto &context = stop_context_;
  const auto &incident_id = stop_incident_id_;
  bool owned = false;
  bool exited = false;
  {
    std::lock_guard lock(mutex_);
    if (stop_complete_)
      return true;
    if (!stop_deadline_)
      return false;
    observe_exit_locked();
    owned = group_owned_;
    exited = exit_.has_value();
  }
  const auto no_live_members = [&] {
    try {
      return !group_has_running_members(pid_);
    } catch (...) {
      return false; // Allocation/read errors never establish absence.
    }
  };
  if (owned && !stop_escalated_) {
    if (exited && no_live_members())
      stop_scanned_drained_ = true;
    if (!stop_scanned_drained_ &&
        std::chrono::steady_clock::now() < *stop_deadline_)
      return false;
    {
      std::lock_guard lock(mutex_);
      observe_exit_locked();
      if (group_owned_)
        stop_kill_sent_ = ::kill(-pid_, SIGKILL) == 0;
      stop_escalated_ = true;
    }
    // Preserve the original one scan after escalation, with no second grace.
    if (!stop_scanned_drained_)
      stop_scanned_drained_ = no_live_members();
  }
  {
    std::lock_guard lock(mutex_);
    if (group_owned_) {
      int status = 0;
      observe_exit_locked();
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      const int wait_error = errno;
      if (result == 0 || (result < 0 && wait_error == EINTR))
        return false; // Post-KILL reap keeps the original unbounded wait.
      if (result == pid_) {
        exact_leader_reaped_ = true;
        if (!exit_)
          exit_ = describe_status(
              status, std::holds_alternative<CellLaunchCommand>(command_));
        capture_exit_locked(
            WIFEXITED(status) ? std::optional<int>(WEXITSTATUS(status))
                              : std::nullopt,
            WIFSIGNALED(status) ? std::optional<int>(WTERMSIG(status))
                                : std::nullopt,
            WIFSIGNALED(status) && WCOREDUMP(status));
      } else {
        if (!exit_)
          exit_ = std::holds_alternative<CellLaunchCommand>(command_)
                      ? "cell launch process is no longer a child of this host"
                      : "controller launch process is no longer a child of "
                        "this host";
        capture_exit_locked(std::nullopt, std::nullopt, false);
      }
      group_owned_ = false;
    }
    if (cleanup_.state != ControllerLaunchCleanupState::kDrained) {
      cleanup_.state = ControllerLaunchCleanupState::kUnknown;
      if (stop_scanned_drained_ && exact_leader_reaped_) {
        if (const auto *component =
                std::get_if<ControllerLaunchCommand>(&command_)) {
          try {
            cleanup_.receipt = ControllerLaunchGroupDrainReceipt{
                pid_, component->controllable_component_id, component->identity,
                observed_utc_now()};
            cleanup_.state = ControllerLaunchCleanupState::kDrained;
          } catch (...) {
            // Missing receipt stays Unknown, independently of exit evidence.
          }
        }
      }
    }
    stop_complete_ = true;
  }
  if (owned) {
    try {
      if (const auto *cell = std::get_if<CellLaunchCommand>(&command_)) {
        output_->append("manyforge_cell_launch_end observed_utc=" +
                        observed_utc_now() + cell_launch_fields(*cell) +
                        " pid=" + std::to_string(pid_) + " request_context=" +
                        log_field(context) + "\n");
      } else {
        output_->append("manyforge_controller_launch_end observed_utc=" +
                        observed_utc_now() + " component=" +
                        log_field(command().controllable_component_id) +
                        " pid=" + std::to_string(pid_) +
                        " request_context=" + log_field(context) +
                        " incident_id=" + log_field(incident_id) + "\n");
      }
    } catch (...) {
      // Containment completed; optional output loss cannot terminate the host.
    }
  }
  return true;
}

void ControllerLaunchProcess::stop(
    const std::chrono::milliseconds grace) noexcept {
  begin_stop(grace);
  while (!poll_stop())
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

struct ControllerLaunchSupervisor::Spawner final {
  struct Stream final {
    int fd{-1};
    // Strong output-only ownership preserves end records without keeping a
    // process alive or running its destructor on the collector thread.
    std::shared_ptr<ControllerLaunchProcess::Output> output;
  };
  std::mutex mutex;
  std::deque<SpawnRequest *> queue;
  std::vector<Stream> registrations;
  bool closing{false};
  int wake_fd{-1};
  int forward_fd{-1};
  bool forward_is_socket{false};
  std::shared_ptr<ControllerLaunchProcess::Output> host_output;
  std::thread thread;

  void notify() const {
    const std::uint64_t value = 1U;
    const auto written = ::write(wake_fd, &value, sizeof(value));
    if (written == static_cast<ssize_t>(sizeof(value)))
      return;
    if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
      return; // A wake is already pending; do not retry a nonblocking handoff.
    // Intentionally preserve the existing ignored unexpected-error outcome.
    // No new refusal/fatal policy is introduced for a corrupted wake
    // descriptor.
  }

  void consume_wake() const {
    std::uint64_t wake_count = 0U;
    const auto received = ::read(wake_fd, &wake_count, sizeof(wake_count));
    if (received == static_cast<ssize_t>(sizeof(wake_count)))
      return;
    if (received < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
      return; // No wake consumed this turn; continue the existing fair drain.
    // Intentionally ignore unexpected receipts as before, without ending the
    // long-lived parent thread or introducing retries/fatal handling.
  }

  Spawner() {
    wake_fd = ::eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (wake_fd < 0)
      fail("cannot prepare the spawner wake descriptor");
    try {
      host_output = std::make_shared<ControllerLaunchProcess::Output>(
          "", ::getpid(), true);
      host_output->attach_wake(wake_fd);
    } catch (...) {
      ::close(wake_fd);
      throw;
    }
    // A separate open description: setting nonblocking must not change host
    // stdout or any child logger's status flags. Failure is counted as loss.
    forward_fd =
        ::open("/proc/self/fd/1", O_WRONLY | O_NONBLOCK | O_APPEND | O_CLOEXEC | O_NOCTTY);
    if (forward_fd < 0 && errno == ENXIO) {
      struct stat stdout_status {};
      if (::fstat(STDOUT_FILENO, &stdout_status) == 0 &&
          S_ISSOCK(stdout_status.st_mode)) {
        // Linux cannot reopen a socket through proc. This dup shares flags,
        // so socket forwarding uses per-call DONTWAIT and never changes them.
        forward_fd = ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 3);
      }
    }
    struct stat forward_status {};
    if (forward_fd >= 0 &&
        (::fstat(forward_fd, &forward_status) != 0 ||
         (!S_ISFIFO(forward_status.st_mode) &&
          !S_ISSOCK(forward_status.st_mode) &&
          !(S_ISCHR(forward_status.st_mode) && ::isatty(forward_fd))))) {
      // O_NONBLOCK cannot prevent regular-file IO stalls. Unsupported sinks
      // use the same drop accounting as unavailable sinks, without fallback.
      ::close(forward_fd);
      forward_fd = -1;
    }
    forward_is_socket = forward_fd >= 0 && S_ISSOCK(forward_status.st_mode);
    try {
      thread = std::thread([this] { run(); });
    } catch (...) {
      ::close(wake_fd);
      if (forward_fd >= 0)
        ::close(forward_fd);
      throw;
    }
  }

  void run() {
    sigset_t blocked;
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGPIPE);
    // Broken host sinks are diagnostic loss, never a host termination.
    static_cast<void>(::pthread_sigmask(SIG_BLOCK, &blocked, nullptr));
    std::vector<Stream> streams{{-1, host_output}};
    std::size_t cursor = 0U;
    while (true) {
      SpawnRequest *request = nullptr;
      bool finish = false;
      {
        std::lock_guard lock(mutex);
        for (auto &stream : registrations)
          streams.push_back(std::move(stream));
        registrations.clear();
        if (!queue.empty()) {
          request = queue.front();
          queue.pop_front();
        }
        finish = closing && request == nullptr;
      }
      std::vector<pollfd> descriptors{{wake_fd, POLLIN, 0}};
      for (const auto &stream : streams)
        if (stream.fd >= 0)
          descriptors.push_back({stream.fd, POLLIN, 0});
      // Idle collectors sleep until child output, a spawn, or a producer's
      // eventfd notice. Remaining bounded handoff work gets another fair turn.
      const auto pending =
          std::any_of(streams.begin(), streams.end(), [](const auto &stream) {
            return stream.output->has_pending();
          });
      static_cast<void>(::poll(descriptors.data(), descriptors.size(),
                               finish || request || pending ? 0 : -1));
      consume_wake();
      // Fair bounded turns: at most 64KiB overall and 16KiB per child before
      // the next queued spawn. Normal child writes may still backpressure if
      // the OS/fork/exec scheduler delays this existing thread.
      std::size_t budget = 64U * 1024U;
      std::size_t forward_budget = 64U * 1024U;
      const auto count = streams.size();
      for (std::size_t turn = 0U; turn < count; ++turn) {
        auto &stream = streams[(cursor + turn) % count];
        const auto &output = stream.output;
        for (int read_turn = 0; stream.fd >= 0 && read_turn < 4 && budget > 0U;
             ++read_turn) {
          char bytes[4096];
          const auto read_size = std::min(budget, sizeof(bytes));
          const auto got = ::read(stream.fd, bytes, read_size);
          if (got > 0) {
            budget -= static_cast<std::size_t>(got);
            output->append(
                std::string_view(bytes, static_cast<std::size_t>(got)), false);
          } else {
            if (got == 0 ||
                (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) {
              ::close(stream.fd);
              stream.fd = -1;
            }
            break;
          }
        }
        for (int forward_turn = 0; forward_turn < 4 && forward_budget > 0U;
             ++forward_turn) {
          const auto packet = output->take(forward_budget);
          if (!packet)
            break;
          forward_budget -= std::min(forward_budget, packet->size());
          const auto sent =
              forward_fd < 0 ? -1
              : forward_is_socket
                  ? ::send(forward_fd, packet->data(), packet->size(),
                           MSG_DONTWAIT | MSG_NOSIGNAL)
                  : ::write(forward_fd, packet->data(), packet->size());
          const auto accepted = sent > 0 ? static_cast<std::size_t>(sent) : 0U;
          if (accepted < packet->size())
            output->dropped(std::string_view(*packet).substr(accepted));
        }
      }
      if (count > 0U)
        cursor = (cursor + 1U) % count;
      streams.erase(std::remove_if(streams.begin(), streams.end(),
                                   [](const auto &stream) {
                                     return stream.fd < 0 &&
                                            stream.output->can_retire();
                                   }),
                    streams.end());
      if (finish)
        break;
      if (request != nullptr) {
        try {
          request->result.set_value(
              spawn_now(request->argv, request->artifacts_root,
                        request->parent_death_signal));
        } catch (...) {
          request->result.set_exception(std::current_exception());
        }
      }
    }
    // Teardown never waits for a blocked sink or an escaped pipe writer.
    // Account every queued fragment before releasing its output owner.
    for (const auto &stream : streams) {
      if (stream.fd >= 0)
        ::close(stream.fd);
      stream.output->discard_pending();
    }
    // Registrations queued while closing cannot leak their read descriptors.
    std::vector<Stream> remaining;
    {
      std::lock_guard lock(mutex);
      remaining = std::move(registrations);
    }
    for (const auto &stream : remaining) {
      if (stream.fd >= 0)
        ::close(stream.fd);
      stream.output->discard_pending();
    }
  }

  ~Spawner() {
    {
      std::lock_guard lock(mutex);
      closing = true;
    }
    notify();
    if (thread.joinable()) thread.join();
    ::close(wake_fd);
    if (forward_fd >= 0)
      ::close(forward_fd);
  }

  SpawnResult spawn(const std::vector<std::string> &argv,
                    const std::filesystem::path &artifacts_root,
                    const int parent_death_signal = SIGTERM) {
    SpawnRequest request{argv, artifacts_root, parent_death_signal, {}};
    auto result = request.result.get_future();
    {
      std::lock_guard lock(mutex);
      if (closing) fail("the supervisor is closing");
      queue.push_back(&request);
    }
    notify();
    return result.get();
  }

  void register_output(
      const int fd,
      const std::shared_ptr<ControllerLaunchProcess::Output> &output) {
    {
      std::lock_guard lock(mutex);
      registrations.push_back(Stream{fd, output});
    }
    notify();
  }
};

ControllerLaunchSupervisor::ControllerLaunchSupervisor(
    const std::chrono::milliseconds stop_grace)
    : stop_grace_(stop_grace), spawner_(std::make_unique<Spawner>()) {}

ControllerLaunchSupervisor::~ControllerLaunchSupervisor() {
  stop_all();
  // Children stopped above; the spawner thread (their PDEATHSIG parent) may
  // end now.
  spawner_.reset();
}

ControllerLaunchSupervisor::Ensured
ControllerLaunchSupervisor::ensure(const ControllerLaunchCommand &command,
                                   const std::filesystem::path &artifacts_root,
                                   std::string context) {
  if (command.controllable_component_id.empty() || command.argv.empty() ||
      command.identity.empty()) {
    fail("incomplete launch command");
  }
  if (artifacts_root.empty() || !artifacts_root.is_absolute())
    fail("the artifacts root must be a nonempty absolute directory");
  while (true) {
    std::shared_ptr<ControllerLaunchProcess> predecessor;
    {
      std::lock_guard lock(mutex_);
      if (host_shutdown_started_)
        fail("controller launch submission is closed for host shutdown");
      const auto found = processes_.find(command.controllable_component_id);
      if (found != processes_.end())
        predecessor = found->second.current;
    }
    // Neither the process mutex nor filesystem preflight may hold the map lock
    // needed by the scalar status counters.
    std::optional<ControllerLaunchObservation> current;
    if (predecessor) {
      current =
          ControllerLaunchObservation{predecessor->command().identity,
                                      predecessor->command().keep_across_apply,
                                      !predecessor->exited().has_value()};
    }
    const auto decision = decide_controller_launch(current, command);
    if (decision != ControllerLaunchDecision::kKeep) {
      // Refuse an unusable root before retiring this exact predecessor. Retain
      // continues using the physical occurrence's original ROS_LOG_DIR.
      prepare_upstream_log_directory(artifacts_root);
      if (predecessor)
        predecessor->stop(stop_grace_);
    }
    std::unique_lock lock(mutex_);
    if (host_shutdown_started_)
      fail("controller launch submission is closed for host shutdown");
    const auto found = processes_.find(command.controllable_component_id);
    const auto actual =
        found == processes_.end() ? nullptr : found->second.current;
    if (actual != predecessor) {
      // Only concurrent ownership changes are reconsidered. Launch/preflight
      // failures escape immediately, never retrying or restoring a process.
      lock.unlock();
      continue;
    }
    if (decision == ControllerLaunchDecision::kKeep)
      return {decision, predecessor};
    // Q66: the checked predecessor has ended before serialized spawn/publish.
    // Re-read the stable slot's counters: a null current can span a concurrent
    // start/remove, whose since-host-start loss must still be preserved.
    auto counters =
        found == processes_.end()
            ? std::make_shared<ControllerLaunchProcess::ForwardingCounters>()
            : found->second.counters;
    if (found != processes_.end())
      found->second.current.reset();
    const auto spawned = spawner_->spawn(command.argv, artifacts_root,
                                         parent_death_signal(command));
    auto process =
        std::shared_ptr<ControllerLaunchProcess>(new ControllerLaunchProcess(
            command, spawned.pid, artifacts_root / "ros", std::move(context),
            counters));
    processes_.insert_or_assign(command.controllable_component_id,
                                Slot{process, std::move(counters)});
    lock.unlock();
    process->output_->attach_wake(spawner_->wake_fd);
    spawner_->register_output(spawned.output_read_fd, process->output_);
    process->append_start_header();
    return {decision, std::move(process)};
  }
}

std::shared_ptr<CellLaunchProcess> ControllerLaunchSupervisor::start_cell_process(
    const CellLaunchCommand &command,
    const std::filesystem::path &artifacts_root, std::string context) {
  if (command.cell_id.empty() || command.identity.empty() || command.argv.empty())
    fail("cell launch requires an exact cell, launch identity and argv");
  switch (command.role) {
  case CellLaunchRole::kSceneBarrier:
    if (command.observer_source_id.has_value())
      fail("cell scene barrier must not name an observer source");
    break;
  case CellLaunchRole::kPoseObserver:
    if (!command.observer_source_id.has_value() ||
        command.observer_source_id->empty())
      fail("cell pose observer requires its exact configured source identity");
    break;
  default:
    fail("cell launch role is unknown");
  }
  prepare_upstream_log_directory(artifacts_root);
  std::lock_guard submission_lock(mutex_);
  if (host_shutdown_started_)
    fail("cell launch submission is closed for host shutdown");
  const auto spawned = spawner_->spawn(command.argv, artifacts_root);
  auto occurrence = std::shared_ptr<ControllerLaunchProcess>(
      new ControllerLaunchProcess(command, spawned.pid, artifacts_root / "ros",
                                  std::move(context)));
  occurrence->output_->attach_wake(spawner_->wake_fd);
  spawner_->register_output(spawned.output_read_fd, occurrence->output_);
  occurrence->append_start_header();
  return std::shared_ptr<CellLaunchProcess>(
      new CellLaunchProcess(std::move(occurrence)));
}

bool ControllerLaunchSupervisor::discard_candidate(
    const Ensured &candidate_launch) {
  if (candidate_launch.decision == ControllerLaunchDecision::kKeep ||
      candidate_launch.process == nullptr) {
    return false;
  }
  return retire_exact(candidate_launch.process);
}

bool ControllerLaunchSupervisor::retire_exact(
    const std::shared_ptr<ControllerLaunchProcess> &expected) {
  if (!expected)
    return false;
  const auto &component = expected->command().controllable_component_id;
  {
    std::lock_guard lock(mutex_);
    const auto found = processes_.find(component);
    if (found == processes_.end() || found->second.current != expected)
      return false;
  }
  // Keep the predecessor registered until containment completes. A concurrent
  // ensure must also stop it before starting the successor (Q66).
  expected->stop(stop_grace_);
  std::lock_guard lock(mutex_);
  const auto found = processes_.find(component);
  if (found != processes_.end() && found->second.current == expected)
    found->second.current.reset();
  return true;
}

ControllerLaunchSupervisor::HostShutdownSnapshot
ControllerLaunchSupervisor::BeginHostShutdown(
    const std::vector<std::shared_ptr<CellLaunchProcess>> &cells) {
  HostShutdownSnapshot snapshot;
  {
    std::lock_guard lock(mutex_);
    host_shutdown_started_ = true;
    for (const auto &[component, slot] : processes_) {
      static_cast<void>(component);
      if (slot.current)
        snapshot.push_back(slot.current);
    }
    for (const auto &cell : cells) {
      if (cell && std::find(snapshot.begin(), snapshot.end(),
                            cell->occurrence_) == snapshot.end())
        snapshot.push_back(cell->occurrence_);
    }
  }
  for (const auto &process : snapshot)
    process->begin_stop(stop_grace_);
  return snapshot;
}

ControllerLaunchSupervisor::ShutdownProgress
ControllerLaunchSupervisor::PollHostShutdown(
    const HostShutdownSnapshot &snapshot) noexcept {
  ShutdownProgress progress;
  progress.total = snapshot.size();
  for (const auto &process : snapshot) {
    const bool complete = process->poll_stop();
    {
      std::lock_guard retirement_lock(process->retirement_mutex_);
      std::lock_guard lock(process->mutex_);
      progress.signalled += process->stop_signal_sent_;
      progress.escalated += process->stop_kill_sent_;
      progress.reaped += process->exact_leader_reaped_;
      progress.drained +=
          process->stop_scanned_drained_ && process->exact_leader_reaped_;
      progress.unknown += complete && !(process->stop_scanned_drained_ &&
                                        process->exact_leader_reaped_);
      if (!complete) {
        ++progress.pending;
        if (progress.pending_pid < 0)
          progress.pending_pid = process->pid_;
      }
    }
    if (complete) {
      if (const auto *component =
              std::get_if<ControllerLaunchCommand>(&process->command_)) {
        std::lock_guard lock(mutex_);
        const auto found =
            processes_.find(component->controllable_component_id);
        if (found != processes_.end() && found->second.current == process)
          found->second.current.reset();
      }
    }
  }
  return progress;
}

void ControllerLaunchSupervisor::stop_all() noexcept {
  HostShutdownSnapshot current;
  {
    std::lock_guard lock(mutex_);
    for (const auto &[component, slot] : processes_) {
      static_cast<void>(component);
      if (slot.current)
        current.push_back(slot.current);
    }
  }
  // Ordinary stop_all does not close submission. Its exact snapshot preserves
  // any concurrently published successor, while all original graces overlap.
  for (const auto &process : current)
    process->begin_stop(stop_grace_);
  while (PollHostShutdown(current).pending != 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void ControllerLaunchSupervisor::enqueue_host_log(std::string line) noexcept {
  try {
    line = bounded_text(std::move(line), 4096U);
    if (line.empty() || line.back() != '\n')
      line += '\n';
    spawner_->host_output->append(line);
  } catch (...) {
    // A best-effort notice cannot change committed host state.
  }
}

std::optional<ControllerLaunchForwardingCounters>
ControllerLaunchSupervisor::component_output_counters(
    const std::string &component) const {
  std::shared_ptr<ControllerLaunchProcess::ForwardingCounters> counters;
  {
    std::lock_guard lock(mutex_);
    const auto found = processes_.find(component);
    if (found == processes_.end())
      return std::nullopt;
    counters = found->second.counters;
  }
  return counters->read();
}

ControllerLaunchForwardingCounters
ControllerLaunchSupervisor::host_output_counters() const {
  return spawner_->host_output->counters();
}

std::optional<ControllerLaunchObservation>
ControllerLaunchSupervisor::observe(const std::string &controllable_component_id) {
  std::shared_ptr<ControllerLaunchProcess> current;
  {
    std::lock_guard lock(mutex_);
    const auto found = processes_.find(controllable_component_id);
    if (found == processes_.end() || !found->second.current)
      return std::nullopt;
    current = found->second.current;
  }
  return ControllerLaunchObservation{current->command().identity,
                                     current->command().keep_across_apply,
                                     !current->exited().has_value()};
}

bool ControllerLaunchProcess::capture_exit_readiness(
    const std::function<ControllerLaunchReadinessEvidence()> &readiness) {
  {
    std::lock_guard lock(mutex_);
    observe_exit_locked();
    if (!exit_evidence_)
      return false;
    if (readiness_capture_complete_)
      return true;
    if (readiness_capture_started_)
      return false;
    readiness_capture_started_ = true;
  }
  // This is the existing cached handle callback, never a synchronous ROS query.
  // A concurrent status tick must not queue effects until this attempt
  // finishes.
  std::optional<ControllerLaunchReadinessEvidence> captured;
  try {
    if (readiness) {
      captured = readiness();
      if (captured->captured_utc.empty())
        captured->captured_utc = observed_utc_now();
    }
  } catch (...) {
    // Unknown readiness preserves the original primary fault.
  }
  std::lock_guard lock(mutex_);
  exit_evidence_->readiness = std::move(captured);
  readiness_capture_complete_ = true;
  return true;
}

ControllerLaunchCachedFaultState
ControllerLaunchProcess::ReadCachedFaultState() const {
  std::lock_guard lock(mutex_);
  return {stopping_, unexpected_exit_observed_, exit_evidence_, fault_reason_};
}

std::string DescribeControllerLaunchFault(
    const ControllerLaunchExitEvidence &evidence) {
  std::string reason =
      "controller_launch_exited: " + evidence.description +
      " component=" + log_field(evidence.controllable_component_id) +
      " pid=" + std::to_string(evidence.pid) +
      " observed_utc=" + evidence.observed_utc +
      " core_dumped=" + (evidence.core_dumped ? "true" : "false") +
      " robot_description_sha256=" +
      log_field(evidence.robot_description_sha256) +
      " controllers_sha256=" + log_field(evidence.controllers_sha256) +
      " upstream_log_directory=" +
      log_field(evidence.upstream_log_directory.string());
  reason +=
      " context=" + bounded_text(log_field(evidence.request_context), 300U);
  reason = bounded_text(std::move(reason), 1024U);
  return reason;
}

std::optional<std::string> ProjectControllerLaunchFault(
    const ControllerLaunchCachedFaultState &state) {
  if (state.stopping && !state.unexpected_exit_observed)
    return std::nullopt;
  if (state.fault_reason)
    return state.fault_reason;
  if (!state.exit_evidence)
    return std::nullopt;
  return DescribeControllerLaunchFault(*state.exit_evidence);
}

std::function<std::optional<std::string>()>
ControllerLaunchSupervisor::fault_source(
    std::shared_ptr<ControllerLaunchProcess> process,
    std::function<ControllerLaunchReadinessEvidence()> readiness) {
  if (process == nullptr)
    fail("fault source has no process");
  return [process = std::move(process),
          readiness = std::move(readiness)]() -> std::optional<std::string> {
    const auto evidence = process->exit_evidence();
    {
      std::lock_guard lock(process->mutex_);
      // A stop this host ordered (restart or teardown) is not a failure.
      if (process->stopping_ && !process->unexpected_exit_observed_)
        return std::nullopt;
      if (process->fault_reason_.has_value())
        return process->fault_reason_;
    }
    if (!evidence.has_value())
      return std::nullopt;
    static_cast<void>(process->capture_exit_readiness(readiness));
    auto reason = DescribeControllerLaunchFault(*evidence);
    std::lock_guard lock(process->mutex_);
    if (process->stopping_ && !process->unexpected_exit_observed_)
      return std::nullopt;
    if (!process->fault_reason_.has_value()) {
      process->fault_reason_ = std::move(reason);
    }
    return process->fault_reason_;
  };
}

bool ControllerLaunchSupervisor::request_unexpected_exit_cleanup(
    const std::shared_ptr<ControllerLaunchProcess> &expected,
    core::IScheduler &scheduler, const std::int64_t now_ns,
    std::function<ControllerLaunchReadinessEvidence()> readiness) {
  if (!expected || now_ns < 0)
    return false;
  if (!expected->capture_exit_readiness(readiness))
    return false;
  {
    std::lock_guard lock(expected->mutex_);
    expected->observe_exit_locked();
    if (!expected->unexpected_exit_observed_ ||
        expected->cleanup_.state != ControllerLaunchCleanupState::kNotRequested)
      return false;
    expected->cleanup_.state = ControllerLaunchCleanupState::kPending;
  }
  try {
    // A due callback may start before schedule returns. The consumed state and
    // owning occurrence are published first; no supervisor/host is captured.
    static_cast<void>(scheduler.schedule(
        now_ns, [expected, grace = stop_grace_] { expected->stop(grace); },
        core::SchedulerPriority::Background));
    return true;
  } catch (...) {
    std::lock_guard lock(expected->mutex_);
    if (expected->cleanup_.state == ControllerLaunchCleanupState::kPending)
      expected->cleanup_.state = ControllerLaunchCleanupState::kUnknown;
    return false;
  }
}

bool ControllerLaunchSupervisor::retire_cell_process(
    const std::shared_ptr<CellLaunchProcess> &process) {
  if (!process) return false;
  process->occurrence_->stop(stop_grace_);
  std::lock_guard lock(process->occurrence_->mutex_);
  return process->occurrence_->exact_leader_reaped_;
}

ControllerLaunchSupervisor &host_controller_launch_supervisor() {
  static ControllerLaunchSupervisor supervisor;
  return supervisor;
}

} // namespace manyforge::roboplan_backend

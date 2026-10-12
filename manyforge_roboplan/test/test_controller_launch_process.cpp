#include <manyforge_roboplan/controller_launch_process.hpp>

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <sstream>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

using namespace manyforge::roboplan_backend;
using namespace std::chrono_literals;

class TemporaryRoot final {
public:
  TemporaryRoot() {
    static std::atomic<int> counter{0};
    path_ = std::filesystem::temp_directory_path() /
            ("manyforge-controller-launch-" + std::to_string(::getpid()) +
             "-" + std::to_string(counter.fetch_add(1)));
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }
  ~TemporaryRoot() {
    std::error_code ignored;
    std::filesystem::remove_all(path_, ignored);
  }
  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] std::string read_file(const std::filesystem::path &path) {
  std::ifstream stream(path, std::ios::binary);
  std::ostringstream buffer;
  buffer << stream.rdbuf();
  return buffer.str();
}

[[nodiscard]] ControllerLaunchArtifacts
mock_artifacts(const std::filesystem::path &directory,
               const std::string &description_sha = "sha256:aa",
               const std::string &component = "ui6i_lite6") {
  ControllerLaunchArtifacts value;
  value.lane = ControllerLaunchLane::kMock;
  value.controllable_component_id = component;
  value.controller_profile = "lite6_lite_gripper";
  value.directory = directory / component;
  value.robot_description_file = value.directory / "robot_description.urdf";
  value.controllers_file = value.directory / "controllers.yaml";
  value.robot_description_sha256 = description_sha;
  value.controllers_sha256 = "sha256:bb";
  value.controllers_yaml = "mock_controller:\n  update_rate: 100\n";
  value.runtime_namespace = "/manyforge/" + component;
  value.arm_controller_name = "lite6_traj_controller";
  value.gripper_controller_name = "lite_gripper_controller";
  value.frame_prefix = component + "/";
  value.launch_arguments = {
      "robot_description_file:=" + value.robot_description_file.string(),
      "controllers_file:=" + value.controllers_file.string(),
      "runtime_namespace:=" + value.runtime_namespace,
      "controller_manager_name:=controller_manager",
      "arm_controller_name:=lite6_traj_controller",
      "gripper_controller_name:=lite_gripper_controller",
      "frame_prefix:=" + value.frame_prefix,
  };
  return value;
}

[[nodiscard]] ControllerLaunchArtifacts
isaac_artifacts(const std::filesystem::path &directory,
                const std::string &frame_prefix = "") {
  ControllerLaunchArtifacts value;
  value.lane = ControllerLaunchLane::kIsaac;
  value.controllable_component_id = "isaac_ur10e_light_robot";
  value.controller_profile = "ur10e";
  value.directory = directory / value.controllable_component_id;
  value.robot_description_file = value.directory / "robot_description.urdf";
  value.controllers_file = value.directory / "controllers.yaml";
  value.robot_description_sha256 = "sha256:cc";
  value.controllers_sha256 = "sha256:dd";
  value.controllers_yaml = "isaac_controller:\n  update_rate: 100\n";
  value.runtime_namespace = "/manyforge/isaac_ur10e_light_robot";
  value.arm_controller_name = "isaac_arm_controller";
  value.frame_prefix = frame_prefix;
  return value;
}

[[nodiscard]] IsaacControllerLane isaac_lane() {
  return IsaacControllerLane{"IsaacUR10eTopicBasedSystem",
                             "/isaac_joint_commands",
                             "/isaac_joint_states",
                             {0.0, 0.0, 0.0, 0.0, 0.0, 0.0}};
}

[[nodiscard]] IsaacControllerLaunchInputs isaac_inputs() {
  IsaacControllerLaunchInputs inputs;
  inputs.world_uri = "file:/worlds/light.usda";
  inputs.expected_world_name = "light";
  inputs.scene_startup_timeout_s = "180.0";
  inputs.scene_service_timeout_s = "120.0";
  return inputs;
}

TEST(ControllerLaunchCommand, MockCommandIsTheTrack2LaunchWithTheArtifactArguments) {
  TemporaryRoot root;
  const auto artifacts = mock_artifacts(root.path());
  const auto command = mock_controller_launch_command(artifacts);
  std::vector<std::string> expected{"ros2", "launch", "manyforge_ros_bringup",
                                    "mock_control.launch.py"};
  expected.insert(expected.end(), artifacts.launch_arguments.begin(),
                  artifacts.launch_arguments.end());
  EXPECT_EQ(command.argv, expected);
  EXPECT_EQ(command.controllable_component_id, "ui6i_lite6");
  EXPECT_EQ(command.lane, ControllerLaunchLane::kMock);
  EXPECT_EQ(command.controller_profile, artifacts.controller_profile);
  EXPECT_EQ(command.runtime_namespace, artifacts.runtime_namespace);
  EXPECT_EQ(command.arm_controller_name, artifacts.arm_controller_name);
  EXPECT_EQ(command.gripper_controller_name, artifacts.gripper_controller_name);
  EXPECT_EQ(command.frame_prefix, artifacts.frame_prefix);
  EXPECT_EQ(command.controllers_yaml, artifacts.controllers_yaml);
  EXPECT_TRUE(command.keep_across_apply);
}

TEST(ControllerLaunchCommand, MockIdentityFollowsContentNotTheActivationPaths) {
  TemporaryRoot first;
  TemporaryRoot second;
  const auto a = mock_controller_launch_command(mock_artifacts(first.path()));
  const auto b = mock_controller_launch_command(mock_artifacts(second.path()));
  EXPECT_NE(a.argv, b.argv);
  EXPECT_EQ(a.identity, b.identity);
  const auto changed = mock_controller_launch_command(
      mock_artifacts(second.path(), "sha256:other"));
  EXPECT_NE(a.identity, changed.identity);
  auto prefixed = mock_artifacts(second.path());
  prefixed.frame_prefix.clear();
  EXPECT_NE(a.identity, mock_controller_launch_command(prefixed).identity);
}

TEST(ControllerLaunchCommand, MockCommandFailsClosed) {
  TemporaryRoot root;
  auto isaac = isaac_artifacts(root.path());
  EXPECT_THROW(static_cast<void>(mock_controller_launch_command(isaac)),
               ControllerLaunchProcessError);
  auto empty = mock_artifacts(root.path());
  empty.launch_arguments.clear();
  EXPECT_THROW(static_cast<void>(mock_controller_launch_command(empty)),
               ControllerLaunchProcessError);
  auto repeated = mock_artifacts(root.path());
  repeated.launch_arguments.push_back("frame_prefix:=x/");
  EXPECT_THROW(static_cast<void>(mock_controller_launch_command(repeated)),
               ControllerLaunchProcessError);
  // An empty value (e.g. a gripper-less arm's `gripper_controller_name:=`)
  // is never passed: ros2 launch rejects it before the controller starts.
  auto empty_value = mock_artifacts(root.path());
  empty_value.launch_arguments.back() = "frame_prefix:=";
  EXPECT_THROW(static_cast<void>(mock_controller_launch_command(empty_value)),
               ControllerLaunchProcessError);
  auto isaac_empty = isaac_inputs();
  isaac_empty.additional_arguments = {"base_xyz:="};
  EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                   isaac_artifacts(root.path()), isaac_lane(), isaac_empty)),
               ControllerLaunchProcessError);
  auto unhashed = mock_artifacts(root.path());
  unhashed.controllers_sha256.clear();
  EXPECT_THROW(static_cast<void>(mock_controller_launch_command(unhashed)),
               ControllerLaunchProcessError);
}

TEST(ControllerLaunchCommand, IsaacCommandCarriesTheContractArguments) {
  TemporaryRoot root;
  const auto artifacts = isaac_artifacts(root.path(), "isaac_ur10e_light_robot/");
  auto inputs = isaac_inputs();
  inputs.manager_run_root = "/run/manager";
  inputs.pose_observer_site_config = "/etc/observer.json";
  inputs.additional_arguments = {"base_xyz:=0 0 0.1"};
  const auto command =
      isaac_controller_launch_command(artifacts, isaac_lane(), inputs);
  const std::vector<std::string> expected{
      "ros2", "launch", "manyforge_ros_bringup",
      "isaac_topic_based_control.launch.py",
      "robot_description_file:=" + artifacts.robot_description_file.string(),
      "controllers_file:=" + artifacts.controllers_file.string(),
      "isaac_profile:=ur10e",
      "runtime_namespace:=/manyforge/isaac_ur10e_light_robot",
      "controller_manager_name:=controller_manager",
      "arm_controller_name:=isaac_arm_controller",
      "joint_commands_topic:=/isaac_joint_commands",
      "joint_states_topic:=/isaac_joint_states",
      "frame_prefix:=isaac_ur10e_light_robot/",
      "world_uri:=file:/worlds/light.usda",
      "expected_world_name:=light",
      "scene_startup_timeout_s:=180.0",
      "scene_service_timeout_s:=120.0",
      "manager_run_root:=/run/manager",
      "pose_observer_site_config:=/etc/observer.json",
      "base_xyz:=0 0 0.1"};
  EXPECT_EQ(command.argv, expected);
  EXPECT_EQ(command.controller_profile, artifacts.controller_profile);
  EXPECT_EQ(command.runtime_namespace, artifacts.runtime_namespace);
  EXPECT_EQ(command.arm_controller_name, artifacts.arm_controller_name);
  EXPECT_EQ(command.gripper_controller_name, artifacts.gripper_controller_name);
  EXPECT_EQ(command.frame_prefix, artifacts.frame_prefix);
  EXPECT_EQ(command.controllers_yaml, artifacts.controllers_yaml);
  EXPECT_TRUE(command.keep_across_apply);
  // An empty retained deployment prefix passes no frame-prefix argument (Q82).
  const auto single = isaac_controller_launch_command(
      isaac_artifacts(root.path()), isaac_lane(), isaac_inputs());
  for (const auto &argument : single.argv) {
    EXPECT_NE(argument.rfind("frame_prefix:=", 0), 0U) << argument;
  }
}

TEST(ControllerLaunchCommand, ManagedCellOwnershipIsExplicitAndPartOfIdentity) {
  TemporaryRoot root;
  auto inputs = isaac_inputs();
  const auto standalone = isaac_controller_launch_command(
      isaac_artifacts(root.path()), isaac_lane(), inputs);
  inputs.manager_cell_world_owned = true;
  const auto managed = isaac_controller_launch_command(
      isaac_artifacts(root.path()), isaac_lane(), inputs);
  EXPECT_EQ(std::count(managed.argv.begin(), managed.argv.end(),
                       "manager_cell_world_owned:=true"), 1);
  EXPECT_NE(standalone.identity, managed.identity);
  EXPECT_EQ(standalone.argv.size() + 1U, managed.argv.size());
  inputs.additional_arguments = {"manager_cell_world_owned:=false"};
  EXPECT_THROW((void)isaac_controller_launch_command(
                   isaac_artifacts(root.path()), isaac_lane(), inputs),
               ControllerLaunchProcessError);
}

TEST(ControllerLaunchCommand,
     IsaacIdentityKeepsOnlyTheSameWorldFilesAndLiteralPrefix) {
  TemporaryRoot first;
  TemporaryRoot second;
  const auto inputs = isaac_inputs();
  const auto original = isaac_controller_launch_command(
      isaac_artifacts(first.path(), "authored//custom"), isaac_lane(), inputs);
  const auto same = isaac_controller_launch_command(
      isaac_artifacts(second.path(), "authored//custom"), isaac_lane(), inputs);
  EXPECT_NE(original.argv, same.argv);
  EXPECT_EQ(original.identity, same.identity);
  EXPECT_TRUE(same.keep_across_apply);
  EXPECT_EQ(
      decide_controller_launch(
          ControllerLaunchObservation{original.identity, true, true}, same),
      ControllerLaunchDecision::kKeep);

  auto changed_world = inputs;
  changed_world.world_uri = "file:/worlds/other.usda";
  auto changed_description = isaac_artifacts(second.path(), "authored//custom");
  changed_description.robot_description_sha256 = "sha256:new-description";
  auto changed_controllers = isaac_artifacts(second.path(), "authored//custom");
  changed_controllers.controllers_sha256 = "sha256:new-controllers";
  for (const auto &changed :
       {isaac_controller_launch_command(
            isaac_artifacts(second.path(), "authored//custom"), isaac_lane(),
            changed_world),
        isaac_controller_launch_command(changed_description, isaac_lane(),
                                        inputs),
        isaac_controller_launch_command(changed_controllers, isaac_lane(),
                                        inputs),
        isaac_controller_launch_command(
            isaac_artifacts(second.path(), "authored/custom"), isaac_lane(),
            inputs)}) {
    EXPECT_NE(changed.identity, original.identity);
    EXPECT_EQ(decide_controller_launch(
                  ControllerLaunchObservation{original.identity, true, true},
                  changed),
              ControllerLaunchDecision::kRestart);
  }
}

TEST(ControllerLaunchCommand, IsaacCommandRequiresCallerInputsAndNeverOverridesTheHost) {
  TemporaryRoot root;
  const auto artifacts = isaac_artifacts(root.path());
  for (auto missing : {&IsaacControllerLaunchInputs::world_uri,
                       &IsaacControllerLaunchInputs::expected_world_name,
                       &IsaacControllerLaunchInputs::scene_startup_timeout_s,
                       &IsaacControllerLaunchInputs::scene_service_timeout_s}) {
    auto inputs = isaac_inputs();
    inputs.*missing = "";
    EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                     artifacts, isaac_lane(), inputs)),
                 ControllerLaunchProcessError);
  }
  auto unpaired = isaac_inputs();
  unpaired.manager_run_root = "/run/manager";
  EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                   artifacts, isaac_lane(), unpaired)),
               ControllerLaunchProcessError);
  for (const std::string override_argument :
       {"joint_states_topic:=/elsewhere", "runtime_namespace:=/x",
        "world_uri:=file:/again"}) {
    auto inputs = isaac_inputs();
    inputs.additional_arguments = {override_argument};
    EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                     artifacts, isaac_lane(), inputs)),
                 ControllerLaunchProcessError)
        << override_argument;
  }
  auto no_topics = isaac_lane();
  no_topics.joint_states_topic.clear();
  EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                   artifacts, no_topics, isaac_inputs())),
               ControllerLaunchProcessError);
  EXPECT_THROW(static_cast<void>(isaac_controller_launch_command(
                   mock_artifacts(root.path()), isaac_lane(), isaac_inputs())),
               ControllerLaunchProcessError);
}

TEST(ControllerLaunchDecision, StartKeepAndRestartRules) {
  TemporaryRoot root;
  const auto mock = mock_controller_launch_command(mock_artifacts(root.path()));
  EXPECT_EQ(decide_controller_launch(std::nullopt, mock),
            ControllerLaunchDecision::kStart);
  EXPECT_EQ(decide_controller_launch(
                ControllerLaunchObservation{mock.identity, true, true}, mock),
            ControllerLaunchDecision::kKeep);
  // The process exited: the next activation starts a new one.
  EXPECT_EQ(decide_controller_launch(
                ControllerLaunchObservation{mock.identity, true, false}, mock),
            ControllerLaunchDecision::kRestart);
  // The description or controllers changed.
  EXPECT_EQ(decide_controller_launch(
                ControllerLaunchObservation{"other", true, true}, mock),
            ControllerLaunchDecision::kRestart);
  // DX224.3: unchanged Isaac identity keeps its launch across Apply.
  const auto isaac = isaac_controller_launch_command(
      isaac_artifacts(root.path()), isaac_lane(), isaac_inputs());
  EXPECT_EQ(decide_controller_launch(
                ControllerLaunchObservation{isaac.identity, true, true}, isaac),
            ControllerLaunchDecision::kKeep);
}

/// A fake `ros2` on PATH: records its argv and descriptors, then runs until
/// interrupted (or exits at once when FAKE_LAUNCH_EXIT is set).
class FakeLaunch final {
public:
  FakeLaunch() {
    const auto bin = root_.path() / "bin";
    std::filesystem::create_directories(bin);
    const auto script = bin / "ros2";
    {
      std::ofstream out(script);
      out << "#!/bin/sh\n"
          << "out=\"" << (root_.path() / "out").string() << "\"\n"
          << "mkdir -p \"$out\"\n"
          << "printf '%s\\n' \"$@\" > \"$out/$$.args\"\n"
          << "ls /proc/$$/fd > \"$out/$$.fds\"\n"
          << "printf '%s' \"$ROS_LOG_DIR\" > \"$out/$$.roslog\"\n"
          << "if [ -n \"$FAKE_LAUNCH_EXIT\" ]; then exit "
             "\"$FAKE_LAUNCH_EXIT\"; fi\n"
          << "trap 'exit 0' INT TERM\n"
          << "while :; do sleep 0.05; done\n";
    }
    std::filesystem::permissions(script, std::filesystem::perms::owner_all);
    const char *path = std::getenv("PATH");
    previous_path_ = path == nullptr ? "" : path;
    ::setenv("PATH", (bin.string() + ":" + previous_path_).c_str(), 1);
  }
  ~FakeLaunch() {
    ::setenv("PATH", previous_path_.c_str(), 1);
    ::unsetenv("FAKE_LAUNCH_EXIT");
  }
  [[nodiscard]] const std::filesystem::path &root() const { return root_.path(); }
  [[nodiscard]] std::filesystem::path out(const int pid,
                                          const std::string &suffix) const {
    return root_.path() / "out" / (std::to_string(pid) + suffix);
  }
  [[nodiscard]] bool wait_for(const std::filesystem::path &path) const {
    for (int attempt = 0; attempt < 200; ++attempt) {
      if (std::filesystem::exists(path) &&
          std::filesystem::file_size(path) > 0U) {
        return true;
      }
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

private:
  TemporaryRoot root_;
  std::string previous_path_;
};

[[nodiscard]] bool alive(const int pid) { return ::kill(pid, 0) == 0; }

// A node in the fake launch's process group. The test keeps parenthood so it
// can reap the probe and assert actual termination even under container PID 1.
class GroupChild final {
public:
  explicit GroupChild(const int group_id) {
    int ready[2];
    if (::pipe(ready) != 0)
      throw std::runtime_error("cannot create group child pipe");
    pid_ = ::fork();
    if (pid_ == 0) {
      ::close(ready[0]);
      const char joined = ::setpgid(0, group_id) == 0 ? '1' : '0';
      ::signal(SIGINT, SIG_IGN);
      ::signal(SIGTERM, SIG_IGN);
      const auto ignored = ::write(ready[1], &joined, 1U);
      static_cast<void>(ignored);
      ::close(ready[1]);
      if (joined != '1')
        ::_exit(1);
      while (true)
        ::pause();
    }
    ::close(ready[1]);
    char joined = '0';
    const auto received = ::read(ready[0], &joined, 1U);
    ::close(ready[0]);
    if (pid_ < 0 || received != 1 || joined != '1') {
      if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, nullptr, 0);
      }
      throw std::runtime_error("cannot join fake launch process group");
    }
  }
  ~GroupChild() {
    if (pid_ > 0 && !reaped_) {
      ::kill(pid_, SIGKILL);
      while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
    }
  }
  [[nodiscard]] bool running() const { return ::kill(pid_, 0) == 0; }
  [[nodiscard]] bool reap_killed() {
    int status = 0;
    for (int attempt = 0; attempt < 200; ++attempt) {
      const auto result = ::waitpid(pid_, &status, WNOHANG);
      if (result == pid_) {
        reaped_ = true;
        return WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
      }
      if (result < 0)
        return false;
      std::this_thread::sleep_for(10ms);
    }
    return false;
  }

private:
  int pid_{-1};
  bool reaped_{false};
};

TEST(ControllerLaunchSupervisor, StartsKeepsAndRestartsOneLaunchPerComponent) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto log = fake.root() / "launch.artifacts";
  const auto first_command =
      mock_controller_launch_command(mock_artifacts(fake.root() / "epoch-1"));
  const auto first = supervisor.ensure(first_command, log);
  EXPECT_EQ(first.decision, ControllerLaunchDecision::kStart);
  ASSERT_TRUE(fake.wait_for(fake.out(first.process->pid(), ".args")));
  std::vector<std::string> recorded;
  {
    std::istringstream lines(read_file(fake.out(first.process->pid(), ".args")));
    for (std::string line; std::getline(lines, line);) recorded.push_back(line);
  }
  EXPECT_EQ(recorded, std::vector<std::string>(first_command.argv.begin() + 1,
                                               first_command.argv.end()));

  // Q21: same robot and description at the next activation keeps the process
  // even though the artifacts were written below another epoch.
  const auto kept = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root() / "epoch-2")),
      log);
  EXPECT_EQ(kept.decision, ControllerLaunchDecision::kKeep);
  EXPECT_EQ(kept.process, first.process);
  EXPECT_TRUE(alive(first.process->pid()));

  // A changed description replaces it: the predecessor ends first (Q66).
  const auto changed = supervisor.ensure(
      mock_controller_launch_command(
          mock_artifacts(fake.root() / "epoch-3", "sha256:changed")),
      log);
  EXPECT_EQ(changed.decision, ControllerLaunchDecision::kRestart);
  EXPECT_NE(changed.process->pid(), first.process->pid());
  EXPECT_TRUE(first.process->exited().has_value());
  // An ordered stop is not a component failure.
  EXPECT_FALSE(ControllerLaunchSupervisor::fault_source(first.process)()
                   .has_value());
  ASSERT_TRUE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_TRUE(supervisor.observe("ui6i_lite6")->running);

  supervisor.stop_all();
  EXPECT_TRUE(changed.process->exited().has_value());
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
}

TEST(ControllerLaunchSupervisor, UnchangedIsaacApplyKeepsTheExactProcess) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto log = fake.root() / "launch.artifacts";
  const auto first = supervisor.ensure(
      isaac_controller_launch_command(isaac_artifacts(fake.root() / "epoch-1"),
                                      isaac_lane(), isaac_inputs()),
      log);
  ASSERT_TRUE(fake.wait_for(fake.out(first.process->pid(), ".args")));
  const auto same = supervisor.ensure(
      isaac_controller_launch_command(isaac_artifacts(fake.root() / "epoch-2"),
                                      isaac_lane(), isaac_inputs()),
      log);
  EXPECT_EQ(same.decision, ControllerLaunchDecision::kKeep);
  EXPECT_EQ(same.process, first.process);
  EXPECT_EQ(same.process->command().argv, first.process->command().argv);
  EXPECT_NE(same.process->command().argv[4].find("epoch-1"), std::string::npos);
  EXPECT_EQ(same.process->command().controllers_yaml,
            isaac_artifacts(fake.root()).controllers_yaml);
  EXPECT_FALSE(supervisor.discard_candidate(same));
  EXPECT_TRUE(alive(first.process->pid()));

  const auto changed = supervisor.ensure(
      isaac_controller_launch_command(
          isaac_artifacts(fake.root() / "epoch-3", "authored/custom"),
          isaac_lane(), isaac_inputs()),
      log);
  EXPECT_EQ(changed.decision, ControllerLaunchDecision::kRestart);
  EXPECT_NE(changed.process, first.process);
  EXPECT_TRUE(first.process->exited().has_value());
  EXPECT_FALSE(supervisor.discard_candidate(first));
  EXPECT_TRUE(supervisor.observe("isaac_ur10e_light_robot")->running);
}

TEST(ControllerLaunchSupervisor,
     PrivateRosLogEnvironmentFollowsActualOccurrence) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto inherited = std::getenv("ROS_LOG_DIR");
  const auto host_value = inherited == nullptr
                              ? std::optional<std::string>()
                              : std::optional<std::string>(inherited);
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  const auto first_root = fake.root() / "first-artifacts";
  const auto second_root = fake.root() / "second-artifacts";
  const auto first = supervisor.ensure(command, first_root);
  ASSERT_TRUE(fake.wait_for(fake.out(first.process->pid(), ".roslog")));
  EXPECT_EQ(read_file(fake.out(first.process->pid(), ".roslog")),
            (first_root / "ros").string());
  const auto kept = supervisor.ensure(command, second_root);
  EXPECT_EQ(kept.process, first.process);
  EXPECT_FALSE(std::filesystem::exists(second_root));
  EXPECT_TRUE(std::filesystem::is_directory(first_root / "ros"));
  const auto after = std::getenv("ROS_LOG_DIR");
  EXPECT_EQ(after == nullptr ? std::optional<std::string>()
                             : std::optional<std::string>(after),
            host_value);
  EXPECT_TRUE(supervisor.discard_candidate(first));
  EXPECT_EQ(first.process->exit_evidence()->upstream_log_directory,
            first_root / "ros");
}

TEST(ControllerLaunchSupervisor,
     InvalidArtifactRootRefusesBeforeProcessEffects) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  EXPECT_THROW(static_cast<void>(supervisor.ensure(command, {})),
               ControllerLaunchProcessError);
  EXPECT_THROW(
      static_cast<void>(supervisor.ensure(command, "relative-artifacts")),
      ControllerLaunchProcessError);
  EXPECT_FALSE(
      supervisor.observe(command.controllable_component_id).has_value());
  EXPECT_FALSE(std::filesystem::exists(fake.root() / "out"));
  const auto original =
      supervisor.ensure(command, fake.root() / "original-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(original.process->pid(), ".args")));
  auto changed = command;
  changed.identity += " changed";
  const auto blocked = fake.root() / "blocked-artifacts";
  {
    std::ofstream file(blocked);
    file << "a regular file cannot be a writable artifact directory";
  }
  EXPECT_THROW(static_cast<void>(supervisor.ensure(changed, {})),
               ControllerLaunchProcessError);
  EXPECT_THROW(static_cast<void>(supervisor.ensure(changed, blocked)),
               ControllerLaunchProcessError);
  EXPECT_TRUE(alive(original.process->pid()));
  const auto observed = supervisor.observe(command.controllable_component_id);
  ASSERT_TRUE(observed.has_value());
  EXPECT_EQ(observed->identity, original.process->command().identity);
  EXPECT_TRUE(observed->running);
}

TEST(ControllerLaunchSupervisor,
     DiscardCandidatePreservesBorrowedAndSupersedingProcesses) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto log = fake.root() / "launch.artifacts";
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  const auto candidate = supervisor.ensure(command, log);
  ASSERT_TRUE(fake.wait_for(fake.out(candidate.process->pid(), ".args")));
  const auto borrowed = supervisor.ensure(command, log);
  ASSERT_EQ(borrowed.decision, ControllerLaunchDecision::kKeep);
  EXPECT_FALSE(supervisor.discard_candidate(borrowed));
  EXPECT_FALSE(supervisor.discard_candidate(
      {ControllerLaunchDecision::kStart, nullptr}));
  EXPECT_TRUE(alive(candidate.process->pid()));

  const auto peer =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:aa", "ui6i_ur5e")),
                        fake.root() / "peer.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(peer.process->pid(), ".args")));
  EXPECT_TRUE(supervisor.discard_candidate(candidate));
  EXPECT_FALSE(supervisor.discard_candidate(candidate));
  EXPECT_TRUE(candidate.process->exited().has_value());
  EXPECT_FALSE(ControllerLaunchSupervisor::fault_source(candidate.process)()
                   .has_value());
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_TRUE(alive(peer.process->pid()));

  // Even a new process with the same launch identity is a different occurrence.
  const auto successor = supervisor.ensure(command, log);
  ASSERT_TRUE(fake.wait_for(fake.out(successor.process->pid(), ".args")));
  EXPECT_EQ(successor.process->command().identity,
            candidate.process->command().identity);
  EXPECT_FALSE(supervisor.discard_candidate(candidate));
  EXPECT_TRUE(alive(successor.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
  EXPECT_TRUE(supervisor.observe("ui6i_lite6")->running);
}

TEST(ControllerLaunchSupervisor,
     DiscardSupersededRestartCandidateCannotStopTheCurrentLaunch) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto log = fake.root() / "launch.artifacts";
  const auto first = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())), log);
  const auto restarted =
      supervisor.ensure(mock_controller_launch_command(
                            mock_artifacts(fake.root(), "sha256:changed")),
                        log);
  ASSERT_EQ(restarted.decision, ControllerLaunchDecision::kRestart);
  ASSERT_TRUE(fake.wait_for(fake.out(restarted.process->pid(), ".args")));
  const auto peer =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:peer", "ui6i_ur5e")),
                        fake.root() / "failed-replace-peer.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(peer.process->pid(), ".args")));
  EXPECT_FALSE(supervisor.discard_candidate(first));
  EXPECT_TRUE(alive(restarted.process->pid()));
  // Failed candidate installation discards the replacement after the original
  // occurrence was retired. Strong receipts remain pinned through cleanup.
  EXPECT_TRUE(supervisor.discard_candidate(restarted));
  EXPECT_TRUE(first.process->exited().has_value());
  EXPECT_TRUE(restarted.process->exited().has_value());
  EXPECT_FALSE(alive(first.process->pid()));
  EXPECT_FALSE(alive(restarted.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_TRUE(supervisor.component_output_counters("ui6i_lite6").has_value());
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_NE(first.process->command().identity,
            restarted.process->command().identity);
  EXPECT_FALSE(
      ControllerLaunchSupervisor::fault_source(first.process)().has_value());
  EXPECT_FALSE(ControllerLaunchSupervisor::fault_source(restarted.process)()
                   .has_value());
}

TEST(ControllerLaunchSupervisor, AnExitedLaunchIsAFaultAndIsNeverRestartedByItself) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  const auto started =
      supervisor.ensure(command, fake.root() / "launch.artifacts");
  const auto fault = ControllerLaunchSupervisor::fault_source(started.process);
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  EXPECT_FALSE(fault().has_value());

  // The launch dies while the cell is active (Q80).
  ASSERT_EQ(::kill(-started.process->pid(), SIGKILL), 0);
  std::optional<std::string> reason;
  for (int attempt = 0; attempt < 200 && !reason.has_value(); ++attempt) {
    reason = fault();
    if (!reason.has_value()) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("controller_launch_exited"), std::string::npos);
  EXPECT_NE(reason->find("signal 9"), std::string::npos);
  // Nothing restarted it.
  ASSERT_TRUE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_FALSE(supervisor.observe("ui6i_lite6")->running);
  EXPECT_TRUE(fault().has_value());

  // The next activation of the component starts a new launch.
  const auto again =
      supervisor.ensure(command, fake.root() / "launch.artifacts");
  EXPECT_EQ(again.decision, ControllerLaunchDecision::kRestart);
  EXPECT_NE(again.process->pid(), started.process->pid());
  EXPECT_TRUE(supervisor.observe("ui6i_lite6")->running);
}

TEST(ControllerLaunchSupervisor,
     ExitEvidenceAndCclcCorrelationAreCapturedOnceInMemory) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto log = fake.root() / "launch.artifacts";
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  const auto started =
      supervisor.ensure(command, log, "Bootstrap runtime_epoch=epoch.1");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  {
    std::ofstream output(log / "existing-output.log", std::ios::app);
    output << "preserve earlier launch output\n";
    for (int index = 0; index < 24; ++index)
      output << "diagnostic line " << index << "\n";
    output << std::string(5000U, 'x') << "\nlast crash detail\n";
  }
  const auto before_fault = read_file(log / "existing-output.log");
  int context_reads = 0;
  started.process->update_request_context(
      "apply_id=apply.7 apply_generation=7");
  ControllerLaunchReadinessEvidence readiness;
  readiness.known = true;
  readiness.controller_bound = true;
  readiness.arm_controller_active = true;
  readiness.controller_manager_present = true;
  readiness.controller_observed = true;
  readiness.controller_loaded = true;
  readiness.hardware_ready = true;
  readiness.joint_sample_available = true;
  readiness.binding_epoch = 17U;
  readiness.sample_age_ns = 1234;
  const auto fault =
      ControllerLaunchSupervisor::fault_source(started.process, [&] {
        ++context_reads;
        // Re-entry proves the callback is outside the process lock.
        EXPECT_TRUE(started.process->exit_evidence().has_value());
        return readiness;
      });
  ASSERT_EQ(::kill(started.process->pid(), SIGKILL), 0);
  std::optional<std::string> reason;
  for (int attempt = 0; attempt < 200 && !reason; ++attempt) {
    reason = fault();
    if (!reason)
      std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(reason.has_value());
  EXPECT_LE(reason->size(), 1024U);
  EXPECT_NE(reason->find("signal 9"), std::string::npos);
  EXPECT_NE(reason->find("component=\"ui6i_lite6\""), std::string::npos);
  EXPECT_NE(reason->find(log.string()), std::string::npos);
  EXPECT_NE(reason->find("apply_id=apply.7"), std::string::npos);
  EXPECT_EQ(reason->find("last crash detail"), std::string::npos);
  EXPECT_EQ(read_file(log / "existing-output.log"), before_fault);
  const auto evidence = started.process->exit_evidence();
  ASSERT_TRUE(evidence.has_value());
  EXPECT_EQ(evidence->pid, started.process->pid());
  EXPECT_FALSE(evidence->exit_status.has_value());
  EXPECT_EQ(evidence->signal, 9);
  EXPECT_FALSE(evidence->core_dumped);
  EXPECT_FALSE(evidence->observed_utc.empty());
  EXPECT_EQ(evidence->observed_utc.back(), 'Z');
  EXPECT_EQ(evidence->controllable_component_id, "ui6i_lite6");
  EXPECT_EQ(evidence->launch_identity, command.identity);
  EXPECT_EQ(evidence->robot_description_sha256,
            command.robot_description_sha256);
  EXPECT_EQ(evidence->controllers_sha256, command.controllers_sha256);
  EXPECT_EQ(evidence->upstream_log_directory, log / "ros");
  EXPECT_EQ(evidence->request_context, "apply_id=apply.7 apply_generation=7");
  ASSERT_TRUE(evidence->readiness.has_value());
  EXPECT_EQ(evidence->readiness->known, true);
  EXPECT_EQ(evidence->readiness->controller_bound, true);
  EXPECT_EQ(evidence->readiness->arm_controller_active, true);
  EXPECT_EQ(evidence->readiness->controller_manager_present, true);
  EXPECT_EQ(evidence->readiness->controller_observed, true);
  EXPECT_EQ(evidence->readiness->controller_loaded, true);
  EXPECT_EQ(evidence->readiness->hardware_ready, true);
  EXPECT_EQ(evidence->readiness->joint_sample_available, true);
  EXPECT_EQ(evidence->readiness->binding_epoch, 17U);
  EXPECT_EQ(evidence->readiness->sample_age_ns, 1234);
  EXPECT_FALSE(evidence->readiness->captured_utc.empty());
  EXPECT_EQ(evidence->readiness->captured_utc.back(), 'Z');
  started.process->update_request_context("apply_id=apply.later");
  readiness.arm_controller_active = false;
  EXPECT_EQ(fault(), reason);
  EXPECT_EQ(context_reads, 1);
  EXPECT_EQ(started.process->exit_evidence()->request_context,
            evidence->request_context);
  EXPECT_EQ(started.process->exit_evidence()->readiness->arm_controller_active,
            true);
  EXPECT_EQ(started.process->exit_evidence()->readiness->captured_utc,
            evidence->readiness->captured_utc);
  EXPECT_EQ(started.process->exit_evidence()->observed_utc,
            evidence->observed_utc);

  EXPECT_TRUE(started.process->append_incident_correlation(
      "cclc.incident.7", "apply_id=apply.7", *reason));
  const auto after_correlation = started.process->output_snapshot();
  EXPECT_NE(after_correlation.tail.find("cclc.incident.7"), std::string::npos);
  EXPECT_NE(after_correlation.tail.find("apply_id=apply.7"), std::string::npos);
  EXPECT_FALSE(started.process->append_incident_correlation(
      "cclc.incident.7", "apply_id=apply.7", *reason));
  EXPECT_EQ(started.process->output_snapshot().tail, after_correlation.tail);
  EXPECT_EQ(read_file(log / "existing-output.log"), before_fault);
  EXPECT_TRUE(supervisor.discard_candidate(started));
  const auto after_shutdown = started.process->output_snapshot();
  EXPECT_NE(after_shutdown.tail.find("manyforge_controller_launch_end"),
            std::string::npos);
  EXPECT_NE(after_shutdown.tail.find("Bootstrap runtime_epoch=epoch.1"),
            std::string::npos);
  EXPECT_EQ(read_file(log / "existing-output.log"), before_fault);
}

// Saturate a private stdout sink before construction, then restore the test
// runner's stdout. The supervisor's separately opened sink remains unread.
class StalledStdout final {
public:
  StalledStdout() {
    int pipe[2];
    if (::pipe2(pipe, O_CLOEXEC) != 0)
      throw std::runtime_error("test output pipe failed");
    reader_ = pipe[0];
    const int writer = pipe[1];
    saved_ = ::dup(STDOUT_FILENO);
    if (saved_ < 0 || ::fcntl(writer, F_SETFL, O_NONBLOCK) < 0)
      throw std::runtime_error("test output setup failed");
    const std::string bytes(4096U, 'x');
    while (::write(writer, bytes.data(), bytes.size()) > 0) {
    }
    if ((errno != EAGAIN && errno != EWOULDBLOCK) ||
        ::fcntl(writer, F_SETFL, 0) < 0 || ::dup2(writer, STDOUT_FILENO) < 0)
      throw std::runtime_error("test output saturation failed");
    ::close(writer);
  }
  void restore() {
    if (saved_ >= 0) {
      static_cast<void>(::dup2(saved_, STDOUT_FILENO));
      ::close(saved_);
      saved_ = -1;
    }
  }
  ssize_t drain(const std::size_t count) {
    std::string bytes(count, '\0');
    return ::read(reader_, bytes.data(), bytes.size());
  }
  ~StalledStdout() {
    restore();
    if (reader_ >= 0)
      ::close(reader_);
  }

private:
  int saved_{-1};
  int reader_{-1};
};

TEST(ControllerLaunchSupervisor,
     StalledForwardingDoesNotDelayPrimaryFaultOrStop) {
  FakeLaunch fake;
  StalledStdout stalled;
  ControllerLaunchSupervisor supervisor(100ms);
  EXPECT_EQ(::fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK, 0);
  stalled.restore();
  const auto log = fake.root() / "launch.artifacts";
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())), log);
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  EXPECT_FALSE(std::filesystem::exists(log / "controller_launch.log"));
  EXPECT_TRUE(started.process->append_incident_correlation(
      "cclc.existing", "apply_id=apply.7", "known launch exit"));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  EXPECT_GT(started.process->output_snapshot().dropped_forward_bytes, 0U);
  EXPECT_GT(started.process->output_snapshot().dropped_forward_lines, 0U);
  ASSERT_EQ(::kill(started.process->pid(), SIGKILL), 0);
  for (int attempt = 0; attempt < 200 && !started.process->exited(); ++attempt)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(started.process->exited().has_value());
  const auto source = ControllerLaunchSupervisor::fault_source(started.process);
  auto primary = std::async(std::launch::async, source);
  const auto primary_ready = primary.wait_for(200ms);
  auto stopped =
      std::async(std::launch::async, [&] { started.process->stop(0ms); });
  const auto stop_ready = stopped.wait_for(200ms);
  // Release the sink on timing failure before collecting futures, so a
  // diagnostic-under-lock regression reports failure instead of hanging.
  if (primary_ready != std::future_status::ready ||
      stop_ready != std::future_status::ready)
    static_cast<void>(stalled.drain(4096U));
  EXPECT_EQ(primary_ready, std::future_status::ready);
  EXPECT_EQ(stop_ready, std::future_status::ready);
  EXPECT_TRUE(primary.get().has_value());
  stopped.get();
  EXPECT_FALSE(std::filesystem::exists(log / "controller_launch.log"));
}

TEST(ControllerLaunchSupervisor, MemoryOutputCapsRecentLinesAndRawBytes) {
  FakeLaunch fake;
  StalledStdout stalled;
  ControllerLaunchSupervisor supervisor(100ms);
  stalled.restore();
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "unused-controller-output.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  for (int index = 0; index < 2100; ++index)
    EXPECT_TRUE(started.process->append_incident_correlation(
        "existing.cclc." + std::to_string(index), "apply_id=apply.9",
        "selected incident"));
  const auto lines = started.process->output_snapshot();
  EXPECT_LE(lines.line_count, 2000U);
  EXPECT_LE(lines.tail.size(), 128U * 1024U);
  EXPECT_TRUE(lines.truncated);
  EXPECT_NE(lines.tail.find("existing.cclc.2099"), std::string::npos);
  EXPECT_EQ(lines.tail.find("existing.cclc.0\""), std::string::npos);
  EXPECT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.large", "",
      std::string(200U * 1024U, 'x') + "final detail"));
  const auto bytes = started.process->output_snapshot();
  EXPECT_LE(bytes.tail.size(), 128U * 1024U);
  EXPECT_LE(bytes.line_count, 2000U);
  EXPECT_TRUE(bytes.truncated);
  EXPECT_NE(bytes.tail.find("final detail"), std::string::npos);
  EXPECT_GT(bytes.dropped_forward_bytes, 0U);
}

class RegularStdout final {
public:
  explicit RegularStdout(const std::filesystem::path &path) {
    saved_ = ::dup(STDOUT_FILENO);
    if (saved_ < 0)
      throw std::runtime_error("cannot save test stdout");
    const int file =
        ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (file < 0 || ::dup2(file, STDOUT_FILENO) < 0) {
      if (file >= 0)
        ::close(file);
      restore();
      throw std::runtime_error("cannot redirect test stdout to regular file");
    }
    ::close(file);
  }
  void restore() {
    if (saved_ >= 0) {
      static_cast<void>(::dup2(saved_, STDOUT_FILENO));
      ::close(saved_);
      saved_ = -1;
    }
  }
  ~RegularStdout() { restore(); }

private:
  int saved_{-1};
};

TEST(ControllerLaunchSupervisor, RegularForwardSinkDropsWithoutWritingFile) {
  FakeLaunch fake;
  const auto sink = fake.root() / "regular-host-output.log";
  {
    std::ofstream file(sink);
    file << "preserve existing operator output\n";
  }
  const auto before = read_file(sink);
  RegularStdout redirected(sink);
  ControllerLaunchSupervisor supervisor(100ms);
  EXPECT_EQ(::fcntl(STDOUT_FILENO, F_GETFL) & O_NONBLOCK, 0);
  redirected.restore();
  supervisor.enqueue_host_log("INFO closed incident pruning count=1");
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "regular-sink-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  EXPECT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.regular-sink", "apply_id=apply.11", "selected incident"));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto output = started.process->output_snapshot();
  EXPECT_GT(output.dropped_forward_bytes, 0U);
  EXPECT_GT(output.dropped_forward_lines, 0U);
  EXPECT_NE(output.tail.find("existing.cclc.regular-sink"), std::string::npos);
  EXPECT_EQ(read_file(sink), before);
  EXPECT_TRUE(supervisor.discard_candidate(started));
  EXPECT_EQ(read_file(sink), before);
}

class StalledSocketStdout final {
public:
  StalledSocketStdout() {
    int pair[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair) != 0)
      throw std::runtime_error("cannot prepare test socket sink");
    writer_ = pair[0];
    peer_ = pair[1];
    saved_ = ::dup(STDOUT_FILENO);
    if (saved_ < 0) {
      ::close(writer_);
      release();
      throw std::runtime_error("cannot save test stdout");
    }
    if (::dup2(writer_, STDOUT_FILENO) < 0) {
      restore();
      ::close(writer_);
      release();
      throw std::runtime_error("cannot redirect test socket sink");
    }
  }
  void saturate() {
    const std::string bytes(4096U, 'x');
    while (::send(writer_, bytes.data(), bytes.size(),
                  MSG_DONTWAIT | MSG_NOSIGNAL) > 0) {
    }
    if (errno != EAGAIN && errno != EWOULDBLOCK)
      throw std::runtime_error("cannot saturate test socket sink");
  }
  std::string receive() {
    std::string bytes(8192U, '\0');
    const auto count = ::recv(peer_, bytes.data(), bytes.size(), MSG_DONTWAIT);
    if (count <= 0)
      return {};
    bytes.resize(static_cast<std::size_t>(count));
    return bytes;
  }
  int flags() const { return ::fcntl(writer_, F_GETFL); }
  bool small_send_buffer() {
    const int bytes = 4096;
    return ::setsockopt(writer_, SOL_SOCKET, SO_SNDBUF, &bytes,
                        sizeof(bytes)) == 0;
  }
  void restore() {
    if (saved_ >= 0) {
      static_cast<void>(::dup2(saved_, STDOUT_FILENO));
      ::close(saved_);
      saved_ = -1;
    }
  }
  void release() {
    if (peer_ >= 0) {
      ::close(peer_);
      peer_ = -1;
    }
  }
  ~StalledSocketStdout() {
    restore();
    release();
    if (writer_ >= 0)
      ::close(writer_);
  }

private:
  int writer_{-1};
  int peer_{-1};
  int saved_{-1};
};

TEST(ControllerLaunchSupervisor,
     SocketForwardingDropsWithoutChangingSharedFlags) {
  FakeLaunch fake;
  StalledSocketStdout stalled;
  const auto flags = stalled.flags();
  ASSERT_EQ(flags & O_NONBLOCK, 0);
  ControllerLaunchSupervisor supervisor(100ms);
  EXPECT_EQ(stalled.flags(), flags);
  stalled.restore();
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "socket-sink-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  std::string forwarded;
  for (int attempt = 0; attempt < 200; ++attempt) {
    forwarded += stalled.receive();
    if (forwarded.find("manyforge_controller_launch_start") !=
        std::string::npos)
      break;
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_NE(
      forwarded.find("manyforge_controller_output component=\"ui6i_lite6\""),
      std::string::npos);
  EXPECT_NE(forwarded.find("manyforge_controller_launch_start"),
            std::string::npos);
  // Positive delivery above proves this is the approved socket route, rather
  // than merely an unavailable sink whose output is always counted as lost.
  stalled.saturate();
  EXPECT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.socket", "apply_id=apply.12", "selected incident"));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  EXPECT_GT(started.process->output_snapshot().dropped_forward_bytes, 0U);
  EXPECT_GT(started.process->output_snapshot().dropped_forward_lines, 0U);
  EXPECT_EQ(stalled.flags(), flags);
  // A closed peer takes the same suffix-loss path without SIGPIPE or flag
  // edits.
  const auto before_closed = started.process->output_snapshot();
  stalled.release();
  ASSERT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.socket.closed", "apply_id=apply.12", "closed peer"));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes ==
           before_closed.dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto closed = started.process->output_snapshot();
  EXPECT_GT(closed.dropped_forward_bytes, before_closed.dropped_forward_bytes);
  EXPECT_GT(closed.dropped_forward_lines, before_closed.dropped_forward_lines);
  EXPECT_EQ(stalled.flags(), flags);
  ASSERT_EQ(::kill(started.process->pid(), SIGKILL), 0);
  for (int attempt = 0; attempt < 200 && !started.process->exited(); ++attempt)
    std::this_thread::sleep_for(10ms);
  ASSERT_TRUE(started.process->exited().has_value());
  auto primary =
      std::async(std::launch::async,
                 ControllerLaunchSupervisor::fault_source(started.process));
  const auto primary_ready = primary.wait_for(200ms);
  auto stopped =
      std::async(std::launch::async, [&] { started.process->stop(0ms); });
  const auto stop_ready = stopped.wait_for(200ms);
  if (primary_ready != std::future_status::ready ||
      stop_ready != std::future_status::ready)
    stalled.release();
  EXPECT_EQ(primary_ready, std::future_status::ready);
  EXPECT_EQ(stop_ready, std::future_status::ready);
  EXPECT_TRUE(primary.get().has_value());
  stopped.get();
  EXPECT_EQ(stalled.flags(), flags);
}

TEST(ControllerLaunchSupervisor, PartialSocketSendCountsOnlyUnsentSuffix) {
  FakeLaunch fake;
  StalledSocketStdout sink;
  ASSERT_TRUE(sink.small_send_buffer());
  const auto flags = sink.flags();
  ControllerLaunchSupervisor supervisor(100ms);
  sink.restore();
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "partial-socket-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  std::string initial;
  for (int attempt = 0;
       attempt < 200 &&
       initial.find("manyforge_controller_launch_start") == std::string::npos;
       ++attempt) {
    initial += sink.receive();
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_NE(initial.find("manyforge_controller_launch_start"),
            std::string::npos);
  const auto before = started.process->output_snapshot();
  ASSERT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.socket.partial", "apply_id=apply.13",
      std::string(11000U, 'x')));
  const auto queued = started.process->output_snapshot();
  const auto tag = "manyforge_controller_output component=\"ui6i_lite6\" pid=" +
                   std::to_string(started.process->pid()) + " ";
  const auto attempted = tag.size() + queued.tail.size() - before.tail.size();
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes ==
           before.dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto after = started.process->output_snapshot();
  const auto lost = after.dropped_forward_bytes - before.dropped_forward_bytes;
  EXPECT_GT(lost, 0U);
  EXPECT_LT(lost, attempted);
  EXPECT_EQ(after.dropped_forward_lines - before.dropped_forward_lines, 1U);
  std::string accepted;
  for (int attempt = 0; attempt < 200 && accepted.size() + lost < attempted;
       ++attempt) {
    accepted += sink.receive();
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_EQ(accepted.size() + lost, attempted);
  EXPECT_EQ(sink.flags(), flags);
}

class TerminalStdout final {
public:
  TerminalStdout() {
    master_ = ::posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC | O_NONBLOCK);
    if (master_ < 0 || ::grantpt(master_) != 0 || ::unlockpt(master_) != 0)
      throw std::runtime_error("cannot prepare test terminal");
    const char *name = ::ptsname(master_);
    if (name == nullptr)
      throw std::runtime_error("cannot resolve test terminal");
    path_ = name;
    slave_ = ::open(path_.c_str(), O_WRONLY | O_NOCTTY | O_CLOEXEC);
    saved_ = ::dup(STDOUT_FILENO);
    if (slave_ < 0 || saved_ < 0 || ::dup2(slave_, STDOUT_FILENO) < 0)
      throw std::runtime_error("cannot redirect test terminal");
  }
  void restore() {
    if (saved_ >= 0) {
      static_cast<void>(::dup2(saved_, STDOUT_FILENO));
      ::close(saved_);
      saved_ = -1;
    }
  }
  int flags() const { return ::fcntl(slave_, F_GETFL); }
  std::string receive() {
    char bytes[8192];
    const auto count = ::read(master_, bytes, sizeof(bytes));
    return count > 0 ? std::string(bytes, static_cast<std::size_t>(count)) : "";
  }
  bool pause_output() {
    if (::tcflow(slave_, TCOOFF) != 0)
      return false;
    const int writer =
        ::open(path_.c_str(), O_WRONLY | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
    if (writer < 0)
      return false;
    const char byte = 'x';
    const auto written = ::write(writer, &byte, 1U);
    const auto failure = errno;
    ::close(writer);
    return written < 0 && (failure == EAGAIN || failure == EWOULDBLOCK);
  }
  bool resume_output() { return ::tcflow(slave_, TCOON) == 0; }
  ~TerminalStdout() {
    restore();
    if (slave_ >= 0) {
      static_cast<void>(resume_output());
      ::close(slave_);
    }
    if (master_ >= 0)
      ::close(master_);
  }

private:
  int master_{-1};
  int slave_{-1};
  int saved_{-1};
  std::string path_;
};

TEST(ControllerLaunchSupervisor,
     TerminalForwardingUsesPrivateNonblockingFlags) {
  FakeLaunch fake;
  TerminalStdout sink;
  const auto flags = sink.flags();
  ASSERT_EQ(flags & O_NONBLOCK, 0);
  ControllerLaunchSupervisor supervisor(100ms);
  EXPECT_EQ(sink.flags(), flags);
  sink.restore();
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "terminal-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  std::string forwarded;
  for (int attempt = 0;
       attempt < 200 &&
       forwarded.find("manyforge_controller_launch_start") == std::string::npos;
       ++attempt) {
    forwarded += sink.receive();
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_NE(forwarded.find("manyforge_controller_launch_start"),
            std::string::npos);
  ASSERT_TRUE(sink.pause_output());
  const auto before = started.process->output_snapshot();
  ASSERT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.terminal", "apply_id=apply.14", "flow-stopped terminal"));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes ==
           before.dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  EXPECT_GT(started.process->output_snapshot().dropped_forward_bytes,
            before.dropped_forward_bytes);
  EXPECT_EQ(sink.flags(), flags);
  auto stopped =
      std::async(std::launch::async, [&] { started.process->stop(0ms); });
  const auto ready = stopped.wait_for(200ms);
  if (ready != std::future_status::ready) {
    EXPECT_TRUE(sink.resume_output());
  }
  EXPECT_EQ(ready, std::future_status::ready);
  stopped.get();
  EXPECT_TRUE(sink.resume_output());
  EXPECT_EQ(sink.flags(), flags);
}

TEST(ControllerLaunchSupervisor,
     ReleasedOccurrenceForwardsCorrelatedEndRecord) {
  FakeLaunch fake;
  StalledSocketStdout sink;
  ControllerLaunchSupervisor supervisor(100ms);
  sink.restore();
  auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "released-output-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  ASSERT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.released", "apply_id=apply.15", "selected incident"));
  EXPECT_TRUE(supervisor.retire_exact(started.process));
  started.process.reset();
  // No process owner remains; only the collector's bounded output owner may
  // forward the end line after child EOF and process destruction.
  std::string forwarded;
  for (int attempt = 0; attempt < 200; ++attempt) {
    forwarded += sink.receive();
    const auto end = forwarded.find("manyforge_controller_launch_end");
    if (end != std::string::npos &&
        forwarded.find("incident_id=\"existing.cclc.released\"", end) !=
            std::string::npos)
      break;
    std::this_thread::sleep_for(10ms);
  }
  const auto end = forwarded.find("manyforge_controller_launch_end");
  ASSERT_NE(end, std::string::npos);
  EXPECT_NE(forwarded.find("incident_id=\"existing.cclc.released\"", end),
            std::string::npos);
  supervisor.enqueue_host_log("INFO closed incidents pruned count=1");
  for (int attempt = 0;
       attempt < 200 &&
       forwarded.find("manyforge_host_notice") == std::string::npos;
       ++attempt) {
    forwarded += sink.receive();
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_NE(forwarded.find("manyforge_host_notice pid="), std::string::npos);
  EXPECT_EQ(forwarded.find("component=\"host\""), std::string::npos);
}

TEST(ControllerLaunchSupervisor,
     ExactRetirementNeverStopsSupersedingOrPeerProcess) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto first = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "first-retired-artifacts");
  auto replacement_command = mock_controller_launch_command(
      mock_artifacts(fake.root(), "sha256:changed"));
  const auto replacement = supervisor.ensure(
      replacement_command, fake.root() / "replacement-artifacts");
  const auto peer =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:peer", "other_robot")),
                        fake.root() / "peer-artifacts");
  EXPECT_FALSE(supervisor.retire_exact(first.process));
  EXPECT_TRUE(alive(replacement.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
  EXPECT_TRUE(supervisor.retire_exact(replacement.process));
  EXPECT_FALSE(alive(replacement.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
  EXPECT_FALSE(supervisor.retire_exact(replacement.process));
}

TEST(ControllerLaunchSupervisor,
     CountersSurviveReplaceRemoveReaddAndLateOutput) {
  FakeLaunch fake;
  StalledStdout sink;
  ControllerLaunchSupervisor supervisor(100ms);
  sink.restore();
  EXPECT_FALSE(
      supervisor.component_output_counters("never_launched").has_value());
  auto first = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "counter-first-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(first.process->pid(), ".args")));
  for (int attempt = 0;
       attempt < 200 &&
       first.process->output_counters().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto first_loss = first.process->output_counters();
  ASSERT_GT(first_loss.dropped_forward_bytes, 0U);
  auto replacement = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root(), "sha256:new")),
      fake.root() / "counter-replacement-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(replacement.process->pid(), ".args")));
  for (int attempt = 0;
       attempt < 200 &&
       replacement.process->output_counters().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto before_late = supervisor.component_output_counters("ui6i_lite6");
  ASSERT_TRUE(before_late.has_value());
  EXPECT_GE(before_late->dropped_forward_bytes,
            first_loss.dropped_forward_bytes +
                replacement.process->output_counters().dropped_forward_bytes);
  // Old occurrence output can be recorded after its replacement is installed.
  ASSERT_TRUE(first.process->append_incident_correlation(
      "existing.cclc.old.late", "apply_id=apply.16", "late old occurrence"));
  for (int attempt = 0;
       attempt < 200 &&
       supervisor.component_output_counters("ui6i_lite6")
               ->dropped_forward_bytes <= before_late->dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto after_late = supervisor.component_output_counters("ui6i_lite6");
  ASSERT_TRUE(after_late.has_value());
  EXPECT_GT(after_late->dropped_forward_bytes,
            before_late->dropped_forward_bytes);
  EXPECT_TRUE(supervisor.retire_exact(replacement.process));
  first.process.reset();
  replacement.process.reset();
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
  const auto removed = supervisor.component_output_counters("ui6i_lite6");
  ASSERT_TRUE(removed.has_value());
  EXPECT_GE(removed->dropped_forward_bytes, after_late->dropped_forward_bytes);
  const auto readded = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "counter-readded-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(readded.process->pid(), ".args")));
  for (int attempt = 0;
       attempt < 200 &&
       readded.process->output_counters().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto after_readd = supervisor.component_output_counters("ui6i_lite6");
  ASSERT_TRUE(after_readd.has_value());
  EXPECT_GE(after_readd->dropped_forward_bytes,
            removed->dropped_forward_bytes +
                readded.process->output_counters().dropped_forward_bytes);
  const auto host_before = supervisor.host_output_counters();
  supervisor.enqueue_host_log("INFO closed incidents pruned count=1");
  for (int attempt = 0;
       attempt < 200 &&
       supervisor.host_output_counters().dropped_forward_bytes ==
           host_before.dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto host_after = supervisor.host_output_counters();
  EXPECT_GT(host_after.dropped_forward_bytes,
            host_before.dropped_forward_bytes);
  EXPECT_GT(host_after.dropped_forward_lines,
            host_before.dropped_forward_lines);
  supervisor.stop_all();
  EXPECT_GE(supervisor.host_output_counters().dropped_forward_bytes,
            host_after.dropped_forward_bytes);
  EXPECT_FALSE(
      supervisor.component_output_counters("never_launched").has_value());
}

[[nodiscard]] bool wait_for_unreaped_exit(const int pid) {
  for (int attempt = 0; attempt < 200; ++attempt) {
    siginfo_t status{};
    if (::waitid(P_PID, static_cast<id_t>(pid), &status,
                 WEXITED | WNOHANG | WNOWAIT) == 0 &&
        status.si_pid == pid)
      return true;
    std::this_thread::sleep_for(10ms);
  }
  return false;
}

TEST(ControllerLaunchSupervisor,
     ReplacementStopDoesNotBlockCountersAndConcurrentEnsureKeepsOneSuccessor) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto original = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "unlocked-stop-original.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(original.process->pid(), ".args")));
  const auto peer =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:peer", "ui6i_ur5e")),
                        fake.root() / "unlocked-stop-peer.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(peer.process->pid(), ".args")));
  auto blocker = std::make_unique<GroupChild>(original.process->pid());
  const auto command = mock_controller_launch_command(
      mock_artifacts(fake.root(), "sha256:replacement"));
  auto first = std::async(std::launch::async, [&] {
    return supervisor.ensure(command,
                             fake.root() / "unlocked-stop-first.artifacts");
  });
  EXPECT_TRUE(wait_for_unreaped_exit(original.process->pid()));
  // The leader has exited but this test-owned group child keeps stop active.
  auto second = std::async(std::launch::async, [&] {
    return supervisor.ensure(command,
                             fake.root() / "unlocked-stop-second.artifacts");
  });
  auto observation = std::async(
      std::launch::async, [&] { return supervisor.observe("ui6i_lite6"); });
  EXPECT_EQ(first.wait_for(10ms), std::future_status::timeout);
  EXPECT_EQ(second.wait_for(10ms), std::future_status::timeout);
  EXPECT_EQ(observation.wait_for(10ms), std::future_status::timeout);
  auto counters = std::async(std::launch::async, [&] {
    return supervisor.component_output_counters("ui6i_lite6");
  });
  const auto ready = counters.wait_for(200ms);
  // Always release the owned blocker before waiting on test futures.
  blocker.reset();
  EXPECT_EQ(ready, std::future_status::ready);
  EXPECT_TRUE(counters.get().has_value());
  const auto installed = first.get();
  const auto concurrent = second.get();
  EXPECT_EQ(installed.process, concurrent.process);
  EXPECT_TRUE((installed.decision == ControllerLaunchDecision::kRestart &&
               concurrent.decision == ControllerLaunchDecision::kKeep) ||
              (concurrent.decision == ControllerLaunchDecision::kRestart &&
               installed.decision == ControllerLaunchDecision::kKeep));
  static_cast<void>(observation.get());
  EXPECT_FALSE(alive(original.process->pid()));
  EXPECT_TRUE(alive(installed.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
  ASSERT_TRUE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_EQ(supervisor.observe("ui6i_lite6")->identity, command.identity);
}

TEST(ControllerLaunchSupervisor,
     StopAllSnapshotDoesNotBlockCountersOrClearLaterPublications) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(2s);
  const auto original = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "stop-all-original.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(original.process->pid(), ".args")));
  auto blocker = std::make_unique<GroupChild>(original.process->pid());
  auto stopped = std::async(std::launch::async, [&] { supervisor.stop_all(); });
  EXPECT_TRUE(wait_for_unreaped_exit(original.process->pid()));
  const auto command = mock_controller_launch_command(
      mock_artifacts(fake.root(), "sha256:later"));
  auto replacement = std::async(std::launch::async, [&] {
    return supervisor.ensure(command,
                             fake.root() / "stop-all-replacement.artifacts");
  });
  auto peer = std::async(std::launch::async, [&] {
    return supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                                 fake.root(), "sha256:peer", "ui6i_ur5e")),
                             fake.root() / "stop-all-later-peer.artifacts");
  });
  auto counters = std::async(std::launch::async, [&] {
    return supervisor.component_output_counters("ui6i_lite6");
  });
  const auto counter_ready = counters.wait_for(200ms);
  const auto peer_ready = peer.wait_for(200ms);
  EXPECT_EQ(stopped.wait_for(0ms), std::future_status::timeout);
  blocker.reset();
  EXPECT_EQ(counter_ready, std::future_status::ready);
  EXPECT_EQ(peer_ready, std::future_status::ready);
  EXPECT_TRUE(counters.get().has_value());
  stopped.get();
  const auto installed = replacement.get();
  const auto later_peer = peer.get();
  EXPECT_FALSE(alive(original.process->pid()));
  EXPECT_TRUE(alive(installed.process->pid()));
  EXPECT_TRUE(alive(later_peer.process->pid()));
  ASSERT_TRUE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_EQ(supervisor.observe("ui6i_lite6")->identity, command.identity);
  EXPECT_TRUE(supervisor.observe("ui6i_ur5e")->running);
}

TEST(ControllerLaunchSupervisor, PartialForwardWriteCountsOnlyUnsentBytes) {
  FakeLaunch fake;
  StalledStdout stalled;
  ControllerLaunchSupervisor supervisor(100ms);
  stalled.restore();
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "partial-write-artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes == 0U;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto before = started.process->output_snapshot();
  ASSERT_GT(before.dropped_forward_bytes, 0U);
  ASSERT_EQ(stalled.drain(4096U), 4096);
  ASSERT_TRUE(started.process->append_incident_correlation(
      "existing.cclc.partial", "apply_id=apply.10", std::string(6000U, 'x')));
  const auto queued = started.process->output_snapshot();
  const auto raw_record_bytes = queued.tail.size() - before.tail.size();
  const auto tag = "manyforge_controller_output component=\"ui6i_lite6\" pid=" +
                   std::to_string(started.process->pid()) + " ";
  for (int attempt = 0;
       attempt < 200 &&
       started.process->output_snapshot().dropped_forward_bytes ==
           before.dropped_forward_bytes;
       ++attempt)
    std::this_thread::sleep_for(10ms);
  const auto after = started.process->output_snapshot();
  const auto lost = after.dropped_forward_bytes - before.dropped_forward_bytes;
  EXPECT_GT(lost, 0U);
  EXPECT_LT(lost, tag.size() + raw_record_bytes);
  EXPECT_EQ(after.dropped_forward_lines - before.dropped_forward_lines, 1U);
}

TEST(ControllerLaunchSupervisor, BlockingChildOutputDrainsWhileHostSinkIsFull) {
  FakeLaunch fake;
  StalledStdout stalled;
  ControllerLaunchSupervisor supervisor(100ms);
  stalled.restore();
  ControllerLaunchCommand command;
  command.controllable_component_id = "output_probe";
  command.identity = "test output probe";
  command.argv = {
      "sh", "-c",
      "cat /proc/$$/fdinfo/1 > '" + (fake.root() / "child.fdinfo").string() +
          "'; awk 'BEGIN {for(i=0;i<8192;i++) printf \"%08d:%080d\\n\",i,i; "
          "for(i=0;i<3000;i++) print \"short line \"i; "
          "printf \"completed output marker\"}'; printf done > '" +
          (fake.root() / "output.done").string() +
          "'; trap 'exit 0' INT TERM; while :; do sleep 0.05; done"};
  const auto started =
      supervisor.ensure(command, fake.root() / "unused.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.root() / "output.done"));
  const auto info = read_file(fake.root() / "child.fdinfo");
  const auto flags_at = info.find("flags:");
  ASSERT_NE(flags_at, std::string::npos);
  std::istringstream flags_input(info.substr(flags_at + 6U));
  int flags = 0;
  flags_input >> std::oct >> flags;
  EXPECT_EQ(flags & O_NONBLOCK, 0);
  ControllerLaunchOutputSnapshot output;
  for (int attempt = 0; attempt < 200; ++attempt) {
    output = started.process->output_snapshot();
    if (output.tail.find("completed output marker") != std::string::npos)
      break;
    std::this_thread::sleep_for(10ms);
  }
  EXPECT_NE(output.tail.find("completed output marker"), std::string::npos);
  EXPECT_LE(output.tail.size(), 128U * 1024U);
  EXPECT_EQ(output.line_count, 2000U);
  ASSERT_FALSE(output.tail.empty());
  EXPECT_EQ(output.tail.back(), 'r');
  EXPECT_NE(output.tail.find("short line 2999\n"), std::string::npos);
  EXPECT_EQ(output.tail.find("short line 0\n"), std::string::npos);
  EXPECT_TRUE(output.truncated);
  EXPECT_GT(output.dropped_forward_bytes, 0U);
  EXPECT_GT(output.dropped_forward_lines, 0U);
  EXPECT_FALSE(std::filesystem::exists(fake.root() / "unused.artifacts" /
                                       "controller_launch.log"));
  // A competing spawn still receives a turn after the bounded drain batch.
  const auto peer = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "peer-unused.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(peer.process->pid(), ".args")));
  EXPECT_TRUE(supervisor.discard_candidate(started));
  EXPECT_TRUE(alive(peer.process->pid()));
}

TEST(ControllerLaunchSupervisor, BoundedFaultContextPreservesUtf8Characters) {
  FakeLaunch fake;
  ::setenv("FAKE_LAUNCH_EXIT", "7", 1);
  ControllerLaunchSupervisor supervisor(100ms);
  std::string context(287U, 'a');
  for (int count = 0; count < 300; ++count)
    context += "\xc3\xa9";
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "utf8-fault-artifacts", context);
  const auto source = ControllerLaunchSupervisor::fault_source(started.process);
  std::optional<std::string> reason;
  for (int attempt = 0; attempt < 200 && !reason; ++attempt) {
    reason = source();
    if (!reason)
      std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(reason.has_value());
  EXPECT_LE(reason->size(), 1024U);
  EXPECT_NE(reason->find("[truncated]"), std::string::npos);
  // The fixture's only non-ASCII character is a two-byte NFC e-acute.
  for (std::size_t byte = 0U; byte < reason->size(); ++byte) {
    const auto value = static_cast<unsigned char>((*reason)[byte]);
    if (value < 0x80U)
      continue;
    ASSERT_EQ(value, 0xc3U);
    ASSERT_LT(byte + 1U, reason->size());
    EXPECT_EQ(static_cast<unsigned char>((*reason)[++byte]), 0xa9U);
  }
  EXPECT_EQ(source(), reason);
}

TEST(ControllerLaunchSupervisor,
     UnavailableReadinessDoesNotReplacePrimaryFault) {
  FakeLaunch fake;
  ::setenv("FAKE_LAUNCH_EXIT", "3", 1);
  ControllerLaunchSupervisor supervisor(100ms);
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "launch.artifacts", "apply_id=apply.8");
  int reads = 0;
  const auto source = ControllerLaunchSupervisor::fault_source(
      started.process, [&]() -> ControllerLaunchReadinessEvidence {
        ++reads;
        throw std::runtime_error("test-only cached readiness unavailable");
      });
  std::optional<std::string> reason;
  for (int attempt = 0; attempt < 200 && !reason; ++attempt) {
    reason = source();
    if (!reason)
      std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("status 3"), std::string::npos);
  EXPECT_EQ(source(), reason);
  EXPECT_EQ(reads, 1);
  const auto evidence = started.process->exit_evidence();
  ASSERT_TRUE(evidence.has_value());
  EXPECT_EQ(evidence->request_context, "apply_id=apply.8");
  EXPECT_FALSE(evidence->readiness.has_value());
}

TEST(ControllerLaunchSupervisor,
     LeaderOnlyCrashCleanupKillsItsGroupAndPreservesOtherOccurrences) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto command =
      mock_controller_launch_command(mock_artifacts(fake.root()));
  const auto crashed =
      supervisor.ensure(command, fake.root() / "crashed.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(crashed.process->pid(), ".args")));
  GroupChild orphan(crashed.process->pid());
  const auto peer =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:aa", "ui6i_ur5e")),
                        fake.root() / "peer.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(peer.process->pid(), ".args")));

  ASSERT_EQ(::kill(crashed.process->pid(), SIGKILL), 0);
  for (int attempt = 0; attempt < 200 && !crashed.process->exited().has_value();
       ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(crashed.process->exited().has_value());
  // WNOWAIT observation keeps the zombie leader's identity until group cleanup.
  siginfo_t status{};
  ASSERT_EQ(::waitid(P_PID, static_cast<id_t>(crashed.process->pid()), &status,
                     WEXITED | WNOHANG | WNOWAIT),
            0);
  EXPECT_EQ(status.si_pid, crashed.process->pid());
  EXPECT_TRUE(orphan.running());
  EXPECT_TRUE(supervisor.discard_candidate(crashed));
  EXPECT_TRUE(orphan.reap_killed());
  EXPECT_TRUE(alive(peer.process->pid()));

  const auto successor =
      supervisor.ensure(command, fake.root() / "successor.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(successor.process->pid(), ".args")));
  EXPECT_FALSE(supervisor.discard_candidate(crashed));
  // Old handles may still call stop(), but ownership was released before reuse.
  crashed.process->stop(0ms);
  EXPECT_TRUE(alive(successor.process->pid()));
  EXPECT_TRUE(alive(peer.process->pid()));
}

TEST(ControllerLaunchSupervisor, LostLeaderOwnershipNeverSignalsItsOldGroup) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "launch.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(started.process->pid(), ".args")));
  GroupChild group_child(started.process->pid());
  ASSERT_EQ(::kill(started.process->pid(), SIGKILL), 0);
  for (int attempt = 0; attempt < 200 && !started.process->exited();
       ++attempt) {
    std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(started.process->exited().has_value());
  const auto evidence = started.process->exit_evidence();
  // Simulate an external reaper after a prior WNOWAIT exit observation.
  ASSERT_EQ(::waitpid(started.process->pid(), nullptr, 0),
            started.process->pid());
  EXPECT_TRUE(supervisor.discard_candidate(started));
  EXPECT_TRUE(group_child.running());
  EXPECT_EQ(started.process->exit_evidence()->observed_utc,
            evidence->observed_utc);
  started.process->stop(0ms);
  EXPECT_TRUE(group_child.running());
  // GroupChild's test-owned destructor performs explicit probe cleanup.
}

TEST(ControllerLaunchSupervisor,
     ALaunchThatFailsAtStartIsReportedWithItsStatus) {
  FakeLaunch fake;
  ::setenv("FAKE_LAUNCH_EXIT", "3", 1);
  ControllerLaunchSupervisor supervisor(2s);
  const auto log = fake.root() / "launch.artifacts";
  {
    std::filesystem::create_directories(log);
    std::ofstream output(log / "existing-output.log");
    for (int index = 0; index < 24; ++index)
      output << "prior line " << index << "\n";
  }
  const auto started = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())), log);
  const auto fault = ControllerLaunchSupervisor::fault_source(started.process);
  std::optional<std::string> reason;
  for (int attempt = 0; attempt < 200 && !reason.has_value(); ++attempt) {
    reason = fault();
    if (!reason.has_value()) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(reason.has_value());
  EXPECT_NE(reason->find("status 3"), std::string::npos);
  const auto evidence = started.process->exit_evidence();
  ASSERT_TRUE(evidence.has_value());
  EXPECT_EQ(evidence->exit_status, 3);
  EXPECT_FALSE(evidence->signal.has_value());
}

TEST(ControllerLaunchSupervisor, AMissingLauncherFailsTheActivation) {
  TemporaryRoot root;
  const char *path = std::getenv("PATH");
  const std::string previous = path == nullptr ? "" : path;
  ::setenv("PATH", (root.path() / "empty").string().c_str(), 1);
  ControllerLaunchSupervisor supervisor(2s);
  EXPECT_THROW(static_cast<void>(supervisor.ensure(
                   mock_controller_launch_command(mock_artifacts(root.path())),
                   root.path() / "launch.artifacts")),
               ControllerLaunchProcessError);
  ::setenv("PATH", previous.c_str(), 1);
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
}

TEST(ControllerLaunchSupervisor,
     TheLaunchInheritsNoHostDescriptorAndExactRetirementPreservesItsPeer) {
  FakeLaunch fake;
  // A non-CLOEXEC descriptor of the host (like its private channels).
  const int host_fd = ::open("/dev/null", O_RDONLY);
  ASSERT_GE(host_fd, 0);
  ASSERT_EQ(::dup2(host_fd, 77), 77);
  ControllerLaunchSupervisor supervisor(2s);
  const auto lite6 = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "lite6.artifacts");
  const auto ur5e =
      supervisor.ensure(mock_controller_launch_command(mock_artifacts(
                            fake.root(), "sha256:aa", "ui6i_ur5e")),
                        fake.root() / "ur5e.artifacts");
  ASSERT_TRUE(fake.wait_for(fake.out(lite6.process->pid(), ".fds")));
  std::istringstream fds(read_file(fake.out(lite6.process->pid(), ".fds")));
  for (std::string fd; std::getline(fds, fd);) {
    EXPECT_NE(fd, "77");
    EXPECT_NE(fd, std::to_string(host_fd));
  }
  ::close(77);
  ::close(host_fd);

  EXPECT_TRUE(supervisor.retire_exact(lite6.process));
  EXPECT_TRUE(lite6.process->exited().has_value());
  EXPECT_FALSE(supervisor.observe("ui6i_lite6").has_value());
  EXPECT_TRUE(alive(ur5e.process->pid()));
  EXPECT_TRUE(supervisor.observe("ui6i_ur5e")->running);
}


[[nodiscard]] CellLaunchCommand cell_command(
    const CellLaunchRole role = CellLaunchRole::kSceneBarrier) {
  return {manyforge::core::CellId("cell_a"), role,
          role == CellLaunchRole::kPoseObserver
              ? std::optional<std::string>("configured_pose_source")
              : std::nullopt,
          {"ros2", "run", "manyforge_ros_bringup", "cell_process"},
          "exact_cell_launch_identity"};
}

TEST(CellLaunchProcess, BarrierExitEvidenceDistinguishesSuccessAndFailure) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  for (const int status : {0, 3}) {
    ::setenv("FAKE_LAUNCH_EXIT", std::to_string(status).c_str(), 1);
    const auto root = fake.root() / ("barrier-" + std::to_string(status));
    const auto process = supervisor.start_cell_process(
        cell_command(), root, "Apply parent=actual_request");
    std::optional<CellLaunchExitEvidence> evidence;
    for (int attempt = 0; attempt < 200 && !evidence; ++attempt) {
      evidence = process->exit_evidence();
      if (!evidence) std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(evidence.has_value());
    EXPECT_EQ(evidence->exit_status, status);
    EXPECT_FALSE(evidence->signal.has_value());
    EXPECT_EQ(evidence->command.cell_id.value(), "cell_a");
    EXPECT_EQ(evidence->command.role, CellLaunchRole::kSceneBarrier);
    EXPECT_FALSE(evidence->command.observer_source_id.has_value());
    EXPECT_EQ(evidence->command.identity, "exact_cell_launch_identity");
    EXPECT_EQ(evidence->request_context, "Apply parent=actual_request");
    EXPECT_EQ(evidence->upstream_log_directory, root / "ros");
    EXPECT_NE(process->output_snapshot().tail.find("manyforge_cell_launch_start"),
              std::string::npos);
    EXPECT_EQ(process->output_snapshot().tail.find(" component="),
              std::string::npos);
    EXPECT_FALSE(supervisor.observe("cell_a").has_value());
    EXPECT_FALSE(supervisor.component_output_counters("cell_a").has_value());
    process->stop(0ms);
    EXPECT_FALSE(alive(process->pid()));
    EXPECT_EQ(process->exit_evidence()->observed_utc, evidence->observed_utc);
  }
}

TEST(CellLaunchProcess, CellObserverSurvivesComponentRemovalAndFreshAddition) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto observer = supervisor.start_cell_process(
      cell_command(CellLaunchRole::kPoseObserver), fake.root() / "observer");
  ASSERT_TRUE(fake.wait_for(fake.out(observer->pid(), ".args")));
  EXPECT_EQ(observer->command().observer_source_id, "configured_pose_source");
  EXPECT_EQ(read_file(fake.out(observer->pid(), ".roslog")),
            (fake.root() / "observer/ros").string());
  const auto first = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "first_component");
  ASSERT_TRUE(supervisor.retire_exact(first.process));
  EXPECT_TRUE(alive(observer->pid()));
  supervisor.stop_all();
  EXPECT_TRUE(alive(observer->pid()));
  const auto next = supervisor.ensure(
      mock_controller_launch_command(mock_artifacts(fake.root())),
      fake.root() / "next_component");
  ASSERT_TRUE(fake.wait_for(fake.out(next.process->pid(), ".args")));
  observer->update_request_context("Apply parent=next_request");
  observer->stop(0ms);
  EXPECT_FALSE(alive(observer->pid()));
  EXPECT_TRUE(alive(next.process->pid()));
  EXPECT_TRUE(supervisor.observe("ui6i_lite6")->running);
  const auto evidence = observer->exit_evidence();
  ASSERT_TRUE(evidence.has_value());
  EXPECT_EQ(evidence->request_context, "Apply parent=next_request");
  EXPECT_EQ(evidence->command.observer_source_id, "configured_pose_source");
}

TEST(CellLaunchProcess, InvalidTypedInputsAndLogRootRefuseBeforeSpawn) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto root = fake.root() / "cell";
  auto invalid = cell_command();
  invalid.observer_source_id = "forbidden_barrier_source";
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  invalid = cell_command(CellLaunchRole::kPoseObserver);
  invalid.observer_source_id.reset();
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  invalid.observer_source_id = "";
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  invalid = cell_command();
  invalid.role = static_cast<CellLaunchRole>(42);
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  invalid = cell_command();
  invalid.argv.clear();
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  invalid = cell_command();
  invalid.identity.clear();
  EXPECT_THROW((void)supervisor.start_cell_process(invalid, root),
               ControllerLaunchProcessError);
  EXPECT_THROW((void)supervisor.start_cell_process(cell_command(), {}),
               ControllerLaunchProcessError);
  EXPECT_THROW((void)supervisor.start_cell_process(cell_command(), "relative"),
               ControllerLaunchProcessError);
  EXPECT_FALSE(std::filesystem::exists(fake.root() / "out"));
  EXPECT_FALSE(std::filesystem::exists(root));
  EXPECT_FALSE(supervisor.observe("cell_a").has_value());
}

TEST(CellLaunchProcess, ZombieLeaderPinsGroupUntilExactCellStop) {
  FakeLaunch fake;
  ControllerLaunchSupervisor supervisor(100ms);
  const auto process = supervisor.start_cell_process(
      cell_command(CellLaunchRole::kPoseObserver), fake.root() / "observer");
  ASSERT_TRUE(fake.wait_for(fake.out(process->pid(), ".args")));
  GroupChild child(process->pid());
  ASSERT_EQ(::kill(process->pid(), SIGKILL), 0);
  std::optional<CellLaunchExitEvidence> evidence;
  for (int attempt = 0; attempt < 200 && !evidence; ++attempt) {
    evidence = process->exit_evidence();
    if (!evidence) std::this_thread::sleep_for(10ms);
  }
  ASSERT_TRUE(evidence.has_value());
  EXPECT_EQ(evidence->signal, SIGKILL);
  EXPECT_FALSE(evidence->exit_status.has_value());
  EXPECT_TRUE(child.running());
  siginfo_t status{};
  ASSERT_EQ(::waitid(P_PID, process->pid(), &status,
                    WEXITED | WNOHANG | WNOWAIT), 0);
  EXPECT_EQ(status.si_pid, process->pid());
  process->stop(0ms);
  EXPECT_TRUE(child.reap_killed());
  EXPECT_FALSE(alive(process->pid()));
  EXPECT_EQ(process->exit_evidence()->observed_utc, evidence->observed_utc);
  process->stop(0ms);
}

} // namespace

#pragma once

#include <filesystem>
#include <map>
#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_roboplan/controller_robot_description.hpp>
#include <manyforge_roboplan/vendor_device_adapter.hpp>

namespace manyforge::roboplan_backend {

/// Generated controller launch artifacts for the ROS lanes ManyForge starts
/// itself (design Q63-Q69, Q73, Q74; ROBOT_DESCRIPTION_GENERATION.md). ROS-free:
/// the inputs are the model URDF, the retained binding facts and the
/// deployment's values; the outputs are the controller `robot_description`
/// file, ONE controllers YAML file and the exact launch contract that consumes
/// them (Track 2's mock launch arguments and Isaac shell environment).
///
/// Templates are Track 2's `manyforge_ros_bringup/config` files, embedded at
/// build time: the UFactory or UR mock arm template by model family, the
/// generic mock gripper template only when a commanded gripper joint exists,
/// and the retained Isaac controller file of each Isaac profile.

/// Failure of a controller launch artifact input. Generation fails closed.
class ControllerLaunchArtifactError final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// The closed mock controller profiles (catalog `mock_profile` values).
[[nodiscard]] std::vector<std::string> mock_controller_profile_ids();

/// The closed Isaac controller profiles (catalog `isaac_profile` values).
[[nodiscard]] std::vector<std::string> isaac_controller_profile_ids();

/// The Isaac row's arrival tolerance (rad, every arm joint): the stock JTC
/// `goal` constraint of its controllers file (a placeholder until the Isaac
/// measurement sets it).
[[nodiscard]] double isaac_controller_arrival_tolerance_rad(
    std::string_view isaac_profile);

/// The Isaac row's settle window (s): the JTC `goal_time` of its controllers
/// file (a placeholder until the Isaac measurement sets it).
[[nodiscard]] double isaac_controller_goal_time_s(std::string_view isaac_profile);

/// Build-owned facts of one closed mock controller profile.
struct MockControllerProfileFacts final {
  std::string profile_id;
  /// The generated `<ros2_control>` element name.
  std::string hardware_component_name;
  /// The retained arm controller name of the model family's controller file
  /// (UFactory: xarm_ros2 c3172e57 vendor file; UR: ManyMove retained file).
  std::string arm_controller_name;
  /// The commanded gripper controller (Track 2 mapping, SX-280); absent for
  /// a profile without a model-backed gripper.
  std::optional<std::string> gripper_controller_name;
  /// The profile needs a commanded mock gripper joint (fail closed without a
  /// model-backed gripper binding).
  bool gripper_required{};
  /// Q79: Isaac's values per model family (Lite 6 family 1.5 s / 5 s, UR
  /// family 0.5 s / 15 s): the participant's hard-stop completion timeout and
  /// the gripper command's result timeout.
  double hard_stop_completion_timeout_s{};
  double gripper_result_timeout_s{};
  /// Arrival: every arm joint's stock JTC `goal` constraint (kept on every
  /// goal), so a successful result is "finished and within tolerance".
  double arrival_tolerance_rad{};
};

/// The controller cadence of one retained profile, read from its embedded
/// controller configuration (the controller manager's `update_rate` and the arm
/// controller's `state_publish_rate` and `action_monitor_rate`), so the
/// execution participant's lane facts reflect the configured values. Actual
/// publication cadence is controller-version dependent.
struct MockControllerLaneRates final {
  double controller_update_rate_hz{};
  double state_publish_rate_hz{};
  double action_monitor_rate_hz{};
};

[[nodiscard]] MockControllerLaneRates
mock_controller_lane_rates(std::string_view mock_profile);

[[nodiscard]] MockControllerLaneRates
isaac_controller_lane_rates(std::string_view isaac_profile);

[[nodiscard]] MockControllerProfileFacts
mock_controller_profile_facts(std::string_view mock_profile);

/// The closed real controller profiles (CP8 step 2a; temporary selector).
[[nodiscard]] std::vector<std::string> real_controller_profile_ids();

/// Build-owned facts of one closed real controller profile.
struct RealControllerProfileFacts final {
  std::string profile_id;
  /// The generated `<ros2_control>` element name (starts inactive).
  std::string hardware_component_name;
  std::string arm_controller_name;
  double hard_stop_completion_timeout_s{};
  /// R7 D3: the measured joint state's freshness budget, per robot.
  double joint_state_freshness_s{};
  /// Measured start/terminal state checks of the execution participant. The
  /// terminal check reads the arrival tolerance, which is also every arm
  /// joint's stock JTC `goal` constraint.
  double start_tolerance_rad{};
  double arrival_tolerance_rad{};
  double completion_state_timeout_s{};
};

[[nodiscard]] RealControllerProfileFacts
real_controller_profile_facts(std::string_view real_profile);

/// The cadence of a real profile's generated controllers (Mock's template).
[[nodiscard]] MockControllerLaneRates
real_controller_lane_rates(std::string_view real_profile);

/// Q66: the host's fixed controller manager of one controllable component.
[[nodiscard]] std::string
controller_runtime_namespace(std::string_view controllable_component_id);
inline constexpr std::string_view kControllerManagerNodeName =
    "controller_manager";

/// One mock lane component.
struct MockControllerArtifactsInput final {
  std::string mock_profile;
  /// The deployment's retained `initial_joint_positions_rad` slice of this
  /// component, paired with `arm_joint_names` by order.
  std::vector<double> initial_joint_positions_rad;
  /// Q48: the deployment's operator Stop profile per-joint maximum
  /// deceleration, by model source joint. Required for every arm joint.
  std::map<std::string, double, std::less<>> max_deceleration_on_cancel_rad_s2;
  /// The gripper binding as the mock lane sees it; absent without a gripper.
  std::optional<MockGripperBinding> gripper;
  core::GripperOutputMode gripper_output_mode{core::GripperOutputMode::kCommanded};
};

/// One Real component on Mock's ros2_control start-up path (CP8 step 2a, R7
/// package 1): the unmodified vendor plugin, its hardware initially inactive.
/// The robot reports measured state, so the deployment supplies no initial
/// pose; no gripper controller in this package.
struct RealControllerArtifactsInput final {
  std::string real_profile;
  /// V-B4: the deployment's vendor adapter family (`VendorAdapterSelection`).
  std::string adapter_family;
  /// The robot controller's address; required.
  std::string robot_ip;
  /// Q48: the deployment's operator Stop per-joint maximum deceleration.
  std::map<std::string, double, std::less<>> max_deceleration_on_cancel_rad_s2;
  /// Deployment input; the runtime handle owns the TCP offset sync policy.
  bool controller_tcp_offset_sync{true};
};

/// One Isaac lane component (amended Q41: backend-commanded initial pose).
struct IsaacControllerArtifactsInput final {
  std::string isaac_profile;
  /// The profile's controller description facts and selected deployment pose.
  IsaacControllerLane lane;
};

struct ControllerLaunchArtifactsRequest final {
  /// Host-owned per-activation runtime root (created if absent).
  std::filesystem::path artifacts_root;
  std::string controllable_component_id;
  /// Exact ManyForge model URDF text of the component's model asset.
  std::string_view model_urdf;
  /// Ordered arm joints from the retained binding.
  std::vector<std::string> arm_joint_names;
  std::variant<MockControllerArtifactsInput, IsaacControllerArtifactsInput,
               RealControllerArtifactsInput>
      lane;
  /// Q82: the retained deployment frame prefix, passed literally; empty
  /// passes no prefix.
  std::string frame_prefix{};
  /// Explicit no-output Isaac gripper's model facts, validation only.
  std::optional<MockGripperBinding> gripper_without_control_interfaces{};
  /// Exact deployment::derive_component_runtime_identity of this component;
  /// includes its physical provider and retained configuration families.
  std::string component_runtime_identity{};
};

/// kMock is the shared ros2_control start-up path of Mock and Real components
/// (CP8 step 2a; the lane enum goes in CP8 step 3). kSimulatedPlc: a
/// ManyForge-owned simulated PLC process (sim-plc design); kPoseSource: the
/// observer process of a `pose_source` component (CP9); kAiModel: the probe
/// process of an `ai_model` component. None of these is produced by
/// controller artifact generation.
enum class ControllerLaunchLane {
  kMock,
  kIsaac,
  kSimulatedPlc,
  kPoseSource,
  kAiModel
};

/// One configuration-scoped stock writer. Its topic is never reused for a
/// different provider, ordered joint set or Deployment initial target.
struct MockResetPositionTarget final {
  std::string component_runtime_identity{};
  std::string controller_name;
  std::string commands_topic;
  std::vector<std::string> joint_names;
  std::vector<double> positions;
  std::string target_digest;

  friend bool operator==(const MockResetPositionTarget &,
                         const MockResetPositionTarget &) = default;
};

struct ControllerLaunchArtifacts final {
  ControllerLaunchLane lane{};
  std::string controllable_component_id;
  std::string controller_profile;
  std::filesystem::path directory;
  std::filesystem::path robot_description_file;
  std::filesystem::path controllers_file;
  /// Lowercase `sha256:<hex>` of the exact written bytes.
  std::string robot_description_sha256;
  std::string controllers_sha256;
  /// Exact effective controller configuration retained from generation memory.
  std::string controllers_yaml;
  std::string runtime_namespace;
  /// Empty only for a genuine gripper-only mock component.
  std::string arm_controller_name;
  std::optional<std::string> gripper_controller_name;
  std::string frame_prefix;
  std::optional<MockResetPositionTarget> mock_reset_position;
  /// Hardware with a driver parameters file (Real): its path and the
  /// `sha256:<hex>` of its exact bytes; empty otherwise.
  std::filesystem::path driver_parameters_file;
  std::string driver_parameters_sha256;
  std::optional<VendorAdapterSelection> vendor_adapter;
  /// Mock lane: `mock_control.launch.py` arguments (`name:=value`).
  std::vector<std::string> launch_arguments;
  /// Isaac lane: `launch-isaac-topic-based-control.sh` environment.
  std::vector<std::pair<std::string, std::string>> launch_environment;
};

/// Generate the mock controllers YAML from the embedded templates.
[[nodiscard]] std::string generate_mock_controllers_yaml(
    const MockControllerProfileFacts &profile,
    std::string_view controllable_component_id,
    const std::vector<std::string> &arm_joint_names,
    const std::map<std::string, double, std::less<>>
        &max_deceleration_on_cancel_rad_s2,
    const std::optional<std::string> &gripper_joint_name,
    core::GripperOutputMode gripper_output_mode = core::GripperOutputMode::kCommanded);

/// The retained Isaac controller file of one Isaac profile, unchanged.
[[nodiscard]] std::string_view retained_isaac_controllers_yaml(
    std::string_view isaac_profile);

/// The Isaac controllers file the host writes: the retained file with every
/// arm joint's `goal` constraint set to the row's arrival tolerance and
/// `goal_time` set to the row's settle window.
[[nodiscard]] std::string isaac_controllers_yaml(std::string_view isaac_profile);

/// Generate both files, write them below `artifacts_root/<component>/` and
/// return their paths, hashes and the launch contract.
[[nodiscard]] ControllerLaunchArtifacts write_controller_launch_artifacts(
    const ControllerLaunchArtifactsRequest &request);

/// Host activation input of one ROS component (Q63-Q69): its retained
/// bindings, controller profile selection and deployment values.
struct ComponentControllerLaunchInput final {
  std::filesystem::path artifacts_root;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  /// The retained model projection of the gripper binding's source joint at
  /// activation (as `main`'s native lane seeds its gripper); required for a
  /// model-backed gripper on the mock lane.
  std::optional<double> gripper_projected_position;
  /// Exactly one: the component's catalog controller profile.
  std::optional<std::string> isaac_profile;
  std::optional<std::string> mock_profile;
  /// CP8 step 2a: a temporary third selector for a Real profile, with the
  /// robot's address. Package IDs replace all three in CP8 step 3.
  std::optional<std::string> real_profile;
  std::string robot_ip;
  /// V-B4: a Real profile's vendor adapter family (deployment).
  std::string adapter_family;
  /// M6b: a Real profile's controller TCP offset sync (deployment, default on).
  bool controller_tcp_offset_sync{true};
  /// Isaac lane facts of `isaac_profile` (retained_isaac_controller_lane).
  std::optional<IsaacControllerLane> isaac_lane;
  /// The component's model asset source URDF.
  std::string model_urdf;
  /// Mock lane: the deployment's initial joint positions of this component.
  std::vector<double> initial_joint_positions_rad;
  /// Mock lane (Q48): the operator Stop per-resource maximum deceleration.
  std::vector<std::pair<core::ResourceId, double>>
      controlled_stop_max_deceleration_rad_s2;
  /// Q82: this component's retained deployment frame prefix.
  std::string frame_prefix;
  /// Caller supplies the strict selected pneumatic output choice. None never
  /// follows from a missing binding and does not create physical gripper output.
  core::GripperOutputMode gripper_output_mode{core::GripperOutputMode::kCommanded};
  /// The Real lane verifies this against the retained gripper row before
  /// accepting a local, no-output pneumatic gripper.
  std::optional<core::GripperActuationKind> gripper_actuation_kind;
  std::string component_runtime_identity{};
};

/// Write one ROS component's controller launch artifacts at activation.
[[nodiscard]] ControllerLaunchArtifacts write_component_controller_launch_artifacts(
    const core::RuntimeParticipantBinding &motion_binding,
    const ComponentControllerLaunchInput &input);

/// The optional-motion join. Without motion, requires a genuine scalar,
/// model-backed mock gripper binding; its exact component owns the artifacts.
[[nodiscard]] ControllerLaunchArtifacts write_component_controller_launch_artifacts(
    const std::optional<core::RuntimeParticipantBinding> &motion_binding,
    const ComponentControllerLaunchInput &input);

/// An Isaac tool (gripper-only) component: only its model description. The
/// tool's joints are in the robot's Isaac articulation, so it has no
/// controller manager and no controllers file; its launch publishes the
/// description and runs the profile's gripper bridge, if any.
[[nodiscard]] ControllerLaunchArtifacts write_isaac_gripper_launch_artifacts(
    const std::filesystem::path &artifacts_root,
    std::string_view controllable_component_id, std::string_view isaac_profile,
    const std::string &model_urdf, const std::string &frame_prefix);

/// One-line JSON evidence record of written artifacts (paths, hashes, launch
/// contract) for activation diagnostics.
[[nodiscard]] std::string
controller_launch_artifacts_evidence(const ControllerLaunchArtifacts &value);

} // namespace manyforge::roboplan_backend

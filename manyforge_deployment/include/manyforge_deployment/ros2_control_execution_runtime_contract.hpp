#pragma once

#include <array>
#include <cstdint>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/execution_runtime_contract.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/motion_interrupt.hpp>
#include <manyforge_core/types/gripper.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/strong_id.hpp>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <utility>

namespace manyforge::deployment {

class RetainedBundle;

} // namespace manyforge::deployment

namespace manyforge::deployment::ros2_control_execution_runtime_contract {

/// Build-owned identities for the retained ROS/JTC execution runtime. It uses
/// the Manager-owned installed-target process protocol.
inline constexpr std::string_view kProductContractId =
    "manyforge.ros2_control_execution_runtime_unit";
inline constexpr std::string_view kRuntimeProfileId =
    "manyforge.ros2_control_execution_runtime";
inline constexpr std::string_view kRuntimeRoleId = "ros2_control_execution";
/// CellId scopes this shared host role independently of robot membership.
inline constexpr std::string_view kCellHostRuntimeUnitId =
    "robot_cell.ros2_control_execution";
inline constexpr std::string_view kStatusEndpointRoleId =
    "ros2_control_execution_status";
inline constexpr std::string_view kProjectionEndpointRoleId =
    execution_runtime_contract::kProjectionEndpointRoleId;
inline constexpr std::string_view kLogicalSceneEndpointRoleId =
    execution_runtime_contract::kLogicalSceneEndpointRoleId;
inline constexpr std::string_view kImplementationId =
    "manyforge.ros2_control_execution_host";
inline constexpr std::string_view kProcessAdapterContractId =
    execution_runtime_contract::kProcessAdapterContractId;
inline constexpr std::string_view kRetrievalContractId =
    execution_runtime_contract::kRetrievalContractId;
inline constexpr std::string_view kStatusEndpointContractId =
    "manyforge.ros2_control_execution.status_direct";
inline constexpr std::string_view kProjectionEndpointContractId =
    execution_runtime_contract::kProjectionEndpointContractId;
inline constexpr std::string_view kLogicalSceneEndpointContractId =
    execution_runtime_contract::kLogicalSceneEndpointContractId;
// These are immutable public API identities, not endpoint-contract revisions.
// Their semantic owner is the ros2_control execution host.
inline constexpr std::string_view kRuntimeRoleApiContractId =
    "manyforge.api.ros2_control.execution";
inline constexpr std::string_view kStatusEndpointApiContractId =
    "manyforge.api.ros2_control.status";
inline constexpr std::string_view kProjectionEndpointApiContractId =
    "manyforge.api.ros2_control.projection";
inline constexpr std::string_view kLogicalSceneEndpointApiContractId =
    "manyforge.api.ros2_control.logical_scene";
inline constexpr std::string_view kOperatingSystem =
    execution_runtime_contract::kOperatingSystem;
inline constexpr std::string_view kLaunchEnvironmentContract =
    "manyforge.ros2_control_execution_host.abi.v1";
inline constexpr std::string_view kInstalledTargetId =
    "manyforge_ros2_control_execution_host";
inline constexpr std::string_view kCandidateWorkerInstalledTargetId =
    execution_runtime_contract::kCandidateWorkerInstalledTargetId;

/// CP9: the streams a ROS component declares (`component_streams` rows):
/// every hosted component its measured joints, a gripper component also its
/// gripper feedback.
inline constexpr std::string_view kMeasuredJointsStreamId = "measured_joints";
inline constexpr std::string_view kGripperFeedbackStreamId = "gripper_feedback";
/// The one stream of an AI model component: its endpoint reachability.
inline constexpr std::string_view kAiModelStreamId = "endpoint";

struct Configuration final {
  /// Deployment-authored fault Stop profile of one motion target, read
  /// directly as `main` did (design Q37): no derivation from the operator
  /// profile. `main`'s authored profile has no jerk field and `main` shaped
  /// every Stop ramp with its default jerk, so the decoded profile enables
  /// jerk limits with no per-joint override (default derived at Stop time).
  struct FaultStopProfile final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    core::MotionInterruptStopProfileDTO profile;

    friend bool operator==(const FaultStopProfile &,
                           const FaultStopProfile &) = default;
  };

  /// Q68: the controller profile of each hosted controllable component
  /// inside the one ROS runtime unit (one controller manager per component,
  /// Q66). The execution host reads the model's `component.yaml`
  /// `lanes.<lane>` row (CONTRACTS §12) from `lane` and `model_family_id`;
  /// Real's adapter comes from that row (`lanes.real.adapter`).
  struct ControllerProfile final {
    core::ControllableComponentId controllable_component_id;
    /// The authored lane: `mock`, `isaac` or `real`.
    std::string lane;
    /// The model's `lanes.<lane>` row (CONTRACTS §12 ModelLaneRow: adapter,
    /// controller, arrival_tolerance_rad, goal_time_s?, adapter-specific
    /// keys, controllers_file?), verbatim as canonical JSON of one object.
    std::string lane_row;
    /// The model family whose lane row runs this component.
    std::string model_family_id;
    /// Q82 authored literal TF prefix; empty preserves unprefixed frames.
    std::string frame_prefix{};
    /// CP8 step 1: the site-settings name of a Real robot's address, present
    /// exactly on lane `real`. The address itself is a site fact, never
    /// compiled.
    std::optional<std::string> device_address_ref{};
    /// Where this component's soft Stop lead-in starts when its trip state is
    /// measured (Real): the controller's command position (absent or
    /// `commanded`) or the measured position (`measured`).
    std::optional<core::StopLeadInAnchor> stop_lead_in_anchor{};
    /// M6b: whether TCP updates are sent to the robot's controller (Real,
    /// default on). Off: its vendor adapter keeps the controller's TCP offset
    /// at zero. Lanes without a vendor adapter ignore it.
    bool controller_tcp_offset_sync{core::kDefaultControllerTcpOffsetSync};
    /// CONTRACTS §16: a machine's arm member names its owner and role; its
    /// `device_address_ref` is the owner's.
    std::optional<std::string> owner_component_id{};
    std::optional<std::string> member_role{};
    /// N17: the launch values the compiler writes, so the host launches the
    /// component from its row alone. An arm's model source joints in
    /// controller order (its family's `arm_joints`); a tool's one gripper
    /// joint. Exactly one of the two is present.
    std::vector<std::string> joint_names{};
    std::optional<std::string> gripper_joint_name{};
    /// The launched robot_description: the model's source URDF with the
    /// static frames the cell adds (offset frames, custom TCPs, the mounts
    /// on it and a process-less mounted child's fixed links).
    std::string description{};
    /// Q48: an arm's operator (controlled) Stop maximum deceleration per
    /// source joint, every arm joint, from the Deployment's
    /// `controlled_stop.per_joint_limits`: its JTC's
    /// `max_deceleration_on_cancel` on every lane. Empty on a tool row.
    std::map<std::string, double, std::less<>> max_deceleration_on_cancel_rad_s2{};

    friend bool operator==(const ControllerProfile &,
                           const ControllerProfile &) = default;
  };

  /// One row per hosted controllable component, sorted by component.
  std::vector<ControllerProfile> controller_profiles;
  /// CONTRACTS §16 / BOUNDARY §3: one row per machine owner, sorted by
  /// component. The owner has no targets; `machine_lane_row` is its machine
  /// model's `lanes.<lane>` row as canonical JSON (absent on a lane without
  /// one: the host's passive owner).
  struct Machine final {
    /// The machine's deactivated arm on a lane where the machine has a
    /// process: the owner runs that process itself (the arm's description,
    /// no arm controllers, hardware left inactive). `lane_row` is the arm
    /// model's `lanes.<lane>` row as canonical JSON, `model_family_id` its
    /// family (the row's relative `controllers_file` is in its folder).
    struct DeactivatedArm final {
      std::string lane_row;
      std::string model_family_id;
      std::string description;
      std::vector<std::string> joint_names;

      friend bool operator==(const DeactivatedArm &, const DeactivatedArm &) = default;
    };
    core::ControllableComponentId controllable_component_id;
    std::string lane;
    std::optional<std::string> machine_lane_row{};
    std::optional<std::string> device_address_ref{};
    std::optional<DeactivatedArm> deactivated_arm{};

    friend bool operator==(const Machine &, const Machine &) = default;
  };
  std::vector<Machine> machines;
  /// One row per sensor component (`kind: sensor` model, runtime kind
  /// `sensor`, e.g. a point-cloud source), sorted by component. No targets,
  /// no lane. `adapter_row` is the model's one adapter row as canonical JSON;
  /// `settings` is the canonical JSON object of the
  /// effective settings (for a point cloud `{topic, source_frame_id,
  /// max_points, minimum_period_s}`), which the host lays over the adapter
  /// row for the adapter.
  struct Sensor final {
    core::ControllableComponentId controllable_component_id;
    std::string adapter_row;
    std::string settings;

    friend bool operator==(const Sensor &, const Sensor &) = default;
  };
  std::vector<Sensor> sensors;
  std::uint32_t planner_rng_seed{};
  std::vector<double> initial_joint_positions_rad;
  std::vector<
      execution_runtime_contract::Configuration::MotionTargetLimit>
      motion_target_limits;
  /// At most one per hosted motion target; absent targets have no fault
  /// Stop profile (as `main` without `default_fault_stop_profile`).
  std::vector<FaultStopProfile> default_fault_stop_profiles;
  /// DX791: active ROS rows require an explicit output choice; no absent
  /// authored ROS field is interpreted as commanded output.
  struct PneumaticOutput final {
    core::IoSignalId signal;
    bool open_value{};
    bool close_value{};

    friend bool operator==(const PneumaticOutput &, const PneumaticOutput &) = default;
  };
  struct PneumaticIo final {
    std::vector<PneumaticOutput> outputs;
    std::map<std::string, core::IoSignalId, std::less<>> inputs;

    friend bool operator==(const PneumaticIo &, const PneumaticIo &) = default;
  };
  /// Opt-in typed grasp confirmation. Absence (or `not_required`) keeps the
  /// unconfirmed Close. A position gripper's evidence is its terminal position
  /// inside the stall window; a pneumatic gripper's is its mapped
  /// `workpiece_grasped` input at Close completion.
  struct GraspStallWindow final {
    double min_position{};
    double max_position{};

    friend bool operator==(const GraspStallWindow &, const GraspStallWindow &) = default;
  };
  struct GraspConfirmation final {
    bool required{};
    std::optional<GraspStallWindow> stall_window;

    friend bool operator==(const GraspConfirmation &, const GraspConfirmation &) = default;
  };
  struct GripperTargetConfiguration final
      : execution_runtime_contract::Configuration::GripperTargetConfiguration {
    core::GripperOutputMode output_mode;
    /// ROS-only pneumatic delivery bound. Absence preserves the source row and
    /// resolves to 1.0 s for pneumatic at backend configuration. Position rows
    /// reject this field. None retains positive metadata but has no delivery
    /// deadline or transport.
    std::optional<double> delivery_result_timeout_s;
    /// Signal identities and output levels; the Program selects which inputs
    /// are required for a particular open or close command.
    std::optional<PneumaticIo> pneumatic_io;
    std::optional<GraspConfirmation> grasp_confirmation;
    /// Position grippers: overrides the component's lane-row
    /// `gripper_stop_finish_tolerance` (finger position units, >= 0; 0 always
    /// cancels). Absence keeps the component default.
    std::optional<double> stop_finish_tolerance;

    GripperTargetConfiguration(
        execution_runtime_contract::Configuration::GripperTargetConfiguration value,
        core::GripperOutputMode mode,
        std::optional<double> delivery_timeout = std::nullopt,
        std::optional<PneumaticIo> io = std::nullopt,
        std::optional<GraspConfirmation> grasp = std::nullopt,
        std::optional<double> finish_tolerance = std::nullopt)
        : execution_runtime_contract::Configuration::GripperTargetConfiguration(
              std::move(value)), output_mode(mode),
          delivery_result_timeout_s(delivery_timeout), pneumatic_io(std::move(io)),
          grasp_confirmation(std::move(grasp)),
          stop_finish_tolerance(finish_tolerance) {}

    friend bool operator==(const GripperTargetConfiguration &,
                           const GripperTargetConfiguration &) = default;
  };
  std::vector<GripperTargetConfiguration> gripper_target_configurations;
  std::uint32_t execution_tick_hz{};
  double collision_monitor_period_ms{};
  std::optional<double> runtime_collision_immediate_query_budget_ms;
  std::optional<double> runtime_collision_lookahead_query_budget_ms;
  std::optional<double> runtime_collision_lookahead_cycle_budget_ms;
  std::optional<double> runtime_collision_lookahead_s;
  std::optional<std::uint32_t> runtime_collision_lookahead_max_samples;
  std::optional<std::uint32_t> runtime_collision_lookahead_workers;
  std::optional<double> runtime_dynamic_body_max_age_cap_ms;
  std::optional<std::uint32_t> validation_collision_workers;
  /// Collision clearance (m), the planner/validation collision-check
  /// step (rad) and whether fresh plans get the pre-dispatch waypoint check;
  /// absent keeps the kernel defaults.
  std::optional<double> collision_clearance_m;
  std::optional<double> collision_check_step_rad;
  std::optional<bool> collision_check_fresh_plans;

  /// CP9: one row per model-free pose-source component (runtime kind
  /// `pose_source`, profile `pose_source_v0`), sorted by component. The
  /// component id is the observer's `source_id`.
  struct PoseSource final {
    /// One observation binding (joint_state, simulation entity or
    /// Detection3D source), keyed by (object_id, policy_id).
    struct Binding final {
      std::string object_id;
      std::string policy_id;
      bool eligible{};
      std::int64_t lifetime_ns{};

      friend bool operator==(const Binding &, const Binding &) = default;
    };
    struct JointState final {
      std::string object_id;
      std::string policy_id;
      std::string topic;

      friend bool operator==(const JointState &, const JointState &) = default;
    };
    core::ControllableComponentId controllable_component_id;
    /// The normalized unit-configuration row minus `controllable_component_id`:
    /// `simulation_poll_period_ms`, optional `joint_state`,
    /// `simulation_entities`, `detection3d_sources`. The host writes it back
    /// as the observer's JSON document.
    CanonicalValue observer_configuration{nullptr};
    /// Observer order: joint_state, simulation_entities, detection3d_sources.
    std::vector<Binding> bindings;
    std::optional<JointState> joint_state;

    friend bool operator==(const PoseSource &, const PoseSource &) = default;
  };
  std::vector<PoseSource> pose_sources;

  /// One row per model-free AI model component (runtime kind `ai_model`,
  /// profile `ai_model_v0`), sorted by component: an external
  /// OpenAI-compatible endpoint. `api_key_env` names the environment variable
  /// holding the bearer token; the token itself is never compiled.
  struct AiModel final {
    core::ControllableComponentId controllable_component_id;
    std::string base_url;
    std::string model;
    std::optional<std::string> api_key_env;

    friend bool operator==(const AiModel &, const AiModel &) = default;
  };
  std::vector<AiModel> ai_models;

  /// CP9: each hosted component's declared streams and freshness budgets
  /// (`component_streams`), sorted by (component, stream). Deployment
  /// `policies.component_streams` rows override the budget at runtime. What a
  /// stale stream means belongs to the adapter, by component kind
  /// (CONTRACTS §7).
  struct ComponentStream final {
    core::ControllableComponentId controllable_component_id;
    std::string stream_id;
    std::int64_t budget_ns{};

    friend bool operator==(const ComponentStream &,
                           const ComponentStream &) = default;
  };
  std::vector<ComponentStream> component_streams;

  friend bool operator==(const Configuration &,
                         const Configuration &) = default;
};

/// Q89: the same ROS cell host with no hosted components. Values retain the
/// established empty-host planner seed and cadence; no component is fabricated.
[[nodiscard]] CanonicalValue empty_host_configuration();

[[nodiscard]] CanonicalValue empty_host_deployment(
    const core::CellId &cell_id, const CanonicalValue &runtime_profile_reference,
    std::string_view deployment_id, std::string_view name);

[[nodiscard]] Configuration
require_configuration(const RetainedBundle &retained_bundle,
                      const core::RuntimeUnitId &runtime_unit_id);

[[nodiscard]] Configuration require_component_configuration(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

[[nodiscard]] CanonicalValue component_configuration_projection(
    const RetainedBundle &bundle, const core::RuntimeUnitId &runtime_unit_id,
    const core::ControllableComponentId &component_id);

/// Each component's lifecycle time budget (ns) authored in the Deployment as
/// `lifecycle_command_timeout_s`: the `lifecycle_command_timeouts` rows
/// (robots, tools, pose sources, AI models) and the I/O devices'
/// `io_provider_devices` rows. CCLC bounds each of the component's lifecycle
/// steps with it; without one, its plans' own bound (the same default) applies.
[[nodiscard]] std::map<std::string, std::int64_t, std::less<>>
component_lifecycle_budgets_ns(const core::CellGraphDTO &graph);

/// The budget of every component of the graph, for a composition exchange:
/// the authored one (an I/O device's always is), else the default component
/// lifecycle budget `kDefaultLifecycleBudgetNs`.
[[nodiscard]] std::map<std::string, std::int64_t, std::less<>>
component_composition_budgets_ns(const core::CellGraphDTO &graph);

/// CP9: the stream id of one pose-source binding,
/// `poses/<object_id>/<policy_id>` (the compiler keeps it a valid stream id).
[[nodiscard]] std::string
pose_source_stream_id(const Configuration::PoseSource::Binding &binding);

/// The pose-source row of one hosted component, or none.
[[nodiscard]] const Configuration::PoseSource *
find_pose_source(const Configuration &configuration,
                 const core::ControllableComponentId &component_id);

/// The AI model row of one hosted component, or none.
[[nodiscard]] const Configuration::AiModel *
find_ai_model(const Configuration &configuration,
              const core::ControllableComponentId &component_id);

/// The sensor row of one hosted component, or none.
[[nodiscard]] const Configuration::Sensor *
find_sensor(const Configuration &configuration,
            const core::ControllableComponentId &component_id);

/// The default row of one declared component stream, or none when the
/// component does not list the stream.
[[nodiscard]] const Configuration::ComponentStream *
find_component_stream(const Configuration &configuration,
                      const core::ControllableComponentId &component,
                      std::string_view stream_id);

/// The controller profile row of one hosted component, or none.
[[nodiscard]] const Configuration::ControllerProfile *
find_controller_profile(const Configuration &configuration,
                        const core::ControllableComponentId &component_id);

/// CP8 step 1: the simulator that feeds one hosted component, resolved once
/// from its compiled controller profile (Isaac profiles depend on Isaac; Mock
/// and Real profiles on none). Consumers read this instead of the lane keys.
enum class ControllerSimulatorDependency : std::uint8_t { kNone, kIsaac };

[[nodiscard]] ControllerSimulatorDependency
controller_simulator_dependency(const Configuration::ControllerProfile &profile);

/// `none` or `isaac`.
[[nodiscard]] std::string_view
to_string(ControllerSimulatorDependency dependency) noexcept;

/// One `{controllable_component_id, simulator}` row per component hosted by a
/// ROS runtime unit of the verified bundle, sorted by component. Derived from
/// the compiled controller profiles; it is not part of any compiled
/// projection or identity.
[[nodiscard]] CanonicalValue
component_simulator_dependencies(const RetainedBundle &bundle);

[[nodiscard]] std::string implementation_revision();

} // namespace manyforge::deployment::ros2_control_execution_runtime_contract

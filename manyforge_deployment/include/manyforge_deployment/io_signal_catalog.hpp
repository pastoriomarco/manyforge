#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/deployment_applicability.hpp>
#include <manyforge_core/types/io_signal.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_core/types/program_reference.hpp>
#include <manyforge_core/types/robot_defaults.generated.hpp>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::deployment {

/// The default lifecycle step budget of every component
/// (core::kDefaultLifecycleCommandTimeoutS, manyforge_core/resources/robot_defaults.tsv).
/// A Deployment `lifecycle_command_timeout_s` replaces it per component.
inline constexpr core::Duration kDefaultLifecycleBudgetNs =
    static_cast<core::Duration>(core::kDefaultLifecycleCommandTimeoutS * 1'000'000'000.0);

/// The provider manifests of the model-free components: I/O devices, pose
/// sources and AI models.
inline constexpr std::string_view kIoProviderManifestId =
    "manyforge.provider.io_signals";
inline constexpr std::string_view kPoseSourceProviderManifestId =
    "manyforge.provider.pose_source";
inline constexpr std::string_view kAiModelProviderManifestId =
    "manyforge.provider.ai_model";

struct IoSignalProviderConfiguration final {
  std::string profile;
  std::optional<std::string> topic;
  std::optional<std::size_t> input_bit;
  std::optional<bool> initial_value;
  std::size_t ingress_depth{};
  /// Fully qualified channel on the existing host's shared ROS context.
  /// Never a portable command operand or a caller-selected reservation key.
  std::optional<std::string> service{};
  /// One actual synthetic output channel, shared by every local alias.
  std::optional<std::string> channel{};
  std::optional<std::string> group{};
  std::optional<std::string> interface_name{};
  std::optional<core::SceneResourceId> target_scene_resource_id{};
  /// Robot observed by geometric contact.
  std::optional<core::ControllableComponentId> robot_component_id{};
  /// Optional synthetic feedback from an output of this I/O component.
  std::optional<std::string> echo_output_signal_name{};
  std::optional<core::Duration> echo_delay_ns{};
  /// An adapter profile's provider object without `profile` and
  /// `ingress_depth`: opaque here, validated by the adapter that serves the
  /// profile (its address, pins, banks, ranges). Empty for generic profiles.
  core::JsonObjectDTO adapter_fields{};
  friend bool operator==(const IoSignalProviderConfiguration &,
                         const IoSignalProviderConfiguration &) = default;
};

/// The generic provider profiles validated here (`mock`, `mock_output`,
/// `ros_bool`, `ros_set_bool`, `ros2_control_gpio`, `geometric_contact`, the
/// compiled `system_carried_object`). Every other `profile` token names an
/// adapter profile: its provider object is kept opaque (`adapter_fields`) and
/// the adapter serving it validates it.
[[nodiscard]] bool IsGenericIoProviderProfile(std::string_view profile) noexcept;

/// An output whose provider acknowledges each write (`ros_set_bool` and every
/// adapter profile but a machine's I/O bank, `component_adapter`, whose
/// vendor family reports the write result itself); its device carries
/// `output_source_ack_timeout_s`.
[[nodiscard]] bool IsAcknowledgedIoOutputProfile(std::string_view profile) noexcept;

/// Resolved from the owning retained runtime unit's typed configuration, never
/// from a Program request. Addresses stay in this sealed configuration value.
struct RetainedIoSignalConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::CommandTargetId command_target_id;
  core::ResourceRoleId resource_role_id;
  core::IoSignalDescriptorDTO descriptor;
  IoSignalProviderConfiguration provider;
};

/// One exact direct Program-signal match. The retained configuration pointer
/// remains valid only while the input catalog stays alive and unchanged.
/// `descriptor` is the actual derived input for changed_outside; its target
/// and provider provenance are those of the retained base output.
struct ProgramSignalMatch final {
  const RetainedIoSignalConfiguration *retained;
  core::IoSignalDescriptorDTO descriptor;
  bool changed_outside;
};

/// Find every exact match, including eligible core-held changed_outside
/// inputs. Multiple results are ambiguity, never a first-match selection.
[[nodiscard]] std::vector<ProgramSignalMatch> FindProgramSignalMatches(
    std::span<const RetainedIoSignalConfiguration> catalog,
    const core::ProgramSignalReferenceDTO &reference);

struct RetainedIoProviderDeviceConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  core::Duration lifecycle_command_timeout_ns;
  /// Required after normalization for acknowledged ROS service outputs.
  /// Mock outputs have immediate synthetic acceptance and no deadline budget.
  std::optional<core::Duration> output_source_ack_timeout_ns{};
  /// Set on a machine's I/O member (its owner's component id
  /// and its member role); absent otherwise.
  std::optional<std::string> owner_component_id{};
  std::optional<std::string> member_role{};
};

/// Applies the default component lifecycle budget (10 s,
/// `kDefaultLifecycleBudgetNs`) per device before schema normalization
/// and graph identity. Ordinary configurations gain no synthetic device rows;
/// provider-owned outputs get the same acknowledged-service policy.
[[nodiscard]] core::Result<void> MaterializeRetainedIoProviderDevices(
    core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// Reads explicit normalized per-device policy. Runtime admission consumes
/// the retained duration and never supplies its own fallback.
[[nodiscard]] core::Result<std::vector<RetainedIoProviderDeviceConfiguration>>
ResolveRetainedIoProviderDevices(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// A unit filter is only for the per-unit schema-normalization hook; graph
/// qualification, binding and activation always validate the complete catalog.
[[nodiscard]] core::Result<std::vector<RetainedIoSignalConfiguration>>
ResolveRetainedIoSignalCatalog(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// System signals (grasp lost as a signal): one compiled input
/// per gripper target, never authored. A gripper target is a command target
/// with a `tool.gripper.command` control binding; the signal lives on that
/// target's component (the robot carrying the object).
inline constexpr std::string_view kSystemCarriedObjectProfile =
    "system_carried_object";
inline constexpr std::string_view kCarriedObjectLostSignalPrefix =
    "carried_object_lost.";
/// The compiled resource role of the signal on its gripper target (wait and
/// check input nodes); component types do not declare it.
inline constexpr std::string_view kCarriedObjectLostResourceRole =
    "carried_object_lost";
/// One in-process source for every system signal of the execution host.
inline constexpr std::string_view kCarriedObjectLostSourceId =
    "system.carried_object_lost";
inline constexpr std::size_t kCarriedObjectLostIngressDepth = 16U;
inline constexpr std::string_view kGripperCommandCapabilityId =
    "tool.gripper.command";

[[nodiscard]] std::string
CarriedObjectLostSignalName(std::string_view command_target_id);

/// The provider profile of a machine's I/O bank on the real
/// lane (`{bank}`; its vendor family serves it through the owner's
/// connection). Inputs sampled periodically (sample-age freshness), outputs
/// with read-back, one channel per signal.
inline constexpr std::string_view kComponentAdapterIoProfile =
    "component_adapter";

/// The authored catalog plus the compiled system signals (input, bool,
/// change_only, sampled_only, freshness `provider_validity`, in-process
/// liveness, profile `system_carried_object`). Every reader of signal values
/// (node and global rule conditions, Program signal roles, the host signal
/// installation) uses this; provider domains, devices, lifecycle and output
/// channels keep the authored catalog. An authored row that takes a system
/// identity is refused (`io_system_signal_conflict`).
[[nodiscard]] core::Result<std::vector<RetainedIoSignalConfiguration>>
ResolveCompiledIoSignalCatalog(const core::CellGraphDTO &graph);

/// The output row whose core-held
/// `<output name>.changed_outside` signal `signal_name` names, or nullptr when
/// the name lacks the suffix or the component has no such output. The caller
/// still checks `core::HoldsChangedOutside` (read-back, not controller-owned).
[[nodiscard]] const RetainedIoSignalConfiguration *FindChangedOutsideOutput(
    std::span<const RetainedIoSignalConfiguration> signals,
    const core::ControllableComponentId &component,
    std::string_view signal_name);

/// One model-free pose-source row (`pose_sources`) of a runtime unit.
struct RetainedPoseSourceConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  /// Sorted, unique stream ids (`PoseSourceStreamId`) of every binding.
  std::vector<std::string> stream_ids;
  /// The normalized unit-configuration row.
  core::JsonValueDTO row;
};

/// One stream (and provider-domain member) per pose-source binding:
/// `poses/<object_id>` for an object link (`policy_id == "scene"`), else
/// `poses/<object_id>/<policy_id>` (robot viewer overlays).
[[nodiscard]] std::string PoseSourceStreamId(std::string_view object_id,
                                             std::string_view policy_id);

/// Reads and checks the normalized `pose_sources` rows: hosted owner, at least
/// one binding, ids of `[a-z0-9_.-]+` with a stream id of at most 64 bytes,
/// unique (object_id, policy_id), unique required joint names. Typed reasons:
/// `pose_source_owner_mismatch`, `pose_source_bindings_empty`,
/// `pose_source_binding_id_invalid`, `pose_source_binding_duplicate`,
/// `pose_source_joint_names_duplicate`, `pose_source_configuration_invalid`.
[[nodiscard]] core::Result<std::vector<RetainedPoseSourceConfiguration>>
ResolveRetainedPoseSources(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// The capability of the single `endpoint` target of an AI model component.
inline constexpr std::string_view kAiModelCapabilityId = "ai.model.invoke";
inline constexpr std::string_view kAiModelTargetId = "endpoint";

/// One model-free AI model row (`ai_models`) of a runtime unit: an external
/// OpenAI-compatible endpoint. `api_key_env` names the token's environment
/// variable (absent: no token).
struct RetainedAiModelConfiguration final {
  core::RuntimeUnitId runtime_unit_id;
  core::ControllableComponentId controllable_component_id;
  std::string base_url;
  std::string model;
  std::optional<std::string> api_key_env;
  /// The normalized unit-configuration row.
  core::JsonValueDTO row;
};

/// Reads the normalized `ai_models` rows (the closed schema proved every
/// field, one row per component): hosted owner. Typed reasons:
/// `ai_model_owner_mismatch`, `ai_model_configuration_invalid`.
[[nodiscard]] core::Result<std::vector<RetainedAiModelConfiguration>>
ResolveRetainedAiModels(
    const core::CellGraphDTO &graph,
    const std::optional<core::RuntimeUnitId> &runtime_unit_id = std::nullopt);

/// Rechecks direct provider scope against the retained component and actual
/// configured signal owners. Component-type role classification is proved by
/// the ordinary applicability qualifier before sealing. `scene.pose.observe`
/// on the `poses` target of a configured pose source, and `ai.model.invoke` on
/// the `endpoint` target of a configured AI model, have no endpoint.
[[nodiscard]] core::Result<void> ValidateNonCoordinateProviderTarget(
    const core::CellGraphDTO &graph,
    const core::CapabilityProviderBindingDTO &provider);

} // namespace manyforge::deployment

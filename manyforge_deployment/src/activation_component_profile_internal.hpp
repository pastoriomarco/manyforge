#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/canonical_value.hpp>

#include "activation_deployment_intent_internal.hpp"

namespace manyforge::deployment::detail {

enum class ActivationRuntimeKind {
  kRos2Control = 1,
  kIoProvider = 2,
  kPoseSource = 3,
  kAiModel = 4,
  /// A machine's owner component (no targets, no model).
  kMachine = 5,
  /// A sensor component (`kind: sensor` model, e.g. a point-cloud source):
  /// no targets, no model asset; its runtime row is `sensors`.
  kSensor = 6
};

/// I/O devices, pose sources, AI models, machine owners and sensors select
/// no model and no physical type.
[[nodiscard]] constexpr bool
is_model_free_runtime_kind(ActivationRuntimeKind kind) noexcept {
  return kind == ActivationRuntimeKind::kIoProvider ||
         kind == ActivationRuntimeKind::kPoseSource ||
         kind == ActivationRuntimeKind::kAiModel ||
         kind == ActivationRuntimeKind::kMachine ||
         kind == ActivationRuntimeKind::kSensor;
}

/// Exact repository-owned semantics selected by one authored component.
///
/// The authored intent owns instance configuration.  This value supplies only
/// the stable profile/family facts needed to compile that configuration; it is
/// not a saved Deployment, catalog selector, or compatibility fallback.
struct ActivationComponentProfile final {
  std::string component_id;
  std::string component_profile_id;
  // Model identity fields are empty and model_family is null for model-free
  // kinds (kIoProvider, kPoseSource, kAiModel, kMachine, kSensor); they never select a model or physical
  // component type.
  std::string model_family_id;
  std::string model_asset_id;
  std::string accepted_package_uri_prefix;
  ActivationRuntimeKind runtime_kind;
  CanonicalValue model_family;
  /// The component model's descriptor (`component.yaml`: kind, roles,
  /// gripper, lanes, defaults); null for model-free kinds.
  CanonicalValue model_descriptor;
  /// The authored lane (a key of the model's `lanes`); none for model-free
  /// kinds and for a lane-free (pneumatic) tool.
  std::optional<std::string> lane;
  /// The model's `lanes.<lane>` row, verbatim.
  /// A machine owner's is its machine model's row (absent on a lane it has
  /// none for: the host's passive owner).
  std::optional<CanonicalValue> lane_row;
  /// A member's owner and role (absent otherwise), and the
  /// owner's site address reference, which the arm member's controller
  /// profile takes on the real lane.
  std::optional<std::string> owner_component_id{};
  std::optional<std::string> member_role{};
  std::optional<CanonicalValue> owner_device_address_ref{};
  /// A machine owner whose arm member is deactivated on a
  /// lane where the machine has a process (its model has a lane row): the
  /// arm model's `lanes.<lane>` row and family, the arm's description (URDF)
  /// and its joints (`arm_joints`), which the owner's process runs with the
  /// hardware left inactive.
  struct DeactivatedArm final {
    CanonicalValue lane_row;
    std::string model_family_id;
    std::string description;
    std::vector<std::string> joint_names;
  };
  std::optional<DeactivatedArm> deactivated_arm{};
};

/// Resolve every authored component: `component_profile_id` names a
/// component model (the descriptor `id` of a `model_families` row's
/// `component_descriptor`, or of a `component_models` descriptor: a machine,
/// a model-form I/O device or a sensor) or one of the model-free profiles
/// (`io_device_v0`, `pose_source_v0`, `ai_model_v0`).
///
/// A sensor (`kind: sensor`, `runtime_kind: sensor`) is owner-less and names
/// its lane; it has no lane row (its model's one `adapter_row` serves every
/// lane).
///
/// Machines: an owner selects a `kind: machine` model and
/// names the lane its members run on; each member's role is a slot of that
/// model, at most once per owner; every slot has a member, active or
/// deactivated (members are never removed; any may be deactivated). A
/// machine's member model is never placed without an owner. Typed failures: `deployment_member_role_unknown`,
/// `deployment_member_role_repeated`, `deployment_member_missing`,
/// `deployment_machine_part_without_machine`. The authored lane must be a key of the
/// model's `lanes`; only a pneumatic tool may name none (lane-free).
///
/// Results preserve the canonical component order in `intent`.  Unknown
/// models, duplicate repository identities and missing families fail before
/// model preparation or runtime-graph construction; a missing, unknown or
/// unexpected lane fails typed (`component_lane_required`,
/// `component_lane_unknown`, `component_lane_unexpected`).
[[nodiscard]] std::vector<ActivationComponentProfile>
resolve_activation_component_profiles(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent);

/// Rewrite each I/O member (a model-form `io_device` member)
/// into an ordinary I/O device row: `runtime_kind: io_provider` with one
/// `io_provider_devices` row (carrying `owner_component_id` and
/// `member_role`) and one `io_signals` row per model signal on target `io`.
/// Real lane: provider `component_adapter` with the model's `{bank}`; Mock
/// and Isaac: inputs `mock`, outputs `mock_output`.
void expand_activation_machine_members(
    ActivationDeploymentIntent &intent,
    const std::vector<ActivationComponentProfile> &profiles);

/// The repository-owned assistant profile row the intent selects by
/// `assistant_profile_id` (`maintained_assistant_profiles.v1.json`), with the
/// shared `mode_profiles` its `mode_profile_id` names; none without an
/// assistant profile. Unknown profiles fail.
struct ActivationAssistantProfile final {
  CanonicalValue profile;
  /// The selected mode profile (mode id -> mode), or an empty object.
  CanonicalValue modes;
};
[[nodiscard]] std::optional<ActivationAssistantProfile>
resolve_activation_assistant_profile(
    const std::filesystem::path &capability_root,
    const ActivationDeploymentIntent &intent);

} // namespace manyforge::deployment::detail

#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_core/types/tcp_entry.hpp>
#include <manyforge_deployment/canonical_value.hpp>

#include "activation_model_source_closure_internal.hpp"

namespace manyforge::deployment::detail {

/// One user-authored custom TCP of a component row (`custom_tcps`):
/// `parent_frame` names a frame of the component's model, either a frame role
/// of its model asset (a descriptor TCP, link-backed or offset) or a URDF
/// link; `pose` is T_parent_tcp in the authoring shape
/// `{position_m, orientation: {rpy_deg} | {xyzw}}`. The compiler resolves the
/// parent and converts the pose.
struct ActivationCustomTcp final {
  std::string tcp_id;
  std::string parent_frame;
  CanonicalValue pose;
};

struct ActivationDeploymentComponent final {
  std::string component_id;
  std::string component_profile_id;
  /// Absent for model-free components and for a child mounted through an
  /// `attachments` row (its placement follows the parent interface).
  std::optional<CanonicalValue> root_pose_in_cell;
  CanonicalValue target_settings;
  CanonicalValue configuration;
  std::optional<CapturedComponentModelSource> model_source;
  std::string frame_prefix{};
  /// Uniquely sorted by `tcp_id`; empty when the row omits `custom_tcps`.
  std::vector<ActivationCustomTcp> custom_tcps{};
  /// The authored `default_tcp` `{command_target_id, frame_role_id}`;
  /// the compiler checks it against the component's TCPs.
  std::optional<core::ComponentDefaultTcpDTO> default_tcp{};
  /// The authored lane (`mock`, `isaac`, `real`): which of the model's lane
  /// rows runs this component. Absent for model-free components and for a
  /// lane-free tool; the profile resolution checks it against the row.
  /// A machine member has none: it runs on its owner's lane.
  std::optional<std::string> lane{};
  /// A member names its owner (the machine's component id)
  /// and its member slot (`arm`, `controller_io`, ...). Absent for owners
  /// and owner-less components.
  std::optional<std::string> owner{};
  std::optional<std::string> role{};
};

/// A member row with `active: false` (or under an inactive owner): left out
/// of `components`, kept for the profile resolution's member checks (role
/// known, unique) and, for a physical member (an arm), its captured
/// description (URDF), which its owner's process still runs.
struct ActivationInactiveMember final {
  std::string component_id;
  std::string component_profile_id;
  std::string owner;
  std::string role;
  std::optional<std::string> description{};
};

struct ActivationDeploymentIntent final {
  std::string deployment_artifact_id;
  std::string name;
  std::optional<std::string> description;
  std::optional<std::string> assistant_profile_id;
  /// Validated `{parent:{component_id, interface}, child:{component_id,
  /// interface}, pose?, primary?}` rows; the compiler maps them to graph
  /// attachments (`primary` marks the primary tool of an interface).
  CanonicalValue attachments;
  CanonicalValue policies;
  std::optional<CanonicalValue> compatibility;
  std::vector<ActivationDeploymentComponent> components;
  CanonicalValue normalized_root;
  /// Optional `assistant_model_components`: assistant-profile model endpoint
  /// alias -> the ai_model component serving it (only with an assistant
  /// profile). The compiler resolves and checks every entry.
  std::map<std::string, std::string, std::less<>> assistant_model_components{};
  /// The deactivated members, never compiled.
  std::vector<ActivationInactiveMember> inactive_members{};
};

/// Parse one normalized authored Deployment intent and join it exactly to the
/// source bytes captured for the same components.
///
/// The caller has no path-based fallback: every physical component must have
/// exactly one captured source row and every captured row must be consumed.
/// Model-free provider components carry neither a model source nor a pose.
///
/// Machines: an owner row `{component_id,
/// component_profile_id, lane, configuration: {device_address_ref?},
/// active?}` has no `owner`, no `model_source` and no
/// `configuration.runtime_kind`; a member row names `owner` and `role` and
/// may set `active: false` (an arm member is a physical row without `lane`;
/// an I/O member is `{component_id, component_profile_id, owner, role,
/// active?}`). Inactive rows are dropped here (their captured sources are
/// consumed; an inactive arm keeps its URDF as `description`). Typed failures: `deployment_member_owner_missing`,
/// `deployment_member_owner_inactive`, `deployment_signal_member_inactive`.
[[nodiscard]] ActivationDeploymentIntent parse_activation_deployment_intent(
    const CanonicalValue &value,
    std::vector<CapturedComponentModelSource> captured_sources);

} // namespace manyforge::deployment::detail

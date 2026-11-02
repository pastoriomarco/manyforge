#pragma once

#include <span>
#include <string>
#include <vector>

#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/component_spec.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::deployment::detail {

/// What a component's launched description is compiled from: its facts
/// (`component_description_facts`), the composed cell's symbols and fixed
/// connections, and the source URDF of every model in the cell.
struct ComponentDescriptionInput final {
  const core::ControllableComponentId &component;
  const ComponentDescriptionFacts &facts;
  std::span<const OccurrenceSymbolRewrite> symbols;
  std::span<const CompositionFixedConnection> fixed_connections;
  std::span<const VerifiedRetainedModelSourceUrdf> sources;
};

/// Static frames ManyForge composes but a component's model URDF lacks, as
/// fixed joints whose child is a new massless link. They go into the
/// component's robot_state_publisher description, so its own launch
/// publishes them on /tf_static from activation on (Prepare and Execute play
/// no part). Names are source link names; the launch adds the component's
/// frame prefix, as for every other link of the description.
///
/// - The component's offset frames: its asset's offset frame roles and its
///   Deployment custom TCPs.
/// - For each component mounted on it: the mount, the child's root link on
///   the parent interface link at the attachment pose.
/// - For a mounted component that runs no process of its own (a no-output
///   pneumatic tool): the links fixed to its root and its offset frames on
///   those links. They are published under the parent's frame prefix.
[[nodiscard]] std::vector<UrdfJointIr>
component_static_frames(const ComponentDescriptionInput &input);

/// The links fixed (transitively, by fixed joints only) below `root_link` in
/// `urdf`, as renamed copies of those joints (`~fixed/<child link>`), so the
/// copies cannot collide with the joint names of the description they join.
[[nodiscard]] std::vector<UrdfJointIr>
fixed_links_below(const UrdfDocumentIr &urdf, const std::string &root_link);

/// `urdf_bytes` (the component's own model URDF) with the component's static
/// frames appended: the robot_state_publisher description of its launch.
[[nodiscard]] std::string component_description(const std::string &urdf_bytes,
                                                const ComponentDescriptionInput &input);

} // namespace manyforge::deployment::detail

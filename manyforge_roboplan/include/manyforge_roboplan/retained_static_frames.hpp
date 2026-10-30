#pragma once

#include <span>
#include <string>
#include <vector>

#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_deployment/model_source.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

namespace manyforge::roboplan_backend {

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
[[nodiscard]] std::vector<deployment::UrdfJointIr>
retained_component_static_frames(
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component);

/// The links fixed (transitively, by fixed joints only) below `root_link` in
/// `urdf`, as renamed copies of those joints (`~fixed/<child link>`), so the
/// copies cannot collide with the joint names of the description they join.
[[nodiscard]] std::vector<deployment::UrdfJointIr>
fixed_links_below(const deployment::UrdfDocumentIr &urdf,
                  const std::string &root_link);

/// True when a ros2_control component starts no process of its own: it hosts
/// no motion target, and every gripper row it has is no-output (pneumatic
/// `pneumatic_io` or output mode none), as the gripper-only composition reads
/// them.
[[nodiscard]] bool retained_component_runs_no_process(
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component);

/// `urdf_bytes` with the component's static frames appended (the
/// robot_state_publisher description of its launch).
[[nodiscard]] std::string retained_component_description(
    const std::string &urdf_bytes,
    const core::CellGraphDTO &graph,
    const deployment::VerifiedRetainedModelMap &model_map,
    std::span<const deployment::VerifiedRetainedModelSourceUrdf> sources,
    const deployment::ros2_control_execution_runtime_contract::Configuration
        &configuration,
    const core::ControllableComponentId &component);

} // namespace manyforge::roboplan_backend

#pragma once

#include "activation_component_profile_internal.hpp"
#include <manyforge_deployment/component_type_codec.hpp>

namespace manyforge::deployment::detail {

/// Actual closed-schema-qualified catalog artifact derived from one captured
/// model-free configuration. No model or live lifecycle authority is implied.
struct ActivationPreparedIoType final {
  std::string component_id;
  QualifiedComponentType type;
  std::string catalog_bytes;
  CanonicalValue normalized_configuration;
};

[[nodiscard]] std::vector<ActivationPreparedIoType> prepare_activation_io_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles,
    std::span<const QualifiedCapabilityDefinition> capabilities);

/// CONTRACTS §16: one derived component type per machine owner (tag
/// `machine`, no command targets) with its `machines` runtime row
/// `{controllable_component_id, lane, machine_lane_row?, device_address_ref?,
/// arm_lane_row?, arm_model_family_id?, arm_description?, arm_joint_names?}`
/// (`machine_lane_row`: the machine model's `lanes.<lane>` row as canonical
/// JSON, absent on a lane it has no row for; the `arm_*` fields: its
/// deactivated arm's lane row (canonical JSON), model family, description
/// (URDF) and joints, present exactly when the owner runs the machine's process itself).
[[nodiscard]] std::vector<ActivationPreparedIoType>
prepare_activation_machine_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles);

/// One derived component type per sensor (tag `sensor`, no command targets)
/// with its `sensors` runtime row `{controllable_component_id, adapter_row,
/// settings}` (a sensor has no lane): `adapter_row` the model's one
/// `adapter_row` as canonical JSON, `settings` the effective settings
/// as canonical JSON. A point-cloud source's settings are `{topic,
/// source_frame_id, max_points, minimum_period_s}`, all required.
/// Typed failure: `sensor_configuration_invalid`.
[[nodiscard]] std::vector<ActivationPreparedIoType>
prepare_activation_sensor_types(
    const ActivationDeploymentIntent &intent,
    std::span<const ActivationComponentProfile> profiles);

} // namespace manyforge::deployment::detail

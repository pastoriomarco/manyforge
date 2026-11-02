#pragma once

#include <optional>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/active_control_configuration.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

namespace manyforge::roboplan_backend::detail {

[[nodiscard]] core::Result<core::RuntimeParticipantBinding>
rebase_runtime_binding_to_predecessor(
    const core::RuntimeParticipantBinding &binding,
    const core::ActiveControlConfigurationDTO &configuration);

/// Select the product-owned state channel configuration.
[[nodiscard]] RetainedObservedStateChannelConfig
select_retained_state_channel_config(
    std::optional<RetainedObservedStateChannelConfig> selected,
    const core::RuntimeParticipantBinding &binding);

} // namespace manyforge::roboplan_backend::detail

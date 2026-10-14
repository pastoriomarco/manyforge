#pragma once

#include <optional>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_roboplan/retained_observed_state_channel.hpp>

namespace manyforge::roboplan_backend::detail {

/// Select the product-owned state channel configuration.
[[nodiscard]] RetainedObservedStateChannelConfig
select_retained_state_channel_config(
    std::optional<RetainedObservedStateChannelConfig> selected,
    const core::RuntimeParticipantBinding &binding);

} // namespace manyforge::roboplan_backend::detail

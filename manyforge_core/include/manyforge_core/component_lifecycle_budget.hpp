#pragma once

#include <cstdint>

namespace manyforge::core {

/// The default lifecycle time budget of every component (robots, tools, I/O
/// devices, pose sources, AI models; Mock, Isaac and Real alike): the maximum
/// time one lifecycle step (Prepare, Enable, Disable, Recover) may take,
/// e.g. how long a robot may take to come up after activation, and its share
/// of a composition exchange that installs it. A Deployment
/// `lifecycle_command_timeout_s` replaces it per component. Every lifecycle
/// plan uses this value. Individual controller and vendor service calls keep
/// their own short response limits: one call not answered in time is an
/// unresponsive component, whatever the step budget.
inline constexpr std::int64_t kDefaultLifecycleBudgetNs = 10'000'000'000LL;

}  // namespace manyforge::core

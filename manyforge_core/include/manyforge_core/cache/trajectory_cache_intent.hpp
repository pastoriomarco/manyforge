#pragma once

#include <string>

#include <manyforge_core/types/execute_request.hpp>

namespace manyforge::core {

/// Canonical typed identity of every request field that can change the planned
/// trajectory. The cache compares these bytes directly; it never treats a
/// finite-width diagnostic hash as proof of equality. Start state and the
/// collision-world revision remain independent admission dimensions.
std::string trajectory_cache_intent_identity(
    const ExecuteRequestDTO& request);

}  // namespace manyforge::core

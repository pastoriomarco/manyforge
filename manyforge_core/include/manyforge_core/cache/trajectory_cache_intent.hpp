#pragma once

#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/types/execute_request.hpp>
#include <string>

namespace manyforge::core {

/// Exact stable cache slot selected solely from the owner-produced execution
/// binding and source-authored move identity.
[[nodiscard]] TrajectoryCacheKey trajectory_cache_key(const ExecuteRequestDTO& request);

/// Canonical typed identity of every request field that can change the planned
/// trajectory. The cache compares these bytes directly; it never treats a
/// finite-width diagnostic hash as proof of equality. Start state and the
/// collision-world revision remain independent admission dimensions.
std::string trajectory_cache_intent_identity(const ExecuteRequestDTO& request);

}  // namespace manyforge::core

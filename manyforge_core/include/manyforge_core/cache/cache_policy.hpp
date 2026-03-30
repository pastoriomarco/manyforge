#pragma once

#include <cstddef>

namespace manyforge::core {

/// Configuration for the trajectory cache (spec 230 §3.1).
struct CacheConfig {
  size_t max_entries = 1000;  // safety bound
};

}  // namespace manyforge::core

#pragma once

#include <cstdint>

namespace manyforge::core {

/// Closed private tag shared by every CCLC-owned request family.
enum class CCLCRequestKindDTO : std::uint8_t {
  kComponentLifecycle = 0,
  kRouteSelection,
  kLifecycleTargetRetirement,
};

}  // namespace manyforge::core

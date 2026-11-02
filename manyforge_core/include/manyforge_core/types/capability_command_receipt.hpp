#pragma once

#include <cstdint>
#include <manyforge_core/types/error_code.hpp>
#include <optional>

namespace manyforge::core {

/// Receipt of the subordinate command-session submission. An accepted receipt
/// does not imply provider acknowledgement or physical output/readback.
struct CapabilityCommandDispatchReceiptDTO {
  bool accepted = false;
  std::uint64_t command_id = 0;
  std::uint64_t generation = 0;
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core

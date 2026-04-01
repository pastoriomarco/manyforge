#pragma once

#include <optional>

#include <manyforge_core/types/error_code.hpp>

namespace manyforge::core {

/// Execution handle for tracking trajectory execution (spec 220 §4.2)
/// Provides phase-event feedback; exact delivery mechanism is implementation-defined.
struct ExecutionHandle {};

/// Stop result (spec 220 §4.2)
struct StopResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;  // present when accepted is false
};

/// Pause result (spec 220 §4.2)
struct PauseResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Resume result (spec 230 §1.1)
struct ResumeResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

/// Reset result (spec 220 §4.2)
struct ResetResultDTO {
  bool accepted = false;
  std::optional<ErrorDTO> error;
};

}  // namespace manyforge::core

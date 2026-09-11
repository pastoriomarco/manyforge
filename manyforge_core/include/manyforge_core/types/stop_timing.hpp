#pragma once

#include <cstdint>
#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Saved operator timing, distinct from the internal post-fault completion
/// envelope. The commanded profile duration remains on the selected profile.
struct StopTimingParametersDTO final {
  std::int64_t command_latency_allowance_ns{};
  std::int64_t settling_allowance_ns{};
  std::int64_t operational_timeout_ns{};

  friend bool operator==(const StopTimingParametersDTO&, const StopTimingParametersDTO&) = default;
};

/// One exact binding's saved timing selection; omission keeps its qualified
/// policy defaults. Route changes of that binding retain the same selection.
struct StopTimingSelectionDTO final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  StopTimingParametersDTO timing;

  friend bool operator==(const StopTimingSelectionDTO&, const StopTimingSelectionDTO&) = default;
};

}  // namespace manyforge::core

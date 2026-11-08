#pragma once

#include <optional>
#include <stop_token>
#include <string_view>
#include <variant>

#include <manyforge_core/types/component_state.hpp>

namespace manyforge::roboplan_backend {

/// `SetOutputResult`: `unknown` when the write's effect is
/// unknown (never resent); `error` present unless it succeeded.
struct SetOutputResult final {
  core::ActionOutcome outcome{core::ActionOutcome::kFailed};
  std::optional<core::ActionError> error;

  friend bool operator==(const SetOutputResult &, const SetOutputResult &) = default;
};

/// The small I/O provider interface a component adapter may
/// also implement (capability `io_manual_write`). The one write path for every
/// output write, manual or Program (`set_io_output`): the host admits the
/// write in the Kernel (`admit_output_write`) and then calls it. ROS-free.
/// The adapter refuses a controller-owned output before sending
/// (`io_output_controller_owned`) and an unknown output (`io_signal_unknown`);
/// it trusts the device and never retries.
class IIoOutputWriter {
public:
  virtual ~IIoOutputWriter() = default;

  /// One write of the output `signal_name` of this component; blocks until
  /// the device or driver answers, or `stop` is requested.
  [[nodiscard]] virtual SetOutputResult SetOutput(std::string_view signal_name,
                                                  std::variant<bool, double> value,
                                                  std::stop_token stop) = 0;
};

} // namespace manyforge::roboplan_backend

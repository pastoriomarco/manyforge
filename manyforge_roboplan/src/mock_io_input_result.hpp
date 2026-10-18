#pragma once

#include <manyforge_core/types/io_signal.hpp>
#include <optional>

namespace manyforge::roboplan_backend::host_detail {

/// Actual synthetic model mutation and independently observed Core delivery.
/// No queue acceptance is promoted to a published acquisition cursor.
struct MockIoInputSetResult final {
  bool model_value;
  std::optional<core::IoSourceCursorDTO> published_cursor;
  core::IoSignalObservationDTO observation;
  std::optional<core::ErrorDTO> publication_error;
};

} // namespace manyforge::roboplan_backend::host_detail

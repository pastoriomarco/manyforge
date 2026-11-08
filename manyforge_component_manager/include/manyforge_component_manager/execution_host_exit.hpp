#pragma once

#include <optional>
#include <string_view>

namespace manyforge::component_manager {

/// Managed host could not safely complete owner or scheduler teardown.
inline constexpr int kExecutionHostShutdownIncompleteExitStatus = 86;

/// Catalog-facing reason for a managed child exit. Preserve the generic
/// reason for ordinary exits, signals, and unknown status.
[[nodiscard]] inline constexpr std::string_view
execution_host_child_exit_reason(std::optional<int> exit_status) noexcept {
  return exit_status == kExecutionHostShutdownIncompleteExitStatus
             ? "execution_host_shutdown_incomplete"
             : "execution_host_child_exited";
}

}  // namespace manyforge::component_manager

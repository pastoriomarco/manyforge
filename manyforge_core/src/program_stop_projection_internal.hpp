#pragma once

#include <cstdint>
#include <manyforge_core/types/program_execution_control.hpp>

namespace manyforge::core::resource_addressed_kernel_detail {

// Stopping settlement is the existing monotone projection at one revision.
// Faulting any other live hold state changes the published selection.
[[nodiscard]] inline bool terminal_program_fault_changes_selection(
    const ProgramNodeHoldStateDTO before) noexcept {
  return before == ProgramNodeHoldStateDTO::kRunning ||
         before == ProgramNodeHoldStateDTO::kReady ||
         before == ProgramNodeHoldStateDTO::kHeld;
}

// Called once per changed fence under the Kernel State lock. Never wrap or
// rewrite an already older legacy selection when terminal facts arrive.
inline void advance_terminal_program_fault_selection(
    std::uint64_t& selection_revision, std::uint64_t& legacy_selection_revision,
    const std::uint64_t maximum_revision) noexcept {
  if (selection_revision >= maximum_revision) return;
  const bool legacy_current = legacy_selection_revision == selection_revision;
  ++selection_revision;
  if (legacy_current) legacy_selection_revision = selection_revision;
}

}  // namespace manyforge::core::resource_addressed_kernel_detail

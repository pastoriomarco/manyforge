#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_core/types/stop_timing.hpp>
#include <optional>
#include <span>

namespace manyforge::core {

/// Authored full Soft/Spring duration, rounded to the nearest nanosecond
/// (half up). Draft, Apply and Prepare share this conversion so decimal input
/// does not spuriously add a nanosecond at an otherwise exact boundary.
[[nodiscard]] Result<std::int64_t> authored_stop_duration_ns(double seconds);

/// Pure cold derivation, not policy qualification or permission to Stop.
/// Absence retains the exact qualified default budget. A configured nominal
/// timeout shifts the existing post-fault suffix without changing its spans,
/// actions or evidence. Intermediate nominal phases retain their offsets.
[[nodiscard]] Result<StopDeadlineBudgetDTO> resolve_stop_timing_budget(
    const StopEvidencePolicyDTO& policy, const std::optional<StopTimingParametersDTO>& timing);

/// Field and range validation only. As `main` (item 24) the timeout is not
/// compared with the profile in advance: the Stop deadline watchdog issues the
/// hard floor on overrun.
[[nodiscard]] Result<void> validate_controlled_stop_timing(const StopTimingParametersDTO& timing,
                                                           std::int64_t full_profile_duration_ns);

/// One internal cell envelope, without extending any target's operational
/// cutoff or phase. Called only on complete, cold compiled route inventories.
[[nodiscard]] Result<void> harmonize_stop_timing_budgets(std::span<StopDeadlineBudgetDTO> budgets);

}  // namespace manyforge::core

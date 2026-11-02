#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/resolved_stop_base_plan.hpp>
#include <string>
#include <string_view>

namespace manyforge::core {

inline constexpr std::string_view kResolvedStopBasePlanApiVersion =
    "manyforge.resolved_stop_base_plan.v1";
inline constexpr std::string_view kResolvedStopBasePlanIdentityDomain =
    "resolved-stop-base-plan-id";

/// Validate one complete normalized value and derive its sole content ID.
/// The caller-supplied own ID is ignored and replaced, preventing circularity.
[[nodiscard]] Result<ResolvedStopBasePlanDTO> MakeResolvedStopBasePlan(
    ResolvedStopBasePlanDTO plan);

/// Repeat all closed-shape, ordering, cross-reference, and own-ID checks.
[[nodiscard]] Result<void> VerifyResolvedStopBasePlan(const ResolvedStopBasePlanDTO& plan);

/// Emit the complete RFC-8785 bytes including the verified own ID.
[[nodiscard]] Result<std::string> EncodeResolvedStopBasePlan(const ResolvedStopBasePlanDTO& plan);

/// Parse one exact canonical wire object and repeat all closed-shape,
/// normalization, cross-reference, bound, and own-ID checks. Non-canonical
/// JSON spellings, explicit null optionals, and unknown/missing members fail
/// closed.
[[nodiscard]] Result<ResolvedStopBasePlanDTO> DecodeResolvedStopBasePlan(
    std::string_view canonical_bytes);

}  // namespace manyforge::core

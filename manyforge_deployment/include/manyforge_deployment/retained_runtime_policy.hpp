#pragma once

#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_policy_codec.hpp>
#include <optional>
#include <stdexcept>

namespace manyforge::deployment {

class RetainedRuntimePolicyError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Re-read and independently qualify the sole retained v1 runtime policy.
///
/// Absence is observable and leaves motion unqualified. More than one policy
/// is ambiguous because v1 has no runtime policy selector.
[[nodiscard]] std::optional<QualifiedRuntimePolicy>
resolve_retained_runtime_policy(const RetainedBundle &bundle,
                                const ICatalogArtifactSource &artifact_store);

/// Resolve the cell-wide envelope from exactly the retained binding policies.
/// Empty and entirely not-stoppable products have no Stop envelope. This is
/// declarative readback, not proof of quiescence or authority to move/Stop.
[[nodiscard]] std::optional<core::StopDeadlineBudgetDTO>
resolve_retained_stop_deadline_budget(
    const RetainedBundle &bundle, const ICatalogArtifactSource &artifact_store);

} // namespace manyforge::deployment

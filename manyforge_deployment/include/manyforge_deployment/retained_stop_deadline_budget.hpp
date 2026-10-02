#pragma once

#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <optional>
#include <stdexcept>

namespace manyforge::deployment {

class RetainedStopDeadlineBudgetError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Resolve the cell-wide envelope from exactly the retained binding policies.
/// Empty and entirely not-stoppable products have no Stop envelope. This is
/// declarative readback, not proof of quiescence or authority to move/Stop.
[[nodiscard]] std::optional<core::StopDeadlineBudgetDTO>
resolve_retained_stop_deadline_budget(
    const RetainedBundle &bundle, const ICatalogArtifactSource &artifact_store);

} // namespace manyforge::deployment

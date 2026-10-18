#pragma once

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

namespace manyforge::core {

/// Construct an unvalidated replacement over the same exact joint/resources.
/// All times and P/V/A must use the same path-time/scaling convention.
///
/// Preserve active's original polynomials through divergence_path_s, with
/// every retained time shifted by -elapsed_path_s (including negative past
/// points). A single full-P/V/A quintic connects that exact sampled state to
/// successor at successor_entry_path_s over connector_duration_path_s. The
/// remaining successor suffix is translated in time, never retimed. Its
/// columns may be reordered only with their exact corresponding resources.
///
/// Divergence must remain strictly ahead of elapsed; neither input cut may
/// extrapolate. Reject malformed/incomplete/nonfinite inputs, unrepresentable
/// arithmetic, or a cut coalesced by the existing Stop join's time resolution.
/// Every translated interval must preserve its source duration to both 1 ns
/// and sqrt(machine epsilon) relative to that local duration; ill-conditioned
/// large-offset or tiny intervals are refused rather than globally retimed.
/// Preserve/rebase every successor Cartesian milestone; reject a cut that
/// would skip one. Derived scaling/fidelity certificates are not transferable
/// to the new curve and are absent from the result.
///
/// This is construction only: it grants no dispatch/Stop permission and owns
/// no clocks, budgets, monitoring or policy. The owner retains authored motion
/// constraints and must validate the actual composite's interior dynamics,
/// fidelity, collisions, scene and current source before dispatch.
[[nodiscard]] Result<TrajectoryDTO> build_quintic_trajectory_splice(
    const TrajectoryDTO& active, const TrajectoryDTO& successor, double elapsed_path_s,
    double divergence_path_s, double successor_entry_path_s, double connector_duration_path_s);

/// Exact future-only validation view: sample full P/V/A at start_path_s as
/// time zero, then retain later knots with their times translated only. The
/// cut must be inside the curve with a nonempty future interval. No negative
/// history is retained; this does not modify the physical delivery curve.
/// Preserve/rebase all milestones or refuse if the cut would skip one. Derived
/// scaling/fidelity certificates are absent. In particular, do not fabricate a
/// Cartesian milestone at zero: the owner must retain authored obligations and
/// reconcile their semantic mapping before ordinary prepared serialization.
[[nodiscard]] Result<TrajectoryDTO> slice_future_trajectory(const TrajectoryDTO& trajectory,
                                                            double start_path_s);

}  // namespace manyforge::core

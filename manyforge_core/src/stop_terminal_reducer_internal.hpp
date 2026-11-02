#pragma once

#include <manyforge_core/stop_terminal_reducer.hpp>
#include <span>
#include <vector>

namespace manyforge::core::internal {

/// Exact participant rows inherited from one source certificate for settled-proof reuse.
struct StopProofPredecessorScope final {
  StopCertificateRefDTO certificate;
  std::vector<StopScopeEntryKeyDTO> matched_scope_rows;
};

[[nodiscard]] Result<StopCertificateDTO> ReduceStopTerminalWithProofScopes(
    const StopTerminalReductionInputDTO& input,
    std::span<const StopProofPredecessorScope> proof_scopes);

}  // namespace manyforge::core::internal

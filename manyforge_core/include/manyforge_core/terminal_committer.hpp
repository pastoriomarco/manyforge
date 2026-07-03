#pragma once

#include <manyforge_core/state_machine.hpp>
#include <manyforge_core/types/execution_status.hpp>

#include <functional>
#include <mutex>
#include <string>

namespace manyforge::core {

/// The single authority for committing TERMINAL execution states
/// (`Completed`, `Stopped`, `Failed`, `Interrupted`, `InterventionRequired`).
///
/// Why it exists (spec/230; decomposition plan K3): terminal decisions used
/// to live in ~35 scattered `try_transition` call sites. Every new terminal
/// path re-derived the authority rules by hand, and the inspect-then-commit
/// window let a Stop / fault latch that landed mid-decision be laundered
/// into a plain `kFailed` / `kCompleted`. The committer serializes
/// inspect+commit under one lock so the decided terminal reflects every
/// authority active at commit time.
///
/// Lock discipline: `commit_mutex_` is one lock level above the probe locks.
/// It is acquired ONLY by `commit()`, which then calls the authority probes
/// (which may take their own leaf locks: stop policy, interrupt manager,
/// state machine). No code path takes `commit_mutex_` while already holding
/// any of those locks — probes and state-machine internals never call back
/// into the committer. Keep it that way: the committer must stay a top-level
/// entry point, never a callee of subsystem internals.
class TerminalCommitter {
 public:
  enum class Origin {
    /// Normal worker/backend/admission path. Any active stop, trip, or fault
    /// authority may override a normal terminal proposal.
    kGeneric,
    /// The stop authority concluding its own stop path.
    kStopAuthority,
    /// The motion-trip handler concluding its own controlled-stop window.
    kMotionTrip,
  };

  /// Authority probes, supplied by the kernel. Each returns whether that
  /// authority is currently asserted. Probes must be safe to call while
  /// holding no kernel locks and must not call back into the committer.
  using Probe = std::function<bool()>;

  struct CommitRequest {
    ExecutionState proposed;  ///< terminal the calling path derived
    const char* reason;       ///< stable correlation token for diagnostics
    Origin origin = Origin::kGeneric;
  };

  struct CommitOutcome {
    /// The terminal actually committed (authority-decided; may differ
    /// from `proposed`). Callers publishing snapshots/events for this
    /// commit MUST use this state, not the proposed one.
    ExecutionState committed{ExecutionState::kIdle};
    /// True when an active authority overrode the proposed terminal.
    bool overrode{false};
    /// True when the state machine accepted the transition (or was
    /// already in the committed state).
    bool accepted{false};
    constexpr bool ok() const { return accepted; }
  };

  TerminalCommitter(ExecutionStateMachine& sm,
                    Probe stop_requested,
                    Probe fault_latch_active,
                    Probe trip_stop_active);

  /// Decide-and-commit under the commit lock. The committer, not the
  /// caller, owns the final terminal: a normal `Completed`/`Failed`
  /// proposal is escalated when a stop or fault-latch authority is
  /// asserted at commit time. Safety proposals pass through unchanged.
  CommitOutcome commit(const CommitRequest& request);

  static bool is_terminal(ExecutionState state);

 private:
  ExecutionState decide(const CommitRequest& request,
                        ExecutionState current) const;

  ExecutionStateMachine& sm_;
  Probe stop_requested_;
  Probe fault_latch_active_;
  Probe trip_stop_active_;
  std::mutex commit_mutex_;
};

}  // namespace manyforge::core

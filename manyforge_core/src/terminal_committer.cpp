#include <manyforge_core/terminal_committer.hpp>

namespace manyforge::core {

TerminalCommitter::TerminalCommitter(ExecutionStateMachine& sm,
                                     Probe stop_requested,
                                     Probe fault_latch_active,
                                     Probe trip_stop_active)
    : sm_(sm),
      stop_requested_(std::move(stop_requested)),
      fault_latch_active_(std::move(fault_latch_active)),
      trip_stop_active_(std::move(trip_stop_active)) {}

bool TerminalCommitter::is_terminal(ExecutionState state) {
  switch (state) {
    case ExecutionState::kCompleted:
    case ExecutionState::kStopped:
    case ExecutionState::kFailed:
    case ExecutionState::kInterrupted:
    case ExecutionState::kInterventionRequired:
      return true;
    default:
      return false;
  }
}

ExecutionState TerminalCommitter::decide(const CommitRequest& request,
                                         ExecutionState current) const {
  const ExecutionState proposed = request.proposed;
  const bool stop_owner = request.origin == Origin::kStopAuthority ||
                          request.origin == Origin::kMotionTrip;
  const bool trip_owner = request.origin == Origin::kMotionTrip;
  // Deterministic authority priority (spec/230): a safety authority
  // asserted at commit time outranks a NORMAL terminal proposal.
  // Safety proposals are never downgraded here. Priority:
  //   InterventionRequired > Interrupted (fault latch) > Stopped (operator)
  //   > Failed > Completed.
  if (proposed == ExecutionState::kInterventionRequired ||
      proposed == ExecutionState::kInterrupted ||
      proposed == ExecutionState::kStopped) {
    return proposed;
  }
  // Commits from kStopping are the stop/trip authority CONCLUDING its own
  // stop: Completed (natural completion inside the stop window, spec/230
  // §3.4), Failed (stop failure), or Stopped. The stop-request and
  // held-latch probes are by definition asserted throughout that window —
  // escalating here would overwrite the authority's own verdict. The
  // laundering window the committer closes lives in the worker/replan
  // phases (Planning / Validating / Executing). A non-owner normal terminal
  // racing while the state machine is already Stopping is not allowed to use
  // this exemption.
  if (current == ExecutionState::kStopping && stop_owner) {
    return proposed;
  }
  if (trip_stop_active_ && trip_stop_active_() && !trip_owner) {
    return ExecutionState::kInterventionRequired;
  }
  if (fault_latch_active_ && fault_latch_active_()) {
    return ExecutionState::kInterrupted;
  }
  if (stop_requested_ && stop_requested_()) {
    return ExecutionState::kStopped;
  }
  if (current == ExecutionState::kStopping) {
    return ExecutionState::kInterventionRequired;
  }
  return proposed;
}

TerminalCommitter::CommitOutcome TerminalCommitter::commit(
    const CommitRequest& request) {
  std::lock_guard<std::mutex> lock(commit_mutex_);
  CommitOutcome outcome;
  const ExecutionState current = sm_.status().state;
  ExecutionState decided = decide(request, current);
  if (current == decided) {
    outcome.committed = decided;
    outcome.overrode = decided != request.proposed;
    outcome.accepted = true;
    return outcome;
  }
  auto attempt = sm_.try_transition(decided);
  if (!attempt.ok() && decided != request.proposed) {
    // The escalated terminal is not reachable from the current phase
    // (e.g. Stopping does not admit Interrupted). InterventionRequired is
    // reachable from every state and is the fail-closed safety sink for
    // an authority that cannot be expressed any other way.
    decided = ExecutionState::kInterventionRequired;
    attempt = sm_.try_transition(decided);
  }
  if (!attempt.ok()) {
    // Table rejected the proposal outright (e.g. a normal terminal from
    // an already-latched safety terminal). Fail closed: report the
    // CURRENT state so callers publish the truth, never the rejected
    // proposal.
    outcome.committed = sm_.status().state;
    outcome.overrode = outcome.committed != request.proposed;
    outcome.accepted = false;
    return outcome;
  }
  outcome.committed = decided;
  outcome.overrode = decided != request.proposed;
  outcome.accepted = true;
  return outcome;
}

}  // namespace manyforge::core

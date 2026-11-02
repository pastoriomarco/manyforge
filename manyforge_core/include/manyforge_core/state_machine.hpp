#pragma once

#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/execution_status.hpp>

#include <mutex>

namespace manyforge::core {

/// Thread-safe working-phase machine of one participant execution.
///
/// Phases: Idle -> Planning <-> Validating -> Executing. An execution ends by
/// recording exactly one participant fact (settle); it then keeps its last
/// phase until retirement returns it to Idle. The machine never holds an
/// operation outcome: the addressed Kernel derives outcomes from the facts.
class ExecutionStateMachine {
 public:
  explicit ExecutionStateMachine(IClock& clock);

  /// Working-phase transition. Rejected for a non-working target state, an
  /// invalid edge, or once the current execution has settled.
  [[nodiscard]] Result<void> try_transition(ExecutionState phase);

  /// Record how the current execution ended. Accepted once per execution and
  /// only from a working phase; a later fact is rejected and the first kept.
  [[nodiscard]] Result<void> settle(ParticipantExecutionFact fact);

  /// Retire the current execution: Idle, no fact, no progress or error.
  void retire();

  ExecutionStatus status() const;
  bool settled() const;

  void set_progress(double progress);
  void set_execution_id(uint64_t execution_id);

 private:
  IClock& clock_;
  mutable std::mutex mutex_;
  ExecutionStatus status_;
};

}  // namespace manyforge::core

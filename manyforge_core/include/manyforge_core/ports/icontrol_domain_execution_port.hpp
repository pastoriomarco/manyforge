#pragma once

#include <manyforge_core/types/participant_stop_stage.hpp>
#include <stop_token>

namespace manyforge::core {

/// Focused binding-owned Stop-stage realization. Kernel alone selects the
/// stage and advances/terminalizes the aggregate Stop transaction.
///
/// Implementations are admitted only when every provider/transport operation
/// is independently cancellable or has a retained finite timeout. The call
/// must return no later than the robot's own Stop-time bound (its supplied
/// controlled deadline, or its lane's stop-completion timeout for a hard
/// stage, Q51/Q57) and must not rely on caller thread abandonment to make progress. A provider
/// that cannot satisfy that property requires a supervised runtime-unit
/// process with bounded IPC; an ordinary finite worker pool is not sufficient
/// isolation because hung calls can exhaust it.
class IControlDomainExecutionPort {
 public:
  virtual ~IControlDomainExecutionPort() = default;

  [[nodiscard]] virtual ParticipantStopStageResultDTO TranslateStopStage(
      const ResolvedParticipantStopStageDTO& stage, const RemainingStopBudgetDTO& budget,
      std::stop_token cancellation) = 0;
};

}  // namespace manyforge::core

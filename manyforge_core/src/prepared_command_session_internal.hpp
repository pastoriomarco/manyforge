#pragma once

#include <atomic>
#include <limits>
#include <manyforge_core/ports/iresolved_prepared_participant_execution_engine.hpp>
#include <manyforge_core/types/capability_command_receipt.hpp>
#include <manyforge_core/types/error_code_mapping.hpp>
#include <memory>
#include <mutex>
#include <optional>

namespace manyforge::core::detail {

/// Shared resident-session bookkeeping only. The enclosing command engine
/// owns this value, its immutable authority and transport-specific behavior.
/// All methods below require the caller to hold mutex.
struct PreparedCommandSessionLifecycle {
  mutable std::mutex mutex;
  std::uint64_t preparation_id{};
  std::atomic<ResolvedPreparedCapabilityCommandStateDTO> state{
      ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed};
  std::optional<ControllerGoalId> controller_goal_id;
  std::optional<ControllerGoalGeneration> controller_goal_generation;
  std::optional<CapabilityCommandDispatchReceiptDTO> dispatch_result;
  std::optional<ErrorDTO> error;
  bool ordinary_result_acknowledged{};

  template <typename BeforeCommit>
  Result<std::optional<CapabilityCommandDispatchReceiptDTO>> BeginDispatch(
      std::uint64_t supplied_preparation_id, const ContentDigest* authoritative_digest,
      const ContentDigest& supplied_digest, const ControllerGoalId& goal,
      ControllerGoalGeneration generation, BeforeCommit&& before_commit) {
    using Reply = Result<std::optional<CapabilityCommandDispatchReceiptDTO>>;
    if (preparation_id != supplied_preparation_id)
      return Reply::Err({kErrorMissingReference,
                         "Prepared capability-command identity is stale",
                         {{"reason", "prepared_command_identity_mismatch"}}});
    const bool exact = authoritative_digest && *authoritative_digest == supplied_digest &&
                       controller_goal_id == goal && controller_goal_generation == generation;
    if ((state == ResolvedPreparedCapabilityCommandStateDTO::kDispatched ||
         state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed) &&
        exact && dispatch_result)
      return Reply::Ok(dispatch_result);
    if (state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared || !authoritative_digest ||
        *authoritative_digest != supplied_digest || goal.empty() || !generation.nonzero())
      return Reply::Err({kErrorInvalidRequest,
                         "Prepared capability-command dispatch identity is invalid",
                         {{"reason", "prepared_command_dispatch_mismatch"}}});
    controller_goal_id = goal;
    controller_goal_generation = generation;
    // Preserve the existing order: transport-specific precommit capture runs
    // before publishing the irreversible DispatchRequested transition.
    before_commit();
    state = ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested;
    return Reply::Ok(std::nullopt);
  }

  void RecordDispatch(CapabilityCommandDispatchReceiptDTO receipt) {
    dispatch_result = std::move(receipt);
    error = dispatch_result->error;
    state = dispatch_result->accepted && !dispatch_result->error
                ? ResolvedPreparedCapabilityCommandStateDTO::kDispatched
                : ResolvedPreparedCapabilityCommandStateDTO::kDispatchFailed;
  }

  Result<void> CancelBeforeDispatch(std::uint64_t supplied_preparation_id) {
    if (preparation_id != supplied_preparation_id)
      return Result<void>::Err({kErrorMissingReference,
                                "Prepared capability-command identity is stale",
                                {{"reason", "prepared_command_identity_mismatch"}}});
    if (state == ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch)
      return Result<void>::Ok();
    if (state != ResolvedPreparedCapabilityCommandStateDTO::kPrepared &&
        state != ResolvedPreparedCapabilityCommandStateDTO::kPreparationFailed)
      return Result<void>::Err({std::string(to_string(ErrorCode::kInvalidStateTransition)),
                                "A released capability command requires the Stop-stage path",
                                {{"reason", "prepared_command_already_dispatched"}}});
    state = ResolvedPreparedCapabilityCommandStateDTO::kCancelledPreDispatch;
    return Result<void>::Ok();
  }
};

/// The enclosing engine owns one bounded resident slot. The callback supplies
/// only transport-specific evidence of a continuing effect; allocation,
/// digest replay and publication retain the existing command-engine order.
template <typename Session, typename Digest, typename ContinuingEffect>
Result<std::uint64_t> AdmitPreparedCommandSession(std::shared_ptr<Session> candidate,
                                                  std::atomic<std::shared_ptr<Session>>& resident,
                                                  std::mutex& resident_mutex,
                                                  std::uint64_t& next_execution_id, Digest&& digest,
                                                  ContinuingEffect&& continuing_effect) {
  std::lock_guard active_lock(resident_mutex);
  if (const auto active = resident.load()) {
    std::lock_guard prior_lock(active->mutex);
    if (digest(*active) != nullptr && digest(*candidate) != nullptr &&
        *digest(*active) == *digest(*candidate))
      return Result<std::uint64_t>::Ok(active->preparation_id);
    const bool effect_active =
        active->state == ResolvedPreparedCapabilityCommandStateDTO::kDispatchRequested ||
        continuing_effect(*active);
    if (active->state == ResolvedPreparedCapabilityCommandStateDTO::kPrepared || effect_active)
      return Result<std::uint64_t>::Err({kErrorInvalidRequest,
                                         "Another prepared capability command is still resident",
                                         {{"reason", "prepared_command_session_busy"}}});
  }
  if (next_execution_id == std::numeric_limits<std::uint64_t>::max())
    return Result<std::uint64_t>::Err({kErrorInternalError,
                                       "Prepared command identity space is exhausted",
                                       {{"reason", "prepared_command_identity_exhausted"}}});
  const auto preparation_id = next_execution_id++;
  candidate->preparation_id = preparation_id;
  resident = std::move(candidate);
  return Result<std::uint64_t>::Ok(preparation_id);
}

}  // namespace manyforge::core::detail

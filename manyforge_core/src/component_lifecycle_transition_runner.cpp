#include <algorithm>
#include <condition_variable>
#include <limits>
#include <manyforge_core/component_lifecycle_transition_runner.hpp>
#include <mutex>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] ErrorDTO runner_error(std::string code, std::string message, std::string reason) {
  return {std::move(code), std::move(message), {{"reason", std::move(reason)}}};
}

[[nodiscard]] ErrorDTO cancelled_error() {
  return runner_error(std::string(to_string(ErrorCode::kUnsupportedCapability)),
                      "the lifecycle transition was cancelled",
                      "external_brand_lifecycle_cancelled");
}

}  // namespace

void WaitLifecycleInterruptibly(std::int64_t ns, std::stop_token cancellation) {
  std::mutex mutex;
  std::condition_variable_any woken;
  std::unique_lock lock(mutex);
  static_cast<void>(woken.wait_for(lock, cancellation, std::chrono::nanoseconds(ns),
                                   [] { return false; }));
}

BrandLifecyclePrimitiveResultDTO RunLifecycleTransition(
    const BrandLifecyclePrimitiveIntentDTO& intent, const ILifecycleAdmission& admission,
    const LifecycleTransitionRunnerIo& io, const LifecycleSubstepFeedbackSink& feedback,
    std::stop_token cancellation) {
  BrandLifecyclePrimitiveResultDTO result{intent.lifecycle_transaction_id,
                                          intent.lifecycle_transaction_generation,
                                          intent.controllable_component_id,
                                          intent.control_domain_id,
                                          intent.control_domain_revision,
                                          intent.provider_instance_id,
                                          intent.provider_manifest_id,
                                          intent.provider_manifest_revision,
                                          intent.provider_runtime_unit_id,
                                          intent.provider_runtime_epoch,
                                          intent.lifecycle_primitive_id,
                                          intent.lifecycle_primitive_attempt_id,
                                          intent.expected_observation_generation,
                                          LifecycleTransitionOutcomeDTO::kSucceeded,
                                          std::nullopt};
  const auto finish = [&](LifecycleTransitionOutcomeDTO outcome, std::optional<ErrorDTO> error) {
    result.outcome = outcome;
    result.error = std::move(error);
    return result;
  };
  const auto failed = [&](ErrorDTO error) {
    return finish(LifecycleTransitionOutcomeDTO::kFailed, std::move(error));
  };
  const auto transition = LifecycleTransitionIntent(intent.lifecycle_primitive_id);
  if (!transition) {
    return failed(runner_error("component_lifecycle_primitive_invalid",
                               "the selected lifecycle primitive is invalid",
                               "transition_action_unknown"));
  }
  const auto started_at_ns = io.clock->now();
  const auto deadline_ns =
      started_at_ns > std::numeric_limits<std::int64_t>::max() - intent.remaining_duration_ns
          ? std::numeric_limits<std::int64_t>::max()
          : started_at_ns + intent.remaining_duration_ns;
  const auto remaining = [&] {
    const auto now_ns = io.clock->now();
    return now_ns >= deadline_ns ? std::int64_t{0} : deadline_ns - now_ns;
  };
  const auto fresh = [&](const BrandLifecycleObservationDTO& observation) {
    const auto now_ns = io.clock->now();
    return observation.received_timestamp_ns <= now_ns && now_ns < observation.fresh_until_ns;
  };
  const auto emit = [&](LifecycleSubstepEventDTO event) {
    if (feedback) feedback(event);
  };

  if (cancellation.stop_requested()) return failed(cancelled_error());
  auto first = io.observe();
  if (!first.ok()) return failed(first.error());
  if (!fresh(first.value())) {
    return failed(runner_error("component_lifecycle_observation_stale",
                               "the lifecycle provider observation is stale or mismatched",
                               "initial_observation_not_fresh"));
  }
  auto observation = std::move(first.value());
  while (true) {
    if (cancellation.stop_requested()) return failed(cancelled_error());
    const auto answer = admission.Evaluate(*transition, intent.requested_state, observation);
    if (!answer.ok()) return failed(answer.error());
    if (answer.value().postcondition == LifecyclePostconditionResultDTO::kSatisfied)
      return finish(LifecycleTransitionOutcomeDTO::kSucceeded, std::nullopt);
    if (answer.value().not_ready) return failed(*answer.value().not_ready);
    if (!answer.value().selected_substep) {
      return failed(runner_error("component_lifecycle_primitive_invalid",
                                 "the selected lifecycle primitive is invalid",
                                 "substep_not_selected"));
    }
    if (result.substeps.size() >= kMaxLifecycleSubsteps) {
      return failed(runner_error(
          "component_lifecycle_postcondition_timeout",
          "the lifecycle primitive postcondition was not satisfied before deadline",
          "substep_bound_reached"));
    }
    const auto left = remaining();
    if (left == 0) {
      return failed(runner_error("component_lifecycle_deadline_expired",
                                 "the lifecycle deadline expired before dispatch",
                                 "deadline_exhausted"));
    }
    const auto substep = *answer.value().selected_substep;
    const auto substep_started_at_ns = io.clock->now();
    emit({intent.lifecycle_primitive_attempt_id, substep.lifecycle_primitive_id, std::nullopt,
          LifecycleSubstepEventDTO::Phase::kStarted, substep_started_at_ns, std::nullopt,
          std::nullopt});
    auto outcome = io.dispatch_substep(substep.lifecycle_primitive_id,
                                       observation.observation_generation, left, feedback,
                                       cancellation);
    if (outcome.outcome != LifecycleTransitionOutcomeDTO::kSucceeded && !outcome.error) {
      outcome.error = runner_error("component_lifecycle_primitive_failed",
                                   "the lifecycle primitive did not succeed", "primitive_failed");
    }
    const auto substep_finished_at_ns = io.clock->now();
    result.substeps.push_back({substep.lifecycle_primitive_id, std::nullopt, outcome.outcome,
                               substep_started_at_ns, substep_finished_at_ns, outcome.error});
    for (auto& nested : outcome.nested) {
      if (result.substeps.size() < kMaxLifecycleSubsteps) result.substeps.push_back(std::move(nested));
    }
    emit({intent.lifecycle_primitive_attempt_id, substep.lifecycle_primitive_id, std::nullopt,
          LifecycleSubstepEventDTO::Phase::kFinished, substep_finished_at_ns, outcome.outcome,
          outcome.error ? std::optional(outcome.error->code) : std::nullopt});
    if (outcome.outcome != LifecycleTransitionOutcomeDTO::kSucceeded)
      return finish(outcome.outcome, std::move(outcome.error));

    // The substep's postcondition, on a fresh observation newer than the one
    // the decision used.
    auto minimum_generation = observation.observation_generation;
    const auto minimum_sequence = observation.source_sequence;
    bool fresh_seen = false;
    while (true) {
      if (cancellation.stop_requested()) return failed(cancelled_error());
      auto next = io.observe();
      if (next.ok() && fresh(next.value()) &&
          next.value().observation_generation > minimum_generation &&
          next.value().source_sequence > minimum_sequence) {
        if (EvaluateLifecyclePredicate(substep.postcondition_predicate, next.value()) ==
            LifecyclePostconditionResultDTO::kSatisfied) {
          observation = std::move(next.value());
          break;
        }
        fresh_seen = true;
        minimum_generation = next.value().observation_generation;
      }
      const auto wait_ns = remaining();
      if (wait_ns == 0) {
        return failed(
            fresh_seen
                ? runner_error(
                      "component_lifecycle_postcondition_timeout",
                      "the lifecycle primitive postcondition was not satisfied before deadline",
                      "transition_postcondition_unsatisfied")
                : runner_error(
                      "component_lifecycle_verification_timeout",
                      "no fresh post-dispatch lifecycle observation arrived before deadline",
                      "postcondition_observation_timeout"));
      }
      io.wait(std::min(io.poll_period_ns, wait_ns), cancellation);
    }
  }
}

}  // namespace manyforge::core

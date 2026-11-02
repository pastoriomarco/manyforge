#include <algorithm>
#include <deque>
#include <limits>
#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <map>
#include <mutex>
#include <set>
#include <tuple>
#include <utility>
#include <variant>

#include "io_blackboard_snapshot.hpp"

namespace manyforge::core {
namespace {

ErrorDTO invalid(const std::string& reason) {
  return {"invalid_request", "I/O signal request is invalid", {{"reason", reason}}};
}

ErrorDTO stale(const std::string& reason) {
  return {"stale_generation", "I/O signal identity is no longer current", {{"reason", reason}}};
}

ErrorDTO exhausted() {
  return {"invariant_violation",
          "I/O signal sequence space is exhausted",
          {{"reason", "io_sequence_exhausted"}}};
}

bool text_valid(const std::string& text, std::size_t maximum) {
  return !text.empty() && text.size() <= maximum && text.find('\0') == std::string::npos;
}

bool error_valid(const ErrorDTO& error, std::size_t maximum) {
  auto remaining = maximum;
  const auto consume = [&](const std::string& text) {
    if (text.size() > remaining) return false;
    remaining -= text.size();
    return true;
  };
  if (error.code.empty() || !consume(error.code) || !consume(error.message)) return false;
  for (const auto& [key, value] : error.details)
    if (!consume(key) || !consume(value)) return false;
  return true;
}

}  // namespace

bool operator==(const IoSignalRecordDTO& left, const IoSignalRecordDTO& right) {
  return left.cursor == right.cursor && left.incarnation == right.incarnation &&
         left.acquisition == right.acquisition && left.loss == right.loss &&
         io_blackboard_snapshots_equal(left.blackboard_snapshot, right.blackboard_snapshot) &&
         (left.retired_blackboard_scope == right.retired_blackboard_scope ||
          (left.retired_blackboard_scope && right.retired_blackboard_scope &&
           *left.retired_blackboard_scope == *right.retired_blackboard_scope));
}

struct IoSignalSubsystem::State final {
  struct Signal final {
    IoSignalDescriptorDTO descriptor;
    std::optional<IoSignalSampleDTO> latest;
    std::uint64_t latest_loss_generation{};
    /// Outputs with read-back that aren't controller owned: the core-held
    /// `changed_outside` signal of this output (CONTRACTS §7).
    std::optional<IoSignalId> changed_outside{};
  };
  struct Source final {
    std::optional<IoSourceIncarnation> incarnation;
    std::uint64_t last_source_sequence{};
    std::uint64_t record_sequence{};
    std::uint64_t loss_generation{};
    std::optional<TimePoint> last_received_at_ns;
    bool available{};
    std::optional<ErrorDTO> error;
    std::deque<IoSignalRecordDTO> records;
    std::size_t record_capacity{};
    bool blackboard_source{};
    /// The source of one core-held `changed_outside` signal; never published
    /// to, lost or replaced from outside.
    bool core_held{};
    bool blackboard_continuity_lost{};
    std::optional<IoProgramBlackboardSourceBindingDTO> blackboard_binding;
    std::shared_ptr<const IoProgramBlackboardSnapshotDTO> blackboard_snapshot;
  };
  struct Watch final {
    IoWatchRegistrationDTO registration;
    IoSignalId signal_id;
    bool expected_value;
    std::optional<IoSignalSampleDTO> matched;
  };

  struct BlackboardInput final {
    InterruptSourceId source_id;
    IoSourceIncarnation incarnation;
    std::size_t value_index;
  };
  struct Rule final {
    IoRuleRegistrationDTO registration;
    std::shared_ptr<const CheckedIoCondition> condition;
    MotionInterruptActivationMode activation_mode;
    std::vector<InterruptSourceId> required_sources;
    std::vector<std::optional<IoRuleSourceLossEvidenceDTO>> losses;
    std::vector<IoConditionValueDTO> values;
    std::vector<const IoConditionValueDTO*> borrowed_values;
    std::vector<std::optional<BlackboardInput>> blackboard_inputs;
    std::vector<std::uint8_t> workspace;
    IoRuleTripEvidenceDTO trip;
    bool tripped{};
    bool previous_value{};
    std::optional<ErrorDTO> evaluation_error;
    std::optional<bool> current_value;
    TimePoint current_evaluated_at_ns;
  };
  using Registration = std::variant<Watch, Rule>;

  State(IoSignalCatalogDTO catalog, IoSignalLimitsDTO supplied_limits, IClock& supplied_clock)
      : owner_epoch(std::move(catalog.owner_epoch)),
        generation(catalog.generation),
        limits(supplied_limits),
        clock(supplied_clock) {
    for (auto& descriptor : catalog.signals) {
      auto& source = sources.try_emplace(descriptor.source_id).first->second;
      const auto depth = limits.records_per_source_overrides.find(descriptor.source_id);
      source.record_capacity = depth == limits.records_per_source_overrides.end()
                                   ? limits.records_per_source
                                   : depth->second;
      const auto id = descriptor.signal_id;
      signals.emplace(id, Signal{std::move(descriptor), std::nullopt});
    }
    // Every core-held `changed_outside` signal is an always-fresh in-process
    // input on its own core source, starting false.
    const auto now = clock.now();
    std::vector<IoSignalDescriptorDTO> derived;
    for (auto& [output_id, output] : signals) {
      if (!HoldsChangedOutside(output.descriptor)) continue;
      const auto id = ChangedOutsideSignalId(output_id);
      const auto source_id = ChangedOutsideSourceId(output_id);
      auto& source = sources[source_id];
      source.incarnation = IoSourceIncarnation("core");
      source.available = true;
      source.core_held = true;
      source.record_capacity = limits.records_per_source;
      output.changed_outside = id;
      derived.push_back({id, source_id, IoSignalValueType::kBoolean, IoSignalDirection::kInput,
          IoSignalUpdateMode::kChangeOnly, IoTransitionCapability::kSampledOnly,
          {IoFreshnessMode::kProviderValidity, std::nullopt}, std::nullopt,
          IoSignalLivenessMode::kInProcess});
    }
    for (auto& descriptor : derived) {
      const auto id = descriptor.signal_id;
      const auto source_id = descriptor.source_id;
      signals.emplace(id, Signal{std::move(descriptor), std::nullopt});
      Commit({source_id, IoSourceIncarnation("core"), 1U, now,
              {{id, IoSampleValidity::kValid, false, std::nullopt}}});
    }
  }

  /// Applies one validated acquisition: samples, record, watches, rules. An
  /// output sample entering `outside` sets its `changed_outside` signal; one
  /// entering `manyforge_write` (our accepted write) clears it.
  IoSourceCursorDTO Commit(const IoSourceAcquisitionDTO& acquisition) {
    auto& source = sources.at(acquisition.source_id);
    const bool gap = source.last_source_sequence != 0U &&
                     acquisition.source_sequence - source.last_source_sequence != 1U;
    if (gap) {
      ++source.loss_generation;
      Append(acquisition.source_id, source, std::nullopt,
             IoSourceLossDTO{IoSourceLossReason::kSequenceGap, source.last_source_sequence,
                             source.last_source_sequence + 1U,
                             acquisition.source_sequence - 1U, std::nullopt, std::nullopt});
    }
    std::vector<std::pair<IoSignalId, bool>> outside_changes;
    // Commit the entire source acquisition before any watch evaluation.
    for (const auto& update : acquisition.updates) {
      auto& signal = signals.at(update.signal_id);
      if (signal.changed_outside && update.validity == IoSampleValidity::kValid) {
        const auto& prior = signal.latest;
        const bool repeated = prior && prior->publication.source == update.source &&
                              prior->publication.value == update.value;
        if (!repeated && update.source == IoSignalSampleSource::kOutside)
          outside_changes.emplace_back(update.signal_id, true);
        if (!repeated && update.source == IoSignalSampleSource::kManyforgeWrite)
          outside_changes.emplace_back(update.signal_id, false);
      }
      signal.latest = IoSignalSampleDTO{
          {update.signal_id, acquisition.source_id, acquisition.incarnation,
           acquisition.source_sequence, update.validity, update.value, update.error, update.source},
          acquisition.trusted_received_at_ns};
      signal.latest_loss_generation = source.loss_generation;
    }
    source.last_source_sequence = acquisition.source_sequence;
    source.last_received_at_ns = acquisition.trusted_received_at_ns;
    source.available = true;
    source.error.reset();
    Append(acquisition.source_id, source, acquisition, std::nullopt);
    const auto now = clock.now();
    for (const auto& update : acquisition.updates) {
      const auto& signal = signals.at(update.signal_id);
      if (Availability(signal, now) != IoSignalAvailability::kFresh) continue;
      for (auto& [watch_id, registration] : watches) {
        (void)watch_id;
        auto* watch = std::get_if<Watch>(&registration);
        if (watch && !watch->matched && watch->signal_id == update.signal_id &&
            watch->expected_value == *update.value)
          watch->matched = signal.latest;
      }
    }
    const IoRuleAcquisitionKeyDTO acquired{acquisition.source_id, acquisition.incarnation,
                                           acquisition.source_sequence,
                                           Cursor(acquisition.source_id, source)};
    EvaluateRegisteredRules(now, true, &acquired);
    const auto cursor = Cursor(acquisition.source_id, source);
    for (const auto& [output, changed] : outside_changes) SetChangedOutside(output, changed, now);
    return cursor;
  }

  /// One change of an output's core-held `changed_outside` signal.
  void SetChangedOutside(const IoSignalId& output, bool value, TimePoint now) {
    const auto& derived_id = *signals.at(output).changed_outside;
    const auto& derived = signals.at(derived_id);
    if (derived.latest && derived.latest->publication.value == std::optional(value)) return;
    const auto& source_id = derived.descriptor.source_id;
    const auto& source = sources.at(source_id);
    Commit({source_id, *source.incarnation, source.last_source_sequence + 1U, now,
            {{derived_id, IoSampleValidity::kValid, value, std::nullopt}}});
  }

  IoSourceCursorDTO Cursor(const InterruptSourceId& id, const Source& source) const {
    return {owner_epoch, generation, id, source.record_sequence};
  }

  bool CursorCurrent(const IoSourceCursorDTO& cursor) const {
    return cursor.owner_epoch == owner_epoch && cursor.catalog_generation == generation;
  }

  bool CanRecord(const Source& source, std::uint64_t count) const {
    return count <= std::numeric_limits<std::uint64_t>::max() - source.record_sequence;
  }

  void Append(const InterruptSourceId& id, Source& source,
              std::optional<IoSourceAcquisitionDTO> acquisition,
              std::optional<IoSourceLossDTO> loss,
              std::shared_ptr<const IoProgramBlackboardSnapshotDTO> blackboard_snapshot = {},
              std::shared_ptr<const IoProgramBlackboardScopeDTO> retired_scope = {}) {
    ++source.record_sequence;
    source.records.push_back({Cursor(id, source), *source.incarnation, std::move(acquisition),
                              std::move(loss), std::move(blackboard_snapshot),
                              std::move(retired_scope)});
    if (source.records.size() > source.record_capacity) source.records.pop_front();
    if (source.records.back().loss)
      RecordRuleSourceLoss(id, source, *source.records.back().loss, clock.now());
  }

  IoSignalAvailability Availability(const Signal& signal, TimePoint now) const {
    const auto& source = sources.at(signal.descriptor.source_id);
    auto availability = IoSignalAvailability::kMissing;
    if (source.incarnation &&
        (!source.available ||
         (signal.latest && signal.latest_loss_generation != source.loss_generation))) {
      availability = IoSignalAvailability::kUnavailable;
    } else if (signal.latest) {
      switch (signal.latest->publication.validity) {
        case IoSampleValidity::kInvalid:
          availability = IoSignalAvailability::kInvalid;
          break;
        case IoSampleValidity::kUnavailable:
          availability = IoSignalAvailability::kUnavailable;
          break;
        case IoSampleValidity::kValid:
          if (signal.descriptor.freshness.mode == IoFreshnessMode::kProviderValidity) {
            availability = IoSignalAvailability::kFresh;
            break;
          }
          availability =
              now < signal.latest->received_at_ns ? IoSignalAvailability::kInvalid
              : now - signal.latest->received_at_ns > *signal.descriptor.freshness.maximum_age_ns
                  ? IoSignalAvailability::kStale
                  : IoSignalAvailability::kFresh;
          break;
      }
    }
    return availability;
  }

  IoSignalObservationDTO Observation(const Signal& signal, TimePoint now) const {
    const auto& source = sources.at(signal.descriptor.source_id);
    return {signal.descriptor,
            Availability(signal, now),
            signal.latest,
            source.incarnation,
            Cursor(signal.descriptor.source_id, source),
            source.loss_generation,
            source.error,
            now};
  }

  bool ScopeValid(const IoWatchScopeDTO& scope) const {
    return text_valid(scope.program_runner_owner_epoch.value(), limits.maximum_identity_bytes) &&
           text_valid(scope.program_run_id.value(), limits.maximum_identity_bytes) &&
           valid_program_node_execution_scope(scope.node) &&
           scope.node.node_instance_id.value().size() <= limits.maximum_identity_bytes &&
           scope.node.node_attempt_id.size() <= limits.maximum_identity_bytes;
  }

  bool TokenCurrent(const IoWatchRegistrationDTO& registration) const {
    return registration.owner_epoch == owner_epoch &&
           registration.catalog_generation == generation && registration.registration_id != 0U &&
           registration.registration_id <= last_watch_id;
  }

  bool RuleScopeValid(const IoRuleScopeDTO& scope) const {
    const auto text = [&](const auto& id) {
      return text_valid(id.value(), limits.maximum_identity_bytes);
    };
    if (const auto* operation = std::get_if<IoOperationRuleScopeDTO>(&scope)) {
      return text(operation->operation_id) && operation->operation_revision.nonzero() &&
             text(operation->participant_id) && text(operation->controllable_component_id) &&
             text(operation->command_target_id) && text(operation->controller_goal_id) &&
             operation->controller_goal_generation.nonzero() && operation->preparation_id != 0 &&
             text(operation->prepared_contract_digest) &&
             text(operation->motion_interrupt_scope_id);
    }
    const auto& activation = std::get<IoActivationRuleScopeDTO>(scope);
    return text(activation.retained_bundle_id) && text(activation.graph_revision);
  }

  bool RuleTokenCurrent(const IoRuleRegistrationDTO& registration) const {
    return registration.owner_epoch == owner_epoch &&
           registration.catalog_generation == generation && registration.registration_id != 0 &&
           registration.registration_id <= last_watch_id;
  }

  bool HasRuleLoss(const Rule& rule) const {
    return std::any_of(rule.losses.begin(), rule.losses.end(),
                       [](const auto& loss) { return loss.has_value(); });
  }

  std::optional<IoBlackboardSourceLossEvidenceDTO> BlackboardLoss(
      const Source& source, std::optional<std::string> key = std::nullopt) const {
    if (!source.blackboard_snapshot) return std::nullopt;
    const auto& snapshot = *source.blackboard_snapshot;
    return IoBlackboardSourceLossEvidenceDTO{snapshot.scope, snapshot.source_id,
                                             snapshot.incarnation, snapshot.message_sequence,
                                             std::move(key)};
  }

  void RecordRuleSourceLoss(const InterruptSourceId& id, const Source& source,
                            const IoSourceLossDTO& loss, TimePoint now) {
    for (auto& [registration_id, registration] : watches) {
      (void)registration_id;
      auto* rule = std::get_if<Rule>(&registration);
      if (!rule) continue;
      const auto required =
          std::lower_bound(rule->required_sources.begin(), rule->required_sources.end(), id);
      if (required == rule->required_sources.end() || *required != id) continue;
      auto& evidence =
          rule->losses[static_cast<std::size_t>(required - rule->required_sources.begin())];
      if (!evidence)
        evidence = IoRuleSourceLossEvidenceDTO{rule->registration,
                                               id,
                                               *source.incarnation,
                                               Cursor(id, source),
                                               IoRuleSourceFailureKind::kContinuityLoss,
                                               loss,
                                               std::nullopt,
                                               now,
                                               BlackboardLoss(source)};
    }
  }

  bool QualifyRule(Rule& rule, TimePoint now) {
    bool qualified = true;
    std::size_t index = 0;
    for (const auto& input : rule.condition->inputs()) {
      if (const auto& captured = rule.blackboard_inputs[index]) {
        const auto& source = sources.at(captured->source_id);
        const bool current = source.blackboard_binding && source.available &&
                             source.incarnation == captured->incarnation &&
                             source.blackboard_snapshot;
        const auto* value =
            current ? &source.blackboard_snapshot->values[captured->value_index] : nullptr;
        rule.borrowed_values[index++] = value && value->value ? &*value->value : nullptr;
        if (value && value->value) continue;
        qualified = false;
        const auto required = std::lower_bound(rule.required_sources.begin(),
                                               rule.required_sources.end(), captured->source_id);
        auto& loss =
            rule.losses[static_cast<std::size_t>(required - rule.required_sources.begin())];
        if (!loss)
          loss = IoRuleSourceLossEvidenceDTO{
              rule.registration,
              captured->source_id,
              captured->incarnation,
              Cursor(captured->source_id, source),
              IoRuleSourceFailureKind::kUnavailable,
              std::nullopt,
              std::nullopt,
              now,
              source.blackboard_snapshot &&
                      source.blackboard_snapshot->incarnation == captured->incarnation
                  ? BlackboardLoss(source, std::get<IoConditionBlackboardKeyDTO>(input.source).name)
                  : std::nullopt};
        continue;
      }
      const auto& signal = signals.at(std::get<IoSignalId>(input.source));
      const auto availability = Availability(signal, now);
      if (availability == IoSignalAvailability::kFresh) {
        rule.values[index] = *signal.latest->publication.value;
        rule.borrowed_values[index] = &rule.values[index];
        ++index;
        continue;
      }
      ++index;
      qualified = false;
      const auto& id = signal.descriptor.source_id;
      const auto& source = sources.at(id);
      const auto position =
          std::lower_bound(rule.required_sources.begin(), rule.required_sources.end(), id);
      auto& evidence =
          rule.losses[static_cast<std::size_t>(position - rule.required_sources.begin())];
      if (evidence) continue;
      const auto kind =
          availability == IoSignalAvailability::kStale     ? IoRuleSourceFailureKind::kStale
          : availability == IoSignalAvailability::kInvalid ? IoRuleSourceFailureKind::kInvalid
          : availability == IoSignalAvailability::kMissing ? IoRuleSourceFailureKind::kMissing
                                                           : IoRuleSourceFailureKind::kUnavailable;
      evidence = IoRuleSourceLossEvidenceDTO{rule.registration,  id,   *source.incarnation,
                                             Cursor(id, source), kind, std::nullopt,
                                             signal.latest,      now};
    }
    return qualified;
  }

  void LatchRuleTrip(Rule& rule, TimePoint now,
                     const IoRuleAcquisitionKeyDTO* acquisition = nullptr) {
    if (rule.tripped) return;
    rule.trip.evaluated_at_ns = now;
    if (acquisition) rule.trip.triggering_acquisition = *acquisition;
    if (rule.trip.blackboard) rule.trip.blackboard->values.clear();
    std::size_t index = 0;
    for (const auto& input : rule.condition->inputs()) {
      if (const auto& captured = rule.blackboard_inputs[index++]) {
        const auto& snapshot = sources.at(captured->source_id).blackboard_snapshot;
        auto& evidence = *rule.trip.blackboard;
        evidence.scope = snapshot->scope;
        evidence.source_id = snapshot->source_id;
        evidence.incarnation = snapshot->incarnation;
        evidence.message_sequence = snapshot->message_sequence;
        evidence.trusted_received_at_ns = snapshot->trusted_received_at_ns;
        evidence.values.emplace_back(snapshot, &snapshot->values[captured->value_index]);
      } else {
        const auto& signal = signals.at(std::get<IoSignalId>(input.source));
        rule.trip.matched_samples.push_back(*signal.latest);
      }
    }
    if (rule.trip.blackboard)
      std::sort(rule.trip.blackboard->values.begin(), rule.trip.blackboard->values.end(),
                [](const auto& left, const auto& right) { return left->name < right->name; });
    for (const auto& id : rule.required_sources)
      rule.trip.source_cursors.push_back(Cursor(id, sources.at(id)));
    rule.tripped = true;
  }

  void EvaluateRegisteredRules(TimePoint now, bool evaluate_predicate,
                               const IoRuleAcquisitionKeyDTO* acquisition = nullptr) {
    for (auto& [registration_id, registration] : watches) {
      (void)registration_id;
      auto* rule = std::get_if<Rule>(&registration);
      if (!rule) continue;
      rule->current_value.reset();
      rule->current_evaluated_at_ns = now;
      const bool qualified = QualifyRule(*rule, now);
      if (!evaluate_predicate || !qualified || HasRuleLoss(*rule) || rule->evaluation_error)
        continue;
      const auto evaluated =
          EvaluateIoCondition(*rule->condition, rule->borrowed_values, rule->workspace);
      if (!evaluated.ok()) {
        rule->evaluation_error = evaluated.error();
        continue;
      }
      rule->current_value = evaluated.value();
      if (evaluated.value() && !rule->previous_value) LatchRuleTrip(*rule, now, acquisition);
      rule->previous_value = evaluated.value();
    }
  }

  Result<void> Validate(const IoSourceAcquisitionDTO& acquisition) const {
    const auto found = sources.find(acquisition.source_id);
    if (found == sources.end()) return Result<void>::Err(invalid("io_source_unknown"));
    const auto& source = found->second;
    if (source.blackboard_source || source.core_held)
      return Result<void>::Err(invalid("io_source_kind_mismatch"));
    if (!source.incarnation || *source.incarnation != acquisition.incarnation)
      return Result<void>::Err(stale("io_source_incarnation_stale"));
    if (acquisition.source_sequence == 0U ||
        acquisition.source_sequence <= source.last_source_sequence)
      return Result<void>::Err(stale("io_source_sequence_stale"));
    if (acquisition.updates.empty() || acquisition.updates.size() > limits.maximum_signals)
      return Result<void>::Err(invalid("io_acquisition_bounds_invalid"));
    for (std::size_t index = 0; index < acquisition.updates.size(); ++index) {
      const auto& update = acquisition.updates[index];
      const auto signal = signals.find(update.signal_id);
      if (signal == signals.end()) return Result<void>::Err(invalid("io_signal_unknown"));
      if (update.source != IoSignalSampleSource::kDevice &&
          update.source != IoSignalSampleSource::kManyforgeWrite &&
          update.source != IoSignalSampleSource::kOutside &&
          update.source != IoSignalSampleSource::kLastWrite)
        return Result<void>::Err(invalid("io_sample_shape_invalid"));
      if (signal->second.descriptor.source_id != acquisition.source_id)
        return Result<void>::Err(invalid("io_signal_source_mismatch"));
      for (std::size_t prior = 0; prior < index; ++prior)
        if (acquisition.updates[prior].signal_id == update.signal_id)
          return Result<void>::Err(invalid("io_acquisition_duplicate_signal"));
      if ((update.validity != IoSampleValidity::kValid &&
           update.validity != IoSampleValidity::kInvalid &&
           update.validity != IoSampleValidity::kUnavailable) ||
          (update.validity == IoSampleValidity::kValid) != update.value.has_value() ||
          (update.validity == IoSampleValidity::kValid && update.error) ||
          (update.error && !error_valid(*update.error, limits.maximum_error_bytes)))
        return Result<void>::Err(invalid("io_sample_shape_invalid"));
    }
    const auto now = clock.now();
    if (now < 0 || acquisition.trusted_received_at_ns < 0 ||
        acquisition.trusted_received_at_ns > now)
      return Result<void>::Err(invalid("io_receiver_timestamp_invalid"));
    if (source.last_received_at_ns &&
        acquisition.trusted_received_at_ns < *source.last_received_at_ns)
      return Result<void>::Err(stale("io_receiver_timestamp_regressed"));
    const bool gap = source.last_source_sequence != 0U &&
                     acquisition.source_sequence - source.last_source_sequence != 1U;
    if (!CanRecord(source, gap ? 2U : 1U) || (gap && source.loss_generation == UINT64_MAX))
      return Result<void>::Err(exhausted());
    return Result<void>::Ok();
  }

  Result<void> LoseSource(const InterruptSourceId& id, const IoSourceIncarnation& incarnation,
                          IoSourceLossReason reason, const std::optional<ErrorDTO>& error,
                          std::optional<std::uint64_t> expected_source_sequence = std::nullopt,
                          std::optional<IoIngressLossBarrierDTO> barrier = std::nullopt,
                          bool retiring_blackboard = false) {
    const auto found = sources.find(id);
    if (found == sources.end()) return Result<void>::Err(invalid("io_source_unknown"));
    auto& source = found->second;
    if (source.core_held) return Result<void>::Err(invalid("io_source_kind_mismatch"));
    if (!source.incarnation || *source.incarnation != incarnation)
      return Result<void>::Err(stale("io_source_incarnation_stale"));
    if (expected_source_sequence && *expected_source_sequence != source.last_source_sequence)
      return Result<void>::Err(stale("io_source_availability_sequence_stale"));
    if (error && !error_valid(*error, limits.maximum_error_bytes))
      return Result<void>::Err(invalid("io_source_error_invalid"));
    if (barrier && (barrier->first_received_at_ns < 0 ||
                    barrier->last_received_at_ns < barrier->first_received_at_ns ||
                    barrier->last_received_at_ns > clock.now() ||
                    (!barrier->middleware_message_lost && !barrier->host_ingress_rejected &&
                     !barrier->sequence_exhausted && !barrier->disconnected) ||
                    (barrier->middleware_reported_lost_messages &&
                     (!barrier->middleware_message_lost || barrier->coalesced))))
      return Result<void>::Err(invalid("io_ingress_loss_invalid"));
    if (!CanRecord(source, 1U) || source.loss_generation == UINT64_MAX)
      return Result<void>::Err(exhausted());
    auto retired_scope =
        retiring_blackboard && source.blackboard_binding
            ? std::make_shared<const IoProgramBlackboardScopeDTO>(source.blackboard_binding->scope)
            : std::shared_ptr<const IoProgramBlackboardScopeDTO>{};
    ++source.loss_generation;
    source.available = false;
    if (source.blackboard_source) source.blackboard_continuity_lost = true;
    source.error = error;
    Append(id, source, std::nullopt,
           IoSourceLossDTO{reason, source.last_source_sequence, std::nullopt, std::nullopt, error,
                           std::move(barrier)},
           {}, std::move(retired_scope));
    return Result<void>::Ok();
  }

  Result<void> ValidateBlackboardBinding(const IoProgramBlackboardSourceBindingDTO& binding) const {
    const auto& scope = binding.scope;
    const auto text = [&](const auto& id) {
      return text_valid(id.value(), limits.maximum_identity_bytes);
    };
    if (scope.request_owner_epoch != owner_epoch || !text(scope.request_owner_epoch) ||
        !text(scope.program_runner_owner_epoch) || !text(scope.program_binding_id) ||
        !scope.active_publication_generation.nonzero() ||
        scope.claim_generation.has_value() != scope.program_run_id.has_value() ||
        (scope.claim_generation && !scope.claim_generation->nonzero()) ||
        (scope.program_run_id && !text(*scope.program_run_id)) || !text(binding.source_id) ||
        !text(binding.incarnation) ||
        binding.keys.size() > limits.maximum_signals - signals.size())
      return Result<void>::Err(invalid("program_bb_binding_invalid"));
    for (const auto& key : binding.keys)
      if (!text_valid(key.name, limits.maximum_identity_bytes))
        return Result<void>::Err(invalid("program_bb_key_invalid"));
    // Reuse the one typed shape/type/byte-capacity validator. This temporary
    // unknown baseline is validation only and is never installed as actual data.
    IoProgramBlackboardPublicationDTO shape{scope, IoBlackboardPublicationKind::kBaseline, 1U, 0U, {},
                                            0};
    shape.values.reserve(binding.keys.size());
    for (const auto& key : binding.keys)
      shape.values.push_back({key.name, key.value_kind, std::nullopt});
    const auto checked = detail::ValidateAndBuildProgramBlackboardSnapshot(binding, {}, shape);
    if (!checked.ok()) return Result<void>::Err(checked.error());
    return Result<void>::Ok();
  }

  RequestOwnerEpoch owner_epoch;
  std::uint64_t generation;
  IoSignalLimitsDTO limits;
  IClock& clock;
  mutable std::mutex mutex;
  std::map<IoSignalId, Signal> signals;
  std::map<InterruptSourceId, Source> sources;
  std::map<std::uint64_t, Registration> watches;
  std::uint64_t last_watch_id{};
  /// What `signal_samples` last showed of each signal: value, source and
  /// freshness (CONTRACTS §7 `SignalSample` without its age).
  using PublishedSample =
      std::tuple<std::optional<std::variant<bool, double>>, IoSignalSampleSource, bool>;
  std::map<IoSignalId, PublishedSample> published_samples;

  /// Raises the event-stream change signal when a sample's value, source or
  /// freshness changed since the last call; the age alone never does.
  void NotifySampleChanges() {
    const auto now = clock.now();
    bool changed = false;
    for (const auto& [id, signal] : signals) {
      PublishedSample current{std::nullopt, IoSignalSampleSource::kDevice,
                              Availability(signal, now) == IoSignalAvailability::kFresh};
      if (signal.latest) {
        if (signal.latest->publication.value)
          std::get<0>(current) = *signal.latest->publication.value;
        std::get<1>(current) = signal.latest->publication.source;
      }
      auto [row, inserted] = published_samples.try_emplace(id, current);
      if (inserted || row->second != current) {
        row->second = std::move(current);
        changed = true;
      }
    }
    if (changed) execution_event_change_signal().Notify();
  }
};

Result<std::unique_ptr<IoSignalSubsystem>> IoSignalSubsystem::Create(IoSignalCatalogDTO catalog,
                                                                     IoSignalLimitsDTO limits,
                                                                     IClock& clock) {
  using Reply = Result<std::unique_ptr<IoSignalSubsystem>>;
  if (limits.maximum_sources == 0U || limits.maximum_signals == 0U ||
      limits.records_per_source == 0U || limits.maximum_watches == 0U ||
      limits.maximum_identity_bytes == 0U || limits.maximum_error_bytes == 0U ||
      catalog.generation == 0U ||
      !text_valid(catalog.owner_epoch.value(), limits.maximum_identity_bytes) ||
      catalog.signals.size() > limits.maximum_signals ||
      catalog.authoritative_synthetic_sources.size() > limits.maximum_sources)
    return Reply::Err(invalid("io_catalog_bounds_invalid"));
  std::set<InterruptSourceId> synthetic_sources;
  for (const auto& source : catalog.authoritative_synthetic_sources) {
    if (!synthetic_sources.insert(source).second)
      return Reply::Err(invalid("io_synthetic_source_capability_invalid"));
  }
  std::map<IoSignalId, bool> identities;
  std::map<InterruptSourceId, bool> sources;
  for (const auto& signal : catalog.signals) {
    // Only outputs carry controller ownership and read-back; each core-held
    // `changed_outside` signal counts against the bounds like any signal.
    if (signal.direction == IoSignalDirection::kInput &&
        (signal.controller_owned || signal.readback))
      return Reply::Err(invalid("io_signal_descriptor_invalid"));
    if (HoldsChangedOutside(signal) &&
        (!identities.emplace(ChangedOutsideSignalId(signal.signal_id), true).second ||
         !sources.emplace(ChangedOutsideSourceId(signal.signal_id), true).second))
      return Reply::Err(invalid("io_signal_identity_invalid"));
  }
  if (identities.size() + catalog.signals.size() > limits.maximum_signals)
    return Reply::Err(invalid("io_catalog_bounds_invalid"));
  for (const auto& signal : catalog.signals) {
    if (!text_valid(signal.signal_id.controllable_component_id.value(), limits.maximum_identity_bytes) ||
        !text_valid(signal.signal_id.name, limits.maximum_identity_bytes) ||
        !text_valid(signal.source_id.value(), limits.maximum_identity_bytes) ||
        !identities.emplace(signal.signal_id, true).second)
      return Reply::Err(invalid("io_signal_identity_invalid"));
    sources.emplace(signal.source_id, true);
    const bool synthetic = synthetic_sources.contains(signal.source_id);
    if (signal.value_type != IoSignalValueType::kBoolean)
      return Reply::Err({"unsupported_capability",
                         "I/O signal contract is not implemented",
                         {{"reason", "io_signal_contract_unsupported"}}});
    // Reuse the sealed descriptor validator: every input declares its actual
    // liveness mode. Change-only receipt evidence has no age deadline; only
    // installed synthetic capability can authorize in-process validity.
    const bool synthetic_contract = signal.direction == IoSignalDirection::kInput &&
                                    signal.liveness == IoSignalLivenessMode::kInProcess;
    if (!ComputeIoSignalDescriptorDigest(signal).ok() || synthetic != synthetic_contract ||
        (synthetic && signal.update_mode != IoSignalUpdateMode::kChangeOnly) ||
        (signal.direction != IoSignalDirection::kInput &&
         signal.direction != IoSignalDirection::kOutput) ||
        (signal.update_mode != IoSignalUpdateMode::kSampled &&
         signal.update_mode != IoSignalUpdateMode::kChangeOnly) ||
        (signal.transition_capability != IoTransitionCapability::kSampledOnly &&
         signal.transition_capability != IoTransitionCapability::kCaptured))
      return Reply::Err(invalid("io_signal_descriptor_invalid"));
    if (signal.direction == IoSignalDirection::kInput) {
      if (signal.output_resource) return Reply::Err(invalid("io_input_has_output_contract"));
    } else if (!signal.output_resource ||
               !text_valid(signal.output_resource->value(), limits.maximum_identity_bytes)) {
      return Reply::Err(invalid("io_output_contract_invalid"));
    }
  }
  for (const auto& source : synthetic_sources)
    if (!sources.contains(source))
      return Reply::Err(invalid("io_synthetic_source_capability_unknown"));
  if (sources.size() > limits.maximum_sources)
    return Reply::Err(invalid("io_source_bound_exceeded"));
  for (const auto& [id, depth] : limits.records_per_source_overrides) {
    if (depth == 0U || !sources.contains(id))
      return Reply::Err(invalid("io_source_record_bound_invalid"));
  }
  return Reply::Ok(std::unique_ptr<IoSignalSubsystem>(
      new IoSignalSubsystem(std::make_unique<State>(std::move(catalog), limits, clock))));
}

IoSignalSubsystem::IoSignalSubsystem(std::unique_ptr<State> state) : state_(std::move(state)) {}
IoSignalSubsystem::~IoSignalSubsystem() = default;

Result<void> IoSignalSubsystem::InstallProgramBlackboardSource(
    const IoProgramBlackboardSourceBindingDTO& binding,
    const std::optional<IoSourceIncarnation>& expected_current_incarnation) {
  std::lock_guard lock(state_->mutex);
  const auto valid = state_->ValidateBlackboardBinding(binding);
  if (!valid.ok()) return valid;
  auto found = state_->sources.find(binding.source_id);
  if (found != state_->sources.end()) {
    auto& source = found->second;
    if (!source.blackboard_source) return Result<void>::Err(invalid("program_bb_source_collision"));
    if (source.blackboard_binding == binding) return Result<void>::Ok();
    if (source.incarnation != expected_current_incarnation ||
        source.incarnation == binding.incarnation)
      return Result<void>::Err(stale("program_bb_install_predecessor_stale"));
    if (!state_->CanRecord(source, source.blackboard_binding ? 2U : 1U) ||
        (source.blackboard_binding && source.loss_generation == UINT64_MAX))
      return Result<void>::Err(exhausted());
    if (source.blackboard_binding) {
      const auto lost = state_->LoseSource(binding.source_id, *source.incarnation,
                                           IoSourceLossReason::kIncarnationChanged, std::nullopt,
                                           std::nullopt, std::nullopt, true);
      if (!lost.ok()) return lost;
    }
  } else {
    if (expected_current_incarnation)
      return Result<void>::Err(stale("program_bb_install_predecessor_stale"));
    if (state_->sources.size() == state_->limits.maximum_sources)
      return Result<void>::Err(invalid("program_bb_source_bound_exceeded"));
    // One existing source slot owns this Program's blackboard occurrence;
    // replacement reuses it rather than accumulating retired source owners.
    if (std::any_of(state_->sources.begin(), state_->sources.end(),
                    [](const auto& entry) { return entry.second.blackboard_source; }))
      return Result<void>::Err(invalid("program_bb_source_collision"));
    found = state_->sources.try_emplace(binding.source_id).first;
    // BB history has one fixed bounded window, independent of physical source
    // ingress configuration. Eviction removes proof; it never implies supersession.
    found->second.record_capacity = 64U;
  }
  auto& source = found->second;
  source.blackboard_source = true;
  source.blackboard_continuity_lost = false;
  source.incarnation = binding.incarnation;
  source.blackboard_binding = binding;
  source.blackboard_snapshot.reset();
  source.last_source_sequence = 0U;
  source.last_received_at_ns.reset();
  source.available = false;  // Actual baseline is still required.
  source.error.reset();
  state_->Append(binding.source_id, source, std::nullopt, std::nullopt);
  return Result<void>::Ok();
}

Result<IoProgramBlackboardAcknowledgementDTO> IoSignalSubsystem::PublishProgramBlackboardBaseline(
    const IoProgramBlackboardSourceBindingDTO& binding,
    const std::optional<IoSourceIncarnation>& expected_current_incarnation,
    const IoProgramBlackboardPublicationDTO& publication) {
  using Reply = Result<IoProgramBlackboardAcknowledgementDTO>;
  std::lock_guard lock(state_->mutex);
  const auto valid = state_->ValidateBlackboardBinding(binding);
  if (!valid.ok()) return Reply::Err(valid.error());
  if (publication.kind != IoBlackboardPublicationKind::kBaseline)
    return Reply::Err(invalid("program_bb_baseline_required"));
  const auto now = state_->clock.now();
  if (now < 0 || publication.trusted_received_at_ns < 0 || publication.trusted_received_at_ns > now)
    return Reply::Err(invalid("io_receiver_timestamp_invalid"));
  const auto snapshot = detail::ValidateAndBuildProgramBlackboardSnapshot(binding, {}, publication);
  if (!snapshot.ok()) return Reply::Err(snapshot.error());
  auto found = state_->sources.find(binding.source_id);
  bool continuing = false;
  std::shared_ptr<const IoProgramBlackboardScopeDTO> retired_scope;
  State::Source candidate;
  if (found != state_->sources.end()) {
    const auto& old = found->second;
    if (!old.blackboard_source) return Reply::Err(invalid("program_bb_source_collision"));
    if (old.incarnation != expected_current_incarnation)
      return Reply::Err(stale("program_bb_install_predecessor_stale"));
    if (old.blackboard_binding == binding && old.blackboard_snapshot)
      return Reply::Err(stale("program_bb_sequence_stale"));
    if (old.blackboard_snapshot && old.incarnation == binding.incarnation)
      return Reply::Err(stale("program_bb_install_predecessor_stale"));
    if (old.last_received_at_ns && publication.trusted_received_at_ns < *old.last_received_at_ns)
      return Reply::Err(stale("io_receiver_timestamp_regressed"));
    if (!state_->CanRecord(old, 1U)) return Reply::Err(exhausted());
    if (old.blackboard_binding) {
      const auto& prior = *old.blackboard_binding;
      const auto& a = prior.scope;
      const auto& b = binding.scope;
      continuing = a.request_owner_epoch == b.request_owner_epoch &&
          a.program_runner_owner_epoch == b.program_runner_owner_epoch &&
          a.program_binding_id == b.program_binding_id &&
          a.active_publication_generation == b.active_publication_generation &&
          prior.keys == binding.keys && prior.maximum_value_bytes == binding.maximum_value_bytes;
      // This is only the qualified same-owner Run/Reset handoff. Actual owner,
      // Program or publication changes must first use explicit teardown.
      if (!continuing) return Reply::Err(stale("program_bb_handoff_context_stale"));
      // A fully qualified new epoch can restore current source knowledge.
      // Existing registrations retain their immutable loss and cannot become
      // qualified again; the latched owner needs its fresh observer/recovery.
      if (old.blackboard_snapshot)
        retired_scope = std::make_shared<const IoProgramBlackboardScopeDTO>(a);
    }
    candidate = old;  // Bounded existing history; snapshots remain shared.
  } else {
    if (expected_current_incarnation)
      return Reply::Err(stale("program_bb_install_predecessor_stale"));
    if (state_->sources.size() == state_->limits.maximum_sources)
      return Reply::Err(invalid("program_bb_source_bound_exceeded"));
    if (std::any_of(state_->sources.begin(), state_->sources.end(),
                   [](const auto& row) { return row.second.blackboard_source; }))
      return Reply::Err(invalid("program_bb_source_collision"));
    candidate.record_capacity = 64U;
  }
  candidate.blackboard_source = true;
  candidate.blackboard_continuity_lost = false;
  candidate.incarnation = binding.incarnation;
  candidate.blackboard_binding = binding;
  candidate.blackboard_snapshot = snapshot.value();
  candidate.last_source_sequence = publication.message_sequence;
  candidate.last_received_at_ns = publication.trusted_received_at_ns;
  candidate.available = true;
  candidate.error.reset();
  // Prepare the accepted frame and exact prior-scope proof before replacing
  // the current source. This record has no manufactured loss or cursor.
  state_->Append(binding.source_id, candidate, std::nullopt, std::nullopt,
                  snapshot.value(), std::move(retired_scope));
  std::vector<std::pair<State::BlackboardInput*, IoSourceIncarnation>> rebindings;
  if (continuing) {
    for (auto& [id, registration] : state_->watches) {
      (void)id;
      auto* rule = std::get_if<State::Rule>(&registration);
      if (!rule || !std::holds_alternative<IoActivationRuleScopeDTO>(rule->registration.scope))
        continue;
      for (auto& input : rule->blackboard_inputs)
        if (input && expected_current_incarnation && input->source_id == binding.source_id &&
            input->incarnation == *expected_current_incarnation)
          rebindings.emplace_back(&*input, binding.incarnation);
    }
  }
  // Observe any genuine old loss/other-source expiry before the replacement.
  // Existing trip/loss evidence and previous predicate level are never reset.
  state_->EvaluateRegisteredRules(now, false);
  if (found == state_->sources.end())
    found = state_->sources.emplace(binding.source_id, std::move(candidate)).first;
  else
    found->second = std::move(candidate);
  for (auto& [input, incarnation] : rebindings) input->incarnation = std::move(incarnation);
  const auto& source = found->second;
  const IoRuleAcquisitionKeyDTO acquired{binding.source_id, binding.incarnation,
      publication.message_sequence, state_->Cursor(binding.source_id, source)};
  state_->EvaluateRegisteredRules(now, true, &acquired);
  return Reply::Ok({binding.scope.request_owner_epoch, binding.scope.program_binding_id,
      binding.scope.cycle_generation, binding.scope.program_run_id, binding.incarnation,
      publication.message_sequence, binding.scope.active_publication_generation,
      IoProgramBlackboardAcknowledgementStatus::kAccepted});
}

Result<IoProgramBlackboardAcknowledgementDTO> IoSignalSubsystem::PublishProgramBlackboard(
    const IoProgramBlackboardPublicationDTO& publication) {
  using Reply = Result<IoProgramBlackboardAcknowledgementDTO>;
  std::lock_guard lock(state_->mutex);
  auto found = std::find_if(state_->sources.begin(), state_->sources.end(), [](const auto& entry) {
    return entry.second.blackboard_binding.has_value();
  });
  if (found == state_->sources.end()) return Reply::Err(stale("program_bb_source_not_installed"));
  auto& source = found->second;
  const auto& binding = *source.blackboard_binding;
  if (publication.scope != binding.scope) return Reply::Err(stale("program_bb_scope_stale"));
  const auto now = state_->clock.now();
  if (now < 0 || publication.trusted_received_at_ns < 0 || publication.trusted_received_at_ns > now)
    return Reply::Err(invalid("io_receiver_timestamp_invalid"));
  if (source.last_received_at_ns &&
      publication.trusted_received_at_ns < *source.last_received_at_ns)
    return Reply::Err(stale("io_receiver_timestamp_regressed"));
  if (publication.message_sequence <= source.last_source_sequence)
    return Reply::Err(stale("program_bb_sequence_stale"));
  // Sequence loss belongs to this exact installed sender, never to an obsolete
  // frame. Do not apply a partial update to knowledge that lost continuity.
  if (source.last_source_sequence == UINT64_MAX ||
      publication.message_sequence != source.last_source_sequence + 1U ||
      publication.previous_sequence != source.last_source_sequence) {
    const auto lost = state_->LoseSource(found->first, *source.incarnation,
                                         IoSourceLossReason::kSequenceGap, std::nullopt);
    if (!lost.ok()) return Reply::Err(lost.error());
    return Reply::Err(stale("program_bb_sequence_gap"));
  }
  if (source.blackboard_continuity_lost) return Reply::Err(stale("program_bb_source_unavailable"));
  if (!state_->CanRecord(source, 1U)) return Reply::Err(exhausted());
  const auto snapshot = detail::ValidateAndBuildProgramBlackboardSnapshot(
      binding, source.blackboard_snapshot, publication);
  if (!snapshot.ok()) return Reply::Err(snapshot.error());
  state_->EvaluateRegisteredRules(now, false);
  source.blackboard_snapshot = snapshot.value();
  source.last_source_sequence = publication.message_sequence;
  source.last_received_at_ns = publication.trusted_received_at_ns;
  source.available = true;
  source.error.reset();
  state_->Append(found->first, source, std::nullopt, std::nullopt, snapshot.value());
  const IoRuleAcquisitionKeyDTO acquired{found->first, *source.incarnation,
                                         publication.message_sequence,
                                         state_->Cursor(found->first, source)};
  state_->EvaluateRegisteredRules(now, true, &acquired);
  return Reply::Ok({binding.scope.request_owner_epoch, binding.scope.program_binding_id,
                    binding.scope.cycle_generation, binding.scope.program_run_id,
                    binding.incarnation, source.last_source_sequence,
                    binding.scope.active_publication_generation,
                    IoProgramBlackboardAcknowledgementStatus::kAccepted});
}

Result<void> IoSignalSubsystem::RetireProgramBlackboardSource(
    const IoProgramBlackboardSourceBindingDTO& binding) {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->sources.find(binding.source_id);
  if (found == state_->sources.end() || found->second.blackboard_binding != binding)
    return Result<void>::Err(stale("program_bb_scope_stale"));
  auto& source = found->second;
  // Exact owner retirement invalidates knowledge even if its history append
  // cannot be retained. Set these first, before any allocating diagnostics.
  source.available = false;
  source.blackboard_continuity_lost = true;
  const auto lost =
      state_->LoseSource(binding.source_id, binding.incarnation, IoSourceLossReason::kDisconnected,
                         std::nullopt, std::nullopt, std::nullopt, true);
  if (!lost.ok()) {
    source.error = lost.error();
    // This is an observed loss, not a recorded retirement. Preserve the last
    // actual cursor and never mint superseded proof from the failed append.
    state_->RecordRuleSourceLoss(
        binding.source_id, source,
        IoSourceLossDTO{IoSourceLossReason::kDisconnected, source.last_source_sequence,
                        std::nullopt, std::nullopt, source.error, std::nullopt},
        state_->clock.now());
    return lost;
  }
  source.blackboard_binding.reset();
  return Result<void>::Ok();
}

Result<bool> IoSignalSubsystem::WasProgramBlackboardScopeRetired(
    const IoProgramBlackboardScopeDTO& scope) const {
  std::lock_guard lock(state_->mutex);
  const auto found =
      std::find_if(state_->sources.begin(), state_->sources.end(),
                   [](const auto& row) { return row.second.blackboard_binding.has_value(); });
  if (found == state_->sources.end()) return Result<bool>::Ok(false);
  const auto& source = found->second;
  if (!source.available || !source.blackboard_snapshot || source.blackboard_binding->scope == scope)
    return Result<bool>::Ok(false);
  return Result<bool>::Ok(
      std::any_of(source.records.begin(), source.records.end(), [&](const auto& record) {
        return record.retired_blackboard_scope && *record.retired_blackboard_scope == scope;
      }));
}

Result<void> IoSignalSubsystem::InstallSource(
    const InterruptSourceId& id, const IoSourceIncarnation& incarnation,
    const std::optional<IoSourceIncarnation>& expected_current_incarnation) {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->sources.find(id);
  if (found == state_->sources.end()) return Result<void>::Err(invalid("io_source_unknown"));
  if (!text_valid(incarnation.value(), state_->limits.maximum_identity_bytes))
    return Result<void>::Err(invalid("io_source_incarnation_invalid"));
  auto& source = found->second;
  if (source.blackboard_source || source.core_held)
    return Result<void>::Err(invalid("io_source_kind_mismatch"));
  if (source.incarnation == incarnation) return Result<void>::Ok();
  if (source.incarnation != expected_current_incarnation)
    return Result<void>::Err(stale("io_source_install_predecessor_stale"));
  if (!state_->CanRecord(source, 1U) || source.loss_generation == UINT64_MAX)
    return Result<void>::Err(exhausted());
  const bool replacing = source.incarnation.has_value();
  const auto old_sequence = source.last_source_sequence;
  source.incarnation = incarnation;
  source.last_source_sequence = 0U;
  source.available = !replacing;
  source.error.reset();
  if (replacing) {
    ++source.loss_generation;
    for (auto& [signal_id, signal] : state_->signals) {
      (void)signal_id;
      if (signal.descriptor.source_id == id) signal.latest.reset();
    }
  }
  state_->Append(id, source, std::nullopt,
                 replacing ? std::optional(IoSourceLossDTO{IoSourceLossReason::kIncarnationChanged,
                                                           old_sequence, std::nullopt, std::nullopt,
                                                           std::nullopt, std::nullopt})
                           : std::nullopt);
  state_->NotifySampleChanges();
  return Result<void>::Ok();
}

Result<void> IoSignalSubsystem::ValidateAcquisition(
    const IoSourceAcquisitionDTO& acquisition) const {
  std::lock_guard lock(state_->mutex);
  return state_->Validate(acquisition);
}

Result<IoSourceCursorDTO> IoSignalSubsystem::Publish(const IoSourceAcquisitionDTO& acquisition) {
  using Reply = Result<IoSourceCursorDTO>;
  std::lock_guard lock(state_->mutex);
  const auto valid = state_->Validate(acquisition);
  if (!valid.ok()) return Reply::Err(valid.error());
  state_->EvaluateRegisteredRules(state_->clock.now(), false);
  const auto cursor = state_->Commit(acquisition);
  state_->NotifySampleChanges();
  return Reply::Ok(cursor);
}

Result<IoSignalIngressConfigurationDTO> IoSignalSubsystem::IngressConfiguration() const {
  std::lock_guard lock(state_->mutex);
  IoSignalIngressConfigurationDTO configuration{state_->limits, {}};
  for (const auto& [id, source] : state_->sources) {
    if (source.blackboard_source || source.core_held) continue;
    if (!source.incarnation)
      return Result<IoSignalIngressConfigurationDTO>::Err(invalid("io_source_not_installed"));
    configuration.sources.push_back({id, *source.incarnation, source.last_source_sequence,
                                     source.last_received_at_ns, source.record_capacity});
  }
  return Result<IoSignalIngressConfigurationDTO>::Ok(std::move(configuration));
}

Result<void> IoSignalSubsystem::ReportIngressLoss(const IoIngressLossBarrierDTO& barrier) {
  std::lock_guard lock(state_->mutex);
  // Typed loss evidence needs no generated error text or adapter error budget.
  // LoseSource fences incarnation before validating an obsolete event's shape.
  auto lost = state_->LoseSource(
      barrier.source_id, barrier.incarnation,
      barrier.disconnected ? IoSourceLossReason::kDisconnected : IoSourceLossReason::kIngressLoss,
      std::nullopt, std::nullopt, barrier);
  state_->NotifySampleChanges();
  return lost;
}

Result<std::vector<IoSignalObservationDTO>> IoSignalSubsystem::ReadSignals(
    std::span<const IoSignalId> signals) const {
  using Reply = Result<std::vector<IoSignalObservationDTO>>;
  std::lock_guard lock(state_->mutex);
  if (signals.size() > state_->limits.maximum_signals)
    return Reply::Err(invalid("io_snapshot_bounds_invalid"));
  for (const auto& id : signals)
    if (!state_->signals.contains(id)) return Reply::Err(invalid("io_signal_unknown"));
  std::vector<IoSignalObservationDTO> observations;
  observations.reserve(signals.size());
  const auto now = state_->clock.now();
  for (const auto& id : signals)
    observations.push_back(state_->Observation(state_->signals.at(id), now));
  return Reply::Ok(std::move(observations));
}

Result<void> IoSignalSubsystem::MarkSourceUnavailable(const InterruptSourceId& source,
                                                      const IoSourceIncarnation& incarnation,
                                                      std::uint64_t expected_source_sequence,
                                                      const ErrorDTO& error) {
  std::lock_guard lock(state_->mutex);
  auto lost = state_->LoseSource(source, incarnation, IoSourceLossReason::kDisconnected, error,
                                 expected_source_sequence);
  state_->NotifySampleChanges();
  return lost;
}

Result<void> IoSignalSubsystem::ReportIngressOverflow(const InterruptSourceId& source,
                                                      const IoSourceIncarnation& incarnation,
                                                      const ErrorDTO& error) {
  std::lock_guard lock(state_->mutex);
  auto lost = state_->LoseSource(source, incarnation, IoSourceLossReason::kIngressOverflow, error);
  state_->NotifySampleChanges();
  return lost;
}

Result<IoSignalRecordPageDTO> IoSignalSubsystem::ReadRecords(const IoSourceCursorDTO& after,
                                                             std::size_t maximum_records) const {
  using Reply = Result<IoSignalRecordPageDTO>;
  std::lock_guard lock(state_->mutex);
  if (!state_->CursorCurrent(after)) return Reply::Err(stale("io_cursor_generation_stale"));
  const auto found = state_->sources.find(after.source_id);
  if (found == state_->sources.end()) return Reply::Err(invalid("io_source_unknown"));
  const auto& source = found->second;
  if (maximum_records == 0U || maximum_records > source.record_capacity ||
      after.record_sequence > source.record_sequence)
    return Reply::Err(invalid("io_cursor_range_invalid"));
  IoSignalRecordPageDTO result{{}, after, state_->Cursor(after.source_id, source), false};
  result.retention_gap = !source.records.empty() &&
                         after.record_sequence < source.records.front().cursor.record_sequence - 1U;
  for (const auto& record : source.records) {
    if (record.cursor.record_sequence <= after.record_sequence) continue;
    result.records.push_back(record);
    result.next_cursor = record.cursor;
    if (result.records.size() == maximum_records) break;
  }
  return Reply::Ok(std::move(result));
}

Result<IoRuleServiceConfigurationDTO> IoSignalSubsystem::RuleServiceConfiguration() const {
  std::lock_guard lock(state_->mutex);
  return Result<IoRuleServiceConfigurationDTO>::Ok(
      {state_->owner_epoch, state_->generation, state_->limits.maximum_watches});
}

Result<IoRuleRegistrationDTO> IoSignalSubsystem::ArmRule(const IoRuleArmRequestDTO& request) {
  using Reply = Result<IoRuleRegistrationDTO>;
  if (!request.condition || request.condition->inputs().empty() ||
      request.condition->inputs().size() > state_->limits.maximum_signals ||
      request.required_sources.size() > state_->limits.maximum_sources ||
      !text_valid(request.policy_id.value(), state_->limits.maximum_identity_bytes) ||
      !state_->RuleScopeValid(request.scope))
    return Reply::Err(invalid("io_rule_request_invalid"));
  if (request.activation_mode != MotionInterruptActivationMode::kRequireInactiveAtArm &&
      request.activation_mode != MotionInterruptActivationMode::kFreshEdgeAfterArm &&
      request.activation_mode != MotionInterruptActivationMode::kTripIfAlreadyActive)
    return Reply::Err({"unsupported_capability",
                       "I/O rule activation mode is not implemented",
                       {{"reason", "io_rule_activation_unsupported"}}});
  if (!std::is_sorted(request.required_sources.begin(), request.required_sources.end()) ||
      std::adjacent_find(request.required_sources.begin(), request.required_sources.end()) !=
          request.required_sources.end())
    return Reply::Err(invalid("io_rule_required_sources_invalid"));

  // Allocate bounded evaluation/snapshot buffers at arm, outside acquisition.
  std::vector<IoConditionValueDTO> values(request.condition->inputs().size(),
                                          IoConditionValueDTO{false});
  std::vector<const IoConditionValueDTO*> borrowed_values(values.size());
  std::vector<std::optional<State::BlackboardInput>> blackboard_inputs(values.size());
  std::vector<std::uint8_t> workspace(request.condition->workspace_size());
  std::vector<std::optional<IoRuleSourceLossEvidenceDTO>> losses(request.required_sources.size());
  std::vector<IoSignalSampleDTO> samples;
  samples.reserve(values.size());
  std::vector<IoSourceCursorDTO> cursors;
  cursors.reserve(request.required_sources.size());
  auto required_sources = request.required_sources;
  std::vector<InterruptSourceId> referenced_sources;
  referenced_sources.reserve(values.size());

  std::lock_guard lock(state_->mutex);
  const auto now = state_->clock.now();
  std::size_t index = 0;
  for (const auto& input : request.condition->inputs()) {
    const auto* id = std::get_if<IoSignalId>(&input.source);
    if (!id) {
      const auto& key = std::get<IoConditionBlackboardKeyDTO>(input.source).name;
      auto source = std::find_if(
          state_->sources.begin(), state_->sources.end(),
          [](const auto& entry) { return entry.second.blackboard_binding.has_value(); });
      if (source == state_->sources.end() || !source->second.available ||
          !source->second.blackboard_snapshot)
        return Reply::Err({kErrorMotionInterruptArmFailed,
                           "Blackboard baseline is unavailable",
                           {{"reason", "io_rule_baseline_unavailable"}}});
      const auto& snapshot = *source->second.blackboard_snapshot;
      const auto value =
          std::lower_bound(snapshot.values.begin(), snapshot.values.end(), key,
                           [](const auto& row, const auto& name) { return row.name < name; });
      if (value == snapshot.values.end() || value->name != key ||
          value->value_kind != input.value_kind)
        return Reply::Err(invalid("io_rule_blackboard_key_mismatch"));
      if (!value->value)
        return Reply::Err({kErrorMotionInterruptArmFailed,
                           "Blackboard key is unavailable",
                           {{"reason", "io_rule_baseline_unavailable"}}});
      blackboard_inputs[index] =
          State::BlackboardInput{source->first, *source->second.incarnation,
                                 static_cast<std::size_t>(value - snapshot.values.begin())};
      borrowed_values[index++] = &*value->value;
      referenced_sources.push_back(source->first);
      continue;
    }
    const auto found = state_->signals.find(*id);
    if (found == state_->signals.end()) return Reply::Err(invalid("io_signal_unknown"));
    if (state_->Availability(found->second, now) != IoSignalAvailability::kFresh)
      return Reply::Err({kErrorMotionInterruptArmFailed,
                         "I/O rule requires a fresh known baseline",
                         {{"reason", "io_rule_baseline_unavailable"}}});
    values[index] = *found->second.latest->publication.value;
    borrowed_values[index] = &values[index];
    ++index;
    referenced_sources.push_back(found->second.descriptor.source_id);
  }
  std::sort(referenced_sources.begin(), referenced_sources.end());
  referenced_sources.erase(std::unique(referenced_sources.begin(), referenced_sources.end()),
                           referenced_sources.end());
  if (referenced_sources != required_sources)
    return Reply::Err(invalid("io_rule_required_sources_mismatch"));
  if (state_->watches.size() == state_->limits.maximum_watches)
    return Reply::Err({"execution_rejected",
                       "I/O registration bound reached",
                       {{"reason", "io_watch_bound_exceeded"}}});
  if (state_->last_watch_id == UINT64_MAX) return Reply::Err(exhausted());
  const auto baseline = EvaluateIoCondition(*request.condition, borrowed_values, workspace);
  if (!baseline.ok()) return Reply::Err(baseline.error());
  if (request.activation_mode == MotionInterruptActivationMode::kRequireInactiveAtArm &&
      baseline.value())
    return Reply::Err({kErrorMotionInterruptSourceActiveAtArm,
                       "I/O rule is active at arm",
                       {{"reason", "io_rule_active_at_arm"}}});
  IoRuleRegistrationDTO registration{state_->owner_epoch, state_->generation,
                                     ++state_->last_watch_id, request.policy_id, request.scope};
  State::Rule rule{registration,
                   request.condition,
                   request.activation_mode,
                   std::move(required_sources),
                   std::move(losses),
                   std::move(values),
                   std::move(borrowed_values),
                   std::move(blackboard_inputs),
                   std::move(workspace),
                   {registration, std::move(samples), std::move(cursors), now},
                   false,
                   baseline.value(),
                   std::nullopt,
                   baseline.value(),
                   now};
  for (const auto& captured : rule.blackboard_inputs) {
    if (!captured) continue;
    const auto& snapshot = *state_->sources.at(captured->source_id).blackboard_snapshot;
    rule.trip.blackboard = IoBlackboardTripEvidenceDTO{snapshot.scope,
                                                       snapshot.source_id,
                                                       snapshot.incarnation,
                                                       snapshot.message_sequence,
                                                       snapshot.trusted_received_at_ns,
                                                       {}};
    rule.trip.blackboard->values.reserve(static_cast<std::size_t>(
        std::count_if(rule.blackboard_inputs.begin(), rule.blackboard_inputs.end(),
                      [](const auto& input) { return input.has_value(); })));
    break;
  }
  if (request.activation_mode == MotionInterruptActivationMode::kTripIfAlreadyActive &&
      baseline.value())
    state_->LatchRuleTrip(rule, now);
  state_->watches.emplace(registration.registration_id, std::move(rule));
  return Reply::Ok(std::move(registration));
}

Result<IoRuleConditionSnapshotDTO> IoSignalSubsystem::ReadRuleConditionSnapshot(
    const IoRuleRegistrationDTO& registration) const {
  using Reply = Result<IoRuleConditionSnapshotDTO>;
  std::lock_guard lock(state_->mutex);
  if (!state_->RuleTokenCurrent(registration)) return Reply::Err(stale("io_rule_generation_stale"));
  const auto found = state_->watches.find(registration.registration_id);
  if (found == state_->watches.end()) return Reply::Err(stale("io_rule_registration_retired"));
  const auto* rule = std::get_if<State::Rule>(&found->second);
  if (!rule || rule->registration != registration) return Reply::Err(stale("io_rule_scope_stale"));
  return Reply::Ok({rule->current_value, rule->current_evaluated_at_ns});
}

Result<IoRuleEvidenceDTO> IoSignalSubsystem::ReadRuleEvidence(
    const IoRuleRegistrationDTO& registration) const {
  using Reply = Result<IoRuleEvidenceDTO>;
  std::lock_guard lock(state_->mutex);
  if (!state_->RuleTokenCurrent(registration)) return Reply::Err(stale("io_rule_generation_stale"));
  const auto found = state_->watches.find(registration.registration_id);
  if (found == state_->watches.end())
    return Reply::Ok({IoRuleRegistrationState::kRetired, std::nullopt, {}, std::nullopt});
  const auto* rule = std::get_if<State::Rule>(&found->second);
  if (!rule || rule->registration != registration) return Reply::Err(stale("io_rule_scope_stale"));
  IoRuleEvidenceDTO evidence{IoRuleRegistrationState::kArmed,
                             rule->tripped ? std::optional(rule->trip) : std::nullopt,
                             {},
                             rule->evaluation_error};
  for (const auto& loss : rule->losses)
    if (loss) evidence.source_losses.push_back(*loss);
  return Reply::Ok(std::move(evidence));
}

Result<IoRuleEvidenceDTO> IoSignalSubsystem::RetireRule(const IoRuleRegistrationDTO& registration) {
  using Reply = Result<IoRuleEvidenceDTO>;
  std::lock_guard lock(state_->mutex);
  if (!state_->RuleTokenCurrent(registration)) return Reply::Err(stale("io_rule_generation_stale"));
  const auto found = state_->watches.find(registration.registration_id);
  if (found == state_->watches.end())
    return Reply::Ok({IoRuleRegistrationState::kRetired, std::nullopt, {}, std::nullopt});
  auto* rule = std::get_if<State::Rule>(&found->second);
  if (!rule || rule->registration != registration) return Reply::Err(stale("io_rule_scope_stale"));
  // Capture only evidence acquired while this registration was eligible.
  // Teardown must not manufacture a later age failure after ordinary completion.
  IoRuleEvidenceDTO evidence{IoRuleRegistrationState::kRetired,
                             rule->tripped ? std::optional(rule->trip) : std::nullopt,
                             {},
                             rule->evaluation_error};
  for (const auto& loss : rule->losses)
    if (loss) evidence.source_losses.push_back(*loss);
  state_->watches.erase(found);
  return Reply::Ok(std::move(evidence));
}

Result<void> IoSignalSubsystem::EvaluateRules() {
  std::lock_guard lock(state_->mutex);
  state_->EvaluateRegisteredRules(state_->clock.now(), true);
  return Result<void>::Ok();
}

Result<std::optional<TimePoint>> IoSignalSubsystem::NextRuleExpiry() const {
  std::lock_guard lock(state_->mutex);
  std::optional<TimePoint> next;
  const auto now = state_->clock.now();
  for (const auto& [id, registration] : state_->watches) {
    (void)id;
    const auto* rule = std::get_if<State::Rule>(&registration);
    if (!rule) continue;
    for (const auto& input : rule->condition->inputs()) {
      if (std::holds_alternative<IoConditionBlackboardKeyDTO>(input.source)) continue;
      const auto& signal = state_->signals.at(std::get<IoSignalId>(input.source));
      const auto required =
          std::lower_bound(rule->required_sources.begin(), rule->required_sources.end(),
                           signal.descriptor.source_id);
      if (rule->losses[static_cast<std::size_t>(required - rule->required_sources.begin())])
        continue;
      if (state_->Availability(signal, now) != IoSignalAvailability::kFresh)
        return Result<std::optional<TimePoint>>::Ok(now);
      if (signal.descriptor.freshness.mode == IoFreshnessMode::kProviderValidity) continue;
      const auto receipt = signal.latest->received_at_ns;
      const auto age = *signal.descriptor.freshness.maximum_age_ns;
      if (receipt > std::numeric_limits<TimePoint>::max() - age ||
          receipt + age == std::numeric_limits<TimePoint>::max())
        continue;
      const auto expiry = receipt + age + 1;
      if (!next || expiry < *next) next = expiry;
    }
  }
  return Result<std::optional<TimePoint>>::Ok(next);
}

Result<void> IoSignalSubsystem::ClearOutsideChange(const IoSignalId& output) {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->signals.find(output);
  if (found == state_->signals.end()) return Result<void>::Err(invalid("io_signal_unknown"));
  if (!found->second.changed_outside)
    return Result<void>::Err(invalid("io_signal_changed_outside_unavailable"));
  state_->SetChangedOutside(output, false, state_->clock.now());
  state_->NotifySampleChanges();
  return Result<void>::Ok();
}

Result<void> IoSignalSubsystem::ClearComponentOutsideChanges(
    const ControllableComponentId& component) {
  std::lock_guard lock(state_->mutex);
  const auto now = state_->clock.now();
  for (const auto& [id, signal] : state_->signals)
    if (id.controllable_component_id == component && signal.changed_outside)
      state_->SetChangedOutside(id, false, now);
  state_->NotifySampleChanges();
  return Result<void>::Ok();
}

std::optional<TimePoint> IoSignalSubsystem::NextSampleExpiry() const {
  std::lock_guard lock(state_->mutex);
  std::optional<TimePoint> next;
  const auto now = state_->clock.now();
  for (const auto& [id, signal] : state_->signals) {
    (void)id;
    if (signal.descriptor.freshness.mode == IoFreshnessMode::kProviderValidity ||
        state_->Availability(signal, now) != IoSignalAvailability::kFresh)
      continue;
    const auto receipt = signal.latest->received_at_ns;
    const auto age = *signal.descriptor.freshness.maximum_age_ns;
    if (receipt > std::numeric_limits<TimePoint>::max() - age - 1) continue;
    const auto expiry = receipt + age + 1;
    if (!next || expiry < *next) next = expiry;
  }
  return next;
}

void IoSignalSubsystem::RefreshSamples() {
  std::lock_guard lock(state_->mutex);
  state_->NotifySampleChanges();
}

Result<IoSignalObservationDTO> IoSignalSubsystem::ReadSignal(const IoSignalId& signal) const {
  std::lock_guard lock(state_->mutex);
  const auto found = state_->signals.find(signal);
  if (found == state_->signals.end())
    return Result<IoSignalObservationDTO>::Err(invalid("io_signal_unknown"));
  return Result<IoSignalObservationDTO>::Ok(
      state_->Observation(found->second, state_->clock.now()));
}

Result<IoWatchRegistrationDTO> IoSignalSubsystem::RegisterLevelWatch(
    const IoLevelWatchRequestDTO& request) {
  using Reply = Result<IoWatchRegistrationDTO>;
  std::lock_guard lock(state_->mutex);
  const auto found = state_->signals.find(request.signal_id);
  if (found == state_->signals.end()) return Reply::Err(invalid("io_signal_unknown"));
  if (!state_->ScopeValid(request.scope)) return Reply::Err(invalid("io_watch_scope_invalid"));
  if (state_->watches.size() == state_->limits.maximum_watches)
    return Reply::Err({"execution_rejected",
                       "I/O watch registration bound reached",
                       {{"reason", "io_watch_bound_exceeded"}}});
  if (state_->last_watch_id == UINT64_MAX) return Reply::Err(exhausted());
  IoWatchRegistrationDTO registration{state_->owner_epoch, state_->generation,
                                      ++state_->last_watch_id, request.scope};
  auto observation = state_->Observation(found->second, state_->clock.now());
  State::Watch watch{registration, request.signal_id, request.expected_value, std::nullopt};
  if (observation.availability == IoSignalAvailability::kFresh &&
      observation.latest_sample->publication.value == request.expected_value)
    watch.matched = observation.latest_sample;
  state_->watches.emplace(registration.registration_id, std::move(watch));
  return Reply::Ok(std::move(registration));
}

Result<IoWatchResultDTO> IoSignalSubsystem::ReadWatch(
    const IoWatchRegistrationDTO& registration) const {
  using Reply = Result<IoWatchResultDTO>;
  std::lock_guard lock(state_->mutex);
  if (!state_->TokenCurrent(registration)) return Reply::Err(stale("io_watch_generation_stale"));
  const auto found = state_->watches.find(registration.registration_id);
  if (found == state_->watches.end())
    return Reply::Ok({IoWatchState::kReleased, std::nullopt, std::nullopt});
  const auto* stored = std::get_if<State::Watch>(&found->second);
  if (!stored || stored->registration != registration)
    return Reply::Err(stale("io_watch_scope_stale"));
  const auto& watch = *stored;
  return Reply::Ok({watch.matched ? IoWatchState::kMatched : IoWatchState::kPending,
                    state_->Observation(state_->signals.at(watch.signal_id), state_->clock.now()),
                    watch.matched});
}

Result<void> IoSignalSubsystem::ReleaseWatch(const IoWatchRegistrationDTO& registration) {
  std::lock_guard lock(state_->mutex);
  if (!state_->TokenCurrent(registration))
    return Result<void>::Err(stale("io_watch_generation_stale"));
  const auto found = state_->watches.find(registration.registration_id);
  if (found == state_->watches.end()) return Result<void>::Ok();
  const auto* stored = std::get_if<State::Watch>(&found->second);
  if (!stored || stored->registration != registration)
    return Result<void>::Err(stale("io_watch_scope_stale"));
  state_->watches.erase(found);
  return Result<void>::Ok();
}

Result<void> IoSignalSubsystem::ReleaseNodeWatches(const IoWatchScopeDTO& scope) {
  std::lock_guard lock(state_->mutex);
  if (!state_->ScopeValid(scope)) return Result<void>::Err(invalid("io_watch_scope_invalid"));
  std::erase_if(state_->watches, [&](const auto& entry) {
    const auto* watch = std::get_if<State::Watch>(&entry.second);
    return watch && watch->registration.scope == scope;
  });
  return Result<void>::Ok();
}

Result<void> IoSignalSubsystem::ReleaseRunWatches(const RequestOwnerEpoch& runner_owner_epoch,
                                                  const ProgramRunId& run_id) {
  std::lock_guard lock(state_->mutex);
  if (!text_valid(runner_owner_epoch.value(), state_->limits.maximum_identity_bytes) ||
      !text_valid(run_id.value(), state_->limits.maximum_identity_bytes))
    return Result<void>::Err(invalid("io_watch_scope_invalid"));
  std::erase_if(state_->watches, [&](const auto& entry) {
    const auto* watch = std::get_if<State::Watch>(&entry.second);
    return watch && watch->registration.scope.program_runner_owner_epoch == runner_owner_epoch &&
           watch->registration.scope.program_run_id == run_id;
  });
  return Result<void>::Ok();
}

}  // namespace manyforge::core

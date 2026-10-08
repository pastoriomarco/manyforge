#include <manyforge_core/types/component_incident.hpp>

#include <algorithm>
#include <manyforge_content_identity/canonical_value.hpp>
#include <string>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

/// Full-width owner-clock time. A negative value is refused (the codec
/// throws), never clamped: validation must have rejected it before.
[[nodiscard]] CanonicalValue NonNegative(const std::int64_t value) {
  return content_identity::nonnegative_int64_value(value);
}

[[nodiscard]] CanonicalValue Unsigned(const std::uint64_t value) {
  constexpr std::uint64_t kMaximumSafe = 9'007'199'254'740'991ULL;
  return value <= kMaximumSafe ? CanonicalValue(static_cast<double>(value))
                               : CanonicalValue(std::to_string(value));
}

[[nodiscard]] CanonicalValue OptionalText(const std::optional<std::string>& value) {
  return value ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue Strings(const std::vector<std::string>& values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto& value : values) result.emplace_back(value);
  return CanonicalValue(std::move(result));
}

[[nodiscard]] std::string LastSegment(const std::string& id) {
  const auto dot = id.rfind('.');
  return dot == std::string::npos ? id : id.substr(dot + 1U);
}

[[nodiscard]] CanonicalValue FactValue(const LifecycleFactValueDTO& value) {
  switch (value.kind) {
    case LifecycleFactValueKindDTO::kUnknown:
      return CanonicalValue(nullptr);
    case LifecycleFactValueKindDTO::kBoolean:
      return value.boolean_value ? CanonicalValue(*value.boolean_value) : CanonicalValue(nullptr);
    case LifecycleFactValueKindDTO::kInteger:
      if (!value.integer_value) return CanonicalValue(nullptr);
      return *value.integer_value >= 0 ? NonNegative(*value.integer_value)
                                       : CanonicalValue(std::to_string(*value.integer_value));
    case LifecycleFactValueKindDTO::kToken:
      return value.token_value ? CanonicalValue(*value.token_value) : CanonicalValue(nullptr);
  }
  return CanonicalValue(nullptr);
}

[[nodiscard]] CanonicalValue ObservationValue(
    const std::optional<ComponentIncidentObservationDTO>& value) {
  if (!value) return CanonicalValue(nullptr);
  CanonicalValue::Object facts;
  for (const auto& fact : value->facts) {
    if (std::any_of(facts.begin(), facts.end(), [&](const auto& member) {
          return member.first == fact.lifecycle_fact_id.value();
        })) {
      continue;
    }
    facts.emplace_back(fact.lifecycle_fact_id.value(), FactValue(fact.value));
  }
  std::sort(facts.begin(), facts.end(),
            [](const auto& left, const auto& right) { return left.first < right.first; });
  return CanonicalValue(CanonicalValue::Object{
      {"control_domain_id", CanonicalValue(value->control_domain_id.value())},
      {"facts", CanonicalValue(std::move(facts))},
      {"generation", Unsigned(value->observation_generation)},
      {"observed_at_ns", NonNegative(value->observed_at_ns)},
      {"observed_state", CanonicalValue(std::string(to_string(value->observed_state)))}});
}

[[nodiscard]] CanonicalValue TimelineEntry(const std::int64_t at_ns,
                                           const LifecycleBlockingCauseClassDTO cause,
                                           const std::vector<std::string>& vendor_codes) {
  return CanonicalValue(CanonicalValue::Object{
      {"at_ns", NonNegative(at_ns)},
      {"cause_class", CanonicalValue(std::string(to_string(cause)))},
      {"vendor_codes", Strings(vendor_codes)}});
}

/// `record_json` with the named members replaced (or added).
[[nodiscard]] std::string Patched(const std::string_view record_json,
                                  CanonicalValue::Object replacements) {
  const auto root = content_identity::parse_canonical_json(record_json);
  if (!root.is_object()) throw content_identity::CanonicalValueError("incident record");
  CanonicalValue::Object fields;
  for (const auto& member : root.as_object()) {
    if (std::none_of(replacements.begin(), replacements.end(),
                     [&](const auto& replaced) { return replaced.first == member.first; })) {
      fields.push_back(member);
    }
  }
  for (auto& replaced : replacements) fields.push_back(std::move(replaced));
  return content_identity::canonical_json(CanonicalValue(std::move(fields)));
}

}  // namespace

bool ComponentLifecycleIncidentDTO::locally_valid() const noexcept {
  return !incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
         !controllable_component_id.empty() &&
         controllable_component_id.value().size() <= kMaxLifecycleIdentityBytes &&
         lifecycle_cause_requires_intervention(cause_class) &&
         vendor_codes.size() <= kMaxComponentIncidentVendorCodes &&
         std::all_of(vendor_codes.begin(), vendor_codes.end(),
                     [](const auto& code) {
                       return !code.empty() && code.size() <= kMaxComponentIncidentVendorCodeBytes;
                     }) &&
         detected_at_ns >= 0 && !summary.empty() &&
         summary.size() <= kMaxComponentIncidentSummaryBytes &&
         (!last_good_observation || last_good_observation->observed_at_ns >= 0) &&
         (containment_state != ComponentIncidentContainmentStateDTO::kRequested ||
          containment.has_value()) &&
         acknowledged == acknowledged_at_ns.has_value() &&
         (!acknowledged_at_ns || *acknowledged_at_ns >= 0);
}

std::vector<LifecycleIncidentRecoveryPolicyDTO> DefaultLifecycleIncidentRecoveryPolicy() {
  using Cause = LifecycleBlockingCauseClassDTO;
  // Addendum 2 item 6: every class requires the operator's acknowledgement;
  // the steps are performed by the lifecycle plans CCLC runs afterwards.
  return {
      {Cause::kEmergencyStop, true, true, false,
       "release_emergency_stop,clear_error,enable_motors,servo_mode,ready"},
      {Cause::kProtectiveStop, true, false, false, "clear_error,ready_from_fresh_state"},
      {Cause::kSafeguardStop, true, true, false, "reset_safeguard,clear_error,ready"},
      {Cause::kFault, true, true, false, "physical_action_if_required,clear_error,ready"},
      {Cause::kConfiguration, true, true, false, "correct_configuration,ready"},
      {Cause::kUnknown, true, true, false, "inspect,ready_from_fresh_state"},
      {Cause::kCollision, true, false, false, "clear_error,ready_from_fresh_state"},
      {Cause::kExternalControl, true, false, false, "ready_sequence_from_fresh_state"},
      {Cause::kProviderLost, true, false, true, "provider_restart_if_managed,ready"},
  };
}

LifecycleIncidentRecoveryPolicyDTO LifecycleIncidentRecoveryPolicyFor(
    const std::vector<LifecycleIncidentRecoveryPolicyDTO>& policy,
    const LifecycleBlockingCauseClassDTO cause) {
  const auto found = std::find_if(policy.begin(), policy.end(),
                                  [&](const auto& row) { return row.cause_class == cause; });
  if (found != policy.end()) return *found;
  return {cause, true, true, false, "inspect,ready_from_fresh_state"};
}

std::vector<std::string> ComponentIncidentVendorCodes(const std::vector<LifecycleFactDTO>& facts) {
  std::vector<std::string> codes;
  for (const auto& fact : facts) {
    const auto& id = fact.lifecycle_fact_id.value();
    if (id.rfind(kLifecycleVendorCodeFactPrefix, 0U) != 0U) continue;
    std::string code;
    switch (fact.value.kind) {
      case LifecycleFactValueKindDTO::kUnknown:
        break;
      case LifecycleFactValueKindDTO::kToken:
        if (fact.value.token_value) code = *fact.value.token_value;
        break;
      case LifecycleFactValueKindDTO::kInteger:
        if (fact.value.integer_value) {
          code = LastSegment(id) + "=" + std::to_string(*fact.value.integer_value);
        }
        break;
      case LifecycleFactValueKindDTO::kBoolean:
        if (fact.value.boolean_value) {
          code = LastSegment(id) + "=" + (*fact.value.boolean_value ? "true" : "false");
        }
        break;
    }
    if (code.empty() || code.size() > kMaxComponentIncidentVendorCodeBytes ||
        !content_identity::is_utf8_nfc(code) ||
        std::find(codes.begin(), codes.end(), code) != codes.end()) {
      continue;
    }
    codes.push_back(std::move(code));
    if (codes.size() == kMaxComponentIncidentVendorCodes) break;
  }
  return codes;
}

std::string ComponentIncidentEventPayload(const ComponentLifecycleIncidentDTO& incident,
                                          const LifecycleIncidentRecoveryPolicyDTO& policy,
                                          const std::string_view event_kind) {
  CanonicalValue::Array operations;
  std::optional<std::string> program_run_id;
  CanonicalValue stop(nullptr);
  if (incident.containment) {
    for (const auto& operation : incident.containment->affected_operation_ids) {
      operations.emplace_back(operation.value());
    }
    if (incident.containment->program_run_id) {
      program_run_id = incident.containment->program_run_id->value();
    }
    stop = CanonicalValue(CanonicalValue::Object{
        {"stop_generation", Unsigned(incident.containment->stop_generation.value())},
        {"stop_id", CanonicalValue(incident.containment->stop_id.value())}});
  }
  CanonicalValue::Array providers;
  for (const auto& provider : incident.providers) {
    providers.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(provider.control_domain_id.value())},
        // CCLC does not observe provider processes; exit status is unknown.
        {"process_exit", CanonicalValue(nullptr)},
        {"provider_instance_id", CanonicalValue(provider.provider_instance_id.value())},
        {"provider_runtime_epoch", CanonicalValue(provider.provider_runtime_epoch.value())},
        {"provider_runtime_unit_id", CanonicalValue(provider.provider_runtime_unit_id.value())}});
  }
  const bool acknowledged = event_kind == kComponentIncidentAcknowledgedEventKind;
  CanonicalValue::Object fields{
      {"acknowledged", CanonicalValue(acknowledged && incident.acknowledged)},
      {"acknowledged_at_ns", acknowledged && incident.acknowledged_at_ns
                                 ? NonNegative(*incident.acknowledged_at_ns)
                                 : CanonicalValue(nullptr)},
      {"acknowledged_by", acknowledged ? OptionalText(incident.acknowledged_by)
                                       : CanonicalValue(nullptr)},
      {"acknowledgement_request_id",
       acknowledged && incident.acknowledgement_request_id
           ? CanonicalValue(incident.acknowledgement_request_id->value())
           : CanonicalValue(nullptr)},
      {"active_operation_ids", CanonicalValue(std::move(operations))},
      {"cause_class", CanonicalValue(std::string(to_string(incident.cause_class)))},
      {"containment_error", OptionalText(incident.containment_error)},
      {"containment_state", CanonicalValue(std::string(to_string(incident.containment_state)))},
      {"controllable_component_id", CanonicalValue(incident.controllable_component_id.value())},
      {"detected_at_ns", NonNegative(incident.detected_at_ns)},
      {"event_kind", CanonicalValue(std::string(event_kind))},
      {"incident_id", CanonicalValue(incident.incident_id)},
      {"last_good_observation", ObservationValue(incident.last_good_observation)},
      {"program_run_id", OptionalText(program_run_id)},
      {"providers", CanonicalValue(std::move(providers))},
      {"recovery_policy",
       CanonicalValue(CanonicalValue::Object{
           {"acknowledgement_required", CanonicalValue(policy.acknowledgement_required)},
           {"physical_action_required", CanonicalValue(policy.physical_action_required)},
           {"provider_restart_after_acknowledgement",
            CanonicalValue(policy.provider_restart_after_acknowledgement)},
           {"recovery_steps", CanonicalValue(std::string(policy.recovery_steps))}})},
      {"schema_version", CanonicalValue("manyforge.component_incident_event.v1")},
      {"stop", std::move(stop)},
      {"summary", CanonicalValue(incident.summary)},
      {"timeline", CanonicalValue(CanonicalValue::Array{TimelineEntry(
                       incident.detected_at_ns, incident.cause_class, incident.vendor_codes)})},
      {"vendor_codes", Strings(incident.vendor_codes)},
  };
  return content_identity::canonical_json(CanonicalValue(std::move(fields)));
}

std::string ComponentIncidentRecordWithContainment(
    const std::string_view record_json, const ComponentIncidentContainmentStateDTO state,
    const std::optional<ComponentIncidentContainmentReceiptDTO>& receipt,
    const std::optional<std::string>& error) {
  CanonicalValue::Array operations;
  CanonicalValue stop(nullptr);
  CanonicalValue program_run(nullptr);
  if (receipt) {
    for (const auto& operation : receipt->affected_operation_ids) {
      operations.emplace_back(operation.value());
    }
    stop = CanonicalValue(CanonicalValue::Object{
        {"stop_generation", Unsigned(receipt->stop_generation.value())},
        {"stop_id", CanonicalValue(receipt->stop_id.value())}});
    if (receipt->program_run_id) program_run = CanonicalValue(receipt->program_run_id->value());
  }
  return Patched(record_json,
                 {{"active_operation_ids", CanonicalValue(std::move(operations))},
                  {"containment_error", OptionalText(error)},
                  {"containment_state", CanonicalValue(std::string(to_string(state)))},
                  {"program_run_id", std::move(program_run)},
                  {"stop", std::move(stop)}});
}

std::string ComponentIncidentRecordWithChange(const std::string_view record_json,
                                              const std::int64_t at_ns,
                                              const LifecycleBlockingCauseClassDTO cause,
                                              const std::vector<std::string>& vendor_codes) {
  const auto root = content_identity::parse_canonical_json(record_json);
  CanonicalValue::Array timeline;
  if (const auto* existing = root.find_object_member("timeline");
      existing != nullptr && existing->is_array()) {
    timeline = existing->as_array();
  }
  // Bounded: the first entry (detection) is kept, then the newest ones.
  if (timeline.size() >= kMaxComponentIncidentTimelineEntries) {
    timeline.erase(timeline.begin() + 1);
  }
  timeline.push_back(TimelineEntry(at_ns, cause, vendor_codes));
  return Patched(record_json,
                 {{"cause_class", CanonicalValue(std::string(to_string(cause)))},
                  {"timeline", CanonicalValue(std::move(timeline))},
                  {"vendor_codes", Strings(vendor_codes)}});
}

std::string ComponentIncidentAcknowledgedPayload(
    const std::string_view record_json, const ComponentLifecycleIncidentDTO& acknowledged) {
  return Patched(
      record_json,
      {{"acknowledged", CanonicalValue(true)},
       {"acknowledged_at_ns", acknowledged.acknowledged_at_ns
                                  ? NonNegative(*acknowledged.acknowledged_at_ns)
                                  : CanonicalValue(nullptr)},
       {"acknowledged_by", OptionalText(acknowledged.acknowledged_by)},
       {"acknowledgement_request_id",
        acknowledged.acknowledgement_request_id
            ? CanonicalValue(acknowledged.acknowledgement_request_id->value())
            : CanonicalValue(nullptr)},
       {"event_kind", CanonicalValue(std::string(kComponentIncidentAcknowledgedEventKind))}});
}

}  // namespace manyforge::core

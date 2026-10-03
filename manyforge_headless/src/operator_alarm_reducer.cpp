#include "manyforge_headless/operator_alarm_owner.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <tuple>
#include <utility>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumIdentityBytes = 1024U;
constexpr std::size_t kMaximumActivationIdBytes = 2048U;
constexpr std::size_t kMaximumWallTimeBytes = 128U;
constexpr std::size_t kMaximumReasonBytes = 4096U;
constexpr std::size_t kMaximumDisplayArgsBytes = 8U * 1024U;
constexpr std::size_t kMaximumDetailsBytes = 32U * 1024U;
constexpr std::size_t kMaximumCorrelationsBytes = 32U * 1024U;
constexpr std::size_t kMaximumMappingKeys = 32U;
constexpr std::size_t kMaximumEvidenceRefBytes = 1024U;

[[noreturn]] void AlarmFail(const std::string &code,
                            const std::string &message) {
  throw OperatorAlarmError(code, message);
}

void Require(const bool condition, const std::string &code,
             const std::string &message) {
  if (!condition) {
    AlarmFail(code, message);
  }
}

void ValidateText(const std::string &value, const std::string_view label,
                  const std::size_t maximum_bytes,
                  const bool allow_empty = false) {
  Require((allow_empty || !value.empty()) && value.size() <= maximum_bytes &&
              content_identity::is_utf8_nfc(value),
          "operator_alarm_value_invalid",
          std::string(label) + " must be bounded NFC UTF-8");
}

void ValidateSafeInteger(const std::uint64_t value,
                         const std::string_view label,
                         const bool nonzero = false) {
  Require(value <= kMaximumSafeInteger && (!nonzero || value != 0U),
          "operator_alarm_value_invalid",
          std::string(label) + " must be an I-JSON safe integer");
}

void ValidateSafeInteger(const std::int64_t value,
                         const std::string_view label) {
  Require(value >= 0,
          "operator_alarm_value_invalid",
          std::string(label) + " must be a nonnegative int64");
}

void ValidateBoundedObject(
    const CanonicalValue &value, const std::string_view label,
    const std::size_t maximum_bytes,
    const std::size_t maximum_keys = kMaximumMappingKeys) {
  Require(value.is_object() && value.as_object().size() <= maximum_keys &&
              content_identity::canonical_json(value).size() <= maximum_bytes,
          "operator_alarm_condition_update_invalid",
          std::string(label) + " must be one bounded object");
}

void ValidateSubject(const AlarmSubject &subject) {
  ValidateText(subject.subject_kind, "subject_kind", kMaximumIdentityBytes);
  ValidateText(subject.subject_id, "subject_id", kMaximumIdentityBytes);
  Require(subject.resource_ids.size() <= kMaximumAlarmSubjectResources,
          "operator_alarm_scope_invalid",
          "resource_ids exceeds the 16-resource subject bound");
  Require(std::is_sorted(subject.resource_ids.begin(),
                         subject.resource_ids.end()) &&
              std::adjacent_find(subject.resource_ids.begin(),
                                 subject.resource_ids.end()) ==
                  subject.resource_ids.end(),
          "operator_alarm_scope_invalid",
          "resource_ids must be sorted and unique");
  for (const auto &resource : subject.resource_ids) {
    ValidateText(resource, "resource_id", kMaximumIdentityBytes);
  }
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label,
                      const bool nonzero = false) {
  ValidateSafeInteger(value, label, nonzero);
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  ValidateSafeInteger(value, label);
  return content_identity::nonnegative_int64_value(value);
}

CanonicalValue OptionalNumber(const std::optional<std::uint64_t> value,
                              const std::string_view label) {
  return value.has_value() ? Number(*value, label, true)
                           : CanonicalValue(nullptr);
}

CanonicalValue OptionalNumber(const std::optional<std::int64_t> value,
                              const std::string_view label) {
  return value.has_value() ? Number(*value, label) : CanonicalValue(nullptr);
}

CanonicalValue OptionalString(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue StringArray(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(CanonicalValue(value));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue SubjectValue(const AlarmSubject &subject) {
  ValidateSubject(subject);
  return CanonicalValue(CanonicalValue::Object{
      {"resource_ids", StringArray(subject.resource_ids)},
      {"subject_id", CanonicalValue(subject.subject_id)},
      {"subject_kind", CanonicalValue(subject.subject_kind)},
  });
}

CanonicalValue CallerValue(const AlarmCallerAttribution &caller) {
  return CanonicalValue(CanonicalValue::Object{
      {"adapter_id", CanonicalValue(caller.adapter_id)},
      {"assurance", CanonicalValue(caller.assurance)},
      {"correlation_id", OptionalString(caller.correlation_id)},
      {"request_id", CanonicalValue(caller.request_id)},
      {"request_owner_epoch", CanonicalValue(caller.request_owner_epoch)},
      {"source_label", CanonicalValue(caller.source_label)},
      {"transport_kind", CanonicalValue(caller.transport_kind)},
      {"trusted_principal_id", OptionalString(caller.trusted_principal_id)},
  });
}

CanonicalValue
OptionalCaller(const std::optional<AlarmCallerAttribution> &caller) {
  return caller.has_value() ? CallerValue(*caller) : CanonicalValue(nullptr);
}

std::string UpdateFingerprint(const AlarmConditionUpdate &update) {
  const CanonicalValue value(CanonicalValue::Object{
      {"active", CanonicalValue(update.active)},
      {"correlations", update.correlations},
      {"definition_id", CanonicalValue(update.definition_id)},
      {"details", update.details},
      {"display_args", update.display_args},
      {"evidence_refs", StringArray(update.evidence_refs)},
      {"observed_at_ns", Number(update.observed_at_ns, "observed_at_ns")},
      {"observed_at_wall", CanonicalValue(update.observed_at_wall)},
      {"quality", CanonicalValue(std::string(ToString(update.quality)))},
      {"source_epoch", CanonicalValue(update.source_epoch)},
      {"source_id", CanonicalValue(update.source_id)},
      {"source_sequence", Number(update.source_sequence, "source_sequence")},
      {"subject", SubjectValue(update.subject)},
  });
  return content_identity::framed_sha256("manyforge-alarm-condition-update",
                                         "v1", value);
}

void ValidateUpdate(const OperatorAlarmDefinition &definition,
                    const AlarmConditionUpdate &update) {
  Require(update.definition_id == definition.definition_id,
          "operator_alarm_definition_mismatch",
          "condition update names a different definition");
  ValidateSubject(update.subject);
  ValidateText(update.source_id, "source_id", kMaximumIdentityBytes);
  ValidateText(update.source_epoch, "source_epoch", kMaximumIdentityBytes);
  ValidateSafeInteger(update.source_sequence, "source_sequence");
  ValidateSafeInteger(update.observed_at_ns, "observed_at_ns");
  ValidateText(update.observed_at_wall, "observed_at_wall",
               kMaximumWallTimeBytes);
  ValidateBoundedObject(update.display_args, "display_args",
                        kMaximumDisplayArgsBytes);
  ValidateBoundedObject(update.details, "details", kMaximumDetailsBytes, 4096U);
  ValidateBoundedObject(update.correlations, "correlations",
                        kMaximumCorrelationsBytes);
  Require(update.evidence_refs.size() <= definition.max_evidence_refs,
          "operator_alarm_condition_update_invalid",
          "evidence_refs exceeds the catalog limit");
  std::set<std::string> unique;
  for (const auto &reference : update.evidence_refs) {
    ValidateText(reference, "evidence_ref", kMaximumEvidenceRefBytes);
    Require(unique.insert(reference).second,
            "operator_alarm_condition_update_invalid",
            "evidence_refs contains a duplicate");
  }
}

std::string RenderMessage(const OperatorAlarmDefinition &definition,
                          const AlarmSubject &subject) {
  std::string result = definition.message_template;
  const auto replace = [&result](const std::string_view placeholder,
                                 const std::string &value) {
    std::size_t offset = 0U;
    while ((offset = result.find(placeholder, offset)) != std::string::npos) {
      result.replace(offset, placeholder.size(), value);
      offset += value.size();
    }
  };
  std::string resources;
  for (const auto &resource : subject.resource_ids) {
    if (!resources.empty()) {
      resources += ", ";
    }
    resources += resource;
  }
  replace("{subject_kind}", subject.subject_kind);
  replace("{subject_id}", subject.subject_id);
  replace("{resource_ids}", resources);
  Require(result.find('{') == std::string::npos &&
              result.find('}') == std::string::npos,
          "operator_alarm_condition_update_invalid",
          "message template contains an unresolved placeholder");
  ValidateText(result, "rendered message", 1024U);
  return result;
}

CanonicalValue MergeObject(const CanonicalValue &existing,
                           const CanonicalValue &incoming,
                           const std::size_t maximum_bytes,
                           const std::size_t maximum_keys) {
  if (incoming.as_object().empty()) {
    return existing;
  }
  std::map<std::string, CanonicalValue> merged;
  for (const auto &[key, value] : existing.as_object()) {
    merged.insert_or_assign(key, value);
  }
  for (const auto &[key, value] : incoming.as_object()) {
    merged.insert_or_assign(key, value);
  }
  CanonicalValue::Object values;
  values.reserve(merged.size());
  for (auto &[key, value] : merged) {
    values.emplace_back(std::move(key), std::move(value));
  }
  CanonicalValue result(std::move(values));
  if (result.as_object().size() > maximum_keys ||
      content_identity::canonical_json(result).size() > maximum_bytes) {
    return incoming;
  }
  return result;
}

std::vector<std::string> MergeEvidence(const std::vector<std::string> &existing,
                                       const std::vector<std::string> &incoming,
                                       const std::size_t limit) {
  auto result = existing;
  for (const auto &value : incoming) {
    if (std::find(result.begin(), result.end(), value) == result.end()) {
      result.push_back(value);
    }
  }
  if (result.size() > limit) {
    result.erase(result.begin(),
                 result.end() - static_cast<std::ptrdiff_t>(limit));
  }
  return result;
}

AlarmLifecycleFact
LifecycleFact(std::string event_type, const OperatorAlarmRecord &record,
              const std::optional<AlarmLifecycleState> previous_state,
              const std::int64_t occurred_at_ns, std::string occurred_at_wall,
              std::optional<AlarmCallerAttribution> caller = std::nullopt,
              std::optional<std::string> reason = std::nullopt) {
  return {
      std::move(event_type),
      record.definition_id,
      record.condition_id,
      record.activation_id,
      record.subject,
      previous_state,
      record.lifecycle_state,
      record.record_revision,
      0U,
      {},
      occurred_at_ns,
      std::move(occurred_at_wall),
      record.source_id,
      record.source_epoch,
      record.source_sequence,
      std::move(caller),
      std::move(reason),
      record.correlations,
      record.details,
  };
}

AlarmSourceCursor NextCursor(const AlarmConditionUpdate &update,
                             const std::uint64_t source_epoch_order,
                             std::string fingerprint) {
  return {
      update.source_id,       update.source_epoch,    source_epoch_order,
      update.source_sequence, std::move(fingerprint), update.observed_at_ns,
  };
}

bool IsDuplicateOrValidateOrdering(const AlarmPointState &point,
                                   const AlarmConditionUpdate &update,
                                   const std::uint64_t source_epoch_order,
                                   const std::string &fingerprint) {
  if (!point.cursor.has_value()) {
    return false;
  }
  const auto &cursor = *point.cursor;
  Require(update.source_id == cursor.source_id,
          "operator_alarm_source_authority_conflict",
          "another source cannot update this condition");
  Require(source_epoch_order >= cursor.source_epoch_order,
          "operator_alarm_source_epoch_stale",
          "condition update belongs to an older source epoch");
  if (source_epoch_order == cursor.source_epoch_order) {
    Require(update.source_epoch == cursor.source_epoch,
            "operator_alarm_source_epoch_conflict",
            "one source epoch order names two epochs");
    Require(update.source_sequence >= cursor.source_sequence,
            "operator_alarm_source_sequence_stale",
            "condition update sequence is stale");
    if (update.source_sequence == cursor.source_sequence) {
      Require(fingerprint == cursor.fingerprint,
              "operator_alarm_source_sequence_conflict",
              "one source sequence contains conflicting state");
      return true;
    }
  }
  return false;
}

CanonicalValue StateValue(const std::optional<AlarmLifecycleState> state) {
  return state.has_value() ? CanonicalValue(std::string(ToString(*state)))
                           : CanonicalValue(nullptr);
}

} // namespace

OperatorAlarmError::OperatorAlarmError(std::string code, std::string message)
    : std::runtime_error(code + ": " + message), code_(std::move(code)) {}

const std::string &OperatorAlarmError::code() const noexcept { return code_; }

std::string_view ToString(const AlarmConditionQuality quality) noexcept {
  switch (quality) {
  case AlarmConditionQuality::kGood:
    return "good";
  case AlarmConditionQuality::kUnknown:
    return "unknown";
  }
  return {};
}

std::string_view ToString(const AlarmLifecycleState state) noexcept {
  switch (state) {
  case AlarmLifecycleState::kActiveUnacknowledged:
    return "active_unacknowledged";
  case AlarmLifecycleState::kActiveAcknowledged:
    return "active_acknowledged";
  case AlarmLifecycleState::kReturnedUnacknowledged:
    return "returned_unacknowledged";
  case AlarmLifecycleState::kReturnedAcknowledged:
    return "returned_acknowledged";
  }
  return {};
}

bool IsActive(const AlarmLifecycleState state) noexcept {
  return state == AlarmLifecycleState::kActiveUnacknowledged ||
         state == AlarmLifecycleState::kActiveAcknowledged;
}

bool IsAcknowledged(const AlarmLifecycleState state) noexcept {
  return state == AlarmLifecycleState::kActiveAcknowledged ||
         state == AlarmLifecycleState::kReturnedAcknowledged;
}

bool RequiresAttention(const AlarmLifecycleState state) noexcept {
  return state == AlarmLifecycleState::kActiveUnacknowledged ||
         state == AlarmLifecycleState::kReturnedUnacknowledged;
}

bool GatesMotion(const OperatorAlarmRecord &record) noexcept {
  return record.requires_motion_attention &&
         (record.acknowledgement_required
              ? RequiresAttention(record.lifecycle_state)
              : IsActive(record.lifecycle_state));
}

bool operator<(const AlarmAcknowledgementMember &left,
               const AlarmAcknowledgementMember &right) noexcept {
  return std::tie(left.condition_id, left.activation_id, left.record_revision) <
         std::tie(right.condition_id, right.activation_id,
                  right.record_revision);
}

std::string CanonicalAlarmConditionId(const OperatorAlarmCatalog &catalog,
                                      const OperatorAlarmDefinition &definition,
                                      const AlarmSubject &subject) {
  ValidateSubject(subject);
  std::string preimage;
  const auto append = [&preimage](const std::string_view value) {
    preimage += std::to_string(value.size());
    preimage.push_back(':');
    preimage.append(value);
    preimage.push_back(';');
  };
  append(catalog.schema_version());
  append(catalog.catalog_version());
  append(definition.definition_id);
  append(std::to_string(definition.version));
  append("subject_kind");
  append(subject.subject_kind);
  append("subject_id");
  append(subject.subject_id);
  append("resource_ids");
  append(std::to_string(subject.resource_ids.size()));
  for (const auto &resource : subject.resource_ids) {
    append(resource);
  }
  const auto digest = content_identity::sha256_digest(preimage);
  return "condition-" + digest.substr(7U, 24U);
}

AlarmTransitionResult ReduceAlarmConditionUpdate(
    const OperatorAlarmCatalog &catalog,
    const OperatorAlarmDefinition &definition, const AlarmPointState &point,
    const AlarmConditionUpdate &update, const std::uint64_t source_epoch_order,
    std::string activation_id) {
  ValidateUpdate(definition, update);
  ValidateSafeInteger(source_epoch_order, "source_epoch_order");
  ValidateText(activation_id, "activation_id", kMaximumActivationIdBytes);
  const auto condition_id =
      CanonicalAlarmConditionId(catalog, definition, update.subject);
  Require(point.condition_id == condition_id,
          "operator_alarm_condition_identity_mismatch",
          "condition point does not match the canonical subject");
  const auto fingerprint = UpdateFingerprint(update);
  if (IsDuplicateOrValidateOrdering(point, update, source_epoch_order,
                                    fingerprint)) {
    return {point, "duplicate", false, false, {}, std::nullopt};
  }
  const auto cursor = NextCursor(update, source_epoch_order, fingerprint);

  if (!update.active) {
    if (update.quality == AlarmConditionQuality::kUnknown &&
        point.record.has_value() && IsActive(point.record->lifecycle_state)) {
      AlarmFail("operator_alarm_clear_quality_unknown",
                "unknown source quality cannot clear an active condition");
    }
    if (!point.record.has_value() || !IsActive(point.record->lifecycle_state)) {
      auto next = point;
      next.cursor = cursor;
      return {std::move(next), "normal_observed", true, false, {},
              std::nullopt};
    }
    const auto previous_state = point.record->lifecycle_state;
    auto returned = *point.record;
    returned.lifecycle_state =
        IsAcknowledged(previous_state)
            ? AlarmLifecycleState::kReturnedAcknowledged
            : AlarmLifecycleState::kReturnedUnacknowledged;
    ++returned.record_revision;
    returned.returned_at_ns = update.observed_at_ns;
    returned.returned_at_wall = update.observed_at_wall;
    returned.source_id = update.source_id;
    returned.source_epoch = update.source_epoch;
    returned.source_sequence = update.source_sequence;
    returned.source_quality = update.quality;
    returned.details = MergeObject(returned.details, update.details,
                                   kMaximumDetailsBytes, 4096U);
    returned.correlations =
        MergeObject(returned.correlations, update.correlations,
                    kMaximumCorrelationsBytes, kMaximumMappingKeys);
    returned.evidence_refs =
        MergeEvidence(returned.evidence_refs, update.evidence_refs,
                      static_cast<std::size_t>(definition.max_evidence_refs));
    auto fact =
        LifecycleFact("operator_alarm_returned", returned, previous_state,
                      update.observed_at_ns, update.observed_at_wall);
    AlarmPointState next{condition_id, point.activation_count, returned,
                         cursor};
    return {std::move(next),
            "returned",
            true,
            false,
            {std::move(fact)},
            IsAcknowledged(returned.lifecycle_state)
                ? std::optional<OperatorAlarmRecord>(returned)
                : std::nullopt};
  }

  if (!point.record.has_value() || !IsActive(point.record->lifecycle_state)) {
    const auto previous_state =
        point.record.has_value()
            ? std::optional<AlarmLifecycleState>(point.record->lifecycle_state)
            : std::nullopt;
    OperatorAlarmRecord activated{
        catalog.schema_version(),
        catalog.catalog_version(),
        catalog.descriptor_digest(),
        definition.version,
        definition.definition_id,
        condition_id,
        std::move(activation_id),
        update.subject,
        definition.alarm_class,
        definition.priority,
        definition.title,
        RenderMessage(definition, update.subject),
        definition.consequence,
        definition.operator_response,
        definition.acknowledgement_required,
        definition.requires_motion_attention,
        definition.acknowledgement_required
            ? AlarmLifecycleState::kActiveUnacknowledged
            : AlarmLifecycleState::kActiveAcknowledged,
        point.activation_count + 1U,
        1U,
        point.record.has_value() ? point.record->record_revision + 1U : 1U,
        update.observed_at_ns,
        update.observed_at_wall,
        update.observed_at_ns,
        update.observed_at_wall,
        update.observed_at_ns,
        update.observed_at_wall,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        std::nullopt,
        update.source_id,
        update.source_epoch,
        update.source_sequence,
        update.quality,
        update.display_args,
        update.details,
        update.evidence_refs,
        update.correlations,
    };
    auto fact =
        LifecycleFact("operator_alarm_activated", activated, previous_state,
                      update.observed_at_ns, update.observed_at_wall);
    AlarmPointState next{condition_id, activated.activation_count, activated,
                         cursor};
    return {std::move(next),
            previous_state.has_value() ? "recurred" : "activated",
            true,
            true,
            {std::move(fact)},
            std::nullopt};
  }

  auto repeated = *point.record;
  ++repeated.occurrence_count;
  ++repeated.record_revision;
  if (update.observed_at_ns >= repeated.last_seen_at_ns) {
    repeated.last_seen_at_ns = update.observed_at_ns;
    repeated.last_seen_at_wall = update.observed_at_wall;
  }
  repeated.message = RenderMessage(definition, update.subject);
  repeated.source_epoch = update.source_epoch;
  repeated.source_sequence = update.source_sequence;
  repeated.source_quality = update.quality;
  repeated.display_args =
      MergeObject(repeated.display_args, update.display_args,
                  kMaximumDisplayArgsBytes, kMaximumMappingKeys);
  repeated.details = MergeObject(repeated.details, update.details,
                                 kMaximumDetailsBytes, 4096U);
  repeated.correlations =
      MergeObject(repeated.correlations, update.correlations,
                  kMaximumCorrelationsBytes, kMaximumMappingKeys);
  repeated.evidence_refs =
      MergeEvidence(repeated.evidence_refs, update.evidence_refs,
                    static_cast<std::size_t>(definition.max_evidence_refs));
  AlarmPointState next{condition_id, point.activation_count, repeated, cursor};
  return {std::move(next), "active_repeat", true, false, {}, std::nullopt};
}

AlarmTransitionResult ReduceAlarmAcknowledgement(
    const OperatorAlarmDefinition &definition, const AlarmPointState &point,
    const std::string_view activation_id,
    const std::uint64_t expected_record_revision,
    const AlarmCallerAttribution &caller,
    const std::optional<std::string> &reason,
    const std::int64_t acknowledged_at_ns, std::string acknowledged_at_wall) {
  Require(point.record.has_value(), "operator_alarm_not_found",
          "alarm condition is not live");
  const auto &record = *point.record;
  Require(record.activation_id == activation_id,
          "operator_alarm_activation_stale",
          "alarm activation changed before acknowledgement");
  if (IsAcknowledged(record.lifecycle_state)) {
    return {point, "already_acknowledged", false, false, {}, std::nullopt};
  }
  Require(record.record_revision == expected_record_revision,
          "operator_alarm_record_revision_stale",
          "alarm record changed before acknowledgement");
  if (definition.acknowledgement_reason_required) {
    Require(reason.has_value() && !reason->empty(),
            "operator_alarm_acknowledgement_reason_required",
            "this alarm definition requires a reason");
  }
  ValidateSafeInteger(acknowledged_at_ns, "acknowledged_at_ns");
  ValidateText(acknowledged_at_wall, "acknowledged_at_wall",
               kMaximumWallTimeBytes);
  if (reason.has_value()) {
    ValidateText(*reason, "reason", kMaximumReasonBytes);
  }
  auto acknowledged = record;
  const auto previous_state = acknowledged.lifecycle_state;
  acknowledged.lifecycle_state =
      IsActive(previous_state) ? AlarmLifecycleState::kActiveAcknowledged
                               : AlarmLifecycleState::kReturnedAcknowledged;
  ++acknowledged.record_revision;
  acknowledged.acknowledged_at_ns = acknowledged_at_ns;
  acknowledged.acknowledged_at_wall = acknowledged_at_wall;
  acknowledged.acknowledgement_attribution = caller;
  acknowledged.acknowledgement_reason = reason;
  auto fact =
      LifecycleFact("operator_alarm_acknowledged", acknowledged, previous_state,
                    acknowledged_at_ns, acknowledged_at_wall, caller, reason);
  auto next = point;
  next.record = acknowledged;
  return {std::move(next),
          "acknowledged",
          true,
          false,
          {std::move(fact)},
          IsActive(acknowledged.lifecycle_state)
              ? std::nullopt
              : std::optional<OperatorAlarmRecord>(acknowledged)};
}

CanonicalValue OperatorAlarmRecordValue(const OperatorAlarmRecord &record) {
  return CanonicalValue(CanonicalValue::Object{
      {"acknowledged", CanonicalValue(IsAcknowledged(record.lifecycle_state))},
      {"acknowledged_at_ns",
       OptionalNumber(record.acknowledged_at_ns, "acknowledged_at_ns")},
      {"acknowledged_at_wall", OptionalString(record.acknowledged_at_wall)},
      {"acknowledgement_attribution",
       OptionalCaller(record.acknowledgement_attribution)},
      {"acknowledgement_reason", OptionalString(record.acknowledgement_reason)},
      {"acknowledgement_required",
       CanonicalValue(record.acknowledgement_required)},
      {"activated_at_ns", Number(record.activated_at_ns, "activated_at_ns")},
      {"activated_at_wall", CanonicalValue(record.activated_at_wall)},
      {"activation_count",
       Number(record.activation_count, "activation_count", true)},
      {"activation_id", CanonicalValue(record.activation_id)},
      {"active", CanonicalValue(IsActive(record.lifecycle_state))},
      {"alarm_class", CanonicalValue(record.alarm_class)},
      {"catalog_descriptor_digest",
       CanonicalValue(record.catalog_descriptor_digest)},
      {"catalog_schema_version", CanonicalValue(record.catalog_schema_version)},
      {"catalog_version", CanonicalValue(record.catalog_version)},
      {"condition_id", CanonicalValue(record.condition_id)},
      {"consequence", CanonicalValue(record.consequence)},
      {"correlations", record.correlations},
      {"definition_id", CanonicalValue(record.definition_id)},
      {"definition_version",
       Number(record.definition_version, "definition_version", true)},
      {"details", record.details},
      {"display_args", record.display_args},
      {"evidence_refs", StringArray(record.evidence_refs)},
      {"first_seen_at_ns", Number(record.first_seen_at_ns, "first_seen_at_ns")},
      {"first_seen_at_wall", CanonicalValue(record.first_seen_at_wall)},
      {"last_seen_at_ns", Number(record.last_seen_at_ns, "last_seen_at_ns")},
      {"last_seen_at_wall", CanonicalValue(record.last_seen_at_wall)},
      {"lifecycle_state",
       CanonicalValue(std::string(ToString(record.lifecycle_state)))},
      {"message", CanonicalValue(record.message)},
      {"occurrence_count",
       Number(record.occurrence_count, "occurrence_count", true)},
      {"operator_response", CanonicalValue(record.operator_response)},
      {"priority", CanonicalValue(record.priority)},
      {"record_revision",
       Number(record.record_revision, "record_revision", true)},
      {"requires_motion_attention",
       CanonicalValue(record.requires_motion_attention)},
      {"returned_at_ns",
       OptionalNumber(record.returned_at_ns, "returned_at_ns")},
      {"returned_at_wall", OptionalString(record.returned_at_wall)},
      {"source_epoch", CanonicalValue(record.source_epoch)},
      {"source_id", CanonicalValue(record.source_id)},
      {"source_quality",
       CanonicalValue(std::string(ToString(record.source_quality)))},
      {"source_sequence", Number(record.source_sequence, "source_sequence")},
      {"subject", SubjectValue(record.subject)},
      {"title", CanonicalValue(record.title)},
  });
}

CanonicalValue AlarmLifecycleFactValue(const AlarmLifecycleFact &fact) {
  return CanonicalValue(CanonicalValue::Object{
      {"activation_id", OptionalString(fact.activation_id)},
      {"caller", OptionalCaller(fact.caller)},
      {"condition_id", OptionalString(fact.condition_id)},
      {"correlations", fact.correlations},
      {"current_state", StateValue(fact.current_state)},
      {"definition_id", OptionalString(fact.definition_id)},
      {"details", fact.details},
      {"event_type", CanonicalValue(fact.event_type)},
      {"occurred_at_ns", Number(fact.occurred_at_ns, "occurred_at_ns")},
      {"occurred_at_wall", CanonicalValue(fact.occurred_at_wall)},
      {"owner_epoch", CanonicalValue(fact.owner_epoch)},
      {"owner_generation", Number(fact.owner_generation, "owner_generation")},
      {"previous_state", StateValue(fact.previous_state)},
      {"reason", OptionalString(fact.reason)},
      {"record_revision",
       OptionalNumber(fact.record_revision, "record_revision")},
      {"source_epoch", OptionalString(fact.source_epoch)},
      {"source_id", OptionalString(fact.source_id)},
      {"source_sequence",
       OptionalNumber(fact.source_sequence, "source_sequence")},
      {"subject", fact.subject.has_value() ? SubjectValue(*fact.subject)
                                           : CanonicalValue(nullptr)},
  });
}

} // namespace manyforge::headless

#include "manyforge_headless/operator_alarm_owner.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iterator>
#include <memory>
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
constexpr std::size_t kMaximumReasonBytes = 4096U;
constexpr std::size_t kMaximumHealthMessageBytes = 4096U;
constexpr std::size_t kMetricSampleRetention = 4096U;
constexpr std::size_t kActivationSampleRetention = 4096U;
constexpr std::size_t kPerConditionActivationRetention = 64U;

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

void ValidateCaller(const AlarmCallerAttribution &caller) {
  ValidateText(caller.request_id, "request_id", kMaximumIdentityBytes);
  ValidateText(caller.request_owner_epoch, "request_owner_epoch",
               kMaximumIdentityBytes);
  ValidateText(caller.source_label, "source_label", kMaximumIdentityBytes);
  if (caller.correlation_id.has_value()) {
    ValidateText(*caller.correlation_id, "correlation_id",
                 kMaximumIdentityBytes);
  }
  ValidateText(caller.transport_kind, "transport_kind", kMaximumIdentityBytes);
  ValidateText(caller.adapter_id, "adapter_id", kMaximumIdentityBytes);
  ValidateText(caller.assurance, "assurance", kMaximumIdentityBytes);
  if (caller.trusted_principal_id.has_value()) {
    ValidateText(*caller.trusted_principal_id, "trusted_principal_id",
                 kMaximumIdentityBytes);
    Require(caller.assurance != "unverified", "operator_alarm_value_invalid",
            "trusted_principal_id requires adapter-stamped assurance");
  }
}

void Advance(std::uint64_t &value, const std::string_view label) {
  Require(value < kMaximumSafeInteger, "operator_alarm_identity_exhausted",
          std::string(label) + " is exhausted");
  ++value;
}

std::uint64_t SteadyNowNs() noexcept {
  return static_cast<std::uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

void Increment(std::map<std::string, std::uint64_t> &metrics,
               const std::string_view name,
               const std::uint64_t amount = 1U) noexcept {
  auto &value = metrics[std::string(name)];
  value = std::min(kMaximumSafeInteger,
                   value + std::min(amount, kMaximumSafeInteger - value));
}

int PriorityRank(const std::string_view priority) noexcept {
  if (priority == "critical") {
    return 0;
  }
  if (priority == "high") {
    return 1;
  }
  if (priority == "medium") {
    return 2;
  }
  return 3;
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label,
                      const bool nonzero = false) {
  ValidateSafeInteger(value, label, nonzero);
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  Require(value >= 0,
          "operator_alarm_value_invalid",
          std::string(label) + " must be a nonnegative int64");
  return content_identity::nonnegative_int64_value(value);
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
  return CanonicalValue(CanonicalValue::Object{
      {"resource_ids", StringArray(subject.resource_ids)},
      {"subject_id", CanonicalValue(subject.subject_id)},
      {"subject_kind", CanonicalValue(subject.subject_kind)},
  });
}

CanonicalValue MembershipValue(const AlarmAcknowledgementMember &member) {
  return CanonicalValue(CanonicalValue::Object{
      {"activation_id", CanonicalValue(member.activation_id)},
      {"condition_id", CanonicalValue(member.condition_id)},
      {"record_revision",
       Number(member.record_revision, "record_revision", true)},
  });
}

CanonicalValue
MembershipArray(const std::vector<AlarmAcknowledgementMember> &members) {
  CanonicalValue::Array result;
  result.reserve(members.size());
  for (const auto &member : members) {
    result.emplace_back(MembershipValue(member));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue CountsValue(const std::map<std::string, std::size_t> &counts) {
  CanonicalValue::Object result;
  result.reserve(counts.size());
  for (const auto &[name, count] : counts) {
    result.emplace_back(name,
                        Number(static_cast<std::uint64_t>(count), "count"));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue HealthValue(const std::map<std::string, std::string> &errors) {
  CanonicalValue::Object details;
  details.reserve(errors.size());
  for (const auto &[code, message] : errors) {
    details.emplace_back(code, CanonicalValue(message));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"details", CanonicalValue(std::move(details))},
      {"state", CanonicalValue(errors.empty() ? "available" : "unavailable")},
  });
}

CanonicalValue MotionMemberValue(const AlarmMotionAttentionMember &member) {
  return CanonicalValue(CanonicalValue::Object{
      {"activation_id", CanonicalValue(member.acknowledgement.activation_id)},
      {"condition_id", CanonicalValue(member.acknowledgement.condition_id)},
      {"definition_id", CanonicalValue(member.definition_id)},
      {"lifecycle_state",
       CanonicalValue(std::string(ToString(member.lifecycle_state)))},
      {"record_revision",
       Number(member.acknowledgement.record_revision, "record_revision", true)},
      {"subject", SubjectValue(member.subject)},
  });
}

std::shared_ptr<const AlarmMotionAttentionProjection> BuildMotionAttention(
    const std::string &cell_id, const std::string &owner_epoch,
    const manyforge::core::HostBootStamp &owner_incarnation,
    const std::uint64_t generation, const bool reconciled,
    const std::optional<AlarmResourceRegistry> &registry,
    const std::map<std::string, AlarmPointState> &points) {
  std::optional<AlarmConfigurationIdentity> active_configuration;
  if (registry.has_value()) {
    active_configuration = AlarmConfigurationIdentity{
        registry->retained_bundle_id, registry->graph_revision};
  }
  auto projection = std::make_shared<AlarmMotionAttentionProjection>(
      AlarmMotionAttentionProjection{cell_id, owner_epoch, owner_incarnation,
                                     generation, reconciled,
                                     std::move(active_configuration), {}});
  for (const auto &[condition_id, point] : points) {
    if (!point.record.has_value() || !GatesMotion(*point.record)) {
      continue;
    }
    projection->members.push_back(
        {AlarmAcknowledgementMember{condition_id, point.record->activation_id,
                                    point.record->record_revision},
         !point.record->acknowledgement_required, point.record->subject});
  }
  return projection;
}

} // namespace

void ValidateAlarmCallerAttribution(const AlarmCallerAttribution &caller) {
  ValidateCaller(caller);
}

AlarmLifecycleOwner::AlarmLifecycleOwner(
    OperatorAlarmCatalog catalog, std::string cell_id, std::string owner_id,
    std::string owner_epoch, manyforge::core::HostBootStamp owner_incarnation,
    MonotonicClock monotonic_clock, WallClock wall_clock,
    std::set<std::string> required_domains,
    const std::size_t terminal_retention, const std::size_t condition_capacity,
    const std::size_t live_alarm_capacity)
    : catalog_(std::move(catalog)), cell_id_(std::move(cell_id)),
      owner_id_(std::move(owner_id)), owner_epoch_(std::move(owner_epoch)),
      owner_incarnation_(std::move(owner_incarnation)),
      monotonic_clock_(std::move(monotonic_clock)),
      wall_clock_(std::move(wall_clock)),
      required_domains_(std::move(required_domains)),
      terminal_retention_(terminal_retention),
      condition_capacity_(condition_capacity),
      live_alarm_capacity_(live_alarm_capacity) {
  ValidateText(cell_id_, "cell_id", kMaximumIdentityBytes);
  ValidateText(owner_id_, "owner_id", kMaximumIdentityBytes);
  ValidateText(owner_epoch_, "owner_epoch", kMaximumIdentityBytes);
  ValidateText(owner_incarnation_.boot_id, "owner_host_boot_id",
               kMaximumIdentityBytes);
  Require(owner_incarnation_.boottime_us != 0U,
          "operator_alarm_configuration_invalid",
          "the owner incarnation must be a positive boot-time stamp");
  Require(static_cast<bool>(monotonic_clock_) && static_cast<bool>(wall_clock_),
          "operator_alarm_configuration_invalid",
          "both alarm-owner clocks are required");
  Require(!required_domains_.empty() &&
              std::none_of(required_domains_.begin(), required_domains_.end(),
                           [](const auto &domain) { return domain.empty(); }),
          "operator_alarm_configuration_invalid",
          "required source domains must be non-empty");
  Require(terminal_retention_ != 0U &&
              terminal_retention_ <= kDefaultTerminalAlarmRetention,
          "operator_alarm_configuration_invalid",
          "terminal retention must be between 1 and 4096");
  Require(condition_capacity_ != 0U && condition_capacity_ <= 100000U,
          "operator_alarm_configuration_invalid",
          "condition capacity is invalid");
  Require(live_alarm_capacity_ != 0U &&
              live_alarm_capacity_ <= kMaximumLiveAlarmCapacity &&
              live_alarm_capacity_ <= condition_capacity_,
          "operator_alarm_configuration_invalid",
          "live alarm capacity must be between 1 and 256 and cannot exceed "
          "condition capacity");
  static_cast<void>(MonotonicNow());
  static_cast<void>(WallNow());
  metrics_.insert_or_assign("owner_restarts", 1U);
  motion_attention_ = BuildMotionAttention(
      cell_id_, owner_epoch_, owner_incarnation_, generation_,
      ReconciledLocked(), resource_registry_, points_);
}

void AlarmLifecycleOwner::AdvanceGenerationLocked() {
  Advance(generation_, "owner generation");
  // Rebuilt under the owner lock, so it names exactly this generation's
  // state; only the pointer swap takes the publication lock.
  auto projection =
      BuildMotionAttention(cell_id_, owner_epoch_, owner_incarnation_,
                           generation_, ReconciledLocked(), resource_registry_,
                           points_);
  std::lock_guard publication(motion_attention_mutex_);
  motion_attention_ = std::move(projection);
}

void AlarmLifecycleOwner::ReplaceResourceRegistry(
    AlarmResourceRegistry registry) {
  ValidateText(registry.retained_bundle_id, "retained_bundle_id",
               kMaximumIdentityBytes);
  ValidateText(registry.graph_revision, "graph_revision",
               kMaximumIdentityBytes);
  Require(registry.resource_ids.size() <= kMaximumAlarmResourceRegistry,
          "operator_alarm_configuration_invalid",
          "resource registry exceeds the 4096-resource bound");
  for (const auto &resource : registry.resource_ids) {
    ValidateText(resource, "resource_id", kMaximumIdentityBytes);
  }
  std::lock_guard lock(mutex_);
  const bool identity_changed =
      !resource_registry_.has_value() ||
      resource_registry_->retained_bundle_id != registry.retained_bundle_id ||
      resource_registry_->graph_revision != registry.graph_revision;
  resource_registry_ = std::move(registry);
  Increment(metrics_, "resource_registry_replacements");
  if (identity_changed) {
    AdvanceGenerationLocked();
    Advance(state_revision_, "state revision");
  }
}

std::int64_t AlarmLifecycleOwner::MonotonicNow() const {
  const auto value = monotonic_clock_();
  Require(value >= 0,
          "operator_alarm_clock_invalid",
          "alarm monotonic clock returned an invalid value");
  return value;
}

std::string AlarmLifecycleOwner::WallNow() const {
  auto value = wall_clock_();
  ValidateText(value, "wall clock", 128U);
  return value;
}

void AlarmLifecycleOwner::RegisterSource(
    std::string domain, std::string source_id, std::string adapter_id,
    std::string source_epoch, const std::uint64_t source_epoch_order) {
  ValidateText(domain, "source domain", 128U);
  ValidateText(source_id, "source_id", kMaximumIdentityBytes);
  ValidateText(adapter_id, "adapter_id", kMaximumIdentityBytes);
  ValidateText(source_epoch, "source_epoch", kMaximumIdentityBytes);
  ValidateSafeInteger(source_epoch_order, "source_epoch_order");
  bool adapter_matches = false;
  for (const auto &definition : catalog_.definitions()) {
    if (definition.adapter_id == adapter_id &&
        definition.alarm_class == domain) {
      adapter_matches = true;
      break;
    }
  }
  Require(adapter_matches, "operator_alarm_source_adapter_unknown",
          "source adapter is not catalogued for this domain");

  std::lock_guard lock(mutex_);
  const auto found = sources_.find(source_id);
  if (found != sources_.end()) {
    Require(found->second.domain == domain &&
                found->second.adapter_id == adapter_id,
            "operator_alarm_source_registration_conflict",
            "source changed its domain or adapter authority");
    Require(source_epoch_order >= found->second.source_epoch_order,
            "operator_alarm_source_epoch_stale",
            "source registered an older epoch");
    if (source_epoch_order == found->second.source_epoch_order) {
      Require(source_epoch == found->second.source_epoch,
              "operator_alarm_source_epoch_conflict",
              "source epoch order was reused");
      return;
    }
  }
  const auto source_key = source_id;
  sources_.insert_or_assign(
      source_key,
      SourceRegistration{std::move(domain), std::move(source_id),
                         std::move(adapter_id), std::move(source_epoch),
                         source_epoch_order, AlarmConditionQuality::kUnknown,
                         false});
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
}

void AlarmLifecycleOwner::CompleteSourceSnapshot(
    const std::string_view source_id, const std::string_view source_epoch,
    const AlarmConditionQuality quality) {
  std::lock_guard lock(mutex_);
  const auto found = sources_.find(std::string(source_id));
  Require(found != sources_.end(), "operator_alarm_source_unregistered",
          "source snapshot belongs to an unregistered source");
  Require(found->second.source_epoch == source_epoch,
          "operator_alarm_source_epoch_stale",
          "source snapshot belongs to a stale epoch");
  if (found->second.snapshot_complete && found->second.quality == quality) {
    return;
  }
  found->second.snapshot_complete = true;
  found->second.quality = quality;
  if (quality == AlarmConditionQuality::kUnknown) {
    Increment(metrics_, "source_quality_unknown_observations");
  }
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
}

void AlarmLifecycleOwner::MarkSourceQualityUnknown(
    const std::string_view source_id, const std::string_view source_epoch) {
  std::lock_guard lock(mutex_);
  const auto found = sources_.find(std::string(source_id));
  Require(found != sources_.end(), "operator_alarm_source_unregistered",
          "quality update belongs to an unregistered source");
  Require(found->second.source_epoch == source_epoch,
          "operator_alarm_source_epoch_stale",
          "quality update belongs to a stale epoch");
  if (!found->second.snapshot_complete &&
      found->second.quality == AlarmConditionQuality::kUnknown) {
    return;
  }
  found->second.snapshot_complete = false;
  found->second.quality = AlarmConditionQuality::kUnknown;
  Increment(metrics_, "source_quality_unknown_observations");
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
}

void AlarmLifecycleOwner::EnsureConditionCapacityLocked(
    const std::string_view condition_id) {
  if (points_.find(std::string(condition_id)) != points_.end()) {
    return;
  }
  while (points_.size() >= condition_capacity_) {
    const auto removable =
        std::find_if(points_.begin(), points_.end(), [](const auto &entry) {
          return !entry.second.record.has_value() ||
                 entry.second.record->lifecycle_state ==
                     AlarmLifecycleState::kReturnedAcknowledged;
        });
    if (removable == points_.end()) {
      health_errors_.insert_or_assign(
          "operator_alarm_condition_capacity_exhausted",
          "condition capacity is exhausted; existing live attention was "
          "retained");
      AdvanceGenerationLocked();
      Advance(state_revision_, "state revision");
      AlarmFail("operator_alarm_condition_capacity_exhausted",
                "condition capacity is exhausted");
    }
    condition_activations_ns_.erase(removable->first);
    points_.erase(removable);
  }
}

void AlarmLifecycleOwner::EnsureLiveAlarmCapacityLocked(
    const AlarmPointState &current, const AlarmPointState &candidate) {
  const auto is_live = [](const AlarmPointState &point) {
    return point.record.has_value() &&
           point.record->lifecycle_state !=
               AlarmLifecycleState::kReturnedAcknowledged;
  };
  if (is_live(current) || !is_live(candidate)) {
    return;
  }
  const auto live = static_cast<std::size_t>(std::count_if(
      points_.begin(), points_.end(),
      [&is_live](const auto &entry) { return is_live(entry.second); }));
  if (live < live_alarm_capacity_) {
    return;
  }
  health_errors_.insert_or_assign(
      "operator_alarm_condition_capacity_exhausted",
      "live alarm capacity is exhausted; existing live attention was "
      "retained");
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
  AlarmFail("operator_alarm_condition_capacity_exhausted",
            "live alarm capacity is exhausted");
}

void AlarmLifecycleOwner::AppendTerminalLocked(
    const OperatorAlarmRecord &record) {
  terminal_records_.push_back(record);
  while (terminal_records_.size() > terminal_retention_) {
    terminal_records_.pop_front();
  }
}

void AlarmLifecycleOwner::StampFactsLocked(
    std::vector<AlarmLifecycleFact> &facts) const {
  for (auto &fact : facts) {
    fact.owner_generation = generation_;
    fact.owner_epoch = owner_epoch_;
  }
}

void AlarmLifecycleOwner::RecordRejectedUpdateLocked(
    const std::string_view code) {
  Increment(metrics_, "rejected_updates");
  if (code == "operator_alarm_scope_invalid" ||
      code == "operator_alarm_scope_inactive") {
    Increment(metrics_, "scope_rejections");
  }
  if (code == "operator_alarm_source_epoch_stale" ||
      code == "operator_alarm_source_sequence_stale") {
    Increment(metrics_, "stale_updates");
  }
  if (code == "operator_alarm_source_epoch_stale") {
    Increment(metrics_, "stale_epoch_updates");
  }
  if (code == "operator_alarm_source_sequence_stale") {
    Increment(metrics_, "out_of_order_sequence_updates");
  }
  if (code == "operator_alarm_source_epoch_conflict" ||
      code == "operator_alarm_source_sequence_conflict" ||
      code == "operator_alarm_source_authority_conflict") {
    Increment(metrics_, "conflicting_updates");
  }
  if (code == "operator_alarm_condition_capacity_exhausted") {
    Increment(metrics_, "capacity_rejections");
  }
}

void AlarmLifecycleOwner::RecordIngestLatencyLocked(
    const std::uint64_t started_ns) {
  const auto completed = SteadyNowNs();
  const auto duration = completed >= started_ns ? completed - started_ns : 0U;
  ingest_latency_ns_.push_back(std::min(duration, kMaximumSafeInteger));
  while (ingest_latency_ns_.size() > kMetricSampleRetention) {
    ingest_latency_ns_.pop_front();
  }
}

AlarmMutation AlarmLifecycleOwner::Submit(AlarmConditionUpdate update) {
  const auto started_ns = SteadyNowNs();
  try {
    const auto received_at_ns = MonotonicNow();
    return SubmitImpl(std::move(update), started_ns, received_at_ns);
  } catch (const OperatorAlarmError &error) {
    std::lock_guard lock(mutex_);
    RecordRejectedUpdateLocked(error.code());
    RecordIngestLatencyLocked(started_ns);
    throw;
  }
}

AlarmMutation
AlarmLifecycleOwner::SubmitImpl(AlarmConditionUpdate update,
                                const std::uint64_t ingest_started_ns,
                                const std::int64_t received_at_ns) {
  const auto *definition = catalog_.Find(update.definition_id);
  Require(definition != nullptr, "operator_alarm_definition_unknown",
          "condition update names an unknown definition");
  const auto condition_id =
      CanonicalAlarmConditionId(catalog_, *definition, update.subject);
  Require(update.subject.subject_kind != "cell" ||
              (update.subject.subject_id == cell_id_ &&
               update.subject.resource_ids.empty()),
          "operator_alarm_scope_invalid", "cell-global alarm scope is invalid");
  if (update.subject.subject_kind != "cell" &&
      definition->requires_motion_attention) {
    Require(!update.subject.resource_ids.empty(),
            "operator_alarm_scope_invalid",
            "motion-attention scope requires canonical resources");
  }

  std::lock_guard lock(mutex_);
  const auto found = points_.find(condition_id);
  // A resource-scoped activation must name the exact active configuration.
  // A return to normal of an already retained condition is accepted after
  // its resources leave the configuration, so a replaced configuration
  // neither erases nor strands the alarm it once admitted.
  std::vector<std::string> inactive_resources;
  for (const auto &resource : update.subject.resource_ids) {
    if (!resource_registry_.has_value() ||
        !resource_registry_->resource_ids.contains(resource)) {
      inactive_resources.push_back(resource);
    }
  }
  if (!inactive_resources.empty() &&
      (update.active || found == points_.end())) {
    std::string message =
        "alarm scope names resources outside the active configuration (";
    message += resource_registry_.has_value()
                   ? "retained bundle " + resource_registry_->retained_bundle_id
                   : std::string("no active configuration observed");
    message += "):";
    for (const auto &resource : inactive_resources) {
      message += " " + resource;
    }
    AlarmFail("operator_alarm_scope_inactive", message);
  }
  const auto source = sources_.find(update.source_id);
  Require(source != sources_.end(), "operator_alarm_source_unregistered",
          "condition update source is unregistered");
  Require(source->second.adapter_id == definition->adapter_id,
          "operator_alarm_source_adapter_mismatch",
          "source adapter cannot update this definition");
  Require(source->second.source_epoch == update.source_epoch,
          "operator_alarm_source_epoch_stale",
          "condition update belongs to a stale registered epoch");
  const auto point =
      found == points_.end()
          ? AlarmPointState{condition_id, 0U, std::nullopt, std::nullopt}
          : found->second;
  const auto activation_id =
      owner_epoch_ + "-activation-" + std::to_string(activation_serial_ + 1U);
  auto transition = ReduceAlarmConditionUpdate(
      catalog_, *definition, point, update, source->second.source_epoch_order,
      activation_id);
  if (!transition.changed) {
    Increment(metrics_, "duplicate_updates");
    RecordIngestLatencyLocked(ingest_started_ns);
    return {transition.status,
            false,
            generation_,
            state_revision_,
            transition.point.record.has_value()
                ? std::vector<OperatorAlarmRecord>{*transition.point.record}
                : std::vector<OperatorAlarmRecord>{},
            {}};
  }
  // Validate and reduce before capacity housekeeping. A rejected new
  // condition must not evict a terminal cursor and thereby weaken stale or
  // conflicting-sequence detection for that existing condition.
  EnsureLiveAlarmCapacityLocked(point, transition.point);
  EnsureConditionCapacityLocked(condition_id);
  points_.insert_or_assign(condition_id, transition.point);
  if (transition.activation_changed) {
    Advance(activation_serial_, "activation serial");
  }
  if (transition.terminal_record.has_value()) {
    AppendTerminalLocked(*transition.terminal_record);
  }
  bool reconciliation_changed = false;
  if (update.quality == AlarmConditionQuality::kUnknown) {
    reconciliation_changed = source->second.snapshot_complete ||
                             source->second.quality !=
                                 AlarmConditionQuality::kUnknown;
    source->second.quality = AlarmConditionQuality::kUnknown;
    source->second.snapshot_complete = false;
    Increment(metrics_, "source_quality_unknown_observations");
  }
  Increment(metrics_, "accepted_updates");
  if (transition.status == "activated" || transition.status == "recurred") {
    Increment(metrics_, "activations");
    recent_activations_ns_.push_back(received_at_ns);
    while (recent_activations_ns_.size() > kActivationSampleRetention) {
      recent_activations_ns_.pop_front();
    }
    auto &condition_activations = condition_activations_ns_[condition_id];
    condition_activations.push_back(received_at_ns);
    while (condition_activations.size() > kPerConditionActivationRetention) {
      condition_activations.pop_front();
    }
    if (transition.status == "recurred") {
      Increment(metrics_, "recurrences");
    }
  } else if (transition.status == "returned") {
    Increment(metrics_, "returns");
    if (point.record.has_value() &&
        point.record->lifecycle_state ==
            AlarmLifecycleState::kActiveAcknowledged &&
        point.record->acknowledged_at_ns.has_value()) {
      const auto duration =
          update.observed_at_ns >= *point.record->acknowledged_at_ns
              ? static_cast<std::uint64_t>(update.observed_at_ns -
                                           *point.record->acknowledged_at_ns)
              : 0U;
      completed_standing_duration_ns_.push_back(duration);
      while (completed_standing_duration_ns_.size() > kMetricSampleRetention) {
        completed_standing_duration_ns_.pop_front();
      }
    }
  } else if (transition.status == "active_repeat") {
    Increment(metrics_, "repeat_occurrences");
  }
  auto &peak = metrics_["peak_live_condition_points"];
  peak = std::max(peak, static_cast<std::uint64_t>(points_.size()));
  // A fresh inactive observation with no live record advances only the
  // source cursor. It cannot change acknowledgement membership or motion
  // attention, so invalidating the public membership generation here would
  // make a human-confirmed acknowledge-all race harmless status polling.
  // An unknown-quality observation does change reconciliation, which the
  // published motion-attention projection carries under a new generation.
  if (transition.status != "normal_observed" || reconciliation_changed) {
    AdvanceGenerationLocked();
  }
  Advance(state_revision_, "state revision");
  StampFactsLocked(transition.lifecycle_facts);
  RecordIngestLatencyLocked(ingest_started_ns);
  return {
      transition.status,
      true,
      generation_,
      state_revision_,
      transition.point.record.has_value()
          ? std::vector<OperatorAlarmRecord>{*transition.point.record}
          : std::vector<OperatorAlarmRecord>{},
      std::move(transition.lifecycle_facts),
  };
}

AlarmMutation AlarmLifecycleOwner::Acknowledge(
    const std::string_view condition_id, const std::string_view activation_id,
    const std::uint64_t expected_record_revision,
    const AlarmCallerAttribution &caller, std::optional<std::string> reason,
    const bool confirmed) {
  try {
    return AcknowledgeImpl(condition_id, activation_id,
                           expected_record_revision, caller, std::move(reason),
                           confirmed);
  } catch (const OperatorAlarmError &) {
    std::lock_guard lock(mutex_);
    Increment(metrics_, "acknowledgement_conflicts");
    throw;
  }
}

AlarmMutation AlarmLifecycleOwner::AcknowledgeImpl(
    const std::string_view condition_id, const std::string_view activation_id,
    const std::uint64_t expected_record_revision,
    const AlarmCallerAttribution &caller, std::optional<std::string> reason,
    const bool confirmed) {
  Require(confirmed, "operator_alarm_acknowledgement_confirmation_required",
          "alarm acknowledgement requires confirmation");
  ValidateCaller(caller);
  if (reason.has_value()) {
    ValidateText(*reason, "reason", kMaximumReasonBytes);
  }
  const auto acknowledged_at_ns = MonotonicNow();
  auto acknowledged_at_wall = WallNow();
  std::lock_guard lock(mutex_);
  const auto found = points_.find(std::string(condition_id));
  Require(found != points_.end() && found->second.record.has_value(),
          "operator_alarm_not_found", "alarm condition is not live");
  const auto *definition = catalog_.Find(found->second.record->definition_id);
  Require(definition != nullptr, "operator_alarm_catalog_unavailable",
          "live alarm definition is missing from the catalog");
  auto transition = ReduceAlarmAcknowledgement(
      *definition, found->second, activation_id, expected_record_revision,
      caller, reason, acknowledged_at_ns, std::move(acknowledged_at_wall));
  if (!transition.changed) {
    return {transition.status,       false, generation_, state_revision_,
            {*found->second.record}, {}};
  }
  found->second = transition.point;
  if (transition.terminal_record.has_value()) {
    AppendTerminalLocked(*transition.terminal_record);
  }
  Increment(metrics_, "acknowledgements");
  const auto latency =
      acknowledged_at_ns >= found->second.record->activated_at_ns
          ? static_cast<std::uint64_t>(acknowledged_at_ns -
                                       found->second.record->activated_at_ns)
          : 0U;
  acknowledgement_latency_ns_.push_back(latency);
  while (acknowledgement_latency_ns_.size() > kMetricSampleRetention) {
    acknowledgement_latency_ns_.pop_front();
  }
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
  StampFactsLocked(transition.lifecycle_facts);
  return {transition.status,
          true,
          generation_,
          state_revision_,
          {*transition.point.record},
          std::move(transition.lifecycle_facts)};
}

std::vector<AlarmAcknowledgementMember>
AlarmLifecycleOwner::UnacknowledgedMembershipLocked(
    const bool motion_only) const {
  std::vector<AlarmAcknowledgementMember> result;
  for (const auto &[condition_id, point] : points_) {
    if (!point.record.has_value() ||
        !RequiresAttention(point.record->lifecycle_state) ||
        (motion_only && !point.record->requires_motion_attention)) {
      continue;
    }
    result.push_back({condition_id, point.record->activation_id,
                      point.record->record_revision});
  }
  std::sort(result.begin(), result.end());
  return result;
}

AlarmMutation AlarmLifecycleOwner::AcknowledgeAll(
    std::vector<AlarmAcknowledgementMember> members,
    const std::string_view expected_owner_epoch,
    const std::uint64_t expected_generation,
    const AlarmCallerAttribution &caller, std::optional<std::string> reason,
    const bool confirmed) {
  try {
    return AcknowledgeAllImpl(std::move(members), expected_owner_epoch,
                              expected_generation, caller, std::move(reason),
                              confirmed);
  } catch (const OperatorAlarmError &) {
    std::lock_guard lock(mutex_);
    Increment(metrics_, "acknowledgement_conflicts");
    throw;
  }
}

AlarmMutation AlarmLifecycleOwner::AcknowledgeAllImpl(
    std::vector<AlarmAcknowledgementMember> members,
    const std::string_view expected_owner_epoch,
    const std::uint64_t expected_generation,
    const AlarmCallerAttribution &caller, std::optional<std::string> reason,
    const bool confirmed) {
  Require(confirmed, "operator_alarm_acknowledge_all_confirmation_required",
          "acknowledge-all requires confirmation");
  ValidateCaller(caller);
  Require(members.size() <= kMaximumAlarmBulkMembers,
          "operator_alarm_acknowledge_all_members_invalid",
          "acknowledge-all membership exceeds the 256-member bound");
  if (reason.has_value()) {
    ValidateText(*reason, "reason", kMaximumReasonBytes);
  }
  std::sort(members.begin(), members.end());
  Require(std::adjacent_find(members.begin(), members.end()) == members.end(),
          "operator_alarm_acknowledge_all_duplicate_member",
          "acknowledge-all membership contains a duplicate");
  const auto acknowledged_at_ns = MonotonicNow();
  auto acknowledged_at_wall = WallNow();

  std::lock_guard lock(mutex_);
  Require(expected_owner_epoch == owner_epoch_,
          "operator_alarm_owner_epoch_stale",
          "alarm owner restarted before acknowledgement");
  Require(expected_generation == generation_,
          "operator_alarm_owner_generation_stale",
          "alarm membership changed before acknowledgement");
  const auto current = UnacknowledgedMembershipLocked(false);
  Require(members == current, "operator_alarm_membership_mismatch",
          "acknowledge-all must name the exact current membership");
  if (current.empty()) {
    return {"already_empty", false, generation_, state_revision_, {}, {}};
  }

  std::vector<AlarmTransitionResult> transitions;
  transitions.reserve(current.size());
  for (const auto &member : current) {
    const auto point = points_.find(member.condition_id);
    const auto *definition = catalog_.Find(point->second.record->definition_id);
    Require(definition != nullptr, "operator_alarm_catalog_unavailable",
            "live alarm definition is missing from the catalog");
    transitions.push_back(ReduceAlarmAcknowledgement(
        *definition, point->second, member.activation_id,
        member.record_revision, caller, reason, acknowledged_at_ns,
        acknowledged_at_wall));
  }
  for (std::size_t index = 0U; index < current.size(); ++index) {
    auto &transition = transitions[index];
    points_.find(current[index].condition_id)->second = transition.point;
    if (transition.terminal_record.has_value()) {
      AppendTerminalLocked(*transition.terminal_record);
    }
  }
  Increment(metrics_, "acknowledgements",
            static_cast<std::uint64_t>(transitions.size()));
  Increment(metrics_, "bulk_acknowledgements");
  for (const auto &transition : transitions) {
    if (!transition.point.record.has_value()) {
      continue;
    }
    const auto latency =
        acknowledged_at_ns >= transition.point.record->activated_at_ns
            ? static_cast<std::uint64_t>(
                  acknowledged_at_ns - transition.point.record->activated_at_ns)
            : 0U;
    acknowledgement_latency_ns_.push_back(latency);
  }
  while (acknowledgement_latency_ns_.size() > kMetricSampleRetention) {
    acknowledgement_latency_ns_.pop_front();
  }
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");

  std::vector<AlarmLifecycleFact> facts;
  std::vector<OperatorAlarmRecord> records;
  for (auto &transition : transitions) {
    records.push_back(*transition.point.record);
    facts.insert(facts.end(),
                 std::make_move_iterator(transition.lifecycle_facts.begin()),
                 std::make_move_iterator(transition.lifecycle_facts.end()));
  }
  const auto membership_digest = content_identity::sha256_digest(
      content_identity::canonical_json(MembershipArray(current)));
  facts.push_back(AlarmLifecycleFact{
      "operator_alarms_acknowledged",
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      generation_,
      owner_epoch_,
      acknowledged_at_ns,
      acknowledged_at_wall,
      std::nullopt,
      std::nullopt,
      std::nullopt,
      caller,
      reason,
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Object{
          {"expected_generation",
           Number(expected_generation, "expected_generation")},
          {"expected_owner_epoch",
           CanonicalValue(std::string(expected_owner_epoch))},
          {"member_count",
           Number(static_cast<std::uint64_t>(current.size()), "member_count")},
          {"members", MembershipArray(current)},
          {"membership_digest", CanonicalValue(membership_digest)},
      }),
  });
  StampFactsLocked(facts);
  return {"acknowledged_all", true,
          generation_,        state_revision_,
          std::move(records), std::move(facts)};
}

bool AlarmLifecycleOwner::ReconciledLocked() const {
  if (!health_errors_.empty()) {
    return false;
  }
  for (const auto &domain : required_domains_) {
    bool saw_source = false;
    for (const auto &[source_id, source] : sources_) {
      static_cast<void>(source_id);
      if (source.domain != domain) {
        continue;
      }
      saw_source = true;
      if (!source.snapshot_complete ||
          source.quality != AlarmConditionQuality::kGood) {
        return false;
      }
    }
    if (!saw_source) {
      return false;
    }
  }
  return true;
}

OperatorAlarmSnapshot
AlarmLifecycleOwner::Snapshot(const std::optional<std::size_t> limit,
                              const std::set<AlarmLifecycleState> &states,
                              const AlarmSnapshotOrdering ordering) const {
  if (limit.has_value()) {
    Require(*limit <= kMaximumAlarmSnapshotPage, "operator_alarm_limit_invalid",
            "alarm page limit exceeds 128");
  }
  const auto timestamp_ns = MonotonicNow();
  std::lock_guard lock(mutex_);
  std::vector<OperatorAlarmRecord> live;
  std::vector<AlarmMotionAttentionMember> motion_membership;
  std::map<std::string, std::size_t> counts_by_state{
      {"active_acknowledged", 0U},
      {"active_unacknowledged", 0U},
      {"returned_acknowledged", 0U},
      {"returned_unacknowledged", 0U},
  };
  std::map<std::string, std::size_t> counts_by_priority{
      {"critical", 0U}, {"high", 0U}, {"low", 0U}, {"medium", 0U}};
  std::map<std::string, std::size_t> counts_by_class;
  std::size_t active_count = 0U;
  std::size_t active_acknowledged_count = 0U;
  for (const auto &[condition_id, point] : points_) {
    if (!point.record.has_value() ||
        point.record->lifecycle_state ==
            AlarmLifecycleState::kReturnedAcknowledged) {
      continue;
    }
    const auto &record = *point.record;
    live.push_back(record);
    ++counts_by_state[std::string(ToString(record.lifecycle_state))];
    ++counts_by_priority[record.priority];
    ++counts_by_class[record.alarm_class];
    active_count += IsActive(record.lifecycle_state) ? 1U : 0U;
    active_acknowledged_count +=
        record.lifecycle_state == AlarmLifecycleState::kActiveAcknowledged ? 1U
                                                                           : 0U;
    if (GatesMotion(record)) {
      motion_membership.push_back({
          AlarmAcknowledgementMember{condition_id, record.activation_id,
                                     record.record_revision},
          record.lifecycle_state,
          record.definition_id,
          record.subject,
      });
    }
  }
  std::sort(
      live.begin(), live.end(),
      [ordering](const auto &left, const auto &right) {
        if (ordering == AlarmSnapshotOrdering::kAttentionNewest) {
          return std::tuple(!RequiresAttention(left.lifecycle_state),
                            -left.activated_at_ns, PriorityRank(left.priority),
                            left.condition_id) <
                 std::tuple(!RequiresAttention(right.lifecycle_state),
                            -right.activated_at_ns,
                            PriorityRank(right.priority), right.condition_id);
        }
        return std::tuple(!RequiresAttention(left.lifecycle_state),
                          PriorityRank(left.priority), -left.activated_at_ns,
                          left.condition_id) <
               std::tuple(!RequiresAttention(right.lifecycle_state),
                          PriorityRank(right.priority), -right.activated_at_ns,
                          right.condition_id);
      });
  const auto membership = UnacknowledgedMembershipLocked(false);
  std::vector<OperatorAlarmRecord> filtered;
  for (const auto &record : live) {
    if (states.empty() || states.find(record.lifecycle_state) != states.end()) {
      filtered.push_back(record);
    }
  }
  const auto effective_limit =
      limit.has_value() ? std::min(*limit, filtered.size()) : filtered.size();
  const bool truncated = effective_limit < filtered.size();
  filtered.resize(effective_limit);

  std::vector<AlarmSourceDomainStatus> domains;
  auto domain_names = required_domains_;
  for (const auto &[source_id, source] : sources_) {
    static_cast<void>(source_id);
    domain_names.insert(source.domain);
  }
  for (const auto &domain : domain_names) {
    std::size_t source_count = 0U;
    std::size_t unknown_count = 0U;
    bool reconciled = true;
    for (const auto &[source_id, source] : sources_) {
      static_cast<void>(source_id);
      if (source.domain != domain) {
        continue;
      }
      ++source_count;
      if (!source.snapshot_complete ||
          source.quality != AlarmConditionQuality::kGood) {
        ++unknown_count;
        reconciled = false;
      }
    }
    reconciled = reconciled && source_count != 0U;
    domains.push_back(
        {domain, required_domains_.find(domain) != required_domains_.end(),
         reconciled, source_count, unknown_count});
  }
  return {
      catalog_.schema_version(),
      catalog_.catalog_version(),
      catalog_.descriptor_digest(),
      owner_id_,
      owner_epoch_,
      generation_,
      state_revision_,
      ReconciledLocked(),
      health_errors_,
      std::move(domains),
      live.size(),
      membership.size(),
      motion_membership.size(),
      active_count,
      active_acknowledged_count,
      membership,
      std::move(motion_membership),
      std::move(counts_by_state),
      std::move(counts_by_priority),
      std::move(counts_by_class),
      std::move(filtered),
      truncated,
      timestamp_ns,
  };
}

std::optional<AlarmPointState>
AlarmLifecycleOwner::ConditionPoint(const std::string_view definition_id,
                                    const AlarmSubject &subject) const {
  const auto *definition = catalog_.Find(definition_id);
  Require(definition != nullptr, "operator_alarm_definition_unknown",
          "condition lookup names an unknown definition");
  const auto condition_id =
      CanonicalAlarmConditionId(catalog_, *definition, subject);
  std::lock_guard lock(mutex_);
  const auto found = points_.find(condition_id);
  return found == points_.end() ? std::nullopt : std::optional(found->second);
}
std::vector<OperatorAlarmRecord> AlarmLifecycleOwner::TerminalRecords(
    const std::optional<std::size_t> limit) const {
  if (limit.has_value()) {
    Require(*limit <= terminal_retention_, "operator_alarm_limit_invalid",
            "terminal record limit exceeds retention");
  }
  std::lock_guard lock(mutex_);
  const auto count = limit.has_value()
                         ? std::min(*limit, terminal_records_.size())
                         : terminal_records_.size();
  std::vector<OperatorAlarmRecord> result;
  result.reserve(count);
  for (std::size_t offset = 0U; offset < count; ++offset) {
    result.push_back(terminal_records_[terminal_records_.size() - 1U - offset]);
  }
  return result;
}

std::shared_ptr<const AlarmMotionAttentionProjection>
AlarmLifecycleOwner::MotionAttentionProjection() const {
  std::lock_guard publication(motion_attention_mutex_);
  return motion_attention_;
}

void AlarmLifecycleOwner::MarkHealthUnavailable(std::string code,
                                                std::string message) {
  ValidateText(code, "health code", 256U);
  ValidateText(message, "health message", kMaximumHealthMessageBytes);
  std::lock_guard lock(mutex_);
  MarkHealthUnavailableLocked(std::move(code), std::move(message));
}

void AlarmLifecycleOwner::MarkHealthUnavailableIf(
    std::string code, std::string message,
    const std::function<bool()> &still_unavailable) {
  ValidateText(code, "health code", 256U);
  ValidateText(message, "health message", kMaximumHealthMessageBytes);
  std::lock_guard lock(mutex_);
  // Decided in the same critical section as `ReconcileHealth`, so a fact
  // reconciled concurrently is never re-marked from a stale observation.
  if (still_unavailable()) {
    MarkHealthUnavailableLocked(std::move(code), std::move(message));
  }
}

void AlarmLifecycleOwner::MarkHealthUnavailableLocked(std::string code,
                                                      std::string message) {
  // Only a new or changed health fact advances the generation; restating
  // the same fact must not invalidate generation-fenced acknowledgements.
  const auto existing = health_errors_.find(code);
  if (existing != health_errors_.end() && existing->second == message) {
    return;
  }
  if (code.find("historian") != std::string::npos) {
    Increment(metrics_, "historian_delivery_failures");
  }
  health_errors_.insert_or_assign(std::move(code), std::move(message));
  AdvanceGenerationLocked();
  Advance(state_revision_, "state revision");
}

void AlarmLifecycleOwner::ReconcileHealth(const std::string_view code) {
  std::lock_guard lock(mutex_);
  if (health_errors_.erase(std::string(code)) != 0U) {
    if (code.find("historian") != std::string_view::npos) {
      Increment(metrics_, "historian_delivery_recoveries");
    }
    AdvanceGenerationLocked();
    Advance(state_revision_, "state revision");
  }
}

CanonicalValue OperatorAlarmMotionAttentionValue(
    const AlarmMotionAttentionProjection &projection) {
  CanonicalValue::Array members;
  members.reserve(projection.members.size());
  for (const auto &member : projection.members) {
    members.emplace_back(CanonicalValue::Object{
        {"activation_id",
         CanonicalValue(member.acknowledgement.activation_id)},
        {"condition_id", CanonicalValue(member.acknowledgement.condition_id)},
        {"gate", CanonicalValue(member.condition_interlock
                                    ? "condition_interlock"
                                    : "acknowledgement")},
        {"record_revision",
         Number(member.acknowledgement.record_revision, "record_revision",
                true)},
        {"subject", SubjectValue(member.subject)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_configuration",
       projection.active_configuration.has_value()
           ? CanonicalValue(CanonicalValue::Object{
                 {"graph_revision",
                  CanonicalValue(projection.active_configuration->graph_revision)},
                 {"retained_bundle_id",
                  CanonicalValue(
                      projection.active_configuration->retained_bundle_id)},
             })
           : CanonicalValue(nullptr)},
      {"cell_id", CanonicalValue(projection.cell_id)},
      {"generation", Number(projection.generation, "generation")},
      {"members", CanonicalValue(std::move(members))},
      {"owner_epoch", CanonicalValue(projection.owner_epoch)},
      {"owner_host_boot_id",
       CanonicalValue(projection.owner_incarnation.boot_id)},
      {"owner_incarnation_us",
       Number(projection.owner_incarnation.boottime_us, "owner_incarnation_us",
              true)},
      {"reconciled", CanonicalValue(projection.reconciled)},
      {"schema_version",
       CanonicalValue(std::string(kOperatorAlarmMotionAttentionApiVersion))},
  });
}

CanonicalValue
OperatorAlarmSnapshotValue(const OperatorAlarmSnapshot &snapshot) {
  CanonicalValue::Array domains;
  domains.reserve(snapshot.domains.size());
  for (const auto &domain : snapshot.domains) {
    domains.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"domain", CanonicalValue(domain.domain)},
        {"reconciled", CanonicalValue(domain.reconciled)},
        {"required", CanonicalValue(domain.required)},
        {"source_count", Number(static_cast<std::uint64_t>(domain.source_count),
                                "source_count")},
        {"unknown_source_count",
         Number(static_cast<std::uint64_t>(domain.unknown_source_count),
                "unknown_source_count")},
    }));
  }
  CanonicalValue::Array alarms;
  alarms.reserve(snapshot.alarms.size());
  for (const auto &record : snapshot.alarms) {
    alarms.emplace_back(OperatorAlarmRecordValue(record));
  }
  CanonicalValue::Array motion_membership;
  motion_membership.reserve(snapshot.motion_attention_membership.size());
  for (const auto &member : snapshot.motion_attention_membership) {
    motion_membership.emplace_back(MotionMemberValue(member));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active_acknowledged_count",
       Number(static_cast<std::uint64_t>(snapshot.active_acknowledged_count),
              "active_acknowledged_count")},
      {"active_count", Number(static_cast<std::uint64_t>(snapshot.active_count),
                              "active_count")},
      {"alarms", CanonicalValue(std::move(alarms))},
      {"catalog_descriptor_digest",
       CanonicalValue(snapshot.catalog_descriptor_digest)},
      {"catalog_schema_version",
       CanonicalValue(snapshot.catalog_schema_version)},
      {"catalog_version", CanonicalValue(snapshot.catalog_version)},
      {"counts_by_class", CountsValue(snapshot.counts_by_class)},
      {"counts_by_priority", CountsValue(snapshot.counts_by_priority)},
      {"counts_by_state", CountsValue(snapshot.counts_by_state)},
      {"domains", CanonicalValue(std::move(domains))},
      {"generation", Number(snapshot.generation, "generation")},
      {"health", HealthValue(snapshot.health_errors)},
      {"live_count",
       Number(static_cast<std::uint64_t>(snapshot.live_count), "live_count")},
      {"motion_attention_count",
       Number(static_cast<std::uint64_t>(snapshot.motion_attention_count),
              "motion_attention_count")},
      {"motion_attention_membership",
       CanonicalValue(std::move(motion_membership))},
      {"owner_epoch", CanonicalValue(snapshot.owner_epoch)},
      {"owner_id", CanonicalValue(snapshot.owner_id)},
      {"reconciled", CanonicalValue(snapshot.reconciled)},
      {"schema_version",
       CanonicalValue(std::string(kOperatorAlarmSnapshotApiVersion))},
      {"state_revision", Number(snapshot.state_revision, "state_revision")},
      {"timestamp_ns", Number(snapshot.timestamp_ns, "timestamp_ns")},
      {"truncated", CanonicalValue(snapshot.truncated)},
      {"unacknowledged_count",
       Number(static_cast<std::uint64_t>(snapshot.unacknowledged_count),
              "unacknowledged_count")},
      {"unacknowledged_membership",
       MembershipArray(snapshot.unacknowledged_membership)},
  });
}

} // namespace manyforge::headless

#include <algorithm>
#include <charconv>
#include <cmath>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_core/types/component_incident.hpp>
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

[[nodiscard]] CanonicalValue DiagnosticValue(const ErrorDTO& diagnostic) {
  CanonicalValue::Object details;
  for (const auto& [key, value] : diagnostic.details)
    details.emplace_back(key, CanonicalValue(value));
  return CanonicalValue(CanonicalValue::Object{{"code", CanonicalValue(diagnostic.code)},
                                               {"message", CanonicalValue(diagnostic.message)},
                                               {"details", CanonicalValue(std::move(details))}});
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
  return CanonicalValue(
      CanonicalValue::Object{{"at_ns", NonNegative(at_ns)},
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

/// Shared Q86 exit/output contract; observer and component captures use the
/// same actual wait evidence and encoded-tail limits.
[[nodiscard]] bool IncidentExitAndOutputShapeIsValid(const CanonicalValue& exit,
                                                     const CanonicalValue& output) {
  const auto exact = [](const CanonicalValue& value, std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.as_object().size() == keys.size() &&
           std::all_of(keys.begin(), keys.end(),
                       [&](auto key) { return value.find_object_member(key); });
  };
  const auto text = [](const CanonicalValue& value, std::size_t limit, bool nullable = true) {
    return (nullable && value.is_null()) ||
           (value.is_string() && value.as_string().size() <= limit);
  };
  const auto owner_unsigned = [](const CanonicalValue& value, bool nullable = true) {
    if (value.is_null()) return nullable;
    if (value.is_number())
      return value.as_number() >= 0.0 && value.as_number() <= 9007199254740991.0 &&
             std::floor(value.as_number()) == value.as_number();
    if (!value.is_string() || value.as_string().empty() ||
        (value.as_string().size() > 1U && value.as_string().front() == '0'))
      return false;
    std::uint64_t parsed{};
    const auto& token = value.as_string();
    const auto [last, error] = std::from_chars(token.data(), token.data() + token.size(), parsed);
    return error == std::errc{} && last == token.data() + token.size();
  };
  if (!exact(exit, {"status", "signal", "core_dumped", "observed_utc"})) return false;
  for (auto key : {"status", "signal"}) {
    const auto& value = *exit.find_object_member(key);
    if (!value.is_null() &&
        (!value.is_number() || value.as_number() < 0.0 || value.as_number() > 255.0 ||
         std::floor(value.as_number()) != value.as_number()))
      return false;
  }
  if (!exit.find_object_member("core_dumped")->is_bool() ||
      !text(*exit.find_object_member("observed_utc"), 128U, false))
    return false;
  if (!exact(output, {"tail", "line_count", "truncated", "dropped_forward_lines",
                      "dropped_forward_bytes", "invalid_text_bytes"}))
    return false;
  const auto& tail = *output.find_object_member("tail");
  const auto& lines = *output.find_object_member("line_count");
  if (!tail.is_string() || content_identity::canonical_json(tail).size() > 128U * 1024U ||
      !lines.is_number() || lines.as_number() < 0.0 || lines.as_number() > 2000.0 ||
      std::floor(lines.as_number()) != lines.as_number() ||
      !output.find_object_member("truncated")->is_bool())
    return false;
  const auto& bytes = tail.as_string();
  const auto count = bytes.empty()
                         ? 0U
                         : static_cast<std::size_t>(std::count(bytes.begin(), bytes.end(), '\n')) +
                               (bytes.back() == '\n' ? 0U : 1U);
  if (lines.as_number() != static_cast<double>(count)) return false;
  for (auto key : {"dropped_forward_lines", "dropped_forward_bytes", "invalid_text_bytes"})
    if (!owner_unsigned(*output.find_object_member(key), false)) return false;
  return true;
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

bool CellObserverIncidentTargetDTO::locally_valid() const noexcept {
  // Same Q94 checked identity budgets as the launch-facts value. Do not
  // normalize or reject an accepted source NUL; the canonical codec escapes it.
  constexpr std::string_view suffix = ".9007199254740991";
  return !cell_id.empty() && cell_id.value().size() <= 256U && !source_id.empty() &&
         source_id.size() <= 256U && !source_epoch_base.empty() &&
         source_epoch_base.size() <= 256U - suffix.size() &&
         source_epoch_base.find('\0') == std::string::npos;
}

bool CellObserverIncidentRecordDTO::locally_valid() const noexcept {
  try {
    if (incident_id.empty() || incident_id.size() > kMaxComponentIncidentIdBytes ||
        !target.locally_valid() || detected_at_ns < 0 || summary.empty() ||
        summary.size() > kMaxComponentIncidentSummaryBytes || !diagnostic_snapshot.is_object() ||
        diagnostic_snapshot.as_object().size() != 6U)
      return false;
    for (const auto key : {"exit", "context", "settings", "generated", "output", "core_dump_link"})
      if (!diagnostic_snapshot.find_object_member(key)) return false;
    const auto& exit = *diagnostic_snapshot.find_object_member("exit");
    const auto& output = *diagnostic_snapshot.find_object_member("output");
    if (!IncidentExitAndOutputShapeIsValid(exit, output) ||
        exit.find_object_member("status")->is_null() ==
            exit.find_object_member("signal")->is_null())
      return false;
    for (const auto key : {"context", "settings", "generated"})
      if (!diagnostic_snapshot.find_object_member(key)->is_object()) return false;
    const auto& context = *diagnostic_snapshot.find_object_member("context");
    for (const auto& [key, expected] :
         {std::pair<std::string_view, std::string_view>{"cell_id", target.cell_id.value()},
          {"source_id", target.source_id},
          {"source_epoch_base", target.source_epoch_base}}) {
      const auto* value = context.find_object_member(key);
      if (!value || !value->is_string() || value->as_string() != expected) return false;
    }
    const auto& core = *diagnostic_snapshot.find_object_member("core_dump_link");
    return core.is_null() || (core.is_string() && core.as_string().size() <= 1024U);
  } catch (...) {
    return false;
  }
}

std::string EncodeCellObserverIncidentRecord(const CellObserverIncidentRecordDTO& record) {
  if (!record.locally_valid())
    throw std::invalid_argument("cell observer incident record is invalid");
  const auto bytes = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"incident_id", CanonicalValue(record.incident_id)},
      {"event_kind", CanonicalValue(kComponentIncidentEventKind.data())},
      {"target_kind", CanonicalValue("cell_observer")},
      {"target", CanonicalValue(CanonicalValue::Object{
                     {"cell_id", CanonicalValue(record.target.cell_id.value())},
                     {"source_id", CanonicalValue(record.target.source_id)},
                     {"source_epoch_base", CanonicalValue(record.target.source_epoch_base)}})},
      {"cause_class", CanonicalValue("provider_lost")},
      {"detected_at_ns", NonNegative(record.detected_at_ns)},
      {"summary", CanonicalValue(record.summary)},
      {"acknowledgement", CanonicalValue(nullptr)},
      {"diagnostic_snapshot", record.diagnostic_snapshot}}));
  if (bytes.size() > kMaxComponentIncidentRecordBytes)
    throw std::invalid_argument("cell observer incident record exceeds the shared byte bound");
  return bytes;
}

CellObserverIncidentRecordDTO DecodeCellObserverIncidentRecord(const std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaxComponentIncidentRecordBytes)
    throw std::invalid_argument("cell observer incident record exceeds the shared byte bound");
  auto limits = content_identity::CanonicalJsonParseLimits{};
  limits.maximum_bytes = kMaxComponentIncidentRecordBytes;
  const auto root = content_identity::parse_canonical_json(bytes, limits);
  const auto exact = [](const CanonicalValue& value, std::initializer_list<std::string_view> keys) {
    return value.is_object() && value.as_object().size() == keys.size() &&
           std::all_of(keys.begin(), keys.end(),
                       [&](auto key) { return value.find_object_member(key) != nullptr; });
  };
  if (!exact(root, {"incident_id", "event_kind", "target_kind", "target", "cause_class",
                    "detected_at_ns", "summary", "acknowledgement", "diagnostic_snapshot"}))
    throw std::invalid_argument("cell observer incident record fields differ");
  const auto text = [](const CanonicalValue& value, const std::string_view key) -> std::string {
    const auto* member = value.find_object_member(key);
    if (!member || !member->is_string())
      throw std::invalid_argument("cell observer incident text field is invalid");
    return member->as_string();
  };
  const auto& target = *root.find_object_member("target");
  if (text(root, "event_kind") != kComponentIncidentEventKind ||
      text(root, "target_kind") != "cell_observer" ||
      text(root, "cause_class") != "provider_lost" ||
      !root.find_object_member("acknowledgement")->is_null() ||
      !exact(target, {"cell_id", "source_id", "source_epoch_base"}))
    throw std::invalid_argument("cell observer incident target or initial state is invalid");
  CellObserverIncidentRecordDTO result{
      text(root, "incident_id"),
      {CellId(text(target, "cell_id")), text(target, "source_id"),
       text(target, "source_epoch_base")},
      content_identity::parse_nonnegative_int64(*root.find_object_member("detected_at_ns")),
      text(root, "summary"),
      *root.find_object_member("diagnostic_snapshot")};
  if (!result.locally_valid())
    throw std::invalid_argument("cell observer incident record values are invalid");
  return result;
}

std::string CellObserverIncidentEventPayload(const CellObserverIncidentRecordDTO& record) {
  return EncodeCellObserverIncidentRecord(record);
}

namespace {

void ObserverExact(const CanonicalValue& value, std::initializer_list<std::string_view> keys) {
  if (!value.is_object() || value.as_object().size() != keys.size() ||
      !std::all_of(keys.begin(), keys.end(),
                   [&](auto key) { return value.find_object_member(key); }))
    throw std::invalid_argument("observer bring-up fields differ");
}
const CanonicalValue& ObserverField(const CanonicalValue& value, std::string_view key) {
  const auto* result = value.find_object_member(key);
  if (!result) throw std::invalid_argument("observer bring-up field is missing");
  return *result;
}
std::string ObserverText(const CanonicalValue& value, std::string_view key) {
  const auto& field = ObserverField(value, key);
  if (!field.is_string()) throw std::invalid_argument("observer bring-up text is invalid");
  return field.as_string();
}
std::optional<std::string> ObserverOptionalText(const CanonicalValue& value) {
  if (value.is_null()) return std::nullopt;
  if (!value.is_string()) throw std::invalid_argument("observer bring-up optional text is invalid");
  return value.as_string();
}
std::uint64_t ObserverUnsigned(const CanonicalValue& value) {
  if (value.is_number()) {
    const auto number = value.as_number();
    if (std::isfinite(number) && number >= 0 && number <= 9007199254740991.0 &&
        std::floor(number) == number)
      return static_cast<std::uint64_t>(number);
  } else if (value.is_string()) {
    const auto& text = value.as_string();
    std::uint64_t result{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
    if (!text.empty() && text.front() != '0' && parsed.ec == std::errc{} &&
        parsed.ptr == text.data() + text.size() && result > 9007199254740991ULL)
      return result;
  }
  throw std::invalid_argument("observer bring-up counter is invalid");
}
bool ObserverBool(const CanonicalValue& value) {
  if (!value.is_bool()) throw std::invalid_argument("observer bring-up boolean is invalid");
  return value.as_bool();
}
CanonicalValue ObserverTarget(const CellObserverIncidentTargetDTO& target) {
  return CanonicalValue(
      CanonicalValue::Object{{"cell_id", CanonicalValue(target.cell_id.value())},
                             {"source_id", CanonicalValue(target.source_id)},
                             {"source_epoch_base", CanonicalValue(target.source_epoch_base)}});
}
CellObserverIncidentTargetDTO ObserverTarget(const CanonicalValue& value) {
  ObserverExact(value, {"cell_id", "source_id", "source_epoch_base"});
  return {CellId(ObserverText(value, "cell_id")), ObserverText(value, "source_id"),
          ObserverText(value, "source_epoch_base")};
}
CanonicalValue ObserverProcess(const std::optional<CellObserverBringUpProcessDTO>& process) {
  if (!process) return CanonicalValue(nullptr);
  return CanonicalValue(
      CanonicalValue::Object{{"leader_pid", NonNegative(process->leader_pid)},
                             {"launch_identity", CanonicalValue(process->launch_identity)}});
}
std::optional<CellObserverBringUpProcessDTO> ObserverProcess(const CanonicalValue& value) {
  if (value.is_null()) return std::nullopt;
  ObserverExact(value, {"leader_pid", "launch_identity"});
  return CellObserverBringUpProcessDTO{
      content_identity::parse_nonnegative_int64(ObserverField(value, "leader_pid")),
      ObserverText(value, "launch_identity")};
}
constexpr std::string_view ObserverPhase(const CellObserverBringUpPhaseDTO phase) {
  switch (phase) {
    case CellObserverBringUpPhaseDTO::kAdmitted:
      return "admitted";
    case CellObserverBringUpPhaseDTO::kRetiring:
      return "retiring";
    case CellObserverBringUpPhaseDTO::kStarted:
      return "started";
    case CellObserverBringUpPhaseDTO::kOpenAccepted:
      return "open_accepted";
    case CellObserverBringUpPhaseDTO::kFailed:
      return "failed";
    case CellObserverBringUpPhaseDTO::kUnknown:
      return "unknown";
  }
  return {};
}
bool ObserverBoundedText(std::string_view value) {
  return !value.empty() && value.size() <= kMaxLifecycleIdentityBytes;
}
}  // namespace

bool CellObserverLossAssociationDTO::locally_valid() const noexcept {
  return !incident_id.empty() && incident_id.size() <= kMaxComponentIncidentIdBytes &&
         !latest_exit_id.empty() && latest_exit_id.size() <= kMaxComponentIncidentIdBytes &&
         latest_exit_digest.size() == 64U &&
         std::all_of(latest_exit_digest.begin(), latest_exit_digest.end(), [](const char value) {
           return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
         });
}

bool CellObserverBringUpIntentDTO::locally_valid() const noexcept {
  try {
    const auto& request = request_context;
    if (!ObserverBoundedText(request.request_id.value()) ||
        !ObserverBoundedText(request.request_owner_epoch.value()) || request.source_label.empty() ||
        request.source_label.size() > kMaxLifecycleRequestLabelBytes ||
        (request.correlation_id &&
         (request.correlation_id->empty() ||
          request.correlation_id->size() > kMaxLifecycleRequestLabelBytes)) ||
        !attribution.locally_valid() ||
        (attribution.principal_id &&
         attribution.principal_id->size() > kMaxLifecycleRequestLabelBytes) ||
        !projection_revision.valid() ||
        !ObserverBoundedText(projection_revision.retained_bundle_id.value()) ||
        !ObserverBoundedText(projection_revision.graph_revision.value()) ||
        !publication_generation.nonzero() ||
        !IsExactLowercaseSha256Digest(publication_digest.value()) ||
        !ObserverBoundedText(runtime_unit_id.value()) || !ObserverBoundedText(host_epoch.value()) ||
        manager_epoch.empty() || manager_epoch.size() > kMaxLifecycleRequestLabelBytes ||
        scene_owner_epoch.empty() || scene_owner_epoch.size() > kMaxLifecycleRequestLabelBytes ||
        scene_identity_json.empty() ||
        scene_identity_json.size() > kMaxComponentIncidentRecordBytes ||
        !predecessor.locally_valid() || !replacement.locally_valid() ||
        predecessor.cell_id != replacement.cell_id ||
        predecessor.source_epoch_base == replacement.source_epoch_base ||
        retained_snapshot_path.empty() ||
        retained_snapshot_path.size() > kMaxObserverBringUpSnapshotPathBytes ||
        retained_snapshot_path.front() != '/' ||
        retained_snapshot_path.find('\0') != std::string::npos ||
        (predecessor_incident_id &&
         (predecessor_incident_id->empty() ||
          predecessor_incident_id->size() > kMaxComponentIncidentIdBytes)))
      return false;
    const auto scene = content_identity::parse_canonical_json(scene_identity_json);
    ObserverExact(scene, {"deployment_generation", "graph_revision", "program_generation",
                          "retained_bundle_id", "runtime_generation"});
    if (!ObserverBoundedText(ObserverText(scene, "graph_revision")) ||
        !ObserverBoundedText(ObserverText(scene, "retained_bundle_id")))
      return false;
    for (const auto key : {"deployment_generation", "program_generation", "runtime_generation"})
      static_cast<void>(ObserverUnsigned(ObserverField(scene, key)));
    // Scene owns the interpretation. Store only the exact canonical identity
    // checked by that owner, never use this record as a Scene authority.
    return content_identity::canonical_json(scene) == scene_identity_json;
  } catch (...) {
    return false;
  }
}
bool CellObserverBringUpProcessDTO::locally_valid() const noexcept {
  return leader_pid > 0 && !launch_identity.empty() &&
         launch_identity.size() <= kMaximumCellAdmissionPrivateBinaryBytes;
}
bool CellObserverBringUpOpenDTO::locally_valid() const noexcept {
  return !source_epoch.empty() && source_epoch.size() <= 256U && source_epoch_order != 0U &&
         source_epoch_order <= 9007199254740991ULL && accepted_at_ns >= 0;
}
bool CellObserverBringUpRecordDTO::locally_valid() const noexcept {
  if (!intent.locally_valid() || ObserverPhase(phase).empty() || admitted_at_ns < 0 ||
      updated_at_ns < admitted_at_ns ||
      (predecessor_process && !predecessor_process->locally_valid()) ||
      (started_process && !started_process->locally_valid()) ||
      (diagnostic && !ComponentIncidentDiagnosticIsValid(*diagnostic)))
    return false;
  if (predecessor_loss.has_value() != intent.predecessor_incident_id.has_value() ||
      (predecessor_loss && (!predecessor_loss->locally_valid() ||
                           predecessor_loss->incident_id != *intent.predecessor_incident_id)))
    return false;
  const bool started = phase == CellObserverBringUpPhaseDTO::kStarted ||
                       phase == CellObserverBringUpPhaseDTO::kOpenAccepted;
  if (started && (!predecessor_drained_or_absent || !started_process)) return false;
  if (started_process && !predecessor_drained_or_absent) return false;
  if (phase == CellObserverBringUpPhaseDTO::kRetiring && started_process) return false;
  if (phase == CellObserverBringUpPhaseDTO::kAdmitted &&
      (predecessor_drained_or_absent || started_process || diagnostic))
    return false;
  if ((phase == CellObserverBringUpPhaseDTO::kOpenAccepted) != first_open.has_value()) return false;
  if (first_open) {
    if (!first_open->locally_valid() || first_open->accepted_at_ns < admitted_at_ns ||
        first_open->accepted_at_ns > updated_at_ns ||
        !first_open->source_epoch.starts_with(intent.replacement.source_epoch_base + "."))
      return false;
    const auto suffix = std::string_view(first_open->source_epoch)
                            .substr(intent.replacement.source_epoch_base.size() + 1U);
    std::uint64_t counter{};
    const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), counter);
    if (suffix.empty() || suffix.front() == '0' || parsed.ec != std::errc{} ||
        parsed.ptr != suffix.data() + suffix.size() || counter == 0U ||
        counter > 9007199254740991ULL)
      return false;
  }
  return true;
}

std::string EncodeCellObserverBringUpRecord(const CellObserverBringUpRecordDTO& record) {
  if (!record.locally_valid()) throw std::invalid_argument("observer bring-up record is invalid");
  const auto& intent = record.intent;
  const auto& request = intent.request_context;
  const auto& projection = intent.projection_revision;
  const auto& scene = projection.scene_revision;
  CanonicalValue first_open(nullptr);
  CanonicalValue predecessor_loss(nullptr);
  if (record.predecessor_loss) {
    predecessor_loss = CanonicalValue(CanonicalValue::Object{
        {"incident_id", CanonicalValue(record.predecessor_loss->incident_id)},
        {"latest_exit_id", CanonicalValue(record.predecessor_loss->latest_exit_id)},
        {"latest_exit_digest", CanonicalValue(record.predecessor_loss->latest_exit_digest)}});
  }
  if (record.first_open) {
    first_open = CanonicalValue(CanonicalValue::Object{
        {"source_epoch", CanonicalValue(record.first_open->source_epoch)},
        {"source_epoch_order", Unsigned(record.first_open->source_epoch_order)},
        {"accepted_at_ns", NonNegative(record.first_open->accepted_at_ns)}});
  }
  const auto bytes = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"intent",
       CanonicalValue(CanonicalValue::Object{
           {"request_context",
            CanonicalValue(CanonicalValue::Object{
                {"request_id", CanonicalValue(request.request_id.value())},
                {"request_owner_epoch", CanonicalValue(request.request_owner_epoch.value())},
                {"source_label", CanonicalValue(request.source_label)},
                {"correlation_id", OptionalText(request.correlation_id)}})},
           {"attribution",
            CanonicalValue(CanonicalValue::Object{
                {"transport", CanonicalValue(static_cast<double>(intent.attribution.transport))},
                {"assurance", CanonicalValue(static_cast<double>(intent.attribution.assurance))},
                {"principal_id", OptionalText(intent.attribution.principal_id)}})},
           {"projection_revision",
            CanonicalValue(CanonicalValue::Object{
                {"retained_bundle_id", CanonicalValue(projection.retained_bundle_id.value())},
                {"graph_revision", CanonicalValue(projection.graph_revision.value())},
                {"scene_id", CanonicalValue(scene.scene_id.value())},
                {"scene_revision", Unsigned(scene.revision_id)},
                {"scene_content_hash", CanonicalValue(scene.content_hash.value())}})},
           {"publication_generation", Unsigned(intent.publication_generation.value())},
           {"publication_digest", CanonicalValue(intent.publication_digest.value())},
           {"runtime_unit_id", CanonicalValue(intent.runtime_unit_id.value())},
           {"host_epoch", CanonicalValue(intent.host_epoch.value())},
           {"manager_epoch", CanonicalValue(intent.manager_epoch)},
           {"scene_owner_epoch", CanonicalValue(intent.scene_owner_epoch)},
           {"scene_identity", content_identity::parse_canonical_json(intent.scene_identity_json)},
           {"predecessor", ObserverTarget(intent.predecessor)},
           {"replacement", ObserverTarget(intent.replacement)},
           {"retained_snapshot_path", CanonicalValue(intent.retained_snapshot_path)},
           {"predecessor_incident_id", OptionalText(intent.predecessor_incident_id)}})},
      {"phase", CanonicalValue(std::string(ObserverPhase(record.phase)))},
      {"admitted_at_ns", NonNegative(record.admitted_at_ns)},
      {"updated_at_ns", NonNegative(record.updated_at_ns)},
      {"predecessor_process", ObserverProcess(record.predecessor_process)},
      {"predecessor_loss", std::move(predecessor_loss)},
      {"predecessor_drained_or_absent", CanonicalValue(record.predecessor_drained_or_absent)},
      {"started_process", ObserverProcess(record.started_process)},
      {"first_open", std::move(first_open)},
      {"diagnostic",
       record.diagnostic ? DiagnosticValue(*record.diagnostic) : CanonicalValue(nullptr)}}));
  if (bytes.size() > kMaxComponentIncidentRecordBytes)
    throw std::length_error("observer_bring_up_record_too_large");
  return bytes;
}

CellObserverBringUpRecordDTO DecodeCellObserverBringUpRecord(std::string_view bytes) {
  auto limits = content_identity::CanonicalJsonParseLimits{};
  limits.maximum_bytes = kMaxComponentIncidentRecordBytes;
  const auto root = content_identity::parse_canonical_json(bytes, limits);
  ObserverExact(root,
                {"intent", "phase", "admitted_at_ns", "updated_at_ns", "predecessor_process",
                 "predecessor_loss", "predecessor_drained_or_absent", "started_process",
                 "first_open", "diagnostic"});
  const auto& intent = ObserverField(root, "intent");
  ObserverExact(intent,
                {"request_context", "attribution", "projection_revision", "publication_generation",
                 "publication_digest", "runtime_unit_id", "host_epoch", "manager_epoch",
                 "scene_owner_epoch", "scene_identity", "predecessor", "replacement",
                 "retained_snapshot_path", "predecessor_incident_id"});
  const auto& request = ObserverField(intent, "request_context");
  ObserverExact(request, {"request_id", "request_owner_epoch", "source_label", "correlation_id"});
  const auto& attribution = ObserverField(intent, "attribution");
  ObserverExact(attribution, {"transport", "assurance", "principal_id"});
  const auto enum_value = [](const CanonicalValue& value, int maximum) {
    if (!value.is_number() || !std::isfinite(value.as_number()) || value.as_number() < 0 ||
        value.as_number() > maximum || std::floor(value.as_number()) != value.as_number())
      throw std::invalid_argument("observer attribution enum is invalid");
    return static_cast<int>(value.as_number());
  };
  const auto& projection = ObserverField(intent, "projection_revision");
  ObserverExact(projection, {"retained_bundle_id", "graph_revision", "scene_id", "scene_revision",
                             "scene_content_hash"});
  CellObserverBringUpRecordDTO result{
      {{RequestId(ObserverText(request, "request_id")),
        RequestOwnerEpoch(ObserverText(request, "request_owner_epoch")),
        ObserverText(request, "source_label"),
        ObserverOptionalText(ObserverField(request, "correlation_id"))},
       {static_cast<RequestTransportDTO>(enum_value(ObserverField(attribution, "transport"), 2)),
        static_cast<RequestAssuranceDTO>(enum_value(ObserverField(attribution, "assurance"), 1)),
        ObserverOptionalText(ObserverField(attribution, "principal_id"))},
       {RetainedBundleId(ObserverText(projection, "retained_bundle_id")),
        GraphRevision(ObserverText(projection, "graph_revision")),
        {SceneId(ObserverText(projection, "scene_id")),
         ObserverUnsigned(ObserverField(projection, "scene_revision")),
         ContentDigest(ObserverText(projection, "scene_content_hash"))}},
       ActiveBundlePublicationGeneration(
           ObserverUnsigned(ObserverField(intent, "publication_generation"))),
       ContentDigest(ObserverText(intent, "publication_digest")),
       RuntimeUnitId(ObserverText(intent, "runtime_unit_id")),
       RuntimeEpoch(ObserverText(intent, "host_epoch")),
       ObserverText(intent, "manager_epoch"),
       ObserverText(intent, "scene_owner_epoch"),
       content_identity::canonical_json(ObserverField(intent, "scene_identity")),
       ObserverTarget(ObserverField(intent, "predecessor")),
       ObserverTarget(ObserverField(intent, "replacement")),
       ObserverText(intent, "retained_snapshot_path"),
       ObserverOptionalText(ObserverField(intent, "predecessor_incident_id"))},
      CellObserverBringUpPhaseDTO::kAdmitted,
      0,
      0,
      std::nullopt,
      false,
      std::nullopt,
      std::nullopt,
      std::nullopt};
  const auto phase = ObserverText(root, "phase");
  bool found = false;
  for (const auto value :
       {CellObserverBringUpPhaseDTO::kAdmitted, CellObserverBringUpPhaseDTO::kRetiring,
        CellObserverBringUpPhaseDTO::kStarted, CellObserverBringUpPhaseDTO::kOpenAccepted,
        CellObserverBringUpPhaseDTO::kFailed, CellObserverBringUpPhaseDTO::kUnknown}) {
    if (ObserverPhase(value) == phase) {
      result.phase = value;
      found = true;
      break;
    }
  }
  if (!found) throw std::invalid_argument("observer bring-up phase is invalid");
  result.admitted_at_ns =
      content_identity::parse_nonnegative_int64(ObserverField(root, "admitted_at_ns"));
  result.updated_at_ns =
      content_identity::parse_nonnegative_int64(ObserverField(root, "updated_at_ns"));
  result.predecessor_process = ObserverProcess(ObserverField(root, "predecessor_process"));
  const auto& predecessor_loss = ObserverField(root, "predecessor_loss");
  if (!predecessor_loss.is_null()) {
    ObserverExact(predecessor_loss, {"incident_id", "latest_exit_id", "latest_exit_digest"});
    result.predecessor_loss = CellObserverLossAssociationDTO{
        ObserverText(predecessor_loss, "incident_id"),
        ObserverText(predecessor_loss, "latest_exit_id"),
        ObserverText(predecessor_loss, "latest_exit_digest")};
  }
  result.predecessor_drained_or_absent =
      ObserverBool(ObserverField(root, "predecessor_drained_or_absent"));
  result.started_process = ObserverProcess(ObserverField(root, "started_process"));
  const auto& open = ObserverField(root, "first_open");
  if (!open.is_null()) {
    ObserverExact(open, {"source_epoch", "source_epoch_order", "accepted_at_ns"});
    result.first_open = CellObserverBringUpOpenDTO{
        ObserverText(open, "source_epoch"),
        ObserverUnsigned(ObserverField(open, "source_epoch_order")),
        content_identity::parse_nonnegative_int64(ObserverField(open, "accepted_at_ns"))};
  }
  const auto& diagnostic = ObserverField(root, "diagnostic");
  if (!diagnostic.is_null()) {
    ObserverExact(diagnostic, {"code", "message", "details"});
    ErrorDTO error{ObserverText(diagnostic, "code"), ObserverText(diagnostic, "message"), {}};
    const auto& details = ObserverField(diagnostic, "details");
    if (!details.is_object())
      throw std::invalid_argument("observer diagnostic details are invalid");
    for (const auto& [key, value] : details.as_object()) {
      if (!value.is_string() || !error.details.emplace(key, value.as_string()).second)
        throw std::invalid_argument("observer diagnostic detail is invalid");
    }
    result.diagnostic = std::move(error);
  }
  if (!result.locally_valid() || EncodeCellObserverBringUpRecord(result) != bytes)
    throw std::invalid_argument("observer bring-up record is invalid or noncanonical");
  return result;
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
                                          const std::string_view event_kind,
                                          const std::optional<ErrorDTO>& diagnostic,
                                          std::shared_ptr<const CanonicalValue> snapshot) {
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
      {"acknowledged_by",
       acknowledged ? OptionalText(incident.acknowledged_by) : CanonicalValue(nullptr)},
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
      {"schema_version", CanonicalValue(std::string(kComponentIncidentEventSchemaVersion))},
      {"stop", std::move(stop)},
      {"summary", CanonicalValue(incident.summary)},
      {"timeline", CanonicalValue(CanonicalValue::Array{TimelineEntry(
                       incident.detected_at_ns, incident.cause_class, incident.vendor_codes)})},
      {"vendor_codes", Strings(incident.vendor_codes)},
  };
  if (diagnostic)
    fields.emplace_back("diagnostic",
                        ComponentIncidentDiagnosticValue(
                            *diagnostic, snapshot ? *snapshot : CanonicalValue(nullptr)));
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
    stop = CanonicalValue(
        CanonicalValue::Object{{"stop_generation", Unsigned(receipt->stop_generation.value())},
                               {"stop_id", CanonicalValue(receipt->stop_id.value())}});
    if (receipt->program_run_id) program_run = CanonicalValue(receipt->program_run_id->value());
  }
  return Patched(record_json, {{"active_operation_ids", CanonicalValue(std::move(operations))},
                               {"containment_error", OptionalText(error)},
                               {"containment_state", CanonicalValue(std::string(to_string(state)))},
                               {"program_run_id", std::move(program_run)},
                               {"stop", std::move(stop)}});
}

std::string ComponentIncidentRecordWithChange(const std::string_view record_json,
                                              const std::int64_t at_ns,
                                              const LifecycleBlockingCauseClassDTO cause,
                                              const std::vector<std::string>& vendor_codes,
                                              const std::optional<ErrorDTO>& diagnostic,
                                              std::shared_ptr<const CanonicalValue> snapshot) {
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
  CanonicalValue::Object updates{{"cause_class", CanonicalValue(std::string(to_string(cause)))},
                                 {"timeline", CanonicalValue(std::move(timeline))},
                                 {"vendor_codes", Strings(vendor_codes)}};
  if (diagnostic)
    updates.emplace_back("diagnostic",
                         ComponentIncidentDiagnosticValue(
                             *diagnostic, snapshot ? *snapshot : CanonicalValue(nullptr)));
  return Patched(record_json, std::move(updates));
}

bool ComponentIncidentDiagnosticIsValid(const ErrorDTO& diagnostic) noexcept {
  try {
    if (diagnostic.code.empty() || diagnostic.code.size() > kMaxLifecycleErrorCodeBytes ||
        diagnostic.message.size() > kMaxLifecycleErrorMessageBytes ||
        diagnostic.details.size() > kMaxLifecycleErrorDetails ||
        !content_identity::is_utf8_nfc(diagnostic.code) ||
        !content_identity::is_utf8_nfc(diagnostic.message))
      return false;
    return std::all_of(
        diagnostic.details.begin(), diagnostic.details.end(), [](const auto& detail) {
          return !detail.first.empty() && detail.first.size() <= kMaxLifecycleErrorDetailKeyBytes &&
                 detail.second.size() <= kMaxLifecycleErrorDetailValueBytes &&
                 content_identity::is_utf8_nfc(detail.first) &&
                 content_identity::is_utf8_nfc(detail.second);
        });
  } catch (...) {
    return false;
  }
}

bool ComponentIncidentDiagnosticSnapshotShapeIsValid(const CanonicalValue& snapshot) noexcept {
  try {
    const auto exact = [](const CanonicalValue& value,
                          std::initializer_list<std::string_view> keys) {
      return value.is_object() && value.as_object().size() == keys.size() &&
             std::all_of(keys.begin(), keys.end(),
                         [&](auto key) { return value.find_object_member(key); });
    };
    const auto text = [](const CanonicalValue& value, std::size_t limit, bool nullable = true) {
      return (nullable && value.is_null()) ||
             (value.is_string() && value.as_string().size() <= limit);
    };
    const auto boolean = [](const CanonicalValue& value) {
      return value.is_null() || value.is_bool();
    };
    const auto owner_unsigned = [](const CanonicalValue& value, bool nullable = true) {
      if (value.is_null()) return nullable;
      if (value.is_number())
        return value.as_number() >= 0.0 && value.as_number() <= 9007199254740991.0 &&
               std::floor(value.as_number()) == value.as_number();
      if (!value.is_string() || value.as_string().empty() ||
          (value.as_string().size() > 1U && value.as_string().front() == '0'))
        return false;
      std::uint64_t parsed{};
      const auto& token = value.as_string();
      const auto [last, error] = std::from_chars(token.data(), token.data() + token.size(), parsed);
      return error == std::errc{} && last == token.data() + token.size();
    };
    if (!exact(snapshot, {"exit", "context", "launch_readiness", "settings", "generated", "output",
                          "core_dump_link"}))
      return false;
    if (!IncidentExitAndOutputShapeIsValid(*snapshot.find_object_member("exit"),
                                           *snapshot.find_object_member("output")))
      return false;
    const auto& context = *snapshot.find_object_member("context");
    if (!exact(context, {"controllable_component_id", "request_context", "runtime_unit_id", "runtime_epoch",
                         "retained_bundle_id", "graph_revision", "generator_identity", "launch_identity"}))
      return false;
    for (const auto& [key, value] : context.as_object())
      if (!text(value, key == "launch_identity" ? kMaximumCellAdmissionPrivateBinaryBytes
                       : key == "request_context" || key == "generator_identity" ? 1024U : 128U))
        return false;
    const auto& readiness = *snapshot.find_object_member("launch_readiness");
    if (!exact(readiness,
               {"known", "controller_manager_present", "controller_observed", "controller_loaded",
                "controller_bound", "arm_controller_active", "hardware_ready",
                "joint_sample_available", "binding_epoch", "sample_age_ns", "captured_utc"}))
      return false;
    for (auto key :
         {"known", "controller_manager_present", "controller_observed", "controller_loaded",
          "controller_bound", "arm_controller_active", "hardware_ready", "joint_sample_available"})
      if (!boolean(*readiness.find_object_member(key))) return false;
    if (!owner_unsigned(*readiness.find_object_member("binding_epoch")) ||
        !text(*readiness.find_object_member("captured_utc"), 128U))
      return false;
    const auto& age = *readiness.find_object_member("sample_age_ns");
    if (!age.is_null()) static_cast<void>(content_identity::parse_nonnegative_int64(age));
    const auto& settings = *snapshot.find_object_member("settings");
    if (!exact(settings, {"argv", "controller_profile", "runtime_namespace", "arm_controller_name",
                          "gripper_controller_name", "frame_prefix", "controllers_yaml",
                          "retained_configuration_json"}))
      return false;
    for (const auto& [key, value] : settings.as_object()) {
      if (key == "argv") {
        if (!value.is_array()) return false;
        for (const auto& argument : value.as_array())
          if (!text(argument, kMaxComponentIncidentRecordBytes, false)) return false;
      } else if (!text(value, kMaxComponentIncidentRecordBytes))
        return false;
    }
    const auto& generated = *snapshot.find_object_member("generated");
    if (!exact(generated, {"robot_description_sha256", "controllers_sha256"})) return false;
    for (const auto& [key, value] : generated.as_object()) {
      if (value.is_null()) continue;
      if (!value.is_string() || value.as_string().size() != 71U ||
          !value.as_string().starts_with("sha256:"))
        return false;
      for (const char digit : value.as_string().substr(7U))
        if (!((digit >= '0' && digit <= '9') || (digit >= 'a' && digit <= 'f'))) return false;
    }
    return text(*snapshot.find_object_member("core_dump_link"), 1024U);
  } catch (...) {
    return false;
  }
}

bool ComponentIncidentDiagnosticSnapshotIsValid(const CanonicalValue& snapshot) noexcept {
  try {
    return ComponentIncidentDiagnosticSnapshotShapeIsValid(snapshot) &&
           content_identity::canonical_json(snapshot).size() <= kMaxComponentIncidentRecordBytes;
  } catch (...) {
    return false;
  }
}

CanonicalValue ComponentIncidentDiagnosticValue(const ErrorDTO& diagnostic,
                                                const CanonicalValue& snapshot) {
  if (!ComponentIncidentDiagnosticIsValid(diagnostic) ||
      (!snapshot.is_null() && !ComponentIncidentDiagnosticSnapshotIsValid(snapshot)))
    throw std::invalid_argument("component incident diagnostic is outside its closed bound");
  return CanonicalValue(
      CanonicalValue::Object{{"error", DiagnosticValue(diagnostic)}, {"snapshot", snapshot}});
}

std::string ComponentIncidentRecordWithDiagnostic(const std::string_view record_json,
                                                  const ErrorDTO& diagnostic,
                                                  std::shared_ptr<const CanonicalValue> snapshot) {
  return Patched(record_json,
                 {{"diagnostic", ComponentIncidentDiagnosticValue(
                                     diagnostic, snapshot ? *snapshot : CanonicalValue(nullptr))}});
}

std::optional<ErrorDTO> ComponentIncidentDiagnosticFromRecord(
    const std::string_view record_json, std::shared_ptr<const CanonicalValue>* snapshot) noexcept {
  try {
    if (snapshot) snapshot->reset();
    const auto root = content_identity::parse_canonical_json(record_json);
    const auto* envelope = root.find_object_member("diagnostic");
    if (!envelope || !envelope->is_object() || envelope->as_object().size() != 2U)
      return std::nullopt;
    const auto* value = envelope->find_object_member("error");
    const auto* captured = envelope->find_object_member("snapshot");
    if (!value || !value->is_object() || value->as_object().size() != 3U || !captured ||
        (!captured->is_null() && !ComponentIncidentDiagnosticSnapshotIsValid(*captured)))
      return std::nullopt;
    const auto* code = value->find_object_member("code");
    const auto* message = value->find_object_member("message");
    const auto* details = value->find_object_member("details");
    if (!code || !code->is_string() || !message || !message->is_string() || !details ||
        !details->is_object())
      return std::nullopt;
    ErrorDTO diagnostic{code->as_string(), message->as_string(), {}};
    for (const auto& [key, item] : details->as_object()) {
      if (!item.is_string()) return std::nullopt;
      diagnostic.details.emplace(key, item.as_string());
    }
    if (!ComponentIncidentDiagnosticIsValid(diagnostic)) return std::nullopt;
    if (snapshot && !captured->is_null())
      *snapshot = std::make_shared<const CanonicalValue>(*captured);
    return diagnostic;
  } catch (...) {
    return std::nullopt;
  }
}

std::optional<ComponentIncidentContainmentReceiptDTO> ComponentIncidentContainmentReceiptFromRecord(
    const std::string_view record_json, std::optional<std::string>* containment_error) noexcept {
  try {
    const auto root = content_identity::parse_canonical_json(record_json);
    if (containment_error) {
      containment_error->reset();
      const auto* error = root.find_object_member("containment_error");
      if (error && error->is_string()) *containment_error = error->as_string();
    }
    const auto* stop = root.find_object_member("stop");
    const auto* operations = root.find_object_member("active_operation_ids");
    const auto* program = root.find_object_member("program_run_id");
    if (!stop || !stop->is_object() || stop->as_object().size() != 2U || !operations ||
        !operations->is_array() || !program)
      return std::nullopt;
    const auto identity = [](const CanonicalValue* value) {
      return value && value->is_string() && !value->as_string().empty() &&
             value->as_string().size() <= kMaxLifecycleIdentityBytes;
    };
    const auto* id = stop->find_object_member("stop_id");
    const auto* generation = stop->find_object_member("stop_generation");
    if (!identity(id) || !generation) return std::nullopt;
    std::uint64_t counter{};
    if (generation->is_number()) {
      const auto number = generation->as_number();
      if (number < 1.0 || number > 9007199254740991.0 || std::floor(number) != number)
        return std::nullopt;
      counter = static_cast<std::uint64_t>(number);
    } else if (generation->is_string()) {
      const auto& token = generation->as_string();
      if (token.empty() || token.front() == '0') return std::nullopt;
      const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), counter);
      if (error != std::errc{} || end != token.data() + token.size()) return std::nullopt;
    } else {
      return std::nullopt;
    }
    ComponentIncidentContainmentReceiptDTO receipt{
        StopId(id->as_string()), StopGeneration(counter), {}, std::nullopt};
    for (const auto& operation : operations->as_array()) {
      if (!identity(&operation)) return std::nullopt;
      receipt.affected_operation_ids.emplace_back(operation.as_string());
    }
    if (!program->is_null()) {
      if (!identity(program)) return std::nullopt;
      receipt.program_run_id = ProgramRunId(program->as_string());
    }
    return receipt;
  } catch (...) {
    return std::nullopt;
  }
}

std::string ComponentIncidentAcknowledgedPayload(
    const std::string_view record_json, const ComponentLifecycleIncidentDTO& acknowledged,
    const std::optional<ComponentIncidentClosureReasonDTO> closure_reason) {
  CanonicalValue::Object updates{{"acknowledged", CanonicalValue(true)},
       {"acknowledged_at_ns", acknowledged.acknowledged_at_ns
                                  ? NonNegative(*acknowledged.acknowledged_at_ns)
                                  : CanonicalValue(nullptr)},
       {"acknowledged_by", OptionalText(acknowledged.acknowledged_by)},
       {"acknowledgement_request_id",
        acknowledged.acknowledgement_request_id
            ? CanonicalValue(acknowledged.acknowledgement_request_id->value())
            : CanonicalValue(nullptr)},
       {"event_kind", CanonicalValue(std::string(kComponentIncidentAcknowledgedEventKind))}};
  if (closure_reason) {
    if (*closure_reason != ComponentIncidentClosureReasonDTO::kCellClosedByOperator)
      throw std::invalid_argument("incident closure reason is invalid");
    updates.emplace_back("closure_reason", CanonicalValue("cell_closed_by_operator"));
  }
  return Patched(record_json, std::move(updates));
}

}  // namespace manyforge::core

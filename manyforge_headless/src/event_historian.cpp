#include "manyforge_headless/event_historian.hpp"

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <exception>
#include <iostream>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {

std::optional<std::int64_t> HistorianRecordedUtcNs(std::string_view wall) noexcept {
  if (wall.size() < 20U || wall.size() > 35U || wall[4] != '-' || wall[7] != '-' ||
      (wall[10] != 'T' && wall[10] != 't' && wall[10] != ' ') ||
      wall[13] != ':' || wall[16] != ':') {
    return std::nullopt;
  }
  const auto digits = [wall](std::size_t start, std::size_t count) {
    int result = 0;
    for (std::size_t i = start; i < start + count; ++i) {
      if (i >= wall.size() || wall[i] < '0' || wall[i] > '9') return -1;
      result = result * 10 + wall[i] - '0';
    }
    return result;
  };
  const int year = digits(0U, 4U), month = digits(5U, 2U), day = digits(8U, 2U);
  const int hour = digits(11U, 2U), minute = digits(14U, 2U), second = digits(17U, 2U);
  if (year < 0 || month < 1 || day < 1 || hour < 0 || hour > 23 ||
      minute < 0 || minute > 59 || second < 0 || second > 59) return std::nullopt;
  const std::chrono::year_month_day date{std::chrono::year{year},
      std::chrono::month{static_cast<unsigned>(month)}, std::chrono::day{static_cast<unsigned>(day)}};
  if (!date.ok()) return std::nullopt;
  std::size_t offset = 19U;
  std::int64_t fraction = 0;
  if (wall[offset] == '.') {
    const auto start = ++offset;
    while (offset < wall.size() && wall[offset] >= '0' && wall[offset] <= '9') {
      if (offset - start >= 9U) return std::nullopt;
      fraction = fraction * 10 + wall[offset++] - '0';
    }
    if (offset == start) return std::nullopt;
    for (auto count = offset - start; count < 9U; ++count) fraction *= 10;
  }
  int zone_seconds = 0;
  if (offset + 1U == wall.size() && (wall[offset] == 'Z' || wall[offset] == 'z')) {
    // Explicit UTC.
  } else if (offset + 6U == wall.size() && (wall[offset] == '+' || wall[offset] == '-') &&
             wall[offset + 3U] == ':') {
    const auto h = digits(offset + 1U, 2U), m = digits(offset + 4U, 2U);
    if (h < 0 || h > 23 || m < 0 || m > 59) return std::nullopt;
    zone_seconds = (h * 3600 + m * 60) * (wall[offset] == '-' ? -1 : 1);
  } else {
    return std::nullopt;
  }
  const auto seconds = std::chrono::sys_days(date).time_since_epoch().count() * 86400LL +
                       hour * 3600 + minute * 60 + second - zone_seconds;
  constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
  if (seconds < 0 || seconds > (maximum - fraction) / 1000000000LL) return std::nullopt;
  return seconds * 1000000000LL + fraction;
}

namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::string_view kHistorianEventV1 = "manyforge.historian_event.v1";
constexpr std::size_t kMaximumIdentityBytes = 1024U;
constexpr std::size_t kMaximumWallTimeBytes = 128U;
constexpr std::size_t kMaximumHistorianQueueCapacity = 65536U;
constexpr std::size_t kMaximumSummaryTraversalNodes = 2048U;
constexpr std::size_t kMaximumSummaryTraversalDepth = 12U;
constexpr std::size_t kMaximumSummaryTextCodePoints = 1024U;

[[noreturn]] void HistorianFail(const std::string &code,
                                const std::string &message) {
  throw EventHistorianError(code, message);
}

void Require(const bool condition, const std::string &code,
             const std::string &message) {
  if (!condition) {
    HistorianFail(code, message);
  }
}

void ValidateText(const std::string &value, const std::string_view label,
                  const std::size_t maximum_bytes) {
  Require(!value.empty() && value.size() <= maximum_bytes &&
              content_identity::is_utf8_nfc(value),
          "historian_event_invalid",
          std::string(label) + " must be bounded NFC UTF-8");
}

void ValidateSafeInteger(const std::uint64_t value,
                         const std::string_view label) {
  Require(value <= kMaximumSafeInteger, "historian_value_invalid",
          std::string(label) + " exceeds the I-JSON safe integer range");
}

void ValidateSafeInteger(const std::int64_t value,
                         const std::string_view label) {
  Require(value >= 0 &&
              static_cast<std::uint64_t>(value) <= kMaximumSafeInteger,
          "historian_value_invalid",
          std::string(label) + " must be a nonnegative I-JSON safe integer");
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label) {
  ValidateSafeInteger(value, label);
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue Number(const std::int64_t value, const std::string_view label) {
  ValidateSafeInteger(value, label);
  return CanonicalValue(static_cast<double>(value));
}

CanonicalValue OptionalNumber(const std::optional<std::uint64_t> value,
                              const std::string_view label) {
  return value.has_value() ? Number(*value, label) : CanonicalValue(nullptr);
}

CanonicalValue OptionalString(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue PersistedNanoseconds(const std::int64_t value) {
  return static_cast<std::uint64_t>(value) <= kMaximumSafeInteger
             ? Number(value, "occurred_at_ns")
             : CanonicalValue(std::to_string(value));
}

void ValidateEvent(const HistorianEvent &event) {
  ValidateText(event.event_id, "event_id", kMaximumIdentityBytes);
  ValidateText(event.component, "component", kMaximumIdentityBytes);
  ValidateText(event.event_type, "event_type", kMaximumIdentityBytes);
  Require(event.severity == "DEBUG" || event.severity == "INFO" ||
              event.severity == "WARN" || event.severity == "ERROR" ||
              event.severity == "CRITICAL",
          "historian_event_invalid", "event severity is unsupported");
  Require(event.occurred_at_ns >= 0, "historian_value_invalid",
          "occurred_at_ns must be nonnegative");
  ValidateText(event.occurred_at_wall, "occurred_at_wall",
               kMaximumWallTimeBytes);
  const auto payload = content_identity::canonical_json(event.payload);
  Require(payload.size() <= kMaximumHistorianPayloadBytes,
          "historian_event_invalid", "event payload exceeds 256 KiB");
}

CanonicalValue EventIdentityValue(const std::string &component,
                                  const std::string &event_type,
                                  const std::string &severity,
                                  const std::int64_t occurred_at_ns,
                                  const std::string &occurred_at_wall,
                                  const CanonicalValue &payload) {
  return CanonicalValue(CanonicalValue::Object{
      {"component", CanonicalValue(component)},
      {"event_type", CanonicalValue(event_type)},
      {"occurred_at_ns", PersistedNanoseconds(occurred_at_ns)},
      {"occurred_at_wall", CanonicalValue(occurred_at_wall)},
      {"payload", payload},
      {"severity", CanonicalValue(severity)},
  });
}

std::optional<std::string>
DedicatedEventIdentity(const std::string &component,
                       const std::string &event_type,
                       const CanonicalValue &payload) {
  if (component != "manyforge.program" ||
      event_type != "program_model_attempt") {
    return std::nullopt;
  }
  Require(payload.is_object(), "historian_event_invalid",
          "Program model-attempt event payload must be an object");
  for (const auto &[name, value] : payload.as_object()) {
    if (name != "model_attempt_id") {
      continue;
    }
    const auto *identity = std::get_if<std::string>(&value.storage());
    Require(identity != nullptr, "historian_event_invalid",
            "Program model-attempt event identity must be text");
    ValidateText(*identity, "model_attempt_id", kMaximumIdentityBytes);
    return *identity;
  }
  HistorianFail("historian_event_invalid",
                "Program model-attempt event identity is absent");
}

void Advance(std::uint64_t &value) noexcept {
  if (value < kMaximumSafeInteger) {
    ++value;
  }
}

std::string Utf8Prefix(const std::string &value,
                       const std::size_t maximum_code_points) {
  std::size_t code_points = 0U;
  for (std::size_t index = 0U; index < value.size(); ++index) {
    const auto byte = static_cast<unsigned char>(value[index]);
    if ((byte & 0xc0U) == 0x80U) {
      continue;
    }
    if (code_points == maximum_code_points) {
      return value.substr(0U, index);
    }
    ++code_points;
  }
  return value;
}

std::string WordsFromIdentifier(const std::string &value,
                                const bool capitalize) {
  auto result = value;
  for (auto &character : result) {
    if (character == '_') {
      character = ' ';
    } else if (capitalize && character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  if (capitalize && !result.empty() && result.front() >= 'a' &&
      result.front() <= 'z') {
    result.front() = static_cast<char>(result.front() - 'a' + 'A');
  }
  return result;
}

const std::string *StringMember(const CanonicalValue &value,
                                const std::string_view name) {
  if (!value.is_object()) {
    return nullptr;
  }
  const auto *member = value.find_object_member(name);
  return member == nullptr ? nullptr
                           : std::get_if<std::string>(&member->storage());
}

struct SummaryIdentities final {
  std::optional<std::string> program_run_id;
  std::optional<std::string> operation_id;
  std::optional<std::string> condition_id;
};

SummaryIdentities SummaryIdentityValues(const CanonicalValue &payload) {
  SummaryIdentities result;
  std::set<std::string> run, operation, condition;
  const auto* schema = StringMember(payload, "schema_version");
  const bool producer_identities_only =
      schema != nullptr &&
      std::ranges::find(kProgramDiagnosticSchemaVersions, *schema) !=
          kProgramDiagnosticSchemaVersions.end();
  std::size_t visited = 0U;
  bool complete = true;
  const auto visit = [&](auto &&self, const CanonicalValue &current, std::size_t depth) -> void {
    if (++visited > kMaximumSummaryTraversalNodes || depth > kMaximumSummaryTraversalDepth) {
      complete = false;
      return;
    }
    if (current.is_object()) {
      for (const auto &[name, child] : current.as_object()) {
        auto *target = name == "program_run_id" ? &run : name == "operation_id" ? &operation :
            name == "condition_id" || name == "operator_alarm_condition_id" ? &condition : nullptr;
        if (target != nullptr) {
          const auto *text = std::get_if<std::string>(&child.storage());
          if (text != nullptr && !text->empty() && target->size() < 2) target->insert(*text);
        }
        // Blackboard/override values are user data, never producer attribution.
        // Match the versioned diagnostic's correlation-index contract.
        if (!producer_identities_only) self(self, child, depth + 1);
        if (!complete) return;
      }
    } else if (current.is_array()) {
      for (const auto &child : current.as_array()) {
        self(self, child, depth + 1);
        if (!complete) return;
      }
    }
  };
  visit(visit, payload, 0);
  const auto unique = [&](const std::set<std::string> &values) -> std::optional<std::string> {
    // A list never labels an ambiguous or clipped identity as exact. All
    // applicable keys remain indexed and available through detail/export.
    return complete && values.size() == 1 && values.begin()->size() <= 1024
        ? std::optional(*values.begin()) : std::nullopt;
  };
  result.program_run_id = unique(run);
  result.operation_id = unique(operation);
  result.condition_id = unique(condition);
  return result;
}

std::string SummaryMessage(const HistorianEvent &event) {
  if (const auto *message = StringMember(event.payload, "message");
      message != nullptr && !message->empty()) {
    return Utf8Prefix(*message, kMaximumSummaryTextCodePoints);
  }
  if (const auto *incident = StringMember(event.payload, "incident_kind");
      incident != nullptr && !incident->empty()) {
    auto message = WordsFromIdentifier(*incident, true);
    if (const auto *phase = StringMember(event.payload, "phase");
        phase != nullptr && *phase == "resolved") {
      message += " resolved";
    }
    return Utf8Prefix(message, kMaximumSummaryTextCodePoints);
  }
  return Utf8Prefix(WordsFromIdentifier(event.event_type, false),
                    kMaximumSummaryTextCodePoints);
}

} // namespace

std::string OperationFailureHistoryMessage(const CanonicalValue &state) {
  const auto *participants =
      state.is_object() ? state.find_object_member("participants") : nullptr;
  if (participants == nullptr || !participants->is_array())
    return {};
  std::string result;
  for (const auto &participant : participants->as_array()) {
    if (!participant.is_object())
      continue;
    const auto *error = participant.find_object_member("terminal_error");
    if (error == nullptr)
      continue;
    const auto *code = StringMember(*error, "code");
    const auto *message = StringMember(*error, "message");
    if (code == nullptr || code->empty() || message == nullptr ||
        message->empty())
      continue;
    if (!result.empty())
      result += "; ";
    // Keep each participant's attribution beside its own error. In particular,
    // a coordinated peer's cancellation must not be called the collision cause.
    const auto *scope = participant.find_object_member("execution_scope");
    if (scope != nullptr) {
      const auto *node = StringMember(*scope, "node_instance_id");
      const auto *attempt = StringMember(*scope, "node_attempt_id");
      if (node != nullptr && attempt != nullptr) {
        result += "[" + Utf8Prefix(*node, kMaximumSummaryTextCodePoints) +
                  " / " + Utf8Prefix(*attempt, kMaximumSummaryTextCodePoints) +
                  "] ";
      }
    }
    result += Utf8Prefix(*code, kMaximumSummaryTextCodePoints) + ": " +
              Utf8Prefix(*message, kMaximumSummaryTextCodePoints);
    if (result.size() >= kMaximumSummaryTextCodePoints)
      return Utf8Prefix(result, kMaximumSummaryTextCodePoints);
  }
  return Utf8Prefix(result, kMaximumSummaryTextCodePoints);
}

EventHistorianError::EventHistorianError(std::string code, std::string message)
    : manyforge::core::TypedError(code, code + ": " + message) {}

const std::string &EventHistorianError::code() const noexcept {
  return manyforge::core::TypedError::code();
}

HistorianEvent MakeHistorianEvent(std::string component, std::string event_type,
                                  std::string severity,
                                  const std::int64_t occurred_at_ns,
                                  std::string occurred_at_wall,
                                  CanonicalValue payload) {
  ValidateText(component, "component", kMaximumIdentityBytes);
  ValidateText(event_type, "event_type", kMaximumIdentityBytes);
  ValidateText(occurred_at_wall, "occurred_at_wall", kMaximumWallTimeBytes);
  const auto dedicated_identity =
      DedicatedEventIdentity(component, event_type, payload);
  const bool session_report = component == "manyforge.workbench" &&
                              event_type == "previous_session_report";
  const auto *session_id = session_report
      ? payload.find_object_member("session_id") : nullptr;
  Require(!session_report || (session_id != nullptr &&
              std::holds_alternative<std::string>(session_id->storage())),
          "historian_event_invalid", "previous-session report identity is absent");
  const auto identity =
      session_report ? CanonicalValue(CanonicalValue::Object{
                           {"session_id", *session_id},
                       }) : dedicated_identity.has_value()
          ? CanonicalValue(CanonicalValue::Object{
                {"model_attempt_id", CanonicalValue(*dedicated_identity)},
            })
          : EventIdentityValue(component, event_type, severity, occurred_at_ns,
                               occurred_at_wall, payload);
  const auto digest = content_identity::framed_sha256(
      session_report ? "manyforge-workbench-previous-session-report-event" :
      dedicated_identity.has_value() ? "manyforge-program-model-attempt-event"
                                     : "manyforge-historian-event",
      session_report || dedicated_identity.has_value() ||
              static_cast<std::uint64_t>(occurred_at_ns) <= kMaximumSafeInteger
          ? "v1"
          : "v2",
      identity);
  HistorianEvent event{
      "event-" + digest.substr(7U, 32U),
      std::move(component),
      std::move(event_type),
      std::move(severity),
      occurred_at_ns,
      std::move(occurred_at_wall),
      std::move(payload),
  };
  ValidateEvent(event);
  return event;
}

HistorianEvent
MakeAlarmLifecycleHistorianEvent(const AlarmLifecycleFact &fact) {
  auto payload = AlarmLifecycleFactValue(fact).as_object();
  payload.emplace_back("clock_owner",
                       CanonicalValue("headless:" + fact.owner_epoch));
  const bool runtime_fault_raised =
      fact.event_type == "alarm_raised" &&
      (fact.definition_id == "manyforge.runtime.program_runner_loss" ||
       fact.definition_id == "manyforge.runtime.operation_journal_write_fault" ||
       fact.definition_id == "manyforge.runtime.kernel_internal_fault");
  return MakeHistorianEvent("manyforge.alarm", fact.event_type,
                            runtime_fault_raised ? "ERROR" : "INFO",
                            fact.occurred_at_ns, fact.occurred_at_wall,
                            CanonicalValue(std::move(payload)));
}

CanonicalValue HistorianEventValue(const HistorianEvent &event) {
  ValidateEvent(event);
  return CanonicalValue(CanonicalValue::Object{
      {"component", CanonicalValue(event.component)},
      {"event_id", CanonicalValue(event.event_id)},
      {"event_type", CanonicalValue(event.event_type)},
      {"occurred_at_ns", CanonicalValue(std::to_string(event.occurred_at_ns))},
      {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
      {"payload", event.payload},
      {"schema_version",
       CanonicalValue(std::string(kHistorianEventSchemaVersion))},
      {"severity", CanonicalValue(event.severity)},
  });
}

CanonicalValue HistorianPersistenceValue(const HistorianEvent &event) {
  ValidateEvent(event);
  const bool legacy =
      static_cast<std::uint64_t>(event.occurred_at_ns) <= kMaximumSafeInteger;
  return CanonicalValue(CanonicalValue::Object{
      {"component", CanonicalValue(event.component)},
      {"event_id", CanonicalValue(event.event_id)},
      {"event_type", CanonicalValue(event.event_type)},
      {"occurred_at_ns", PersistedNanoseconds(event.occurred_at_ns)},
      {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
      {"payload", event.payload},
      {"schema_version",
       CanonicalValue(std::string(legacy ? kHistorianEventV1
                                         : kHistorianEventSchemaVersion))},
      {"severity", CanonicalValue(event.severity)},
  });
}

CanonicalValue HistorianPageValue(const HistorianPage &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &stored : page.events) {
    events.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"event", HistorianEventValue(stored.event)},
        {"sequence", Number(stored.sequence, "sequence")},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"next_before_sequence",
       OptionalNumber(page.next_before_sequence, "next_before_sequence")},
      {"schema_version",
       CanonicalValue(std::string(kHistorianPageSchemaVersion))},
      {"truncated", CanonicalValue(page.truncated)},
  });
}

CanonicalValue HistorianSummaryEventValue(const HistorianEvent& event,
                                          std::optional<std::int64_t> occurred_at_utc_ns,
                                          const std::optional<std::string>& clock_owner,
                                          bool wall_clock_regressed) {
  const auto identities = SummaryIdentityValues(event.payload);
  return CanonicalValue(CanonicalValue::Object{
      {"component", CanonicalValue(event.component)},
      {"condition_id", OptionalString(identities.condition_id)},
      {"condition_severity", CanonicalValue(HistorianConditionSeverity(event))},
      {"clock_owner", OptionalString(clock_owner)},
      {"event_id", CanonicalValue(event.event_id)},
      {"event_type", CanonicalValue(event.event_type)},
      {"message", CanonicalValue(SummaryMessage(event))},
      {"occurred_at_ns", CanonicalValue(std::to_string(event.occurred_at_ns))},
      {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
      {"occurred_at_utc_ns", occurred_at_utc_ns
                                 ? CanonicalValue(std::to_string(*occurred_at_utc_ns))
                                 : CanonicalValue(nullptr)},
      {"wall_clock_regressed", CanonicalValue(wall_clock_regressed)},
      {"operation_id", OptionalString(identities.operation_id)},
      {"program_run_id", OptionalString(identities.program_run_id)},
      {"schema_version", CanonicalValue(std::string(kHistorianEventSummarySchemaVersion))},
      {"severity", CanonicalValue(event.severity)},
  });
}

CanonicalValue HistorianSummaryPageValue(const HistorianPage &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &stored : page.events) {
    ValidateEvent(stored.event);
    events.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"event", HistorianSummaryEventValue(stored.event, stored.occurred_at_utc_ns,
                                             stored.clock_owner, stored.wall_clock_regressed)},
        {"sequence", Number(stored.sequence, "sequence")},
    }));
  }
  return HistorianSummaryEnvelopeValue(std::move(events), page);
}

CanonicalValue HistorianSummaryEnvelopeValue(CanonicalValue::Array events,
                                             const HistorianPage& page) {
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"next_before_sequence",
       OptionalNumber(page.next_before_sequence, "next_before_sequence")},
      {"schema_version",
       CanonicalValue(std::string(kHistorianSummaryPageSchemaVersion))},
      {"retention", page.retention},
      {"truncated", CanonicalValue(page.truncated)},
  });
}

std::string HistorianConditionSeverity(const HistorianEvent &event) {
  const auto *schema = StringMember(event.payload, "schema_version");
  const auto *original = StringMember(event.payload, "original_severity");
  if (schema != nullptr && *schema == "manyforge.client_incident_event.v1" && original != nullptr &&
      (*original == "WARN" || *original == "ERROR" || *original == "CRITICAL")) return *original;
  return event.severity;
}

CanonicalValue HistorianExportPageValue(const HistorianPage &page) {
  // Full diagnostic reads remain paged and byte bounded; they never make the
  // control WebSocket carry event payloads or turn export into an archive.
  CanonicalValue::Array events;
  // Reserve the complete fixed envelope, retained-coverage metadata and the
  // HTTP adapter's exact time-window fields, not just event bodies.
  std::size_t bytes = content_identity::canonical_json(page.retention).size() + 512U;
  std::optional<std::uint64_t> last;
  for (const auto &stored : page.events) {
    CanonicalValue value(CanonicalValue::Object{
        {"sequence", Number(stored.sequence, "sequence")},
        {"event", HistorianEventValue(stored.event)},
        {"persistence_digest", CanonicalValue(content_identity::sha256_digest(
            content_identity::canonical_json(HistorianPersistenceValue(stored.event))))},
        {"clock_owner", OptionalString(stored.clock_owner)},
        {"occurred_at_utc_ns", stored.occurred_at_utc_ns
            ? CanonicalValue(std::to_string(*stored.occurred_at_utc_ns)) : CanonicalValue(nullptr)},
        {"wall_clock_regressed", CanonicalValue(stored.wall_clock_regressed)},
        {"condition_severity", CanonicalValue(HistorianConditionSeverity(stored.event))},
    });
    const auto size = content_identity::canonical_json(value).size();
    if (!events.empty() && (events.size() == 32U || bytes + size + 1U > 1024U * 1024U)) break;
    bytes += size + 1U;
    last = stored.sequence;
    events.push_back(std::move(value));
  }
  const bool truncated = page.truncated || events.size() < page.events.size();
  CanonicalValue::Object envelope{
      {"schema_version", CanonicalValue("manyforge.historian_export_page.v1")},
      {"events", CanonicalValue(std::move(events))},
      {"truncated", CanonicalValue(truncated)},
      {"next_before_sequence", OptionalNumber(truncated ? last : std::nullopt, "next_before_sequence")},
      {"retention", page.retention},
  };
  // A capture export states its range, contents and known limits with the
  // records it pages; it is retained context, not a complete state dump.
  if (!page.capture.is_null()) envelope.emplace_back("capture", page.capture);
  return CanonicalValue(std::move(envelope));
}

CanonicalValue HistorianDetailValue(const HistorianStoredEvent &stored) {
  ValidateSafeInteger(stored.sequence, "history detail sequence");
  ValidateEvent(stored.event);
  const auto section = [](const bool available, std::string reason,
                          CanonicalValue value) {
    return CanonicalValue(CanonicalValue::Object{
        {"available", CanonicalValue(available)},
        {"reason", available ? CanonicalValue(nullptr)
                             : CanonicalValue(std::move(reason))},
        {"value", available ? std::move(value) : CanonicalValue(nullptr)},
    });
  };
  const auto &event = stored.event;
  const auto *schema = event.payload.is_object()
                           ? event.payload.find_object_member("schema_version")
                           : nullptr;
  const auto *schema_text = schema == nullptr
                                ? nullptr
                                : std::get_if<std::string>(&schema->storage());
  const bool operation = schema_text != nullptr &&
                         *schema_text == "manyforge.operation_history_event.v1";
  const auto condition_severity = HistorianConditionSeverity(event);
  const bool incident = event.component == "manyforge.alarm" ||
                        condition_severity == "WARN" || condition_severity == "ERROR" ||
                        condition_severity == "CRITICAL";

  CanonicalValue::Object execution;
  if (operation) {
    for (const auto name :
         {"effect_pending", "native_sequence", "native_sequence_gap",
          "operation_id", "operation_revision", "payload_digest", "record_kind",
          "request_id", "source_owner_epoch", "journal_payload"}) {
      if (const auto *value = event.payload.find_object_member(name);
          value != nullptr) {
        execution.emplace_back(name, *value);
      }
    }
  }

  const CanonicalValue *blackboard = nullptr;
  if (event.payload.is_object()) {
    blackboard = event.payload.find_object_member("blackboard");
    if (blackboard == nullptr) {
      const auto *journal = event.payload.find_object_member("journal_payload");
      if (journal != nullptr && journal->is_object()) {
        blackboard = journal->find_object_member("blackboard");
      }
    }
  }

  const CanonicalValue *latency = nullptr;
  std::string latency_unit;
  if (event.payload.is_object()) {
    for (const auto &[name, unit] :
         {std::pair{"latency_ms", "ms"}, std::pair{"latency_ns", "ns"},
          std::pair{"duration_ns", "ns"}}) {
      if (const auto *value = event.payload.find_object_member(name);
          value != nullptr) {
        latency = value;
        latency_unit = unit;
        break;
      }
    }
  }

  return CanonicalValue(CanonicalValue::Object{
      {"event", HistorianEventValue(event)},
      {"schema_version",
       CanonicalValue(std::string(kHistorianDetailSchemaVersion))},
      {"sections",
       CanonicalValue(CanonicalValue::Object{
           {"blackboard",
            section(blackboard != nullptr,
                    "This event source did not publish a blackboard snapshot.",
                    blackboard == nullptr ? CanonicalValue(nullptr)
                                          : *blackboard)},
           {"execution_digest",
            section(operation,
                    "This event is not an execution or operation record.",
                    CanonicalValue(std::move(execution)))},
           {"incident",
            section(incident, "This event is not an incident record.",
                    CanonicalValue(CanonicalValue::Object{
                        {"component", CanonicalValue(event.component)},
                        {"event_id", CanonicalValue(event.event_id)},
                        {"event_type", CanonicalValue(event.event_type)},
                        {"severity", CanonicalValue(event.severity)},
                        {"condition_severity", CanonicalValue(condition_severity)},
                        {"recorded_facts", event.payload},
                    }))},
           {"latency",
            section(latency != nullptr,
                    "This event source did not publish latency evidence.",
                    latency == nullptr
                        ? CanonicalValue(nullptr)
                        : CanonicalValue(CanonicalValue::Object{
                              {"unit", CanonicalValue(latency_unit)},
                              {"value", *latency},
                          }))},
       })},
      {"sequence", Number(stored.sequence, "history detail sequence")},
  });
}

CanonicalValue AsyncHistorianHealthValue(const AsyncHistorianHealth &health) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_count", Number(health.accepted_count, "accepted_count")},
      {"accepting", CanonicalValue(health.accepting)},
      {"available", CanonicalValue(health.available)},
      {"delivered_count", Number(health.delivered_count, "delivered_count")},
      {"delivery_failure_count",
       Number(health.delivery_failure_count, "delivery_failure_count")},
      {"duplicate_count", Number(health.duplicate_count, "duplicate_count")},
      {"error_code", OptionalString(health.error_code)},
      {"error_message", OptionalString(health.error_message)},
      {"health_generation",
       Number(health.health_generation, "health_generation")},
      {"overflow_count", Number(health.overflow_count, "overflow_count")},
      {"queue_capacity",
       Number(static_cast<std::uint64_t>(health.queue_capacity),
              "queue_capacity")},
      {"queue_depth",
       Number(static_cast<std::uint64_t>(health.queue_depth), "queue_depth")},
      {"recovery_ready", CanonicalValue(health.recovery_ready)},
      {"schema_version", CanonicalValue(health.schema_version)},
      {"write_in_flight", CanonicalValue(health.write_in_flight)},
  });
}

IEventHistoryStore::AppendBatchResult
IEventHistoryStore::AppendBatch(const std::span<const HistorianEvent> events) {
  AppendBatchResult result;
  for (const auto &event : events) {
    try {
      result.statuses.push_back(Append(event));
    } catch (...) {
      result.failure = std::current_exception();
      break;
    }
  }
  return result;
}

namespace {

/// The writer is background work: never a realtime thread, and below normal
/// priority, so it only uses capacity that realtime and control work leave.
/// Lowering priority needs no privilege; a refusal leaves the writer at the
/// inherited priority, which is still correct, only less polite.
void LowerWriterThreadPriority() {
  sched_param normal{};
  normal.sched_priority = 0;
  bool lowered = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal) == 0;
  const auto thread = static_cast<id_t>(::syscall(SYS_gettid));
  errno = 0;
  const auto current = getpriority(PRIO_PROCESS, thread);
  lowered = lowered && errno == 0 &&
            setpriority(PRIO_PROCESS, thread, std::min(19, current + 10)) == 0;
  if (!lowered) {
    // Not a persistence failure: the writer still works at the inherited
    // priority. Reported once so a deployment can see it.
    std::cerr << "historian writer priority could not be lowered; it runs at the inherited priority\n";
  }
}

std::pair<std::string, std::string> StoreFailure(const std::exception_ptr &failure) {
  try {
    std::rethrow_exception(failure);
  } catch (const EventHistorianError &error) {
    return {error.code(), error.what()};
  } catch (const std::exception &error) {
    return {"historian_store_failure", error.what()};
  } catch (...) {
    return {"historian_store_failure", "historian store raised a non-standard exception"};
  }
}

} // namespace

AsyncEventHistorian::AsyncEventHistorian(
    std::shared_ptr<IEventHistoryStore> store, const std::size_t queue_capacity,
    const std::chrono::milliseconds commit_gap)
    : store_(std::move(store)), queue_capacity_(queue_capacity), commit_gap_(commit_gap) {
  Require(store_ != nullptr, "historian_configuration_invalid",
          "historian store is required");
  Require(queue_capacity_ != 0U &&
              queue_capacity_ <= kMaximumHistorianQueueCapacity,
          "historian_configuration_invalid",
          "historian queue capacity must be between 1 and 65536");
  Require(commit_gap_.count() >= 0 && commit_gap_ <= std::chrono::seconds(1),
          "historian_configuration_invalid",
          "historian commit gap must be between 0 and 1000 ms");
  worker_ = std::thread([this]() { Run(); });
}

AsyncEventHistorian::~AsyncEventHistorian() { Shutdown(); }

bool AsyncEventHistorian::TryEnqueue(HistorianEvent event) {
  ValidateEvent(event);
  std::lock_guard lock(mutex_);
  Require(accepting_, "historian_closed", "historian no longer accepts events");
  if (queued_ids_.contains(event.event_id)) {
    // Rare path: only a repeated identity walks the queue to compare content.
    const auto duplicate =
        std::find_if(queue_.begin(), queue_.end(), [&event](const auto &queued) {
          return queued.event_id == event.event_id;
        });
    Require(duplicate != queue_.end(), "historian_internal_failure",
            "queued historian identity index is inconsistent");
    Require(content_identity::canonical_json(HistorianEventValue(*duplicate)) ==
                content_identity::canonical_json(HistorianEventValue(event)),
            "historian_event_identity_conflict",
            "one event identity names conflicting content");
    Advance(duplicate_count_);
    return true;
  }
  if (queue_.size() >= queue_capacity_) {
    Advance(overflow_count_);
    MarkFailureLocked("historian_queue_overflow",
                      "bounded historian delivery queue is full");
    return false;
  }
  queued_ids_.insert(event.event_id);
  queue_.push_back(std::move(event));
  Advance(accepted_count_);
  condition_.notify_one();
  return true;
}

AsyncHistorianHealth AsyncEventHistorian::Health() const {
  std::lock_guard lock(mutex_);
  return {
      "manyforge.async_historian_health.v1",
      !failure_active_,
      failure_active_ && recovery_write_observed_ && health_generation_ < kMaximumSafeInteger,
      accepting_,
      health_generation_,
      error_code_,
      error_message_,
      queue_.size(),
      queue_capacity_,
      write_in_flight_,
      accepted_count_,
      delivered_count_,
      duplicate_count_,
      overflow_count_,
      delivery_failure_count_,
  };
}

void AsyncEventHistorian::ReportDeliveryFailure(std::string code, std::string message) {
  std::lock_guard lock(mutex_);
  Advance(delivery_failure_count_);
  MarkFailureLocked(std::move(code), std::move(message));
}

bool AsyncEventHistorian::WaitUntilIdle(
    const std::chrono::milliseconds timeout) const {
  Require(timeout.count() >= 0, "historian_timeout_invalid",
          "historian idle timeout must be nonnegative");
  std::unique_lock lock(mutex_);
  return idle_condition_.wait_for(
      lock, timeout, [this]() { return queue_.empty() && !write_in_flight_; });
}

bool AsyncEventHistorian::ReconcileHealth(
    const std::uint64_t expected_generation) {
  ValidateSafeInteger(expected_generation, "expected_generation");
  std::lock_guard lock(mutex_);
  Require(expected_generation == health_generation_,
          "historian_health_generation_stale",
          "historian health changed before reconciliation");
  if (!failure_active_) {
    // An already healthy generation has no state change or new audit fact.
    return false;
  }
  if (!recovery_write_observed_) {
    return false;
  }
  if (!AdvanceHealthGenerationLocked()) {
    return false;
  }
  failure_active_ = false;
  recovery_write_observed_ = false;
  error_code_.reset();
  error_message_.reset();
  return true;
}

bool AsyncEventHistorian::AdvanceHealthGenerationLocked() {
  if (health_generation_ < kMaximumSafeInteger) {
    ++health_generation_;
    return true;
  }
  // Diagnostic counters may saturate; a causal generation must never be
  // reused to authorize a later repair. Only a new owner incarnation can
  // replace this exhausted namespace.
  failure_active_ = true;
  recovery_write_observed_ = false;
  error_code_ = "historian_health_generation_exhausted";
  error_message_ = "historian health identity exhausted; owner replacement required";
  return false;
}

void AsyncEventHistorian::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (!accepting_ && !worker_.joinable()) {
      return;
    }
    accepting_ = false;
    stop_requested_ = true;
    condition_.notify_all();
  }
  if (worker_.joinable()) {
    worker_.join();
  }
}

void AsyncEventHistorian::MarkFailureLocked(std::string code,
                                            std::string message) {
  const bool changed =
      !failure_active_ || error_code_ != code || error_message_ != message;
  failure_active_ = true;
  recovery_write_observed_ = false;
  error_code_ = std::move(code);
  error_message_ = std::move(message);
  if (changed) {
    static_cast<void>(AdvanceHealthGenerationLocked());
  }
}

void AsyncEventHistorian::Run() {
  LowerWriterThreadPriority();
  std::unique_lock lock(mutex_);
  while (true) {
    condition_.wait(lock,
                    [this]() { return stop_requested_ || !queue_.empty(); });
    if (queue_.empty()) {
      if (stop_requested_) {
        break;
      }
      continue;
    }
    // At most one commit per gap: events arriving meanwhile join this batch.
    // Shutdown skips the wait so the queue drains promptly.
    if (last_commit_ && !stop_requested_) {
      condition_.wait_until(lock, *last_commit_ + commit_gap_,
                            [this]() { return stop_requested_; });
    }
    write_in_flight_ = true;
    const auto count = std::min(queue_.size(), kMaximumHistorianBatchEvents);
    const std::vector<HistorianEvent> batch(
        queue_.begin(), queue_.begin() + static_cast<std::ptrdiff_t>(count));
    // Losses are counted when they happen, before later events are written;
    // the store may use the cumulative value only to mark coverage uncertain.
    const auto delivery_loss = overflow_count_ + delivery_failure_count_;
    last_commit_ = std::chrono::steady_clock::now();
    lock.unlock();

    IEventHistoryStore::AppendBatchResult result;
    try {
      store_->ObserveDeliveryLoss(delivery_loss);
      result = store_->AppendBatch(batch);
      // Without a failure a store may commit a leading part of the batch
      // (its transaction bound); the rest waits for the next commit.
      if (result.statuses.size() > batch.size() ||
          (!result.failure && result.statuses.empty())) {
        throw EventHistorianError("historian_store_failure",
                                  "historian store reported an inconsistent batch result");
      }
    } catch (...) {
      result = {{}, std::current_exception()};
    }

    lock.lock();
    write_in_flight_ = false;
    for (std::size_t index = 0; index < result.statuses.size(); ++index) {
      if (!queue_.empty() && queue_.front().event_id == batch[index].event_id) {
        queued_ids_.erase(queue_.front().event_id);
        queue_.pop_front();
      }
      if (result.statuses[index] == IEventHistoryStore::AppendStatus::kStored) {
        Advance(delivered_count_);
      } else {
        Advance(duplicate_count_);
      }
    }
    if (!result.statuses.empty() && failure_active_) {
      recovery_write_observed_ = true;
    }
    if (!result.failure) {
      if (queue_.empty()) {
        idle_condition_.notify_all();
      }
      continue;
    }

    auto [error_code, error_message] = StoreFailure(result.failure);
    Advance(delivery_failure_count_);
    MarkFailureLocked(std::move(error_code), std::move(error_message));
    idle_condition_.notify_all();
    if (stop_requested_) {
      queue_.clear();
      queued_ids_.clear();
      idle_condition_.notify_all();
      break;
    }
    condition_.wait_for(lock, std::chrono::milliseconds(50),
                        [this]() { return stop_requested_; });
  }
  write_in_flight_ = false;
  idle_condition_.notify_all();
}

} // namespace manyforge::headless

#include "manyforge_headless/event_historian.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <limits>
#include <set>
#include <string>
#include <utility>

#include <manyforge_content_identity/content_identity.hpp>

namespace manyforge::headless {
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
  std::vector<std::pair<const CanonicalValue *, std::size_t>> pending{
      {&payload, 0U}};
  std::size_t visited = 0U;
  while (!pending.empty() && visited < kMaximumSummaryTraversalNodes) {
    const auto [current, depth] = pending.back();
    pending.pop_back();
    ++visited;
    if (depth > kMaximumSummaryTraversalDepth) {
      continue;
    }
    if (current->is_object()) {
      for (const auto &[name, child] : current->as_object()) {
        std::optional<std::string> *target = nullptr;
        if (name == "program_run_id") {
          target = &result.program_run_id;
        } else if (name == "operation_id") {
          target = &result.operation_id;
        } else if (name == "condition_id" ||
                   name == "operator_alarm_condition_id") {
          target = &result.condition_id;
        }
        if (target != nullptr && !target->has_value()) {
          const auto *text = std::get_if<std::string>(&child.storage());
          if (text != nullptr && !text->empty()) {
            *target = Utf8Prefix(*text, kMaximumSummaryTextCodePoints);
          }
        }
        pending.emplace_back(&child, depth + 1U);
      }
    } else if (current->is_array()) {
      for (const auto &child : current->as_array()) {
        pending.emplace_back(&child, depth + 1U);
      }
    }
  }
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

EventHistorianError::EventHistorianError(std::string code, std::string message)
    : std::runtime_error(code + ": " + message), code_(std::move(code)) {}

const std::string &EventHistorianError::code() const noexcept { return code_; }

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
  const auto identity =
      dedicated_identity.has_value()
          ? CanonicalValue(CanonicalValue::Object{
                {"model_attempt_id", CanonicalValue(*dedicated_identity)},
            })
          : EventIdentityValue(component, event_type, severity, occurred_at_ns,
                               occurred_at_wall, payload);
  const auto digest = content_identity::framed_sha256(
      dedicated_identity.has_value() ? "manyforge-program-model-attempt-event"
                                     : "manyforge-historian-event",
      dedicated_identity.has_value() ||
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
  return MakeHistorianEvent("manyforge.alarm", fact.event_type, "INFO",
                            fact.occurred_at_ns, fact.occurred_at_wall,
                            AlarmLifecycleFactValue(fact));
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

CanonicalValue HistorianSummaryPageValue(const HistorianPage &page) {
  CanonicalValue::Array events;
  events.reserve(page.events.size());
  for (const auto &stored : page.events) {
    ValidateEvent(stored.event);
    const auto identities = SummaryIdentityValues(stored.event.payload);
    events.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"event",
         CanonicalValue(CanonicalValue::Object{
             {"component", CanonicalValue(stored.event.component)},
             {"condition_id", OptionalString(identities.condition_id)},
             {"event_id", CanonicalValue(stored.event.event_id)},
             {"event_type", CanonicalValue(stored.event.event_type)},
             {"message", CanonicalValue(SummaryMessage(stored.event))},
             {"occurred_at_ns",
              CanonicalValue(std::to_string(stored.event.occurred_at_ns))},
             {"occurred_at_wall",
              CanonicalValue(stored.event.occurred_at_wall)},
             {"operation_id", OptionalString(identities.operation_id)},
             {"program_run_id", OptionalString(identities.program_run_id)},
             {"schema_version",
              CanonicalValue(std::string(kHistorianEventSummarySchemaVersion))},
             {"severity", CanonicalValue(stored.event.severity)},
         })},
        {"sequence", Number(stored.sequence, "sequence")},
    }));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"events", CanonicalValue(std::move(events))},
      {"next_before_sequence",
       OptionalNumber(page.next_before_sequence, "next_before_sequence")},
      {"schema_version",
       CanonicalValue(std::string(kHistorianSummaryPageSchemaVersion))},
      {"truncated", CanonicalValue(page.truncated)},
  });
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
  const bool incident = event.component == "manyforge.alarm" ||
                        event.severity == "WARN" || event.severity == "ERROR" ||
                        event.severity == "CRITICAL";

  CanonicalValue::Object execution;
  if (operation) {
    for (const auto name :
         {"effect_pending", "native_sequence", "native_sequence_gap",
          "operation_id", "operation_revision", "payload_digest", "record_kind",
          "request_id", "source_owner_epoch"}) {
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

AsyncEventHistorian::AsyncEventHistorian(
    std::shared_ptr<IEventHistoryStore> store, const std::size_t queue_capacity)
    : store_(std::move(store)), queue_capacity_(queue_capacity) {
  Require(store_ != nullptr, "historian_configuration_invalid",
          "historian store is required");
  Require(queue_capacity_ != 0U &&
              queue_capacity_ <= kMaximumHistorianQueueCapacity,
          "historian_configuration_invalid",
          "historian queue capacity must be between 1 and 65536");
  worker_ = std::thread([this]() { Run(); });
}

AsyncEventHistorian::~AsyncEventHistorian() { Shutdown(); }

bool AsyncEventHistorian::TryEnqueue(HistorianEvent event) {
  ValidateEvent(event);
  std::lock_guard lock(mutex_);
  Require(accepting_, "historian_closed", "historian no longer accepts events");
  const auto duplicate =
      std::find_if(queue_.begin(), queue_.end(), [&event](const auto &queued) {
        return queued.event_id == event.event_id;
      });
  if (duplicate != queue_.end()) {
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
      failure_active_ && recovery_write_observed_,
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
    return true;
  }
  if (!recovery_write_observed_) {
    return false;
  }
  failure_active_ = false;
  recovery_write_observed_ = false;
  error_code_.reset();
  error_message_.reset();
  Advance(health_generation_);
  return true;
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
    Advance(health_generation_);
  }
}

void AsyncEventHistorian::Run() {
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
    write_in_flight_ = true;
    const auto event = queue_.front();
    lock.unlock();

    std::optional<IEventHistoryStore::AppendStatus> status;
    std::string error_code;
    std::string error_message;
    try {
      status = store_->Append(event);
    } catch (const EventHistorianError &error) {
      error_code = error.code();
      error_message = error.what();
    } catch (const std::exception &error) {
      error_code = "historian_store_failure";
      error_message = error.what();
    } catch (...) {
      error_code = "historian_store_failure";
      error_message = "historian store raised a non-standard exception";
    }

    lock.lock();
    write_in_flight_ = false;
    if (status.has_value()) {
      if (!queue_.empty() && queue_.front().event_id == event.event_id) {
        queue_.pop_front();
      }
      if (*status == IEventHistoryStore::AppendStatus::kStored) {
        Advance(delivered_count_);
      } else {
        Advance(duplicate_count_);
      }
      if (failure_active_) {
        recovery_write_observed_ = true;
      }
      if (queue_.empty()) {
        idle_condition_.notify_all();
      }
      continue;
    }

    Advance(delivery_failure_count_);
    MarkFailureLocked(std::move(error_code), std::move(error_message));
    idle_condition_.notify_all();
    if (stop_requested_) {
      queue_.clear();
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

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
constexpr std::size_t kMaximumIdentityBytes = 1024U;
constexpr std::size_t kMaximumWallTimeBytes = 128U;
constexpr std::size_t kMaximumHistorianQueueCapacity = 65536U;

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

void ValidateEvent(const HistorianEvent &event) {
  ValidateText(event.event_id, "event_id", kMaximumIdentityBytes);
  ValidateText(event.component, "component", kMaximumIdentityBytes);
  ValidateText(event.event_type, "event_type", kMaximumIdentityBytes);
  Require(event.severity == "DEBUG" || event.severity == "INFO" ||
              event.severity == "WARN" || event.severity == "ERROR" ||
              event.severity == "CRITICAL",
          "historian_event_invalid", "event severity is unsupported");
  ValidateSafeInteger(event.occurred_at_ns, "occurred_at_ns");
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
      {"occurred_at_ns", Number(occurred_at_ns, "occurred_at_ns")},
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
      "v1", identity);
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
      {"occurred_at_ns", Number(event.occurred_at_ns, "occurred_at_ns")},
      {"occurred_at_wall", CanonicalValue(event.occurred_at_wall)},
      {"payload", event.payload},
      {"schema_version",
       CanonicalValue(std::string(kHistorianEventSchemaVersion))},
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

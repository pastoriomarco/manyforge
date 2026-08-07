#include "manyforge_headless/headless_api_core.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
constexpr std::size_t kMaximumReplayCapacity = 65536U;
constexpr std::size_t kOwnerEpochRandomBytes = 16U;
constexpr std::string_view kNodeEnvironmentProjectionIdentityDomain =
    "manyforge-node-environment-projection-semantic";
constexpr std::string_view kHistorianOwnerHealthCode =
    "historian_delivery_unavailable";

[[noreturn]] void CoreFail(const std::string &code,
                           const std::string &message) {
  throw OperatorAlarmError(code, message);
}

void Require(const bool condition, const std::string &code,
             const std::string &message) {
  if (!condition) {
    CoreFail(code, message);
  }
}

HeadlessApiCoreConfig ValidateConfig(HeadlessApiCoreConfig config) {
  Require(!config.publication_root.empty() && !config.package_root.empty() &&
              !config.node_catalog_path.empty() &&
              !config.alarm_catalog_path.empty() &&
              !config.historian_database_path.empty(),
          "headless_api_configuration_invalid",
          "publication, package, node-catalog, alarm-catalog, and historian "
          "paths are required");
  Require(!config.cell_id.empty() && !config.owner_id.empty() &&
              !config.owner_epoch.empty(),
          "headless_api_configuration_invalid",
          "cell and owner identities are required");
  Require(config.mutation_replay_capacity != 0U &&
              config.mutation_replay_capacity <= kMaximumReplayCapacity,
          "headless_api_configuration_invalid",
          "mutation replay capacity must be between 1 and 65536");
  return config;
}

HeadlessApiCoreClocks ResolveClocks(HeadlessApiCoreClocks clocks) {
  const bool any = static_cast<bool>(clocks.wall_time_ns) ||
                   static_cast<bool>(clocks.monotonic_time_ns) ||
                   static_cast<bool>(clocks.wall_time_text);
  if (!any) {
    return ProductionHeadlessApiClocks();
  }
  Require(static_cast<bool>(clocks.wall_time_ns) &&
              static_cast<bool>(clocks.monotonic_time_ns) &&
              static_cast<bool>(clocks.wall_time_text),
          "headless_api_configuration_invalid",
          "custom headless API clocks must be supplied as one complete set");
  return clocks;
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *value = root.find_object_member(name);
  Require(value != nullptr, "headless_source_invalid",
          "headless source is missing field " + std::string(name));
  return *value;
}

const CanonicalValue &RequiredSection(const CanonicalValue &runtime,
                                      const std::string_view name) {
  const auto &sections = RequiredMember(runtime, "sections");
  Require(sections.is_object(), "headless_source_invalid",
          "headless runtime sections must be an object");
  return RequiredMember(sections, name);
}

std::string RequiredString(const CanonicalValue &root,
                           const std::string_view name) {
  const auto *value =
      std::get_if<std::string>(&RequiredMember(root, name).storage());
  Require(value != nullptr && !value->empty(), "headless_source_invalid",
          "headless source field " + std::string(name) +
              " must be a non-empty string");
  return *value;
}

std::optional<std::string> OptionalStringMember(const CanonicalValue &root,
                                                const std::string_view name) {
  const auto &member = RequiredMember(root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return std::nullopt;
  }
  const auto *value = std::get_if<std::string>(&member.storage());
  Require(value != nullptr && !value->empty(), "headless_source_invalid",
          "headless source optional string is invalid");
  return *value;
}

bool RequiredBool(const CanonicalValue &root, const std::string_view name) {
  const auto *value = std::get_if<bool>(&RequiredMember(root, name).storage());
  Require(value != nullptr, "headless_source_invalid",
          "headless source field " + std::string(name) + " must be boolean");
  return *value;
}

std::vector<std::string> StringArray(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *values =
      std::get_if<CanonicalValue::Array>(&RequiredMember(root, name).storage());
  Require(values != nullptr, "headless_source_invalid",
          "headless source field " + std::string(name) + " must be an array");
  std::vector<std::string> result;
  result.reserve(values->size());
  for (const auto &value : *values) {
    const auto *text = std::get_if<std::string>(&value.storage());
    Require(text != nullptr && !text->empty(), "headless_source_invalid",
            "headless source string array is invalid");
    result.push_back(*text);
  }
  return result;
}

CanonicalValue StringArrayValue(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  result.reserve(values.size());
  for (const auto &value : values) {
    result.emplace_back(CanonicalValue(value));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue OptionalStringValue(const std::optional<std::string> &value) {
  return value.has_value() ? CanonicalValue(*value) : CanonicalValue(nullptr);
}

CanonicalValue Number(const std::uint64_t value, const std::string_view label) {
  Require(value <= kMaximumSafeInteger, "headless_value_invalid",
          std::string(label) + " exceeds the I-JSON safe integer range");
  return CanonicalValue(static_cast<double>(value));
}

std::uint64_t RequiredCounter(const CanonicalValue &root,
                              const std::string_view name,
                              const std::uint64_t minimum = 0U) {
  const auto *value =
      std::get_if<double>(&RequiredMember(root, name).storage());
  Require(value != nullptr && std::isfinite(*value) &&
              *value >= static_cast<double>(minimum) &&
              std::floor(*value) == *value &&
              *value <= static_cast<double>(kMaximumSafeInteger),
          "headless_operation_event_invalid",
          "operation event field " + std::string(name) +
              " must be a safe unsigned integer");
  return static_cast<std::uint64_t>(*value);
}

const CanonicalValue::Array &RequiredArray(const CanonicalValue &root,
                                           const std::string_view name) {
  const auto *value =
      std::get_if<CanonicalValue::Array>(&RequiredMember(root, name).storage());
  Require(value != nullptr, "headless_operation_event_invalid",
          "operation event field " + std::string(name) + " must be an array");
  return *value;
}

bool IsOperationFaultState(const std::string_view state) noexcept {
  return state == "failed" || state == "interrupted" ||
         state == "intervention_required";
}

bool IsNormalOperationTerminal(const std::string_view state) noexcept {
  return state == "completed" || state == "stopped" ||
         state == "cancelled_pre_dispatch";
}

std::string OperationHistoryEventType(const std::string_view record_kind,
                                      const CanonicalValue &payload) {
  if (record_kind == "operation_accepted") {
    return "operation_accepted";
  }
  if (record_kind == "stop_accepted") {
    return "operation_stop_accepted";
  }
  if (record_kind == "runtime_scale_accepted") {
    return "runtime_scale_requested";
  }
  if (record_kind == "control_accepted") {
    return "operation_" +
           RequiredString(RequiredMember(payload, "result"), "control") +
           "_accepted";
  }
  const auto &state = RequiredMember(payload, "operation_state");
  return "operation_" + RequiredString(state, "state");
}

CanonicalValue MembershipValue(const AlarmAcknowledgementMember &member) {
  return CanonicalValue(CanonicalValue::Object{
      {"activation_id", CanonicalValue(member.activation_id)},
      {"condition_id", CanonicalValue(member.condition_id)},
      {"record_revision", Number(member.record_revision, "record_revision")},
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

std::string ReplayKey(const AlarmCallerAttribution &caller) {
  auto result = caller.request_owner_epoch;
  result.push_back('\0');
  result += caller.request_id;
  return result;
}

CanonicalValue CallerValue(const AlarmCallerAttribution &caller) {
  return CanonicalValue(CanonicalValue::Object{
      {"adapter_id", CanonicalValue(caller.adapter_id)},
      {"assurance", CanonicalValue(caller.assurance)},
      {"correlation_id", OptionalStringValue(caller.correlation_id)},
      {"request_id", CanonicalValue(caller.request_id)},
      {"request_owner_epoch", CanonicalValue(caller.request_owner_epoch)},
      {"source_label", CanonicalValue(caller.source_label)},
      {"transport_kind", CanonicalValue(caller.transport_kind)},
      {"trusted_principal_id",
       OptionalStringValue(caller.trusted_principal_id)},
  });
}

std::string RequestDigest(const CanonicalValue &request) {
  return content_identity::framed_sha256("manyforge-headless-api-request", "v1",
                                         request);
}

bool SectionFresh(const CanonicalValue &section) {
  return RequiredBool(section, "available") &&
         RequiredString(section, "freshness") == "fresh";
}

bool HasLastValidPayload(const CanonicalValue &runtime,
                         const std::string_view section_name) {
  const auto &payload =
      RequiredMember(RequiredSection(runtime, section_name), "payload");
  if (std::holds_alternative<std::nullptr_t>(payload.storage())) {
    return false;
  }
  Require(payload.is_object(), "headless_source_invalid",
          "headless source last-valid payload must be an object");
  return true;
}

std::string ProductionWallText() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::time_point_cast<std::chrono::seconds>(now);
  const auto nanoseconds =
      std::chrono::duration_cast<std::chrono::nanoseconds>(now - seconds)
          .count();
  const std::time_t raw = std::chrono::system_clock::to_time_t(seconds);
  std::tm value{};
  if (::gmtime_r(&raw, &value) == nullptr) {
    throw std::runtime_error("headless API wall clock conversion failed");
  }
  std::array<char, 64> output{};
  const auto count = std::snprintf(
      output.data(), output.size(), "%04d-%02d-%02dT%02d:%02d:%02d.%09lldZ",
      value.tm_year + 1900, value.tm_mon + 1, value.tm_mday, value.tm_hour,
      value.tm_min, value.tm_sec, static_cast<long long>(nanoseconds));
  if (count <= 0 || static_cast<std::size_t>(count) >= output.size()) {
    throw std::runtime_error("headless API wall clock formatting failed");
  }
  return std::string(output.data(), static_cast<std::size_t>(count));
}

} // namespace

class HeadlessApiOwnerLock final {
public:
  explicit HeadlessApiOwnerLock(
      const std::filesystem::path &historian_database_path) {
    auto lock_path = historian_database_path;
    lock_path += ".headless-owner.lock";
    const int descriptor =
        ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW,
               S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
      CoreFail("headless_owner_lock_unavailable",
               std::string("cannot open the headless owner lock: ") +
                   std::system_error(errno, std::generic_category()).what());
    }
    struct stat status {};
    if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
        status.st_uid != ::geteuid()) {
      static_cast<void>(::close(descriptor));
      CoreFail("headless_owner_lock_unavailable",
               "headless owner lock is not a same-user regular file");
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
      const int error = errno;
      static_cast<void>(::close(descriptor));
      if (error == EWOULDBLOCK || error == EAGAIN) {
        CoreFail("headless_owner_conflict",
                 "another headless Alarm Lifecycle Owner already owns this "
                 "historian identity");
      }
      CoreFail("headless_owner_lock_unavailable",
               std::string("cannot acquire the headless owner lock: ") +
                   std::system_error(error, std::generic_category()).what());
    }
    descriptor_ = descriptor;
  }

  ~HeadlessApiOwnerLock() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  HeadlessApiOwnerLock(const HeadlessApiOwnerLock &) = delete;
  HeadlessApiOwnerLock &operator=(const HeadlessApiOwnerLock &) = delete;

private:
  int descriptor_{-1};
};

HeadlessApiCoreClocks ProductionHeadlessApiClocks() {
  const auto monotonic_origin =
      std::make_shared<std::chrono::steady_clock::time_point>(
          std::chrono::steady_clock::now());
  return {
      []() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
      },
      [monotonic_origin]() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::steady_clock::now() - *monotonic_origin)
            .count();
      },
      []() { return ProductionWallText(); },
  };
}

std::string MintHeadlessOwnerEpoch(const std::string_view prefix) {
  Require(!prefix.empty() && prefix.size() <= 128U,
          "headless_api_configuration_invalid",
          "owner epoch prefix is invalid");
  std::array<unsigned char, kOwnerEpochRandomBytes> bytes{};
  std::size_t offset = 0U;
  while (offset < bytes.size()) {
    const auto count =
        ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0 && errno == EINTR) {
      continue;
    }
    if (count <= 0) {
      throw std::system_error(errno, std::generic_category(),
                              "mint headless owner epoch");
    }
    offset += static_cast<std::size_t>(count);
  }
  constexpr char kHex[] = "0123456789abcdef";
  std::string result(prefix);
  result.reserve(result.size() + bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[(byte >> 4U) & 0x0fU]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

HeadlessApiCore::HeadlessApiCore(HeadlessApiCoreConfig config,
                                 HeadlessApiCoreClocks clocks)
    : config_(ValidateConfig(std::move(config))),
      clocks_(ResolveClocks(std::move(clocks))),
      owner_lock_(std::make_unique<HeadlessApiOwnerLock>(
          config_.historian_database_path)),
      retained_bundle_source_(config_.package_root),
      node_environment_projection_(config_.package_root,
                                   config_.node_catalog_path),
      history_store_(std::make_shared<SqliteEventHistoryStore>(
          config_.historian_database_path, config_.historian_record_retention)),
      historian_(history_store_, config_.historian_queue_capacity),
      runtime_aggregator_(config_.publication_root, config_.cell_id,
                          config_.owner_id + ".projection", config_.owner_epoch,
                          config_.freshness_policy, clocks_.wall_time_ns,
                          clocks_.monotonic_time_ns),
      alarm_owner_(OperatorAlarmCatalog::Load(config_.alarm_catalog_path),
                   config_.cell_id, config_.resource_ids,
                   config_.owner_id + ".alarm", config_.owner_epoch,
                   clocks_.monotonic_time_ns, clocks_.wall_time_text),
      runtime_health_adapter_{
          config_.owner_id + ".runtime-health-adapter",
          "runtime_health",
          "manyforge.alarm_adapter.runtime_health_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      },
      scene_adapter_{
          config_.owner_id + ".scene-initialization-adapter",
          "runtime_health",
          "manyforge.alarm_adapter.runtime_health_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      },
      operation_adapter_{
          config_.owner_id + ".operation-condition-adapter",
          "motion",
          "manyforge.alarm_adapter.kernel_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      } {
  RegisterAdapterEpochLocked(runtime_health_adapter_,
                             config_.owner_epoch + ".runtime-health");
}

HeadlessApiCore::~HeadlessApiCore() {
  try {
    Shutdown();
  } catch (...) {
  }
}

CanonicalValue HeadlessApiCore::RuntimeSnapshot() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return RefreshLocked().root;
}

CanonicalValue HeadlessApiCore::NodeEnvironmentProjection() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return NodeEnvironmentProjectionLocked(RefreshLocked());
}

CanonicalValue
HeadlessApiCore::AlarmSnapshot(const std::optional<std::size_t> limit,
                               const std::set<AlarmLifecycleState> &states,
                               const AlarmSnapshotOrdering ordering) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  static_cast<void>(RefreshLocked());
  return OperatorAlarmSnapshotValue(
      alarm_owner_.Snapshot(limit, states, ordering));
}

CanonicalValue HeadlessApiCore::AlarmMetrics() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  static_cast<void>(RefreshLocked());
  return OperatorAlarmMetricsValue(alarm_owner_.Metrics());
}

CanonicalValue HeadlessApiCore::AcknowledgeAlarm(
    std::string condition_id, std::string activation_id,
    const std::uint64_t expected_record_revision, AlarmCallerAttribution caller,
    std::optional<std::string> reason, const bool confirmed) {
  const CanonicalValue request(CanonicalValue::Object{
      {"activation_id", CanonicalValue(activation_id)},
      {"caller", CallerValue(caller)},
      {"condition_id", CanonicalValue(condition_id)},
      {"confirmed", CanonicalValue(confirmed)},
      {"expected_record_revision",
       Number(expected_record_revision, "expected_record_revision")},
      {"reason", OptionalStringValue(reason)},
  });
  const auto digest = RequestDigest(request);
  const auto key = ReplayKey(caller);
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "alarm acknowledgement targets a stale headless owner epoch");
  if (const auto replay = LookupReplayLocked(key, digest); replay.has_value()) {
    return *replay;
  }
  RequireReplayCapacityLocked();
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.Acknowledge(
      condition_id, activation_id, expected_record_revision, caller,
      std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
  SynchronizeHistorianHealthLocked();
  auto response = MutationResponse(mutation, audit_ids);
  StoreReplayLocked(key, digest, response);
  return response;
}

CanonicalValue HeadlessApiCore::AcknowledgeAllAlarms(
    std::vector<AlarmAcknowledgementMember> members,
    const std::uint64_t expected_generation, AlarmCallerAttribution caller,
    std::optional<std::string> reason, const bool confirmed) {
  Require(members.size() <= kMaximumAlarmBulkMembers,
          "operator_alarm_acknowledge_all_members_invalid",
          "acknowledge-all membership exceeds the 256-member bound");
  const CanonicalValue request(CanonicalValue::Object{
      {"caller", CallerValue(caller)},
      {"confirmed", CanonicalValue(confirmed)},
      {"expected_generation",
       Number(expected_generation, "expected_generation")},
      {"members", MembershipArray(members)},
      {"reason", OptionalStringValue(reason)},
  });
  const auto digest = RequestDigest(request);
  const auto key = ReplayKey(caller);
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "bulk alarm acknowledgement targets a stale headless owner epoch");
  if (const auto replay = LookupReplayLocked(key, digest); replay.has_value()) {
    return *replay;
  }
  RequireReplayCapacityLocked();
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.AcknowledgeAll(
      std::move(members), caller.request_owner_epoch, expected_generation,
      caller, std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
  SynchronizeHistorianHealthLocked();
  auto response = MutationResponse(mutation, audit_ids);
  StoreReplayLocked(key, digest, response);
  return response;
}

CanonicalValue HeadlessApiCore::HistoryPage(const HistorianQuery &query) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    SynchronizeHistorianHealthLocked();
    store = history_store_;
  }
  // History storage is intentionally outside the core coordination lock.
  // A slow/busy query may delay that observer, but cannot hold up alarm
  // acknowledgement, source refresh, or non-blocking lifecycle delivery.
  return HistorianPageValue(store->Query(query));
}

CanonicalValue HeadlessApiCore::HistorianHealth() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  SynchronizeHistorianHealthLocked();
  return AsyncHistorianHealthValue(historian_.Health());
}

CanonicalValue HeadlessApiCore::ReconcileHistorianHealth(
    const std::uint64_t expected_health_generation,
    AlarmCallerAttribution caller) {
  const CanonicalValue request(CanonicalValue::Object{
      {"caller", CallerValue(caller)},
      {"expected_health_generation",
       Number(expected_health_generation, "expected_health_generation")},
      {"operation", CanonicalValue("reconcile_historian_health")},
  });
  const auto digest = RequestDigest(request);
  const auto key = ReplayKey(caller);
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "historian reconciliation targets a stale headless owner epoch");
  if (const auto replay = LookupReplayLocked(key, digest); replay.has_value()) {
    return *replay;
  }
  RequireReplayCapacityLocked();
  const bool reconciled =
      historian_.ReconcileHealth(expected_health_generation);
  std::vector<std::string> audit_event_ids;
  if (reconciled) {
    alarm_owner_.ReconcileHealth(kHistorianOwnerHealthCode);
    auto event = MakeHistorianEvent(
        "manyforge.historian", "historian_health_reconciled", "INFO",
        clocks_.monotonic_time_ns(), clocks_.wall_time_text(),
        CanonicalValue(CanonicalValue::Object{
            {"caller", CallerValue(caller)},
            {"expected_health_generation",
             Number(expected_health_generation, "expected_health_generation")},
            {"schema_version",
             CanonicalValue(
                 "manyforge.historian_health_reconciliation_event.v1")},
        }));
    audit_event_ids.push_back(event.event_id);
    if (!historian_.TryEnqueue(std::move(event))) {
      alarm_owner_.MarkHealthUnavailable(
          std::string(kHistorianOwnerHealthCode),
          "historian delivery queue rejected its reconciliation audit fact");
    }
  }
  SynchronizeHistorianHealthLocked();
  auto response = CanonicalValue(CanonicalValue::Object{
      {"audit_event_ids", StringArrayValue(audit_event_ids)},
      {"health", AsyncHistorianHealthValue(historian_.Health())},
      {"reconciled", CanonicalValue(reconciled)},
      {"schema_version",
       CanonicalValue("manyforge.historian_reconciliation.v1")},
  });
  StoreReplayLocked(key, digest, response);
  return response;
}

CanonicalValue HeadlessApiCore::IngestOperationEvent(
    std::string source_owner_epoch, const std::uint64_t source_sequence,
    std::string record_kind, std::optional<std::string> request_id,
    std::string operation_id, const std::uint64_t operation_revision,
    const std::int64_t occurred_at_ns, std::string occurred_at_wall,
    const bool effect_pending, CanonicalValue journal_payload,
    std::string payload_digest) {
  Require(
      !source_owner_epoch.empty() && source_sequence != 0U &&
          source_sequence <= kMaximumSafeInteger && !record_kind.empty() &&
          !operation_id.empty() && operation_revision != 0U &&
          operation_revision <= kMaximumSafeInteger && occurred_at_ns >= 0 &&
          static_cast<std::uint64_t>(occurred_at_ns) <= kMaximumSafeInteger &&
          !occurred_at_wall.empty() && occurred_at_wall.size() <= 128U &&
          content_identity::is_utf8_nfc(occurred_at_wall) &&
          (!request_id.has_value() || !request_id->empty()) &&
          content_identity::is_sha256_digest(payload_digest) &&
          journal_payload.is_object(),
      "headless_operation_event_invalid",
      "operation event envelope is malformed");
  const std::set<std::string_view> supported_kinds{
      "operation_accepted",     "control_accepted", "stop_accepted",
      "runtime_scale_accepted", "state_observed",   "host_replaced"};
  Require(supported_kinds.contains(record_kind),
          "headless_operation_event_invalid",
          "operation event record kind is unsupported");
  const auto payload_json = content_identity::canonical_json(journal_payload);
  Require(content_identity::sha256_digest(payload_json) == payload_digest &&
              RequiredString(journal_payload, "api_version") ==
                  "manyforge.operation_journal_record.v1" &&
              RequiredString(journal_payload, "owner_epoch") ==
                  source_owner_epoch &&
              RequiredString(journal_payload, "record_kind") == record_kind &&
              RequiredBool(journal_payload, "effect_pending") == effect_pending,
          "headless_operation_event_invalid",
          "operation event payload does not match its durable envelope");
  const auto &operation_state =
      RequiredMember(journal_payload, "operation_state");
  Require(operation_state.is_object() &&
              RequiredString(operation_state, "operation_id") == operation_id &&
              RequiredCounter(operation_state, "operation_revision", 1U) ==
                  operation_revision,
          "headless_operation_event_invalid",
          "operation event state does not match its durable envelope");
  const auto event_type =
      OperationHistoryEventType(record_kind, journal_payload);
  const auto state_name = RequiredString(operation_state, "state");
  const auto severity = IsOperationFaultState(state_name) ? "ERROR" : "INFO";
  const auto native_record_digest = content_identity::framed_sha256(
      "manyforge-operation-journal-event", "v1",
      CanonicalValue(CanonicalValue::Object{
          {"effect_pending", CanonicalValue(effect_pending)},
          {"occurred_at_ns", Number(static_cast<std::uint64_t>(occurred_at_ns),
                                    "occurred_at_ns")},
          {"occurred_at_wall", CanonicalValue(occurred_at_wall)},
          {"operation_id", CanonicalValue(operation_id)},
          {"operation_revision",
           Number(operation_revision, "operation_revision")},
          {"payload_digest", CanonicalValue(payload_digest)},
          {"record_kind", CanonicalValue(record_kind)},
          {"request_id", OptionalStringValue(request_id)},
          {"source_owner_epoch", CanonicalValue(source_owner_epoch)},
          {"source_sequence", Number(source_sequence, "source_sequence")},
      }));

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  RegisterOperationEpochLocked(source_owner_epoch);
  if (source_sequence <= operation_native_sequence_) {
    if (source_sequence == operation_native_sequence_) {
      Require(native_record_digest == operation_native_record_digest_,
              "headless_operation_event_identity_conflict",
              "one native operation event sequence names conflicting content");
    }
    return CanonicalValue(CanonicalValue::Object{
        {"event_id", CanonicalValue(nullptr)},
        {"native_sequence", Number(source_sequence, "native_sequence")},
        {"schema_version",
         CanonicalValue("manyforge.operation_event_ingest.v1")},
        {"status", CanonicalValue("already_ingested")},
    });
  }

  const bool native_sequence_gap =
      operation_native_sequence_ != 0U &&
      source_sequence > operation_native_sequence_ + 1U;
  CanonicalValue history_payload(CanonicalValue::Object{
      {"effect_pending", CanonicalValue(effect_pending)},
      {"journal_payload", journal_payload},
      {"native_sequence", Number(source_sequence, "native_sequence")},
      {"native_sequence_gap", CanonicalValue(native_sequence_gap)},
      {"operation_id", CanonicalValue(operation_id)},
      {"operation_revision", Number(operation_revision, "operation_revision")},
      {"payload_digest", CanonicalValue(payload_digest)},
      {"record_kind", CanonicalValue(record_kind)},
      {"request_id", OptionalStringValue(request_id)},
      {"schema_version",
       CanonicalValue("manyforge.operation_history_event.v1")},
      {"source_owner_epoch", CanonicalValue(source_owner_epoch)},
  });
  std::optional<std::string> event_id;
  try {
    auto event = MakeHistorianEvent("manyforge.runtime", event_type, severity,
                                    occurred_at_ns, occurred_at_wall,
                                    std::move(history_payload));
    const auto candidate_event_id = event.event_id;
    if (historian_.TryEnqueue(std::move(event))) {
      event_id = candidate_event_id;
    } else {
      alarm_owner_.MarkHealthUnavailable(
          std::string(kHistorianOwnerHealthCode),
          "historian delivery queue rejected a committed operation fact");
    }
  } catch (const std::exception &error) {
    alarm_owner_.MarkHealthUnavailable(std::string(kHistorianOwnerHealthCode),
                                       error.what());
  } catch (...) {
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        "operation historian delivery raised an unknown failure");
  }

  ReconcileOperationFaultLocked(
      journal_payload, record_kind, request_id.value_or(""), source_sequence,
      payload_digest, occurred_at_ns, occurred_at_wall, event_id.value_or(""));
  operation_native_sequence_ = source_sequence;
  operation_native_record_digest_ = native_record_digest;
  SynchronizeHistorianHealthLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", OptionalStringValue(event_id)},
      {"native_sequence", Number(source_sequence, "native_sequence")},
      {"schema_version", CanonicalValue("manyforge.operation_event_ingest.v1")},
      {"status", CanonicalValue("ingested")},
  });
}

CanonicalValue HeadlessApiCore::CompleteOperationEventSnapshot(
    std::string source_owner_epoch,
    const std::uint64_t through_source_sequence) {
  Require(!source_owner_epoch.empty() &&
              through_source_sequence <= kMaximumSafeInteger,
          "headless_operation_event_invalid",
          "operation event snapshot identity is invalid");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  RegisterOperationEpochLocked(source_owner_epoch);
  Require(
      through_source_sequence == operation_native_sequence_,
      "headless_operation_event_snapshot_incomplete",
      "operation event snapshot completion is ahead of or behind ingestion");

  std::vector<std::string> retired;
  for (const auto &[operation, subject] : active_operation_faults_) {
    if (observed_operation_faults_.contains(operation)) {
      continue;
    }
    Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
            "headless_adapter_sequence_exhausted",
            "operation alarm adapter sequence is exhausted");
    ++operation_adapter_.source_sequence;
    AlarmConditionUpdate update{
        "manyforge.motion.execution_fault",
        subject,
        false,
        operation_adapter_.source_id,
        operation_adapter_.source_epoch,
        operation_adapter_.source_sequence,
        clocks_.monotonic_time_ns(),
        clocks_.wall_time_text(),
        AlarmConditionQuality::kGood,
        CanonicalValue(CanonicalValue::Object{}),
        CanonicalValue(CanonicalValue::Object{
            {"reason", CanonicalValue("execution_context_retired")},
        }),
        {},
        CanonicalValue(CanonicalValue::Object{
            {"operation_id", CanonicalValue(operation)},
        }),
    };
    const auto mutation = alarm_owner_.Submit(std::move(update));
    static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
    retired.push_back(operation);
  }
  for (const auto &operation : retired) {
    active_operation_faults_.erase(operation);
  }
  alarm_owner_.CompleteSourceSnapshot(operation_adapter_.source_id,
                                      operation_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
  SynchronizeHistorianHealthLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"native_sequence", Number(through_source_sequence, "native_sequence")},
      {"schema_version",
       CanonicalValue("manyforge.operation_event_snapshot.v1")},
      {"source_owner_epoch", CanonicalValue(source_owner_epoch)},
      {"status", CanonicalValue("complete")},
  });
}

CanonicalValue HeadlessApiCore::MarkOperationEventSourceUnknown(
    std::string source_owner_epoch) {
  Require(!source_owner_epoch.empty(), "headless_operation_event_invalid",
          "operation event source identity is empty");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const bool current = operation_adapter_.source_epoch == source_owner_epoch;
  if (current) {
    alarm_owner_.MarkSourceQualityUnknown(operation_adapter_.source_id,
                                          operation_adapter_.source_epoch);
    SynchronizeHistorianHealthLocked();
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.operation_event_source_quality.v1")},
      {"source_owner_epoch", CanonicalValue(source_owner_epoch)},
      {"status", CanonicalValue(current ? "marked_unknown" : "stale_ignored")},
  });
}

void HeadlessApiCore::Shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (shutdown_) {
      return;
    }
    shutdown_ = true;
  }
  historian_.Shutdown();
  history_store_->Close();
  owner_lock_.reset();
}

HeadlessRuntimeSnapshot HeadlessApiCore::RefreshLocked() {
  auto runtime = runtime_aggregator_.Evaluate();
  RefreshRuntimeHealthAdapterLocked(runtime);
  RefreshSceneAdapterLocked(runtime);
  SynchronizeHistorianHealthLocked();
  return runtime;
}

CanonicalValue HeadlessApiCore::NodeEnvironmentProjectionLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  if (std::holds_alternative<std::nullptr_t>(manager_payload.storage())) {
    CoreFail("node_environment_projection_unavailable",
             "Manager has no current or last-valid active-bundle selection");
  }
  Require(manager_payload.is_object(), "node_environment_projection_invalid",
          "Manager active-bundle selection is not an object");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto retained_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto graph_revision = RequiredString(active, "graph_revision");
  const auto manager_ready = RequiredBool(manager_payload, "manager_ready");
  const auto &manager_runtime = RequiredMember(manager_payload, "runtime");
  const auto manager_converged = RequiredBool(manager_runtime, "converged");
  Require(manager_ready == manager_converged,
          "node_environment_projection_invalid",
          "Manager readiness and runtime convergence disagree");

  std::shared_ptr<const manyforge::deployment::RetainedBundle> active_bundle;
  try {
    active_bundle = retained_bundle_source_.get_verified(
        manyforge::core::RetainedBundleId(retained_bundle_id));
  } catch (const std::exception &error) {
    CoreFail("node_environment_projection_invalid",
             "the selected retained bundle failed independent verification: " +
                 std::string(error.what()));
  }
  if (active_bundle == nullptr) {
    CoreFail("node_environment_projection_unavailable",
             "the Manager-selected retained bundle is absent");
  }
  Require(active_bundle->value().cell_graph.cell_id.value() ==
                  config_.cell_id &&
              active_bundle->value().cell_graph.graph_revision.value() ==
                  graph_revision &&
              active_bundle->retained_bundle_id().value() == retained_bundle_id,
          "node_environment_projection_invalid",
          "the independently verified bundle does not match the active cell "
          "and graph selection");

  CanonicalValue projection;
  try {
    projection =
        node_environment_projection_.Project(*active_bundle, manager_converged);
  } catch (const NodeEnvironmentProjectionError &error) {
    CoreFail("node_environment_projection_invalid", error.what());
  }
  const auto semantic_digest = content_identity::framed_sha256(
      kNodeEnvironmentProjectionIdentityDomain,
      kNodeEnvironmentProjectionApiVersion, projection);
  if (node_environment_generation_ == 0U) {
    node_environment_generation_ = 1U;
  } else if (semantic_digest != previous_node_environment_digest_) {
    Require(node_environment_generation_ < kMaximumSafeInteger,
            "node_environment_projection_invalid",
            "node environment projection generation is exhausted");
    ++node_environment_generation_;
  }
  previous_node_environment_digest_ = semantic_digest;

  CanonicalValue::Object result = projection.as_object();
  result.emplace_back(
      "source",
      CanonicalValue(CanonicalValue::Object{
          {"available", RequiredMember(manager_section, "available")},
          {"freshness", RequiredMember(manager_section, "freshness")},
          {"known_gaps", RequiredMember(manager_section, "known_gaps")},
          {"projection_generation",
           Number(node_environment_generation_, "projection_generation")},
          {"projection_owner_epoch", CanonicalValue(config_.owner_epoch)},
          {"receiver_age_ns",
           RequiredMember(manager_section, "receiver_age_ns")},
          {"selection_projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"selection_source_content_digest",
           RequiredMember(manager_section, "source_content_digest")},
          {"selection_source_generation",
           RequiredMember(manager_section, "source_generation")},
          {"selection_source_owner_epoch",
           RequiredMember(manager_section, "source_owner_epoch")},
          {"selection_source_owner_id",
           RequiredMember(manager_section, "source_owner_id")},
          {"source_content_digest", CanonicalValue(semantic_digest)},
          {"source_generation",
           Number(node_environment_generation_, "source_generation")},
          {"source_owner_epoch", CanonicalValue(config_.owner_epoch)},
          {"source_owner_id",
           CanonicalValue(config_.owner_id + ".node-environment")},
          {"source_owner_kind", CanonicalValue("node_environment_projection")},
      }));
  return CanonicalValue(std::move(result));
}

void HeadlessApiCore::RefreshRuntimeHealthAdapterLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const bool active = !RequiredBool(runtime.root, "complete");
  const bool complete_source_set_was_observed =
      HasLastValidPayload(runtime.root, "manager") &&
      HasLastValidPayload(runtime.root, "logical_scene") &&
      HasLastValidPayload(runtime.root, "scene_runtime");
  if (active && !complete_source_set_was_observed) {
    // Expected startup waiting is unknown source quality, not an alarm. Once
    // every owner has published at least one valid snapshot, later freshness
    // loss is an authoritative provider-unavailable condition and cannot be
    // cleared by silence.
    alarm_owner_.MarkSourceQualityUnknown(runtime_health_adapter_.source_id,
                                          runtime_health_adapter_.source_epoch);
    return;
  }
  const auto reasons = StringArray(runtime.root, "reasons");
  const auto unavailable = StringArray(runtime.root, "unavailable_sections");
  const CanonicalValue details(CanonicalValue::Object{
      {"projection_content_digest",
       CanonicalValue(runtime.semantic_content_digest)},
      {"projection_generation",
       Number(runtime.projection_generation, "projection_generation")},
      {"reasons", StringArrayValue(reasons)},
      {"unavailable_sections", StringArrayValue(unavailable)},
  });
  const CanonicalValue correlations(CanonicalValue::Object{
      {"graph_revision", RequiredMember(runtime.root, "active_graph_revision")},
      {"retained_bundle_id",
       RequiredMember(runtime.root, "active_retained_bundle_id")},
  });
  const auto mutation = SubmitAdapterUpdateLocked(
      runtime_health_adapter_, "manyforge.runtime.provider_unavailable",
      AlarmSubject{"cell", config_.cell_id, {}}, active, details, correlations);
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  alarm_owner_.CompleteSourceSnapshot(runtime_health_adapter_.source_id,
                                      runtime_health_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
}

void HeadlessApiCore::RefreshSceneAdapterLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const auto &section = RequiredSection(runtime.root, "scene_runtime");
  if (!SectionFresh(section)) {
    if (!scene_adapter_.source_epoch.empty()) {
      alarm_owner_.MarkSourceQualityUnknown(scene_adapter_.source_id,
                                            scene_adapter_.source_epoch);
    }
    return;
  }
  const auto source_epoch = OptionalStringMember(section, "source_owner_epoch");
  Require(source_epoch.has_value(), "headless_source_invalid",
          "fresh scene source lacks an owner epoch");
  RegisterAdapterEpochLocked(scene_adapter_, *source_epoch);
  const auto &payload = RequiredMember(section, "payload");
  const auto &state = RequiredMember(payload, "state");
  const auto state_name = RequiredString(state, "state");
  const auto active = state_name == "failed";
  const CanonicalValue details(CanonicalValue::Object{
      {"error", RequiredMember(state, "error")},
      {"initialization_generation",
       RequiredMember(state, "initialization_generation")},
      {"initialization_token", RequiredMember(state, "initialization_token")},
      {"reason", RequiredMember(state, "reason")},
      {"scene_runtime_state", CanonicalValue(state_name)},
  });
  const CanonicalValue correlations(CanonicalValue::Object{
      {"graph_revision", RequiredMember(runtime.root, "active_graph_revision")},
      {"retained_bundle_id",
       RequiredMember(runtime.root, "active_retained_bundle_id")},
  });
  const auto mutation = SubmitAdapterUpdateLocked(
      scene_adapter_, "manyforge.runtime.scene_initialization_fault",
      AlarmSubject{"cell", config_.cell_id, {}}, active, details, correlations);
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  alarm_owner_.CompleteSourceSnapshot(scene_adapter_.source_id,
                                      scene_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
}

void HeadlessApiCore::RegisterOperationEpochLocked(
    std::string source_owner_epoch) {
  if (operation_adapter_.source_epoch == source_owner_epoch) {
    return;
  }
  RegisterAdapterEpochLocked(operation_adapter_, std::move(source_owner_epoch));
  operation_native_sequence_ = 0U;
  operation_native_record_digest_.clear();
  observed_operation_faults_.clear();
}

void HeadlessApiCore::ReconcileOperationFaultLocked(
    const CanonicalValue &journal_payload, const std::string_view record_kind,
    const std::string_view request_id, const std::uint64_t native_sequence,
    const std::string_view payload_digest, const std::int64_t occurred_at_ns,
    const std::string_view occurred_at_wall,
    const std::string_view raw_event_id) {
  const auto &state = RequiredMember(journal_payload, "operation_state");
  const auto operation_id = RequiredString(state, "operation_id");
  const auto state_name = RequiredString(state, "state");
  bool reset_accepted = false;
  if (record_kind == "control_accepted") {
    reset_accepted = RequiredString(RequiredMember(journal_payload, "result"),
                                    "control") == "reset";
  }
  const bool active = IsOperationFaultState(state_name) && !reset_accepted;
  const bool clear = reset_accepted || IsNormalOperationTerminal(state_name);
  if (!active && !clear) {
    return;
  }

  AlarmSubject subject;
  if (active) {
    std::vector<std::string> resources;
    for (const auto &participant : RequiredArray(state, "participants")) {
      const auto participant_resources = StringArray(participant, "resources");
      resources.insert(resources.end(), participant_resources.begin(),
                       participant_resources.end());
    }
    std::sort(resources.begin(), resources.end());
    resources.erase(std::unique(resources.begin(), resources.end()),
                    resources.end());
    Require(!resources.empty() &&
                resources.size() <= kMaximumAlarmSubjectResources,
            "headless_operation_event_invalid",
            "I6 operation fault scope must contain at most 16 exact resources");
    subject = AlarmSubject{"operation", operation_id, std::move(resources)};
    active_operation_faults_.insert_or_assign(operation_id, subject);
    observed_operation_faults_.insert(operation_id);
  } else {
    const auto found = active_operation_faults_.find(operation_id);
    if (found == active_operation_faults_.end()) {
      return;
    }
    subject = found->second;
    active_operation_faults_.erase(found);
    observed_operation_faults_.erase(operation_id);
  }

  Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "operation alarm adapter sequence is exhausted");
  ++operation_adapter_.source_sequence;
  const auto &participants = RequiredArray(state, "participants");
  const CanonicalValue terminal_error =
      participants.empty()
          ? CanonicalValue(nullptr)
          : RequiredMember(participants.front(), "terminal_error");
  std::vector<std::string> evidence_refs;
  if (!raw_event_id.empty()) {
    evidence_refs.emplace_back(raw_event_id);
  }
  AlarmConditionUpdate update{
      "manyforge.motion.execution_fault",
      std::move(subject),
      active,
      operation_adapter_.source_id,
      operation_adapter_.source_epoch,
      operation_adapter_.source_sequence,
      occurred_at_ns,
      std::string(occurred_at_wall),
      AlarmConditionQuality::kGood,
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Object{
          {"native_sequence", Number(native_sequence, "native_sequence")},
          {"payload_digest", CanonicalValue(std::string(payload_digest))},
          {"record_kind", CanonicalValue(std::string(record_kind))},
          {"state", CanonicalValue(state_name)},
          {"terminal_error", terminal_error},
      }),
      std::move(evidence_refs),
      CanonicalValue(CanonicalValue::Object{
          {"operation_id", CanonicalValue(operation_id)},
          {"program_binding_id", RequiredMember(state, "program_binding_id")},
          {"program_run_id", RequiredMember(state, "program_run_id")},
          {"request_id", request_id.empty()
                             ? CanonicalValue(nullptr)
                             : CanonicalValue(std::string(request_id))},
      }),
  };
  const auto mutation = alarm_owner_.Submit(std::move(update));
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
}

void HeadlessApiCore::RegisterAdapterEpochLocked(AdapterCursor &cursor,
                                                 std::string source_epoch) {
  if (cursor.source_epoch == source_epoch) {
    return;
  }
  Require(cursor.source_epoch_order < kMaximumSafeInteger,
          "headless_adapter_epoch_exhausted",
          "headless alarm adapter epoch order is exhausted");
  ++cursor.source_epoch_order;
  cursor.source_sequence = 0U;
  cursor.last_condition_fingerprint.clear();
  cursor.last_update.reset();
  alarm_owner_.RegisterSource(cursor.domain, cursor.source_id,
                              cursor.adapter_id, source_epoch,
                              cursor.source_epoch_order);
  cursor.source_epoch = std::move(source_epoch);
}

AlarmMutation HeadlessApiCore::SubmitAdapterUpdateLocked(
    AdapterCursor &cursor, std::string definition_id, AlarmSubject subject,
    const bool active, CanonicalValue details, CanonicalValue correlations) {
  const CanonicalValue condition(CanonicalValue::Object{
      {"active", CanonicalValue(active)},
      {"correlations", correlations},
      {"definition_id", CanonicalValue(definition_id)},
      {"details", details},
      {"subject", CanonicalValue(CanonicalValue::Object{
                      {"resource_ids", StringArrayValue(subject.resource_ids)},
                      {"subject_id", CanonicalValue(subject.subject_id)},
                      {"subject_kind", CanonicalValue(subject.subject_kind)},
                  })},
  });
  const auto fingerprint = content_identity::framed_sha256(
      "manyforge-headless-alarm-adapter-condition", "v1", condition);
  if (fingerprint == cursor.last_condition_fingerprint &&
      cursor.last_update.has_value()) {
    const auto snapshot = alarm_owner_.Snapshot();
    return {"unchanged", false, snapshot.generation, snapshot.state_revision,
            {},          {}};
  }
  Require(cursor.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "headless alarm adapter sequence is exhausted");
  ++cursor.source_sequence;
  AlarmConditionUpdate update{
      std::move(definition_id),
      std::move(subject),
      active,
      cursor.source_id,
      cursor.source_epoch,
      cursor.source_sequence,
      clocks_.monotonic_time_ns(),
      clocks_.wall_time_text(),
      AlarmConditionQuality::kGood,
      CanonicalValue(CanonicalValue::Object{}),
      std::move(details),
      {},
      std::move(correlations),
  };
  auto mutation = alarm_owner_.Submit(update);
  cursor.last_condition_fingerprint = fingerprint;
  cursor.last_update = std::move(update);
  return mutation;
}

std::vector<std::string> HeadlessApiCore::DeliverFactsLocked(
    const std::vector<AlarmLifecycleFact> &facts) {
  std::vector<std::string> identities;
  identities.reserve(facts.size());
  for (const auto &fact : facts) {
    try {
      auto event = MakeAlarmLifecycleHistorianEvent(fact);
      identities.push_back(event.event_id);
      if (!historian_.TryEnqueue(std::move(event))) {
        alarm_owner_.MarkHealthUnavailable(
            std::string(kHistorianOwnerHealthCode),
            "historian delivery queue rejected a committed lifecycle fact");
      }
    } catch (const std::exception &error) {
      alarm_owner_.MarkHealthUnavailable(std::string(kHistorianOwnerHealthCode),
                                         error.what());
    } catch (...) {
      alarm_owner_.MarkHealthUnavailable(
          std::string(kHistorianOwnerHealthCode),
          "historian delivery raised an unknown failure");
    }
  }
  return identities;
}

void HeadlessApiCore::SynchronizeHistorianHealthLocked() {
  const auto health = historian_.Health();
  if (!health.available) {
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        health.error_message.value_or("historian delivery is unavailable"));
  }
}

CanonicalValue HeadlessApiCore::MutationResponse(
    const AlarmMutation &mutation,
    const std::vector<std::string> &audit_event_ids) {
  return CanonicalValue(CanonicalValue::Object{
      {"audit_event_ids", StringArrayValue(audit_event_ids)},
      {"changed", CanonicalValue(mutation.changed)},
      {"schema_version",
       CanonicalValue(std::string(kHeadlessApiMutationResponseSchemaVersion))},
      {"snapshot", OperatorAlarmSnapshotValue(alarm_owner_.Snapshot())},
      {"status", CanonicalValue(mutation.status)},
  });
}

std::optional<CanonicalValue> HeadlessApiCore::LookupReplayLocked(
    const std::string_view request_key,
    const std::string_view request_digest) const {
  const auto found = replay_.find(std::string(request_key));
  if (found == replay_.end()) {
    return std::nullopt;
  }
  Require(found->second.request_digest == request_digest,
          "headless_request_identity_conflict",
          "one request identity names conflicting mutation content");
  return found->second.response;
}

void HeadlessApiCore::RequireReplayCapacityLocked() const {
  Require(replay_.size() < config_.mutation_replay_capacity,
          "headless_request_ledger_full",
          "bounded headless mutation replay ledger is full");
}

void HeadlessApiCore::StoreReplayLocked(std::string request_key,
                                        std::string request_digest,
                                        CanonicalValue response) {
  if (replay_.find(request_key) != replay_.end()) {
    return;
  }
  RequireReplayCapacityLocked();
  replay_.insert_or_assign(
      std::move(request_key),
      ReplayEntry{std::move(request_digest), std::move(response)});
}

} // namespace manyforge::headless

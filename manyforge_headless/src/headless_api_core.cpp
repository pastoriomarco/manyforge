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
#include <iterator>
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
constexpr std::string_view kSourceModelProjectionIdentityDomain =
    "manyforge-source-model-projection-semantic";
constexpr std::string_view kAssistantModeProjectionIdentityDomain =
    "manyforge-assistant-mode-projection-semantic";
constexpr std::string_view kAssistantModeIndexIdentityDomain =
    "manyforge-assistant-mode-index-semantic";
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
  Require(!config.projection_root.empty() && !config.install_root.empty() &&
              !config.artifact_root.empty() &&
              !config.program_selection_state_root.empty() &&
              !config.node_catalog_path.empty() &&
              !config.alarm_catalog_path.empty() &&
              !config.historian_database_path.empty(),
          "headless_api_configuration_invalid",
          "projection, install, artifact, Program-selection state, node-"
          "catalog, alarm-catalog, and historian paths are required");
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
  if (record_kind == "stop_acceptance_projected") {
    return "stop_acceptance_projected";
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

class HeadlessApiCore::PackageRootDescriptor final {
public:
  explicit PackageRootDescriptor(const std::filesystem::path &package_root) {
    descriptor_ = ::open(package_root.c_str(),
                         O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (descriptor_ < 0) {
      throw std::system_error(errno, std::generic_category(),
                              "open Headless package root");
    }
  }

  ~PackageRootDescriptor() {
    if (descriptor_ >= 0) {
      static_cast<void>(::close(descriptor_));
    }
  }

  PackageRootDescriptor(const PackageRootDescriptor &) = delete;
  PackageRootDescriptor &operator=(const PackageRootDescriptor &) = delete;

  [[nodiscard]] std::filesystem::path root() const {
    if (descriptor_ < 0) {
      throw std::logic_error("Headless package root descriptor is closed");
    }
    return std::filesystem::path("/proc/self/fd") /
           std::to_string(descriptor_);
  }

private:
  int descriptor_{-1};
};

HeadlessApiCore::HeadlessApiCore(HeadlessApiCoreConfig config,
                                 HeadlessApiCoreClocks clocks)
    : config_(ValidateConfig(std::move(config))),
      clocks_(ResolveClocks(std::move(clocks))),
      owner_lock_(std::make_unique<HeadlessApiOwnerLock>(
          config_.historian_database_path)),
      package_root_descriptor_(
          std::make_unique<PackageRootDescriptor>(config_.install_root)),
      retained_bundle_source_(package_root_descriptor_->root()),
      scenario_catalog_source_(package_root_descriptor_->root(), "maintained"),
      program_selection_owner_(
          package_root_descriptor_->root(), config_.artifact_root,
          config_.program_selection_state_root,
          config_.owner_id + ".program-selection", config_.owner_epoch),
      // This reader validates the final path component with symlink_status;
      // "." keeps that component directory-valued while traversal remains
      // anchored through the retained procfd root.
      node_environment_projection_(package_root_descriptor_->root() / ".",
                                   config_.node_catalog_path),
      source_model_projection_(package_root_descriptor_->root()),
      viewer_model_source_(package_root_descriptor_->root()),
      history_store_(std::make_shared<SqliteEventHistoryStore>(
          config_.historian_database_path, config_.historian_record_retention)),
      historian_(history_store_, config_.historian_queue_capacity),
      runtime_aggregator_(config_.projection_root, config_.cell_id,
                          config_.owner_id + ".projection", config_.owner_epoch,
                          config_.freshness_policy, clocks_.wall_time_ns,
                          clocks_.monotonic_time_ns),
      alarm_owner_(OperatorAlarmCatalog::Load(config_.alarm_catalog_path),
                   config_.cell_id, config_.resource_ids,
                   config_.owner_id + ".alarm", config_.owner_epoch,
                   clocks_.monotonic_time_ns, clocks_.wall_time_text,
                   // Headless currently owns operation and runtime-health
                   // alarm adapters.  Scene-initialization faults use the
                   // runtime-health adapter; no independent scene-domain
                   // producer exists on this boundary.
                   {"motion", "runtime_health"}),
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

CanonicalValue HeadlessApiCore::SourceModelProjection() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return SourceModelProjectionLocked(RefreshLocked());
}

CanonicalValue HeadlessApiCore::SourceModelContext() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"node_environment", NodeEnvironmentProjectionLocked(runtime)},
      {"schema_version",
       CanonicalValue(std::string(kSourceModelContextSchemaVersion))},
      {"source_model", SourceModelProjectionLocked(runtime)},
  });
}

CanonicalValue HeadlessApiCore::ScenarioCatalogProjection() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  try {
    return scenario_catalog_source_.LoadVerifiedCatalog();
  } catch (const deployment::ScenarioApplyCandidateError &error) {
    CoreFail("scenario_catalog_unavailable", error.what());
  } catch (const deployment::ScenarioCatalogError &error) {
    CoreFail("scenario_catalog_unavailable", error.what());
  }
}

CanonicalValue
HeadlessApiCore::ScenarioProgramSelection(const std::string_view scenario_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  Require(RequiredBool(runtime.root, "complete"),
          "scenario_program_selection_unavailable",
          "scenario Program selection requires fresh Manager and scene owners");
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &logical_scene_section =
      RequiredSection(runtime.root, "logical_scene");
  const auto &scene_runtime_section =
      RequiredSection(runtime.root, "scene_runtime");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  Require(manager_payload.is_object(), "scenario_program_selection_invalid",
          "Manager has no active selection");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto active_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto active_graph_revision = RequiredString(active, "graph_revision");
  Require(
      RequiredBool(manager_payload, "manager_ready") &&
          RequiredBool(RequiredMember(manager_payload, "runtime"), "converged"),
      "scenario_program_selection_unavailable",
      "Manager has not converged the active retained bundle");

  const auto selection = [&]() -> deployment::ScenarioProgramSelection {
    try {
      return scenario_catalog_source_.LoadExactProgramSelection(scenario_id);
    } catch (const deployment::ScenarioApplyCandidateError &error) {
      CoreFail("scenario_program_selection_unavailable", error.what());
    } catch (const deployment::ScenarioCatalogError &error) {
      CoreFail("scenario_program_selection_unavailable", error.what());
    } catch (const std::exception &error) {
      CoreFail("scenario_program_selection_invalid", error.what());
    }
  }();
  Require(selection.retained_bundle_id.value() == active_bundle_id &&
              selection.graph_revision.value() == active_graph_revision,
          "scenario_program_selection_not_applied",
          "scenario Program selection names another active deployment");

  const auto environment = NodeEnvironmentProjectionLocked(runtime);
  Require(RequiredString(environment, "environment_digest") ==
                  selection.node_environment_digest.value() &&
              RequiredString(environment, "retained_bundle_id") ==
                  selection.retained_bundle_id.value() &&
              RequiredString(environment, "graph_revision") ==
                  selection.graph_revision.value(),
          "scenario_program_selection_invalid",
          "scenario Program selection and retained node environment disagree");

  const auto owner_identity = [](const std::string_view role,
                                 const CanonicalValue &section) {
    return CanonicalValue(CanonicalValue::Object{
        {"owner_epoch", RequiredMember(section, "source_owner_epoch")},
        {"owner_id", RequiredMember(section, "source_owner_id")},
        {"owner_role", CanonicalValue(std::string(role))},
    });
  };
  return CanonicalValue(CanonicalValue::Object{
      {"assistant_projection", selection.normalized_assistant_projection},
      {"assistant_mode",
       CanonicalValue(selection.assistant_mode ==
                              deployment::ScenarioAssistantMode::kRequired
                          ? "required"
                      : selection.assistant_mode ==
                              deployment::ScenarioAssistantMode::kOptional
                          ? "optional"
                          : "forbidden")},
      {"deployment_artifact_id",
       CanonicalValue(selection.deployment_artifact_id.value())},
      {"deployment_revision",
       CanonicalValue(selection.deployment_revision.value())},
      {"graph_revision", CanonicalValue(selection.graph_revision.value())},
      {"node_environment_digest",
       CanonicalValue(selection.node_environment_digest.value())},
      {"owner_identities",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"owner_epoch", CanonicalValue(config_.owner_epoch)},
               {"owner_id", CanonicalValue(config_.owner_id)},
               {"owner_role", CanonicalValue("headless_projection")},
           }),
           owner_identity("manager", manager_section),
           owner_identity("scene_runtime", scene_runtime_section),
           owner_identity("scene_transactions", logical_scene_section),
       })},
      {"program", selection.normalized_program_root},
      {"program_binding", selection.normalized_program_binding_root},
      {"program_binding_id",
       CanonicalValue(selection.program_binding_id.value())},
      {"program_id", CanonicalValue(selection.program_id.value())},
      {"program_revision", CanonicalValue(selection.program_revision.value())},
      {"retained_bundle_id",
       CanonicalValue(selection.retained_bundle_id.value())},
      {"scenario_catalog_revision",
       CanonicalValue(selection.scenario_catalog_revision.value())},
      {"scenario_id", CanonicalValue(selection.scenario_id)},
      {"schema_version",
       CanonicalValue(std::string(kScenarioProgramSelectionSchemaVersion))},
      {"typed_parameter_overrides", selection.typed_parameter_overrides},
  });
}

CanonicalValue HeadlessApiCore::PrepareProgramApply(
    const std::string_view scenario_id,
    const std::string_view expected_scenario_catalog_revision,
    const std::string_view canonical_program_bytes) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  Require(SectionFresh(manager_section), "program_apply_deployment_not_active",
          "Program Apply preparation requires a fresh Manager owner");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  Require(manager_payload.is_object(), "program_apply_deployment_not_active",
          "Program Apply preparation requires an active Manager selection");
  const auto &active = RequiredMember(manager_payload, "active");
  Require(
      RequiredBool(manager_payload, "manager_ready") &&
          RequiredBool(RequiredMember(manager_payload, "runtime"), "converged"),
      "program_apply_deployment_not_active",
      "Program Apply preparation requires one converged Manager selection");
  return program_selection_owner_.Prepare(
      scenario_id, expected_scenario_catalog_revision, canonical_program_bytes,
      RequiredString(active, "retained_bundle_id"),
      RequiredString(active, "graph_revision"));
}

CanonicalValue HeadlessApiCore::CommitProgramApply(
    const std::string_view canonical_preparation_bytes,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  if (auto replay = program_selection_owner_.Replay(
          canonical_preparation_bytes, expected_applied_record_digest,
          request_owner_epoch, request_id);
      replay.has_value()) {
    return *replay;
  }
  const auto runtime = RefreshLocked();
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  Require(SectionFresh(manager_section), "program_apply_deployment_not_active",
          "Program Apply commit requires a fresh Manager owner");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  Require(manager_payload.is_object(), "program_apply_deployment_not_active",
          "Program Apply commit requires an active Manager selection");
  const auto &active = RequiredMember(manager_payload, "active");
  Require(
      RequiredBool(manager_payload, "manager_ready") &&
          RequiredBool(RequiredMember(manager_payload, "runtime"), "converged"),
      "program_apply_deployment_not_active",
      "Program Apply commit requires one converged Manager selection");
  return program_selection_owner_.Commit(
      canonical_preparation_bytes, expected_applied_record_digest,
      request_owner_epoch, request_id,
      RequiredString(active, "retained_bundle_id"),
      RequiredString(active, "graph_revision"), clocks_.monotonic_time_ns());
}

CanonicalValue HeadlessApiCore::ReplayProgramApply(
    const std::string_view scenario_id,
    const std::string_view scenario_catalog_revision,
    const std::string_view program_id, const std::string_view program_revision,
    const std::optional<std::string> &expected_applied_record_digest,
    const std::string_view request_owner_epoch,
    const std::string_view request_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  auto replay = program_selection_owner_.ReplayRequest(
      scenario_id, scenario_catalog_revision, program_id, program_revision,
      expected_applied_record_digest, request_owner_epoch, request_id);
  return replay.has_value() ? std::move(*replay) : CanonicalValue(nullptr);
}

CanonicalValue HeadlessApiCore::AppliedProgramSelection() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto selection = program_selection_owner_.ReadApplied();
  const auto runtime = RefreshLocked();
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  Require(SectionFresh(manager_section),
          "program_applied_selection_unavailable",
          "Applied Program requires a fresh Manager owner");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  Require(manager_payload.is_object(), "program_applied_selection_unavailable",
          "Applied Program requires an active Manager selection");
  const auto &active = RequiredMember(manager_payload, "active");
  Require(RequiredBool(manager_payload, "manager_ready") &&
              RequiredBool(RequiredMember(manager_payload, "runtime"),
                           "converged") &&
              RequiredString(active, "retained_bundle_id") ==
                  RequiredString(selection, "retained_bundle_id") &&
              RequiredString(active, "graph_revision") ==
                  RequiredString(selection, "graph_revision"),
          "program_applied_selection_not_active",
          "the last Applied Program does not belong to the fresh active "
          "Manager selection");
  const auto environment = NodeEnvironmentProjectionLocked(runtime);
  Require(RequiredString(environment, "environment_digest") ==
              RequiredString(selection, "node_environment_digest"),
          "program_applied_selection_invalid",
          "Applied Program and active node environment disagree");
  return selection;
}

ViewerModelAsset HeadlessApiCore::ViewerModelUrdf() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return ViewerModelAssetLocked(RefreshLocked(), std::nullopt, std::nullopt);
}

ViewerModelAsset HeadlessApiCore::ViewerModelResource(
    const std::string_view expected_retained_bundle_id,
    const std::string_view logical_uri) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return ViewerModelAssetLocked(RefreshLocked(), expected_retained_bundle_id,
                                logical_uri);
}

CanonicalValue HeadlessApiCore::AssistantModeIndex() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return AssistantModeIndexLocked(RefreshLocked());
}

CanonicalValue
HeadlessApiCore::AssistantModeProjection(const std::string_view mode_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return AssistantModeProjectionLocked(RefreshLocked(), mode_id);
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

CanonicalValue
HeadlessApiCore::CommitProgramModelAttempt(CanonicalValue record) {
  constexpr std::size_t kProgramModelAttemptFieldCount = 30U;
  const std::string error_code = "headless_program_model_attempt_invalid";
  Require(record.is_object() &&
              record.as_object().size() == kProgramModelAttemptFieldCount,
          error_code, "Program model-attempt record has wrong fields");
  const auto text = [&](const std::string_view name,
                        const std::size_t maximum_bytes = 512U) {
    const auto value = RequiredString(record, name);
    Require(!value.empty() && value.size() <= maximum_bytes &&
                content_identity::is_utf8_nfc(value),
            error_code,
            "Program model-attempt text field is invalid: " +
                std::string(name));
    return value;
  };
  const auto optional_digest = [&](const std::string_view name) {
    const auto value = OptionalStringMember(record, name);
    Require(!value.has_value() || content_identity::is_sha256_digest(*value),
            error_code,
            "Program model-attempt digest field is invalid: " +
                std::string(name));
    return value;
  };
  const auto counter = [&](const std::string_view name,
                           const std::uint64_t minimum = 0U) {
    const auto *value =
        std::get_if<double>(&RequiredMember(record, name).storage());
    Require(value != nullptr && std::isfinite(*value) &&
                *value >= static_cast<double>(minimum) &&
                std::floor(*value) == *value &&
                *value <= static_cast<double>(kMaximumSafeInteger),
            error_code,
            "Program model-attempt counter field is invalid: " +
                std::string(name));
    return static_cast<std::uint64_t>(*value);
  };
  Require(text("schema_version") == "manyforge.program_model_attempt.v1",
          error_code, "Program model-attempt version is unsupported");
  const auto model_attempt_id = text("model_attempt_id");
  text("program_run_id");
  text("node_instance_id");
  text("node_attempt_id");
  const auto attempt_index = counter("external_attempt_index", 1U);
  Require(attempt_index <= 2U, error_code,
          "Program model-attempt index exceeds the bounded retry contract");
  Require(content_identity::is_sha256_digest(
              text("assistant_projection_digest", 71U)) &&
              content_identity::is_sha256_digest(
                  text("prompt_template_digest", 71U)) &&
              content_identity::is_sha256_digest(text("prompt_digest", 71U)),
          error_code, "Program model-attempt retained identity is invalid");
  text("model_endpoint_alias");
  text("external_resource_alias");
  text("model_id");
  text("prompt_template_id");
  text("prompt_template_version");
  const auto &arguments = RequiredMember(record, "argument_digests");
  Require(arguments.is_object() && arguments.as_object().size() <= 256U,
          error_code, "Program model-attempt argument digests are invalid");
  for (const auto &[name, digest_value] : arguments.as_object()) {
    const auto *digest = std::get_if<std::string>(&digest_value.storage());
    Require(!name.empty() && name.size() <= 512U &&
                content_identity::is_utf8_nfc(name) && digest != nullptr &&
                content_identity::is_sha256_digest(*digest),
            error_code, "Program model-attempt argument digest is invalid");
  }
  optional_digest("attachment_digest");
  const auto &audit_policy = RequiredMember(record, "audit_policy");
  Require(audit_policy.is_object() && audit_policy.as_object().size() == 3U,
          error_code, "Program model-attempt audit policy is invalid");
  const bool redact_arguments =
      RequiredBool(audit_policy, "redact_argument_values");
  const bool store_full_prompts =
      RequiredBool(audit_policy, "store_full_prompts");
  const bool store_full_responses =
      RequiredBool(audit_policy, "store_full_responses");
  const auto &argument_values = RequiredMember(record, "argument_values");
  Require((redact_arguments &&
           std::holds_alternative<std::nullptr_t>(argument_values.storage())) ||
              (!redact_arguments && argument_values.is_object()),
          error_code,
          "Program model-attempt argument retention disagrees with policy");
  const auto &prompt_text = RequiredMember(record, "prompt_text");
  const auto *retained_prompt =
      std::get_if<std::string>(&prompt_text.storage());
  Require((store_full_prompts && retained_prompt != nullptr &&
           !retained_prompt->empty()) ||
              (!store_full_prompts &&
               std::holds_alternative<std::nullptr_t>(prompt_text.storage())),
          error_code,
          "Program model-attempt prompt retention disagrees with policy");
  const auto outcome = text("outcome", 64U);
  const std::set<std::string> outcomes{"success",
                                       "timeout",
                                       "parse_error",
                                       "schema_error",
                                       "confidence_below_threshold",
                                       "cancelled",
                                       "transport_error",
                                       "invalid_response"};
  Require(outcomes.contains(outcome), error_code,
          "Program model-attempt outcome is unsupported");
  const auto attempt_error = OptionalStringMember(record, "error_code");
  Require((outcome == "success") == !attempt_error.has_value() &&
              (!attempt_error.has_value() ||
               (!attempt_error->empty() && attempt_error->size() <= 256U &&
                content_identity::is_utf8_nfc(*attempt_error))),
          error_code,
          "Program model-attempt error identity disagrees with outcome");
  const auto *error_message = std::get_if<std::string>(
      &RequiredMember(record, "error_message").storage());
  const auto *model_version = std::get_if<std::string>(
      &RequiredMember(record, "model_version").storage());
  const auto occurred_at_ns = counter("occurred_at_ns");
  const auto occurred_at_wall = text("occurred_at_wall", 128U);
  Require(error_message != nullptr && error_message->size() <= 4096U &&
              content_identity::is_utf8_nfc(*error_message) &&
              model_version != nullptr && model_version->size() <= 4096U &&
              content_identity::is_utf8_nfc(*model_version),
          error_code, "Program model-attempt result text is invalid");
  const auto response_digest = optional_digest("response_digest");
  const auto output_digest = optional_digest("validated_output_digest");
  Require((outcome != "success" ||
           (response_digest.has_value() && output_digest.has_value())) &&
              (outcome == "success" || !output_digest.has_value()),
          error_code,
          "Program model-attempt output evidence disagrees with outcome");
  const auto &response_value = RequiredMember(record, "response_value");
  const bool response_is_present =
      std::holds_alternative<std::string>(response_value.storage()) ||
      response_value.is_object();
  Require((!store_full_responses &&
           std::holds_alternative<std::nullptr_t>(response_value.storage())) ||
              (store_full_responses &&
               ((!response_digest.has_value() && !response_is_present) ||
                (response_digest.has_value() && response_is_present))),
          error_code,
          "Program model-attempt response retention disagrees with policy");
  const auto *latency =
      std::get_if<double>(&RequiredMember(record, "latency_ms").storage());
  Require(latency != nullptr && std::isfinite(*latency) && *latency >= 0.0 &&
              *latency <= 3600000.0,
          error_code, "Program model-attempt latency is invalid");
  const auto &usage = RequiredMember(record, "usage");
  Require(std::holds_alternative<std::nullptr_t>(usage.storage()) ||
              usage.is_object(),
          error_code, "Program model-attempt usage is invalid");
  Require(content_identity::canonical_json(record).size() <=
              kMaximumHistorianPayloadBytes,
          error_code, "Program model-attempt record exceeds its byte bound");

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  auto event = MakeHistorianEvent("manyforge.program", "program_model_attempt",
                                  outcome == "success" ? "INFO" : "WARN",
                                  static_cast<std::int64_t>(occurred_at_ns),
                                  occurred_at_wall, std::move(record));
  const auto event_id = event.event_id;
  const auto status = history_store_->Append(event);
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", CanonicalValue(event_id)},
      {"model_attempt_id", CanonicalValue(model_attempt_id)},
      {"schema_version",
       CanonicalValue("manyforge.program_model_attempt_commit.v1")},
      {"status",
       CanonicalValue(status == IEventHistoryStore::AppendStatus::kStored
                          ? "stored"
                          : "duplicate")},
  });
}

CanonicalValue HeadlessApiCore::IngestOperationEvent(
    std::string source_owner_epoch, const std::uint64_t source_sequence,
    std::string record_kind, std::optional<std::string> request_id,
    std::optional<std::string> operation_id,
    const std::optional<std::uint64_t> operation_revision,
    const std::int64_t occurred_at_ns, std::string occurred_at_wall,
    const bool effect_pending, CanonicalValue journal_payload,
    std::string payload_digest) {
  const bool has_operation_id = operation_id.has_value();
  const bool has_operation_revision = operation_revision.has_value();
  const bool stop_acceptance_projection =
      record_kind == "stop_acceptance_projected";
  Require(!source_owner_epoch.empty() && source_sequence != 0U &&
              source_sequence <= kMaximumSafeInteger && !record_kind.empty() &&
              has_operation_id == has_operation_revision &&
              (stop_acceptance_projection != has_operation_id) &&
              (!has_operation_id ||
               (!operation_id->empty() && *operation_revision != 0U &&
                *operation_revision <= kMaximumSafeInteger)) &&
              occurred_at_ns >= 0 &&
              static_cast<std::uint64_t>(occurred_at_ns) <=
                  kMaximumSafeInteger &&
              !occurred_at_wall.empty() && occurred_at_wall.size() <= 128U &&
              content_identity::is_utf8_nfc(occurred_at_wall) &&
              (!request_id.has_value() || !request_id->empty()) &&
              content_identity::is_sha256_digest(payload_digest) &&
              journal_payload.is_object(),
          "headless_operation_event_invalid",
          "operation event envelope is malformed");
  const std::set<std::string_view> supported_kinds{
      "operation_accepted",     "control_accepted", "stop_acceptance_projected",
      "runtime_scale_accepted", "state_observed",   "host_replaced"};
  Require(supported_kinds.contains(record_kind),
          "headless_operation_event_invalid",
          "operation event record kind is unsupported");
  const auto payload_json = content_identity::canonical_json(journal_payload);
  Require(content_identity::sha256_digest(payload_json) == payload_digest &&
              RequiredString(journal_payload, "record_kind") == record_kind,
          "headless_operation_event_invalid",
          "operation event payload does not match its durable envelope");
  const CanonicalValue *operation_state = nullptr;
  if (stop_acceptance_projection) {
    const auto &acceptance = RequiredMember(journal_payload, "acceptance");
    const auto &accepted = RequiredMember(journal_payload, "accepted");
    const auto &affected_state =
        RequiredMember(journal_payload, "affected_state");
    const auto &intent = RequiredMember(journal_payload, "intent");
    const auto entry_state = RequiredString(acceptance, "entry_state");
    const auto stop_id = RequiredString(acceptance, "stop_id");
    const auto stop_generation =
        RequiredCounter(acceptance, "stop_generation", 1U);
    Require(RequiredString(journal_payload, "api_version") ==
                    "manyforge.stop_acceptance_projection.v2" &&
                accepted.is_object() && affected_state.is_object() &&
                intent.is_object() && request_id.has_value() &&
                RequiredString(acceptance, "owner_epoch") ==
                    source_owner_epoch &&
                RequiredString(acceptance, "request_id") == *request_id &&
                RequiredString(accepted, "request_id") == *request_id &&
                RequiredString(accepted, "stop_id") == stop_id &&
                RequiredCounter(accepted, "stop_generation", 1U) ==
                    stop_generation &&
                (entry_state == "active" || entry_state == "terminal") &&
                effect_pending == (entry_state == "active"),
            "headless_operation_event_invalid",
            "Stop acceptance projection does not match its durable envelope");
  } else {
    Require(RequiredString(journal_payload, "api_version") ==
                    "manyforge.operation_journal_record.v1" &&
                RequiredString(journal_payload, "owner_epoch") ==
                    source_owner_epoch &&
                RequiredBool(journal_payload, "effect_pending") ==
                    effect_pending,
            "headless_operation_event_invalid",
            "operation event payload does not match its durable envelope");
    operation_state = &RequiredMember(journal_payload, "operation_state");
    Require(operation_state->is_object() &&
                RequiredString(*operation_state, "operation_id") ==
                    *operation_id &&
                RequiredCounter(*operation_state, "operation_revision", 1U) ==
                    *operation_revision,
            "headless_operation_event_invalid",
            "operation event state does not match its durable envelope");
  }
  const auto event_type =
      OperationHistoryEventType(record_kind, journal_payload);
  const auto severity =
      operation_state != nullptr &&
              IsOperationFaultState(RequiredString(*operation_state, "state"))
          ? "ERROR"
          : "INFO";
  const auto native_record_digest = content_identity::framed_sha256(
      "manyforge-operation-journal-event", "v1",
      CanonicalValue(CanonicalValue::Object{
          {"effect_pending", CanonicalValue(effect_pending)},
          {"occurred_at_ns", Number(static_cast<std::uint64_t>(occurred_at_ns),
                                    "occurred_at_ns")},
          {"occurred_at_wall", CanonicalValue(occurred_at_wall)},
          {"operation_id", OptionalStringValue(operation_id)},
          {"operation_revision",
           operation_revision.has_value()
               ? Number(*operation_revision, "operation_revision")
               : CanonicalValue(nullptr)},
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
      {"operation_id", OptionalStringValue(operation_id)},
      {"operation_revision",
       operation_revision.has_value()
           ? Number(*operation_revision, "operation_revision")
           : CanonicalValue(nullptr)},
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

  if (stop_acceptance_projection) {
    ReconcileRuntimeCollisionStopLocked(
        journal_payload, source_sequence, payload_digest, occurred_at_ns,
        occurred_at_wall, event_id.value_or(""));
  } else if (operation_state != nullptr) {
    ReconcileOperationFaultLocked(journal_payload, record_kind,
                                  request_id.value_or(""), source_sequence,
                                  payload_digest, occurred_at_ns,
                                  occurred_at_wall, event_id.value_or(""));
  }
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
  retired.clear();
  for (const auto &[operation, subject] : active_runtime_collisions_) {
    if (observed_runtime_collisions_.contains(operation)) {
      continue;
    }
    Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
            "headless_adapter_sequence_exhausted",
            "operation alarm adapter sequence is exhausted");
    ++operation_adapter_.source_sequence;
    AlarmConditionUpdate update{
        "manyforge.motion.runtime_collision",
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
    active_runtime_collisions_.erase(operation);
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

std::shared_ptr<const manyforge::deployment::RetainedBundle>
HeadlessApiCore::VerifiedActiveBundleLocked(
    const std::string_view retained_bundle_id,
    const std::string_view graph_revision, const std::string_view invalid_code,
    const std::string_view unavailable_code) {
  if (verified_active_bundle_ == nullptr ||
      verified_active_bundle_->retained_bundle_id().value() !=
          retained_bundle_id) {
    std::shared_ptr<const manyforge::deployment::RetainedBundle> selected;
    try {
      selected = retained_bundle_source_.get_verified(
          manyforge::core::RetainedBundleId(std::string(retained_bundle_id)));
    } catch (const std::exception &error) {
      CoreFail(
          std::string(invalid_code),
          "the selected retained bundle failed independent verification: " +
              std::string(error.what()));
    }
    if (selected == nullptr) {
      CoreFail(std::string(unavailable_code),
               "the Manager-selected retained bundle is absent");
    }
    Require(selected->value().cell_graph.cell_id.value() == config_.cell_id &&
                selected->value().cell_graph.graph_revision.value() ==
                    graph_revision &&
                selected->retained_bundle_id().value() == retained_bundle_id,
            std::string(invalid_code),
            "the independently verified bundle does not match the active cell "
            "and graph selection");

    // Retained bundles are immutable content-addressed products. Verify once
    // for each Manager-selected identity in this process, then reuse the
    // closed object for read projections instead of reparsing the same files
    // on every authoring callback.
    verified_active_bundle_ = std::move(selected);
    source_model_projection_cache_.reset();
    verified_viewer_model_.reset();
    assistant_mode_index_cache_.reset();
    assistant_mode_projection_cache_.clear();
  }
  Require(
      verified_active_bundle_->value().cell_graph.cell_id.value() ==
              config_.cell_id &&
          verified_active_bundle_->value().cell_graph.graph_revision.value() ==
              graph_revision &&
          verified_active_bundle_->retained_bundle_id().value() ==
              retained_bundle_id,
      std::string(invalid_code),
      "the cached verified bundle does not match the active cell and "
      "graph selection");
  return verified_active_bundle_;
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

  const auto active_bundle = VerifiedActiveBundleLocked(
      retained_bundle_id, graph_revision, "node_environment_projection_invalid",
      "node_environment_projection_unavailable");

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

CanonicalValue HeadlessApiCore::SourceModelProjectionLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  if (std::holds_alternative<std::nullptr_t>(manager_payload.storage())) {
    CoreFail("source_model_projection_unavailable",
             "Manager has no current or last-valid active-bundle selection");
  }
  Require(manager_payload.is_object(), "source_model_projection_invalid",
          "Manager active-bundle selection is not an object");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto retained_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto graph_revision = RequiredString(active, "graph_revision");
  const auto manager_ready = RequiredBool(manager_payload, "manager_ready");
  const auto &manager_runtime = RequiredMember(manager_payload, "runtime");
  const auto manager_converged = RequiredBool(manager_runtime, "converged");
  Require(manager_ready == manager_converged, "source_model_projection_invalid",
          "Manager readiness and runtime convergence disagree");

  const auto active_bundle = VerifiedActiveBundleLocked(
      retained_bundle_id, graph_revision, "source_model_projection_invalid",
      "source_model_projection_unavailable");

  CanonicalValue projection;
  if (source_model_projection_cache_.has_value()) {
    projection = *source_model_projection_cache_;
  } else {
    try {
      projection = source_model_projection_.Project(*active_bundle);
    } catch (const SourceModelProjectionError &error) {
      CoreFail("source_model_projection_invalid", error.what());
    }
    source_model_projection_cache_ = projection;
  }
  const auto semantic_digest = content_identity::framed_sha256(
      kSourceModelProjectionIdentityDomain, kSourceModelProjectionApiVersion,
      projection);
  if (source_model_generation_ == 0U) {
    source_model_generation_ = 1U;
  } else if (semantic_digest != previous_source_model_digest_) {
    Require(source_model_generation_ < kMaximumSafeInteger,
            "source_model_projection_invalid",
            "source model projection generation is exhausted");
    ++source_model_generation_;
  }
  previous_source_model_digest_ = semantic_digest;

  CanonicalValue::Object result = projection.as_object();
  result.emplace_back(
      "source",
      CanonicalValue(CanonicalValue::Object{
          {"available", RequiredMember(manager_section, "available")},
          {"freshness", RequiredMember(manager_section, "freshness")},
          {"known_gaps", RequiredMember(manager_section, "known_gaps")},
          {"projection_generation",
           Number(source_model_generation_, "projection_generation")},
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
           Number(source_model_generation_, "source_generation")},
          {"source_owner_epoch", CanonicalValue(config_.owner_epoch)},
          {"source_owner_id",
           CanonicalValue(config_.owner_id + ".source-model")},
          {"source_owner_kind", CanonicalValue("source_model_projection")},
      }));
  return CanonicalValue(std::move(result));
}

ViewerModelAsset HeadlessApiCore::ViewerModelAssetLocked(
    const HeadlessRuntimeSnapshot &runtime,
    const std::optional<std::string_view> expected_retained_bundle_id,
    const std::optional<std::string_view> logical_uri) {
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  if (std::holds_alternative<std::nullptr_t>(manager_payload.storage())) {
    CoreFail("viewer_model_asset_unavailable",
             "Manager has no current or last-valid active-bundle selection");
  }
  Require(manager_payload.is_object(), "viewer_model_asset_invalid",
          "Manager active-bundle selection is not an object");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto retained_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto graph_revision = RequiredString(active, "graph_revision");
  const auto manager_ready = RequiredBool(manager_payload, "manager_ready");
  const auto &manager_runtime = RequiredMember(manager_payload, "runtime");
  const auto manager_converged = RequiredBool(manager_runtime, "converged");
  Require(manager_ready == manager_converged, "viewer_model_asset_invalid",
          "Manager readiness and runtime convergence disagree");

  if (expected_retained_bundle_id.has_value()) {
    Require(content_identity::is_sha256_digest(*expected_retained_bundle_id),
            "viewer_model_asset_invalid",
            "expected retained-bundle identity is not a SHA-256 digest");
    Require(*expected_retained_bundle_id == retained_bundle_id,
            "viewer_model_selection_stale",
            "requested viewer asset belongs to an inactive retained bundle");
  }
  if (logical_uri.has_value()) {
    Require(!logical_uri->empty() &&
                logical_uri->size() <= kMaximumViewerModelResourceUriBytes &&
                logical_uri->find('\0') == std::string_view::npos,
            "viewer_model_asset_invalid",
            "viewer resource URI is outside its closed byte bound");
  }

  const auto active_bundle = VerifiedActiveBundleLocked(
      retained_bundle_id, graph_revision, "viewer_model_asset_invalid",
      "viewer_model_asset_unavailable");
  const auto &viewer_product =
      active_bundle->value().compiled_products.viewer_model;
  Require(viewer_product.has_value(), "viewer_model_asset_unavailable",
          "the active retained bundle has no viewer-model product");

  if (verified_viewer_model_ == nullptr) {
    std::optional<deployment::VerifiedRetainedViewerModel> selected;
    try {
      selected = viewer_model_source_.get_verified(
          deployment::RetainedViewerModelQuery{
              active_bundle->retained_bundle_id(), viewer_product->product_id});
    } catch (const std::exception &error) {
      CoreFail("viewer_model_asset_invalid",
               "the active viewer product failed independent verification: " +
                   std::string(error.what()));
    }
    Require(selected.has_value(), "viewer_model_asset_unavailable",
            "the active viewer product is absent");
    verified_viewer_model_ =
        std::make_shared<deployment::VerifiedRetainedViewerModel>(
            std::move(*selected));
  }
  Require(verified_viewer_model_->query().retained_bundle_id ==
                  active_bundle->retained_bundle_id() &&
              verified_viewer_model_->query().product_id ==
                  viewer_product->product_id,
          "viewer_model_asset_invalid",
          "the cached viewer product does not match the active selection");

  const auto &output_records = active_bundle->value().provenance.output_records;
  const auto output = std::find_if(
      output_records.begin(), output_records.end(),
      [&viewer_product](const core::RetainedOutputRecordDTO &candidate) {
        return candidate.product_id == viewer_product->product_id &&
               candidate.product_kind ==
                   core::RetainedProductKind::kViewerModel;
      });
  Require(output != output_records.end(), "viewer_model_asset_invalid",
          "the active viewer product has no exact provenance output");

  std::optional<std::size_t> resource_index;
  if (logical_uri.has_value()) {
    const auto &resources = verified_viewer_model_->resources();
    const auto resource = std::lower_bound(
        resources.begin(), resources.end(), *logical_uri,
        [](const deployment::VerifiedRetainedPlanningResource &candidate,
           const std::string_view selected_uri) {
          return candidate.logical_uri() < selected_uri;
        });
    Require(resource != resources.end() &&
                resource->logical_uri() == *logical_uri,
            "viewer_model_resource_not_found",
            "the requested URI is not an active viewer-model resource");
    resource_index =
        static_cast<std::size_t>(std::distance(resources.begin(), resource));
  }

  return ViewerModelAsset(verified_viewer_model_, config_.cell_id,
                          graph_revision, output->content_digest,
                          resource_index);
}

CanonicalValue HeadlessApiCore::AssistantModeProjectionLocked(
    const HeadlessRuntimeSnapshot &runtime, const std::string_view mode_id) {
  Require(!mode_id.empty() && mode_id.size() <= 256U,
          "assistant_mode_projection_invalid",
          "assistant mode identity must be bounded non-empty text");
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  if (std::holds_alternative<std::nullptr_t>(manager_payload.storage())) {
    CoreFail("assistant_mode_projection_unavailable",
             "Manager has no current or last-valid active-bundle selection");
  }
  Require(manager_payload.is_object(), "assistant_mode_projection_invalid",
          "Manager active-bundle selection is not an object");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto retained_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto graph_revision = RequiredString(active, "graph_revision");
  const auto manager_ready = RequiredBool(manager_payload, "manager_ready");
  const auto &manager_runtime = RequiredMember(manager_payload, "runtime");
  const auto manager_converged = RequiredBool(manager_runtime, "converged");
  Require(manager_ready == manager_converged,
          "assistant_mode_projection_invalid",
          "Manager readiness and runtime convergence disagree");

  const auto active_bundle = VerifiedActiveBundleLocked(
      retained_bundle_id, graph_revision, "assistant_mode_projection_invalid",
      "assistant_mode_projection_unavailable");

  CanonicalValue projection;
  const std::string mode_key(mode_id);
  const auto cached = assistant_mode_projection_cache_.find(mode_key);
  if (cached != assistant_mode_projection_cache_.end()) {
    projection = cached->second;
  } else {
    try {
      projection = ProjectAssistantMode(*active_bundle, mode_id);
    } catch (const AssistantModeProjectionError &error) {
      CoreFail(error.code(), error.what());
    }
    assistant_mode_projection_cache_.emplace(mode_key, projection);
  }
  const auto semantic_digest = content_identity::framed_sha256(
      kAssistantModeProjectionIdentityDomain,
      kAssistantModeProjectionApiVersion, projection);
  CanonicalValue::Object result = projection.as_object();
  result.emplace_back(
      "source",
      CanonicalValue(CanonicalValue::Object{
          {"available", RequiredMember(manager_section, "available")},
          {"freshness", RequiredMember(manager_section, "freshness")},
          {"known_gaps", RequiredMember(manager_section, "known_gaps")},
          {"projection_generation",
           RequiredMember(manager_section, "projection_generation")},
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
           RequiredMember(manager_section, "projection_generation")},
          {"source_owner_epoch", CanonicalValue(config_.owner_epoch)},
          {"source_owner_id",
           CanonicalValue(config_.owner_id + ".assistant-mode." +
                          std::string(mode_id))},
          {"source_owner_kind", CanonicalValue("assistant_mode_projection")},
      }));
  return CanonicalValue(std::move(result));
}

CanonicalValue HeadlessApiCore::AssistantModeIndexLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  if (std::holds_alternative<std::nullptr_t>(manager_payload.storage())) {
    CoreFail("assistant_mode_projection_unavailable",
             "Manager has no current or last-valid active-bundle selection");
  }
  Require(manager_payload.is_object(), "assistant_mode_projection_invalid",
          "Manager active-bundle selection is not an object");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto retained_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto graph_revision = RequiredString(active, "graph_revision");
  const auto manager_ready = RequiredBool(manager_payload, "manager_ready");
  const auto &manager_runtime = RequiredMember(manager_payload, "runtime");
  const auto manager_converged = RequiredBool(manager_runtime, "converged");
  Require(manager_ready == manager_converged,
          "assistant_mode_projection_invalid",
          "Manager readiness and runtime convergence disagree");

  const auto active_bundle = VerifiedActiveBundleLocked(
      retained_bundle_id, graph_revision, "assistant_mode_projection_invalid",
      "assistant_mode_projection_unavailable");

  CanonicalValue projection;
  if (assistant_mode_index_cache_.has_value()) {
    projection = *assistant_mode_index_cache_;
  } else {
    try {
      projection = ProjectAssistantModeIndex(*active_bundle);
    } catch (const AssistantModeProjectionError &error) {
      CoreFail(error.code(), error.what());
    }
    assistant_mode_index_cache_ = projection;
  }
  const auto semantic_digest = content_identity::framed_sha256(
      kAssistantModeIndexIdentityDomain, kAssistantModeIndexApiVersion,
      projection);
  CanonicalValue::Object result = projection.as_object();
  result.emplace_back(
      "source",
      CanonicalValue(CanonicalValue::Object{
          {"available", RequiredMember(manager_section, "available")},
          {"freshness", RequiredMember(manager_section, "freshness")},
          {"known_gaps", RequiredMember(manager_section, "known_gaps")},
          {"projection_generation",
           RequiredMember(manager_section, "projection_generation")},
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
           RequiredMember(manager_section, "projection_generation")},
          {"source_owner_epoch", CanonicalValue(config_.owner_epoch)},
          {"source_owner_id",
           CanonicalValue(config_.owner_id + ".assistant-mode-index")},
          {"source_owner_kind", CanonicalValue("assistant_mode_index")},
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
  observed_runtime_collisions_.clear();
}

void HeadlessApiCore::ReconcileRuntimeCollisionStopLocked(
    const CanonicalValue &journal_payload, const std::uint64_t native_sequence,
    const std::string_view payload_digest, const std::int64_t occurred_at_ns,
    const std::string_view occurred_at_wall,
    const std::string_view raw_event_id) {
  const auto &intent = RequiredMember(journal_payload, "intent");
  if (RequiredString(intent, "intent_kind") != "ordinary_stop") {
    return;
  }
  const auto &value = RequiredMember(intent, "value");
  Require(value.is_object(), "headless_operation_event_invalid",
          "ordinary Stop projection value must be an object");
  const auto &context = RequiredMember(value, "request_context");
  const auto source_label = RequiredString(context, "source_label");
  if (source_label != "kernel.runtime_collision") {
    return;
  }
  const auto reason = OptionalStringMember(value, "reason");
  const auto correlation = OptionalStringMember(context, "correlation_id");
  const auto &affected = RequiredMember(journal_payload, "affected_state");
  const auto &operations = RequiredArray(affected, "operations");
  auto resources = StringArray(affected, "resources");
  Require(reason.has_value() && *reason == "runtime_collision_fault" &&
              correlation.has_value() && operations.size() == 1U &&
              !resources.empty(),
          "headless_operation_event_invalid",
          "kernel runtime-collision Stop projection has invalid scope");
  std::sort(resources.begin(), resources.end());
  const auto unique_end = std::unique(resources.begin(), resources.end());
  Require(unique_end == resources.end() &&
              resources.size() <= kMaximumAlarmSubjectResources,
          "headless_operation_event_invalid",
          "kernel runtime-collision Stop resources are invalid");
  const auto operation_id = RequiredString(operations.front(), "operation_id");
  const auto program_run_id =
      RequiredString(operations.front(), "program_run_id");
  const auto &acceptance = RequiredMember(journal_payload, "acceptance");
  const auto stop_id = RequiredString(acceptance, "stop_id");
  const auto stop_generation =
      RequiredCounter(acceptance, "stop_generation", 1U);
  const auto request_id = RequiredString(context, "request_id");
  const auto &selector = RequiredMember(value, "selector");
  const auto &selected_operations = RequiredArray(selector, "operations");
  bool participant_matches = false;
  for (const auto &participant :
       RequiredArray(operations.front(), "participants")) {
    participant_matches =
        participant_matches ||
        RequiredString(participant, "participant_id") == *correlation;
  }
  Require(RequiredString(context, "request_owner_epoch") ==
                  RequiredString(acceptance, "owner_epoch") &&
              request_id == RequiredString(acceptance, "request_id") &&
              RequiredString(selector, "selector_kind") == "operations" &&
              selected_operations.size() == 1U &&
              RequiredString(selected_operations.front(), "operation_id") ==
                  operation_id &&
              participant_matches,
          "headless_operation_event_invalid",
          "kernel runtime-collision Stop attribution is inconsistent");

  Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "operation alarm adapter sequence is exhausted");
  ++operation_adapter_.source_sequence;
  std::vector<std::string> evidence_refs;
  if (!raw_event_id.empty()) {
    evidence_refs.emplace_back(raw_event_id);
  }
  AlarmConditionUpdate update{
      "manyforge.motion.runtime_collision",
      AlarmSubject{"operation", operation_id, resources},
      true,
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
          {"reason", CanonicalValue(*reason)},
          {"source_label", CanonicalValue(source_label)},
          {"stop_generation", Number(stop_generation, "stop_generation")},
          {"stop_id", CanonicalValue(stop_id)},
      }),
      std::move(evidence_refs),
      CanonicalValue(CanonicalValue::Object{
          {"operation_id", CanonicalValue(operation_id)},
          {"participant_id", CanonicalValue(*correlation)},
          {"program_run_id", CanonicalValue(program_run_id)},
          {"request_id", CanonicalValue(request_id)},
      }),
  };
  const auto mutation = alarm_owner_.Submit(std::move(update));
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  active_runtime_collisions_.insert_or_assign(
      operation_id, AlarmSubject{"operation", operation_id, resources});
  observed_runtime_collisions_.insert(operation_id);
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
  if (reset_accepted) {
    const auto collision = active_runtime_collisions_.find(operation_id);
    if (collision != active_runtime_collisions_.end()) {
      Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
              "headless_adapter_sequence_exhausted",
              "operation alarm adapter sequence is exhausted");
      ++operation_adapter_.source_sequence;
      AlarmConditionUpdate update{
          "manyforge.motion.runtime_collision",
          collision->second,
          false,
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
              {"reason", CanonicalValue("operation_reset_accepted")},
              {"record_kind", CanonicalValue(std::string(record_kind))},
          }),
          raw_event_id.empty()
              ? std::vector<std::string>{}
              : std::vector<std::string>{std::string(raw_event_id)},
          CanonicalValue(CanonicalValue::Object{
              {"operation_id", CanonicalValue(operation_id)},
              {"program_binding_id",
               RequiredMember(state, "program_binding_id")},
              {"program_run_id", RequiredMember(state, "program_run_id")},
              {"request_id", request_id.empty()
                                 ? CanonicalValue(nullptr)
                                 : CanonicalValue(std::string(request_id))},
          }),
      };
      const auto mutation = alarm_owner_.Submit(std::move(update));
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      active_runtime_collisions_.erase(collision);
      observed_runtime_collisions_.erase(operation_id);
    }
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

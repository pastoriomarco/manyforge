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
              !config.node_environment_manifest_path.empty() &&
              !config.alarm_catalog_path.empty() &&
              !config.historian_database_path.empty(),
          "headless_api_configuration_invalid",
          "projection, install, artifact, Program-selection state, node-"
          "catalog, node-environment manifest, alarm-catalog, and historian "
          "paths are required");
  Require(!config.cell_id.empty() && !config.owner_id.empty() &&
              !config.owner_epoch.empty(),
          "headless_api_configuration_invalid",
          "cell and owner identities are required");
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

CanonicalValue ScenarioSelectionProjection(
    const deployment::ScenarioProgramSelection &selection,
    CanonicalValue owner_identities) {
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
      {"owner_identities", std::move(owner_identities)},
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

bool ValidateCapturedProgramOrigin(const CanonicalValue &state, bool allowed) {
  bool captured = false;
  for (const auto &participant : RequiredArray(state, "participants")) {
    const auto *scope = participant.find_object_member("execution_scope");
    if (scope == nullptr)
      continue;
    captured = true;
    Require(allowed && scope->is_object() && scope->as_object().size() == 2U,
            "headless_operation_event_invalid",
            "authored origin requires its exact journal version");
    const auto &node = RequiredString(*scope, "node_instance_id");
    const auto &attempt = RequiredString(*scope, "node_attempt_id");
    Require(!node.empty() && node.size() <= 128U &&
                node.find('\0') == std::string::npos && !attempt.empty() &&
                attempt.size() <= 512U &&
                attempt.find('\0') == std::string::npos,
            "headless_operation_event_invalid",
            "authored origin is invalid or unbounded");
  }
  if (captured) {
    const auto &order = RequiredMember(state, "journal_capture_order");
    Require(order.is_string() &&
                content_identity::parse_nonnegative_int64(order) > 0,
            "headless_operation_event_invalid",
            "authored origin requires an ordered snapshot");
  }
  return captured;
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
  if (record_kind == "stop_terminal_projected") {
    return "stop_terminal_projected";
  }
  if (record_kind == "stop_fault_cleared") {
    return "stop_fault_cleared";
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

bool SectionFresh(const CanonicalValue &section) {
  return RequiredBool(section, "available") &&
         RequiredString(section, "freshness") == "fresh";
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
    return std::filesystem::path("/proc/self/fd") / std::to_string(descriptor_);
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
      retained_bundle_source_(package_root_descriptor_->root(),
                              config_.artifact_root),
      scenario_catalog_source_(package_root_descriptor_->root(), "maintained",
                               config_.artifact_root),
      program_selection_owner_(
          package_root_descriptor_->root(), config_.artifact_root,
          config_.program_selection_state_root,
          config_.owner_id + ".program-selection", config_.owner_epoch),
      // This reader validates the final path component with symlink_status;
      // "." keeps that component directory-valued while traversal remains
      // anchored through the retained procfd root.
      node_environment_projection_(package_root_descriptor_->root() / ".",
                                   config_.node_catalog_path,
                                   config_.node_environment_manifest_path),
      source_model_projection_(package_root_descriptor_->root(),
                               config_.artifact_root),
      viewer_model_source_(package_root_descriptor_->root(),
                           config_.artifact_root),
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

CanonicalValue
HeadlessApiCore::AssistantModeContext(const std::string_view mode_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"assistant_mode", AssistantModeProjectionLocked(runtime, mode_id)},
      {"node_environment", NodeEnvironmentProjectionLocked(runtime)},
      {"schema_version",
       CanonicalValue(std::string(kAssistantModeContextSchemaVersion))},
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

  const auto selected =
      [&]() -> std::optional<deployment::ScenarioProgramSelection> {
    try {
      std::optional<CanonicalValue> applied;
      try {
        applied = program_selection_owner_.ReadApplied();
      } catch (const ProgramApplyError &error) {
        if (error.code() != "program_applied_selection_not_found") {
          throw;
        }
      }
      if (applied.has_value() &&
          RequiredString(*applied, "scenario_id") == scenario_id &&
          RequiredString(*applied, "retained_bundle_id") == active_bundle_id &&
          RequiredString(*applied, "graph_revision") == active_graph_revision) {
        auto selection =
            scenario_catalog_source_.LoadExactProgramSelectionForCatalog(
                scenario_id, core::ScenarioCatalogRevision(RequiredString(
                                 *applied, "scenario_catalog_revision")));
        if (selection.retained_bundle_id.value() != active_bundle_id ||
            selection.graph_revision.value() != active_graph_revision) {
          throw deployment::ScenarioApplyCandidateError(
              "Applied Scenario catalog names another active deployment");
        }
        return selection;
      }
      return scenario_catalog_source_.TryLoadExactProgramSelectionForActive(
          scenario_id, core::RetainedBundleId(active_bundle_id),
          core::GraphRevision(active_graph_revision));
    } catch (const deployment::ScenarioApplyCandidateError &error) {
      CoreFail("scenario_program_selection_unavailable", error.what());
    } catch (const deployment::ScenarioCatalogError &error) {
      CoreFail("scenario_program_selection_unavailable", error.what());
    } catch (const std::exception &error) {
      CoreFail("scenario_program_selection_invalid", error.what());
    }
  }();
  Require(selected.has_value(), "scenario_program_selection_not_applied",
          "scenario Program selection names another active deployment");
  const auto &selection = *selected;
  const auto environment = NodeEnvironmentProjectionLocked(runtime);
  const auto &package_environment =
      RequiredMember(environment, "package_environment");
  Require(RequiredString(package_environment, "environment_digest") ==
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
  return ScenarioSelectionProjection(
      selection,
      CanonicalValue(CanonicalValue::Array{
          CanonicalValue(CanonicalValue::Object{
              {"owner_epoch", CanonicalValue(config_.owner_epoch)},
              {"owner_id", CanonicalValue(config_.owner_id)},
              {"owner_role", CanonicalValue("headless_projection")},
          }),
          owner_identity("manager", manager_section),
          owner_identity("scene_runtime", scene_runtime_section),
          owner_identity("scene_transactions", logical_scene_section),
      }));
}

CanonicalValue
HeadlessApiCore::ScenarioLoadSelection(const std::string_view scenario_id) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  try {
    const auto runtime = RefreshLocked();
    const auto &manager_section = RequiredSection(runtime.root, "manager");
    if (SectionFresh(manager_section)) {
      const auto &manager_payload = RequiredMember(manager_section, "payload");
      if (manager_payload.is_object() &&
          RequiredBool(manager_payload, "manager_ready") &&
          RequiredBool(RequiredMember(manager_payload, "runtime"),
                       "converged")) {
        const auto &active = RequiredMember(manager_payload, "active");
        auto selected =
            scenario_catalog_source_.TryLoadExactProgramSelectionForActive(
                scenario_id,
                core::RetainedBundleId(
                    RequiredString(active, "retained_bundle_id")),
                core::GraphRevision(RequiredString(active, "graph_revision")));
        if (selected.has_value()) {
          return ScenarioSelectionProjection(
              *selected, CanonicalValue(CanonicalValue::Array{}));
        }
      }
    }
    return ScenarioSelectionProjection(
        scenario_catalog_source_.LoadExactProgramSelection(scenario_id),
        CanonicalValue(CanonicalValue::Array{}));
  } catch (const deployment::ScenarioApplyCandidateError &error) {
    CoreFail("scenario_program_selection_unavailable", error.what());
  } catch (const deployment::ScenarioCatalogError &error) {
    CoreFail("scenario_program_selection_unavailable", error.what());
  } catch (const std::exception &error) {
    CoreFail("scenario_program_selection_invalid", error.what());
  }
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
  const auto &package_environment =
      RequiredMember(environment, "package_environment");
  Require(RequiredString(package_environment, "environment_digest") ==
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
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "alarm acknowledgement targets a stale headless owner epoch");
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.Acknowledge(
      condition_id, activation_id, expected_record_revision, caller,
      std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
  SynchronizeHistorianHealthLocked();
  return MutationResponse(mutation, audit_ids);
}

CanonicalValue HeadlessApiCore::AcknowledgeAllAlarms(
    std::vector<AlarmAcknowledgementMember> members,
    const std::uint64_t expected_generation, AlarmCallerAttribution caller,
    std::optional<std::string> reason, const bool confirmed) {
  Require(members.size() <= kMaximumAlarmBulkMembers,
          "operator_alarm_acknowledge_all_members_invalid",
          "acknowledge-all membership exceeds the 256-member bound");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "bulk alarm acknowledgement targets a stale headless owner epoch");
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.AcknowledgeAll(
      std::move(members), caller.request_owner_epoch, expected_generation,
      caller, std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
  SynchronizeHistorianHealthLocked();
  return MutationResponse(mutation, audit_ids);
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

CanonicalValue HeadlessApiCore::HistoryQueryPage(const HistorianQuery &query,
                                                 const bool full_records) {
  Require(query.limit >= 1U && query.limit <= 32U, "historian_query_invalid",
          "public history page limit must be between 1 and 32");
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    SynchronizeHistorianHealthLocked();
    store = history_store_;
  }
  // Keep both the durable read and list projection outside the coordination
  // lock. The summary deliberately omits full payloads; callers fetch one
  // selected event through HistoryDetail instead.
  return full_records ? HistorianExportPageValue(store->QueryExport(query))
                      : store->QuerySummaries(query);
}

CanonicalValue HeadlessApiCore::HistoryDiscovery(const std::string &kind,
                                                 const std::string &after,
                                                 std::size_t limit) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    store = history_store_;
  }
  return store->Discover(kind, after, limit);
}

CanonicalValue HeadlessApiCore::HistoryDetail(const std::uint64_t sequence) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    SynchronizeHistorianHealthLocked();
    store = history_store_;
  }
  const auto stored = store->Read(sequence);
  Require(stored.has_value(), "historian_event_not_found",
          "the requested history event is not retained");
  return HistorianDetailValue(*stored);
}

CanonicalValue HeadlessApiCore::RecordClientIncident(
    std::string incident_id, std::string incident_kind, std::string phase,
    std::string control_session_id, std::string client_incarnation,
    std::string source_label, std::string reason_code,
    const std::uint64_t occurrence_count,
    const std::optional<std::uint64_t> maximum_delay_ns,
    const std::uint64_t first_observed_ns, const std::uint64_t last_observed_ns,
    std::map<std::string, std::string> causal_context) {
  const auto bounded_text = [](const std::string &value,
                               const std::size_t maximum_bytes) {
    return !value.empty() && value.size() <= maximum_bytes &&
           content_identity::is_utf8_nfc(value);
  };
  const std::set<std::string> allowed_kinds{
      "browser_render_delayed", "command_delivery_unconfirmed",
      "control_delayed",        "control_disconnected",
      "control_stale",          "history_detail_request_failed",
      "history_request_failed", "control_source_unavailable"};
  Require(bounded_text(incident_id, 256U) &&
              bounded_text(control_session_id, 256U) &&
              bounded_text(client_incarnation, 256U) &&
              bounded_text(source_label, 256U) &&
              bounded_text(reason_code, 256U),
          "client_incident_invalid",
          "client incident attribution must be bounded NFC text");
  Require(allowed_kinds.contains(incident_kind), "client_incident_invalid",
          "client incident kind is unsupported");
  Require(phase == "activated" || phase == "resolved",
          "client_incident_invalid", "client incident phase is unsupported");
  Require(occurrence_count > 0U && occurrence_count <= kMaximumSafeInteger &&
              first_observed_ns <= last_observed_ns &&
              (!maximum_delay_ns.has_value() ||
               *maximum_delay_ns <= kMaximumSafeInteger),
          "client_incident_invalid",
          "client incident counters or monotonic times are invalid");

  const bool warning = incident_kind == "control_delayed";
  const auto severity = warning ? "WARN" : "ERROR";
  Require(causal_context.size() <= 24, "client_incident_invalid",
          "incident context exceeds its fixed reference budget");
  const std::set<std::string> context_fields{"program_run_id",
                                             "program_runner_owner_epoch",
                                             "cycle_generation",
                                             "cycle_state",
                                             "reset_phase",
                                             "execution_control_revision",
                                             "run_revision",
                                             "program_id",
                                             "program_revision",
                                             "graph_revision",
                                             "retained_bundle_id",
                                             "program_binding_id",
                                             "node_environment_digest",
                                             "scene_id",
                                             "scene_revision_id",
                                             "scene_content_hash",
                                             "scene_source_owner_id",
                                             "scene_source_owner_epoch",
                                             "scene_source_generation",
                                             "scene_source_freshness",
                                             "program_source_current",
                                             "cold_source_current"};
  CanonicalValue::Object context{
      {"schema_version", CanonicalValue("manyforge.incident_context.v1")},
      {"capture_semantics", CanonicalValue("last_received_source_facts")}};
  for (auto &[name, value] : causal_context) {
    Require(context_fields.contains(name) && bounded_text(value, 1024U),
            "client_incident_invalid",
            "incident context contains an invalid reference");
    context.emplace_back(name, CanonicalValue(std::move(value)));
  }
  CanonicalValue payload(CanonicalValue::Object{
      {"causal_context", CanonicalValue(std::move(context))},
      {"client_incarnation", CanonicalValue(client_incarnation)},
      {"clock_owner", CanonicalValue("headless:" + config_.owner_epoch)},
      {"control_session_id", CanonicalValue(control_session_id)},
      {"first_observed_ns", CanonicalValue(std::to_string(first_observed_ns))},
      {"incident_id", CanonicalValue(incident_id)},
      {"incident_kind", CanonicalValue(incident_kind)},
      {"last_observed_ns", CanonicalValue(std::to_string(last_observed_ns))},
      {"maximum_delay_ns", maximum_delay_ns.has_value()
                               ? Number(*maximum_delay_ns, "maximum_delay_ns")
                               : CanonicalValue(nullptr)},
      {"occurrence_count", Number(occurrence_count, "occurrence_count")},
      {"original_severity", CanonicalValue(severity)},
      {"phase", CanonicalValue(phase)},
      {"reason_code", CanonicalValue(reason_code)},
      {"schema_version", CanonicalValue("manyforge.client_incident_event.v1")},
      {"source_label", CanonicalValue(source_label)},
  });

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  auto event = MakeHistorianEvent(
      "manyforge.composer", "client_incident_" + phase,
      phase == "resolved" ? "INFO" : severity, clocks_.monotonic_time_ns(),
      clocks_.wall_time_text(), std::move(payload));
  const auto event_id = event.event_id;
  const bool recorded = historian_.TryEnqueue(std::move(event));
  if (!recorded) {
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        "historian delivery queue rejected a client incident transition");
  }
  SynchronizeHistorianHealthLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)},
      {"schema_version", CanonicalValue("manyforge.client_incident_record.v1")},
  });
}

CanonicalValue HeadlessApiCore::HistorianHealth() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  SynchronizeHistorianHealthLocked();
  return AsyncHistorianHealthValue(historian_.Health());
}

CanonicalValue
HeadlessApiCore::RecordGripperConfirmation(CanonicalValue record) {
  const std::string code = "gripper_confirmation_trace_invalid";
  const std::set<std::string> fields{"schema_version",
                                     "origin_epoch",
                                     "request_id",
                                     "source_label",
                                     "native_owner_epoch",
                                     "program_binding_id",
                                     "target_role_id",
                                     "command_id",
                                     "generation",
                                     "expected_status_revision",
                                     "confirmed",
                                     "phase",
                                     "details"};
  Require(record.is_object() && record.as_object().size() == fields.size(),
          code, "Gripper confirmation trace has invalid fields");
  for (const auto &[name, value] : record.as_object())
    Require(fields.contains(name), code,
            "Gripper confirmation trace has an unknown field");
  for (const auto name : {"origin_epoch", "request_id", "source_label",
                          "native_owner_epoch", "target_role_id"}) {
    const auto value = RequiredString(record, name);
    Require(!value.empty() && value.size() <= 256U &&
                content_identity::is_utf8_nfc(value),
            code, "Gripper confirmation trace identity is invalid");
  }
  Require(RequiredString(record, "schema_version") ==
                  "manyforge.gripper_confirmation_trace.v1" &&
              content_identity::is_sha256_digest(
                  RequiredString(record, "program_binding_id")),
          code, "Gripper confirmation trace version or binding is invalid");
  for (const auto name :
       {"command_id", "generation", "expected_status_revision"}) {
    const auto value =
        content_identity::parse_nonnegative_int64(RequiredMember(record, name));
    Require(value > 0 &&
                value <= static_cast<std::int64_t>(kMaximumSafeInteger),
            code, "Gripper confirmation trace counter is invalid");
  }
  Require(std::holds_alternative<bool>(
              RequiredMember(record, "confirmed").storage()),
          code, "Gripper confirmation trace result is not boolean");
  const auto phase = RequiredString(record, "phase");
  Require(phase == "attempted" || phase == "accepted" || phase == "unconfirmed",
          code, "Gripper confirmation trace phase is invalid");
  Require(RequiredMember(record, "details").is_object() &&
              content_identity::canonical_json(record).size() <= 8192U,
          code, "Gripper confirmation trace exceeds its byte bound");
  auto payload = record.as_object();
  payload.emplace_back("clock_owner",
                       CanonicalValue("headless:" + config_.owner_epoch));
  payload.emplace_back("evidence_kind",
                       CanonicalValue("operator_attestation_trace"));
  auto event = MakeHistorianEvent(
      "manyforge.gripper", "gripper_confirmation_" + phase,
      phase == "unconfirmed" ? "ERROR" : "INFO", clocks_.monotonic_time_ns(),
      clocks_.wall_time_text(), CanonicalValue(std::move(payload)));
  const auto event_id = event.event_id;
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const bool recorded = historian_.TryEnqueue(std::move(event));
  if (!recorded)
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        "historian queue rejected a gripper confirmation trace");
  SynchronizeHistorianHealthLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.gripper_confirmation_trace_record.v1")},
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)}});
}

CanonicalValue HeadlessApiCore::RecordProgramDiagnostic(CanonicalValue record) {
  const std::string code = "program_diagnostic_invalid";
  const std::set<std::string> fields{"schema_version",
                                     "program_run_id",
                                     "owner_epoch",
                                     "program_revision",
                                     "program_binding_id",
                                     "retained_bundle_id",
                                     "graph_revision",
                                     "state",
                                     "reason_code",
                                     "message",
                                     "occurred_at_ns",
                                     "occurred_at_wall",
                                     "blackboard",
                                     "runtime_overrides",
                                     "omitted_blackboard_count",
                                     "omitted_override_count",
                                     "delivery_loss_count"};
  Require(record.is_object() && record.as_object().size() == fields.size(),
          code, "Program diagnostic fields are invalid");
  for (const auto &[name, value] : record.as_object()) {
    Require(fields.contains(name), code, "Program diagnostic field is unknown");
  }
  const auto text = [&](const std::string_view name, std::size_t limit = 512U) {
    const auto value = RequiredString(record, name);
    Require(value.size() <= limit && content_identity::is_utf8_nfc(value), code,
            "Program diagnostic text is invalid");
    return value;
  };
  Require(text("schema_version") == "manyforge.program_diagnostic.v1", code,
          "Program diagnostic version is unsupported");
  text("program_run_id");
  const auto epoch = text("owner_epoch");
  for (const auto name : {"program_revision", "program_binding_id",
                          "retained_bundle_id", "graph_revision"}) {
    Require(content_identity::is_sha256_digest(text(name)), code,
            "Program diagnostic artifact identity is invalid");
  }
  const auto state = text("state");
  Require(state == "succeeded" || state == "failed" ||
              state == "intervention_required",
          code, "Program diagnostic transition is unsupported");
  const auto reason = OptionalStringMember(record, "reason_code");
  Require(!reason || (reason->size() <= 512U &&
                      content_identity::is_utf8_nfc(*reason)),
          code, "Program diagnostic reason is invalid");
  text("message", 4096U);
  const auto wall = text("occurred_at_wall", 64U);
  Require(HistorianRecordedUtcNs(wall).has_value(), code,
          "Program diagnostic UTC is invalid");
  const auto monotonic = content_identity::parse_nonnegative_int64(
      RequiredMember(record, "occurred_at_ns"));
  const auto bounded_value = [&](const auto &visit, const CanonicalValue &value,
                                 std::size_t depth, std::size_t &items,
                                 std::int64_t &bytes) -> void {
    bytes -= 2; // Conservative separators/brackets, as at the producer.
    Require(++items <= 512U && depth <= 16U && bytes >= 0, code,
            "Program diagnostic value exceeds traversal or byte bounds");
    if (value.is_object()) {
      for (const auto &[key, child] : value.as_object()) {
        visit(visit, CanonicalValue(key), depth + 1U, items, bytes);
        visit(visit, child, depth + 1U, items, bytes);
      }
    } else if (value.is_array()) {
      for (const auto &child : value.as_array()) {
        visit(visit, child, depth + 1U, items, bytes);
      }
    } else {
      const auto *scalar = std::get_if<std::string>(&value.storage());
      Require(scalar == nullptr || (scalar->size() <= 2048U &&
                                    content_identity::is_utf8_nfc(*scalar)),
              code, "Program diagnostic string is invalid");
      bytes -= static_cast<std::int64_t>(
          content_identity::canonical_json(value).size());
      Require(bytes >= 0, code, "Program diagnostic value exceeds byte bound");
    }
  };
  for (const auto name : {"blackboard", "runtime_overrides"}) {
    const auto &values = RequiredMember(record, name);
    Require(values.is_object() && values.as_object().size() <= 32U, code,
            "Program diagnostic values exceed their entry bound");
    std::int64_t budget = 16 * 1024;
    for (const auto &[key, value] : values.as_object()) {
      std::size_t items = 0U;
      std::int64_t bytes = 16 * 1024 - 2;
      bounded_value(bounded_value, CanonicalValue(key), 0U, items, bytes);
      bounded_value(bounded_value, value, 0U, items, bytes);
      budget -= static_cast<std::int64_t>(
          content_identity::canonical_json(
              CanonicalValue(CanonicalValue::Object{{key, value}}))
              .size());
      Require(budget >= 0, code,
              "Program diagnostic section exceeds byte bound");
    }
  }
  for (const auto name : {"omitted_blackboard_count", "omitted_override_count",
                          "delivery_loss_count"}) {
    const auto value =
        content_identity::parse_nonnegative_int64(RequiredMember(record, name));
    Require(value <= (std::string_view(name) == "delivery_loss_count"
                          ? static_cast<std::int64_t>(kMaximumSafeInteger)
                          : 100000),
            code, "Program diagnostic counter is invalid");
  }
  Require(content_identity::canonical_json(record).size() <= 64U * 1024U, code,
          "Program diagnostic exceeds its byte bound");
  auto payload = record.as_object();
  payload.emplace_back("clock_owner",
                       CanonicalValue("program_runner:" + epoch));
  // Attribution is not authentication, command acceptance, or physical proof.
  payload.emplace_back("evidence_kind",
                       CanonicalValue("attributed_program_values"));
  auto event =
      MakeHistorianEvent("manyforge.program_runner", "program_" + state,
                         state == "succeeded" ? "INFO" : "ERROR", monotonic,
                         wall, CanonicalValue(std::move(payload)));
  const auto event_id = event.event_id;
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const bool recorded = historian_.TryEnqueue(std::move(event));
  if (!recorded) {
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        "historian queue rejected a Program diagnostic");
  }
  SynchronizeHistorianHealthLocked();
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_diagnostic_record.v1")},
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)}});
}

CanonicalValue HeadlessApiCore::ReconcileHistorianHealth(
    const std::uint64_t expected_health_generation,
    AlarmCallerAttribution caller) {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  ValidateAlarmCallerAttribution(caller);
  Require(caller.request_owner_epoch == config_.owner_epoch,
          "stale_request_owner_epoch",
          "historian reconciliation targets a stale headless owner epoch");
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
  return CanonicalValue(CanonicalValue::Object{
      {"audit_event_ids", StringArrayValue(audit_event_ids)},
      {"health", AsyncHistorianHealthValue(historian_.Health())},
      {"reconciled", CanonicalValue(reconciled)},
      {"schema_version",
       CanonicalValue("manyforge.historian_reconciliation.v1")},
  });
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
  const auto occurred_at_ns = content_identity::parse_nonnegative_int64(
      RequiredMember(record, "occurred_at_ns"));
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
  const bool stop_terminal_projection =
      record_kind == "stop_terminal_projected";
  const bool stop_fault_clear_projection = record_kind == "stop_fault_cleared";
  const bool stop_scoped_projection = stop_acceptance_projection ||
                                      stop_terminal_projection ||
                                      stop_fault_clear_projection;
  Require(!source_owner_epoch.empty() && source_sequence != 0U &&
              source_sequence <= kMaximumSafeInteger && !record_kind.empty() &&
              has_operation_id == has_operation_revision &&
              (stop_scoped_projection != has_operation_id) &&
              (!has_operation_id ||
               (!operation_id->empty() && *operation_revision != 0U &&
                *operation_revision <= kMaximumSafeInteger)) &&
              occurred_at_ns >= 0 && !occurred_at_wall.empty() &&
              occurred_at_wall.size() <= 128U &&
              content_identity::is_utf8_nfc(occurred_at_wall) &&
              (!request_id.has_value() || !request_id->empty()) &&
              content_identity::is_sha256_digest(payload_digest) &&
              journal_payload.is_object(),
          "headless_operation_event_invalid",
          "operation event envelope is malformed");
  const std::set<std::string_view> supported_kinds{"operation_accepted",
                                                   "control_accepted",
                                                   "stop_acceptance_projected",
                                                   "stop_terminal_projected",
                                                   "stop_fault_cleared",
                                                   "runtime_scale_accepted",
                                                   "operation_prepared",
                                                   "state_observed",
                                                   "host_replaced"};
  Require(supported_kinds.contains(record_kind),
          "headless_operation_event_invalid",
          "operation event record kind is unsupported");
  const auto payload_json = content_identity::canonical_json(journal_payload);
  Require(content_identity::sha256_digest(payload_json) == payload_digest &&
              RequiredString(journal_payload, "record_kind") == record_kind,
          "headless_operation_event_invalid",
          "operation event payload does not match its durable envelope");
  const CanonicalValue *operation_state = nullptr;
  std::optional<std::uint64_t> stop_terminal_duration_ns;
  std::optional<std::string> stop_terminal_result;
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
    const auto api = RequiredString(journal_payload, "api_version");
    const bool scoped = api == "manyforge.stop_acceptance_projection.v3";
    Require((scoped || api == "manyforge.stop_acceptance_projection.v2") &&
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
    bool captured = false;
    for (const auto &operation : RequiredArray(affected_state, "operations")) {
      captured = ValidateCapturedProgramOrigin(operation, scoped) || captured;
    }
    Require(captured == scoped, "headless_operation_event_invalid",
            "Stop projection version does not match authored origin");
  } else if (stop_terminal_projection) {
    const auto &acceptance = RequiredMember(journal_payload, "acceptance");
    const auto &attempted_stages =
        RequiredArray(journal_payload, "attempted_stages");
    const auto &certificate_evidence =
        RequiredArray(journal_payload, "certificate_evidence");
    const auto &participant_settlement =
        RequiredArray(journal_payload, "participant_settlement");
    const auto details_truncated =
        RequiredBool(journal_payload, "details_truncated");
    const auto request = RequiredString(journal_payload, "request_id");
    const auto stop_id = RequiredString(journal_payload, "stop_id");
    const auto stop_generation =
        RequiredCounter(journal_payload, "stop_generation", 1U);
    const auto accepted_at_ns = content_identity::parse_nonnegative_int64(
        RequiredMember(acceptance, "accepted_at_ns"));
    const auto completed_at_ns = content_identity::parse_nonnegative_int64(
        RequiredMember(journal_payload, "completed_at_ns"));
    const auto duration_ns =
        RequiredCounter(journal_payload, "acceptance_to_terminal_latency_ns");
    const auto attempted_stage_count =
        RequiredCounter(journal_payload, "attempted_stage_count");
    const auto certificate_evidence_count =
        RequiredCounter(journal_payload, "certificate_evidence_count");
    const auto participant_settlement_count =
        RequiredCounter(journal_payload, "participant_settlement_count");
    const auto result = RequiredString(journal_payload, "result");
    const auto state = RequiredString(journal_payload, "state");
    const bool successful =
        result == "succeeded_nominal" || result == "succeeded_escalated";
    Require(
        RequiredString(journal_payload, "api_version") ==
                "manyforge.stop_terminal_projection.v1" &&
            RequiredString(journal_payload, "record_kind") == record_kind &&
            RequiredString(journal_payload, "owner_epoch") ==
                source_owner_epoch &&
            !request_id.has_value() && !request.empty() && !stop_id.empty() &&
            RequiredString(acceptance, "request_id") == request &&
            RequiredString(acceptance, "stop_id") == stop_id &&
            RequiredCounter(acceptance, "stop_generation", 1U) ==
                stop_generation &&
            !RequiredString(journal_payload, "stop_certificate_id").empty() &&
            RequiredCounter(journal_payload, "stop_certificate_revision", 1U) !=
                0U &&
            content_identity::is_sha256_digest(
                RequiredString(journal_payload, "durable_projection_digest")) &&
            completed_at_ns >= accepted_at_ns &&
            duration_ns ==
                static_cast<std::uint64_t>(completed_at_ns - accepted_at_ns) &&
            (successful || result == "partial" ||
             result == "failed_uncontained") &&
            ((successful && state == "succeeded") ||
             (result == "partial" && state == "partial") ||
             (result == "failed_uncontained" && state == "failed")) &&
            RequiredBool(journal_payload, "escalated") ==
                (result == "succeeded_escalated") &&
            (details_truncated ||
             (attempted_stages.size() == attempted_stage_count &&
              certificate_evidence.size() == certificate_evidence_count &&
              participant_settlement.size() == participant_settlement_count)) &&
            (!details_truncated ||
             (attempted_stages.empty() && certificate_evidence.empty() &&
              participant_settlement.empty())) &&
            !effect_pending,
        "headless_operation_event_invalid",
        "Stop terminal projection does not match its durable envelope");
    stop_terminal_duration_ns = duration_ns;
    stop_terminal_result = result;
  } else if (stop_fault_clear_projection) {
    const auto &fault_latch = RequiredMember(journal_payload, "fault_latch");
    const auto &result = RequiredMember(journal_payload, "result");
    const auto &certificate = RequiredMember(fault_latch, "stop_certificate");
    const auto request = RequiredString(journal_payload, "request_id");
    const auto stop_id = RequiredString(journal_payload, "stop_id");
    const auto stop_generation =
        RequiredCounter(journal_payload, "stop_generation", 1U);
    Require(RequiredString(journal_payload, "api_version") ==
                    "manyforge.stop_fault_clear_projection.v1" &&
                RequiredString(journal_payload, "owner_epoch") ==
                    source_owner_epoch &&
                request_id.has_value() && *request_id == request &&
                result.is_object() && RequiredBool(result, "accepted") &&
                RequiredString(result, "request_id") == request &&
                std::holds_alternative<std::nullptr_t>(
                    RequiredMember(result, "error").storage()) &&
                fault_latch.is_object() &&
                RequiredString(fault_latch, "state") == "cleared" &&
                RequiredString(fault_latch, "recovery_status") == "cleared" &&
                RequiredString(fault_latch, "reason_code") ==
                    "stop_fault_recovery_cleared" &&
                RequiredString(fault_latch, "stop_id") == stop_id &&
                RequiredCounter(fault_latch, "stop_generation", 1U) ==
                    stop_generation &&
                certificate.is_object() &&
                RequiredString(certificate, "stop_certificate_id") ==
                    RequiredString(journal_payload, "stop_certificate_id") &&
                RequiredCounter(certificate, "stop_certificate_revision", 1U) ==
                    RequiredCounter(journal_payload,
                                    "stop_certificate_revision", 1U) &&
                !effect_pending,
            "headless_operation_event_invalid",
            "Stop fault-clear projection does not match its durable envelope");
  } else {
    const auto record_api = RequiredString(journal_payload, "api_version");
    Require((record_api == "manyforge.operation_journal_record.v1" ||
             record_api == "manyforge.operation_journal_record.v2" ||
             record_api == "manyforge.operation_journal_record.v3") &&
                RequiredString(journal_payload, "owner_epoch") ==
                    source_owner_epoch &&
                RequiredBool(journal_payload, "effect_pending") ==
                    effect_pending,
            "headless_operation_event_invalid",
            "operation event payload does not match its durable envelope");
    operation_state = &RequiredMember(journal_payload, "operation_state");
    const auto *capture_order =
        operation_state->find_object_member("journal_capture_order");
    Require(
        record_api == "manyforge.operation_journal_record.v1"
            ? capture_order == nullptr
            : (capture_order != nullptr && capture_order->is_string() &&
               content_identity::parse_nonnegative_int64(*capture_order) > 0),
        "headless_operation_event_invalid",
        "operation journal capture order does not match its payload version");
    Require(operation_state->is_object() &&
                RequiredString(*operation_state, "operation_id") ==
                    *operation_id &&
                RequiredCounter(*operation_state, "operation_revision", 1U) ==
                    *operation_revision,
            "headless_operation_event_invalid",
            "operation event state does not match its durable envelope");
    const bool captured_origin = ValidateCapturedProgramOrigin(
        *operation_state,
        record_api == "manyforge.operation_journal_record.v3");
    Require(captured_origin ==
                (record_api == "manyforge.operation_journal_record.v3"),
            "headless_operation_event_invalid",
            "journal version does not match authored origin");
  }
  const auto event_type =
      OperationHistoryEventType(record_kind, journal_payload);
  const auto severity =
      stop_terminal_result == "partial" ||
              stop_terminal_result == "failed_uncontained" ||
              (operation_state != nullptr &&
               IsOperationFaultState(RequiredString(*operation_state, "state")))
          ? "ERROR"
      : stop_terminal_result == "succeeded_escalated" ? "WARN"
                                                      : "INFO";
  const auto native_record_digest = content_identity::framed_sha256(
      "manyforge-operation-journal-event", "v1",
      CanonicalValue(CanonicalValue::Object{
          {"effect_pending", CanonicalValue(effect_pending)},
          {"occurred_at_ns",
           content_identity::nonnegative_int64_value(occurred_at_ns)},
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
  CanonicalValue::Object history_fields{
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
  };
  if (stop_terminal_duration_ns.has_value()) {
    history_fields.emplace_back(
        "duration_ns", Number(*stop_terminal_duration_ns, "duration_ns"));
  }
  CanonicalValue history_payload(std::move(history_fields));
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
    historian_.ReportDeliveryFailure("historian_projection_failure",
                                     error.what());
    SynchronizeHistorianHealthLocked();
  } catch (...) {
    historian_.ReportDeliveryFailure(
        "historian_projection_failure",
        "operation historian delivery raised an unknown failure");
    SynchronizeHistorianHealthLocked();
  }

  if (stop_acceptance_projection) {
    ReconcileRuntimeCollisionStopLocked(
        journal_payload, source_sequence, payload_digest, occurred_at_ns,
        occurred_at_wall, event_id.value_or(""));
  } else if (stop_terminal_projection || stop_fault_clear_projection) {
    ReconcileStopFaultLocked(journal_payload, record_kind, source_sequence,
                             payload_digest, occurred_at_ns, occurred_at_wall,
                             event_id.value_or(""));
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
    std::string source_owner_epoch, const std::uint64_t through_source_sequence,
    std::optional<CanonicalValue> current_stop_fault) {
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

  const bool current_fault_active =
      current_stop_fault.has_value() &&
      !std::holds_alternative<std::nullptr_t>(current_stop_fault->storage());
  if (current_stop_fault.has_value()) {
    if (current_fault_active) {
      ObserveCurrentStopFaultLocked(*current_stop_fault,
                                    through_source_sequence);
    } else {
      current_stop_fault_fingerprint_.clear();
    }
  }

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
  retired.clear();
  for (const auto &[latch, subject] : active_stop_faults_) {
    // A selected current latch is not a complete set of unresolved faults.
    // Keep older identities until exact clear or an authoritative null
    // snapshot.
    if (!current_stop_fault.has_value() || current_fault_active) {
      continue;
    }
    retired.push_back(latch);
    Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
            "headless_adapter_sequence_exhausted",
            "operation alarm adapter sequence is exhausted");
    ++operation_adapter_.source_sequence;
    AlarmConditionUpdate update{
        "manyforge.motion.stop_fault",
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
            {"reason", CanonicalValue("stop_fault_context_retired")},
        }),
        {},
        CanonicalValue(CanonicalValue::Object{
            {"latch_id", CanonicalValue(latch)},
        }),
    };
    const auto mutation = alarm_owner_.Submit(std::move(update));
    static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  }
  for (const auto &latch : retired) {
    active_stop_faults_.erase(latch);
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
  const bool complete = RequiredBool(runtime.root, "complete");
  if (complete) {
    runtime_health_baseline_observed_ = true;
  } else if (!runtime_health_baseline_observed_) {
    // Retained publications can be valid but stale while a workbench starts.
    // Until this Headless owner observes one complete fresh source set there
    // is no transition-to-unavailable fact to alarm on. Readiness remains
    // degraded, while the alarm domain stays unknown rather than fabricating
    // an acknowledgement obligation. A later loss after the fresh baseline
    // is authoritative and follows the normal alarm lifecycle below.
    alarm_owner_.MarkSourceQualityUnknown(runtime_health_adapter_.source_id,
                                          runtime_health_adapter_.source_epoch);
    return;
  }
  const bool active = !complete;
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
  current_stop_fault_fingerprint_.clear();
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

void HeadlessApiCore::ReconcileStopFaultLocked(
    const CanonicalValue &journal_payload, const std::string_view record_kind,
    const std::uint64_t native_sequence, const std::string_view payload_digest,
    const std::int64_t occurred_at_ns, const std::string_view occurred_at_wall,
    const std::string_view raw_event_id) {
  const auto &fault_latch = RequiredMember(journal_payload, "fault_latch");
  if (std::holds_alternative<std::nullptr_t>(fault_latch.storage())) {
    return;
  }
  Require(fault_latch.is_object(), "headless_operation_event_invalid",
          "Stop fault projection latch must be an object or null");
  const auto latch_id = RequiredString(fault_latch, "latch_id");
  const auto cell_id = RequiredString(fault_latch, "cell_id");
  const auto state = RequiredString(fault_latch, "state");
  const bool active = record_kind == "stop_terminal_projected";
  Require((active && (state == "latched" || state == "recovery_pending")) ||
              (!active && record_kind == "stop_fault_cleared" &&
               state == "cleared"),
          "headless_operation_event_invalid",
          "Stop fault projection state does not match its event kind");
  current_stop_fault_fingerprint_.clear();

  AlarmSubject subject;
  if (active) {
    subject = AlarmSubject{"cell", cell_id, {}};
    active_stop_faults_.insert_or_assign(latch_id, subject);
  } else {
    const auto found = active_stop_faults_.find(latch_id);
    if (found == active_stop_faults_.end()) {
      return;
    }
    subject = found->second;
    active_stop_faults_.erase(found);
    if (std::any_of(
            active_stop_faults_.begin(), active_stop_faults_.end(),
            [&](const auto &entry) { return entry.second == subject; })) {
      return;
    }
  }

  Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "operation alarm adapter sequence is exhausted");
  ++operation_adapter_.source_sequence;
  std::vector<std::string> evidence_refs;
  if (!raw_event_id.empty()) {
    evidence_refs.emplace_back(raw_event_id);
  }
  const auto &certificate = RequiredMember(fault_latch, "stop_certificate");
  Require(certificate.is_object(), "headless_operation_event_invalid",
          "Stop fault projection must name its exact certificate");
  AlarmConditionUpdate update{
      "manyforge.motion.stop_fault",
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
          {"fault_latch", fault_latch},
          {"native_sequence", Number(native_sequence, "native_sequence")},
          {"payload_digest", CanonicalValue(std::string(payload_digest))},
          {"record_kind", CanonicalValue(std::string(record_kind))},
      }),
      std::move(evidence_refs),
      CanonicalValue(CanonicalValue::Object{
          {"cell_id", CanonicalValue(cell_id)},
          {"latch_id", CanonicalValue(latch_id)},
          {"stop_certificate_id",
           RequiredMember(certificate, "stop_certificate_id")},
          {"stop_certificate_revision",
           RequiredMember(certificate, "stop_certificate_revision")},
          {"stop_generation", RequiredMember(fault_latch, "stop_generation")},
          {"stop_id", RequiredMember(fault_latch, "stop_id")},
      }),
  };
  const auto mutation = alarm_owner_.Submit(std::move(update));
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
}

void HeadlessApiCore::ObserveCurrentStopFaultLocked(
    const CanonicalValue &fault_latch, const std::uint64_t native_sequence) {
  Require(fault_latch.is_object() && fault_latch.as_object().size() == 19U,
          "headless_operation_event_invalid",
          "current Stop-fault condition has an invalid envelope");
  const auto latch_id = RequiredString(fault_latch, "latch_id");
  const auto cell_id = RequiredString(fault_latch, "cell_id");
  const auto state = RequiredString(fault_latch, "state");
  const auto recovery_status = RequiredString(fault_latch, "recovery_status");
  const auto trigger = RequiredString(fault_latch, "trigger");
  const auto stop_id = RequiredString(fault_latch, "stop_id");
  const auto stop_generation =
      RequiredCounter(fault_latch, "stop_generation", 1U);
  const auto &certificate = RequiredMember(fault_latch, "stop_certificate");
  const bool certificate_pending =
      std::holds_alternative<std::nullptr_t>(certificate.storage());
  Require(
      cell_id == config_.cell_id &&
          (state == "latched" || state == "recovery_pending") &&
          (recovery_status == "waiting_for_observation" ||
           recovery_status == "waiting_for_quiescence" ||
           recovery_status == "waiting_for_configuration" ||
           recovery_status == "waiting_for_external_reconciliation" ||
           recovery_status == "waiting_for_durable_record" ||
           recovery_status == "ready" ||
           recovery_status == "recovery_pending") &&
          (trigger == "scope_promotion" || trigger == "escalated" ||
           trigger == "partial" || trigger == "failed_uncontained" ||
           trigger == "physical_rest_unproven" ||
           trigger == "persistence_degraded") &&
          RequiredCounter(fault_latch, "latch_generation", 1U) != 0U &&
          RequiredCounter(fault_latch, "recovery_assessment_revision", 1U) !=
              0U &&
          RequiredMember(fault_latch, "captured_operation_ids").is_array() &&
          RequiredMember(fault_latch, "captured_resources").is_array() &&
          RequiredMember(fault_latch, "evidence_refs").is_array() &&
          RequiredMember(fault_latch, "terminal_projection").is_object() &&
          RequiredMember(fault_latch, "clear_projection").is_object() &&
          std::holds_alternative<bool>(
              RequiredMember(fault_latch, "persistence_degraded").storage()) &&
          !RequiredString(fault_latch, "graph_revision").empty() &&
          !RequiredString(fault_latch, "reason_code").empty() &&
          !RequiredString(fault_latch, "retained_bundle_id").empty() &&
          ((certificate_pending && state == "latched" &&
            recovery_status == "waiting_for_observation" &&
            RequiredString(RequiredMember(fault_latch, "terminal_projection"),
                           "state") == "pending" &&
            RequiredString(RequiredMember(fault_latch, "clear_projection"),
                           "state") == "not_applicable") ||
           (certificate.is_object() && certificate.as_object().size() == 4U &&
            RequiredString(certificate, "stop_id") == stop_id &&
            RequiredCounter(certificate, "stop_generation", 1U) ==
                stop_generation &&
            RequiredCounter(certificate, "stop_certificate_revision", 1U) !=
                0U &&
            !RequiredString(certificate, "stop_certificate_id").empty())),
      "headless_operation_event_invalid",
      "current Stop-fault condition is internally inconsistent");

  const AlarmSubject subject{"cell", cell_id, {}};
  const auto payload_digest = content_identity::sha256_digest(
      content_identity::canonical_json(fault_latch));
  const auto found = active_stop_faults_.find(latch_id);
  if (found != active_stop_faults_.end()) {
    Require(found->second == subject, "headless_operation_event_invalid",
            "current Stop-fault condition changed subject identity");
    if (current_stop_fault_fingerprint_ == payload_digest)
      return;
  }

  active_stop_faults_.insert_or_assign(latch_id, subject);
  Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "operation alarm adapter sequence is exhausted");
  ++operation_adapter_.source_sequence;
  AlarmConditionUpdate update{
      "manyforge.motion.stop_fault",
      subject,
      true,
      operation_adapter_.source_id,
      operation_adapter_.source_epoch,
      operation_adapter_.source_sequence,
      clocks_.monotonic_time_ns(),
      clocks_.wall_time_text(),
      AlarmConditionQuality::kGood,
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Object{
          {"fault_latch", fault_latch},
          {"native_sequence", Number(native_sequence, "native_sequence")},
          {"payload_digest", CanonicalValue(payload_digest)},
          {"record_kind", CanonicalValue("current_stop_fault_snapshot")},
      }),
      {},
      CanonicalValue(CanonicalValue::Object{
          {"cell_id", CanonicalValue(cell_id)},
          {"latch_id", CanonicalValue(latch_id)},
          {"stop_certificate_id",
           certificate_pending
               ? CanonicalValue(nullptr)
               : RequiredMember(certificate, "stop_certificate_id")},
          {"stop_certificate_revision",
           certificate_pending
               ? CanonicalValue(nullptr)
               : RequiredMember(certificate, "stop_certificate_revision")},
          {"stop_generation", RequiredMember(fault_latch, "stop_generation")},
          {"stop_id", RequiredMember(fault_latch, "stop_id")},
      }),
  };
  const auto mutation = alarm_owner_.Submit(std::move(update));
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  current_stop_fault_fingerprint_ = payload_digest;
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
      historian_.ReportDeliveryFailure("historian_projection_failure",
                                       error.what());
      SynchronizeHistorianHealthLocked();
    } catch (...) {
      historian_.ReportDeliveryFailure(
          "historian_projection_failure",
          "historian delivery raised an unknown failure");
      SynchronizeHistorianHealthLocked();
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

} // namespace manyforge::headless

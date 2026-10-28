#include "manyforge_headless/headless_api_core.hpp"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <map>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/carried_object_lost.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
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
constexpr std::string_view kAlarmResourceRegistryHealthCode =
    "operator_alarm_resource_registry_unavailable";

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
              !config.node_catalog_path.empty() &&
              !config.node_environment_manifest_path.empty() &&
              !config.alarm_catalog_path.empty() &&
              !config.historian_database_path.empty(),
          "headless_api_configuration_invalid",
          "projection, install, artifact, node-catalog, node-environment "
          "manifest, alarm-catalog, and historian "
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

HistorianEvent PreviousSessionReportEvent(const CanonicalValue &report) {
  constexpr auto code = "headless_previous_session_report_invalid";
  Require(report.is_object() && report.as_object().size() == 8U, code,
          "previous-session report must contain its eight fixed fields");
  const auto text = [&](const std::string_view field) {
    const auto value = RequiredString(report, field);
    Require(value.size() <= 1024U && content_identity::is_utf8_nfc(value),
            code, "previous-session report text must be bounded NFC UTF-8");
    return value;
  };
  Require(text("schema_version") == "manyforge.workbench_previous_session.v1",
          code, "previous-session report version is unsupported");
  const auto session_id = text("session_id");
  Require(session_id.size() == 32U &&
              std::all_of(session_id.begin(), session_id.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
              }) &&
              text("archive_relative_path") == "previous-sessions/" + session_id,
          code, "previous-session archive identity is invalid");
  const auto shutdown = text("shutdown");
  const auto effects = text("effects");
  Require((shutdown == "clean" || shutdown == "unknown") &&
              (effects == "closed" || effects == "unresolved" || effects == "unknown") &&
              text("history_coverage") == "not_imported", code,
          "previous-session report classification is invalid");
  static_cast<void>(text("message"));
  const auto &families = RequiredMember(report, "journal_families");
  const std::array<std::string_view, 5U> expected{
      "program_cycle", "scene", "manager", "cclc", "kernel_operation"};
  Require(families.is_array() && families.as_array().size() == expected.size(),
          code, "previous-session report must name all five journal families");
  for (std::size_t i = 0U; i < expected.size(); ++i) {
    const auto *family = std::get_if<std::string>(&families.as_array()[i].storage());
    Require(family != nullptr && *family == expected[i], code,
            "previous-session report journal families differ from the fixed inventory");
  }
  // This report has no source occurrence clock. Keep it explicitly unknown
  // and stable across Headless restarts; do not assign an old event a new time.
  return MakeHistorianEvent("manyforge.workbench", "previous_session_report",
                            "INFO", 0, "unknown", report);
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

deployment::VerifiedRetainedModelMap LoadActiveProgramModelMap(
    const deployment::RetainedBundle &bundle,
    const deployment::FilesystemVerifiedRetainedModelMapSource &source) {
  Require(bundle.value().compiled_products.symbol_role_maps.size() == 1U,
          "program_applied_selection_invalid",
          "active retained bundle must select one exact model map");
  auto model_map = source.get_verified(deployment::RetainedModelMapQuery{
      bundle.retained_bundle_id(),
      bundle.value().compiled_products.symbol_role_maps.front().product_id});
  Require(model_map.has_value(), "program_applied_selection_invalid",
          "active retained model map is absent or unverifiable");
  return std::move(*model_map);
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

std::optional<std::string>
ManagerApplyTransitionId(const CanonicalValue &runtime) {
  const auto &manager = RequiredSection(runtime, "manager");
  const auto *payload = manager.find_object_member("payload");
  if (payload == nullptr || !payload->is_object()) {
    return std::nullopt;
  }
  const auto *apply = payload->find_object_member("apply");
  if (apply == nullptr || !apply->is_object()) {
    return std::nullopt;
  }
  const auto *records = apply->find_object_member("records");
  if (records == nullptr) {
    return std::nullopt;
  }
  const auto *rows = std::get_if<CanonicalValue::Array>(&records->storage());
  Require(rows != nullptr, "headless_source_invalid",
          "Manager Apply records must be an array");
  std::optional<std::string> transition;
  const auto admitted_phase = [](const std::string_view phase) noexcept {
    return phase == "fence_held" ||
           phase == "runtime_convergence_pending" ||
           phase == "runtime_staged_ready" ||
           phase == "scene_stage_pending" || phase == "scene_staged" ||
           phase == "activation_selected" || phase == "scene_activated" ||
           phase == "private_materialized" ||
           phase == "command_surfaces_retired" ||
           phase == "active_bundle_published" ||
           phase == "scene_finalized" ||
           phase == "fence_release_pending";
  };
  for (const auto &row : *rows) {
    Require(row.is_object(), "headless_source_invalid",
            "Manager Apply record must be an object");
    const auto *incomplete = row.find_object_member("incomplete");
    Require(incomplete != nullptr, "headless_source_invalid",
            "Manager Apply record has no completion state");
    const auto *pending = std::get_if<bool>(&incomplete->storage());
    Require(pending != nullptr, "headless_source_invalid",
            "Manager Apply completion state must be boolean");
    if (!*pending || !admitted_phase(RequiredString(row, "phase"))) {
      continue;
    }
    const auto &fence = RequiredMember(row, "fence");
    const auto fence_state = RequiredString(fence, "state");
    if (fence_state != "held" && fence_state != "release_pending") {
      continue;
    }
    const auto &scope = RequiredMember(fence, "scope");
    std::set<std::string> affected_components;
    for (const auto &value :
         RequiredArray(scope, "controllable_component_ids")) {
      const auto *component_id = std::get_if<std::string>(&value.storage());
      Require(component_id != nullptr, "headless_source_invalid",
              "Manager Apply component scope must contain strings");
      affected_components.insert(*component_id);
    }
    std::set<std::string> affected_units;
    const auto &active = RequiredMember(*payload, "active");
    const auto &graph = RequiredMember(active, "cell_graph");
    for (const auto &component :
         RequiredArray(graph, "controllable_components")) {
      if (!affected_components.contains(
              RequiredString(component, "controllable_component_id"))) {
        continue;
      }
      for (const auto &value : RequiredArray(component, "runtime_unit_ids")) {
        const auto *runtime_unit_id =
            std::get_if<std::string>(&value.storage());
        Require(runtime_unit_id != nullptr, "headless_source_invalid",
                "runtime unit scope must contain strings");
        affected_units.insert(*runtime_unit_id);
      }
    }
    std::set<std::string> desired_units;
    const auto &manager_runtime = RequiredMember(*payload, "runtime");
    for (const auto &unit : RequiredArray(manager_runtime, "desired_units")) {
      desired_units.insert(RequiredString(unit, "runtime_unit_id"));
    }
    // An empty active cell has no predecessor components to map to runtime
    // units. It may still have a cell-level host runtime unit.
    const bool initializing_empty_cell =
        RequiredArray(graph, "controllable_components").empty() &&
        !affected_components.empty();
    std::set<std::string> active_units;
    for (const auto &unit : RequiredArray(graph, "runtime_units")) {
      active_units.insert(RequiredString(unit, "runtime_unit_id"));
    }
    // desired_units describes the active predecessor, not the candidate.
    if (initializing_empty_cell ? desired_units != active_units
                                : desired_units.empty() ||
                                      affected_units != desired_units) {
      continue;
    }
    Require(!transition.has_value(), "headless_source_invalid",
            "Manager exposes more than one incomplete Apply");
    const auto generation = RequiredCounter(row, "apply_generation", 1U);
    transition =
        RequiredString(row, "apply_id") + ":" + std::to_string(generation);
  }
  return transition;
}

const CanonicalValue &
ValidateStopFaultProjection(const CanonicalValue &payload,
                            const std::string_view record_kind) {
  const auto &latch = RequiredMember(payload, "fault_latch");
  if (latch.is_null())
    return latch;
  Require(latch.is_object(), "headless_operation_event_invalid",
          "Stop fault projection latch must be an object or null");
  RequiredString(latch, "latch_id");
  RequiredString(latch, "cell_id");
  const auto state = RequiredString(latch, "state");
  const bool active = record_kind == "stop_terminal_projected";
  Require((active && (state == "latched" || state == "recovery_pending")) ||
              (!active && record_kind == "stop_fault_cleared" &&
               state == "cleared"),
          "headless_operation_event_invalid",
          "Stop fault projection state does not match its event kind");
  const auto &certificate = RequiredMember(latch, "stop_certificate");
  Require(certificate.is_object(), "headless_operation_event_invalid",
          "Stop fault projection must name its exact certificate");
  RequiredMember(certificate, "stop_certificate_id");
  RequiredMember(certificate, "stop_certificate_revision");
  RequiredMember(latch, "stop_generation");
  RequiredMember(latch, "stop_id");
  return latch;
}

bool IsOperationFaultState(const std::string_view state) noexcept {
  return state == "failed" || state == "interrupted" ||
         state == "intervention_required";
}

bool IsNormalOperationTerminal(const std::string_view state) noexcept {
  return state == "completed" || state == "stopped" ||
         state == "cancelled_pre_dispatch";
}

void ValidateInterruptRuleStateProjection(const CanonicalValue &payload,
                                          std::string_view source_owner_epoch) {
  constexpr auto code = "headless_operation_event_invalid";
  const auto closed = [&](const CanonicalValue &value,
                          std::initializer_list<std::string_view> fields) {
    Require(value.is_object() && value.as_object().size() == fields.size(),
            code, "rule-state projection has an invalid closed shape");
    for (const auto field : fields)
      Require(value.find_object_member(field) != nullptr, code,
              "rule-state projection field is missing");
  };
  const auto counter = [&](const CanonicalValue &value, std::string_view field) {
    const auto text = RequiredString(value, field);
    std::uint64_t result{};
    const auto parsed =
        std::from_chars(text.data(), text.data() + text.size(), result);
    Require(!text.empty() && text.front() != '0' &&
                parsed.ec == std::errc{} &&
                parsed.ptr == text.data() + text.size() && result != 0U,
            code, "rule-state counter is not a canonical positive uint64 string");
    return result;
  };
  closed(payload, {"owner_epoch", "catalog_generation",
                   "active_publication_generation", "retained_bundle_id",
                   "graph_revision", "rule_id", "state",
                   "triggering_acquisition"});
  const core::IoRuleStateContextDTO context{
      core::RequestOwnerEpoch(RequiredString(payload, "owner_epoch")),
      counter(payload, "catalog_generation"),
      core::ActiveBundlePublicationGeneration(
          RequiredCounter(payload, "active_publication_generation", 1U)),
      core::RetainedBundleId(RequiredString(payload, "retained_bundle_id")),
      core::GraphRevision(RequiredString(payload, "graph_revision"))};
  const auto state = RequiredString(payload, "state");
  Require(context.owner_epoch.value() == source_owner_epoch &&
              (state == "clear" || state == "tripped" || state == "latched"),
          code, "rule-state projection differs from its actual owner or state");
  const auto kind = state == "clear" ? core::IoGlobalRuleState::kClear
                  : state == "tripped" ? core::IoGlobalRuleState::kTripped
                                       : core::IoGlobalRuleState::kLatched;
  core::IoRuleStateDTO rule{
      core::InterruptPolicyId(RequiredString(payload, "rule_id")), kind,
      std::nullopt};
  const auto &acquired = RequiredMember(payload, "triggering_acquisition");
  if (!acquired.is_null()) {
    closed(acquired, {"source_id", "source_epoch", "source_sequence",
                      "source_cursor"});
    const auto &cursor = RequiredMember(acquired, "source_cursor");
    closed(cursor, {"owner_epoch", "catalog_generation", "source_id",
                    "record_sequence"});
    rule.triggering_acquisition = core::IoRuleAcquisitionKeyDTO{
        core::InterruptSourceId(RequiredString(acquired, "source_id")),
        core::IoSourceIncarnation(RequiredString(acquired, "source_epoch")),
        counter(acquired, "source_sequence"),
        {core::RequestOwnerEpoch(RequiredString(cursor, "owner_epoch")),
         counter(cursor, "catalog_generation"),
         core::InterruptSourceId(RequiredString(cursor, "source_id")),
         counter(cursor, "record_sequence")}};
  }
  Require(!core::io_rule_state_field_error(context, rule), code,
          "rule-state projection has invalid context or causal acquisition");
}

std::string
OperationHistoryEventType(const std::string_view record_kind,
                          const CanonicalValue &payload) {
  if (record_kind == "interrupt_rule_state_changed") {
    return "interrupt_rule_state_changed";
  }
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

// Registered bounded failure-detail keys. manyforge_headless_api tests check
// this set against the Python runtime's FAILURE_DETAIL_KEYS.
const std::set<std::string> kFailureDetailKeys{
    "input_problem",    "latch_id",      "path",
    "reason",           "reference_id",  "reference_type",
    "required_action",  "retryable",     "source_kind",
    "source_reference", "target_role_id"};

// Closed failure `details`/`origin` of a run record. Values pass through
// unchanged; this only rejects shapes outside their bounds.
void ValidateFailureDetailsAndOrigin(const CanonicalValue &details,
                                     const CanonicalValue &origin,
                                     const std::string &code) {
  if (!details.is_null()) {
    Require(details.is_object() && !details.as_object().empty() &&
                details.as_object().size() <= 16U &&
                content_identity::canonical_json(details).size() <= 2048U,
            code, "Failure details are empty or exceed their bounds");
    for (const auto &[key, value] : details.as_object()) {
      Require(kFailureDetailKeys.contains(key), code,
              "Failure detail key is not registered");
      const auto *text = std::get_if<std::string>(&value.storage());
      Require(!value.is_object() && !value.is_array() &&
                  (text == nullptr || text->size() <= 256U),
              code, "Failure detail value is not a bounded scalar");
    }
  }
  if (origin.is_null()) {
    Require(details.is_null(), code,
            "Failure details require a failure origin");
    return;
  }
  Require(origin.is_object() && origin.as_object().size() == 3U, code,
          "Failure origin fields are invalid");
  const auto *kind_member = origin.find_object_member("kind");
  const auto *kind = kind_member == nullptr
                         ? nullptr
                         : std::get_if<std::string>(&kind_member->storage());
  const auto identity =
      [&](const std::string_view name) -> const std::string * {
    const auto *member = origin.find_object_member(name);
    Require(member != nullptr, code, "Failure origin fields are invalid");
    if (member->is_null()) {
      return nullptr;
    }
    const auto *text = std::get_if<std::string>(&member->storage());
    Require(text != nullptr && !text->empty() && text->size() <= 256U, code,
            "Failure origin identity is invalid");
    return text;
  };
  const auto *node = identity("node_instance_id");
  const auto *owner_api = identity("owner_api");
  Require(kind != nullptr &&
              ((*kind == "program_node" && node != nullptr &&
                owner_api == nullptr) ||
               (*kind == "owner" && node != nullptr) ||
               (*kind == "program_runner" && owner_api == nullptr)),
          code, "Failure origin kind is invalid");
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
      program_repository_(config_.artifact_root),
      binding_repository_(config_.artifact_root),
      retained_model_map_source_(package_root_descriptor_->root(),
                                 config_.artifact_root),
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
                   config_.cell_id, config_.owner_id + ".alarm",
                   config_.owner_epoch,
                   // Captured once: this owner instance's incarnation.
                   manyforge::core::ReadHostBootStamp(),
                   clocks_.monotonic_time_ns, clocks_.wall_time_text,
                   // Headless currently owns operation and runtime-health
                   // alarm adapters.  Scene-initialization faults use the
                   // runtime-health adapter. The scene-domain carried-object
                   // alarm is optional for reconciliation.
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
      runtime_fault_adapter_{
          config_.owner_id + ".runtime-fault-adapter",
          "runtime_health",
          "manyforge.alarm_adapter.runtime_fault_condition",
          {}, 0U, 0U, {}, std::nullopt,
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
      scene_pose_adapter_{
          config_.owner_id + ".scene-carried-object-adapter", "scene",
          "manyforge.alarm_adapter.runtime_scene_condition", {}, 0U, 0U, {},
          std::nullopt},
      operation_adapter_{
          config_.owner_id + ".operation-condition-adapter",
          "motion",
          "manyforge.alarm_adapter.kernel_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      },
      program_failure_adapter_{
          config_.owner_id + ".program-failure-adapter", "motion",
          "manyforge.alarm_adapter.program_condition", {}, 0U, 0U, {}, std::nullopt},
      alarm_scope_adapter_{
          config_.owner_id + ".alarm-scope-adapter",
          "runtime_health",
          "manyforge.alarm_adapter.runtime_health_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      },
      component_lifecycle_adapter_{
          config_.owner_id + ".cclc-lifecycle-condition-adapter",
          "runtime_health",
          "manyforge.alarm_adapter.cclc_condition",
          {},
          0U,
          0U,
          {},
          std::nullopt,
      },
      workbench_session_adapter_{config_.owner_id + ".workbench-session-adapter",
                                 "runtime_health",
                                 "manyforge.alarm_adapter.workbench_session_condition",
                                 {}, 0U, 0U, {}, std::nullopt} {
  RegisterAdapterEpochLocked(runtime_health_adapter_,
                             config_.owner_epoch + ".runtime-health");
  // The retained refusals are this owner's own state and start empty, so the
  // alarm-scope source is complete from its first epoch.
  RegisterAdapterEpochLocked(alarm_scope_adapter_,
                             config_.owner_epoch + ".alarm-scope");
  alarm_owner_.CompleteSourceSnapshot(alarm_scope_adapter_.source_id,
                                      alarm_scope_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
}

HeadlessApiCore::~HeadlessApiCore() {
  try {
    Shutdown();
  } catch (...) {
  }
}

CanonicalValue
HeadlessApiCore::ObserveProgramFailureCondition(CanonicalValue condition) {
  constexpr std::string_view definition = "manyforge.program.operation_failure";
  const std::string code = "headless_program_failure_condition_invalid";
  try {
    Require(condition.is_object() && condition.as_object().size() == 10U, code,
            "Program failure observation has unsupported fields");
    const auto text = [&](const CanonicalValue &object, std::string_view name,
                          std::size_t maximum, bool empty = false) {
      const auto *encoded =
          std::get_if<std::string>(&RequiredMember(object, name).storage());
      Require(encoded != nullptr, code, "Program failure field must be text");
      const auto &value = *encoded;
      Require((empty || !value.empty()) && value.size() <= maximum &&
                  content_identity::is_utf8_nfc(value),
              code, "Program failure text is invalid: " + std::string(name));
      return value;
    };
    Require(text(condition, "schema_version", 128U) ==
                "manyforge.program_failure_condition.v1",
            code, "Program failure observation version is unsupported");
    const auto epoch = text(condition, "program_runner_owner_epoch", 512U);
    const auto run = text(condition, "program_run_id", 512U);
    const auto control_revision =
        RequiredCounter(condition, "execution_control_revision", 1U);
    const auto run_revision = RequiredCounter(condition, "run_revision", 1U);
    const auto state = text(condition, "state", 16U);
    Require(state == "active" || state == "clear" || state == "unknown", code,
            "Program failure observation state is unsupported");
    const auto &occurrence = RequiredMember(condition, "occurrence");
    const auto &operation = RequiredMember(condition, "operation");
    const auto &failure = RequiredMember(condition, "failure");
    Require(occurrence.is_object() && occurrence.as_object().size() == 3U &&
                operation.is_object() && operation.as_object().size() == 3U &&
                failure.is_object() && failure.as_object().size() == 2U,
            code, "Program failure provenance has unsupported fields");
    const auto node = text(occurrence, "node_instance_id", 128U);
    const auto attempt = text(occurrence, "node_attempt_id", 512U);
    Require(node.find('\0') == std::string::npos &&
                attempt.find('\0') == std::string::npos,
            code, "Program node scope is invalid");
    const auto facing_revision =
        RequiredCounter(occurrence, "operator_facing_run_revision", 1U);
    Require(run_revision >= facing_revision, code,
            "Program failure resolution does not follow its occurrence");
    const auto native_epoch = text(operation, "source_owner_epoch", 2048U);
    const auto operation_id = text(operation, "operation_id", 2048U);
    const auto operation_revision =
        RequiredCounter(operation, "operation_revision", 1U);
    static_cast<void>(text(failure, "code", 256U));
    static_cast<void>(text(failure, "message", 4096U, true));
    auto resources = StringArray(condition, "resource_ids");
    Require(
        !resources.empty() &&
            resources.size() <= kMaximumAlarmSubjectResources &&
            std::is_sorted(resources.begin(), resources.end()) &&
            std::adjacent_find(resources.begin(), resources.end()) ==
                resources.end(),
        code,
        "Program operation failure requires its exact sorted resource scope");
    for (const auto &resource : resources)
      Require(!resource.empty() && resource.size() <= 1024U &&
                  content_identity::is_utf8_nfc(resource),
              code, "Program failure resource is invalid");
    // Only the accepted internal Run source supplies operation applicability.
    // Resource IDs scope that operation; they do not classify a logic failure.
    const auto origin = CanonicalValue(CanonicalValue::Object{
        {"program_runner_owner_epoch", CanonicalValue(epoch)},
        {"program_run_id", CanonicalValue(run)},
        {"occurrence", occurrence},
        {"operation", operation}});
    const auto tracking_id = content_identity::framed_sha256(
        "manyforge-program-operation-failure", "v1", origin);
    const AlarmSubject subject{"program_operation", tracking_id, resources};
    const CanonicalValue details(
        CanonicalValue::Object{{"failure", failure},
                               {"node_instance_id", CanonicalValue(node)},
                               {"node_attempt_id", CanonicalValue(attempt)}});
    CanonicalValue correlations(CanonicalValue::Object{
        {"program_runner_owner_epoch", CanonicalValue(epoch)},
        {"program_run_id", CanonicalValue(run)},
        {"source_owner_epoch", CanonicalValue(native_epoch)},
        {"operation_id", CanonicalValue(operation_id)},
        {"operation_revision",
         Number(operation_revision, "operation_revision")},
        {"operator_facing_run_revision",
         Number(facing_revision, "operator_facing_run_revision")}});
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    const auto response = [](std::string status, bool changed) {
      return CanonicalValue(CanonicalValue::Object{
          {"schema_version",
           CanonicalValue("manyforge.program_failure_observation.v1")},
          {"status", CanonicalValue(std::move(status))},
          {"changed", CanonicalValue(changed)}});
    };
    if (state != "active" && program_failure_adapter_.source_epoch != epoch)
      return response("stale", false);
    if (program_failure_adapter_.source_epoch == epoch &&
        (control_revision < program_failure_control_revision_ ||
         (run == program_failure_run_id_ &&
          run_revision < program_failure_run_revision_)))
      return response("stale", false);
    const auto before = alarm_owner_.MotionAttentionProjection()->generation;
    if (program_failure_adapter_.source_epoch != epoch) {
      RegisterAdapterEpochLocked(program_failure_adapter_, epoch);
      program_failure_control_revision_ = 0U;
      program_failure_run_revision_ = 0U;
      program_failure_run_id_.clear();
    }
    Require(control_revision != program_failure_control_revision_ ||
                program_failure_run_id_.empty() ||
                program_failure_run_id_ == run,
            code, "one accepted Program control revision names different runs");
    if (state == "unknown") {
      program_failure_control_revision_ = control_revision;
      program_failure_run_revision_ = run_revision;
      program_failure_run_id_ = run;
      alarm_owner_.MarkSourceQualityUnknown(program_failure_adapter_.source_id,
                                            epoch);
      return response("observed",
                      alarm_owner_.MotionAttentionProjection()->generation !=
                          before);
    }
    const bool active = state == "active";
    const auto point = alarm_owner_.ConditionPoint(definition, subject);
    const auto refused_key = std::string(definition) + '\n' + tracking_id;
    const auto refused = refused_operation_conditions_.find(refused_key);
    const auto origin_control_revision =
        point && point->record
            ? RequiredCounter(point->record->correlations,
                              "condition_control_revision", 1U)
        : refused != refused_operation_conditions_.end()
            ? RequiredCounter(refused->second.update.correlations,
                              "condition_control_revision", 1U)
            : control_revision;
    auto correlation_fields = correlations.as_object();
    correlation_fields.emplace_back(
        "condition_control_revision",
        Number(origin_control_revision, "condition_control_revision"));
    correlations = CanonicalValue(std::move(correlation_fields));
    if (!active &&
        ((point && point->record && IsActive(point->record->lifecycle_state)) ||
         refused != refused_operation_conditions_.end()))
      Require(control_revision > origin_control_revision, code,
              "Program resolution must follow its accepted failure condition");
    bool duplicate = false;
    if (point) {
      if (!point->record) {
        // A resolved occurrence cannot be resurrected by a delayed active fact.
        if (active && point->activation_count != 0U)
          return response("stale", false);
        duplicate = !active;
      } else {
        const auto &record = *point->record;
        Require(record.details == details &&
                    record.correlations == correlations,
                code,
                "one Program failure occurrence names conflicting provenance");
        duplicate = IsActive(record.lifecycle_state) == active;
        if (active && !IsActive(record.lifecycle_state))
          return response("stale", false);
      }
    }
    if (refused != refused_operation_conditions_.end()) {
      Require(refused->second.update.details == details &&
                  refused->second.update.correlations == correlations,
              code,
              "one refused Program failure occurrence names conflicting "
              "provenance");
      duplicate = active;
      if (!active)
        ForgetRefusedOperationConditionLocked(definition, tracking_id);
    }
    if (!active && !point)
      duplicate = true; // Never create a condition merely to clear it.
    if (!duplicate) {
      Require(program_failure_adapter_.source_sequence < kMaximumSafeInteger,
              "headless_adapter_sequence_exhausted",
              "Program condition sequence is exhausted");
      AlarmConditionUpdate update{std::string(definition),
                                  subject,
                                  active,
                                  program_failure_adapter_.source_id,
                                  epoch,
                                  ++program_failure_adapter_.source_sequence,
                                  clocks_.monotonic_time_ns(),
                                  clocks_.wall_time_text(),
                                  AlarmConditionQuality::kGood,
                                  CanonicalValue(CanonicalValue::Object{}),
                                  details,
                                  {},
                                  correlations};
      if (const auto mutation = SubmitOperationConditionLocked(
              std::move(update), OperationConditionKind::kProgramFailure,
              tracking_id))
        static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
    }
    program_failure_control_revision_ = control_revision;
    program_failure_run_revision_ = run_revision;
    program_failure_run_id_ = run;
    alarm_owner_.CompleteSourceSnapshot(program_failure_adapter_.source_id,
                                        epoch, AlarmConditionQuality::kGood);
    SynchronizeHistorianHealthLocked();
    return response(duplicate ? "duplicate" : "observed",
                    alarm_owner_.MotionAttentionProjection()->generation !=
                        before);
  } catch (const OperatorAlarmError &) {
    // An invalid current source is UNKNOWN, never a cause-clear observation.
    std::lock_guard lock(mutex_);
    if (!program_failure_adapter_.source_epoch.empty())
      alarm_owner_.MarkSourceQualityUnknown(
          program_failure_adapter_.source_id,
          program_failure_adapter_.source_epoch);
    throw;
  }
}
CanonicalValue HeadlessApiCore::ObserveRuntimeFaultConditions(
    std::string source_owner_epoch, CanonicalValue current_runtime_faults) {
  constexpr auto code = "headless_runtime_fault_invalid";
  // execution_host_config.hpp / execution_host_commands.cpp Text contract.
  constexpr std::size_t native_identity_bytes = 2048U;
  const auto text = [&](const CanonicalValue &value, const std::string_view key,
                        const std::size_t maximum) {
    const auto result = RequiredString(value, key);
    Require(!result.empty() && result.size() <= maximum &&
                content_identity::is_utf8_nfc(result),
            code, "runtime fault text is invalid");
    return result;
  };
  const auto closed = [&](const CanonicalValue &value,
                          std::initializer_list<std::string_view> keys) {
    Require(value.is_object() && value.as_object().size() == keys.size(), code,
            "runtime fault has an invalid closed shape");
    for (const auto key : keys)
      Require(value.find_object_member(key) != nullptr, code,
              "runtime fault field is missing");
  };
  Require(!source_owner_epoch.empty() &&
              source_owner_epoch.size() <= native_identity_bytes &&
              content_identity::is_utf8_nfc(source_owner_epoch),
          code, "runtime fault source owner is invalid");
  closed(current_runtime_faults, {"operation_journal_write_fault",
                                  "program_runner_loss", "kernel_internal_fault"});
  Require(content_identity::canonical_json(current_runtime_faults).size() <=
              kMaximumRuntimeFaultSnapshotBytes,
          code, "runtime fault snapshot exceeds its bound");
  // Validate the complete snapshot before submitting either condition.
  for (const auto kind : {"operation_journal_write_fault",
                          "program_runner_loss", "kernel_internal_fault"}) {
    const auto &fact = RequiredMember(current_runtime_faults, kind);
    if (fact.is_null())
      continue;
    Require(text(fact, "request_owner_epoch", native_identity_bytes) ==
                source_owner_epoch,
            code, "runtime fault names another Kernel owner");
    if (std::string_view(kind) == "operation_journal_write_fault") {
      closed(fact, {"request_owner_epoch", "write_kind", "error",
                    "operation_id", "operation_revision", "request_id",
                    "program_run_id", "execution_scope"});
      static_cast<void>(text(fact, "write_kind", 64U));
      const auto &error = RequiredMember(fact, "error");
      closed(error, {"code", "message", "details"});
      static_cast<void>(text(error, "code", 256U));
      const auto message = RequiredString(error, "message");
      Require(message.size() <= 1024U && content_identity::is_utf8_nfc(message),
              code, "runtime fault error message is invalid");
      Require(RequiredMember(error, "details").is_object() &&
                  RequiredMember(error, "details").as_object().empty(),
              code, "runtime fault error details must be empty");
      for (const auto key : {"operation_id", "request_id", "program_run_id"})
        if (!RequiredMember(fact, key).is_null())
          static_cast<void>(text(fact, key, native_identity_bytes));
      if (!RequiredMember(fact, "operation_revision").is_null())
        static_cast<void>(RequiredCounter(fact, "operation_revision", 1U));
      const auto &scope = RequiredMember(fact, "execution_scope");
      if (!scope.is_null()) {
        closed(scope, {"node_instance_id", "node_attempt_id"});
        static_cast<void>(text(scope, "node_instance_id", 128U));
        static_cast<void>(text(scope, "node_attempt_id", 512U));
      }
    } else if (std::string_view(kind) == "kernel_internal_fault") {
      closed(fact, {"request_owner_epoch", "reason_code", "stop_accepted",
                    "stop_refusal_code", "later_count"});
      static_cast<void>(text(fact, "reason_code", 128U));
      // A refusal code exactly when the containment Full Stop was refused.
      const bool stop_accepted = RequiredBool(fact, "stop_accepted");
      Require(RequiredMember(fact, "stop_refusal_code").is_null() == stop_accepted,
              code, "internal fault containment outcome is invalid");
      if (!stop_accepted)
        static_cast<void>(text(fact, "stop_refusal_code", 128U));
      static_cast<void>(RequiredCounter(fact, "later_count", 0U));
    } else {
      closed(fact, {"request_owner_epoch", "manager_owner_epoch",
                    "manager_runtime_epoch", "established_cycle",
                    "failure_predicate", "message"});
      static_cast<void>(text(fact, "manager_owner_epoch", 128U));
      static_cast<void>(text(fact, "manager_runtime_epoch", 128U));
      const auto predicate = text(fact, "failure_predicate", 128U);
      Require(predicate == "program_runner_liveness_probe" ||
                  predicate == "program_runner_liveness_renewal" ||
                  predicate == "program_runner_reconciliation_renewal" ||
                  predicate == "program_runner_publication_fault",
              code, "runtime fault is not a Runner contact failure");
      const auto message = RequiredString(fact, "message");
      Require(message.size() <= 1024U && content_identity::is_utf8_nfc(message),
              code, "Runner contact error message is invalid");
      const auto &cycle = RequiredMember(fact, "established_cycle");
      closed(cycle, {"program_runner_owner_epoch", "cycle_generation", "state",
                     "program_run_id", "cycle_claim_generation",
                     "terminal_or_settlement_reference", "accepted_reset"});
      static_cast<void>(text(cycle, "program_runner_owner_epoch", 128U));
      static_cast<void>(text(cycle, "program_run_id", 128U));
      static_cast<void>(RequiredCounter(cycle, "cycle_generation", 1U));
      static_cast<void>(RequiredCounter(cycle, "cycle_claim_generation", 1U));
      const auto state = RequiredString(cycle, "state");
      Require(
          (state == "starting" || state == "running" || state == "stopping") &&
              RequiredMember(cycle, "accepted_reset").is_null() &&
              RequiredMember(cycle, "terminal_or_settlement_reference")
                  .is_null(),
          code, "Runner loss requires an established active Program run");
    }
  }
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  bool changed = false;
  for (const auto kind : {"operation_journal_write_fault",
                          "program_runner_loss", "kernel_internal_fault"}) {
    const auto &fact = RequiredMember(current_runtime_faults, kind);
    if (fact.is_null() || active_runtime_fault_kinds_.contains(kind))
      continue;
    RegisterAdapterEpochLocked(runtime_fault_adapter_,
                               config_.owner_epoch + ".runtime-faults");
    // Keep exact attribution while respecting the existing 32 KiB details
    // and correlations limits, including six-byte JSON control escapes.
    CanonicalValue::Object details;
    CanonicalValue::Object correlations{
        {"source_owner_epoch", CanonicalValue(source_owner_epoch)}};
    for (const auto &[key, value] : fact.as_object()) {
      if (key == "request_owner_epoch")
        continue; // Indexed above, once.
      if (std::string_view(kind) == "operation_journal_write_fault" &&
          (key == "operation_id" || key == "execution_scope" ||
           key == "write_kind"))
        correlations.emplace_back(key, value);
      else
        details.emplace_back(key, value);
    }
    const auto mutation = SubmitAdapterUpdateLocked(
        runtime_fault_adapter_, "manyforge.runtime." + std::string(kind),
        AlarmSubject{"cell", config_.cell_id, {}}, true,
        CanonicalValue(std::move(details)),
        CanonicalValue(std::move(correlations)));
    static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
    active_runtime_fault_kinds_.insert(kind);
    changed = true;
    alarm_owner_.CompleteSourceSnapshot(runtime_fault_adapter_.source_id,
                                        runtime_fault_adapter_.source_epoch,
                                        AlarmConditionQuality::kGood);
  }
  return CanonicalValue(
      CanonicalValue::Object{{"status", CanonicalValue("observed")},
                             {"changed", CanonicalValue(changed)}});
}

CanonicalValue HeadlessApiCore::AppendWorkbenchPreviousSessionReport(CanonicalValue report) {
  auto event = PreviousSessionReportEvent(report);
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    store = history_store_;
  }
  // FULL synchronous commit precedes this receipt. Duplicate status also
  // verifies the persisted digest, so changed content cannot be finalized.
  const auto status = store->Append(event);
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", CanonicalValue(event.event_id)},
      {"session_id", RequiredMember(report, "session_id")},
      {"status", CanonicalValue(status == IEventHistoryStore::AppendStatus::kStored
                                   ? "stored" : "duplicate")},
  });
}

CanonicalValue HeadlessApiCore::ObserveWorkbenchPreviousSessionReport(CanonicalValue report) {
  // Reuse the immutable report validator and durable duplicate proof. This
  // registration never opens the archived runtime stores.
  const auto event = PreviousSessionReportEvent(report);
  const auto identity = RequiredString(report, "session_id");
  const auto incident_id = "workbench.previous-session." + identity;
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    Require(!workbench_previous_session_id_ || *workbench_previous_session_id_ == identity,
            "headless_previous_session_report_invalid",
            "only the current predecessor may be registered");
    store = history_store_;
  }
  static_cast<void>(store->Append(event));
  HistorianQuery query;
  query.components = {"manyforge.alarm"};
  query.event_types = {"operator_alarm_acknowledged"};
  query.correlations = {{"incident_id", incident_id}};
  query.limit = 1U;
  // A read failure propagates; absence or an unreadable result is never an ACK.
  const auto page = store->Query(query);
  const bool acknowledged = !page.events.empty() &&
      RequiredString(page.events.front().event.payload, "definition_id") ==
          "manyforge.runtime.previous_session_unclean" &&
      RequiredString(RequiredMember(page.events.front().event.payload, "subject"),
                     "subject_kind") == "workbench_session" &&
      RequiredString(RequiredMember(page.events.front().event.payload, "subject"),
                     "subject_id") == identity &&
      RequiredMember(RequiredMember(page.events.front().event.payload, "subject"),
                     "resource_ids").is_array() &&
      RequiredMember(RequiredMember(page.events.front().event.payload, "subject"),
                     "resource_ids").as_array().empty();
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  Require(!workbench_previous_session_id_ || *workbench_previous_session_id_ == identity,
          "headless_previous_session_report_invalid",
          "only the current predecessor may be registered");
  const bool visible = RequiredString(report, "shutdown") != "clean" ||
                       RequiredString(report, "effects") == "unresolved";
  if (!workbench_previous_session_id_ && visible && !acknowledged) {
    RegisterAdapterEpochLocked(workbench_session_adapter_,
                               config_.owner_epoch + ".workbench-session");
    const AlarmSubject subject{"workbench_session", identity, {}};
    const CanonicalValue details(CanonicalValue::Object{
        {"previous_session", report},
        {"failure_time", CanonicalValue(nullptr)},
        {"failure_time_status", CanonicalValue("unavailable")},
    });
    const CanonicalValue correlations(CanonicalValue::Object{
        {"incident_id", CanonicalValue(incident_id)},
    });
    // This is a historical occurrence, not a current execution condition.
    // Return leaves the ordinary acknowledgement obligation until seen.
    for (const bool active : {true, false}) {
      const auto mutation = SubmitAdapterUpdateLocked(
          workbench_session_adapter_, "manyforge.runtime.previous_session_unclean",
          subject, active, details, correlations);
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
    }
    alarm_owner_.CompleteSourceSnapshot(workbench_session_adapter_.source_id,
                                        workbench_session_adapter_.source_epoch,
                                        AlarmConditionQuality::kGood);
  }
  workbench_previous_session_id_ = identity;
  return CanonicalValue(CanonicalValue::Object{
      {"session_id", CanonicalValue(identity)},
      {"status", CanonicalValue(acknowledged ? "acknowledged" : visible ? "registered" : "quiet")},
  });
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

const CanonicalValue &HeadlessApiCore::ActiveAppliedRecordLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  Require(SectionFresh(manager_section), "program_applied_selection_unavailable",
          "Applied Program requires a fresh Manager owner");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  Require(manager_payload.is_object(), "program_applied_selection_unavailable",
          "Applied Program requires an active Manager selection");
  const auto &active = RequiredMember(manager_payload, "active");
  Require(RequiredBool(manager_payload, "manager_ready") &&
              RequiredBool(RequiredMember(manager_payload, "runtime"),
                           "converged"),
          "program_applied_selection_not_active",
          "Applied Program requires one converged active configuration");
  const auto &transaction = RequiredMember(
      RequiredMember(active, "publication"), "transaction");
  Require(RequiredString(transaction, "kind") == "named_apply",
          "program_applied_selection_not_found",
          "the active configuration was not installed from a Scenario");
  const auto apply_id = RequiredString(transaction, "apply_id");
  const auto apply_generation =
      RequiredCounter(transaction, "apply_generation", 1U);
  const auto &records = RequiredArray(RequiredMember(manager_payload, "apply"),
                                      "records");
  const CanonicalValue *record = nullptr;
  for (const auto &candidate : records) {
    if (RequiredString(candidate, "apply_id") == apply_id &&
        RequiredCounter(candidate, "apply_generation", 1U) ==
            apply_generation) {
      Require(record == nullptr, "program_applied_selection_invalid",
              "Manager published duplicate active Apply records");
      record = &candidate;
    }
  }
  Require(record != nullptr && RequiredString(*record, "terminal_state") ==
                                   "succeeded",
          "program_applied_selection_not_active",
          "the active Apply has no successful terminal evidence");
  const auto &prepared = RequiredMember(*record, "prepared_activation");
  // Development cutover: an active Apply recorded before the removal of
  // launch-time parameter overrides is refused, never reinterpreted.
  Require(RequiredString(prepared, "schema_version") ==
              deployment::kPreparedActivationInputSchemaVersion,
          "program_applied_selection_invalid",
          "the active Apply used an unsupported prepared activation version; "
          "replace the stopped workbench with fresh runtime state");
  Require(RequiredString(prepared, "retained_bundle_id") ==
                  RequiredString(active, "retained_bundle_id") &&
              RequiredString(prepared, "graph_revision") ==
                  RequiredString(active, "graph_revision"),
          "program_applied_selection_invalid",
          "active Manager publication and prepared activation disagree");
  return *record;
}

namespace {

/// The typed Program outcome of the active prepared activation (H1a): the
/// binding id when bound, or the decoded non-executable result.
using AppliedProgramOutcome = deployment::PreparedActivationProgramOutcomeDTO;

AppliedProgramOutcome
ReadAppliedProgramOutcome(const CanonicalValue &prepared) {
  auto outcome =
      deployment::parse_prepared_activation_program_outcome(prepared);
  if (!outcome.ok())
    CoreFail(outcome.error().code, outcome.error().message);
  return std::move(outcome.value());
}

/// The exact prepared origin is retained on every committed Apply, including
/// a sealed empty Apply which carries no Scenario or Program.
const CanonicalValue &ReadPreparedOrigin(const CanonicalValue &prepared) {
  const auto &origin = RequiredMember(prepared, "origin");
  try {
    const auto typed = deployment::parse_prepared_activation_origin(origin);
    if (const auto *sealed =
            std::get_if<deployment::PreparedSealedCanonicalEmptyOrigin>(&typed)) {
      Require(sealed->deployment_artifact_id.value() ==
                      RequiredString(prepared, "deployment_artifact_id") &&
                  sealed->deployment_revision.value() ==
                      RequiredString(prepared, "deployment_revision") &&
                  sealed->retained_bundle_id.value() ==
                      RequiredString(prepared, "retained_bundle_id") &&
                  sealed->graph_revision.value() ==
                      RequiredString(prepared, "graph_revision") &&
                  RequiredString(RequiredMember(prepared, "program_outcome"), "kind") ==
                      "no_program" &&
                  RequiredMember(prepared, "program_id").is_null() &&
                  RequiredMember(prepared, "program_revision").is_null(),
              "program_applied_selection_invalid", "invalid sealed empty origin");
    }
  } catch (const deployment::PreparedActivationInputError &error) {
    CoreFail("program_applied_selection_invalid",
             std::string("invalid prepared origin: ") + error.what());
  }
  return origin;
}

/// One operator-readable sentence naming every unresolved reference.
std::string NonExecutableMessage(
    const deployment::ProgramNonExecutableOutcomeDTO &outcome) {
  std::string message =
      "Applied Program is not executable in the active Deployment:";
  for (const auto &row : outcome.unresolved_roles) {
    message += row.target_role_id.has_value()
                   ? " target role '" + row.target_role_id->value() +
                         "' (component '" +
                         row.controllable_component_id.value() + "', target '" +
                         row.command_target_id->value() + "')"
                   : " component role '" + row.component_role_id.value() +
                         "' (component '" +
                         row.controllable_component_id.value() + "')";
    message += " is " + std::string(deployment::to_string(row.reason)) + ";";
  }
  message.pop_back();
  return message;
}

// Simulator entity mapping of the active bundle: its eligible
// simulation-entity object links (expanded pose-source rows) and their one
// cell frame; null when the bundle has none.
CanonicalValue SimulationScene(const deployment::RetainedBundle &bundle) {
  using Object = core::JsonValueDTO::Object;
  using Array = core::JsonValueDTO::Array;
  const auto sources =
      deployment::ResolveRetainedPoseSources(bundle.value().cell_graph);
  if (!sources.ok())
    CoreFail("program_applied_selection_invalid",
             "active pose-source configuration is invalid");
  std::optional<std::string> cell_frame_id;
  std::map<std::string, std::string> entities;  // scene_resource_id -> entity
  const auto text = [](const Object &row, const char *key) {
    return std::get<std::string>(row.at(key).value);
  };
  for (const auto &source : sources.value()) {
    const auto &row = std::get<Object>(source.row.value);
    for (const auto &item :
         std::get<Array>(row.at("simulation_entities").value)) {
      const auto &binding = std::get<Object>(item.value);
      if (!std::get<bool>(binding.at("eligible").value))
        continue;
      const auto frame = text(binding, "source_coordinate_frame_id");
      if (cell_frame_id && *cell_frame_id != frame)
        CoreFail("simulation_cell_frame_ambiguous",
                 "simulation object links use more than one cell frame (" +
                     *cell_frame_id + ", " + frame + ")");
      cell_frame_id = frame;
      entities.emplace(text(binding, "object_id"), text(binding, "entity"));
    }
  }
  if (!cell_frame_id)
    return CanonicalValue(nullptr);
  CanonicalValue::Array scene_entities;
  for (const auto &[scene_resource_id, entity] : entities)
    scene_entities.emplace_back(CanonicalValue::Object{
        {"entity", CanonicalValue(entity)},
        {"scene_resource_id", CanonicalValue(scene_resource_id)},
    });
  return CanonicalValue(CanonicalValue::Object{
      {"cell_frame_id", CanonicalValue(*cell_frame_id)},
      {"scene_entities", CanonicalValue(std::move(scene_entities))},
  });
}

} // namespace

// Each assistant model alias names its component through
// `external_resource_alias`.
CanonicalValue AssistantAiModels(const core::CellGraphDTO &graph) {
  const auto models = deployment::ResolveRetainedAiModels(graph);
  if (!models.ok())
    CoreFail("program_applied_selection_invalid",
             "active AI model configuration is invalid");
  CanonicalValue::Array rows;
  for (const auto &model : models.value())
    rows.emplace_back(CanonicalValue::Object{
        {"api_key_env", model.api_key_env.has_value()
                            ? CanonicalValue(*model.api_key_env)
                            : CanonicalValue(nullptr)},
        {"base_url", CanonicalValue(model.base_url)},
        {"controllable_component_id",
         CanonicalValue(model.controllable_component_id.value())},
        {"model", CanonicalValue(model.model)},
    });
  return CanonicalValue(std::move(rows));
}

CanonicalValue HeadlessApiCore::ActiveProgramSelectionLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  // Freshness, convergence and the successful Apply are verified first.
  const auto &record = ActiveAppliedRecordLocked(runtime);
  const auto &manager_section = RequiredSection(runtime.root, "manager");
  const auto &manager_payload = RequiredMember(manager_section, "payload");
  const auto &active = RequiredMember(manager_payload, "active");
  const auto apply_generation = RequiredCounter(record, "apply_generation", 1U);
  const auto &prepared = RequiredMember(record, "prepared_activation");
  // A Program that no longer binds in the active Deployment is preserved as
  // authored and refused by name for every consumer (Execute, runner, API);
  // it is never replaced by another binding (H1a).
  const auto outcome = ReadAppliedProgramOutcome(prepared);
  if (outcome.non_executable.has_value()) {
    CoreFail("program_applied_non_executable",
             NonExecutableMessage(*outcome.non_executable));
  }
  if (!outcome.program_binding_id.has_value()) {
    CoreFail("program_not_selected", "No Program is selected; create a Program and Apply it before Execute.");
  }
  const auto &origin = ReadPreparedOrigin(prepared);
  Require(RequiredString(origin, "kind") == "scenario",
          "program_applied_selection_invalid",
          "a bound Program requires an actual Scenario origin");
  const auto active_bundle_id = RequiredString(active, "retained_bundle_id");
  const auto active_graph_revision = RequiredString(active, "graph_revision");
  const auto bundle = retained_bundle_source_.get_verified(
      core::RetainedBundleId(active_bundle_id));
  Require(bundle != nullptr, "program_applied_selection_invalid",
          "active retained bundle is unavailable");
  auto model_map = LoadActiveProgramModelMap(*bundle, retained_model_map_source_);
  const auto program_id = core::ProgramId(RequiredString(prepared, "program_id"));
  const auto program_revision =
      core::ProgramRevision(RequiredString(prepared, "program_revision"));
  const auto binding_id = *outcome.program_binding_id;
  const auto program =
      program_repository_.get_verified(program_id, program_revision, *bundle);
  const auto binding =
      binding_repository_.get_verified(binding_id, *bundle, model_map);
  Require(program != nullptr, "program_applied_selection_invalid",
          "active Program is unavailable");
  Require(binding != nullptr, "program_applied_selection_invalid",
          "active Program binding is unavailable");
  Require(binding->value().program_id == program_id &&
              binding->value().program_revision == program_revision,
          "program_applied_selection_invalid",
          "active Program and binding are divergent");

  // The retained assistant member plus `ai_models`: the AI model components
  // its Program model calls use.
  CanonicalValue assistant_projection(nullptr);
  if (bundle->value().assistant.has_value()) {
    auto ai_models = AssistantAiModels(bundle->value().cell_graph);
    CanonicalValue::Object members;
    try {
      members = deployment::parse_strict_json_model_yaml(
                    bundle->value().assistant->canonical_json)
                    .root()
                    .as_object();
    } catch (const std::exception &error) {
      CoreFail("program_applied_selection_invalid",
               "active assistant projection is invalid: " +
                   std::string(error.what()));
    }
    members.emplace_back("ai_models", std::move(ai_models));
    assistant_projection = CanonicalValue(std::move(members));
  }

  const auto environment = NodeEnvironmentProjectionLocked(runtime);
  const auto &package_environment =
      RequiredMember(environment, "package_environment");
  Require(RequiredString(package_environment, "environment_digest") ==
              RequiredString(prepared, "node_environment_digest") &&
              RequiredString(environment, "retained_bundle_id") ==
                  active_bundle_id &&
              RequiredString(environment, "graph_revision") ==
                  active_graph_revision,
          "program_applied_selection_invalid",
          "active Program and node environment disagree");

  const auto &logical_scene_section =
      RequiredSection(runtime.root, "logical_scene");
  const auto &scene_runtime_section =
      RequiredSection(runtime.root, "scene_runtime");
  const auto owner_identity = [](const std::string_view role,
                                 const CanonicalValue &section) {
    return CanonicalValue(CanonicalValue::Object{
        {"owner_epoch", RequiredMember(section, "source_owner_epoch")},
        {"owner_id", RequiredMember(section, "source_owner_id")},
        {"owner_role", CanonicalValue(std::string(role))},
    });
  };

  return CanonicalValue(CanonicalValue::Object{
      {"assistant_mode", RequiredMember(prepared, "assistant_mode")},
      {"assistant_projection", std::move(assistant_projection)},
      {"baseline_program_binding_id", CanonicalValue(binding_id.value())},
      {"deployment_artifact_id",
       RequiredMember(prepared, "deployment_artifact_id")},
      {"deployment_revision", RequiredMember(prepared, "deployment_revision")},
      {"graph_revision", CanonicalValue(active_graph_revision)},
      {"node_environment_digest",
       RequiredMember(prepared, "node_environment_digest")},
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
      {"preparation_digest",
       RequiredMember(prepared, "prepared_activation_id")},
      {"program", program->normalized_program_root()},
      {"program_binding", binding->normalized_root()},
      {"program_binding_id", CanonicalValue(binding_id.value())},
      {"program_id", CanonicalValue(program_id.value())},
      {"program_revision", CanonicalValue(program_revision.value())},
      {"retained_bundle_id", CanonicalValue(active_bundle_id)},
      {"source_document_digest",
       RequiredMember(origin, "source_document_digest")},
      {"scenario_id", RequiredMember(origin, "scenario_id")},
      {"project_path", RequiredMember(origin, "project_path")},
      {"schema_version",
       CanonicalValue(std::string(kAppliedProgramSelectionSchemaVersion))},
      {"simulation_scene", SimulationScene(*bundle)},
      {"source", CanonicalValue(CanonicalValue::Object{
                     {"record_digest", RequiredMember(record, "intent_digest")},
                     {"semantic_content_digest",
                      RequiredMember(prepared, "prepared_activation_id")},
                     {"source_generation",
                      CanonicalValue(static_cast<double>(apply_generation))},
                     {"source_owner_epoch",
                      RequiredMember(manager_section, "source_owner_epoch")},
                     {"source_owner_id",
                      RequiredMember(manager_section, "source_owner_id")},
                 })},
  });
}

CanonicalValue HeadlessApiCore::AppliedProgramSelection() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return ActiveProgramSelectionLocked(RefreshLocked());
}

CanonicalValue HeadlessApiCore::AppliedProgramStatus() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  const auto &record = ActiveAppliedRecordLocked(runtime);
  const auto &prepared = RequiredMember(record, "prepared_activation");
  const auto outcome = ReadAppliedProgramOutcome(prepared);
  const auto &origin = ReadPreparedOrigin(prepared);
  // Derived from the durable active Apply on every read; nothing here is a
  // stored warning that could outlive the facts it describes.
  CanonicalValue program(nullptr);
  CanonicalValue non_executable(nullptr);
  if (outcome.non_executable.has_value()) {
    const auto bundle = retained_bundle_source_.get_verified(
        core::RetainedBundleId(RequiredString(prepared, "retained_bundle_id")));
    Require(bundle != nullptr, "program_applied_selection_invalid",
            "active retained bundle is unavailable");
    const auto authored = program_repository_.get_verified(
        core::ProgramId(RequiredString(prepared, "program_id")),
        core::ProgramRevision(RequiredString(prepared, "program_revision")),
        *bundle);
    Require(authored != nullptr, "program_applied_selection_invalid",
            "active Program is unavailable");
    program = authored->normalized_program_root();
    non_executable = content_identity::parse_canonical_json(
        deployment::EncodeProgramNonExecutableOutcome(*outcome.non_executable));
  }
  return CanonicalValue(CanonicalValue::Object{
      {"deployment_artifact_id",
       RequiredMember(prepared, "deployment_artifact_id")},
      {"deployment_revision", RequiredMember(prepared, "deployment_revision")},
      {"executable", CanonicalValue(outcome.program_binding_id.has_value())},
      {"graph_revision", RequiredMember(prepared, "graph_revision")},
      {"message", outcome.non_executable.has_value()
                      ? CanonicalValue(NonExecutableMessage(*outcome.non_executable))
                      : (!outcome.program_binding_id.has_value()
                             ? CanonicalValue("No Program is selected")
                             : CanonicalValue(nullptr))},
      {"non_executable", std::move(non_executable)},
      {"program", std::move(program)},
      {"program_binding_id",
       outcome.program_binding_id.has_value()
           ? CanonicalValue(outcome.program_binding_id->value())
           : CanonicalValue(nullptr)},
      {"program_id", RequiredMember(prepared, "program_id")},
      {"program_revision", RequiredMember(prepared, "program_revision")},
      {"record_digest", RequiredMember(record, "intent_digest")},
      {"retained_bundle_id", RequiredMember(prepared, "retained_bundle_id")},
      {"origin", origin},
      {"schema_version",
       CanonicalValue(std::string(kAppliedProgramStatusSchemaVersion))},
  });
}

CanonicalValue HeadlessApiCore::CommittedScenarioActivation() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  // One Manager source snapshot answers both which committed activation is
  // current and which committed Applies its publication history holds.
  const auto runtime = RefreshLocked();
  const auto &record = ActiveAppliedRecordLocked(runtime);
  const auto &manager_payload =
      RequiredMember(RequiredSection(runtime.root, "manager"), "payload");
  const auto publication_generation = RequiredCounter(
      RequiredMember(RequiredMember(manager_payload, "active"), "publication"),
      "publication_generation", 2U);
  // Every committed Apply, including sealed empty origins, remains in the
  // complete publication inventory. Canonical origin bytes keep ordering exact.
  using CommittedApply = std::tuple<std::string, std::string, std::string,
                                    std::uint64_t, std::string>;
  const auto committed_apply = [](const CanonicalValue &apply_record) {
    const auto initialization =
        RequiredString(apply_record, "scene_initialization");
    const auto origin =
        ReadPreparedOrigin(RequiredMember(apply_record, "prepared_activation"));
    Require((initialization == "preserve" || initialization == "initialize") &&
                (initialization == "preserve" ||
                 RequiredString(origin, "kind") == "scenario"),
            "program_activation_invalid",
            "Manager Scene initialization purpose is invalid");
    return CommittedApply(RequiredString(apply_record, "intent_digest"),
                          content_identity::canonical_json(origin),
                          RequiredString(apply_record, "apply_id"),
                          RequiredCounter(apply_record, "apply_generation", 1U),
                          initialization);
  };
  std::vector<CommittedApply> committed;
  for (const auto &candidate :
       RequiredArray(RequiredMember(manager_payload, "apply"), "records")) {
    if (RequiredBool(candidate, "committed")) {
      committed.push_back(committed_apply(candidate));
    }
  }
  std::sort(committed.begin(), committed.end());
  // Publication 1 is the canonical-empty bootstrap and every later one is
  // exactly one committed named Apply, so the inventory must account for
  // every publication; otherwise no consumer can prove it saw them all.
  Require(committed.size() + 1U == publication_generation &&
              std::adjacent_find(committed.begin(), committed.end(),
                                 [](const auto &left, const auto &right) {
                                   return std::get<0>(left) ==
                                          std::get<0>(right);
                                 }) == committed.end(),
          "program_activation_history_incomplete",
          "Manager's committed Applies do not account for every publication");
  CanonicalValue::Array committed_applies;
  committed_applies.reserve(committed.size());
  for (const auto &[record_digest, origin_json, apply_id, apply_generation,
                    initialization] : committed) {
    committed_applies.emplace_back(CanonicalValue(CanonicalValue::Object{
        {"apply_generation", Number(apply_generation, "apply_generation")},
        {"apply_id", CanonicalValue(apply_id)},
        {"record_digest", CanonicalValue(record_digest)},
        {"origin", content_identity::parse_canonical_json(origin_json)},
        {"scene_initialization", CanonicalValue(initialization)},
    }));
  }
  const auto &prepared = RequiredMember(record, "prepared_activation");
  const auto outcome = ReadAppliedProgramOutcome(prepared);
  CanonicalValue program(nullptr);
  if (outcome.program_binding_id.has_value() ||
      outcome.non_executable.has_value()) {
    // Bound or not, the committed declarations are the authored Program's.
    const auto bundle = retained_bundle_source_.get_verified(
        core::RetainedBundleId(RequiredString(prepared, "retained_bundle_id")));
    Require(bundle != nullptr, "program_applied_selection_invalid",
            "active retained bundle is unavailable");
    const auto authored = program_repository_.get_verified(
        core::ProgramId(RequiredString(prepared, "program_id")),
        core::ProgramRevision(RequiredString(prepared, "program_revision")),
        *bundle);
    Require(authored != nullptr, "program_applied_selection_invalid",
            "active Program is unavailable");
    program = authored->normalized_program_root();
  }
  const auto active = committed_apply(record);
  Require(std::binary_search(committed.begin(), committed.end(), active),
          "program_activation_history_incomplete",
          "the active Apply is not among Manager's committed Applies");
  const auto &[record_digest, origin_json, apply_id, apply_generation,
               initialization] = active;
  return CanonicalValue(CanonicalValue::Object{
      {"apply_generation", Number(apply_generation, "apply_generation")},
      {"apply_id", CanonicalValue(apply_id)},
      {"committed_applies", CanonicalValue(std::move(committed_applies))},
      {"program", std::move(program)},
      {"publication_generation",
       Number(publication_generation, "publication_generation")},
      {"record_digest", CanonicalValue(record_digest)},
      {"origin", content_identity::parse_canonical_json(origin_json)},
      {"scene_initialization", CanonicalValue(initialization)},
      {"schema_version",
       CanonicalValue(std::string(kCommittedScenarioActivationSchemaVersion))},
  });
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

CanonicalValue HeadlessApiCore::OperatorAlarmMotionAttention() {
  // Deliberately outside `mutex_`: the publisher's period must not wait on a
  // source refresh, a bundle verification or any other long core operation.
  // The owner publishes an immutable projection at every generation advance;
  // this read only copies that pointer.
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto overdue = [this]() {
    const auto refreshed_at_ns =
        alarm_sources_refreshed_at_ns_.load(std::memory_order_acquire);
    return refreshed_at_ns < 0 || clocks_.monotonic_time_ns() -
                                          refreshed_at_ns >
                                      kAlarmSourceCurrencyBoundNs;
  };
  if (overdue()) {
    // The facts deciding reconciliation may be older than C. Name that as
    // owner health: the owner advances its generation and the projection
    // reports not-reconciled until the next completed refresh clears it.
    // `RefreshLocked` stores the refresh time before it reconciles inside
    // the owner, so re-deciding there never re-marks a completed refresh.
    alarm_owner_.MarkHealthUnavailableIf(
        std::string(kAlarmSourceRefreshOverdueHealthCode),
        "alarm sources were not refreshed within the currency bound", overdue);
  }
  return OperatorAlarmMotionAttentionValue(
      *alarm_owner_.MotionAttentionProjection());
}

CanonicalValue HeadlessApiCore::RefreshAlarmSources() {
  const auto due = [this]() {
    const auto refreshed_at_ns =
        alarm_sources_refreshed_at_ns_.load(std::memory_order_acquire);
    return refreshed_at_ns < 0 || clocks_.monotonic_time_ns() -
                                          refreshed_at_ns >=
                                      kAlarmSourceRefreshPeriodNs;
  };
  bool refreshed = false;
  // A refresh by any reader within the period already made the sources
  // current; only a due refresh takes the coordination lock.
  if (due()) {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    if (due()) {
      static_cast<void>(RefreshLocked());
      refreshed = true;
    }
  }
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return CanonicalValue(CanonicalValue::Object{
      {"component_lifecycle_observation_pending",
       CanonicalValue(component_lifecycle_observation_pending_.load(
           std::memory_order_acquire))},
      {"refreshed", CanonicalValue(refreshed)},
      {"refreshed_at_ns",
       Number(static_cast<std::uint64_t>(alarm_sources_refreshed_at_ns_.load(
                  std::memory_order_acquire)),
              "refreshed_at_ns")},
      {"schema_version",
       CanonicalValue(std::string(kAlarmSourceRefreshSchemaVersion))},
  });
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

CanonicalValue HeadlessApiCore::HistoryCapturePage(
    const std::optional<std::uint64_t> before_capture_id, const std::size_t limit) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    SynchronizeHistorianHealthLocked();
    store = history_store_;
  }
  // Read-only projection outside the coordination lock; capture state is
  // written only by the asynchronous Historian writer.
  return store->Captures(before_capture_id, limit);
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
      "history_request_failed", "control_source_unavailable",
      // Browser-only viewer conditions presented as errors (spec 450 §5.0.1):
      // an unavailable/unverified view of the applied robot or Scene.
      "viewer_model_unavailable", "viewer_scene_unverified",
      "viewer_observation_stale", "viewer_geometry_unavailable"};
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
                                     "omitted_blackboard_count",
                                     "delivery_loss_count",
                                     "details",
                                     "origin"};
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
  Require(text("schema_version") == "manyforge.program_diagnostic.v3", code,
          "Program diagnostic version is unsupported");
  text("program_run_id");
  const auto epoch = text("owner_epoch");
  for (const auto name : {"program_revision", "program_binding_id",
                          "retained_bundle_id", "graph_revision"}) {
    Require(content_identity::is_sha256_digest(text(name)), code,
            "Program diagnostic artifact identity is invalid");
  }
  const auto state = text("state");
  Require(state == "succeeded" || state == "failed" || state == "held" ||
              state == "intervention_required",
          code, "Program diagnostic transition is unsupported");
  const auto reason = OptionalStringMember(record, "reason_code");
  Require(!reason || (reason->size() <= 512U &&
                      content_identity::is_utf8_nfc(*reason)),
          code, "Program diagnostic reason is invalid");
  text("message", 4096U);
  const auto &origin = RequiredMember(record, "origin");
  ValidateFailureDetailsAndOrigin(RequiredMember(record, "details"), origin,
                                  code);
  Require(state != "succeeded" || origin.is_null(), code,
          "Program diagnostic success cannot carry a failure origin");
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
  {
    const auto &values = RequiredMember(record, "blackboard");
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
  for (const auto name : {"omitted_blackboard_count", "delivery_loss_count"}) {
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
  auto event = MakeHistorianEvent(
      "manyforge.program_runner", "program_" + state,
      state == "succeeded" || state == "held" ? "INFO" : "ERROR", monotonic,
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

namespace {

// The current v8 common row carries one neutral addressed command identity.
// Optional provider/output/rule evidence retains its existing typed validation.
void ValidateParticipantExtensions(const CanonicalValue& state) {
  for (const auto& participant : RequiredArray(state, "participants")) {
    Require(participant.is_object() &&
                participant.find_object_member("controller_goal_id") == nullptr &&
                participant.find_object_member("controller_goal_generation") == nullptr,
            "headless_operation_event_invalid", "common participant uses obsolete command identity");
    static_cast<void>(RequiredString(participant, "command_id"));
    static_cast<void>(RequiredCounter(participant, "command_generation", 1U));
    const auto* binding = participant.find_object_member("control_binding_id");
    const bool provider = binding && binding->is_null();
    const auto* output = participant.find_object_member("output_evidence");
    const auto* rules = participant.find_object_member("io_rule_outcomes");
    if (!provider && !output && !rules) continue;
    const auto& revision = RequiredMember(participant, "control_binding_revision");
    const auto& capability = RequiredMember(participant, "capability_provider_binding_id");
    const auto& capability_revision =
        RequiredMember(participant, "capability_provider_binding_revision");
    std::optional<core::CapabilityProviderBindingIdentityDTO> capability_binding;
    if (!capability.is_null() || !capability_revision.is_null()) {
      capability_binding = core::CapabilityProviderBindingIdentityDTO{
          core::CapabilityProviderBindingId(RequiredString(participant, "capability_provider_binding_id")),
          core::CapabilityProviderBindingRevision(RequiredString(participant, "capability_provider_binding_revision"))};
    }
    Require(!provider || (revision.is_null() && capability_binding.has_value()),
            "headless_operation_event_invalid", "provider binding identity is incomplete");
    const core::ParticipantBindingIdentityDTO identity{
        core::ControlDomainId(RequiredString(participant, "control_domain_id")),
        core::ControlDomainRevision(RequiredString(participant, "control_domain_revision")),
        provider ? decltype(core::ParticipantBindingIdentityDTO::binding)(*capability_binding)
                 : decltype(core::ParticipantBindingIdentityDTO::binding)(
                       core::ControllerParticipantBindingIdentityDTO{
                           core::ControlBindingId(RequiredString(participant, "control_binding_id")),
                           core::ControlBindingRevision(RequiredString(participant, "control_binding_revision")),
                           capability_binding})};
    Require(identity.locally_valid(), "headless_operation_event_invalid",
            "participant binding identity is invalid");
    if (output) {
      Require(output->is_string() &&
                  (output->as_string() == "not_submitted" || output->as_string() == "submitted" ||
                   output->as_string() == "source_accepted" || output->as_string() == "source_rejected" ||
                   output->as_string() == "acceptance_unknown"),
              "headless_operation_event_invalid", "output evidence is invalid");
    }
    if (rules) {
      const auto decoded = core::DecodeIoRuleOutcomes(content_identity::canonical_json(*rules));
      Require(decoded.ok() && !decoded.value().empty(), "headless_operation_event_invalid",
              "I/O rule evidence is invalid or empty");
      // Share the journal's historical chronology and exact containing tuple
      // checks. This observation never grants live execution authority.
      const auto qualified = core::ValidateIoRuleOutcomeParticipant(
          core::OperationId(RequiredString(state, "operation_id")),
          core::OperationRevision(RequiredCounter(state, "operation_revision", 1U)),
          core::OperationParticipantId(RequiredString(participant, "participant_id")),
          core::ControllableComponentId(RequiredString(participant, "controllable_component_id")),
          core::CommandTargetId(RequiredString(participant, "command_target_id")),
          core::ControllerGoalId(RequiredString(participant, "command_id")),
          core::ControllerGoalGeneration(RequiredCounter(participant, "command_generation", 1U)),
          decoded.value());
      Require(qualified.ok(), "headless_operation_event_invalid",
              "I/O rule evidence relabels its containing participant");
    }
  }
}

} // namespace

HeadlessApiCore::ParsedOperationFacts HeadlessApiCore::ParseOperationFacts(
    const CanonicalValue &state, const CanonicalValue *intent,
    const bool captured_origin_allowed, const bool terminal_facts_allowed,
    const bool preemption_allowed) {
  const auto parse_participants = [&](const CanonicalValue::Array &participants,
                                      const bool allow_facts) {
    ParsedOperationFacts result;
    bool all_captured = !participants.empty();
    bool has_failure = false;
    bool failure_effects_known = true;
    for (const auto &participant : participants) {
      Require(participant.is_object(), "headless_operation_event_invalid",
              "operation participant must be an object");
      Require(participant.find_object_member("automatic_retry_context") ==
                  nullptr,
              "headless_operation_event_invalid",
              "operation participant has unsupported fields");
      const auto *error = participant.find_object_member("terminal_error");
      const auto *state_value = participant.find_object_member("state");
      const std::string_view participant_state =
          state_value && state_value->is_string() ? state_value->as_string()
                                                  : std::string_view{};
      bool known_local_failure = false;
      if (const auto *fact = participant.find_object_member("terminal_fact")) {
        Require(allow_facts && fact->is_object() &&
                    fact->as_object().size() == 3U,
                "headless_operation_event_invalid",
                "terminal fact has unsupported fields or version");
        Require(IsOperationFaultState(participant_state) ||
                    IsNormalOperationTerminal(participant_state),
                "headless_operation_event_invalid",
                "terminal fact requires a terminal participant");
        const auto kind = RequiredString(*fact, "kind");
        const auto effect = RequiredString(*fact, "effect");
        Require((kind == "computed_completion" || kind == "local_failure" ||
                 kind == "cancelled_before_dispatch" ||
                 kind == "preempted_by_addressed_stop" ||
                 kind == "preempted_by_replacement") &&
                    (effect == "no_effect" || effect == "effect_settled" ||
                     effect == "effect_uncertain"),
                "headless_operation_event_invalid",
                "terminal fact vocabulary is unsupported");
        const auto &fact_error = RequiredMember(*fact, "error");
        const auto validate_error_shape = [](const CanonicalValue &value) {
          Require(value.is_null() ||
                      (value.is_object() && value.as_object().size() == 3U &&
                       RequiredMember(value, "code").is_string() &&
                       RequiredMember(value, "message").is_string() &&
                       RequiredMember(value, "details").is_object()),
                  "headless_operation_event_invalid",
                  "participant terminal error has unsupported fields");
          if (value.is_object())
            for (const auto &[key, detail] :
                 RequiredMember(value, "details").as_object()) {
              static_cast<void>(key);
              Require(detail.is_string(), "headless_operation_event_invalid",
                      "participant terminal error details must contain text "
                      "values");
            }
        };
        validate_error_shape(fact_error);
        Require(error != nullptr, "headless_operation_event_invalid",
                "terminal fact requires the selected error field");
        validate_error_shape(*error);
        if (kind == "preempted_by_replacement") {
          const auto *preemption = state.find_object_member("preemption");
          Require(preemption_allowed && preemption &&
                      RequiredString(*preemption, "kind") == "preempted" &&
                      participant_state == "stopped" &&
                      effect == "effect_settled" && fact_error.is_null(),
                  "headless_operation_event_invalid",
                  "replacement fact requires a settled preempted participant");
        }
        result.terminal_facts_present = true;
        known_local_failure =
            kind == "local_failure" && !fact_error.is_null() && error &&
            fact_error == *error &&
            (effect == "no_effect" || effect == "effect_settled");
      }
      if ((error && !error->is_null()) ||
          IsOperationFaultState(participant_state)) {
        has_failure = true;
        failure_effects_known = failure_effects_known && known_local_failure;
      }
      const auto *scope = participant.find_object_member("execution_scope");
      if (scope) {
        Require(captured_origin_allowed && scope->is_object() &&
                    scope->as_object().size() == 2U,
                "headless_operation_event_invalid",
                "authored origin requires its exact journal version");
        const auto &node = RequiredString(*scope, "node_instance_id");
        const auto &attempt = RequiredString(*scope, "node_attempt_id");
        Require(node.size() <= 128U && node.find('\0') == std::string::npos &&
                    attempt.size() <= 512U &&
                    attempt.find('\0') == std::string::npos,
                "headless_operation_event_invalid",
                "authored origin is invalid or unbounded");
        result.captured_origin = true;
      } else {
        all_captured = false;
      }
    }
    result.captured_failure_effects_known =
        all_captured && has_failure && failure_effects_known;
    return result;
  };
  Require(state.find_object_member("motion_retry_candidate") == nullptr &&
              state.find_object_member("motion_retry_disposition") == nullptr,
          "headless_operation_event_invalid",
          "operation state has unsupported fields");
  auto result = parse_participants(RequiredArray(state, "participants"),
                                   terminal_facts_allowed);
  if (result.captured_origin) {
    const auto &order = RequiredMember(state, "journal_capture_order");
    Require(order.is_string() &&
                content_identity::parse_nonnegative_int64(order) > 0,
            "headless_operation_event_invalid",
            "authored origin requires an ordered snapshot");
  }
  if (const auto *preemption = state.find_object_member("preemption")) {
    Require(preemption_allowed && result.captured_origin &&
                preemption->is_object() && preemption->as_object().size() == 2U,
            "headless_operation_event_invalid",
            "operation preemption requires its captured Program version");
    const auto kind = RequiredString(*preemption, "kind");
    const auto &by = RequiredMember(*preemption, "by");
    Require((kind == "preempted" || kind == "superseded_unsent") &&
                by.is_object() && by.as_object().size() == 3U,
            "headless_operation_event_invalid",
            "operation preemption has unsupported fields or kind");
    const auto &operation = RequiredString(by, "operation_id");
    const auto &node = RequiredString(by, "node_instance_id");
    const auto &attempt = RequiredString(by, "node_attempt_id");
    Require(operation.size() <= 2048U &&
                operation.find('\0') == std::string::npos &&
                node.size() <= 128U && node.find('\0') == std::string::npos &&
                attempt.size() <= 512U &&
                attempt.find('\0') == std::string::npos &&
                operation != RequiredString(state, "operation_id"),
            "headless_operation_event_invalid",
            "operation preemption actor is invalid or unbounded");
    result.preemption_present = true;
  }
  if (intent && intent->is_object()) {
    if (const auto *participants = intent->find_object_member("participants")) {
      Require(participants->is_array(), "headless_operation_event_invalid",
              "operation intent participants must be an array");
      static_cast<void>(parse_participants(participants->as_array(), false));
    }
  }
  return result;
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
  const bool rule_state_projection =
      record_kind == "interrupt_rule_state_changed";
  const bool stop_scoped_projection = stop_acceptance_projection ||
                                      stop_terminal_projection ||
                                      stop_fault_clear_projection;
  Require(!source_owner_epoch.empty() && source_sequence != 0U &&
              source_sequence <= kMaximumSafeInteger && !record_kind.empty() &&
              has_operation_id == has_operation_revision &&
              ((stop_scoped_projection || rule_state_projection) !=
               has_operation_id) &&
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
                                                   "interrupt_rule_state_changed",
                                                   "operation_prepared",
                                                   "state_observed",
                                                   "host_replaced"};
  Require(supported_kinds.contains(record_kind),
          "headless_operation_event_invalid",
          "operation event record kind is unsupported");
  const auto payload_json = content_identity::canonical_json(journal_payload);
  Require(content_identity::sha256_digest(payload_json) == payload_digest &&
              (rule_state_projection ||
               RequiredString(journal_payload, "record_kind") == record_kind),
          "headless_operation_event_invalid",
          "operation event payload does not match its durable envelope");
  const CanonicalValue *operation_state = nullptr;
  ParsedOperationFacts operation_facts;
  std::optional<std::uint64_t> stop_terminal_duration_ns;
  std::optional<std::string> stop_terminal_result;
  if (rule_state_projection) {
    Require(!request_id && !effect_pending,
            "headless_operation_event_invalid",
            "rule-state projection cannot name a request or pending effect");
    ValidateInterruptRuleStateProjection(journal_payload, source_owner_epoch);
  } else if (stop_acceptance_projection) {
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
    Require(api == "manyforge.stop_acceptance_projection.v8" &&
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
    for (const auto &operation : RequiredArray(affected_state, "operations")) {
      ValidateParticipantExtensions(operation);
      static_cast<void>(ParseOperationFacts(operation, nullptr, true, true, true));
    }
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
    Require(record_api == "manyforge.operation_journal_record.v8" &&
                RequiredString(journal_payload, "owner_epoch") ==
                    source_owner_epoch &&
                RequiredBool(journal_payload, "effect_pending") ==
                    effect_pending,
            "headless_operation_event_invalid",
            "operation event payload does not match its durable envelope");
    operation_state = &RequiredMember(journal_payload, "operation_state");
    const auto *capture_order =
        operation_state->find_object_member("journal_capture_order");
    Require(capture_order == nullptr ||
                (capture_order->is_string() &&
                 content_identity::parse_nonnegative_int64(*capture_order) > 0),
            "headless_operation_event_invalid",
            "operation journal capture order is invalid");
    Require(operation_state->is_object() &&
                RequiredString(*operation_state, "operation_id") ==
                    *operation_id &&
                RequiredCounter(*operation_state, "operation_revision", 1U) ==
                    *operation_revision,
            "headless_operation_event_invalid",
            "operation event state does not match its durable envelope");
    ValidateParticipantExtensions(*operation_state);
    operation_facts = ParseOperationFacts(
        *operation_state, journal_payload.find_object_member("intent"), true, true, true);
  }
  if (stop_terminal_projection || stop_fault_clear_projection)
    static_cast<void>(
        ValidateStopFaultProjection(journal_payload, record_kind));
  const auto event_type = OperationHistoryEventType(record_kind, journal_payload);
  // Handling a failure changes its alarm disposition, not its history class.
  const bool stopped_with_error =
      operation_state != nullptr &&
      RequiredString(*operation_state, "state") == "stopped" &&
      std::ranges::any_of(RequiredArray(*operation_state, "participants"),
                          [](const auto &participant) {
                            const auto *error = participant.find_object_member(
                                "terminal_error");
                            return error != nullptr && !error->is_null();
                          });
  const auto severity =
      stopped_with_error ||
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
  if (operation_state != nullptr && std::string_view(severity) == "ERROR") {
    auto message = OperationFailureHistoryMessage(*operation_state);
    if (!message.empty())
      history_fields.emplace_back("message",
                                  CanonicalValue(std::move(message)));
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
    ReconcileOperationFaultLocked(journal_payload, operation_facts, record_kind,
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
    if (const auto mutation = SubmitOperationConditionLocked(
            std::move(update), OperationConditionKind::kExecutionFault,
            operation)) {
      static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
    }
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
    if (const auto mutation = SubmitOperationConditionLocked(
            std::move(update), OperationConditionKind::kRuntimeCollision,
            operation)) {
      static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
    }
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
    if (const auto mutation = SubmitOperationConditionLocked(
            std::move(update), OperationConditionKind::kStopFault, latch)) {
      static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
    }
  }
  for (const auto &latch : retired) {
    active_stop_faults_.erase(latch);
  }
  if (current_stop_fault.has_value() && !current_fault_active) {
    // An authoritative null snapshot also ends refused, never-admitted ones.
    std::erase_if(refused_operation_conditions_, [](const auto &entry) {
      return entry.second.kind == OperationConditionKind::kStopFault;
    });
    SynchronizeAlarmScopeConditionLocked();
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

namespace {

constexpr std::string_view kComponentLifecycleConditionSchemaVersion =
    "manyforge.component_lifecycle_condition.v1";
constexpr std::string_view kComponentLifecycleFaultDefinition =
    "manyforge.runtime.component_lifecycle_fault";
constexpr std::size_t kMaximumComponentLifecycleConditionScope = 256U;

/// Blocking cause classes held as a lifecycle fault (spec 530 §3.3,
/// 2026-10-08; incident classes of the 2026-10-08 addendum 2).
/// `configuration` stays in CCLC/control-domain reconciliation status and
/// `availability` is not a lifecycle fault.
bool ComponentLifecycleFaultCause(const std::string_view cause) {
  return cause == "emergency_stop" || cause == "protective_stop" ||
         cause == "safeguard_stop" || cause == "fault" || cause == "unknown" ||
         cause == "collision" || cause == "external_control" ||
         cause == "provider_lost";
}

bool KnownBlockingCauseClass(const std::string_view cause) {
  return ComponentLifecycleFaultCause(cause) || cause == "none" ||
         cause == "availability" || cause == "configuration";
}

/// A nullable object member; absent and null are both no value.
const CanonicalValue *NullableObjectMember(const CanonicalValue &root,
                                           const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr ||
      std::holds_alternative<std::nullptr_t>(member->storage())) {
    return nullptr;
  }
  Require(member->is_object(), "headless_component_lifecycle_invalid",
          "CCLC component snapshot " + std::string(name) +
              " must be an object or null");
  return member;
}

CanonicalValue NullableText(const CanonicalValue *root,
                            const std::string_view name) {
  if (root == nullptr) {
    return CanonicalValue(nullptr);
  }
  const auto &member = RequiredMember(*root, name);
  if (std::holds_alternative<std::nullptr_t>(member.storage())) {
    return member;
  }
  Require(std::holds_alternative<std::string>(member.storage()),
          "headless_component_lifecycle_invalid",
          "CCLC component snapshot " + std::string(name) +
              " must be text or null");
  return member;
}

} // namespace

CanonicalValue HeadlessApiCore::ComponentLifecycleConditionScope() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto identity = [this](const bool bundle) {
    return alarm_registry_identity_.has_value()
               ? CanonicalValue(bundle ? alarm_registry_identity_->first
                                       : alarm_registry_identity_->second)
               : CanonicalValue(nullptr);
  };
  return CanonicalValue(CanonicalValue::Object{
      {"component_ids", StringArrayValue(alarm_component_ids_)},
      {"graph_revision", identity(false)},
      {"retained_bundle_id", identity(true)},
      {"schema_version",
       CanonicalValue(std::string(kComponentLifecycleConditionSchemaVersion))},
      {"source_registered",
       CanonicalValue(!component_lifecycle_adapter_.source_epoch.empty())},
  });
}

bool HeadlessApiCore::RefreshCurrentAlarmRegistryLocked() {
  // Spec 530 §7.1: judged against the configuration Manager publishes now.
  // The refresh replaces the registry (and marks lifecycle quality unknown)
  // when the publication names a new configuration. A stale or absent
  // publication still names its last configuration, which an Apply may
  // already have superseded, and an unverifiable one leaves the registry on
  // the previous configuration: neither can judge an observation.
  const auto runtime = RefreshLocked();
  const auto bundle_id =
      OptionalStringMember(runtime.root, "active_retained_bundle_id");
  const auto graph_revision =
      OptionalStringMember(runtime.root, "active_graph_revision");
  return SectionFresh(RequiredSection(runtime.root, "manager")) &&
         bundle_id.has_value() && graph_revision.has_value() &&
         alarm_registry_identity_ ==
             std::make_pair(*bundle_id, *graph_revision);
}

void HeadlessApiCore::ReturnRemovedComponentConditionsLocked(
    const std::set<std::string> &in_scope,
    const std::function<CanonicalValue(const std::string &component_id)>
        &correlations) {
  std::vector<std::pair<std::string, std::string>> removed;
  for (const auto &[key, state] : component_conditions_) {
    static_cast<void>(state);
    if (!in_scope.contains(key.second)) {
      removed.push_back(key);
    }
  }
  for (const auto &key : removed) {
    const auto state = component_conditions_.at(key);
    SynchronizeComponentConditionLocked(
        component_lifecycle_adapter_, key.first, key.second, state.subject,
        false,
        CanonicalValue(CanonicalValue::Object{
            {"reason", CanonicalValue("component_removed")}}),
        correlations(key.second));
    component_conditions_.erase(key);
  }
}

CanonicalValue HeadlessApiCore::ObserveComponentLifecycleConditions(
    std::string cclc_owner_epoch, CanonicalValue snapshots,
    std::vector<std::string> unavailable_component_ids) {
  const auto *rows = std::get_if<CanonicalValue::Array>(&snapshots.storage());
  Require(!cclc_owner_epoch.empty() && cclc_owner_epoch.size() <= 256U &&
              content_identity::is_utf8_nfc(cclc_owner_epoch) &&
              rows != nullptr &&
              rows->size() + unavailable_component_ids.size() <=
                  kMaximumComponentLifecycleConditionScope,
          "headless_component_lifecycle_invalid",
          "component lifecycle observation envelope is malformed");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");

  struct Observed final {
    AlarmSubject subject;
    bool lifecycle_fault{};
    CanonicalValue details;
    CanonicalValue correlations;
    /// (retained bundle, graph revision) CCLC captured the row under.
    std::pair<std::string, std::string> configuration;
  };
  std::set<std::string> named;
  for (const auto &component : unavailable_component_ids) {
    Require(!component.empty() && named.insert(component).second,
            "headless_component_lifecycle_invalid",
            "unavailable component ids must be unique and non-empty");
  }
  std::map<std::string, Observed> observed;
  for (const auto &row : *rows) {
    Require(row.is_object(), "headless_component_lifecycle_invalid",
            "CCLC component snapshot must be an object");
    auto component = RequiredString(row, "controllable_component_id");
    Require(!component.empty() && named.insert(component).second,
            "headless_component_lifecycle_invalid",
            "CCLC component snapshots must name unique components");
    std::set<std::string> resources;
    CanonicalValue::Array domain_ids;
    for (const auto &domain : RequiredArray(row, "domains")) {
      domain_ids.emplace_back(RequiredString(domain, "control_domain_id"));
      for (const auto &resource : RequiredArray(domain, "resources")) {
        const auto *resource_id = std::get_if<std::string>(&resource.storage());
        Require(resource_id != nullptr && !resource_id->empty(),
                "headless_component_lifecycle_invalid",
                "component resources must be non-empty text");
        resources.insert(*resource_id);
      }
    }
    const auto *target = NullableObjectMember(row, "lifecycle_target");
    const auto *progress = NullableObjectMember(row, "lifecycle_progress");
    const auto cause = progress == nullptr
                           ? std::string("none")
                           : RequiredString(*progress, "blocking_cause_class");
    Require(KnownBlockingCauseClass(cause),
            "headless_component_lifecycle_invalid",
            "CCLC component snapshot names an unknown blocking cause class");
    // An open incident (addendum 2, 2026-10-08) holds the condition until
    // the operator's acknowledgement is accepted and CCLC publishes
    // `incident: null`; a current intervention cause holds it as well.
    const auto *incident =
        progress == nullptr ? nullptr
                            : NullableObjectMember(*progress, "incident");
    CanonicalValue::Object details{
        {"blocking_cause_class", CanonicalValue(cause)},
        {"blocking_reason", NullableText(progress, "blocking_reason")},
        {"component_lifecycle_state",
         CanonicalValue(RequiredString(row, "component_lifecycle_state"))},
    };
    if (incident != nullptr) {
      const auto id = RequiredString(*incident, "incident_id");
      const auto incident_class = RequiredString(*incident, "cause_class");
      Require(!id.empty() && id.size() <= 128U &&
                  KnownBlockingCauseClass(incident_class) &&
                  incident_class != "none" && incident_class != "availability",
              "headless_component_lifecycle_invalid",
              "CCLC component snapshot incident is malformed");
      for (const auto &code : RequiredArray(*incident, "vendor_codes")) {
        const auto *text = std::get_if<std::string>(&code.storage());
        Require(text != nullptr && !text->empty() && text->size() <= 64U,
                "headless_component_lifecycle_invalid",
                "CCLC component snapshot incident vendor codes are malformed");
      }
      Require(RequiredArray(*incident, "vendor_codes").size() <= 16U,
              "headless_component_lifecycle_invalid",
              "CCLC component snapshot incident names too many vendor codes");
      // Present only while the incident is open: the returned record keeps
      // the identity of the incident that raised it.
      details.emplace_back("incident_cause_class",
                           CanonicalValue(incident_class));
      details.emplace_back("incident_id", CanonicalValue(id));
      details.emplace_back("incident_summary",
                           CanonicalValue(RequiredString(*incident, "summary")));
      details.emplace_back("incident_vendor_codes",
                           RequiredMember(*incident, "vendor_codes"));
    }
    details.emplace_back("lifecycle_phase", NullableText(progress, "phase"));
    details.emplace_back(
        "requested_state",
        target == nullptr
            ? CanonicalValue(nullptr)
            : CanonicalValue(RequiredString(*target, "requested_state")));
    details.emplace_back(
        "target_generation",
        target == nullptr
            ? CanonicalValue(nullptr)
            : Number(RequiredCounter(*target, "target_generation", 1U),
                     "target_generation"));
    auto configuration = std::make_pair(RequiredString(row, "retained_bundle_id"),
                                        RequiredString(row, "graph_revision"));
    Observed value{
        AlarmSubject{"component", component,
                     std::vector<std::string>(resources.begin(),
                                              resources.end())},
        incident != nullptr || ComponentLifecycleFaultCause(cause),
        CanonicalValue(std::move(details)),
        CanonicalValue(CanonicalValue::Object{
            {"cclc_owner_epoch", CanonicalValue(cclc_owner_epoch)},
            {"control_domain_ids", CanonicalValue(std::move(domain_ids))},
            {"controllable_component_id", CanonicalValue(component)},
            {"graph_revision", CanonicalValue(configuration.second)},
            {"retained_bundle_id", CanonicalValue(configuration.first)},
        }),
        std::move(configuration),
    };
    observed.insert_or_assign(std::move(component), std::move(value));
  }
  const auto response = [&](const std::string_view status) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version",
         CanonicalValue(std::string(kComponentLifecycleConditionSchemaVersion))},
        {"source_owner_epoch", CanonicalValue(cclc_owner_epoch)},
        {"status", CanonicalValue(std::string(status))},
    });
  };
  if (!RefreshCurrentAlarmRegistryLocked()) {
    return response("configuration_unavailable");
  }
  const std::set<std::string> scope(alarm_component_ids_.begin(),
                                    alarm_component_ids_.end());
  if (named != scope) {
    // The active configuration changed between the scope read and this
    // observation; the next observation names the new scope.
    return response("scope_changed");
  }
  for (const auto &[component, value] : observed) {
    static_cast<void>(component);
    if (value.configuration != alarm_registry_identity_) {
      // Fenced to the active configuration: a row CCLC captured under a
      // superseded retained bundle or graph revision is no observation of
      // the active one, even when it names the same components. It raises,
      // keeps and clears nothing; CCLC restates the row once it has adopted
      // the active configuration.
      return response("scope_changed");
    }
  }
  if (scope.empty() && component_lifecycle_adapter_.source_epoch.empty()) {
    // Nothing in scope and no source to reconcile.
    component_lifecycle_observation_pending_.store(false,
                                                   std::memory_order_release);
    return response("empty");
  }

  if (component_lifecycle_adapter_.source_epoch != cclc_owner_epoch) {
    // A replacement CCLC owner restarts source ordering: restate every
    // tracked point under the new epoch.
    for (auto &[key, state] : component_conditions_) {
      static_cast<void>(key);
      state.fingerprint.clear();
    }
  }
  RegisterAdapterEpochLocked(component_lifecycle_adapter_, cclc_owner_epoch);
  for (const auto &[component, value] : observed) {
    SynchronizeComponentConditionLocked(
        component_lifecycle_adapter_,
        std::string(kComponentLifecycleFaultDefinition), component,
        value.subject, value.lifecycle_fault, value.details,
        value.correlations);
  }
  // A component that left the active configuration has no lifecycle row:
  // its raised conditions return to normal and are forgotten.
  ReturnRemovedComponentConditionsLocked(
      named, [&cclc_owner_epoch](const std::string &component_id) {
        return CanonicalValue(CanonicalValue::Object{
            {"cclc_owner_epoch", CanonicalValue(cclc_owner_epoch)},
            {"controllable_component_id", CanonicalValue(component_id)},
        });
      });
  if (!unavailable_component_ids.empty()) {
    // Only a complete observation of the active configuration establishes
    // current quality. An unread component leaves it as it was, so a read
    // after an Apply cannot confirm conditions of the superseded one. The
    // read components were applied; the caller repeats the read soon rather
    // than treating this as the period's accepted observation.
    return response("incomplete");
  }
  alarm_owner_.CompleteSourceSnapshot(
      component_lifecycle_adapter_.source_id,
      component_lifecycle_adapter_.source_epoch, AlarmConditionQuality::kGood);
  component_lifecycle_observation_pending_.store(false,
                                                 std::memory_order_release);
  return response("observed");
}

CanonicalValue HeadlessApiCore::ObserveEmptyComponentLifecycleConfiguration(
    std::string retained_bundle_id, std::string graph_revision) {
  Require(!retained_bundle_id.empty() && retained_bundle_id.size() <= 256U &&
              content_identity::is_utf8_nfc(retained_bundle_id) &&
              !graph_revision.empty() && graph_revision.size() <= 256U &&
              content_identity::is_utf8_nfc(graph_revision),
          "headless_component_lifecycle_invalid",
          "empty-configuration lifecycle observation identity is malformed");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto response = [&](const std::string_view status) {
    return CanonicalValue(CanonicalValue::Object{
        {"graph_revision", CanonicalValue(graph_revision)},
        {"retained_bundle_id", CanonicalValue(retained_bundle_id)},
        {"schema_version",
         CanonicalValue(std::string(kComponentLifecycleConditionSchemaVersion))},
        {"status", CanonicalValue(std::string(status))},
    });
  };
  if (!RefreshCurrentAlarmRegistryLocked()) {
    return response("configuration_unavailable");
  }
  if (alarm_registry_identity_ !=
          std::make_pair(retained_bundle_id, graph_revision) ||
      !alarm_component_ids_.empty()) {
    // An Apply since the scope read: the next read names the new scope.
    return response("scope_changed");
  }
  if (component_lifecycle_adapter_.source_epoch.empty()) {
    // Never registered: no condition was ever raised.
    component_lifecycle_observation_pending_.store(false,
                                                   std::memory_order_release);
    return response("empty");
  }
  // A configuration without controlled components has no CCLC owner and no
  // lifecycle conditions. This is Headless's own observation of the active
  // configuration, not a CCLC statement, so it continues the source's
  // current epoch and is correlated to the configuration, not to an owner.
  ReturnRemovedComponentConditionsLocked(
      {}, [&](const std::string &component_id) {
        return CanonicalValue(CanonicalValue::Object{
            {"controllable_component_id", CanonicalValue(component_id)},
            {"graph_revision", CanonicalValue(graph_revision)},
            {"retained_bundle_id", CanonicalValue(retained_bundle_id)},
        });
      });
  alarm_owner_.CompleteSourceSnapshot(
      component_lifecycle_adapter_.source_id,
      component_lifecycle_adapter_.source_epoch, AlarmConditionQuality::kGood);
  component_lifecycle_observation_pending_.store(false,
                                                 std::memory_order_release);
  return response("observed");
}

CanonicalValue HeadlessApiCore::MarkComponentLifecycleSourceUnknown() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const bool registered = !component_lifecycle_adapter_.source_epoch.empty();
  if (registered) {
    // Unknown quality cannot clear: raised conditions stay active.
    alarm_owner_.MarkSourceQualityUnknown(
        component_lifecycle_adapter_.source_id,
        component_lifecycle_adapter_.source_epoch);
    component_lifecycle_observation_pending_.store(true,
                                                   std::memory_order_release);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue(std::string(kComponentLifecycleConditionSchemaVersion))},
      {"status",
       CanonicalValue(registered ? "marked_unknown" : "not_registered")},
  });
}

namespace {

constexpr std::string_view kCCLCIncidentHistorySchemaVersion =
    "manyforge.cclc_incident_history_record.v2";
constexpr std::size_t kMaximumRecordedCCLCIncidentEvents = 2048U;

} // namespace

CanonicalValue
HeadlessApiCore::RecordCCLCIncidentEvent(std::string cclc_owner_epoch,
                                         CanonicalValue event) {
  const std::string code = "headless_cclc_incident_event_invalid";
  Require(!cclc_owner_epoch.empty() && cclc_owner_epoch.size() <= 256U &&
              content_identity::is_utf8_nfc(cclc_owner_epoch) &&
              event.is_object() && event.as_object().size() == 5U,
          code, "CCLC incident event envelope is malformed");
  const auto event_kind = RequiredString(event, "event_kind");
  const auto incident_id = RequiredString(event, "incident_id");
  const auto &details = RequiredMember(event, "payload");
  const auto *sequence =
      std::get_if<double>(&RequiredMember(event, "event_sequence").storage());
  Require((event_kind == "component_incident" ||
           event_kind == "component_incident_acknowledged") &&
              incident_id.size() <= 128U &&
              content_identity::is_utf8_nfc(incident_id) &&
              sequence != nullptr && std::isfinite(*sequence) &&
              *sequence >= 1.0 && std::floor(*sequence) == *sequence &&
              *sequence <= static_cast<double>(kMaximumSafeInteger) &&
              details.is_object() &&
              RequiredString(details, "incident_id") == incident_id &&
              RequiredString(details, "event_kind") == event_kind,
          code, "CCLC incident event does not match its record");
  Require(RequiredString(details, "schema_version") == core::kComponentIncidentEventSchemaVersion,
          code, "CCLC component incident version is unsupported");
  const auto cclc_occurred_at_ns = content_identity::parse_nonnegative_int64(
      RequiredMember(event, "occurred_at_ns"));
  const auto key = std::make_pair(incident_id, event_kind);
  const auto response = [&](const std::string_view status,
                            const std::optional<std::string> &event_id) {
    return CanonicalValue(CanonicalValue::Object{
        {"event_id", OptionalStringValue(event_id)},
        {"schema_version",
         CanonicalValue(std::string(kCCLCIncidentHistorySchemaVersion))},
        {"status", CanonicalValue(std::string(status))},
    });
  };

  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    if (recorded_cclc_incident_events_.contains(key)) {
      return response("duplicate", std::nullopt);
    }
    store = history_store_;
  }
  // A restarted reader replays from its cursor; the durable record of an
  // earlier process answers it. The read stays outside the core lock.
  HistorianQuery query;
  query.components = {"manyforge.cclc"};
  query.event_types = {event_kind};
  query.correlations = {{"incident_id", incident_id}};
  query.limit = 1U;
  const bool durable = !store->Query(query).events.empty();

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  if (recorded_cclc_incident_events_.size() >=
      kMaximumRecordedCCLCIncidentEvents) {
    recorded_cclc_incident_events_.clear();
  }
  if (durable || recorded_cclc_incident_events_.contains(key)) {
    recorded_cclc_incident_events_.insert(key);
    return response("duplicate", std::nullopt);
  }
  // Historian time is this owner's recording time: CCLC stamps its own
  // monotonic clock, kept in the record as `cclc_occurred_at_ns`.
  auto historian_event = MakeHistorianEvent(
      "manyforge.cclc", event_kind,
      event_kind == "component_incident" ? "ERROR" : "INFO",
      clocks_.monotonic_time_ns(), clocks_.wall_time_text(),
      CanonicalValue(CanonicalValue::Object{
          {"cclc_occurred_at_ns",
           content_identity::nonnegative_int64_value(cclc_occurred_at_ns)},
          {"cclc_owner_epoch", CanonicalValue(cclc_owner_epoch)},
          {"clock_owner", CanonicalValue("headless:" + config_.owner_epoch)},
          {"details", details},
          {"event_kind", CanonicalValue(event_kind)},
          {"event_sequence", CanonicalValue(*sequence)},
          {"incident_id", CanonicalValue(incident_id)},
          {"schema_version",
           CanonicalValue("manyforge.cclc_incident_history_event.v2")},
      }));
  const auto event_id = historian_event.event_id;
  const bool recorded = historian_.TryEnqueue(std::move(historian_event));
  if (recorded) {
    recorded_cclc_incident_events_.insert(key);
  } else {
    alarm_owner_.MarkHealthUnavailable(
        std::string(kHistorianOwnerHealthCode),
        "historian delivery queue rejected a CCLC incident event");
  }
  SynchronizeHistorianHealthLocked();
  return response(recorded ? "recorded" : "rejected",
                  recorded ? std::optional(event_id) : std::nullopt);
}

void HeadlessApiCore::SynchronizeComponentConditionLocked(
    AdapterCursor &cursor, const std::string &definition_id,
    const std::string &component_id, const AlarmSubject &subject,
    const bool active, const CanonicalValue &details,
    const CanonicalValue &correlations) {
  const auto key = std::make_pair(definition_id, component_id);
  auto found = component_conditions_.find(key);
  if (found == component_conditions_.end() && !active) {
    return;
  }
  const auto submit = [&](const AlarmSubject &scope, const bool raise) {
    Require(cursor.source_sequence < kMaximumSafeInteger,
            "headless_adapter_sequence_exhausted",
            "component lifecycle alarm adapter sequence is exhausted");
    ++cursor.source_sequence;
    AlarmConditionUpdate update{
        definition_id,
        scope,
        raise,
        cursor.source_id,
        cursor.source_epoch,
        cursor.source_sequence,
        clocks_.monotonic_time_ns(),
        clocks_.wall_time_text(),
        AlarmConditionQuality::kGood,
        CanonicalValue(CanonicalValue::Object{}),
        details,
        {},
        correlations,
    };
    try {
      const auto mutation = alarm_owner_.Submit(std::move(update));
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      return true;
    } catch (const OperatorAlarmError &error) {
      // A component whose resources are outside the active configuration
      // (or that has none) cannot be scoped. The refusal is counted by the
      // owner; the same state is retried only after the configuration or
      // the observation changes.
      if (error.code() != "operator_alarm_scope_inactive" &&
          error.code() != "operator_alarm_scope_invalid") {
        throw;
      }
      return false;
    }
  };
  if (found != component_conditions_.end() && found->second.active &&
      found->second.subject != subject) {
    // The resources changed while raised: the old point returns to normal.
    static_cast<void>(submit(found->second.subject, false));
    found->second = ComponentConditionState{subject, false, {}};
  }
  const CanonicalValue condition(CanonicalValue::Object{
      {"active", CanonicalValue(active)},
      {"details", details},
      {"registry",
       alarm_registry_identity_.has_value()
           ? CanonicalValue(alarm_registry_identity_->first + "\n" +
                            alarm_registry_identity_->second)
           : CanonicalValue(nullptr)},
      {"resource_ids", StringArrayValue(subject.resource_ids)},
  });
  const auto fingerprint = content_identity::framed_sha256(
      "manyforge-headless-component-lifecycle-condition", "v1", condition);
  if (found != component_conditions_.end()) {
    if (found->second.fingerprint == fingerprint) {
      return;
    }
    if (!found->second.active && !active) {
      // Nothing raised to return to normal.
      found->second = ComponentConditionState{subject, false, fingerprint};
      return;
    }
  }
  const bool admitted = submit(subject, active);
  component_conditions_.insert_or_assign(
      key, ComponentConditionState{subject, active && admitted, fingerprint});
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
  SynchronizeAlarmResourceRegistryLocked(runtime);
  RefreshRuntimeHealthAdapterLocked(runtime);
  RefreshSceneAdapterLocked(runtime);
  SynchronizeHistorianHealthLocked();
  // Only a refresh that completed every alarm source makes them current, as
  // of the instant the sources were observed (never later).
  alarm_sources_refreshed_at_ns_.store(runtime.projected_at_ns,
                                       std::memory_order_release);
  alarm_owner_.ReconcileHealth(kAlarmSourceRefreshOverdueHealthCode);
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
  const auto transition_id = ManagerApplyTransitionId(runtime.root);
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
  } else if (transition_id.has_value()) {
    // Manager's durable incomplete operation is the owner-issued explanation
    // for this planned unavailable interval. Keep readiness fail-closed and
    // expose that operation's phase, but do not turn its intentional teardown
    // into a runtime-loss alarm. A terminal failure removes the incomplete
    // operation, so a still-unavailable provider is alarmed on the next owner
    // publication without a timeout or inferred recovery.
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
    for (const auto *cursor : {&scene_adapter_, &scene_pose_adapter_}) {
      if (!cursor->source_epoch.empty()) {
        alarm_owner_.MarkSourceQualityUnknown(cursor->source_id,
                                              cursor->source_epoch);
      }
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

  // Each new carried-object-lost event whose attachment has
  // `on_grasp_lost.raise_alarm` raises its dedicated alarm once. It returns at
  // once and stays listed until acknowledged; nothing is blocked.
  RegisterAdapterEpochLocked(scene_pose_adapter_, *source_epoch);
  std::set<std::string> published;
  if (const auto *losses = state.find_object_member("carried_object_losses")) {
    for (const auto &loss : losses->as_array()) {
      const auto event_id = RequiredString(loss, "event_id");
      published.insert(event_id);
      if (raised_carried_object_losses_.contains(event_id) ||
          !RequiredBool(RequiredMember(loss, "on_grasp_lost"),
                        "raise_alarm")) {
        continue;
      }
      const AlarmSubject subject{"scene_resource",
                                 RequiredString(loss, "scene_resource_id"), {}};
      CanonicalValue::Object loss_fields{
          {"code", RequiredMember(loss, "code")},
          {"event_id", CanonicalValue(event_id)},
          {"full_text", RequiredMember(loss, "full_text")},
      };
      // The typed publication error of a fail-closed loss (SX-204).
      if (const auto *signal_error = loss.find_object_member("signal_error"))
        loss_fields.emplace_back("signal_error", *signal_error);
      const CanonicalValue loss_details(std::move(loss_fields));
      for (const bool active : {true, false}) {
        const auto raised = SubmitAdapterUpdateLocked(
            scene_pose_adapter_,
            std::string(manyforge::core::kCarriedObjectLostRow.code), subject,
            active, loss_details, correlations);
        static_cast<void>(DeliverFactsLocked(raised.lifecycle_facts));
      }
    }
  }
  raised_carried_object_losses_ = std::move(published);
  alarm_owner_.CompleteSourceSnapshot(scene_pose_adapter_.source_id,
                                      scene_pose_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
}

void HeadlessApiCore::RegisterOperationEpochLocked(
    std::string source_owner_epoch) {
  if (operation_adapter_.source_epoch == source_owner_epoch) {
    return;
  }
  RegisterAdapterEpochLocked(operation_adapter_, std::move(source_owner_epoch));
  std::erase_if(refused_operation_conditions_, [](const auto &entry) {
    return entry.second.kind != OperationConditionKind::kProgramFailure;
  });
  SynchronizeAlarmScopeConditionLocked();
  operation_native_sequence_ = 0U;
  operation_native_record_digest_.clear();
  observed_operation_faults_.clear();
  observed_runtime_collisions_.clear();
  current_stop_fault_fingerprint_.clear();
}

void HeadlessApiCore::ReconcileRuntimeCollisionStopLocked(
    const CanonicalValue &journal_payload,
    const std::uint64_t native_sequence, const std::string_view payload_digest,
    const std::int64_t occurred_at_ns, const std::string_view occurred_at_wall,
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
  if (const auto mutation = SubmitOperationConditionLocked(
          std::move(update), OperationConditionKind::kRuntimeCollision,
          operation_id)) {
    static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
  }
}

void HeadlessApiCore::ReconcileOperationFaultLocked(
    const CanonicalValue &journal_payload, const ParsedOperationFacts &facts,
    const std::string_view record_kind, const std::string_view request_id,
    const std::uint64_t native_sequence, const std::string_view payload_digest,
    const std::int64_t occurred_at_ns, const std::string_view occurred_at_wall,
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
      if (const auto mutation = SubmitOperationConditionLocked(
              std::move(update), OperationConditionKind::kRuntimeCollision,
              operation_id)) {
        static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
      }
      active_runtime_collisions_.erase(collision);
      observed_runtime_collisions_.erase(operation_id);
    } else {
      ForgetRefusedOperationConditionLocked(
          "manyforge.motion.runtime_collision", operation_id);
    }
  }
  // Runtime collision is a separate native condition, including for Program work.
  bool runtime_collision_outcome = active_runtime_collisions_.contains(operation_id);
  for (const auto &participant : RequiredArray(state, "participants")) {
    const auto *error = participant.find_object_member("terminal_error");
    const auto *details =
        error != nullptr && error->is_object() ? error->find_object_member("details")
                                               : nullptr;
    const auto *reason = details != nullptr && details->is_object()
                             ? details->find_object_member("reason")
                             : nullptr;
    if (reason != nullptr && reason->is_string() &&
        reason->as_string() == "runtime_collision_fault") {
      runtime_collision_outcome = true;
    }
  }
  const auto *stop_id = state.find_object_member("active_stop_id");
  const auto *stop_generation = state.find_object_member("active_stop_generation");
  // A participant fact does not settle a Stop or an owner cleanup fault.
  // Ordinary proven Program failures are annunciated by the accepted Run,
  // while their original committed attempt is already ERROR history.
  const bool ordinary_program_failure = facts.captured_failure_effects_known &&
      stop_id && stop_id->is_null() && stop_generation && stop_generation->is_null() &&
      record_kind != "host_replaced";
  const bool active = IsOperationFaultState(state_name) && !reset_accepted &&
                      !runtime_collision_outcome && !ordinary_program_failure;
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
  } else {
    const auto found = active_operation_faults_.find(operation_id);
    if (found == active_operation_faults_.end()) {
      ForgetRefusedOperationConditionLocked("manyforge.motion.execution_fault",
                                            operation_id);
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
      subject,
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
  // Only an admitted condition is tracked for its later return to normal.
  if (const auto mutation = SubmitOperationConditionLocked(
          std::move(update), OperationConditionKind::kExecutionFault,
          operation_id)) {
    static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
  }
}

void HeadlessApiCore::ReconcileStopFaultLocked(
    const CanonicalValue &journal_payload, const std::string_view record_kind,
    const std::uint64_t native_sequence, const std::string_view payload_digest,
    const std::int64_t occurred_at_ns, const std::string_view occurred_at_wall,
    const std::string_view raw_event_id) {
  const auto &fault_latch =
      ValidateStopFaultProjection(journal_payload, record_kind);
  if (std::holds_alternative<std::nullptr_t>(fault_latch.storage())) {
    return;
  }
  const auto latch_id = RequiredString(fault_latch, "latch_id");
  const auto cell_id = RequiredString(fault_latch, "cell_id");
  const bool active = record_kind == "stop_terminal_projected";
  current_stop_fault_fingerprint_.clear();

  AlarmSubject subject;
  if (active) {
    subject = AlarmSubject{"cell", cell_id, {}};
  } else {
    const auto found = active_stop_faults_.find(latch_id);
    if (found == active_stop_faults_.end()) {
      ForgetRefusedOperationConditionLocked("manyforge.motion.stop_fault",
                                            latch_id);
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
  AlarmConditionUpdate update{
      "manyforge.motion.stop_fault",
      subject,
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
  if (const auto mutation = SubmitOperationConditionLocked(
          std::move(update), OperationConditionKind::kStopFault, latch_id)) {
    static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
  }
}

void HeadlessApiCore::ObserveCurrentStopFaultLocked(
    const CanonicalValue &fault_latch, const std::uint64_t native_sequence) {
  Require(fault_latch.is_object() && fault_latch.as_object().size() == 20U,
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
           trigger == "persistence_degraded" ||
           trigger == "post_reserve_fence_failed") &&
          RequiredCounter(fault_latch, "latch_generation", 1U) != 0U &&
          RequiredCounter(fault_latch, "recovery_assessment_revision", 1U) !=
              0U &&
          RequiredMember(fault_latch, "captured_operation_ids").is_array() &&
          RequiredMember(fault_latch, "captured_resources").is_array() &&
          RequiredMember(fault_latch, "evidence_refs").is_array() &&
          RequiredMember(fault_latch, "observed_triggers").is_array() &&
          !RequiredMember(fault_latch, "observed_triggers").as_array().empty() &&
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
  }
  if (current_stop_fault_fingerprint_ == payload_digest) {
    return;
  }

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
  const auto mutation = SubmitOperationConditionLocked(
      std::move(update), OperationConditionKind::kStopFault, latch_id);
  // An unchanged observation is not resubmitted on every snapshot. A refused
  // one is retained by the submit path and reconsidered once when the active
  // configuration changes, so it is neither flooded nor stranded.
  current_stop_fault_fingerprint_ = payload_digest;
  if (mutation.has_value()) {
    static_cast<void>(DeliverFactsLocked(mutation->lifecycle_facts));
  }
}

void HeadlessApiCore::SynchronizeAlarmResourceRegistryLocked(
    const HeadlessRuntimeSnapshot &runtime) {
  // The alarm resource inventory follows the exact configuration Manager
  // publishes as active. Before the first publication it stays empty; a stale
  // publication keeps its last named configuration, because no newer
  // configuration can be active without Manager publishing it.
  const auto bundle_id =
      OptionalStringMember(runtime.root, "active_retained_bundle_id");
  const auto graph_revision =
      OptionalStringMember(runtime.root, "active_graph_revision");
  if (!bundle_id.has_value() || !graph_revision.has_value()) {
    return;
  }
  auto identity = std::make_pair(*bundle_id, *graph_revision);
  if (alarm_registry_identity_ == identity) {
    return;
  }
  bool replaced = false;
  try {
    const auto bundle = VerifiedActiveBundleLocked(
        *bundle_id, *graph_revision,
        std::string(kAlarmResourceRegistryHealthCode),
        std::string(kAlarmResourceRegistryHealthCode));
    std::set<std::string> resources;
    for (const auto &binding : bundle->value().cell_graph.control_bindings) {
      for (const auto &resource : binding.resource_bindings) {
        resources.insert(resource.resource_id.value());
      }
    }
    // Logical output identities are sealed by the same qualified catalog as
    // command binding. Input-only devices contribute no invented resource.
    const auto signals =
        deployment::ResolveRetainedIoSignalCatalog(bundle->value().cell_graph);
    if (!signals.ok()) {
      throw std::runtime_error(signals.error().message);
    }
    for (const auto &signal : signals.value()) {
      if (signal.descriptor.output_resource) {
        resources.insert(signal.descriptor.output_resource->value());
      }
    }
    std::set<std::string> components;
    for (const auto &component :
         bundle->value().cell_graph.controllable_components) {
      components.insert(component.controllable_component_id.value());
    }
    alarm_owner_.ReplaceResourceRegistry(
        AlarmResourceRegistry{*bundle_id, *graph_revision, std::move(resources)});
    alarm_registry_identity_ = std::move(identity);
    alarm_component_ids_.assign(components.begin(), components.end());
    replaced = true;
    alarm_owner_.ReconcileHealth(kAlarmResourceRegistryHealthCode);
    ReconsiderRefusedOperationConditionsLocked();
  } catch (const std::exception &error) {
    // An active configuration whose inventory cannot be verified leaves no
    // trustworthy attention scope: name it as owner health, which blocks new
    // motion admission while the retained alarms stay visible.
    std::string detail = error.what();
    if (detail.empty() || detail.size() > 1024U ||
        !content_identity::is_utf8_nfc(detail)) {
      detail = "inventory verification failed";
    }
    alarm_owner_.MarkHealthUnavailable(
        std::string(kAlarmResourceRegistryHealthCode),
        "active configuration " + *bundle_id +
            " has no verified alarm resource inventory: " + detail);
  }
  const bool lifecycle_registered =
      !component_lifecycle_adapter_.source_epoch.empty();
  if (replaced && lifecycle_registered) {
    // Lifecycle conditions observed under the superseded configuration are
    // not observations of this one. They keep their state at unknown quality
    // (which cannot clear them) until an observation fenced to this
    // configuration, possibly of an empty scope, restates or returns them.
    alarm_owner_.MarkSourceQualityUnknown(
        component_lifecycle_adapter_.source_id,
        component_lifecycle_adapter_.source_epoch);
  }
  if (replaced && (lifecycle_registered || !alarm_component_ids_.empty())) {
    // The new configuration needs its own lifecycle observation: the event
    // pump reads it at once instead of at its next ordinary period.
    component_lifecycle_observation_pending_.store(true,
                                                   std::memory_order_release);
  }
}

namespace {

std::string RefusedConditionKey(const std::string_view definition_id,
                                const std::string_view tracking_id) {
  return std::string(definition_id) + '\n' + std::string(tracking_id);
}

} // namespace

void HeadlessApiCore::TrackAdmittedOperationConditionLocked(
    const OperationConditionKind kind, const std::string &tracking_id,
    const AlarmSubject &subject) {
  switch (kind) {
  case OperationConditionKind::kExecutionFault:
    active_operation_faults_.insert_or_assign(tracking_id, subject);
    observed_operation_faults_.insert(tracking_id);
    break;
  case OperationConditionKind::kRuntimeCollision:
    active_runtime_collisions_.insert_or_assign(tracking_id, subject);
    observed_runtime_collisions_.insert(tracking_id);
    break;
  case OperationConditionKind::kProgramFailure:
    break; // AlarmLifecycleOwner is the sole Program condition registry.
  case OperationConditionKind::kStopFault:
    active_stop_faults_.insert_or_assign(tracking_id, subject);
    break;
  }
}

void HeadlessApiCore::ForgetRefusedOperationConditionLocked(
    const std::string_view definition_id, const std::string_view tracking_id) {
  refused_operation_conditions_.erase(
      RefusedConditionKey(definition_id, tracking_id));
  SynchronizeAlarmScopeConditionLocked();
}

void HeadlessApiCore::ReconsiderRefusedOperationConditionsLocked() {
  // Observation reconciliation for a changed active configuration: each
  // still-current refused activation is offered once more. A renewed refusal
  // is kept silently; it was already recorded when first observed.
  auto refused = std::exchange(refused_operation_conditions_, {});
  for (auto &[key, entry] : refused) {
    auto &cursor = entry.kind == OperationConditionKind::kProgramFailure
                       ? program_failure_adapter_ : operation_adapter_;
    if (entry.update.source_epoch != cursor.source_epoch ||
        cursor.source_sequence >= kMaximumSafeInteger) {
      continue;
    }
    entry.update.source_sequence = ++cursor.source_sequence;
    try {
      const auto mutation = alarm_owner_.Submit(entry.update);
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      TrackAdmittedOperationConditionLocked(entry.kind, entry.tracking_id,
                                            entry.update.subject);
    } catch (const OperatorAlarmError &) {
      refused_operation_conditions_.insert_or_assign(key, std::move(entry));
    }
  }
  SynchronizeAlarmScopeConditionLocked();
}

void HeadlessApiCore::SynchronizeAlarmScopeConditionLocked() {
  // One cell-scoped condition stands for every retained refusal, so it can
  // never itself be refused for its scope. It names the refused faults within
  // the details bound and returns to normal once none is retained (admitted
  // after reconsideration, superseded, cleared, or ended with their source).
  constexpr std::size_t kMaximumNamedRefusalBytes = 24U * 1024U;
  CanonicalValue::Array named;
  std::size_t named_bytes = 0U;
  for (const auto &[key, entry] : refused_operation_conditions_) {
    static_cast<void>(key);
    CanonicalValue refusal(CanonicalValue::Object{
        {"definition_id", CanonicalValue(entry.update.definition_id)},
        {"subject",
         CanonicalValue(CanonicalValue::Object{
             {"resource_ids",
              StringArrayValue(entry.update.subject.resource_ids)},
             {"subject_id", CanonicalValue(entry.update.subject.subject_id)},
             {"subject_kind",
              CanonicalValue(entry.update.subject.subject_kind)},
         })},
        {"tracking_id", CanonicalValue(entry.tracking_id)},
    });
    const auto bytes = content_identity::canonical_json(refusal).size() + 1U;
    if (named_bytes + bytes > kMaximumNamedRefusalBytes) {
      break;
    }
    named_bytes += bytes;
    named.push_back(std::move(refusal));
  }
  const bool active = !refused_operation_conditions_.empty();
  const bool truncated = named.size() < refused_operation_conditions_.size();
  // While refusals are retained, the configuration they were judged against
  // is correlated; with none retained, a configuration change alone is not a
  // new observation of this condition.
  CanonicalValue::Object correlations;
  if (active) {
    CanonicalValue registry(nullptr);
    if (alarm_registry_identity_.has_value()) {
      registry = CanonicalValue(CanonicalValue::Object{
          {"graph_revision", CanonicalValue(alarm_registry_identity_->second)},
          {"retained_bundle_id",
           CanonicalValue(alarm_registry_identity_->first)},
      });
    }
    correlations.emplace_back("active_configuration", std::move(registry));
  }
  const CanonicalValue details(CanonicalValue::Object{
      {"refused_condition_count",
       Number(refused_operation_conditions_.size(), "refused_condition_count")},
      {"refused_conditions", CanonicalValue(std::move(named))},
      {"refused_conditions_truncated", CanonicalValue(truncated)},
  });
  try {
    const auto mutation = SubmitAdapterUpdateLocked(
        alarm_scope_adapter_, "manyforge.runtime.alarm_scope_unavailable",
        AlarmSubject{"cell", config_.cell_id, {}}, active, details,
        CanonicalValue(std::move(correlations)));
    static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  } catch (const OperatorAlarmError &) {
    // The owner already counts the rejected update and names an exhausted
    // capacity as health. The unchanged cursor resubmits on the next change
    // of the retained refusals; the operation feed is never failed by it.
  }
}

std::optional<AlarmMutation> HeadlessApiCore::SubmitOperationConditionLocked(
    AlarmConditionUpdate update, const OperationConditionKind kind,
    std::string tracking_id) {
  if (update.active && !update.subject.resource_ids.empty()) {
    // Judge a new resource-scoped activation against the configuration
    // Manager publishes now, not against the last one a reader happened to
    // refresh: an Apply may have added or removed these resources. This is a
    // read of the existing publication per fault fact, not a poll.
    static_cast<void>(RefreshLocked());
  }
  const auto refused_key =
      RefusedConditionKey(update.definition_id, tracking_id);
  try {
    auto mutation = alarm_owner_.Submit(update);
    if (refused_operation_conditions_.erase(refused_key) != 0U) {
      SynchronizeAlarmScopeConditionLocked();
    }
    if (update.active) {
      TrackAdmittedOperationConditionLocked(kind, tracking_id, update.subject);
    }
    return mutation;
  } catch (const OperatorAlarmError &error) {
    if (error.code() != "operator_alarm_scope_inactive" &&
        error.code() != "operator_alarm_scope_invalid") {
      throw;
    }
    if (update.active) {
      // Bounded like the live alarm set; beyond it a refusal stays recorded
      // in history but is not reconsidered.
      if (refused_operation_conditions_.contains(refused_key) ||
          refused_operation_conditions_.size() < kMaximumLiveAlarmCapacity) {
        refused_operation_conditions_.insert_or_assign(
            refused_key,
            RefusedOperationCondition{update, kind, std::move(tracking_id)});
      }
    } else {
      refused_operation_conditions_.erase(refused_key);
    }
    CanonicalValue registry(nullptr);
    if (alarm_registry_identity_.has_value()) {
      registry = CanonicalValue(CanonicalValue::Object{
          {"graph_revision", CanonicalValue(alarm_registry_identity_->second)},
          {"retained_bundle_id",
           CanonicalValue(alarm_registry_identity_->first)},
      });
    }
    try {
      auto event = MakeHistorianEvent(
          "manyforge.alarm", "alarm_condition_rejected", "ERROR",
          update.observed_at_ns, update.observed_at_wall,
          CanonicalValue(CanonicalValue::Object{
              {"active", CanonicalValue(update.active)},
              {"active_configuration", std::move(registry)},
              {"code", CanonicalValue(error.code())},
              {"correlations", update.correlations},
              {"definition_id", CanonicalValue(update.definition_id)},
              {"message", CanonicalValue(std::string(error.what()))},
              {"schema_version",
               CanonicalValue("manyforge.alarm_condition_rejection.v1")},
              {"source_epoch", CanonicalValue(update.source_epoch)},
              {"source_id", CanonicalValue(update.source_id)},
              {"source_sequence",
               Number(update.source_sequence, "source_sequence")},
              {"subject",
               CanonicalValue(CanonicalValue::Object{
                   {"resource_ids",
                    StringArrayValue(update.subject.resource_ids)},
                   {"subject_id", CanonicalValue(update.subject.subject_id)},
                   {"subject_kind",
                    CanonicalValue(update.subject.subject_kind)},
               })},
          }));
      if (!historian_.TryEnqueue(std::move(event))) {
        alarm_owner_.MarkHealthUnavailable(
            std::string(kHistorianOwnerHealthCode),
            "historian delivery queue rejected an alarm scope rejection");
      }
    } catch (const std::exception &history_error) {
      historian_.ReportDeliveryFailure("historian_projection_failure",
                                       history_error.what());
    }
    SynchronizeAlarmScopeConditionLocked();
    return std::nullopt;
  }
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

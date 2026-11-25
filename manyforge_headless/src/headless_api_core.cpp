#include "manyforge_headless/headless_api_core.hpp"

#include <fcntl.h>
#include <sys/file.h>
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
#include <new>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/operation_member_id_json.hpp>
#include <manyforge_core/types/application_identity.hpp>
#include <manyforge_core/types/carried_object_lost.hpp>
#include <manyforge_core/types/component_state.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/error_catalog.generated.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/program_binding_outcome.hpp>
#include <manyforge_deployment/program_reference_facts.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
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
constexpr std::string_view kNodeEnvironmentProjectionIdentityDomain =
    "manyforge-node-environment-projection-semantic";
constexpr std::string_view kSourceModelProjectionIdentityDomain =
    "manyforge-source-model-projection-semantic";
constexpr std::string_view kAssistantModeIndexIdentityDomain =
    "manyforge-assistant-mode-index-semantic";
constexpr std::string_view kHistoryMissingRecordsAlarm =
    "manyforge.runtime.history_missing_records";
constexpr std::string_view kRuntimeCollisionAlarm = "manyforge.motion.runtime_collision";
constexpr std::string_view kRuntimeMonitorObservationLostAlarm =
    "manyforge.motion.runtime_monitor_observation_lost";
constexpr std::string_view kRuntimeMonitorChecksStoppedAlarm =
    "manyforge.motion.runtime_monitor_checks_stopped";
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
  Require(!config.install_root.empty() &&
              !config.artifact_root.empty() &&
              !config.node_catalog_path.empty() &&
              !config.node_environment_manifest_path.empty() &&
              !config.alarm_catalog_path.empty() &&
              !config.historian_directory.empty(),
          "headless_api_configuration_invalid",
          "install, artifact, node-catalog, node-environment "
          "manifest, alarm-catalog, and historian "
          "paths are required");
  Require(!config.cell_id.empty() && !config.owner_id.empty() &&
              !config.application_start_id.empty(),
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

std::uint64_t RequiredCounter(const CanonicalValue &root,
                              std::string_view name,
                              std::uint64_t minimum = 0U);

// A safe integer (at least `minimum`) of a previous-session fact.
bool PreviousSessionInteger(const CanonicalValue &value, const double minimum) {
  const auto *number = std::get_if<double>(&value.storage());
  return number != nullptr && std::isfinite(*number) && *number >= minimum &&
         std::floor(*number) == *number &&
         std::fabs(*number) <= static_cast<double>(kMaximumSafeInteger);
}

// UTC text ending in Z, as the launcher and the Manager write it.
bool PreviousSessionUtcText(const CanonicalValue &value) {
  const auto *text = std::get_if<std::string>(&value.storage());
  return text != nullptr && text->size() <= 64U && text->ends_with('Z') &&
         HistorianRecordedUtcNs(*text).has_value();
}

// The v4 report's `application_end` (schema manyforge.application_end.v1):
// how the previous run ended, as the Manager recorded it, or null.
void ValidateApplicationEnd(const CanonicalValue &end, const std::string &code) {
  if (end.is_null()) return;
  Require(end.is_object() && end.as_object().size() == 4U &&
              RequiredString(end, "schema_version") == "manyforge.application_end.v1",
          code, "previous-session application end is invalid");
  const auto ended_by = RequiredString(end, "ended_by");
  Require((ended_by == "process_exit" || ended_by == "stop_signal" ||
           ended_by == "shutdown_runtime" || ended_by == "manager_failure") &&
              PreviousSessionUtcText(RequiredMember(end, "ended_at_wall")),
          code, "previous-session application end is invalid");
  // A process exit names its process; every other ending names none.
  const auto &process = RequiredMember(end, "process");
  Require((ended_by == "process_exit") != process.is_null(), code,
          "previous-session application end is invalid");
  if (process.is_null()) return;
  Require(process.is_object() && process.as_object().size() == 5U, code,
          "previous-session ended process is invalid");
  const auto role = RequiredString(process, "role");
  const auto nullable_integer = [&](const std::string_view name) {
    const auto &value = RequiredMember(process, name);
    return value.is_null() || PreviousSessionInteger(value, -static_cast<double>(kMaximumSafeInteger));
  };
  Require(role.size() <= 256U && content_identity::is_utf8_nfc(role) &&
              PreviousSessionInteger(RequiredMember(process, "pid"), 1.0) &&
              nullable_integer("exit_status") && nullable_integer("signal") &&
              std::holds_alternative<bool>(RequiredMember(process, "core_dumped").storage()),
          code, "previous-session ended process is invalid");
}

// The v4 report's `archived_older_marker`: null, or the marker format of an
// older build's session the launcher archived at an upgrade.
bool PreviousSessionOlderMarker(const CanonicalValue &value) {
  if (value.is_null()) return true;
  constexpr std::string_view kPrefix = "manyforge.workbench_session.v";
  const auto *text = std::get_if<std::string>(&value.storage());
  return text != nullptr && text->size() <= 64U && text->size() > kPrefix.size() &&
         text->starts_with(kPrefix) &&
         std::all_of(text->begin() + static_cast<std::ptrdiff_t>(kPrefix.size()), text->end(),
                     [](char c) { return c >= '0' && c <= '9'; });
}

// The previous-session alarm's headline when the report names an older
// build's session archived at an upgrade (its marker format and archive),
// the process that ended the run (its role, pid, signal or exit status and
// time) or a Manager failure (its time).
std::optional<std::string> PreviousSessionEndMessage(const CanonicalValue &report) {
  constexpr std::string_view kKept =
      ". The cell started empty; nothing from the previous session was resumed."
      " Its records are kept; details in History.";
  const auto *older = report.find_object_member("archived_older_marker");
  if (older != nullptr && !older->is_null()) {
    return "ManyForge was upgraded: the previous session ran an older build (marker " +
           older->as_string() + ") and was archived to " +
           RequiredString(report, "archive_relative_path") +
           "; whether it closed correctly is unknown" + std::string(kKept);
  }
  const auto *end = report.find_object_member("application_end");
  if (end == nullptr || end->is_null()) return std::nullopt;
  if (RequiredString(*end, "ended_by") == "manager_failure") {
    return "ManyForge did not close correctly last time: the Manager failed at " +
           RequiredString(*end, "ended_at_wall") + std::string(kKept);
  }
  if (RequiredMember(*end, "process").is_null()) return std::nullopt;
  const auto &process = RequiredMember(*end, "process");
  const auto integer = [&](const std::string_view name) {
    return std::to_string(static_cast<long long>(
        std::get<double>(RequiredMember(process, name).storage())));
  };
  std::string how = "ended";
  if (!RequiredMember(process, "signal").is_null()) {
    how = "was ended by signal " + integer("signal");
  } else if (!RequiredMember(process, "exit_status").is_null()) {
    how = "exited with status " + integer("exit_status");
  }
  if (RequiredMember(process, "core_dumped").as_bool()) how += " (core dumped)";
  return "ManyForge did not close correctly last time: " +
         RequiredString(process, "role") + " (pid " + integer("pid") + ") " + how +
         " at " + RequiredString(*end, "ended_at_wall") + std::string(kKept);
}

HistorianEvent PreviousSessionReportEvent(const CanonicalValue &report) {
  constexpr auto code = "headless_previous_session_report_invalid";
  Require(report.is_object() &&
              (report.as_object().size() == 11U || report.as_object().size() == 15U),
          code, "previous-session report must contain its version's fixed fields");
  const auto text = [&](const std::string_view field) {
    const auto value = RequiredString(report, field);
    Require(value.size() <= 1024U && content_identity::is_utf8_nfc(value),
            code, "previous-session report text must be bounded NFC UTF-8");
    return value;
  };
  // v4 is current: v3, how the previous run ended (`application_end`), the
  // marker format of an older build's session archived at an upgrade
  // (`archived_older_marker`), its start and when the launcher wrote the
  // report.
  // A v2 report was written while the Scene still had a journal; one left
  // pending is filed as recorded, its "scene" family kept.
  const auto version = text("schema_version");
  const bool version2 = version == "manyforge.workbench_previous_session.v2";
  const bool version4 = version == "manyforge.workbench_previous_session.v4";
  Require(version == "manyforge.workbench_previous_session.v3" || version2 || version4,
          code, "previous-session report version is unsupported");
  Require(report.as_object().size() == (version4 ? 15U : 11U), code,
          "previous-session report must contain its version's fixed fields");
  if (version4) {
    ValidateApplicationEnd(RequiredMember(report, "application_end"), code);
    Require(PreviousSessionOlderMarker(RequiredMember(report, "archived_older_marker")), code,
            "previous-session older marker format is invalid");
  }
  const auto session_id = text("session_id");
  Require(session_id.size() == 32U &&
              std::all_of(session_id.begin(), session_id.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
              }) &&
              text("archive_relative_path") == "previous-runs/" + session_id,
          code, "previous-session archive identity is invalid");
  const auto shutdown = text("shutdown");
  const auto effects = text("effects");
  Require((shutdown == "clean" || shutdown == "unknown") &&
              (effects == "closed" || effects == "unresolved" || effects == "unknown") &&
              text("history_coverage") == "not_imported", code,
          "previous-session report classification is invalid");
  static_cast<void>(text("message"));
  const auto &families = RequiredMember(report, "journal_families");
  std::vector<std::string_view> expected{"program_cycle", "manager", "kernel_operation"};
  if (version2) {
    expected.insert(expected.begin() + 1, "scene");
  }
  Require(families.is_array() && families.as_array().size() == expected.size(),
          code, "previous-session report must name its version's journal families");
  for (std::size_t i = 0U; i < expected.size(); ++i) {
    const auto *family = std::get_if<std::string>(&families.as_array()[i].storage());
    Require(family != nullptr && *family == expected[i], code,
            "previous-session report journal families differ from the fixed inventory");
  }
  // The previous run's unfinished operations: the first 64 and their count,
  // each marked host_replaced; both null when the journal was unreadable.
  const auto &unfinished = RequiredMember(report, "unfinished_operations");
  const auto &unfinished_count =
      RequiredMember(report, "unfinished_operation_count");
  Require(unfinished.is_null() == unfinished_count.is_null(), code,
          "previous-session unfinished operations and count are paired");
  // The start the report describes, read from the previous run's Kernel
  // journal together with its operations: History files the report (and the
  // alarm rows that embed it) under that start.
  const auto &described_start = RequiredMember(report, "application_start_id");
  const auto *described_start_text = std::get_if<std::string>(&described_start.storage());
  Require(described_start.is_null() == unfinished.is_null() &&
              (described_start.is_null() ||
               (described_start_text != nullptr &&
                core::ParseApplicationStartId(*described_start_text).has_value())),
          code, "previous-session start identity is invalid or not paired with its operations");
  if (!unfinished.is_null()) {
    Require(unfinished.is_array() && unfinished.as_array().size() <= 64U, code,
            "previous-session report lists at most 64 unfinished operations");
    for (const auto &row : unfinished.as_array()) {
      Require(row.is_object() && row.as_object().size() == 2U &&
                  RequiredString(row, "disposition") == "host_replaced",
              code, "previous-session unfinished operation is invalid");
      // The previous start's operation number (contract 1.5).
      const auto *operation =
          std::get_if<double>(&RequiredMember(row, "operation_id").storage());
      Require(operation != nullptr && std::isfinite(*operation) &&
                  *operation >= 1.0 && std::floor(*operation) == *operation &&
                  *operation <= static_cast<double>(kMaximumSafeInteger),
              code, "previous-session unfinished operation is invalid");
    }
    Require(RequiredCounter(report, "unfinished_operation_count", 0U) >=
                unfinished.as_array().size(),
            code, "previous-session unfinished operation count is invalid");
  }
  // A v4 report is filed at the previous session's start (its UTC text; no
  // monotonic clock of that run applies), the time unknown when the launcher
  // could not read it; earlier versions keep the unknown time. Both times are
  // part of the report file, so a repeated filing is an exact duplicate.
  std::string occurred_at_wall = "unknown";
  if (version4) {
    const auto utc = [](const CanonicalValue &value) {
      const auto *wall = std::get_if<std::string>(&value.storage());
      return wall != nullptr && wall->size() <= 64U &&
             HistorianRecordedUtcNs(*wall).has_value();
    };
    const auto &started = RequiredMember(report, "started_at_wall");
    Require((started.is_null() || utc(started)) &&
                utc(RequiredMember(report, "reported_at_wall")),
            code, "previous-session report times are invalid");
    if (!started.is_null()) occurred_at_wall = started.as_string();
  }
  return MakeHistorianEvent("manyforge.workbench", "previous_session_report", "INFO", 0,
                            occurred_at_wall, report);
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
                              const std::uint64_t minimum) {
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

// A request key on the wire: exactly 32 lowercase hex (contract 1.4).
bool IsRequestKeyText(const std::string_view text) {
  return text.size() == 32U &&
         std::ranges::all_of(text, [](const char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
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
    transition = std::to_string(RequiredCounter(row, "apply_id", 1U)) + ":" +
                 std::to_string(generation);
  }
  return transition;
}


bool IsOperationFaultState(const std::string_view state) noexcept {
  return state == "failed" || state == "interrupted" ||
         state == "intervention_required";
}

bool IsNormalOperationTerminal(const std::string_view state) noexcept {
  return state == "completed" || state == "stopped" ||
         state == "cancelled_pre_dispatch";
}

void ValidateInterruptRuleStateProjection(const CanonicalValue &payload) {
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
  const auto state = RequiredString(payload, "state");
  if (state == "unknown")
    closed(payload, {"catalog_generation",
                     "active_publication_generation", "retained_bundle_id",
                     "graph_revision", "rule_id", "state",
                     "triggering_acquisition", "unavailable_reason"});
  else
    closed(payload, {"catalog_generation",
                     "active_publication_generation", "retained_bundle_id",
                     "graph_revision", "rule_id", "state",
                     "triggering_acquisition"});
  const core::IoRuleStateContextDTO context{
      counter(payload, "catalog_generation"),
      core::ActiveBundlePublicationGeneration(
          RequiredCounter(payload, "active_publication_generation", 1U)),
      core::RetainedBundleId(RequiredString(payload, "retained_bundle_id")),
      core::GraphRevision(RequiredString(payload, "graph_revision"))};
  Require(state == "inactive" || state == "clear" || state == "tripped" ||
              state == "unknown",
          code, "rule-state projection has an unknown state");
  const auto kind = state == "inactive" ? core::IoGlobalRuleState::kInactive
                  : state == "clear" ? core::IoGlobalRuleState::kClear
                  : state == "tripped" ? core::IoGlobalRuleState::kTripped
                                       : core::IoGlobalRuleState::kUnknown;
  core::IoRuleStateDTO rule{
      core::InterruptPolicyId(RequiredString(payload, "rule_id")), kind,
      std::nullopt};
  if (state == "unknown") {
    const auto &reason = RequiredMember(payload, "unavailable_reason");
    closed(reason, {"code", "message", "details"});
    const auto &message = RequiredMember(reason, "message");
    Require(message.is_string(), code, "unavailable reason message must be text");
    core::ErrorDTO error{RequiredString(reason, "code"), message.as_string(), {}};
    const auto &details = RequiredMember(reason, "details");
    Require(details.is_object(), code, "unavailable reason details must be an object");
    for (const auto &[key, value] : details.as_object()) {
      Require(value.is_string(), code, "unavailable reason detail must be text");
      error.details.emplace(key, value.as_string());
    }
    rule.unavailable_reason = std::move(error);
  }
  const auto &acquired = RequiredMember(payload, "triggering_acquisition");
  if (!acquired.is_null()) {
    closed(acquired, {"source_id", "source_epoch", "source_sequence",
                      "source_cursor"});
    const auto &cursor = RequiredMember(acquired, "source_cursor");
    closed(cursor, {"catalog_generation", "source_id", "record_sequence"});
    rule.triggering_acquisition = core::IoRuleAcquisitionKeyDTO{
        core::InterruptSourceId(RequiredString(acquired, "source_id")),
        core::IoSourceIncarnation(RequiredString(acquired, "source_epoch")),
        counter(acquired, "source_sequence"),
        {counter(cursor, "catalog_generation"),
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
  if (record_kind == "active_tcp_changed") {
    return "active_tcp_changed";
  }
  if (record_kind == "operation_accepted") {
    return "operation_accepted";
  }
  if (record_kind == "stop_event_accepted") {
    return "stop_event_accepted";
  }
  if (record_kind == "stop_event_completed") {
    return "stop_event_completed";
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
    "component_id",     "input_problem",  "path",
    "reason",           "reference",      "reference_id",   "reference_type",
    "required_action",  "retryable",      "rule_id",
    "source_kind",      "source_reference"};

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
      if (key == "reference") {
        const auto *kind_value = details.find_object_member("reference_type");
        const auto *kind = kind_value == nullptr ? nullptr : std::get_if<std::string>(&kind_value->storage());
        const std::string_view member = kind == nullptr ? std::string_view{} :
            *kind == "target" ? "command_target_id" :
            *kind == "signal" ? "signal_name" :
            *kind == "frame" ? "frame_id" :
            *kind == "collision_body" ? "collision_body_id" : "";
        Require(!member.empty() && value.is_object() && value.as_object().size() == 2U,
                code, "Failure reference shape is invalid");
        const auto *component_value = value.find_object_member("component_id");
        const auto *identity_value = value.find_object_member(member);
        const auto *component = component_value == nullptr ? nullptr : std::get_if<std::string>(&component_value->storage());
        const auto *identity = identity_value == nullptr ? nullptr : std::get_if<std::string>(&identity_value->storage());
        const auto portable = [](const std::string &text, bool first_lower) {
          if (text.empty() || text.size() > 128U) return false;
          const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
          const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
          const auto digit = [](char c) { return c >= '0' && c <= '9'; };
          if (!(lower(text.front()) || (!first_lower && (upper(text.front()) || digit(text.front()))))) return false;
          return std::all_of(text.begin() + 1, text.end(), [&](char c) {
            return lower(c) || digit(c) || (!first_lower && upper(c)) || c == '_' || c == '.' || c == '-';
          });
        };
        Require(component != nullptr && identity != nullptr && portable(*component, true) &&
                    (member == "signal_name"
                         ? !identity->empty() && identity->size() <= 512U && identity->find('\0') == std::string::npos
                         : portable(*identity, false)),
                code, "Failure reference identity is invalid");
        continue;
      }
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
      const std::filesystem::path &historian_directory) {
    auto lock_path = SqliteEventHistoryStore::DatabasePath(historian_directory);
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
          config_.historian_directory)),
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
          config_.historian_directory, config_.application_start_id,
          config_.historian_record_retention)),
      historian_(history_store_, config_.historian_queue_capacity),
      manager_status_(config_.cell_id,
                      config_.freshness_policy.manager_silence_ns,
                      clocks_.monotonic_time_ns),
      runtime_aggregator_(manager_status_, config_.owner_id + ".projection",
                          config_.application_start_id,
                          clocks_.monotonic_time_ns),
      alarm_owner_(OperatorAlarmCatalog::Load(config_.alarm_catalog_path),
                   config_.cell_id, config_.owner_id + ".alarm",
                   config_.application_start_id,
                   // The host boot the Kernel checks attention against.
                   manyforge::core::ReadHostBootStamp().boot_id,
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
      component_fault_adapter_{
          config_.owner_id + ".component-fault-adapter", "runtime_health",
          "manyforge.alarm_adapter.runtime_health_condition",
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
      workbench_session_adapter_{config_.owner_id + ".workbench-session-adapter",
                                 "runtime_health",
                                 "manyforge.alarm_adapter.workbench_session_condition",
                                 {}, 0U, 0U, {}, std::nullopt},
      history_loss_adapter_{config_.owner_id + ".history-loss-adapter",
                            "runtime_health",
                            "manyforge.alarm_adapter.runtime_health_condition",
                            {}, 0U, 0U, {}, std::nullopt},
      disk_records_adapter_{config_.owner_id + ".disk-records-adapter",
                            "runtime_health",
                            "manyforge.alarm_adapter.runtime_health_condition",
                            {}, 0U, 0U, {}, std::nullopt} {
  RegisterAdapterEpochLocked(runtime_health_adapter_,
                             config_.application_start_id + ".runtime-health");
  // The retained refusals are this owner's own state and start empty, so the
  // alarm-scope source is complete from its first epoch.
  RegisterAdapterEpochLocked(alarm_scope_adapter_,
                             config_.application_start_id + ".alarm-scope");
  alarm_owner_.CompleteSourceSnapshot(alarm_scope_adapter_.source_id,
                                      alarm_scope_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
  // History's loss state is this process's own and starts without loss.
  RegisterAdapterEpochLocked(history_loss_adapter_,
                             config_.application_start_id + ".history-loss");
  alarm_owner_.CompleteSourceSnapshot(history_loss_adapter_.source_id,
                                      history_loss_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
  // The disk check is this process's own and starts with the condition absent.
  RegisterAdapterEpochLocked(disk_records_adapter_,
                             config_.application_start_id + ".disk-records");
  alarm_owner_.CompleteSourceSnapshot(disk_records_adapter_.source_id,
                                      disk_records_adapter_.source_epoch,
                                      AlarmConditionQuality::kGood);
  // One clock anchor per start: Headless's event clock, the host's raw
  // CLOCK_MONOTONIC (the Kernel's and Runner's clock) and UTC, read back to
  // back, so this start's rows can be ordered against theirs.
  const auto owner_monotonic_ns = clocks_.monotonic_time_ns();
  timespec raw{};
  static_cast<void>(::clock_gettime(CLOCK_MONOTONIC, &raw));
  const auto wall = clocks_.wall_time_text();
  const auto clock_owner = "headless:" + config_.application_start_id;
  static_cast<void>(historian_.TryEnqueue(MakeHistorianEvent(
      "manyforge.headless", "clock_anchor", "INFO", owner_monotonic_ns, wall,
      CanonicalValue(CanonicalValue::Object{
          {"clock_owner", CanonicalValue(clock_owner)},
          {"message", CanonicalValue("Clock anchor of " + clock_owner)},
          {"owner_monotonic_ns",
           content_identity::nonnegative_int64_value(owner_monotonic_ns)},
          {"raw_monotonic_ns",
           content_identity::nonnegative_int64_value(
               static_cast<std::int64_t>(raw.tv_sec) * 1000000000LL +
               raw.tv_nsec)},
          {"utc_wall", CanonicalValue(wall)}}))));
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
    Require(condition.is_object() && condition.as_object().size() == 9U, code,
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
                "manyforge.program_failure_condition.v2",
            code, "Program failure observation version is unsupported");
    // The runner's conditions are one source for the whole start (its
    // owner is the role `program_runner` with this start's `A`, 1.16.4).
    const auto &epoch = config_.application_start_id;
    const auto run = RequiredCounter(condition, "program_run_id", 1U);
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
                operation.is_object() && operation.as_object().size() == 2U &&
                failure.is_object() && failure.as_object().size() == 2U,
            code, "Program failure provenance has unsupported fields");
    const auto node = text(occurrence, "node_instance_id", 128U);
    const auto attempt = RequiredCounter(occurrence, "node_attempt_id", 1U);
    Require(node.find('\0') == std::string::npos, code,
            "Program node scope is invalid");
    const auto facing_revision =
        RequiredCounter(occurrence, "operator_facing_run_revision", 1U);
    Require(run_revision >= facing_revision, code,
            "Program failure resolution does not follow its occurrence");
    const auto operation_id = RequiredCounter(operation, "operation_id", 1U);
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
        {"program_run_id", Number(run, "program_run_id")},
        {"occurrence", occurrence},
        {"operation", operation}});
    const auto tracking_id = content_identity::framed_sha256(
        "manyforge-program-operation-failure", "v1", origin);
    const AlarmSubject subject{"program_operation", tracking_id, resources};
    const CanonicalValue details(
        CanonicalValue::Object{{"failure", failure},
                               {"node_instance_id", CanonicalValue(node)},
                               {"node_attempt_id",
                                Number(attempt, "node_attempt_id")}});
    CanonicalValue correlations(CanonicalValue::Object{
        {"program_run_id", Number(run, "program_run_id")},
        {"operation_id", Number(operation_id, "operation_id")},
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
      program_failure_run_id_ = 0U;
    }
    Require(control_revision != program_failure_control_revision_ ||
                program_failure_run_id_ == 0U ||
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
    CanonicalValue current_runtime_faults) {
  constexpr auto code = "headless_runtime_fault_invalid";
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
    if (std::string_view(kind) == "operation_journal_write_fault") {
      closed(fact, {"write_kind", "error", "operation_id",
                    "operation_revision", "request_key", "program_run_id",
                    "execution_scope", "queue_full"});
      static_cast<void>(text(fact, "write_kind", 64U));
      // OD-215: the queue's "full" level, read by the disk check from the
      // event pump; not part of this alarm's facts.
      Require(RequiredMember(fact, "queue_full").is_bool(), code,
              "runtime fault queue level is invalid");
      const auto &error = RequiredMember(fact, "error");
      closed(error, {"code", "message", "details"});
      static_cast<void>(text(error, "code", 256U));
      const auto message = RequiredString(error, "message");
      Require(message.size() <= 1024U && content_identity::is_utf8_nfc(message),
              code, "runtime fault error message is invalid");
      Require(RequiredMember(error, "details").is_object() &&
                  RequiredMember(error, "details").as_object().empty(),
              code, "runtime fault error details must be empty");
      for (const auto key : {"operation_id", "program_run_id"})
        if (!RequiredMember(fact, key).is_null())
          static_cast<void>(RequiredCounter(fact, key, 1U));
      if (!RequiredMember(fact, "request_key").is_null())
        Require(IsRequestKeyText(RequiredString(fact, "request_key")), code,
                "runtime fault request key is invalid");
      if (!RequiredMember(fact, "operation_revision").is_null())
        static_cast<void>(RequiredCounter(fact, "operation_revision", 1U));
      const auto &scope = RequiredMember(fact, "execution_scope");
      if (!scope.is_null()) {
        closed(scope, {"node_instance_id", "node_attempt_id"});
        static_cast<void>(text(scope, "node_instance_id", 128U));
        static_cast<void>(RequiredCounter(scope, "node_attempt_id", 1U));
      }
    } else if (std::string_view(kind) == "kernel_internal_fault") {
      // Its containment is a Halt, which cannot be refused (stop state model).
      closed(fact, {"reason_code", "later_count"});
      static_cast<void>(text(fact, "reason_code", 128U));
      static_cast<void>(RequiredCounter(fact, "later_count", 0U));
    } else {
      closed(fact, {"application_start_id", "established_cycle",
                    "failure_predicate", "message", "fault_code"});
      static_cast<void>(text(fact, "application_start_id", 16U));
      const auto predicate = text(fact, "failure_predicate", 128U);
      Require(predicate == "program_runner_liveness_probe" ||
                  predicate == "program_runner_liveness_renewal" ||
                  predicate == "program_runner_reconciliation_renewal" ||
                  predicate == "program_runner_fault",
              code, "runtime fault is not a Runner contact failure");
      // Every Runner loss is typed: a Runner fault carries its own error
      // code, a lost contact `program_runner_contact_lost` (OD-296 (a)).
      const auto fault_code = text(fact, "fault_code", 128U);
      Require((predicate == "program_runner_fault") !=
                  (fault_code == manyforge::core::kProgramRunnerContactLostCode),
              code, "Runner loss carries the wrong typed code");
      const auto message = RequiredString(fact, "message");
      Require(message.size() <= 1024U && content_identity::is_utf8_nfc(message),
              code, "Runner contact error message is invalid");
      const auto &cycle = RequiredMember(fact, "established_cycle");
      closed(cycle, {"cycle_generation", "state", "program_run_id",
                     "cycle_claim_generation",
                     "terminal_or_settlement_reference", "accepted_reset"});
      static_cast<void>(RequiredCounter(cycle, "program_run_id", 1U));
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
    // The Runner loss and the journal write fault follow the Kernel's current
    // fact: they clear when it is null. The internal fault stays raised.
    if (fact.is_null() && active_runtime_fault_kinds_.contains(kind) &&
        std::string_view(kind) != "kernel_internal_fault") {
      const auto mutation = SubmitAdapterUpdateLocked(
          runtime_fault_adapter_, "manyforge.runtime." + std::string(kind),
          AlarmSubject{"cell", config_.cell_id, {}}, false,
          CanonicalValue(CanonicalValue::Object{}),
          CanonicalValue(CanonicalValue::Object{}));
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      active_runtime_fault_kinds_.erase(std::string(kind));
      changed = true;
      alarm_owner_.CompleteSourceSnapshot(runtime_fault_adapter_.source_id,
                                          runtime_fault_adapter_.source_epoch,
                                          AlarmConditionQuality::kGood);
      continue;
    }
    if (fact.is_null() || active_runtime_fault_kinds_.contains(kind))
      continue;
    RegisterAdapterEpochLocked(runtime_fault_adapter_,
                               config_.application_start_id + ".runtime-faults");
    // Keep exact attribution while respecting the existing 32 KiB details
    // and correlations limits, including six-byte JSON control escapes.
    // History files the row under this start (its own start column).
    CanonicalValue::Object details;
    CanonicalValue::Object correlations;
    for (const auto &[key, value] : fact.as_object()) {
      if (std::string_view(kind) == "operation_journal_write_fault" &&
          key == "queue_full")
        continue; // a level the disk check reads, not this alarm's fact
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

CanonicalValue HeadlessApiCore::ObserveComponentFaults(CanonicalValue faults) {
  constexpr auto code = "headless_component_fault_invalid";
  constexpr std::string_view kComponentFaultAlarm =
      "manyforge.runtime.component_fault";
  const auto text = [&](const CanonicalValue &value, const std::string_view key,
                        const bool allow_empty) {
    const auto *member = value.find_object_member(key);
    Require(member != nullptr && member->is_string(), code,
            "component fault text is missing");
    const auto &result = member->as_string();
    Require((allow_empty || !result.empty()) && result.size() <= 1024U &&
                content_identity::is_utf8_nfc(result),
            code, "component fault text is invalid");
    return result;
  };
  // Validate the complete list before submitting any condition.
  Require(faults.is_array(), code, "component faults must be a list");
  std::map<std::string, const CanonicalValue *> current;
  for (const auto &entry : faults.as_array()) {
    Require(entry.is_object() && entry.as_object().size() == 2U, code,
            "component fault entry has an invalid closed shape");
    const auto component_id = text(entry, "component_id", false);
    const auto *fault = entry.find_object_member("fault");
    Require(fault != nullptr && fault->is_object(), code,
            "component fault entry has no fault");
    std::size_t fields = 4U;
    Require(manyforge::core::parse_fault_code(text(*fault, "code", false))
                .has_value(),
            code, "component fault code is unknown");
    static_cast<void>(text(*fault, "message", true));
    const auto *clearable = fault->find_object_member("clearable");
    Require(clearable != nullptr && clearable->is_bool(), code,
            "component fault clearable flag is invalid");
    Require(manyforge::core::parse_fault_operator_action(
                text(*fault, "operator_action", false))
                .has_value(),
            code, "component fault operator action is unknown");
    for (const auto key : {"vendor_code", "vendor_text"})
      if (fault->find_object_member(key) != nullptr) {
        static_cast<void>(text(*fault, key, false));
        ++fields;
      }
    if (const auto *concerns = fault->find_object_member("concerns")) {
      Require(concerns->is_array() && !concerns->as_array().empty(), code,
              "component fault concerns are invalid");
      for (const auto &member : concerns->as_array())
        Require(member.is_string() && !member.as_string().empty() &&
                    member.as_string().size() <= 1024U &&
                    content_identity::is_utf8_nfc(member.as_string()),
                code, "component fault concern is invalid");
      ++fields;
    }
    Require(fault->as_object().size() == fields, code,
            "component fault has an invalid closed shape");
    Require(current.emplace(component_id, fault).second, code,
            "component fault lists a component twice");
  }
  const auto subject = [](const std::string &component_id) {
    return AlarmSubject{"component", component_id, {}};
  };
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  bool changed = false;
  // One alarm per component, holding the code it was raised with. A fault
  // that ended, or whose code changed, returns its occurrence to normal; a
  // changed code then raises a new occurrence below. An unchanged fault
  // submits nothing.
  for (auto alarmed = active_component_faults_.begin();
       alarmed != active_component_faults_.end();) {
    const auto found = current.find(alarmed->first);
    if (found != current.end() &&
        found->second->find_object_member("code")->as_string() ==
            alarmed->second) {
      ++alarmed;
      continue;
    }
    try {
      const auto mutation = SubmitAdapterUpdateLocked(
          component_fault_adapter_, std::string(kComponentFaultAlarm),
          subject(alarmed->first), false,
          CanonicalValue(CanonicalValue::Object{
              {"reason", CanonicalValue("fault_ended")}}),
          CanonicalValue(CanonicalValue::Object{}));
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      changed = changed || mutation.changed;
      alarmed = active_component_faults_.erase(alarmed);
    } catch (const OperatorAlarmError &) {
      // The owner counts the refused update and names its health; the
      // unchanged entry is submitted again at the next observation.
      ++alarmed;
    }
  }
  for (const auto &[component_id, fault] : current) {
    if (active_component_faults_.contains(component_id))
      continue;
    RegisterAdapterEpochLocked(
        component_fault_adapter_,
        config_.application_start_id + ".component-faults");
    // The fault's own code and message are the headline
    // (`failure.message`); the rest names its action and vendor facts.
    CanonicalValue::Object details{
        {"clearable", RequiredMember(*fault, "clearable")},
        {"failure",
         CanonicalValue(CanonicalValue::Object{
             {"code", RequiredMember(*fault, "code")},
             {"message", RequiredMember(*fault, "message")},
         })},
        {"operator_action", RequiredMember(*fault, "operator_action")},
    };
    for (const auto key : {"concerns", "vendor_code", "vendor_text"})
      if (const auto *member = fault->find_object_member(key))
        details.emplace_back(key, *member);
    try {
      const auto mutation = SubmitAdapterUpdateLocked(
          component_fault_adapter_, std::string(kComponentFaultAlarm),
          subject(component_id), true, CanonicalValue(std::move(details)),
          CanonicalValue(CanonicalValue::Object{}));
      static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
      changed = changed || mutation.changed;
      active_component_faults_.emplace(
          component_id, fault->find_object_member("code")->as_string());
    } catch (const OperatorAlarmError &) {
      // As above: not alarmed yet, submitted again at the next observation.
    }
  }
  if (!component_fault_adapter_.source_epoch.empty())
    alarm_owner_.CompleteSourceSnapshot(component_fault_adapter_.source_id,
                                        component_fault_adapter_.source_epoch,
                                        AlarmConditionQuality::kGood);
  return CanonicalValue(
      CanonicalValue::Object{{"status", CanonicalValue("observed")},
                             {"changed", CanonicalValue(changed)}});
}

CanonicalValue HeadlessApiCore::HistoryDiskSignals() {
  // Deliberately outside `mutex_`: the disk check's thread must not wait on
  // a core call that may be writing to a stalled disk. History's own lock is
  // never held across a write.
  const auto signals = historian_.DiskSignals();
  return CanonicalValue(CanonicalValue::Object{
      {"queue_full", CanonicalValue(signals.queue_full)},
      {"storage_full", CanonicalValue(signals.storage_full)},
  });
}

CanonicalValue
HeadlessApiCore::ObserveDiskRecordsCondition(CanonicalValue condition) {
  constexpr auto code = "headless_disk_condition_invalid";
  const auto counter = [&](const CanonicalValue &value, std::string_view key,
                           bool nullable) {
    const auto &member = RequiredMember(value, key);
    if (nullable && member.is_null())
      return;
    static_cast<void>(RequiredCounter(value, key, 0U));
  };
  const auto text = [&](const CanonicalValue &value, std::string_view key,
                        std::size_t maximum) {
    const auto result = RequiredString(value, key);
    Require(!result.empty() && result.size() <= maximum &&
                content_identity::is_utf8_nfc(result),
            code, "disk condition text is invalid");
    return result;
  };
  Require(condition.is_object() && condition.as_object().size() == 2U &&
              RequiredMember(condition, "active").is_bool(),
          code, "disk condition has an invalid closed shape");
  const bool active = RequiredMember(condition, "active").as_bool();
  const auto &details = RequiredMember(condition, "details");
  Require(details.is_object() &&
              details.as_object().size() == (active ? 7U : 0U),
          code, "disk condition details have an invalid closed shape");
  if (active) {
    // The facts fixed at entry (OD-215); `failure.message` is the headline.
    const auto cause = text(details, "cause", 16U);
    Require(cause == "below_floor" || cause == "queue_full", code,
            "disk condition cause is unsupported");
    static_cast<void>(text(details, "path", 4096U));
    counter(details, "free_bytes", true);
    counter(details, "floor_bytes", false);
    counter(details, "margin_bytes", false);
    const auto &writer = RequiredMember(details, "writer");
    Require(cause == "queue_full"
                ? writer.is_string() &&
                      (writer.as_string() == "operation_journal" ||
                       writer.as_string() == "event_history")
                : writer.is_null(),
            code, "disk condition writer is invalid");
    // The panel's headline (`failure.message`), composed from these facts.
    const auto &failure = RequiredMember(details, "failure");
    Require(failure.is_object() && failure.as_object().size() == 1U, code,
            "disk condition failure has an invalid closed shape");
    static_cast<void>(text(failure, "message", 4096U));
  }
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto mutation = SubmitAdapterUpdateLocked(
      disk_records_adapter_, std::string(kDiskCannotKeepRecordsAlarm),
      AlarmSubject{"cell", config_.cell_id, {}}, active, details,
      CanonicalValue(CanonicalValue::Object{}));
  static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  return CanonicalValue(
      CanonicalValue::Object{{"status", CanonicalValue("observed")},
                             {"changed", CanonicalValue(mutation.changed)}});
}

CanonicalValue HeadlessApiCore::AppendWorkbenchPreviousSessionReport(CanonicalValue report) {
  auto event = PreviousSessionReportEvent(report);
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    store = history_store_;
  }
  // The commit precedes this receipt. Duplicate status also
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
                               config_.application_start_id + ".workbench-session");
    const AlarmSubject subject{"workbench_session", identity, {}};
    CanonicalValue::Object detail_fields{
        {"previous_session", report},
        {"failure_time", CanonicalValue(nullptr)},
        {"failure_time_status", CanonicalValue("unavailable")},
    };
    // The headline names the process that ended the run when it is known.
    if (const auto ended = PreviousSessionEndMessage(report)) {
      detail_fields.emplace_back(
          "failure", CanonicalValue(CanonicalValue::Object{{"message", CanonicalValue(*ended)}}));
    }
    const CanonicalValue details(std::move(detail_fields));
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
  // The Manager's Apply number (contract 1.16.1).
  const auto apply_id = RequiredCounter(transaction, "apply_id", 1U);
  const auto apply_generation =
      RequiredCounter(transaction, "apply_generation", 1U);
  const auto &records = RequiredArray(RequiredMember(manager_payload, "apply"),
                                      "records");
  const CanonicalValue *record = nullptr;
  for (const auto &candidate : records) {
    if (RequiredCounter(candidate, "apply_id", 1U) == apply_id &&
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
  // Only the active configuration's record carries its preparation receipt
  // (OD-281); the other records carry null.
  const auto &prepared = RequiredMember(*record, "prepared_activation");
  Require(prepared.is_object(), "program_applied_selection_invalid",
          "the active Apply record carries no preparation receipt");
  // Development cutover: an active Apply recorded before the removal of
  // launch-time parameter overrides is refused, never reinterpreted.
  Require(RequiredString(prepared, "schema_version") ==
              deployment::kPreparedActivationInputSchemaVersion,
          "program_applied_selection_invalid",
          "the active Apply used an unsupported prepared activation version; "
          "stop ManyForge and start it with fresh runtime state");
  Require(RequiredString(prepared, "retained_bundle_id") ==
                  RequiredString(active, "retained_bundle_id") &&
              RequiredString(prepared, "graph_revision") ==
                  RequiredString(active, "graph_revision"),
          "program_applied_selection_invalid",
          "active Manager publication and prepared activation disagree");
  return *record;
}

namespace {

/// The typed Program outcome of the active prepared activation: the
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

/// The exact prepared origin is retained on every committed Apply, the empty
/// cell (Close, New) included.
const CanonicalValue &ReadPreparedOrigin(const CanonicalValue &prepared) {
  const auto &origin = RequiredMember(prepared, "origin");
  try {
    static_cast<void>(deployment::parse_prepared_activation_origin(origin));
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
  for (const auto &row : outcome.unresolved_targets) {
    message += " target '" + row.target.command_target_id.value() +
               "' (component '" + row.target.component_id.value() + "')";
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
  // it is never replaced by another binding.
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
  const auto program_revision =
      core::ProgramRevision(RequiredString(prepared, "program_revision"));
  const auto binding_id = *outcome.program_binding_id;
  const auto program =
      program_repository_.get_verified(program_revision, *bundle);
  std::shared_ptr<const deployment::ProgramBinding> binding;
  try {
    const deployment::FilesystemVerifiedCatalogArtifactSource catalog(config_.artifact_root);
    binding = binding_repository_.get_verified(binding_id, *bundle, model_map, catalog);
  } catch (const deployment::ProgramBindingError &) {
    CoreFail("program_applied_selection_invalid", "active Program binding or catalog is unavailable");
  }
  Require(program != nullptr, "program_applied_selection_invalid",
          "active Program is unavailable");
  Require(binding != nullptr, "program_applied_selection_invalid",
          "active Program binding is unavailable");
  Require(binding->value().program_revision == program_revision,
          "program_applied_selection_invalid",
          "active Program and binding are divergent");

  // Keep one facts capture for the active Apply, under the existing core lock.
  const auto record_digest = RequiredString(record, "intent_digest");
  const bool same_facts = applied_program_frame_facts_ &&
      applied_program_frame_facts_->first == record_digest &&
      RequiredString(applied_program_frame_facts_->second, "program_binding_id") == binding_id.value() &&
      RequiredString(applied_program_frame_facts_->second, "program_revision") == program_revision.value() &&
      RequiredString(applied_program_frame_facts_->second, "retained_bundle_id") == active_bundle_id &&
      RequiredString(applied_program_frame_facts_->second, "graph_revision") == active_graph_revision;
  if (!same_facts) {
    applied_program_frame_facts_ = std::make_pair(
        record_digest, deployment::CaptureProgramFrameFacts(
            program->value(), model_map, record_digest, binding_id.value(),
            program_revision.value(), active_bundle_id, active_graph_revision));
  }
  const auto &program_frame_facts = applied_program_frame_facts_->second;

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

  static_cast<void>(RequiredSection(runtime.root, "logical_scene"));
  static_cast<void>(RequiredSection(runtime.root, "scene_runtime"));
  // An identity is the pair (role, A); nothing is composed (contract 1.2).
  const auto owner_identity = [this](const std::string_view role) {
    return CanonicalValue(CanonicalValue::Object{
        {"application_start_id", CanonicalValue(config_.application_start_id)},
        {"role", CanonicalValue(std::string(role))},
    });
  };

  return CanonicalValue(CanonicalValue::Object{
      {"assistant_mode", RequiredMember(prepared, "assistant_mode")},
      {"assistant_projection", std::move(assistant_projection)},
      {"baseline_program_binding_id", CanonicalValue(binding_id.value())},
      {"deployment_revision", RequiredMember(prepared, "deployment_revision")},
      {"graph_revision", CanonicalValue(active_graph_revision)},
      {"node_environment_digest",
       RequiredMember(prepared, "node_environment_digest")},
      {"owner_identities",
       CanonicalValue(CanonicalValue::Array{
           owner_identity("headless_projection"),
           owner_identity("logical_scene"),
           owner_identity("manager"),
           owner_identity("scene_runtime"),
       })},
      {"preparation_digest",
       RequiredMember(prepared, "prepared_activation_id")},
      {"program", program->normalized_program_root()},
      {"program_binding", binding->normalized_root()},
      {"program_frame_facts", program_frame_facts},
      {"program_binding_id", CanonicalValue(binding_id.value())},
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

CanonicalValue HeadlessApiCore::LookupComponentFrame(const CanonicalValue &request) {
  constexpr auto invalid = "component_frame_lookup_invalid";
  Require(request.is_object() && request.as_object().size() == 4U, invalid,
          "component frame lookup has an invalid closed shape");
  for (const auto field : {"schema_version", "retained_bundle_id", "graph_revision", "frame"})
    Require(request.find_object_member(field) != nullptr, invalid, "component frame lookup field is missing");
  const auto string_field = [&](const CanonicalValue &object, std::string_view field) {
    const auto *value = object.find_object_member(field);
    const auto *text = value == nullptr ? nullptr : std::get_if<std::string>(&value->storage());
    Require(text != nullptr && !text->empty(), invalid, "component frame lookup field must be text");
    return *text;
  };
  Require(string_field(request, "schema_version") == "manyforge.component_frame_lookup.v1", invalid,
          "component frame lookup version is unsupported");
  const auto bundle_id = string_field(request, "retained_bundle_id");
  const auto graph_revision = string_field(request, "graph_revision");
  const auto &frame = *request.find_object_member("frame");
  Require(frame.is_object() && frame.as_object().size() == 2U &&
              frame.find_object_member("component_id") != nullptr && frame.find_object_member("frame_id") != nullptr,
          invalid, "component frame reference has an invalid closed shape");
  const auto component_id = string_field(frame, "component_id");
  const auto frame_id = string_field(frame, "frame_id");
  const auto valid_id = [](const std::string &id, bool component) {
    if (id.empty() || id.size() > 128U) return false;
    const auto lower = [](char c) { return c >= 'a' && c <= 'z'; };
    const auto upper = [](char c) { return c >= 'A' && c <= 'Z'; };
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    return (lower(id.front()) || (!component && (upper(id.front()) || digit(id.front())))) &&
           std::all_of(id.begin() + 1, id.end(), [&](char c) {
             return lower(c) || digit(c) || (!component && upper(c)) || c == '.' || c == '_' || c == '-';
           });
  };
  Require(valid_id(component_id, true) && valid_id(frame_id, false), invalid,
          "component frame reference identity is invalid");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  const auto runtime = RefreshLocked();
  const auto &record = ActiveAppliedRecordLocked(runtime);
  const auto &prepared = RequiredMember(record, "prepared_activation");
  Require(RequiredString(prepared, "retained_bundle_id") == bundle_id &&
              RequiredString(prepared, "graph_revision") == graph_revision,
          "component_frame_lookup_stale", "captured deployment is no longer active");
  const auto bundle = retained_bundle_source_.get_verified(core::RetainedBundleId(bundle_id));
  Require(bundle != nullptr, "program_applied_selection_invalid", "active retained bundle is unavailable");
  const auto &components = bundle->value().cell_graph.controllable_components;
  Require(std::any_of(components.begin(), components.end(), [&](const auto &component) {
            return component.controllable_component_id.value() == component_id;
          }), "component_reference_missing", "component is absent from the retained deployment");
  const auto model_map = LoadActiveProgramModelMap(*bundle, retained_model_map_source_);
  const deployment::CompositionComponentModelFrame *selected = nullptr;
  for (const auto &path : model_map.frame_paths()) {
    const auto *candidate = std::get_if<deployment::CompositionComponentModelFrame>(&path.terminal_frame);
    if (candidate == nullptr || candidate->controllable_component_id.value() != component_id ||
        candidate->frame_role_id.value() != frame_id) continue;
    Require(selected == nullptr || *selected == *candidate, "frame_missing",
            "component frame is ambiguous in the retained model");
    selected = candidate;
  }
  Require(selected != nullptr, "frame_missing", "component frame is absent from the retained model");
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.component_frame_lookup_result.v1")},
      {"retained_bundle_id", CanonicalValue(bundle_id)},
      {"graph_revision", CanonicalValue(graph_revision)},
      {"frame", frame}, {"exists", CanonicalValue(true)}});
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
        core::ProgramRevision(RequiredString(prepared, "program_revision")),
        *bundle);
    Require(authored != nullptr, "program_applied_selection_invalid",
            "active Program is unavailable");
    program = authored->normalized_program_root();
    non_executable = content_identity::parse_canonical_json(
        deployment::EncodeProgramNonExecutableOutcome(*outcome.non_executable));
  }
  return CanonicalValue(CanonicalValue::Object{
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
  // One Manager source snapshot answers which committed activation is current.
  const auto runtime = RefreshLocked();
  const auto &record = ActiveAppliedRecordLocked(runtime);
  const auto &manager_payload =
      RequiredMember(RequiredSection(runtime.root, "manager"), "payload");
  const auto publication_generation = RequiredCounter(
      RequiredMember(RequiredMember(manager_payload, "active"), "publication"),
      "publication_generation", 2U);
  const auto initialization = RequiredString(record, "scene_initialization");
  Require(initialization == "preserve" || initialization == "initialize",
          "program_activation_invalid",
          "Manager Scene initialization purpose is invalid");
  const auto &prepared = RequiredMember(record, "prepared_activation");
  const auto &origin = ReadPreparedOrigin(prepared);
  const auto record_digest = RequiredString(record, "intent_digest");
  const auto outcome = ReadAppliedProgramOutcome(prepared);
  CanonicalValue program(nullptr);
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::shared_ptr<const deployment::ProgramBindingContract> authored;
  if (outcome.program_binding_id.has_value() || outcome.non_executable.has_value()) {
    // Bound or not, the committed declarations are the authored Program's.
    bundle = retained_bundle_source_.get_verified(
        core::RetainedBundleId(RequiredString(prepared, "retained_bundle_id")));
    Require(bundle != nullptr, "program_applied_selection_invalid",
            "active retained bundle is unavailable");
    authored = program_repository_.get_verified(
        core::ProgramRevision(RequiredString(prepared, "program_revision")), *bundle);
    Require(authored != nullptr, "program_applied_selection_invalid",
            "active Program is unavailable");
    program = authored->normalized_program_root();
  }
  CanonicalValue program_frame_facts(nullptr);
  if (outcome.program_binding_id.has_value() && !outcome.non_executable.has_value()) {
    const auto revision = RequiredString(prepared, "program_revision");
    const auto bundle_id = RequiredString(prepared, "retained_bundle_id");
    const auto graph_revision = RequiredString(prepared, "graph_revision");
    const bool same_facts = applied_program_frame_facts_ &&
        applied_program_frame_facts_->first == record_digest &&
        RequiredString(applied_program_frame_facts_->second, "program_binding_id") == outcome.program_binding_id->value() &&
        RequiredString(applied_program_frame_facts_->second, "program_revision") == revision &&
        RequiredString(applied_program_frame_facts_->second, "retained_bundle_id") == bundle_id &&
        RequiredString(applied_program_frame_facts_->second, "graph_revision") == graph_revision;
    if (!same_facts) {
      // Missing frame evidence must not make unrelated persistent reads fail.
      // Null facts cannot authorize a component frame at runtime.
      try {
        const auto model_map = LoadActiveProgramModelMap(*bundle, retained_model_map_source_);
        const deployment::FilesystemVerifiedCatalogArtifactSource catalog(config_.artifact_root);
        const auto binding = binding_repository_.get_verified(*outcome.program_binding_id, *bundle, model_map, catalog);
        if (binding != nullptr &&
            binding->value().program_revision == core::ProgramRevision(revision) &&
            binding->value().retained_bundle_id == core::RetainedBundleId(bundle_id) &&
            binding->value().graph_revision == core::GraphRevision(graph_revision)) {
          applied_program_frame_facts_ = std::make_pair(
              record_digest, deployment::CaptureProgramFrameFacts(
                  authored->value(), model_map, record_digest,
                  outcome.program_binding_id->value(), revision, bundle_id, graph_revision));
        }
      } catch (const std::bad_alloc &) {
        throw;
      } catch (const std::exception &) {
        // Preserve the committed Program; only frame-dependent writes refuse.
      }
    }
    if (applied_program_frame_facts_ && applied_program_frame_facts_->first == record_digest &&
        RequiredString(applied_program_frame_facts_->second, "program_binding_id") == outcome.program_binding_id->value() &&
        RequiredString(applied_program_frame_facts_->second, "program_revision") == revision &&
        RequiredString(applied_program_frame_facts_->second, "retained_bundle_id") == bundle_id &&
        RequiredString(applied_program_frame_facts_->second, "graph_revision") == graph_revision)
      program_frame_facts = applied_program_frame_facts_->second;
  }
  return CanonicalValue(CanonicalValue::Object{
      {"apply_generation",
       Number(RequiredCounter(record, "apply_generation", 1U),
              "apply_generation")},
      {"apply_id", Number(RequiredCounter(record, "apply_id", 1U), "apply_id")},
      {"program", std::move(program)},
      {"program_frame_facts", std::move(program_frame_facts)},
      {"program_binding_id", outcome.program_binding_id.has_value()
          ? CanonicalValue(outcome.program_binding_id->value()) : CanonicalValue(nullptr)},
      {"retained_bundle_id", RequiredMember(prepared, "retained_bundle_id")},
      {"graph_revision", RequiredMember(prepared, "graph_revision")},
      {"publication_generation",
       Number(publication_generation, "publication_generation")},
      {"record_digest", CanonicalValue(record_digest)},
      {"origin", origin},
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

CanonicalValue HeadlessApiCore::ApplyManagerStatusFrame(
    const std::string_view frame) {
  const auto result = manager_status_.Apply(frame);
  return CanonicalValue(CanonicalValue::Object{
      {"control_changed", CanonicalValue(result.control_changed)},
      {"tick_advanced", CanonicalValue(result.tick_advanced)},
      {"viewer_changed", CanonicalValue(result.viewer_changed)},
  });
}

bool HeadlessApiCore::MarkManagerStatusDisconnected() {
  return manager_status_.MarkDisconnected();
}

std::optional<std::pair<std::string, std::string>>
HeadlessApiCore::ViewerPoseSources() const {
  return manager_status_.ViewerPoseSources();
}

std::optional<std::string>
HeadlessApiCore::LogicalSceneActiveProjection() const {
  return manager_status_.LogicalSceneActiveProjection();
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
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.Acknowledge(
      condition_id, activation_id, expected_record_revision, caller,
      std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
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
  static_cast<void>(RefreshLocked());
  const auto mutation = alarm_owner_.AcknowledgeAll(
      std::move(members), expected_generation,
      caller, std::move(reason), confirmed);
  const auto audit_ids = DeliverFactsLocked(mutation.lifecycle_facts);
  return MutationResponse(mutation, audit_ids);
}

CanonicalValue HeadlessApiCore::HistoryPage(const HistorianQuery &query) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
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
                                                 std::size_t limit,
                                                 const std::optional<std::string> &application_start_id) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    store = history_store_;
  }
  return store->Discover(kind, after, limit, application_start_id);
}

CanonicalValue HeadlessApiCore::HistoryCapturePage(
    const std::optional<std::uint64_t> before_capture_id, const std::size_t limit) {
  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
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
    store = history_store_;
  }
  const auto stored = store->Read(sequence);
  Require(stored.has_value(), "historian_event_not_found",
          "the requested history event is not retained");
  return HistorianDetailValue(*stored);
}

CanonicalValue HeadlessApiCore::RecordClientIncident(
    std::string incident_id, std::string incident_kind, std::string phase,
    const std::optional<std::uint64_t> control_session_number,
    std::string client_incarnation,
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
      "control_stale",          "control_projection_invalid",
      "history_detail_request_failed",
      "history_request_failed", "control_source_unavailable",
      // Browser-only viewer conditions presented as errors:
      // an unavailable/unverified view of the applied robot or Scene.
      "viewer_model_unavailable", "viewer_scene_unverified",
      "viewer_observation_stale", "viewer_geometry_unavailable"};
  Require(bounded_text(incident_id, 256U) &&
              (!control_session_number.has_value() ||
               (*control_session_number != 0U &&
                *control_session_number <= kMaximumSafeInteger)) &&
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
                                             "cycle_generation",
                                             "cycle_state",
                                             "reset_phase",
                                             "execution_control_revision",
                                             "run_revision",
                                             "program_revision",
                                             "graph_revision",
                                             "retained_bundle_id",
                                             "program_binding_id",
                                             "node_environment_digest",
                                             "scene_id",
                                             "scene_revision_id",
                                             "scene_content_hash",
                                             "scene_source_owner_id",
                                             "scene_source_generation",
                                             "scene_source_freshness",
                                             "program_source_current",
                                             "cold_source_current"};
  CanonicalValue::Object context{
      {"schema_version", CanonicalValue("manyforge.incident_context.v2")},
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
      {"clock_owner", CanonicalValue("headless:" + config_.application_start_id)},
      {"control_session_number",
       control_session_number.has_value()
           ? Number(*control_session_number, "control_session_number")
           : CanonicalValue(nullptr)},
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
      {"schema_version", CanonicalValue("manyforge.client_incident_event.v2")},
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
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)},
      {"schema_version", CanonicalValue("manyforge.client_incident_record.v1")},
  });
}

CanonicalValue HeadlessApiCore::HistorianHealth() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  return AsyncHistorianHealthValue(historian_.Health());
}

CanonicalValue HeadlessApiCore::RecordProgramDiagnostic(CanonicalValue record) {
  const std::string code = "program_diagnostic_invalid";
  const std::set<std::string> fields{"schema_version",
                                     "program_run_id",
                                     "application_start_id",
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
  Require(text("schema_version") == "manyforge.program_diagnostic.v5", code,
          "Program diagnostic version is unsupported");
  const auto &run_number = RequiredMember(record, "program_run_id");
  Require(run_number.is_number() &&
              content_identity::parse_nonnegative_int64(run_number) >= 1,
          code, "Program diagnostic run number is invalid");
  // ``A`` of the start that recorded it; checked once at the HTTP route.
  const auto start = text("application_start_id", 16U);
  Require(core::ParseApplicationStartId(start).has_value(), code,
          "Program diagnostic start identity is invalid");
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
                       CanonicalValue("program_runner:" + start));
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
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_diagnostic_record.v1")},
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)}});
}

CanonicalValue HeadlessApiCore::RecordActivity(CanonicalValue record) {
  const std::string code = "activity_record_invalid";
  Require(record.is_object(), code, "activity record fields are invalid");
  // A Program Reset record (OD-305) also carries its `reset` facts, a
  // Program node attempt its `attempt` facts and a scene change its
  // `scene_change` facts (OD-375).
  const auto *kind_member = record.find_object_member("kind");
  const auto *kind_text = kind_member == nullptr
                              ? nullptr
                              : std::get_if<std::string>(&kind_member->storage());
  const bool program_reset = kind_text != nullptr && *kind_text == "program_reset";
  const bool node_attempt =
      kind_text != nullptr && *kind_text == "program_node_attempt";
  const bool scene_change = kind_text != nullptr && *kind_text == "scene_change";
  // An operator command's end, a manual output write, a Program run's
  // start, end or audit event, a producer's lost records and a captured
  // motion trace carry one flat object of scalar facts; any kind may name
  // its caller.
  const bool command_finished =
      kind_text != nullptr && *kind_text == "command_finished";
  const bool io_output = kind_text != nullptr && *kind_text == "io_output_written";
  const bool run_event = kind_text != nullptr && *kind_text == "program_run_event";
  const bool records_lost = kind_text != nullptr && *kind_text == "records_lost";
  const bool motion_trace =
      kind_text != nullptr && *kind_text == "motion_trace_captured";
  const bool scalar_kind =
      command_finished || io_output || run_event || records_lost || motion_trace;
  const bool has_caller = record.find_object_member("caller") != nullptr;
  std::set<std::string> fields{"schema_version", "kind",   "subject",
                               "action",         "outcome", "error",
                               "components",     "message"};
  if (program_reset) fields.insert("reset");
  if (node_attempt) fields.insert("attempt");
  if (scene_change) fields.insert("scene_change");
  if (command_finished) fields.insert("command");
  if (io_output) fields.insert("io_output");
  if (run_event) fields.insert("run_event");
  if (records_lost) fields.insert("records_lost");
  if (motion_trace) fields.insert("motion_trace");
  if (has_caller) fields.insert("caller");
  Require(record.as_object().size() == fields.size(), code,
          "activity record fields are invalid");
  for (const auto &[name, value] : record.as_object()) {
    Require(fields.contains(name), code, "activity record field is unknown");
  }
  const auto bounded = [&](const std::string &value, std::size_t limit) {
    Require(!value.empty() && value.size() <= limit &&
                content_identity::is_utf8_nfc(value),
            code, "activity record text is invalid");
  };
  const auto text = [&](const std::string_view name, std::size_t limit = 256U) {
    const auto *value =
        std::get_if<std::string>(&RequiredMember(record, name).storage());
    Require(value != nullptr, code, "activity record text is missing");
    bounded(*value, limit);
    return *value;
  };
  // A typed error: code and message, optionally the vendor's code and text.
  const auto error_value = [&](const CanonicalValue &error) {
    if (std::holds_alternative<std::nullptr_t>(error.storage())) return false;
    Require(error.is_object() && error.as_object().size() <= 5U, code,
            "activity record error is invalid");
    bool has_code = false;
    for (const auto &[name, value] : error.as_object()) {
      if (name == "details") {
        // Structured details (bounded by the producer and the record bound).
        Require(value.is_object(), code, "activity record error details are invalid");
        continue;
      }
      const auto *member = std::get_if<std::string>(&value.storage());
      Require(member != nullptr &&
                  (name == "code" || name == "message" ||
                   name == "vendor_code" || name == "vendor_text"),
              code, "activity record error field is invalid");
      bounded(*member, name == "code" ? 256U : 2048U);
      has_code = has_code || name == "code";
    }
    Require(has_code, code, "activity record error has no code");
    return true;
  };
  Require(text("schema_version") == "manyforge.activity_event.v1", code,
          "activity record version is unsupported");
  const auto kind = text("kind");
  const auto subject = text("subject");
  const auto outcome = text("outcome");
  text("message", 4096U);
  const bool component_action = kind == "component_action";
  Require(component_action || kind == "apply" || program_reset ||
              node_attempt || scene_change || scalar_kind,
          code, "activity record kind is unsupported");
  if (has_caller) {
    const auto &caller = RequiredMember(record, "caller");
    Require(caller.is_object() && !caller.as_object().empty() &&
                caller.as_object().size() <= 5U,
            code, "activity record caller is invalid");
    for (const auto &[name, value] : caller.as_object()) {
      const auto *member = std::get_if<std::string>(&value.storage());
      Require(member != nullptr &&
                  (name == "request_id" || name == "request_key" ||
                   name == "transport" || name == "source_label" ||
                   name == "principal_id"),
              code, "activity record caller field is invalid");
      bounded(*member, 256U);
      Require(name != "transport" || *member == "http" || *member == "ws" ||
                  *member == "mcp",
              code, "activity record caller transport is unsupported");
    }
  }
  // A command or run event name, or a run event's outcome label.
  const auto identifier = [](const std::string &value, const bool digits) {
    return value.size() <= 64U &&
           std::all_of(value.begin(), value.end(), [digits](const char c) {
             return (c >= 'a' && c <= 'z') || c == '_' ||
                    (digits && c >= '0' && c <= '9');
           });
  };
  Require(!(command_finished || run_event) || identifier(subject, true), code,
          "activity record subject is invalid");
  // A producer name: lowercase letters, digits, '_' and '.'.
  Require(!records_lost ||
              (subject.size() <= 64U &&
               std::all_of(subject.begin(), subject.end(), [](const char c) {
                 return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                        c == '_' || c == '.';
               })),
          code, "activity record subject is invalid");
  const std::set<std::string> outcomes =
      run_event ? std::set<std::string>{}
      : records_lost ? std::set<std::string>{"lost"}
      : motion_trace ? std::set<std::string>{"succeeded", "failed"}
      : command_finished
          ? std::set<std::string>{"succeeded", "accepted", "refused", "failed",
                                  "unknown"}
      : io_output
          ? std::set<std::string>{"succeeded", "refused", "failed", "unknown"}
      : component_action
          ? std::set<std::string>{"succeeded", "failed", "unknown", "refused",
                                  "interrupted"}
      : program_reset || scene_change
          ? std::set<std::string>{"succeeded", "failed", "unknown", "refused"}
      : node_attempt
          ? std::set<std::string>{"succeeded", "failed", "interrupted",
                                  "stopped", "preempted"}
          : std::set<std::string>{"succeeded", "accepted", "failed", "refused"};
  Require(run_event ? identifier(outcome, false) : outcomes.contains(outcome),
          code, "activity record outcome is unsupported");
  const auto &action = RequiredMember(record, "action");
  const auto &components = RequiredMember(record, "components");
  Require(components.is_array() && components.as_array().size() <= 256U, code,
          "activity record components are invalid");
  // The exact fields of a kind's facts object.
  const auto facts_object = [&](const std::string_view name,
                                const std::set<std::string> &names)
      -> const CanonicalValue & {
    const auto &facts = RequiredMember(record, name);
    Require(facts.is_object() && facts.as_object().size() == names.size(),
            code, "activity record facts are invalid");
    for (const auto &[member, value] : facts.as_object()) {
      Require(names.contains(member), code,
              "activity record facts field is unknown");
    }
    return facts;
  };
  const auto optional_fact_text = [&](const CanonicalValue &facts,
                                      const std::string_view name) {
    const auto &value = RequiredMember(facts, name);
    if (std::holds_alternative<std::nullptr_t>(value.storage())) return false;
    const auto *member = std::get_if<std::string>(&value.storage());
    Require(member != nullptr, code, "activity record facts text is invalid");
    bounded(*member, 256U);
    return true;
  };
  const auto fact_number = [&](const CanonicalValue &facts,
                               const std::string_view name, bool optional) {
    const auto &value = RequiredMember(facts, name);
    if (optional && std::holds_alternative<std::nullptr_t>(value.storage())) {
      return;
    }
    const auto *member = std::get_if<double>(&value.storage());
    Require(member != nullptr && std::isfinite(*member) && *member >= 1.0 &&
                std::floor(*member) == *member &&
                *member <= static_cast<double>(kMaximumSafeInteger),
            code, "activity record facts number is invalid");
  };
  const auto fact_duration = [&](const CanonicalValue &facts) {
    const auto *member =
        std::get_if<double>(&RequiredMember(facts, "duration_ms").storage());
    Require(member != nullptr && std::isfinite(*member) && *member >= 0.0,
            code, "activity record duration is invalid");
  };
  // A node attempt's end or a run event (Runner monotonic ns, UTC text): its
  // event time.
  std::optional<std::pair<std::int64_t, std::string>> attempt_end;
  if (component_action) {
    text("action", 64U);
    Require(components.as_array().empty(), code,
            "a component action record carries no component rows");
  } else if (node_attempt || scene_change) {
    Require(std::holds_alternative<std::nullptr_t>(action.storage()) &&
                components.as_array().empty(),
            code, "a node attempt or scene change record carries no action "
                  "or component rows");
    if (node_attempt) {
      // The subject is the node type; the facts name the run, the node
      // instance, the attempt, its start and duration and, for a scene
      // node, the scene operation and object.
      const auto &attempt = facts_object(
          "attempt", {"program_run_id", "node_instance_id", "node_attempt_id",
                      "started_at_wall", "finished_at_ns", "finished_at_wall",
                      "duration_ms", "scene_operation", "scene_resource_id"});
      fact_number(attempt, "program_run_id", false);
      fact_number(attempt, "node_attempt_id", false);
      Require(optional_fact_text(attempt, "node_instance_id"), code,
              "activity record node instance is missing");
      const auto *started = std::get_if<std::string>(
          &RequiredMember(attempt, "started_at_wall").storage());
      Require(started != nullptr && started->size() <= 64U &&
                  HistorianRecordedUtcNs(*started).has_value(),
              code, "activity record node attempt start is invalid");
      // The event is stamped at the attempt's end on the Runner's clocks.
      const auto *finished_wall = std::get_if<std::string>(
          &RequiredMember(attempt, "finished_at_wall").storage());
      Require(finished_wall != nullptr && finished_wall->size() <= 64U &&
                  HistorianRecordedUtcNs(*finished_wall).has_value(),
              code, "activity record node attempt end is invalid");
      attempt_end = std::pair{
          content_identity::parse_nonnegative_int64(
              RequiredMember(attempt, "finished_at_ns")),
          *finished_wall};
      fact_duration(attempt);
      optional_fact_text(attempt, "scene_operation");
      optional_fact_text(attempt, "scene_resource_id");
    } else {
      const std::set<std::string> operations{
          "add_object",
          "upsert_object",
          "remove_object",
          "update_object_pose",
          "attach_carried_object",
          "detach_carried_object",
          "detach_carried_object_at_current_pose",
          "set_collision_policy"};
      Require(operations.contains(subject), code,
              "activity record scene operation is unsupported");
      // The object, the caller, the time it took and the committed revision.
      const auto &change = facts_object(
          "scene_change", {"scene_resource_id", "source_label", "principal_id",
                           "duration_ms", "scene_revision_id"});
      optional_fact_text(change, "scene_resource_id");
      optional_fact_text(change, "source_label");
      optional_fact_text(change, "principal_id");
      fact_duration(change);
      fact_number(change, "scene_revision_id", true);
    }
  } else if (scalar_kind) {
    Require(std::holds_alternative<std::nullptr_t>(action.storage()) &&
                components.as_array().empty(),
            code, "a command, output write, run event, lost-records or motion "
                  "trace record carries no action or component rows");
    const auto &facts = RequiredMember(
        record, command_finished ? "command"
                : io_output      ? "io_output"
                : run_event      ? "run_event"
                : records_lost   ? "records_lost"
                                 : "motion_trace");
    Require(facts.is_object() &&
                facts.as_object().size() >= (run_event ? 3U : 1U) &&
                facts.as_object().size() <=
                    (run_event ? 32U : records_lost ? 8U : 16U),
            code, "activity record facts are invalid");
    for (const auto &[name, value] : facts.as_object()) {
      const auto *member = std::get_if<std::string>(&value.storage());
      const auto *number = std::get_if<double>(&value.storage());
      Require(member != nullptr
                  ? member->size() <= 2048U && content_identity::is_utf8_nfc(*member)
              : number != nullptr ? std::isfinite(*number)
                                  : value.is_null() ||
                                        std::holds_alternative<bool>(value.storage()),
              code, "activity record facts value is invalid");
    }
    const auto required = [&](const std::string_view name) {
      Require(facts.find_object_member(name) != nullptr, code,
              "activity record facts field is missing");
    };
    if (command_finished) {
      // Identical refusals coalesced into this row.
      required("repeat_count");
      fact_number(facts, "repeat_count", false);
    } else if (records_lost) {
      // How many records the producer dropped (its bounded queue was full).
      required("lost_count");
      fact_number(facts, "lost_count", false);
    } else if (run_event) {
      // Stamped by the Runner at production, on its clocks, like a node
      // attempt's end.
      required("program_run_id");
      required("occurred_at_ns");
      required("occurred_at_wall");
      fact_number(facts, "program_run_id", false);
      const auto *wall = std::get_if<std::string>(
          &RequiredMember(facts, "occurred_at_wall").storage());
      Require(wall != nullptr && wall->size() <= 64U &&
                  HistorianRecordedUtcNs(*wall).has_value(),
              code, "activity record run event time is invalid");
      std::optional<std::int64_t> occurred_at_ns;
      try {
        occurred_at_ns = content_identity::parse_nonnegative_int64(
            RequiredMember(facts, "occurred_at_ns"));
      } catch (const content_identity::CanonicalValueError &) {
      }
      Require(occurred_at_ns.has_value(), code,
              "activity record run event time is invalid");
      attempt_end = std::pair{*occurred_at_ns, *wall};
    }
  } else if (program_reset) {
    Require(subject == "program_reset", code,
            "activity record Program Reset subject is unsupported");
    Require(std::holds_alternative<std::nullptr_t>(action.storage()) &&
                components.as_array().empty(),
            code, "a Program Reset record carries no action or component rows");
    // Its options, the run it reset, the Applied Program it used and the
    // scene revision the verified readback left (null when not reloaded).
    const auto &reset = RequiredMember(record, "reset");
    const std::set<std::string> reset_fields{
        "reload_initial_scene", "reset_robot_position", "reset_stored_values",
        "program_run_id",       "program_revision",     "applied_record_digest",
        "scene_revision_id",    "scene_content_hash",   "cause_code"};
    Require(reset.is_object() &&
                reset.as_object().size() == reset_fields.size(),
            code, "activity record reset facts are invalid");
    for (const auto &[name, value] : reset.as_object()) {
      Require(reset_fields.contains(name), code,
              "activity record reset field is unknown");
    }
    const auto flag = [&](const std::string_view name) {
      Require(std::holds_alternative<bool>(RequiredMember(reset, name).storage()),
              code, "activity record reset option is invalid");
    };
    const auto optional_text = [&](const std::string_view name) {
      const auto &value = RequiredMember(reset, name);
      if (std::holds_alternative<std::nullptr_t>(value.storage())) return false;
      const auto *member = std::get_if<std::string>(&value.storage());
      Require(member != nullptr, code, "activity record reset text is invalid");
      bounded(*member, 256U);
      return true;
    };
    const auto optional_number = [&](const std::string_view name) {
      const auto &value = RequiredMember(reset, name);
      if (std::holds_alternative<std::nullptr_t>(value.storage())) return false;
      const auto *member = std::get_if<double>(&value.storage());
      Require(member != nullptr && std::isfinite(*member) && *member >= 1.0 &&
                  std::floor(*member) == *member &&
                  *member <= static_cast<double>(kMaximumSafeInteger),
              code, "activity record reset number is invalid");
      return true;
    };
    flag("reload_initial_scene");
    flag("reset_robot_position");
    flag("reset_stored_values");
    optional_number("program_run_id");
    optional_text("program_revision");
    optional_text("applied_record_digest");
    optional_text("cause_code");
    Require(optional_number("scene_revision_id") ==
                optional_text("scene_content_hash"),
            code, "activity record reset scene revision is incomplete");
  } else {
    Require(std::holds_alternative<std::nullptr_t>(action.storage()), code,
            "an Apply record carries no action");
    const std::set<std::string> apply_kinds{
        "scenario_load",     "scenario_new",  "deployment_apply",
        "deployment_revert", "program_apply", "program_revert"};
    Require(apply_kinds.contains(subject), code,
            "activity record Apply kind is unsupported");
    for (const auto &row : components.as_array()) {
      Require(row.is_object() && row.as_object().size() <= 4U, code,
              "activity record component row is invalid");
      for (const auto &[name, value] : row.as_object()) {
        if (name == "error") {
          error_value(value);
          continue;
        }
        const auto *member = std::get_if<std::string>(&value.storage());
        Require(member != nullptr &&
                    (name == "component_id" || name == "disposition" ||
                     name == "lifecycle_after"),
                code, "activity record component row field is invalid");
        bounded(*member, 256U);
      }
      Require(row.find_object_member("component_id") != nullptr, code,
              "activity record component row has no component id");
    }
  }
  const bool has_error = error_value(RequiredMember(record, "error"));
  // A node attempt carries its error only when it failed or was interrupted
  // (a Stop or a preemption is no error).
  Require(has_error == (node_attempt
                            ? outcome == "failed" || outcome == "interrupted"
                        : run_event ? outcome == "failed"
                        : records_lost ? false
                            : outcome != "succeeded" && outcome != "accepted"),
          code, "activity record error does not match its outcome");
  Require(content_identity::canonical_json(record).size() <= 64U * 1024U, code,
          "activity record exceeds its byte bound");
  // A failed node attempt is WARN, never ERROR: a failed condition node is
  // ordinary control flow, and ERROR would start a failure capture.
  // The command, output-write, run-event, lost-records and motion-trace
  // kinds are never ERROR either.
  const auto severity =
      scalar_kind ? (std::set<std::string>{"refused", "failed", "unknown",
                                           "stopped", "halted", "held",
                                           "intervention_required",
                                           "interrupted", "lost"}
                             .contains(outcome)
                         ? "WARN"
                         : "INFO")
      : node_attempt ? (outcome == "failed" ? "WARN" : "INFO")
      : outcome == "succeeded" || outcome == "accepted" ? "INFO"
      : outcome == "refused" || outcome == "interrupted" ? "WARN"
                                                         : "ERROR";
  auto payload = record.as_object();
  payload.emplace_back("clock_owner",
                       CanonicalValue((attempt_end ? "program_runner:" : "headless:") +
                                      config_.application_start_id));

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  auto event = MakeHistorianEvent(
      component_action ? subject
      : program_reset || node_attempt || run_event
          ? std::string("manyforge.program_runner")
      : scene_change     ? std::string("manyforge.scene")
      : records_lost     ? std::string(subject.starts_with("program_runner")
                                           ? "manyforge.program_runner"
                                           : "manyforge.headless")
      : command_finished ? std::string("manyforge.headless")
      : io_output        ? std::string("manyforge.io")
      : motion_trace     ? std::string("manyforge.motion")
                         : std::string("manyforge.composer"),
      component_action ? "component_action_finished"
      : program_reset  ? "program_reset_finished"
      : node_attempt   ? "program_node_attempt_finished"
      : scene_change   ? "scene_change_finished"
      : run_event      ? subject
      : records_lost   ? "historian_records_lost"
      : scalar_kind    ? kind
                       : "apply_finished",
      severity,
      attempt_end ? attempt_end->first : clocks_.monotonic_time_ns(),
      attempt_end ? attempt_end->second : clocks_.wall_time_text(),
      CanonicalValue(std::move(payload)));
  const auto event_id = event.event_id;
  const bool recorded = historian_.TryEnqueue(std::move(event));
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", CanonicalValue(event_id)},
      {"recorded", CanonicalValue(recorded)},
      {"schema_version", CanonicalValue("manyforge.activity_record.v1")}});
}

CanonicalValue
HeadlessApiCore::CommitProgramModelAttempt(CanonicalValue record) {
  constexpr std::size_t kProgramModelAttemptFieldCount = 31U;
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
  Require(text("schema_version") == "manyforge.program_model_attempt.v2",
          error_code, "Program model-attempt version is unsupported");
  // ``A`` of the start that made the attempt, inside the record (1.15.12);
  // checked once at the HTTP route.
  Require(core::ParseApplicationStartId(text("application_start_id", 16U))
              .has_value(),
          error_code, "Program model-attempt start identity is invalid");
  const auto model_attempt_id = text("model_attempt_id");
  static_cast<void>(counter("program_run_id", 1U));
  text("node_instance_id");
  static_cast<void>(counter("node_attempt_id", 1U));
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

  std::shared_ptr<SqliteEventHistoryStore> store;
  {
    std::lock_guard lock(mutex_);
    Require(!shutdown_, "headless_api_closed", "headless API core is closed");
    store = history_store_;
  }
  // OD-221: the synchronous write runs after the core lock. The store
  // serializes appends itself, and its unique event identity decides "stored"
  // or "duplicate". A call racing Shutdown may meet a closed store
  // (historian_closed).
  auto event = MakeHistorianEvent("manyforge.program", "program_model_attempt",
                                  outcome == "success" ? "INFO" : "WARN",
                                  static_cast<std::int64_t>(occurred_at_ns),
                                  occurred_at_wall, std::move(record));
  const auto event_id = event.event_id;
  const auto status = store->Append(event);
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

// The current v10 common row carries one neutral addressed command identity:
// `participant_id` {operation_id, ordinal} and `command_id` {operation_id,
// ordinal, generation} (contract 1.5). Optional rule evidence retains its
// existing typed validation.
void ValidateParticipantExtensions(const CanonicalValue& state) {
  for (const auto& participant : RequiredArray(state, "participants")) {
    Require(participant.is_object() &&
                participant.find_object_member("controller_goal_id") == nullptr &&
                participant.find_object_member("controller_goal_generation") == nullptr,
            "headless_operation_event_invalid", "common participant uses obsolete command identity");
    const auto participant_id =
        core::ParseOperationParticipantId(RequiredMember(participant, "participant_id"));
    const auto command_id =
        core::ParseControllerGoalId(RequiredMember(participant, "command_id"));
    Require(participant_id.has_value() && command_id.has_value(),
            "headless_operation_event_invalid",
            "participant or command identity is not its integer object");
    const auto* rules = participant.find_object_member("io_rule_outcomes");
    if (!rules) continue;
    const auto& capability = RequiredMember(participant, "capability_provider_binding_id");
    const auto& capability_revision =
        RequiredMember(participant, "capability_provider_binding_revision");
    std::optional<core::CapabilityProviderBindingIdentityDTO> capability_binding;
    if (!capability.is_null() || !capability_revision.is_null()) {
      capability_binding = core::CapabilityProviderBindingIdentityDTO{
          core::CapabilityProviderBindingId(RequiredString(participant, "capability_provider_binding_id")),
          core::CapabilityProviderBindingRevision(RequiredString(participant, "capability_provider_binding_revision"))};
    }
    const core::ParticipantBindingIdentityDTO identity{
        core::ControlDomainId(RequiredString(participant, "control_domain_id")),
        core::ControlDomainRevision(RequiredString(participant, "control_domain_revision")),
        core::ControllerParticipantBindingIdentityDTO{
            core::ControlBindingId(RequiredString(participant, "control_binding_id")),
            core::ControlBindingRevision(RequiredString(participant, "control_binding_revision")),
            capability_binding}};
    Require(identity.locally_valid(), "headless_operation_event_invalid",
            "participant binding identity is invalid");
    const auto decoded = core::DecodeIoRuleOutcomes(content_identity::canonical_json(*rules));
    Require(decoded.ok() && !decoded.value().empty(), "headless_operation_event_invalid",
            "I/O rule evidence is invalid or empty");
    // Share the journal's historical chronology and exact containing tuple
    // checks. This observation never grants live execution authority.
    const auto qualified = core::ValidateIoRuleOutcomeParticipant(
        core::OperationNumber(RequiredCounter(state, "operation_id", 1U)),
        core::OperationRevision(RequiredCounter(state, "operation_revision", 1U)),
        *participant_id,
        core::ControllableComponentId(RequiredString(participant, "controllable_component_id")),
        core::CommandTargetId(RequiredString(participant, "command_target_id")),
        *command_id,
        decoded.value());
    Require(qualified.ok(), "headless_operation_event_invalid",
            "I/O rule evidence relabels its containing participant");
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
      }
      const auto *scope = participant.find_object_member("execution_scope");
      if (scope) {
        Require(captured_origin_allowed && scope->is_object() &&
                    scope->as_object().size() == 2U,
                "headless_operation_event_invalid",
                "authored origin requires its exact journal version");
        const auto &node = RequiredString(*scope, "node_instance_id");
        static_cast<void>(RequiredCounter(*scope, "node_attempt_id", 1U));
        Require(node.size() <= 128U && node.find('\0') == std::string::npos,
                "headless_operation_event_invalid",
                "authored origin is invalid or unbounded");
        result.captured_origin = true;
      }
    }
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
    const auto operation = RequiredCounter(by, "operation_id", 1U);
    const auto &node = RequiredString(by, "node_instance_id");
    static_cast<void>(RequiredCounter(by, "node_attempt_id", 1U));
    Require(node.size() <= 128U && node.find('\0') == std::string::npos &&
                operation != RequiredCounter(state, "operation_id", 1U),
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
    const std::uint64_t source_sequence,
    std::string record_kind, std::optional<std::string> request_key,
    const std::optional<std::uint64_t> operation_id,
    const std::optional<std::uint64_t> operation_revision,
    const std::int64_t occurred_at_ns, std::string occurred_at_wall,
    CanonicalValue journal_payload,
    std::string payload_digest) {
  const bool has_operation_id = operation_id.has_value();
  const bool has_operation_revision = operation_revision.has_value();
  // Stop state model: one acceptance and one completion row per Stop event
  // (payload `manyforge.stop_event.v1`); neither names a request key column
  // or an operation.
  const bool stop_event_accepted = record_kind == "stop_event_accepted";
  const bool stop_event_completed = record_kind == "stop_event_completed";
  // Active TCP: one History event per confirmed change (robot,
  // old -> new entry, source, offset sent), carried in the journal payload.
  const bool active_tcp_projection = record_kind == "active_tcp_changed";
  const bool rule_state_projection =
      record_kind == "interrupt_rule_state_changed" || active_tcp_projection;
  const bool stop_event_row = stop_event_accepted || stop_event_completed;
  Require(source_sequence != 0U &&
              source_sequence <= kMaximumSafeInteger && !record_kind.empty() &&
              has_operation_id == has_operation_revision &&
              ((stop_event_row || rule_state_projection) != has_operation_id) &&
              (!has_operation_id ||
               (*operation_id != 0U && *operation_id <= kMaximumSafeInteger &&
                *operation_revision != 0U &&
                *operation_revision <= kMaximumSafeInteger)) &&
              occurred_at_ns >= 0 && !occurred_at_wall.empty() &&
              occurred_at_wall.size() <= 128U &&
              content_identity::is_utf8_nfc(occurred_at_wall) &&
              (!request_key.has_value() || IsRequestKeyText(*request_key)) &&
              content_identity::is_sha256_digest(payload_digest) &&
              journal_payload.is_object(),
          "headless_operation_event_invalid",
          "operation event envelope is malformed");
  const std::set<std::string_view> supported_kinds{"operation_accepted",
                                                   "control_accepted",
                                                   "stop_event_accepted",
                                                   "stop_event_completed",
                                                   "runtime_scale_accepted",
                                                   "interrupt_rule_state_changed",
                                                   "active_tcp_changed",
                                                   "operation_prepared",
                                                   "state_observed"};
  Require(supported_kinds.contains(record_kind),
          "headless_operation_event_invalid",
          "operation event record kind is unsupported");
  const auto payload_json = content_identity::canonical_json(journal_payload);
  Require(content_identity::sha256_digest(payload_json) == payload_digest &&
              (rule_state_projection || stop_event_row ||
               RequiredString(journal_payload, "record_kind") == record_kind),
          "headless_operation_event_invalid",
          "operation event payload does not match its durable envelope");
  const CanonicalValue *operation_state = nullptr;
  std::optional<std::uint64_t> stop_event_id;
  std::optional<std::uint64_t> stop_duration_ns;
  std::optional<std::string> stop_result;
  if (active_tcp_projection) {
    Require(!request_key &&
                RequiredString(journal_payload, "api_version") ==
                    "manyforge.active_tcp_change.v3" &&
                !RequiredString(journal_payload, "component_id").empty(),
            "headless_operation_event_invalid",
            "Active TCP change does not match its durable envelope");
  } else if (rule_state_projection) {
    Require(!request_key,
            "headless_operation_event_invalid",
            "rule-state projection cannot name a request");
    ValidateInterruptRuleStateProjection(journal_payload);
  } else if (stop_event_row) {
    // The rows carry the Kernel's event record as written (contract §5.11);
    // History keeps the payload whole and derives severity and duration.
    stop_event_id = RequiredCounter(journal_payload, "event_id", 1U);
    const auto token_in = [&](const std::string_view name,
                              std::initializer_list<std::string_view> tokens) {
      const auto token = RequiredString(journal_payload, name);
      return std::ranges::find(tokens, std::string_view(token)) != tokens.end();
    };
    const auto covered_valid = [&](const bool completed) {
      for (const auto &entry : RequiredArray(journal_payload, "covered")) {
        static_cast<void>(RequiredCounter(entry, "operation_id", 1U));
        static_cast<void>(RequiredCounter(entry, "participant_ordinal", 1U));
        static_cast<void>(RequiredBool(entry, "started"));
        if (completed) {
          const auto phase = RequiredString(entry, "phase");
          if (phase != "stopping" && phase != "stopped" && phase != "failed")
            return false;
          static_cast<void>(RequiredBool(entry, "escalated"));
          static_cast<void>(RequiredBool(entry, "stop_path_collision"));
          if (RequiredMember(entry, "containing_stage_index").is_null() !=
              RequiredMember(entry, "containing_stage_kind").is_null())
            return false;
        }
      }
      return true;
    };
    const auto accepted_at_ns = content_identity::parse_nonnegative_int64(
        RequiredMember(journal_payload, "accepted_at_ns"));
    Require(!request_key &&
                RequiredString(journal_payload, "schema_version") ==
                    "manyforge.stop_event.v1" &&
                RequiredString(journal_payload, "row") ==
                    (stop_event_accepted ? "accepted" : "completed") &&
                !RequiredString(journal_payload, "applied").empty() &&
                covered_valid(stop_event_completed),
            "headless_operation_event_invalid",
            "Stop event row does not match its durable envelope");
    if (stop_event_accepted) {
      Require(IsRequestKeyText(RequiredString(journal_payload, "request_key")) &&
                  !RequiredString(journal_payload, "requested").empty() &&
                  !RequiredString(journal_payload, "widen_reason").empty() &&
                  RequiredMember(journal_payload, "reason").is_string() &&
                  RequiredMember(journal_payload, "source_label").is_string() &&
                  RequiredMember(journal_payload, "attribution").is_object() &&
                  RequiredArray(journal_payload, "groups").size() <=
                      RequiredCounter(journal_payload, "group_count") &&
                  RequiredArray(journal_payload, "stale_diagnostics").size() <=
                      RequiredCounter(journal_payload, "stale_total"),
              "headless_operation_event_invalid",
              "Stop event acceptance row is invalid");
    } else {
      const auto completed_at_ns = content_identity::parse_nonnegative_int64(
          RequiredMember(journal_payload, "completed_at_ns"));
      Require(token_in("result",
                       {"no_effect", "stopped", "stopped_escalated", "some_failed"}) &&
                  completed_at_ns >= accepted_at_ns,
              "headless_operation_event_invalid",
              "Stop event completion row is invalid");
      stop_duration_ns =
          static_cast<std::uint64_t>(completed_at_ns - accepted_at_ns);
      stop_result = RequiredString(journal_payload, "result");
    }
  } else {
    const auto record_api = RequiredString(journal_payload, "api_version");
    Require(record_api == "manyforge.operation_journal_record.v10",
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
                RequiredCounter(*operation_state, "operation_id", 1U) ==
                    *operation_id &&
                RequiredCounter(*operation_state, "operation_revision", 1U) ==
                    *operation_revision,
            "headless_operation_event_invalid",
            "operation event state does not match its durable envelope");
    // "Did this operation have a Stop": its `stop_event_id` (stop state
    // model H2), an event id or null.
    const auto &stop_event = RequiredMember(*operation_state, "stop_event_id");
    if (!stop_event.is_null())
      static_cast<void>(RequiredCounter(*operation_state, "stop_event_id", 1U));
    ValidateParticipantExtensions(*operation_state);
    // Rows are History records (OD-205 (b)): validated, never alarm input.
    static_cast<void>(ParseOperationFacts(
        *operation_state, journal_payload.find_object_member("intent"), true, true, true));
  }
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
      stopped_with_error || stop_result == "some_failed" ||
              (operation_state != nullptr &&
               IsOperationFaultState(RequiredString(*operation_state, "state")))
          ? "ERROR"
      : stop_result == "stopped_escalated" ? "WARN"
                                           : "INFO";
  const auto native_record_digest = content_identity::framed_sha256(
      "manyforge-operation-journal-event", "v1",
      CanonicalValue(CanonicalValue::Object{
          {"occurred_at_ns",
           content_identity::nonnegative_int64_value(occurred_at_ns)},
          {"occurred_at_wall", CanonicalValue(occurred_at_wall)},
          {"operation_id", operation_id.has_value()
                               ? Number(*operation_id, "operation_id")
                               : CanonicalValue(nullptr)},
          {"operation_revision",
           operation_revision.has_value()
               ? Number(*operation_revision, "operation_revision")
               : CanonicalValue(nullptr)},
          {"payload_digest", CanonicalValue(payload_digest)},
          {"record_kind", CanonicalValue(record_kind)},
          {"request_key", OptionalStringValue(request_key)},
          {"source_sequence", Number(source_sequence, "source_sequence")},
      }));

  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  RegisterOperationSourceLocked();
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

  CanonicalValue::Object history_fields{
      {"application_start_id", CanonicalValue(config_.application_start_id)},
      {"journal_payload", journal_payload},
      {"native_sequence", Number(source_sequence, "native_sequence")},
      {"operation_id", operation_id.has_value()
                           ? Number(*operation_id, "operation_id")
                           : CanonicalValue(nullptr)},
      {"operation_revision",
       operation_revision.has_value()
           ? Number(*operation_revision, "operation_revision")
           : CanonicalValue(nullptr)},
      {"payload_digest", CanonicalValue(payload_digest)},
      {"record_kind", CanonicalValue(record_kind)},
      {"request_key", OptionalStringValue(request_key)},
      {"schema_version",
       CanonicalValue("manyforge.operation_history_event.v2")},
  };
  if (stop_event_id.has_value()) {
    history_fields.emplace_back("stop_event_id",
                                Number(*stop_event_id, "stop_event_id"));
  }
  if (stop_duration_ns.has_value()) {
    history_fields.emplace_back("duration_ns",
                                Number(*stop_duration_ns, "duration_ns"));
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
    }
  } catch (const std::exception &error) {
    historian_.ReportDeliveryFailure("historian_projection_failure",
                                     error.what());
  } catch (...) {
    historian_.ReportDeliveryFailure(
        "historian_projection_failure",
        "operation historian delivery raised an unknown failure");
  }

  operation_native_sequence_ = source_sequence;
  operation_native_record_digest_ = native_record_digest;
  return CanonicalValue(CanonicalValue::Object{
      {"event_id", OptionalStringValue(event_id)},
      {"native_sequence", Number(source_sequence, "native_sequence")},
      {"schema_version", CanonicalValue("manyforge.operation_event_ingest.v1")},
      {"status", CanonicalValue("ingested")},
  });
}

CanonicalValue HeadlessApiCore::CompleteOperationEventSnapshot(
    const std::uint64_t through_source_sequence) {
  Require(through_source_sequence <= kMaximumSafeInteger,
          "headless_operation_event_invalid",
          "operation event snapshot identity is invalid");
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  RegisterOperationSourceLocked();
  Require(
      through_source_sequence == operation_native_sequence_,
      "headless_operation_event_snapshot_incomplete",
      "operation event snapshot completion is ahead of or behind ingestion");

  // The operation source's quality follows the operations table
  // (`ObserveOperationTable`); History rows only advance the cursor.
  return CanonicalValue(CanonicalValue::Object{
      {"native_sequence", Number(through_source_sequence, "native_sequence")},
      {"schema_version",
       CanonicalValue("manyforge.operation_event_snapshot.v2")},
      {"status", CanonicalValue("complete")},
  });
}

CanonicalValue HeadlessApiCore::MarkOperationEventSourceUnknown() {
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  // Before the first event of this start the source is not registered yet,
  // so there is nothing to mark.
  const bool current = !operation_adapter_.source_epoch.empty();
  if (current) {
    alarm_owner_.MarkSourceQualityUnknown(operation_adapter_.source_id,
                                          operation_adapter_.source_epoch);
  }
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.operation_event_source_quality.v2")},
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
  SynchronizeAlarmResourceRegistryLocked(runtime);
  RefreshRuntimeHealthAdapterLocked(runtime);
  RefreshSceneAdapterLocked(runtime);
  SynchronizeHistoryLossAlarmLocked();
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
          {"receiver_age_ns",
           RequiredMember(manager_section, "receiver_age_ns")},
          {"selection_projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"selection_source_content_digest",
           RequiredMember(manager_section, "source_content_digest")},
          {"selection_source_generation",
           RequiredMember(manager_section, "source_generation")},
          {"selection_source_owner_id",
           RequiredMember(manager_section, "source_owner_id")},
          {"source_content_digest", CanonicalValue(semantic_digest)},
          {"source_generation",
           Number(node_environment_generation_, "source_generation")},
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
          {"receiver_age_ns",
           RequiredMember(manager_section, "receiver_age_ns")},
          {"selection_projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"selection_source_content_digest",
           RequiredMember(manager_section, "source_content_digest")},
          {"selection_source_generation",
           RequiredMember(manager_section, "source_generation")},
          {"selection_source_owner_id",
           RequiredMember(manager_section, "source_owner_id")},
          {"source_content_digest", CanonicalValue(semantic_digest)},
          {"source_generation",
           Number(source_model_generation_, "source_generation")},
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
  // The node catalog part (tool descriptors, the derived node set, their
  // digests and source_content_digest) is joined by the Python service of
  // this process, which owns the loaded catalog.
  CanonicalValue::Object result = projection.as_object();
  result.emplace_back(
      "source",
      CanonicalValue(CanonicalValue::Object{
          {"available", RequiredMember(manager_section, "available")},
          {"freshness", RequiredMember(manager_section, "freshness")},
          {"known_gaps", RequiredMember(manager_section, "known_gaps")},
          {"projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"receiver_age_ns",
           RequiredMember(manager_section, "receiver_age_ns")},
          {"selection_projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"selection_source_content_digest",
           RequiredMember(manager_section, "source_content_digest")},
          {"selection_source_generation",
           RequiredMember(manager_section, "source_generation")},
          {"selection_source_owner_id",
           RequiredMember(manager_section, "source_owner_id")},
          {"source_generation",
           RequiredMember(manager_section, "projection_generation")},
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
          {"receiver_age_ns",
           RequiredMember(manager_section, "receiver_age_ns")},
          {"selection_projection_generation",
           RequiredMember(manager_section, "projection_generation")},
          {"selection_source_content_digest",
           RequiredMember(manager_section, "source_content_digest")},
          {"selection_source_generation",
           RequiredMember(manager_section, "source_generation")},
          {"selection_source_owner_id",
           RequiredMember(manager_section, "source_owner_id")},
          {"source_content_digest", CanonicalValue(semantic_digest)},
          {"source_generation",
           RequiredMember(manager_section, "projection_generation")},
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
  // The scene source is this start's Manager; both adapters register once,
  // at the first fresh scene source of this start.
  const auto &source_epoch = config_.application_start_id;
  RegisterAdapterEpochLocked(scene_adapter_, source_epoch);
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
  RegisterAdapterEpochLocked(scene_pose_adapter_, source_epoch);
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
      // The typed publication error of a fail-closed loss.
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

void HeadlessApiCore::RegisterOperationSourceLocked() {
  // Registered once per start, at the first event: the operation source is
  // this start's execution host, whose identity is this start's.
  if (!operation_adapter_.source_epoch.empty()) {
    return;
  }
  RegisterAdapterEpochLocked(operation_adapter_, config_.application_start_id);
  std::erase_if(refused_operation_conditions_, [](const auto &entry) {
    return entry.second.kind != OperationConditionKind::kProgramFailure;
  });
  SynchronizeAlarmScopeConditionLocked();
  operation_native_sequence_ = 0U;
  operation_native_record_digest_.clear();
  observed_operation_table_entries_.clear();
}

namespace {

constexpr auto kOperationTableInvalid = "headless_operation_table_invalid";

bool IsRuntimeMonitorReason(const CanonicalValue &reason) {
  return reason.is_string() &&
         (reason.as_string() == "runtime_collision_fault" ||
          reason.as_string() ==
              manyforge::core::error_codes::kRuntimeMonitorObservationLost ||
          reason.as_string() ==
              manyforge::core::error_codes::kRuntimeMonitorChecksStopped);
}

// "Ordinary proven Program failure": every participant has its authored
// origin, at least one failed, and each failing participant's terminal fact
// is a local failure with no or settled effect whose error is its terminal
// error. The rule formerly applied to journal rows, unchanged; the table
// carries its raw inputs.
bool CapturedFailureEffectsKnown(const CanonicalValue::Array &participants) {
  bool all_captured = !participants.empty();
  bool has_failure = false;
  bool failure_effects_known = true;
  for (const auto &participant : participants) {
    Require(participant.is_object(), kOperationTableInvalid,
            "operations table participant must be an object");
    const auto state = RequiredString(participant, "state");
    const auto &error = RequiredMember(participant, "terminal_error");
    const auto &fact = RequiredMember(participant, "terminal_fact");
    Require(error.is_null() || error.is_object(), kOperationTableInvalid,
            "operations table participant error is invalid");
    Require(fact.is_null() || fact.is_object(), kOperationTableInvalid,
            "operations table participant fact is invalid");
    all_captured = all_captured && RequiredBool(participant, "execution_scope");
    if (!error.is_null() || IsOperationFaultState(state)) {
      has_failure = true;
      bool known_local_failure = false;
      if (fact.is_object()) {
        const auto effect = RequiredString(fact, "effect");
        known_local_failure = RequiredString(fact, "kind") == "local_failure" &&
                              RequiredString(fact, "error") == "same" &&
                              (effect == "no_effect" || effect == "effect_settled");
      }
      failure_effects_known = failure_effects_known && known_local_failure;
    }
  }
  return all_captured && has_failure && failure_effects_known;
}

// The first participant's terminal error in the alarm's details shape; a cut
// message ends in "…" (the whole text stays in History).
CanonicalValue TableTerminalError(const CanonicalValue &error) {
  if (error.is_null()) return CanonicalValue(nullptr);
  Require(error.is_object(), kOperationTableInvalid,
          "operations table terminal error is invalid");
  auto message = RequiredMember(error, "message");
  Require(message.is_string(), kOperationTableInvalid,
          "operations table error message is invalid");
  if (RequiredBool(error, "message_truncated"))
    message = CanonicalValue(message.as_string() + "\xe2\x80\xa6");  // "…"
  CanonicalValue::Object details;
  if (const auto &reason = RequiredMember(error, "reason"); reason.is_string())
    details.emplace_back("reason", reason);
  return CanonicalValue(CanonicalValue::Object{
      {"code", RequiredMember(error, "code")},
      {"details", CanonicalValue(std::move(details))},
      {"message", std::move(message)},
  });
}

} // namespace

CanonicalValue HeadlessApiCore::ObserveOperationTable(CanonicalValue operations) {
  Require(operations.is_object(), kOperationTableInvalid,
          "operations table must be an object");
  static_cast<void>(RequiredCounter(operations, "change"));
  const auto &faulted = RequiredArray(operations, "faulted");
  const auto faulted_total = RequiredCounter(operations, "faulted_total");
  Require(faulted.size() <= faulted_total, kOperationTableInvalid,
          "operations table lists more entries than it counts");
  // Every operation is listed: an absent one no longer raises an alarm.
  const bool all_listed = faulted.size() == faulted_total;
  bool fully_read = all_listed;
  std::lock_guard lock(mutex_);
  Require(!shutdown_, "headless_api_closed", "headless API core is closed");
  RegisterOperationSourceLocked();
  bool changed = false;
  std::set<std::string> listed;
  for (const auto &entry : faulted) {
    Require(entry.is_object(), kOperationTableInvalid,
            "operations table entry must be an object");
    const auto operation_id =
        std::to_string(RequiredCounter(entry, "operation_id", 1U));
    Require(listed.insert(operation_id).second, kOperationTableInvalid,
            "operations table lists an operation twice");
    fully_read = fully_read && RequiredBool(entry, "complete");
    auto key = content_identity::canonical_json(entry);
    const auto seen = observed_operation_table_entries_.find(operation_id);
    if (seen != observed_operation_table_entries_.end() && seen->second == key) continue;
    changed = true;
    // An entry whose evaluation throws (as a refused journal row before) is
    // evaluated again at the next observation: it is remembered once applied.
    ReconcileOperationTableEntryLocked(entry);
    observed_operation_table_entries_.insert_or_assign(operation_id, std::move(key));
  }
  std::erase_if(observed_operation_table_entries_, [&](const auto &item) {
    return !listed.contains(item.first);
  });
  if (all_listed) {
    // Follow-up A: an operation that left the list (retired by Reset,
    // Recover or retry, or out of its fault state) clears its alarms.
    const auto absent = [&](const auto &map) {
      std::vector<std::string> ids;
      for (const auto &[operation_id, condition] : map) {
        static_cast<void>(condition);
        if (!listed.contains(operation_id)) ids.push_back(operation_id);
      }
      return ids;
    };
    const CanonicalValue retired_details(CanonicalValue::Object{
        {"reason", CanonicalValue("operation_not_faulted")}});
    for (const auto &operation_id : absent(active_operation_faults_)) {
      const auto subject = active_operation_faults_.at(operation_id);
      SubmitOperationTableConditionLocked(
          "manyforge.motion.execution_fault", subject, false, retired_details,
          CanonicalValue(CanonicalValue::Object{
              {"operation_id", Number(std::stoull(operation_id), "operation_id")}}),
          OperationConditionKind::kExecutionFault, operation_id);
      active_operation_faults_.erase(operation_id);
      changed = true;
    }
    for (const auto &operation_id : absent(active_runtime_collisions_)) {
      const auto condition = active_runtime_collisions_.at(operation_id);
      SubmitOperationTableConditionLocked(
          condition.definition_id, condition.subject, false, retired_details,
          CanonicalValue(CanonicalValue::Object{
              {"operation_id", Number(std::stoull(operation_id), "operation_id")}}),
          OperationConditionKind::kRuntimeCollision, operation_id);
      active_runtime_collisions_.erase(operation_id);
      changed = true;
    }
    std::vector<std::pair<std::string, std::string>> refused;
    for (const auto &[key, entry] : refused_operation_conditions_) {
      static_cast<void>(key);
      if (entry.kind != OperationConditionKind::kProgramFailure &&
          !listed.contains(entry.tracking_id))
        refused.emplace_back(entry.update.definition_id, entry.tracking_id);
    }
    for (const auto &[definition_id, operation_id] : refused) {
      ForgetRefusedOperationConditionLocked(definition_id, operation_id);
      changed = true;
    }
  }
  // An unlisted operation or an entry without its alarm facts: the source
  // is not fully read; the present alarms stay as they are.
  alarm_owner_.CompleteSourceSnapshot(
      operation_adapter_.source_id, operation_adapter_.source_epoch,
      fully_read ? AlarmConditionQuality::kGood : AlarmConditionQuality::kUnknown);
  return CanonicalValue(CanonicalValue::Object{
      {"changed", CanonicalValue(changed)},
      {"fully_read", CanonicalValue(fully_read)},
      {"schema_version",
       CanonicalValue("manyforge.operation_table_observation.v1")},
  });
}

void HeadlessApiCore::ReconcileOperationTableEntryLocked(const CanonicalValue &entry) {
  const auto operation_number = RequiredCounter(entry, "operation_id", 1U);
  // Subject and tracking ids stay text: the operation number in decimal.
  const auto operation_id = std::to_string(operation_number);
  const auto revision = RequiredCounter(entry, "operation_revision", 1U);
  const auto state_name = RequiredString(entry, "state");
  const bool complete = RequiredBool(entry, "complete");
  const auto &pending = RequiredMember(entry, "pending_control");
  Require(pending.is_null() || pending.is_string(), kOperationTableInvalid,
          "operations table pending control is invalid");
  // The Reset's acceptance: the Kernel keeps it pending until it settles.
  const bool reset_accepted = pending.is_string() && pending.as_string() == "reset";
  const auto &stop_event = RequiredMember(entry, "stop_event_id");
  if (!stop_event.is_null())
    static_cast<void>(RequiredCounter(entry, "stop_event_id", 1U));
  const auto &monitor_stop = RequiredMember(entry, "runtime_monitor_stop");
  Require(monitor_stop.is_null() || monitor_stop.is_object(), kOperationTableInvalid,
          "operations table runtime-monitor Stop is invalid");
  const CanonicalValue program_binding_id =
      complete ? RequiredMember(entry, "program_binding_id") : CanonicalValue(nullptr);
  const CanonicalValue program_run_id =
      complete ? RequiredMember(entry, "program_run_id") : CanonicalValue(nullptr);
  const auto observed_details = [&](std::string_view reason) {
    return CanonicalValue(CanonicalValue::Object{
        {"reason", CanonicalValue(std::string(reason))},
        {"state", CanonicalValue(state_name)},
    });
  };
  const auto correlations = [&](CanonicalValue request_key) {
    CanonicalValue::Object fields{
        {"operation_id", Number(operation_number, "operation_id")}};
    if (complete) {
      fields.emplace_back("program_binding_id", program_binding_id);
      fields.emplace_back("program_run_id", program_run_id);
    }
    fields.emplace_back("request_key", std::move(request_key));
    return CanonicalValue(std::move(fields));
  };
  // The alarm subject: the union of every participant's resources, at most
  // the alarm's bound (as for journal rows before OD-205 (b)).
  const auto subject = [&] {
    auto resources = StringArray(entry, "resources");
    const auto total = RequiredCounter(entry, "resource_total");
    Require(!resources.empty() && total == resources.size() &&
                total <= kMaximumAlarmSubjectResources,
            "headless_operation_event_invalid",
            "operation fault scope must contain at most 16 exact resources");
    return AlarmSubject{"operation", operation_id, std::move(resources)};
  };

  // 1. A Reset's acceptance clears the runtime-monitor Stop alarm.
  if (reset_accepted) {
    const auto collision = active_runtime_collisions_.find(operation_id);
    if (collision != active_runtime_collisions_.end()) {
      const auto condition = collision->second;
      SubmitOperationTableConditionLocked(
          condition.definition_id, condition.subject, false,
          observed_details("operation_reset_accepted"), correlations(CanonicalValue(nullptr)),
          OperationConditionKind::kRuntimeCollision, operation_id);
      active_runtime_collisions_.erase(operation_id);
    } else {
      ForgetRefusedOperationConditionLocked(kRuntimeCollisionAlarm, operation_id);
      ForgetRefusedOperationConditionLocked(kRuntimeMonitorObservationLostAlarm,
                                            operation_id);
      ForgetRefusedOperationConditionLocked(kRuntimeMonitorChecksStoppedAlarm,
                                            operation_id);
    }
  }
  // 2. The runtime monitor's Stop (a collision, lost measured robot state, or
  //    collision checks that did not run) raises its own alarm while no Reset
  //    is accepted.
  if (!reset_accepted && complete && monitor_stop.is_object()) {
    const auto &reason = RequiredMember(monitor_stop, "reason");
    Require(IsRuntimeMonitorReason(reason), "headless_operation_event_invalid",
            "kernel runtime-collision Stop event has invalid scope");
    const auto alarm =
        reason.as_string() == manyforge::core::error_codes::kRuntimeMonitorObservationLost
            ? kRuntimeMonitorObservationLostAlarm
        : reason.as_string() == manyforge::core::error_codes::kRuntimeMonitorChecksStopped
            ? kRuntimeMonitorChecksStoppedAlarm
            : kRuntimeCollisionAlarm;
    const auto request_key = RequiredString(monitor_stop, "request_key");
    Require(IsRequestKeyText(request_key), kOperationTableInvalid,
            "operations table Stop request key is invalid");
    SubmitOperationTableConditionLocked(
        std::string(alarm),
        subject(), true,
        CanonicalValue(CanonicalValue::Object{
            {"reason", reason},
            {"source_label", RequiredMember(monitor_stop, "source_label")},
            {"stop_event_id", stop_event},
        }),
        CanonicalValue(CanonicalValue::Object{
            {"operation_id", Number(operation_number, "operation_id")},
            {"program_run_id", program_run_id},
            {"request_key", CanonicalValue(request_key)},
        }),
        OperationConditionKind::kRuntimeCollision, operation_id);
  }
  // 3. The motion fault. A runtime collision is its own condition; an
  //    ordinary proven Program failure with no Stop is annunciated by the Run.
  bool runtime_collision_outcome = active_runtime_collisions_.contains(operation_id);
  bool ordinary_program_failure = false;
  if (complete) {
    const auto &participants = RequiredArray(entry, "participants");
    for (const auto &participant : participants) {
      const auto &error = RequiredMember(participant, "terminal_error");
      if (error.is_object() && IsRuntimeMonitorReason(RequiredMember(error, "reason")))
        runtime_collision_outcome = true;
    }
    ordinary_program_failure =
        CapturedFailureEffectsKnown(participants) && stop_event.is_null();
  }
  const bool active = IsOperationFaultState(state_name) && !reset_accepted &&
                      !runtime_collision_outcome && !ordinary_program_failure;
  const bool clear = reset_accepted || IsNormalOperationTerminal(state_name);
  if (active && complete) {
    SubmitOperationTableConditionLocked(
        "manyforge.motion.execution_fault", subject(), true,
        CanonicalValue(CanonicalValue::Object{
            {"operation_revision", Number(revision, "operation_revision")},
            {"state", CanonicalValue(state_name)},
            {"terminal_error", TableTerminalError(RequiredMember(entry, "terminal_error"))},
        }),
        correlations(CanonicalValue(nullptr)), OperationConditionKind::kExecutionFault,
        operation_id);
  } else if (clear) {
    const auto found = active_operation_faults_.find(operation_id);
    if (found == active_operation_faults_.end()) {
      ForgetRefusedOperationConditionLocked("manyforge.motion.execution_fault",
                                            operation_id);
      return;
    }
    const auto subject_of_alarm = found->second;
    SubmitOperationTableConditionLocked(
        "manyforge.motion.execution_fault", subject_of_alarm, false,
        observed_details(reset_accepted ? "operation_reset_accepted"
                                        : "operation_normal_terminal"),
        correlations(CanonicalValue(nullptr)), OperationConditionKind::kExecutionFault,
        operation_id);
    active_operation_faults_.erase(operation_id);
  }
}

void HeadlessApiCore::SubmitOperationTableConditionLocked(
    std::string definition_id, AlarmSubject subject, const bool active,
    CanonicalValue details, CanonicalValue correlations,
    const OperationConditionKind kind, const std::string &operation_id) {
  Require(operation_adapter_.source_sequence < kMaximumSafeInteger,
          "headless_adapter_sequence_exhausted",
          "operation alarm adapter sequence is exhausted");
  ++operation_adapter_.source_sequence;
  // Observed from Kernel memory: the time is this observation's; the
  // History row of the same change carries the journal's evidence.
  AlarmConditionUpdate update{
      std::move(definition_id),
      std::move(subject),
      active,
      operation_adapter_.source_id,
      operation_adapter_.source_epoch,
      operation_adapter_.source_sequence,
      clocks_.monotonic_time_ns(),
      clocks_.wall_time_text(),
      AlarmConditionQuality::kGood,
      CanonicalValue(CanonicalValue::Object{}),
      std::move(details),
      {},
      std::move(correlations),
  };
  if (const auto mutation =
          SubmitOperationConditionLocked(std::move(update), kind, operation_id)) {
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
    alarm_owner_.ReplaceResourceRegistry(
        AlarmResourceRegistry{*bundle_id, *graph_revision, std::move(resources)});
    alarm_registry_identity_ = std::move(identity);
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
}

namespace {

std::string RefusedConditionKey(const std::string_view definition_id,
                                const std::string_view tracking_id) {
  return std::string(definition_id) + '\n' + std::string(tracking_id);
}

} // namespace

void HeadlessApiCore::TrackAdmittedOperationConditionLocked(
    const OperationConditionKind kind, const std::string &tracking_id,
    const AlarmConditionUpdate &update) {
  const auto &subject = update.subject;
  switch (kind) {
  case OperationConditionKind::kExecutionFault:
    active_operation_faults_.insert_or_assign(tracking_id, subject);
    break;
  case OperationConditionKind::kRuntimeCollision:
    active_runtime_collisions_.insert_or_assign(
        tracking_id, RuntimeMonitorStopCondition{update.definition_id, subject});
    break;
  case OperationConditionKind::kProgramFailure:
    break; // AlarmLifecycleOwner is the sole Program condition registry.
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
                                            entry.update);
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
      TrackAdmittedOperationConditionLocked(kind, tracking_id, update);
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
      static_cast<void>(historian_.TryEnqueue(std::move(event)));
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
      static_cast<void>(historian_.TryEnqueue(std::move(event)));
    } catch (const std::exception &error) {
      historian_.ReportDeliveryFailure("historian_projection_failure",
                                       error.what());
    } catch (...) {
      historian_.ReportDeliveryFailure(
          "historian_projection_failure",
          "historian delivery raised an unknown failure");
    }
  }
  return identities;
}

void HeadlessApiCore::SynchronizeHistoryLossAlarmLocked() {
  // A record History could not take or write is dropped and counted by
  // History; it never makes the alarm owner unhealthy. This alarm only says
  // so, and returns to normal once History has written its queue empty.
  const bool losing = !historian_.Health().available;
  if (!losing && !history_loss_adapter_.last_update.has_value()) {
    return; // Never raised in this start: nothing to return to normal.
  }
  try {
    const auto mutation = SubmitAdapterUpdateLocked(
        history_loss_adapter_, std::string(kHistoryMissingRecordsAlarm),
        AlarmSubject{"cell", config_.cell_id, {}}, losing,
        CanonicalValue(CanonicalValue::Object{}),
        CanonicalValue(CanonicalValue::Object{}));
    static_cast<void>(DeliverFactsLocked(mutation.lifecycle_facts));
  } catch (const OperatorAlarmError &) {
    // The owner already counts the rejected update and names an exhausted
    // capacity as health. The unchanged cursor resubmits on the next refresh.
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

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/deployment_authoring.hpp>
#include <manyforge_deployment/expression.hpp>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_headless/headless_api_core.hpp"

namespace py = pybind11;

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
namespace deployment = manyforge::deployment;

py::dict
DeploymentDraftValue(const deployment::CompiledDeploymentDraft &compiled) {
  py::dict result;
  result["canonical_json"] = compiled.canonical_json;
  result["deployment_artifact_id"] = compiled.deployment_artifact_id;
  result["deployment_revision"] = compiled.deployment_revision;
  result["graph_revision"] = compiled.graph_revision;
  return result;
}

py::dict CompileDeploymentDraft(const std::string &source,
                                std::optional<bool> migrate_jerk = {}) {
  const auto compiled = [&]() {
    py::gil_scoped_release release;
    return migrate_jerk.has_value()
               ? deployment::migrate_controlled_stop_jerk_choice(source,
                                                                 *migrate_jerk)
               : deployment::compile_deployment_draft(source);
  }();
  return DeploymentDraftValue(compiled);
}

CanonicalValue ErrorValue(const std::string_view code,
                          const std::string_view message) {
  return CanonicalValue(CanonicalValue::Object{
      {"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue(std::string(code))},
                    {"message", CanonicalValue(std::string(message))},
                })},
      {"ok", CanonicalValue(false)},
  });
}

template <typename Callback> std::string Invoke(Callback &&callback) {
  try {
    auto value = callback();
    return content_identity::canonical_json(
        CanonicalValue(CanonicalValue::Object{
            {"ok", CanonicalValue(true)},
            {"value", std::move(value)},
        }));
  } catch (const OperatorAlarmError &error) {
    return content_identity::canonical_json(
        ErrorValue(error.code(), error.what()));
  } catch (const EventHistorianError &error) {
    return content_identity::canonical_json(
        ErrorValue(error.code(), error.what()));
  } catch (const RuntimeSnapshotError &error) {
    return content_identity::canonical_json(
        ErrorValue("headless_runtime_snapshot_unavailable", error.what()));
  } catch (const SourceSnapshotError &error) {
    return content_identity::canonical_json(
        ErrorValue("headless_source_snapshot_unavailable", error.what()));
  } catch (const std::exception &error) {
    return content_identity::canonical_json(
        ErrorValue("headless_api_internal_failure", error.what()));
  } catch (...) {
    return content_identity::canonical_json(ErrorValue(
        "headless_api_internal_failure", "unknown headless API failure"));
  }
}

py::dict AssetError(const std::string_view code,
                    const std::string_view message) {
  py::dict error;
  error["code"] = py::str(code);
  error["message"] = py::str(message);
  py::dict result;
  result["error"] = std::move(error);
  result["ok"] = py::bool_(false);
  return result;
}

template <typename Callback> py::dict InvokeAsset(Callback &&callback) {
  try {
    ViewerModelAsset asset = [&callback]() {
      py::gil_scoped_release release;
      return callback();
    }();
    const auto logical_uri = asset.logical_uri();
    const CanonicalValue metadata(CanonicalValue::Object{
        {"asset_kind", CanonicalValue(std::string(asset.asset_kind()))},
        {"byte_size", CanonicalValue(static_cast<double>(asset.byte_size()))},
        {"cell_id", CanonicalValue(std::string(asset.cell_id()))},
        {"content_digest", CanonicalValue(asset.content_digest().value())},
        {"graph_revision", CanonicalValue(std::string(asset.graph_revision()))},
        {"logical_uri", logical_uri.has_value()
                            ? CanonicalValue(std::string(*logical_uri))
                            : CanonicalValue(nullptr)},
        {"media_type", CanonicalValue(std::string(asset.media_type()))},
        {"product_contract_id",
         CanonicalValue(asset.product_contract_id().value())},
        {"product_contract_revision",
         CanonicalValue(asset.product_contract_revision().value())},
        {"product_id", CanonicalValue(asset.product_id().value())},
        {"retained_bundle_id",
         CanonicalValue(asset.retained_bundle_id().value())},
        {"schema_version",
         CanonicalValue(std::string(kViewerModelAssetSchemaVersion))},
        {"viewer_model_revision",
         CanonicalValue(asset.viewer_model_revision().value())},
    });
    py::dict result;
    result["bytes"] = py::bytes(asset.bytes().data(), asset.bytes().size());
    result["metadata"] = py::str(content_identity::canonical_json(metadata));
    result["ok"] = py::bool_(true);
    return result;
  } catch (const OperatorAlarmError &error) {
    return AssetError(error.code(), error.what());
  } catch (const std::exception &error) {
    return AssetError("headless_api_internal_failure", error.what());
  } catch (...) {
    return AssetError("headless_api_internal_failure",
                      "unknown headless API failure");
  }
}

AlarmLifecycleState ParseState(const std::string_view state) {
  if (state == "active_unacknowledged") {
    return AlarmLifecycleState::kActiveUnacknowledged;
  }
  if (state == "active_acknowledged") {
    return AlarmLifecycleState::kActiveAcknowledged;
  }
  if (state == "returned_unacknowledged") {
    return AlarmLifecycleState::kReturnedUnacknowledged;
  }
  if (state == "returned_acknowledged") {
    return AlarmLifecycleState::kReturnedAcknowledged;
  }
  throw OperatorAlarmError("operator_alarm_state_filter_invalid",
                           "alarm state filter is unsupported");
}

std::set<AlarmLifecycleState>
ParseStates(const std::vector<std::string> &states) {
  std::set<AlarmLifecycleState> result;
  for (const auto &state : states) {
    result.insert(ParseState(state));
  }
  return result;
}

AlarmSnapshotOrdering ParseOrdering(const std::string_view ordering) {
  if (ordering == "default") {
    return AlarmSnapshotOrdering::kDefault;
  }
  if (ordering == "attention_newest") {
    return AlarmSnapshotOrdering::kAttentionNewest;
  }
  throw OperatorAlarmError("operator_alarm_ordering_invalid",
                           "alarm snapshot ordering is unsupported");
}

class HeadlessApiBridge final {
public:
  HeadlessApiBridge(std::string projection_root, std::string install_root,
                    std::string artifact_root,
                    std::string node_catalog_path,
                    std::string node_environment_manifest_path,
                    std::string alarm_catalog_path,
                    std::string historian_database_path, std::string cell_id,
                    std::string owner_id,
                    std::optional<std::string> owner_epoch,
                    const std::uint64_t freshness_milliseconds,
                    const std::size_t historian_queue_capacity,
                    const std::size_t historian_record_retention)
      : core_(std::make_unique<HeadlessApiCore>(HeadlessApiCoreConfig{
            std::move(projection_root),
            std::move(install_root),
            std::move(artifact_root),
            std::move(node_catalog_path),
            std::move(node_environment_manifest_path),
            std::move(alarm_catalog_path),
            std::move(historian_database_path),
            std::move(cell_id),
            std::move(owner_id),
            owner_epoch.has_value() ? std::move(*owner_epoch)
                                    : MintHeadlessOwnerEpoch("headless-api-"),
            RuntimeSnapshotFreshnessPolicy{
                MillisecondsToNanoseconds(freshness_milliseconds),
                MillisecondsToNanoseconds(freshness_milliseconds),
                MillisecondsToNanoseconds(freshness_milliseconds),
            },
            historian_queue_capacity,
            historian_record_retention,
        })) {}

  std::string RuntimeSnapshot() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->RuntimeSnapshot(); });
  }

  std::string NodeEnvironmentProjection() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->NodeEnvironmentProjection(); });
  }

  std::string SourceModelProjection() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->SourceModelProjection(); });
  }

  std::string SourceModelContext() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->SourceModelContext(); });
  }

  std::string AssistantModeContext(std::string mode_id) {
    py::gil_scoped_release release;
    return Invoke([this, mode_id = std::move(mode_id)]() {
      return core_->AssistantModeContext(mode_id);
    });
  }

  std::string AppliedProgramSelection() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->AppliedProgramSelection(); });
  }

  std::string AppliedProgramStatus() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->AppliedProgramStatus(); });
  }

  std::string CommittedScenarioActivation() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->CommittedScenarioActivation(); });
  }

  py::dict ViewerModelUrdf() {
    return InvokeAsset([this]() { return core_->ViewerModelUrdf(); });
  }

  py::dict ViewerModelResource(std::string expected_retained_bundle_id,
                               std::string logical_uri) {
    return InvokeAsset(
        [this,
         expected_retained_bundle_id = std::move(expected_retained_bundle_id),
         logical_uri = std::move(logical_uri)]() {
          return core_->ViewerModelResource(expected_retained_bundle_id,
                                            logical_uri);
        });
  }

  std::string AssistantModeIndex() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->AssistantModeIndex(); });
  }

  std::string AssistantModeProjection(std::string mode_id) {
    py::gil_scoped_release release;
    return Invoke([this, mode_id = std::move(mode_id)]() {
      return core_->AssistantModeProjection(mode_id);
    });
  }

  std::string AlarmSnapshot(const std::optional<std::size_t> limit,
                            const std::vector<std::string> &states,
                            const std::string &ordering) {
    py::gil_scoped_release release;
    return Invoke([this, limit, states, ordering]() {
      return core_->AlarmSnapshot(limit, ParseStates(states),
                                  ParseOrdering(ordering));
    });
  }

  std::string AlarmMetrics() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->AlarmMetrics(); });
  }

  std::string OperatorAlarmMotionAttention() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->OperatorAlarmMotionAttention(); });
  }

  std::string RefreshAlarmSources() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->RefreshAlarmSources(); });
  }

  std::string Acknowledge(
      std::string condition_id, std::string activation_id,
      const std::uint64_t expected_record_revision, std::string request_id,
      std::string request_owner_epoch, std::string source_label,
      std::optional<std::string> correlation_id, std::string transport_kind,
      std::string adapter_id, std::string assurance,
      std::optional<std::string> trusted_principal_id,
      std::optional<std::string> reason, const bool confirmed) {
    AlarmCallerAttribution caller{
        std::move(request_id),     std::move(request_owner_epoch),
        std::move(source_label),   std::move(correlation_id),
        std::move(transport_kind), std::move(adapter_id),
        std::move(assurance),      std::move(trusted_principal_id)};
    py::gil_scoped_release release;
    return Invoke([this, condition_id = std::move(condition_id),
                   activation_id = std::move(activation_id),
                   expected_record_revision, caller = std::move(caller),
                   reason = std::move(reason), confirmed]() mutable {
      return core_->AcknowledgeAlarm(
          std::move(condition_id), std::move(activation_id),
          expected_record_revision, std::move(caller), std::move(reason),
          confirmed);
    });
  }

  std::string AcknowledgeAll(
      const std::vector<std::tuple<std::string, std::string, std::uint64_t>>
          &raw_members,
      const std::uint64_t expected_generation, std::string request_id,
      std::string request_owner_epoch, std::string source_label,
      std::optional<std::string> correlation_id, std::string transport_kind,
      std::string adapter_id, std::string assurance,
      std::optional<std::string> trusted_principal_id,
      std::optional<std::string> reason, const bool confirmed) {
    std::vector<AlarmAcknowledgementMember> members;
    members.reserve(raw_members.size());
    for (const auto &[condition_id, activation_id, record_revision] :
         raw_members) {
      members.push_back({condition_id, activation_id, record_revision});
    }
    AlarmCallerAttribution caller{
        std::move(request_id),     std::move(request_owner_epoch),
        std::move(source_label),   std::move(correlation_id),
        std::move(transport_kind), std::move(adapter_id),
        std::move(assurance),      std::move(trusted_principal_id)};
    py::gil_scoped_release release;
    return Invoke([this, members = std::move(members), expected_generation,
                   caller = std::move(caller), reason = std::move(reason),
                   confirmed]() mutable {
      return core_->AcknowledgeAllAlarms(std::move(members),
                                         expected_generation, std::move(caller),
                                         std::move(reason), confirmed);
    });
  }

  std::string History(const std::optional<std::int64_t> start_ns,
                      const std::optional<std::int64_t> end_ns,
                      const std::vector<std::string> &components,
                      const std::vector<std::string> &event_types,
                      const std::optional<std::uint64_t> before_sequence,
                      const std::size_t limit) {
    HistorianQuery query{
        start_ns,
        end_ns,
        std::set<std::string>(components.begin(), components.end()),
        std::set<std::string>(event_types.begin(), event_types.end()),
        before_sequence,
        limit};
    py::gil_scoped_release release;
    return Invoke([this, query = std::move(query)]() {
      return core_->HistoryPage(query);
    });
  }

  std::string HistoryQuery(const std::optional<std::int64_t> start_ns,
                             const std::optional<std::int64_t> end_ns,
                             const std::vector<std::string> &components,
                             const std::vector<std::string> &event_types,
                             const std::optional<std::uint64_t> before_sequence,
                             const std::size_t limit,
                             const std::optional<std::int64_t> start_utc_ns,
                             const std::optional<std::int64_t> end_utc_ns,
                             const std::vector<std::string> &severities,
                             const std::map<std::string, std::string> &correlations,
                             const std::optional<std::string> &alarm_phase,
                             const std::string &search,
                             const std::optional<std::string> &clock_owner,
                             const bool full_records,
                             const std::optional<std::uint64_t> capture_id) {
    HistorianQuery query{
        start_ns,
        end_ns,
        std::set<std::string>(components.begin(), components.end()),
        std::set<std::string>(event_types.begin(), event_types.end()),
        before_sequence,
        limit, start_utc_ns, end_utc_ns, std::set<std::string>(severities.begin(), severities.end()), correlations, alarm_phase, search, clock_owner,
        capture_id};
    py::gil_scoped_release release;
    return Invoke([this, query = std::move(query), full_records]() {
      return core_->HistoryQueryPage(query, full_records);
    });
  }

  std::string HistoryCaptures(const std::optional<std::uint64_t> before_capture_id,
                              const std::size_t limit) {
    py::gil_scoped_release release;
    return Invoke([&] { return core_->HistoryCapturePage(before_capture_id, limit); });
  }

  std::string HistoryDiscovery(const std::string &kind, const std::string &after, std::size_t limit) {
    py::gil_scoped_release release;
    return Invoke([&] { return core_->HistoryDiscovery(kind, after, limit); });
  }

  std::string HistorianHealth() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->HistorianHealth(); });
  }

  std::string HistoryDetail(const std::uint64_t sequence) {
    py::gil_scoped_release release;
    return Invoke(
        [this, sequence]() { return core_->HistoryDetail(sequence); });
  }

  std::string
  RecordClientIncident(std::string incident_id, std::string incident_kind,
                       std::string phase, std::string control_session_id,
                       std::string client_incarnation, std::string source_label,
                       std::string reason_code,
                       const std::uint64_t occurrence_count,
                       const std::optional<std::uint64_t> maximum_delay_ns,
                       const std::uint64_t first_observed_ns,
                       const std::uint64_t last_observed_ns,
                       std::map<std::string, std::string> causal_context) {
    py::gil_scoped_release release;
    return Invoke(
        [this, incident_id = std::move(incident_id),
         incident_kind = std::move(incident_kind), phase = std::move(phase),
         control_session_id = std::move(control_session_id),
         client_incarnation = std::move(client_incarnation),
         source_label = std::move(source_label),
         reason_code = std::move(reason_code), occurrence_count,
         maximum_delay_ns, first_observed_ns, last_observed_ns,
         causal_context = std::move(causal_context)]() mutable {
          return core_->RecordClientIncident(
              std::move(incident_id), std::move(incident_kind),
              std::move(phase), std::move(control_session_id),
              std::move(client_incarnation), std::move(source_label),
              std::move(reason_code), occurrence_count, maximum_delay_ns,
              first_observed_ns, last_observed_ns, std::move(causal_context));
        });
  }

  std::string CommitProgramModelAttempt(std::string record_json) {
    py::gil_scoped_release release;
    return Invoke([this, record_json = std::move(record_json)]() mutable {
      CanonicalValue record;
      try {
        record = deployment::parse_strict_json_model_yaml(record_json).root();
      } catch (const std::exception &error) {
        throw OperatorAlarmError(
            "headless_program_model_attempt_invalid",
            "Program model-attempt record is not strict JSON: " +
                std::string(error.what()));
      }
      if (content_identity::canonical_json(record) != record_json) {
        throw OperatorAlarmError(
            "headless_program_model_attempt_invalid",
            "Program model-attempt record is not canonical JSON");
      }
      return core_->CommitProgramModelAttempt(std::move(record));
    });
  }

  std::string RecordGripperConfirmation(const std::string& record_json) {
    py::gil_scoped_release release;
    return Invoke([this, &record_json]() {
      if (record_json.size() > 8192U)
        throw OperatorAlarmError("gripper_confirmation_trace_invalid",
                                 "Gripper confirmation trace exceeds its byte bound");
      auto record = deployment::parse_strict_json_model_yaml(record_json).root();
      return core_->RecordGripperConfirmation(std::move(record));
    });
  }

  std::string RecordProgramDiagnostic(const std::string& record_json) {
    py::gil_scoped_release release;
    return Invoke([this, &record_json]() {
      if (record_json.size() > 64U * 1024U) {
        throw OperatorAlarmError("program_diagnostic_invalid",
                                 "Program diagnostic exceeds its byte bound");
      }
      auto record = deployment::parse_strict_json_model_yaml(record_json).root();
      return core_->RecordProgramDiagnostic(std::move(record));
    });
  }

  std::string ReconcileHistorianHealth(
      const std::uint64_t expected_generation, std::string request_id,
      std::string request_owner_epoch, std::string source_label,
      std::optional<std::string> correlation_id, std::string transport_kind,
      std::string adapter_id, std::string assurance,
      std::optional<std::string> trusted_principal_id) {
    AlarmCallerAttribution caller{
        std::move(request_id),     std::move(request_owner_epoch),
        std::move(source_label),   std::move(correlation_id),
        std::move(transport_kind), std::move(adapter_id),
        std::move(assurance),      std::move(trusted_principal_id)};
    py::gil_scoped_release release;
    return Invoke(
        [this, expected_generation, caller = std::move(caller)]() mutable {
          return core_->ReconcileHistorianHealth(expected_generation,
                                                 std::move(caller));
        });
  }

  std::string IngestOperationEvent(
      std::string source_owner_epoch, const std::uint64_t source_sequence,
      std::string record_kind, std::optional<std::string> request_id,
      std::optional<std::string> operation_id,
      const std::optional<std::uint64_t> operation_revision,
      const std::int64_t occurred_at_ns, std::string occurred_at_wall,
      const bool effect_pending, std::string payload_json,
      std::string payload_digest) {
    py::gil_scoped_release release;
    return Invoke([this, source_owner_epoch = std::move(source_owner_epoch),
                   source_sequence, record_kind = std::move(record_kind),
                   request_id = std::move(request_id),
                   operation_id = std::move(operation_id), operation_revision,
                   occurred_at_ns,
                   occurred_at_wall = std::move(occurred_at_wall),
                   effect_pending, payload_json = std::move(payload_json),
                   payload_digest = std::move(payload_digest)]() mutable {
      CanonicalValue payload;
      try {
        payload = deployment::parse_strict_json_model_yaml(payload_json).root();
      } catch (const std::exception &error) {
        throw OperatorAlarmError(
            "headless_operation_event_invalid",
            "operation event payload is not strict JSON: " +
                std::string(error.what()));
      }
      if (content_identity::canonical_json(payload) != payload_json) {
        throw OperatorAlarmError(
            "headless_operation_event_invalid",
            "operation event payload is not canonical JSON");
      }
      return core_->IngestOperationEvent(
          std::move(source_owner_epoch), source_sequence,
          std::move(record_kind), std::move(request_id),
          std::move(operation_id), operation_revision, occurred_at_ns,
          std::move(occurred_at_wall), effect_pending, std::move(payload),
          std::move(payload_digest));
    });
  }

  std::string CompleteOperationEventSnapshot(
      std::string source_owner_epoch,
      const std::uint64_t through_source_sequence,
      std::optional<std::string> current_stop_fault_json) {
    py::gil_scoped_release release;
    return Invoke([this, source_owner_epoch = std::move(source_owner_epoch),
                   through_source_sequence,
                   current_stop_fault_json =
                       std::move(current_stop_fault_json)]() mutable {
      std::optional<CanonicalValue> current_stop_fault;
      if (current_stop_fault_json.has_value()) {
        try {
          current_stop_fault =
              deployment::parse_strict_json_model_yaml(*current_stop_fault_json)
                  .root();
        } catch (const std::exception &error) {
          throw OperatorAlarmError(
              "headless_operation_event_invalid",
              "current Stop-fault condition is not strict JSON: " +
                  std::string(error.what()));
        }
        if (content_identity::canonical_json(*current_stop_fault) !=
            *current_stop_fault_json) {
          throw OperatorAlarmError(
              "headless_operation_event_invalid",
              "current Stop-fault condition is not canonical JSON");
        }
      }
      return core_->CompleteOperationEventSnapshot(
          std::move(source_owner_epoch), through_source_sequence,
          std::move(current_stop_fault));
    });
  }

  std::string MarkOperationEventSourceUnknown(std::string source_owner_epoch) {
    py::gil_scoped_release release;
    return Invoke(
        [this, source_owner_epoch = std::move(source_owner_epoch)]() mutable {
          return core_->MarkOperationEventSourceUnknown(
              std::move(source_owner_epoch));
        });
  }

  void Shutdown() {
    py::gil_scoped_release release;
    core_->Shutdown();
  }

private:
  static std::int64_t
  MillisecondsToNanoseconds(const std::uint64_t milliseconds) {
    constexpr std::uint64_t kMaximumSafeInteger = 9007199254740991ULL;
    if (milliseconds == 0U || milliseconds > kMaximumSafeInteger / 1000000U) {
      throw std::invalid_argument(
          "freshness_milliseconds is outside the supported range");
    }
    return static_cast<std::int64_t>(milliseconds * 1000000U);
  }

  std::unique_ptr<HeadlessApiCore> core_;
};

namespace expression = manyforge::deployment::expression;

PYBIND11_CONSTINIT py::gil_safe_call_once_and_store<py::object>
    expression_error_type;

constexpr std::array<std::pair<std::string_view, expression::ValueKind>, 5>
    kExpressionKinds{{
        {"bool", expression::ValueKind::kBool},
        {"int64", expression::ValueKind::kInt64},
        {"float64", expression::ValueKind::kFloat64},
        {"string", expression::ValueKind::kString},
        {"pose", expression::ValueKind::kPose},
    }};

expression::ValueKind ExpressionKind(const std::string &name) {
  for (const auto &[text, kind] : kExpressionKinds) {
    if (text == name) {
      return kind;
    }
  }
  throw py::value_error("unknown expression type '" + name + "'");
}

std::string_view ExpressionKindName(const expression::ValueKind kind) {
  for (const auto &[text, value] : kExpressionKinds) {
    if (value == kind) {
      return text;
    }
  }
  throw std::logic_error("expression kind has no Python name");
}

[[noreturn]] void
RaiseExpressionError(const std::vector<expression::Diagnostic> &diagnostics) {
  py::list rows;
  for (const auto &diagnostic : diagnostics) {
    py::dict row;
    row["code"] = diagnostic.code;
    row["message"] = diagnostic.message;
    row["position"] = diagnostic.position;
    row["length"] = diagnostic.length;
    row["name"] = diagnostic.name;
    rows.append(std::move(row));
  }
  const auto &type = expression_error_type.get_stored();
  py::object error = type(diagnostics.front().message);
  error.attr("diagnostics") = std::move(rows);
  PyErr_SetObject(type.ptr(), error.ptr());
  throw py::error_already_set();
}

expression::ValueType ExpressionType(const py::handle &value) {
  if (py::isinstance<py::str>(value)) {
    return {ExpressionKind(value.cast<std::string>()), {}};
  }
  expression::ValueType type{expression::ValueKind::kRecord, {}};
  for (const auto &[name, field] : value.cast<py::dict>()) {
    type.fields.push_back({name.cast<std::string>(), ExpressionType(field)});
  }
  return type;
}

std::map<std::string, expression::ValueType, std::less<>>
ExpressionTypes(const py::dict &section) {
  std::map<std::string, expression::ValueType, std::less<>> types;
  for (const auto &[name, type] : section) {
    types.emplace(name.cast<std::string>(), ExpressionType(type));
  }
  return types;
}

// Program-shaped mappings: {"parameters": {...}, "keys": {...}}.
void RequireExpressionSections(const py::dict &mapping) {
  for (const auto &[name, section] : mapping) {
    const auto text = name.cast<std::string>();
    if (text != "parameters" && text != "keys") {
      throw py::value_error("expression mapping has unknown section '" + text +
                            "'");
    }
  }
}

py::dict ExpressionSection(const py::dict &mapping, const char *name) {
  return mapping.contains(name) ? mapping[name].cast<py::dict>() : py::dict();
}

double ExpressionNumber(const py::handle &value) {
  if (py::isinstance<py::bool_>(value) ||
      !(py::isinstance<py::int_>(value) || py::isinstance<py::float_>(value))) {
    throw py::type_error("expression float64 value must be a number");
  }
  return value.cast<double>();
}

template <std::size_t Size>
std::array<double, Size> ExpressionNumbers(const py::handle &value) {
  const auto items = value.cast<py::sequence>();
  if (items.size() != Size) {
    throw py::value_error("expression pose list must have " +
                          std::to_string(Size) + " numbers");
  }
  std::array<double, Size> result{};
  for (std::size_t index = 0; index < Size; ++index) {
    result[index] = ExpressionNumber(items[index]);
  }
  return result;
}

expression::Value ExpressionValue(const py::handle &value,
                                  const expression::ValueType &type) {
  switch (type.kind) {
  case expression::ValueKind::kBool:
    if (!py::isinstance<py::bool_>(value)) {
      throw py::type_error("expression bool value must be a bool");
    }
    return {value.cast<bool>()};
  case expression::ValueKind::kInt64:
    if (py::isinstance<py::bool_>(value) || !py::isinstance<py::int_>(value)) {
      throw py::type_error("expression int64 value must be an int");
    }
    return {value.cast<std::int64_t>()};
  case expression::ValueKind::kFloat64:
    return {ExpressionNumber(value)};
  case expression::ValueKind::kString:
    if (!py::isinstance<py::str>(value)) {
      throw py::type_error("expression string value must be a str");
    }
    return {value.cast<std::string>()};
  case expression::ValueKind::kPose: {
    const auto pose = value.cast<py::dict>();
    for (const auto &[name, item] : pose) {
      const auto text = name.cast<std::string>();
      if (text != "position_m" && text != "orientation" &&
          text != "frame_ref") {
        throw py::value_error("expression pose has unknown field '" + text +
                              "'");
      }
    }
    // Expressions read a pose in its own frame, so frame_ref is not needed.
    const auto orientation = pose["orientation"].cast<py::dict>();
    if (orientation.size() != 1U) {
      throw py::value_error(
          "expression pose orientation needs exactly one of rpy_deg or xyzw");
    }
    expression::Pose result;
    result.position_m = ExpressionNumbers<3>(pose["position_m"]);
    if (orientation.contains("rpy_deg")) {
      result.orientation =
          expression::RpyDeg{ExpressionNumbers<3>(orientation["rpy_deg"])};
    } else if (orientation.contains("xyzw")) {
      result.orientation =
          expression::Xyzw{ExpressionNumbers<4>(orientation["xyzw"])};
    } else {
      throw py::value_error(
          "expression pose orientation needs exactly one of rpy_deg or xyzw");
    }
    return {std::move(result)};
  }
  case expression::ValueKind::kRecord: {
    expression::Record record;
    for (const auto &[name, item] : value.cast<py::dict>()) {
      const auto text = name.cast<std::string>();
      const auto field =
          std::find_if(type.fields.begin(), type.fields.end(),
                       [&](const auto &row) { return row.name == text; });
      if (field == type.fields.end()) {
        throw py::value_error("expression record field '" + text +
                              "' is not declared");
      }
      record.push_back({text, ExpressionValue(item, field->type)});
    }
    return {std::move(record)};
  }
  }
  throw std::logic_error("expression kind has no Python conversion");
}

py::object ExpressionResult(const expression::Value &value) {
  return std::visit(
      [](const auto &item) -> py::object {
        using Item = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<Item, expression::Pose> ||
                      std::is_same_v<Item, expression::Record>) {
          throw std::logic_error("expression result is not a scalar");
        } else {
          return py::cast(item);
        }
      },
      value.data);
}

expression::ParsedExpression
ParseExpression(const std::string &text,
                const std::optional<py::dict> &limits) {
  expression::Limits bounds;
  if (limits.has_value()) {
    for (const auto &[name, value] : *limits) {
      const auto text_name = name.cast<std::string>();
      const auto number = value.cast<std::size_t>();
      if (text_name == "text_length") {
        bounds.text_length = number;
      } else if (text_name == "nesting_depth") {
        bounds.nesting_depth = number;
      } else if (text_name == "tokens") {
        bounds.tokens = number;
      } else if (text_name == "syntax_nodes") {
        bounds.syntax_nodes = number;
      } else if (text_name == "evaluation_steps") {
        bounds.evaluation_steps = number;
      } else {
        throw py::value_error("unknown expression limit '" + text_name + "'");
      }
    }
  }
  auto parsed = expression::parse(text, bounds);
  if (auto *diagnostic = std::get_if<expression::Diagnostic>(&parsed)) {
    RaiseExpressionError({std::move(*diagnostic)});
  }
  return std::get<expression::ParsedExpression>(std::move(parsed));
}

expression::CheckedExpression
CheckExpression(const expression::ParsedExpression &parsed,
                const py::dict &environment, const std::string &destination) {
  RequireExpressionSections(environment);
  expression::TypeEnvironment types;
  types.parameters =
      ExpressionTypes(ExpressionSection(environment, "parameters"));
  types.keys = ExpressionTypes(ExpressionSection(environment, "keys"));
  auto checked = expression::check(parsed, types, ExpressionKind(destination));
  if (auto *diagnostics =
          std::get_if<std::vector<expression::Diagnostic>>(&checked)) {
    RaiseExpressionError(*diagnostics);
  }
  return std::get<expression::CheckedExpression>(std::move(checked));
}

py::object EvaluateExpression(const expression::CheckedExpression &checked,
                              const py::dict &values) {
  RequireExpressionSections(values);
  const auto parameters = ExpressionSection(values, "parameters");
  const auto keys = ExpressionSection(values, "keys");
  expression::ValueSnapshot snapshot;
  // Only referenced names are converted; a missing one is the engine's
  // missing_value diagnostic.
  for (const auto &[reference, type] : checked.references()) {
    const bool parameter = reference.source == expression::Source::kParameter;
    const auto &section = parameter ? parameters : keys;
    if (!section.contains(reference.name)) {
      continue;
    }
    (parameter ? snapshot.parameters : snapshot.keys)
        .emplace(reference.name,
                 ExpressionValue(section[py::str(reference.name)], type));
  }
  auto result = expression::evaluate(checked, snapshot);
  if (auto *diagnostic = std::get_if<expression::Diagnostic>(&result)) {
    RaiseExpressionError({std::move(*diagnostic)});
  }
  return ExpressionResult(std::get<expression::Value>(result));
}

void RegisterExpressionModule(py::module_ &parent) {
  auto module = parent.def_submodule(
      "expression", "Thin binding over the one C++ Program expression engine");
  expression_error_type.call_once_and_store_result([&]() {
    return py::reinterpret_steal<py::object>(
        PyErr_NewException("manyforge_headless_py.expression.ExpressionError",
                           PyExc_ValueError, nullptr));
  });
  module.attr("ExpressionError") = expression_error_type.get_stored();
  py::dict limits;
  limits["text_length"] = expression::kMaxTextLength;
  limits["nesting_depth"] = expression::kMaxNestingDepth;
  limits["tokens"] = expression::kMaxTokens;
  limits["syntax_nodes"] = expression::kMaxSyntaxNodes;
  limits["evaluation_steps"] = expression::kMaxEvaluationSteps;
  module.attr("LIMITS") = limits;
  py::class_<expression::CheckedExpression>(module, "CheckedExpression")
      .def_property_readonly("text",
                             [](const expression::CheckedExpression &self) {
                               return std::string(self.text());
                             })
      .def_property_readonly("result_type",
                             [](const expression::CheckedExpression &self) {
                               return std::string(
                                   ExpressionKindName(self.result_kind()));
                             })
      .def_property_readonly(
          "references",
          [](const expression::CheckedExpression &self) {
            py::list rows;
            for (const auto &[reference, type] : self.references()) {
              rows.append(py::make_tuple(
                  reference.source == expression::Source::kParameter ? "param"
                                                                     : "key",
                  reference.name));
            }
            return rows;
          })
      .def("evaluate", &EvaluateExpression, py::arg("values"));
  py::class_<expression::ParsedExpression>(module, "ParsedExpression")
      .def_property_readonly("text",
                             [](const expression::ParsedExpression &self) {
                               return std::string(self.text());
                             })
      .def("check", &CheckExpression, py::arg("environment"),
           py::arg("destination"));
  module.def("parse", &ParseExpression, py::arg("text"),
             py::arg("limits") = std::nullopt);
}

} // namespace
} // namespace manyforge::headless

PYBIND11_MODULE(manyforge_headless_py, module) {
  using manyforge::headless::HeadlessApiBridge;

  module.doc() =
      "Thin binding over the C++ ManyForge headless runtime API core";
  manyforge::headless::RegisterExpressionModule(module);
  module.def(
      "compile_deployment_draft",
      [](const std::string &source) {
        return manyforge::headless::CompileDeploymentDraft(source);
      },
      py::arg("source"));
  module.def(
      "migrate_controlled_stop_jerk_choice",
      [](const std::string &source, bool enabled) {
        return manyforge::headless::CompileDeploymentDraft(source, enabled);
      },
      py::arg("source"), py::arg("enabled").noconvert());
  module.def(
      "import_moveit_angular_jerk_limits",
      [](const std::string &source, const std::string &moveit_source,
         const std::string &source_id, const std::string &runtime_unit_id,
         const std::string &component_id, const std::string &target_id,
         const std::vector<std::string> &source_joint_names) {
        const auto compiled = [&] {
          py::gil_scoped_release release;
          return manyforge::deployment::import_moveit_angular_jerk_limits(
              source, moveit_source, source_id, runtime_unit_id, component_id,
              target_id, source_joint_names);
        }();
        return manyforge::headless::DeploymentDraftValue(compiled);
      },
      py::arg("source"), py::arg("moveit_source"), py::arg("source_id"),
      py::arg("runtime_unit_id"), py::arg("component_id"), py::arg("target_id"),
      py::arg("source_joint_names"));
  module.def(
      "read_control_source_file_hint",
      [](const std::string &path) {
        return manyforge::headless::ReadControlSourceFileHint(path);
      },
      py::arg("path"), py::call_guard<py::gil_scoped_release>());
  py::class_<HeadlessApiBridge>(module, "HeadlessApiCore")
      .def(py::init<std::string, std::string, std::string,
                    std::string, std::string, std::string, std::string,
                    std::string, std::string, std::optional<std::string>,
                    std::uint64_t, std::size_t, std::size_t>(),
           py::arg("projection_root"), py::arg("install_root"),
           py::arg("artifact_root"), py::arg("node_catalog_path"),
           py::arg("node_environment_manifest_path"),
           py::arg("alarm_catalog_path"), py::arg("historian_database_path"),
           py::arg("cell_id"), py::arg("owner_id"),
           py::arg("owner_epoch") = std::nullopt,
           py::arg("freshness_milliseconds") = 2000U,
           py::arg("historian_queue_capacity") = 1024U,
           py::arg("historian_record_retention") = 100000U)
      .def("runtime_snapshot", &HeadlessApiBridge::RuntimeSnapshot)
      .def("node_environment_projection",
           &HeadlessApiBridge::NodeEnvironmentProjection)
      .def("source_model_projection", &HeadlessApiBridge::SourceModelProjection)
      .def("source_model_context", &HeadlessApiBridge::SourceModelContext)
      .def("assistant_mode_context", &HeadlessApiBridge::AssistantModeContext,
           py::arg("mode_id"))
      .def("applied_program_selection",
           &HeadlessApiBridge::AppliedProgramSelection)
      .def("applied_program_status",
           &HeadlessApiBridge::AppliedProgramStatus)
      .def("committed_scenario_activation",
           &HeadlessApiBridge::CommittedScenarioActivation)
      .def("viewer_model_urdf", &HeadlessApiBridge::ViewerModelUrdf)
      .def("viewer_model_resource", &HeadlessApiBridge::ViewerModelResource,
           py::arg("expected_retained_bundle_id"), py::arg("logical_uri"))
      .def("assistant_mode_index", &HeadlessApiBridge::AssistantModeIndex)
      .def("assistant_mode_projection",
           &HeadlessApiBridge::AssistantModeProjection, py::arg("mode_id"))
      .def("alarm_snapshot", &HeadlessApiBridge::AlarmSnapshot,
           py::arg("limit") = std::optional<std::size_t>(32U),
           py::arg("states") = std::vector<std::string>{},
           py::arg("ordering") = "default")
      .def("alarm_metrics", &HeadlessApiBridge::AlarmMetrics)
      .def("operator_alarm_motion_attention",
           &HeadlessApiBridge::OperatorAlarmMotionAttention)
      .def("refresh_alarm_sources", &HeadlessApiBridge::RefreshAlarmSources)
      .def("acknowledge_alarm", &HeadlessApiBridge::Acknowledge,
           py::arg("condition_id"), py::arg("activation_id"),
           py::arg("expected_record_revision"), py::arg("request_id"),
           py::arg("request_owner_epoch"), py::arg("source_label"),
           py::arg("correlation_id") = std::nullopt,
           py::arg("transport_kind") = "http",
           py::arg("adapter_id") = "manyforge.headless.http.v1",
           py::arg("assurance") = "unverified",
           py::arg("trusted_principal_id") = std::nullopt,
           py::arg("reason") = std::nullopt, py::arg("confirmed") = false)
      .def("acknowledge_all", &HeadlessApiBridge::AcknowledgeAll,
           py::arg("members"), py::arg("expected_generation"),
           py::arg("request_id"), py::arg("request_owner_epoch"),
           py::arg("source_label"), py::arg("correlation_id") = std::nullopt,
           py::arg("transport_kind") = "http",
           py::arg("adapter_id") = "manyforge.headless.http.v1",
           py::arg("assurance") = "unverified",
           py::arg("trusted_principal_id") = std::nullopt,
           py::arg("reason") = std::nullopt, py::arg("confirmed") = false)
      .def("history", &HeadlessApiBridge::History,
           py::arg("start_ns") = std::nullopt, py::arg("end_ns") = std::nullopt,
           py::arg("components") = std::vector<std::string>{},
           py::arg("event_types") = std::vector<std::string>{},
           py::arg("before_sequence") = std::nullopt, py::arg("limit") = 1000U)
      .def("history_query", &HeadlessApiBridge::HistoryQuery,
           py::arg("start_ns") = std::nullopt, py::arg("end_ns") = std::nullopt,
           py::arg("components") = std::vector<std::string>{},
           py::arg("event_types") = std::vector<std::string>{},
           py::arg("before_sequence") = std::nullopt, py::arg("limit") = 32U,
           py::arg("start_utc_ns") = std::nullopt,
           py::arg("end_utc_ns") = std::nullopt,
           py::arg("severities") = std::vector<std::string>{},
           py::arg("correlations") = std::map<std::string, std::string>{},
           py::arg("alarm_phase") = std::nullopt, py::arg("search") = "",
           py::arg("clock_owner") = std::nullopt,
           py::arg("full_records") = false,
           py::arg("capture_id") = std::nullopt)
      .def("history_captures", &HeadlessApiBridge::HistoryCaptures,
           py::arg("before_capture_id") = std::nullopt, py::arg("limit") = 32U)
      .def("history_detail", &HeadlessApiBridge::HistoryDetail,
           py::arg("sequence"))
      .def("history_discovery", &HeadlessApiBridge::HistoryDiscovery,
           py::arg("kind"), py::arg("after") = "", py::arg("limit") = 32U)
      .def("record_client_incident", &HeadlessApiBridge::RecordClientIncident,
           py::arg("incident_id"), py::arg("incident_kind"), py::arg("phase"),
           py::arg("control_session_id"), py::arg("client_incarnation"),
           py::arg("source_label"), py::arg("reason_code"),
           py::arg("occurrence_count"), py::arg("maximum_delay_ns"),
           py::arg("first_observed_ns"), py::arg("last_observed_ns"),
           py::arg("causal_context") = std::map<std::string, std::string>{})
      .def("historian_health", &HeadlessApiBridge::HistorianHealth)
      .def("record_program_diagnostic",
           &HeadlessApiBridge::RecordProgramDiagnostic, py::arg("record_json"))
      .def("record_gripper_confirmation",
           &HeadlessApiBridge::RecordGripperConfirmation,
           py::arg("record_json"))
      .def("commit_program_model_attempt",
           &HeadlessApiBridge::CommitProgramModelAttempt,
           py::arg("record_json"))
      .def("reconcile_historian_health",
           &HeadlessApiBridge::ReconcileHistorianHealth,
           py::arg("expected_generation"), py::arg("request_id"),
           py::arg("request_owner_epoch"), py::arg("source_label"),
           py::arg("correlation_id") = std::nullopt,
           py::arg("transport_kind") = "http",
           py::arg("adapter_id") = "manyforge.headless.http.v1",
           py::arg("assurance") = "unverified",
           py::arg("trusted_principal_id") = std::nullopt)
      .def("ingest_operation_event", &HeadlessApiBridge::IngestOperationEvent,
           py::arg("source_owner_epoch"), py::arg("source_sequence"),
           py::arg("record_kind"), py::arg("request_id"),
           py::arg("operation_id"), py::arg("operation_revision"),
           py::arg("occurred_at_ns"), py::arg("occurred_at_wall"),
           py::arg("effect_pending"), py::arg("payload_json"),
           py::arg("payload_digest"))
      .def("complete_operation_event_snapshot",
           &HeadlessApiBridge::CompleteOperationEventSnapshot,
           py::arg("source_owner_epoch"), py::arg("through_source_sequence"),
           py::arg("current_stop_fault_json") = std::nullopt)
      .def("mark_operation_event_source_unknown",
           &HeadlessApiBridge::MarkOperationEventSourceUnknown,
           py::arg("source_owner_epoch"))
      .def("shutdown", &HeadlessApiBridge::Shutdown);
}

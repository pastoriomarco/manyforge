#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <cstddef>
#include <cstdint>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/deployment_authoring.hpp>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "manyforge_headless/headless_api_core.hpp"

namespace py = pybind11;

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
namespace deployment = manyforge::deployment;

py::dict CompileDeploymentDraft(const std::string &source) {
  const auto compiled = [&]() {
    py::gil_scoped_release release;
    return deployment::compile_deployment_draft(source);
  }();
  py::dict result;
  result["canonical_json"] = compiled.canonical_json;
  result["deployment_artifact_id"] = compiled.deployment_artifact_id;
  result["deployment_revision"] = compiled.deployment_revision;
  result["graph_revision"] = compiled.graph_revision;
  return result;
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
  } catch (const ProgramApplyError &error) {
    return content_identity::canonical_json(
        ErrorValue(error.code(), error.what()));
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
                    std::string program_selection_state_root,
                    std::string node_catalog_path,
                    std::string node_environment_manifest_path,
                    std::string alarm_catalog_path,
                    std::string historian_database_path, std::string cell_id,
                    std::vector<std::string> resource_ids, std::string owner_id,
                    std::optional<std::string> owner_epoch,
                    const std::uint64_t freshness_milliseconds,
                    const std::size_t historian_queue_capacity,
                    const std::size_t historian_record_retention,
                    const std::size_t mutation_replay_capacity)
      : core_(std::make_unique<HeadlessApiCore>(HeadlessApiCoreConfig{
            std::move(projection_root),
            std::move(install_root),
            std::move(artifact_root),
            std::move(program_selection_state_root),
            std::move(node_catalog_path),
            std::move(node_environment_manifest_path),
            std::move(alarm_catalog_path),
            std::move(historian_database_path),
            std::move(cell_id),
            std::set<std::string>(resource_ids.begin(), resource_ids.end()),
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
            mutation_replay_capacity,
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

  std::string ScenarioCatalogProjection() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->ScenarioCatalogProjection(); });
  }

  std::string ScenarioProgramSelection(std::string scenario_id) {
    py::gil_scoped_release release;
    return Invoke([this, scenario_id = std::move(scenario_id)]() {
      return core_->ScenarioProgramSelection(scenario_id);
    });
  }

  std::string ScenarioLoadSelection(std::string scenario_id) {
    py::gil_scoped_release release;
    return Invoke([this, scenario_id = std::move(scenario_id)]() {
      return core_->ScenarioLoadSelection(scenario_id);
    });
  }

  std::string PrepareProgramApply(std::string scenario_id,
                                  std::string scenario_catalog_revision,
                                  std::string canonical_program_json) {
    py::gil_scoped_release release;
    return Invoke(
        [this, scenario_id = std::move(scenario_id),
         scenario_catalog_revision = std::move(scenario_catalog_revision),
         canonical_program_json = std::move(canonical_program_json)]() {
          return core_->PrepareProgramApply(
              scenario_id, scenario_catalog_revision, canonical_program_json);
        });
  }

  std::string ReplayProgramApply(
      std::string scenario_id, std::string scenario_catalog_revision,
      std::string program_id, std::string program_revision,
      std::optional<std::string> expected_applied_record_digest,
      std::string request_owner_epoch, std::string request_id) {
    py::gil_scoped_release release;
    return Invoke(
        [this, scenario_id = std::move(scenario_id),
         scenario_catalog_revision = std::move(scenario_catalog_revision),
         program_id = std::move(program_id),
         program_revision = std::move(program_revision),
         expected_applied_record_digest =
             std::move(expected_applied_record_digest),
         request_owner_epoch = std::move(request_owner_epoch),
         request_id = std::move(request_id)]() {
          return core_->ReplayProgramApply(
              scenario_id, scenario_catalog_revision, program_id,
              program_revision, expected_applied_record_digest,
              request_owner_epoch, request_id);
        });
  }

  std::string CommitProgramApply(
      std::string canonical_preparation_json,
      std::optional<std::string> expected_applied_record_digest,
      std::string request_owner_epoch, std::string request_id) {
    py::gil_scoped_release release;
    return Invoke(
        [this,
         canonical_preparation_json = std::move(canonical_preparation_json),
         expected_applied_record_digest =
             std::move(expected_applied_record_digest),
         request_owner_epoch = std::move(request_owner_epoch),
         request_id = std::move(request_id)]() {
          return core_->CommitProgramApply(canonical_preparation_json,
                                           expected_applied_record_digest,
                                           request_owner_epoch, request_id);
        });
  }

  std::string AppliedProgramSelection() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->AppliedProgramSelection(); });
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

  std::string HistorianHealth() {
    py::gil_scoped_release release;
    return Invoke([this]() { return core_->HistorianHealth(); });
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

  std::string
  CompleteOperationEventSnapshot(std::string source_owner_epoch,
                                 const std::uint64_t through_source_sequence) {
    py::gil_scoped_release release;
    return Invoke([this, source_owner_epoch = std::move(source_owner_epoch),
                   through_source_sequence]() mutable {
      return core_->CompleteOperationEventSnapshot(
          std::move(source_owner_epoch), through_source_sequence);
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

} // namespace
} // namespace manyforge::headless

PYBIND11_MODULE(manyforge_headless_py, module) {
  using manyforge::headless::HeadlessApiBridge;

  module.doc() =
      "Thin binding over the C++ ManyForge headless runtime API core";
  module.def("compile_deployment_draft",
             &manyforge::headless::CompileDeploymentDraft,
             py::arg("source"));
  py::class_<HeadlessApiBridge>(module, "HeadlessApiCore")
      .def(py::init<std::string, std::string, std::string, std::string,
                    std::string, std::string, std::string, std::string,
                    std::string,
                    std::vector<std::string>, std::string,
                    std::optional<std::string>, std::uint64_t, std::size_t,
                    std::size_t, std::size_t>(),
           py::arg("projection_root"), py::arg("install_root"),
           py::arg("artifact_root"),
           py::arg("program_selection_state_root"),
           py::arg("node_catalog_path"),
           py::arg("node_environment_manifest_path"),
           py::arg("alarm_catalog_path"),
           py::arg("historian_database_path"), py::arg("cell_id"),
           py::arg("resource_ids"), py::arg("owner_id"),
           py::arg("owner_epoch") = std::nullopt,
           py::arg("freshness_milliseconds") = 2000U,
           py::arg("historian_queue_capacity") = 1024U,
           py::arg("historian_record_retention") = 100000U,
           py::arg("mutation_replay_capacity") = 4096U)
      .def("runtime_snapshot", &HeadlessApiBridge::RuntimeSnapshot)
      .def("node_environment_projection",
           &HeadlessApiBridge::NodeEnvironmentProjection)
      .def("source_model_projection", &HeadlessApiBridge::SourceModelProjection)
      .def("source_model_context", &HeadlessApiBridge::SourceModelContext)
      .def("assistant_mode_context", &HeadlessApiBridge::AssistantModeContext,
           py::arg("mode_id"))
      .def("scenario_catalog_projection",
           &HeadlessApiBridge::ScenarioCatalogProjection)
      .def("scenario_program_selection",
           &HeadlessApiBridge::ScenarioProgramSelection, py::arg("scenario_id"))
      .def("scenario_load_selection", &HeadlessApiBridge::ScenarioLoadSelection,
           py::arg("scenario_id"))
      .def("prepare_program_apply", &HeadlessApiBridge::PrepareProgramApply,
           py::arg("scenario_id"), py::arg("scenario_catalog_revision"),
           py::arg("canonical_program_json"))
      .def("replay_program_apply", &HeadlessApiBridge::ReplayProgramApply,
           py::arg("scenario_id"), py::arg("scenario_catalog_revision"),
           py::arg("program_id"), py::arg("program_revision"),
           py::arg("expected_applied_record_digest"),
           py::arg("request_owner_epoch"), py::arg("request_id"))
      .def("commit_program_apply", &HeadlessApiBridge::CommitProgramApply,
           py::arg("canonical_preparation_json"),
           py::arg("expected_applied_record_digest"),
           py::arg("request_owner_epoch"), py::arg("request_id"))
      .def("applied_program_selection",
           &HeadlessApiBridge::AppliedProgramSelection)
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
      .def("historian_health", &HeadlessApiBridge::HistorianHealth)
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
           py::arg("source_owner_epoch"), py::arg("through_source_sequence"))
      .def("mark_operation_event_source_unknown",
           &HeadlessApiBridge::MarkOperationEventSourceUnknown,
           py::arg("source_owner_epoch"))
      .def("shutdown", &HeadlessApiBridge::Shutdown);
}

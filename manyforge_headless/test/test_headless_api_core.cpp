#include "io_retained_fixture.hpp"
#include <gtest/gtest.h>
#include <manyforge_deployment/prepared_activation_input.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_core/types/component_incident.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_headless/headless_api_core.hpp"
#include "manyforge_headless/assistant_mode_projection.hpp"
#include "manyforge_headless/event_historian.hpp"
#include "manyforge_headless/operator_alarm_owner.hpp"
#include "manyforge_headless/node_environment_projection.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_contract.hpp"
#include "manyforge_headless/source_model_projection.hpp"
#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
using namespace std::chrono_literals;

constexpr std::string_view kCellId = "c1_build_authority_cell";

TEST(HeadlessTypedErrors, ExistingRuntimeErrorsKeepTheirCodesAndMessages) {
  const EventHistorianError historian("historian_error", "history unavailable");
  const OperatorAlarmError alarm("alarm_error", "alarm unavailable");
  const AssistantModeProjectionError assistant("assistant_error", "mode unavailable");
  for (const auto *error : std::vector<const std::runtime_error *>{&historian, &alarm, &assistant}) {
    const auto cause = core::CauseOf(*error, "fallback", "headless_api");
    EXPECT_EQ(cause.producer, "headless_api");
    EXPECT_NE(cause.error.code, "fallback");
    EXPECT_EQ(cause.error.message, error->what());
  }
  EXPECT_EQ(core::CauseOf(historian, "fallback", "headless_api").error.code, "historian_error");
  EXPECT_EQ(core::CauseOf(alarm, "fallback", "headless_api").error.code, "alarm_error");
  EXPECT_EQ(core::CauseOf(assistant, "fallback", "headless_api").error.code, "assistant_error");
  EXPECT_STREQ(historian.what(), "historian_error: history unavailable");
  EXPECT_STREQ(alarm.what(), "alarm_error: alarm unavailable");
  EXPECT_STREQ(assistant.what(), "mode unavailable");
}

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-api-core-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("headless API test mkdtemp failed");
    }
    path_ = created;
    std::filesystem::create_directory(publications());
    std::filesystem::create_directory(artifacts());
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] std::filesystem::path publications() const {
    return path_ / "publications";
  }
  [[nodiscard]] std::filesystem::path artifacts() const {
    return path_ / "artifacts";
  }
  [[nodiscard]] std::filesystem::path history() const {
    return path_ / "history.sqlite3";
  }
private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

const CanonicalValue &Member(const CanonicalValue &value,
                             const std::string_view name) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("missing test member " + std::string(name));
  }
  return *member;
}

std::string Text(const CanonicalValue &value, const std::string_view name) {
  return std::get<std::string>(Member(value, name).storage());
}

std::uint64_t Unsigned(const CanonicalValue &value,
                       const std::string_view name) {
  return static_cast<std::uint64_t>(
      std::get<double>(Member(value, name).storage()));
}

const CanonicalValue::Array &Array(const CanonicalValue &value,
                                   const std::string_view name) {
  return std::get<CanonicalValue::Array>(Member(value, name).storage());
}

AlarmCallerAttribution Caller(std::string request_id = "request.1",
                              std::string owner_epoch = "api.epoch.1") {
  return {std::move(request_id),
          std::move(owner_epoch),
          "operator_ui",
          "correlation.1",
          "test",
          "manyforge.test.alarm",
          "authenticated:test",
          "principal.operator"};
}

HeadlessApiCoreConfig
Config(const TemporaryDirectory &directory, std::string owner_epoch,
       std::filesystem::path package_root =
           MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT) {
  return {
      directory.publications(),
      std::move(package_root),
      directory.artifacts(),
      MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
      MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST,
      MANYFORGE_HEADLESS_TEST_ALARM_CATALOG,
      directory.history(),
      std::string(kCellId),
      "headless-api.cell.i5",
      std::move(owner_epoch),
      RuntimeSnapshotFreshnessPolicy{10'000'000'000LL, 10'000'000'000LL,
                                     10'000'000'000LL},
      32U,
      1000U,
  };
}

std::string Digest(const char digit) {
  return "sha256:" + std::string(64U, digit);
}

CanonicalValue ProgramModelAttemptRecord(const bool success = true) {
  CanonicalValue error_code = success ? CanonicalValue(nullptr)
                                      : CanonicalValue("model_request_timeout");
  CanonicalValue response_digest =
      success ? CanonicalValue(Digest('5')) : CanonicalValue(nullptr);
  CanonicalValue validated_output_digest =
      success ? CanonicalValue(Digest('6')) : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"argument_digests", CanonicalValue(CanonicalValue::Object{
                               {"object", CanonicalValue(Digest('4'))},
                           })},
      {"argument_values", CanonicalValue(nullptr)},
      {"assistant_projection_digest", CanonicalValue(Digest('1'))},
      {"attachment_digest", CanonicalValue(nullptr)},
      {"audit_policy", CanonicalValue(CanonicalValue::Object{
                           {"redact_argument_values", CanonicalValue(true)},
                           {"store_full_prompts", CanonicalValue(false)},
                           {"store_full_responses", CanonicalValue(false)},
                       })},
      {"error_code", std::move(error_code)},
      {"error_message",
       CanonicalValue(success ? "" : "model request timed out")},
      {"external_attempt_index", CanonicalValue(success ? 1.0 : 2.0)},
      {"external_resource_alias", CanonicalValue("site_llm")},
      {"latency_ms", CanonicalValue(12.5)},
      {"model_attempt_id",
       CanonicalValue(success ? "model-attempt:one" : "model-attempt:two")},
      {"model_endpoint_alias", CanonicalValue("planning_model")},
      {"model_id", CanonicalValue("model-a")},
      {"model_version", CanonicalValue(success ? "model-a.2026-08" : "")},
      {"node_attempt_id", CanonicalValue("node-attempt:one")},
      {"node_instance_id", CanonicalValue("query-model")},
      {"occurred_at_ns", CanonicalValue(success ? 1200.0 : 1300.0)},
      {"occurred_at_wall", CanonicalValue(success ? "2026-08-10T10:00:00Z"
                                                  : "2026-08-10T10:00:01Z")},
      {"outcome", CanonicalValue(success ? "success" : "timeout")},
      {"program_run_id", CanonicalValue("program-run:one")},
      {"prompt_digest", CanonicalValue(Digest('3'))},
      {"prompt_text", CanonicalValue(nullptr)},
      {"prompt_template_digest", CanonicalValue(Digest('2'))},
      {"prompt_template_id", CanonicalValue("select")},
      {"prompt_template_version", CanonicalValue("1")},
      {"response_digest", std::move(response_digest)},
      {"response_value", CanonicalValue(nullptr)},
      {"schema_version", CanonicalValue("manyforge.program_model_attempt.v1")},
      {"usage", success ? CanonicalValue(CanonicalValue::Object{
                              {"total_tokens", CanonicalValue(17.0)},
                          })
                        : CanonicalValue(nullptr)},
      {"validated_output_digest", std::move(validated_output_digest)},
  });
}

HeadlessApiCoreClocks Clocks() {
  auto monotonic = std::make_shared<std::int64_t>(1000);
  return {
      []() {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
      },
      [monotonic]() { return ++*monotonic; },
      []() { return "2026-08-05T00:00:00Z"; },
  };
}

CanonicalValue ManagerPayloadFor(const deployment::RetainedBundle &bundle) {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();
  const auto *cell_graph =
      bundle.normalized_root().find_object_member("cell_graph");
  if (cell_graph == nullptr || !cell_graph->is_object()) {
    throw std::runtime_error("test retained bundle has no cell graph");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active",
       CanonicalValue(CanonicalValue::Object{
           {"canonical_empty", CanonicalValue(
                bundle.retained_bundle_id() == admission.retained_bundle_id() &&
                bundle.value().cell_graph.graph_revision == admission.graph_revision())},
           {"cell_graph", *cell_graph},
           {"graph_revision",
            CanonicalValue(bundle.value().cell_graph.graph_revision.value())},
           {"publication", CanonicalValue(CanonicalValue::Object{})},
           {"retained_bundle_id",
            CanonicalValue(bundle.retained_bundle_id().value())},
       })},
      {"api_version",
       CanonicalValue(std::string(kManagerSourcePayloadApiVersion))},
      {"apply", CanonicalValue(CanonicalValue::Object{})},
      {"cell_id", CanonicalValue(bundle.value().cell_graph.cell_id.value())},
      {"manager_ready", CanonicalValue(true)},
      {"runtime",
       CanonicalValue(CanonicalValue::Object{
           {"converged", CanonicalValue(true)},
           {"desired_units", CanonicalValue(CanonicalValue::Array{})},
           {"observed_units", CanonicalValue(CanonicalValue::Array{})},
       })},
  });
}

CanonicalValue ManagerPayload() {
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  return ManagerPayloadFor(source.load_verified().retained_bundle());
}

CanonicalValue LogicalScenePayload() {
  return CanonicalValue(CanonicalValue::Object{
      {"access", CanonicalValue(CanonicalValue::Object{})},
      {"api_version",
       CanonicalValue(std::string(kLogicalSceneSourcePayloadApiVersion))},
      {"snapshot", CanonicalValue(nullptr)},
  });
}

CanonicalValue SceneRuntimePayload(const std::string &state,
                                   const std::uint64_t generation,
                                   const std::string_view cell_id = kCellId) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kSceneRuntimeSourcePayloadApiVersion))},
      {"cell_id", CanonicalValue(std::string(cell_id))},
      {"state", CanonicalValue(CanonicalValue::Object{
                    {"api_version", CanonicalValue(std::string(
                                        kSceneRuntimeStateSnapshotApiVersion))},
                    {"error", state == "failed" ? CanonicalValue("seed failed")
                                                : CanonicalValue(nullptr)},
                    {"initialization_generation",
                     CanonicalValue(static_cast<double>(generation))},
                    {"initialization_token", CanonicalValue("scene-token")},
                    {"pose_sources", CanonicalValue(CanonicalValue::Array{})},
                    {"reason", CanonicalValue("test")},
                    {"state", CanonicalValue(state)},
                })},
  });
}

CanonicalValue OperationJournalPayload(
    const std::string &owner_epoch, const std::string &record_kind,
    const std::string &state, const CanonicalValue &terminal_error,
    const std::optional<std::string> &control = std::nullopt,
    const std::string &resource = "resource.arm.a",
    const std::string &operation_id = "operation.test.1") {
  const CanonicalValue result = control.has_value()
                                    ? CanonicalValue(CanonicalValue::Object{
                                          {"control", CanonicalValue(*control)},
                                      })
                                    : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v8")},
      {"effect_pending", CanonicalValue(record_kind != "state_observed" &&
                                        record_kind != "operation_prepared")},
      {"intent", CanonicalValue(CanonicalValue::Object{})},
      {"operation_state",
       CanonicalValue(CanonicalValue::Object{
           {"operation_id", CanonicalValue(operation_id)},
           {"operation_revision", CanonicalValue(1.0)},
           {"participants",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"command_id", CanonicalValue(operation_id + ".command.1")},
                    {"command_generation", CanonicalValue(1.0)},
                    {"resources", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(resource)})},
                    {"terminal_error", terminal_error},
                })})},
           {"program_binding_id", CanonicalValue("program-binding.test")},
           {"program_run_id", CanonicalValue("program-run.test")},
           {"state", CanonicalValue(state)},
       })},
      {"owner_epoch", CanonicalValue(owner_epoch)},
      {"record_kind", CanonicalValue(record_kind)},
      {"result", result},
      {"runtime_scale_status", CanonicalValue(nullptr)},
  });
}

CanonicalValue
StopAcceptanceProjectionPayload(const std::string &owner_epoch,
                                const std::string &entry_state = "active") {
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", CanonicalValue(CanonicalValue::Object{
                         {"entry_state", CanonicalValue(entry_state)},
                         {"owner_epoch", CanonicalValue(owner_epoch)},
                         {"request_id", CanonicalValue("stop-request.1")},
                         {"stop_generation", CanonicalValue(1.0)},
                         {"stop_id", CanonicalValue("stop.test.1")},
                     })},
      {"accepted", CanonicalValue(CanonicalValue::Object{
                       {"request_id", CanonicalValue("stop-request.1")},
                       {"stop_generation", CanonicalValue(1.0)},
                       {"stop_id", CanonicalValue("stop.test.1")},
                   })},
      {"affected_state",
       CanonicalValue(CanonicalValue::Object{
           {"operations", CanonicalValue(CanonicalValue::Array{})},
           {"resources", CanonicalValue(CanonicalValue::Array{})},
       })},
      {"api_version",
       CanonicalValue("manyforge.stop_acceptance_projection.v8")},
      {"intent", CanonicalValue(CanonicalValue::Object{
                     {"intent_kind", CanonicalValue("full_stop")},
                     {"value", CanonicalValue(CanonicalValue::Object{})},
                 })},
      {"record_kind", CanonicalValue("stop_acceptance_projected")},
  });
}

CanonicalValue
StopTerminalProjectionPayload(const std::string &owner_epoch,
                              const std::string &result = "succeeded_nominal",
                              const std::string &state = "succeeded",
                              const bool stop_fault = false) {
  const auto fault_latch =
      stop_fault
          ? CanonicalValue(CanonicalValue::Object{
                {"captured_operation_count", CanonicalValue(1.0)},
                {"captured_resource_count", CanonicalValue(1.0)},
                {"cell_id", CanonicalValue(std::string(kCellId))},
                {"latch_generation", CanonicalValue(1.0)},
                {"latch_id", CanonicalValue("stop-fault.test.1")},
                {"persistence_degraded", CanonicalValue(false)},
                {"reason_code", CanonicalValue("stop_escalated")},
                {"recovery_assessment_revision", CanonicalValue(1.0)},
                {"recovery_status", CanonicalValue("waiting_for_observation")},
                {"state", CanonicalValue("latched")},
                {"stop_certificate",
                 CanonicalValue(CanonicalValue::Object{
                     {"stop_certificate_id",
                      CanonicalValue("stop-certificate.test.1")},
                     {"stop_certificate_revision", CanonicalValue(1.0)},
                     {"stop_generation", CanonicalValue(1.0)},
                     {"stop_id", CanonicalValue("stop.test.1")},
                 })},
                {"stop_generation", CanonicalValue(1.0)},
                {"stop_id", CanonicalValue("stop.test.1")},
                {"trigger", CanonicalValue("escalated")},
            })
          : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance", CanonicalValue(CanonicalValue::Object{
                         {"accepted_at_ns", CanonicalValue(100.0)},
                         {"request_id", CanonicalValue("stop-request.1")},
                         {"stop_generation", CanonicalValue(1.0)},
                         {"stop_id", CanonicalValue("stop.test.1")},
                     })},
      {"acceptance_to_terminal_latency_ns", CanonicalValue(1000.0)},
      {"api_version", CanonicalValue("manyforge.stop_terminal_projection.v1")},
      {"attempted_stage_count", CanonicalValue(1.0)},
      {"attempted_stages",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"actual_stop_mode", CanonicalValue("soft_on_path")},
               {"outcome", CanonicalValue("succeeded")},
               {"profile",
                CanonicalValue(CanonicalValue::Object{
                    {"profile_id", CanonicalValue("operator_soft_on_path")},
                })},
           }),
       })},
      {"certificate_evidence",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"evidence_item_id", CanonicalValue("motion-rest")},
               {"state", CanonicalValue("satisfied")},
           }),
       })},
      {"certificate_evidence_count", CanonicalValue(1.0)},
      {"completed_at_ns", CanonicalValue(1100.0)},
      {"details_truncated", CanonicalValue(false)},
      {"durable_projection_digest", CanonicalValue(Digest('8'))},
      {"escalated", CanonicalValue(result == "succeeded_escalated")},
      {"fault_latch", fault_latch},
      {"hard_stop_floor_used", CanonicalValue(false)},
      {"owner_epoch", CanonicalValue(owner_epoch)},
      {"participant_settlement",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"terminal_settlement", CanonicalValue("settled")},
           }),
       })},
      {"participant_settlement_count", CanonicalValue(1.0)},
      {"record_kind", CanonicalValue("stop_terminal_projected")},
      {"request_id", CanonicalValue("stop-request.1")},
      {"result", CanonicalValue(result)},
      {"state", CanonicalValue(state)},
      {"stop_certificate_id", CanonicalValue("stop-certificate.test.1")},
      {"stop_certificate_revision", CanonicalValue(1.0)},
      {"stop_generation", CanonicalValue(1.0)},
      {"stop_id", CanonicalValue("stop.test.1")},
  });
}

CanonicalValue StopFaultClearProjectionPayload(const std::string &owner_epoch) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue("manyforge.stop_fault_clear_projection.v1")},
      {"fault_latch",
       CanonicalValue(CanonicalValue::Object{
           {"captured_operation_count", CanonicalValue(1.0)},
           {"captured_resource_count", CanonicalValue(1.0)},
           {"cell_id", CanonicalValue(std::string(kCellId))},
           {"latch_generation", CanonicalValue(1.0)},
           {"latch_id", CanonicalValue("stop-fault.test.1")},
           {"persistence_degraded", CanonicalValue(false)},
           {"reason_code", CanonicalValue("stop_fault_recovery_cleared")},
           {"recovery_assessment_revision", CanonicalValue(2.0)},
           {"recovery_status", CanonicalValue("cleared")},
           {"state", CanonicalValue("cleared")},
           {"stop_certificate",
            CanonicalValue(CanonicalValue::Object{
                {"stop_certificate_id",
                 CanonicalValue("stop-certificate.test.1")},
                {"stop_certificate_revision", CanonicalValue(1.0)},
                {"stop_generation", CanonicalValue(1.0)},
                {"stop_id", CanonicalValue("stop.test.1")},
            })},
           {"stop_generation", CanonicalValue(1.0)},
           {"stop_id", CanonicalValue("stop.test.1")},
           {"trigger", CanonicalValue("escalated")},
       })},
      {"owner_epoch", CanonicalValue(owner_epoch)},
      {"record_kind", CanonicalValue("stop_fault_cleared")},
      {"request_id", CanonicalValue("stop-fault-reset.test.1")},
      {"result", CanonicalValue(CanonicalValue::Object{
                     {"accepted", CanonicalValue(true)},
                     {"error", CanonicalValue(nullptr)},
                     {"replay_status", CanonicalValue("accepted_new")},
                     {"request_id", CanonicalValue("stop-fault-reset.test.1")},
                 })},
      {"stop_certificate_id", CanonicalValue("stop-certificate.test.1")},
      {"stop_certificate_revision", CanonicalValue(1.0)},
      {"stop_generation", CanonicalValue(1.0)},
      {"stop_id", CanonicalValue("stop.test.1")},
  });
}

CanonicalValue WithField(const CanonicalValue &value,
                         const std::string_view key,
                         CanonicalValue replacement) {
  auto fields = value.as_object();
  for (auto &[name, field] : fields) {
    if (name == key)
      field = std::move(replacement);
  }
  return CanonicalValue(std::move(fields));
}

CanonicalValue ManagerPayloadWithIncompleteApply(
    const std::string_view phase = "runtime_convergence_pending",
    const std::string_view scoped_component = "robot-a",
    const bool empty_predecessor = false) {
  auto payload = ManagerPayload();
  auto active = *payload.find_object_member("active");
  auto graph = *active.find_object_member("cell_graph");
  graph = WithField(
      graph, "controllable_components",
      CanonicalValue(CanonicalValue::Array{CanonicalValue(
          CanonicalValue::Object{
              {"controllable_component_id", CanonicalValue("robot-a")},
              {"runtime_unit_ids", CanonicalValue(CanonicalValue::Array{
                                       CanonicalValue("runtime-a")})},
          })}));
  if (empty_predecessor) {
    graph = WithField(graph, "controllable_components",
                      CanonicalValue(CanonicalValue::Array{}));
    graph = WithField(
        graph, "runtime_units",
        CanonicalValue(CanonicalValue::Array{CanonicalValue(
            CanonicalValue::Object{
                {"runtime_unit_id", CanonicalValue("runtime-a")},
            })}));
  }
  active = WithField(active, "cell_graph", std::move(graph));
  payload = WithField(payload, "active", std::move(active));
  auto runtime = *payload.find_object_member("runtime");
  runtime = WithField(
      runtime, "desired_units",
      CanonicalValue(CanonicalValue::Array{CanonicalValue(
          CanonicalValue::Object{
              {"runtime_unit_id", CanonicalValue("runtime-a")},
          })}));
  payload = WithField(payload, "runtime", std::move(runtime));
  return WithField(
      payload, "apply",
      CanonicalValue(CanonicalValue::Object{
          {"mutation_available", CanonicalValue(true)},
          {"records",
           CanonicalValue(CanonicalValue::Array{
               CanonicalValue(CanonicalValue::Object{
                   {"apply_generation", CanonicalValue(1.0)},
                   {"apply_id", CanonicalValue("apply-transition-1")},
                   {"fence", CanonicalValue(CanonicalValue::Object{
                                 {"state", CanonicalValue("held")},
                                 {"scope", CanonicalValue(
                                      CanonicalValue::Object{
                                          {"controllable_component_ids",
                                           CanonicalValue(CanonicalValue::Array{
                                               CanonicalValue(std::string(
                                                   scoped_component))})},
                                      })},
                             })},
                   {"incomplete", CanonicalValue(true)},
                   {"phase", CanonicalValue(std::string(phase))},
               }),
           })},
          {"unavailable_reason_code", CanonicalValue(nullptr)},
      }));
}

CanonicalValue CurrentStopFaultLatch() {
  return CanonicalValue(CanonicalValue::Object{
      {"captured_operation_ids", CanonicalValue(CanonicalValue::Array{
                                     CanonicalValue("operation.test.1"),
                                 })},
      {"captured_resources", CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue("resource.arm.a"),
                             })},
      {"cell_id", CanonicalValue(std::string(kCellId))},
      {"clear_projection", CanonicalValue(CanonicalValue::Object{
                               {"error", CanonicalValue(nullptr)},
                               {"retry_generation", CanonicalValue(nullptr)},
                               {"state", CanonicalValue("not_applicable")},
                           })},
      {"evidence_refs", CanonicalValue(CanonicalValue::Array{})},
      {"graph_revision", CanonicalValue(Digest('6'))},
      {"latch_generation", CanonicalValue(1.0)},
      {"latch_id", CanonicalValue("stop-fault.test.1")},
      {"observed_triggers",
       CanonicalValue(CanonicalValue::Array{CanonicalValue("escalated")})},
      {"persistence_degraded", CanonicalValue(false)},
      {"reason_code", CanonicalValue("stop_escalated")},
      {"recovery_assessment_revision", CanonicalValue(1.0)},
      {"recovery_status", CanonicalValue("waiting_for_observation")},
      {"retained_bundle_id", CanonicalValue(Digest('7'))},
      {"state", CanonicalValue("latched")},
      {"stop_certificate",
       CanonicalValue(CanonicalValue::Object{
           {"stop_certificate_id", CanonicalValue("stop-certificate.test.1")},
           {"stop_certificate_revision", CanonicalValue(1.0)},
           {"stop_generation", CanonicalValue(1.0)},
           {"stop_id", CanonicalValue("stop.test.1")},
       })},
      {"stop_generation", CanonicalValue(1.0)},
      {"stop_id", CanonicalValue("stop.test.1")},
      {"terminal_projection", CanonicalValue(CanonicalValue::Object{
                                  {"error", CanonicalValue(nullptr)},
                                  {"retry_generation", CanonicalValue(nullptr)},
                                  {"state", CanonicalValue("recorded")},
                              })},
      {"trigger", CanonicalValue("escalated")},
  });
}

CanonicalValue RuntimeCollisionStopAcceptanceProjectionPayload(
    const std::string &owner_epoch, const std::string &resource) {
  return CanonicalValue(CanonicalValue::Object{
      {"acceptance",
       CanonicalValue(CanonicalValue::Object{
           {"entry_state", CanonicalValue("active")},
           {"owner_epoch", CanonicalValue(owner_epoch)},
           {"request_id", CanonicalValue("stop-request.collision.1")},
           {"stop_generation", CanonicalValue(1.0)},
           {"stop_id", CanonicalValue("stop.collision.1")},
       })},
      {"accepted",
       CanonicalValue(CanonicalValue::Object{
           {"request_id", CanonicalValue("stop-request.collision.1")},
           {"stop_generation", CanonicalValue(1.0)},
           {"stop_id", CanonicalValue("stop.collision.1")},
       })},
      {"affected_state",
       CanonicalValue(CanonicalValue::Object{
           {"operations",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"operation_id", CanonicalValue("operation.test.1")},
                    {"operation_revision", CanonicalValue(2.0)},
                    {"participants",
                     CanonicalValue(CanonicalValue::Array{
                         CanonicalValue(CanonicalValue::Object{
                             {"command_id", CanonicalValue("command.collision.1")},
                             {"command_generation", CanonicalValue(1.0)},
                             {"participant_id",
                              CanonicalValue("participant.collision.1")},
                             {"resources",
                              CanonicalValue(CanonicalValue::Array{
                                  CanonicalValue(resource)})},
                             {"terminal_error", CanonicalValue(nullptr)},
                         })})},
                    {"program_binding_id",
                     CanonicalValue("program-binding.test")},
                    {"program_run_id", CanonicalValue("program-run.test")},
                    {"state", CanonicalValue("stopping")},
                })})},
           {"resources", CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(resource)})},
       })},
      {"api_version",
       CanonicalValue("manyforge.stop_acceptance_projection.v8")},
      {"intent",
       CanonicalValue(CanonicalValue::Object{
           {"intent_kind", CanonicalValue("ordinary_stop")},
           {"value",
            CanonicalValue(CanonicalValue::Object{
                {"reason", CanonicalValue("runtime_collision_fault")},
                {"request_attribution",
                 CanonicalValue(CanonicalValue::Object{
                     {"assurance", CanonicalValue("unverified")},
                     {"principal_id", CanonicalValue(nullptr)},
                     {"transport", CanonicalValue("internal")},
                 })},
                {"request_context",
                 CanonicalValue(CanonicalValue::Object{
                     {"correlation_id",
                      CanonicalValue("participant.collision.1")},
                     {"request_id", CanonicalValue("stop-request.collision.1")},
                     {"request_owner_epoch", CanonicalValue(owner_epoch)},
                     {"source_label",
                      CanonicalValue("kernel.runtime_collision")},
                 })},
                {"selector",
                 CanonicalValue(CanonicalValue::Object{
                     {"operations",
                      CanonicalValue(CanonicalValue::Array{
                          CanonicalValue(CanonicalValue::Object{
                              {"expected_operation_revision",
                               CanonicalValue(1.0)},
                              {"expected_stop_generation", CanonicalValue(0.0)},
                              {"operation_id",
                               CanonicalValue("operation.test.1")},
                          })})},
                     {"selector_kind", CanonicalValue("operations")},
                 })},
            })},
       })},
      {"record_kind", CanonicalValue("stop_acceptance_projected")},
  });
}

CanonicalValue JournalField(const CanonicalValue &value, const std::string &field,
                          CanonicalValue replacement) {
  auto object = value.as_object();
  const auto existing =
      std::find_if(object.begin(), object.end(),
                   [&](const auto &row) { return row.first == field; });
  if (existing == object.end())
    object.emplace_back(field, std::move(replacement));
  else
    existing->second = std::move(replacement);
  return CanonicalValue(std::move(object));
}

CanonicalValue CapturedState(const CanonicalValue &state) {
  auto participants = Member(state, "participants").as_array();
  for (auto &participant : participants) {
    if (!participant.find_object_member("participant_id"))
      participant = JournalField(participant, "participant_id", CanonicalValue("participant.collision.1"));
    participant = JournalField(participant, "execution_scope", CanonicalValue(CanonicalValue::Object{
        {"node_instance_id", CanonicalValue("move")},
        {"node_attempt_id", CanonicalValue("move.attempt.1")}}));
  }
  return JournalField(JournalField(state, "participants", CanonicalValue(std::move(participants))),
                   "journal_capture_order", CanonicalValue("1"));
}

CanonicalValue CapturedCollisionStop(const std::string &epoch, const std::string &resource) {
  auto payload = RuntimeCollisionStopAcceptanceProjectionPayload(epoch, resource);
  auto affected = Member(payload, "affected_state");
  auto operations = Member(affected, "operations").as_array();
  operations.front() = CapturedState(operations.front());
  affected = JournalField(affected, "operations", CanonicalValue(std::move(operations)));
  return JournalField(JournalField(payload, "affected_state", affected), "api_version",
                   CanonicalValue("manyforge.stop_acceptance_projection.v8"));
}

CanonicalValue
CollisionFailure(const std::string &message = "first collision") {
  return CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{
                      {"reason", CanonicalValue("runtime_collision_fault")}})},
      {"message", CanonicalValue(message)},
  });
}

CanonicalValue CapturedOperation(const std::string &epoch, const std::string &resource,
                                  const CanonicalValue &error) {
  auto payload = OperationJournalPayload(epoch, "state_observed", "intervention_required",
                                         error, std::nullopt, resource);
  return JournalField(JournalField(payload, "operation_state",
                              CapturedState(Member(payload, "operation_state"))), "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v8"));
}

void IngestJournalEvent(HeadlessApiCore &core, const CanonicalValue &payload,
                      const std::string &epoch, const std::uint64_t sequence) {
  const auto kind = Text(payload, "record_kind");
  const bool stop =
      kind == "stop_acceptance_projected" || kind == "stop_terminal_projected";
  const std::optional<std::string> request =
      kind == "stop_acceptance_projected"
          ? std::optional<std::string>(
                Text(Member(payload, "acceptance"), "request_id"))
          : std::nullopt;
  const auto result = core.IngestOperationEvent(
      epoch, sequence, kind, request,
      stop ? std::nullopt
           : std::optional<std::string>(
                 Text(Member(payload, "operation_state"), "operation_id")),
      stop ? std::nullopt : std::optional<std::uint64_t>(1U), 300 + sequence,
      "2026-08-07T10:00:00.000000007Z",
      stop ? kind == "stop_acceptance_projected"
           : std::get<bool>(Member(payload, "effect_pending").storage()),
      payload,
      content_identity::sha256_digest(
          content_identity::canonical_json(payload)));
  EXPECT_EQ(Text(result, "status"), "ingested");
}

void PublishHealthySources(const TemporaryDirectory &directory,
                           const std::string &scene_state,
                           const std::uint64_t scene_generation = 1U) {
  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  AtomicSourceSnapshotChannel scene(directory.publications(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel runtime(directory.publications(),
                                      "scene-runtime-source.json");
  static_cast<void>(manager.Publish("component_manager",
                                    "component-manager." + std::string(kCellId),
                                    "manager.epoch.1", 1, ManagerPayload()));
  static_cast<void>(scene.Publish("logical_scene",
                                  "logical-scene." + std::string(kCellId),
                                  "scene.epoch.1", 1, LogicalScenePayload()));
  static_cast<void>(
      runtime.Publish("scene_runtime", "scene-runtime." + std::string(kCellId),
                      "scene-runtime.epoch.1", 1,
                      SceneRuntimePayload(scene_state, scene_generation)));
}

void PublishManagerApplyTransition(
    const TemporaryDirectory &directory,
    const std::string_view phase = "runtime_convergence_pending",
    const std::string_view scoped_component = "robot-a",
    const bool empty_predecessor = false) {
  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + std::string(kCellId),
      "manager.epoch.1", 2,
      ManagerPayloadWithIncompleteApply(phase, scoped_component,
                                        empty_predecessor)));
}

/// The successive configurations of the alarm-registry fixture cell: one
/// Lite6, two same-type Lite6 instances, and the canonical-empty
/// configuration without controlled components.
struct RegistryConfigurations final {
  std::shared_ptr<const deployment::RetainedBundle> single;
  std::shared_ptr<const deployment::RetainedBundle> dual;
  std::shared_ptr<const deployment::RetainedBundle> empty;
};

RegistryConfigurations LoadRegistryConfigurations() {
  std::ifstream stream(MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_BUNDLE_IDS_FILE,
                       std::ios::binary);
  std::string single_id;
  std::string dual_id;
  std::string empty_id;
  if (!stream || !std::getline(stream, single_id) ||
      !std::getline(stream, dual_id) || !std::getline(stream, empty_id)) {
    throw std::runtime_error("alarm registry fixture bundle IDs are missing");
  }
  const deployment::FilesystemVerifiedRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT);
  RegistryConfigurations result{
      source.get_verified(core::RetainedBundleId(single_id)),
      source.get_verified(core::RetainedBundleId(dual_id)),
      source.get_verified(core::RetainedBundleId(empty_id))};
  if (result.single == nullptr || result.dual == nullptr ||
      result.empty == nullptr ||
      !result.empty->value().cell_graph.controllable_components.empty()) {
    throw std::runtime_error("alarm registry fixture bundles are missing");
  }
  return result;
}

/// The exact Kernel resources a configuration's control bindings name.
std::vector<std::string> ResourceIds(const deployment::RetainedBundle &bundle) {
  std::set<std::string> resources;
  for (const auto &binding : bundle.value().cell_graph.control_bindings) {
    for (const auto &resource : binding.resource_bindings) {
      resources.insert(resource.resource_id.value());
    }
  }
  return {resources.begin(), resources.end()};
}

HeadlessApiCoreConfig RegistryConfig(const TemporaryDirectory &directory,
                                     std::string owner_epoch) {
  return Config(directory, std::move(owner_epoch),
                MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT);
}

/// Manager publishes `bundle` as the active configuration.
void PublishActiveConfiguration(const TemporaryDirectory &directory,
                                const deployment::RetainedBundle &bundle,
                                const std::uint64_t sequence) {
  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + std::string(kCellId),
      "manager.epoch.1", sequence, ManagerPayloadFor(bundle)));
}

const CanonicalValue &OnlyAlarm(const CanonicalValue &snapshot) {
  const auto &alarms = Array(snapshot, "alarms");
  if (alarms.size() != 1U) {
    throw std::runtime_error("expected one alarm");
  }
  return alarms.front();
}

constexpr std::string_view kAlarmScopeDefinition =
    "manyforge.runtime.alarm_scope_unavailable";

/// The alarm-scope condition record, if the snapshot holds one.
const CanonicalValue *ScopeAlarm(const CanonicalValue &snapshot) {
  for (const auto &alarm : Array(snapshot, "alarms")) {
    if (Text(alarm, "definition_id") == kAlarmScopeDefinition) {
      return &alarm;
    }
  }
  return nullptr;
}

/// Subjects the active alarm-scope condition names as refused; empty when it
/// is absent or has returned to normal.
std::set<std::string> RefusedSubjects(const CanonicalValue &snapshot) {
  std::set<std::string> subjects;
  const auto *alarm = ScopeAlarm(snapshot);
  if (alarm == nullptr ||
      !std::get<bool>(Member(*alarm, "active").storage())) {
    return subjects;
  }
  for (const auto &refused :
       Array(Member(*alarm, "details"), "refused_conditions")) {
    subjects.insert(Text(Member(refused, "subject"), "subject_id"));
  }
  return subjects;
}

std::size_t HistoryCount(HeadlessApiCore &core) {
  return Array(core.HistoryPage(HistorianQuery{}), "events").size();
}

TEST(HeadlessApiCore,
     MissingOwnersRemainUnknownWithoutFabricatingAProviderCondition) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto runtime = core.RuntimeSnapshot();
  EXPECT_FALSE(std::get<bool>(Member(runtime, "complete").storage()));
  try {
    static_cast<void>(core.NodeEnvironmentProjection());
    FAIL() << "missing Manager selection produced a node environment";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "node_environment_projection_unavailable");
  }
  try {
    static_cast<void>(core.SourceModelProjection());
    FAIL() << "missing Manager selection produced a source model";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "source_model_projection_unavailable");
  }
  try {
    static_cast<void>(core.ViewerModelUrdf());
    FAIL() << "missing Manager selection produced a viewer model";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "viewer_model_asset_unavailable");
  }
  try {
    static_cast<void>(core.AssistantModeProjection("composer-assistant"));
    FAIL() << "missing Manager selection produced an assistant mode";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "assistant_mode_projection_unavailable");
  }

  const auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));
}

TEST(HeadlessApiCore, AppliedSelectionAiModelsListEveryComponentInIdOrder) {
  using Json = core::JsonValueDTO;
  const auto configurations = LoadRegistryConfigurations();
  // A bundle without AI model components projects an empty array.
  const auto none =
      AssistantAiModels(configurations.single->value().cell_graph);
  EXPECT_TRUE(std::get<CanonicalValue::Array>(none.storage()).empty());

  // Two model-free components hosted by the first runtime unit, retained
  // sorted by id.
  auto graph = configurations.single->value().cell_graph;
  ASSERT_FALSE(graph.runtime_units.empty());
  ASSERT_FALSE(graph.controllable_components.empty());
  auto &unit = graph.runtime_units.front();
  for (const auto *id : {"chat_model", "selection_model"}) {
    auto component = graph.controllable_components.front();
    component.controllable_component_id = core::ControllableComponentId(id);
    component.runtime_unit_ids = {unit.runtime_unit_id};
    graph.controllable_components.push_back(std::move(component));
  }
  // Activation omits a null api_key_env from the retained row.
  const auto row = [](std::string id, std::string base_url, Json api_key_env) {
    Json::Object value{
        {"base_url", Json{std::move(base_url)}},
        {"controllable_component_id", Json{std::move(id)}},
        {"model", Json{std::string("served-model")}},
    };
    if (!std::holds_alternative<std::nullptr_t>(api_key_env.value))
      value.emplace("api_key_env", std::move(api_key_env));
    return Json{std::move(value)};
  };
  unit.configuration["ai_models"] = Json{Json::Array{
      row("chat_model", "http://127.0.0.1:8200/v1", Json{nullptr}),
      row("selection_model", "http://127.0.0.1:8000/v1",
          Json{std::string("MANYFORGE_AI_SELECTION_TOKEN")}),
  }};

  const auto projected = AssistantAiModels(graph);
  const auto &rows = std::get<CanonicalValue::Array>(projected.storage());
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(Text(rows[0], "controllable_component_id"), "chat_model");
  EXPECT_EQ(Text(rows[0], "base_url"), "http://127.0.0.1:8200/v1");
  EXPECT_EQ(Text(rows[0], "model"), "served-model");
  EXPECT_TRUE(Member(rows[0], "api_key_env").is_null());
  EXPECT_EQ(Text(rows[1], "controllable_component_id"), "selection_model");
  EXPECT_EQ(Text(rows[1], "base_url"), "http://127.0.0.1:8000/v1");
  EXPECT_EQ(Text(rows[1], "api_key_env"), "MANYFORGE_AI_SELECTION_TOKEN");
}

TEST(HeadlessApiCore,
     SharedPreparedProgramOutcomePreservesSelectionAndIngressErrors) {
  const auto no_program = CanonicalValue(CanonicalValue::Object{
      {"program_outcome", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("no_program")}})},
      {"program_id", CanonicalValue(nullptr)},
      {"program_revision", CanonicalValue(nullptr)}});
  const auto absent =
      deployment::parse_prepared_activation_program_outcome(no_program);
  ASSERT_TRUE(absent.ok());
  EXPECT_FALSE(absent.value().program_binding_id);
  EXPECT_FALSE(absent.value().non_executable);
  const auto bound = deployment::parse_prepared_activation_program_outcome(
      CanonicalValue(CanonicalValue::Object{
          {"program_outcome",
           CanonicalValue(CanonicalValue::Object{
               {"kind", CanonicalValue("bound")},
               {"program_binding_id", CanonicalValue("binding.exact")}})}}));
  ASSERT_TRUE(bound.ok());
  ASSERT_TRUE(bound.value().program_binding_id);
  EXPECT_EQ(bound.value().program_binding_id->value(), "binding.exact");
  const auto missing = deployment::parse_prepared_activation_program_outcome(
      CanonicalValue(CanonicalValue::Object{}));
  ASSERT_FALSE(missing.ok());
  EXPECT_EQ(missing.error().code, "headless_source_invalid");
  EXPECT_EQ(missing.error().message,
            "headless source is missing field program_outcome");
  const auto inconsistent =
      deployment::parse_prepared_activation_program_outcome(
          JournalField(no_program, "program_id", CanonicalValue("unexpected")));
  ASSERT_FALSE(inconsistent.ok());
  EXPECT_EQ(inconsistent.error().code, "program_applied_selection_invalid");
  EXPECT_EQ(inconsistent.error().message, "inconsistent no_program outcome");
}

TEST(HeadlessApiCore,
     AppliedStatusAndSelectionRejectDivergentPublicationBeforeEitherOutcome) {
  for (const auto kind : {"bound", "non_executable"}) {
    SCOPED_TRACE(kind);
    TemporaryDirectory directory;
    PublishHealthySources(directory, "open");
    auto payload = ManagerPayload();
    auto active = Member(payload, "active");
    active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
        {"transaction", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("named_apply")},
            {"apply_id", CanonicalValue("apply.divergent")},
            {"apply_generation", CanonicalValue(1.0)},
        })},
    }));
    payload = JournalField(payload, "active", active);
    payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
        {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"apply_id", CanonicalValue("apply.divergent")},
            {"apply_generation", CanonicalValue(1.0)},
            {"terminal_state", CanonicalValue("succeeded")},
            {"incomplete", CanonicalValue(false)},
            {"prepared_activation", CanonicalValue(CanonicalValue::Object{
                {"schema_version",
                 CanonicalValue("manyforge.prepared_activation_input.v6")},
                {"retained_bundle_id", CanonicalValue(Digest('f'))},
                {"graph_revision", Member(active, "graph_revision")},
                {"program_outcome", CanonicalValue(CanonicalValue::Object{
                    {"kind", CanonicalValue(kind)},
                })},
            })},
        })})},
    }));
    AtomicSourceSnapshotChannel manager(directory.publications(),
                                        "manager-source.json");
    static_cast<void>(manager.Publish(
        "component_manager", "component-manager." + std::string(kCellId),
        "manager.epoch.1", 2, payload));
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    for (const bool status : {false, true}) {
      try {
        static_cast<void>(status ? core.AppliedProgramStatus()
                                 : core.AppliedProgramSelection());
        FAIL() << "divergent publication was reported as applied";
      } catch (const OperatorAlarmError &error) {
        EXPECT_EQ(error.code(), "program_applied_selection_invalid") << error.what();
        EXPECT_NE(std::string(error.what()).find(
                      "active Manager publication and prepared activation disagree"),
                  std::string::npos);
      }
    }
  }
}

TEST(HeadlessApiCore, NoProgramIsPublishedNormallyButHasNoExecutableSelection) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue("apply.new")},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  const CanonicalValue prepared(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.prepared_activation_input.v6")},
      {"prepared_activation_id", CanonicalValue(Digest('a'))},
      {"retained_bundle_id", Member(active, "retained_bundle_id")},
      {"graph_revision", Member(active, "graph_revision")},
      {"origin",
       CanonicalValue(CanonicalValue::Object{
           {"kind", CanonicalValue("scenario")},
           {"scenario_id", CanonicalValue("new_scenario")},
           {"project_path", CanonicalValue("/fixtures/new_scenario.scenario")},
           {"source_document_digest", CanonicalValue(Digest('b'))},
       })},
      {"deployment_artifact_id", CanonicalValue("new_deployment")},
      {"deployment_revision", CanonicalValue(Digest('c'))},
      {"program_id", CanonicalValue(nullptr)},
      {"program_revision", CanonicalValue(nullptr)},
      {"program_outcome", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("no_program")},
                          })},
  });
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"apply_id", CanonicalValue("apply.new")},
          {"apply_generation", CanonicalValue(1.0)},
          {"terminal_state", CanonicalValue("succeeded")},
          {"incomplete", CanonicalValue(false)},
          {"intent_digest", CanonicalValue(Digest('d'))},
          {"prepared_activation", prepared},
      })})},
  }));
  AtomicSourceSnapshotChannel manager(directory.publications(), "manager-source.json");
  static_cast<void>(manager.Publish("component_manager", "component-manager." + std::string(kCellId),
                                    "manager.epoch.1", 2, payload));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto status = core.AppliedProgramStatus();
  EXPECT_TRUE(Member(status, "program_id").is_null());
  EXPECT_TRUE(Member(status, "program_binding_id").is_null());
  EXPECT_TRUE(Member(status, "non_executable").is_null());
  EXPECT_FALSE(std::get<bool>(Member(status, "executable").storage()));
  try {
    static_cast<void>(core.AppliedProgramSelection());
    FAIL() << "no_program returned an executable selection";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "program_not_selected");
  }
}

TEST(HeadlessApiCore, ActiveApplyFromTheRemovedOverrideVersionIsRefused) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue("apply.old")},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  // v4 carried launch-time typed_parameter_overrides (removed, D9 cutover).
  const CanonicalValue prepared(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.prepared_activation_input.v4")},
      {"retained_bundle_id", Member(active, "retained_bundle_id")},
      {"graph_revision", Member(active, "graph_revision")},
      {"program_id", CanonicalValue(nullptr)},
      {"program_revision", CanonicalValue(nullptr)},
      {"program_outcome", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("no_program")},
      })},
      {"typed_parameter_overrides", CanonicalValue(CanonicalValue::Object{})},
  });
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"apply_id", CanonicalValue("apply.old")},
          {"apply_generation", CanonicalValue(1.0)},
          {"terminal_state", CanonicalValue("succeeded")},
          {"incomplete", CanonicalValue(false)},
          {"intent_digest", CanonicalValue(Digest('d'))},
          {"prepared_activation", prepared},
      })})},
  }));
  AtomicSourceSnapshotChannel manager(directory.publications(), "manager-source.json");
  static_cast<void>(manager.Publish("component_manager", "component-manager." + std::string(kCellId),
                                    "manager.epoch.1", 2, payload));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  try {
    static_cast<void>(core.AppliedProgramStatus());
    FAIL() << "an Apply prepared before the override removal was read";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "program_applied_selection_invalid");
  }
}

namespace {

CanonicalValue CommittedApplyRecord(const std::string &apply_id,
                                    const char digest,
                                    const std::string &scenario_id,
                                    const bool committed,
                                    const CanonicalValue &active) {
  return CanonicalValue(CanonicalValue::Object{
      {"apply_id", CanonicalValue(apply_id)},
      {"apply_generation", CanonicalValue(1.0)},
      {"committed", CanonicalValue(committed)},
      {"terminal_state", CanonicalValue(committed ? "succeeded" : "failed")},
      {"incomplete", CanonicalValue(false)},
      {"intent_digest", CanonicalValue(Digest(digest))},
      {"scene_initialization", CanonicalValue("initialize")},
      {"prepared_activation",
       CanonicalValue(CanonicalValue::Object{
           {"schema_version",
            CanonicalValue("manyforge.prepared_activation_input.v6")},
           {"retained_bundle_id", Member(active, "retained_bundle_id")},
           {"graph_revision", Member(active, "graph_revision")},
           {"origin",
            CanonicalValue(CanonicalValue::Object{
                {"kind", CanonicalValue("scenario")},
                {"scenario_id", CanonicalValue(scenario_id)},
                {"project_path",
                 CanonicalValue("/fixtures/" + scenario_id + ".scenario")},
                {"source_document_digest", CanonicalValue(Digest('b'))},
            })},
           {"program_id", CanonicalValue(nullptr)},
           {"program_revision", CanonicalValue(nullptr)},
           {"program_outcome", CanonicalValue(CanonicalValue::Object{
                                   {"kind", CanonicalValue("no_program")},
                               })},
       })},
  });
}

CanonicalValue CommittedActivationCore(TemporaryDirectory &directory,
                                       const double publication_generation,
                                       const bool include_older_apply,
                                       const bool sealed_current = false) {
  PublishHealthySources(directory, "open");
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"publication_generation", CanonicalValue(publication_generation)},
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue("apply.new")},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  CanonicalValue::Array records{
      CommittedApplyRecord("apply.new", 'd', "cell_pick", true, active),
      CommittedApplyRecord("apply.refused", 'e', "cell_pick", false, active),
  };
  if (include_older_apply) {
    records.push_back(
        CommittedApplyRecord("apply.old", 'c', "other_cell", true, active));
  }
  if (sealed_current) {
    auto prepared = Member(records.front(), "prepared_activation");
    auto sealed_prepared_fields = prepared.as_object();
    sealed_prepared_fields.emplace_back("deployment_artifact_id",
                                       CanonicalValue("deployment.empty"));
    sealed_prepared_fields.emplace_back("deployment_revision",
                                       CanonicalValue(Digest('a')));
    prepared = CanonicalValue(std::move(sealed_prepared_fields));
    prepared = JournalField(prepared, "origin", CanonicalValue(CanonicalValue::Object{
        {"kind", CanonicalValue("sealed_canonical_empty")},
        {"cell_id", CanonicalValue(std::string(kCellId))},
        {"deployment_artifact_id", CanonicalValue("deployment.empty")},
        {"deployment_revision", CanonicalValue(Digest('a'))},
        {"retained_bundle_id", Member(active, "retained_bundle_id")},
        {"graph_revision", Member(active, "graph_revision")},
        {"admission_digest", CanonicalValue(Digest('f'))},
    }));
    records.front() = JournalField(records.front(), "prepared_activation", prepared);
    records.front() = JournalField(records.front(), "scene_initialization",
                                CanonicalValue("preserve"));
  }
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(std::move(records))},
  }));
  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + std::string(kCellId),
      "manager.epoch.1", 2, payload));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  return core.CommittedScenarioActivation();
}

} // namespace

TEST(HeadlessApiCore, CommittedActivationNamesEveryCommittedApplyOfItsHistory) {
  TemporaryDirectory directory;
  const auto activation = CommittedActivationCore(directory, 3.0, true);
  EXPECT_EQ(Text(activation, "schema_version"),
            "manyforge.committed_scenario_activation.v3");
  EXPECT_EQ(Text(activation, "scene_initialization"), "initialize");
  EXPECT_EQ(Text(activation, "record_digest"), Digest('d'));
  EXPECT_EQ(Text(activation, "apply_id"), "apply.new");
  EXPECT_EQ(std::get<double>(Member(activation, "apply_generation").storage()),
            1.0);
  EXPECT_EQ(Text(Member(activation, "origin"), "scenario_id"), "cell_pick");
  EXPECT_EQ(std::get<double>(Member(activation, "publication_generation").storage()),
            3.0);
  // A Scenario without a Program declares nothing; it is still an activation.
  EXPECT_TRUE(Member(activation, "program").is_null());
  // Only Applies that published a configuration, in digest order, each
  // named by its Apply identity; a refused Apply never published one and is
  // not part of the history.
  const auto &committed = Array(activation, "committed_applies");
  ASSERT_EQ(committed.size(), 2U);
  EXPECT_EQ(Text(committed[0], "record_digest"), Digest('c'));
  EXPECT_EQ(Text(Member(committed[0], "origin"), "scenario_id"), "other_cell");
  EXPECT_EQ(Text(committed[0], "apply_id"), "apply.old");
  EXPECT_EQ(std::get<double>(Member(committed[0], "apply_generation").storage()),
            1.0);
  EXPECT_EQ(Text(committed[1], "record_digest"), Digest('d'));
  EXPECT_EQ(Text(Member(committed[1], "origin"), "scenario_id"), "cell_pick");
  EXPECT_EQ(Text(committed[1], "apply_id"), "apply.new");
  EXPECT_EQ(std::get<double>(Member(committed[1], "apply_generation").storage()),
            1.0);
  EXPECT_EQ(committed[0].as_object().size(), 5U);
  EXPECT_EQ(committed[1].as_object().size(), 5U);
}

TEST(HeadlessApiCore, SealedEmptyApplyRemainsInTheCompleteCommittedHistory) {
  TemporaryDirectory directory;
  const auto activation = CommittedActivationCore(directory, 3.0, true, true);
  EXPECT_EQ(Text(Member(activation, "origin"), "kind"), "sealed_canonical_empty");
  EXPECT_EQ(activation.find_object_member("scenario_id"), nullptr);
  EXPECT_TRUE(Member(activation, "program").is_null());
  const auto &history = Array(activation, "committed_applies");
  ASSERT_EQ(history.size(), 2U);
  EXPECT_EQ(Text(Member(history[0], "origin"), "kind"), "scenario");
  EXPECT_EQ(Text(Member(history[1], "origin"), "kind"), "sealed_canonical_empty");
  EXPECT_EQ(history[1].find_object_member("scenario_id"), nullptr);
  HeadlessApiCore core(Config(directory, "api.epoch.status"), Clocks());
  const auto status = core.AppliedProgramStatus();
  EXPECT_EQ(Text(Member(status, "origin"), "kind"), "sealed_canonical_empty");
  EXPECT_EQ(status.find_object_member("scenario_id"), nullptr);
  EXPECT_FALSE(std::get<bool>(Member(status, "executable").storage()));
  EXPECT_TRUE(Member(status, "program_id").is_null());
  try {
    static_cast<void>(core.AppliedProgramSelection());
    FAIL() << "sealed empty Apply returned an executable Program";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "program_not_selected");
  }
}

TEST(HeadlessApiCore, CommittedActivationRefusesAHistoryThatMissesAPublication) {
  TemporaryDirectory directory;
  try {
    // Three publications (bootstrap and two Applies) but one committed Apply.
    static_cast<void>(CommittedActivationCore(directory, 3.0, false));
    FAIL() << "an incomplete publication history was reported";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "program_activation_history_incomplete");
  }
}

/// A monotonic clock that parks one chosen thread inside its read. A parked
/// `RefreshLocked` holds the core coordination lock for as long as wanted.
struct ParkingMonotonicClock final {
  std::mutex mutex;
  std::condition_variable changed;
  std::optional<std::thread::id> parked_thread;
  bool parked{};
  std::atomic<std::int64_t> now_ns{1000};

  std::int64_t Read() {
    std::unique_lock lock(mutex);
    if (parked_thread == std::this_thread::get_id()) {
      parked = true;
      changed.notify_all();
      changed.wait(lock, [this]() { return !parked_thread.has_value(); });
      parked = false;
    }
    return now_ns.load();
  }
};

TEST(HeadlessApiCore,
     MotionAttentionIsReadWithoutTheCoreLockAndReportsOverdueSources) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto clock = std::make_shared<ParkingMonotonicClock>();
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          []() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
          },
          [clock]() { return clock->Read(); },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  const auto attention = [&core]() {
    const auto value = core.OperatorAlarmMotionAttention();
    return std::pair{std::get<bool>(Member(value, "reconciled").storage()),
                     Unsigned(value, "generation")};
  };

  // Before any completed refresh the sources' currency is unknown.
  EXPECT_FALSE(attention().first);
  const auto first = core.RefreshAlarmSources();
  EXPECT_TRUE(std::get<bool>(Member(first, "refreshed").storage()));
  static_cast<void>(
      core.CompleteOperationEventSnapshot("execution-host.epoch.idle", 0U));
  const auto [current, current_generation] = attention();
  EXPECT_TRUE(current);
  // Within the period R a refresh is not repeated and takes no core lock.
  EXPECT_FALSE(std::get<bool>(
      Member(core.RefreshAlarmSources(), "refreshed").storage()));

  // A reader's refresh holds the core coordination lock, parked inside it.
  std::thread holder([&core, clock]() {
    {
      std::lock_guard lock(clock->mutex);
      clock->parked_thread = std::this_thread::get_id();
    }
    static_cast<void>(core.RuntimeSnapshot());
  });
  {
    std::unique_lock lock(clock->mutex);
    ASSERT_TRUE(clock->changed.wait_for(lock, std::chrono::seconds(10),
                                        [&clock]() { return clock->parked; }));
  }
  // The attention read neither waits for that lock nor refreshes.
  EXPECT_EQ(attention(), std::pair(true, current_generation));
  // Sources older than the currency bound C are reported, never served as
  // current: a new generation that is not reconciled.
  clock->now_ns += kAlarmSourceCurrencyBoundNs + 1;
  const auto [overdue, overdue_generation] = attention();
  EXPECT_FALSE(overdue);
  EXPECT_GT(overdue_generation, current_generation);
  // Repeated overdue reads name the same health fact: no generation churn.
  EXPECT_EQ(attention(), std::pair(false, overdue_generation));
  {
    std::lock_guard lock(clock->mutex);
    clock->parked_thread.reset();
  }
  clock->changed.notify_all();
  holder.join();

  // The completed refresh observed the sources now: currency is restored.
  const auto [restored, restored_generation] = attention();
  EXPECT_TRUE(restored);
  EXPECT_GT(restored_generation, overdue_generation);
  clock->now_ns += kAlarmSourceRefreshPeriodNs;
  EXPECT_TRUE(std::get<bool>(
      Member(core.RefreshAlarmSources(), "refreshed").storage()));
}

TEST(HeadlessApiCore,
     OverdueMarkIsRedecidedAgainstARefreshThatCompletedMeanwhile) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  // A monotonic clock that runs one injected action inside a chosen read,
  // interleaving a completed refresh between the attention read's overdue
  // observation and its health mark, deterministically on one thread.
  struct InterleavingClock final {
    std::int64_t now_ns{1000};
    std::function<void()> on_next_read;
  };
  auto clock = std::make_shared<InterleavingClock>();
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          []() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
          },
          [clock]() {
            if (auto action = std::exchange(clock->on_next_read, nullptr)) {
              action();
            }
            return clock->now_ns;
          },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  static_cast<void>(core.RefreshAlarmSources());
  static_cast<void>(
      core.CompleteOperationEventSnapshot("execution-host.epoch.idle", 0U));
  const auto attention = [&core]() {
    const auto value = core.OperatorAlarmMotionAttention();
    return std::pair{std::get<bool>(Member(value, "reconciled").storage()),
                     Unsigned(value, "generation")};
  };
  ASSERT_TRUE(attention().first);

  // The attention read loads the old refresh time, then reads the clock; a
  // refresh completes inside that clock read, so the stale observation says
  // overdue although the sources are current when the mark is decided.
  clock->now_ns += kAlarmSourceCurrencyBoundNs + 1;
  std::optional<std::uint64_t> refreshed_generation;
  clock->on_next_read = [&core, &refreshed_generation, &attention]() {
    ASSERT_TRUE(std::get<bool>(
        Member(core.RefreshAlarmSources(), "refreshed").storage()));
    refreshed_generation = attention().second;
  };
  const auto [reconciled, generation] = attention();
  ASSERT_TRUE(refreshed_generation.has_value());
  EXPECT_TRUE(reconciled);
  EXPECT_EQ(generation, *refreshed_generation);
  EXPECT_EQ(attention(), std::pair(true, *refreshed_generation));
}

TEST(HeadlessApiCore,
     IdleOperationSnapshotReconcilesOnlyImplementedAlarmDomains) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  static_cast<void>(core.RuntimeSnapshot());
  static_cast<void>(
      core.CompleteOperationEventSnapshot("execution-host.epoch.idle", 0U));

  const auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_TRUE(std::get<bool>(Member(alarms, "reconciled").storage()));
  const auto &domains = Array(alarms, "domains");
  ASSERT_EQ(domains.size(), 3U);
  const auto scene =
      std::find_if(domains.begin(), domains.end(), [](const auto &domain) {
        return Text(domain, "domain") == "scene";
      });
  ASSERT_NE(scene, domains.end());
  EXPECT_TRUE(std::get<bool>(Member(*scene, "reconciled").storage()));
  const auto motion =
      std::find_if(domains.begin(), domains.end(), [](const auto &domain) {
        return Text(domain, "domain") == "motion";
      });
  ASSERT_NE(motion, domains.end());
  EXPECT_TRUE(std::get<bool>(Member(*motion, "reconciled").storage()));
  const auto runtime_health =
      std::find_if(domains.begin(), domains.end(), [](const auto &domain) {
        return Text(domain, "domain") == "runtime_health";
      });
  ASSERT_NE(runtime_health, domains.end());
  EXPECT_TRUE(std::get<bool>(Member(*runtime_health, "reconciled").storage()));
}

TEST(HeadlessApiCore,
     RejectsAnOverlappingAlarmOwnerAndAllowsOrderlyReplacement) {
  TemporaryDirectory directory;
  HeadlessApiCore owner(Config(directory, "api.epoch.1"), Clocks());
  try {
    HeadlessApiCore duplicate(Config(directory, "api.epoch.2"), Clocks());
    static_cast<void>(duplicate);
    FAIL() << "overlapping Alarm Lifecycle Owner was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_owner_conflict");
  }

  owner.Shutdown();
  HeadlessApiCore replacement(Config(directory, "api.epoch.3"), Clocks());
  EXPECT_EQ(Text(replacement.AlarmSnapshot(), "owner_epoch"), "api.epoch.3");
}

TEST(HeadlessApiCore,
     PreviouslyCompleteSourcesGoingStaleProduceOneProviderCondition) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCoreClocks clocks{
      [wall_time]() { return *wall_time; },
      [monotonic]() { return ++*monotonic; },
      []() { return "2026-08-05T00:00:00Z"; },
  };
  HeadlessApiCore core(Config(directory, "api.epoch.1"), std::move(clocks));
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  *wall_time += 20'000'000'000LL;
  const auto runtime = core.RuntimeSnapshot();
  EXPECT_FALSE(std::get<bool>(Member(runtime, "complete").storage()));
  const auto alarms = core.AlarmSnapshot();
  const auto &provider = OnlyAlarm(alarms);
  EXPECT_EQ(Text(provider, "definition_id"),
            "manyforge.runtime.provider_unavailable");
  EXPECT_EQ(Text(provider, "lifecycle_state"), "active_unacknowledged");
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

  try {
    static_cast<void>(core.AcknowledgeAlarm(
        Text(provider, "condition_id"), Text(provider, "activation_id"),
        Unsigned(provider, "record_revision"),
        Caller("stale.request", "api.epoch.stale"), std::nullopt, true));
    FAIL() << "stale headless owner epoch was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "stale_request_owner_epoch");
  }

  const auto response = core.AcknowledgeAlarm(
      Text(provider, "condition_id"), Text(provider, "activation_id"),
      Unsigned(provider, "record_revision"), Caller(), "operator aware", true);
  EXPECT_TRUE(std::get<bool>(Member(response, "changed").storage()));
  EXPECT_EQ(Array(response, "audit_event_ids").size(), 1U);
  const auto duplicate = core.AcknowledgeAlarm(
      Text(provider, "condition_id"), Text(provider, "activation_id"),
      Unsigned(provider, "record_revision"), Caller(), "operator aware", true);
  EXPECT_EQ(Text(duplicate, "status"), "already_acknowledged");
  EXPECT_FALSE(std::get<bool>(Member(duplicate, "changed").storage()));
  EXPECT_TRUE(Array(duplicate, "audit_event_ids").empty());
  // Attribution is not an owner-lifetime receipt key. The exact activation
  // remains acknowledged, even if another caller supplies a different reason.
  const auto repeated = core.AcknowledgeAlarm(
      Text(provider, "condition_id"), Text(provider, "activation_id"),
      Unsigned(provider, "record_revision"), Caller(), "changed reason", true);
  EXPECT_FALSE(std::get<bool>(Member(repeated, "changed").storage()));
  EXPECT_TRUE(Array(repeated, "audit_event_ids").empty());

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (HistoryCount(core) < 2U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_GE(HistoryCount(core), 2U);
}

TEST(HeadlessApiCore, BoundedManagerApplyTransitionDoesNotCreateProviderAlarm) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCoreClocks clocks{
      [wall_time]() { return *wall_time; },
      [monotonic]() { return *monotonic; },
      []() { return "2026-08-05T00:00:00Z"; },
  };
  auto config = Config(directory, "api.epoch.1");
  HeadlessApiCore core(std::move(config), std::move(clocks));
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(directory);
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  *wall_time += 20'000'000'000LL;
  *monotonic += 5'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

  PublishHealthySources(directory, "open", 2U);
  *wall_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                   std::chrono::system_clock::now().time_since_epoch())
                   .count();
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  const auto &domains = Array(alarms, "domains");
  const auto runtime_health =
      std::find_if(domains.begin(), domains.end(), [](const auto &domain) {
        return Text(domain, "domain") == "runtime_health";
      });
  ASSERT_NE(runtime_health, domains.end());
  EXPECT_TRUE(std::get<bool>(Member(*runtime_health, "reconciled").storage()));
}

TEST(HeadlessApiCore,
     LongManagerApplyTransitionRemainsOperationStatusNotProviderAlarm) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCoreClocks clocks{
      [wall_time]() { return *wall_time; },
      [monotonic]() { return *monotonic; },
      []() { return "2026-08-05T00:00:00Z"; },
  };
  auto config = Config(directory, "api.epoch.1");
  HeadlessApiCore core(std::move(config), std::move(clocks));
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(directory);
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  *wall_time += 20'000'000'000LL;
  *monotonic += 5'000'000'001LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  const auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));
}

TEST(HeadlessApiCore,
     EmptyCellApplyIsExpectedAtStartupAndAfterReturningToEmpty) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          [wall_time]() { return *wall_time; },
          []() { return 1000; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  for (int activation = 0; activation < 2; ++activation) {
    SCOPED_TRACE(activation);
    PublishManagerApplyTransition(directory, "runtime_convergence_pending",
                                  "robot-a", true);
    *wall_time += 20'000'000'000LL;
    EXPECT_FALSE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    const auto alarms = core.AlarmSnapshot();
    EXPECT_TRUE(Array(alarms, "alarms").empty());
    EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

    // A later empty Scenario is ready in the same Headless session. The next
    // Apply must receive the same treatment as the initial empty-cell Apply.
    PublishHealthySources(directory, "open", 2U + activation);
    *wall_time = std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::system_clock::now().time_since_epoch())
                     .count();
    EXPECT_TRUE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  // Once the operation is gone, an unexplained loss is still an alarm.
  *wall_time += 20'000'000'000LL;
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            "manyforge.runtime.provider_unavailable");
}

TEST(HeadlessApiCore,
     EmptyCellDoesNotHideUnfencedTerminalOrUnrelatedProviderLoss) {
  for (const std::string_view condition :
       {"before_admission", "terminal", "released_fence", "empty_scope",
        "unexpected_desired_units", "existing_runtime"}) {
    SCOPED_TRACE(condition);
    TemporaryDirectory directory;
    PublishHealthySources(directory, "open");
    auto wall_time = std::make_shared<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    HeadlessApiCore core(
        Config(directory, "api.epoch.1"),
        HeadlessApiCoreClocks{
            [wall_time]() { return *wall_time; },
            []() { return 1000; },
            []() { return "2026-08-05T00:00:00Z"; },
        });
    EXPECT_TRUE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

    auto payload = ManagerPayloadWithIncompleteApply(
        "runtime_convergence_pending", "robot-a", true);
    auto apply = Member(payload, "apply");
    auto row = Array(apply, "records").front();
    if (condition == "before_admission") {
      row = JournalField(row, "phase", CanonicalValue("fence_acquire_pending"));
    } else if (condition == "terminal") {
      row = JournalField(row, "incomplete", CanonicalValue(false));
    } else if (condition == "released_fence") {
      row = JournalField(row, "fence",
                      JournalField(Member(row, "fence"), "state",
                                CanonicalValue("released")));
    } else if (condition == "empty_scope") {
      const auto &fence = Member(row, "fence");
      row = JournalField(
          row, "fence",
          JournalField(fence, "scope",
                    JournalField(Member(fence, "scope"),
                              "controllable_component_ids",
                              CanonicalValue(CanonicalValue::Array{}))));
    } else if (condition == "unexpected_desired_units") {
      payload = JournalField(
          payload, "runtime",
          JournalField(Member(payload, "runtime"), "desired_units",
                    CanonicalValue(CanonicalValue::Array{CanonicalValue(
                        CanonicalValue::Object{
                            {"runtime_unit_id",
                             CanonicalValue("unrelated-runtime")},
                        })})));
    } else if (condition == "existing_runtime") {
      const auto &active = Member(payload, "active");
      payload = JournalField(
          payload, "active",
          JournalField(active, "cell_graph",
                    JournalField(Member(active, "cell_graph"), "runtime_units",
                              CanonicalValue(CanonicalValue::Array{
                                  CanonicalValue(CanonicalValue::Object{
                                      {"runtime_unit_id",
                                       CanonicalValue("unrelated-runtime")},
                                  }),
                              }))));
    }
    payload = JournalField(
        payload, "apply",
        JournalField(apply, "records",
                  CanonicalValue(CanonicalValue::Array{std::move(row)})));
    AtomicSourceSnapshotChannel manager(directory.publications(),
                                        "manager-source.json");
    static_cast<void>(manager.Publish(
        "component_manager", "component-manager." + std::string(kCellId),
        "manager.epoch.1", 2, payload));
    *wall_time += 20'000'000'000LL;
    EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
              "manyforge.runtime.provider_unavailable");
  }
}

TEST(HeadlessApiCore,
     ApplyBeforeAdmissionDoesNotSuppressProviderLossAlarm) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          [wall_time]() { return *wall_time; },
          [monotonic]() { return *monotonic; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(directory, "fence_acquire_pending");
  *wall_time += 20'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);
}

TEST(HeadlessApiCore,
     ApplyForUnrelatedRuntimeDoesNotSuppressProviderLossAlarm) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          [wall_time]() { return *wall_time; },
          [monotonic]() { return *monotonic; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(directory, "runtime_convergence_pending",
                                "another-robot");
  *wall_time += 20'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);
}

TEST(HeadlessApiCore,
     RestartWithOnlyStaleRetainedSourcesDoesNotFabricateProviderCondition) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count() +
      1'000'000'000LL);
  auto monotonic = std::make_shared<std::int64_t>(1000);
  const auto clocks = [wall_time, monotonic]() {
    return HeadlessApiCoreClocks{
        [wall_time]() { return *wall_time; },
        [monotonic]() { return ++*monotonic; },
        []() { return "2026-08-05T00:00:00Z"; },
    };
  };
  {
    HeadlessApiCore core(Config(directory, "api.epoch.1"), clocks());
    EXPECT_TRUE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  *wall_time += 20'000'000'000LL;
  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), clocks());
  EXPECT_FALSE(std::get<bool>(
      Member(replacement.RuntimeSnapshot(), "complete").storage()));
  const auto alarms = replacement.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));
}

TEST(HeadlessApiCore,
     IndependentlyProjectsTheManagerSelectedNodeEnvironmentWithStableIdentity) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto first = core.NodeEnvironmentProjection();
  const auto second = core.NodeEnvironmentProjection();
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource bundle_source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = bundle_source.load_verified();
  EXPECT_EQ(Text(first, "schema_version"),
            kNodeEnvironmentProjectionApiVersion);
  EXPECT_EQ(Text(first, "retained_bundle_id"),
            admission.retained_bundle_id().value());
  const auto &availability = Member(first, "availability");
  EXPECT_TRUE(std::get<bool>(
      Member(availability, "manager_runtime_converged").storage()));
  EXPECT_FALSE(
      std::get<bool>(Member(availability, "execution_available").storage()));
  const auto &source = Member(first, "source");
  EXPECT_EQ(Text(source, "source_owner_id"),
            "headless-api.cell.i5.node-environment");
  EXPECT_EQ(Text(source, "selection_source_owner_id"),
            "component-manager.c1_build_authority_cell");
  EXPECT_EQ(Unsigned(source, "projection_generation"), 1U);
  EXPECT_EQ(Unsigned(Member(second, "source"), "projection_generation"), 1U);
  EXPECT_EQ(Text(Member(second, "source"), "source_content_digest"),
            Text(source, "source_content_digest"));
}

TEST(HeadlessApiCore,
     IndependentlyProjectsTheManagerSelectedSourceModelWithStableIdentity) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto first = core.SourceModelProjection();
  const auto second = core.SourceModelProjection();
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource bundle_source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = bundle_source.load_verified();
  EXPECT_EQ(Text(first, "schema_version"), kSourceModelProjectionApiVersion);
  EXPECT_EQ(Text(first, "retained_bundle_id"),
            admission.retained_bundle_id().value());
  EXPECT_TRUE(Array(first, "occurrences").empty());
  EXPECT_TRUE(Array(first, "targets").empty());
  const auto &source = Member(first, "source");
  EXPECT_EQ(Text(source, "source_owner_id"),
            "headless-api.cell.i5.source-model");
  EXPECT_EQ(Text(source, "selection_source_owner_id"),
            "component-manager.c1_build_authority_cell");
  EXPECT_EQ(Unsigned(source, "projection_generation"), 1U);
  EXPECT_EQ(Unsigned(Member(second, "source"), "projection_generation"), 1U);
  EXPECT_EQ(Text(Member(second, "source"), "source_content_digest"),
            Text(source, "source_content_digest"));
}

TEST(HeadlessApiCore,
     SourceModelContextCorrelatesCanonicalEmptyChildrenFromOneRefresh) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto context = core.SourceModelContext();
  EXPECT_EQ(context.as_object().size(), 3U);
  EXPECT_EQ(Text(context, "schema_version"), kSourceModelContextSchemaVersion);
  const auto &environment = Member(context, "node_environment");
  const auto &source_model = Member(context, "source_model");
  EXPECT_EQ(Text(environment, "schema_version"),
            kNodeEnvironmentProjectionApiVersion);
  EXPECT_EQ(Text(source_model, "schema_version"),
            kSourceModelProjectionApiVersion);
  EXPECT_EQ(Text(environment, "retained_bundle_id"),
            Text(source_model, "retained_bundle_id"));
  EXPECT_EQ(Text(environment, "graph_revision"),
            Text(source_model, "graph_revision"));

  constexpr std::array<std::string_view, 5> kSelectionFields{
      "selection_projection_generation", "selection_source_content_digest",
      "selection_source_generation",     "selection_source_owner_epoch",
      "selection_source_owner_id",
  };
  const auto &environment_source = Member(environment, "source");
  const auto &source_model_source = Member(source_model, "source");
  for (const auto field : kSelectionFields) {
    EXPECT_EQ(Member(environment_source, field),
              Member(source_model_source, field));
  }
  EXPECT_TRUE(Array(source_model, "occurrences").empty());
  EXPECT_TRUE(Array(source_model, "targets").empty());
}

TEST(HeadlessApiCore, ServesOnlyTheExactVerifiedActiveRetainedViewerProduct) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource bundle_source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = bundle_source.load_verified();

  const auto urdf = core.ViewerModelUrdf();
  EXPECT_EQ(urdf.asset_kind(), "urdf");
  EXPECT_EQ(urdf.cell_id(), kCellId);
  EXPECT_EQ(urdf.graph_revision(), admission.graph_revision().value());
  EXPECT_EQ(urdf.retained_bundle_id().value(),
            admission.retained_bundle_id().value());
  EXPECT_EQ(urdf.product_id().value(), "cell_viewer_model");
  EXPECT_EQ(urdf.product_contract_id().value(), "manyforge.viewer_model");
  EXPECT_EQ(urdf.media_type(), "application/xml");
  EXPECT_FALSE(urdf.logical_uri().has_value());
  EXPECT_FALSE(urdf.bytes().empty());
  EXPECT_EQ(urdf.byte_size(), urdf.bytes().size());

  try {
    static_cast<void>(
        core.ViewerModelResource(std::string("sha256:") + std::string(64U, 'f'),
                                 "package://fixture/unknown.stl"));
    FAIL() << "stale retained-bundle coordinates selected a viewer resource";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "viewer_model_selection_stale");
  }
  try {
    static_cast<void>(
        core.ViewerModelResource(admission.retained_bundle_id().value(),
                                 "package://fixture/unknown.stl"));
    FAIL() << "unknown logical URI selected a viewer resource";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "viewer_model_resource_not_found");
  }
}

TEST(HeadlessApiCore,
     CanonicalEmptySelectionDoesNotFabricateAssistantModeAvailability) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  try {
    static_cast<void>(core.AssistantModeProjection("composer-assistant"));
    FAIL() << "canonical-empty selection produced an assistant mode";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "assistant_mode_projection_unavailable");
  }
  try {
    static_cast<void>(core.AssistantModeContext("composer-assistant"));
    FAIL() << "canonical-empty selection produced an assistant context";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "assistant_mode_projection_unavailable");
  }
}

TEST(HeadlessApiCore,
     SceneFailureReturnsButKeepsAttentionAndRestartReannunciates) {
  TemporaryDirectory directory;
  PublishHealthySources(directory, "failed");
  std::string first_activation;
  {
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    auto alarms = core.AlarmSnapshot();
    const auto &failure = OnlyAlarm(alarms);
    EXPECT_EQ(Text(failure, "definition_id"),
              "manyforge.runtime.scene_initialization_fault");
    first_activation = Text(failure, "activation_id");

    PublishHealthySources(directory, "open", 2U);
    alarms = core.AlarmSnapshot();
    const auto &returned = OnlyAlarm(alarms);
    EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
    static_cast<void>(core.AcknowledgeAlarm(
        Text(returned, "condition_id"), Text(returned, "activation_id"),
        Unsigned(returned, "record_revision"), Caller(), std::nullopt, true));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  PublishHealthySources(directory, "failed", 3U);
  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), Clocks());
  const auto replacement_snapshot = replacement.AlarmSnapshot();
  const auto &rediscovered = OnlyAlarm(replacement_snapshot);
  EXPECT_NE(Text(rediscovered, "activation_id"), first_activation);
  EXPECT_EQ(Text(rediscovered, "lifecycle_state"), "active_unacknowledged");
  EXPECT_GE(HistoryCount(replacement), 3U);
}

TEST(HeadlessApiCore,
     HealthyReconciliationHasNoReceiptLifetimeOrRepeatedAuditEffect) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto first = core.ReconcileHistorianHealth(0U, Caller("request.1"));
  EXPECT_EQ(Text(first, "schema_version"),
            "manyforge.historian_reconciliation.v1");
  EXPECT_FALSE(std::get<bool>(Member(first, "reconciled").storage()));
  EXPECT_TRUE(Array(first, "audit_event_ids").empty());
  EXPECT_EQ(core.ReconcileHistorianHealth(0U, Caller("request.1")), first);

  try {
    static_cast<void>(core.ReconcileHistorianHealth(1U, Caller("request.1")));
    FAIL() << "a non-current generation reconciled historian health";
  } catch (const EventHistorianError &error) {
    EXPECT_EQ(error.code(), "historian_health_generation_stale");
  }
  try {
    static_cast<void>(core.ReconcileHistorianHealth(
        0U, Caller("stale.request", "api.epoch.stale")));
    FAIL() << "stale owner epoch reconciled historian health";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "stale_request_owner_epoch");
  }

  // More than twice the removed default receipt capacity, without retained
  // per-request state, historian work, or eventual permanent refusal.
  for (std::size_t index = 0U; index < 10000U; ++index) {
    EXPECT_EQ(core.ReconcileHistorianHealth(
                  0U, Caller("request." + std::to_string(index))),
              first);
  }
  EXPECT_EQ(core.ReconcileHistorianHealth(0U, Caller("request.1")), first);
  EXPECT_EQ(HistoryCount(core), 0U);
}

TEST(
    HeadlessApiCore,
    DistinctAlarmMutationsOutliveSmallHistoryWithoutAcknowledgingNewerActivations) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  config.historian_record_retention = 4U;
  HeadlessApiCore core(config, Clocks());
  std::string old_condition;
  std::string old_activation;
  std::uint64_t old_revision = 0;
  for (std::uint64_t index = 1; index <= 64; ++index) {
    PublishHealthySources(directory, "failed", index * 2);
    const auto snapshot = core.AlarmSnapshot();
    const auto &active = OnlyAlarm(snapshot);
    const auto condition = Text(active, "condition_id");
    const auto activation = Text(active, "activation_id");
    const auto revision = Unsigned(active, "record_revision");
    if (!old_activation.empty()) {
      EXPECT_THROW(static_cast<void>(core.AcknowledgeAlarm(
                       old_condition, old_activation, old_revision,
                       Caller("late.previous"), std::nullopt, true)),
                   OperatorAlarmError);
    }
    const auto acknowledged = core.AcknowledgeAlarm(
        condition, activation, revision, Caller("ack." + std::to_string(index)),
        std::nullopt, true);
    EXPECT_EQ(Array(acknowledged, "audit_event_ids").size(), 1U);
    const auto duplicate = core.AcknowledgeAlarm(
        condition, activation, revision,
        Caller("retry." + std::to_string(index)), std::nullopt, true);
    EXPECT_TRUE(Array(duplicate, "audit_event_ids").empty());
    PublishHealthySources(directory, "open", index * 2 + 1);
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
    old_condition = condition;
    old_activation = activation;
    old_revision = revision;
  }
  HistorianQuery query;
  query.limit = 32U;
  const auto page = core.HistoryQueryPage(query, false);
  const auto &retention = Member(page, "retention");
  EXPECT_EQ(Text(retention, "partition_row_limit"), "4");
  const auto &partitions = Array(retention, "partitions");
  ASSERT_EQ(partitions.size(), 2U);
  for (const auto &partition : partitions) {
    EXPECT_LE(std::stoull(Text(partition, "retained_count")), 4U);
    EXPECT_GT(std::stoull(Text(partition, "pruned_count")), 0U);
  }
  EXPECT_LE(Array(page, "events").size(), 8U);
}

TEST(HeadlessApiCore,
     ProgramModelAttemptIsValidatedAndSynchronouslyStoredInHistorian) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto record = ProgramModelAttemptRecord();

  const auto receipt = core.CommitProgramModelAttempt(record);

  EXPECT_EQ(Text(receipt, "schema_version"),
            "manyforge.program_model_attempt_commit.v1");
  EXPECT_EQ(Text(receipt, "model_attempt_id"), "model-attempt:one");
  EXPECT_EQ(Text(receipt, "status"), "stored");
  EXPECT_FALSE(Text(receipt, "event_id").empty());

  const auto duplicate_receipt = core.CommitProgramModelAttempt(record);
  EXPECT_EQ(Text(duplicate_receipt, "status"), "duplicate");
  EXPECT_EQ(Text(duplicate_receipt, "event_id"), Text(receipt, "event_id"));

  HistorianQuery query;
  query.components.insert("manyforge.program");
  query.event_types.insert("program_model_attempt");
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &event = Member(Array(page, "events").front(), "event");
  EXPECT_EQ(Text(event, "severity"), "INFO");
  EXPECT_EQ(Member(event, "payload"), record);

  auto conflicting_identity = record.as_object();
  for (auto &[name, value] : conflicting_identity) {
    if (name == "model_version") {
      value = CanonicalValue("conflicting-version");
    }
  }
  EXPECT_THROW(static_cast<void>(core.CommitProgramModelAttempt(
                   CanonicalValue(std::move(conflicting_identity)))),
               EventHistorianError);
  EXPECT_EQ(Array(core.HistoryPage(query), "events").size(), 1U);

  const auto failure_receipt =
      core.CommitProgramModelAttempt(ProgramModelAttemptRecord(false));
  EXPECT_EQ(Text(failure_receipt, "model_attempt_id"), "model-attempt:two");
  EXPECT_EQ(Text(failure_receipt, "status"), "stored");
  EXPECT_EQ(Array(core.HistoryPage(query), "events").size(), 2U);

  try {
    static_cast<void>(core.CommitProgramModelAttempt(
        CanonicalValue(CanonicalValue::Object{})));
    FAIL() << "malformed Program model-attempt record was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_program_model_attempt_invalid");
  }

  auto empty_identity = ProgramModelAttemptRecord().as_object();
  for (auto &[name, value] : empty_identity) {
    if (name == "model_attempt_id") {
      value = CanonicalValue("");
    }
  }
  EXPECT_THROW(static_cast<void>(core.CommitProgramModelAttempt(
                   CanonicalValue(std::move(empty_identity)))),
               OperatorAlarmError);

  core.Shutdown();
  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), Clocks());
  const auto restart_receipt = replacement.CommitProgramModelAttempt(record);
  EXPECT_EQ(Text(restart_receipt, "status"), "duplicate");
  EXPECT_EQ(Text(restart_receipt, "event_id"), Text(receipt, "event_id"));
  EXPECT_EQ(Array(replacement.HistoryPage(query), "events").size(), 2U);
}

TEST(HeadlessApiCore,
     ClientIncidentTransitionsAreStrictSparseAndDurablyQueued) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto activated = core.RecordClientIncident(
      "client-incident.one", "control_delayed", "activated",
      "control-session.one", "client.one", "operator_ui", "heartbeat_delayed",
      1U, 600'000'000U, 100U, 100U);
  EXPECT_EQ(Text(activated, "schema_version"),
            "manyforge.client_incident_record.v1");
  EXPECT_TRUE(std::get<bool>(Member(activated, "recorded").storage()));
  const auto resolved = core.RecordClientIncident(
      "client-incident.one", "control_delayed", "resolved",
      "control-session.one", "client.one", "operator_ui", "heartbeat_recovered",
      3U, 700'000'000U, 100U, 800U,
      {{"program_run_id", "run.at.incident"},
       {"scene_id", "scene.at.incident"},
       {"program_source_current", "false"}});
  EXPECT_TRUE(std::get<bool>(Member(resolved, "recorded").storage()));

  HistorianQuery query;
  query.components.insert("manyforge.composer");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 2U) {
      break;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 2U);
  const auto &newest = Member(Array(page, "events").front(), "event");
  EXPECT_EQ(Text(newest, "event_type"), "client_incident_resolved");
  EXPECT_EQ(Text(newest, "severity"), "INFO");
  const auto &payload = Member(newest, "payload");
  EXPECT_EQ(Text(payload, "original_severity"), "WARN");
  EXPECT_EQ(Text(payload, "first_observed_ns"), "100");
  EXPECT_EQ(Text(payload, "last_observed_ns"), "800");
  EXPECT_EQ(Text(Member(payload, "causal_context"), "program_run_id"),
            "run.at.incident");
  EXPECT_EQ(Text(Member(payload, "causal_context"), "scene_id"),
            "scene.at.incident");
  EXPECT_EQ(Text(Member(payload, "causal_context"), "capture_semantics"),
            "last_received_source_facts");

  constexpr auto kBeyondJsonSafeInteger = std::uint64_t{1} << 53U;
  EXPECT_NO_THROW(static_cast<void>(core.RecordClientIncident(
      "client-incident.long-lived", "control_delayed", "activated",
      "control-session.one", "client.one", "operator_ui", "heartbeat_delayed",
      1U, 600'000'000U, kBeyondJsonSafeInteger + 1U,
      kBeyondJsonSafeInteger + 2U)));

  try {
    static_cast<void>(core.RecordClientIncident(
        "client-incident.invalid", "arbitrary_event", "activated",
        "control-session.one", "client.one", "operator_ui", "forged", 1U,
        std::nullopt, 1U, 1U));
    FAIL() << "arbitrary client event kind was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "client_incident_invalid");
  }
}

TEST(HeadlessApiCore, ViewerClientIncidentsAreErrorsWithTheirClosedKinds) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::vector<std::string> kinds{
      "viewer_model_unavailable", "viewer_scene_unverified",
      "viewer_observation_stale", "viewer_geometry_unavailable"};
  std::uint64_t observed = 100U;
  for (const auto &kind : kinds) {
    const auto receipt = core.RecordClientIncident(
        "client-incident." + kind, kind, "activated", "control-session.one",
        "client.one", "operator_ui", kind, 1U, std::nullopt, observed,
        observed);
    EXPECT_TRUE(std::get<bool>(Member(receipt, "recorded").storage())) << kind;
    ++observed;
  }

  HistorianQuery query;
  query.components.insert("manyforge.composer");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == kinds.size()) {
      break;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), kinds.size());
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    // An unavailable or unverified view of the applied cell is an error, so
    // its activation is retained with the important partition's evidence.
    EXPECT_EQ(Text(event, "event_type"), "client_incident_activated");
    EXPECT_EQ(Text(event, "severity"), "ERROR");
    EXPECT_EQ(Text(Member(event, "payload"), "original_severity"), "ERROR");
  }
}

TEST(HeadlessApiCore,
     ProgramHeldDiagnosticIsInformationalAndRetainsExactValues) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  CanonicalValue record(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.program_diagnostic.v3")},
      {"program_run_id", CanonicalValue("run.diagnostic")},
      {"owner_epoch", CanonicalValue("runner.1")},
      {"program_revision", CanonicalValue(Digest('1'))},
      {"program_binding_id", CanonicalValue(Digest('2'))},
      {"retained_bundle_id", CanonicalValue(Digest('3'))},
      {"graph_revision", CanonicalValue(Digest('4'))},
      {"state", CanonicalValue("held")},
      {"reason_code", CanonicalValue("operation_stopped")},
      {"message", CanonicalValue("Program motion stopped and held")},
      {"occurred_at_ns", CanonicalValue("9007199254740993")},
      {"occurred_at_wall", CanonicalValue("2026-09-08T00:00:00Z")},
      {"blackboard", CanonicalValue(CanonicalValue::Object{
                         {"ready", CanonicalValue(false)},
                         {"program_run_id", CanonicalValue("foreign.run")}})},
      {"omitted_blackboard_count", CanonicalValue(2.0)},
      {"delivery_loss_count", CanonicalValue(3.0)},
      {"details", CanonicalValue(nullptr)},
      {"origin", CanonicalValue(nullptr)},
  });
  const auto result = core.RecordProgramDiagnostic(record);
  EXPECT_TRUE(std::get<bool>(Member(result, "recorded").storage()));
  HistorianQuery query;
  query.components.insert("manyforge.program_runner");
  query.correlations.emplace("program_run_id", "run.diagnostic");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 1U)
      break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &stored = Array(page, "events").front();
  const auto &event = Member(stored, "event");
  EXPECT_EQ(Text(event, "occurred_at_ns"), "9007199254740993");
  EXPECT_EQ(Text(event, "event_type"), "program_held");
  EXPECT_EQ(Text(event, "severity"), "INFO");
  EXPECT_EQ(content_identity::canonical_json(
                Member(Member(event, "payload"), "blackboard")),
            "{\"program_run_id\":\"foreign.run\",\"ready\":false}");
  EXPECT_EQ(Text(Member(event, "payload"), "clock_owner"),
            "program_runner:runner.1");
  query.correlations["program_run_id"] = "foreign.run";
  EXPECT_TRUE(Array(core.HistoryPage(query), "events").empty());
  auto near_limit = record.as_object();
  CanonicalValue::Object near_values;
  for (std::size_t index = 0U; index < 8U; ++index) {
    std::string key("a");
    key += std::to_string(index);
    near_values.emplace_back(
        std::move(key),
        CanonicalValue(std::string(index == 7U ? 1761U : 2040U, 'x')));
  }
  near_values.emplace_back(
      "z", CanonicalValue(CanonicalValue::Array(100, CanonicalValue(0.0))));
  for (auto &[name, value] : near_limit) {
    if (name == "blackboard")
      value = CanonicalValue(near_values);
  }
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordProgramDiagnostic(CanonicalValue(near_limit)),
             "recorded")
          .storage()));
  for (const auto &oversized :
       {CanonicalValue(std::string(3000, 'x')),
        CanonicalValue(CanonicalValue::Array(513, CanonicalValue(0.0)))}) {
    auto invalid_values = record.as_object();
    for (auto &[name, value] : invalid_values) {
      if (name == "blackboard")
        value =
            CanonicalValue(CanonicalValue::Object{{"oversized", oversized}});
    }
    EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
                     CanonicalValue(invalid_values))),
                 OperatorAlarmError);
  }
  auto invalid = record.as_object();
  invalid.emplace_back("accepted", CanonicalValue(true));
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
                   CanonicalValue(std::move(invalid)))),
               OperatorAlarmError);
  // Ingest accepts only the current shape; v2 also carried the withdrawn
  // runtime-override section, which v3 removed.
  for (const auto *const retired :
       {"manyforge.program_diagnostic.v1", "manyforge.program_diagnostic.v2"}) {
    auto previous_version = record.as_object();
    for (auto &[name, value] : previous_version) {
      if (name == "schema_version")
        value = CanonicalValue(retired);
    }
    previous_version.emplace_back("runtime_overrides",
                                  CanonicalValue(CanonicalValue::Object{}));
    previous_version.emplace_back("omitted_override_count", CanonicalValue(0.0));
    EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
                     CanonicalValue(std::move(previous_version)))),
                 OperatorAlarmError);
  }
}

TEST(HeadlessApiCore,
     ProgramFailureDiagnosticCarriesBoundedDetailsAndOriginUnchanged) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const CanonicalValue details(CanonicalValue::Object{
      {"reason", CanonicalValue("target_not_ready")},
      {"reference_id", CanonicalValue(std::string(256U, 'r'))},
      {"retryable", CanonicalValue(false)},
      {"latch_id", CanonicalValue(nullptr)},
      {"path", CanonicalValue(2.5)},
  });
  const CanonicalValue origin(CanonicalValue::Object{
      {"kind", CanonicalValue("owner")},
      {"node_instance_id", CanonicalValue("node.move")},
      {"owner_api", CanonicalValue("kernel.operation")},
  });
  const auto with = [](CanonicalValue::Object fields, std::string_view field,
                       CanonicalValue replacement) {
    for (auto &[name, value] : fields) {
      if (name == field)
        value = replacement;
    }
    return CanonicalValue(std::move(fields));
  };
  const CanonicalValue::Object record{
      {"schema_version", CanonicalValue("manyforge.program_diagnostic.v3")},
      {"program_run_id", CanonicalValue("run.failure")},
      {"owner_epoch", CanonicalValue("runner.1")},
      {"program_revision", CanonicalValue(Digest('1'))},
      {"program_binding_id", CanonicalValue(Digest('2'))},
      {"retained_bundle_id", CanonicalValue(Digest('3'))},
      {"graph_revision", CanonicalValue(Digest('4'))},
      {"state", CanonicalValue("failed")},
      {"reason_code", CanonicalValue("owner_refused")},
      {"message", CanonicalValue("Kernel refused node.move: target not ready")},
      {"occurred_at_ns", CanonicalValue("10")},
      {"occurred_at_wall", CanonicalValue("2026-09-08T00:00:00Z")},
      {"blackboard", CanonicalValue(CanonicalValue::Object{})},
      {"omitted_blackboard_count", CanonicalValue(0.0)},
      {"delivery_loss_count", CanonicalValue(0.0)},
      {"details", details},
      {"origin", origin},
  };
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordProgramDiagnostic(CanonicalValue(record)), "recorded")
          .storage()));
  HistorianQuery query;
  query.correlations.emplace("program_run_id", "run.failure");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 1U)
      break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &payload =
      Member(Member(Array(page, "events").front(), "event"), "payload");
  EXPECT_EQ(Member(payload, "details"), details);
  EXPECT_EQ(Member(payload, "origin"), origin);

  for (const auto &[kind, node, owner_api] :
       std::vector<std::tuple<std::string, CanonicalValue, CanonicalValue>>{
           {"program_node", CanonicalValue("node.wait"),
            CanonicalValue(nullptr)},
           {"owner", CanonicalValue("node.move"), CanonicalValue(nullptr)},
           {"program_runner", CanonicalValue(nullptr), CanonicalValue(nullptr)},
           {"program_runner", CanonicalValue("node.move"),
            CanonicalValue(nullptr)},
       }) {
    EXPECT_NO_THROW(static_cast<void>(
        core.RecordProgramDiagnostic(with(record, "origin",
                                          CanonicalValue(CanonicalValue::Object{
                                              {"kind", CanonicalValue(kind)},
                                              {"node_instance_id", node},
                                              {"owner_api", owner_api},
                                          })))));
  }
  EXPECT_NO_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", CanonicalValue(nullptr)))));

  CanonicalValue::Object registered;
  for (const auto key :
       {"input_problem", "latch_id", "path", "reason", "reference_id",
        "reference_type", "required_action", "retryable", "source_kind",
        "source_reference", "target_role_id"}) {
    registered.emplace_back(key, CanonicalValue(true));
  }
  EXPECT_NO_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", CanonicalValue(registered)))));
  // Nine maximal strings exceed the 2,048-byte canonical details bound.
  CanonicalValue::Object oversized;
  for (const auto key : {"input_problem", "path", "reason", "reference_id",
                         "reference_type", "required_action", "source_kind",
                         "source_reference", "target_role_id"}) {
    oversized.emplace_back(key, CanonicalValue(std::string(250U, 'x')));
  }
  const auto origin_of = [](CanonicalValue kind, CanonicalValue node,
                            CanonicalValue owner_api) {
    return CanonicalValue(CanonicalValue::Object{
        {"kind", std::move(kind)},
        {"node_instance_id", std::move(node)},
        {"owner_api", std::move(owner_api)},
    });
  };
  for (const auto &[field, replacement] :
       std::vector<std::pair<std::string, CanonicalValue>>{
           {"details", CanonicalValue(CanonicalValue::Object{
                           {"unregistered", CanonicalValue("x")}})},
           {"details",
            CanonicalValue(CanonicalValue::Object{
                {"reason", CanonicalValue(std::string(257U, 'x'))}})},
           {"details",
            CanonicalValue(CanonicalValue::Object{
                {"reason", CanonicalValue(CanonicalValue::Object{})}})},
           {"details",
            CanonicalValue(CanonicalValue::Object{
                {"reason", CanonicalValue(CanonicalValue::Array{})}})},
           {"details", CanonicalValue(oversized)},
           {"details", CanonicalValue(CanonicalValue::Object{})},
           {"details", CanonicalValue("reason")},
           {"origin", CanonicalValue(nullptr)},
           {"origin", CanonicalValue("owner")},
           {"origin",
            origin_of(CanonicalValue("kernel"), CanonicalValue("node.move"),
                      CanonicalValue(nullptr))},
           {"origin",
            origin_of(CanonicalValue("program_node"), CanonicalValue(nullptr),
                      CanonicalValue(nullptr))},
           {"origin", origin_of(CanonicalValue("program_node"),
                                CanonicalValue("node.move"),
                                CanonicalValue("kernel.operation"))},
           {"origin",
            origin_of(CanonicalValue("owner"), CanonicalValue(nullptr),
                      CanonicalValue("kernel.operation"))},
           {"origin",
            origin_of(CanonicalValue("program_runner"), CanonicalValue(nullptr),
                      CanonicalValue("kernel.operation"))},
           {"origin", origin_of(CanonicalValue("owner"),
                                CanonicalValue(std::string(257U, 'n')),
                                CanonicalValue(nullptr))},
           {"origin",
            origin_of(CanonicalValue("owner"), CanonicalValue("node.move"),
                      CanonicalValue(""))},
           {"origin", CanonicalValue(CanonicalValue::Object{
                          {"kind", CanonicalValue("program_runner")},
                          {"node_instance_id", CanonicalValue(nullptr)},
                      })},
       }) {
    EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
                     with(record, field, replacement))),
                 OperatorAlarmError)
        << field << " " << content_identity::canonical_json(replacement);
  }
  auto succeeded = with(record, "state", CanonicalValue("succeeded"));
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(succeeded)),
               OperatorAlarmError);
}

TEST(HeadlessApiCore,
     DurableOperationFactsDriveHistoryAndAlarmWithoutOwningExecutionState) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.1";
  const auto failure = OperationJournalPayload(
      source_epoch, "state_observed", "failed",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"reason", CanonicalValue("planner_failed")},
                      })},
          {"message", CanonicalValue("planning failed")},
      }),
      std::nullopt, ResourceIds(*configurations.single).front());
  const auto failure_json = content_identity::canonical_json(failure);
  const auto failure_digest = content_identity::sha256_digest(failure_json);
  const auto ingested = core.IngestOperationEvent(
      source_epoch, 1U, "state_observed", std::nullopt, "operation.test.1", 1U,
      100U, "2026-08-07T10:00:00.000000001Z", false, failure, failure_digest);
  EXPECT_EQ(Text(ingested, "status"), "ingested");
  EXPECT_FALSE(std::holds_alternative<std::nullptr_t>(
      Member(ingested, "event_id").storage()));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 1U));

  auto alarms = core.AlarmSnapshot();
  const auto &fault = OnlyAlarm(alarms);
  EXPECT_EQ(Text(fault, "definition_id"), "manyforge.motion.execution_fault");
  EXPECT_EQ(Text(fault, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(fault, "subject"), "subject_id"), "operation.test.1");

  const auto replayed = core.IngestOperationEvent(
      source_epoch, 1U, "state_observed", std::nullopt, "operation.test.1", 1U,
      100U, "2026-08-07T10:00:00.000000001Z", false, failure, failure_digest);
  EXPECT_EQ(Text(replayed, "status"), "already_ingested");

  const auto unavailable = core.MarkOperationEventSourceUnknown(source_epoch);
  EXPECT_EQ(Text(unavailable, "status"), "marked_unknown");
  const auto unknown_snapshot = core.AlarmSnapshot();
  const auto &unknown_domains = Array(unknown_snapshot, "domains");
  const auto unknown_motion = std::find_if(
      unknown_domains.begin(), unknown_domains.end(),
      [](const auto &domain) { return Text(domain, "domain") == "motion"; });
  ASSERT_NE(unknown_motion, unknown_domains.end());
  EXPECT_FALSE(std::get<bool>(Member(*unknown_motion, "reconciled").storage()));
  EXPECT_EQ(Unsigned(*unknown_motion, "unknown_source_count"), 1U);
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 1U));
  const auto recovered_snapshot = core.AlarmSnapshot();
  const auto &recovered_domains = Array(recovered_snapshot, "domains");
  const auto recovered_motion = std::find_if(
      recovered_domains.begin(), recovered_domains.end(),
      [](const auto &domain) { return Text(domain, "domain") == "motion"; });
  ASSERT_NE(recovered_motion, recovered_domains.end());
  EXPECT_TRUE(
      std::get<bool>(Member(*recovered_motion, "reconciled").storage()));
  EXPECT_EQ(Unsigned(*recovered_motion, "unknown_source_count"), 0U);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (HistoryCount(core) < 2U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(HistoryCount(core), 2U);

  const auto retired =
      core.CompleteOperationEventSnapshot("execution-host.epoch.2", 0U);
  EXPECT_EQ(Text(retired, "status"), "complete");
  alarms = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(alarms);
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  const auto returned_deadline = std::chrono::steady_clock::now() + 2s;
  while (HistoryCount(core) < 3U &&
         std::chrono::steady_clock::now() < returned_deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(HistoryCount(core), 3U);
}

TEST(HeadlessApiCore,
     PreparedOperationEvidenceIsAcceptedAsAHistoryFactWithoutRaisingAnAlarm) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.prepared";
  const auto prepared = OperationJournalPayload(
      source_epoch, "operation_prepared", "prepared", CanonicalValue(nullptr));
  const auto prepared_json = content_identity::canonical_json(prepared);
  const auto ingested = core.IngestOperationEvent(
      source_epoch, 1U, "operation_prepared", std::nullopt, "operation.test.1",
      1U, 100U, "2026-08-07T10:00:00.000000001Z", false, prepared,
      content_identity::sha256_digest(prepared_json));
  EXPECT_EQ(Text(ingested, "status"), "ingested");
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("operation_prepared");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  EXPECT_EQ(
      Member(Member(Array(page, "events").front(), "event"), "event_type"),
      CanonicalValue("operation_prepared"));
}

TEST(HeadlessApiCore, OrderedJournalCapturesPreserveExactInt64HistoryIngress) {
  for (const auto order : {std::uint64_t{1}, (std::uint64_t{1} << 53U) + 1U,
                           static_cast<std::uint64_t>(INT64_MAX)}) {
    SCOPED_TRACE(order);
    TemporaryDirectory directory;
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    const std::string epoch = "execution-host.ordered";
    auto fields = OperationJournalPayload(epoch, "operation_prepared",
                                          "prepared", CanonicalValue(nullptr))
                      .as_object();
    for (auto &[name, value] : fields) {
      if (name == "api_version")
        value = CanonicalValue("manyforge.operation_journal_record.v8");
      if (name == "operation_state") {
        auto state = value.as_object();
        state.emplace_back("journal_capture_order",
                           CanonicalValue(std::to_string(order)));
        value = CanonicalValue(std::move(state));
      }
    }
    const CanonicalValue payload(std::move(fields));
    const auto ingested = core.IngestOperationEvent(
        epoch, 1U, "operation_prepared", std::nullopt, "operation.test.1", 1U,
        100U, "2026-09-07T10:00:00.000000001Z", false, payload,
        content_identity::sha256_digest(
            content_identity::canonical_json(payload)));
    EXPECT_EQ(Text(ingested, "status"), "ingested");
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }
}

TEST(HeadlessApiCore,
     CapturedProgramOriginIsTracedWithAnExplicitJournalVersion) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.captured";
  auto fields = OperationJournalPayload(epoch, "operation_prepared", "prepared",
                                        CanonicalValue(nullptr))
                    .as_object();
  for (auto &[name, value] : fields) {
    if (name == "api_version")
      value = CanonicalValue("manyforge.operation_journal_record.v8");
    if (name != "operation_state")
      continue;
    auto state = value.as_object();
    state.emplace_back("journal_capture_order", CanonicalValue("1"));
    for (auto &[field, content] : state) {
      if (field != "participants")
        continue;
      auto participants = content.as_array();
      auto participant = participants.front().as_object();
      participant.emplace_back(
          "execution_scope",
          CanonicalValue(CanonicalValue::Object{
              {"node_instance_id", CanonicalValue("pick")},
              {"node_attempt_id", CanonicalValue("pick.attempt.1")}}));
      participants.front() = CanonicalValue(std::move(participant));
      content = CanonicalValue(std::move(participants));
    }
    value = CanonicalValue(std::move(state));
  }
  const auto ingest = [&](const CanonicalValue &payload) {
    return core.IngestOperationEvent(
        epoch, 1U, "operation_prepared", std::nullopt, "operation.test.1", 1U,
        100U, "2026-09-07T10:00:00.000000001Z", false, payload,
        content_identity::sha256_digest(
            content_identity::canonical_json(payload)));
  };
  const CanonicalValue payload(fields);
  EXPECT_EQ(Text(ingest(payload), "status"), "ingested");
  for (auto &[name, value] : fields) {
    if (name == "api_version")
      value = CanonicalValue("manyforge.operation_journal_record.v2");
  }
  EXPECT_THROW(ingest(CanonicalValue(fields)), std::exception);
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  const auto stop_ingest = [&](const std::string &api,
                               const CanonicalValue &order) {
    auto operation = Member(payload, "operation_state").as_object();
    for (auto &[name, value] : operation) {
      if (name == "journal_capture_order")
        value = order;
    }
    auto stop = StopAcceptanceProjectionPayload(epoch).as_object();
    for (auto &[name, value] : stop) {
      if (name == "api_version")
        value = CanonicalValue(api);
      if (name == "affected_state")
        value = CanonicalValue(CanonicalValue::Object{
            {"operations",
             CanonicalValue(CanonicalValue::Array{CanonicalValue(operation)})},
            {"resources", CanonicalValue(CanonicalValue::Array{})}});
    }
    const CanonicalValue projection(std::move(stop));
    return core.IngestOperationEvent(
        epoch, 2U, "stop_acceptance_projected", "stop-request.1", std::nullopt,
        std::nullopt, 101U, "2026-09-07T10:00:00.000000002Z", true, projection,
        content_identity::sha256_digest(
            content_identity::canonical_json(projection)));
  };
  EXPECT_THROW(stop_ingest("manyforge.stop_acceptance_projection.v2",
                           CanonicalValue("1")),
               std::exception);
  for (const auto &bad_order :
       {CanonicalValue("0"), CanonicalValue(1.0), CanonicalValue(nullptr)}) {
    EXPECT_THROW(
        stop_ingest("manyforge.stop_acceptance_projection.v8", bad_order),
        std::exception);
  }
  EXPECT_EQ(Text(stop_ingest("manyforge.stop_acceptance_projection.v8",
                             CanonicalValue("1")),
                 "status"),
            "ingested");
}

TEST(HeadlessApiCore,
     OversizedCommittedFactHasRecoverableGenerationFencedHistoryHealth) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.epoch.1";
  const auto normal = OperationJournalPayload(
      epoch, "operation_prepared", "prepared", CanonicalValue(nullptr));
  auto fields = std::get<CanonicalValue::Object>(normal.storage());
  for (auto &[key, value] : fields) {
    if (key == "intent") {
      value = CanonicalValue(CanonicalValue::Object{
          {"diagnostic", CanonicalValue(std::string(300 * 1024, 'x'))}});
    }
  }
  const CanonicalValue oversized(std::move(fields));
  const auto ingest = [&](const CanonicalValue &payload,
                          const std::uint64_t sequence) {
    return core.IngestOperationEvent(
        epoch, sequence, "operation_prepared", std::nullopt, "operation.test.1",
        1U, 100, "2026-08-07T10:00:00Z", false, payload,
        content_identity::sha256_digest(
            content_identity::canonical_json(payload)));
  };
  EXPECT_EQ(Text(ingest(oversized, 1), "status"), "ingested");
  const auto failed = core.HistorianHealth();
  EXPECT_FALSE(std::get<bool>(Member(failed, "available").storage()));
  const auto generation = Unsigned(failed, "health_generation");
  EXPECT_FALSE(std::get<bool>(
      Member(core.ReconcileHistorianHealth(generation, Caller("early")),
             "reconciled")
          .storage()));
  EXPECT_EQ(Text(ingest(normal, 2), "status"), "ingested");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!std::get<bool>(
             Member(core.HistorianHealth(), "recovery_ready").storage()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto recovered =
      core.ReconcileHistorianHealth(generation, Caller("repair"));
  EXPECT_TRUE(std::get<bool>(Member(recovered, "reconciled").storage()));
  EXPECT_EQ(Array(recovered, "audit_event_ids").size(), 1U);
  EXPECT_TRUE(
      std::get<bool>(Member(core.HistorianHealth(), "available").storage()));
}

TEST(HeadlessApiCore,
     GlobalStopAcceptanceProjectionKeepsPairedNullOperationIdentity) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.stop";
  const auto projection = StopAcceptanceProjectionPayload(source_epoch);
  const auto projection_json = content_identity::canonical_json(projection);
  const auto projection_digest =
      content_identity::sha256_digest(projection_json);

  const auto ingested = core.IngestOperationEvent(
      source_epoch, 1U, "stop_acceptance_projected", "stop-request.1",
      std::nullopt, std::nullopt, 100U, "2026-08-07T10:00:00.000000001Z", true,
      projection, projection_digest);
  EXPECT_EQ(Text(ingested, "status"), "ingested");
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("stop_acceptance_projected");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &history_payload = Member(Array(page, "events").front(), "event");
  const auto &envelope = Member(history_payload, "payload");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Member(envelope, "operation_id").storage()));
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Member(envelope, "operation_revision").storage()));
  EXPECT_EQ(Member(envelope, "journal_payload"), projection);

  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   source_epoch, 2U, "stop_acceptance_projected",
                   "stop-request.1", "operation.test.1", std::nullopt, 101U,
                   "2026-08-07T10:00:00.000000002Z", true, projection,
                   projection_digest)),
               OperatorAlarmError);
  EXPECT_THROW(
      static_cast<void>(core.IngestOperationEvent(
          source_epoch, 2U, "state_observed", std::nullopt, std::nullopt,
          std::nullopt, 101U, "2026-08-07T10:00:00.000000002Z", false,
          projection, projection_digest)),
      OperatorAlarmError);
}

// Produce the same committed payloads read by the operational events route.
// These typed journal facts do not assert a Kernel trip/Stop execution.
std::vector<manyforge::core::OperationJournalEventDTO> NativeRuleStateEvents(
    const TemporaryDirectory &directory, const std::string &source_epoch) {
  namespace native = manyforge::core;
  const native::RequestOwnerEpoch owner(source_epoch);
  native::SqliteOperationJournal journal(
      directory.path() / "native-rule-state.sqlite3",
      native::CellId(std::string(kCellId)), 8U,
      [] { return "2026-10-18T10:00:00.000000001Z"; });
  const auto opened = journal.BeginOwnerEpoch(owner, 0, 8U, {});
  if (!opened.ok())
    throw std::runtime_error(opened.error().message);
  native::IoRuleStateChangeDTO change{
      {owner, UINT64_MAX, native::ActiveBundlePublicationGeneration(3U),
       native::RetainedBundleId("retained.global"),
       native::GraphRevision("graph.global")},
      {native::InterruptPolicyId("door.guard"),
       native::IoGlobalRuleState::kTripped,
       native::IoRuleAcquisitionKeyDTO{
           native::InterruptSourceId("door.source"),
           native::IoSourceIncarnation("door.epoch"), UINT64_MAX,
           {owner, UINT64_MAX, native::InterruptSourceId("door.source"),
            UINT64_MAX}}},
      100};
  for (const auto state : {native::IoGlobalRuleState::kTripped,
                           native::IoGlobalRuleState::kLatched,
                           native::IoGlobalRuleState::kClear}) {
    change.rule.state = state;
    if (state == native::IoGlobalRuleState::kClear)
      change.rule.triggering_acquisition.reset();
    ++change.occurred_at_ns;
    const auto recorded = journal.RecordInterruptRuleStateChanged(change);
    if (!recorded.ok())
      throw std::runtime_error(recorded.error().message);
  }
  const auto page = journal.ReadOperationalEvents(owner, 0U);
  if (!page.ok() || page.value().events.size() != 3U)
    throw std::runtime_error("actual Core rule-state history is unavailable");
  return page.value().events;
}

TEST(HeadlessApiCore, NotifyUnknownWithTypedReasonPassesEventValidation) {
  TemporaryDirectory directory;
  namespace native = manyforge::core;
  const std::string owner = "execution-host.epoch.notify";
  native::SqliteOperationJournal journal(directory.path() / "notify.sqlite3",
                                         native::CellId(std::string(kCellId)), 8U);
  ASSERT_TRUE(journal.BeginOwnerEpoch(native::RequestOwnerEpoch(owner), 0, 8U, {}).ok());
  native::IoRuleStateChangeDTO change{
      {native::RequestOwnerEpoch(owner), 4U, native::ActiveBundlePublicationGeneration(3U),
       native::RetainedBundleId("retained.global"), native::GraphRevision("graph.global")},
      {native::InterruptPolicyId("door.guard"), native::IoGlobalRuleState::kUnknown,
       std::nullopt, native::ErrorDTO{"io_rule_source_unavailable", "lost", {{"source", "door"}}}},
      100};
  ASSERT_TRUE(journal.RecordInterruptRuleStateChanged(change).ok());
  const auto page = journal.ReadOperationalEvents(native::RequestOwnerEpoch(owner), 0U);
  ASSERT_TRUE(page.ok());
  ASSERT_EQ(page.value().events.size(), 1U);
  const auto &event = page.value().events.front();
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  EXPECT_EQ(Text(core.IngestOperationEvent(
      owner, event.sequence, event.record_kind, std::nullopt, std::nullopt, std::nullopt,
      event.occurred_at_ns, event.occurred_at_wall, event.effect_pending,
      content_identity::parse_canonical_json(event.payload_json), event.payload_digest), "status"), "ingested");
}

TEST(HeadlessApiCore,
     ActualCoreRuleTransitionsReachHistoryWithNoFakeOperationAndReplaySafely) {
  TemporaryDirectory directory;
  const std::string owner = "execution-host.epoch.rules";
  const auto events = NativeRuleStateEvents(directory, owner);
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto ingest = [&](const auto &event) {
    EXPECT_EQ(event.record_kind, "interrupt_rule_state_changed");
    EXPECT_FALSE(event.request_id);
    EXPECT_FALSE(event.operation_id);
    EXPECT_FALSE(event.operation_revision);
    EXPECT_FALSE(event.effect_pending);
    return core.IngestOperationEvent(
        owner, event.sequence, event.record_kind, std::nullopt, std::nullopt,
        std::nullopt, event.occurred_at_ns, event.occurred_at_wall,
        event.effect_pending,
        content_identity::parse_canonical_json(event.payload_json),
        event.payload_digest);
  };
  for (const auto &event : events)
    EXPECT_EQ(Text(ingest(event), "status"), "ingested");
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(owner,
                                                    events.back().sequence),
                 "status"),
            "complete");
  EXPECT_EQ(Text(ingest(events.front()), "status"), "already_ingested");
  EXPECT_EQ(Text(ingest(events.back()), "status"), "already_ingested");
  // The current source cursor cannot name different retained content.
  const auto &first = events.front();
  EXPECT_THROW(
      static_cast<void>(core.IngestOperationEvent(
          owner, events.back().sequence, first.record_kind, std::nullopt,
          std::nullopt, std::nullopt, first.occurred_at_ns,
          first.occurred_at_wall, false,
          content_identity::parse_canonical_json(first.payload_json),
          first.payload_digest)),
      OperatorAlarmError);

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("interrupt_rule_state_changed");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").size() != events.size() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), events.size());
  std::set<std::string> states;
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    EXPECT_EQ(Text(event, "schema_version"), "manyforge.historian_event.v2");
    EXPECT_EQ(Text(event, "event_type"), "interrupt_rule_state_changed");
    EXPECT_EQ(Text(event, "severity"), "INFO");
    const auto &payload = Member(event, "payload");
    EXPECT_EQ(Text(payload, "schema_version"),
              "manyforge.operation_history_event.v1");
    EXPECT_EQ(Text(payload, "source_owner_epoch"), owner);
    EXPECT_FALSE(Member(payload, "native_sequence_gap").as_bool());
    for (const auto *name : {"operation_id", "operation_revision", "request_id"})
      EXPECT_TRUE(Member(payload, name).is_null());
    const auto sequence = Unsigned(payload, "native_sequence");
    const auto actual = std::find_if(events.begin(), events.end(),
                                    [&](const auto &row) {
                                      return row.sequence == sequence;
                                    });
    ASSERT_NE(actual, events.end());
    EXPECT_EQ(Text(payload, "payload_digest"), actual->payload_digest);
    EXPECT_EQ(Member(payload, "journal_payload"),
              content_identity::parse_canonical_json(actual->payload_json));
    const auto &journal = Member(payload, "journal_payload");
    states.insert(Text(journal, "state"));
    EXPECT_EQ(Text(journal, "catalog_generation"),
              "18446744073709551615");
    const auto &acquired = Member(journal, "triggering_acquisition");
    if (!acquired.is_null()) {
      EXPECT_EQ(Text(acquired, "source_sequence"),
                "18446744073709551615");
      EXPECT_EQ(Text(Member(acquired, "source_cursor"), "record_sequence"),
                "18446744073709551615");
    }
  }
  EXPECT_EQ(states, (std::set<std::string>{"clear", "latched", "tripped"}));
}

TEST(HeadlessApiCore,
     RuleStateHistoryRejectsForeignContextDigestAndOperationIdentityBeforeCursor) {
  TemporaryDirectory directory;
  const std::string owner = "execution-host.epoch.rules";
  const auto events = NativeRuleStateEvents(directory, owner);
  const auto &event = events.front();
  const auto payload = content_identity::parse_canonical_json(event.payload_json);
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto ingest = [&](const CanonicalValue &value, std::string digest) {
    return core.IngestOperationEvent(
        owner, event.sequence, event.record_kind, std::nullopt, std::nullopt,
        std::nullopt, event.occurred_at_ns, event.occurred_at_wall, false,
        value, std::move(digest));
  };
  const auto &acquired = Member(payload, "triggering_acquisition");
  const auto &cursor = Member(acquired, "source_cursor");
  for (const auto &invalid : {
           WithField(payload, "owner_epoch", CanonicalValue("foreign.owner")),
           WithField(payload, "catalog_generation", CanonicalValue("0")),
           WithField(payload, "catalog_generation", CanonicalValue("019")),
           WithField(payload, "catalog_generation",
                     CanonicalValue("18446744073709551616")),
           WithField(payload, "active_publication_generation", CanonicalValue(0.0)),
           WithField(payload, "graph_revision", CanonicalValue("")),
           WithField(payload, "state", CanonicalValue("pending")),
           WithField(payload, "state", CanonicalValue("clear")),
           WithField(payload, "triggering_acquisition",
                     WithField(acquired, "source_cursor",
                               WithField(cursor, "catalog_generation",
                                         CanonicalValue("1"))))}) {
    EXPECT_THROW(static_cast<void>(ingest(
                     invalid, content_identity::sha256_digest(
                                  content_identity::canonical_json(invalid)))),
                 OperatorAlarmError);
  }
  auto extra = payload.as_object();
  extra.emplace_back("operation_id", CanonicalValue("invented.operation"));
  const CanonicalValue forged(std::move(extra));
  EXPECT_THROW(static_cast<void>(ingest(
                   forged, content_identity::sha256_digest(
                               content_identity::canonical_json(forged)))),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(ingest(payload, Digest('0'))),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   owner, event.sequence, event.record_kind, std::nullopt,
                   "invented.operation", 1U, event.occurred_at_ns,
                   event.occurred_at_wall, false, payload, event.payload_digest)),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   owner, event.sequence, event.record_kind, "invented.request",
                   std::nullopt, std::nullopt, event.occurred_at_ns,
                   event.occurred_at_wall, false, payload, event.payload_digest)),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   owner, event.sequence, event.record_kind, std::nullopt,
                   std::nullopt, std::nullopt, event.occurred_at_ns,
                   event.occurred_at_wall, true, payload, event.payload_digest)),
               OperatorAlarmError);
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(owner, 0U), "status"),
            "complete");
  EXPECT_EQ(Text(ingest(payload, event.payload_digest), "status"), "ingested");
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(owner, event.sequence),
                 "status"),
            "complete");
}

TEST(HeadlessApiCore,
     GlobalStopTerminalProjectionPublishesCausalHistoryAndLatency) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.stop";
  const auto projection = StopTerminalProjectionPayload(source_epoch);
  const auto projection_json = content_identity::canonical_json(projection);
  const auto projection_digest =
      content_identity::sha256_digest(projection_json);

  const auto ingested = core.IngestOperationEvent(
      source_epoch, 2U, "stop_terminal_projected", std::nullopt, std::nullopt,
      std::nullopt, 1100U, "2026-08-07T10:00:01.000000001Z", false, projection,
      projection_digest);
  EXPECT_EQ(Text(ingested, "status"), "ingested");

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("stop_terminal_projected");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &stored = Array(page, "events").front();
  const auto &history_event = Member(stored, "event");
  EXPECT_EQ(Text(history_event, "event_type"), "stop_terminal_projected");
  EXPECT_EQ(Text(history_event, "severity"), "INFO");
  const auto &history_payload = Member(history_event, "payload");
  EXPECT_EQ(Unsigned(history_payload, "duration_ns"), 1000U);
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Member(history_payload, "operation_id").storage()));
  EXPECT_EQ(Member(history_payload, "journal_payload"), projection);

  const auto detail = core.HistoryDetail(Unsigned(stored, "sequence"));
  const auto &latency = Member(Member(detail, "sections"), "latency");
  EXPECT_TRUE(std::get<bool>(Member(latency, "available").storage()));
  EXPECT_EQ(Text(Member(latency, "value"), "unit"), "ns");
  EXPECT_EQ(Unsigned(Member(latency, "value"), "value"), 1000U);

  EXPECT_THROW(
      static_cast<void>(core.IngestOperationEvent(
          source_epoch, 3U, "stop_terminal_projected", std::nullopt,
          "operation.test.1", 1U, 1101U, "2026-08-07T10:00:01.000000002Z",
          false, projection, projection_digest)),
      OperatorAlarmError);
}

TEST(HeadlessApiCore,
     KernelRuntimeCollisionStopDrivesDedicatedAlarmUntilAcceptedReset) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.collision";
  const auto projection =
      RuntimeCollisionStopAcceptanceProjectionPayload(source_epoch, resource);
  const auto projection_digest = content_identity::sha256_digest(
      content_identity::canonical_json(projection));

  const auto ingested = core.IngestOperationEvent(
      source_epoch, 1U, "stop_acceptance_projected", "stop-request.collision.1",
      std::nullopt, std::nullopt, 300U, "2026-08-07T10:00:00.000000007Z", true,
      projection, projection_digest);
  EXPECT_EQ(Text(ingested, "status"), "ingested");
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 1U));

  const auto active_snapshot = core.AlarmSnapshot();
  const auto &active = OnlyAlarm(active_snapshot);
  EXPECT_EQ(Text(active, "definition_id"),
            "manyforge.motion.runtime_collision");
  EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(active, "priority"), "critical");
  EXPECT_TRUE(
      std::get<bool>(Member(active, "requires_motion_attention").storage()));
  EXPECT_EQ(Text(Member(active, "subject"), "subject_id"), "operation.test.1");
  ASSERT_EQ(Array(Member(active, "subject"), "resource_ids").size(), 1U);
  EXPECT_EQ(
      std::get<std::string>(
          Array(Member(active, "subject"), "resource_ids").front().storage()),
      resource);
  EXPECT_EQ(Text(Member(active, "details"), "reason"),
            "runtime_collision_fault");
  EXPECT_EQ(Text(Member(active, "details"), "source_label"),
            "kernel.runtime_collision");
  EXPECT_EQ(Text(Member(active, "correlations"), "participant_id"),
            "participant.collision.1");
  EXPECT_EQ(Text(Member(active, "correlations"), "program_run_id"),
            "program-run.test");

  // Items 19 and Q30: the contained collision ends the operation
  // InterventionRequired; that outcome raises no second (execution-fault)
  // alarm for the same event.
  const auto intervention = OperationJournalPayload(
      source_epoch, "state_observed", "intervention_required",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"reason", CanonicalValue("runtime_collision_fault")}})},
          {"message",
           CanonicalValue("Runtime collision containment stopped the operation")},
      }),
      std::nullopt, resource);
  const auto intervention_digest = content_identity::sha256_digest(
      content_identity::canonical_json(intervention));
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 2U, "state_observed", std::nullopt, "operation.test.1", 1U,
      301U, "2026-08-07T10:00:00.000000008Z", false, intervention,
      intervention_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 2U));
  const auto intervention_snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(intervention_snapshot), "definition_id"),
            "manyforge.motion.runtime_collision");

  const auto reset = OperationJournalPayload(
      source_epoch, "control_accepted", "intervention_required",
      CanonicalValue(CanonicalValue::Object{}), "reset", resource);
  const auto reset_digest =
      content_identity::sha256_digest(content_identity::canonical_json(reset));
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 3U, "control_accepted", "request.reset.collision.1",
      "operation.test.1", 1U, 302U, "2026-08-07T10:00:00.000000009Z", true,
      reset, reset_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 3U));

  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "definition_id"),
            "manyforge.motion.runtime_collision");
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"),
            "operation_reset_accepted");
}

TEST(HeadlessApiCore,
     KernelRuntimeMonitorObservationLossDrivesItsOwnAlarmUntilAcceptedReset) {
  // Live incident 2026-10-29: the containment Stop for lost measured robot
  // state was annunciated as a runtime collision. It keeps the same Stop
  // source and outcome but raises its own alarm.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.observation-lost";
  auto projection =
      RuntimeCollisionStopAcceptanceProjectionPayload(source_epoch, resource);
  const auto intent = Member(projection, "intent");
  projection = JournalField(
      projection, "intent",
      JournalField(intent, "value",
                   JournalField(Member(intent, "value"), "reason",
                                CanonicalValue("runtime_monitor_observation_lost"))));
  const auto projection_digest = content_identity::sha256_digest(
      content_identity::canonical_json(projection));
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 1U, "stop_acceptance_projected", "stop-request.collision.1",
      std::nullopt, std::nullopt, 300U, "2026-08-07T10:00:00.000000007Z", true,
      projection, projection_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 1U));

  const auto active_snapshot = core.AlarmSnapshot();
  const auto &active = OnlyAlarm(active_snapshot);
  EXPECT_EQ(Text(active, "definition_id"),
            "manyforge.motion.runtime_monitor_observation_lost");
  EXPECT_EQ(Text(active, "title"), "Robot position feedback lost");
  EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(active, "priority"), "critical");
  EXPECT_EQ(Text(Member(active, "details"), "reason"),
            "runtime_monitor_observation_lost");
  EXPECT_EQ(Text(Member(active, "correlations"), "program_run_id"),
            "program-run.test");

  // Its InterventionRequired outcome raises no execution-fault alarm.
  const auto intervention = OperationJournalPayload(
      source_epoch, "state_observed", "intervention_required",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("runtime_monitor_observation_lost")},
          {"details",
           CanonicalValue(CanonicalValue::Object{
               {"reason", CanonicalValue("runtime_monitor_observation_lost")}})},
          {"message",
           CanonicalValue(
               "Runtime monitor stopped the operation: measured robot state was lost")},
      }),
      std::nullopt, resource);
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 2U, "state_observed", std::nullopt, "operation.test.1", 1U,
      301U, "2026-08-07T10:00:00.000000008Z", false, intervention,
      content_identity::sha256_digest(
          content_identity::canonical_json(intervention))));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 2U));
  const auto intervention_snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(intervention_snapshot), "definition_id"),
            "manyforge.motion.runtime_monitor_observation_lost");

  // The accepted Reset returns exactly this alarm.
  const auto reset = OperationJournalPayload(
      source_epoch, "control_accepted", "intervention_required",
      CanonicalValue(CanonicalValue::Object{}), "reset", resource);
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 3U, "control_accepted", "request.reset.collision.1",
      "operation.test.1", 1U, 302U, "2026-08-07T10:00:00.000000009Z", true,
      reset,
      content_identity::sha256_digest(content_identity::canonical_json(reset))));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 3U));
  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "definition_id"),
            "manyforge.motion.runtime_monitor_observation_lost");
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"),
            "operation_reset_accepted");
}

TEST(HeadlessApiCore, NominalStoppedFailureIsErrorButOrdinaryStopRemainsInfo) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.history"),
                       Clocks());
  const std::string epoch = "execution-host.history";
  IngestJournalEvent(core, CapturedCollisionStop(epoch, resource), epoch,
                   1U);
  auto collision = CapturedOperation(epoch, resource, CollisionFailure());
  collision = JournalField(collision, "operation_state",
                         JournalField(Member(collision, "operation_state"),
                                    "state", CanonicalValue("stopped")));
  IngestJournalEvent(core, collision, epoch, 2U);
  IngestJournalEvent(core,
                   OperationJournalPayload(epoch, "state_observed", "stopped",
                                           CanonicalValue(nullptr),
                                           std::nullopt, resource,
                                           "operation.ordinary.stop"),
                   epoch, 3U);
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            "manyforge.motion.runtime_collision");
  HistorianQuery query;
  query.event_types.insert("operation_stopped");
  CanonicalValue page;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 2U)
      break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 2U);
  for (const auto &row : Array(page, "events")) {
    const auto &event = Member(row, "event");
    const auto &payload = Member(event, "payload");
    if (Text(payload, "operation_id") == "operation.ordinary.stop") {
      EXPECT_EQ(Text(event, "severity"), "INFO");
    } else {
      EXPECT_EQ(Text(event, "severity"), "ERROR");
      EXPECT_EQ(Text(payload, "message"),
                "[move / move.attempt.1] execution_aborted: first collision");
      EXPECT_EQ(Member(payload, "journal_payload"), collision);
    }
  }
}

TEST(HeadlessApiCore,
     LatchedStopFaultDrivesExactAcknowledgeableAlarmUntilDurableClear) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.stop-fault";
  const auto terminal = StopTerminalProjectionPayload(
      source_epoch, "succeeded_escalated", "succeeded", true);
  const auto terminal_digest = content_identity::sha256_digest(
      content_identity::canonical_json(terminal));

  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 1U, "stop_terminal_projected", std::nullopt, std::nullopt,
      std::nullopt, 1100U, "2026-08-07T10:00:01.000000001Z", false, terminal,
      terminal_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 1U));

  const auto active_snapshot = core.AlarmSnapshot();
  const auto &active = OnlyAlarm(active_snapshot);
  EXPECT_EQ(Text(active, "definition_id"), "manyforge.motion.stop_fault");
  EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(active, "priority"), "high");
  EXPECT_TRUE(
      std::get<bool>(Member(active, "requires_motion_attention").storage()));
  EXPECT_EQ(Text(Member(active, "subject"), "subject_id"), kCellId);
  EXPECT_EQ(Text(Member(active, "correlations"), "latch_id"),
            "stop-fault.test.1");
  EXPECT_EQ(Text(Member(active, "correlations"), "stop_certificate_id"),
            "stop-certificate.test.1");
  EXPECT_EQ(
      Text(Member(Member(active, "details"), "fault_latch"), "reason_code"),
      "stop_escalated");

  const auto cleared = StopFaultClearProjectionPayload(source_epoch);
  const auto cleared_digest = content_identity::sha256_digest(
      content_identity::canonical_json(cleared));
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 2U, "stop_fault_cleared", "stop-fault-reset.test.1",
      std::nullopt, std::nullopt, 1200U, "2026-08-07T10:00:01.000000002Z",
      false, cleared, cleared_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 2U));

  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "definition_id"), "manyforge.motion.stop_fault");
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "record_kind"),
            "stop_fault_cleared");

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("stop_fault_cleared");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(Array(core.HistoryPage(query), "events").size(), 1U);
}

TEST(HeadlessApiCore,
     CurrentStopFaultSnapshotReannunciatesAfterOwnerRestartAndClearsExactly) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.stop-fault-snapshot";

  static_cast<void>(core.CompleteOperationEventSnapshot(
      source_epoch, 0U, CurrentStopFaultLatch()));
  const auto active_snapshot = core.AlarmSnapshot();
  const auto &active = OnlyAlarm(active_snapshot);
  EXPECT_EQ(Text(active, "definition_id"), "manyforge.motion.stop_fault");
  EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(active, "subject"), "subject_id"), kCellId);
  EXPECT_EQ(Text(Member(active, "correlations"), "latch_id"),
            "stop-fault.test.1");
  EXPECT_EQ(Text(Member(active, "details"), "record_kind"),
            "current_stop_fault_snapshot");
  const auto activation_id = Text(active, "activation_id");

  static_cast<void>(core.CompleteOperationEventSnapshot(
      source_epoch, 0U, CurrentStopFaultLatch()));
  const auto repeated_snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(repeated_snapshot), "activation_id"), activation_id);

  static_cast<void>(core.CompleteOperationEventSnapshot(
      source_epoch, 0U, CanonicalValue(nullptr)));
  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"),
            "stop_fault_context_retired");
}

TEST(HeadlessApiCore,
     PendingStopFaultIsVisibleBeforeItsCertificateAndNeverAutoAcknowledged) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.epoch.pending-stop";
  auto pending = JournalField(CurrentStopFaultLatch(), "stop_certificate",
                           CanonicalValue(nullptr));
  pending = JournalField(pending, "terminal_projection",
                      JournalField(Member(pending, "terminal_projection"), "state",
                                CanonicalValue("pending")));
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 0U, pending));
  auto snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
            "active_unacknowledged");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Member(Member(OnlyAlarm(snapshot), "correlations"), "stop_certificate_id")
          .storage()));
  const auto activation = Text(OnlyAlarm(snapshot), "activation_id");
  EXPECT_THROW(
      core.CompleteOperationEventSnapshot(
          epoch, 0U,
          JournalField(pending, "recovery_status", CanonicalValue("ready"))),
      OperatorAlarmError);

  const auto terminal = StopTerminalProjectionPayload(
      epoch, "succeeded_escalated", "succeeded", true);
  static_cast<void>(core.IngestOperationEvent(
      epoch, 1U, "stop_terminal_projected", std::nullopt, std::nullopt,
      std::nullopt, 1100U, "2026-08-07T10:00:01.000000001Z", false, terminal,
      content_identity::sha256_digest(
          content_identity::canonical_json(terminal))));
  static_cast<void>(
      core.CompleteOperationEventSnapshot(epoch, 1U, CurrentStopFaultLatch()));
  snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "activation_id"), activation);
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
            "active_unacknowledged");
  EXPECT_EQ(
      Text(Member(OnlyAlarm(snapshot), "correlations"), "stop_certificate_id"),
      "stop-certificate.test.1");
}

TEST(HeadlessApiCore,
     ClearingTheSelectedStopFaultDoesNotHideAnOlderUnresolvedFault) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.epoch.two-stop-faults";
  // The clear fixture names test.1: make it the newer selected fault.
  const auto older = JournalField(CurrentStopFaultLatch(), "latch_id",
                               CanonicalValue("stop-fault.older"));
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 0U, older));
  const auto initial = core.AlarmSnapshot();
  const auto activation = Text(OnlyAlarm(initial), "activation_id");
  static_cast<void>(
      core.CompleteOperationEventSnapshot(epoch, 0U, CurrentStopFaultLatch()));
  // An event page omitting the cached observation is not an authoritative null.
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 0U));
  auto snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
            "active_unacknowledged");
  const auto cleared = StopFaultClearProjectionPayload(epoch);
  static_cast<void>(core.IngestOperationEvent(
      epoch, 1U, "stop_fault_cleared", "stop-fault-reset.test.1", std::nullopt,
      std::nullopt, 1200U, "2026-08-07T10:00:01.000000002Z", false, cleared,
      content_identity::sha256_digest(
          content_identity::canonical_json(cleared))));
  snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
            "active_unacknowledged");
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 1U, older));
  snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "activation_id"), activation);
  EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
            "active_unacknowledged");
  EXPECT_EQ(Text(Member(OnlyAlarm(snapshot), "correlations"), "latch_id"),
            "stop-fault.older");
}

TEST(HeadlessApiCore,
     AcceptedResetClearsMatchingOperationFaultAndConflictsFailClosed) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.reset";
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")},
  });
  const auto failure =
      OperationJournalPayload(source_epoch, "state_observed",
                              "intervention_required", error, std::nullopt,
                              resource);
  const auto failure_json = content_identity::canonical_json(failure);
  const auto failure_digest = content_identity::sha256_digest(failure_json);
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 4U, "state_observed", std::nullopt, "operation.test.1", 1U,
      200U, "2026-08-07T10:00:00.000000004Z", false, failure, failure_digest));

  auto conflicting = failure;
  const auto conflicting_digest = content_identity::sha256_digest(
      content_identity::canonical_json(conflicting));
  try {
    static_cast<void>(core.IngestOperationEvent(
        source_epoch, 4U, "state_observed", std::nullopt, "operation.test.1",
        1U, 201U, "2026-08-07T10:00:00.000000005Z", false,
        std::move(conflicting), conflicting_digest));
    FAIL() << "one native sequence accepted conflicting event content";
  } catch (const OperatorAlarmError &error_value) {
    EXPECT_EQ(error_value.code(), "headless_operation_event_identity_conflict");
  }

  const auto reset =
      OperationJournalPayload(source_epoch, "control_accepted",
                              "intervention_required", error, "reset",
                              resource);
  const auto reset_json = content_identity::canonical_json(reset);
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 5U, "control_accepted", "request.reset.1",
      "operation.test.1", 1U, 202U, "2026-08-07T10:00:00.000000006Z", true,
      reset, content_identity::sha256_digest(reset_json)));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 5U));
  const auto alarms = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(alarms);
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
}

TEST(HeadlessApiCore,
     AlarmResourceScopeFollowsEachActiveConfigurationWithoutSilencingTheFeed) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto single = ResourceIds(*configurations.single);
  const auto dual = ResourceIds(*configurations.dual);
  ASSERT_FALSE(single.empty());
  const auto kept = single.front();
  ASSERT_TRUE(std::find(dual.begin(), dual.end(), kept) != dual.end());
  const auto added = std::find_if(dual.begin(), dual.end(), [&](const auto &id) {
    return std::find(single.begin(), single.end(), id) == single.end();
  });
  ASSERT_NE(added, dual.end());

  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.epoch.registry";
  std::uint64_t sequence = 0U;
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")},
  });
  const auto ingest = [&](const std::string &operation,
                          const std::string &resource,
                          const std::string &state = "failed") {
    const auto payload =
        OperationJournalPayload(epoch, "state_observed", state, error,
                                std::nullopt, resource, operation);
    ++sequence;
    const auto ingested = core.IngestOperationEvent(
        epoch, sequence, "state_observed", std::nullopt, operation, 1U,
        static_cast<std::int64_t>(100U + sequence),
        "2026-10-02T10:00:00.000000001Z", false, payload,
        content_identity::sha256_digest(
            content_identity::canonical_json(payload)));
    EXPECT_EQ(Text(ingested, "status"), "ingested");
    static_cast<void>(core.CompleteOperationEventSnapshot(epoch, sequence));
  };
  const auto motion_reconciled = [&]() {
    const auto snapshot = core.AlarmSnapshot();
    for (const auto &domain : Array(snapshot, "domains")) {
      if (Text(domain, "domain") == "motion") {
        return std::get<bool>(Member(domain, "reconciled").storage());
      }
    }
    return false;
  };
  const auto active_subjects = [&]() {
    std::set<std::string> subjects;
    const auto snapshot = core.AlarmSnapshot();
    for (const auto &alarm : Array(snapshot, "alarms")) {
      if (Text(alarm, "lifecycle_state") == "active_unacknowledged" &&
          Text(alarm, "definition_id") != kAlarmScopeDefinition) {
        subjects.insert(Text(Member(alarm, "subject"), "subject_id"));
      }
    }
    return subjects;
  };

  // Empty startup: no active configuration names any resource. The fault is
  // refused by name while the operation feed itself stays current, and the
  // cell-scoped alarm-scope condition annunciates the retained refusal.
  ingest("operation.startup", kept);
  EXPECT_TRUE(active_subjects().empty());
  EXPECT_EQ(RefusedSubjects(core.AlarmSnapshot()),
            (std::set<std::string>{"operation.startup"}));
  EXPECT_TRUE(motion_reconciled());

  // Apply the single-arm configuration: its resources are admitted at once,
  // before any other reader refreshed the projection, and the still-current
  // startup fault is reconsidered and admitted.
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  ingest("operation.kept", kept);
  ingest("operation.added.early", *added);
  EXPECT_EQ(active_subjects(),
            (std::set<std::string>{"operation.kept", "operation.startup"}));
  EXPECT_EQ(RefusedSubjects(core.AlarmSnapshot()),
            (std::set<std::string>{"operation.added.early"}));

  // Replace it with the dual configuration: the new resources are admitted,
  // including the earlier refused, still-current fault on them.
  PublishActiveConfiguration(directory, *configurations.dual, 2U);
  ingest("operation.added", *added);
  const std::set<std::string> admitted{"operation.added",
                                       "operation.added.early",
                                       "operation.kept", "operation.startup"};
  EXPECT_EQ(active_subjects(), admitted);
  {
    // No refusal remains retained: the alarm-scope condition returns and,
    // needing no acknowledgement, leaves the live list at once.
    const auto snapshot = core.AlarmSnapshot();
    EXPECT_EQ(ScopeAlarm(snapshot), nullptr);
    EXPECT_TRUE(RefusedSubjects(snapshot).empty());
  }

  // Replace it back: the removed resource's retained alarm stays current and
  // still returns to normal, while a new activation on it is stale. An
  // invalid cell scope is refused without silencing the valid alarms.
  PublishActiveConfiguration(directory, *configurations.single, 3U);
  ingest("operation.added.stale", *added);
  auto latch = StopTerminalProjectionPayload(epoch, "succeeded_escalated",
                                             "succeeded", true);
  latch = JournalField(latch, "fault_latch",
                    JournalField(Member(latch, "fault_latch"), "cell_id",
                              CanonicalValue("another_cell")));
  ++sequence;
  static_cast<void>(core.IngestOperationEvent(
      epoch, sequence, "stop_terminal_projected", std::nullopt, std::nullopt,
      std::nullopt, 1100U, "2026-10-02T10:00:01.000000001Z", false, latch,
      content_identity::sha256_digest(content_identity::canonical_json(latch))));
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, sequence));
  EXPECT_EQ(active_subjects(), admitted);
  EXPECT_EQ(RefusedSubjects(core.AlarmSnapshot()),
            (std::set<std::string>{"another_cell", "operation.added.stale"}));
  EXPECT_TRUE(motion_reconciled());
  ingest("operation.added", *added, "completed");
  EXPECT_EQ(active_subjects(),
            (std::set<std::string>{"operation.added.early", "operation.kept",
                                   "operation.startup"}));
  const auto snapshot = core.AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 5U);
  ASSERT_NE(ScopeAlarm(snapshot), nullptr);
  // One activation per refusal episode (startup, early, stale); a further
  // refusal within an episode updates the standing alarm instead.
  EXPECT_EQ(Unsigned(*ScopeAlarm(snapshot), "activation_count"), 3U);
  EXPECT_EQ(Unsigned(*ScopeAlarm(snapshot), "occurrence_count"), 2U);
  EXPECT_EQ(Text(*ScopeAlarm(snapshot), "lifecycle_state"),
            "active_acknowledged");
  EXPECT_TRUE(motion_reconciled());

  const auto totals = Member(core.AlarmMetrics(), "totals");
  EXPECT_EQ(Unsigned(totals, "scope_rejections"), 4U);
  HistorianQuery query;
  query.components.insert("manyforge.alarm");
  query.event_types.insert("alarm_condition_rejected");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").size() < 4U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  std::vector<std::string> codes;
  const auto page = core.HistoryPage(query);
  for (const auto &stored : Array(page, "events")) {
    codes.push_back(
        Text(Member(Member(stored, "event"), "payload"), "code"));
  }
  std::sort(codes.begin(), codes.end());
  EXPECT_EQ(codes, (std::vector<std::string>{
                       "operator_alarm_scope_inactive",
                       "operator_alarm_scope_inactive",
                       "operator_alarm_scope_inactive",
                       "operator_alarm_scope_invalid",
                   }));
}

TEST(HeadlessApiCore,
     FaultRefusedBeforeAnyConfigurationIsAdmittedOnceThePublicationAppears) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "execution-host.epoch.early";
  const auto failure = OperationJournalPayload(
      epoch, "state_observed", "failed",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{})},
          {"message", CanonicalValue("failed")},
      }),
      std::nullopt, resource, "operation.early");
  static_cast<void>(core.IngestOperationEvent(
      epoch, 1U, "state_observed", std::nullopt, "operation.early", 1U, 100U,
      "2026-10-02T10:00:00.000000001Z", false, failure,
      content_identity::sha256_digest(content_identity::canonical_json(failure))));
  // The unchanged fault is completed repeatedly while no configuration is
  // published: it stays refused without being resubmitted or re-recorded,
  // and its one alarm-scope alarm is neither repeated nor reactivated.
  for (int repeat = 0; repeat < 3; ++repeat) {
    static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 1U));
    const auto refused = core.AlarmSnapshot();
    ASSERT_EQ(Array(refused, "alarms").size(), 1U);
    const auto &scope = OnlyAlarm(refused);
    EXPECT_EQ(Text(scope, "definition_id"), kAlarmScopeDefinition);
    // A condition interlock: no acknowledgement is owed, yet it is the one
    // cell-global motion-attention member while active.
    EXPECT_EQ(Text(scope, "lifecycle_state"), "active_acknowledged");
    EXPECT_FALSE(
        std::get<bool>(Member(scope, "acknowledgement_required").storage()));
    EXPECT_EQ(Unsigned(refused, "unacknowledged_count"), 0U);
    EXPECT_EQ(Unsigned(refused, "motion_attention_count"), 1U);
    const auto &gating = Array(refused, "motion_attention_membership");
    ASSERT_EQ(gating.size(), 1U);
    EXPECT_EQ(Text(gating.front(), "condition_id"),
              Text(scope, "condition_id"));
    EXPECT_EQ(Text(Member(scope, "subject"), "subject_kind"), "cell");
    EXPECT_TRUE(Array(Member(scope, "subject"), "resource_ids").empty());
    EXPECT_EQ(Unsigned(scope, "activation_count"), 1U);
    EXPECT_EQ(Unsigned(scope, "occurrence_count"), 1U);
    EXPECT_EQ(Unsigned(Member(scope, "details"), "refused_condition_count"),
              1U);
    const auto &named = Array(Member(scope, "details"), "refused_conditions");
    ASSERT_EQ(named.size(), 1U);
    EXPECT_EQ(Text(named.front(), "definition_id"),
              "manyforge.motion.execution_fault");
    EXPECT_EQ(Text(Member(named.front(), "subject"), "subject_id"),
              "operation.early");
    EXPECT_EQ(Array(Member(named.front(), "subject"), "resource_ids"),
              (CanonicalValue::Array{CanonicalValue(resource)}));
  }

  // Manager publishes the configuration; the next observation of the
  // projection reconsiders the still-current fault once and admits it, and
  // the alarm-scope condition returns to normal, leaving the live list and
  // the motion gate without an acknowledgement.
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  const auto admitted = core.AlarmSnapshot();
  ASSERT_EQ(Array(admitted, "alarms").size(), 1U);
  const CanonicalValue *fault = nullptr;
  for (const auto &alarm : Array(admitted, "alarms")) {
    if (Text(alarm, "definition_id") == "manyforge.motion.execution_fault") {
      fault = &alarm;
    }
  }
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(Text(*fault, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*fault, "subject"), "subject_id"), "operation.early");
  EXPECT_EQ(ScopeAlarm(admitted), nullptr);
  EXPECT_EQ(Unsigned(admitted, "motion_attention_count"), 1U);
  const auto &gating = Array(admitted, "motion_attention_membership");
  ASSERT_EQ(gating.size(), 1U);
  EXPECT_EQ(Text(gating.front(), "condition_id"),
            Text(*fault, "condition_id"));
  static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 1U));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);

  // Its normal terminal state returns it like any admitted fault.
  const auto completed = OperationJournalPayload(
      epoch, "state_observed", "completed", CanonicalValue(nullptr),
      std::nullopt, resource, "operation.early");
  static_cast<void>(core.IngestOperationEvent(
      epoch, 2U, "state_observed", std::nullopt, "operation.early", 1U, 101U,
      "2026-10-02T10:00:00.000000002Z", false, completed,
      content_identity::sha256_digest(
          content_identity::canonical_json(completed))));
  const auto returned = core.AlarmSnapshot();
  for (const auto &alarm : Array(returned, "alarms")) {
    EXPECT_EQ(Text(alarm, "lifecycle_state"), "returned_unacknowledged");
  }

  EXPECT_EQ(Unsigned(Member(core.AlarmMetrics(), "totals"), "scope_rejections"),
            1U);
  HistorianQuery query;
  query.components.insert("manyforge.alarm");
  query.event_types.insert("alarm_condition_rejected");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(100ms);
  EXPECT_EQ(Array(core.HistoryPage(query), "events").size(), 1U);
}

/// The CCLC component snapshot members the lifecycle-cause adapters read.
CanonicalValue ComponentConditionSnapshot(
    const std::string &component, const std::string &resource,
    const std::string &achieved, const std::string &phase,
    const std::string &cause, const std::string &requested = "active",
    const double target_generation = 1.0) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_lifecycle_state", CanonicalValue(achieved)},
      {"controllable_component_id", CanonicalValue(component)},
      {"domains",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
           {"control_domain_id", CanonicalValue("domain.arm")},
           {"kind", CanonicalValue("controller")},
           {"resources",
            CanonicalValue(CanonicalValue::Array{CanonicalValue(resource)})},
       })})},
      {"graph_revision", CanonicalValue("graph.test")},
      {"lifecycle_progress",
       CanonicalValue(CanonicalValue::Object{
           {"blocking_cause_class", CanonicalValue(cause)},
           {"blocking_reason",
            cause == "none" ? CanonicalValue(nullptr)
                            : CanonicalValue("reason." + cause)},
           {"phase", CanonicalValue(phase)},
       })},
      {"lifecycle_target",
       CanonicalValue(CanonicalValue::Object{
           {"origin", CanonicalValue("startup")},
           {"requested_state", CanonicalValue(requested)},
           {"target_generation", CanonicalValue(target_generation)},
       })},
      {"retained_bundle_id", CanonicalValue("bundle.test")},
  });
}

/// `snapshot` as CCLC captures it under `bundle` (retained bundle id and
/// graph revision).
CanonicalValue InConfiguration(const CanonicalValue &snapshot,
                               const deployment::RetainedBundle &bundle) {
  auto row = snapshot.as_object();
  for (auto &[name, value] : row) {
    if (name == "graph_revision") {
      value = CanonicalValue(bundle.value().cell_graph.graph_revision.value());
    } else if (name == "retained_bundle_id") {
      value = CanonicalValue(bundle.retained_bundle_id().value());
    }
  }
  return CanonicalValue(std::move(row));
}

/// The component-lifecycle fixture: the single-arm configuration is active
/// and its controllable component and first resource are named.
struct ComponentConditionFixture final {
  TemporaryDirectory directory;
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  std::string component;
  std::string resource;
  std::unique_ptr<HeadlessApiCore> core;

  ComponentConditionFixture() {
    const auto configurations = LoadRegistryConfigurations();
    bundle = configurations.single;
    const auto &components = bundle->value().cell_graph.controllable_components;
    if (components.empty()) {
      throw std::runtime_error("registry fixture has no component");
    }
    component = components.front().controllable_component_id.value();
    resource = ResourceIds(*bundle).front();
    PublishActiveConfiguration(directory, *bundle, 1U);
    core = std::make_unique<HeadlessApiCore>(
        RegistryConfig(directory, "api.epoch.1"), Clocks());
    // Observing the projection establishes the alarm resource registry.
    static_cast<void>(core->AlarmSnapshot());
  }

  /// Observe `snapshot` as captured under the active configuration.
  CanonicalValue Observe(const CanonicalValue &snapshot,
                         const std::string &epoch = "cclc.epoch.1") {
    return core->ObserveComponentLifecycleConditions(
        epoch,
        CanonicalValue(CanonicalValue::Array{InConfiguration(snapshot, *bundle)}),
        {});
  }
};

/// Sources of the `runtime_health` domain (which holds the component
/// lifecycle source) whose current quality is unknown.
std::uint64_t UnknownRuntimeHealthSources(HeadlessApiCore &core) {
  const auto snapshot = core.AlarmSnapshot();
  for (const auto &domain : Array(snapshot, "domains")) {
    if (Text(domain, "domain") == "runtime_health") {
      return Unsigned(domain, "unknown_source_count");
    }
  }
  throw std::runtime_error("runtime_health domain is missing");
}

const CanonicalValue *AlarmFor(const CanonicalValue &snapshot,
                               const std::string_view definition) {
  const CanonicalValue *found = nullptr;
  for (const auto &alarm : Array(snapshot, "alarms")) {
    if (Text(alarm, "definition_id") == definition) {
      if (found != nullptr) {
        throw std::runtime_error("expected at most one alarm per definition");
      }
      found = &alarm;
    }
  }
  return found;
}

constexpr std::string_view kComponentLifecycleFault =
    "manyforge.runtime.component_lifecycle_fault";

TEST(HeadlessApiCore,
     SafetyStopHoldsComponentLifecycleFaultAcrossCommunicationLossUntilFreshClear) {
  ComponentConditionFixture fixture;
  const auto scope_value = fixture.core->ComponentLifecycleConditionScope();
  const auto &scope = Array(scope_value, "component_ids");
  ASSERT_EQ(scope.size(), 1U);
  EXPECT_EQ(std::get<std::string>(scope.front().storage()), fixture.component);

  EXPECT_EQ(Text(fixture.Observe(ComponentConditionSnapshot(
                     fixture.component, fixture.resource, "active",
                     "satisfied", "none")),
                 "status"),
            "observed");
  EXPECT_TRUE(Array(fixture.core->AlarmSnapshot(), "alarms").empty());

  // An emergency stop after the component reached its target.
  const auto stopped = ComponentConditionSnapshot(
      fixture.component, fixture.resource, "active", "satisfied",
      "emergency_stop");
  static_cast<void>(fixture.Observe(stopped));
  auto snapshot = fixture.core->AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 1U);
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*alarm, "subject"), "subject_kind"), "component");
  EXPECT_EQ(Text(Member(*alarm, "subject"), "subject_id"), fixture.component);
  EXPECT_EQ(Array(Member(*alarm, "subject"), "resource_ids"),
            (CanonicalValue::Array{CanonicalValue(fixture.resource)}));
  EXPECT_EQ(Text(Member(*alarm, "details"), "blocking_cause_class"),
            "emergency_stop");
  const auto activation = Text(*alarm, "activation_id");
  const auto revision = Unsigned(*alarm, "record_revision");

  // An unchanged observation submits nothing.
  static_cast<void>(fixture.Observe(stopped));
  snapshot = fixture.core->AlarmSnapshot();
  EXPECT_EQ(Unsigned(*AlarmFor(snapshot, kComponentLifecycleFault),
                     "record_revision"),
            revision);

  // CCLC unreachable, then the component unread: the condition stays.
  EXPECT_EQ(Text(fixture.core->MarkComponentLifecycleSourceUnknown(), "status"),
            "marked_unknown");
  snapshot = fixture.core->AlarmSnapshot();
  EXPECT_EQ(Text(*AlarmFor(snapshot, kComponentLifecycleFault),
                 "lifecycle_state"),
            "active_unacknowledged");
  static_cast<void>(fixture.core->ObserveComponentLifecycleConditions(
      "cclc.epoch.1", CanonicalValue(CanonicalValue::Array{}),
      {fixture.component}));
  snapshot = fixture.core->AlarmSnapshot();
  EXPECT_EQ(Text(*AlarmFor(snapshot, kComponentLifecycleFault),
                 "lifecycle_state"),
            "active_unacknowledged");
  // CCLC retains the class across controller communication loss as well;
  // availability alone is never a lifecycle fault.
  static_cast<void>(fixture.Observe(stopped));
  EXPECT_EQ(Text(*AlarmFor(fixture.core->AlarmSnapshot(),
                           kComponentLifecycleFault),
                 "activation_id"),
            activation);

  // Fresh status clears the cause: the alarm returns to normal and still
  // awaits acknowledgement (automatic recovery acknowledges nothing).
  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "active", "satisfied", "none")));
  snapshot = fixture.core->AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 1U);
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(*alarm, "activation_id"), activation);
}

TEST(HeadlessApiCore,
     FaultAndUnknownCausesAreLifecycleFaultsButConfigurationIsNot) {
  ComponentConditionFixture fixture;
  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "declared", "blocked",
      "configuration")));
  EXPECT_TRUE(Array(fixture.core->AlarmSnapshot(), "alarms").empty());
  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "disabled", "satisfied", "fault",
      "disabled")));
  auto snapshot = fixture.core->AlarmSnapshot();
  ASSERT_NE(AlarmFor(snapshot, kComponentLifecycleFault), nullptr);
  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "declared", "blocked", "unknown")));
  snapshot = fixture.core->AlarmSnapshot();
  EXPECT_EQ(Text(*AlarmFor(snapshot, kComponentLifecycleFault),
                 "lifecycle_state"),
            "active_unacknowledged");
  EXPECT_EQ(Text(Member(*AlarmFor(snapshot, kComponentLifecycleFault),
                        "details"),
                 "blocking_cause_class"),
            "unknown");
}

/// `snapshot` with an open incident in its lifecycle progress.
CanonicalValue WithIncident(const CanonicalValue &snapshot,
                            const std::string &incident_id,
                            const std::string &cause) {
  auto row = snapshot.as_object();
  for (auto &[name, value] : row) {
    if (name != "lifecycle_progress") {
      continue;
    }
    auto progress = value.as_object();
    progress.emplace_back(
        "incident",
        CanonicalValue(CanonicalValue::Object{
            {"acknowledged", CanonicalValue(false)},
            {"cause_class", CanonicalValue(cause)},
            {"detected_at_ns", CanonicalValue(1000.0)},
            {"incident_id", CanonicalValue(incident_id)},
            {"summary", CanonicalValue("Controller lost")},
            {"vendor_codes",
             CanonicalValue(CanonicalValue::Array{CanonicalValue("C2")})},
        }));
    value = CanonicalValue(std::move(progress));
  }
  return CanonicalValue(std::move(row));
}

TEST(HeadlessApiCore,
     OpenIncidentHoldsComponentLifecycleFaultUntilItsAcknowledgementClearsIt) {
  ComponentConditionFixture fixture;
  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "active", "satisfied", "none")));
  const auto lost = WithIncident(
      ComponentConditionSnapshot(fixture.component, fixture.resource, "active",
                                 "blocked", "provider_lost"),
      "incident.1", "provider_lost");
  static_cast<void>(fixture.Observe(lost));
  auto snapshot = fixture.core->AlarmSnapshot();
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*alarm, "details"), "incident_id"), "incident.1");
  EXPECT_EQ(Text(Member(*alarm, "details"), "incident_cause_class"),
            "provider_lost");
  EXPECT_EQ(Array(Member(*alarm, "details"), "incident_vendor_codes"),
            (CanonicalValue::Array{CanonicalValue("C2")}));
  const auto activation = Text(*alarm, "activation_id");

  // Readiness returning does not end the incident: only its acknowledgement
  // (the incident gone from a fresh snapshot) does.
  static_cast<void>(fixture.Observe(WithIncident(
      ComponentConditionSnapshot(fixture.component, fixture.resource, "active",
                                 "blocked", "none"),
      "incident.1", "provider_lost")));
  snapshot = fixture.core->AlarmSnapshot();
  EXPECT_EQ(Text(*AlarmFor(snapshot, kComponentLifecycleFault),
                 "lifecycle_state"),
            "active_unacknowledged");

  static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
      fixture.component, fixture.resource, "active", "satisfied", "none")));
  snapshot = fixture.core->AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  // The alarm itself still follows the catalog's acknowledgement rule.
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(*alarm, "activation_id"), activation);
  EXPECT_EQ(Text(Member(*alarm, "details"), "incident_id"), "incident.1");
}

TEST(HeadlessApiCore, EveryIncidentCauseClassIsAComponentLifecycleFault) {
  for (const std::string cause : {"collision", "external_control",
                                  "provider_lost", "emergency_stop",
                                  "protective_stop", "safeguard_stop"}) {
    ComponentConditionFixture fixture;
    static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
        fixture.component, fixture.resource, "active", "blocked", cause)));
    EXPECT_NE(AlarmFor(fixture.core->AlarmSnapshot(), kComponentLifecycleFault),
              nullptr)
        << cause;
  }
  ComponentConditionFixture fixture;
  EXPECT_THROW(static_cast<void>(fixture.Observe(WithIncident(
                   ComponentConditionSnapshot(fixture.component,
                                              fixture.resource, "active",
                                              "blocked", "availability"),
                   "incident.1", "availability"))),
               OperatorAlarmError);
  EXPECT_TRUE(Array(fixture.core->AlarmSnapshot(), "alarms").empty());
}

TEST(HeadlessApiCore, ComponentLifecycleObservationMustNameTheCurrentScope) {
  ComponentConditionFixture fixture;
  EXPECT_EQ(Text(fixture.core->MarkComponentLifecycleSourceUnknown(), "status"),
            "not_registered");
  EXPECT_EQ(Text(fixture.core->ObserveComponentLifecycleConditions(
                     "cclc.epoch.1", CanonicalValue(CanonicalValue::Array{}),
                     {}),
                 "status"),
            "scope_changed");
  EXPECT_EQ(Text(fixture.Observe(ComponentConditionSnapshot(
                     "component.elsewhere", fixture.resource, "active",
                     "satisfied", "emergency_stop")),
                 "status"),
            "scope_changed");
  const auto snapshot = fixture.core->AlarmSnapshot();
  EXPECT_TRUE(Array(snapshot, "alarms").empty());
  EXPECT_THROW(
      static_cast<void>(fixture.Observe(ComponentConditionSnapshot(
          fixture.component, fixture.resource, "active", "satisfied",
          "intervention"))),
      OperatorAlarmError);
}

TEST(HeadlessApiCore,
     ComponentLifecycleSourceReconcilesAfterAnApplyRemovesAComponent) {
  // Regression: an Apply that retires a component replaces the CCLC owner,
  // which is briefly unreachable. The first observation of the new scope
  // under the new owner epoch must reconcile the source (the owner admits
  // motion again) and return the removed component's raised condition.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto component_ids = [](const deployment::RetainedBundle &bundle) {
    std::set<std::string> ids;
    for (const auto &component :
         bundle.value().cell_graph.controllable_components) {
      ids.insert(component.controllable_component_id.value());
    }
    return ids;
  };
  const auto single_components = component_ids(*configurations.single);
  const auto dual_components = component_ids(*configurations.dual);
  std::vector<std::string> removed_components;
  std::set_difference(dual_components.begin(), dual_components.end(),
                      single_components.begin(), single_components.end(),
                      std::back_inserter(removed_components));
  ASSERT_EQ(single_components.size(), 1U);
  ASSERT_EQ(removed_components.size(), 1U);
  const auto kept = *single_components.begin();
  const auto removed = removed_components.front();
  const auto single_resources = ResourceIds(*configurations.single);
  std::vector<std::string> removed_resources;
  for (const auto &resource : ResourceIds(*configurations.dual)) {
    if (std::find(single_resources.begin(), single_resources.end(),
                  resource) == single_resources.end()) {
      removed_resources.push_back(resource);
    }
  }
  ASSERT_FALSE(removed_resources.empty());

  PublishActiveConfiguration(directory, *configurations.dual, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.AlarmSnapshot());
  const auto unknown_runtime_sources = [&]() {
    return UnknownRuntimeHealthSources(core);
  };
  const auto baseline = unknown_runtime_sources();

  EXPECT_EQ(Text(core.ObserveComponentLifecycleConditions(
                     "cclc.epoch.1",
                     CanonicalValue(CanonicalValue::Array{
                         InConfiguration(
                             ComponentConditionSnapshot(
                                 kept, single_resources.front(), "active",
                                 "satisfied", "none"),
                             *configurations.dual),
                         InConfiguration(
                             ComponentConditionSnapshot(
                                 removed, removed_resources.front(), "active",
                                 "satisfied", "emergency_stop"),
                             *configurations.dual),
                     }),
                     {}),
                 "status"),
            "observed");
  ASSERT_NE(AlarmFor(core.AlarmSnapshot(), kComponentLifecycleFault), nullptr);
  EXPECT_EQ(unknown_runtime_sources(), baseline);

  // Apply: the CCLC owner is replaced and briefly unreachable.
  EXPECT_EQ(Text(core.MarkComponentLifecycleSourceUnknown(), "status"),
            "marked_unknown");
  EXPECT_EQ(unknown_runtime_sources(), baseline + 1U);
  PublishActiveConfiguration(directory, *configurations.single, 2U);
  static_cast<void>(core.AlarmSnapshot());
  EXPECT_EQ(Array(core.ComponentLifecycleConditionScope(), "component_ids"),
            (CanonicalValue::Array{CanonicalValue(kept)}));

  EXPECT_EQ(Text(core.ObserveComponentLifecycleConditions(
                     "cclc.epoch.2",
                     CanonicalValue(CanonicalValue::Array{
                         InConfiguration(
                             ComponentConditionSnapshot(
                                 kept, single_resources.front(), "active",
                                 "satisfied", "none"),
                             *configurations.single),
                     }),
                     {}),
                 "status"),
            "observed");
  EXPECT_EQ(unknown_runtime_sources(), baseline);
  const auto snapshot = core.AlarmSnapshot();
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(Member(*alarm, "subject"), "subject_id"), removed);
  EXPECT_FALSE(std::get<bool>(Member(*alarm, "active").storage()));
}

TEST(HeadlessApiCore,
     LifecycleObservationCapturedBeforeApplyRaisesKeepsAndClearsNothing) {
  // Regression: matching component IDs alone admitted a CCLC snapshot taken
  // under the configuration an Apply superseded. Each row is now fenced to
  // the retained bundle and graph revision it was captured under; until a
  // row of the active configuration arrives, the conditions held from the
  // superseded one stay at unknown quality.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &single = *configurations.single;
  const auto &dual = *configurations.dual;
  const auto kept =
      single.value().cell_graph.controllable_components.front()
          .controllable_component_id.value();
  std::string added;
  for (const auto &component : dual.value().cell_graph.controllable_components) {
    if (component.controllable_component_id.value() != kept) {
      added = component.controllable_component_id.value();
    }
  }
  ASSERT_FALSE(added.empty());
  // The kept component's condition keeps one subject across both
  // configurations, so only the fence decides what is submitted.
  const auto resource = ResourceIds(single).front();
  const auto row = [&](const std::string &component, const std::string &cause,
                       const deployment::RetainedBundle &captured_under) {
    return InConfiguration(ComponentConditionSnapshot(component, resource,
                                                      "active", "satisfied",
                                                      cause),
                           captured_under);
  };
  const auto observe = [&](HeadlessApiCore &core,
                           CanonicalValue::Array rows,
                           std::vector<std::string> unavailable = {}) {
    return Text(core.ObserveComponentLifecycleConditions(
                    "cclc.epoch.1", CanonicalValue(std::move(rows)),
                    std::move(unavailable)),
                "status");
  };

  PublishActiveConfiguration(directory, dual, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.AlarmSnapshot());
  EXPECT_FALSE(std::get<bool>(
      Member(core.ComponentLifecycleConditionScope(), "source_registered")
          .storage()));
  ASSERT_EQ(observe(core, {row(kept, "none", dual), row(added, "none", dual)}),
            "observed");
  EXPECT_TRUE(std::get<bool>(
      Member(core.ComponentLifecycleConditionScope(), "source_registered")
          .storage()));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  const auto baseline = UnknownRuntimeHealthSources(core);

  // Apply to the single-arm configuration: the scope still names `kept`.
  PublishActiveConfiguration(directory, single, 2U);
  static_cast<void>(core.AlarmSnapshot());
  ASSERT_EQ(Array(core.ComponentLifecycleConditionScope(), "component_ids"),
            (CanonicalValue::Array{CanonicalValue(kept)}));
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);

  // A stale capture raises nothing.
  EXPECT_EQ(observe(core, {row(kept, "emergency_stop", dual)}),
            "scope_changed");
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);
  // An unread component does not reconcile the source either.
  EXPECT_EQ(observe(core, {}, {kept}), "incomplete");
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);

  // A capture under the active configuration raises and reconciles.
  EXPECT_EQ(observe(core, {row(kept, "emergency_stop", single)}), "observed");
  auto snapshot = core.AlarmSnapshot();
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  const auto activation = Text(*alarm, "activation_id");
  const auto revision = Unsigned(*alarm, "record_revision");
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline);

  // Apply back to the dual configuration with the same `kept` component.
  PublishActiveConfiguration(directory, dual, 3U);
  static_cast<void>(core.AlarmSnapshot());
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);

  // A stale capture neither clears nor keeps the raised condition: the
  // record is unchanged and its source stays at unknown quality.
  EXPECT_EQ(observe(core, {row(kept, "none", single), row(added, "none", single)}),
            "scope_changed");
  EXPECT_EQ(observe(core, {row(kept, "emergency_stop", single),
                           row(added, "none", single)}),
            "scope_changed");
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Unsigned(*alarm, "record_revision"), revision);
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);

  // Fresh normal status of the active configuration returns it.
  EXPECT_EQ(observe(core, {row(kept, "none", dual), row(added, "none", dual)}),
            "observed");
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(*alarm, "activation_id"), activation);
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline);
}

TEST(HeadlessApiCore,
     ConfigurationChangeReportsAPendingLifecycleObservationUntilOneIsAccepted) {
  // Spec 530 §7.1: an Apply is respected without polling. The registry swap
  // that marks the lifecycle source unknown is reported through the refresh
  // the event pump offers each iteration, so the pump reads the new
  // configuration at once; only a complete observation of the active
  // configuration clears it.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &single = *configurations.single;
  const auto &dual = *configurations.dual;
  const auto kept =
      single.value().cell_graph.controllable_components.front()
          .controllable_component_id.value();
  std::string added;
  for (const auto &component : dual.value().cell_graph.controllable_components) {
    if (component.controllable_component_id.value() != kept) {
      added = component.controllable_component_id.value();
    }
  }
  ASSERT_FALSE(added.empty());
  const auto resource = ResourceIds(single).front();
  const auto row = [&](const std::string &component,
                       const deployment::RetainedBundle &captured_under) {
    return InConfiguration(ComponentConditionSnapshot(component, resource,
                                                      "active", "satisfied",
                                                      "none"),
                           captured_under);
  };
  const auto observe = [&](HeadlessApiCore &core, CanonicalValue::Array rows,
                           std::vector<std::string> unavailable = {}) {
    return Text(core.ObserveComponentLifecycleConditions(
                    "cclc.epoch.1", CanonicalValue(std::move(rows)),
                    std::move(unavailable)),
                "status");
  };
  const auto pending = [](HeadlessApiCore &core) {
    return std::get<bool>(Member(core.RefreshAlarmSources(),
                                 "component_lifecycle_observation_pending")
                              .storage());
  };

  PublishActiveConfiguration(directory, dual, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.AlarmSnapshot());
  // The first configuration names components the source has not observed.
  EXPECT_TRUE(pending(core));
  ASSERT_EQ(observe(core, {row(kept, dual), row(added, dual)}), "observed");
  EXPECT_FALSE(pending(core));
  const auto baseline = UnknownRuntimeHealthSources(core);

  // The registry swap marks the source unknown and the observation pending.
  PublishActiveConfiguration(directory, single, 2U);
  static_cast<void>(core.AlarmSnapshot());
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);
  EXPECT_TRUE(pending(core));
  // A row CCLC captured under the superseded configuration is no observation.
  EXPECT_EQ(observe(core, {row(kept, dual)}), "scope_changed");
  EXPECT_TRUE(pending(core));
  // A partial observation leaves quality unknown and is distinct.
  EXPECT_EQ(observe(core, {}, {kept}), "incomplete");
  EXPECT_TRUE(pending(core));
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);
  // A complete observation of the new configuration clears it.
  EXPECT_EQ(observe(core, {row(kept, single)}), "observed");
  EXPECT_FALSE(pending(core));
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline);

  // An unreachable CCLC is the same pending observation.
  EXPECT_EQ(Text(core.MarkComponentLifecycleSourceUnknown(), "status"),
            "marked_unknown");
  EXPECT_TRUE(pending(core));
  EXPECT_EQ(observe(core, {row(kept, single)}), "observed");
  EXPECT_FALSE(pending(core));
}

TEST(HeadlessApiCore,
     LifecycleObservationIsJudgedAgainstThePublicationNotTheCachedRegistry) {
  // Regression: the fence compared rows with the cached registry identity,
  // which only another reader's refresh updates. After an Apply that no
  // reader had refreshed yet, a row captured under the superseded
  // configuration still matched that cache and could raise or clear a
  // condition. The observation now rereads the current Manager publication
  // first; without a fresh one it is judged against nothing.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &single = *configurations.single;
  const auto &dual = *configurations.dual;
  const auto kept =
      single.value().cell_graph.controllable_components.front()
          .controllable_component_id.value();
  std::string added;
  for (const auto &component : dual.value().cell_graph.controllable_components) {
    if (component.controllable_component_id.value() != kept) {
      added = component.controllable_component_id.value();
    }
  }
  ASSERT_FALSE(added.empty());
  const auto resource = ResourceIds(single).front();
  const auto row = [&](const std::string &component, const std::string &cause,
                       const deployment::RetainedBundle &captured_under) {
    return InConfiguration(ComponentConditionSnapshot(component, resource,
                                                      "active", "satisfied",
                                                      cause),
                           captured_under);
  };
  const auto observe = [&](HeadlessApiCore &core, CanonicalValue::Array rows) {
    return Text(core.ObserveComponentLifecycleConditions(
                    "cclc.epoch.1", CanonicalValue(std::move(rows)), {}),
                "status");
  };

  PublishActiveConfiguration(directory, dual, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.AlarmSnapshot());
  ASSERT_EQ(observe(core, {row(kept, "none", dual), row(added, "none", dual)}),
            "observed");

  // Apply to the single-arm configuration; nothing has refreshed the
  // registry since. A capture of the superseded configuration raises nothing.
  PublishActiveConfiguration(directory, single, 2U);
  EXPECT_EQ(observe(core, {row(kept, "emergency_stop", dual),
                           row(added, "emergency_stop", dual)}),
            "scope_changed");
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  // A capture of the published configuration is accepted.
  EXPECT_EQ(observe(core, {row(kept, "emergency_stop", single)}), "observed");
  auto snapshot = core.AlarmSnapshot();
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  const auto revision = Unsigned(*alarm, "record_revision");

  // Apply back to the dual configuration, again unrefreshed: a capture of the
  // superseded single-arm configuration clears nothing.
  PublishActiveConfiguration(directory, dual, 3U);
  EXPECT_EQ(observe(core, {row(kept, "none", single)}), "scope_changed");
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Unsigned(*alarm, "record_revision"), revision);

  // Without a current publication there is no active configuration to judge
  // against, even for rows naming the last one.
  std::filesystem::remove(directory.publications() / "manager-source.json");
  EXPECT_EQ(observe(core, {row(kept, "none", dual), row(added, "none", dual)}),
            "configuration_unavailable");
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Unsigned(*alarm, "record_revision"), revision);

  PublishActiveConfiguration(directory, dual, 4U);
  EXPECT_EQ(observe(core, {row(kept, "none", dual), row(added, "none", dual)}),
            "observed");
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "returned_unacknowledged");
}

TEST(HeadlessApiCore,
     EmptyConfigurationReturnsRemovedLifecycleConditionsWithoutCCLC) {
  // Regression: an Apply to a configuration without controlled components
  // removes the CCLC service, so an empty scope observed through CCLC only
  // marked the source unknown and the removed components' conditions stayed
  // active. The empty configuration is observed by Headless itself, fenced
  // to the configuration Manager publishes now.
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &single = *configurations.single;
  const auto &empty = *configurations.empty;
  const auto component =
      single.value().cell_graph.controllable_components.front()
          .controllable_component_id.value();
  const auto resource = ResourceIds(single).front();
  const auto observe_empty = [](HeadlessApiCore &core,
                                const deployment::RetainedBundle &bundle) {
    return Text(core.ObserveEmptyComponentLifecycleConfiguration(
                    bundle.retained_bundle_id().value(),
                    bundle.value().cell_graph.graph_revision.value()),
                "status");
  };

  PublishActiveConfiguration(directory, single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.AlarmSnapshot());
  const auto scope = core.ComponentLifecycleConditionScope();
  EXPECT_EQ(Text(scope, "retained_bundle_id"),
            single.retained_bundle_id().value());
  // A configuration with components is never observed as empty.
  EXPECT_EQ(observe_empty(core, single), "scope_changed");
  ASSERT_EQ(Text(core.ObserveComponentLifecycleConditions(
                     "cclc.epoch.1",
                     CanonicalValue(CanonicalValue::Array{InConfiguration(
                         ComponentConditionSnapshot(component, resource,
                                                    "active", "satisfied",
                                                    "emergency_stop"),
                         single)}),
                     {}),
                 "status"),
            "observed");
  ASSERT_NE(AlarmFor(core.AlarmSnapshot(), kComponentLifecycleFault), nullptr);
  const auto baseline = UnknownRuntimeHealthSources(core);

  const auto pending = [](HeadlessApiCore &value) {
    return std::get<bool>(Member(value.RefreshAlarmSources(),
                                 "component_lifecycle_observation_pending")
                              .storage());
  };
  EXPECT_FALSE(pending(core));

  // Apply to the empty configuration: its CCLC owner is gone.
  PublishActiveConfiguration(directory, empty, 2U);
  static_cast<void>(core.AlarmSnapshot());
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);
  // The registered source must observe the empty scope too.
  EXPECT_TRUE(pending(core));
  const auto empty_scope = core.ComponentLifecycleConditionScope();
  EXPECT_TRUE(Array(empty_scope, "component_ids").empty());
  EXPECT_EQ(Text(empty_scope, "retained_bundle_id"),
            empty.retained_bundle_id().value());
  EXPECT_EQ(Text(empty_scope, "graph_revision"),
            empty.value().cell_graph.graph_revision.value());
  EXPECT_EQ(Text(core.MarkComponentLifecycleSourceUnknown(), "status"),
            "marked_unknown");
  // An observation fenced to the superseded configuration returns nothing.
  EXPECT_EQ(observe_empty(core, single), "scope_changed");
  auto snapshot = core.AlarmSnapshot();
  const auto *alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_TRUE(std::get<bool>(Member(*alarm, "active").storage()));
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline + 1U);

  EXPECT_TRUE(pending(core));
  EXPECT_EQ(observe_empty(core, empty), "observed");
  EXPECT_FALSE(pending(core));
  snapshot = core.AlarmSnapshot();
  alarm = AlarmFor(snapshot, kComponentLifecycleFault);
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(Member(*alarm, "subject"), "subject_id"), component);
  EXPECT_FALSE(std::get<bool>(Member(*alarm, "active").storage()));
  EXPECT_EQ(UnknownRuntimeHealthSources(core), baseline);

  // A source that was never registered has nothing to return.
  TemporaryDirectory unregistered_directory;
  PublishActiveConfiguration(unregistered_directory, empty, 1U);
  HeadlessApiCore unregistered(
      RegistryConfig(unregistered_directory, "api.epoch.2"), Clocks());
  // Nothing in scope and nothing registered: no observation is pending.
  EXPECT_FALSE(pending(unregistered));
  EXPECT_EQ(observe_empty(unregistered, empty), "empty");
  EXPECT_FALSE(pending(unregistered));
}

TEST(HeadlessApiCore, CCLCIncidentEventsAreRecordedOncePerIncidentAndKind) {
  TemporaryDirectory directory;
  const auto event = [](const std::string &kind, const double sequence) {
    core::ComponentLifecycleIncidentDTO incident{
        "incident.component.arm.5000", core::ControllableComponentId("arm"),
        core::LifecycleBlockingCauseClassDTO::kUnknown, {}, 0, {}, std::nullopt, {},
        core::ComponentIncidentContainmentStateDTO::kPending, std::nullopt, std::nullopt,
        false, std::nullopt, std::nullopt, std::nullopt, std::nullopt, {}};
    incident.cause_class = core::LifecycleBlockingCauseClassDTO::kEmergencyStop;
    incident.detected_at_ns = 5000;
    incident.summary = "Actual emergency-stop incident";
    incident.containment_state = core::ComponentIncidentContainmentStateDTO::kRequested;
    incident.containment = core::ComponentIncidentContainmentReceiptDTO{
        core::StopId("stop.1"), core::StopGeneration(1U), {}, std::nullopt};
    if (kind == "component_incident_acknowledged") {
      incident.acknowledged = true;
      incident.acknowledged_at_ns = 5001;
      incident.acknowledged_by = "operator";
      incident.acknowledgement_request_id = core::RequestId("ack.1");
    }
    if (!incident.locally_valid())
      throw std::runtime_error("typed component incident fixture is invalid");
    const auto policy = core::LifecycleIncidentRecoveryPolicyFor(
        core::DefaultLifecycleIncidentRecoveryPolicy(), incident.cause_class);
    const auto payload = content_identity::parse_canonical_json(
        core::ComponentIncidentEventPayload(incident, policy, kind));
    return CanonicalValue(CanonicalValue::Object{
        {"event_kind", CanonicalValue(kind)},
        {"event_sequence", CanonicalValue(sequence)},
        {"incident_id", CanonicalValue(incident.incident_id)},
        {"occurred_at_ns", CanonicalValue(5000.0)},
        {"payload", payload},
    });
  };
  HistorianQuery query;
  query.components.insert("manyforge.cclc");
  const auto durable = [&query](HeadlessApiCore &core, const std::size_t count) {
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    CanonicalValue page;
    do {
      page = core.HistoryPage(query);
      if (Array(page, "events").size() == count) break;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return page;
  };
  {
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    const auto opened =
        core.RecordCCLCIncidentEvent("cclc.epoch.1", event("component_incident", 1.0));
    EXPECT_EQ(Text(opened, "status"), "recorded");
    EXPECT_EQ(Text(opened, "schema_version"),
              "manyforge.cclc_incident_history_record.v2");
    // The same event again (a replayed cursor) is not enqueued twice.
    EXPECT_EQ(Text(core.RecordCCLCIncidentEvent("cclc.epoch.1",
                                                event("component_incident", 1.0)),
                   "status"),
              "duplicate");
    EXPECT_EQ(Text(core.RecordCCLCIncidentEvent(
                       "cclc.epoch.1", event("component_incident_acknowledged", 2.0)),
                   "status"),
              "recorded");
    const auto page = durable(core, 2U);
    ASSERT_EQ(Array(page, "events").size(), 2U);
    const auto &newest = Member(Array(page, "events").front(), "event");
    EXPECT_EQ(Text(newest, "event_type"), "component_incident_acknowledged");
    EXPECT_EQ(Text(newest, "severity"), "INFO");
    const auto &oldest = Member(Array(page, "events").back(), "event");
    EXPECT_EQ(Text(oldest, "event_type"), "component_incident");
    EXPECT_EQ(Text(oldest, "severity"), "ERROR");
    const auto &payload = Member(oldest, "payload");
    EXPECT_EQ(Text(payload, "schema_version"),
              "manyforge.cclc_incident_history_event.v2");
    EXPECT_EQ(Text(payload, "clock_owner"), "headless:api.epoch.1");
    EXPECT_EQ(Text(payload, "cclc_owner_epoch"), "cclc.epoch.1");
    EXPECT_EQ(Text(payload, "incident_id"), "incident.component.arm.5000");
    EXPECT_EQ(content_identity::canonical_json(Member(payload, "cclc_occurred_at_ns")),
              "5000");
    EXPECT_EQ(Text(Member(payload, "details"), "cause_class"), "emergency_stop");

    // Malformed or mismatched events are refused before any record.
    auto old_schema = event("component_incident", 3.0).as_object();
    for (auto &[key, value] : old_schema) {
      if (key != "payload") continue;
      auto details = value.as_object();
      for (auto &[name, field] : details)
        if (name == "schema_version")
          field = CanonicalValue("manyforge.component_incident_event.v1");
      value = CanonicalValue(std::move(details));
    }
    EXPECT_THROW(static_cast<void>(core.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", CanonicalValue(old_schema))),
                 OperatorAlarmError);
    auto mismatched = event("component_incident", 3.0).as_object();
    for (auto &[key, value] : mismatched) {
      if (key == "incident_id") value = CanonicalValue("incident.other");
    }
    EXPECT_THROW(static_cast<void>(core.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", CanonicalValue(mismatched))),
                 OperatorAlarmError);
    EXPECT_THROW(static_cast<void>(core.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", event("component_incident_reopened", 3.0))),
                 OperatorAlarmError);
    EXPECT_THROW(static_cast<void>(core.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", event("component_incident", 0.0))),
                 OperatorAlarmError);
    EXPECT_THROW(static_cast<void>(core.RecordCCLCIncidentEvent(
                     "", event("component_incident", 3.0))),
                 OperatorAlarmError);
    core.Shutdown();
  }
  // A restarted reader replays from a lost cursor: the durable history
  // answers, so nothing is recorded twice.
  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), Clocks());
  EXPECT_EQ(Text(replacement.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", event("component_incident", 1.0)),
                 "status"),
            "duplicate");
  EXPECT_EQ(Text(replacement.RecordCCLCIncidentEvent(
                     "cclc.epoch.1", event("component_incident_acknowledged", 2.0)),
                 "status"),
            "duplicate");
  EXPECT_EQ(Array(replacement.HistoryPage(query), "events").size(), 2U);
}

CanonicalValue PreviousSessionReport(const std::string &message =
    "Previous session journals retained; old events are not imported into History.") {
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.workbench_previous_session.v1")},
      {"session_id", CanonicalValue(std::string(32U, 'a'))},
      {"archive_relative_path", CanonicalValue("previous-sessions/" + std::string(32U, 'a'))},
      {"shutdown", CanonicalValue("clean")},
      {"effects", CanonicalValue("closed")},
      {"history_coverage", CanonicalValue("not_imported")},
      {"journal_families", CanonicalValue(CanonicalValue::Array{
          CanonicalValue("program_cycle"), CanonicalValue("scene"),
          CanonicalValue("manager"), CanonicalValue("cclc"),
          CanonicalValue("kernel_operation")})},
      {"message", CanonicalValue(message)},
  });
}

TEST(HeadlessApiCore, PreviousSessionReportCommitsBeforeReceiptAndDeduplicatesAcrossOwners) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  HistorianQuery query;
  query.components.insert("manyforge.workbench");
  query.event_types.insert("previous_session_report");
  std::string identity;
  {
    HeadlessApiCore core(config, Clocks());
    const auto receipt = core.AppendWorkbenchPreviousSessionReport(PreviousSessionReport());
    EXPECT_EQ(Text(receipt, "status"), "stored");
    EXPECT_EQ(Text(receipt, "session_id"), std::string(32U, 'a'));
    const auto page = core.HistoryPage(query);
    const auto &rows = Array(page, "events");
    ASSERT_EQ(rows.size(), 1U);
    const auto &event = Member(rows.front(), "event");
    identity = Text(event, "event_id");
    EXPECT_EQ(Text(event, "severity"), "INFO");
    EXPECT_EQ(Text(event, "occurred_at_wall"), "unknown");
    EXPECT_EQ(Text(event, "occurred_at_ns"), "0");
    EXPECT_EQ(content_identity::canonical_json(Member(event, "payload")),
              content_identity::canonical_json(PreviousSessionReport()));
  }
  config.owner_epoch = "api.epoch.2";
  {
    HeadlessApiCore replacement(config, Clocks());
    const auto receipt = replacement.AppendWorkbenchPreviousSessionReport(PreviousSessionReport());
    EXPECT_EQ(Text(receipt, "status"), "duplicate");
    const auto page = replacement.HistoryPage(query);
    ASSERT_EQ(Array(page, "events").size(), 1U);
    EXPECT_EQ(Text(Member(Array(page, "events").front(), "event"), "event_id"), identity);
  }
  try {
    HeadlessApiCore conflicting(config, Clocks());
    static_cast<void>(conflicting.AppendWorkbenchPreviousSessionReport(
        PreviousSessionReport("Conflicting report content")));
    FAIL() << "same archived session accepted conflicting report content";
  } catch (const EventHistorianError &error) {
    EXPECT_EQ(error.code(), "historian_event_identity_conflict");
  }
}

TEST(HeadlessApiCore, PreviousSessionReportRefusesChangedFamiliesAndImportedClaims) {
  TemporaryDirectory directory;
  for (const auto &[field, replacement] : CanonicalValue::Object{
           {"journal_families", CanonicalValue(CanonicalValue::Array{})},
           {"history_coverage", CanonicalValue("imported")},
           {"shutdown", CanonicalValue("verified")},
           {"effects", CanonicalValue("safe")},
           {"archive_relative_path", CanonicalValue("../foreign")}}) {
    auto report = PreviousSessionReport().as_object();
    for (auto &[name, value] : report) if (name == field) value = replacement;
    auto config = Config(directory, "api.epoch.invalid");
    HeadlessApiCore core(config, Clocks());
    EXPECT_THROW(static_cast<void>(core.AppendWorkbenchPreviousSessionReport(
                     CanonicalValue(std::move(report)))), OperatorAlarmError) << field;
  }
}

} // namespace
CanonicalValue ClassifiedPreviousSessionReport(const std::string &shutdown,
                                               const std::string &effects,
                                               const std::string &identity = std::string(32U, 'a')) {
  auto fields = PreviousSessionReport().as_object();
  for (auto &[name, value] : fields) {
    if (name == "shutdown") value = CanonicalValue(shutdown);
    if (name == "effects") value = CanonicalValue(effects);
    if (name == "session_id") value = CanonicalValue(identity);
    if (name == "archive_relative_path") value = CanonicalValue("previous-sessions/" + identity);
  }
  return CanonicalValue(std::move(fields));
}

CanonicalValue::Array PreviousSessionAlarms(HeadlessApiCore &core) {
  const auto snapshot = core.AlarmSnapshot();
  CanonicalValue::Array result;
  for (const auto &alarm : Array(snapshot, "alarms")) {
    if (Text(alarm, "definition_id") == "manyforge.runtime.previous_session_unclean") {
      result.push_back(alarm);
    }
  }
  return result;
}

TEST(HeadlessApiCore, PreviousSessionNoticeUsesExistingNonMotionAcknowledgementAndHistory) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  const auto report = ClassifiedPreviousSessionReport("unknown", "unknown");
  HistorianQuery acknowledged;
  acknowledged.components = {"manyforge.alarm"};
  acknowledged.event_types = {"operator_alarm_acknowledged"};
  acknowledged.correlations = {{"incident_id", "workbench.previous-session." + std::string(32U, 'a')}};
  acknowledged.limit = 1U;
  {
    HeadlessApiCore core(config, Clocks());
    EXPECT_EQ(Text(core.ObserveWorkbenchPreviousSessionReport(report), "status"), "registered");
    const auto alarms = PreviousSessionAlarms(core);
    ASSERT_EQ(alarms.size(), 1U);
    const auto &alarm = alarms.front();
    EXPECT_EQ(Text(alarm, "lifecycle_state"), "returned_unacknowledged");
    EXPECT_EQ(Text(alarm, "priority"), "medium");
    EXPECT_FALSE(std::get<bool>(Member(alarm, "requires_motion_attention").storage()));
    EXPECT_TRUE(std::get<bool>(Member(alarm, "acknowledgement_required").storage()));
    EXPECT_EQ(Text(alarm, "message"), "ManyForge did not close correctly last time. The cell started empty; nothing from the previous session was resumed. Its records are kept; details in History.");
    EXPECT_EQ(Text(Member(alarm, "details"), "failure_time_status"), "unavailable");
    EXPECT_EQ(content_identity::canonical_json(Member(Member(alarm, "details"), "previous_session")),
              content_identity::canonical_json(report));
    static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(report));
    EXPECT_EQ(Text(PreviousSessionAlarms(core).front(), "activation_id"), Text(alarm, "activation_id"));
    static_cast<void>(core.AcknowledgeAlarm(Text(alarm, "condition_id"), Text(alarm, "activation_id"),
        Unsigned(alarm, "record_revision"), Caller(), std::nullopt, true));
    EXPECT_TRUE(PreviousSessionAlarms(core).empty());
    static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(report));
    EXPECT_TRUE(PreviousSessionAlarms(core).empty());
    // Ordinary ACK delivery is asynchronous. Prove its commit before replacing
    // the owner; this is not a claim about a crash before that commit.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (Array(core.HistoryPage(acknowledged), "events").empty() &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_EQ(Array(core.HistoryPage(acknowledged), "events").size(), 1U);
  }
  config.owner_epoch = "api.epoch.2";
  {
    HeadlessApiCore replacement(config, Clocks());
    EXPECT_EQ(Text(replacement.ObserveWorkbenchPreviousSessionReport(report), "status"), "acknowledged");
    EXPECT_TRUE(PreviousSessionAlarms(replacement).empty());
  }
  config.owner_epoch = "api.epoch.3";
  HeadlessApiCore next(config, Clocks());
  EXPECT_EQ(Text(next.ObserveWorkbenchPreviousSessionReport(
      ClassifiedPreviousSessionReport("unknown", "unknown", std::string(32U, 'b'))), "status"), "registered");
  EXPECT_EQ(PreviousSessionAlarms(next).size(), 1U);
}

TEST(HeadlessApiCore, PreviousSessionNoticeKeepsUnacknowledgedObligationAcrossOwnerReplacement) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  const auto report = ClassifiedPreviousSessionReport("unknown", "closed");
  {
    HeadlessApiCore core(config, Clocks());
    static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(report));
    ASSERT_EQ(PreviousSessionAlarms(core).size(), 1U);
  }
  config.owner_epoch = "api.epoch.2";
  HeadlessApiCore replacement(config, Clocks());
  EXPECT_EQ(Text(replacement.ObserveWorkbenchPreviousSessionReport(report), "status"), "registered");
  ASSERT_EQ(PreviousSessionAlarms(replacement).size(), 1U);
}

TEST(HeadlessApiCore, PreviousSessionNoticePreservesClosedOpaqueQuietAndReadableUnresolvedPolicy) {
  for (const auto &effects : {"closed", "unknown", "unresolved"}) {
    TemporaryDirectory directory;
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    EXPECT_EQ(Text(core.ObserveWorkbenchPreviousSessionReport(
        ClassifiedPreviousSessionReport("clean", effects)), "status"),
        std::string(effects) == "unresolved" ? "registered" : "quiet");
    EXPECT_EQ(PreviousSessionAlarms(core).size(), std::string(effects) == "unresolved" ? 1U : 0U);
  }
}

TEST(HeadlessApiCore, PreviousSessionNoticeRefusesAnotherPredecessorWithoutWritingIt) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(ClassifiedPreviousSessionReport("unknown", "unknown")));
  EXPECT_THROW(static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(
      ClassifiedPreviousSessionReport("unknown", "unknown", std::string(32U, 'b')))), OperatorAlarmError);
  HistorianQuery reports;
  reports.event_types = {"previous_session_report"};
  ASSERT_EQ(Array(core.HistoryPage(reports), "events").size(), 1U);
}

TEST(HeadlessApiCore, PreviousSessionNoticeRejectsAcknowledgementForAnotherDefinitionOrSubject) {
  for (const int mismatch : {0, 1, 2}) {
    const bool wrong_definition = mismatch == 0;
    TemporaryDirectory directory;
    const auto config = Config(directory, "api.epoch.1");
    const auto identity = std::string(32U, 'a');
    {
      SqliteEventHistoryStore store(config.historian_database_path,
                                   config.historian_record_retention);
      const auto false_ack = MakeHistorianEvent("manyforge.alarm", "operator_alarm_acknowledged", "INFO", 0, "unknown",
          CanonicalValue(CanonicalValue::Object{
              {"definition_id", CanonicalValue(wrong_definition ? "manyforge.test.other" : "manyforge.runtime.previous_session_unclean")},
              {"subject", CanonicalValue(CanonicalValue::Object{
                  {"subject_kind", CanonicalValue("workbench_session")},
                  {"subject_id", CanonicalValue(mismatch == 1 ? std::string(32U, 'b') : identity)},
                  {"resource_ids", CanonicalValue(mismatch == 2 ? CanonicalValue::Array{CanonicalValue("other_resource")} : CanonicalValue::Array{})}})},
              {"correlations", CanonicalValue(CanonicalValue::Object{
                  {"incident_id", CanonicalValue("workbench.previous-session." + identity)}})},
          }));
      static_cast<void>(store.Append(false_ack));
    }
    HeadlessApiCore core(config, Clocks());
    EXPECT_EQ(Text(core.ObserveWorkbenchPreviousSessionReport(
        ClassifiedPreviousSessionReport("unknown", "unknown")), "status"), "registered");
    EXPECT_EQ(PreviousSessionAlarms(core).size(), 1U);
  }
}

TEST(HeadlessApiCore, PreviousSessionNoticeBulkAcknowledgementUsesTheSameRetainedHistoryProof) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  const auto report = ClassifiedPreviousSessionReport("unknown", "unknown");
  {
    HeadlessApiCore core(config, Clocks());
    static_cast<void>(core.ObserveWorkbenchPreviousSessionReport(report));
    const auto snapshot = core.AlarmSnapshot();
    std::vector<AlarmAcknowledgementMember> members;
    for (const auto &alarm : Array(snapshot, "alarms")) {
      const auto state = Text(alarm, "lifecycle_state");
      if (state == "active_unacknowledged" || state == "returned_unacknowledged") {
        members.push_back({Text(alarm, "condition_id"), Text(alarm, "activation_id"), Unsigned(alarm, "record_revision")});
      }
    }
    ASSERT_FALSE(members.empty());
    static_cast<void>(core.AcknowledgeAllAlarms(std::move(members),
        Unsigned(snapshot, "generation"), Caller(), std::nullopt, true));
    EXPECT_TRUE(PreviousSessionAlarms(core).empty());
    // Ordinary graceful shutdown drains the existing historian writer.
  }
  config.owner_epoch = "api.epoch.2";
  HeadlessApiCore replacement(config, Clocks());
  EXPECT_EQ(Text(replacement.ObserveWorkbenchPreviousSessionReport(report), "status"), "acknowledged");
  EXPECT_TRUE(PreviousSessionAlarms(replacement).empty());
}

namespace {
CanonicalValue RuntimeFaultSnapshot(const std::string &epoch, bool runner,
                                    bool journal, bool internal = false) {
  const CanonicalValue journal_fact(CanonicalValue::Object{
      {"request_owner_epoch", CanonicalValue(epoch)},
      {"write_kind", CanonicalValue("state_observed")},
      {"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue("sqlite_write_failed")},
                    {"message", CanonicalValue("disk full")},
                    {"details", CanonicalValue(CanonicalValue::Object{})}})},
      {"operation_id", CanonicalValue("operation.1")},
      {"operation_revision", CanonicalValue(2.0)},
      {"request_id", CanonicalValue("request.1")},
      {"program_run_id", CanonicalValue("run.1")},
      {"execution_scope",
       CanonicalValue(CanonicalValue::Object{
           {"node_instance_id", CanonicalValue("move")},
           {"node_attempt_id", CanonicalValue("move.attempt.1")}})}});
  const CanonicalValue runner_fact(CanonicalValue::Object{
      {"request_owner_epoch", CanonicalValue(epoch)},
      {"manager_owner_epoch", CanonicalValue("manager.owner")},
      {"manager_runtime_epoch", CanonicalValue("manager.runtime")},
      {"failure_predicate", CanonicalValue("program_runner_liveness_probe")},
      {"message", CanonicalValue("connection refused")},
      {"established_cycle",
       CanonicalValue(CanonicalValue::Object{
           {"program_runner_owner_epoch", CanonicalValue("runner.owner")},
           {"cycle_generation", CanonicalValue(2.0)},
           {"state", CanonicalValue("running")},
           {"program_run_id", CanonicalValue("run.1")},
           {"cycle_claim_generation", CanonicalValue(1.0)},
           {"terminal_or_settlement_reference", CanonicalValue(nullptr)},
           {"accepted_reset", CanonicalValue(nullptr)}})}});
  const CanonicalValue internal_fact(CanonicalValue::Object{
      {"request_owner_epoch", CanonicalValue(epoch)},
      {"reason_code",
       CanonicalValue("scheduler_runtime_safety_callback_exception")},
      {"stop_accepted", CanonicalValue(false)},
      {"stop_refusal_code", CanonicalValue("stop_operation_not_fenceable")},
      {"later_count", CanonicalValue(3.0)}});
  return CanonicalValue(CanonicalValue::Object{
      {"operation_journal_write_fault",
       journal ? journal_fact : CanonicalValue(nullptr)},
      {"program_runner_loss", runner ? runner_fact : CanonicalValue(nullptr)},
      {"kernel_internal_fault",
       internal ? internal_fact : CanonicalValue(nullptr)}});
}
} // namespace

TEST(HeadlessApiCore, PublicationFaultKeepsItsActualPredicateAndDiagnostic) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto facts = RuntimeFaultSnapshot("kernel.1", true, false);
  auto fault =
      WithField(Member(facts, "program_runner_loss"), "failure_predicate",
                CanonicalValue("program_runner_publication_fault"));
  fault =
      WithField(fault, "message",
                CanonicalValue(
                    "program_bbkey_publication_failed: exact key ACK missing"));
  facts = WithField(facts, "program_runner_loss", fault);
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions("kernel.1", facts), "changed")
          .as_bool());
  const auto snapshot = core.AlarmSnapshot();
  const auto *alarm =
      AlarmFor(snapshot, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(Member(*alarm, "details"), "failure_predicate"),
            "program_runner_publication_fault");
  EXPECT_EQ(Text(Member(*alarm, "details"), "message"),
            "program_bbkey_publication_failed: exact key ACK missing");
  EXPECT_EQ(Member(Member(*alarm, "details"), "established_cycle"),
            Member(fault, "established_cycle"));
  EXPECT_FALSE(
      Member(core.ObserveRuntimeFaultConditions(
                 "kernel.1", RuntimeFaultSnapshot("kernel.1", false, false)),
             "changed")
          .as_bool());
  const auto retained = core.AlarmSnapshot();
  const auto *same =
      AlarmFor(retained, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(same, nullptr);
  EXPECT_EQ(*same, *alarm);
}

TEST(HeadlessApiCore, KernelInternalFaultRaisesItsTypedCauseOnce) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto facts = RuntimeFaultSnapshot("kernel.1", false, false, true);
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions("kernel.1", facts), "changed")
          .as_bool());
  const auto snapshot = core.AlarmSnapshot();
  const auto *alarm =
      AlarmFor(snapshot, "manyforge.runtime.kernel_internal_fault");
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*alarm, "details"), "reason_code"),
            "scheduler_runtime_safety_callback_exception");
  // A refused containment Full Stop is visible with its refusal code.
  EXPECT_FALSE(Member(Member(*alarm, "details"), "stop_accepted").as_bool());
  EXPECT_EQ(Text(Member(*alarm, "details"), "stop_refusal_code"),
            "stop_operation_not_fenceable");
  EXPECT_EQ(Text(Member(*alarm, "correlations"), "source_owner_epoch"),
            "kernel.1");
  // A higher later_count is the same fault: no second activation.
  EXPECT_FALSE(
      Member(core.ObserveRuntimeFaultConditions(
                 "kernel.1",
                 WithField(facts, "kernel_internal_fault",
                           WithField(Member(facts, "kernel_internal_fault"),
                                     "later_count", CanonicalValue(9.0)))),
             "changed")
          .as_bool());
  auto unknown = Member(facts, "kernel_internal_fault");
  unknown = WithField(unknown, "reason_code", CanonicalValue(""));
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   "kernel.1", WithField(facts, "kernel_internal_fault", unknown))),
               std::exception);
  // A refusal code exactly when the Full Stop was refused.
  const auto accepted_with_code = WithField(Member(facts, "kernel_internal_fault"),
                                            "stop_accepted", CanonicalValue(true));
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   "kernel.1",
                   WithField(facts, "kernel_internal_fault", accepted_with_code))),
               std::exception);
  const auto refused_without_code = WithField(Member(facts, "kernel_internal_fault"),
                                              "stop_refusal_code", CanonicalValue(nullptr));
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   "kernel.1",
                   WithField(facts, "kernel_internal_fault", refused_without_code))),
               std::exception);
}

TEST(HeadlessApiCore,
     RuntimeFaultsRaiseOnceAndRemainActiveAcrossAckNullAndNativeOwnerChange) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto none = RuntimeFaultSnapshot("kernel.1", false, false);
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions("kernel.1", none), "changed")
          .storage()));
  const auto facts = RuntimeFaultSnapshot("kernel.1", true, true);
  EXPECT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions("kernel.1", facts), "changed")
          .storage()));
  const auto raised = core.AlarmSnapshot();
  for (const auto definition :
       {"manyforge.runtime.program_runner_loss",
        "manyforge.runtime.operation_journal_write_fault"}) {
    const auto *alarm = AlarmFor(raised, definition);
    ASSERT_NE(alarm, nullptr);
    EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
    EXPECT_TRUE(
        std::get<bool>(Member(*alarm, "acknowledgement_required").storage()));
    EXPECT_EQ(Text(Member(*alarm, "correlations"), "source_owner_epoch"),
              "kernel.1");
    static_cast<void>(core.AcknowledgeAlarm(
        Text(*alarm, "condition_id"), Text(*alarm, "activation_id"),
        Unsigned(*alarm, "record_revision"), Caller(std::string(definition)),
        std::nullopt, true));
  }
  const auto acknowledged = core.AlarmSnapshot();
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions("kernel.1", facts), "changed")
          .storage()));
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(
                 "kernel.2", RuntimeFaultSnapshot("kernel.2", false, false)),
             "changed")
          .storage()));
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(
                 "kernel.2", RuntimeFaultSnapshot("kernel.2", true, true)),
             "changed")
          .storage()));
  const auto retained = core.AlarmSnapshot();
  for (const auto definition :
       {"manyforge.runtime.program_runner_loss",
        "manyforge.runtime.operation_journal_write_fault"}) {
    const auto *alarm = AlarmFor(retained, definition);
    ASSERT_NE(alarm, nullptr);
    const auto *old = AlarmFor(acknowledged, definition);
    ASSERT_NE(old, nullptr);
    EXPECT_EQ(Text(*alarm, "activation_id"), Text(*old, "activation_id"));
    EXPECT_EQ(Unsigned(*alarm, "record_revision"),
              Unsigned(*old, "record_revision"));
    EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_acknowledged");
    EXPECT_EQ(Text(Member(*alarm, "correlations"), "source_owner_epoch"),
              "kernel.1");
  }
}

TEST(HeadlessApiCore,
     RuntimeFaultSnapshotValidatesBothFactsBeforeAnyAlarmMutation) {
  for (const auto scenario : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(scenario);
    TemporaryDirectory directory;
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    auto fields = RuntimeFaultSnapshot("kernel.1", true, true).as_object();
    for (auto &[kind, fact] : fields) {
      if (kind != "program_runner_loss")
        continue;
      auto members = fact.as_object();
      for (auto &[name, value] : members) {
        if (scenario == 0 && name == "request_owner_epoch")
          value = CanonicalValue("foreign.kernel");
        if (scenario == 1 && name == "failure_predicate")
          value = CanonicalValue("provider_unavailable");
        if (scenario == 2 && name == "message")
          value = CanonicalValue(std::string(1025U, 'x'));
        if (scenario == 3 && name == "established_cycle") {
          auto cycle = value.as_object();
          for (auto &[key, field] : cycle)
            if (key == "state")
              field = CanonicalValue("idle_after_reset");
          value = CanonicalValue(std::move(cycle));
        }
      }
      if (scenario == 4)
        members.emplace_back("unexpected", CanonicalValue(true));
      fact = CanonicalValue(std::move(members));
    }
    EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                     "kernel.1", CanonicalValue(std::move(fields)))),
                 std::exception);
    const auto snapshot = core.AlarmSnapshot();
    EXPECT_EQ(AlarmFor(snapshot, "manyforge.runtime.program_runner_loss"),
              nullptr);
    EXPECT_EQ(
        AlarmFor(snapshot, "manyforge.runtime.operation_journal_write_fault"),
        nullptr);
  }
}

TEST(HeadlessApiCore,
     RuntimeFaultMaximumValidEscapedIdentitiesReachTheAlarmOwnerUnchanged) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  // C0 is valid NFC text and requires six JSON bytes per source byte.
  const std::string native_identity(2048U, '\x01');
  const std::string role_identity(128U, '\x01');
  const std::string attempt(512U, '\x01');
  const std::string message(1024U, '\x01');
  auto fields = RuntimeFaultSnapshot(native_identity, true, true).as_object();
  for (auto &[kind, fact] : fields) {
    if (fact.is_null()) // kernel_internal_fault is absent in this snapshot
      continue;
    auto members = fact.as_object();
    for (auto &[key, value] : members) {
      if (kind == "operation_journal_write_fault") {
        if (key == "request_id" || key == "program_run_id" ||
            key == "operation_id")
          value = CanonicalValue(native_identity);
        if (key == "write_kind")
          value = CanonicalValue(std::string(64U, '\x01'));
        if (key == "execution_scope")
          value = CanonicalValue(CanonicalValue::Object{
              {"node_instance_id", CanonicalValue(role_identity)},
              {"node_attempt_id", CanonicalValue(attempt)}});
        if (key == "error")
          value = CanonicalValue(CanonicalValue::Object{
              {"code", CanonicalValue(std::string(256U, '\x01'))},
              {"message", CanonicalValue(message)},
              {"details", CanonicalValue(CanonicalValue::Object{})}});
      } else {
        if (key == "manager_owner_epoch" || key == "manager_runtime_epoch")
          value = CanonicalValue(role_identity);
        if (key == "message")
          value = CanonicalValue(message);
        if (key == "established_cycle") {
          auto cycle = value.as_object();
          for (auto &[name, item] : cycle)
            if (name == "program_run_id" ||
                name == "program_runner_owner_epoch")
              item = CanonicalValue(role_identity);
          value = CanonicalValue(std::move(cycle));
        }
      }
    }
    fact = CanonicalValue(std::move(members));
  }
  const CanonicalValue maximum(std::move(fields));
  EXPECT_GT(content_identity::canonical_json(maximum).size(), 16U * 1024U);
  ASSERT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(native_identity, maximum),
             "changed")
          .storage()));
  const auto snapshot = core.AlarmSnapshot();
  const auto *fault =
      AlarmFor(snapshot, "manyforge.runtime.operation_journal_write_fault");
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(Text(*fault, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*fault, "correlations"), "source_owner_epoch"),
            native_identity);
  EXPECT_EQ(Text(Member(*fault, "correlations"), "operation_id"),
            native_identity);
  EXPECT_EQ(Text(Member(*fault, "details"), "request_id"), native_identity);
  EXPECT_EQ(Text(Member(*fault, "details"), "program_run_id"), native_identity);
  EXPECT_EQ(Text(Member(Member(*fault, "correlations"), "execution_scope"),
                 "node_attempt_id"),
            attempt);
  const auto *runner =
      AlarmFor(snapshot, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(Text(*runner, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*runner, "correlations"), "source_owner_epoch"),
            native_identity);
  EXPECT_EQ(Text(Member(*runner, "details"), "message"), message);
}

CanonicalValue ProgramFailureCondition(const std::string &resource) {
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_failure_condition.v1")},
      {"program_runner_owner_epoch", CanonicalValue("runner.1")},
      {"execution_control_revision", CanonicalValue(2.0)},
      {"program_run_id", CanonicalValue("run.1")},
      {"run_revision", CanonicalValue(4.0)},
      {"state", CanonicalValue("active")},
      {"occurrence",
       CanonicalValue(CanonicalValue::Object{
           {"node_instance_id", CanonicalValue("move")},
           {"node_attempt_id", CanonicalValue("move.attempt.1")},
           {"operator_facing_run_revision", CanonicalValue(4.0)}})},
      {"operation", CanonicalValue(CanonicalValue::Object{
                        {"source_owner_epoch", CanonicalValue("kernel.1")},
                        {"operation_id", CanonicalValue("operation.1")},
                        {"operation_revision", CanonicalValue(1.0)}})},
      {"resource_ids",
       CanonicalValue(CanonicalValue::Array{CanonicalValue(resource)})},
      {"failure", CanonicalValue(CanonicalValue::Object{
                      {"code", CanonicalValue("planning_failed")},
                      {"message", CanonicalValue("original failure")}})}});
}

CanonicalValue ProgramFailureState(CanonicalValue value,
                                   const std::string &state,
                                   double control_revision,
                                   double run_revision) {
  value = JournalField(value, "state", CanonicalValue(state));
  value = JournalField(value, "execution_control_revision",
                       CanonicalValue(control_revision));
  return JournalField(value, "run_revision", CanonicalValue(run_revision));
}

// Source currency is projected by the existing source registry. A retained
// record keeps the quality of its last accepted condition update, as in
// DurableOperationFactsDriveHistoryAndAlarmWithoutOwningExecutionState.
void ExpectProgramFailureSourceCurrent(const CanonicalValue &snapshot,
                                      const bool current) {
  const auto &domains = Array(snapshot, "domains");
  const auto motion = std::find_if(
      domains.begin(), domains.end(),
      [](const auto &domain) { return Text(domain, "domain") == "motion"; });
  ASSERT_NE(motion, domains.end());
  EXPECT_EQ(Unsigned(*motion, "source_count"), 1U);
  EXPECT_EQ(Unsigned(*motion, "unknown_source_count"), current ? 0U : 1U);
  EXPECT_EQ(std::get<bool>(Member(*motion, "reconciled").storage()), current);
}

TEST(HeadlessApiCore,
     ProgramFailureUsesExactOccurrenceAndAckDoesNotResolveItsCause) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto active = ProgramFailureCondition(resource);
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(active), "status"),
            "observed");
  const auto first = core.AlarmSnapshot();
  const auto alarm = OnlyAlarm(first);
  EXPECT_THROW(static_cast<void>(core.ObserveProgramFailureCondition(
                   ProgramFailureState(active, "clear", 2, 4))),
               OperatorAlarmError);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  EXPECT_EQ(Text(alarm, "definition_id"),
            "manyforge.program.operation_failure");
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(active), "status"),
            "duplicate");
  EXPECT_EQ(Unsigned(OnlyAlarm(core.AlarmSnapshot()), "occurrence_count"), 1U);
  static_cast<void>(core.AcknowledgeAlarm(
      Text(alarm, "condition_id"), Text(alarm, "activation_id"),
      Unsigned(alarm, "record_revision"), Caller(), std::nullopt, true));
  const auto acknowledged_alarm = OnlyAlarm(core.AlarmSnapshot());
  EXPECT_EQ(Text(acknowledged_alarm, "lifecycle_state"), "active_acknowledged");
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "unknown", 2, 4)));
  const auto unknown_snapshot = core.AlarmSnapshot();
  ExpectProgramFailureSourceCurrent(unknown_snapshot, false);
  EXPECT_EQ(OnlyAlarm(unknown_snapshot), acknowledged_alarm);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  const auto recovered_snapshot = core.AlarmSnapshot();
  ExpectProgramFailureSourceCurrent(recovered_snapshot, true);
  EXPECT_EQ(OnlyAlarm(recovered_snapshot), acknowledged_alarm);
  // Advancing a failed Run observation keeps the same active occurrence.
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "active", 3, 5)));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "active_acknowledged");
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "clear", 4, 6)));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(active), "status"),
            "stale");
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}

TEST(HeadlessApiCore,
     ProgramFailureForeignOrDifferentOccurrenceNeverClearsCurrentFailure) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  auto clear = ProgramFailureState(active, "clear", 3, 5);
  auto foreign = JournalField(clear, "program_runner_owner_epoch",
                              CanonicalValue("foreign"));
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(foreign), "status"),
            "stale");
  auto other =
      JournalField(clear, "occurrence",
                   JournalField(Member(clear, "occurrence"), "node_attempt_id",
                                CanonicalValue("other.attempt")));
  static_cast<void>(core.ObserveProgramFailureCondition(other));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "active_unacknowledged");
  static_cast<void>(core.ObserveProgramFailureCondition(clear));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "returned_unacknowledged");
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(clear), "status"),
            "duplicate");
}

TEST(HeadlessApiCore,
     ProgramFailureMultipleExactOriginsReuseOwnerPointsForIdempotence) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto first = ProgramFailureCondition(resource);
  auto second =
      JournalField(first, "operation",
                   JournalField(Member(first, "operation"), "operation_id",
                                CanonicalValue("operation.2")));
  second =
      JournalField(second, "occurrence",
                   JournalField(Member(second, "occurrence"), "node_attempt_id",
                                CanonicalValue("move.attempt.2")));
  for (const auto &value : {first, second, first, second})
    static_cast<void>(core.ObserveProgramFailureCondition(value));
  const auto snapshot = core.AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 2U);
  for (const auto &alarm : Array(snapshot, "alarms"))
    EXPECT_EQ(Unsigned(alarm, "occurrence_count"), 1U);
}

TEST(HeadlessApiCore,
     ProgramFailureOwnerReplacementAndMalformedSourceLeavePriorCauseActive) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  const auto accepted_alarm = OnlyAlarm(core.AlarmSnapshot());
  auto invalid = JournalField(active, "operation", CanonicalValue(nullptr));
  EXPECT_THROW(static_cast<void>(core.ObserveProgramFailureCondition(invalid)),
               OperatorAlarmError);
  const auto unknown_snapshot = core.AlarmSnapshot();
  ExpectProgramFailureSourceCurrent(unknown_snapshot, false);
  EXPECT_EQ(OnlyAlarm(unknown_snapshot), accepted_alarm);
  auto next = JournalField(active, "program_runner_owner_epoch",
                           CanonicalValue("runner.2"));
  static_cast<void>(core.ObserveProgramFailureCondition(next));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 2U);
  EXPECT_EQ(Text(core.ObserveProgramFailureCondition(
                     ProgramFailureState(active, "clear", 5, 7)),
                 "status"),
            "stale");
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 2U);
}

TEST(HeadlessApiCore,
     ProgramFailureMaximumEscapedProvenanceFitsTheRealAlarmOwner) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  auto value = ProgramFailureCondition(resource);
  value = JournalField(value, "program_runner_owner_epoch",
                       CanonicalValue(std::string(512, '\1')));
  value = JournalField(value, "program_run_id",
                       CanonicalValue(std::string(512, '\1')));
  value = JournalField(
      value, "operation",
      CanonicalValue(CanonicalValue::Object{
          {"source_owner_epoch", CanonicalValue(std::string(2048, '\1'))},
          {"operation_id", CanonicalValue(std::string(2048, '\1'))},
          {"operation_revision", CanonicalValue(1.0)}}));
  value = JournalField(
      value, "occurrence",
      CanonicalValue(CanonicalValue::Object{
          {"node_instance_id", CanonicalValue(std::string(128, '\1'))},
          {"node_attempt_id", CanonicalValue(std::string(512, '\1'))},
          {"operator_facing_run_revision", CanonicalValue(4.0)}}));
  value =
      JournalField(value, "failure",
                   CanonicalValue(CanonicalValue::Object{
                       {"code", CanonicalValue(std::string(256, '\1'))},
                       {"message", CanonicalValue(std::string(4096, '\1'))}}));
  ASSERT_NO_THROW(
      static_cast<void>(core.ObserveProgramFailureCondition(value)));
  const auto snapshot = core.AlarmSnapshot();
  const auto &alarm = OnlyAlarm(snapshot);
  EXPECT_EQ(Member(Member(alarm, "details"), "failure"),
            Member(value, "failure"));
  EXPECT_EQ(Text(Member(alarm, "correlations"), "operation_id"),
            std::string(2048, '\1'));
  EXPECT_LT(content_identity::canonical_json(Member(alarm, "details")).size(),
            32U * 1024U);
  EXPECT_LT(
      content_identity::canonical_json(Member(alarm, "correlations")).size(),
      32U * 1024U);
}

TEST(
    HeadlessApiCore,
    ProgramFailureReusesScopeRefusalAndReconsidersAfterConfigurationPublication) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            kAlarmScopeDefinition);
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            "manyforge.program.operation_failure");
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "clear", 3, 5)));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "returned_unacknowledged");
}

TEST(HeadlessApiCore,
     ActualTerminalFactVersionIsClosedAndObsoleteRetryVersionIsRefused) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "kernel.1";
  const auto failure = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"message", CanonicalValue("settlement is unknown")},
      {"details", CanonicalValue(CanonicalValue::Object{})}});
  auto value = CapturedOperation(epoch, resource, failure);
  const auto obsolete =
      JournalField(value, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v4"));
  EXPECT_THROW(IngestJournalEvent(core, obsolete, epoch, 1U),
               OperatorAlarmError);
  auto state = Member(value, "operation_state");
  auto participants = Member(state, "participants").as_array();
  participants.front() = JournalField(participants.front(), "state",
                                      CanonicalValue("intervention_required"));
  const auto fact = CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("local_failure")},
      {"effect", CanonicalValue("effect_uncertain")},
      {"error", Member(participants.front(), "terminal_error")}});
  participants.front() =
      JournalField(participants.front(), "terminal_fact", fact);
  value = JournalField(
      value, "operation_state",
      JournalField(state, "participants", CanonicalValue(participants)));
  EXPECT_THROW(IngestJournalEvent(core, JournalField(value, "api_version",
                    CanonicalValue("manyforge.operation_journal_record.v7")), epoch, 1U), OperatorAlarmError);
  value = JournalField(value, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v8"));
  auto mismatch = participants;
  mismatch.front() =
      JournalField(mismatch.front(), "terminal_error", CanonicalValue(nullptr));
  const auto retained_mismatch = JournalField(
      value, "operation_state",
      JournalField(state, "participants", CanonicalValue(mismatch)));
  EXPECT_NO_THROW(IngestJournalEvent(core, retained_mismatch, epoch, 1U));
  const auto selected_error = Member(participants.front(), "terminal_error");
  auto missing = selected_error.as_object();
  std::erase_if(missing,
                [](const auto &field) { return field.first == "code"; });
  const std::vector<CanonicalValue> malformed_selected_errors{
      CanonicalValue("cleanup_failed"), CanonicalValue(CanonicalValue::Array{}),
      CanonicalValue(std::move(missing)),
      JournalField(selected_error, "unexpected", CanonicalValue("field")),
      JournalField(selected_error, "details",
                   CanonicalValue(CanonicalValue::Object{
                       {"reason", CanonicalValue(7.0)}}))};
  for (const auto &malformed : malformed_selected_errors) {
    auto invalid = participants;
    invalid.front() =
        JournalField(invalid.front(), "terminal_error", malformed);
    EXPECT_THROW(
        IngestJournalEvent(core,
                           JournalField(value, "operation_state",
                                        JournalField(state, "participants",
                                                     CanonicalValue(invalid))),
                           epoch, 2U),
        OperatorAlarmError);
  }
  auto nonterminal = participants;
  nonterminal.front() =
      JournalField(nonterminal.front(), "state", CanonicalValue("executing"));
  EXPECT_THROW(IngestJournalEvent(
                   core,
                   JournalField(value, "operation_state",
                                JournalField(state, "participants",
                                             CanonicalValue(nonterminal))),
                   epoch, 2U),
               OperatorAlarmError);
  EXPECT_NO_THROW(IngestJournalEvent(core, value, epoch, 2U));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            "manyforge.motion.execution_fault");
}

TEST(HeadlessApiCore, LogicOnlyFailureCannotInventAnOperationAlarmOrCellScope) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto value = ProgramFailureCondition("unused");
  value = JournalField(value, "operation", CanonicalValue(nullptr));
  value = JournalField(value, "resource_ids",
                       CanonicalValue(CanonicalValue::Array{}));
  EXPECT_THROW(static_cast<void>(core.ObserveProgramFailureCondition(value)),
               OperatorAlarmError);
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}
CanonicalValue OperationFailureFacts(const std::string &epoch,
                                     const std::string &resource,
                                     const std::string &effect,
                                     bool captured = true) {
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("planning_failed")},
      {"message", CanonicalValue("temporary")},
      {"details", CanonicalValue(CanonicalValue::Object{})}});
  auto payload = CapturedOperation(epoch, resource, error);
  auto state = Member(payload, "operation_state");
  auto participants = Member(state, "participants").as_array();
  participants.front() = JournalField(participants.front(), "state",
                                      CanonicalValue("intervention_required"));
  participants.front() =
      JournalField(participants.front(), "terminal_fact",
                   CanonicalValue(CanonicalValue::Object{
                       {"kind", CanonicalValue("local_failure")},
                       {"effect", CanonicalValue(effect)},
                       {"error", error}}));
  if (!captured) {
    auto fields = participants.front().as_object();
    std::erase_if(fields, [](const auto &field) {
      return field.first == "execution_scope";
    });
    participants.front() = CanonicalValue(std::move(fields));
  }
  state = JournalField(state, "participants",
                       CanonicalValue(std::move(participants)));
  state = JournalField(state, "active_stop_id", CanonicalValue(nullptr));
  state =
      JournalField(state, "active_stop_generation", CanonicalValue(nullptr));
  payload = JournalField(payload, "operation_state", state);
  return JournalField(payload, "api_version",
                      CanonicalValue("manyforge.operation_journal_record.v8"));
}

TEST(HeadlessApiCore,
     ProvenOrdinaryProgramFailureIsErrorHistoryBeforeItsRunCondition) {
  for (const std::string effect : {"no_effect", "effect_settled"}) {
    SCOPED_TRACE(effect);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    PublishActiveConfiguration(directory, *configurations.single, 1U);
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    const std::string epoch = "kernel.1";
    const auto payload = OperationFailureFacts(epoch, resource, effect);
    IngestJournalEvent(core, payload, epoch, 1U);
    static_cast<void>(core.CompleteOperationEventSnapshot(epoch, 1U));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
    HistorianQuery query;
    query.event_types.insert("operation_intervention_required");
    CanonicalValue page;
    const auto deadline = std::chrono::steady_clock::now() + 2s;
    do {
      page = core.HistoryPage(query);
      if (!Array(page, "events").empty())
        break;
      std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    ASSERT_EQ(Array(page, "events").size(), 1U);
    const auto &event = Member(Array(page, "events").front(), "event");
    EXPECT_EQ(Text(event, "severity"), "ERROR");
    EXPECT_EQ(Member(Member(event, "payload"), "journal_payload"), payload);
    EXPECT_EQ(Text(Member(event, "payload"), "message"),
              "[move / move.attempt.1] planning_failed: temporary");
    auto condition = ProgramFailureCondition(resource);
    condition = JournalField(condition, "program_run_id",
                             CanonicalValue("program-run.test"));
    condition = JournalField(condition, "operation",
                             JournalField(Member(condition, "operation"),
                                          "operation_id",
                                          CanonicalValue("operation.test.1")));
    condition = JournalField(condition, "failure",
                             CanonicalValue(CanonicalValue::Object{
                                 {"code", CanonicalValue("planning_failed")},
                                 {"message", CanonicalValue("temporary")}}));
    static_cast<void>(core.ObserveProgramFailureCondition(condition));
    EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
              "manyforge.program.operation_failure");
  }
}

TEST(HeadlessApiCore,
     NativeUnknownManualAndStopOwnedFailuresCannotBeDeferredByProgramScope) {
  for (const std::string context : {"uncertain", "absent", "manual", "stop",
                                    "cleanup", "sibling", "mismatched"}) {
    SCOPED_TRACE(context);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    PublishActiveConfiguration(directory, *configurations.single, 1U);
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    const std::string epoch = "kernel.1";
    auto payload = OperationFailureFacts(
        epoch, resource,
        context == "uncertain" ? "effect_uncertain" : "no_effect",
        context != "manual");
    auto state = Member(payload, "operation_state");
    auto participants = Member(state, "participants").as_array();
    if (context == "absent" || context == "cleanup") {
      auto fields = participants.front().as_object();
      std::erase_if(fields, [](const auto &field) {
        return field.first == "terminal_fact";
      });
      participants.front() = CanonicalValue(std::move(fields));
      payload =
          JournalField(payload, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v8"));
    }
    if (context == "cleanup" || context == "mismatched") {
      const auto error = CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("cleanup_failed")},
          {"message", CanonicalValue("cleanup outcome unknown")},
          {"details", CanonicalValue(CanonicalValue::Object{})}});
      participants.front() =
          JournalField(participants.front(), "terminal_error", error);
    }
    if (context == "sibling") {
      auto sibling = participants.front();
      auto fields = sibling.as_object();
      std::erase_if(fields, [](const auto &field) {
        return field.first == "terminal_fact";
      });
      sibling =
          JournalField(CanonicalValue(std::move(fields)), "participant_id",
                       CanonicalValue("participant.unknown"));
      participants.push_back(std::move(sibling));
    }
    state = JournalField(state, "participants",
                         CanonicalValue(std::move(participants)));
    if (context == "stop") {
      state = JournalField(state, "active_stop_id", CanonicalValue("stop.1"));
      state =
          JournalField(state, "active_stop_generation", CanonicalValue(1.0));
    }
    payload = JournalField(payload, "operation_state", state);
    IngestJournalEvent(core, payload, epoch, 1U);
    EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
              "manyforge.motion.execution_fault");
  }
}
TEST(HeadlessApiCore,
     AcceptedUnknownWatermarksBothRevisionsWithoutClearingTheCause) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  PublishActiveConfiguration(directory, *configurations.single, 1U);
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  const auto accepted_alarm = OnlyAlarm(core.AlarmSnapshot());
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "unknown", 5, 7)));
  for (const auto &[control, run] :
       {std::pair{3.0, 5.0}, std::pair{6.0, 5.0}}) {
    EXPECT_EQ(Text(core.ObserveProgramFailureCondition(
                       ProgramFailureState(active, "clear", control, run)),
                   "status"),
              "stale");
    const auto snapshot = core.AlarmSnapshot();
    EXPECT_EQ(Text(OnlyAlarm(snapshot), "lifecycle_state"),
              "active_unacknowledged");
    ExpectProgramFailureSourceCurrent(snapshot, false);
    EXPECT_EQ(OnlyAlarm(snapshot), accepted_alarm);
  }
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "active", 5, 7)));
  const auto recovered_snapshot = core.AlarmSnapshot();
  ExpectProgramFailureSourceCurrent(recovered_snapshot, true);
  EXPECT_EQ(OnlyAlarm(recovered_snapshot), accepted_alarm);
  // Completed Reset may retain the last actual Run revision; it cannot regress
  // it.
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(active, "clear", 6, 7)));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "returned_unacknowledged");
  auto next = JournalField(active, "program_run_id", CanonicalValue("run.2"));
  next = JournalField(next, "operation",
                      JournalField(Member(next, "operation"), "operation_id",
                                   CanonicalValue("operation.2")));
  next = JournalField(next, "occurrence",
                      JournalField(Member(next, "occurrence"),
                                   "operator_facing_run_revision",
                                   CanonicalValue(1.0)));
  static_cast<void>(core.ObserveProgramFailureCondition(
      ProgramFailureState(next, "active", 7, 1)));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 2U);
}

namespace {
CanonicalValue LogicalPreemption(const std::string &kind = "preempted") {
  return CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue(kind)},
      {"by", CanonicalValue(CanonicalValue::Object{
                 {"operation_id", CanonicalValue("operation.winner")},
                 {"node_instance_id", CanonicalValue("winner")},
                 {"node_attempt_id", CanonicalValue("winner.attempt.1")}})}});
}
} // namespace

TEST(HeadlessApiCore,
     LogicalPreemptionVersionPreservesIndependentPhysicalFailureFacts) {
  for (const std::string kind : {"preempted", "superseded_unsent"}) {
    for (const bool with_fact : {false, true}) {
      SCOPED_TRACE(kind);
      SCOPED_TRACE(with_fact);
      TemporaryDirectory directory;
      const auto configurations = LoadRegistryConfigurations();
      const auto resource = ResourceIds(*configurations.single).front();
      PublishActiveConfiguration(directory, *configurations.single, 1U);
      HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
      const std::string epoch = "kernel.1";
      const auto physical_failure =
          OperationFailureFacts(epoch, resource, "effect_uncertain");
      auto payload = with_fact
                         ? physical_failure
                         : CapturedOperation(
                               epoch, resource,
                               Member(Array(Member(physical_failure,
                                                   "operation_state"),
                                            "participants")
                                          .front(),
                                      "terminal_error"));
      const auto state = Member(payload, "operation_state");
      payload = JournalField(
          payload, "operation_state",
          JournalField(state, "preemption", LogicalPreemption(kind)));
      // A logical result never certifies physical settlement or clears a fault.
      for (const std::string api : {"manyforge.operation_journal_record.v3",
                                    "manyforge.operation_journal_record.v5"}) {
        EXPECT_THROW(IngestJournalEvent(core,
                                        JournalField(payload, "api_version",
                                                     CanonicalValue(api)),
                                        epoch, 1U),
                     OperatorAlarmError);
      }
      payload =
          JournalField(payload, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v8"));
      EXPECT_THROW(
          IngestJournalEvent(
              core, JournalField(payload, "operation_state", JournalField(
                  state, "preemption", LogicalPreemption("unknown"))), epoch, 1U),
          OperatorAlarmError);
      EXPECT_NO_THROW(IngestJournalEvent(core, payload, epoch, 1U));
      EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
                "manyforge.motion.execution_fault");
    }
  }
}

TEST(HeadlessApiCore, LogicalPreemptionRequiresExactDistinctCapturedActor) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "kernel.1";
  auto payload = CapturedOperation(epoch, "robot.1", CollisionFailure());
  payload =
      JournalField(payload, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v8"));
  const auto state = Member(payload, "operation_state");
  const auto ingest = [&](const CanonicalValue &preemption,
                          const CanonicalValue &captured_state) {
    IngestJournalEvent(
        core,
        JournalField(payload, "operation_state",
                     JournalField(captured_state, "preemption", preemption)),
        epoch, 1U);
  };
  EXPECT_THROW(ingest(CanonicalValue(nullptr), state), OperatorAlarmError);
  EXPECT_THROW(ingest(LogicalPreemption("completed"), state),
               OperatorAlarmError);
  EXPECT_THROW(
      ingest(JournalField(LogicalPreemption(), "extra", CanonicalValue(true)),
             state),
      OperatorAlarmError);
  for (const auto &[field, maximum] :
       std::vector<std::pair<std::string, std::size_t>>{
           {"operation_id", 2048U},
           {"node_instance_id", 128U},
           {"node_attempt_id", 512U}}) {
    for (const std::string &invalid :
         {std::string{}, std::string(maximum + 1U, 'x'),
          std::string("x\0y", 3)}) {
      const auto by = JournalField(Member(LogicalPreemption(), "by"), field,
                                   CanonicalValue(invalid));
      EXPECT_THROW(ingest(JournalField(LogicalPreemption(), "by", by), state),
                   OperatorAlarmError);
    }
  }
  EXPECT_THROW(
      ingest(JournalField(LogicalPreemption(), "by",
                          JournalField(Member(LogicalPreemption(), "by"),
                                       "operation_id",
                                       Member(state, "operation_id"))),
             state),
      OperatorAlarmError);
  auto participants = Member(state, "participants").as_array();
  auto participant = participants.front().as_object();
  std::erase_if(participant, [](const auto &field) {
    return field.first == "execution_scope";
  });
  participants.front() = CanonicalValue(std::move(participant));
  EXPECT_THROW(
      ingest(LogicalPreemption(),
             JournalField(state, "participants", CanonicalValue(participants))),
      OperatorAlarmError);
  EXPECT_THROW(
      ingest(LogicalPreemption(),
             JournalField(state, "journal_capture_order", CanonicalValue("0"))),
      OperatorAlarmError);
  auto by = Member(LogicalPreemption(), "by");
  by =
      JournalField(by, "operation_id", CanonicalValue(std::string(2048, '\1')));
  by = JournalField(by, "node_instance_id",
                    CanonicalValue(std::string(128, '\1')));
  by = JournalField(by, "node_attempt_id",
                    CanonicalValue(std::string(512, '\1')));
  EXPECT_NO_THROW(ingest(JournalField(LogicalPreemption(), "by", by), state));
}

TEST(HeadlessApiCore, StopPreemptionVersionChecksTheCompleteAffectedCapture) {
  for (const bool with_fact : {false, true}) {
    SCOPED_TRACE(with_fact);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    PublishActiveConfiguration(directory, *configurations.single, 1U);
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    const std::string epoch = "kernel.1";
    auto payload = StopAcceptanceProjectionPayload(epoch);
    auto affected =
        Member(CapturedCollisionStop(epoch, resource), "affected_state");
    auto operations = Member(affected, "operations").as_array();
    // A sibling may omit preemption and retain its independently captured fact.
    if (with_fact) {
      const auto sibling =
          Member(OperationFailureFacts(epoch, resource, "effect_uncertain"),
                 "operation_state");
      operations.push_back(JournalField(sibling, "operation_id",
                                        CanonicalValue("operation.sibling")));
    }
    const auto without_preemption = operations;
    operations.front() =
        JournalField(operations.front(), "preemption", LogicalPreemption());
    payload = JournalField(
        payload, "affected_state",
        JournalField(affected, "operations", CanonicalValue(operations)));
    for (const std::string api : {"manyforge.stop_acceptance_projection.v3",
                                  "manyforge.stop_acceptance_projection.v5"}) {
      EXPECT_THROW(IngestJournalEvent(core,
                                      JournalField(payload, "api_version",
                                                   CanonicalValue(api)),
                                      epoch, 1U),
                   OperatorAlarmError);
    }
    payload =
        JournalField(payload, "api_version",
                     CanonicalValue("manyforge.stop_acceptance_projection.v8"));
    auto invalid_preemption = without_preemption;
    invalid_preemption.front() = JournalField(invalid_preemption.front(), "preemption", LogicalPreemption("unknown"));
    EXPECT_THROW(
        IngestJournalEvent(
            core,
            JournalField(payload, "affected_state",
                         JournalField(affected, "operations",
                                      CanonicalValue(invalid_preemption))),
            epoch, 1U),
        OperatorAlarmError);
    EXPECT_NO_THROW(IngestJournalEvent(core, payload, epoch, 1U));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }
}

TEST(HeadlessApiCore,
     PhysicalReplacementFactRequiresItsSettledPreemptionCapture) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string epoch = "kernel.1";
  auto payload = CapturedOperation(epoch, "robot.1", CanonicalValue(nullptr));
  auto state = Member(payload, "operation_state");
  auto participants = Member(state, "participants").as_array();
  participants.front() =
      JournalField(participants.front(), "state", CanonicalValue("stopped"));
  const auto fact = CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("preempted_by_replacement")},
      {"effect", CanonicalValue("effect_settled")},
      {"error", CanonicalValue(nullptr)}});
  participants.front() =
      JournalField(participants.front(), "terminal_fact", fact);
  state = JournalField(state, "state", CanonicalValue("stopped"));
  state = JournalField(state, "participants", CanonicalValue(participants));
  state = JournalField(state, "preemption", LogicalPreemption());
  payload = JournalField(payload, "operation_state", state);
  EXPECT_THROW(
      IngestJournalEvent(
          core,
          JournalField(payload, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v5")),
          epoch, 1U),
      OperatorAlarmError);
  payload =
      JournalField(payload, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v8"));
  EXPECT_THROW(IngestJournalEvent(
                   core,
                   JournalField(payload, "operation_state",
                                JournalField(state, "preemption",
                                             LogicalPreemption("superseded_unsent"))),
                   epoch, 1U),
               OperatorAlarmError);
  auto absent = state.as_object();
  std::erase_if(absent,
                [](const auto &field) { return field.first == "preemption"; });
  EXPECT_THROW(IngestJournalEvent(core,
                                  JournalField(payload, "operation_state",
                                               CanonicalValue(absent)),
                                  epoch, 1U),
               OperatorAlarmError);
  for (const std::string effect : {"no_effect", "effect_uncertain"}) {
    auto invalid = participants;
    invalid.front() =
        JournalField(invalid.front(), "terminal_fact",
                     JournalField(fact, "effect", CanonicalValue(effect)));
    EXPECT_THROW(
        IngestJournalEvent(core,
                           JournalField(payload, "operation_state",
                                        JournalField(state, "participants",
                                                     CanonicalValue(invalid))),
                           epoch, 1U),
        OperatorAlarmError);
  }
  auto invalid = participants;
  invalid.front() =
      JournalField(invalid.front(), "state", CanonicalValue("succeeded"));
  EXPECT_THROW(
      IngestJournalEvent(core,
                         JournalField(payload, "operation_state",
                                      JournalField(state, "participants",
                                                   CanonicalValue(invalid))),
                         epoch, 1U),
      OperatorAlarmError);
  auto failure = CollisionFailure();
  invalid = participants;
  invalid.front() = JournalField(invalid.front(), "terminal_error", failure);
  invalid.front() = JournalField(invalid.front(), "terminal_fact",
                                 JournalField(fact, "error", failure));
  EXPECT_THROW(
      IngestJournalEvent(core,
                         JournalField(payload, "operation_state",
                                      JournalField(state, "participants",
                                                   CanonicalValue(invalid))),
                         epoch, 1U),
      OperatorAlarmError);
  EXPECT_NO_THROW(IngestJournalEvent(core, payload, epoch, 1U));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}


TEST(HeadlessApiCore, LifecycleObservationUsesOnlyTheCommonDomainResourcesMember) {
  for (const std::string kind : {"controller", "io_provider"}) {
    for (const bool empty : {false, true}) {
      SCOPED_TRACE(kind);
      SCOPED_TRACE(empty);
      ComponentConditionFixture fixture;
      auto snapshot = ComponentConditionSnapshot(
          fixture.component, fixture.resource, "active", "satisfied", "none");
      auto domain = Array(snapshot, "domains").front();
      domain = JournalField(domain, "kind", CanonicalValue(kind));
      domain = JournalField(domain, "resources", CanonicalValue(
          empty ? CanonicalValue::Array{} : CanonicalValue::Array{CanonicalValue(fixture.resource)}));
      snapshot = JournalField(snapshot, "domains", CanonicalValue(CanonicalValue::Array{domain}));
      EXPECT_EQ(Text(fixture.Observe(snapshot), "status"), "observed");
      // A superseded member is never used as a fallback source of resource truth.
      auto obsolete = domain.as_object();
      for (auto& [name, value] : obsolete)
        if (name == "resources") name = kind == "io_provider" ? "output_resources" : "current_resources";
      EXPECT_THROW(static_cast<void>(fixture.Observe(JournalField(
                       snapshot, "domains", CanonicalValue(CanonicalValue::Array{
                           CanonicalValue(std::move(obsolete))})))), OperatorAlarmError);
    }
  }
}

// Exact durable native40 state_observed row17, including its real I/O-rule
// evidence. This is recorded producer data, not a fabricated successful trip.
CanonicalValue Native40IoRuleRecord() {
  return content_identity::parse_canonical_json(R"native40({"api_version":"manyforge.operation_journal_record.v7","effect_pending":false,"intent":{"transition_reason":"runtime_scale_status_observed"},"operation_state":{"active_stop_generation":null,"active_stop_id":null,"coordination_policy":"independent","journal_capture_order":"8","operation_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participants":[{"capability_id":"execution.trajectory.execute","capability_provider_binding_id":null,"capability_provider_binding_revision":null,"command_target_id":"arm","control_binding_id":"lite6_robot.arm.execution-trajectory-execute","control_binding_revision":"sha256:8e3f32bc78e641e62fc0b9902b615e6a3ebe7626d83eb39e81efd9ca3c2425e6","control_domain_id":"lite6_robot.arm.control","control_domain_revision":"sha256:669ef8a3882b5167bc6e94e6c253f72e88a511990821348f46b3ca2c6cb2937c","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","execution_scope":{"node_attempt_id":"attempt:aa48a448ad3b425681b41b013bcae166","node_instance_id":"search_trip"},"io_rule_outcomes":[{"error":null,"evidence":{"evaluation_error":null,"source_losses":[],"state":"retired","trip":null},"registration":{"catalog_generation":"2","owner_epoch":"kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41","policy_id":"node.9d4549f11850b1b2fc25f067c5c1a46e727105f792656507e8739f08ac7b859a","registration_id":"1","scope":{"command_target_id":"arm","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","kind":"operation","motion_interrupt_scope_id":"node.9d4549f11850b1b2fc25f067c5c1a46e727105f792656507e8739f08ac7b859a","operation_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participant_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.participant.1","preparation_id":"2","prepared_contract_digest":"sha256:3b41addb986ce1e6507c476d16d0f5598d6ce8c86f4f482b35e35fedcaa82d04"}},"stop_certificate_id":null,"stop_certificate_revision":null,"stop_generation":null,"stop_id":null,"stop_result":null,"terminal_cause":"no_signal_trip"}],"participant_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.participant.1","payload_kind":"motion","progress":1,"resources":["lite6_robot.arm.coordinate.000000","lite6_robot.arm.coordinate.000001","lite6_robot.arm.coordinate.000002","lite6_robot.arm.coordinate.000003","lite6_robot.arm.coordinate.000004","lite6_robot.arm.coordinate.000005"],"state":"completed","terminal_error":null,"terminal_fact":{"effect":"effect_settled","error":null,"kind":"computed_completion"}}],"program_binding_id":"sha256:5ef2d8159a7dc41022f543ece89a058120b49cd679656ec6181ddeaac093858e","program_run_id":"program-cycle.1","runtime_scale_generation":0,"state":"completed","updated_at_ns":595434142274283},"owner_epoch":"kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41","record_kind":"state_observed","result":null,"runtime_scale_status":{"effective_scale":1,"operation_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participants":[{"accepted_generation":0,"acknowledgement_sequence":1888,"acknowledgement_source_epoch":"native-runtime-ab1ab108c01811d071cbf157d05eef41","acknowledgement_timestamp_ns":595434139380760,"active_control_configuration_generation":1,"active_control_configuration_id":"active.i6.native.arm","applied_scale":1,"capability_id":"execution.trajectory.execute","command_target_id":"arm","commanded_scale":1,"control_binding_id":"lite6_robot.arm.execution-trajectory-execute","control_binding_revision":"sha256:8e3f32bc78e641e62fc0b9902b615e6a3ebe7626d83eb39e81efd9ca3c2425e6","control_domain_id":"lite6_robot.arm.control","control_domain_revision":"sha256:669ef8a3882b5167bc6e94e6c253f72e88a511990821348f46b3ca2c6cb2937c","control_route_id":"arm_position","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","controller_topology_id":"manyforge.ros2_control_arm_direct","coordinated_barrier_id":null,"error":null,"participant_id":"operation.kernel-owner-native-runtime-ab1ab108c01811d071cbf157d05eef41.2.participant.1","resources":["lite6_robot.arm.coordinate.000000","lite6_robot.arm.coordinate.000001","lite6_robot.arm.coordinate.000002","lite6_robot.arm.coordinate.000003","lite6_robot.arm.coordinate.000004","lite6_robot.arm.coordinate.000005"],"scaling_domain_id":"lite6_robot.arm.runtime_scale","state":"pending","supported":true,"topology_revision":"sha256:299f321da75dc2279ca2cebf66887ae13fb365a8a9db07d5c93d8aae6303352a","virtual_trajectory_time_s":2.533308666}],"requested_scale":1,"runtime_scale_generation":0,"state":"pending","updated_at_ns":595434145763632}})native40");
}

// Re-pin the captured factual participant through the CURRENT typed journal
// producer. The old raw record is provenance/input only, never a v8 wire alias.
core::OperationJournalEventDTO NativeCurrentIoRuleEvent(const TemporaryDirectory& directory) {
  const auto input = Native40IoRuleRecord();
  const auto& original = Member(input, "operation_state");
  const auto& row = Array(original, "participants").front();
  const auto& fact = Member(row, "terminal_fact");
  if (Text(original, "state") != "completed" || Text(row, "state") != "completed" ||
      Text(row, "payload_kind") != "motion" || Text(fact, "kind") != "computed_completion" ||
      Text(fact, "effect") != "effect_settled" || !Member(fact, "error").is_null() ||
      !Member(row, "terminal_error").is_null() ||
      !Member(row, "capability_provider_binding_id").is_null() ||
      !Member(row, "capability_provider_binding_revision").is_null())
    throw std::logic_error("captured native40 factual fixture changed shape");
  const auto outcomes = core::DecodeIoRuleOutcomes(
      content_identity::canonical_json(Member(row, "io_rule_outcomes")));
  if (!outcomes.ok()) throw std::logic_error(outcomes.error().message);
  std::vector<core::ResourceId> resources;
  for (const auto& resource : Array(row, "resources")) resources.emplace_back(resource.as_string());
  core::OperationParticipantStateDTO participant{
      core::OperationParticipantId(Text(row, "participant_id")),
      core::ControllerGoalId(Text(row, "controller_goal_id")),
      core::ControllerGoalGeneration(Unsigned(row, "controller_goal_generation")),
      core::ControllableComponentId(Text(row, "controllable_component_id")),
      core::CommandTargetId(Text(row, "command_target_id")),
      core::CapabilityId(Text(row, "capability_id")),
      core::ParticipantBindingIdentityDTO{
          core::ControlDomainId(Text(row, "control_domain_id")),
          core::ControlDomainRevision(Text(row, "control_domain_revision")),
          core::ControllerParticipantBindingIdentityDTO{
              core::ControlBindingId(Text(row, "control_binding_id")),
              core::ControlBindingRevision(Text(row, "control_binding_revision")), std::nullopt}},
      core::OperationParticipantPayloadKind::kMotion, core::OperationStateKind::kCompleted,
      Member(row, "progress").as_number(), resources, std::nullopt};
  participant.execution_scope = core::ProgramNodeExecutionScopeDTO{
      core::ProgramNodeInstanceId(Text(Member(row, "execution_scope"), "node_instance_id")),
      Text(Member(row, "execution_scope"), "node_attempt_id")};
  participant.terminal_fact = core::ParticipantExecutionFact{
      core::ParticipantExecutionFactKind::kComputedCompletion,
      core::ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
  participant.io_rule_outcomes = outcomes.value();
  core::OperationStateDTO state{
      core::OperationId(Text(original, "operation_id")),
      core::OperationRevision(Unsigned(original, "operation_revision")),
      core::ProgramBindingId(Text(original, "program_binding_id")),
      core::ProgramRunId(Text(original, "program_run_id")),
      core::OperationCoordinationPolicy::kIndependent, core::OperationStateKind::kCompleted,
      {participant}, content_identity::parse_nonnegative_int64(Member(original, "updated_at_ns")),
      std::nullopt, std::nullopt,
      core::RuntimeScaleGeneration(Unsigned(original, "runtime_scale_generation")),
      static_cast<std::uint64_t>(content_identity::parse_nonnegative_int64(
          Member(original, "journal_capture_order")))};
  const core::RequestOwnerEpoch owner(Text(input, "owner_epoch"));
  core::SqliteOperationJournal journal(directory.path() / "native-command-v8.sqlite3",
      core::CellId(std::string(kCellId)), 8U,
      [] { return "2026-10-18T08:17:51.987135216Z"; });
  const auto opened = journal.BeginOwnerEpoch(owner, 0, 8U, resources);
  if (!opened.ok()) throw std::runtime_error(opened.error().message);
  const auto reason = Text(Member(input, "intent"), "transition_reason");
  const auto recorded = journal.RecordOperationState(owner, state, nullptr, reason.c_str());
  if (!recorded.ok()) throw std::runtime_error(recorded.error().message);
  const auto page = journal.ReadOperationalEvents(owner, 0U);
  if (!page.ok()) throw std::runtime_error(page.error().message);
  if (page.value().events.size() != 1U) throw std::logic_error("current typed fixture has no unique event");
  return page.value().events.front();
}

TEST(HeadlessApiCore, NativeV8IoRuleRecordKeepsTypedProducerEnvelopeAndParticipantEvidence) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto event = NativeCurrentIoRuleEvent(directory);
  const auto payload = content_identity::parse_canonical_json(event.payload_json);
  ASSERT_EQ(Text(payload, "api_version"), "manyforge.operation_journal_record.v8");
  const auto epoch = Text(payload, "owner_epoch");
  const auto operation = Text(Member(payload, "operation_state"), "operation_id");
  const auto ingest = [&](const CanonicalValue& value, const std::string& digest) {
    return core.IngestOperationEvent(
        epoch, event.sequence, event.record_kind, std::nullopt, operation, 1U,
        event.occurred_at_ns, event.occurred_at_wall, event.effect_pending, value, digest);
  };
  const auto digest = content_identity::sha256_digest(content_identity::canonical_json(payload));
  ASSERT_EQ(digest, event.payload_digest);
  const auto& common = Array(Member(payload, "operation_state"), "participants").front();
  EXPECT_EQ(common.find_object_member("controller_goal_id"), nullptr);
  EXPECT_EQ(common.find_object_member("controller_goal_generation"), nullptr);
  EXPECT_EQ(Text(common, "command_id"), Text(Array(Member(Native40IoRuleRecord(), "operation_state"), "participants").front(), "controller_goal_id"));
  EXPECT_THROW(static_cast<void>(ingest(payload, "sha256:" + std::string(64U, '0'))), OperatorAlarmError);
  for (const std::string old_version : {"v1", "v2", "v3", "v4", "v5", "v6", "v7", "v9"}) {
    const auto invalid_version = JournalField(payload, "api_version", CanonicalValue("manyforge.operation_journal_record." + old_version));
    EXPECT_THROW(static_cast<void>(ingest(invalid_version, content_identity::sha256_digest(
                        content_identity::canonical_json(invalid_version)))), OperatorAlarmError);
  }
  const auto& state = Member(payload, "operation_state");
  const auto& participant = Array(state, "participants").front();
  const auto obsolete_identity = JournalField(payload, "operation_state", JournalField(
      state, "participants", CanonicalValue(CanonicalValue::Array{JournalField(
          participant, "controller_goal_id", Member(participant, "command_id"))})));
  EXPECT_THROW(static_cast<void>(ingest(obsolete_identity, content_identity::sha256_digest(
      content_identity::canonical_json(obsolete_identity)))), OperatorAlarmError);
  for (const bool relabel : {false, true}) {
    auto altered = participant;
    if (relabel) {
      altered = JournalField(altered, "participant_id", CanonicalValue("foreign.participant"));
    } else {
      auto outcomes = Array(participant, "io_rule_outcomes");
      outcomes.front() = JournalField(outcomes.front(), "terminal_cause", CanonicalValue("invented_cause"));
      altered = JournalField(altered, "io_rule_outcomes", CanonicalValue(std::move(outcomes)));
    }
    const auto invalid = JournalField(payload, "operation_state", JournalField(
        state, "participants", CanonicalValue(CanonicalValue::Array{altered})));
    EXPECT_THROW(static_cast<void>(ingest(invalid, content_identity::sha256_digest(
                        content_identity::canonical_json(invalid)))), OperatorAlarmError);
  }
  // The one current common-row format permits absent optional extensions.
  // Its actual command identity and terminal/capture checks still apply.
  auto without_rules = participant.as_object();
  std::erase_if(without_rules, [](const auto& field) { return field.first == "io_rule_outcomes"; });
  const auto no_extension = JournalField(payload, "operation_state", JournalField(
      state, "participants", CanonicalValue(CanonicalValue::Array{CanonicalValue(std::move(without_rules))})));
  TemporaryDirectory plain_directory;
  HeadlessApiCore plain(Config(plain_directory, "api.epoch.1"), Clocks());
  EXPECT_EQ(Text(plain.IngestOperationEvent(epoch, event.sequence, event.record_kind,
      std::nullopt, operation, 1U, event.occurred_at_ns, event.occurred_at_wall,
      event.effect_pending, no_extension, content_identity::sha256_digest(
          content_identity::canonical_json(no_extension))), "status"), "ingested");
  EXPECT_EQ(Text(ingest(payload, digest), "status"), "ingested");
  EXPECT_EQ(Text(ingest(payload, digest), "status"), "already_ingested");
  EXPECT_EQ(Unsigned(core.CompleteOperationEventSnapshot(epoch, event.sequence), "native_sequence"), event.sequence);

  // Contract127: an ACK/history successor retains immutable origin revision1.
  // Only chronology relaxes; no prepared/goal/participant identity is relabelled.
  TemporaryDirectory history_directory;
  HeadlessApiCore history_core(Config(history_directory, "api.epoch.1"), Clocks());
  const auto historical = JournalField(payload, "operation_state", JournalField(
      state, "operation_revision", CanonicalValue(2.0)));
  const auto ingest_history = [&](const CanonicalValue& value) {
    return history_core.IngestOperationEvent(
        epoch, 18U, "state_observed", std::nullopt, operation, 2U,
        595434142274284LL, "2026-10-18T08:17:51.987135217Z", false, value,
        content_identity::sha256_digest(content_identity::canonical_json(value)));
  };
  auto future_rules = Array(participant, "io_rule_outcomes");
  auto registration = Member(future_rules.front(), "registration");
  registration = JournalField(registration, "scope", JournalField(
      Member(registration, "scope"), "operation_revision", CanonicalValue(3.0)));
  future_rules.front() = JournalField(future_rules.front(), "registration", registration);
  auto future_participant = JournalField(participant, "io_rule_outcomes", CanonicalValue(future_rules));
  const auto future = JournalField(historical, "operation_state", JournalField(
      Member(historical, "operation_state"), "participants",
      CanonicalValue(CanonicalValue::Array{future_participant})));
  EXPECT_THROW(static_cast<void>(ingest_history(future)), OperatorAlarmError);
  const auto historical_wrong_goal = JournalField(historical, "operation_state", JournalField(
      Member(historical, "operation_state"), "participants", CanonicalValue(CanonicalValue::Array{
          JournalField(participant, "command_id", CanonicalValue("foreign.goal"))})));
  EXPECT_THROW(static_cast<void>(ingest_history(historical_wrong_goal)), OperatorAlarmError);
  EXPECT_EQ(Text(ingest_history(historical), "status"), "ingested");
  EXPECT_EQ(Text(ingest_history(historical), "status"), "already_ingested");
  EXPECT_EQ(Unsigned(history_core.CompleteOperationEventSnapshot(epoch, 18U), "native_sequence"), 18U);
}

} // namespace manyforge::headless


// EOF-only companion to the shared Headless fixture. Use the actual Core
// journal producer and scope-digest constructors, not a version-edited payload.
#include <manyforge_core/stop_scope_resolver.hpp>

namespace manyforge::headless {
TEST(HeadlessApiCore,
     ActualSqliteStopV8ProviderProjectionPreservesProofAndRejectsMalformedIdentity) {
  namespace native = manyforge::core;
  TemporaryDirectory directory;
  const native::RequestOwnerEpoch owner("execution-host.epoch.stop.provider");
  const native::OperationId operation("operation.output.stop");
  const native::ProgramRunId run("run.output");
  const native::ControllableComponentId component("station_io");
  const native::ResourceId output("io.output.station.lamp");
  const native::StopId stop("stop.output");
  const native::StopGeneration generation(2U);
  const native::FullStopIntentDTO intent{
      {native::RequestId("request.output.stop"), owner, "journal.fixture", std::nullopt},
      native::StopGeneration(1U)};
  // Logical output membership is explicit; no controller binding or physical
  // resource is invented for this provider-only affected participant.
  const native::StopScopeResolverSnapshotDTO snapshot{
      owner, native::GraphRevision("graph.output"), 1U, native::StopGeneration(1U),
      {{operation, run, native::OperationRevision(1U), native::StopGeneration(1U),
        native::OperationStateKind::kExecuting, true, false, {component}, {output}}},
      {{component, native::ComponentLifecycleGeneration(1U), {output}, {output}}},
      {{output, native::StopGeneration(1U), false}}, {run}};
  const auto resolved = native::resolve_full_stop_scope(intent, snapshot);
  ASSERT_TRUE(resolved.ok()) << resolved.error().message;
  EXPECT_TRUE(resolved.value().full_stop);
  EXPECT_EQ(resolved.value().resources, std::vector<native::ResourceId>{output});
  const auto accepted = native::MakeStopAccepted(
      intent, stop, generation, resolved.value(),
      {{operation, native::OperationRevision(2U)}}, 100);
  ASSERT_TRUE(accepted.ok()) << accepted.error().message;
  const auto normalized = native::ComputeNormalizedStopIntentDigest(intent);
  const auto selector = native::ComputeRequestedStopSelectorDigest(intent);
  const auto captured = native::ComputeCapturedStopScopeDigest(resolved.value());
  ASSERT_TRUE(normalized.ok());
  ASSERT_TRUE(selector.ok());
  ASSERT_TRUE(captured.ok());
  EXPECT_EQ(accepted.value().requested_selector_digest, selector.value());
  EXPECT_EQ(accepted.value().captured_scope_digest, captured.value());
  native::OperationParticipantStateDTO participant{
      native::OperationParticipantId("participant.output"),
      native::ControllerGoalId("correlation.output"), native::ControllerGoalGeneration(1U),
      component, native::CommandTargetId("lamp"), native::CapabilityId("io.signal.write"),
      native::ParticipantBindingIdentityDTO{
          native::ControlDomainId("domain.output"),
          native::ControlDomainRevision("domain.output.revision"),
          native::CapabilityProviderBindingIdentityDTO{
              native::CapabilityProviderBindingId("provider.output"),
              native::CapabilityProviderBindingRevision("provider.output.revision")}},
      native::OperationParticipantPayloadKind::kCapabilityCommand,
      native::OperationStateKind::kStopping, 0.0, {output}, std::nullopt};
  participant.output_evidence = native::IoOutputEvidence::kSubmitted;
  const native::OperationStateDTO state{
      operation, native::OperationRevision(2U), native::ProgramBindingId("binding.output"), run,
      native::OperationCoordinationPolicy::kIndependent, native::OperationStateKind::kStopping,
      {participant}, 100, stop, generation, std::nullopt};
  const native::EmergencyStopAcceptanceRecordDTO record{
      owner, intent.request_context.request_id, normalized.value(), std::nullopt,
      intent.attribution, native::RequestReplayStatusDTO::kAcceptedNew, stop, generation, 100,
      accepted.value().resolution_disposition, captured.value(),
      native::EmergencyStopEntryStateDTO::kActive,
      native::EmergencyStopAcceptancePathDTO::kFreshReserved,
      native::EmergencyStopProjectionStateDTO::kPending};
  native::SqliteOperationJournal journal(
      directory.path() / "native-stop-v8.sqlite3", native::CellId(std::string(kCellId)), 8U,
      [] { return "2026-10-18T10:00:00.000000001Z"; });
  ASSERT_TRUE(journal.BeginOwnerEpoch(owner, 0, 8U, {output}).ok());
  // These are typed accepted-Stop persistence facts, not a claim of physical
  // stopping, certificate completion, provider acceptance, or a Kernel trip.
  const auto recorded = journal.RecordStopAccepted(
      owner, record, native::EmergencyStopProjectedIntentDTO{intent}, accepted.value(),
      native::EmergencyStopAffectedStateSnapshotDTO{{state}, {output}});
  ASSERT_EQ(recorded.projection_state, native::EmergencyStopProjectionStateDTO::kRecorded)
      << (recorded.error ? recorded.error->message : "missing recorded projection");
  ASSERT_TRUE(recorded.journal_sequence);
  const auto replayed = journal.RecordStopAccepted(
      owner, record, native::EmergencyStopProjectedIntentDTO{intent}, accepted.value(),
      native::EmergencyStopAffectedStateSnapshotDTO{{state}, {output}});
  EXPECT_EQ(replayed, recorded);
  const auto page = journal.ReadOperationalEvents(owner, 0U);
  ASSERT_TRUE(page.ok()) << page.error().message;
  ASSERT_EQ(page.value().events.size(), 1U);
  const auto& event = page.value().events.front();
  EXPECT_EQ(event.sequence, *recorded.journal_sequence);
  EXPECT_EQ(event.record_kind, "stop_acceptance_projected");
  EXPECT_EQ(event.owner_epoch, owner);
  ASSERT_TRUE(event.request_id);
  EXPECT_EQ(*event.request_id, intent.request_context.request_id);
  EXPECT_FALSE(event.operation_id);
  EXPECT_FALSE(event.operation_revision);
  EXPECT_TRUE(event.effect_pending);
  EXPECT_EQ(event.payload_digest, content_identity::sha256_digest(event.payload_json));
  const auto payload = content_identity::parse_canonical_json(event.payload_json);
  EXPECT_EQ(Text(payload, "api_version"), "manyforge.stop_acceptance_projection.v8");
  EXPECT_EQ(Text(Member(payload, "acceptance"), "normalized_request_digest"),
            normalized.value().value());
  EXPECT_EQ(Text(Member(payload, "accepted"), "requested_selector_digest"),
            selector.value().value());
  EXPECT_EQ(Text(Member(payload, "accepted"), "captured_scope_digest"),
            captured.value().value());
  const auto& actual = Array(Array(Member(payload, "affected_state"), "operations").front(),
                             "participants").front();
  EXPECT_TRUE(Member(actual, "control_binding_id").is_null());
  EXPECT_TRUE(Member(actual, "control_binding_revision").is_null());
  EXPECT_EQ(Text(actual, "capability_provider_binding_id"), "provider.output");
  EXPECT_EQ(Text(actual, "output_evidence"), "submitted");
  EXPECT_EQ(Text(actual, "command_id"), participant.command_id.value());
  EXPECT_EQ(Unsigned(actual, "command_generation"), participant.command_generation.value());
  EXPECT_EQ(actual.find_object_member("controller_goal_id"), nullptr);
  EXPECT_EQ(actual.find_object_member("controller_goal_generation"), nullptr);

  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto ingest = [&](const CanonicalValue& value, const std::string& digest) {
    return core.IngestOperationEvent(
        owner.value(), event.sequence, event.record_kind, event.request_id->value(),
        std::nullopt, std::nullopt, event.occurred_at_ns, event.occurred_at_wall,
        event.effect_pending, value, digest);
  };
  // Try invalid but correctly hashed envelopes before any valid ingestion, so
  // refusal cannot be explained by an already-consumed sequence identity.
  for (const auto& invalid : {
           JournalField(payload, "api_version",
                        CanonicalValue("manyforge.stop_acceptance_projection.v9")),
           JournalField(payload, "acceptance", JournalField(
               Member(payload, "acceptance"), "owner_epoch", CanonicalValue("foreign.owner"))),
           JournalField(payload, "accepted", JournalField(
               Member(payload, "accepted"), "stop_id", CanonicalValue("foreign.stop")))}) {
    EXPECT_THROW(static_cast<void>(ingest(invalid, content_identity::sha256_digest(
                        content_identity::canonical_json(invalid)))), OperatorAlarmError);
  }
  auto invalid_participant = JournalField(
      actual, "capability_provider_binding_revision", CanonicalValue(nullptr));
  const auto& affected = Member(payload, "affected_state");
  const auto& operation_row = Array(affected, "operations").front();
  const auto invalid_provider = JournalField(payload, "affected_state", JournalField(
      affected, "operations", CanonicalValue(CanonicalValue::Array{JournalField(
          operation_row, "participants", CanonicalValue(CanonicalValue::Array{invalid_participant}))})));
  EXPECT_THROW(static_cast<void>(ingest(invalid_provider, content_identity::sha256_digest(
                      content_identity::canonical_json(invalid_provider)))), OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(ingest(payload, Digest('0'))), OperatorAlarmError);
  EXPECT_EQ(Text(ingest(payload, event.payload_digest), "status"), "ingested");
  EXPECT_EQ(Text(ingest(payload, event.payload_digest), "status"), "already_ingested");
  EXPECT_EQ(Unsigned(core.CompleteOperationEventSnapshot(owner.value(), event.sequence),
                     "native_sequence"), event.sequence);
  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("stop_acceptance_projected");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").empty() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::yield();
  const auto history = core.HistoryPage(query);
  ASSERT_EQ(Array(history, "events").size(), 1U);
  const auto& saved = Member(Member(Array(history, "events").front(), "event"), "payload");
  EXPECT_EQ(Text(saved, "payload_digest"), event.payload_digest);
  EXPECT_EQ(Member(saved, "journal_payload"), payload);
}

TEST(HeadlessApiCore,
     SealedLogicalOutputsJoinAlarmRegistryAndInputOnlyAddsNoResources) {
  TemporaryDirectory directory;
  const auto package = directory.path() / "io-package";
  ASSERT_TRUE(std::filesystem::create_directory(package));
  const auto output = deployment::test::materialize_boolean_io_retained_fixture(
                          package, true, true)
                          .bundle;
  const auto input = deployment::test::materialize_boolean_io_retained_fixture(
                         package, true, false)
                         .bundle;
  ASSERT_TRUE(output);
  ASSERT_TRUE(input);
  ASSERT_TRUE(output->value().cell_graph.control_bindings.empty());
  ASSERT_TRUE(input->value().cell_graph.control_bindings.empty());
  const auto catalog =
      deployment::ResolveRetainedIoSignalCatalog(output->value().cell_graph);
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;
  std::set<std::string> outputs;
  for (const auto &row : catalog.value())
    if (row.descriptor.output_resource)
      outputs.insert(row.descriptor.output_resource->value());
  ASSERT_EQ(outputs.size(), 1U);
  const auto resource = *outputs.begin();
  const auto input_catalog =
      deployment::ResolveRetainedIoSignalCatalog(input->value().cell_graph);
  ASSERT_TRUE(input_catalog.ok()) << input_catalog.error().message;
  ASSERT_FALSE(input_catalog.value().empty());
  for (const auto &row : input_catalog.value())
    ASSERT_FALSE(row.descriptor.output_resource);
  auto config = Config(directory, "api.epoch.io-registry", package);
  config.cell_id = output->value().cell_graph.cell_id.value();
  const auto publish = [&](const deployment::RetainedBundle &bundle,
                           std::uint64_t sequence) {
    AtomicSourceSnapshotChannel manager(directory.publications(),
                                        "manager-source.json");
    static_cast<void>(manager.Publish(
        "component_manager", "component-manager." + config.cell_id,
        "manager.epoch.1", sequence, ManagerPayloadFor(bundle)));
  };
  publish(*output, 1U);
  HeadlessApiCore core(config, Clocks());
  const std::string epoch = "execution-host.epoch.io-registry";
  const CanonicalValue failure(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")}});
  const auto ingest = [&](const std::string &operation,
                          std::uint64_t sequence) {
    const auto payload =
        OperationJournalPayload(epoch, "state_observed", "failed", failure,
                                std::nullopt, resource, operation);
    EXPECT_EQ(
        Text(core.IngestOperationEvent(
                 epoch, sequence, "state_observed", std::nullopt, operation, 1U,
                 static_cast<std::int64_t>(100U + sequence),
                 "2026-10-18T10:00:00Z", false, payload,
                 content_identity::sha256_digest(
                     content_identity::canonical_json(payload))),
             "status"),
        "ingested");
    static_cast<void>(core.CompleteOperationEventSnapshot(epoch, sequence));
  };
  ingest("operation.io-output", 1U);
  EXPECT_TRUE(RefusedSubjects(core.AlarmSnapshot()).empty());
  const auto alarms = core.AlarmSnapshot();
  const auto &alarm = OnlyAlarm(alarms);
  EXPECT_EQ(Text(Member(alarm, "subject"), "subject_id"),
            "operation.io-output");
  EXPECT_EQ(Text(alarm, "lifecycle_state"), "active_unacknowledged");
  // A new input-only publication owns no logical output. Existing history
  // survives, while a new activation naming that removed output is refused.
  publish(*input, 2U);
  ingest("operation.output-after-removal", 2U);
  EXPECT_EQ(RefusedSubjects(core.AlarmSnapshot()),
            (std::set<std::string>{"operation.output-after-removal"}));
}

TEST(HeadlessApiCore,
     ClearingOneComponentConditionCannotClearAnotherOrTheCellIncidentLatch) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &bundle = *configurations.dual;
  ASSERT_EQ(bundle.value().cell_graph.controllable_components.size(), 2U);
  const auto first = bundle.value()
                         .cell_graph.controllable_components[0]
                         .controllable_component_id.value();
  const auto second = bundle.value()
                          .cell_graph.controllable_components[1]
                          .controllable_component_id.value();
  const auto resource_for = [&](const std::string &component) {
    for (const auto &binding : bundle.value().cell_graph.control_bindings)
      if (binding.controllable_component_id.value() == component &&
          !binding.resource_bindings.empty())
        return binding.resource_bindings.front().resource_id.value();
    throw std::logic_error("actual component has no retained resource");
  };
  PublishActiveConfiguration(directory, bundle, 1U);
  HeadlessApiCore core(
      RegistryConfig(directory, "api.epoch.incident-isolation"), Clocks());
  const auto row = [&](const std::string &component, const char *cause) {
    return InConfiguration(
        ComponentConditionSnapshot(component, resource_for(component), "active",
                                   "satisfied", cause),
        bundle);
  };
  const auto observe = [&](const char *first_cause) {
    return core.ObserveComponentLifecycleConditions(
        "cclc.epoch.incident-isolation",
        CanonicalValue(CanonicalValue::Array{row(first, first_cause),
                                             row(second, "fault")}),
        {});
  };
  ASSERT_EQ(Text(observe("fault"), "status"), "observed");
  const std::string epoch = "execution-host.epoch.incident-isolation";
  static_cast<void>(
      core.CompleteOperationEventSnapshot(epoch, 0U, CurrentStopFaultLatch()));
  const auto before = core.AlarmSnapshot();
  const auto *cell = AlarmFor(before, "manyforge.motion.stop_fault");
  ASSERT_NE(cell, nullptr);
  const auto cell_activation = Text(*cell, "activation_id");
  EXPECT_EQ(Text(Member(*cell, "subject"), "subject_id"), kCellId);

  ASSERT_EQ(Text(observe("none"), "status"), "observed");
  const auto after = core.AlarmSnapshot();
  std::set<std::string> active_components;
  for (const auto &alarm : Array(after, "alarms")) {
    if (Text(alarm, "definition_id") == kComponentLifecycleFault &&
        std::get<bool>(Member(alarm, "active").storage()))
      active_components.insert(Text(Member(alarm, "subject"), "subject_id"));
  }
  EXPECT_EQ(active_components, (std::set<std::string>{second}));
  cell = AlarmFor(after, "manyforge.motion.stop_fault");
  ASSERT_NE(cell, nullptr);
  EXPECT_EQ(Text(*cell, "activation_id"), cell_activation);
  EXPECT_TRUE(std::get<bool>(Member(*cell, "active").storage()));
}

} // namespace manyforge::headless

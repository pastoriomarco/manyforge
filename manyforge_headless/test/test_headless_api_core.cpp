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
#include <iomanip>
#include <iterator>
#include <map>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_core/io_rule_outcome_codec.hpp>
#include <manyforge_core/types/io_rule_state.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
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
#include "manager_status_frames.hpp"

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
    std::filesystem::create_directory(artifacts());
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }
  [[nodiscard]] std::filesystem::path artifacts() const {
    return path_ / "artifacts";
  }
  [[nodiscard]] std::filesystem::path history_directory() const {
    return path_;
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

// A caller with a distinct 32-hex request key per label.
AlarmCallerAttribution Caller(const std::string &label = "request.1") {
  return {manyforge::content_identity::sha256_digest(label).substr(7U, 32U),
          "operator_ui",
          "test",
          "manyforge.test.alarm",
          "authenticated:test",
          "principal.operator"};
}

HeadlessApiCoreConfig
Config(const TemporaryDirectory &directory, std::string application_start_id,
       std::filesystem::path package_root =
           MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT) {
  return {
      std::move(package_root),
      directory.artifacts(),
      MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
      MANYFORGE_HEADLESS_TEST_NODE_ENVIRONMENT_MANIFEST,
      MANYFORGE_HEADLESS_TEST_ALARM_CATALOG,
      directory.history_directory(),
      std::string(kCellId),
      "headless-api.cell",
      std::move(application_start_id),
      RuntimeSnapshotFreshnessPolicy{10'000'000'000LL},
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
      {"application_start_id", CanonicalValue("3f9a0000000000a1")},
      // Class 2 keys (run 1, attempt 1, two model-attempt phases; 1.15.12).
      {"model_attempt_id",
       CanonicalValue(success ? "80000000000000010000000000010101"
                              : "80000000000000010000000000010102")},
      {"model_endpoint_alias", CanonicalValue("planning_model")},
      {"model_id", CanonicalValue("model-a")},
      {"model_version", CanonicalValue(success ? "model-a.2026-08" : "")},
      {"node_attempt_id", CanonicalValue(1.0)},
      {"node_instance_id", CanonicalValue("query-model")},
      {"occurred_at_ns", CanonicalValue(success ? 1200.0 : 1300.0)},
      {"occurred_at_wall", CanonicalValue(success ? "2026-08-10T10:00:00Z"
                                                  : "2026-08-10T10:00:01Z")},
      {"outcome", CanonicalValue(success ? "success" : "timeout")},
      {"program_run_id", CanonicalValue(1.0)},
      {"prompt_digest", CanonicalValue(Digest('3'))},
      {"prompt_text", CanonicalValue(nullptr)},
      {"prompt_template_digest", CanonicalValue(Digest('2'))},
      {"prompt_template_id", CanonicalValue("select")},
      {"prompt_template_version", CanonicalValue("1")},
      {"response_digest", std::move(response_digest)},
      {"response_value", CanonicalValue(nullptr)},
      {"schema_version", CanonicalValue("manyforge.program_model_attempt.v2")},
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

// Request keys of the test Stops: 32 lowercase hex (contract 1.4).
constexpr char kStopRequestKey[] = "40000000000000010000000000000001";
constexpr char kCollisionStopRequestKey[] = "40000000000000030000000000000001";

// Contract 1.5 integer objects of a participant and its controller goal.
CanonicalValue ParticipantIdValue(const std::uint64_t operation_id,
                                  const std::uint32_t ordinal = 1U) {
  return CanonicalValue(CanonicalValue::Object{
      {"operation_id", CanonicalValue(static_cast<double>(operation_id))},
      {"ordinal", CanonicalValue(static_cast<double>(ordinal))}});
}

CanonicalValue CommandIdValue(const std::uint64_t operation_id,
                              const std::uint32_t ordinal = 1U,
                              const std::uint32_t generation = 1U) {
  return CanonicalValue(CanonicalValue::Object{
      {"generation", CanonicalValue(static_cast<double>(generation))},
      {"operation_id", CanonicalValue(static_cast<double>(operation_id))},
      {"ordinal", CanonicalValue(static_cast<double>(ordinal))}});
}

CanonicalValue OperationJournalPayload(
    const std::string &record_kind,
    const std::string &state, const CanonicalValue &terminal_error,
    const std::optional<std::string> &control = std::nullopt,
    const std::string &resource = "resource.arm.a",
    const std::uint64_t operation_id = 1U) {
  const CanonicalValue result = control.has_value()
                                    ? CanonicalValue(CanonicalValue::Object{
                                          {"control", CanonicalValue(*control)},
                                      })
                                    : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v10")},
      {"intent", CanonicalValue(CanonicalValue::Object{})},
      {"operation_state",
       CanonicalValue(CanonicalValue::Object{
           {"operation_id", CanonicalValue(static_cast<double>(operation_id))},
           {"operation_revision", CanonicalValue(1.0)},
           {"participants",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"command_id", CommandIdValue(operation_id)},
                    {"participant_id", ParticipantIdValue(operation_id)},
                    {"resources", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue(resource)})},
                    {"terminal_error", terminal_error},
                })})},
           {"program_binding_id", CanonicalValue("program-binding.test")},
           {"program_run_id", CanonicalValue(1.0)},
           {"state", CanonicalValue(state)},
           {"stop_event_id", CanonicalValue(nullptr)},
       })},
      {"record_kind", CanonicalValue(record_kind)},
      {"result", result},
      {"runtime_scale_status", CanonicalValue(nullptr)},
  });
}

// One Stop event's acceptance row as the Kernel writes it (payload
// `manyforge.stop_event.v1`, stop_event_codec): it covers participant 1 of
// operation 1.
CanonicalValue StopEventAcceptedPayload(
    const std::string &requested = "stop_all", const std::string &reason = "",
    const std::uint64_t event_id = 1U,
    const std::string &request_key = kStopRequestKey,
    const std::int64_t accepted_at_ns = 100) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", CanonicalValue(static_cast<double>(accepted_at_ns))},
      {"applied", CanonicalValue(requested)},
      {"attribution", CanonicalValue(CanonicalValue::Object{
                          {"assurance", CanonicalValue("unverified")},
                          {"principal_id", CanonicalValue(nullptr)},
                          {"transport", CanonicalValue("internal")},
                      })},
      {"caused_by", CanonicalValue(nullptr)},
      {"covered",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"operation_id", CanonicalValue(1.0)},
               {"participant_ordinal", CanonicalValue(1.0)},
               {"started", CanonicalValue(true)},
           }),
       })},
      {"event_id", CanonicalValue(static_cast<double>(event_id))},
      {"fence_degraded", CanonicalValue(nullptr)},
      {"group_count", CanonicalValue(0.0)},
      {"groups", CanonicalValue(CanonicalValue::Array{})},
      {"hold_change", CanonicalValue(CanonicalValue::Array{})},
      {"hold_revision_after", CanonicalValue(0.0)},
      {"program_run_id", CanonicalValue(nullptr)},
      {"reason", CanonicalValue(reason)},
      {"request_key", CanonicalValue(request_key)},
      {"requested", CanonicalValue(requested)},
      {"row", CanonicalValue("accepted")},
      {"schema_version", CanonicalValue("manyforge.stop_event.v1")},
      {"source_label", CanonicalValue("operator.test")},
      {"stale_diagnostics", CanonicalValue(CanonicalValue::Array{})},
      {"stale_total", CanonicalValue(0.0)},
      {"widen_reason", CanonicalValue("none")},
  });
}

// The completion row of the same Stop event.
CanonicalValue StopEventCompletedPayload(const std::string &result = "stopped",
                                         const std::uint64_t event_id = 1U,
                                         const std::int64_t accepted_at_ns = 100,
                                         const std::int64_t completed_at_ns = 250) {
  return CanonicalValue(CanonicalValue::Object{
      {"accepted_at_ns", CanonicalValue(static_cast<double>(accepted_at_ns))},
      {"applied", CanonicalValue("stop_all")},
      {"completed_at_ns", CanonicalValue(static_cast<double>(completed_at_ns))},
      {"covered",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"containing_stage_index", CanonicalValue(0.0)},
               {"containing_stage_kind", CanonicalValue("nominal")},
               {"error_code", result == "some_failed"
                                  ? CanonicalValue("execution_aborted")
                                  : CanonicalValue(nullptr)},
               {"escalated", CanonicalValue(result == "stopped_escalated")},
               {"operation_id", CanonicalValue(1.0)},
               {"participant_ordinal", CanonicalValue(1.0)},
               {"phase", CanonicalValue(result == "some_failed" ? "failed"
                                                                : "stopped")},
               {"started", CanonicalValue(true)},
               {"stop_path_collision", CanonicalValue(false)},
           }),
       })},
      {"event_id", CanonicalValue(static_cast<double>(event_id))},
      {"result", CanonicalValue(result)},
      {"row", CanonicalValue("completed")},
      {"schema_version", CanonicalValue("manyforge.stop_event.v1")},
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
                   {"apply_id", CanonicalValue(1.0)},
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

// The runtime monitor's Stop (stop state model S14): event 2 of kind
// `runtime_collision` covering operation 1, with no Program run.
CanonicalValue RuntimeCollisionStopEventPayload(
    const std::string &reason = "runtime_collision_fault") {
  return WithField(StopEventAcceptedPayload("runtime_collision", reason, 2U,
                                            kCollisionStopRequestKey),
                   "source_label", CanonicalValue("kernel.runtime_collision"));
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
      participant = JournalField(participant, "participant_id", ParticipantIdValue(1U));
    participant = JournalField(participant, "execution_scope", CanonicalValue(CanonicalValue::Object{
        {"node_instance_id", CanonicalValue("move")},
        {"node_attempt_id", CanonicalValue(1.0)}}));
  }
  return JournalField(JournalField(state, "participants", CanonicalValue(std::move(participants))),
                   "journal_capture_order", CanonicalValue("1"));
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

CanonicalValue CapturedOperation(const std::string &resource,
                                  const CanonicalValue &error) {
  auto payload = OperationJournalPayload("state_observed", "intervention_required",
                                         error, std::nullopt, resource);
  return JournalField(JournalField(payload, "operation_state",
                              CapturedState(Member(payload, "operation_state"))), "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v10"));
}
// The operation's state row the Kernel writes after a Stop event row
// (`state_observed`, intent.transition_reason `stop_accepted`,
// `stop_participant_completed` or `stop_terminal_committed`): Stop event
// `stop_event_id` covers its one participant, which holds `resource`.
CanonicalValue StopStateRow(const std::string &transition_reason,
                            const std::string &state,
                            const std::string &resource,
                            const std::uint64_t stop_event_id = 2U) {
  auto payload = OperationJournalPayload("state_observed", state,
                                         CanonicalValue(nullptr), std::nullopt,
                                         resource);
  payload = JournalField(payload, "intent",
                         CanonicalValue(CanonicalValue::Object{
                             {"transition_reason", CanonicalValue(transition_reason)}}));
  auto operation = Member(payload, "operation_state");
  auto participants = Member(operation, "participants").as_array();
  participants.front() = JournalField(
      participants.front(), "stop",
      CanonicalValue(CanonicalValue::Object{
          {"attempt", CanonicalValue(1.0)},
          {"attribution_class", CanonicalValue("program_own")},
          {"containing_stage_id", CanonicalValue(nullptr)},
          {"containing_stage_index", CanonicalValue(nullptr)},
          {"escalated", CanonicalValue(false)},
          {"mode", CanonicalValue("fault")},
          {"phase", CanonicalValue(state == "stopped" ? "stopped" : "stopping")},
          {"plan_digest", CanonicalValue(nullptr)},
          {"stop_event_id", CanonicalValue(static_cast<double>(stop_event_id))},
      }));
  operation = JournalField(operation, "participants",
                           CanonicalValue(std::move(participants)));
  operation = JournalField(operation, "stop_event_id",
                           CanonicalValue(static_cast<double>(stop_event_id)));
  return JournalField(payload, "operation_state", operation);
}

void IngestJournalEvent(HeadlessApiCore &core, const CanonicalValue &payload,
                        const std::uint64_t sequence) {
  // Stop event rows name neither a request key nor an operation.
  const auto *row = payload.find_object_member("row");
  const bool stop = row != nullptr;
  const auto kind =
      stop ? "stop_event_" + row->as_string() : Text(payload, "record_kind");
  const auto result = core.IngestOperationEvent(
      sequence, kind, std::nullopt,
      stop ? std::nullopt
           : std::optional<std::uint64_t>(
                 Unsigned(Member(payload, "operation_state"), "operation_id")),
      stop ? std::nullopt : std::optional<std::uint64_t>(1U), 300 + sequence,
      "2026-08-07T10:00:00.000000007Z", payload,
      content_identity::sha256_digest(
          content_identity::canonical_json(payload)));
  EXPECT_EQ(Text(result, "status"), "ingested");
}

// OD-205 (b): the runtime monitor's Stop as the operations table names it.
CanonicalValue RuntimeMonitorStop(
    const std::string &reason = "runtime_collision_fault") {
  return CanonicalValue(CanonicalValue::Object{
      {"reason", CanonicalValue(reason)},
      {"request_key", CanonicalValue(kCollisionStopRequestKey)},
      {"source_label", CanonicalValue("kernel.runtime_collision")},
  });
}

// The operations-table entry the Kernel lists for operation `state` (the
// encoding of `OperationTableValue`; within the table's bounds).
CanonicalValue OperationTableEntry(
    const CanonicalValue &state,
    CanonicalValue pending_control = CanonicalValue(nullptr),
    CanonicalValue runtime_monitor_stop = CanonicalValue(nullptr)) {
  std::set<std::string> union_of_resources;
  CanonicalValue::Array participants;
  const auto reason_of = [](const CanonicalValue &error) {
    const auto *details = error.find_object_member("details");
    const auto *reason = details ? details->find_object_member("reason") : nullptr;
    return reason ? *reason : CanonicalValue(nullptr);
  };
  for (const auto &participant : Array(state, "participants")) {
    for (const auto &resource : Array(participant, "resources"))
      union_of_resources.insert(resource.as_string());
    const auto *state_member = participant.find_object_member("state");
    const auto *error = participant.find_object_member("terminal_error");
    const bool has_error = error != nullptr && !error->is_null();
    const auto *scope = participant.find_object_member("execution_scope");
    CanonicalValue fact(nullptr);
    if (const auto *source = participant.find_object_member("terminal_fact");
        source != nullptr && !source->is_null()) {
      const auto &fact_error = Member(*source, "error");
      fact = CanonicalValue(CanonicalValue::Object{
          {"effect", Member(*source, "effect")},
          {"error", CanonicalValue(fact_error.is_null()                ? "none"
                                   : has_error && fact_error == *error ? "same"
                                                                      : "different")},
          {"kind", Member(*source, "kind")},
      });
    }
    participants.emplace_back(CanonicalValue::Object{
        {"execution_scope", CanonicalValue(scope != nullptr && !scope->is_null())},
        {"state", state_member ? *state_member : Member(state, "state")},
        {"terminal_error",
         has_error ? CanonicalValue(CanonicalValue::Object{{"reason", reason_of(*error)}})
                   : CanonicalValue(nullptr)},
        {"terminal_fact", std::move(fact)},
    });
  }
  CanonicalValue::Array resources;
  for (const auto &resource : union_of_resources) {
    if (resources.size() == 16U) break;
    resources.emplace_back(resource);
  }
  CanonicalValue terminal_error(nullptr);
  const auto &first = Array(state, "participants").front();
  if (const auto *error = first.find_object_member("terminal_error");
      error != nullptr && !error->is_null()) {
    terminal_error = CanonicalValue(CanonicalValue::Object{
        {"code", Member(*error, "code")},
        {"message", Member(*error, "message")},
        {"message_truncated", CanonicalValue(false)},
        {"reason", reason_of(*error)},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"complete", CanonicalValue(true)},
      {"operation_id", Member(state, "operation_id")},
      {"operation_revision", Member(state, "operation_revision")},
      {"participants", CanonicalValue(std::move(participants))},
      {"pending_control", std::move(pending_control)},
      {"program_binding_id", Member(state, "program_binding_id")},
      {"program_run_id", Member(state, "program_run_id")},
      {"resource_total", CanonicalValue(static_cast<double>(union_of_resources.size()))},
      {"resources", CanonicalValue(std::move(resources))},
      {"runtime_monitor_stop", std::move(runtime_monitor_stop)},
      {"state", Member(state, "state")},
      {"stop_event_id", Member(state, "stop_event_id")},
      {"terminal_error", std::move(terminal_error)},
  });
}

// One observation of the operations table with `entries` listed out of
// `total` (default: all of them).
CanonicalValue ObserveOperationTable(HeadlessApiCore &core,
                                     CanonicalValue::Array entries,
                                     std::optional<std::size_t> total = std::nullopt) {
  static std::uint64_t change = 0U;
  const auto count = total.value_or(entries.size());
  return core.ObserveOperationTable(CanonicalValue(CanonicalValue::Object{
      {"change", CanonicalValue(static_cast<double>(++change))},
      {"faulted", CanonicalValue(std::move(entries))},
      {"faulted_total", CanonicalValue(static_cast<double>(count))},
  }));
}

// The operation of a journal row, observed as the Kernel lists it: only
// while its state raises an alarm.
CanonicalValue ObserveOperationRow(HeadlessApiCore &core, const CanonicalValue &payload) {
  const auto &state = Member(payload, "operation_state");
  const auto name = Text(state, "state");
  const bool listed = name == "failed" || name == "interrupted" ||
                      name == "intervention_required";
  return ObserveOperationTable(
      core, listed ? CanonicalValue::Array{OperationTableEntry(state)} : CanonicalValue::Array{});
}

/// Feeds one core's mirror of the Manager's status stream (OD-216) as the
/// stream's follower does: the first frame is a snapshot, then whole
/// sections, each on a new tick. A section's generation rises only when its
/// payload changes, as the Manager's does.
class ManagerStatusFeed final {
public:
  explicit ManagerStatusFeed(HeadlessApiCore &core) : core_(core) {}

  void Section(const std::string_view name, const CanonicalValue &payload) {
    if (!connected_) {
      static_cast<void>(
          core_.ApplyManagerStatusFrame(test::StatusSnapshotFrame(++tick_)));
      connected_ = true;
    }
    auto &[generation, last] = sections_[std::string(name)];
    if (generation == 0U || !(last == payload)) {
      ++generation;
      last = payload;
    }
    static_cast<void>(core_.ApplyManagerStatusFrame(
        test::StatusSectionFrame(name, generation, payload, ++tick_)));
  }

private:
  HeadlessApiCore &core_;
  std::uint64_t tick_{};
  bool connected_{};
  std::map<std::string, std::pair<std::uint64_t, CanonicalValue>> sections_;
};

void PublishHealthySources(ManagerStatusFeed &status,
                           const std::string &scene_state,
                           const std::uint64_t scene_generation = 1U) {
  status.Section("manager", ManagerPayload());
  status.Section("logical_scene", LogicalScenePayload());
  status.Section("scene_runtime",
                 SceneRuntimePayload(scene_state, scene_generation));
}

void PublishManagerApplyTransition(
    ManagerStatusFeed &status,
    const std::string_view phase = "runtime_convergence_pending",
    const std::string_view scoped_component = "robot-a",
    const bool empty_predecessor = false) {
  status.Section("manager",
                 ManagerPayloadWithIncompleteApply(phase, scoped_component,
                                                   empty_predecessor));
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
                                     std::string application_start_id) {
  return Config(directory, std::move(application_start_id),
                MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT);
}

/// Manager publishes `bundle` as the active configuration.
void PublishActiveConfiguration(ManagerStatusFeed &status,
                                const deployment::RetainedBundle &bundle) {
  status.Section("manager", ManagerPayloadFor(bundle));
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

TEST(HeadlessApiCore, ComponentFrameLookupUsesVerifiedRetainedPaths) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto &bundle = *configurations.single;
  ASSERT_EQ(bundle.value().compiled_products.symbol_role_maps.size(), 1U);
  deployment::FilesystemVerifiedRetainedModelMapSource maps(
      MANYFORGE_HEADLESS_TEST_ALARM_REGISTRY_PACKAGE_ROOT, directory.artifacts());
  const auto model_map = maps.get_verified(deployment::RetainedModelMapQuery{
      bundle.retained_bundle_id(), bundle.value().compiled_products.symbol_role_maps.front().product_id});
  ASSERT_TRUE(model_map.has_value());
  const deployment::CompositionComponentModelFrame *frame = nullptr;
  for (const auto &path : model_map->frame_paths()) {
    frame = std::get_if<deployment::CompositionComponentModelFrame>(&path.terminal_frame);
    if (frame != nullptr) break;
  }
  ASSERT_NE(frame, nullptr);
  auto payload = ManagerPayloadFor(bundle);
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")}, {"apply_id", CanonicalValue(1.0)},
          {"apply_generation", CanonicalValue(1.0)}})}}));
  const CanonicalValue prepared(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.prepared_activation_input.v8")},
      {"retained_bundle_id", Member(active, "retained_bundle_id")},
      {"graph_revision", Member(active, "graph_revision")}});
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"apply_id", CanonicalValue(1.0)}, {"apply_generation", CanonicalValue(1.0)},
          {"terminal_state", CanonicalValue("succeeded")}, {"incomplete", CanonicalValue(false)},
          {"prepared_activation", prepared}})})}}));
  HeadlessApiCore api(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(api);
  PublishHealthySources(manager_status, "open");
  manager_status.Section("manager", payload);
  const auto request = [&](std::string component, std::string name) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.component_frame_lookup.v1")},
        {"retained_bundle_id", Member(active, "retained_bundle_id")},
        {"graph_revision", Member(active, "graph_revision")},
        {"frame", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(std::move(component))},
            {"frame_id", CanonicalValue(std::move(name))}})}});
  };
  const auto component = frame->controllable_component_id.value();
  const auto frame_id = frame->frame_role_id.value();
  const auto result = api.LookupComponentFrame(request(component, frame_id));
  EXPECT_TRUE(std::get<bool>(Member(result, "exists").storage()));
  EXPECT_EQ(Text(result, "schema_version"), "manyforge.component_frame_lookup_result.v1");
  EXPECT_EQ(Member(result, "frame"), Member(request(component, frame_id), "frame"));
  for (const auto &[bad_component, bad_frame, expected] :
       std::vector<std::tuple<std::string, std::string, std::string>>{
           {"not_present", frame_id, "component_reference_missing"},
           {component, "not_present", "frame_missing"}}) {
    try {
      static_cast<void>(api.LookupComponentFrame(request(bad_component, bad_frame)));
      FAIL() << "absent component or frame was accepted";
    } catch (const OperatorAlarmError &error) {
      EXPECT_EQ(error.code(), expected);
    }
  }
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
          JournalField(no_program, "program_revision", CanonicalValue("unexpected")));
  ASSERT_FALSE(inconsistent.ok());
  EXPECT_EQ(inconsistent.error().code, "program_applied_selection_invalid");
  EXPECT_EQ(inconsistent.error().message, "inconsistent no_program outcome");
}

TEST(HeadlessApiCore,
     AppliedStatusAndSelectionRejectDivergentPublicationBeforeEitherOutcome) {
  for (const auto kind : {"bound", "non_executable"}) {
    SCOPED_TRACE(kind);
    TemporaryDirectory directory;
    auto payload = ManagerPayload();
    auto active = Member(payload, "active");
    active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
        {"transaction", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("named_apply")},
            {"apply_id", CanonicalValue(2.0)},
            {"apply_generation", CanonicalValue(1.0)},
        })},
    }));
    payload = JournalField(payload, "active", active);
    payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
        {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
            {"apply_id", CanonicalValue(2.0)},
            {"apply_generation", CanonicalValue(1.0)},
            {"terminal_state", CanonicalValue("succeeded")},
            {"incomplete", CanonicalValue(false)},
            {"prepared_activation", CanonicalValue(CanonicalValue::Object{
                {"schema_version",
                 CanonicalValue("manyforge.prepared_activation_input.v8")},
                {"retained_bundle_id", CanonicalValue(Digest('f'))},
                {"graph_revision", Member(active, "graph_revision")},
                {"program_outcome", CanonicalValue(CanonicalValue::Object{
                    {"kind", CanonicalValue(kind)},
                })},
            })},
        })})},
    }));
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishHealthySources(manager_status, "open");
    manager_status.Section("manager", payload);
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
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue(3.0)},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  const CanonicalValue prepared(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.prepared_activation_input.v8")},
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
      {"deployment_revision", CanonicalValue(Digest('c'))},
      {"program_revision", CanonicalValue(nullptr)},
      {"program_outcome", CanonicalValue(CanonicalValue::Object{
                              {"kind", CanonicalValue("no_program")},
                          })},
  });
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"apply_id", CanonicalValue(3.0)},
          {"apply_generation", CanonicalValue(1.0)},
          {"terminal_state", CanonicalValue("succeeded")},
          {"incomplete", CanonicalValue(false)},
          {"intent_digest", CanonicalValue(Digest('d'))},
          {"prepared_activation", prepared},
      })})},
  }));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  manager_status.Section("manager", payload);
  const auto lookup = CanonicalValue(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.component_frame_lookup.v1")},
      {"retained_bundle_id", CanonicalValue(Digest('f'))},
      {"graph_revision", Member(active, "graph_revision")},
      {"frame", CanonicalValue(CanonicalValue::Object{
                    {"component_id", CanonicalValue("robot")},
                    {"frame_id", CanonicalValue("tcp")}})}});
  try {
    static_cast<void>(core.LookupComponentFrame(lookup));
    FAIL() << "stale retained deployment was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "component_frame_lookup_stale");
  }
  try {
    static_cast<void>(core.LookupComponentFrame(CanonicalValue(CanonicalValue::Object{})));
    FAIL() << "invalid lookup was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "component_frame_lookup_invalid");
  }
  const auto status = core.AppliedProgramStatus();
  EXPECT_TRUE(Member(status, "program_revision").is_null());
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
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue(4.0)},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  // v4 carried launch-time typed_parameter_overrides (removed).
  const CanonicalValue prepared(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.prepared_activation_input.v4")},
      {"retained_bundle_id", Member(active, "retained_bundle_id")},
      {"graph_revision", Member(active, "graph_revision")},
      {"program_revision", CanonicalValue(nullptr)},
      {"program_outcome", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("no_program")},
      })},
      {"typed_parameter_overrides", CanonicalValue(CanonicalValue::Object{})},
  });
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"apply_id", CanonicalValue(4.0)},
          {"apply_generation", CanonicalValue(1.0)},
          {"terminal_state", CanonicalValue("succeeded")},
          {"incomplete", CanonicalValue(false)},
          {"intent_digest", CanonicalValue(Digest('d'))},
          {"prepared_activation", prepared},
      })})},
  }));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  manager_status.Section("manager", payload);
  try {
    static_cast<void>(core.AppliedProgramStatus());
    FAIL() << "an Apply prepared before the override removal was read";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "program_applied_selection_invalid");
  }
}

namespace {

/// As the Manager publishes it: only the record of the Apply that installed
/// the active configuration carries its preparation receipt (OD-281).
CanonicalValue CommittedApplyRecord(const double apply_id,
                                    const std::string &digest,
                                    const std::string &scenario_id,
                                    const bool committed,
                                    const CanonicalValue &active,
                                    const bool installed_active) {
  auto record = CanonicalValue(CanonicalValue::Object{
      {"apply_id", CanonicalValue(apply_id)},
      {"apply_generation", CanonicalValue(1.0)},
      {"committed", CanonicalValue(committed)},
      {"terminal_state", CanonicalValue(committed ? "succeeded" : "failed")},
      {"incomplete", CanonicalValue(false)},
      {"intent_digest", CanonicalValue(digest)},
      {"scene_initialization", CanonicalValue("initialize")},
      {"prepared_activation", CanonicalValue(nullptr)},
  });
  if (!installed_active) {
    return record;
  }
  return JournalField(
      record, "prepared_activation",
      CanonicalValue(CanonicalValue::Object{
          {"schema_version",
           CanonicalValue("manyforge.prepared_activation_input.v8")},
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
          {"program_revision", CanonicalValue(nullptr)},
          {"program_outcome", CanonicalValue(CanonicalValue::Object{
                                  {"kind", CanonicalValue("no_program")},
                              })},
      }));
}

/// The active Apply, a refused one, and `older_applies` older committed
/// Applies the journal still retains (the first one of `other_cell`).
CanonicalValue CommittedActivationCore(TemporaryDirectory &directory,
                                       const double publication_generation,
                                       const std::size_t older_applies) {
  auto payload = ManagerPayload();
  auto active = Member(payload, "active");
  active = JournalField(active, "publication", CanonicalValue(CanonicalValue::Object{
      {"publication_generation", CanonicalValue(publication_generation)},
      {"transaction", CanonicalValue(CanonicalValue::Object{
          {"kind", CanonicalValue("named_apply")},
          {"apply_id", CanonicalValue(3.0)},
          {"apply_generation", CanonicalValue(1.0)},
      })},
  }));
  CanonicalValue::Array records{
      CommittedApplyRecord(3, Digest('d'), "cell_pick", true, active, true),
      CommittedApplyRecord(5, Digest('e'), "cell_pick", false,
                           active, false),
  };
  for (std::size_t index = 0U; index < older_applies; ++index) {
    std::ostringstream digest;
    digest << "sha256:" << std::setw(64) << std::setfill('0') << index;
    records.push_back(CommittedApplyRecord(
        index == 0U ? 4.0 : 10.0 + static_cast<double>(index),
        index == 0U ? Digest('c') : digest.str(),
        index == 0U ? "other_cell" : "cell_pick", true, active, false));
  }
  payload = JournalField(payload, "active", active);
  payload = JournalField(payload, "apply", CanonicalValue(CanonicalValue::Object{
      {"records", CanonicalValue(std::move(records))},
  }));
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  manager_status.Section("manager", payload);
  return core.CommittedScenarioActivation();
}

} // namespace

TEST(HeadlessApiCore, CommittedActivationNamesTheActiveApply) {
  TemporaryDirectory directory;
  const auto activation = CommittedActivationCore(directory, 3.0, 1U);
  EXPECT_EQ(Text(activation, "schema_version"),
            "manyforge.committed_scenario_activation.v6");
  EXPECT_EQ(Text(activation, "scene_initialization"), "initialize");
  EXPECT_EQ(Text(activation, "record_digest"), Digest('d'));
  EXPECT_EQ(std::get<double>(Member(activation, "apply_id").storage()), 3.0);
  EXPECT_EQ(std::get<double>(Member(activation, "apply_generation").storage()),
            1.0);
  EXPECT_EQ(Text(Member(activation, "origin"), "scenario_id"), "cell_pick");
  EXPECT_EQ(std::get<double>(Member(activation, "publication_generation").storage()),
            3.0);
  // A Scenario without a Program declares nothing; it is still an activation.
  EXPECT_TRUE(Member(activation, "program").is_null());
  EXPECT_TRUE(Member(activation, "program_binding_id").is_null());
  EXPECT_TRUE(Member(activation, "program_frame_facts").is_null());
  EXPECT_FALSE(Text(activation, "retained_bundle_id").empty());
  EXPECT_FALSE(Text(activation, "graph_revision").empty());
  // Only the active Apply: older Applies are not part of the activation.
  EXPECT_EQ(activation.find_object_member("committed_applies"), nullptr);
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  const auto attention = [&core]() {
    const auto value = core.OperatorAlarmMotionAttention();
    return std::pair{std::get<bool>(Member(value, "reconciled").storage()),
                     Unsigned(value, "generation")};
  };

  // Before any completed refresh the sources' currency is unknown.
  EXPECT_FALSE(attention().first);
  const auto first = core.RefreshAlarmSources();
  EXPECT_TRUE(std::get<bool>(Member(first, "refreshed").storage()));
  // The operations table reconciles the operation alarm source (OD-205 (b)).
  static_cast<void>(ObserveOperationTable(core, {}));
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  static_cast<void>(core.RefreshAlarmSources());
  // The operations table reconciles the operation alarm source (OD-205 (b)).
  static_cast<void>(ObserveOperationTable(core, {}));
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
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");

  static_cast<void>(core.RuntimeSnapshot());
  // The operations table reconciles the operation alarm source (OD-205 (b)).
  static_cast<void>(ObserveOperationTable(core, {}));

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
  // The owner lock follows the History file's name.
  auto lock = SqliteEventHistoryStore::DatabasePath(directory.history_directory());
  lock += ".headless-owner.lock";
  EXPECT_TRUE(std::filesystem::is_regular_file(lock));
  try {
    HeadlessApiCore duplicate(Config(directory, "api.epoch.2"), Clocks());
    static_cast<void>(duplicate);
    FAIL() << "overlapping Alarm Lifecycle Owner was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_owner_conflict");
  }

  owner.Shutdown();
  HeadlessApiCore replacement(Config(directory, "api.epoch.3"), Clocks());
  // The snapshot names no start of its own: the enclosing message does.
  EXPECT_EQ(replacement.AlarmSnapshot().find_object_member("owner_epoch"),
            nullptr);
  EXPECT_EQ(Text(replacement.AlarmSnapshot(), "owner_id"),
            "headless-api.cell.alarm");
}

TEST(HeadlessApiCore,
     PreviouslyCompleteSourcesGoingStaleProduceOneProviderCondition) {
  TemporaryDirectory directory;
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  // The Manager shows no new tick for longer than the silence bound.
  *monotonic += 20'000'000'000LL;
  const auto runtime = core.RuntimeSnapshot();
  EXPECT_FALSE(std::get<bool>(Member(runtime, "complete").storage()));
  const auto alarms = core.AlarmSnapshot();
  const auto &provider = OnlyAlarm(alarms);
  EXPECT_EQ(Text(provider, "definition_id"),
            "manyforge.runtime.provider_unavailable");
  EXPECT_EQ(Text(provider, "lifecycle_state"), "active_unacknowledged");
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(manager_status);
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  *monotonic += 20'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

  PublishHealthySources(manager_status, "open", 2U);
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(manager_status);
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  *monotonic += 20'000'000'001LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  const auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));
}

TEST(HeadlessApiCore,
     EmptyCellApplyIsExpectedAtStartupAndAfterReturningToEmpty) {
  TemporaryDirectory directory;
  auto wall_time = std::make_shared<std::int64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::system_clock::now().time_since_epoch())
          .count());
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          [wall_time]() { return *wall_time; },
          [monotonic]() { return *monotonic; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  for (int activation = 0; activation < 2; ++activation) {
    SCOPED_TRACE(activation);
    PublishManagerApplyTransition(manager_status, "runtime_convergence_pending",
                                  "robot-a", true);
    *monotonic += 20'000'000'000LL;
    EXPECT_FALSE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    const auto alarms = core.AlarmSnapshot();
    EXPECT_TRUE(Array(alarms, "alarms").empty());
    EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));

    // A later empty Scenario is ready in the same Headless session. The next
    // Apply must receive the same treatment as the initial empty-cell Apply.
    PublishHealthySources(manager_status, "open", 2U + activation);
    EXPECT_TRUE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  // Once the operation is gone, an unexplained loss is still an alarm.
  *monotonic += 20'000'000'000LL;
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
    auto wall_time = std::make_shared<std::int64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    auto monotonic = std::make_shared<std::int64_t>(1000);
    HeadlessApiCore core(
        Config(directory, "api.epoch.1"),
        HeadlessApiCoreClocks{
            [wall_time]() { return *wall_time; },
            [monotonic]() { return *monotonic; },
            []() { return "2026-08-05T00:00:00Z"; },
        });
    ManagerStatusFeed manager_status(core);
    PublishHealthySources(manager_status, "open");
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
    manager_status.Section("manager", payload);
    *monotonic += 20'000'000'000LL;
    EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
              "manyforge.runtime.provider_unavailable");
  }
}

TEST(HeadlessApiCore,
     ApplyBeforeAdmissionDoesNotSuppressProviderLossAlarm) {
  TemporaryDirectory directory;
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(manager_status, "fence_acquire_pending");
  *monotonic += 20'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);
}

TEST(HeadlessApiCore,
     ApplyForUnrelatedRuntimeDoesNotSuppressProviderLossAlarm) {
  TemporaryDirectory directory;
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
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  EXPECT_TRUE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  PublishManagerApplyTransition(manager_status, "runtime_convergence_pending",
                                "another-robot");
  *monotonic += 20'000'000'000LL;
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);
}

TEST(HeadlessApiCore,
     RestartWithOnlyStaleRetainedSourcesDoesNotFabricateProviderCondition) {
  TemporaryDirectory directory;
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
    ManagerStatusFeed manager_status(core);
    PublishHealthySources(manager_status, "open");
    EXPECT_TRUE(
        std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  // The replacement receives the Manager's values, but the Manager stops
  // answering before the replacement has observed them complete.
  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), clocks());
  ManagerStatusFeed replacement_status(replacement);
  PublishHealthySources(replacement_status, "open");
  EXPECT_TRUE(replacement.MarkManagerStatusDisconnected());
  EXPECT_FALSE(std::get<bool>(
      Member(replacement.RuntimeSnapshot(), "complete").storage()));
  const auto alarms = replacement.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_FALSE(std::get<bool>(Member(alarms, "reconciled").storage()));
}

// One subscription to the Manager's status stream: its snapshot, then the
// manager, logical-scene and scene-runtime sections at `generation`,
// `generation + 1` and `generation + 2`.
void ApplyStatusSubscription(HeadlessApiCore &core, const std::uint64_t tick,
                             const std::uint64_t generation) {
  static_cast<void>(
      core.ApplyManagerStatusFrame(test::StatusSnapshotFrame(tick)));
  static_cast<void>(core.ApplyManagerStatusFrame(
      test::StatusSectionFrame("manager", generation, ManagerPayload(), tick)));
  static_cast<void>(core.ApplyManagerStatusFrame(test::StatusSectionFrame(
      "logical_scene", generation + 1U, LogicalScenePayload(), tick)));
  static_cast<void>(core.ApplyManagerStatusFrame(test::StatusSectionFrame(
      "scene_runtime", generation + 2U, SceneRuntimePayload("open", 1U),
      tick)));
}

constexpr std::array<std::array<std::string_view, 3>, 3> kStatusSectionOwners{{
    {"manager", "component_manager", "manager"},
    {"logical_scene", "logical_scene", "logical_scene"},
    {"scene_runtime", "scene_runtime", "scene_runtime"},
}};

void ExpectStatusSectionsFresh(const CanonicalValue &runtime,
                               const std::uint64_t generation) {
  EXPECT_TRUE(std::get<bool>(Member(runtime, "complete").storage()));
  EXPECT_TRUE(Array(runtime, "reasons").empty());
  for (std::size_t index = 0U; index < kStatusSectionOwners.size(); ++index) {
    const auto &[name, kind, owner] = kStatusSectionOwners[index];
    SCOPED_TRACE(name);
    const auto &section = Member(Member(runtime, "sections"), name);
    EXPECT_EQ(Text(section, "freshness"), "fresh");
    EXPECT_TRUE(Array(section, "known_gaps").empty());
    EXPECT_EQ(Unsigned(section, "source_generation"), generation + index);
    EXPECT_EQ(Text(section, "source_owner_kind"), kind);
    EXPECT_EQ(Text(section, "source_owner_id"), owner);
  }
}

void ExpectManagerNotAnswering(const CanonicalValue &runtime) {
  const CanonicalValue::Array not_answering{
      CanonicalValue("manager_status_unavailable")};
  EXPECT_FALSE(std::get<bool>(Member(runtime, "complete").storage()));
  EXPECT_EQ(Array(runtime, "reasons"), not_answering);
  for (const auto &[name, kind, owner] : kStatusSectionOwners) {
    SCOPED_TRACE(name);
    const auto &section = Member(Member(runtime, "sections"), name);
    EXPECT_EQ(Text(section, "freshness"), "stale");
    EXPECT_EQ(Array(section, "known_gaps"), not_answering);
  }
}

TEST(HeadlessApiCore, ManagerStatusFramesMakeTheRuntimeSnapshotComplete) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ApplyStatusSubscription(core, 1U, 7U);
  ExpectStatusSectionsFresh(core.RuntimeSnapshot(), 7U);
}

TEST(HeadlessApiCore, ManagerStatusDisconnectMakesTheManagerNotAnsweringAtOnce) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ApplyStatusSubscription(core, 1U, 7U);
  ExpectStatusSectionsFresh(core.RuntimeSnapshot(), 7U);
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  EXPECT_TRUE(core.MarkManagerStatusDisconnected());
  EXPECT_FALSE(core.MarkManagerStatusDisconnected());
  ExpectManagerNotAnswering(core.RuntimeSnapshot());
  // The runtime health alarm names the one condition.
  const auto alarms = core.AlarmSnapshot();
  const auto &provider = OnlyAlarm(alarms);
  EXPECT_EQ(Text(provider, "definition_id"),
            "manyforge.runtime.provider_unavailable");
  EXPECT_EQ(Text(provider, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Array(Member(provider, "details"), "reasons"),
            (CanonicalValue::Array{CanonicalValue("manager_status_unavailable")}));
}

TEST(HeadlessApiCore, ManagerStatusReconnectResynchronises) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ApplyStatusSubscription(core, 1U, 7U);
  EXPECT_TRUE(core.MarkManagerStatusDisconnected());
  ExpectManagerNotAnswering(core.RuntimeSnapshot());

  // A new subscription: each section waits for its own frame.
  static_cast<void>(
      core.ApplyManagerStatusFrame(test::StatusSnapshotFrame(40U)));
  const auto waiting = core.RuntimeSnapshot();
  EXPECT_FALSE(std::get<bool>(Member(waiting, "complete").storage()));
  EXPECT_EQ(Array(Member(Member(waiting, "sections"), "manager"), "known_gaps"),
            (CanonicalValue::Array{CanonicalValue("manager_source_absent")}));
  ApplyStatusSubscription(core, 41U, 20U);
  ExpectStatusSectionsFresh(core.RuntimeSnapshot(), 20U);
}

TEST(HeadlessApiCore, ManagerStatusSilenceIsNotAnsweringUntilANewTick) {
  TemporaryDirectory directory;
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          []() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
          },
          [monotonic]() { return *monotonic; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  const auto tick_advanced = [&core](const std::string &frame) {
    return std::get<bool>(
        Member(core.ApplyManagerStatusFrame(frame), "tick_advanced").storage());
  };
  ApplyStatusSubscription(core, 1U, 7U);
  ExpectStatusSectionsFresh(core.RuntimeSnapshot(), 7U);

  // No new tick for longer than the silence bound (10 s).
  *monotonic += 10'000'000'001LL;
  ExpectManagerNotAnswering(core.RuntimeSnapshot());
  // A keepalive with a new tick answers again on the same subscription.
  EXPECT_TRUE(tick_advanced(test::StatusKeepaliveFrame(2U)));
  ExpectStatusSectionsFresh(core.RuntimeSnapshot(), 7U);

  // A section on the same tick shows no new loop of the Manager.
  *monotonic += 10'000'000'001LL;
  EXPECT_FALSE(tick_advanced(
      test::StatusSectionFrame("manager", 7U, ManagerPayload(), 2U)));
  ExpectManagerNotAnswering(core.RuntimeSnapshot());
}

TEST(HeadlessApiCore, ManagerStatusErrorAndInvalidFramesThrowTheirCodes) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto code = [&core](const std::string_view frame) -> std::string {
    try {
      static_cast<void>(core.ApplyManagerStatusFrame(frame));
    } catch (const ManagerStatusStreamError &error) {
      return error.code();
    }
    return "accepted";
  };
  EXPECT_EQ(code(R"({"api_version":"manyforge.manager_status_stream.v1",)"
                 R"("error":{"code":"manager_status_subscribers_full",)"
                 R"("details":{},"message":"full"},"type":"error"})"),
            "manager_status_subscribers_full");
  EXPECT_EQ(code("not a frame"), "manager_status_stream_invalid");
}

TEST(HeadlessApiCore,
     IndependentlyProjectsTheManagerSelectedNodeEnvironmentWithStableIdentity) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");

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
            "headless-api.cell.node-environment");
  EXPECT_EQ(Text(source, "selection_source_owner_id"),
            "manager");
  EXPECT_EQ(Unsigned(source, "projection_generation"), 1U);
  EXPECT_EQ(Unsigned(Member(second, "source"), "projection_generation"), 1U);
  EXPECT_EQ(Text(Member(second, "source"), "source_content_digest"),
            Text(source, "source_content_digest"));
}

TEST(HeadlessApiCore,
     IndependentlyProjectsTheManagerSelectedSourceModelWithStableIdentity) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");

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
            "headless-api.cell.source-model");
  EXPECT_EQ(Text(source, "selection_source_owner_id"),
            "manager");
  EXPECT_EQ(Unsigned(source, "projection_generation"), 1U);
  EXPECT_EQ(Unsigned(Member(second, "source"), "projection_generation"), 1U);
  EXPECT_EQ(Text(Member(second, "source"), "source_content_digest"),
            Text(source, "source_content_digest"));
}

TEST(HeadlessApiCore,
     SourceModelContextCorrelatesCanonicalEmptyChildrenFromOneRefresh) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");

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

  constexpr std::array<std::string_view, 4> kSelectionFields{
      "selection_projection_generation", "selection_source_content_digest",
      "selection_source_generation",     "selection_source_owner_id",
  };
  EXPECT_EQ(Member(environment, "source")
                .find_object_member("selection_source_owner_epoch"),
            nullptr);
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
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
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
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");

  try {
    static_cast<void>(core.AssistantModeProjection("composer-assistant"));
    FAIL() << "canonical-empty selection produced an assistant mode";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "assistant_component_absent");
  }
  try {
    static_cast<void>(core.AssistantModeContext("composer-assistant"));
    FAIL() << "canonical-empty selection produced an assistant context";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "assistant_component_absent");
  }
}

TEST(HeadlessApiCore,
     SceneFailureReturnsButKeepsAttentionAndRestartReannunciates) {
  TemporaryDirectory directory;
  std::string first_activation;
  {
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishHealthySources(manager_status, "failed");
    auto alarms = core.AlarmSnapshot();
    const auto &failure = OnlyAlarm(alarms);
    EXPECT_EQ(Text(failure, "definition_id"),
              "manyforge.runtime.scene_initialization_fault");
    first_activation = Text(failure, "activation_id");

    PublishHealthySources(manager_status, "open", 2U);
    alarms = core.AlarmSnapshot();
    const auto &returned = OnlyAlarm(alarms);
    EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
    static_cast<void>(core.AcknowledgeAlarm(
        Text(returned, "condition_id"), Text(returned, "activation_id"),
        Unsigned(returned, "record_revision"), Caller(), std::nullopt, true));
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  }

  HeadlessApiCore replacement(Config(directory, "api.epoch.2"), Clocks());
  ManagerStatusFeed replacement_status(replacement);
  PublishHealthySources(replacement_status, "failed", 3U);
  const auto replacement_snapshot = replacement.AlarmSnapshot();
  const auto &rediscovered = OnlyAlarm(replacement_snapshot);
  EXPECT_NE(Text(rediscovered, "activation_id"), first_activation);
  EXPECT_EQ(Text(rediscovered, "lifecycle_state"), "active_unacknowledged");
  EXPECT_GE(HistoryCount(replacement), 3U);
}

TEST(
    HeadlessApiCore,
    DistinctAlarmMutationsOutliveSmallHistoryWithoutAcknowledgingNewerActivations) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  config.historian_record_retention = 4U;
  HeadlessApiCore core(config, Clocks());
  ManagerStatusFeed manager_status(core);
  std::string old_condition;
  std::string old_activation;
  std::uint64_t old_revision = 0;
  for (std::uint64_t index = 1; index <= 64; ++index) {
    PublishHealthySources(manager_status, "failed", index * 2);
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
    PublishHealthySources(manager_status, "open", index * 2 + 1);
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
  EXPECT_EQ(Text(receipt, "model_attempt_id"),
            "80000000000000010000000000010101");
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
  EXPECT_EQ(Text(failure_receipt, "model_attempt_id"),
            "80000000000000010000000000010102");
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
  // A later start repeats run numbers and attempt counters: its attempt with
  // the same model_attempt_id is another event, never a conflict.
  auto later_start = record.as_object();
  for (auto &[name, value] : later_start) {
    if (name == "application_start_id") value = CanonicalValue("3f9a0000000000b2");
  }
  const auto later_receipt =
      replacement.CommitProgramModelAttempt(CanonicalValue(std::move(later_start)));
  EXPECT_EQ(Text(later_receipt, "status"), "stored");
  EXPECT_NE(Text(later_receipt, "event_id"), Text(receipt, "event_id"));
}

TEST(HeadlessApiCore,
     ConcurrentProgramModelAttemptsWithOneIdentityStoreExactlyOnce) {
  // OD-221: the write runs after the core lock; the store alone decides.
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto record = ProgramModelAttemptRecord();
  constexpr std::size_t kCallers = 8U;
  std::vector<std::string> statuses(kCallers);
  std::atomic<bool> go{false};
  std::vector<std::thread> callers;
  for (std::size_t index = 0; index < kCallers; ++index) {
    callers.emplace_back([&, index] {
      while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
      statuses[index] = Text(core.CommitProgramModelAttempt(record), "status");
    });
  }
  go.store(true, std::memory_order_release);
  for (auto &caller : callers) caller.join();
  EXPECT_EQ(std::count(statuses.begin(), statuses.end(), "stored"), 1);
  EXPECT_EQ(std::count(statuses.begin(), statuses.end(), "duplicate"),
            static_cast<std::ptrdiff_t>(kCallers - 1U));
  HistorianQuery query;
  query.components.insert("manyforge.program");
  query.event_types.insert("program_model_attempt");
  EXPECT_EQ(Array(core.HistoryPage(query), "events").size(), 1U);
}

TEST(HeadlessApiCore, ProgramModelAttemptRacingShutdownIsStoredOrRefusedAsClosed) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  std::atomic<bool> go{false};
  std::string outcome;
  std::thread caller([&] {
    while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
    try {
      outcome = Text(core.CommitProgramModelAttempt(ProgramModelAttemptRecord()), "status");
    } catch (const manyforge::core::TypedError &error) {
      outcome = error.code();
    }
  });
  go.store(true, std::memory_order_release);
  core.Shutdown();
  caller.join();
  // Accepted difference (OD-221): a call racing Shutdown may meet the closed
  // store instead of the closed core.
  EXPECT_TRUE(outcome == "stored" || outcome == "headless_api_closed" ||
              outcome == "historian_closed")
      << outcome;
  try {
    static_cast<void>(core.CommitProgramModelAttempt(ProgramModelAttemptRecord(false)));
    FAIL() << "a model attempt was accepted after Shutdown";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_api_closed");
  }
}

TEST(HeadlessApiCore,
     ClientIncidentTransitionsAreStrictSparseAndDurablyQueued) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());

  const auto activated = core.RecordClientIncident(
      "client-incident.one", "control_delayed", "activated",
      std::optional<std::uint64_t>{1U}, "client.one", "operator_ui", "heartbeat_delayed",
      1U, 600'000'000U, 100U, 100U);
  EXPECT_EQ(Text(activated, "schema_version"),
            "manyforge.client_incident_record.v1");
  EXPECT_TRUE(std::get<bool>(Member(activated, "recorded").storage()));
  const auto resolved = core.RecordClientIncident(
      "client-incident.one", "control_delayed", "resolved",
      std::optional<std::uint64_t>{1U}, "client.one", "operator_ui", "heartbeat_recovered",
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
      std::optional<std::uint64_t>{1U}, "client.one", "operator_ui", "heartbeat_delayed",
      1U, 600'000'000U, kBeyondJsonSafeInteger + 1U,
      kBeyondJsonSafeInteger + 2U)));

  try {
    static_cast<void>(core.RecordClientIncident(
        "client-incident.invalid", "arbitrary_event", "activated",
        std::optional<std::uint64_t>{1U}, "client.one", "operator_ui", "forged", 1U,
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
        "client-incident." + kind, kind, "activated", std::optional<std::uint64_t>{1U},
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

TEST(HeadlessApiCore, InvalidControlProjectionIsAnErrorIncident) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto receipt = core.RecordClientIncident(
      "client-incident.projection", "control_projection_invalid", "activated",
      std::optional<std::uint64_t>{1U}, "client.one", "operator_ui",
      "invalid_projection:delta:42", 1U, std::nullopt, 100U, 100U);
  EXPECT_TRUE(std::get<bool>(Member(receipt, "recorded").storage()));

  HistorianQuery query;
  query.components.insert("manyforge.composer");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (!Array(page, "events").empty()) {
      break;
    }
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &event = Member(Array(page, "events").front(), "event");
  EXPECT_EQ(Text(event, "event_type"), "client_incident_activated");
  EXPECT_EQ(Text(event, "severity"), "ERROR");
  EXPECT_EQ(Text(Member(event, "payload"), "incident_kind"),
            "control_projection_invalid");
  EXPECT_EQ(Text(Member(event, "payload"), "reason_code"),
            "invalid_projection:delta:42");
}

TEST(HeadlessApiCore, ComponentActionAndApplyOutcomesAreRecordedOnce) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto error = [](const std::string &code) {
    return CanonicalValue(CanonicalValue::Object{
        {"code", CanonicalValue(code)},
        {"message", CanonicalValue("Joint angles unavailable")}});
  };
  const auto action = [&](const std::string &outcome, CanonicalValue failure) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.activity_event.v1")},
        {"kind", CanonicalValue("component_action")},
        {"subject", CanonicalValue("lite6_arm")},
        {"action", CanonicalValue("enable")},
        {"outcome", CanonicalValue(outcome)},
        {"error", std::move(failure)},
        {"components", CanonicalValue(CanonicalValue::Array{})},
        {"message", CanonicalValue("Enable lite6_arm " + outcome)}});
  };
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordActivity(action("failed",
                                        error("xarm_joint_angles_unavailable"))),
             "recorded")
          .storage()));
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordActivity(action("succeeded", CanonicalValue(nullptr))),
             "recorded")
          .storage()));
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordActivity(CanonicalValue(CanonicalValue::Object{
                 {"schema_version", CanonicalValue("manyforge.activity_event.v1")},
                 {"kind", CanonicalValue("apply")},
                 {"subject", CanonicalValue("scenario_load")},
                 {"action", CanonicalValue(nullptr)},
                 {"outcome", CanonicalValue("failed")},
                 {"error", error("execution_owner_unavailable")},
                 {"components",
                  CanonicalValue(CanonicalValue::Array{CanonicalValue(
                      CanonicalValue::Object{
                          {"component_id", CanonicalValue("lite6")},
                          {"disposition", CanonicalValue("add")},
                          {"error", error("execution_owner_unavailable")}})})},
                 {"message", CanonicalValue("Scenario load failed")}})),
             "recorded")
          .storage()));
  // A failure needs its typed error, and an unknown outcome is refused.
  for (auto invalid : {action("failed", CanonicalValue(nullptr)),
                       action("exploded", error("x"))}) {
    try {
      static_cast<void>(core.RecordActivity(std::move(invalid)));
      FAIL() << "an invalid activity record was accepted";
    } catch (const OperatorAlarmError &failure) {
      EXPECT_EQ(failure.code(), "activity_record_invalid");
    }
  }

  HistorianQuery query;
  query.event_types = {"component_action_finished", "apply_finished"};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 3U) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 3U);
  std::map<std::string, std::string> severities;
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    if (Text(event, "event_type") == "program_node_attempt_finished") {
      // Stamped at the attempt's end on the Runner's clocks.
      EXPECT_EQ(Text(event, "occurred_at_ns"), "5000000000");
      EXPECT_EQ(Text(event, "occurred_at_wall"), "2026-11-24T10:00:01.365+00:00");
      EXPECT_EQ(Text(Member(event, "payload"), "clock_owner"),
                "program_runner:api.epoch.1");
    }
    severities[Text(event, "component") + "/" +
               Text(Member(event, "payload"), "outcome")] =
        Text(event, "severity");
  }
  EXPECT_EQ(severities, (std::map<std::string, std::string>{
                            {"lite6_arm/failed", "ERROR"},
                            {"lite6_arm/succeeded", "INFO"},
                            {"manyforge.composer/failed", "ERROR"}}));
}

TEST(HeadlessApiCore, ProgramResetOutcomesAreRecordedWithTheirFacts) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto facts = [](CanonicalValue scene_revision_id,
                        CanonicalValue scene_content_hash) {
    return CanonicalValue(CanonicalValue::Object{
        {"reload_initial_scene", CanonicalValue(true)},
        {"reset_robot_position", CanonicalValue(false)},
        {"program_run_id", CanonicalValue(7.0)},
        {"program_revision", CanonicalValue(Digest('1'))},
        {"applied_record_digest", CanonicalValue(Digest('2'))},
        {"scene_revision_id", std::move(scene_revision_id)},
        {"scene_content_hash", std::move(scene_content_hash)},
        {"cause_code", CanonicalValue(nullptr)}});
  };
  const auto reset = [](const std::string &outcome, CanonicalValue failure,
                        CanonicalValue reset_facts) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.activity_event.v1")},
        {"kind", CanonicalValue("program_reset")},
        {"subject", CanonicalValue("program_reset")},
        {"action", CanonicalValue(nullptr)},
        {"outcome", CanonicalValue(outcome)},
        {"error", std::move(failure)},
        {"components", CanonicalValue(CanonicalValue::Array{})},
        {"message", CanonicalValue("Program reset " + outcome)},
        {"reset", std::move(reset_facts)}});
  };
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("authoring_not_ready")},
      {"message", CanonicalValue("authoring admission is not ready")}});
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordActivity(reset(
                 "succeeded", CanonicalValue(nullptr),
                 facts(CanonicalValue(10.0), CanonicalValue(Digest('3'))))),
             "recorded")
          .storage()));
  EXPECT_TRUE(std::get<bool>(
      Member(core.RecordActivity(
                 reset("refused", error,
                       facts(CanonicalValue(nullptr), CanonicalValue(nullptr)))),
             "recorded")
          .storage()));
  // A reset record needs its facts, complete scene revisions, its typed
  // error on a failure, and only the Reset outcomes.
  auto missing = reset("succeeded", CanonicalValue(nullptr),
                       facts(CanonicalValue(nullptr), CanonicalValue(nullptr)))
                     .as_object();
  std::erase_if(missing, [](const auto &member) { return member.first == "reset"; });
  for (auto invalid :
       {CanonicalValue(std::move(missing)),
        reset("succeeded", CanonicalValue(nullptr),
              facts(CanonicalValue(10.0), CanonicalValue(nullptr))),
        reset("succeeded", CanonicalValue(nullptr),
              facts(CanonicalValue(0.0), CanonicalValue(Digest('3')))),
        reset("failed", CanonicalValue(nullptr),
              facts(CanonicalValue(nullptr), CanonicalValue(nullptr))),
        reset("accepted", CanonicalValue(nullptr),
              facts(CanonicalValue(nullptr), CanonicalValue(nullptr)))}) {
    try {
      static_cast<void>(core.RecordActivity(std::move(invalid)));
      FAIL() << "an invalid Program Reset record was accepted";
    } catch (const OperatorAlarmError &failure) {
      EXPECT_EQ(failure.code(), "activity_record_invalid");
    }
  }

  HistorianQuery query;
  query.event_types = {"program_reset_finished"};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 2U) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 2U);
  std::map<std::string, std::string> severities;
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    if (Text(event, "event_type") == "program_node_attempt_finished") {
      // Stamped at the attempt's end on the Runner's clocks.
      EXPECT_EQ(Text(event, "occurred_at_ns"), "5000000000");
      EXPECT_EQ(Text(event, "occurred_at_wall"), "2026-11-24T10:00:01.365+00:00");
      EXPECT_EQ(Text(Member(event, "payload"), "clock_owner"),
                "program_runner:api.epoch.1");
    }
    EXPECT_EQ(Text(event, "component"), "manyforge.program_runner");
    severities[Text(Member(event, "payload"), "outcome")] =
        Text(event, "severity");
  }
  EXPECT_EQ(severities, (std::map<std::string, std::string>{
                            {"refused", "WARN"}, {"succeeded", "INFO"}}));
}

TEST(HeadlessApiCore, NodeAttemptsAndSceneChangesAreRecordedWithTheirFacts) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("motion_plan_failed")},
      {"message", CanonicalValue("No path")},
      {"details", CanonicalValue(CanonicalValue::Object{
                      {"input_problem", CanonicalValue("invalid")}})}});
  const auto attempt_facts = [](CanonicalValue started_at_wall,
                                CanonicalValue duration_ms) {
    return CanonicalValue(CanonicalValue::Object{
        {"program_run_id", CanonicalValue(3.0)},
        {"node_instance_id", CanonicalValue("add_box")},
        {"node_attempt_id", CanonicalValue(12.0)},
        {"started_at_wall", std::move(started_at_wall)},
        {"finished_at_ns", CanonicalValue("5000000000")},
        {"finished_at_wall", CanonicalValue("2026-11-24T10:00:01.365+00:00")},
        {"duration_ms", std::move(duration_ms)},
        {"scene_operation", CanonicalValue("add_object")},
        {"scene_resource_id", CanonicalValue("box_1")}});
  };
  const auto attempt = [](const std::string &outcome, CanonicalValue failure,
                          CanonicalValue facts) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.activity_event.v1")},
        {"kind", CanonicalValue("program_node_attempt")},
        {"subject", CanonicalValue("scene.add_object")},
        {"action", CanonicalValue(nullptr)},
        {"outcome", CanonicalValue(outcome)},
        {"error", std::move(failure)},
        {"components", CanonicalValue(CanonicalValue::Array{})},
        {"message", CanonicalValue("Node add_box (scene.add_object) " + outcome)},
        {"attempt", std::move(facts)}});
  };
  const auto change_facts = [](CanonicalValue scene_revision_id) {
    return CanonicalValue(CanonicalValue::Object{
        {"scene_resource_id", CanonicalValue("box_1")},
        {"source_label", CanonicalValue("composer_ui")},
        {"principal_id", CanonicalValue(nullptr)},
        {"duration_ms", CanonicalValue(12.5)},
        {"scene_revision_id", std::move(scene_revision_id)}});
  };
  const auto change = [](const std::string &subject, const std::string &outcome,
                         CanonicalValue failure, CanonicalValue facts) {
    return CanonicalValue(CanonicalValue::Object{
        {"schema_version", CanonicalValue("manyforge.activity_event.v1")},
        {"kind", CanonicalValue("scene_change")},
        {"subject", CanonicalValue(subject)},
        {"action", CanonicalValue(nullptr)},
        {"outcome", CanonicalValue(outcome)},
        {"error", std::move(failure)},
        {"components", CanonicalValue(CanonicalValue::Array{})},
        {"message", CanonicalValue("Scene add object box_1 " + outcome)},
        {"scene_change", std::move(facts)}});
  };
  const auto wall = CanonicalValue("2026-11-24T10:00:00.125+00:00");
  for (auto valid :
       {attempt("failed", error, attempt_facts(wall, CanonicalValue(1240.0))),
        attempt("stopped", CanonicalValue(nullptr),
                attempt_facts(wall, CanonicalValue(0.0))),
        change("add_object", "succeeded", CanonicalValue(nullptr),
               change_facts(CanonicalValue(4.0))),
        change("add_object", "refused", error,
               change_facts(CanonicalValue(nullptr)))}) {
    EXPECT_TRUE(std::get<bool>(
        Member(core.RecordActivity(std::move(valid)), "recorded").storage()));
  }
  // Exact facts, a parseable start, a non-negative duration, the error only
  // on a failed or interrupted attempt, and only known scene operations.
  auto missing = attempt("succeeded", CanonicalValue(nullptr),
                         attempt_facts(wall, CanonicalValue(1.0)))
                     .as_object();
  std::erase_if(missing,
                [](const auto &member) { return member.first == "attempt"; });
  for (auto invalid :
       {CanonicalValue(std::move(missing)),
        attempt("succeeded", CanonicalValue(nullptr),
                attempt_facts(CanonicalValue("yesterday"), CanonicalValue(1.0))),
        attempt("succeeded", CanonicalValue(nullptr),
                attempt_facts(wall, CanonicalValue(-1.0))),
        attempt("failed", CanonicalValue(nullptr),
                attempt_facts(wall, CanonicalValue(1.0))),
        attempt("stopped", error, attempt_facts(wall, CanonicalValue(1.0))),
        attempt("refused", error, attempt_facts(wall, CanonicalValue(1.0))),
        change("rename_object", "succeeded", CanonicalValue(nullptr),
               change_facts(CanonicalValue(4.0))),
        change("add_object", "succeeded", CanonicalValue(nullptr),
               change_facts(CanonicalValue(0.0))),
        change("add_object", "unknown", CanonicalValue(nullptr),
               change_facts(CanonicalValue(nullptr)))}) {
    try {
      static_cast<void>(core.RecordActivity(std::move(invalid)));
      FAIL() << "an invalid node attempt or scene change record was accepted";
    } catch (const OperatorAlarmError &failure) {
      EXPECT_EQ(failure.code(), "activity_record_invalid");
    }
  }

  HistorianQuery query;
  query.event_types = {"program_node_attempt_finished", "scene_change_finished"};
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 4U) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 4U);
  std::map<std::string, std::string> severities;
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    if (Text(event, "event_type") == "program_node_attempt_finished") {
      // Stamped at the attempt's end on the Runner's clocks.
      EXPECT_EQ(Text(event, "occurred_at_ns"), "5000000000");
      EXPECT_EQ(Text(event, "occurred_at_wall"), "2026-11-24T10:00:01.365+00:00");
      EXPECT_EQ(Text(Member(event, "payload"), "clock_owner"),
                "program_runner:api.epoch.1");
    }
    severities[Text(event, "component") + "/" + Text(event, "event_type") +
               "/" + Text(Member(event, "payload"), "outcome")] =
        Text(event, "severity");
  }
  // A failed node attempt is WARN, never ERROR (ordinary control flow).
  EXPECT_EQ(severities,
            (std::map<std::string, std::string>{
                {"manyforge.program_runner/program_node_attempt_finished/failed",
                 "WARN"},
                {"manyforge.program_runner/program_node_attempt_finished/stopped",
                 "INFO"},
                {"manyforge.scene/scene_change_finished/refused", "WARN"},
                {"manyforge.scene/scene_change_finished/succeeded", "INFO"}}));
}

TEST(HeadlessApiCore,
     ProgramHeldDiagnosticIsInformationalAndRetainsExactValues) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  CanonicalValue record(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.program_diagnostic.v5")},
      {"program_run_id", CanonicalValue(7.0)},
      {"application_start_id", CanonicalValue("3f9a0000000000a1")},
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
  query.correlations.emplace("program_run_id", "7");
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
            "program_runner:3f9a0000000000a1");
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
       {"manyforge.program_diagnostic.v1", "manyforge.program_diagnostic.v2",
        "manyforge.program_diagnostic.v3", "manyforge.program_diagnostic.v4"}) {
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
      {"rule_id", CanonicalValue(nullptr)},
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
      {"schema_version", CanonicalValue("manyforge.program_diagnostic.v5")},
      {"program_run_id", CanonicalValue(8.0)},
      {"application_start_id", CanonicalValue("3f9a0000000000a1")},
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
  query.correlations.emplace("program_run_id", "8");
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
       {"component_id", "input_problem", "path", "reason", "reference_id",
        "reference_type", "required_action", "retryable", "rule_id",
        "source_kind", "source_reference"}) {
    registered.emplace_back(key, CanonicalValue(true));
  }
  EXPECT_NO_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", CanonicalValue(registered)))));
  const auto reference_details = [](std::string kind, std::string member,
                                    std::string component, std::string identity) {
    return CanonicalValue(CanonicalValue::Object{
        {"reference_type", CanonicalValue(std::move(kind))},
        {"reference", CanonicalValue(CanonicalValue::Object{
            {"component_id", CanonicalValue(std::move(component))},
            {std::move(member), CanonicalValue(std::move(identity))}})}});
  };
  for (const auto &[kind, member] :
       std::vector<std::pair<std::string, std::string>>{
           {"target", "command_target_id"}, {"frame", "frame_id"},
           {"collision_body", "collision_body_id"}, {"signal", "signal_name"}}) {
    const auto identity = kind == "signal" ? std::string(512U, 's') : "Feature.1";
    EXPECT_NO_THROW(static_cast<void>(core.RecordProgramDiagnostic(
        with(record, "details", reference_details(kind, member, "component.1", identity)))));
    EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
        with(record, "details", reference_details(kind, member, "Bad Component", identity)))),
        OperatorAlarmError);
  }
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", reference_details("target", "frame_id", "component", "f")))),
      OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", reference_details("signal", "signal_name", "component", std::string(513U, 's'))))),
      OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", reference_details("frame", "frame_id", "component", std::string(129U, 'f'))))),
      OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", CanonicalValue(CanonicalValue::Object{
          {"reference", CanonicalValue(CanonicalValue::Object{})}})))),
      OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", reference_details("signal", "signal_name", "component", std::string("a\0b", 3))))),
      OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.RecordProgramDiagnostic(
      with(record, "details", CanonicalValue(CanonicalValue::Object{
          {"reference_type", CanonicalValue("frame")},
          {"reference", CanonicalValue(CanonicalValue::Object{
              {"component_id", CanonicalValue("component")},
              {"frame_id", CanonicalValue("frame")},
              {"extra", CanonicalValue("x")}})}})))),
      OperatorAlarmError);
  // Nine maximal strings exceed the 2,048-byte canonical details bound.
  CanonicalValue::Object oversized;
  for (const auto key : {"input_problem", "path", "reason", "reference_id",
                         "reference_type", "required_action", "source_kind",
                         "source_reference", "component_id"}) {
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
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto failure = OperationJournalPayload("state_observed", "failed",
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
      1U, "state_observed", std::nullopt, 1U, 1U,
      100U, "2026-08-07T10:00:00.000000001Z", failure, failure_digest);
  EXPECT_EQ(Text(ingested, "status"), "ingested");
  EXPECT_FALSE(std::holds_alternative<std::nullptr_t>(
      Member(ingested, "event_id").storage()));
  static_cast<void>(core.CompleteOperationEventSnapshot(1U));
  // OD-205 (b): a journal row is History only; the alarm follows the
  // Kernel's operations table.
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  const auto table_entry = OperationTableEntry(Member(failure, "operation_state"));
  static_cast<void>(ObserveOperationTable(core, {table_entry}));

  auto alarms = core.AlarmSnapshot();
  const auto &fault = OnlyAlarm(alarms);
  EXPECT_EQ(Text(fault, "definition_id"), "manyforge.motion.execution_fault");
  EXPECT_EQ(Text(Member(Member(fault, "details"), "terminal_error"), "message"),
            "planning failed");
  EXPECT_EQ(Text(Member(Member(Member(fault, "details"), "terminal_error"), "details"),
                 "reason"),
            "planner_failed");
  EXPECT_TRUE(Member(Member(fault, "correlations"), "request_key").is_null());
  EXPECT_EQ(Text(fault, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(fault, "subject"), "subject_id"), "1");

  const auto replayed = core.IngestOperationEvent(
      1U, "state_observed", std::nullopt, 1U, 1U,
      100U, "2026-08-07T10:00:00.000000001Z", failure, failure_digest);
  EXPECT_EQ(Text(replayed, "status"), "already_ingested");

  const auto unavailable = core.MarkOperationEventSourceUnknown();
  EXPECT_EQ(Text(unavailable, "status"), "marked_unknown");
  const auto unknown_snapshot = core.AlarmSnapshot();
  const auto &unknown_domains = Array(unknown_snapshot, "domains");
  const auto unknown_motion = std::find_if(
      unknown_domains.begin(), unknown_domains.end(),
      [](const auto &domain) { return Text(domain, "domain") == "motion"; });
  ASSERT_NE(unknown_motion, unknown_domains.end());
  EXPECT_FALSE(std::get<bool>(Member(*unknown_motion, "reconciled").storage()));
  EXPECT_EQ(Unsigned(*unknown_motion, "unknown_source_count"), 1U);
  // The next table reconciles the source; the unchanged entry is not
  // submitted again.
  static_cast<void>(ObserveOperationTable(core, {table_entry}));
  const auto recovered_snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Unsigned(OnlyAlarm(recovered_snapshot), "activation_count"), 1U);
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
  // One execution host serves the whole start, so no later host epoch
  // retires these faults; the start's end does.
}

TEST(HeadlessApiCore,
     PreparedOperationEvidenceIsAcceptedAsAHistoryFactWithoutRaisingAnAlarm) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto prepared = OperationJournalPayload("operation_prepared", "prepared", CanonicalValue(nullptr));
  const auto prepared_json = content_identity::canonical_json(prepared);
  const auto ingested = core.IngestOperationEvent(
      1U, "operation_prepared", std::nullopt, 1U,
      1U, 100U, "2026-08-07T10:00:00.000000001Z", prepared,
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
    auto fields = OperationJournalPayload("operation_prepared",
                                          "prepared", CanonicalValue(nullptr))
                      .as_object();
    for (auto &[name, value] : fields) {
      if (name == "api_version")
        value = CanonicalValue("manyforge.operation_journal_record.v10");
      if (name == "operation_state") {
        auto state = value.as_object();
        state.emplace_back("journal_capture_order",
                           CanonicalValue(std::to_string(order)));
        value = CanonicalValue(std::move(state));
      }
    }
    const CanonicalValue payload(std::move(fields));
    const auto ingested = core.IngestOperationEvent(
        1U, "operation_prepared", std::nullopt, 1U, 1U,
        100U, "2026-09-07T10:00:00.000000001Z", payload,
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
  auto fields = OperationJournalPayload("operation_prepared", "prepared",
                                        CanonicalValue(nullptr))
                    .as_object();
  for (auto &[name, value] : fields) {
    if (name == "api_version")
      value = CanonicalValue("manyforge.operation_journal_record.v10");
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
              {"node_attempt_id", CanonicalValue(1.0)}}));
      participants.front() = CanonicalValue(std::move(participant));
      content = CanonicalValue(std::move(participants));
    }
    value = CanonicalValue(std::move(state));
  }
  const auto ingest = [&](const CanonicalValue &payload) {
    return core.IngestOperationEvent(
        1U, "operation_prepared", std::nullopt, 1U, 1U,
        100U, "2026-09-07T10:00:00.000000001Z", payload,
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
}

TEST(HeadlessApiCore,
     DroppedHistoryRecordIsCountedAlarmedAndNeverGatesMotion) {
  TemporaryDirectory directory;
  auto monotonic = std::make_shared<std::int64_t>(1000);
  HeadlessApiCore core(
      Config(directory, "api.epoch.1"),
      HeadlessApiCoreClocks{
          []() {
            return std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
          },
          [monotonic]() { return ++*monotonic; },
          []() { return "2026-08-05T00:00:00Z"; },
      });
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  const auto normal = OperationJournalPayload("operation_prepared", "prepared", CanonicalValue(nullptr));
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
        sequence, "operation_prepared", std::nullopt, 1U,
        1U, 100, "2026-08-07T10:00:00Z", payload,
        content_identity::sha256_digest(
            content_identity::canonical_json(payload)));
  };
  // Motion admission as the Kernel reads it, after a completed refresh.
  const auto admitted = [&core]() {
    static_cast<void>(core.AlarmSnapshot());
    return std::get<bool>(
        Member(core.OperatorAlarmMotionAttention(), "reconciled").storage());
  };
  const auto history_alarm = [&core]() -> std::optional<CanonicalValue> {
    const auto alarms_now = core.AlarmSnapshot();
    for (const auto &alarm : Array(alarms_now, "alarms")) {
      if (Text(alarm, "definition_id") ==
              "manyforge.runtime.history_missing_records" &&
          Text(alarm, "lifecycle_state").starts_with("active")) {
        return alarm;
      }
    }
    return std::nullopt;
  };

  // Settle the first refresh's own records so that only the loss below is
  // in flight.
  static_cast<void>(core.AlarmSnapshot());
  const auto settle_deadline = std::chrono::steady_clock::now() + 2s;
  while (true) {
    const auto health = core.HistorianHealth();
    if ((Unsigned(health, "queue_depth") == 0U &&
         !std::get<bool>(Member(health, "write_in_flight").storage())) ||
        std::chrono::steady_clock::now() >= settle_deadline) {
      break;
    }
    std::this_thread::yield();
  }

  // A committed fact that cannot be made into a History entry is dropped.
  EXPECT_EQ(Text(ingest(oversized, 1), "status"), "ingested");
  const auto failed = core.HistorianHealth();
  static_cast<void>(core.CompleteOperationEventSnapshot(1U));
  // The operations table reconciles the operation alarm source (OD-205 (b)).
  static_cast<void>(ObserveOperationTable(core, {}));
  EXPECT_FALSE(std::get<bool>(Member(failed, "available").storage()));
  EXPECT_EQ(Text(failed, "error_code"), "historian_projection_failure");
  EXPECT_EQ(Unsigned(failed, "delivery_failure_count"), 1U);

  // History raises its own alarm at the loss. It needs no acknowledgement
  // and gates nothing: the alarm owner stays reconciled and motion admitted.
  const auto raised = history_alarm();
  ASSERT_TRUE(raised.has_value());
  EXPECT_EQ(Text(*raised, "lifecycle_state"), "active_acknowledged");
  EXPECT_FALSE(std::get<bool>(Member(*raised, "acknowledgement_required").storage()));
  EXPECT_FALSE(std::get<bool>(Member(*raised, "requires_motion_attention").storage()));
  EXPECT_TRUE(std::get<bool>(Member(core.AlarmSnapshot(), "reconciled").storage()));
  EXPECT_TRUE(admitted());

  // The other causes are unchanged: alarm sources older than the currency
  // bound still gate motion, and the next refresh clears that by itself.
  *monotonic += kAlarmSourceCurrencyBoundNs + 1;
  EXPECT_FALSE(std::get<bool>(
      Member(core.OperatorAlarmMotionAttention(), "reconciled").storage()));
  EXPECT_TRUE(admitted());

  // The next record History writes with its queue empty ends the loss, and
  // the alarm returns to normal without any operator action.
  EXPECT_EQ(Text(ingest(normal, 2), "status"), "ingested");
  static_cast<void>(core.CompleteOperationEventSnapshot(2U));
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (!std::get<bool>(
             Member(core.HistorianHealth(), "available").storage()) &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto recovered = core.HistorianHealth();
  EXPECT_TRUE(std::get<bool>(Member(recovered, "available").storage()));
  EXPECT_EQ(Unsigned(recovered, "delivery_failure_count"), 1U);
  EXPECT_FALSE(history_alarm().has_value());
  EXPECT_TRUE(admitted());
}

TEST(HeadlessApiCore, StopEventRowsBecomeHistoryWithSeverityAndDuration) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto accepted = StopEventAcceptedPayload();
  IngestJournalEvent(core, accepted, 1U);
  // One completion per result: its severity follows the result and its
  // duration is completion minus acceptance (100 -> 250).
  struct Completion final {
    std::string result;
    std::uint64_t event_id;
    std::string severity;
  };
  const std::vector<Completion> completions{{"stopped", 1U, "INFO"},
                                            {"stopped_escalated", 2U, "WARN"},
                                            {"some_failed", 3U, "ERROR"}};
  std::uint64_t sequence = 1U;
  for (const auto &completion : completions) {
    IngestJournalEvent(core,
                       StopEventCompletedPayload(completion.result,
                                                 completion.event_id),
                       ++sequence);
  }

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("stop_event_accepted");
  query.event_types.insert("stop_event_completed");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").size() != 4U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 4U);
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    const auto &payload = Member(event, "payload");
    for (const auto *name : {"operation_id", "operation_revision", "request_key"})
      EXPECT_TRUE(Member(payload, name).is_null()) << name;
    const auto &row = Member(payload, "journal_payload");
    if (Text(row, "row") == "accepted") {
      EXPECT_EQ(Text(event, "event_type"), "stop_event_accepted");
      EXPECT_EQ(Text(event, "severity"), "INFO");
      EXPECT_EQ(Unsigned(payload, "stop_event_id"), 1U);
      EXPECT_EQ(payload.find_object_member("duration_ns"), nullptr);
      EXPECT_EQ(row, accepted);
      continue;
    }
    const auto result = Text(row, "result");
    SCOPED_TRACE(result);
    const auto expected = std::find_if(
        completions.begin(), completions.end(),
        [&](const auto &completion) { return completion.result == result; });
    ASSERT_NE(expected, completions.end());
    EXPECT_EQ(Text(event, "event_type"), "stop_event_completed");
    EXPECT_EQ(Text(event, "severity"), expected->severity);
    EXPECT_EQ(Unsigned(payload, "stop_event_id"), expected->event_id);
    EXPECT_EQ(Unsigned(payload, "duration_ns"), 150U);
    EXPECT_EQ(row, StopEventCompletedPayload(result, expected->event_id));
    const auto detail = core.HistoryDetail(Unsigned(stored, "sequence"));
    const auto &latency = Member(Member(detail, "sections"), "latency");
    EXPECT_TRUE(std::get<bool>(Member(latency, "available").storage()));
    EXPECT_EQ(Text(Member(latency, "value"), "unit"), "ns");
    EXPECT_EQ(Unsigned(Member(latency, "value"), "value"), 150U);
  }
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}

TEST(HeadlessApiCore, StopEventRowWithRequestKeyOrOperationIsRefused) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  std::uint64_t sequence = 0U;
  for (const auto &entry :
       {std::pair{std::string("stop_event_accepted"), StopEventAcceptedPayload()},
        std::pair{std::string("stop_event_completed"),
                  StopEventCompletedPayload()}}) {
    const auto &kind = entry.first;
    const auto &payload = entry.second;
    SCOPED_TRACE(kind);
    ++sequence;
    const auto digest = content_identity::sha256_digest(
        content_identity::canonical_json(payload));
    const auto ingest = [&](std::optional<std::string> request,
                            const std::optional<std::uint64_t> operation,
                            const std::optional<std::uint64_t> revision) {
      return core.IngestOperationEvent(
          sequence, kind, std::move(request), operation, revision,
          100 + sequence, "2026-08-07T10:00:00.000000001Z", payload, digest);
    };
    EXPECT_THROW(static_cast<void>(
                     ingest(kStopRequestKey, std::nullopt, std::nullopt)),
                 OperatorAlarmError);
    EXPECT_THROW(static_cast<void>(ingest(std::nullopt, 1U, 1U)),
                 OperatorAlarmError);
    EXPECT_THROW(static_cast<void>(ingest(std::nullopt, 1U, std::nullopt)),
                 OperatorAlarmError);
    EXPECT_EQ(Text(ingest(std::nullopt, std::nullopt, std::nullopt), "status"),
              "ingested");
  }
  // A Stop row is no operation record of another kind.
  const auto payload = StopEventAcceptedPayload();
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   3U, "state_observed", std::nullopt, std::nullopt,
                   std::nullopt, 103U, "2026-08-07T10:00:00.000000002Z",
                   payload,
                   content_identity::sha256_digest(
                       content_identity::canonical_json(payload)))),
               OperatorAlarmError);
}

TEST(HeadlessApiCore, CompletionBeforeAcceptanceIsInvalid) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  EXPECT_THROW(IngestJournalEvent(
                   core, StopEventCompletedPayload("stopped", 1U, 250, 100),
                   1U),
               OperatorAlarmError);
  IngestJournalEvent(core, StopEventCompletedPayload("stopped", 1U, 250, 250),
                     1U);
}

TEST(HeadlessApiCore, OperationStopEventIdMustBeIntegerOrNull) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto payload = OperationJournalPayload("operation_prepared", "prepared",
                                               CanonicalValue(nullptr));
  const auto &state = Member(payload, "operation_state");
  auto missing = state.as_object();
  std::erase_if(missing, [](const auto &field) {
    return field.first == "stop_event_id";
  });
  EXPECT_THROW(IngestJournalEvent(core,
                                  JournalField(payload, "operation_state",
                                               CanonicalValue(std::move(missing))),
                                  1U),
               OperatorAlarmError);
  for (const auto &invalid : {CanonicalValue("3"), CanonicalValue(0.0)}) {
    EXPECT_THROW(
        IngestJournalEvent(core,
                           JournalField(payload, "operation_state",
                                        JournalField(state, "stop_event_id", invalid)),
                           1U),
        OperatorAlarmError);
  }
  IngestJournalEvent(core, payload, 1U);
  IngestJournalEvent(core,
                     JournalField(payload, "operation_state",
                                  JournalField(state, "stop_event_id",
                                               CanonicalValue(3.0))),
                     2U);
}

TEST(HeadlessApiCore, OperationStopStateRowsBecomeHistory) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::vector<std::pair<CanonicalValue, std::string>> rows{
      {StopStateRow("stop_accepted", "stopping", "resource.arm.a"),
       "operation_stopping"},
      {StopStateRow("stop_participant_completed", "stopping", "resource.arm.a"),
       "operation_stopping"},
      {StopStateRow("stop_terminal_committed", "stopped", "resource.arm.a"),
       "operation_stopped"}};
  for (std::size_t index = 0U; index < rows.size(); ++index)
    IngestJournalEvent(core, rows[index].first, index + 1U);

  HistorianQuery query;
  query.components.insert("manyforge.runtime");
  query.event_types.insert("operation_stopping");
  query.event_types.insert("operation_stopped");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (Array(core.HistoryPage(query), "events").size() != rows.size() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), rows.size());
  for (const auto &stored : Array(page, "events")) {
    const auto &event = Member(stored, "event");
    const auto &payload = Member(event, "payload");
    const auto sequence = Unsigned(payload, "native_sequence");
    ASSERT_TRUE(sequence >= 1U && sequence <= rows.size());
    const auto &[row, event_type] = rows[sequence - 1U];
    EXPECT_EQ(Text(event, "event_type"), event_type);
    EXPECT_EQ(Text(event, "severity"), "INFO");
    EXPECT_EQ(Member(payload, "journal_payload"), row);
    const auto &participant = Array(
        Member(Member(payload, "journal_payload"), "operation_state"),
        "participants").front();
    EXPECT_TRUE(Member(participant, "stop").is_object());
  }
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}

// Produce the same committed payloads read by the operational events route.
// These typed journal facts do not assert a Kernel trip/Stop execution.
std::vector<manyforge::core::OperationJournalEventDTO> NativeRuleStateEvents(
    const TemporaryDirectory &directory) {
  namespace native = manyforge::core;
  native::SqliteOperationJournal journal(
      directory.path() / "native-rule-state.sqlite3",
      native::CellId(std::string(kCellId)), 8U,
      [](std::int64_t) { return "2026-10-18T10:00:00.000000001Z"; });
  const auto opened =
      journal.BeginStart(native::ApplicationStartId{0x3f9a0000000000a1ULL}, 0, 8U);
  if (!opened.ok())
    throw std::runtime_error(opened.error().message);
  native::IoRuleStateChangeDTO change{
      {UINT64_MAX, native::ActiveBundlePublicationGeneration(3U),
       native::RetainedBundleId("retained.global"),
       native::GraphRevision("graph.global")},
      {native::InterruptPolicyId("door.guard"),
       native::IoGlobalRuleState::kTripped,
       native::IoRuleAcquisitionKeyDTO{
           native::InterruptSourceId("door.source"),
           native::IoSourceIncarnation("door.epoch"), UINT64_MAX,
           {UINT64_MAX, native::InterruptSourceId("door.source"),
            UINT64_MAX}}},
      100};
  for (const auto state : {native::IoGlobalRuleState::kTripped,
                           native::IoGlobalRuleState::kClear}) {
    change.rule.state = state;
    if (state == native::IoGlobalRuleState::kClear)
      change.rule.triggering_acquisition.reset();
    ++change.occurred_at_ns;
    const std::array records{native::JournalRecord{change, "test"}};
    const auto recorded = journal.WriteBatch(records, 0U);
    if (!recorded.failures.empty())
      throw std::runtime_error(recorded.failures.front().error.message);
  }
  const auto page = journal.ReadOperationalEvents(0U);
  if (!page.ok() || page.value().events.size() != 2U)
    throw std::runtime_error("actual Core rule-state history is unavailable");
  return page.value().events;
}

TEST(HeadlessApiCore, NotifyUnknownWithTypedReasonPassesEventValidation) {
  TemporaryDirectory directory;
  namespace native = manyforge::core;
  native::SqliteOperationJournal journal(directory.path() / "notify.sqlite3",
                                         native::CellId(std::string(kCellId)), 8U);
  ASSERT_TRUE(
      journal.BeginStart(native::ApplicationStartId{0x3f9a0000000000a1ULL}, 0, 8U).ok());
  native::IoRuleStateChangeDTO change{
      {4U, native::ActiveBundlePublicationGeneration(3U),
       native::RetainedBundleId("retained.global"), native::GraphRevision("graph.global")},
      {native::InterruptPolicyId("door.guard"), native::IoGlobalRuleState::kUnknown,
       std::nullopt, native::ErrorDTO{"io_rule_source_unavailable", "lost", {{"source", "door"}}}},
      100};
  const std::array records{native::JournalRecord{change, "test"}};
  ASSERT_TRUE(journal.WriteBatch(records, 0U).failures.empty());
  const auto page = journal.ReadOperationalEvents(0U);
  ASSERT_TRUE(page.ok());
  ASSERT_EQ(page.value().events.size(), 1U);
  const auto &event = page.value().events.front();
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  EXPECT_EQ(Text(core.IngestOperationEvent(
      event.sequence, event.record_kind, std::nullopt, std::nullopt, std::nullopt,
      event.occurred_at_ns, event.occurred_at_wall, content_identity::parse_canonical_json(event.payload_json), event.payload_digest), "status"), "ingested");
}

TEST(HeadlessApiCore,
     ActualCoreRuleTransitionsReachHistoryWithNoFakeOperationAndReplaySafely) {
  TemporaryDirectory directory;
  const auto events = NativeRuleStateEvents(directory);
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto ingest = [&](const auto &event) {
    EXPECT_EQ(event.record_kind, "interrupt_rule_state_changed");
    EXPECT_FALSE(event.request_key);
    EXPECT_FALSE(event.operation_id);
    EXPECT_FALSE(event.operation_revision);
    return core.IngestOperationEvent(
        event.sequence, event.record_kind, std::nullopt, std::nullopt,
        std::nullopt, event.occurred_at_ns, event.occurred_at_wall, content_identity::parse_canonical_json(event.payload_json),
        event.payload_digest);
  };
  for (const auto &event : events)
    EXPECT_EQ(Text(ingest(event), "status"), "ingested");
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(events.back().sequence),
                 "status"),
            "complete");
  EXPECT_EQ(Text(ingest(events.front()), "status"), "already_ingested");
  EXPECT_EQ(Text(ingest(events.back()), "status"), "already_ingested");
  // The current source cursor cannot name different retained content.
  const auto &first = events.front();
  EXPECT_THROW(
      static_cast<void>(core.IngestOperationEvent(
          events.back().sequence, first.record_kind, std::nullopt,
          std::nullopt, std::nullopt, first.occurred_at_ns,
          first.occurred_at_wall, content_identity::parse_canonical_json(first.payload_json),
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
    EXPECT_EQ(Text(event, "schema_version"), "manyforge.historian_event.v3");
    EXPECT_EQ(Text(event, "event_type"), "interrupt_rule_state_changed");
    EXPECT_EQ(Text(event, "severity"), "INFO");
    const auto &payload = Member(event, "payload");
    EXPECT_EQ(Text(payload, "schema_version"),
              "manyforge.operation_history_event.v2");
    EXPECT_EQ(Text(payload, "application_start_id"), "api.epoch.1");
    for (const auto *name : {"operation_id", "operation_revision", "request_key"})
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
  EXPECT_EQ(states, (std::set<std::string>{"clear", "tripped"}));
}

TEST(HeadlessApiCore,
     RuleStateHistoryRejectsForeignContextDigestAndOperationIdentityBeforeCursor) {
  TemporaryDirectory directory;
  const auto events = NativeRuleStateEvents(directory);
  const auto &event = events.front();
  const auto payload = content_identity::parse_canonical_json(event.payload_json);
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto ingest = [&](const CanonicalValue &value, std::string digest) {
    return core.IngestOperationEvent(
        event.sequence, event.record_kind, std::nullopt, std::nullopt,
        std::nullopt, event.occurred_at_ns, event.occurred_at_wall, value, std::move(digest));
  };
  const auto &acquired = Member(payload, "triggering_acquisition");
  const auto &cursor = Member(acquired, "source_cursor");
  for (const auto &invalid : {
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
  extra.emplace_back("operation_id", CanonicalValue(9.0));
  const CanonicalValue forged(std::move(extra));
  EXPECT_THROW(static_cast<void>(ingest(
                   forged, content_identity::sha256_digest(
                               content_identity::canonical_json(forged)))),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(ingest(payload, Digest('0'))),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   event.sequence, event.record_kind, std::nullopt,
                   9U, 1U, event.occurred_at_ns,
                   event.occurred_at_wall, payload, event.payload_digest)),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(core.IngestOperationEvent(
                   event.sequence, event.record_kind,
                   "90000000000000000000000000000009",
                   std::nullopt, std::nullopt, event.occurred_at_ns,
                   event.occurred_at_wall, payload, event.payload_digest)),
               OperatorAlarmError);
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(0U), "status"),
            "complete");
  EXPECT_EQ(Text(ingest(payload, event.payload_digest), "status"), "ingested");
  EXPECT_EQ(Text(core.CompleteOperationEventSnapshot(event.sequence),
                 "status"),
            "complete");
}

TEST(HeadlessApiCore,
     KernelRuntimeCollisionStopDrivesDedicatedAlarmUntilAcceptedReset) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto acceptance = RuntimeCollisionStopEventPayload();
  IngestJournalEvent(core, acceptance, 1U);
  const auto stopping = StopStateRow("stop_accepted", "stopping", resource);
  IngestJournalEvent(core, stopping, 2U);
  static_cast<void>(core.CompleteOperationEventSnapshot(2U));
  // OD-205 (b): the rows are History only.
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  // The operations table lists the operation with the runtime monitor's Stop.
  const auto stopping_entry = OperationTableEntry(
      Member(stopping, "operation_state"), CanonicalValue(nullptr), RuntimeMonitorStop());
  static_cast<void>(ObserveOperationTable(core, {stopping_entry}));

  const auto active_snapshot = core.AlarmSnapshot();
  const auto &active = OnlyAlarm(active_snapshot);
  EXPECT_EQ(Text(active, "definition_id"),
            "manyforge.motion.runtime_collision");
  EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(active, "priority"), "critical");
  EXPECT_FALSE(  // Acknowledgement never gates motion.
      std::get<bool>(Member(active, "requires_motion_attention").storage()));
  EXPECT_EQ(Text(Member(active, "subject"), "subject_id"), "1");
  ASSERT_EQ(Array(Member(active, "subject"), "resource_ids").size(), 1U);
  EXPECT_EQ(
      std::get<std::string>(
          Array(Member(active, "subject"), "resource_ids").front().storage()),
      resource);
  EXPECT_EQ(Text(Member(active, "details"), "reason"),
            "runtime_collision_fault");
  EXPECT_EQ(Text(Member(active, "details"), "source_label"),
            "kernel.runtime_collision");
  EXPECT_EQ(Unsigned(Member(active, "details"), "stop_event_id"), 2U);
  // From memory: no journal evidence at the raise.
  EXPECT_EQ(Member(active, "details").find_object_member("native_sequence"), nullptr);
  EXPECT_EQ(Member(active, "details").find_object_member("stop_id"), nullptr);
  EXPECT_EQ(Unsigned(Member(active, "correlations"), "operation_id"), 1U);
  EXPECT_EQ(Unsigned(Member(active, "correlations"), "program_run_id"), 1U);
  EXPECT_EQ(Text(Member(active, "correlations"), "request_key"),
            kCollisionStopRequestKey);
  // An unchanged entry is not submitted again.
  static_cast<void>(ObserveOperationTable(core, {stopping_entry}));
  EXPECT_EQ(Unsigned(OnlyAlarm(core.AlarmSnapshot()), "activation_count"), 1U);

  // The contained collision ends the operation InterventionRequired; that
  // outcome raises no second (execution-fault) alarm for the same event.
  const auto intervention = OperationJournalPayload("state_observed", "intervention_required",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"reason", CanonicalValue("runtime_collision_fault")}})},
          {"message",
           CanonicalValue("Runtime collision containment stopped the operation")},
      }),
      std::nullopt, resource);
  auto intervention_state = Member(intervention, "operation_state");
  intervention_state =
      JournalField(intervention_state, "stop_event_id", CanonicalValue(2.0));
  static_cast<void>(ObserveOperationTable(
      core, {OperationTableEntry(intervention_state, CanonicalValue(nullptr),
                                 RuntimeMonitorStop())}));
  const auto intervention_snapshot = core.AlarmSnapshot();
  EXPECT_EQ(Text(OnlyAlarm(intervention_snapshot), "definition_id"),
            "manyforge.motion.runtime_collision");

  // The accepted Reset (pending in the Kernel) returns it.
  static_cast<void>(ObserveOperationTable(
      core, {OperationTableEntry(intervention_state, CanonicalValue("reset"),
                                 RuntimeMonitorStop())}));
  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "definition_id"),
            "manyforge.motion.runtime_collision");
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"),
            "operation_reset_accepted");
}

// Follow-up A: an operation closed by Recover or retry (no Reset, no row)
// leaves the table and its alarm returns.
TEST(HeadlessApiCore, OperationAlarmsReturnWhenTheOperationLeavesTheTable) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto fault = OperationJournalPayload("state_observed", "intervention_required",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{})},
          {"message", CanonicalValue("failed")}}),
      std::nullopt, resource, 3U);
  const auto stopped = StopStateRow("stop_terminal_committed", "stopped", resource);
  static_cast<void>(ObserveOperationTable(
      core, {OperationTableEntry(Member(stopped, "operation_state"), CanonicalValue(nullptr),
                                 RuntimeMonitorStop()),
             OperationTableEntry(Member(fault, "operation_state"))}));
  ASSERT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 2U);
  // Not every operation listed: the present alarms stay and the operation
  // source is not fully read.
  const auto partial = ObserveOperationTable(core, {}, 2U);
  EXPECT_FALSE(std::get<bool>(Member(partial, "fully_read").storage()));
  const auto both_alarms = core.AlarmSnapshot();
  for (const auto &alarm : Array(both_alarms, "alarms"))
    EXPECT_EQ(Text(alarm, "lifecycle_state"), "active_unacknowledged");
  // Both retired: both return.
  static_cast<void>(ObserveOperationTable(core, {}));
  const auto snapshot = core.AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 2U);
  for (const auto &alarm : Array(snapshot, "alarms")) {
    EXPECT_EQ(Text(alarm, "lifecycle_state"), "returned_unacknowledged");
    EXPECT_EQ(Text(Member(alarm, "details"), "reason"), "operation_not_faulted");
  }
}

// An entry without its alarm facts (an identity beyond the table's bound)
// raises nothing and leaves the source not fully read.
TEST(HeadlessApiCore, IncompleteTableEntryRaisesNothingAndIsNotFullyRead) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const CanonicalValue entry(CanonicalValue::Object{
      {"complete", CanonicalValue(false)},
      {"operation_id", CanonicalValue(4.0)},
      {"operation_revision", CanonicalValue(1.0)},
      {"pending_control", CanonicalValue(nullptr)},
      {"runtime_monitor_stop", CanonicalValue(nullptr)},
      {"state", CanonicalValue("failed")},
      {"stop_event_id", CanonicalValue(nullptr)},
  });
  const auto observed = ObserveOperationTable(core, {entry});
  EXPECT_FALSE(std::get<bool>(Member(observed, "fully_read").storage()));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  const auto domain_snapshot = core.AlarmSnapshot();
  const auto &domains = Array(domain_snapshot, "domains");
  const auto motion = std::find_if(domains.begin(), domains.end(), [](const auto &domain) {
    return Text(domain, "domain") == "motion";
  });
  ASSERT_NE(motion, domains.end());
  EXPECT_FALSE(std::get<bool>(Member(*motion, "reconciled").storage()));
  EXPECT_GE(Unsigned(*motion, "unknown_source_count"), 1U);
  static_cast<void>(resource);
}

// An alarm subject above the alarm's bound is refused exactly as a journal
// row's was: the observation fails and is evaluated again next time.
TEST(HeadlessApiCore, OperationTableSubjectAboveTheAlarmBoundIsRefused) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto entry = OperationTableEntry(Member(
      OperationJournalPayload("state_observed", "failed", CanonicalValue(nullptr)),
      "operation_state"));
  entry = JournalField(entry, "resource_total", CanonicalValue(17.0));
  EXPECT_THROW(static_cast<void>(ObserveOperationTable(core, {entry})),
               OperatorAlarmError);
  EXPECT_THROW(static_cast<void>(ObserveOperationTable(core, {entry})),
               OperatorAlarmError);
}

TEST(HeadlessApiCore,
     KernelRuntimeMonitorObservationLossDrivesItsOwnAlarmUntilAcceptedReset) {
  // Live incident 2026-10-29: the containment Stop for lost measured robot
  // state was annunciated as a runtime collision. It keeps the same Stop
  // source and outcome but raises its own alarm. OD-217: so does the Stop
  // for collision checks that did not run in time.
  struct MonitorCase {
    const char *reason;
    const char *definition_id;
    const char *title;
    const char *error_message;
  };
  for (const auto &monitor_case :
       {MonitorCase{"runtime_monitor_observation_lost",
                    "manyforge.motion.runtime_monitor_observation_lost",
                    "Robot position feedback lost",
                    "Runtime monitor stopped the operation: measured robot state was lost"},
        MonitorCase{"runtime_monitor_checks_stopped",
                    "manyforge.motion.runtime_monitor_checks_stopped",
                    "Collision checks stopped",
                    "Runtime monitor stopped the operation: its collision checks did not "
                    "run in time"}}) {
    SCOPED_TRACE(monitor_case.reason);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishActiveConfiguration(manager_status, *configurations.single);
    IngestJournalEvent(
        core, RuntimeCollisionStopEventPayload(monitor_case.reason),
        1U);
    const auto stopping = StopStateRow("stop_accepted", "stopping", resource);
    IngestJournalEvent(core, stopping, 2U);
    EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
    static_cast<void>(ObserveOperationTable(
        core, {OperationTableEntry(Member(stopping, "operation_state"), CanonicalValue(nullptr),
                                   RuntimeMonitorStop(monitor_case.reason))}));

    const auto active_snapshot = core.AlarmSnapshot();
    const auto &active = OnlyAlarm(active_snapshot);
    EXPECT_EQ(Text(active, "definition_id"), monitor_case.definition_id);
    EXPECT_EQ(Text(active, "title"), monitor_case.title);
    EXPECT_EQ(Text(active, "lifecycle_state"), "active_unacknowledged");
    EXPECT_EQ(Text(active, "priority"), "critical");
    EXPECT_EQ(Text(Member(active, "details"), "reason"), monitor_case.reason);
    ASSERT_EQ(Array(Member(active, "subject"), "resource_ids").size(), 1U);
    EXPECT_EQ(
        std::get<std::string>(
            Array(Member(active, "subject"), "resource_ids").front().storage()),
        resource);
    EXPECT_EQ(Unsigned(Member(active, "correlations"), "program_run_id"), 1U);
    if (std::string_view(monitor_case.reason) == "runtime_monitor_checks_stopped") {
      EXPECT_EQ(Text(active, "message"),
                "The collision checks of " + resource +
                    " did not run in time, so the robot was stopped. Check the load on the "
                    "controller computer, then Recover.");
    }

    // Its InterventionRequired outcome raises no execution-fault alarm.
    const auto intervention = OperationJournalPayload("state_observed", "intervention_required",
        CanonicalValue(CanonicalValue::Object{
            {"code", CanonicalValue(monitor_case.reason)},
            {"details",
             CanonicalValue(CanonicalValue::Object{
                 {"reason", CanonicalValue(monitor_case.reason)}})},
            {"message", CanonicalValue(monitor_case.error_message)},
        }),
        std::nullopt, resource);
    const auto intervention_state = JournalField(
        Member(intervention, "operation_state"), "stop_event_id", CanonicalValue(2.0));
    static_cast<void>(ObserveOperationTable(
        core, {OperationTableEntry(intervention_state, CanonicalValue(nullptr),
                                   RuntimeMonitorStop(monitor_case.reason))}));
    const auto intervention_snapshot = core.AlarmSnapshot();
    EXPECT_EQ(Text(OnlyAlarm(intervention_snapshot), "definition_id"),
              monitor_case.definition_id);

    // The accepted Reset returns exactly this alarm.
    static_cast<void>(ObserveOperationTable(
        core, {OperationTableEntry(intervention_state, CanonicalValue("reset"),
                                   RuntimeMonitorStop(monitor_case.reason))}));
    const auto returned_snapshot = core.AlarmSnapshot();
    const auto &returned = OnlyAlarm(returned_snapshot);
    EXPECT_EQ(Text(returned, "definition_id"), monitor_case.definition_id);
    EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
    EXPECT_EQ(Text(Member(returned, "details"), "reason"),
              "operation_reset_accepted");
  }
}

TEST(HeadlessApiCore, NominalStoppedFailureIsErrorButOrdinaryStopRemainsInfo) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.history"),
                       Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  IngestJournalEvent(core, RuntimeCollisionStopEventPayload(), 1U);
  auto collision = CapturedOperation(resource, CollisionFailure());
  collision = JournalField(collision, "operation_state",
                         JournalField(Member(collision, "operation_state"),
                                    "state", CanonicalValue("stopped")));
  IngestJournalEvent(core, collision, 2U);
  IngestJournalEvent(core,
                   OperationJournalPayload("state_observed", "stopped",
                                           CanonicalValue(nullptr),
                                           std::nullopt, resource, 2U),
                   3U);
  // Rows are History only (OD-205 (b)).
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
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
    if (Unsigned(payload, "operation_id") == 2U) {
      EXPECT_EQ(Text(event, "severity"), "INFO");
    } else {
      EXPECT_EQ(Text(event, "severity"), "ERROR");
      EXPECT_EQ(Text(payload, "message"),
                "[move / 1] execution_aborted: first collision");
      EXPECT_EQ(Member(payload, "journal_payload"), collision);
    }
  }
}





TEST(HeadlessApiCore,
     AcceptedResetClearsMatchingOperationFaultAndConflictsFailClosed) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")},
  });
  const auto failure =
      OperationJournalPayload("state_observed",
                              "intervention_required", error, std::nullopt,
                              resource);
  const auto failure_json = content_identity::canonical_json(failure);
  const auto failure_digest = content_identity::sha256_digest(failure_json);
  static_cast<void>(core.IngestOperationEvent(
      4U, "state_observed", std::nullopt, 1U, 1U,
      200U, "2026-08-07T10:00:00.000000004Z", failure, failure_digest));

  auto conflicting = failure;
  const auto conflicting_digest = content_identity::sha256_digest(
      content_identity::canonical_json(conflicting));
  try {
    static_cast<void>(core.IngestOperationEvent(
        4U, "state_observed", std::nullopt, 1U,
        1U, 201U, "2026-08-07T10:00:00.000000005Z", std::move(conflicting), conflicting_digest));
    FAIL() << "one native sequence accepted conflicting event content";
  } catch (const OperatorAlarmError &error_value) {
    EXPECT_EQ(error_value.code(), "headless_operation_event_identity_conflict");
  }

  const auto reset =
      OperationJournalPayload("control_accepted",
                              "intervention_required", error, "reset",
                              resource);
  const auto reset_json = content_identity::canonical_json(reset);
  static_cast<void>(core.IngestOperationEvent(
      5U, "control_accepted", "50000000000000000000000000000004",
      1U, 1U, 202U, "2026-08-07T10:00:00.000000006Z", reset, content_identity::sha256_digest(reset_json)));
  static_cast<void>(core.CompleteOperationEventSnapshot(5U));
  // The rows raised and cleared nothing (OD-205 (b)); the table does.
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  const auto &state = Member(failure, "operation_state");
  static_cast<void>(ObserveOperationTable(core, {OperationTableEntry(state)}));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "active_unacknowledged");
  // The Reset's acceptance (pending in the Kernel) returns it.
  static_cast<void>(ObserveOperationTable(
      core, {OperationTableEntry(state, CanonicalValue("reset"))}));
  const auto alarms = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(alarms);
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"), "operation_reset_accepted");
  // Its cleanup fails: the settled fault (no Reset pending) is raised again.
  auto failed_again = JournalField(state, "operation_revision", CanonicalValue(2.0));
  static_cast<void>(ObserveOperationTable(core, {OperationTableEntry(failed_again)}));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "lifecycle_state"),
            "active_unacknowledged");
}

TEST(HeadlessApiCore, ActiveTcpEventsRequireNativeV3) {
  TemporaryDirectory directory;
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.active_tcp"),
                       Clocks());
  const auto ingest = [&](const std::string &api_version) {
    const CanonicalValue payload(CanonicalValue::Object{
        {"api_version", CanonicalValue(api_version)},
        {"component_id", CanonicalValue("robot")},
        {"previous", CanonicalValue(nullptr)},
        {"entry",
         CanonicalValue(CanonicalValue::Object{
             {"target", CanonicalValue(CanonicalValue::Object{
                            {"component_id", CanonicalValue("tool")},
                            {"command_target_id", CanonicalValue("gripper")}})},
             {"frame", CanonicalValue(CanonicalValue::Object{
                           {"component_id", CanonicalValue("robot")},
                           {"frame_id", CanonicalValue("tcp")}})}})},
        {"source", CanonicalValue("program")},
        {"offset_sent", CanonicalValue(false)},
        {"registration_generation", CanonicalValue(1.0)},
        {"sequence", CanonicalValue(1.0)},
    });
    const auto digest = content_identity::sha256_digest(
        content_identity::canonical_json(payload));
    return core.IngestOperationEvent(1U, "active_tcp_changed",
                                     std::nullopt, std::nullopt, std::nullopt,
                                     1, "2026-11-07T10:00:00.000000001Z", payload, digest);
  };

  EXPECT_THROW(ingest("manyforge.active_tcp_change.v2"), OperatorAlarmError);
  EXPECT_NO_THROW(ingest("manyforge.active_tcp_change.v3"));
}

TEST(HeadlessApiCore,
     FaultRefusedBeforeAnyConfigurationIsAdmittedOnceThePublicationAppears) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  const auto failure = OperationJournalPayload("state_observed", "failed",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{})},
          {"message", CanonicalValue("failed")},
      }),
      std::nullopt, resource, 7U);
  static_cast<void>(core.IngestOperationEvent(
      1U, "state_observed", std::nullopt, 7U, 1U, 100U,
      "2026-10-02T10:00:00.000000001Z", failure,
      content_identity::sha256_digest(content_identity::canonical_json(failure))));
  // The unchanged fault is observed repeatedly while no configuration is
  // published: it stays refused without being resubmitted or re-recorded,
  // and its one alarm-scope alarm is neither repeated nor reactivated.
  const auto fault_entry = OperationTableEntry(Member(failure, "operation_state"));
  for (int repeat = 0; repeat < 3; ++repeat) {
    static_cast<void>(ObserveOperationTable(core, {fault_entry}));
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
    EXPECT_EQ(Text(Member(named.front(), "subject"), "subject_id"), "7");
    EXPECT_EQ(Array(Member(named.front(), "subject"), "resource_ids"),
              (CanonicalValue::Array{CanonicalValue(resource)}));
  }

  // Manager publishes the configuration; the next observation of the
  // projection reconsiders the still-current fault once and admits it, and
  // the alarm-scope condition returns to normal. The admitted fault needs
  // acknowledgement and so never gates motion.
  PublishActiveConfiguration(manager_status, *configurations.single);
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
  EXPECT_EQ(Text(Member(*fault, "subject"), "subject_id"), "7");
  EXPECT_EQ(ScopeAlarm(admitted), nullptr);
  EXPECT_EQ(Unsigned(admitted, "motion_attention_count"), 0U);
  EXPECT_TRUE(Array(admitted, "motion_attention_membership").empty());
  static_cast<void>(ObserveOperationTable(core, {fault_entry}));
  EXPECT_EQ(Array(core.AlarmSnapshot(), "alarms").size(), 1U);

  // Its normal terminal state takes it off the table: it returns like any
  // admitted fault.
  static_cast<void>(ObserveOperationTable(core, {}));
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

// The start the previous-session report describes.
const std::string kPreviousStart = "00000000000000aa";

CanonicalValue PreviousSessionReport(const std::string &message =
    "Previous session journals retained; old events are not imported into History.") {
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.workbench_previous_session.v3")},
      {"session_id", CanonicalValue(std::string(32U, 'a'))},
      {"archive_relative_path", CanonicalValue("previous-runs/" + std::string(32U, 'a'))},
      {"shutdown", CanonicalValue("clean")},
      {"effects", CanonicalValue("closed")},
      {"history_coverage", CanonicalValue("not_imported")},
      {"journal_families", CanonicalValue(CanonicalValue::Array{
          CanonicalValue("program_cycle"), CanonicalValue("manager"),
          CanonicalValue("kernel_operation")})},
      {"message", CanonicalValue(message)},
      {"unfinished_operations", CanonicalValue(CanonicalValue::Array{
          CanonicalValue(CanonicalValue::Object{
              {"operation_id", CanonicalValue(7.0)},
              {"disposition", CanonicalValue("host_replaced")}})})},
      {"unfinished_operation_count", CanonicalValue(1.0)},
      {"application_start_id", CanonicalValue(kPreviousStart)},
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
  config.application_start_id = "api.epoch.2";
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

// A report of the previous version (v2) names the scene's journal family too.
CanonicalValue Version2PreviousSessionReport(const CanonicalValue::Array &families) {
  auto fields = PreviousSessionReport().as_object();
  for (auto &[name, value] : fields) {
    if (name == "schema_version") value = CanonicalValue("manyforge.workbench_previous_session.v2");
    if (name == "journal_families") value = CanonicalValue(families);
  }
  return CanonicalValue(std::move(fields));
}

const CanonicalValue::Array kVersion2Families{
    CanonicalValue("program_cycle"), CanonicalValue("scene"),
    CanonicalValue("manager"), CanonicalValue("kernel_operation")};

TEST(HeadlessApiCore, PreviousSessionReportOfTheEarlierVersionIsFiledAsRecorded) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto report = Version2PreviousSessionReport(kVersion2Families);
  EXPECT_EQ(Text(core.AppendWorkbenchPreviousSessionReport(report), "status"), "stored");
  HistorianQuery query;
  query.components.insert("manyforge.workbench");
  query.event_types.insert("previous_session_report");
  const auto page = core.HistoryPage(query);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  EXPECT_EQ(content_identity::canonical_json(Member(Member(Array(page, "events").front(), "event"), "payload")),
            content_identity::canonical_json(report));
}

TEST(HeadlessApiCore, PreviousSessionReportFamiliesFollowItsVersion) {
  TemporaryDirectory directory;
  const auto version3 = PreviousSessionReport().as_object();
  auto version3_with_scene = version3;
  for (auto &[name, value] : version3_with_scene) {
    if (name == "journal_families") value = CanonicalValue(kVersion2Families);
  }
  auto unknown_version = version3;
  for (auto &[name, value] : unknown_version) {
    if (name == "schema_version") value = CanonicalValue("manyforge.workbench_previous_session.v1");
  }
  const std::array refused{
      CanonicalValue(std::move(version3_with_scene)),
      Version2PreviousSessionReport(Member(PreviousSessionReport(), "journal_families").as_array()),
      CanonicalValue(std::move(unknown_version))};
  for (const auto &report : refused) {
    HeadlessApiCore core(Config(directory, "api.epoch.invalid"), Clocks());
    try {
      static_cast<void>(core.AppendWorkbenchPreviousSessionReport(report));
      FAIL() << content_identity::canonical_json(report);
    } catch (const OperatorAlarmError &error) {
      EXPECT_EQ(error.code(), "headless_previous_session_report_invalid");
    }
  }
}

TEST(HeadlessApiCore, PreviousSessionReportRefusesChangedFamiliesAndImportedClaims) {
  TemporaryDirectory directory;
  for (const auto &[field, replacement] : CanonicalValue::Object{
           {"journal_families", CanonicalValue(CanonicalValue::Array{})},
           {"history_coverage", CanonicalValue("imported")},
           {"shutdown", CanonicalValue("verified")},
           {"effects", CanonicalValue("safe")},
           {"archive_relative_path", CanonicalValue("../foreign")},
           // The start is read with the operations: both or neither.
           {"application_start_id", CanonicalValue(nullptr)},
           {"application_start_id", CanonicalValue("api.epoch.x")}}) {
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
    if (name == "archive_relative_path") value = CanonicalValue("previous-runs/" + identity);
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
  config.application_start_id = "api.epoch.2";
  {
    HeadlessApiCore replacement(config, Clocks());
    EXPECT_EQ(Text(replacement.ObserveWorkbenchPreviousSessionReport(report), "status"), "acknowledged");
    EXPECT_TRUE(PreviousSessionAlarms(replacement).empty());
  }
  config.application_start_id = "api.epoch.3";
  HeadlessApiCore next(config, Clocks());
  EXPECT_EQ(Text(next.ObserveWorkbenchPreviousSessionReport(
      ClassifiedPreviousSessionReport("unknown", "unknown", std::string(32U, 'b'))), "status"), "registered");
  EXPECT_EQ(PreviousSessionAlarms(next).size(), 1U);
}

TEST(HeadlessApiCore, PreviousSessionReportAndUncleanAlarmAreFiledUnderTheStartTheyDescribe) {
  TemporaryDirectory directory;
  auto config = Config(directory, "0000000000000bbb");
  HeadlessApiCore core(config, Clocks());
  const auto report = ClassifiedPreviousSessionReport("unknown", "unknown");
  static_cast<void>(core.AppendWorkbenchPreviousSessionReport(report));
  EXPECT_EQ(Text(core.ObserveWorkbenchPreviousSessionReport(report), "status"), "registered");
  HistorianQuery previous;
  previous.correlations = {{"operation_id", "7"}};
  previous.application_start_id = kPreviousStart;
  previous.components = {"manyforge.alarm"};
  // Alarm rows reach History through the asynchronous writer.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (Array(core.HistoryPage(previous), "events").empty() &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  const auto previous_page = core.HistoryPage(previous);
  for (const auto &row : Array(previous_page, "events")) {
    EXPECT_EQ(Text(row, "application_start_id"), kPreviousStart);
  }
  ASSERT_FALSE(Array(core.HistoryPage(previous), "events").empty());
  previous.components = {"manyforge.workbench"};
  ASSERT_EQ(Array(core.HistoryPage(previous), "events").size(), 1U);
  // The current start's operation 7 shows neither.
  HistorianQuery current;
  current.correlations = {{"operation_id", "7"}};
  EXPECT_TRUE(Array(core.HistoryPage(current), "events").empty());
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
  config.application_start_id = "api.epoch.2";
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
      SqliteEventHistoryStore store(config.historian_directory,
                                   config.application_start_id,
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
  config.application_start_id = "api.epoch.2";
  HeadlessApiCore replacement(config, Clocks());
  EXPECT_EQ(Text(replacement.ObserveWorkbenchPreviousSessionReport(report), "status"), "acknowledged");
  EXPECT_TRUE(PreviousSessionAlarms(replacement).empty());
}

namespace {
constexpr char kFaultRequestKey[] = "40000000000000040000000000000001";

// No fact names a Kernel owner any more (contract 1.4); Headless correlates
// the faults with its own start.
CanonicalValue RuntimeFaultSnapshot(bool runner, bool journal,
                                    bool internal = false) {
  const CanonicalValue journal_fact(CanonicalValue::Object{
      {"write_kind", CanonicalValue("state_observed")},
      {"error", CanonicalValue(CanonicalValue::Object{
                    {"code", CanonicalValue("sqlite_write_failed")},
                    {"message", CanonicalValue("disk full")},
                    {"details", CanonicalValue(CanonicalValue::Object{})}})},
      {"operation_id", CanonicalValue(1.0)},
      {"operation_revision", CanonicalValue(2.0)},
      {"request_key", CanonicalValue(kFaultRequestKey)},
      {"program_run_id", CanonicalValue(1.0)},
      {"execution_scope",
       CanonicalValue(CanonicalValue::Object{
           {"node_instance_id", CanonicalValue("move")},
           {"node_attempt_id", CanonicalValue(1.0)}})},
      {"queue_full", CanonicalValue(false)}});
  const CanonicalValue runner_fact(CanonicalValue::Object{
      {"application_start_id", CanonicalValue("3f9a0000000000a1")},
      {"failure_predicate", CanonicalValue("program_runner_liveness_probe")},
      {"message", CanonicalValue("connection refused")},
      {"fault_code", CanonicalValue("program_runner_contact_lost")},
      {"established_cycle",
       CanonicalValue(CanonicalValue::Object{
           {"cycle_generation", CanonicalValue(2.0)},
           {"state", CanonicalValue("running")},
           {"program_run_id", CanonicalValue(1.0)},
           {"cycle_claim_generation", CanonicalValue(1.0)},
           {"terminal_or_settlement_reference", CanonicalValue(nullptr)},
           {"accepted_reset", CanonicalValue(nullptr)}})}});
  const CanonicalValue internal_fact(CanonicalValue::Object{
      {"reason_code",
       CanonicalValue("scheduler_runtime_safety_callback_exception")},
      {"later_count", CanonicalValue(3.0)}});
  return CanonicalValue(CanonicalValue::Object{
      {"operation_journal_write_fault",
       journal ? journal_fact : CanonicalValue(nullptr)},
      {"program_runner_loss", runner ? runner_fact : CanonicalValue(nullptr)},
      {"kernel_internal_fault",
       internal ? internal_fact : CanonicalValue(nullptr)}});
}
} // namespace

TEST(HeadlessApiCore, RunnerFaultKeepsItsActualPredicateAndDiagnostic) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto facts = RuntimeFaultSnapshot(true, false);
  auto fault =
      WithField(Member(facts, "program_runner_loss"), "failure_predicate",
                CanonicalValue("program_runner_fault"));
  fault = WithField(fault, "message", CanonicalValue("exact key ACK missing"));
  // A fault without its own typed code is refused.
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   WithField(facts, "program_runner_loss", fault))),
               std::exception);
  fault = WithField(fault, "fault_code",
                    CanonicalValue("program_bbkey_publication_failed"));
  facts = WithField(facts, "program_runner_loss", fault);
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(facts), "changed")
          .as_bool());
  const auto snapshot = core.AlarmSnapshot();
  const auto *alarm =
      AlarmFor(snapshot, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(Member(*alarm, "details"), "failure_predicate"),
            "program_runner_fault");
  EXPECT_EQ(Text(Member(*alarm, "details"), "message"),
            "exact key ACK missing");
  EXPECT_EQ(Text(Member(*alarm, "details"), "fault_code"),
            "program_bbkey_publication_failed");
  EXPECT_EQ(Member(Member(*alarm, "details"), "established_cycle"),
            Member(fault, "established_cycle"));
  // The fault ends with its fact; the returned alarm keeps its diagnostic.
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(
                 RuntimeFaultSnapshot(false, false)),
             "changed")
          .as_bool());
  const auto retained = core.AlarmSnapshot();
  const auto *same =
      AlarmFor(retained, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(same, nullptr);
  EXPECT_EQ(Text(*same, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(*same, "activation_id"), Text(*alarm, "activation_id"));
  EXPECT_EQ(Text(Member(*same, "details"), "failure_predicate"),
            "program_runner_fault");
}

TEST(HeadlessApiCore, RunnerLossAlarmClearsWhenTheRunnerIsHeardAgain) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto lost = RuntimeFaultSnapshot(true, false);
  const auto heard = RuntimeFaultSnapshot(false, false);
  ASSERT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(lost), "changed").as_bool());
  const auto first = core.AlarmSnapshot();
  const auto *raised = AlarmFor(first, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(raised, nullptr);
  EXPECT_EQ(Text(*raised, "lifecycle_state"), "active_unacknowledged");
  // Repeated reports of the same loss change nothing.
  EXPECT_FALSE(
      Member(core.ObserveRuntimeFaultConditions(lost), "changed").as_bool());
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(heard), "changed").as_bool());
  const auto cleared = core.AlarmSnapshot();
  const auto *returned =
      AlarmFor(cleared, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(returned, nullptr);
  EXPECT_EQ(Text(*returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Member(*returned, "correlations").find_object_member("source_owner_epoch"), nullptr);
  EXPECT_FALSE(
      Member(core.ObserveRuntimeFaultConditions(heard), "changed").as_bool());
  // A second loss raises a new activation.
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(lost), "changed").as_bool());
  const auto second = core.AlarmSnapshot();
  const auto *again = AlarmFor(second, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(again, nullptr);
  EXPECT_EQ(Text(*again, "lifecycle_state"), "active_unacknowledged");
  EXPECT_NE(Text(*again, "activation_id"), Text(*raised, "activation_id"));
}

TEST(HeadlessApiCore, KernelInternalFaultRaisesItsTypedCauseOnce) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto facts = RuntimeFaultSnapshot(false, false, true);
  EXPECT_TRUE(
      Member(core.ObserveRuntimeFaultConditions(facts), "changed")
          .as_bool());
  const auto snapshot = core.AlarmSnapshot();
  const auto *alarm =
      AlarmFor(snapshot, "manyforge.runtime.kernel_internal_fault");
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(Member(*alarm, "details"), "reason_code"),
            "scheduler_runtime_safety_callback_exception");
  // History files the row under its start; no correlation repeats it.
  EXPECT_EQ(Member(*alarm, "correlations").find_object_member("source_owner_epoch"), nullptr);
  // A higher later_count is the same fault: no second activation.
  EXPECT_FALSE(
      Member(core.ObserveRuntimeFaultConditions(
                 WithField(facts, "kernel_internal_fault",
                           WithField(Member(facts, "kernel_internal_fault"),
                                     "later_count", CanonicalValue(9.0)))),
             "changed")
          .as_bool());
  auto unknown = Member(facts, "kernel_internal_fault");
  unknown = WithField(unknown, "reason_code", CanonicalValue(""));
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   WithField(facts, "kernel_internal_fault", unknown))),
               std::exception);
  // Its containment is a Halt, which cannot be refused: the fact is the
  // closed set reason_code, later_count.
  const auto with_stop_field =
      JournalField(Member(facts, "kernel_internal_fault"), "stop_accepted",
                   CanonicalValue(true));
  EXPECT_THROW(static_cast<void>(core.ObserveRuntimeFaultConditions(
                   WithField(facts, "kernel_internal_fault", with_stop_field))),
               std::exception);
}

TEST(HeadlessApiCore,
     RuntimeFaultsRaiseOnceAndTheRunnerLossAndJournalFaultClearOnNull) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto none = RuntimeFaultSnapshot(false, false);
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(none), "changed")
          .storage()));
  const auto facts = RuntimeFaultSnapshot(true, true);
  EXPECT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(facts), "changed")
          .storage()));
  const auto raised = core.AlarmSnapshot();
  const auto *runner = AlarmFor(raised, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(Text(*runner, "lifecycle_state"), "active_unacknowledged");
  EXPECT_TRUE(
      std::get<bool>(Member(*runner, "acknowledgement_required").storage()));
  static_cast<void>(core.AcknowledgeAlarm(
      Text(*runner, "condition_id"), Text(*runner, "activation_id"),
      Unsigned(*runner, "record_revision"),
      Caller("manyforge.runtime.program_runner_loss"), std::nullopt, true));
  // Lost operation records need no acknowledgement: active while the fact is.
  const auto *journal =
      AlarmFor(raised, "manyforge.runtime.operation_journal_write_fault");
  ASSERT_NE(journal, nullptr);
  EXPECT_EQ(Text(*journal, "lifecycle_state"), "active_acknowledged");
  EXPECT_FALSE(
      std::get<bool>(Member(*journal, "acknowledgement_required").storage()));
  EXPECT_EQ(Member(*journal, "correlations").find_object_member("source_owner_epoch"), nullptr);
  const auto first_activation = Text(*journal, "activation_id");
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(facts), "changed")
          .storage()));
  // Both end with their facts.
  EXPECT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(none), "changed")
          .storage()));
  const auto cleared = core.AlarmSnapshot();
  EXPECT_EQ(AlarmFor(cleared, "manyforge.runtime.operation_journal_write_fault"),
            nullptr);
  EXPECT_EQ(AlarmFor(cleared, "manyforge.runtime.program_runner_loss"),
            nullptr);
  EXPECT_FALSE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(none), "changed")
          .storage()));
  // A later loss episode raises a new activation.
  EXPECT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(
                 RuntimeFaultSnapshot(false, true)),
             "changed")
          .storage()));
  const auto again = core.AlarmSnapshot();
  const auto *second =
      AlarmFor(again, "manyforge.runtime.operation_journal_write_fault");
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(Text(*second, "lifecycle_state"), "active_acknowledged");
  EXPECT_NE(Text(*second, "activation_id"), first_activation);
}

namespace {

constexpr std::string_view kComponentFaultAlarm =
    "manyforge.runtime.component_fault";

// One `{component_id, fault}` entry as the event pump passes it (no since_ns).
CanonicalValue ComponentFaultEntry(const std::string &component_id,
                                   const std::string &code,
                                   const std::string &message = "Fault") {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(component_id)},
      {"fault", CanonicalValue(CanonicalValue::Object{
                    {"clearable", CanonicalValue(false)},
                    {"code", CanonicalValue(code)},
                    {"message", CanonicalValue(message)},
                    {"operator_action", CanonicalValue("recover")},
                })},
  });
}

CanonicalValue ComponentFaults(CanonicalValue::Array entries = {}) {
  return CanonicalValue(std::move(entries));
}

bool Changed(const CanonicalValue &result) {
  return std::get<bool>(Member(result, "changed").storage());
}

// The listed component-fault records of one component.
std::vector<CanonicalValue> ComponentFaultAlarms(HeadlessApiCore &core,
                                                 const std::string &component_id) {
  std::vector<CanonicalValue> found;
  const auto snapshot = core.AlarmSnapshot();
  for (const auto &alarm : Array(snapshot, "alarms"))
    if (Text(alarm, "definition_id") == kComponentFaultAlarm &&
        Text(Member(alarm, "subject"), "subject_id") == component_id)
      found.push_back(alarm);
  return found;
}

// The component-fault History rows, oldest first, once `count` are written.
std::vector<CanonicalValue> ComponentFaultHistory(HeadlessApiCore &core,
                                                  const std::size_t count) {
  HistorianQuery query;
  query.components = {"manyforge.alarm"};
  std::vector<CanonicalValue> rows;
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  do {
    rows.clear();
    const auto page = core.HistoryPage(query);
    for (const auto &stored : Array(page, "events")) {
      const auto &event = Member(stored, "event");
      const auto *definition =
          Member(event, "payload").find_object_member("definition_id");
      if (definition != nullptr && definition->is_string() &&
          definition->as_string() == kComponentFaultAlarm)
        rows.insert(rows.begin(), event); // the page is newest first
    }
    if (rows.size() >= count) break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  return rows;
}

} // namespace

// OD-247/OD-248: one alarm per component in fault, raised and returned to
// normal on edges, listed until acknowledged.
TEST(HeadlessApiCore, ComponentFaultAlarmIsRaisedOnEdgesAndListedUntilAcknowledged) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  // No fault: nothing is registered or submitted.
  EXPECT_FALSE(Changed(core.ObserveComponentFaults(ComponentFaults())));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());

  auto entry = ComponentFaultEntry("robot", "fault_communication",
                                   "Robot status report missing");
  auto fault = Member(entry, "fault").as_object();
  fault.emplace_back("vendor_code", CanonicalValue("C19"));
  fault.emplace_back("concerns",
                     CanonicalValue(CanonicalValue::Array{CanonicalValue("robot_io")}));
  entry = WithField(entry, "fault", CanonicalValue(std::move(fault)));
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults({entry}))));
  auto listed = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(listed.size(), 1U);
  const auto raised = listed.front();
  EXPECT_EQ(Text(raised, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(raised, "title"), "Component fault");
  EXPECT_EQ(Text(raised, "message"), "Fault on component robot");
  EXPECT_EQ(Text(raised, "priority"), "high");
  EXPECT_TRUE(std::get<bool>(Member(raised, "acknowledgement_required").storage()));
  EXPECT_FALSE(std::get<bool>(Member(raised, "requires_motion_attention").storage()));
  EXPECT_EQ(Text(Member(raised, "subject"), "subject_kind"), "component");
  EXPECT_TRUE(Array(Member(raised, "subject"), "resource_ids").empty());
  const auto &details = Member(raised, "details");
  EXPECT_EQ(Text(Member(details, "failure"), "code"), "fault_communication");
  EXPECT_EQ(Text(Member(details, "failure"), "message"), "Robot status report missing");
  EXPECT_EQ(Text(details, "operator_action"), "recover");
  EXPECT_FALSE(std::get<bool>(Member(details, "clearable").storage()));
  EXPECT_EQ(Text(details, "vendor_code"), "C19");
  EXPECT_EQ(Array(details, "concerns").size(), 1U);
  EXPECT_TRUE(Member(raised, "correlations").as_object().empty());
  // No motion gate.
  EXPECT_TRUE(Array(core.OperatorAlarmMotionAttention(), "members").empty());

  // The same fault again, even with another message, submits nothing.
  const auto generation = Unsigned(core.AlarmSnapshot(), "generation");
  EXPECT_FALSE(Changed(core.ObserveComponentFaults(ComponentFaults({entry}))));
  EXPECT_FALSE(Changed(core.ObserveComponentFaults(ComponentFaults(
      {ComponentFaultEntry("robot", "fault_communication", "Still missing")}))));
  EXPECT_EQ(Unsigned(core.AlarmSnapshot(), "generation"), generation);

  // The fault ends: returned to normal, still listed.
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults())));
  listed = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(listed.size(), 1U);
  EXPECT_EQ(Text(listed.front(), "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(listed.front(), "activation_id"), Text(raised, "activation_id"));
  EXPECT_EQ(Text(Member(listed.front(), "details"), "reason"), "fault_ended");
  EXPECT_FALSE(Changed(core.ObserveComponentFaults(ComponentFaults())));

  // Acknowledged: it leaves the list.
  static_cast<void>(core.AcknowledgeAlarm(
      Text(listed.front(), "condition_id"), Text(listed.front(), "activation_id"),
      Unsigned(listed.front(), "record_revision"), Caller(), std::nullopt, true));
  EXPECT_TRUE(ComponentFaultAlarms(core, "robot").empty());

  // History: raised, returned, acknowledged.
  const auto rows = ComponentFaultHistory(core, 3U);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(Text(rows[0], "event_type"), "operator_alarm_activated");
  EXPECT_EQ(Text(rows[1], "event_type"), "operator_alarm_returned");
  EXPECT_EQ(Text(rows[2], "event_type"), "operator_alarm_acknowledged");
  for (const auto &row : rows) {
    EXPECT_EQ(Text(row, "component"), "manyforge.alarm");
    EXPECT_EQ(Text(Member(row, "payload"), "activation_id"),
              Text(raised, "activation_id"));
    EXPECT_EQ(Text(Member(Member(row, "payload"), "subject"), "subject_id"), "robot");
  }
  EXPECT_EQ(Text(Member(Member(rows[0], "payload"), "details"), "vendor_code"), "C19");

  // The fault comes back: a new occurrence.
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults({entry}))));
  listed = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(listed.size(), 1U);
  EXPECT_EQ(Text(listed.front(), "lifecycle_state"), "active_unacknowledged");
  EXPECT_NE(Text(listed.front(), "activation_id"), Text(raised, "activation_id"));
}

// OD-248 (2): one alarm per component; a changed code while the fault lasts
// returns the earlier occurrence and raises a new one at once.
TEST(HeadlessApiCore, ComponentFaultCodeChangeReturnsTheEarlierOccurrenceAndRaisesANewOne) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ASSERT_TRUE(Changed(core.ObserveComponentFaults(
      ComponentFaults({ComponentFaultEntry("robot", "fault_stream_stale")}))));
  const auto first = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(first.size(), 1U);
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults(
      {ComponentFaultEntry("robot", "fault_communication", "Lost")}))));
  const auto second = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(second.size(), 1U); // the list shows the new occurrence
  EXPECT_EQ(Text(second.front(), "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Text(second.front(), "condition_id"), Text(first.front(), "condition_id"));
  EXPECT_NE(Text(second.front(), "activation_id"), Text(first.front(), "activation_id"));
  EXPECT_EQ(Text(Member(Member(second.front(), "details"), "failure"), "code"),
            "fault_communication");
  // The earlier occurrence is in History: raised and returned.
  const auto rows = ComponentFaultHistory(core, 3U);
  ASSERT_EQ(rows.size(), 3U);
  EXPECT_EQ(Text(rows[0], "event_type"), "operator_alarm_activated");
  EXPECT_EQ(Text(rows[1], "event_type"), "operator_alarm_returned");
  EXPECT_EQ(Text(Member(rows[1], "payload"), "activation_id"),
            Text(first.front(), "activation_id"));
  EXPECT_EQ(Text(rows[2], "event_type"), "operator_alarm_activated");
  EXPECT_EQ(Text(Member(rows[2], "payload"), "activation_id"),
            Text(second.front(), "activation_id"));
  // The same new code again submits nothing.
  EXPECT_FALSE(Changed(core.ObserveComponentFaults(ComponentFaults(
      {ComponentFaultEntry("robot", "fault_communication", "Lost")}))));
}

TEST(HeadlessApiCore, ComponentFaultAlarmsArePerComponentAndSurviveTheComponentsRemoval) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  // A shared transport stall shows on two components: two alarms.
  ASSERT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults(
      {ComponentFaultEntry("robot", "component_owner_stalled"),
       ComponentFaultEntry("gripper", "component_owner_stalled")}))));
  const auto robot = ComponentFaultAlarms(core, "robot");
  const auto gripper = ComponentFaultAlarms(core, "gripper");
  ASSERT_EQ(robot.size(), 1U);
  ASSERT_EQ(gripper.size(), 1U);
  EXPECT_NE(Text(robot.front(), "condition_id"), Text(gripper.front(), "condition_id"));
  // The stall ends on the robot; the gripper is removed by an Apply.
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults())));
  for (const auto *component : {"robot", "gripper"}) {
    const auto listed = ComponentFaultAlarms(core, component);
    ASSERT_EQ(listed.size(), 1U);
    EXPECT_EQ(Text(listed.front(), "lifecycle_state"), "returned_unacknowledged");
    static_cast<void>(core.AcknowledgeAlarm(
        Text(listed.front(), "condition_id"), Text(listed.front(), "activation_id"),
        Unsigned(listed.front(), "record_revision"), Caller(component),
        std::nullopt, true));
    EXPECT_TRUE(ComponentFaultAlarms(core, component).empty());
  }
}

TEST(HeadlessApiCore, ComponentFaultAlarmKeepsTheOwnerReconciledAndGatesNothing) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishHealthySources(manager_status, "open");
  static_cast<void>(core.RuntimeSnapshot());
  static_cast<void>(ObserveOperationTable(core, {}));
  ASSERT_TRUE(std::get<bool>(Member(core.AlarmSnapshot(), "reconciled").storage()));
  const auto before = core.OperatorAlarmMotionAttention();
  ASSERT_TRUE(Changed(core.ObserveComponentFaults(
      ComponentFaults({ComponentFaultEntry("robot", "fault_drive")}))));
  EXPECT_TRUE(std::get<bool>(Member(core.AlarmSnapshot(), "reconciled").storage()));
  const auto after = core.OperatorAlarmMotionAttention();
  EXPECT_TRUE(std::get<bool>(Member(after, "reconciled").storage()));
  EXPECT_EQ(content_identity::canonical_json(Member(after, "members")),
            content_identity::canonical_json(Member(before, "members")));
  EXPECT_TRUE(Changed(core.ObserveComponentFaults(ComponentFaults())));
  EXPECT_TRUE(std::get<bool>(Member(core.AlarmSnapshot(), "reconciled").storage()));
}

TEST(HeadlessApiCore, ComponentFaultStillPresentAfterARestartIsANewOccurrence) {
  TemporaryDirectory directory;
  const auto faults = ComponentFaults({ComponentFaultEntry("robot", "fault_vendor")});
  std::string first_activation;
  {
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    ASSERT_TRUE(Changed(core.ObserveComponentFaults(faults)));
    first_activation = Text(ComponentFaultAlarms(core, "robot").front(), "activation_id");
    core.Shutdown();
  }
  HeadlessApiCore core(Config(directory, "api.epoch.2"), Clocks());
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
  ASSERT_TRUE(Changed(core.ObserveComponentFaults(faults)));
  const auto listed = ComponentFaultAlarms(core, "robot");
  ASSERT_EQ(listed.size(), 1U);
  EXPECT_EQ(Text(listed.front(), "lifecycle_state"), "active_unacknowledged");
  EXPECT_NE(Text(listed.front(), "activation_id"), first_activation);
}

TEST(HeadlessApiCore, ComponentFaultListIsValidatedBeforeAnyAlarmMutation) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto good = ComponentFaultEntry("robot", "fault_drive");
  auto with_time = Member(good, "fault").as_object();
  with_time.emplace_back("since_ns", CanonicalValue(5.0));
  const std::vector<CanonicalValue> invalid{
      CanonicalValue(CanonicalValue::Object{}),
      ComponentFaults({good, ComponentFaultEntry("gripper", "fault_unknown")}),
      ComponentFaults({good, ComponentFaultEntry("robot", "fault_vendor")}),
      ComponentFaults({good, WithField(ComponentFaultEntry("gripper", "fault_vendor"),
                                       "fault", CanonicalValue(std::move(with_time)))}),
      ComponentFaults({good, ComponentFaultEntry("", "fault_vendor")}),
  };
  for (const auto &faults : invalid) {
    try {
      static_cast<void>(core.ObserveComponentFaults(faults));
      FAIL() << "an invalid component fault list was accepted";
    } catch (const OperatorAlarmError &error) {
      EXPECT_EQ(error.code(), "headless_component_fault_invalid");
    }
  }
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}

// OD-215: the disk check's alarm gates motion on the cell exactly while it is
// active, tells the Kernel's projection so (manual moves and a Reset's move
// back are then refused too), needs no acknowledgement and clears by itself.
TEST(HeadlessApiCore, DiskConditionAlarmGatesMotionWhileActiveAndClearsByItself) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto changed = [](const CanonicalValue &result) {
    return std::get<bool>(Member(result, "changed").storage());
  };
  const auto signals = core.HistoryDiskSignals();
  EXPECT_FALSE(std::get<bool>(Member(signals, "queue_full").storage()));
  EXPECT_FALSE(std::get<bool>(Member(signals, "storage_full").storage()));
  const std::string headline =
      "Disk nearly full: 4 GB free on the disk that holds /state/current. "
      "ManyForge stops below 10 GB. Free at least 7 GB on that disk.";
  const CanonicalValue details(CanonicalValue::Object{
      {"cause", CanonicalValue("below_floor")},
      {"path", CanonicalValue("/state/current")},
      {"free_bytes", CanonicalValue(4000000000.0)},
      {"floor_bytes", CanonicalValue(10000000000.0)},
      {"margin_bytes", CanonicalValue(1000000000.0)},
      {"writer", CanonicalValue(nullptr)},
      {"failure", CanonicalValue(CanonicalValue::Object{
                      {"message", CanonicalValue(headline)}})}});
  const auto condition = [](const bool active, CanonicalValue facts) {
    return CanonicalValue(CanonicalValue::Object{
        {"active", CanonicalValue(active)}, {"details", std::move(facts)}});
  };
  EXPECT_TRUE(changed(core.ObserveDiskRecordsCondition(condition(true, details))));
  // Unchanged: not resubmitted, no second activation.
  EXPECT_FALSE(changed(core.ObserveDiskRecordsCondition(condition(true, details))));
  const auto raised = core.AlarmSnapshot();
  const auto *alarm = AlarmFor(raised, "manyforge.runtime.disk_cannot_keep_records");
  ASSERT_NE(alarm, nullptr);
  EXPECT_EQ(Text(*alarm, "lifecycle_state"), "active_acknowledged");
  EXPECT_FALSE(std::get<bool>(Member(*alarm, "acknowledgement_required").storage()));
  EXPECT_TRUE(std::get<bool>(Member(*alarm, "requires_motion_attention").storage()));
  EXPECT_EQ(Text(Member(Member(*alarm, "details"), "failure"), "message"), headline);
  const auto gated = core.OperatorAlarmMotionAttention();
  EXPECT_TRUE(std::get<bool>(Member(gated, "disk_cannot_keep_records").storage()));
  EXPECT_EQ(Array(gated, "members").size(), 1U);

  // It clears by itself; nothing else is changed.
  EXPECT_TRUE(changed(core.ObserveDiskRecordsCondition(
      condition(false, CanonicalValue(CanonicalValue::Object{})))));
  const auto cleared = core.AlarmSnapshot();
  EXPECT_EQ(AlarmFor(cleared, "manyforge.runtime.disk_cannot_keep_records"), nullptr);
  const auto open = core.OperatorAlarmMotionAttention();
  EXPECT_FALSE(std::get<bool>(Member(open, "disk_cannot_keep_records").storage()));
  EXPECT_TRUE(Array(open, "members").empty());

  // A malformed condition is refused before any alarm mutation.
  EXPECT_THROW(static_cast<void>(core.ObserveDiskRecordsCondition(
                   condition(true, CanonicalValue(CanonicalValue::Object{})))),
               OperatorAlarmError);

  // The journal's "queue full" level reaches the disk check, not the
  // journal alarm's facts: a change of it alone changes no alarm.
  auto faults = RuntimeFaultSnapshot(false, true);
  faults = WithField(faults, "operation_journal_write_fault",
                     WithField(Member(faults, "operation_journal_write_fault"),
                               "queue_full", CanonicalValue(true)));
  EXPECT_TRUE(changed(core.ObserveRuntimeFaultConditions(faults)));
  const auto lost = core.AlarmSnapshot();
  const auto *journal =
      AlarmFor(lost, "manyforge.runtime.operation_journal_write_fault");
  ASSERT_NE(journal, nullptr);
  EXPECT_EQ(Member(*journal, "details").find_object_member("queue_full"), nullptr);
  EXPECT_FALSE(changed(core.ObserveRuntimeFaultConditions(RuntimeFaultSnapshot(false, true))));
}

TEST(HeadlessApiCore,
     RuntimeFaultSnapshotValidatesBothFactsBeforeAnyAlarmMutation) {
  for (const auto scenario : {0, 1, 2, 3, 4}) {
    SCOPED_TRACE(scenario);
    TemporaryDirectory directory;
    HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
    auto fields = RuntimeFaultSnapshot(true, true).as_object();
    for (auto &[kind, fact] : fields) {
      if (kind != "program_runner_loss")
        continue;
      auto members = fact.as_object();
      for (auto &[name, value] : members) {
        if (scenario == 0 && name == "established_cycle") {
          // Run numbers are integers (contract 1.5); text is refused.
          auto cycle = value.as_object();
          for (auto &[key, field] : cycle)
            if (key == "program_run_id")
              field = CanonicalValue("run.1");
          value = CanonicalValue(std::move(cycle));
        }
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
                     CanonicalValue(std::move(fields)))),
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
  // Each bounded field at the largest value ObserveRuntimeFaultConditions
  // accepts (src/headless_api_core.cpp). C0 is valid NFC text and needs six
  // JSON bytes per source byte, so text fields are filled with it.
  constexpr std::size_t kWriteKindBytes = 64U;          // :1154
  constexpr std::size_t kErrorCodeBytes = 256U;         // :1161
  constexpr std::size_t kMessageBytes = 1024U;          // :1163 and :1198
  constexpr std::size_t kNodeInstanceBytes = 128U;      // :1179
  constexpr std::size_t kReasonCodeBytes = 128U;        // :1185
  constexpr std::size_t kApplicationStartBytes = 16U;   // :1190
  constexpr std::size_t kRequestKeyBytes = 32U;         // :1172 (IsRequestKeyText, exact)
  // The predicate is one of four names (the 128-byte text bound is not
  // reachable). Only program_runner_fault carries a fault code (128 bytes),
  // so it is the largest Runner fact.
  const std::string fault_predicate = "program_runner_fault";
  constexpr std::size_t kFaultCodeBytes = 128U;
  // :1209: the longest accepted established state ("starting"/"stopping").
  const std::string longest_state = "starting";
  // Counters (:1170, :1175, :1180, :1186, :1204-1206) up to the I-JSON safe
  // integer, 16 JSON bytes.
  const CanonicalValue maximum_number(9007199254740991.0);
  constexpr std::size_t kCounterBytes = 16U;
  const auto escaped = [](std::size_t bytes) { return std::string(bytes, '\x01'); };
  const std::string message = escaped(kMessageBytes);
  ASSERT_EQ(std::string_view(kFaultRequestKey).size(), kRequestKeyBytes);
  auto fields = RuntimeFaultSnapshot(true, true, true).as_object();
  for (auto &[kind, fact] : fields) {
    auto members = fact.as_object();
    for (auto &[key, value] : members) {
      if (kind == "operation_journal_write_fault") {
        if (key == "program_run_id" || key == "operation_id" ||
            key == "operation_revision")
          value = maximum_number;
        if (key == "write_kind")
          value = CanonicalValue(escaped(kWriteKindBytes));
        if (key == "execution_scope")
          value = CanonicalValue(CanonicalValue::Object{
              {"node_instance_id", CanonicalValue(escaped(kNodeInstanceBytes))},
              {"node_attempt_id", maximum_number}});
        if (key == "error")
          value = CanonicalValue(CanonicalValue::Object{
              {"code", CanonicalValue(escaped(kErrorCodeBytes))},
              {"message", CanonicalValue(message)},
              {"details", CanonicalValue(CanonicalValue::Object{})}});
      } else if (kind == "kernel_internal_fault") {
        if (key == "reason_code")
          value = CanonicalValue(escaped(kReasonCodeBytes));
        if (key == "later_count")
          value = maximum_number;
      } else {
        if (key == "message")
          value = CanonicalValue(message);
        if (key == "application_start_id")
          value = CanonicalValue(escaped(kApplicationStartBytes));
        if (key == "failure_predicate")
          value = CanonicalValue(fault_predicate);
        if (key == "fault_code")
          value = CanonicalValue(escaped(kFaultCodeBytes));
        if (key == "established_cycle") {
          auto cycle = value.as_object();
          for (auto &[name, item] : cycle) {
            if (name == "program_run_id" || name == "cycle_generation" ||
                name == "cycle_claim_generation")
              item = maximum_number;
            if (name == "state")
              item = CanonicalValue(longest_state);
          }
          value = CanonicalValue(std::move(cycle));
        }
      }
    }
    fact = CanonicalValue(std::move(members));
  }
  const CanonicalValue maximum(std::move(fields));
  // The bounded values alone, at their maxima (quotes included): the whole
  // snapshot (closed shapes add only their fixed keys and punctuation) is at
  // least this large.
  const auto text_bytes = [](std::size_t source_bytes) { return 6U * source_bytes + 2U; };
  const std::size_t maxima_bytes =
      text_bytes(kWriteKindBytes) + text_bytes(kErrorCodeBytes) +
      2U * text_bytes(kMessageBytes) + text_bytes(kNodeInstanceBytes) +
      text_bytes(kReasonCodeBytes) + text_bytes(kApplicationStartBytes) +
      (kRequestKeyBytes + 2U) + (fault_predicate.size() + 2U) +
      text_bytes(kFaultCodeBytes) +
      (longest_state.size() + 2U) +
      // journal: operation_id, operation_revision, program_run_id,
      // node_attempt_id; internal: later_count; cycle: three counters.
      8U * kCounterBytes;
  const auto maximum_size = content_identity::canonical_json(maximum).size();
  EXPECT_GE(maximum_size, maxima_bytes);
  EXPECT_LE(maximum_size, kMaximumRuntimeFaultSnapshotBytes); // :1141-1143
  ASSERT_TRUE(std::get<bool>(
      Member(core.ObserveRuntimeFaultConditions(maximum),
             "changed")
          .storage()));
  const auto snapshot = core.AlarmSnapshot();
  const auto *fault =
      AlarmFor(snapshot, "manyforge.runtime.operation_journal_write_fault");
  ASSERT_NE(fault, nullptr);
  EXPECT_EQ(Text(*fault, "lifecycle_state"), "active_acknowledged");
  EXPECT_EQ(Member(*fault, "correlations").find_object_member("source_owner_epoch"), nullptr);
  EXPECT_EQ(Unsigned(Member(*fault, "correlations"), "operation_id"),
            9007199254740991U);
  EXPECT_EQ(Text(Member(*fault, "details"), "request_key"), kFaultRequestKey);
  EXPECT_EQ(Unsigned(Member(*fault, "details"), "program_run_id"),
            9007199254740991U);
  EXPECT_EQ(Unsigned(Member(Member(*fault, "correlations"), "execution_scope"),
                     "node_attempt_id"),
            9007199254740991U);
  const auto *runner =
      AlarmFor(snapshot, "manyforge.runtime.program_runner_loss");
  ASSERT_NE(runner, nullptr);
  EXPECT_EQ(Text(*runner, "lifecycle_state"), "active_unacknowledged");
  EXPECT_EQ(Member(*runner, "correlations").find_object_member("source_owner_epoch"), nullptr);
  EXPECT_EQ(Text(Member(*runner, "details"), "message"), message);
  EXPECT_EQ(Text(Member(*runner, "details"), "application_start_id"),
            escaped(kApplicationStartBytes));
  EXPECT_EQ(Text(Member(*runner, "details"), "failure_predicate"),
            fault_predicate);
  EXPECT_EQ(Text(Member(*runner, "details"), "fault_code"),
            escaped(kFaultCodeBytes));
  const auto *internal =
      AlarmFor(snapshot, "manyforge.runtime.kernel_internal_fault");
  ASSERT_NE(internal, nullptr);
  EXPECT_EQ(Text(Member(*internal, "details"), "reason_code"),
            escaped(kReasonCodeBytes));
  EXPECT_EQ(Unsigned(Member(*internal, "details"), "later_count"),
            9007199254740991U);
}

CanonicalValue ProgramFailureCondition(const std::string &resource) {
  return CanonicalValue(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.program_failure_condition.v2")},
      {"execution_control_revision", CanonicalValue(2.0)},
      {"program_run_id", CanonicalValue(1.0)},
      {"run_revision", CanonicalValue(4.0)},
      {"state", CanonicalValue("active")},
      {"occurrence",
       CanonicalValue(CanonicalValue::Object{
           {"node_instance_id", CanonicalValue("move")},
           {"node_attempt_id", CanonicalValue(1.0)},
           {"operator_facing_run_revision", CanonicalValue(4.0)}})},
      {"operation", CanonicalValue(CanonicalValue::Object{
                        {"operation_id", CanonicalValue(1.0)},
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
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
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
     ProgramFailureDifferentOccurrenceNeverClearsCurrentFailure) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  auto clear = ProgramFailureState(active, "clear", 3, 5);
  auto other =
      JournalField(clear, "occurrence",
                   JournalField(Member(clear, "occurrence"), "node_attempt_id",
                                CanonicalValue(2.0)));
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
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto first = ProgramFailureCondition(resource);
  auto second =
      JournalField(first, "operation",
                   JournalField(Member(first, "operation"), "operation_id",
                                CanonicalValue(2.0)));
  second =
      JournalField(second, "occurrence",
                   JournalField(Member(second, "occurrence"), "node_attempt_id",
                                CanonicalValue(2.0)));
  for (const auto &value : {first, second, first, second})
    static_cast<void>(core.ObserveProgramFailureCondition(value));
  const auto snapshot = core.AlarmSnapshot();
  ASSERT_EQ(Array(snapshot, "alarms").size(), 2U);
  for (const auto &alarm : Array(snapshot, "alarms"))
    EXPECT_EQ(Unsigned(alarm, "occurrence_count"), 1U);
}

TEST(HeadlessApiCore,
     ProgramFailureMalformedSourceLeavesPriorCauseActive) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  const auto accepted_alarm = OnlyAlarm(core.AlarmSnapshot());
  auto invalid = JournalField(active, "operation", CanonicalValue(nullptr));
  EXPECT_THROW(static_cast<void>(core.ObserveProgramFailureCondition(invalid)),
               OperatorAlarmError);
  const auto unknown_snapshot = core.AlarmSnapshot();
  ExpectProgramFailureSourceCurrent(unknown_snapshot, false);
  EXPECT_EQ(OnlyAlarm(unknown_snapshot), accepted_alarm);
}

TEST(HeadlessApiCore,
     ProgramFailureMaximumEscapedProvenanceFitsTheRealAlarmOwner) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  auto value = ProgramFailureCondition(resource);
  value = JournalField(value, "program_run_id",
                       CanonicalValue(9007199254740991.0));
  value = JournalField(
      value, "operation",
      CanonicalValue(CanonicalValue::Object{
          {"operation_id", CanonicalValue(9007199254740991.0)},
          {"operation_revision", CanonicalValue(1.0)}}));
  value = JournalField(
      value, "occurrence",
      CanonicalValue(CanonicalValue::Object{
          {"node_instance_id", CanonicalValue(std::string(128, '\1'))},
          {"node_attempt_id", CanonicalValue(9007199254740991.0)},
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
  EXPECT_EQ(Unsigned(Member(alarm, "correlations"), "operation_id"),
            9007199254740991U);
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
  ManagerStatusFeed manager_status(core);
  const auto active = ProgramFailureCondition(resource);
  static_cast<void>(core.ObserveProgramFailureCondition(active));
  EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
            kAlarmScopeDefinition);
  PublishActiveConfiguration(manager_status, *configurations.single);
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
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
  const auto failure = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"message", CanonicalValue("settlement is unknown")},
      {"details", CanonicalValue(CanonicalValue::Object{})}});
  auto value = CapturedOperation(resource, failure);
  const auto obsolete =
      JournalField(value, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v4"));
  EXPECT_THROW(IngestJournalEvent(core, obsolete, 1U),
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
                    CanonicalValue("manyforge.operation_journal_record.v7")), 1U), OperatorAlarmError);
  value = JournalField(value, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v10"));
  auto mismatch = participants;
  mismatch.front() =
      JournalField(mismatch.front(), "terminal_error", CanonicalValue(nullptr));
  const auto retained_mismatch = JournalField(
      value, "operation_state",
      JournalField(state, "participants", CanonicalValue(mismatch)));
  EXPECT_NO_THROW(IngestJournalEvent(core, retained_mismatch, 1U));
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
                           2U),
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
                   2U),
               OperatorAlarmError);
  EXPECT_NO_THROW(IngestJournalEvent(core, value, 2U));
  static_cast<void>(ObserveOperationRow(core, value));
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
CanonicalValue OperationFailureFacts(const std::string &resource,
                                     const std::string &effect,
                                     bool captured = true) {
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("planning_failed")},
      {"message", CanonicalValue("temporary")},
      {"details", CanonicalValue(CanonicalValue::Object{})}});
  auto payload = CapturedOperation(resource, error);
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
  payload = JournalField(payload, "operation_state", state);
  return JournalField(payload, "api_version",
                      CanonicalValue("manyforge.operation_journal_record.v10"));
}

TEST(HeadlessApiCore,
     ProvenOrdinaryProgramFailureIsErrorHistoryBeforeItsRunCondition) {
  for (const std::string effect : {"no_effect", "effect_settled"}) {
    SCOPED_TRACE(effect);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishActiveConfiguration(manager_status, *configurations.single);
    const auto payload = OperationFailureFacts(resource, effect);
    IngestJournalEvent(core, payload, 1U);
    static_cast<void>(core.CompleteOperationEventSnapshot(1U));
    static_cast<void>(ObserveOperationRow(core, payload));
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
              "[move / 1] planning_failed: temporary");
    auto condition = ProgramFailureCondition(resource);
    condition = JournalField(condition, "program_run_id", CanonicalValue(1.0));
    condition = JournalField(condition, "operation",
                             JournalField(Member(condition, "operation"),
                                          "operation_id", CanonicalValue(1.0)));
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
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishActiveConfiguration(manager_status, *configurations.single);
    auto payload = OperationFailureFacts(
        resource,
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
                       CanonicalValue("manyforge.operation_journal_record.v10"));
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
                       ParticipantIdValue(1U, 2U));
      participants.push_back(std::move(sibling));
    }
    state = JournalField(state, "participants",
                         CanonicalValue(std::move(participants)));
    if (context == "stop") {
      state = JournalField(state, "stop_event_id", CanonicalValue(1.0));
    }
    payload = JournalField(payload, "operation_state", state);
    IngestJournalEvent(core, payload, 1U);
    static_cast<void>(ObserveOperationRow(core, payload));
    EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
              "manyforge.motion.execution_fault");
  }
}

// "Any Stop on this operation?" is the state's `stop_event_id`: null keeps a
// proven failure an ordinary Program failure; a Stop makes it a fault.
TEST(HeadlessApiCore, OperationWithoutStopEventIsOrdinaryProgramFailure) {
  for (const bool stopped : {false, true}) {
    SCOPED_TRACE(stopped);
    TemporaryDirectory directory;
    const auto configurations = LoadRegistryConfigurations();
    const auto resource = ResourceIds(*configurations.single).front();
    HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
    ManagerStatusFeed manager_status(core);
    PublishActiveConfiguration(manager_status, *configurations.single);
    auto payload = OperationFailureFacts(resource, "no_effect");
    if (stopped) {
      payload = JournalField(payload, "operation_state",
                             JournalField(Member(payload, "operation_state"),
                                          "stop_event_id", CanonicalValue(1.0)));
    }
    IngestJournalEvent(core, payload, 1U);
    static_cast<void>(core.CompleteOperationEventSnapshot(1U));
    static_cast<void>(ObserveOperationRow(core, payload));
    const auto snapshot = core.AlarmSnapshot();
    if (stopped) {
      EXPECT_EQ(Text(OnlyAlarm(snapshot), "definition_id"),
                "manyforge.motion.execution_fault");
    } else {
      EXPECT_TRUE(Array(snapshot, "alarms").empty());
    }
  }
}

TEST(HeadlessApiCore,
     AcceptedUnknownWatermarksBothRevisionsWithoutClearingTheCause) {
  TemporaryDirectory directory;
  const auto configurations = LoadRegistryConfigurations();
  const auto resource = ResourceIds(*configurations.single).front();
  HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *configurations.single);
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
  auto next = JournalField(active, "program_run_id", CanonicalValue(2.0));
  next = JournalField(next, "operation",
                      JournalField(Member(next, "operation"), "operation_id",
                                   CanonicalValue(2.0)));
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
                 {"operation_id", CanonicalValue(99.0)},
                 {"node_instance_id", CanonicalValue("winner")},
                 {"node_attempt_id", CanonicalValue(1.0)}})}});
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
      HeadlessApiCore core(RegistryConfig(directory, "api.epoch.1"), Clocks());
      ManagerStatusFeed manager_status(core);
      PublishActiveConfiguration(manager_status, *configurations.single);
      const auto physical_failure =
          OperationFailureFacts(resource, "effect_uncertain");
      auto payload = with_fact
                         ? physical_failure
                         : CapturedOperation(resource,
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
                                        1U),
                     OperatorAlarmError);
      }
      payload =
          JournalField(payload, "api_version",
                       CanonicalValue("manyforge.operation_journal_record.v10"));
      EXPECT_THROW(
          IngestJournalEvent(
              core, JournalField(payload, "operation_state", JournalField(
                  state, "preemption", LogicalPreemption("unknown"))), 1U),
          OperatorAlarmError);
      EXPECT_NO_THROW(IngestJournalEvent(core, payload, 1U));
      static_cast<void>(ObserveOperationRow(core, payload));
      EXPECT_EQ(Text(OnlyAlarm(core.AlarmSnapshot()), "definition_id"),
                "manyforge.motion.execution_fault");
    }
  }
}

TEST(HeadlessApiCore, LogicalPreemptionRequiresExactDistinctCapturedActor) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto payload = CapturedOperation("robot.1", CollisionFailure());
  payload =
      JournalField(payload, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v10"));
  const auto state = Member(payload, "operation_state");
  const auto ingest = [&](const CanonicalValue &preemption,
                          const CanonicalValue &captured_state) {
    IngestJournalEvent(
        core,
        JournalField(payload, "operation_state",
                     JournalField(captured_state, "preemption", preemption)),
        1U);
  };
  EXPECT_THROW(ingest(CanonicalValue(nullptr), state), OperatorAlarmError);
  EXPECT_THROW(ingest(LogicalPreemption("completed"), state),
               OperatorAlarmError);
  EXPECT_THROW(
      ingest(JournalField(LogicalPreemption(), "extra", CanonicalValue(true)),
             state),
      OperatorAlarmError);
  for (const std::string &invalid :
       {std::string{}, std::string(129U, 'x'), std::string("x\0y", 3)}) {
    const auto by = JournalField(Member(LogicalPreemption(), "by"),
                                 "node_instance_id", CanonicalValue(invalid));
    EXPECT_THROW(ingest(JournalField(LogicalPreemption(), "by", by), state),
                 OperatorAlarmError);
  }
  // Operation and node attempt numbers are positive safe integers (1.5).
  for (const std::string field : {"operation_id", "node_attempt_id"}) {
    for (const auto &invalid :
         {CanonicalValue(0.0), CanonicalValue("7"),
          CanonicalValue(9007199254740992.0)}) {
      const auto by =
          JournalField(Member(LogicalPreemption(), "by"), field, invalid);
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
  by = JournalField(by, "operation_id", CanonicalValue(9007199254740991.0));
  by = JournalField(by, "node_instance_id",
                    CanonicalValue(std::string(128, '\1')));
  by = JournalField(by, "node_attempt_id", CanonicalValue(9007199254740991.0));
  EXPECT_NO_THROW(ingest(JournalField(LogicalPreemption(), "by", by), state));
}

TEST(HeadlessApiCore,
     PhysicalReplacementFactRequiresItsSettledPreemptionCapture) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  auto payload = CapturedOperation("robot.1", CanonicalValue(nullptr));
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
          1U),
      OperatorAlarmError);
  payload =
      JournalField(payload, "api_version",
                   CanonicalValue("manyforge.operation_journal_record.v10"));
  EXPECT_THROW(IngestJournalEvent(
                   core,
                   JournalField(payload, "operation_state",
                                JournalField(state, "preemption",
                                             LogicalPreemption("superseded_unsent"))),
                   1U),
               OperatorAlarmError);
  auto absent = state.as_object();
  std::erase_if(absent,
                [](const auto &field) { return field.first == "preemption"; });
  EXPECT_THROW(IngestJournalEvent(core,
                                  JournalField(payload, "operation_state",
                                               CanonicalValue(absent)),
                                  1U),
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
                           1U),
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
                         1U),
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
                         1U),
      OperatorAlarmError);
  EXPECT_NO_THROW(IngestJournalEvent(core, payload, 1U));
  EXPECT_TRUE(Array(core.AlarmSnapshot(), "alarms").empty());
}


// Exact durable native40 state_observed row17, including its real I/O-rule
// evidence. This is recorded producer data, not a fabricated successful trip.
CanonicalValue Native40IoRuleRecord() {
  return content_identity::parse_canonical_json(R"native40({"api_version":"manyforge.operation_journal_record.v7","effect_pending":false,"intent":{"transition_reason":"runtime_scale_status_observed"},"operation_state":{"active_stop_generation":null,"active_stop_id":null,"coordination_policy":"independent","journal_capture_order":"8","operation_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participants":[{"capability_id":"execution.trajectory.execute","capability_provider_binding_id":null,"capability_provider_binding_revision":null,"command_target_id":"arm","control_binding_id":"lite6_robot.arm.execution-trajectory-execute","control_binding_revision":"sha256:8e3f32bc78e641e62fc0b9902b615e6a3ebe7626d83eb39e81efd9ca3c2425e6","control_domain_id":"lite6_robot.arm.control","control_domain_revision":"sha256:669ef8a3882b5167bc6e94e6c253f72e88a511990821348f46b3ca2c6cb2937c","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","execution_scope":{"node_attempt_id":"attempt:aa48a448ad3b425681b41b013bcae166","node_instance_id":"search_trip"},"io_rule_outcomes":[{"error":null,"evidence":{"evaluation_error":null,"source_losses":[],"state":"retired","trip":null},"registration":{"catalog_generation":"2","owner_epoch":"kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41","policy_id":"node.9d4549f11850b1b2fc25f067c5c1a46e727105f792656507e8739f08ac7b859a","registration_id":"1","scope":{"command_target_id":"arm","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","kind":"operation","motion_interrupt_scope_id":"node.9d4549f11850b1b2fc25f067c5c1a46e727105f792656507e8739f08ac7b859a","operation_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participant_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.participant.1","preparation_id":"2","prepared_contract_digest":"sha256:3b41addb986ce1e6507c476d16d0f5598d6ce8c86f4f482b35e35fedcaa82d04"}},"stop_certificate_id":null,"stop_certificate_revision":null,"stop_generation":null,"stop_id":null,"stop_result":null,"terminal_cause":"no_signal_trip"}],"participant_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.participant.1","payload_kind":"motion","progress":1,"resources":["lite6_robot.arm.coordinate.000000","lite6_robot.arm.coordinate.000001","lite6_robot.arm.coordinate.000002","lite6_robot.arm.coordinate.000003","lite6_robot.arm.coordinate.000004","lite6_robot.arm.coordinate.000005"],"state":"completed","terminal_error":null,"terminal_fact":{"effect":"effect_settled","error":null,"kind":"computed_completion"}}],"program_binding_id":"sha256:5ef2d8159a7dc41022f543ece89a058120b49cd679656ec6181ddeaac093858e","program_run_id":"program-cycle.1","runtime_scale_generation":0,"state":"completed","updated_at_ns":595434142274283},"owner_epoch":"kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41","record_kind":"state_observed","result":null,"runtime_scale_status":{"effective_scale":1,"operation_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2","operation_revision":1,"participants":[{"accepted_generation":0,"acknowledgement_sequence":1888,"acknowledgement_source_epoch":"execution-host-ab1ab108c01811d071cbf157d05eef41","acknowledgement_timestamp_ns":595434139380760,"active_control_configuration_generation":1,"active_control_configuration_id":"active.i6.native.arm","applied_scale":1,"capability_id":"execution.trajectory.execute","command_target_id":"arm","commanded_scale":1,"control_binding_id":"lite6_robot.arm.execution-trajectory-execute","control_binding_revision":"sha256:8e3f32bc78e641e62fc0b9902b615e6a3ebe7626d83eb39e81efd9ca3c2425e6","control_domain_id":"lite6_robot.arm.control","control_domain_revision":"sha256:669ef8a3882b5167bc6e94e6c253f72e88a511990821348f46b3ca2c6cb2937c","control_route_id":"arm_position","controllable_component_id":"lite6_robot","controller_goal_generation":1,"controller_goal_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.controller-goal.1","controller_topology_id":"manyforge.ros2_control_arm_direct","coordinated_barrier_id":null,"error":null,"participant_id":"operation.kernel-owner-execution-host-ab1ab108c01811d071cbf157d05eef41.2.participant.1","resources":["lite6_robot.arm.coordinate.000000","lite6_robot.arm.coordinate.000001","lite6_robot.arm.coordinate.000002","lite6_robot.arm.coordinate.000003","lite6_robot.arm.coordinate.000004","lite6_robot.arm.coordinate.000005"],"scaling_domain_id":"lite6_robot.arm.runtime_scale","state":"pending","supported":true,"topology_revision":"sha256:299f321da75dc2279ca2cebf66887ae13fb365a8a9db07d5c93d8aae6303352a","virtual_trajectory_time_s":2.533308666}],"requested_scale":1,"runtime_scale_generation":0,"state":"pending","updated_at_ns":595434145763632}})native40");
}

// Re-pin the captured factual participant through the CURRENT typed journal
// producer. The old raw record is provenance/input only, never a v8 wire alias.
// Its text ids become their contract 1.5 integer objects (operation 2,
// participant ordinal 1, goal generation 1) and its owner epoch is dropped:
// the evidence itself is unchanged.
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
  CanonicalValue::Array current_outcomes;
  for (const auto& recorded : Array(row, "io_rule_outcomes")) {
    auto registration = Member(recorded, "registration").as_object();
    std::erase_if(registration, [](const auto& field) { return field.first == "owner_epoch"; });
    auto scope = Member(CanonicalValue(registration), "scope").as_object();
    std::erase_if(scope, [](const auto& field) {
      return field.first == "controller_goal_generation";
    });
    CanonicalValue current_scope(std::move(scope));
    current_scope = JournalField(current_scope, "operation_id", CanonicalValue(2.0));
    current_scope = JournalField(current_scope, "participant_id", ParticipantIdValue(2U));
    current_scope = JournalField(current_scope, "controller_goal_id", CommandIdValue(2U));
    // A Stop is recorded as one Stop event: the recorded Stop and
    // certificate identities (all absent here) become an absent event.
    auto outcome = recorded.as_object();
    const auto erased = std::erase_if(outcome, [](const auto& field) {
      return (field.first == "stop_id" || field.first == "stop_generation" ||
              field.first == "stop_certificate_id" ||
              field.first == "stop_certificate_revision") &&
             field.second.is_null();
    });
    if (erased != 4U || !Member(recorded, "stop_result").is_null())
      throw std::logic_error("captured native40 factual fixture changed shape");
    outcome.emplace_back("stop_event_id", CanonicalValue(nullptr));
    current_outcomes.push_back(JournalField(
        CanonicalValue(std::move(outcome)), "registration",
        JournalField(CanonicalValue(std::move(registration)), "scope", current_scope)));
  }
  const auto outcomes = core::DecodeIoRuleOutcomes(
      content_identity::canonical_json(CanonicalValue(std::move(current_outcomes))));
  if (!outcomes.ok()) throw std::logic_error(outcomes.error().message);
  std::vector<core::ResourceId> resources;
  for (const auto& resource : Array(row, "resources")) resources.emplace_back(resource.as_string());
  core::OperationParticipantStateDTO participant{
      core::OperationParticipantId{core::OperationNumber(2U), 1U},
      core::ControllerGoalId{core::OperationNumber(2U), 1U, 1U},
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
      // The recorded attempt is the run's first (contract 1.5: per-run counter).
      core::NodeAttemptNumber(1U)};
  participant.terminal_fact = core::ParticipantExecutionFact{
      core::ParticipantExecutionFactKind::kComputedCompletion,
      core::ParticipantEffectKnowledge::kEffectSettled, std::nullopt};
  participant.io_rule_outcomes = outcomes.value();
  core::OperationStateDTO state{
      // The recorded operation and run, as numbers (contract 1.5):
      // ``operation.<owner>.2`` and ``program-cycle.1``.
      core::OperationNumber(2U),
      core::OperationRevision(Unsigned(original, "operation_revision")),
      core::ProgramBindingId(Text(original, "program_binding_id")),
      core::ProgramRunNumber(1U),
      core::OperationCoordinationPolicy::kIndependent, core::OperationStateKind::kCompleted,
      {participant}, content_identity::parse_nonnegative_int64(Member(original, "updated_at_ns")),
      std::nullopt,
      core::RuntimeScaleGeneration(Unsigned(original, "runtime_scale_generation")),
      static_cast<std::uint64_t>(content_identity::parse_nonnegative_int64(
          Member(original, "journal_capture_order")))};
  core::SqliteOperationJournal journal(directory.path() / "native-command-v8.sqlite3",
      core::CellId(std::string(kCellId)), 8U,
      [](std::int64_t) { return "2026-10-18T08:17:51.987135216Z"; });
  const auto opened = journal.BeginStart(core::ApplicationStartId{0x3f9a0000000000a1ULL}, 0, 8U);
  if (!opened.ok()) throw std::runtime_error(opened.error().message);
  const auto reason = Text(Member(input, "intent"), "transition_reason");
  const std::array records{core::JournalRecord{
      core::JournalOperationState{state, std::nullopt, reason.c_str(), {}}, "test"}};
  const auto recorded = journal.WriteBatch(records, 0U);
  if (!recorded.failures.empty())
    throw std::runtime_error(recorded.failures.front().error.message);
  const auto page = journal.ReadOperationalEvents(0U);
  if (!page.ok()) throw std::runtime_error(page.error().message);
  if (page.value().events.size() != 1U) throw std::logic_error("current typed fixture has no unique event");
  return page.value().events.front();
}

TEST(HeadlessApiCore, NativeV8IoRuleRecordKeepsTypedProducerEnvelopeAndParticipantEvidence) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const auto event = NativeCurrentIoRuleEvent(directory);
  const auto payload = content_identity::parse_canonical_json(event.payload_json);
  ASSERT_EQ(Text(payload, "api_version"), "manyforge.operation_journal_record.v10");
  const auto operation = Unsigned(Member(payload, "operation_state"), "operation_id");
  const auto ingest = [&](const CanonicalValue& value, const std::string& digest) {
    return core.IngestOperationEvent(
        event.sequence, event.record_kind, std::nullopt, operation, 1U,
        event.occurred_at_ns, event.occurred_at_wall, value, digest);
  };
  const auto digest = content_identity::sha256_digest(content_identity::canonical_json(payload));
  ASSERT_EQ(digest, event.payload_digest);
  const auto& common = Array(Member(payload, "operation_state"), "participants").front();
  EXPECT_EQ(common.find_object_member("controller_goal_id"), nullptr);
  EXPECT_EQ(common.find_object_member("controller_goal_generation"), nullptr);
  EXPECT_EQ(Member(common, "participant_id"), ParticipantIdValue(2U));
  EXPECT_EQ(Member(common, "command_id"), CommandIdValue(2U));
  EXPECT_EQ(common.find_object_member("command_generation"), nullptr);
  EXPECT_THROW(static_cast<void>(ingest(payload, "sha256:" + std::string(64U, '0'))), OperatorAlarmError);
  for (const std::string old_version : {"v1", "v2", "v3", "v4", "v5", "v6", "v7", "v8", "v9"}) {
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
      altered = JournalField(altered, "participant_id", ParticipantIdValue(2U, 2U));
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
  EXPECT_EQ(Text(plain.IngestOperationEvent(event.sequence, event.record_kind,
      std::nullopt, operation, 1U, event.occurred_at_ns, event.occurred_at_wall, no_extension, content_identity::sha256_digest(
          content_identity::canonical_json(no_extension))), "status"), "ingested");
  EXPECT_EQ(Text(ingest(payload, digest), "status"), "ingested");
  EXPECT_EQ(Text(ingest(payload, digest), "status"), "already_ingested");
  EXPECT_EQ(Unsigned(core.CompleteOperationEventSnapshot(event.sequence), "native_sequence"), event.sequence);

  // An ACK/history successor retains immutable origin revision1.
  // Only chronology relaxes; no prepared/goal/participant identity is relabelled.
  TemporaryDirectory history_directory;
  HeadlessApiCore history_core(Config(history_directory, "api.epoch.1"), Clocks());
  const auto historical = JournalField(payload, "operation_state", JournalField(
      state, "operation_revision", CanonicalValue(2.0)));
  const auto ingest_history = [&](const CanonicalValue& value) {
    return history_core.IngestOperationEvent(
        18U, "state_observed", std::nullopt, operation, 2U,
        595434142274284LL, "2026-10-18T08:17:51.987135217Z", value,
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
          JournalField(participant, "command_id", CommandIdValue(2U, 1U, 2U))})));
  EXPECT_THROW(static_cast<void>(ingest_history(historical_wrong_goal)), OperatorAlarmError);
  EXPECT_EQ(Text(ingest_history(historical), "status"), "ingested");
  EXPECT_EQ(Text(ingest_history(historical), "status"), "already_ingested");
  EXPECT_EQ(Unsigned(history_core.CompleteOperationEventSnapshot(18U), "native_sequence"), 18U);
}

} // namespace manyforge::headless


namespace manyforge::headless {
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
  HeadlessApiCore core(config, Clocks());
  ManagerStatusFeed manager_status(core);
  PublishActiveConfiguration(manager_status, *output);
  const CanonicalValue failure(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")}});
  const auto ingest = [&](const std::uint64_t operation,
                          std::uint64_t sequence) {
    const auto payload =
        OperationJournalPayload("state_observed", "failed", failure,
                                std::nullopt, resource, operation);
    EXPECT_EQ(
        Text(core.IngestOperationEvent(
                 sequence, "state_observed", std::nullopt, operation, 1U,
                 static_cast<std::int64_t>(100U + sequence),
                 "2026-10-18T10:00:00Z", payload,
                 content_identity::sha256_digest(
                     content_identity::canonical_json(payload))),
             "status"),
        "ingested");
    static_cast<void>(core.CompleteOperationEventSnapshot(sequence));
    static_cast<void>(ObserveOperationRow(core, payload));
  };
  ingest(1U, 1U);
  EXPECT_TRUE(RefusedSubjects(core.AlarmSnapshot()).empty());
  const auto alarms = core.AlarmSnapshot();
  const auto &alarm = OnlyAlarm(alarms);
  EXPECT_EQ(Text(Member(alarm, "subject"), "subject_id"), "1");
  EXPECT_EQ(Text(alarm, "lifecycle_state"), "active_unacknowledged");
  // A new input-only publication owns no logical output. Existing history
  // survives, while a new activation naming that removed output is refused.
  PublishActiveConfiguration(manager_status, *input);
  ingest(2U, 2U);
  EXPECT_EQ(RefusedSubjects(core.AlarmSnapshot()), (std::set<std::string>{"2"}));
}


} // namespace manyforge::headless

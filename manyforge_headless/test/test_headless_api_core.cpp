#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

#include "manyforge_headless/headless_api_core.hpp"
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
  void CreateRetentionSkeleton() const {
    std::filesystem::create_directories(path_ / "retained-bundles/sha256");
    std::filesystem::create_directories(path_ / "raw-artifacts/sha256");
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
      {"resource.arm.a", "resource.arm.b"},
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
  const auto *cell_graph =
      bundle.normalized_root().find_object_member("cell_graph");
  if (cell_graph == nullptr || !cell_graph->is_object()) {
    throw std::runtime_error("test retained bundle has no cell graph");
  }
  return CanonicalValue(CanonicalValue::Object{
      {"active",
       CanonicalValue(CanonicalValue::Object{
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
    const std::optional<std::string> &control = std::nullopt) {
  const CanonicalValue result = control.has_value()
                                    ? CanonicalValue(CanonicalValue::Object{
                                          {"control", CanonicalValue(*control)},
                                      })
                                    : CanonicalValue(nullptr);
  return CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.operation_journal_record.v1")},
      {"effect_pending", CanonicalValue(record_kind != "state_observed" &&
                                        record_kind != "operation_prepared")},
      {"intent", CanonicalValue(CanonicalValue::Object{})},
      {"operation_state",
       CanonicalValue(CanonicalValue::Object{
           {"operation_id", CanonicalValue("operation.test.1")},
           {"operation_revision", CanonicalValue(1.0)},
           {"participants",
            CanonicalValue(
                CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                    {"resources", CanonicalValue(CanonicalValue::Array{
                                      CanonicalValue("resource.arm.a")})},
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
       CanonicalValue("manyforge.stop_acceptance_projection.v2")},
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
    const std::string_view scoped_component = "robot-a") {
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
    const std::string &owner_epoch) {
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
                             {"participant_id",
                              CanonicalValue("participant.collision.1")},
                             {"resources",
                              CanonicalValue(CanonicalValue::Array{
                                  CanonicalValue("resource.arm.a")})},
                             {"terminal_error", CanonicalValue(nullptr)},
                         })})},
                    {"program_binding_id",
                     CanonicalValue("program-binding.test")},
                    {"program_run_id", CanonicalValue("program-run.test")},
                    {"state", CanonicalValue("stopping")},
                })})},
           {"resources", CanonicalValue(CanonicalValue::Array{
                             CanonicalValue("resource.arm.a")})},
       })},
      {"api_version",
       CanonicalValue("manyforge.stop_acceptance_projection.v2")},
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
    const std::string_view scoped_component = "robot-a") {
  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + std::string(kCellId),
      "manager.epoch.1", 2,
      ManagerPayloadWithIncompleteApply(phase, scoped_component)));
}

const CanonicalValue &OnlyAlarm(const CanonicalValue &snapshot) {
  const auto &alarms = Array(snapshot, "alarms");
  if (alarms.size() != 1U) {
    throw std::runtime_error("expected one alarm");
  }
  return alarms.front();
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
  ASSERT_EQ(domains.size(), 2U);
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

TEST(HeadlessApiCore,
     GripperConfirmationTraceIsBoundedAttributedAndNeverLiveProof) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  CanonicalValue record(CanonicalValue::Object{
      {"schema_version",
       CanonicalValue("manyforge.gripper_confirmation_trace.v1")},
      {"origin_epoch", CanonicalValue("control.session.1")},
      {"request_id", CanonicalValue("press.1")},
      {"source_label", CanonicalValue("station.operator")},
      {"native_owner_epoch", CanonicalValue("native.1")},
      {"program_binding_id", CanonicalValue(Digest('2'))},
      {"target_role_id", CanonicalValue("gripper")},
      {"command_id", CanonicalValue(7.0)},
      {"generation", CanonicalValue(2.0)},
      {"expected_status_revision", CanonicalValue(9.0)},
      {"confirmed", CanonicalValue(true)},
      {"phase", CanonicalValue("attempted")},
      {"details", CanonicalValue(CanonicalValue::Object{
                      {"assurance", CanonicalValue("unverified")},
                      {"principal_id", CanonicalValue(nullptr)},
                  })},
  });
  const auto result = core.RecordGripperConfirmation(record);
  EXPECT_TRUE(std::get<bool>(Member(result, "recorded").storage()));
  HistorianQuery query;
  query.components.insert("manyforge.gripper");
  const auto deadline = std::chrono::steady_clock::now() + 2s;
  CanonicalValue page;
  do {
    page = core.HistoryPage(query);
    if (Array(page, "events").size() == 1U)
      break;
    std::this_thread::yield();
  } while (std::chrono::steady_clock::now() < deadline);
  ASSERT_EQ(Array(page, "events").size(), 1U);
  const auto &event = Member(Array(page, "events").front(), "event");
  EXPECT_EQ(Text(event, "event_type"), "gripper_confirmation_attempted");
  const auto &payload = Member(event, "payload");
  EXPECT_EQ(Text(payload, "origin_epoch"), "control.session.1");
  EXPECT_EQ(Text(payload, "clock_owner"), "headless:api.epoch.1");
  EXPECT_EQ(Text(payload, "evidence_kind"), "operator_attestation_trace");
  EXPECT_EQ(payload.find_object_member("evidence"), nullptr);
  for (const auto &[field, invalid] : CanonicalValue::Object{
           {"phase", CanonicalValue("sensor_proof")},
           {"command_id", CanonicalValue(0.0)},
           {"details", CanonicalValue(std::string(9000, 'x'))},
           {"confirmed", CanonicalValue(1.0)},
       }) {
    auto changed = record.as_object();
    for (auto &[key, value] : changed)
      if (key == field)
        value = invalid;
    EXPECT_THROW(static_cast<void>(
                     core.RecordGripperConfirmation(CanonicalValue(changed))),
                 OperatorAlarmError);
  }
}

TEST(HeadlessApiCore,
     ProgramHeldDiagnosticIsInformationalAndRetainsExactValues) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  CanonicalValue record(CanonicalValue::Object{
      {"schema_version", CanonicalValue("manyforge.program_diagnostic.v1")},
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
      {"runtime_overrides",
       CanonicalValue(CanonicalValue::Object{
           {"[\"move\",\"velocity_scale\"]", CanonicalValue(0.2)}})},
      {"omitted_blackboard_count", CanonicalValue(2.0)},
      {"omitted_override_count", CanonicalValue(0.0)},
      {"delivery_loss_count", CanonicalValue(3.0)},
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
}

TEST(HeadlessApiCore,
     DurableOperationFactsDriveHistoryAndAlarmWithoutOwningExecutionState) {
  TemporaryDirectory directory;
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.1";
  const auto failure = OperationJournalPayload(
      source_epoch, "state_observed", "failed",
      CanonicalValue(CanonicalValue::Object{
          {"code", CanonicalValue("execution_aborted")},
          {"details", CanonicalValue(CanonicalValue::Object{
                          {"reason", CanonicalValue("planner_failed")},
                      })},
          {"message", CanonicalValue("planning failed")},
      }));
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
        value = CanonicalValue("manyforge.operation_journal_record.v2");
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
      value = CanonicalValue("manyforge.operation_journal_record.v3");
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
        stop_ingest("manyforge.stop_acceptance_projection.v3", bad_order),
        std::exception);
  }
  EXPECT_EQ(Text(stop_ingest("manyforge.stop_acceptance_projection.v3",
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
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.collision";
  const auto projection =
      RuntimeCollisionStopAcceptanceProjectionPayload(source_epoch);
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
      "resource.arm.a");
  EXPECT_EQ(Text(Member(active, "details"), "reason"),
            "runtime_collision_fault");
  EXPECT_EQ(Text(Member(active, "details"), "source_label"),
            "kernel.runtime_collision");
  EXPECT_EQ(Text(Member(active, "correlations"), "participant_id"),
            "participant.collision.1");
  EXPECT_EQ(Text(Member(active, "correlations"), "program_run_id"),
            "program-run.test");

  const auto reset = OperationJournalPayload(
      source_epoch, "control_accepted", "intervention_required",
      CanonicalValue(CanonicalValue::Object{}), "reset");
  const auto reset_digest =
      content_identity::sha256_digest(content_identity::canonical_json(reset));
  static_cast<void>(core.IngestOperationEvent(
      source_epoch, 2U, "control_accepted", "request.reset.collision.1",
      "operation.test.1", 1U, 301U, "2026-08-07T10:00:00.000000008Z", true,
      reset, reset_digest));
  static_cast<void>(core.CompleteOperationEventSnapshot(source_epoch, 2U));

  const auto returned_snapshot = core.AlarmSnapshot();
  const auto &returned = OnlyAlarm(returned_snapshot);
  EXPECT_EQ(Text(returned, "definition_id"),
            "manyforge.motion.runtime_collision");
  EXPECT_EQ(Text(returned, "lifecycle_state"), "returned_unacknowledged");
  EXPECT_EQ(Text(Member(returned, "details"), "reason"),
            "operation_reset_accepted");
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
  auto pending = WithField(CurrentStopFaultLatch(), "stop_certificate",
                           CanonicalValue(nullptr));
  pending = WithField(pending, "terminal_projection",
                      WithField(Member(pending, "terminal_projection"), "state",
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
          WithField(pending, "recovery_status", CanonicalValue("ready"))),
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
  const auto older = WithField(CurrentStopFaultLatch(), "latch_id",
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
  HeadlessApiCore core(Config(directory, "api.epoch.1"), Clocks());
  const std::string source_epoch = "execution-host.epoch.reset";
  const auto error = CanonicalValue(CanonicalValue::Object{
      {"code", CanonicalValue("execution_aborted")},
      {"details", CanonicalValue(CanonicalValue::Object{})},
      {"message", CanonicalValue("failed")},
  });
  const auto failure = OperationJournalPayload(source_epoch, "state_observed",
                                               "intervention_required", error);
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
                              "intervention_required", error, "reset");
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

} // namespace
} // namespace manyforge::headless

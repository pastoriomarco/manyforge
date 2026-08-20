#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/scenario_catalog.hpp>
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
      MANYFORGE_HEADLESS_TEST_NODE_CATALOG,
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
      32U,
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
      {"effect_pending", CanonicalValue(record_kind != "state_observed")},
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
  static_cast<void>(core.CompleteOperationEventSnapshot(
      "execution-host.epoch.idle", 0U));

  const auto alarms = core.AlarmSnapshot();
  EXPECT_TRUE(Array(alarms, "alarms").empty());
  EXPECT_TRUE(std::get<bool>(Member(alarms, "reconciled").storage()));
  const auto &domains = Array(alarms, "domains");
  ASSERT_EQ(domains.size(), 2U);
  const auto motion = std::find_if(
      domains.begin(), domains.end(),
      [](const auto &domain) { return Text(domain, "domain") == "motion"; });
  ASSERT_NE(motion, domains.end());
  EXPECT_TRUE(std::get<bool>(Member(*motion, "reconciled").storage()));
  const auto runtime_health = std::find_if(
      domains.begin(), domains.end(), [](const auto &domain) {
        return Text(domain, "domain") == "runtime_health";
      });
  ASSERT_NE(runtime_health, domains.end());
  EXPECT_TRUE(
      std::get<bool>(Member(*runtime_health, "reconciled").storage()));
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
  const auto replay = core.AcknowledgeAlarm(
      Text(provider, "condition_id"), Text(provider, "activation_id"),
      Unsigned(provider, "record_revision"), Caller(), "operator aware", true);
  EXPECT_EQ(replay, response);
  try {
    static_cast<void>(core.AcknowledgeAlarm(
        Text(provider, "condition_id"), Text(provider, "activation_id"),
        Unsigned(provider, "record_revision"), Caller(), "changed reason",
        true));
    FAIL() << "conflicting request replay was accepted";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_request_identity_conflict");
  }

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (HistoryCount(core) < 2U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_GE(HistoryCount(core), 2U);
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
     ServesTheExactVerifiedScenarioCatalogWithoutRuntimeSelection) {
  TemporaryDirectory directory;
  directory.CreateRetentionSkeleton();
  const auto catalog = deployment::build_scenario_catalog({
      deployment::ScenarioCatalogRowDTO{
          .scenario_id = "cell-pick",
          .description = "Exact maintained scenario",
          .aliases = {"pick"},
          .deployment_artifact_id = core::DeploymentArtifactId("cell_a"),
          .deployment_revision = core::DeploymentRevision(Digest('1')),
          .retained_bundle_id = core::RetainedBundleId(Digest('2')),
          .program_binding_id = core::ProgramBindingId(Digest('3')),
          .deployment_compatibility =
              deployment::ScenarioDeploymentCompatibilityDTO{"manyforge-only",
                                                             {}},
          .assistant_mode = deployment::ScenarioAssistantMode::kForbidden,
          .typed_parameter_overrides = CanonicalValue(CanonicalValue::Object{}),
      },
  });
  {
    deployment::FilesystemScenarioCatalogRepository repository(directory.path(),
                                                               "maintained");
    ASSERT_EQ(repository.compare_and_set(std::nullopt, catalog),
              deployment::ScenarioCatalogPublishResult::kPublished);
  }
  PublishHealthySources(directory, "open");
  HeadlessApiCore core(Config(directory, "api.epoch.1", directory.path()),
                       Clocks());

  const auto projection = core.ScenarioCatalogProjection();
  EXPECT_EQ(Text(projection, "api_version"),
            deployment::kScenarioCatalogApiVersion);
  EXPECT_EQ(Text(projection, "scenario_catalog_revision"),
            catalog->revision().value());
  const auto &scenarios = Array(projection, "scenarios");
  ASSERT_EQ(scenarios.size(), 1U);
  EXPECT_EQ(Text(scenarios.front(), "scenario_id"), "cell-pick");
  EXPECT_EQ(Text(scenarios.front(), "program_binding_id"), Digest('3'));

  try {
    static_cast<void>(core.ScenarioProgramSelection("pick"));
    FAIL() << "presentation alias selected a Program closure";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "scenario_program_selection_unavailable");
    EXPECT_NE(std::string(error.what()).find("exact canonical"),
              std::string::npos);
  }
}

TEST(HeadlessApiCore, PackageReadsRemainPinnedToTheStartupDirectoryDescriptor) {
  TemporaryDirectory directory;
  const auto package = directory.path() / "selected-package";
  const auto retained_package = directory.path() / "retained-package";
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT, package,
                        std::filesystem::copy_options::recursive);

  const deployment::FilesystemScenarioApplyCandidateSource scenarios(
      package, "maintained");
  const auto selection =
      scenarios.LoadExactProgramSelection("program_apply_fixture");
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(package);
  const auto bundle = bundles.get_verified(selection.retained_bundle_id);
  ASSERT_NE(bundle, nullptr);
  const auto cell_id = bundle->value().cell_graph.cell_id.value();

  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  static_cast<void>(
      manager.Publish("component_manager", "component-manager." + cell_id,
                      "manager.epoch.1", 1, ManagerPayloadFor(*bundle)));
  auto config = Config(directory, "api.epoch.1", package);
  config.cell_id = cell_id;
  HeadlessApiCore core(std::move(config), Clocks());
  std::filesystem::rename(package, retained_package);
  std::filesystem::create_directory(package);

  // First use verifies the Manager-selected retained bundle through the
  // descriptor and exercises the node-environment reader's anchored "/."
  // spelling.
  const auto environment = core.NodeEnvironmentProjection();
  EXPECT_EQ(Text(environment, "retained_bundle_id"),
            selection.retained_bundle_id.value());
  EXPECT_EQ(Text(environment, "graph_revision"),
            selection.graph_revision.value());
  EXPECT_EQ(Text(environment, "environment_digest"),
            selection.node_environment_digest.value());

  const auto catalog = core.ScenarioCatalogProjection();
  EXPECT_EQ(Text(catalog, "scenario_catalog_revision"),
            selection.scenario_catalog_revision.value());
  const auto &catalog_scenarios = Array(catalog, "scenarios");
  ASSERT_EQ(catalog_scenarios.size(), 1U);
  EXPECT_EQ(Text(catalog_scenarios.front(), "scenario_id"),
            selection.scenario_id);
  EXPECT_EQ(Text(catalog_scenarios.front(), "retained_bundle_id"),
            selection.retained_bundle_id.value());
  EXPECT_EQ(Text(catalog_scenarios.front(), "program_binding_id"),
            selection.program_binding_id.value());

  const auto prepared = core.PrepareProgramApply(
      selection.scenario_id, selection.scenario_catalog_revision.value(),
      content_identity::canonical_json(selection.normalized_program_root));
  EXPECT_EQ(Text(prepared, "retained_bundle_id"),
            selection.retained_bundle_id.value());
  EXPECT_EQ(Text(prepared, "graph_revision"), selection.graph_revision.value());
  EXPECT_EQ(Text(prepared, "baseline_program_binding_id"),
            selection.program_binding_id.value());
  EXPECT_EQ(Text(prepared, "node_environment_digest"),
            selection.node_environment_digest.value());

  const auto source_model = core.SourceModelProjection();
  EXPECT_EQ(Text(source_model, "retained_bundle_id"),
            selection.retained_bundle_id.value());
  EXPECT_EQ(Text(source_model, "graph_revision"),
            selection.graph_revision.value());
  const auto source_model_bytes =
      content_identity::canonical_json(source_model);
  EXPECT_NE(source_model_bytes.find(
                "\"model_instance_id\":\"lite6_fixture_instance\""),
            std::string::npos);
  EXPECT_NE(source_model_bytes.find("\"component_id\":\"lite6_arm\""),
            std::string::npos);

  const auto viewer = core.ViewerModelUrdf();
  EXPECT_EQ(viewer.retained_bundle_id().value(),
            selection.retained_bundle_id.value());
  EXPECT_EQ(viewer.graph_revision(), selection.graph_revision.value());
  EXPECT_EQ(viewer.cell_id(), cell_id);
  EXPECT_EQ(viewer.product_id().value(), "cell_viewer_model");
  EXPECT_FALSE(viewer.bytes().empty());

  EXPECT_TRUE(std::filesystem::is_directory(retained_package));
  EXPECT_TRUE(std::filesystem::is_empty(package));
}

TEST(HeadlessApiCore,
     ProgramApplyDependsOnFreshManagerButNotUnrelatedSceneAvailability) {
  TemporaryDirectory directory;
  const auto package = directory.path() / "program-apply-package";
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_ASSISTANT_PACKAGE_ROOT, package,
                        std::filesystem::copy_options::recursive);
  const deployment::FilesystemScenarioApplyCandidateSource scenarios(
      package, "maintained");
  const auto selection =
      scenarios.LoadExactProgramSelection("program_apply_fixture");
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(package);
  const auto bundle = bundles.get_verified(selection.retained_bundle_id);
  ASSERT_NE(bundle, nullptr);

  AtomicSourceSnapshotChannel manager(directory.publications(),
                                      "manager-source.json");
  const auto cell_id = bundle->value().cell_graph.cell_id.value();
  static_cast<void>(
      manager.Publish("component_manager", "component-manager." + cell_id,
                      "manager.epoch.1", 1, ManagerPayloadFor(*bundle)));

  auto config = Config(directory, "api.epoch.1", package);
  config.cell_id = cell_id;
  HeadlessApiCore core(std::move(config), Clocks());
  EXPECT_FALSE(
      std::get<bool>(Member(core.RuntimeSnapshot(), "complete").storage()));

  const auto prepared = core.PrepareProgramApply(
      selection.scenario_id, selection.scenario_catalog_revision.value(),
      content_identity::canonical_json(selection.normalized_program_root));
  const auto applied = core.CommitProgramApply(
      content_identity::canonical_json(prepared), std::nullopt);
  EXPECT_EQ(Text(applied, "program_revision"),
            selection.program_revision.value());
  EXPECT_EQ(core.AppliedProgramSelection(), applied);

  AtomicSourceSnapshotChannel scene(directory.publications(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel runtime(directory.publications(),
                                      "scene-runtime-source.json");
  static_cast<void>(scene.Publish("logical_scene", "logical-scene." + cell_id,
                                  "scene.epoch.1", 1, LogicalScenePayload()));
  static_cast<void>(runtime.Publish("scene_runtime", "scene-runtime." + cell_id,
                                    "scene-runtime.epoch.1", 1,
                                    SceneRuntimePayload("open", 1, cell_id)));
  const auto scenario = core.ScenarioProgramSelection(selection.scenario_id);
  EXPECT_EQ(Text(scenario, "schema_version"),
            kScenarioProgramSelectionSchemaVersion);
  EXPECT_EQ(content_identity::canonical_json(
                Member(scenario, "assistant_projection")),
            content_identity::canonical_json(
                selection.normalized_assistant_projection));
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
     HistorianReconciliationIsAttributedReplayedAndNeverSilentlyEvicted) {
  TemporaryDirectory directory;
  auto config = Config(directory, "api.epoch.1");
  config.mutation_replay_capacity = 2U;
  HeadlessApiCore core(std::move(config), Clocks());

  const auto first = core.ReconcileHistorianHealth(0U, Caller("request.1"));
  EXPECT_EQ(Text(first, "schema_version"),
            "manyforge.historian_reconciliation.v1");
  EXPECT_TRUE(std::get<bool>(Member(first, "reconciled").storage()));
  ASSERT_EQ(Array(first, "audit_event_ids").size(), 1U);
  EXPECT_EQ(core.ReconcileHistorianHealth(0U, Caller("request.1")), first);

  try {
    static_cast<void>(core.ReconcileHistorianHealth(1U, Caller("request.1")));
    FAIL() << "one request identity accepted conflicting reconciliation data";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_request_identity_conflict");
  }
  try {
    static_cast<void>(core.ReconcileHistorianHealth(
        0U, Caller("stale.request", "api.epoch.stale")));
    FAIL() << "stale owner epoch reconciled historian health";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "stale_request_owner_epoch");
  }

  const auto second = core.ReconcileHistorianHealth(0U, Caller("request.2"));
  EXPECT_TRUE(std::get<bool>(Member(second, "reconciled").storage()));
  try {
    static_cast<void>(core.ReconcileHistorianHealth(0U, Caller("request.3")));
    FAIL() << "full mutation ledger silently evicted an earlier request";
  } catch (const OperatorAlarmError &error) {
    EXPECT_EQ(error.code(), "headless_request_ledger_full");
  }
  EXPECT_EQ(core.ReconcileHistorianHealth(0U, Caller("request.1")), first);

  const auto deadline = std::chrono::steady_clock::now() + 2s;
  while (HistoryCount(core) < 2U &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::yield();
  }
  EXPECT_EQ(HistoryCount(core), 2U);
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

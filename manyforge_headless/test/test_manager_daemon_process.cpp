#include "../src/program_runner_publisher_fault_internal.hpp"
#include "../src/manager_reference_diagnostics_internal.hpp"
#include <fcntl.h>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/execution_host_unit_admission.hpp>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/types/cell_admission.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/activation_content_collection.hpp>
#include <manyforge_deployment/canonical_empty_admission.hpp>
#include <manyforge_deployment/ros2_control_execution_runtime_contract.hpp>
#include <manyforge_deployment/prepared_activation_repository.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/program_reference_facts.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/runtime_contract_registry.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "manyforge_headless/cell_admission_readiness_reducer.hpp"
#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

namespace component_manager = manyforge::component_manager;
namespace content_identity = manyforge::content_identity;
namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

TEST(ManagerReferenceDiagnostics, RetainsTypedRowsAndBoundsTheResponse) {
  using V = content_identity::CanonicalValue;
  const V response(V::Object{{"api_version", V("manyforge.manager_command_response.v2")},
      {"ok", V(false)}, {"error", V(V::Object{{"code", V("program_reference_validation_failed")},
          {"message", V("invalid reference")}, {"details", V(V::Object{})}, {"producer", V("manager")}})}});
  const deployment::ProgramReferenceDiagnostic row{
      "signal_missing", {"/tree/parameters/signal", std::string("read"), std::string("signal")},
      V(V::Object{{"component_id", V("device")}, {"signal_name", V("ready")}})};
  const auto result = manyforge::headless::detail::AttachReferenceDiagnostics(response, {row});
  const auto *error = result.find_object_member("error");
  ASSERT_NE(error, nullptr);
  const auto *detail = error->find_object_member("reference_validation");
  ASSERT_NE(detail, nullptr);
  EXPECT_EQ(detail->find_object_member("diagnostics")->as_array().front(),
            deployment::program_reference_diagnostic_value(row));
  EXPECT_EQ(detail->find_object_member("total_count")->as_number(), 1.0);
  EXPECT_FALSE(detail->find_object_member("truncated")->as_bool());

  auto large = row;
  large.payload = V(V::Object{{"value", V(std::string(1024U, 'x'))}});
  const std::vector<deployment::ProgramReferenceDiagnostic> many(100U, large);
  const auto bounded = manyforge::headless::detail::AttachReferenceDiagnostics(response, many);
  const auto *bounded_detail = bounded.find_object_member("error")->find_object_member("reference_validation");
  ASSERT_NE(bounded_detail, nullptr);
  EXPECT_TRUE(bounded_detail->find_object_member("truncated")->as_bool());
  EXPECT_EQ(bounded_detail->find_object_member("total_count")->as_number(), 100.0);
  EXPECT_LT(content_identity::canonical_json(bounded).size(), 64U * 1024U);
}

core::ProgramCycleStatusDTO IdleCycleStatus() {
  return {core::RequestOwnerEpoch("runner-owner"),
          4U,
          core::ProgramCycleStateDTO::kIdleAfterReset,
          std::nullopt,
          std::nullopt,
          std::nullopt};
}

TEST(ManagerCellAdmissionReadinessReducer,
     ProjectsOnlyOwnerCurrentHealthyConvergedSettledFacts) {
  const auto reduced = ReduceManagerCellAdmissionReadiness(
      {true, IdleCycleStatus(), true, 3U, 3U, true, true, 0U, 0U, 0U});

  EXPECT_TRUE(reduced.program_cycle_status_current);
  ASSERT_TRUE(reduced.program_cycle_status.has_value());
  EXPECT_EQ(reduced.program_cycle_status->state,
            core::ProgramCycleStateDTO::kIdleAfterReset);
  EXPECT_EQ(reduced.required_component_total, 3U);
  EXPECT_EQ(reduced.required_component_current_compatible_ready_converged, 3U);
  EXPECT_TRUE(reduced.deployment_status_current);
  EXPECT_TRUE(reduced.deployment_converged);
  EXPECT_EQ(reduced.blocking_fault_count, 0U);
  EXPECT_EQ(reduced.unresolved_owner_count, 0U);
  EXPECT_EQ(reduced.unsettled_effect_count, 0U);
}

TEST(ManagerCellAdmissionReadinessReducer,
     PreservesRunningStaleFaultedAndUnsettledClosureFacts) {
  auto running = IdleCycleStatus();
  running.state = core::ProgramCycleStateDTO::kRunning;
  running.program_run_id = core::ProgramRunId("run");
  running.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  const auto reduced = ReduceManagerCellAdmissionReadiness(
      {false, running, false, 3U, 2U, false, false, 1U, 1U, 1U});

  ASSERT_TRUE(reduced.program_cycle_status.has_value());
  EXPECT_EQ(reduced.program_cycle_status->state,
            core::ProgramCycleStateDTO::kRunning);
  EXPECT_FALSE(reduced.program_cycle_status_current);
  EXPECT_FALSE(reduced.required_component_status_current);
  EXPECT_NE(reduced.required_component_total,
            reduced.required_component_current_compatible_ready_converged);
  EXPECT_FALSE(reduced.deployment_status_current);
  EXPECT_FALSE(reduced.deployment_converged);
  EXPECT_EQ(reduced.blocking_fault_count, 1U);
  EXPECT_EQ(reduced.unresolved_owner_count, 1U);
  EXPECT_EQ(reduced.unsettled_effect_count, 1U);
}

TEST(ManagerCellAdmissionReadinessReducer,
     AcceptsOnlyTheCurrentProgramRunnerOwner) {
  const auto before = IdleCycleStatus();
  auto after = before;
  after.program_runner_owner_epoch =
      core::RequestOwnerEpoch("runner-successor");
  EXPECT_TRUE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));

  // Cycle progression is owned by the Runner and may happen between Manager's
  // probe and renewal exchange.
  after.cycle_generation += 1U;
  EXPECT_TRUE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));
  after = before;
  after.program_runner_owner_epoch =
      core::RequestOwnerEpoch("foreign-successor");
  EXPECT_FALSE(IsCurrentProgramRunnerOwner(
      after, core::RuntimeEpoch("runner-successor")));
}

TEST(ManagerCellAdmissionReadinessReducer,
     RunnerContactLossRequiresPreviouslyQualifiedActiveRun) {
  const core::RequestOwnerEpoch manager("manager.owner");
  const core::RuntimeEpoch manager_runtime("manager.runtime");
  const auto loss = [&](const std::optional<core::ProgramCycleStatusDTO> &cycle,
                        std::string_view predicate =
                            "program_runner_liveness_probe",
                        std::string_view message = "contact failed") {
    return MakeProgramRunnerContactLoss(cycle, manager, manager_runtime,
                                        predicate, message);
  };
  EXPECT_FALSE(loss(std::nullopt));
  EXPECT_FALSE(loss(IdleCycleStatus()));
  auto cycle = IdleCycleStatus();
  cycle.program_run_id = core::ProgramRunId("run");
  cycle.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  for (const auto state : {core::ProgramCycleStateDTO::kStarting,
                           core::ProgramCycleStateDTO::kRunning,
                           core::ProgramCycleStateDTO::kStopping}) {
    cycle.state = state;
    for (const auto predicate :
         {"program_runner_liveness_probe", "program_runner_liveness_renewal",
          "program_runner_reconciliation_renewal",
          "program_runner_publication_fault"}) {
      const auto fact = loss(cycle, predicate);
      ASSERT_TRUE(fact);
      EXPECT_EQ(fact->established_cycle, cycle);
      EXPECT_EQ(fact->failure_predicate, predicate);
    }
  }
  EXPECT_FALSE(loss(cycle, "required_native_receiver_observable"));
  cycle.state = core::ProgramCycleStateDTO::kTerminalAwaitingReset;
  cycle.terminal_or_settlement_reference =
      core::ContentDigest("sha256:" + std::string(64U, 'a'));
  EXPECT_FALSE(loss(cycle));
  cycle.state = core::ProgramCycleStateDTO::kResetting;
  EXPECT_FALSE(loss(cycle));
  cycle.state = core::ProgramCycleStateDTO::kRunning;
  cycle.terminal_or_settlement_reference.reset();
  const auto bounded =
      loss(cycle, "program_runner_liveness_probe", std::string(2048U, 'x'));
  ASSERT_TRUE(bounded);
  EXPECT_EQ(bounded->message.size(), 1024U);
}

TEST(ManagerProgramRunnerPublicationFault,
     NullableTypedErrorIsClosedAndBounded) {
  using content_identity::CanonicalValue;
  const auto fault = [](std::string message =
                            "final key write was not acknowledged") {
    return CanonicalValue(CanonicalValue::Object{
        {"code", CanonicalValue("program_bbkey_publication_failed")},
        {"message", CanonicalValue(std::move(message))},
        {"details", CanonicalValue(CanonicalValue::Object{})}});
  };
  EXPECT_FALSE(
      detail::DecodeProgramRunnerPublisherFault(CanonicalValue(nullptr)));
  const auto decoded = detail::DecodeProgramRunnerPublisherFault(fault());
  ASSERT_TRUE(decoded);
  EXPECT_EQ(decoded->code, "program_bbkey_publication_failed");
  EXPECT_EQ(decoded->message, "final key write was not acknowledged");
  EXPECT_TRUE(decoded->details.empty());
  for (const auto &invalid :
       {CanonicalValue(false),
        CanonicalValue(CanonicalValue::Object{
            {"code", CanonicalValue("program_bbkey_publication_failed")}}),
        CanonicalValue(CanonicalValue::Object{
            {"code", CanonicalValue("program_runner_liveness_probe")},
            {"message", CanonicalValue("alive")},
            {"details", CanonicalValue(CanonicalValue::Object{})}}),
        CanonicalValue(CanonicalValue::Object{
            {"code", CanonicalValue("program_bbkey_publication_failed")},
            {"message", CanonicalValue("failed")},
            {"details",
             CanonicalValue(CanonicalValue::Object{
                 {"failure_predicate", CanonicalValue("caller.asserted")}})}}),
        fault(std::string(1025U, 'x'))}) {
    EXPECT_THROW(
        static_cast<void>(detail::DecodeProgramRunnerPublisherFault(invalid)),
        std::invalid_argument);
  }
  // Invalid UTF-8 never reaches the decoder: no CanonicalValue can hold it.
  EXPECT_THROW(static_cast<void>(
                   fault(std::string("bad") + static_cast<char>(0xff))),
               content_identity::CanonicalValueError);
}

TEST(ManagerProgramRunnerPublicationFault,
     RawStateIsClosedBeforeDigestAndFaultClassification) {
  EXPECT_EQ(detail::DecodeProgramRunnerManagerState("current"),
            core::ManagerLivenessStateDTO::kCurrent);
  EXPECT_EQ(detail::DecodeProgramRunnerManagerState("unreconciled"),
            core::ManagerLivenessStateDTO::kUnreconciled);
  for (const auto state : {"garbage", "expired", "", "Current"}) {
    EXPECT_THROW(
        static_cast<void>(detail::DecodeProgramRunnerManagerState(state)),
        std::invalid_argument);
  }
}

TEST(ManagerProgramRunnerPublicationFault,
     CurrentFencedPublisherIsTypedWithoutInventingRunnerDeath) {
  auto cycle = IdleCycleStatus();
  cycle.state = core::ProgramCycleStateDTO::kRunning;
  cycle.program_run_id = core::ProgramRunId("run");
  cycle.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  const auto fault =
      content_identity::CanonicalValue(content_identity::CanonicalValue::Object{
          {"code", content_identity::CanonicalValue(
                       "program_bbkey_publication_failed")},
          {"message", content_identity::CanonicalValue("key ACK missing")},
          {"details", content_identity::CanonicalValue(
                          content_identity::CanonicalValue::Object{})}});
  const core::RuntimeEpoch receiver("runner-owner");
  // Golden vector produced by the existing Python RFC8785 helper, not a
  // locally invented framing or an assumed Manager process-death reference.
  const std::optional<std::string> reference =
      "sha256:dcf9f1d846eb19d655b0da3da50f978ca68913d08f206476e20787d68f13686b";
  EXPECT_EQ(
      content_identity::sha256_digest(content_identity::canonical_json(fault)),
      *reference);
  EXPECT_NO_THROW(detail::CheckProgramRunnerPublicationHealth(
      content_identity::CanonicalValue(nullptr), cycle, receiver, false,
      std::nullopt));
  EXPECT_THROW(detail::CheckProgramRunnerPublicationHealth(
                   fault, cycle, receiver, false, reference),
               std::invalid_argument);
  EXPECT_THROW(
      detail::CheckProgramRunnerPublicationHealth(
          fault, cycle, core::RuntimeEpoch("foreign-runner"), true, reference),
      std::invalid_argument);
  EXPECT_THROW(detail::CheckProgramRunnerPublicationHealth(
                   fault, cycle, receiver, true, std::nullopt),
               std::invalid_argument);
  EXPECT_THROW(detail::CheckProgramRunnerPublicationHealth(
                   fault, cycle, receiver, true,
                   std::string("sha256:") + std::string(64U, 'a')),
               std::invalid_argument);
  try {
    detail::CheckProgramRunnerPublicationHealth(fault, cycle, receiver, true,
                                                reference);
    FAIL() << "publisher failure did not stop normal renewal progression";
  } catch (const detail::ProgramRunnerPublisherFault &error) {
    EXPECT_EQ(error.cycle(), cycle);
    EXPECT_EQ(std::string(error.what()),
              "program_bbkey_publication_failed: key ACK missing");
    const auto loss = MakeProgramRunnerContactLoss(
        cycle, core::RequestOwnerEpoch("manager"),
        core::RuntimeEpoch("manager.runtime"),
        "program_runner_publication_fault", error.what());
    ASSERT_TRUE(loss);
    EXPECT_EQ(loss->established_cycle, cycle);
    EXPECT_EQ(loss->failure_predicate, "program_runner_publication_fault");
  }
}

TEST(ManagerProgramRunnerPublicationFault,
     RetainedCauseRemainsTypedAtUtf8Boundary) {
  std::string message;
  for (std::size_t i = 0; i < 512U; ++i)
    message += "é";
  const auto retained = detail::ProgramRunnerPublisherFaultMessage(
      {"program_bbkey_publication_failed", message, {}});
  EXPECT_LE(retained.size(), 1024U);
  EXPECT_TRUE(content_identity::is_utf8_nfc(retained));
  EXPECT_TRUE(retained.starts_with("program_bbkey_publication_failed: "));
  auto cycle = IdleCycleStatus();
  cycle.state = core::ProgramCycleStateDTO::kRunning;
  cycle.program_run_id = core::ProgramRunId("run");
  cycle.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  const auto loss = MakeProgramRunnerContactLoss(
      cycle, core::RequestOwnerEpoch("manager"),
      core::RuntimeEpoch("manager.runtime"), "program_runner_publication_fault",
      retained);
  ASSERT_TRUE(loss);
  EXPECT_EQ(loss->failure_predicate, "program_runner_publication_fault");
  EXPECT_EQ(loss->message, retained);
  EXPECT_EQ(loss->established_cycle, cycle);
  EXPECT_FALSE(MakeProgramRunnerContactLoss(
      IdleCycleStatus(), core::RequestOwnerEpoch("manager"),
      core::RuntimeEpoch("manager.runtime"), "program_runner_publication_fault",
      retained));
}

TEST(ManagerCellAdmissionReadinessReducer,
     IdleToCapturedRunDoesNotInventAnAdmissionOutage) {
  const auto before = ReduceManagerCellAdmissionReadiness(
      {true, IdleCycleStatus(), true, 3U, 3U, true, true, 0U, 0U, 0U});
  auto after = before;
  auto &cycle = *after.program_cycle_status;
  cycle.state = core::ProgramCycleStateDTO::kStarting;
  cycle.program_run_id = core::ProgramRunId("run");
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  cycle.cycle_claim_generation = core::CellAdmissionClaimGeneration(9U);
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  cycle.state = core::ProgramCycleStateDTO::kRunning;
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  EXPECT_FALSE(IsSameProgramAdmissionProgress(after, before));
  for (const auto state : {core::ProgramCycleStateDTO::kStopping,
                           core::ProgramCycleStateDTO::kResetting,
                           core::ProgramCycleStateDTO::kTerminalAwaitingReset,
                           core::ProgramCycleStateDTO::kRecoveryRequired}) {
    auto changed = after;
    changed.program_cycle_status->state = state;
    EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  }
  auto changed = after;
  changed.program_cycle_status->program_runner_owner_epoch =
      core::RequestOwnerEpoch("foreign");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  ++changed.program_cycle_status->cycle_generation;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.blocking_fault_count = 1U;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.program_cycle_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
}

TEST(ManagerCellAdmissionReadinessReducer,
     StartingToRunningDoesNotInventAnAdmissionOutage) {
  auto starting = IdleCycleStatus();
  starting.state = core::ProgramCycleStateDTO::kStarting;
  starting.program_run_id = core::ProgramRunId("run");
  auto before = ReduceManagerCellAdmissionReadiness(
      {true, starting, true, 3U, 3U, true, true, 0U, 0U, 0U});
  auto after = before;
  after.program_cycle_status->state = core::ProgramCycleStateDTO::kRunning;
  after.program_cycle_status->cycle_claim_generation =
      core::CellAdmissionClaimGeneration(9U);
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, after));
  auto claimed_starting = after;
  claimed_starting.program_cycle_status->state =
      core::ProgramCycleStateDTO::kStarting;
  EXPECT_TRUE(IsSameProgramAdmissionProgress(before, claimed_starting));
  EXPECT_TRUE(IsSameProgramAdmissionProgress(claimed_starting, after));
  auto replaced_claim = claimed_starting;
  replaced_claim.program_cycle_status->cycle_claim_generation =
      core::CellAdmissionClaimGeneration(10U);
  EXPECT_FALSE(
      IsSameProgramAdmissionProgress(claimed_starting, replaced_claim));
  for (const auto state :
       {core::ProgramCycleStateDTO::kStopping,
        core::ProgramCycleStateDTO::kResetting,
        core::ProgramCycleStateDTO::kTerminalAwaitingReset}) {
    auto changed = after;
    changed.program_cycle_status->state = state;
    EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  }
  auto changed = after;
  changed.program_cycle_status->program_runner_owner_epoch =
      core::RequestOwnerEpoch("other");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.program_cycle_status->program_run_id = core::ProgramRunId("other");
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  ++changed.program_cycle_status->cycle_generation;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.blocking_fault_count = 1;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  changed = after;
  changed.deployment_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, changed));
  before.program_cycle_status_current = false;
  EXPECT_FALSE(IsSameProgramAdmissionProgress(before, after));
}

constexpr std::string_view kDeploymentRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

const content_identity::CanonicalValue &
RequiredMember(const content_identity::CanonicalValue &value,
               const std::string_view name) {
  const auto *member = value.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("Manager source fixture lacks " +
                             std::string(name));
  }
  return *member;
}

/// Whether one Manager source value pairs its active publication with an
/// Apply history of the same generation: publication 1 is the bootstrap and
/// each later one is exactly one committed named Apply. These short sessions
/// evict nothing, so the counts match exactly (Headless itself only refuses
/// more committed Applies than publications: program_activation_history_incomplete).
bool ManagerSourceHistoryIsOneGeneration(
    const content_identity::CanonicalValue &payload) {
  const auto generation =
      RequiredMember(RequiredMember(RequiredMember(payload, "active"),
                                    "publication"),
                     "publication_generation")
          .as_number();
  double committed = 0.0;
  for (const auto &record :
       RequiredMember(RequiredMember(payload, "apply"), "records").as_array()) {
    if (RequiredMember(record, "committed").as_bool()) {
      committed += 1.0;
    }
  }
  return committed + 1.0 == generation;
}

const std::string &RequiredText(const content_identity::CanonicalValue &value,
                                const std::string_view name) {
  const auto *text =
      std::get_if<std::string>(&RequiredMember(value, name).storage());
  if (text == nullptr || text->empty()) {
    throw std::runtime_error("Manager Apply metadata has invalid " +
                             std::string(name));
  }
  return *text;
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
struct RuntimeOfferProjection final {
  std::string selected;
  std::string observed;
  std::string observed_epoch;
};

RuntimeOfferProjection
ReadRuntimeOfferProjection(const content_identity::CanonicalValue &payload) {
  const auto &runtime = RequiredMember(payload, "runtime");
  const auto &desired = RequiredMember(runtime, "desired_units").as_array();
  const auto &observed = RequiredMember(runtime, "observed_units").as_array();
  if (desired.size() != 1U || observed.size() != 1U) {
    throw std::runtime_error(
        "Manager source fixture must contain one runtime projection row");
  }
  return {
      RequiredText(desired.front(), "selected_runtime_contract_offer_digest"),
      RequiredText(observed.front(), "observed_runtime_contract_offer_digest"),
      RequiredText(observed.front(), "runtime_epoch"),
  };
}

/// What generate_manager_apply_package writes for every profile: one example
/// Scenario cell (the physical candidate), Close (the Apply of the empty
/// intent), the cell-scoped canonical-empty predecessor, the fixture Program
/// and its prepared activations.
struct ManagerApplyFixtureMetadata final {
  std::string admission_filename;
  std::string cell_id;
  std::string close_deployment_artifact_id;
  std::string close_deployment_revision;
  std::string close_retained_bundle_id;
  std::string physical_candidate_retained_bundle_id;
  std::string physical_deployment_artifact_id;
  std::string physical_deployment_revision;
  std::string predecessor_graph_revision;
  std::string predecessor_retained_bundle_id;
  /// The stored prepared activation of the physical cell for the
  /// `program_apply_fixture` Scenario.
  std::string prepared_activation_id;
  std::string program_binding_id;
  std::string program_id;
  std::string program_revision;
};

ManagerApplyFixtureMetadata ReadManagerApplyFixtureMetadata(
    const std::filesystem::path &metadata_path =
        MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_METADATA) {
  std::ifstream stream(metadata_path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("Manager Apply fixture metadata is absent");
  }
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  const auto root = deployment::parse_strict_json_model_yaml(bytes).root();
  if (!root.is_object() || root.as_object().size() != 15U ||
      content_identity::canonical_json(root) != bytes) {
    throw std::runtime_error("Manager Apply fixture metadata is invalid");
  }
  static_cast<void>(RequiredText(root, "physical_graph_revision"));
  const auto &prepared = RequiredMember(root, "prepared_activations");
  return {
      RequiredText(root, "admission_filename"),
      RequiredText(root, "cell_id"),
      RequiredText(root, "close_deployment_artifact_id"),
      RequiredText(root, "close_deployment_revision"),
      RequiredText(root, "close_retained_bundle_id"),
      RequiredText(root, "physical_candidate_retained_bundle_id"),
      RequiredText(root, "physical_deployment_artifact_id"),
      RequiredText(root, "physical_deployment_revision"),
      RequiredText(root, "predecessor_graph_revision"),
      RequiredText(root, "predecessor_retained_bundle_id"),
      RequiredText(RequiredMember(prepared, "program_apply_fixture"),
                   "prepared_activation_id"),
      RequiredText(root, "program_binding_id"),
      RequiredText(root, "program_id"),
      RequiredText(root, "program_revision"),
  };
}


#endif

class DaemonDirectory final {
public:
  explicit DaemonDirectory(bool preserve_on_failure = false)
      : preserve_on_failure_(preserve_on_failure) {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-daemon-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("Manager daemon test mkdtemp failed");
    }
    root_ = created;
    std::filesystem::create_directory(manager_root());
    std::filesystem::create_directory(manager_run_root());
    std::filesystem::create_directory(execution_state_root());
    std::filesystem::create_directory(execution_run_root());
    std::filesystem::create_directory(publication_root());
    std::filesystem::create_directory(activation_root());
  }

  ~DaemonDirectory() {
    if (preserve_on_failure_ &&
        (::testing::Test::HasFailure() || std::uncaught_exceptions() > initial_uncaught_exceptions_)) {
      // One private tree per run. Never delete a previous failed run here.
      std::cerr << "Manager process failure evidence retained at " << root_
                << '\n';
      return;
    }
    std::error_code error;
    std::filesystem::remove_all(root_, error);
  }

  [[nodiscard]] std::filesystem::path manager_root() const {
    return root_ / "manager";
  }
  [[nodiscard]] std::filesystem::path execution_state_root() const {
    return root_ / "execution-state";
  }
  [[nodiscard]] std::filesystem::path execution_run_root() const {
    return root_ / "execution-run";
  }
  [[nodiscard]] std::filesystem::path manager_run_root() const {
    return root_ / "manager-run";
  }
  [[nodiscard]] std::filesystem::path publication_root() const {
    return root_ / "publications";
  }
  [[nodiscard]] std::filesystem::path scene_database() const {
    return root_ / "logical-scene.sqlite3";
  }
  [[nodiscard]] std::filesystem::path program_cycle_journal() const {
    return root_ / "program-cycle.sqlite3";
  }
  [[nodiscard]] std::filesystem::path socket_path() const {
    return manager_run_root() / "manager-command.sock";
  }
  [[nodiscard]] std::filesystem::path startup_stderr() const {
    return root_ / "manager-startup.stderr";
  }
  [[nodiscard]] std::filesystem::path execution_socket_path() const {
    return execution_run_root() / "execution-command.sock";
  }
  [[nodiscard]] std::filesystem::path
  program_runner_liveness_socket_path() const {
    return manager_run_root() / "program-runner-liveness.sock";
  }
  [[nodiscard]] std::filesystem::path program_runner_log_path() const {
    return root_ / "program-runner-process.stderr";
  }
  [[nodiscard]] std::filesystem::path installed_root() const {
    return root_ / "installed";
  }
  [[nodiscard]] std::filesystem::path apply_package_root() const {
    return root_ / "apply-package";
  }
  /// Trusted activation store: the Manager admits only prepared values
  /// published here (disjoint from every install/runtime root).
  [[nodiscard]] std::filesystem::path activation_root() const {
    return root_ / "activation";
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path root_;
  bool preserve_on_failure_;
  const int initial_uncaught_exceptions_{std::uncaught_exceptions()};
};

std::string SendCommand(const std::filesystem::path &path,
                        const std::string &request);
void SendCommandAndDisconnect(const std::filesystem::path &path,
                              const std::string &request);

class ManagerProcess final {
public:
  explicit ManagerProcess(
      const DaemonDirectory &directory,
      std::string package_root =
          MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
      std::string admission_filename = "canonical-empty-admission.json",
      std::string executable = MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON,
      std::string program_runner_liveness_socket = {},
      std::string projection_root = {}) {
    const auto manager_root = directory.manager_root().string();
    const auto manager_run_root = directory.manager_run_root().string();
    const auto execution_state_root = directory.execution_state_root().string();
    const auto execution_run_root = directory.execution_run_root().string();
    const auto scene_database = directory.scene_database().string();
    const auto program_cycle_journal =
        directory.program_cycle_journal().string();
    const auto publication_root = projection_root.empty()
                                      ? directory.publication_root().string()
                                      : projection_root;
    const auto startup_stderr = directory.startup_stderr().string();
    const auto activation_root = directory.activation_root().string();
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("Manager daemon test fork failed");
    }
    if (pid_ == 0) {
      const int stderr_fd =
          ::open(startup_stderr.c_str(),
                 O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, S_IRUSR | S_IWUSR);
      if (stderr_fd < 0 || ::dup2(stderr_fd, STDERR_FILENO) < 0) {
        ::_exit(127);
      }
      static_cast<void>(::close(stderr_fd));
      if (program_runner_liveness_socket.empty()) {
        ::execl(executable.c_str(), executable.c_str(), "--install-root",
                package_root.c_str(), "--activation-root",
                activation_root.c_str(), "--canonical-empty-admission-filename",
                admission_filename.c_str(), "--manager-state-root",
                manager_root.c_str(), "--manager-run-root",
                manager_run_root.c_str(), "--execution-state-root",
                execution_state_root.c_str(), "--execution-run-root",
                execution_run_root.c_str(), "--logical-scene-database",
                scene_database.c_str(), "--program-cycle-journal",
                program_cycle_journal.c_str(), "--projection-root",
                publication_root.c_str(), "--reconciliation-period-ms", "10",
                static_cast<char *>(nullptr));
      } else {
        ::execl(
            executable.c_str(), executable.c_str(), "--install-root",
            package_root.c_str(), "--activation-root", activation_root.c_str(),
            "--canonical-empty-admission-filename",
            admission_filename.c_str(), "--manager-state-root",
            manager_root.c_str(), "--manager-run-root",
            manager_run_root.c_str(), "--execution-state-root",
            execution_state_root.c_str(), "--execution-run-root",
            execution_run_root.c_str(), "--logical-scene-database",
            scene_database.c_str(), "--program-cycle-journal",
            program_cycle_journal.c_str(), "--projection-root",
            publication_root.c_str(), "--program-runner-liveness-socket",
            program_runner_liveness_socket.c_str(),
            "--reconciliation-period-ms", "10", static_cast<char *>(nullptr));
      }
      ::_exit(127);
    }
  }

  ~ManagerProcess() {
    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGKILL));
      int status = 0;
      static_cast<void>(::waitpid(pid_, &status, 0));
    }
  }

  ManagerProcess(const ManagerProcess &) = delete;
  ManagerProcess &operator=(const ManagerProcess &) = delete;

  void WaitReady(const DaemonDirectory &directory) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      const auto waited = ::waitpid(pid_, &status, WNOHANG);
      if (waited == pid_) {
        pid_ = -1;
        std::ifstream input(directory.startup_stderr(), std::ios::binary);
        const std::string detail{std::istreambuf_iterator<char>(input),
                                 std::istreambuf_iterator<char>()};
        throw std::runtime_error(
            "Manager daemon exited before readiness" +
            (detail.empty() ? std::string{} : ": " + detail));
      }
      if (std::filesystem::is_socket(directory.socket_path()) &&
          std::filesystem::is_regular_file(directory.publication_root() /
                                           "manager-source.json") &&
          std::filesystem::is_regular_file(directory.publication_root() /
                                           "logical-scene-source.json")) {
        try {
          const auto response =
              SendCommand(directory.socket_path(),
                          "{\"api_version\":\"manyforge.manager_command.v1\","
                          "\"command\":\"reconcile\"}");
          if (response.find("\"ok\":true") != std::string::npos) {
            return;
          }
        } catch (...) {
          // A stale socket may survive an ungraceful predecessor. Readiness
          // requires a response from the replacement, not path existence.
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    throw std::runtime_error("Manager daemon readiness timed out");
  }

  [[nodiscard]] pid_t pid() const noexcept { return pid_; }

  int Stop(const int signal) {
    if (pid_ <= 0) {
      throw std::runtime_error("Manager daemon is not running");
    }
    if (::kill(pid_, signal) != 0) {
      throw std::runtime_error("Manager daemon signal failed");
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) != pid_) {
      throw std::runtime_error("Manager daemon wait failed");
    }
    pid_ = -1;
    return status;
  }

  int Wait() {
    if (pid_ <= 0) {
      throw std::runtime_error("Manager daemon is not running");
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) != pid_) {
      throw std::runtime_error("Manager daemon wait failed");
    }
    pid_ = -1;
    return status;
  }

private:
  pid_t pid_{-1};
};

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
class ProgramRunnerProcess final {
public:
  ProgramRunnerProcess(const DaemonDirectory &directory,
                       const std::string &cell_id)
      : socket_path_(directory.program_runner_liveness_socket_path()),
        log_path_(directory.program_runner_log_path()) {
    const auto execution_run_root = directory.execution_run_root().string();
    const auto journal = directory.program_cycle_journal().string();
    const auto socket = socket_path_.string();
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("Program Runner test fork failed");
    }
    if (pid_ == 0) {
      const auto log = log_path_.string();
      const int log_fd =
          ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
                 S_IRUSR | S_IWUSR);
      if (log_fd < 0 || ::dup2(log_fd, STDOUT_FILENO) < 0 ||
          ::dup2(log_fd, STDERR_FILENO) < 0) {
        ::_exit(127);
      }
      static_cast<void>(::close(log_fd));
      const char *existing = ::getenv("PYTHONPATH");
      std::string python_path(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT);
      python_path.append(":")
          .append(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT)
          .append("/manyforge_behavior:")
          .append(MANYFORGE_HEADLESS_TEST_PYTHON_MODULE_ROOT);
      if (existing != nullptr && *existing != '\0') {
        python_path.append(":").append(existing);
      }
      if (::setenv("PYTHONPATH", python_path.c_str(), 1) != 0) {
        ::_exit(127);
      }
      ::execl(
          MANYFORGE_HEADLESS_TEST_PYTHON, MANYFORGE_HEADLESS_TEST_PYTHON, "-m",
          "manyforge_program_runner", "--host", "127.0.0.1", "--port", "0",
          "--headless-url", "http://127.0.0.1:1", "--execution-run-root",
          execution_run_root.c_str(), "--cycle-journal-path", journal.c_str(),
          "--manager-liveness-socket", socket.c_str(), "--cell-id",
          cell_id.c_str(), "--owner-id", "program-runner.manager-process-test",
          "--headless-timeout-s", "0.25", static_cast<char *>(nullptr));
      ::_exit(127);
    }
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(8);
    while (std::chrono::steady_clock::now() < deadline) {
      int status = 0;
      if (::waitpid(pid_, &status, WNOHANG) == pid_) {
        pid_ = -1;
        throw std::runtime_error("Program Runner exited before liveness ready" +
                                 LogDetail());
      }
      std::error_code error;
      if (std::filesystem::is_socket(socket_path_, error) && !error) {
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    static_cast<void>(::kill(pid_, SIGTERM));
    int status = 0;
    static_cast<void>(::waitpid(pid_, &status, 0));
    pid_ = -1;
    throw std::runtime_error("Program Runner liveness readiness timed out" +
                             LogDetail());
  }

  ~ProgramRunnerProcess() {
    if (pid_ > 0) {
      static_cast<void>(::kill(pid_, SIGTERM));
      int status = 0;
      static_cast<void>(::waitpid(pid_, &status, 0));
    }
  }

  ProgramRunnerProcess(const ProgramRunnerProcess &) = delete;
  ProgramRunnerProcess &operator=(const ProgramRunnerProcess &) = delete;

  [[nodiscard]] const std::filesystem::path &socket_path() const noexcept {
    return socket_path_;
  }

private:
  [[nodiscard]] std::string LogDetail() const {
    std::ifstream input(log_path_, std::ios::binary);
    const std::string detail{std::istreambuf_iterator<char>(input),
                             std::istreambuf_iterator<char>()};
    return detail.empty() ? std::string{} : std::string(": ") + detail;
  }

  std::filesystem::path socket_path_;
  std::filesystem::path log_path_;
  pid_t pid_{-1};
};
#endif

std::vector<pid_t> ReadDirectChildren(const pid_t parent) {
  std::vector<pid_t> children;
  std::error_code error;
  std::filesystem::directory_iterator iterator(
      "/proc", std::filesystem::directory_options::skip_permission_denied,
      error);
  const std::filesystem::directory_iterator end;
  while (!error && iterator != end) {
    const auto pid_spelling = iterator->path().filename().string();
    if (!pid_spelling.empty() &&
        std::all_of(pid_spelling.begin(), pid_spelling.end(),
                    [](const unsigned char character) {
                      return std::isdigit(character) != 0;
                    })) {
      std::ifstream status(iterator->path() / "status");
      std::string field;
      pid_t observed_parent = 0;
      char process_state = '\0';
      while (status >> field) {
        if (field == "State:") {
          static_cast<void>(status >> process_state);
        } else if (field == "PPid:") {
          static_cast<void>(status >> observed_parent);
        }
        status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
      }
      if (observed_parent == parent && process_state != 'Z') {
        children.push_back(static_cast<pid_t>(std::stol(pid_spelling)));
      }
    }
    iterator.increment(error);
  }
  std::sort(children.begin(), children.end());
  return children;
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
std::filesystem::path PrepareManagerApplyInstalledTargets(
    const DaemonDirectory &directory,
    const std::filesystem::path &host_source,
    const std::filesystem::path &worker_source,
    std::string_view installed_host_id) {
  std::filesystem::create_directory(directory.installed_root());
  const auto manager = directory.installed_root() / "manyforge_manager_daemon";
  const auto host = directory.installed_root() / installed_host_id;
  const auto candidate_worker =
      directory.installed_root() / "manyforge_roboplan_candidate_worker";
  std::filesystem::copy_file(MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON, manager,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(host_source, host,
                             std::filesystem::copy_options::overwrite_existing);
  std::filesystem::copy_file(worker_source,
                             candidate_worker,
                             std::filesystem::copy_options::overwrite_existing);
  constexpr auto protected_executable_permissions =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write |
      std::filesystem::perms::owner_exec | std::filesystem::perms::group_read |
      std::filesystem::perms::group_exec | std::filesystem::perms::others_read |
      std::filesystem::perms::others_exec;
  std::filesystem::permissions(manager, protected_executable_permissions,
                               std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      host, std::filesystem::status(host_source).permissions(),
      std::filesystem::perm_options::replace);
  std::filesystem::permissions(
      candidate_worker,
      std::filesystem::status(worker_source)
          .permissions(),
      std::filesystem::perm_options::replace);
  return manager;
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
std::filesystem::path PrepareManagerApplyInstalledTargets(
    const DaemonDirectory &directory,
    const std::filesystem::path &ros_host_source =
        MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST) {
  return PrepareManagerApplyInstalledTargets(
      directory, ros_host_source,
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER,
      "manyforge_ros2_control_execution_host");
}

struct NativePackageIdentity final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  deployment::VerifiedRetainedRuntimeUnitProduct product;
  core::RuntimeContractOfferDTO offer;
  std::string executable_digest;
};

NativePackageIdentity
ReadNativePackageIdentity(const std::filesystem::path &package_root,
                          const std::string_view retained_bundle_id,
                          const std::filesystem::path &installed_host) {
  deployment::FilesystemVerifiedRetainedBundleSource bundle_source(
      package_root);
  deployment::FilesystemVerifiedRetainedProductArtifactSource artifact_source(
      package_root);
  const auto bundle = bundle_source.get_verified(
      core::RetainedBundleId(std::string(retained_bundle_id)));
  if (bundle == nullptr ||
      bundle->value().cell_graph.runtime_units.size() != 1U) {
    throw std::runtime_error("native package identity lacks one runtime");
  }
  const auto runtime_id =
      bundle->value().cell_graph.runtime_units.front().runtime_unit_id;
  const auto product = deployment::verify_runtime_unit_product(
      *bundle, runtime_id, artifact_source);
  if (!product.has_value() ||
      product->product_contract_id().value() !=
          deployment::ros2_control_execution_runtime_contract::kProductContractId) {
    throw std::runtime_error("package identity lacks the selected ROS product");
  }
  const auto *qualified =
      deployment::runtime_contract_declaration_registry().find(
          bundle->value().cell_graph.runtime_units.front().runtime_profile_id,
          bundle->value()
              .cell_graph.runtime_units.front()
              .runtime_profile_revision,
          product->product_contract_id(), product->product_contract_revision());
  if (qualified == nullptr) {
    throw std::runtime_error(
        "native package identity lacks its declared runtime offer");
  }
  std::ifstream stream(installed_host, std::ios::binary);
  const std::string bytes{std::istreambuf_iterator<char>(stream),
                          std::istreambuf_iterator<char>()};
  if (!stream && !stream.eof()) {
    throw std::runtime_error("native package identity target is unreadable");
  }
  return {bundle, *product, qualified->offer(),
          content_identity::sha256_digest(bytes)};
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
struct ExecutionReadyObservation final {
  std::string response;
  std::string owner_epoch;
  bool observed_closed{};
};

ExecutionReadyObservation WaitForExecutionReady(
    const DaemonDirectory &directory,
    const std::string_view expected_retained_bundle_id,
    const std::chrono::seconds timeout = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  ExecutionReadyObservation observation;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      observation.response = SendCommand(
          directory.execution_socket_path(),
          R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
      if (observation.response.find("\"code\":\"execution_owner_not_ready\"") !=
          std::string::npos) {
        observation.observed_closed = true;
      }
      if (observation.response.find("\"ok\":true") != std::string::npos &&
          observation.response.find(expected_retained_bundle_id) !=
              std::string::npos) {
        constexpr std::string_view kOwnerEpochPrefix = "\"owner_epoch\":\"";
        const auto owner_begin = observation.response.find(kOwnerEpochPrefix);
        if (owner_begin == std::string::npos) {
          throw std::runtime_error(
              "ready execution response has no owner epoch");
        }
        const auto value_begin = owner_begin + kOwnerEpochPrefix.size();
        const auto value_end = observation.response.find('"', value_begin);
        if (value_end == std::string::npos || value_end == value_begin) {
          throw std::runtime_error(
              "ready execution response has an invalid owner epoch");
        }
        observation.owner_epoch =
            observation.response.substr(value_begin, value_end - value_begin);
        return observation;
      }
    } catch (const std::exception &error) {
      observation.observed_closed = true;
      observation.response = std::string("transport_error:") + error.what();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::ifstream manager_log(directory.startup_stderr(), std::ios::binary);
  std::string manager_detail{std::istreambuf_iterator<char>(manager_log),
                             std::istreambuf_iterator<char>()};
  constexpr std::size_t kMaximumFailureDetailBytes = 16U * 1024U;
  if (manager_detail.size() > kMaximumFailureDetailBytes) {
    manager_detail.erase(
        0U, manager_detail.size() - kMaximumFailureDetailBytes);
  }
  throw std::runtime_error("execution owner did not reopen exact product: " +
                           observation.response +
                           (manager_detail.empty()
                                ? std::string{}
                                : std::string("; manager stderr: ") +
                                      manager_detail));
}

#endif

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
RuntimeOfferProjection WaitForRuntimeOfferProjection(
    const AtomicSourceSnapshotChannel &manager_source,
    const std::string_view expected_execution_owner_epoch,
    const std::chrono::seconds timeout = std::chrono::seconds(20)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      const auto snapshot = manager_source.Read();
      if (snapshot.has_value()) {
        const auto projection = ReadRuntimeOfferProjection(snapshot->payload);
        if (!projection.selected.empty() && !projection.observed.empty() &&
            projection.selected == projection.observed &&
            expected_execution_owner_epoch ==
                "kernel-owner-" + projection.observed_epoch) {
          return projection;
        }
      }
    } catch (const std::exception &) {
      // Publication may still be converging to the execution owner epoch.
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error(
      "Manager source did not publish the expected runtime offer projection");
}

pid_t WaitForSoleManagedChild(const pid_t manager,
                              const std::string_view observation_stage) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < deadline) {
    const auto children = ReadDirectChildren(manager);
    if (children.size() == 1U && ::kill(children.front(), 0) == 0) {
      return children.front();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Manager did not converge one child " +
                           std::string(observation_stage));
}
#endif

std::string SendCommand(const std::filesystem::path &path,
                        const std::string &request) {
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (client < 0) {
    throw std::runtime_error("Manager command client socket failed");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command path is too long");
  }
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  if (::connect(client, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client connect failed");
  }
  const auto sent =
      ::send(client, request.data(), request.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command send failed");
  }
  char probe{};
  ssize_t response_size = -1;
  do {
    response_size = ::recv(client, &probe, 1U, MSG_PEEK | MSG_TRUNC);
  } while (response_size < 0 && errno == EINTR);
  if (response_size <= 0 ||
      static_cast<std::size_t>(response_size) >
          core::kMaximumCellAdmissionJsonResponseBytes) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command response is invalid");
  }
  std::string response(static_cast<std::size_t>(response_size), '\0');
  ssize_t received = -1;
  do {
    received = ::recv(client, response.data(), response.size(), 0);
  } while (received < 0 && errno == EINTR);
  static_cast<void>(::close(client));
  if (received != response_size) {
    throw std::runtime_error("Manager command response changed during read");
  }
  return response;
}

void SendCommandAndDisconnect(const std::filesystem::path &path,
                              const std::string &request) {
  const int client = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  if (client < 0) {
    throw std::runtime_error("Manager command client socket failed");
  }
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = path.string();
  if (spelling.size() >= sizeof(address.sun_path)) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command path is too long");
  }
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  if (::connect(client, reinterpret_cast<sockaddr *>(&address),
                sizeof(address)) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client connect failed");
  }
  const auto sent =
      ::send(client, request.data(), request.size(), MSG_NOSIGNAL);
  if (sent < 0 || static_cast<std::size_t>(sent) != request.size()) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command send failed");
  }
  if (::shutdown(client, SHUT_RDWR) != 0) {
    static_cast<void>(::close(client));
    throw std::runtime_error("Manager command client shutdown failed");
  }
  static_cast<void>(::close(client));
}

std::string ReadStartupStderr(const DaemonDirectory &directory) {
  std::ifstream input(directory.startup_stderr(), std::ios::binary);
  return {std::istreambuf_iterator<char>(input),
          std::istreambuf_iterator<char>()};
}

struct AuthoringAdmissionProjection final {
  bool authoring_allowed{};
  std::uint64_t admission_generation{};
};

AuthoringAdmissionProjection
ReadAuthoringAdmissionProjection(const DaemonDirectory &directory) {
  const auto response = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v3","command":"cell_admission_get"})");
  const auto root = deployment::parse_strict_json_model_yaml(response).root();
  const auto *ok = std::get_if<bool>(&RequiredMember(root, "ok").storage());
  const auto &admission = RequiredMember(root, "authoring_admission");
  const auto *allowed = std::get_if<bool>(
      &RequiredMember(admission, "authoring_allowed").storage());
  const auto generation =
      RequiredMember(admission, "admission_generation").as_number();
  if (ok == nullptr || !*ok || allowed == nullptr || generation < 1.0 ||
      generation > 9007199254740991.0 ||
      static_cast<double>(static_cast<std::uint64_t>(generation)) !=
          generation) {
    throw std::runtime_error(
        "stable Kernel returned an invalid authoring-admission projection");
  }
  return {*allowed, static_cast<std::uint64_t>(generation)};
}

[[maybe_unused]] AuthoringAdmissionProjection WaitForAuthoringAdmission(
    const DaemonDirectory &directory,
    const std::chrono::seconds timeout = std::chrono::seconds(10)) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  std::optional<AuthoringAdmissionProjection> last;
  while (std::chrono::steady_clock::now() < deadline) {
    try {
      last = ReadAuthoringAdmissionProjection(directory);
      if (last->authoring_allowed) {
        return *last;
      }
    } catch (...) {
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  throw std::runtime_error("Kernel authoring admission did not become ready" +
                           (last.has_value()
                                ? std::string("; last generation=") +
                                      std::to_string(last->admission_generation)
                                : std::string{}));
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
/// Publish one prepared activation with its complete captured closure into
/// the daemon's trusted activation store, as the native preparer does.
void PublishPreparedActivationClosure(const DaemonDirectory &directory,
                                      const deployment::PreparedActivationInput &prepared) {
  // Publish the complete captured closure under the same writer lock used by
  // the native preparer. The Manager's collector may run concurrently.
  const deployment::ActivationContentWriterLock writing(directory.activation_root());
  const auto &closure = prepared.closure();
  std::vector<std::string_view> raw_values;
  for (const auto &artifact : closure.raw_artifacts)
    raw_values.emplace_back(artifact.bytes);
  deployment::FilesystemRawArtifactStore activation_raw(directory.activation_root());
  const auto raw_stored = activation_raw.put_all_if_absent(raw_values);
  for (std::size_t index = 0; index < raw_stored.size(); ++index) {
    const auto &expected = closure.raw_artifacts[index];
    if (raw_stored[index].content_addressed_store_key != expected.content_addressed_store_key ||
        raw_stored[index].content_digest != expected.content_digest ||
        raw_stored[index].byte_size != expected.bytes.size())
      throw std::runtime_error("prepared fixture raw artifact identity mismatch");
  }
  std::vector<deployment::CatalogArtifactPublication> catalog_values;
  for (const auto &artifact : closure.catalog_artifacts)
    catalog_values.push_back({artifact.content_addressed_store_key,
                              artifact.content_digest, artifact.bytes});
  deployment::FilesystemCatalogArtifactStore activation_catalogs(directory.activation_root());
  const auto catalog_stored = activation_catalogs.put_all_if_absent(catalog_values);
  for (std::size_t index = 0; index < catalog_stored.size(); ++index) {
    const auto &expected = closure.catalog_artifacts[index];
    if (catalog_stored[index].content_addressed_store_key != expected.content_addressed_store_key ||
        catalog_stored[index].content_digest != expected.content_digest ||
        catalog_stored[index].byte_size != expected.bytes.size())
      throw std::runtime_error("prepared fixture catalog artifact identity mismatch");
  }
  deployment::FilesystemRetainedBundleRepository activation_bundles(directory.activation_root());
  static_cast<void>(activation_bundles.put_if_absent(closure.retained_bundle));
  if (closure.program) {
    deployment::FilesystemProgramRepository programs(directory.activation_root());
    static_cast<void>(programs.put_if_absent(closure.program, *closure.retained_bundle));
  }
  if (closure.program_binding) {
    if (!closure.retained_model_map)
      throw std::logic_error("prepared fixture Binding has no retained model map");
    deployment::FilesystemProgramBindingRepository bindings(directory.activation_root());
    static_cast<void>(bindings.put_if_absent(closure.program_binding,
        *closure.retained_bundle, *closure.retained_model_map, activation_catalogs));
  }
  static_cast<void>(
      deployment::FilesystemPreparedActivationRepository(directory.activation_root())
          .put_if_absent(prepared));
}

/// Assemble and publish one real prepared activation for the exact candidate
/// this test applies, and return its Apply summary. The fixture Program is
/// re-verified against that candidate; if its roles do not resolve there the
/// value is honestly non-executable.
content_identity::CanonicalValue PublishTestPreparedActivation(
    const DaemonDirectory &directory,
    const deployment::RetainedBundle &candidate,
    const ManagerApplyFixtureMetadata &fixture) {
  const auto package_root = directory.apply_package_root();
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(package_root);
  const auto physical = bundles.get_verified(
      core::RetainedBundleId(fixture.physical_candidate_retained_bundle_id));
  if (physical == nullptr) {
    throw std::runtime_error("Manager Apply fixture lost its physical bundle");
  }
  const auto authored = deployment::FilesystemVerifiedProgramSource(package_root)
                            .get_verified(core::ProgramId(fixture.program_id),
                                          core::ProgramRevision(fixture.program_revision),
                                          *physical);
  if (authored == nullptr) {
    throw std::runtime_error("Manager Apply fixture Program is unavailable");
  }
  const auto &map_products =
      candidate.value().compiled_products.symbol_role_maps;
  if (map_products.size() > 1U) {
    throw std::runtime_error(
        "Manager Apply fixture candidate has multiple model maps");
  }
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  if (!map_products.empty()) {
    model_map =
        deployment::FilesystemVerifiedRetainedModelMapSource(package_root)
            .get_verified({candidate.retained_bundle_id(),
                           map_products.front().product_id});
  } else if (!candidate.value().cell_graph.controllable_components.empty() ||
             !candidate.value().cell_graph.model_instances.empty()) {
    throw std::runtime_error(
        "Manager Apply fixture candidate omits required model data");
  }
  deployment::FilesystemCatalogArtifactStore catalogs(package_root);
  const auto program = deployment::verify_program_binding_contract(
      authored->canonical_program_bytes(), candidate, model_map, catalogs);
  if (program == nullptr) {
    throw std::runtime_error("Manager Apply fixture Program is unavailable");
  }
  deployment::FilesystemRawArtifactStore raw_artifacts(package_root);
  const auto prepared = deployment::assemble_prepared_activation(
      {deployment::PreparedScenarioOrigin{
           "scenario.test",
           core::ContentDigest(
               "sha256:1414141414141414141414141414141414141414141414"
               "141414141414141414"),
           "/fixtures/scenario.test.scenario"},
       core::ContentDigest(
           "sha256:2020202020202020202020202020202020202020202020"
           "202020202020202020"),
       "optional"},
      candidate, model_map, *program, catalogs,
      raw_artifacts);
  PublishPreparedActivationClosure(directory, prepared);
  content_identity::CanonicalValue::Object summary;
  for (const auto &[name, value] : prepared.normalized_root().as_object()) {
    if (name != "activation_payload") summary.emplace_back(name, value);
  }
  return content_identity::CanonicalValue(std::move(summary));
}

/// Assemble and publish the prepared activation of the fixture's Close (the
/// Apply of the empty intent) applied without a Program, and return its id.
std::string PublishTestCloseWithoutProgram(
    const DaemonDirectory &directory,
    const ManagerApplyFixtureMetadata &fixture) {
  const auto package_root = directory.apply_package_root();
  const auto close =
      deployment::FilesystemVerifiedRetainedBundleSource(package_root)
          .get_verified(core::RetainedBundleId(fixture.close_retained_bundle_id));
  if (close == nullptr) {
    throw std::runtime_error("Manager Apply fixture lost its Close bundle");
  }
  const auto &map_products = close->value().compiled_products.symbol_role_maps;
  std::optional<deployment::VerifiedRetainedModelMap> model_map;
  if (!map_products.empty()) {
    model_map =
        deployment::FilesystemVerifiedRetainedModelMapSource(package_root)
            .get_verified({close->retained_bundle_id(),
                           map_products.front().product_id});
  }
  deployment::FilesystemRawArtifactStore raw_artifacts(package_root);
  const auto prepared = deployment::assemble_prepared_activation_without_program(
      {deployment::PreparedScenarioOrigin{
           "scenario.close",
           core::ContentDigest(
               "sha256:1515151515151515151515151515151515151515151515"
               "151515151515151515"),
           "/fixtures/scenario.close.scenario"},
       core::ContentDigest(
           "sha256:2121212121212121212121212121212121212121212121"
           "212121212121212121"),
       "optional"},
      *close, model_map, raw_artifacts);
  PublishPreparedActivationClosure(directory, prepared);
  return prepared.prepared_activation_id().value();
}
#endif

std::string ApplyRequest(
    const DaemonDirectory &directory,
    const std::string_view manager_owner_epoch,
    const std::uint64_t expected_admission_generation,
    const std::string_view deployment_artifact_id = "candidate",
    const std::string_view deployment_revision = kDeploymentRevision,
    const std::string_view expected_graph_revision = "current-graph",
    const std::string_view expected_retained_bundle_id = "current-bundle",
    const std::string_view request_id = "request-1",
    const std::string_view preview_owner_epoch = {},
    const bool preview_only = false,
    // "receipt" echoes the previewed impact digest (explicit acceptance), an
    // empty value accepts nothing, anything else is sent verbatim.
    const std::string_view accepted_impact = "receipt",
    const std::string_view prepared_activation_id_override = {}) {
  using content_identity::CanonicalValue;
  std::string resolved_graph_revision(expected_graph_revision);
  std::string resolved_retained_bundle_id(expected_retained_bundle_id);
  if (expected_graph_revision == "current-graph" ||
      expected_retained_bundle_id == "current-bundle") {
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    const auto current = manager_source.Read();
    if (!current.has_value()) {
      throw std::runtime_error(
          "Manager current projection is unavailable in test setup");
    }
    const auto &active = RequiredMember(current->payload, "active");
    if (expected_graph_revision == "current-graph") {
      resolved_graph_revision = RequiredText(active, "graph_revision");
    }
    if (expected_retained_bundle_id == "current-bundle") {
      resolved_retained_bundle_id = RequiredText(active, "retained_bundle_id");
    }
  }
  const auto owner =
      preview_owner_epoch.empty() ? manager_owner_epoch : preview_owner_epoch;
  const auto expected = CanonicalValue(CanonicalValue::Object{
      {"graph_revision", CanonicalValue(resolved_graph_revision)},
      {"retained_bundle_id", CanonicalValue(resolved_retained_bundle_id)},
  });
  // Negative preview tests intentionally have no candidate package (or have
  // displaced it). Use well-formed absent identities so the Manager, not
  // fixture lookup, reports the expected rejection.
  std::string candidate_bundle = "sha256:" + std::string(64U, '1');
  std::string candidate_graph = "sha256:" + std::string(64U, '2');
  // Negative previews never reach impact derivation; they keep a
  // well-formed absent identity.
  std::string prepared_activation_id =
      "sha256:1616161616161616161616161616161616161616161616161616161616161616";
  CanonicalValue prepared_summary(nullptr);
  if (!preview_only) {
    candidate_bundle.clear();
    candidate_graph.clear();
    const deployment::FilesystemVerifiedRetainedBundleSource bundles(
        directory.apply_package_root());
    const auto retained_root =
        directory.apply_package_root() / "retained-bundles" / "sha256";
    for (const auto &entry : std::filesystem::directory_iterator(retained_root)) {
      if (!entry.is_regular_file() || entry.path().extension() != ".json")
        continue;
      const auto bundle = bundles.get_verified(core::RetainedBundleId(
          "sha256:" + entry.path().stem().string()));
      if (bundle != nullptr &&
          bundle->value().deployment_artifact_id.value() ==
              deployment_artifact_id &&
          bundle->value().deployment_revision.value() == deployment_revision) {
        candidate_bundle = bundle->retained_bundle_id().value();
        candidate_graph = bundle->value().cell_graph.graph_revision.value();
        if (prepared_activation_id_override.empty()) {
#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
          prepared_summary = PublishTestPreparedActivation(
              directory, *bundle, ReadManagerApplyFixtureMetadata());
#else
          // The Manager Apply fixture (and its prepared activation) exists
          // only when the selected ROS execution host fixture is configured.
          throw std::logic_error(
              "publishing a prepared activation needs the selected ROS execution "
              "host test fixture");
#endif
          prepared_activation_id =
              RequiredText(prepared_summary, "prepared_activation_id");
        } else {
          prepared_activation_id = prepared_activation_id_override;
          const auto prepared =
              deployment::FilesystemPreparedActivationRepository(
                  directory.activation_root())
                  .get_verified(core::ContentDigest(prepared_activation_id));
          if (prepared == nullptr) {
            throw std::runtime_error(
                "Manager Apply fixture prepared activation is unavailable");
          }
          CanonicalValue::Object summary;
          for (const auto &[name, value] :
               prepared->normalized_root().as_object()) {
            if (name != "activation_payload") {
              summary.emplace_back(name, value);
            }
          }
          prepared_summary = CanonicalValue(std::move(summary));
        }
        break;
      }
    }
    if (candidate_bundle.empty() || candidate_graph.empty()) {
      throw std::runtime_error("Manager retained fixture has no matching candidate");
    }
  }
  const auto preview_request =
      content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
          {"api_version", CanonicalValue("manyforge.manager_command.v1")},
          {"command", CanonicalValue("deployment_candidate_preview")},
          {"candidate_graph_revision", CanonicalValue(candidate_graph)},
          {"candidate_retained_bundle_id", CanonicalValue(candidate_bundle)},
          {"deployment_artifact_id",
           CanonicalValue(std::string(deployment_artifact_id))},
          {"deployment_revision",
           CanonicalValue(std::string(deployment_revision))},
          {"expected_admission_generation",
           CanonicalValue(static_cast<double>(expected_admission_generation))},
          {"expected_current", expected},
          {"prepared_activation_id", CanonicalValue(prepared_activation_id)},
          {"request_owner_epoch", CanonicalValue(std::string(owner))},
      }));
  const auto preview_response =
      SendCommand(directory.socket_path(), preview_request);
  if (preview_only) {
    return preview_response;
  }
  const auto preview =
      deployment::parse_strict_json_model_yaml(preview_response).root();
  const auto *ok = std::get_if<bool>(&RequiredMember(preview, "ok").storage());
  if (ok == nullptr || !*ok) {
    throw std::runtime_error("Manager candidate preview failed in test setup: " +
                             preview_response);
  }
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"admission_context",
       CanonicalValue(CanonicalValue::Object{
           {"expected_admission_generation",
            CanonicalValue(static_cast<double>(expected_admission_generation))},
           {"kind", CanonicalValue("direct")},
       })},
      {"accepted_impact_digest",
       accepted_impact == "receipt"
           ? RequiredMember(RequiredMember(preview, "candidate_receipt"),
                            "impact_digest")
       : accepted_impact.empty()
           ? CanonicalValue(nullptr)
           : CanonicalValue(std::string(accepted_impact))},
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"candidate_receipt", RequiredMember(preview, "candidate_receipt")},
      {"command", CanonicalValue("apply")},
      {"correlation_id", CanonicalValue("correlation-1")},
      {"deployment_artifact_id",
       CanonicalValue(std::string(deployment_artifact_id))},
      {"deployment_revision", CanonicalValue(std::string(deployment_revision))},
      {"expected_current", expected},
      {"prepared_activation", prepared_summary},
      {"reject_if_affected_busy", CanonicalValue(true)},
      {"scene_initialization", CanonicalValue("preserve")},
      {"request_id", CanonicalValue(std::string(request_id))},
      {"request_owner_epoch", CanonicalValue(std::string(manager_owner_epoch))},
      {"source_label", CanonicalValue("manager-process-test")},
  }));
}

[[maybe_unused]] std::string
ApplyStatusRequest(const std::string_view submission_response) {
  using content_identity::CanonicalValue;
  const auto response =
      deployment::parse_strict_json_model_yaml(submission_response).root();
  const auto *apply_id =
      std::get_if<std::string>(&RequiredMember(response, "apply_id").storage());
  const auto generation =
      RequiredMember(response, "apply_generation").as_number();
  if (apply_id == nullptr || apply_id->empty() || generation < 1.0 ||
      generation > 9007199254740991.0 ||
      static_cast<double>(static_cast<std::uint64_t>(generation)) !=
          generation) {
    throw std::runtime_error("Manager Apply submission receipt is invalid");
  }
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string("manyforge.manager_command.v1"))},
      {"apply_generation", CanonicalValue(generation)},
      {"apply_id", CanonicalValue(*apply_id)},
      {"command", CanonicalValue(std::string("apply_status"))},
  }));
}

std::string SceneAddRequest(
    const SourceSnapshot &scene_source, const std::string_view request_id,
    const std::uint64_t expected_admission_generation, const double box_x = 0.1,
    const std::string_view scene_resource_id = "scene-process-box") {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  const auto &cell_id = RequiredMember(
      RequiredMember(scene_source.payload, "snapshot"), "cell_id");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_commit")},
      {"transaction",
       CanonicalValue(CanonicalValue::Object{
           {"attribution", CanonicalValue(CanonicalValue::Object{
                               {"assurance", CanonicalValue("unverified")},
                               {"principal_id", CanonicalValue(nullptr)},
                               {"transport", CanonicalValue("http")},
                           })},
           {"expected_admission_generation",
            CanonicalValue(static_cast<double>(expected_admission_generation))},
           {"expected_projection_revision", projection},
           {"operation",
            CanonicalValue(CanonicalValue::Object{
                {"frame_ref", CanonicalValue(CanonicalValue::Object{
                                  {"cell_id", cell_id},
                                  {"kind", CanonicalValue("cell_frame")},
                              })},
                {"kind", CanonicalValue("add_object")},
                {"pose",
                 CanonicalValue(CanonicalValue::Object{
                     {"orientation", CanonicalValue(CanonicalValue::Object{
                                         {"w", CanonicalValue(1.0)},
                                         {"x", CanonicalValue(0.0)},
                                         {"y", CanonicalValue(0.0)},
                                         {"z", CanonicalValue(0.0)},
                                     })},
                     {"position_m", CanonicalValue(CanonicalValue::Object{
                                        {"x_m", CanonicalValue(0.0)},
                                        {"y_m", CanonicalValue(0.0)},
                                        {"z_m", CanonicalValue(0.15)},
                                    })},
                 })},
                {"scene_resource_id",
                 CanonicalValue(std::string(scene_resource_id))},
                {"shape",
                 CanonicalValue(CanonicalValue::Object{
                     {"box_dimensions_m", CanonicalValue(CanonicalValue::Object{
                                              {"x_m", CanonicalValue(box_x)},
                                              {"y_m", CanonicalValue(0.2)},
                                              {"z_m", CanonicalValue(0.3)},
                                          })},
                     {"type", CanonicalValue("box")},
                 })},
                {"support_contacts", CanonicalValue(CanonicalValue::Array{})},
            })},
           {"program_reset_scene_authorization", CanonicalValue(nullptr)},
           {"request_context",
            CanonicalValue(CanonicalValue::Object{
                {"correlation_id", CanonicalValue(nullptr)},
                {"request_id", CanonicalValue(std::string(request_id))},
                {"request_owner_epoch",
                 CanonicalValue(scene_source.source_owner_epoch)},
                {"source_label", CanonicalValue("manager-process-test")},
            })},
       })},
  }));
}

std::string ScenePoseRequest(const SourceSnapshot &scene_source,
                             const std::string_view scene_resource_id) {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_pose_query")},
      {"expected_projection_revision", projection},
      {"scene_resource_ids", CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue(std::string(scene_resource_id)),
                             })},
  }));
}

// The active projection with its Scene revision advanced by one: a request
// naming a revision the owner has not reached.
std::string ScenePoseRequestAheadOfOwner(const SourceSnapshot &scene_source,
                                         const std::string_view scene_resource_id) {
  using content_identity::CanonicalValue;
  const auto &projection = RequiredMember(
      RequiredMember(scene_source.payload, "access"), "active_projection");
  const auto &scene = RequiredMember(projection, "scene_revision");
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_pose_query")},
      {"expected_projection_revision",
       CanonicalValue(CanonicalValue::Object{
           {"graph_revision", RequiredMember(projection, "graph_revision")},
           {"retained_bundle_id",
            RequiredMember(projection, "retained_bundle_id")},
           {"scene_revision",
            CanonicalValue(CanonicalValue::Object{
                {"content_hash", RequiredMember(scene, "content_hash")},
                {"revision_id",
                 CanonicalValue(
                     RequiredMember(scene, "revision_id").as_number() + 1.0)},
                {"scene_id", RequiredMember(scene, "scene_id")},
            })},
       })},
      {"scene_resource_ids", CanonicalValue(CanonicalValue::Array{
                                 CanonicalValue(std::string(scene_resource_id)),
                             })},
  }));
}

std::string
ScenePreviewRequest(const SourceSnapshot &scene_source,
                    const std::string_view request_id,
                    const std::uint64_t expected_admission_generation) {
  using content_identity::CanonicalValue;
  const auto transaction = deployment::parse_strict_json_model_yaml(
                               SceneAddRequest(scene_source, request_id,
                                               expected_admission_generation))
                               .root();
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"candidate_request",
       CanonicalValue(CanonicalValue::Object{
           {"frame_resolution", CanonicalValue(nullptr)},
           {"request", RequiredMember(transaction, "transaction")},
       })},
      {"command", CanonicalValue("scene_candidate_preview")},
  }));
}

const content_identity::CanonicalValue &
SceneRuntimeIdentityValue(const SourceSnapshot &scene_runtime_source) {
  return RequiredMember(RequiredMember(scene_runtime_source.payload, "state"),
                        "runtime_identity");
}

content_identity::CanonicalValue
DifferentSceneRuntimeIdentityValue(const SourceSnapshot &scene_runtime_source) {
  using content_identity::CanonicalValue;
  const auto &identity = SceneRuntimeIdentityValue(scene_runtime_source);
  return CanonicalValue(CanonicalValue::Object{
      {"deployment_generation",
       RequiredMember(identity, "deployment_generation")},
      {"graph_revision", RequiredMember(identity, "graph_revision")},
      {"program_generation", RequiredMember(identity, "program_generation")},
      {"retained_bundle_id", RequiredMember(identity, "retained_bundle_id")},
      {"runtime_generation",
       CanonicalValue(
           RequiredMember(identity, "runtime_generation").as_number() + 1.0)},
  });
}

std::string SceneRuntimeObserverOpenRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch,
    const std::string_view source_id = "isaac.observer",
    const std::string_view object_id = "robot",
    const std::string_view policy_id = "viewer.measured_robot",
    const bool eligible = false) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"bindings",
       CanonicalValue(CanonicalValue::Array{
           CanonicalValue(CanonicalValue::Object{
               {"eligible", CanonicalValue(eligible)},
               {"object_id", CanonicalValue(std::string(object_id))},
               {"policy_id", CanonicalValue(std::string(policy_id))},
           }),
       })},
      {"command", CanonicalValue("scene_runtime_observer_open")},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
  }));
}

std::string SceneRuntimeObserveRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch, const std::uint64_t source_sequence,
    const std::uint64_t lifetime_ns = 1000000000ULL,
    const double position = 0.25,
    const content_identity::CanonicalValue *expected_identity = nullptr,
    const std::string_view source_id = "isaac.observer",
    const std::string_view object_id = "robot",
    const std::string_view policy_id = "viewer.measured_robot",
    const bool eligible = false) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_observe")},
      {"eligible", CanonicalValue(eligible)},
      {"expected_runtime_identity",
       expected_identity == nullptr
           ? SceneRuntimeIdentityValue(scene_runtime_source)
           : *expected_identity},
      {"lifetime_ns", CanonicalValue(static_cast<double>(lifetime_ns))},
      {"object_id", CanonicalValue(std::string(object_id))},
      {"payload",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
           {"kind", CanonicalValue("joint_state")},
           {"positions", CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(position),
                         })},
       })},
      {"policy_id", CanonicalValue(std::string(policy_id))},
      {"payload_quality", CanonicalValue("good")},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
      {"source_sequence", CanonicalValue(static_cast<double>(source_sequence))},
  }));
}

std::string SceneRuntimePoseObserveRequest(
    const SourceSnapshot &scene_runtime_source,
    const std::string_view source_epoch, const std::uint64_t source_sequence,
    const std::uint64_t lifetime_ns = 1000000000ULL,
    const double position = 0.25,
    const std::string_view source_id = "isaac.planning-writer",
    const std::string_view object_id = "scene-process-box") {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_observe")},
      {"eligible", CanonicalValue(true)},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
      {"lifetime_ns", CanonicalValue(static_cast<double>(lifetime_ns))},
      {"object_id", CanonicalValue(std::string(object_id))},
      {"payload",
       CanonicalValue(CanonicalValue::Object{
           {"api_version", CanonicalValue("manyforge.ros_pose_observation.v2")},
           {"kind", CanonicalValue("simulation_entity_pose")},
           {"pose", CanonicalValue(CanonicalValue::Object{
                        {"frame_id", CanonicalValue("universe")},
                        {"frame_kind", CanonicalValue("cell_frame")},
                        {"orientation_xyzw",
                         CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(0.0), CanonicalValue(0.0),
                             CanonicalValue(0.0), CanonicalValue(1.0)})},
                        {"position_m",
                         CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(position), CanonicalValue(-0.5),
                             CanonicalValue(0.75)})},
                        {"source_stamp_ns", CanonicalValue(1250000000.0)},
                    })},
           {"source", CanonicalValue(CanonicalValue::Object{
                          {"entity", CanonicalValue("/box")}})},
       })},
      {"policy_id", CanonicalValue("scene")},
      {"payload_quality", CanonicalValue("good")},
      {"source_epoch", CanonicalValue(std::string(source_epoch))},
      {"source_id", CanonicalValue(std::string(source_id))},
      {"source_sequence", CanonicalValue(static_cast<double>(source_sequence))},
  }));
}

std::string SceneRuntimeBeforePlanningLatchRequest(
    const SourceSnapshot &scene_runtime_source,
    const SourceSnapshot &) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.manager_command.v1")},
      {"command", CanonicalValue("scene_runtime_latch_before_planning")},
      {"expected_runtime_identity",
       SceneRuntimeIdentityValue(scene_runtime_source)},
  }));
}

TEST(ManagerDaemonProcess,
     RejectsPreManifestRootWithTypedStartupErrorBeforeIngress) {
  DaemonDirectory legacy;
  const deployment::FilesystemCanonicalEmptyRetainedBundleSource source(
      MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT);
  const auto admission = source.load_verified();
  const auto legacy_journal =
      legacy.manager_root() /
      std::string(component_manager::kManagerBootstrapJournalFilename);
  {
    component_manager::SqliteManagerBootstrapJournal journal(
        legacy_journal, admission.cell_id());
  }

  ManagerProcess manager(legacy);
  const auto status = manager.Wait();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 70);
  const auto startup_error = ReadStartupStderr(legacy);
  EXPECT_NE(
      startup_error.find("\"code\":\"runtime_compatibility_manifest_missing\""),
      std::string::npos);
  EXPECT_NE(
      startup_error.find("\"required_action\":\"clean_canonical_bootstrap\""),
      std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(legacy.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(legacy.execution_socket_path()));
  EXPECT_FALSE(std::filesystem::exists(legacy.publication_root() /
                                       "manager-source.json"));
  EXPECT_TRUE(std::filesystem::exists(legacy_journal));
  EXPECT_FALSE(std::filesystem::exists(
      legacy.manager_root() /
      std::string(component_manager::kManagerStateMarkerFilename)));

  DaemonDirectory clean;
  ManagerProcess supported(clean);
  supported.WaitReady(clean);
  EXPECT_TRUE(std::filesystem::exists(
      clean.manager_root() /
      std::string(component_manager::kManagerStateMarkerFilename)));
}

TEST(ManagerDaemonProcess,
     RejectsDisposableRunRootNestedInsideDurableProjectionRoot) {
  DaemonDirectory directory;
  ManagerProcess manager(
      directory, MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
      "canonical-empty-admission.json", MANYFORGE_HEADLESS_TEST_MANAGER_DAEMON,
      {}, directory.publication_root().parent_path().string());
  const auto status = manager.Wait();
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_NE(WEXITSTATUS(status), 0);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "must be disjoint from disposable run storage"),
            std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(directory.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(directory.execution_socket_path()));
}

TEST(ManagerDaemonProcess, StartupReplacesTornStatusFiles) {
  // The status files are written without fsync; a power loss can leave one
  // empty, truncated or garbage. Start-up replaces each and starts.
  DaemonDirectory directory;
  const auto root = directory.publication_root();
  { std::ofstream empty(root / "manager-source.json"); }
  {
    AtomicSourceSnapshotChannel previous(root, "logical-scene-source.json");
    static_cast<void>(previous.Publish(
        "logical_scene", "logical-scene.previous", "epoch-previous", 1,
        content_identity::CanonicalValue(
            content_identity::CanonicalValue::Object{})));
    const auto path = root / "logical-scene-source.json";
    std::filesystem::resize_file(path, std::filesystem::file_size(path) / 2U);
  }
  for (const auto *name :
       {"scene-runtime-source.json", "viewer-pose-sessions.json"}) {
    std::ofstream garbage(root / name, std::ios::binary);
    garbage << "{\"api_version\":\"manyforge.source_snapshot.v1\",\"pay";
  }

  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  const std::pair<const char *, const char *> rebuilt[]{
      {"manager-source.json", "component_manager"},
      {"logical-scene-source.json", "logical_scene"},
      {"scene-runtime-source.json", "scene_runtime"}};
  for (const auto &[name, owner_kind] : rebuilt) {
    const auto snapshot = AtomicSourceSnapshotChannel(root, name).Read();
    ASSERT_TRUE(snapshot.has_value()) << name;
    EXPECT_EQ(snapshot->source_owner_kind, owner_kind);
  }
  EXPECT_NO_THROW(static_cast<void>(
      AtomicSourceSnapshotChannel(root, "viewer-pose-sessions.json").Read()));
}

TEST(ManagerDaemonProcess, ProcessStopDecoderPreservesActualMultilineLaunchIdentity) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(), "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before);
  const std::string launches[]{
      "mock\nlite6_robot\nlite6_lite_gripper\n/manyforge/lite6_robot\nlite6_traj_controller\nlite_gripper_controller\n\nsha256:948e121935a4e7d66ca927d1945438e48332cce2bd820f60488fc049bb18d4b1\nsha256:61f01507a85d3a8d6539b8488d9a525dbaf115dca33338dc0d1702168d3b34a4\n",
      "mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n"};
  EXPECT_EQ(launches[0].size(), 250U);
  EXPECT_EQ(launches[1].size(), 257U);
  const auto base = deployment::parse_strict_json_model_yaml(
      R"({"request_context":{"request_id":"close.stop.1","request_owner_epoch":"caller.1","source_label":"composer"},"predecessor_projection_revision":{"retained_bundle_id":"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","graph_revision":"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","scene_revision":{"scene_id":"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee","revision_id":1,"content_hash":"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"}},"expected_predecessor_publication_generation":1,"expected_predecessor_public_pointer_digest":"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","runtime_unit_id":"host.1","runtime_epoch":"host.epoch.1","manager_runtime_epoch":"foreign.manager.epoch","controllable_component_id":"placeholder","launch_identity":"placeholder","leader_pid":42})").root();
  const auto exchange = [&](const std::string &command, const std::string &launch,
                            const std::string &component, const std::string &runtime_unit) {
    auto fields = base.as_object();
    for (auto &[key, value] : fields) {
      if (key == "launch_identity") value = CanonicalValue(launch);
      if (key == "controllable_component_id") value = CanonicalValue(component);
      if (key == "runtime_unit_id") value = CanonicalValue(runtime_unit);
    }
    const auto packet = content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.manager_command.v1")},
        {"command", CanonicalValue(command)}, {"request", CanonicalValue(std::move(fields))}}));
    return deployment::parse_strict_json_model_yaml(SendCommand(directory.socket_path(), packet)).root();
  };
  for (std::size_t i = 0U; i < 2U; ++i) {
    const std::string component = i == 0U ? "lite6_robot" : "ur5e_robot";
    for (const auto *command : {"stop_component_process", "component_process_stop_result"}) {
      const auto response = exchange(command, launches[i], component, "host.1");
      EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(false));
      const auto &error = RequiredMember(response, "error");
      EXPECT_EQ(RequiredText(error, "code"), "manager_command_rejected");
      // A refusal after decoding names its producer.
      EXPECT_EQ(RequiredText(error, "producer"), "component_manager");
      // Reaching the original Manager occurrence guard proves complete decoding;
      // the foreign Manager epoch prevents forwarding or any Stop effect.
      EXPECT_EQ(RequiredText(error, "message"),
                "Manager daemon: component process Stop names another Manager occurrence");
    }
  }
  for (const auto &launch : {std::string(), launches[1]}) {
    const auto response = exchange("stop_component_process", launch, "ur5e_robot",
                                   launch.empty() ? "host.1" : std::string(257U, 'x'));
    EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(false));
    const auto message = RequiredText(RequiredMember(response, "error"), "message");
    EXPECT_NE(message.find(launch.empty() ? "command field launch_identity" : "command field runtime_unit_id"),
              std::string::npos);
  }
  const auto component_rejected = exchange("stop_component_process", launches[1], std::string(129U, 'x'), "host.1");
  EXPECT_NE(RequiredText(RequiredMember(component_rejected, "error"), "message").find(
      "component process Stop request is invalid"), std::string::npos);
  const auto after = manager_source.Read();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->source_owner_epoch, before->source_owner_epoch);
  EXPECT_EQ(RequiredMember(after->payload, "active"), RequiredMember(before->payload, "active"));
}

TEST(ManagerDaemonProcess, ProcessStopOfferReturnsKnownEmptyInventoryAndIsPassive) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(), "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before);
  const auto bytes = SendCommand(directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":"component.1"})");
  const auto response = deployment::parse_strict_json_model_yaml(bytes).root();
  ASSERT_EQ(response.as_object().size(), 4U);
  EXPECT_EQ(RequiredMember(response, "api_version"), CanonicalValue("manyforge.manager_command_response.v2"));
  EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(true)) << bytes;
  EXPECT_EQ(RequiredMember(response, "error"), CanonicalValue(nullptr));
  const auto &offer = RequiredMember(response, "result");
  ASSERT_EQ(offer.as_object().size(), 2U);
  EXPECT_EQ(RequiredMember(offer, "controllable_component_id"), CanonicalValue("component.1"));
  EXPECT_TRUE(RequiredMember(offer, "process_inventories").as_array().empty());
  for (const auto* request : {
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer"})",
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":false})",
      R"({"api_version":"manyforge.manager_command.v1","command":"component_process_stop_offer","controllable_component_id":"component.1","extra":true})"}) {
    const auto rejected_bytes = SendCommand(directory.socket_path(), request);
    const auto rejected = deployment::parse_strict_json_model_yaml(rejected_bytes).root();
    EXPECT_EQ(RequiredMember(rejected, "ok"), CanonicalValue(false)) << rejected_bytes;
    // A command that does not decode is one request-validation refusal.
    const auto &error = RequiredMember(rejected, "error");
    EXPECT_EQ(RequiredMember(error, "code"), CanonicalValue("invalid_request")) << rejected_bytes;
    EXPECT_EQ(RequiredMember(RequiredMember(error, "details"), "command"),
              CanonicalValue("invalid")) << rejected_bytes;
    EXPECT_EQ(RequiredMember(error, "producer"), CanonicalValue("component_manager"))
        << rejected_bytes;
  }
  const auto after = manager_source.Read();
  ASSERT_TRUE(after);
  EXPECT_EQ(after->source_owner_epoch, before->source_owner_epoch);
  EXPECT_EQ(RequiredMember(after->payload, "active"), RequiredMember(before->payload, "active"));
}

TEST(ManagerDaemonProcess, ApplyAcknowledgementRequiresAnExactRequestContext) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  const auto valid = SendCommand(directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","apply_generation":1,"apply_id":"apply.absent","command":"apply_acknowledge","request_context":{"request_id":"ack.1","source_label":"composer"}})");
  const auto accepted_shape = deployment::parse_strict_json_model_yaml(valid).root();
  EXPECT_EQ(RequiredMember(RequiredMember(accepted_shape, "error"), "code"),
            CanonicalValue("apply_status_not_found")) << valid;
  EXPECT_EQ(RequiredMember(RequiredMember(RequiredMember(accepted_shape, "error"), "details"), "effect"),
            CanonicalValue("none")) << valid;
  for (const auto *request : {
      R"({"api_version":"manyforge.manager_command.v1","apply_generation":1,"apply_id":"apply.absent","command":"apply_acknowledge"})",
      R"({"api_version":"manyforge.manager_command.v1","apply_generation":1,"apply_id":"apply.absent","command":"apply_acknowledge","extra":true,"request_context":{"request_id":"ack.1","source_label":"composer"}})",
      R"({"api_version":"manyforge.manager_command.v1","apply_generation":1,"apply_id":"apply.absent","command":"apply_acknowledge","request_context":{"extra":true,"request_id":"ack.1","source_label":"composer"}})"}) {
    const auto bytes = SendCommand(directory.socket_path(), request);
    const auto rejected = deployment::parse_strict_json_model_yaml(bytes).root();
    EXPECT_EQ(RequiredMember(RequiredMember(rejected, "error"), "code"),
              CanonicalValue("invalid_request")) << bytes;
    EXPECT_EQ(RequiredMember(RequiredMember(RequiredMember(rejected, "error"), "details"), "effect"),
              CanonicalValue("none")) << bytes;
  }
}

// New's release finds no failed Apply holding a fresh cell; the command
// takes only its request context.
TEST(ManagerDaemonProcess, ApplyReleaseForNewReleasesNothingOnAFreshCell) {
  using content_identity::CanonicalValue;
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  const auto valid = SendCommand(directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"apply_release_for_new","request_context":{"request_id":"new.1","source_label":"composer"}})");
  const auto released = deployment::parse_strict_json_model_yaml(valid).root();
  EXPECT_EQ(RequiredMember(released, "ok"), CanonicalValue(true)) << valid;
  EXPECT_EQ(RequiredMember(released, "released"), CanonicalValue(nullptr)) << valid;
  for (const auto *request : {
      R"({"api_version":"manyforge.manager_command.v1","command":"apply_release_for_new"})",
      R"({"api_version":"manyforge.manager_command.v1","apply_generation":1,"apply_id":"apply.absent","command":"apply_release_for_new","request_context":{"request_id":"new.1","source_label":"composer"}})"}) {
    const auto bytes = SendCommand(directory.socket_path(), request);
    const auto rejected = deployment::parse_strict_json_model_yaml(bytes).root();
    EXPECT_EQ(RequiredMember(RequiredMember(rejected, "error"), "code"),
              CanonicalValue("invalid_request")) << bytes;
  }
}

TEST(ManagerDaemonProcess,
     PublishesWithoutComposerAndSurvivesManagerReplacement) {
  DaemonDirectory directory;
  std::string first_manager_epoch;
  std::uint64_t first_manager_generation = 0U;
  std::string first_scene_epoch;
  std::string first_scene_digest;
  std::uint64_t first_scene_generation = 0U;
  std::string first_scene_runtime_epoch;
  std::uint64_t first_scene_runtime_generation = 0U;
  std::string unavailable_response;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto manager_snapshot = manager_source.Read();
    const auto scene_snapshot = scene_source.Read();
    const auto scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(manager_snapshot.has_value());
    ASSERT_TRUE(scene_snapshot.has_value());
    ASSERT_TRUE(scene_runtime_snapshot.has_value());
    EXPECT_EQ(manager_snapshot->source_owner_kind, "component_manager");
    EXPECT_EQ(scene_snapshot->source_owner_kind, "logical_scene");
    EXPECT_EQ(scene_runtime_snapshot->source_owner_kind, "scene_runtime");
    const auto manager_payload_bytes =
        content_identity::canonical_json(manager_snapshot->payload);
    EXPECT_EQ(manager_payload_bytes.find("node_environment"),
              std::string::npos);
    first_manager_epoch = manager_snapshot->source_owner_epoch;
    first_manager_generation = manager_snapshot->source_generation;
    first_scene_epoch = scene_snapshot->source_owner_epoch;
    first_scene_digest = scene_snapshot->semantic_content_digest;
    first_scene_generation = scene_snapshot->source_generation;
    first_scene_runtime_epoch = scene_runtime_snapshot->source_owner_epoch;
    first_scene_runtime_generation = scene_runtime_snapshot->source_generation;

    unavailable_response = ApplyRequest(
        directory, first_manager_epoch, 1U, "candidate", kDeploymentRevision,
        "current-graph", "current-bundle", "request-1", {}, true);
    EXPECT_EQ(ApplyRequest(directory, first_manager_epoch, 1U, "candidate",
                           kDeploymentRevision, "current-graph",
                           "current-bundle", "request-1", {}, true),
              unavailable_response);
    EXPECT_NE(unavailable_response.find("\"deployment_candidate_rejected\""),
              std::string::npos);
    EXPECT_EQ(unavailable_response.find("\"state\":\"accepted\""),
              std::string::npos);

    SendCommandAndDisconnect(
        directory.socket_path(),
        "{\"api_version\":\"manyforge.manager_command.v1\","
        "\"command\":\"reconcile\"}");
    const auto post_disconnect_response =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"reconcile\"}");
    EXPECT_NE(post_disconnect_response.find("\"ok\":true"), std::string::npos);
    EXPECT_EQ(::kill(manager.pid(), 0), 0);

    component_manager::SqliteManagerApplyJournal apply_journal(
        directory.manager_root() /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId("c1_build_authority_cell"));
    EXPECT_TRUE(apply_journal.ReadStatusInventory().empty());

    std::atomic<bool> read_failed{false};
    std::vector<std::thread> readers;
    for (int index = 0; index < 4; ++index) {
      readers.emplace_back([&] {
        try {
          AtomicSourceSnapshotChannel reader(directory.publication_root(),
                                             "manager-source.json");
          for (int read = 0; read < 100; ++read) {
            const auto snapshot = reader.Read();
            if (!snapshot.has_value() || !snapshot->valid()) {
              read_failed.store(true);
              return;
            }
          }
        } catch (...) {
          read_failed.store(true);
        }
      });
    }
    for (int reconcile = 0; reconcile < 20; ++reconcile) {
      const auto response =
          SendCommand(directory.socket_path(),
                      "{\"api_version\":\"manyforge.manager_command.v1\","
                      "\"command\":\"reconcile\"}");
      EXPECT_NE(response.find("\"ok\":true"), std::string::npos);
    }
    for (auto &reader : readers) {
      reader.join();
    }
    EXPECT_FALSE(read_failed.load());
    const auto reconciled_manager_snapshot = manager_source.Read();
    const auto reconciled_scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(reconciled_manager_snapshot.has_value());
    ASSERT_TRUE(reconciled_scene_runtime_snapshot.has_value());
    EXPECT_EQ(reconciled_manager_snapshot->source_generation,
              first_manager_generation);
    EXPECT_EQ(reconciled_scene_runtime_snapshot->source_generation,
              first_scene_runtime_generation);

    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
    EXPECT_EQ(WTERMSIG(status), SIGKILL);
  }

  {
    ManagerProcess replacement(directory);
    replacement.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto manager_snapshot = manager_source.Read();
    const auto scene_snapshot = scene_source.Read();
    const auto scene_runtime_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(manager_snapshot.has_value());
    ASSERT_TRUE(scene_snapshot.has_value());
    ASSERT_TRUE(scene_runtime_snapshot.has_value());
    EXPECT_NE(manager_snapshot->source_owner_epoch, first_manager_epoch);
    EXPECT_EQ(manager_snapshot->source_generation, 1U);
    EXPECT_EQ(scene_snapshot->source_owner_epoch, first_scene_epoch);
    EXPECT_EQ(scene_snapshot->semantic_content_digest, first_scene_digest);
    EXPECT_EQ(scene_snapshot->source_generation, first_scene_generation);
    EXPECT_NE(scene_runtime_snapshot->source_owner_epoch,
              first_scene_runtime_epoch);
    EXPECT_EQ(scene_runtime_snapshot->source_generation, 1U);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_snapshot->payload)
                  .find("\"state\":\"open\""),
              std::string::npos);
    const auto stale_apply = ApplyRequest(
        directory, first_manager_epoch, 1U, "candidate", kDeploymentRevision,
        "current-graph", "current-bundle", "request-1", {}, true);
    EXPECT_NE(stale_apply.find("\"stale_request_owner_epoch\""),
              std::string::npos);
    const auto stale_generation = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*scene_snapshot, "replacement-stale-generation.1", 2U));
    EXPECT_NE(stale_generation.find("\"authoring_not_ready\""),
              std::string::npos);
    const auto replacement_unavailable =
        ApplyRequest(directory, manager_snapshot->source_owner_epoch, 1U,
                     "candidate", kDeploymentRevision, "current-graph",
                     "current-bundle", "request-1", {}, true);
    EXPECT_NE(replacement_unavailable.find("\"deployment_candidate_rejected\""),
              std::string::npos);

    const auto shutdown =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"shutdown_runtime\"}");
    EXPECT_NE(shutdown.find("\"ok\":true"), std::string::npos);
    const auto status = replacement.Wait();
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
  }
}

TEST(ManagerDaemonProcess,
     PeriodicOwnerPublicationIsNotStarvedByCommandTraffic) {
  DaemonDirectory directory;
  ManagerProcess manager(directory);
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto before = scene_source.ReadObserved();
  ASSERT_TRUE(before.has_value());

  std::atomic<bool> stop{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> senders;
  for (int index = 0; index < 4; ++index) {
    senders.emplace_back([&] {
      try {
        while (!stop.load()) {
          const auto response =
              SendCommand(directory.socket_path(),
                          "{\"api_version\":\"manyforge.manager_command.v1\","
                          "\"command\":\"scene_owner_read\"}");
          if (response.find("\"ok\":true") == std::string::npos) {
            failed.store(true);
            return;
          }
        }
      } catch (...) {
        failed.store(true);
      }
    });
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  stop.store(true);
  for (auto &sender : senders) {
    sender.join();
  }

  const auto after = scene_source.ReadObserved();
  ASSERT_TRUE(after.has_value());
  EXPECT_FALSE(failed.load());
  EXPECT_GT(after->file_modified_at_unix_ns, before->file_modified_at_unix_ns);
}

TEST(ManagerDaemonProcess,
     SceneRuntimeObservationIngressFencesIdentityReplacementAndFreshness) {
  DaemonDirectory directory;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel viewer_sessions(
        directory.publication_root(), "viewer-pose-sessions.json");
    const auto initial = scene_runtime_source.Read();
    const auto initial_scene = scene_source.Read();
    ASSERT_TRUE(initial.has_value());
    ASSERT_TRUE(initial_scene.has_value());
    const auto initial_sessions = viewer_sessions.Read();
    ASSERT_TRUE(initial_sessions.has_value());
    EXPECT_EQ(initial_sessions->source_owner_id, initial->source_owner_id);
    EXPECT_EQ(initial_sessions->source_owner_epoch, initial->source_owner_epoch);
    EXPECT_EQ(RequiredMember(initial_sessions->payload, "runtime_identity"),
              SceneRuntimeIdentityValue(*initial));
    EXPECT_TRUE(RequiredMember(initial_sessions->payload, "sessions").as_array().empty());

    const auto owner_read = SendCommand(
        directory.socket_path(),
        R"({"api_version":"manyforge.manager_command.v1","command":"scene_runtime_owner_read"})");
    EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(owner_read.find("\"scene_runtime_owner_epoch\":"),
              std::string::npos);
    EXPECT_NE(owner_read.find("\"initialization_state\":\"open\""),
              std::string::npos);
    EXPECT_NE(owner_read.find("\"runtime_identity\":"), std::string::npos);
    EXPECT_EQ(owner_read.find("\"pose_sources\":"), std::string::npos);

    auto between_request =
        deployment::parse_strict_json_model_yaml(
            SceneRuntimeBeforePlanningLatchRequest(*initial, *initial_scene))
            .root();
    auto between_fields = between_request.as_object();
    for (auto &[name, value] : between_fields)
      if (name == "command")
        value = content_identity::CanonicalValue(
            "scene_runtime_latch_between_nodes");
    std::erase_if(between_fields, [](const auto &entry) {
      return entry.first == "expected_projection_revision";
    });
    const auto between =
        SendCommand(directory.socket_path(),
                    content_identity::canonical_json(
                        content_identity::CanonicalValue(between_fields)));
    EXPECT_NE(between.find("\"ok\":true"), std::string::npos) << between;
    EXPECT_NE(between.find("\"latched_observation_count\":0"),
              std::string::npos)
        << between;
    between_fields.emplace_back(
        "scene_resource_ids",
        content_identity::CanonicalValue(
            content_identity::CanonicalValue::Array{
                content_identity::CanonicalValue("scene_object")}));
    const auto scoped =
        SendCommand(directory.socket_path(),
                    content_identity::canonical_json(
                        content_identity::CanonicalValue(between_fields)));
    EXPECT_NE(scoped.find("\"ok\":true"), std::string::npos) << scoped;
    EXPECT_NE(scoped.find("\"latched_observation_count\":0"), std::string::npos)
        << scoped;

    auto crossed_between = between_fields;
    for (auto &[name, value] : crossed_between)
      if (name == "expected_runtime_identity")
        value = DifferentSceneRuntimeIdentityValue(*initial);
    const auto crossed_latch =
        SendCommand(directory.socket_path(),
                    content_identity::canonical_json(
                        content_identity::CanonicalValue(crossed_between)));
    EXPECT_NE(crossed_latch.find("scene_runtime_identity_changed"),
              std::string::npos)
        << crossed_latch;

    const auto viewer_path = directory.publication_root() / "viewer-pose-sessions.json";
    const auto saved_viewer_path = directory.publication_root() / "viewer-pose-sessions.saved";
    std::filesystem::rename(viewer_path, saved_viewer_path);
    std::filesystem::create_directory(viewer_path);
    const auto opened_first = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserverOpenRequest(*initial, "observer-epoch-1"));
    std::filesystem::remove(viewer_path);
    std::filesystem::rename(saved_viewer_path, viewer_path);
    EXPECT_NE(opened_first.find("\"source_epoch_order\":1"), std::string::npos)
        << opened_first;
    const auto retry_open = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserverOpenRequest(*initial, "observer-epoch-1"));
    EXPECT_NE(retry_open.find("\"source_epoch_order\":1"), std::string::npos)
        << retry_open;
    const auto opened_sessions = viewer_sessions.Read();
    ASSERT_TRUE(opened_sessions.has_value());
    const auto &opened_rows = RequiredMember(opened_sessions->payload, "sessions").as_array();
    ASSERT_EQ(opened_rows.size(), 1U);
    EXPECT_EQ(RequiredMember(opened_rows.front(), "source_id").as_string(), "isaac.observer");
    EXPECT_EQ(RequiredMember(opened_rows.front(), "source_epoch").as_string(), "observer-epoch-1");
    EXPECT_EQ(RequiredMember(opened_rows.front(), "source_epoch_order").as_number(), 1.0);
    // Eligible object bindings observe as "scene"; the application policy
    // is the Deployment link row only.
    const auto policy_named_binding =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "observer-epoch-1", "isaac.observer",
                        "scene_object", "continuous_unattached", true));
    EXPECT_NE(policy_named_binding.find("invalid_request"),
              std::string::npos)
        << policy_named_binding;
    const auto changed_registration =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "observer-epoch-1", "isaac.observer",
                        "scene_object", "scene", true));
    EXPECT_NE(
        changed_registration.find("scene_runtime_observer_request_conflict"),
        std::string::npos)
        << changed_registration;
    const auto monotonic_before_observation =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    const auto accepted = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U));
    const auto monotonic_after_observation =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    EXPECT_NE(accepted.find("\"disposition\":\"observed_only\""),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"fresh\":true"), std::string::npos) << accepted;
    EXPECT_NE(accepted.find("\"scene_application_status\":\"not_requested\""),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_projection_revision\":null"),
              std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_request_id\":null"), std::string::npos)
        << accepted;
    EXPECT_NE(accepted.find("\"scene_update_status\":null"), std::string::npos)
        << accepted;
    const auto accepted_snapshot = scene_runtime_source.Read();
    ASSERT_TRUE(accepted_snapshot.has_value());
    const auto &accepted_pose_sources =
        RequiredMember(RequiredMember(accepted_snapshot->payload, "state"),
                       "pose_sources")
            .as_array();
    ASSERT_EQ(accepted_pose_sources.size(), 1U);
    const auto observed_at_ns =
        RequiredMember(accepted_pose_sources.front(), "observed_at_ns")
            .as_number();
    EXPECT_GE(observed_at_ns,
              static_cast<double>(monotonic_before_observation));
    EXPECT_LE(observed_at_ns, static_cast<double>(monotonic_after_observation));
    const auto duplicate = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U));
    EXPECT_EQ(duplicate, accepted);
    const auto changed_intent =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 1U,
                                               1000000000ULL, 0.5));
    EXPECT_NE(changed_intent.find("scene_runtime_observation_request_conflict"),
              std::string::npos)
        << changed_intent;
    const auto wrong_identity = DifferentSceneRuntimeIdentityValue(*initial);
    const auto crossed_runtime = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 2U,
                                   1000000000ULL, 0.25, &wrong_identity));
    EXPECT_NE(crossed_runtime.find("scene_runtime_identity_changed"),
              std::string::npos)
        << crossed_runtime;

    const auto writable_open =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "writer-epoch-1", "isaac.writer.a",
                        "scene_object", "scene", true));
    EXPECT_NE(writable_open.find("\"ok\":true"), std::string::npos)
        << writable_open;
    const auto conflicting_writer =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "writer-epoch-1", "isaac.writer.b",
                        "scene_object", "scene", true));
    EXPECT_NE(conflicting_writer.find("scene_runtime_pose_writer_conflict"),
              std::string::npos)
        << conflicting_writer;
    const auto undeclared_observation =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserveRequest(*initial, "writer-epoch-1", 1U,
                                               1000000000ULL, 0.25, nullptr,
                                               "isaac.writer.a", "scene_object",
                                               "before_planning", true));
    EXPECT_NE(
        undeclared_observation.find("scene_runtime_observation_not_declared"),
        std::string::npos)
        << undeclared_observation;

    const auto before_planning_open =
        SendCommand(directory.socket_path(),
                    SceneRuntimeObserverOpenRequest(
                        *initial, "planner-epoch-1", "isaac.planning-writer",
                        "scene-process-box", "scene", true));
    EXPECT_NE(before_planning_open.find("\"ok\":true"), std::string::npos)
        << before_planning_open;
    // The canonical-empty bundle has no object link row for this binding, so
    // it is never applied or latched (no fallback to the binding policy_id).
    const auto never_seen_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *initial_scene));
    EXPECT_NE(never_seen_latch.find("\"latched_observation_count\":0"),
              std::string::npos)
        << never_seen_latch;
    const auto added_object = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial_scene, "scene-runtime-planning-object.1", 1U));
    EXPECT_NE(added_object.find("\"authoring_not_ready\""), std::string::npos)
        << added_object;
    const auto scene_before_latch = scene_source.Read();
    ASSERT_TRUE(scene_before_latch.has_value());
    EXPECT_EQ(scene_before_latch->source_generation,
              initial_scene->source_generation);
    const auto before_planning = SendCommand(
        directory.socket_path(),
        SceneRuntimePoseObserveRequest(*initial, "planner-epoch-1", 1U));
    EXPECT_NE(before_planning.find(
                  "\"scene_application_status\":\"not_requested\""),
              std::string::npos)
        << before_planning;
    EXPECT_NE(before_planning.find("\"scene_projection_revision\":null"),
              std::string::npos)
        << before_planning;
    const auto latched = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_before_latch));
    EXPECT_NE(latched.find("\"latched_observation_count\":0"),
              std::string::npos)
        << latched;
    const auto scene_after_latch = scene_source.Read();
    ASSERT_TRUE(scene_after_latch.has_value());
    const auto observed_pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*scene_after_latch, "scene-process-box"));
    EXPECT_NE(observed_pose.find("\"object_not_found\""), std::string::npos)
        << observed_pose;
    const auto repeated_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_after_latch));
    EXPECT_NE(repeated_latch.find("\"latched_observation_count\":0"),
              std::string::npos)
        << repeated_latch;
    const auto advanced_scene =
        SendCommand(directory.socket_path(),
                    SceneAddRequest(*scene_after_latch, "scene-runtime-fence.1",
                                    1U, 0.2, "scene-process-fence-box"));
    EXPECT_NE(advanced_scene.find("\"authoring_not_ready\""), std::string::npos)
        << advanced_scene;
    const auto scene_after_advance = scene_source.Read();
    ASSERT_TRUE(scene_after_advance.has_value());
    EXPECT_EQ(scene_after_advance->source_generation,
              initial_scene->source_generation);
    const auto expiring = SendCommand(
        directory.socket_path(),
        SceneRuntimePoseObserveRequest(*initial, "planner-epoch-1", 2U, 1U));
    EXPECT_NE(expiring.find("\"scene_application_status\":\"not_requested\""),
              std::string::npos)
        << expiring;
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    const auto stale_observation_latch = SendCommand(
        directory.socket_path(),
        SceneRuntimeBeforePlanningLatchRequest(*initial, *scene_after_advance));
    EXPECT_NE(
        stale_observation_latch.find("\"latched_observation_count\":0"),
        std::string::npos)
        << stale_observation_latch;

    const auto observed = scene_runtime_source.Read();
    ASSERT_TRUE(observed.has_value());
    const auto observed_bytes =
        content_identity::canonical_json(observed->payload);
    EXPECT_NE(observed_bytes.find("\"source_id\":\"isaac.observer\""),
              std::string::npos);
    EXPECT_NE(observed_bytes.find("\"kind\":\"joint_state\""),
              std::string::npos);

    const auto opened_replacement = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserverOpenRequest(*initial, "observer-epoch-2"));
    EXPECT_NE(opened_replacement.find("\"source_epoch_order\":2"),
              std::string::npos)
        << opened_replacement;
    const auto replaced_sessions = viewer_sessions.Read();
    ASSERT_TRUE(replaced_sessions.has_value());
    const auto &replaced_rows = RequiredMember(replaced_sessions->payload, "sessions").as_array();
    const auto replaced = std::find_if(replaced_rows.begin(), replaced_rows.end(),
        [](const auto &row) {
          return RequiredMember(row, "source_id").as_string() == "isaac.observer";
        });
    ASSERT_NE(replaced, replaced_rows.end());
    EXPECT_EQ(RequiredMember(*replaced, "source_epoch").as_string(), "observer-epoch-2");
    EXPECT_EQ(RequiredMember(*replaced, "source_epoch_order").as_number(), 2.0);
    const auto stale = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-1", 2U));
    EXPECT_NE(stale.find("scene_runtime_observer_session_stale"),
              std::string::npos)
        << stale;
    const auto replacement_accepted = SendCommand(
        directory.socket_path(),
        SceneRuntimeObserveRequest(*initial, "observer-epoch-2", 1U, 1U));
    EXPECT_NE(replacement_accepted.find("\"source_epoch_order\":2"),
              std::string::npos)
        << replacement_accepted;

    const auto freshness_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(2);
    bool observed_stale = false;
    while (std::chrono::steady_clock::now() < freshness_deadline) {
      const auto snapshot = scene_runtime_source.Read();
      if (snapshot.has_value() &&
          content_identity::canonical_json(snapshot->payload)
                  .find("\"fresh\":false") != std::string::npos) {
        observed_stale = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    EXPECT_TRUE(observed_stale);
    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory);
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_runtime_source(directory.publication_root(),
                                                   "scene-runtime-source.json");
  const auto replacement_snapshot = scene_runtime_source.Read();
  ASSERT_TRUE(replacement_snapshot.has_value());
  EXPECT_EQ(content_identity::canonical_json(replacement_snapshot->payload)
                .find("\"source_id\":\"isaac.observer\""),
            std::string::npos);
  AtomicSourceSnapshotChannel viewer_sessions(directory.publication_root(),
                                             "viewer-pose-sessions.json");
  const auto restarted_sessions = viewer_sessions.Read();
  ASSERT_TRUE(restarted_sessions.has_value());
  EXPECT_EQ(restarted_sessions->source_owner_epoch,
            replacement_snapshot->source_owner_epoch);
  EXPECT_TRUE(RequiredMember(restarted_sessions->payload, "sessions").as_array().empty());
  const auto stale_after_owner_loss =
      SendCommand(directory.socket_path(),
                  SceneRuntimeObserveRequest(*replacement_snapshot,
                                             "observer-epoch-2", 2U));
  EXPECT_NE(stale_after_owner_loss.find("scene_runtime_observer_session_stale"),
            std::string::npos)
      << stale_after_owner_loss;
  const auto reopened = SendCommand(
      directory.socket_path(), SceneRuntimeObserverOpenRequest(
                                   *replacement_snapshot, "observer-epoch-3"));
  EXPECT_NE(reopened.find("\"source_epoch_order\":1"), std::string::npos)
      << reopened;
  EXPECT_TRUE(WIFEXITED(replacement.Stop(SIGTERM)));
}

TEST(ManagerDaemonProcess,
     PackageRootReplacementLatchesTypedRestartWhileReadsAndStopRemainOpen) {
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_CANONICAL_EMPTY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto selected_root = directory.apply_package_root();
  const auto displaced_root = selected_root.string() + ".displaced";
  ManagerProcess manager(directory, selected_root.string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before.has_value());
  const auto children_before_replacement = ReadDirectChildren(manager.pid());
  std::filesystem::rename(selected_root, displaced_root);
  ASSERT_TRUE(std::filesystem::create_directory(selected_root));

  const auto reconcile = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})");
  EXPECT_EQ(ReadDirectChildren(manager.pid()), children_before_replacement);
  EXPECT_NE(reconcile.find("\"code\":\"full_stack_restart_required\""),
            std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"reason\":\"snapshot_integrity_lost\""),
            std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"retryable\":\"false\""), std::string::npos)
      << reconcile;
  EXPECT_NE(reconcile.find("\"required_action\":\"complete_stack_stop_start\""),
            std::string::npos)
      << reconcile;

  const auto apply =
      ApplyRequest(directory, before->source_owner_epoch, 1U, "candidate",
                   kDeploymentRevision, "current-graph", "current-bundle",
                   "request-1", {}, true);
  EXPECT_NE(apply.find("\"code\":\"full_stack_restart_required\""),
            std::string::npos)
      << apply;

  const auto owner_read = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"scene_owner_read"})");
  EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos) << owner_read;

  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(2);
  std::optional<SourceSnapshot> blocked;
  while (std::chrono::steady_clock::now() < deadline) {
    blocked = manager_source.Read();
    if (blocked.has_value() &&
        content_identity::canonical_json(blocked->payload)
                .find("full_stack_restart_required") != std::string::npos) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_TRUE(blocked.has_value());
  const auto blocked_payload =
      content_identity::canonical_json(blocked->payload);
  EXPECT_NE(blocked_payload.find("\"manager_ready\":false"), std::string::npos);
  EXPECT_NE(blocked_payload.find(
                "\"unavailable_reason_code\":\"full_stack_restart_required\""),
            std::string::npos);

  const auto shutdown = SendCommand(
      directory.socket_path(),
      R"({"api_version":"manyforge.manager_command.v1","command":"shutdown_runtime"})");
  EXPECT_NE(shutdown.find("\"ok\":true"), std::string::npos) << shutdown;
  const auto status = manager.Wait();
  EXPECT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
}

TEST(ManagerDaemonProcess, EnforcesOneProcessOwnerPerManagerStateRoot) {
  DaemonDirectory directory;
  ManagerProcess owner(directory);
  owner.WaitReady(directory);
  ManagerProcess contender(directory);
  const auto contender_status = contender.Wait();
  ASSERT_TRUE(WIFEXITED(contender_status));
  EXPECT_EQ(WEXITSTATUS(contender_status), 70);
  EXPECT_EQ(::kill(owner.pid(), 0), 0);
  const auto owner_status = owner.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(owner_status));
  EXPECT_EQ(WEXITSTATUS(owner_status), 0);
}

TEST(ManagerDaemonProcess,
     SceneCommandsFailClosedWithoutKernelAndReadsSurviveReplacement) {
  DaemonDirectory directory;
  std::string request;
  std::string committed_digest;
  std::uint64_t committed_generation = 0U;
  {
    ManagerProcess manager(directory);
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    const auto initial = scene_source.Read();
    ASSERT_TRUE(initial.has_value());
    const auto owner_read =
        SendCommand(directory.socket_path(),
                    "{\"api_version\":\"manyforge.manager_command.v1\","
                    "\"command\":\"scene_owner_read\"}");
    EXPECT_NE(owner_read.find("\"ok\":true"), std::string::npos);
    EXPECT_NE(owner_read.find(initial->source_owner_epoch), std::string::npos);
    const auto preview = SendCommand(
        directory.socket_path(),
        ScenePreviewRequest(*initial, "scene-process-preview.1", 1U));
    EXPECT_NE(preview.find("\"ok\":false"), std::string::npos) << preview;
    EXPECT_NE(
        preview.find("\"reason\":\"scene_candidate_operation_unsupported\""),
        std::string::npos)
        << preview;
    const auto after_preview = scene_source.Read();
    ASSERT_TRUE(after_preview.has_value());
    EXPECT_EQ(after_preview->semantic_content_digest,
              initial->semantic_content_digest);
    EXPECT_EQ(after_preview->source_generation, initial->source_generation);
    request = SceneAddRequest(*initial, "scene-process-request.1", 1U);
    const auto committed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(committed.find("\"ok\":false"), std::string::npos);
    EXPECT_NE(committed.find("\"authoring_not_ready\""), std::string::npos);
    EXPECT_NE(committed.find("\"replay_status\":\"accepted_new\""),
              std::string::npos);
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"authoring_not_ready\""), std::string::npos);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos);

    const auto published = scene_source.Read();
    ASSERT_TRUE(published.has_value());
    EXPECT_EQ(published->source_generation, initial->source_generation);
    const auto pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*published, "scene-process-box"));
    EXPECT_NE(pose.find("\"object_not_found\""), std::string::npos);
    const auto stale_pose =
        SendCommand(directory.socket_path(),
                    ScenePoseRequest(*initial, "scene-process-box"));
    EXPECT_NE(stale_pose.find("\"object_not_found\""), std::string::npos);
    EXPECT_NE(stale_pose.find(
                  "\"details\":{\"scene_resource_id\":\"scene-process-box\"}"),
              std::string::npos)
        << stale_pose;
    // The owner resolves against its current projection, a same or newer
    // revision of the expected Scene; one it has not reached is refused.
    const auto ahead_pose = SendCommand(
        directory.socket_path(),
        ScenePoseRequestAheadOfOwner(*published, "scene-process-box"));
    EXPECT_NE(ahead_pose.find("\"scene_projection_changed\""),
              std::string::npos)
        << ahead_pose;
    const auto absent_pose = SendCommand(
        directory.socket_path(), ScenePoseRequest(*published, "absent-object"));
    EXPECT_NE(absent_pose.find("\"object_not_found\""), std::string::npos);
    EXPECT_NE(absent_pose.find(
                  "\"details\":{\"scene_resource_id\":\"absent-object\"}"),
              std::string::npos)
        << absent_pose;
    committed_digest = published->semantic_content_digest;
    committed_generation = published->source_generation;

    const auto conflict = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial, "scene-process-request.1", 1U, 0.15));
    EXPECT_NE(conflict.find("\"request_id_conflict\""), std::string::npos)
        << conflict;
    const auto stale = SendCommand(
        directory.socket_path(),
        SceneAddRequest(*initial, "scene-process-request.stale", 1U));
    EXPECT_NE(stale.find("\"authoring_not_ready\""), std::string::npos);

    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(directory);
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto recovered = scene_source.Read();
  ASSERT_TRUE(recovered.has_value());
  EXPECT_EQ(recovered->semantic_content_digest, committed_digest);
  EXPECT_EQ(recovered->source_generation, committed_generation);
  const auto replayed = SendCommand(directory.socket_path(), request);
  EXPECT_NE(replayed.find("\"ok\":false"), std::string::npos);
  EXPECT_NE(replayed.find("\"authoring_not_ready\""), std::string::npos);
  EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos);
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

#if defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
TEST(ManagerDaemonProcess,
     NativeLivenessWarningsNameTheirScopeAndRearmAfterActualSuccess) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  const auto socket = directory.program_runner_liveness_socket_path();
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename,
                         installed_manager.string(), socket.string());
  manager.WaitReady(directory);

  const std::string prefix =
      "Manager native liveness renewal remains fail-closed:";
  const auto warning_count = [&] {
    const auto text = ReadStartupStderr(directory);
    std::size_t count = 0U;
    for (auto position = text.find(prefix); position != std::string::npos;
         position = text.find(prefix, position + prefix.size()))
      ++count;
    return count;
  };
  const auto wait_for_warning = [&](const std::size_t expected) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (warning_count() < expected &&
           std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    EXPECT_EQ(warning_count(), expected) << ReadStartupStderr(directory);
  };
  const auto heartbeat = std::chrono::nanoseconds(
      deployment::execution_runtime_contract::manager_liveness_policy()
          .heartbeat_period_ns);
  const auto expect_unchanged_over_heartbeats = [&](const std::size_t expected) {
    const auto deadline = std::chrono::steady_clock::now() + 3 * heartbeat;
    while (std::chrono::steady_clock::now() < deadline) {
      EXPECT_EQ(warning_count(), expected) << ReadStartupStderr(directory);
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
  };

  wait_for_warning(1U);
  EXPECT_NE(ReadStartupStderr(directory).find("cell=" + fixture.cell_id),
            std::string::npos);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "predicate=program_runner_liveness_probe "
                "scope=cell-role:program_runner"),
            std::string::npos);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "Program Runner liveness socket is not an exact same-user socket"),
            std::string::npos);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  expect_unchanged_over_heartbeats(1U);

  // A stale same-user socket changes the actual connection failure, without
  // introducing another server or modifying the renewal packet contract.
  sockaddr_un address{};
  address.sun_family = AF_UNIX;
  const auto spelling = socket.string();
  ASSERT_LT(spelling.size(), sizeof(address.sun_path));
  const int stale = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
  ASSERT_GE(stale, 0);
  std::copy(spelling.begin(), spelling.end(), address.sun_path);
  const int bound = ::bind(stale, reinterpret_cast<sockaddr *>(&address),
                           sizeof(address));
  static_cast<void>(::close(stale));
  ASSERT_EQ(bound, 0);
  wait_for_warning(2U);
  EXPECT_NE(ReadStartupStderr(directory).find(
                "connect Program Runner liveness socket"),
            std::string::npos);
  expect_unchanged_over_heartbeats(2U);
  std::filesystem::remove(socket);

  ProgramRunnerProcess runner(directory, fixture.cell_id);
  // Owner-issued admission proves the complete validated renewal/readiness
  // path succeeded; a probe or a retry-gate reset alone is not recovery.
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  const auto recovered_warnings = warning_count();
  expect_unchanged_over_heartbeats(recovered_warnings);
  const auto saved_socket = socket.string() + ".saved";
  std::filesystem::rename(socket, saved_socket);
  wait_for_warning(recovered_warnings + 1U);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  // Restore promptly, before spending the existing lease-expiry budget. No
  // changed intermediate cause may disguise a missing success-edge reset.
  std::filesystem::rename(saved_socket, socket);
  EXPECT_TRUE(WaitForAuthoringAdmission(directory).authoring_allowed);
  expect_unchanged_over_heartbeats(recovered_warnings + 1U);
  std::filesystem::rename(socket, saved_socket);
  wait_for_warning(recovered_warnings + 2U);
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed);
  std::filesystem::rename(saved_socket, socket);
  const auto stopped = manager.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

TEST(ManagerDaemonProcess,
     RobotiqPositionHostApplyPublishesAControllableCellWithoutMotion) {
  const auto fixture = ReadManagerApplyFixtureMetadata(
      MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_METADATA);
  DaemonDirectory directory(true);
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_ROBOTIQ_POSITION_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared = package_activations.get_verified(
      core::ContentDigest(fixture.prepared_activation_id));
  ASSERT_NE(prepared, nullptr);
  PublishPreparedActivationClosure(directory, *prepared);
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename,
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);

  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto initial = manager_source.Read();
  ASSERT_TRUE(initial.has_value());
  const auto request = ApplyRequest(
      directory, initial->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      fixture.physical_deployment_artifact_id,
      fixture.physical_deployment_revision,
      fixture.predecessor_graph_revision,
      fixture.predecessor_retained_bundle_id,
      "manager-robotiq-position-apply.1", {}, false, "receipt",
      fixture.prepared_activation_id);
  const auto submitted = SendCommand(directory.socket_path(), request);
  ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos)
      << submitted;
  auto applied = submitted;
  const auto status_request = ApplyStatusRequest(submitted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (applied.find("\"state\":\"succeeded\"") ==
             std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    applied = SendCommand(directory.socket_path(), status_request);
  }
  ASSERT_NE(applied.find("\"state\":\"succeeded\""),
            std::string::npos)
      << applied << "\nmanager stderr:\n" << ReadStartupStderr(directory);
  const auto applied_status = SendCommand(directory.socket_path(), status_request);
  EXPECT_NE(applied_status.find("\"failure_acknowledgement\":\"unavailable\""),
            std::string::npos);
  const auto ready = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_FALSE(ready.owner_epoch.empty());
  const auto status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
  ASSERT_NE(status.find("\"ok\":true"), std::string::npos) << status;
  EXPECT_NE(status.find("tool.gripper.command"), std::string::npos)
      << status;
  EXPECT_EQ(status.find("execution.trajectory.execute"), std::string::npos)
      << status;
}

TEST(
    ManagerDaemonProcess,
    PneumaticCommandedFeedbackAliasInitialApplyPublishesAControllableCellWithoutMotion) {
  const auto fixture = ReadManagerApplyFixtureMetadata(
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_METADATA);
  DaemonDirectory directory(true);
  std::filesystem::copy(
      MANYFORGE_HEADLESS_TEST_PNEUMATIC_COMMANDED_FEEDBACK_PACKAGE_ROOT,
      directory.apply_package_root(), std::filesystem::copy_options::recursive);
  // Inspect the actual generated product: the command and feedback bindings
  // share one pneumatic target, so this exercises the scalar alias closure.
  const deployment::FilesystemVerifiedRetainedBundleSource bundles(
      directory.apply_package_root());
  const auto candidate = bundles.get_verified(
      core::RetainedBundleId(fixture.physical_candidate_retained_bundle_id));
  ASSERT_NE(candidate, nullptr);
  const auto &graph = candidate->value().cell_graph;
  ASSERT_EQ(graph.controllable_components.size(), 1U);
  ASSERT_EQ(graph.runtime_units.size(), 1U);
  ASSERT_EQ(graph.control_bindings.size(), 2U);
  const auto configuration = deployment::
      ros2_control_execution_runtime_contract::require_configuration(
          *candidate, graph.runtime_units.front().runtime_unit_id);
  ASSERT_EQ(configuration.gripper_target_configurations.size(), 1U);
  const auto &gripper = configuration.gripper_target_configurations.front();
  EXPECT_EQ(gripper.actuation_kind, "pneumatic");
  EXPECT_EQ(gripper.output_mode, core::GripperOutputMode::kCommanded);
  ASSERT_EQ(configuration.controller_profiles.size(), 1U);
  EXPECT_EQ(configuration.controller_profiles.front().lane, "mock");
  EXPECT_EQ(configuration.controller_profiles.front().model_family_id,
            "lite_gripper");
  const auto command_binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "tool.gripper.command";
      });
  const auto feedback_binding = std::find_if(
      graph.control_bindings.begin(), graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "tool.gripper.feedback";
      });
  ASSERT_NE(command_binding, graph.control_bindings.end());
  ASSERT_NE(feedback_binding, graph.control_bindings.end());
  EXPECT_EQ(command_binding->controllable_component_id,
            graph.controllable_components.front().controllable_component_id);
  EXPECT_EQ(command_binding->controllable_component_id,
            gripper.controllable_component_id);
  EXPECT_EQ(command_binding->command_target_id, gripper.command_target_id);
  EXPECT_EQ(feedback_binding->controllable_component_id,
            command_binding->controllable_component_id);
  EXPECT_EQ(feedback_binding->command_target_id,
            command_binding->command_target_id);
  EXPECT_EQ(feedback_binding->control_domain_id,
            command_binding->control_domain_id);
  EXPECT_EQ(feedback_binding->provider_runtime_unit_id,
            command_binding->provider_runtime_unit_id);
  EXPECT_EQ(feedback_binding->resource_bindings,
            command_binding->resource_bindings);

  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared = package_activations.get_verified(
      core::ContentDigest(fixture.prepared_activation_id));
  ASSERT_NE(prepared, nullptr);
  PublishPreparedActivationClosure(directory, *prepared);
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         fixture.admission_filename, installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);

  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto initial = manager_source.Read();
  ASSERT_TRUE(initial.has_value());
  const auto request = ApplyRequest(
      directory, initial->source_owner_epoch,
      WaitForAuthoringAdmission(directory).admission_generation,
      fixture.physical_deployment_artifact_id,
      fixture.physical_deployment_revision, fixture.predecessor_graph_revision,
      fixture.predecessor_retained_bundle_id,
      "manager-pneumatic-commanded-feedback-apply.1", {}, false, "receipt",
      fixture.prepared_activation_id);
  const auto submitted = SendCommand(directory.socket_path(), request);
  ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
  auto applied = submitted;
  const auto status_request = ApplyStatusRequest(submitted);
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    applied = SendCommand(directory.socket_path(), status_request);
  }
  ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
      << applied << "\nmanager stderr:\n"
      << ReadStartupStderr(directory);
  const auto ready = WaitForExecutionReady(
      directory, fixture.physical_candidate_retained_bundle_id);
  EXPECT_FALSE(ready.owner_epoch.empty());
  // The Manager's own source refreshes on its next observation cycle, which
  // may follow the Apply's terminal record and the host's readiness.
  auto current = manager_source.Read();
  const auto source_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while ((!current.has_value() ||
          RequiredText(RequiredMember(current->payload, "active"),
                       "retained_bundle_id") !=
              fixture.physical_candidate_retained_bundle_id) &&
         std::chrono::steady_clock::now() < source_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    current = manager_source.Read();
  }
  ASSERT_TRUE(current.has_value());
  EXPECT_EQ(RequiredText(RequiredMember(current->payload, "active"),
                         "retained_bundle_id"),
            fixture.physical_candidate_retained_bundle_id);
  const auto status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
  ASSERT_NE(status.find("\"ok\":true"), std::string::npos) << status;
  EXPECT_NE(status.find("tool.gripper.command"), std::string::npos) << status;
  EXPECT_EQ(status.find("execution.trajectory.execute"), std::string::npos)
      << status;
}

TEST(ManagerDaemonProcess,
     CompatibleNativeReplacementUsesFreshEpochAfterCleanStop) {
  const auto fixture_a = ReadManagerApplyFixtureMetadata();
  const auto fixture_b = ReadManagerApplyFixtureMetadata(
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_METADATA);
  ASSERT_EQ(fixture_a.close_retained_bundle_id,
            fixture_b.close_retained_bundle_id);

  const std::filesystem::path package_a =
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT;
  const std::filesystem::path package_b =
      MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_REPLACEMENT_PACKAGE_ROOT;
  ASSERT_NE(package_a, package_b);

  DaemonDirectory directory_a;
  std::filesystem::copy(package_a, directory_a.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager_a = PrepareManagerApplyInstalledTargets(
      directory_a, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST);
  const auto identity_a = ReadNativePackageIdentity(
      package_a, fixture_a.physical_candidate_retained_bundle_id,
      directory_a.installed_root() / "manyforge_ros2_control_execution_host");
  ASSERT_TRUE(component_manager::VerifyExecutionHostUnitForHost(
                  *identity_a.bundle,
                  identity_a.bundle->value()
                      .cell_graph.runtime_units.front()
                      .runtime_unit_id,
                  deployment::FilesystemVerifiedRetainedProductArtifactSource(
                      package_a),
                  "linux",
                  deployment::ros2_control_execution_runtime_contract::
                      kLaunchEnvironmentContract)
                  .has_value());

  ProgramRunnerProcess program_runner_a(directory_a, fixture_a.cell_id);
  std::string manager_epoch_a;
  std::string runtime_epoch_a;
  {
    ManagerProcess manager(
        directory_a, package_a.string(), fixture_a.admission_filename,
        installed_manager_a.string(), program_runner_a.socket_path().string());
    manager.WaitReady(directory_a);
    AtomicSourceSnapshotChannel manager_source(directory_a.publication_root(),
                                               "manager-source.json");
    const auto initial = manager_source.Read();
    ASSERT_TRUE(initial.has_value());
    manager_epoch_a = initial->source_owner_epoch;
    const auto admission = WaitForAuthoringAdmission(directory_a);
    const auto request = ApplyRequest(directory_a, manager_epoch_a,
                                      admission.admission_generation,
                                      fixture_a.physical_deployment_artifact_id,
                                      fixture_a.physical_deployment_revision,
                                      fixture_a.predecessor_graph_revision,
                                      fixture_a.predecessor_retained_bundle_id,
                                      "manager-replacement-a.1");
    const auto submitted = SendCommand(directory_a.socket_path(), request);
    ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory_a.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    const auto status = deployment::parse_strict_json_model_yaml(applied).root();
    const auto &prepared = RequiredMember(status, "prepared_activation");
    ASSERT_TRUE(prepared.is_object());
    EXPECT_EQ(prepared.as_object().size(), 5U);
    const auto requested = deployment::parse_strict_json_model_yaml(request).root();
    EXPECT_EQ(RequiredText(prepared, "prepared_activation_id"),
              RequiredText(RequiredMember(requested, "prepared_activation"),
                           "prepared_activation_id"));
    ASSERT_NE(applied.find(fixture_a.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;
    const auto ready = WaitForExecutionReady(
        directory_a, fixture_a.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(ready.owner_epoch.empty());
    runtime_epoch_a = ready.owner_epoch;
    ASSERT_FALSE(runtime_epoch_a.empty());
    const auto stopped = manager.Stop(SIGTERM);
    ASSERT_TRUE(WIFEXITED(stopped));
    EXPECT_EQ(WEXITSTATUS(stopped), 0);
    EXPECT_FALSE(std::filesystem::exists(directory_a.socket_path()));
    EXPECT_FALSE(std::filesystem::exists(directory_a.execution_socket_path()));
  }

  DaemonDirectory directory_b;
  std::filesystem::copy(package_b, directory_b.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto installed_manager_b = PrepareManagerApplyInstalledTargets(
      directory_b, MANYFORGE_HEADLESS_TEST_ROS_REPLACEMENT_EXECUTION_HOST);
  const auto identity_b = ReadNativePackageIdentity(
      package_b, fixture_b.physical_candidate_retained_bundle_id,
      directory_b.installed_root() / "manyforge_ros2_control_execution_host");
  ASSERT_TRUE(component_manager::VerifyExecutionHostUnitForHost(
                  *identity_b.bundle,
                  identity_b.bundle->value()
                      .cell_graph.runtime_units.front()
                      .runtime_unit_id,
                  deployment::FilesystemVerifiedRetainedProductArtifactSource(
                      package_b),
                  "linux",
                  deployment::ros2_control_execution_runtime_contract::
                      kLaunchEnvironmentContract)
                  .has_value());
  EXPECT_NE(identity_a.executable_digest, identity_b.executable_digest);
  // Different compatible executables consume the same semantic package. Their
  // release identity is diagnostic and no longer package admission authority.
  EXPECT_EQ(identity_a.product.product_identity(),
            identity_b.product.product_identity());
  EXPECT_EQ(identity_a.product.configuration_identity(),
            identity_b.product.configuration_identity());
  EXPECT_EQ(identity_a.bundle->retained_bundle_id(),
            identity_b.bundle->retained_bundle_id());
  EXPECT_EQ(identity_a.offer, identity_b.offer);
  EXPECT_EQ(identity_a.offer.offer_digest, identity_b.offer.offer_digest);

  ProgramRunnerProcess program_runner_b(directory_b, fixture_b.cell_id);
  std::string manager_epoch_b;
  std::string runtime_epoch_b;
  {
    ManagerProcess manager(
        directory_b, package_b.string(), fixture_b.admission_filename,
        installed_manager_b.string(), program_runner_b.socket_path().string());
    manager.WaitReady(directory_b);
    AtomicSourceSnapshotChannel manager_source(directory_b.publication_root(),
                                               "manager-source.json");
    const auto initial = manager_source.Read();
    ASSERT_TRUE(initial.has_value());
    manager_epoch_b = initial->source_owner_epoch;
    const auto admission = WaitForAuthoringAdmission(directory_b);
    const auto request = ApplyRequest(directory_b, manager_epoch_b,
                                      admission.admission_generation,
                                      fixture_b.physical_deployment_artifact_id,
                                      fixture_b.physical_deployment_revision,
                                      fixture_b.predecessor_graph_revision,
                                      fixture_b.predecessor_retained_bundle_id,
                                      "manager-replacement-b.1");
    const auto submitted = SendCommand(directory_b.socket_path(), request);
    ASSERT_NE(submitted.find("\"apply_id\""), std::string::npos) << submitted;
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory_b.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    ASSERT_NE(applied.find(fixture_b.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;
    const auto ready = WaitForExecutionReady(
        directory_b, fixture_b.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(ready.owner_epoch.empty());
    runtime_epoch_b = ready.owner_epoch;
    ASSERT_FALSE(runtime_epoch_b.empty());
    EXPECT_NE(manager_epoch_a, manager_epoch_b);
    EXPECT_NE(runtime_epoch_a, runtime_epoch_b);
    const auto stopped = manager.Stop(SIGTERM);
    ASSERT_TRUE(WIFEXITED(stopped));
    EXPECT_EQ(WEXITSTATUS(stopped), 0);
    EXPECT_FALSE(std::filesystem::exists(directory_b.socket_path()));
    EXPECT_FALSE(std::filesystem::exists(directory_b.execution_socket_path()));
  }
}

TEST(ManagerDaemonProcess,
     DestructiveApplyImpactRequiresExplicitAcceptanceOfItsExactDigest) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  ManagerProcess manager(directory, package_root, fixture.admission_filename,
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto before = manager_source.Read();
  ASSERT_TRUE(before.has_value());
  // First Apply the physical fixture cell with its Program.
  const auto physical_submitted = SendCommand(
      directory.socket_path(),
      ApplyRequest(directory, before->source_owner_epoch,
                   WaitForAuthoringAdmission(directory).admission_generation,
                   fixture.physical_deployment_artifact_id,
                   fixture.physical_deployment_revision,
                   fixture.predecessor_graph_revision,
                   fixture.predecessor_retained_bundle_id,
                   "impact-physical-apply.1"));
  const auto physical_status_request = ApplyStatusRequest(physical_submitted);
  auto physical_applied = physical_submitted;
  const auto physical_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  while (physical_applied.find("\"state\":\"succeeded\"") == std::string::npos &&
         std::chrono::steady_clock::now() < physical_deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    physical_applied = SendCommand(directory.socket_path(), physical_status_request);
  }
  ASSERT_NE(physical_applied.find("\"state\":\"succeeded\""), std::string::npos)
      << physical_applied << "\nmanager stderr:\n" << ReadStartupStderr(directory);
  const auto physical =
      deployment::FilesystemVerifiedRetainedBundleSource(
          directory.apply_package_root())
          .get_verified(core::RetainedBundleId(
              fixture.physical_candidate_retained_bundle_id));
  ASSERT_NE(physical, nullptr);
  const auto physical_graph_revision =
      physical->value().cell_graph.graph_revision.value();

  // Then Close (the Apply of the empty intent) without a Program: the active
  // physical cell's components are removed, a destructive impact.
  const auto close_prepared_activation_id =
      PublishTestCloseWithoutProgram(directory, fixture);
  const auto admission =
      WaitForAuthoringAdmission(directory).admission_generation;
  std::string last_request;
  const auto submit = [&](const std::string_view request_id,
                          const std::string_view accepted) {
    last_request =
        ApplyRequest(directory, before->source_owner_epoch, admission,
                     fixture.close_deployment_artifact_id,
                     fixture.close_deployment_revision,
                     physical_graph_revision,
                     fixture.physical_candidate_retained_bundle_id, request_id,
                     {}, false, accepted, close_prepared_activation_id);
    const auto response = SendCommand(directory.socket_path(), last_request);
    return response;
  };

  const auto unaccepted = submit("impact-unaccepted", "");
  EXPECT_NE(unaccepted.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << unaccepted;
  EXPECT_NE(unaccepted.find("\"details\":{\"effect\":\"none\"}"),
            std::string::npos)
      << unaccepted;
  const auto wrong = submit(
      "impact-wrong",
      "sha256:0000000000000000000000000000000000000000000000000000000000000000");
  EXPECT_NE(wrong.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << wrong;
  EXPECT_NE(wrong.find("\"details\":{\"effect\":\"none\"}"),
            std::string::npos)
      << wrong;
  // Neither refusal recorded an operation or changed the active selection.
  const auto unchanged = manager_source.Read();
  ASSERT_TRUE(unchanged.has_value());
  const auto unchanged_payload =
      content_identity::canonical_json(unchanged->payload);
  EXPECT_EQ(unchanged_payload.find("impact-unaccepted"), std::string::npos);
  EXPECT_EQ(unchanged_payload.find("impact-wrong"), std::string::npos);

  const auto accepted = submit("impact-accepted", "receipt");
  EXPECT_EQ(accepted.find("\"code\":\"confirmation_required\""),
            std::string::npos)
      << accepted;
  EXPECT_NE(accepted.find("\"ok\":true"), std::string::npos) << accepted;

  // Once that exact request is durable, a conflicting replay is not a
  // pre-acceptance no-op even though the conflict is found synchronously.
  auto changed_replay =
      deployment::parse_strict_json_model_yaml(last_request).root().as_object();
  for (auto &[name, value] : changed_replay) {
    if (name == "source_label") {
      value = content_identity::CanonicalValue("changed-replay-source");
    }
  }
  const auto replay = deployment::parse_strict_json_model_yaml(SendCommand(
                          directory.socket_path(),
                          content_identity::canonical_json(
                              content_identity::CanonicalValue(
                                  std::move(changed_replay)))))
                          .root();
  ASSERT_EQ(RequiredMember(replay, "ok"),
            content_identity::CanonicalValue(false));
  const auto &replay_error = RequiredMember(replay, "error");
  EXPECT_EQ(RequiredText(replay_error, "code"), "apply_rejected");
  // Error responses always carry details; this refusal carries no effect.
  EXPECT_EQ(RequiredMember(replay_error, "details").find_object_member("effect"), nullptr)
      << content_identity::canonical_json(replay);
}

TEST(ManagerDaemonProcess,
     NamedApplyUsesTheRealManagedFenceAndSurvivesReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory(true);
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  const auto wait_execution_response = [&] {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::string status;
    while (std::chrono::steady_clock::now() < deadline) {
      try {
        status = SendCommand(
            directory.execution_socket_path(),
            R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
        if (status.find("\"ok\":true") != std::string::npos ||
            status.find("\"code\":\"execution_owner_not_ready\"") !=
                std::string::npos) {
          return status;
        }
      } catch (const std::exception &error) {
        status = std::string("transport_error:") + error.what();
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return status;
  };
  std::string preserved_scene_snapshot;
  std::string first_manager_epoch;
  std::string request;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string(),
                           program_runner.socket_path().string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    AtomicSourceSnapshotChannel scene_runtime_source(
        directory.publication_root(), "scene-runtime-source.json");
    const auto before = manager_source.Read();
    const auto scene_before = scene_source.Read();
    const auto scene_runtime_before = scene_runtime_source.Read();
    ASSERT_TRUE(before.has_value());
    ASSERT_TRUE(scene_before.has_value());
    const auto *const predecessor_snapshot =
        scene_before->payload.find_object_member("snapshot");
    ASSERT_NE(predecessor_snapshot, nullptr);
    ASSERT_TRUE(predecessor_snapshot->is_object());
    preserved_scene_snapshot = content_identity::canonical_json(*predecessor_snapshot);
    ASSERT_TRUE(scene_runtime_before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    EXPECT_NE(content_identity::canonical_json(before->payload)
                  .find("\"apply\":{\"mutation_available\":true"),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_before->payload)
                  .find(fixture.predecessor_retained_bundle_id),
              std::string::npos);
    request = ApplyRequest(
        directory, first_manager_epoch,
        WaitForAuthoringAdmission(directory).admission_generation,
        fixture.physical_deployment_artifact_id,
        fixture.physical_deployment_revision,
        fixture.predecessor_graph_revision,
        fixture.predecessor_retained_bundle_id, "manager-program-apply.1");

    // Model a browser disappearing after the kernel accepted its packet but
    // before it received any HTTP/RPC receipt. Manager must finish without
    // that client, and a replacement observer must reconstruct the result
    // from the authoritative inventory alone.
    SendCommandAndDisconnect(directory.socket_path(), request);
    std::string applied;
    // Every value Manager publishes while the Apply progresses is one
    // generation: a committed Apply is never shown beside the predecessor
    // publication (the transition lane once paired its last pre-Apply
    // runtime with a journal that already held the Apply's receipt).
    std::optional<std::string> split_generation;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < apply_deadline) {
      const auto observed = manager_source.Read();
      if (observed.has_value()) {
        applied = content_identity::canonical_json(observed->payload);
        if (!split_generation.has_value() &&
            !ManagerSourceHistoryIsOneGeneration(observed->payload)) {
          split_generation = applied;
        }
        if (applied.find(R"("request_id":"manager-program-apply.1")") !=
                std::string::npos &&
            applied.find(R"("terminal_state":"succeeded")") !=
                std::string::npos) {
          break;
        }
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Apply status, the logical Scene, and the Scene-runtime identity are
    // separate owner publications. The Apply worker may publish its terminal
    // record before the Manager's bounded observation cycle has refreshed the
    // latter two projections. Configuration-only Apply preserves the actual
    // logical Scene; wait for the candidate runtime with that exact snapshot.
    const auto projection_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < projection_deadline) {
      const auto observed_scene = scene_source.Read();
      const auto observed_scene_runtime = scene_runtime_source.Read();
      if (observed_scene.has_value() && observed_scene_runtime.has_value() &&
          observed_scene->payload.find_object_member("snapshot") != nullptr &&
          content_identity::canonical_json(
              *observed_scene->payload.find_object_member("snapshot")) ==
              preserved_scene_snapshot &&
          content_identity::canonical_json(observed_scene_runtime->payload)
                  .find(fixture.physical_candidate_retained_bundle_id) !=
              std::string::npos) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(split_generation.has_value())
        << split_generation.value_or("");
    {
      const auto settled = manager_source.Read();
      ASSERT_TRUE(settled.has_value());
      EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(settled->payload))
          << content_identity::canonical_json(settled->payload);
    }
    SCOPED_TRACE(applied);
    EXPECT_NE(applied.find(R"("terminal_state":"succeeded")"),
              std::string::npos)
        << [&] {
             // The daemon's own diagnostics name the failed Apply phase.
             std::ifstream stderr_file(directory.startup_stderr());
             const std::string text{std::istreambuf_iterator<char>(stderr_file),
                                    std::istreambuf_iterator<char>()};
             return text.size() > 6000U ? text.substr(text.size() - 6000U) : text;
           }();
    EXPECT_NE(applied.find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto execution_status = wait_execution_response();
    EXPECT_NE(execution_status.find("\"ok\":true"), std::string::npos)
        << execution_status;
    const auto replayed = SendCommand(directory.socket_path(), request);
    EXPECT_NE(replayed.find("\"replay_status\":\"replayed_exact\""),
              std::string::npos)
        << replayed;

    component_manager::SqliteManagerApplyJournal journal(
        directory.manager_root() /
            std::string(component_manager::kManagerApplyJournalFilename),
        core::CellId(fixture.cell_id));
    const auto inventory = journal.ReadStatusInventory();
    ASSERT_EQ(inventory.size(), 1U);
    ASSERT_TRUE(inventory.front().terminal_outcome.has_value());
    ASSERT_TRUE(inventory.front()
                    .terminal_outcome->terminal_outcome
                    .candidate_projection_revision.has_value());
    EXPECT_EQ(inventory.front()
                  .terminal_outcome->terminal_outcome
                  .candidate_projection_revision->retained_bundle_id.value(),
              fixture.physical_candidate_retained_bundle_id);

    // Optional export is reached only after this test's real Manager Apply
    // and its durable journal report the same successful candidate.
    if (const char* export_path = std::getenv("MANYFORGE_REAL_APPLY_FIXTURE_EXPORT")) {
      const auto& intent = inventory.front().intent;
      ASSERT_TRUE(inventory.front().terminal_outcome->terminal_outcome.succeeded());
      ASSERT_NE(applied.find(R"("terminal_state":"succeeded")"), std::string::npos);
      ASSERT_NE(applied.find(intent.intent_digest.value()), std::string::npos);
      ASSERT_TRUE(intent.intent.prepared_activation.program_binding_id.has_value());
      ASSERT_EQ(intent.intent.prepared_activation.program_binding_id->value(), fixture.program_binding_id);
      ASSERT_EQ(intent.intent.candidate_retained_bundle_id.value(), fixture.physical_candidate_retained_bundle_id);
      const auto package = directory.apply_package_root();
      const auto bundle = deployment::FilesystemVerifiedRetainedBundleSource(package)
          .get_verified(intent.intent.candidate_retained_bundle_id);
      ASSERT_NE(bundle, nullptr);
      ASSERT_EQ(bundle->value().cell_graph.graph_revision, intent.intent.candidate_graph_revision);
      ASSERT_EQ(bundle->value().compiled_products.symbol_role_maps.size(), 1U);
      const auto map = deployment::FilesystemVerifiedRetainedModelMapSource(package)
          .get_verified({bundle->retained_bundle_id(), bundle->value().compiled_products.symbol_role_maps.front().product_id});
      ASSERT_TRUE(map.has_value());
      deployment::FilesystemCatalogArtifactStore catalogs(package);
      const auto program = deployment::FilesystemVerifiedProgramSource(package).get_verified(
          core::ProgramId(fixture.program_id), core::ProgramRevision(fixture.program_revision), *bundle);
      ASSERT_NE(program, nullptr);
      const auto contract = deployment::verify_program_binding_contract(
          program->canonical_program_bytes(), *bundle, map, catalogs);
      const auto binding = deployment::FilesystemVerifiedProgramBindingSource(package).get_verified(
          *intent.intent.prepared_activation.program_binding_id, *bundle, *map, catalogs);
      ASSERT_NE(binding, nullptr);
      ASSERT_EQ(binding->value().program_revision.value(), fixture.program_revision);
      const auto facts = deployment::CaptureProgramFrameFacts(
          contract->value(), *map, intent.intent_digest.value(), binding->value().program_binding_id.value(),
          fixture.program_revision, bundle->retained_bundle_id().value(),
          bundle->value().cell_graph.graph_revision.value());
      const char* source_root = std::getenv("MANYFORGE_REAL_APPLY_FIXTURE_SOURCE_ROOT");
      ASSERT_NE(source_root, nullptr);
      const auto source_hash = [&](const char* name) {
        std::ifstream in(std::filesystem::path(source_root) / name, std::ios::binary);
        if (!in) throw std::runtime_error("fixture provenance source is unavailable");
        const std::string bytes{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        return content_identity::sha256_digest(bytes);
      };
      const auto fact_bytes = content_identity::canonical_json(facts);
      using V = content_identity::CanonicalValue;
      const auto provenance = V(V::Object{
          {"producer_test_source_sha256", V(source_hash("manyforge_headless/test/test_manager_daemon_process.cpp"))},
          {"package_generator_source_sha256", V(source_hash("manyforge_headless/test/generate_manager_apply_package.cpp"))},
          {"frame_capture_source_sha256", V(source_hash("manyforge_deployment/src/program_reference_facts.cpp"))},
          {"captured_input_digest", V(intent.intent.prepared_activation.captured_input_digest.value())},
          {"prepared_activation_id", V(intent.intent.prepared_activation.prepared_activation_id.value())},
          {"program_sha256", V(content_identity::sha256_digest(program->canonical_program_bytes()))},
          {"binding_sha256", V(content_identity::sha256_digest(binding->canonical_bytes()))},
          {"frame_facts_sha256", V(content_identity::sha256_digest(fact_bytes))},
          {"manager_snapshot_sha256", V(content_identity::sha256_digest(applied))},
      });
      const auto destination = std::filesystem::path(export_path);
      std::filesystem::create_directories(destination);
      const auto emit = [&](const char* name, std::string_view bytes) {
        std::ofstream out(destination / name, std::ios::binary | std::ios::trunc);
        if (!out || !(out << bytes)) throw std::runtime_error("could not export committed Apply fixture");
      };
      emit("program.json", program->canonical_program_bytes());
      emit("program_binding.json", binding->canonical_bytes());
      emit("program_frame_facts.json", fact_bytes);
      emit("committed_manager_snapshot.json", applied);
      emit("provenance.json", content_identity::canonical_json(provenance));
    }

    const auto manager_after = manager_source.Read();
    const auto scene_after = scene_source.Read();
    const auto scene_runtime_after = scene_runtime_source.Read();
    ASSERT_TRUE(manager_after.has_value());
    ASSERT_TRUE(scene_after.has_value());
    ASSERT_TRUE(scene_runtime_after.has_value());
    EXPECT_NE(content_identity::canonical_json(manager_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto *const applied_snapshot = scene_after->payload.find_object_member("snapshot");
    ASSERT_NE(applied_snapshot, nullptr);
    EXPECT_EQ(content_identity::canonical_json(*applied_snapshot), preserved_scene_snapshot);
    EXPECT_EQ(content_identity::canonical_json(scene_after->payload).find("apply_fixture_marker"),
              std::string::npos);
    EXPECT_NE(content_identity::canonical_json(scene_runtime_after->payload)
                  .find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos);
    const auto status = manager.Stop(SIGKILL);
    EXPECT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(
      directory, package_root, fixture.admission_filename,
      installed_manager.string(), program_runner.socket_path().string());
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime_source(directory.publication_root(),
                                                   "scene-runtime-source.json");
  const auto recovered = manager_source.Read();
  const auto recovered_scene = scene_source.Read();
  const auto scene_runtime_recovered = scene_runtime_source.Read();
  ASSERT_TRUE(recovered.has_value());
  ASSERT_TRUE(recovered_scene.has_value());
  ASSERT_TRUE(scene_runtime_recovered.has_value());
  const auto recovered_manager_bytes =
      content_identity::canonical_json(recovered->payload);
  EXPECT_NE(recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(recovered_manager_bytes.find(
                fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  EXPECT_NE(recovered_manager_bytes.find(
                R"("request_id":"manager-program-apply.1")"),
            std::string::npos);
  EXPECT_NE(
      recovered_manager_bytes.find(R"("incomplete":false,"intent_digest":)"),
      std::string::npos);
  EXPECT_NE(content_identity::canonical_json(scene_runtime_recovered->payload)
                .find(fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  const auto *const recovered_snapshot = recovered_scene->payload.find_object_member("snapshot");
  ASSERT_NE(recovered_snapshot, nullptr);
  EXPECT_EQ(content_identity::canonical_json(*recovered_snapshot), preserved_scene_snapshot);
  EXPECT_EQ(content_identity::canonical_json(recovered_scene->payload).find("apply_fixture_marker"),
            std::string::npos);
  const auto recovered_execution_status = wait_execution_response();
  // Reopening the Manager may read completed Apply evidence, but it must not
  // silently restart a physical execution owner after process loss.
  EXPECT_NE(recovered_execution_status.find("\"ok\":false"),
            std::string::npos)
      << recovered_execution_status;
  EXPECT_NE(recovered_execution_status.find(
                "\"code\":\"execution_owner_not_ready\""),
            std::string::npos)
      << recovered_execution_status;
  const auto replay = SendCommand(directory.socket_path(), request);
  EXPECT_NE(replay.find("\"state\":\"succeeded\""), std::string::npos)
      << replay;
  EXPECT_NE(replay.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos)
      << replay;
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
}

TEST(ManagerDaemonProcess,
     PhysicalCompositionStaysClosedAfterUnexpectedChildLossAndManagerReplacement) {
  const auto fixture = ReadManagerApplyFixtureMetadata();
  DaemonDirectory directory;
  std::filesystem::copy(MANYFORGE_HEADLESS_TEST_MANAGER_APPLY_PACKAGE_ROOT,
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  const auto package_root = directory.apply_package_root().string();
  const auto installed_manager = PrepareManagerApplyInstalledTargets(directory);
  ProgramRunnerProcess program_runner(directory, fixture.cell_id);
  std::string first_manager_epoch;
  std::string apply_request;
  std::string applied_owner_epoch;
  RuntimeOfferProjection initial_offer;
  std::uint64_t advanced_scene_generation = 0U;
  {
    ManagerProcess manager(directory, package_root, fixture.admission_filename,
                           installed_manager.string(),
                           program_runner.socket_path().string());
    manager.WaitReady(directory);
    AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                               "manager-source.json");
    AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                             "logical-scene-source.json");
    const auto before = manager_source.Read();
    ASSERT_TRUE(before.has_value());
    first_manager_epoch = before->source_owner_epoch;
    apply_request =
        ApplyRequest(directory, first_manager_epoch,
                     WaitForAuthoringAdmission(directory).admission_generation,
                     fixture.physical_deployment_artifact_id,
                     fixture.physical_deployment_revision,
                     fixture.predecessor_graph_revision,
                     fixture.predecessor_retained_bundle_id,
                     "manager-physical-recovery-apply.1");

    const auto submitted = SendCommand(directory.socket_path(), apply_request);
    const auto status_request = ApplyStatusRequest(submitted);
    auto applied = submitted;
    const auto apply_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (applied.find("\"state\":\"succeeded\"") == std::string::npos &&
           std::chrono::steady_clock::now() < apply_deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
      applied = SendCommand(directory.socket_path(), status_request);
    }
    ASSERT_NE(applied.find("\"state\":\"succeeded\""), std::string::npos)
        << applied;
    ASSERT_NE(applied.find(fixture.physical_candidate_retained_bundle_id),
              std::string::npos)
        << applied;

    const auto initial = WaitForExecutionReady(
        directory, fixture.physical_candidate_retained_bundle_id);
    ASSERT_FALSE(initial.owner_epoch.empty());
    applied_owner_epoch = initial.owner_epoch;
    initial_offer =
        WaitForRuntimeOfferProjection(manager_source, initial.owner_epoch);
    EXPECT_EQ(initial_offer.selected, initial_offer.observed);
    EXPECT_EQ(initial.owner_epoch,
              "kernel-owner-" + initial_offer.observed_epoch);

    const auto advanced_scene_projection = scene_source.Read();
    ASSERT_TRUE(advanced_scene_projection.has_value());
    const auto advanced_scene = SendCommand(
        directory.socket_path(),
        SceneAddRequest(
            *advanced_scene_projection, "manager-physical-recovery-scene.1",
            WaitForAuthoringAdmission(directory).admission_generation));
    EXPECT_NE(advanced_scene.find("\"ok\":true"), std::string::npos)
        << advanced_scene;
    const auto scene_advance_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::optional<SourceSnapshot> after_scene_advance;
    while (std::chrono::steady_clock::now() < scene_advance_deadline &&
           (!after_scene_advance.has_value() ||
            after_scene_advance->source_generation <=
                advanced_scene_projection->source_generation)) {
      after_scene_advance = scene_source.Read();
      if (after_scene_advance.has_value() &&
          after_scene_advance->source_generation >
              advanced_scene_projection->source_generation) {
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ASSERT_TRUE(after_scene_advance.has_value());
    EXPECT_GT(after_scene_advance->source_generation,
              advanced_scene_projection->source_generation);
    advanced_scene_generation = after_scene_advance->source_generation;

    const auto first_child =
        WaitForSoleManagedChild(manager.pid(), "before deliberate SIGKILL");
    ASSERT_EQ(::kill(first_child, SIGKILL), 0);

    // Unexpected loss of a required owner is contained fault evidence, not an
    // implicit replacement request. Steady-state observation and the public
    // reconcile command must never start the lost managed unit again; only the
    // Manager's own bootstrap may reinitialize it.
    const auto reap_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < reap_deadline &&
           !ReadDirectChildren(manager.pid()).empty()) {
      static_cast<void>(SendCommand(
          directory.socket_path(),
          R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})"));
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    ASSERT_TRUE(ReadDirectChildren(manager.pid()).empty())
        << "the exited managed child was not observed and reaped";
    for (int attempt = 0; attempt < 5; ++attempt) {
      const auto reconciled = SendCommand(
          directory.socket_path(),
          R"({"api_version":"manyforge.manager_command.v1","command":"reconcile"})");
      EXPECT_NE(reconciled.find("\"ok\":true"), std::string::npos)
          << reconciled;
      EXPECT_TRUE(ReadDirectChildren(manager.pid()).empty())
          << "steady-state observation restarted a lost managed child";
    }
    const auto manager_loss = manager_source.Read();
    ASSERT_TRUE(manager_loss.has_value());
    EXPECT_NE(content_identity::canonical_json(manager_loss->payload)
                  .find("\"manager_ready\":false"),
              std::string::npos)
        << "lost required owner must keep readiness closed";
    const auto status = manager.Stop(SIGKILL);
    ASSERT_TRUE(WIFSIGNALED(status));
  }

  ManagerProcess replacement(
      directory, package_root, fixture.admission_filename,
      installed_manager.string(), program_runner.socket_path().string());
  replacement.WaitReady(directory);
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  const auto recovered_scene = scene_source.Read();
  ASSERT_TRUE(recovered_scene.has_value());
  EXPECT_GE(recovered_scene->source_generation, advanced_scene_generation);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  const auto manager_recovered = manager_source.Read();
  ASSERT_TRUE(manager_recovered.has_value());
  const auto manager_recovered_offer =
      ReadRuntimeOfferProjection(manager_recovered->payload);
  EXPECT_EQ(manager_recovered_offer.selected, manager_recovered_offer.observed);
  EXPECT_EQ(manager_recovered_offer.selected, initial_offer.selected);
  EXPECT_NE(manager_recovered_offer.observed_epoch,
            initial_offer.observed_epoch);
  EXPECT_NE("kernel-owner-" + manager_recovered_offer.observed_epoch,
            applied_owner_epoch);
  EXPECT_NE(manager_recovered->source_owner_epoch, first_manager_epoch);
  EXPECT_NE(content_identity::canonical_json(manager_recovered->payload)
                .find(fixture.physical_candidate_retained_bundle_id),
            std::string::npos);
  // An unexpected required-child loss is not a new Apply request. A Manager
  // restart may restore observation of the durable selection, but cannot
  // silently rebuild the old physical product or reopen control admission.
  const auto execution_status = SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
  EXPECT_NE(execution_status.find("\"code\":\"execution_owner_not_ready\""),
            std::string::npos)
      << execution_status;
  EXPECT_FALSE(ReadAuthoringAdmissionProjection(directory).authoring_allowed)
      << SendCommand(directory.execution_socket_path(),
                     R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
  const auto replay = SendCommand(directory.socket_path(), apply_request);
  EXPECT_NE(replay.find("\"state\":\"succeeded\""), std::string::npos)
      << replay;
  EXPECT_NE(replay.find("\"replay_status\":\"replayed_exact\""),
            std::string::npos)
      << replay;
  const auto stopped = replacement.Stop(SIGTERM);
  EXPECT_TRUE(WIFEXITED(stopped));
  EXPECT_EQ(WEXITSTATUS(stopped), 0);
}
#endif

TEST(ManagerDaemonProcess,
     EmptyRosApplyConfiguresInactiveMockThenExplicitActivation) {
#if !defined(MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST)
  if (std::getenv("MANYFORGE_ROS2_CONTROL_EXECUTION_HOST") != nullptr) {
    FAIL() << "Explicit ros-e2e lane did not register the production ROS host "
              "fixture";
  }
  GTEST_SKIP() << "Requires the production ROS host and sourced ros-e2e "
                  "container environment";
#else
  const auto *ros_host = std::getenv("MANYFORGE_ROS2_CONTROL_EXECUTION_HOST");
  if (ros_host == nullptr) {
    GTEST_SKIP() << "Requires the explicitly sourced ros-e2e lane";
  }
  ASSERT_TRUE(std::filesystem::equivalent(
      ros_host, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST));
  using content_identity::CanonicalValue;
  const auto read_file = [](const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input.good()) throw std::runtime_error("Fixture input is unavailable: " + path.string());
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  };
  // The same sourced ament lookup used by ros2 launch records the selected
  // overlay input. Refuse qualification against an older image launch file.
  const auto *launch_evidence = std::getenv("MANYFORGE_MANAGER_ROS_LAUNCH_EVIDENCE");
  ASSERT_NE(launch_evidence, nullptr);
  const auto launch_input = deployment::parse_strict_json_model_yaml(read_file(launch_evidence)).root();
  ASSERT_EQ(RequiredText(launch_input, "source_launch_file"),
            std::filesystem::canonical(MANYFORGE_HEADLESS_TEST_ROS_LAUNCH_SOURCE).string());
  const auto launch_hash = content_identity::sha256_digest(read_file(RequiredText(launch_input, "launch_file")));
  EXPECT_EQ(launch_hash, RequiredText(launch_input, "launch_sha256"));
  EXPECT_EQ(launch_hash, content_identity::sha256_digest(read_file(MANYFORGE_HEADLESS_TEST_ROS_LAUNCH_SOURCE)));
  EXPECT_EQ(launch_hash, RequiredText(launch_input, "source_sha256"));
  std::cerr << "actual Mock controller launch input="
            << content_identity::canonical_json(launch_input) << '\n';
  DaemonDirectory directory(true);
  const auto fixture_root =
      std::filesystem::path(MANYFORGE_HEADLESS_TEST_MANAGER_ROS_APPLY_FIXTURE);
  std::ifstream metadata_stream(fixture_root / "metadata.json",
                                std::ios::binary);
  ASSERT_TRUE(metadata_stream.good());
  const std::string metadata_bytes{
      std::istreambuf_iterator<char>(metadata_stream),
      std::istreambuf_iterator<char>()};
  const auto metadata =
      deployment::parse_strict_json_model_yaml(metadata_bytes).root();
  ASSERT_TRUE(metadata.is_object());
  ASSERT_EQ(metadata.as_object().size(), 9U);
  ASSERT_EQ(content_identity::canonical_json(metadata), metadata_bytes);
  const auto cell = RequiredText(metadata, "cell_id");
  const auto component = RequiredText(metadata, "component_id");
  const auto candidate_id =
      RequiredText(metadata, "candidate_retained_bundle_id");
  const auto predecessor_id =
      RequiredText(metadata, "predecessor_retained_bundle_id");
  std::filesystem::copy(fixture_root / "activation",
                        directory.apply_package_root(),
                        std::filesystem::copy_options::recursive);
  // The package and daemon activation repositories are distinct capabilities.
  // Publish the genuine stored product with its whole closure, as the
  // preparer does: the host reads the selected Program, its Binding and their
  // catalog bytes from the activation root.
  const deployment::FilesystemPreparedActivationRepository package_activations(
      directory.apply_package_root());
  const auto prepared_activation = package_activations.get_verified(
      core::ContentDigest(RequiredText(metadata, "prepared_activation_id")));
  ASSERT_NE(prepared_activation, nullptr);
  PublishPreparedActivationClosure(directory, *prepared_activation);
  // Reuse the real installed-target loader and Program runner with only the
  // selected ROS executable and its actual shared candidate worker.
  const auto installed_manager = PrepareManagerApplyInstalledTargets(
      directory, MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_HOST,
      MANYFORGE_HEADLESS_TEST_ROS_EXECUTION_WORKER,
      "manyforge_ros2_control_execution_host");
  deployment::FilesystemVerifiedRetainedBundleSource bundles(
      directory.apply_package_root());
  const auto predecessor =
      bundles.get_verified(core::RetainedBundleId(predecessor_id));
  const auto candidate =
      bundles.get_verified(core::RetainedBundleId(candidate_id));
  ASSERT_NE(predecessor, nullptr);
  ASSERT_NE(candidate, nullptr);
  ASSERT_TRUE(predecessor->value().cell_graph.controllable_components.empty());
  // The Mock Lite 6 is a machine (ARCHITECTURE.md "Machines: owners and
  // members"): owner, arm member and the
  // gripper mounted on the arm are three components; `component` is the arm.
  ASSERT_EQ(candidate->value().cell_graph.controllable_components.size(), 3U);
  EXPECT_EQ(std::count_if(
                candidate->value().cell_graph.controllable_components.begin(),
                candidate->value().cell_graph.controllable_components.end(),
                [&](const auto &row) {
                  return row.controllable_component_id.value() == component;
                }),
            1);
  for (const auto *bundle : {predecessor.get(), candidate.get()}) {
    ASSERT_EQ(bundle->value().cell_graph.runtime_units.size(), 1U);
    EXPECT_EQ(bundle->value()
                  .cell_graph.runtime_units.front()
                  .runtime_profile_id.value(),
              "manyforge.ros2_control_execution_runtime");
  }
  ProgramRunnerProcess program_runner(directory, cell);
  ManagerProcess manager(directory, directory.apply_package_root().string(),
                         RequiredText(metadata, "admission_filename"),
                         installed_manager.string(),
                         program_runner.socket_path().string());
  manager.WaitReady(directory);
  AtomicSourceSnapshotChannel manager_source(directory.publication_root(),
                                             "manager-source.json");
  AtomicSourceSnapshotChannel scene_source(directory.publication_root(),
                                           "logical-scene-source.json");
  AtomicSourceSnapshotChannel runtime_source(directory.publication_root(),
                                             "scene-runtime-source.json");
  const auto before = manager_source.Read();
  const auto scene_before = scene_source.Read();
  const auto runtime_before = runtime_source.Read();
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(scene_before.has_value());
  ASSERT_TRUE(runtime_before.has_value());
  EXPECT_EQ(RequiredText(RequiredMember(before->payload, "active"),
                         "retained_bundle_id"),
            predecessor_id);
  // The component model and component actions are served
  // on the execution host's Stop control socket.
  const auto control_command = [&](CanonicalValue::Object request) {
    request.emplace_back("api_version",
                         CanonicalValue("manyforge.execution_command.v3"));
    const auto bytes = SendCommand(
        directory.execution_run_root() / "execution-control.sock",
        content_identity::canonical_json(CanonicalValue(std::move(request))));
    const auto response =
        deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual component command failed: " + bytes);
    return response;
  };
  // Events use the stable owner/journal even before a physical product exists.
  const auto idle_identity = deployment::parse_strict_json_model_yaml(SendCommand(
      directory.execution_socket_path(),
      R"({"api_version":"manyforge.execution_command.v3","command":"identity"})")).root();
  ASSERT_EQ(RequiredMember(idle_identity, "ok"), CanonicalValue(false));
  const auto event_owner_epoch = RequiredText(
      RequiredMember(RequiredMember(idle_identity, "error"), "details"), "owner_epoch");
  const auto read_event_metadata = [&](const bool expected_ready, const bool stale_cursor = false) {
    const auto response = deployment::parse_strict_json_model_yaml(SendCommand(
        directory.execution_socket_path(), content_identity::canonical_json(
            CanonicalValue(CanonicalValue::Object{
                {"api_version", CanonicalValue("manyforge.execution_command.v3")},
                {"command", CanonicalValue("events")},
                {"owner_epoch", CanonicalValue(stale_cursor ? "unregistered-event-owner" : event_owner_epoch)},
                {"after_sequence", CanonicalValue(0.0)}, {"limit", CanonicalValue(1.0)}})))).root();
    EXPECT_EQ(RequiredMember(response, "ok"), CanonicalValue(!stale_cursor))
        << content_identity::canonical_json(response);
    EXPECT_EQ(RequiredText(response, "owner_epoch"), event_owner_epoch);
    EXPECT_EQ(RequiredMember(response, "execution_owner_ready"), CanonicalValue(expected_ready));
    const auto& faults = RequiredMember(response, "runtime_faults");
    EXPECT_EQ(faults.as_object().size(), 3U);
    EXPECT_EQ(RequiredMember(faults, "operation_journal_write_fault"), CanonicalValue(nullptr));
    EXPECT_EQ(RequiredMember(faults, "program_runner_loss"), CanonicalValue(nullptr));
    EXPECT_EQ(RequiredMember(faults, "kernel_internal_fault"), CanonicalValue(nullptr));
    if (stale_cursor) {
      EXPECT_EQ(RequiredText(RequiredMember(response, "error"), "code"),
                "operation_journal_owner_epoch_stale");
    } else {
      EXPECT_EQ(RequiredText(RequiredMember(response, "event_page"), "owner_epoch"), event_owner_epoch);
    }
  };
  read_event_metadata(false);
  read_event_metadata(false, true);  // Page error keeps actual-owner metadata.
  const auto request =
      ApplyRequest(directory, before->source_owner_epoch,
                   WaitForAuthoringAdmission(directory).admission_generation,
                   RequiredText(metadata, "deployment_artifact_id"),
                   RequiredText(metadata, "deployment_revision"),
                   RequiredText(metadata, "predecessor_graph_revision"),
                   predecessor_id, "empty-ros-one-mock.1", {}, false, "receipt",
                   RequiredText(metadata, "prepared_activation_id"));
  // Decode genuine Apply envelopes with an intentionally unowned parent claim.
  // The stale outer owner prevents every valid-shape control from applying.
  {
    const auto replace = [](CanonicalValue value, const std::string &name,
                            CanonicalValue member) {
      auto fields = value.as_object();
      for (auto &[key, field] : fields) {
        if (key == name) {
          field = std::move(member);
          return CanonicalValue(std::move(fields));
        }
      }
      throw std::logic_error("parent claim fixture field is absent");
    };
    const CanonicalValue intent(CanonicalValue::Object{
        {"claim_kind", CanonicalValue("authoring_mutation")},
        {"owner_role", CanonicalValue("authoring_owner")},
        {"owner_epoch", CanonicalValue(std::string(60U, 'o'))},
        {"owner_transaction_id", CanonicalValue("parent.decoder.transaction")},
        {"request_id", CanonicalValue("parent.decoder.request")},
        {"request_payload_digest",
         CanonicalValue("sha256:" + std::string(64U, 'a'))},
        {"expected_admission_generation", CanonicalValue(1.0)},
        {"scope",
         CanonicalValue(CanonicalValue::Object{
             {"resources", CanonicalValue(CanonicalValue::Array{})},
             {"configurations", CanonicalValue(CanonicalValue::Array{})}})},
        {"program_cycle_evidence", CanonicalValue(nullptr)},
        {"intervention_evidence", CanonicalValue(nullptr)}});
    const CanonicalValue claim(CanonicalValue::Object{
        {"intent", intent},
        {"claim_generation", CanonicalValue(2.0)},
        {"acquired_admission_generation", CanonicalValue(3.0)},
        {"accepted_at_ns", CanonicalValue(7.0)}});
    const auto submit_parent = [&](CanonicalValue parent) {
      auto command = deployment::parse_strict_json_model_yaml(request).root();
      command = replace(std::move(command), "request_owner_epoch",
                        CanonicalValue("manager.parent-decoder.stale"));
      command = replace(std::move(command), "admission_context",
                        CanonicalValue(CanonicalValue::Object{
                            {"kind", CanonicalValue("parent_authoring_claim")},
                            {"claim", std::move(parent)}}));
      return deployment::parse_strict_json_model_yaml(
                 SendCommand(directory.socket_path(),
                             content_identity::canonical_json(command)))
          .root();
    };
    for (const auto &field :
         {"claim_generation", "acquired_admission_generation"}) {
      const auto rejected =
          submit_parent(replace(claim, field, CanonicalValue(0.0)));
      EXPECT_EQ(RequiredText(RequiredMember(rejected, "error"), "code"),
                "invalid_request");
      EXPECT_EQ(RequiredText(RequiredMember(rejected, "error"), "message"),
                "Manager daemon: parent " + std::string(field) +
                    " must be one positive safe integer");
    }
    const auto bad_timestamp = submit_parent(
        replace(claim, "accepted_at_ns", CanonicalValue("not-a-timestamp")));
    EXPECT_EQ(RequiredText(RequiredMember(bad_timestamp, "error"), "code"),
              "invalid_request");
    EXPECT_EQ(RequiredText(RequiredMember(bad_timestamp, "error"), "message"),
              "exact nonnegative int64 scalar required");
    // An earlier claim generation still wins over a malformed final timestamp.
    const auto precedence = submit_parent(replace(
        replace(claim, "accepted_at_ns", CanonicalValue("not-a-timestamp")),
        "claim_generation", CanonicalValue(0.0)));
    EXPECT_EQ(RequiredText(RequiredMember(precedence, "error"), "message"),
              "Manager daemon: parent claim_generation must be one positive "
              "safe integer");
    for (const auto &timestamp :
         {CanonicalValue(7.0), CanonicalValue("9007199254740992")}) {
      const auto valid =
          submit_parent(replace(claim, "accepted_at_ns", timestamp));
      EXPECT_EQ(RequiredText(RequiredMember(valid, "error"), "code"),
                "stale_request_owner_epoch")
          << content_identity::canonical_json(valid);
      EXPECT_EQ(RequiredText(valid, "request_owner_epoch"),
                "manager.parent-decoder.stale");
    }
  }
  const auto submitted = SendCommand(directory.socket_path(), request);
  const auto submission =
      deployment::parse_strict_json_model_yaml(submitted).root();
  ASSERT_EQ(RequiredMember(submission, "ok"), CanonicalValue(true))
      << submitted;

  // Reuse the existing process suite's bounded observation interval. A guard,
  // failure or unknown outcome never qualifies as a successful first Apply.
  const auto apply_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  std::string last;
  bool succeeded = false;
  while (std::chrono::steady_clock::now() < apply_deadline) {
    const auto status = manager_source.Read();
    if (status.has_value()) {
      last = content_identity::canonical_json(status->payload);
      EXPECT_TRUE(ManagerSourceHistoryIsOneGeneration(status->payload)) << last;
      if (last.find("\"request_id\":\"empty-ros-one-mock.1\"") !=
              std::string::npos &&
          last.find("\"terminal_state\":\"succeeded\"") != std::string::npos) {
        succeeded = true;
        break;
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(succeeded) << last;
  read_event_metadata(true);  // Same stable owner and envelope after composition.
  read_event_metadata(true, true);
  component_manager::SqliteManagerApplyJournal journal(
      directory.manager_root() /
          std::string(component_manager::kManagerApplyJournalFilename),
      core::CellId(cell));
  const auto inventory = journal.ReadStatusInventory();
  ASSERT_EQ(inventory.size(), 1U);
  const auto &status = inventory.front();
  ASSERT_TRUE(status.fence_released.has_value());
  ASSERT_TRUE(status.publication_receipt.has_value());

  const auto read_component = [&](const std::string &component_id) {
    const auto model = control_command(
        {{"command", CanonicalValue("read_component_model")}});
    for (const auto &state :
         RequiredMember(model, "component_states").as_array())
      if (RequiredText(state, "component_id") == component_id) return state;
    throw std::runtime_error("Actual component state is absent: " +
                             content_identity::canonical_json(model));
  };
  // An added component starts not prepared; the operator prepares it
  // (ARCHITECTURE.md "Component model"). The arm is a machine member: offline with
  // `owner_unavailable` until its owner is prepared. The gripper
  // mounted on it is its own component ("Tools as components").
  const auto arm_state = read_component(component);
  EXPECT_EQ(RequiredText(arm_state, "lifecycle"), "offline");
  const auto owner = RequiredText(arm_state, "owner_id");
  std::string gripper_component;
  for (const auto &row : candidate->value().cell_graph.controllable_components) {
    const auto id = row.controllable_component_id.value();
    if (id != component && id != owner) gripper_component = id;
  }
  ASSERT_FALSE(gripper_component.empty());
  EXPECT_EQ(RequiredText(read_component(owner), "lifecycle"), "not_prepared");
  // An added component starts not_prepared, or offline until its process
  // reports.
  const auto gripper_state = read_component(gripper_component);
  EXPECT_TRUE(RequiredText(gripper_state, "lifecycle") == "not_prepared" ||
              RequiredText(gripper_state, "lifecycle") == "offline")
      << content_identity::canonical_json(gripper_state);

  const auto observe_ros = [&](const std::string &state, const std::string &label) {
    const auto output = directory.execution_run_root() / (label + ".ros-observation.json");
    const auto log = directory.execution_run_root() / (label + ".ros-observation.log");
    const auto execution_root = directory.execution_run_root().string();
    const auto helper = std::filesystem::path(MANYFORGE_HEADLESS_TEST_SOURCE_ROOT) /
                        "manyforge_deployment/test/prepare_activation_tool.py";
    const auto child = ::fork();
    if (child < 0) throw std::runtime_error("ROS observation fork failed");
    if (child == 0) {
      const auto fd = ::open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      if (fd < 0 || ::dup2(fd, STDOUT_FILENO) < 0 || ::dup2(fd, STDERR_FILENO) < 0) _exit(126);
      ::close(fd);
      ::execl(MANYFORGE_HEADLESS_TEST_PYTHON, MANYFORGE_HEADLESS_TEST_PYTHON,
              helper.c_str(), "--observe-manager-ros", "--execution-root", execution_root.c_str(),
              "--component", component.c_str(), "--gripper-component", gripper_component.c_str(),
              "--state", state.c_str(), "--label", label.c_str(),
              "--output", output.c_str(), static_cast<char *>(nullptr));
      _exit(127);
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    int result = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      const auto waited = ::waitpid(child, &result, WNOHANG);
      if (waited == child) {
        if (!WIFEXITED(result) || WEXITSTATUS(result) != 0)
          throw std::runtime_error("Actual ROS observation failed: " + read_file(log));
        std::cerr << "actual ROS observation=" << read_file(output) << '\n';
        return;
      }
      if (waited < 0 && errno != EINTR)
        throw std::runtime_error("ROS observation wait lost child ownership: " + read_file(log));
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    ::kill(child, SIGKILL);
    while (::waitpid(child, &result, 0) < 0 && errno == EINTR) {}
    throw std::runtime_error("Actual ROS observation timed out: " + read_file(log));
  };

  // This is the installed Kernel's actual per-component command gate, not
  // Manager convergence or process presence. Reading it never dispatches
  // motion.
  const auto read_command_gate = [&] {
    const auto bytes = SendCommand(
        directory.execution_socket_path(),
        R"({"api_version":"manyforge.execution_command.v3","command":"targets"})");
    const auto response =
        deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual execution targets read failed: " +
                               bytes);
    const auto &components =
        RequiredMember(RequiredMember(response, "targets"), "components")
            .as_array();
    const auto row = std::find_if(components.begin(), components.end(), [&](const auto &entry) {
      return RequiredText(entry, "controllable_component_id") == component;
    });
    if (row == components.end())
      throw std::runtime_error("Actual execution target membership mismatch: " +
                               bytes);
    return RequiredMember(*row, "availability");
  };
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"),
            CanonicalValue(false));

  // Produce the same intrinsic manual OperationIntent as the existing native
  // process test. Every identity, binding, role, axis and goal comes from the
  // installed owner's actual status; no Program-node/route authority is minted.
  const auto submit_current_joint_hold = [&](const std::string &request_id,
                                             std::optional<CanonicalValue>
                                                 automatic_retry_context =
                                                     std::nullopt,
                                             bool non_motion_capability =
                                                 false) {
    const auto bytes = SendCommand(
        directory.execution_socket_path(),
        R"({"api_version":"manyforge.execution_command.v3","command":"status"})");
    const auto response =
        deployment::parse_strict_json_model_yaml(bytes).root();
    if (RequiredMember(response, "ok") != CanonicalValue(true))
      throw std::runtime_error("Actual motion producer status unavailable: " + bytes);
    const auto &actual = RequiredMember(response, "status");
    const auto &identity = RequiredMember(actual, "identity");
    const auto &components = RequiredMember(RequiredMember(actual, "targets"), "components").as_array();
    const auto arm_row = std::find_if(components.begin(), components.end(), [&](const auto &entry) {
      return RequiredText(entry, "controllable_component_id") == component;
    });
    if (arm_row == components.end())
      throw std::runtime_error("Actual motion producer component closure differs");
    const auto &targets = RequiredMember(*arm_row, "targets").as_array();
    const auto target = std::find_if(targets.begin(), targets.end(), [&](const auto &row) {
      return RequiredText(row, "presentation") == "motion";
    });
    if (target == targets.end())
      throw std::runtime_error("Actual motion target is absent");
    const auto &command = RequiredMember(*target, "command");
    const auto &manual = RequiredMember(command, "manual_control");
    const auto &observed_targets = RequiredMember(RequiredMember(actual, "control_state"), "observed_targets").as_array();
    const auto observed_row = std::find_if(observed_targets.begin(), observed_targets.end(), [&](const auto &row) {
      return RequiredText(row, "controllable_component_id") == component &&
          RequiredText(row, "command_target_id") == RequiredText(*target, "command_target_id") &&
          RequiredText(row, "operation_capability_id") == RequiredText(*target, "operation_capability_id");
    });
    if (observed_row == observed_targets.end())
      throw std::runtime_error("Actual motion observation is absent");
    const auto &observed = RequiredMember(*observed_row, "observed");
    if (!observed.is_object() || RequiredMember(observed, "complete") != CanonicalValue(true) ||
        RequiredText(observed, "source_epoch").empty() || RequiredMember(observed, "sequence").as_number() <= 0.0)
      throw std::runtime_error("Actual motion observation is incomplete");
    CanonicalValue::Array joint_names;
    for (const auto &joint : RequiredMember(command, "joints").as_array())
      joint_names.emplace_back(RequiredMember(joint, "source_joint_name"));
    if (CanonicalValue(joint_names) != RequiredMember(observed, "source_joint_names") ||
        joint_names.size() != RequiredMember(observed, "positions_rad").as_array().size())
      throw std::runtime_error("Actual motion axes differ from the compiled direct target");
    CanonicalValue payload(CanonicalValue::Object{
        {"kind", CanonicalValue("motion")}, {"move_id", CanonicalValue(request_id + ".hold")},
        {"base_frame", RequiredMember(command, "base_frame")},
        // Active TCP: Robot control moves at the robot's Active TCP.
        {"tcp_endpoint", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("current")}})},
        {"tolerances", CanonicalValue(CanonicalValue::Object{
            {"joint_position_tol_rad", CanonicalValue(0.01)}, {"translation_tol_m", CanonicalValue(0.001)},
            {"rotation_tol_rad", CanonicalValue(0.01)}})},
        {"speed_profile", CanonicalValue(CanonicalValue::Object{
            {"velocity_scale", CanonicalValue(0.5)}, {"acceleration_scale", CanonicalValue(0.5)},
            {"max_tcp_linear_speed_mps", CanonicalValue(nullptr)}})},
        {"timeouts", CanonicalValue(CanonicalValue::Object{
            {"planning_timeout_s", CanonicalValue(2.0)}, {"execution_timeout_s", CanonicalValue(10.0)}})},
        {"cache_mode", CanonicalValue("default")},
        {"target_mode", CanonicalValue("absolute")}, {"target_reference", CanonicalValue(nullptr)},
        {"offset_reference", CanonicalValue(nullptr)},
        {"motion", CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("joint_goal")}, {"joint_names", CanonicalValue(std::move(joint_names))},
            {"positions_rad", RequiredMember(observed, "positions_rad")}})},
        {"contact_policy_id", CanonicalValue(nullptr)},
        {"per_joint_dynamics_limits", CanonicalValue(CanonicalValue::Object{})},
        {"motion_interrupt_scopes", CanonicalValue(CanonicalValue::Array{})},
        {"runtime_object_bindings", CanonicalValue(CanonicalValue::Array{})}});
    CanonicalValue::Object participant{
        {"target", RequiredMember(command, "target")},
        {"required_capability_id",
         non_motion_capability
             ? CanonicalValue("tool.gripper.command")
             : RequiredMember(*target, "operation_capability_id")},
        {"action_span", CanonicalValue(nullptr)},
        {"payload", std::move(payload)}};
    if (automatic_retry_context)
      participant.emplace_back("automatic_retry_context",
                               *automatic_retry_context);
    CanonicalValue operation(CanonicalValue::Object{
        {"request_context",
         CanonicalValue(CanonicalValue::Object{
             {"request_id", CanonicalValue(request_id)},
             {"request_owner_epoch", RequiredMember(identity, "owner_epoch")},
             {"source_label", CanonicalValue("manager_ros_apply_fixture")},
             {"correlation_id", CanonicalValue(nullptr)}})},
        {"program_id", RequiredMember(manual, "program_id")},
        {"program_revision", RequiredMember(manual, "program_revision")},
        {"program_binding_id", RequiredMember(manual, "program_binding_id")},
        {"program_run_id", RequiredMember(manual, "program_run_id")},
        {"expected_graph_revision", RequiredMember(identity, "graph_revision")},
        {"expected_retained_bundle_id",
         RequiredMember(identity, "retained_bundle_id")},
        {"expected_scene_revision", CanonicalValue(nullptr)},
        {"participants", CanonicalValue(CanonicalValue::Array{
                             CanonicalValue(std::move(participant))})},
        {"coordination_policy", CanonicalValue("independent")}});
    const auto submitted_motion = SendCommand(
        directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue("manyforge.execution_command.v3")},
            {"command", CanonicalValue("submit")},
            {"operation", std::move(operation)},
            {"attribution", CanonicalValue(CanonicalValue::Object{
                                {"assurance", CanonicalValue("unverified")},
                                {"principal_id", CanonicalValue(nullptr)},
                                {"transport", CanonicalValue("http")}})}})));
    return deployment::parse_strict_json_model_yaml(submitted_motion).root();
  };

  // Runs one component action and waits for its result in the component's
  // state (`202`-style acceptance, then `last_action`).
  const auto run_action = [&](const std::string &action,
                              const std::string &component_id) {
    const auto accepted = control_command(
        {{"command", CanonicalValue("component_action")},
         {"request", CanonicalValue(CanonicalValue::Object{
                         {"action", CanonicalValue(action)},
                         {"component_id", CanonicalValue(component_id)}})}});
    const auto started =
        RequiredMember(RequiredMember(accepted, "accepted"), "started_at_ns");
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (std::chrono::steady_clock::now() < deadline) {
      const auto state = read_component(component_id);
      const auto *last = state.find_object_member("last_action");
      if (state.find_object_member("action") == nullptr && last != nullptr &&
          RequiredText(*last, "action") == action &&
          RequiredMember(*last, "started_at_ns") == started) {
        if (RequiredText(*last, "outcome") != "succeeded")
          throw std::runtime_error("Component action did not succeed: " +
                                   content_identity::canonical_json(state));
        return state;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    throw std::runtime_error("Component action did not finish: " + action);
  };
  EXPECT_EQ(RequiredText(run_action("prepare", owner), "lifecycle"), "prepared");
  EXPECT_EQ(RequiredText(run_action("prepare", component), "lifecycle"), "prepared");
  EXPECT_EQ(RequiredText(run_action("prepare", gripper_component), "lifecycle"), "prepared");
  // Prepared: both controllers configured and inactive.
  observe_ros("inactive", "after-prepare");
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"),
            CanonicalValue(false));

  // A prepared component takes no command.
  const auto refused_motion = submit_current_joint_hold("empty-ros-one-mock.prepared-motion");
  ASSERT_EQ(RequiredMember(refused_motion, "ok"), CanonicalValue(false))
      << content_identity::canonical_json(refused_motion);

  EXPECT_EQ(RequiredText(run_action("enable", component), "lifecycle"), "enabled");
  EXPECT_EQ(RequiredText(run_action("enable", gripper_component), "lifecycle"), "enabled");
  observe_ros("active", "after-enable");
  const auto ready_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  CanonicalValue command_gate(nullptr);
  do {
    command_gate = read_command_gate();
    if (RequiredMember(command_gate, "command_eligible") ==
        CanonicalValue(true))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < ready_deadline);
  EXPECT_EQ(RequiredMember(command_gate, "command_eligible"),
            CanonicalValue(true));
  // These private commands require the genuine managed cell above. The
  // standalone CLI cannot construct this stable-cell owner relationship.
  const auto execution_command = [&](CanonicalValue::Object request) {
    const auto bytes = SendCommand(directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(std::move(request))));
    return deployment::parse_strict_json_model_yaml(bytes).root();
  };
  const CanonicalValue::Object capability_request{
      {"api_version", CanonicalValue("manyforge.execution_command.v3")},
      {"command", CanonicalValue("reset_robot_position_capability")}};
  const auto capability = execution_command(capability_request);
  ASSERT_EQ(capability.as_object().size(), 5U);
  ASSERT_EQ(RequiredMember(capability, "ok"), CanonicalValue(true));
  EXPECT_EQ(RequiredText(capability, "api_version"), "manyforge.execution_command_response.v4");
  EXPECT_EQ(RequiredText(capability, "command"), "reset_robot_position_capability");
  const auto &position_capability = RequiredMember(capability, "reset_robot_position");
  ASSERT_EQ(position_capability.as_object().size(), 2U);
  EXPECT_EQ(RequiredMember(position_capability, "available"), CanonicalValue(true));
  EXPECT_EQ(RequiredMember(position_capability, "reason"), CanonicalValue(nullptr));
  const auto &position_identity = RequiredMember(capability, "identity");
  EXPECT_EQ(RequiredText(position_identity, "schema_version"), "manyforge.execution_identity.v2");
  const CanonicalValue::Object reset_authorization{
      {"program_runner_owner_epoch", CanonicalValue("absent.runner")},
      {"cycle_generation", CanonicalValue(1.0)},
      {"request_owner_epoch", CanonicalValue("absent.request-owner")},
      {"request_id", CanonicalValue("absent.reset")},
      {"request_payload_digest", CanonicalValue("sha256:" + std::string(64U, '1'))},
      {"program_run_id", CanonicalValue(nullptr)},
      {"cycle_claim_generation", CanonicalValue(nullptr)},
      {"terminal_or_settlement_reference", CanonicalValue(nullptr)},
      {"reset_claim_generation", CanonicalValue(1.0)}};
  CanonicalValue::Object expected_deployment;
  for (const auto *key : {"owner_epoch", "active_publication_generation", "retained_bundle_id", "graph_revision"})
    expected_deployment.emplace_back(key, RequiredMember(position_identity, key));
  const auto reset_command = [](CanonicalValue::Object authorization, CanonicalValue::Object expected) {
    return CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.execution_command.v3")},
        {"command", CanonicalValue("reset_robot_position")},
        {"authorization", CanonicalValue(std::move(authorization))},
        {"expected_deployment_identity", CanonicalValue(std::move(expected))}};
  };
  const auto refused_reset = execution_command(reset_command(reset_authorization, expected_deployment));
  ASSERT_EQ(RequiredMember(refused_reset, "ok"), CanonicalValue(false));
  const auto &reset_error = RequiredMember(refused_reset, "error");
  EXPECT_EQ(RequiredText(reset_error, "code"), "authoring_not_ready");
  const auto &reset_details = RequiredMember(reset_error, "details");
  EXPECT_EQ(RequiredText(reset_details, "reason"), "program_reset_claim_not_exact");
  EXPECT_EQ(RequiredText(reset_details, "effect_disposition"), "known_no_effect");
  std::vector<CanonicalValue::Object> malformed_reset_commands;
  auto extra_authorization = reset_authorization;
  extra_authorization.emplace_back("trusted", CanonicalValue(true));
  malformed_reset_commands.push_back(reset_command(std::move(extra_authorization), expected_deployment));
  auto caller_targets = expected_deployment;
  caller_targets.emplace_back("joint_positions_rad", CanonicalValue(CanonicalValue::Array{CanonicalValue(0.0)}));
  malformed_reset_commands.push_back(reset_command(reset_authorization, std::move(caller_targets)));
  for (const auto *field : {"cycle_claim_generation", "reset_claim_generation"}) {
    auto wrong_claim = reset_authorization;
    for (auto &[key, value] : wrong_claim)
      if (key == field)
        value = key == "cycle_claim_generation" ? CanonicalValue(1.0) : CanonicalValue(nullptr);
    malformed_reset_commands.push_back(reset_command(std::move(wrong_claim), expected_deployment));
  }
  auto extra_capability = capability_request;
  extra_capability.emplace_back("unexpected", CanonicalValue(true));
  malformed_reset_commands.push_back(std::move(extra_capability));
  for (auto &request : malformed_reset_commands) {
    const auto invalid = execution_command(std::move(request));
    ASSERT_EQ(RequiredMember(invalid, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(invalid, "error"), "code"), "execution_command_invalid");
  }
  // Submit, rather than preview, consumes the real current route/readiness,
  // ordinary manual Cell claim and configured resource admission gate.
  static_cast<void>(WaitForAuthoringAdmission(directory));
  const auto retry_context = [](CanonicalValue remaining) {
    return CanonicalValue(
        CanonicalValue::Object{{"remaining_retries", std::move(remaining)}});
  };
  // The abandoned retry-policy field is refused even when its old count
  // would have been valid. Program policy is not a native operation input.
  for (const auto remaining : {1.0, 10.0}) {
    const auto refused = submit_current_joint_hold(
        "retry-context-no-program-" + std::to_string(remaining),
        retry_context(CanonicalValue(remaining)));
    ASSERT_EQ(RequiredMember(refused, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(refused, "error"), "code"),
              "execution_command_invalid")
        << content_identity::canonical_json(refused);
  }
  std::vector<CanonicalValue> malformed_retry_contexts;
  for (const auto &remaining :
       {CanonicalValue(0.0), CanonicalValue(11.0), CanonicalValue(1.5),
        CanonicalValue(true), CanonicalValue("1"), CanonicalValue(nullptr)})
    malformed_retry_contexts.push_back(retry_context(remaining));
  malformed_retry_contexts.emplace_back(nullptr);
  malformed_retry_contexts.emplace_back(CanonicalValue::Object{});
  malformed_retry_contexts.emplace_back(
      CanonicalValue::Object{{"remaining_retries", CanonicalValue(1.0)},
                             {"unknown_field", CanonicalValue(false)}});
  malformed_retry_contexts.emplace_back(
      CanonicalValue::Object{{"unknown_field", CanonicalValue(1.0)}});
  for (std::size_t index = 0; index < malformed_retry_contexts.size();
       ++index) {
    const auto refused = submit_current_joint_hold(
        "malformed-retry-context-" + std::to_string(index),
        malformed_retry_contexts[index]);
    ASSERT_EQ(RequiredMember(refused, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(refused, "error"), "code"),
              "execution_command_invalid")
        << content_identity::canonical_json(refused);
  }
  const auto non_motion_context = submit_current_joint_hold(
      "retry-context-non-motion", retry_context(CanonicalValue(1.0)), true);
  ASSERT_EQ(RequiredMember(non_motion_context, "ok"), CanonicalValue(false));
  EXPECT_EQ(RequiredText(RequiredMember(non_motion_context, "error"), "code"),
            "execution_command_invalid");
  const auto accepted_motion =
      submit_current_joint_hold("empty-ros-one-mock.active-motion");
  ASSERT_EQ(RequiredMember(accepted_motion, "ok"), CanonicalValue(true))
      << content_identity::canonical_json(accepted_motion);
  const auto operation_id =
      RequiredText(RequiredMember(accepted_motion, "accepted"), "operation_id");
  const auto motion_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  CanonicalValue actual_motion(nullptr);
  while (std::chrono::steady_clock::now() < motion_deadline) {
    const auto bytes = SendCommand(directory.execution_socket_path(),
        content_identity::canonical_json(CanonicalValue(CanonicalValue::Object{
            {"api_version", CanonicalValue("manyforge.execution_command.v3")},
            {"command", CanonicalValue("read")}, {"operation_id", CanonicalValue(operation_id)}})));
    const auto actual = deployment::parse_strict_json_model_yaml(bytes).root();
    ASSERT_EQ(RequiredMember(actual, "ok"), CanonicalValue(true)) << bytes;
    actual_motion = RequiredMember(actual, "operation");
    const auto state = RequiredText(actual_motion, "state");
    if (state == "completed" || state == "failed" || state == "intervention_required" ||
        state == "stopped" || state == "cancelled_pre_dispatch") break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_TRUE(actual_motion.is_object());
  ASSERT_EQ(RequiredText(actual_motion, "state"), "completed")
      << content_identity::canonical_json(actual_motion);
  const auto &motion_participants = RequiredMember(actual_motion, "participants").as_array();
  ASSERT_EQ(motion_participants.size(), 1U);
  const auto &motion_participant = motion_participants.front();
  EXPECT_EQ(actual_motion.find_object_member("motion_retry_candidate"), nullptr);
  EXPECT_EQ(actual_motion.find_object_member("motion_retry_disposition"), nullptr);
  EXPECT_EQ(motion_participant.find_object_member("automatic_retry_context"), nullptr);
  EXPECT_EQ(motion_participant.find_object_member("execution_scope"), nullptr);
  EXPECT_EQ(RequiredText(motion_participant, "controllable_component_id"), component);
  EXPECT_EQ(RequiredText(motion_participant, "state"), "completed");
  EXPECT_EQ(RequiredMember(motion_participant, "terminal_error"),
            CanonicalValue(nullptr));
  const auto arm = std::find_if(
      candidate->value().cell_graph.control_bindings.begin(),
      candidate->value().cell_graph.control_bindings.end(),
      [](const auto &binding) {
        return binding.capability_id.value() == "execution.trajectory.execute";
      });
  ASSERT_NE(arm, candidate->value().cell_graph.control_bindings.end());
  EXPECT_EQ(RequiredText(motion_participant, "control_binding_id"), arm->control_binding_id.value());
  EXPECT_EQ(RequiredText(motion_participant, "control_domain_id"), arm->control_domain_id.value());

  // These commands reach the actual private decoder and stable Kernel. This
  // product has no sealed signals, so valid shapes refuse without registration
  // or effects; malformed counter/condition shapes must stop at the decoder.
  // The applied Program's blackboard installs the I/O signal service, so a
  // forged watch token is refused as not current (io_watch_generation_stale).
  const CanonicalValue io_attribution(CanonicalValue::Object{
      {"transport", CanonicalValue("internal")},
      {"assurance", CanonicalValue("unverified")},
      {"principal_id", CanonicalValue(nullptr)}});
  const auto io_command = [&](const std::string &name, const std::string &field,
                              CanonicalValue payload) {
    return deployment::parse_strict_json_model_yaml(
               SendCommand(
                   directory.execution_socket_path(),
                   content_identity::canonical_json(
                       CanonicalValue(CanonicalValue::Object{
                           {"api_version",
                            CanonicalValue("manyforge.execution_command.v3")},
                           {"command", CanonicalValue(name)},
                           {"attribution", io_attribution},
                           {field, std::move(payload)}}))))
        .root();
  };
  // The target is absent, so a valid Reset reaches Kernel validation without
  // an output write.
  CanonicalValue::Object reset_request{
      {"request_context",
       CanonicalValue(CanonicalValue::Object{
           {"request_id", CanonicalValue("reset.output-ack.decoder")},
           {"request_owner_epoch", CanonicalValue(event_owner_epoch)},
           {"source_label", CanonicalValue("native-output-ack-test")},
           {"correlation_id", CanonicalValue(nullptr)}})},
      {"operation_id", CanonicalValue("operation.absent-output")},
      {"expected_operation_revision", CanonicalValue(1.0)},
      {"reason", CanonicalValue(nullptr)}};
  const auto decoded =
      io_command("reset", "request", CanonicalValue(reset_request));
  ASSERT_EQ(RequiredMember(decoded, "ok"), CanonicalValue(false));
  EXPECT_NE(RequiredText(RequiredMember(decoded, "error"), "code"),
            "execution_command_invalid");

  CanonicalValue::Object io_token{
      {"owner_epoch", CanonicalValue(event_owner_epoch)},
      {"catalog_generation", CanonicalValue("18446744073709551615")},
      {"registration_id", CanonicalValue("18446744073709551615")},
      {"scope",
       CanonicalValue(CanonicalValue::Object{
           {"program_runner_owner_epoch", CanonicalValue("runner.io-wire")},
           {"program_run_id", CanonicalValue("run.io-wire")},
           {"node",
            CanonicalValue(CanonicalValue::Object{
                {"node_instance_id", CanonicalValue("io_wait")},
                {"node_attempt_id", CanonicalValue("attempt.io-wire")}})}})}};
  for (const auto &command : {"read_io_watch", "release_io_watch"}) {
    const auto refused =
        io_command(command, "registration", CanonicalValue(io_token));
    ASSERT_EQ(RequiredMember(refused, "ok"), CanonicalValue(false));
    EXPECT_EQ(
        RequiredText(RequiredMember(RequiredMember(refused, "error"), "details"),
                     "reason"),
        "io_watch_generation_stale")
        << content_identity::canonical_json(refused);
    for (const auto &bad :
         {CanonicalValue(1.0), CanonicalValue("01"),
          CanonicalValue("18446744073709551616"), CanonicalValue("0")}) {
      auto malformed = io_token;
      for (auto &[key, value] : malformed)
        if (key == "registration_id")
          value = bad;
      const auto invalid = io_command(command, "registration",
                                      CanonicalValue(std::move(malformed)));
      EXPECT_EQ(RequiredText(RequiredMember(invalid, "error"), "code"),
                "execution_command_invalid");
    }
  }
  CanonicalValue::Object io_request{
      {"schema_version", CanonicalValue("manyforge.io_watch_request.v2")},
      {"owner_epoch", CanonicalValue(event_owner_epoch)},
      {"program_binding_id", CanonicalValue("binding.io-wire")},
      {"program_runner_owner_epoch", CanonicalValue("runner.io-wire")},
      {"claim_generation", CanonicalValue(1.0)},
      {"program_run_id", CanonicalValue("run.io-wire")},
      {"node_instance_id", CanonicalValue("io_wait")},
      {"node_attempt_id", CanonicalValue("attempt.io-wire")},
      {"signal", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue("io_target")},
          {"signal_name", CanonicalValue("ready_input")}})},
      {"condition", CanonicalValue(CanonicalValue::Object{
                        {"kind", CanonicalValue("level")},
                        {"expected_value", CanonicalValue(false)}})}};
  const auto unscoped_watch =
      io_command("register_io_watch", "request", CanonicalValue(io_request));
  EXPECT_EQ(RequiredText(RequiredMember(RequiredMember(unscoped_watch, "error"),
                                        "details"),
                         "reason"),
            "io_watch_scope_stale");
  CanonicalValue::Object io_scope_release{
      {"schema_version", CanonicalValue("manyforge.io_watch_scope_release.v1")},
      {"owner_epoch", CanonicalValue(event_owner_epoch)},
      {"program_binding_id", CanonicalValue("binding.io-wire")},
      {"program_runner_owner_epoch", CanonicalValue("runner.io-wire")},
      {"claim_generation", CanonicalValue(1.0)},
      {"program_run_id", CanonicalValue("run.io-wire")},
      {"node_instance_id", CanonicalValue("io_wait")},
      {"node_attempt_id", CanonicalValue("attempt.io-wire")}};
  const auto stale_scope_release = io_command(
      "release_io_watch", "scope_request", CanonicalValue(io_scope_release));
  EXPECT_EQ(
      RequiredText(RequiredMember(RequiredMember(stale_scope_release, "error"),
                                  "details"),
                   "reason"),
      "io_watch_scope_stale");
  const auto read_scope_refused = io_command("read_io_watch", "scope_request",
                                             CanonicalValue(io_scope_release));
  EXPECT_EQ(RequiredText(RequiredMember(read_scope_refused, "error"), "code"),
            "execution_command_invalid");
  io_scope_release.emplace_back("unexpected", CanonicalValue(false));
  const auto malformed_scope = io_command("release_io_watch", "scope_request",
                                          CanonicalValue(io_scope_release));
  EXPECT_EQ(RequiredText(RequiredMember(malformed_scope, "error"), "code"),
            "execution_command_invalid");

  // Valid output values pass the decoder but this invented Program binding has
  // no captured node origin. The existing origin gate refuses before Kernel
  // owner validation or any provider effect.
  const auto submit_output_value = [&](CanonicalValue command_value,
                                       std::optional<CanonicalValue>
                                           caller_timeout = std::nullopt) {
    CanonicalValue::Object payload{
        {"kind", CanonicalValue("capability_command")},
        {"command_schema_id", CanonicalValue("io-write-schema")},
        {"command_schema_revision", CanonicalValue("io-write-schema-revision")},
        {"command_value", std::move(command_value)},
        {"caller_options", CanonicalValue(CanonicalValue::Array{})}};
    if (caller_timeout)
      payload.emplace_back("completion_timeout_s", *caller_timeout);
    return io_command(
        "submit", "operation",
        CanonicalValue(CanonicalValue::Object{
            {"request_context",
             CanonicalValue(CanonicalValue::Object{
                 {"request_id", CanonicalValue("io-write-wire")},
                 {"request_owner_epoch", CanonicalValue("owner.output.stale")},
                 {"source_label", CanonicalValue("io-write-wire")},
                 {"correlation_id", CanonicalValue(nullptr)}})},
            {"program_id", CanonicalValue("program.io-write-wire")},
            {"program_revision", CanonicalValue("revision.io-write-wire")},
            {"program_binding_id", CanonicalValue("binding.io-write-wire")},
            {"program_run_id", CanonicalValue("run.io-write-wire")},
            {"expected_graph_revision", CanonicalValue("graph.io-write-wire")},
            {"expected_retained_bundle_id",
             CanonicalValue("bundle.io-write-wire")},
            {"expected_scene_revision", CanonicalValue(nullptr)},
            {"participants",
             CanonicalValue(
                 CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
                     {"target", CanonicalValue(CanonicalValue::Object{
                         {"component_id", CanonicalValue("io_device")},
                         {"command_target_id", CanonicalValue("outputs")}})},
                     {"required_capability_id",
                      CanonicalValue("io.output.command")},
                     {"action_span", CanonicalValue(nullptr)},
                     {"payload", CanonicalValue(std::move(payload))}})})},
            {"coordination_policy", CanonicalValue("independent")}}));
  };
  for (const bool value : {false, true}) {
    const auto stale_output =
        submit_output_value(CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("io_signal_write")},
            {"resource_role_id", CanonicalValue("valve_enable")},
            {"value", CanonicalValue(value)}}));
    // Output writes are no longer operations (one write path through
    // `POST /api/io/{signal_id}/set`): the `io_signal_write` command value is
    // refused at the decoder.
    ASSERT_EQ(RequiredMember(stale_output, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(stale_output, "error"), "code"),
              "execution_command_invalid")
        << content_identity::canonical_json(stale_output);
  }
  for (const auto &value : {CanonicalValue(nullptr), CanonicalValue(0.0),
                            CanonicalValue("false")}) {
    const auto malformed_output =
        submit_output_value(CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("io_signal_write")},
            {"resource_role_id", CanonicalValue("valve_enable")},
            {"value", value}}));
    EXPECT_EQ(RequiredText(RequiredMember(malformed_output, "error"), "code"),
              "execution_command_invalid");
  }

  for (const auto &timeout : {CanonicalValue(1.0), CanonicalValue(nullptr)}) {
    const auto forbidden = submit_output_value(
        CanonicalValue(CanonicalValue::Object{
            {"kind", CanonicalValue("io_signal_write")},
            {"resource_role_id", CanonicalValue("valve_enable")},
            {"value", CanonicalValue(false)}}),
        timeout);
    EXPECT_EQ(RequiredText(RequiredMember(forbidden, "error"), "code"),
              "execution_command_invalid");
  }
  const auto gripper = CanonicalValue(CanonicalValue::Object{
      {"kind", CanonicalValue("gripper_position")},
      {"position", CanonicalValue(CanonicalValue::Object{
                       {"kind", CanonicalValue("angular_rad")},
                       {"value_rad", CanonicalValue(0.1)}})},
      {"max_effort", CanonicalValue(nullptr)}});
  const auto gripper_without_timeout = submit_output_value(gripper);
  EXPECT_EQ(
      RequiredText(RequiredMember(gripper_without_timeout, "error"), "code"),
      "execution_command_invalid");
  const auto gripper_with_timeout =
      submit_output_value(gripper, CanonicalValue(1.0));
  EXPECT_EQ(RequiredText(RequiredMember(gripper_with_timeout, "error"), "code"),
            "invalid_request");
  EXPECT_EQ(RequiredText(RequiredMember(RequiredMember(gripper_with_timeout, "error"),
                                       "details"),
                         "reason"),
            "program_execution_scope_missing");

  for (auto &[key, value] : io_request)
    if (key == "condition")
      value = CanonicalValue(
          CanonicalValue::Object{{"kind", CanonicalValue("rising_edge")},
                                 {"expected_value", CanonicalValue(false)}});
  const auto unsupported =
      io_command("register_io_watch", "request", CanonicalValue(io_request));
  EXPECT_EQ(RequiredText(RequiredMember(unsupported, "error"), "code"),
            "io_watch_condition_unsupported");
  io_request.emplace_back("unknown", CanonicalValue(false));
  const auto malformed_watch =
      io_command("register_io_watch", "request", CanonicalValue(io_request));
  EXPECT_EQ(RequiredText(RequiredMember(malformed_watch, "error"), "code"),
            "execution_command_invalid");

  CanonicalValue::Object io_query{
      {"schema_version", CanonicalValue("manyforge.target_observation_query.v2")},
      {"owner_epoch", CanonicalValue(event_owner_epoch)},
      {"program_binding_id", CanonicalValue("binding.io-wire")},
      {"kind", CanonicalValue("io_signal")},
      {"signal", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue("io_target")},
          {"signal_name", CanonicalValue("ready_input")}})}};
  const auto query_io = [&](CanonicalValue query) {
    return deployment::parse_strict_json_model_yaml(
               SendCommand(
                   directory.execution_socket_path(),
                   content_identity::canonical_json(
                       CanonicalValue(CanonicalValue::Object{
                           {"api_version",
                            CanonicalValue("manyforge.execution_command.v3")},
                           {"command", CanonicalValue("query_target")},
                           {"query", std::move(query)}}))))
        .root();
  };
  // The service is installed (the applied Program's blackboard); this
  // invented signal is bound to nothing in it.
  const auto missing_service = query_io(CanonicalValue(io_query));
  EXPECT_EQ(RequiredText(RequiredMember(missing_service, "error"), "code"),
            "io_signal_unbound");
  CanonicalValue::Object mock_set{
      {"schema_version", CanonicalValue("manyforge.mock_io_input_request.v2")},
      {"owner_epoch", CanonicalValue(event_owner_epoch)},
      {"program_binding_id", CanonicalValue("binding.io-wire")},
      {"signal", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue("io_target")},
          {"signal_name", CanonicalValue("ready_input")}})},
      {"expected_source_epoch", CanonicalValue("installation.io-wire")},
      {"value", CanonicalValue(false)}};
  const auto missing_mock =
      io_command("set_mock_io_input", "request", CanonicalValue(mock_set));
  EXPECT_EQ(RequiredText(RequiredMember(missing_mock, "error"), "code"),
            "io_signal_unbound");
  for (auto &[key, value] : mock_set)
    if (key == "value")
      value = CanonicalValue("false");
  const auto malformed_mock =
      io_command("set_mock_io_input", "request", CanonicalValue(mock_set));
  EXPECT_EQ(RequiredText(RequiredMember(malformed_mock, "error"), "code"),
            "execution_command_invalid");
  io_query.emplace_back("coordinate_role_id", CanonicalValue("fake_axis"));
  const auto mixed_coordinate = query_io(CanonicalValue(std::move(io_query)));
  EXPECT_EQ(RequiredText(RequiredMember(mixed_coordinate, "error"), "code"),
            "execution_command_invalid");
  // The owner automatically acknowledges clean completed manual work.
  // Wait for its actual retirement, rather than racing that owner for a
  // recovery assessment; a completed manual move needs no Program Reset.
  const auto retired_deadline =
      std::chrono::steady_clock::now() + std::chrono::seconds(20);
  CanonicalValue retired_motion(nullptr);
  do {
    retired_motion = execution_command(CanonicalValue::Object{
        {"api_version", CanonicalValue("manyforge.execution_command.v3")},
        {"command", CanonicalValue("read")},
        {"operation_id", CanonicalValue(operation_id)}});
    ASSERT_EQ(RequiredMember(retired_motion, "ok"), CanonicalValue(true));
    if (RequiredMember(retired_motion, "resident") == CanonicalValue(false))
      break;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  } while (std::chrono::steady_clock::now() < retired_deadline);
  ASSERT_EQ(RequiredMember(retired_motion, "resident"), CanonicalValue(false));
  EXPECT_EQ(RequiredText(RequiredMember(retired_motion, "operation"), "state"),
            "completed");
  const auto read_recovery = [&] {
    return deployment::parse_strict_json_model_yaml(
               SendCommand(
                   directory.execution_socket_path(),
                   content_identity::canonical_json(
                       CanonicalValue(CanonicalValue::Object{
                           {"api_version",
                            CanonicalValue("manyforge.execution_command.v3")},
                           {"command",
                            CanonicalValue("get_operation_recovery")},
                           {"operation_id", CanonicalValue(operation_id)}}))))
        .root();
  };
  const auto recovery = read_recovery();
  ASSERT_EQ(RequiredMember(recovery, "ok"), CanonicalValue(false))
      << content_identity::canonical_json(recovery);
  EXPECT_EQ(
      RequiredText(RequiredMember(RequiredMember(recovery, "error"), "details"),
                   "reason"),
      "operation_recovery_unavailable");
  // These inert fences exercise wire validation only. No resident owner can
  // authorize an effect. Live-owner replay/fence behavior has focused Core
  // tests.
  CanonicalValue::Object recovery_request{
      {"request_id", CanonicalValue("managed-recovery-wire")},
      {"action", CanonicalValue("reset_cycle")},
      {"principal_id", CanonicalValue("operator.recovery-wire-test")},
      {"expected_assessment_revision", CanonicalValue(1.0)},
      {"expected_control_command_epoch", CanonicalValue(0.0)},
      {"expected_execution_id", CanonicalValue(1.0)},
      {"expected_stop_certificate_id", CanonicalValue("")},
      {"expected_stop_owner_generation", CanonicalValue(0.0)},
      {"reason", CanonicalValue("wire validation only")}};
  const auto apply_recovery = [&](CanonicalValue request_value,
                                  const CanonicalValue &operation_revision) {
    return deployment::parse_strict_json_model_yaml(
               SendCommand(
                   directory.execution_socket_path(),
                   content_identity::canonical_json(
                       CanonicalValue(CanonicalValue::Object{
                           {"api_version",
                            CanonicalValue("manyforge.execution_command.v3")},
                           {"command",
                            CanonicalValue("apply_operation_recovery")},
                           {"expected_operation_revision", operation_revision},
                           {"operation_id", CanonicalValue(operation_id)},
                           {"request", std::move(request_value)}}))))
        .root();
  };

  const CanonicalValue stale_revision(
      RequiredMember(actual_motion, "operation_revision").as_number() + 1.0);
  for (const auto flag : {std::optional<bool>{}, std::optional<bool>{false},
                          std::optional<bool>{true}}) {
    auto request_value = recovery_request;
    if (flag.has_value())
      request_value.emplace_back("fresh_motion_attempt", CanonicalValue(*flag));
    const auto refused = apply_recovery(
        CanonicalValue(std::move(request_value)), stale_revision);
    ASSERT_EQ(RequiredMember(refused, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(
                  RequiredMember(RequiredMember(refused, "error"), "details"),
                  "reason"),
              flag.value_or(false) ? "operation_recovery_unavailable"
                                   : "operation_recovery_stale")
        << content_identity::canonical_json(refused);
  }
  auto final_failure_request = recovery_request;
  std::erase_if(final_failure_request,
                [](const auto &field) { return field.first == "action"; });
  final_failure_request.emplace_back(
      "action", CanonicalValue("report_final_motion_failure"));
  const auto stale_final_failure = apply_recovery(
      CanonicalValue(std::move(final_failure_request)), stale_revision);
  ASSERT_EQ(RequiredMember(stale_final_failure, "ok"), CanonicalValue(false));
  EXPECT_EQ(RequiredText(RequiredMember(stale_final_failure, "error"), "code"),
            "execution_command_invalid")
      << content_identity::canonical_json(stale_final_failure);
  std::vector<CanonicalValue> malformed_requests;
  for (const auto &flag :
       {CanonicalValue(nullptr), CanonicalValue(0.0), CanonicalValue(1.0),
        CanonicalValue("false"), CanonicalValue(CanonicalValue::Array{}),
        CanonicalValue(CanonicalValue::Object{})}) {
    auto request_value = recovery_request;
    request_value.emplace_back("fresh_motion_attempt", flag);
    malformed_requests.emplace_back(std::move(request_value));
  }
  auto unknown_field = recovery_request;
  unknown_field.emplace_back("unknown_field", CanonicalValue(false));
  malformed_requests.emplace_back(std::move(unknown_field));
  auto extra_field = recovery_request;
  extra_field.emplace_back("fresh_motion_attempt", CanonicalValue(false));
  extra_field.emplace_back("unknown_field", CanonicalValue(false));
  malformed_requests.emplace_back(std::move(extra_field));
  auto missing_field = recovery_request;
  std::erase_if(missing_field,
                [](const auto &field) { return field.first == "reason"; });
  missing_field.emplace_back("fresh_motion_attempt", CanonicalValue(true));
  malformed_requests.emplace_back(std::move(missing_field));
  auto replaced_field = recovery_request;
  std::erase_if(replaced_field,
                [](const auto &field) { return field.first == "reason"; });
  replaced_field.emplace_back("unknown_field", CanonicalValue(false));
  malformed_requests.emplace_back(std::move(replaced_field));
  for (const auto &request_value : malformed_requests) {
    const auto refused = apply_recovery(request_value, stale_revision);
    ASSERT_EQ(RequiredMember(refused, "ok"), CanonicalValue(false));
    EXPECT_EQ(RequiredText(RequiredMember(refused, "error"), "code"),
              "execution_command_invalid")
        << content_identity::canonical_json(refused);
  }
  EXPECT_EQ(read_recovery(), recovery);
  const auto execution = WaitForExecutionReady(directory, candidate_id);
  EXPECT_FALSE(execution.owner_epoch.empty());
  const auto scene_after = scene_source.Read();
  const auto runtime_after = runtime_source.Read();
  ASSERT_TRUE(scene_after.has_value());
  ASSERT_TRUE(runtime_after.has_value());
  EXPECT_NE(
      content_identity::canonical_json(scene_after->payload).find(candidate_id),
      std::string::npos);
  EXPECT_NE(content_identity::canonical_json(runtime_after->payload)
                .find(candidate_id),
            std::string::npos);
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "objects"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "objects"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "carried_object_attachments"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "carried_object_attachments"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "snapshot"),
                           "revision"),
            RequiredMember(RequiredMember(scene_before->payload, "snapshot"),
                           "revision"));
  EXPECT_EQ(RequiredMember(RequiredMember(scene_after->payload, "access"),
                           "projection_leases_open"),
            CanonicalValue(true));
  EXPECT_EQ(RequiredText(
                RequiredMember(RequiredMember(scene_after->payload, "access"),
                               "active_projection"),
                "retained_bundle_id"),
            candidate_id);
  EXPECT_EQ(RequiredText(run_action("disable", component), "lifecycle"), "prepared");
  EXPECT_EQ(RequiredText(run_action("disable", gripper_component), "lifecycle"), "prepared");
  observe_ros("inactive", "after-disable");
  EXPECT_EQ(RequiredMember(read_command_gate(), "command_eligible"), CanonicalValue(false));

  // Exact config replay does not introduce activation or child work.
  EXPECT_NE(SendCommand(directory.socket_path(), request)
                .find("\"replay_status\":\"replayed_exact\""),
            std::string::npos);
  const auto replay_inventory = journal.ReadStatusInventory();
  ASSERT_EQ(replay_inventory.size(), 1U);

  // Let the actual Manager child owner settle normal runtime shutdown before
  // the successful fixture removes the controller launch artifact directory.
  const auto stopped = manager.Stop(SIGTERM);
  ASSERT_TRUE(WIFEXITED(stopped));
  EXPECT_EQ(WEXITSTATUS(stopped), 0);
  EXPECT_FALSE(std::filesystem::exists(directory.socket_path()));
  EXPECT_FALSE(std::filesystem::exists(directory.execution_socket_path()));
#endif
}

} // namespace
} // namespace manyforge::headless

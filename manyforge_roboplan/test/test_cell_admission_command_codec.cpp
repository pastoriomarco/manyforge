#include "cell_admission_command_codec.hpp"

#include <gtest/gtest.h>
#include <manyforge_content_identity/content_identity.hpp>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <optional>
#include <cerrno>
#include <string>
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend::detail {
namespace {
namespace cm = manyforge::component_manager::detail;
namespace ci = manyforge::content_identity;
namespace core = manyforge::core;
using Value = ci::CanonicalValue;

constexpr std::string_view kDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

Value Envelope(std::string command, Value request) {
  return Value(Value::Object{
      {"api_version", Value(std::string(kCellAdmissionCommandApiVersion))},
      {"command", Value(std::move(command))},
      {"request", std::move(request)}});
}
Value Readiness() {
  return Value(Value::Object{
      {"blocking_fault_count", Value(0.0)},
      {"deployment_converged", Value(true)},
      {"deployment_status_current", Value(true)},
      {"program_cycle_status", Value(nullptr)},
      {"program_cycle_status_current", Value(true)},
      {"required_component_current_compatible_ready_converged", Value(1.0)},
      {"required_component_status_current", Value(true)},
      {"required_component_total", Value(1.0)},
      {"unresolved_owner_count", Value(0.0)},
      {"unsettled_effect_count", Value(0.0)}});
}
Value Intent() {
  return Value(Value::Object{
      {"claim_kind", Value("authoring_mutation")},
      {"expected_admission_generation", Value(1.0)},
      {"intervention_evidence", Value(nullptr)},
      {"owner_epoch", Value("owner-1")},
      {"owner_role", Value("authoring_owner")},
      {"owner_transaction_id", Value("transaction-1")},
      {"program_cycle_evidence", Value(nullptr)},
      {"request_id", Value("request-1")},
      {"request_payload_digest", Value(std::string(kDigest))},
      {"scope", Value(Value::Object{{"configurations", Value(Value::Array{})},
                                     {"resources", Value(Value::Array{})}})}});
}

Value ResetReadiness(Value accepted) {
  auto fields = Readiness().as_object();
  for (auto& [name, value] : fields) {
    if (name != "program_cycle_status") continue;
    value = Value(Value::Object{
        {"program_runner_owner_epoch", Value("runner.1")},
        {"cycle_generation", Value(3.0)}, {"state", Value("resetting")},
        {"program_run_id", Value(nullptr)}, {"cycle_claim_generation", Value(nullptr)},
        {"terminal_or_settlement_reference", Value(nullptr)},
        {"accepted_reset", std::move(accepted)}});
  }
  return Value(std::move(fields));
}
Value Release() {
  return Value(Value::Object{
      {"acquired_admission_generation", Value(1.0)},
      {"claim_generation", Value(1.0)},
      {"owner_epoch", Value("owner-1")},
      {"owner_role", Value("authoring_owner")},
      {"owner_transaction_id", Value("transaction-1")},
      {"request_id", Value("release-1")},
      {"request_payload_digest", Value(std::string(kDigest))},
      {"settlement",
       Value(Value::Object{{"evidence_digest", Value(std::string(kDigest))},
                           {"settlement_kind", Value("owner_commit")}})}});
}
Value Reconcile() {
  return Value(Value::Object{
      {"current_owner_epochs", Value(Value::Array{})},
      {"kernel_owner_epoch", Value("kernel-1")},
      {"reconciliation_evidence_digest", Value(std::string(kDigest))},
      {"retained_claims", Value(Value::Array{})}});
}
Value RecoveryQuery() {
  return Value(Value::Object{
      {"expected_claim", Value(nullptr)},
      {"expected_release", Value(nullptr)},
      {"intent", Intent()}});
}

class RecordingKernel final : public core::IKernelCellAdmissionPort {
public:
  core::AuthoringAdmissionDTO GetAuthoringAdmission() const override {
    ++calls;
    return {true, core::AdmissionGeneration(7U)};
  }
  core::Result<core::AuthoringAdmissionDTO> UpdateCellAdmissionReadiness(
      const core::CellAdmissionReadinessSnapshotDTO &) override {
    ++calls;
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        {true, core::AdmissionGeneration(8U)});
  }
  core::Result<core::CellAdmissionClaimDTO> AcquireCellAdmissionClaim(
      const core::CellAdmissionClaimIntentDTO &intent) override {
    ++calls;
    ++acquire_calls;
    if (acquire_calls >= 3U)
      return core::Result<core::CellAdmissionClaimDTO>::Err(
          core::ErrorDTO{"stale_generation", "expected_generation_stale",
                         {{"admission_generation", "8"}}});
    return core::Result<core::CellAdmissionClaimDTO>::Ok(
        {intent, core::CellAdmissionClaimGeneration(3U),
         core::AdmissionGeneration(8U), 42});
  }
  core::Result<core::AuthoringAdmissionDTO> ReleaseCellAdmissionClaim(
      const core::CellAdmissionClaimReleaseDTO &) override {
    ++calls;
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        {true, core::AdmissionGeneration(9U)});
  }
  core::Result<core::AuthoringAdmissionDTO> ReconcileCellAdmissionClaims(
      const core::CellAdmissionReconcileDTO &) override {
    ++calls;
    return core::Result<core::AuthoringAdmissionDTO>::Ok(
        {true, core::AdmissionGeneration(10U)});
  }
  core::Result<core::CellAdmissionRecoveryReceiptDTO>
  GetCellAdmissionRecoveryReceipt(
      const core::CellAdmissionRecoveryReceiptQueryDTO &query) const override {
    ++calls;
    core::CellAdmissionClaimDTO claim{
        query.intent, core::CellAdmissionClaimGeneration(3U),
        query.intent.expected_admission_generation, 42};
    return core::Result<core::CellAdmissionRecoveryReceiptDTO>::Ok(
        {core::CellAdmissionRecoveryReceiptStatusDTO::kHeldExact,
         core::RequestOwnerEpoch("kernel-1"), core::AdmissionGeneration(8U),
         claim, std::nullopt, std::nullopt, {claim},
         {{query.intent.owner_role, query.intent.owner_epoch}}});
  }
  core::Result<core::OperatorAlarmAttentionAcceptedDTO>
  PublishOperatorAlarmAttention(
      const core::OperatorAlarmAttentionSnapshotDTO &snapshot) override {
    ++calls;
    attention = snapshot;
    return core::Result<core::OperatorAlarmAttentionAcceptedDTO>::Ok(
        {snapshot.owner_epoch, snapshot.generation});
  }
  mutable std::size_t calls{};
  std::size_t acquire_calls{};
  std::optional<core::OperatorAlarmAttentionSnapshotDTO> attention;
};

Value AttentionMember(std::string gate, Value::Array resources,
                      std::string subject_kind, std::string subject_id) {
  return Value(Value::Object{
      {"activation_id", Value("activation.1")},
      {"condition_id", Value("condition.1")},
      {"gate", Value(std::move(gate))},
      {"record_revision", Value(2.0)},
      {"subject",
       Value(Value::Object{{"resource_ids", Value(std::move(resources))},
                           {"subject_id", Value(std::move(subject_id))},
                           {"subject_kind", Value(std::move(subject_kind))}})}});
}

Value Attention(Value member,
               Value configuration = Value(Value::Object{
                   {"graph_revision", Value("graph.1")},
                   {"retained_bundle_id", Value("bundle.1")}})) {
  return Value(Value::Object{
      {"active_configuration", std::move(configuration)},
      {"cell_id", Value("cell.test")},
      {"generation", Value(5.0)},
      {"members", Value(Value::Array{std::move(member)})},
      {"owner_epoch", Value("alarm.owner.1")},
      {"owner_host_boot_id", Value("boot.test")},
      {"owner_incarnation_us", Value(42.0)},
      {"reconciled", Value(true)},
      {"schema_version",
       Value(std::string(core::kOperatorAlarmAttentionSchemaVersion))}});
}

TEST(CellAdmissionCommandCodec, DecodesAllSixStrictCommands) {
  RecordingKernel kernel;
  const std::array<Value, 6> commands{
      Value(Value::Object{
          {"api_version",
           Value(std::string(kCellAdmissionCommandApiVersion))},
          {"command", Value("cell_admission_get")}}),
      Envelope("cell_admission_update_readiness", Readiness()),
      Envelope("cell_admission_acquire", Intent()),
      Envelope("cell_admission_release", Release()),
      Envelope("cell_admission_reconcile", Reconcile()),
      Envelope("cell_admission_recovery_receipt", RecoveryQuery())};
  for (const auto &command : commands) {
    const auto decoded =
        TryDecodeCellAdmissionCommand(ci::canonical_json(command));
    ASSERT_TRUE(decoded.has_value());
    const auto response =
        cm::DispatchKernelCellAdmissionRpc(kernel, *decoded);
    EXPECT_FALSE(std::holds_alternative<core::ErrorDTO>(response));
  }
  EXPECT_EQ(kernel.calls, 6U);
}

TEST(CellAdmissionCommandCodec,
     OperatorAlarmAttentionReachesTheKernelPortOnlyOverJson) {
  RecordingKernel kernel;
  const auto decoded = TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope(std::string(cm::kPublishOperatorAlarmAttentionCommand),
               Attention(AttentionMember("condition_interlock",
                                         {Value("robot.joint_1")}, "operation",
                                         "operation.1")))));
  ASSERT_TRUE(decoded.has_value());
  const auto response = cm::DispatchKernelCellAdmissionRpc(kernel, *decoded);
  ASSERT_TRUE(kernel.attention.has_value());
  EXPECT_EQ(kernel.attention->owner_epoch, "alarm.owner.1");
  EXPECT_EQ(kernel.attention->owner_host_boot_id, "boot.test");
  EXPECT_EQ(kernel.attention->owner_incarnation_us, 42U);
  EXPECT_EQ(kernel.attention->generation, 5U);
  ASSERT_EQ(kernel.attention->members.size(), 1U);
  EXPECT_EQ(kernel.attention->members.front().gate,
            core::OperatorAlarmAttentionGateDTO::kConditionInterlock);
  EXPECT_EQ(kernel.attention->members.front().resource_ids,
            std::vector<core::ResourceId>{core::ResourceId("robot.joint_1")});
  ASSERT_TRUE(kernel.attention->active_configuration.has_value());
  EXPECT_EQ(kernel.attention->active_configuration->retained_bundle_id.value(),
            "bundle.1");
  EXPECT_EQ(kernel.attention->active_configuration->graph_revision.value(),
            "graph.1");
  // Before the owner observed a configuration it publishes null.
  const auto unobserved = TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope(std::string(cm::kPublishOperatorAlarmAttentionCommand),
               Attention(AttentionMember("acknowledgement",
                                         {Value("robot.joint_1")}, "operation",
                                         "operation.1"),
                         Value(nullptr)))));
  ASSERT_TRUE(unobserved.has_value());
  EXPECT_FALSE(std::get<core::OperatorAlarmAttentionSnapshotDTO>(
                   unobserved->request)
                   .active_configuration.has_value());
  EXPECT_EQ(ci::canonical_json(CellAdmissionResponseValue(decoded->command,
                                                          response)),
            "{\"api_version\":\"manyforge.execution_command_response.v1\","
            "\"command\":\"cell_admission_publish_operator_alarm_attention\","
            "\"ok\":true,\"operator_alarm_attention\":{\"generation\":5,"
            "\"owner_epoch\":\"alarm.owner.1\"}}");
  // The Manager-child binary channel never forwards this publication.
  EXPECT_THROW(static_cast<void>(cm::EncodeKernelCellAdmissionRpcRequest(*decoded)),
               std::exception);
  // Closed shape: an unknown gate is rejected before the Kernel sees it.
  EXPECT_THROW(
      static_cast<void>(TryDecodeCellAdmissionCommand(ci::canonical_json(
          Envelope(std::string(cm::kPublishOperatorAlarmAttentionCommand),
                   Attention(AttentionMember("advisory", {Value("robot.joint_1")},
                                             "operation", "operation.1")))))),
      std::exception);
}

TEST(CellAdmissionCommandCodec, ProgramNodeEvidenceIsExactAcrossBothTransports) {
  auto fields = Intent().as_object();
  for (auto& [key, value] : fields) {
    if (key == "claim_kind") value = Value("authored_scene");
    if (key == "owner_role") value = Value("authored_scene_owner");
  }
  fields.emplace_back("program_node_evidence", Value(Value::Object{
      {"program_runner_owner_epoch", Value("runner.owner")},
      {"program_run_id", Value("run.1")},
      {"parent_program_cycle_claim_generation", Value(7.0)},
      {"node_instance_id", Value("attach")},
      {"node_attempt_id", Value(std::string(512U, 'a'))},
      {"owner_api", Value("scene.carried_attachment")}}));
  const auto packet = TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Value(fields))));
  ASSERT_TRUE(packet.has_value());
  const auto binary = cm::EncodeKernelCellAdmissionRpcRequest(*packet);
  EXPECT_EQ(cm::DecodeKernelCellAdmissionRpcRequest(binary).request, packet->request);
  auto old_version = binary;
  old_version[4] = '1';
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcRequest(old_version), std::invalid_argument);
  RecordingKernel kernel;
  const auto response = cm::DispatchKernelCellAdmissionRpc(kernel, *packet);
  const auto json = CellAdmissionResponseValue(packet->command, response);
  EXPECT_EQ(DecodeCellAdmissionClaim(ci::canonical_json(*json.find_object_member("claim"))),
            std::get<core::CellAdmissionClaimDTO>(response));
  fields.back().second = Value(nullptr);
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Value(fields)))), std::invalid_argument);
}

TEST(CellAdmissionCommandCodec, RejectsUnknownExtraTypeRangeEnumAndOversize) {
  EXPECT_FALSE(TryDecodeCellAdmissionCommand(
                   R"({"api_version":"manyforge.execution_command.v1","command":"status"})")
                   .has_value());
  auto extra = Readiness().as_object();
  extra.emplace_back("unknown", Value(false));
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_update_readiness",
                            Value(std::move(extra))))),
               std::invalid_argument);
  auto wrong_type = Readiness().as_object();
  for (auto &[key, value] : wrong_type)
    if (key == "program_cycle_status_current")
      value = Value("true");
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_update_readiness",
                            Value(std::move(wrong_type))))),
               std::invalid_argument);
  for (const auto *old_field :
       {"manager_current", "manager_status_current"}) {
    auto old_shape = Readiness().as_object();
    old_shape.emplace_back(old_field, Value(true));
    EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                     Envelope("cell_admission_update_readiness",
                              Value(std::move(old_shape))))),
                 std::invalid_argument);
  }
  auto range = Readiness().as_object();
  for (auto &[key, value] : range)
    if (key == "required_component_total")
      value = Value(4294967296.0);
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_update_readiness",
                            Value(std::move(range))))),
               std::invalid_argument);
  auto bad_enum = Intent().as_object();
  for (auto &[key, value] : bad_enum)
    if (key == "owner_role")
      value = Value("AUTHORING_OWNER");
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_acquire",
                            Value(std::move(bad_enum))))),
               std::invalid_argument);
  EXPECT_THROW(TryDecodeCellAdmissionCommand(
                   std::string("{\"command\":\"cell_admission_") +
                   std::string(1024U * 1024U + 1U, 'x')),
               std::invalid_argument);
  auto incomplete_query = RecoveryQuery().as_object();
  for (auto &[key, value] : incomplete_query)
    if (key == "expected_release")
      value = Release();
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_recovery_receipt",
                            Value(std::move(incomplete_query))))),
               std::invalid_argument);
}

TEST(CellAdmissionCommandCodec,
     PrivateAndJsonTransportsDispatchToOneOwnerAndPreserveIdentityAndError) {
  RecordingKernel kernel;
  const auto json_packet = TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Intent())));
  ASSERT_TRUE(json_packet.has_value());
  const auto private_bytes = cm::EncodeKernelCellAdmissionRpcRequest(*json_packet);
  const auto private_packet =
      cm::DecodeKernelCellAdmissionRpcRequest(private_bytes);
  EXPECT_EQ(private_packet.request, json_packet->request);

  const auto first = cm::DispatchKernelCellAdmissionRpc(kernel, *json_packet);
  ASSERT_TRUE(std::holds_alternative<core::CellAdmissionClaimDTO>(first));
  const auto replay = cm::DispatchKernelCellAdmissionRpc(kernel, private_packet);
  ASSERT_TRUE(std::holds_alternative<core::CellAdmissionClaimDTO>(replay));
  EXPECT_EQ(replay, first);
  EXPECT_EQ(kernel.calls, 2U);

  const auto stale = cm::DispatchKernelCellAdmissionRpc(kernel, private_packet);
  const auto response =
      CellAdmissionResponseValue(private_packet.command, stale);
  EXPECT_EQ(*response.find_object_member("command"),
            Value("cell_admission_acquire"));
  const auto &error = *response.find_object_member("error");
  EXPECT_EQ(error.find_object_member("code")->as_string(),
            "stale_generation");
  EXPECT_EQ(error.find_object_member("message")->as_string(),
            "expected_generation_stale");
  EXPECT_EQ(error.find_object_member("details")
                ->find_object_member("admission_generation")
                ->as_string(),
            "8");
}

TEST(CellAdmissionCommandCodec, PrivateDtoAndResponseRoundTripsAreLossless) {
  for (const auto &command :
       {Value(Value::Object{
            {"api_version",
             Value(std::string(kCellAdmissionCommandApiVersion))},
            {"command", Value("cell_admission_get")}}),
        Envelope("cell_admission_update_readiness", Readiness()),
        Envelope("cell_admission_acquire", Intent()),
        Envelope("cell_admission_release", Release()),
        Envelope("cell_admission_reconcile", Reconcile()),
        Envelope("cell_admission_recovery_receipt", RecoveryQuery())}) {
    const auto packet =
        *TryDecodeCellAdmissionCommand(ci::canonical_json(command));
    EXPECT_EQ(cm::DecodeKernelCellAdmissionRpcRequest(
                  cm::EncodeKernelCellAdmissionRpcRequest(packet))
                  .request,
              packet.request);
  }
  const core::ErrorDTO error{"code", "message", {{"key", "value"}}};
  const auto bytes = cm::EncodeKernelCellAdmissionRpcResponse(
      "cell_admission_release", error);
  const auto decoded = cm::DecodeKernelCellAdmissionRpcResponse(bytes);
  EXPECT_EQ(decoded.command, "cell_admission_release");
  EXPECT_EQ(std::get<core::ErrorDTO>(decoded.response), error);

  RecordingKernel kernel;
  const auto query = *TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_recovery_receipt", RecoveryQuery())));
  const auto receipt =
      cm::DispatchKernelCellAdmissionRpc(kernel, query);
  const auto receipt_bytes = cm::EncodeKernelCellAdmissionRpcResponse(
      query.command, receipt);
  EXPECT_EQ(cm::DecodeKernelCellAdmissionRpcResponse(receipt_bytes).response,
            receipt);
  auto trailing_query = cm::EncodeKernelCellAdmissionRpcRequest(query);
  trailing_query.push_back('\0');
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcRequest(trailing_query),
               std::invalid_argument);
  EXPECT_THROW(cm::EncodeKernelCellAdmissionRpcResponse(
                   "cell_admission_release", receipt),
               std::invalid_argument);
}

TEST(CellAdmissionCommandCodec, LongServiceClaimSurvivesJsonAndPrivateOwnerTransport) {
  const auto packet = *TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Intent())));
  RecordingKernel kernel;
  auto response = cm::DispatchKernelCellAdmissionRpc(kernel, packet);
  auto &claim = std::get<core::CellAdmissionClaimDTO>(response);
  claim.accepted_at_ns = 31LL * 366 * 86400 * 1000000000;
  const auto json = CellAdmissionResponseValue("cell_admission_acquire", response);
  const auto &wire_claim = *json.find_object_member("claim");
  EXPECT_EQ(wire_claim.find_object_member("accepted_at_ns")->as_string(),
            std::to_string(claim.accepted_at_ns));
  EXPECT_EQ(DecodeCellAdmissionClaim(ci::canonical_json(wire_claim)), claim);
  const auto private_bytes = cm::EncodeKernelCellAdmissionRpcResponse(packet.command, response);
  EXPECT_EQ(cm::DecodeKernelCellAdmissionRpcResponse(private_bytes).response, response);
}

TEST(CellAdmissionCommandCodec, FreezesExactCommandSpecificJsonShapes) {
  const auto admission = ci::canonical_json(CellAdmissionResponseValue(
      "cell_admission_release",
      core::AuthoringAdmissionDTO{true, core::AdmissionGeneration(9U)}));
  EXPECT_EQ(
      admission,
      R"({"api_version":"manyforge.execution_command_response.v1","authoring_admission":{"admission_generation":9,"authoring_allowed":true},"command":"cell_admission_release","ok":true})");

  const auto packet = *TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Intent())));
  RecordingKernel kernel;
  const auto claim =
      cm::DispatchKernelCellAdmissionRpc(kernel, packet);
  const auto claim_json = ci::canonical_json(
      CellAdmissionResponseValue("cell_admission_acquire", claim));
  EXPECT_NE(claim_json.find(R"("claim":{)"), std::string::npos);
  EXPECT_EQ(claim_json.find(R"("result":)"), std::string::npos);
  EXPECT_EQ(claim_json.find(R"("authoring_admission":)"), std::string::npos);

  const core::ErrorDTO error{"stale_generation", "stale",
                             {{"admission_generation", "9"}}};
  EXPECT_EQ(
      ci::canonical_json(CellAdmissionResponseValue(
          "cell_admission_acquire", error)),
      R"({"api_version":"manyforge.execution_command_response.v1","command":"cell_admission_acquire","error":{"code":"stale_generation","details":{"admission_generation":"9"},"message":"stale"},"ok":false})");

  auto payload = Envelope("cell_admission_release", Release()).as_object();
  for (auto &[key, value] : payload) {
    if (key == "request") {
      key = "payload";
    }
  }
  EXPECT_THROW(
      TryDecodeCellAdmissionCommand(ci::canonical_json(Value(std::move(payload)))),
      std::invalid_argument);
}

TEST(CellAdmissionCommandCodec,
     PrivateTransportAdmitsItsExactBoundAndRejectsOneByteOver) {
  const core::ErrorDTO empty{"code", "", {}};
  const auto base = cm::EncodeKernelCellAdmissionRpcResponse(
      "cell_admission_release", empty);
  ASSERT_LT(base.size(), cm::kMaximumManagedRuntimeRpcPacketBytes);
  core::ErrorDTO maximum{"code",
                         std::string(cm::kMaximumManagedRuntimeRpcPacketBytes -
                                         base.size(),
                                     'm'),
                         {}};
  const auto admitted = cm::EncodeKernelCellAdmissionRpcResponse(
      "cell_admission_release", maximum);
  EXPECT_EQ(admitted.size(), cm::kMaximumManagedRuntimeRpcPacketBytes);
  maximum.message.push_back('m');
  EXPECT_THROW(cm::EncodeKernelCellAdmissionRpcResponse(
                   "cell_admission_release", maximum),
               std::invalid_argument);
  std::string malformed(cm::kMaximumManagedRuntimeRpcPacketBytes + 1U, 'x');
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcResponse(malformed),
               std::invalid_argument);

  auto claim_packet = *TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Intent())));
  auto &intent =
      std::get<core::CellAdmissionClaimIntentDTO>(claim_packet.request);
  const auto empty_claim_size =
      cm::EncodeKernelCellAdmissionRpcRequest(claim_packet).size();
  const auto remaining =
      cm::kMaximumManagedRuntimeRpcPacketBytes - empty_claim_size;
  const auto resource_count = (remaining + 135U) / 136U;
  ASSERT_LE(resource_count, core::kMaximumCellAdmissionScopeEntries);
  auto string_bytes = remaining - resource_count * sizeof(std::uint64_t);
  ASSERT_GE(string_bytes, resource_count * 16U);
  std::vector<std::size_t> resource_lengths(resource_count, 16U);
  string_bytes -= resource_count * 16U;
  for (std::size_t index = 0; index < resource_count; ++index) {
    const auto additional = std::min<std::size_t>(112U, string_bytes);
    resource_lengths[index] += additional;
    string_bytes -= additional;
    const auto index_text = std::to_string(index);
    auto resource =
        std::string("r") + std::string(6U - index_text.size(), '0') + index_text;
    resource.append(resource_lengths[index] - resource.size(), 'x');
    intent.scope.resources.emplace_back(std::move(resource));
  }
  ASSERT_EQ(string_bytes, 0U);
  EXPECT_EQ(cm::EncodeKernelCellAdmissionRpcRequest(claim_packet).size(),
            cm::kMaximumManagedRuntimeRpcPacketBytes);
  intent.scope.resources.back() =
      core::ResourceId(intent.scope.resources.back().value() + "r");
  EXPECT_THROW(cm::EncodeKernelCellAdmissionRpcRequest(claim_packet),
               std::invalid_argument);
}

TEST(CellAdmissionCommandCodec,
     BothCodecsRejectEveryOversizedScopeIdentity) {
  auto packet = *TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_acquire", Intent())));
  auto& intent =
      std::get<core::CellAdmissionClaimIntentDTO>(packet.request);
  const auto oversized =
      std::string(core::kMaximumCellAdmissionIdentityBytes + 1U, 'x');
  intent.scope.resources = {core::ResourceId(oversized)};
  const auto binary_resource = cm::EncodeKernelCellAdmissionRpcRequest(packet);
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcRequest(binary_resource),
               std::invalid_argument);
  intent.scope.resources.clear();
  intent.scope.configurations = {core::CellAdmissionConfigurationScopeDTO{
      core::ControlDomainId(oversized),
      core::ActiveControlConfigurationId("configuration"),
      core::ActiveControlConfigurationGeneration(1U)}};
  const auto binary_domain = cm::EncodeKernelCellAdmissionRpcRequest(packet);
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcRequest(binary_domain),
               std::invalid_argument);
  intent.scope.configurations.front().control_domain_id =
      core::ControlDomainId("domain");
  intent.scope.configurations.front().active_configuration_id =
      core::ActiveControlConfigurationId(oversized);
  const auto binary_configuration =
      cm::EncodeKernelCellAdmissionRpcRequest(packet);
  EXPECT_THROW(cm::DecodeKernelCellAdmissionRpcRequest(binary_configuration),
               std::invalid_argument);

  auto json_resource = Intent().as_object();
  for (auto& [key, value] : json_resource) {
    if (key == "scope") {
      value = Value(Value::Object{
          {"configurations", Value(Value::Array{})},
          {"resources", Value(Value::Array{Value(oversized)})}});
    }
  }
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_acquire",
                            Value(std::move(json_resource))))),
               std::invalid_argument);
  auto json_configuration = Intent().as_object();
  for (auto& [key, value] : json_configuration) {
    if (key == "scope") {
      value = Value(Value::Object{
          {"configurations",
           Value(Value::Array{Value(Value::Object{
               {"active_configuration_generation", Value(1.0)},
               {"active_configuration_id", Value(oversized)},
               {"control_domain_id", Value("domain")}})})},
          {"resources", Value(Value::Array{})}});
    }
  }
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_acquire",
                            Value(std::move(json_configuration))))),
               std::invalid_argument);
  auto json_domain = Intent().as_object();
  for (auto& [key, value] : json_domain) {
    if (key == "scope") {
      value = Value(Value::Object{
          {"configurations",
           Value(Value::Array{Value(Value::Object{
               {"active_configuration_generation", Value(1.0)},
               {"active_configuration_id", Value("configuration")},
               {"control_domain_id", Value(oversized)}})})},
          {"resources", Value(Value::Array{})}});
    }
  }
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
                   Envelope("cell_admission_acquire",
                            Value(std::move(json_domain))))),
               std::invalid_argument);
}

TEST(CellAdmissionCommandCodec,
     ConfiguredSeqpacketCarriesTheAdmittedAtomicCeiling) {
  int descriptors[2]{-1, -1};
  ASSERT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0,
                         descriptors),
            0);
  const int packet_bytes =
      static_cast<int>(cm::kMaximumManagedRuntimeRpcPacketBytes);
  for (const auto descriptor : descriptors) {
    ASSERT_EQ(::setsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &packet_bytes,
                           sizeof(packet_bytes)),
              0);
    ASSERT_EQ(::setsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &packet_bytes,
                           sizeof(packet_bytes)),
              0);
    int configured = 0;
    socklen_t configured_size = sizeof(configured);
    ASSERT_EQ(::getsockopt(descriptor, SOL_SOCKET, SO_SNDBUF, &configured,
                           &configured_size),
              0);
    ASSERT_GE(configured, packet_bytes);
    configured = 0;
    configured_size = sizeof(configured);
    ASSERT_EQ(::getsockopt(descriptor, SOL_SOCKET, SO_RCVBUF, &configured,
                           &configured_size),
              0);
    ASSERT_GE(configured, packet_bytes);
  }
  const std::string outbound(cm::kMaximumManagedRuntimeRpcPacketBytes, 'p');
  ASSERT_EQ(::send(descriptors[0], outbound.data(), outbound.size(),
                   MSG_NOSIGNAL),
            static_cast<ssize_t>(outbound.size()));
  std::string inbound(outbound.size(), '\0');
  iovec vector{inbound.data(), inbound.size()};
  msghdr message{};
  message.msg_iov = &vector;
  message.msg_iovlen = 1U;
  ASSERT_EQ(::recvmsg(descriptors[1], &message, 0),
            static_cast<ssize_t>(outbound.size()));
  EXPECT_EQ(message.msg_flags & MSG_TRUNC, 0);
  EXPECT_EQ(inbound, outbound);
  ::close(descriptors[0]);
  ::close(descriptors[1]);
}

TEST(CellAdmissionCommandCodec, SelectedIdleResetHasClosedAcceptedContext) {
  const Value accepted(Value::Object{
      {"request_owner_epoch", Value("operator.1")}, {"request_id", Value("reset.1")},
      {"request_payload_digest", Value(std::string(kDigest))},
      {"reload_initial_scene", Value(true)}, {"reset_robot_position", Value(false)},
      {"reset_claim_generation", Value(5.0)}});
  const auto decoded = TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_update_readiness", ResetReadiness(accepted))));
  ASSERT_TRUE(decoded.has_value());
  const auto& readiness = std::get<core::CellAdmissionReadinessSnapshotDTO>(decoded->request);
  ASSERT_TRUE(readiness.program_cycle_status->accepted_reset);
  EXPECT_FALSE(readiness.program_cycle_status->program_run_id);
  EXPECT_EQ(readiness.program_cycle_status->accepted_reset->request_id, core::RequestId("reset.1"));
  for (const auto name : {"reload_initial_scene", "reset_claim_generation"}) {
    auto fields = accepted.as_object();
    for (auto& [key, value] : fields) {
      if (key == name) value = Value(0.0);
    }
    EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
        Envelope("cell_admission_update_readiness", ResetReadiness(Value(std::move(fields)))))),
        std::invalid_argument);
  }
  EXPECT_THROW(TryDecodeCellAdmissionCommand(ci::canonical_json(
      Envelope("cell_admission_update_readiness", ResetReadiness(Value(nullptr))))),
      std::invalid_argument);
}

} // namespace
} // namespace manyforge::roboplan_backend::detail

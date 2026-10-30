#include "execution_host_commands.hpp"
#include "execution_host_responses.hpp"

#include <gtest/gtest.h>
#include <limits>

namespace manyforge::roboplan_backend::host_detail {
namespace {
using Value = content_identity::CanonicalValue;
const Value &Field(const Value &value, std::string_view key) {
  const auto *member = value.find_object_member(key);
  if (!member) throw std::runtime_error("missing wire field");
  return *member;
}
Value Replace(const Value &value, std::string key, Value replacement) {
  auto rows = value.as_object();
  for (auto &[name, member] : rows)
    if (name == key) {
      member = std::move(replacement);
      return Value(std::move(rows));
    }
  rows.emplace_back(std::move(key), std::move(replacement));
  return Value(std::move(rows));
}
core::IoRuleStateContextDTO Context() {
  return {core::RequestOwnerEpoch("kernel.owner"), UINT64_MAX,
          core::ActiveBundlePublicationGeneration(9'007'199'254'740'991ULL),
          core::RetainedBundleId("actual.bundle"), core::GraphRevision("actual.graph")};
}
core::IoGlobalRuleRecoveryOfferDTO Offer() {
  return {{Context(), core::InterruptPolicyId("global.door"), UINT64_MAX},
          false, core::InterruptPostLatchDisposition::kAwaitOperatorAck,
          core::IoGlobalRuleLatchCauseDTO::kSourceLoss};
}
Value Request(const Value &selector) {
  return Value(Value::Object{
      {"kind", Value("global_io_rule")},
      {"request_context", Value(Value::Object{
          {"request_id", Value("operator.clear")},
          {"request_owner_epoch", Value("operator.epoch")},
          {"source_label", Value("operator")},
          {"correlation_id", Value(nullptr)}})},
      {"selector", selector}, {"reason", Value(nullptr)}});
}
Command Decode(Value request) {
  return DecodeCommand(content_identity::canonical_json(Value(Value::Object{
      {"api_version", Value(std::string(kCommandApiVersion))},
      {"command", Value("recover_stop_fault")},
      {"request", std::move(request)}})));
}

// Typed wire fixtures only: these values do not install/latch a rule, prove a
// fresh source, grant operator authority or stand in for a Stop certificate.
TEST(GlobalRuleRecoveryWireTest, OfferSelectorRoundTripsWithoutCounterTruncation) {
  const auto offer = Offer();
  const auto projected = GlobalIoRuleRecoveryOfferValue(offer);
  EXPECT_EQ(Field(projected, "latch_cause").as_string(), "source_loss");
  EXPECT_FALSE(std::get<bool>(Field(projected, "current_condition").storage()));
  EXPECT_EQ(Field(projected, "post_latch_disposition").as_string(), "await_operator_ack");
  const auto decoded = Decode(Request(Field(projected, "selector")));
  ASSERT_EQ(decoded.kind, CommandKind::kRecoverStopFault);
  ASSERT_TRUE(decoded.global_io_rule_recovery);
  EXPECT_FALSE(decoded.stop_fault_recovery);
  EXPECT_EQ(decoded.global_io_rule_recovery->selector, offer.selector);
  EXPECT_FALSE(decoded.global_io_rule_recovery->reason);
  EXPECT_EQ(Field(Field(projected, "selector"), "expected_rule_registration_id").as_string(),
            "18446744073709551615");
  EXPECT_EQ(Field(Field(Field(projected, "selector"), "context"), "catalog_generation").as_string(),
            "18446744073709551615");
}

TEST(GlobalRuleRecoveryWireTest, UnknownConditionIsNotFalseOrAutomaticLossRecovery) {
  auto offer = Offer();
  offer.current_condition.reset();
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Field(GlobalIoRuleRecoveryOfferValue(offer), "current_condition").storage()));
  offer.post_latch_disposition = core::InterruptPostLatchDisposition::kAutoClearOnSignalRelease;
  EXPECT_THROW(GlobalIoRuleRecoveryOfferValue(offer), std::exception);
  offer.latch_cause = core::IoGlobalRuleLatchCauseDTO::kCondition;
  EXPECT_EQ(Field(GlobalIoRuleRecoveryOfferValue(offer), "post_latch_disposition").as_string(),
            "auto_clear_on_signal_release");
}

TEST(GlobalRuleRecoveryWireTest, RejectsCounterCoercionOverflowAndForeignFields) {
  const auto selector = Field(GlobalIoRuleRecoveryOfferValue(Offer()), "selector");
  for (const auto &counter : {"0", "01", "+1", "-1", "18446744073709551616"})
    EXPECT_THROW(Decode(Request(Replace(selector, "expected_rule_registration_id", Value(counter)))),
                 std::exception);
  EXPECT_THROW(Decode(Request(Replace(selector, "expected_rule_registration_id", Value(1.0)))),
               std::exception);
  auto context = Field(selector, "context");
  context = Replace(context, "catalog_generation", Value(1.0));
  EXPECT_THROW(Decode(Request(Replace(selector, "context", context))), std::exception);
  EXPECT_THROW(Decode(Request(Replace(selector, "stop_id", Value("not-a-stop")))), std::exception);
  EXPECT_THROW(Decode(Replace(Request(selector), "kind", Value("unknown"))), std::exception);
}

TEST(GlobalRuleRecoveryWireTest, ResultReusesExactRuleStateAndActualCausalEncoding) {
  const auto context = Context();
  const core::InterruptSourceId source("physical.source");
  const core::IoRuleStateDTO prior{
      core::InterruptPolicyId("global.door"), core::IoGlobalRuleState::kLatched,
      core::IoRuleAcquisitionKeyDTO{
          source, core::IoSourceIncarnation("actual.epoch"), UINT64_MAX,
          {context.owner_epoch, UINT64_MAX, source, UINT64_MAX}}};
  core::IoGlobalRuleRecoveryResultDTO result{
      core::RequestId("operator.clear"), core::RequestReplayStatusDTO::kAcceptedNew,
      false, context, prior,
      core::ErrorDTO{"io_global_rule_source_not_clear", std::string(8192U, 'e'), {}}};
  auto value = GlobalIoRuleRecoveryResultValue(result);
  EXPECT_EQ(Field(Field(Field(value, "rule"), "triggering_acquisition"), "source_sequence").as_string(),
            "18446744073709551615");
  EXPECT_EQ(Field(Field(value, "error"), "message").as_string().size(), 8192U);
  result.accepted = true;
  result.error.reset();
  result.rule = {prior.rule_id, core::IoGlobalRuleState::kClear, std::nullopt};
  value = GlobalIoRuleRecoveryResultValue(result);
  EXPECT_EQ(Field(Field(value, "rule"), "state").as_string(), "clear");
  EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(
      Field(Field(value, "rule"), "triggering_acquisition").storage()));
}

// Item 37: the Force close command names its step; only `arm` carries the
// operator's Full Stop generation. Any other shape is refused before the Kernel.
TEST(ForceCloseWireTest, DecodesBothStepsAndRefusesOtherShapes) {
  const auto command = [](Value::Object extra) {
    Value::Object root{{"api_version", Value(std::string(kCommandApiVersion))},
                       {"command", Value("force_close")}};
    for (auto &row : extra) root.emplace_back(std::move(row));
    return content_identity::canonical_json(Value(std::move(root)));
  };
  const auto arm = DecodeCommand(command({{"step", Value("arm")}, {"stop_generation", Value(5.0)}}));
  EXPECT_EQ(arm.kind, CommandKind::kForceClose);
  EXPECT_EQ(arm.force_close_step, std::optional<std::string>("arm"));
  ASSERT_TRUE(arm.stop_generation.has_value());
  EXPECT_EQ(arm.stop_generation->value(), 5U);
  const auto close = DecodeCommand(command({{"step", Value("close_operations")}}));
  EXPECT_EQ(close.force_close_step, std::optional<std::string>("close_operations"));
  EXPECT_FALSE(close.stop_generation.has_value());
  EXPECT_ANY_THROW(static_cast<void>(DecodeCommand(command({{"step", Value("arm")}}))));
  EXPECT_ANY_THROW(static_cast<void>(DecodeCommand(command({{"step", Value("arm")}, {"stop_generation", Value(0.0)}}))));
  EXPECT_ANY_THROW(static_cast<void>(DecodeCommand(
      command({{"step", Value("close_operations")}, {"stop_generation", Value(5.0)}}))));
  EXPECT_ANY_THROW(static_cast<void>(DecodeCommand(command({{"step", Value("recover")}}))));
}

} // namespace
} // namespace manyforge::roboplan_backend::host_detail

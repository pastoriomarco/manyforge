#include <gtest/gtest.h>

#include <manyforge_core/io_signal_descriptor.hpp>
#include <manyforge_core/prepared_participant_stop_plan_codec.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <algorithm>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "activation_component_profile_internal.hpp"
#include "activation_deployment_intent_internal.hpp"
#include "program_io_condition_compiler_internal.hpp"

namespace {

namespace c = manyforge::core;
namespace d = manyforge::deployment;
using V = d::CanonicalValue;

V compare(std::string key, std::string op, V value) {
  return V(V::Object{
      {"compare",
       V(V::Object{{"operand", V(V::Object{{"key", V(std::move(key))}})},
                   {"op", V(std::move(op))},
                   {"value", std::move(value)}})}});
}

V signal_compare(V value) {
  return V(V::Object{
      {"compare",
       V(V::Object{
           {"operand",
            V(V::Object{
                {"signal", V(V::Object{{"component_id", V("device")},
                                       {"signal_name", V("ready")}})}})},
           {"op", V("==")},
           {"value", std::move(value)}})}});
}

struct Context final {
  std::vector<c::ProgramSignalBindingDTO> bindings;
  std::vector<c::ProgramBlackboardKeyDeclarationDTO> keys{
      {"ready", "bool", true},         {"count", "int64", true},
      {"measured", "float64", true},   {"label", "string", true},
      {"node_outcome.pick", "string"}, {"interrupt_state.guard", "string"},
      {"computed.answer", "int64"},    {"pose", "stored_pose", true}};
  std::vector<d::RetainedIoSignalConfiguration> signals;

  auto compile(const V &condition) const {
    return d::detail::compile_authored_program_io_condition(condition, bindings,
                                                            keys, signals);
  }

  void input(
      c::IoSignalLivenessMode liveness = c::IoSignalLivenessMode::kInProcess) {
    const c::IoSignalDescriptorDTO descriptor{
        {c::ControllableComponentId("device"), "ready"},
        c::InterruptSourceId("device.input"),
        c::IoSignalValueType::kBoolean,
        c::IoSignalDirection::kInput,
        c::IoSignalUpdateMode::kChangeOnly,
        c::IoTransitionCapability::kSampledOnly,
        {c::IoFreshnessMode::kProviderValidity, std::nullopt},
        std::nullopt,
        liveness};
    auto digest = c::ComputeIoSignalDescriptorDigest(descriptor);
    if (!digest.ok())
      throw std::logic_error(digest.error().message);
    bindings.push_back({{c::ControllableComponentId("device"), "ready"},
                        {c::ControllableComponentId("device"), c::CommandTargetId("gpio")},
                        c::RuntimeUnitId("host"), descriptor.value_type,
                        descriptor.direction, descriptor.transition_capability, digest.value()});
    signals.push_back({c::RuntimeUnitId("host"),
                       c::CommandTargetId("gpio"),
                       c::ResourceRoleId("ready"),
                       descriptor,
                       {}});
  }
};

bool evaluate(const c::CheckedIoCondition &condition,
              const std::vector<c::IoConditionValueDTO> &values) {
  std::vector<std::uint8_t> workspace(condition.workspace_size());
  auto result = c::EvaluateIoCondition(condition, values, workspace);
  if (!result.ok())
    throw std::logic_error(result.error().message);
  return result.value();
}

TEST(ProgramIoConditionCompilerTest,
     CompoundTypingAndSingletonNormalizationUseCoreEvaluator) {
  Context context;
  const auto negated = V(V::Object{{"not", compare("ready", "==", V(false))}});
  const auto singleton = V(V::Object{{"any", V(V::Array{negated})}});
  const auto condition = V(V::Object{
      {"all", V(V::Array{compare("count", ">=", V(10.0)), singleton})}});
  const auto compiled = context.compile(condition);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  EXPECT_EQ(compiled.value().condition->inputs().size(), 2U);
  EXPECT_TRUE(evaluate(*compiled.value().condition, {std::int64_t{10}, true}));
  EXPECT_FALSE(evaluate(*compiled.value().condition, {std::int64_t{9}, true}));
  EXPECT_FALSE(
      evaluate(*compiled.value().condition, {std::int64_t{10}, false}));
  EXPECT_TRUE(compiled.value().required_signal_sources.empty());
}

TEST(ProgramIoConditionCompilerTest,
     ExactScalarTypesRequireUserInitialOrQualifiedSystemDeclaration) {
  Context context;
  for (const auto &declaration : context.keys) {
    const auto &name = declaration.name;
    const auto &type = declaration.declared_type;
    if (type == "stored_pose")
      continue;
    const auto value = type == "bool"     ? V(true)
                       : type == "string" ? V("signal_trip")
                                          : V(1.0);
    const auto compiled = context.compile(compare(name, "==", value));
    const bool system = name.starts_with("node_outcome.") ||
                        name.starts_with("interrupt_state.");
    if (!declaration.has_declared_initial && !system) {
      ASSERT_FALSE(compiled.ok());
      EXPECT_EQ(compiled.error().details.at("reason"),
                "interrupt_key_initial_required");
      continue;
    }
    ASSERT_TRUE(compiled.ok()) << compiled.error().message;
    ASSERT_EQ(compiled.value().condition->inputs().size(), 1U);
    EXPECT_EQ(std::get<c::IoConditionBlackboardKeyDTO>(
                  compiled.value().condition->inputs().front().source)
                  .name,
              name);
  }
  EXPECT_FALSE(context.compile(compare("missing", "==", V(true))).ok());
  EXPECT_FALSE(context.compile(compare("pose", "==", V(true))).ok());
  EXPECT_FALSE(context.compile(compare("ready", "<", V(true))).ok());
  EXPECT_FALSE(context.compile(compare("ready", "==", V(1.0))).ok());
  EXPECT_FALSE(context.compile(compare("count", "==", V("1"))).ok());
  EXPECT_FALSE(context.compile(compare("count", "=", V(1.0))).ok());
  EXPECT_FALSE(
      context.compile(compare("count", "==", V(9007199254740992.0))).ok());
}

TEST(ProgramIoConditionCompilerTest,
     NumericComparisonsPreserveExactMixedIntAndFloatMeaning) {
  Context context;
  const auto compiled = context.compile(compare("count", ">", V(1.5)));
  ASSERT_TRUE(compiled.ok());
  EXPECT_TRUE(evaluate(*compiled.value().condition, {std::int64_t{2}}));
  EXPECT_FALSE(evaluate(*compiled.value().condition, {std::int64_t{1}}));
  const auto maximum =
      context.compile(compare("count", ">", V(9007199254740991.0)));
  ASSERT_TRUE(maximum.ok());
  EXPECT_TRUE(
      evaluate(*maximum.value().condition, {std::int64_t{9007199254740992LL}}));
}

TEST(ProgramIoConditionCompilerTest,
     ClosedShapesRefuseParameterAndOperandComparisons) {
  Context context;
  for (const auto &value : V::Array{
           V(V::Object{{"all", V(V::Array{})}}),
           V(V::Object{{"any", V(V::Array{})}}),
           V(V::Object{{"not", V(V::Array{})}}),
           V(V::Object{{"all", V(V::Array{compare("ready", "==", V(true))})},
                       {"extra", V(true)}}),
           V(V::Object{
               {"compare",
                V(V::Object{{"operand", V(V::Object{{"param", V("enabled")}})},
                            {"op", V("==")},
                            {"value", V(true)}})}}),
           compare("ready", "==", V(V::Object{{"key", V("ready")}}))})
    EXPECT_FALSE(context.compile(value).ok());
}

TEST(ProgramIoConditionCompilerTest,
     AuthoredDepthAndNodesAreBoundedBeforeSingletonCollapse) {
  Context context;
  auto condition = compare("ready", "==", V(true));
  for (std::size_t depth = 1; depth < 8U; ++depth)
    condition = V(V::Object{{"all", V(V::Array{condition})}});
  EXPECT_TRUE(context.compile(condition).ok());
  condition = V(V::Object{{"all", V(V::Array{condition})}});
  EXPECT_FALSE(context.compile(condition).ok());
  V::Array children(63U, compare("ready", "==", V(true)));
  const auto at_bound = context.compile(V(V::Object{{"all", V(children)}}));
  ASSERT_TRUE(at_bound.ok());
  EXPECT_EQ(at_bound.value().condition->inputs().size(), 1U);
  children.push_back(compare("ready", "==", V(true)));
  EXPECT_FALSE(context.compile(V(V::Object{{"all", V(children)}})).ok());
}

TEST(ProgramIoConditionCompilerTest, GrammarBoundAllowsMaximumDistinctInputs) {
  constexpr std::size_t maximum_nodes = 64U;
  constexpr std::size_t maximum_inputs = maximum_nodes - 1U;
  Context context;
  V::Array children;
  for (std::size_t index = 0; index < maximum_inputs; ++index) {
    const auto name = "key" + std::to_string(index);
    context.keys.push_back({name, "bool", true});
    children.push_back(compare(name, "==", V(true)));
  }
  const auto compiled = context.compile(V(V::Object{{"all", V(children)}}));
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  EXPECT_EQ(compiled.value().condition->inputs().size(), maximum_inputs);
  std::vector<c::IoConditionValueDTO> actual_values(maximum_inputs, true);
  EXPECT_TRUE(evaluate(*compiled.value().condition, actual_values));
  actual_values.back() = false;
  EXPECT_FALSE(evaluate(*compiled.value().condition, actual_values));
  // This expression adds a comparison, so its all parent exceeds the retained
  // authored 64-node bound; no separate input-limit policy is involved.
  context.keys.push_back({"extra", "bool", true});
  children.push_back(compare("extra", "==", V(true)));
  const auto overflow = context.compile(V(V::Object{{"all", V(children)}}));
  ASSERT_FALSE(overflow.ok());
  EXPECT_EQ(overflow.error().details.at("reason"), "interrupt_condition_invalid");
  const auto single = context.compile(compare("key0", "==", V(true)));
  ASSERT_TRUE(single.ok());
  EXPECT_EQ(single.value().condition->inputs().size(), 1U);
}

TEST(ProgramIoConditionCompilerTest,
     SignalSourceRequiresExactBindingDescriptorAndInputDirection) {
  Context context;
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
  context.input();
  const auto compiled = context.compile(signal_compare(V(true)));
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().required_signal_sources.size(), 1U);
  EXPECT_EQ(compiled.value().required_signal_sources.front().value(),
            "device.input");
  EXPECT_TRUE(evaluate(*compiled.value().condition, {true}));
  context.signals.front().descriptor.signal_id.name = "other";
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
  context.signals.front().descriptor.signal_id.name = "ready";
  context.signals.front().command_target_id = c::CommandTargetId("other");
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
}

TEST(ProgramIoConditionCompilerTest, PrebindingSignalComparisonUsesCoreTypeRules) {
  Context context;
  context.input();
  const auto &descriptor = context.signals.front().descriptor;
  EXPECT_TRUE(d::detail::check_program_signal_comparison(descriptor, V("=="), V(true)).ok());
  const auto wrong_type = d::detail::check_program_signal_comparison(descriptor, V("=="), V(1.0));
  ASSERT_FALSE(wrong_type.ok());
  EXPECT_EQ(wrong_type.error().details.at("reason"), "io_signal_type_mismatch");
  const auto wrong_order = d::detail::check_program_signal_comparison(descriptor, V("<"), V(true));
  ASSERT_FALSE(wrong_order.ok());
  EXPECT_EQ(wrong_order.error().details.at("reason"), "io_signal_type_mismatch");
  const auto unknown_operator = d::detail::check_program_signal_comparison(descriptor, V("bad"), V(true));
  ASSERT_FALSE(unknown_operator.ok());
  EXPECT_EQ(unknown_operator.error().details.at("reason"), "interrupt_condition_invalid");
}

TEST(ProgramIoConditionCompilerTest, DirectSignalRequiresUniqueExactBindingAndRetainedSource) {
  Context context;
  context.input();
  ASSERT_TRUE(context.compile(signal_compare(V(true))).ok());
  context.bindings.push_back(context.bindings.front());
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
  context.bindings.pop_back();
  context.signals.push_back(context.signals.front());
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
  context.signals.pop_back();
  context.bindings.front().runtime_unit_id = c::RuntimeUnitId("other");
  EXPECT_FALSE(context.compile(signal_compare(V(true))).ok());
  const auto old_role_selector = d::parse_strict_json_model_yaml(
      R"({"compare":{"operand":{"signal":{"target_role":"inputs","resource_role":"ready"}},"op":"==","value":true}})").root();
  EXPECT_FALSE(context.compile(old_role_selector).ok());
}

TEST(ProgramIoConditionCompilerTest,
     ExistingProfileDecoderRefusesOpenOrNoncanonicalValues) {
  const auto profile =
      V(V::Object{{"profile_id", V("floor")},
                  {"stop_mode", V("hard_deceleration")},
                  {"allow_natural_completion_on_short_path", V(false)},
                  {"require_backend_confirmation", V(true)},
                  {"jerk_limits_enabled", V(false)}});
  const auto decoded =
      c::DecodeMotionInterruptStopProfile(d::canonical_json(profile));
  ASSERT_TRUE(decoded.ok()) << decoded.error().message;
  EXPECT_EQ(decoded.value().stop_mode,
            c::MotionInterruptStopMode::kHardDeceleration);
  auto unknown = profile.as_object();
  unknown.emplace_back("unknown", V(true));
  EXPECT_FALSE(
      c::DecodeMotionInterruptStopProfile(d::canonical_json(V(unknown))).ok());
  EXPECT_FALSE(
      c::DecodeMotionInterruptStopProfile(" " + d::canonical_json(profile))
          .ok());
}

TEST(ProgramIoConditionCompilerTest,
     ChangeOnlyRefusesEveryInterruptConditionOperand) {
  Context context;
  context.input(c::IoSignalLivenessMode::kChangeOnly);
  const auto node = context.compile(signal_compare(V(true)));
  ASSERT_FALSE(node.ok());
  EXPECT_EQ(node.error().details.at("reason"), "io_signal_liveness_unproven");
  const auto global_condition = V(V::Object{
      {"compare",
       V(V::Object{
           {"operand",
            V(V::Object{{"signal",
                         V(V::Object{{"controllable_component_id", V("device")},
                                     {"signal_name", V("ready")}})}})},
           {"op", V("==")},
           {"value", V(true)}})}});
  const auto global = d::detail::compile_authored_deployment_io_condition(
      global_condition, context.keys, context.signals);
  ASSERT_FALSE(global.ok());
  EXPECT_EQ(global.error().details.at("reason"), "io_signal_liveness_unproven");
}

TEST(ProgramIoConditionCompilerTest,
     SharedGlobalAndNodeLowererRefusesMissingInitialInsideCompoundConditions) {
  Context context;
  context.keys.front().has_declared_initial = false;
  const auto condition =
      V(V::Object{{"not", compare("ready", "==", V(false))}});
  const auto node = context.compile(condition);
  const auto global = d::detail::compile_authored_deployment_io_condition(
      condition, context.keys, context.signals);
  for (const auto *result : {&node, &global}) {
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().details.at("reason"),
              "interrupt_key_initial_required");
  }
  // A different uninitialized declaration is irrelevant when not subscribed.
  context.keys.front().has_declared_initial = true;
  EXPECT_TRUE(context.compile(condition).ok());
}

TEST(ProgramIoConditionCompilerTest,
     SystemInitialDoesNotGrantGlobalRuleStateOperands) {
  Context context;
  for (const auto &[key, value] :
       {std::pair{"node_outcome.pick", "none"},
        std::pair{"interrupt_state.guard", "clear"},
        std::pair{"interrupt_state.guard", "pending"}}) {
    const auto condition = compare(key, "==", V(value));
    const auto node = context.compile(condition);
    const auto global = d::detail::compile_authored_deployment_io_condition(
        condition, context.keys, context.signals);
    ASSERT_TRUE(node.ok()) << node.error().message;
    EXPECT_TRUE(evaluate(*node.value().condition, {std::string(value)}));
    if (std::string_view(key).starts_with("interrupt_state.")) {
      ASSERT_FALSE(global.ok());
      EXPECT_EQ(global.error().details.at("reason"),
                "global_rule_state_operand_unsupported");
      EXPECT_EQ(global.error().details.at("key"), key);
    } else {
      ASSERT_TRUE(global.ok()) << global.error().message;
      EXPECT_TRUE(evaluate(*global.value().condition, {std::string(value)}));
    }
  }
  for (const auto *key : {"node_outcome.absent", "interrupt_state.absent"}) {
    const auto refused = context.compile(compare(key, "==", V("none")));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "interrupt_condition_key_refused");
  }
  for (const auto *key :
       {"node_outcome", "interrupt_state", "node_outcome.", "interrupt_state.",
        "node_outcomes.pick", "interrupt_states.guard"}) {
    context.keys.push_back({key, "string"});
    const auto refused = context.compile(compare(key, "==", V("none")));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "interrupt_key_initial_required");
  }
}

TEST(ProgramIoConditionCompilerTest,
     MissingKeyMetadataComesFromTheExactNestedOperandWithoutChangingItsReason) {
  Context context;
  const auto condition =
      V(V::Object{{"not", compare("missing", "==", V(false))}});
  const auto node = context.compile(condition);
  const auto global = d::detail::compile_authored_deployment_io_condition(
      condition, context.keys, context.signals);
  for (const auto *result : {&node, &global}) {
    ASSERT_FALSE(result->ok());
    EXPECT_EQ(result->error().details.at("reason"),
              "interrupt_condition_key_refused");
    EXPECT_EQ(result->error().details.at("key"), "missing");
  }
  context.keys.front().has_declared_initial = false;
  const auto initial = context.compile(compare("ready", "==", V(false)));
  ASSERT_FALSE(initial.ok());
  EXPECT_EQ(initial.error().details.at("reason"),
            "interrupt_key_initial_required");
  EXPECT_EQ(initial.error().details.count("key"), 0U);
}

TEST(
    ProgramIoConditionCompilerTest,
    NoProgramMixedConditionWaitsWholeWithoutInventingTypesOrPartialEvaluation) {
  Context context;
  context.input();
  const auto condition = d::parse_strict_json_model_yaml(R"({"all":[
    {"compare":{"operand":{"key":"z"},"op":"==","value":false}},
    {"not":{"compare":{"operand":{"signal":{"controllable_component_id":"device","signal_name":"ready"}},"op":"==","value":false}}},
    {"compare":{"operand":{"key":"a"},"op":">=","value":0}},
    {"compare":{"operand":{"key":"z"},"op":"==","value":false}}]})")
                             .root();
  const auto waiting = d::detail::compile_authored_deployment_io_condition(
      condition, {}, context.signals, true);
  ASSERT_TRUE(waiting.ok()) << waiting.error().message;
  EXPECT_EQ(waiting.value().condition, nullptr);
  EXPECT_EQ(waiting.value().waiting_for_program_keys,
            (std::vector<std::string>{"a", "z"}));
  EXPECT_EQ(waiting.value().required_signal_sources,
            (std::vector<c::InterruptSourceId>{
                c::InterruptSourceId("device.input")}));
  // A real declaration never bypasses87, even in dependency validation mode.
  context.keys.front().has_declared_initial = false;
  const auto initial = d::detail::compile_authored_deployment_io_condition(
      compare("ready", "==", V(false)), context.keys, context.signals, true);
  ASSERT_FALSE(initial.ok());
  EXPECT_EQ(initial.error().details.at("reason"),
            "interrupt_key_initial_required");
}

TEST(ProgramIoConditionCompilerTest,
     WaitingStillValidatesAllSignalAndUnsupportedBranchesAndOperandBounds) {
  const auto combined = [](V later) {
    return V(V::Object{{"all", V(V::Array{compare("missing", "==", V(false)),
                                          std::move(later)})}});
  };
  const auto signal = [](V value, std::string name = "ready") {
    return V(V::Object{
        {"compare",
         V(V::Object{
             {"operand",
              V(V::Object{
                  {"signal",
                   V(V::Object{{"controllable_component_id", V("device")},
                               {"signal_name", V(std::move(name))}})}})},
             {"op", V("==")},
             {"value", std::move(value)}})}});
  };
  Context context;
  context.input();
  const auto wrong_type = d::detail::compile_authored_deployment_io_condition(
      combined(signal(V(1.0))), {}, context.signals, true);
  ASSERT_FALSE(wrong_type.ok());
  EXPECT_EQ(wrong_type.error().details.at("reason"), "io_signal_type_mismatch");
  const auto unbound = d::detail::compile_authored_deployment_io_condition(
      combined(signal(V(false), "absent")), {}, context.signals, true);
  ASSERT_FALSE(unbound.ok());
  EXPECT_EQ(unbound.error().details.at("reason"), "io_signal_unbound");
  Context change_only;
  change_only.input(c::IoSignalLivenessMode::kChangeOnly);
  const auto liveness = d::detail::compile_authored_deployment_io_condition(
      combined(signal(V(false))), {}, change_only.signals, true);
  ASSERT_FALSE(liveness.ok());
  EXPECT_EQ(liveness.error().details.at("reason"),
            "io_signal_liveness_unproven");
  const auto state = d::detail::compile_authored_deployment_io_condition(
      combined(compare("interrupt_state.guard", "==", V("clear"))), {},
      context.signals, true);
  ASSERT_FALSE(state.ok());
  EXPECT_EQ(state.error().details.at("reason"),
            "global_rule_state_operand_unsupported");
  const auto outcome = d::detail::compile_authored_deployment_io_condition(
      combined(compare("node_outcome.absent", "==", V("none"))), {},
      context.signals, true);
  ASSERT_FALSE(outcome.ok());
  EXPECT_EQ(outcome.error().details.at("reason"),
            "interrupt_condition_key_refused");
  // A condition's only limits are 64
  // authored nodes and depth 8; there is no separate input cap. One `all`
  // with 63 comparisons is 64 nodes and compiles; 64 comparisons exceed the
  // node bound and are refused.
  const auto all_of = [&](unsigned count) {
    V::Array operands;
    for (unsigned i = 0; i != count; ++i)
      operands.push_back(compare("key_" + std::to_string(i), "==", V(false)));
    return d::detail::compile_authored_deployment_io_condition(
        V(V::Object{{"all", V(std::move(operands))}}), {}, context.signals, true);
  };
  EXPECT_TRUE(all_of(63U).ok());
  const auto bound = all_of(64U);
  ASSERT_FALSE(bound.ok());
  EXPECT_EQ(bound.error().details.at("reason"), "interrupt_condition_invalid");
  const auto malformed = d::detail::compile_authored_deployment_io_condition(
      combined(V(V::Object{{"not", V(V::Array{})}})), {}, context.signals,
      true);
  EXPECT_FALSE(malformed.ok());
  for (const auto &name :
       {std::string{}, std::string(257U, 'k'), std::string("x\0y", 3U)})
    EXPECT_FALSE(d::detail::compile_authored_deployment_io_condition(
                     compare(name, "==", V(false)), {}, context.signals, true)
                     .ok());
}

TEST(ProgramIoConditionCompilerTest,
     BindingDependencyDiscoveryReusesLiteralConditionTraversalWithoutTypes) {
  const auto node = [](std::string id, V condition) {
    return V(V::Object{{"node_id", V(std::move(id))},
                       {"parameters", V(V::Object{{"interrupt_condition",
                                                   std::move(condition)}})}});
  };
  const auto condition = d::parse_strict_json_model_yaml(R"({"all":[
    {"compare":{"operand":{"key":"ready"},"op":"==","value":true}},
    {"any":[
      {"compare":{"operand":{"signal":{"component_id":"station","signal_name":"b"}},"op":"==","value":true}},
      {"not":{"compare":{"operand":{"signal":{"component_id":"station","signal_name":"a"}},"op":"==","value":false}}}]}]})")
                             .root();
  for (const auto &id : {"motion.move_until_signal", "motion.probe_search"}) {
    const auto refs =
        d::detail::collect_program_node_io_signal_references(node(id, condition));
    ASSERT_TRUE(refs.ok()) << refs.error().message;
    EXPECT_EQ(refs.value(),
              (std::vector<d::detail::ProgramIoSignalReference>{
                  {c::ControllableComponentId("station"), "a"},
                  {c::ControllableComponentId("station"), "b"}}));
    auto dynamic = node(id, condition).as_object();
    dynamic.emplace_back(
        "value_sources",
        V(V::Object{{"interrupt_condition.compare.operand.signal.signal_name",
                     V(V::Object{})}}));
    const auto refused =
        d::detail::collect_program_node_io_signal_references(V(dynamic));
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "interrupt_condition_invalid");
  }
  const auto malformed = d::detail::collect_program_node_io_signal_references(
      node("motion.move_until_signal", V(V::Object{{"not", V(V::Array{})}})));
  EXPECT_FALSE(malformed.ok());
  // Collection is not a declaration or initial-value exemption: full lowering
  // still requires a real Program declaration for the same key operand.
  const auto compiled = d::detail::compile_authored_program_io_condition(
      compare("ready", "==", V(true)), {}, {}, {});
  ASSERT_FALSE(compiled.ok());
  EXPECT_EQ(compiled.error().details.at("reason"),
            "interrupt_condition_key_refused");
}

TEST(ProgramIoConditionCompilerTest, DirectReferencesDeduplicateAndKeepDistinctInputs) {
  constexpr unsigned maximum_inputs = 64U - 1U;
  Context duplicate;
  duplicate.input();
  V::Array repeated;
  for (unsigned i = 0; i != maximum_inputs; ++i)
    repeated.push_back(signal_compare(V(true)));
  const V same(V::Object{{"all", V(repeated)}});
  const auto same_result = duplicate.compile(same);
  ASSERT_TRUE(same_result.ok()) << same_result.error().message;
  EXPECT_EQ(same_result.value().condition->inputs().size(), 1U);
  EXPECT_EQ(same_result.value().required_signal_sources.size(), 1U);

  Context distinct;
  V::Array comparisons;
  for (unsigned i = 0; i != maximum_inputs; ++i) {
    Context item;
    item.input();
    const auto name = "device_" + std::to_string(i);
    const c::ControllableComponentId component(name);
    auto &descriptor = item.signals.front().descriptor;
    descriptor.signal_id.controllable_component_id = component;
    descriptor.source_id = c::InterruptSourceId("source_" + std::to_string(i));
    const auto digest = c::ComputeIoSignalDescriptorDigest(descriptor);
    ASSERT_TRUE(digest.ok()) << digest.error().message;
    item.bindings.front().reference.component_id = component;
    item.bindings.front().target.component_id = component;
    item.bindings.front().signal_descriptor_digest = digest.value();
    distinct.bindings.push_back(item.bindings.front());
    distinct.signals.push_back(item.signals.front());
    comparisons.emplace_back(V::Object{{"compare", V(V::Object{
      {"operand", V(V::Object{{"signal", V(V::Object{{"component_id", V(name)}, {"signal_name", V("ready")}})}})},
      {"op", V("==")}, {"value", V(true)}})}});
  }
  const V condition(V::Object{{"all", V(comparisons)}});
  const V node(V::Object{{"node_id", V("motion.move_until_signal")},
                         {"parameters", V(V::Object{{"interrupt_condition", condition}})}});
  const auto discovered = d::detail::collect_program_node_io_signal_references(node);
  ASSERT_TRUE(discovered.ok()) << discovered.error().message;
  EXPECT_EQ(discovered.value().size(), maximum_inputs);
  const auto checked = distinct.compile(condition);
  ASSERT_TRUE(checked.ok()) << checked.error().message;
  EXPECT_EQ(checked.value().condition->inputs().size(), maximum_inputs);
  EXPECT_EQ(checked.value().required_signal_sources.size(), maximum_inputs);
  std::vector<c::IoConditionValueDTO> values(maximum_inputs, true);
  EXPECT_TRUE(evaluate(*checked.value().condition, values));
  values.back() = false;
  EXPECT_FALSE(evaluate(*checked.value().condition, values));
}

// A Deployment condition names the core-held
// `<output>.changed_outside` bool of an output with read-back that isn't
// controller-owned; it resolves to the core's signal and source ids.
c::IoSignalDescriptorDTO output_descriptor(std::string name, bool readback,
                                           bool controller_owned) {
  c::IoSignalDescriptorDTO descriptor{
      {c::ControllableComponentId("device"), std::move(name)},
      c::InterruptSourceId("device.outputs"),
      c::IoSignalValueType::kBoolean,
      c::IoSignalDirection::kOutput,
      c::IoSignalUpdateMode::kChangeOnly,
      c::IoTransitionCapability::kSampledOnly,
      {c::IoFreshnessMode::kProviderValidity, std::nullopt},
      c::ResourceId("device.valve"),
      std::nullopt};
  descriptor.readback = readback;
  descriptor.controller_owned = controller_owned;
  return descriptor;
}

V changed_outside_compare(std::string signal_name) {
  return V(V::Object{
      {"compare",
       V(V::Object{
           {"operand",
            V(V::Object{{"signal",
                         V(V::Object{{"controllable_component_id", V("device")},
                                     {"signal_name", V(std::move(signal_name))}})}})},
           {"op", V("==")},
           {"value", V(true)}})}});
}

TEST(ProgramIoConditionCompilerTest,
     GlobalChangedOutsideResolvesToTheCoreHeldSignalAndSource) {
  Context context;
  const auto descriptor = output_descriptor("valve", true, false);
  context.signals.push_back({c::RuntimeUnitId("host"), c::CommandTargetId("gpio"),
                             c::ResourceRoleId("valve"), descriptor, {}});
  const auto compiled = d::detail::compile_authored_deployment_io_condition(
      changed_outside_compare("valve.changed_outside"), context.keys,
      context.signals);
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  ASSERT_EQ(compiled.value().condition->inputs().size(), 1U);
  EXPECT_EQ(compiled.value().condition->inputs().front().source,
            c::IoConditionInputSourceDTO(
                c::ChangedOutsideSignalId(descriptor.signal_id)));
  EXPECT_EQ(compiled.value().required_signal_sources,
            std::vector<c::InterruptSourceId>{
                c::ChangedOutsideSourceId(descriptor.signal_id)});
  EXPECT_TRUE(evaluate(*compiled.value().condition, {true}));
  // The output itself is still not a condition input.
  const auto output = d::detail::compile_authored_deployment_io_condition(
      changed_outside_compare("valve"), context.keys, context.signals);
  ASSERT_FALSE(output.ok());
  EXPECT_EQ(output.error().details.at("reason"), "io_signal_direction_mismatch");
}

TEST(ProgramIoConditionCompilerTest,
     GlobalChangedOutsideIsRefusedWithoutReadbackOrWhenControllerOwned) {
  Context context;
  for (const auto &[name, readback, owned] :
       {std::tuple{std::string("blind"), false, false},
        std::tuple{std::string("owned"), true, true}}) {
    context.signals.push_back({c::RuntimeUnitId("host"),
                               c::CommandTargetId("gpio"),
                               c::ResourceRoleId(name),
                               output_descriptor(name, readback, owned),
                               {}});
  }
  for (const auto *name : {"blind.changed_outside", "owned.changed_outside"}) {
    SCOPED_TRACE(name);
    const auto refused = d::detail::compile_authored_deployment_io_condition(
        changed_outside_compare(name), context.keys, context.signals);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().details.at("reason"),
              "io_output_changed_outside_unavailable");
  }
  const auto unknown = d::detail::compile_authored_deployment_io_condition(
      changed_outside_compare("missing.changed_outside"), context.keys,
      context.signals);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "io_signal_unbound");
}

c::JsonValueDTO json(const V &value) {
  return std::visit(
      [](const auto &item) -> c::JsonValueDTO {
        using T = std::decay_t<decltype(item)>;
        if constexpr (std::is_same_v<T, V::Array>) {
          c::JsonValueDTO::Array result;
          for (const auto &element : item) result.push_back(json(element));
          return {std::move(result)};
        } else if constexpr (std::is_same_v<T, V::Object>) {
          c::JsonValueDTO::Object result;
          for (const auto &[name, element] : item) result.emplace(name, json(element));
          return {std::move(result)};
        } else {
          return {item};
        }
      },
      value.storage());
}

// A real xArm control-box input (e.g. a light curtain
// on CI0) drives a global rule. The I/O member's rows come from the curated
// Lite 6 models on the real lane; the input is periodic with the model's
// sample-age budget, so a stale stream leaves the rule unable to watch.
TEST(ProgramIoConditionCompilerTest, RealXArmControlBoxInputCompilesIntoAGlobalRule) {
  using Object = V::Object;
  const auto member = [](std::string id, std::string model, std::string role) {
    return V(Object{{"component_id", V(std::move(id))},
                    {"component_profile_id", V(std::move(model))},
                    {"owner", V("lite6")},
                    {"role", V(std::move(role))}});
  };
  auto tool_io = member("lite6_tool_io", "ufactory.lite6_tool_io", "tool_io").as_object();
  tool_io.emplace_back("active", V(false));
  auto intent = d::detail::parse_activation_deployment_intent(
      V(Object{{"api_version", V("manyforge.deployment_intent.v2")},
               {"deployment_artifact_id", V("machine")},
               {"name", V("machine")},
               {"attachments", V(V::Array{})},
               {"components",
                V(V::Array{V(Object{{"component_id", V("lite6")},
                                    {"component_profile_id", V("ufactory.lite6_machine")},
                                    {"lane", V("real")},
                                    {"configuration",
                                     V(Object{{"device_address_ref", V("lite6_main")}})}}),
                           member("lite6_io", "ufactory.lite6_controller_io", "controller_io"),
                           V(std::move(tool_io))})},
               {"policies", V(Object{})}}),
      {});
  intent.inactive_members.push_back(
      {"lite6_robot", "ufactory.lite6", "lite6", "arm", "<robot name=\"lite6\"/>"});
  const auto profiles = d::detail::resolve_activation_component_profiles(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), intent);
  d::detail::expand_activation_machine_members(intent, profiles);
  const auto &rows = *intent.components.at(1)
                          .configuration.find_object_member("runtime_configuration")
                          ->find_object_member("io_signals");
  const c::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      c::GraphRevision("graph.io"),
      c::CellId("cell.io"),
      {{c::RuntimeUnitId("host.io"),
        c::RuntimeProfileId("profile.io"),
        c::RuntimeProfileRevision("revision.io"),
        {},
        {{"io_signals", json(rows)},
         {"controller_profiles", c::JsonValueDTO{c::JsonValueDTO::Array{}}},
         {"initial_joint_positions_rad", c::JsonValueDTO{c::JsonValueDTO::Array{}}},
         {"motion_target_limits", c::JsonValueDTO{c::JsonValueDTO::Array{}}}}}},
      {},
      {{c::ControllableComponentId("lite6_io"),
        c::ComponentTypeId("io.module"),
        c::ComponentTypeRevision("revision.io"),
        {},
        {c::RuntimeUnitId("host.io")},
        {}}},
      {},
      {},
      {}};
  const auto signals = d::ResolveRetainedIoSignalCatalog(graph);
  ASSERT_TRUE(signals.ok()) << signals.error().message;
  const auto ci0 = std::find_if(signals.value().begin(), signals.value().end(),
                                [](const auto &row) { return row.descriptor.signal_id.name == "CI0"; });
  ASSERT_NE(ci0, signals.value().end());
  EXPECT_EQ(ci0->provider.profile, d::kComponentAdapterIoProfile);
  EXPECT_EQ(ci0->descriptor.liveness, c::IoSignalLivenessMode::kPeriodic);
  EXPECT_EQ(ci0->descriptor.freshness.mode, c::IoFreshnessMode::kSampleAge);
  EXPECT_EQ(ci0->descriptor.freshness.maximum_age_ns, std::optional<c::Duration>(100'000'000));
  const auto condition = V(Object{
      {"compare",
       V(Object{{"operand",
                 V(Object{{"signal", V(Object{{"controllable_component_id", V("lite6_io")},
                                              {"signal_name", V("CI0")}})}})},
                {"op", V("==")},
                {"value", V(true)}})}});
  const auto compiled =
      d::detail::compile_authored_deployment_io_condition(condition, {}, signals.value());
  ASSERT_TRUE(compiled.ok()) << compiled.error().message;
  EXPECT_EQ(compiled.value().required_signal_sources,
            std::vector<c::InterruptSourceId>{c::InterruptSourceId("lite6_io.CI0")});
}

} // namespace

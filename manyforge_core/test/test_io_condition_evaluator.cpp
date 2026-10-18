#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <limits>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <utility>

namespace manyforge::core {
namespace {

IoConditionLimitsDTO limits() {
  return {8, 32, 64, 16, 128, 128};
}

IoConditionOperandDTO input(std::size_t index) {
  return IoConditionInputReferenceDTO{index};
}

IoConditionOperandDTO literal(IoConditionValueDTO value) {
  return value;
}

IoConditionNodeDTO compare(IoConditionComparison op, IoConditionOperandDTO left,
                           IoConditionOperandDTO right) {
  return IoConditionCompareDTO{op, std::move(left), std::move(right)};
}

IoConditionNodeDTO constant(bool value) {
  return compare(IoConditionComparison::kEqual, literal(value), literal(true));
}

IoConditionInputDTO key(std::string name, IoConditionValueKind kind) {
  return {IoConditionBlackboardKeyDTO{std::move(name)}, kind};
}

IoConditionDefinitionDTO single_input(IoConditionValueKind kind,
                                      IoConditionValueDTO comparison_value) {
  return {{key("key", kind)},
          {compare(IoConditionComparison::kEqual, input(0), literal(std::move(comparison_value)))}};
}

Result<bool> evaluate(const CheckedIoCondition& checked,
                      std::vector<IoConditionValueDTO> values = {}) {
  std::vector<std::uint8_t> workspace(checked.workspace_size());
  return EvaluateIoCondition(checked, values, workspace);
}

bool expected(IoConditionComparison op, int order) {
  switch (op) {
    case IoConditionComparison::kEqual:
      return order == 0;
    case IoConditionComparison::kNotEqual:
      return order != 0;
    case IoConditionComparison::kLess:
      return order < 0;
    case IoConditionComparison::kLessEqual:
      return order <= 0;
    case IoConditionComparison::kGreater:
      return order > 0;
    case IoConditionComparison::kGreaterEqual:
      return order >= 0;
  }
  return false;
}

constexpr std::array kComparisons{
    IoConditionComparison::kEqual,   IoConditionComparison::kNotEqual,
    IoConditionComparison::kLess,    IoConditionComparison::kLessEqual,
    IoConditionComparison::kGreater, IoConditionComparison::kGreaterEqual};

TEST(IoConditionEvaluatorTest, AllSixComparisonsPreserveIntegerAndFloatingOrdering) {
  struct Case {
    IoConditionValueDTO left;
    IoConditionValueDTO right;
    int order;
  };
  const std::vector<Case> cases{
      {std::int64_t(-7), std::int64_t(4), -1},
      {std::int64_t(4), std::int64_t(4), 0},
      {3.0, 2.0, 1},
      {-0.0, 0.0, 0},
      {std::int64_t(0), -0.5, 1},
      {std::int64_t(0), 0.5, -1},
      {std::int64_t(-2), -2.25, 1},
      {std::int64_t(-2), -1.75, -1},
      {std::int64_t(2), 2.0, 0},
  };
  for (const auto& item : cases) {
    for (const auto op : kComparisons) {
      const auto checked =
          CheckIoCondition({{}, {compare(op, literal(item.left), literal(item.right))}}, limits());
      ASSERT_TRUE(checked.ok());
      const auto result = evaluate(checked.value());
      ASSERT_TRUE(result.ok());
      EXPECT_EQ(result.value(), expected(op, item.order));
    }
  }
}

TEST(IoConditionEvaluatorTest, MixedNumbersStayExactBeyondDoubleIntegerPrecisionAndAtInt64Bounds) {
  struct Case {
    std::int64_t left;
    double right;
    int order;
  };
  const auto inf = std::numeric_limits<double>::infinity();
  const std::vector<Case> cases{
      {std::int64_t(9'007'199'254'740'993LL), 9'007'199'254'740'992.0, 1},
      {std::numeric_limits<std::int64_t>::max(), 0x1p63, -1},
      {std::numeric_limits<std::int64_t>::min(), -0x1p63, 0},
      {std::numeric_limits<std::int64_t>::min(), std::nextafter(-0x1p63, -inf), 1},
      {std::numeric_limits<std::int64_t>::max(), std::nextafter(0x1p63, 0.0), 1},
  };
  for (const auto& item : cases) {
    for (const auto op : kComparisons) {
      for (const bool reverse : {false, true}) {
        IoConditionValueDTO left_value = item.left;
        IoConditionValueDTO right_value = item.right;
        if (reverse) {
          left_value = item.right;
          right_value = item.left;
        }
        IoConditionOperandDTO left_operand = literal(std::move(left_value));
        IoConditionOperandDTO right_operand = literal(std::move(right_value));
        IoConditionNodeDTO comparison =
            compare(op, std::move(left_operand), std::move(right_operand));
        IoConditionDefinitionDTO definition;
        definition.nodes.push_back(std::move(comparison));
        const auto checked = CheckIoCondition(std::move(definition), limits());
        ASSERT_TRUE(checked.ok());
        const auto result = evaluate(checked.value());
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(result.value(), expected(op, reverse ? -item.order : item.order));
      }
    }
  }
}

TEST(IoConditionEvaluatorTest, BooleanAndStringEqualityHaveNoNumericOrStringCoercion) {
  for (const auto op : {IoConditionComparison::kEqual, IoConditionComparison::kNotEqual}) {
    const auto boolean =
        CheckIoCondition({{}, {compare(op, literal(false), literal(true))}}, limits());
    ASSERT_TRUE(boolean.ok());
    EXPECT_EQ(evaluate(boolean.value()).value(), op == IoConditionComparison::kNotEqual);
    const auto string =
        CheckIoCondition({{key("node_outcome.move", IoConditionValueKind::kString)},
                          {compare(op, input(0), literal(std::string("signal_trip")))}},
                         limits());
    ASSERT_TRUE(string.ok());
    EXPECT_EQ(evaluate(string.value(), {std::string("signal_trip")}).value(),
              op == IoConditionComparison::kEqual);
    EXPECT_EQ(evaluate(string.value(), {std::string("no_signal_trip")}).value(),
              op == IoConditionComparison::kNotEqual);
  }
  EXPECT_FALSE(
      CheckIoCondition(
          {{}, {compare(IoConditionComparison::kEqual, literal(true), literal(std::int64_t(1)))}},
          limits())
          .ok());
  EXPECT_FALSE(
      CheckIoCondition(
          {{}, {compare(IoConditionComparison::kEqual, literal(std::string("1")), literal(1.0))}},
          limits())
          .ok());
  for (const auto op : {IoConditionComparison::kLess, IoConditionComparison::kLessEqual,
                        IoConditionComparison::kGreater, IoConditionComparison::kGreaterEqual}) {
    EXPECT_FALSE(
        CheckIoCondition({{}, {compare(op, literal(false), literal(true))}}, limits()).ok());
    EXPECT_FALSE(
        CheckIoCondition({{}, {compare(op, literal(std::string("a")), literal(std::string("b")))}},
                         limits())
            .ok());
  }
}

TEST(IoConditionEvaluatorTest, CombinesBooleanSignalNumericKeyAndOutcomeKeyWithAndOrNot) {
  IoConditionDefinitionDTO definition{
      {{IoSignalId{ControllableComponentId("component.io"), "input"},
        IoConditionValueKind::kBoolean},
       key("pressure", IoConditionValueKind::kFloat64),
       key("node_outcome.move", IoConditionValueKind::kString)},
      {compare(IoConditionComparison::kEqual, input(0), literal(true)),
       compare(IoConditionComparison::kLess, input(1), literal(3.0)),
       compare(IoConditionComparison::kEqual, input(2), literal(std::string("failed"))),
       IoConditionAnyDTO{{1, 2}}, IoConditionAllDTO{{0, 3}}, IoConditionNotDTO{4},
       IoConditionNotDTO{5}}};
  const auto checked = CheckIoCondition(definition, limits());
  ASSERT_TRUE(checked.ok());
  EXPECT_EQ(checked.value().inputs().size(), 3U);
  std::vector<std::uint8_t> workspace(checked.value().workspace_size());
  const auto* storage = workspace.data();
  std::vector<IoConditionValueDTO> values{true, 1.5, std::string("completed")};
  EXPECT_TRUE(EvaluateIoCondition(checked.value(), values, workspace).value());
  values = {true, 4.0, std::string("failed")};
  EXPECT_TRUE(EvaluateIoCondition(checked.value(), values, workspace).value());
  values = {false, 1.5, std::string("failed")};
  EXPECT_FALSE(EvaluateIoCondition(checked.value(), values, workspace).value());
  EXPECT_EQ(workspace.data(), storage);
  // Caller mutation cannot change the already checked immutable plan.
  definition.nodes.back() = constant(true);
  EXPECT_FALSE(EvaluateIoCondition(checked.value(), values, workspace).value());
}

TEST(IoConditionEvaluatorTest, MissingInvalidTypedValuesAndWorkspaceReturnErrorNeverFalse) {
  const auto checked =
      CheckIoCondition(single_input(IoConditionValueKind::kInt64, std::int64_t(4)), limits());
  ASSERT_TRUE(checked.ok());
  const auto absent = evaluate(checked.value());
  ASSERT_FALSE(absent.ok());
  EXPECT_EQ(absent.error().details.at("reason"), "io_condition_input_count_mismatch");
  const auto wrong_type = evaluate(checked.value(), {4.0});
  ASSERT_FALSE(wrong_type.ok());
  EXPECT_EQ(wrong_type.error().details.at("reason"), "io_condition_input_type_mismatch");
  const std::vector<IoConditionValueDTO> values{std::int64_t(4)};
  EXPECT_FALSE(EvaluateIoCondition(checked.value(), values, {}).ok());
  EXPECT_TRUE(evaluate(checked.value(), values).value());
}

TEST(IoConditionEvaluatorTest, NonfiniteLiteralsAndKnownValuesRefuseBeforeBooleanEvaluation) {
  const auto inf = std::numeric_limits<double>::infinity();
  for (const auto invalid : {inf, -inf, std::numeric_limits<double>::quiet_NaN()}) {
    EXPECT_FALSE(
        CheckIoCondition(single_input(IoConditionValueKind::kFloat64, invalid), limits()).ok());
    IoConditionDefinitionDTO definition{
        {key("number", IoConditionValueKind::kFloat64)},
        {constant(true), compare(IoConditionComparison::kLess, input(0), literal(2.0)),
         IoConditionAnyDTO{{0, 1}}}};
    const auto checked = CheckIoCondition(std::move(definition), limits());
    ASSERT_TRUE(checked.ok());
    const auto result = evaluate(checked.value(), {invalid});
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().details.at("reason"), "io_condition_value_nonfinite");
  }
}

TEST(IoConditionEvaluatorTest, RefusesForwardCyclesUnreachableNodesUnusedAndDuplicateInputs) {
  EXPECT_FALSE(CheckIoCondition({{}, {IoConditionNotDTO{0}}}, limits()).ok());
  EXPECT_FALSE(
      CheckIoCondition({{}, {constant(true), IoConditionNotDTO{2}, IoConditionNotDTO{1}}}, limits())
          .ok());
  EXPECT_FALSE(CheckIoCondition({{}, {constant(false), constant(true)}}, limits()).ok());
  EXPECT_FALSE(CheckIoCondition({{key("unused", IoConditionValueKind::kBoolean)}, {constant(true)}},
                                limits())
                   .ok());
  auto duplicate = single_input(IoConditionValueKind::kBoolean, true);
  duplicate.inputs.push_back(duplicate.inputs.front());
  EXPECT_FALSE(CheckIoCondition(std::move(duplicate), limits()).ok());
  auto physical_number = single_input(IoConditionValueKind::kInt64, std::int64_t(1));
  physical_number.inputs.front().source =
      IoSignalId{ControllableComponentId("component.io"), "input"};
  EXPECT_FALSE(CheckIoCondition(std::move(physical_number), limits()).ok());
  EXPECT_FALSE(
      CheckIoCondition({{}, {compare(IoConditionComparison::kEqual, input(0), literal(true))}},
                       limits())
          .ok());
}

TEST(IoConditionEvaluatorTest, EnforcesExplicitNodeDepthEdgeIdentityAndStringBounds) {
  auto small = limits();
  small.maximum_nodes = 1;
  EXPECT_FALSE(CheckIoCondition({{}, {constant(true), IoConditionNotDTO{0}}}, small).ok());
  small = limits();
  small.maximum_depth = 1;
  EXPECT_FALSE(CheckIoCondition({{}, {constant(true), IoConditionNotDTO{0}}}, small).ok());
  small = limits();
  small.maximum_edges = 1;
  EXPECT_FALSE(
      CheckIoCondition({{}, {constant(true), constant(false), IoConditionAllDTO{{0, 1}}}}, small)
          .ok());
  EXPECT_FALSE(CheckIoCondition({{}, {constant(true), IoConditionAllDTO{{0}}}}, limits()).ok());
  auto empty_key = single_input(IoConditionValueKind::kBoolean, true);
  empty_key.inputs.front().source = IoConditionBlackboardKeyDTO{""};
  EXPECT_FALSE(CheckIoCondition(std::move(empty_key), limits()).ok());
  small = limits();
  small.maximum_string_bytes = 3;
  EXPECT_FALSE(
      CheckIoCondition(single_input(IoConditionValueKind::kString, std::string("long")), small)
          .ok());
  const auto checked =
      CheckIoCondition(single_input(IoConditionValueKind::kString, std::string("yes")), small);
  ASSERT_TRUE(checked.ok());
  EXPECT_FALSE(evaluate(checked.value(), {std::string("long")}).ok());
  EXPECT_TRUE(evaluate(checked.value(), {std::string("yes")}).value());
}

TEST(IoConditionEvaluatorTest, BorrowedTypedInputsReuseSnapshotAndRejectUnknownBeforeEvaluation) {
  IoConditionDefinitionDTO definition;
  definition.inputs.push_back({IoConditionBlackboardKeyDTO{"text"}, IoConditionValueKind::kString});
  definition.nodes.push_back(IoConditionCompareDTO{IoConditionComparison::kEqual,
                                                   IoConditionInputReferenceDTO{0U},
                                                   IoConditionValueDTO{std::string("done")}});
  auto checked = CheckIoCondition(std::move(definition), {1U, 1U, 1U, 1U, 128U, 4096U});
  ASSERT_TRUE(checked.ok());
  IoConditionValueDTO value{std::string("done")};
  std::vector<const IoConditionValueDTO*> inputs{&value};
  std::vector<std::uint8_t> workspace(checked.value().workspace_size());
  const auto matched = EvaluateIoCondition(checked.value(), inputs, workspace);
  ASSERT_TRUE(matched.ok());
  EXPECT_TRUE(matched.value());
  std::get<std::string>(value) = "waiting";
  EXPECT_FALSE(EvaluateIoCondition(checked.value(), inputs, workspace).value());
  inputs[0] = nullptr;
  const auto unknown = EvaluateIoCondition(checked.value(), inputs, workspace);
  ASSERT_FALSE(unknown.ok());
  EXPECT_EQ(unknown.error().details.at("reason"), "io_condition_input_unknown");
}

}  // namespace
}  // namespace manyforge::core

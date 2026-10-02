#include <gtest/gtest.h>
#include <yaml.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "manyforge_deployment/expression.hpp"

namespace manyforge::deployment::expression {
namespace {

static_assert(kMaxTextLength == 1024U);
static_assert(kMaxNestingDepth == 64U);
static_assert(kMaxTokens == 512U);
static_assert(kMaxSyntaxNodes == 256U);
static_assert(kMaxEvaluationSteps == 256U);

const std::set<std::string> kCodes{
    "text_too_long",
    "invalid_utf8",
    "invalid_character",
    "unterminated_string",
    "invalid_escape",
    "invalid_number",
    "integer_literal_out_of_range",
    "float_literal_out_of_range",
    "invalid_name",
    "unknown_function",
    "wrong_argument_count",
    "unexpected_token",
    "unexpected_end",
    "too_many_tokens",
    "nesting_too_deep",
    "syntax_tree_too_large",
    "unknown_name",
    "unknown_field",
    "type_mismatch",
    "destination_type_mismatch",
    "division_by_zero",
    "integer_overflow",
    "non_finite_result",
    "negative_remainder_operand",
    "sqrt_of_negative",
    "clamp_bounds_inverted",
    "missing_value",
    "invalid_value",
    "evaluation_step_limit",
};

// ------------------------------------------------------------ YAML reading

struct Yaml final {
  bool is_scalar = false;
  std::string scalar;
  std::vector<Yaml> items;
  std::vector<std::string> keys;
  std::vector<Yaml> values;

  const Yaml *find(std::string_view key) const {
    for (std::size_t index = 0; index < keys.size(); ++index) {
      if (keys[index] == key) {
        return &values[index];
      }
    }
    return nullptr;
  }

  const Yaml &at(std::string_view key) const {
    const auto *found = find(key);
    if (found == nullptr) {
      throw std::runtime_error("conformance entry lacks '" + std::string(key) +
                               "'");
    }
    return *found;
  }
};

Yaml convert(yaml_document_t &document, yaml_node_t *node) {
  Yaml result;
  switch (node->type) {
  case YAML_SCALAR_NODE:
    result.is_scalar = true;
    result.scalar.assign(
        reinterpret_cast<const char *>(node->data.scalar.value),
        node->data.scalar.length);
    break;
  case YAML_SEQUENCE_NODE:
    for (auto *item = node->data.sequence.items.start;
         item < node->data.sequence.items.top; ++item) {
      result.items.push_back(
          convert(document, yaml_document_get_node(&document, *item)));
    }
    break;
  case YAML_MAPPING_NODE:
    for (auto *pair = node->data.mapping.pairs.start;
         pair < node->data.mapping.pairs.top; ++pair) {
      result.keys.push_back(
          convert(document, yaml_document_get_node(&document, pair->key))
              .scalar);
      result.values.push_back(
          convert(document, yaml_document_get_node(&document, pair->value)));
    }
    break;
  default:
    throw std::runtime_error("conformance file has an empty YAML node");
  }
  return result;
}

Yaml load_conformance() {
  std::ifstream stream(MANYFORGE_EXPRESSION_CONFORMANCE, std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(stream)),
                         std::istreambuf_iterator<char>());
  yaml_parser_t parser;
  yaml_parser_initialize(&parser);
  yaml_parser_set_input_string(
      &parser, reinterpret_cast<const unsigned char *>(text.data()),
      text.size());
  yaml_document_t document;
  if (yaml_parser_load(&parser, &document) == 0) {
    yaml_parser_delete(&parser);
    throw std::runtime_error("conformance file is not valid YAML");
  }
  auto root = convert(document, yaml_document_get_root_node(&document));
  yaml_document_delete(&document);
  yaml_parser_delete(&parser);
  return root;
}

ValueKind kind_of(const std::string &name) {
  static const std::vector<std::pair<std::string, ValueKind>> kinds{
      {"bool", ValueKind::kBool},       {"int64", ValueKind::kInt64},
      {"float64", ValueKind::kFloat64}, {"string", ValueKind::kString},
      {"pose", ValueKind::kPose},
  };
  for (const auto &[text, kind] : kinds) {
    if (text == name) {
      return kind;
    }
  }
  throw std::runtime_error("unknown conformance type '" + name + "'");
}

ValueType type_of(const Yaml &node) {
  if (node.is_scalar) {
    return ValueType{kind_of(node.scalar), {}};
  }
  ValueType type{ValueKind::kRecord, {}};
  for (std::size_t index = 0; index < node.keys.size(); ++index) {
    type.fields.push_back({node.keys[index], type_of(node.values[index])});
  }
  return type;
}

std::size_t unsigned_of(const Yaml &node) {
  return static_cast<std::size_t>(std::stoull(node.scalar));
}

double double_of(const Yaml &node) {
  char *end = nullptr;
  const double value = std::strtod(node.scalar.c_str(), &end);
  if (end != node.scalar.c_str() + node.scalar.size()) {
    throw std::runtime_error("malformed conformance float '" + node.scalar +
                             "'");
  }
  return value;
}

template <std::size_t Size>
std::array<double, Size> doubles_of(const Yaml &node) {
  if (node.items.size() != Size) {
    throw std::runtime_error("conformance pose list has the wrong length");
  }
  std::array<double, Size> result{};
  for (std::size_t index = 0; index < Size; ++index) {
    result[index] = double_of(node.items[index]);
  }
  return result;
}

Value value_of(const Yaml &node, const ValueType &type) {
  switch (type.kind) {
  case ValueKind::kBool:
    if (node.scalar != "true" && node.scalar != "false") {
      throw std::runtime_error("malformed conformance bool");
    }
    return Value{node.scalar == "true"};
  case ValueKind::kInt64: {
    std::int64_t value = 0;
    const auto *end = node.scalar.data() + node.scalar.size();
    const auto parsed = std::from_chars(node.scalar.data(), end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
      throw std::runtime_error("malformed conformance int64");
    }
    return Value{value};
  }
  case ValueKind::kFloat64:
    return Value{double_of(node)};
  case ValueKind::kString:
    return Value{node.scalar};
  case ValueKind::kPose: {
    Pose pose;
    pose.position_m = doubles_of<3>(node.at("position_m"));
    const auto &orientation = node.at("orientation");
    if (const auto *rpy = orientation.find("rpy_deg")) {
      pose.orientation = RpyDeg{doubles_of<3>(*rpy)};
    } else {
      pose.orientation = Xyzw{doubles_of<4>(orientation.at("xyzw"))};
    }
    return Value{pose};
  }
  case ValueKind::kRecord: {
    Record record;
    for (std::size_t index = 0; index < node.keys.size(); ++index) {
      const auto field = std::find_if(
          type.fields.begin(), type.fields.end(),
          [&](const auto &row) { return row.name == node.keys[index]; });
      if (field == type.fields.end()) {
        throw std::runtime_error("conformance record field is undeclared");
      }
      record.push_back(
          {field->name, value_of(node.values[index], field->type)});
    }
    return Value{record};
  }
  }
  throw std::runtime_error("unreachable conformance type");
}

TypeEnvironment environment_of(const Yaml &node) {
  TypeEnvironment environment;
  const auto fill = [](const Yaml *section, auto &map) {
    if (section == nullptr) {
      return;
    }
    for (std::size_t index = 0; index < section->keys.size(); ++index) {
      map.emplace(section->keys[index], type_of(section->values[index]));
    }
  };
  fill(node.find("parameters"), environment.parameters);
  fill(node.find("keys"), environment.keys);
  return environment;
}

ValueSnapshot snapshot_of(const Yaml &node, const TypeEnvironment &types) {
  ValueSnapshot snapshot;
  const auto fill = [](const Yaml *section, const auto &declared, auto &map) {
    if (section == nullptr) {
      return;
    }
    for (std::size_t index = 0; index < section->keys.size(); ++index) {
      map.emplace(
          section->keys[index],
          value_of(section->values[index], declared.at(section->keys[index])));
    }
  };
  fill(node.find("parameters"), types.parameters, snapshot.parameters);
  fill(node.find("keys"), types.keys, snapshot.keys);
  return snapshot;
}

Limits limits_of(const Yaml *node) {
  Limits limits;
  if (node == nullptr) {
    return limits;
  }
  for (std::size_t index = 0; index < node->keys.size(); ++index) {
    const auto &key = node->keys[index];
    const auto value = unsigned_of(node->values[index]);
    if (key == "text_length") {
      limits.text_length = value;
    } else if (key == "nesting_depth") {
      limits.nesting_depth = value;
    } else if (key == "tokens") {
      limits.tokens = value;
    } else if (key == "syntax_nodes") {
      limits.syntax_nodes = value;
    } else if (key == "evaluation_steps") {
      limits.evaluation_steps = value;
    } else {
      throw std::runtime_error("unknown conformance limit '" + key + "'");
    }
  }
  return limits;
}

std::string describe(const Diagnostic &diagnostic) {
  return diagnostic.code + " at " + std::to_string(diagnostic.position) + "+" +
         std::to_string(diagnostic.length) + " (" + diagnostic.name +
         "): " + diagnostic.message;
}

void expect_diagnostic(const Yaml &expected, const Diagnostic &actual) {
  EXPECT_EQ(expected.at("code").scalar, actual.code) << describe(actual);
  EXPECT_FALSE(actual.message.empty());
  if (const auto *position = expected.find("position")) {
    EXPECT_EQ(unsigned_of(*position), actual.position) << describe(actual);
  }
  if (const auto *length = expected.find("length")) {
    EXPECT_EQ(unsigned_of(*length), actual.length) << describe(actual);
  }
  if (const auto *name = expected.find("name")) {
    EXPECT_EQ(name->scalar, actual.name) << describe(actual);
  }
}

void expect_value(const Yaml &test_case, ValueKind destination,
                  const Value &actual) {
  const auto expected =
      value_of(test_case.at("result"), ValueType{destination, {}});
  ASSERT_EQ(expected.data.index(), actual.data.index());
  if (destination == ValueKind::kFloat64) {
    const double want = std::get<double>(expected.data);
    const double got = std::get<double>(actual.data);
    if (const auto *tolerance = test_case.find("tolerance")) {
      EXPECT_NEAR(want, got, double_of(*tolerance));
    } else {
      EXPECT_EQ(want, got);
    }
    return;
  }
  if (destination == ValueKind::kBool) {
    EXPECT_EQ(std::get<bool>(expected.data), std::get<bool>(actual.data));
  } else if (destination == ValueKind::kInt64) {
    EXPECT_EQ(std::get<std::int64_t>(expected.data),
              std::get<std::int64_t>(actual.data));
  } else {
    EXPECT_EQ(std::get<std::string>(expected.data),
              std::get<std::string>(actual.data));
  }
}

void run_case(const Yaml &root, const Yaml &test_case) {
  const auto *error = test_case.find("error");
  const auto phase =
      error == nullptr ? std::string() : error->at("phase").scalar;
  const auto parsed = parse(test_case.at("expression").scalar,
                            limits_of(test_case.find("limits")));
  if (phase == "parse") {
    ASSERT_TRUE(std::holds_alternative<Diagnostic>(parsed));
    expect_diagnostic(*error, std::get<Diagnostic>(parsed));
    return;
  }
  if (const auto *diagnostic = std::get_if<Diagnostic>(&parsed)) {
    FAIL() << describe(*diagnostic);
  }
  const auto *case_environment = test_case.find("environment");
  const auto environment = environment_of(
      case_environment ? *case_environment : root.at("environment"));
  const auto destination = kind_of(test_case.at("destination").scalar);
  const auto checked =
      check(std::get<ParsedExpression>(parsed), environment, destination);
  if (phase == "check") {
    ASSERT_TRUE(std::holds_alternative<std::vector<Diagnostic>>(checked));
    const auto &diagnostics = std::get<std::vector<Diagnostic>>(checked);
    expect_diagnostic(*error, diagnostics.front());
    if (const auto *count = test_case.find("diagnostic_count")) {
      EXPECT_EQ(unsigned_of(*count), diagnostics.size());
    }
    return;
  }
  if (const auto *diagnostics =
          std::get_if<std::vector<Diagnostic>>(&checked)) {
    FAIL() << describe(diagnostics->front());
  }
  const auto *case_values = test_case.find("values");
  const auto values =
      snapshot_of(case_values ? *case_values : root.at("values"), environment);
  const auto &expression = std::get<CheckedExpression>(checked);
  EXPECT_EQ(destination, expression.result_kind());
  const auto result = evaluate(expression, values);
  if (phase == "evaluate") {
    ASSERT_TRUE(std::holds_alternative<Diagnostic>(result));
    expect_diagnostic(*error, std::get<Diagnostic>(result));
    return;
  }
  ASSERT_EQ(phase, "") << "unknown phase";
  if (const auto *diagnostic = std::get_if<Diagnostic>(&result)) {
    FAIL() << describe(*diagnostic);
  }
  expect_value(test_case, destination, std::get<Value>(result));
}

TEST(ExpressionConformance, EveryCasePasses) {
  const auto root = load_conformance();
  ASSERT_EQ(root.at("schema").scalar, "manyforge.expression_conformance.v1");
  const auto &cases = root.at("cases").items;
  ASSERT_GT(cases.size(), 200U);
  std::set<std::string> ids;
  std::set<std::string> codes;
  for (const auto &test_case : cases) {
    const auto &id = test_case.at("id").scalar;
    SCOPED_TRACE(id);
    EXPECT_TRUE(ids.insert(id).second) << "repeated case id";
    if (const auto *error = test_case.find("error")) {
      codes.insert(error->at("code").scalar);
    }
    run_case(root, test_case);
  }
  // invalid_utf8 cannot be written in a YAML file; the pathological tests
  // below cover it.
  codes.insert("invalid_utf8");
  EXPECT_EQ(kCodes, codes) << "every diagnostic code has a conformance case";
}

// ------------------------------------------------------------ helpers

TypeEnvironment sample_environment() {
  return environment_of(load_conformance().at("environment"));
}

ValueSnapshot sample_values(const TypeEnvironment &environment) {
  return snapshot_of(load_conformance().at("values"), environment);
}

Diagnostic parse_error(std::string_view text) {
  auto parsed = parse(text);
  if (!std::holds_alternative<Diagnostic>(parsed)) {
    ADD_FAILURE() << "expected a parse diagnostic";
    return {};
  }
  return std::get<Diagnostic>(parsed);
}

Value value(std::string_view text, ValueKind destination,
            const TypeEnvironment &environment = {},
            const ValueSnapshot &values = {}) {
  auto parsed = parse(text);
  if (const auto *diagnostic = std::get_if<Diagnostic>(&parsed)) {
    throw std::runtime_error(describe(*diagnostic));
  }
  auto checked =
      check(std::get<ParsedExpression>(parsed), environment, destination);
  if (const auto *diagnostics =
          std::get_if<std::vector<Diagnostic>>(&checked)) {
    throw std::runtime_error(describe(diagnostics->front()));
  }
  auto result = evaluate(std::get<CheckedExpression>(checked), values);
  if (const auto *diagnostic = std::get_if<Diagnostic>(&result)) {
    throw std::runtime_error(describe(*diagnostic));
  }
  return std::get<Value>(result);
}

std::string repeat(std::string_view unit, std::size_t count) {
  std::string text;
  for (std::size_t index = 0; index < count; ++index) {
    text += unit;
  }
  return text;
}

// ----------------------------------------------------------- API contract

TEST(Expression, LimitsCanOnlyBeNarrowed) {
  EXPECT_THROW((void)parse("1", Limits{.text_length = kMaxTextLength + 1}),
               std::invalid_argument);
  EXPECT_THROW((void)parse("1", Limits{.nesting_depth = 0}),
               std::invalid_argument);
  EXPECT_THROW((void)parse("1", Limits{.tokens = kMaxTokens + 1}),
               std::invalid_argument);
  EXPECT_THROW((void)parse("1", Limits{.syntax_nodes = kMaxSyntaxNodes + 1}),
               std::invalid_argument);
  EXPECT_THROW(
      (void)parse("1", Limits{.evaluation_steps = kMaxEvaluationSteps + 1}),
      std::invalid_argument);
}

TEST(Expression, DestinationMustBeAScalar) {
  const auto parsed = std::get<ParsedExpression>(parse("1"));
  EXPECT_THROW((void)check(parsed, {}, ValueKind::kPose),
               std::invalid_argument);
  EXPECT_THROW((void)check(parsed, {}, ValueKind::kRecord),
               std::invalid_argument);
}

TEST(Expression, MalformedRecordTypesAreRejected) {
  const auto parsed = std::get<ParsedExpression>(parse("key.r.a"));
  const auto rejects = [&](ValueType type) {
    TypeEnvironment environment;
    environment.keys.emplace("r", std::move(type));
    EXPECT_THROW((void)check(parsed, environment, ValueKind::kFloat64),
                 std::invalid_argument);
  };
  rejects(ValueType{ValueKind::kRecord, {}});
  rejects(ValueType{
      ValueKind::kRecord,
      {{"a", {ValueKind::kFloat64, {}}}, {"a", {ValueKind::kFloat64, {}}}}});
  rejects(ValueType{ValueKind::kRecord, {{"a.b", {ValueKind::kFloat64, {}}}}});
  rejects(ValueType{ValueKind::kFloat64, {{"a", {ValueKind::kFloat64, {}}}}});
}

TEST(Expression, ReferencesNameEachDeclaredValueOnce) {
  const auto environment = sample_environment();
  const auto parsed = std::get<ParsedExpression>(parse(
      "key.index + key.index + param.goal.x + "
      "key.perception.grasp_plan.approach_pose.z + key.perception.level"));
  const auto checked = std::get<CheckedExpression>(
      check(parsed, environment, ValueKind::kFloat64));
  std::vector<Reference> names;
  for (const auto &[reference, type] : checked.references()) {
    names.push_back(reference);
    EXPECT_EQ(type.kind, reference.name == "index"  ? ValueKind::kInt64
                         : reference.name == "goal" ? ValueKind::kPose
                                                    : ValueKind::kRecord);
  }
  EXPECT_EQ(names, (std::vector<Reference>{
                       {Source::kParameter, "goal"},
                       {Source::kKey, "index"},
                       {Source::kKey, "perception"},
                       {Source::kKey, "perception.grasp_plan"},
                   }));
  EXPECT_EQ(checked.text(), parsed.text());
  const auto constant = std::get<CheckedExpression>(
      check(std::get<ParsedExpression>(parse("1 + 2")), environment,
            ValueKind::kInt64));
  EXPECT_TRUE(constant.references().empty());
}

TEST(Expression, OneCheckedExpressionEvaluatesManySnapshots) {
  const auto environment = sample_environment();
  auto values = sample_values(environment);
  const auto checked = std::get<CheckedExpression>(
      check(std::get<ParsedExpression>(parse("key.index + 1")), environment,
            ValueKind::kInt64));
  for (std::int64_t index = 0; index < 5; ++index) {
    values.keys["index"] = Value{index};
    const auto result = evaluate(checked, values);
    ASSERT_TRUE(std::holds_alternative<Value>(result));
    EXPECT_EQ(std::get<std::int64_t>(std::get<Value>(result).data), index + 1);
  }
}

TEST(Expression, SuppliedValuesMustMatchTheirDeclaredType) {
  const auto environment = sample_environment();
  const auto evaluate_with = [&](std::string_view text, ValueSnapshot values) {
    const auto checked = std::get<CheckedExpression>(
        check(std::get<ParsedExpression>(parse(text)), environment,
              ValueKind::kFloat64));
    return evaluate(checked, values);
  };
  ValueSnapshot values = sample_values(environment);
  values.keys["index"] = Value{1.5};
  auto result = evaluate_with("key.index", values);
  ASSERT_TRUE(std::holds_alternative<Diagnostic>(result));
  EXPECT_EQ(std::get<Diagnostic>(result).code, "invalid_value");
  EXPECT_EQ(std::get<Diagnostic>(result).name, "key.index");

  values = sample_values(environment);
  auto &plan = std::get<Record>(values.keys["perception.grasp_plan"].data);
  plan.push_back({"extra", Value{1.0}});
  result = evaluate_with("key.perception.grasp_plan.score", values);
  ASSERT_TRUE(std::holds_alternative<Diagnostic>(result));
  EXPECT_EQ(std::get<Diagnostic>(result).code, "invalid_value");

  values = sample_values(environment);
  values.parameters["goal"] =
      Value{Pose{{0.0, std::nan(""), 0.0}, RpyDeg{{0.0, 0.0, 0.0}}}};
  result = evaluate_with("param.goal.x", values);
  ASSERT_TRUE(std::holds_alternative<Diagnostic>(result));
  EXPECT_EQ(std::get<Diagnostic>(result).code, "invalid_value");
}

// ----------------------------------------------- pathological inputs

TEST(ExpressionPathological, TextLengthCountsCodePoints) {
  EXPECT_EQ(
      std::get<std::int64_t>(
          value("1" + repeat(" ", kMaxTextLength - 1), ValueKind::kInt64).data),
      1);
  auto error = parse_error("1" + repeat(" ", kMaxTextLength));
  EXPECT_EQ(error.code, "text_too_long");
  EXPECT_EQ(error.position, kMaxTextLength);
  EXPECT_EQ(error.length, 1U);
  const auto wide = "\"" + repeat("é", kMaxTextLength - 2) + "\"";
  EXPECT_EQ(std::get<std::string>(value(wide, ValueKind::kString).data),
            repeat("é", kMaxTextLength - 2));
  EXPECT_EQ(parse_error("\"" + repeat("é", kMaxTextLength - 1) + "\"").code,
            "text_too_long");
  error = parse_error(repeat("(", 1U << 20U));
  EXPECT_EQ(error.code, "text_too_long");
  EXPECT_EQ(error.position, kMaxTextLength);
}

TEST(ExpressionPathological, DeepNesting) {
  const auto nested = [](std::size_t depth) {
    return repeat("(", depth) + "1" + repeat(")", depth);
  };
  EXPECT_EQ(std::get<std::int64_t>(
                value(nested(kMaxNestingDepth), ValueKind::kInt64).data),
            1);
  auto error = parse_error(nested(kMaxNestingDepth + 1));
  EXPECT_EQ(error.code, "nesting_too_deep");
  EXPECT_EQ(error.position, kMaxNestingDepth + 1);
  EXPECT_EQ(parse_error(repeat("(", kMaxTokens)).code, "nesting_too_deep");
  EXPECT_EQ(
      std::get<std::int64_t>(
          value(repeat("-", kMaxNestingDepth) + "1", ValueKind::kInt64).data),
      1);
  EXPECT_EQ(parse_error(repeat("-", kMaxNestingDepth + 1) + "1").code,
            "nesting_too_deep");
  EXPECT_EQ(parse_error(repeat("!", kMaxTokens)).code, "nesting_too_deep");
  EXPECT_EQ(std::get<std::int64_t>(
                value(repeat("true ? 1 : ", kMaxNestingDepth) + "2",
                      ValueKind::kInt64)
                    .data),
            1);
  EXPECT_EQ(parse_error(repeat("true ? 1 : ", kMaxNestingDepth + 1) + "2").code,
            "nesting_too_deep");
  EXPECT_EQ(parse_error(repeat("abs(", kMaxNestingDepth + 1) + "1" +
                        repeat(")", kMaxNestingDepth + 1))
                .code,
            "nesting_too_deep");
}

TEST(ExpressionPathological, TokenAndSyntaxTreeBounds) {
  auto error = parse_error(repeat("1+", 300) + "1");
  EXPECT_EQ(error.code, "too_many_tokens");
  EXPECT_EQ(error.position, kMaxTokens);
  const auto sum = [](std::size_t terms) {
    return "1" + repeat("+1", terms - 1);
  };
  // n terms make 2n - 1 nodes.
  EXPECT_EQ(std::get<std::int64_t>(
                value(sum(kMaxSyntaxNodes / 2), ValueKind::kInt64).data),
            static_cast<std::int64_t>(kMaxSyntaxNodes / 2));
  EXPECT_EQ(parse_error(sum(kMaxSyntaxNodes / 2 + 1)).code,
            "syntax_tree_too_large");
}

TEST(ExpressionPathological, LongNumbers) {
  auto error = parse_error(repeat("1", 1000));
  EXPECT_EQ(error.code, "integer_literal_out_of_range");
  EXPECT_EQ(error.length, 1000U);
  EXPECT_EQ(std::get<double>(
                value("1." + repeat("0", 1000), ValueKind::kFloat64).data),
            1.0);
  EXPECT_EQ(parse_error(repeat("9", 400) + ".0").code,
            "float_literal_out_of_range");
  EXPECT_EQ(parse_error("0." + repeat("0", 400) + "1").code,
            "float_literal_out_of_range");
  EXPECT_EQ(parse_error(repeat("0", 1000)).code, "invalid_number");
}

TEST(ExpressionPathological, StringsAndBytes) {
  auto error = parse_error("\"" + repeat("a", 1000));
  EXPECT_EQ(error.code, "unterminated_string");
  EXPECT_EQ(error.position, 0U);
  EXPECT_EQ(error.length, 1001U);
  error = parse_error(std::string("1 + 2\0", 6));
  EXPECT_EQ(error.code, "invalid_character");
  EXPECT_EQ(error.position, 5U);
  error = parse_error(std::string("\"a\0b\"", 5));
  EXPECT_EQ(error.code, "invalid_character");
  EXPECT_EQ(error.position, 2U);
  EXPECT_EQ(parse_error(std::string(1, '\0')).code, "invalid_character");
  const std::vector<std::pair<std::string, std::size_t>> invalid{
      {"\xff", 0},         {"1 + \xc0\xaf", 4},
      {"\xed\xa0\x80", 0}, {"\xf4\x90\x80\x80", 0},
      {"\xe6\x97", 0},     {"\"\xe6\x97\xa5\xe6\x9c\xac\xff\"", 3},
      {"\x80", 0},
  };
  for (const auto &[text, position] : invalid) {
    error = parse_error(text);
    EXPECT_EQ(error.code, "invalid_utf8");
    EXPECT_EQ(error.position, position);
  }
  EXPECT_EQ(std::get<std::string>(
                value("\"\xf0\x9f\x99\x82\"", ValueKind::kString).data),
            "\xf0\x9f\x99\x82");
  error = parse_error("\xf0\x9f\x99\x82");
  EXPECT_EQ(error.code, "invalid_character");
  EXPECT_EQ(error.length, 1U);
  EXPECT_EQ(parse_error("\x7f").code, "invalid_character");
  EXPECT_EQ(parse_error("1 +\v2").code, "invalid_character");
}

// ------------------------------------------------------------------- fuzz

class Fuzzer final {
public:
  Fuzzer()
      : environment_(sample_environment()),
        values_(sample_values(environment_)) {}

  void run(const std::string &text) {
    const auto started = std::chrono::steady_clock::now();
    const auto parsed = parse(text);
    if (const auto *diagnostic = std::get_if<Diagnostic>(&parsed)) {
      structured(text, *diagnostic);
    } else {
      ++parsed_;
      for (const auto destination : {ValueKind::kBool, ValueKind::kInt64,
                                     ValueKind::kFloat64, ValueKind::kString}) {
        evaluate_as(text, std::get<ParsedExpression>(parsed), destination);
      }
    }
    slowest_ = std::max(slowest_, std::chrono::steady_clock::now() - started);
    ++runs_;
  }

  std::size_t parsed() const { return parsed_; }
  std::size_t evaluated() const { return evaluated_; }
  std::size_t runs() const { return runs_; }
  std::chrono::steady_clock::duration slowest() const { return slowest_; }

private:
  void evaluate_as(const std::string &text, const ParsedExpression &parsed,
                   ValueKind destination) {
    const auto checked = check(parsed, environment_, destination);
    if (const auto *diagnostics =
            std::get_if<std::vector<Diagnostic>>(&checked)) {
      ASSERT_FALSE(diagnostics->empty()) << text;
      for (const auto &diagnostic : *diagnostics) {
        structured(text, diagnostic);
      }
      return;
    }
    const auto result = evaluate(std::get<CheckedExpression>(checked), values_);
    if (const auto *diagnostic = std::get_if<Diagnostic>(&result)) {
      structured(text, *diagnostic);
      return;
    }
    ++evaluated_;
    const auto &data = std::get<Value>(result).data;
    switch (destination) {
    case ValueKind::kBool:
      EXPECT_TRUE(std::holds_alternative<bool>(data)) << text;
      break;
    case ValueKind::kInt64:
      EXPECT_TRUE(std::holds_alternative<std::int64_t>(data)) << text;
      break;
    case ValueKind::kFloat64:
      ASSERT_TRUE(std::holds_alternative<double>(data)) << text;
      EXPECT_TRUE(std::isfinite(std::get<double>(data))) << text;
      break;
    default:
      EXPECT_TRUE(std::holds_alternative<std::string>(data)) << text;
      break;
    }
  }

  static void structured(const std::string &text,
                         const Diagnostic &diagnostic) {
    EXPECT_TRUE(kCodes.contains(diagnostic.code))
        << diagnostic.code << " for " << text;
    EXPECT_FALSE(diagnostic.message.empty()) << text;
    std::size_t characters = 0;
    for (const char byte : text) {
      characters += (static_cast<unsigned char>(byte) & 0xC0U) != 0x80U;
    }
    EXPECT_LE(diagnostic.position + diagnostic.length,
              std::max(characters, kMaxTextLength))
        << describe(diagnostic) << " for " << text;
  }

  TypeEnvironment environment_;
  ValueSnapshot values_;
  std::size_t parsed_ = 0;
  std::size_t evaluated_ = 0;
  std::size_t runs_ = 0;
  std::chrono::steady_clock::duration slowest_{};
};

const std::vector<std::string> kFragments{"param.",
                                          "key.",
                                          "index",
                                          "part_pose",
                                          ".x",
                                          ".yaw",
                                          ".qw",
                                          "goal",
                                          "param.goal.roll",
                                          "key.part_pose.pitch",
                                          "key.index",
                                          "param.columns",
                                          "key.perception.grasp_plan.score",
                                          "key.perception.level",
                                          "key.missing",
                                          "param.big",
                                          "param.small",
                                          "param.huge",
                                          "param.zero",
                                          "param.neg",
                                          "key.fragile",
                                          "param.name",
                                          "(",
                                          ")",
                                          ",",
                                          "?",
                                          ":",
                                          "+",
                                          "-",
                                          "*",
                                          "/",
                                          "%",
                                          "!",
                                          "<",
                                          "<=",
                                          ">",
                                          ">=",
                                          "==",
                                          "!=",
                                          "&&",
                                          "||",
                                          "=",
                                          "&",
                                          "|",
                                          "0",
                                          "1",
                                          "7",
                                          "42",
                                          "9223372036854775807",
                                          "9223372036854775808",
                                          "1.5",
                                          "1e308",
                                          "1e-400",
                                          "0.0",
                                          "2.5e+2",
                                          "01",
                                          "1.",
                                          "\"",
                                          "\"s\"",
                                          "\\",
                                          "\"a\\\"b\"",
                                          "true",
                                          "false",
                                          "abs(",
                                          "min(",
                                          "max(",
                                          "clamp(",
                                          "floor(",
                                          "ceil(",
                                          "round(",
                                          "sqrt(",
                                          "sin(",
                                          "cos(",
                                          "tan(",
                                          "atan2(",
                                          "deg(",
                                          "rad(",
                                          "foo(",
                                          " ",
                                          "\t",
                                          "\n",
                                          "\xc3\xa9",
                                          "\xff",
                                          "\xc0\xaf",
                                          std::string(1, '\0'),
                                          ".",
                                          "..",
                                          "@"};

TEST(ExpressionFuzz, RandomFragmentsAreBoundedAndStructured) {
  std::mt19937_64 random(0x6578707265737331ULL);
  std::uniform_int_distribution<std::size_t> pick(0, kFragments.size() - 1);
  std::uniform_int_distribution<std::size_t> length(1, 48);
  Fuzzer fuzzer;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    std::string text;
    for (auto count = length(random); count > 0; --count) {
      text += kFragments[pick(random)];
    }
    fuzzer.run(text);
    if (HasFatalFailure() || HasNonfatalFailure()) {
      return;
    }
  }
  // The fixed seed must keep reaching evaluation, not only syntax errors.
  EXPECT_GT(fuzzer.parsed(), 90U);
  EXPECT_GT(fuzzer.evaluated(), 20U);
  EXPECT_LT(fuzzer.slowest(), std::chrono::milliseconds(250));
}

TEST(ExpressionFuzz, MutatedConformanceExpressionsAreBoundedAndStructured) {
  const auto root = load_conformance();
  std::vector<std::string> corpus;
  for (const auto &test_case : root.at("cases").items) {
    corpus.push_back(test_case.at("expression").scalar);
  }
  std::mt19937_64 random(0x6d75746174696f6eULL);
  const auto below = [&](std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
  };
  Fuzzer fuzzer;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    auto text = corpus[below(corpus.size())];
    for (auto mutations = 1 + below(4); mutations > 0; --mutations) {
      const auto at = below(text.size() + 1);
      switch (below(5)) {
      case 0:
        text.insert(at, 1, static_cast<char>(below(256)));
        break;
      case 1:
        if (at < text.size()) {
          text.erase(at, 1);
        }
        break;
      case 2:
        if (at < text.size()) {
          text[at] = static_cast<char>(below(256));
        }
        break;
      case 3: {
        const auto span = text.substr(at, below(16) + 1);
        text.insert(below(text.size() + 1), repeat(span, 1 + below(64)));
        break;
      }
      default:
        text.insert(at, kFragments[below(kFragments.size())]);
        break;
      }
    }
    fuzzer.run(text);
    if (HasFatalFailure() || HasNonfatalFailure()) {
      return;
    }
  }
  EXPECT_GT(fuzzer.evaluated(), 400U);
  EXPECT_LT(fuzzer.slowest(), std::chrono::milliseconds(250));
}

class ExpressionGenerator final {
public:
  explicit ExpressionGenerator(std::uint64_t seed) : random_(seed) {}

  std::string next(std::size_t depth) {
    if (depth == 0 || below(4) == 0) {
      return below(2) == 0 ? pick(kNames) : pick(kLiterals);
    }
    switch (below(6)) {
    case 0:
      return pick(std::vector<std::string>{"-", "!"}) + next(depth - 1);
    case 1:
      return "(" + next(depth - 1) + ")";
    case 2:
      return next(depth - 1) + " ? " + next(depth - 1) + " : " +
             next(depth - 1);
    case 3: {
      const auto &[name, arity] = kCalls[below(kCalls.size())];
      std::string text = name + "(";
      for (std::size_t index = 0; index < arity; ++index) {
        text += (index == 0 ? "" : ", ") + next(depth - 1);
      }
      return text + ")";
    }
    default:
      return next(depth - 1) + " " + pick(kOperators) + " " + next(depth - 1);
    }
  }

private:
  std::size_t below(std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random_);
  }

  std::string pick(const std::vector<std::string> &values) {
    return values[below(values.size())];
  }

  inline static const std::vector<std::string> kNames{
      "param.tube_length", "key.index",     "param.goal.yaw",
      "key.part_pose.qz",  "key.fragile",   "param.name",
      "key.missing",       "param.big",     "param.small",
      "param.huge",        "param.zero",    "param.neg",
      "key.label",         "param.columns", "key.gimbal_pose.roll"};
  inline static const std::vector<std::string> kLiterals{
      "0",    "1",     "7",       "3",
      "2.5",  "0.0",   "1e308",   "9223372036854775807",
      "true", "false", "\"tray\""};
  inline static const std::vector<std::string> kOperators{
      "+", "-", "*", "/", "%", "<", "<=", ">", ">=", "==", "!=", "&&", "||"};
  inline static const std::vector<std::pair<std::string, std::size_t>> kCalls{
      {"abs", 1},  {"min", 2},   {"max", 2},  {"clamp", 3}, {"floor", 1},
      {"ceil", 1}, {"round", 1}, {"sqrt", 1}, {"sin", 1},   {"cos", 1},
      {"tan", 1},  {"atan2", 2}, {"deg", 1},  {"rad", 1}};

  std::mt19937_64 random_;
};

TEST(ExpressionFuzz, GeneratedExpressionsAreBoundedAndStructured) {
  ExpressionGenerator generator(0x67656e6572617465ULL);
  Fuzzer fuzzer;
  for (int iteration = 0; iteration < 20000; ++iteration) {
    fuzzer.run(generator.next(1 + iteration % 7));
    if (HasFatalFailure() || HasNonfatalFailure()) {
      return;
    }
  }
  EXPECT_GT(fuzzer.parsed(), 19000U);
  EXPECT_GT(fuzzer.evaluated(), 4000U);
  EXPECT_LT(fuzzer.slowest(), std::chrono::milliseconds(250));
}

} // namespace
} // namespace manyforge::deployment::expression

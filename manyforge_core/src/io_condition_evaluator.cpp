#include <algorithm>
#include <cmath>
#include <compare>
#include <manyforge_core/io_condition_evaluator.hpp>
#include <optional>
#include <set>
#include <utility>

namespace manyforge::core {
namespace {

template <typename T>
Result<T> reject(const std::string& reason) {
  return Result<T>::Err({"invalid_request", "I/O condition is invalid", {{"reason", reason}}});
}

bool text_valid(const std::string& value, std::size_t bound) {
  return !value.empty() && value.size() <= bound && value.find('\0') == std::string::npos;
}

bool kind_valid(IoConditionValueKind kind) {
  switch (kind) {
    case IoConditionValueKind::kBoolean:
    case IoConditionValueKind::kInt64:
    case IoConditionValueKind::kFloat64:
    case IoConditionValueKind::kString:
      return true;
  }
  return false;
}

IoConditionValueKind kind_of(const IoConditionValueDTO& value) {
  if (std::holds_alternative<bool>(value)) return IoConditionValueKind::kBoolean;
  if (std::holds_alternative<std::int64_t>(value)) return IoConditionValueKind::kInt64;
  if (std::holds_alternative<double>(value)) return IoConditionValueKind::kFloat64;
  return IoConditionValueKind::kString;
}

std::optional<std::string> value_error(const IoConditionValueDTO& value,
                                       const IoConditionLimitsDTO& limits) {
  if (const auto* number = std::get_if<double>(&value); number && !std::isfinite(*number))
    return "io_condition_value_nonfinite";
  if (const auto* text = std::get_if<std::string>(&value);
      text && text->size() > limits.maximum_string_bytes)
    return "io_condition_string_bound_exceeded";
  return std::nullopt;
}

bool numeric(IoConditionValueKind kind) {
  return kind == IoConditionValueKind::kInt64 || kind == IoConditionValueKind::kFloat64;
}

bool comparison_valid(IoConditionComparison comparison) {
  switch (comparison) {
    case IoConditionComparison::kEqual:
    case IoConditionComparison::kNotEqual:
    case IoConditionComparison::kLess:
    case IoConditionComparison::kLessEqual:
    case IoConditionComparison::kGreater:
    case IoConditionComparison::kGreaterEqual:
      return true;
  }
  return false;
}

Result<IoConditionValueKind> operand_kind(const IoConditionOperandDTO& operand,
                                          const IoConditionDefinitionDTO& definition,
                                          const IoConditionLimitsDTO& limits,
                                          std::vector<bool>& used_inputs) {
  if (const auto* input = std::get_if<IoConditionInputReferenceDTO>(&operand)) {
    if (input->input_index >= definition.inputs.size())
      return reject<IoConditionValueKind>("io_condition_input_index_invalid");
    used_inputs[input->input_index] = true;
    return Result<IoConditionValueKind>::Ok(definition.inputs[input->input_index].value_kind);
  }
  const auto& literal = std::get<IoConditionValueDTO>(operand);
  if (const auto reason = value_error(literal, limits))
    return reject<IoConditionValueKind>(*reason);
  return Result<IoConditionValueKind>::Ok(kind_of(literal));
}

/// Same exact finite mixed-number ordering as Program expression.cpp's
/// compare_mixed: range-check before truncating, never round int64 to double.
std::partial_ordering compare_mixed(std::int64_t integer, double real) {
  constexpr double two_to_63 = 0x1p63;
  if (real >= two_to_63) return std::partial_ordering::less;
  if (real < -two_to_63) return std::partial_ordering::greater;
  const auto truncated = std::trunc(real);
  const auto whole = static_cast<std::int64_t>(truncated);
  if (integer != whole) return integer <=> whole;
  return 0.0 <=> (real - truncated);
}

std::partial_ordering compare_values(const IoConditionValueDTO& left,
                                     const IoConditionValueDTO& right) {
  if (const auto* boolean = std::get_if<bool>(&left)) return *boolean <=> std::get<bool>(right);
  if (const auto* text = std::get_if<std::string>(&left))
    return *text <=> std::get<std::string>(right);
  if (const auto* integer = std::get_if<std::int64_t>(&left)) {
    if (const auto* other_integer = std::get_if<std::int64_t>(&right))
      return *integer <=> *other_integer;
    return compare_mixed(*integer, std::get<double>(right));
  }
  if (const auto* integer = std::get_if<std::int64_t>(&right))
    return 0 <=> compare_mixed(*integer, std::get<double>(left));
  return std::get<double>(left) <=> std::get<double>(right);
}

bool comparison_result(IoConditionComparison comparison, std::partial_ordering order) {
  switch (comparison) {
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
  return false;  // Only privately checked plans reach evaluation.
}

const IoConditionValueDTO* borrowed_value(const IoConditionValueDTO& value) {
  return &value;
}
const IoConditionValueDTO* borrowed_value(const IoConditionValueDTO* value) {
  return value;
}

template <typename Values>
const IoConditionValueDTO& operand_value(const IoConditionOperandDTO& operand, Values values) {
  if (const auto* input = std::get_if<IoConditionInputReferenceDTO>(&operand))
    return *borrowed_value(values[input->input_index]);
  return std::get<IoConditionValueDTO>(operand);
}

std::span<const std::size_t> children_of(const IoConditionNodeDTO& node) {
  if (const auto* all = std::get_if<IoConditionAllDTO>(&node)) return all->children;
  if (const auto* any = std::get_if<IoConditionAnyDTO>(&node)) return any->children;
  if (const auto* negate = std::get_if<IoConditionNotDTO>(&node)) return {&negate->child, 1U};
  return {};
}

}  // namespace

Result<CheckedIoCondition> CheckIoCondition(IoConditionDefinitionDTO definition,
                                            IoConditionLimitsDTO limits) {
  if (limits.maximum_inputs == 0U || limits.maximum_nodes == 0U || limits.maximum_edges == 0U ||
      limits.maximum_depth == 0U || limits.maximum_identity_bytes == 0U ||
      limits.maximum_string_bytes == 0U || definition.inputs.size() > limits.maximum_inputs ||
      definition.nodes.empty() || definition.nodes.size() > limits.maximum_nodes)
    return reject<CheckedIoCondition>("io_condition_bounds_invalid");
  std::set<IoConditionInputSourceDTO> sources;
  for (const auto& input : definition.inputs) {
    if (!kind_valid(input.value_kind))
      return reject<CheckedIoCondition>("io_condition_input_declaration_invalid");
    if (const auto* signal = std::get_if<IoSignalId>(&input.source)) {
      if (!text_valid(signal->controllable_component_id.value(), limits.maximum_identity_bytes) ||
          !text_valid(signal->name, limits.maximum_identity_bytes) ||
          input.value_kind != IoConditionValueKind::kBoolean)
        return reject<CheckedIoCondition>("io_condition_signal_input_invalid");
    } else if (!text_valid(std::get<IoConditionBlackboardKeyDTO>(input.source).name,
                           limits.maximum_identity_bytes)) {
      return reject<CheckedIoCondition>("io_condition_key_input_invalid");
    }
    if (!sources.insert(input.source).second)
      return reject<CheckedIoCondition>("io_condition_input_declaration_invalid");
  }
  std::vector<std::size_t> depths(definition.nodes.size(), 1U);
  std::vector<bool> used_inputs(definition.inputs.size(), false);
  std::size_t edges = 0U;
  for (std::size_t index = 0U; index < definition.nodes.size(); ++index) {
    const auto& node = definition.nodes[index];
    if (const auto* compare = std::get_if<IoConditionCompareDTO>(&node)) {
      if (!comparison_valid(compare->comparison))
        return reject<CheckedIoCondition>("io_condition_comparison_invalid");
      const auto left = operand_kind(compare->left, definition, limits, used_inputs);
      if (!left.ok()) return Result<CheckedIoCondition>::Err(left.error());
      const auto right = operand_kind(compare->right, definition, limits, used_inputs);
      if (!right.ok()) return Result<CheckedIoCondition>::Err(right.error());
      const bool numbers = numeric(left.value()) && numeric(right.value());
      const bool equality = compare->comparison == IoConditionComparison::kEqual ||
                            compare->comparison == IoConditionComparison::kNotEqual;
      if (!numbers && (!equality || left.value() != right.value()))
        return reject<CheckedIoCondition>("io_condition_operand_types_incompatible");
      continue;
    }
    const auto children = children_of(node);
    if ((!std::holds_alternative<IoConditionNotDTO>(node) && children.size() < 2U) ||
        children.size() > limits.maximum_edges - edges)
      return reject<CheckedIoCondition>("io_condition_children_invalid");
    edges += children.size();
    for (const auto child : children) {
      if (child >= index) return reject<CheckedIoCondition>("io_condition_postorder_invalid");
      if (depths[child] >= limits.maximum_depth)
        return reject<CheckedIoCondition>("io_condition_depth_exceeded");
      depths[index] = std::max(depths[index], depths[child] + 1U);
    }
  }
  std::vector<bool> reachable(definition.nodes.size(), false);
  reachable.back() = true;
  for (std::size_t index = definition.nodes.size(); index-- > 0U;) {
    if (!reachable[index]) return reject<CheckedIoCondition>("io_condition_unreachable_node");
    for (const auto child : children_of(definition.nodes[index])) reachable[child] = true;
  }
  if (std::find(used_inputs.begin(), used_inputs.end(), false) != used_inputs.end())
    return reject<CheckedIoCondition>("io_condition_unused_input");
  return Result<CheckedIoCondition>::Ok(CheckedIoCondition(std::move(definition), limits));
}

CheckedIoCondition::CheckedIoCondition(IoConditionDefinitionDTO definition,
                                       IoConditionLimitsDTO limits)
    : definition_(std::move(definition)), limits_(limits) {}

std::span<const IoConditionInputDTO> CheckedIoCondition::inputs() const noexcept {
  return definition_.inputs;
}

std::size_t CheckedIoCondition::workspace_size() const noexcept {
  return definition_.nodes.size();
}

namespace {
template <typename Values>
Result<bool> evaluate(const IoConditionDefinitionDTO& definition,
                      const IoConditionLimitsDTO& limits, Values known_values,
                      std::span<std::uint8_t> workspace) {
  if (known_values.size() != definition.inputs.size())
    return reject<bool>("io_condition_input_count_mismatch");
  if (workspace.size() < definition.nodes.size())
    return reject<bool>("io_condition_workspace_too_small");
  for (std::size_t index = 0U; index < known_values.size(); ++index) {
    const auto* value = borrowed_value(known_values[index]);
    if (!value) return reject<bool>("io_condition_input_unknown");
    if (kind_of(*value) != definition.inputs[index].value_kind)
      return reject<bool>("io_condition_input_type_mismatch");
    if (const auto reason = value_error(*value, limits)) return reject<bool>(*reason);
  }
  for (std::size_t index = 0U; index < definition.nodes.size(); ++index) {
    const auto& node = definition.nodes[index];
    if (const auto* compare = std::get_if<IoConditionCompareDTO>(&node)) {
      workspace[index] = comparison_result(
          compare->comparison, compare_values(operand_value(compare->left, known_values),
                                              operand_value(compare->right, known_values)));
    } else if (const auto* all = std::get_if<IoConditionAllDTO>(&node)) {
      workspace[index] = std::all_of(all->children.begin(), all->children.end(),
                                     [&](auto child) { return workspace[child] != 0U; });
    } else if (const auto* any = std::get_if<IoConditionAnyDTO>(&node)) {
      workspace[index] = std::any_of(any->children.begin(), any->children.end(),
                                     [&](auto child) { return workspace[child] != 0U; });
    } else {
      workspace[index] = workspace[std::get<IoConditionNotDTO>(node).child] == 0U;
    }
  }
  return Result<bool>::Ok(workspace[definition.nodes.size() - 1U] != 0U);
}

}  // namespace

Result<bool> EvaluateIoCondition(const CheckedIoCondition& condition,
                                 std::span<const IoConditionValueDTO> known_values,
                                 std::span<std::uint8_t> workspace) {
  return evaluate(condition.definition_, condition.limits_, known_values, workspace);
}
Result<bool> EvaluateIoCondition(const CheckedIoCondition& condition,
                                 std::span<const IoConditionValueDTO* const> known_values,
                                 std::span<std::uint8_t> workspace) {
  return evaluate(condition.definition_, condition.limits_, known_values, workspace);
}

}  // namespace manyforge::core

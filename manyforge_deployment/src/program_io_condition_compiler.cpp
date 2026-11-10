#include <manyforge_deployment/program_io_condition_compiler.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "program_io_condition_compiler_internal.hpp"

namespace manyforge::deployment {
namespace {

// Authored tree bounds agreed independently of signal-cache/history limits.
constexpr std::size_t kMaximumConditionDepth = 8U;
constexpr std::size_t kMaximumConditionNodes = 64U;
// A compound expression needs one parent node; every distinct input needs a
// comparison leaf. A lone comparison needs one input and is also covered.
// This derives the generic Core safety bound from the authored grammar, rather
// than adding an independent product input cap.
constexpr std::size_t kMaximumConditionInputs = kMaximumConditionNodes - 1U;
constexpr double kMaximumSafeInteger = 9007199254740991.0;

class ConditionError final : public std::invalid_argument {
public:
  ConditionError(std::string reason, std::string message, std::string key = {})
      : std::invalid_argument(std::move(message)), reason(std::move(reason)),
        key(std::move(key)) {}
  std::string reason;
  std::string key;
};

[[noreturn]] void fail(std::string reason, std::string message,
                       std::string key = {}) {
  throw ConditionError(std::move(reason), std::move(message), std::move(key));
}

const CanonicalValue &member(const CanonicalValue &value,
                             std::string_view name) {
  const auto *found = value.find_object_member(name);
  if (found == nullptr)
    fail("interrupt_condition_invalid", "Missing field " + std::string(name));
  return *found;
}

const std::string &text(const CanonicalValue &value) {
  if (!value.is_string())
    fail("interrupt_condition_invalid", "Expected literal string");
  return value.as_string();
}

void fields(const CanonicalValue &value,
            std::initializer_list<std::string_view> expected) {
  if (!value.is_object() || value.as_object().size() != expected.size())
    fail("interrupt_condition_invalid", "Condition record has wrong fields");
  for (const auto name : expected)
    (void)member(value, name);
}

core::IoConditionValueKind scalar_kind(std::string_view type) {
  if (type == "bool")
    return core::IoConditionValueKind::kBoolean;
  if (type == "int64")
    return core::IoConditionValueKind::kInt64;
  if (type == "float64")
    return core::IoConditionValueKind::kFloat64;
  if (type == "string")
    return core::IoConditionValueKind::kString;
  fail("interrupt_condition_key_refused", "Condition requires a scalar key");
}

core::IoConditionComparison comparison(const CanonicalValue &value) {
  const auto &op = text(value);
  if (op == "==")
    return core::IoConditionComparison::kEqual;
  if (op == "!=")
    return core::IoConditionComparison::kNotEqual;
  if (op == "<")
    return core::IoConditionComparison::kLess;
  if (op == "<=")
    return core::IoConditionComparison::kLessEqual;
  if (op == ">")
    return core::IoConditionComparison::kGreater;
  if (op == ">=")
    return core::IoConditionComparison::kGreaterEqual;
  fail("interrupt_condition_invalid", "Unknown comparison operator");
}

core::IoConditionValueDTO literal(const CanonicalValue &value) {
  if (value.is_bool())
    return value.as_bool();
  if (value.is_string())
    return value.as_string();
  if (value.is_number()) {
    const double number = value.as_number();
    if (std::trunc(number) == number) {
      // The canonical Program number carrier is double. Never reinterpret a
      // rounded out-of-range integer as an exact authored int64 literal.
      if (std::fabs(number) > kMaximumSafeInteger)
        fail("interrupt_condition_invalid",
             "Integer literal exceeds safe range");
      return static_cast<std::int64_t>(number);
    }
    return number;
  }
  fail("interrupt_condition_invalid",
       "Comparison value must be a scalar literal");
}

core::IoConditionLimitsDTO condition_limits() {
  return {kMaximumConditionInputs,
          kMaximumConditionNodes,
          kMaximumConditionNodes - 1U,
          kMaximumConditionDepth,
          256U,
          content_identity::kMaximumCanonicalJsonBytes};
}

struct ResolvedConditionInput final {
  core::IoConditionInputSourceDTO source;
  std::optional<core::IoConditionValueKind> value_kind;
};

class Lowerer final {
public:
  Lowerer(std::span<const core::ProgramSignalBindingDTO> bindings,
          std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys,
          std::span<const RetainedIoSignalConfiguration> signals,
          bool deployment_operand = false,
          bool allow_waiting_program_keys = false)
      : bindings_(bindings), keys_(keys), signals_(signals),
        deployment_operand_(deployment_operand),
        allow_waiting_program_keys_(deployment_operand &&
                                    allow_waiting_program_keys) {}

  std::vector<detail::ProgramIoSignalReference>
  collect_signal_references(const CanonicalValue &value) {
    collecting_signal_references_ = true;
    (void)lower(value, 1U);
    return {signal_references_.begin(), signal_references_.end()};
  }

  detail::CompiledProgramIoCondition compile(const CanonicalValue &value) {
    (void)lower(value, 1U);
    if (!waiting_keys_.empty()) {
      for (const auto &node : definition_.nodes)
        if (const auto *compare =
                std::get_if<core::IoConditionCompareDTO>(&node))
          validate_known_compare(
              std::get<core::IoConditionInputReferenceDTO>(compare->left)
                  .input_index,
              compare->comparison,
              std::get<core::IoConditionValueDTO>(compare->right));
      return {nullptr,
              {required_sources_.begin(), required_sources_.end()},
              {waiting_keys_.begin(), waiting_keys_.end()}};
    }
    auto checked =
        core::CheckIoCondition(std::move(definition_), condition_limits());
    if (!checked.ok())
      fail("io_signal_type_mismatch", checked.error().message);
    return {std::make_shared<const core::CheckedIoCondition>(
                std::move(checked.value())),
            {required_sources_.begin(), required_sources_.end()}};
  }

private:
  // `<output name>.changed_outside` is the
  // core-held in-process bool of an output with read-back that isn't
  // controller-owned; the core derives its signal and source.
  ResolvedConditionInput changed_outside(const std::string &component,
                                         std::string_view name) {
    const auto *output = FindChangedOutsideOutput(
        signals_, core::ControllableComponentId(component), name);
    if (output == nullptr)
      fail("io_signal_unbound",
           "Deployment condition names no retained output for changed_outside");
    if (!core::HoldsChangedOutside(output->descriptor))
      fail("io_output_changed_outside_unavailable",
           output->descriptor.controller_owned
               ? "A controller-owned output has no changed_outside signal"
               : "An output without read-back has no changed_outside signal");
    required_sources_.insert(
        core::ChangedOutsideSourceId(output->descriptor.signal_id));
    return ResolvedConditionInput{
        core::ChangedOutsideSignalId(output->descriptor.signal_id),
        core::IoConditionValueKind::kBoolean};
  }

  std::optional<ResolvedConditionInput> resolve(const CanonicalValue &operand) {
    if (!operand.is_object() || operand.as_object().size() != 1U)
      fail("interrupt_condition_invalid", "Expected one operand selector");
    if (const auto *key = operand.find_object_member("key")) {
      const auto &name = text(*key);
      if (collecting_signal_references_)
        return std::nullopt;
      if (deployment_operand_ &&
          (name == "interrupt_state" || name.starts_with("interrupt_state.")))
        fail("global_rule_state_operand_unsupported",
             "Global rules cannot read interrupt_state operands", name);
      const auto found =
          std::find_if(keys_.begin(), keys_.end(),
                       [&](const auto &row) { return row.name == name; });
      if (found == keys_.end()) {
        if (allow_waiting_program_keys_ && name != "node_outcome" &&
            !name.starts_with("node_outcome.")) {
          if (name.empty() ||
              name.size() > condition_limits().maximum_identity_bytes ||
              name.find('\0') != std::string::npos)
            fail("interrupt_condition_invalid",
                 "Deferred key identity is invalid");
          waiting_keys_.insert(name);
          return ResolvedConditionInput{core::IoConditionBlackboardKeyDTO{name},
                                        std::nullopt};
        }
        fail("interrupt_condition_key_refused",
             "Key is not declared by Program", name);
      }
      // Qualified Program declarations supply these system-owned keys only
      // for actual motion instances / retained rule IDs. Their initial values
      // belong to Runner/rule state, never to an authored declaration (105).
      const auto in_system_namespace = [&](std::string_view prefix) {
        return name.starts_with(prefix) && name.size() > prefix.size();
      };
      const bool system_initial = in_system_namespace("node_outcome.") ||
                                  in_system_namespace("interrupt_state.");
      if (!found->has_declared_initial && !system_initial)
        fail("interrupt_key_initial_required",
             "Interrupt condition key requires a declared initial value");
      return ResolvedConditionInput{core::IoConditionBlackboardKeyDTO{name},
                                    scalar_kind(found->declared_type)};
    }
    const auto &signal = member(operand, "signal");
    if (deployment_operand_) {
      fields(signal, {"controllable_component_id", "signal_name"});
      const auto &component = text(member(signal, "controllable_component_id"));
      const auto &name = text(member(signal, "signal_name"));
      if (name.ends_with(core::kChangedOutsideSignalSuffix))
        return changed_outside(component, name);
      const auto retained =
          std::find_if(signals_.begin(), signals_.end(), [&](const auto &row) {
            return row.descriptor.signal_id.controllable_component_id.value() == component &&
                   row.descriptor.signal_id.name == name;
          });
      if (retained == signals_.end())
        fail("io_signal_unbound",
             "Deployment condition names no retained signal");
      if (retained->descriptor.direction != core::IoSignalDirection::kInput)
        fail("io_signal_direction_mismatch",
             "Condition requires a declared input");
      if (retained->descriptor.value_type != core::IoSignalValueType::kBoolean)
        fail("io_signal_type_mismatch", "Unsupported signal value type");
      if (!retained->descriptor.liveness ||
          retained->descriptor.liveness ==
              core::IoSignalLivenessMode::kChangeOnly)
        fail("io_signal_liveness_unproven",
             "Interrupt conditions require qualified source liveness");
      required_sources_.insert(retained->descriptor.source_id);
      return ResolvedConditionInput{retained->descriptor.signal_id,
                                    core::IoConditionValueKind::kBoolean};
    }
    fields(signal, {"component_id", "signal_name"});
    const core::ProgramSignalReferenceDTO reference{
        core::ControllableComponentId(text(member(signal, "component_id"))),
        text(member(signal, "signal_name"))};
    if (collecting_signal_references_) {
      signal_references_.insert(reference);
      return std::nullopt;
    }
    const auto binding = std::find_if(bindings_.begin(), bindings_.end(),
        [&](const auto &row) { return row.reference == reference; });
    if (binding == bindings_.end() ||
        std::count_if(bindings_.begin(), bindings_.end(),
            [&](const auto &row) { return row.reference == reference; }) != 1)
      fail("io_signal_unbound", "Condition signal has no exact unique binding");
    const auto matches = FindProgramSignalMatches(signals_, reference);
    if (matches.size() != 1U)
      fail("io_signal_unbound", "Condition signal has no exact unique retained descriptor");
    const auto &match = matches.front();
    const auto &descriptor = match.descriptor;
    const auto digest = core::ComputeIoSignalDescriptorDigest(descriptor);
    if (!digest.ok() || digest.value() != binding->signal_descriptor_digest ||
        binding->target.component_id != reference.component_id ||
        binding->target.command_target_id != match.retained->command_target_id ||
        binding->runtime_unit_id != match.retained->runtime_unit_id ||
        descriptor.value_type != binding->value_type ||
        descriptor.direction != binding->direction ||
        descriptor.transition_capability != binding->transition_capability)
      fail("io_signal_unbound", "Condition signal binding differs from retained descriptor");
    if (descriptor.direction != core::IoSignalDirection::kInput)
      fail("io_signal_direction_mismatch",
           "Condition requires a declared input");
    if (descriptor.value_type != core::IoSignalValueType::kBoolean)
      fail("io_signal_type_mismatch", "Unsupported signal value type");
    if (!descriptor.liveness ||
        descriptor.liveness == core::IoSignalLivenessMode::kChangeOnly)
      fail("io_signal_liveness_unproven",
           "Interrupt conditions require qualified source liveness");
    required_sources_.insert(descriptor.source_id);
    return ResolvedConditionInput{descriptor.signal_id,
                                  core::IoConditionValueKind::kBoolean};
  }

  std::optional<std::size_t> input(const CanonicalValue &operand) {
    auto resolved_input = resolve(operand);
    if (!resolved_input)
      return std::nullopt;
    auto &resolved = *resolved_input;
    const auto found = input_indices_.find(resolved.source);
    if (found != input_indices_.end())
      return found->second;
    if (input_indices_.size() == kMaximumConditionInputs)
      fail("interrupt_condition_invalid", "Condition exceeds operand bound");
    if (!resolved.value_kind) {
      input_indices_.emplace(std::move(resolved.source), std::nullopt);
      return std::nullopt;
    }
    const auto index = definition_.inputs.size();
    input_indices_.emplace(resolved.source, index);
    definition_.inputs.push_back(
        {std::move(resolved.source), *resolved.value_kind});
    return index;
  }

  void validate_known_compare(std::size_t input_index,
                              core::IoConditionComparison op,
                              const core::IoConditionValueDTO &value) {
    // A complete, actually typed operand is checked by the SAME Core validator
    // even if another branch is waiting. No partial condition is exposed.
    auto checked = core::CheckIoCondition(
        {{definition_.inputs[input_index]},
         {core::IoConditionCompareDTO{
             op, core::IoConditionInputReferenceDTO{0U}, value}}},
        condition_limits());
    if (!checked.ok())
      fail("io_signal_type_mismatch", checked.error().message);
  }

  std::optional<std::size_t> lower(const CanonicalValue &value,
                                   std::size_t depth) {
    if (depth > kMaximumConditionDepth ||
        ++authored_nodes_ > kMaximumConditionNodes)
      fail("interrupt_condition_invalid",
           "Condition exceeds authored tree bounds");
    if (!value.is_object() || value.as_object().size() != 1U)
      fail("interrupt_condition_invalid",
           "Expected exactly one condition operator");
    if (const auto *row = value.find_object_member("compare")) {
      fields(*row, {"operand", "op", "value"});
      const auto op = comparison(member(*row, "op"));
      const auto selected = input(member(*row, "operand"));
      const auto compared = literal(member(*row, "value"));
      if (!selected) {
        if (const auto *number = std::get_if<double>(&compared);
            number && !std::isfinite(*number))
          fail("io_signal_type_mismatch", "I/O condition is invalid");
        if (const auto *text = std::get_if<std::string>(&compared);
            text && text->size() > condition_limits().maximum_string_bytes)
          fail("io_signal_type_mismatch", "I/O condition is invalid");
        return std::nullopt;
      }
      definition_.nodes.emplace_back(core::IoConditionCompareDTO{
          op, core::IoConditionInputReferenceDTO{*selected}, compared});
    } else if (const auto *child = value.find_object_member("not")) {
      const auto index = lower(*child, depth + 1U);
      if (!index)
        return std::nullopt;
      definition_.nodes.emplace_back(core::IoConditionNotDTO{*index});
    } else {
      const bool all = value.find_object_member("all") != nullptr;
      const auto &children = member(value, all ? "all" : "any");
      if (!children.is_array() || children.as_array().empty() ||
          children.as_array().size() > kMaximumConditionNodes - authored_nodes_)
        fail("interrupt_condition_invalid", "Invalid condition child count");
      std::vector<std::size_t> indices;
      bool waiting = false;
      for (const auto &child : children.as_array()) {
        const auto lowered = lower(child, depth + 1U);
        if (lowered)
          indices.push_back(*lowered);
        else
          waiting = true;
      }
      if (waiting)
        return std::nullopt;
      if (indices.size() == 1U)
        return indices.front();
      if (all)
        definition_.nodes.emplace_back(
            core::IoConditionAllDTO{std::move(indices)});
      else
        definition_.nodes.emplace_back(
            core::IoConditionAnyDTO{std::move(indices)});
    }
    return definition_.nodes.size() - 1U;
  }

  std::span<const core::ProgramSignalBindingDTO> bindings_;
  std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys_;
  std::span<const RetainedIoSignalConfiguration> signals_;
  core::IoConditionDefinitionDTO definition_;
  std::map<core::IoConditionInputSourceDTO, std::optional<std::size_t>>
      input_indices_;
  std::set<core::InterruptSourceId> required_sources_;
  std::size_t authored_nodes_{};
  bool deployment_operand_;
  bool allow_waiting_program_keys_;
  std::set<std::string> waiting_keys_;
  bool collecting_signal_references_{};
  std::set<detail::ProgramIoSignalReference> signal_references_;
};

bool is_program_interrupt_node(std::string_view id) {
  return id == "motion.move_until_signal" || id == "motion.probe_search";
}

const CanonicalValue &literal_node_condition(const CanonicalValue &node) {
  const auto &parameters = member(node, "parameters");
  if (parameters.find_object_member("interrupt_policy_id"))
    fail("interrupt_node_policy_reference_refused",
         "Node rules do not reference Deployment policies");
  if (parameters.find_object_member("fallback_outcome"))
    fail("interrupt_node_fallback_outcome_refused",
         "Node rules use success_through_stage_id");
  if (const auto *sources = node.find_object_member("value_sources")) {
    for (const auto &[path, unused] : sources->as_object()) {
      (void)unused;
      for (const std::string_view literal :
           {"interrupt_condition", "target", "activation_mode",
            "stop_mode", "priority", "stop_entry_stage_id",
            "success_through_stage_id", "fallback_outcome"})
        if (path == literal || path.starts_with(std::string(literal) + ".") ||
            path.starts_with(std::string(literal) + "["))
          fail("interrupt_condition_invalid",
               "Node rule fields must stay literal");
    }
  }
  return member(parameters, "interrupt_condition");
}

core::ErrorDTO error(const ConditionError &failure) {
  core::ErrorDTO result{
      "invalid_request", failure.what(), {{"reason", failure.reason}}};
  if (!failure.key.empty())
    result.details.emplace("key", failure.key);
  return result;
}

} // namespace

core::Result<void> detail::check_program_signal_comparison(
    const core::IoSignalDescriptorDTO &descriptor, const CanonicalValue &op,
    const CanonicalValue &value) {
  try {
    if (descriptor.value_type != core::IoSignalValueType::kBoolean)
      fail("io_signal_type_mismatch", "Unsupported signal value type");
    auto checked = core::CheckIoCondition(
        {{{descriptor.signal_id, core::IoConditionValueKind::kBoolean}},
         {core::IoConditionCompareDTO{comparison(op),
              core::IoConditionInputReferenceDTO{0U}, literal(value)}}},
        condition_limits());
    if (!checked.ok())
      fail("io_signal_type_mismatch", checked.error().message);
    return core::Result<void>::Ok();
  } catch (const ConditionError &failure) {
    return core::Result<void>::Err(error(failure));
  }
}

core::Result<detail::CompiledProgramIoCondition>
detail::compile_authored_program_io_condition(
    const CanonicalValue &condition,
    std::span<const core::ProgramSignalBindingDTO> bindings,
    std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys,
    std::span<const RetainedIoSignalConfiguration> signals) {
  try {
    return core::Result<CompiledProgramIoCondition>::Ok(
        Lowerer(bindings, keys, signals).compile(condition));
  } catch (const ConditionError &failure) {
    return core::Result<CompiledProgramIoCondition>::Err(error(failure));
  }
}

core::Result<detail::CompiledProgramIoCondition>
detail::compile_authored_deployment_io_condition(
    const CanonicalValue &condition,
    std::span<const core::ProgramBlackboardKeyDeclarationDTO> keys,
    std::span<const RetainedIoSignalConfiguration> signals,
    bool allow_waiting_program_keys) {
  try {
    return core::Result<CompiledProgramIoCondition>::Ok(
        Lowerer({}, keys, signals, true, allow_waiting_program_keys)
            .compile(condition));
  } catch (const ConditionError &failure) {
    return core::Result<CompiledProgramIoCondition>::Err(error(failure));
  }
}

core::Result<std::vector<detail::ProgramIoSignalReference>>
detail::collect_program_node_io_signal_references(const CanonicalValue &node) {
  using Reply = core::Result<std::vector<ProgramIoSignalReference>>;
  try {
    if (!is_program_interrupt_node(text(member(node, "node_id"))))
      return Reply::Ok({});
    return Reply::Ok(
        Lowerer({}, {}, {}).collect_signal_references(literal_node_condition(node)));
  } catch (const ConditionError &failure) {
    return Reply::Err(error(failure));
  } catch (const std::exception &failure) {
    return Reply::Err(
        error(ConditionError("interrupt_condition_invalid", failure.what())));
  }
}

core::Result<core::ProgramIoConditionProjectionDTO>
compile_program_io_conditions(const ProgramBindingContract &program,
                              const ProgramBinding &binding,
                              const RetainedBundle &retained_bundle) {
  try {
    const auto &bound = binding.value();
    if (bound.program_id != program.value().program_id ||
        bound.program_revision != program.value().program_revision ||
        bound.node_environment != program.value().node_environment ||
        bound.retained_bundle_id != retained_bundle.retained_bundle_id() ||
        bound.graph_revision !=
            retained_bundle.value().cell_graph.graph_revision)
      fail("program_capture_product_mismatch",
           "Condition compiler requires exact sealed inputs");
    const auto signals =
        ResolveCompiledIoSignalCatalog(retained_bundle.value().cell_graph);
    if (!signals.ok())
      return core::Result<core::ProgramIoConditionProjectionDTO>::Err(
          signals.error());
    core::ProgramIoConditionProjectionDTO projection;
    std::map<std::string, core::IoConditionValueKind> referenced;
    const auto visit = [&](auto &&self, const CanonicalValue &node) -> void {
      const auto &node_id = text(member(node, "node_id"));
      if (is_program_interrupt_node(node_id)) {
        const auto &parameters = member(node, "parameters");
        const auto &condition = literal_node_condition(node);
        auto compiled = detail::compile_authored_program_io_condition(
            condition, bound.signal_bindings,
            program.blackboard_key_declarations(), signals.value());
        if (!compiled.ok())
          throw ConditionError(compiled.error().details.at("reason"),
                               compiled.error().message);
        for (const auto &input : compiled.value().condition->inputs())
          if (const auto *key =
                  std::get_if<core::IoConditionBlackboardKeyDTO>(&input.source))
            referenced.emplace(key->name, input.value_kind);
        const auto id = text(member(node, "instance_id"));
        const auto digest = content_identity::framed_sha256(
            "program-node-io-rule", "manyforge.program_node_io_rule.v1",
            CanonicalValue(CanonicalValue::Object{
                {"program_id",
                 CanonicalValue(program.value().program_id.value())},
                {"program_revision",
                 CanonicalValue(program.value().program_revision.value())},
                {"node_instance_id", CanonicalValue(id)}}));
        auto activation =
            core::MotionInterruptActivationMode::kRequireInactiveAtArm;
        if (const auto *value =
                parameters.find_object_member("activation_mode")) {
          const auto &mode = text(*value);
          if (mode == "fresh_edge_after_arm")
            activation =
                core::MotionInterruptActivationMode::kFreshEdgeAfterArm;
          else if (mode == "trip_if_already_active")
            activation =
                core::MotionInterruptActivationMode::kTripIfAlreadyActive;
          else if (mode != "require_inactive_at_arm")
            fail("interrupt_condition_invalid",
                 "Unsupported node activation mode");
        }
        core::MotionInterruptStopMode stop;
        const auto &mode = text(member(parameters, "stop_mode"));
        if (mode == "soft_on_path")
          stop = core::MotionInterruptStopMode::kSoftOnPath;
        else if (mode == "spring_back_hold")
          stop = core::MotionInterruptStopMode::kSpringBackHold;
        else if (mode == "spring_back_fast")
          stop = core::MotionInterruptStopMode::kSpringBackFast;
        else if (mode == "hard_deceleration")
          stop = core::MotionInterruptStopMode::kHardDeceleration;
        else
          fail("interrupt_condition_invalid", "Unsupported node Stop mode");
        int priority = 0;
        if (const auto *value = parameters.find_object_member("priority")) {
          const auto number = value->as_number();
          if (!std::isfinite(number) || number < 0 || number > 100 ||
              std::trunc(number) != number)
            fail("interrupt_condition_invalid",
                 "Node priority is outside 0..100");
          priority = static_cast<int>(number);
        }
        std::optional<core::StopEscalationStageId> entry;
        if (const auto *value =
                parameters.find_object_member("stop_entry_stage_id"))
          entry.emplace(text(*value));
        std::optional<core::StopEscalationStageId> success_through;
        if (const auto *value =
                parameters.find_object_member("success_through_stage_id"))
          success_through.emplace(text(*value));
        projection.program_node_conditions.push_back(
            {core::ProgramNodeInstanceId(id),
             core::InterruptPolicyId("node." + digest.substr(7U)),
             std::move(compiled.value().condition),
             std::move(compiled.value().required_signal_sources),
             core::ProgramNodeIoRuleSemanticsDTO{
                 core::ProgramTargetReferenceDTO{
                     core::ControllableComponentId(text(member(member(parameters, "target"), "component_id"))),
                     core::CommandTargetId(text(member(member(parameters, "target"), "command_target_id")))},
                 activation, stop,
                 node_id == "motion.probe_search"
                     ? core::MotionInterruptNodeCategory::kPrecisionContact
                     : core::MotionInterruptNodeCategory::kGeneric,
                 priority, entry, success_through}});
      }
      for (const auto &child : member(node, "children").as_array())
        self(self, child);
    };
    visit(visit, member(program.normalized_program_root(), "tree"));
    std::sort(projection.program_node_conditions.begin(),
              projection.program_node_conditions.end(),
              [](const auto &left, const auto &right) {
                return left.node_instance_id < right.node_instance_id;
              });
    for (const auto &[name, kind] : referenced)
      projection.referenced_program_blackboard_keys.push_back({name, kind});
    return core::Result<core::ProgramIoConditionProjectionDTO>::Ok(
        std::move(projection));
  } catch (const ConditionError &failure) {
    return core::Result<core::ProgramIoConditionProjectionDTO>::Err(
        error(failure));
  }
}

} // namespace manyforge::deployment

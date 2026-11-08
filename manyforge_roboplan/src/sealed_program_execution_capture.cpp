#include "sealed_program_execution_capture.hpp"

#include <manyforge_deployment/program_io_condition_compiler.hpp>
#include <algorithm>
#include <optional>
#include <tuple>
#include <vector>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::roboplan_backend::detail {
namespace {
core::ErrorDTO
CaptureError(std::string message, std::string reason,
             core::ErrorCode code = core::ErrorCode::kExecutionRejected) {
  return {std::string(core::to_string(code)),
          std::move(message),
          {{"reason", std::move(reason)}}};
}

[[nodiscard]] std::shared_ptr<const core::ProgramExecutionTableDTO>
PneumaticExecutionTable(const std::shared_ptr<const deployment::ProgramBindingContract> &program,
                        const deployment::ProgramBinding &binding) {
  std::optional<core::ProgramExecutionTableDTO> table;
  const auto *tree = program->normalized_program_root().find_object_member("tree");
  if (!tree) throw std::invalid_argument("Program has no verified tree");
  std::vector<const deployment::CanonicalValue *> pending{tree};
  while (!pending.empty()) {
    const auto *node = pending.back();
    pending.pop_back();
    if (const auto *children = node->find_object_member("children"))
      for (const auto &child : children->as_array()) pending.push_back(&child);
    const auto *node_id = node->find_object_member("node_id");
    if (!node_id || !node_id->is_string() || node_id->as_string() != "gripper.command_position")
      continue;
    const auto *parameters = node->find_object_member("parameters");
    const auto *reference = parameters ? parameters->find_object_member("target") : nullptr;
    const auto *component = reference ? reference->find_object_member("component_id") : nullptr;
    const auto *command_target = reference ? reference->find_object_member("command_target_id") : nullptr;
    const auto *instance = node->find_object_member("instance_id");
    if (!component || !component->is_string() || !command_target ||
        !command_target->is_string() || !instance || !instance->is_string())
      throw std::invalid_argument("gripper node has no literal captured target or instance");
    const auto target = std::find_if(binding.value().target_bindings.begin(),
        binding.value().target_bindings.end(),
        [&](const auto &row) { return row.target.component_id.value() == component->as_string() &&
                                      row.target.command_target_id.value() == command_target->as_string(); });
    if (target == binding.value().target_bindings.end())
      throw std::invalid_argument("gripper node has no bound target");
    const auto *required = parameters->find_object_member("required_inputs");
    if (const auto *sources = node->find_object_member("value_sources");
        sources && sources->find_object_member("required_inputs"))
      throw std::invalid_argument("pneumatic required inputs must be literal at capture");
    if (required && !required->is_array())
      throw std::invalid_argument("pneumatic required inputs are not an array");
    if (required && !required->as_array().empty() && !target->pneumatic_io)
      throw std::invalid_argument("gripper required input is not declared by its deployment");
    if (!target->pneumatic_io) continue;
    if (!table) table = program->execution_table();
    const auto table_node = std::find_if(table->nodes.begin(), table->nodes.end(),
        [&](const auto &row) { return row.instance_id.value() == instance->as_string(); });
    if (table_node == table->nodes.end())
      throw std::invalid_argument("gripper node is absent from its execution table");
    auto &effects = table_node->target_effects;
    const auto signal_target = [&](const core::ProgramSignalReferenceDTO &reference) {
      const auto &signals = binding.value().signal_bindings;
      const auto signal = std::find_if(signals.begin(), signals.end(),
          [&](const auto &row) { return row.reference == reference; });
      if (signal == signals.end())
        throw std::invalid_argument("pneumatic signal has no bound target");
      return signal->target;
    };
    for (const auto &output : target->pneumatic_io->outputs)
      effects.push_back({signal_target(output.signal), core::CapabilityId("io.gpio.set_output"), true});
    if (required) {
      for (const auto &item : required->as_array()) {
        const auto *name = item.find_object_member("input");
        const auto *value = item.find_object_member("value");
        if (!name || !name->is_string() || !value || !value->is_bool())
          throw std::invalid_argument("gripper required input is invalid");
        const auto input = std::find_if(target->pneumatic_io->inputs.begin(), target->pneumatic_io->inputs.end(),
            [&](const auto &row) { return row.input == name->as_string(); });
        if (input == target->pneumatic_io->inputs.end())
          throw std::invalid_argument("gripper required input is not declared by its deployment");
        effects.push_back({signal_target(input->signal), core::CapabilityId("io.gpio.wait_for_input"), false});
        effects.push_back({signal_target(input->signal), core::CapabilityId("io.gpio.get_input"), false});
      }
    }
    std::sort(effects.begin(), effects.end(), [](const auto &left, const auto &right) {
      return std::tie(left.target, left.capability_id) <
             std::tie(right.target, right.capability_id);
    });
    effects.erase(std::unique(effects.begin(), effects.end()), effects.end());
  }
  if (!table)
    return std::shared_ptr<const core::ProgramExecutionTableDTO>(program, &program->execution_table());
  return std::make_shared<const core::ProgramExecutionTableDTO>(std::move(*table));
}
} // namespace

core::Result<void> ValidateSealedProgramCaptureRequest(
    const core::ProgramRunExecutionCaptureDTO &capture,
    const deployment::RetainedBundle &bundle) {
  if (capture.table != nullptr || capture.binding != nullptr ||
      capture.blackboard_key_declarations != nullptr ||
      capture.io_conditions != nullptr ||
      capture.retained_bundle_id != bundle.retained_bundle_id() ||
      capture.graph_revision != bundle.value().cell_graph.graph_revision)
    return core::Result<void>::Err(CaptureError(
        "Program capture must name the active product and carry no table",
        "program_capture_product_mismatch", core::ErrorCode::kInvalidRequest));
  return core::Result<void>::Ok();
}

core::Result<void> ValidateSealedProgramCaptureBinding(
    const core::ProgramRunExecutionCaptureDTO &capture,
    const deployment::ProgramBinding *binding) {
  // Repository key matching used to establish Binding identity implicitly.
  // A caller with an already sealed pair must prove the same exact join.
  if (binding == nullptr ||
      binding->program_binding_id() != capture.program_binding_id ||
      binding->value().program_id != capture.program_id ||
      binding->value().program_revision != capture.program_revision)
    return core::Result<void>::Err(
        CaptureError("Program capture has no exact generated Binding",
                     "program_capture_binding_unavailable",
                     core::ErrorCode::kMissingReference));
  return core::Result<void>::Ok();
}

core::Result<core::ProgramRunExecutionCaptureDTO> ResolveSealedProgramCapture(
    core::ProgramRunExecutionCaptureDTO capture,
    std::shared_ptr<const deployment::ProgramBindingContract> program,
    std::shared_ptr<const deployment::ProgramBinding> binding,
    const deployment::RetainedBundle &bundle) {
  using Reply = core::Result<core::ProgramRunExecutionCaptureDTO>;
  try {
    const auto request = ValidateSealedProgramCaptureRequest(capture, bundle);
    if (!request.ok())
      return Reply::Err(request.error());
    const auto selected_binding =
        ValidateSealedProgramCaptureBinding(capture, binding.get());
    if (!selected_binding.ok())
      return Reply::Err(selected_binding.error());
    if (program == nullptr ||
        program->value().program_id != capture.program_id ||
        program->value().program_revision != capture.program_revision ||
        program->value().node_environment != binding->value().node_environment)
      return Reply::Err(
          CaptureError("Program capture has no exact canonical Program",
                       "program_capture_source_unavailable",
                       core::ErrorCode::kMissingReference));
    auto conditions =
        deployment::compile_program_io_conditions(*program, *binding, bundle);
    if (!conditions.ok())
      return Reply::Err(conditions.error());
    capture.io_conditions =
        std::make_shared<const core::ProgramIoConditionProjectionDTO>(
            std::move(conditions.value()));
    capture.table = PneumaticExecutionTable(program, *binding);
    capture.binding = std::shared_ptr<const core::ProgramBindingDTO>(
        binding, &binding->value());
    capture.blackboard_key_declarations = std::shared_ptr<
        const std::vector<core::ProgramBlackboardKeyDeclarationDTO>>(
        program, &program->blackboard_key_declarations());
    return Reply::Ok(std::move(capture));
  } catch (const std::exception &exception) {
    return Reply::Err(CaptureError(
        std::string("Program capture product resolution failed: ") +
            exception.what(),
        "program_capture_resolution_failed"));
  }
}

} // namespace manyforge::roboplan_backend::detail

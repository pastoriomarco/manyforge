#include "sealed_program_execution_capture.hpp"

#include <manyforge_deployment/program_io_condition_compiler.hpp>
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
    capture.table = std::shared_ptr<const core::ProgramExecutionTableDTO>(
        program, &program->execution_table());
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

#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/types/command_schema.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {

void register_command_schema_bindings(py::module_& module) {
  py::enum_<CommandValueContractKind>(module, "CommandValueContractKind")
      .value("kGripperPosition",
             CommandValueContractKind::kGripperPosition);
  py::enum_<CommandEffortLimitContract>(module,
                                        "CommandEffortLimitContract")
      .value(
          "kOptionalNonnegativeMatchingCoordinate",
          CommandEffortLimitContract::
              kOptionalNonnegativeMatchingCoordinate);
  py::enum_<CommandCallerOptionValueKind>(
      module, "CommandCallerOptionValueKind")
      .value("kNonnegativeDurationSeconds",
             CommandCallerOptionValueKind::kNonnegativeDurationSeconds);
  py::enum_<CommandCompletionRule>(module, "CommandCompletionRule")
      .value("kGoalFencedTargetReached",
             CommandCompletionRule::kGoalFencedTargetReached);
  py::enum_<CommandConfirmationRule>(module, "CommandConfirmationRule")
      .value("kBindingFeedbackRequired",
             CommandConfirmationRule::kBindingFeedbackRequired);
  py::enum_<CommandStopEvidenceRequirement>(
      module, "CommandStopEvidenceRequirement")
      .value("kCommandTerminal",
             CommandStopEvidenceRequirement::kCommandTerminal);

  py::class_<CommandSchemaCompatibleCapabilityDTO>(
      module, "CommandSchemaCompatibleCapabilityDTO", py::is_final())
      .def_readonly("capability_id",
                    &CommandSchemaCompatibleCapabilityDTO::capability_id)
      .def_readonly(
          "capability_definition_revision",
          &CommandSchemaCompatibleCapabilityDTO::
              capability_definition_revision)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<GripperPositionCommandValueContractDTO>(
      module, "GripperPositionCommandValueContractDTO", py::is_final())
      .def_readonly(
          "accepted_position_kinds",
          &GripperPositionCommandValueContractDTO::accepted_position_kinds)
      .def_readonly(
          "minimum_resource_count",
          &GripperPositionCommandValueContractDTO::minimum_resource_count)
      .def_readonly(
          "maximum_resource_count",
          &GripperPositionCommandValueContractDTO::maximum_resource_count)
      .def_readonly(
          "max_effort_contract",
          &GripperPositionCommandValueContractDTO::max_effort_contract)
      .def(py::self == py::self)
      .def(py::self != py::self);
  module.attr("CommandValueContractDTO") =
      py::module_::import("typing")
          .attr("Union")
          .attr("__getitem__")(
              py::make_tuple(
                  module.attr("GripperPositionCommandValueContractDTO")));
  py::class_<CommandCallerOptionDTO>(module, "CommandCallerOptionDTO",
                                     py::is_final())
      .def_readonly("option_id", &CommandCallerOptionDTO::option_id)
      .def_readonly("value_kind", &CommandCallerOptionDTO::value_kind)
      .def_readonly("required", &CommandCallerOptionDTO::required)
      .def(py::self == py::self)
      .def(py::self != py::self);
  py::class_<CommandSchemaDTO>(module, "CommandSchemaDTO", py::is_final())
      .def_readonly("command_schema_id", &CommandSchemaDTO::command_schema_id)
      .def_readonly("command_schema_revision",
                    &CommandSchemaDTO::command_schema_revision)
      .def_readonly("compatible_capabilities",
                    &CommandSchemaDTO::compatible_capabilities)
      .def_readonly("command_value", &CommandSchemaDTO::command_value)
      .def_readonly("resource_role_selectors",
                    &CommandSchemaDTO::resource_role_selectors)
      .def_readonly("channel_role_selectors",
                    &CommandSchemaDTO::channel_role_selectors)
      .def_readonly("caller_options", &CommandSchemaDTO::caller_options)
      .def_readonly("completion_rule", &CommandSchemaDTO::completion_rule)
      .def_readonly("confirmation_rule", &CommandSchemaDTO::confirmation_rule)
      .def_readonly("stop_evidence_requirements",
                    &CommandSchemaDTO::stop_evidence_requirements)
      .def(py::self == py::self)
      .def(py::self != py::self);

  module.attr("COMMAND_SCHEMA_API_VERSION") =
      py::str(kCommandSchemaApiVersion);
  module.attr("GRIPPER_POSITION_COMMAND_SCHEMA_ID") =
      py::str(kGripperPositionCommandSchemaId);
  module.attr("GRIPPER_POSITION_COMMAND_SCHEMA_REVISION") =
      py::str(kGripperPositionCommandSchemaRevision);
}

}  // namespace manyforge::core::python

#pragma once

#include <pybind11/pybind11.h>

namespace manyforge::core::python {

void register_strong_identity_bindings(pybind11::module_& module);
void register_catalog_record_bindings(pybind11::module_& module);
void register_cell_graph_bindings(pybind11::module_& module);
void register_component_lifecycle_bindings(pybind11::module_& module);
void register_component_type_bindings(pybind11::module_& module);
void register_command_schema_bindings(pybind11::module_& module);
void register_coordinate_state_bindings(pybind11::module_& module);
void register_interrupt_bindings(pybind11::module_& module);
void register_operation_runtime_bindings(pybind11::module_& module);
void register_deployment_candidate_bindings(pybind11::module_& module);
void register_model_compiler_bindings(pybind11::module_& module);
void register_scenario_catalog_bindings(pybind11::module_& module);

}  // namespace manyforge::core::python

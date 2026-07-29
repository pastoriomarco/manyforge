#include <pybind11/pybind11.h>

#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <string>
#include <string_view>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {

void register_deployment_candidate_bindings(py::module_& module) {
  using deployment::IntrinsicallyValidatedCellGraphCandidate;
  using deployment::ParsedArtifactCandidate;
  using deployment::TypedCellGraphCandidate;

  py::register_exception<deployment::ArtifactCodecError>(module, "ArtifactCodecError",
                                                         PyExc_ValueError);
  py::register_exception<deployment::CellGraphCodecError>(module, "CellGraphCodecError",
                                                          PyExc_ValueError);

  py::class_<ParsedArtifactCandidate>(module, "ParsedArtifactCandidate", py::is_final())
      .def_property_readonly("canonical_json", [](const ParsedArtifactCandidate& candidate) {
        py::gil_scoped_release release;
        return deployment::canonical_json(candidate.root());
      });

  py::class_<TypedCellGraphCandidate>(module, "TypedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const TypedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json", [](const TypedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  py::class_<IntrinsicallyValidatedCellGraphCandidate>(
      module, "IntrinsicallyValidatedCellGraphCandidate", py::is_final())
      .def_property_readonly("graph",
                             [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
                               py::gil_scoped_release release;
                               return candidate.graph();
                             })
      .def_property_readonly(
          "structurally_normalized_canonical_json",
          [](const IntrinsicallyValidatedCellGraphCandidate& candidate) {
            py::gil_scoped_release release;
            return deployment::canonical_json(candidate.structurally_normalized_root());
          });

  module.def(
      "parse_strict_json_model_yaml",
      [](const std::string& yaml_bytes) {
        return deployment::parse_strict_json_model_yaml(yaml_bytes);
      },
      py::arg("yaml_bytes"), py::call_guard<py::gil_scoped_release>());
  module.def("decode_cell_graph_candidate", &deployment::decode_cell_graph_candidate,
             py::arg("parsed"), py::call_guard<py::gil_scoped_release>());
  module.def("validate_intrinsic_cell_graph_candidate",
             &deployment::validate_intrinsic_cell_graph_candidate, py::arg("candidate"),
             py::call_guard<py::gil_scoped_release>());
}

}  // namespace manyforge::core::python

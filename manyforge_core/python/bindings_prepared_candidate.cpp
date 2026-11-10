#include <pybind11/pybind11.h>

#include <algorithm>
#include <exception>
#include <filesystem>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <utility>

#include <manyforge_core/types/strong_id.hpp>
#include <manyforge_deployment/activation_preparation_request.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/program_reference_diagnostics.hpp>
#include <manyforge_deployment/program_reference_validator.hpp>

#include "prepared_activation_materialization_internal.hpp"
#include "prepared_catalog_source_internal.hpp"
#include "prepared_selector_facts_internal.hpp"

namespace py = pybind11;
namespace deployment = manyforge::deployment;

namespace manyforge::core::python {
namespace {

class PreparedActivationCandidateError final : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Python owns this exception type for the lifetime of the module. Its
// instances retain the native ProgramContractError code and detail values.
PyObject* program_contract_error_type = nullptr;

// This object owns the immutable exact materialization. No mutable graph,
// publication port, or independently supplied catalog is exposed to Python.
class PreparedActivationCandidate final {
 public:
  explicit PreparedActivationCandidate(
      std::shared_ptr<const deployment::detail::PreparedActivationMaterialization> value)
      : value_(std::move(value)) {}

  [[nodiscard]] std::string validate_program_references(
      const std::string& program_json, bool draft) const {
    deployment::ParsedArtifactCandidate program = [&]() {
      try {
        return deployment::parse_strict_json_model_yaml(program_json);
      } catch (const deployment::ArtifactCodecError& error) {
        throw deployment::ProgramContractError(error.what());
      }
    }();
    const deployment::detail::PreparedCatalogSource source(value_->catalog_artifacts);
    const auto diagnostics = deployment::validate_program_references(
        program.root(), draft, *value_->bundle, value_->model_map, source);
    // Same bounded wire representation used by activation preparation.
    return deployment::bounded_program_reference_refusal(diagnostics, 1024U * 1024U);
  }

  [[nodiscard]] std::string selector_facts_json() const {
    try {
      return deployment::canonical_json(deployment::detail::prepared_selector_facts(*value_));
    } catch (const std::bad_alloc&) {
      throw;
    } catch (const std::exception& error) {
      throw PreparedActivationCandidateError(error.what());
    }
  }

 private:
  std::shared_ptr<const deployment::detail::PreparedActivationMaterialization> value_;
};

[[nodiscard]] PreparedActivationCandidate prepare_candidate(
    const std::string& capability_root, const std::string& deployment_json,
    const std::string& source_closure_json, const std::string& prepared_closure_json,
    const std::string& cell_id) {
  try {
    if (cell_id.empty() || cell_id.size() > 128U || cell_id.front() < 'a' ||
        cell_id.front() > 'z' ||
        !std::all_of(cell_id.begin(), cell_id.end(), [](char c) {
          return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '-' || c == '.';
        })) {
      throw std::invalid_argument("runtime_cell_id must be a bounded portable identifier");
    }
    const auto limit = deployment::kMaxActivationPreparationRequestBytes;
    if (deployment_json.size() > limit || source_closure_json.size() > limit ||
        prepared_closure_json.size() > limit ||
        deployment_json.size() > limit - source_closure_json.size() ||
        deployment_json.size() + source_closure_json.size() > limit - prepared_closure_json.size()) {
      throw std::length_error("activation preparation inputs exceed the request bound");
    }
    const auto deployment_value = deployment::parse_strict_json_model_yaml(deployment_json, limit);
    const auto source_closure = deployment::parse_strict_json_model_yaml(source_closure_json, limit);
    const auto prepared_closure = deployment::parse_strict_json_model_yaml(prepared_closure_json, limit);
    return PreparedActivationCandidate(deployment::detail::build_prepared_activation_candidate(
        std::filesystem::path(capability_root), deployment_value.root(), source_closure.root(),
        prepared_closure.root(), CellId(cell_id), false));
  } catch (const std::bad_alloc&) {
    throw;
  } catch (const std::exception& error) {
    throw PreparedActivationCandidateError(error.what());
  }
}

}  // namespace

void register_prepared_activation_candidate_bindings(py::module_& module) {
  py::register_exception<PreparedActivationCandidateError>(module, "PreparedActivationCandidateError");
  py::register_exception<deployment::ProgramReferenceValidationIncomplete>(
      module, "ProgramReferenceValidationIncomplete");
  auto contract_error = py::reinterpret_steal<py::object>(PyErr_NewException(
      "manyforge_core_py.ProgramContractError", PyExc_ValueError, nullptr));
  if (!contract_error) throw py::error_already_set();
  module.attr("ProgramContractError") = contract_error;
  program_contract_error_type = contract_error.ptr();
  py::register_local_exception_translator([](std::exception_ptr error) {
    if (!error) return;
    try {
      std::rethrow_exception(error);
    } catch (const deployment::ProgramContractError& exception) {
      auto instance = py::reinterpret_steal<py::object>(
          PyObject_CallFunction(program_contract_error_type, "s", exception.what()));
      if (!instance) return;
      instance.attr("code") = py::str(exception.code());
      py::dict details;
      if (const auto& format = exception.unsupported_format()) {
        details["actual_api_version"] = py::str(format->actual_api_version);
        details["supported_api_version"] = py::str(format->supported_api_version);
      }
      instance.attr("details") = std::move(details);
      PyErr_SetObject(program_contract_error_type, instance.ptr());
    }
  });
  py::class_<PreparedActivationCandidate>(module, "PreparedActivationCandidate")
      .def("validate_program_references", &PreparedActivationCandidate::validate_program_references,
           py::arg("program_json"), py::arg("draft"), py::call_guard<py::gil_scoped_release>())
      .def("selector_facts_json", &PreparedActivationCandidate::selector_facts_json,
           py::call_guard<py::gil_scoped_release>());
  module.def("prepare_activation_candidate", &prepare_candidate,
             py::arg("capability_root"), py::arg("deployment_json"),
             py::arg("model_source_closure_json"), py::arg("prepared_model_closure_json"),
             py::arg("cell_id"), py::call_guard<py::gil_scoped_release>());
}

}  // namespace manyforge::core::python

#include <pybind11/pybind11.h>

#include <manyforge_deployment/scenario_catalog.hpp>

#include <filesystem>
#include <stdexcept>
#include <string>
#include <utility>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

[[nodiscard]] deployment::ScenarioAssistantMode assistant_mode(
    const std::string& value) {
  if (value == "required") {
    return deployment::ScenarioAssistantMode::kRequired;
  }
  if (value == "optional") {
    return deployment::ScenarioAssistantMode::kOptional;
  }
  if (value == "forbidden") {
    return deployment::ScenarioAssistantMode::kForbidden;
  }
  throw py::value_error("assistant_mode is not required, optional, or forbidden");
}

}  // namespace

void register_scenario_catalog_bindings(py::module_& module) {
  module.def(
      "verify_scenario_artifact_closure",
      [](std::string scenario_id, std::string scenario_catalog_revision,
         std::string repository_root, std::string scenario_catalog_path,
         std::string deployment_path, std::string retained_bundle_path,
         std::string program_binding_path, std::string program_path,
         std::string deployment_artifact_id,
         std::string deployment_revision, std::string retained_bundle_id,
         std::string program_binding_id, std::string graph_revision,
         std::string program_id, std::string program_revision,
         std::string node_environment_digest, std::string assistant_mode_value,
         std::string typed_parameter_overrides_json) {
        const deployment::ScenarioArtifactClosureQuery query{
            .scenario_id = std::move(scenario_id),
            .scenario_catalog_revision = ScenarioCatalogRevision(
                std::move(scenario_catalog_revision)),
            .repository_root = std::filesystem::path(repository_root),
            .scenario_catalog_path =
                std::filesystem::path(scenario_catalog_path),
            .deployment_path = std::filesystem::path(deployment_path),
            .retained_bundle_path =
                std::filesystem::path(retained_bundle_path),
            .program_binding_path =
                std::filesystem::path(program_binding_path),
            .program_path = std::filesystem::path(program_path),
            .deployment_artifact_id = DeploymentArtifactId(
                std::move(deployment_artifact_id)),
            .deployment_revision =
                DeploymentRevision(std::move(deployment_revision)),
            .retained_bundle_id =
                RetainedBundleId(std::move(retained_bundle_id)),
            .program_binding_id =
                ProgramBindingId(std::move(program_binding_id)),
            .graph_revision = GraphRevision(std::move(graph_revision)),
            .program_id = ProgramId(std::move(program_id)),
            .program_revision = ProgramRevision(std::move(program_revision)),
            .node_environment_digest =
                ContentDigest(std::move(node_environment_digest)),
            .assistant_mode = assistant_mode(assistant_mode_value),
            .typed_parameter_overrides_json =
                std::move(typed_parameter_overrides_json),
        };
        deployment::VerifiedScenarioArtifactClosure verified{
            "", false};
        {
          py::gil_scoped_release release;
          verified = deployment::verify_scenario_artifact_closure(query);
        }
        return py::make_tuple(verified.lane, verified.assistant_enabled);
      },
      py::arg("scenario_id"), py::arg("scenario_catalog_revision"),
      py::arg("repository_root"), py::arg("scenario_catalog_path"),
      py::arg("deployment_path"), py::arg("retained_bundle_path"),
      py::arg("program_binding_path"), py::arg("program_path"),
      py::arg("deployment_artifact_id"), py::arg("deployment_revision"),
      py::arg("retained_bundle_id"), py::arg("program_binding_id"),
      py::arg("graph_revision"), py::arg("program_id"),
      py::arg("program_revision"), py::arg("node_environment_digest"),
      py::arg("assistant_mode"),
      py::arg("typed_parameter_overrides_json"));
}

}  // namespace manyforge::core::python

#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <manyforge_core/types/geometry.hpp>
#include <manyforge_core/types/kernel_event.hpp>
#include <manyforge_core/types/execution_result.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/cell_structure.hpp>
#include <manyforge_deployment/closed_schema.hpp>

#include "proof_types.hpp"

namespace py = pybind11;

namespace {

using manyforge::core::ExecutionState;
using manyforge::core::ExecutionSnapshot;
using manyforge::core::KernelEvent;
using manyforge::core::KernelEventType;
using manyforge::core::Pose3DTO;

const char *event_type_name(KernelEventType type) {
  switch (type) {
  case KernelEventType::kStateTransition:
    return "state_transition";
  case KernelEventType::kExecuteStarted:
    return "execute_started";
  default:
    return "other";
  }
}

py::dict materialize_kernel_event() {
  KernelEvent event{
      .type = KernelEventType::kStateTransition,
      .execution_id = 42,
      .timestamp_ns = 123456,
      .from_state = ExecutionState::kIdle,
      .to_state = ExecutionState::kExecuting,
      .error = std::nullopt,
      .details = {{"program_run_id", "run-7"}, {"source", "e0-proof"}},
      .sequence = 9,
  };
  py::dict result;
  result["type"] = event_type_name(event.type);
  result["execution_id"] = event.execution_id;
  result["timestamp_ns"] = event.timestamp_ns;
  result["sequence"] = event.sequence;
  result["from_state"] = manyforge::core::to_string(event.from_state);
  result["to_state"] = manyforge::core::to_string(event.to_state);
  py::dict details;
  for (const auto &[key, value] : event.details) {
    details[py::str(key)] = value;
  }
  result["details"] = details;
  result["program_run_id"] = event.details.at("program_run_id");
  return result;
}

py::dict materialize_execution_snapshot() {
  ExecutionSnapshot snapshot;
  snapshot.timestamp_ns = 123456;
  snapshot.sequence_number = 17;
  snapshot.execution_id = 42;
  snapshot.move_id = "move-6";
  snapshot.state = ExecutionState::kExecuting;
  snapshot.progress = 0.625;
  snapshot.joint_positions_rad = {0.1, -0.2, 0.3};
  snapshot.joint_velocities_rad_s = {0.01, -0.02, 0.03};
  snapshot.state_source = "e0-proof";
  snapshot.source_timestamp_ns = 123000;
  snapshot.source_age_ns = 456;
  snapshot.contact_policy_id = "contact-policy-1";

  py::dict result;
  result["timestamp_ns"] = snapshot.timestamp_ns;
  result["sequence_number"] = snapshot.sequence_number;
  result["execution_id"] = snapshot.execution_id;
  result["move_id"] = snapshot.move_id;
  result["state"] = manyforge::core::to_string(snapshot.state);
  result["progress"] = snapshot.progress;
  result["joint_positions_rad"] = snapshot.joint_positions_rad;
  result["joint_velocities_rad_s"] = snapshot.joint_velocities_rad_s;
  result["state_source"] = snapshot.state_source;
  result["source_timestamp_ns"] = snapshot.source_timestamp_ns;
  result["source_age_ns"] = snapshot.source_age_ns;
  result["contact_policy_id"] = snapshot.contact_policy_id;
  return result;
}

py::dict pose_round_trip(const py::dict &input) {
  const auto position = input["position_m"].cast<py::dict>();
  const auto orientation = input["orientation"].cast<py::dict>();
  const Pose3DTO pose{
      .position_m =
          {
              .x_m = position["x_m"].cast<double>(),
              .y_m = position["y_m"].cast<double>(),
              .z_m = position["z_m"].cast<double>(),
          },
      .orientation =
          {
              .x = orientation["x"].cast<double>(),
              .y = orientation["y"].cast<double>(),
              .z = orientation["z"].cast<double>(),
              .w = orientation["w"].cast<double>(),
          },
  };
  py::dict output_position;
  output_position["x_m"] = pose.position_m.x_m;
  output_position["y_m"] = pose.position_m.y_m;
  output_position["z_m"] = pose.position_m.z_m;
  py::dict output_orientation;
  output_orientation["x"] = pose.orientation.x;
  output_orientation["y"] = pose.orientation.y;
  output_orientation["z"] = pose.orientation.z;
  output_orientation["w"] = pose.orientation.w;
  py::dict output;
  output["position_m"] = output_position;
  output["orientation"] = output_orientation;
  return output;
}

py::dict qualify_empty_cell() {
  constexpr std::string_view yaml = R"YAML(
api_version: manyforge.cell_graph.v1
graph_revision: sha256:0000000000000000000000000000000000000000000000000000000000000000
cell_id: e0_empty
runtime_units: []
model_instances: []
controllable_components: []
control_domains: []
control_bindings: []
component_attachments: []
)YAML";
  const auto parsed =
      manyforge::deployment::parse_strict_json_model_yaml(yaml);
  const auto decoded =
      manyforge::deployment::decode_cell_graph_candidate(parsed);
  const auto intrinsic =
      manyforge::deployment::validate_intrinsic_cell_graph_candidate(decoded);
  const auto identity_ready =
      manyforge::deployment::prepare_cell_graph_identity(intrinsic, {});
  const auto revision_verified =
      manyforge::deployment::compute_graph_revision(identity_ready);
  const auto qualified = manyforge::deployment::qualify_cell_structure(
      revision_verified, {}, {});
  py::dict result;
  result["cell_id"] = qualified.graph().cell_id.value();
  result["graph_revision"] = qualified.graph().graph_revision.value();
  result["model_instance_count"] = qualified.model_instances().size();
  result["component_count"] = qualified.components().size();
  return result;
}

py::dict compile_closed_schema(const std::string &descriptor) {
  const auto compiled =
      manyforge::deployment::compile_closed_schema_descriptor(
          manyforge::deployment::parse_strict_json_model_yaml(descriptor));
  py::dict result;
  result["schema_id"] = compiled.schema_id().value();
  result["schema_revision"] = compiled.schema_revision().value();
  result["root_shape"] = static_cast<int>(compiled.root_shape());
  return result;
}

std::string compile_schema_revision_repeated(const std::string &descriptor,
                                             std::uint64_t repetitions) {
  if (repetitions == 0 || repetitions > 10'000) {
    throw std::invalid_argument("repetitions must be in [1, 10000]");
  }
  std::string revision;
  for (std::uint64_t index = 0; index < repetitions; ++index) {
    const auto compiled =
        manyforge::deployment::compile_closed_schema_descriptor(
            manyforge::deployment::parse_strict_json_model_yaml(descriptor));
    revision = compiled.schema_revision().value();
  }
  return revision;
}

} // namespace

PYBIND11_MODULE(e0_pybind, module) {
  using manyforge::core::CellId;
  using manyforge::deployment::ParsedArtifactCandidate;
  using namespace manyforge::e0;

  py::register_exception<manyforge::deployment::ArtifactCodecError>(
      module, "ArtifactCodecError", PyExc_ValueError);

  py::enum_<KernelEventType>(module, "KernelEventType")
      .value("StateTransition", KernelEventType::kStateTransition)
      .value("ExecuteStarted", KernelEventType::kExecuteStarted);

  py::class_<CellId>(module, "CellId", py::is_final())
      .def(py::init<std::string>(), py::arg("value"))
      .def_property_readonly("value", &CellId::value)
      .def("__str__", [](const CellId &value) { return value.value(); })
      .def("__eq__",
           [](const CellId &lhs, const CellId &rhs) { return lhs == rhs; })
      .def("__hash__", [](const CellId &value) {
        return std::hash<std::string>{}(value.value());
      });

  py::class_<Snapshot>(module, "Snapshot", py::is_final())
      .def_property_readonly("sequence",
                             [](const Snapshot &value) {
                               return value.sequence;
                             })
      .def_property_readonly("values",
                             [](const Snapshot &value) {
                               return py::tuple(py::cast(value.values));
                             })
      .def("__eq__", [](const Snapshot &lhs, const Snapshot &rhs) {
        return lhs == rhs;
      });

  py::class_<OptionalConfig>(module, "OptionalConfig", py::is_final())
      .def(py::init([](std::string name, std::optional<std::string> note) {
             return OptionalConfig{std::move(name), std::move(note)};
           }),
           py::arg("name"), py::arg("note") = py::none())
      .def_property_readonly("name",
                             [](const OptionalConfig &value) {
                               return value.name;
                             })
      .def_property_readonly("note",
                             [](const OptionalConfig &value) {
                               return value.note;
                             });

  py::class_<Child>(module, "Child", py::is_final())
      .def_property_readonly("value",
                             [](const Child &value) {
                               return value.value;
                             });
  py::class_<Parent>(module, "Parent", py::is_final())
      .def(py::init<std::string>())
      .def_property_readonly(
          "child", &Parent::child,
          py::return_value_policy::reference_internal);

  py::class_<SharedToken, std::shared_ptr<SharedToken>>(
      module, "SharedToken", py::is_final())
      .def_property_readonly("value", &SharedToken::value)
      .def_static("live", &SharedToken::live);

  py::class_<ParsedArtifactCandidate>(
      module, "ParsedArtifactCandidate", py::is_final())
      .def_property_readonly(
          "canonical_json", [](const ParsedArtifactCandidate &candidate) {
            py::gil_scoped_release release;
            return manyforge::deployment::canonical_json(candidate.root());
          });

  module.def("make_snapshot", &make_snapshot, py::arg("count"));
  module.def("make_shared_token", [](std::uint64_t value) {
    return std::make_shared<SharedToken>(value);
  });
  module.def(
      "parse_yaml",
      [](const std::string &value) {
        py::gil_scoped_release release;
        return manyforge::deployment::parse_strict_json_model_yaml(value);
      },
      py::arg("value"));
  module.def("qualify_empty_cell", &qualify_empty_cell);
  module.def("compile_closed_schema", &compile_closed_schema,
             py::arg("descriptor"));
  module.def("compile_schema_revision_repeated",
             &compile_schema_revision_repeated, py::arg("descriptor"),
             py::arg("repetitions"),
             py::call_guard<py::gil_scoped_release>());
  module.def("materialize_execution_snapshot",
             &materialize_execution_snapshot);
  module.def("materialize_kernel_event", &materialize_kernel_event);
  module.def("pose_round_trip", &pose_round_trip, py::arg("value"));
  module.def("bounded_work", &bounded_work, py::arg("iterations"),
             py::call_guard<py::gil_scoped_release>());
  module.def("bounded_wait", &bounded_wait, py::arg("milliseconds"),
             py::call_guard<py::gil_scoped_release>());
  module.def("bounded_wait_recorded", &bounded_wait_recorded,
             py::arg("milliseconds"),
             py::call_guard<py::gil_scoped_release>());
  module.def("recorded_wait_start_count", &recorded_wait_start_count);
  module.def("recorded_wait_completion_count",
             &recorded_wait_completion_count);
}

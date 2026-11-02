#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/shared_ptr.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/vector.h>

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

namespace nb = nanobind;
using namespace nb::literals;

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

nb::dict materialize_kernel_event() {
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
  nb::dict result;
  result["type"] = event_type_name(event.type);
  result["execution_id"] = event.execution_id;
  result["timestamp_ns"] = event.timestamp_ns;
  result["sequence"] = event.sequence;
  result["from_state"] = manyforge::core::to_string(event.from_state);
  result["to_state"] = manyforge::core::to_string(event.to_state);
  nb::dict details;
  for (const auto &[key, value] : event.details) {
    details[nb::str(key.c_str())] = value;
  }
  result["details"] = details;
  result["program_run_id"] = event.details.at("program_run_id");
  return result;
}

nb::dict materialize_execution_snapshot() {
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

  nb::dict result;
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

nb::dict pose_round_trip(const nb::dict &input) {
  const auto position = nb::cast<nb::dict>(input["position_m"]);
  const auto orientation = nb::cast<nb::dict>(input["orientation"]);
  const Pose3DTO pose{
      .position_m =
          {
              .x_m = nb::cast<double>(position["x_m"]),
              .y_m = nb::cast<double>(position["y_m"]),
              .z_m = nb::cast<double>(position["z_m"]),
          },
      .orientation =
          {
              .x = nb::cast<double>(orientation["x"]),
              .y = nb::cast<double>(orientation["y"]),
              .z = nb::cast<double>(orientation["z"]),
              .w = nb::cast<double>(orientation["w"]),
          },
  };
  nb::dict output_position;
  output_position["x_m"] = pose.position_m.x_m;
  output_position["y_m"] = pose.position_m.y_m;
  output_position["z_m"] = pose.position_m.z_m;
  nb::dict output_orientation;
  output_orientation["x"] = pose.orientation.x;
  output_orientation["y"] = pose.orientation.y;
  output_orientation["z"] = pose.orientation.z;
  output_orientation["w"] = pose.orientation.w;
  nb::dict output;
  output["position_m"] = output_position;
  output["orientation"] = output_orientation;
  return output;
}

nb::dict qualify_empty_cell() {
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
  nb::dict result;
  result["cell_id"] = qualified.graph().cell_id.value();
  result["graph_revision"] = qualified.graph().graph_revision.value();
  result["model_instance_count"] = qualified.model_instances().size();
  result["component_count"] = qualified.components().size();
  return result;
}

nb::dict compile_closed_schema(const std::string &descriptor) {
  const auto compiled =
      manyforge::deployment::compile_closed_schema_descriptor(
          manyforge::deployment::parse_strict_json_model_yaml(descriptor));
  nb::dict result;
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

NB_MODULE(e0_nanobind, module) {
  using manyforge::core::CellId;
  using manyforge::deployment::ParsedArtifactCandidate;
  using namespace manyforge::e0;

  nb::exception<manyforge::deployment::ArtifactCodecError>(
      module, "ArtifactCodecError", PyExc_ValueError);

  nb::enum_<KernelEventType>(module, "KernelEventType")
      .value("StateTransition", KernelEventType::kStateTransition)
      .value("ExecuteStarted", KernelEventType::kExecuteStarted);

  nb::class_<CellId>(module, "CellId")
      .def(nb::init<std::string>(), "value"_a)
      .def_prop_ro("value", &CellId::value)
      .def("__str__", [](const CellId &value) { return value.value(); })
      .def("__eq__",
           [](const CellId &lhs, const CellId &rhs) { return lhs == rhs; })
      .def("__hash__", [](const CellId &value) {
        return std::hash<std::string>{}(value.value());
      });

  nb::class_<Snapshot>(module, "Snapshot")
      .def_prop_ro("sequence",
                   [](const Snapshot &value) {
                     return value.sequence;
                   })
      .def_prop_ro("values",
                   [](const Snapshot &value) {
                     return nb::tuple(nb::cast(value.values));
                   })
      .def("__eq__", [](const Snapshot &lhs, const Snapshot &rhs) {
        return lhs == rhs;
      });

  nb::class_<OptionalConfig>(module, "OptionalConfig")
      .def(
          "__init__",
          [](OptionalConfig *self, std::string name,
             std::optional<std::string> note) {
            new (self)
                OptionalConfig{std::move(name), std::move(note)};
          },
          "name"_a, "note"_a = nb::none())
      .def_prop_ro("name",
                   [](const OptionalConfig &value) {
                     return value.name;
                   })
      .def_prop_ro("note",
                   [](const OptionalConfig &value) {
                     return value.note;
                   });

  nb::class_<Child>(module, "Child")
      .def_prop_ro("value",
                   [](const Child &value) {
                     return value.value;
                   });
  nb::class_<Parent>(module, "Parent")
      .def(nb::init<std::string>())
      .def_prop_ro("child", &Parent::child,
                   nb::rv_policy::reference_internal);

  nb::class_<SharedToken>(module, "SharedToken")
      .def_prop_ro("value", &SharedToken::value)
      .def_static("live", &SharedToken::live);

  nb::class_<ParsedArtifactCandidate>(module, "ParsedArtifactCandidate")
      .def_prop_ro(
          "canonical_json", [](const ParsedArtifactCandidate &candidate) {
            nb::gil_scoped_release release;
            return manyforge::deployment::canonical_json(candidate.root());
          });

  module.def("make_snapshot", &make_snapshot, "count"_a);
  module.def("make_shared_token", [](std::uint64_t value) {
    return std::make_shared<SharedToken>(value);
  });
  module.def(
      "parse_yaml",
      [](const std::string &value) {
        nb::gil_scoped_release release;
        return manyforge::deployment::parse_strict_json_model_yaml(value);
      },
      "value"_a);
  module.def("qualify_empty_cell", &qualify_empty_cell);
  module.def("compile_closed_schema", &compile_closed_schema,
             "descriptor"_a);
  module.def("compile_schema_revision_repeated",
             &compile_schema_revision_repeated, "descriptor"_a,
             "repetitions"_a,
             nb::call_guard<nb::gil_scoped_release>());
  module.def("materialize_execution_snapshot",
             &materialize_execution_snapshot);
  module.def("materialize_kernel_event", &materialize_kernel_event);
  module.def("pose_round_trip", &pose_round_trip, "value"_a);
  module.def("bounded_work", &bounded_work, "iterations"_a,
             nb::call_guard<nb::gil_scoped_release>());
  module.def("bounded_wait", &bounded_wait, "milliseconds"_a,
             nb::call_guard<nb::gil_scoped_release>());
  module.def("bounded_wait_recorded", &bounded_wait_recorded,
             "milliseconds"_a,
             nb::call_guard<nb::gil_scoped_release>());
  module.def("recorded_wait_start_count", &recorded_wait_start_count);
  module.def("recorded_wait_completion_count",
             &recorded_wait_completion_count);
}

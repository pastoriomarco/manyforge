#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <manyforge_core/types/coordinate_state.hpp>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

template <typename ResultType>
void require_ok(const ResultType& result) {
  if (!result.ok()) {
    const auto& error = result.error();
    const auto reason = error.details.find("reason");
    throw py::value_error(
        reason == error.details.end() ? error.message : reason->second);
  }
}

}  // namespace

void register_coordinate_state_bindings(py::module_& module) {
  py::enum_<CoordinateKind>(module, "CoordinateKind")
      .value("kAngular", CoordinateKind::kAngular)
      .value("kLinear", CoordinateKind::kLinear);

  py::class_<AngularPositionDTO>(module, "AngularPositionDTO")
      .def(py::init<double>(), py::arg("value_rad"))
      .def_readwrite("value_rad", &AngularPositionDTO::value_rad);
  py::class_<LinearPositionDTO>(module, "LinearPositionDTO")
      .def(py::init<double>(), py::arg("value_m"))
      .def_readwrite("value_m", &LinearPositionDTO::value_m);
  py::class_<AngularVelocityDTO>(module, "AngularVelocityDTO")
      .def(py::init<double>(), py::arg("value_rad_s"))
      .def_readwrite("value_rad_s", &AngularVelocityDTO::value_rad_s);
  py::class_<LinearVelocityDTO>(module, "LinearVelocityDTO")
      .def(py::init<double>(), py::arg("value_m_s"))
      .def_readwrite("value_m_s", &LinearVelocityDTO::value_m_s);
  py::class_<AngularAccelerationDTO>(module, "AngularAccelerationDTO")
      .def(py::init<double>(), py::arg("value_rad_s2"))
      .def_readwrite("value_rad_s2", &AngularAccelerationDTO::value_rad_s2);
  py::class_<LinearAccelerationDTO>(module, "LinearAccelerationDTO")
      .def(py::init<double>(), py::arg("value_m_s2"))
      .def_readwrite("value_m_s2", &LinearAccelerationDTO::value_m_s2);
  py::class_<AngularEffortDTO>(module, "AngularEffortDTO")
      .def(py::init<double>(), py::arg("value_nm"))
      .def_readwrite("value_nm", &AngularEffortDTO::value_nm);
  py::class_<LinearEffortDTO>(module, "LinearEffortDTO")
      .def(py::init<double>(), py::arg("value_n"))
      .def_readwrite("value_n", &LinearEffortDTO::value_n);

  py::class_<ControlledStateDTO>(module, "ControlledStateDTO")
      .def(py::init<
               ControllableComponentId, CommandTargetId, ControlDomainId,
               ControlDomainRevision, ControlBindingId,
               ControlBindingRevision, ControllerTopologyId,
               TopologyRevision, ControlRouteId,
               ActiveControlConfigurationId,
               ActiveControlConfigurationGeneration,
               std::vector<ResourceId>, std::vector<std::string>,
               std::vector<CoordinatePositionDTO>,
               std::optional<std::vector<CoordinateVelocityDTO>>>(),
           py::arg("controllable_component_id"), py::arg("command_target_id"),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("control_binding_id"), py::arg("control_binding_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("control_route_id"),
           py::arg("active_control_configuration_id"),
           py::arg("active_control_configuration_generation"),
           py::arg("resources"), py::arg("joint_names"),
           py::arg("positions"), py::arg("velocities") = std::nullopt)
      .def_readwrite("controllable_component_id",
                     &ControlledStateDTO::controllable_component_id)
      .def_readwrite("command_target_id", &ControlledStateDTO::command_target_id)
      .def_readwrite("control_domain_id", &ControlledStateDTO::control_domain_id)
      .def_readwrite("control_domain_revision",
                     &ControlledStateDTO::control_domain_revision)
      .def_readwrite("control_binding_id", &ControlledStateDTO::control_binding_id)
      .def_readwrite("control_binding_revision",
                     &ControlledStateDTO::control_binding_revision)
      .def_readwrite("controller_topology_id",
                     &ControlledStateDTO::controller_topology_id)
      .def_readwrite("topology_revision", &ControlledStateDTO::topology_revision)
      .def_readwrite("control_route_id", &ControlledStateDTO::control_route_id)
      .def_readwrite("active_control_configuration_id",
                     &ControlledStateDTO::active_control_configuration_id)
      .def_readwrite("active_control_configuration_generation",
                     &ControlledStateDTO::active_control_configuration_generation)
      .def_readwrite("resources", &ControlledStateDTO::resources)
      .def_readwrite("joint_names", &ControlledStateDTO::joint_names)
      .def_readwrite("positions", &ControlledStateDTO::positions)
      .def_readwrite("velocities", &ControlledStateDTO::velocities);

  py::enum_<ObservedStateSourceKind>(module, "ObservedStateSourceKind")
      .value("kActionFeedback", ObservedStateSourceKind::kActionFeedback)
      .value("kControllerState", ObservedStateSourceKind::kControllerState)
      .value("kJointState", ObservedStateSourceKind::kJointState)
      .value("kCommandedState", ObservedStateSourceKind::kCommandedState)
      .value("kPredictedState", ObservedStateSourceKind::kPredictedState);

  py::class_<ObservedStateSnapshotDTO>(module, "ObservedStateSnapshotDTO")
      .def(py::init<
               ControllableComponentId, CommandTargetId, ControlDomainId,
               ControlDomainRevision, ControlBindingId,
               ControlBindingRevision, ProviderInstanceId,
               ProviderManifestId, ProviderManifestRevision,
               ControllerTopologyId, TopologyRevision, ControlRouteId,
               ActiveControlConfigurationId,
               ActiveControlConfigurationGeneration,
               std::vector<ResourceId>, std::uint64_t,
               std::vector<std::string>,
               std::vector<CoordinatePositionDTO>,
               std::optional<std::vector<CoordinateVelocityDTO>>, std::string,
               RuntimeEpoch, ObservedStateSourceKind, std::int64_t,
               std::int64_t, StateAuthorityId, StateAuthorityClass,
               std::uint64_t, std::int64_t, bool>(),
           py::arg("controllable_component_id"), py::arg("command_target_id"),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("control_binding_id"), py::arg("control_binding_revision"),
           py::arg("provider_instance_id"), py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("control_route_id"),
           py::arg("active_control_configuration_id"),
           py::arg("active_control_configuration_generation"),
           py::arg("resources"), py::arg("sequence"), py::arg("joint_names"),
           py::arg("positions"), py::arg("velocities"), py::arg("source_id"),
           py::arg("source_epoch"), py::arg("source_kind"),
           py::arg("source_timestamp_ns"), py::arg("received_timestamp_ns"),
           py::arg("state_authority_id"), py::arg("state_authority_class"),
           py::arg("authority_generation"), py::arg("freshness_budget_ns"),
           py::arg("complete"))
      .def_readwrite("controllable_component_id",
                     &ObservedStateSnapshotDTO::controllable_component_id)
      .def_readwrite("command_target_id",
                     &ObservedStateSnapshotDTO::command_target_id)
      .def_readwrite("control_domain_id",
                     &ObservedStateSnapshotDTO::control_domain_id)
      .def_readwrite("control_domain_revision",
                     &ObservedStateSnapshotDTO::control_domain_revision)
      .def_readwrite("control_binding_id",
                     &ObservedStateSnapshotDTO::control_binding_id)
      .def_readwrite("control_binding_revision",
                     &ObservedStateSnapshotDTO::control_binding_revision)
      .def_readwrite("provider_instance_id",
                     &ObservedStateSnapshotDTO::provider_instance_id)
      .def_readwrite("provider_manifest_id",
                     &ObservedStateSnapshotDTO::provider_manifest_id)
      .def_readwrite("provider_manifest_revision",
                     &ObservedStateSnapshotDTO::provider_manifest_revision)
      .def_readwrite("controller_topology_id",
                     &ObservedStateSnapshotDTO::controller_topology_id)
      .def_readwrite("topology_revision",
                     &ObservedStateSnapshotDTO::topology_revision)
      .def_readwrite("control_route_id",
                     &ObservedStateSnapshotDTO::control_route_id)
      .def_readwrite("active_control_configuration_id",
                     &ObservedStateSnapshotDTO::active_control_configuration_id)
      .def_readwrite("active_control_configuration_generation",
                     &ObservedStateSnapshotDTO::active_control_configuration_generation)
      .def_readwrite("resources", &ObservedStateSnapshotDTO::resources)
      .def_readwrite("sequence", &ObservedStateSnapshotDTO::sequence)
      .def_readwrite("joint_names", &ObservedStateSnapshotDTO::joint_names)
      .def_readwrite("positions", &ObservedStateSnapshotDTO::positions)
      .def_readwrite("velocities", &ObservedStateSnapshotDTO::velocities)
      .def_readwrite("source_id", &ObservedStateSnapshotDTO::source_id)
      .def_readwrite("source_epoch", &ObservedStateSnapshotDTO::source_epoch)
      .def_readwrite("source_kind", &ObservedStateSnapshotDTO::source_kind)
      .def_readwrite("source_timestamp_ns",
                     &ObservedStateSnapshotDTO::source_timestamp_ns)
      .def_readwrite("received_timestamp_ns",
                     &ObservedStateSnapshotDTO::received_timestamp_ns)
      .def_readwrite("state_authority_id",
                     &ObservedStateSnapshotDTO::state_authority_id)
      .def_readwrite("state_authority_class",
                     &ObservedStateSnapshotDTO::state_authority_class)
      .def_readwrite("authority_generation",
                     &ObservedStateSnapshotDTO::authority_generation)
      .def_readwrite("freshness_budget_ns",
                     &ObservedStateSnapshotDTO::freshness_budget_ns)
      .def_readwrite("complete", &ObservedStateSnapshotDTO::complete);
  module.def("validate_controlled_state", [](const ControlledStateDTO& state) {
    require_ok(validate_controlled_state(state));
  });
  module.def("validate_observed_state_snapshot",
             [](const ObservedStateSnapshotDTO& state, std::int64_t now_ns) {
               require_ok(validate_observed_state_snapshot(state, now_ns));
             });
  module.def("angular_position_projection",
             [](const ControlledStateDTO& state) {
               auto result = angular_position_projection(state);
               require_ok(result);
               return result.value();
             });
}

}  // namespace manyforge::core::python

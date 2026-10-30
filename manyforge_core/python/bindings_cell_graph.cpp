#include <pybind11/operators.h>
#include <pybind11/pybind11.h>
#include <pybind11/stl.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/json_value.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "binding_registrations.hpp"

namespace py = pybind11;

namespace manyforge::core::python {
namespace {

constexpr std::int64_t kMaxSafeJsonInteger = 9007199254740991LL;

struct JsonConversionContext final {
  std::size_t node_count = 0U;
  std::set<PyObject*, std::less<>> active_containers;

  void consume_node() {
    if (node_count >= deployment::kMaxArtifactNodeCount) {
      throw py::value_error("JSON value exceeds the v1 node-count limit");
    }
    ++node_count;
  }
};

class ActiveJsonContainer final {
 public:
  ActiveJsonContainer(JsonConversionContext& context, PyObject* container)
      : context_(context), container_(container) {
    if (context_.active_containers.size() >= deployment::kMaxArtifactNestingDepth) {
      throw py::value_error("JSON value exceeds the v1 nesting-depth limit");
    }
    if (!context_.active_containers.emplace(container_).second) {
      throw py::value_error("cyclic JSON containers are forbidden");
    }
  }

  ~ActiveJsonContainer() { context_.active_containers.erase(container_); }

  ActiveJsonContainer(const ActiveJsonContainer&) = delete;
  ActiveJsonContainer& operator=(const ActiveJsonContainer&) = delete;

 private:
  JsonConversionContext& context_;
  PyObject* container_;
};

template <typename Value>
JsonValueDTO make_json_value(Value&& value) {
  JsonValueDTO result;
  result.value = std::forward<Value>(value);
  return result;
}

[[nodiscard]] JsonValueDTO json_value_from_python(py::handle value,
                                                  JsonConversionContext& context) {
  const bool is_list = PyList_CheckExact(value.ptr()) != 0;
  const bool is_dict = PyDict_CheckExact(value.ptr()) != 0;
  if (is_list || is_dict) {
    ActiveJsonContainer active_container(context, value.ptr());
    context.consume_node();

    if (is_list) {
      JsonValueDTO::Array result;
      const auto size = PyList_GET_SIZE(value.ptr());
      result.reserve(static_cast<std::size_t>(size));
      for (Py_ssize_t index = 0; index < size; ++index) {
        result.push_back(json_value_from_python(PyList_GET_ITEM(value.ptr(), index), context));
      }
      return make_json_value(std::move(result));
    }

    JsonValueDTO::Object result;
    PyObject* key = nullptr;
    PyObject* member = nullptr;
    Py_ssize_t position = 0;
    while (PyDict_Next(value.ptr(), &position, &key, &member) != 0) {
      context.consume_node();
      if (PyUnicode_CheckExact(key) == 0) {
        throw py::type_error("JSON object keys must be strings");
      }
      result.emplace(py::cast<std::string>(py::handle(key)),
                     json_value_from_python(py::handle(member), context));
    }
    return make_json_value(std::move(result));
  }

  context.consume_node();
  if (value.is_none()) {
    return JsonValueDTO{};
  }
  if (PyBool_Check(value.ptr()) != 0) {
    return make_json_value(value.ptr() == Py_True);
  }
  if (PyLong_CheckExact(value.ptr()) != 0) {
    int overflow = 0;
    const long long integer = PyLong_AsLongLongAndOverflow(value.ptr(), &overflow);
    if (overflow != 0 || PyErr_Occurred() != nullptr) {
      PyErr_Clear();
      throw py::value_error("integer is outside the lossless I-JSON range");
    }
    if (integer < -kMaxSafeJsonInteger || integer > kMaxSafeJsonInteger) {
      throw py::value_error("integer is outside the lossless I-JSON range");
    }
    return make_json_value(static_cast<double>(integer));
  }
  if (PyFloat_CheckExact(value.ptr()) != 0) {
    const double number = PyFloat_AS_DOUBLE(value.ptr());
    if (!std::isfinite(number)) {
      throw py::value_error("number is not a finite I-JSON binary64 value");
    }
    if (number == 0.0 && std::signbit(number)) {
      throw py::value_error("negative zero is forbidden");
    }
    if (std::trunc(number) == number && (number < -static_cast<double>(kMaxSafeJsonInteger) ||
                                         number > static_cast<double>(kMaxSafeJsonInteger))) {
      throw py::value_error("integer-valued number is outside the lossless I-JSON range");
    }
    return make_json_value(number);
  }
  if (PyUnicode_CheckExact(value.ptr()) != 0) {
    return make_json_value(py::cast<std::string>(value));
  }
  throw py::type_error(
      "JsonValueDTO accepts only None, bool, int, float, str, list, "
      "or dict values");
}

[[nodiscard]] JsonValueDTO json_value_from_python(py::handle value) {
  JsonConversionContext context;
  return json_value_from_python(value, context);
}

}  // namespace

void register_cell_graph_bindings(py::module_& module) {
  module.attr("CELL_GRAPH_API_VERSION") = py::str(std::string{kCellGraphApiVersion});

  py::enum_<ContainmentGranularity>(module, "ContainmentGranularity")
      .value("kResource", ContainmentGranularity::kResource)
      .value("kControllerSet", ContainmentGranularity::kControllerSet)
      .value("kControlDomain", ContainmentGranularity::kControlDomain);

  py::enum_<StateAuthorityClass>(module, "StateAuthorityClass")
      .value("kMeasured", StateAuthorityClass::kMeasured)
      .value("kCommanded", StateAuthorityClass::kCommanded)
      .value("kPredicted", StateAuthorityClass::kPredicted);

  py::enum_<RuntimeScaleAcknowledgementKind>(
      module, "RuntimeScaleAcknowledgementKind")
      .value("kAppliedState", RuntimeScaleAcknowledgementKind::kAppliedState);

  py::class_<JsonValueDTO>(module, "JsonValueDTO", py::is_final())
      .def(py::init<>())
      .def(py::init([](py::handle value) { return json_value_from_python(value); }),
           py::arg("value"))
      .def_readonly("value", &JsonValueDTO::value)
      .def(
          "__eq__", [](const JsonValueDTO& lhs, const JsonValueDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const JsonValueDTO& lhs, const JsonValueDTO& rhs) { return lhs != rhs; },
          py::is_operator())
      .def("__repr__", [](const JsonValueDTO& value) {
        return py::str("JsonValueDTO({})").format(py::repr(py::cast(value.value)));
      });

  py::class_<PlacementPoseDTO>(module, "PlacementPoseDTO", py::is_final())
      .def(py::init([](std::array<double, 3> position_m, std::array<double, 4> orientation_xyzw) {
             return PlacementPoseDTO{position_m, orientation_xyzw};
           }),
           py::arg("position_m"), py::arg("orientation_xyzw"))
      .def_readonly("position_m", &PlacementPoseDTO::position_m)
      .def_readonly("orientation_xyzw", &PlacementPoseDTO::orientation_xyzw)
      .def(
          "__eq__",
          [](const PlacementPoseDTO& lhs, const PlacementPoseDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const PlacementPoseDTO& lhs, const PlacementPoseDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ModelRoleBindingDTO>(module, "ModelRoleBindingDTO", py::is_final())
      .def(py::init([](ModelRoleId model_role_id, ModelInstanceId model_instance_id) {
             return ModelRoleBindingDTO{std::move(model_role_id), std::move(model_instance_id)};
           }),
           py::arg("model_role_id"), py::arg("model_instance_id"))
      .def_readonly("model_role_id", &ModelRoleBindingDTO::model_role_id)
      .def_readonly("model_instance_id", &ModelRoleBindingDTO::model_instance_id)
      .def(
          "__eq__",
          [](const ModelRoleBindingDTO& lhs, const ModelRoleBindingDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelRoleBindingDTO& lhs, const ModelRoleBindingDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ResourceBindingDTO>(module, "ResourceBindingDTO", py::is_final())
      .def(py::init([](ResourceRoleId resource_role_id, ResourceId resource_id) {
             return ResourceBindingDTO{std::move(resource_role_id), std::move(resource_id)};
           }),
           py::arg("resource_role_id"), py::arg("resource_id"))
      .def_readonly("resource_role_id", &ResourceBindingDTO::resource_role_id)
      .def_readonly("resource_id", &ResourceBindingDTO::resource_id)
      .def(
          "__eq__",
          [](const ResourceBindingDTO& lhs, const ResourceBindingDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ResourceBindingDTO& lhs, const ResourceBindingDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<RouteEndpointProjectionDTO>(module, "RouteEndpointProjectionDTO", py::is_final())
      .def(py::init([](ControlRouteId control_route_id,
                       std::vector<EndpointRoleId> command_endpoint_role_ids,
                       std::vector<EndpointRoleId> state_endpoint_role_ids) {
             return RouteEndpointProjectionDTO{
                 std::move(control_route_id),
                 std::move(command_endpoint_role_ids),
                 std::move(state_endpoint_role_ids),
             };
           }),
           py::arg("control_route_id"), py::arg("command_endpoint_role_ids"),
           py::arg("state_endpoint_role_ids"))
      .def_readonly("control_route_id", &RouteEndpointProjectionDTO::control_route_id)
      .def_readonly("command_endpoint_role_ids",
                    &RouteEndpointProjectionDTO::command_endpoint_role_ids)
      .def_readonly("state_endpoint_role_ids", &RouteEndpointProjectionDTO::state_endpoint_role_ids)
      .def(
          "__eq__",
          [](const RouteEndpointProjectionDTO& lhs, const RouteEndpointProjectionDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const RouteEndpointProjectionDTO& lhs, const RouteEndpointProjectionDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<RuntimeScaleRouteQualificationDTO>(
      module, "RuntimeScaleRouteQualificationDTO", py::is_final())
      .def(py::init([](
                        ControlRouteId control_route_id,
                        ScalingDomainId scaling_domain_id,
                        EndpointRoleId command_endpoint_role_id,
                        EndpointRoleId applied_state_endpoint_role_id,
                        std::optional<CoordinatedScalingBarrierId>
                            coordinated_barrier_id,
                        RuntimeScaleAcknowledgementKind acknowledgement_kind,
                        double maximum_slew_per_second,
                        std::int64_t freshness_budget_ns) {
             return RuntimeScaleRouteQualificationDTO{
                 std::move(control_route_id),
                 std::move(scaling_domain_id),
                 std::move(command_endpoint_role_id),
                 std::move(applied_state_endpoint_role_id),
                 std::move(coordinated_barrier_id),
                 acknowledgement_kind,
                 maximum_slew_per_second,
                 freshness_budget_ns};
           }),
           py::arg("control_route_id"), py::arg("scaling_domain_id"),
           py::arg("command_endpoint_role_id"),
           py::arg("applied_state_endpoint_role_id"),
           py::arg("coordinated_barrier_id"),
           py::arg("acknowledgement_kind"),
           py::arg("maximum_slew_per_second"),
           py::arg("freshness_budget_ns"))
      .def_readonly("control_route_id",
                    &RuntimeScaleRouteQualificationDTO::control_route_id)
      .def_readonly("scaling_domain_id",
                    &RuntimeScaleRouteQualificationDTO::scaling_domain_id)
      .def_readonly("command_endpoint_role_id",
                    &RuntimeScaleRouteQualificationDTO::command_endpoint_role_id)
      .def_readonly(
          "applied_state_endpoint_role_id",
          &RuntimeScaleRouteQualificationDTO::applied_state_endpoint_role_id)
      .def_readonly("coordinated_barrier_id",
                    &RuntimeScaleRouteQualificationDTO::coordinated_barrier_id)
      .def_readonly("acknowledgement_kind",
                    &RuntimeScaleRouteQualificationDTO::acknowledgement_kind)
      .def_readonly("maximum_slew_per_second",
                    &RuntimeScaleRouteQualificationDTO::maximum_slew_per_second)
      .def_readonly("freshness_budget_ns",
                    &RuntimeScaleRouteQualificationDTO::freshness_budget_ns)
      .def(
          "__eq__",
          [](const RuntimeScaleRouteQualificationDTO& lhs,
             const RuntimeScaleRouteQualificationDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const RuntimeScaleRouteQualificationDTO& lhs,
             const RuntimeScaleRouteQualificationDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ComponentAttachmentEndpointDTO>(module, "ComponentAttachmentEndpointDTO",
                                             py::is_final())
      .def(py::init([](ControllableComponentId controllable_component_id,
                       AttachmentInterfaceId attachment_interface_id) {
             return ComponentAttachmentEndpointDTO{
                 std::move(controllable_component_id),
                 std::move(attachment_interface_id),
             };
           }),
           py::arg("controllable_component_id"), py::arg("attachment_interface_id"))
      .def_readonly("controllable_component_id",
                    &ComponentAttachmentEndpointDTO::controllable_component_id)
      .def_readonly("attachment_interface_id",
                    &ComponentAttachmentEndpointDTO::attachment_interface_id)
      .def(
          "__eq__",
          [](const ComponentAttachmentEndpointDTO& lhs, const ComponentAttachmentEndpointDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ComponentAttachmentEndpointDTO& lhs, const ComponentAttachmentEndpointDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<RuntimeUnitDTO>(module, "RuntimeUnitDTO", py::is_final())
      .def(py::init([](RuntimeUnitId runtime_unit_id, RuntimeProfileId runtime_profile_id,
                       RuntimeProfileRevision runtime_profile_revision,
                       std::vector<RuntimeUnitId> dependency_runtime_unit_ids,
                       JsonObjectDTO configuration) {
             return RuntimeUnitDTO{
                 std::move(runtime_unit_id), std::move(runtime_profile_id),
                 std::move(runtime_profile_revision),
                 std::move(dependency_runtime_unit_ids), std::move(configuration),
             };
           }),
           py::arg("runtime_unit_id"), py::arg("runtime_profile_id"),
           py::arg("runtime_profile_revision"), py::arg("dependency_runtime_unit_ids"),
           py::arg("configuration"))
      .def_readonly("runtime_unit_id", &RuntimeUnitDTO::runtime_unit_id)
      .def_readonly("runtime_profile_id", &RuntimeUnitDTO::runtime_profile_id)
      .def_readonly("runtime_profile_revision", &RuntimeUnitDTO::runtime_profile_revision)
      .def_readonly("dependency_runtime_unit_ids", &RuntimeUnitDTO::dependency_runtime_unit_ids)
      .def_readonly("configuration", &RuntimeUnitDTO::configuration)
      .def(
          "__eq__", [](const RuntimeUnitDTO& lhs, const RuntimeUnitDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const RuntimeUnitDTO& lhs, const RuntimeUnitDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ModelCustomTcpDTO>(module, "ModelCustomTcpDTO", py::is_final())
      .def(py::init([](FrameRoleId frame_role_id, std::string parent_link_name,
                       PlacementPoseDTO pose_in_parent_link) {
             return ModelCustomTcpDTO{std::move(frame_role_id), std::move(parent_link_name),
                                      std::move(pose_in_parent_link)};
           }),
           py::arg("frame_role_id"), py::arg("parent_link_name"),
           py::arg("pose_in_parent_link"))
      .def_readonly("frame_role_id", &ModelCustomTcpDTO::frame_role_id)
      .def_readonly("parent_link_name", &ModelCustomTcpDTO::parent_link_name)
      .def_readonly("pose_in_parent_link", &ModelCustomTcpDTO::pose_in_parent_link)
      .def(
          "__eq__",
          [](const ModelCustomTcpDTO& lhs, const ModelCustomTcpDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelCustomTcpDTO& lhs, const ModelCustomTcpDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ModelInstanceDTO>(module, "ModelInstanceDTO", py::is_final())
      .def(py::init([](ModelInstanceId model_instance_id, ModelAssetId model_asset_id,
                       ModelRevision model_revision,
                       std::optional<PlacementPoseDTO> root_pose_in_cell,
                       std::vector<ModelCustomTcpDTO> custom_tcps) {
             return ModelInstanceDTO{
                 std::move(model_instance_id), std::move(model_asset_id),
                 std::move(model_revision), std::move(root_pose_in_cell),
                 std::move(custom_tcps),
             };
           }),
           py::arg("model_instance_id"), py::arg("model_asset_id"), py::arg("model_revision"),
           py::arg("root_pose_in_cell") = std::nullopt,
           py::arg("custom_tcps") = std::vector<ModelCustomTcpDTO>{})
      .def_readonly("model_instance_id", &ModelInstanceDTO::model_instance_id)
      .def_readonly("model_asset_id", &ModelInstanceDTO::model_asset_id)
      .def_readonly("model_revision", &ModelInstanceDTO::model_revision)
      .def_readonly("root_pose_in_cell", &ModelInstanceDTO::root_pose_in_cell)
      .def_readonly("custom_tcps", &ModelInstanceDTO::custom_tcps)
      .def(
          "__eq__",
          [](const ModelInstanceDTO& lhs, const ModelInstanceDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ModelInstanceDTO& lhs, const ModelInstanceDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ComponentDefaultTcpDTO>(module, "ComponentDefaultTcpDTO", py::is_final())
      .def(py::init([](CommandTargetId command_target_id, FrameRoleId frame_role_id) {
             return ComponentDefaultTcpDTO{std::move(command_target_id),
                                           std::move(frame_role_id)};
           }),
           py::arg("command_target_id"), py::arg("frame_role_id"))
      .def_readonly("command_target_id", &ComponentDefaultTcpDTO::command_target_id)
      .def_readonly("frame_role_id", &ComponentDefaultTcpDTO::frame_role_id)
      .def(
          "__eq__",
          [](const ComponentDefaultTcpDTO& lhs, const ComponentDefaultTcpDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ComponentDefaultTcpDTO& lhs, const ComponentDefaultTcpDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ControllableComponentDTO>(module, "ControllableComponentDTO", py::is_final())
      .def(py::init([](ControllableComponentId controllable_component_id,
                       ComponentTypeId component_type_id,
                       ComponentTypeRevision component_type_revision,
                       std::vector<ModelRoleBindingDTO> model_role_bindings,
                       std::vector<RuntimeUnitId> runtime_unit_ids,
                       std::optional<PlacementPoseDTO> root_pose_in_cell,
                       std::optional<ComponentDefaultTcpDTO> default_tcp) {
             return ControllableComponentDTO{
                 std::move(controllable_component_id), std::move(component_type_id),
                 std::move(component_type_revision), std::move(model_role_bindings),
                 std::move(runtime_unit_ids), std::move(root_pose_in_cell),
                 std::move(default_tcp),
             };
           }),
           py::arg("controllable_component_id"), py::arg("component_type_id"),
           py::arg("component_type_revision"), py::arg("model_role_bindings"),
           py::arg("runtime_unit_ids"), py::arg("root_pose_in_cell") = std::nullopt,
           py::arg("default_tcp") = std::nullopt)
      .def_readonly("controllable_component_id",
                    &ControllableComponentDTO::controllable_component_id)
      .def_readonly("component_type_id", &ControllableComponentDTO::component_type_id)
      .def_readonly("component_type_revision", &ControllableComponentDTO::component_type_revision)
      .def_readonly("model_role_bindings", &ControllableComponentDTO::model_role_bindings)
      .def_readonly("runtime_unit_ids", &ControllableComponentDTO::runtime_unit_ids)
      .def_readonly("root_pose_in_cell", &ControllableComponentDTO::root_pose_in_cell)
      .def_readonly("default_tcp", &ControllableComponentDTO::default_tcp)
      .def(
          "__eq__",
          [](const ControllableComponentDTO& lhs, const ControllableComponentDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControllableComponentDTO& lhs, const ControllableComponentDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<ProviderControlDomainDTO>(module, "ProviderControlDomainDTO", py::is_final())
      .def(py::init<ControllableComponentId, RuntimeUnitId, ProviderInstanceId, ProviderManifestId,
                    ProviderManifestRevision, ContentDigest, ContentDigest,
                    std::vector<std::string>, Duration>(),
           py::arg("controllable_component_id"), py::arg("provider_runtime_unit_id"),
           py::arg("provider_instance_id"), py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"), py::arg("lifecycle_plan_catalog_digest"),
           py::arg("signal_configuration_digest"), py::arg("signal_names"),
           py::arg("lifecycle_command_timeout_ns"))
      .def_readonly("controllable_component_id",
                    &ProviderControlDomainDTO::controllable_component_id)
      .def_readonly("provider_runtime_unit_id", &ProviderControlDomainDTO::provider_runtime_unit_id)
      .def_readonly("provider_instance_id", &ProviderControlDomainDTO::provider_instance_id)
      .def_readonly("provider_manifest_id", &ProviderControlDomainDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision",
                    &ProviderControlDomainDTO::provider_manifest_revision)
      .def_readonly("lifecycle_plan_catalog_digest",
                    &ProviderControlDomainDTO::lifecycle_plan_catalog_digest)
      .def_readonly("signal_configuration_digest",
                    &ProviderControlDomainDTO::signal_configuration_digest)
      .def_readonly("signal_names", &ProviderControlDomainDTO::signal_names)
      .def_readonly("lifecycle_command_timeout_ns",
                    &ProviderControlDomainDTO::lifecycle_command_timeout_ns)
      .def("locally_valid", &ProviderControlDomainDTO::locally_valid)
      .def(py::self == py::self);

  py::class_<ControlDomainDTO>(module, "ControlDomainDTO", py::is_final())
      .def(py::init<ControlDomainId, ControlDomainRevision, std::vector<RuntimeUnitId>,
                    EndpointRoleId, ContainmentGranularity, ProviderControlDomainDTO>(),
           py::arg("control_domain_id"), py::arg("control_domain_revision"),
           py::arg("runtime_unit_ids"), py::arg("control_coordinator_endpoint_role"),
           py::arg("containment_granularity"), py::arg("provider"))
      .def_property_readonly(
          "provider", [](const ControlDomainDTO& value) { return value.provider(); },
          py::return_value_policy::reference_internal)
      .def(
          py::init(
              [](ControlDomainId control_domain_id, ControlDomainRevision control_domain_revision,
                 std::vector<RuntimeUnitId> runtime_unit_ids,
                 EndpointRoleId control_coordinator_endpoint_role,
                 ControllerTopologyId controller_topology_id, TopologyRevision topology_revision,
                 ContainmentGranularity containment_granularity) {
                return ControlDomainDTO{
                    std::move(control_domain_id),      std::move(control_domain_revision),
                    std::move(runtime_unit_ids),       std::move(control_coordinator_endpoint_role),
                    std::move(controller_topology_id), std::move(topology_revision),
                    containment_granularity,
                };
              }),
          py::arg("control_domain_id"), py::arg("control_domain_revision"),
          py::arg("runtime_unit_ids"), py::arg("control_coordinator_endpoint_role"),
          py::arg("controller_topology_id"), py::arg("topology_revision"),
          py::arg("containment_granularity"))
      .def_readonly("control_domain_id", &ControlDomainDTO::control_domain_id)
      .def_readonly("control_domain_revision", &ControlDomainDTO::control_domain_revision)
      .def_readonly("runtime_unit_ids", &ControlDomainDTO::runtime_unit_ids)
      .def_readonly("control_coordinator_endpoint_role",
                    &ControlDomainDTO::control_coordinator_endpoint_role)
      .def_property_readonly(
          "controller_topology_id",
          [](const ControlDomainDTO& value) -> const ControllerTopologyId& {
            return value.controller().controller_topology_id;
          },
          py::return_value_policy::reference_internal)
      .def_property_readonly(
          "topology_revision",
          [](const ControlDomainDTO& value) -> const TopologyRevision& {
            return value.controller().topology_revision;
          },
          py::return_value_policy::reference_internal)
      .def_readonly("containment_granularity", &ControlDomainDTO::containment_granularity)
      .def(
          "__eq__",
          [](const ControlDomainDTO& lhs, const ControlDomainDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControlDomainDTO& lhs, const ControlDomainDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ControlBindingDTO>(module, "ControlBindingDTO", py::is_final())
      .def(py::init([](ControlBindingId control_binding_id,
                       ControlBindingRevision control_binding_revision,
                       ControllableComponentId controllable_component_id,
                       CommandTargetId command_target_id, CapabilityId capability_id,
                       CapabilityDefinitionRevision capability_definition_revision,
                       ControlDomainId control_domain_id,
                       std::vector<ResourceBindingDTO> resource_bindings,
                       ControlInterfaceContractId control_interface_contract_id,
                       ControlInterfaceRevision control_interface_revision,
                       ControllerTopologyId controller_topology_id,
                       TopologyRevision topology_revision,
                       std::vector<ControlRouteId> legal_control_route_ids,
                       std::vector<RouteEndpointProjectionDTO> route_endpoint_projections,
                       ProviderInstanceId provider_instance_id,
                       RuntimeUnitId provider_runtime_unit_id,
                       ProviderManifestId provider_manifest_id,
                       ProviderManifestRevision provider_manifest_revision,
                       StateAuthorityId state_authority_id,
                       StateAuthorityClass state_authority_class,
                       StopEvidencePolicyId stop_evidence_policy_id,
                       StopEvidencePolicyRevision stop_evidence_policy_revision,
                       ContainmentGranularity containment_granularity,
                       std::vector<RuntimeScaleRouteQualificationDTO>
                           runtime_scale_route_qualifications) {
             return ControlBindingDTO{
                 std::move(control_binding_id),
                 std::move(control_binding_revision),
                 std::move(controllable_component_id),
                 std::move(command_target_id),
                 std::move(capability_id),
                 std::move(capability_definition_revision),
                 std::move(control_domain_id),
                 std::move(resource_bindings),
                 std::move(control_interface_contract_id),
                 std::move(control_interface_revision),
                 std::move(controller_topology_id),
                 std::move(topology_revision),
                 std::move(legal_control_route_ids),
                 std::move(route_endpoint_projections),
                 std::move(provider_instance_id),
                 std::move(provider_runtime_unit_id),
                 std::move(provider_manifest_id),
                 std::move(provider_manifest_revision),
                 std::move(state_authority_id),
                 state_authority_class,
                 std::move(stop_evidence_policy_id),
                 std::move(stop_evidence_policy_revision),
                 containment_granularity,
                 std::move(runtime_scale_route_qualifications),
             };
           }),
           py::arg("control_binding_id"), py::arg("control_binding_revision"),
           py::arg("controllable_component_id"), py::arg("command_target_id"),
           py::arg("capability_id"), py::arg("capability_definition_revision"),
           py::arg("control_domain_id"), py::arg("resource_bindings"),
           py::arg("control_interface_contract_id"), py::arg("control_interface_revision"),
           py::arg("controller_topology_id"), py::arg("topology_revision"),
           py::arg("legal_control_route_ids"), py::arg("route_endpoint_projections"),
           py::arg("provider_instance_id"),
           py::arg("provider_runtime_unit_id"),
           py::arg("provider_manifest_id"),
           py::arg("provider_manifest_revision"),
           py::arg("state_authority_id"), py::arg("state_authority_class"),
           py::arg("stop_evidence_policy_id"), py::arg("stop_evidence_policy_revision"),
           py::arg("containment_granularity"),
           py::arg("runtime_scale_route_qualifications") =
               std::vector<RuntimeScaleRouteQualificationDTO>{})
      .def_readonly("control_binding_id", &ControlBindingDTO::control_binding_id)
      .def_readonly("control_binding_revision", &ControlBindingDTO::control_binding_revision)
      .def_readonly("controllable_component_id", &ControlBindingDTO::controllable_component_id)
      .def_readonly("command_target_id", &ControlBindingDTO::command_target_id)
      .def_readonly("capability_id", &ControlBindingDTO::capability_id)
      .def_readonly("capability_definition_revision",
                    &ControlBindingDTO::capability_definition_revision)
      .def_readonly("control_domain_id", &ControlBindingDTO::control_domain_id)
      .def_readonly("resource_bindings", &ControlBindingDTO::resource_bindings)
      .def_readonly("control_interface_contract_id",
                    &ControlBindingDTO::control_interface_contract_id)
      .def_readonly("control_interface_revision", &ControlBindingDTO::control_interface_revision)
      .def_readonly("controller_topology_id", &ControlBindingDTO::controller_topology_id)
      .def_readonly("topology_revision", &ControlBindingDTO::topology_revision)
      .def_readonly("legal_control_route_ids", &ControlBindingDTO::legal_control_route_ids)
      .def_readonly("route_endpoint_projections", &ControlBindingDTO::route_endpoint_projections)
      .def_readonly("provider_instance_id", &ControlBindingDTO::provider_instance_id)
      .def_readonly("provider_runtime_unit_id",
                    &ControlBindingDTO::provider_runtime_unit_id)
      .def_readonly("provider_manifest_id", &ControlBindingDTO::provider_manifest_id)
      .def_readonly("provider_manifest_revision", &ControlBindingDTO::provider_manifest_revision)
      .def_readonly("state_authority_id", &ControlBindingDTO::state_authority_id)
      .def_readonly("state_authority_class", &ControlBindingDTO::state_authority_class)
      .def_readonly("stop_evidence_policy_id", &ControlBindingDTO::stop_evidence_policy_id)
      .def_readonly("stop_evidence_policy_revision",
                    &ControlBindingDTO::stop_evidence_policy_revision)
      .def_readonly("containment_granularity", &ControlBindingDTO::containment_granularity)
      .def_readonly("runtime_scale_route_qualifications",
                    &ControlBindingDTO::runtime_scale_route_qualifications)
      .def(
          "__eq__",
          [](const ControlBindingDTO& lhs, const ControlBindingDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__",
          [](const ControlBindingDTO& lhs, const ControlBindingDTO& rhs) { return lhs != rhs; },
          py::is_operator());

  py::class_<ComponentAttachmentEdgeDTO>(module, "ComponentAttachmentEdgeDTO", py::is_final())
      .def(py::init([](ComponentAttachmentId component_attachment_id,
                       ComponentAttachmentRevision component_attachment_revision,
                       ComponentAttachmentEndpointDTO parent, ComponentAttachmentEndpointDTO child,
                       PlacementPoseDTO child_interface_pose_in_parent_interface,
                       bool primary) {
             return ComponentAttachmentEdgeDTO{
                 std::move(component_attachment_id),
                 std::move(component_attachment_revision),
                 std::move(parent),
                 std::move(child),
                 std::move(child_interface_pose_in_parent_interface),
                 primary,
             };
           }),
           py::arg("component_attachment_id"), py::arg("component_attachment_revision"),
           py::arg("parent"), py::arg("child"),
           py::arg("child_interface_pose_in_parent_interface"), py::arg("primary") = false)
      .def_readonly("component_attachment_id", &ComponentAttachmentEdgeDTO::component_attachment_id)
      .def_readonly("component_attachment_revision",
                    &ComponentAttachmentEdgeDTO::component_attachment_revision)
      .def_readonly("parent", &ComponentAttachmentEdgeDTO::parent)
      .def_readonly("child", &ComponentAttachmentEdgeDTO::child)
      .def_readonly("child_interface_pose_in_parent_interface",
                    &ComponentAttachmentEdgeDTO::child_interface_pose_in_parent_interface)
      .def_readonly("primary", &ComponentAttachmentEdgeDTO::primary)
      .def(
          "__eq__",
          [](const ComponentAttachmentEdgeDTO& lhs, const ComponentAttachmentEdgeDTO& rhs) {
            return lhs == rhs;
          },
          py::is_operator())
      .def(
          "__ne__",
          [](const ComponentAttachmentEdgeDTO& lhs, const ComponentAttachmentEdgeDTO& rhs) {
            return lhs != rhs;
          },
          py::is_operator());

  py::class_<CellGraphDTO>(module, "CellGraphDTO", py::is_final())
      .def(py::init([](std::string api_version, GraphRevision graph_revision, CellId cell_id,
                       std::vector<RuntimeUnitDTO> runtime_units,
                       std::vector<ModelInstanceDTO> model_instances,
                       std::vector<ControllableComponentDTO> controllable_components,
                       std::vector<ControlDomainDTO> control_domains,
                       std::vector<ControlBindingDTO> control_bindings,
                       std::vector<ComponentAttachmentEdgeDTO> component_attachments) {
             return CellGraphDTO{
                 std::move(api_version),
                 std::move(graph_revision),
                 std::move(cell_id),
                 std::move(runtime_units),
                 std::move(model_instances),
                 std::move(controllable_components),
                 std::move(control_domains),
                 std::move(control_bindings),
                 std::move(component_attachments),
             };
           }),
           py::arg("api_version"), py::arg("graph_revision"), py::arg("cell_id"),
           py::arg("runtime_units"), py::arg("model_instances"), py::arg("controllable_components"),
           py::arg("control_domains"), py::arg("control_bindings"),
           py::arg("component_attachments"))
      .def_readonly("api_version", &CellGraphDTO::api_version)
      .def_readonly("graph_revision", &CellGraphDTO::graph_revision)
      .def_readonly("cell_id", &CellGraphDTO::cell_id)
      .def_readonly("runtime_units", &CellGraphDTO::runtime_units)
      .def_readonly("model_instances", &CellGraphDTO::model_instances)
      .def_readonly("controllable_components", &CellGraphDTO::controllable_components)
      .def_readonly("control_domains", &CellGraphDTO::control_domains)
      .def_readonly("control_bindings", &CellGraphDTO::control_bindings)
      .def_readonly("component_attachments", &CellGraphDTO::component_attachments)
      .def(
          "__eq__", [](const CellGraphDTO& lhs, const CellGraphDTO& rhs) { return lhs == rhs; },
          py::is_operator())
      .def(
          "__ne__", [](const CellGraphDTO& lhs, const CellGraphDTO& rhs) { return lhs != rhs; },
          py::is_operator());
}

}  // namespace manyforge::core::python

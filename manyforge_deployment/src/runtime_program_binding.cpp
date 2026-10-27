#include <manyforge_deployment/runtime_program_binding.hpp>
#include <manyforge_deployment/io_signal_catalog.hpp>
#include <manyforge_core/io_signal_descriptor.hpp>

#include <algorithm>
#include <iterator>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string message) {
  throw RuntimeProgramBindingResolutionError(std::move(message));
}

template <typename Range, typename Predicate>
[[nodiscard]] const typename Range::value_type &
require_one(const Range &values, Predicate predicate,
            std::string_view description) {
  const auto first = std::find_if(values.begin(), values.end(), predicate);
  if (first == values.end()) {
    fail("runtime Program Binding is missing " + std::string(description));
  }
  if (std::find_if(std::next(first), values.end(), predicate) != values.end()) {
    fail("runtime Program Binding has ambiguous " + std::string(description));
  }
  return *first;
}

[[nodiscard]] const core::ControlDomainDTO &
require_control_domain(const core::CellGraphDTO &graph,
                       const core::ControlBindingDTO &binding) {
  const auto &domain = require_one(
      graph.control_domains,
      [&](const core::ControlDomainDTO &candidate) {
        return candidate.control_domain_id == binding.control_domain_id;
      },
      "control domain");
  if (domain.controller().controller_topology_id != binding.controller_topology_id ||
      domain.controller().topology_revision != binding.topology_revision) {
    fail("runtime Program Binding control domain and binding topology differ");
  }
  return domain;
}

[[nodiscard]] std::pair<const core::DesiredControlConfigurationDTO &,
                        const core::ControlRouteId &>
require_desired_route(const core::RetainedBundleDTO &bundle,
                      const core::ControlDomainDTO &domain,
                      const core::ControlBindingDTO &binding) {
  const auto &configuration = require_one(
      bundle.desired_control_configurations,
      [&](const core::DesiredControlConfigurationDTO &candidate) {
        return candidate.control_domain_id == domain.control_domain_id &&
               candidate.control_domain_revision ==
                   domain.control_domain_revision;
      },
      "desired control configuration");
  if (configuration.controller_topology_id != domain.controller().controller_topology_id ||
      configuration.topology_revision != domain.controller().topology_revision) {
    fail("runtime Program Binding desired configuration topology differs");
  }
  const auto &route = require_one(
      configuration.binding_routes,
      [&](const core::DesiredControlBindingRouteDTO &candidate) {
        return candidate.control_binding_id == binding.control_binding_id &&
               candidate.control_binding_revision ==
                   binding.control_binding_revision;
      },
      "desired control route");
  if (std::find(binding.legal_control_route_ids.begin(),
                binding.legal_control_route_ids.end(),
                route.control_route_id) ==
      binding.legal_control_route_ids.end()) {
    fail("runtime Program Binding desired route is not legal for its binding");
  }
  return {configuration, route.control_route_id};
}

} // namespace

RuntimeProgramTargetBinding resolve_runtime_program_target(
    const ProgramBinding &program_binding,
    const core::TargetRoleId &target_role_id,
    const core::CapabilityId &capability_id,
    const RetainedBundle &retained_bundle,
    const VerifiedRetainedModelMap &retained_model_map) {
  if (!verify_program_binding_identity(program_binding)) {
    fail("runtime Program Binding failed intrinsic identity verification");
  }
  if (!verify_retained_bundle_identity(retained_bundle)) {
    fail(
        "runtime Program Binding retained bundle failed identity verification");
  }

  const auto &binding_value = program_binding.value();
  const auto &bundle_value = retained_bundle.value();
  if (binding_value.retained_bundle_id !=
          retained_bundle.retained_bundle_id() ||
      binding_value.graph_revision != bundle_value.cell_graph.graph_revision ||
      retained_model_map.query().retained_bundle_id !=
          retained_bundle.retained_bundle_id()) {
    fail("runtime Program Binding context does not select one exact bundle");
  }

  const auto &target_role = require_one(
      binding_value.target_role_bindings,
      [&](const core::ProgramTargetRoleBindingDTO &candidate) {
        return candidate.target_role_id == target_role_id;
      },
      "target role");
  const auto &component_role = require_one(
      binding_value.component_role_bindings,
      [&](const core::ProgramComponentRoleBindingDTO &candidate) {
        return candidate.component_role_id == target_role.component_role_id;
      },
      "component role");
  if (component_role.controllable_component_id !=
      target_role.controllable_component_id) {
    fail("runtime Program Binding target and component roles disagree");
  }
  const auto &capability = require_one(
      target_role.capability_bindings,
      [&](const core::ProgramCapabilityBindingDTO &candidate) {
        return candidate.capability_id == capability_id;
      },
      "target capability");

  const auto &component = require_one(
      bundle_value.cell_graph.controllable_components,
      [&](const core::ControllableComponentDTO &candidate) {
        return candidate.controllable_component_id ==
               component_role.controllable_component_id;
      },
      "controllable component");
  if (component.component_type_id != component_role.component_type_id ||
      component.component_type_revision !=
          component_role.component_type_revision) {
    fail("runtime Program Binding component type differs from retained graph");
  }

  const auto *model_target = retained_model_map.find_target(
      target_role.controllable_component_id, target_role.command_target_id);
  if (model_target == nullptr) {
    fail("runtime Program Binding target has no exact retained model mapping");
  }

  std::optional<core::ControlBindingDTO> control_binding;
  std::optional<core::ControlDomainDTO> control_domain;
  std::optional<core::DesiredControlConfigurationDTO> desired_configuration;
  std::optional<core::ControlRouteId> desired_route;
  std::optional<CompositionControlBindingCoordinateMap> coordinate_map;
  if (capability.control_binding.has_value()) {
    const auto &reference = *capability.control_binding;
    const auto &graph_binding = require_one(
        bundle_value.cell_graph.control_bindings,
        [&](const core::ControlBindingDTO &candidate) {
          return candidate.control_binding_id == reference.control_binding_id &&
                 candidate.control_binding_revision ==
                     reference.control_binding_revision;
        },
        "control binding");
    if (graph_binding.controllable_component_id !=
            target_role.controllable_component_id ||
        graph_binding.command_target_id != target_role.command_target_id ||
        graph_binding.capability_id != capability.capability_id ||
        graph_binding.capability_definition_revision !=
            capability.capability_definition_revision) {
      fail("runtime Program Binding control binding scope differs");
    }
    const auto &domain =
        require_control_domain(bundle_value.cell_graph, graph_binding);
    const auto [configuration, route] =
        require_desired_route(bundle_value, domain, graph_binding);
    const auto *mapped = retained_model_map.find_binding(
        target_role.controllable_component_id, target_role.command_target_id,
        graph_binding.control_binding_id);
    if (mapped == nullptr ||
        mapped->control_binding_id != graph_binding.control_binding_id) {
      fail("runtime Program Binding control binding has no exact coordinate "
           "map");
    }
    control_binding = graph_binding;
    control_domain = domain;
    desired_configuration = configuration;
    desired_route = route;
    coordinate_map = *mapped;
  }

  std::optional<core::CapabilityProviderBindingDTO> provider_binding;
  if (capability.capability_provider_binding.has_value()) {
    const auto &reference = *capability.capability_provider_binding;
    const auto &provider = require_one(
        bundle_value.capability_provider_bindings,
        [&](const core::CapabilityProviderBindingDTO &candidate) {
          return candidate.capability_provider_binding_id ==
                     reference.capability_provider_binding_id &&
                 candidate.capability_provider_binding_revision ==
                     reference.capability_provider_binding_revision;
        },
        "capability-provider binding");
    if (provider.capability_id != capability.capability_id ||
        provider.capability_definition_revision !=
            capability.capability_definition_revision) {
      fail("runtime Program Binding provider capability differs");
    }
    provider_binding = provider;
  }

  return RuntimeProgramTargetBinding(
      binding_value.program_id, binding_value.program_revision,
      binding_value.program_binding_id, binding_value.retained_bundle_id,
      binding_value.graph_revision, component_role, target_role, capability,
      component, *model_target, std::move(control_binding),
      std::move(control_domain), std::move(desired_configuration),
      std::move(desired_route), std::move(coordinate_map),
      std::move(provider_binding), binding_value.contact_grasp_bindings);
}

core::RuntimeParticipantBindingDTO build_runtime_io_output_binding_dto(
    const RuntimeProgramTargetBinding &target,
    const core::ResourceRoleId &resource_role,
    const RetainedBundle &retained_bundle,
    const core::RuntimeProviderDomainConfigurationReadbackDTO
        &actual_provider) {
  const auto &graph = retained_bundle.value().cell_graph;
  const auto *domain = actual_provider.domain.provider();
  const auto schema = core::builtin_io_signal_write_command_schema();
  if (!verify_retained_bundle_identity(retained_bundle) ||
      !actual_provider.locally_valid() || !domain || resource_role.empty() ||
      target.control_binding() ||
      target.capability().capability_id.value() != "io.gpio.set_output" ||
      target.capability().command_schema !=
          core::ProgramCommandSchemaRefDTO{schema.command_schema_id,
                                           schema.command_schema_revision} ||
      !target.capability_provider_binding() ||
      target.retained_bundle_id() != retained_bundle.retained_bundle_id() ||
      target.graph_revision() != graph.graph_revision ||
      actual_provider.retained_bundle_id != target.retained_bundle_id() ||
      actual_provider.graph_revision != target.graph_revision() ||
      domain->provider.controllable_component_id !=
          target.component().controllable_component_id)
    fail("runtime output has no exact retained target/provider occurrence");

  const auto &sealed_domain = require_one(
      graph.control_domains,
      [&](const auto &value) {
        return value.control_domain_id ==
                   actual_provider.domain.control_domain_id &&
               value.control_domain_revision ==
                   actual_provider.domain.control_domain_revision;
      },
      "output provider domain");
  if (!sealed_domain.provider() ||
      *sealed_domain.provider() != domain->provider)
    fail("runtime output provider differs from its sealed domain");
  const auto &provider = *target.capability_provider_binding();
  const auto *provider_target =
      std::get_if<core::ComponentTargetCapabilityProviderTargetDTO>(
          &provider.target);
  const auto &occurrence = actual_provider.provider_occurrence;
  if (!provider_target ||
      provider_target->controllable_component_id !=
          target.component().controllable_component_id ||
      provider_target->component_type_revision !=
          target.component().component_type_revision ||
      provider_target->command_target_id !=
          target.target_role().command_target_id ||
      provider.capability_id != target.capability().capability_id ||
      provider.capability_definition_revision !=
          target.capability().capability_definition_revision ||
      provider.provider_instance_id != occurrence.provider_instance_id ||
      provider.provider_manifest_id != occurrence.provider_manifest_id ||
      provider.provider_manifest_revision !=
          occurrence.provider_manifest_revision)
    fail("runtime output capability belongs to another provider occurrence");

  const auto catalog = ResolveRetainedIoSignalCatalog(graph);
  if (!catalog.ok())
    fail("runtime output signal catalog is not qualified: " +
         catalog.error().message);
  std::vector<std::string> member_names;
  std::vector<core::ResourceId> output_resources;
  for (const auto &member : catalog.value()) {
    if (member.descriptor.signal_id.controllable_component_id !=
            target.component().controllable_component_id ||
        member.runtime_unit_id != occurrence.provider_runtime_unit_id ||
        !std::binary_search(domain->provider.signal_names.begin(),
                            domain->provider.signal_names.end(),
                            member.descriptor.signal_id.name))
      continue;
    member_names.push_back(member.descriptor.signal_id.name);
    if (member.descriptor.output_resource)
      output_resources.push_back(*member.descriptor.output_resource);
  }
  std::sort(member_names.begin(), member_names.end());
  std::sort(output_resources.begin(), output_resources.end());
  output_resources.erase(
      std::unique(output_resources.begin(), output_resources.end()),
      output_resources.end());
  if (member_names != domain->provider.signal_names ||
      output_resources != domain->output_resources ||
      std::find(target.component().runtime_unit_ids.begin(),
                target.component().runtime_unit_ids.end(),
                occurrence.provider_runtime_unit_id) ==
          target.component().runtime_unit_ids.end())
    fail("runtime output provider membership differs from the complete "
         "retained catalog");
  const auto &row = require_one(
      catalog.value(),
      [&](const auto &value) {
        return value.descriptor.signal_id.controllable_component_id ==
                   target.component().controllable_component_id &&
               value.command_target_id ==
                   target.target_role().command_target_id &&
               value.resource_role_id == resource_role;
      },
      "output signal resource");
  const auto digest = core::ComputeIoSignalDescriptorDigest(row.descriptor);
  const auto &role = require_one(
      target.target_role().signal_role_bindings,
      [&](const auto &value) {
        return value.resource_role_id == resource_role;
      },
      "output signal role");
  if (row.descriptor.direction != core::IoSignalDirection::kOutput ||
      row.descriptor.value_type != core::IoSignalValueType::kBoolean ||
      !row.descriptor.output_resource ||
      row.runtime_unit_id != occurrence.provider_runtime_unit_id ||
      !digest.ok() || role.signal_descriptor_digest != digest.value() ||
      role.signal_name != row.descriptor.signal_id.name ||
      role.direction != row.descriptor.direction ||
      role.value_type != row.descriptor.value_type ||
      role.transition_capability != row.descriptor.transition_capability ||
      !std::binary_search(domain->provider.signal_names.begin(),
                          domain->provider.signal_names.end(),
                          row.descriptor.signal_id.name) ||
      !std::binary_search(domain->output_resources.begin(),
                          domain->output_resources.end(),
                          *row.descriptor.output_resource))
    fail("runtime output role, descriptor or actual channel differs from its "
         "seal");

  core::IoOutputCompletionPolicyDTO policy(core::IoOutputImmediateDTO{});
  if (row.provider.profile == "ros_set_bool" || row.provider.profile == "ur_set_io" ||
      row.provider.profile == "xarm_digital") {
    const auto devices = ResolveRetainedIoProviderDevices(graph);
    if (!devices.ok())
      fail("runtime output device policy is not qualified: " +
           devices.error().message);
    const auto &device = require_one(
        devices.value(),
        [&](const auto &value) {
          return value.runtime_unit_id == row.runtime_unit_id &&
                 value.controllable_component_id ==
                     row.descriptor.signal_id.controllable_component_id;
        },
        "output mechanism device policy");
    if (!device.output_source_ack_timeout_ns ||
        *device.output_source_ack_timeout_ns <= 0)
      fail("runtime output mechanism has no retained source-acknowledgement "
           "budget");
    policy = core::IoOutputCompletionPolicyDTO(
        core::IoOutputSourceAckBudgetDTO{*device.output_source_ack_timeout_ns});
  } else if (row.provider.profile == "ros2_control_gpio") {
    policy = core::IoOutputCompletionPolicyDTO(core::IoOutputSubmittedDTO{});
  } else if (row.provider.profile != "mock_output") {
    fail("runtime output mechanism is unsupported");
  }

  return core::RuntimeParticipantBindingDTO{
      target.program_id(),
      target.program_revision(),
      target.program_binding_id(),
      target.retained_bundle_id(),
      target.graph_revision(),
      target.component_role().component_role_id,
      target.target_role().target_role_id,
      target.component().controllable_component_id,
      target.component().component_type_id,
      target.component().component_type_revision,
      target.target_role().command_target_id,
      target.capability().capability_id,
      target.capability().capability_definition_revision,
      target.capability().command_schema,
      {},
      {},
      {},
      {},
      {},
      std::nullopt,
      core::RuntimeCapabilityProviderPathDTO{
          provider.capability_id, provider.capability_definition_revision,
          provider.capability_provider_binding_id,
          provider.capability_provider_binding_revision,
          occurrence.provider_instance_id, occurrence.provider_manifest_id,
          occurrence.provider_manifest_revision,
          occurrence.provider_runtime_unit_id,
          occurrence.provider_runtime_epoch},
      {},
      {{occurrence.provider_runtime_unit_id,
        occurrence.provider_runtime_epoch}},
      {},
      core::RuntimeIoOutputBindingDTO{
          resource_role, row.descriptor, digest.value(),
          actual_provider.component_lifecycle_generation, occurrence,
          domain->provider.signal_configuration_digest, std::move(policy)}};
}

core::RuntimeParticipantBindingDTO build_runtime_participant_binding_dto(
    const RuntimeProgramTargetBinding &target,
    const ActiveRuntimeParticipantFactsDTO &active_facts,
    std::span<const RuntimeProgramTargetBinding> supporting_capabilities) {
  if (active_facts.control_route_id.empty() ||
      active_facts.active_control_configuration_id.empty() ||
      !active_facts.active_control_configuration_generation.nonzero()) {
    fail("runtime participant active control facts are incomplete");
  }

  const auto find_runtime_epoch =
      [&](const core::RuntimeUnitId &id) -> const core::RuntimeEpoch & {
    const auto &fact = require_one(
        active_facts.runtime_unit_epochs,
        [&](const core::RuntimeUnitEpochBindingDTO &candidate) {
          return candidate.runtime_unit_id == id;
        },
        "runtime-unit epoch");
    if (fact.runtime_epoch.empty()) {
      fail("runtime participant has an empty runtime-unit epoch");
    }
    return fact.runtime_epoch;
  };
  const auto find_provider_epoch =
      [&](const core::ProviderInstanceId &id,
          const core::RuntimeUnitId &runtime_unit_id)
      -> const core::RuntimeEpoch & {
    const auto &fact = require_one(
        active_facts.provider_epochs,
        [&](const RuntimeProviderEpochFactDTO &candidate) {
          return candidate.provider_instance_id == id;
        },
        "provider runtime epoch");
    if (fact.provider_runtime_unit_id != runtime_unit_id ||
        fact.runtime_epoch.empty()) {
      fail("runtime participant provider occurrence differs from its retained "
           "runtime unit");
    }
    return fact.runtime_epoch;
  };
  const auto find_provider_occurrence = [&](const core::ProviderInstanceId &id)
      -> const RuntimeProviderEpochFactDTO & {
    const auto &fact = require_one(
        active_facts.provider_epochs,
        [&](const RuntimeProviderEpochFactDTO &candidate) {
          return candidate.provider_instance_id == id;
        },
        "provider runtime occurrence");
    if (fact.provider_runtime_unit_id.empty() || fact.runtime_epoch.empty()) {
      fail("runtime participant has an incomplete provider runtime occurrence");
    }
    return fact;
  };

  std::vector<core::RuntimeModelGroupBindingDTO> model_groups;
  model_groups.reserve(target.model_target().model_groups.size());
  for (const auto &group : target.model_target().model_groups) {
    model_groups.push_back(core::RuntimeModelGroupBindingDTO{
        group.model_group_role_id, group.model_instance_id,
        group.compiled_group_name, group.expanded_compiled_joint_names});
  }

  std::optional<core::RuntimeControlPathDTO> control_path;
  std::vector<core::RuntimeResourceBindingDTO> resources;
  std::set<core::RuntimeUnitId> required_runtime_units;
  for (const auto &id : target.component().runtime_unit_ids) {
    required_runtime_units.insert(id);
  }
  if (target.control_binding().has_value()) {
    if (!target.control_domain().has_value() ||
        !target.desired_control_configuration().has_value() ||
        !target.desired_route().has_value() ||
        !target.coordinate_map().has_value()) {
      fail("runtime participant control path was not completely resolved");
    }
    const auto &binding = *target.control_binding();
    const auto &domain = *target.control_domain();
    const auto &desired = *target.desired_control_configuration();
    if (active_facts.control_route_id != *target.desired_route()) {
      fail("I6 active route differs from the retained desired route");
    }
    for (const auto &id : domain.runtime_unit_ids) {
      required_runtime_units.insert(id);
    }

    const auto &coordinate_map = *target.coordinate_map();
    resources.reserve(binding.resource_bindings.size());
    for (const auto &resource_binding : binding.resource_bindings) {
      const auto &coordinate = require_one(
          coordinate_map.resources,
          [&](const CompositionResourceCoordinateRow &candidate) {
            return candidate.resource_id == resource_binding.resource_id;
          },
          "resource coordinate");
      if (coordinate.resource_role_id != resource_binding.resource_role_id) {
        fail("runtime resource is not one exact direct coordinate binding");
      }
      const auto &program_resource = require_one(
          target.target_role().resource_role_bindings,
          [&](const core::ProgramResourceRoleBindingDTO &candidate) {
            return candidate.resource_role_id ==
                   resource_binding.resource_role_id;
          },
          "Program resource role");
      if (std::find(program_resource.resource_ids.begin(),
                    program_resource.resource_ids.end(),
                    resource_binding.resource_id) ==
          program_resource.resource_ids.end()) {
        fail("runtime control resource is absent from the Program Binding");
      }
      if (coordinate.coordinates.empty()) {
        // Observation bindings use the same retained coordinate without a
        // command schema. Resolving that resource does not grant command
        // authority; the exact capability/schema remain part of the DTO.
        if (target.capability().command_schema.has_value() &&
            target.capability().command_schema->command_schema_id.value() !=
                core::kGripperPositionCommandSchemaId) {
          fail("component-local runtime resource has no exact scalar command "
               "schema");
        }
        if (!coordinate.component_local_coordinate_kind.has_value()) {
          fail("component-local runtime resource has no exact coordinate "
               "kind");
        }
        resources.push_back(core::RuntimeResourceBindingDTO{
            resource_binding.resource_role_id, resource_binding.resource_id,
            std::nullopt, resource_binding.resource_id.value(),
            *coordinate.component_local_coordinate_kind});
        continue;
      }
      if (coordinate.component_local_coordinate_kind.has_value()) {
        fail("model-backed runtime resource carries component-local "
             "coordinate semantics");
      }
      if (coordinate.coordinates.front().derived_mimic ||
          coordinate.coordinates.front().compiled_joint_name.empty()) {
        fail("runtime resource is not one exact direct coordinate binding");
      }
      const auto &projection = coordinate.coordinates.front();
      core::CoordinateKind kind{};
      if (projection.coordinate_kind == UrdfCoordinateKind::kAngular &&
          projection.unit == CompositionCoordinateUnit::kRadians) {
        kind = core::CoordinateKind::kAngular;
      } else if (projection.coordinate_kind == UrdfCoordinateKind::kLinear &&
                 projection.unit == CompositionCoordinateUnit::kMeters) {
        kind = core::CoordinateKind::kLinear;
      } else {
        fail("runtime resource has an unsupported coordinate kind or unit");
      }
      resources.push_back(core::RuntimeResourceBindingDTO{
          resource_binding.resource_role_id, resource_binding.resource_id,
          projection.source_joint_name, projection.compiled_joint_name, kind});
    }
    std::optional<core::RuntimeScaleRouteQualificationDTO>
        runtime_scale_qualification;
    const auto qualified_scale = std::find_if(
        binding.runtime_scale_route_qualifications.begin(),
        binding.runtime_scale_route_qualifications.end(),
        [&](const auto &candidate) {
          return candidate.control_route_id == active_facts.control_route_id;
        });
    if (qualified_scale != binding.runtime_scale_route_qualifications.end()) {
      runtime_scale_qualification = *qualified_scale;
    }
    control_path = core::RuntimeControlPathDTO{
        domain.control_domain_id,
        domain.control_domain_revision,
        binding.control_binding_id,
        binding.control_binding_revision,
        binding.control_interface_contract_id,
        binding.control_interface_revision,
        binding.controller_topology_id,
        binding.topology_revision,
        active_facts.control_route_id,
        desired.activation_set_id,
        active_facts.active_control_configuration_id,
        active_facts.active_control_configuration_generation,
        binding.provider_instance_id,
        binding.provider_manifest_id,
        binding.provider_manifest_revision,
        binding.provider_runtime_unit_id,
        find_provider_epoch(binding.provider_instance_id,
                            binding.provider_runtime_unit_id),
        binding.state_authority_id,
        binding.state_authority_class,
        binding.stop_evidence_policy_id,
        binding.stop_evidence_policy_revision,
        binding.containment_granularity,
        std::move(runtime_scale_qualification)};
  }

  std::optional<core::RuntimeCapabilityProviderPathDTO> provider_path;
  if (target.capability_provider_binding().has_value()) {
    const auto &provider = *target.capability_provider_binding();
    const auto &occurrence =
        find_provider_occurrence(provider.provider_instance_id);
    required_runtime_units.insert(occurrence.provider_runtime_unit_id);
    provider_path = core::RuntimeCapabilityProviderPathDTO{
        target.capability().capability_id,
        target.capability().capability_definition_revision,
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        provider.provider_instance_id,
        provider.provider_manifest_id,
        provider.provider_manifest_revision,
        occurrence.provider_runtime_unit_id,
        occurrence.runtime_epoch};
  }

  std::vector<core::RuntimeCapabilityProviderPathDTO> supporting_paths;
  supporting_paths.reserve(supporting_capabilities.size());
  for (const auto &supporting : supporting_capabilities) {
    if (supporting.program_id() != target.program_id() ||
        supporting.program_revision() != target.program_revision() ||
        supporting.program_binding_id() != target.program_binding_id() ||
        supporting.retained_bundle_id() != target.retained_bundle_id() ||
        supporting.graph_revision() != target.graph_revision() ||
        supporting.component_role().component_role_id !=
            target.component_role().component_role_id ||
        supporting.target_role().target_role_id !=
            target.target_role().target_role_id ||
        supporting.component().controllable_component_id !=
            target.component().controllable_component_id ||
        supporting.model_target().command_target_id !=
            target.model_target().command_target_id ||
        supporting.control_binding().has_value() ||
        !supporting.capability_provider_binding().has_value()) {
      fail("supporting runtime capability crosses its exact Program target "
           "or is not provider-backed");
    }
    const auto &capability = supporting.capability();
    const auto &provider = *supporting.capability_provider_binding();
    const auto &occurrence =
        find_provider_occurrence(provider.provider_instance_id);
    required_runtime_units.insert(occurrence.provider_runtime_unit_id);
    const bool exact_support_scope = std::visit(
        [&](const auto &provider_target) {
          using Target = std::remove_cvref_t<decltype(provider_target)>;
          if constexpr (std::is_same_v<
                            Target,
                            core::TargetBindingCapabilityProviderTargetDTO>) {
            return target.control_binding().has_value() &&
                   provider_target.control_binding_id ==
                       target.control_binding()->control_binding_id &&
                   provider_target.control_binding_revision ==
                       target.control_binding()->control_binding_revision;
          } else if constexpr (std::is_same_v<Target, core::ComponentTargetCapabilityProviderTargetDTO>) {
            // A non-coordinate observation provider is never a supporting
            // planning/control path for physical operation dispatch.
            return false;
          } else if constexpr (
              std::is_same_v<Target,
                             core::ControlDomainCapabilityProviderTargetDTO>) {
            return target.control_domain().has_value() &&
                   provider_target.control_domain_id ==
                       target.control_domain()->control_domain_id &&
                   provider_target.control_domain_revision ==
                       target.control_domain()->control_domain_revision;
          } else {
            return true;
          }
        },
        provider.target);
    if (!exact_support_scope) {
      fail("supporting runtime capability is not anchored to the selected "
           "control path");
    }
    supporting_paths.push_back(core::RuntimeCapabilityProviderPathDTO{
        capability.capability_id, capability.capability_definition_revision,
        provider.capability_provider_binding_id,
        provider.capability_provider_binding_revision,
        provider.provider_instance_id, provider.provider_manifest_id,
        provider.provider_manifest_revision,
        occurrence.provider_runtime_unit_id, occurrence.runtime_epoch});
  }
  std::sort(
      supporting_paths.begin(), supporting_paths.end(),
      [](const auto &lhs, const auto &rhs) {
        return std::tie(lhs.capability_id, lhs.capability_definition_revision) <
               std::tie(rhs.capability_id, rhs.capability_definition_revision);
      });
  if (std::adjacent_find(supporting_paths.begin(), supporting_paths.end(),
                         [](const auto &lhs, const auto &rhs) {
                           return lhs.capability_id == rhs.capability_id &&
                                  lhs.capability_definition_revision ==
                                      rhs.capability_definition_revision;
                         }) != supporting_paths.end()) {
    fail("supporting runtime capabilities repeat one exact capability");
  }

  std::vector<core::RuntimeUnitEpochBindingDTO> runtime_epochs;
  runtime_epochs.reserve(required_runtime_units.size());
  for (const auto &id : required_runtime_units) {
    runtime_epochs.push_back(
        core::RuntimeUnitEpochBindingDTO{id, find_runtime_epoch(id)});
  }

  return core::RuntimeParticipantBindingDTO{
      target.program_id(),
      target.program_revision(),
      target.program_binding_id(),
      target.retained_bundle_id(),
      target.graph_revision(),
      target.component_role().component_role_id,
      target.target_role().target_role_id,
      target.component().controllable_component_id,
      target.component().component_type_id,
      target.component().component_type_revision,
      target.target_role().command_target_id,
      target.capability().capability_id,
      target.capability().capability_definition_revision,
      target.capability().command_schema,
      target.component_role().model_role_bindings,
      std::move(model_groups),
      target.target_role().frame_role_bindings,
      std::move(resources),
      target.target_role().collision_body_role_bindings,
      std::move(control_path),
      std::move(provider_path),
      std::move(supporting_paths),
      std::move(runtime_epochs),
      target.contact_grasp_bindings()};
}

} // namespace manyforge::deployment

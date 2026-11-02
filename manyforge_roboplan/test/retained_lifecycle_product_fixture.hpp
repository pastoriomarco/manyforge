#pragma once

#include "lite6_retained_consumer_fixture.hpp"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace manyforge::roboplan_backend::product_test {

class PackageRoot final {
public:
  PackageRoot()
      : path_(std::filesystem::temp_directory_path() /
              ("manyforge-product-health-" + std::to_string(::getpid()) + "-" +
               std::to_string(sequence_.fetch_add(1)))) {
    if (!std::filesystem::create_directory(path_))
      throw std::runtime_error("cannot create retained product test package");
  }
  ~PackageRoot() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }
  const std::filesystem::path &path() const { return path_; }

private:
  inline static std::atomic<unsigned> sequence_{0};
  std::filesystem::path path_;
};

// This test path binds one actual qualified arm domain from the retained
// fixture. It does not rewrite a sealed provider occurrence or active route.
inline core::RuntimeParticipantBinding
ArmBinding(const deployment::RetainedBundle &bundle,
           const std::filesystem::path &package_root) {
  const auto &graph = bundle.value().cell_graph;
  if (graph.control_bindings.size() != 1 || graph.control_domains.size() != 1 ||
      graph.controllable_components.size() != 1 ||
      bundle.value().desired_control_configurations.size() != 1)
    throw std::runtime_error("product fixture must own one arm domain");
  const auto &binding = graph.control_bindings.front();
  const auto &component = graph.controllable_components.front();
  const auto &domain = graph.control_domains.front();
  const auto &desired = bundle.value().desired_control_configurations.front();
  if (desired.binding_routes.size() != 1 || graph.model_instances.size() != 1)
    throw std::runtime_error("product fixture must own one route and model");
  const auto &route = desired.binding_routes.front();
  const auto &model = graph.model_instances.front();
  const auto &maps = bundle.value().compiled_products.symbol_role_maps;
  if (maps.size() != 1)
    throw std::runtime_error("product fixture must own one model map");
  const deployment::FilesystemVerifiedRetainedModelMapSource map_source(
      package_root);
  const auto map = map_source.get_verified(
      {bundle.retained_bundle_id(), maps.front().product_id});
  if (!map)
    throw std::runtime_error("product fixture model map unreadable");
  const auto *target = map->find_target(component.controllable_component_id,
                                        binding.command_target_id);
  const auto *coordinates =
      map->find_binding(component.controllable_component_id,
                        binding.command_target_id, binding.control_binding_id);
  if (target == nullptr || coordinates == nullptr)
    throw std::runtime_error("product fixture model target missing");
  std::vector<core::RuntimeModelGroupBindingDTO> model_groups;
  for (const auto &group : target->model_groups)
    model_groups.push_back({group.model_group_role_id, group.model_instance_id,
                            group.compiled_group_name,
                            group.expanded_compiled_joint_names});
  std::vector<core::ProgramFrameRoleBindingDTO> frames;
  for (const auto &resolution : target->frames) {
    const auto *frame = std::get_if<deployment::CompositionComponentModelFrame>(
        &resolution.frame);
    if (frame == nullptr ||
        frame->controllable_component_id !=
            component.controllable_component_id ||
        frame->frame_role_id != resolution.frame_role_id)
      throw std::runtime_error("product fixture frame ownership differs");
    frames.push_back({resolution.frame_role_id, frame->model_instance_id,
                      frame->model_revision, frame->compiled_frame_name});
  }
  const core::RuntimeEpoch epoch("product.test.epoch");
  std::vector<core::RuntimeResourceBindingDTO> resources;
  for (const auto &resource : binding.resource_bindings) {
    const auto row =
        std::find_if(coordinates->resources.begin(),
                     coordinates->resources.end(), [&](const auto &candidate) {
                       return candidate.resource_id == resource.resource_id;
                     });
    if (row == coordinates->resources.end() ||
        row->resource_role_id != resource.resource_role_id ||
        row->coordinates.empty() || row->coordinates.front().derived_mimic)
      throw std::runtime_error("product fixture direct resource differs");
    const auto &coordinate = row->coordinates.front();
    resources.push_back(
        {resource.resource_role_id, resource.resource_id,
         coordinate.source_joint_name, coordinate.compiled_joint_name,
         coordinate.coordinate_kind == deployment::UrdfCoordinateKind::kAngular
             ? core::CoordinateKind::kAngular
             : core::CoordinateKind::kLinear});
  }
  auto sealed = core::verify_runtime_participant_binding(
      {core::ProgramId("product.test.program"),
       core::ProgramRevision("product.test.revision"),
       core::ProgramBindingId("product.test.binding"),
       bundle.retained_bundle_id(),
       graph.graph_revision,
       core::ComponentRoleId("robot"),
       core::TargetRoleId("arm"),
       component.controllable_component_id,
       component.component_type_id,
       component.component_type_revision,
       binding.command_target_id,
       binding.capability_id,
       binding.capability_definition_revision,
       std::nullopt,
       {{component.model_role_bindings.front().model_role_id,
         model.model_instance_id, model.model_revision}},
       std::move(model_groups),
       std::move(frames),
       std::move(resources),
       {},
       core::RuntimeControlPathDTO{
           domain.control_domain_id,
           domain.control_domain_revision,
           binding.control_binding_id,
           binding.control_binding_revision,
           binding.control_interface_contract_id,
           binding.control_interface_revision,
           binding.controller_topology_id,
           binding.topology_revision,
           route.control_route_id,
           desired.activation_set_id,
           core::ActiveControlConfigurationId("product.test.active"),
           core::ActiveControlConfigurationGeneration(1),
           binding.provider_instance_id,
           binding.provider_manifest_id,
           binding.provider_manifest_revision,
           binding.provider_runtime_unit_id,
           epoch,
           binding.state_authority_id,
           binding.state_authority_class,
           binding.stop_evidence_policy_id,
           binding.stop_evidence_policy_revision,
           binding.containment_granularity,
           std::nullopt},
       std::nullopt,
       {},
       {{binding.provider_runtime_unit_id, epoch}},
       {}});
  if (!sealed.ok())
    throw std::runtime_error(sealed.error().message);
  return std::move(sealed.value());
}

} // namespace manyforge::roboplan_backend::product_test

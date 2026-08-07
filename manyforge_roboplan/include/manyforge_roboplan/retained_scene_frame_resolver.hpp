#pragma once

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace manyforge::roboplan_backend {

struct RetainedTargetSceneFrameInput final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  core::ResolvedComponentModelFrameDTO frame;
};

struct RetainedSceneFrameStateSourceInput final {
  core::ObservedStateQueryContextDTO context;
  core::ObservedStateSourceKind publication_source{core::ObservedStateSourceKind::kPredictedState};
  std::shared_ptr<const core::IObservedStateProvider> provider;
  std::string local_source_id;
};

struct RetainedSceneFrameComponentAttachmentInput final {
  core::ComponentAttachmentId component_attachment_id;
  core::ComponentAttachmentRevision component_attachment_revision;
};

/// Construction policy for state owned by the selected local runtime unit.
///
/// A command-capable runtime must use kRequireDirectProviders and register the
/// exact existing owner publications. Explicit non-local providers use the
/// same port and retain their source runtime epoch, which preserves the c2
/// external receiver seam without adding transport here. The neutral bootstrap
/// is reserved for the Manager child while its command surface is explicitly
/// fenced closed; it must not silently survive activation of a command-capable
/// composition.
enum class RetainedSceneFrameLocalStateMode {
  kRequireDirectProviders,
  kCommandFencedNeutralBootstrap,
};

struct RetainedSceneFrameResolverInput final {
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::CellId cell_id;
  std::string compiled_cell_frame_name;
  std::vector<core::KinematicFramePathDTO> frame_paths;
  std::vector<RetainedTargetSceneFrameInput> target_frames;
  std::vector<RetainedSceneFrameComponentAttachmentInput> component_attachments;
  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  std::int64_t maximum_receiver_time_skew_ns{10'000'000};
  std::int64_t initialization_time_ns{};
};

/// Exact retained-graph scene-frame resolver. It builds c1 immutable state
/// views from direct local providers or the unchanged c2 external provider
/// seam, then evaluates FK against fresh model scratch only.
class RetainedSceneFrameResolver final : public core::IReferencePoseResolver {
 public:
  [[nodiscard]] static std::shared_ptr<RetainedSceneFrameResolver> create(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      RetainedSceneFrameResolverInput input);

  template <typename RetainedBundle, typename VerifiedRetainedModelMap,
            typename VerifiedRetainedPlanningModel>
  [[nodiscard]] static std::shared_ptr<RetainedSceneFrameResolver> create_verified(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      const RetainedBundle& retained_bundle, const VerifiedRetainedModelMap& model_map,
      const VerifiedRetainedPlanningModel& planning_model,
      const core::RuntimeUnitId& local_runtime_unit_id,
      const core::RuntimeEpoch& local_runtime_epoch,
      const RetainedSceneFrameLocalStateMode local_state_mode,
      std::vector<RetainedSceneFrameStateSourceInput> provided_state_sources,
      const std::int64_t initialization_time_ns) {
    if (!verify_retained_bundle_identity(retained_bundle)) {
      throw std::invalid_argument("scene-frame resolver received an unverifiable retained bundle");
    }
    model_map.require_same_bundle_as(planning_model);
    if (model_map.query().retained_bundle_id != retained_bundle.retained_bundle_id() ||
        planning_model.query().retained_bundle_id != retained_bundle.retained_bundle_id() ||
        local_runtime_unit_id.empty() || local_runtime_epoch.empty() ||
        initialization_time_ns < 0) {
      throw std::invalid_argument("scene-frame resolver crosses retained or runtime identity");
    }

    RetainedSceneFrameResolverInput input{retained_bundle.retained_bundle_id(),
                                          retained_bundle.value().cell_graph.graph_revision,
                                          model_map.cell_frame().cell_id,
                                          model_map.cell_frame().compiled_frame_name,
                                          model_map.kinematic_frame_paths(),
                                          {},
                                          {},
                                          {},
                                          10'000'000,
                                          initialization_time_ns};

    for (const auto& attachment : retained_bundle.value().cell_graph.component_attachments) {
      input.component_attachments.push_back(
          {attachment.component_attachment_id, attachment.component_attachment_revision});
    }

    for (const auto& target : model_map.targets()) {
      for (const auto& target_frame : target.frames) {
        std::visit(
            [&](const auto& frame) {
              if constexpr (requires {
                              frame.controllable_component_id;
                              frame.model_instance_id;
                              frame.frame_role_id;
                              frame.compiled_frame_name;
                              frame.model_revision;
                            }) {
                input.target_frames.push_back(
                    {target.controllable_component_id, target.command_target_id,
                     core::ResolvedComponentModelFrameDTO{
                         frame.controllable_component_id, frame.model_instance_id,
                         frame.frame_role_id, frame.compiled_frame_name, frame.model_revision}});
              }
            },
            target_frame.frame);
      }
    }

    const auto& value = retained_bundle.value();
    const auto& graph = value.cell_graph;
    for (const auto& coordinate_map : model_map.control_bindings()) {
      const auto graph_binding = std::find_if(
          graph.control_bindings.begin(), graph.control_bindings.end(), [&](const auto& candidate) {
            return candidate.control_binding_id == coordinate_map.control_binding_id &&
                   candidate.controllable_component_id ==
                       coordinate_map.controllable_component_id &&
                   candidate.command_target_id == coordinate_map.command_target_id;
          });
      if (graph_binding == graph.control_bindings.end()) {
        throw std::invalid_argument("scene-frame state map has no exact graph control binding");
      }
      const auto component = std::find_if(
          graph.controllable_components.begin(), graph.controllable_components.end(),
          [&](const auto& candidate) {
            return candidate.controllable_component_id == graph_binding->controllable_component_id;
          });
      const auto domain = std::find_if(
          graph.control_domains.begin(), graph.control_domains.end(), [&](const auto& candidate) {
            return candidate.control_domain_id == graph_binding->control_domain_id;
          });
      if (component == graph.controllable_components.end() ||
          domain == graph.control_domains.end()) {
        throw std::invalid_argument("scene-frame state binding has no exact component/domain");
      }
      const bool local =
          std::find(component->runtime_unit_ids.begin(), component->runtime_unit_ids.end(),
                    local_runtime_unit_id) != component->runtime_unit_ids.end() ||
          std::find(domain->runtime_unit_ids.begin(), domain->runtime_unit_ids.end(),
                    local_runtime_unit_id) != domain->runtime_unit_ids.end();
      const auto desired = std::find_if(
          value.desired_control_configurations.begin(), value.desired_control_configurations.end(),
          [&](const auto& candidate) {
            return candidate.control_domain_id == domain->control_domain_id &&
                   candidate.control_domain_revision == domain->control_domain_revision;
          });
      if (desired == value.desired_control_configurations.end() ||
          desired->controller_topology_id != graph_binding->controller_topology_id ||
          desired->topology_revision != graph_binding->topology_revision) {
        throw std::invalid_argument("scene-frame state has no exact desired control configuration");
      }
      const auto route = std::find_if(
          desired->binding_routes.begin(), desired->binding_routes.end(),
          [&](const auto& candidate) {
            return candidate.control_binding_id == graph_binding->control_binding_id &&
                   candidate.control_binding_revision == graph_binding->control_binding_revision;
          });
      if (route == desired->binding_routes.end()) {
        throw std::invalid_argument("scene-frame state binding has no selected desired route");
      }

      core::ObservedStateQueryContextDTO context{
          graph_binding->controllable_component_id,
          graph_binding->command_target_id,
          graph_binding->capability_id,
          domain->control_domain_id,
          domain->control_domain_revision,
          graph_binding->control_binding_id,
          graph_binding->control_binding_revision,
          graph_binding->controller_topology_id,
          graph_binding->topology_revision,
          route->control_route_id,
          {},
          {},
          {},
          graph_binding->control_interface_contract_id,
          graph_binding->control_interface_revision,
          core::ActiveControlConfigurationId("active.i6.manager_native"),
          core::ActiveControlConfigurationGeneration(1U),
          graph_binding->provider_instance_id,
          graph_binding->provider_manifest_id,
          graph_binding->provider_manifest_revision,
          local_runtime_epoch,
          graph_binding->state_authority_id,
          graph_binding->state_authority_class,
          0};
      context.resources.reserve(coordinate_map.resources.size());
      context.joint_names.reserve(coordinate_map.resources.size());
      context.coordinate_kinds.reserve(coordinate_map.resources.size());
      for (const auto& resource : coordinate_map.resources) {
        if (resource.coordinates.empty() || resource.coordinates.front().derived_mimic) {
          throw std::invalid_argument("scene-frame state resource has no direct model coordinate");
        }
        const auto& coordinate = resource.coordinates.front();
        using Kind = std::remove_cvref_t<decltype(coordinate.coordinate_kind)>;
        using Unit = std::remove_cvref_t<decltype(coordinate.unit)>;
        std::optional<core::CoordinateKind> kind;
        if (coordinate.coordinate_kind == Kind::kAngular && coordinate.unit == Unit::kRadians) {
          kind = core::CoordinateKind::kAngular;
        } else if (coordinate.coordinate_kind == Kind::kLinear &&
                   coordinate.unit == Unit::kMeters) {
          kind = core::CoordinateKind::kLinear;
        }
        if (!kind.has_value()) {
          throw std::invalid_argument("scene-frame state coordinate has no canonical scalar unit");
        }
        context.resources.push_back(resource.resource_id);
        context.joint_names.push_back(coordinate.compiled_joint_name);
        context.coordinate_kinds.push_back(*kind);
      }

      const auto provided = std::find_if(
          provided_state_sources.begin(), provided_state_sources.end(), [&](const auto& candidate) {
            return candidate.context.control_binding_id == graph_binding->control_binding_id &&
                   candidate.context.control_binding_revision ==
                       graph_binding->control_binding_revision;
          });
      if (provided != provided_state_sources.end()) {
        const auto source_matches_authority = [](const core::StateAuthorityClass authority,
                                                 const core::ObservedStateSourceKind source) {
          switch (authority) {
            case core::StateAuthorityClass::kMeasured:
              return source == core::ObservedStateSourceKind::kActionFeedback ||
                     source == core::ObservedStateSourceKind::kControllerState ||
                     source == core::ObservedStateSourceKind::kJointState;
            case core::StateAuthorityClass::kCommanded:
              return source == core::ObservedStateSourceKind::kCommandedState;
            case core::StateAuthorityClass::kPredicted:
              return source == core::ObservedStateSourceKind::kPredictedState;
          }
          return false;
        };
        if (std::find_if(std::next(provided), provided_state_sources.end(),
                         [&](const auto& candidate) {
                           return candidate.context.control_binding_id ==
                                      graph_binding->control_binding_id &&
                                  candidate.context.control_binding_revision ==
                                      graph_binding->control_binding_revision;
                         }) != provided_state_sources.end() ||
            !provided->provider || !provided->local_source_id.empty() ||
            !source_matches_authority(graph_binding->state_authority_class,
                                      provided->publication_source) ||
            provided->context.controllable_component_id != context.controllable_component_id ||
            provided->context.command_target_id != context.command_target_id ||
            provided->context.capability_id != context.capability_id ||
            provided->context.control_domain_id != context.control_domain_id ||
            provided->context.control_domain_revision != context.control_domain_revision ||
            provided->context.controller_topology_id != context.controller_topology_id ||
            provided->context.topology_revision != context.topology_revision ||
            provided->context.control_route_id != context.control_route_id ||
            provided->context.resources != context.resources ||
            provided->context.joint_names != context.joint_names ||
            provided->context.coordinate_kinds != context.coordinate_kinds ||
            provided->context.control_interface_contract_id !=
                context.control_interface_contract_id ||
            provided->context.control_interface_revision != context.control_interface_revision ||
            provided->context.active_control_configuration_id.empty() ||
            !provided->context.active_control_configuration_generation.nonzero() ||
            provided->context.provider_instance_id != context.provider_instance_id ||
            provided->context.provider_manifest_id != context.provider_manifest_id ||
            provided->context.provider_manifest_revision != context.provider_manifest_revision ||
            provided->context.runtime_epoch.empty() ||
            (local && provided->context.runtime_epoch != local_runtime_epoch) ||
            provided->context.state_authority_id != context.state_authority_id ||
            provided->context.state_authority_class != context.state_authority_class ||
            provided->context.freshness_budget_ns < 0 ||
            (provided->context.state_authority_class == core::StateAuthorityClass::kMeasured &&
             provided->context.freshness_budget_ns <= 0)) {
          throw std::invalid_argument(
              "scene-frame state provider differs from its retained binding");
        }
        input.state_sources.push_back(std::move(*provided));
        provided_state_sources.erase(provided);
      } else {
        if (!local) {
          continue;
        }
        if (graph_binding->state_authority_class != core::StateAuthorityClass::kPredicted ||
            local_state_mode != RetainedSceneFrameLocalStateMode::kCommandFencedNeutralBootstrap) {
          throw std::invalid_argument("scene-frame direct local state provider is missing");
        }
        input.state_sources.push_back(
            {std::move(context), core::ObservedStateSourceKind::kPredictedState, nullptr,
             "manager-native-frame-state/" + local_runtime_unit_id.value() + "/" +
                 graph_binding->control_binding_id.value()});
      }
    }
    if (!provided_state_sources.empty()) {
      throw std::invalid_argument("scene-frame state provider is unused");
    }
    return create(std::move(shared_model), std::move(input));
  }

  ~RetainedSceneFrameResolver() override;

  [[nodiscard]] core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO& reference, const std::string& base_frame,
      const core::ProjectionLeaseBindingDTO& projection_binding) const override;

  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO> resolve_scene_frame(
      const core::SceneFrameRefDTO& reference,
      const core::SceneFrameResolutionContextDTO& context) const override;

  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO> Resolve(
      const core::SceneFrameResolutionQueryDTO& query,
      std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
      std::int64_t now_ns) const;

 private:
  class Impl;
  explicit RetainedSceneFrameResolver(std::shared_ptr<Impl> impl);
  std::shared_ptr<Impl> impl_;
};

}  // namespace manyforge::roboplan_backend

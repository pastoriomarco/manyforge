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
#include <variant>
#include <vector>

namespace manyforge::roboplan_backend {

/// The component that owns one model instance in the cell graph.
struct RetainedSceneFrameModelOwnerInput final {
  core::ModelInstanceId model_instance_id;
  core::ControllableComponentId controllable_component_id;
};

/// The retained frame a Program frame-role binding names. A frame of a tool
/// mounted on the bound robot keeps its own component (its model's owner);
/// the binding's component owns every other frame.
[[nodiscard]] core::ResolvedComponentModelFrameDTO program_binding_frame(
    const std::vector<RetainedSceneFrameModelOwnerInput> &model_owners,
    const core::ControllableComponentId &binding_component,
    const core::ProgramFrameRoleBindingDTO &frame);

/// LOADFIX6: one owner row per model that ends a component-model frame path
/// of the cell kinematic index, in path order; the owner is the index's
/// `owner_of` (the cell graph). Passive models have no row.
template <typename KinematicIndex>
[[nodiscard]] std::vector<RetainedSceneFrameModelOwnerInput>
retained_scene_frame_model_owners(const KinematicIndex &index) {
  std::vector<RetainedSceneFrameModelOwnerInput> owners;
  for (const auto &path : index.frame_paths()) {
    const auto *terminal =
        std::get_if<core::ResolvedComponentModelFrameDTO>(&path.terminal_frame);
    if (terminal == nullptr ||
        std::any_of(owners.begin(), owners.end(), [&](const auto &row) {
          return row.model_instance_id == terminal->model_instance_id;
        }))
      continue;
    if (const auto *owner = index.owner_of(terminal->model_instance_id))
      owners.push_back({terminal->model_instance_id, *owner});
  }
  return owners;
}

struct RetainedTargetSceneFrameInput final {
  core::ControllableComponentId controllable_component_id;
  core::CommandTargetId command_target_id;
  core::FrameRoleId target_frame_role_id;
  core::ResolvedComponentModelFrameDTO frame;
};

/// G6 [C-P6]: one mounted component and every component it is mounted on,
/// nearest first (the cell kinematic index's `ancestors_of`).
struct RetainedSceneFrameComponentMountInput final {
  core::ControllableComponentId controllable_component_id;
  std::vector<core::ControllableComponentId> mounted_on;
};

/// G6: one mount row per graph component that is mounted on another.
template <typename CellGraph, typename KinematicIndex>
[[nodiscard]] std::vector<RetainedSceneFrameComponentMountInput>
retained_scene_frame_component_mounts(const CellGraph &graph,
                                      const KinematicIndex &index) {
  std::vector<RetainedSceneFrameComponentMountInput> mounts;
  for (const auto &component : graph.controllable_components) {
    auto mounted_on = index.ancestors_of(component.controllable_component_id);
    if (!mounted_on.empty())
      mounts.push_back(
          {component.controllable_component_id, std::move(mounted_on)});
  }
  return mounts;
}

/// G6 [C-P6]: a component is local to a resolver when it has a live state
/// source there (`has_source`), or when it has none and anything it is
/// mounted on has one (a pneumatic tool on a local arm).
template <typename HasSource>
[[nodiscard]] bool retained_scene_frame_component_local(
    const std::vector<RetainedSceneFrameComponentMountInput> &mounts,
    const core::ControllableComponentId &component,
    const HasSource &has_source) {
  if (has_source(component))
    return true;
  const auto row =
      std::find_if(mounts.begin(), mounts.end(), [&](const auto &candidate) {
        return candidate.controllable_component_id == component;
      });
  return row != mounts.end() &&
         std::any_of(row->mounted_on.begin(), row->mounted_on.end(),
                     has_source);
}

enum class RetainedSceneFrameCoordinateAuthority {
  kRequiredPublication,
  // Host-selected strict pneumatic binding: no coordinate observation exists.
  kUnavailablePneumatic,
};

struct RetainedSceneFrameStateSourceInput final {
  core::ObservedStateQueryContextDTO context;
  core::ObservedStateSourceKind publication_source{
      core::ObservedStateSourceKind::kPredictedState};
  std::shared_ptr<const core::IObservedStateProvider> provider;
  RetainedSceneFrameCoordinateAuthority coordinate_authority{
      RetainedSceneFrameCoordinateAuthority::kRequiredPublication};
};

struct RetainedSceneFrameComponentAttachmentInput final {
  core::ComponentAttachmentId component_attachment_id;
  core::ComponentAttachmentRevision component_attachment_revision;
};

struct RetainedSceneFrameResolverInput final {
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::CellId cell_id;
  std::string compiled_cell_frame_name;
  std::vector<core::KinematicFramePathDTO> frame_paths;
  /// The owner of each model that ends a component-model frame path.
  std::vector<RetainedSceneFrameModelOwnerInput> model_owners;
  std::vector<RetainedTargetSceneFrameInput> target_frames;
  std::vector<RetainedSceneFrameComponentAttachmentInput> component_attachments;
  /// G6: what each mounted component is mounted on.
  std::vector<RetainedSceneFrameComponentMountInput> component_mounts;
  std::vector<RetainedSceneFrameStateSourceInput> state_sources;
  std::int64_t maximum_receiver_time_skew_ns{10'000'000};
};

/// One "express pose P, given in frame A, in frame B" query. Frames are
/// `kCellFrame`, robot-qualified `kFrameRole`, or `kSceneResource` references;
/// every robot frame must belong to a robot hosted by this cell resolver.
struct RetainedSceneFrameTransformQuery final {
  core::Pose3DTO pose_in_source_frame;
  core::ReferenceDTO source_frame;
  core::ReferenceDTO target_frame;
  core::ProjectionLeaseBindingDTO projection_binding;
};

/// Immutable transform answer evaluated from one state sample time.
struct RetainedSceneFrameTransform final {
  core::Pose3DTO pose_in_target_frame;
  core::Pose3DTO source_frame_in_cell;
  core::Pose3DTO target_frame_in_cell;
  core::ProjectionLeaseBindingDTO projection_binding;
  std::int64_t evaluated_at_ns{};
  /// Resolution evidence of every retained frame path crossed, sorted and
  /// unique. Empty when both frames are cell-fixed.
  std::vector<core::ContentDigest> resolution_evidence_digests;
};

/// Exact retained-graph scene-frame resolver. It builds c1 immutable state
/// views from direct local providers or the unchanged c2 external provider
/// seam, then evaluates FK against fresh model scratch only. A command-fenced
/// projection is staged without a resolver; it never fabricates a local state
/// publication and receives this resolver only from the active composition.
class RetainedSceneFrameResolver final : public core::IReferencePoseResolver {
public:
  [[nodiscard]] static std::shared_ptr<RetainedSceneFrameResolver>
  create(collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
         RetainedSceneFrameResolverInput input);

  template <typename RetainedBundle, typename VerifiedRetainedModelMap,
            typename VerifiedRetainedPlanningModel>
  [[nodiscard]] static std::shared_ptr<RetainedSceneFrameResolver>
  create_verified(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      const RetainedBundle &retained_bundle,
      const VerifiedRetainedModelMap &model_map,
      const VerifiedRetainedPlanningModel &planning_model,
      const core::RuntimeUnitId &local_runtime_unit_id,
      const core::RuntimeEpoch &local_runtime_epoch,
      std::vector<RetainedSceneFrameStateSourceInput> provided_state_sources,
      std::optional<core::ControllableComponentId> local_component_id =
          std::nullopt) {
    if (!verify_retained_bundle_identity(retained_bundle)) {
      throw std::invalid_argument(
          "scene-frame resolver received an unverifiable retained bundle");
    }
    model_map.require_same_bundle_as(planning_model);
    if (model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id() ||
        planning_model.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id() ||
        local_runtime_unit_id.empty() || local_runtime_epoch.empty()) {
      throw std::invalid_argument(
          "scene-frame resolver crosses retained or runtime identity");
    }

    RetainedSceneFrameResolverInput input{
        retained_bundle.retained_bundle_id(),
        retained_bundle.value().cell_graph.graph_revision,
        model_map.cell_frame().cell_id,
        model_map.cell_frame().compiled_frame_name,
        model_map.kinematic_index().frame_paths(),
        retained_scene_frame_model_owners(model_map.kinematic_index()),
        {},
        {},
        retained_scene_frame_component_mounts(
            retained_bundle.value().cell_graph, model_map.kinematic_index()),
        {},
        10'000'000};

    for (const auto &attachment :
         retained_bundle.value().cell_graph.component_attachments) {
      input.component_attachments.push_back(
          {attachment.component_attachment_id,
           attachment.component_attachment_revision});
    }

    for (const auto &target : model_map.targets()) {
      for (const auto &target_frame : target.frames) {
        std::visit(
            [&](const auto &frame) {
              if constexpr (requires {
                              frame.controllable_component_id;
                              frame.model_instance_id;
                              frame.frame_role_id;
                              frame.compiled_frame_name;
                              frame.model_revision;
                            }) {
                input.target_frames.push_back(
                    {target.controllable_component_id, target.command_target_id,
                     target_frame.frame_role_id,
                     core::ResolvedComponentModelFrameDTO{
                         frame.controllable_component_id,
                         frame.model_instance_id, frame.frame_role_id,
                         frame.compiled_frame_name, frame.model_revision}});
              }
            },
            target_frame.frame);
      }
    }

    const auto &value = retained_bundle.value();
    const auto &graph = value.cell_graph;
    const auto source_matches_authority =
        [](const core::StateAuthorityClass authority,
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
    const auto source_group_covers =
        [&](const RetainedSceneFrameStateSourceInput &source,
            const core::ObservedStateQueryContextDTO &context,
            const bool local, const bool allow_feedback_alias = false) {
          const bool unavailable_pneumatic =
              source.coordinate_authority ==
                  RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic;
          if (source.coordinate_authority !=
                  RetainedSceneFrameCoordinateAuthority::kRequiredPublication &&
              !unavailable_pneumatic) {
            return false;
          }
          return (unavailable_pneumatic
                      ? !source.provider &&
                            source.context.capability_id.value() ==
                                "tool.gripper.command" &&
                            (context.capability_id.value() == "tool.gripper.command" ||
                             (allow_feedback_alias &&
                              context.capability_id.value() == "tool.gripper.feedback"))
                      : static_cast<bool>(source.provider)) &&
                 source_matches_authority(context.state_authority_class,
                                          source.publication_source) &&
                 source.context.controllable_component_id ==
                     context.controllable_component_id &&
                 source.context.command_target_id ==
                     context.command_target_id &&
                 source.context.control_domain_id ==
                     context.control_domain_id &&
                 source.context.control_domain_revision ==
                     context.control_domain_revision &&
                 source.context.controller_topology_id ==
                     context.controller_topology_id &&
                 source.context.topology_revision ==
                     context.topology_revision &&
                 source.context.resources == context.resources &&
                 source.context.joint_names == context.joint_names &&
                 source.context.coordinate_kinds == context.coordinate_kinds &&
                 source.context.control_interface_contract_id ==
                     context.control_interface_contract_id &&
                 source.context.control_interface_revision ==
                     context.control_interface_revision &&
                 !source.context.active_control_configuration_id.empty() &&
                 source.context.active_control_configuration_generation
                     .nonzero() &&
                 source.context.provider_instance_id ==
                     context.provider_instance_id &&
                 source.context.provider_manifest_id ==
                     context.provider_manifest_id &&
                 source.context.provider_manifest_revision ==
                     context.provider_manifest_revision &&
                 !source.context.runtime_epoch.empty() &&
                 (!local ||
                  source.context.runtime_epoch == local_runtime_epoch) &&
                 source.context.state_authority_id ==
                     context.state_authority_id &&
                 source.context.state_authority_class ==
                     context.state_authority_class &&
                 source.context.freshness_budget_ns >= 0 &&
                 (source.context.state_authority_class !=
                      core::StateAuthorityClass::kMeasured ||
                  source.context.freshness_budget_ns > 0);
    };
    for (const auto &coordinate_map : model_map.control_bindings()) {
      if (coordinate_map.resources.empty()) {
        throw std::invalid_argument(
            "scene-frame state map has no retained resources");
      }
      const bool component_local_only =
          std::all_of(coordinate_map.resources.begin(),
                      coordinate_map.resources.end(), [](const auto &resource) {
                        return resource.coordinates.empty();
                      });
      if (component_local_only) {
        // Component-local semantic I/O has no model coordinate by contract.
        // It remains owned by its state/control channel, but cannot contribute
        // to FK and must never be projected through a fabricated URDF joint.
        continue;
      }
      if (std::any_of(coordinate_map.resources.begin(),
                      coordinate_map.resources.end(), [](const auto &resource) {
                        return resource.coordinates.empty();
                      })) {
        throw std::invalid_argument(
            "scene-frame state map mixes model and component-local resources");
      }
      const auto graph_binding = std::find_if(
          graph.control_bindings.begin(), graph.control_bindings.end(),
          [&](const auto &candidate) {
            return candidate.control_binding_id ==
                       coordinate_map.control_binding_id &&
                   candidate.controllable_component_id ==
                       coordinate_map.controllable_component_id &&
                   candidate.command_target_id ==
                       coordinate_map.command_target_id;
          });
      if (graph_binding == graph.control_bindings.end()) {
        throw std::invalid_argument(
            "scene-frame state map has no exact graph control binding");
      }
      const auto component = std::find_if(
          graph.controllable_components.begin(),
          graph.controllable_components.end(), [&](const auto &candidate) {
            return candidate.controllable_component_id ==
                   graph_binding->controllable_component_id;
          });
      const auto domain =
          std::find_if(graph.control_domains.begin(),
                       graph.control_domains.end(), [&](const auto &candidate) {
                         return candidate.control_domain_id ==
                                graph_binding->control_domain_id;
                       });
      if (component == graph.controllable_components.end() ||
          domain == graph.control_domains.end()) {
        throw std::invalid_argument(
            "scene-frame state binding has no exact component/domain");
      }
      const bool component_selected =
          !local_component_id.has_value() ||
          graph_binding->controllable_component_id == *local_component_id;
      const bool local =
          component_selected &&
          (std::find(component->runtime_unit_ids.begin(),
                     component->runtime_unit_ids.end(), local_runtime_unit_id) !=
               component->runtime_unit_ids.end() ||
           std::find(domain->runtime_unit_ids.begin(),
                     domain->runtime_unit_ids.end(), local_runtime_unit_id) !=
               domain->runtime_unit_ids.end());
      const auto desired = std::find_if(
          value.desired_control_configurations.begin(),
          value.desired_control_configurations.end(),
          [&](const auto &candidate) {
            return candidate.control_domain_id == domain->control_domain_id &&
                   candidate.control_domain_revision ==
                       domain->control_domain_revision;
          });
      if (desired == value.desired_control_configurations.end() ||
          desired->controller_topology_id !=
              graph_binding->controller_topology_id ||
          desired->topology_revision != graph_binding->topology_revision) {
        throw std::invalid_argument(
            "scene-frame state has no exact desired control configuration");
      }
      const auto route = std::find_if(
          desired->binding_routes.begin(), desired->binding_routes.end(),
          [&](const auto &candidate) {
            return candidate.control_binding_id ==
                       graph_binding->control_binding_id &&
                   candidate.control_binding_revision ==
                       graph_binding->control_binding_revision;
          });
      if (route == desired->binding_routes.end()) {
        throw std::invalid_argument(
            "scene-frame state binding has no selected desired route");
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
      for (const auto &resource : coordinate_map.resources) {
        if (resource.coordinates.empty() ||
            resource.coordinates.front().derived_mimic) {
          throw std::invalid_argument(
              "scene-frame state resource has no direct model coordinate");
        }
        const auto &coordinate = resource.coordinates.front();
        using Kind = std::remove_cvref_t<decltype(coordinate.coordinate_kind)>;
        using Unit = std::remove_cvref_t<decltype(coordinate.unit)>;
        std::optional<core::CoordinateKind> kind;
        if (coordinate.coordinate_kind == Kind::kAngular &&
            coordinate.unit == Unit::kRadians) {
          kind = core::CoordinateKind::kAngular;
        } else if (coordinate.coordinate_kind == Kind::kLinear &&
                   coordinate.unit == Unit::kMeters) {
          kind = core::CoordinateKind::kLinear;
        }
        if (!kind.has_value()) {
          throw std::invalid_argument(
              "scene-frame state coordinate has no canonical scalar unit");
        }
        context.resources.push_back(resource.resource_id);
        context.joint_names.push_back(coordinate.compiled_joint_name);
        context.coordinate_kinds.push_back(*kind);
      }

      const auto provided = std::find_if(
          provided_state_sources.begin(), provided_state_sources.end(),
          [&](const auto &candidate) {
            return candidate.context.control_binding_id ==
                       graph_binding->control_binding_id &&
                   candidate.context.control_binding_revision ==
                       graph_binding->control_binding_revision;
          });
      if (provided != provided_state_sources.end()) {
        if (std::find_if(std::next(provided), provided_state_sources.end(),
                         [&](const auto &candidate) {
                           return candidate.context.control_binding_id ==
                                      graph_binding->control_binding_id &&
                                  candidate.context.control_binding_revision ==
                                      graph_binding->control_binding_revision;
                         }) != provided_state_sources.end() ||
            !source_group_covers(*provided, context, local) ||
            provided->context.capability_id != context.capability_id ||
            provided->context.control_route_id != context.control_route_id ||
            provided->context.control_binding_id !=
                context.control_binding_id ||
            provided->context.control_binding_revision !=
                context.control_binding_revision) {
          throw std::invalid_argument(
              "scene-frame state provider differs from its retained binding");
        }
        // Keep checked command absence available for same-owner feedback
        // aliases during construction; remove it before creating live sources.
        input.state_sources.push_back(std::move(*provided));
        provided_state_sources.erase(provided);
      } else {
        if (!local) {
          continue;
        }
        const auto covered_by_direct_source =
            std::any_of(input.state_sources.begin(), input.state_sources.end(),
                        [&](const auto &source) {
                          return source.coordinate_authority ==
                                     RetainedSceneFrameCoordinateAuthority::kRequiredPublication &&
                                 source_group_covers(source, context, local);
                        }) ||
            std::any_of(provided_state_sources.begin(),
                        provided_state_sources.end(),
                        [&](const auto &source) {
                          return source.coordinate_authority ==
                                     RetainedSceneFrameCoordinateAuthority::kRequiredPublication &&
                                 source_group_covers(source, context, local);
                        });
        const auto same_pneumatic_command_owner = [&](const auto &source) {
          if (source.coordinate_authority !=
                  RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic ||
              context.capability_id.value() != "tool.gripper.feedback" ||
              source.context.control_route_id != context.control_route_id ||
              !source_group_covers(source, context, local, true)) {
            return false;
          }
          const auto command = std::find_if(
              graph.control_bindings.begin(), graph.control_bindings.end(),
              [&](const auto &candidate) {
                return candidate.control_binding_id ==
                           source.context.control_binding_id &&
                       candidate.control_binding_revision ==
                           source.context.control_binding_revision &&
                       candidate.capability_id.value() == "tool.gripper.command" &&
                       candidate.controllable_component_id ==
                           context.controllable_component_id &&
                       candidate.command_target_id == context.command_target_id;
              });
          return command != graph.control_bindings.end() &&
                 command->provider_runtime_unit_id ==
                     graph_binding->provider_runtime_unit_id;
        };
        // A feedback map may precede its command map. The command marker must
        // still pass exact binding validation and be consumed before return.
        const bool covered_by_pneumatic_command =
            std::any_of(input.state_sources.begin(), input.state_sources.end(),
                        same_pneumatic_command_owner) ||
            std::any_of(provided_state_sources.begin(),
                        provided_state_sources.end(),
                        same_pneumatic_command_owner);
        if (covered_by_direct_source || covered_by_pneumatic_command) {
          continue;
        }
        throw std::invalid_argument(
            "scene-frame direct local state provider is missing");
      }
    }
    if (!provided_state_sources.empty()) {
      throw std::invalid_argument("scene-frame state provider is unused");
    }
    // Construction-only absence never reaches the live FK/provider list.
    std::erase_if(input.state_sources, [](const auto &source) {
      return source.coordinate_authority ==
             RetainedSceneFrameCoordinateAuthority::kUnavailablePneumatic;
    });
    return create(std::move(shared_model), std::move(input));
  }

  ~RetainedSceneFrameResolver() override;

  [[nodiscard]] bool matches_structural_revision(
      const core::RetainedBundleId &retained_bundle_id,
      const core::GraphRevision &graph_revision) const noexcept;

  [[nodiscard]] core::Result<core::ProgramFrameResolutionContextDTO>
  capture_program_frame_resolution_context(
      core::ProgramFrameResolutionCaptureDTO capture) const override;

  [[nodiscard]] core::Result<core::Pose3DTO> resolve_reference_pose(
      const core::ReferenceDTO &reference,
      const core::ProgramFrameResolutionContextDTO &context) const override;

  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO>
  resolve_scene_frame(
      const core::SceneFrameRefDTO &reference,
      const core::SceneFrameResolutionContextDTO &context) const override;

  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO>
  Resolve(const core::SceneFrameResolutionQueryDTO &query,
          std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
          std::int64_t now_ns) const;

  /// Express one pose between two cell frames. Both frames are resolved from
  /// one coherent state capture at `now_ns`: every crossed control binding is
  /// read once and both frame paths see that same publication, each checked
  /// under the ordinary freshness rules; planner state is never read.
  [[nodiscard]] core::Result<RetainedSceneFrameTransform>
  ResolveFrameTransform(
      const RetainedSceneFrameTransformQuery &query,
      std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
      std::int64_t now_ns) const;

  /// Resolve one carrier frame against the exact retained predecessor while
  /// replacing only the selected control binding with a predicted terminal
  /// state. All other path state remains owner-issued and immutable.
  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO> ResolveCandidate(
      const core::CandidateSceneFrameResolutionQueryDTO &query,
      std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
      std::int64_t now_ns) const;

private:
  class Impl;
  class StateCapture;
  explicit RetainedSceneFrameResolver(std::shared_ptr<Impl> impl);
  /// A failure to find the selected frame's retained path names
  /// `requested_reference` when the frame is resolved for one, else the
  /// selected frame; every other failure names no reference.
  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO>
  ResolveWithCandidateState(
      const core::SceneFrameResolutionSelectorDTO &selector,
      const core::ProjectionLeaseBindingDTO &projection_binding,
      std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
      const StateCapture &capture,
      const core::ControlledStateDTO *predicted_state,
      const core::ReferenceDTO *requested_reference) const;
  [[nodiscard]] core::Result<core::ResolvedSceneFramePoseDTO>
  ResolveFrameWithCandidateState(
      const core::SceneFrameRefDTO &reference,
      const core::SceneFrameResolutionContextDTO &context,
      const core::ControlledStateDTO *predicted_state) const;
  std::shared_ptr<Impl> impl_;
};

} // namespace manyforge::roboplan_backend

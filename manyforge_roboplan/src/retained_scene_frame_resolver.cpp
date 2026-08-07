#include <cmath>
#include <manyforge_core/addressed_observed_state_publication.hpp>
#include <manyforge_core/kinematic_state_view_provider.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

namespace manyforge::roboplan_backend {
namespace {

using SourceKey = std::pair<core::ControlBindingId, core::ControlBindingRevision>;

template <typename T>
[[nodiscard]] core::Result<T> resolution_error(std::string reason, std::string message) {
  return core::Result<T>::Err(
      core::ErrorDTO{std::string(core::to_string(core::ErrorCode::kMissingReference)),
                     std::move(message),
                     {{"reason", std::move(reason)}}});
}

[[nodiscard]] double position_value(const core::CoordinatePositionDTO& position) noexcept {
  if (const auto* angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

[[nodiscard]] core::ObservedRobotStateSourceKind numeric_source_kind(
    const core::ObservedStateSourceKind source) noexcept {
  switch (source) {
    case core::ObservedStateSourceKind::kActionFeedback:
      return core::ObservedRobotStateSourceKind::kActionFeedback;
    case core::ObservedStateSourceKind::kControllerState:
      return core::ObservedRobotStateSourceKind::kControllerState;
    case core::ObservedStateSourceKind::kJointState:
      return core::ObservedRobotStateSourceKind::kJointState;
    case core::ObservedStateSourceKind::kCommandedState:
      return core::ObservedRobotStateSourceKind::kCommandedState;
    case core::ObservedStateSourceKind::kPredictedState:
      return core::ObservedRobotStateSourceKind::kPredictedState;
  }
  return core::ObservedRobotStateSourceKind::kJointState;
}

[[nodiscard]] core::ObservedRobotStateAuthorityProfile authority_profile(
    const core::StateAuthorityClass authority) {
  switch (authority) {
    case core::StateAuthorityClass::kMeasured:
      return core::ObservedRobotStateAuthorityProfile::kMeasured;
    case core::StateAuthorityClass::kCommanded:
      return core::ObservedRobotStateAuthorityProfile::kCommandedSimulation;
    case core::StateAuthorityClass::kPredicted:
      return core::ObservedRobotStateAuthorityProfile::kPredictedNative;
  }
  throw std::invalid_argument("unknown frame-state authority class");
}

[[nodiscard]] bool reference_matches_frame(const core::SceneFrameRefDTO& reference,
                                           const core::ResolvedFrameDTO& resolved) noexcept {
  return std::visit(
      [&](const auto& selected) {
        using Ref = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<Ref, core::CellSceneFrameRefDTO>) {
          const auto* frame = std::get_if<core::ResolvedCellFrameDTO>(&resolved);
          return frame != nullptr && frame->cell_id == selected.cell_id;
        } else if constexpr (std::is_same_v<Ref, core::ComponentSceneFrameRefDTO>) {
          const auto* frame = std::get_if<core::ResolvedComponentModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->controllable_component_id == selected.controllable_component_id &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else {
          const auto* frame = std::get_if<core::ResolvedPassiveModelFrameDTO>(&resolved);
          return frame != nullptr && frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        }
      },
      reference);
}

}  // namespace

class RetainedSceneFrameResolver::Impl final {
 public:
  struct Source final {
    core::ObservedStateQueryContextDTO context;
    core::ObservedStateSourceKind publication_source;
    std::shared_ptr<const core::IObservedStateProvider> provider;
  };

  Impl(collision::ManyForgeCollisionEngine::SharedModelPtr model,
       RetainedSceneFrameResolverInput input,
       std::vector<std::shared_ptr<core::AddressedObservedStatePublication>> local_publications,
       std::vector<Source> sources,
       std::unique_ptr<core::ImmutableKinematicStateViewProvider> view_provider)
      : model_(std::move(model)),
        input_(std::move(input)),
        local_publications_(std::move(local_publications)),
        sources_(std::move(sources)),
        view_provider_(std::move(view_provider)) {}

  [[nodiscard]] const Source* FindSource(const SourceKey& key) const noexcept {
    const auto found =
        std::lower_bound(sources_.begin(), sources_.end(), key,
                         [](const Source& candidate, const SourceKey& selected) {
                           return SourceKey{candidate.context.control_binding_id,
                                            candidate.context.control_binding_revision} < selected;
                         });
    return found == sources_.end() || found->context.control_binding_id != key.first ||
                   found->context.control_binding_revision != key.second
               ? nullptr
               : &*found;
  }

  [[nodiscard]] core::Result<core::KinematicStatePathQueryDTO> BuildQuery(
      const core::KinematicFramePathDTO& path) const {
    std::map<core::ResourceId, const Source*> selected_sources;
    std::map<core::ControlDomainId, core::KinematicActiveControlConfigurationExpectationDTO>
        configurations;
    for (const auto& segment : path.segments) {
      const auto* joint = std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
      if (joint == nullptr ||
          joint->coordinate_kind == core::KinematicJointCoordinateKind::kFixed) {
        continue;
      }
      std::vector<std::pair<const Source*, const core::KinematicResourceCoordinateDependencyDTO*>>
          candidates;
      for (const auto& dependency : joint->eligible_resource_dependencies) {
        const auto* source =
            FindSource({dependency.control_binding_id, dependency.control_binding_revision});
        if (source != nullptr &&
            source->context.control_domain_id == dependency.control_domain_id &&
            source->context.control_domain_revision == dependency.control_domain_revision &&
            std::find(source->context.resources.begin(), source->context.resources.end(),
                      dependency.resource_id) != source->context.resources.end()) {
          candidates.emplace_back(source, &dependency);
        }
      }
      std::sort(candidates.begin(), candidates.end(), [](const auto& left, const auto& right) {
        return std::tie(left.first->context.control_binding_id,
                        left.first->context.control_binding_revision, left.second->resource_id) <
               std::tie(right.first->context.control_binding_id,
                        right.first->context.control_binding_revision, right.second->resource_id);
      });
      candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
      if (candidates.empty()) {
        return resolution_error<core::KinematicStatePathQueryDTO>(
            "kinematic_state_source_unavailable",
            "no exact observed-state source covers the retained frame path");
      }
      if (candidates.size() != 1U) {
        return resolution_error<core::KinematicStatePathQueryDTO>(
            "kinematic_state_source_ambiguous",
            "multiple active observed-state sources cover one frame "
            "coordinate");
      }
      const auto* source = candidates.front().first;
      const auto& dependency = *candidates.front().second;
      const auto [resource, inserted] = selected_sources.emplace(dependency.resource_id, source);
      if (!inserted && resource->second != source) {
        return resolution_error<core::KinematicStatePathQueryDTO>(
            "kinematic_state_source_ambiguous",
            "one frame resource resolves to multiple observed-state sources");
      }
      const core::KinematicActiveControlConfigurationExpectationDTO expected{
          source->context.control_domain_id, source->context.control_domain_revision,
          source->context.active_control_configuration_id,
          source->context.active_control_configuration_generation};
      const auto [configuration, unique] =
          configurations.emplace(expected.control_domain_id, expected);
      if (!unique && configuration->second != expected) {
        return resolution_error<core::KinematicStatePathQueryDTO>(
            "kinematic_active_configuration_ambiguous",
            "frame path crosses conflicting active-control configurations");
      }
    }

    core::KinematicStatePathQueryDTO query{input_.retained_bundle_id,
                                           input_.graph_revision,
                                           path,
                                           {},
                                           input_.maximum_receiver_time_skew_ns,
                                           {}};
    for (const auto& [unused, expected] : configurations) {
      static_cast<void>(unused);
      query.expected_active_control_configurations.push_back(expected);
    }
    for (const auto& [resource, source] : selected_sources) {
      query.source_projections.push_back(
          {resource,
           {{source->context.control_binding_id, source->context.control_binding_revision}},
           source->context.provider_instance_id,
           source->context.provider_manifest_id,
           source->context.provider_manifest_revision,
           source->context.state_authority_id,
           source->context.state_authority_class,
           source->publication_source});
    }
    return core::Result<core::KinematicStatePathQueryDTO>::Ok(std::move(query));
  }

  collision::ManyForgeCollisionEngine::SharedModelPtr model_;
  RetainedSceneFrameResolverInput input_;
  std::vector<std::shared_ptr<core::AddressedObservedStatePublication>> local_publications_;
  std::vector<Source> sources_;
  std::unique_ptr<core::ImmutableKinematicStateViewProvider> view_provider_;
};

std::shared_ptr<RetainedSceneFrameResolver> RetainedSceneFrameResolver::create(
    collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
    RetainedSceneFrameResolverInput input) {
  if (!shared_model || input.retained_bundle_id.empty() || input.graph_revision.empty() ||
      input.cell_id.empty() || input.compiled_cell_frame_name.empty() ||
      input.frame_paths.empty() || input.maximum_receiver_time_skew_ns < 0 ||
      input.initialization_time_ns < 0) {
    throw std::invalid_argument("retained scene-frame resolver input is incomplete");
  }
  std::sort(input.target_frames.begin(), input.target_frames.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.controllable_component_id, left.command_target_id,
                              left.frame.frame_role_id, left.frame.model_instance_id) <
                     std::tie(right.controllable_component_id, right.command_target_id,
                              right.frame.frame_role_id, right.frame.model_instance_id);
            });
  for (const auto& target : input.target_frames) {
    if (target.controllable_component_id.empty() || target.command_target_id.empty() ||
        target.frame.controllable_component_id != target.controllable_component_id ||
        target.frame.model_instance_id.empty() || target.frame.frame_role_id.empty() ||
        target.frame.compiled_frame_name.empty() || target.frame.model_revision.empty() ||
        !collision::ManyForgeCollisionEngine::retained_frame_exists(
            shared_model, target.frame.compiled_frame_name)) {
      throw std::invalid_argument("retained scene-frame target map is invalid");
    }
  }
  std::sort(input.component_attachments.begin(), input.component_attachments.end(),
            [](const auto& left, const auto& right) {
              return left.component_attachment_id < right.component_attachment_id;
            });
  for (std::size_t index = 0U; index < input.component_attachments.size(); ++index) {
    const auto& attachment = input.component_attachments[index];
    if (attachment.component_attachment_id.empty() ||
        attachment.component_attachment_revision.empty() ||
        (index != 0U && input.component_attachments[index - 1U].component_attachment_id ==
                            attachment.component_attachment_id)) {
      throw std::invalid_argument("retained scene-frame component attachment map is invalid");
    }
  }

  std::vector<std::shared_ptr<core::AddressedObservedStatePublication>> local_publications;
  std::vector<Impl::Source> sources;
  std::vector<core::KinematicObservedStateSourceBinding> provider_bindings;
  for (auto& input_source : input.state_sources) {
    std::shared_ptr<const core::IObservedStateProvider> provider = input_source.provider;
    if (provider && !input_source.local_source_id.empty()) {
      throw std::invalid_argument("direct frame-state provider also declares a bootstrap source");
    }
    if (!provider) {
      if (input_source.local_source_id.empty() ||
          input_source.context.state_authority_class != core::StateAuthorityClass::kPredicted ||
          input_source.publication_source != core::ObservedStateSourceKind::kPredictedState) {
        throw std::invalid_argument("local frame-state source is not exact predicted authority");
      }
      auto neutral = collision::ManyForgeCollisionEngine::retained_neutral_scalar_joint_positions(
          shared_model, input_source.context.joint_names);
      if (!neutral.ok()) {
        throw std::invalid_argument(neutral.error().message);
      }
      auto publication = std::make_shared<core::AddressedObservedStatePublication>(
          core::AddressedObservedStatePublicationConfig{
              input_source.context,
              {core::ObservedRobotStateSourceConfig{
                  numeric_source_kind(input_source.publication_source),
                  input_source.local_source_id,
                  authority_profile(input_source.context.state_authority_class),
                  input_source.context.freshness_budget_ns, "retained_scene_frame_local_state"}},
              1U,
              256U});
      const std::vector<double> velocities(neutral.value().size(), 0.0);
      const auto published = publication->publish(
          input_source.publication_source, input_source.context.joint_names, neutral.value(),
          velocities, input.initialization_time_ns, input.initialization_time_ns);
      if (!published.published || !published.complete) {
        throw std::invalid_argument("retained scene-frame neutral state could not be published");
      }
      provider = publication;
      local_publications.push_back(std::move(publication));
    }
    sources.push_back({input_source.context, input_source.publication_source, provider});
    provider_bindings.push_back({input_source.context.control_binding_id,
                                 input_source.context.control_binding_revision,
                                 std::move(provider)});
  }
  std::sort(sources.begin(), sources.end(), [](const auto& left, const auto& right) {
    return std::tie(left.context.control_binding_id, left.context.control_binding_revision) <
           std::tie(right.context.control_binding_id, right.context.control_binding_revision);
  });
  for (std::size_t index = 1U; index < sources.size(); ++index) {
    if (sources[index - 1U].context.control_binding_id ==
            sources[index].context.control_binding_id &&
        sources[index - 1U].context.control_binding_revision ==
            sources[index].context.control_binding_revision) {
      throw std::invalid_argument("retained scene-frame state source is duplicated");
    }
  }
  auto view_provider =
      std::make_unique<core::ImmutableKinematicStateViewProvider>(std::move(provider_bindings));
  return std::shared_ptr<RetainedSceneFrameResolver>(
      new RetainedSceneFrameResolver(std::make_shared<Impl>(
          std::move(shared_model), std::move(input), std::move(local_publications),
          std::move(sources), std::move(view_provider))));
}

RetainedSceneFrameResolver::RetainedSceneFrameResolver(std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RetainedSceneFrameResolver::~RetainedSceneFrameResolver() = default;

core::Result<core::Pose3DTO> RetainedSceneFrameResolver::resolve_reference_pose(
    const core::ReferenceDTO&, const std::string&, const core::ProjectionLeaseBindingDTO&) const {
  return resolution_error<core::Pose3DTO>("program_reference_resolution_not_installed",
                                          "Program reference resolution is completed by N5c-d/N6");
}

core::Result<core::ResolvedSceneFramePoseDTO> RetainedSceneFrameResolver::resolve_scene_frame(
    const core::SceneFrameRefDTO& reference,
    const core::SceneFrameResolutionContextDTO& context) const {
  if (!context.valid() ||
      !reference_matches_frame(reference, context.kinematic_path.terminal_frame)) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_context_mismatch",
        "scene-frame reference differs from its immutable retained context");
  }

  std::vector<collision::RetainedKinematicCoordinateValue> coordinates;
  for (const auto& model_state : context.kinematic_state_path_view.model_states) {
    if (model_state.coordinate_projection.size() != model_state.positions.size()) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "kinematic_state_view_shape_mismatch",
          "scene-frame state view has mismatched model coordinates");
    }
    for (std::size_t index = 0U; index < model_state.coordinate_projection.size(); ++index) {
      const auto& projection = model_state.coordinate_projection[index];
      if (projection.derived_mimic) {
        continue;
      }
      const core::KinematicModelJointPathSegmentDTO* selected = nullptr;
      for (const auto& segment : context.kinematic_path.segments) {
        const auto* joint = std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
        if (joint == nullptr || !joint->controllable_component_id.has_value() ||
            *joint->controllable_component_id != model_state.controllable_component_id ||
            joint->model_instance_id != model_state.model_instance_id ||
            joint->model_revision != model_state.model_revision ||
            joint->composition_coordinate_index != projection.composition_coordinate_index) {
          continue;
        }
        if (selected != nullptr) {
          return resolution_error<core::ResolvedSceneFramePoseDTO>(
              "kinematic_coordinate_ambiguous",
              "state-view coordinate maps to multiple retained path joints");
        }
        selected = joint;
      }
      if (selected == nullptr) {
        return resolution_error<core::ResolvedSceneFramePoseDTO>(
            "kinematic_coordinate_missing", "state-view coordinate has no retained path joint");
      }
      coordinates.push_back(
          {selected->compiled_joint_name, position_value(model_state.positions[index])});
    }
  }
  auto pose = collision::ManyForgeCollisionEngine::resolve_retained_frame_pose(
      impl_->model_, impl_->input_.compiled_cell_frame_name,
      core::compiled_frame_name(context.kinematic_path.terminal_frame), coordinates);
  if (!pose.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(pose.error());
  }
  core::ResolvedSceneFramePoseDTO result{context.kinematic_path.terminal_frame,
                                         pose.value(),
                                         {},
                                         context.projection_binding,
                                         context.kinematic_path,
                                         context.kinematic_state_path_view,
                                         core::ContentDigest(std::string{})};
  try {
    result.resolution_evidence_digest = core::ComputeSceneFrameResolutionEvidenceDigest(result);
  } catch (const std::exception& error) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>("scene_frame_evidence_failed",
                                                             error.what());
  }
  if (!result.valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_result_invalid", "scene-frame resolver produced an invalid immutable result");
  }
  return core::Result<core::ResolvedSceneFramePoseDTO>::Ok(std::move(result));
}

core::Result<core::ResolvedSceneFramePoseDTO> RetainedSceneFrameResolver::Resolve(
    const core::SceneFrameResolutionQueryDTO& query,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
    const std::int64_t now_ns) const {
  if (!query.valid() || !logical_scene || !logical_scene->locally_valid() ||
      logical_scene->revision != query.projection_binding.projection_revision.scene_revision ||
      query.projection_binding.projection_revision.retained_bundle_id !=
          impl_->input_.retained_bundle_id ||
      query.projection_binding.projection_revision.graph_revision != impl_->input_.graph_revision ||
      logical_scene->cell_id != impl_->input_.cell_id || now_ns < 0) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_query_mismatch",
        "scene-frame query differs from the active retained projection");
  }

  auto selected_reference = std::visit(
      [&](const auto& selector) -> core::Result<core::SceneFrameRefDTO> {
        using Selector = std::remove_cvref_t<decltype(selector)>;
        if constexpr (std::is_same_v<Selector, core::SceneFrameRefDTO>) {
          return core::Result<core::SceneFrameRefDTO>::Ok(selector);
        } else {
          const RetainedTargetSceneFrameInput* selected = nullptr;
          for (const auto& candidate : impl_->input_.target_frames) {
            if (candidate.controllable_component_id != selector.controllable_component_id ||
                candidate.command_target_id != selector.command_target_id ||
                candidate.frame.frame_role_id != selector.frame_role_id) {
              continue;
            }
            if (selected != nullptr) {
              return resolution_error<core::SceneFrameRefDTO>(
                  "scene_frame_reference_ambiguous",
                  "target carrier frame is ambiguous in retained model map");
            }
            selected = &candidate;
          }
          if (selected == nullptr) {
            return resolution_error<core::SceneFrameRefDTO>(
                "scene_frame_reference_missing",
                "target carrier frame is absent from retained model map");
          }
          return core::Result<core::SceneFrameRefDTO>::Ok(core::ComponentSceneFrameRefDTO{
              selected->frame.controllable_component_id, selected->frame.model_instance_id,
              selected->frame.frame_role_id});
        }
      },
      query.selector);
  if (!selected_reference.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(selected_reference.error());
  }
  const auto& reference = selected_reference.value();

  const core::KinematicFramePathDTO* path = nullptr;
  for (const auto& candidate : impl_->input_.frame_paths) {
    if (!reference_matches_frame(reference, candidate.terminal_frame)) {
      continue;
    }
    if (path != nullptr) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "scene_frame_reference_ambiguous",
          "scene-frame reference maps to multiple retained paths");
    }
    path = &candidate;
  }
  if (path == nullptr) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_reference_missing", "scene-frame reference has no retained compiler path");
  }
  std::vector<core::ComponentAttachmentRevision> attachment_revisions;
  for (const auto& segment : path->segments) {
    const auto* attachment =
        std::get_if<core::KinematicComponentAttachmentPathSegmentDTO>(&segment);
    if (attachment == nullptr) {
      continue;
    }
    const auto mapped = std::lower_bound(
        impl_->input_.component_attachments.begin(), impl_->input_.component_attachments.end(),
        attachment->component_attachment_id,
        [](const auto& candidate, const core::ComponentAttachmentId& selected) {
          return candidate.component_attachment_id < selected;
        });
    if (mapped == impl_->input_.component_attachments.end() ||
        mapped->component_attachment_id != attachment->component_attachment_id) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "scene_frame_structural_revision_missing",
          "scene-frame path crosses an unmapped component attachment");
    }
    attachment_revisions.push_back(mapped->component_attachment_revision);
  }
  std::sort(attachment_revisions.begin(), attachment_revisions.end());
  attachment_revisions.erase(std::unique(attachment_revisions.begin(), attachment_revisions.end()),
                             attachment_revisions.end());
  auto state_query = impl_->BuildQuery(*path);
  if (!state_query.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(state_query.error());
  }
  auto state_view =
      impl_->view_provider_->current_kinematic_state_path(state_query.value(), now_ns);
  if (!state_view.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(state_view.error());
  }
  auto result = resolve_scene_frame(
      reference,
      core::SceneFrameResolutionContextDTO{query.projection_binding, std::move(logical_scene),
                                           *path, std::move(state_view.value())});
  if (!result.ok()) {
    return result;
  }
  result.value().component_attachment_revisions = std::move(attachment_revisions);
  try {
    result.value().resolution_evidence_digest =
        core::ComputeSceneFrameResolutionEvidenceDigest(result.value());
  } catch (const std::exception& error) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>("scene_frame_evidence_failed",
                                                             error.what());
  }
  if (!result.value().valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_result_invalid", "scene-frame structural evidence is invalid");
  }
  return result;
}

}  // namespace manyforge::roboplan_backend

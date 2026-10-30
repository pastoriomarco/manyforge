#include <cmath>
#include <functional>
#include <manyforge_core/geometry/pose_math.hpp>
#include <manyforge_core/kinematic_state_view_provider.hpp>
#include <manyforge_roboplan/retained_scene_frame_resolver.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <variant>

namespace manyforge::roboplan_backend {
namespace {

using SourceKey =
    std::pair<core::ControlBindingId, core::ControlBindingRevision>;

template <typename T>
[[nodiscard]] core::Result<T> resolution_error(std::string reason,
                                               std::string message) {
  return core::Result<T>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kMissingReference)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

template <typename T>
[[nodiscard]] core::Result<T> ambiguous_resolution_error(std::string reason,
                                                         std::string message) {
  return core::Result<T>::Err(core::ErrorDTO{core::kErrorAmbiguousReference,
                                             std::move(message),
                                             {{"reason", std::move(reason)}}});
}

/// Stable wire name of one ReferenceDTO kind (spec 210 §4.4).
[[nodiscard]] const char *
reference_type_name(const core::ReferenceDTO::Type type) noexcept {
  switch (type) {
  case core::ReferenceDTO::Type::kFrameRole:
    return "frame_role";
  case core::ReferenceDTO::Type::kComponentBodyRole:
    return "component_body_role";
  case core::ReferenceDTO::Type::kSceneResource:
    return "scene_resource";
  case core::ReferenceDTO::Type::kRuntimeObject:
    return "runtime_object";
  case core::ReferenceDTO::Type::kPose:
    return "pose";
  case core::ReferenceDTO::Type::kCellFrame:
    return "cell_frame";
  }
  return "pose";
}

/// The complete identity of the one reference a failure is about (spec 210
/// §5.3). `target_role_id` is set only for a robot-qualified frame role.
struct ReferenceIdentity final {
  std::string reference_type;
  std::string reference_id;
  std::string target_role_id;
};

/// A requested reference. A cell frame is identified by the resolver's cell;
/// an inline pose has no id.
[[nodiscard]] ReferenceIdentity
reference_identity(const core::ReferenceDTO &reference,
                   const core::CellId &cell_id) {
  return {reference_type_name(reference.type),
          reference.type == core::ReferenceDTO::Type::kCellFrame
              ? cell_id.value()
              : std::string(core::reference_identity_value(reference)),
          reference.target.has_value()
              ? reference.target->target_role_id.value()
              : std::string{}};
}

/// One resolved or scene frame: the cell frame by its cell, every model frame
/// by its frame role. `target_role_id` qualifies a frame of a declared
/// Program target, such as the moving target's own base frame.
template <typename Frame>
[[nodiscard]] ReferenceIdentity
frame_identity(const Frame &frame,
               const core::TargetRoleId *target_role_id = nullptr) {
  if constexpr (requires { frame.cell_id; }) {
    return {"cell_frame", frame.cell_id.value(), {}};
  } else if constexpr (requires { frame.controllable_component_id; }) {
    return {"frame_role", frame.frame_role_id.value(),
            target_role_id != nullptr ? target_role_id->value()
                                      : std::string{}};
  } else {
    // A passive model frame belongs to no Program target.
    return {"frame_role", frame.frame_role_id.value(), {}};
  }
}

template <typename... Frames>
[[nodiscard]] ReferenceIdentity
frame_identity(const std::variant<Frames...> &frame,
               const core::TargetRoleId *target_role_id = nullptr) {
  return std::visit(
      [&](const auto &selected) {
        return frame_identity(selected, target_role_id);
      },
      frame);
}

/// The moving target's own base frame, named with that target's role so a
/// base-frame failure is never read as a frame of another robot.
[[nodiscard]] ReferenceIdentity
base_frame_identity(const core::ResolvedExecutionBindingDTO &binding) {
  return frame_identity(binding.base_frame, &binding.target_role_id);
}

/// One runtime-object binding of a capture, by its runtime-object key.
[[nodiscard]] ReferenceIdentity
runtime_object_identity(const core::RuntimeObjectId &runtime_object_id) {
  return {reference_type_name(core::ReferenceDTO::Type::kRuntimeObject),
          runtime_object_id.value(),
          {}};
}

/// Attaches `about` to a failure. Attribution is opt-in (spec 210 §5.3): only
/// a site that knows which one reference failed passes its identity, so every
/// other failure, including any new one, names no reference. The identity is
/// written whole and never mixed with another reference's fields.
[[nodiscard]] core::ErrorDTO about_reference(core::ErrorDTO error,
                                             const ReferenceIdentity &about) {
  error.details["reference_type"] = about.reference_type;
  error.details["reference_id"] = about.reference_id;
  if (about.target_role_id.empty()) {
    error.details.erase("target_role_id");
  } else {
    error.details["target_role_id"] = about.target_role_id;
  }
  return error;
}

template <typename T>
[[nodiscard]] core::Result<T> resolution_error(std::string reason,
                                               std::string message,
                                               const ReferenceIdentity &about) {
  return core::Result<T>::Err(about_reference(
      resolution_error<T>(std::move(reason), std::move(message)).error(),
      about));
}

template <typename T>
[[nodiscard]] core::Result<T>
ambiguous_resolution_error(std::string reason, std::string message,
                           const ReferenceIdentity &about) {
  return core::Result<T>::Err(
      about_reference(ambiguous_resolution_error<T>(std::move(reason),
                                                    std::move(message))
                          .error(),
                      about));
}

[[nodiscard]] double
position_value(const core::CoordinatePositionDTO &position) noexcept {
  if (const auto *angular = std::get_if<core::AngularPositionDTO>(&position)) {
    return angular->value_rad;
  }
  return std::get<core::LinearPositionDTO>(position).value_m;
}

[[nodiscard]] bool
reference_matches_frame(const core::SceneFrameRefDTO &reference,
                        const core::ResolvedFrameDTO &resolved) noexcept {
  return std::visit(
      [&](const auto &selected) {
        using Ref = std::remove_cvref_t<decltype(selected)>;
        if constexpr (std::is_same_v<Ref, core::CellSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedCellFrameDTO>(&resolved);
          return frame != nullptr && frame->cell_id == selected.cell_id;
        } else if constexpr (std::is_same_v<Ref,
                                            core::ComponentSceneFrameRefDTO>) {
          const auto *frame =
              std::get_if<core::ResolvedComponentModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->controllable_component_id ==
                     selected.controllable_component_id &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        } else {
          const auto *frame =
              std::get_if<core::ResolvedPassiveModelFrameDTO>(&resolved);
          return frame != nullptr &&
                 frame->model_instance_id == selected.model_instance_id &&
                 frame->frame_role_id == selected.frame_role_id;
        }
      },
      reference);
}

[[nodiscard]] std::optional<core::SceneFrameRefDTO>
scene_reference_for(const core::ResolvedFrameDTO &resolved) {
  return std::visit(
      [](const auto &frame) -> std::optional<core::SceneFrameRefDTO> {
        using Frame = std::remove_cvref_t<decltype(frame)>;
        if constexpr (std::is_same_v<Frame, core::ResolvedCellFrameDTO>) {
          return core::SceneFrameRefDTO{
              core::CellSceneFrameRefDTO{frame.cell_id}};
        } else if constexpr (std::is_same_v<
                                 Frame, core::ResolvedComponentModelFrameDTO>) {
          return core::SceneFrameRefDTO{core::ComponentSceneFrameRefDTO{
              frame.controllable_component_id, frame.model_instance_id,
              frame.frame_role_id}};
        } else {
          return core::SceneFrameRefDTO{core::PassiveModelSceneFrameRefDTO{
              frame.model_instance_id, frame.frame_role_id}};
        }
      },
      resolved);
}

[[nodiscard]] const core::SceneObjectSnapshotDTO *
find_scene_object(const core::LogicalSceneSnapshotDTO &scene,
                  const core::SceneResourceId &scene_resource_id) noexcept {
  const auto found = std::lower_bound(
      scene.objects.begin(), scene.objects.end(), scene_resource_id,
      [](const auto &candidate, const auto &selected) {
        return candidate.scene_resource_id < selected;
      });
  return found != scene.objects.end() &&
                 found->scene_resource_id == scene_resource_id
             ? &*found
             : nullptr;
}

[[nodiscard]] const core::CarriedObjectAttachmentDTO *
find_attachment(const core::LogicalSceneSnapshotDTO &scene,
                const core::CarriedObjectAttachmentId &attachment_id) noexcept {
  const auto found = std::lower_bound(
      scene.carried_object_attachments.begin(),
      scene.carried_object_attachments.end(), attachment_id,
      [](const auto &candidate, const auto &selected) {
        return candidate.carried_object_attachment_id < selected;
      });
  return found != scene.carried_object_attachments.end() &&
                 found->carried_object_attachment_id == attachment_id
             ? &*found
             : nullptr;
}

[[nodiscard]] bool publication_matches_controlled_start(
    const core::ObservedStatePublicationDTO &publication,
    const core::ControlledStateDTO &state) noexcept {
  return publication.controllable_component_id ==
             state.controllable_component_id &&
         publication.command_target_id == state.command_target_id &&
         publication.control_domain_id == state.control_domain_id &&
         publication.control_domain_revision == state.control_domain_revision &&
         publication.control_binding_id == state.control_binding_id &&
         publication.control_binding_revision ==
             state.control_binding_revision &&
         publication.controller_topology_id == state.controller_topology_id &&
         publication.topology_revision == state.topology_revision &&
         publication.control_route_id == state.control_route_id &&
         publication.active_control_configuration_id ==
             state.active_control_configuration_id &&
         publication.active_control_configuration_generation ==
             state.active_control_configuration_generation &&
         publication.snapshot.resources == state.resources &&
         publication.snapshot.joint_names == state.joint_names &&
         publication.snapshot.positions == state.positions &&
         publication.snapshot.velocities == state.velocities;
}

[[nodiscard]] bool state_view_agrees_with_primary_publication(
    const core::KinematicStatePathViewDTO &view,
    const core::ObservedStatePublicationDTO &publication) noexcept {
  const auto &snapshot = publication.snapshot;
  for (const auto &row : view.resource_states) {
    const auto found = std::find(snapshot.resources.begin(),
                                 snapshot.resources.end(), row.resource_id);
    if (found == snapshot.resources.end()) {
      continue;
    }
    const auto index = static_cast<std::size_t>(
        std::distance(snapshot.resources.begin(), found));
    const auto &sources = row.source_projection.contributing_control_bindings;
    if (index >= snapshot.positions.size() ||
        row.position != snapshot.positions[index] || sources.size() != 1U ||
        sources.front().control_binding_id != publication.control_binding_id ||
        sources.front().control_binding_revision !=
            publication.control_binding_revision ||
        row.source_projection.provider_instance_id !=
            publication.provider_instance_id ||
        row.source_projection.provider_manifest_id !=
            publication.provider_manifest_id ||
        row.source_projection.provider_manifest_revision !=
            publication.provider_manifest_revision ||
        row.source_projection.state_authority_id !=
            publication.state_authority_id ||
        row.source_projection.state_authority_class !=
            publication.state_authority_class ||
        row.active_control_configuration_id !=
            publication.active_control_configuration_id ||
        row.active_control_configuration_generation !=
            publication.active_control_configuration_generation ||
        row.runtime_epoch != publication.runtime_epoch ||
        row.authority_generation != publication.authority_generation ||
        row.publication_sequence != publication.publication_sequence) {
      return false;
    }
  }
  return true;
}

} // namespace

core::ResolvedComponentModelFrameDTO program_binding_frame(
    const std::vector<RetainedSceneFrameModelOwnerInput> &model_owners,
    const core::ControllableComponentId &binding_component,
    const core::ProgramFrameRoleBindingDTO &frame) {
  const auto found = std::find_if(
      model_owners.begin(), model_owners.end(), [&](const auto &row) {
        return row.model_instance_id == frame.model_instance_id;
      });
  auto owner = found != model_owners.end() ? found->controllable_component_id
                                           : binding_component;
  return core::ResolvedComponentModelFrameDTO{
      std::move(owner), frame.model_instance_id, frame.frame_role_id,
      frame.compiled_frame_name, frame.model_revision};
}

class RetainedSceneFrameResolver::Impl final {
public:
  struct Source final {
    core::ObservedStateQueryContextDTO context;
    core::ObservedStateSourceKind publication_source;
    std::shared_ptr<const core::IObservedStateProvider> provider;
  };

  Impl(collision::ManyForgeCollisionEngine::SharedModelPtr model,
       RetainedSceneFrameResolverInput input, std::vector<Source> sources)
      : model_(std::move(model)), input_(std::move(input)),
        sources_(std::move(sources)) {}

  [[nodiscard]] const Source *FindSource(const SourceKey &key) const noexcept {
    const auto found = std::lower_bound(
        sources_.begin(), sources_.end(), key,
        [](const Source &candidate, const SourceKey &selected) {
          return SourceKey{candidate.context.control_binding_id,
                           candidate.context.control_binding_revision} <
                 selected;
        });
    return found == sources_.end() ||
                   found->context.control_binding_id != key.first ||
                   found->context.control_binding_revision != key.second
               ? nullptr
               : &*found;
  }

  /// A component is local when this resolver owns at least one live state
  /// source for it, or (G6) when it has none and a component it is mounted on
  /// has one.
  [[nodiscard]] bool
  HostsRobot(const core::ControllableComponentId &component_id) const {
    return retained_scene_frame_component_local(
        input_.component_mounts, component_id,
        [&](const core::ControllableComponentId &candidate) {
          return std::any_of(sources_.begin(), sources_.end(),
                             [&](const Source &source) {
                               return source.context.controllable_component_id ==
                                      candidate;
                             });
        });
  }

  /// An object carried by a robot hosted elsewhere is refused by name, the
  /// same as a frame of that robot. `about` is the requested reference the
  /// object was reached through.
  [[nodiscard]] core::Result<void>
  RequireLocalCarrier(const core::SceneFrameRefDTO &carrier,
                      const ReferenceIdentity &about) const {
    const auto *component =
        std::get_if<core::ComponentSceneFrameRefDTO>(&carrier);
    if (component == nullptr ||
        HostsRobot(component->controllable_component_id)) {
      return core::Result<void>::Ok();
    }
    return core::Result<void>::Err(
        resolution_error<core::ResolvedFrameDTO>(
            "program_reference_robot_not_local",
            "carried scene resource is held by a robot that is not hosted by "
            "this cell resolver",
            about)
            .error());
  }

  /// Resolve one robot-qualified frame role through the cell-wide retained
  /// target map. A robot hosted elsewhere is refused by name. A failure to
  /// find the frame names `about`; a malformed reference names nothing.
  [[nodiscard]] core::Result<core::ResolvedFrameDTO>
  RobotFrame(const core::ReferenceTargetDTO &target,
             const core::FrameRoleId &frame_role_id,
             const ReferenceIdentity &about) const {
    if (!target.complete() || frame_role_id.empty()) {
      return resolution_error<core::ResolvedFrameDTO>(
          "program_reference_shape_invalid",
          "robot-qualified frame reference is incomplete");
    }
    if (!HostsRobot(target.controllable_component_id)) {
      return resolution_error<core::ResolvedFrameDTO>(
          "program_reference_robot_not_local",
          "frame reference names a robot that is not hosted by this cell "
          "resolver",
          about);
    }
    const RetainedTargetSceneFrameInput *selected = nullptr;
    for (const auto &candidate : input_.target_frames) {
      if (candidate.controllable_component_id !=
              target.controllable_component_id ||
          candidate.command_target_id != target.command_target_id ||
          candidate.target_frame_role_id != frame_role_id) {
        continue;
      }
      if (selected != nullptr) {
        return ambiguous_resolution_error<core::ResolvedFrameDTO>(
            "program_reference_robot_frame_ambiguous",
            "robot frame role maps to multiple retained frames", about);
      }
      selected = &candidate;
    }
    if (selected == nullptr) {
      return resolution_error<core::ResolvedFrameDTO>(
          "program_reference_robot_frame_missing",
          "robot frame role is absent from the retained target map", about);
    }
    return core::Result<core::ResolvedFrameDTO>::Ok(selected->frame);
  }

  [[nodiscard]] core::Result<void>
  ValidateCandidateState(const core::ControlledStateDTO &state) const {
    if (auto valid = core::validate_controlled_state(state); !valid.ok()) {
      return valid;
    }
    const auto *source =
        FindSource({state.control_binding_id, state.control_binding_revision});
    if (source == nullptr) {
      return core::Result<void>::Err(
          resolution_error<core::ResolvedSceneFramePoseDTO>(
              "candidate_state_binding_unavailable",
              "predicted candidate state has no exact retained path source")
              .error());
    }
    const auto current =
        source->provider->publication(source->context.control_binding_id,
                                      source->context.control_binding_revision);
    if (!current.ok()) {
      return core::Result<void>::Err(current.error());
    }
    const auto &expected = source->context;
    const auto &publication = current.value();
    if (state.controllable_component_id != expected.controllable_component_id ||
        state.command_target_id != expected.command_target_id ||
        state.control_domain_id != expected.control_domain_id ||
        state.control_domain_revision != expected.control_domain_revision ||
        state.controller_topology_id != expected.controller_topology_id ||
        state.topology_revision != expected.topology_revision ||
        state.control_route_id != publication.control_route_id ||
        state.active_control_configuration_id !=
            publication.active_control_configuration_id ||
        state.active_control_configuration_generation !=
            publication.active_control_configuration_generation ||
        state.resources != expected.resources ||
        state.joint_names != expected.joint_names ||
        state.positions.size() != expected.coordinate_kinds.size()) {
      return core::Result<void>::Err(
          resolution_error<core::ResolvedSceneFramePoseDTO>(
              "candidate_state_binding_mismatch",
              "predicted candidate state differs from its exact retained "
              "control binding")
              .error());
    }
    for (std::size_t index = 0U; index < state.positions.size(); ++index) {
      if (core::coordinate_kind(state.positions[index]) !=
          expected.coordinate_kinds[index]) {
        return core::Result<void>::Err(
            resolution_error<core::ResolvedSceneFramePoseDTO>(
                "candidate_state_coordinate_mismatch",
                "predicted candidate state changes a retained coordinate "
                "kind")
                .error());
      }
    }
    return core::Result<void>::Ok();
  }

  /// `publications` is the call's one coherent state capture; the query and
  /// the view built from it read the same publication of every source.
  [[nodiscard]] core::Result<core::KinematicStatePathQueryDTO>
  BuildQuery(const core::KinematicFramePathDTO &path, const std::int64_t now_ns,
             const core::IObservedStateProvider &publications) const {
    std::map<core::ResourceId, const Source *> selected_sources;
    std::map<core::ControlDomainId,
             core::KinematicActiveControlConfigurationExpectationDTO>
        configurations;
    for (const auto &segment : path.segments) {
      const auto *joint =
          std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
      if (joint == nullptr || joint->coordinate_kind ==
                                  core::KinematicJointCoordinateKind::kFixed) {
        continue;
      }
      std::vector<
          std::pair<const Source *,
                    const core::KinematicResourceCoordinateDependencyDTO *>>
          candidates;
      for (const auto &dependency : joint->eligible_resource_dependencies) {
        const auto *source = FindSource({dependency.control_binding_id,
                                         dependency.control_binding_revision});
        if (source != nullptr &&
            source->context.control_domain_id == dependency.control_domain_id &&
            source->context.control_domain_revision ==
                dependency.control_domain_revision &&
            std::find(source->context.resources.begin(),
                      source->context.resources.end(),
                      dependency.resource_id) !=
                source->context.resources.end()) {
          candidates.emplace_back(source, &dependency);
        }
      }
      std::sort(candidates.begin(), candidates.end(),
                [](const auto &left, const auto &right) {
                  return std::tie(left.first->context.control_binding_id,
                                  left.first->context.control_binding_revision,
                                  left.second->resource_id) <
                         std::tie(right.first->context.control_binding_id,
                                  right.first->context.control_binding_revision,
                                  right.second->resource_id);
                });
      candidates.erase(std::unique(candidates.begin(), candidates.end()),
                       candidates.end());
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
      const auto *source = candidates.front().first;
      const auto &dependency = *candidates.front().second;
      const auto [resource, inserted] =
          selected_sources.emplace(dependency.resource_id, source);
      if (!inserted && resource->second != source) {
        return resolution_error<core::KinematicStatePathQueryDTO>(
            "kinematic_state_source_ambiguous",
            "one frame resource resolves to multiple observed-state sources");
      }
      const auto current = publications.publication_at(
          source->context.control_binding_id,
          source->context.control_binding_revision, now_ns);
      if (!current.ok()) {
        return core::Result<core::KinematicStatePathQueryDTO>::Err(
            current.error());
      }
      const core::KinematicActiveControlConfigurationExpectationDTO expected{
          current.value().control_domain_id,
          current.value().control_domain_revision,
          current.value().active_control_configuration_id,
          current.value().active_control_configuration_generation};
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
    for (const auto &[unused, expected] : configurations) {
      static_cast<void>(unused);
      query.expected_active_control_configurations.push_back(expected);
    }
    for (const auto &[resource, source] : selected_sources) {
      query.source_projections.push_back(
          {resource,
           {{source->context.control_binding_id,
             source->context.control_binding_revision}},
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
  std::vector<Source> sources_;
};

/// One coherent read of the resolver's live state at one evaluation time.
/// Each control binding is read at most once; every frame-path query and
/// view built from this capture sees that same publication, so two frames of
/// one robot (or a carrier and its robot) can never mix samples. Freshness is
/// still validated per publication at the capture time by the view provider,
/// and the retained receiver-time skew budget holds across every contributor
/// of the capture, so independently fresh robots must also agree in time.
class RetainedSceneFrameResolver::StateCapture final
    : public core::IObservedStateProvider {
public:
  StateCapture(std::shared_ptr<const Impl> impl, const std::int64_t at_ns)
      : impl_(std::move(impl)), at_ns_(at_ns) {}

  [[nodiscard]] static std::shared_ptr<const StateCapture>
  Create(std::shared_ptr<const Impl> impl, const std::int64_t at_ns) {
    auto capture = std::make_shared<StateCapture>(std::move(impl), at_ns);
    std::vector<core::KinematicObservedStateSourceBinding> bindings;
    bindings.reserve(capture->impl_->sources_.size());
    for (const auto &source : capture->impl_->sources_) {
      // Non-owning: the view provider is owned by this capture.
      bindings.push_back(
          {source.context.control_binding_id,
           source.context.control_binding_revision,
           std::shared_ptr<const core::IObservedStateProvider>(
               std::shared_ptr<const void>{}, capture.get())});
    }
    capture->views_ = std::make_unique<core::ImmutableKinematicStateViewProvider>(
        std::move(bindings));
    return capture;
  }

  [[nodiscard]] std::int64_t at_ns() const noexcept { return at_ns_; }

  /// Admits one reading received over [earliest_ns, latest_ns] into this
  /// capture; refused when the capture would exceed the retained skew budget.
  [[nodiscard]] core::Result<void>
  AdmitReceived(const std::int64_t earliest_ns,
                const std::int64_t latest_ns) const {
    const std::int64_t earliest =
        received_.has_value() ? std::min(received_->first, earliest_ns)
                              : earliest_ns;
    const std::int64_t latest =
        received_.has_value() ? std::max(received_->second, latest_ns)
                              : latest_ns;
    if (latest - earliest > impl_->input_.maximum_receiver_time_skew_ns) {
      return core::Result<void>::Err(core::ErrorDTO{
          core::kErrorInvalidRequest,
          "Observed-state sources of one coherent capture exceed the "
          "retained coherence budget",
          {{"reason", "kinematic_receiver_time_skew_exceeded"}}});
    }
    received_.emplace(earliest, latest);
    return core::Result<void>::Ok();
  }

  /// Builds one frame-path view from this capture and admits its readings.
  [[nodiscard]] core::Result<core::KinematicStatePathViewDTO>
  View(const core::KinematicStatePathQueryDTO &query) const {
    auto view = views_->current_kinematic_state_path(query, at_ns_);
    if (!view.ok() || !view.value().earliest_received_timestamp_ns.has_value() ||
        !view.value().latest_received_timestamp_ns.has_value()) {
      return view;
    }
    if (auto admitted =
            AdmitReceived(*view.value().earliest_received_timestamp_ns,
                          *view.value().latest_received_timestamp_ns);
        !admitted.ok()) {
      return core::Result<core::KinematicStatePathViewDTO>::Err(
          admitted.error());
    }
    return view;
  }

  [[nodiscard]] core::Result<core::ObservedStateSnapshotDTO>
  current_state(const core::ObservedStateQueryContextDTO &,
                std::int64_t) const override {
    return core::Result<core::ObservedStateSnapshotDTO>::Err(
        resolution_error<core::ObservedStateSnapshotDTO>(
            "coherent_state_capture_read_unsupported",
            "a coherent frame capture serves only time-bound publications")
            .error());
  }

  [[nodiscard]] core::Result<core::ObservedStatePublicationDTO>
  publication(const core::ControlBindingId &,
              const core::ControlBindingRevision &) const override {
    return core::Result<core::ObservedStatePublicationDTO>::Err(
        resolution_error<core::ObservedStatePublicationDTO>(
            "coherent_state_capture_read_unsupported",
            "a coherent frame capture serves only time-bound publications")
            .error());
  }

  [[nodiscard]] core::Result<core::ObservedStatePublicationDTO>
  publication_at(const core::ControlBindingId &control_binding_id,
                 const core::ControlBindingRevision &control_binding_revision,
                 const std::int64_t now_ns) const override {
    using PublicationResult = core::Result<core::ObservedStatePublicationDTO>;
    if (now_ns != at_ns_) {
      return PublicationResult::Err(
          resolution_error<core::ObservedStatePublicationDTO>(
              "coherent_state_capture_time_mismatch",
              "a coherent frame capture is read at its own evaluation time")
              .error());
    }
    const SourceKey key{control_binding_id, control_binding_revision};
    if (const auto found = publications_.find(key);
        found != publications_.end()) {
      return found->second;
    }
    const auto *source = impl_->FindSource(key);
    if (source == nullptr) {
      return PublicationResult::Err(
          resolution_error<core::ObservedStatePublicationDTO>(
              "kinematic_state_source_unavailable",
              "Required observed-state source is unavailable")
              .error());
    }
    return publications_
        .emplace(key, source->provider->publication_at(
                          control_binding_id, control_binding_revision, at_ns_))
        .first->second;
  }

private:
  std::shared_ptr<const Impl> impl_;
  std::int64_t at_ns_;
  std::unique_ptr<core::ImmutableKinematicStateViewProvider> views_;
  mutable std::map<SourceKey, core::Result<core::ObservedStatePublicationDTO>>
      publications_;
  mutable std::optional<std::pair<std::int64_t, std::int64_t>> received_;
};

std::shared_ptr<RetainedSceneFrameResolver> RetainedSceneFrameResolver::create(
    collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
    RetainedSceneFrameResolverInput input) {
  if (!shared_model || input.retained_bundle_id.empty() ||
      input.graph_revision.empty() || input.cell_id.empty() ||
      input.compiled_cell_frame_name.empty() || input.frame_paths.empty() ||
      input.maximum_receiver_time_skew_ns < 0) {
    throw std::invalid_argument(
        "retained scene-frame resolver input is incomplete");
  }
  std::sort(input.target_frames.begin(), input.target_frames.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id, left.target_frame_role_id,
                              left.frame.model_instance_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id,
                              right.target_frame_role_id,
                              right.frame.model_instance_id);
            });
  for (std::size_t index = 0U; index < input.target_frames.size(); ++index) {
    const auto &target = input.target_frames[index];
    // B2: every target row names a frame of its own component.
    if (target.controllable_component_id.empty() ||
        target.command_target_id.empty() ||
        target.target_frame_role_id.empty() ||
        target.frame.controllable_component_id !=
            target.controllable_component_id ||
        target.frame.model_instance_id.empty() ||
        target.frame.frame_role_id.empty() ||
        target.frame.compiled_frame_name.empty() ||
        target.frame.model_revision.empty() ||
        !collision::ManyForgeCollisionEngine::retained_frame_exists(
            shared_model, target.frame.compiled_frame_name)) {
      throw std::invalid_argument("retained scene-frame target map is invalid");
    }
    if (index != 0U) {
      const auto &previous = input.target_frames[index - 1U];
      if (previous.controllable_component_id ==
              target.controllable_component_id &&
          previous.command_target_id == target.command_target_id &&
          previous.target_frame_role_id == target.target_frame_role_id) {
        throw std::invalid_argument(
            "retained scene-frame target role is ambiguous");
      }
    }
  }
  std::sort(
      input.component_attachments.begin(), input.component_attachments.end(),
      [](const auto &left, const auto &right) {
        return left.component_attachment_id < right.component_attachment_id;
      });
  for (std::size_t index = 0U; index < input.component_attachments.size();
       ++index) {
    const auto &attachment = input.component_attachments[index];
    if (attachment.component_attachment_id.empty() ||
        attachment.component_attachment_revision.empty() ||
        (index != 0U &&
         input.component_attachments[index - 1U].component_attachment_id ==
             attachment.component_attachment_id)) {
      throw std::invalid_argument(
          "retained scene-frame component attachment map is invalid");
    }
  }

  std::vector<Impl::Source> sources;
  for (auto &input_source : input.state_sources) {
    if (!input_source.provider) {
      throw std::invalid_argument(
          "retained scene-frame state provider is missing");
    }
    sources.push_back({input_source.context, input_source.publication_source,
                       std::move(input_source.provider)});
  }
  std::sort(sources.begin(), sources.end(),
            [](const auto &left, const auto &right) {
              return std::tie(left.context.control_binding_id,
                              left.context.control_binding_revision) <
                     std::tie(right.context.control_binding_id,
                              right.context.control_binding_revision);
            });
  for (std::size_t index = 1U; index < sources.size(); ++index) {
    if (sources[index - 1U].context.control_binding_id ==
            sources[index].context.control_binding_id &&
        sources[index - 1U].context.control_binding_revision ==
            sources[index].context.control_binding_revision) {
      throw std::invalid_argument(
          "retained scene-frame state source is duplicated");
    }
  }
  return std::shared_ptr<RetainedSceneFrameResolver>(
      new RetainedSceneFrameResolver(std::make_shared<Impl>(
          std::move(shared_model), std::move(input), std::move(sources))));
}

RetainedSceneFrameResolver::RetainedSceneFrameResolver(
    std::shared_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

RetainedSceneFrameResolver::~RetainedSceneFrameResolver() = default;

bool RetainedSceneFrameResolver::matches_structural_revision(
    const core::RetainedBundleId &retained_bundle_id,
    const core::GraphRevision &graph_revision) const noexcept {
  return impl_ != nullptr &&
         impl_->input_.retained_bundle_id == retained_bundle_id &&
         impl_->input_.graph_revision == graph_revision;
}

core::Result<core::ProgramFrameResolutionContextDTO>
RetainedSceneFrameResolver::capture_program_frame_resolution_context(
    core::ProgramFrameResolutionCaptureDTO capture) const {
  const auto fail = [](std::string reason, std::string message) {
    return resolution_error<core::ProgramFrameResolutionContextDTO>(
        std::move(reason), std::move(message));
  };
  if (capture.captured_at_ns < 0 || !capture.logical_scene ||
      !capture.logical_scene->locally_valid() ||
      !capture.projection_binding.valid() ||
      capture.binding.retained_bundle_id != impl_->input_.retained_bundle_id ||
      capture.binding.graph_revision != impl_->input_.graph_revision ||
      capture.projection_binding.projection_revision.retained_bundle_id !=
          impl_->input_.retained_bundle_id ||
      capture.projection_binding.projection_revision.graph_revision !=
          impl_->input_.graph_revision ||
      capture.logical_scene->revision !=
          capture.projection_binding.projection_revision.scene_revision ||
      capture.logical_scene->cell_id != impl_->input_.cell_id) {
    return fail(
        "program_reference_capture_identity_mismatch",
        "Program reference capture differs from the retained projection");
  }
  if (auto valid = core::validate_controlled_state(capture.start_state);
      !valid.ok()) {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        valid.error());
  }
  if (auto valid = core::validate_observed_state_publication(
          capture.observed_start_publication, capture.captured_at_ns);
      !valid.ok()) {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        valid.error());
  }
  if (!publication_matches_controlled_start(capture.observed_start_publication,
                                            capture.start_state) ||
      capture.start_state.controllable_component_id !=
          capture.binding.controllable_component_id ||
      capture.start_state.command_target_id !=
          capture.binding.command_target_id ||
      capture.start_state.control_binding_id !=
          capture.binding.control_binding_id ||
      capture.start_state.control_binding_revision !=
          capture.binding.control_binding_revision ||
      capture.start_state.resources != capture.binding.resources ||
      capture.observed_start_publication.provider_instance_id !=
          capture.binding.provider_instance_id ||
      capture.observed_start_publication.provider_manifest_id !=
          capture.binding.provider_manifest_id ||
      capture.observed_start_publication.provider_manifest_revision !=
          capture.binding.provider_manifest_revision ||
      capture.observed_start_publication.runtime_epoch !=
          capture.binding.provider_runtime_epoch) {
    return fail("program_reference_start_state_mismatch",
                "Program reference capture does not contain the selected start "
                "publication");
  }

  std::sort(capture.runtime_object_bindings.begin(),
            capture.runtime_object_bindings.end(),
            [](const auto &left, const auto &right) {
              return left.runtime_object_id < right.runtime_object_id;
            });
  for (std::size_t index = 0U; index < capture.runtime_object_bindings.size();
       ++index) {
    const auto &binding = capture.runtime_object_bindings[index];
    if (binding.runtime_object_id.empty() ||
        binding.scene_resource_id.empty() ||
        find_scene_object(*capture.logical_scene, binding.scene_resource_id) ==
            nullptr ||
        (index != 0U &&
         capture.runtime_object_bindings[index - 1U].runtime_object_id ==
             binding.runtime_object_id)) {
      constexpr const char *kMessage =
          "Runtime-object reference binding is missing, duplicated, or stale";
      // The failing binding is known by its runtime-object key; a binding
      // without one names nothing.
      if (binding.runtime_object_id.empty()) {
        return fail("runtime_object_binding_invalid", kMessage);
      }
      return resolution_error<core::ProgramFrameResolutionContextDTO>(
          "runtime_object_binding_invalid", kMessage,
          runtime_object_identity(binding.runtime_object_id));
    }
  }

  struct CapturedPath final {
    core::KinematicFramePathDTO path;
    core::KinematicStatePathViewDTO view;
  };
  std::vector<CapturedPath> captured_paths;
  // Every requested frame path is read from one coherent capture, which also
  // holds the selected start publication.
  const auto state = StateCapture::Create(impl_, capture.captured_at_ns);
  if (auto admitted = state->AdmitReceived(
          capture.observed_start_publication.received_timestamp_ns,
          capture.observed_start_publication.received_timestamp_ns);
      !admitted.ok()) {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        admitted.error());
  }
  // Finding a frame's retained path is about `owner`, the reference the
  // frame belongs to; reading its state is about the capture and names none.
  const auto capture_resolved_frame =
      [&](const core::ResolvedFrameDTO &frame,
          const ReferenceIdentity &owner) -> core::Result<void> {
    if (const auto *cell = std::get_if<core::ResolvedCellFrameDTO>(&frame);
        cell != nullptr) {
      if (cell->cell_id != impl_->input_.cell_id ||
          cell->compiled_frame_name != impl_->input_.compiled_cell_frame_name) {
        return core::Result<void>::Err(
            resolution_error<core::Pose3DTO>(
                "program_reference_cell_frame_mismatch",
                "Program reference names a foreign cell frame", owner)
                .error());
      }
      return core::Result<void>::Ok();
    }
    const core::KinematicFramePathDTO *selected = nullptr;
    for (const auto &path : impl_->input_.frame_paths) {
      if (path.terminal_frame != frame) {
        continue;
      }
      if (selected != nullptr) {
        return core::Result<void>::Err(
            ambiguous_resolution_error<core::Pose3DTO>(
                "program_reference_frame_path_ambiguous",
                "Program reference maps to multiple retained frame paths",
                owner)
                .error());
      }
      selected = &path;
    }
    if (selected == nullptr) {
      return core::Result<void>::Err(
          resolution_error<core::Pose3DTO>(
              "program_reference_frame_path_missing",
              "Program reference has no retained compiler frame path", owner)
              .error());
    }
    if (std::any_of(
            captured_paths.begin(), captured_paths.end(),
            [&](const auto &existing) { return existing.path == *selected; })) {
      return core::Result<void>::Ok();
    }
    auto query = impl_->BuildQuery(*selected, capture.captured_at_ns, *state);
    if (!query.ok()) {
      return core::Result<void>::Err(query.error());
    }
    auto view = state->View(query.value());
    if (!view.ok()) {
      return core::Result<void>::Err(view.error());
    }
    if (!state_view_agrees_with_primary_publication(
            view.value(), capture.observed_start_publication)) {
      return core::Result<void>::Err(
          fail("program_reference_state_capture_conflict",
               "Frame-path state differs from the selected operation start "
               "publication")
              .error());
    }
    captured_paths.push_back({*selected, std::move(view.value())});
    return core::Result<void>::Ok();
  };

  const auto frame_for_role = [&](const core::FrameRoleId &role,
                                  const ReferenceIdentity &about)
      -> core::Result<core::ResolvedFrameDTO> {
    const core::ProgramFrameRoleBindingDTO *selected = nullptr;
    for (const auto &candidate : capture.binding.frame_role_bindings) {
      if (candidate.frame_role_id != role) {
        continue;
      }
      if (selected != nullptr) {
        return ambiguous_resolution_error<core::ResolvedFrameDTO>(
            "program_frame_role_ambiguous",
            "Program frame role maps to multiple retained frames", about);
      }
      selected = &candidate;
    }
    if (selected == nullptr) {
      return resolution_error<core::ResolvedFrameDTO>(
          "program_frame_role_missing",
          "Program frame role is absent from the exact binding", about);
    }
    return core::Result<core::ResolvedFrameDTO>::Ok(program_binding_frame(
        impl_->input_.model_owners, capture.binding.controllable_component_id,
        *selected));
  };

  const auto capture_carrier_for_resource =
      [&](const core::SceneResourceId &resource_id,
          const ReferenceIdentity &about) -> core::Result<void> {
    const auto *object = find_scene_object(*capture.logical_scene, resource_id);
    if (object == nullptr) {
      return core::Result<void>::Err(
          resolution_error<core::Pose3DTO>(
              "program_scene_resource_missing",
              "Program reference names a scene resource absent from the "
              "retained snapshot",
              about)
              .error());
    }
    const auto *carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&object->placement);
    if (carried == nullptr) {
      return core::Result<void>::Ok();
    }
    const auto *attachment = find_attachment(
        *capture.logical_scene, carried->carried_object_attachment_id);
    if (attachment == nullptr ||
        attachment->carried_object_attachment_revision !=
            carried->carried_object_attachment_revision ||
        attachment->scene_resource_id != resource_id) {
      return core::Result<void>::Err(
          resolution_error<core::Pose3DTO>(
              "program_scene_attachment_mismatch",
              "Carried scene resource has no exact retained attachment", about)
              .error());
    }
    if (auto local =
            impl_->RequireLocalCarrier(attachment->carrier_frame_ref, about);
        !local.ok()) {
      return local;
    }
    const core::KinematicFramePathDTO *selected = nullptr;
    for (const auto &path : impl_->input_.frame_paths) {
      if (!reference_matches_frame(attachment->carrier_frame_ref,
                                   path.terminal_frame)) {
        continue;
      }
      if (selected != nullptr) {
        return core::Result<void>::Err(
            ambiguous_resolution_error<core::Pose3DTO>(
                "program_scene_carrier_ambiguous",
                "Carried scene resource maps to multiple carrier paths", about)
                .error());
      }
      selected = &path;
    }
    if (selected == nullptr) {
      return core::Result<void>::Err(
          resolution_error<core::Pose3DTO>(
              "program_scene_carrier_missing",
              "Carried scene resource has no retained carrier path", about)
              .error());
    }
    return capture_resolved_frame(selected->terminal_frame, about);
  };

  if (auto base = capture_resolved_frame(capture.binding.base_frame,
                                         base_frame_identity(capture.binding));
      !base.ok()) {
    return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
        base.error());
  }
  // Each lookup of a requested reference names that reference when it
  // fails; a capture or context failure met on the way names none.
  const auto capture_reference =
      [&](const core::ReferenceDTO &reference) -> core::Result<void> {
    const auto about = reference_identity(reference, impl_->input_.cell_id);
    switch (reference.type) {
    case core::ReferenceDTO::Type::kFrameRole: {
      const auto &role = std::get<core::FrameRoleId>(reference.id);
      auto frame = reference.target.has_value()
                       ? impl_->RobotFrame(*reference.target, role, about)
                       : frame_for_role(role, about);
      if (!frame.ok()) {
        return core::Result<void>::Err(frame.error());
      }
      return capture_resolved_frame(frame.value(), about);
    }
    case core::ReferenceDTO::Type::kComponentBodyRole:
    case core::ReferenceDTO::Type::kPose:
    case core::ReferenceDTO::Type::kCellFrame:
      return core::Result<void>::Ok();
    case core::ReferenceDTO::Type::kSceneResource:
      return capture_carrier_for_resource(
          std::get<core::SceneResourceId>(reference.id), about);
    case core::ReferenceDTO::Type::kRuntimeObject: {
      const auto &runtime_id = std::get<core::RuntimeObjectId>(reference.id);
      const auto selected =
          std::lower_bound(capture.runtime_object_bindings.begin(),
                           capture.runtime_object_bindings.end(), runtime_id,
                           [](const auto &candidate, const auto &id) {
                             return candidate.runtime_object_id < id;
                           });
      if (selected == capture.runtime_object_bindings.end() ||
          selected->runtime_object_id != runtime_id) {
        return core::Result<void>::Err(
            resolution_error<core::Pose3DTO>(
                "program_runtime_object_missing",
                "Program runtime-object key has no admission binding", about)
                .error());
      }
      return capture_carrier_for_resource(selected->scene_resource_id, about);
    }
    }
    return core::Result<void>::Ok();
  };
  for (const auto &reference : capture.requested_references) {
    if (!core::reference_identity_matches_type(reference) ||
        (reference.type == core::ReferenceDTO::Type::kPose) !=
            reference.pose.has_value()) {
      return fail(
          "program_reference_shape_invalid",
          "Program reference has an invalid typed identity or pose payload");
    }
    if (auto captured = capture_reference(reference); !captured.ok()) {
      return core::Result<core::ProgramFrameResolutionContextDTO>::Err(
          captured.error());
    }
  }

  std::sort(captured_paths.begin(), captured_paths.end(),
            [](const auto &left, const auto &right) {
              return core::compiled_frame_name(left.path.terminal_frame) <
                     core::compiled_frame_name(right.path.terminal_frame);
            });
  for (std::size_t index = 1U; index < captured_paths.size(); ++index) {
    if (core::compiled_frame_name(
            captured_paths[index - 1U].path.terminal_frame) ==
        core::compiled_frame_name(captured_paths[index].path.terminal_frame)) {
      return fail("program_reference_compiled_frame_ambiguous",
                  "Different retained frames share one compiled frame name");
    }
  }
  std::vector<core::KinematicFramePathDTO> paths;
  std::vector<core::KinematicStatePathViewDTO> views;
  paths.reserve(captured_paths.size());
  views.reserve(captured_paths.size());
  for (auto &captured : captured_paths) {
    paths.push_back(std::move(captured.path));
    views.push_back(std::move(captured.view));
  }
  core::ProgramFrameResolutionContextDTO context{
      std::move(capture.binding),
      std::move(capture.start_state),
      std::move(capture.observed_start_publication),
      capture.projection_binding,
      std::move(capture.logical_scene),
      std::move(capture.runtime_object_bindings),
      std::move(paths),
      std::move(views),
      capture.captured_at_ns};
  if (!context.valid()) {
    return fail(
        "program_reference_context_invalid",
        "Captured Program reference context is internally inconsistent");
  }
  return core::Result<core::ProgramFrameResolutionContextDTO>::Ok(
      std::move(context));
}

core::Result<core::Pose3DTO> RetainedSceneFrameResolver::resolve_reference_pose(
    const core::ReferenceDTO &reference,
    const core::ProgramFrameResolutionContextDTO &context) const {
  if (!context.valid() ||
      context.binding.retained_bundle_id != impl_->input_.retained_bundle_id ||
      context.binding.graph_revision != impl_->input_.graph_revision ||
      context.logical_scene->cell_id != impl_->input_.cell_id ||
      !core::reference_identity_matches_type(reference) ||
      (reference.type == core::ReferenceDTO::Type::kPose) !=
          reference.pose.has_value()) {
    return resolution_error<core::Pose3DTO>(
        "program_reference_context_mismatch",
        "Program reference differs from its immutable retained context");
  }
  if (reference.type == core::ReferenceDTO::Type::kPose) {
    return core::Result<core::Pose3DTO>::Ok(*reference.pose);
  }
  // Each lookup of the requested reference names it when it fails; the base
  // frame's lookup names the base frame. A context or state failure met on
  // the way names no reference (spec 210 §4.4).
  const auto about = reference_identity(reference, impl_->input_.cell_id);

  // Finding a frame's captured view is about `owner`, the reference the
  // frame belongs to; evaluating the view names none.
  const auto pose_in_cell_for_frame =
      [&](const core::ResolvedFrameDTO &frame,
          const ReferenceIdentity &owner) -> core::Result<core::Pose3DTO> {
    if (const auto *cell = std::get_if<core::ResolvedCellFrameDTO>(&frame);
        cell != nullptr) {
      if (cell->cell_id == impl_->input_.cell_id &&
          cell->compiled_frame_name == impl_->input_.compiled_cell_frame_name) {
        return core::Result<core::Pose3DTO>::Ok(core::Pose3DTO{});
      }
      return resolution_error<core::Pose3DTO>(
          "program_reference_cell_frame_mismatch",
          "Program reference names a foreign cell frame", owner);
    }
    const auto found = std::lower_bound(
        context.kinematic_paths.begin(), context.kinematic_paths.end(),
        core::compiled_frame_name(frame),
        [](const auto &candidate, const std::string &compiled_name) {
          return core::compiled_frame_name(candidate.terminal_frame) <
                 compiled_name;
        });
    if (found == context.kinematic_paths.end() ||
        found->terminal_frame != frame) {
      return resolution_error<core::Pose3DTO>(
          "program_reference_frame_view_missing",
          "Program reference has no captured immutable frame view", owner);
    }
    const auto index = static_cast<std::size_t>(
        std::distance(context.kinematic_paths.begin(), found));
    auto scene_reference = scene_reference_for(frame);
    if (!scene_reference.has_value() ||
        index >= context.kinematic_state_path_views.size()) {
      return resolution_error<core::Pose3DTO>(
          "program_reference_frame_view_invalid",
          "Program reference frame view is internally inconsistent");
    }
    auto resolved = ResolveFrameWithCandidateState(
        *scene_reference,
        core::SceneFrameResolutionContextDTO{
            context.projection_binding, context.logical_scene, *found,
            context.kinematic_state_path_views[index]},
        nullptr);
    if (!resolved.ok()) {
      return core::Result<core::Pose3DTO>::Err(resolved.error());
    }
    return core::Result<core::Pose3DTO>::Ok(
        resolved.value().frame_pose_in_cell);
  };

  auto base_in_cell = pose_in_cell_for_frame(
      context.binding.base_frame, base_frame_identity(context.binding));
  if (!base_in_cell.ok()) {
    return base_in_cell;
  }

  const auto frame_for_role = [&](const core::FrameRoleId &role)
      -> core::Result<core::ResolvedFrameDTO> {
    const core::ProgramFrameRoleBindingDTO *selected = nullptr;
    for (const auto &candidate : context.binding.frame_role_bindings) {
      if (candidate.frame_role_id != role) {
        continue;
      }
      if (selected != nullptr) {
        return ambiguous_resolution_error<core::ResolvedFrameDTO>(
            "program_frame_role_ambiguous",
            "Program frame role maps to multiple retained frames", about);
      }
      selected = &candidate;
    }
    if (selected == nullptr) {
      return resolution_error<core::ResolvedFrameDTO>(
          "program_frame_role_missing",
          "Program frame role is absent from the exact binding", about);
    }
    return core::Result<core::ResolvedFrameDTO>::Ok(program_binding_frame(
        impl_->input_.model_owners, context.binding.controllable_component_id,
        *selected));
  };

  const auto start_coordinates =
      [&]() -> core::Result<
                std::vector<collision::RetainedKinematicCoordinateValue>> {
    if (context.start_state.joint_names.size() !=
        context.start_state.positions.size()) {
      return resolution_error<
          std::vector<collision::RetainedKinematicCoordinateValue>>(
          "program_reference_start_state_shape_mismatch",
          "Program start state has mismatched coordinate names and values");
    }
    std::vector<collision::RetainedKinematicCoordinateValue> coordinates;
    coordinates.reserve(context.start_state.joint_names.size());
    for (std::size_t index = 0U; index < context.start_state.joint_names.size();
         ++index) {
      coordinates.push_back(
          {context.start_state.joint_names[index],
           position_value(context.start_state.positions[index])});
    }
    return core::
        Result<std::vector<collision::RetainedKinematicCoordinateValue>>::Ok(
            std::move(coordinates));
  };

  const auto scene_resource_pose = [&](const core::SceneResourceId &resource_id)
      -> core::Result<core::Pose3DTO> {
    const auto *object = find_scene_object(*context.logical_scene, resource_id);
    if (object == nullptr) {
      return resolution_error<core::Pose3DTO>(
          "program_scene_resource_missing",
          "Program reference names a scene resource absent from the retained "
          "snapshot",
          about);
    }
    if (const auto *fixed = std::get_if<core::CellFixedSceneObjectPlacementDTO>(
            &object->placement)) {
      if (fixed->cell_id != impl_->input_.cell_id) {
        return resolution_error<core::Pose3DTO>(
            "program_scene_resource_cell_mismatch",
            "Scene resource is fixed in a foreign cell", about);
      }
      return core::Result<core::Pose3DTO>::Ok(fixed->pose_in_cell);
    }
    const auto &carried =
        std::get<core::CarriedSceneObjectPlacementDTO>(object->placement);
    const auto *attachment = find_attachment(
        *context.logical_scene, carried.carried_object_attachment_id);
    if (attachment == nullptr ||
        attachment->carried_object_attachment_revision !=
            carried.carried_object_attachment_revision ||
        attachment->scene_resource_id != resource_id) {
      return resolution_error<core::Pose3DTO>(
          "program_scene_attachment_mismatch",
          "Carried scene resource has no exact retained attachment", about);
    }
    const core::ResolvedFrameDTO *carrier = nullptr;
    for (const auto &path : context.kinematic_paths) {
      if (!reference_matches_frame(attachment->carrier_frame_ref,
                                   path.terminal_frame)) {
        continue;
      }
      if (carrier != nullptr) {
        return ambiguous_resolution_error<core::Pose3DTO>(
            "program_scene_carrier_ambiguous",
            "Carried scene resource maps to multiple captured carrier frames",
            about);
      }
      carrier = &path.terminal_frame;
    }
    if (carrier == nullptr) {
      return resolution_error<core::Pose3DTO>(
          "program_scene_carrier_missing",
          "Carried scene resource has no captured carrier frame", about);
    }
    auto carrier_pose = pose_in_cell_for_frame(*carrier, about);
    if (!carrier_pose.ok()) {
      return carrier_pose;
    }
    return core::Result<core::Pose3DTO>::Ok(core::multiply_poses(
        carrier_pose.value(), attachment->pose_in_carrier_frame));
  };

  core::Result<core::Pose3DTO> target_in_cell =
      resolution_error<core::Pose3DTO>("program_reference_type_invalid",
                                       "Program reference type is invalid");
  switch (reference.type) {
  case core::ReferenceDTO::Type::kFrameRole: {
    const auto &role = std::get<core::FrameRoleId>(reference.id);
    auto frame = reference.target.has_value()
                     ? impl_->RobotFrame(*reference.target, role, about)
                     : frame_for_role(role);
    if (!frame.ok()) {
      return core::Result<core::Pose3DTO>::Err(frame.error());
    }
    target_in_cell = pose_in_cell_for_frame(frame.value(), about);
    break;
  }
  case core::ReferenceDTO::Type::kCellFrame:
    target_in_cell = core::Result<core::Pose3DTO>::Ok(core::Pose3DTO{});
    break;
  case core::ReferenceDTO::Type::kComponentBodyRole: {
    const auto &role = std::get<core::CollisionBodyRoleId>(reference.id);
    const core::ProgramCollisionBodyRoleBindingDTO *selected = nullptr;
    for (const auto &candidate : context.binding.collision_body_role_bindings) {
      if (candidate.collision_body_role_id != role) {
        continue;
      }
      if (selected != nullptr) {
        return ambiguous_resolution_error<core::Pose3DTO>(
            "program_component_body_role_ambiguous",
            "Program component-body role maps to multiple retained bodies",
            about);
      }
      selected = &candidate;
    }
    if (selected == nullptr ||
        selected->compiled_collision_body_names.empty()) {
      return resolution_error<core::Pose3DTO>(
          "program_component_body_role_missing",
          "Program component-body role is absent from the exact binding",
          about);
    }
    if (selected->compiled_collision_body_names.size() != 1U) {
      return ambiguous_resolution_error<core::Pose3DTO>(
          "program_component_body_role_spatially_ambiguous",
          "Program component-body role does not identify one spatial body",
          about);
    }
    auto coordinates = start_coordinates();
    if (!coordinates.ok()) {
      return core::Result<core::Pose3DTO>::Err(coordinates.error());
    }
    target_in_cell =
        collision::ManyForgeCollisionEngine::resolve_retained_frame_pose(
            impl_->model_, impl_->input_.compiled_cell_frame_name,
            selected->compiled_collision_body_names.front(),
            coordinates.value());
    break;
  }
  case core::ReferenceDTO::Type::kSceneResource:
    target_in_cell =
        scene_resource_pose(std::get<core::SceneResourceId>(reference.id));
    break;
  case core::ReferenceDTO::Type::kRuntimeObject: {
    const auto &runtime_id = std::get<core::RuntimeObjectId>(reference.id);
    const auto selected =
        std::lower_bound(context.runtime_object_bindings.begin(),
                         context.runtime_object_bindings.end(), runtime_id,
                         [](const auto &candidate, const auto &id) {
                           return candidate.runtime_object_id < id;
                         });
    if (selected == context.runtime_object_bindings.end() ||
        selected->runtime_object_id != runtime_id) {
      return resolution_error<core::Pose3DTO>(
          "program_runtime_object_missing",
          "Program runtime-object key has no admission binding", about);
    }
    target_in_cell = scene_resource_pose(selected->scene_resource_id);
    break;
  }
  case core::ReferenceDTO::Type::kPose:
    break;
  }
  if (!target_in_cell.ok()) {
    return target_in_cell;
  }
  return core::Result<core::Pose3DTO>::Ok(core::multiply_poses(
      core::inverse_pose(base_in_cell.value()), target_in_cell.value()));
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedSceneFrameResolver::resolve_scene_frame(
    const core::SceneFrameRefDTO &reference,
    const core::SceneFrameResolutionContextDTO &context) const {
  // The frame's retained path is already chosen by its context: every
  // failure here is about the context or its state, and names no reference.
  return ResolveFrameWithCandidateState(reference, context, nullptr);
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedSceneFrameResolver::ResolveFrameWithCandidateState(
    const core::SceneFrameRefDTO &reference,
    const core::SceneFrameResolutionContextDTO &context,
    const core::ControlledStateDTO *predicted_state) const {
  if (!context.valid() ||
      !reference_matches_frame(reference,
                               context.kinematic_path.terminal_frame)) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_context_mismatch",
        "scene-frame reference differs from its immutable retained context");
  }

  if (predicted_state != nullptr) {
    auto valid = impl_->ValidateCandidateState(*predicted_state);
    if (!valid.ok()) {
      return core::Result<core::ResolvedSceneFramePoseDTO>::Err(valid.error());
    }
  }

  std::set<core::ResourceId> predicted_resources_used;
  std::vector<collision::RetainedKinematicCoordinateValue> coordinates;
  for (const auto &model_state :
       context.kinematic_state_path_view.model_states) {
    if (model_state.coordinate_projection.size() !=
        model_state.positions.size()) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "kinematic_state_view_shape_mismatch",
          "scene-frame state view has mismatched model coordinates");
    }
    for (std::size_t index = 0U;
         index < model_state.coordinate_projection.size(); ++index) {
      const auto &projection = model_state.coordinate_projection[index];
      if (projection.derived_mimic) {
        continue;
      }
      const core::KinematicModelJointPathSegmentDTO *selected = nullptr;
      for (const auto &segment : context.kinematic_path.segments) {
        const auto *joint =
            std::get_if<core::KinematicModelJointPathSegmentDTO>(&segment);
        if (joint == nullptr || !joint->controllable_component_id.has_value() ||
            *joint->controllable_component_id !=
                model_state.controllable_component_id ||
            joint->model_instance_id != model_state.model_instance_id ||
            joint->model_revision != model_state.model_revision ||
            joint->composition_coordinate_index !=
                projection.composition_coordinate_index) {
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
            "kinematic_coordinate_missing",
            "state-view coordinate has no retained path joint");
      }
      auto projected_position = model_state.positions[index];
      if (predicted_state != nullptr) {
        const auto candidate_resource =
            std::find(predicted_state->resources.begin(),
                      predicted_state->resources.end(), projection.resource_id);
        if (candidate_resource != predicted_state->resources.end()) {
          const auto candidate_index = static_cast<std::size_t>(std::distance(
              predicted_state->resources.begin(), candidate_resource));
          const auto dependency = std::find_if(
              selected->eligible_resource_dependencies.begin(),
              selected->eligible_resource_dependencies.end(),
              [&](const auto &value) {
                return value.resource_id == projection.resource_id &&
                       value.control_binding_id ==
                           predicted_state->control_binding_id &&
                       value.control_binding_revision ==
                           predicted_state->control_binding_revision &&
                       value.coordinate_kind == projection.coordinate_kind &&
                       value.multiplier == projection.multiplier &&
                       value.offset == projection.offset &&
                       value.derived_mimic == projection.derived_mimic;
              });
          if (model_state.controllable_component_id !=
                  predicted_state->controllable_component_id ||
              dependency == selected->eligible_resource_dependencies.end() ||
              candidate_index >= predicted_state->positions.size() ||
              candidate_index >= predicted_state->joint_names.size() ||
              predicted_state->joint_names[candidate_index] !=
                  selected->compiled_joint_name ||
              core::coordinate_kind(
                  predicted_state->positions[candidate_index]) !=
                  projection.coordinate_kind) {
            return resolution_error<core::ResolvedSceneFramePoseDTO>(
                "candidate_state_path_mismatch",
                "predicted candidate state does not exactly map to the "
                "retained carrier-frame path");
          }
          const double projected =
              position_value(predicted_state->positions[candidate_index]) *
                  projection.multiplier +
              projection.offset;
          if (!std::isfinite(projected)) {
            return resolution_error<core::ResolvedSceneFramePoseDTO>(
                "candidate_state_projection_nonfinite",
                "predicted candidate state produced a non-finite model "
                "coordinate");
          }
          projected_position = [&]() -> core::CoordinatePositionDTO {
            if (projection.coordinate_kind == core::CoordinateKind::kAngular) {
              return core::AngularPositionDTO{projected};
            }
            return core::LinearPositionDTO{projected};
          }();
          predicted_resources_used.insert(projection.resource_id);
        }
      }
      coordinates.push_back(
          {selected->compiled_joint_name, position_value(projected_position)});
    }
  }
  if (predicted_state != nullptr && predicted_resources_used.empty()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "candidate_state_path_disjoint",
        "predicted candidate state does not influence the selected carrier "
        "frame");
  }
  auto pose = collision::ManyForgeCollisionEngine::resolve_retained_frame_pose(
      impl_->model_, impl_->input_.compiled_cell_frame_name,
      core::compiled_frame_name(context.kinematic_path.terminal_frame),
      coordinates);
  if (!pose.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(pose.error());
  }
  core::ResolvedSceneFramePoseDTO result{context.kinematic_path.terminal_frame,
                                         pose.value(),
                                         {},
                                         context.projection_binding,
                                         context.kinematic_path,
                                         context.kinematic_state_path_view,
                                         std::nullopt,
                                         core::ContentDigest(std::string{})};
  try {
    if (predicted_state != nullptr) {
      result.predicted_state_digest =
          core::ComputeCandidateStateEvidenceDigest(*predicted_state);
    }
    result.resolution_evidence_digest =
        core::ComputeSceneFrameResolutionEvidenceDigest(result);
  } catch (const std::exception &error) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_evidence_failed", error.what());
  }
  if (!result.valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_result_invalid",
        "scene-frame resolver produced an invalid immutable result");
  }
  return core::Result<core::ResolvedSceneFramePoseDTO>::Ok(std::move(result));
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedSceneFrameResolver::Resolve(
    const core::SceneFrameResolutionQueryDTO &query,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
    const std::int64_t now_ns) const {
  if (!query.valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_query_mismatch", "scene-frame query is invalid");
  }
  if (now_ns < 0) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_query_mismatch",
        "scene-frame query differs from the active retained projection");
  }
  return ResolveWithCandidateState(query.selector, query.projection_binding,
                                   std::move(logical_scene),
                                   *StateCapture::Create(impl_, now_ns),
                                   nullptr, nullptr);
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedSceneFrameResolver::ResolveCandidate(
    const core::CandidateSceneFrameResolutionQueryDTO &query,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
    const std::int64_t now_ns) const {
  if (!query.valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "candidate_scene_frame_query_mismatch",
        "candidate scene-frame query is invalid");
  }
  if (now_ns < 0) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_query_mismatch",
        "scene-frame query differs from the active retained projection");
  }
  return ResolveWithCandidateState(query.selector, query.projection_binding,
                                   std::move(logical_scene),
                                   *StateCapture::Create(impl_, now_ns),
                                   &query.predicted_state, nullptr);
}

core::Result<RetainedSceneFrameTransform>
RetainedSceneFrameResolver::ResolveFrameTransform(
    const RetainedSceneFrameTransformQuery &query,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
    const std::int64_t now_ns) const {
  using TransformResult = core::Result<RetainedSceneFrameTransform>;
  if (!query.projection_binding.valid() || !logical_scene ||
      !logical_scene->locally_valid() ||
      logical_scene->revision !=
          query.projection_binding.projection_revision.scene_revision ||
      query.projection_binding.projection_revision.retained_bundle_id !=
          impl_->input_.retained_bundle_id ||
      query.projection_binding.projection_revision.graph_revision !=
          impl_->input_.graph_revision ||
      logical_scene->cell_id != impl_->input_.cell_id || now_ns < 0) {
    return resolution_error<RetainedSceneFrameTransform>(
        "frame_transform_query_mismatch",
        "frame transform differs from the active retained projection");
  }
  const auto &pose = query.pose_in_source_frame;
  if (!std::isfinite(pose.position_m.x_m) ||
      !std::isfinite(pose.position_m.y_m) ||
      !std::isfinite(pose.position_m.z_m) ||
      !std::isfinite(pose.orientation.x) || !std::isfinite(pose.orientation.y) ||
      !std::isfinite(pose.orientation.z) || !std::isfinite(pose.orientation.w)) {
    return TransformResult::Err(
        core::ErrorDTO{core::kErrorInvalidRequest,
                       "frame transform pose must be finite",
                       {{"reason", "frame_transform_pose_invalid"}}});
  }
  std::vector<core::ContentDigest> evidence;
  // Source and target are resolved from one coherent capture: a robot crossed
  // by both frame paths contributes one publication to both.
  const auto state = StateCapture::Create(impl_, now_ns);
  // `reference` is the requested frame `selector` is resolved for.
  const auto resolve_frame =
      [&](const core::SceneFrameRefDTO &selector,
          const core::ReferenceDTO &reference) -> core::Result<core::Pose3DTO> {
    auto resolved =
        ResolveWithCandidateState(selector, query.projection_binding,
                                  logical_scene, *state, nullptr, &reference);
    if (!resolved.ok()) {
      return core::Result<core::Pose3DTO>::Err(resolved.error());
    }
    evidence.push_back(resolved.value().resolution_evidence_digest);
    return core::Result<core::Pose3DTO>::Ok(
        resolved.value().frame_pose_in_cell);
  };
  // Each lookup of a requested frame names that frame when it fails; a
  // context or state failure met on the way names none (spec 210 §4.4).
  const auto frame_in_cell =
      [&](const core::ReferenceDTO &reference) -> core::Result<core::Pose3DTO> {
    const auto about = reference_identity(reference, impl_->input_.cell_id);
    const auto unsupported = [&] {
      return core::Result<core::Pose3DTO>::Err(about_reference(
          core::ErrorDTO{
              core::kErrorInvalidRequest,
              "frame transform supports the cell frame, robot-qualified frame "
              "roles and scene resources",
              {{"reason", "frame_transform_reference_unsupported"}}},
          about));
    };
    if (!core::reference_identity_matches_type(reference) ||
        reference.pose.has_value()) {
      return unsupported();
    }
    switch (reference.type) {
    case core::ReferenceDTO::Type::kCellFrame:
      return core::Result<core::Pose3DTO>::Ok(core::Pose3DTO{});
    case core::ReferenceDTO::Type::kFrameRole: {
      if (!reference.target.has_value()) {
        return unsupported();
      }
      auto frame = impl_->RobotFrame(
          *reference.target, std::get<core::FrameRoleId>(reference.id), about);
      if (!frame.ok()) {
        return core::Result<core::Pose3DTO>::Err(frame.error());
      }
      const auto selector = scene_reference_for(frame.value());
      if (!selector.has_value()) {
        return unsupported();
      }
      return resolve_frame(*selector, reference);
    }
    case core::ReferenceDTO::Type::kSceneResource: {
      const auto &resource_id = std::get<core::SceneResourceId>(reference.id);
      const auto *object = find_scene_object(*logical_scene, resource_id);
      if (object == nullptr) {
        return resolution_error<core::Pose3DTO>(
            "program_scene_resource_missing",
            "frame transform names a scene resource absent from the retained "
            "snapshot",
            about);
      }
      if (const auto *fixed =
              std::get_if<core::CellFixedSceneObjectPlacementDTO>(
                  &object->placement)) {
        if (fixed->cell_id != impl_->input_.cell_id) {
          return resolution_error<core::Pose3DTO>(
              "program_scene_resource_cell_mismatch",
              "Scene resource is fixed in a foreign cell", about);
        }
        return core::Result<core::Pose3DTO>::Ok(fixed->pose_in_cell);
      }
      const auto &carried =
          std::get<core::CarriedSceneObjectPlacementDTO>(object->placement);
      const auto *attachment =
          find_attachment(*logical_scene, carried.carried_object_attachment_id);
      if (attachment == nullptr ||
          attachment->carried_object_attachment_revision !=
              carried.carried_object_attachment_revision ||
          attachment->scene_resource_id != resource_id) {
        return resolution_error<core::Pose3DTO>(
            "program_scene_attachment_mismatch",
            "Carried scene resource has no exact retained attachment", about);
      }
      if (auto local =
              impl_->RequireLocalCarrier(attachment->carrier_frame_ref, about);
          !local.ok()) {
        return core::Result<core::Pose3DTO>::Err(local.error());
      }
      auto carrier = resolve_frame(attachment->carrier_frame_ref, reference);
      if (!carrier.ok()) {
        return carrier;
      }
      return core::Result<core::Pose3DTO>::Ok(core::multiply_poses(
          carrier.value(), attachment->pose_in_carrier_frame));
    }
    case core::ReferenceDTO::Type::kComponentBodyRole:
    case core::ReferenceDTO::Type::kRuntimeObject:
    case core::ReferenceDTO::Type::kPose:
      return unsupported();
    }
    return unsupported();
  };

  auto source_in_cell = frame_in_cell(query.source_frame);
  if (!source_in_cell.ok()) {
    return TransformResult::Err(source_in_cell.error());
  }
  auto target_in_cell = frame_in_cell(query.target_frame);
  if (!target_in_cell.ok()) {
    return TransformResult::Err(target_in_cell.error());
  }
  std::sort(evidence.begin(), evidence.end());
  evidence.erase(std::unique(evidence.begin(), evidence.end()),
                 evidence.end());
  return TransformResult::Ok(RetainedSceneFrameTransform{
      core::multiply_poses(
          core::inverse_pose(target_in_cell.value()),
          core::multiply_poses(source_in_cell.value(),
                               query.pose_in_source_frame)),
      source_in_cell.value(), target_in_cell.value(), query.projection_binding,
      now_ns, std::move(evidence)});
}

core::Result<core::ResolvedSceneFramePoseDTO>
RetainedSceneFrameResolver::ResolveWithCandidateState(
    const core::SceneFrameResolutionSelectorDTO &selector,
    const core::ProjectionLeaseBindingDTO &projection_binding,
    std::shared_ptr<const core::LogicalSceneSnapshotDTO> logical_scene,
    const StateCapture &capture,
    const core::ControlledStateDTO *predicted_state,
    const core::ReferenceDTO *requested_reference) const {
  // Only finding the selected frame's retained path is about one reference:
  // the requested reference it is resolved for, else the selected frame.
  const auto about = [&] {
    return requested_reference != nullptr
               ? reference_identity(*requested_reference, impl_->input_.cell_id)
               : frame_identity(selector);
  };
  const std::int64_t now_ns = capture.at_ns();
  if (!core::SceneFrameResolutionQueryDTO{selector, projection_binding}
           .valid() ||
      !logical_scene || !logical_scene->locally_valid() ||
      logical_scene->revision !=
          projection_binding.projection_revision.scene_revision ||
      projection_binding.projection_revision.retained_bundle_id !=
          impl_->input_.retained_bundle_id ||
      projection_binding.projection_revision.graph_revision !=
          impl_->input_.graph_revision ||
      logical_scene->cell_id != impl_->input_.cell_id || now_ns < 0) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_query_mismatch",
        "scene-frame query differs from the active retained projection");
  }

  auto selected_reference = std::visit(
      [&](const auto &selector) -> core::Result<core::SceneFrameRefDTO> {
        using Selector = std::remove_cvref_t<decltype(selector)>;
        if constexpr (std::is_same_v<Selector, core::SceneFrameRefDTO>) {
          return core::Result<core::SceneFrameRefDTO>::Ok(selector);
        } else {
          const RetainedTargetSceneFrameInput *selected = nullptr;
          for (const auto &candidate : impl_->input_.target_frames) {
            if (candidate.controllable_component_id !=
                    selector.controllable_component_id ||
                candidate.command_target_id != selector.command_target_id ||
                candidate.target_frame_role_id != selector.frame_role_id) {
              continue;
            }
            if (selected != nullptr) {
              return resolution_error<core::SceneFrameRefDTO>(
                  "scene_frame_reference_ambiguous",
                  "target carrier frame is ambiguous in retained model map",
                  about());
            }
            selected = &candidate;
          }
          if (selected == nullptr) {
            return resolution_error<core::SceneFrameRefDTO>(
                "scene_frame_reference_missing",
                "target carrier frame is absent from retained model map",
                about());
          }
          return core::Result<core::SceneFrameRefDTO>::Ok(
              core::ComponentSceneFrameRefDTO{
                  selected->frame.controllable_component_id,
                  selected->frame.model_instance_id,
                  selected->frame.frame_role_id});
        }
      },
      selector);
  if (!selected_reference.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(
        selected_reference.error());
  }
  const auto &reference = selected_reference.value();

  const core::KinematicFramePathDTO *path = nullptr;
  for (const auto &candidate : impl_->input_.frame_paths) {
    if (!reference_matches_frame(reference, candidate.terminal_frame)) {
      continue;
    }
    if (path != nullptr) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "scene_frame_reference_ambiguous",
          "scene-frame reference maps to multiple retained paths", about());
    }
    path = &candidate;
  }
  if (path == nullptr) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_reference_missing",
        "scene-frame reference has no retained compiler path", about());
  }
  std::vector<core::ComponentAttachmentRevision> attachment_revisions;
  for (const auto &segment : path->segments) {
    const auto *attachment =
        std::get_if<core::KinematicComponentAttachmentPathSegmentDTO>(&segment);
    if (attachment == nullptr) {
      continue;
    }
    const auto mapped = std::lower_bound(
        impl_->input_.component_attachments.begin(),
        impl_->input_.component_attachments.end(),
        attachment->component_attachment_id,
        [](const auto &candidate, const core::ComponentAttachmentId &selected) {
          return candidate.component_attachment_id < selected;
        });
    if (mapped == impl_->input_.component_attachments.end() ||
        mapped->component_attachment_id !=
            attachment->component_attachment_id) {
      return resolution_error<core::ResolvedSceneFramePoseDTO>(
          "scene_frame_structural_revision_missing",
          "scene-frame path crosses an unmapped component attachment");
    }
    attachment_revisions.push_back(mapped->component_attachment_revision);
  }
  std::sort(attachment_revisions.begin(), attachment_revisions.end());
  attachment_revisions.erase(
      std::unique(attachment_revisions.begin(), attachment_revisions.end()),
      attachment_revisions.end());
  auto state_query = impl_->BuildQuery(*path, now_ns, capture);
  if (!state_query.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(
        state_query.error());
  }
  auto state_view = capture.View(state_query.value());
  if (!state_view.ok()) {
    return core::Result<core::ResolvedSceneFramePoseDTO>::Err(
        state_view.error());
  }
  auto result = ResolveFrameWithCandidateState(
      reference,
      core::SceneFrameResolutionContextDTO{projection_binding,
                                           std::move(logical_scene), *path,
                                           std::move(state_view.value())},
      predicted_state);
  if (!result.ok()) {
    return result;
  }
  result.value().component_attachment_revisions =
      std::move(attachment_revisions);
  try {
    result.value().resolution_evidence_digest =
        core::ComputeSceneFrameResolutionEvidenceDigest(result.value());
  } catch (const std::exception &error) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_evidence_failed", error.what());
  }
  if (!result.value().valid()) {
    return resolution_error<core::ResolvedSceneFramePoseDTO>(
        "scene_frame_result_invalid",
        "scene-frame structural evidence is invalid");
  }
  return result;
}

} // namespace manyforge::roboplan_backend

#include <manyforge_roboplan/retained_collision_world_projector.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <set>
#include <type_traits>

#include <Eigen/Geometry>

#include <manyforge_core/collision/collision_world_snapshot_builder.hpp>
#include <manyforge_core/logical_scene_identity.hpp>

namespace manyforge::roboplan_backend {
namespace {

core::Result<core::CollisionWorldSnapshot> projection_error(
    std::string message) {
  return core::Result<core::CollisionWorldSnapshot>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {},
  });
}

core::Result<core::CollisionPolicySnapshot> policy_projection_error(
    std::string message) {
  return core::Result<core::CollisionPolicySnapshot>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {},
  });
}

core::Pose3DTO identity_pose() {
  return {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0, 1.0}};
}

core::Pose3DTO compose_pose(const core::Pose3DTO& parent_from_frame,
                            const core::Pose3DTO& frame_from_object) {
  const auto quaternion = [](const core::QuaternionDTO& value) {
    Eigen::Quaterniond result(value.w, value.x, value.y, value.z);
    const double norm = result.norm();
    if (!std::isfinite(norm) || norm <= 0.0) {
      throw RetainedCollisionWorldProjectionError(
          "carried-object projection received an invalid quaternion");
    }
    return result.normalized();
  };
  const Eigen::Quaterniond parent_rotation =
      quaternion(parent_from_frame.orientation);
  const Eigen::Quaterniond object_rotation =
      quaternion(frame_from_object.orientation);
  const Eigen::Vector3d parent_translation(parent_from_frame.position_m.x_m,
                                           parent_from_frame.position_m.y_m,
                                           parent_from_frame.position_m.z_m);
  const Eigen::Vector3d object_translation(frame_from_object.position_m.x_m,
                                           frame_from_object.position_m.y_m,
                                           frame_from_object.position_m.z_m);
  const Eigen::Vector3d translation =
      parent_translation + parent_rotation * object_translation;
  const Eigen::Quaterniond rotation =
      (parent_rotation * object_rotation).normalized();
  return {
      {translation.x(), translation.y(), translation.z()},
      {rotation.x(), rotation.y(), rotation.z(), rotation.w()},
  };
}

const RetainedCollisionModelMapInput::Target* find_target(
    const RetainedCollisionModelMapInput& model_map,
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& command_target_id) {
  const auto found =
      std::lower_bound(model_map.targets.begin(), model_map.targets.end(),
                       std::tie(component_id, command_target_id),
                       [](const RetainedCollisionModelMapInput::Target& target,
                          const auto& key) {
                         return std::tie(target.controllable_component_id,
                                         target.command_target_id) < key;
                       });
  return found != model_map.targets.end() &&
                 found->controllable_component_id == component_id &&
                 found->command_target_id == command_target_id
             ? &*found
             : nullptr;
}

const collision::RetainedComponentModelFrameProjectionInput* find_carrier_frame(
    const std::vector<collision::RetainedComponentModelFrameProjectionInput>&
        frames,
    const core::ComponentSceneFrameRefDTO& carrier) {
  const auto found = std::find_if(
      frames.begin(), frames.end(), [&](const auto& frame) {
        return frame.controllable_component_id ==
                   carrier.controllable_component_id &&
               frame.model_instance_id == carrier.model_instance_id &&
               frame.frame_role_id == carrier.frame_role_id;
      });
  return found == frames.end() ? nullptr : &*found;
}

// The collision bodies an attachment's carrier frame and touch roles select:
// the holder target's, or (OD-372) those of the component mounted on the
// holder's robot that owns the carrier frame.
std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
carrier_bodies(const RetainedCollisionModelMapInput& model_map,
               const RetainedCollisionModelMapInput::Target& target,
               const core::ComponentSceneFrameRefDTO& carrier) {
  if (carrier.controllable_component_id == target.controllable_component_id) {
    return target.bodies;
  }
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput> bodies;
  std::copy_if(model_map.component_bodies.begin(),
               model_map.component_bodies.end(), std::back_inserter(bodies),
               [&](const auto& body) {
                 return body.controllable_component_id ==
                        carrier.controllable_component_id;
               });
  return bodies;
}

core::CollisionPairId canonical_pair(core::CollisionBodyId left,
                                     core::CollisionBodyId right) {
  if (right < left) {
    std::swap(left, right);
  }
  return {std::move(left), std::move(right)};
}

core::CollisionGeometryDescriptor scene_geometry_descriptor(
    const core::CollisionBodyId& body_id,
    const core::CollisionShapeDTO& shape) {
  core::CollisionGeometryDescriptor descriptor;
  descriptor.body_id = body_id;
  descriptor.owner_kind = core::CollisionOwnerKind::kSceneResource;
  descriptor.local_pose = identity_pose();
  descriptor.quality_labels = {"logical_scene_resource"};
  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::BoxShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kBox;
          descriptor.box_dimensions_m = value.dimensions_m;
          descriptor.quality_labels.push_back("primitive");
        } else if constexpr (std::is_same_v<T, core::SphereShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kSphere;
          descriptor.sphere_radius_m = value.radius_m;
          descriptor.quality_labels.push_back("primitive");
        } else if constexpr (std::is_same_v<T, core::CylinderShapeDTO>) {
          descriptor.geometry_kind = core::CollisionGeometryKind::kCylinder;
          descriptor.cylinder_radius_m = value.radius_m;
          descriptor.cylinder_height_m = value.height_m;
          descriptor.quality_labels.push_back("primitive");
        } else {
          descriptor.geometry_kind = core::CollisionGeometryKind::kTriangleMesh;
          descriptor.source_uri = value.resource_uri;
          // Consumers read the captured bytes by this digest only.
          descriptor.source_hash = value.content_digest;
          descriptor.mesh_scale = value.scale;
          descriptor.quality_labels.push_back("collision_mesh");
        }
      },
      shape);
  std::sort(descriptor.quality_labels.begin(), descriptor.quality_labels.end());
  return descriptor;
}

}  // namespace

CanonicalRetainedComponentBodies canonicalize_retained_component_bodies(
    const std::span<
        const collision::RetainedComponentCollisionBodyProjectionInput>
        bodies) {
  if (bodies.empty()) {
    throw RetainedCollisionWorldProjectionError(
        "retained collision projection has no component bodies");
  }
  using Input = collision::RetainedComponentCollisionBodyProjectionInput;
  struct Selected final {
    Input body;
    core::CollisionBodyId body_id;
  };
  std::map<std::string, Selected, std::less<>> selected_by_frame;
  std::set<core::CollisionBodyId> semantic_ids;
  for (const auto& body : bodies) {
    if (body.controllable_component_id.empty() ||
        body.model_instance_id.empty() || body.model_revision.empty() ||
        body.collision_body_role_id.empty() ||
        body.compiled_frame_name.empty()) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision-body alias input is incomplete");
    }
    const auto body_id = core::make_component_collision_body_id(
        body.controllable_component_id, body.model_instance_id,
        body.collision_body_role_id, body.collision_index);
    if (!semantic_ids.insert(body_id).second) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision-body semantic identity is duplicated");
    }
    const auto [selected, inserted] = selected_by_frame.emplace(
        body.compiled_frame_name, Selected{body, body_id});
    if (!inserted) {
      const auto& existing = selected->second.body;
      if (existing.controllable_component_id !=
              body.controllable_component_id ||
          existing.model_instance_id != body.model_instance_id ||
          existing.model_revision != body.model_revision) {
        throw RetainedCollisionWorldProjectionError(
            "one compiled collision frame crosses component/model identity");
      }
      if (body_id < selected->second.body_id) {
        selected->second = Selected{body, body_id};
      }
    }
  }

  CanonicalRetainedComponentBodies result;
  result.physical_bodies.reserve(selected_by_frame.size());
  for (const auto& [unused, selected] : selected_by_frame) {
    static_cast<void>(unused);
    result.physical_bodies.push_back(selected.body);
  }
  for (const auto& body : bodies) {
    const auto semantic = core::make_component_collision_body_id(
        body.controllable_component_id, body.model_instance_id,
        body.collision_body_role_id, body.collision_index);
    const auto selected = selected_by_frame.find(body.compiled_frame_name);
    if (selected == selected_by_frame.end() ||
        !result.aliases.emplace(semantic, selected->second.body_id).second) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision-body alias construction is incomplete");
    }
  }
  return result;
}

RetainedCollisionWorldProjector RetainedCollisionWorldProjector::create(
    collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
    RetainedCollisionModelMapInput model_map) {
  if (model_map.retained_bundle_id.empty() ||
      model_map.graph_revision.empty() || model_map.cell_id.empty() ||
      model_map.compiled_cell_frame_name.empty()) {
    throw RetainedCollisionWorldProjectionError(
        "retained collision model map has no complete cell-frame identity");
  }
  std::sort(model_map.targets.begin(), model_map.targets.end(),
            [](const auto& left, const auto& right) {
              return std::tie(left.controllable_component_id,
                              left.command_target_id) <
                     std::tie(right.controllable_component_id,
                              right.command_target_id);
            });
  std::sort(model_map.dynamic_compiled_joint_names.begin(),
            model_map.dynamic_compiled_joint_names.end());
  if (std::any_of(model_map.dynamic_compiled_joint_names.begin(),
                  model_map.dynamic_compiled_joint_names.end(),
                  [](const auto& name) { return name.empty(); }) ||
      std::adjacent_find(model_map.dynamic_compiled_joint_names.begin(),
                         model_map.dynamic_compiled_joint_names.end()) !=
          model_map.dynamic_compiled_joint_names.end()) {
    throw RetainedCollisionWorldProjectionError(
        "retained collision dynamic-coordinate inventory is invalid");
  }
  for (std::size_t target_index = 0U; target_index < model_map.targets.size();
       ++target_index) {
    const auto& target = model_map.targets[target_index];
    if (target.controllable_component_id.empty() ||
        target.command_target_id.empty() ||
        (target_index != 0U &&
         model_map.targets[target_index - 1U].controllable_component_id ==
             target.controllable_component_id &&
         model_map.targets[target_index - 1U].command_target_id ==
             target.command_target_id)) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision target identity is incomplete or duplicated");
    }
    for (const auto& frame : target.frames) {
      if (frame.controllable_component_id != target.controllable_component_id) {
        throw RetainedCollisionWorldProjectionError(
            "retained collision target contains a foreign frame");
      }
    }
    for (const auto& body : target.bodies) {
      if (body.controllable_component_id != target.controllable_component_id) {
        throw RetainedCollisionWorldProjectionError(
            "retained collision target contains a foreign body");
      }
    }
  }

  auto canonical_bodies =
      canonicalize_retained_component_bodies(model_map.component_bodies);
  auto projected =
      collision::ManyForgeCollisionEngine::project_retained_component_collision(
          shared_model, model_map.compiled_cell_frame_name,
          canonical_bodies.physical_bodies, model_map.component_frames);
  if (!projected.ok()) {
    throw RetainedCollisionWorldProjectionError(projected.error().message);
  }
  auto static_bodies =
      collision::ManyForgeCollisionEngine::select_retained_static_component_bodies(
          shared_model, canonical_bodies.physical_bodies,
          model_map.dynamic_compiled_joint_names);
  if (!static_bodies.ok()) {
    throw RetainedCollisionWorldProjectionError(static_bodies.error().message);
  }
  return RetainedCollisionWorldProjector(std::move(shared_model),
                                         std::move(model_map),
                                         std::move(projected.value()),
                                         std::move(static_bodies.value()),
                                         std::move(canonical_bodies.aliases));
}

core::Result<core::CollisionPolicySnapshot>
RetainedCollisionWorldProjector::project_effective_policy(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    const std::uint64_t effective_policy_revision_id) const {
  // The scene owner validated and digested this Scene once (OD-376); it is
  // not validated again here.
  if (logical_scene.cell_id != model_map_.cell_id ||
      effective_policy_revision_id == 0U) {
    return policy_projection_error(
        "effective collision policy has invalid cell or revision identity");
  }

  core::CollisionPolicySnapshot effective =
      logical_scene.configured_collision_policy;
  if (!effective.static_body_ids.empty() ||
      !effective.allowed_touch_link_pairs.empty() ||
      !effective.contact_rules.empty()) {
    return policy_projection_error(
        "configured collision policy contains authority-derived body pairs "
        "or collision rules of a move");
  }
  try {
    const auto configured_digest =
        core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
            effective);
    if (configured_digest.value() != effective.revision.content_hash) {
      return policy_projection_error(
          "configured collision policy content identity is not exact");
    }
  } catch (const std::exception& error) {
    return policy_projection_error(
        "configured collision policy cannot be verified: " +
        std::string(error.what()));
  }

  // The static bodies: the component bodies no dynamic coordinate moves and
  // every shape of a cell-fixed object. Two of them are never checked against
  // each other; the policy names the bodies, not their pairs.
  effective.static_body_ids = static_component_body_ids_;
  for (const auto& object : logical_scene.objects) {
    if (std::holds_alternative<core::CellFixedSceneObjectPlacementDTO>(
            object.placement)) {
      for (std::size_t shape = 0U; shape < object.collision_shapes.size();
           ++shape) {
        effective.static_body_ids.push_back(core::make_scene_collision_body_id(
            object.scene_resource_id, shape));
      }
    }
  }

  for (const auto& object : logical_scene.objects) {
    const auto* carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&object.placement);
    if (carried == nullptr) {
      continue;
    }
    const auto attachment =
        std::lower_bound(logical_scene.carried_object_attachments.begin(),
                         logical_scene.carried_object_attachments.end(),
                         carried->carried_object_attachment_id,
                         [](const core::CarriedObjectAttachmentDTO& candidate,
                            const core::CarriedObjectAttachmentId& id) {
                           return candidate.carried_object_attachment_id < id;
                         });
    if (attachment == logical_scene.carried_object_attachments.end() ||
        attachment->carried_object_attachment_id !=
            carried->carried_object_attachment_id) {
      return policy_projection_error(
          "carried scene resource has no exact attachment row");
    }
    const auto* target =
        find_target(model_map_, attachment->controllable_component_id,
                    attachment->command_target_id);
    const auto* carrier = std::get_if<core::ComponentSceneFrameRefDTO>(
        &attachment->carrier_frame_ref);
    if (target == nullptr || carrier == nullptr) {
      return policy_projection_error(
          "carried attachment names an unknown component/command target");
    }

    const auto touch_bodies = carrier_bodies(model_map_, *target, *carrier);
    for (const auto& role : attachment->touch_body_roles) {
      bool role_found = false;
      for (const auto& target_body : touch_bodies) {
        if (target_body.collision_body_role_id != role) {
          continue;
        }
        role_found = true;
        const core::CollisionBodyId semantic_body_id =
            core::make_component_collision_body_id(
                target_body.controllable_component_id,
                target_body.model_instance_id,
                target_body.collision_body_role_id,
                target_body.collision_index);
        const auto physical_body =
            component_body_aliases_.find(semantic_body_id);
        if (physical_body == component_body_aliases_.end()) {
          return policy_projection_error(
              "carried attachment touch role has no physical collision "
              "projection");
        }
        for (std::size_t shape_index = 0U;
             shape_index < object.collision_shapes.size(); ++shape_index) {
          effective.allowed_touch_link_pairs.push_back(canonical_pair(
              physical_body->second,
              core::make_scene_collision_body_id(object.scene_resource_id,
                                                  shape_index)));
        }
      }
      if (!role_found) {
        return policy_projection_error(
            "carried attachment touch role is absent from its addressed "
            "command target");
      }
    }
  }

  std::sort(effective.allowed_touch_link_pairs.begin(),
            effective.allowed_touch_link_pairs.end());
  effective.allowed_touch_link_pairs.erase(
      std::unique(effective.allowed_touch_link_pairs.begin(),
                  effective.allowed_touch_link_pairs.end()),
      effective.allowed_touch_link_pairs.end());
  std::sort(effective.static_body_ids.begin(),
            effective.static_body_ids.end());
  effective.static_body_ids.erase(
      std::unique(effective.static_body_ids.begin(),
                  effective.static_body_ids.end()),
      effective.static_body_ids.end());

  effective.revision.revision_id = effective_policy_revision_id;
  try {
    effective.revision.content_hash =
        core::logical_scene_identity::ComputeCollisionPolicyContentDigest(
            effective)
            .value();
  } catch (const std::exception& error) {
    return policy_projection_error(
        "effective collision policy cannot be normalized: " +
        std::string(error.what()));
  }
  return core::Result<core::CollisionPolicySnapshot>::Ok(std::move(effective));
}

core::Result<core::CollisionWorldSnapshot>
RetainedCollisionWorldProjector::project(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    const std::uint64_t collision_world_revision_id,
    const std::int64_t emitted_at_ns, std::string publisher_id) const {
  if (!logical_scene.locally_valid()) {
    return projection_error(
        "logical scene is not a valid immutable projection input");
  }
  if (logical_scene.cell_id != model_map_.cell_id) {
    return projection_error(
        "logical scene cell differs from the retained collision model map");
  }

  auto effective_policy = project_effective_policy(logical_scene, 1U);
  if (!effective_policy.ok()) {
    return projection_error(effective_policy.error().message);
  }

  core::CollisionWorldSnapshotBuilder builder;
  builder.set_publisher_id(std::move(publisher_id))
      .set_revision_id(collision_world_revision_id)
      .set_emitted_at_ns(emitted_at_ns)
      .set_policy_overlay_content_hash(
          effective_policy.value().revision.content_hash);
  for (const auto& frame : component_projection_.component_model_frames) {
    builder.add_component_model_frame(frame);
  }
  for (const auto& body : component_projection_.component_bodies) {
    builder.add_body(body);
  }

  for (const auto& object : logical_scene.objects) {
    const auto* fixed =
        std::get_if<core::CellFixedSceneObjectPlacementDTO>(&object.placement);
    if (fixed != nullptr && fixed->cell_id != model_map_.cell_id) {
      return projection_error("cell-fixed scene resource names a foreign cell");
    }
    std::optional<collision::RetainedCarriedObjectParentProjection>
        carried_parent;
    const core::CarriedObjectAttachmentDTO* carried_attachment = nullptr;
    if (fixed == nullptr) {
      const auto* carried =
          std::get_if<core::CarriedSceneObjectPlacementDTO>(&object.placement);
      if (carried == nullptr) {
        return projection_error(
            "scene resource has an unsupported placement variant");
      }
      const auto attachment =
          std::lower_bound(logical_scene.carried_object_attachments.begin(),
                           logical_scene.carried_object_attachments.end(),
                           carried->carried_object_attachment_id,
                           [](const core::CarriedObjectAttachmentDTO& candidate,
                              const core::CarriedObjectAttachmentId& id) {
                             return candidate.carried_object_attachment_id < id;
                           });
      if (attachment == logical_scene.carried_object_attachments.end() ||
          attachment->carried_object_attachment_id !=
              carried->carried_object_attachment_id) {
        return projection_error(
            "carried scene resource has no exact attachment row");
      }
      // A `continuous` pose source may keep updating a carried object's
      // carrier-relative pose; its evidence is provenance only here. The
      // carried pose is the attachment's.
      const auto* target =
          find_target(model_map_, attachment->controllable_component_id,
                      attachment->command_target_id);
      if (target == nullptr) {
        return projection_error(
            "carried attachment names an unknown component/command target");
      }
      const auto* carrier = std::get_if<core::ComponentSceneFrameRefDTO>(
          &attachment->carrier_frame_ref);
      if (carrier == nullptr) {
        return projection_error(
            "carried attachment has no exact component carrier frame");
      }
      // The holder target's own frame, or (OD-372) a frame of the component
      // mounted on the holder's robot.
      const auto* carrier_frame = find_carrier_frame(
          carrier->controllable_component_id ==
                  target->controllable_component_id
              ? target->frames
              : model_map_.component_frames,
          *carrier);
      if (carrier_frame == nullptr) {
        return projection_error(
            "carried attachment carrier frame is absent from its addressed "
            "command target");
      }
      auto resolved = collision::ManyForgeCollisionEngine::
          resolve_retained_carried_object_parent(
              shared_model_, carrier_frame->compiled_frame_name,
              carrier_bodies(model_map_, *target, *carrier));
      if (!resolved.ok()) {
        return projection_error(resolved.error().message);
      }
      const auto physical_parent =
          component_body_aliases_.find(resolved.value().parent_body_id);
      if (physical_parent == component_body_aliases_.end()) {
        return projection_error(
            "carried-object parent has no physical collision projection");
      }
      resolved.value().parent_body_id = physical_parent->second;
      carried_parent = std::move(resolved.value());
      carried_attachment = &*attachment;
    }
    for (std::size_t shape_index = 0U;
         shape_index < object.collision_shapes.size(); ++shape_index) {
      core::CollisionWorldBody body;
      body.body_id = core::make_scene_collision_body_id(
          object.scene_resource_id, shape_index);
      body.owner_kind = core::CollisionOwnerKind::kSceneResource;
      if (carried_parent.has_value()) {
        body.parent_body_id = carried_parent->parent_body_id;
        body.frame_id = carried_parent->compiled_parent_frame_name;
        body.pose = compose_pose(carried_parent->parent_frame_from_carrier,
                                 carried_attachment->pose_in_carrier_frame);
      } else {
        body.frame_id = model_map_.compiled_cell_frame_name;
        body.pose = fixed->pose_in_cell;
      }
      body.geometries.push_back(scene_geometry_descriptor(
          body.body_id, object.collision_shapes[shape_index]));
      builder.add_body(std::move(body));
    }
  }
  return builder.build();
}

}  // namespace manyforge::roboplan_backend

#include <manyforge_roboplan/retained_collision_world_projector.hpp>

#include <cmath>
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
  const Eigen::Vector3d parent_translation(
      parent_from_frame.position_m.x_m,
      parent_from_frame.position_m.y_m,
      parent_from_frame.position_m.z_m);
  const Eigen::Vector3d object_translation(
      frame_from_object.position_m.x_m,
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
  const auto found = std::lower_bound(
      model_map.targets.begin(), model_map.targets.end(),
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

const collision::RetainedComponentModelFrameProjectionInput*
find_carrier_frame(
    const RetainedCollisionModelMapInput::Target& target,
    const core::ComponentSceneFrameRefDTO& carrier) {
  const auto found = std::find_if(
      target.frames.begin(), target.frames.end(), [&](const auto& frame) {
        return frame.controllable_component_id ==
                   carrier.controllable_component_id &&
               frame.model_instance_id == carrier.model_instance_id &&
               frame.frame_role_id == carrier.frame_role_id;
      });
  return found == target.frames.end() ? nullptr : &*found;
}

core::CollisionPairId canonical_pair(core::CollisionBodyId left,
                                     core::CollisionBodyId right) {
  if (right < left) {
    std::swap(left, right);
  }
  return {std::move(left), std::move(right)};
}

std::string scene_body_id(const core::SceneResourceId& resource_id,
                          const std::size_t shape_index) {
  return "scene/" + resource_id.value() + "/shape/" +
         std::to_string(shape_index);
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
          descriptor.geometry_kind =
              core::CollisionGeometryKind::kTriangleMesh;
          descriptor.source_uri = value.resource_uri;
          descriptor.mesh_scale = value.scale;
          descriptor.quality_labels.push_back("collision_mesh");
        }
      },
      shape);
  std::sort(descriptor.quality_labels.begin(),
            descriptor.quality_labels.end());
  return descriptor;
}

}  // namespace

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
  for (std::size_t target_index = 0U;
       target_index < model_map.targets.size(); ++target_index) {
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
      if (frame.controllable_component_id !=
          target.controllable_component_id) {
        throw RetainedCollisionWorldProjectionError(
            "retained collision target contains a foreign frame");
      }
    }
    for (const auto& body : target.bodies) {
      if (body.controllable_component_id !=
          target.controllable_component_id) {
        throw RetainedCollisionWorldProjectionError(
            "retained collision target contains a foreign body");
      }
    }
  }

  auto projected =
      collision::ManyForgeCollisionEngine::project_retained_component_collision(
          shared_model, model_map.compiled_cell_frame_name,
          model_map.component_bodies, model_map.component_frames);
  if (!projected.ok()) {
    throw RetainedCollisionWorldProjectionError(projected.error().message);
  }
  return RetainedCollisionWorldProjector(std::move(shared_model),
                                         std::move(model_map),
                                         std::move(projected.value()));
}

core::Result<core::CollisionPolicySnapshot>
RetainedCollisionWorldProjector::project_effective_policy(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    const std::uint64_t collision_policy_revision_id) const {
  if (!logical_scene.locally_valid()) {
    return policy_projection_error(
        "logical scene is not a valid immutable policy projection input");
  }
  if (logical_scene.cell_id != model_map_.cell_id ||
      collision_policy_revision_id == 0U) {
    return policy_projection_error(
        "effective collision policy has invalid cell or revision identity");
  }

  core::CollisionPolicySnapshot effective =
      logical_scene.configured_collision_policy;
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

  std::set<core::CollisionBodyId> body_ids;
  for (const auto& body : component_projection_.component_bodies) {
    body_ids.insert(body.body_id);
  }
  for (const auto& object : logical_scene.objects) {
    for (std::size_t shape_index = 0U;
         shape_index < object.collision_shapes.size(); ++shape_index) {
      body_ids.insert(scene_body_id(object.scene_resource_id, shape_index));
    }
  }

  for (const auto& object : logical_scene.objects) {
    const auto* carried =
        std::get_if<core::CarriedSceneObjectPlacementDTO>(&object.placement);
    if (carried == nullptr) {
      continue;
    }
    const auto attachment = std::lower_bound(
        logical_scene.carried_object_attachments.begin(),
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
    const auto* target = find_target(
        model_map_, attachment->controllable_component_id,
        attachment->command_target_id);
    if (target == nullptr) {
      return policy_projection_error(
          "carried attachment names an unknown component/command target");
    }

    for (const auto& role : attachment->touch_body_roles) {
      bool role_found = false;
      for (const auto& target_body : target->bodies) {
        if (target_body.collision_body_role_id != role) {
          continue;
        }
        role_found = true;
        const std::string component_body_id =
            "component/" +
            target_body.controllable_component_id.value() + "/model/" +
            target_body.model_instance_id.value() + "/body/" +
            target_body.collision_body_role_id.value() + "/collision/" +
            std::to_string(target_body.collision_index);
        for (std::size_t shape_index = 0U;
             shape_index < object.collision_shapes.size(); ++shape_index) {
          effective.allowed_touch_link_pairs.push_back(canonical_pair(
              component_body_id,
              scene_body_id(object.scene_resource_id, shape_index)));
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

  const auto validate_pair = [&](const core::CollisionPairId& pair) {
    return !pair.body_a.empty() && !pair.body_b.empty() &&
           pair.body_a <= pair.body_b && body_ids.contains(pair.body_a) &&
           body_ids.contains(pair.body_b);
  };
  if (!std::all_of(effective.allowed_static_static_pairs.begin(),
                   effective.allowed_static_static_pairs.end(),
                   validate_pair) ||
      !std::all_of(effective.allowed_touch_link_pairs.begin(),
                   effective.allowed_touch_link_pairs.end(), validate_pair) ||
      !std::all_of(effective.support_contacts.begin(),
                   effective.support_contacts.end(),
                   [&](const auto& entry) {
                     return validate_pair(entry.pair);
                   })) {
    return policy_projection_error(
        "effective collision policy references a body outside the projected "
        "collision world");
  }

  effective.revision.revision_id = collision_policy_revision_id;
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
  return core::Result<core::CollisionPolicySnapshot>::Ok(
      std::move(effective));
}

core::Result<core::CollisionWorldSnapshot>
RetainedCollisionWorldProjector::project(
    const core::LogicalSceneSnapshotDTO& logical_scene,
    const std::uint64_t collision_world_revision_id,
    const std::int64_t emitted_at_ns,
    std::string publisher_id) const {
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
      return projection_error(
          "cell-fixed scene resource names a foreign cell");
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
      const auto attachment = std::lower_bound(
          logical_scene.carried_object_attachments.begin(),
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
      if (object.provider_evidence.has_value()) {
        return projection_error(
            "carried scene resource retains provider pose authority");
      }
      const auto* target = find_target(
          model_map_, attachment->controllable_component_id,
          attachment->command_target_id);
      if (target == nullptr) {
        return projection_error(
            "carried attachment names an unknown component/command target");
      }
      const auto* carrier =
          std::get_if<core::ComponentSceneFrameRefDTO>(
              &attachment->carrier_frame_ref);
      if (carrier == nullptr ||
          carrier->controllable_component_id !=
              target->controllable_component_id) {
        return projection_error(
            "carried attachment has no exact component carrier frame");
      }
      const auto* carrier_frame = find_carrier_frame(*target, *carrier);
      if (carrier_frame == nullptr) {
        return projection_error(
            "carried attachment carrier frame is absent from its addressed "
            "command target");
      }
      auto resolved =
          collision::ManyForgeCollisionEngine::
              resolve_retained_carried_object_parent(
                  shared_model_, carrier_frame->compiled_frame_name,
                  target->bodies);
      if (!resolved.ok()) {
        return projection_error(resolved.error().message);
      }
      carried_parent = std::move(resolved.value());
      carried_attachment = &*attachment;
    }
    for (std::size_t shape_index = 0U;
         shape_index < object.collision_shapes.size(); ++shape_index) {
      core::CollisionWorldBody body;
      body.body_id = scene_body_id(object.scene_resource_id, shape_index);
      body.owner_kind = core::CollisionOwnerKind::kSceneResource;
      if (carried_parent.has_value()) {
        body.parent_body_id = carried_parent->parent_body_id;
        body.frame_id = carried_parent->compiled_parent_frame_name;
        body.pose = compose_pose(
            carried_parent->parent_frame_from_carrier,
            carried_attachment->pose_in_carrier_frame);
      } else {
        body.frame_id = model_map_.compiled_cell_frame_name;
        body.pose = fixed->pose_in_cell;
      }
      if (!carried_parent.has_value() &&
          object.provider_evidence.has_value()) {
        body.source_id =
            object.provider_evidence->provider_instance_id.value();
        body.source_timestamp_ns = object.provider_evidence->received_at_ns;
        body.source_freshness_budget_ns =
            object.provider_evidence->source_freshness_budget_ns;
      }
      body.geometries.push_back(scene_geometry_descriptor(
          body.body_id, object.collision_shapes[shape_index]));
      builder.add_body(std::move(body));
    }
  }
  return builder.build();
}

}  // namespace manyforge::roboplan_backend

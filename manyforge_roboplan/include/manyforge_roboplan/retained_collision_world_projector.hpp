#pragma once

#include <algorithm>
#include <cstdint>
#include <map>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/collision/i_collision_world_snapshot_publisher.hpp>
#include <manyforge_core/types/logical_scene_snapshot.hpp>

namespace manyforge::roboplan_backend {

class RetainedCollisionWorldProjectionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Deployment-neutral copy of the compiler-owned collision namespace needed
/// by the native projection boundary.
///
/// As with RetainedPlanningModelInput, this is a value bridge rather than a
/// dependency on the deployment compiler. The sealed deployment reader must
/// provide the exact retained model map; create_verified() copies only the
/// already-verified runtime fields below.
struct RetainedCollisionModelMapInput final {
  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  core::CellId cell_id;
  std::string compiled_cell_frame_name;
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
      component_bodies;
  std::vector<collision::RetainedComponentModelFrameProjectionInput>
      component_frames;
  std::vector<std::string> dynamic_compiled_joint_names;
  struct Target final {
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::vector<collision::RetainedComponentModelFrameProjectionInput>
        frames;
    std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
        bodies;
  };
  std::vector<Target> targets;
};

using RetainedComponentBodyAliasMap =
    std::map<core::CollisionBodyId, core::CollisionBodyId>;

struct CanonicalRetainedComponentBodies final {
  std::vector<collision::RetainedComponentCollisionBodyProjectionInput>
      physical_bodies;
  RetainedComponentBodyAliasMap aliases;
};

/// Collapse overlapping semantic role views onto one physical compiled-frame
/// body while retaining a total alias for every role/index identity. Source
/// URDF/SRDF roles may legally overlap; duplicated physical geometry may not.
[[nodiscard]] CanonicalRetainedComponentBodies
canonicalize_retained_component_bodies(
    std::span<
        const collision::RetainedComponentCollisionBodyProjectionInput>
        bodies);

/// Pure retained-model + logical-scene to immutable collision-world projector.
///
/// This class owns no scene mutation authority and does not mint logical-scene
/// identity. It projects one scene-owner-issued immutable snapshot together
/// with the exact verified retained model/map generation. Collision-world
/// revision sequencing and ledger publication remain the publisher's job.
class RetainedCollisionWorldProjector final {
 public:
  [[nodiscard]] static RetainedCollisionWorldProjector create(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      RetainedCollisionModelMapInput model_map);

  /// Copy the narrow runtime map from one independently verified deployment
  /// model-map value. The planning-model argument enforces exact same-bundle
  /// identity before any compiler-owned symbol is accepted.
  template <typename RetainedBundle, typename VerifiedRetainedModelMap,
            typename VerifiedRetainedPlanningModel>
  [[nodiscard]] static RetainedCollisionWorldProjector create_verified(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& model_map,
      const VerifiedRetainedPlanningModel& planning_model) {
    if (!verify_retained_bundle_identity(retained_bundle)) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision projector received an unverifiable bundle");
    }
    model_map.require_same_bundle_as(planning_model);
    if (model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id() ||
        planning_model.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      throw RetainedCollisionWorldProjectionError(
          "retained collision projector crosses bundle identity");
    }

    RetainedCollisionModelMapInput input{
        retained_bundle.retained_bundle_id(),
        retained_bundle.value().cell_graph.graph_revision,
        model_map.cell_frame().cell_id,
        model_map.cell_frame().compiled_frame_name,
        {},
        {},
        {},
        {},
    };

    for (const auto& role : model_map.model_collision_bodies()) {
      if (!role.owner_component_id.has_value()) {
        throw RetainedCollisionWorldProjectionError(
            "passive-model collision geometry has no normative public body "
            "namespace in the selected collision contract");
      }
      for (std::size_t index = 0U;
           index < role.compiled_link_names.size(); ++index) {
        input.component_bodies.push_back({
            *role.owner_component_id,
            role.model_instance_id,
            role.model_revision,
            role.collision_body_role_id,
            index,
            role.compiled_link_names[index],
        });
      }
    }

    for (const auto& coordinate : model_map.coordinates()) {
      using Kind = std::remove_cvref_t<decltype(coordinate.coordinate_kind)>;
      if (coordinate.coordinate_kind != Kind::kNone) {
        input.dynamic_compiled_joint_names.push_back(
            coordinate.compiled_joint_name);
      }
    }

    using FrameKey = std::tuple<std::string, std::string, std::string>;
    std::map<FrameKey,
             collision::RetainedComponentModelFrameProjectionInput>
        frames_by_key;
    for (const auto& target : model_map.targets()) {
      RetainedCollisionModelMapInput::Target target_input{
          target.controllable_component_id,
          target.command_target_id,
          {},
          {},
      };
      for (const auto& target_frame : target.frames) {
        std::visit(
            [&](const auto& resolved) {
              using T = std::decay_t<decltype(resolved)>;
              if constexpr (requires(const T& value) {
                              value.controllable_component_id;
                              value.model_instance_id;
                              value.model_revision;
                              value.frame_role_id;
                              value.compiled_frame_name;
                            }) {
                if (resolved.controllable_component_id !=
                    target.controllable_component_id) {
                  throw RetainedCollisionWorldProjectionError(
                      "retained target frame is owned by a different "
                      "component");
                }
                collision::RetainedComponentModelFrameProjectionInput row{
                    resolved.controllable_component_id,
                    resolved.model_instance_id,
                    resolved.model_revision,
                    resolved.frame_role_id,
                    resolved.compiled_frame_name,
                };
                const FrameKey key{
                    row.controllable_component_id.value(),
                    row.model_instance_id.value(), row.frame_role_id.value()};
                const auto [found, inserted] =
                    frames_by_key.emplace(key, row);
                if (!inserted &&
                    (found->second.model_revision != row.model_revision ||
                     found->second.compiled_frame_name !=
                         row.compiled_frame_name)) {
                  throw RetainedCollisionWorldProjectionError(
                      "retained targets disagree on one component/model/frame "
                      "projection");
                }
                target_input.frames.push_back(std::move(row));
              }
            },
            target_frame.frame);
      }
      for (const auto& target_collision : target.collision_bodies) {
        for (std::size_t index = 0U;
             index < target_collision.compiled_link_names.size(); ++index) {
          const auto body = std::find_if(
              input.component_bodies.begin(), input.component_bodies.end(),
              [&](const auto& candidate) {
                return candidate.controllable_component_id ==
                           target.controllable_component_id &&
                       candidate.model_instance_id ==
                           target_collision.model_instance_id &&
                       candidate.collision_body_role_id ==
                           target_collision.collision_body_role_id &&
                       candidate.collision_index == index &&
                       candidate.compiled_frame_name ==
                           target_collision.compiled_link_names[index];
              });
          if (body == input.component_bodies.end()) {
            throw RetainedCollisionWorldProjectionError(
                "retained target collision role is absent from the exact "
                "component collision inventory");
          }
          target_input.bodies.push_back(*body);
        }
      }
      input.targets.push_back(std::move(target_input));
    }
    input.component_frames.reserve(frames_by_key.size());
    for (auto& [key, frame] : frames_by_key) {
      static_cast<void>(key);
      input.component_frames.push_back(std::move(frame));
    }
    return create(std::move(shared_model), std::move(input));
  }

  [[nodiscard]] core::Result<core::CollisionWorldSnapshot> project(
      const core::LogicalSceneSnapshotDTO& logical_scene,
      std::uint64_t collision_world_revision_id,
      std::int64_t emitted_at_ns,
      std::string publisher_id) const;

  /// Resolve configured policy plus carried-object touch-role allowances to
  /// exact public collision-body pairs for the same immutable scene.
  [[nodiscard]] core::Result<core::CollisionPolicySnapshot>
  project_effective_policy(
      const core::LogicalSceneSnapshotDTO& logical_scene,
      std::uint64_t effective_policy_revision_id) const;

  /// Build the exact lease sidecars for an already projected immutable world.
  /// Durable publication and candidate-local projection share these helpers so
  /// scene-object freshness and body identities cannot drift.
  [[nodiscard]] std::vector<core::CollisionWorldBodyFreshness>
  project_body_freshness(
      const core::CollisionWorldSnapshot& world) const;
  [[nodiscard]] std::vector<core::SceneSnapshotLiveObject>
  project_live_objects(
      const core::LogicalSceneSnapshotDTO& logical_scene,
      const core::CollisionWorldSnapshot& world) const;

  [[nodiscard]] const RetainedCollisionModelMapInput& model_map()
      const noexcept {
    return model_map_;
  }
  [[nodiscard]] const collision::RetainedComponentCollisionProjection&
  component_projection() const noexcept {
    return component_projection_;
  }
  [[nodiscard]] const collision::ManyForgeCollisionEngine::SharedModelPtr&
  shared_model() const noexcept {
    return shared_model_;
  }
  [[nodiscard]] const RetainedComponentBodyAliasMap& component_body_aliases()
      const noexcept {
    return component_body_aliases_;
  }

 private:
  RetainedCollisionWorldProjector(
      collision::ManyForgeCollisionEngine::SharedModelPtr shared_model,
      RetainedCollisionModelMapInput model_map,
      collision::RetainedComponentCollisionProjection component_projection,
      std::vector<core::CollisionBodyId> static_component_body_ids,
      RetainedComponentBodyAliasMap component_body_aliases)
      : shared_model_(std::move(shared_model)),
        model_map_(std::move(model_map)),
        component_projection_(std::move(component_projection)),
        static_component_body_ids_(std::move(static_component_body_ids)),
        component_body_aliases_(std::move(component_body_aliases)) {}

  collision::ManyForgeCollisionEngine::SharedModelPtr shared_model_;
  RetainedCollisionModelMapInput model_map_;
  collision::RetainedComponentCollisionProjection component_projection_;
  std::vector<core::CollisionBodyId> static_component_body_ids_;
  RetainedComponentBodyAliasMap component_body_aliases_;
};

}  // namespace manyforge::roboplan_backend

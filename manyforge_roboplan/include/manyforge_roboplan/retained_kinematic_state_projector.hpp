#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <roboplan/core/scene.hpp>

#include <manyforge_core/runtime_participant_binding.hpp>
#include <manyforge_core/types/coordinate_state.hpp>

namespace manyforge::roboplan_backend {

enum class RetainedStateProjectionMode {
  kBlocking,
  kBestEffort,
};

enum class RetainedStateProjectionStatus {
  kApplied,
  kBusy,
  kRejected,
  kFailed,
};

struct RetainedStateProjectionResult final {
  RetainedStateProjectionStatus status{RetainedStateProjectionStatus::kRejected};
  std::optional<core::ErrorDTO> error;

  [[nodiscard]] bool applied() const noexcept {
    return status == RetainedStateProjectionStatus::kApplied;
  }
};

struct RetainedModelCoordinateStateDTO final {
  core::ModelInstanceId model_instance_id;
  std::string compiled_joint_name;
  core::CoordinatePositionDTO position;

  friend bool operator==(const RetainedModelCoordinateStateDTO&,
                         const RetainedModelCoordinateStateDTO&) = default;
};

/// Deployment-neutral copy of the compiler-owned coordinate projection needed
/// by the native kinematic boundary. The deployment reader verifies the
/// retained bundle and model products before create_verified() copies these
/// runtime values; RoboPlan never gains compiler, artifact, or Apply authority.
struct RetainedKinematicModelMapInput final {
  struct Coordinate final {
    core::ModelInstanceId model_instance_id;
    std::string compiled_joint_name;
    core::CoordinateKind coordinate_kind{core::CoordinateKind::kAngular};
    double multiplier{1.0};
    double offset{};
    bool derived_mimic{};
  };

  struct Resource final {
    core::ResourceId resource_id;
    /// Empty only for an independently verified component-local resource.
    /// Such a row remains in the complete retained map but can never be
    /// selected by a RoboPlan projection target.
    std::vector<Coordinate> coordinates;
  };

  struct ControlBinding final {
    core::ControlBindingId control_binding_id;
    core::ControllableComponentId controllable_component_id;
    core::CommandTargetId command_target_id;
    std::vector<Resource> resources;
  };

  core::RetainedBundleId retained_bundle_id;
  core::GraphRevision graph_revision;
  std::vector<ControlBinding> control_bindings;
};

/// Exact unit-tagged state projection for a verified retained planning model.
///
/// Each registered target is derived from a sealed RuntimeParticipantBinding
/// and checked against the same retained bundle/model map used to construct the
/// RoboPlan scene. Applying one target updates its direct and compiler-declared
/// mimic coordinates while retaining the other targets' current values.
class RetainedKinematicStateProjector final {
 public:
  [[nodiscard]] static RetainedKinematicStateProjector create(
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex,
      RetainedKinematicModelMapInput model_map,
      std::vector<core::RuntimeParticipantBinding> bindings,
      std::int64_t freshness_budget_ns);

  /// One freshness budget per binding (same order): each target registers its
  /// own state channel's budget (e.g. a Real measured arm beside a commanded
  /// gripper).
  [[nodiscard]] static RetainedKinematicStateProjector create(
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex,
      RetainedKinematicModelMapInput model_map,
      std::vector<core::RuntimeParticipantBinding> bindings,
      std::vector<std::int64_t> freshness_budgets_ns);

  /// Copy only the exact runtime coordinate namespace from deployment-owned,
  /// independently verified products. The unqualified identity verifier is
  /// intentionally resolved by argument-dependent lookup at instantiation so
  /// this library does not import or link the deployment compiler.
  /// `freshness_budget_ns` is one budget for every target or one per binding,
  /// as in create().
  template <typename RetainedBundle, typename VerifiedRetainedModelMap,
            typename VerifiedRetainedPlanningModel, typename FreshnessBudget>
  [[nodiscard]] static RetainedKinematicStateProjector create_verified(
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex,
      const RetainedBundle& retained_bundle,
      const VerifiedRetainedModelMap& retained_model_map,
      const VerifiedRetainedPlanningModel& planning_model,
      std::vector<core::RuntimeParticipantBinding> bindings,
      FreshnessBudget freshness_budget_ns) {
    if (!verify_retained_bundle_identity(retained_bundle)) {
      throw std::invalid_argument(
          "retained state projector received an unverifiable bundle");
    }
    retained_model_map.require_same_bundle_as(planning_model);
    if (retained_model_map.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id() ||
        planning_model.query().retained_bundle_id !=
            retained_bundle.retained_bundle_id()) {
      throw std::invalid_argument(
          "retained state projector crosses bundle identity");
    }

    RetainedKinematicModelMapInput input{
        retained_bundle.retained_bundle_id(),
        retained_bundle.value().cell_graph.graph_revision,
        {},
    };
    input.control_bindings.reserve(
        retained_model_map.control_bindings().size());
    for (const auto& binding : retained_model_map.control_bindings()) {
      RetainedKinematicModelMapInput::ControlBinding binding_input{
          binding.control_binding_id,
          binding.controllable_component_id,
          binding.command_target_id,
          {},
      };
      binding_input.resources.reserve(binding.resources.size());
      for (const auto& resource : binding.resources) {
        RetainedKinematicModelMapInput::Resource resource_input{
            resource.resource_id, {}};
        resource_input.coordinates.reserve(resource.coordinates.size());
        for (const auto& coordinate : resource.coordinates) {
          using MapKind = std::remove_cvref_t<
              decltype(coordinate.coordinate_kind)>;
          using MapUnit = std::remove_cvref_t<decltype(coordinate.unit)>;
          std::optional<core::CoordinateKind> coordinate_kind;
          if (coordinate.coordinate_kind == MapKind::kAngular &&
              coordinate.unit == MapUnit::kRadians) {
            coordinate_kind = core::CoordinateKind::kAngular;
          } else if (coordinate.coordinate_kind == MapKind::kLinear &&
                     coordinate.unit == MapUnit::kMeters) {
            coordinate_kind = core::CoordinateKind::kLinear;
          } else {
            throw std::invalid_argument(
                "retained state coordinate has no supported canonical unit");
          }
          resource_input.coordinates.push_back({
              coordinate.model_instance_id,
              coordinate.compiled_joint_name,
              *coordinate_kind,
              coordinate.multiplier,
              coordinate.offset,
              coordinate.derived_mimic,
          });
        }
        binding_input.resources.push_back(std::move(resource_input));
      }
      input.control_bindings.push_back(std::move(binding_input));
    }
    return create(std::move(scene), std::move(scene_mutex), std::move(input),
                  std::move(bindings), std::move(freshness_budget_ns));
  }

  ~RetainedKinematicStateProjector();

  RetainedKinematicStateProjector(
      const RetainedKinematicStateProjector&) = delete;
  RetainedKinematicStateProjector& operator=(
      const RetainedKinematicStateProjector&) = delete;
  RetainedKinematicStateProjector(
      RetainedKinematicStateProjector&&) noexcept;
  RetainedKinematicStateProjector& operator=(
      RetainedKinematicStateProjector&&) noexcept;

  [[nodiscard]] RetainedStateProjectionResult apply(
      const core::ObservedStateSnapshotDTO& state,
      std::int64_t now_ns,
      RetainedStateProjectionMode mode =
          RetainedStateProjectionMode::kBlocking);

  [[nodiscard]] core::Result<core::ControlledStateDTO> current_state(
      const core::ControlBindingId& control_binding_id,
      const core::ControlBindingRevision& control_binding_revision) const;

  /// Complete registered model-coordinate view in canonical compiled-name
  /// order. This is an adapter-private bridge for FK/collision projection;
  /// units remain tagged and are never exposed as a public angular vector.
  [[nodiscard]] core::Result<std::vector<RetainedModelCoordinateStateDTO>>
  model_coordinate_state() const;

  [[nodiscard]] std::vector<core::ObservedStateQueryContextDTO>
  query_contexts() const;

 private:
  struct TargetProjection;

  RetainedKinematicStateProjector(
      std::shared_ptr<roboplan::Scene> scene,
      std::shared_ptr<std::mutex> scene_mutex,
      std::vector<TargetProjection> targets);

  std::shared_ptr<roboplan::Scene> scene_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::vector<TargetProjection> targets_;
};

}  // namespace manyforge::roboplan_backend

#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <manyforge_collision/manyforge_collision_engine.hpp>
#include <manyforge_core/cache/trajectory_cache.hpp>
#include <manyforge_core/execution_kernel.hpp>
#include <manyforge_core/ports/ireference_pose_resolver.hpp>
#include <manyforge_core/resource_addressed_execution_kernel.hpp>
#include <manyforge_core/sqlite_operation_journal.hpp>
#include <manyforge_roboplan/native_runtime_support.hpp>
#include <manyforge_roboplan/retained_collision_world_publisher.hpp>
#include <manyforge_roboplan/retained_focused_adapters.hpp>
#include <manyforge_roboplan/retained_gripper_command_backend.hpp>
#include <manyforge_roboplan/roboplan_execution_backend.hpp>
#include <manyforge_roboplan/roboplan_planner_backend.hpp>
#include <memory>
#include <mutex>
#include <optional>
#include <roboplan/core/scene.hpp>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

struct RetainedNativeRuntimeConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::CacheConfig cache;
  core::ExecutionKernelConfig kernel;
  RoboPlanExecutionBackendConfig execution;
  SimpleSchedulerConfig scheduler;
  core::Duration gripper_update_period_ns{10'000'000};
  /// Exact motion-resource order from the retained runtime-unit product.
  /// Empty remains available only to lower-level fixtures that intentionally
  /// bootstrap from the scene's already-established state.
  std::vector<double> initial_motion_positions_rad;
};

struct RetainedNativeAddressedOwnerConfig final {
  core::RequestOwnerEpoch request_owner_epoch;
  core::CellId cell_id;
  std::filesystem::path operation_journal_path;
  std::size_t request_replay_capacity{1024U};
  core::IResourceOperationAdmissionGate *operation_admission_gate{};
};

using RetainedReferenceResolverFactory =
    std::function<std::shared_ptr<core::IReferencePoseResolver>(
        const std::vector<std::shared_ptr<RetainedObservedStateChannel>> &)>;

using RetainedRuntimeParticipantResolver =
    std::function<core::Result<core::RuntimeParticipantBinding>(
        const core::OperationIntentDTO &, std::size_t)>;

/// Already-verified inputs to one native runtime composition.
///
/// Deployment/product readers and the logical-scene owner remain outside this
/// boundary. They supply sealed bindings, one exact retained scene/projector,
/// a planner whose worker model has the same projection revision, and a
/// reference resolver backed by the logical-scene owner. No path, model name,
/// robot default, or group selector is accepted here.
struct RetainedNativeRuntimeInput final {
  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  /// Additional Program contexts for the same exact physical motion target.
  /// This is used by build-owned direct-control Programs; aliases may differ
  /// only in Program/role identity and cannot widen the physical runtime.
  std::vector<core::RuntimeParticipantBinding> motion_binding_aliases;
  /// Additional Program contexts for the same exact physical gripper target.
  std::vector<core::RuntimeParticipantBinding> gripper_binding_aliases;
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  collision::ManyForgeCollisionEngine::SharedModelPtr collision_shared_model;
  std::unique_ptr<RoboPlanPlannerBackend> planner;
  RetainedReferenceResolverFactory reference_resolver_factory;
  /// Optional exact Program/Program-Binding point resolver. The composition
  /// supplies only sealed bindings for this physical runtime; Core rechecks
  /// the request address, resource inventory, and pinned engine before use.
  RetainedRuntimeParticipantResolver participant_resolver;
  /// Optional configuration-fence interlock owned by the surrounding managed
  /// execution process. It must outlive this runtime composition.
  core::IResourceOperationAdmissionGate *operation_admission_gate{};
};

/// Owning native composition root. Destruction order is deliberate: the public
/// addressed owner unsubscribes first, then the subordinate Kernel quiesces,
/// then backends/scheduler/state/model resources retire.
struct RetainedNativeRuntime final {
  RetainedNativeRuntime(core::RuntimeParticipantBinding motion,
                        std::optional<core::RuntimeParticipantBinding> gripper)
      : motion_binding(std::move(motion)), gripper_binding(std::move(gripper)) {
  }

  RetainedNativeRuntime(const RetainedNativeRuntime &) = delete;
  RetainedNativeRuntime &operator=(const RetainedNativeRuntime &) = delete;

  core::RuntimeParticipantBinding motion_binding;
  std::optional<core::RuntimeParticipantBinding> gripper_binding;
  std::shared_ptr<roboplan::Scene> scene;
  std::shared_ptr<std::mutex> scene_mutex;
  std::shared_ptr<RetainedKinematicStateProjector> state_projector;
  std::shared_ptr<RetainedObservedStateChannel> motion_state_channel;
  std::shared_ptr<RetainedObservedStateChannel> gripper_state_channel;

  std::shared_ptr<WallClock> clock;
  std::unique_ptr<SimpleScheduler> scheduler;
  std::unique_ptr<core::TrajectoryCache> cache;
  std::unique_ptr<RoboPlanPlannerBackend> planner;
  std::shared_ptr<core::IReferencePoseResolver> reference_resolver;
  std::unique_ptr<core::AddressedAngularObservedRobotStateAdapter>
      angular_observed_state;
  std::unique_ptr<RetainedRoboPlanKinematicsAdapter> kinematics;
  std::unique_ptr<RoboPlanTrajectoryValidator> trajectory_validator;
  std::unique_ptr<RoboPlanCollisionTrajectoryProjector>
      collision_trajectory_projector;
  std::unique_ptr<RoboPlanExecutionBackend> execution;
  std::unique_ptr<RetainedGripperCommandBackend> gripper;
  std::optional<RetainedGripperPositionLimits> gripper_position_limits;

  std::shared_ptr<core::ICollisionWorldSnapshotPublisher> collision_publisher;
  std::shared_ptr<core::ICollisionQueryEngine> runtime_collision_engine;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      validation_collision_engines;
  std::vector<std::shared_ptr<core::ICollisionQueryEngine>>
      lookahead_collision_engines;

  std::shared_ptr<core::ExecutionKernel> participant_engine;
  std::vector<core::RuntimeParticipantExecutionRegistration>
      participant_registrations;
  core::RuntimeParticipantAdmissionResolver participant_resolver;
  std::unique_ptr<core::SqliteOperationJournal> operation_journal;
  std::unique_ptr<core::ResourceAddressedExecutionKernel> addressed_kernel;
};

/// Assemble only the product-specific subordinate participant runtime. This
/// is the preparation boundary for a fenced Apply: it opens no operation
/// journal and creates no addressed Kernel owner. A supplied clock keeps a
/// later candidate attached to the same stable owner lifetime.
[[nodiscard]] std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime_participants(
    RetainedNativeRuntimeInput input, RetainedNativeRuntimeConfig config,
    std::shared_ptr<WallClock> shared_clock = {});

/// Attach the sole stable addressed owner to one prepared participant set.
/// The owner config is process-selected, not supplied by a Program or caller.
void attach_retained_native_addressed_owner(
    const std::shared_ptr<RetainedNativeRuntime> &runtime,
    RetainedNativeAddressedOwnerConfig config);

/// Assemble and seed one production native participant runtime from exact
/// retained values. Inconsistent projection, planner, state, component, target,
/// group, unit, or worker identities throw before any operation is admitted.
[[nodiscard]] std::shared_ptr<RetainedNativeRuntime>
create_retained_native_runtime(RetainedNativeRuntimeInput input,
                               RetainedNativeRuntimeConfig config);

} // namespace manyforge::roboplan_backend

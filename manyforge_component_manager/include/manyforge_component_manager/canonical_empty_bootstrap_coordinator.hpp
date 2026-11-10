#pragma once

#include <stdexcept>

#include <manyforge_component_manager/durable_store_set.hpp>
#include <manyforge_core/ports/imanager_scene_projection.hpp>

namespace manyforge::deployment {
class CanonicalEmptyRetainedBundleAdmission;
}

namespace manyforge::component_manager {

class CanonicalEmptyBootstrapCoordinatorError final
    : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

/// Bounded canonical-empty bootstrap and recovery coordinator.
///
/// This operation admits only the sealed canonical-empty package already
/// used to open the durable stores. Begin observes the scene owner's
/// command-fenced prerequisites before minting the first identity, durably
/// records the exact intent, reconciles Begin through Status, and records the
/// accepted target. Advance verifies that durable target and then coordinates
/// zero-native Rebuild, generation-1 publication, and Finalize through exact
/// independently re-read evidence.
///
/// It cannot accept native bindings and never invokes process, Kernel, ROS,
/// controller, robot, or native-projection effects directly.
class CanonicalEmptyBootstrapCoordinator final {
 public:
  CanonicalEmptyBootstrapCoordinator(
      ExternallyProvisionedManagerStores& stores,
      manyforge::core::IManagerSceneProjectionBootstrapPort& scene_owner)
      noexcept;

  [[nodiscard]] PersistedManagerBootstrapAcceptedTarget BeginOrRecover(
      const manyforge::deployment::
          CanonicalEmptyRetainedBundleAdmission& admission);

  /// Advances one already accepted canonical-empty bootstrap through the
  /// scene-owner Rebuild, durable publication, and scene-owner Finalize
  /// boundaries.
  ///
  /// One invocation performs at most one mutating scene-owner operation.
  /// Durable status and publication evidence are re-read before every
  /// recovery decision. Exact replay is accepted; absent, conflicting,
  /// corrupt, or uncertain evidence fails closed.
  [[nodiscard]] manyforge::core::ProjectionBootstrapStatusDTO
  AdvanceOrRecover(
      const manyforge::deployment::
          CanonicalEmptyRetainedBundleAdmission& admission);

  CanonicalEmptyBootstrapCoordinator(
      const CanonicalEmptyBootstrapCoordinator&) = delete;
  CanonicalEmptyBootstrapCoordinator& operator=(
      const CanonicalEmptyBootstrapCoordinator&) = delete;

 private:
  ExternallyProvisionedManagerStores& stores_;
  manyforge::core::IManagerSceneProjectionBootstrapPort& scene_owner_;
};

}  // namespace manyforge::component_manager

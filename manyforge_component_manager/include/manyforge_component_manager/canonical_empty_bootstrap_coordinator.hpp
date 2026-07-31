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

/// Bounded canonical-empty bootstrap Begin/recovery coordinator.
///
/// This operation admits only the sealed I3 canonical-empty package already
/// used to open the durable stores. It verifies an empty publication head,
/// observes the scene owner's command-fenced prerequisites before minting the
/// first identity, durably records the exact intent, reconciles Begin through
/// Status, and durably records the accepted target.
///
/// It cannot accept native bindings and never invokes Rebuild, Finalize,
/// publication, process, Kernel, ROS, controller, or native effects.
class CanonicalEmptyBootstrapCoordinator final {
 public:
  CanonicalEmptyBootstrapCoordinator(
      ExternallyProvisionedManagerStores& stores,
      manyforge::core::IManagerSceneProjectionBootstrapPort& scene_owner)
      noexcept;

  [[nodiscard]] PersistedManagerBootstrapAcceptedTarget BeginOrRecover(
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

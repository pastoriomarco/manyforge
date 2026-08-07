#pragma once

#include <manyforge_core/ports/ikinematic_state_view_provider.hpp>
#include <manyforge_core/ports/iobserved_state_provider.hpp>
#include <memory>
#include <vector>

namespace manyforge::core {

struct KinematicObservedStateSourceBinding final {
  ControlBindingId control_binding_id;
  ControlBindingRevision control_binding_revision;
  std::shared_ptr<const IObservedStateProvider> provider;
};

/// Planner-neutral immutable join of compiler-derived path dependencies and
/// exact owner-issued observed-state publications.
///
/// The registry is fixed at construction. A local channel and a future
/// external/ROS adapter can implement the same observed-state port; placement
/// never changes the path/view contract.
class ImmutableKinematicStateViewProvider final : public IKinematicStateViewProvider {
 public:
  explicit ImmutableKinematicStateViewProvider(
      std::vector<KinematicObservedStateSourceBinding> sources);

  [[nodiscard]] Result<KinematicStatePathViewDTO> current_kinematic_state_path(
      const KinematicStatePathQueryDTO& query, std::int64_t now_ns) const override;

 private:
  std::vector<KinematicObservedStateSourceBinding> sources_;
};

}  // namespace manyforge::core

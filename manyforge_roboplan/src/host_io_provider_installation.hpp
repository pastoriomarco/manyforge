#pragma once

#include "host_io_output_channels.hpp"
#include "ros_host_composition.hpp"
#include <map>
#include <span>

namespace manyforge::roboplan_backend::host_detail {

// Call-local pins captured before an in-place publication-view refresh. The
// exact held parent remains the authority; these values are not a registry.
struct CompositionIoResourcePredecessor final {
  std::shared_ptr<const deployment::RetainedBundle> bundle;
  core::ActiveBundlePublicationGeneration publication_generation;
  std::vector<std::shared_ptr<const IoProviderDriverGroup>> driver_groups;
  std::vector<std::shared_ptr<HostIoOutputChannelEntry>> channels;
  std::shared_ptr<HostIoSignalRuntime> runtime;
};
[[nodiscard]] std::vector<CompositionIoResourcePredecessor>
CaptureCompositionIoResourcePredecessors(
    std::span<const Composition *const> roots);

// Caller holds the existing exact Apply/composition effect pin. Existing roots
// are the actual lifetime-pinned current/retired/prepared owners, never wire
// projections. New resources are paused; retained source/alias replacement is
// an actual Apply effect, including the approved once-publication model reset.
[[nodiscard]] core::Result<void> PrepareCompositionIoProviderResources(
    Composition &candidate, RetainedCellRuntime &cell,
    rclcpp::Node::SharedPtr generic_node,
    std::span<const CompositionIoResourcePredecessor> actual_existing);

} // namespace manyforge::roboplan_backend::host_detail

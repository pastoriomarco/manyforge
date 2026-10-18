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
    Composition &candidate, RetainedNativeCellRuntime &cell,
    rclcpp::Node::SharedPtr generic_node,
    std::span<const CompositionIoResourcePredecessor> actual_existing);

using IoProviderConstructionGenerations =
    std::map<core::ControllableComponentId, core::ComponentLifecycleGeneration>;

// Generations must come from the actual accepted CCLC child/recorded Apply
// plan, or the actual published facade's current snapshot. This constructs
// CLOSED binding metadata only; no driver/readiness/admission fact is
// manufactured.
[[nodiscard]] core::Result<void> BindCompositionIoOutputParticipants(
    Composition &candidate,
    const IoProviderConstructionGenerations &actual_generations);

} // namespace manyforge::roboplan_backend::host_detail

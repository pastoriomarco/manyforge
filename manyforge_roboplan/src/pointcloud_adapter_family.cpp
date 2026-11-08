// The vendor-neutral point-cloud source adapter family `pointcloud`:
// one library, registered in the ament resource index
// `manyforge_component_adapters`. A `kind: sensor` model with sensor type
// `pointcloud` names it in its one adapter row (a sensor has no lane); the
// Deployment's settings arrive set over the row (topic, source frame, limits).

#include <manyforge_roboplan/component_adapter_registry.hpp>
#include <manyforge_roboplan/pointcloud_source_adapter.hpp>

#include "ros_pointcloud_bridge_link.hpp"

namespace {

manyforge::roboplan_backend::ComponentAdapters make_pointcloud_adapter(
    const manyforge::roboplan_backend::ComponentAdapterContext &context) {
  using namespace manyforge::roboplan_backend;
  auto link = std::make_shared<RosPointCloudBridgeLink>(
      context.node, context.ros_namespace, context.clock, context.ros_entity_owner);
  return {std::make_shared<PointCloudSourceAdapter>(
              pointcloud_source_settings(context.lane_row),
              context.ros_namespace, context.cell_id, context.start_process,
              link),
          {},
          std::nullopt,
          std::nullopt,
          context.ros_entity_owner ? link : nullptr};
}

// The bridge is the component's own process (the host's component process
// launcher), not a ros2_control lane: no launch contribution.
constexpr manyforge::roboplan_backend::ComponentAdapterFamily kFamily{
    "pointcloud", &make_pointcloud_adapter, nullptr, nullptr};

} // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}

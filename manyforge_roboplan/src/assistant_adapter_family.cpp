// The assistant adapter family `assistant`: one library, registered in the
// ament resource index `manyforge_component_adapters`. A `kind: assistant`
// model names it in its one adapter row (the assistant has no lane); the
// Deployment's settings arrive set over the row (profile and model keys,
// which the adapter doesn't read).

#include <manyforge_roboplan/assistant_component_adapter.hpp>
#include <manyforge_roboplan/component_adapter_registry.hpp>

#include "ros_assistant_host_link.hpp"

namespace {

manyforge::roboplan_backend::ComponentAdapters make_assistant_adapter(
    const manyforge::roboplan_backend::ComponentAdapterContext &context) {
  using namespace manyforge::roboplan_backend;
  auto link = std::make_shared<RosAssistantHostLink>(
      context.node, context.ros_namespace, context.clock, context.ros_entity_owner);
  return {std::make_shared<AssistantComponentAdapter>(
              assistant_settings(context.lane_row), context.ros_namespace,
              context.start_process, link, context.clock,
              context.lifecycle_command_timeout),
          {},
          std::nullopt,
          std::nullopt,
          context.ros_entity_owner ? link : nullptr};
}

// The host is the component's own process (the host's component process
// launcher), not a ros2_control lane: no launch contribution.
constexpr manyforge::roboplan_backend::ComponentAdapterFamily kFamily{
    "assistant", &make_assistant_adapter, nullptr, nullptr};

} // namespace

extern "C" const manyforge::roboplan_backend::ComponentAdapterFamily *
manyforge_component_adapter_family() {
  return &kFamily;
}

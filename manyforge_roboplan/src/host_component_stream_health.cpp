#include <manyforge_roboplan/host_component_stream_health.hpp>

#include <manyforge_roboplan/execution_host_support.hpp>

namespace manyforge::roboplan_backend {

std::shared_ptr<core::ComponentStreamHealth> host_component_stream_health() {
  // Never destroyed: adapters and scheduler callbacks may outlive static
  // destruction order at process exit.
  struct Holder final {
    std::shared_ptr<WallClock> clock = std::make_shared<WallClock>();
    SimpleScheduler scheduler;
    std::shared_ptr<core::ComponentStreamHealth> health =
        std::make_shared<core::ComponentStreamHealth>(clock, scheduler);
  };
  static Holder* const holder = new Holder();
  return holder->health;
}

}  // namespace manyforge::roboplan_backend

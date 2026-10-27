#pragma once

#include <manyforge_core/component_stream_health.hpp>

#include <memory>

namespace manyforge::roboplan_backend {

/// The execution host's one CP9 stream-health registry: the sink every C++
/// adapter of this process declares its streams into, and the source the
/// CCLC composition reacts to. Created on first use with its own steady
/// clock and scheduler; lives until process exit.
[[nodiscard]] std::shared_ptr<core::ComponentStreamHealth> host_component_stream_health();

}  // namespace manyforge::roboplan_backend

#include <manyforge_core/runtime_route_configuration.hpp>
#include <tuple>

namespace manyforge::core {
namespace {

constexpr std::size_t kMaxRouteIdentityBytes = 256U;

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& value) noexcept {
  return !value.empty() && value.value().size() <= kMaxRouteIdentityBytes;
}

}  // namespace

bool RouteReadinessPublicationRowDTO::locally_valid() const noexcept {
  return bounded_identity(controllable_component_id) && component_lifecycle_generation.nonzero() &&
         bounded_identity(control_binding_id) && bounded_identity(control_binding_revision) &&
         bounded_identity(resolved_stop_base_plan_id);
}

bool CanonicalRouteReadinessPublicationRowLess(
    const RouteReadinessPublicationRowDTO& lhs,
    const RouteReadinessPublicationRowDTO& rhs) noexcept {
  return std::tie(lhs.control_binding_id, lhs.control_binding_revision,
                  lhs.controllable_component_id) < std::tie(rhs.control_binding_id,
                                                            rhs.control_binding_revision,
                                                            rhs.controllable_component_id);
}

}  // namespace manyforge::core

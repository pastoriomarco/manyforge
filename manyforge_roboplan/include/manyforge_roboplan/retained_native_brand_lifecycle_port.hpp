#pragma once

#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <functional>
#include <memory>
#include <optional>

namespace manyforge::roboplan_backend {

/// Observation adapter for one constructed in-process native domain.
///
/// The in-process native engine is already constructed by Manager. Each
/// observation verifies the exact engine and, for Active, current route
/// readiness. The adapter owns provider readiness only; it never changes
/// Kernel command eligibility, which CCLC publishes for the whole component.
class RetainedNativeBrandLifecyclePort final
    : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedNativeBrandLifecyclePort>>
  Create(core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
         core::BrandLifecyclePlanCatalogDTO verified_catalog,
         core::IClock &clock, core::Duration observation_freshness_ns,
         std::function<core::Result<void>()> verify_engine,
         std::function<core::Result<bool>()> route_ready,
         std::optional<core::ComponentLifecycleStateDTO> retained_state,
         bool allow_closed_active_during_route_recovery = false);

  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override;

  /// Same native classification as Observe, without advancing its bookkeeping.
  [[nodiscard]] core::Result<core::LifecycleDomainHealthDTO>
  ReadLifecycleHealth(const core::ControlDomainId &domain,
                      core::TimePoint now_ns) const;

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override;

private:
  struct State;

  explicit RetainedNativeBrandLifecyclePort(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend

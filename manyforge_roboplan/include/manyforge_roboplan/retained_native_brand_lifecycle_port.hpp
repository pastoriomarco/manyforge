#pragma once

#include <manyforge_core/ports/ibrand_lifecycle_port.hpp>
#include <manyforge_core/time/iclock.hpp>
#include <manyforge_core/types/component_lifecycle.hpp>
#include <memory>

namespace manyforge::roboplan_backend {

/// Deterministic native adapter for a retained declarative lifecycle profile.
///
/// This adapter is intentionally limited to in-process retained product
/// fixtures. CCLC remains the sole owner of selection, retries, deadlines,
/// reconciliation, and durable transaction truth. The adapter owns only the
/// current normalized product observation and one-wave primitive effects.
class RetainedNativeBrandLifecyclePort final
    : public core::IBrandLifecyclePort {
public:
  [[nodiscard]] static core::Result<
      std::shared_ptr<RetainedNativeBrandLifecyclePort>>
  Create(core::ResolvedLifecycleProviderOccurrenceDTO provider_occurrence,
         core::BrandLifecyclePlanCatalogDTO verified_catalog,
         core::IClock &clock, core::Duration observation_freshness_ns);

  [[nodiscard]] core::Result<core::BrandLifecycleObservationDTO>
  Observe(const core::BrandLifecycleObservationQueryDTO &query) override;

  [[nodiscard]] core::BrandLifecyclePrimitiveResultDTO
  Dispatch(const core::BrandLifecyclePrimitiveIntentDTO &intent,
           std::stop_token cancellation) override;

private:
  struct State;

  explicit RetainedNativeBrandLifecyclePort(std::shared_ptr<State> state);

  std::shared_ptr<State> state_;
};

} // namespace manyforge::roboplan_backend


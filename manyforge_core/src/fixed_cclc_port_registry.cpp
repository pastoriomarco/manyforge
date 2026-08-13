#include <algorithm>
#include <manyforge_core/fixed_cclc_port_registry.hpp>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>

namespace manyforge::core {
namespace {

[[nodiscard]] auto brand_key(const ResolvedLifecycleProviderOccurrenceDTO& value) noexcept {
  return std::tie(value.control_domain_id, value.control_domain_revision,
                  value.provider_instance_id, value.provider_manifest_id,
                  value.provider_manifest_revision, value.provider_runtime_unit_id,
                  value.provider_runtime_epoch, value.lifecycle_plan_catalog_digest);
}

[[nodiscard]] auto controller_key(const ControllerLifecycleObservationQueryDTO& value) noexcept {
  return std::tie(value.control_domain_id, value.control_domain_revision,
                  value.controller_topology_id, value.topology_revision, value.product_id,
                  value.product_contract_id, value.product_contract_revision,
                  value.control_artifact_digest, value.source_conformance_digest,
                  value.source_runtime_unit_id, value.source_runtime_epoch);
}

template <typename Value>
[[nodiscard]] Result<Value> missing(std::string reason, std::string message) {
  return Result<Value>::Err(ErrorDTO{std::string(to_string(ErrorCode::kMissingReference)),
                                     std::move(message),
                                     {{"reason", std::move(reason)}}});
}

}  // namespace

FixedCCLCPortRegistry::FixedCCLCPortRegistry(
    std::vector<BrandLifecyclePortRegistrationDTO> brand_ports,
    std::vector<ControllerLifecyclePortRegistrationDTO> controller_ports)
    : brand_ports_(std::move(brand_ports)), controller_ports_(std::move(controller_ports)) {
  std::sort(brand_ports_.begin(), brand_ports_.end(), [](const auto& lhs, const auto& rhs) {
    return brand_key(lhs.provider_occurrence) < brand_key(rhs.provider_occurrence);
  });
  std::sort(controller_ports_.begin(), controller_ports_.end(),
            [](const auto& lhs, const auto& rhs) {
              return controller_key(lhs.query) < controller_key(rhs.query);
            });
  const bool invalid_brand =
      std::any_of(brand_ports_.begin(), brand_ports_.end(), [](const auto& registration) {
        return !registration.provider_occurrence.locally_valid() || registration.port == nullptr;
      });
  const bool duplicate_brand =
      std::adjacent_find(
          brand_ports_.begin(), brand_ports_.end(), [](const auto& lhs, const auto& rhs) {
            return brand_key(lhs.provider_occurrence) == brand_key(rhs.provider_occurrence);
          }) != brand_ports_.end();
  const bool invalid_controller =
      std::any_of(controller_ports_.begin(), controller_ports_.end(), [](const auto& registration) {
        return !registration.query.locally_valid() || registration.port == nullptr;
      });
  const bool duplicate_controller =
      std::adjacent_find(controller_ports_.begin(), controller_ports_.end(),
                         [](const auto& lhs, const auto& rhs) {
                           return controller_key(lhs.query) == controller_key(rhs.query);
                         }) != controller_ports_.end();
  if (invalid_brand || duplicate_brand || invalid_controller || duplicate_controller) {
    throw std::invalid_argument(
        "fixed CCLC port registry contains an invalid or duplicate exact "
        "registration");
  }
}

Result<std::shared_ptr<IBrandLifecyclePort>> FixedCCLCPortRegistry::ResolveExactLifecyclePort(
    const ResolvedLifecycleProviderOccurrenceDTO& provider_occurrence) const {
  if (!provider_occurrence.locally_valid()) {
    return missing<std::shared_ptr<IBrandLifecyclePort>>(
        "provider_occurrence_invalid",
        "lifecycle resolver received an invalid exact provider occurrence");
  }
  const auto found =
      std::lower_bound(brand_ports_.begin(), brand_ports_.end(), provider_occurrence,
                       [](const auto& registration, const auto& requested) {
                         return brand_key(registration.provider_occurrence) < brand_key(requested);
                       });
  if (found == brand_ports_.end() || found->provider_occurrence != provider_occurrence) {
    return missing<std::shared_ptr<IBrandLifecyclePort>>(
        "provider_occurrence_not_registered",
        "no lifecycle adapter is registered for the exact provider "
        "occurrence");
  }
  return Result<std::shared_ptr<IBrandLifecyclePort>>::Ok(found->port);
}

Result<std::shared_ptr<IControllerLifecyclePort>>
FixedCCLCPortRegistry::ResolveExactControllerLifecyclePort(
    const ControllerLifecycleObservationQueryDTO& query) const {
  if (!query.locally_valid()) {
    return missing<std::shared_ptr<IControllerLifecyclePort>>(
        "controller_source_query_invalid",
        "controller resolver received an invalid exact source query");
  }
  const auto found =
      std::lower_bound(controller_ports_.begin(), controller_ports_.end(), query,
                       [](const auto& registration, const auto& requested) {
                         return controller_key(registration.query) < controller_key(requested);
                       });
  if (found == controller_ports_.end() || found->query != query) {
    return missing<std::shared_ptr<IControllerLifecyclePort>>(
        "controller_source_not_registered",
        "no controller lifecycle adapter is registered for the exact source "
        "query");
  }
  return Result<std::shared_ptr<IControllerLifecyclePort>>::Ok(found->port);
}

}  // namespace manyforge::core

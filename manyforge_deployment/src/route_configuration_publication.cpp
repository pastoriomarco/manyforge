#include <manyforge_deployment/route_configuration_publication.hpp>

#include <algorithm>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_deployment/canonical_value.hpp>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

namespace manyforge::deployment {
namespace {

constexpr std::uint64_t kMaximumSafeUnsignedInteger = 9'007'199'254'740'991ULL;
constexpr std::int64_t kMaximumSafeSignedInteger = 9'007'199'254'740'991LL;

template <typename Value>
[[nodiscard]] core::Result<Value> publication_error(std::string reason,
                                                    std::string message) {
  return core::Result<Value>::Err(core::ErrorDTO{
      std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
      std::move(message),
      {{"reason", std::move(reason)}}});
}

[[nodiscard]] bool expected_domain_matches_acceptance(
    const core::RouteSelectionAcceptedDTO &acceptance,
    const core::ControllerLifecycleExpectedDomainDTO &expected) noexcept {
  return !acceptance.accepted_retained_bundle_id.empty() &&
         !acceptance.accepted_graph_revision.empty() &&
         expected.desired_configuration == acceptance.desired_configuration &&
         expected.control_domain_id ==
             acceptance.desired_configuration.control_domain_id &&
         expected.control_domain_revision ==
             acceptance.desired_configuration.control_domain_revision &&
         expected.controller_topology_id ==
             acceptance.desired_configuration.controller_topology_id &&
         expected.topology_revision ==
             acceptance.desired_configuration.topology_revision;
}

[[nodiscard]] bool receipt_integral_values_are_safe(
    const core::RouteReadinessPublicationReceiptDTO &receipt) noexcept {
  if (receipt.route_selection_transaction_generation.value() >
          kMaximumSafeUnsignedInteger ||
      receipt.active_control_configuration_generation.value() >
          kMaximumSafeUnsignedInteger ||
      receipt.published_at_ns < 0 ||
      receipt.published_at_ns > kMaximumSafeSignedInteger) {
    return false;
  }
  return std::all_of(receipt.readiness_rows.begin(),
                     receipt.readiness_rows.end(), [](const auto &row) {
                       return row.component_lifecycle_generation.value() <=
                              kMaximumSafeUnsignedInteger;
                     });
}

[[nodiscard]] CanonicalValue
publication_row_value(const core::RouteReadinessPublicationRowDTO &row) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_lifecycle_generation",
       CanonicalValue(
           static_cast<double>(row.component_lifecycle_generation.value()))},
      {"controllable_component_id",
       CanonicalValue(row.controllable_component_id.value())},
      {"control_binding_id", CanonicalValue(row.control_binding_id.value())},
      {"control_binding_revision",
       CanonicalValue(row.control_binding_revision.value())},
      {"resolved_stop_base_plan_id",
       CanonicalValue(row.resolved_stop_base_plan_id.value())},
  });
}

[[nodiscard]] core::ContentDigest publication_receipt_digest(
    const core::RouteReadinessPublicationReceiptDTO &receipt) {
  CanonicalValue::Array rows;
  rows.reserve(receipt.readiness_rows.size());
  for (const auto &row : receipt.readiness_rows) {
    rows.push_back(publication_row_value(row));
  }
  const CanonicalValue body(CanonicalValue::Object{
      {"active_control_configuration_generation",
       CanonicalValue(static_cast<double>(
           receipt.active_control_configuration_generation.value()))},
      {"active_control_configuration_id",
       CanonicalValue(receipt.active_control_configuration_id.value())},
      {"operation_admission_open",
       CanonicalValue(receipt.operation_admission_open)},
      {"published_at_ns",
       CanonicalValue(static_cast<double>(receipt.published_at_ns))},
      {"readiness_rows", CanonicalValue(std::move(rows))},
      {"route_selection_transaction_generation",
       CanonicalValue(static_cast<double>(
           receipt.route_selection_transaction_generation.value()))},
      {"route_selection_transaction_id",
       CanonicalValue(receipt.route_selection_transaction_id.value())},
  });
  return core::ContentDigest(
      content_identity::sha256_digest(content_identity::canonical_json(body)));
}

[[nodiscard]] bool
canonical_rows_equal(std::vector<core::RouteReadinessPublicationRowDTO> left,
                     std::vector<core::RouteReadinessPublicationRowDTO> right) {
  const auto less = core::CanonicalRouteReadinessPublicationRowLess;
  std::sort(left.begin(), left.end(), less);
  std::sort(right.begin(), right.end(), less);
  return left == right;
}

[[nodiscard]] core::Result<
    std::vector<core::KinematicResourceSourceProjectionDTO>>
select_domain_source_projections(
    const core::DesiredControlConfigurationDTO &configuration,
    const std::vector<core::KinematicResourceSourceProjectionDTO>
        &cell_projections) {
  std::set<core::ControlBindingId> selected_bindings;
  for (const auto &route : configuration.binding_routes) {
    selected_bindings.insert(route.control_binding_id);
  }

  std::vector<core::KinematicResourceSourceProjectionDTO> result;
  result.reserve(cell_projections.size());
  for (const auto &projection : cell_projections) {
    bool contains_selected = false;
    bool contains_unselected = false;
    for (const auto &source : projection.contributing_control_bindings) {
      if (selected_bindings.contains(source.control_binding_id)) {
        contains_selected = true;
      } else {
        contains_unselected = true;
      }
    }
    if (contains_selected && contains_unselected) {
      return publication_error<
          std::vector<core::KinematicResourceSourceProjectionDTO>>(
          "route_publication_cross_domain_source_projection",
          "one kinematic resource projection mixes selected and foreign "
          "control bindings");
    }
    if (contains_selected) {
      result.push_back(projection);
    }
  }
  return core::Result<std::vector<core::KinematicResourceSourceProjectionDTO>>::
      Ok(std::move(result));
}

} // namespace

VerifiedRouteConfigurationPublicationPort::
    VerifiedRouteConfigurationPublicationPort(
        const RetainedBundle &retained_bundle,
        const IVerifiedRetainedProductArtifactSource &artifact_source,
        core::IResolvedStopBasePlanRepository &plan_repository,
        IRouteConfigurationKernelPublicationOwner &kernel_owner,
        std::vector<core::KinematicResourceSourceProjectionDTO>
            source_projections,
        std::function<std::int64_t()> now_ns)
    : retained_bundle_(retained_bundle), artifact_source_(artifact_source),
      plan_repository_(plan_repository), kernel_owner_(kernel_owner),
      source_projections_(std::move(source_projections)),
      now_ns_(std::move(now_ns)) {
  if (!verify_retained_bundle_identity(retained_bundle_) || !now_ns_) {
    throw std::invalid_argument(
        "route-configuration publisher has an invalid retained owner");
  }
}

core::Result<core::ActiveControlConfigurationDTO>
VerifiedRouteConfigurationPublicationPort::
    ResolveResultingActiveControlConfiguration(
        const core::RouteSelectionAcceptedDTO &acceptance,
        const core::ControllerLifecycleExpectedDomain
            &expected_controller_domain,
        const core::ControllerLifecycleObservationDTO &observation,
        const core::ControllerLifecycleReconciliationDTO &reconciliation,
        const core::RouteSelectionEffectDTO transaction_effect) const {
  if (!acceptance.locally_valid() ||
      acceptance.accepted_retained_bundle_id !=
          retained_bundle_.retained_bundle_id() ||
      acceptance.accepted_graph_revision !=
          retained_bundle_.value().cell_graph.graph_revision ||
      !expected_domain_matches_acceptance(acceptance,
                                          expected_controller_domain.value())) {
    return publication_error<core::ActiveControlConfigurationDTO>(
        "route_publication_acceptance_mismatch",
        "route publication does not belong to its retained acceptance");
  }
  auto domain_projections = select_domain_source_projections(
      acceptance.desired_configuration, source_projections_);
  if (!domain_projections.ok()) {
    return core::Result<core::ActiveControlConfigurationDTO>::Err(
        domain_projections.error());
  }
  return core::ResolveRouteSelectionActiveControlConfiguration(
      acceptance, expected_controller_domain, observation, reconciliation,
      transaction_effect, std::move(domain_projections.value()));
}

core::Result<core::RouteReadinessPublicationReceiptDTO>
VerifiedRouteConfigurationPublicationPort::PublishAndReadBack(
    const core::RouteSelectionAcceptedDTO &acceptance,
    const core::ControllerLifecycleExpectedDomain &expected_controller_domain,
    const core::ActiveControlConfigurationDTO &durable_configuration) {
  if (!acceptance.locally_valid() || !durable_configuration.locally_valid() ||
      acceptance.accepted_retained_bundle_id !=
          retained_bundle_.retained_bundle_id() ||
      acceptance.accepted_graph_revision !=
          retained_bundle_.value().cell_graph.graph_revision ||
      !expected_domain_matches_acceptance(acceptance,
                                          expected_controller_domain.value()) ||
      durable_configuration.active_control_configuration_id !=
          acceptance.candidate_active_control_configuration_id ||
      durable_configuration.active_control_configuration_generation !=
          acceptance.candidate_active_control_configuration_generation) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_durable_configuration_mismatch",
        "durable route configuration differs from its exact acceptance");
  }

  std::vector<VerifiedRouteReadinessParticipant> participants;
  std::vector<core::RouteReadinessPublicationRowDTO> expected_rows;
  if (durable_configuration.state ==
      core::ActiveControlConfigurationStateDTO::kReconciled) {
    auto current =
        kernel_owner_.ReadCurrentPhysicalParticipants(durable_configuration);
    if (!current.ok()) {
      return core::Result<core::RouteReadinessPublicationReceiptDTO>::Err(
          current.error());
    }
    auto resolved = resolve_store_and_verify_route_readiness_batch(
        retained_bundle_, artifact_source_, expected_controller_domain,
        durable_configuration, acceptance.component_expectations,
        current.value(), plan_repository_);
    if (!resolved.ok()) {
      return core::Result<core::RouteReadinessPublicationReceiptDTO>::Err(
          resolved.error());
    }
    participants = std::move(resolved.value());
    expected_rows.reserve(participants.size());
    for (const auto &participant : participants) {
      expected_rows.push_back(participant.readiness_row);
    }
  } else if (durable_configuration.state !=
             core::ActiveControlConfigurationStateDTO::
                 kExternalOrUnknownUnreconciled) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_configuration_state_invalid",
        "route publication has an unknown configuration state");
  }

  // Route selection installs geometry and Stop readiness only. Component
  // command eligibility is CCLC lifecycle state, published separately; route
  // selection neither enables a Disabled component nor closes an enabled peer.
  const bool reconciled = durable_configuration.state ==
                          core::ActiveControlConfigurationStateDTO::kReconciled;
  auto readback = kernel_owner_.PublishAndReadBackRouteConfiguration(
      durable_configuration, std::move(participants));
  if (!readback.ok()) {
    return core::Result<core::RouteReadinessPublicationReceiptDTO>::Err(
        readback.error());
  }
  if (readback.value().active_configuration != durable_configuration ||
      readback.value().operation_admission_open != reconciled ||
      !canonical_rows_equal(readback.value().readiness_rows, expected_rows)) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_kernel_readback_mismatch",
        "Kernel registration readback differs from the published route truth");
  }

  const auto published_at_ns = now_ns_();
  if (published_at_ns < 0 || published_at_ns > kMaximumSafeSignedInteger) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_time_invalid",
        "route publication clock returned a non-I-JSON-safe time");
  }
  core::RouteReadinessPublicationReceiptDTO receipt{
      acceptance.route_selection_transaction_id,
      acceptance.route_selection_transaction_generation,
      durable_configuration.active_control_configuration_id,
      durable_configuration.active_control_configuration_generation,
      std::move(expected_rows),
      reconciled,
      published_at_ns,
      core::ContentDigest("pending")};
  if (!receipt_integral_values_are_safe(receipt)) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_integer_invalid",
        "route publication contains a non-I-JSON-safe integer");
  }
  receipt.receipt_digest = publication_receipt_digest(receipt);
  if (!receipt.locally_valid()) {
    return publication_error<core::RouteReadinessPublicationReceiptDTO>(
        "route_publication_receipt_invalid",
        "route publication produced an invalid readback receipt");
  }
  return core::Result<core::RouteReadinessPublicationReceiptDTO>::Ok(
      std::move(receipt));
}

} // namespace manyforge::deployment

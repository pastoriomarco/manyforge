#pragma once

#include <optional>
#include <variant>

#include <manyforge_core/types/projection_common.hpp>

namespace manyforge::core {

struct NamedApplyPublicationTransactionDTO final {
  ApplyId apply_id;
  ApplyGeneration apply_generation;
  ProjectionStageGeneration projection_stage_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !apply_id.empty() && apply_generation.nonzero() &&
           projection_stage_generation.nonzero();
  }

  friend bool operator==(const NamedApplyPublicationTransactionDTO&,
                         const NamedApplyPublicationTransactionDTO&) = default;
};

struct BootstrapPublicationTransactionDTO final {
  BootstrapId bootstrap_id;
  BootstrapGeneration bootstrap_generation;

  [[nodiscard]] bool valid() const noexcept {
    return !bootstrap_id.empty() && bootstrap_generation.nonzero();
  }

  friend bool operator==(const BootstrapPublicationTransactionDTO&,
                         const BootstrapPublicationTransactionDTO&) = default;
};

using ActiveBundlePublicationTransactionRefDTO =
    std::variant<NamedApplyPublicationTransactionDTO,
                 BootstrapPublicationTransactionDTO>;

struct ActiveBundlePublicationRequestDTO final {
  ActiveBundlePublicationTransactionRefDTO transaction_ref;
  ProjectionRevisionDTO candidate_projection_revision;
  RetainedBundleId candidate_retained_bundle_id;
  GraphRevision candidate_graph_revision;
  std::optional<ActiveBundlePublicationGeneration>
      expected_previous_publication_generation;
  ContentDigest private_materialization_content_digest;
  ContentDigest
      obsolete_managed_command_surfaces_retired_evidence_digest;

  [[nodiscard]] bool valid() const noexcept {
    const bool identities_match =
        candidate_projection_revision.valid() &&
        candidate_retained_bundle_id ==
            candidate_projection_revision.retained_bundle_id &&
        candidate_graph_revision ==
            candidate_projection_revision.graph_revision;
    const bool is_named_apply =
        std::holds_alternative<NamedApplyPublicationTransactionDTO>(
            transaction_ref);
    const bool transaction_valid = std::visit(
        [](const auto& transaction) { return transaction.valid(); },
        transaction_ref);
    const bool previous_generation_valid =
        !expected_previous_publication_generation.has_value() ||
        expected_previous_publication_generation->nonzero();
    return identities_match && transaction_valid &&
           previous_generation_valid &&
           is_named_apply ==
               expected_previous_publication_generation.has_value() &&
           !private_materialization_content_digest.empty() &&
           !obsolete_managed_command_surfaces_retired_evidence_digest.empty();
  }

  friend bool operator==(const ActiveBundlePublicationRequestDTO&,
                         const ActiveBundlePublicationRequestDTO&) = default;
};

struct ActiveBundlePublicationReceiptDTO final {
  ActiveBundlePublicationRequestDTO request;
  ActiveBundlePublicationGeneration publication_generation;
  ContentDigest durable_public_pointer_content_digest;

  [[nodiscard]] bool valid() const noexcept {
    return request.valid() && publication_generation.nonzero() &&
           !durable_public_pointer_content_digest.empty();
  }

  [[nodiscard]] bool matches_named_apply(
      const ApplyId& apply_id, ApplyGeneration apply_generation,
      ProjectionStageGeneration stage_generation,
      const ProjectionRevisionDTO& candidate_revision) const noexcept {
    const auto* transaction =
        std::get_if<NamedApplyPublicationTransactionDTO>(
            &request.transaction_ref);
    return valid() && transaction != nullptr &&
           transaction->apply_id == apply_id &&
           transaction->apply_generation == apply_generation &&
           transaction->projection_stage_generation == stage_generation &&
           request.candidate_projection_revision == candidate_revision;
  }

  [[nodiscard]] bool matches_bootstrap(
      const BootstrapId& bootstrap_id,
      BootstrapGeneration bootstrap_generation,
      const ProjectionRevisionDTO& target) const noexcept {
    const auto* transaction =
        std::get_if<BootstrapPublicationTransactionDTO>(
            &request.transaction_ref);
    return valid() && transaction != nullptr &&
           transaction->bootstrap_id == bootstrap_id &&
           transaction->bootstrap_generation == bootstrap_generation &&
           request.candidate_projection_revision == target;
  }

  friend bool operator==(const ActiveBundlePublicationReceiptDTO&,
                         const ActiveBundlePublicationReceiptDTO&) = default;
};

}  // namespace manyforge::core

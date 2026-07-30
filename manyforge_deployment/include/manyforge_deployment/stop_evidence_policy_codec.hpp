#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/stop_evidence_policy.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class StopEvidencePolicyCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A stop-evidence policy whose exact build-qualified descriptor, semantic
/// identity, typed catalog reference, presence matrix, phase budget, and
/// evidence compatibility invariants have been verified.
///
/// This remains a declarative catalog fact. Qualification does not resolve a
/// provider-contract role, expand topology, select endpoints, prove a complete
/// invocation plan, prove that the authored all-of evidence is sufficient for
/// the selected command/provider, or authorize/execute Stop.
class QualifiedStopEvidencePolicy final {
 public:
  QualifiedStopEvidencePolicy(const QualifiedStopEvidencePolicy&) noexcept =
      default;
  QualifiedStopEvidencePolicy& operator=(
      const QualifiedStopEvidencePolicy&) noexcept = default;

  [[nodiscard]] const core::StopEvidencePolicyCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::StopEvidencePolicyDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::StopEvidencePolicyCatalogRecordRefDTO& reference_in,
         core::StopEvidencePolicyDTO value_in,
         CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::StopEvidencePolicyCatalogRecordRefDTO reference;
    core::StopEvidencePolicyDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedStopEvidencePolicy(
      const core::StopEvidencePolicyCatalogRecordRefDTO& reference,
      core::StopEvidencePolicyDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedStopEvidencePolicy resolve_and_qualify_stop_evidence_policy(
      const core::StopEvidencePolicyCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves one exact user-authored policy through the complete typed catalog
/// boundary. It verifies the schema selector, stored-byte digest/retrieval
/// key, semantic record identity, typed ID/revision, and every policy-local
/// invariant. Cross-record/provider/topology/active-configuration resolution
/// deliberately belongs to the deployment compiler and readiness/admission
/// path, where one complete invocation plan and command/evidence sufficiency
/// per legal route can be proven.
[[nodiscard]] QualifiedStopEvidencePolicy
resolve_and_qualify_stop_evidence_policy(
    const core::StopEvidencePolicyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/contact_policy.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class ContactPolicyCodecError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// Exact schema-, identity-, and semantics-qualified contact policy.
class QualifiedContactPolicy final {
public:
  QualifiedContactPolicy(const QualifiedContactPolicy &) noexcept = default;
  QualifiedContactPolicy &
  operator=(const QualifiedContactPolicy &) noexcept = default;

  [[nodiscard]] const core::ContactPolicyCatalogRecordRefDTO &
  reference() const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::ContactPolicyDTO &value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue &normalized_root() const noexcept {
    return data_->normalized_root;
  }

private:
  struct Data final {
    Data(const core::ContactPolicyCatalogRecordRefDTO &reference_in,
         core::ContactPolicyDTO value_in, CanonicalValue normalized_root_in)
        : reference(reference_in), value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::ContactPolicyCatalogRecordRefDTO reference;
    core::ContactPolicyDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedContactPolicy(
      const core::ContactPolicyCatalogRecordRefDTO &reference,
      core::ContactPolicyDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(reference, std::move(value),
                                           std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedContactPolicy resolve_and_qualify_contact_policy(
      const core::ContactPolicyCatalogRecordRefDTO &,
      const core::ContentAddressedStoreKey &, std::string_view);
};

[[nodiscard]] QualifiedContactPolicy resolve_and_qualify_contact_policy(
    const core::ContactPolicyCatalogRecordRefDTO &reference,
    const core::ContentAddressedStoreKey &resolved_store_key,
    std::string_view stored_bytes);

} // namespace manyforge::deployment

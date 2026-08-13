#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/runtime_policy.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class RuntimePolicyCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Exact schema-, identity-, and semantics-qualified runtime policy.
class QualifiedRuntimePolicy final {
 public:
  QualifiedRuntimePolicy(const QualifiedRuntimePolicy&) noexcept = default;
  QualifiedRuntimePolicy& operator=(const QualifiedRuntimePolicy&) noexcept =
      default;

  [[nodiscard]] const core::RuntimePolicyCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::RuntimePolicyDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::RuntimePolicyCatalogRecordRefDTO& reference_in,
         core::RuntimePolicyDTO value_in, CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::RuntimePolicyCatalogRecordRefDTO reference;
    core::RuntimePolicyDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedRuntimePolicy(
      const core::RuntimePolicyCatalogRecordRefDTO& reference,
      core::RuntimePolicyDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(reference, std::move(value),
                                           std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedRuntimePolicy resolve_and_qualify_runtime_policy(
      const core::RuntimePolicyCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

[[nodiscard]] QualifiedRuntimePolicy resolve_and_qualify_runtime_policy(
    const core::RuntimePolicyCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

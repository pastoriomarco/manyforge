#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class ProviderManifestCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A reusable provider declaration whose exact build-qualified schema,
/// semantic identity, typed catalog referent, capability-definition
/// references, provider-contract value schemas, and declaration-local
/// invariants have been verified.
///
/// Qualification does not create a provider instance, choose applicability,
/// resolve concrete endpoints, start a runtime, select a Stop policy, or prove
/// live readiness. Those are binding/compiler/runtime responsibilities.
class QualifiedProviderManifest final {
 public:
  QualifiedProviderManifest(const QualifiedProviderManifest&) noexcept =
      default;
  QualifiedProviderManifest& operator=(
      const QualifiedProviderManifest&) noexcept = default;

  [[nodiscard]] const core::ProviderManifestCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::ProviderManifestDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::ProviderManifestCatalogRecordRefDTO& reference_in,
         core::ProviderManifestDTO value_in,
         CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::ProviderManifestCatalogRecordRefDTO reference;
    core::ProviderManifestDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedProviderManifest(
      const core::ProviderManifestCatalogRecordRefDTO& reference,
      core::ProviderManifestDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedProviderManifest resolve_and_qualify_provider_manifest(
      const core::ProviderManifestCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves and qualifies one exact user-authored provider-manifest
/// declaration.
[[nodiscard]] QualifiedProviderManifest resolve_and_qualify_provider_manifest(
    const core::ProviderManifestCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/provider_manifest.hpp>
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
  [[nodiscard]] const core::ProviderManifestDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedProviderManifest(core::ProviderManifestDTO value,
                            CanonicalValue normalized_root)
      : value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::ProviderManifestDTO value_;
  CanonicalValue normalized_root_;

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

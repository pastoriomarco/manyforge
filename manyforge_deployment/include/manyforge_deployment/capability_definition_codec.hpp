#pragma once

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/catalog.hpp>

#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class CapabilityDefinitionCodecError : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

/// A capability definition whose descriptor, semantic identity, and complete
/// built-in-registry row have all been verified by this build.
class QualifiedCapabilityDefinition final {
 public:
  QualifiedCapabilityDefinition(const QualifiedCapabilityDefinition&) =
      default;
  QualifiedCapabilityDefinition& operator=(
      const QualifiedCapabilityDefinition&) = default;

  [[nodiscard]] const core::CapabilityDefinitionCatalogRecordRefDTO&
  reference() const noexcept {
    return reference_;
  }
  [[nodiscard]] const core::CapabilityDefinitionDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedCapabilityDefinition(
      const core::CapabilityDefinitionCatalogRecordRefDTO& reference,
      core::CapabilityDefinitionDTO value, CanonicalValue normalized_root)
      : reference_(reference),
        value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::CapabilityDefinitionCatalogRecordRefDTO reference_;
  core::CapabilityDefinitionDTO value_;
  CanonicalValue normalized_root_;

  friend QualifiedCapabilityDefinition
  resolve_and_qualify_capability_definition(
      const core::CapabilityDefinitionCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves the complete typed catalog-reference boundary for one exact byte
/// payload.
///
/// `resolved_store_key` is the key by which the caller actually retrieved
/// `stored_bytes`; it must equal the immutable reference. The function checks
/// the typed ID/revision pair, exact schema selector, byte digest, retrieval
/// key, payload schema, payload semantic identity, and normative registry row
/// before returning a qualified value.
[[nodiscard]] QualifiedCapabilityDefinition
resolve_and_qualify_capability_definition(
    const core::CapabilityDefinitionCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

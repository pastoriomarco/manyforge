#pragma once

#include <manyforge_core/types/capability_definition.hpp>
#include <manyforge_core/types/catalog.hpp>

#include <memory>
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
  QualifiedCapabilityDefinition(
      const QualifiedCapabilityDefinition&) noexcept = default;
  QualifiedCapabilityDefinition& operator=(
      const QualifiedCapabilityDefinition&) noexcept = default;

  [[nodiscard]] const core::CapabilityDefinitionCatalogRecordRefDTO&
  reference() const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::CapabilityDefinitionDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::CapabilityDefinitionCatalogRecordRefDTO& reference_in,
         core::CapabilityDefinitionDTO value_in,
         CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::CapabilityDefinitionCatalogRecordRefDTO reference;
    core::CapabilityDefinitionDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedCapabilityDefinition(
      const core::CapabilityDefinitionCatalogRecordRefDTO& reference,
      core::CapabilityDefinitionDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

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

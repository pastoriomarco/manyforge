#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/component_type.hpp>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/capability_definition_codec.hpp"

namespace manyforge::deployment {

class ComponentTypeCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A component-type declaration whose exact build-qualified schema, semantic
/// identity, typed catalog referent, target capability dependencies, and
/// declaration-local structural invariants have been verified.
///
/// Qualification deliberately does not select ModelAssets, prove asset-local
/// source roles, select control contracts/topologies/providers, generate
/// names, instantiate runtime units, or observe live state. Those facts belong
/// to the later graph compiler and retained deployment product.
class QualifiedComponentType final {
 public:
  QualifiedComponentType(const QualifiedComponentType&) noexcept = default;
  QualifiedComponentType& operator=(const QualifiedComponentType&) noexcept =
      default;

  [[nodiscard]] const core::ComponentTypeCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::ComponentTypeDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::ComponentTypeCatalogRecordRefDTO& reference_in,
         core::ComponentTypeDTO value_in, CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::ComponentTypeCatalogRecordRefDTO reference;
    core::ComponentTypeDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedComponentType(
      const core::ComponentTypeCatalogRecordRefDTO& reference,
      core::ComponentTypeDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedComponentType resolve_and_qualify_component_type(
      const core::ComponentTypeCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view,
      std::span<const QualifiedCapabilityDefinition>);
};

/// Resolves and qualifies one exact user-authored component type.
///
/// `qualified_capabilities` is the caller-prefiltered, duplicate-free,
/// bounded dependency closure selected by this declaration (at most 256 exact
/// ID/revision referents). Only target-binding capability definitions are
/// legal.
[[nodiscard]] QualifiedComponentType resolve_and_qualify_component_type(
    const core::ComponentTypeCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes,
    std::span<const QualifiedCapabilityDefinition> qualified_capabilities);

}  // namespace manyforge::deployment

#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/component_type.hpp>
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
  [[nodiscard]] const core::ComponentTypeDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedComponentType(core::ComponentTypeDTO value,
                         CanonicalValue normalized_root)
      : value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::ComponentTypeDTO value_;
  CanonicalValue normalized_root_;

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

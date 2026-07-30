#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class ModelAssetCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A model-asset declaration whose exact build-qualified schema, semantic
/// identity, typed catalog referent, and declaration-local invariants have
/// been verified.
///
/// Qualification deliberately does not retrieve model-source blobs, parse
/// URDF/SRDF, prove dependency closure or source symbols, rewrite names, or
/// claim compatibility with a runtime lane.
class QualifiedModelAsset final {
 public:
  [[nodiscard]] const core::ModelAssetDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedModelAsset(core::ModelAssetDTO value,
                      CanonicalValue normalized_root)
      : value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::ModelAssetDTO value_;
  CanonicalValue normalized_root_;

  friend QualifiedModelAsset resolve_and_qualify_model_asset(
      const core::ModelAssetCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves and qualifies one exact user-authored model-asset declaration.
///
/// `resolved_store_key` identifies only the catalog referent bytes and must
/// equal the immutable outer reference. Source-artifact retrieval is a later
/// compiler/store responsibility keyed by each declared content digest.
[[nodiscard]] QualifiedModelAsset resolve_and_qualify_model_asset(
    const core::ModelAssetCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

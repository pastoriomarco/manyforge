#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <memory>
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
  QualifiedModelAsset(const QualifiedModelAsset&) noexcept = default;
  QualifiedModelAsset& operator=(const QualifiedModelAsset&) noexcept =
      default;

  [[nodiscard]] const core::ModelAssetCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::ModelAssetDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::ModelAssetCatalogRecordRefDTO& reference_in,
         core::ModelAssetDTO value_in, CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::ModelAssetCatalogRecordRefDTO reference;
    core::ModelAssetDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedModelAsset(const core::ModelAssetCatalogRecordRefDTO& reference,
                      core::ModelAssetDTO value,
                      CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

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

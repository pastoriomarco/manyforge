#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/command_schema.hpp>

#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class CommandSchemaCodecError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// One exact command schema whose closed descriptor, semantic identity, and
/// build-owned I6 family row have all been verified.
class QualifiedCommandSchema final {
 public:
  QualifiedCommandSchema(const QualifiedCommandSchema&) noexcept = default;
  QualifiedCommandSchema& operator=(const QualifiedCommandSchema&) noexcept =
      default;

  [[nodiscard]] const core::CommandSchemaCatalogRecordRefDTO& reference()
      const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::CommandSchemaDTO& value() const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::CommandSchemaCatalogRecordRefDTO& reference_in,
         core::CommandSchemaDTO value_in, CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::CommandSchemaCatalogRecordRefDTO reference;
    core::CommandSchemaDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedCommandSchema(
      const core::CommandSchemaCatalogRecordRefDTO& reference,
      core::CommandSchemaDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(reference, std::move(value),
                                           std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedCommandSchema resolve_and_qualify_command_schema(
      const core::CommandSchemaCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

[[nodiscard]] QualifiedCommandSchema resolve_and_qualify_command_schema(
    const core::CommandSchemaCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

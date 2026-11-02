#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/control_interface_contract.hpp>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class ControlInterfaceContractCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A control-interface contract whose exact descriptor, semantic identity,
/// cross-field ordering invariants, and typed catalog referent have all been
/// verified by this build.
class QualifiedControlInterfaceContract final {
 public:
  QualifiedControlInterfaceContract(
      const QualifiedControlInterfaceContract&) noexcept = default;
  QualifiedControlInterfaceContract& operator=(
      const QualifiedControlInterfaceContract&) noexcept = default;

  [[nodiscard]] const core::ControlInterfaceContractCatalogRecordRefDTO&
  reference() const noexcept {
    return data_->reference;
  }
  [[nodiscard]] const core::ControlInterfaceContractDTO& value()
      const noexcept {
    return data_->value;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(const core::ControlInterfaceContractCatalogRecordRefDTO& reference_in,
         core::ControlInterfaceContractDTO value_in,
         CanonicalValue normalized_root_in)
        : reference(reference_in),
          value(std::move(value_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::ControlInterfaceContractCatalogRecordRefDTO reference;
    core::ControlInterfaceContractDTO value;
    CanonicalValue normalized_root;
  };

  QualifiedControlInterfaceContract(
      const core::ControlInterfaceContractCatalogRecordRefDTO& reference,
      core::ControlInterfaceContractDTO value, CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            reference, std::move(value), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

  friend QualifiedControlInterfaceContract
  resolve_and_qualify_control_interface_contract(
      const core::ControlInterfaceContractCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves one exact user-authored control-interface contract through its
/// complete typed catalog-reference boundary.
///
/// `resolved_store_key` is the key by which the caller actually retrieved
/// `stored_bytes`; it must equal the immutable reference. Qualification checks
/// the exact schema selector, byte digest, retrieval key, semantic identity,
/// typed ID/revision, and all cross-field role/order invariants. It does not
/// impose a build-owned whitelist of contract contents.
[[nodiscard]] QualifiedControlInterfaceContract
resolve_and_qualify_control_interface_contract(
    const core::ControlInterfaceContractCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

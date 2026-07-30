#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/runtime_profile.hpp>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

class RuntimeProfileCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// A runtime-profile declaration whose exact build-qualified schema,
/// semantic identity, typed catalog referent, selected configuration schema,
/// and declaration-local invariants have been verified.
///
/// Qualification does not select or start a process, resolve concrete
/// endpoints, assign providers, evaluate freshness/restart policy, or claim
/// live health/readiness.
class QualifiedRuntimeProfile final {
 public:
  QualifiedRuntimeProfile(const QualifiedRuntimeProfile&) = default;
  QualifiedRuntimeProfile& operator=(const QualifiedRuntimeProfile&) =
      default;

  [[nodiscard]] const core::RuntimeProfileCatalogRecordRefDTO& reference()
      const noexcept {
    return reference_;
  }
  [[nodiscard]] const core::RuntimeProfileDTO& value() const noexcept {
    return value_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  QualifiedRuntimeProfile(
      const core::RuntimeProfileCatalogRecordRefDTO& reference,
      core::RuntimeProfileDTO value, CanonicalValue normalized_root)
      : reference_(reference),
        value_(std::move(value)),
        normalized_root_(std::move(normalized_root)) {}

  core::RuntimeProfileCatalogRecordRefDTO reference_;
  core::RuntimeProfileDTO value_;
  CanonicalValue normalized_root_;

  friend QualifiedRuntimeProfile resolve_and_qualify_runtime_profile(
      const core::RuntimeProfileCatalogRecordRefDTO&,
      const core::ContentAddressedStoreKey&, std::string_view);
};

/// Resolves and qualifies one exact user-authored runtime-profile declaration.
[[nodiscard]] QualifiedRuntimeProfile resolve_and_qualify_runtime_profile(
    const core::RuntimeProfileCatalogRecordRefDTO& reference,
    const core::ContentAddressedStoreKey& resolved_store_key,
    std::string_view stored_bytes);

}  // namespace manyforge::deployment

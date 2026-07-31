#include "manyforge_deployment/verified_retained_bundle.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "manyforge_deployment/content_identity.hpp"
#include "posix_durability_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

namespace posix = detail::posix_durability;

[[nodiscard]] std::exception_ptr make_retention_error(
    std::string_view message) {
  return std::make_exception_ptr(RetainedBundleError(std::string(message)));
}

const posix::ErrorPolicy kPosixErrors{make_retention_error};
constexpr posix::TrustedRootMessages kTrustedRootMessages{
    "durable retention trusted root must not be empty",
    "cannot resolve durable retention trusted root",
    "durable retention trusted root must be an existing plain directory",
};
constexpr posix::ExistingDirectoryMessages kReadOnlyDirectoryMessages{
    "cannot inspect read-only durable retention directory",
    "read-only durable retention path must be an existing plain directory",
};
constexpr posix::ReadMessages kReadMessages{
    "cannot open durable retention entry",
    "cannot stat durable retention entry",
    "durable retention entry violates its immutable byte bound",
    "durable retention entry changed during exact read",
    "durable retention entry grew during exact read",
};

struct ReadOnlyRetentionPaths final {
  std::filesystem::path bundle_data;
  std::filesystem::path raw_data;
};

[[nodiscard]] ReadOnlyRetentionPaths require_read_only_retention_paths(
    const std::filesystem::path& trusted_root) {
  const std::filesystem::path bundle_namespace =
      trusted_root / "retained-bundles";
  const std::filesystem::path bundle_data =
      bundle_namespace / "sha256";
  const std::filesystem::path raw_namespace =
      trusted_root / "raw-artifacts";
  const std::filesystem::path raw_data = raw_namespace / "sha256";
  for (const auto& directory :
       {bundle_namespace, bundle_data, raw_namespace, raw_data}) {
    posix::require_existing_plain_directory(
        directory, kPosixErrors, kReadOnlyDirectoryMessages);
  }
  return {bundle_data, raw_data};
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
read_verified_bundle_entry(
    const std::filesystem::path& bundle_data,
    const core::RetainedBundleId& retained_bundle_id) {
  const auto hex =
      detail::retained_digest_hex(retained_bundle_id.value());
  if (!hex.has_value()) {
    return nullptr;
  }
  const auto bytes = posix::read_exact_regular_file(
      bundle_data / (std::string(*hex) + ".json"),
      kMaxDurableRetainedBundleBytes, true, kPosixErrors,
      kReadMessages);
  if (!bytes.has_value()) {
    return nullptr;
  }
  const auto bundle =
      detail::RetainedBundleReloadAuthority::reload(*bytes);
  if (bundle->retained_bundle_id() != retained_bundle_id) {
    detail::fail_retained_bundle(
        "read-only retained-bundle key/content mismatch");
  }
  return bundle;
}

[[nodiscard]] std::string read_verified_raw_reference(
    const std::filesystem::path& raw_data,
    const core::ContentAddressedStoreKey& key,
    const core::ContentDigest& digest,
    std::optional<std::uint64_t> expected_size,
    std::string_view reference_label) {
  const auto hex = detail::retained_raw_key_hex(key, digest);
  if (!hex.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " has an inconsistent digest-derived raw-CAS key");
  }
  auto bytes = posix::read_exact_regular_file(
      raw_data / (std::string(*hex) + ".blob"),
      kMaxDurableRetainedBlobBytes, true, kPosixErrors,
      kReadMessages);
  if (!bytes.has_value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " is absent from the durable raw-artifact CAS");
  }
  if ((expected_size.has_value() &&
       static_cast<std::uint64_t>(bytes->size()) != *expected_size) ||
      sha256_digest(*bytes) != digest.value()) {
    detail::fail_retained_bundle(
        std::string(reference_label) +
        " failed exact durable raw-artifact verification");
  }
  return std::move(*bytes);
}

void require_verified_product_artifacts(
    const std::filesystem::path& raw_data,
    const core::RetainedProductDTO& product) {
  for (const auto& artifact : product.artifact_refs) {
    static_cast<void>(read_verified_raw_reference(
        raw_data, artifact.content_addressed_store_key,
        artifact.content_digest, artifact.byte_size,
        "retained product artifact reference"));
  }
}

void require_verified_bundle_references(
    const std::filesystem::path& raw_data, const RetainedBundle& bundle) {
  const auto& value = bundle.value();
  for (const auto& record : value.catalogs.records) {
    std::visit(
        [&raw_data](const auto& typed_record) {
          static_cast<void>(read_verified_raw_reference(
              raw_data, typed_record.content_addressed_store_key,
              typed_record.content_digest, std::nullopt,
              "retained catalog-record reference"));
        },
        record);
  }
  for (const auto& schema : value.catalogs.schema_artifacts) {
    static_cast<void>(read_verified_raw_reference(
        raw_data, schema.content_addressed_store_key,
        schema.content_digest, std::nullopt,
        "retained schema-artifact reference"));
  }

  const auto& products = value.compiled_products;
  require_verified_product_artifacts(
      raw_data, products.planning_collision_model);
  if (products.viewer_model.has_value()) {
    require_verified_product_artifacts(
        raw_data, *products.viewer_model);
  }
  for (const auto& group : products.control_domain_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifacts(raw_data, product);
    }
  }
  for (const auto& product : products.symbol_role_maps) {
    require_verified_product_artifacts(raw_data, product);
  }
  for (const auto& group : products.runtime_unit_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifacts(raw_data, product);
    }
  }
}

[[nodiscard]] const core::RetainedProductDTO*
find_exact_product(const core::RetainedBundleDTO& bundle,
                   const RetainedProductArtifactQuery& query) {
  const core::RetainedProductDTO* match = nullptr;
  const auto consider =
      [&query, &match](const core::RetainedProductDTO& candidate) {
        if (candidate.product_id != query.product_id ||
            candidate.product_kind != query.product_kind) {
          return;
        }
        if (match != nullptr) {
          detail::fail_retained_bundle(
              "verified retained bundle contains duplicate exact product "
              "coordinates");
        }
        match = &candidate;
      };

  const auto& products = bundle.compiled_products;
  consider(products.planning_collision_model);
  if (products.viewer_model.has_value()) {
    consider(*products.viewer_model);
  }
  for (const auto& group : products.control_domain_products) {
    for (const auto& product : group.products) {
      consider(product);
    }
  }
  for (const auto& product : products.symbol_role_maps) {
    consider(product);
  }
  for (const auto& group : products.runtime_unit_products) {
    for (const auto& product : group.products) {
      consider(product);
    }
  }
  return match;
}

}  // namespace

FilesystemVerifiedRetainedBundleSource::
    FilesystemVerifiedRetainedBundleSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::shared_ptr<const RetainedBundle>
FilesystemVerifiedRetainedBundleSource::get_verified(
    const core::RetainedBundleId& retained_bundle_id) const {
  const std::filesystem::path current_root =
      posix::normalize_existing_plain_root(
          trusted_package_root_, kPosixErrors, kTrustedRootMessages);
  if (current_root != trusted_package_root_) {
    detail::fail_retained_bundle(
        "read-only retained-bundle trusted root changed after "
        "construction");
  }
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data, retained_bundle_id);
  if (bundle == nullptr) {
    return nullptr;
  }
  require_verified_bundle_references(paths.raw_data, *bundle);
  return bundle;
}

FilesystemVerifiedRetainedProductArtifactSource::
    FilesystemVerifiedRetainedProductArtifactSource(
        std::filesystem::path trusted_package_root)
    : trusted_package_root_(posix::normalize_existing_plain_root(
          trusted_package_root, kPosixErrors, kTrustedRootMessages)) {
  (void)require_read_only_retention_paths(trusted_package_root_);
}

std::optional<VerifiedRetainedProductArtifact>
FilesystemVerifiedRetainedProductArtifactSource::get_verified(
    const RetainedProductArtifactQuery& query) const {
  const std::filesystem::path current_root =
      posix::normalize_existing_plain_root(
          trusted_package_root_, kPosixErrors, kTrustedRootMessages);
  if (current_root != trusted_package_root_) {
    detail::fail_retained_bundle(
        "read-only retained-product trusted root changed after "
        "construction");
  }
  const ReadOnlyRetentionPaths paths =
      require_read_only_retention_paths(current_root);
  const auto bundle =
      read_verified_bundle_entry(paths.bundle_data,
                                 query.retained_bundle_id);
  if (bundle == nullptr) {
    return std::nullopt;
  }
  const core::RetainedProductDTO* product =
      find_exact_product(bundle->value(), query);
  if (product == nullptr) {
    return std::nullopt;
  }
  const auto artifact = std::find_if(
      product->artifact_refs.begin(), product->artifact_refs.end(),
      [&query](const core::RetainedArtifactRefDTO& candidate) {
        return candidate.artifact_role_id == query.artifact_role_id;
      });
  if (artifact == product->artifact_refs.end()) {
    return std::nullopt;
  }

  std::string bytes = read_verified_raw_reference(
      paths.raw_data, artifact->content_addressed_store_key,
      artifact->content_digest, artifact->byte_size,
      "selected retained product artifact");
  return VerifiedRetainedProductArtifact(
      query, product->product_contract_id,
      product->product_contract_revision, *artifact,
      std::move(bytes));
}

}  // namespace manyforge::deployment

#pragma once

// Test-only bounded in-memory fixtures for fault isolation and effect-count
// assertions. Production code has no in-memory semantic-manifest,
// retained-artifact, or retained-bundle persistence implementation.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_deployment/build_manifest.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

#include "build_manifest_internal.hpp"
#include "retained_bundle_internal.hpp"

namespace manyforge::deployment::test {

class InMemoryBuildManifestRepositoryFixture final
    : public IBuildManifestRepository {
 public:
  explicit InMemoryBuildManifestRepositoryFixture(
      std::size_t max_manifest_count = kMaxQualifiedManifestCount,
      std::uint64_t max_total_bytes = kMaxQualifiedManifestBytes)
      : max_manifest_count_(max_manifest_count),
        max_total_bytes_(max_total_bytes) {
    if (max_manifest_count_ == 0U || max_total_bytes_ == 0U) {
      throw BuildManifestError(
          "test build-manifest fixture limits must be positive");
    }
  }

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const BuildQualifiedManifest> manifest) override {
    if (manifest == nullptr) {
      throw BuildManifestError(
          "test build-manifest fixture rejects a null manifest");
    }
    const auto reloaded =
        detail::reload_self_verified_build_manifest(
            manifest->key().manifest_kind,
            manifest->canonical_bytes());
    if (reloaded->key() != manifest->key()) {
      throw BuildManifestError(
          "test build-manifest fixture rejects a key/content mismatch");
    }
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(manifest->canonical_bytes().size());
    std::lock_guard lock(mutex_);
    const auto existing = manifests_.find(manifest->key());
    if (existing != manifests_.end()) {
      if (existing->second != manifest->canonical_bytes()) {
        throw std::runtime_error(
            "test build-manifest fixture detected an identity collision");
      }
      return false;
    }
    if (manifests_.size() >= max_manifest_count_) {
      throw BuildManifestError(
          "test build-manifest fixture entry bound is exhausted");
    }
    if (byte_size > max_total_bytes_ - retained_bytes_) {
      throw BuildManifestError(
          "test build-manifest fixture byte bound is exhausted");
    }
    manifests_.emplace(
        manifest->key(), std::string(manifest->canonical_bytes()));
    retained_bytes_ += byte_size;
    return true;
  }

  [[nodiscard]] std::shared_ptr<const BuildQualifiedManifest>
  get_verified(const BuildManifestKey& key) const override {
    std::string bytes;
    {
      std::lock_guard lock(mutex_);
      const auto found = manifests_.find(key);
      if (found == manifests_.end()) {
        return nullptr;
      }
      bytes = found->second;
    }
    const auto manifest =
        detail::reload_self_verified_build_manifest(
            key.manifest_kind, bytes);
    if (manifest->key() != key) {
      throw std::runtime_error(
          "test build-manifest fixture reloaded a different semantic key");
    }
    return manifest;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return manifests_.size();
  }

  [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
    std::lock_guard lock(mutex_);
    return retained_bytes_;
  }

 private:
  std::size_t max_manifest_count_;
  std::uint64_t max_total_bytes_;
  std::map<BuildManifestKey, std::string> manifests_;
  std::uint64_t retained_bytes_{0U};
  mutable std::mutex mutex_;
};

class InMemoryRawArtifactStoreFixture final : public IRawArtifactStore {
 public:
  explicit InMemoryRawArtifactStoreFixture(
      std::size_t max_blob_count = kMaxDurableRetainedBlobCount,
      std::uint64_t max_total_bytes = kMaxDurableRetainedBlobBytes,
      std::uint64_t max_single_blob_bytes =
          kMaxDurableRetainedBlobBytes)
      : max_blob_count_(max_blob_count),
        max_total_bytes_(max_total_bytes),
        max_single_blob_bytes_(max_single_blob_bytes) {
    if (max_blob_count_ == 0U || max_total_bytes_ == 0U ||
        max_single_blob_bytes_ == 0U ||
        max_single_blob_bytes_ > max_total_bytes_) {
      throw RetainedBundleError(
          "test raw-artifact fixture limits are invalid");
    }
  }

  [[nodiscard]] StoredRawArtifact put_if_absent(
      std::string_view bytes) override {
    if (bytes.size() >
        static_cast<std::size_t>(std::min<std::uint64_t>(
            max_single_blob_bytes_,
            static_cast<std::uint64_t>(
                std::numeric_limits<std::size_t>::max())))) {
      throw RetainedBundleError(
          "test raw-artifact fixture single-blob bound is exhausted");
    }
    const std::string digest = sha256_digest(bytes);
    const std::string key = "sha256/" + digest.substr(7U);
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(bytes.size());
    std::lock_guard lock(mutex_);
    const auto existing = blobs_.find(key);
    if (existing != blobs_.end()) {
      if (existing->second != bytes) {
        throw std::runtime_error(
            "test raw-artifact fixture detected an identity collision");
      }
      return {
          core::ContentDigest(digest),
          core::ContentAddressedStoreKey(key),
          byte_size,
          false,
      };
    }
    if (blobs_.size() >= max_blob_count_ ||
        byte_size > max_total_bytes_ - retained_bytes_) {
      throw RetainedBundleError(
          "test raw-artifact fixture bound is exhausted");
    }
    blobs_.emplace(key, bytes);
    retained_bytes_ += byte_size;
    return {
        core::ContentDigest(digest),
        core::ContentAddressedStoreKey(key),
        byte_size,
        true,
    };
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& key,
      const core::ContentDigest& expected_digest,
      std::uint64_t expected_size) const override {
    if (!is_sha256_digest(expected_digest.value()) ||
        key.value() != "sha256/" + expected_digest.value().substr(7U)) {
      return std::nullopt;
    }
    std::string bytes;
    {
      std::lock_guard lock(mutex_);
      const auto found = blobs_.find(key.value());
      if (found == blobs_.end()) {
        return std::nullopt;
      }
      bytes = found->second;
    }
    if (static_cast<std::uint64_t>(bytes.size()) != expected_size ||
        sha256_digest(bytes) != expected_digest.value()) {
      return std::nullopt;
    }
    return bytes;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return blobs_.size();
  }

  [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
    std::lock_guard lock(mutex_);
    return retained_bytes_;
  }

 private:
  std::size_t max_blob_count_;
  std::uint64_t max_total_bytes_;
  std::uint64_t max_single_blob_bytes_;
  std::map<std::string, std::string> blobs_;
  std::uint64_t retained_bytes_{0U};
  mutable std::mutex mutex_;
};

class InMemoryCatalogArtifactStoreFixture final
    : public ICatalogArtifactStore {
 public:
  explicit InMemoryCatalogArtifactStoreFixture(
      std::size_t max_artifact_count =
          kMaxDurableCatalogArtifactCount,
      std::uint64_t max_total_payload_bytes =
          kMaxDurableRetainedBlobBytes,
      std::uint64_t max_single_payload_bytes =
          kMaxDurableRetainedBlobBytes)
      : max_artifact_count_(max_artifact_count),
        max_total_payload_bytes_(max_total_payload_bytes),
        max_single_payload_bytes_(max_single_payload_bytes) {
    if (max_artifact_count_ == 0U ||
        max_total_payload_bytes_ == 0U ||
        max_single_payload_bytes_ == 0U ||
        max_single_payload_bytes_ > max_total_payload_bytes_) {
      throw RetainedBundleError(
          "test catalog-artifact fixture limits are invalid");
    }
  }

  [[nodiscard]] StoredCatalogArtifact put_if_absent(
      const core::ContentAddressedStoreKey& exact_key,
      const core::ContentDigest& expected_digest,
      std::string_view bytes) override {
    if (!is_sha256_digest(expected_digest.value()) ||
        sha256_digest(bytes) != expected_digest.value()) {
      throw RetainedBundleError(
          "test catalog-artifact fixture rejects an invalid digest");
    }
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(bytes.size());
    if (byte_size > max_single_payload_bytes_) {
      throw RetainedBundleError(
          "test catalog-artifact fixture single-artifact bound is "
          "exhausted");
    }
    std::lock_guard lock(mutex_);
    const auto existing = artifacts_.find(exact_key.value());
    if (existing != artifacts_.end()) {
      if (existing->second.digest != expected_digest ||
          existing->second.bytes != bytes) {
        throw std::runtime_error(
            "test catalog-artifact fixture detected an exact-key "
            "collision");
      }
      return {
          exact_key, expected_digest, byte_size, false};
    }
    if (artifacts_.size() >= max_artifact_count_ ||
        byte_size >
            max_total_payload_bytes_ - retained_payload_bytes_) {
      throw RetainedBundleError(
          "test catalog-artifact fixture bound is exhausted");
    }
    artifacts_.emplace(
        exact_key.value(),
        Artifact{expected_digest, std::string(bytes)});
    retained_payload_bytes_ += byte_size;
    return {exact_key, expected_digest, byte_size, true};
  }

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& exact_key,
      const core::ContentDigest& expected_digest,
      std::optional<std::uint64_t> expected_size =
          std::nullopt) const override {
    std::lock_guard lock(mutex_);
    const auto found = artifacts_.find(exact_key.value());
    if (found == artifacts_.end() ||
        found->second.digest != expected_digest ||
        sha256_digest(found->second.bytes) !=
            expected_digest.value() ||
        (expected_size.has_value() &&
         found->second.bytes.size() != *expected_size)) {
      return std::nullopt;
    }
    return found->second.bytes;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return artifacts_.size();
  }

  [[nodiscard]] std::uint64_t retained_payload_bytes() const noexcept {
    std::lock_guard lock(mutex_);
    return retained_payload_bytes_;
  }

 private:
  struct Artifact final {
    core::ContentDigest digest;
    std::string bytes;
  };

  std::size_t max_artifact_count_;
  std::uint64_t max_total_payload_bytes_;
  std::uint64_t max_single_payload_bytes_;
  std::map<std::string, Artifact> artifacts_;
  std::uint64_t retained_payload_bytes_{0U};
  mutable std::mutex mutex_;
};

class InMemoryRetainedBundleRepositoryFixture final
    : public IRetainedBundleRepository {
 public:
  explicit InMemoryRetainedBundleRepositoryFixture(
      std::size_t max_bundle_count = kMaxDurableRetainedBundleCount,
      std::uint64_t max_total_bytes = kMaxDurableRetainedBundleBytes)
      : max_bundle_count_(max_bundle_count),
        max_total_bytes_(max_total_bytes) {
    if (max_bundle_count_ == 0U || max_total_bytes_ == 0U) {
      throw RetainedBundleError(
          "test retained-bundle fixture limits are invalid");
    }
  }

  [[nodiscard]] bool put_if_absent(
      std::shared_ptr<const RetainedBundle> bundle) override {
    if (bundle == nullptr || !verify_retained_bundle_identity(*bundle)) {
      throw RetainedBundleError(
          "test retained-bundle fixture rejects invalid input");
    }
    const auto reloaded =
        detail::RetainedBundleReloadAuthority::reload(
            bundle->canonical_bytes());
    if (reloaded->retained_bundle_id() !=
            bundle->retained_bundle_id() ||
        reloaded->value() != bundle->value()) {
      throw RetainedBundleError(
          "test retained-bundle fixture rejects divergent typed and "
          "canonical values");
    }
    const std::uint64_t byte_size =
        static_cast<std::uint64_t>(bundle->canonical_bytes().size());
    std::lock_guard lock(mutex_);
    const auto existing = bundles_.find(bundle->retained_bundle_id().value());
    if (existing != bundles_.end()) {
      if (existing->second != bundle->canonical_bytes()) {
        throw std::runtime_error(
            "test retained-bundle fixture detected an identity collision");
      }
      return false;
    }
    if (bundles_.size() >= max_bundle_count_ ||
        byte_size > max_total_bytes_ - retained_bytes_) {
      throw RetainedBundleError(
          "test retained-bundle fixture bound is exhausted");
    }
    bundles_.emplace(
        bundle->retained_bundle_id().value(),
        std::string(bundle->canonical_bytes()));
    retained_bytes_ += byte_size;
    return true;
  }

  [[nodiscard]] std::shared_ptr<const RetainedBundle>
  get_verified_envelope(
      const core::RetainedBundleId& retained_bundle_id) const override {
    std::string bytes;
    {
      std::lock_guard lock(mutex_);
      const auto found = bundles_.find(retained_bundle_id.value());
      if (found == bundles_.end()) {
        return nullptr;
      }
      bytes = found->second;
    }
    const auto reloaded =
        detail::RetainedBundleReloadAuthority::reload(bytes);
    if (reloaded->retained_bundle_id() != retained_bundle_id ||
        !verify_retained_bundle_identity(*reloaded)) {
      throw std::runtime_error(
          "test retained-bundle fixture re-verification failed");
    }
    return reloaded;
  }

  [[nodiscard]] std::size_t size() const noexcept {
    std::lock_guard lock(mutex_);
    return bundles_.size();
  }

  [[nodiscard]] std::uint64_t retained_bytes() const noexcept {
    std::lock_guard lock(mutex_);
    return retained_bytes_;
  }

 private:
  std::size_t max_bundle_count_;
  std::uint64_t max_total_bytes_;
  std::map<std::string, std::string> bundles_;
  std::uint64_t retained_bytes_{0U};
  mutable std::mutex mutex_;
};

}  // namespace manyforge::deployment::test

#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/prepared_activation_input.hpp>
#include <manyforge_deployment/retained_bundle.hpp>

namespace manyforge::deployment::detail {

// Borrows the exact prepared artifact vector. Its verified closure must remain
// alive for every synchronous lookup through this source.
class PreparedCatalogSource final : public ICatalogArtifactSource {
 public:
  explicit PreparedCatalogSource(const std::vector<PreparedActivationArtifact>& values)
      : values_(values) {}

  [[nodiscard]] std::optional<std::string> get_verified(
      const core::ContentAddressedStoreKey& key, const core::ContentDigest& digest,
      std::optional<std::uint64_t> size) const override {
    const auto found = std::find_if(values_.begin(), values_.end(), [&](const auto& value) {
      return value.content_addressed_store_key == key;
    });
    if (found == values_.end() || found->content_digest != digest ||
        (size.has_value() && found->bytes.size() != *size) ||
        sha256_digest(found->bytes) != digest.value()) return std::nullopt;
    return found->bytes;
  }

 private:
  const std::vector<PreparedActivationArtifact>& values_;
};

}  // namespace manyforge::deployment::detail

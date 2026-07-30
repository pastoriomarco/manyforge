#pragma once

#include <manyforge_deployment/content_identity.hpp>

#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {

/// Generates one compiler-owned logical name in the private mf1 namespace.
///
/// This is deliberately an implementation-only helper. Authored deployment
/// data cannot supply a generated name or select this encoding.
[[nodiscard]] inline std::string make_mf1_generated_name(
    std::string_view tag,
    const std::vector<std::pair<std::string_view, std::string_view>>& fields) {
  std::string preimage{"manyforge.occurrence-name.mf1"};
  for (const auto& [field, value] : fields) {
    preimage.push_back('\0');
    preimage.append(field);
    preimage.push_back('=');
    preimage.append(std::to_string(value.size()));
    preimage.push_back(':');
    preimage.append(value);
  }
  const auto digest = sha256_digest(preimage);
  if (!digest.starts_with("sha256:") || digest.size() != 71U) {
    throw std::logic_error("internal mf1 digest is malformed");
  }
  return "mf1_" + std::string(tag) + "_" + digest.substr(7U);
}

}  // namespace manyforge::deployment::detail

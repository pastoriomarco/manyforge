#pragma once

#include <filesystem>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <manyforge_core/types/error_code.hpp>
#include <manyforge_core/types/runtime_contract_manifest.hpp>

namespace manyforge::headless {

class ManagerPackageGenerationRestartRequired final
    : public std::runtime_error {
public:
  explicit ManagerPackageGenerationRestartRequired(
      manyforge::core::ErrorDTO error);

  [[nodiscard]] const manyforge::core::ErrorDTO &error() const noexcept;

private:
  manyforge::core::ErrorDTO error_;
};

/// Pins the package root and complete runtime-contract manifest set selected
/// before a Manager generation opens ingress. A detected replacement latches
/// closed until the complete stack is stopped and started again.
class ManagerPackageGeneration final {
public:
  ManagerPackageGeneration(
      std::filesystem::path package_root,
      manyforge::core::RuntimeContractManifestSetDTO manifest_set);
  ~ManagerPackageGeneration();

  ManagerPackageGeneration(const ManagerPackageGeneration &) = delete;
  ManagerPackageGeneration &operator=(const ManagerPackageGeneration &) =
      delete;

  [[nodiscard]] const std::filesystem::path &package_root() const noexcept;
  [[nodiscard]] const manyforge::core::RuntimeContractManifestSetDTO &
  manifest_set() const noexcept;

  void RequireCurrent(
      const manyforge::core::RuntimeContractManifestSetDTO &observed);

private:
  [[noreturn]] void LatchAndThrow(
      std::string reason,
      std::optional<manyforge::core::RuntimeContractManifestSetDigest>
          observed_digest = std::nullopt);

  std::filesystem::path package_root_;
  manyforge::core::RuntimeContractManifestSetDTO manifest_set_;
  int root_descriptor_{-1};
  std::uint64_t root_device_{};
  std::uint64_t root_inode_{};
  std::optional<manyforge::core::ErrorDTO> latched_error_;
};

} // namespace manyforge::headless

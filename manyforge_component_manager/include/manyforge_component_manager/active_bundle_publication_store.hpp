#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_core/ports/iactive_bundle_publication_store.hpp>

namespace manyforge::component_manager {

inline constexpr std::string_view kActiveBundlePublicationStoreSchemaId =
    "manyforge.internal.active-bundle-publication-store.i4.v1";
inline constexpr std::int64_t kActiveBundlePublicationStoreSchemaVersion = 1;
inline constexpr std::int64_t kActiveBundlePublicationStoreSqliteApplicationId =
    1296449872;
inline constexpr std::int64_t kMaximumActiveBundlePublicationReceipts = 4096;
inline constexpr std::int64_t
    kMaximumActiveBundlePublicationDatabasePageImageBytes =
        64LL * 1024LL * 1024LL;
inline constexpr std::size_t
    kMaximumActiveBundlePublicationIdentifierBytes = 256U;

class ActiveBundlePublicationStoreError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class ActiveBundlePublicationConflictError final
    : public ActiveBundlePublicationStoreError {
 public:
  using ActiveBundlePublicationStoreError::ActiveBundlePublicationStoreError;
};

class ActiveBundlePublicationLimitError final
    : public ActiveBundlePublicationStoreError {
 public:
  using ActiveBundlePublicationStoreError::ActiveBundlePublicationStoreError;
};

class ActiveBundlePublicationIntegrityError final
    : public ActiveBundlePublicationStoreError {
 public:
  using ActiveBundlePublicationStoreError::ActiveBundlePublicationStoreError;
};

class ActiveBundlePublicationStoreClosedError final
    : public ActiveBundlePublicationStoreError {
 public:
  using ActiveBundlePublicationStoreError::ActiveBundlePublicationStoreError;
};

/// Diagnostic integrity result. This immutable value is not an active pointer
/// or an alternate publication authority.
struct ActiveBundlePublicationStoreIntegrityReport final {
  manyforge::core::CellId cell_id;
  std::string schema_id;
  std::int64_t schema_version;
  std::string schema_artifact_digest;
  std::int64_t receipt_count;
  manyforge::core::ActiveBundlePublicationGeneration current_generation;
  std::string current_pointer_digest;
};

/// SQLite-backed sole owner of the durable active-bundle public pointer.
///
/// The store contains no Manager journal, scene/process/Kernel/robot effect,
/// compiler product construction, or caller authority. Exact immutable
/// receipts are retained for idempotent Publish replay and integrity. The
/// reader capability succeeds only when its requested generation still names
/// the singleton active pointer.
class SqliteActiveBundlePublicationStore final
    : public manyforge::core::IActiveBundlePublicationStore {
 public:
  SqliteActiveBundlePublicationStore(
      std::filesystem::path database_path,
      manyforge::core::CellId cell_id);
  ~SqliteActiveBundlePublicationStore() override;

  SqliteActiveBundlePublicationStore(
      SqliteActiveBundlePublicationStore&&) noexcept;
  SqliteActiveBundlePublicationStore& operator=(
      SqliteActiveBundlePublicationStore&&) noexcept;
  SqliteActiveBundlePublicationStore(
      const SqliteActiveBundlePublicationStore&) = delete;
  SqliteActiveBundlePublicationStore& operator=(
      const SqliteActiveBundlePublicationStore&) = delete;

  [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
  [[nodiscard]] const manyforge::core::CellId& cell_id() const noexcept;

  [[nodiscard]] manyforge::core::ActiveBundlePublicationReceiptDTO
  PublishActiveBundle(
      const manyforge::core::ActiveBundlePublicationRequestDTO& request)
      override;

  [[nodiscard]] manyforge::core::Result<
      manyforge::core::ActiveBundlePublicationReceiptDTO>
  ReadActiveBundlePublication(
      manyforge::core::ActiveBundlePublicationGeneration
          publication_generation) const override;

  [[nodiscard]] ActiveBundlePublicationStoreIntegrityReport
  VerifyIntegrity() const;

  void Close();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace manyforge::component_manager

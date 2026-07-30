#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::component_manager {

inline constexpr std::string_view kPersistenceFoundationSchemaId =
    "manyforge.internal.component_manager.persistence-fixture.i4.v1";
inline constexpr std::int64_t kPersistenceFoundationSchemaVersion = 1;
inline constexpr std::int64_t kPersistenceSqliteApplicationId = 1296454480;
inline constexpr std::int64_t kMaximumOperations = 4096;
inline constexpr std::int64_t kMaximumJournalEntriesPerOperation = 512;
inline constexpr std::int64_t kMaximumTotalJournalEntries = 16384;
inline constexpr std::int64_t kMaximumFoundationReceipts = 4096;
inline constexpr std::size_t kMaximumJournalPayloadBytes = 1024U * 1024U;
inline constexpr std::int64_t kMaximumMainDatabasePageImageBytes =
    512LL * 1024LL * 1024LL;
inline constexpr std::size_t kMaximumPersistenceIdentifierBytes = 256;

class PersistenceError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class PersistenceConflictError final : public PersistenceError {
 public:
  using PersistenceError::PersistenceError;
};

class PersistenceLimitError final : public PersistenceError {
 public:
  using PersistenceError::PersistenceError;
};

class PersistenceIntegrityError final : public PersistenceError {
 public:
  using PersistenceError::PersistenceError;
};

class PersistenceClosedError final : public PersistenceError {
 public:
  using PersistenceError::PersistenceError;
};

enum class OperationKind {
  kBootstrap,
  kApply,
};

class OperationRef final {
 public:
  OperationRef(
      OperationKind kind,
      std::string operation_id,
      std::int64_t generation);

  [[nodiscard]] OperationKind kind() const noexcept;
  [[nodiscard]] const std::string& operation_id() const noexcept;
  [[nodiscard]] std::int64_t generation() const noexcept;

  bool operator==(const OperationRef&) const = default;

 private:
  OperationKind kind_;
  std::string operation_id_;
  std::int64_t generation_;
};

class BootstrapIdentity final {
 public:
  BootstrapIdentity(
      std::string bootstrap_id,
      std::int64_t generation,
      std::string intent_digest,
      std::string target_bundle_identity,
      std::string target_graph_revision);

  [[nodiscard]] const std::string& bootstrap_id() const noexcept;
  [[nodiscard]] std::int64_t generation() const noexcept;
  [[nodiscard]] const std::string& intent_digest() const noexcept;
  [[nodiscard]] const std::string& target_bundle_identity() const noexcept;
  [[nodiscard]] const std::string& target_graph_revision() const noexcept;

 private:
  std::string bootstrap_id_;
  std::int64_t generation_;
  std::string intent_digest_;
  std::string target_bundle_identity_;
  std::string target_graph_revision_;
};

class ApplyIdentity final {
 public:
  ApplyIdentity(
      std::string apply_id,
      std::int64_t generation,
      std::string request_id,
      std::string manager_owner_epoch,
      std::string intent_digest,
      std::int64_t expected_foundation_generation,
      std::string predecessor_bundle_identity,
      std::string predecessor_graph_revision,
      std::string target_bundle_identity,
      std::string target_graph_revision);

  [[nodiscard]] const std::string& apply_id() const noexcept;
  [[nodiscard]] std::int64_t generation() const noexcept;
  [[nodiscard]] const std::string& request_id() const noexcept;
  [[nodiscard]] const std::string& manager_owner_epoch() const noexcept;
  [[nodiscard]] const std::string& intent_digest() const noexcept;
  [[nodiscard]] std::int64_t expected_foundation_generation() const noexcept;
  [[nodiscard]] const std::string& predecessor_bundle_identity()
      const noexcept;
  [[nodiscard]] const std::string& predecessor_graph_revision()
      const noexcept;
  [[nodiscard]] const std::string& target_bundle_identity() const noexcept;
  [[nodiscard]] const std::string& target_graph_revision() const noexcept;

 private:
  std::string apply_id_;
  std::int64_t generation_;
  std::string request_id_;
  std::string manager_owner_epoch_;
  std::string intent_digest_;
  std::int64_t expected_foundation_generation_;
  std::string predecessor_bundle_identity_;
  std::string predecessor_graph_revision_;
  std::string target_bundle_identity_;
  std::string target_graph_revision_;
};

class OperationRecord final {
 public:
  [[nodiscard]] const OperationRef& ref() const noexcept;
  [[nodiscard]] const std::string& request_id() const noexcept;
  [[nodiscard]] const std::string& manager_owner_epoch() const noexcept;
  [[nodiscard]] const std::string& intent_digest() const noexcept;
  [[nodiscard]] std::int64_t expected_foundation_generation() const noexcept;
  [[nodiscard]] const std::string& predecessor_bundle_identity()
      const noexcept;
  [[nodiscard]] const std::string& predecessor_graph_revision()
      const noexcept;
  [[nodiscard]] const std::string& target_bundle_identity() const noexcept;
  [[nodiscard]] const std::string& target_graph_revision() const noexcept;
  [[nodiscard]] const std::string& identity_digest() const noexcept;
  [[nodiscard]] std::int64_t created_sequence() const noexcept;

 private:
  friend class ManagerPersistence;

  OperationRecord(
      OperationRef ref,
      std::string request_id,
      std::string manager_owner_epoch,
      std::string intent_digest,
      std::int64_t expected_foundation_generation,
      std::string predecessor_bundle_identity,
      std::string predecessor_graph_revision,
      std::string target_bundle_identity,
      std::string target_graph_revision,
      std::string identity_digest,
      std::int64_t created_sequence);

  OperationRef ref_;
  std::string request_id_;
  std::string manager_owner_epoch_;
  std::string intent_digest_;
  std::int64_t expected_foundation_generation_;
  std::string predecessor_bundle_identity_;
  std::string predecessor_graph_revision_;
  std::string target_bundle_identity_;
  std::string target_graph_revision_;
  std::string identity_digest_;
  std::int64_t created_sequence_;
};

struct OperationWriteResult final {
  OperationRecord record;
  bool inserted;
};

class JournalEvent final {
 public:
  JournalEvent(
      std::string entry_id,
      std::string event_kind,
      std::vector<std::byte> payload);

  [[nodiscard]] const std::string& entry_id() const noexcept;
  [[nodiscard]] const std::string& event_kind() const noexcept;
  [[nodiscard]] const std::vector<std::byte>& payload() const noexcept;

 private:
  std::string entry_id_;
  std::string event_kind_;
  std::vector<std::byte> payload_;
};

class JournalEntry final {
 public:
  [[nodiscard]] const OperationRef& ref() const noexcept;
  [[nodiscard]] std::int64_t sequence() const noexcept;
  [[nodiscard]] const std::string& entry_id() const noexcept;
  [[nodiscard]] const std::string& event_kind() const noexcept;
  [[nodiscard]] const std::vector<std::byte>& payload() const noexcept;
  [[nodiscard]] const std::string& payload_digest() const noexcept;
  [[nodiscard]] const std::string& previous_entry_digest() const noexcept;
  [[nodiscard]] const std::string& entry_digest() const noexcept;

 private:
  friend class ManagerPersistence;

  JournalEntry(
      OperationRef ref,
      std::int64_t sequence,
      std::string entry_id,
      std::string event_kind,
      std::vector<std::byte> payload,
      std::string payload_digest,
      std::string previous_entry_digest,
      std::string entry_digest);

  OperationRef ref_;
  std::int64_t sequence_;
  std::string entry_id_;
  std::string event_kind_;
  std::vector<std::byte> payload_;
  std::string payload_digest_;
  std::string previous_entry_digest_;
  std::string entry_digest_;
};

struct JournalAppendResult final {
  JournalEntry entry;
  bool inserted;
};

class FoundationAdvanceIntent final {
 public:
  FoundationAdvanceIntent(
      std::string advance_id,
      OperationRef operation_ref,
      std::string operation_identity_digest);

  [[nodiscard]] const std::string& advance_id() const noexcept;
  [[nodiscard]] const OperationRef& operation_ref() const noexcept;
  [[nodiscard]] const std::string& operation_identity_digest() const noexcept;

 private:
  std::string advance_id_;
  OperationRef operation_ref_;
  std::string operation_identity_digest_;
};

class FoundationReceipt final {
 public:
  [[nodiscard]] const std::string& advance_id() const noexcept;
  [[nodiscard]] const std::string& intent_digest() const noexcept;
  [[nodiscard]] const OperationRef& operation_ref() const noexcept;
  [[nodiscard]] const std::string& operation_identity_digest() const noexcept;
  [[nodiscard]] std::int64_t expected_generation() const noexcept;
  [[nodiscard]] const std::string& predecessor_bundle_identity()
      const noexcept;
  [[nodiscard]] const std::string& predecessor_graph_revision()
      const noexcept;
  [[nodiscard]] std::int64_t committed_generation() const noexcept;
  [[nodiscard]] const std::string& target_bundle_identity() const noexcept;
  [[nodiscard]] const std::string& target_graph_revision() const noexcept;
  [[nodiscard]] const std::string& receipt_digest() const noexcept;
  [[nodiscard]] std::int64_t created_sequence() const noexcept;

 private:
  friend class ManagerPersistence;

  FoundationReceipt(
      std::string advance_id,
      std::string intent_digest,
      OperationRef operation_ref,
      std::string operation_identity_digest,
      std::int64_t expected_generation,
      std::string predecessor_bundle_identity,
      std::string predecessor_graph_revision,
      std::int64_t committed_generation,
      std::string target_bundle_identity,
      std::string target_graph_revision,
      std::string receipt_digest,
      std::int64_t created_sequence);

  std::string advance_id_;
  std::string intent_digest_;
  OperationRef operation_ref_;
  std::string operation_identity_digest_;
  std::int64_t expected_generation_;
  std::string predecessor_bundle_identity_;
  std::string predecessor_graph_revision_;
  std::int64_t committed_generation_;
  std::string target_bundle_identity_;
  std::string target_graph_revision_;
  std::string receipt_digest_;
  std::int64_t created_sequence_;
};

struct FoundationAdvanceResult final {
  FoundationReceipt receipt;
  bool inserted;
};

class FoundationHead final {
 public:
  [[nodiscard]] std::int64_t generation() const noexcept;
  [[nodiscard]] const std::string& bundle_identity() const noexcept;
  [[nodiscard]] const std::string& graph_revision() const noexcept;
  [[nodiscard]] const std::string& advance_id() const noexcept;
  [[nodiscard]] const std::string& receipt_digest() const noexcept;
  [[nodiscard]] const OperationRef& operation_ref() const noexcept;

 private:
  friend class ManagerPersistence;

  FoundationHead(
      std::int64_t generation,
      std::string bundle_identity,
      std::string graph_revision,
      std::string advance_id,
      std::string receipt_digest,
      OperationRef operation_ref);

  std::int64_t generation_;
  std::string bundle_identity_;
  std::string graph_revision_;
  std::string advance_id_;
  std::string receipt_digest_;
  OperationRef operation_ref_;
};

struct IntegrityReport final {
  std::string cell_id;
  std::string schema_id;
  std::int64_t schema_version;
  std::string schema_artifact_digest;
  std::int64_t operation_count;
  std::int64_t journal_entry_count;
  std::int64_t foundation_receipt_count;
  std::int64_t foundation_head_generation;
  std::string foundation_head_receipt_digest;
};

class ManagerPersistence final {
 public:
  ManagerPersistence(
      std::filesystem::path database_path,
      std::string cell_id);
  ~ManagerPersistence();
  ManagerPersistence(ManagerPersistence&&) noexcept;
  ManagerPersistence& operator=(ManagerPersistence&&) noexcept;
  ManagerPersistence(const ManagerPersistence&) = delete;
  ManagerPersistence& operator=(const ManagerPersistence&) = delete;

  [[nodiscard]] const std::filesystem::path& database_path() const noexcept;
  [[nodiscard]] const std::string& cell_id() const noexcept;
  void Close();

  [[nodiscard]] OperationWriteResult EnsureBootstrap(
      const BootstrapIdentity& identity);
  [[nodiscard]] OperationWriteResult EnsureApply(
      const ApplyIdentity& identity);
  [[nodiscard]] std::optional<OperationRecord> Operation(
      const OperationRef& ref) const;
  [[nodiscard]] std::vector<OperationRecord> Operations() const;

  [[nodiscard]] JournalAppendResult AppendJournalEvent(
      const OperationRef& ref,
      const JournalEvent& event);
  [[nodiscard]] std::vector<JournalEntry> Journal(
      const OperationRef& ref) const;

  [[nodiscard]] FoundationAdvanceResult AdvanceFoundationHead(
      const FoundationAdvanceIntent& intent);
  [[nodiscard]] std::optional<FoundationHead> FoundationHeadValue() const;
  [[nodiscard]] std::optional<FoundationReceipt> FoundationReceiptValue(
      const std::string& advance_id) const;
  [[nodiscard]] std::vector<FoundationReceipt> FoundationReceipts() const;

  [[nodiscard]] IntegrityReport VerifyIntegrity() const;

  // Internal fixture only: this is not an Apply state machine, retained
  // product verifier, active-bundle publication store, process adapter, or
  // command surface.

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

[[nodiscard]] std::string_view ToString(OperationKind value);

}  // namespace manyforge::component_manager

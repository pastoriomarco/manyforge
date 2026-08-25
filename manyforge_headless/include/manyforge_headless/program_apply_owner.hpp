#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_deployment/program_binding.hpp>
#include <manyforge_deployment/program_contract.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/scenario_apply_candidate_source.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>
#include <manyforge_headless/source_snapshot.hpp>

namespace manyforge::headless {

inline constexpr std::string_view kProgramApplyPreparationSchemaVersion =
    "manyforge.program_apply_preparation.v1";
inline constexpr std::string_view kAppliedProgramSelectionSchemaVersion =
    "manyforge.applied_program_selection.v1";
inline constexpr std::string_view kProgramSelectionOwnerStateFile =
    "program-selection-owner.json";
inline constexpr std::string_view kProgramSelectionOwnerStateSchemaVersion =
    "manyforge.program_selection_owner_state.v2";
inline constexpr std::size_t kMaximumProgramApplyRequestLedgerEntries = 4096U;

class ProgramApplyError final : public std::runtime_error {
public:
  ProgramApplyError(std::string code, std::string message);

  [[nodiscard]] const std::string &code() const noexcept { return code_; }

private:
  std::string code_;
};

/// Native owner of immutable Program/Binding preparation and the sole active
/// Applied-Program pointer.
///
/// Preparation is effect-free with respect to runtime selection: it verifies
/// one exact maintained occurrence, reuses that occurrence's authored role
/// assignment, compiles the candidate against its retained bundle, and stores
/// only immutable content-addressed values. Commit is a separate small CAS
/// that succeeds only after the caller proves the Manager's fresh active
/// bundle/graph identity. Failures before the atomic publication point leave
/// the prior Applied pointer intact; an error after atomic rename is resolved
/// by reading the selected record and is returned as success only when it is
/// the exact candidate.
class ProgramSelectionOwner final {
public:
  ProgramSelectionOwner(std::filesystem::path install_root,
                        std::filesystem::path artifact_root,
                        std::filesystem::path owner_state_root,
                        std::string owner_id, std::string owner_epoch);
  ~ProgramSelectionOwner() = default;

  ProgramSelectionOwner(const ProgramSelectionOwner &) = delete;
  ProgramSelectionOwner &operator=(const ProgramSelectionOwner &) = delete;

  [[nodiscard]] manyforge::content_identity::CanonicalValue
  Prepare(std::string_view scenario_id,
          std::string_view expected_scenario_catalog_revision,
          std::string_view canonical_program_bytes,
          std::string_view active_retained_bundle_id,
          std::string_view active_graph_revision);

  [[nodiscard]] manyforge::content_identity::CanonicalValue
  Commit(std::string_view canonical_preparation_bytes,
         const std::optional<std::string> &expected_applied_record_digest,
         std::string_view request_owner_epoch, std::string_view request_id,
         std::string_view active_retained_bundle_id,
         std::string_view active_graph_revision, std::int64_t observed_at_ns);

  [[nodiscard]] std::optional<manyforge::content_identity::CanonicalValue>
  Replay(std::string_view canonical_preparation_bytes,
         const std::optional<std::string> &expected_applied_record_digest,
         std::string_view request_owner_epoch, std::string_view request_id);

  [[nodiscard]] std::optional<manyforge::content_identity::CanonicalValue>
  ReplayRequest(
      std::string_view scenario_id,
      std::string_view scenario_catalog_revision, std::string_view program_id,
      std::string_view program_revision,
      const std::optional<std::string> &expected_applied_record_digest,
      std::string_view request_owner_epoch, std::string_view request_id);

  /// Return the independently reverified durable Applied selection. Absence is
  /// reported with `program_applied_selection_not_found` rather than by
  /// fabricating a default/current Program.
  [[nodiscard]] manyforge::content_identity::CanonicalValue ReadApplied();

private:
  [[nodiscard]] manyforge::content_identity::CanonicalValue ReadAppliedLocked();
  [[nodiscard]] manyforge::content_identity::CanonicalValue BuildAppliedLocked(
      const manyforge::content_identity::CanonicalValue &record,
      std::string_view record_digest, std::uint64_t generation,
      std::string_view selection_source_owner_id,
      std::string_view selection_source_owner_epoch);
  [[nodiscard]] std::optional<manyforge::content_identity::CanonicalValue>
  ReplayResultLocked(std::string_view intent_digest,
                     std::string_view request_owner_epoch,
                     std::string_view request_id);

  manyforge::deployment::FilesystemScenarioApplyCandidateSource
      scenario_source_;
  manyforge::deployment::FilesystemVerifiedRetainedBundleSource bundle_source_;
  manyforge::deployment::FilesystemVerifiedRetainedModelMapSource
      model_map_source_;
  manyforge::deployment::FilesystemVerifiedCatalogArtifactSource
      catalog_source_;
  manyforge::deployment::FilesystemVerifiedRawArtifactSource
      raw_artifact_source_;
  manyforge::deployment::FilesystemVerifiedProgramBindingSource
      baseline_binding_source_;
  manyforge::deployment::FilesystemRawArtifactStore raw_artifact_store_;
  manyforge::deployment::FilesystemCatalogArtifactStore catalog_store_;
  manyforge::deployment::FilesystemRetainedBundleRepository bundle_repository_;
  manyforge::deployment::FilesystemProgramRepository program_repository_;
  manyforge::deployment::FilesystemProgramBindingRepository binding_repository_;
  manyforge::deployment::FilesystemVerifiedRetainedBundleSource
      artifact_bundle_source_;
  manyforge::deployment::FilesystemVerifiedRetainedModelMapSource
      artifact_model_map_source_;
  AtomicSourceSnapshotChannel owner_state_;
  std::string source_owner_id_;
  std::string source_owner_epoch_;
  std::optional<manyforge::content_identity::CanonicalValue> applied_cache_;
  std::string applied_cache_record_digest_;
  std::mutex mutex_;
};

} // namespace manyforge::headless

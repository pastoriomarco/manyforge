#pragma once

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/strong_id.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

namespace manyforge::deployment {

inline constexpr std::string_view kClosedSchemaApiVersion = "manyforge.closed_schema.v1";
inline constexpr std::string_view kClosedSchemaBootstrapContractDigest =
    "sha256:29a2fef8e781ca406eaaad448e46fb6061fe18e99e2c1cd39f303e2e0d815834";
inline constexpr std::size_t kMaxClosedSchemaDefinitions = 1024U;
inline constexpr std::size_t kMaxClosedSchemaFields = 1024U;
inline constexpr std::size_t kMaxClosedSchemaConstraints = 8U;
inline constexpr std::size_t kMaxClosedSchemaAlternatives = 256U;
inline constexpr std::size_t kMaxClosedSchemaEnumValues = 1024U;
inline constexpr std::size_t kMaxClosedSchemaKeyPaths = 16U;
inline constexpr std::size_t kMaxClosedSchemaKeyPathSegments = 16U;
inline constexpr std::size_t kMaxClosedSchemaArrayItems = kMaxArtifactNodeCount;
inline constexpr std::size_t kMaxClosedSchemaStringCodePoints = kMaxArtifactScalarBytes;
inline constexpr std::size_t kMaxNormalizedAggregateScalarBytes = kMaxArtifactEncodedBytes;

class ClosedSchemaError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class ClosedSchemaPurpose { kArtifactDocument, kValue };
enum class ClosedSchemaRootShape { kScalar, kObject, kArray, kTaggedUnion };

namespace detail {
struct CompiledClosedSchemaData;
}

class UnqualifiedSchemaNormalizedArtifactCandidate;

/// A descriptor whose closed shape, local-reference graph, defaults, and
/// declared SchemaRevision have all been verified against the immutable v1
/// bootstrap.
///
/// The private constructor prevents callers from assigning schema trust to an
/// arbitrary value or revision. Compilation verifies an already-declared
/// content identity; this API intentionally does not expose identity minting.
/// Compilation alone does not qualify the descriptor for production use.
class CompiledClosedSchema final {
 public:
  [[nodiscard]] const core::SchemaId& schema_id() const noexcept;
  [[nodiscard]] const core::SchemaRevision& schema_revision() const noexcept;
  [[nodiscard]] ClosedSchemaPurpose schema_purpose() const noexcept;
  [[nodiscard]] ClosedSchemaRootShape root_shape() const noexcept;
  [[nodiscard]] const CanonicalValue& normalized_descriptor() const noexcept;

 private:
  explicit CompiledClosedSchema(std::shared_ptr<const detail::CompiledClosedSchemaData> data);

  std::shared_ptr<const detail::CompiledClosedSchemaData> data_;

  friend CompiledClosedSchema compile_closed_schema_descriptor(
      const ParsedArtifactCandidate& candidate);
  friend class UnqualifiedSchemaNormalizedArtifactCandidate;
  friend UnqualifiedSchemaNormalizedArtifactCandidate
  validate_and_normalize_unqualified_artifact_candidate(
      const CompiledClosedSchema& schema, const ParsedArtifactCandidate& candidate);
};

/// Unqualified artifact candidate normalized by one exact compiled descriptor.
///
/// Defaults have been materialized and every schema-classified set has been
/// canonically ordered. This phase does not prove that the descriptor is
/// build-qualified for any production owner or catalog family.
class UnqualifiedSchemaNormalizedArtifactCandidate final {
 public:
  [[nodiscard]] const core::SchemaId& schema_id() const noexcept { return schema_id_; }
  [[nodiscard]] const core::SchemaRevision& schema_revision() const noexcept {
    return schema_revision_;
  }
  [[nodiscard]] ClosedSchemaPurpose schema_purpose() const noexcept {
    return schema_purpose_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  UnqualifiedSchemaNormalizedArtifactCandidate(
      core::SchemaId schema_id, core::SchemaRevision schema_revision,
      ClosedSchemaPurpose schema_purpose, CanonicalValue normalized_root)
      : schema_id_(std::move(schema_id)),
        schema_revision_(std::move(schema_revision)),
        schema_purpose_(schema_purpose),
        normalized_root_(std::move(normalized_root)) {}

  core::SchemaId schema_id_;
  core::SchemaRevision schema_revision_;
  ClosedSchemaPurpose schema_purpose_;
  CanonicalValue normalized_root_;

  friend UnqualifiedSchemaNormalizedArtifactCandidate
  validate_and_normalize_unqualified_artifact_candidate(
      const CompiledClosedSchema& schema, const ParsedArtifactCandidate& candidate);
};

/// Validates, normalizes, and verifies one descriptor under the immutable
/// `manyforge.closed_schema.v1` bootstrap.
[[nodiscard]] CompiledClosedSchema compile_closed_schema_descriptor(
    const ParsedArtifactCandidate& candidate);

/// Validates and normalizes candidate data through one unqualified descriptor.
///
/// The fixed v1 output budgets include materialized defaults, mapping keys,
/// aggregate scalar bytes, total nodes, and collection depth. The returned type
/// deliberately cannot satisfy a future production-qualified acceptance API.
[[nodiscard]] UnqualifiedSchemaNormalizedArtifactCandidate
validate_and_normalize_unqualified_artifact_candidate(
    const CompiledClosedSchema& schema, const ParsedArtifactCandidate& candidate);

enum class CatalogSchemaQualification { kUnqualified, kQualified };

class BuildCatalogSchemaEntry final {
 public:
  [[nodiscard]] core::CatalogRecordKind record_kind() const noexcept { return record_kind_; }
  [[nodiscard]] const core::SchemaId& schema_id() const noexcept { return schema_id_; }
  [[nodiscard]] CatalogSchemaQualification qualification() const noexcept {
    return qualification_;
  }
  [[nodiscard]] const CompiledClosedSchema* compiled_schema() const noexcept {
    return compiled_schema_.get();
  }

 private:
  BuildCatalogSchemaEntry(core::CatalogRecordKind record_kind, core::SchemaId schema_id,
                          CatalogSchemaQualification qualification,
                          std::shared_ptr<const CompiledClosedSchema> compiled_schema)
      : record_kind_(record_kind),
        schema_id_(std::move(schema_id)),
        qualification_(qualification),
        compiled_schema_(std::move(compiled_schema)) {
    const auto* family = core::catalog_record_type_descriptor(record_kind_);
    if (family == nullptr || family->schema_family != schema_id_.value()) {
      throw std::logic_error(
          "catalog schema entry does not match the closed record-kind family");
    }
    if (qualification_ == CatalogSchemaQualification::kUnqualified) {
      if (compiled_schema_ != nullptr) {
        throw std::logic_error(
            "unqualified catalog schema entry cannot own a compiled schema");
      }
      return;
    }
    if (compiled_schema_ == nullptr ||
        compiled_schema_->schema_purpose() !=
            ClosedSchemaPurpose::kArtifactDocument ||
        compiled_schema_->schema_id() != schema_id_) {
      throw std::logic_error(
          "qualified catalog schema entry requires an exact artifact-document schema");
    }
  }

  core::CatalogRecordKind record_kind_;
  core::SchemaId schema_id_;
  CatalogSchemaQualification qualification_;
  std::shared_ptr<const CompiledClosedSchema> compiled_schema_;

  friend class BuildQualifiedClosedSchemaRegistry;
};

class BuildQualifiedValueSchemaEntry final {
 public:
  [[nodiscard]] const core::SchemaId& schema_id() const noexcept { return schema_id_; }
  [[nodiscard]] const core::SchemaRevision& schema_revision() const noexcept {
    return schema_revision_;
  }
  [[nodiscard]] const CompiledClosedSchema& compiled_schema() const noexcept {
    return *compiled_schema_;
  }

 private:
  BuildQualifiedValueSchemaEntry(core::SchemaId schema_id,
                                 core::SchemaRevision schema_revision,
                                 std::shared_ptr<const CompiledClosedSchema> compiled_schema)
      : schema_id_(std::move(schema_id)),
        schema_revision_(std::move(schema_revision)),
        compiled_schema_(std::move(compiled_schema)) {
    if (compiled_schema_ == nullptr ||
        compiled_schema_->schema_purpose() != ClosedSchemaPurpose::kValue ||
        compiled_schema_->schema_id() != schema_id_ ||
        compiled_schema_->schema_revision() != schema_revision_) {
      throw std::logic_error(
          "qualified value-schema entry requires an exact value descriptor");
    }
  }

  core::SchemaId schema_id_;
  core::SchemaRevision schema_revision_;
  std::shared_ptr<const CompiledClosedSchema> compiled_schema_;

  friend class BuildQualifiedClosedSchemaRegistry;
};

/// Immutable schema registry compiled into this build.
///
/// There is deliberately no public constructor or registry-document loader:
/// catalog qualification is a build decision, never a runtime assertion from
/// untrusted input. At this checkpoint all catalog families are unqualified.
class BuildQualifiedClosedSchemaRegistry final {
 public:
  [[nodiscard]] std::span<const BuildCatalogSchemaEntry> catalog_schemas() const noexcept;
  [[nodiscard]] std::span<const BuildQualifiedValueSchemaEntry> value_schemas() const noexcept;
  [[nodiscard]] const BuildCatalogSchemaEntry* find_catalog_schema_family(
      core::CatalogRecordKind kind) const noexcept;
  [[nodiscard]] const BuildCatalogSchemaEntry* find_qualified_catalog_schema(
      core::CatalogRecordKind kind, const core::SchemaId& schema_id,
      const core::SchemaRevision& schema_revision) const noexcept;
  [[nodiscard]] const BuildQualifiedValueSchemaEntry* find_qualified_value_schema(
      const core::SchemaId& schema_id,
      const core::SchemaRevision& schema_revision) const noexcept;

 private:
  BuildQualifiedClosedSchemaRegistry();

  struct Storage;
  std::shared_ptr<const Storage> storage_;

  friend const BuildQualifiedClosedSchemaRegistry& build_qualified_closed_schema_registry();
};

/// Returns the process-lifetime immutable registry pinned into this build.
[[nodiscard]] const BuildQualifiedClosedSchemaRegistry&
build_qualified_closed_schema_registry();

}  // namespace manyforge::deployment

#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"

namespace manyforge::deployment {

class CellGraphCodecError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

/// Exact graph-envelope schema, structurally normalized candidate data whose
/// declared revision is not yet trusted.
///
/// This phase proves only the closed CellGraphDTO wire shape and normalization
/// of collections classified by that graph schema. It deliberately does not
/// imply local-reference validity, revision validity, catalog resolution, or
/// qualification of catalog-constrained nested values.
class TypedCellGraphCandidate final {
 public:
  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept { return graph_; }
  [[nodiscard]] const CanonicalValue& structurally_normalized_root() const noexcept {
    return structurally_normalized_root_;
  }

 private:
  TypedCellGraphCandidate(core::CellGraphDTO graph, CanonicalValue structurally_normalized_root)
      : graph_(std::move(graph)),
        structurally_normalized_root_(std::move(structurally_normalized_root)) {}

  core::CellGraphDTO graph_;
  CanonicalValue structurally_normalized_root_;

  friend TypedCellGraphCandidate decode_cell_graph_candidate(const ParsedArtifactCandidate&);
};

/// Intrinsically valid graph candidate whose declared revision is not trusted.
///
/// Catalog referents have not been resolved at this phase. Consequently this
/// type makes no claim about GraphRevision validity, catalog-constrained
/// configuration/defaults, component roles/capabilities, attachment
/// compatibility/capacity, generated products, compiler output, or runtime
/// admission.
class IntrinsicallyValidatedCellGraphCandidate final {
 public:
  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept { return graph_; }
  [[nodiscard]] const CanonicalValue& structurally_normalized_root() const noexcept {
    return structurally_normalized_root_;
  }

 private:
  IntrinsicallyValidatedCellGraphCandidate(core::CellGraphDTO graph,
                                           CanonicalValue structurally_normalized_root)
      : graph_(std::move(graph)),
        structurally_normalized_root_(std::move(structurally_normalized_root)) {}

  core::CellGraphDTO graph_;
  CanonicalValue structurally_normalized_root_;

  friend IntrinsicallyValidatedCellGraphCandidate validate_intrinsic_cell_graph_candidate(
      const TypedCellGraphCandidate&);
};

class GraphRevisionVerifiedCellGraph;

/// Intrinsically valid graph whose catalog-governed runtime configurations
/// have been normalized through their exact qualified runtime-profile schemas.
///
/// The exact caller-prefiltered runtime-profile dependency closure has been
/// verified, required configuration defaults have been materialized, and the
/// complete normalized graph satisfies the v1 output budgets. Graph-owned
/// nested revisions and the outer GraphRevision remain untrusted at this
/// stage. This is not a compiler-qualified graph.
class GraphIdentityReadyCellGraphCandidate final {
 public:
  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return graph_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  GraphIdentityReadyCellGraphCandidate(core::CellGraphDTO graph,
                                       CanonicalValue normalized_root)
      : graph_(std::move(graph)),
        normalized_root_(std::move(normalized_root)) {}

  core::CellGraphDTO graph_;
  CanonicalValue normalized_root_;

  friend GraphIdentityReadyCellGraphCandidate prepare_cell_graph_identity(
      const IntrinsicallyValidatedCellGraphCandidate&,
      std::span<const QualifiedRuntimeProfile>);
  friend GraphRevisionVerifiedCellGraph compute_graph_revision(
      const GraphIdentityReadyCellGraphCandidate&);
  friend GraphRevisionVerifiedCellGraph verify_graph_revision(
      const GraphIdentityReadyCellGraphCandidate&);
};

/// Fully normalized graph whose three nested graph-record identity classes and
/// outer GraphRevision have been computed or verified in dependency order.
///
/// This trust stage proves canonical content identity only. It does not prove
/// catalog/model/control compatibility, source composition, compiler products,
/// retained-bundle completeness, runtime readiness, admission, or any lane.
class GraphRevisionVerifiedCellGraph final {
 public:
  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return graph_;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return normalized_root_;
  }

 private:
  GraphRevisionVerifiedCellGraph(core::CellGraphDTO graph,
                                 CanonicalValue normalized_root)
      : graph_(std::move(graph)),
        normalized_root_(std::move(normalized_root)) {}

  core::CellGraphDTO graph_;
  CanonicalValue normalized_root_;

  friend GraphRevisionVerifiedCellGraph compute_graph_revision(
      const GraphIdentityReadyCellGraphCandidate&);
  friend GraphRevisionVerifiedCellGraph verify_graph_revision(
      const GraphIdentityReadyCellGraphCandidate&);
};

/// Constructs the exact typed graph candidate from a strictly decoded
/// JSON-model artifact.
///
/// Unknown/missing fields, wrong JSON types, malformed typed revisions or
/// digests, duplicate set elements, and non-normalized quaternions reject.
/// Graph IDs use 1..128 ASCII characters, begin with an alphanumeric, and
/// otherwise contain only alphanumerics, `.`, `_`, or `-`; this makes byte
/// ordering identical to their RFC 8785/UTF-16 ordering.
/// Identifier-keyed and set-valued arrays are returned in canonical order;
/// resource_bindings preserve authored semantic order. Unresolved
/// catalog-constrained runtime configuration remains candidate data and
/// preserves every nested array exactly; graph-identity preparation must
/// schema-classify, validate, normalize, and materialize defaults before
/// GraphRevision is computed. Quaternion norm-squared must be within 1e-6 of
/// one; accepted values are never silently renormalized. Optional
/// root_pose_in_cell fields are represented only by key absence; explicit null
/// is rejected.
[[nodiscard]] TypedCellGraphCandidate decode_cell_graph_candidate(
    const ParsedArtifactCandidate& parsed);

/// Verifies every graph invariant that can be decided without resolving
/// catalog payloads. This includes per-component model-binding injectivity,
/// global model ownership, owned/passive model placement, root/attached
/// component placement, and an acyclic single-parent component-attachment
/// forest. Declared component model-role bijection and attachment-interface
/// compatibility/capacity remain catalog-resolved compiler checks.
///
/// This does not compute or verify GraphRevision: true graph identity requires
/// the exact catalog-constrained schemas, validation, and default
/// materialization introduced at C0.2.
[[nodiscard]] IntrinsicallyValidatedCellGraphCandidate validate_intrinsic_cell_graph_candidate(
    const TypedCellGraphCandidate& candidate);

/// Resolves exactly the runtime-profile revisions selected by the graph,
/// normalizes every runtime configuration through the selected profile's
/// build-qualified value schema, and enforces the complete normalized-output
/// budget.
///
/// `qualified_runtime_profiles` is one exact caller-prefiltered dependency
/// closure. Duplicate exact pairs, missing/stale pairs, and unused extras
/// reject. Several revisions of one profile ID are legal when different
/// runtime units select those exact pairs.
[[nodiscard]] GraphIdentityReadyCellGraphCandidate prepare_cell_graph_identity(
    const IntrinsicallyValidatedCellGraphCandidate& candidate,
    std::span<const QualifiedRuntimeProfile> qualified_runtime_profiles);

/// Mints every graph-owned nested revision and then the outer GraphRevision.
///
/// Existing declared revision values are placeholders on this explicit
/// producer path. The returned graph carries only the recomputed identities.
[[nodiscard]] GraphRevisionVerifiedCellGraph compute_graph_revision(
    const GraphIdentityReadyCellGraphCandidate& candidate);

/// Verifies every graph-owned nested revision before verifying GraphRevision.
///
/// This verifier never repairs a stale declaration. A mismatch throws
/// CellGraphCodecError and no verified wrapper is produced.
[[nodiscard]] GraphRevisionVerifiedCellGraph verify_graph_revision(
    const GraphIdentityReadyCellGraphCandidate& candidate);

}  // namespace manyforge::deployment

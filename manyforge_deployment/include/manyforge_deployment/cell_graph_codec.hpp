#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <memory>
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
  TypedCellGraphCandidate(const TypedCellGraphCandidate&) noexcept = default;
  TypedCellGraphCandidate& operator=(
      const TypedCellGraphCandidate&) noexcept = default;

  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return data_->graph;
  }
  [[nodiscard]] const CanonicalValue& structurally_normalized_root() const noexcept {
    return data_->structurally_normalized_root;
  }

 private:
  struct Data final {
    Data(core::CellGraphDTO graph_in,
         CanonicalValue structurally_normalized_root_in)
        : graph(std::move(graph_in)),
          structurally_normalized_root(
              std::move(structurally_normalized_root_in)) {}

    core::CellGraphDTO graph;
    CanonicalValue structurally_normalized_root;
  };

  TypedCellGraphCandidate(core::CellGraphDTO graph, CanonicalValue structurally_normalized_root)
      : data_(std::make_shared<const Data>(
            std::move(graph), std::move(structurally_normalized_root))) {}

  std::shared_ptr<const Data> data_;

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
  IntrinsicallyValidatedCellGraphCandidate(
      const IntrinsicallyValidatedCellGraphCandidate&) noexcept = default;
  IntrinsicallyValidatedCellGraphCandidate& operator=(
      const IntrinsicallyValidatedCellGraphCandidate&) noexcept = default;

  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return data_->candidate.graph();
  }
  [[nodiscard]] const CanonicalValue& structurally_normalized_root() const noexcept {
    return data_->candidate.structurally_normalized_root();
  }

 private:
  struct Data final {
    explicit Data(const TypedCellGraphCandidate& candidate_in)
        : candidate(candidate_in) {}

    TypedCellGraphCandidate candidate;
  };

  explicit IntrinsicallyValidatedCellGraphCandidate(
      const TypedCellGraphCandidate& candidate)
      : data_(std::make_shared<const Data>(candidate)) {}

  std::shared_ptr<const Data> data_;

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
  GraphIdentityReadyCellGraphCandidate(
      const GraphIdentityReadyCellGraphCandidate&) noexcept = default;
  GraphIdentityReadyCellGraphCandidate& operator=(
      const GraphIdentityReadyCellGraphCandidate&) noexcept = default;

  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return data_->graph;
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->normalized_root;
  }

 private:
  struct Data final {
    Data(core::CellGraphDTO graph_in, CanonicalValue normalized_root_in)
        : graph(std::move(graph_in)),
          normalized_root(std::move(normalized_root_in)) {}

    core::CellGraphDTO graph;
    CanonicalValue normalized_root;
  };

  GraphIdentityReadyCellGraphCandidate(core::CellGraphDTO graph,
                                       CanonicalValue normalized_root)
      : data_(std::make_shared<const Data>(
            std::move(graph), std::move(normalized_root))) {}

  std::shared_ptr<const Data> data_;

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
  GraphRevisionVerifiedCellGraph(
      const GraphRevisionVerifiedCellGraph&) noexcept = default;
  GraphRevisionVerifiedCellGraph& operator=(
      const GraphRevisionVerifiedCellGraph&) noexcept = default;

  [[nodiscard]] const core::CellGraphDTO& graph() const noexcept {
    return data_->candidate.graph();
  }
  [[nodiscard]] const CanonicalValue& normalized_root() const noexcept {
    return data_->candidate.normalized_root();
  }

 private:
  struct Data final {
    explicit Data(const GraphIdentityReadyCellGraphCandidate& candidate_in)
        : candidate(candidate_in) {}

    GraphIdentityReadyCellGraphCandidate candidate;
  };

  explicit GraphRevisionVerifiedCellGraph(
      const GraphIdentityReadyCellGraphCandidate& candidate)
      : data_(std::make_shared<const Data>(candidate)) {}

  std::shared_ptr<const Data> data_;

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

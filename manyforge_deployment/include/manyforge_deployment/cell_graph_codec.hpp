#pragma once

#include <manyforge_core/types/cell_graph.hpp>
#include <stdexcept>
#include <string>
#include <utility>

#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/canonical_value.hpp"

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
/// catalog-constrained configuration/instance-parameter values remain
/// candidate data and preserve every nested array exactly; C0.2 must
/// schema-classify, validate, normalize, and materialize defaults before
/// GraphRevision is computed. Quaternion norm-squared must be within 1e-6 of
/// one; accepted values are never silently renormalized.
[[nodiscard]] TypedCellGraphCandidate decode_cell_graph_candidate(
    const ParsedArtifactCandidate& parsed);

/// Verifies every graph invariant that can be decided without resolving
/// catalog payloads.
///
/// This does not compute or verify GraphRevision: true graph identity requires
/// the exact catalog-constrained schemas, validation, and default
/// materialization introduced at C0.2.
[[nodiscard]] IntrinsicallyValidatedCellGraphCandidate validate_intrinsic_cell_graph_candidate(
    const TypedCellGraphCandidate& candidate);

}  // namespace manyforge::deployment

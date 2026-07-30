#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::deployment {

// Build-qualified v1 source-verification limits. I3 store retrieval and every
// language binding must preflight the same contract rather than duplicating
// policy constants.
inline constexpr std::uint64_t kMaxModelSourceXmlEntrypointBytes =
    16ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxModelSourceOpaqueArtifactBytes =
    256ULL * 1024ULL * 1024ULL;
inline constexpr std::uint64_t kMaxModelSourceClosureBytes =
    512ULL * 1024ULL * 1024ULL;
inline constexpr std::size_t kMaxModelSourceXmlDepth = 128U;
inline constexpr std::size_t kMaxModelSourceXmlElementCount = 250000U;
inline constexpr std::size_t kMaxModelSourceXmlAttributeCount = 1000000U;

class ModelSourceError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class ModelSourceArtifactKind : std::uint8_t {
  kUrdfXmlEntrypoint = 0,
  kSrdfXmlEntrypoint,
  kStlMesh,
  kPngTexture,
  kJpegTexture,
};

/// One exact immutable-store read required by a verified source declaration.
struct ModelSourceRetrievalPlanRow final {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  core::ContentDigest content_digest;
  core::ContentAddressedStoreKey expected_store_key;
  ModelSourceArtifactKind artifact_kind;

  friend bool operator==(const ModelSourceRetrievalPlanRow&,
                         const ModelSourceRetrievalPlanRow&) = default;
};

/// I2-owned declaration preflight consumed by I3 retrieval and verification.
///
/// The plan contains no bytes and performs no I/O. Its private construction
/// prevents callers from reinterpreting MIME, entrypoint, key, or limit
/// policy independently.
class ModelSourceRetrievalPlan final {
 public:
  [[nodiscard]] const QualifiedModelAsset& qualified_asset() const noexcept {
    return qualified_asset_;
  }
  [[nodiscard]] const std::vector<ModelSourceRetrievalPlanRow>& artifacts()
      const noexcept {
    return artifacts_;
  }
  [[nodiscard]] std::uint64_t total_bytes() const noexcept {
    return total_bytes_;
  }

 private:
  ModelSourceRetrievalPlan(
      QualifiedModelAsset qualified_asset,
      std::vector<ModelSourceRetrievalPlanRow> artifacts,
      std::uint64_t total_bytes)
      : qualified_asset_(std::move(qualified_asset)),
        artifacts_(std::move(artifacts)),
        total_bytes_(total_bytes) {}

  QualifiedModelAsset qualified_asset_;
  std::vector<ModelSourceRetrievalPlanRow> artifacts_;
  std::uint64_t total_bytes_;

  friend ModelSourceRetrievalPlan preflight_model_source_declaration(
      const QualifiedModelAsset&);
};

/// One caller-resolved source candidate.
///
/// This boundary performs no store I/O. The caller supplies the bytes returned
/// for `resolved_store_key`; qualification proves that the key and exact bytes
/// match the immutable artifact declaration.
struct ResolvedModelSourceArtifactCandidate final {
  std::string logical_uri;
  core::ContentAddressedStoreKey resolved_store_key;
  std::string stored_bytes;
};

/// Retained proof metadata for one verified source artifact.
///
/// Source bytes are intentionally not retained in the qualified product.
struct VerifiedModelSourceArtifact final {
  std::string logical_uri;
  std::string media_type;
  std::uint64_t byte_size;
  core::ContentDigest content_digest;
  core::ContentAddressedStoreKey resolved_store_key;

  friend bool operator==(const VerifiedModelSourceArtifact&,
                         const VerifiedModelSourceArtifact&) = default;
};

enum class ParsedUrdfJointType : std::uint8_t {
  kRevolute = 0,
  kContinuous,
  kPrismatic,
  kFixed,
  kFloating,
  kPlanar,
};

struct ParsedUrdfJoint final {
  std::string name;
  ParsedUrdfJointType type;
  std::string parent_link_name;
  std::string child_link_name;
  std::optional<std::string> mimic_joint_name;

  friend bool operator==(const ParsedUrdfJoint&,
                         const ParsedUrdfJoint&) = default;
};

struct ParsedSrdfChain final {
  std::string base_link_name;
  std::string tip_link_name;

  friend bool operator==(const ParsedSrdfChain&,
                         const ParsedSrdfChain&) = default;
};

/// Direct members declared by one SRDF group.
///
/// Subgroup and chain expansion belongs to the later structural compiler.
struct ParsedSrdfGroup final {
  std::string name;
  std::vector<std::string> link_names;
  std::vector<std::string> joint_names;
  std::vector<ParsedSrdfChain> chains;
  std::vector<std::string> subgroup_names;

  friend bool operator==(const ParsedSrdfGroup&,
                         const ParsedSrdfGroup&) = default;
};

struct ParsedSrdfGroupStateJoint final {
  std::string joint_name;
  double value;

  friend bool operator==(const ParsedSrdfGroupStateJoint&,
                         const ParsedSrdfGroupStateJoint&) = default;
};

struct ParsedSrdfGroupState final {
  std::string name;
  std::string group_name;
  std::vector<ParsedSrdfGroupStateJoint> joints;

  friend bool operator==(const ParsedSrdfGroupState&,
                         const ParsedSrdfGroupState&) = default;
};

struct ParsedSrdfEndEffector final {
  std::string name;
  std::string parent_link_name;
  std::string group_name;
  std::optional<std::string> parent_group_name;

  friend bool operator==(const ParsedSrdfEndEffector&,
                         const ParsedSrdfEndEffector&) = default;
};

struct ParsedSrdfDisabledCollision final {
  std::string first_link_name;
  std::string second_link_name;
  std::optional<std::string> reason;

  friend bool operator==(const ParsedSrdfDisabledCollision&,
                         const ParsedSrdfDisabledCollision&) = default;
};

/// Typed, bounded structural index retained after source verification.
///
/// This checkpoint retains only names, link/joint topology, joint kinds,
/// mimic targets, SRDF structural references, and closure-local artifact
/// references. It is not a complete re-emittable URDF/SRDF IR: numeric model
/// details such as origins, axes, limits, inertials, and geometry parameters
/// remain a later compiler concern. The retained order is canonical and
/// independent of caller artifact order.
struct ParsedModelSourceIr final {
  std::string robot_name;
  std::string root_link_name;
  std::vector<std::string> material_names;
  std::vector<std::string> link_names;
  std::vector<ParsedUrdfJoint> joints;
  std::vector<ParsedSrdfGroup> groups;
  std::vector<ParsedSrdfGroupState> group_states;
  std::vector<ParsedSrdfEndEffector> end_effectors;
  std::vector<std::string> passive_joint_names;
  std::vector<ParsedSrdfDisabledCollision> disabled_collisions;
  std::vector<std::string> referenced_artifact_logical_uris;

  friend bool operator==(const ParsedModelSourceIr&,
                         const ParsedModelSourceIr&) = default;
};

/// A declared model asset whose complete caller-resolved source closure,
/// immutable artifact identities, bounded XML syntax, and retained URDF/SRDF
/// structural references have been verified.
///
/// This product proves neither graph placement nor lane/runtime compatibility.
class SourceVerifiedModelAsset final {
 public:
  [[nodiscard]] const core::ModelAssetDTO& value() const noexcept {
    return qualified_asset_.value();
  }
  [[nodiscard]] const QualifiedModelAsset& qualified_asset() const noexcept {
    return qualified_asset_;
  }
  [[nodiscard]] const std::vector<VerifiedModelSourceArtifact>&
  verified_artifacts() const noexcept {
    return verified_artifacts_;
  }
  [[nodiscard]] const ParsedModelSourceIr& parsed_ir() const noexcept {
    return parsed_ir_;
  }

 private:
  SourceVerifiedModelAsset(
      QualifiedModelAsset qualified_asset,
      std::vector<VerifiedModelSourceArtifact> verified_artifacts,
      ParsedModelSourceIr parsed_ir)
      : qualified_asset_(std::move(qualified_asset)),
        verified_artifacts_(std::move(verified_artifacts)),
        parsed_ir_(std::move(parsed_ir)) {}

  QualifiedModelAsset qualified_asset_;
  std::vector<VerifiedModelSourceArtifact> verified_artifacts_;
  ParsedModelSourceIr parsed_ir_;

  friend SourceVerifiedModelAsset verify_model_source_closure(
      const QualifiedModelAsset&,
      std::vector<ResolvedModelSourceArtifactCandidate>);
  friend SourceVerifiedModelAsset verify_model_source_closure(
      const ModelSourceRetrievalPlan&,
      std::vector<ResolvedModelSourceArtifactCandidate>);
};

/// Classifies and bounds one exact model-source declaration before retrieval.
///
/// The returned rows are canonical-sorted by logical URI and derive the only
/// legal immutable-store key from each declared digest.
[[nodiscard]] ModelSourceRetrievalPlan preflight_model_source_declaration(
    const QualifiedModelAsset& qualified_asset);

/// Verifies one exact, already-resolved expanded-model source closure.
///
/// The candidate set must match the declaration exactly. This function
/// performs no filesystem, network, package-index, ROS, or content-store I/O.
[[nodiscard]] SourceVerifiedModelAsset verify_model_source_closure(
    const ModelSourceRetrievalPlan& retrieval_plan,
    std::vector<ResolvedModelSourceArtifactCandidate> resolved_artifacts);

/// Convenience entrypoint that runs the same I2-owned declaration preflight.
[[nodiscard]] SourceVerifiedModelAsset verify_model_source_closure(
    const QualifiedModelAsset& qualified_asset,
    std::vector<ResolvedModelSourceArtifactCandidate> resolved_artifacts);

}  // namespace manyforge::deployment

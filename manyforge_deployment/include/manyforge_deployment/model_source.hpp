#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_core/types/model_asset.hpp>
#include <manyforge_core/types/placement_pose.hpp>
#include <manyforge_deployment/model_asset_codec.hpp>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
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
// One aggregate budget covers the URDF and optional SRDF entrypoints.
inline constexpr std::size_t kMaxModelSourceXmlDepth = 128U;
inline constexpr std::size_t kMaxModelSourceXmlElementCount = 250000U;
inline constexpr std::size_t kMaxModelSourceXmlAttributeCount = 1000000U;
// Collada dependencies are streamed, never retained as a DOM. A separate
// aggregate budget covers every Collada artifact in the same source closure
// so mesh syntax cannot consume or weaken the URDF/SRDF semantic budget.
inline constexpr std::size_t kMaxModelSourceColladaXmlDepth = 128U;
inline constexpr std::size_t kMaxModelSourceColladaXmlElementCount =
    250000U;
inline constexpr std::size_t kMaxModelSourceColladaXmlAttributeCount =
    1000000U;
// One aggregate budget covers every retained unique expanded link/joint row
// and every retained provenance occurrence. Each provenance occurrence owns
// exactly one index entry in an expanded member, so that 1:1 index is accounted
// by the same occurrence instead of being double-counted. Temporary chain-path
// traversal storage is independently bounded by the XML/topology ceilings.
inline constexpr std::size_t kMaxModelSourceExpandedSrdfSemanticRows =
    250000U;

class ModelSourceError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class ModelSourceArtifactKind : std::uint8_t {
  kUrdfXmlEntrypoint = 0,
  kSrdfXmlEntrypoint,
  kStlMesh,
  kColladaMesh,
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
  ModelSourceRetrievalPlan(const ModelSourceRetrievalPlan&) noexcept =
      default;
  ModelSourceRetrievalPlan& operator=(
      const ModelSourceRetrievalPlan&) noexcept = default;

  [[nodiscard]] const QualifiedModelAsset& qualified_asset() const noexcept {
    return data_->qualified_asset;
  }
  [[nodiscard]] const std::vector<ModelSourceRetrievalPlanRow>& artifacts()
      const noexcept {
    return data_->artifacts;
  }
  [[nodiscard]] std::uint64_t total_bytes() const noexcept {
    return data_->total_bytes;
  }

 private:
  struct Data final {
    Data(const QualifiedModelAsset& qualified_asset_in,
         std::vector<ModelSourceRetrievalPlanRow> artifacts_in,
         std::uint64_t total_bytes_in)
        : qualified_asset(qualified_asset_in),
          artifacts(std::move(artifacts_in)),
          total_bytes(total_bytes_in) {}

    QualifiedModelAsset qualified_asset;
    std::vector<ModelSourceRetrievalPlanRow> artifacts;
    std::uint64_t total_bytes;
  };

  ModelSourceRetrievalPlan(
      const QualifiedModelAsset& qualified_asset,
      std::vector<ModelSourceRetrievalPlanRow> artifacts,
      std::uint64_t total_bytes)
      : data_(std::make_shared<const Data>(
            qualified_asset, std::move(artifacts), total_bytes)) {}

  std::shared_ptr<const Data> data_;

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

using ModelSourceVector3 = std::array<double, 3U>;
using ModelSourceVector4 = std::array<double, 4U>;

enum class UrdfJointType : std::uint8_t {
  kRevolute = 0,
  kContinuous,
  kPrismatic,
  kFixed,
  kFloating,
  kPlanar,
};

enum class UrdfCoordinateKind : std::uint8_t {
  kNone = 0,
  kAngular,
  kLinear,
  kPlanar,
  kFloating,
};

/// One URDF origin with both authored presence and materialized semantics.
///
/// `xyz_m` and `rpy_rad` are always populated. Missing attributes materialize
/// to zero while `authored_xyz_m` / `authored_rpy_rad` retain whether the
/// source supplied them. `element_authored` distinguishes an omitted origin
/// from an explicitly authored empty `<origin/>`.
struct UrdfOriginIr final {
  bool element_authored = false;
  std::optional<ModelSourceVector3> authored_xyz_m;
  std::optional<ModelSourceVector3> authored_rpy_rad;
  ModelSourceVector3 xyz_m{0.0, 0.0, 0.0};
  ModelSourceVector3 rpy_rad{0.0, 0.0, 0.0};

  friend bool operator==(const UrdfOriginIr&, const UrdfOriginIr&) = default;
};

struct UrdfBoxGeometryIr final {
  ModelSourceVector3 size_m;

  friend bool operator==(const UrdfBoxGeometryIr&,
                         const UrdfBoxGeometryIr&) = default;
};

struct UrdfCylinderGeometryIr final {
  double radius_m;
  double length_m;

  friend bool operator==(const UrdfCylinderGeometryIr&,
                         const UrdfCylinderGeometryIr&) = default;
};

struct UrdfSphereGeometryIr final {
  double radius_m;

  friend bool operator==(const UrdfSphereGeometryIr&,
                         const UrdfSphereGeometryIr&) = default;
};

struct UrdfMeshGeometryIr final {
  std::string logical_uri;
  std::optional<ModelSourceVector3> authored_scale;
  ModelSourceVector3 scale{1.0, 1.0, 1.0};

  friend bool operator==(const UrdfMeshGeometryIr&,
                         const UrdfMeshGeometryIr&) = default;
};

using UrdfGeometryIr =
    std::variant<UrdfBoxGeometryIr, UrdfCylinderGeometryIr,
                 UrdfSphereGeometryIr, UrdfMeshGeometryIr>;

struct UrdfMaterialColorIr final {
  ModelSourceVector4 rgba;

  friend bool operator==(const UrdfMaterialColorIr&,
                         const UrdfMaterialColorIr&) = default;
};

struct UrdfMaterialTextureIr final {
  std::string logical_uri;

  friend bool operator==(const UrdfMaterialTextureIr&,
                         const UrdfMaterialTextureIr&) = default;
};

struct UrdfResolvedMaterialReferenceIr final {
  std::string definition_name;
  std::optional<UrdfMaterialColorIr> color;
  std::optional<UrdfMaterialTextureIr> texture;

  friend bool operator==(const UrdfResolvedMaterialReferenceIr&,
                         const UrdfResolvedMaterialReferenceIr&) = default;
};

/// A complete top-level or inline URDF material occurrence.
///
/// Every material in a source-qualified root authored a `name` attribute. A
/// top-level material has a non-empty name plus color or texture. An inline
/// color/texture definition may use the common-consumer `name=""` spelling,
/// represented by `nullopt` and re-emitted exactly as an empty attribute; an
/// inline material with only a non-empty name is a reference. Exact reference
/// identity and its resolved immutable definition are both retained so no
/// downstream consumer repeats name resolution. Named inline color/texture
/// definitions remain complete occurrences rather than being flattened into
/// the referenced material.
struct UrdfMaterialIr final {
  std::optional<std::string> name;
  std::optional<UrdfMaterialColorIr> color;
  std::optional<UrdfMaterialTextureIr> texture;
  std::optional<UrdfResolvedMaterialReferenceIr> resolved_reference;

  friend bool operator==(const UrdfMaterialIr&,
                         const UrdfMaterialIr&) = default;
};

struct UrdfVisualIr final {
  std::optional<std::string> name;
  UrdfOriginIr origin;
  UrdfGeometryIr geometry;
  std::optional<UrdfMaterialIr> material;

  friend bool operator==(const UrdfVisualIr&, const UrdfVisualIr&) = default;
};

struct UrdfCollisionIr final {
  std::optional<std::string> name;
  UrdfOriginIr origin;
  UrdfGeometryIr geometry;

  friend bool operator==(const UrdfCollisionIr&,
                         const UrdfCollisionIr&) = default;
};

struct UrdfInertiaIr final {
  double ixx;
  double ixy;
  double ixz;
  double iyy;
  double iyz;
  double izz;

  friend bool operator==(const UrdfInertiaIr&, const UrdfInertiaIr&) =
      default;
};

struct UrdfInertialIr final {
  UrdfOriginIr origin;
  double mass_kg;
  UrdfInertiaIr inertia;

  friend bool operator==(const UrdfInertialIr&,
                         const UrdfInertialIr&) = default;
};

struct UrdfLinkIr final {
  std::string name;
  std::optional<UrdfInertialIr> inertial;
  std::vector<UrdfVisualIr> visuals;
  std::vector<UrdfCollisionIr> collisions;

  friend bool operator==(const UrdfLinkIr&, const UrdfLinkIr&) = default;
};

struct UrdfAxisIr final {
  bool element_authored = false;
  std::optional<ModelSourceVector3> authored_xyz;
  ModelSourceVector3 xyz{1.0, 0.0, 0.0};

  friend bool operator==(const UrdfAxisIr&, const UrdfAxisIr&) = default;
};

struct UrdfCalibrationIr final {
  std::optional<double> rising;
  std::optional<double> falling;
  std::optional<double> authored_reference_position;
  double reference_position = 0.0;

  friend bool operator==(const UrdfCalibrationIr&,
                         const UrdfCalibrationIr&) = default;
};

struct UrdfDynamicsIr final {
  std::optional<double> authored_damping;
  std::optional<double> authored_friction;
  double damping = 0.0;
  double friction = 0.0;

  friend bool operator==(const UrdfDynamicsIr&,
                         const UrdfDynamicsIr&) = default;
};

struct UrdfLimitIr final {
  std::optional<double> authored_lower;
  std::optional<double> authored_upper;
  double lower = 0.0;
  double upper = 0.0;
  double effort = 0.0;
  double velocity = 0.0;
  std::optional<double> acceleration;

  friend bool operator==(const UrdfLimitIr&, const UrdfLimitIr&) = default;
};

struct UrdfMimicIr final {
  std::string joint_name;
  std::optional<double> authored_multiplier;
  std::optional<double> authored_offset;
  double multiplier = 1.0;
  double offset = 0.0;

  friend bool operator==(const UrdfMimicIr&, const UrdfMimicIr&) = default;
};

struct UrdfSafetyControllerIr final {
  std::optional<double> authored_soft_lower_limit;
  std::optional<double> authored_soft_upper_limit;
  std::optional<double> authored_k_position;
  double soft_lower_limit = 0.0;
  double soft_upper_limit = 0.0;
  double k_position = 0.0;
  double k_velocity = 0.0;

  friend bool operator==(const UrdfSafetyControllerIr&,
                         const UrdfSafetyControllerIr&) = default;
};

struct UrdfJointIr final {
  std::string name;
  UrdfJointType type;
  UrdfCoordinateKind coordinate_kind;
  std::string parent_link_name;
  std::string child_link_name;
  UrdfOriginIr origin;
  std::optional<UrdfAxisIr> axis;
  std::optional<UrdfCalibrationIr> calibration;
  std::optional<UrdfDynamicsIr> dynamics;
  std::optional<UrdfLimitIr> limit;
  std::optional<UrdfMimicIr> mimic;
  std::optional<UrdfSafetyControllerIr> safety_controller;

  friend bool operator==(const UrdfJointIr&, const UrdfJointIr&) = default;
};

struct UrdfDocumentIr final {
  std::string robot_name;
  std::vector<UrdfMaterialIr> materials;
  std::vector<UrdfLinkIr> links;
  std::vector<UrdfJointIr> joints;
  std::uint32_t version_major = 1U;
  std::uint32_t version_minor = 0U;

  friend bool operator==(const UrdfDocumentIr&, const UrdfDocumentIr&) =
      default;
};

struct SrdfLinkGroupMemberIr final {
  std::string link_name;

  friend bool operator==(const SrdfLinkGroupMemberIr&,
                         const SrdfLinkGroupMemberIr&) = default;
};

struct SrdfJointGroupMemberIr final {
  std::string joint_name;

  friend bool operator==(const SrdfJointGroupMemberIr&,
                         const SrdfJointGroupMemberIr&) = default;
};

struct SrdfChainGroupMemberIr final {
  std::string base_link_name;
  std::string tip_link_name;

  friend bool operator==(const SrdfChainGroupMemberIr&,
                         const SrdfChainGroupMemberIr&) = default;
};

struct SrdfSubgroupMemberIr final {
  std::string group_name;

  friend bool operator==(const SrdfSubgroupMemberIr&,
                         const SrdfSubgroupMemberIr&) = default;
};

using SrdfGroupMemberIr =
    std::variant<SrdfLinkGroupMemberIr, SrdfJointGroupMemberIr,
                 SrdfChainGroupMemberIr, SrdfSubgroupMemberIr>;

struct SrdfGroupIr final {
  std::string name;
  std::vector<SrdfGroupMemberIr> members;

  friend bool operator==(const SrdfGroupIr&, const SrdfGroupIr&) = default;
};

struct SrdfGroupStateJointIr final {
  std::string joint_name;
  double value;

  friend bool operator==(const SrdfGroupStateJointIr&,
                         const SrdfGroupStateJointIr&) = default;
};

struct SrdfGroupStateIr final {
  std::string name;
  std::string group_name;
  std::vector<SrdfGroupStateJointIr> joints;

  friend bool operator==(const SrdfGroupStateIr&,
                         const SrdfGroupStateIr&) = default;
};

struct SrdfEndEffectorIr final {
  std::string name;
  std::string parent_link_name;
  std::string group_name;
  std::optional<std::string> parent_group_name;

  friend bool operator==(const SrdfEndEffectorIr&,
                         const SrdfEndEffectorIr&) = default;
};

struct SrdfDisabledCollisionIr final {
  std::string first_link_name;
  std::string second_link_name;
  std::optional<std::string> reason;

  friend bool operator==(const SrdfDisabledCollisionIr&,
                         const SrdfDisabledCollisionIr&) = default;
};

struct SrdfDocumentIr final {
  std::string robot_name;
  std::vector<SrdfGroupIr> groups;
  std::vector<SrdfGroupStateIr> group_states;
  std::vector<SrdfEndEffectorIr> end_effectors;
  std::vector<std::string> passive_joint_names;
  std::vector<SrdfDisabledCollisionIr> disabled_collisions;

  friend bool operator==(const SrdfDocumentIr&, const SrdfDocumentIr&) =
      default;
};

/// Syntax- and semantics-checked model documents without a source-qualified
/// artifact claim. Runtime owners use this only after their retained planning
/// model has already been independently verified; it reuses the compiler's
/// bounded parser instead of reconstructing SRDF meaning in an adapter.
struct ParsedModelDocuments final {
  UrdfDocumentIr urdf;
  std::optional<SrdfDocumentIr> srdf;
};

enum class SourceArtifactReferenceKind : std::uint8_t {
  kMesh = 0,
  kTexture,
};

struct SourceArtifactReferenceIr final {
  std::string logical_uri;
  SourceArtifactReferenceKind kind;

  friend bool operator==(const SourceArtifactReferenceIr&,
                         const SourceArtifactReferenceIr&) = default;
};

struct ModelSourceNameIndexEntryIr final {
  std::string name;
  std::size_t source_index;

  friend bool operator==(const ModelSourceNameIndexEntryIr&,
                         const ModelSourceNameIndexEntryIr&) = default;
};

struct ModelSourceGroupStateIndexEntryIr final {
  std::string group_name;
  std::string state_name;
  std::size_t source_index;

  friend bool operator==(const ModelSourceGroupStateIndexEntryIr&,
                         const ModelSourceGroupStateIndexEntryIr&) = default;
};

enum class SrdfExpansionProvenanceKind : std::uint8_t {
  kDirectLink = 0,
  kDirectJoint,
  kChainLink,
  kChainJoint,
  kSubgroup,
};

/// One node in a compact, bounded SRDF expansion-provenance DAG.
///
/// Direct/chain nodes identify the authored member that contributed a symbol.
/// A subgroup node identifies the authored subgroup inclusion and points to
/// the child's provenance node. Following `nested_provenance_index` therefore
/// reconstructs the complete inclusion path without copying path strings
/// quadratically for deep subgroup graphs.
struct SrdfExpansionProvenanceNodeIr final {
  std::string expansion_group_name;
  std::size_t source_member_index;
  SrdfExpansionProvenanceKind kind;
  std::string source_symbol_name;
  std::optional<std::string> chain_base_link_name;
  std::optional<std::string> chain_tip_link_name;
  std::optional<std::size_t> nested_provenance_index;

  friend bool operator==(const SrdfExpansionProvenanceNodeIr&,
                         const SrdfExpansionProvenanceNodeIr&) = default;
};

struct SrdfExpandedMemberIr final {
  std::string name;
  std::vector<std::size_t> provenance_indices;

  friend bool operator==(const SrdfExpandedMemberIr&,
                         const SrdfExpandedMemberIr&) = default;
};

struct ExpandedSrdfGroupIr final {
  std::string group_name;
  std::vector<SrdfExpandedMemberIr> links;
  std::vector<SrdfExpandedMemberIr> joints;

  friend bool operator==(const ExpandedSrdfGroupIr&,
                         const ExpandedSrdfGroupIr&) = default;
};

/// Deterministic indexes and fully expanded SRDF semantics.
struct ModelSourceSemanticIndex final {
  std::string root_link_name;
  std::vector<ModelSourceNameIndexEntryIr> material_names;
  std::vector<ModelSourceNameIndexEntryIr> link_names;
  std::vector<ModelSourceNameIndexEntryIr> joint_names;
  std::vector<ModelSourceNameIndexEntryIr> group_names;
  std::vector<ModelSourceGroupStateIndexEntryIr> group_state_names;
  std::vector<ModelSourceNameIndexEntryIr> end_effector_names;
  std::vector<std::string> group_expansion_postorder;
  std::vector<SrdfExpansionProvenanceNodeIr> group_expansion_provenance;
  std::vector<ExpandedSrdfGroupIr> expanded_groups;

  friend bool operator==(const ModelSourceSemanticIndex&,
                         const ModelSourceSemanticIndex&) = default;
};

class SourceVerifiedModelAsset;

/// Complete, typed, re-emittable URDF/SRDF meaning for one verified source.
///
/// Construction is private so a caller cannot forge source qualification by
/// assembling otherwise well-typed public leaf values. Leaf values remain
/// ordinary transformable compiler DTOs for later pure compilation; only this
/// trust-bearing root and SourceVerifiedModelAsset claim non-forgeability.
/// Authored default presence is retained as provenance. Non-semantic named and
/// geometry collections are normalized before construction; ordered tagged
/// SRDF group members remain source-ordered because they define first-
/// occurrence target-resource order.
class ModelSourceIr final {
 public:
  ModelSourceIr(const ModelSourceIr&) noexcept = default;
  ModelSourceIr& operator=(const ModelSourceIr&) noexcept = default;

  [[nodiscard]] const UrdfDocumentIr& urdf() const noexcept {
    return data_->urdf;
  }
  [[nodiscard]] const std::optional<SrdfDocumentIr>& srdf() const noexcept {
    return data_->srdf;
  }
  [[nodiscard]] const ModelSourceSemanticIndex& semantic_index()
      const noexcept {
    return data_->semantic_index;
  }
  [[nodiscard]] const std::vector<SourceArtifactReferenceIr>&
  artifact_references() const noexcept {
    return data_->artifact_references;
  }

  /// Semantic equality deliberately excludes authored-default provenance.
  friend bool operator==(const ModelSourceIr&, const ModelSourceIr&);

 private:
  struct Data final {
    Data(
        UrdfDocumentIr urdf_in, std::optional<SrdfDocumentIr> srdf_in,
        ModelSourceSemanticIndex semantic_index_in,
        std::vector<SourceArtifactReferenceIr> artifact_references_in)
        : urdf(std::move(urdf_in)),
          srdf(std::move(srdf_in)),
          semantic_index(std::move(semantic_index_in)),
          artifact_references(std::move(artifact_references_in)) {}

    UrdfDocumentIr urdf;
    std::optional<SrdfDocumentIr> srdf;
    ModelSourceSemanticIndex semantic_index;
    std::vector<SourceArtifactReferenceIr> artifact_references;
  };

  ModelSourceIr(
      UrdfDocumentIr urdf, std::optional<SrdfDocumentIr> srdf,
      ModelSourceSemanticIndex semantic_index,
      std::vector<SourceArtifactReferenceIr> artifact_references)
      : data_(std::make_shared<const Data>(
            std::move(urdf), std::move(srdf),
            std::move(semantic_index),
            std::move(artifact_references))) {}

  std::shared_ptr<const Data> data_;

  friend class SourceVerifiedModelAsset;
  friend SourceVerifiedModelAsset verify_model_source_closure(
      const QualifiedModelAsset&,
      std::vector<ResolvedModelSourceArtifactCandidate>);
  friend SourceVerifiedModelAsset verify_model_source_closure(
      const ModelSourceRetrievalPlan&,
      std::vector<ResolvedModelSourceArtifactCandidate>);
};

/// A declared model asset whose complete caller-resolved source closure,
/// immutable artifact identities, bounded XML syntax, and retained URDF/SRDF
/// structural references have been verified.
///
/// The exact I2 retrieval plan is retained transitively as source provenance;
/// the model-asset views below derive from that sole owned plan.
///
/// This product proves neither graph placement nor lane/runtime compatibility.
class SourceVerifiedModelAsset final {
 public:
  SourceVerifiedModelAsset(const SourceVerifiedModelAsset&) noexcept =
      default;
  SourceVerifiedModelAsset& operator=(
      const SourceVerifiedModelAsset&) noexcept = default;

  [[nodiscard]] const core::ModelAssetDTO& value() const noexcept {
    return data_->retrieval_plan.qualified_asset().value();
  }
  [[nodiscard]] const QualifiedModelAsset& qualified_asset() const noexcept {
    return data_->retrieval_plan.qualified_asset();
  }
  [[nodiscard]] const ModelSourceRetrievalPlan& retrieval_plan()
      const noexcept {
    return data_->retrieval_plan;
  }
  [[nodiscard]] const std::vector<VerifiedModelSourceArtifact>&
  verified_artifacts() const noexcept {
    return data_->verified_artifacts;
  }
  [[nodiscard]] const ModelSourceIr& source_ir() const noexcept {
    return data_->source_ir;
  }

 private:
  struct Data final {
    Data(const ModelSourceRetrievalPlan& retrieval_plan_in,
         std::vector<VerifiedModelSourceArtifact> verified_artifacts_in,
         ModelSourceIr source_ir_in)
        : retrieval_plan(retrieval_plan_in),
          verified_artifacts(std::move(verified_artifacts_in)),
          source_ir(std::move(source_ir_in)) {}

    ModelSourceRetrievalPlan retrieval_plan;
    std::vector<VerifiedModelSourceArtifact> verified_artifacts;
    ModelSourceIr source_ir;
  };

  SourceVerifiedModelAsset(
      const ModelSourceRetrievalPlan& retrieval_plan,
      std::vector<VerifiedModelSourceArtifact> verified_artifacts,
      ModelSourceIr source_ir)
      : data_(std::make_shared<const Data>(
            retrieval_plan, std::move(verified_artifacts),
            std::move(source_ir))) {}

  std::shared_ptr<const Data> data_;

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

/// Emits deterministic, compact XML for the complete verified typed source.
///
/// Attribute/category order and round-trip number spelling are fixed by the
/// compiler contract. Authored-default provenance is retained in the IR but
/// excluded from this semantic projection: omitted and explicitly authored
/// default origins, axes, mesh scales, and mimic affine values emit the same
/// bytes.
[[nodiscard]] std::string emit_canonical_urdf(const ModelSourceIr& source_ir);
[[nodiscard]] std::optional<std::string> emit_canonical_srdf(
    const ModelSourceIr& source_ir);

/// Emits a transformed compiler document without claiming source
/// qualification for that transformable leaf aggregate.
[[nodiscard]] std::string emit_canonical_urdf_document(
    const UrdfDocumentIr& document);
[[nodiscard]] std::string emit_canonical_srdf_document(
    const SrdfDocumentIr& document);

/// One offset frame of a model occurrence: a generated massless link
/// `link_name` (no inertial, visual or collision) fixed to `parent_link_name`
/// at `pose` (T_parent_link). A descriptor offset frame (`custom == false`)
/// hangs off a source link; a Deployment custom TCP also off a descriptor
/// offset frame, never off another custom TCP.
struct OffsetFrameLinkIr final {
  std::string link_name;
  std::string parent_link_name;
  core::PlacementPoseDTO pose;
  bool custom = false;

  friend bool operator==(const OffsetFrameLinkIr&,
                         const OffsetFrameLinkIr&) = default;
};

/// The URDF origin of a placement pose (quaternion to roll-pitch-yaw).
[[nodiscard]] UrdfOriginIr urdf_origin_from_pose(
    const core::PlacementPoseDTO& pose);

/// The source-local name of an offset frame's generated fixed joint.
[[nodiscard]] std::string offset_frame_joint_name(std::string_view link_name);

/// A model occurrence's offset frames: the asset's offset frame roles, then
/// the occurrence's custom TCPs.
[[nodiscard]] std::vector<OffsetFrameLinkIr> occurrence_offset_frames(
    std::span<const core::ModelFrameRoleBindingDTO> asset_bindings,
    std::span<const core::ModelCustomTcpDTO> custom_tcps);

/// `urdf` plus one generated link and fixed joint per offset frame, in
/// canonical order. Throws ModelSourceError (`tcp_parent_unknown`,
/// `custom_tcp_parent_unknown`, `tcp_name_duplicate`,
/// `custom_tcp_name_collision`).
[[nodiscard]] UrdfDocumentIr with_offset_frame_links(
    UrdfDocumentIr urdf, std::span<const OffsetFrameLinkIr> frames);

/// The generated fixed joint of one offset frame.
[[nodiscard]] UrdfJointIr offset_frame_joint(const OffsetFrameLinkIr& frame);

/// `urdf_bytes` with one empty (massless) link per fixed joint's child and
/// the joint itself inserted before the final `</robot>`; every other byte
/// is kept. robot_state_publisher publishes such joints on /tf_static.
[[nodiscard]] std::string append_fixed_links(
    std::string_view urdf_bytes, std::span<const UrdfJointIr> joints);

[[nodiscard]] ParsedModelDocuments parse_model_documents_for_projection(
    std::string_view urdf_bytes, std::optional<std::string_view> srdf_bytes);

}  // namespace manyforge::deployment

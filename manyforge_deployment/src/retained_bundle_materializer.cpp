#include "manyforge_deployment/retained_bundle.hpp"

#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "build_manifest_internal.hpp"
#include "content_identity_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/model_source.hpp"
#include "retained_bundle_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] const BuildQualifiedProductContract&
require_flattened_model_contract() {
  const auto* contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(
          std::string(detail::flattened_model_product_contract_id())),
      detail::flattened_model_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      contract->placement() !=
          ProductPlacement::kCellPlanningCollisionModel) {
    throw std::logic_error(
        "build-qualified flattened-model product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] CanonicalValue node_environment_projection(
    const core::RetainedNodeEnvironmentDTO& environment) {
  return CanonicalValue(CanonicalValue::Object{
      {"catalog_id", CanonicalValue(environment.catalog_id)},
      {"content_version", CanonicalValue(environment.content_version)},
      {"catalog_digest",
       CanonicalValue(environment.catalog_digest.value())},
      {"environment_digest",
       CanonicalValue(environment.environment_digest.value())},
  });
}

[[nodiscard]] CanonicalValue records_projection(
    std::span<const core::CatalogRecordRefDTO> records) {
  const CanonicalValue catalogs =
      detail::encode_deployment_catalogs(records);
  const CanonicalValue* encoded_records =
      catalogs.find_object_member("records");
  if (encoded_records == nullptr) {
    throw std::logic_error(
        "deployment catalog codec omitted its required records member");
  }
  return *encoded_records;
}

[[nodiscard]] CanonicalValue catalogs_projection(
    std::span<const core::CatalogRecordRefDTO> records,
    const core::RetainedNodeEnvironmentDTO& environment) {
  const auto& schema_registry = build_qualified_closed_schema_registry();
  return CanonicalValue(CanonicalValue::Object{
      {"dialect_api_version",
       CanonicalValue(std::string(kClosedSchemaApiVersion))},
      {"bootstrap_contract_digest",
       CanonicalValue(std::string(kClosedSchemaBootstrapContractDigest))},
      {"qualification_registry_digest",
       CanonicalValue(
           schema_registry.qualification_registry_digest().value())},
      {"records", records_projection(records)},
      {"schema_artifacts", CanonicalValue(CanonicalValue::Array{})},
      {"node_environment", node_environment_projection(environment)},
  });
}

[[nodiscard]] CanonicalValue compiled_products_projection(
    const core::RetainedProductDTO& planning_collision_model) {
  return CanonicalValue(CanonicalValue::Object{
      {"planning_collision_model",
       detail::retained_product_projection(planning_collision_model)},
      {"viewer_model", CanonicalValue(nullptr)},
      {"control_domain_products", CanonicalValue(CanonicalValue::Array{})},
      {"symbol_role_maps", CanonicalValue(CanonicalValue::Array{})},
      {"runtime_unit_products", CanonicalValue(CanonicalValue::Array{})},
  });
}

[[nodiscard]] CanonicalValue provenance_projection(
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const core::ContentDigest output_digest =
      detail::retained_product_output_digest(planning_collision_model);
  CanonicalValue::Array outputs;
  outputs.emplace_back(CanonicalValue::Object{
      {"product_id",
       CanonicalValue(planning_collision_model.product_id.value())},
      {"product_kind",
       CanonicalValue(std::string(
           core::to_string(planning_collision_model.product_kind)))},
      {"content_digest", CanonicalValue(output_digest.value())},
  });
  return CanonicalValue(CanonicalValue::Object{
      {"compiler_contract_digest",
       CanonicalValue(context.compiler_contract_digest().value())},
      {"compiler_build_digest",
       CanonicalValue(context.compiler_build_digest().value())},
      {"source_records", CanonicalValue(CanonicalValue::Array{})},
      {"generator_records", CanonicalValue(CanonicalValue::Array{})},
      {"output_records", CanonicalValue(std::move(outputs))},
  });
}

[[nodiscard]] core::RetainedBundleDTO retained_bundle_value(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment& qualified,
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const auto& deployment = qualified.deployment();
  const auto& registry = build_qualified_closed_schema_registry();
  return core::RetainedBundleDTO{
      std::string(core::kRetainedBundleApiVersion),
      core::RetainedBundleId(std::string(retained_bundle_id)),
      deployment.deployment_artifact_id(),
      deployment.deployment_revision(),
      deployment.graph().graph(),
      core::RetainedCatalogDTO{
          std::string(kClosedSchemaApiVersion),
          core::ContentDigest(
              std::string(kClosedSchemaBootstrapContractDigest)),
          registry.qualification_registry_digest(),
          deployment.catalog_records(),
          {},
          context.node_environment(),
      },
      deployment.applicability_inputs()
          .value()
          .desired_control_configurations,
      deployment.applicability_inputs()
          .value()
          .capability_provider_bindings,
      core::RetainedCompiledProductsDTO{
          planning_collision_model,
          std::nullopt,
          {},
          {},
          {},
      },
      core::RetainedBundleProvenanceDTO{
          context.compiler_contract_digest(),
          context.compiler_build_digest(),
          {},
          {},
          {core::RetainedOutputRecordDTO{
              planning_collision_model.product_id,
              planning_collision_model.product_kind,
              detail::retained_product_output_digest(
                  planning_collision_model),
          }},
      },
  };
}

[[nodiscard]] CanonicalValue retained_bundle_projection(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment& qualified,
    const detail::BuildQualifiedRetentionContext& context,
    const core::RetainedProductDTO& planning_collision_model) {
  const auto& deployment = qualified.deployment();
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(core::kRetainedBundleApiVersion))},
      {"retained_bundle_id",
       CanonicalValue(std::string(retained_bundle_id))},
      {"deployment_artifact_id",
       CanonicalValue(deployment.deployment_artifact_id().value())},
      {"deployment_revision",
       CanonicalValue(deployment.deployment_revision().value())},
      {"cell_graph", deployment.graph().normalized_root()},
      {"catalogs",
       catalogs_projection(deployment.catalog_records(),
                           context.node_environment())},
      {"desired_control_configurations",
       deployment.applicability_inputs()
           .normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       deployment.applicability_inputs()
           .normalized_capability_provider_bindings()},
      {"compiled_products",
       compiled_products_projection(planning_collision_model)},
      {"provenance",
       provenance_projection(context, planning_collision_model)},
  });
}

void require_current_subset_before_store(
    const RetentionQualifiedDeployment& qualified) {
  const auto& deployment = qualified.deployment();
  const auto& graph = deployment.graph().graph();
  const auto& inputs = deployment.applicability_inputs().value();
  const auto& compiler = qualified.compiler_ir();

  if (!graph.runtime_units.empty() || !graph.model_instances.empty() ||
      !graph.controllable_components.empty() ||
      !graph.control_domains.empty() || !graph.control_bindings.empty() ||
      !graph.component_attachments.empty() ||
      !deployment.catalog_records().empty() ||
      !inputs.desired_control_configurations.empty() ||
      !inputs.capability_provider_bindings.empty() ||
      !inputs.external_resources.empty() ||
      inputs.site_overlay_ref.has_value() ||
      !compiler.catalog_record_refs().empty()) {
    detail::fail_retained_bundle(
        "retained-bundle materialization currently qualifies only the "
        "canonical-empty deployment subset");
  }

  const auto& applicability = compiler.applicability();
  const auto& composition = applicability.composition();
  if (!composition.model_assets().empty() ||
      !composition.component_types().empty() ||
      !composition.model_occurrences().empty() ||
      !composition.symbol_rewrites().empty() ||
      !composition.reverse_symbol_rewrites().empty() ||
      !composition.reference_rewrites().empty() ||
      !composition.frames().empty() ||
      !composition.fixed_connections().empty() ||
      !composition.coordinates().empty() ||
      !composition.control_binding_coordinate_maps().empty() ||
      !composition.targets().empty() ||
      !applicability.runtime_profiles().empty() ||
      !applicability.capability_definitions().empty() ||
      !applicability.control_interface_contracts().empty() ||
      !applicability.controller_topologies().empty() ||
      !applicability.provider_manifests().empty() ||
      !applicability.stop_evidence_policies().empty() ||
      !applicability.binding_interfaces().empty() ||
      !applicability.controllers().empty() ||
      !applicability.routes().empty() ||
      !applicability.capabilities().empty() ||
      !applicability.provider_endpoints().empty() ||
      !applicability.external_resources().empty() ||
      !applicability.stop_applicability().empty()) {
    detail::fail_retained_bundle(
        "canonical-empty compiler IR contains an unexpected semantic "
        "product input");
  }

  const auto& cell_frame = composition.cell_frame();
  const auto& urdf = composition.flattened_urdf();
  const auto& srdf = composition.flattened_srdf();
  const auto& frame_paths = composition.frame_paths();
  const auto* path_cell_frame =
      frame_paths.size() == 1U
          ? std::get_if<CompositionCellFrame>(
                &frame_paths.front().terminal_frame)
          : nullptr;
  if (cell_frame.cell_id != graph.cell_id ||
      urdf.robot_name.empty() || !urdf.materials.empty() ||
      urdf.links.size() != 1U || !urdf.joints.empty() ||
      urdf.links.front().name != cell_frame.compiled_frame_name ||
      urdf.links.front().inertial.has_value() ||
      !urdf.links.front().visuals.empty() ||
      !urdf.links.front().collisions.empty() ||
      srdf.robot_name != urdf.robot_name || !srdf.groups.empty() ||
      !srdf.group_states.empty() || !srdf.end_effectors.empty() ||
      !srdf.passive_joint_names.empty() ||
      !srdf.disabled_collisions.empty() || path_cell_frame == nullptr ||
      *path_cell_frame != cell_frame ||
      !frame_paths.front().segments.empty()) {
    detail::fail_retained_bundle(
        "canonical-empty compiler IR does not contain the exact semantic "
        "cell-frame product");
  }
}

struct PreparedCanonicalEmptyProduct final {
  std::string urdf;
  std::string srdf;
  const BuildQualifiedProductContract* contract;
};

[[nodiscard]] PreparedCanonicalEmptyProduct
prepare_canonical_empty_product_before_store(
    const RetentionQualifiedDeployment& qualified) {
  require_current_subset_before_store(qualified);
  const auto& composition =
      qualified.compiler_ir().applicability().composition();
  std::string urdf =
      emit_canonical_urdf_document(composition.flattened_urdf());
  std::string srdf =
      emit_canonical_srdf_document(composition.flattened_srdf());
  const auto& contract = require_flattened_model_contract();
  return {std::move(urdf), std::move(srdf), &contract};
}

void require_verified_product_artifact_readback(
    const core::RetainedProductDTO& product,
    IRawArtifactStore& artifact_store) {
  for (const auto& artifact : product.artifact_refs) {
    const auto bytes = artifact_store.get_verified(
        artifact.content_addressed_store_key, artifact.content_digest,
        artifact.byte_size);
    if (!bytes.has_value() ||
        static_cast<std::uint64_t>(bytes->size()) != artifact.byte_size ||
        sha256_digest(*bytes) != artifact.content_digest.value() ||
        artifact.content_addressed_store_key.value() !=
            detail::retained_store_key_for_digest(
                artifact.content_digest.value())) {
      throw std::runtime_error(
          "production retention could not re-verify a referenced raw "
          "artifact");
    }
  }
}

void require_verified_current_bundle_artifacts(
    const RetainedBundle& bundle, IRawArtifactStore& artifact_store) {
  const auto& value = bundle.value();
  if (!value.catalogs.records.empty() ||
      !value.catalogs.schema_artifacts.empty()) {
    throw std::logic_error(
        "current production retention cannot verify non-empty catalog "
        "content through the bounded raw-artifact port");
  }

  const auto& products = value.compiled_products;
  require_verified_product_artifact_readback(
      products.planning_collision_model, artifact_store);
  if (products.viewer_model.has_value()) {
    require_verified_product_artifact_readback(
        *products.viewer_model, artifact_store);
  }
  for (const auto& group : products.control_domain_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifact_readback(product, artifact_store);
    }
  }
  for (const auto& product : products.symbol_role_maps) {
    require_verified_product_artifact_readback(product, artifact_store);
  }
  for (const auto& group : products.runtime_unit_products) {
    for (const auto& product : group.products) {
      require_verified_product_artifact_readback(product, artifact_store);
    }
  }
}

}  // namespace

detail::BuildQualifiedRetentionContext::BuildQualifiedRetentionContext(
    core::RetainedNodeEnvironmentDTO node_environment,
    core::ContentDigest compiler_contract_digest,
    core::ContentDigest compiler_build_digest)
    : node_environment_(std::move(node_environment)),
      compiler_contract_digest_(std::move(compiler_contract_digest)),
      compiler_build_digest_(std::move(compiler_build_digest)) {
  detail::require_retained_portable_identity(
      node_environment_.catalog_id, "node_environment.catalog_id");
  detail::require_retained_portable_identity(
      node_environment_.content_version, "node_environment.content_version");
  detail::require_retained_digest(node_environment_.catalog_digest.value(),
                                  "node_environment.catalog_digest");
  detail::require_retained_digest(node_environment_.environment_digest.value(),
                                  "node_environment.environment_digest");
  detail::require_retained_digest(compiler_contract_digest_.value(),
                                  "compiler_contract_digest");
  detail::require_retained_digest(compiler_build_digest_.value(),
                                  "compiler_build_digest");
}

detail::BuildQualifiedRetentionContext::BuildQualifiedRetentionContext(
    core::RetainedNodeEnvironmentDTO node_environment,
    core::ContentDigest compiler_contract_digest,
    core::ContentDigest compiler_build_digest,
    std::vector<std::shared_ptr<const BuildQualifiedManifest>> manifests)
    : BuildQualifiedRetentionContext(
          std::move(node_environment), std::move(compiler_contract_digest),
          std::move(compiler_build_digest)) {
  if (manifests.size() != 3U || manifests[0] == nullptr ||
      manifests[1] == nullptr || manifests[2] == nullptr ||
      manifests[0]->key().manifest_kind !=
          BuildManifestKind::kNodeEnvironment ||
      manifests[1]->key().manifest_kind !=
          BuildManifestKind::kCompilerContract ||
      manifests[2]->key().manifest_kind !=
          BuildManifestKind::kCompilerBuild ||
      manifests[0]->key().content_digest !=
          node_environment_.environment_digest ||
      manifests[1]->key().content_digest != compiler_contract_digest_ ||
      manifests[2]->key().content_digest != compiler_build_digest_) {
    throw std::logic_error(
        "production retention context manifest closure is incoherent");
  }
  manifests_ = std::move(manifests);
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize_prepared(
    const RetentionQualifiedDeployment& qualified,
    const BuildQualifiedRetentionContext& context,
    std::string_view urdf, std::string_view srdf,
    const BuildQualifiedProductContract& contract,
    IRawArtifactStore& artifact_store) {
  const std::vector<detail::RetainedArtifactInput> artifacts{
      {core::ArtifactRoleId("srdf"), "application/xml", std::string(srdf)},
      {core::ArtifactRoleId("urdf"), "application/xml", std::string(urdf)},
  };
  const core::RetainedProductDTO planning =
      detail::build_and_retain_product(
          core::ProductId("cell_planning_collision_model"),
          core::RetainedProductKind::kPlanningCollisionModel,
          ProductPlacement::kCellPlanningCollisionModel,
          contract.product_contract_id(),
          contract.product_contract_revision(), artifacts,
          artifact_store);

  const CanonicalValue placeholder = retained_bundle_projection(
      kZeroDigest, qualified, context, planning);
  const std::string retained_bundle_id = detail::compute_content_identity(
      placeholder,
      detail::ContentIdentitySpec{
          .identity_domain = "retained-bundle-id",
          .api_version = std::string(core::kRetainedBundleApiVersion),
          .own_field = "retained_bundle_id",
      });
  CanonicalValue root = retained_bundle_projection(
      retained_bundle_id, qualified, context, planning);
  std::string bytes = canonical_json(root);
  core::RetainedBundleDTO value = retained_bundle_value(
      retained_bundle_id, qualified, context, planning);
  auto bundle = detail::RetainedBundleConstructionAuthority::construct(
      core::RetainedBundleId(retained_bundle_id), std::move(value),
      std::move(root), std::move(bytes));
  if (!verify_retained_bundle_identity(*bundle)) {
    throw std::logic_error(
        "I3 materializer produced an invalid retained-bundle projection");
  }
  return bundle;
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize_store_and_reload_production(
    const RetentionQualifiedDeployment& qualified,
    IBuildManifestRepository& manifest_repository,
    IRawArtifactStore& artifact_store,
    IRetainedBundleRepository& bundle_repository) {
  const PreparedCanonicalEmptyProduct prepared =
      prepare_canonical_empty_product_before_store(qualified);
  const BuildQualifiedRetentionContext context =
      BuildQualifiedRetentionAuthority::qualify_persisted(
          manifest_repository);
  const auto bundle = materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      *prepared.contract, artifact_store);
  (void)bundle_repository.put_if_absent(bundle);
  const auto reloaded =
      bundle_repository.get_verified_envelope(
          bundle->retained_bundle_id());
  if (reloaded == nullptr ||
      reloaded->retained_bundle_id() != bundle->retained_bundle_id() ||
      reloaded->canonical_bytes() != bundle->canonical_bytes() ||
      !verify_retained_bundle_identity(*reloaded)) {
    throw std::runtime_error(
        "production retention did not independently reload the exact "
        "semantic bundle");
  }
  require_verified_current_bundle_artifacts(*reloaded, artifact_store);
  return reloaded;
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize(
    const RetentionQualifiedDeployment& qualified,
    const BuildQualifiedRetentionContext& context,
    IRawArtifactStore& artifact_store) {
  const PreparedCanonicalEmptyProduct prepared =
      prepare_canonical_empty_product_before_store(qualified);
  return materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      *prepared.contract, artifact_store);
}

}  // namespace manyforge::deployment

#include "manyforge_deployment/retained_bundle.hpp"
#include "manyforge_deployment/scene_pose_source_policy_codec.hpp"
#include <manyforge_deployment/io_provider_domain.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "build_manifest_internal.hpp"
#include "compiler_semantic_limits_internal.hpp"
#include "composed_model_symbol_role_map_internal.hpp"
#include "content_identity_internal.hpp"
#include "control_domain_control_internal.hpp"
#include "deployment_catalog_codec_internal.hpp"
#include "manyforge_core/runtime_contract_manifest_codec.hpp"
#include "manyforge_deployment/capability_definition_codec.hpp"
#include "manyforge_deployment/closed_schema.hpp"
#include "manyforge_deployment/command_schema_codec.hpp"
#include "manyforge_deployment/component_type_codec.hpp"
#include "manyforge_deployment/contact_policy_codec.hpp"
#include "manyforge_deployment/content_identity.hpp"
#include "manyforge_deployment/control_interface_contract_codec.hpp"
#include "manyforge_deployment/controller_topology_codec.hpp"
#include "manyforge_deployment/model_source.hpp"
#include "manyforge_deployment/provider_manifest_codec.hpp"
#include "manyforge_deployment/runtime_contract_registry.hpp"
#include "manyforge_deployment/runtime_profile_codec.hpp"
#include "manyforge_deployment/stop_evidence_policy_codec.hpp"
#include "planning_resource_map_internal.hpp"
#include "reference_runtime_unit_internal.hpp"
#include "retained_bundle_internal.hpp"
#include "ros2_control_execution_runtime_unit_internal.hpp"
#include "verified_retained_bundle_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr std::string_view kZeroDigest =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

[[nodiscard]] const BuildQualifiedProductContract &
require_flattened_model_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(
          std::string(detail::flattened_model_product_contract_id())),
      detail::flattened_model_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kPlanningCollisionModel ||
      contract->placement() != ProductPlacement::kCellPlanningCollisionModel) {
    throw std::logic_error(
        "build-qualified flattened-model product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] const BuildQualifiedProductContract &
require_viewer_model_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(
          std::string(detail::viewer_model_product_contract_id())),
      detail::viewer_model_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() != core::RetainedProductKind::kViewerModel ||
      contract->placement() != ProductPlacement::kCellViewerModel) {
    throw std::logic_error(
        "build-qualified viewer-model product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] const BuildQualifiedProductContract &
require_composed_model_symbol_role_map_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(std::string(
          detail::composed_model_symbol_role_map_product_contract_id())),
      detail::composed_model_symbol_role_map_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() != core::RetainedProductKind::kSymbolRoleMap ||
      contract->placement() != ProductPlacement::kSymbolRoleMap) {
    throw std::logic_error(
        "build-qualified composed-model symbol/role-map contract is absent "
        "or incoherent");
  }
  return *contract;
}

[[nodiscard]] const BuildQualifiedProductContract &
require_control_domain_control_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(
          std::string(detail::control_domain_control_product_contract_id())),
      detail::control_domain_control_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kControlDomainControl ||
      contract->placement() != ProductPlacement::kControlDomain) {
    throw std::logic_error(
        "build-qualified control-domain product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] const BuildQualifiedProductContract &
require_reference_runtime_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(
          std::string(detail::reference_runtime_product_contract_id())),
      detail::reference_runtime_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      contract->placement() != ProductPlacement::kRuntimeUnit) {
    throw std::logic_error(
        "build-qualified reference runtime product contract is absent or "
        "incoherent");
  }
  return *contract;
}

[[nodiscard]] const BuildQualifiedProductContract &
require_ros2_control_execution_runtime_contract() {
  const auto *contract = build_qualified_product_contract_registry().find(
      core::ProductContractId(std::string(
          detail::ros2_control_execution_runtime_product_contract_id())),
      detail::ros2_control_execution_runtime_product_contract_revision());
  if (contract == nullptr ||
      contract->product_kind() !=
          core::RetainedProductKind::kRuntimeUnitConfiguration ||
      contract->placement() != ProductPlacement::kRuntimeUnit) {
    throw std::logic_error(
        "build-qualified ros2_control execution runtime product contract is "
        "absent or incoherent");
  }
  return *contract;
}

[[nodiscard]] CanonicalValue node_environment_projection(
    const core::RetainedNodeEnvironmentDTO &environment) {
  return CanonicalValue(CanonicalValue::Object{
      {"catalog_id", CanonicalValue(environment.catalog_id)},
      {"content_version", CanonicalValue(environment.content_version)},
      {"catalog_digest", CanonicalValue(environment.catalog_digest.value())},
      {"environment_digest",
       CanonicalValue(environment.environment_digest.value())},
  });
}

[[nodiscard]] CanonicalValue
site_overlay_ref_projection(const core::SiteOverlayRefDTO &reference) {
  return CanonicalValue(CanonicalValue::Object{
      {"non_secret_digest",
       CanonicalValue(reference.non_secret_digest.value())},
      {"site_overlay_id", CanonicalValue(reference.site_overlay_id.value())},
      {"site_overlay_revision",
       CanonicalValue(reference.site_overlay_revision.value())},
  });
}

[[nodiscard]] CanonicalValue
records_projection(std::span<const core::CatalogRecordRefDTO> records) {
  const CanonicalValue catalogs = detail::encode_deployment_catalogs(records);
  const CanonicalValue *encoded_records =
      catalogs.find_object_member("records");
  if (encoded_records == nullptr) {
    throw std::logic_error(
        "deployment catalog codec omitted its required records member");
  }
  return *encoded_records;
}

[[nodiscard]] CanonicalValue schema_artifacts_projection(
    std::span<const core::RetainedSchemaArtifactRefDTO> schemas) {
  CanonicalValue::Array rows;
  rows.reserve(schemas.size());
  for (const auto &schema : schemas) {
    rows.emplace_back(CanonicalValue::Object{
        {"schema_id", CanonicalValue(schema.schema_id.value())},
        {"schema_revision", CanonicalValue(schema.schema_revision.value())},
        {"content_digest", CanonicalValue(schema.content_digest.value())},
        {"content_addressed_store_key",
         CanonicalValue(schema.content_addressed_store_key.value())},
    });
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue
catalogs_projection(std::span<const core::CatalogRecordRefDTO> records,
                    std::span<const core::RetainedSchemaArtifactRefDTO> schemas,
                    const core::RetainedNodeEnvironmentDTO &environment) {
  const auto &schema_registry = build_qualified_closed_schema_registry();
  return CanonicalValue(CanonicalValue::Object{
      {"dialect_api_version",
       CanonicalValue(std::string(kClosedSchemaApiVersion))},
      {"bootstrap_contract_digest",
       CanonicalValue(std::string(kClosedSchemaBootstrapContractDigest))},
      {"qualification_registry_digest",
       CanonicalValue(schema_registry.qualification_registry_digest().value())},
      {"records", records_projection(records)},
      {"schema_artifacts", schema_artifacts_projection(schemas)},
      {"node_environment", node_environment_projection(environment)},
  });
}

[[nodiscard]] CanonicalValue compiled_products_projection(
    const core::RetainedProductDTO &planning_collision_model,
    const core::RetainedProductDTO &viewer_model,
    std::span<const core::RetainedControlDomainProductsDTO>
        control_domain_products,
    std::span<const core::RetainedProductDTO> symbol_role_maps,
    std::span<const core::RetainedRuntimeUnitProductsDTO>
        runtime_unit_products) {
  CanonicalValue::Array control_products;
  control_products.reserve(control_domain_products.size());
  for (const auto &group : control_domain_products) {
    CanonicalValue::Array products;
    products.reserve(group.products.size());
    for (const auto &product : group.products) {
      products.push_back(detail::retained_product_projection(product));
    }
    control_products.emplace_back(CanonicalValue::Object{
        {"control_domain_id", CanonicalValue(group.control_domain_id.value())},
        {"products", CanonicalValue(std::move(products))},
    });
  }
  CanonicalValue::Array symbol_products;
  symbol_products.reserve(symbol_role_maps.size());
  for (const auto &product : symbol_role_maps) {
    symbol_products.push_back(detail::retained_product_projection(product));
  }
  CanonicalValue::Array runtime_products;
  runtime_products.reserve(runtime_unit_products.size());
  for (const auto &group : runtime_unit_products) {
    CanonicalValue::Array products;
    products.reserve(group.products.size());
    for (const auto &product : group.products) {
      products.push_back(detail::retained_product_projection(product));
    }
    runtime_products.emplace_back(CanonicalValue::Object{
        {"runtime_unit_id", CanonicalValue(group.runtime_unit_id.value())},
        {"products", CanonicalValue(std::move(products))},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"planning_collision_model",
       detail::retained_product_projection(planning_collision_model)},
      {"viewer_model", detail::retained_product_projection(viewer_model)},
      {"control_domain_products", CanonicalValue(std::move(control_products))},
      {"symbol_role_maps", CanonicalValue(std::move(symbol_products))},
      {"runtime_unit_products", CanonicalValue(std::move(runtime_products))},
  });
}

[[nodiscard]] CanonicalValue source_records_projection(
    std::span<const core::RetainedSourceRecordDTO> source_records) {
  CanonicalValue::Array rows;
  rows.reserve(source_records.size());
  for (const auto &source_record : source_records) {
    rows.emplace_back(std::visit(
        [](const auto &typed) {
          using Record = std::remove_cvref_t<decltype(typed)>;
          return CanonicalValue(CanonicalValue::Object{
              {"record_kind", CanonicalValue(std::string(
                                  core::to_string(Record::kRecordKind)))},
              {"record_id", CanonicalValue(typed.record_id.value())},
              {"revision", CanonicalValue(typed.revision.value())},
          });
        },
        source_record));
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue generator_records_projection(
    std::span<const core::RetainedGeneratorRecordDTO> generator_records) {
  CanonicalValue::Array rows;
  rows.reserve(generator_records.size());
  for (const auto &record : generator_records) {
    rows.emplace_back(CanonicalValue::Object{
        {"generator_id", CanonicalValue(record.generator_id.value())},
        {"generator_version", CanonicalValue(record.generator_version.value())},
        {"build_digest", CanonicalValue(record.build_digest.value())},
        {"configuration_digest",
         CanonicalValue(record.configuration_digest.value())},
    });
  }
  return CanonicalValue(std::move(rows));
}

[[nodiscard]] CanonicalValue provenance_projection(
    const detail::BuildQualifiedRetentionContext &context,
    std::span<const core::RetainedSourceRecordDTO> source_records,
    std::span<const core::RetainedGeneratorRecordDTO> generator_records,
    const core::RetainedProductDTO &planning_collision_model,
    const core::RetainedProductDTO &viewer_model,
    std::span<const core::RetainedControlDomainProductsDTO>
        control_domain_products,
    std::span<const core::RetainedProductDTO> symbol_role_maps,
    std::span<const core::RetainedRuntimeUnitProductsDTO>
        runtime_unit_products) {
  std::vector<const core::RetainedProductDTO *> products{
      &planning_collision_model, &viewer_model};
  for (const auto &group : control_domain_products) {
    for (const auto &product : group.products) {
      products.push_back(&product);
    }
  }
  for (const auto &product : symbol_role_maps) {
    products.push_back(&product);
  }
  for (const auto &group : runtime_unit_products) {
    for (const auto &product : group.products) {
      products.push_back(&product);
    }
  }
  std::sort(products.begin(), products.end(),
            [](const auto *left, const auto *right) {
              return left->product_id.value() < right->product_id.value();
            });
  CanonicalValue::Array outputs;
  outputs.reserve(products.size());
  for (const auto *product : products) {
    outputs.emplace_back(CanonicalValue::Object{
        {"product_id", CanonicalValue(product->product_id.value())},
        {"product_kind",
         CanonicalValue(std::string(core::to_string(product->product_kind)))},
        {"content_digest",
         CanonicalValue(
             detail::retained_product_output_digest(*product).value())},
    });
  }
  return CanonicalValue(CanonicalValue::Object{
      {"compiler_contract_digest",
       CanonicalValue(context.compiler_contract_digest().value())},
      {"compiler_build_digest",
       CanonicalValue(context.compiler_build_digest().value())},
      {"source_records", source_records_projection(source_records)},
      {"generator_records", generator_records_projection(generator_records)},
      {"output_records", CanonicalValue(std::move(outputs))},
  });
}

[[nodiscard]] core::RetainedBundleDTO retained_bundle_value(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment &qualified,
    const detail::BuildQualifiedRetentionContext &context,
    std::vector<core::RetainedSchemaArtifactRefDTO> schema_artifacts,
    std::vector<core::RetainedSourceRecordDTO> source_records,
    std::vector<core::RetainedGeneratorRecordDTO> generator_records,
    const core::RetainedProductDTO &planning_collision_model,
    const core::RetainedProductDTO &viewer_model,
    std::vector<core::RetainedControlDomainProductsDTO> control_domain_products,
    std::vector<core::RetainedProductDTO> symbol_role_maps,
    std::vector<core::RetainedRuntimeUnitProductsDTO> runtime_unit_products,
    std::vector<core::RetainedRuntimeContractRequirementDTO>
        runtime_contract_requirements) {
  const auto &deployment = qualified.deployment();
  const auto &registry = build_qualified_closed_schema_registry();
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
          std::move(schema_artifacts),
          context.node_environment(),
      },
      deployment.applicability_inputs().value().desired_control_configurations,
      deployment.applicability_inputs().value().capability_provider_bindings,
      deployment.applicability_inputs().value().retained_policy_records,
      core::RetainedCompiledProductsDTO{
          planning_collision_model,
          viewer_model,
          control_domain_products,
          symbol_role_maps,
          runtime_unit_products,
      },
      core::RetainedBundleProvenanceDTO{
          context.compiler_contract_digest(),
          context.compiler_build_digest(),
          std::move(source_records),
          std::move(generator_records),
          [&]() {
            std::vector<core::RetainedOutputRecordDTO> outputs;
            std::size_t control_product_count = 0U;
            for (const auto &group : control_domain_products) {
              control_product_count += group.products.size();
            }
            outputs.reserve(2U + control_product_count +
                            symbol_role_maps.size() +
                            runtime_unit_products.size());
            outputs.push_back(core::RetainedOutputRecordDTO{
                planning_collision_model.product_id,
                planning_collision_model.product_kind,
                detail::retained_product_output_digest(
                    planning_collision_model)});
            outputs.push_back(core::RetainedOutputRecordDTO{
                viewer_model.product_id, viewer_model.product_kind,
                detail::retained_product_output_digest(viewer_model)});
            for (const auto &group : control_domain_products) {
              for (const auto &product : group.products) {
                outputs.push_back(core::RetainedOutputRecordDTO{
                    product.product_id, product.product_kind,
                    detail::retained_product_output_digest(product)});
              }
            }
            for (const auto &product : symbol_role_maps) {
              outputs.push_back(core::RetainedOutputRecordDTO{
                  product.product_id, product.product_kind,
                  detail::retained_product_output_digest(product)});
            }
            for (const auto &group : runtime_unit_products) {
              for (const auto &product : group.products) {
                outputs.push_back(core::RetainedOutputRecordDTO{
                    product.product_id, product.product_kind,
                    detail::retained_product_output_digest(product)});
              }
            }
            std::sort(outputs.begin(), outputs.end(),
                      [](const auto &left, const auto &right) {
                        return left.product_id.value() <
                               right.product_id.value();
                      });
            return outputs;
          }(),
      },
      qualified.compiler_ir().applicability().selected_site_overlay_ref(),
      deployment.applicability_inputs().value().external_resources,
      qualified.compiler_ir().applicability().assistant_settings().enabled()
          ? std::optional<core::RetainedAssistantProjectionDTO>(
                qualified.compiler_ir()
                    .applicability()
                    .assistant_settings()
                    .retained_projection())
          : std::nullopt,
      std::move(runtime_contract_requirements),
      deployment.applicability_inputs()
          .value()
          .motion_takeover_preparation_budget_s,
      deployment.applicability_inputs()
          .value()
          .motion_takeover_planning_budget_s,
      deployment.applicability_inputs().value().scene_pose_sources,
  };
}

[[nodiscard]] CanonicalValue retained_bundle_projection(
    std::string_view retained_bundle_id,
    const RetentionQualifiedDeployment &qualified,
    const detail::BuildQualifiedRetentionContext &context,
    std::span<const core::RetainedSchemaArtifactRefDTO> schema_artifacts,
    std::span<const core::RetainedSourceRecordDTO> source_records,
    std::span<const core::RetainedGeneratorRecordDTO> generator_records,
    const core::RetainedProductDTO &planning_collision_model,
    const core::RetainedProductDTO &viewer_model,
    std::span<const core::RetainedControlDomainProductsDTO>
        control_domain_products,
    std::span<const core::RetainedProductDTO> symbol_role_maps,
    std::span<const core::RetainedRuntimeUnitProductsDTO> runtime_unit_products,
    std::span<const core::RetainedRuntimeContractRequirementDTO>
        runtime_contract_requirements) {
  const auto &deployment = qualified.deployment();
  CanonicalValue::Object root{
      {"api_version",
       CanonicalValue(std::string(core::kRetainedBundleApiVersion))},
      {"retained_bundle_id", CanonicalValue(std::string(retained_bundle_id))},
      {"deployment_artifact_id",
       CanonicalValue(deployment.deployment_artifact_id().value())},
      {"deployment_revision",
       CanonicalValue(deployment.deployment_revision().value())},
      {"cell_graph", deployment.graph().normalized_root()},
      {"catalogs",
       catalogs_projection(deployment.catalog_records(), schema_artifacts,
                           context.node_environment())},
      {"desired_control_configurations",
       deployment.applicability_inputs()
           .normalized_desired_control_configurations()},
      {"capability_provider_bindings",
       deployment.applicability_inputs()
           .normalized_capability_provider_bindings()},
      {"retained_policy_records",
       *detail::encode_deployment_catalogs(
            deployment.applicability_inputs().value().retained_policy_records)
            .find_object_member("records")},
      {"runtime_contract_requirements",
       [&]() {
         CanonicalValue::Array rows;
         rows.reserve(runtime_contract_requirements.size());
         for (const auto &row : runtime_contract_requirements) {
           const auto encoded =
               core::EncodeRuntimeContractRequirement(row.requirement);
           if (!encoded.ok()) {
             throw std::logic_error(
                 "runtime requirement encoding failed during retention");
           }
           rows.emplace_back(CanonicalValue::Object{
               {"runtime_unit_id", CanonicalValue(row.runtime_unit_id.value())},
               {"requirement",
                parse_strict_json_model_yaml(encoded.value()).root()},
           });
         }
         return CanonicalValue(std::move(rows));
       }()},
      {"compiled_products",
       compiled_products_projection(planning_collision_model, viewer_model,
                                    control_domain_products, symbol_role_maps,
                                    runtime_unit_products)},
      {"provenance",
       provenance_projection(context, source_records, generator_records,
                             planning_collision_model, viewer_model,
                             control_domain_products, symbol_role_maps,
                             runtime_unit_products)},
  };
  // Preserve empty-policy bundle bytes. Non-empty declarations travel only
  // from the revision-verified deployment projection, never a runtime request.
  if (const auto &policies =
          deployment.applicability_inputs().normalized_policies();
      policies.has_value()) {
    // The codec emits interrupt_rules only when nonempty.
    if (const auto *rules = policies->find_object_member("interrupt_rules"))
      root.emplace_back("interrupt_policy_configuration",
                        CanonicalValue(CanonicalValue::Object{
                            {"interrupt_rules", *rules}}));
  }
  const auto &applicability = qualified.compiler_ir().applicability();
  if (applicability.selected_site_overlay_ref().has_value()) {
    root.emplace_back("site_overlay_ref",
                      site_overlay_ref_projection(
                          *applicability.selected_site_overlay_ref()));
  }
  if (!qualified.deployment()
           .applicability_inputs()
           .value()
           .external_resources.empty()) {
    root.emplace_back("external_resources",
                      qualified.deployment()
                          .applicability_inputs()
                          .normalized_external_resources());
  }
  if (applicability.assistant_settings().enabled()) {
    root.emplace_back(
        "assistant",
        applicability.assistant_settings().normalized_projection());
  }
  if (const auto budget = deployment.applicability_inputs()
                              .value()
                              .motion_takeover_preparation_budget_s;
      budget.has_value()) {
    root.emplace_back("motion_takeover_preparation_budget_s",
                      CanonicalValue(*budget));
  }
  if (const auto budget = deployment.applicability_inputs()
                              .value()
                              .motion_takeover_planning_budget_s;
      budget.has_value()) {
    root.emplace_back("motion_takeover_planning_budget_s",
                      CanonicalValue(*budget));
  }
  if (const auto &sources =
          deployment.applicability_inputs().value().scene_pose_sources;
      !sources.empty())
    root.emplace_back("scene_pose_sources", EncodeScenePoseSources(sources));
  // Stream parameter overrides travel only from the revision-verified
  // deployment projection; the normalized policies omit an empty table.
  if (const auto &policies =
          deployment.applicability_inputs().normalized_policies();
      policies.has_value()) {
    if (const auto *streams = policies->find_object_member("component_streams"))
      root.emplace_back("component_streams", *streams);
  }
  return CanonicalValue(std::move(root));
}

struct PreparedCatalogArtifact final {
  core::ContentAddressedStoreKey key;
  core::ContentDigest digest;
  std::uint64_t byte_size;
};

struct PreparedSourceArtifact final {
  core::ContentAddressedStoreKey key;
  core::ContentDigest digest;
  std::uint64_t byte_size;
};

struct PreparedSchemaArtifact final {
  core::CatalogRecordKind record_kind;
  core::RetainedSchemaArtifactRefDTO reference;
};

struct PreparedPlanningCollisionProduct final {
  std::string urdf;
  std::string srdf;
  std::string planning_resource_map;
  const BuildQualifiedProductContract *contract;
  const BuildQualifiedProductContract *viewer_contract;
  std::optional<std::string> symbol_role_map;
  const BuildQualifiedProductContract *symbol_role_map_contract;
  std::vector<detail::PreparedControlDomainArtifact> control_domain_artifacts;
  const BuildQualifiedProductContract *control_domain_contract;
  std::vector<detail::PreparedRuntimeUnitArtifact> runtime_unit_artifacts;
  std::vector<core::RetainedSchemaArtifactRefDTO> schema_artifacts;
  std::vector<core::CatalogRecordKind> schema_artifact_kinds;
  std::vector<core::RetainedSourceRecordDTO> source_records;
  std::vector<core::RetainedGeneratorRecordDTO> generator_records;
  std::vector<PreparedCatalogArtifact> catalog_artifacts;
  std::vector<PreparedSourceArtifact> source_artifacts;
};

struct PreparedControlArtifacts final {
  std::vector<detail::RetainedArtifactInput> conditional_artifacts;
};

[[nodiscard]] core::RetainedArtifactRefDTO
retained_artifact_ref(std::string_view role, std::string_view media_type,
                      std::string_view bytes) {
  if (bytes.empty() || bytes.size() > detail::kMaximumIJsonInteger) {
    detail::fail_retained_bundle(
        "conditional control-domain artifact is empty or over-limit");
  }
  const core::ContentDigest digest(sha256_digest(bytes));
  return core::RetainedArtifactRefDTO{
      core::ArtifactRoleId(std::string(role)), std::string(media_type),
      static_cast<std::uint64_t>(bytes.size()), digest,
      core::ContentAddressedStoreKey(
          detail::retained_store_key_for_digest(digest.value()))};
}

[[nodiscard]] PreparedControlArtifacts prepare_control_artifacts(
    const RetentionQualifiedDeployment &qualified,
    const core::ControlDomainDTO &domain,
    const detail::ICompilerInputArtifactSource &input_source) {
  std::optional<core::RetainedArtifactRefDTO> expected;
  std::optional<std::string> bytes;
  for (const auto &stop_value :
       qualified.compiler_ir().applicability().stop_applicability()) {
    const auto *stop =
        std::get_if<LogicalStoppableApplicabilityTemplate>(&stop_value);
    if (stop == nullptr ||
        stop->control_domain_id != domain.control_domain_id) {
      continue;
    }
    for (const auto &stage : stop->escalation_stages) {
      if (!stage.commissioned_stop_interface.has_value()) {
        continue;
      }
      const auto &candidate = stage.commissioned_stop_interface
                                  ->commissioning_evidence_artifact_ref;
      if (expected.has_value() && *expected != candidate) {
        detail::fail_retained_bundle("one control domain selects more than one "
                                     "commissioning evidence package");
      }
      const auto loaded = input_source.get_verified(
          candidate.content_addressed_store_key, candidate.content_digest,
          candidate.byte_size);
      if (!loaded.has_value() || loaded->empty() ||
          static_cast<std::uint64_t>(loaded->size()) != candidate.byte_size ||
          sha256_digest(*loaded) != candidate.content_digest.value()) {
        detail::fail_retained_bundle("commissioned Stop evidence disappeared "
                                     "before retention preflight");
      }
      expected = candidate;
      bytes = *loaded;
    }
  }
  PreparedControlArtifacts result;
  if (bytes.has_value()) {
    const auto exact = retained_artifact_ref(
        core::kStopInterfaceCommissioningArtifactRole,
        core::kStopInterfaceCommissioningMediaType, *bytes);
    if (!expected.has_value() || exact != *expected) {
      detail::fail_retained_bundle("commissioning evidence reference differs "
                                   "from the exact retained bytes");
    }
    result.conditional_artifacts.push_back(detail::RetainedArtifactInput{
        exact.artifact_role_id, exact.media_type, std::move(*bytes)});
  }
  return result;
}

template <typename Qualified>
void require_exact_requalified_catalog_record(
    const Qualified &requalified,
    const std::vector<Qualified> &compiler_qualified, std::string_view label) {
  const Qualified *match = nullptr;
  for (const auto &selected : compiler_qualified) {
    if (selected.reference() != requalified.reference()) {
      continue;
    }
    if (match != nullptr) {
      detail::fail_retained_bundle(
          std::string(label) +
          " appears more than once in the compiler-qualified closure");
    }
    match = &selected;
  }
  if (match == nullptr || match->value() != requalified.value() ||
      match->normalized_root() != requalified.normalized_root()) {
    detail::fail_retained_bundle(
        std::string(label) +
        " differs from the exact compiler-qualified catalog referent");
  }
}

[[nodiscard]] std::vector<QualifiedCapabilityDefinition>
select_component_capability_dependencies(
    const QualifiedComponentType &component,
    std::span<const QualifiedCapabilityDefinition> deployment_capabilities) {
  std::vector<QualifiedCapabilityDefinition> result;
  for (const auto &capability : deployment_capabilities) {
    const bool selected = std::any_of(
        component.value().command_targets.begin(),
        component.value().command_targets.end(), [&](const auto &target) {
          return std::any_of(
              target.capabilities.begin(), target.capabilities.end(),
              [&](const auto &dependency) {
                return dependency.capability_id ==
                           capability.value().capability_id &&
                       dependency.capability_definition_revision ==
                           capability.value().capability_definition_revision;
              });
        });
    if (selected) {
      result.push_back(capability);
    }
  }
  return result;
}

template <typename Record>
[[nodiscard]] core::RetainedSourceRecordDTO
retained_source_record(const Record &record) {
  using Source =
      core::TypedRetainedSourceRecordDTO<Record::kRecordKind,
                                         typename Record::IdType,
                                         typename Record::RevisionType>;
  return Source{record.record_id, record.record_revision};
}

[[nodiscard]] std::string
require_verified_input_bytes(const detail::ICompilerInputArtifactSource &source,
                             const core::ContentAddressedStoreKey &key,
                             const core::ContentDigest &digest,
                             std::optional<std::uint64_t> expected_size,
                             std::string_view label) {
  const auto bytes = source.get_verified(key, digest, expected_size);
  if (!bytes.has_value() ||
      (expected_size.has_value() &&
       static_cast<std::uint64_t>(bytes->size()) != *expected_size) ||
      sha256_digest(*bytes) != digest.value()) {
    detail::fail_retained_bundle(
        std::string(label) +
        " could not be independently resolved by exact key and digest");
  }
  return *bytes;
}

void add_prepared_catalog_artifact(
    std::map<std::string, PreparedCatalogArtifact, std::less<>> &inputs,
    core::ContentAddressedStoreKey key, core::ContentDigest digest,
    std::uint64_t byte_size, std::string_view label) {
  const std::string exact_key = key.value();
  const auto found = inputs.find(exact_key);
  if (found != inputs.end()) {
    if (found->second.digest != digest ||
        found->second.byte_size != byte_size) {
      detail::fail_retained_bundle(
          std::string(label) +
          " collides with unequal content under one opaque catalog key");
    }
    return;
  }
  inputs.emplace(exact_key, PreparedCatalogArtifact{
                                std::move(key), std::move(digest), byte_size});
}

void add_prepared_source_artifact(
    std::map<std::string, PreparedSourceArtifact, std::less<>> &inputs,
    core::ContentAddressedStoreKey key, core::ContentDigest digest,
    std::uint64_t byte_size, std::string_view label) {
  const std::string exact_key = key.value();
  if (exact_key != detail::retained_store_key_for_digest(digest.value())) {
    detail::fail_retained_bundle(
        std::string(label) +
        " does not use the retained package's digest-derived CAS key");
  }
  const auto found = inputs.find(exact_key);
  if (found != inputs.end()) {
    if (found->second.digest != digest ||
        found->second.byte_size != byte_size) {
      detail::fail_retained_bundle(
          std::string(label) +
          " collides with unequal metadata under one retained CAS key");
    }
    return;
  }
  inputs.emplace(exact_key, PreparedSourceArtifact{
                                std::move(key), std::move(digest), byte_size});
}

void require_qualified_structural_subset_before_store(
    const RetentionQualifiedDeployment &qualified) {
  const auto &deployment = qualified.deployment();
  const auto &graph = deployment.graph().graph();
  const auto &inputs = deployment.applicability_inputs().value();
  const auto &compiler = qualified.compiler_ir();

  const auto &applicability = compiler.applicability();
  const auto &composition = applicability.composition();
  std::set<std::pair<core::RuntimeProfileId, core::RuntimeProfileRevision>>
      graph_runtime_profiles;
  for (const auto &unit : graph.runtime_units) {
    graph_runtime_profiles.emplace(unit.runtime_profile_id,
                                   unit.runtime_profile_revision);
  }
  std::set<std::pair<core::RuntimeProfileId, core::RuntimeProfileRevision>>
      qualified_runtime_profiles;
  for (const auto &profile : applicability.runtime_profiles()) {
    qualified_runtime_profiles.emplace(
        profile.value().runtime_profile_id,
        profile.value().runtime_profile_revision);
  }
  if (qualified_runtime_profiles != graph_runtime_profiles ||
      applicability.external_resources().size() !=
          inputs.external_resources.size()) {
    detail::fail_retained_bundle(
        "compiler IR contains an incoherent runtime-profile or external "
        "provider semantic input");
  }

  const auto provider_domains = ValidateIoProviderDomains(
      graph, inputs.capability_provider_bindings,
      applicability.runtime_profiles(), applicability.provider_manifests());
  if (!provider_domains.ok())
    detail::fail_retained_bundle(provider_domains.error().details.at("reason"));
  const auto controller_domain_count = static_cast<std::size_t>(std::count_if(
      graph.control_domains.begin(), graph.control_domains.end(),
      [](const auto &domain) { return domain.provider() == nullptr; }));
  const bool provider_only_subset =
      graph.model_instances.empty() && !graph.controllable_components.empty() &&
      !graph.control_domains.empty() && controller_domain_count == 0U &&
      graph.control_bindings.empty() &&
      inputs.desired_control_configurations.empty() &&
      std::all_of(
          graph.controllable_components.begin(),
          graph.controllable_components.end(), [&](const auto &component) {
            return component.model_role_bindings.empty() &&
                   std::any_of(
                       graph.control_domains.begin(),
                       graph.control_domains.end(), [&](const auto &domain) {
                         return domain.provider()->controllable_component_id ==
                                component.controllable_component_id;
                       });
          });
  const bool passive_subset =
      graph.runtime_units.empty() && graph.controllable_components.empty() &&
      graph.control_domains.empty() && graph.control_bindings.empty() &&
      inputs.desired_control_configurations.empty();
  const bool controllable_subset =
      !graph.model_instances.empty() &&
      !graph.controllable_components.empty() && controller_domain_count != 0U &&
      !graph.control_bindings.empty() &&
      inputs.desired_control_configurations.size() == controller_domain_count;
  const bool infrastructure_runtime_subset =
      graph.runtime_units.size() == 1U && graph.model_instances.empty() &&
      graph.controllable_components.empty() && graph.control_domains.empty() &&
      graph.control_bindings.empty() &&
      inputs.desired_control_configurations.empty();
  if (!passive_subset && !controllable_subset && !provider_only_subset &&
      !infrastructure_runtime_subset) {
    detail::fail_retained_bundle(
        "retention accepts passive models, a bounded controllable "
        "cell, or one exact infrastructure runtime unit");
  }

  if (passive_subset) {
    if (std::any_of(graph.model_instances.begin(), graph.model_instances.end(),
                    [](const core::ModelInstanceDTO &instance) {
                      return !instance.root_pose_in_cell.has_value();
                    }) ||
        !composition.component_types().empty() ||
        !composition.control_binding_coordinate_maps().empty() ||
        !composition.targets().empty() ||
        !applicability.capability_definitions().empty() ||
        !applicability.control_interface_contracts().empty() ||
        !applicability.controller_topologies().empty() ||
        !applicability.provider_manifests().empty() ||
        !applicability.stop_evidence_policies().empty() ||
        !applicability.binding_interfaces().empty() ||
        !applicability.controllers().empty() ||
        !applicability.routes().empty() ||
        !applicability.capabilities().empty() ||
        !applicability.stop_applicability().empty()) {
      detail::fail_retained_bundle(
          "passive structural retention contains component/control "
          "semantics or an unplaced model");
    }
  } else if (controllable_subset || provider_only_subset) {
    const bool has_runtime_units = !graph.runtime_units.empty();
    const bool exact_execution_runtime_units = std::all_of(
        graph.runtime_units.begin(), graph.runtime_units.end(),
        [](const core::RuntimeUnitDTO &unit) {
          const bool ros2_control_execution =
              unit.runtime_profile_id.value() ==
                  detail::kRos2ControlExecutionRuntimeProfileId &&
              unit.runtime_profile_revision.value() ==
                  detail::ros2_control_execution_runtime_profile_revision() &&
              detail::ros2_control_execution_runtime_configuration_is_qualified(
                  unit.configuration);
          return ros2_control_execution &&
                 unit.dependency_runtime_unit_ids.empty();
        });
    if (!has_runtime_units || !exact_execution_runtime_units ||
        composition.component_types().empty() ||
        composition.control_binding_coordinate_maps().size() !=
            graph.control_bindings.size() ||
        composition.targets().empty() ||
        applicability.binding_interfaces().size() !=
            graph.control_bindings.size() ||
        (controller_domain_count != 0U &&
         (applicability.controllers().empty() ||
          applicability.routes().empty())) ||
        (controller_domain_count == 0U &&
         (!applicability.controllers().empty() ||
          !applicability.routes().empty() ||
          !applicability.control_interface_contracts().empty() ||
          !applicability.controller_topologies().empty() ||
          !applicability.stop_evidence_policies().empty())) ||
        applicability.capabilities().empty() ||
        applicability.stop_applicability().size() !=
            applicability.routes().size()) {
      detail::fail_retained_bundle(
          "controllable retention is outside its bounded qualified "
          "component/target/domain subset");
    }

    {
      std::set<core::RuntimeUnitId> declared;
      for (const auto &unit : graph.runtime_units) {
        declared.insert(unit.runtime_unit_id);
      }
      for (const auto &component : graph.controllable_components) {
        if (std::any_of(component.runtime_unit_ids.begin(),
                        component.runtime_unit_ids.end(),
                        [&](const auto &runtime_unit_id) {
                          return !declared.contains(runtime_unit_id);
                        })) {
          detail::fail_retained_bundle(
              "native component selects an undeclared execution runtime");
        }
      }
      for (const auto &domain : graph.control_domains) {
        if (std::any_of(domain.runtime_unit_ids.begin(),
                        domain.runtime_unit_ids.end(),
                        [&](const auto &runtime_unit_id) {
                          return !declared.contains(runtime_unit_id);
                        })) {
          detail::fail_retained_bundle(
              "native control domain selects an undeclared execution "
              "runtime");
        }
      }
      for (const auto &binding : graph.control_bindings) {
        if (!declared.contains(binding.provider_runtime_unit_id)) {
          detail::fail_retained_bundle(
              "native control binding selects an undeclared provider "
              "runtime");
        }
      }
    }

    for (const auto &binding : graph.control_bindings) {
      const auto coordinate_maps = std::count_if(
          composition.control_binding_coordinate_maps().begin(),
          composition.control_binding_coordinate_maps().end(),
          [&binding](const CompositionControlBindingCoordinateMap &map) {
            return map.control_binding_id == binding.control_binding_id &&
                   map.controllable_component_id ==
                       binding.controllable_component_id &&
                   map.command_target_id == binding.command_target_id &&
                   map.resources.size() == binding.resource_bindings.size();
          });
      const auto targets = std::count_if(
          composition.targets().begin(), composition.targets().end(),
          [&binding](const CompositionTargetResolution &target) {
            return target.controllable_component_id ==
                       binding.controllable_component_id &&
                   target.command_target_id == binding.command_target_id;
          });
      const auto interface_templates = std::count_if(
          applicability.binding_interfaces().begin(),
          applicability.binding_interfaces().end(),
          [&binding](const LogicalBindingInterfaceTemplate &value) {
            return value.control_binding_id == binding.control_binding_id &&
                   value.control_binding_revision ==
                       binding.control_binding_revision;
          });
      if (binding.resource_bindings.empty() ||
          binding.legal_control_route_ids.empty() ||
          binding.route_endpoint_projections.size() !=
              binding.legal_control_route_ids.size() ||
          coordinate_maps != 1 || targets != 1 || interface_templates != 1) {
        detail::fail_retained_bundle(
            "native controllable retention lacks one exact qualified "
            "coordinate, target, interface, or route projection for a "
            "control binding");
      }
    }
  } else {
    const auto &unit = graph.runtime_units.front();
    const bool exact_reference =
        unit.runtime_unit_id.value() == detail::kReferenceRuntimeUnitId &&
        unit.runtime_profile_id.value() == detail::kReferenceRuntimeProfileId &&
        unit.runtime_profile_revision.value() ==
            detail::reference_runtime_profile_revision() &&
        unit.configuration.empty();
    const bool exact_ros2_control_host =
        unit.runtime_profile_id.value() ==
            detail::kRos2ControlExecutionRuntimeProfileId &&
        unit.runtime_profile_revision.value() ==
            detail::ros2_control_execution_runtime_profile_revision() &&
        detail::ros2_control_execution_runtime_configuration_is_qualified(
            unit.configuration);
    if ((!exact_reference && !exact_ros2_control_host) ||
        !unit.dependency_runtime_unit_ids.empty() ||
        applicability.runtime_profiles().size() != 1U ||
        !composition.component_types().empty() ||
        !composition.control_binding_coordinate_maps().empty() ||
        !composition.targets().empty() ||
        !applicability.capability_definitions().empty() ||
        !applicability.control_interface_contracts().empty() ||
        !applicability.controller_topologies().empty() ||
        !applicability.provider_manifests().empty() ||
        !applicability.stop_evidence_policies().empty() ||
        !applicability.binding_interfaces().empty() ||
        !applicability.controllers().empty() ||
        !applicability.routes().empty() ||
        !applicability.capabilities().empty() ||
        !applicability.stop_applicability().empty()) {
      detail::fail_retained_bundle(
          "runtime retention is outside an exact infrastructure process row");
    }
    if (exact_reference) {
      static_cast<void>(detail::emit_reference_runtime_manifest(
          qualified.compiler_ir(), unit.runtime_unit_id));
    } else {
      static_cast<void>(detail::emit_ros2_control_execution_runtime_manifest(
          qualified.compiler_ir(), unit.runtime_unit_id));
    }
  }

  const auto &cell_frame = composition.cell_frame();
  const auto &urdf = composition.flattened_urdf();
  const auto &srdf = composition.flattened_srdf();
  const auto &frame_paths = composition.frame_paths();
  const std::string_view incoherent_reason = [&]() -> std::string_view {
    if (cell_frame.cell_id != graph.cell_id) {
      return "cell identity differs";
    }
    if (urdf.robot_name.empty()) {
      return "flattened URDF has no robot name";
    }
    if (urdf.links.empty()) {
      return "flattened URDF has no cell frame";
    }
    if (urdf.links.front().name != cell_frame.compiled_frame_name) {
      return "flattened URDF does not begin with the cell frame";
    }
    if (srdf.robot_name != urdf.robot_name) {
      return "flattened URDF and SRDF identities differ";
    }
    if (frame_paths.empty()) {
      return "flattened model has no frame paths";
    }
    if (composition.model_occurrences().size() !=
        graph.model_instances.size()) {
      return "model occurrence count differs from the graph";
    }
    return {};
  }();
  if (!incoherent_reason.empty()) {
    detail::fail_retained_bundle(
        "structural compiler IR does not contain a coherent flattened "
        "planning/collision model: " +
        std::string(incoherent_reason));
  }
  if (graph.model_instances.empty()) {
    const auto *path_cell_frame = frame_paths.size() == 1U
                                      ? std::get_if<CompositionCellFrame>(
                                            &frame_paths.front().terminal_frame)
                                      : nullptr;
    if (!urdf.materials.empty() || urdf.links.size() != 1U ||
        !urdf.joints.empty() || urdf.links.front().inertial.has_value() ||
        !urdf.links.front().visuals.empty() ||
        !urdf.links.front().collisions.empty() || !srdf.groups.empty() ||
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
}

[[nodiscard]] PreparedPlanningCollisionProduct
prepare_planning_collision_product_before_store(
    const RetentionQualifiedDeployment &qualified,
    const detail::ICompilerInputArtifactSource &input_source,
    std::span<const BuildQualifiedStructuralModelPreparation>
        structural_model_preparations,
    const bool retain_empty_model_map) {
  require_qualified_structural_subset_before_store(qualified);
  const auto &composition =
      qualified.compiler_ir().applicability().composition();
  detail::require_pinocchio_family_mimic_profile(composition.flattened_urdf());

  std::map<std::pair<std::string, std::string>,
           const SourceVerifiedModelAsset *>
      assets;
  for (const auto &asset : composition.model_assets()) {
    const auto key = std::make_pair(asset.value().model_asset_id.value(),
                                    asset.value().model_revision.value());
    if (!assets.emplace(key, &asset).second) {
      detail::fail_retained_bundle(
          "model-only compiler IR contains a duplicate qualified model asset");
    }
  }
  for (const auto &instance : composition.graph().model_instances) {
    if (!assets.contains({instance.model_asset_id.value(),
                          instance.model_revision.value()})) {
      detail::fail_retained_bundle(
          "model-only graph selects a model asset absent from the exact "
          "qualified source closure");
    }
  }

  if (structural_model_preparations.size() != assets.size()) {
    detail::fail_retained_bundle(
        "the current structural retention path requires exactly one "
        "build-qualified preparation per unique model asset");
  }
  std::set<std::pair<std::string, std::string>> prepared_asset_identities;
  std::vector<core::RetainedGeneratorRecordDTO> generator_records;
  generator_records.reserve(structural_model_preparations.size());
  for (const auto &preparation : structural_model_preparations) {
    const auto &prepared = preparation.verified_model_asset();
    const auto identity =
        std::make_pair(prepared.value().model_asset_id.value(),
                       prepared.value().model_revision.value());
    const auto asset = assets.find(identity);
    if (asset == assets.end() ||
        !prepared_asset_identities.insert(identity).second ||
        prepared.qualified_asset().reference() !=
            asset->second->qualified_asset().reference() ||
        prepared.qualified_asset().value() !=
            asset->second->qualified_asset().value() ||
        prepared.qualified_asset().normalized_root() !=
            asset->second->qualified_asset().normalized_root() ||
        prepared.verified_artifacts() != asset->second->verified_artifacts() ||
        !(prepared.source_ir() == asset->second->source_ir())) {
      detail::fail_retained_bundle(
          "structural preparation provenance does not match exactly one "
          "compiler-qualified model asset");
    }
    generator_records.push_back(preparation.generator_record());
  }
  std::sort(generator_records.begin(), generator_records.end(),
            [](const core::RetainedGeneratorRecordDTO &left,
               const core::RetainedGeneratorRecordDTO &right) {
              return std::tie(left.generator_id.value(),
                              left.generator_version.value(),
                              left.build_digest.value(),
                              left.configuration_digest.value()) <
                     std::tie(right.generator_id.value(),
                              right.generator_version.value(),
                              right.build_digest.value(),
                              right.configuration_digest.value());
            });
  generator_records.erase(
      std::unique(generator_records.begin(), generator_records.end()),
      generator_records.end());

  std::map<std::string, PreparedCatalogArtifact, std::less<>> prepared_catalogs;
  std::map<std::string, PreparedSourceArtifact, std::less<>> prepared_sources;
  std::map<std::pair<std::string, std::string>, PreparedSchemaArtifact> schemas;
  std::vector<core::RetainedSourceRecordDTO> source_records;
  source_records.reserve(qualified.compiler_ir().catalog_record_refs().size());

  std::set<std::pair<std::string, std::string>> referenced_assets;
  for (const auto &record : qualified.compiler_ir().catalog_record_refs()) {
    std::visit(
        [&](const auto &typed_reference) {
          using Record = std::remove_cvref_t<decltype(typed_reference)>;
          const std::string catalog_bytes = require_verified_input_bytes(
              input_source, typed_reference.content_addressed_store_key,
              typed_reference.content_digest, std::nullopt,
              "structural catalog referent");
          add_prepared_catalog_artifact(
              prepared_catalogs, typed_reference.content_addressed_store_key,
              typed_reference.content_digest,
              static_cast<std::uint64_t>(catalog_bytes.size()),
              "structural catalog referent");

          if constexpr (std::is_same_v<Record,
                                       core::ModelAssetCatalogRecordRefDTO>) {
            const auto asset_key =
                std::make_pair(typed_reference.record_id.value(),
                               typed_reference.record_revision.value());
            const auto asset = assets.find(asset_key);
            if (asset == assets.end() ||
                asset->second->qualified_asset().reference() !=
                    typed_reference ||
                !referenced_assets.insert(asset_key).second) {
              detail::fail_retained_bundle(
                  "retained model-asset reference does not match exactly "
                  "one compiler-qualified source asset");
            }
            const QualifiedModelAsset requalified =
                resolve_and_qualify_model_asset(
                    typed_reference,
                    typed_reference.content_addressed_store_key, catalog_bytes);
            if (requalified.value() !=
                    asset->second->qualified_asset().value() ||
                requalified.normalized_root() !=
                    asset->second->qualified_asset().normalized_root()) {
              detail::fail_retained_bundle(
                  "re-resolved model-asset referent differs from the "
                  "compiler-qualified source");
            }

            std::vector<ResolvedModelSourceArtifactCandidate> source_candidates;
            source_candidates.reserve(
                asset->second->verified_artifacts().size());
            for (const auto &artifact : asset->second->verified_artifacts()) {
              std::string bytes = require_verified_input_bytes(
                  input_source, artifact.resolved_store_key,
                  artifact.content_digest, artifact.byte_size,
                  "model-source artifact");
              source_candidates.push_back(
                  {artifact.logical_uri, artifact.resolved_store_key, bytes});
              add_prepared_source_artifact(
                  prepared_sources, artifact.resolved_store_key,
                  artifact.content_digest,
                  static_cast<std::uint64_t>(bytes.size()),
                  "model-source artifact");
            }
            const SourceVerifiedModelAsset reverified =
                verify_model_source_closure(requalified,
                                            std::move(source_candidates));
            if (reverified.verified_artifacts() !=
                    asset->second->verified_artifacts() ||
                !(reverified.source_ir() == asset->second->source_ir())) {
              detail::fail_retained_bundle(
                  "re-resolved model-source closure differs from the "
                  "compiler-qualified source IR");
            }
          } else if constexpr (std::is_same_v<
                                   Record,
                                   core::RuntimeProfileCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_runtime_profile(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir().applicability().runtime_profiles(),
                "runtime profile");
          } else if constexpr (
              std::is_same_v<Record,
                             core::CapabilityDefinitionCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_capability_definition(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir()
                    .applicability()
                    .capability_definitions(),
                "capability definition");
          } else if constexpr (std::is_same_v<
                                   Record,
                                   core::CommandSchemaCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_command_schema(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir().applicability().command_schemas(),
                "command schema");
          } else if constexpr (std::is_same_v<
                                   Record,
                                   core::ComponentTypeCatalogRecordRefDTO>) {
            const auto &compiler_components = qualified.compiler_ir()
                                                  .applicability()
                                                  .composition()
                                                  .component_types();
            const auto component = std::find_if(
                compiler_components.begin(), compiler_components.end(),
                [&](const auto &candidate) {
                  return candidate.reference() == typed_reference;
                });
            if (component == compiler_components.end()) {
              detail::fail_retained_bundle(
                  "component type is absent from the compiler-qualified "
                  "closure");
            }
            const auto dependencies = select_component_capability_dependencies(
                *component, qualified.compiler_ir()
                                .applicability()
                                .capability_definitions());
            const auto requalified = resolve_and_qualify_component_type(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes, dependencies);
            require_exact_requalified_catalog_record(
                requalified, compiler_components, "component type");
          } else if constexpr (
              std::is_same_v<
                  Record, core::ControlInterfaceContractCatalogRecordRefDTO>) {
            const auto requalified =
                resolve_and_qualify_control_interface_contract(
                    typed_reference,
                    typed_reference.content_addressed_store_key, catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir()
                    .applicability()
                    .control_interface_contracts(),
                "control-interface contract");
          } else if constexpr (
              std::is_same_v<Record,
                             core::ControllerTopologyCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_controller_topology(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes,
                std::span<const QualifiedControlInterfaceContract>(
                    qualified.compiler_ir()
                        .applicability()
                        .control_interface_contracts()));
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir().applicability().controller_topologies(),
                "controller topology");
          } else if constexpr (std::is_same_v<
                                   Record,
                                   core::ProviderManifestCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_provider_manifest(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir().applicability().provider_manifests(),
                "provider manifest");
          } else if constexpr (
              std::is_same_v<Record,
                             core::StopEvidencePolicyCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_stop_evidence_policy(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir()
                    .applicability()
                    .stop_evidence_policies(),
                "Stop-evidence policy");
          } else if constexpr (std::is_same_v<
                                   Record,
                                   core::ContactPolicyCatalogRecordRefDTO>) {
            const auto requalified = resolve_and_qualify_contact_policy(
                typed_reference, typed_reference.content_addressed_store_key,
                catalog_bytes);
            require_exact_requalified_catalog_record(
                requalified,
                qualified.compiler_ir().applicability().contact_policies(),
                "contact policy");
          } else {
            detail::fail_retained_bundle(
                "structural retention encountered an unsupported catalog "
                "record kind");
          }

          const auto &registry = build_qualified_closed_schema_registry();
          const auto *schema = registry.find_qualified_catalog_schema(
              Record::kRecordKind, typed_reference.schema_id,
              typed_reference.schema_revision);
          if (schema == nullptr || schema->compiled_schema() == nullptr) {
            detail::fail_retained_bundle(
                "retained structural catalog reference selects no "
                "build-qualified schema");
          }
          const std::string schema_bytes(schema->descriptor_bytes());
          const core::ContentDigest schema_digest(sha256_digest(schema_bytes));
          const core::ContentAddressedStoreKey schema_key(
              detail::retained_store_key_for_digest(schema_digest.value()));
          const auto schema_identity =
              std::make_pair(typed_reference.schema_id.value(),
                             typed_reference.schema_revision.value());
          const PreparedSchemaArtifact prepared_schema{
              Record::kRecordKind,
              core::RetainedSchemaArtifactRefDTO{
                  typed_reference.schema_id, typed_reference.schema_revision,
                  schema_digest, schema_key}};
          const auto [schema_entry, schema_inserted] =
              schemas.emplace(schema_identity, prepared_schema);
          if (!schema_inserted &&
              (schema_entry->second.record_kind != Record::kRecordKind ||
               schema_entry->second.reference != prepared_schema.reference)) {
            detail::fail_retained_bundle(
                "one retained schema identity resolved to unequal catalog "
                "families or artifacts");
          }
          source_records.push_back(retained_source_record(typed_reference));
        },
        record);
  }
  if (referenced_assets.size() != assets.size()) {
    detail::fail_retained_bundle(
        "model-only compiler source closure contains an unretained asset");
  }

  std::string urdf = emit_canonical_urdf_document(composition.flattened_urdf());
  std::string srdf = emit_canonical_srdf_document(composition.flattened_srdf());
  std::vector<detail::PlanningResourceMapRow> planning_resource_rows;
  planning_resource_rows.reserve(
      composition.planning_resource_rewrites().size());
  for (const auto &row : composition.planning_resource_rewrites()) {
    planning_resource_rows.push_back(
        {row.model_asset_id, row.model_revision, row.source_logical_uri,
         row.compiled_logical_uri, row.media_type, row.byte_size,
         row.content_digest, row.content_addressed_store_key});
  }
  std::string planning_resource_map =
      detail::emit_planning_resource_map(std::move(planning_resource_rows));
  const auto &contract = require_flattened_model_contract();
  const std::vector<detail::RetainedArtifactInput> product_inputs{
      {core::ArtifactRoleId(
           std::string(detail::kPlanningResourceMapArtifactRole)),
       std::string(detail::kPlanningResourceMapMediaType),
       planning_resource_map},
      {core::ArtifactRoleId("srdf"), "application/xml", srdf},
      {core::ArtifactRoleId("urdf"), "application/xml", urdf},
  };
  detail::preflight_retained_product(
      core::ProductId("cell_planning_collision_model"),
      core::RetainedProductKind::kPlanningCollisionModel,
      ProductPlacement::kCellPlanningCollisionModel,
      contract.product_contract_id(), contract.product_contract_revision(),
      product_inputs);
  const auto &viewer_contract = require_viewer_model_contract();
  const std::array<detail::RetainedArtifactInput, 2U> viewer_inputs{{
      {core::ArtifactRoleId(
           std::string(detail::kPlanningResourceMapArtifactRole)),
       std::string(detail::kPlanningResourceMapMediaType),
       planning_resource_map},
      {core::ArtifactRoleId("urdf"), "application/xml", urdf},
  }};
  detail::preflight_retained_product(
      core::ProductId("cell_viewer_model"),
      core::RetainedProductKind::kViewerModel,
      ProductPlacement::kCellViewerModel, viewer_contract.product_contract_id(),
      viewer_contract.product_contract_revision(), viewer_inputs);
  std::optional<std::string> symbol_role_map;
  const BuildQualifiedProductContract *symbol_role_map_contract = nullptr;
  // The empty map is accepted only for the Scenario-candidate path of a
  // model-free cell; every other map stays strictly populated.
  const bool allow_empty_model_map =
      composition.model_occurrences().empty() && retain_empty_model_map;
  if (!composition.model_occurrences().empty() || retain_empty_model_map) {
    symbol_role_map = detail::emit_composed_model_symbol_role_map(
        qualified.compiler_ir(), allow_empty_model_map);
    detail::require_composed_model_symbol_role_map_matches(
        *symbol_role_map, qualified.compiler_ir(), allow_empty_model_map);
    const auto &qualified_symbol_contract =
        require_composed_model_symbol_role_map_contract();
    const std::array<detail::RetainedArtifactInput, 1U> symbol_inputs{{
        {core::ArtifactRoleId(
             std::string(detail::kComposedModelSymbolRoleMapArtifactRole)),
         std::string(detail::kComposedModelSymbolRoleMapMediaType),
         *symbol_role_map},
    }};
    detail::preflight_retained_product(
        core::ProductId("cell_composed_model_symbol_role_map"),
        core::RetainedProductKind::kSymbolRoleMap,
        ProductPlacement::kSymbolRoleMap,
        qualified_symbol_contract.product_contract_id(),
        qualified_symbol_contract.product_contract_revision(), symbol_inputs);
    symbol_role_map_contract = &qualified_symbol_contract;
  }
  std::vector<detail::PreparedControlDomainArtifact> control_domain_artifacts;
  const BuildQualifiedProductContract *control_domain_contract = nullptr;
  if (std::any_of(
          composition.graph().control_domains.begin(),
          composition.graph().control_domains.end(),
          [](const auto &domain) { return domain.provider() == nullptr; })) {
    const auto &qualified_control_contract =
        require_control_domain_control_contract();
    control_domain_artifacts.reserve(
        composition.graph().control_domains.size());
    for (const auto &domain : composition.graph().control_domains) {
      if (domain.provider())
        continue;
      auto prepared =
          prepare_control_artifacts(qualified, domain, input_source);
      std::string control_bytes = detail::emit_control_domain_control(
          qualified.compiler_ir(), domain.control_domain_id);
      detail::require_control_domain_control_matches(
          control_bytes, qualified.compiler_ir(), domain.control_domain_id);
      prepared.conditional_artifacts.push_back(detail::RetainedArtifactInput{
          core::ArtifactRoleId(
              std::string(detail::kControlDomainControlArtifactRole)),
          std::string(detail::kControlDomainControlMediaType),
          std::move(control_bytes)});
      std::sort(prepared.conditional_artifacts.begin(),
                prepared.conditional_artifacts.end(),
                [](const auto &left, const auto &right) {
                  return left.artifact_role_id < right.artifact_role_id;
                });
      detail::preflight_retained_product(
          core::ProductId(detail::control_domain_control_product_id(
              domain.control_domain_id)),
          core::RetainedProductKind::kControlDomainControl,
          ProductPlacement::kControlDomain,
          qualified_control_contract.product_contract_id(),
          qualified_control_contract.product_contract_revision(),
          prepared.conditional_artifacts);
      control_domain_artifacts.push_back(detail::PreparedControlDomainArtifact{
          domain.control_domain_id, std::move(prepared.conditional_artifacts)});
    }
    control_domain_contract = &qualified_control_contract;
  }
  std::vector<detail::PreparedRuntimeUnitArtifact> runtime_unit_artifacts;
  runtime_unit_artifacts.reserve(composition.graph().runtime_units.size());
  for (const auto &unit : composition.graph().runtime_units) {
    detail::PreparedRuntimeUnitKind kind;
    const BuildQualifiedProductContract *qualified_runtime_contract = nullptr;
    std::string manifest;
    std::string launchable;
    std::string_view manifest_role;
    std::string_view manifest_media_type;
    std::string_view launchable_role;
    std::string_view launchable_media_type;
    core::ProductId product_id(std::string{});
    if (unit.runtime_profile_id.value() == detail::kReferenceRuntimeProfileId &&
        unit.runtime_profile_revision.value() ==
            detail::reference_runtime_profile_revision()) {
      kind = detail::PreparedRuntimeUnitKind::kReference;
      qualified_runtime_contract = &require_reference_runtime_contract();
      manifest = detail::emit_reference_runtime_manifest(
          qualified.compiler_ir(), unit.runtime_unit_id);
      detail::require_reference_runtime_manifest_matches(
          manifest, qualified.compiler_ir(), unit.runtime_unit_id);
      launchable = std::string(detail::reference_runtime_launchable_script());
      if (launchable.empty() ||
          launchable.size() > detail::kMaxReferenceRuntimeLaunchableBytes) {
        throw std::logic_error(
            "build-owned reference runtime launchable exceeds its bound");
      }
      manifest_role = detail::kReferenceRuntimeManifestArtifactRole;
      manifest_media_type = detail::kReferenceRuntimeManifestMediaType;
      launchable_role = detail::kReferenceRuntimeLaunchableArtifactRole;
      launchable_media_type = detail::kReferenceRuntimeLaunchableMediaType;
      product_id = core::ProductId(
          detail::reference_runtime_product_id(unit.runtime_unit_id));
    } else if (unit.runtime_profile_id.value() ==
                   detail::kRos2ControlExecutionRuntimeProfileId &&
               unit.runtime_profile_revision.value() ==
                   detail::ros2_control_execution_runtime_profile_revision()) {
      kind = detail::PreparedRuntimeUnitKind::kRos2ControlExecution;
      qualified_runtime_contract =
          &require_ros2_control_execution_runtime_contract();
      manifest = detail::emit_ros2_control_execution_runtime_manifest(
          qualified.compiler_ir(), unit.runtime_unit_id);
      detail::require_ros2_control_execution_runtime_manifest_matches(
          manifest, qualified.compiler_ir(), unit.runtime_unit_id);
      launchable = detail::ros2_control_execution_runtime_launch_descriptor();
      if (launchable.empty() ||
          launchable.size() >
              detail::kMaxRos2ControlExecutionLaunchDescriptorBytes) {
        throw std::logic_error(
            "build-owned ros2_control execution launch descriptor exceeds "
            "its bound");
      }
      manifest_role = detail::kRos2ControlExecutionRuntimeManifestArtifactRole;
      manifest_media_type =
          detail::kRos2ControlExecutionRuntimeManifestMediaType;
      launchable_role =
          detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole;
      launchable_media_type =
          detail::kRos2ControlExecutionRuntimeLaunchDescriptorMediaType;
      product_id =
          core::ProductId(detail::ros2_control_execution_runtime_product_id(
              unit.runtime_unit_id));
    } else {
      detail::fail_retained_bundle(
          "runtime unit selects no build-qualified retained runtime product");
    }
    std::vector<detail::RetainedArtifactInput> runtime_inputs{
        {core::ArtifactRoleId(std::string(launchable_role)),
         std::string(launchable_media_type), launchable},
        {core::ArtifactRoleId(std::string(manifest_role)),
         std::string(manifest_media_type), manifest},
    };
    std::sort(runtime_inputs.begin(), runtime_inputs.end(),
              [](const auto &left, const auto &right) {
                return left.artifact_role_id < right.artifact_role_id;
              });
    detail::preflight_retained_product(
        product_id, core::RetainedProductKind::kRuntimeUnitConfiguration,
        ProductPlacement::kRuntimeUnit,
        qualified_runtime_contract->product_contract_id(),
        qualified_runtime_contract->product_contract_revision(),
        runtime_inputs);
    runtime_unit_artifacts.push_back(detail::PreparedRuntimeUnitArtifact{
        unit.runtime_unit_id, std::move(manifest), std::move(launchable), kind,
        qualified_runtime_contract});
  }
  std::vector<core::RetainedSchemaArtifactRefDTO> schema_artifacts;
  std::vector<core::CatalogRecordKind> schema_artifact_kinds;
  schema_artifacts.reserve(schemas.size());
  schema_artifact_kinds.reserve(schemas.size());
  for (auto &[unused, schema] : schemas) {
    (void)unused;
    schema_artifact_kinds.push_back(schema.record_kind);
    schema_artifacts.push_back(std::move(schema.reference));
  }
  std::vector<PreparedCatalogArtifact> catalog_artifacts;
  catalog_artifacts.reserve(prepared_catalogs.size());
  for (auto &[unused, artifact] : prepared_catalogs) {
    (void)unused;
    catalog_artifacts.push_back(std::move(artifact));
  }
  std::vector<PreparedSourceArtifact> source_artifacts;
  source_artifacts.reserve(prepared_sources.size());
  for (auto &[unused, artifact] : prepared_sources) {
    (void)unused;
    source_artifacts.push_back(std::move(artifact));
  }
  return {
      std::move(urdf),
      std::move(srdf),
      std::move(planning_resource_map),
      &contract,
      &viewer_contract,
      std::move(symbol_role_map),
      symbol_role_map_contract,
      std::move(control_domain_artifacts),
      control_domain_contract,
      std::move(runtime_unit_artifacts),
      std::move(schema_artifacts),
      std::move(schema_artifact_kinds),
      std::move(source_records),
      std::move(generator_records),
      std::move(catalog_artifacts),
      std::move(source_artifacts),
  };
}

void require_verified_product_artifact_readback(
    const core::RetainedProductDTO &product,
    IRawArtifactStore &artifact_store) {
  for (const auto &artifact : product.artifact_refs) {
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

void retain_prepared_inputs(
    const PreparedPlanningCollisionProduct &prepared,
    const detail::ICompilerInputArtifactSource &input_source,
    ICatalogArtifactStore &catalog_artifact_store,
    IRawArtifactStore &artifact_store) {
  for (const auto &input : prepared.catalog_artifacts) {
    const std::string bytes = require_verified_input_bytes(
        input_source, input.key, input.digest, input.byte_size,
        "prepared catalog artifact");
    const StoredCatalogArtifact stored =
        catalog_artifact_store.put_if_absent(input.key, input.digest, bytes);
    if (stored.content_digest != input.digest ||
        stored.content_addressed_store_key != input.key ||
        stored.byte_size != input.byte_size) {
      throw std::runtime_error(
          "production catalog insertion returned incoherent content "
          "identity metadata");
    }
    const auto readback = catalog_artifact_store.get_verified(
        input.key, input.digest, input.byte_size);
    if (!readback.has_value() || *readback != bytes) {
      throw std::runtime_error(
          "production retention could not re-verify an exact retained "
          "catalog artifact");
    }
  }

  for (const auto &input : prepared.source_artifacts) {
    const std::string bytes = require_verified_input_bytes(
        input_source, input.key, input.digest, input.byte_size,
        "prepared model-source artifact");
    const StoredRawArtifact stored = artifact_store.put_if_absent(bytes);
    if (stored.content_digest != input.digest ||
        stored.content_addressed_store_key != input.key ||
        stored.byte_size != input.byte_size) {
      throw std::runtime_error(
          "production retention input insertion returned incoherent "
          "content identity metadata");
    }
    const auto readback =
        artifact_store.get_verified(input.key, input.digest, input.byte_size);
    if (!readback.has_value() || *readback != bytes) {
      throw std::runtime_error(
          "production retention could not re-verify an exact retained "
          "model-source artifact");
    }
  }

  const auto &registry = build_qualified_closed_schema_registry();
  if (prepared.schema_artifact_kinds.size() !=
      prepared.schema_artifacts.size()) {
    throw std::logic_error(
        "prepared retained schema kind/reference closure is incoherent");
  }
  for (std::size_t index = 0U; index < prepared.schema_artifacts.size();
       ++index) {
    const auto &schema_ref = prepared.schema_artifacts[index];
    const auto *schema = registry.find_qualified_catalog_schema(
        prepared.schema_artifact_kinds[index], schema_ref.schema_id,
        schema_ref.schema_revision);
    if (schema == nullptr || schema->compiled_schema() == nullptr) {
      throw std::logic_error(
          "prepared retained schema artifact is no longer build-qualified");
    }
    const std::string bytes(schema->descriptor_bytes());
    if (sha256_digest(bytes) != schema_ref.content_digest.value() ||
        detail::retained_store_key_for_digest(
            schema_ref.content_digest.value()) !=
            schema_ref.content_addressed_store_key.value()) {
      throw std::logic_error(
          "prepared retained schema artifact changed before publication");
    }
    const StoredRawArtifact stored = artifact_store.put_if_absent(bytes);
    if (stored.content_digest != schema_ref.content_digest ||
        stored.content_addressed_store_key !=
            schema_ref.content_addressed_store_key ||
        stored.byte_size != bytes.size()) {
      throw std::runtime_error(
          "production schema insertion returned incoherent content "
          "identity metadata");
    }
    const auto readback = artifact_store.get_verified(
        schema_ref.content_addressed_store_key, schema_ref.content_digest,
        static_cast<std::uint64_t>(bytes.size()));
    if (!readback.has_value() || *readback != bytes) {
      throw std::runtime_error(
          "production retention could not re-verify an exact retained "
          "schema artifact");
    }
  }
}

void require_verified_current_bundle_artifacts(
    const RetainedBundle &bundle,
    const PreparedPlanningCollisionProduct &prepared,
    ICatalogArtifactStore &catalog_artifact_store,
    IRawArtifactStore &artifact_store,
    std::span<const CompositionControlBindingCoordinateMap> coordinate_maps) {
  const auto &value = bundle.value();
  if (value.catalogs.schema_artifacts != prepared.schema_artifacts ||
      value.provenance.source_records != prepared.source_records ||
      value.provenance.generator_records != prepared.generator_records) {
    throw std::logic_error(
        "reloaded retained bundle changed its prepared catalog/source "
        "closure");
  }
  for (const auto &input : prepared.catalog_artifacts) {
    const auto bytes = catalog_artifact_store.get_verified(
        input.key, input.digest, input.byte_size);
    if (!bytes.has_value()) {
      throw std::runtime_error(
          "production retention could not re-verify a retained catalog "
          "artifact after semantic reload");
    }
  }
  for (const auto &input : prepared.source_artifacts) {
    const auto bytes =
        artifact_store.get_verified(input.key, input.digest, input.byte_size);
    if (!bytes.has_value()) {
      throw std::runtime_error(
          "production retention could not re-verify a retained model-source "
          "artifact after semantic reload");
    }
  }
  if (prepared.schema_artifact_kinds.size() !=
      prepared.schema_artifacts.size()) {
    throw std::logic_error(
        "reloaded retained schema kind/reference closure is incoherent");
  }
  for (std::size_t index = 0U; index < prepared.schema_artifacts.size();
       ++index) {
    const auto &schema_ref = prepared.schema_artifacts[index];
    const auto *schema =
        build_qualified_closed_schema_registry().find_qualified_catalog_schema(
            prepared.schema_artifact_kinds[index], schema_ref.schema_id,
            schema_ref.schema_revision);
    if (schema == nullptr || schema->compiled_schema() == nullptr) {
      throw std::logic_error(
          "reloaded retained bundle names an unavailable schema artifact");
    }
    const std::string expected(schema->descriptor_bytes());
    const auto bytes = artifact_store.get_verified(
        schema_ref.content_addressed_store_key, schema_ref.content_digest,
        static_cast<std::uint64_t>(expected.size()));
    if (!bytes.has_value() || *bytes != expected) {
      throw std::runtime_error(
          "production retention could not re-verify a retained schema "
          "artifact after semantic reload");
    }
  }

  const auto &products = value.compiled_products;
  require_verified_product_artifact_readback(products.planning_collision_model,
                                             artifact_store);
  if (products.viewer_model.has_value()) {
    require_verified_product_artifact_readback(*products.viewer_model,
                                               artifact_store);
  }
  for (const auto &product : products.symbol_role_maps) {
    require_verified_product_artifact_readback(product, artifact_store);
    if (product.artifact_refs.size() != 1U ||
        product.artifact_refs.front().artifact_role_id.value() !=
            detail::kComposedModelSymbolRoleMapArtifactRole) {
      throw std::logic_error(
          "reloaded symbol/role-map product has an incoherent artifact set");
    }
    const auto bytes = artifact_store.get_verified(
        product.artifact_refs.front().content_addressed_store_key,
        product.artifact_refs.front().content_digest,
        product.artifact_refs.front().byte_size);
    if (!bytes.has_value() || !prepared.symbol_role_map.has_value() ||
        *bytes != *prepared.symbol_role_map) {
      throw std::runtime_error(
          "reloaded symbol/role-map product differs from its prepared bytes");
    }
    // A model-free bundle may carry its empty candidate map; see
    // parse_and_verify_composed_model_symbol_role_map.
    (void)detail::parse_and_verify_composed_model_symbol_role_map(
        *bytes, value.cell_graph.model_instances.empty());
  }
  if (products.symbol_role_maps.empty() !=
      !prepared.symbol_role_map.has_value()) {
    throw std::logic_error(
        "reloaded bundle changed symbol/role-map product presence");
  }
  if (products.control_domain_products.size() !=
      prepared.control_domain_artifacts.size()) {
    throw std::logic_error(
        "reloaded bundle changed control-domain product presence");
  }
  for (std::size_t index = 0U; index < products.control_domain_products.size();
       ++index) {
    const auto &group = products.control_domain_products[index];
    const auto &expected = prepared.control_domain_artifacts[index];
    if (group.control_domain_id != expected.control_domain_id ||
        group.products.size() != 1U ||
        group.products.front().artifact_refs.size() !=
            expected.artifacts.size()) {
      throw std::logic_error(
          "reloaded control-domain product has an incoherent subject or "
          "artifact set");
    }
    const auto &product = group.products.front();
    require_verified_product_artifact_readback(product, artifact_store);
    bool saw_control = false;
    for (const auto &expected_artifact : expected.artifacts) {
      const auto artifact =
          std::find_if(product.artifact_refs.begin(),
                       product.artifact_refs.end(), [&](const auto &candidate) {
                         return candidate.artifact_role_id ==
                                expected_artifact.artifact_role_id;
                       });
      if (artifact == product.artifact_refs.end()) {
        throw std::logic_error(
            "reloaded control-domain product omitted a prepared artifact");
      }
      const auto bytes = artifact_store.get_verified(
          artifact->content_addressed_store_key, artifact->content_digest,
          artifact->byte_size);
      if (!bytes.has_value() || *bytes != expected_artifact.bytes) {
        throw std::runtime_error(
            "reloaded control-domain product differs from its prepared bytes");
      }
      if (artifact->artifact_role_id.value() ==
          detail::kControlDomainControlArtifactRole) {
        static_cast<void>(detail::parse_and_verify_control_domain_control(
            *bytes, &value.cell_graph, coordinate_maps));
        saw_control = true;
      }
    }
    if (!saw_control) {
      throw std::logic_error(
          "reloaded control-domain product omitted its control artifact");
    }
  }
  if (products.runtime_unit_products.size() !=
      prepared.runtime_unit_artifacts.size()) {
    throw std::logic_error(
        "reloaded bundle changed runtime-unit product presence");
  }
  for (std::size_t index = 0U; index < products.runtime_unit_products.size();
       ++index) {
    const auto &group = products.runtime_unit_products[index];
    const auto &expected = prepared.runtime_unit_artifacts[index];
    if (group.runtime_unit_id != expected.runtime_unit_id ||
        group.products.size() != 1U ||
        group.products.front().artifact_refs.size() != 2U) {
      throw std::logic_error(
          "reloaded runtime-unit product has an incoherent subject or "
          "artifact set");
    }
    const auto &product = group.products.front();
    require_verified_product_artifact_readback(product, artifact_store);
    const std::string_view manifest_role =
        expected.kind == detail::PreparedRuntimeUnitKind::kReference
            ? detail::kReferenceRuntimeManifestArtifactRole
            : detail::kRos2ControlExecutionRuntimeManifestArtifactRole;
    const std::string_view launchable_role =
        expected.kind == detail::PreparedRuntimeUnitKind::kReference
            ? detail::kReferenceRuntimeLaunchableArtifactRole
            : detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole;
    std::optional<std::string> manifest;
    std::optional<std::string> launchable;
    for (const auto &artifact : product.artifact_refs) {
      const auto bytes = artifact_store.get_verified(
          artifact.content_addressed_store_key, artifact.content_digest,
          artifact.byte_size);
      if (!bytes.has_value()) {
        throw std::runtime_error(
            "reloaded runtime-unit artifact failed exact read-back");
      }
      if (artifact.artifact_role_id.value() == manifest_role) {
        manifest = *bytes;
      } else if (artifact.artifact_role_id.value() == launchable_role) {
        launchable = *bytes;
      }
    }
    if (!manifest.has_value() || !launchable.has_value() ||
        *manifest != expected.manifest_bytes ||
        *launchable != expected.launchable_bytes) {
      throw std::runtime_error(
          "reloaded runtime-unit product differs from its prepared bytes");
    }
    if (expected.kind == detail::PreparedRuntimeUnitKind::kReference) {
      detail::require_reference_runtime_product_matches_retained_bundle(
          *manifest, *launchable, value, group.runtime_unit_id);
    } else {
      detail::
          require_ros2_control_execution_runtime_product_matches_retained_bundle(
              *manifest, *launchable, value, group.runtime_unit_id);
    }
  }
}

void require_runtime_contracts_before_store(
    const RetentionQualifiedDeployment &qualified) {
  const auto &applicability = qualified.compiler_ir().applicability();
  static_cast<void>(detail::derive_runtime_contract_requirements(
      applicability.composition().graph(),
      qualified.deployment()
          .applicability_inputs()
          .value()
          .capability_provider_bindings,
      applicability.capabilities(), applicability.provider_endpoints(),
      applicability.runtime_profiles(), {}));
}

} // namespace

void detail::require_pinocchio_family_mimic_profile(
    const UrdfDocumentIr &urdf) {
  std::map<std::string, const UrdfJointIr *, std::less<>> joints_by_name;
  for (const auto &joint : urdf.joints) {
    if (!joints_by_name.emplace(joint.name, &joint).second) {
      detail::fail_retained_bundle(
          "Pinocchio-family profile received duplicate URDF joints");
    }
  }
  for (const auto &joint : urdf.joints) {
    if (!joint.mimic.has_value()) {
      continue;
    }
    const auto source = joints_by_name.find(joint.mimic->joint_name);
    if (source == joints_by_name.end()) {
      detail::fail_retained_bundle(
          "Pinocchio-family profile received an unresolved mimic "
          "source");
    }
    if (source->second->mimic.has_value()) {
      detail::fail_retained_bundle(
          "planning/collision publication rejects mimic-of-mimic: "
          "the current Pinocchio-family import profile is qualified only "
          "for one-level master/follower relations; generic "
          "source/composition retains valid chains unchanged");
    }
  }
}

detail::BuildQualifiedRetentionContext::BuildQualifiedRetentionContext(
    core::RetainedNodeEnvironmentDTO node_environment,
    core::ContentDigest compiler_contract_digest,
    core::ContentDigest compiler_build_digest)
    : node_environment_(std::move(node_environment)),
      compiler_contract_digest_(std::move(compiler_contract_digest)),
      compiler_build_digest_(std::move(compiler_build_digest)) {
  detail::require_retained_portable_identity(node_environment_.catalog_id,
                                             "node_environment.catalog_id");
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
    : BuildQualifiedRetentionContext(std::move(node_environment),
                                     std::move(compiler_contract_digest),
                                     std::move(compiler_build_digest)) {
  if (manifests.size() != 3U || manifests[0] == nullptr ||
      manifests[1] == nullptr || manifests[2] == nullptr ||
      manifests[0]->key().manifest_kind !=
          BuildManifestKind::kNodeEnvironment ||
      manifests[1]->key().manifest_kind !=
          BuildManifestKind::kCompilerContract ||
      manifests[2]->key().manifest_kind != BuildManifestKind::kCompilerBuild ||
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
    const RetentionQualifiedDeployment &qualified,
    const BuildQualifiedRetentionContext &context, std::string_view urdf,
    std::string_view srdf, std::string_view planning_resource_map,
    const BuildQualifiedProductContract &contract,
    const BuildQualifiedProductContract &viewer_contract,
    std::optional<std::string_view> symbol_role_map,
    const BuildQualifiedProductContract *symbol_role_map_contract,
    std::span<const detail::PreparedControlDomainArtifact>
        control_domain_artifacts,
    const BuildQualifiedProductContract *control_domain_contract,
    std::span<const detail::PreparedRuntimeUnitArtifact> runtime_unit_artifacts,
    std::span<const core::RetainedSchemaArtifactRefDTO> schema_artifacts,
    std::span<const core::RetainedSourceRecordDTO> source_records,
    std::span<const core::RetainedGeneratorRecordDTO> generator_records,
    IRawArtifactStore &artifact_store) {
  const auto &applicability = qualified.compiler_ir().applicability();
  const auto runtime_contract_requirements =
      detail::derive_runtime_contract_requirements(
          qualified.compiler_ir().applicability().composition().graph(),
          qualified.deployment()
              .applicability_inputs()
              .value()
              .capability_provider_bindings,
          applicability.capabilities(), applicability.provider_endpoints(),
          applicability.runtime_profiles(), {});
  const std::vector<detail::RetainedArtifactInput> artifacts{
      {core::ArtifactRoleId(
           std::string(detail::kPlanningResourceMapArtifactRole)),
       std::string(detail::kPlanningResourceMapMediaType),
       std::string(planning_resource_map)},
      {core::ArtifactRoleId("srdf"), "application/xml", std::string(srdf)},
      {core::ArtifactRoleId("urdf"), "application/xml", std::string(urdf)},
  };
  const core::RetainedProductDTO planning = detail::build_and_retain_product(
      core::ProductId("cell_planning_collision_model"),
      core::RetainedProductKind::kPlanningCollisionModel,
      ProductPlacement::kCellPlanningCollisionModel,
      contract.product_contract_id(), contract.product_contract_revision(),
      artifacts, artifact_store);

  const std::array<detail::RetainedArtifactInput, 2U> viewer_artifacts{{
      {core::ArtifactRoleId(
           std::string(detail::kPlanningResourceMapArtifactRole)),
       std::string(detail::kPlanningResourceMapMediaType),
       std::string(planning_resource_map)},
      {core::ArtifactRoleId("urdf"), "application/xml", std::string(urdf)},
  }};
  const core::RetainedProductDTO viewer = detail::build_and_retain_product(
      core::ProductId("cell_viewer_model"),
      core::RetainedProductKind::kViewerModel,
      ProductPlacement::kCellViewerModel, viewer_contract.product_contract_id(),
      viewer_contract.product_contract_revision(), viewer_artifacts,
      artifact_store);

  std::vector<core::RetainedProductDTO> symbol_role_maps;
  if (symbol_role_map.has_value() != (symbol_role_map_contract != nullptr)) {
    throw std::logic_error(
        "symbol/role-map bytes and qualified contract must be present "
        "together");
  }
  if (symbol_role_map.has_value()) {
    detail::require_composed_model_symbol_role_map_matches(
        *symbol_role_map, qualified.compiler_ir(),
        qualified.compiler_ir().applicability().composition()
            .model_occurrences().empty());
    const std::array<detail::RetainedArtifactInput, 1U> symbol_artifacts{{
        {core::ArtifactRoleId(
             std::string(detail::kComposedModelSymbolRoleMapArtifactRole)),
         std::string(detail::kComposedModelSymbolRoleMapMediaType),
         std::string(*symbol_role_map)},
    }};
    symbol_role_maps.push_back(detail::build_and_retain_product(
        core::ProductId("cell_composed_model_symbol_role_map"),
        core::RetainedProductKind::kSymbolRoleMap,
        ProductPlacement::kSymbolRoleMap,
        symbol_role_map_contract->product_contract_id(),
        symbol_role_map_contract->product_contract_revision(), symbol_artifacts,
        artifact_store));
    const auto readback = artifact_store.get_verified(
        symbol_role_maps.front()
            .artifact_refs.front()
            .content_addressed_store_key,
        symbol_role_maps.front().artifact_refs.front().content_digest,
        symbol_role_maps.front().artifact_refs.front().byte_size);
    if (!readback.has_value()) {
      throw std::runtime_error(
          "symbol/role-map raw artifact vanished after verified retention");
    }
    detail::require_composed_model_symbol_role_map_matches(
        *readback, qualified.compiler_ir(),
        qualified.compiler_ir().applicability().composition()
            .model_occurrences().empty());
  }

  if (control_domain_artifacts.empty() !=
      (control_domain_contract == nullptr)) {
    throw std::logic_error(
        "control-domain artifact bytes and qualified contract must be "
        "present together");
  }
  std::vector<core::RetainedControlDomainProductsDTO> control_domain_products;
  control_domain_products.reserve(control_domain_artifacts.size());
  for (const auto &input : control_domain_artifacts) {
    const auto control_input = std::find_if(
        input.artifacts.begin(), input.artifacts.end(), [](const auto &value) {
          return value.artifact_role_id.value() ==
                 detail::kControlDomainControlArtifactRole;
        });
    if (control_input == input.artifacts.end()) {
      throw std::logic_error(
          "prepared control-domain product lost its required artifact");
    }
    detail::require_control_domain_control_matches(
        control_input->bytes, qualified.compiler_ir(), input.control_domain_id);
    core::RetainedProductDTO product = detail::build_and_retain_product(
        core::ProductId(
            detail::control_domain_control_product_id(input.control_domain_id)),
        core::RetainedProductKind::kControlDomainControl,
        ProductPlacement::kControlDomain,
        control_domain_contract->product_contract_id(),
        control_domain_contract->product_contract_revision(), input.artifacts,
        artifact_store);
    const auto retained_control =
        std::find_if(product.artifact_refs.begin(), product.artifact_refs.end(),
                     [](const auto &value) {
                       return value.artifact_role_id.value() ==
                              detail::kControlDomainControlArtifactRole;
                     });
    if (retained_control == product.artifact_refs.end()) {
      throw std::logic_error(
          "retained control-domain product lost its required artifact");
    }
    const auto readback = artifact_store.get_verified(
        retained_control->content_addressed_store_key,
        retained_control->content_digest, retained_control->byte_size);
    if (!readback.has_value()) {
      throw std::runtime_error(
          "control-domain raw artifact vanished after verified retention");
    }
    detail::require_control_domain_control_matches(
        *readback, qualified.compiler_ir(), input.control_domain_id);
    control_domain_products.push_back(core::RetainedControlDomainProductsDTO{
        input.control_domain_id,
        std::vector<core::RetainedProductDTO>{std::move(product)}});
  }

  std::vector<core::RetainedRuntimeUnitProductsDTO> runtime_unit_products;
  runtime_unit_products.reserve(runtime_unit_artifacts.size());
  for (const auto &input : runtime_unit_artifacts) {
    if (input.contract == nullptr) {
      throw std::logic_error(
          "prepared runtime-unit product has no qualified contract");
    }
    std::string_view manifest_role;
    std::string_view manifest_media_type;
    std::string_view launchable_role;
    std::string_view launchable_media_type;
    core::ProductId product_id(std::string{});
    switch (input.kind) {
    case detail::PreparedRuntimeUnitKind::kReference:
      detail::require_reference_runtime_manifest_matches(
          input.manifest_bytes, qualified.compiler_ir(), input.runtime_unit_id);
      if (input.launchable_bytes !=
          detail::reference_runtime_launchable_script()) {
        throw std::logic_error(
            "prepared reference runtime launchable changed before retention");
      }
      manifest_role = detail::kReferenceRuntimeManifestArtifactRole;
      manifest_media_type = detail::kReferenceRuntimeManifestMediaType;
      launchable_role = detail::kReferenceRuntimeLaunchableArtifactRole;
      launchable_media_type = detail::kReferenceRuntimeLaunchableMediaType;
      product_id = core::ProductId(
          detail::reference_runtime_product_id(input.runtime_unit_id));
      break;
    case detail::PreparedRuntimeUnitKind::kRos2ControlExecution:
      detail::require_ros2_control_execution_runtime_manifest_matches(
          input.manifest_bytes, qualified.compiler_ir(), input.runtime_unit_id);
      if (input.launchable_bytes !=
          detail::ros2_control_execution_runtime_launch_descriptor()) {
        throw std::logic_error(
            "prepared ros2_control execution launch descriptor changed before "
            "retention");
      }
      manifest_role = detail::kRos2ControlExecutionRuntimeManifestArtifactRole;
      manifest_media_type =
          detail::kRos2ControlExecutionRuntimeManifestMediaType;
      launchable_role =
          detail::kRos2ControlExecutionRuntimeLaunchDescriptorArtifactRole;
      launchable_media_type =
          detail::kRos2ControlExecutionRuntimeLaunchDescriptorMediaType;
      product_id =
          core::ProductId(detail::ros2_control_execution_runtime_product_id(
              input.runtime_unit_id));
      break;
    }
    std::vector<detail::RetainedArtifactInput> runtime_artifacts{
        {core::ArtifactRoleId(std::string(launchable_role)),
         std::string(launchable_media_type), input.launchable_bytes},
        {core::ArtifactRoleId(std::string(manifest_role)),
         std::string(manifest_media_type), input.manifest_bytes},
    };
    std::sort(runtime_artifacts.begin(), runtime_artifacts.end(),
              [](const auto &left, const auto &right) {
                return left.artifact_role_id < right.artifact_role_id;
              });
    core::RetainedProductDTO product = detail::build_and_retain_product(
        product_id, core::RetainedProductKind::kRuntimeUnitConfiguration,
        ProductPlacement::kRuntimeUnit, input.contract->product_contract_id(),
        input.contract->product_contract_revision(), runtime_artifacts,
        artifact_store);
    std::optional<std::string> retained_manifest;
    std::optional<std::string> retained_launchable;
    for (const auto &artifact : product.artifact_refs) {
      const auto readback = artifact_store.get_verified(
          artifact.content_addressed_store_key, artifact.content_digest,
          artifact.byte_size);
      if (!readback.has_value()) {
        throw std::runtime_error(
            "runtime-unit artifact vanished after verified retention");
      }
      if (artifact.artifact_role_id.value() == manifest_role) {
        retained_manifest = *readback;
      } else if (artifact.artifact_role_id.value() == launchable_role) {
        retained_launchable = *readback;
      }
    }
    if (!retained_manifest.has_value() || !retained_launchable.has_value() ||
        *retained_manifest != input.manifest_bytes ||
        *retained_launchable != input.launchable_bytes) {
      throw std::runtime_error(
          "runtime-unit product failed exact artifact read-back");
    }
    runtime_unit_products.push_back(core::RetainedRuntimeUnitProductsDTO{
        input.runtime_unit_id,
        std::vector<core::RetainedProductDTO>{std::move(product)}});
  }
  detail::require_runtime_contract_requirements(
      qualified.compiler_ir().applicability().composition().graph(),
      qualified.deployment()
          .applicability_inputs()
          .value()
          .capability_provider_bindings,
      runtime_contract_requirements, runtime_unit_products);

  const CanonicalValue placeholder = retained_bundle_projection(
      kZeroDigest, qualified, context, schema_artifacts, source_records,
      generator_records, planning, viewer, control_domain_products,
      symbol_role_maps, runtime_unit_products, runtime_contract_requirements);
  const std::string retained_bundle_id = detail::compute_content_identity(
      placeholder,
      detail::ContentIdentitySpec{
          .identity_domain = "retained-bundle-id",
          .api_version = std::string(core::kRetainedBundleApiVersion),
          .own_field = "retained_bundle_id",
      });
  CanonicalValue root = retained_bundle_projection(
      retained_bundle_id, qualified, context, schema_artifacts, source_records,
      generator_records, planning, viewer, control_domain_products,
      symbol_role_maps, runtime_unit_products, runtime_contract_requirements);
  std::string bytes = canonical_json(root);
  core::RetainedBundleDTO value = retained_bundle_value(
      retained_bundle_id, qualified, context,
      std::vector<core::RetainedSchemaArtifactRefDTO>(schema_artifacts.begin(),
                                                      schema_artifacts.end()),
      std::vector<core::RetainedSourceRecordDTO>(source_records.begin(),
                                                 source_records.end()),
      std::vector<core::RetainedGeneratorRecordDTO>(generator_records.begin(),
                                                    generator_records.end()),
      planning, viewer, control_domain_products, symbol_role_maps,
      runtime_unit_products, runtime_contract_requirements);
  auto bundle = detail::RetainedBundleConstructionAuthority::construct(
      core::RetainedBundleId(retained_bundle_id), std::move(value),
      std::move(root), std::move(bytes));
  if (!verify_retained_bundle_identity(*bundle)) {
    try {
      static_cast<void>(detail::RetainedBundleReloadAuthority::reload(
          bundle->canonical_bytes()));
    } catch (const std::exception &error) {
      throw std::logic_error(
          std::string("materializer produced an invalid retained-bundle "
                      "projection: ") +
          error.what());
    }
    throw std::logic_error(
        "materializer projection reparsed but differed from its typed "
        "value");
  }
  return bundle;
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize_store_and_reload_production(
    const RetentionQualifiedDeployment &qualified,
    IBuildManifestRepository &manifest_repository,
    const ICompilerInputArtifactSource &input_artifact_source,
    std::span<const BuildQualifiedStructuralModelPreparation>
        structural_model_preparations,
    ICatalogArtifactStore &catalog_artifact_store,
    IRawArtifactStore &artifact_store,
    IRetainedBundleRepository &bundle_repository,
    const bool retain_empty_model_map) {
  const PreparedPlanningCollisionProduct prepared =
      prepare_planning_collision_product_before_store(
          qualified, input_artifact_source, structural_model_preparations,
          retain_empty_model_map);
  require_runtime_contracts_before_store(qualified);
  const BuildQualifiedRetentionContext context =
      BuildQualifiedRetentionAuthority::qualify_persisted(manifest_repository);
  retain_prepared_inputs(prepared, input_artifact_source,
                         catalog_artifact_store, artifact_store);
  const auto bundle = materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      prepared.planning_resource_map, *prepared.contract,
      *prepared.viewer_contract,
      prepared.symbol_role_map.has_value()
          ? std::optional<std::string_view>(*prepared.symbol_role_map)
          : std::nullopt,
      prepared.symbol_role_map_contract, prepared.control_domain_artifacts,
      prepared.control_domain_contract, prepared.runtime_unit_artifacts,
      prepared.schema_artifacts, prepared.source_records,
      prepared.generator_records, artifact_store);
  (void)bundle_repository.put_if_absent(bundle);
  const auto reloaded =
      bundle_repository.get_verified_envelope(bundle->retained_bundle_id());
  if (reloaded == nullptr ||
      reloaded->retained_bundle_id() != bundle->retained_bundle_id() ||
      reloaded->canonical_bytes() != bundle->canonical_bytes() ||
      !verify_retained_bundle_identity(*reloaded)) {
    throw std::runtime_error(
        "production retention did not independently reload the exact "
        "semantic bundle");
  }
  require_verified_current_bundle_artifacts(
      *reloaded, prepared, catalog_artifact_store, artifact_store,
      qualified.compiler_ir()
          .applicability()
          .composition()
          .control_binding_coordinate_maps());
  return reloaded;
}

std::shared_ptr<const RetainedBundle>
detail::RetainedBundleMaterializer::materialize(
    const RetentionQualifiedDeployment &qualified,
    const BuildQualifiedRetentionContext &context,
    const ICompilerInputArtifactSource &input_artifact_source,
    std::span<const BuildQualifiedStructuralModelPreparation>
        structural_model_preparations,
    ICatalogArtifactStore &catalog_artifact_store,
    IRawArtifactStore &artifact_store) {
  const PreparedPlanningCollisionProduct prepared =
      prepare_planning_collision_product_before_store(
          qualified, input_artifact_source, structural_model_preparations,
          false);
  require_runtime_contracts_before_store(qualified);
  retain_prepared_inputs(prepared, input_artifact_source,
                         catalog_artifact_store, artifact_store);
  return materialize_prepared(
      qualified, context, prepared.urdf, prepared.srdf,
      prepared.planning_resource_map, *prepared.contract,
      *prepared.viewer_contract,
      prepared.symbol_role_map.has_value()
          ? std::optional<std::string_view>(*prepared.symbol_role_map)
          : std::nullopt,
      prepared.symbol_role_map_contract, prepared.control_domain_artifacts,
      prepared.control_domain_contract, prepared.runtime_unit_artifacts,
      prepared.schema_artifacts, prepared.source_records,
      prepared.generator_records, artifact_store);
}

} // namespace manyforge::deployment

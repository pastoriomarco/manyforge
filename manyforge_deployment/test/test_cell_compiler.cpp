#include <gtest/gtest.h>

#include <manyforge_core/types/catalog.hpp>
#include <manyforge_core/types/cell_graph.hpp>
#include <manyforge_deployment/apply_fence_qualification.hpp>
#include <manyforge_deployment/artifact_codec.hpp>
#include <manyforge_deployment/cell_applicability.hpp>
#include <manyforge_deployment/cell_compiler.hpp>
#include <manyforge_deployment/cell_composition.hpp>
#include <manyforge_deployment/cell_graph_codec.hpp>
#include <manyforge_deployment/deployment_applicability_codec.hpp>
#include <manyforge_deployment/site_overlay_resolution.hpp>

#include <algorithm>
#include <concepts>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "cell_compiler_internal.hpp"

namespace manyforge::deployment {
namespace {

constexpr char kZeroDigest[] =
    "sha256:0000000000000000000000000000000000000000000000000000000000000000";

template <typename Value>
concept PublicCompilerInput = requires(Value&& value) {
  compile_cell_ir(std::forward<Value>(value));
};

static_assert(!std::is_aggregate_v<CompilerQualifiedCellIr>);
static_assert(!std::is_default_constructible_v<CompilerQualifiedCellIr>);
static_assert(!std::is_constructible_v<CompilerQualifiedCellIr,
                                       ApplicabilityQualifiedCellIr>);
static_assert(std::same_as<
              decltype(std::declval<const CompilerQualifiedCellIr&>()
                           .applicability()),
              const ApplicabilityQualifiedCellIr&>);
static_assert(std::same_as<
              decltype(std::declval<const CompilerQualifiedCellIr&>()
                           .catalog_record_refs()),
              const std::vector<core::CatalogRecordRefDTO>&>);
static_assert(PublicCompilerInput<ApplicabilityQualifiedCellIr>);
static_assert(!PublicCompilerInput<core::CellGraphDTO>);
static_assert(!PublicCompilerInput<GraphRevisionVerifiedCellGraph>);
static_assert(!PublicCompilerInput<CompositionQualifiedCellIr>);

[[nodiscard]] GraphRevisionVerifiedCellGraph make_empty_verified_graph() {
  const auto candidate = decode_cell_graph_candidate(
      parse_strict_json_model_yaml(
          "{\"api_version\":\"manyforge.cell_graph.v1\","
          "\"graph_revision\":\"" +
          std::string(kZeroDigest) +
          "\",\"cell_id\":\"compiler_empty_cell\",\"runtime_units\":[],"
          "\"model_instances\":[],\"controllable_components\":[],"
          "\"control_domains\":[],\"control_bindings\":[],"
          "\"component_attachments\":[]}"));
  return compute_graph_revision(prepare_cell_graph_identity(
      validate_intrinsic_cell_graph_candidate(candidate), {}));
}

[[nodiscard]] RevisionVerifiedDeploymentApplicabilityInputs
make_empty_applicability_inputs() {
  return compute_capability_provider_binding_revisions(
      decode_deployment_applicability_inputs_candidate(
          parse_strict_json_model_yaml(
              "{\"api_version\":\"manyforge.deployment.v1\","
              "\"desired_control_configurations\":[],"
              "\"capability_provider_bindings\":[],"
              "\"external_resources\":[]}")));
}

[[nodiscard]] CompilerQualifiedCellIr compile_canonical_empty_cell() {
  const auto verified_graph = make_empty_verified_graph();
  const auto composition = compose_cell_occurrences(
      verified_graph, std::span<const SourceVerifiedModelAsset>{},
      std::span<const QualifiedComponentType>{});
  const auto deployment_inputs = make_empty_applicability_inputs();
  const auto selected_overlay = select_site_overlay_for_deployment(
      deployment_inputs.value().site_overlay_ref,
      deployment_inputs.value().external_resources, nullptr);
  auto applicability = qualify_cell_applicability(
      composition, deployment_inputs, {}, {}, {}, {}, {}, {}, {}, {}, {},
      selected_overlay);
  return compile_cell_ir(applicability);
}

TEST(CellCompiler,
     CanonicalEmptyTraversesIdentityCompositionApplicabilityAndFinalCompiler) {
  // Every predecessor is local to this helper. Reading the returned value
  // therefore proves that the final boundary owns, rather than borrows, the
  // exact applicability-qualified result.
  const auto compiled = compile_canonical_empty_cell();
  const auto& retained = compiled.applicability();

  EXPECT_EQ(retained.composition().graph().cell_id,
            core::CellId("compiler_empty_cell"));
  EXPECT_NE(retained.composition().graph().graph_revision.value(),
            std::string(kZeroDigest));
  EXPECT_FALSE(retained.selected_site_overlay_ref().has_value());
  EXPECT_TRUE(retained.runtime_profiles().empty());
  EXPECT_TRUE(retained.capability_definitions().empty());
  EXPECT_TRUE(retained.control_interface_contracts().empty());
  EXPECT_TRUE(retained.controller_topologies().empty());
  EXPECT_TRUE(retained.provider_manifests().empty());
  EXPECT_TRUE(retained.stop_evidence_policies().empty());
  EXPECT_TRUE(retained.binding_interfaces().empty());
  EXPECT_TRUE(retained.controllers().empty());
  EXPECT_TRUE(retained.routes().empty());
  EXPECT_TRUE(retained.capabilities().empty());
  EXPECT_TRUE(retained.provider_endpoints().empty());
  EXPECT_TRUE(retained.external_resources().empty());
  EXPECT_TRUE(retained.stop_applicability().empty());
  EXPECT_TRUE(compiled.catalog_record_refs().empty());
}

TEST(CellCompiler, CopySharesTheSameImmutableQualifiedPredecessor) {
  const auto compiled = compile_canonical_empty_cell();
  const auto copied = compiled;

  EXPECT_EQ(&copied.applicability(), &compiled.applicability());
  EXPECT_EQ(copied.applicability().composition().graph(),
            compiled.applicability().composition().graph());
  EXPECT_EQ(copied.catalog_record_refs(), compiled.catalog_record_refs());
}

[[nodiscard]] core::ControllableComponentDTO qualification_component(
    const std::size_t index) {
  return {
      core::ControllableComponentId("component-" + std::to_string(index)),
      core::ComponentTypeId("component-type"),
      core::ComponentTypeRevision(kZeroDigest),
      {},
      {},
      std::nullopt,
  };
}

[[nodiscard]] core::ControlDomainDTO qualification_domain(
    const std::size_t index) {
  return {
      core::ControlDomainId("domain-" + std::to_string(index)),
      core::ControlDomainRevision(kZeroDigest),
      {},
      core::EndpointRoleId("coordinator"),
      core::ControllerTopologyId("topology"),
      core::TopologyRevision(kZeroDigest),
      core::ContainmentGranularity::kResource,
  };
}

[[nodiscard]] core::ControlBindingDTO qualification_binding(
    const std::size_t index) {
  return {
      core::ControlBindingId("binding-" + std::to_string(index)),
      core::ControlBindingRevision(kZeroDigest),
      core::ControllableComponentId(
          "component-" +
          std::to_string(
              index %
              kMaximumQualifiedApplyFenceControllableComponentsPerCell)),
      core::CommandTargetId("target-" + std::to_string(index)),
      core::CapabilityId("capability"),
      core::CapabilityDefinitionRevision(kZeroDigest),
      core::ControlDomainId(
          "domain-" +
          std::to_string(
              index % kMaximumQualifiedApplyFenceControlDomainsPerCell)),
      {},
      core::ControlInterfaceContractId("interface"),
      core::ControlInterfaceRevision(kZeroDigest),
      core::ControllerTopologyId("topology"),
      core::TopologyRevision(kZeroDigest),
      {core::ControlRouteId("route")},
      {},
      core::ProviderInstanceId("provider"),
      core::RuntimeUnitId("provider-runtime"),
      core::ProviderManifestId("manifest"),
      core::ProviderManifestRevision(kZeroDigest),
      core::StateAuthorityId("state-authority"),
      core::StateAuthorityClass::kMeasured,
      core::StopEvidencePolicyId("stop-policy"),
      core::StopEvidencePolicyRevision(kZeroDigest),
      core::ContainmentGranularity::kResource,
      {},
  };
}

[[nodiscard]] core::CellGraphDTO graph_at_apply_fence_bound() {
  core::CellGraphDTO graph{
      "manyforge.cell_graph.v1",
      core::GraphRevision(kZeroDigest),
      core::CellId("qualification-cell"),
      {},
      {},
      {},
      {},
      {},
      {},
  };
  for (std::size_t index = 0U;
       index < kMaximumQualifiedApplyFenceControllableComponentsPerCell;
       ++index) {
    graph.controllable_components.push_back(qualification_component(index));
  }
  for (std::size_t index = 0U;
       index < kMaximumQualifiedApplyFenceControlDomainsPerCell; ++index) {
    graph.control_domains.push_back(qualification_domain(index));
  }
  for (std::size_t index = 0U;
       index < kMaximumQualifiedApplyFenceControlBindingsPerCell; ++index) {
    graph.control_bindings.push_back(qualification_binding(index));
  }
  for (std::size_t index = 0U;
       index < kMaximumQualifiedApplyFenceResourcesPerCell; ++index) {
    graph.control_bindings[index % graph.control_bindings.size()]
        .resource_bindings.push_back(
            {core::ResourceRoleId("role-" + std::to_string(index)),
             core::ResourceId("resource-" + std::to_string(index))});
  }
  return graph;
}

void expect_apply_fence_bound_error(const core::CellGraphDTO& graph,
                                    const std::string_view dimension) {
  try {
    detail::validate_apply_fence_qualification_bound(graph);
    FAIL() << "expected CellCompilerError";
  } catch (const CellCompilerError& error) {
    EXPECT_NE(std::string_view(error.what()).find("Apply-fence envelope"),
              std::string_view::npos)
        << error.what();
    EXPECT_NE(std::string_view(error.what()).find(dimension),
              std::string_view::npos)
        << error.what();
  }
}

TEST(CellCompiler, ApplyFenceQualificationAcceptsTheExactPerCellEnvelope) {
  auto graph = graph_at_apply_fence_bound();
  EXPECT_NO_THROW(detail::validate_apply_fence_qualification_bound(graph));

  // Reusing one physical resource in another binding does not enlarge the
  // transition scope and therefore does not consume another resource row.
  graph.control_bindings.back().resource_bindings.push_back(
      {core::ResourceRoleId("shared-role"), core::ResourceId("resource-0")});
  EXPECT_NO_THROW(detail::validate_apply_fence_qualification_bound(graph));
}

TEST(CellCompiler, ApplyFenceQualificationRejectsEachOversizedDimension) {
  {
    auto graph = graph_at_apply_fence_bound();
    graph.controllable_components.push_back(qualification_component(
        kMaximumQualifiedApplyFenceControllableComponentsPerCell));
    expect_apply_fence_bound_error(graph, "controllable_components");
  }
  {
    auto graph = graph_at_apply_fence_bound();
    graph.control_domains.push_back(qualification_domain(
        kMaximumQualifiedApplyFenceControlDomainsPerCell));
    expect_apply_fence_bound_error(graph, "control_domains");
  }
  {
    auto graph = graph_at_apply_fence_bound();
    graph.control_bindings.push_back(qualification_binding(
        kMaximumQualifiedApplyFenceControlBindingsPerCell));
    expect_apply_fence_bound_error(graph, "control_bindings");
  }
  {
    auto graph = graph_at_apply_fence_bound();
    graph.control_bindings.front().resource_bindings.push_back(
        {core::ResourceRoleId("overflow-role"),
         core::ResourceId("overflow-resource")});
    expect_apply_fence_bound_error(graph, "resources");
  }
}

[[nodiscard]] core::ModelAssetCatalogRecordRefDTO model_reference(
    std::string id = "asset") {
  return {
      core::ModelAssetId(std::move(id)),
      core::ModelRevision(std::string(kZeroDigest)),
      core::SchemaId(std::string(core::kModelAssetApiVersion)),
      core::SchemaRevision(std::string(kZeroDigest)),
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey("sha256/model"),
  };
}

[[nodiscard]] core::RuntimeProfileCatalogRecordRefDTO runtime_reference() {
  return {
      core::RuntimeProfileId("runtime"),
      core::RuntimeProfileRevision(std::string(kZeroDigest)),
      core::SchemaId(std::string(core::kRuntimeProfileApiVersion)),
      core::SchemaRevision(std::string(kZeroDigest)),
      core::ContentDigest(std::string(kZeroDigest)),
      core::ContentAddressedStoreKey("sha256/runtime"),
  };
}

void expect_catalog_error_containing(
    std::vector<core::CatalogRecordRefDTO> references,
    std::string_view fragment) {
  try {
    (void)detail::canonicalize_compiler_catalog_record_refs(
        std::move(references));
    FAIL() << "expected CellCompilerError";
  } catch (const CellCompilerError& error) {
    EXPECT_NE(std::string_view(error.what()).find(fragment),
              std::string_view::npos)
        << error.what();
  }
}

TEST(CellCompiler,
     CatalogProvenanceCanonicalizationRejectsExactAndConflictingDuplicates) {
  const core::CatalogRecordRefDTO model = model_reference();
  const core::CatalogRecordRefDTO runtime = runtime_reference();
  const auto forward = detail::canonicalize_compiler_catalog_record_refs(
      {runtime, model});
  const auto reverse = detail::canonicalize_compiler_catalog_record_refs(
      {model, runtime});

  EXPECT_EQ(forward, reverse);
  ASSERT_EQ(forward.size(), 2U);
  EXPECT_TRUE(std::is_sorted(forward.begin(), forward.end(),
                             core::CanonicalCatalogRecordRefLess{}));

  expect_catalog_error_containing({model, model}, "duplicate exact");

  auto conflicting_typed = model_reference();
  conflicting_typed.content_digest =
      core::ContentDigest(
          "sha256:1111111111111111111111111111111111111111111111111111111111111111");
  const core::CatalogRecordRefDTO conflicting = conflicting_typed;
  expect_catalog_error_containing({model, conflicting}, "unequal");
}

}  // namespace
}  // namespace manyforge::deployment

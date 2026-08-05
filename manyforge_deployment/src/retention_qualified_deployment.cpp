#include <manyforge_deployment/retention_qualified_deployment.hpp>

#include <string>
#include <string_view>

namespace manyforge::deployment {
namespace {

[[noreturn]] void fail(std::string_view projection) {
  throw RetentionQualificationError(
      "deployment/compiler retention projection mismatch: " +
      std::string(projection));
}

void require_exact_applicability_projection(
    const RevisionVerifiedDeploymentApplicabilityInputs& deployment,
    const RevisionVerifiedDeploymentApplicabilityInputs& compiled) {
  if (deployment.value() != compiled.value()) {
    fail("applicability value tables");
  }
  if (deployment.normalized_desired_control_configurations() !=
      compiled.normalized_desired_control_configurations()) {
    fail("desired_control_configurations");
  }
  if (deployment.normalized_capability_provider_bindings() !=
      compiled.normalized_capability_provider_bindings()) {
    fail("capability_provider_bindings");
  }
  if (deployment.normalized_external_resources() !=
      compiled.normalized_external_resources()) {
    fail("external_resources");
  }
  if (deployment.site_overlay_ref_presence() !=
      compiled.site_overlay_ref_presence()) {
    fail("site_overlay_ref source presence");
  }
  if (deployment.normalized_site_overlay_ref() !=
      compiled.normalized_site_overlay_ref()) {
    fail("site_overlay_ref source value");
  }
  if (deployment.external_resources_was_present() !=
      compiled.external_resources_was_present()) {
    fail("external_resources source presence");
  }
  if (deployment.assistant_settings().source_was_present() !=
          compiled.assistant_settings().source_was_present() ||
      deployment.assistant_settings().value() !=
          compiled.assistant_settings().value() ||
      deployment.assistant_settings().normalized_source() !=
          compiled.assistant_settings().normalized_source()) {
    fail("assistant source projection");
  }
}

}  // namespace

RetentionQualifiedDeployment qualify_deployment_for_retention(
    const RevisionVerifiedDeploymentArtifact& deployment,
    const CompilerQualifiedCellIr& compiler_ir) {
  const auto& applicability = compiler_ir.applicability();
  const auto& composition = applicability.composition();
  const auto& compiled_graph = composition.verified_graph();

  if (deployment.cell_id() != compiled_graph.graph().cell_id) {
    fail("cell_id");
  }
  if (deployment.graph().graph() != compiled_graph.graph() ||
      deployment.graph().normalized_root() !=
          compiled_graph.normalized_root()) {
    fail("complete cell_graph");
  }

  require_exact_applicability_projection(
      deployment.applicability_inputs(),
      applicability.deployment_inputs());

  const auto& selected_ref = applicability.selected_site_overlay_ref();
  const auto& authored_ref =
      deployment.applicability_inputs().value().site_overlay_ref;
  if (selected_ref != authored_ref) {
    fail("selected site_overlay_ref");
  }

  if (applicability.assistant_settings().value() !=
      deployment.assistant_settings().value()) {
    fail("qualified assistant settings");
  }

  if (deployment.catalog_records() != compiler_ir.catalog_record_refs()) {
    fail("catalog/source reference union");
  }

  return RetentionQualifiedDeployment(deployment, compiler_ir);
}

}  // namespace manyforge::deployment

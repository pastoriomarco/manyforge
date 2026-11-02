#include "manyforge_deployment/verified_retained_bundle.hpp"

#include <utility>

namespace manyforge::deployment {

VerifiedRetainedProjectionInput verify_retained_projection_input(
    const RetainedBundle& bundle,
    core::LogicalSceneSnapshotRoot logical_scene,
    const IVerifiedRetainedPlanningModelSource& planning_model_source) {
  if (!verify_retained_bundle_identity(bundle)) {
    throw RetainedProjectionInputError(
        "retained projection input has an invalid retained bundle");
  }

  const auto& retained = bundle.value();
  const auto& snapshot = *logical_scene.shared();
  if (!snapshot.locally_valid()) {
    throw RetainedProjectionInputError(
        "retained projection input has an invalid logical snapshot");
  }
  if (snapshot.cell_id != retained.cell_graph.cell_id) {
    throw RetainedProjectionInputError(
        "retained projection input crosses cell identities");
  }

  const auto& planning_product =
      retained.compiled_products.planning_collision_model;
  const RetainedPlanningModelQuery query{
      bundle.retained_bundle_id(), planning_product.product_id};
  auto planning_model = planning_model_source.get_verified(query);
  if (!planning_model.has_value()) {
    throw RetainedProjectionInputError(
        "retained projection planning product is absent");
  }
  if (planning_model->query() != query) {
    throw RetainedProjectionInputError(
        "retained projection planning source returned different coordinates");
  }

  core::ProjectionRevisionDTO projection_revision{
      bundle.retained_bundle_id(), retained.cell_graph.graph_revision,
      snapshot.revision};
  if (!projection_revision.valid()) {
    throw RetainedProjectionInputError(
        "retained projection revision is invalid");
  }

  return VerifiedRetainedProjectionInput(
      std::move(projection_revision), std::move(logical_scene),
      std::move(*planning_model));
}

}  // namespace manyforge::deployment

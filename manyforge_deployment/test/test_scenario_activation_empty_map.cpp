#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include <manyforge_deployment/canonical_value.hpp>
#include <manyforge_deployment/retained_bundle.hpp>
#include <manyforge_deployment/verified_retained_bundle.hpp>

#include "activation_deployment_intent_internal.hpp"
#include "prepared_activation_materialization_internal.hpp"

namespace {

using manyforge::deployment::CanonicalValue;
namespace detail = manyforge::deployment::detail;

// Close and New are an ordinary Apply of the empty cell: a Scenario whose
// Deployment has no component goes through the same activation materializer
// as prepare_activation. Its candidate must carry the empty model map (so a
// zero-role Program can bind), while the Manager's startup empty-cell
// predecessor published beside it keeps none (Q125).
TEST(ScenarioActivationEmptyCell,
     CandidateCarriesTheEmptyMapAndSealedPredecessorDoesNot) {
  const CanonicalValue intent_value(CanonicalValue::Object{
      {"api_version", CanonicalValue("manyforge.deployment_intent.v2")},
      {"deployment_artifact_id", CanonicalValue("empty_scenario_activation")},
      {"name", CanonicalValue("empty_scenario_activation")},
      {"attachments", CanonicalValue(CanonicalValue::Array{})},
      {"components", CanonicalValue(CanonicalValue::Array{})},
      {"policies", CanonicalValue(CanonicalValue::Object{})},
  });
  const auto intent =
      detail::parse_activation_deployment_intent(intent_value, {});

  const auto materialized = detail::materialize_prepared_activation(
      std::filesystem::path(MANYFORGE_WORKSPACE_ROOT), {}, intent,
      manyforge::core::CellId("empty_scenario_cell"), {},
      /*prepare_canonical_empty=*/true);

  ASSERT_NE(materialized.bundle, nullptr);
  const auto &candidate = materialized.bundle->value();
  EXPECT_TRUE(candidate.cell_graph.controllable_components.empty());
  EXPECT_TRUE(candidate.cell_graph.model_instances.empty());
  ASSERT_EQ(candidate.compiled_products.symbol_role_maps.size(), 1U);
  ASSERT_TRUE(materialized.model_map.has_value());
  EXPECT_EQ(materialized.model_map->query().product_id,
            candidate.compiled_products.symbol_role_maps.front().product_id);

  ASSERT_NE(materialized.canonical_empty_bundle, nullptr);
  EXPECT_TRUE(materialized.canonical_empty_bundle->value()
                  .compiled_products.symbol_role_maps.empty());
  EXPECT_NE(materialized.canonical_empty_bundle->retained_bundle_id(),
            materialized.bundle->retained_bundle_id());
}

} // namespace

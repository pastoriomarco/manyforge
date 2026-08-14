#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <string_view>

namespace manyforge::core {
namespace {

constexpr std::string_view kManifestRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";

[[nodiscard]] LifecycleFactValueDTO Token(std::string value) {
  return {LifecycleFactValueKindDTO::kToken, std::nullopt, std::nullopt, std::move(value)};
}

[[nodiscard]] LifecyclePredicateDTO Predicate(
    ComponentLifecycleStateDTO state,
    std::optional<std::pair<std::string, std::string>> fact = std::nullopt) {
  std::vector<LifecycleFactPredicateDTO> facts;
  if (fact.has_value()) {
    facts.push_back({LifecycleFactId(fact->first), Token(fact->second)});
  }
  return {{state}, std::move(facts)};
}

[[nodiscard]] BrandLifecycleObservationDTO Observation(
    ComponentLifecycleStateDTO state = ComponentLifecycleStateDTO::kDeclared,
    std::vector<LifecycleFactDTO> facts = {{LifecycleFactId("mode"), Token("automatic")}}) {
  return {LifecycleTransactionId("transaction.1"),
          LifecycleTransactionGeneration(4U),
          ControllableComponentId("component.1"),
          ControlDomainId("domain.1"),
          ControlDomainRevision("domain-revision.1"),
          ProviderInstanceId("provider.1"),
          ProviderManifestId("manifest.1"),
          ProviderManifestRevision(std::string(kManifestRevision)),
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          5U,
          7U,
          10,
          20,
          30,
          state,
          std::move(facts)};
}

[[nodiscard]] LifecyclePrimitiveTransitionDTO Transition(
    std::string id, LifecyclePredicateDTO eligibility,
    ComponentLifecycleStateDTO postcondition_state) {
  return {LifecyclePrimitiveId(std::move(id)), std::move(eligibility),
          Predicate(postcondition_state)};
}

[[nodiscard]] LifecycleIntentPlanDTO FixedPlan(
    std::vector<LifecyclePrimitiveTransitionDTO> transitions) {
  return {ComponentLifecycleIntentKindDTO::kPrepare,
          ComponentLifecycleStateDTO::kPrepared,
          std::nullopt,
          Predicate(ComponentLifecycleStateDTO::kPrepared),
          1'000,
          std::move(transitions)};
}

[[nodiscard]] LifecycleIntentPlanDTO RecoverPlan(ComponentLifecycleStateDTO target,
                                                 LifecyclePredicateDTO eligibility) {
  return {ComponentLifecycleIntentKindDTO::kRecover,
          target,
          std::move(eligibility),
          Predicate(target),
          1'000,
          {}};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO Catalog(std::vector<LifecycleIntentPlanDTO> plans) {
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(std::string(kManifestRevision)),
      std::move(plans));
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().message;
    return {ProviderManifestId("manifest.1"),
            ProviderManifestRevision(std::string(kManifestRevision)),
            ContentDigest("invalid"),
            {}};
  }
  return std::move(catalog.value());
}

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO Occurrence(
    const BrandLifecyclePlanCatalogDTO& catalog) {
  return {ControlDomainId("domain.1"),
          ControlDomainRevision("domain-revision.1"),
          ProviderInstanceId("provider.1"),
          ProviderManifestId("manifest.1"),
          ProviderManifestRevision(std::string(kManifestRevision)),
          RuntimeUnitId("runtime.1"),
          RuntimeEpoch("runtime-epoch.1"),
          catalog.catalog_digest};
}

TEST(ComponentLifecyclePlanEvaluator, AlreadySatisfiedSelectsNoPrimitive) {
  const auto catalog =
      Catalog({FixedPlan({Transition("prepare", Predicate(ComponentLifecycleStateDTO::kDeclared),
                                     ComponentLifecycleStateDTO::kPrepared)})});

  const auto decision = EvaluateLifecycleDomainPlan(
      ComponentLifecycleIntentKindDTO::kPrepare, ComponentLifecycleStateDTO::kPrepared, catalog,
      Observation(ComponentLifecycleStateDTO::kPrepared));

  ASSERT_TRUE(decision.ok()) << decision.error().message;
  EXPECT_EQ(decision.value().kind, LifecycleDomainPlanDecisionKind::kAlreadySatisfied);
  EXPECT_FALSE(decision.value().selected_transition.has_value());
}

TEST(ComponentLifecyclePlanEvaluator, SelectsOnlyUniqueConclusivePrimitive) {
  const auto catalog = Catalog(
      {FixedPlan({Transition("prepare-auto",
                             Predicate(ComponentLifecycleStateDTO::kDeclared,
                                       std::pair{std::string("mode"), std::string("automatic")}),
                             ComponentLifecycleStateDTO::kPrepared),
                  Transition("prepare-manual",
                             Predicate(ComponentLifecycleStateDTO::kDeclared,
                                       std::pair{std::string("mode"), std::string("manual")}),
                             ComponentLifecycleStateDTO::kPrepared)})});

  const auto decision =
      EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                  ComponentLifecycleStateDTO::kPrepared, catalog, Observation());

  ASSERT_TRUE(decision.ok()) << decision.error().message;
  ASSERT_TRUE(decision.value().selected_transition.has_value());
  EXPECT_EQ(decision.value().selected_transition->lifecycle_primitive_id,
            LifecyclePrimitiveId("prepare-auto"));
}

TEST(ComponentLifecyclePlanEvaluator, AmbiguousOrInconclusiveTransitionFailsClosed) {
  auto catalog =
      Catalog({FixedPlan({Transition("first", Predicate(ComponentLifecycleStateDTO::kDeclared),
                                     ComponentLifecycleStateDTO::kPrepared),
                          Transition("second", Predicate(ComponentLifecycleStateDTO::kDeclared),
                                     ComponentLifecycleStateDTO::kPrepared)})});
  auto decision =
      EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                  ComponentLifecycleStateDTO::kPrepared, catalog, Observation());
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error().code, "component_lifecycle_transition_ambiguous");

  catalog = Catalog(
      {FixedPlan({Transition("selected", Predicate(ComponentLifecycleStateDTO::kDeclared),
                             ComponentLifecycleStateDTO::kPrepared),
                  Transition("unknown-mode",
                             Predicate(ComponentLifecycleStateDTO::kDeclared,
                                       std::pair{std::string("missing"), std::string("value")}),
                             ComponentLifecycleStateDTO::kPrepared)})});
  decision =
      EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                  ComponentLifecycleStateDTO::kPrepared, catalog, Observation());
  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error().code, "component_lifecycle_transition_inconclusive");
}

TEST(ComponentLifecyclePlanEvaluator, RejectsCatalogContentBehindAStaleDeclaredDigest) {
  auto catalog =
      Catalog({FixedPlan({Transition("prepare", Predicate(ComponentLifecycleStateDTO::kDeclared),
                                     ComponentLifecycleStateDTO::kPrepared)})});
  catalog.plans.front().timeout_duration_ns += 1;

  const auto decision =
      EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                  ComponentLifecycleStateDTO::kPrepared, catalog, Observation());

  ASSERT_FALSE(decision.ok());
  EXPECT_EQ(decision.error().code, "component_lifecycle_plan_evaluation_invalid");
}

TEST(ComponentLifecyclePlanEvaluator, SelectsExactlyOneCommonRecoverTarget) {
  const auto catalog =
      Catalog({RecoverPlan(ComponentLifecycleStateDTO::kDeclared,
                           Predicate(ComponentLifecycleStateDTO::kFaulted,
                                     std::pair{std::string("mode"), std::string("manual")})),
               RecoverPlan(ComponentLifecycleStateDTO::kPrepared,
                           Predicate(ComponentLifecycleStateDTO::kFaulted,
                                     std::pair{std::string("mode"), std::string("automatic")}))});
  auto observation = Observation(ComponentLifecycleStateDTO::kFaulted);

  const auto target = SelectCommonRecoverTarget(
      {{Occurrence(catalog), catalog, observation}, {Occurrence(catalog), catalog, observation}});

  ASSERT_TRUE(target.ok()) << target.error().message;
  EXPECT_EQ(target.value(), ComponentLifecycleStateDTO::kPrepared);
}

TEST(ComponentLifecyclePlanEvaluator, RecoverAmbiguityAndInconclusivenessFailClosed) {
  auto catalog = Catalog({RecoverPlan(ComponentLifecycleStateDTO::kDeclared,
                                      Predicate(ComponentLifecycleStateDTO::kFaulted)),
                          RecoverPlan(ComponentLifecycleStateDTO::kPrepared,
                                      Predicate(ComponentLifecycleStateDTO::kFaulted))});
  auto target = SelectCommonRecoverTarget(
      {{Occurrence(catalog), catalog, Observation(ComponentLifecycleStateDTO::kFaulted)}});
  ASSERT_FALSE(target.ok());
  EXPECT_EQ(target.error().code, "component_lifecycle_recover_target_ambiguous");

  catalog =
      Catalog({RecoverPlan(ComponentLifecycleStateDTO::kDeclared,
                           Predicate(ComponentLifecycleStateDTO::kFaulted,
                                     std::pair{std::string("missing"), std::string("value")})),
               RecoverPlan(ComponentLifecycleStateDTO::kPrepared,
                           Predicate(ComponentLifecycleStateDTO::kFaulted,
                                     std::pair{std::string("mode"), std::string("manual")}))});
  target = SelectCommonRecoverTarget(
      {{Occurrence(catalog), catalog, Observation(ComponentLifecycleStateDTO::kFaulted)}});
  ASSERT_FALSE(target.ok());
  EXPECT_EQ(target.error().code, "component_lifecycle_recover_target_inconclusive");
}

TEST(ComponentLifecyclePlanEvaluator,
     IndependentUrShapedFakeMappingIsDeclarativeAndUnsupportedOperationsFailClosed) {
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("fake.ur.lifecycle-provider"),
      ProviderManifestRevision(
          "sha256:2222222222222222222222222222222222222222222222222222222222222222"),
      {{ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        Predicate(ComponentLifecycleStateDTO::kPrepared,
                  std::pair{std::string("robot_mode"), std::string("running")}),
        1'000,
        {Transition("ur.power_on_and_release_brakes",
                    Predicate(ComponentLifecycleStateDTO::kDeclared,
                              std::pair{std::string("robot_mode"), std::string("power_off")}),
                    ComponentLifecycleStateDTO::kPrepared)}},
       {ComponentLifecycleIntentKindDTO::kDisable,
        ComponentLifecycleStateDTO::kDisabled,
        std::nullopt,
        Predicate(ComponentLifecycleStateDTO::kDisabled,
                  std::pair{std::string("robot_mode"), std::string("power_off")}),
        1'000,
        {Transition("ur.stop_and_power_off", Predicate(ComponentLifecycleStateDTO::kActive),
                    ComponentLifecycleStateDTO::kDisabled)}}});
  ASSERT_TRUE(catalog.ok()) << catalog.error().message;

  auto observation = Observation(ComponentLifecycleStateDTO::kDeclared,
                                 {{LifecycleFactId("robot_mode"), Token("power_off")}});
  observation.provider_manifest_id = catalog.value().provider_manifest_id;
  observation.provider_manifest_revision = catalog.value().provider_manifest_revision;

  const auto prepare = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                                   ComponentLifecycleStateDTO::kPrepared,
                                                   catalog.value(), observation);
  ASSERT_TRUE(prepare.ok()) << prepare.error().message;
  ASSERT_TRUE(prepare.value().selected_transition.has_value());
  EXPECT_EQ(prepare.value().selected_transition->lifecycle_primitive_id,
            LifecyclePrimitiveId("ur.power_on_and_release_brakes"));

  const auto unsupported = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kEnable,
                                                       ComponentLifecycleStateDTO::kActive,
                                                       catalog.value(), observation);
  ASSERT_FALSE(unsupported.ok());
  EXPECT_EQ(unsupported.error().code, "component_lifecycle_intent_unsupported");
  EXPECT_EQ(unsupported.error().details.at("reason"), "selected_plan_missing");
}

}  // namespace
}  // namespace manyforge::core

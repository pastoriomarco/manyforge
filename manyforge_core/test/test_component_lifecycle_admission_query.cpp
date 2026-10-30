#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_admission_query.hpp>
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
    std::vector<ComponentLifecycleStateDTO> states,
    std::optional<std::pair<std::string, std::string>> fact = std::nullopt) {
  std::vector<LifecycleFactPredicateDTO> facts;
  if (fact.has_value()) facts.push_back({LifecycleFactId(fact->first), Token(fact->second)});
  return {std::move(states), std::move(facts)};
}

[[nodiscard]] BrandLifecycleObservationDTO Observation(
    ComponentLifecycleStateDTO state,
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

[[nodiscard]] LifecycleDomainHealthDTO Health(const BrandLifecycleObservationDTO& observation) {
  return {observation.observed_state, observation.facts, std::nullopt};
}

[[nodiscard]] LifecyclePrimitiveTransitionDTO Transition(std::string id,
                                                         LifecyclePredicateDTO eligibility,
                                                         ComponentLifecycleStateDTO post) {
  return {LifecyclePrimitiveId(std::move(id)), std::move(eligibility), Predicate({post})};
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

// A Prepare plan with two mode-selected transitions, an inconclusive one
// (`arm` may be missing), a Disable plan, and Recover plans for the unarmed
// resting rule and target selection.
[[nodiscard]] BrandLifecyclePlanCatalogDTO MixedCatalog() {
  using State = ComponentLifecycleStateDTO;
  return Catalog(
      {{ComponentLifecycleIntentKindDTO::kPrepare,
        State::kPrepared,
        std::nullopt,
        Predicate({State::kPrepared}, std::pair{std::string("mode"), std::string("automatic")}),
        1'000,
        {Transition("prepare-auto",
                    Predicate({State::kDeclared},
                              std::pair{std::string("mode"), std::string("automatic")}),
                    State::kPrepared),
         Transition(
             "prepare-armed",
             Predicate({State::kDeclared}, std::pair{std::string("arm"), std::string("armed")}),
             State::kPrepared)}},
       {ComponentLifecycleIntentKindDTO::kDisable,
        State::kDisabled,
        std::nullopt,
        Predicate({State::kDisabled}),
        1'000,
        {Transition("disable", Predicate({State::kActive}), State::kDisabled)}},
       {ComponentLifecycleIntentKindDTO::kRecover,
        State::kDeclared,
        Predicate({State::kFaulted}),
        Predicate({State::kDeclared}),
        1'000,
        {}}});
}

// The adapter's answer must carry the evaluator's decision and its exact
// refusal; only an invalid input stays an error.
template <typename Input>
void ExpectSameAsEvaluator(const BrandLifecyclePlanCatalogDTO& catalog,
                           ComponentLifecycleIntentKindDTO intent,
                           ComponentLifecycleStateDTO requested, const Input& input) {
  const auto decision = EvaluateLifecycleDomainPlan(intent, requested, catalog, input);
  const auto answer = CatalogLifecycleAdmission(catalog).Evaluate(intent, requested, input);
  if (!decision.ok() && decision.error().code == "component_lifecycle_plan_evaluation_invalid") {
    ASSERT_FALSE(answer.ok());
    EXPECT_EQ(answer.error(), decision.error());
    return;
  }
  ASSERT_TRUE(answer.ok()) << answer.error().message;
  if (!decision.ok()) {
    EXPECT_EQ(answer.value().not_ready, decision.error());
    EXPECT_NE(answer.value().postcondition, LifecyclePostconditionResultDTO::kSatisfied);
    EXPECT_FALSE(answer.value().selected_substep);
    return;
  }
  EXPECT_FALSE(answer.value().not_ready);
  EXPECT_EQ(answer.value().postcondition == LifecyclePostconditionResultDTO::kSatisfied,
            decision.value().kind == LifecycleDomainPlanDecisionKind::kAlreadySatisfied);
  EXPECT_EQ(answer.value().selected_substep, decision.value().selected_transition);
}

TEST(CatalogLifecycleAdmission, AnswersExactlyAsThePlanEvaluator) {
  using State = ComponentLifecycleStateDTO;
  using Intent = ComponentLifecycleIntentKindDTO;
  const auto catalog = MixedCatalog();
  const std::vector<BrandLifecycleObservationDTO> inputs{
      // Already done; ready (prepare-auto); refused with decisive facts.
      Observation(State::kPrepared),
      Observation(State::kDeclared, {{LifecycleFactId("arm"), Token("idle")},
                                     {LifecycleFactId("mode"), Token("automatic")}}),
      Observation(State::kDeclared, {{LifecycleFactId("arm"), Token("idle")},
                                     {LifecycleFactId("mode"), Token("manual")}}),
      // Inconclusive transition and inconclusive postcondition.
      Observation(State::kDeclared, {{LifecycleFactId("mode"), Token("manual")}}),
      Observation(State::kPrepared, {}), Observation(State::kActive)};
  for (const auto& observation : inputs) {
    ExpectSameAsEvaluator(catalog, Intent::kPrepare, State::kPrepared, observation);
    ExpectSameAsEvaluator(catalog, Intent::kPrepare, State::kPrepared, Health(observation));
    ExpectSameAsEvaluator(catalog, Intent::kDisable, State::kDisabled, observation);
    ExpectSameAsEvaluator(catalog, Intent::kEnable, State::kActive, observation);
  }
  auto invalid = Observation(State::kDeclared);
  invalid.source_sequence = 0U;
  ExpectSameAsEvaluator(catalog, Intent::kPrepare, State::kPrepared, invalid);
}

TEST(CatalogLifecycleAdmission, PostconditionAnswerIsThreeWay) {
  using State = ComponentLifecycleStateDTO;
  const CatalogLifecycleAdmission admission(MixedCatalog());
  const auto prepare = [&](const BrandLifecycleObservationDTO& observation) {
    return admission.Evaluate(ComponentLifecycleIntentKindDTO::kPrepare, State::kPrepared,
                              observation);
  };
  auto answer = prepare(Observation(State::kPrepared));
  ASSERT_TRUE(answer.ok());
  EXPECT_EQ(answer.value().postcondition, LifecyclePostconditionResultDTO::kSatisfied);

  answer = prepare(Observation(State::kDeclared, {{LifecycleFactId("arm"), Token("idle")},
                                                  {LifecycleFactId("mode"), Token("automatic")}}));
  ASSERT_TRUE(answer.ok());
  EXPECT_EQ(answer.value().postcondition, LifecyclePostconditionResultDTO::kNotSatisfied);
  ASSERT_TRUE(answer.value().selected_substep);
  EXPECT_EQ(answer.value().selected_substep->lifecycle_primitive_id,
            LifecyclePrimitiveId("prepare-auto"));

  // The requested postcondition's own fact is missing: inconclusive, not ready.
  answer = prepare(Observation(State::kPrepared, {}));
  ASSERT_TRUE(answer.ok());
  EXPECT_EQ(answer.value().postcondition, LifecyclePostconditionResultDTO::kInconclusive);
  ASSERT_TRUE(answer.value().not_ready);
  EXPECT_EQ(answer.value().not_ready->code, "component_lifecycle_postcondition_inconclusive");

  // No plan for the intent: not satisfied, not ready with the evaluator's code.
  answer = admission.Evaluate(ComponentLifecycleIntentKindDTO::kEnable, State::kActive,
                              Observation(State::kPrepared));
  ASSERT_TRUE(answer.ok());
  EXPECT_EQ(answer.value().postcondition, LifecyclePostconditionResultDTO::kNotSatisfied);
  ASSERT_TRUE(answer.value().not_ready);
  EXPECT_EQ(answer.value().not_ready->code, "component_lifecycle_intent_unsupported");
}

// The "unarmed resting = already Disabled" rule is part of the answer.
TEST(CatalogLifecycleAdmission, UnarmedRestingDeclaredIsAlreadyDisabled) {
  using State = ComponentLifecycleStateDTO;
  const CatalogLifecycleAdmission admission(MixedCatalog());
  for (const auto& answer :
       {admission.Evaluate(ComponentLifecycleIntentKindDTO::kDisable, State::kDisabled,
                           Observation(State::kDeclared)),
        admission.Evaluate(ComponentLifecycleIntentKindDTO::kDisable, State::kDisabled,
                           Health(Observation(State::kDeclared)))}) {
    ASSERT_TRUE(answer.ok()) << answer.error().message;
    EXPECT_EQ(answer.value().postcondition, LifecyclePostconditionResultDTO::kSatisfied);
    EXPECT_FALSE(answer.value().not_ready);
  }
}

TEST(CatalogLifecycleAdmission, RecoverTargetsSelectTodaysCommonTarget) {
  using State = ComponentLifecycleStateDTO;
  const auto recover = [](State target, LifecyclePredicateDTO eligibility) {
    return LifecycleIntentPlanDTO{ComponentLifecycleIntentKindDTO::kRecover,
                                  target,
                                  std::move(eligibility),
                                  Predicate({target}),
                                  1'000,
                                  {}};
  };
  const CatalogLifecycleAdmission arm(Catalog(
      {recover(State::kDeclared,
               Predicate({State::kFaulted}, std::pair{std::string("mode"), std::string("manual")})),
       recover(State::kPrepared, Predicate({State::kFaulted}, std::pair{std::string("mode"),
                                                                        std::string("automatic")})),
       recover(State::kDisabled, Predicate({State::kFaulted}))}));
  const CatalogLifecycleAdmission gripper(
      Catalog({recover(State::kPrepared, Predicate({State::kFaulted}))}));
  const auto faulted = Observation(State::kFaulted);

  const auto arm_targets = arm.RecoverTargets(faulted);
  ASSERT_TRUE(arm_targets.ok());
  EXPECT_EQ(arm_targets.value(), EvaluateLifecycleRecoverTargets(arm.catalog(), faulted).value());
  EXPECT_EQ(arm_targets.value(), (std::array{LifecyclePostconditionResultDTO::kNotSatisfied,
                                             LifecyclePostconditionResultDTO::kSatisfied,
                                             LifecyclePostconditionResultDTO::kSatisfied}));
  // Alone the arm is ambiguous; with the gripper only Prepared is common.
  auto target = SelectCommonRecoverTarget({arm_targets.value()});
  ASSERT_FALSE(target.ok());
  EXPECT_EQ(target.error().code, "component_lifecycle_recover_target_ambiguous");
  target =
      SelectCommonRecoverTarget({arm_targets.value(), gripper.RecoverTargets(faulted).value()});
  ASSERT_TRUE(target.ok()) << target.error().message;
  EXPECT_EQ(target.value(), State::kPrepared);

  // A healthy observation excludes every target.
  target = SelectCommonRecoverTarget({arm.RecoverTargets(Observation(State::kDeclared)).value()});
  ASSERT_FALSE(target.ok());
  EXPECT_EQ(target.error().code, "component_lifecycle_recover_target_missing");

  auto invalid = faulted;
  invalid.observation_generation = 0U;
  const auto refused = arm.RecoverTargets(invalid);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "component_lifecycle_recover_selection_invalid");
}

}  // namespace
}  // namespace manyforge::core

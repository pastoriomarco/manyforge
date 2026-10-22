#include <gtest/gtest.h>

#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/types/lifecycle_cause.hpp>
#include <map>
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

[[nodiscard]] LifecycleFactValueDTO Boolean(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt};
}

[[nodiscard]] std::vector<LifecycleFactDTO> PrepareFacts(bool loaded = true, bool fault = false,
                                                       bool transport = true) {
  return {{LifecycleFactId("external.controller_loaded"), Boolean(loaded)},
          {LifecycleFactId("external.fault_present"), Boolean(fault)},
          {LifecycleFactId("external.transport_ready"), Boolean(transport)}};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO PrepareCatalog() {
  const auto facts = PrepareFacts();
  std::vector<LifecycleFactPredicateDTO> predicates;
  for (const auto& fact : facts) {
    predicates.push_back({fact.lifecycle_fact_id, fact.value});
  }
  return Catalog(
      {{ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared,
        std::nullopt,
        {{ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kActive}, predicates},
        1'000,
        {{LifecyclePrimitiveId("prepare"),
          {{ComponentLifecycleStateDTO::kDeclared, ComponentLifecycleStateDTO::kDisabled},
           predicates},
          {{ComponentLifecycleStateDTO::kPrepared, ComponentLifecycleStateDTO::kActive},
           predicates}}}}});
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
  std::get<std::int64_t>(catalog.plans.front().timeout_policy) += 1;

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

TEST(ComponentLifecyclePlanEvaluator, FailedPrepareNamesTheSameObservationWithoutChangingIt) {
  const auto catalog = PrepareCatalog();
  const std::vector<BrandLifecycleObservationDTO> observations{
      Observation(ComponentLifecycleStateDTO::kDeclared, PrepareFacts(false, false, true)),
      Observation(ComponentLifecycleStateDTO::kDeclared, PrepareFacts(true, true, true)),
      Observation(ComponentLifecycleStateDTO::kDeclared, PrepareFacts(true, false, false)),
      Observation(ComponentLifecycleStateDTO::kFaulted, PrepareFacts())};
  const std::vector<std::vector<std::string>> expected{{"declared", "false", "false", "true"},
                                                       {"declared", "true", "true", "true"},
                                                       {"declared", "true", "false", "false"},
                                                       {"faulted", "true", "false", "true"}};
  for (std::size_t index = 0; index < observations.size(); ++index) {
    SCOPED_TRACE(index);
    const auto original = observations[index];
    ASSERT_TRUE(original.locally_valid());
    const auto result = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                                    ComponentLifecycleStateDTO::kPrepared, catalog,
                                                    observations[index]);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, "component_lifecycle_transition_missing");
    EXPECT_EQ(result.error().message,
              "no lifecycle primitive is eligible for the fresh observation");
    const std::map<std::string, std::string> details{
        {"reason", "no_transition_satisfied"},
        {"observed_state", expected[index][0]},
        {"observed_controller_loaded", expected[index][1]},
        {"observed_fault_present", expected[index][2]},
        {"observed_transport_ready", expected[index][3]},
        {"observed_blocking_cause", "missing"}};
    EXPECT_EQ(result.error().details, details);
    EXPECT_LE(result.error().details.size(), kMaxLifecycleErrorDetails);
    EXPECT_EQ(observations[index], original);
  }
}

TEST(ComponentLifecyclePlanEvaluator, FailureDistinguishesMissingUnknownAndNonbooleanFacts) {
  const auto catalog = PrepareCatalog();
  for (const std::string marker : {"missing", "unknown", "nonboolean"}) {
    SCOPED_TRACE(marker);
    auto facts = PrepareFacts();
    if (marker == "missing") {
      facts.erase(facts.begin());
    } else if (marker == "unknown") {
      facts.front().value = {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt,
                             std::nullopt};
    } else {
      facts.front().value = Token("private-token-must-not-be-copied");
    }
    const auto observation = Observation(ComponentLifecycleStateDTO::kDeclared, std::move(facts));
    ASSERT_TRUE(observation.locally_valid());
    const auto original = observation;
    const auto result =
        EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                    ComponentLifecycleStateDTO::kPrepared, catalog, observation);
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.error().code, marker == "nonboolean"
                                       ? "component_lifecycle_transition_missing"
                                       : "component_lifecycle_transition_inconclusive");
    EXPECT_EQ(result.error().details.at("reason"), marker == "nonboolean"
                                                       ? "no_transition_satisfied"
                                                       : "transition_eligibility_inconclusive");
    EXPECT_EQ(result.error().details.at("observed_controller_loaded"), marker);
    EXPECT_EQ(result.error().details.at("observed_fault_present"), "false");
    EXPECT_EQ(result.error().details.at("observed_transport_ready"), "true");
    EXPECT_EQ(result.error().details.size(), 6U);
    EXPECT_EQ(observation, original);
  }
}

TEST(ComponentLifecyclePlanEvaluator, FailureNamesStandardBlockingCauseAndDiagnosticCode) {
  // The I/O driver publishes io.driver.* (not external.*) facts; they fill the
  // same keys, and the standard cause token and bounded diagnostic explain.
  const auto catalog = Catalog({FixedPlan({{LifecyclePrimitiveId("io.provider.prepare"),
      {{ComponentLifecycleStateDTO::kDeclared},
       {{LifecycleFactId("io.driver.transport_ready"), Boolean(true)}}},
      Predicate(ComponentLifecycleStateDTO::kPrepared)}})});
  const LifecycleDomainHealthDTO health{
      ComponentLifecycleStateDTO::kDeclared,
      {{LifecycleFactId("io.driver.enabled"), Boolean(false)},
       {LifecycleFactId("io.driver.fault_present"), Boolean(false)},
       {LifecycleFactId("io.driver.resources_ready"), Boolean(true)},
       {LifecycleFactId("io.driver.transport_ready"), Boolean(false)},
       {LifecycleFactId(std::string(kLifecycleBlockingCauseFactId)), Token("availability")}},
      ErrorDTO{"io_provider_transport_unavailable", "ROS service /plc_io/start is not available",
               {{"ros_name", "/plc_io/start"}}}};
  ASSERT_TRUE(health.locally_valid());
  const auto before = health;
  const auto result = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared, catalog, Occurrence(catalog), health);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_lifecycle_transition_missing");
  const std::map<std::string, std::string> details{
      {"reason", "no_transition_satisfied"},
      {"observed_state", "declared"},
      {"observed_controller_loaded", "missing"},
      {"observed_fault_present", "false"},
      {"observed_transport_ready", "false"},
      {"observed_resources_ready", "true"},
      {"observed_blocking_cause", "availability"},
      {"observed_diagnostic_code", "io_provider_transport_unavailable"}};
  EXPECT_EQ(result.error().details, details);
  EXPECT_LE(result.error().details.size(), kMaxLifecycleErrorDetails);
  EXPECT_EQ(health, before);
}

TEST(ComponentLifecyclePlanEvaluator, RobotFailureKeepsExternalKeysAndAddsCauseAndDiagnostic) {
  const auto catalog = PrepareCatalog();
  auto facts = PrepareFacts(true, true, true);
  facts.push_back({LifecycleFactId(std::string(kLifecycleBlockingCauseFactId)), Token("fault")});
  const LifecycleDomainHealthDTO health{ComponentLifecycleStateDTO::kDeclared, std::move(facts),
      ErrorDTO{"physical_fault", "drive fault", {{"reason", "guard_trip"}}}};
  ASSERT_TRUE(health.locally_valid());
  const auto result = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared, catalog, Occurrence(catalog), health);
  ASSERT_FALSE(result.ok());
  const std::map<std::string, std::string> details{
      {"reason", "no_transition_satisfied"},
      {"observed_state", "declared"},
      {"observed_controller_loaded", "true"},
      {"observed_fault_present", "true"},
      {"observed_transport_ready", "true"},
      {"observed_blocking_cause", "fault"},
      {"observed_diagnostic_code", "physical_fault"}};
  EXPECT_EQ(result.error().details, details);
}

TEST(ComponentLifecyclePlanEvaluator, EligiblePrepareAndInvalidObservationKeepExistingDecisions) {
  const auto catalog = PrepareCatalog();
  auto observation = Observation(ComponentLifecycleStateDTO::kDeclared, PrepareFacts());
  const auto original = observation;
  auto result =
      EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                  ComponentLifecycleStateDTO::kPrepared, catalog, observation);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().kind, LifecycleDomainPlanDecisionKind::kDispatchPrimitive);
  ASSERT_TRUE(result.value().selected_transition.has_value());
  EXPECT_EQ(result.value().selected_transition->lifecycle_primitive_id,
            LifecyclePrimitiveId("prepare"));
  EXPECT_EQ(observation, original);

  observation.received_timestamp_ns = observation.fresh_until_ns;
  result = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                       ComponentLifecycleStateDTO::kPrepared, catalog, observation);
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().code, "component_lifecycle_plan_evaluation_invalid");
  const std::map<std::string, std::string> details{{"reason", "catalog_or_observation_invalid"}};
  EXPECT_EQ(result.error().details, details);
}

TEST(ComponentLifecyclePlanEvaluator, PureHealthUsesSameDecisionsAndOriginalFailureDetails) {
  const auto catalog = PrepareCatalog();
  const auto occurrence = Occurrence(catalog);
  const std::vector<BrandLifecycleObservationDTO> cases{
      Observation(ComponentLifecycleStateDTO::kDeclared, PrepareFacts()),
      Observation(ComponentLifecycleStateDTO::kPrepared, PrepareFacts()),
      Observation(ComponentLifecycleStateDTO::kDisabled, PrepareFacts()),
      Observation(ComponentLifecycleStateDTO::kDegraded, PrepareFacts(false)),
      Observation(ComponentLifecycleStateDTO::kFaulted, PrepareFacts(true, true)),
      Observation(ComponentLifecycleStateDTO::kDeclared,
                  {{LifecycleFactId("external.controller_loaded"),
                    {LifecycleFactValueKindDTO::kUnknown, std::nullopt, std::nullopt, std::nullopt}},
                   {LifecycleFactId("external.fault_present"), Boolean(false)},
                   {LifecycleFactId("external.transport_ready"), Boolean(true)}})};
  for (const auto& observation : cases) {
    const LifecycleDomainHealthDTO health{observation.observed_state, observation.facts,
                                        observation.diagnostic};
    const auto before = health;
    const auto execution = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared, catalog, observation);
    const auto projection = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared, catalog, occurrence, health);
    ASSERT_EQ(projection.ok(), execution.ok());
    if (execution.ok()) {
      EXPECT_EQ(projection.value().kind, execution.value().kind);
      EXPECT_EQ(projection.value().selected_plan, execution.value().selected_plan);
      EXPECT_EQ(projection.value().selected_transition, execution.value().selected_transition);
    } else {
      EXPECT_EQ(projection.error(), execution.error());
      EXPECT_LE(projection.error().details.size(), kMaxLifecycleErrorDetails);
    }
    EXPECT_EQ(health, before);
  }
}

TEST(ComponentLifecyclePlanEvaluator, PureHealthKeepsCustomFactsAndExactCatalogQualification) {
  const auto catalog = Catalog({FixedPlan({Transition("prepare-custom",
      Predicate(ComponentLifecycleStateDTO::kDeclared,
                std::pair{std::string("mode"), std::string("automatic")}),
      ComponentLifecycleStateDTO::kPrepared)})});
  const auto observation = Observation();
  const LifecycleDomainHealthDTO health{observation.observed_state, observation.facts,
                                      ErrorDTO{"source_diagnostic", "retained", {{"reason", "monitor_only"}}}};
  const auto occurrence = Occurrence(catalog);
  const auto decision = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared, catalog, occurrence, health);
  ASSERT_TRUE(decision.ok());
  ASSERT_TRUE(decision.value().selected_transition);
  EXPECT_EQ(decision.value().selected_transition->lifecycle_primitive_id,
            LifecyclePrimitiveId("prepare-custom"));
  for (int mutation = 0; mutation < 4; ++mutation) {
    auto foreign = occurrence;
    auto malformed = health;
    if (mutation == 0) foreign.provider_manifest_id = ProviderManifestId("foreign");
    if (mutation == 1) foreign.lifecycle_plan_catalog_digest = ContentDigest(std::string("sha256:") + std::string(64, 'f'));
    if (mutation == 2) foreign.provider_runtime_epoch = RuntimeEpoch("");
    if (mutation == 3) malformed.facts.push_back(malformed.facts.front());
    const auto refused = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
        ComponentLifecycleStateDTO::kPrepared, catalog, foreign, malformed);
    ASSERT_FALSE(refused.ok());
    EXPECT_EQ(refused.error().code, "component_lifecycle_plan_evaluation_invalid");
    EXPECT_EQ(refused.error().details.size(), 1U);
  }
  auto invalid_execution = observation;
  invalid_execution.lifecycle_transaction_id = LifecycleTransactionId("");
  const auto refused = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
      ComponentLifecycleStateDTO::kPrepared, catalog, invalid_execution);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "catalog_or_observation_invalid");
}

TEST(ComponentLifecyclePlanEvaluator, ActiveHealthIsDiagnosticAndNeverInventsNextEnable) {
  const auto no_fault = LifecycleFactPredicateDTO{
      LifecycleFactId("fault"), Boolean(false)};
  const auto catalog = Catalog({{
      ComponentLifecycleIntentKindDTO::kEnable, ComponentLifecycleStateDTO::kActive,
      std::nullopt, {{ComponentLifecycleStateDTO::kActive}, {no_fault}}, 1'000,
      {{LifecyclePrimitiveId("enable"),
        {{ComponentLifecycleStateDTO::kPrepared}, {no_fault}},
        {{ComponentLifecycleStateDTO::kActive}, {no_fault}}}}}});
  const LifecycleDomainHealthDTO healthy{ComponentLifecycleStateDTO::kActive,
      {{LifecycleFactId("fault"), Boolean(false)}}, std::nullopt};
  const auto positive = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kEnable,
      ComponentLifecycleStateDTO::kActive, catalog, Occurrence(catalog), healthy);
  ASSERT_TRUE(positive.ok());
  EXPECT_EQ(positive.value().kind, LifecycleDomainPlanDecisionKind::kAlreadySatisfied);
  EXPECT_FALSE(positive.value().selected_transition);
  auto faulted = healthy;
  faulted.facts.front().value = Boolean(true);
  faulted.diagnostic = ErrorDTO{"physical_fault", "real retained cause", {{"reason", "guard_trip"}}};
  const auto before = faulted;
  const auto negative = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kEnable,
      ComponentLifecycleStateDTO::kActive, catalog, Occurrence(catalog), faulted);
  ASSERT_FALSE(negative.ok());
  EXPECT_EQ(negative.error().code, "component_lifecycle_transition_missing");
  EXPECT_EQ(faulted, before);
}

}  // namespace
}  // namespace manyforge::core

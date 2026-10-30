#include <gtest/gtest.h>

#include <functional>
#include <manyforge_core/component_lifecycle_plan_catalog_codec.hpp>
#include <manyforge_core/component_lifecycle_plan_evaluator.hpp>
#include <manyforge_core/component_lifecycle_transition_runner.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <map>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {
namespace {

using State = ComponentLifecycleStateDTO;
using Outcome = LifecycleTransitionOutcomeDTO;

constexpr std::string_view kManifestRevision =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kClear = "device.clear_error";
constexpr std::string_view kHardware = "ros2_control.hardware.activate";
constexpr std::string_view kBroadcaster = "ros2_control.broadcaster.activate";
constexpr std::string_view kSync = "device.tool_context.sync";
constexpr std::int64_t kFreshNs = 1'000'000'000;
constexpr std::int64_t kPollNs = 10'000;

[[nodiscard]] LifecycleFactValueDTO Boolean(bool value) {
  return {LifecycleFactValueKindDTO::kBoolean, value, std::nullopt, std::nullopt};
}

// Fact clauses must be given in canonical (sorted) order.
[[nodiscard]] LifecyclePredicateDTO Predicate(
    State state, std::vector<std::pair<std::string, bool>> facts = {}) {
  std::vector<LifecycleFactPredicateDTO> predicates;
  for (const auto& [id, value] : facts) predicates.push_back({LifecycleFactId(id), Boolean(value)});
  return {{state}, std::move(predicates)};
}

[[nodiscard]] BrandLifecyclePlanCatalogDTO Catalog(LifecycleIntentPlanDTO plan) {
  auto catalog = MakeBrandLifecyclePlanCatalog(
      ProviderManifestId("manifest.1"), ProviderManifestRevision(std::string(kManifestRevision)),
      {std::move(plan)});
  if (!catalog.ok()) {
    ADD_FAILURE() << catalog.error().message;
    return {ProviderManifestId("manifest.1"),
            ProviderManifestRevision(std::string(kManifestRevision)),
            ContentDigest("invalid"),
            {}};
  }
  return std::move(catalog.value());
}

// Shaped like the Real arm Prepare: clear the vendor error, arm the hardware,
// then bring the broadcaster up (V2-3 names).
[[nodiscard]] BrandLifecyclePlanCatalogDTO RealPrepareCatalog() {
  constexpr auto kBroadcasting = "external.broadcaster_active";
  constexpr auto kError = "external.error_present";
  constexpr auto kArmed = "external.hardware_active";
  return Catalog(
      {ComponentLifecycleIntentKindDTO::kPrepare,
       State::kPrepared,
       std::nullopt,
       Predicate(State::kPrepared, {{kBroadcasting, true}}),
       1'000,
       {{LifecyclePrimitiveId(std::string(kClear)), Predicate(State::kDeclared, {{kError, true}}),
         Predicate(State::kDeclared, {{kError, false}})},
        {LifecyclePrimitiveId(std::string(kHardware)),
         Predicate(State::kDeclared, {{kError, false}, {kArmed, false}}),
         Predicate(State::kDeclared, {{kArmed, true}})},
        {LifecyclePrimitiveId(std::string(kBroadcaster)),
         Predicate(State::kDeclared, {{kBroadcasting, false}, {kError, false}, {kArmed, true}}),
         Predicate(State::kPrepared, {{kBroadcasting, true}})}}});
}

// Two steps that undo each other: a plan that never completes.
[[nodiscard]] BrandLifecyclePlanCatalogDTO ToggleCatalog() {
  return Catalog({ComponentLifecycleIntentKindDTO::kPrepare,
                  State::kPrepared,
                  std::nullopt,
                  Predicate(State::kPrepared),
                  1'000,
                  {{LifecyclePrimitiveId("toggle.on"),
                    Predicate(State::kDeclared, {{"external.toggle", false}}),
                    Predicate(State::kDeclared, {{"external.toggle", true}})},
                   {LifecyclePrimitiveId("toggle.off"),
                    Predicate(State::kDeclared, {{"external.toggle", true}}),
                    Predicate(State::kDeclared, {{"external.toggle", false}})}}});
}

// A fake arm: Observe reports its facts with a new generation each time;
// a substep applies its effect unless a script overrides it.
struct FakeArm {
  FakeClock clock;
  State state{State::kDeclared};
  bool error{true};
  bool armed{false};
  bool broadcasting{false};
  bool toggle{false};
  bool report_armed{true};
  bool observe_fails{false};
  std::int64_t age_ns{};
  std::uint64_t generation{};
  std::optional<BrandLifecycleObservationDTO> last;
  std::vector<std::string> dispatched;
  std::vector<std::uint64_t> dispatched_generations;
  std::vector<std::int64_t> dispatched_remaining;
  std::map<std::string, std::function<LifecycleSubstepDispatchOutcome()>, std::less<>> scripts;

  Result<BrandLifecycleObservationDTO> Observe() {
    if (observe_fails)
      return Result<BrandLifecycleObservationDTO>::Err(
          {"external_brand_lifecycle_query_invalid", "status unavailable", {}});
    ++generation;
    std::vector<LifecycleFactDTO> facts{
        {LifecycleFactId("external.broadcaster_active"), Boolean(broadcasting)},
        {LifecycleFactId("external.error_present"), Boolean(error)}};
    if (report_armed)
      facts.push_back({LifecycleFactId("external.hardware_active"), Boolean(armed)});
    facts.push_back({LifecycleFactId("external.toggle"), Boolean(toggle)});
    const auto received = clock.now() - age_ns;
    last = BrandLifecycleObservationDTO{LifecycleTransactionId("transaction.1"),
                                        LifecycleTransactionGeneration(4U),
                                        ControllableComponentId("component.1"),
                                        ControlDomainId("domain.1"),
                                        ControlDomainRevision("domain-revision.1"),
                                        ProviderInstanceId("provider.1"),
                                        ProviderManifestId("manifest.1"),
                                        ProviderManifestRevision(std::string(kManifestRevision)),
                                        RuntimeUnitId("runtime.1"),
                                        RuntimeEpoch("runtime-epoch.1"),
                                        generation,
                                        generation,
                                        received,
                                        received,
                                        received + (age_ns != 0 ? age_ns / 2 : kFreshNs),
                                        state,
                                        std::move(facts)};
    return Result<BrandLifecycleObservationDTO>::Ok(*last);
  }

  LifecycleSubstepDispatchOutcome Dispatch(const LifecyclePrimitiveId& substep,
                                           std::uint64_t observed_generation,
                                           std::int64_t remaining_ns) {
    dispatched.push_back(substep.value());
    dispatched_generations.push_back(observed_generation);
    dispatched_remaining.push_back(remaining_ns);
    clock.advance(1);
    if (const auto script = scripts.find(substep.value()); script != scripts.end())
      return script->second();
    if (substep.value() == kClear) error = false;
    if (substep.value() == kHardware) armed = true;
    if (substep.value() == kBroadcaster) {
      broadcasting = true;
      state = State::kPrepared;
    }
    if (substep.value() == "toggle.on") toggle = true;
    if (substep.value() == "toggle.off") toggle = false;
    return {Outcome::kSucceeded, std::nullopt};
  }

  LifecycleTransitionRunnerIo Io() {
    return {
        [this] { return Observe(); },
        [this](const LifecyclePrimitiveId& substep, std::uint64_t observed_generation,
               std::int64_t remaining_ns, const LifecycleSubstepFeedbackSink&,
               std::stop_token) { return Dispatch(substep, observed_generation, remaining_ns); },
        [this](std::int64_t ns, std::stop_token) { clock.advance(ns); }, &clock, kPollNs};
  }
};

[[nodiscard]] BrandLifecyclePrimitiveIntentDTO PrepareIntent(
    std::int64_t remaining_ns = 1'000'000) {
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
          LifecycleTransitionActionId(ComponentLifecycleIntentKindDTO::kPrepare),
          LifecyclePrimitiveAttemptId("attempt.4.1"),
          1U,
          remaining_ns,
          State::kPrepared};
}

[[nodiscard]] std::vector<std::string> SubstepIds(const BrandLifecyclePrimitiveResultDTO& result) {
  std::vector<std::string> ids;
  for (const auto& substep : result.substeps) ids.push_back(substep.substep_id.value());
  return ids;
}

[[nodiscard]] ErrorDTO Error(std::string code) { return {std::move(code), "refused", {}}; }

TEST(LifecycleTransitionRunner, RunsTheRealPrepareSubstepsInOrderWithEvents) {
  FakeArm arm;
  const CatalogLifecycleAdmission admission(RealPrepareCatalog());
  std::vector<LifecycleSubstepEventDTO> events;
  const auto intent = PrepareIntent();
  const auto result = RunLifecycleTransition(
      intent, admission, arm.Io(),
      [&](const LifecycleSubstepEventDTO& event) { events.push_back(event); }, {});

  EXPECT_EQ(result.outcome, Outcome::kSucceeded);
  EXPECT_FALSE(result.error);
  EXPECT_TRUE(result.matches(intent));
  const std::vector<std::string> order{std::string(kClear), std::string(kHardware),
                                       std::string(kBroadcaster)};
  EXPECT_EQ(SubstepIds(result), order);
  for (const auto& substep : result.substeps) {
    EXPECT_EQ(substep.outcome, Outcome::kSucceeded);
    EXPECT_FALSE(substep.within);
    EXPECT_LT(substep.started_at_ns, substep.finished_at_ns);
  }
  // Each substep gets the generation of the observation its decision used.
  EXPECT_EQ(arm.dispatched_generations, (std::vector<std::uint64_t>{1U, 2U, 3U}));

  ASSERT_EQ(events.size(), 6U);
  for (std::size_t index = 0U; index < events.size(); ++index) {
    const auto& event = events[index];
    EXPECT_TRUE(event.locally_valid());
    EXPECT_EQ(event.attempt_id, intent.lifecycle_primitive_attempt_id);
    EXPECT_EQ(event.substep_id.value(), order[index / 2U]);
    EXPECT_EQ(event.phase, index % 2U == 0U ? LifecycleSubstepEventDTO::Phase::kStarted
                                            : LifecycleSubstepEventDTO::Phase::kFinished);
    EXPECT_EQ(event.outcome, index % 2U == 0U ? std::nullopt : std::optional(Outcome::kSucceeded));
    EXPECT_FALSE(event.error_code);
  }
}

TEST(LifecycleTransitionRunner, AlreadyDoneSucceedsWithoutASubstep) {
  FakeArm arm;
  arm.state = State::kPrepared;
  arm.error = false;
  arm.armed = true;
  arm.broadcasting = true;
  std::size_t events = 0U;
  const auto result =
      RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                             arm.Io(), [&](const auto&) { ++events; }, {});
  EXPECT_EQ(result.outcome, Outcome::kSucceeded);
  EXPECT_TRUE(result.substeps.empty());
  EXPECT_TRUE(arm.dispatched.empty());
  EXPECT_EQ(events, 0U);
}

TEST(LifecycleTransitionRunner, StopsAtTheFirstFailedSubstepWithItsError) {
  FakeArm arm;
  auto refused = Error("ros2_control_hardware_activation_refused");
  refused.details.emplace("vendor_status", "error=0 state=4");
  arm.scripts[std::string(kHardware)] = [&] {
    return LifecycleSubstepDispatchOutcome{Outcome::kFailed, refused};
  };
  std::vector<LifecycleSubstepEventDTO> events;
  const auto result = RunLifecycleTransition(
      PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()), arm.Io(),
      [&](const LifecycleSubstepEventDTO& event) { events.push_back(event); }, {});

  EXPECT_EQ(result.outcome, Outcome::kFailed);
  EXPECT_EQ(result.error, refused);
  EXPECT_TRUE(result.locally_valid());
  EXPECT_EQ(SubstepIds(result),
            (std::vector<std::string>{std::string(kClear), std::string(kHardware)}));
  EXPECT_EQ(result.substeps[0].outcome, Outcome::kSucceeded);
  EXPECT_EQ(result.substeps[1].outcome, Outcome::kFailed);
  EXPECT_EQ(result.substeps[1].error, refused);
  EXPECT_EQ(arm.dispatched.size(), 2U);
  ASSERT_FALSE(events.empty());
  EXPECT_EQ(events.back().outcome, Outcome::kFailed);
  EXPECT_EQ(events.back().error_code, refused.code);
}

TEST(LifecycleTransitionRunner, ConnectionLostEndsTheTransitionAsConnectionLost) {
  FakeArm arm;
  const auto unresponsive = Error("controller_manager_unresponsive");
  arm.scripts[std::string(kHardware)] = [&] {
    return LifecycleSubstepDispatchOutcome{Outcome::kConnectionLost, unresponsive};
  };
  const auto result = RunLifecycleTransition(
      PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kConnectionLost);
  EXPECT_EQ(result.error, unresponsive);
  EXPECT_EQ(result.substeps.back().outcome, Outcome::kConnectionLost);
  EXPECT_EQ(arm.dispatched.size(), 2U);
}

TEST(LifecycleTransitionRunner, FailedSubstepWithoutAnErrorGetsTheTypedFallback) {
  FakeArm arm;
  arm.scripts[std::string(kClear)] = [] {
    return LifecycleSubstepDispatchOutcome{Outcome::kFailed, std::nullopt};
  };
  const auto result = RunLifecycleTransition(
      PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_primitive_failed");
  EXPECT_EQ(result.error->details.at("reason"), "primitive_failed");
  EXPECT_EQ(result.substeps.front().error, result.error);
  EXPECT_TRUE(result.locally_valid());
}

// Current status decides each next step: a mid-plan refusal is the
// evaluator's typed reason on the latest observation, unchanged.
TEST(LifecycleTransitionRunner, NotReadyMidPlanFailsWithTheEvaluatorsError) {
  FakeArm arm;
  arm.scripts[std::string(kClear)] = [&] {
    arm.error = false;
    arm.report_armed = false;
    return LifecycleSubstepDispatchOutcome{Outcome::kSucceeded, std::nullopt};
  };
  const auto catalog = RealPrepareCatalog();
  const auto result =
      RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(catalog), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(arm.last);
  const auto evaluated = EvaluateLifecycleDomainPlan(ComponentLifecycleIntentKindDTO::kPrepare,
                                                     State::kPrepared, catalog, *arm.last);
  ASSERT_FALSE(evaluated.ok());
  EXPECT_EQ(result.error, evaluated.error());
  EXPECT_EQ(result.error->code, "component_lifecycle_transition_inconclusive");
  EXPECT_EQ(SubstepIds(result), (std::vector<std::string>{std::string(kClear)}));
}

TEST(LifecycleTransitionRunner, DeadlineEndsBeforeTheNextSubstep) {
  FakeArm arm;
  arm.scripts[std::string(kClear)] = [&] {
    arm.error = false;
    arm.clock.advance(1'000);
    return LifecycleSubstepDispatchOutcome{Outcome::kSucceeded, std::nullopt};
  };
  const auto result = RunLifecycleTransition(
      PrepareIntent(1'000), CatalogLifecycleAdmission(RealPrepareCatalog()), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_deadline_expired");
  EXPECT_EQ(result.error->details.at("reason"), "deadline_exhausted");
  EXPECT_EQ(arm.dispatched, (std::vector<std::string>{std::string(kClear)}));
  // The substep gets the transaction's remaining time.
  EXPECT_EQ(arm.dispatched_remaining, (std::vector<std::int64_t>{1'000}));
}

TEST(LifecycleTransitionRunner, PostconditionAndVerificationTimeoutsKeepTodaysCodes) {
  FakeArm unchanged;
  unchanged.scripts[std::string(kClear)] = [] {
    return LifecycleSubstepDispatchOutcome{Outcome::kSucceeded, std::nullopt};
  };
  auto result = RunLifecycleTransition(
      PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()), unchanged.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_postcondition_timeout");
  EXPECT_EQ(result.error->details.at("reason"), "transition_postcondition_unsatisfied");
  EXPECT_EQ(unchanged.dispatched.size(), 1U);

  FakeArm silent;
  silent.scripts[std::string(kClear)] = [&] {
    silent.error = false;
    silent.observe_fails = true;
    return LifecycleSubstepDispatchOutcome{Outcome::kSucceeded, std::nullopt};
  };
  result = RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                                  silent.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_verification_timeout");
  EXPECT_EQ(result.error->details.at("reason"), "postcondition_observation_timeout");
}

TEST(LifecycleTransitionRunner, SubstepBoundEndsAPlanThatNeverCompletes) {
  FakeArm arm;
  const auto result = RunLifecycleTransition(
      PrepareIntent(1'000'000'000), CatalogLifecycleAdmission(ToggleCatalog()), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_postcondition_timeout");
  EXPECT_EQ(result.error->details.at("reason"), "substep_bound_reached");
  EXPECT_EQ(result.substeps.size(), kMaxLifecycleSubsteps);
  EXPECT_EQ(arm.dispatched.size(), kMaxLifecycleSubsteps);
  EXPECT_TRUE(result.locally_valid());
}

TEST(LifecycleTransitionRunner, CancellationFailsWithoutAnotherSubstep) {
  FakeArm before;
  std::stop_source stopped;
  stopped.request_stop();
  auto result =
      RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                             before.Io(), {}, stopped.get_token());
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "unsupported_capability");
  EXPECT_EQ(result.error->details.at("reason"), "external_brand_lifecycle_cancelled");
  EXPECT_EQ(before.generation, 0U);
  EXPECT_TRUE(before.dispatched.empty());

  FakeArm during;
  std::stop_source shutdown;
  during.scripts[std::string(kClear)] = [&] {
    during.error = false;
    shutdown.request_stop();
    return LifecycleSubstepDispatchOutcome{Outcome::kSucceeded, std::nullopt};
  };
  result = RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                                  during.Io(), {}, shutdown.get_token());
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->details.at("reason"), "external_brand_lifecycle_cancelled");
  EXPECT_EQ(SubstepIds(result), (std::vector<std::string>{std::string(kClear)}));
}

TEST(LifecycleTransitionRunner, NestedSubstepsFollowTheirEnclosingSubstep) {
  FakeArm arm;
  arm.scripts[std::string(kHardware)] = [&] {
    arm.armed = true;
    const auto at = arm.clock.now();
    return LifecycleSubstepDispatchOutcome{
        Outcome::kSucceeded,
        std::nullopt,
        {{LifecyclePrimitiveId(std::string(kSync)), LifecyclePrimitiveId(std::string(kHardware)),
          Outcome::kSucceeded, at, at, std::nullopt}}};
  };
  const auto result = RunLifecycleTransition(
      PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()), arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kSucceeded);
  EXPECT_EQ(SubstepIds(result),
            (std::vector<std::string>{std::string(kClear), std::string(kHardware),
                                      std::string(kSync), std::string(kBroadcaster)}));
  EXPECT_EQ(result.substeps[2].within, LifecyclePrimitiveId(std::string(kHardware)));
  EXPECT_TRUE(result.locally_valid());
}

TEST(LifecycleTransitionRunner, RefusesAnUnknownActionAndAnUnusableFirstObservation) {
  FakeArm arm;
  auto intent = PrepareIntent();
  intent.lifecycle_primitive_id = LifecyclePrimitiveId("enable-control");
  auto result = RunLifecycleTransition(intent, CatalogLifecycleAdmission(RealPrepareCatalog()),
                                       arm.Io(), {}, {});
  EXPECT_EQ(result.outcome, Outcome::kFailed);
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_primitive_invalid");
  EXPECT_EQ(result.error->details.at("reason"), "transition_action_unknown");
  EXPECT_EQ(arm.generation, 0U);

  arm.observe_fails = true;
  result = RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                                  arm.Io(), {}, {});
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "external_brand_lifecycle_query_invalid");

  arm.observe_fails = false;
  arm.clock.set(1'000);
  arm.age_ns = 100;
  result = RunLifecycleTransition(PrepareIntent(), CatalogLifecycleAdmission(RealPrepareCatalog()),
                                  arm.Io(), {}, {});
  ASSERT_TRUE(result.error);
  EXPECT_EQ(result.error->code, "component_lifecycle_observation_stale");
  EXPECT_EQ(result.error->details.at("reason"), "initial_observation_not_fresh");
  EXPECT_TRUE(arm.dispatched.empty());
}

}  // namespace
}  // namespace manyforge::core

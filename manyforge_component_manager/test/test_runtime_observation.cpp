#include "manyforge_component_manager/runtime_observation.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace manyforge::component_manager {
namespace {

class ManualClock final {
 public:
  [[nodiscard]] std::uint64_t operator()() const noexcept {
    return now_;
  }

  void Advance(const std::uint64_t amount) {
    now_ += amount;
  }

 private:
  std::uint64_t now_{1};
};

RuntimeUnitExpectation Expectation(
    const std::string& runtime_unit_id,
    std::vector<std::string> dependencies = {},
    const std::uint64_t maximum_presence_age_ns = 100,
    const std::uint64_t maximum_report_age_ns = 100,
    const std::optional<std::uint64_t> maximum_progress_age_ns =
        std::nullopt,
    std::string product_identity = "product-v1",
    std::string configuration_identity = "configuration-v1",
    std::vector<std::string> roles = {"status"},
    const ManagedCommandSurfaceState command_surface =
        ManagedCommandSurfaceState::kClosedNoActiveEffect) {
  return RuntimeUnitExpectation(
      runtime_unit_id,
      RuntimeLifecycleControl::kManagerManaged,
      std::move(product_identity),
      std::move(configuration_identity),
      maximum_presence_age_ns,
      maximum_report_age_ns,
      maximum_progress_age_ns,
      std::move(dependencies),
      std::move(roles),
      command_surface);
}

ProcessPresenceObservation Presence(
    const std::string& runtime_unit_id,
    const std::string& incarnation,
    const std::uint64_t sequence = 1,
    const std::string& observer = "observer-a") {
  return ProcessPresenceObservation(
      runtime_unit_id,
      observer,
      sequence,
      ProcessPresenceState::kPresent,
      incarnation);
}

RuntimeUnitSourceReport Report(
    const std::string& runtime_unit_id,
    const std::string& incarnation,
    const std::uint64_t sequence = 1,
    const std::uint64_t progress = 1,
    const RuntimeSourcePhase phase = RuntimeSourcePhase::kServing,
    const RuntimeSourceHealth health = RuntimeSourceHealth::kHealthy,
    std::string product_identity = "product-v1",
    std::string configuration_identity = "configuration-v1",
    std::vector<std::string> roles = {"status"},
    const ManagedCommandSurfaceState command_surface =
        ManagedCommandSurfaceState::kClosedNoActiveEffect,
    std::vector<std::string> reasons = {}) {
  return RuntimeUnitSourceReport(
      runtime_unit_id,
      incarnation,
      sequence,
      progress,
      phase,
      health,
      std::move(product_identity),
      std::move(configuration_identity),
      std::move(roles),
      command_surface,
      std::move(reasons));
}

std::string SerializeObservation(
    const std::string& checkpoint,
    const RuntimeUnitObservation& observation) {
  std::string final_field;
  if (!observation.reason_codes().empty()) {
    for (std::size_t index = 0;
         index < observation.reason_codes().size();
         ++index) {
      if (index != 0U) {
        final_field.push_back(',');
      }
      final_field += observation.reason_codes()[index];
    }
  } else {
    for (const auto& [dependency, epoch] :
         observation.dependency_runtime_epochs()) {
      if (!final_field.empty()) {
        final_field.push_back(',');
      }
      final_field += dependency + "=" + epoch;
    }
  }
  return checkpoint + "|" + observation.runtime_unit_id() + "|" +
      std::string(ToString(observation.derived_state())) + "|" +
      std::string(ToString(observation.process_presence())) + "|" +
      observation.runtime_epoch() + "|" + final_field;
}

std::vector<std::string> ReadGolden() {
  const auto path =
      std::filesystem::path(MANYFORGE_SOURCE_ROOT) /
      "manyforge_component_manager/testdata/"
      "runtime_observation_parity_v1.txt";
  std::ifstream input(path);
  if (!input) {
    throw std::runtime_error("cannot open parity vector: " + path.string());
  }
  std::vector<std::string> lines;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty()) {
      lines.push_back(line);
    }
  }
  return lines;
}

TEST(RuntimeObservationValues, RejectInvalidAndUnboundedInput) {
  EXPECT_THROW(
      RuntimeUnitExpectation(
          "unit",
          RuntimeLifecycleControl::kManagerManaged,
          "product",
          "config",
          0,
          1),
      std::invalid_argument);
  EXPECT_THROW(
      RuntimeUnitExpectation(
          "unit",
          RuntimeLifecycleControl::kManagerManaged,
          "product",
          "config",
          1,
          1,
          std::nullopt,
          {"unit"}),
      std::invalid_argument);
  EXPECT_THROW(
      ProcessPresenceObservation(
          "unit",
          "observer",
          1,
          ProcessPresenceState::kAbsent,
          "epoch"),
      std::invalid_argument);
  EXPECT_THROW(
      RuntimeUnitSourceReport(
          "unit",
          "epoch",
          1,
          0,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kHealthy,
          "product",
          "config",
          {"status", "status"},
          ManagedCommandSurfaceState::kClosedNoActiveEffect),
      std::invalid_argument);

  ManualClock clock;
  EXPECT_THROW(
      RuntimeObservationReducer(
          [&clock] { return clock(); },
          std::vector<std::string>(65, "duplicate")),
      std::invalid_argument);
}

TEST(RuntimeObservationValues, ValidateIndependentProcessHandles) {
  ValidateRuntimeUnitProcessBindings({
      RuntimeUnitProcessBinding(
          "unit-a",
          "shared-process",
          RuntimeLifecycleControl::kManagerManaged,
          "presence-a",
          "status-a",
          "progress-a",
          "incarnation-a",
          "lifecycle-a"),
      RuntimeUnitProcessBinding(
          "unit-b",
          "shared-process",
          RuntimeLifecycleControl::kExternallyManaged,
          "presence-b",
          "status-b",
          "progress-b",
          "incarnation-b"),
  });
  EXPECT_THROW(
      ValidateRuntimeUnitProcessBindings({
          RuntimeUnitProcessBinding(
              "unit-a",
              "process-a",
              RuntimeLifecycleControl::kManagerManaged,
              "shared-presence",
              "status-a",
              "progress-a",
              "incarnation-a",
              "lifecycle-a"),
          RuntimeUnitProcessBinding(
              "unit-b",
              "process-b",
              RuntimeLifecycleControl::kManagerManaged,
              "shared-presence",
              "status-b",
              "progress-b",
              "incarnation-b",
              "lifecycle-b"),
      }),
      std::invalid_argument);
}

TEST(RuntimeObservationReducer, MissingThenExactEvidenceConverges) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"unit"});
  const auto target = Expectation("unit");

  const auto missing = subject.Evaluate({target});
  ASSERT_NE(missing.observation("unit"), nullptr);
  EXPECT_EQ(
      missing.observation("unit")->derived_state(),
      RuntimeConvergenceState::kLost);
  EXPECT_EQ(
      missing.observation("unit")->reason_codes(),
      std::vector<std::string>{"process_presence_missing"});

  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch")));
  auto no_report = subject.Evaluate({target});
  EXPECT_EQ(
      no_report.observation("unit")->derived_state(),
      RuntimeConvergenceState::kNonconverged);
  EXPECT_TRUE(subject.IngestSourceReport(Report("unit", "epoch")));
  const auto converged = subject.Evaluate({target});
  ASSERT_NE(converged.observation("unit"), nullptr);
  EXPECT_TRUE(converged.observation("unit")->converged());
  EXPECT_EQ(converged.observation("unit")->presence_age_ns(), 0);
  EXPECT_EQ(converged.observation("unit")->report_age_ns(), 0);
}

TEST(RuntimeObservationReducer, DuplicateAndConflictDoNotRenewFreshness) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"unit"});
  const auto target = Expectation("unit", {}, 200, 100);
  const auto first = Report("unit", "epoch", 2, 2);
  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch", 1)));
  EXPECT_TRUE(subject.IngestSourceReport(first));
  clock.Advance(50);
  EXPECT_FALSE(subject.IngestSourceReport(first));
  EXPECT_FALSE(subject.IngestSourceReport(
      Report(
          "unit",
          "epoch",
          2,
          2,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kDegraded)));
  EXPECT_FALSE(subject.IngestSourceReport(Report("unit", "epoch", 1, 1)));
  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch", 2)));
  clock.Advance(51);

  const auto snapshot = subject.Evaluate({target});
  const auto observation = snapshot.observation("unit");
  ASSERT_NE(observation, nullptr);
  EXPECT_EQ(observation->report_age_ns(), 101);
  EXPECT_EQ(
      observation->reason_codes(),
      std::vector<std::string>{"source_report_stale"});
  const auto metrics = subject.metrics();
  EXPECT_EQ(metrics.duplicate_total, 1U);
  EXPECT_EQ(metrics.rejection_reasons.at(
                "source_report_sequence_conflict"),
            1U);
  EXPECT_EQ(metrics.rejection_reasons.at(
                "source_report_sequence_regressed"),
            1U);
}

TEST(RuntimeObservationReducer, ReportRefreshDoesNotFabricateProgress) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"unit"});
  const auto target = Expectation("unit", {}, 200, 50, 100);
  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch")));
  EXPECT_TRUE(subject.IngestSourceReport(Report("unit", "epoch", 1, 1)));
  clock.Advance(80);
  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch", 2)));
  EXPECT_TRUE(subject.IngestSourceReport(Report("unit", "epoch", 2, 1)));
  clock.Advance(21);

  const auto snapshot = subject.Evaluate({target});
  const auto observation = snapshot.observation("unit");
  ASSERT_NE(observation, nullptr);
  EXPECT_EQ(observation->report_age_ns(), 21);
  EXPECT_EQ(observation->progress_age_ns(), 101);
  EXPECT_EQ(
      observation->reason_codes(),
      std::vector<std::string>{"critical_progress_stale"});
}

TEST(RuntimeObservationReducer, ExactSourceEvidenceIsRequired) {
  const auto check_reason = [](RuntimeUnitSourceReport report,
                               const std::string& expected_reason) {
    ManualClock clock;
    RuntimeObservationReducer subject(
        [&clock] { return clock(); },
        {"unit"});
    EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch")));
    EXPECT_TRUE(subject.IngestSourceReport(report));
    const auto snapshot = subject.Evaluate({Expectation("unit")});
    const auto observation = snapshot.observation("unit");
    ASSERT_NE(observation, nullptr);
    EXPECT_EQ(
        observation->derived_state(),
        RuntimeConvergenceState::kNonconverged);
    EXPECT_EQ(
        observation->reason_codes(),
        std::vector<std::string>{expected_reason});
  };

  check_reason(
      Report(
          "unit",
          "epoch",
          1,
          1,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kHealthy,
          "wrong-product"),
      "runtime_product_identity_mismatch");
  check_reason(
      Report(
          "unit",
          "epoch",
          1,
          1,
          RuntimeSourcePhase::kStarting),
      "runtime_source_phase_starting");
  check_reason(
      Report(
          "unit",
          "epoch",
          1,
          1,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kDegraded),
      "runtime_source_health_degraded");
  check_reason(
      Report(
          "unit",
          "epoch",
          1,
          1,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kHealthy,
          "product-v1",
          "configuration-v1",
          {}),
      "runtime_roles_not_ready");
  check_reason(
      Report(
          "unit",
          "epoch",
          1,
          1,
          RuntimeSourcePhase::kServing,
          RuntimeSourceHealth::kHealthy,
          "product-v1",
          "configuration-v1",
          {"status"},
          ManagedCommandSurfaceState::kOpen),
      "runtime_command_surface_state_mismatch");
}

TEST(RuntimeObservationReducer, ObserverAndIncarnationReuseAreFenced) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"unit"});
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("unit", "epoch-a", 1, "observer-a")));
  EXPECT_TRUE(subject.IngestSourceReport(Report("unit", "epoch-a")));
  const auto converged_snapshot = subject.Evaluate({Expectation("unit")});
  EXPECT_TRUE(converged_snapshot.observations().front().converged());
  clock.Advance(1);
  EXPECT_TRUE(subject.ReplaceProcessObserver(
      Presence("unit", "epoch-a", 1, "observer-b")));
  EXPECT_FALSE(subject.ObserveProcess(
      Presence("unit", "epoch-a", 2, "observer-a")));
  EXPECT_TRUE(subject.ObserveProcess(ProcessPresenceObservation(
      "unit",
      "observer-b",
      2,
      ProcessPresenceState::kAbsent)));
  EXPECT_FALSE(subject.ObserveProcess(
      Presence("unit", "epoch-a", 3, "observer-b")));
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("unit", "epoch-b", 4, "observer-b")));

  const auto snapshot = subject.Evaluate({Expectation("unit")});
  const auto observation = snapshot.observation("unit");
  ASSERT_NE(observation, nullptr);
  EXPECT_EQ(
      observation->derived_state(),
      RuntimeConvergenceState::kRecovering);
  EXPECT_EQ(
      observation->reason_codes(),
      std::vector<std::string>{"source_report_missing"});
}

TEST(RuntimeObservationReducer, UnknownPresenceDoesNotRetireIncarnation) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"unit"});
  const auto target = Expectation("unit");
  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch-a")));
  EXPECT_TRUE(subject.IngestSourceReport(Report("unit", "epoch-a")));
  const auto converged = subject.Evaluate({target});
  ASSERT_NE(converged.observation("unit"), nullptr);
  EXPECT_TRUE(converged.observation("unit")->converged());

  clock.Advance(1);
  EXPECT_TRUE(subject.ObserveProcess(ProcessPresenceObservation(
      "unit",
      "observer-a",
      2,
      ProcessPresenceState::kUnknown)));
  const auto unknown = subject.Evaluate({target});
  ASSERT_NE(unknown.observation("unit"), nullptr);
  EXPECT_EQ(
      unknown.observation("unit")->derived_state(),
      RuntimeConvergenceState::kLost);

  EXPECT_TRUE(subject.ObserveProcess(Presence("unit", "epoch-a", 3)));
  const auto restored = subject.Evaluate({target});
  ASSERT_NE(restored.observation("unit"), nullptr);
  EXPECT_TRUE(restored.observation("unit")->converged());
}

TEST(RuntimeObservationReducer, DependencyRestartRequiresCausalRejoin) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"dependency", "worker"});
  const auto dependency = Expectation("dependency");
  const auto worker = Expectation("worker", {"dependency"});
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("dependency", "dependency-epoch-1")));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("dependency", "dependency-epoch-1")));
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("worker", "worker-epoch-1")));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("worker", "worker-epoch-1")));
  const auto initial_snapshot = subject.Evaluate({worker, dependency});
  ASSERT_NE(initial_snapshot.observation("worker"), nullptr);
  EXPECT_TRUE(initial_snapshot.observation("worker")->converged());

  clock.Advance(1);
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("worker", "worker-epoch-1", 2, 2)));
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("dependency", "dependency-epoch-2", 2)));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("dependency", "dependency-epoch-2")));
  const auto fenced_snapshot = subject.Evaluate({dependency, worker});
  const auto fenced = fenced_snapshot.observation("worker");
  ASSERT_NE(fenced, nullptr);
  EXPECT_EQ(fenced->derived_state(), RuntimeConvergenceState::kRecovering);
  EXPECT_EQ(
      fenced->reason_codes(),
      std::vector<std::string>{"runtime_dependency_epoch_changed"});

  EXPECT_TRUE(subject.IngestSourceReport(
      Report("worker", "worker-epoch-1", 3, 3)));
  const auto recovered_snapshot = subject.Evaluate({dependency, worker});
  const auto recovered = recovered_snapshot.observation("worker");
  ASSERT_NE(recovered, nullptr);
  EXPECT_TRUE(recovered->converged());
  EXPECT_EQ(
      recovered->dependency_runtime_epochs().at("dependency"),
      "dependency-epoch-2");
}

TEST(RuntimeObservationReducer, ExpectationGraphIsStrictAndAcyclic) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"a", "b"});
  EXPECT_THROW(
      subject.Evaluate({Expectation("a", {"missing"})}),
      std::invalid_argument);
  EXPECT_THROW(
      subject.Evaluate({
          Expectation("a", {"b"}),
          Expectation("b", {"a"}),
      }),
      std::invalid_argument);
}

TEST(RuntimeObservationReducer, ObserverAndIncarnationLedgersAreBounded) {
  ManualClock clock;
  RuntimeObservationReducer observers(
      [&clock] { return clock(); },
      {"unit"});
  EXPECT_TRUE(observers.ObserveProcess(
      Presence("unit", "epoch", 1, "observer-0")));
  for (std::size_t index = 1;
       index < kMaximumObserverSessionsPerRuntimeUnit;
       ++index) {
    EXPECT_TRUE(observers.ReplaceProcessObserver(Presence(
        "unit",
        "epoch",
        1,
        "observer-" + std::to_string(index))));
  }
  EXPECT_FALSE(observers.ReplaceProcessObserver(
      Presence("unit", "epoch", 1, "observer-overflow")));
  EXPECT_EQ(
      observers.metrics().rejection_reasons.at(
          "process_observer_session_limit_exceeded"),
      1U);

  RuntimeObservationReducer incarnations(
      [&clock] { return clock(); },
      {"unit"});
  for (std::size_t index = 0;
       index < kMaximumIncarnationsPerRuntimeUnit;
       ++index) {
    EXPECT_TRUE(incarnations.ObserveProcess(Presence(
        "unit",
        "epoch-" + std::to_string(index),
        index + 1)));
  }
  EXPECT_FALSE(incarnations.ObserveProcess(Presence(
      "unit",
      "epoch-overflow",
      kMaximumIncarnationsPerRuntimeUnit + 1)));
}

TEST(RuntimeObservationReducer, SharedGoldenMatchesPythonReference) {
  ManualClock clock;
  RuntimeObservationReducer subject(
      [&clock] { return clock(); },
      {"dependency", "worker"});
  const auto dependency = Expectation("dependency");
  const auto worker = Expectation("worker", {"dependency"});

  EXPECT_TRUE(subject.ObserveProcess(
      Presence("dependency", "dependency-epoch-1")));
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("worker", "worker-epoch-1")));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("dependency", "dependency-epoch-1")));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("worker", "worker-epoch-1")));
  std::vector<std::string> actual;
  const auto initial_snapshot = subject.Evaluate({worker, dependency});
  for (const auto& observation : initial_snapshot.observations()) {
    actual.push_back(SerializeObservation("initial", observation));
  }

  clock.Advance(1);
  EXPECT_TRUE(subject.ObserveProcess(
      Presence("dependency", "dependency-epoch-2", 2)));
  EXPECT_TRUE(subject.IngestSourceReport(
      Report("dependency", "dependency-epoch-2")));
  const auto restarted_snapshot = subject.Evaluate({worker, dependency});
  for (const auto& observation : restarted_snapshot.observations()) {
    actual.push_back(
        SerializeObservation("dependency_restarted", observation));
  }

  EXPECT_TRUE(subject.IngestSourceReport(
      Report("worker", "worker-epoch-1", 2, 2)));
  const auto rejoined_snapshot = subject.Evaluate({worker, dependency});
  for (const auto& observation : rejoined_snapshot.observations()) {
    actual.push_back(SerializeObservation("worker_rejoined", observation));
  }
  EXPECT_EQ(actual, ReadGolden());
}

TEST(RuntimeObservationReducer, MonotonicClockRegressionFailsClosed) {
  std::uint64_t now = 2;
  RuntimeObservationReducer subject([&now] { return now; }, {"unit"});
  static_cast<void>(subject.Evaluate({Expectation("unit")}));
  now = 1;
  EXPECT_THROW(
      static_cast<void>(subject.Evaluate({Expectation("unit")})),
      std::runtime_error);
}

}  // namespace
}  // namespace manyforge::component_manager

#include <manyforge_roboplan/mock_boolean_input_provider.hpp>

#include <atomic>
#include <chrono>
#include <future>
#include <gtest/gtest.h>
#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <manyforge_core/time/manual_scheduler.hpp>
#include <mutex>

namespace manyforge::roboplan_backend {
namespace {

const core::InterruptSourceId kSource("mock.inputs");
const core::IoSourceIncarnation kIncarnation("activation.1");

core::IoSignalId id(std::string name) {
  return {core::ControllableComponentId("component.io"), std::move(name)};
}
core::IoSignalDescriptorDTO descriptor(std::string name) {
  return {id(std::move(name)),
          kSource,
          core::IoSignalValueType::kBoolean,
          core::IoSignalDirection::kInput,
          core::IoSignalUpdateMode::kChangeOnly,
          core::IoTransitionCapability::kSampledOnly,
          {core::IoFreshnessMode::kProviderValidity, std::nullopt},
          std::nullopt,
          core::IoSignalLivenessMode::kInProcess};
}
MockBooleanInputSource source() {
  return {
      kSource, kIncarnation, {{descriptor("off")}, {descriptor("on"), true}}};
}
core::IoLevelWatchRequestDTO watch() {
  return {{core::RequestOwnerEpoch("runner"),
           core::ProgramRunId("run"),
           {core::ProgramNodeInstanceId("wait"), "attempt"}},
          id("on"),
          true};
}
core::IoSourceCursorDTO origin() {
  return {core::RequestOwnerEpoch("host"), 1, kSource, 0};
}

class MockBooleanInputProviderTest : public ::testing::Test {
protected:
  void Build(std::size_t capacity = 8,
             std::optional<std::uint64_t> prepared = std::nullopt) {
    auto created =
        core::IoSignalSubsystem::Create({core::RequestOwnerEpoch("host"),
                                         1,
                                         {descriptor("off"), descriptor("on")},
                                         {kSource}},
                                        {1, 2, capacity, 4, 128, 1024}, clock);
    ASSERT_TRUE(created.ok());
    signals =
        std::shared_ptr<core::IoSignalSubsystem>(std::move(created.value()));
    ASSERT_TRUE(
        signals->InstallSource(kSource, kIncarnation, std::nullopt).ok());
    auto queued = core::IoSignalIngress::Create(signals);
    ASSERT_TRUE(queued.ok());
    ingress = std::shared_ptr<core::IoSignalIngress>(std::move(queued.value()));
    auto provided =
        MockBooleanInputProvider::Create(ingress, clock, source(), prepared);
    ASSERT_TRUE(provided.ok());
    provider = std::move(provided.value());
  }
  void Successor(const core::IoSourceIncarnation &incarnation,
                 std::shared_ptr<core::IoSignalSubsystem> &next_signals,
                 std::shared_ptr<core::IoSignalIngress> &next_ingress) {
    auto created =
        core::IoSignalSubsystem::Create({core::RequestOwnerEpoch("host"),
                                         2,
                                         {descriptor("off"), descriptor("on")},
                                         {kSource}},
                                        {1, 2, 8, 4, 128, 1024}, clock);
    ASSERT_TRUE(created.ok());
    next_signals =
        std::shared_ptr<core::IoSignalSubsystem>(std::move(created.value()));
    ASSERT_TRUE(
        next_signals->InstallSource(kSource, incarnation, std::nullopt).ok());
    auto queued = core::IoSignalIngress::Create(next_signals);
    ASSERT_TRUE(queued.ok());
    next_ingress =
        std::shared_ptr<core::IoSignalIngress>(std::move(queued.value()));
  }
  core::FakeClock clock;
  core::ManualScheduler scheduler;
  std::shared_ptr<core::IoSignalSubsystem> signals;
  std::shared_ptr<core::IoSignalIngress> ingress;
  std::unique_ptr<MockBooleanInputProvider> provider;
};

TEST_F(MockBooleanInputProviderTest,
       InitialSnapshotUsesDefaultsAndIsAtomicIdempotentAndCallerDriven) {
  Build();
  ASSERT_TRUE(provider);
  EXPECT_EQ(signals->ReadSignal(id("off")).value().availability,
            core::IoSignalAvailability::kMissing);
  const auto registered = signals->RegisterLevelWatch(watch());
  ASSERT_TRUE(registered.ok());
  ASSERT_EQ(provider->PublishInitial().value(), 1U);
  EXPECT_EQ(provider->PublishInitial().value(), 1U);
  EXPECT_EQ(signals->ReadWatch(registered.value()).value().state,
            core::IoWatchState::kPending);
  const auto drained = ingress->Drain(8);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 1U);
  const auto off = signals->ReadSignal(id("off")).value();
  const auto on = signals->ReadSignal(id("on")).value();
  ASSERT_TRUE(off.latest_sample);
  ASSERT_TRUE(on.latest_sample);
  EXPECT_EQ(off.latest_sample->publication.value, false);
  EXPECT_EQ(on.latest_sample->publication.value, true);
  EXPECT_EQ(off.cursor, on.cursor);
  EXPECT_EQ(signals->ReadWatch(registered.value()).value().state,
            core::IoWatchState::kMatched);
  const auto records = signals->ReadRecords(origin(), 8);
  ASSERT_TRUE(records.ok());
  std::size_t acquisitions = 0;
  for (const auto &record : records.value().records) {
    if (!record.acquisition)
      continue;
    ++acquisitions;
    EXPECT_EQ(record.acquisition->updates.size(), 2U);
  }
  EXPECT_EQ(acquisitions, 1U);
}

TEST_F(MockBooleanInputProviderTest,
       DisabledModelChangesPublishOnlyOnActualEnableWithoutReseeding) {
  Build(8, 1U);
  ASSERT_TRUE(provider);
  EXPECT_FALSE(provider->PublishInitial().ok());
  auto changed = provider->SetValue(id("off"), true);
  ASSERT_TRUE(changed.ok());
  EXPECT_TRUE(changed.value().model_value);
  EXPECT_FALSE(changed.value().enqueued_source_sequence);
  EXPECT_FALSE(changed.value().publication_error);
  EXPECT_EQ(ingress->Drain(8).value().attempted, 0U);
  ASSERT_TRUE(provider->Resume(2U).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(
      signals->ReadSignal(id("off")).value().latest_sample->publication.value,
      true);
  ASSERT_TRUE(provider->Pause(3U));
  ASSERT_TRUE(ingress->Drain(8).ok());
  changed = provider->SetValue(id("off"), false);
  ASSERT_TRUE(changed.ok());
  EXPECT_FALSE(changed.value().model_value);
  EXPECT_FALSE(changed.value().enqueued_source_sequence);
  EXPECT_FALSE(changed.value().publication_error);
  EXPECT_EQ(signals->ReadSignal(id("off")).value().availability,
            core::IoSignalAvailability::kUnavailable);
  ASSERT_EQ(provider->Resume(4U).value(), 2U);
  EXPECT_EQ(provider->Resume(4U).value(), 2U);
  EXPECT_EQ(ingress->Drain(8).value().applied, 1U);
  const auto observed = signals->ReadSignal(id("off")).value();
  EXPECT_EQ(observed.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.latest_sample->publication.value, false);
  EXPECT_EQ(observed.latest_sample->publication.incarnation, kIncarnation);
  EXPECT_FALSE(provider->Pause(3U));
  provider->Shutdown();
  EXPECT_FALSE(provider->SetValue(id("off"), true).ok());
  EXPECT_FALSE(provider->Resume(5U).ok());
}

TEST_F(MockBooleanInputProviderTest,
       UnchangedInstalledStateStaysFreshWithoutRefreshOrAutoAnswer) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  clock.set(1'000'000'000'000);
  scheduler.advance_to(clock.now());
  const auto drained = ingress->Drain(8);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().attempted, 0U);
  const auto observation = signals->ReadSignal(id("on")).value();
  EXPECT_EQ(observation.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(observation.latest_sample->received_at_ns, 0);
  EXPECT_EQ(provider->status().last_ingress_source_sequence, 1U);
  EXPECT_EQ(signals->ReadWatch(signals->RegisterLevelWatch(watch()).value())
                .value()
                .state,
            core::IoWatchState::kMatched);
  auto opposite = watch();
  opposite.expected_value = false;
  const auto waiting = signals->RegisterLevelWatch(opposite);
  ASSERT_TRUE(waiting.ok());
  clock.set(2'000'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(signals->ReadWatch(waiting.value()).value().state,
            core::IoWatchState::kPending);
}

TEST_F(MockBooleanInputProviderTest,
       ExplicitChangesUseSharedObservationAndLastSetWins) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  clock.set(3);
  ASSERT_EQ(
      provider->SetValue(id("off"), true).value().enqueued_source_sequence, 2U);
  EXPECT_EQ(
      signals->ReadSignal(id("off")).value().latest_sample->publication.value,
      false);
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(
      signals->ReadSignal(id("off")).value().latest_sample->publication.value,
      true);
  clock.set(4);
  ASSERT_EQ(
      provider->SetValue(id("off"), false).value().enqueued_source_sequence,
      3U);
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto off = signals->ReadSignal(id("off")).value();
  const auto on = signals->ReadSignal(id("on")).value();
  EXPECT_EQ(off.latest_sample->publication.value, false);
  EXPECT_EQ(on.latest_sample->publication.value, true);
  EXPECT_EQ(off.latest_sample->received_at_ns, 4);
  EXPECT_EQ(off.latest_sample->publication.source_sequence, 3U);
  EXPECT_EQ(off.cursor, on.cursor);
  EXPECT_EQ(provider->PublishInitial().value(), 1U);
  EXPECT_EQ(provider->status().last_ingress_source_sequence, 3U);
}

TEST_F(MockBooleanInputProviderTest,
       EqualValueIsAnExplicitSnapshotWithNewReceiptAndNoDirectSuccess) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  clock.set(9);
  ASSERT_EQ(
      provider->SetValue(id("off"), false).value().enqueued_source_sequence,
      2U);
  EXPECT_EQ(signals->ReadSignal(id("off"))
                .value()
                .latest_sample->publication.source_sequence,
            1U);
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observation = signals->ReadSignal(id("off")).value();
  EXPECT_EQ(observation.latest_sample->publication.value, false);
  EXPECT_EQ(observation.latest_sample->publication.source_sequence, 2U);
  EXPECT_EQ(observation.latest_sample->received_at_ns, 9);
}

TEST_F(MockBooleanInputProviderTest,
       FailedIngressKeepsModelChangeAndRealLossUntilNextExplicitEvent) {
  Build(1);
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  ASSERT_TRUE(provider->SetValue(id("off"), true).ok());
  const auto changed = provider->SetValue(id("on"), false);
  ASSERT_TRUE(changed.ok());
  EXPECT_FALSE(changed.value().model_value);
  EXPECT_FALSE(changed.value().enqueued_source_sequence);
  EXPECT_TRUE(changed.value().publication_error);
  EXPECT_EQ(provider->status().last_ingress_source_sequence, 3U);
  EXPECT_EQ(provider->status().last_enqueued_source_sequence, 2U);
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(signals->ReadSignal(id("on")).value().availability,
            core::IoSignalAvailability::kUnavailable);
  clock.set(1'000'000'000'000);
  scheduler.advance_to(clock.now());
  EXPECT_EQ(ingress->Drain(8).value().attempted, 0U);
  EXPECT_EQ(signals->ReadSignal(id("on")).value().availability,
            core::IoSignalAvailability::kUnavailable);
  ASSERT_EQ(
      provider->SetValue(id("off"), true).value().enqueued_source_sequence, 4U);
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto off = signals->ReadSignal(id("off")).value();
  const auto on = signals->ReadSignal(id("on")).value();
  EXPECT_EQ(off.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(on.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(off.latest_sample->publication.value, true);
  EXPECT_EQ(on.latest_sample->publication.value, false);
  EXPECT_GT(on.continuity_loss_generation, 0U);
}

TEST_F(MockBooleanInputProviderTest,
       ApplyReplacementRestoresInitialValuesAndRetiresOldProvider) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  ASSERT_TRUE(provider->SetValue(id("off"), true).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto retired = std::move(provider);
  retired->Shutdown();
  ASSERT_TRUE(ingress->Drain(8).ok());
  auto next = source();
  next.incarnation = core::IoSourceIncarnation("activation.2");
  ASSERT_TRUE(
      signals->InstallSource(kSource, next.incarnation, kIncarnation).ok());
  auto queued = core::IoSignalIngress::Create(signals);
  ASSERT_TRUE(queued.ok());
  ingress = std::shared_ptr<core::IoSignalIngress>(std::move(queued.value()));
  auto provided =
      MockBooleanInputProvider::Create(ingress, clock, std::move(next));
  ASSERT_TRUE(provided.ok());
  provider = std::move(provided.value());
  EXPECT_FALSE(retired->SetValue(id("off"), true).ok());
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  const auto observation = signals->ReadSignal(id("off")).value();
  EXPECT_EQ(observation.latest_sample->publication.value, false);
  EXPECT_EQ(observation.latest_sample->publication.incarnation.value(),
            "activation.2");
}

TEST_F(MockBooleanInputProviderTest,
       RetainedDriverApplyRebindResetsInitialModelOnce) {
  Build();
  ASSERT_TRUE(provider);
  const auto *actual_driver = provider.get();
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(provider->SetValue(id("off"), true).ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  std::shared_ptr<core::IoSignalSubsystem> next_signals;
  std::shared_ptr<core::IoSignalIngress> next_ingress;
  const core::IoSourceIncarnation incarnation("activation.2");
  Successor(incarnation, next_signals, next_ingress);
  ASSERT_TRUE(next_signals && next_ingress);
  const auto rebound = provider->RebindIngress(next_ingress, incarnation, 1U);
  ASSERT_TRUE(rebound.ok());
  EXPECT_EQ(rebound.value(), 1U);
  EXPECT_EQ(provider.get(), actual_driver);
  EXPECT_TRUE(provider->status().accepting);
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_NE(signals->ReadSignal(id("off")).value().availability,
            core::IoSignalAvailability::kFresh);
  ASSERT_TRUE(next_ingress->Drain(8).ok());
  const auto baseline = next_signals->ReadSignal(id("off")).value();
  ASSERT_EQ(baseline.availability, core::IoSignalAvailability::kFresh);
  EXPECT_EQ(baseline.latest_sample->publication.incarnation, incarnation);
  EXPECT_EQ(baseline.latest_sample->publication.value, false);
  ASSERT_TRUE(provider->SetValue(id("off"), true).ok());
  EXPECT_EQ(provider->RebindIngress(next_ingress, incarnation, 1U).value(), 1U);
  ASSERT_TRUE(next_ingress->Drain(8).ok());
  const auto changed = next_signals->ReadSignal(id("off")).value();
  EXPECT_EQ(changed.latest_sample->publication.value, true);
  EXPECT_EQ(changed.latest_sample->publication.source_sequence, 2U);
  EXPECT_EQ(provider->Resume(1U).value(), 1U);
}

TEST_F(MockBooleanInputProviderTest,
       PausedApplyRebindResetsModelButEnableKeepsLaterManualChange) {
  Build(8, 1U);
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->SetValue(id("off"), true).ok());
  std::shared_ptr<core::IoSignalSubsystem> next_signals;
  std::shared_ptr<core::IoSignalIngress> next_ingress;
  const core::IoSourceIncarnation incarnation("activation.2");
  Successor(incarnation, next_signals, next_ingress);
  ASSERT_TRUE(next_signals && next_ingress);
  const auto rebound = provider->RebindIngress(next_ingress, incarnation, 2U);
  ASSERT_TRUE(rebound.ok());
  EXPECT_FALSE(rebound.value());
  EXPECT_FALSE(provider->status().accepting);
  ASSERT_TRUE(next_ingress->Drain(8).ok());
  EXPECT_NE(next_signals->ReadSignal(id("off")).value().availability,
            core::IoSignalAvailability::kFresh);
  const auto manual = provider->SetValue(id("off"), true);
  ASSERT_TRUE(manual.ok());
  EXPECT_TRUE(manual.value().model_value);
  EXPECT_FALSE(manual.value().enqueued_source_sequence);
  ASSERT_TRUE(provider->Resume(3U).ok());
  ASSERT_TRUE(next_ingress->Drain(8).ok());
  EXPECT_EQ(next_signals->ReadSignal(id("off"))
                .value()
                .latest_sample->publication.value,
            true);
}

TEST_F(MockBooleanInputProviderTest,
       ShutdownFencesDelayedHostSetAndDoesNotHideDisconnection) {
  Build();
  ASSERT_TRUE(provider);
  ASSERT_TRUE(provider->PublishInitial().ok());
  ASSERT_TRUE(ingress->Drain(8).ok());
  bool accepted = true;
  scheduler.schedule(
      5, [&] { accepted = provider->SetValue(id("off"), true).ok(); },
      core::SchedulerPriority::RuntimeSafety);
  provider->Shutdown();
  provider->Shutdown();
  clock.set(5);
  scheduler.advance_to(clock.now());
  EXPECT_FALSE(accepted);
  EXPECT_EQ(provider->status().last_ingress_source_sequence, 1U);
  ASSERT_TRUE(ingress->Drain(8).ok());
  EXPECT_EQ(signals->ReadSignal(id("off")).value().availability,
            core::IoSignalAvailability::kUnavailable);
}

class RefusingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &) noexcept override {
    ++attempted;
    return false;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &) noexcept override {}
  std::size_t attempted = 0;
};
TEST(MockBooleanInputProviderConfigTest,
     FailedApplyBaselineIsNotResentByExactRebindReplay) {
  core::FakeClock clock;
  auto old = std::make_shared<RefusingIngress>();
  auto next = std::make_shared<RefusingIngress>();
  auto created = MockBooleanInputProvider::Create(old, clock, source());
  ASSERT_TRUE(created.ok());
  auto provider = std::move(created.value());
  const core::IoSourceIncarnation incarnation("activation.2");
  EXPECT_FALSE(provider->RebindIngress(next, incarnation, 1U).ok());
  EXPECT_FALSE(provider->RebindIngress(next, incarnation, 1U).ok());
  EXPECT_EQ(next->attempted, 1U);
  const auto manual = provider->SetValue(id("off"), true);
  ASSERT_TRUE(manual.ok());
  EXPECT_TRUE(manual.value().model_value);
  EXPECT_TRUE(manual.value().publication_error);
  EXPECT_EQ(next->attempted, 2U);
}
TEST(
    MockBooleanInputProviderConfigTest,
    RefusedInitialSnapshotIsNotAutomaticallyResentAndModelSetReportsPublication) {
  core::FakeClock clock;
  auto ingress = std::make_shared<RefusingIngress>();
  auto created = MockBooleanInputProvider::Create(ingress, clock, source());
  ASSERT_TRUE(created.ok());
  auto provider = std::move(created.value());
  EXPECT_FALSE(provider->PublishInitial().ok());
  EXPECT_FALSE(provider->PublishInitial().ok());
  EXPECT_EQ(ingress->attempted, 1U);
  const auto changed = provider->SetValue(id("off"), true);
  ASSERT_TRUE(changed.ok());
  EXPECT_TRUE(changed.value().model_value);
  EXPECT_FALSE(changed.value().enqueued_source_sequence);
  EXPECT_TRUE(changed.value().publication_error);
  EXPECT_EQ(ingress->attempted, 2U);
  EXPECT_EQ(provider->status().last_enqueued_source_sequence, 0U);
}

class ContendedAcquisitionClock final : public core::IClock {
public:
  ContendedAcquisitionClock() : released(release.get_future().share()) {}
  core::TimePoint now() const override {
    if (!armed.load())
      return 0;
    const auto read = reads.fetch_add(1);
    if (read == 0) {
      entered.set_value();
      released.wait();
      return 1;
    }
    if (read == 1)
      second_entered.set_value();
    return 2;
  }
  std::atomic<bool> armed = false;
  mutable std::atomic<unsigned> reads = 0;
  mutable std::promise<void> entered;
  mutable std::promise<void> second_entered;
  std::promise<void> release;
  std::shared_future<void> released;
};
class CapturingIngress final : public core::IIoSignalIngress {
public:
  bool TryEnqueue(const core::IoSourceAcquisitionDTO &frame) noexcept override {
    std::lock_guard lock(mutex);
    frames.push_back(frame);
    return true;
  }
  void
  NotifyLoss(const core::IoIngressLossNotificationDTO &) noexcept override {}
  std::mutex mutex;
  std::vector<core::IoSourceAcquisitionDTO> frames;
};
TEST(MockBooleanInputProviderConcurrencyTest,
     ModelMutationSerializesTwoWriterAcquisitionTimes) {
  using namespace std::chrono_literals;
  ContendedAcquisitionClock clock;
  auto ingress = std::make_shared<CapturingIngress>();
  auto created = MockBooleanInputProvider::Create(ingress, clock, source());
  ASSERT_TRUE(created.ok());
  auto provider = std::move(created.value());
  ASSERT_TRUE(provider->PublishInitial().ok());
  auto selected = clock.entered.get_future();
  auto second_clock_entry = clock.second_entered.get_future();
  clock.armed.store(true);
  // Hold the first writer at actual acquisition while a second host writer
  // enters. No sleep, periodic sampler or wall-clock semantic deadline.
  auto first = std::async(std::launch::async,
                          [&] { return provider->SetValue(id("off"), true); });
  EXPECT_EQ(selected.wait_for(2s), std::future_status::ready);
  std::promise<void> second_started;
  auto started = second_started.get_future();
  auto second = std::async(std::launch::async, [&] {
    second_started.set_value();
    return provider->SetValue(id("off"), false);
  });
  EXPECT_EQ(started.wait_for(2s), std::future_status::ready);
  // Synchronization bound only: the selected first writer holds the model
  // mutex. A second writer must not read acquisition time or complete yet.
  // The old pre-lock stamp reaches second_entered while the first is held.
  EXPECT_EQ(second_clock_entry.wait_for(250ms), std::future_status::timeout);
  EXPECT_EQ(second.wait_for(0ms), std::future_status::timeout);
  clock.release.set_value();
  const auto first_result = first.get();
  const auto second_result = second.get();
  ASSERT_TRUE(first_result.ok());
  ASSERT_TRUE(second_result.ok());
  EXPECT_EQ(first_result.value().enqueued_source_sequence, 2U);
  EXPECT_EQ(second_result.value().enqueued_source_sequence, 3U);
  ASSERT_EQ(ingress->frames.size(), 3U);
  EXPECT_EQ(ingress->frames[1].trusted_received_at_ns, 1);
  EXPECT_EQ(ingress->frames[2].trusted_received_at_ns, 2);
  EXPECT_EQ(ingress->frames[1].updates[0].value, true);
  EXPECT_EQ(ingress->frames[2].updates[0].value, false);
}

TEST(MockBooleanInputProviderConfigTest,
     RefusesLegacySampledAndUnsupportedOrAmbiguousInputs) {
  core::FakeClock clock;
  auto ingress = std::make_shared<RefusingIngress>();
  auto invalid = source();
  invalid.inputs[0].descriptor.freshness = {core::IoFreshnessMode::kSampleAge,
                                            10};
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[0].descriptor.freshness.maximum_age_ns = 10;
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[0].descriptor.update_mode = core::IoSignalUpdateMode::kSampled;
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[1].descriptor.source_id = core::InterruptSourceId("wrong");
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[1] = invalid.inputs[0];
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[0].descriptor.transition_capability =
      core::IoTransitionCapability::kCaptured;
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
  invalid = source();
  invalid.inputs[0].descriptor.direction = core::IoSignalDirection::kOutput;
  EXPECT_FALSE(MockBooleanInputProvider::Create(ingress, clock, invalid).ok());
}

} // namespace
} // namespace manyforge::roboplan_backend

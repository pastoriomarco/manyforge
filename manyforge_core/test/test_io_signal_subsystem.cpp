#include <gtest/gtest.h>

#include <manyforge_core/execution_event_change_signal.hpp>
#include <manyforge_core/io_signal_subsystem.hpp>
#include <manyforge_core/time/fake_clock.hpp>
#include <utility>

namespace manyforge::core {
namespace {

const InterruptSourceId kSourceA("source.a");
const InterruptSourceId kSourceB("source.b");
const IoSourceIncarnation kIncarnation("provider.1");

IoSignalId signal_id(std::string name = "input") {
  return {ControllableComponentId("component.io"), std::move(name)};
}

IoSignalDescriptorDTO descriptor(std::string name = "input", InterruptSourceId source = kSourceA) {
  return {signal_id(std::move(name)),        std::move(source),
          IoSignalValueType::kBoolean,       IoSignalDirection::kInput,
          IoSignalUpdateMode::kSampled,      IoTransitionCapability::kSampledOnly,
          {IoFreshnessMode::kSampleAge, 10}, std::nullopt,
          IoSignalLivenessMode::kPeriodic};
}

IoSignalLimitsDTO limits(std::size_t records = 8, std::size_t watches = 8) {
  return {2, 4, records, watches, 512, 1024};
}

IoSignalCatalogDTO catalog(std::vector<IoSignalDescriptorDTO> signals = {descriptor()}) {
  return {RequestOwnerEpoch("host.1"), 1, std::move(signals)};
}

IoWatchScopeDTO scope(std::string run = "run.1", std::string owner = "runner.1") {
  return {RequestOwnerEpoch(std::move(owner)),
          ProgramRunId(std::move(run)),
          {ProgramNodeInstanceId("wait"), "attempt.1"}};
}

IoSignalPublicationDTO publication(std::uint64_t sequence, bool value, std::string name = "input",
                                   InterruptSourceId source = kSourceA) {
  return {signal_id(std::move(name)),
          std::move(source),
          kIncarnation,
          sequence,
          IoSampleValidity::kValid,
          value,
          std::nullopt};
}

IoSourceAcquisitionDTO acquisition(const IoSignalPublicationDTO& sample, TimePoint received) {
  return {sample.source_id,
          sample.incarnation,
          sample.source_sequence,
          received,
          {{sample.signal_id, sample.validity, sample.value, sample.error}}};
}

IoSourceCursorDTO origin(InterruptSourceId source = kSourceA) {
  return {RequestOwnerEpoch("host.1"), 1, std::move(source), 0};
}

TEST(IoSignalSubsystemTest, ValidatesWholeCatalogAndExplicitBoundsBeforeConstruction) {
  FakeClock clock;
  auto duplicate = catalog({descriptor(), descriptor()});
  EXPECT_FALSE(IoSignalSubsystem::Create(std::move(duplicate), limits(), clock).ok());
  auto unsupported = catalog();
  unsupported.signals.front().freshness = {IoFreshnessMode::kProviderValidity, std::nullopt};
  const auto refused = IoSignalSubsystem::Create(std::move(unsupported), limits(), clock);
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "invalid_request");
  auto no_age = catalog();
  no_age.signals.front().freshness.maximum_age_ns = 0;
  EXPECT_FALSE(IoSignalSubsystem::Create(std::move(no_age), limits(), clock).ok());
  auto no_records = limits();
  no_records.records_per_source = 0;
  EXPECT_FALSE(IoSignalSubsystem::Create(catalog(), no_records, clock).ok());
  auto too_many_sources = limits();
  too_many_sources.maximum_sources = 1;
  EXPECT_FALSE(IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceB)}),
                                         too_many_sources, clock)
                   .ok());
  auto output = catalog();
  output.signals.front().direction = IoSignalDirection::kOutput;
  EXPECT_FALSE(IoSignalSubsystem::Create(output, limits(), clock).ok());
  output.signals.front().output_resource = ResourceId("resource.output");
  output.signals.front().liveness.reset();
  EXPECT_TRUE(IoSignalSubsystem::Create(std::move(output), limits(), clock).ok());
}

TEST(IoSignalSubsystemTest, FalseMissingInvalidUnavailableAndStaleRemainDistinct) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kMissing);
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, false), clock.now())).ok());
  const auto fresh = subsystem->ReadSignal(signal_id());
  ASSERT_TRUE(fresh.ok());
  EXPECT_EQ(fresh.value().availability, IoSignalAvailability::kFresh);
  EXPECT_EQ(fresh.value().latest_sample->publication.value, false);
  clock.advance(10);
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability, IoSignalAvailability::kFresh);
  clock.advance(1);
  const auto stale = subsystem->ReadSignal(signal_id());
  EXPECT_EQ(stale.value().availability, IoSignalAvailability::kStale);
  EXPECT_EQ(stale.value().latest_sample->received_at_ns, 0);
  EXPECT_EQ(stale.value().latest_sample->publication.value, false);
  auto invalid = publication(2, false);
  invalid.validity = IoSampleValidity::kInvalid;
  invalid.value.reset();
  ASSERT_TRUE(subsystem->Publish(acquisition(invalid, clock.now())).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kInvalid);
  ASSERT_TRUE(subsystem
                  ->MarkSourceUnavailable(kSourceA, kIncarnation, 2,
                                          {"source_disconnected", "provider unavailable", {}})
                  .ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
}

TEST(IoSignalSubsystemTest, RejectedIdentitySequenceAndShapeNeverMutateCurrentSample) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  EXPECT_FALSE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, false), clock.now())).ok());
  const auto before = subsystem->ReadSignal(signal_id()).value();
  auto wrong_incarnation = publication(2, true);
  wrong_incarnation.incarnation = IoSourceIncarnation("provider.old");
  EXPECT_FALSE(subsystem->Publish(acquisition(wrong_incarnation, clock.now())).ok());
  EXPECT_FALSE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  auto no_value = publication(2, true);
  no_value.value.reset();
  EXPECT_FALSE(subsystem->Publish(acquisition(no_value, clock.now())).ok());
  auto wrong_source = publication(2, true);
  wrong_source.source_id = kSourceB;
  EXPECT_FALSE(subsystem->Publish(acquisition(wrong_source, clock.now())).ok());
  const auto after = subsystem->ReadSignal(signal_id()).value();
  EXPECT_EQ(after.latest_sample, before.latest_sample);
  EXPECT_EQ(after.cursor, before.cursor);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().continuity_loss_generation, 0U);
}

TEST(IoSignalSubsystemTest, OrderedRetentionAndReaderCursorsAreIndependentPerSource) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceB)}),
                                           limits(2), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto a0 = subsystem->Publish(acquisition(publication(1, false), clock.now()));
  ASSERT_TRUE(a0.ok());
  ASSERT_TRUE(
      subsystem->Publish(acquisition(publication(1, false, "other", kSourceB), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(3, false), clock.now())).ok());
  auto page = subsystem->ReadRecords(a0.value(), 2);
  ASSERT_TRUE(page.ok());
  EXPECT_FALSE(page.value().retention_gap);
  ASSERT_EQ(page.value().records.size(), 2U);
  EXPECT_EQ(page.value().records.front().acquisition->updates.front().value, true);
  EXPECT_EQ(page.value().records.back().acquisition->updates.front().value, false);
  EXPECT_EQ(subsystem->ReadRecords(a0.value(), 2).value().records.size(), 2U);
  EXPECT_TRUE(subsystem->ReadRecords(origin(), 2).value().retention_gap);
  EXPECT_FALSE(subsystem->ReadRecords(origin(kSourceB), 2).value().retention_gap);
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().continuity_loss_generation, 0U);
  auto single = subsystem->ReadRecords(a0.value(), 1);
  ASSERT_TRUE(single.ok());
  EXPECT_NE(single.value().next_cursor, single.value().latest_cursor);
  EXPECT_EQ(subsystem->ReadRecords(single.value().next_cursor, 1).value().records.size(), 1U);
}

TEST(IoSignalSubsystemTest, SequenceGapAndActualIngressLossDoNotUnqualifyAnotherSource) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceB)}),
                                           limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto baseline = subsystem->Publish(acquisition(publication(1, false), clock.now()));
  ASSERT_TRUE(baseline.ok());
  ASSERT_TRUE(
      subsystem->Publish(acquisition(publication(1, true, "other", kSourceB), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(4, false), clock.now())).ok());
  const auto records = subsystem->ReadRecords(baseline.value(), 8);
  ASSERT_TRUE(records.ok());
  ASSERT_EQ(records.value().records.size(), 2U);
  ASSERT_TRUE(records.value().records.front().loss);
  EXPECT_EQ(records.value().records.front().loss->first_lost_source_sequence, 2U);
  EXPECT_EQ(records.value().records.front().loss->last_lost_source_sequence, 3U);
  ASSERT_TRUE(subsystem
                  ->ReportIngressOverflow(kSourceA, kIncarnation,
                                          {"ingress_overflow", "source queue lost records", {}})
                  .ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
  const auto unaffected = subsystem->ReadSignal(signal_id("other"));
  EXPECT_EQ(unaffected.value().availability, IoSignalAvailability::kFresh);
  EXPECT_EQ(unaffected.value().continuity_loss_generation, 0U);
}

TEST(IoSignalSubsystemTest, LevelWatchRetainsSingleAcquiredMatchAfterLatestAndHistoryOverwrite) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(1), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, false), clock.now())).ok());
  clock.advance(11);
  const auto result = subsystem->ReadWatch(watch.value());
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value().state, IoWatchState::kMatched);
  EXPECT_EQ(result.value().matched_sample->publication.source_sequence, 1U);
  EXPECT_EQ(result.value().matched_sample->publication.value, true);
  EXPECT_EQ(result.value().observation->latest_sample->publication.value, false);
  EXPECT_EQ(result.value().observation->availability, IoSignalAvailability::kStale);
}

TEST(IoSignalSubsystemTest, StaleOrUnavailableBaselineStaysPendingUntilFreshMatchingPublication) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  const auto fresh_watch = subsystem->RegisterLevelWatch({scope("fresh"), signal_id(), true});
  ASSERT_TRUE(fresh_watch.ok());
  EXPECT_EQ(subsystem->ReadWatch(fresh_watch.value()).value().state, IoWatchState::kMatched);
  clock.advance(11);
  const auto stale_watch = subsystem->RegisterLevelWatch({scope("stale"), signal_id(), true});
  ASSERT_TRUE(stale_watch.ok());
  EXPECT_EQ(subsystem->ReadWatch(stale_watch.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(
      subsystem
          ->MarkSourceUnavailable(kSourceA, kIncarnation, 1, {"source_disconnected", "lost", {}})
          .ok());
  EXPECT_EQ(subsystem->ReadWatch(stale_watch.value()).value().observation->availability,
            IoSignalAvailability::kUnavailable);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadWatch(stale_watch.value()).value().state, IoWatchState::kMatched);
}

TEST(IoSignalSubsystemTest, ExplicitSourceReplacementInvalidatesBaselineAndFencesLateSamples) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, false), clock.now())).ok());
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  const IoSourceIncarnation replacement("provider.2");
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, replacement, kIncarnation).ok());
  const auto replaced = subsystem->ReadSignal(signal_id());
  EXPECT_FALSE(replaced.value().latest_sample);
  EXPECT_EQ(replaced.value().availability, IoSignalAvailability::kUnavailable);
  EXPECT_EQ(replaced.value().continuity_loss_generation, 1U);
  // An old queued installer cannot reset this source back to its predecessor.
  const auto stale_install = subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt);
  ASSERT_FALSE(stale_install.ok());
  EXPECT_EQ(stale_install.error().details.at("reason"), "io_source_install_predecessor_stale");
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().installed_incarnation, replacement);
  // Repeating the accepted installation is inert even with its old precondition.
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, replacement, kIncarnation).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().continuity_loss_generation, 1U);
  EXPECT_FALSE(subsystem->Publish(acquisition(publication(2, true), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  auto current = publication(1, true);
  current.incarnation = replacement;
  ASSERT_TRUE(subsystem->Publish(acquisition(current, clock.now())).ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().matched_sample->publication.incarnation,
            replacement);
}

TEST(IoSignalSubsystemTest, ExactReleaseAndRunTeardownCannotReleaseOtherOwnerOrReviveTokens) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  const auto own = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  const auto other = subsystem->RegisterLevelWatch({scope("run.2"), signal_id(), true});
  ASSERT_TRUE(own.ok());
  ASSERT_TRUE(other.ok());
  auto forged = own.value();
  forged.scope.program_run_id = ProgramRunId("run.2");
  EXPECT_FALSE(subsystem->ReleaseWatch(forged).ok());
  auto stale = own.value();
  ++stale.catalog_generation;
  EXPECT_FALSE(subsystem->ReadWatch(stale).ok());
  ASSERT_TRUE(
      subsystem->ReleaseRunWatches(RequestOwnerEpoch("runner.other"), ProgramRunId("run.1")).ok());
  EXPECT_EQ(subsystem->ReadWatch(own.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(
      subsystem->ReleaseRunWatches(RequestOwnerEpoch("runner.1"), ProgramRunId("run.1")).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadWatch(own.value()).value().state, IoWatchState::kReleased);
  EXPECT_EQ(subsystem->ReadWatch(other.value()).value().state, IoWatchState::kMatched);
  EXPECT_TRUE(subsystem->ReleaseWatch(own.value()).ok());
}

TEST(IoSignalSubsystemTest, RegistrationBoundFreesCapacityWithoutReusingReleasedIdentity) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(2, 1), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  const auto first = subsystem->RegisterLevelWatch({scope(), signal_id(), false});
  ASSERT_TRUE(first.ok());
  const auto refused = subsystem->RegisterLevelWatch({scope(), signal_id(), false});
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().details.at("reason"), "io_watch_bound_exceeded");
  ASSERT_TRUE(subsystem->ReleaseWatch(first.value()).ok());
  const auto second = subsystem->RegisterLevelWatch({scope(), signal_id(), false});
  ASSERT_TRUE(second.ok());
  EXPECT_GT(second.value().registration_id, first.value().registration_id);
  EXPECT_EQ(subsystem->ReadWatch(first.value()).value().state, IoWatchState::kReleased);
}

TEST(IoSignalSubsystemTest, SourceRecoveryRequiresEachSignalsOwnPostLossBaseline) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceA)}),
                                           limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, false), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true, "other"), clock.now())).ok());
  ASSERT_TRUE(subsystem
                  ->MarkSourceUnavailable(kSourceA, kIncarnation, 2,
                                          {"source_disconnected", "source unavailable", {}})
                  .ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(3, false), clock.now())).ok());
  const auto other = subsystem->ReadSignal(signal_id("other"));
  ASSERT_TRUE(other.ok());
  EXPECT_EQ(other.value().availability, IoSignalAvailability::kUnavailable);
  EXPECT_EQ(other.value().latest_sample->publication.value, true);
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id("other"), true});
  ASSERT_TRUE(watch.ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(4, true, "other"), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kMatched);
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().matched_sample->publication.source_sequence,
            4U);
}

TEST(IoSignalSubsystemTest, SequenceGapRequiresSiblingBaselineAndLateDisconnectCannotUndoRecovery) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceA)}),
                                           limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, true), clock.now())).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(3, false, "other"), clock.now())).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(4, true), clock.now())).ok());
  const auto before = subsystem->ReadSignal(signal_id()).value();
  const auto late = subsystem->MarkSourceUnavailable(
      kSourceA, kIncarnation, 1, {"source_disconnected", "delayed callback", {}});
  ASSERT_FALSE(late.ok());
  EXPECT_EQ(late.error().details.at("reason"), "io_source_availability_sequence_stale");
  const auto after = subsystem->ReadSignal(signal_id()).value();
  EXPECT_EQ(after.availability, IoSignalAvailability::kFresh);
  EXPECT_EQ(after.latest_sample, before.latest_sample);
  EXPECT_EQ(after.cursor, before.cursor);
  EXPECT_EQ(after.continuity_loss_generation, before.continuity_loss_generation);
  // Real loss is still recorded even if ordinary publication has advanced.
  ASSERT_TRUE(subsystem
                  ->ReportIngressOverflow(kSourceA, kIncarnation,
                                          {"ingress_overflow", "actual lost ingress", {}})
                  .ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
}

TEST(IoSignalSubsystemTest, DeferredExpiredSampleKeepsOriginalIngressAgeAndCannotMatchLevelWatch) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  const auto ingress_time = clock.now();
  clock.advance(11);  // The receiver's acquired sample expired while queued.
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, true), ingress_time)).ok());
  const auto observed = subsystem->ReadSignal(signal_id());
  EXPECT_EQ(observed.value().availability, IoSignalAvailability::kStale);
  EXPECT_EQ(observed.value().latest_sample->received_at_ns, ingress_time);
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  const auto records = subsystem->ReadRecords(origin(), 8);
  ASSERT_TRUE(records.ok());
  EXPECT_EQ(records.value().records.back().acquisition->trusted_received_at_ns, ingress_time);
  clock.advance(1);
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true), clock.now())).ok());
  const auto matched = subsystem->ReadWatch(watch.value());
  EXPECT_EQ(matched.value().state, IoWatchState::kMatched);
  EXPECT_EQ(matched.value().matched_sample->received_at_ns, 12);
}

TEST(IoSignalSubsystemTest, FutureNegativeAndSourceWideRegressedIngressTimesRefuseBeforeMutation) {
  FakeClock clock;
  clock.set(100);
  auto created = IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other", kSourceA)}),
                                           limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(1, false), 100)).ok());
  const auto before = subsystem->ReadSignal(signal_id()).value();
  const auto regressed = subsystem->Publish(acquisition(publication(2, true, "other"), 99));
  ASSERT_FALSE(regressed.ok());
  EXPECT_EQ(regressed.error().details.at("reason"), "io_receiver_timestamp_regressed");
  EXPECT_FALSE(subsystem->Publish(acquisition(publication(2, true, "other"), 101)).ok());
  EXPECT_FALSE(subsystem->Publish(acquisition(publication(2, true, "other"), -1)).ok());
  const auto after = subsystem->ReadSignal(signal_id()).value();
  EXPECT_EQ(after.cursor, before.cursor);
  EXPECT_EQ(after.latest_sample, before.latest_sample);
  EXPECT_EQ(after.continuity_loss_generation, before.continuity_loss_generation);
  EXPECT_FALSE(subsystem->ReadSignal(signal_id("other")).value().latest_sample);
  // Refusal did not consume sequence 2; equal timestamps across siblings are valid.
  ASSERT_TRUE(subsystem->Publish(acquisition(publication(2, true, "other"), 100)).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id("other")).value().availability,
            IoSignalAvailability::kFresh);
  EXPECT_EQ(subsystem->ReadSignal(signal_id("other")).value().continuity_loss_generation, 0U);
}

TEST(IoSignalSubsystemTest, AtomicAcquisitionCommitsOneRecordAndSnapshotBeforeWatchReads) {
  FakeClock clock;
  auto created =
      IoSignalSubsystem::Create(catalog({descriptor(), descriptor("other")}), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  auto frame = acquisition(publication(1, true), clock.now());
  frame.updates.push_back({signal_id("other"), IoSampleValidity::kValid, false, std::nullopt});
  const auto committed = subsystem->Publish(frame);
  ASSERT_TRUE(committed.ok());
  EXPECT_EQ(committed.value().record_sequence, 2U);  // installation + one acquisition
  const std::vector<IoSignalId> ids{signal_id(), signal_id("other")};
  const auto snapshot = subsystem->ReadSignals(ids);
  ASSERT_TRUE(snapshot.ok());
  ASSERT_EQ(snapshot.value().size(), 2U);
  EXPECT_EQ(snapshot.value()[0].latest_sample->publication.value, true);
  EXPECT_EQ(snapshot.value()[1].latest_sample->publication.value, false);
  EXPECT_EQ(snapshot.value()[0].cursor, snapshot.value()[1].cursor);
  EXPECT_EQ(snapshot.value()[0].latest_sample->publication.source_sequence, 1U);
  EXPECT_EQ(snapshot.value()[1].latest_sample->publication.source_sequence, 1U);
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kMatched);
  const auto records = subsystem->ReadRecords(origin(), 8);
  ASSERT_TRUE(records.ok());
  ASSERT_TRUE(records.value().records.back().acquisition);
  EXPECT_EQ(*records.value().records.back().acquisition, frame);
}

TEST(IoSignalSubsystemTest, InvalidLastUpdateDuplicateAndForeignSignalsRefuseWholeAcquisition) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(
      catalog({descriptor(), descriptor("other"), descriptor("foreign", kSourceB)}), limits(),
      clock);
  ASSERT_TRUE(created.ok());
  auto subsystem = std::move(created.value());
  ASSERT_TRUE(subsystem->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(subsystem->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto watch = subsystem->RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  const auto before = subsystem->ReadSignal(signal_id()).value().cursor;
  auto frame = acquisition(publication(1, true), clock.now());
  frame.updates.push_back(
      {signal_id("other"), IoSampleValidity::kValid, std::nullopt, std::nullopt});
  EXPECT_FALSE(subsystem->Publish(frame).ok());
  frame.updates.back() = frame.updates.front();
  EXPECT_FALSE(subsystem->Publish(frame).ok());
  frame.updates.back() = {signal_id("foreign"), IoSampleValidity::kValid, false, std::nullopt};
  EXPECT_FALSE(subsystem->Publish(frame).ok());
  EXPECT_EQ(subsystem->ReadSignal(signal_id()).value().cursor, before);
  EXPECT_FALSE(subsystem->ReadSignal(signal_id()).value().latest_sample);
  EXPECT_EQ(subsystem->ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  frame.updates.back() = {signal_id("other"), IoSampleValidity::kValid, false, std::nullopt};
  EXPECT_TRUE(subsystem->Publish(frame).ok());  // rejected sequence was not consumed
}

IoSignalCatalogDTO synthetic_catalog() {
  auto installed = catalog();
  installed.authoritative_synthetic_sources = {kSourceA};
  installed.signals.front().update_mode = IoSignalUpdateMode::kChangeOnly;
  installed.signals.front().freshness = {IoFreshnessMode::kProviderValidity, std::nullopt};
  installed.signals.front().liveness = IoSignalLivenessMode::kInProcess;
  return installed;
}

TEST(IoSignalSubsystemTest, AuthoritativeSyntheticCapabilityRequiresExactInstalledSourceContract) {
  FakeClock clock;
  EXPECT_TRUE(IoSignalSubsystem::Create(synthetic_catalog(), limits(), clock).ok());
  auto duplicate = synthetic_catalog();
  duplicate.authoritative_synthetic_sources.push_back(kSourceA);
  EXPECT_FALSE(IoSignalSubsystem::Create(duplicate, limits(), clock).ok());
  auto unknown = synthetic_catalog();
  unknown.authoritative_synthetic_sources.push_back(kSourceB);
  EXPECT_FALSE(IoSignalSubsystem::Create(unknown, limits(), clock).ok());
  auto sampled = synthetic_catalog();
  sampled.signals.front().update_mode = IoSignalUpdateMode::kSampled;
  EXPECT_FALSE(IoSignalSubsystem::Create(sampled, limits(), clock).ok());
  auto aged = synthetic_catalog();
  aged.signals.front().freshness.maximum_age_ns = 10;
  EXPECT_FALSE(IoSignalSubsystem::Create(aged, limits(), clock).ok());
  auto unqualified = synthetic_catalog();
  unqualified.authoritative_synthetic_sources.clear();
  EXPECT_FALSE(IoSignalSubsystem::Create(unqualified, limits(), clock).ok());
  auto mixed = synthetic_catalog();
  mixed.signals.push_back(descriptor("real", kSourceB));
  EXPECT_TRUE(IoSignalSubsystem::Create(mixed, limits(), clock).ok());
  mixed.signals.back().freshness = {IoFreshnessMode::kProviderValidity, std::nullopt};
  mixed.signals.back().update_mode = IoSignalUpdateMode::kChangeOnly;
  EXPECT_FALSE(IoSignalSubsystem::Create(mixed, limits(), clock).ok());
}

TEST(IoSignalSubsystemTest, UnchangedSyntheticValueRemainsFreshWithoutRefreshButRealSampleAges) {
  FakeClock clock;
  auto installed = synthetic_catalog();
  installed.signals.push_back(descriptor("real", kSourceB));
  auto created = IoSignalSubsystem::Create(installed, limits(), clock);
  ASSERT_TRUE(created.ok());
  auto service = std::move(created.value());
  ASSERT_TRUE(service->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(service->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(service->Publish(acquisition(publication(1, false), 0)).ok());
  ASSERT_TRUE(service->Publish(acquisition(publication(1, false, "real", kSourceB), 0)).ok());
  const auto cursor = service->ReadSignal(signal_id()).value().cursor;
  clock.advance(1000000000000LL);
  const auto observed = service->ReadSignal(signal_id());
  ASSERT_TRUE(observed.ok());
  EXPECT_EQ(observed.value().availability, IoSignalAvailability::kFresh);
  EXPECT_EQ(observed.value().latest_sample->received_at_ns, 0);
  EXPECT_EQ(observed.value().cursor, cursor);  // no fabricated refresh records
  EXPECT_EQ(service->ReadSignal(signal_id("real")).value().availability,
            IoSignalAvailability::kStale);
  const auto immediate = service->RegisterLevelWatch({scope(), signal_id(), false});
  ASSERT_TRUE(immediate.ok());
  EXPECT_EQ(service->ReadWatch(immediate.value()).value().state, IoWatchState::kMatched);
}

TEST(IoSignalSubsystemTest, SyntheticLossStaysUnavailableUntilExplicitEventAndKeepsLossHistory) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(synthetic_catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto service = std::move(created.value());
  ASSERT_TRUE(service->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(service->Publish(acquisition(publication(1, false), 0)).ok());
  const auto before = service->ReadSignal(signal_id()).value().cursor;
  ASSERT_TRUE(
      service
          ->ReportIngressOverflow(kSourceA, kIncarnation, {"overflow", "actual ingress loss", {}})
          .ok());
  clock.advance(1000000000000LL);
  EXPECT_EQ(service->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
  const auto pending = service->RegisterLevelWatch({scope(), signal_id(), false});
  ASSERT_TRUE(pending.ok());
  EXPECT_EQ(service->ReadWatch(pending.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(service->Publish(acquisition(publication(2, false), clock.now())).ok());
  EXPECT_EQ(service->ReadWatch(pending.value()).value().state, IoWatchState::kMatched);
  EXPECT_EQ(service->ReadSignal(signal_id()).value().availability, IoSignalAvailability::kFresh);
  const auto history = service->ReadRecords(before, 8);
  ASSERT_TRUE(history.ok());
  ASSERT_EQ(history.value().records.size(), 2U);
  ASSERT_TRUE(history.value().records.front().loss);
  EXPECT_EQ(history.value().records.front().loss->reason, IoSourceLossReason::kIngressOverflow);
  ASSERT_TRUE(
      service
          ->MarkSourceUnavailable(kSourceA, kIncarnation, 2, {"shutdown", "provider stopped", {}})
          .ok());
  clock.advance(1000);
  EXPECT_EQ(service->ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
}

TEST(IoSignalSubsystemTest, SyntheticInvalidAndReplacementCannotBorrowPriorValidity) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(synthetic_catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto service = std::move(created.value());
  ASSERT_TRUE(service->InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(service->Publish(acquisition(publication(1, true), 0)).ok());
  auto invalid = publication(2, false);
  invalid.validity = IoSampleValidity::kInvalid;
  invalid.value.reset();
  ASSERT_TRUE(service->Publish(acquisition(invalid, 0)).ok());
  EXPECT_EQ(service->ReadSignal(signal_id()).value().availability, IoSignalAvailability::kInvalid);
  const IoSourceIncarnation replacement("provider.2");
  ASSERT_TRUE(service->InstallSource(kSourceA, replacement, kIncarnation).ok());
  EXPECT_NE(service->ReadSignal(signal_id()).value().availability, IoSignalAvailability::kFresh);
  EXPECT_FALSE(service->Publish(acquisition(publication(3, true), 0)).ok());
  auto current = publication(1, false);
  current.incarnation = replacement;
  ASSERT_TRUE(service->Publish(acquisition(current, 0)).ok());
  clock.advance(1000000000000LL);
  EXPECT_EQ(service->ReadSignal(signal_id()).value().availability, IoSignalAvailability::kFresh);
}

TEST(IoSignalSubsystemTest, NodeReleaseErasesOnlyExactOwnerRunNodeAttemptWatches) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto& service = *created.value();
  const auto own = scope();
  auto other_attempt = own;
  other_attempt.node.node_attempt_id = "attempt.2";
  const auto first = service.RegisterLevelWatch({own, signal_id(), true});
  const auto duplicate = service.RegisterLevelWatch({own, signal_id(), false});
  const auto next = service.RegisterLevelWatch({other_attempt, signal_id(), true});
  const auto other_run = service.RegisterLevelWatch({scope("run.2"), signal_id(), true});
  const auto other_owner =
      service.RegisterLevelWatch({scope("run.1", "runner.2"), signal_id(), true});
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(duplicate.ok());
  ASSERT_TRUE(next.ok());
  ASSERT_TRUE(other_run.ok());
  ASSERT_TRUE(other_owner.ok());
  ASSERT_TRUE(service.ReleaseNodeWatches(own).ok());
  EXPECT_TRUE(service.ReleaseNodeWatches(own).ok());
  EXPECT_EQ(service.ReadWatch(first.value()).value().state, IoWatchState::kReleased);
  EXPECT_EQ(service.ReadWatch(duplicate.value()).value().state, IoWatchState::kReleased);
  EXPECT_EQ(service.ReadWatch(next.value()).value().state, IoWatchState::kPending);
  EXPECT_EQ(service.ReadWatch(other_run.value()).value().state, IoWatchState::kPending);
  EXPECT_EQ(service.ReadWatch(other_owner.value()).value().state, IoWatchState::kPending);
  auto malformed = own;
  malformed.node.node_attempt_id.clear();
  EXPECT_FALSE(service.ReleaseNodeWatches(malformed).ok());
}

TEST(IoSignalSubsystemTest, RequiredLivenessModeCannotBorrowSyntheticAuthority) {
  FakeClock clock;
  auto missing = catalog();
  missing.signals.front().liveness.reset();
  EXPECT_FALSE(IoSignalSubsystem::Create(missing, limits(), clock).ok());
  auto unqualified = synthetic_catalog();
  unqualified.authoritative_synthetic_sources.clear();
  EXPECT_FALSE(IoSignalSubsystem::Create(unqualified, limits(), clock).ok());
  auto mismatched = catalog();
  mismatched.signals.front().liveness = IoSignalLivenessMode::kChangeOnly;
  EXPECT_FALSE(IoSignalSubsystem::Create(mismatched, limits(), clock).ok());
}

TEST(IoSignalSubsystemTest, ChangeOnlyRetainsObservedValueButNeverMasksLossOrInventsRefresh) {
  FakeClock clock;
  auto installed = catalog();
  auto& input = installed.signals.front();
  input.update_mode = IoSignalUpdateMode::kChangeOnly;
  input.freshness = {IoFreshnessMode::kProviderValidity, std::nullopt};
  input.liveness = IoSignalLivenessMode::kChangeOnly;
  auto created = IoSignalSubsystem::Create(installed, limits(), clock);
  ASSERT_TRUE(created.ok());
  auto& service = *created.value();
  ASSERT_TRUE(service.InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  EXPECT_EQ(service.ReadSignal(signal_id()).value().availability, IoSignalAvailability::kMissing);
  ASSERT_TRUE(service.Publish(acquisition(publication(1, false), 0)).ok());
  const auto before = service.ReadSignal(signal_id()).value().cursor;
  clock.advance(1000000000000LL);
  const auto current = service.ReadSignal(signal_id());
  ASSERT_TRUE(current.ok());
  EXPECT_EQ(current.value().availability, IoSignalAvailability::kFresh);
  EXPECT_EQ(current.value().cursor, before);
  EXPECT_EQ(current.value().latest_sample->received_at_ns, 0);
  ASSERT_TRUE(
      service
          .MarkSourceUnavailable(kSourceA, kIncarnation, 1, {"disconnect", "actual disconnect", {}})
          .ok());
  EXPECT_EQ(service.ReadSignal(signal_id()).value().availability,
            IoSignalAvailability::kUnavailable);
  const auto watch = service.RegisterLevelWatch({scope(), signal_id(), true});
  ASSERT_TRUE(watch.ok());
  EXPECT_EQ(service.ReadWatch(watch.value()).value().state, IoWatchState::kPending);
  ASSERT_TRUE(service.Publish(acquisition(publication(2, true), clock.now())).ok());
  EXPECT_EQ(service.ReadWatch(watch.value()).value().state, IoWatchState::kMatched);
}

TEST(IoSignalSubsystemTest, SameValuedPeriodicReceiptRefreshesItsActualReceiptBudget) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto& service = *created.value();
  ASSERT_TRUE(service.InstallSource(kSourceA, kIncarnation, std::nullopt).ok());
  ASSERT_TRUE(service.Publish(acquisition(publication(1, true), 0)).ok());
  clock.advance(9);
  ASSERT_TRUE(service.Publish(acquisition(publication(2, true), clock.now())).ok());
  clock.advance(9);
  EXPECT_EQ(service.ReadSignal(signal_id()).value().availability, IoSignalAvailability::kFresh);
  clock.advance(2);
  EXPECT_EQ(service.ReadSignal(signal_id()).value().availability, IoSignalAvailability::kStale);
}


TEST(IoSignalSubsystemTest, SampleValueSourceOrFreshnessChangeRaisesTheEventSignalAgeAloneDoesNot) {
  // The event stream pushes `state` when a sample's value,
  // source or freshness changes, never for its age alone.
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(catalog(), limits(), clock);
  ASSERT_TRUE(created.ok());
  auto& io = *created.value();
  auto& signal = execution_event_change_signal();
  const auto raised = [&](const auto& step) {
    const auto before = signal.Generation();
    step();
    return signal.Generation() != before;
  };
  EXPECT_TRUE(raised([&] { ASSERT_TRUE(io.InstallSource(kSourceA, kIncarnation, std::nullopt).ok()); }));
  EXPECT_FALSE(io.NextSampleExpiry());  // nothing fresh yet
  EXPECT_TRUE(raised([&] { ASSERT_TRUE(io.Publish(acquisition(publication(1, true), 0)).ok()); }));
  EXPECT_EQ(io.NextSampleExpiry(), std::optional<TimePoint>(11));
  // The same value again, only older or renewed: no change.
  clock.advance(5);
  EXPECT_FALSE(raised([&] { io.RefreshSamples(); }));
  EXPECT_FALSE(raised([&] {
    ASSERT_TRUE(io.Publish(acquisition(publication(2, true), clock.now())).ok());
  }));
  EXPECT_EQ(io.NextSampleExpiry(), std::optional<TimePoint>(16));
  // A new value.
  clock.advance(1);
  EXPECT_TRUE(raised([&] {
    ASSERT_TRUE(io.Publish(acquisition(publication(3, false), clock.now())).ok());
  }));
  // Fresh turning stale with no acquisition: seen at the expiry refresh.
  clock.advance(10);
  EXPECT_FALSE(raised([&] { io.RefreshSamples(); }));
  clock.advance(1);
  EXPECT_TRUE(raised([&] { io.RefreshSamples(); }));
  EXPECT_FALSE(io.NextSampleExpiry());
  EXPECT_FALSE(raised([&] { io.RefreshSamples(); }));
  // A lost source.
  EXPECT_TRUE(raised([&] {
    ASSERT_TRUE(io.Publish(acquisition(publication(4, false), clock.now())).ok());
  }));
  EXPECT_TRUE(raised([&] {
    ASSERT_TRUE(io.MarkSourceUnavailable(kSourceA, kIncarnation, 4,
                                         {"source_disconnected", "provider unavailable", {}})
                    .ok());
  }));
}

IoSignalDescriptorDTO output_descriptor(const std::string& name, bool readback = true,
                                        bool controller_owned = false) {
  return {signal_id(name),
          kSourceB,
          IoSignalValueType::kBoolean,
          IoSignalDirection::kOutput,
          IoSignalUpdateMode::kChangeOnly,
          IoTransitionCapability::kSampledOnly,
          {IoFreshnessMode::kProviderValidity, std::nullopt},
          ResourceId("resource." + name),
          std::nullopt,
          controller_owned,
          readback};
}

IoSourceAcquisitionDTO output_sample(std::uint64_t sequence, bool value,
                                     IoSignalSampleSource source, TimePoint received,
                                     const std::string& name = "valve") {
  return {kSourceB, kIncarnation, sequence, received,
          {{signal_id(name), IoSampleValidity::kValid, value, std::nullopt, source}}};
}

TEST(IoSignalSubsystemTest, ChangedOutsideExistsOnlyForReadBackOutputsNotControllerOwned) {
  FakeClock clock;
  auto created = IoSignalSubsystem::Create(
      catalog({descriptor(), output_descriptor("valve"), output_descriptor("owned", true, true),
               output_descriptor("blind", false)}),
      {3, 8, 8, 8, 512, 1024}, clock);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto& io = *created.value();
  const auto flag = io.ReadSignal(ChangedOutsideSignalId(signal_id("valve")));
  ASSERT_TRUE(flag.ok());
  EXPECT_EQ(flag.value().descriptor, ChangedOutsideDescriptor(output_descriptor("valve")));
  EXPECT_EQ(flag.value().descriptor.direction, IoSignalDirection::kInput);
  EXPECT_EQ(flag.value().descriptor.liveness, std::optional(IoSignalLivenessMode::kInProcess));
  EXPECT_EQ(flag.value().availability, IoSignalAvailability::kFresh);
  ASSERT_TRUE(flag.value().latest_sample);
  EXPECT_EQ(flag.value().latest_sample->publication.value, std::optional(false));
  EXPECT_FALSE(io.ReadSignal(ChangedOutsideSignalId(signal_id("owned"))).ok());
  EXPECT_FALSE(io.ReadSignal(ChangedOutsideSignalId(signal_id("blind"))).ok());
  EXPECT_FALSE(io.ClearOutsideChange(signal_id("owned")).ok());
  EXPECT_FALSE(io.ClearOutsideChange(signal_id("blind")).ok());
  EXPECT_TRUE(io.ClearOutsideChange(signal_id("valve")).ok());
  // The core-held source takes no outside installation or samples.
  const auto core_source = ChangedOutsideSourceId(signal_id("valve"));
  EXPECT_FALSE(io.InstallSource(core_source, kIncarnation, std::nullopt).ok());
  // Read-back and controller ownership are output facts only.
  auto input = catalog();
  input.signals.front().readback = true;
  EXPECT_FALSE(IoSignalSubsystem::Create(std::move(input), limits(), clock).ok());
  // Each core-held signal and source counts against the explicit bounds.
  EXPECT_FALSE(IoSignalSubsystem::Create(catalog({output_descriptor("valve")}),
                                         {1, 4, 8, 8, 512, 1024}, clock).ok());
}

TEST(IoSignalSubsystemTest, OutputWithoutReadbackProjectsNotReadableNotLastWrite) {
  FakeClock clock;
  // Without read-back nothing shows the device's state: the projection is
  // `not_readable` with no value, never the last accepted write.
  auto without = IoSignalSubsystem::Create(
      catalog({output_descriptor("valve", /*readback=*/false)}), limits(), clock);
  ASSERT_TRUE(without.ok()) << without.error().message;
  ASSERT_TRUE(without.value()->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto unreadable = ToSignalSample(without.value()->ReadSignal(signal_id("valve")).value());
  EXPECT_EQ(unreadable.source, IoSignalSampleSource::kNotReadable);
  EXPECT_FALSE(unreadable.value.has_value());
  EXPECT_FALSE(unreadable.fresh);
  // With read-back an output shows the device's sample; before the first one
  // it is pending (device, no value), not not_readable.
  auto with = IoSignalSubsystem::Create(catalog({output_descriptor("valve")}), limits(), clock);
  ASSERT_TRUE(with.ok()) << with.error().message;
  ASSERT_TRUE(with.value()->InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto pending = ToSignalSample(with.value()->ReadSignal(signal_id("valve")).value());
  EXPECT_EQ(pending.source, IoSignalSampleSource::kDevice);
  EXPECT_FALSE(pending.value.has_value());
}

TEST(IoSignalSubsystemTest, ChangedOutsideSetsOnOutsideAndClearsOnWriteClearAndPrepare) {
  FakeClock clock;
  auto created =
      IoSignalSubsystem::Create(catalog({output_descriptor("valve")}), limits(), clock);
  ASSERT_TRUE(created.ok()) << created.error().message;
  auto& io = *created.value();
  ASSERT_TRUE(io.InstallSource(kSourceB, kIncarnation, std::nullopt).ok());
  const auto flag_id = ChangedOutsideSignalId(signal_id("valve"));
  const auto flag = [&] {
    return io.ReadSignal(flag_id).value().latest_sample->publication.value;
  };
  const auto publish = [&](std::uint64_t sequence, bool value, IoSignalSampleSource source) {
    clock.advance(1);
    return io.Publish(output_sample(sequence, value, source, clock.now()));
  };
  ASSERT_TRUE(publish(1U, true, IoSignalSampleSource::kManyforgeWrite).ok());
  EXPECT_EQ(flag(), std::optional(false));
  // A Program level watch reads it like any signal.
  const auto watch = io.RegisterLevelWatch({scope(), flag_id, true});
  ASSERT_TRUE(watch.ok());
  ASSERT_TRUE(publish(2U, false, IoSignalSampleSource::kOutside).ok());
  EXPECT_EQ(flag(), std::optional(true));
  EXPECT_EQ(io.ReadWatch(watch.value()).value().state, IoWatchState::kMatched);
  const auto sample = ToSignalSample(io.ReadSignal(signal_id("valve")).value());
  EXPECT_EQ(sample.source, IoSignalSampleSource::kOutside);
  EXPECT_EQ(sample.value, (std::optional<std::variant<bool, double>>(false)));
  EXPECT_TRUE(sample.fresh);
  // The node `io.clear_outside_change` clears it; the same outside reading
  // repeated is no new change.
  ASSERT_TRUE(io.ClearOutsideChange(signal_id("valve")).ok());
  EXPECT_EQ(flag(), std::optional(false));
  ASSERT_TRUE(publish(3U, false, IoSignalSampleSource::kOutside).ok());
  EXPECT_EQ(flag(), std::optional(false));
  ASSERT_TRUE(publish(4U, true, IoSignalSampleSource::kOutside).ok());
  EXPECT_EQ(flag(), std::optional(true));
  // An accepted ManyForge write clears it.
  ASSERT_TRUE(publish(5U, true, IoSignalSampleSource::kManyforgeWrite).ok());
  EXPECT_EQ(flag(), std::optional(false));
  ASSERT_TRUE(publish(6U, false, IoSignalSampleSource::kOutside).ok());
  EXPECT_EQ(flag(), std::optional(true));
  // The owning component becoming prepared clears it; other components don't.
  ASSERT_TRUE(io.ClearComponentOutsideChanges(ControllableComponentId("component.other")).ok());
  EXPECT_EQ(flag(), std::optional(true));
  ASSERT_TRUE(io.ClearComponentOutsideChanges(ControllableComponentId("component.io")).ok());
  EXPECT_EQ(flag(), std::optional(false));
  // A device reading never sets it; an adapter cannot report `not_readable`,
  // which the core derives from a declared output without read-back.
  ASSERT_TRUE(publish(7U, true, IoSignalSampleSource::kDevice).ok());
  EXPECT_FALSE(publish(8U, true, IoSignalSampleSource::kNotReadable).ok());
  EXPECT_EQ(flag(), std::optional(false));
}

}  // namespace
}  // namespace manyforge::core

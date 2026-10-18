#include <gtest/gtest.h>

#include <manyforge_core/io_signal_ingress.hpp>
#include <manyforge_core/time/fake_clock.hpp>

namespace manyforge::core {
namespace {

const InterruptSourceId kA("source.a"), kB("source.b");
const IoSourceIncarnation kInstalled("source.1");

IoSignalId id(std::string name = "a") {
  return {ControllableComponentId("io"), std::move(name)};
}

IoSignalDescriptorDTO descriptor(std::string name, InterruptSourceId source) {
  return {id(std::move(name)),
          std::move(source),
          IoSignalValueType::kBoolean,
          IoSignalDirection::kInput,
          IoSignalUpdateMode::kSampled,
          IoTransitionCapability::kSampledOnly,
          {IoFreshnessMode::kSampleAge, 10},
          std::nullopt, IoSignalLivenessMode::kPeriodic};
}

std::shared_ptr<IoSignalSubsystem> signals(FakeClock& clock, std::size_t capacity = 4) {
  auto created = IoSignalSubsystem::Create(
      {RequestOwnerEpoch("host"), 1, {descriptor("a", kA), descriptor("b", kB)}},
      {2, 4, capacity, 4, 128, 1024}, clock);
  if (!created.ok()) return {};
  std::shared_ptr<IoSignalSubsystem> service(std::move(created.value()));
  if (!service->InstallSource(kA, kInstalled, std::nullopt).ok() ||
      !service->InstallSource(kB, kInstalled, std::nullopt).ok())
    return {};
  return service;
}

IoSourceAcquisitionDTO frame(std::uint64_t sequence, TimePoint received, bool value = true,
                             InterruptSourceId source = kA, std::string name = "a") {
  return {std::move(source),
          kInstalled,
          sequence,
          received,
          {{id(std::move(name)), IoSampleValidity::kValid, value, std::nullopt}}};
}

IoLevelWatchRequestDTO watch() {
  return {{RequestOwnerEpoch("runner"),
           ProgramRunId("run"),
           {ProgramNodeInstanceId("wait"), "attempt"}},
          id(),
          true};
}

IoSourceCursorDTO origin() {
  return {RequestOwnerEpoch("host"), 1, kA, 0};
}

TEST(IoSignalIngressTest, MockIngressFeedsWatchOnlyWhenCallerDrainsAndPreservesReceiptAge) {
  FakeClock clock;
  auto service = signals(clock);
  ASSERT_TRUE(service);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  const auto registration = service->RegisterLevelWatch(watch());
  ASSERT_TRUE(registration.ok());
  IIoSignalIngress& mock_provider_sink = *ingress;
  EXPECT_TRUE(mock_provider_sink.TryEnqueue(frame(1, clock.now())));
  EXPECT_EQ(service->ReadWatch(registration.value()).value().state, IoWatchState::kPending);
  clock.advance(11);
  const auto drained = ingress->Drain(1);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 1U);
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kStale);
  EXPECT_EQ(service->ReadSignal(id()).value().latest_sample->received_at_ns, 0);
  EXPECT_EQ(service->ReadWatch(registration.value()).value().state, IoWatchState::kPending);
  EXPECT_TRUE(mock_provider_sink.TryEnqueue(frame(2, clock.now())));
  ASSERT_TRUE(ingress->Drain(1).ok());
  EXPECT_EQ(service->ReadWatch(registration.value()).value().state, IoWatchState::kMatched);
}

TEST(IoSignalIngressTest, MultiSignalEnvelopeUsesOneQueueSlotAndOneSourceRecord) {
  FakeClock clock;
  auto created_service = IoSignalSubsystem::Create(
      {RequestOwnerEpoch("host"), 1, {descriptor("a", kA), descriptor("other", kA)}},
      {1, 2, 2, 4, 128, 1024}, clock);
  ASSERT_TRUE(created_service.ok());
  std::shared_ptr<IoSignalSubsystem> service(std::move(created_service.value()));
  ASSERT_TRUE(service->InstallSource(kA, kInstalled, std::nullopt).ok());
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  auto acquisition = frame(1, 0);
  acquisition.updates.push_back({id("other"), IoSampleValidity::kValid, false, std::nullopt});
  ASSERT_TRUE(ingress->TryEnqueue(acquisition));
  const auto drained = ingress->Drain(1);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 1U);
  const std::vector<IoSignalId> ids{id(), id("other")};
  const auto snapshot = service->ReadSignals(ids);
  ASSERT_TRUE(snapshot.ok());
  EXPECT_EQ(snapshot.value()[0].cursor, snapshot.value()[1].cursor);
  EXPECT_EQ(snapshot.value()[0].cursor.record_sequence, 2U);
  EXPECT_EQ(snapshot.value()[0].latest_sample->publication.value, true);
  EXPECT_EQ(snapshot.value()[1].latest_sample->publication.value, false);
  EXPECT_EQ(snapshot.value()[0].evaluated_at_ns, snapshot.value()[1].evaluated_at_ns);
  EXPECT_EQ(ingress->Drain(1).value().attempted, 0U);
}

TEST(IoSignalIngressTest, CapacityLossSurvivesFullQueueAndBlocksRecoveryUntilDelivered) {
  FakeClock clock;
  auto service = signals(clock, 2);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, false)));
  ASSERT_TRUE(ingress->TryEnqueue(frame(2, 0, false)));
  EXPECT_FALSE(ingress->TryEnqueue(frame(3, 0)));
  const auto first = ingress->Drain(1);
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(first.value().applied, 1U);
  EXPECT_FALSE(ingress->TryEnqueue(frame(4, 0)));  // space exists, retained loss still owns barrier
  ASSERT_TRUE(ingress->Drain(1).ok());
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kFresh);
  const auto loss = ingress->Drain(1);
  ASSERT_TRUE(loss.ok());
  EXPECT_EQ(loss.value().applied, 1U);
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kUnavailable);
  const auto page = service->ReadRecords(origin(), 2);
  ASSERT_TRUE(page.ok());
  ASSERT_TRUE(page.value().records.back().loss->ingress_barrier);
  const auto& evidence = *page.value().records.back().loss->ingress_barrier;
  EXPECT_TRUE(evidence.host_ingress_rejected);
  EXPECT_TRUE(evidence.coalesced);
  EXPECT_FALSE(evidence.middleware_reported_lost_messages);
  ASSERT_TRUE(ingress->TryEnqueue(frame(5, 0)));
  ASSERT_TRUE(ingress->Drain(1).ok());
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kFresh);
}

TEST(IoSignalIngressTest, CoalescedBarrierPreservesDisconnectAndActualMiddlewareEvidence) {
  FakeClock clock;
  auto service = signals(clock);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, false)));
  ingress->NotifyLoss({kA, kInstalled, 0, 1, IoIngressLossReason::kMiddlewareMessageLost, 7});
  ingress->NotifyLoss({kA, kInstalled, 0, 1, IoIngressLossReason::kDisconnected, std::nullopt});
  ingress->NotifyLoss(
      {kA, kInstalled, 0, 1, IoIngressLossReason::kSequenceExhausted, std::nullopt});
  EXPECT_FALSE(ingress->TryEnqueue(frame(2, 0)));
  const auto drained = ingress->Drain(2);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 2U);
  const auto page = service->ReadRecords(origin(), 4);
  ASSERT_TRUE(page.ok());
  ASSERT_TRUE(page.value().records.back().loss->ingress_barrier);
  const auto& loss = *page.value().records.back().loss->ingress_barrier;
  EXPECT_TRUE(loss.middleware_message_lost);
  EXPECT_TRUE(loss.disconnected);
  EXPECT_TRUE(loss.sequence_exhausted);
  EXPECT_TRUE(loss.host_ingress_rejected);
  EXPECT_TRUE(loss.coalesced);
  EXPECT_FALSE(loss.middleware_reported_lost_messages);  // not an invented total
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kUnavailable);
}

TEST(IoSignalIngressTest, SingleMiddlewareNotificationRetainsActualCountWithoutInferringRange) {
  FakeClock clock;
  auto service = signals(clock);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ingress->NotifyLoss({kA, kInstalled, 0, 0, IoIngressLossReason::kMiddlewareMessageLost, 3});
  ASSERT_TRUE(ingress->Drain(1).ok());
  const auto records = service->ReadRecords(origin(), 4);
  ASSERT_TRUE(records.ok());
  const auto& loss = *records.value().records.back().loss;
  EXPECT_EQ(loss.ingress_barrier->middleware_reported_lost_messages, 3U);
  EXPECT_FALSE(loss.ingress_barrier->coalesced);
  EXPECT_FALSE(loss.first_lost_source_sequence);
  EXPECT_FALSE(loss.last_lost_source_sequence);
}

TEST(IoSignalIngressTest, BoundedDrainRotatesSourcesRatherThanDrainingOneBacklog) {
  FakeClock clock;
  auto service = signals(clock);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  EXPECT_FALSE(ingress->Drain(0).ok());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, false)));
  ASSERT_TRUE(ingress->TryEnqueue(frame(2, 0, true)));
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, true, kB, "b")));
  ASSERT_EQ(ingress->Drain(1).value().attempted, 1U);
  EXPECT_FALSE(service->ReadSignal(id("b")).value().latest_sample);
  ASSERT_EQ(ingress->Drain(1).value().attempted, 1U);
  EXPECT_EQ(service->ReadSignal(id("b")).value().latest_sample->publication.value, true);
  EXPECT_EQ(service->ReadSignal(id()).value().latest_sample->publication.source_sequence, 1U);
  ASSERT_EQ(ingress->Drain(1).value().attempted, 1U);
  EXPECT_EQ(service->ReadSignal(id()).value().latest_sample->publication.source_sequence, 2U);
}

TEST(IoSignalIngressTest, InvalidOrStaleRefusalDoesNotCreateCurrentSourceLoss) {
  FakeClock clock;
  auto service = signals(clock, 1);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, false)));
  auto stale = frame(2, 0);
  stale.incarnation = IoSourceIncarnation("old");
  EXPECT_FALSE(ingress->TryEnqueue(stale));
  auto malformed = frame(2, 0);
  malformed.updates.front().value.reset();
  EXPECT_FALSE(ingress->TryEnqueue(malformed));
  auto foreign = frame(2, 0);
  foreign.updates.front().signal_id = id("b");
  EXPECT_FALSE(ingress->TryEnqueue(foreign));
  ASSERT_TRUE(ingress->Drain(1).ok());
  EXPECT_EQ(ingress->Drain(1).value().attempted, 0U);
  EXPECT_EQ(service->ReadSignal(id()).value().continuity_loss_generation, 0U);
}

TEST(IoSignalIngressTest, SourceReplacementBeforeDrainRetiresOldDataAndLossHarmlessly) {
  FakeClock clock;
  auto service = signals(clock, 1);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0)));
  EXPECT_FALSE(ingress->TryEnqueue(frame(2, 0)));
  const IoSourceIncarnation replacement("source.2");
  ASSERT_TRUE(service->InstallSource(kA, replacement, kInstalled).ok());
  auto current = frame(1, 0, false);
  current.incarnation = replacement;
  ASSERT_TRUE(service->Publish(current).ok());
  const auto before = service->ReadSignal(id()).value();
  const auto drained = ingress->Drain(2);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().obsolete, 2U);
  EXPECT_EQ(drained.value().applied, 0U);
  EXPECT_FALSE(drained.value().blocked_error);
  EXPECT_EQ(service->ReadSignal(id()).value().cursor, before.cursor);
  EXPECT_EQ(service->ReadSignal(id()).value().continuity_loss_generation,
            before.continuity_loss_generation);
  EXPECT_EQ(service->ReadSignal(id()).value().latest_sample, before.latest_sample);
  EXPECT_FALSE(ingress->TryEnqueue(frame(3, 0)));
  ingress->NotifyLoss({kA, kInstalled, 0, 3, IoIngressLossReason::kDisconnected, std::nullopt});
  ASSERT_TRUE(ingress->Drain(1).ok());
  EXPECT_EQ(service->ReadSignal(id()).value().cursor, before.cursor);
}

TEST(IoSignalIngressTest, TypedLossNeedsNoErrorTextBudgetAndObsoleteLossRetiresBeforeShapeCheck) {
  FakeClock clock;
  auto created_service = IoSignalSubsystem::Create(
      {RequestOwnerEpoch("host"), 1, {descriptor("a", kA)}}, {1, 1, 4, 1, 128, 1},
      clock);  // a valid one-byte adapter-error budget
  ASSERT_TRUE(created_service.ok());
  std::shared_ptr<IoSignalSubsystem> service(std::move(created_service.value()));
  ASSERT_TRUE(service->InstallSource(kA, kInstalled, std::nullopt).ok());
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ingress->NotifyLoss({kA, kInstalled, 0, 0, IoIngressLossReason::kDisconnected, std::nullopt});
  const auto drained = ingress->Drain(1);
  ASSERT_TRUE(drained.ok());
  EXPECT_EQ(drained.value().applied, 1U);
  EXPECT_FALSE(drained.value().blocked_error);
  EXPECT_EQ(service->ReadSignal(id()).value().availability, IoSignalAvailability::kUnavailable);
  const auto records = service->ReadRecords(origin(), 4);
  ASSERT_TRUE(records.ok());
  ASSERT_TRUE(records.value().records.back().loss);
  EXPECT_FALSE(records.value().records.back().loss->error);
  EXPECT_TRUE(records.value().records.back().loss->ingress_barrier->disconnected);
  // An old trusted-host callback may carry a bad timestamp. Once obsolete, it
  // cannot keep a replacement source's discarded ingress lifetime blocked.
  ingress->NotifyLoss({kA, kInstalled, 100, 0, IoIngressLossReason::kDisconnected, std::nullopt});
  ASSERT_TRUE(service->InstallSource(kA, IoSourceIncarnation("source.2"), kInstalled).ok());
  const auto before = service->ReadSignal(id()).value().cursor;
  const auto obsolete = ingress->Drain(1);
  ASSERT_TRUE(obsolete.ok());
  EXPECT_EQ(obsolete.value().obsolete, 1U);
  EXPECT_FALSE(obsolete.value().blocked_error);
  EXPECT_EQ(service->ReadSignal(id()).value().cursor, before);
}

TEST(IoSignalIngressTest, FailedApplicableLossAdmissionRemainsRetainedAndBlocksRecovery) {
  FakeClock clock;
  auto service = signals(clock);
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  // A malformed trusted host event is observable as a drain error; popping it
  // does not authorize recovery. Time advance makes this exact event applicable.
  ingress->NotifyLoss({kA, kInstalled, 10, 0, IoIngressLossReason::kDisconnected, std::nullopt});
  const auto blocked = ingress->Drain(1);
  ASSERT_TRUE(blocked.ok());
  ASSERT_TRUE(blocked.value().blocked_error);
  EXPECT_EQ(blocked.value().applied, 0U);
  EXPECT_FALSE(ingress->TryEnqueue(frame(1, 0)));
  clock.advance(10);
  ASSERT_EQ(ingress->Drain(1).value().applied, 1U);  // original in-flight loss
  EXPECT_FALSE(ingress->TryEnqueue(frame(2, 10)));   // refusal is separate retained loss
  ASSERT_EQ(ingress->Drain(1).value().applied, 1U);
  EXPECT_TRUE(ingress->TryEnqueue(frame(3, 10)));
}

TEST(IoSignalIngressTest, SourceDepthsRemainIndependentForIngressAndRecordRetention) {
  FakeClock clock;
  IoSignalLimitsDTO bounds{2, 2, 8, 2, 128, 1024, {{kA, 1}, {kB, 3}}};
  const IoSignalCatalogDTO catalog{
      RequestOwnerEpoch("host"), 1, {descriptor("a", kA), descriptor("b", kB)}};
  auto invalid_bounds = bounds;
  invalid_bounds.records_per_source_overrides[kA] = 0;
  EXPECT_FALSE(IoSignalSubsystem::Create(catalog, invalid_bounds, clock).ok());
  invalid_bounds = bounds;
  invalid_bounds.records_per_source_overrides[InterruptSourceId("unknown")] = 1;
  EXPECT_FALSE(IoSignalSubsystem::Create(catalog, invalid_bounds, clock).ok());
  auto service_result = IoSignalSubsystem::Create(catalog, bounds, clock);
  ASSERT_TRUE(service_result.ok());
  std::shared_ptr<IoSignalSubsystem> service(std::move(service_result.value()));
  ASSERT_TRUE(service->InstallSource(kA, kInstalled, std::nullopt).ok());
  ASSERT_TRUE(service->InstallSource(kB, kInstalled, std::nullopt).ok());
  auto created = IoSignalIngress::Create(service);
  ASSERT_TRUE(created.ok());
  auto ingress = std::move(created.value());
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0)));
  EXPECT_FALSE(ingress->TryEnqueue(frame(2, 0)));
  ASSERT_TRUE(ingress->TryEnqueue(frame(1, 0, false, kB, "b")));
  ASSERT_TRUE(ingress->TryEnqueue(frame(2, 0, true, kB, "b")));
  ASSERT_TRUE(ingress->TryEnqueue(frame(3, 0, false, kB, "b")));
  EXPECT_EQ(ingress->Drain(5).value().applied, 5U);      // A sample+loss; all three B samples
  EXPECT_FALSE(service->ReadRecords(origin(), 2).ok());  // A owns depth one
  auto b_origin = origin();
  b_origin.source_id = kB;
  const auto page = service->ReadRecords(b_origin, 3);
  ASSERT_TRUE(page.ok());
  EXPECT_EQ(page.value().records.size(), 3U);
  EXPECT_TRUE(page.value().retention_gap);
  EXPECT_EQ(page.value().records.front().acquisition->source_sequence, 1U);
  EXPECT_EQ(service->ReadSignal(id("b")).value().continuity_loss_generation, 0U);
}

}  // namespace
}  // namespace manyforge::core

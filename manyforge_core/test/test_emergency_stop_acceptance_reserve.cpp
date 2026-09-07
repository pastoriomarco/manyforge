#include <gtest/gtest.h>

#include <future>
#include <manyforge_core/emergency_stop_acceptance_reserve.hpp>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace manyforge::core {
namespace {

constexpr std::string_view kRequestDigest =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDifferentRequestDigest =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kScopeDigest =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kOtherScopeDigest =
    "sha256:4444444444444444444444444444444444444444444444444444444444444444";
constexpr std::string_view kFullScopeDigest =
    "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff";

[[nodiscard]] EmergencyStopAcceptanceCandidate candidate(std::string_view request_id,
                                                         std::string_view stop_id = "stop.1",
                                                         std::uint64_t generation = 1U) {
  return EmergencyStopAcceptanceCandidate{
      .request_id = request_id,
      .normalized_request_digest = kRequestDigest,
      .transport = RequestTransportDTO::kInternal,
      .assurance = RequestAssuranceDTO::kUnverified,
      .principal_id = std::nullopt,
      .malformed_reason = std::nullopt,
      .proposed_stop_id = stop_id,
      .proposed_stop_generation = StopGeneration(generation),
      .accepted_at_ns = 100,
      .outer_deadline_ns = 1'100,
      .full_stop_outer_deadline_ns = 1'100,
      .disposition = StopResolutionDispositionDTO::kExact,
      .captured_scope_digest = kScopeDigest,
      .full_stop_scope_digest = kFullScopeDigest,
      .overlapping_stop_id = std::nullopt,
      .full_stop = false,
  };
}

TEST(EmergencyStopAcceptanceReserveTest, RejectsInvalidConstructionBounds) {
  EXPECT_THROW(EmergencyStopAcceptanceReserve(0U), std::invalid_argument);
  EXPECT_THROW(EmergencyStopAcceptanceReserve(kMaximumEmergencyStopCorrelationEntries + 1U),
               std::invalid_argument);
}

TEST(EmergencyStopAcceptanceReserveTest,
     FreshAcceptanceAndExactDuplicateRetainIdentityAndDeadline) {
  EmergencyStopAcceptanceReserve reserve(2U);
  const auto request = candidate("request.stop.1");

  const auto fresh = reserve.reserve_or_join(request);
  ASSERT_TRUE(fresh.accepted());
  EXPECT_EQ(fresh.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(fresh.acceptance_path, EmergencyStopAcceptancePathDTO::kFreshReserved);
  EXPECT_EQ(fresh.stop_id(), "stop.1");
  EXPECT_EQ(fresh.stop_generation, StopGeneration(1U));
  EXPECT_EQ(fresh.accepted_at_ns, 100);
  EXPECT_EQ(fresh.outer_deadline_ns, 1'100);
  EXPECT_EQ(fresh.captured_scope_digest(), kScopeDigest);
  EXPECT_EQ(fresh.work_token, (StopControlWorkToken{0U, 1U}));
  EXPECT_EQ(fresh.correlation_slot, 0U);
  EXPECT_TRUE(fresh.correlation_retained);
  EXPECT_FALSE(fresh.full_stop);

  auto later_proposal = request;
  later_proposal.proposed_stop_id = "stop.must.not.replace";
  later_proposal.proposed_stop_generation = StopGeneration(99U);
  later_proposal.accepted_at_ns = 500;
  later_proposal.outer_deadline_ns = 9'999;
  later_proposal.full_stop_outer_deadline_ns = 9'999;
  const auto replay = reserve.reserve_or_join(later_proposal);
  ASSERT_TRUE(replay.accepted());
  EXPECT_EQ(replay.replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(replay.acceptance_path, EmergencyStopAcceptancePathDTO::kFreshReserved);
  EXPECT_EQ(replay.stop_id(), fresh.stop_id());
  EXPECT_EQ(replay.stop_generation, fresh.stop_generation);
  EXPECT_EQ(replay.accepted_at_ns, fresh.accepted_at_ns);
  EXPECT_EQ(replay.outer_deadline_ns, fresh.outer_deadline_ns);
  EXPECT_EQ(replay.work_token, fresh.work_token);
  EXPECT_EQ(replay.correlation_slot, fresh.correlation_slot);
  EXPECT_EQ(reserve.active_correlation_count(), 1U);
}

TEST(EmergencyStopAcceptanceReserveTest, StaleExplicitFullStopRetainsItsResolvedDisposition) {
  EmergencyStopAcceptanceReserve reserve(2U);
  auto request = candidate("request.stale.full");
  request.full_stop = true;
  request.disposition = StopResolutionDispositionDTO::kStaleButUnambiguouslyResolved;
  const auto first = reserve.reserve_or_join(request);
  ASSERT_TRUE(first.accepted());
  EXPECT_TRUE(first.full_stop);
  EXPECT_EQ(first.disposition, request.disposition);
  EXPECT_EQ(first.acceptance_path, EmergencyStopAcceptancePathDTO::kFreshReserved);
  request.request_id = "request.stale.full.join";
  request.proposed_stop_id = "stop.unused";
  request.proposed_stop_generation = StopGeneration(2U);
  request.accepted_at_ns = 200;
  request.outer_deadline_ns = 2'000;
  request.full_stop_outer_deadline_ns = 2'000;
  const auto joined = reserve.reserve_or_join(request);
  ASSERT_TRUE(joined.accepted());
  EXPECT_EQ(joined.stop_id(), first.stop_id());
  EXPECT_EQ(joined.work_token, first.work_token);
  EXPECT_EQ(joined.outer_deadline_ns, first.outer_deadline_ns);
  EXPECT_EQ(joined.disposition, request.disposition);
}

TEST(EmergencyStopAcceptanceReserveTest, ConflictingResidentIdentityPromotesAndLatchesFullStop) {
  EmergencyStopAcceptanceReserve reserve(2U);
  ASSERT_TRUE(reserve.reserve_or_join(candidate("request.conflict")).accepted());

  auto conflicting = candidate("request.conflict", "stop.full.2", 2U);
  conflicting.normalized_request_digest = kDifferentRequestDigest;
  conflicting.captured_scope_digest = kOtherScopeDigest;
  conflicting.full_stop_outer_deadline_ns = 900;
  const auto promoted = reserve.reserve_or_join(conflicting);
  ASSERT_TRUE(promoted.accepted());
  EXPECT_EQ(promoted.replay_status, RequestReplayStatusDTO::kAcceptedNew);
  EXPECT_EQ(promoted.acceptance_path, EmergencyStopAcceptancePathDTO::kCoalescedFull);
  EXPECT_EQ(promoted.disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_TRUE(promoted.full_stop);
  EXPECT_EQ(promoted.outer_deadline_ns, 900);
  EXPECT_EQ(promoted.captured_scope_digest(), kFullScopeDigest);
  EXPECT_TRUE(reserve.full_stop_active());

  const auto exact_conflict_replay = reserve.reserve_or_join(conflicting);
  EXPECT_EQ(exact_conflict_replay.replay_status, RequestReplayStatusDTO::kReplayedExact);
  EXPECT_EQ(exact_conflict_replay.stop_id(), promoted.stop_id());
  EXPECT_EQ(exact_conflict_replay.work_token, promoted.work_token);
  EXPECT_EQ(exact_conflict_replay.correlation_slot, promoted.correlation_slot);
  const auto found = reserve.find("request.conflict");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->stop_id(), promoted.stop_id());
  EXPECT_EQ(found->work_token, promoted.work_token);
}

TEST(EmergencyStopAcceptanceReserveTest, OverlapJoinsResidentWithoutExtendingDeadline) {
  EmergencyStopAcceptanceReserve reserve(3U);
  const auto first = reserve.reserve_or_join(candidate("request.first"));
  ASSERT_TRUE(first.accepted());

  auto overlapping = candidate("request.overlap", "stop.unused", 2U);
  overlapping.accepted_at_ns = 900;
  overlapping.outer_deadline_ns = 20'000;
  overlapping.full_stop_outer_deadline_ns = 20'000;
  overlapping.captured_scope_digest = kOtherScopeDigest;
  overlapping.overlapping_stop_id = first.stop_id();
  const auto joined = reserve.reserve_or_join(overlapping);
  ASSERT_TRUE(joined.accepted());
  EXPECT_EQ(joined.acceptance_path, EmergencyStopAcceptancePathDTO::kJoinedResident);
  EXPECT_EQ(joined.stop_id(), first.stop_id());
  EXPECT_EQ(joined.stop_generation, first.stop_generation);
  EXPECT_EQ(joined.accepted_at_ns, first.accepted_at_ns);
  EXPECT_EQ(joined.outer_deadline_ns, first.outer_deadline_ns);
  EXPECT_EQ(joined.captured_scope_digest(), first.captured_scope_digest());
  EXPECT_EQ(joined.work_token, first.work_token);
  EXPECT_NE(joined.correlation_slot, first.correlation_slot);
  EXPECT_EQ(joined.correlation_slot, 1U);
  EXPECT_EQ(reserve.active_correlation_count(), 2U);

  const auto replayed_join = reserve.find("request.overlap");
  ASSERT_TRUE(replayed_join.has_value());
  EXPECT_EQ(replayed_join->work_token, first.work_token);
  EXPECT_EQ(replayed_join->correlation_slot, joined.correlation_slot);
}

TEST(EmergencyStopAcceptanceReserveTest, MissingOverlapAssertionFailsClosedThroughFullStop) {
  EmergencyStopAcceptanceReserve reserve(2U);
  auto request = candidate("request.missing.overlap");
  request.overlapping_stop_id = "stop.no.longer.resident";
  const auto result = reserve.reserve_or_join(request);
  ASSERT_TRUE(result.accepted());
  EXPECT_EQ(result.acceptance_path, EmergencyStopAcceptancePathDTO::kCoalescedFull);
  EXPECT_EQ(result.disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_TRUE(result.full_stop);
}

TEST(EmergencyStopAcceptanceReserveTest, CorrelationExhaustionCoalescesWithoutDroppingOrWaiting) {
  EmergencyStopAcceptanceReserve reserve(1U);
  ASSERT_TRUE(reserve.reserve_or_join(candidate("request.slot.owner")).accepted());

  const auto overflow = reserve.reserve_or_join(candidate("request.overflow", "stop.full", 2U));
  ASSERT_TRUE(overflow.accepted());
  EXPECT_EQ(overflow.acceptance_path, EmergencyStopAcceptancePathDTO::kCoalescedFull);
  EXPECT_TRUE(overflow.correlation_retained);
  EXPECT_TRUE(overflow.full_stop);
  EXPECT_EQ(overflow.work_token.slot, 1U);
  EXPECT_EQ(overflow.correlation_slot, 1U);
  EXPECT_EQ(reserve.active_correlation_count(), 1U);

  const auto additional =
      reserve.reserve_or_join(candidate("request.additional", "stop.must.not.replace", 3U));
  ASSERT_TRUE(additional.accepted());
  EXPECT_EQ(additional.acceptance_path, EmergencyStopAcceptancePathDTO::kCoalescedFull);
  EXPECT_FALSE(additional.correlation_retained);
  EXPECT_EQ(additional.work_token, overflow.work_token);
  EXPECT_EQ(additional.correlation_slot, 1U);
  EXPECT_EQ(additional.stop_id(), overflow.stop_id());
  EXPECT_EQ(additional.outer_deadline_ns, overflow.outer_deadline_ns);
}

TEST(EmergencyStopAcceptanceReserveTest, ExplicitFullStopUsesDedicatedNonEvictableSlot) {
  EmergencyStopAcceptanceReserve reserve(1U);
  auto full = candidate("request.full", "stop.full", 7U);
  full.full_stop = true;
  full.captured_scope_digest = kFullScopeDigest;

  const auto accepted = reserve.reserve_or_join(full);
  ASSERT_TRUE(accepted.accepted());
  EXPECT_EQ(accepted.acceptance_path, EmergencyStopAcceptancePathDTO::kFreshReserved);
  EXPECT_EQ(accepted.disposition, StopResolutionDispositionDTO::kExact);
  EXPECT_TRUE(accepted.full_stop);
  EXPECT_EQ(accepted.work_token.slot, 1U);
  EXPECT_EQ(accepted.correlation_slot, 1U);
  EXPECT_EQ(reserve.active_correlation_count(), 0U);
  EXPECT_TRUE(reserve.full_stop_active());
}

TEST(EmergencyStopAcceptanceReserveTest, MalformedRecognizedIngressHasNoCallerScopeAndPromotes) {
  EmergencyStopAcceptanceReserve reserve(1U);
  auto malformed = candidate("adapter.request.1", "stop.full", 1U);
  malformed.normalized_request_digest.reset();
  malformed.malformed_reason = "selector_limit_exceeded";
  malformed.disposition = StopResolutionDispositionDTO::kPromotedToFull;
  malformed.full_stop = true;
  malformed.captured_scope_digest = kFullScopeDigest;

  const auto accepted = reserve.reserve_or_join(malformed);
  ASSERT_TRUE(accepted.accepted());
  EXPECT_TRUE(accepted.full_stop);
  EXPECT_EQ(accepted.disposition, StopResolutionDispositionDTO::kPromotedToFull);
  EXPECT_EQ(accepted.captured_scope_digest(), kFullScopeDigest);
}

TEST(EmergencyStopAcceptanceReserveTest, ProjectionFailureDoesNotPinSlotAfterTerminalHandoff) {
  EmergencyStopAcceptanceReserve reserve(1U);
  const auto first = reserve.reserve_or_join(candidate("request.first"));
  ASSERT_TRUE(first.accepted());
  EXPECT_TRUE(reserve.mark_projection(first.stop_id(), EmergencyStopProjectionStateDTO::kFailed));
  EXPECT_EQ(reserve.release_after_terminal_handoff(first.stop_id()), 1U);
  EXPECT_EQ(reserve.active_correlation_count(), 0U);

  const auto replacement =
      reserve.reserve_or_join(candidate("request.replacement", "stop.replacement", 2U));
  ASSERT_TRUE(replacement.accepted());
  EXPECT_EQ(replacement.stop_id(), "stop.replacement");
  EXPECT_EQ(replacement.acceptance_path, EmergencyStopAcceptancePathDTO::kFreshReserved);
  EXPECT_EQ(replacement.work_token.slot, first.work_token.slot);
  EXPECT_GT(replacement.work_token.generation, first.work_token.generation);
}

TEST(EmergencyStopAcceptanceReserveTest,
     PhysicalWorkGenerationSurvivesSlotZeroingAndStaleProposals) {
  EmergencyStopAcceptanceReserve reserve(1U);
  const auto first = reserve.reserve_or_join(candidate("request.first", "stop.first", 10U));
  ASSERT_TRUE(first.accepted());
  EXPECT_EQ(first.work_token, (StopControlWorkToken{0U, 10U}));
  EXPECT_EQ(reserve.release_after_terminal_handoff(first.stop_id()), 1U);

  const auto successor =
      reserve.reserve_or_join(candidate("request.successor", "stop.successor", 2U));
  ASSERT_TRUE(successor.accepted());
  EXPECT_EQ(successor.work_token, (StopControlWorkToken{0U, 11U}));
  EXPECT_EQ(successor.correlation_slot, 0U);

  auto full = candidate("request.full", "stop.full", 3U);
  full.full_stop = true;
  full.captured_scope_digest = kFullScopeDigest;
  const auto first_full = reserve.reserve_or_join(full);
  ASSERT_TRUE(first_full.accepted());
  EXPECT_EQ(first_full.work_token, (StopControlWorkToken{1U, 3U}));
  EXPECT_EQ(reserve.release_after_terminal_handoff(first_full.stop_id()), 1U);
  full.request_id = "request.full.successor";
  full.proposed_stop_id = "stop.full.successor";
  full.proposed_stop_generation = StopGeneration(1U);
  const auto next_full = reserve.reserve_or_join(full);
  ASSERT_TRUE(next_full.accepted());
  EXPECT_EQ(next_full.work_token, (StopControlWorkToken{1U, 4U}));
}

TEST(EmergencyStopAcceptanceReserveTest, ConcurrentFreshRequestsRemainBoundedAndAllAccepted) {
  EmergencyStopAcceptanceReserve reserve(4U);
  std::vector<std::string> request_ids;
  std::vector<std::string> stop_ids;
  request_ids.reserve(16U);
  stop_ids.reserve(16U);
  for (std::size_t index = 0U; index < 16U; ++index) {
    request_ids.push_back("request.concurrent." + std::to_string(index));
    stop_ids.push_back("stop.concurrent." + std::to_string(index));
  }

  std::vector<std::future<EmergencyStopAcceptanceOutcome>> calls;
  calls.reserve(request_ids.size());
  for (std::size_t index = 0U; index < request_ids.size(); ++index) {
    calls.push_back(std::async(std::launch::async, [&, index] {
      return reserve.reserve_or_join(candidate(request_ids[index], stop_ids[index], index + 1U));
    }));
  }
  for (auto& call : calls) {
    EXPECT_TRUE(call.get().accepted());
  }
  EXPECT_EQ(reserve.active_correlation_count(), 4U);
  EXPECT_TRUE(reserve.full_stop_active());
  std::size_t retained = 0U;
  std::size_t retained_full = 0U;
  for (const auto& request_id : request_ids) {
    const auto found = reserve.find(request_id);
    if (!found.has_value()) {
      continue;
    }
    ++retained;
    retained_full += found->full_stop ? 1U : 0U;
  }
  EXPECT_EQ(retained, 5U);
  EXPECT_EQ(retained_full, 1U);
}

TEST(EmergencyStopAcceptanceReserveTest,
     RejectsUnboundedOrInternallyInconsistentCandidateWithoutMutation) {
  EmergencyStopAcceptanceReserve reserve(1U);
  auto invalid = candidate("request.invalid");
  invalid.proposed_stop_generation = StopGeneration(0U);
  const auto result = reserve.reserve_or_join(invalid);
  EXPECT_FALSE(result.accepted());
  EXPECT_EQ(result.error, EmergencyStopReserveError::kInvalidCandidate);
  EXPECT_EQ(reserve.active_correlation_count(), 0U);
  EXPECT_FALSE(reserve.full_stop_active());
  EXPECT_FALSE(
      reserve.mark_projection("stop.unknown", static_cast<EmergencyStopProjectionStateDTO>(99)));
  EXPECT_EQ(reserve.release_after_terminal_handoff("invalid stop id"), 0U);
}

}  // namespace
}  // namespace manyforge::core

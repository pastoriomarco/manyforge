#include <gtest/gtest.h>

#include <limits>

#include "io_blackboard_snapshot.hpp"

namespace manyforge::core::detail {
namespace {
using Kind = IoConditionValueKind;
using PublicationKind = IoBlackboardPublicationKind;
using Snapshot = IoProgramBlackboardSnapshotDTO;

IoProgramBlackboardSourceBindingDTO binding() {
  return {{RequestOwnerEpoch("host.1"), RequestOwnerEpoch("runner.1"), 7U,
           CellAdmissionClaimGeneration(8U), ProgramRunId("run.1"), ProgramBindingId("binding.1"),
           ActiveBundlePublicationGeneration(9U)},
          InterruptSourceId("program.bb"),
          IoSourceIncarnation("source.1"),
          {{"text", Kind::kString},
           {"flag", Kind::kBoolean},
           {"count", Kind::kInt64},
           {"ratio", Kind::kFloat64}},
          8192U};
}

IoProgramBlackboardPublicationDTO baseline(const IoProgramBlackboardSourceBindingDTO& source) {
  return {source.scope,
          PublicationKind::kBaseline,
          1U,
          0U,
          {{"text", Kind::kString, IoConditionValueDTO{std::string("hello")}},
           {"flag", Kind::kBoolean, IoConditionValueDTO{false}},
           {"count", Kind::kInt64, IoConditionValueDTO{std::int64_t(-9)}},
           {"ratio", Kind::kFloat64, std::nullopt}},
          10};
}

std::shared_ptr<const Snapshot> start(const IoProgramBlackboardSourceBindingDTO& source) {
  const auto result = ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source));
  if (!result.ok()) {
    ADD_FAILURE() << result.error().details.at("reason");
    return {};
  }
  return result.value();
}

IoProgramBlackboardPublicationDTO update(const IoProgramBlackboardSourceBindingDTO& source,
                                         std::vector<IoBlackboardValueDTO> values,
                                         std::uint64_t sequence = 2U) {
  return {source.scope, PublicationKind::kUpdate, sequence, sequence - 1U, std::move(values), 20};
}

const IoBlackboardValueDTO& key(const Snapshot& snapshot, std::string name) {
  for (const auto& row : snapshot.values)
    if (row.name == name) return row;
  throw std::logic_error("missing fixture key");
}

void expect_reason(const Result<std::shared_ptr<const Snapshot>>& result, const char* reason) {
  ASSERT_FALSE(result.ok());
  EXPECT_EQ(result.error().details.at("reason"), reason);
}

TEST(ProgramBlackboardSnapshot, BaselineBuildsCompleteCanonicalTypedSnapshot) {
  const auto source = binding();
  const auto snapshot = start(source);
  ASSERT_NE(snapshot, nullptr);
  EXPECT_EQ(snapshot->scope, source.scope);
  EXPECT_EQ(snapshot->source_id, source.source_id);
  EXPECT_EQ(snapshot->incarnation, source.incarnation);
  EXPECT_EQ(snapshot->message_sequence, 1U);
  EXPECT_EQ(snapshot->trusted_received_at_ns, 10);
  ASSERT_EQ(snapshot->values.size(), 4U);
  EXPECT_EQ(snapshot->values[0].name, "count");
  EXPECT_EQ(snapshot->values[1].name, "flag");
  EXPECT_EQ(snapshot->values[2].name, "ratio");
  EXPECT_EQ(snapshot->values[3].name, "text");
  EXPECT_EQ(std::get<bool>(*key(*snapshot, "flag").value), false);
  EXPECT_FALSE(key(*snapshot, "ratio").value.has_value());
  EXPECT_EQ(key(*snapshot, "ratio").value_kind, Kind::kFloat64);
}

TEST(ProgramBlackboardSnapshot, UpdateIsWholeSnapshotAtomicAndUnsetIsPerKey) {
  const auto source = binding();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  const auto original = previous->values;
  const auto result = ValidateAndBuildProgramBlackboardSnapshot(
      source, previous,
      update(source, {{"text", Kind::kString, std::nullopt},
                      {"ratio", Kind::kFloat64, IoConditionValueDTO{0.25}}}));
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(previous->values, original);
  EXPECT_EQ(previous->message_sequence, 1U);
  EXPECT_EQ(result.value()->message_sequence, 2U);
  EXPECT_FALSE(key(*result.value(), "text").value.has_value());
  EXPECT_EQ(key(*result.value(), "count"), key(*previous, "count"));
  EXPECT_EQ(key(*result.value(), "flag"), key(*previous, "flag"));
  EXPECT_EQ(std::get<double>(*key(*result.value(), "ratio").value), 0.25);
  auto invalid = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}},
                                 {"count", Kind::kInt64, IoConditionValueDTO{2.0}}});
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, invalid),
                "program_bb_value_type_mismatch");
  EXPECT_EQ(previous->values, original);
  EXPECT_EQ(previous->message_sequence, 1U);
}

TEST(ProgramBlackboardSnapshot, BaselineMustBeFirstExactUnionAtOneZero) {
  const auto source = binding();
  auto frame = baseline(source);
  frame.kind = PublicationKind::kUpdate;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_baseline_required");
  frame = baseline(source);
  frame.message_sequence = 2;
  frame.previous_sequence = 1;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_baseline_required");
  frame = baseline(source);
  frame.values.pop_back();
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_key_union_mismatch");
  frame = baseline(source);
  frame.values[0].name = "unknown";
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_key_union_mismatch");
  frame = baseline(source);
  frame.values[0] = frame.values[1];
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_duplicate_key");
}

TEST(ProgramBlackboardSnapshot, UpdatesRequireExactNextSequenceAndNeverRebaseline) {
  const auto source = binding();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, baseline(source)),
                "program_bb_update_required");
  for (const auto sequence : {0U, 1U, 3U}) {
    auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
    frame.message_sequence = sequence;
    expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                  "program_bb_sequence_mismatch");
  }
  auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
  frame.previous_sequence = 0;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                "program_bb_sequence_mismatch");
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, update(source, {})),
                "program_bb_key_union_mismatch");
  const auto next = ValidateAndBuildProgramBlackboardSnapshot(
      source, previous, update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}}));
  ASSERT_TRUE(next.ok());
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(
                    source, next.value(),
                    update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}})),
                "program_bb_sequence_mismatch");
}

TEST(ProgramBlackboardSnapshot, FullWidthSequenceIsExactAndCannotWrap) {
  const auto source = binding();
  const auto initial = start(source);
  ASSERT_NE(initial, nullptr);
  auto late = std::make_shared<Snapshot>(*initial);
  late->message_sequence = std::numeric_limits<std::uint64_t>::max() - 1U;
  auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}},
                      std::numeric_limits<std::uint64_t>::max());
  const auto last = ValidateAndBuildProgramBlackboardSnapshot(source, late, frame);
  ASSERT_TRUE(last.ok());
  EXPECT_EQ(last.value()->message_sequence, std::numeric_limits<std::uint64_t>::max());
  frame.message_sequence = 0;
  frame.previous_sequence = std::numeric_limits<std::uint64_t>::max();
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, last.value(), frame),
                "program_bb_sequence_exhausted");
}

TEST(ProgramBlackboardSnapshot, EveryScopeFieldAndPreviousSourceAreFenced) {
  const auto source = binding();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  std::vector<IoProgramBlackboardScopeDTO> scopes(7U, source.scope);
  scopes[0].request_owner_epoch = RequestOwnerEpoch("other");
  scopes[1].program_runner_owner_epoch = RequestOwnerEpoch("other");
  scopes[2].cycle_generation += 1;
  scopes[3].claim_generation = CellAdmissionClaimGeneration(9);
  scopes[4].program_run_id = ProgramRunId("other");
  scopes[5].program_binding_id = ProgramBindingId("other");
  scopes[6].active_publication_generation = ActiveBundlePublicationGeneration(10U);
  for (const auto& scope : scopes) {
    auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
    frame.scope = scope;
    const auto result = ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame);
    expect_reason(result, "program_bb_scope_stale");
    EXPECT_EQ(result.error().code, "stale_generation");
  }
  auto old = std::make_shared<Snapshot>(*previous);
  old->source_id = InterruptSourceId("old");
  auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, old, frame),
                "program_bb_scope_stale");
  old = std::make_shared<Snapshot>(*previous);
  old->incarnation = IoSourceIncarnation("old");
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, old, frame),
                "program_bb_scope_stale");
  old = std::make_shared<Snapshot>(*previous);
  old->scope.cycle_generation += 1;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, old, frame),
                "program_bb_scope_stale");
}

TEST(ProgramBlackboardSnapshot, BetweenRunsIsActualNullableScopeNotFabricatedRun) {
  auto source = binding();
  source.scope.claim_generation.reset();
  source.scope.program_run_id.reset();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  EXPECT_FALSE(previous->scope.program_run_id);
  EXPECT_FALSE(previous->scope.claim_generation);
  const auto result = ValidateAndBuildProgramBlackboardSnapshot(
      source, previous, update(source, {{"count", Kind::kInt64, std::nullopt}}));
  ASSERT_TRUE(result.ok());
  EXPECT_FALSE(key(*result.value(), "count").value);
  source.scope.program_run_id = ProgramRunId("invented");
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_binding_invalid");
}

TEST(ProgramBlackboardSnapshot, ScalarAlternativesAreExactAndFiniteWithoutCoercion) {
  const auto source = binding();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  for (auto row : std::vector<IoBlackboardValueDTO>{
           {"flag", Kind::kBoolean, IoConditionValueDTO{std::int64_t(1)}},
           {"count", Kind::kInt64, IoConditionValueDTO{1.0}},
           {"ratio", Kind::kFloat64, IoConditionValueDTO{std::int64_t(1)}},
           {"text", Kind::kString, IoConditionValueDTO{true}}}) {
    expect_reason(
        ValidateAndBuildProgramBlackboardSnapshot(source, previous, update(source, {row})),
        "program_bb_value_type_mismatch");
  }
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(
                    source, previous, update(source, {{"flag", Kind::kString, std::nullopt}})),
                "program_bb_value_kind_mismatch");
  for (const auto number :
       {std::numeric_limits<double>::infinity(), -std::numeric_limits<double>::infinity(),
        std::numeric_limits<double>::quiet_NaN()})
    expect_reason(ValidateAndBuildProgramBlackboardSnapshot(
                      source, previous,
                      update(source, {{"ratio", Kind::kFloat64, IoConditionValueDTO{number}}})),
                  "program_bb_value_nonfinite");
}

TEST(ProgramBlackboardSnapshot, FullInt64AndLongStringsKeepActualValues) {
  const auto source = binding();
  const auto initial = start(source);
  ASSERT_NE(initial, nullptr);
  auto first = ValidateAndBuildProgramBlackboardSnapshot(
      source, initial,
      update(source, {{"count", Kind::kInt64,
                       IoConditionValueDTO{std::numeric_limits<std::int64_t>::min()}},
                      {"text", Kind::kString, IoConditionValueDTO{std::string(5000U, 'x')}}}));
  ASSERT_TRUE(first.ok());
  EXPECT_EQ(std::get<std::int64_t>(*key(*first.value(), "count").value),
            std::numeric_limits<std::int64_t>::min());
  EXPECT_EQ(std::get<std::string>(*key(*first.value(), "text").value).size(), 5000U);
  auto second = ValidateAndBuildProgramBlackboardSnapshot(
      source, first.value(),
      update(
          source,
          {{"count", Kind::kInt64, IoConditionValueDTO{std::numeric_limits<std::int64_t>::max()}}},
          3U));
  ASSERT_TRUE(second.ok());
  EXPECT_EQ(std::get<std::int64_t>(*key(*second.value(), "count").value),
            std::numeric_limits<std::int64_t>::max());
}

TEST(ProgramBlackboardSnapshot, WholeStateBoundIncludesEarlierSiblingUpdates) {
  auto source = binding();
  source.keys = {{"a", Kind::kString}, {"b", Kind::kString}};
  source.maximum_value_bytes = 12U;
  auto frame = baseline(source);
  frame.values = {{"a", Kind::kString, std::nullopt}, {"b", Kind::kString, std::nullopt}};
  const auto initial = ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame);
  ASSERT_TRUE(initial.ok());
  const auto first = ValidateAndBuildProgramBlackboardSnapshot(
      source, initial.value(),
      update(source, {{"a", Kind::kString, IoConditionValueDTO{std::string(8U, 'x')}}}));
  ASSERT_TRUE(first.ok());
  const auto original = first.value()->values;
  expect_reason(
      ValidateAndBuildProgramBlackboardSnapshot(
          source, first.value(),
          update(source, {{"b", Kind::kString, IoConditionValueDTO{std::string(8U, 'y')}}}, 3U)),
      "program_bb_value_bound_exceeded");
  EXPECT_EQ(first.value()->values, original);
  EXPECT_EQ(first.value()->message_sequence, 2U);
  const auto exact = ValidateAndBuildProgramBlackboardSnapshot(
      source, first.value(),
      update(source, {{"b", Kind::kString, IoConditionValueDTO{std::string(2U, 'y')}}}, 3U));
  ASSERT_TRUE(exact.ok());
  EXPECT_EQ(exact.value()->message_sequence, 3U);
}

TEST(ProgramBlackboardSnapshot, AggregateAccountingUsesDeclaredScalarWidthsAndNullZero) {
  auto source = binding();
  auto frame = baseline(source);
  frame.values[3].value = IoConditionValueDTO{0.0};
  // Names18 + string5 + bool1 + int8 + double8 =40.
  source.maximum_value_bytes = 40U;
  ASSERT_TRUE(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame).ok());
  source.maximum_value_bytes = 39U;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame),
                "program_bb_value_bound_exceeded");
  frame.values[3].value.reset();
  source.maximum_value_bytes = 32U;
  ASSERT_TRUE(ValidateAndBuildProgramBlackboardSnapshot(source, {}, frame).ok());
}

TEST(ProgramBlackboardSnapshot, InvalidBindingDescriptorsAndDuplicatesCannotInstallValues) {
  auto source = binding();
  source.keys.push_back(source.keys.front());
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_duplicate_key");
  source = binding();
  source.keys[0].value_kind = static_cast<Kind>(99);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_key_descriptor_invalid");
  source = binding();
  source.keys[0].name = std::string("x\0y", 3);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_key_descriptor_invalid");
  source = binding();
  source.maximum_value_bytes = 1U;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_value_bound_exceeded");
  source = binding();
  source.maximum_value_bytes = 0U;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_binding_invalid");
  source = binding();
  source.scope.claim_generation = CellAdmissionClaimGeneration(0);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_binding_invalid");
}

TEST(ProgramBlackboardSnapshot, UpdatesRejectUnknownOrRepeatedKeysBeforePublishingAny) {
  const auto source = binding();
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(
                    source, previous,
                    update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}},
                                    {"flag", Kind::kBoolean, std::nullopt}})),
                "program_bb_duplicate_key");
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(
                    source, previous,
                    update(source, {{"unknown", Kind::kBoolean, IoConditionValueDTO{true}}})),
                "program_bb_key_union_mismatch");
  EXPECT_EQ(previous->values, start(source)->values);
}

TEST(ProgramBlackboardSnapshot, PreviousSnapshotMustStillMatchFullTypedSource) {
  const auto source = binding();
  const auto initial = start(source);
  ASSERT_NE(initial, nullptr);
  auto frame = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
  auto previous = std::make_shared<Snapshot>(*initial);
  previous->values.pop_back();
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                "program_bb_key_union_mismatch");
  previous = std::make_shared<Snapshot>(*initial);
  previous->values[0] = previous->values[1];
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                "program_bb_duplicate_key");
  previous = std::make_shared<Snapshot>(*initial);
  previous->message_sequence = 0;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                "program_bb_previous_snapshot_invalid");
  previous = std::make_shared<Snapshot>(*initial);
  previous->values.back().value = IoConditionValueDTO{std::string(9000U, 'x')};
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, previous, frame),
                "program_bb_value_bound_exceeded");
}

TEST(ProgramBlackboardSnapshot, ReceiptIsEvidenceOnlyAndNoClockOrAgePolicyIsAdded) {
  const auto source = binding();
  auto first = baseline(source);
  first.trusted_received_at_ns = -1;
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, first),
                "program_bb_receipt_invalid");
  const auto previous = start(source);
  ASSERT_NE(previous, nullptr);
  auto next = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
  next.trusted_received_at_ns = 0;
  const auto result = ValidateAndBuildProgramBlackboardSnapshot(source, previous, next);
  ASSERT_TRUE(result.ok());
  EXPECT_EQ(result.value()->trusted_received_at_ns, 0);
}

TEST(ProgramBlackboardSnapshot, AppliedPublicationCannotABAAtSameBindingCycleAndNullableRun) {
  auto source = binding();
  source.scope.claim_generation.reset();
  source.scope.program_run_id.reset();
  const auto first = start(source);
  ASSERT_NE(first, nullptr);
  const auto obsolete = update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}});
  // A -> B -> A can have the same binding and cycle but a new actual Applied
  // publication. Only the existing owner supplies that generation.
  source.scope.active_publication_generation = ActiveBundlePublicationGeneration(11U);
  const auto replacement = start(source);
  ASSERT_NE(replacement, nullptr);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, replacement, obsolete),
                "program_bb_scope_stale");
  expect_reason(
      ValidateAndBuildProgramBlackboardSnapshot(
          source, first, update(source, {{"flag", Kind::kBoolean, IoConditionValueDTO{true}}})),
      "program_bb_scope_stale");
  source.scope.active_publication_generation = ActiveBundlePublicationGeneration(0U);
  expect_reason(ValidateAndBuildProgramBlackboardSnapshot(source, {}, baseline(source)),
                "program_bb_binding_invalid");
}

TEST(ProgramBlackboardSnapshot, EmptyCompiledUnionHasExactEmptyBaselineOnly) {
  auto source = binding();
  source.keys.clear();
  auto first = baseline(source);
  first.values.clear();
  const auto result = ValidateAndBuildProgramBlackboardSnapshot(source, {}, first);
  ASSERT_TRUE(result.ok());
  EXPECT_TRUE(result.value()->values.empty());
  expect_reason(
      ValidateAndBuildProgramBlackboardSnapshot(source, result.value(), update(source, {})),
      "program_bb_key_union_mismatch");
}

}  // namespace
}  // namespace manyforge::core::detail

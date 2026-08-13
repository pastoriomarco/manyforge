#include <gtest/gtest.h>

#include <manyforge_core/prepared_motion_evidence_codec.hpp>

namespace manyforge::core {
namespace {

[[nodiscard]] std::string Digest(char value) {
  return "sha256:" + std::string(64U, value);
}

[[nodiscard]] TrajectoryDTO Trajectory() {
  return {{ResourceId("joint.1")}, {"joint_1"}, {{0.0, {0.0}, {}, {}}, {1.0, {0.5}, {0.0}, {0.0}}}};
}

[[nodiscard]] ProjectionRevisionDTO Projection() {
  return {RetainedBundleId(Digest('1')), GraphRevision(Digest('2')),
          SceneRevision{SceneId(Digest('3')), 4U, ContentDigest(Digest('4'))}};
}

[[nodiscard]] PreparedMotionPlanningEvidenceDTO Planning() {
  return {PlanResultDTO{Trajectory(), std::nullopt, Projection()}, 2U,
          PlannerCapabilityBindingDTO{
              CapabilityId("planning.motion.joint_goal"), CapabilityDefinitionRevision(Digest('5')),
              CapabilityProviderBindingId(Digest('6')),
              CapabilityProviderBindingRevision(Digest('7')), ProviderInstanceId("planner.1"),
              ProviderManifestId(Digest('8')), ProviderManifestRevision(Digest('9')),
              RuntimeUnitId("planner.runtime"), RuntimeEpoch("planner.epoch")}};
}

[[nodiscard]] PreparedMotionValidationEvidenceDTO Validation() {
  const CollisionWorldRevision world{3U, Digest('a')};
  CollisionDecision decision;
  decision.accepted = true;
  decision.severity = CollisionDecisionSeverity::kNone;
  decision.action = CollisionDecisionAction::kDispatch;
  decision.collision_world_revision = world;
  return {ValidationRevision("validation.3"), ValidationResultDTO{true, std::nullopt}, world,
          CollisionPolicyRevision{5U, Digest('b')}, std::move(decision)};
}

TEST(PreparedMotionEvidenceCodecTest, RoundTripsAcceptedPlanningAndValidationEvidence) {
  const auto planning = EncodePreparedMotionPlanningEvidence(Planning());
  ASSERT_TRUE(planning.ok()) << planning.error().message;
  const auto decoded_planning = DecodePreparedMotionPlanningEvidence(planning.value());
  ASSERT_TRUE(decoded_planning.ok()) << decoded_planning.error().message;
  EXPECT_EQ(decoded_planning.value().planning_attempt_index, 2U);
  EXPECT_EQ(EncodePreparedMotionPlanningEvidence(decoded_planning.value()).value(),
            planning.value());

  const auto validation = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(validation.ok()) << validation.error().message;
  const auto decoded_validation = DecodePreparedMotionValidationEvidence(validation.value());
  ASSERT_TRUE(decoded_validation.ok()) << decoded_validation.error().message;
  EXPECT_EQ(decoded_validation.value().collision_world_revision.revision_id, 3U);
  EXPECT_EQ(EncodePreparedMotionValidationEvidence(decoded_validation.value()).value(),
            validation.value());
}

TEST(PreparedMotionEvidenceCodecTest, RejectsFailureEvidenceAndClosedShapeViolations) {
  auto invalid = Validation();
  invalid.collision_decision.failure_archive_id = "archive.1";
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(invalid).ok());

  const auto encoded = EncodePreparedMotionValidationEvidence(Validation());
  ASSERT_TRUE(encoded.ok());
  auto unknown = encoded.value();
  unknown.insert(unknown.size() - 1U, ",\"unknown\":true");
  EXPECT_FALSE(DecodePreparedMotionValidationEvidence(unknown).ok());

  auto mismatched = Validation();
  mismatched.collision_decision.collision_world_revision->revision_id += 1U;
  EXPECT_FALSE(EncodePreparedMotionValidationEvidence(mismatched).ok());
}

}  // namespace
}  // namespace manyforge::core

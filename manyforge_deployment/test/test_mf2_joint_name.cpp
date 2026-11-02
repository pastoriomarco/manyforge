#include "mf2_joint_name_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace manyforge::deployment::detail {
namespace {

[[nodiscard]] UrdfJointIr joint(
    std::string name, std::string parent_link, std::string child_link,
    std::optional<std::string> mimic_master = std::nullopt) {
  std::optional<UrdfMimicIr> mimic;
  if (mimic_master.has_value()) {
    UrdfMimicIr value;
    value.joint_name = std::move(*mimic_master);
    mimic = std::move(value);
  }
  UrdfJointIr result;
  result.name = std::move(name);
  result.type = UrdfJointType::kRevolute;
  result.coordinate_kind = UrdfCoordinateKind::kAngular;
  result.parent_link_name = std::move(parent_link);
  result.child_link_name = std::move(child_link);
  result.mimic = std::move(mimic);
  return result;
}

[[nodiscard]] std::map<std::string, std::size_t, std::less<>> ranks(
    const UrdfDocumentIr& urdf) {
  const auto rows = dependency_ordered_joint_local_ranks(urdf);
  return {rows.begin(), rows.end()};
}

[[nodiscard]] UrdfDocumentIr document(std::vector<UrdfJointIr> joints) {
  UrdfDocumentIr result;
  result.robot_name = "fixture";
  result.joints = std::move(joints);
  return result;
}

[[nodiscard]] std::string numbered(std::string_view prefix,
                                   std::size_t value) {
  auto suffix = std::to_string(value);
  return std::string(prefix) + std::string(6U - suffix.size(), '0') +
         suffix;
}

TEST(Mf2JointName, OrdersSiblingMasterBeforeFollower) {
  const auto actual = ranks(document({
      joint("a_follower", "root", "follower", "z_master"),
      joint("z_master", "root", "master"),
  }));

  EXPECT_EQ(actual.at("z_master"), 0U);
  EXPECT_EQ(actual.at("a_follower"), 1U);
}

TEST(Mf2JointName, ProjectsDependencyAcrossTopLevelSiblingBranches) {
  const auto actual = ranks(document({
      joint("a_follower_branch", "root", "follower_branch"),
      joint("z_master_branch", "root", "master_branch"),
      joint("follower", "follower_branch", "follower_tip", "master"),
      joint("master", "master_branch", "master_tip"),
  }));

  EXPECT_EQ(actual.at("z_master_branch"), 0U);
  EXPECT_EQ(actual.at("a_follower_branch"), 1U);
  EXPECT_EQ(actual.at("master"), 0U);
  EXPECT_EQ(actual.at("follower"), 0U);
}

TEST(Mf2JointName,
     ProjectsUnequalDepthDependencyAtNonRootLowestCommonAncestor) {
  const auto actual = ranks(document({
      joint("shared_trunk", "root", "shared"),
      joint("a_follower_branch", "shared", "follower_branch"),
      joint("follower", "follower_branch", "follower_tip", "master"),
      joint("z_master_branch", "shared", "master_branch"),
      joint("master_middle", "master_branch", "master_middle_link"),
      joint("master", "master_middle_link", "master_tip"),
  }));

  EXPECT_EQ(actual.at("shared_trunk"), 0U);
  EXPECT_EQ(actual.at("z_master_branch"), 0U);
  EXPECT_EQ(actual.at("a_follower_branch"), 1U);
  EXPECT_EQ(actual.at("master_middle"), 0U);
  EXPECT_EQ(actual.at("master"), 0U);
  EXPECT_EQ(actual.at("follower"), 0U);
}

TEST(Mf2JointName, LeavesAncestorMasterDependencyAlreadySafe) {
  const auto actual = ranks(document({
      joint("z_master", "root", "middle"),
      joint("a_follower", "middle", "tip", "z_master"),
  }));

  EXPECT_EQ(actual.at("z_master"), 0U);
  EXPECT_EQ(actual.at("a_follower"), 0U);
}

TEST(Mf2JointName, AcceptsAndOrdersAcyclicMimicChains) {
  const auto actual = ranks(document({
      joint("c_follower", "root", "c", "b_middle"),
      joint("b_middle", "root", "b", "a_master"),
      joint("a_master", "root", "a"),
  }));

  EXPECT_EQ(actual.at("a_master"), 0U);
  EXPECT_EQ(actual.at("b_middle"), 1U);
  EXPECT_EQ(actual.at("c_follower"), 2U);
}

TEST(Mf2JointName, IsIndependentOfInputJointRowOrder) {
  auto ordered = document({
      joint("branch_a", "root", "a"),
      joint("branch_b", "root", "b"),
      joint("master", "b", "master_tip"),
      joint("follower", "a", "follower_tip", "master"),
  });
  auto shuffled = ordered;
  std::reverse(shuffled.joints.begin(), shuffled.joints.end());

  EXPECT_EQ(ranks(ordered), ranks(shuffled));
}

TEST(Mf2JointName, DoesNotRenumberChildrenOfAnUnrelatedParent) {
  auto without_mimic = document({
      joint("branch_a", "root", "a"),
      joint("branch_b", "root", "b"),
      joint("a_first", "a", "a_first_tip"),
      joint("a_second", "a", "a_second_tip"),
      joint("b_first", "b", "b_first_tip"),
      joint("b_second", "b", "b_second_tip"),
  });
  auto with_mimic = without_mimic;
  const auto follower = std::find_if(
      with_mimic.joints.begin(), with_mimic.joints.end(),
      [](const UrdfJointIr& candidate) {
        return candidate.name == "a_first";
      });
  ASSERT_NE(follower, with_mimic.joints.end());
  UrdfMimicIr mimic;
  mimic.joint_name = "a_second";
  follower->mimic = std::move(mimic);

  const auto before = ranks(without_mimic);
  const auto after = ranks(with_mimic);
  EXPECT_EQ(before.at("b_first"), after.at("b_first"));
  EXPECT_EQ(before.at("b_second"), after.at("b_second"));
}

TEST(Mf2JointName, RejectsMasterInFollowerDescendantSubtree) {
  const auto impossible = document({
      joint("follower", "root", "middle", "master"),
      joint("master", "middle", "tip"),
  });

  EXPECT_THROW((void)ranks(impossible), Mf2JointOrderingError);
}

TEST(Mf2JointName, RejectsCyclicProjectedSiblingConstraints) {
  const auto cyclic = document({
      joint("branch_a", "root", "a"),
      joint("branch_b", "root", "b"),
      joint("a_follower", "a", "a_follower_tip", "b_master"),
      joint("a_master", "a", "a_master_tip"),
      joint("b_follower", "b", "b_follower_tip", "a_master"),
      joint("b_master", "b", "b_master_tip"),
  });

  EXPECT_THROW((void)ranks(cyclic), Mf2JointOrderingError);
}

TEST(Mf2JointName, RejectsCyclicPhysicalParentGraph) {
  const auto cyclic = document({
      joint("joint_a", "link_b", "link_a"),
      joint("joint_b", "link_a", "link_b"),
  });

  EXPECT_THROW((void)ranks(cyclic), Mf2JointOrderingError);
}

TEST(Mf2JointName, RejectsLinkWithMultipleParentJoints) {
  const auto multiple_parents = document({
      joint("joint_a", "root_a", "shared_child"),
      joint("joint_b", "root_b", "shared_child"),
  });

  EXPECT_THROW((void)ranks(multiple_parents), Mf2JointOrderingError);
}

TEST(Mf2JointName, BoundsStorageForThousandsOfNoMimicSiblings) {
  constexpr std::size_t kJointCount = 16384U;
  std::vector<UrdfJointIr> joints;
  joints.reserve(kJointCount);
  for (std::size_t index = 0U; index < kJointCount; ++index) {
    const auto name = numbered("joint_", index);
    joints.push_back(joint(name, "root", numbered("link_", index)));
  }

  const auto actual = ranks(document(std::move(joints)));

  ASSERT_EQ(actual.size(), kJointCount);
  EXPECT_EQ(actual.at("joint_000000"), 0U);
  EXPECT_EQ(actual.at("joint_008191"), 8191U);
  EXPECT_EQ(actual.at("joint_016383"), 16383U);
}

TEST(Mf2JointName, BoundsTraversalForThousandsOfDeepAncestorJoints) {
  constexpr std::size_t kJointCount = 16384U;
  std::vector<UrdfJointIr> joints;
  joints.reserve(kJointCount);
  std::string parent_link = "root";
  for (std::size_t index = 0U; index < kJointCount; ++index) {
    const auto name = numbered("joint_", index);
    const auto child_link = numbered("link_", index);
    joints.push_back(joint(
        name, parent_link, child_link,
        index + 1U == kJointCount
            ? std::optional<std::string>("joint_000000")
            : std::nullopt));
    parent_link = child_link;
  }

  const auto actual = ranks(document(std::move(joints)));

  ASSERT_EQ(actual.size(), kJointCount);
  EXPECT_EQ(actual.at("joint_000000"), 0U);
  EXPECT_EQ(actual.at("joint_008191"), 0U);
  EXPECT_EQ(actual.at("joint_016383"), 0U);
}

TEST(Mf2JointName, EncodesFixedWidthRankBeforeStableIdentityDigest) {
  const std::vector<std::pair<std::string_view, std::string_view>> fields{
      {"cell_id", "cell"},
      {"model_instance_id", "arm"},
      {"model_asset_id", "asset"},
      {"model_revision",
       "sha256:1111111111111111111111111111111111111111111111111111111111111111"},
      {"kind", "joint"},
      {"scope", ""},
      {"source", "wrist"},
  };
  const auto first = make_mf2_generated_joint_name(2U, fields);
  const auto moved = make_mf2_generated_joint_name(18U, fields);

  ASSERT_TRUE(first.starts_with("mf2_joint_000002_"));
  ASSERT_TRUE(moved.starts_with("mf2_joint_000018_"));
  EXPECT_EQ(
      first,
      "mf2_joint_000002_"
      "ab013a0a4b0e833472920ffa9a13fe8158a9eaaa3983080f8008622fa60ab097");
  EXPECT_EQ(first.substr(first.rfind('_') + 1U),
            moved.substr(moved.rfind('_') + 1U));
  EXPECT_EQ(first.size(), 81U);
  EXPECT_THROW(
      (void)make_mf2_generated_joint_name(
          kMf2JointMaximumLocalRank + 1U, fields),
      Mf2JointOrderingError);
}

}  // namespace
}  // namespace manyforge::deployment::detail

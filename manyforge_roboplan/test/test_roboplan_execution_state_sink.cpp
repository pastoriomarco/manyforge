#include <manyforge_roboplan/native_kernel_factory.hpp>
#include <manyforge_roboplan/roboplan_execution_state_sink.hpp>

#include <gtest/gtest.h>

#include <Eigen/Dense>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using manyforge::core::ExecutionStateSinkStatus;
using manyforge::core::ExecutionStateUpdateMode;
using manyforge::roboplan_backend::NativeKernelBundle;
using manyforge::roboplan_backend::RoboPlanExecutionStateSink;
using manyforge::roboplan_backend::create_native_kernel;

std::filesystem::path repo_root() {
  return std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
}

std::shared_ptr<NativeKernelBundle> create_ur10e_robotiq_bundle() {
  const auto root = repo_root();
  const auto model_dir =
      root / "manyforge_example_models/models/ur10e_robotiq_2f_140";
  return create_native_kernel(
      "ur10e_robotiq_2f_140", "arm",
      (model_dir / "ur10e_robotiq_2f_140.urdf").string(),
      (model_dir / "ur10e_robotiq_2f_140.srdf").string(), {root.string()});
}

std::filesystem::path write_private_mimic_model() {
  const auto suffix = std::to_string(
      std::chrono::steady_clock::now().time_since_epoch().count());
  const auto dir =
      std::filesystem::temp_directory_path() /
      ("manyforge_rma_mimic_sink_" + suffix);
  std::filesystem::create_directories(dir);

  {
    std::ofstream urdf(dir / "rma_mimic_sink.urdf");
    urdf << R"xml(
<robot name="rma_mimic_sink">
  <link name="base"/>
  <link name="gripper_base"/>
  <joint name="gripper_mount" type="fixed">
    <parent link="base"/>
    <child link="gripper_base"/>
  </joint>
  <link name="right_finger"/>
  <joint name="__rma_mimic_0000_right_finger_joint" type="prismatic">
    <parent link="gripper_base"/>
    <child link="right_finger"/>
    <axis xyz="0 -1 0"/>
    <limit lower="0" upper="0.01" effort="10" velocity="1"/>
  </joint>
  <link name="left_finger"/>
  <joint name="__rma_mimic_0001_left_finger_joint" type="prismatic">
    <parent link="gripper_base"/>
    <child link="left_finger"/>
    <axis xyz="0 1 0"/>
    <limit lower="0" upper="0.01" effort="10" velocity="1"/>
    <mimic joint="__rma_mimic_0000_right_finger_joint" multiplier="1" offset="0"/>
  </joint>
</robot>
)xml";
  }
  {
    std::ofstream srdf(dir / "rma_mimic_sink.srdf");
    srdf << R"xml(
<robot name="rma_mimic_sink">
  <group name="gripper">
    <joint name="__rma_mimic_0001_left_finger_joint"/>
    <joint name="__rma_mimic_0000_right_finger_joint"/>
  </group>
  <group_state name="open" group="gripper">
    <joint name="__rma_mimic_0001_left_finger_joint" value="0.0081"/>
    <joint name="__rma_mimic_0000_right_finger_joint" value="0.0081"/>
  </group_state>
</robot>
)xml";
  }
  return dir;
}

double mimic_expanded_position(const roboplan::Scene& scene,
                               const std::string& joint_name) {
  const auto q_with_mimics = scene.getCurrentJointPositionsWithMimics();
  const auto joint_names = scene.getJointNamesWithMimics();
  const auto joint_it =
      std::find(joint_names.begin(), joint_names.end(), joint_name);
  if (joint_it == joint_names.end()) {
    throw std::runtime_error("joint missing from mimic-expanded readout");
  }
  const auto index =
      static_cast<Eigen::Index>(std::distance(joint_names.begin(), joint_it));
  return q_with_mimics[index];
}

class RoboPlanExecutionStateSinkTest : public ::testing::Test {
 protected:
  void SetUp() override {
    bundle_ = create_native_kernel("ur5_gripper", "arm");
    ASSERT_NE(bundle_, nullptr);
    ASSERT_NE(bundle_->scene, nullptr);
    scene_mutex_ = std::make_shared<std::mutex>();
    sink_ = std::make_unique<RoboPlanExecutionStateSink>(
        bundle_->scene, scene_mutex_);
  }

  Eigen::VectorXd current_positions() const {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    return bundle_->scene->getCurrentJointPositions();
  }

  void set_current_positions(const Eigen::VectorXd& positions) {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    bundle_->scene->setJointPositions(positions);
  }

  std::vector<double> group_positions_for(const std::string& group_id,
                                          double base,
                                          double step) const {
    const auto maybe_group_info = bundle_->scene->getJointGroupInfo(group_id);
    EXPECT_TRUE(maybe_group_info);
    if (!maybe_group_info) {
      return {};
    }
    std::vector<double> result;
    result.reserve(maybe_group_info->joint_names.size());
    for (size_t i = 0; i < maybe_group_info->joint_names.size(); ++i) {
      result.push_back(base + step * static_cast<double>(i));
    }
    return result;
  }

  void expect_group_matches(const std::string& group_id,
                            const std::vector<double>& expected) const {
    std::lock_guard<std::mutex> lock(*scene_mutex_);
    const auto maybe_group_info = bundle_->scene->getJointGroupInfo(group_id);
    ASSERT_TRUE(maybe_group_info);
    const auto& group_info = *maybe_group_info;
    ASSERT_EQ(expected.size(), group_info.joint_names.size());
    ASSERT_EQ(group_info.joint_names.size(),
              static_cast<size_t>(group_info.q_indices.size()));
    const auto q = bundle_->scene->getCurrentJointPositions();
    for (size_t i = 0; i < expected.size(); ++i) {
      const auto q_index = group_info.q_indices[static_cast<Eigen::Index>(i)];
      ASSERT_GE(q_index, 0);
      ASSERT_LT(q_index, q.size());
      EXPECT_NEAR(q[q_index], expected[i], 1e-12);
    }
  }

  std::shared_ptr<NativeKernelBundle> bundle_;
  std::shared_ptr<std::mutex> scene_mutex_;
  std::unique_ptr<RoboPlanExecutionStateSink> sink_;
};

TEST(RoboPlanExecutionStateSink, ConstructorRejectsNullScene) {
  EXPECT_THROW(
      RoboPlanExecutionStateSink(nullptr, std::make_shared<std::mutex>()),
      std::invalid_argument);
}

TEST_F(RoboPlanExecutionStateSinkTest, EmptyGroupVectorIsNoOpSuccess) {
  std::lock_guard<std::mutex> lock(*scene_mutex_);
  const auto result = sink_->apply_group_joint_positions(
      "arm", {}, ExecutionStateUpdateMode::kBestEffort);
  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kApplied);
}

TEST_F(RoboPlanExecutionStateSinkTest, GroupSizedVectorExpandsToFullSceneState) {
  const auto initial = Eigen::VectorXd::Zero(current_positions().size());
  set_current_positions(initial);
  const auto arm_positions = group_positions_for("arm", 0.1, 0.05);

  const auto result = sink_->apply_group_joint_positions(
      "arm", arm_positions, ExecutionStateUpdateMode::kBlocking);

  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kApplied);
  expect_group_matches("arm", arm_positions);
  const auto actual = current_positions();
  ASSERT_EQ(actual.size(), initial.size());
}

TEST_F(RoboPlanExecutionStateSinkTest, FullSizedVectorAppliesDirectly) {
  const auto size = current_positions().size();
  std::vector<double> full_positions;
  full_positions.reserve(static_cast<size_t>(size));
  for (Eigen::Index i = 0; i < size; ++i) {
    full_positions.push_back(0.02 * static_cast<double>(i + 1));
  }

  const auto result = sink_->apply_group_joint_positions(
      "arm", full_positions, ExecutionStateUpdateMode::kBlocking);

  ASSERT_EQ(result.status, ExecutionStateSinkStatus::kApplied);
  const auto actual = current_positions();
  ASSERT_EQ(actual.size(), size);
  for (Eigen::Index i = 0; i < size; ++i) {
    EXPECT_NEAR(actual[i], full_positions[static_cast<size_t>(i)], 1e-12);
  }
}

TEST_F(RoboPlanExecutionStateSinkTest, BestEffortGroupUpdateReturnsBusyWhenLocked) {
  const auto before = current_positions();
  std::lock_guard<std::mutex> lock(*scene_mutex_);

  const auto result = sink_->apply_group_joint_positions(
      "arm", group_positions_for("arm", 0.2, 0.01),
      ExecutionStateUpdateMode::kBestEffort);

  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kBusy);
  const auto after = bundle_->scene->getCurrentJointPositions();
  ASSERT_EQ(after.size(), before.size());
  for (Eigen::Index i = 0; i < after.size(); ++i) {
    EXPECT_NEAR(after[i], before[i], 1e-12);
  }
}

TEST_F(RoboPlanExecutionStateSinkTest, RejectsInvalidGroupPositionInputs) {
  const auto full_size = current_positions().size();
  const auto unknown_group = sink_->apply_group_joint_positions(
      "missing_group", {0.1, 0.2}, ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(unknown_group.status, ExecutionStateSinkStatus::kInvalidGroup);

  std::vector<double> wrong_size(static_cast<size_t>(full_size + 1), 0.1);
  const auto invalid_size = sink_->apply_group_joint_positions(
      "arm", wrong_size, ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(invalid_size.status, ExecutionStateSinkStatus::kInvalidPositions);

  auto non_finite = group_positions_for("arm", 0.0, 0.01);
  ASSERT_FALSE(non_finite.empty());
  non_finite.front() = std::numeric_limits<double>::quiet_NaN();
  const auto invalid_position = sink_->apply_group_joint_positions(
      "arm", non_finite, ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(invalid_position.status, ExecutionStateSinkStatus::kInvalidPositions);
}

TEST_F(RoboPlanExecutionStateSinkTest, SingleJointReadAndWriteUsesGroupProjection) {
  bundle_ = create_ur10e_robotiq_bundle();
  ASSERT_NE(bundle_, nullptr);
  ASSERT_NE(bundle_->scene, nullptr);
  scene_mutex_ = std::make_shared<std::mutex>();
  sink_ = std::make_unique<RoboPlanExecutionStateSink>(
      bundle_->scene, scene_mutex_);

  const auto maybe_group_info = bundle_->scene->getJointGroupInfo("gripper");
  ASSERT_TRUE(maybe_group_info);
  ASSERT_FALSE(maybe_group_info->joint_names.empty());
  const auto joint_name = maybe_group_info->joint_names.front();

  const auto result = sink_->apply_group_joint_position(
      "gripper", joint_name, 0.42, ExecutionStateUpdateMode::kBlocking);

  ASSERT_EQ(result.status, ExecutionStateSinkStatus::kApplied);
  const auto projected = sink_->group_joint_position("gripper", joint_name);
  ASSERT_TRUE(projected.has_value());
  EXPECT_NEAR(*projected, 0.42, 1e-12);
}

TEST(RoboPlanExecutionStateSink,
     RmaMimicGroupUsesCanonicalCommandableCompactSlot) {
  const auto model_dir = write_private_mimic_model();
  auto scene = std::make_shared<roboplan::Scene>(
      "rma_mimic_sink", model_dir / "rma_mimic_sink.urdf",
      model_dir / "rma_mimic_sink.srdf",
      std::vector<std::filesystem::path>{model_dir});
  auto scene_mutex = std::make_shared<std::mutex>();
  RoboPlanExecutionStateSink sink(scene, scene_mutex);
  sink.set_robot_model_authority_metadata(
      {
          {"right_finger_joint", "__rma_mimic_0000_right_finger_joint"},
          {"left_finger_joint", "__rma_mimic_0001_left_finger_joint"},
      },
      {
          {"__rma_mimic_0000_right_finger_joint", "right_finger_joint"},
          {"__rma_mimic_0001_left_finger_joint", "left_finger_joint"},
      },
      {{"gripper", {"right_finger_joint"}}});

  const auto maybe_group_info = scene->getJointGroupInfo("gripper");
  ASSERT_TRUE(maybe_group_info);
  EXPECT_EQ(maybe_group_info->joint_names.size(), 2u);
  EXPECT_EQ(static_cast<size_t>(maybe_group_info->q_indices.size()), 1u);

  const auto open_result = sink.apply_group_joint_position(
      "gripper", "right_finger_joint", 0.0081,
      ExecutionStateUpdateMode::kBlocking);

  ASSERT_EQ(open_result.status, ExecutionStateSinkStatus::kApplied);
  const auto command_position =
      sink.group_joint_position("gripper", "right_finger_joint");
  ASSERT_TRUE(command_position.has_value());
  EXPECT_NEAR(*command_position, 0.0081, 1e-12);
  EXPECT_FALSE(sink.group_joint_position("gripper", "left_finger_joint"));
  EXPECT_NEAR(
      mimic_expanded_position(*scene, "__rma_mimic_0000_right_finger_joint"),
      0.0081, 1e-12);
  EXPECT_NEAR(
      mimic_expanded_position(*scene, "__rma_mimic_0001_left_finger_joint"),
      0.0081, 1e-12);

  const auto compact_result = sink.apply_group_joint_positions(
      "gripper", {0.004}, ExecutionStateUpdateMode::kBlocking);
  ASSERT_EQ(compact_result.status, ExecutionStateSinkStatus::kApplied);
  EXPECT_NEAR(*sink.group_joint_position("gripper", "right_finger_joint"),
              0.004, 1e-12);

  const auto expanded_result = sink.apply_group_joint_positions(
      "gripper", {0.001, 0.006}, ExecutionStateUpdateMode::kBlocking);
  ASSERT_EQ(expanded_result.status, ExecutionStateSinkStatus::kApplied);
  EXPECT_NEAR(*sink.group_joint_position("gripper", "right_finger_joint"),
              0.006, 1e-12);
  EXPECT_NEAR(
      mimic_expanded_position(*scene, "__rma_mimic_0001_left_finger_joint"),
      0.006, 1e-12);
}

TEST_F(RoboPlanExecutionStateSinkTest, RejectsInvalidSingleJointInputs) {
  bundle_ = create_ur10e_robotiq_bundle();
  ASSERT_NE(bundle_, nullptr);
  ASSERT_NE(bundle_->scene, nullptr);
  scene_mutex_ = std::make_shared<std::mutex>();
  sink_ = std::make_unique<RoboPlanExecutionStateSink>(
      bundle_->scene, scene_mutex_);

  const auto unknown_group = sink_->apply_group_joint_position(
      "missing_group", "finger_joint", 0.1, ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(unknown_group.status, ExecutionStateSinkStatus::kInvalidGroup);

  const auto unknown_joint = sink_->apply_group_joint_position(
      "gripper", "missing_joint", 0.1, ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(unknown_joint.status, ExecutionStateSinkStatus::kInvalidJoint);

  const auto non_finite = sink_->apply_group_joint_position(
      "gripper", "finger_joint", std::numeric_limits<double>::infinity(),
      ExecutionStateUpdateMode::kBlocking);
  EXPECT_EQ(non_finite.status, ExecutionStateSinkStatus::kInvalidPositions);
}

TEST_F(RoboPlanExecutionStateSinkTest,
       BestEffortSingleJointUpdateReturnsBusyWhenLocked) {
  bundle_ = create_ur10e_robotiq_bundle();
  ASSERT_NE(bundle_, nullptr);
  ASSERT_NE(bundle_->scene, nullptr);
  scene_mutex_ = std::make_shared<std::mutex>();
  sink_ = std::make_unique<RoboPlanExecutionStateSink>(
      bundle_->scene, scene_mutex_);

  const auto maybe_group_info = bundle_->scene->getJointGroupInfo("gripper");
  ASSERT_TRUE(maybe_group_info);
  ASSERT_FALSE(maybe_group_info->joint_names.empty());
  const auto joint_name = maybe_group_info->joint_names.front();
  std::lock_guard<std::mutex> lock(*scene_mutex_);

  const auto result = sink_->apply_group_joint_position(
      "gripper", joint_name, 0.5, ExecutionStateUpdateMode::kBestEffort);

  EXPECT_EQ(result.status, ExecutionStateSinkStatus::kBusy);
}

TEST_F(RoboPlanExecutionStateSinkTest, MissingGroupOrJointReadReturnsNullopt) {
  bundle_ = create_ur10e_robotiq_bundle();
  ASSERT_NE(bundle_, nullptr);
  ASSERT_NE(bundle_->scene, nullptr);
  scene_mutex_ = std::make_shared<std::mutex>();
  sink_ = std::make_unique<RoboPlanExecutionStateSink>(
      bundle_->scene, scene_mutex_);

  EXPECT_FALSE(sink_->group_joint_position("missing_group", "finger_joint"));
  EXPECT_FALSE(sink_->group_joint_position("gripper", "missing_joint"));
}

}  // namespace

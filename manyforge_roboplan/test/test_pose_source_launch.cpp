#include <manyforge_roboplan/pose_source_launch.hpp>

#include <manyforge_content_identity/canonical_value.hpp>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <unistd.h>

namespace {

using namespace manyforge::roboplan_backend;
using manyforge::content_identity::CanonicalValue;

CanonicalValue observer_configuration(bool joint_state) {
  CanonicalValue::Object row{
      {"simulation_poll_period_ms", CanonicalValue(250.0)},
      {"simulation_entities", CanonicalValue(CanonicalValue::Array{})},
      {"detection3d_sources", CanonicalValue(CanonicalValue::Array{})}};
  if (joint_state)
    row.emplace_back(
        "joint_state",
        CanonicalValue(CanonicalValue::Object{
            {"topic", CanonicalValue("/isaac/joint_states")},
            {"object_id", CanonicalValue("robot")},
            {"policy_id", CanonicalValue("viewer")},
            {"lifetime_ns", CanonicalValue(1e9)},
            {"eligible", CanonicalValue(false)},
            {"required_joint_names",
             CanonicalValue(CanonicalValue::Array{CanonicalValue("j1")})}}));
  return CanonicalValue(std::move(row));
}

TEST(PoseSourceLaunch, DocumentCarriesTheCellSourceAndAbsentJointStateAsNull) {
  EXPECT_EQ(pose_source_observer_document("cell", "isaac_scene_objects",
                                          observer_configuration(false)),
            "{\"api_version\":\"manyforge.isaac_pose_observer_site.v1\","
            "\"cell_id\":\"cell\",\"detection3d_sources\":[],"
            "\"joint_state\":null,\"simulation_entities\":[],"
            "\"simulation_poll_period_ms\":250,"
            "\"source_id\":\"isaac_scene_objects\"}");
  const auto with_joint = pose_source_observer_document(
      "cell", "isaac_scene_objects", observer_configuration(true));
  EXPECT_NE(with_joint.find("\"joint_state\":{"), std::string::npos);
  EXPECT_NE(with_joint.find("\"lifetime_ns\":1000000000"), std::string::npos);
}

TEST(PoseSourceLaunch, PlanWritesTheDocumentAndLaunchesTheObserver) {
  const auto root = std::filesystem::temp_directory_path() /
                    ("manyforge-pose-source-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto plan = plan_pose_source("cell", "isaac_scene_objects",
                                     observer_configuration(false),
                                     root / "run", root / "manager");
  EXPECT_EQ(plan.site_config_path,
            root / "run/controller-launch-artifacts/isaac_scene_objects/"
                   "pose_source.json");
  write_pose_source_document(plan);
  std::ifstream in(plan.site_config_path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}),
            plan.document);

  const auto command =
      pose_source_launch_command(plan, "isaac_scene_objects.abc");
  EXPECT_EQ(command.controllable_component_id, "isaac_scene_objects");
  EXPECT_EQ(command.lane, ControllerLaunchLane::kPoseSource);
  EXPECT_TRUE(command.keep_across_apply);
  EXPECT_EQ(command.argv,
            (std::vector<std::string>{
                "ros2", "launch", "manyforge_ros_bringup",
                "pose_source.launch.py",
                "site_config_path:=" + plan.site_config_path.string(),
                "manager_run_root:=" + (root / "manager").string(),
                "source_epoch_base:=isaac_scene_objects.abc"}));
  // The per-spawn epoch base never changes the identity; the document does.
  EXPECT_EQ(pose_source_launch_command(plan, "isaac_scene_objects.xyz").identity,
            command.identity);
  const auto changed = plan_pose_source("cell", "isaac_scene_objects",
                                        observer_configuration(true),
                                        root / "run", root / "manager");
  EXPECT_NE(pose_source_launch_command(changed, "isaac_scene_objects.abc")
                .identity,
            command.identity);
  EXPECT_FALSE(pose_source_launch_survives(nullptr, plan));
  std::filesystem::remove_all(root);
}

TEST(PoseSourceLaunch, ExitStatus78IsATypedConfigErrorWithTheOutputTail) {
  const auto exited = pose_source_exit_fault("exited with status 1", 1, "");
  EXPECT_EQ(exited.code, "pose_source_exited");
  EXPECT_TRUE(exited.details.empty());

  const auto direct = pose_source_exit_fault(
      "exited with status 78", 78,
      "isaac_pose_observer_failed: site config api_version is unsupported\n");
  EXPECT_EQ(direct.code, "pose_source_config_invalid");
  EXPECT_EQ(direct.message, "exited with status 78");
  EXPECT_EQ(direct.details.at("output_tail"),
            "isaac_pose_observer_failed: site config api_version is "
            "unsupported");

  // `ros2 launch` exits 0 and reports its child's status in the output.
  const std::string long_prefix(400U, 'x');
  const auto launched = pose_source_exit_fault(
      "exited with status 0", 0,
      long_prefix + "\n[ERROR] [pose_source_snapshot_publisher-1]: process has "
                    "died [pid 7, exit code 78, cmd '...'].\n");
  EXPECT_EQ(launched.code, "pose_source_config_invalid");
  EXPECT_LE(launched.details.at("output_tail").size(), 256U);
  EXPECT_NE(launched.details.at("output_tail").find("exit code 78"),
            std::string::npos);
}

TEST(PoseSourceLaunch, AiModelPlanWritesTheProbeDocumentAndLaunchesTheProbe) {
  EXPECT_EQ(ai_model_probe_document("cell", "assistant_model", "endpoint",
                                    "http://127.0.0.1:8000/v1", "gemma",
                                    std::nullopt, 2000),
            "{\"api_key_env\":null,"
            "\"api_version\":\"manyforge.ai_model_probe_site.v1\","
            "\"base_url\":\"http://127.0.0.1:8000/v1\","
            "\"cell_id\":\"cell\",\"component_id\":\"assistant_model\","
            "\"model\":\"gemma\",\"probe_period_ms\":2000,"
            "\"stream_id\":\"endpoint\"}");

  const auto root = std::filesystem::temp_directory_path() /
                    ("manyforge-ai-model-" + std::to_string(::getpid()));
  std::filesystem::remove_all(root);
  const auto plan =
      plan_ai_model("cell", "assistant_model", "endpoint",
                    "http://127.0.0.1:8000/v1", "gemma",
                    std::string("MODEL_TOKEN"), root / "run");
  EXPECT_EQ(plan.lane, ControllerLaunchLane::kAiModel);
  EXPECT_EQ(plan.site_config_path,
            root / "run/controller-launch-artifacts/assistant_model/"
                   "ai_model.json");
  EXPECT_NE(plan.document.find("\"api_key_env\":\"MODEL_TOKEN\""),
            std::string::npos);
  EXPECT_NE(plan.document.find("\"probe_period_ms\":2000"), std::string::npos);
  write_pose_source_document(plan);
  std::ifstream in(plan.site_config_path, std::ios::binary);
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), {}),
            plan.document);

  const auto command = pose_source_launch_command(plan, "assistant_model.abc");
  EXPECT_EQ(command.controllable_component_id, "assistant_model");
  EXPECT_EQ(command.lane, ControllerLaunchLane::kAiModel);
  EXPECT_TRUE(command.keep_across_apply);
  EXPECT_EQ(command.argv,
            (std::vector<std::string>{
                "ros2", "launch", "manyforge_ros_bringup",
                "ai_model.launch.py",
                "site_config_path:=" + plan.site_config_path.string()}));
  EXPECT_EQ(pose_source_launch_command(plan, "assistant_model.xyz").identity,
            command.identity);
  const auto changed =
      plan_ai_model("cell", "assistant_model", "endpoint",
                    "http://127.0.0.1:8000/v1", "other", std::nullopt,
                    root / "run");
  EXPECT_NE(pose_source_launch_command(changed, "assistant_model.abc").identity,
            command.identity);
  EXPECT_FALSE(pose_source_launch_survives(nullptr, plan));
  std::filesystem::remove_all(root);
}

TEST(PoseSourceLaunch, AiModelExitCodesAreTyped) {
  EXPECT_EQ(pose_source_exit_fault("exited with status 1", 1, "",
                                   ControllerLaunchLane::kAiModel)
                .code,
            "ai_model_exited");
  const auto invalid = pose_source_exit_fault(
      "exited with status 0", 0,
      "[ERROR] [ai_model_probe-1]: process has died [pid 7, exit code 78, "
      "cmd '...'].\n",
      ControllerLaunchLane::kAiModel);
  EXPECT_EQ(invalid.code, "ai_model_config_invalid");
  EXPECT_NE(invalid.details.at("output_tail").find("exit code 78"),
            std::string::npos);
}

} // namespace

#include <manyforge_roboplan/direct_collision_query_engine.hpp>

#include <roboplan_example_models/resources.hpp>

#include <Eigen/Dense>
#include <chrono>
#include <map>
#include <stdexcept>
#include <utility>

namespace manyforge::roboplan_backend {

namespace {

Eigen::VectorXd to_eigen_(const std::vector<double>& v) {
  Eigen::VectorXd out(static_cast<Eigen::Index>(v.size()));
  for (std::size_t i = 0; i < v.size(); ++i) {
    out(static_cast<Eigen::Index>(i)) = v[i];
  }
  return out;
}

struct ExampleModelLookup {
  std::string urdf_rel;
  std::string srdf_rel;
  std::string default_group;
};

const std::map<std::string, ExampleModelLookup>& example_model_lookup() {
  static const std::map<std::string, ExampleModelLookup> configs = {
      {"ur5_gripper",
       {"ur_robot_model/ur5_gripper.urdf", "ur_robot_model/ur5_gripper.srdf",
        "arm"}},
      {"fr3",
       {"franka_robot_model/fr3.urdf", "franka_robot_model/fr3.srdf",
        "fr3_arm"}},
      {"kinova",
       {"kinova_robot_model/kinova_robotiq.urdf",
        "kinova_robot_model/kinova_robotiq.srdf", "kinova_arm"}},
      {"so101",
       {"so101_robot_model/so101.urdf", "so101_robot_model/so101.srdf",
        "arm"}},
  };
  return configs;
}

bool supported_required_capability(const std::string& capability) {
  // Chunk 4 direct-engine compare mode is intentionally binary-collision only.
  // It does not yet expose stable body-pair identities, contact points,
  // penetration depth, distance, or max-contact capping. Required capabilities
  // outside this minimal surface must fail closed.
  return capability == "binary_collision";
}

}  // namespace

DirectCollisionQueryEngine::DirectCollisionQueryEngine(
    const std::string& model_name,
    const std::filesystem::path& urdf_path,
    const std::filesystem::path& srdf_path,
    const std::vector<std::filesystem::path>& package_paths,
    std::string group_name)
    : group_name_(std::move(group_name)),
      engine_id_("direct_roboplan_robot_only_compare"),
      engine_version_("0.x") {
  // Build a fully independent roboplan::Scene. Each engine instance
  // owns its own Pinocchio Model + Data + Coal collision objects, so
  // two instances can be queried concurrently without shared state and
  // without touching the RoboPlanPlanningContext's scene_mutex_.
  scene_ = std::make_shared<roboplan::Scene>(model_name, urdf_path, srdf_path,
                                              package_paths);
}

core::CollisionResult DirectCollisionQueryEngine::check(
    const core::CollisionQuery& query) {
  core::CollisionResult result;
  result.query_id = query.query_id;
  result.surface = query.surface;
  result.policy_revision = query.policy_revision;
  result.state_time_ns = query.state_time_ns;
  result.sampling_mode = query.sampling_mode;
  result.engine_id = engine_id_;
  result.engine_version = engine_version_;
  if (query.collision_world_revision.revision_id != 0 ||
      !query.collision_world_revision.content_hash.empty()) {
    result.collision_world_revision = query.collision_world_revision;
  }

  const auto t_start = std::chrono::steady_clock::now();
  auto finish_with_duration = [&]() {
    const auto t_end = std::chrono::steady_clock::now();
    result.query_duration_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(t_end - t_start)
            .count();
  };

  for (const auto& capability : query.required_capabilities) {
    if (!supported_required_capability(capability)) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kDiagnosticIncomplete;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }
  }
  if (query.timeout_budget_ns && *query.timeout_budget_ns <= 0) {
    finish_with_duration();
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }

  {
    try {
      std::lock_guard<std::mutex> scene_lock(scene_mutex_);

      Eigen::VectorXd q = to_eigen_(query.robot_state.positions_rad);
      if (q.size() != scene_->getCurrentJointPositions().size()) {
        q = scene_->toFullJointPositions(group_name_, q);
      }

      result.in_collision = scene_->hasCollisions(q);
      if (result.in_collision) {
        // Mirror the legacy collision_check shape: report the canonical
        // contact pair names if available. We do not normalize body ids
        // here — the planning context's mapper is the authoritative
        // normalization surface; this engine only reports raw outcomes
        // for compare-mode mismatch detection.
        result.contact_body_a.reset();
        result.contact_body_b.reset();
      }
    } catch (const std::exception&) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kEngineError;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    } catch (...) {
      finish_with_duration();
      result.engine_status = core::CollisionEngineStatus::kEngineError;
      result.decision = core::CollisionDecisionResult::kInconclusiveFail;
      return result;
    }
  }
  finish_with_duration();
  if (query.timeout_budget_ns &&
      result.query_duration_ns &&
      *result.query_duration_ns > *query.timeout_budget_ns) {
    result.engine_status = core::CollisionEngineStatus::kTimeout;
    result.decision = core::CollisionDecisionResult::kInconclusiveFail;
    return result;
  }
  result.engine_status = core::CollisionEngineStatus::kOk;
  result.decision = result.in_collision ? core::CollisionDecisionResult::kFail
                                         : core::CollisionDecisionResult::kPass;
  return result;
}

std::shared_ptr<DirectCollisionQueryEngine>
DirectCollisionQueryEngine::create_from_example_model(
    const std::string& model_name, const std::string& group_name) {
  const auto& configs = example_model_lookup();
  auto it = configs.find(model_name);
  if (it == configs.end()) {
    throw std::runtime_error(
        "DirectCollisionQueryEngine::create_from_example_model: unknown model: " +
        model_name);
  }
  const auto& config = it->second;
  const std::string actual_group =
      group_name.empty() ? config.default_group : group_name;

  const auto models_dir = roboplan::example_models::get_package_models_dir();
  const auto pkg_share = roboplan::example_models::get_package_share_dir();

  std::filesystem::path urdf = models_dir / config.urdf_rel;
  std::filesystem::path srdf = models_dir / config.srdf_rel;
  std::vector<std::filesystem::path> pkg_paths{pkg_share};
  return std::make_shared<DirectCollisionQueryEngine>(model_name, urdf, srdf,
                                                       pkg_paths, actual_group);
}

}  // namespace manyforge::roboplan_backend

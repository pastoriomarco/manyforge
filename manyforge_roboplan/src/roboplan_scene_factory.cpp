#include "manyforge_roboplan/roboplan_scene_factory.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>

#include <roboplan/core/scene.hpp>

namespace manyforge::roboplan_backend {
namespace {

[[noreturn]] void fail(const std::string &detail) {
  throw std::runtime_error("RoboPlan retained Scene input: " + detail);
}

void require_vector(const std::vector<double> &values, std::size_t size,
                    bool positive, const std::string &joint_name,
                    const char *field) {
  if (values.size() != size) {
    fail("joint '" + joint_name + "' has the wrong " + field +
         " dimension");
  }
  if (std::any_of(values.begin(), values.end(), [positive](double value) {
        return !std::isfinite(value) || (positive && value <= 0.0);
      })) {
    fail("joint '" + joint_name + "' has invalid " + field + " values");
  }
}

YAML::Node sequence(const std::vector<double> &values) {
  YAML::Node result(YAML::NodeType::Sequence);
  for (const double value : values) {
    result.push_back(value);
  }
  return result;
}

void require_applied(const roboplan::JointInfo &info,
                     const RoboPlanJointLimitInput &row) {
  const auto equal = [](const Eigen::VectorXd &actual,
                        const std::vector<double> &expected) {
    if (actual.size() != static_cast<Eigen::Index>(expected.size())) {
      return false;
    }
    for (Eigen::Index index = 0; index < actual.size(); ++index) {
      if (actual[index] != expected[static_cast<std::size_t>(index)]) {
        return false;
      }
    }
    return true;
  };
  if ((row.minimum_position.has_value() &&
       !equal(info.limits.min_position, *row.minimum_position)) ||
      (row.maximum_position.has_value() &&
       !equal(info.limits.max_position, *row.maximum_position)) ||
      !equal(info.limits.max_velocity, row.maximum_velocity) ||
      (row.maximum_acceleration.has_value() &&
       !equal(info.limits.max_acceleration, *row.maximum_acceleration))) {
    fail("joint '" + row.joint_name + "' limits were not applied exactly");
  }
}

void apply_scene_semantics(roboplan::Scene &scene,
                           const std::vector<RoboPlanJointLimitInput> &joint_limits,
                           const std::string &srdf_xml) {
  std::map<std::string, const RoboPlanJointLimitInput *> rows;
  YAML::Node config(YAML::NodeType::Map);
  YAML::Node limits(YAML::NodeType::Map);
  for (const auto &row : joint_limits) {
    if (row.joint_name.empty() || !rows.emplace(row.joint_name, &row).second) {
      fail("joint-limit names are empty or duplicated");
    }
    const auto info = scene.getJointInfo(row.joint_name);
    if (!info.has_value() || info->mimic_info.has_value()) {
      fail("joint-limit row names a foreign or mimic joint '" +
           row.joint_name + "'");
    }
    const auto dimension = info->num_velocity_dofs;
    require_vector(row.maximum_velocity, dimension, true, row.joint_name,
                   "maximum velocity");
    if (row.minimum_position.has_value() !=
        row.maximum_position.has_value()) {
      fail("joint '" + row.joint_name +
           "' has only one position-limit side");
    }
    if (row.minimum_position) {
      require_vector(*row.minimum_position, dimension, false, row.joint_name,
                     "minimum position");
      require_vector(*row.maximum_position, dimension, false, row.joint_name,
                     "maximum position");
      for (std::size_t index = 0; index < dimension; ++index) {
        if ((*row.minimum_position)[index] >
            (*row.maximum_position)[index]) {
          fail("joint '" + row.joint_name +
               "' has inverted position limits");
        }
      }
    }
    if (row.maximum_acceleration) {
      require_vector(*row.maximum_acceleration, dimension, true,
                     row.joint_name, "maximum acceleration");
    }

    YAML::Node values(YAML::NodeType::Map);
    if (row.minimum_position) {
      values["min_position"] = sequence(*row.minimum_position);
      values["max_position"] = sequence(*row.maximum_position);
    }
    values["max_velocity"] = sequence(row.maximum_velocity);
    if (row.maximum_acceleration) {
      values["max_acceleration"] = sequence(*row.maximum_acceleration);
    }
    limits[row.joint_name] = std::move(values);
  }

  for (const auto &joint_name : scene.getJointNames()) {
    const auto info = scene.getJointInfo(joint_name);
    if (!info.has_value()) {
      fail("constructed Scene lost joint '" + joint_name + "'");
    }
    const bool requires_explicit_row =
        info->type == roboplan::JointType::PRISMATIC ||
        info->type == roboplan::JointType::REVOLUTE ||
        info->type == roboplan::JointType::CONTINUOUS;
    if (!info->mimic_info.has_value() && requires_explicit_row &&
        !rows.contains(joint_name)) {
      fail("verified limit configuration omits joint '" + joint_name + "'");
    }
  }

  config["joint_limits"] = std::move(limits);
  scene.importJointLimitsFromConfig(config);
  for (const auto &[joint_name, row] : rows) {
    const auto info = scene.getJointInfo(joint_name);
    if (!info.has_value()) {
      fail("imported limit row disappeared for joint '" + joint_name + "'");
    }
    require_applied(*info, *row);
  }

  if (const auto imported = scene.importSrdf(srdf_xml);
      !imported.has_value()) {
    fail("SRDF import failed: " + imported.error());
  }
}

}  // namespace

std::shared_ptr<roboplan::Scene> make_roboplan_scene(
    RoboPlanSceneInput input) {
  if (input.scene_name.empty() || input.urdf_xml.empty() ||
      input.srdf_xml.empty()) {
    fail("scene name, URDF, and SRDF are required");
  }
  for (const auto &path : input.package_paths) {
    if (!path.is_absolute() || !std::filesystem::is_directory(path)) {
      fail("package path is not an absolute directory");
    }
  }

  auto scene = std::make_shared<roboplan::Scene>(
      input.scene_name, roboplan::loadUrdfSceneDescriptionFromXml(
                            input.urdf_xml, input.package_paths));

  apply_scene_semantics(*scene, input.joint_limits, input.srdf_xml);
  return scene;
}

std::shared_ptr<roboplan::Scene> make_collision_disabled_scene(
    const roboplan::Scene &source, const std::string &scene_name,
    const std::vector<RoboPlanJointLimitInput> &joint_limits,
    const std::string &srdf_xml) {
  roboplan::PinocchioSceneDescription description{source.getModel(),
                                                    source.getCollisionModel()};
  description.collision_model.removeAllCollisionPairs();
  auto scene = std::make_shared<roboplan::Scene>(scene_name, description);
  apply_scene_semantics(*scene, joint_limits, srdf_xml);
  scene->setJointPositions(source.getCurrentJointPositions());
  return scene;
}

}  // namespace manyforge::roboplan_backend

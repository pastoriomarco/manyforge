#include <manyforge_deployment/initial_scene_projection.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

namespace manyforge::deployment {
namespace {

using content_identity::CanonicalValue;

[[noreturn]] void Fail(std::string_view field) {
  throw ProgramBindingError("invalid bound initial Scene: " +
                            std::string(field));
}

const CanonicalValue &Member(const CanonicalValue &value,
                             std::string_view name) {
  if (!value.is_object())
    Fail("expected object");
  const auto *member = value.find_object_member(name);
  if (member == nullptr)
    Fail(name);
  return *member;
}

std::string Text(const CanonicalValue &value, std::string_view field,
                 std::size_t maximum_bytes = 512U) {
  if (!value.is_string() || value.as_string().empty() ||
      value.as_string().size() > maximum_bytes)
    Fail(field);
  return value.as_string();
}

double Number(const CanonicalValue &value, std::string_view field) {
  if (!value.is_number() || !std::isfinite(value.as_number()))
    Fail(field);
  return value.as_number();
}

bool Boolean(const CanonicalValue &value, std::string_view field) {
  if (!value.is_bool())
    Fail(field);
  return value.as_bool();
}

std::array<double, 3U> Triple(const CanonicalValue &value,
                              std::string_view field) {
  if (!value.is_array() || value.as_array().size() != 3U)
    Fail(field);
  return {Number(value.as_array()[0], field),
          Number(value.as_array()[1], field),
          Number(value.as_array()[2], field)};
}

core::Vector3DTO Vector(const CanonicalValue &value,
                        std::string_view field) {
  const auto xyz = Triple(value, field);
  return {xyz[0], xyz[1], xyz[2]};
}

core::QuaternionDTO Orientation(const CanonicalValue &value) {
  if (!value.is_object() || value.as_object().size() != 1U)
    Fail("pose.orientation");
  if (const auto *xyzw = value.find_object_member("xyzw")) {
    if (!xyzw->is_array() || xyzw->as_array().size() != 4U)
      Fail("pose.orientation.xyzw");
    std::array<double, 4U> q{};
    for (std::size_t i = 0U; i < q.size(); ++i)
      q[i] = Number(xyzw->as_array()[i], "pose.orientation.xyzw");
    const double norm = std::sqrt(q[0] * q[0] + q[1] * q[1] +
                                  q[2] * q[2] + q[3] * q[3]);
    if (!std::isfinite(norm) || norm < 1e-12)
      Fail("pose.orientation.xyzw");
    return {q[0] / norm, q[1] / norm, q[2] / norm, q[3] / norm};
  }
  const auto rpy = Triple(Member(value, "rpy_deg"), "pose.orientation.rpy_deg");
  const double scale = std::numbers::pi / 360.0;
  const double cr = std::cos(rpy[0] * scale), sr = std::sin(rpy[0] * scale);
  const double cp = std::cos(rpy[1] * scale), sp = std::sin(rpy[1] * scale);
  const double cy = std::cos(rpy[2] * scale), sy = std::sin(rpy[2] * scale);
  return {sr * cp * cy - cr * sp * sy,
          cr * sp * cy + sr * cp * sy,
          cr * cp * sy - sr * sp * cy,
          cr * cp * cy + sr * sp * sy};
}

core::CollisionShapeDTO Shape(const CanonicalValue &value) {
  const auto kind = Text(Member(value, "type"), "shape.type");
  if (kind == "box" && value.as_object().size() == 2U) {
    return core::BoxShapeDTO{Vector(Member(value, "box_dimensions_m"),
                                    "shape.box_dimensions_m")};
  }
  if (kind == "sphere" && value.as_object().size() == 2U) {
    return core::SphereShapeDTO{
        Number(Member(value, "sphere_radius_m"), "shape.sphere_radius_m")};
  }
  if (kind == "cylinder" && value.as_object().size() == 3U) {
    return core::CylinderShapeDTO{
        Number(Member(value, "cylinder_radius_m"), "shape.cylinder_radius_m"),
        Number(Member(value, "cylinder_height_m"), "shape.cylinder_height_m")};
  }
  if (kind == "mesh" && value.as_object().size() == 3U) {
    return core::MeshShapeDTO{
        Text(Member(value, "mesh_resource_uri"), "shape.mesh_resource_uri",
             4096U),
        Vector(Member(value, "mesh_scale"), "shape.mesh_scale")};
  }
  Fail("shape.type");
}

core::SceneTransactionFrameRefDTO Frame(
    const CanonicalValue &value,
    const std::vector<core::ProgramFrameBindingDTO> &frames,
    const core::CellId &cell_id) {
  const auto kind = Text(Member(value, "kind"), "pose.frame_ref.kind");
  if (kind == "cell_frame" && value.as_object().size() == 1U)
    return core::CellSceneFrameRefDTO{cell_id};
  if (kind != "component_frame" || value.as_object().size() != 2U)
    Fail("pose.frame_ref.kind");
  const auto &reference = Member(value, "frame");
  if (!reference.is_object() || reference.as_object().size() != 2U)
    Fail("pose.frame_ref.frame");
  const auto component = Text(Member(reference, "component_id"), "pose.frame_ref.frame.component_id");
  const auto frame_id = Text(Member(reference, "frame_id"), "pose.frame_ref.frame.frame_id");
  const core::ProgramFrameBindingDTO *match = nullptr;
  for (const auto &row : frames) {
    if (row.reference.component_id.value() != component ||
        row.reference.frame_id.value() != frame_id)
      continue;
    if (match != nullptr)
      Fail("pose.frame_ref.frame is ambiguous");
    match = &row;
  }
  if (match == nullptr)
    Fail("pose.frame_ref.frame is missing");
  return core::ComponentSceneFrameRefDTO{
      match->reference.component_id, match->model_instance_id,
      match->reference.frame_id};
}

std::vector<core::SupportContactDTO>
SupportContacts(const CanonicalValue &row) {
  const auto *metadata = row.find_object_member("metadata");
  if (metadata == nullptr || metadata->is_null())
    return {};
  if (!metadata->is_object())
    Fail("metadata");
  const auto *contacts = metadata->find_object_member("support_contacts");
  if (contacts == nullptr)
    return {};
  if (!contacts->is_array() || contacts->as_array().size() > 4096U)
    Fail("metadata.support_contacts");
  std::vector<core::SupportContactDTO> result;
  result.reserve(contacts->as_array().size());
  for (const auto &value : contacts->as_array()) {
    core::SupportContactDTO contact;
    contact.support_object_id =
        Text(Member(value, "support_object_id"), "support_object_id");
    const auto optional = [&](std::string_view name) {
      return value.find_object_member(name);
    };
    if (const auto *mode = optional("mode")) {
      const auto text = Text(*mode, "support.mode");
      if (text == "contact_release")
        contact.mode = core::CollisionContactPolicyMode::kContactRelease;
      else if (text == "bounded_penetration")
        contact.mode = core::CollisionContactPolicyMode::kBoundedPenetration;
      else
        Fail("support.mode");
    }
    if (const auto *evidence = optional("release_evidence_mode")) {
      const auto text = Text(*evidence, "support.release_evidence_mode");
      if (text == "signed_penetration")
        contact.release_evidence_mode =
            core::ContactReleaseEvidenceMode::kSignedPenetration;
      else if (text == "binary_contact")
        contact.release_evidence_mode =
            core::ContactReleaseEvidenceMode::kBinaryContact;
      else
        Fail("support.release_evidence_mode");
    }
    if (const auto *field = optional("enforce_max_start_penetration"))
      contact.enforce_max_start_penetration = Boolean(*field, "support.enforce");
    if (const auto *field = optional("max_start_penetration_m"))
      contact.max_start_penetration_m = Number(*field, "support.max_start");
    if (const auto *field = optional("release_tolerance_m"))
      contact.release_tolerance_m = Number(*field, "support.release_tolerance");
    if (const auto *field = optional("min_release_progress_m"))
      contact.min_release_progress_m = Number(*field, "support.min_progress");
    if (const auto *field = optional("max_contact_penetration_m"))
      contact.max_contact_penetration_m = Number(*field, "support.max_contact");
    if (const auto *field = optional("require_separation_by_end"))
      contact.require_separation_by_end = Boolean(*field, "support.separation");
    if (const auto *field = optional("require_strictly_decreasing_penetration"))
      contact.require_strictly_decreasing_penetration =
          Boolean(*field, "support.decreasing");
    if (const auto *field = optional("allow_recontact"))
      contact.allow_recontact = Boolean(*field, "support.recontact");
    result.push_back(std::move(contact));
  }
  return result;
}

double MassKg(const CanonicalValue &row) {
  const auto *metadata = row.find_object_member("metadata");
  if (metadata == nullptr || metadata->is_null())
    return 0.0;
  if (!metadata->is_object())
    Fail("metadata");
  const auto *mass = metadata->find_object_member("mass_kg");
  if (mass == nullptr)
    return 0.0;
  const auto value = Number(*mass, "metadata.mass_kg");
  if (value < 0.0 || value > 1000.0)
    Fail("metadata.mass_kg");
  return value;
}

} // namespace

namespace detail {
std::vector<core::AddObjectRequestDTO>
project_initial_scene_references_value(
    const CanonicalValue &normalized_program,
    const std::vector<core::ProgramFrameBindingDTO> &frames,
    const core::CellId &cell_id) {
  if (cell_id.empty())
    Fail("cell identity");
  const auto &scene = Member(normalized_program, "scene");
  if (!scene.is_array() || scene.as_array().size() > 4096U)
    Fail("scene");
  std::vector<core::AddObjectRequestDTO> result;
  result.reserve(scene.as_array().size());
  std::string previous_id;
  for (const auto &row : scene.as_array()) {
    const auto id = Text(Member(row, "scene_resource_id"),
                         "scene_resource_id");
    if (!previous_id.empty() && id <= previous_id)
      Fail("scene ordering");
    previous_id = id;
    const auto &pose = Member(row, "pose");
    result.push_back(core::AddObjectRequestDTO{
        core::SceneResourceId(id), Shape(Member(row, "shape")),
        core::Pose3DTO{Vector(Member(pose, "position_m"), "pose.position_m"),
                       Orientation(Member(pose, "orientation"))},
        Frame(Member(pose, "frame_ref"), frames, cell_id),
        SupportContacts(row), MassKg(row)});
  }
  return result;
}

std::vector<core::AddObjectRequestDTO>
project_bound_initial_scene_value(const CanonicalValue &normalized_program,
                                  const core::ProgramBindingDTO &binding,
                                  const core::CellId &cell_id) {
  return project_initial_scene_references_value(
      normalized_program, binding.frame_bindings, cell_id);
}
} // namespace detail

std::vector<core::AddObjectRequestDTO>
project_bound_initial_scene(const ProgramBindingContract &program,
                            const ProgramBinding &binding,
                            const core::CellId &cell_id) {
  if (binding.value().program_id != program.value().program_id ||
      binding.value().program_revision != program.value().program_revision ||
      binding.value().retained_bundle_id.empty())
    Fail("Program/Binding identity");
  return detail::project_bound_initial_scene_value(
      program.normalized_program_root(), binding.value(), cell_id);
}

std::vector<core::AddObjectRequestDTO>
project_resolved_initial_scene(const ProgramBindingContract &program,
                               const ProgramResolvedReferencesDTO &resolved_references,
                               const core::CellId &cell_id) {
  // The partial reference resolution is a Scene placement view, never a fabricated
  // Program binding that could be mistaken for execution authority.
  const auto &scene = Member(program.normalized_program_root(), "scene");
  if (!scene.is_array())
    Fail("scene");
  for (const auto &row : scene.as_array()) {
    const auto &frame = Member(Member(row, "pose"), "frame_ref");
    if (Text(Member(frame, "kind"), "pose.frame_ref.kind") == "cell_frame")
      continue;
    try {
      static_cast<void>(Frame(frame, resolved_references.frame_bindings, cell_id));
    } catch (const ProgramBindingError &) {
      throw ProgramBindingError(
          "Program Scene object '" +
          Text(Member(row, "scene_resource_id"), "scene_resource_id") +
          "' is placed on a component frame that does not resolve in this "
          "Deployment, so it cannot be placed while the Program is "
          "non-executable");
    }
  }
  return detail::project_initial_scene_references_value(
      program.normalized_program_root(), resolved_references.frame_bindings, cell_id);
}

} // namespace manyforge::deployment

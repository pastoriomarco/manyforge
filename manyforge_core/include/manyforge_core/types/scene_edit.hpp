#pragma once

#include <string>
#include <variant>
#include <vector>

#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/geometry.hpp>

namespace manyforge::core {

/// Box shape
struct BoxShapeDTO {
  Vector3DTO dimensions_m;  // full dimensions, matching URDF/ROS conventions

  friend bool operator==(const BoxShapeDTO&, const BoxShapeDTO&) = default;
};

/// Sphere shape
struct SphereShapeDTO {
  double radius_m;  // > 0

  friend bool operator==(const SphereShapeDTO&, const SphereShapeDTO&) =
      default;
};

/// Cylinder shape
struct CylinderShapeDTO {
  double radius_m;   // > 0
  double height_m;   // > 0

  friend bool operator==(const CylinderShapeDTO&, const CylinderShapeDTO&) =
      default;
};

/// Mesh shape
struct MeshShapeDTO {
  std::string resource_uri;
  Vector3DTO scale;  // default {1,1,1}

  friend bool operator==(const MeshShapeDTO&, const MeshShapeDTO&) = default;
};

/// Collision shape as a tagged union
using CollisionShapeDTO = std::variant<BoxShapeDTO, SphereShapeDTO, CylinderShapeDTO, MeshShapeDTO>;

/// Declared support contact for an authored scene object.
///
/// This is a semantic ManyForge policy, not a backend collision-matrix knob:
/// kContactRelease governs separation-only motion, while kBoundedPenetration
/// permits ongoing/reappearing contact whose colliding samples all stay within
/// max_contact_penetration_m.
struct SupportContactDTO {
  std::string support_object_id;
  CollisionContactPolicyMode mode =
      CollisionContactPolicyMode::kContactRelease;
  ContactReleaseEvidenceMode release_evidence_mode =
      ContactReleaseEvidenceMode::kSignedPenetration;
  bool enforce_max_start_penetration = true;
  double max_start_penetration_m = 0.0;
  double release_tolerance_m = 1e-5;
  double min_release_progress_m = 0.0;
  double max_contact_penetration_m = 0.0;
  bool require_separation_by_end = true;
  bool require_strictly_decreasing_penetration = true;
  bool allow_recontact = false;

  friend bool operator==(const SupportContactDTO&,
                         const SupportContactDTO&) = default;
};

}  // namespace manyforge::core

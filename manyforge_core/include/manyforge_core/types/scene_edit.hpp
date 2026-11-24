#pragma once

#include <string>
#include <variant>
#include <vector>

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
  /// `sha256:<64 hex>` of the mesh bytes captured into the activation
  /// raw-artifact store. Consumers read the bytes by this digest only.
  std::string content_digest;

  friend bool operator==(const MeshShapeDTO&, const MeshShapeDTO&) = default;
};

/// Collision shape as a tagged union
using CollisionShapeDTO = std::variant<BoxShapeDTO, SphereShapeDTO, CylinderShapeDTO, MeshShapeDTO>;

}  // namespace manyforge::core

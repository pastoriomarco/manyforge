#pragma once

#include <cstdint>
#include <string>

namespace manyforge::core {

/// Severity of a collision event.
enum class CollisionSeverity {
  kWarning,   // Advisory, no automatic stop
  kSoftStop,  // Decelerate to stop
  kHardStop,  // Immediate stop
};

/// A collision event detected during pre-execution check or execution.
/// ADR-0009: collision monitoring is first-class and policy-driven.
struct CollisionEvent {
  /// Which objects are involved (link name or object id).
  std::string object_a;
  std::string object_b;

  /// Estimated penetration depth (meters), 0 if unknown.
  double penetration_depth = 0.0;

  /// Severity determines the response policy.
  CollisionSeverity severity = CollisionSeverity::kHardStop;

  /// Timestamp in nanoseconds (from IClock).
  int64_t timestamp_ns = 0;

  /// Scene version at detection time.
  uint64_t scene_version = 0;
};

}  // namespace manyforge::core

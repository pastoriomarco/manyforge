#pragma once

// Frames of the Manager's status stream (OD-216) for tests that feed the
// Headless API's mirror directly, as the stream's follower does.

#include <manyforge_content_identity/canonical_value.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace manyforge::headless::test {

inline constexpr std::string_view kStatusFrameStart = "0123456789abcdef";

inline content_identity::CanonicalValue
StatusEnvelope(const std::string_view type, const std::uint64_t tick,
               content_identity::CanonicalValue::Object members = {}) {
  using content_identity::CanonicalValue;
  members.emplace_back("api_version",
                       CanonicalValue("manyforge.manager_status_stream.v2"));
  members.emplace_back("application_start_id",
                       CanonicalValue(std::string(kStatusFrameStart)));
  members.emplace_back("tick", CanonicalValue(static_cast<double>(tick)));
  members.emplace_back("type", CanonicalValue(std::string(type)));
  return CanonicalValue(std::move(members));
}

/// The first frame of a subscription (generations are informational).
inline std::string StatusSnapshotFrame(const std::uint64_t tick,
                                       const std::uint64_t manager = 0U,
                                       const std::uint64_t logical_scene = 0U,
                                       const std::uint64_t scene_runtime = 0U,
                                       const std::uint64_t viewer_pose_sessions = 0U,
                                       const std::uint64_t scene_changes = 0U) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(StatusEnvelope(
      "snapshot", tick,
      {{"generations",
        CanonicalValue(CanonicalValue::Object{
            {"logical_scene", CanonicalValue(static_cast<double>(logical_scene))},
            {"manager", CanonicalValue(static_cast<double>(manager))},
            {"scene_changes", CanonicalValue(static_cast<double>(scene_changes))},
            {"scene_runtime", CanonicalValue(static_cast<double>(scene_runtime))},
            {"viewer_pose_sessions",
             CanonicalValue(static_cast<double>(viewer_pose_sessions))},
        })}}));
}

/// One whole section value; `name` is manager, logical_scene, scene_runtime,
/// viewer_pose_sessions or scene_changes.
inline std::string StatusSectionFrame(const std::string_view name,
                                      const std::uint64_t generation,
                                      const content_identity::CanonicalValue &payload,
                                      const std::uint64_t tick,
                                      const std::int64_t observed_at_ns = 1) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(StatusEnvelope(
      "section", tick,
      {{"name", CanonicalValue(std::string(name))},
       {"section",
        CanonicalValue(CanonicalValue::Object{
            {"generation", CanonicalValue(static_cast<double>(generation))},
            {"observed_at_ns",
             content_identity::nonnegative_int64_value(observed_at_ns)},
            {"oversized", CanonicalValue(false)},
            {"payload", payload},
        })}}));
}

/// A section the Manager could not fit in one frame.
inline std::string StatusOversizedSectionFrame(const std::string_view name,
                                               const std::uint64_t generation,
                                               const std::uint64_t tick) {
  using content_identity::CanonicalValue;
  return content_identity::canonical_json(StatusEnvelope(
      "section", tick,
      {{"name", CanonicalValue(std::string(name))},
       {"section", CanonicalValue(CanonicalValue::Object{
                       {"generation", CanonicalValue(static_cast<double>(generation))},
                       {"observed_at_ns", CanonicalValue(1.0)},
                       {"oversized", CanonicalValue(true)},
                       {"payload", CanonicalValue(nullptr)},
                   })}}));
}

inline std::string StatusKeepaliveFrame(const std::uint64_t tick) {
  return content_identity::canonical_json(StatusEnvelope("keepalive", tick));
}

} // namespace manyforge::headless::test

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

#include "manager_status_frames.hpp"
#include "manyforge_headless/runtime_snapshot.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_contract.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;
using test::StatusEnvelope;
using test::StatusKeepaliveFrame;
using test::StatusOversizedSectionFrame;
using test::StatusSectionFrame;
using test::StatusSnapshotFrame;
using test::kStatusFrameStart;

constexpr std::int64_t kSilenceNs = 2'000'000'000LL;
const std::string kCellId = "cell-a";

CanonicalValue ManagerPayload(const std::string &cell_id,
                              const std::string &graph_revision) {
  return CanonicalValue(CanonicalValue::Object{
      {"active",
       CanonicalValue(CanonicalValue::Object{
           {"canonical_empty", CanonicalValue(false)},
           {"cell_graph",
            CanonicalValue(CanonicalValue::Object{
                {"api_version", CanonicalValue("manyforge.cell_graph.v1")},
                {"cell_id", CanonicalValue(cell_id)},
            })},
           {"graph_revision", CanonicalValue(graph_revision)},
           {"publication", CanonicalValue(CanonicalValue::Object{})},
           {"retained_bundle_id", CanonicalValue("bundle-1")},
       })},
      {"api_version",
       CanonicalValue(std::string(kManagerSourcePayloadApiVersion))},
      {"apply", CanonicalValue(CanonicalValue::Object{})},
      {"cell_id", CanonicalValue(cell_id)},
      {"manager_ready", CanonicalValue(true)},
      {"runtime", CanonicalValue(CanonicalValue::Object{
                      {"converged", CanonicalValue(true)},
                  })},
  });
}

CanonicalValue LogicalScenePayload(const std::string &cell_id) {
  return CanonicalValue(CanonicalValue::Object{
      {"access", CanonicalValue(CanonicalValue::Object{})},
      {"api_version",
       CanonicalValue(std::string(kLogicalSceneSourcePayloadApiVersion))},
      {"snapshot", CanonicalValue(CanonicalValue::Object{
                       {"cell_id", CanonicalValue(cell_id)},
                       {"objects", CanonicalValue(CanonicalValue::Array{})},
                   })},
  });
}

CanonicalValue SceneRuntimePayload(const std::string &cell_id) {
  return CanonicalValue(CanonicalValue::Object{
      {"api_version",
       CanonicalValue(std::string(kSceneRuntimeSourcePayloadApiVersion))},
      {"cell_id", CanonicalValue(cell_id)},
      {"state", SceneRuntimeStateValue(SceneRuntimeStateSnapshot{
                    1U,
                    std::nullopt,
                    SceneInitializationState::kOpen,
                    false,
                    SceneRuntimeIdentity{"bundle-1", "graph-1", 1U, 0U, 1U},
                    "bootstrap",
                    std::nullopt,
                    0,
                    0,
                    std::nullopt,
                    {},
                    {},
                })},
  });
}

CanonicalValue
SceneRuntimePayloadWithJointStates(const std::string &cell_id,
                                   CanonicalValue::Array rows) {
  auto payload = SceneRuntimePayload(cell_id).as_object();
  payload.emplace_back("configured_joint_states",
                       CanonicalValue(std::move(rows)));
  return CanonicalValue(std::move(payload));
}

CanonicalValue::Object JointStateRow() {
  return CanonicalValue::Object{
      {"object_id", CanonicalValue("robot.1")},
      {"policy_id", CanonicalValue("viewer.measured_robot")},
      {"source_id", CanonicalValue("isaac_scene_objects")},
      {"topic", CanonicalValue("/isaac_joint_states")},
  };
}

CanonicalValue ViewerPoseSessionsPayload(CanonicalValue::Array sessions = {}) {
  return CanonicalValue(CanonicalValue::Object{
      {"runtime_identity", CanonicalValue(nullptr)},
      {"sessions", CanonicalValue(std::move(sessions))},
  });
}

/// `object` with member `name` replaced (or added).
CanonicalValue WithMember(const CanonicalValue &object,
                          const std::string_view name, CanonicalValue value) {
  auto members = object.as_object();
  bool replaced = false;
  for (auto &[key, field] : members) {
    if (key == name) {
      field = value;
      replaced = true;
    }
  }
  if (!replaced) {
    members.emplace_back(std::string(name), std::move(value));
  }
  return CanonicalValue(std::move(members));
}

/// A scene-runtime payload whose `state.<name>` is `value`.
CanonicalValue SceneRuntimeWithState(const std::string_view name,
                                     CanonicalValue value) {
  const auto payload = SceneRuntimePayload(kCellId);
  return WithMember(payload, "state",
                    WithMember(*payload.find_object_member("state"), name,
                               std::move(value)));
}

/// A Manager payload whose runtime reports one observed unit.
CanonicalValue ManagerWithObservedUnit(const std::int64_t process_sequence,
                                       const std::int64_t report_sequence) {
  return WithMember(
      ManagerPayload(kCellId, "graph-1"), "runtime",
      CanonicalValue(CanonicalValue::Object{
          {"converged", CanonicalValue(true)},
          {"observed_units",
           CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
               {"process_observation_sequence",
                content_identity::nonnegative_int64_value(process_sequence)},
               {"source_report_sequence",
                content_identity::nonnegative_int64_value(report_sequence)},
               {"unit_id", CanonicalValue("unit-1")},
           })})},
      }));
}

bool RequiredBool(const CanonicalValue &root, const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("missing test field " + std::string(name));
  }
  const auto *value = std::get_if<bool>(&member->storage());
  if (value == nullptr) {
    throw std::runtime_error("non-boolean test field " + std::string(name));
  }
  return *value;
}

const CanonicalValue &RequiredMember(const CanonicalValue &root,
                                     const std::string_view name) {
  const auto *member = root.find_object_member(name);
  if (member == nullptr) {
    throw std::runtime_error("missing test field " + std::string(name));
  }
  return *member;
}

const CanonicalValue &SectionOf(const HeadlessRuntimeSnapshot &snapshot,
                                const std::string_view name) {
  return RequiredMember(RequiredMember(snapshot.root, "sections"), name);
}

std::string Freshness(const CanonicalValue &section) {
  return RequiredMember(section, "freshness").as_string();
}

CanonicalValue Strings(const std::vector<std::string> &values) {
  CanonicalValue::Array result;
  for (const auto &value : values) {
    result.emplace_back(CanonicalValue(value));
  }
  return CanonicalValue(std::move(result));
}

CanonicalValue Count(const std::int64_t value) {
  return content_identity::nonnegative_int64_value(value);
}

const std::vector<std::string> kSectionNames{"manager", "logical_scene",
                                             "scene_runtime"};

/// One mirror with an injected monotonic clock and its aggregator.
struct Harness final {
  std::int64_t now{1'000};
  std::int64_t projection_now{100};
  ManagerStatusMirror mirror{kCellId, kSilenceNs, [this] { return now; }};
  RuntimeSnapshotAggregator aggregator{mirror, "headless-api.cell-a",
                                       "api-epoch-1",
                                       [this] { return projection_now; }};

  ManagerStatusMirror::ApplyResult Apply(const std::string &frame) {
    return mirror.Apply(frame);
  }
  HeadlessRuntimeSnapshot Evaluate() { return aggregator.Evaluate(); }

  /// A new subscription followed by the three control sections.
  void ConnectFullState(const std::uint64_t tick,
                        const std::uint64_t generation = 1U) {
    static_cast<void>(Apply(
        StatusSnapshotFrame(tick, generation, generation, generation, 0U)));
    static_cast<void>(Apply(StatusSectionFrame(
        "manager", generation, ManagerPayload(kCellId, "graph-1"), tick, 10)));
    static_cast<void>(Apply(StatusSectionFrame(
        "logical_scene", generation, LogicalScenePayload(kCellId), tick, 20)));
    static_cast<void>(Apply(StatusSectionFrame(
        "scene_runtime", generation, SceneRuntimePayload(kCellId), tick, 30)));
  }
};

std::string StreamErrorCode(ManagerStatusMirror &mirror,
                            const std::string_view frame) {
  try {
    static_cast<void>(mirror.Apply(frame));
  } catch (const ManagerStatusStreamError &error) {
    return error.code();
  }
  return "no error";
}

TEST(RuntimeSnapshotAggregator, FullStateIsCompleteFreshWithFixedOwnerRoles) {
  Harness h;
  const auto before = h.Evaluate();
  EXPECT_FALSE(RequiredBool(before.root, "complete"));
  for (const auto &name : kSectionNames) {
    const auto &section = SectionOf(before, name);
    EXPECT_FALSE(RequiredBool(section, "available"));
    EXPECT_EQ(Freshness(section), "unavailable");
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({name + "_source_absent"}));
    EXPECT_TRUE(RequiredMember(section, "receiver_age_ns").is_null());
  }

  h.ConnectFullState(1U);
  h.now = 1'500;
  const auto snapshot = h.Evaluate();
  EXPECT_TRUE(RequiredBool(snapshot.root, "complete"));
  EXPECT_EQ(RequiredMember(snapshot.root, "reasons"), Strings({}));
  EXPECT_EQ(RequiredMember(snapshot.root, "unavailable_sections"), Strings({}));
  EXPECT_EQ(RequiredMember(snapshot.root, "active_retained_bundle_id"),
            CanonicalValue("bundle-1"));
  EXPECT_EQ(RequiredMember(snapshot.root, "active_graph_revision"),
            CanonicalValue("graph-1"));
  struct Expected final {
    std::string name;
    std::string owner_kind;
    std::string owner_id;
    std::int64_t observed_at_ns;
    CanonicalValue payload;
  };
  const std::vector<Expected> expected{
      {"manager", "component_manager", "manager", 10,
       ManagerPayload(kCellId, "graph-1")},
      {"logical_scene", "logical_scene", "logical_scene", 20,
       LogicalScenePayload(kCellId)},
      {"scene_runtime", "scene_runtime", "scene_runtime", 30,
       SceneRuntimePayload(kCellId)},
  };
  for (const auto &entry : expected) {
    const auto &section = SectionOf(snapshot, entry.name);
    EXPECT_TRUE(RequiredBool(section, "available"));
    EXPECT_EQ(Freshness(section), "fresh");
    EXPECT_EQ(RequiredMember(section, "known_gaps"), Strings({}));
    EXPECT_EQ(RequiredMember(section, "source_owner_kind"),
              CanonicalValue(entry.owner_kind));
    EXPECT_EQ(RequiredMember(section, "source_owner_id"),
              CanonicalValue(entry.owner_id));
    EXPECT_EQ(RequiredMember(section, "source_generation"), CanonicalValue(1.0));
    EXPECT_EQ(RequiredMember(section, "source_observed_at_ns"),
              Count(entry.observed_at_ns));
    EXPECT_EQ(RequiredMember(section, "payload"), entry.payload);
    EXPECT_EQ(RequiredMember(section, "receiver_age_ns"), Count(500));
  }

  // The age counts from the last frame that advanced the tick.
  h.now = 1'700;
  static_cast<void>(h.Apply(StatusKeepaliveFrame(2U)));
  h.now = 1'800;
  const auto later = h.Evaluate();
  EXPECT_TRUE(RequiredBool(later.root, "complete"));
  for (const auto &name : kSectionNames) {
    EXPECT_EQ(RequiredMember(SectionOf(later, name), "receiver_age_ns"),
              Count(100));
  }
}

TEST(RuntimeSnapshotAggregator,
     DisconnectMakesEverySectionStaleAtOnceAndKeepsLastValues) {
  Harness h;
  EXPECT_FALSE(h.mirror.MarkDisconnected());
  h.ConnectFullState(1U);
  const auto known = h.Evaluate();
  ASSERT_TRUE(RequiredBool(known.root, "complete"));

  EXPECT_TRUE(h.mirror.MarkDisconnected());
  EXPECT_FALSE(h.mirror.MarkDisconnected());
  const auto lost = h.Evaluate();
  EXPECT_FALSE(RequiredBool(lost.root, "complete"));
  EXPECT_EQ(RequiredMember(lost.root, "reasons"),
            Strings({"manager_status_unavailable"}));
  EXPECT_EQ(RequiredMember(lost.root, "unavailable_sections"),
            Strings(kSectionNames));
  EXPECT_EQ(RequiredMember(lost.root, "active_graph_revision"),
            CanonicalValue("graph-1"));
  for (const auto &name : kSectionNames) {
    const auto &section = SectionOf(lost, name);
    const auto &before = SectionOf(known, name);
    EXPECT_EQ(Freshness(section), "stale");
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({"manager_status_unavailable"}));
    EXPECT_TRUE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "payload"),
              RequiredMember(before, "payload"));
    EXPECT_EQ(RequiredMember(section, "source_content_digest"),
              RequiredMember(before, "source_content_digest"));
    EXPECT_EQ(RequiredMember(section, "source_generation"),
              RequiredMember(before, "source_generation"));
  }
  EXPECT_GT(lost.projection_generation, known.projection_generation);
}

TEST(RuntimeSnapshotAggregator, SilenceMakesSectionsStaleUntilTheTickAdvances) {
  Harness h;
  h.ConnectFullState(1U);
  h.now = 1'000 + kSilenceNs;
  EXPECT_TRUE(RequiredBool(h.Evaluate().root, "complete"));

  h.now = 1'000 + kSilenceNs + 1;
  const auto silent = h.Evaluate();
  EXPECT_FALSE(RequiredBool(silent.root, "complete"));
  EXPECT_EQ(RequiredMember(silent.root, "reasons"),
            Strings({"manager_status_unavailable"}));
  for (const auto &name : kSectionNames) {
    EXPECT_EQ(Freshness(SectionOf(silent, name)), "stale");
    EXPECT_TRUE(RequiredBool(SectionOf(silent, name), "available"));
  }

  // A section frame of the same tick does not prove the loop ran.
  const auto same_tick = h.Apply(StatusSectionFrame(
      "manager", 2U, ManagerPayload(kCellId, "graph-1"), 1U, 40));
  EXPECT_FALSE(same_tick.tick_advanced);
  const auto still_silent = h.Evaluate();
  EXPECT_FALSE(RequiredBool(still_silent.root, "complete"));
  EXPECT_EQ(RequiredMember(SectionOf(still_silent, "manager"), "known_gaps"),
            Strings({"manager_status_unavailable"}));

  // A keepalive with a higher tick: answering again, with no reconnect.
  const auto keepalive = h.Apply(StatusKeepaliveFrame(2U));
  EXPECT_TRUE(keepalive.tick_advanced);
  const auto answering = h.Evaluate();
  EXPECT_TRUE(RequiredBool(answering.root, "complete"));
  EXPECT_EQ(RequiredMember(SectionOf(answering, "manager"), "source_generation"),
            CanonicalValue(2.0));
  for (const auto &name : kSectionNames) {
    EXPECT_EQ(Freshness(SectionOf(answering, name)), "fresh");
    EXPECT_EQ(RequiredMember(SectionOf(answering, name), "receiver_age_ns"),
              Count(0));
  }
  EXPECT_FALSE(h.Apply(StatusKeepaliveFrame(2U)).tick_advanced);
}

TEST(RuntimeSnapshotAggregator,
     ReconnectShowsSectionsAbsentUntilTheirFramesArrive) {
  Harness h;
  h.ConnectFullState(3U);
  const auto known = h.Evaluate();
  ASSERT_TRUE(RequiredBool(known.root, "complete"));
  ASSERT_TRUE(h.mirror.MarkDisconnected());

  // A resubscription within the silence bound (its tick does not count).
  h.now = 5'000;
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U, 2U, 2U, 2U, 0U)));
  const auto waiting = h.Evaluate();
  EXPECT_FALSE(RequiredBool(waiting.root, "complete"));
  EXPECT_EQ(RequiredMember(waiting.root, "reasons"),
            Strings({"manager_source_absent", "logical_scene_source_absent",
                     "scene_runtime_source_absent"}));
  for (const auto &name : kSectionNames) {
    const auto &section = SectionOf(waiting, name);
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({name + "_source_absent"}));
    EXPECT_TRUE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "source_generation"), CanonicalValue(1.0));
    EXPECT_EQ(RequiredMember(section, "payload"),
              RequiredMember(SectionOf(known, name), "payload"));
  }

  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 2U, ManagerPayload(kCellId, "graph-2"), 1U, 50)));
  const auto partial = h.Evaluate();
  EXPECT_FALSE(RequiredBool(partial.root, "complete"));
  EXPECT_EQ(Freshness(SectionOf(partial, "manager")), "fresh");
  EXPECT_EQ(RequiredMember(partial.root, "active_graph_revision"),
            CanonicalValue("graph-2"));
  EXPECT_EQ(RequiredMember(SectionOf(partial, "logical_scene"), "known_gaps"),
            Strings({"logical_scene_source_absent"}));

  static_cast<void>(h.Apply(StatusSectionFrame(
      "logical_scene", 2U, LogicalScenePayload(kCellId), 1U, 60)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", 2U, SceneRuntimePayload(kCellId), 1U, 70)));
  const auto complete = h.Evaluate();
  EXPECT_TRUE(RequiredBool(complete.root, "complete"));
  for (const auto &name : kSectionNames) {
    EXPECT_EQ(Freshness(SectionOf(complete, name)), "fresh");
    EXPECT_EQ(RequiredMember(SectionOf(complete, name), "source_generation"),
              CanonicalValue(2.0));
  }
  EXPECT_GT(complete.projection_generation, partial.projection_generation);
}

TEST(ManagerStatusMirror, ReconnectSnapshotCountsOnlyANewTickOrANewStart) {
  // OD-284 (§5.3): the snapshot comes from the stream's own threads, so a
  // reconnect to a Manager whose loop is held must not look alive.
  Harness h;
  h.ConnectFullState(3U);
  ASSERT_TRUE(h.mirror.MarkDisconnected());
  h.now = 1'000 + kSilenceNs + 1;
  EXPECT_FALSE(h.Apply(StatusSnapshotFrame(3U)).tick_advanced);
  EXPECT_EQ(RequiredMember(h.Evaluate().root, "reasons"),
            Strings({"manager_status_unavailable"}));
  ASSERT_TRUE(h.mirror.MarkDisconnected());
  EXPECT_TRUE(h.Apply(StatusSnapshotFrame(4U)).tick_advanced);

  // Another application start counts, whatever its tick.
  ASSERT_TRUE(h.mirror.MarkDisconnected());
  h.now += kSilenceNs + 1;
  auto restarted = StatusSnapshotFrame(1U);
  const auto at = restarted.find(kStatusFrameStart);
  ASSERT_NE(at, std::string::npos);
  restarted.replace(at, kStatusFrameStart.size(), "fedcba9876543210");
  EXPECT_TRUE(h.Apply(restarted).tick_advanced);
}

TEST(RuntimeSnapshotAggregator, OversizedSectionKeepsItsLastValueOthersFresh) {
  Harness h;
  h.ConnectFullState(1U);
  const auto known = h.Evaluate();
  static_cast<void>(h.Apply(StatusOversizedSectionFrame("logical_scene", 2U, 2U)));
  const auto oversized = h.Evaluate();
  EXPECT_FALSE(RequiredBool(oversized.root, "complete"));
  EXPECT_EQ(RequiredMember(oversized.root, "reasons"),
            Strings({"manager_status_section_oversized"}));
  EXPECT_EQ(RequiredMember(oversized.root, "unavailable_sections"),
            Strings({"logical_scene"}));
  const auto &scene = SectionOf(oversized, "logical_scene");
  EXPECT_EQ(RequiredMember(scene, "known_gaps"),
            Strings({"manager_status_section_oversized"}));
  EXPECT_TRUE(RequiredBool(scene, "available"));
  EXPECT_EQ(RequiredMember(scene, "source_content_digest"),
            RequiredMember(SectionOf(known, "logical_scene"),
                           "source_content_digest"));
  EXPECT_EQ(Freshness(SectionOf(oversized, "manager")), "fresh");
  EXPECT_EQ(Freshness(SectionOf(oversized, "scene_runtime")), "fresh");
}

TEST(ManagerStatusMirror, ChangeFlagsFollowWhatEachConsumerShows) {
  Harness h;
  const auto snapshot = h.Apply(StatusSnapshotFrame(1U));
  EXPECT_TRUE(snapshot.control_changed);
  EXPECT_TRUE(snapshot.viewer_changed);
  EXPECT_TRUE(snapshot.tick_advanced);
  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 1U, ManagerWithObservedUnit(1, 1), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "logical_scene", 1U, LogicalScenePayload(kCellId), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", 1U, SceneRuntimePayload(kCellId), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "viewer_pose_sessions", 1U, ViewerPoseSessionsPayload(), 1U)));

  const auto expect_flags = [&](const std::string &frame, const bool control,
                                const bool viewer) {
    const auto result = h.Apply(frame);
    EXPECT_EQ(result.control_changed, control) << frame;
    EXPECT_EQ(result.viewer_changed, viewer) << frame;
    EXPECT_FALSE(result.tick_advanced) << frame;
  };
  // Pose samples and the scene runtime's update time wake only the viewer.
  expect_flags(StatusSectionFrame(
                   "scene_runtime", 2U,
                   SceneRuntimeWithState(
                       "pose_sources",
                       CanonicalValue(CanonicalValue::Array{
                           CanonicalValue(CanonicalValue::Object{
                               {"source_id", CanonicalValue("pose-1")}})})),
                   1U),
               false, true);
  expect_flags(StatusSectionFrame("scene_runtime", 3U,
                                  SceneRuntimeWithState("updated_at_ns", Count(42)),
                                  1U),
               false, true);
  // The Manager's observation counters wake nobody.
  expect_flags(StatusSectionFrame("manager", 2U, ManagerWithObservedUnit(2, 1), 1U),
               false, false);
  expect_flags(StatusSectionFrame("manager", 3U, ManagerWithObservedUnit(2, 7), 1U),
               false, false);
  // The same logical-scene value under a new generation changes nothing.
  expect_flags(StatusSectionFrame("logical_scene", 2U,
                                  LogicalScenePayload(kCellId), 1U),
               false, false);
  // Meaningful changes wake the control state.
  expect_flags(StatusSectionFrame("manager", 4U,
                                  ManagerPayload(kCellId, "graph-2"), 1U),
               true, false);
  expect_flags(StatusSectionFrame(
                   "scene_runtime", 4U,
                   SceneRuntimePayloadWithJointStates(
                       kCellId, {CanonicalValue(JointStateRow())}),
                   1U),
               true, true);
  expect_flags(StatusSectionFrame("logical_scene", 3U,
                                  LogicalScenePayload("cell-b"), 1U),
               true, false);
  // Pose sessions are the viewer's alone.
  expect_flags(StatusSectionFrame(
                   "viewer_pose_sessions", 2U,
                   ViewerPoseSessionsPayload({CanonicalValue("session-1")}), 1U),
               false, true);
  // A new subscription wakes everyone.
  const auto resubscribed = h.Apply(StatusSnapshotFrame(2U, 4U, 3U, 4U, 2U));
  EXPECT_TRUE(resubscribed.control_changed);
  EXPECT_TRUE(resubscribed.viewer_changed);
  EXPECT_TRUE(resubscribed.tick_advanced);
}

TEST(ManagerStatusMirror, ViewerPoseSourcesOnlyWhileAnsweringAndBothCurrent) {
  Harness h;
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U, 0U, 0U, 1U, 1U)));
  const auto scene = SceneRuntimePayload(kCellId);
  const auto sessions = ViewerPoseSessionsPayload({CanonicalValue("session-1")});
  static_cast<void>(h.Apply(StatusSectionFrame("scene_runtime", 1U, scene, 1U)));
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(
      h.Apply(StatusSectionFrame("viewer_pose_sessions", 1U, sessions, 1U)));
  const std::pair<std::string, std::string> expected{
      content_identity::canonical_json(scene),
      content_identity::canonical_json(sessions)};
  EXPECT_TRUE(h.mirror.ViewerPoseSources() == expected);

  h.now += kSilenceNs + 1;
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(h.Apply(StatusKeepaliveFrame(2U)));
  EXPECT_TRUE(h.mirror.ViewerPoseSources() == expected);

  // A scene runtime of another cell is not shown.
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", 2U, SceneRuntimePayload("cell-b"), 2U)));
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(h.Apply(StatusSectionFrame("scene_runtime", 3U, scene, 2U)));
  EXPECT_TRUE(h.mirror.ViewerPoseSources() == expected);

  ASSERT_TRUE(h.mirror.MarkDisconnected());
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U, 0U, 0U, 3U, 1U)));
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(h.Apply(StatusSectionFrame("scene_runtime", 3U, scene, 1U)));
  EXPECT_FALSE(h.mirror.ViewerPoseSources().has_value());
  static_cast<void>(
      h.Apply(StatusSectionFrame("viewer_pose_sessions", 1U, sessions, 1U)));
  EXPECT_TRUE(h.mirror.ViewerPoseSources() == expected);
}

TEST(ManagerStatusMirror, ErrorAndInvalidFramesEndTheSubscriptionWithACode) {
  Harness h;
  const auto error_frame = content_identity::canonical_json(StatusEnvelope(
      "error", 0U,
      {{"error", CanonicalValue(CanonicalValue::Object{
                     {"code", CanonicalValue("manager_status_subscribers_full")},
                     {"message", CanonicalValue("full")},
                 })}}));
  EXPECT_EQ(StreamErrorCode(h.mirror, error_frame),
            "manager_status_subscribers_full");
  // Frames before the subscription's snapshot.
  EXPECT_EQ(StreamErrorCode(h.mirror, StatusKeepaliveFrame(1U)),
            "manager_status_stream_invalid");
  EXPECT_EQ(StreamErrorCode(h.mirror,
                            StatusSectionFrame("manager", 1U,
                                               ManagerPayload(kCellId, "graph-1"),
                                               1U)),
            "manager_status_stream_invalid");

  h.ConnectFullState(5U);
  const auto other_version = [] {
    auto members = StatusEnvelope("keepalive", 6U).as_object();
    for (auto &[key, value] : members) {
      if (key == "api_version") {
        value = CanonicalValue("manyforge.manager_status_stream.v0");
      }
    }
    return content_identity::canonical_json(CanonicalValue(std::move(members)));
  }();
  for (const std::string &frame : {
           std::string("not-json"),
           other_version,
           StatusKeepaliveFrame(4U),
           StatusSectionFrame("bogus", 1U, LogicalScenePayload(kCellId), 5U),
           StatusSectionFrame("manager", 0U, ManagerPayload(kCellId, "graph-1"),
                              5U),
       }) {
    EXPECT_EQ(StreamErrorCode(h.mirror, frame), "manager_status_stream_invalid")
        << frame;
  }
  // A refused frame leaves the mirror as it was.
  EXPECT_TRUE(RequiredBool(h.Evaluate().root, "complete"));
  EXPECT_EQ(h.mirror.Load()->tick, 5U);

  ASSERT_TRUE(h.mirror.MarkDisconnected());
  EXPECT_EQ(StreamErrorCode(h.mirror, StatusKeepaliveFrame(6U)),
            "manager_status_stream_invalid");
}

TEST(RuntimeSnapshotAggregator,
     GenerationTracksSemanticAndFreshnessTransitionsButNotClockTicks) {
  Harness h;
  h.ConnectFullState(1U);
  const auto first = h.Evaluate();
  EXPECT_EQ(first.projection_generation, 1U);
  EXPECT_TRUE(RequiredBool(first.root, "complete"));

  h.now += 100;
  h.projection_now = 200;
  const auto clock_only = h.Evaluate();
  EXPECT_EQ(clock_only.projection_generation, 1U);
  EXPECT_EQ(clock_only.semantic_content_digest, first.semantic_content_digest);
  EXPECT_NE(clock_only.root, first.root);

  h.now = 1'000 + kSilenceNs + 1;
  h.projection_now = 300;
  const auto stale = h.Evaluate();
  EXPECT_EQ(stale.projection_generation, 2U);
  EXPECT_FALSE(RequiredBool(stale.root, "complete"));
  EXPECT_NE(stale.semantic_content_digest, first.semantic_content_digest);

  static_cast<void>(h.Apply(StatusKeepaliveFrame(2U)));
  h.projection_now = 400;
  const auto fresh_again = h.Evaluate();
  EXPECT_EQ(fresh_again.projection_generation, 3U);
  EXPECT_TRUE(RequiredBool(fresh_again.root, "complete"));
  EXPECT_EQ(fresh_again.semantic_content_digest, first.semantic_content_digest);

  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 2U, ManagerPayload(kCellId, "graph-2"), 2U, 50)));
  h.projection_now = 500;
  const auto changed = h.Evaluate();
  EXPECT_EQ(changed.projection_generation, 4U);
  EXPECT_TRUE(RequiredBool(changed.root, "complete"));
  EXPECT_NE(changed.semantic_content_digest, fresh_again.semantic_content_digest);
  EXPECT_EQ(RequiredMember(changed.root, "active_graph_revision"),
            CanonicalValue("graph-2"));
  EXPECT_EQ(RequiredMember(changed.root, "active_retained_bundle_id"),
            CanonicalValue("bundle-1"));
}

TEST(RuntimeSnapshotAggregator, SamePayloadKeepsItsSourceContentDigest) {
  Harness h;
  h.ConnectFullState(1U);
  const auto first = h.Evaluate();
  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 7U, ManagerPayload(kCellId, "graph-1"), 1U, 99)));
  const auto again = h.Evaluate();
  EXPECT_EQ(RequiredMember(SectionOf(again, "manager"), "source_content_digest"),
            RequiredMember(SectionOf(first, "manager"), "source_content_digest"));
  EXPECT_EQ(RequiredMember(SectionOf(again, "manager"), "source_generation"),
            CanonicalValue(7.0));

  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 8U, ManagerPayload(kCellId, "graph-2"), 1U, 100)));
  EXPECT_NE(RequiredMember(SectionOf(h.Evaluate(), "manager"),
                           "source_content_digest"),
            RequiredMember(SectionOf(first, "manager"), "source_content_digest"));
}

TEST(RuntimeSnapshotAggregator,
     AcceptsConfiguredJointStatesButRejectsMalformedShape) {
  Harness h;
  std::uint64_t generation = 1U;
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 1U, ManagerPayload(kCellId, "graph-1"), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "logical_scene", 1U, LogicalScenePayload(kCellId), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", generation, SceneRuntimePayloadWithJointStates(kCellId, {}),
      1U)));
  EXPECT_TRUE(RequiredBool(h.Evaluate().root, "complete"));

  const auto valid_rows =
      SceneRuntimePayloadWithJointStates(kCellId, {CanonicalValue(JointStateRow())});
  static_cast<void>(
      h.Apply(StatusSectionFrame("scene_runtime", ++generation, valid_rows, 1U)));
  EXPECT_TRUE(RequiredBool(h.Evaluate().root, "complete"));

  const auto expect_invalid = [&](CanonicalValue payload) {
    static_cast<void>(h.Apply(
        StatusSectionFrame("scene_runtime", ++generation, payload, 1U)));
    const auto invalid = h.Evaluate();
    EXPECT_FALSE(RequiredBool(invalid.root, "complete"));
    const auto &section = SectionOf(invalid, "scene_runtime");
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({"scene_runtime_source_invalid"}));
    // The last valid value stays.
    EXPECT_TRUE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "payload"), valid_rows);
    // Back to valid, so each case is checked on its own.
    static_cast<void>(h.Apply(
        StatusSectionFrame("scene_runtime", ++generation, valid_rows, 1U)));
    EXPECT_TRUE(RequiredBool(h.Evaluate().root, "complete"));
  };
  auto extra_field = JointStateRow();
  extra_field.emplace_back("unexpected", CanonicalValue(true));
  expect_invalid(SceneRuntimePayloadWithJointStates(
      kCellId, {CanonicalValue(std::move(extra_field))}));
  auto missing_topic = JointStateRow();
  missing_topic.pop_back();
  expect_invalid(SceneRuntimePayloadWithJointStates(
      kCellId, {CanonicalValue(std::move(missing_topic))}));
  auto non_string = JointStateRow();
  non_string.back().second = CanonicalValue(1.0);
  expect_invalid(SceneRuntimePayloadWithJointStates(
      kCellId, {CanonicalValue(std::move(non_string))}));
  auto not_array = SceneRuntimePayload(kCellId).as_object();
  not_array.emplace_back("configured_joint_states", CanonicalValue(nullptr));
  expect_invalid(CanonicalValue(std::move(not_array)));
  auto malformed = SceneRuntimePayload(kCellId).as_object();
  malformed.emplace_back("unexpected", CanonicalValue(true));
  expect_invalid(CanonicalValue(std::move(malformed)));
  expect_invalid(SceneRuntimeWithState("pose_sources", CanonicalValue(nullptr)));
}

TEST(RuntimeSnapshotAggregator, InvalidOrAbsentSectionKeepsLastValidValue) {
  Harness h;
  h.ConnectFullState(1U);
  const auto known = h.Evaluate();
  ASSERT_TRUE(RequiredBool(known.root, "complete"));
  const auto &known_manager = SectionOf(known, "manager");

  // A payload the Headless API cannot use.
  auto unready = ManagerPayload(kCellId, "graph-2");
  unready = WithMember(unready, "manager_ready", CanonicalValue(false));
  static_cast<void>(h.Apply(StatusSectionFrame("manager", 2U, unready, 1U)));
  const auto invalid = h.Evaluate();
  EXPECT_FALSE(RequiredBool(invalid.root, "complete"));
  EXPECT_EQ(RequiredMember(invalid.root, "reasons"),
            Strings({"manager_source_invalid"}));
  const auto &invalid_manager = SectionOf(invalid, "manager");
  EXPECT_EQ(Freshness(invalid_manager), "unavailable");
  EXPECT_TRUE(RequiredBool(invalid_manager, "available"));
  EXPECT_EQ(RequiredMember(invalid_manager, "source_content_digest"),
            RequiredMember(known_manager, "source_content_digest"));
  EXPECT_EQ(RequiredMember(invalid_manager, "source_generation"),
            RequiredMember(known_manager, "source_generation"));
  EXPECT_EQ(RequiredMember(invalid.root, "active_graph_revision"),
            CanonicalValue("graph-1"));

  // A new subscription that has not resent the Manager section yet.
  ASSERT_TRUE(h.mirror.MarkDisconnected());
  static_cast<void>(h.Apply(StatusSnapshotFrame(2U, 2U, 1U, 1U, 0U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "logical_scene", 1U, LogicalScenePayload(kCellId), 2U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", 1U, SceneRuntimePayload(kCellId), 2U)));
  const auto absent = h.Evaluate();
  EXPECT_FALSE(RequiredBool(absent.root, "complete"));
  EXPECT_EQ(RequiredMember(absent.root, "reasons"),
            Strings({"manager_source_absent"}));
  const auto &absent_manager = SectionOf(absent, "manager");
  EXPECT_EQ(RequiredMember(absent_manager, "source_content_digest"),
            RequiredMember(known_manager, "source_content_digest"));
  EXPECT_EQ(RequiredMember(absent_manager, "source_generation"),
            RequiredMember(known_manager, "source_generation"));
}

TEST(RuntimeSnapshotAggregator, RejectsInvalidConfigurationAndForeignCellPayloads) {
  const auto clock = [] { return std::int64_t{1}; };
  EXPECT_THROW(ManagerStatusMirror("cell-a", 0, clock), RuntimeSnapshotError);
  EXPECT_THROW(ManagerStatusMirror("", kSilenceNs, clock), RuntimeSnapshotError);
  EXPECT_THROW(ManagerStatusMirror("cell-a", kSilenceNs, {}),
               RuntimeSnapshotError);
  const ManagerStatusMirror mirror("cell-a", kSilenceNs, clock);
  EXPECT_THROW(RuntimeSnapshotAggregator(mirror, "", "epoch", clock),
               RuntimeSnapshotError);
  EXPECT_THROW(RuntimeSnapshotAggregator(mirror, "api.cell-a", "", clock),
               RuntimeSnapshotError);
  EXPECT_THROW(RuntimeSnapshotAggregator(mirror, "api.cell-a", "epoch", {}),
               RuntimeSnapshotError);

  Harness h;
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "manager", 1U, ManagerPayload("cell-b", "graph-1"), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "logical_scene", 1U, LogicalScenePayload("cell-b"), 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame(
      "scene_runtime", 1U, SceneRuntimePayload("cell-b"), 1U)));
  const auto snapshot = h.Evaluate();
  EXPECT_FALSE(RequiredBool(snapshot.root, "complete"));
  for (const auto &name : kSectionNames) {
    const auto &section = SectionOf(snapshot, name);
    EXPECT_FALSE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({name + "_source_invalid"}));
  }
  EXPECT_TRUE(RequiredMember(snapshot.root, "active_graph_revision").is_null());
}

/// The aggregated Manager section after one Manager frame with `payload`.
CanonicalValue ManagerSectionAfter(const CanonicalValue &payload) {
  Harness h;
  static_cast<void>(h.Apply(StatusSnapshotFrame(1U, 1U)));
  static_cast<void>(h.Apply(StatusSectionFrame("manager", 1U, payload, 1U)));
  const auto snapshot = h.Evaluate();
  return SectionOf(snapshot, "manager");
}

/// A Manager payload whose `active` object is changed by `edit`.
CanonicalValue ManagerWithActive(const std::function<void(CanonicalValue::Object &)> &edit) {
  const auto payload = ManagerPayload("cell-a", "graph-1");
  auto active = payload.find_object_member("active")->as_object();
  edit(active);
  return WithMember(payload, "active", CanonicalValue(std::move(active)));
}

TEST(RuntimeSnapshotAggregator, PreservesRequiredCanonicalEmptyOwnerBoolean) {
  for (const bool canonical_empty : {false, true}) {
    const auto section = ManagerSectionAfter(
        ManagerWithActive([&](CanonicalValue::Object &active) {
          for (auto &[key, field] : active) {
            if (key == "canonical_empty") field = CanonicalValue(canonical_empty);
          }
        }));
    ASSERT_TRUE(RequiredBool(section, "available"));
    const auto &active = RequiredMember(RequiredMember(section, "payload"), "active");
    EXPECT_EQ(RequiredBool(active, "canonical_empty"), canonical_empty);
  }
}

TEST(RuntimeSnapshotAggregator, RejectsMissingNonBooleanAndUnknownCanonicalEmptyShape) {
  for (const int invalid_shape : {0, 1, 2}) {
    const auto section = ManagerSectionAfter(
        ManagerWithActive([&](CanonicalValue::Object &active) {
          CanonicalValue::Object result;
          for (const auto &[key, field] : active) {
            if (key == "canonical_empty" && invalid_shape == 0) continue;
            result.emplace_back(key, key == "canonical_empty" && invalid_shape == 1
                                         ? CanonicalValue("true") : field);
          }
          if (invalid_shape == 2) result.emplace_back("unknown", CanonicalValue(false));
          active = std::move(result);
        }));
    EXPECT_FALSE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({"manager_source_invalid"}));
    EXPECT_TRUE(RequiredMember(section, "payload").is_null());
  }
}

CanonicalValue DependencyRow(const std::string &component,
                             const std::string &simulator) {
  return CanonicalValue(CanonicalValue::Object{
      {"controllable_component_id", CanonicalValue(component)},
      {"simulator", CanonicalValue(simulator)},
  });
}

// The aggregated Manager section of a payload whose active section carries
// `dependencies`.
CanonicalValue ManagerSectionWithDependencies(const CanonicalValue &dependencies) {
  return ManagerSectionAfter(
      ManagerWithActive([&](CanonicalValue::Object &active) {
        active.emplace_back("component_simulator_dependencies", dependencies);
      }));
}

TEST(RuntimeSnapshotAggregator, AcceptsOptionalComponentSimulatorDependencies) {
  for (const auto &rows :
       {CanonicalValue::Array{},
        CanonicalValue::Array{DependencyRow("arm.isaac", "isaac"),
                              DependencyRow("arm.real", "none")}}) {
    const auto section = ManagerSectionWithDependencies(CanonicalValue(rows));
    ASSERT_TRUE(RequiredBool(section, "available"));
    const auto &active = RequiredMember(RequiredMember(section, "payload"), "active");
    EXPECT_EQ(RequiredMember(active, "component_simulator_dependencies"),
              CanonicalValue(rows));
  }
}

TEST(RuntimeSnapshotAggregator, RejectsMalformedComponentSimulatorDependencies) {
  auto open_row = DependencyRow("arm", "none").as_object();
  open_row.emplace_back("isaac_profile", CanonicalValue("lite6"));
  const std::vector<CanonicalValue> invalid{
      CanonicalValue(CanonicalValue::Object{}),
      CanonicalValue(CanonicalValue::Array{CanonicalValue("arm")}),
      CanonicalValue(CanonicalValue::Array{CanonicalValue(std::move(open_row))}),
      CanonicalValue(CanonicalValue::Array{CanonicalValue(CanonicalValue::Object{
          {"controllable_component_id", CanonicalValue("arm")}})}),
      CanonicalValue(CanonicalValue::Array{DependencyRow("arm", "mock")}),
      CanonicalValue(CanonicalValue::Array{DependencyRow("", "none")}),
      CanonicalValue(CanonicalValue::Array{DependencyRow("arm", "none"),
                                           DependencyRow("arm", "isaac")}),
  };
  for (const auto &rows : invalid) {
    const auto section = ManagerSectionWithDependencies(rows);
    EXPECT_FALSE(RequiredBool(section, "available"));
    EXPECT_EQ(RequiredMember(section, "known_gaps"),
              Strings({"manager_source_invalid"}));
  }
}

} // namespace
} // namespace manyforge::headless

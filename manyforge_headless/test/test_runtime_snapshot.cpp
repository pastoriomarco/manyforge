#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <manyforge_content_identity/canonical_value.hpp>

#include "manyforge_headless/runtime_snapshot.hpp"
#include "manyforge_headless/scene_runtime_state_owner.hpp"
#include "manyforge_headless/source_contract.hpp"
#include "manyforge_headless/source_snapshot.hpp"

namespace manyforge::headless {
namespace {

using content_identity::CanonicalValue;

constexpr std::int64_t kFreshnessBudgetNs = 10'000'000'000LL;

class TemporaryDirectory final {
public:
  TemporaryDirectory() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "manyforge-headless-runtime-snapshot-XXXXXX")
                       .string();
    buffer_.assign(pattern.begin(), pattern.end());
    buffer_.push_back('\0');
    const auto *created = ::mkdtemp(buffer_.data());
    if (created == nullptr) {
      throw std::runtime_error("runtime snapshot test mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::vector<char> buffer_;
  std::filesystem::path path_;
};

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

const CanonicalValue &Section(const HeadlessRuntimeSnapshot &snapshot,
                              const std::string_view name) {
  return RequiredMember(RequiredMember(snapshot.root, "sections"), name);
}

TEST(RuntimeSnapshotAggregator,
     GenerationTracksSemanticAndFreshnessTransitionsButNotClockTicks) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
  AtomicSourceSnapshotChannel scene(directory.path(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime(directory.path(),
                                            "scene-runtime-source.json");
  const std::string cell_id = "cell-a";
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      10, ManagerPayload(cell_id, "graph-1")));
  static_cast<void>(scene.Publish("logical_scene", "logical-scene." + cell_id,
                                  "scene-epoch-1", 20,
                                  LogicalScenePayload(cell_id)));
  static_cast<void>(scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 30,
      SceneRuntimePayload(cell_id)));
  const auto manager_observation = manager.ReadObserved();
  const auto scene_observation = scene.ReadObserved();
  const auto scene_runtime_observation = scene_runtime.ReadObserved();
  ASSERT_TRUE(manager_observation.has_value());
  ASSERT_TRUE(scene_observation.has_value());
  ASSERT_TRUE(scene_runtime_observation.has_value());

  const auto publication_time =
      std::max({manager_observation->file_modified_at_unix_ns,
                scene_observation->file_modified_at_unix_ns,
                scene_runtime_observation->file_modified_at_unix_ns});
  std::int64_t wall_now = publication_time + 10;
  std::int64_t projection_now = 100;
  RuntimeSnapshotAggregator aggregator(
      directory.path(), cell_id, "headless-api.cell-a", "api-epoch-1",
      RuntimeSnapshotFreshnessPolicy{kFreshnessBudgetNs, kFreshnessBudgetNs,
                                     kFreshnessBudgetNs},
      [&] { return wall_now; }, [&] { return projection_now; });

  const auto first = aggregator.Evaluate();
  EXPECT_EQ(first.projection_generation, 1U);
  EXPECT_TRUE(RequiredBool(first.root, "complete"));

  wall_now = publication_time + 20;
  projection_now = 200;
  const auto clock_only = aggregator.Evaluate();
  EXPECT_EQ(clock_only.projection_generation, 1U);
  EXPECT_EQ(clock_only.semantic_content_digest, first.semantic_content_digest);
  EXPECT_NE(clock_only.root, first.root);

  wall_now = publication_time + kFreshnessBudgetNs + 1;
  projection_now = 300;
  const auto stale = aggregator.Evaluate();
  EXPECT_EQ(stale.projection_generation, 2U);
  EXPECT_FALSE(RequiredBool(stale.root, "complete"));
  EXPECT_NE(stale.semantic_content_digest, first.semantic_content_digest);
  EXPECT_NE(
      content_identity::canonical_json(stale.root).find("manager_source_stale"),
      std::string::npos);

  const auto refreshed_manager = manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      30, ManagerPayload(cell_id, "graph-1"));
  const auto refreshed_scene =
      scene.Publish("logical_scene", "logical-scene." + cell_id,
                    "scene-epoch-1", 40, LogicalScenePayload(cell_id));
  const auto refreshed_scene_runtime = scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 50,
      SceneRuntimePayload(cell_id));
  EXPECT_EQ(refreshed_manager.source_generation, 1U);
  EXPECT_EQ(refreshed_scene.source_generation, 1U);
  EXPECT_EQ(refreshed_scene_runtime.source_generation, 1U);
  const auto refreshed_manager_observation = manager.ReadObserved();
  const auto refreshed_scene_observation = scene.ReadObserved();
  const auto refreshed_scene_runtime_observation = scene_runtime.ReadObserved();
  ASSERT_TRUE(refreshed_manager_observation.has_value());
  ASSERT_TRUE(refreshed_scene_observation.has_value());
  ASSERT_TRUE(refreshed_scene_runtime_observation.has_value());
  wall_now =
      std::max(
          {refreshed_manager_observation->file_modified_at_unix_ns,
           refreshed_scene_observation->file_modified_at_unix_ns,
           refreshed_scene_runtime_observation->file_modified_at_unix_ns}) +
      10;
  projection_now = 400;
  const auto fresh_again = aggregator.Evaluate();
  EXPECT_EQ(fresh_again.projection_generation, 3U);
  EXPECT_TRUE(RequiredBool(fresh_again.root, "complete"));

  const auto changed_manager = manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      50, ManagerPayload(cell_id, "graph-2"));
  EXPECT_EQ(changed_manager.source_generation, 2U);
  const auto changed_observation = manager.ReadObserved();
  ASSERT_TRUE(changed_observation.has_value());
  wall_now =
      std::max(
          {changed_observation->file_modified_at_unix_ns,
           refreshed_scene_observation->file_modified_at_unix_ns,
           refreshed_scene_runtime_observation->file_modified_at_unix_ns}) +
      10;
  projection_now = 500;
  const auto changed = aggregator.Evaluate();
  EXPECT_EQ(changed.projection_generation, 4U);
  EXPECT_TRUE(RequiredBool(changed.root, "complete"));
  EXPECT_NE(changed.semantic_content_digest,
            fresh_again.semantic_content_digest);
}

TEST(RuntimeSnapshotAggregator,
     AcceptsConfiguredJointStatesButRejectsMalformedShape) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
  AtomicSourceSnapshotChannel scene(directory.path(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime(directory.path(),
                                            "scene-runtime-source.json");
  const std::string cell_id = "cell-a";
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      10, ManagerPayload(cell_id, "graph-1")));
  static_cast<void>(scene.Publish("logical_scene", "logical-scene." + cell_id,
                                  "scene-epoch-1", 20,
                                  LogicalScenePayload(cell_id)));
  static_cast<void>(scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 30,
      SceneRuntimePayloadWithJointStates(cell_id, {})));
  std::int64_t wall_now = std::max(
      {manager.ReadObserved()->file_modified_at_unix_ns,
       scene.ReadObserved()->file_modified_at_unix_ns,
       scene_runtime.ReadObserved()->file_modified_at_unix_ns}) + 10;
  RuntimeSnapshotAggregator aggregator(
      directory.path(), cell_id, "headless-api.cell-a", "api-epoch-1",
      RuntimeSnapshotFreshnessPolicy{kFreshnessBudgetNs, kFreshnessBudgetNs,
                                     kFreshnessBudgetNs},
      [&] { return wall_now; }, [] { return 100; });
  EXPECT_TRUE(RequiredBool(aggregator.Evaluate().root, "complete"));

  static_cast<void>(scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 40,
      SceneRuntimePayloadWithJointStates(cell_id,
                                         {CanonicalValue(JointStateRow())})));
  wall_now = scene_runtime.ReadObserved()->file_modified_at_unix_ns + 10;
  EXPECT_TRUE(RequiredBool(aggregator.Evaluate().root, "complete"));

  const auto expect_invalid = [&](const std::int64_t observed_at,
                                  CanonicalValue payload) {
    static_cast<void>(scene_runtime.Publish(
        "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1",
        observed_at, std::move(payload)));
    wall_now = scene_runtime.ReadObserved()->file_modified_at_unix_ns + 10;
    const auto invalid = aggregator.Evaluate();
    EXPECT_FALSE(RequiredBool(invalid.root, "complete"));
    EXPECT_NE(content_identity::canonical_json(invalid.root)
                  .find("scene_runtime_source_invalid"),
              std::string::npos);
  };
  auto extra_field = JointStateRow();
  extra_field.emplace_back("unexpected", CanonicalValue(true));
  expect_invalid(50, SceneRuntimePayloadWithJointStates(
                         cell_id, {CanonicalValue(std::move(extra_field))}));
  auto missing_topic = JointStateRow();
  missing_topic.pop_back();
  expect_invalid(55, SceneRuntimePayloadWithJointStates(
                         cell_id, {CanonicalValue(std::move(missing_topic))}));
  auto non_string = JointStateRow();
  non_string.back().second = CanonicalValue(1.0);
  expect_invalid(57, SceneRuntimePayloadWithJointStates(
                         cell_id, {CanonicalValue(std::move(non_string))}));
  auto not_array = SceneRuntimePayload(cell_id).as_object();
  not_array.emplace_back("configured_joint_states", CanonicalValue(nullptr));
  expect_invalid(60, CanonicalValue(std::move(not_array)));
  auto malformed = SceneRuntimePayload(cell_id).as_object();
  malformed.emplace_back("unexpected", CanonicalValue(true));
  expect_invalid(70, CanonicalValue(std::move(malformed)));
}

TEST(RuntimeSnapshotAggregator,
     FreshnessUsesAWallClockSampleTakenAfterEachAtomicRead) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
  AtomicSourceSnapshotChannel scene(directory.path(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime(directory.path(),
                                            "scene-runtime-source.json");
  const std::string cell_id = "cell-a";
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      10, ManagerPayload(cell_id, "graph-1")));
  static_cast<void>(scene.Publish("logical_scene", "logical-scene." + cell_id,
                                  "scene-epoch-1", 20,
                                  LogicalScenePayload(cell_id)));
  static_cast<void>(scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 30,
      SceneRuntimePayload(cell_id)));
  const auto manager_observation = manager.ReadObserved();
  const auto scene_observation = scene.ReadObserved();
  const auto scene_runtime_observation = scene_runtime.ReadObserved();
  ASSERT_TRUE(manager_observation.has_value());
  ASSERT_TRUE(scene_observation.has_value());
  ASSERT_TRUE(scene_runtime_observation.has_value());
  const auto publication_time =
      std::max({manager_observation->file_modified_at_unix_ns,
                scene_observation->file_modified_at_unix_ns,
                scene_runtime_observation->file_modified_at_unix_ns});

  std::size_t wall_samples = 0U;
  RuntimeSnapshotAggregator aggregator(
      directory.path(), cell_id, "headless-api.cell-a", "api-epoch-1",
      RuntimeSnapshotFreshnessPolicy{kFreshnessBudgetNs, kFreshnessBudgetNs,
                                     kFreshnessBudgetNs},
      [&] {
        ++wall_samples;
        return wall_samples == 1U ? publication_time - 1 : publication_time + 1;
      },
      [] { return 100; });

  const auto snapshot = aggregator.Evaluate();
  EXPECT_TRUE(RequiredBool(snapshot.root, "complete"));
  EXPECT_GE(wall_samples, 4U);
  EXPECT_EQ(content_identity::canonical_json(snapshot.root)
                .find("publication_time_in_future"),
            std::string::npos);
}

TEST(RuntimeSnapshotAggregator,
     RestartMeasuresPersistedAgeAndLossRetainsLastIdentifiedGeneration) {
  TemporaryDirectory directory;
  AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
  AtomicSourceSnapshotChannel scene(directory.path(),
                                    "logical-scene-source.json");
  AtomicSourceSnapshotChannel scene_runtime(directory.path(),
                                            "scene-runtime-source.json");
  const std::string cell_id = "cell-a";
  static_cast<void>(manager.Publish(
      "component_manager", "component-manager." + cell_id, "manager-epoch-1",
      10, ManagerPayload(cell_id, "graph-1")));
  static_cast<void>(scene.Publish("logical_scene", "logical-scene." + cell_id,
                                  "scene-epoch-1", 20,
                                  LogicalScenePayload(cell_id)));
  static_cast<void>(scene_runtime.Publish(
      "scene_runtime", "scene-runtime." + cell_id, "scene-runtime-epoch-1", 30,
      SceneRuntimePayload(cell_id)));
  const auto manager_observation = manager.ReadObserved();
  const auto scene_observation = scene.ReadObserved();
  const auto scene_runtime_observation = scene_runtime.ReadObserved();
  ASSERT_TRUE(manager_observation.has_value());
  ASSERT_TRUE(scene_observation.has_value());
  ASSERT_TRUE(scene_runtime_observation.has_value());
  const auto publication_time =
      std::max({manager_observation->file_modified_at_unix_ns,
                scene_observation->file_modified_at_unix_ns,
                scene_runtime_observation->file_modified_at_unix_ns});

  std::int64_t wall_now = publication_time + kFreshnessBudgetNs + 1;
  std::int64_t projection_now = 100;
  RuntimeSnapshotAggregator restarted(
      directory.path(), cell_id, "headless-api.cell-a", "api-epoch-2",
      RuntimeSnapshotFreshnessPolicy{kFreshnessBudgetNs, kFreshnessBudgetNs,
                                     kFreshnessBudgetNs},
      [&] { return wall_now; }, [&] { return projection_now; });
  const auto first_after_restart = restarted.Evaluate();
  EXPECT_EQ(first_after_restart.projection_generation, 1U);
  EXPECT_FALSE(RequiredBool(first_after_restart.root, "complete"));
  EXPECT_NE(content_identity::canonical_json(first_after_restart.root)
                .find("manager_source_stale"),
            std::string::npos);

  wall_now = publication_time + 10;
  projection_now = 200;
  const auto known = restarted.Evaluate();
  EXPECT_TRUE(RequiredBool(known.root, "complete"));
  const auto &known_manager = Section(known, "manager");

  ASSERT_TRUE(
      std::filesystem::remove(directory.path() / "manager-source.json"));
  projection_now = 300;
  const auto absent = restarted.Evaluate();
  EXPECT_FALSE(RequiredBool(absent.root, "complete"));
  const auto &absent_manager = Section(absent, "manager");
  EXPECT_EQ(RequiredMember(absent_manager, "source_content_digest"),
            RequiredMember(known_manager, "source_content_digest"));
  EXPECT_EQ(RequiredMember(absent_manager, "source_generation"),
            RequiredMember(known_manager, "source_generation"));
  EXPECT_NE(content_identity::canonical_json(absent.root)
                .find("manager_source_absent"),
            std::string::npos);

  {
    std::ofstream corrupt(directory.path() / "manager-source.json");
    corrupt << "not-json";
  }
  projection_now = 400;
  const auto invalid = restarted.Evaluate();
  EXPECT_FALSE(RequiredBool(invalid.root, "complete"));
  EXPECT_EQ(
      RequiredMember(Section(invalid, "manager"), "source_content_digest"),
      RequiredMember(known_manager, "source_content_digest"));
  EXPECT_NE(content_identity::canonical_json(invalid.root)
                .find("manager_source_invalid"),
            std::string::npos);
}

TEST(RuntimeSnapshotAggregator, RejectsInvalidConfigurationAndForeignSources) {
  TemporaryDirectory directory;
  EXPECT_THROW(RuntimeSnapshotAggregator(
                   directory.path(), "cell-a", "api.cell-a", "epoch", {0, 1, 1},
                   [] { return 1; }, [] { return 1; }),
               RuntimeSnapshotError);

  AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
  static_cast<void>(manager.Publish("component_manager",
                                    "component-manager.cell-b", "manager-epoch",
                                    10, ManagerPayload("cell-b", "graph-1")));
  RuntimeSnapshotAggregator aggregator(
      directory.path(), "cell-a", "api.cell-a", "epoch", {100, 100, 100},
      [] { return 1'000'000'000'000'000LL; }, [] { return 1; });
  const auto snapshot = aggregator.Evaluate();
  EXPECT_FALSE(RequiredBool(snapshot.root, "complete"));
  EXPECT_NE(content_identity::canonical_json(snapshot.root)
                .find("manager_source_identity_mismatch"),
            std::string::npos);
}

TEST(RuntimeSnapshotAggregator, PreservesRequiredCanonicalEmptyOwnerBoolean) {
  for (const bool canonical_empty : {false, true}) {
    TemporaryDirectory directory;
    auto payload = ManagerPayload("cell-a", "graph-1");
    auto members = payload.as_object();
    for (auto &[name, value] : members) {
      if (name != "active") continue;
      auto active = value.as_object();
      for (auto &[key, field] : active) {
        if (key == "canonical_empty") field = CanonicalValue(canonical_empty);
      }
      value = CanonicalValue(std::move(active));
    }
    AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
    static_cast<void>(manager.Publish("component_manager", "component-manager.cell-a",
                                     "manager-epoch", 10, CanonicalValue(std::move(members))));
    const auto observed = manager.ReadObserved();
    ASSERT_TRUE(observed.has_value());
    RuntimeSnapshotAggregator aggregator(directory.path(), "cell-a", "api.cell-a", "epoch",
                                         {100, 100, 100},
                                         [&] { return observed->file_modified_at_unix_ns; },
                                         [] { return 1; });
    const auto snapshot = aggregator.Evaluate();
    const auto &section = Section(snapshot, "manager");
    ASSERT_TRUE(RequiredBool(section, "available"));
    const auto &active = RequiredMember(RequiredMember(section, "payload"), "active");
    EXPECT_EQ(RequiredBool(active, "canonical_empty"), canonical_empty);
  }
}

TEST(RuntimeSnapshotAggregator, RejectsMissingNonBooleanAndUnknownCanonicalEmptyShape) {
  for (const int invalid_shape : {0, 1, 2}) {
    TemporaryDirectory directory;
    auto members = ManagerPayload("cell-a", "graph-1").as_object();
    for (auto &[name, value] : members) {
      if (name != "active") continue;
      CanonicalValue::Object active;
      for (const auto &[key, field] : value.as_object()) {
        if (key == "canonical_empty" && invalid_shape == 0) continue;
        active.emplace_back(key, key == "canonical_empty" && invalid_shape == 1
                                     ? CanonicalValue("true") : field);
      }
      if (invalid_shape == 2) active.emplace_back("unknown", CanonicalValue(false));
      value = CanonicalValue(std::move(active));
    }
    AtomicSourceSnapshotChannel manager(directory.path(), "manager-source.json");
    static_cast<void>(manager.Publish("component_manager", "component-manager.cell-a",
                                     "manager-epoch", 10, CanonicalValue(std::move(members))));
    const auto observed = manager.ReadObserved();
    ASSERT_TRUE(observed.has_value());
    RuntimeSnapshotAggregator aggregator(directory.path(), "cell-a", "api.cell-a", "epoch",
                                         {100, 100, 100},
                                         [&] { return observed->file_modified_at_unix_ns; },
                                         [] { return 1; });
    const auto snapshot = aggregator.Evaluate();
    const auto &section = Section(snapshot, "manager");
    EXPECT_FALSE(RequiredBool(section, "available"));
    EXPECT_NE(content_identity::canonical_json(section).find("manager_source_invalid"),
              std::string::npos);
    EXPECT_TRUE(std::holds_alternative<std::nullptr_t>(RequiredMember(section, "payload").storage()));
  }
}

} // namespace
} // namespace manyforge::headless

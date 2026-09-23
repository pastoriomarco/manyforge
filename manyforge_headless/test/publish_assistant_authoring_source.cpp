#include <chrono>
#include <filesystem>
#include <iostream>
#include <manyforge_component_manager/manager_apply_journal.hpp>
#include <manyforge_component_manager/runtime_observation.hpp>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/active_bundle_publication.hpp>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

#include "assistant_authoring_fixture.hpp"
#include "manyforge_headless/owner_publication.hpp"
#include "manyforge_headless/source_snapshot.hpp"

namespace {

namespace component_manager = manyforge::component_manager;
namespace core = manyforge::core;
namespace headless = manyforge::headless;

constexpr std::string_view kDigestOne =
    "sha256:1111111111111111111111111111111111111111111111111111111111111111";
constexpr std::string_view kDigestTwo =
    "sha256:2222222222222222222222222222222222222222222222222222222222222222";
constexpr std::string_view kDigestThree =
    "sha256:3333333333333333333333333333333333333333333333333333333333333333";
constexpr std::string_view kSceneId =
    "sha256:abababababababababababababababababababababababababababababababab";

[[nodiscard]] std::filesystem::path ParsePublicationRoot(const int argc,
                                                         char **argv) {
  if (argc != 3 || std::string_view(argv[1]) != "--publication-root" ||
      std::string_view(argv[2]).empty()) {
    throw std::invalid_argument(
        "usage: publish_assistant_authoring_source --publication-root <path>");
  }
  return std::filesystem::path(argv[2]);
}

[[nodiscard]] core::ProjectionRevisionDTO
Projection(const manyforge::deployment::RetainedBundle &bundle) {
  return {
      bundle.retained_bundle_id(),
      bundle.value().cell_graph.graph_revision,
      core::SceneRevision{
          core::SceneId(std::string(kSceneId)),
          1U,
          core::ContentDigest(std::string(kDigestOne)),
      },
  };
}

[[nodiscard]] component_manager::ManagerActiveRuntimeConvergenceResult
RuntimeSelection(const manyforge::deployment::RetainedBundle &bundle) {
  const auto projection = Projection(bundle);
  const core::ActiveBundlePublicationReceiptDTO publication{
      core::ActiveBundlePublicationRequestDTO{
          core::NamedApplyPublicationTransactionDTO{
              core::ApplyId("assistant-authoring-test-apply"),
              core::ApplyGeneration(1U),
          },
          projection,
          projection.retained_bundle_id,
          projection.graph_revision,
          core::ActiveBundlePublicationGeneration(1U),
          core::ContentDigest(std::string(kDigestOne)),
      },
      core::ActiveBundlePublicationGeneration(2U),
      core::ContentDigest(std::string(kDigestThree)),
  };
  component_manager::RuntimeObservationReducer reducer([] { return 1U; }, {});
  return {
      component_manager::ManagerActiveRuntimePlanDTO{
          publication,
          core::ContentDigest(std::string(kDigestThree)),
          {},
      },
      reducer.Evaluate({}),
      component_manager::ManagerActiveRuntimeConvergenceState::kConverged,
  };
}

[[nodiscard]] core::LogicalSceneSnapshotRoot
LogicalScene(const manyforge::deployment::RetainedBundle &bundle) {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = "free_space";
  policy.revision = {
      1U,
      std::string(core::kCanonicalEmptyCollisionPolicyDigest),
  };
  return core::LogicalSceneSnapshotRoot(
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          core::LogicalSceneSnapshotDTO{
              Projection(bundle).scene_revision,
              bundle.value().cell_graph.cell_id,
              1U,
              1U,
              1U,
              std::move(policy),
              {},
              {},
          }));
}

} // namespace

int main(int argc, char **argv) {
  try {
    const auto publication_root = ParsePublicationRoot(argc, argv);
    const auto bundle =
        manyforge::headless::test::LoadAssistantAuthoringBundle();
    component_manager::SqliteManagerApplyJournal apply_journal(
        publication_root / "assistant-authoring-test-apply.sqlite3",
        bundle->value().cell_graph.cell_id);
    const auto payload = headless::BuildManagerSourcePayload(
        *bundle, RuntimeSelection(*bundle), apply_journal,
        headless::ManagerApplyMutationAvailability::kKernelOwnerUnavailable);
    const auto observed_at_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch())
            .count();
    if (observed_at_ns < 0) {
      throw std::runtime_error("monotonic clock returned a negative value");
    }
    headless::AtomicSourceSnapshotChannel manager_channel(
        publication_root, "manager-source.json");
    headless::AtomicSourceSnapshotChannel scene_channel(
        publication_root, "logical-scene-source.json");
    headless::AtomicSourceSnapshotChannel scene_runtime_channel(
        publication_root, "scene-runtime-source.json");
    static_cast<void>(manager_channel.Publish(
        "component_manager",
        "component-manager." + bundle->value().cell_graph.cell_id.value(),
        "component-manager.assistant-authoring-test.epoch.1", observed_at_ns,
        payload));
    const auto projection = Projection(*bundle);
    const auto scene = LogicalScene(*bundle);
    static_cast<void>(scene_channel.Publish(
        "logical_scene",
        "logical-scene." + bundle->value().cell_graph.cell_id.value(),
        "logical-scene.assistant-authoring-test.epoch.1", observed_at_ns,
        headless::BuildLogicalSceneSourcePayload(
            manyforge::scene::CanonicalEmptySceneAccessStatus{
                manyforge::scene::CanonicalEmptySceneBootstrapPhase::kFinalized,
                true,
                true,
                projection,
            },
            scene)));
    static_cast<void>(scene_runtime_channel.Publish(
        "scene_runtime",
        "scene-runtime." + bundle->value().cell_graph.cell_id.value(),
        "scene-runtime.assistant-authoring-test.epoch.1", observed_at_ns,
        headless::BuildSceneRuntimeSourcePayload(
            bundle->value().cell_graph.cell_id.value(),
            headless::SceneRuntimeStateSnapshot{
                1U,
                std::nullopt,
                headless::SceneInitializationState::kOpen,
                false,
                headless::SceneRuntimeIdentity{
                    bundle->retained_bundle_id().value(),
                    bundle->value().cell_graph.graph_revision.value(),
                    1U,
                    0U,
                    1U,
                },
                "assistant authoring test fixture",
                std::nullopt,
                observed_at_ns,
                observed_at_ns,
                std::nullopt,
                {},
                {},
            })));
    return 0;
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}

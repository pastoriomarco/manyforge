#include "manyforge_component_manager/manager_selected_runtime_composition_coordinator.hpp"

#include <limits>
#include <manyforge_core/types/typed_error.hpp>
#include <manyforge_deployment/content_identity.hpp>
#include <string>

namespace manyforge::component_manager {
namespace {

namespace core = manyforge::core;
namespace deployment = manyforge::deployment;

void Require(const bool condition, const std::string &detail) {
  if (!condition) {
    throw ManagerSelectedRuntimeCompositionCoordinatorError(detail);
  }
}

} // namespace

ManagerSelectedRuntimeCompositionCoordinator::
    ManagerSelectedRuntimeCompositionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        IManagerSelectedRuntimeCompositionPort &runtime_composition)
    : apply_journal_(apply_journal), runtime_composition_(runtime_composition) {}

HostApplyResult ManagerSelectedRuntimeCompositionCoordinator::ApplyOnHost(
    const core::ApplyId &apply_id, const core::ApplyGeneration apply_generation,
    const deployment::RetainedBundle &predecessor,
    const deployment::RetainedBundle &candidate) {
  std::lock_guard lock(operation_mutex_);
  const auto status = apply_journal_.ReadStatus(apply_id, apply_generation);
  Require(status && status->affected_scope && status->fence_acceptance &&
              status->latest_runtime_staged_ready &&
              !status->terminal_outcome && !status->private_materialization,
          "host Apply lacks its exact unpublished Apply, fence and staged host");
  const auto &intent = status->intent.intent;
  Require(status->latest_runtime_staged_ready->staged_ready.runtime_units.size() == 1U,
          "host Apply requires the sole staged execution host");
  Require(deployment::verify_retained_bundle_identity(predecessor) &&
              deployment::verify_retained_bundle_identity(candidate) &&
              predecessor.retained_bundle_id() ==
                  intent.expected_active_projection_revision.retained_bundle_id &&
              candidate.retained_bundle_id() == intent.candidate_retained_bundle_id &&
              candidate.value().cell_graph.graph_revision == intent.candidate_graph_revision &&
              candidate.value().cell_graph.cell_id == apply_journal_.cell_id(),
          "host Apply bundles differ from the durable Apply");
  const auto request = MakeHostApplyRequest(
      {apply_id, apply_generation}, predecessor, candidate,
      status->affected_scope->affected_scope.component_delta);
  Require(request.valid(), "host Apply request is invalid");
  const auto &runtime_unit =
      status->latest_runtime_staged_ready->staged_ready.runtime_units.front();
  auto result = runtime_composition_.ApplyOnHost(runtime_unit.runtime_unit_id, request);
  if (!result.ok()) throw core::TypedError(result.error(), std::string(kExecutionHostProducer));
  Require(result.value().valid(), "execution host returned an invalid Apply result");
  if (result.value().outcome == HostApplyOutcome::kFailed)
    throw core::TypedError(*result.value().error, std::string(kExecutionHostProducer));

  const auto bytes = candidate.canonical_bytes();
  Require(!bytes.empty() &&
              bytes.size() <= static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max()),
          "host Apply candidate byte extent is invalid");
  // The Scene revision recorded by the Apply is evidence of the request; the
  // Scene owner adopts the latest Scene.
  const auto &request_scene = intent.expected_active_projection_revision.scene_revision;
  const ManagerApplyPrivateMaterializationDTO materialization{
      request_scene.content_hash,
      apply_id,
      apply_generation,
      {intent.candidate_retained_bundle_id, intent.candidate_graph_revision, request_scene},
      core::ContentDigest(deployment::sha256_digest(bytes)),
      static_cast<std::uint64_t>(bytes.size()),
  };
  Require(materialization.valid(), "host Apply materialization is invalid");
  static_cast<void>(apply_journal_.EnsurePrivateMaterialization(materialization));
  return result.value();
}

} // namespace manyforge::component_manager

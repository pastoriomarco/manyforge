#include "manyforge_component_manager/manager_selected_bundle_transition_coordinator.hpp"

#include <algorithm>
#include <exception>
#include <limits>
#include <manyforge_deployment/content_identity.hpp>
#include <manyforge_deployment/native_execution_runtime_contract.hpp>
#include <memory>
#include <utility>

namespace manyforge::component_manager {
namespace {

using core::ActiveBundlePublicationGeneration;
using core::ActiveBundlePublicationReceiptDTO;
using core::ActiveBundlePublicationRequestDTO;
using core::ApplyGeneration;
using core::ApplyId;
using core::NamedApplyPublicationTransactionDTO;
using core::ProjectionApplyFinalizeDTO;
using core::ProjectionApplyFinalizeResultDTO;
using core::ProjectionApplyFinalizeResultState;
using core::ProjectionApplyStatusDTO;
using core::ProjectionApplyStatusQueryDTO;
using core::ProjectionApplyStatusState;
using core::ProjectionTransitionEvidenceState;
using deployment::RetainedBundle;

[[nodiscard]] bool SameDeploymentRevision(
    const core::ProjectionRevisionDTO &left,
    const core::ProjectionRevisionDTO &right) noexcept {
  return left.retained_bundle_id == right.retained_bundle_id &&
         left.graph_revision == right.graph_revision;
}

[[noreturn]] void Fail(const std::string &detail) {
  throw ManagerSelectedBundleTransitionCoordinatorError(detail);
}

[[nodiscard]] std::shared_ptr<const RetainedBundle>
LoadExactBundle(const deployment::IVerifiedRetainedBundleSource &source,
                const core::RetainedBundleId &retained_bundle_id) {
  try {
    auto bundle = source.get_verified(retained_bundle_id);
    if (bundle == nullptr ||
        !deployment::verify_retained_bundle_identity(*bundle) ||
        bundle->retained_bundle_id() != retained_bundle_id) {
      Fail("selected-bundle transition cannot reload the exact verified "
           "retained bundle");
    }
    return bundle;
  } catch (const ManagerSelectedBundleTransitionCoordinatorError &) {
    throw;
  } catch (const std::exception &error) {
    Fail("selected-bundle retained-bundle reload failed: " +
         std::string(error.what()));
  } catch (...) {
    Fail("selected-bundle retained-bundle reload failed");
  }
}

void VerifyBundleBindings(const ManagerApplyIntentDTO &intent,
                          const ManagerApplySceneActivatedDTO &activated,
                          const RetainedBundle &predecessor,
                          const RetainedBundle &candidate) {
  const auto &predecessor_graph = predecessor.value().cell_graph;
  const auto &candidate_graph = candidate.value().cell_graph;
  const auto &activation = activated.activation_result;
  if (intent.cell_id != predecessor_graph.cell_id ||
      intent.cell_id != candidate_graph.cell_id ||
      intent.expected_active_projection_revision.retained_bundle_id !=
          predecessor.retained_bundle_id() ||
      intent.expected_active_projection_revision.graph_revision !=
          predecessor_graph.graph_revision ||
      intent.candidate_retained_bundle_id != candidate.retained_bundle_id() ||
      intent.candidate_graph_revision != candidate_graph.graph_revision ||
      activation.expected_active_projection_revision !=
          intent.expected_active_projection_revision ||
      activation.candidate_projection_revision.retained_bundle_id !=
          candidate.retained_bundle_id() ||
      activation.candidate_projection_revision.graph_revision !=
          candidate_graph.graph_revision) {
    Fail("selected-bundle retained bundles differ from the durable Apply "
         "and activation identities");
  }
}

template <typename Row, typename IdAccessor>
[[nodiscard]] const Row *FindById(const std::vector<Row> &rows,
                                  const std::string &identity,
                                  IdAccessor id_accessor) {
  const auto found =
      std::find_if(rows.begin(), rows.end(), [&](const auto &row) {
        return id_accessor(row).value() == identity;
      });
  return found == rows.end() ? nullptr : &*found;
}

std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO>
ObserveObsoleteManagedCommandSurfaces(
    const RetainedBundle &predecessor, const RetainedBundle &candidate,
    const deployment::IVerifiedRetainedProductArtifactSource *artifact_source,
    const PersistedManagerApplyRuntimeStagedReady *staged_ready,
    const core::ContentDigest & /*expected_staged_ready_digest*/) {
  std::vector<ObsoleteManagedCommandSurfaceEvidenceDTO> result;
  const auto &predecessor_units = predecessor.value().cell_graph.runtime_units;
  const auto &candidate_units = candidate.value().cell_graph.runtime_units;
  const auto &predecessor_products =
      predecessor.value().compiled_products.runtime_unit_products;
  const auto &candidate_products =
      candidate.value().compiled_products.runtime_unit_products;
  for (const auto &predecessor_unit : predecessor_units) {
    const auto runtime_id = predecessor_unit.runtime_unit_id.value();
    const auto *candidate_unit =
        FindById(candidate_units, runtime_id,
                 [](const auto &row) -> const core::RuntimeUnitId & {
                   return row.runtime_unit_id;
                 });
    const auto *predecessor_product =
        FindById(predecessor_products, runtime_id,
                 [](const auto &row) -> const core::RuntimeUnitId & {
                   return row.runtime_unit_id;
                 });
    const auto *candidate_product =
        FindById(candidate_products, runtime_id,
                 [](const auto &row) -> const core::RuntimeUnitId & {
                   return row.runtime_unit_id;
                 });
    if (candidate_unit == nullptr || predecessor_product == nullptr ||
        candidate_product == nullptr) {
      Fail("obsolete or replaced managed runtime command surfaces require a "
           "qualified retirement adapter before selection");
    }
    if (*candidate_unit == predecessor_unit &&
        *candidate_product == *predecessor_product)
      continue;
    if (!artifact_source || !staged_ready)
      Fail("native command-surface retirement lacks exact candidate readiness");
    // The scene-stage intent remains bound to the original staged-ready
    // digest.  A Manager restart may append a newer staged-ready observation
    // for the recreated child epoch; the journal already verifies that this
    // latest row belongs to the same Apply/runtime-convergence intent.
    const auto old = deployment::verify_runtime_unit_product(
        predecessor, predecessor_unit.runtime_unit_id, *artifact_source);
    const auto next = deployment::verify_runtime_unit_product(
        candidate, candidate_unit->runtime_unit_id, *artifact_source);
    constexpr auto native_adapter = deployment::
        native_execution_runtime_contract::kProcessAdapterContractId;
    if (!old || !next || old->process_adapter_contract_id() != native_adapter ||
        next->process_adapter_contract_id() != native_adapter ||
        old->product_contract_id() != next->product_contract_id() ||
        old->product_contract_revision() != next->product_contract_revision())
      Fail("runtime command-surface replacement has no qualified native "
           "retirement contract");
    if (old->product_identity() == next->product_identity() &&
        old->configuration_identity() == next->configuration_identity())
      continue;
    const auto *observed =
        FindById(staged_ready->staged_ready.runtime_units, runtime_id,
                 [](const auto &row) -> const core::RuntimeUnitId & {
                   return row.runtime_unit_id;
                 });
    if (!observed || !observed->valid() ||
        observed->lifecycle_control !=
            core::RuntimeLifecycleControl::kManagerManaged ||
        observed->observed_product_identity != next->product_identity() ||
        observed->observed_configuration_identity !=
            next->configuration_identity())
      Fail("native replacement readiness does not attest the exact candidate "
           "product");
    // This qualified adapter has one direct child per runtime unit and reaps
    // its predecessor before starting a different product. The successor's
    // source epoch therefore proves the old product is no longer selectable;
    // it is not a fabricated final report from the retired process.
    result.push_back(
        {predecessor_unit.runtime_unit_id, observed->runtime_epoch,
         old->product_identity(), old->configuration_identity(),
         observed->source_report_sequence,
         ObsoleteManagedCommandSurfaceDisposition::kNonselectable});
  }
  return result;
}

[[nodiscard]] bool ExactPredecessorPublication(
    const ActiveBundlePublicationReceiptDTO &receipt,
    const ManagerApplySceneStageIntentDTO &stage_intent) {
  return receipt.valid() &&
         receipt.publication_generation ==
             stage_intent.predecessor_publication_generation &&
         receipt.durable_public_pointer_content_digest ==
             stage_intent.predecessor_public_pointer_digest &&
         SameDeploymentRevision(
             receipt.request.candidate_projection_revision,
             stage_intent.expected_active_projection_revision) &&
         receipt.request.candidate_retained_bundle_id ==
             stage_intent.expected_active_projection_revision
                 .retained_bundle_id &&
         receipt.request.candidate_graph_revision ==
             stage_intent.expected_active_projection_revision.graph_revision;
}

[[nodiscard]] bool
ExactPublication(const ActiveBundlePublicationReceiptDTO &receipt,
                 const ActiveBundlePublicationRequestDTO &request,
                 const ActiveBundlePublicationGeneration expected_generation) {
  return receipt.valid() && receipt.request == request &&
         receipt.publication_generation == expected_generation;
}

[[nodiscard]] bool
TransitionRowsMatch(const core::ProjectionTransitionEvidenceDTO &left,
                    const core::ProjectionTransitionEvidenceDTO &right) {
  if (left.transition_rows.size() != right.transition_rows.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < left.transition_rows.size(); ++index) {
    if (left.transition_rows[index].transition_row !=
        right.transition_rows[index].transition_row) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] bool
ExactFinalizedResult(const ProjectionApplyFinalizeResultDTO &result,
                     const ManagerApplySceneActivatedDTO &activated,
                     const ActiveBundlePublicationReceiptDTO &publication) {
  const auto &activation = activated.activation_result;
  return result.state == ProjectionApplyFinalizeResultState::kFinalized &&
         result.presence_valid() && result.apply_id == activation.apply_id &&
         result.apply_generation == activation.apply_generation &&
         result.projection_stage_generation ==
             activation.projection_stage_generation &&
         result.candidate_projection_revision ==
             activation.candidate_projection_revision &&
         result.publication_receipt == publication &&
         result.acknowledged_candidate_projection_revision ==
             std::optional<core::ProjectionRevisionDTO>{
                 activation.candidate_projection_revision} &&
         result.transition_evidence.state ==
             ProjectionTransitionEvidenceState::kFinalized &&
         TransitionRowsMatch(result.transition_evidence,
                             activation.transition_evidence);
}

[[nodiscard]] bool ExactActivatedOrPublishedStatus(
    const ProjectionApplyStatusDTO &status,
    const ManagerApplySceneActivatedDTO &activated,
    const ActiveBundlePublicationReceiptDTO &publication) {
  const auto &activation = activated.activation_result;
  const bool phase_valid =
      status.state == ProjectionApplyStatusState::kActivated ||
      (status.state == ProjectionApplyStatusState::kPublished &&
       status.publication_receipt ==
           std::optional<ActiveBundlePublicationReceiptDTO>{publication});
  return phase_valid && status.presence_valid() &&
         status.apply_id == activation.apply_id &&
         status.apply_generation == activation.apply_generation &&
         status.predecessor_projection_revision ==
             std::optional<core::ProjectionRevisionDTO>{
                 activation.expected_active_projection_revision} &&
         status.projection_stage_generation ==
             std::optional<core::ProjectionStageGeneration>{
                 activation.projection_stage_generation} &&
         status.candidate_projection_revision ==
             std::optional<core::ProjectionRevisionDTO>{
                 activation.candidate_projection_revision} &&
         status.transition_evidence.has_value() &&
         TransitionRowsMatch(*status.transition_evidence,
                             activation.transition_evidence);
}

[[nodiscard]] ProjectionApplyFinalizeResultDTO FinalizedResultFromStatus(
    const ProjectionApplyStatusDTO &status,
    const ManagerApplySceneActivatedDTO &activated,
    const ActiveBundlePublicationReceiptDTO &publication) {
  if (status.state != ProjectionApplyStatusState::kFinalized ||
      !status.presence_valid() ||
      !status.projection_stage_generation.has_value() ||
      !status.candidate_projection_revision.has_value() ||
      !status.transition_evidence.has_value() ||
      !status.publication_receipt.has_value() ||
      !status.candidate_aggregate_health.has_value()) {
    Fail("scene owner does not expose complete finalized recovery evidence");
  }
  ProjectionApplyFinalizeResultDTO result{
      status.apply_id,
      status.apply_generation,
      *status.projection_stage_generation,
      *status.candidate_projection_revision,
      *status.publication_receipt,
      ProjectionApplyFinalizeResultState::kFinalized,
      status.candidate_projection_revision,
      *status.transition_evidence,
      *status.candidate_aggregate_health,
      std::nullopt,
  };
  if (!ExactFinalizedResult(result, activated, publication)) {
    Fail("scene owner finalized status differs from the selected Apply");
  }
  return result;
}

} // namespace

ManagerSelectedBundleTransitionEffectUncertainError::
    ManagerSelectedBundleTransitionEffectUncertainError(
        std::string detail,
        ActiveBundlePublicationRequestDTO publication_request,
        ProjectionApplyStatusQueryDTO scene_status_query,
        std::optional<ActiveBundlePublicationReceiptDTO> observed_publication,
        std::optional<ProjectionApplyStatusDTO> observed_scene_status,
        std::optional<ProjectionApplyFinalizeResultDTO>
            observed_finalize_result)
    : ManagerSelectedBundleTransitionCoordinatorError(std::move(detail)),
      publication_request_(std::move(publication_request)),
      scene_status_query_(std::move(scene_status_query)),
      observed_publication_(std::move(observed_publication)),
      observed_scene_status_(std::move(observed_scene_status)),
      observed_finalize_result_(std::move(observed_finalize_result)) {}

const ActiveBundlePublicationRequestDTO &
ManagerSelectedBundleTransitionEffectUncertainError::publication_request()
    const noexcept {
  return publication_request_;
}

const ProjectionApplyStatusQueryDTO &
ManagerSelectedBundleTransitionEffectUncertainError::scene_status_query()
    const noexcept {
  return scene_status_query_;
}

const std::optional<ActiveBundlePublicationReceiptDTO> &
ManagerSelectedBundleTransitionEffectUncertainError::observed_publication()
    const noexcept {
  return observed_publication_;
}

const std::optional<ProjectionApplyStatusDTO> &
ManagerSelectedBundleTransitionEffectUncertainError::observed_scene_status()
    const noexcept {
  return observed_scene_status_;
}

const std::optional<ProjectionApplyFinalizeResultDTO> &
ManagerSelectedBundleTransitionEffectUncertainError::observed_finalize_result()
    const noexcept {
  return observed_finalize_result_;
}

ManagerSelectedBundleTransitionCoordinator::
    ManagerSelectedBundleTransitionCoordinator(
        SqliteManagerApplyJournal &apply_journal,
        const deployment::IVerifiedRetainedBundleSource &retained_bundle_source,
        core::IActiveBundlePublicationStore &publication_store,
        core::IManagerSceneProjectionHandoffPort &scene_owner,
        const deployment::IVerifiedRetainedProductArtifactSource
            *artifact_source)
    : apply_journal_(apply_journal),
      retained_bundle_source_(retained_bundle_source),
      publication_store_(publication_store), scene_owner_(scene_owner),
      artifact_source_(artifact_source) {}

ManagerSelectedBundleTransitionResult
ManagerSelectedBundleTransitionCoordinator::AdvanceOrRecover(
    const ApplyId &apply_id, const ApplyGeneration apply_generation) {
  std::lock_guard operation_lock(operation_mutex_);
  const auto apply = apply_journal_.ReadIntent(apply_id, apply_generation);
  const auto stage_intent =
      apply_journal_.ReadSceneStageIntent(apply_id, apply_generation);
  const auto activated =
      apply_journal_.ReadSceneActivated(apply_id, apply_generation);
  if (!apply.has_value() || !stage_intent.has_value() ||
      !activated.has_value()) {
    Fail("selected-bundle transition requires exact durable Apply, scene "
         "stage, and activation records");
  }

  auto predecessor = LoadExactBundle(
      retained_bundle_source_,
      apply->intent.expected_active_projection_revision.retained_bundle_id);
  auto candidate = LoadExactBundle(retained_bundle_source_,
                                   apply->intent.candidate_retained_bundle_id);
  VerifyBundleBindings(apply->intent, activated->scene_activated, *predecessor,
                       *candidate);
  const auto staged_ready =
      apply_journal_.ReadLatestRuntimeStagedReady(apply_id, apply_generation);
  auto retired_surfaces = ObserveObsoleteManagedCommandSurfaces(
      *predecessor, *candidate, artifact_source_,
      staged_ready ? &*staged_ready : nullptr,
      stage_intent->scene_stage_intent.runtime_staged_ready_digest);

  const auto canonical_bytes = candidate->canonical_bytes();
  if (canonical_bytes.empty() ||
      canonical_bytes.size() >
          static_cast<std::size_t>(std::numeric_limits<std::uint64_t>::max())) {
    Fail("selected retained bundle has an invalid canonical byte extent");
  }
  const ManagerApplyPrivateMaterializationDTO expected_materialization{
      activated->scene_activated_digest,
      apply_id,
      apply_generation,
      activated->scene_activated.activation_result.projection_stage_generation,
      activated->scene_activated.activation_result
          .candidate_projection_revision,
      core::ContentDigest(deployment::sha256_digest(canonical_bytes)),
      static_cast<std::uint64_t>(canonical_bytes.size()),
  };
  const auto durable_materialization =
      apply_journal_.ReadPrivateMaterialization(apply_id, apply_generation);
  if (!durable_materialization.has_value() ||
      durable_materialization->private_materialization !=
          expected_materialization) {
    Fail("selected runtime composition is not durably materialized");
  }
  const ManagerApplyPrivateMaterializationWriteResult materialization{
      *durable_materialization, false};
  const auto retired = apply_journal_.EnsureObsoleteCommandSurfacesRetired({
      materialization.record.private_materialization_content_digest,
      apply_id,
      apply_generation,
      std::move(retired_surfaces),
  });

  const auto &activation = activated->scene_activated.activation_result;
  const ActiveBundlePublicationRequestDTO publication_request{
      NamedApplyPublicationTransactionDTO{
          apply_id,
          apply_generation,
          activation.projection_stage_generation,
      },
      activation.candidate_projection_revision,
      activation.candidate_projection_revision.retained_bundle_id,
      activation.candidate_projection_revision.graph_revision,
      stage_intent->scene_stage_intent.predecessor_publication_generation,
      materialization.record.private_materialization_content_digest,
      retired.record.obsolete_managed_command_surfaces_retired_evidence_digest,
  };
  if (!publication_request.valid()) {
    Fail("durable selected-bundle records reconstruct an invalid publication "
         "request");
  }
  const auto previous_generation =
      stage_intent->scene_stage_intent.predecessor_publication_generation
          .value();
  if (previous_generation == std::numeric_limits<std::uint64_t>::max()) {
    Fail("active-bundle publication generation is exhausted");
  }
  const ActiveBundlePublicationGeneration expected_generation(
      previous_generation + 1U);
  const ProjectionApplyStatusQueryDTO status_query{apply_id, apply_generation};

  const auto read_current = [&](const ActiveBundlePublicationGeneration value) {
    try {
      return publication_store_.ReadActiveBundlePublication(value);
    } catch (const std::exception &error) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "active-bundle current-head read failed: " +
              std::string(error.what()),
          publication_request, status_query);
    } catch (...) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "active-bundle current-head read failed", publication_request,
          status_query);
    }
  };

  auto publication_path =
      ManagerSelectedBundleTransitionPath::kPublishedFromNewCall;
  ManagerApplyPublicationReceiptWriteResult durable_publication = [&] {
    if (const auto existing =
            apply_journal_.ReadPublicationReceipt(apply_id, apply_generation);
        existing.has_value()) {
      if (existing->publication.publication_receipt.request !=
              publication_request ||
          existing->publication.publication_receipt.publication_generation !=
              expected_generation) {
        Fail("durable Apply publication differs from selected-bundle "
             "records");
      }
      return ManagerApplyPublicationReceiptWriteResult{*existing, false};
    }

    const auto possible_success = read_current(expected_generation);
    if (possible_success.ok()) {
      if (!ExactPublication(possible_success.value(), publication_request,
                            expected_generation)) {
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "current publication generation belongs to a different "
            "transaction",
            publication_request, status_query, possible_success.value());
      }
      publication_path =
          ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead;
      try {
        return apply_journal_.EnsurePublicationReceipt(
            {retired.record
                 .obsolete_managed_command_surfaces_retired_evidence_digest,
             possible_success.value()});
      } catch (const std::exception &error) {
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "recovered publication could not be made durable: " +
                std::string(error.what()),
            publication_request, status_query, possible_success.value());
      }
    }

    const auto predecessor_head = read_current(
        stage_intent->scene_stage_intent.predecessor_publication_generation);
    if (!predecessor_head.ok() ||
        !ExactPredecessorPublication(predecessor_head.value(),
                                     stage_intent->scene_stage_intent)) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "neither the expected predecessor nor selected publication is the "
          "current durable head",
          publication_request, status_query,
          predecessor_head.ok()
              ? std::optional<
                    ActiveBundlePublicationReceiptDTO>{predecessor_head.value()}
              : std::nullopt);
    }

    ActiveBundlePublicationReceiptDTO receipt = [&] {
      try {
        return publication_store_.PublishActiveBundle(publication_request);
      } catch (const std::exception &error) {
        const auto recovered = read_current(expected_generation);
        if (recovered.ok() &&
            ExactPublication(recovered.value(), publication_request,
                             expected_generation)) {
          publication_path =
              ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead;
          return recovered.value();
        }
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "active-bundle publication outcome is uncertain: " +
                std::string(error.what()),
            publication_request, status_query,
            recovered.ok()
                ? std::optional<ActiveBundlePublicationReceiptDTO>{recovered
                                                                       .value()}
                : std::nullopt);
      } catch (...) {
        const auto recovered = read_current(expected_generation);
        if (recovered.ok() &&
            ExactPublication(recovered.value(), publication_request,
                             expected_generation)) {
          publication_path =
              ManagerSelectedBundleTransitionPath::kRecoveredPublicationHead;
          return recovered.value();
        }
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "active-bundle publication outcome is uncertain",
            publication_request, status_query,
            recovered.ok()
                ? std::optional<ActiveBundlePublicationReceiptDTO>{recovered
                                                                       .value()}
                : std::nullopt);
      }
    }();
    if (!ExactPublication(receipt, publication_request, expected_generation)) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "publication store returned malformed or foreign evidence",
          publication_request, status_query, receipt);
    }
    try {
      return apply_journal_.EnsurePublicationReceipt(
          {retired.record
               .obsolete_managed_command_surfaces_retired_evidence_digest,
           receipt});
    } catch (const std::exception &error) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "publication receipt could not be made durable: " +
              std::string(error.what()),
          publication_request, status_query, receipt);
    }
  }();

  if (const auto finalized =
          apply_journal_.ReadSceneFinalized(apply_id, apply_generation);
      finalized.has_value()) {
    return {
        materialization,
        retired,
        durable_publication,
        ManagerApplySceneFinalizedWriteResult{*finalized, false},
        ManagerSelectedBundleTransitionPath::kDurableFinalizedReplay,
    };
  }

  if (durable_publication.inserted) {
    return {
        materialization, retired,          durable_publication,
        std::nullopt,    publication_path,
    };
  }

  const auto current = read_current(expected_generation);
  if (!current.ok() ||
      !ExactPublication(current.value(), publication_request,
                        expected_generation) ||
      current.value() !=
          durable_publication.record.publication.publication_receipt) {
    throw ManagerSelectedBundleTransitionEffectUncertainError(
        "durable Apply publication is not the current public head",
        publication_request, status_query,
        current.ok()
            ? std::optional<ActiveBundlePublicationReceiptDTO>{current.value()}
            : std::nullopt);
  }

  ProjectionApplyStatusDTO status = [&] {
    try {
      auto observed = scene_owner_.ProjectionApplyStatus(status_query);
      if (!observed.presence_valid() || observed.apply_id != apply_id ||
          observed.apply_generation != apply_generation) {
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "scene owner returned malformed or foreign Finalize status",
            publication_request, status_query, current.value(), observed);
      }
      return observed;
    } catch (const ManagerSelectedBundleTransitionEffectUncertainError &) {
      throw;
    } catch (const std::exception &error) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "scene Finalize status query failed: " + std::string(error.what()),
          publication_request, status_query, current.value());
    } catch (...) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "scene Finalize status query failed", publication_request,
          status_query, current.value());
    }
  }();

  const auto persist_finalized =
      [&](const ProjectionApplyFinalizeResultDTO &result,
          const ManagerSelectedBundleTransitionPath path) {
        if (!ExactFinalizedResult(
                result, activated->scene_activated,
                durable_publication.record.publication.publication_receipt)) {
          throw ManagerSelectedBundleTransitionEffectUncertainError(
              "scene owner returned malformed or foreign Finalize evidence",
              publication_request, status_query, current.value(), status,
              result);
        }
        try {
          auto durable = apply_journal_.EnsureSceneFinalized({
              durable_publication.record.publication_receipt_digest,
              result,
          });
          return ManagerSelectedBundleTransitionResult{
              materialization, retired, durable_publication, durable, path,
          };
        } catch (const std::exception &error) {
          throw ManagerSelectedBundleTransitionEffectUncertainError(
              "scene Finalize evidence could not be made durable: " +
                  std::string(error.what()),
              publication_request, status_query, current.value(), status,
              result);
        }
      };

  if (status.state == ProjectionApplyStatusState::kFinalized) {
    ProjectionApplyFinalizeResultDTO recovered = [&] {
      try {
        return FinalizedResultFromStatus(
            status, activated->scene_activated,
            durable_publication.record.publication.publication_receipt);
      } catch (const std::exception &error) {
        throw ManagerSelectedBundleTransitionEffectUncertainError(
            "scene finalized status cannot reconstruct exact evidence: " +
                std::string(error.what()),
            publication_request, status_query, current.value(), status);
      }
    }();
    return persist_finalized(
        recovered,
        ManagerSelectedBundleTransitionPath::kRecoveredFinalizedOwnerStatus);
  }
  if (!ExactActivatedOrPublishedStatus(
          status, activated->scene_activated,
          durable_publication.record.publication.publication_receipt)) {
    throw ManagerSelectedBundleTransitionEffectUncertainError(
        "scene owner is not exactly activated, published, or finalized for "
        "the selected Apply",
        publication_request, status_query, current.value(), status);
  }

  const ProjectionApplyFinalizeDTO finalize_request{
      apply_id,
      apply_generation,
      activation.projection_stage_generation,
      activation.candidate_projection_revision,
      durable_publication.record.publication.publication_receipt,
  };
  ProjectionApplyFinalizeResultDTO finalize_result = [&] {
    try {
      return scene_owner_.FinalizeProjectionTransition(finalize_request);
    } catch (const std::exception &error) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "scene Finalize outcome is uncertain: " + std::string(error.what()),
          publication_request, status_query, current.value(), status);
    } catch (...) {
      throw ManagerSelectedBundleTransitionEffectUncertainError(
          "scene Finalize outcome is uncertain", publication_request,
          status_query, current.value(), status);
    }
  }();
  return persist_finalized(
      finalize_result,
      ManagerSelectedBundleTransitionPath::kFinalizedFromNewCall);
}

} // namespace manyforge::component_manager

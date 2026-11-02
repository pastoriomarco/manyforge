#include "manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <array>
#include <cstdint>
#include <cstddef>
#include <exception>
#include <span>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

using manyforge::core::BootstrapGeneration;
using manyforge::core::BootstrapId;
using manyforge::core::ActiveBundlePublicationGeneration;
using manyforge::core::ActiveBundlePublicationReceiptDTO;
using manyforge::core::ActiveBundlePublicationRequestDTO;
using manyforge::core::BootstrapPublicationTransactionDTO;
using manyforge::core::ContentDigest;
using manyforge::core::ProjectionBootstrapAcceptedDTO;
using manyforge::core::ProjectionBootstrapEvidenceDTO;
using manyforge::core::ProjectionBootstrapFinalizeDTO;
using manyforge::core::ProjectionBootstrapFinalizeResultDTO;
using manyforge::core::ProjectionBootstrapFinalizeResultState;
using manyforge::core::ProjectionBootstrapIntentDTO;
using manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO;
using manyforge::core::ProjectionBootstrapRebuildRequestDTO;
using manyforge::core::ProjectionBootstrapRebuildResultDTO;
using manyforge::core::ProjectionBootstrapRebuildResultState;
using manyforge::core::ProjectionBootstrapStatusDTO;
using manyforge::core::ProjectionBootstrapStatusQueryDTO;
using manyforge::core::ProjectionBootstrapStatusState;

inline constexpr std::size_t kBootstrapRandomBytes = 16U;
inline constexpr std::string_view kBootstrapIdPrefix = "bootstrap-";
inline constexpr std::string_view kMaterializationDigestDomain =
    "manyforge.component-manager.canonical-empty."
    "private-materialization.v1";

[[noreturn]] void Fail(const std::string& detail) {
  throw CanonicalEmptyBootstrapCoordinatorError(detail);
}

bool HasCoordinatorBootstrapIdShape(const BootstrapId& bootstrap_id) {
  const auto& value = bootstrap_id.value();
  if (value.size() !=
          kBootstrapIdPrefix.size() + (kBootstrapRandomBytes * 2U) ||
      value.compare(0U, kBootstrapIdPrefix.size(), kBootstrapIdPrefix) != 0) {
    return false;
  }
  for (auto index = kBootstrapIdPrefix.size(); index < value.size(); ++index) {
    const char character = value[index];
    if (!((character >= '0' && character <= '9') ||
          (character >= 'a' && character <= 'f'))) {
      return false;
    }
  }
  return true;
}

BootstrapId MintBootstrapId() {
  static_assert(kBootstrapRandomBytes * 8U >= 128U);
  std::array<unsigned char, kBootstrapRandomBytes> random_bytes{};
  if (RAND_bytes(
          random_bytes.data(),
          static_cast<int>(random_bytes.size())) != 1) {
    Fail("cryptographic bootstrap identity generation failed");
  }

  static constexpr char kHex[] = "0123456789abcdef";
  std::string value(kBootstrapIdPrefix);
  value.reserve(kBootstrapIdPrefix.size() + (random_bytes.size() * 2U));
  for (const auto byte : random_bytes) {
    value.push_back(kHex[(byte >> 4U) & 0x0fU]);
    value.push_back(kHex[byte & 0x0fU]);
  }
  return BootstrapId(std::move(value));
}

void AppendSemanticField(
    std::string& record,
    const std::string_view name,
    const std::string_view value) {
  record.append(std::to_string(name.size()));
  record.push_back(':');
  record.append(name);
  record.push_back('=');
  record.append(std::to_string(value.size()));
  record.push_back(':');
  record.append(value);
  record.push_back('\n');
}

std::string Hex(const std::span<const unsigned char> bytes) {
  constexpr std::array<char, 16> kHex{
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f',
  };
  std::string result;
  result.reserve(bytes.size() * 2U);
  for (const auto byte : bytes) {
    result.push_back(kHex[byte >> 4U]);
    result.push_back(kHex[byte & 0x0fU]);
  }
  return result;
}

ContentDigest SemanticDigest(const std::string_view record) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> output{};
  unsigned int output_length = 0U;
  auto* context = EVP_MD_CTX_new();
  if (context == nullptr) {
    Fail("cannot allocate canonical-empty publication digest context");
  }
  const auto release = [&]() { EVP_MD_CTX_free(context); };
  if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1 ||
      EVP_DigestUpdate(context, record.data(), record.size()) != 1 ||
      EVP_DigestFinal_ex(context, output.data(), &output_length) != 1) {
    release();
    Fail("canonical-empty publication digest computation failed");
  }
  release();
  return ContentDigest(
      "sha256:" +
      Hex(std::span<const unsigned char>(output.data(), output_length)));
}

ContentDigest BootstrapSemanticDigest(
    const std::string_view domain,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const ManagerBootstrapIntentDTO& intent,
    const core::ProjectionRevisionDTO& target,
    const std::string_view semantic_count_name) {
  std::string record;
  record.reserve(1024U);
  AppendSemanticField(record, "domain", domain);
  AppendSemanticField(
      record, "admission_digest", admission.admission_digest().value());
  AppendSemanticField(record, "cell_id", admission.cell_id().value());
  AppendSemanticField(
      record, "retained_bundle_id", admission.retained_bundle_id().value());
  AppendSemanticField(
      record, "graph_revision", admission.graph_revision().value());
  AppendSemanticField(
      record, "bootstrap_id", intent.bootstrap_id.value());
  AppendSemanticField(
      record,
      "bootstrap_generation",
      std::to_string(intent.bootstrap_generation.value()));
  AppendSemanticField(
      record, "target_retained_bundle_id",
      target.retained_bundle_id.value());
  AppendSemanticField(
      record, "target_graph_revision", target.graph_revision.value());
  AppendSemanticField(
      record, "target_scene_id", target.scene_revision.scene_id.value());
  AppendSemanticField(
      record,
      "target_scene_revision",
      std::to_string(target.scene_revision.revision_id));
  AppendSemanticField(
      record,
      "target_scene_content_digest",
      target.scene_revision.content_hash.value());
  AppendSemanticField(record, semantic_count_name, "0");
  return SemanticDigest(record);
}

ProjectionBootstrapPrerequisiteQueryDTO AdmissionQuery(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  ProjectionBootstrapPrerequisiteQueryDTO query{
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
  };
  if (!query.valid()) {
    Fail("canonical-empty admission produced an invalid prerequisite query");
  }
  return query;
}

void VerifyAdmission(
    const bool store_admission_matches,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  const auto& bundle = admission.retained_bundle();
  const auto& value = bundle.value();
  if (admission.api_version() !=
          deployment::kCanonicalEmptyAdmissionApiVersion ||
      admission.admission_digest().empty() ||
      admission.canonical_bytes().empty() ||
      !store_admission_matches ||
      !deployment::verify_retained_bundle_identity(bundle) ||
      bundle.retained_bundle_id() != admission.retained_bundle_id() ||
      value.retained_bundle_id != admission.retained_bundle_id() ||
      value.cell_graph.cell_id != admission.cell_id() ||
      value.cell_graph.graph_revision != admission.graph_revision()) {
    Fail(
        "canonical-empty admission does not exactly match the manager store "
        "identity");
  }
}

ManagerBootstrapIntentDTO ManagerIntent(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const BootstrapId& bootstrap_id) {
  ManagerBootstrapIntentDTO intent{
      bootstrap_id,
      BootstrapGeneration(1U),
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
      {},
  };
  if (!intent.valid() || !HasCoordinatorBootstrapIdShape(bootstrap_id)) {
    Fail("canonical-empty manager bootstrap intent is invalid");
  }
  return intent;
}

ProjectionBootstrapIntentDTO SceneIntent(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const ManagerBootstrapIntentDTO& manager_intent) {
  ProjectionBootstrapIntentDTO intent{
      manager_intent.bootstrap_id,
      manager_intent.bootstrap_generation,
      manager_intent.cell_id,
      admission.retained_bundle().value(),
      {},
  };
  if (!intent.valid() || !intent.execution_host_bindings.empty()) {
    Fail("canonical-empty scene bootstrap intent is invalid");
  }
  return intent;
}

bool IntentExactlyMatchesAdmission(
    const ManagerBootstrapIntentDTO& intent,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  return intent.valid() &&
         HasCoordinatorBootstrapIdShape(intent.bootstrap_id) &&
         intent.bootstrap_generation == BootstrapGeneration(1U) &&
         intent.cell_id == admission.cell_id() &&
         intent.canonical_empty_retained_bundle_id ==
             admission.retained_bundle_id() &&
         intent.canonical_empty_graph_revision ==
             admission.graph_revision() &&
         intent.execution_host_bindings.empty();
}

bool TargetExactlyMatches(
    const ManagerBootstrapAcceptedTargetDTO& target,
    const ManagerBootstrapIntentDTO& intent) {
  return target.valid() &&
         target.bootstrap_id == intent.bootstrap_id &&
         target.bootstrap_generation == intent.bootstrap_generation &&
         target.canonical_empty_target.retained_bundle_id ==
             intent.canonical_empty_retained_bundle_id &&
         target.canonical_empty_target.graph_revision ==
             intent.canonical_empty_graph_revision;
}

ActiveBundlePublicationRequestDTO PublicationRequest(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  ActiveBundlePublicationRequestDTO request{
      BootstrapPublicationTransactionDTO{
          intent.bootstrap_id,
          intent.bootstrap_generation,
      },
      target.canonical_empty_target,
      intent.canonical_empty_retained_bundle_id,
      intent.canonical_empty_graph_revision,
      std::nullopt,
      BootstrapSemanticDigest(
          kMaterializationDigestDomain,
          admission,
          intent,
          target.canonical_empty_target,
          "native_materialization_count"),
  };
  if (!request.valid()) {
    Fail("canonical-empty publication request is invalid");
  }
  return request;
}

bool IsExactGenerationOneReceipt(
    const ActiveBundlePublicationReceiptDTO& receipt,
    const ActiveBundlePublicationRequestDTO& request) {
  return receipt.valid() &&
         receipt.publication_generation ==
             ActiveBundlePublicationGeneration(1U) &&
         receipt.request == request;
}

std::optional<ActiveBundlePublicationReceiptDTO> ReadPublicationHead(
    ExternallyProvisionedManagerStores& stores,
    const ActiveBundlePublicationRequestDTO& expected_request) {
  const auto inventory =
      stores.active_bundle_publication_store().VerifyIntegrity();
  if (inventory.cell_id != stores.cell_id()) {
    Fail("active-bundle publication store changed cell identity");
  }
  if (inventory.receipt_count == 0 &&
      inventory.current_generation.value() == 0U) {
    return std::nullopt;
  }
  if (inventory.receipt_count != 1 ||
      inventory.current_generation !=
          ActiveBundlePublicationGeneration(1U)) {
    Fail(
        "canonical-empty bootstrap requires an empty or exact generation-1 "
        "publication head");
  }
  const auto read =
      stores.active_bundle_publication_store().ReadActiveBundlePublication(
          inventory.current_generation);
  if (!read.ok() ||
      !IsExactGenerationOneReceipt(read.value(), expected_request)) {
    Fail(
        "canonical-empty publication head does not exactly match the "
        "durable accepted target");
  }
  return read.value();
}

bool ZeroNativeEvidenceExactlyMatches(
    const ProjectionBootstrapEvidenceDTO& evidence,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  return intent.execution_host_bindings.empty() &&
         evidence.matches_bootstrap(
             intent.bootstrap_id,
             intent.bootstrap_generation,
             target.canonical_empty_target) &&
         evidence.rebuild_results.empty() &&
         evidence.candidate_aggregate_health.native_projection_health
             .empty();
}

void RequireExactZeroNativeEvidence(
    const ProjectionBootstrapEvidenceDTO& evidence,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  if (!ZeroNativeEvidenceExactlyMatches(evidence, intent, target)) {
    Fail(
        "scene owner did not provide exact healthy zero-native bootstrap "
        "evidence");
  }
}

void RequireExactZeroNativeFinalHealth(
    const core::ProjectionAggregateHealthDTO& health,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target,
    const ProjectionBootstrapEvidenceDTO& evidence) {
  if (!health.matches_bootstrap(
          intent.bootstrap_id,
          intent.bootstrap_generation,
          target.canonical_empty_target) ||
      !health.healthy_for_target(target.canonical_empty_target) ||
      !health.native_projection_health.empty() ||
      !evidence.final_health_bindings_match(health)) {
    Fail(
        "scene owner did not provide exact healthy zero-native final "
        "aggregate");
  }
}

ManagerBootstrapAcceptedTargetDTO AcceptedTarget(
    const ProjectionBootstrapAcceptedDTO& accepted,
    const ManagerBootstrapIntentDTO& intent) {
  ManagerBootstrapAcceptedTargetDTO target{
      accepted.bootstrap_id,
      accepted.bootstrap_generation,
      accepted.canonical_empty_target,
  };
  if (!accepted.valid() ||
      accepted.bootstrap_id != intent.bootstrap_id ||
      accepted.bootstrap_generation != intent.bootstrap_generation ||
      accepted.cell_id != intent.cell_id ||
      !TargetExactlyMatches(target, intent)) {
    Fail("scene owner returned a mismatched bootstrap Begin acceptance");
  }
  return target;
}

ManagerBootstrapAcceptedTargetDTO AcceptedTarget(
    const ProjectionBootstrapStatusDTO& status,
    const ManagerBootstrapIntentDTO& intent,
    const bool allow_exact_later_state = false) {
  const bool accepted_or_allowed_later =
      status.state == ProjectionBootstrapStatusState::kAccepted ||
      (allow_exact_later_state &&
       (status.state == ProjectionBootstrapStatusState::kRebuilding ||
        status.state == ProjectionBootstrapStatusState::kReadyToPublish ||
        status.state == ProjectionBootstrapStatusState::kPublished ||
        status.state == ProjectionBootstrapStatusState::kFinalized));
  if (!status.presence_valid() ||
      status.bootstrap_id != intent.bootstrap_id ||
      status.bootstrap_generation != intent.bootstrap_generation ||
      !accepted_or_allowed_later ||
      !status.canonical_empty_target.has_value()) {
    Fail(
        "scene owner did not report one exact non-uncertain bootstrap "
        "target");
  }
  ManagerBootstrapAcceptedTargetDTO target{
      status.bootstrap_id,
      status.bootstrap_generation,
      *status.canonical_empty_target,
  };
  if (!TargetExactlyMatches(target, intent)) {
    Fail("scene-owner bootstrap status target mismatches the durable intent");
  }
  return target;
}

void RequireReadyPrerequisites(
    core::IManagerSceneProjectionBootstrapPort& scene_owner,
    const ProjectionBootstrapPrerequisiteQueryDTO& query) {
  try {
    const auto status =
        scene_owner.ObserveProjectionBootstrapPrerequisites(query);
    if (!status.ready_for(query)) {
      Fail(
          "scene-owner canonical-empty bootstrap prerequisites are not "
          "exactly ready and command-fenced");
    }
  } catch (const CanonicalEmptyBootstrapCoordinatorError&) {
    throw;
  } catch (const std::exception& error) {
    Fail(
        "scene-owner canonical-empty prerequisite observation failed: " +
        std::string(error.what()));
  } catch (...) {
    Fail("scene-owner canonical-empty prerequisite observation failed");
  }
}

ProjectionBootstrapStatusDTO ReadStatus(
    core::IManagerSceneProjectionBootstrapPort& scene_owner,
    const ManagerBootstrapIntentDTO& intent) {
  try {
    auto status = scene_owner.ProjectionBootstrapStatus(
        ProjectionBootstrapStatusQueryDTO{
            intent.bootstrap_id,
            intent.bootstrap_generation,
        });
    if (!status.presence_valid() ||
        status.bootstrap_id != intent.bootstrap_id ||
        status.bootstrap_generation != intent.bootstrap_generation) {
      Fail("scene owner returned mismatched bootstrap status");
    }
    return status;
  } catch (const CanonicalEmptyBootstrapCoordinatorError&) {
    throw;
  } catch (const std::exception& error) {
    Fail(
        "scene-owner bootstrap status observation failed: " +
        std::string(error.what()));
  } catch (...) {
    Fail("scene-owner bootstrap status observation failed");
  }
}

void RequireExactAcceptedContext(
    const ProjectionBootstrapStatusDTO& status,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  if (!status.presence_valid() ||
      status.bootstrap_id != intent.bootstrap_id ||
      status.bootstrap_generation != intent.bootstrap_generation ||
      !status.canonical_empty_target.has_value() ||
      *status.canonical_empty_target != target.canonical_empty_target ||
      !status.native_rebuild_observations.empty()) {
    Fail(
        "scene-owner bootstrap status does not exactly match the durable "
        "zero-native accepted target");
  }
}

void RequireExactLaterStatus(
    const ProjectionBootstrapStatusDTO& status,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target,
    const ActiveBundlePublicationRequestDTO& publication_request,
    const std::optional<ActiveBundlePublicationReceiptDTO>& publication_head) {
  RequireExactAcceptedContext(status, intent, target);
  switch (status.state) {
    case ProjectionBootstrapStatusState::kReadyToPublish:
      if (!status.bootstrap_evidence.has_value() ||
          status.publication_receipt.has_value() ||
          status.finalized_aggregate_health.has_value()) {
        Fail("ready bootstrap status carries invalid phase evidence");
      }
      RequireExactZeroNativeEvidence(
          *status.bootstrap_evidence, intent, target);
      return;
    case ProjectionBootstrapStatusState::kPublished:
      if (!status.bootstrap_evidence.has_value() ||
          !status.publication_receipt.has_value() ||
          status.finalized_aggregate_health.has_value() ||
          !publication_head.has_value() ||
          *status.publication_receipt != *publication_head ||
          !IsExactGenerationOneReceipt(
              *status.publication_receipt, publication_request)) {
        Fail("published bootstrap status lacks exact durable publication");
      }
      RequireExactZeroNativeEvidence(
          *status.bootstrap_evidence, intent, target);
      return;
    case ProjectionBootstrapStatusState::kFinalized:
      if (!status.bootstrap_evidence.has_value() ||
          !status.publication_receipt.has_value() ||
          !status.finalized_aggregate_health.has_value() ||
          !publication_head.has_value() ||
          *status.publication_receipt != *publication_head ||
          !IsExactGenerationOneReceipt(
              *status.publication_receipt, publication_request)) {
        Fail("finalized bootstrap status lacks exact durable publication");
      }
      RequireExactZeroNativeEvidence(
          *status.bootstrap_evidence, intent, target);
      RequireExactZeroNativeFinalHealth(
          *status.finalized_aggregate_health,
          intent,
          target,
          *status.bootstrap_evidence);
      return;
    case ProjectionBootstrapStatusState::kAccepted:
    case ProjectionBootstrapStatusState::kRebuilding:
      if (publication_head.has_value()) {
        Fail(
            "accepted or rebuilding bootstrap status cannot follow durable "
            "publication");
      }
      return;
    case ProjectionBootstrapStatusState::kAbsent:
    case ProjectionBootstrapStatusState::kFailedProjectionUncertain:
      return;
  }
}

struct ExactBootstrapObservation final {
  ProjectionBootstrapStatusDTO status;
  std::optional<ActiveBundlePublicationReceiptDTO> publication_head;
};

ExactBootstrapObservation ObserveExactBootstrapPhase(
    core::IManagerSceneProjectionBootstrapPort& scene_owner,
    ExternallyProvisionedManagerStores& stores,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target,
    const ActiveBundlePublicationRequestDTO& publication_request) {
  auto status = ReadStatus(scene_owner, intent);
  auto publication_head =
      ReadPublicationHead(stores, publication_request);

  // Publication is monotonic. Once the exact generation-1 head is observed,
  // retain it and reread only status if that first status could predate the
  // publication. A second Accepted/Rebuilding status after the observed head
  // is genuine contradictory phase evidence and fails closed.
  if (publication_head.has_value() &&
      (status.state == ProjectionBootstrapStatusState::kAccepted ||
       status.state == ProjectionBootstrapStatusState::kRebuilding)) {
    status = ReadStatus(scene_owner, intent);
  }

  RequireExactAcceptedContext(status, intent, target);
  RequireExactLaterStatus(
      status, intent, target, publication_request, publication_head);
  return {std::move(status), std::move(publication_head)};
}

PersistedManagerBootstrapAcceptedTarget PersistAcceptedTarget(
    ExternallyProvisionedManagerStores& stores,
    const ManagerBootstrapIntentDTO& intent,
    const ManagerBootstrapAcceptedTargetDTO& target) {
  if (!TargetExactlyMatches(target, intent)) {
    Fail("bootstrap accepted target cannot be durably recorded");
  }
  const auto write =
      stores.bootstrap_journal().RecordAcceptedTarget(target);
  if (write.record.accepted_target != target) {
    Fail("bootstrap journal replayed a different accepted target");
  }
  return write.record;
}

}  // namespace

CanonicalEmptyBootstrapCoordinator::CanonicalEmptyBootstrapCoordinator(
    ExternallyProvisionedManagerStores& stores,
    core::IManagerSceneProjectionBootstrapPort& scene_owner) noexcept
    : stores_(stores), scene_owner_(scene_owner) {}

PersistedManagerBootstrapAcceptedTarget
CanonicalEmptyBootstrapCoordinator::BeginOrRecover(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  VerifyAdmission(stores_.matches_admission(admission), admission);

  // Both stores must remain structurally valid and cell-bound. An exact
  // generation-1 publication is tolerated only after this journal already
  // contains the durable accepted target; it is crash-recovery evidence, not
  // proof that Begin itself completed.
  const auto inventory = stores_.StartupInventory();
  if (inventory.bootstrap_journal.cell_id != admission.cell_id() ||
      inventory.active_bundle_publication.cell_id != admission.cell_id()) {
    Fail("canonical-empty Begin requires exact cell-bound stores");
  }

  auto snapshot = stores_.bootstrap_journal().ReadSnapshot();
  if (!snapshot.valid() || snapshot.cell_id != admission.cell_id()) {
    Fail("canonical-empty bootstrap journal snapshot is invalid");
  }
  const bool publication_is_empty =
      inventory.active_bundle_publication.receipt_count == 0 &&
      inventory.active_bundle_publication.current_generation.value() == 0U;
  if (snapshot.phase !=
          ManagerBootstrapJournalPhase::kAcceptedTargetPersisted &&
      !publication_is_empty) {
    Fail(
        "canonical-empty Begin requires an empty publication head before "
        "the accepted target is durable");
  }

  const auto query = AdmissionQuery(admission);
  ManagerBootstrapIntentDTO intent{
      BootstrapId("invalid"),
      BootstrapGeneration(0U),
      admission.cell_id(),
      admission.retained_bundle_id(),
      admission.graph_revision(),
      {},
  };

  if (snapshot.phase == ManagerBootstrapJournalPhase::kEmpty) {
    if (snapshot.intent.has_value() ||
        snapshot.accepted_target.has_value()) {
      Fail("empty bootstrap journal contains persisted state");
    }

    // This first observation must precede identity minting and persistence.
    RequireReadyPrerequisites(scene_owner_, query);
    intent = ManagerIntent(admission, MintBootstrapId());
    try {
      const auto write =
          stores_.bootstrap_journal().EnsureIntent(intent);
      if (!write.inserted || write.record.intent != intent) {
        Fail(
            "bootstrap journal did not durably insert the first exact "
            "intent");
      }
      snapshot = stores_.bootstrap_journal().ReadSnapshot();
    } catch (const ManagerBootstrapJournalConflictError&) {
      // Another coordinator can win after both observed an empty journal.
      // Reuse only that exact durable winner; never retry with our losing ID.
      snapshot = stores_.bootstrap_journal().ReadSnapshot();
      if (!snapshot.valid() || !snapshot.intent.has_value() ||
          !IntentExactlyMatchesAdmission(
              snapshot.intent->intent, admission)) {
        Fail(
            "concurrent bootstrap intent winner does not exactly match the "
            "admitted canonical-empty package");
      }
      intent = snapshot.intent->intent;
    }
  } else {
    if (!snapshot.intent.has_value() ||
        !IntentExactlyMatchesAdmission(
            snapshot.intent->intent, admission)) {
      Fail(
          "persisted bootstrap intent does not exactly match the admitted "
          "canonical-empty package");
    }
    intent = snapshot.intent->intent;
    const auto replay =
        stores_.bootstrap_journal().EnsureIntent(intent);
    if (replay.inserted || replay.record != *snapshot.intent) {
      Fail("bootstrap journal did not exactly replay its durable intent");
    }
  }

  if (snapshot.phase ==
      ManagerBootstrapJournalPhase::kAcceptedTargetPersisted) {
    if (!snapshot.accepted_target.has_value() ||
        !TargetExactlyMatches(
            snapshot.accepted_target->accepted_target, intent)) {
      Fail("persisted bootstrap accepted target mismatches its exact intent");
    }
    const auto expected_request = PublicationRequest(
        admission, intent, snapshot.accepted_target->accepted_target);
    const auto observation = ObserveExactBootstrapPhase(
        scene_owner_,
        stores_,
        intent,
        snapshot.accepted_target->accepted_target,
        expected_request);
    const auto observed_target =
        AcceptedTarget(observation.status, intent, true);
    if (observed_target !=
        snapshot.accepted_target->accepted_target) {
      Fail(
          "scene-owner accepted status mismatches the durable accepted "
          "target");
    }
    return PersistAcceptedTarget(stores_, intent, observed_target);
  }

  if (snapshot.phase != ManagerBootstrapJournalPhase::kEmpty &&
      snapshot.phase != ManagerBootstrapJournalPhase::kIntentPersisted) {
    Fail("bootstrap journal is in an unsupported phase");
  }

  auto status = ReadStatus(scene_owner_, intent);
  if (status.state == ProjectionBootstrapStatusState::kAccepted) {
    return PersistAcceptedTarget(
        stores_, intent, AcceptedTarget(status, intent));
  }
  if (status.state != ProjectionBootstrapStatusState::kAbsent) {
    Fail(
        "bootstrap Begin recovery found a later or uncertain scene-owner "
        "state");
  }

  // Status absence does not preserve the earlier point observation. Recheck
  // the same owner immediately before its atomic Begin recheck.
  RequireReadyPrerequisites(scene_owner_, query);
  const auto scene_intent = SceneIntent(admission, intent);
  std::optional<ProjectionBootstrapAcceptedDTO> accepted;
  try {
    accepted = scene_owner_.BeginProjectionBootstrap(scene_intent);
  } catch (...) {
    std::string begin_failure = "non-standard exception";
    try {
      throw;
    } catch (const std::exception& error) {
      begin_failure = error.what();
    } catch (...) {
    }
    // A lost Begin response is the one ambiguous transport outcome this slice
    // can reconcile. Only an exact accepted status is safe to persist.
    try {
      status = ReadStatus(scene_owner_, intent);
    } catch (const std::exception& status_error) {
      Fail(
          "bootstrap Begin failed (" + begin_failure +
          "); status recovery also failed: " + status_error.what());
    }
    if (status.state != ProjectionBootstrapStatusState::kAccepted) {
      Fail(
          "bootstrap Begin failed (" + begin_failure +
          ") without an exact accepted status; outcome is unavailable or "
          "uncertain");
    }
    return PersistAcceptedTarget(
        stores_, intent, AcceptedTarget(status, intent));
  }
  return PersistAcceptedTarget(
      stores_, intent, AcceptedTarget(*accepted, intent));
}

ProjectionBootstrapStatusDTO
CanonicalEmptyBootstrapCoordinator::AdvanceOrRecover(
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  VerifyAdmission(stores_.matches_admission(admission), admission);

  const auto inventory = stores_.StartupInventory();
  if (inventory.bootstrap_journal.cell_id != admission.cell_id() ||
      inventory.active_bundle_publication.cell_id != admission.cell_id()) {
    Fail("canonical-empty advance requires exact cell-bound stores");
  }

  const auto snapshot = stores_.bootstrap_journal().ReadSnapshot();
  if (!snapshot.valid() ||
      snapshot.cell_id != admission.cell_id() ||
      snapshot.phase !=
          ManagerBootstrapJournalPhase::kAcceptedTargetPersisted ||
      !snapshot.intent.has_value() ||
      !snapshot.accepted_target.has_value() ||
      !IntentExactlyMatchesAdmission(
          snapshot.intent->intent, admission) ||
      !TargetExactlyMatches(
          snapshot.accepted_target->accepted_target,
          snapshot.intent->intent)) {
    Fail(
        "canonical-empty advance requires one exact durable accepted "
        "target");
  }
  const auto& intent = snapshot.intent->intent;
  const auto& target = snapshot.accepted_target->accepted_target;
  const auto publication_request =
      PublicationRequest(admission, intent, target);
  auto observation = ObserveExactBootstrapPhase(
      scene_owner_, stores_, intent, target, publication_request);
  auto status = std::move(observation.status);
  auto publication_head = std::move(observation.publication_head);

  if (status.state == ProjectionBootstrapStatusState::kAbsent) {
    Fail("durable accepted target is absent from the scene owner");
  }
  if (status.state ==
      ProjectionBootstrapStatusState::kFailedProjectionUncertain) {
    Fail("scene owner reports uncertain canonical-empty bootstrap state");
  }
  if (status.state == ProjectionBootstrapStatusState::kFinalized) {
    return status;
  }
  if (status.state == ProjectionBootstrapStatusState::kRebuilding) {
    return status;
  }

  if (status.state == ProjectionBootstrapStatusState::kAccepted) {
    const ProjectionBootstrapRebuildRequestDTO request{
        intent.bootstrap_id,
        intent.bootstrap_generation,
        target.canonical_empty_target,
    };
    if (!request.valid()) {
      Fail("canonical-empty rebuild request is invalid");
    }
    try {
      const ProjectionBootstrapRebuildResultDTO rebuilt =
          scene_owner_.RebuildProjectionBootstrap(request);
      if (!rebuilt.presence_valid() ||
          rebuilt.bootstrap_id != intent.bootstrap_id ||
          rebuilt.bootstrap_generation != intent.bootstrap_generation ||
          rebuilt.canonical_empty_target !=
              target.canonical_empty_target ||
          rebuilt.state !=
              ProjectionBootstrapRebuildResultState::kReadyToPublish ||
          !rebuilt.bootstrap_evidence.has_value()) {
        Fail("scene owner returned mismatched canonical-empty rebuild result");
      }
      RequireExactZeroNativeEvidence(
          *rebuilt.bootstrap_evidence, intent, target);
      ProjectionBootstrapStatusDTO ready{
          intent.bootstrap_id,
          intent.bootstrap_generation,
          ProjectionBootstrapStatusState::kReadyToPublish,
          target.canonical_empty_target,
          {},
          rebuilt.bootstrap_evidence,
          std::nullopt,
          std::nullopt,
          std::nullopt,
      };
      if (!ready.presence_valid()) {
        Fail("canonical-empty rebuild result cannot form exact ready status");
      }
      return ready;
    } catch (const CanonicalEmptyBootstrapCoordinatorError&) {
      throw;
    } catch (...) {
      std::string rebuild_failure = "non-standard exception";
      try {
        throw;
      } catch (const std::exception& error) {
        rebuild_failure = error.what();
      } catch (...) {
      }
      try {
        observation = ObserveExactBootstrapPhase(
            scene_owner_, stores_, intent, target, publication_request);
        status = std::move(observation.status);
        publication_head = std::move(observation.publication_head);
      } catch (const std::exception& status_error) {
        Fail(
            "bootstrap Rebuild failed (" + rebuild_failure +
            "); recovery observation also failed: " +
            status_error.what());
      }
      if (status.state ==
              ProjectionBootstrapStatusState::kRebuilding ||
          status.state ==
              ProjectionBootstrapStatusState::kReadyToPublish ||
          status.state == ProjectionBootstrapStatusState::kPublished ||
          status.state == ProjectionBootstrapStatusState::kFinalized) {
        return status;
      }
      Fail(
          "bootstrap Rebuild failed (" + rebuild_failure +
          ") without exact later-state evidence");
    }
  }

  if (status.state == ProjectionBootstrapStatusState::kReadyToPublish) {
    if (!status.bootstrap_evidence.has_value()) {
      Fail("ready bootstrap status lacks exact rebuild evidence");
    }
    if (!publication_head.has_value()) {
      try {
        const auto published =
            stores_.active_bundle_publication_store().PublishActiveBundle(
                publication_request);
        if (!IsExactGenerationOneReceipt(
                published, publication_request)) {
          Fail(
              "publication store returned mismatched canonical-empty "
              "receipt");
        }
        publication_head = published;
      } catch (const CanonicalEmptyBootstrapCoordinatorError&) {
        throw;
      } catch (...) {
        std::string publish_failure = "non-standard exception";
        try {
          throw;
        } catch (const std::exception& error) {
          publish_failure = error.what();
        } catch (...) {
        }
        try {
          publication_head =
              ReadPublicationHead(stores_, publication_request);
        } catch (const std::exception& recovery_error) {
          Fail(
              "bootstrap publication failed (" + publish_failure +
              "); durable-head recovery also failed: " +
              recovery_error.what());
        }
        if (!publication_head.has_value()) {
          Fail(
              "bootstrap publication failed (" + publish_failure +
              ") without exact durable receipt evidence");
        }
      }
    }
  }

  if (status.state != ProjectionBootstrapStatusState::kReadyToPublish &&
      status.state != ProjectionBootstrapStatusState::kPublished) {
    Fail("scene owner reported an unsupported bootstrap advance state");
  }
  if (!status.bootstrap_evidence.has_value() ||
      !publication_head.has_value()) {
    Fail("canonical-empty Finalize requires exact evidence and publication");
  }
  RequireExactZeroNativeEvidence(
      *status.bootstrap_evidence, intent, target);

  const ProjectionBootstrapFinalizeDTO finalize_request{
      intent.bootstrap_id,
      intent.bootstrap_generation,
      target.canonical_empty_target,
      *publication_head,
  };
  if (!finalize_request.valid()) {
    Fail("canonical-empty Finalize request is invalid");
  }
  try {
    const ProjectionBootstrapFinalizeResultDTO finalized =
        scene_owner_.FinalizeProjectionBootstrap(finalize_request);
    if (!finalized.presence_valid() ||
        finalized.bootstrap_id != intent.bootstrap_id ||
        finalized.bootstrap_generation != intent.bootstrap_generation ||
        finalized.canonical_empty_target !=
            target.canonical_empty_target ||
        finalized.publication_receipt != *publication_head ||
        finalized.state !=
            ProjectionBootstrapFinalizeResultState::kFinalized ||
        !finalized.bootstrap_evidence.has_value() ||
        !finalized.candidate_aggregate_health.has_value()) {
      Fail("scene owner returned mismatched canonical-empty Finalize result");
    }
    RequireExactZeroNativeEvidence(
        *finalized.bootstrap_evidence, intent, target);
    RequireExactZeroNativeFinalHealth(
        *finalized.candidate_aggregate_health,
        intent,
        target,
        *finalized.bootstrap_evidence);
    ProjectionBootstrapStatusDTO complete{
        intent.bootstrap_id,
        intent.bootstrap_generation,
        ProjectionBootstrapStatusState::kFinalized,
        target.canonical_empty_target,
        {},
        finalized.bootstrap_evidence,
        finalized.publication_receipt,
        finalized.candidate_aggregate_health,
        std::nullopt,
    };
    if (!complete.presence_valid()) {
      Fail("canonical-empty Finalize result cannot form exact final status");
    }
    return complete;
  } catch (const CanonicalEmptyBootstrapCoordinatorError&) {
    throw;
  } catch (...) {
    std::string finalize_failure = "non-standard exception";
    try {
      throw;
    } catch (const std::exception& error) {
      finalize_failure = error.what();
    } catch (...) {
    }
    try {
      observation = ObserveExactBootstrapPhase(
          scene_owner_, stores_, intent, target, publication_request);
      status = std::move(observation.status);
      publication_head = std::move(observation.publication_head);
    } catch (const std::exception& status_error) {
      Fail(
          "bootstrap Finalize failed (" + finalize_failure +
          "); recovery observation also failed: " +
          status_error.what());
    }
    if (status.state == ProjectionBootstrapStatusState::kFinalized) {
      return status;
    }
    Fail(
        "bootstrap Finalize failed (" + finalize_failure +
        ") without exact finalized evidence");
  }
}

}  // namespace manyforge::component_manager

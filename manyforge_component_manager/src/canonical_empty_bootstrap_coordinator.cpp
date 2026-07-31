#include "manyforge_component_manager/canonical_empty_bootstrap_coordinator.hpp"

#include <openssl/rand.h>

#include <array>
#include <cstddef>
#include <exception>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <manyforge_deployment/canonical_empty_admission.hpp>

namespace manyforge::component_manager {
namespace {

using manyforge::core::BootstrapGeneration;
using manyforge::core::BootstrapId;
using manyforge::core::ProjectionBootstrapAcceptedDTO;
using manyforge::core::ProjectionBootstrapIntentDTO;
using manyforge::core::ProjectionBootstrapPrerequisiteQueryDTO;
using manyforge::core::ProjectionBootstrapStatusDTO;
using manyforge::core::ProjectionBootstrapStatusQueryDTO;
using manyforge::core::ProjectionBootstrapStatusState;

inline constexpr std::size_t kBootstrapRandomBytes = 16U;
inline constexpr std::string_view kBootstrapIdPrefix = "bootstrap-";

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
    const ExternallyProvisionedManagerStores& stores,
    const deployment::CanonicalEmptyRetainedBundleAdmission& admission) {
  const auto& bundle = admission.retained_bundle();
  const auto& value = bundle.value();
  if (admission.api_version() !=
          deployment::kCanonicalEmptyAdmissionApiVersion ||
      admission.admission_digest().empty() ||
      admission.canonical_bytes().empty() ||
      stores.cell_id() != admission.cell_id() ||
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
  if (!intent.valid() || !intent.native_runtime_bindings.empty()) {
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
         intent.native_runtime_bindings.empty();
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
  VerifyAdmission(stores_, admission);

  // Store integrity and an empty publication head are prerequisites, not
  // states that bootstrap is allowed to repair or supersede.
  const auto inventory = stores_.StartupInventory();
  if (inventory.bootstrap_journal.cell_id != admission.cell_id() ||
      inventory.active_bundle_publication.cell_id != admission.cell_id() ||
      inventory.active_bundle_publication.receipt_count != 0 ||
      inventory.active_bundle_publication.current_generation.value() != 0U) {
    Fail(
        "canonical-empty Begin requires exact stores and an empty active "
        "publication head");
  }

  auto snapshot = stores_.bootstrap_journal().ReadSnapshot();
  if (!snapshot.valid() || snapshot.cell_id != admission.cell_id()) {
    Fail("canonical-empty bootstrap journal snapshot is invalid");
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
    const auto status = ReadStatus(scene_owner_, intent);
    const auto observed_target =
        AcceptedTarget(status, intent, true);
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

}  // namespace manyforge::component_manager

#include "manager_liveness_rpc_internal.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace manyforge::component_manager::detail {

ManagerLivenessRpcRejection::ManagerLivenessRpcRejection(std::string reason)
    : std::invalid_argument("manager liveness RPC: request rejected: " +
                            reason),
      reason_(std::move(reason)) {}

namespace {

using namespace manyforge::core;

[[noreturn]] void Fail(std::string_view reason) {
  throw std::invalid_argument(std::string("manager liveness RPC: ") +
                              std::string(reason));
}

std::vector<std::string> Fields(std::string_view bytes) {
  if (bytes.empty() || bytes.size() > kMaximumManagerLivenessRpcPacketBytes ||
      bytes.find_first_of("\r\n\t") != std::string_view::npos) {
    Fail("invalid framing");
  }
  std::istringstream stream{std::string(bytes)};
  std::vector<std::string> result;
  for (std::string field; stream >> field;) {
    result.push_back(std::move(field));
  }
  return result;
}

template <typename T>
T Unsigned(std::string_view text) {
  T value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      (text.size() > 1U && text.front() == '0')) {
    Fail("non-canonical unsigned integer");
  }
  return value;
}

std::int64_t Signed(std::string_view text) {
  std::int64_t value{};
  const auto parsed =
      std::from_chars(text.data(), text.data() + text.size(), value);
  if (text.empty() || parsed.ec != std::errc{} ||
      parsed.ptr != text.data() + text.size() ||
      (text.size() > 1U && text.front() == '0') || text == "-0") {
    Fail("non-canonical signed integer");
  }
  return value;
}

void CheckToken(std::string_view token) {
  if (token.empty() || token.find_first_of(" \r\n\t") !=
                           std::string_view::npos) {
    Fail("invalid identity token");
  }
}

void RequireCoherent(const ManagerLivenessTransportRequestDTO& request) {
  const auto& set = request.required_set;
  const auto& beat = request.heartbeat;
  const auto& policy = request.policy;
  const auto target_count =
      std::count_if(set.required_receivers.begin(),
                    set.required_receivers.end(), [&](const auto& row) {
                      return row.runtime_unit_id ==
                             request.receiver_runtime_unit_id;
                    });
  const bool target_policy_matches =
      std::any_of(set.required_receivers.begin(),
                  set.required_receivers.end(), [&](const auto& row) {
                    return row.runtime_unit_id ==
                               request.receiver_runtime_unit_id &&
                           row.runtime_policy_id ==
                               policy.runtime_policy_id &&
                           row.runtime_policy_revision ==
                               policy.runtime_policy_revision;
                  });
  const bool heartbeat_matches =
      beat.cell_id == set.cell_id &&
      beat.manager_runtime_unit_id == set.manager_runtime_unit_id &&
      beat.runtime_policy_id == policy.runtime_policy_id &&
      beat.runtime_policy_revision == policy.runtime_policy_revision;
  bool reconciliation_matches = true;
  if (request.reconciliation.has_value()) {
    const auto& ack = *request.reconciliation;
    reconciliation_matches =
        ack.cell_id == set.cell_id &&
        ack.manager_runtime_unit_id == set.manager_runtime_unit_id &&
        ack.active_bundle_id == set.active_bundle_id &&
        ack.required_runtime_set_digest == set.required_runtime_set_digest &&
        manyforge::content_identity::is_sha256_digest(
            ack.reconciliation_evidence_digest.value());
  }
  if (target_count != 1U || !target_policy_matches || !heartbeat_matches ||
      !reconciliation_matches) {
    Fail("request identities are incoherent");
  }
}

}  // namespace

bool IsManagerLivenessRpcPacket(std::string_view bytes) noexcept {
  return bytes.starts_with(kManagerLivenessRequestTag) &&
         bytes.size() > kManagerLivenessRequestTag.size() &&
         bytes[kManagerLivenessRequestTag.size()] == ' ';
}

std::string EncodeManagerLivenessRpcRequest(
    const ManagerLivenessTransportRequestDTO& request) {
  if (!ValidateManagerLivenessRequiredSet(request.required_set).ok() ||
      !ValidateManagerLivenessPolicy(request.policy).ok() ||
      !ValidateManagerHeartbeat(request.heartbeat).ok() ||
      request.receiver_runtime_unit_id.empty() ||
      !request.active_publication.valid() ||
      !manyforge::content_identity::is_sha256_digest(
          request.active_publication.durable_public_pointer_content_digest.value())) {
    Fail("request is invalid");
  }
  RequireCoherent(request);
  const auto& set = request.required_set;
  const auto& policy = request.policy;
  const auto& beat = request.heartbeat;
  const auto& publication = request.active_publication;
  const auto& projection = publication.request.candidate_projection_revision;
  std::ostringstream out;
  out << kManagerLivenessRequestTag << ' '
      << publication.publication_generation.value() << ' '
      << publication.durable_public_pointer_content_digest.value() << ' ';
  if (const auto* apply = std::get_if<NamedApplyPublicationTransactionDTO>(
          &publication.request.transaction_ref)) {
    out << "apply " << apply->apply_id.value() << ' '
        << apply->apply_generation.value() << ' ';
  } else {
    const auto& bootstrap =
        std::get<BootstrapPublicationTransactionDTO>(publication.request.transaction_ref);
    out << "bootstrap " << bootstrap.bootstrap_id.value() << ' '
        << bootstrap.bootstrap_generation.value() << ' ';
  }
  // The candidate bundle and graph are the projection's own.
  out << projection.retained_bundle_id.value() << ' '
      << projection.graph_revision.value() << ' '
      << projection.scene_revision.scene_id.value() << ' '
      << projection.scene_revision.revision_id << ' '
      << projection.scene_revision.content_hash.value() << ' ';
  if (publication.request.expected_previous_publication_generation) {
    out << publication.request.expected_previous_publication_generation->value();
  } else {
    out << '-';
  }
  out << ' ' << publication.request.private_materialization_content_digest.value() << ' '
      << set.cell_id.value() << ' '
      << set.active_bundle_id.value() << ' '
      << set.required_runtime_set_digest.value() << ' '
      << set.manager_runtime_unit_id.value() << ' '
      << request.receiver_runtime_unit_id.value() << ' '
      << policy.runtime_policy_id.value() << ' '
      << policy.runtime_policy_revision.value() << ' '
      << policy.heartbeat_period_ns << ' '
      << beat.heartbeat_sequence << ' ';
  if (request.reconciliation.has_value()) {
    out << request.reconciliation->reconciliation_evidence_digest.value();
  } else {
    out << '-';
  }
  out << ' ' << set.required_receivers.size();
  for (const auto& receiver : set.required_receivers) {
    out << ' ' << receiver.runtime_unit_id.value() << ' '
        << receiver.runtime_epoch.value() << ' '
        << receiver.runtime_policy_id.value() << ' '
        << receiver.runtime_policy_revision.value();
  }
  const auto bytes = out.str();
  if (bytes.size() > kMaximumManagerLivenessRpcPacketBytes) {
    Fail("request exceeds bound");
  }
  return bytes;
}

ManagerLivenessTransportRequestDTO DecodeManagerLivenessRpcRequest(
    std::string_view bytes) {
  // Twelve publication tokens precede the required set (offset 13).
  const auto f = Fields(bytes);
  if (f.size() < 28U || f[0] != kManagerLivenessRequestTag) {
    Fail("wrong closed request shape");
  }
  for (std::size_t i = 1; i < f.size(); ++i) {
    CheckToken(f[i]);
  }
  const auto receiver_count = Unsigned<std::size_t>(f[23]);
  if (receiver_count == 0U ||
      receiver_count > kMaximumManagerLivenessReceivers ||
      f.size() != 24U + receiver_count * 4U) {
    Fail("wrong required receiver count");
  }
  std::vector<RequiredManagerLivenessReceiverDTO> receivers;
  receivers.reserve(receiver_count);
  for (std::size_t index = 0; index < receiver_count; ++index) {
    const auto offset = 24U + index * 4U;
    receivers.push_back(
        {RuntimeUnitId(f[offset]), RuntimeEpoch(f[offset + 1U]),
         RuntimePolicyId(f[offset + 2U]),
         RuntimePolicyRevision(f[offset + 3U])});
  }
  auto transaction = [&]() -> ActiveBundlePublicationTransactionRefDTO {
    if (f[3] == "apply") {
      return NamedApplyPublicationTransactionDTO{
          ApplyId(Unsigned<std::uint64_t>(f[4])),
          ApplyGeneration(Unsigned<std::uint64_t>(f[5]))};
    }
    if (f[3] == "bootstrap") {
      return BootstrapPublicationTransactionDTO{
          BootstrapId(f[4]), BootstrapGeneration(Unsigned<std::uint64_t>(f[5]))};
    }
    Fail("unknown publication transaction kind");
  }();
  const ProjectionRevisionDTO projection{
      RetainedBundleId(f[6]), GraphRevision(f[7]),
      {SceneId(f[8]), Unsigned<std::uint64_t>(f[9]), ContentDigest(f[10])}};
  ActiveBundlePublicationReceiptDTO publication{
      {std::move(transaction), projection, projection.retained_bundle_id,
       projection.graph_revision,
       f[11] == "-" ? std::nullopt
                    : std::optional(ActiveBundlePublicationGeneration(
                          Unsigned<std::uint64_t>(f[11]))),
       ContentDigest(f[12])},
      ActiveBundlePublicationGeneration(Unsigned<std::uint64_t>(f[1])),
      ContentDigest(f[2])};
  ManagerLivenessRequiredSetDTO set{
      CellId(f[13]), RetainedBundleId(f[14]), ContentDigest(f[15]),
      RuntimeUnitId(f[16]), std::move(receivers)};
  ManagerLivenessPolicyDTO policy{
      RuntimePolicyId(f[18]), RuntimePolicyRevision(f[19]), Signed(f[20])};
  ManagerHeartbeatDTO heartbeat{
      CellId(f[13]), RuntimeUnitId(f[16]),
      Unsigned<std::uint64_t>(f[21]), RuntimePolicyId(f[18]),
      RuntimePolicyRevision(f[19])};
  ManagerLivenessTransportRequestDTO result{
      RuntimeUnitId(f[17]), std::move(publication),
      set, policy, heartbeat, std::nullopt};
  if (f[22] != "-") {
    result.reconciliation = ManagerLivenessReconciliationAck{
        set.cell_id, set.manager_runtime_unit_id,
        set.active_bundle_id, set.required_runtime_set_digest,
        ContentDigest(f[22])};
  }
  if (!result.active_publication.valid() ||
      !manyforge::content_identity::is_sha256_digest(
          result.active_publication.durable_public_pointer_content_digest.value()) ||
      !ValidateManagerLivenessRequiredSet(set).ok() ||
      !ValidateManagerLivenessPolicy(policy).ok() ||
      !ValidateManagerHeartbeat(heartbeat).ok()) {
    Fail("decoded request is invalid");
  }
  RequireCoherent(result);
  return result;
}

std::string EncodeManagerLivenessRpcResponse(
    const ManagerLivenessObservationDTO& value) {
  if (!ValidateManagerLivenessObservation(value).ok()) {
    Fail("response is invalid");
  }
  const auto& s = value.status;
  std::ostringstream out;
  out << kManagerLivenessResponseTag << ' ' << s.cell_id.value() << ' '
      << s.receiver_runtime_unit_id.value() << ' '
      << s.receiver_runtime_epoch.value() << ' '
      << s.manager_runtime_unit_id.value() << ' '
      << s.runtime_policy_id.value() << ' '
      << s.runtime_policy_revision.value() << ' '
      << s.last_accepted_heartbeat_sequence << ' '
      << s.receiver_status_sequence << ' '
      << static_cast<unsigned>(s.state) << ' ' << (s.command_fenced ? 1 : 0)
      << ' ' << (s.fault_or_effect_reference.has_value()
                     ? s.fault_or_effect_reference->value()
                     : "-")
      << ' ' << value.active_bundle_id.value() << ' '
      << value.required_runtime_set_digest.value();
  return out.str();
}

ManagerLivenessObservationDTO DecodeManagerLivenessRpcResponse(
    std::string_view bytes) {
  const auto f = Fields(bytes);
  if (f.size() == 2U && f[0] == kManagerLivenessRejectionTag) {
    throw ManagerLivenessRpcRejection(f[1]);
  }
  if (f.size() != 14U || f[0] != kManagerLivenessResponseTag) {
    Fail("wrong closed response shape");
  }
  const auto state = Unsigned<unsigned>(f[9]);
  const auto fenced = Unsigned<unsigned>(f[10]);
  if (state > static_cast<unsigned>(ManagerLivenessStateDTO::kUnreconciled) ||
      fenced > 1U) {
    Fail("response enum is invalid");
  }
  ManagerLivenessStatusDTO status{
      CellId(f[1]), RuntimeUnitId(f[2]), RuntimeEpoch(f[3]),
      RuntimeUnitId(f[4]),
      RuntimePolicyId(f[5]), RuntimePolicyRevision(f[6]),
      Unsigned<std::uint64_t>(f[7]), Unsigned<std::uint64_t>(f[8]),
      static_cast<ManagerLivenessStateDTO>(state), fenced == 1U,
      f[11] == "-" ? std::nullopt
                   : std::optional<ContentDigest>(ContentDigest(f[11]))};
  ManagerLivenessObservationDTO result{
      status, RetainedBundleId(f[12]), ContentDigest(f[13])};
  if (!ValidateManagerLivenessObservation(result).ok()) {
    Fail("decoded response is invalid");
  }
  return result;
}

std::string EncodeManagerLivenessRpcRejection(std::string_view reason) {
  CheckToken(reason);
  return std::string(kManagerLivenessRejectionTag) + " " +
         std::string(reason);
}

std::string_view NativeShutdownPhaseName(const NativeShutdownPhase phase) {
  switch (phase) {
  case NativeShutdownPhase::kRunning:
    return "running";
  case NativeShutdownPhase::kGroupRetirement:
    return "group-retirement";
  case NativeShutdownPhase::kExecutionDrain:
    return "execution-drain";
  case NativeShutdownPhase::kProjectionDrain:
    return "projection-drain";
  case NativeShutdownPhase::kKernelDestruction:
    return "kernel-destruction";
  case NativeShutdownPhase::kPreparedCompositionDestruction:
    return "prepared-composition-destruction";
  case NativeShutdownPhase::kRetiredCompositionDestruction:
    return "retired-composition-destruction";
  case NativeShutdownPhase::kActiveCompositionDestruction:
    return "active-composition-destruction";
  case NativeShutdownPhase::kFailedCompositionDestruction:
    return "failed-composition-destruction";
  case NativeShutdownPhase::kProjectionDestruction:
    return "projection-destruction";
  case NativeShutdownPhase::kLivenessReceiverDestruction:
    return "liveness-receiver-destruction";
  case NativeShutdownPhase::kSchedulerOwnerRelease:
    return "scheduler-owner-release";
  case NativeShutdownPhase::kFenceOwnerDestruction:
    return "fence-owner-destruction";
  case NativeShutdownPhase::kServicingLaneDrain:
    return "servicing-lane-drain";
  case NativeShutdownPhase::kPlannedHalt:
    return "planned-halt";
  }
  Fail("unknown shutdown phase");
}

bool IsNativeShutdownRpcPacket(std::string_view bytes) noexcept {
  return bytes.starts_with(kNativeShutdownRequestTag);
}
std::string EncodeNativeShutdownRpcRequest(std::string_view epoch) {
  CheckToken(epoch);
  if (epoch.size() > 1024U)
    Fail("shutdown epoch exceeds bound");
  return std::string(kNativeShutdownRequestTag) + ' ' + std::string(epoch);
}
std::string DecodeNativeShutdownRpcRequest(std::string_view bytes) {
  const auto f = Fields(bytes);
  if (f.size() != 2U || f[0] != kNativeShutdownRequestTag)
    Fail("wrong shutdown request shape");
  CheckToken(f[1]);
  if (f[1].size() > 1024U)
    Fail("shutdown epoch exceeds bound");
  return f[1];
}
std::string EncodeNativeShutdownRpcResponse(const NativeShutdownProgress &p) {
  CheckToken(p.runtime_epoch);
  if (p.runtime_epoch.size() > 1024U || p.completed_workers > 3U ||
      p.pending_groups > p.total_groups ||
      p.signalled_groups > p.total_groups ||
      p.escalated_groups > p.total_groups || p.reaped_groups > p.total_groups ||
      p.drained_groups > p.reaped_groups || p.unknown_groups > p.total_groups ||
      p.pending_pid < -1 || p.pending_pid == 0 ||
      (p.pending_groups == 0U && p.pending_pid != -1))
    Fail("invalid shutdown progress");
  static_cast<void>(NativeShutdownPhaseName(p.phase));
  std::ostringstream out;
  out << kNativeShutdownResponseTag << ' ' << p.runtime_epoch << ' '
      << static_cast<unsigned>(p.phase) << ' ' << p.completed_workers << ' '
      << p.total_groups << ' ' << p.pending_groups << ' ' << p.signalled_groups
      << ' ' << p.escalated_groups << ' ' << p.reaped_groups << ' '
      << p.drained_groups << ' ' << p.unknown_groups << ' ' << p.pending_pid;
  return out.str();
}
NativeShutdownProgress DecodeNativeShutdownRpcResponse(std::string_view bytes) {
  const auto f = Fields(bytes);
  if (f.size() != 12U || f[0] != kNativeShutdownResponseTag)
    Fail("wrong shutdown response shape");
  const auto phase = Unsigned<unsigned>(f[2]);
  if (phase > static_cast<unsigned>(NativeShutdownPhase::kPlannedHalt))
    Fail("unknown shutdown phase");
  const auto pid = Signed(f[11]);
  if (pid < -1 || pid > std::numeric_limits<int>::max())
    Fail("invalid shutdown pending PID");
  NativeShutdownProgress p{f[1],
                           static_cast<NativeShutdownPhase>(phase),
                           Unsigned<std::size_t>(f[3]),
                           Unsigned<std::size_t>(f[4]),
                           Unsigned<std::size_t>(f[5]),
                           Unsigned<std::size_t>(f[6]),
                           Unsigned<std::size_t>(f[7]),
                           Unsigned<std::size_t>(f[8]),
                           Unsigned<std::size_t>(f[9]),
                           Unsigned<std::size_t>(f[10]),
                           static_cast<int>(pid)};
  static_cast<void>(EncodeNativeShutdownRpcResponse(p));
  return p;
}

}  // namespace manyforge::component_manager::detail

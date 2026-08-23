#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>

#include "manager_liveness_rpc_internal.hpp"

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace manyforge::component_manager::detail {
namespace core = manyforge::core;
namespace {
constexpr std::string_view kMagic = "MFCA1";
constexpr std::size_t kMaxEntries = core::kMaximumCellAdmissionClaims;

[[noreturn]] void Fail(const char *message) {
  throw std::invalid_argument(message);
}

class Writer {
public:
  void Byte(std::uint8_t v) { out_.push_back(static_cast<char>(v)); }
  void Bool(bool v) { Byte(v ? 1U : 0U); }
  void U64(std::uint64_t v) {
    for (int s = 56; s >= 0; s -= 8)
      Byte(v >> s);
  }
  void I64(std::int64_t v) { U64(static_cast<std::uint64_t>(v)); }
  void String(std::string_view v) {
    if (v.size() > std::numeric_limits<std::uint32_t>::max())
      Fail("string too large");
    if (out_.size() > kMaximumManagedRuntimeRpcPacketBytes ||
        v.size() > kMaximumManagedRuntimeRpcPacketBytes - out_.size() ||
        v.size() + sizeof(std::uint64_t) >
            kMaximumManagedRuntimeRpcPacketBytes - out_.size())
      Fail("packet too large");
    U64(v.size());
    out_.append(v);
  }
  std::string Finish() && {
    if (out_.size() > kMaximumManagedRuntimeRpcPacketBytes)
      Fail("packet too large");
    return std::move(out_);
  }
  std::string out_;
};
class Reader {
public:
  explicit Reader(std::string_view v) : in_(v) {
    if (v.size() < kMagic.size() ||
        v.size() > kMaximumManagedRuntimeRpcPacketBytes ||
        v.substr(0, kMagic.size()) != kMagic)
      Fail("invalid cell-admission packet");
    pos_ = kMagic.size();
  }
  std::uint8_t Byte() {
    if (pos_ >= in_.size())
      Fail("truncated packet");
    return in_[pos_++];
  }
  bool Bool() {
    auto v = Byte();
    if (v > 1U)
      Fail("noncanonical boolean");
    return v != 0U;
  }
  std::uint64_t U64() {
    std::uint64_t v = 0;
    for (int s = 56; s >= 0; s -= 8)
      v |= std::uint64_t(Byte()) << s;
    return v;
  }
  std::uint32_t U32() {
    const auto value = U64();
    if (value > std::numeric_limits<std::uint32_t>::max()) {
      Fail("uint32 value out of range");
    }
    return static_cast<std::uint32_t>(value);
  }
  std::int64_t I64() { return static_cast<std::int64_t>(U64()); }
  std::string String() {
    auto n = U64();
    if (n > in_.size() - pos_)
      Fail("invalid string length");
    auto v = std::string(in_.substr(pos_, n));
    pos_ += n;
    return v;
  }
  void Finish() const {
    if (pos_ != in_.size())
      Fail("trailing packet fields");
  }

private:
  std::string_view in_;
  std::size_t pos_{};
};
template <class T> void S(Writer &w, const T &v) { w.String(v.value()); }
template <class T> T S(Reader &r) { return T(r.String()); }
template <class T> void G(Writer &w, T v) { w.U64(v.value()); }
template <class T> T G(Reader &r) { return T(r.U64()); }
void Enum(Writer &w, auto v) { w.U64(static_cast<std::uint64_t>(v)); }
template <class T> T Enum(Reader &r, std::uint64_t count) {
  const auto value = r.U64();
  if (value >= count) {
    Fail("unknown enum value");
  }
  return static_cast<T>(value);
}
template <class T> void Opt(Writer &w, const std::optional<T> &v, auto fn) {
  w.Bool(v.has_value());
  if (v)
    fn(w, *v);
}
template <class T> std::optional<T> Opt(Reader &r, auto fn) {
  return r.Bool() ? std::optional<T>(fn(r)) : std::nullopt;
}
template <class T> void Vec(Writer &w, const std::vector<T> &v, auto fn) {
  if (v.size() > kMaxEntries)
    Fail("collection too large");
  w.U64(v.size());
  for (const auto &x : v)
    fn(w, x);
}
template <class T> std::vector<T> Vec(Reader &r, auto fn) {
  auto n = r.U64();
  if (n > kMaxEntries)
    Fail("collection too large");
  std::vector<T> v;
  v.reserve(n);
  while (n--)
    v.push_back(fn(r));
  return v;
}
void Scope(Writer &w, const core::CellAdmissionScopeDTO &x) {
  Vec(w, x.resources, [](Writer &w, const auto &v) { S(w, v); });
  Vec(w, x.configurations, [](Writer &w, const auto &v) {
    S(w, v.control_domain_id);
    S(w, v.active_configuration_id);
    G(w, v.active_configuration_generation);
  });
}
core::CellAdmissionScopeDTO Scope(Reader &r) {
  return {Vec<core::ResourceId>(
              r, [](Reader &r) { return S<core::ResourceId>(r); }),
          Vec<core::CellAdmissionConfigurationScopeDTO>(r, [](Reader &r) {
            return core::CellAdmissionConfigurationScopeDTO{
                S<core::ControlDomainId>(r),
                S<core::ActiveControlConfigurationId>(r),
                G<core::ActiveControlConfigurationGeneration>(r)};
          })};
}
void Intent(Writer &w, const core::CellAdmissionClaimIntentDTO &x) {
  Enum(w, x.claim_kind);
  Enum(w, x.owner_role);
  S(w, x.owner_epoch);
  w.String(x.owner_transaction_id);
  S(w, x.request_id);
  S(w, x.request_payload_digest);
  G(w, x.expected_admission_generation);
  Scope(w, x.scope);
  Opt(w, x.program_cycle_evidence, [](Writer &w, const auto &v) {
    S(w, v.draft_program_revision);
    S(w, v.applied_program_revision);
  });
  Opt(w, x.intervention_evidence, [](Writer &w, const auto &v) {
    Enum(w, v.offering_owner_role);
    S(w, v.offering_owner_epoch);
    w.String(v.offer_transaction_id);
    S(w, v.offer_digest);
    Opt(w, v.parent_program_cycle_claim_generation,
        [](Writer &w, const auto &g) { G(w, g); });
  });
}
core::CellAdmissionClaimIntentDTO Intent(Reader &r) {
  core::CellAdmissionClaimIntentDTO x{
      Enum<core::CellAdmissionClaimKindDTO>(r, 7U),
      Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
      S<core::RequestOwnerEpoch>(r),
      r.String(),
      S<core::RequestId>(r),
      S<core::ContentDigest>(r),
      G<core::AdmissionGeneration>(r),
      Scope(r),
      std::nullopt,
      std::nullopt};
  x.program_cycle_evidence =
      Opt<core::ProgramCycleAdmissionEvidenceDTO>(r, [](Reader &r) {
        return core::ProgramCycleAdmissionEvidenceDTO{
            S<core::ProgramRevision>(r), S<core::ProgramRevision>(r)};
      });
  x.intervention_evidence =
      Opt<core::CellAdmissionInterventionEvidenceDTO>(r, [](Reader &r) {
        return core::CellAdmissionInterventionEvidenceDTO{
            Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
            S<core::RequestOwnerEpoch>(r), r.String(),
            S<core::ContentDigest>(r),
            Opt<core::CellAdmissionClaimGeneration>(r, [](Reader &r) {
              return G<core::CellAdmissionClaimGeneration>(r);
            })};
      });
  return x;
}
void Claim(Writer &w, const core::CellAdmissionClaimDTO &x) {
  Intent(w, x.intent);
  G(w, x.claim_generation);
  G(w, x.acquired_admission_generation);
  w.I64(x.accepted_at_ns);
}
core::CellAdmissionClaimDTO Claim(Reader &r) {
  return {Intent(r), G<core::CellAdmissionClaimGeneration>(r),
          G<core::AdmissionGeneration>(r), r.I64()};
}
void Release(Writer &w, const core::CellAdmissionClaimReleaseDTO &x) {
  Enum(w, x.owner_role);
  S(w, x.owner_epoch);
  w.String(x.owner_transaction_id);
  S(w, x.request_id);
  S(w, x.request_payload_digest);
  G(w, x.acquired_admission_generation);
  G(w, x.claim_generation);
  Enum(w, x.settlement.settlement_kind);
  S(w, x.settlement.evidence_digest);
}
core::CellAdmissionClaimReleaseDTO Release(Reader &r) {
  return {Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
          S<core::RequestOwnerEpoch>(r),
          r.String(),
          S<core::RequestId>(r),
          S<core::ContentDigest>(r),
          G<core::AdmissionGeneration>(r),
          G<core::CellAdmissionClaimGeneration>(r),
          {Enum<core::CellAdmissionSettlementKindDTO>(r, 5U),
           S<core::ContentDigest>(r)}};
}
void Readiness(Writer &w, const core::CellAdmissionReadinessSnapshotDTO &x) {
  w.Bool(x.program_cycle_status_current);
  Opt(w, x.program_cycle_status, [](Writer &w, const auto &v) {
    S(w, v.program_runner_owner_epoch);
    w.U64(v.cycle_generation);
    Enum(w, v.state);
    Opt(w, v.program_run_id, [](Writer &w, const auto &v) { S(w, v); });
    Opt(w, v.cycle_claim_generation, [](Writer &w, const auto &v) { G(w, v); });
    Opt(w, v.terminal_or_settlement_reference,
        [](Writer &w, const auto &v) { S(w, v); });
  });
  w.Bool(x.required_component_status_current);
  w.U64(x.required_component_total);
  w.U64(x.required_component_current_compatible_ready_converged);
  w.Bool(x.deployment_status_current);
  w.Bool(x.deployment_converged);
  w.U64(x.blocking_fault_count);
  w.U64(x.unresolved_owner_count);
  w.U64(x.unsettled_effect_count);
}

core::CellAdmissionReadinessSnapshotDTO Readiness(Reader &r) {
  core::CellAdmissionReadinessSnapshotDTO x;
  x.program_cycle_status_current = r.Bool();
  x.program_cycle_status = Opt<core::ProgramCycleStatusDTO>(r, [](Reader &r) {
    return core::ProgramCycleStatusDTO{
        S<core::RequestOwnerEpoch>(r),
        r.U64(),
        Enum<core::ProgramCycleStateDTO>(r, 6U),
        Opt<core::ProgramRunId>(
            r, [](Reader &r) { return S<core::ProgramRunId>(r); }),
        Opt<core::CellAdmissionClaimGeneration>(
            r,
            [](Reader &r) { return G<core::CellAdmissionClaimGeneration>(r); }),
        Opt<core::ContentDigest>(
            r, [](Reader &r) { return S<core::ContentDigest>(r); })};
  });
  x.required_component_status_current = r.Bool();
  x.required_component_total = r.U32();
  x.required_component_current_compatible_ready_converged = r.U32();
  x.deployment_status_current = r.Bool();
  x.deployment_converged = r.Bool();
  x.blocking_fault_count = r.U32();
  x.unresolved_owner_count = r.U32();
  x.unsettled_effect_count = r.U32();
  if (x.required_component_current_compatible_ready_converged >
      x.required_component_total) {
    Fail("readiness count exceeds total");
  }
  if (x.program_cycle_status.has_value() &&
      !core::ValidateProgramCycleStatus(*x.program_cycle_status).ok()) {
    Fail("invalid program-cycle status");
  }
  return x;
}

void Reconcile(Writer &w, const core::CellAdmissionReconcileDTO &x) {
  S(w, x.kernel_owner_epoch);
  Vec(w, x.retained_claims, [](Writer &w, const auto &v) { Claim(w, v); });
  Vec(w, x.current_owner_epochs, [](Writer &w, const auto &v) {
    Enum(w, v.owner_role);
    S(w, v.owner_epoch);
  });
  S(w, x.reconciliation_evidence_digest);
}

core::CellAdmissionReconcileDTO Reconcile(Reader &r) {
  auto result = core::CellAdmissionReconcileDTO{
      S<core::RequestOwnerEpoch>(r),
      Vec<core::CellAdmissionClaimDTO>(r, [](Reader &r) { return Claim(r); }),
      Vec<core::CellAdmissionCurrentOwnerEpochDTO>(
          r,
          [](Reader &r) {
            return core::CellAdmissionCurrentOwnerEpochDTO{
                Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
                S<core::RequestOwnerEpoch>(r)};
          }),
      S<core::ContentDigest>(r)};
  for (const auto &claim : result.retained_claims) {
    if (!core::ValidateCellAdmissionClaimIntent(claim.intent).ok() ||
        claim.claim_generation.value() == 0U ||
        claim.acquired_admission_generation.value() == 0U) {
      Fail("invalid retained claim");
    }
  }
  return result;
}

void OwnerEpoch(Writer &w, const core::CellAdmissionCurrentOwnerEpochDTO &x) {
  Enum(w, x.owner_role);
  S(w, x.owner_epoch);
}
core::CellAdmissionCurrentOwnerEpochDTO OwnerEpoch(Reader &r) {
  return {Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
          S<core::RequestOwnerEpoch>(r)};
}
void ReconcileSettlement(
    Writer &w, const core::CellAdmissionReconcileSettlementDTO &x) {
  Claim(w, x.predecessor_claim);
  Enum(w, x.successor_owner_role);
  S(w, x.successor_owner_epoch);
  Enum(w, x.settlement_predicate);
  S(w, x.reconciliation_evidence_digest);
}
core::CellAdmissionReconcileSettlementDTO ReconcileSettlement(Reader &r) {
  return {Claim(r), Enum<core::CellAdmissionOwnerRoleDTO>(r, 7U),
          S<core::RequestOwnerEpoch>(r),
          Enum<core::CellAdmissionSettlementKindDTO>(r, 5U),
          S<core::ContentDigest>(r)};
}
void RecoveryQuery(Writer &w,
                   const core::CellAdmissionRecoveryReceiptQueryDTO &x) {
  Intent(w, x.intent);
  Opt(w, x.expected_claim,
      [](Writer &w, const auto &v) { Claim(w, v); });
  Opt(w, x.expected_release,
      [](Writer &w, const auto &v) { Release(w, v); });
}
core::CellAdmissionRecoveryReceiptQueryDTO RecoveryQuery(Reader &r) {
  auto result = core::CellAdmissionRecoveryReceiptQueryDTO{
      Intent(r),
      Opt<core::CellAdmissionClaimDTO>(
          r, [](Reader &r) { return Claim(r); }),
      Opt<core::CellAdmissionClaimReleaseDTO>(
          r, [](Reader &r) { return Release(r); })};
  if (!core::ValidateCellAdmissionRecoveryReceiptQuery(result).ok())
    Fail("invalid recovery receipt query");
  return result;
}
void RecoveryReceipt(Writer &w,
                     const core::CellAdmissionRecoveryReceiptDTO &x) {
  if (!core::ValidateCellAdmissionRecoveryReceipt(x).ok())
    Fail("invalid recovery receipt");
  Enum(w, x.status);
  S(w, x.kernel_owner_epoch);
  G(w, x.admission_generation);
  Opt(w, x.matched_claim,
      [](Writer &w, const auto &v) { Claim(w, v); });
  Opt(w, x.matched_release,
      [](Writer &w, const auto &v) { Release(w, v); });
  Opt(w, x.matched_reconcile_settlement,
      [](Writer &w, const auto &v) { ReconcileSettlement(w, v); });
  Vec(w, x.held_claims,
      [](Writer &w, const auto &v) { Claim(w, v); });
  Vec(w, x.current_owner_epochs,
      [](Writer &w, const auto &v) { OwnerEpoch(w, v); });
}
core::CellAdmissionRecoveryReceiptDTO RecoveryReceipt(Reader &r) {
  auto result = core::CellAdmissionRecoveryReceiptDTO{
      Enum<core::CellAdmissionRecoveryReceiptStatusDTO>(r, 5U),
      S<core::RequestOwnerEpoch>(r),
      G<core::AdmissionGeneration>(r),
      Opt<core::CellAdmissionClaimDTO>(
          r, [](Reader &r) { return Claim(r); }),
      Opt<core::CellAdmissionClaimReleaseDTO>(
          r, [](Reader &r) { return Release(r); }),
      Opt<core::CellAdmissionReconcileSettlementDTO>(
          r, [](Reader &r) { return ReconcileSettlement(r); }),
      Vec<core::CellAdmissionClaimDTO>(
          r, [](Reader &r) { return Claim(r); }),
      Vec<core::CellAdmissionCurrentOwnerEpochDTO>(
          r, [](Reader &r) { return OwnerEpoch(r); })};
  if (!core::ValidateCellAdmissionRecoveryReceipt(result).ok())
    Fail("invalid recovery receipt");
  return result;
}

} // namespace

bool IsKernelCellAdmissionRpcPacket(std::string_view b) noexcept {
  return b.size() >= kMagic.size() && b.substr(0, kMagic.size()) == kMagic;
}

KernelCellAdmissionRpcResponse
DispatchKernelCellAdmissionRpc(core::IKernelCellAdmissionPort &kernel,
                               const KernelCellAdmissionRpcPacket &request) {
  const auto adopt = [](const auto &result) -> KernelCellAdmissionRpcResponse {
    return result.ok() ? KernelCellAdmissionRpcResponse{result.value()}
                       : KernelCellAdmissionRpcResponse{result.error()};
  };
  if (request.command == "cell_admission_get") {
    if (!std::holds_alternative<std::monostate>(request.request))
      Fail("get command has an unexpected request DTO");
    return kernel.GetAuthoringAdmission();
  }
  if (request.command == "cell_admission_update_readiness")
    return adopt(kernel.UpdateCellAdmissionReadiness(
        std::get<core::CellAdmissionReadinessSnapshotDTO>(request.request)));
  if (request.command == "cell_admission_publish_liveness") {
    const auto published = kernel.Publish(
        std::get<core::ManagerLivenessObservationDTO>(request.request));
    return published.ok() ? KernelCellAdmissionRpcResponse{std::monostate{}}
                          : KernelCellAdmissionRpcResponse{published.error()};
  }
  if (request.command == "cell_admission_acquire")
    return adopt(kernel.AcquireCellAdmissionClaim(
        std::get<core::CellAdmissionClaimIntentDTO>(request.request)));
  if (request.command == "cell_admission_release")
    return adopt(kernel.ReleaseCellAdmissionClaim(
        std::get<core::CellAdmissionClaimReleaseDTO>(request.request)));
  if (request.command == "cell_admission_reconcile")
    return adopt(kernel.ReconcileCellAdmissionClaims(
        std::get<core::CellAdmissionReconcileDTO>(request.request)));
  if (request.command == "cell_admission_recovery_receipt")
    return adopt(kernel.GetCellAdmissionRecoveryReceipt(
        std::get<core::CellAdmissionRecoveryReceiptQueryDTO>(
            request.request)));
  Fail("unknown cell-admission command");
}
std::string
EncodeKernelCellAdmissionRpcRequest(const KernelCellAdmissionRpcPacket &x) {
  const bool get = x.command == "cell_admission_get";
  const bool readiness = x.command == "cell_admission_update_readiness";
  const bool liveness = x.command == "cell_admission_publish_liveness";
  const bool acquire = x.command == "cell_admission_acquire";
  const bool release = x.command == "cell_admission_release";
  const bool reconcile = x.command == "cell_admission_reconcile";
  const bool recovery_receipt =
      x.command == "cell_admission_recovery_receipt";
  if (!get && !readiness && !liveness && !acquire && !release && !reconcile &&
      !recovery_receipt)
    Fail("unknown cell-admission command");
  if ((get && !std::holds_alternative<std::monostate>(x.request)) ||
      (readiness &&
       !std::holds_alternative<core::CellAdmissionReadinessSnapshotDTO>(
           x.request)) ||
      (liveness &&
       !std::holds_alternative<core::ManagerLivenessObservationDTO>(
           x.request)) ||
      (acquire &&
       !std::holds_alternative<core::CellAdmissionClaimIntentDTO>(x.request)) ||
      (release &&
       !std::holds_alternative<core::CellAdmissionClaimReleaseDTO>(
           x.request)) ||
      (reconcile &&
       !std::holds_alternative<core::CellAdmissionReconcileDTO>(x.request)) ||
      (recovery_receipt &&
       !std::holds_alternative<
           core::CellAdmissionRecoveryReceiptQueryDTO>(x.request)))
    Fail("cell-admission command has the wrong request DTO");
  Writer w;
  w.out_.append(kMagic);
  w.String(x.command);
  std::visit(
      [&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (!std::is_same_v<T, std::monostate>) {
          if constexpr (std::is_same_v<T,
                                       core::CellAdmissionReadinessSnapshotDTO>)
            Readiness(w, v);
          else if constexpr (
              std::is_same_v<T, core::ManagerLivenessObservationDTO>)
            w.String(EncodeManagerLivenessRpcResponse(v));
          else if constexpr (std::is_same_v<T,
                                            core::CellAdmissionClaimIntentDTO>)
            Intent(w, v);
          else if constexpr (std::is_same_v<T,
                                            core::CellAdmissionClaimReleaseDTO>)
            Release(w, v);
          else if constexpr (std::is_same_v<
                                 T, core::CellAdmissionReconcileDTO>)
            Reconcile(w, v);
          else
            RecoveryQuery(w, v);
        }
      },
      x.request);
  return std::move(w).Finish();
}
KernelCellAdmissionRpcPacket
DecodeKernelCellAdmissionRpcRequest(std::string_view b) {
  Reader r(b);
  auto command = r.String();
  KernelCellAdmissionRpcRequest x;
  if (command == "cell_admission_get")
    x = std::monostate{};
  else if (command == "cell_admission_update_readiness")
    x = Readiness(r);
  else if (command == "cell_admission_publish_liveness")
    x = DecodeManagerLivenessRpcResponse(r.String());
  else if (command == "cell_admission_acquire") {
    auto v = Intent(r);
    if (!core::ValidateCellAdmissionClaimIntent(v).ok())
      Fail("invalid claim intent");
    x = std::move(v);
  } else if (command == "cell_admission_release") {
    auto v = Release(r);
    if (!core::ValidateCellAdmissionClaimRelease(v).ok())
      Fail("invalid claim release");
    x = std::move(v);
  } else if (command == "cell_admission_reconcile")
    x = Reconcile(r);
  else if (command == "cell_admission_recovery_receipt")
    x = RecoveryQuery(r);
  else
    Fail("unknown cell-admission command");
  r.Finish();
  return {std::move(command), std::move(x)};
}
std::string
EncodeKernelCellAdmissionRpcResponse(std::string_view command,
                                     const KernelCellAdmissionRpcResponse &x) {
  if (command != "cell_admission_get" &&
      command != "cell_admission_update_readiness" &&
      command != "cell_admission_publish_liveness" &&
      command != "cell_admission_acquire" &&
      command != "cell_admission_release" &&
      command != "cell_admission_reconcile" &&
      command != "cell_admission_recovery_receipt")
    Fail("unknown response command");
  if (command == "cell_admission_acquire" &&
      !std::holds_alternative<core::CellAdmissionClaimDTO>(x) &&
      !std::holds_alternative<core::ErrorDTO>(x))
    Fail("acquire response has the wrong DTO");
  if (command == "cell_admission_recovery_receipt" &&
      !std::holds_alternative<core::CellAdmissionRecoveryReceiptDTO>(x) &&
      !std::holds_alternative<core::ErrorDTO>(x))
    Fail("recovery receipt response has the wrong DTO");
  if (command == "cell_admission_publish_liveness" &&
      !std::holds_alternative<std::monostate>(x) &&
      !std::holds_alternative<core::ErrorDTO>(x))
    Fail("liveness publication response has the wrong DTO");
  if (command != "cell_admission_acquire" &&
      command != "cell_admission_recovery_receipt" &&
      command != "cell_admission_publish_liveness" &&
      !std::holds_alternative<core::AuthoringAdmissionDTO>(x) &&
      !std::holds_alternative<core::ErrorDTO>(x))
    Fail("response has the wrong DTO");
  Writer w;
  w.out_.append(kMagic);
  w.String(command);
  std::visit(
      [&](const auto &v) {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          w.Byte(4);
        } else if constexpr (std::is_same_v<T, core::AuthoringAdmissionDTO>) {
          w.Byte(0);
          w.Bool(v.authoring_allowed);
          G(w, v.admission_generation);
        } else if constexpr (std::is_same_v<T, core::CellAdmissionClaimDTO>) {
          w.Byte(1);
          Claim(w, v);
        } else if constexpr (std::is_same_v<
                                 T, core::CellAdmissionRecoveryReceiptDTO>) {
          w.Byte(3);
          RecoveryReceipt(w, v);
        } else {
          w.Byte(2);
          w.String(v.code);
          w.String(v.message);
          if (v.details.size() > kMaxEntries)
            Fail("error details too large");
          w.U64(v.details.size());
          for (auto &[k, val] : v.details) {
            w.String(k);
            w.String(val);
          }
        }
      },
      x);
  return std::move(w).Finish();
}
KernelCellAdmissionRpcResponsePacket
DecodeKernelCellAdmissionRpcResponse(std::string_view b) {
  Reader r(b);
  auto command = r.String();
  if (command != "cell_admission_get" &&
      command != "cell_admission_update_readiness" &&
      command != "cell_admission_publish_liveness" &&
      command != "cell_admission_acquire" &&
      command != "cell_admission_release" &&
      command != "cell_admission_reconcile" &&
      command != "cell_admission_recovery_receipt")
    Fail("unknown response command");
  auto tag = r.Byte();
  KernelCellAdmissionRpcResponse x;
  if (tag == 0)
    x = core::AuthoringAdmissionDTO{r.Bool(), G<core::AdmissionGeneration>(r)};
  else if (tag == 1)
    x = Claim(r);
  else if (tag == 2) {
    core::ErrorDTO e{r.String(), r.String(), {}};
    auto n = r.U64();
    if (n > kMaxEntries)
      Fail("error details too large");
    while (n--) {
      auto key = r.String();
      auto value = r.String();
      if (!e.details.emplace(std::move(key), std::move(value)).second)
        Fail("duplicate error detail");
    }
    x = e;
  } else if (tag == 3)
    x = RecoveryReceipt(r);
  else if (tag == 4)
    x = std::monostate{};
  else
    Fail("unknown response tag");
  if (const auto *claim = std::get_if<core::CellAdmissionClaimDTO>(&x);
      claim != nullptr &&
      (!core::ValidateCellAdmissionClaimIntent(claim->intent).ok() ||
       claim->claim_generation.value() == 0U ||
       claim->acquired_admission_generation.value() == 0U))
    Fail("invalid response claim");
  r.Finish();
  return {std::move(command), std::move(x)};
}
}

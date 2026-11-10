#include <cmath>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/stop_journal_projection_codec.hpp>
#include <manyforge_core/stop_terminal_reducer.hpp>
#include <manyforge_core/stop_terminal_state.hpp>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;
using V = CanonicalValue;
using A = CanonicalValue::Array;

constexpr std::uint64_t kMaxSafeUnsigned = 9'007'199'254'740'991ULL;
constexpr std::size_t kMaxProjectionBytes = 16U * 1024U * 1024U;

class CodecError final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

[[noreturn]] void bad(std::string_view reason) {
  throw CodecError(std::string(reason));
}

template <typename T>
[[nodiscard]] Result<T> reject(std::string_view reason) {
  return Result<T>::Err(ErrorDTO{"invalid_stop_journal_projection",
                                 "Stop journal projection is invalid",
                                 {{"reason", std::string(reason)}}});
}

[[nodiscard]] V arr(std::initializer_list<V> values) {
  return V(A(values));
}

[[nodiscard]] const A& exact(const V& value, const std::size_t size,
                             const std::string_view context) {
  if (!value.is_array() || value.as_array().size() != size) {
    bad(std::string(context) + " has the wrong structural arity");
  }
  return value.as_array();
}

[[nodiscard]] std::string str(const V& value, const std::string_view context) {
  if (!value.is_string()) {
    bad(std::string(context) + " is not a string");
  }
  return value.as_string();
}

[[nodiscard]] bool boolean(const V& value, const std::string_view context) {
  if (!value.is_bool()) {
    bad(std::string(context) + " is not a boolean");
  }
  return value.as_bool();
}

[[nodiscard]] double number(const V& value, const std::string_view context) {
  if (!value.is_number() || !std::isfinite(value.as_number())) {
    bad(std::string(context) + " is not a finite number");
  }
  return value.as_number();
}

[[nodiscard]] std::uint64_t u64(const V& value, const std::string_view context) {
  const auto result = number(value, context);
  if (result < 0.0 || result > static_cast<double>(kMaxSafeUnsigned) ||
      std::trunc(result) != result) {
    bad(std::string(context) + " is not an I-JSON unsigned integer");
  }
  return static_cast<std::uint64_t>(result);
}

[[nodiscard]] std::int64_t i64(const V& value, const std::string_view context) {
  try {
    return content_identity::parse_nonnegative_int64(value);
  } catch (const content_identity::CanonicalValueError&) {
    bad(std::string(context) + " is not an exact nonnegative int64");
  }
}

template <typename Tag>
[[nodiscard]] V enc(const StrongString<Tag>& value) {
  return V(value.value());
}

template <typename Tag>
[[nodiscard]] V enc(const StrongCounter<Tag>& value) {
  return V(static_cast<double>(value.value()));
}

template <typename T>
[[nodiscard]] T did(const V& value, const std::string_view context) {
  if constexpr (requires(T candidate) { candidate.value(); } &&
                std::is_same_v<decltype(std::declval<T>().value()), const std::string&>) {
    return T(str(value, context));
  } else {
    return T(u64(value, context));
  }
}

template <typename E>
  requires std::is_enum_v<E>
[[nodiscard]] V enc(const E value) {
  return V(static_cast<double>(static_cast<std::underlying_type_t<E>>(value)));
}

inline constexpr auto id_value = [](const auto& value) { return enc(value); };

template <typename E>
[[nodiscard]] E den(const V& value, const std::uint64_t maximum, const std::string_view context) {
  const auto raw = u64(value, context);
  if (raw > maximum) {
    bad(std::string(context) + " has an unknown enum value");
  }
  return static_cast<E>(raw);
}

template <typename T, typename Encode>
[[nodiscard]] V opt(const std::optional<T>& value, Encode encode) {
  return value.has_value() ? encode(*value) : V(nullptr);
}

template <typename T, typename Decode>
[[nodiscard]] std::optional<T> dopt(const V& value, Decode decode) {
  return value.is_null() ? std::nullopt : std::optional<T>(decode(value));
}

template <typename T, typename Encode>
[[nodiscard]] V vec(const std::vector<T>& values, Encode encode) {
  A result;
  result.reserve(values.size());
  for (const auto& value : values) {
    result.emplace_back(encode(value));
  }
  return V(std::move(result));
}

template <typename T, typename Decode>
[[nodiscard]] std::vector<T> dvec(const V& value, Decode decode, const std::string_view context) {
  if (!value.is_array()) {
    bad(std::string(context) + " is not an array");
  }
  std::vector<T> result;
  result.reserve(value.as_array().size());
  for (const auto& item : value.as_array()) {
    result.emplace_back(decode(item));
  }
  return result;
}

[[nodiscard]] V error_value(const ErrorDTO& error) {
  A details;
  details.reserve(error.details.size());
  for (const auto& [key, value] : error.details) {
    details.emplace_back(arr({V(key), V(value)}));
  }
  return arr({V(error.code), V(error.message), V(std::move(details))});
}

[[nodiscard]] ErrorDTO error_from(const V& value) {
  const auto& fields = exact(value, 3U, "error");
  ErrorDTO result{str(fields[0], "error.code"), str(fields[1], "error.message"), {}};
  if (!fields[2].is_array()) {
    bad("error.details is not an array");
  }
  for (const auto& item : fields[2].as_array()) {
    const auto& row = exact(item, 2U, "error detail");
    if (!result.details.emplace(str(row[0], "error detail key"), str(row[1], "error detail value"))
             .second) {
      bad("error details contain duplicate keys");
    }
  }
  return result;
}

[[nodiscard]] V json_value(const JsonValueDTO& value) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, std::nullptr_t>) {
          return arr({V(0.0), V(nullptr)});
        } else if constexpr (std::is_same_v<T, bool>) {
          return arr({V(1.0), V(current)});
        } else if constexpr (std::is_same_v<T, double>) {
          return arr({V(2.0), V(current)});
        } else if constexpr (std::is_same_v<T, std::string>) {
          return arr({V(3.0), V(current)});
        } else if constexpr (std::is_same_v<T, JsonValueDTO::Array>) {
          return arr({V(4.0), vec(current, json_value)});
        } else {
          A rows;
          rows.reserve(current.size());
          for (const auto& [key, item] : current) {
            rows.emplace_back(arr({V(key), json_value(item)}));
          }
          return arr({V(5.0), V(std::move(rows))});
        }
      },
      value.value);
}

[[nodiscard]] JsonValueDTO json_from(const V& value) {
  const auto& fields = exact(value, 2U, "json value");
  switch (u64(fields[0], "json value kind")) {
    case 0U:
      if (!fields[1].is_null()) bad("json null payload is not null");
      return JsonValueDTO{nullptr};
    case 1U:
      return JsonValueDTO{boolean(fields[1], "json boolean")};
    case 2U:
      return JsonValueDTO{number(fields[1], "json number")};
    case 3U:
      return JsonValueDTO{str(fields[1], "json string")};
    case 4U:
      return JsonValueDTO{dvec<JsonValueDTO>(fields[1], json_from, "json array")};
    case 5U: {
      if (!fields[1].is_array()) bad("json object is not an array");
      JsonObjectDTO object;
      for (const auto& item : fields[1].as_array()) {
        const auto& row = exact(item, 2U, "json object member");
        if (!object.emplace(str(row[0], "json object key"), json_from(row[1])).second) {
          bad("json object contains duplicate keys");
        }
      }
      return JsonValueDTO{std::move(object)};
    }
    default:
      bad("json value has an unknown kind");
  }
}

[[nodiscard]] V stale_target_value(const StopScopeDiagnosticTargetDTO& value) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return arr({V(0.0), V(nullptr)});
        } else if constexpr (std::is_same_v<T, OperationId>) {
          return arr({V(1.0), enc(current)});
        } else if constexpr (std::is_same_v<T, ControllableComponentId>) {
          return arr({V(2.0), enc(current)});
        } else {
          return arr({V(3.0), enc(current)});
        }
      },
      value);
}

[[nodiscard]] StopScopeDiagnosticTargetDTO stale_target_from(const V& value) {
  const auto& f = exact(value, 2U, "stale target");
  switch (u64(f[0], "stale target kind")) {
    case 0U:
      if (!f[1].is_null()) bad("empty stale target has a payload");
      return std::monostate{};
    case 1U:
      return did<OperationId>(f[1], "operation ID");
    case 2U:
      return did<ControllableComponentId>(f[1], "component ID");
    case 3U:
      return did<ResourceId>(f[1], "resource ID");
    default:
      bad("stale target has an unknown kind");
  }
}

[[nodiscard]] V stale_value_value(const StopScopeStaleValueDTO& value) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, std::uint64_t>) {
          return arr({V(0.0), V(static_cast<double>(current))});
        } else if constexpr (std::is_same_v<T, RequestOwnerEpoch>) {
          return arr({V(1.0), enc(current)});
        } else if constexpr (std::is_same_v<T, GraphRevision>) {
          return arr({V(2.0), enc(current)});
        } else if constexpr (std::is_same_v<T, OperationRevision>) {
          return arr({V(3.0), enc(current)});
        } else if constexpr (std::is_same_v<T, ComponentLifecycleGeneration>) {
          return arr({V(4.0), enc(current)});
        } else {
          return arr({V(5.0), enc(current)});
        }
      },
      value);
}

[[nodiscard]] StopScopeStaleValueDTO stale_value_from(const V& value) {
  const auto& f = exact(value, 2U, "stale value");
  switch (u64(f[0], "stale value kind")) {
    case 0U:
      return u64(f[1], "stale unsigned value");
    case 1U:
      return did<RequestOwnerEpoch>(f[1], "owner epoch");
    case 2U:
      return did<GraphRevision>(f[1], "graph revision");
    case 3U:
      return did<OperationRevision>(f[1], "operation revision");
    case 4U:
      return did<ComponentLifecycleGeneration>(f[1], "lifecycle generation");
    case 5U:
      return did<StopGeneration>(f[1], "Stop generation");
    default:
      bad("stale value has an unknown kind");
  }
}

[[nodiscard]] V stale_diagnostic_value(const StopScopeStaleDiagnosticDTO& value) {
  return arr({enc(value.field), stale_target_value(value.target), stale_value_value(value.expected),
              stale_value_value(value.current)});
}

[[nodiscard]] StopScopeStaleDiagnosticDTO stale_diagnostic_from(const V& value) {
  const auto& f = exact(value, 4U, "stale diagnostic");
  return StopScopeStaleDiagnosticDTO{den<StopScopeStaleFieldDTO>(f[0], 7U, "stale field"),
                                     stale_target_from(f[1]), stale_value_from(f[2]),
                                     stale_value_from(f[3])};
}

[[nodiscard]] V accepted_value(const StopAcceptedDTO& value) {
  return arr(
      {enc(value.request_id), enc(value.replay_status), enc(value.stop_id),
       enc(value.stop_generation), enc(value.requested_selector_digest),
       enc(value.resolution_disposition), enc(value.resolution_reason),
       vec(value.stale_diagnostics, stale_diagnostic_value), enc(value.captured_scope_digest),
       vec(value.joined_predecessor_stops,
           [](const auto& row) {
             return arr({enc(row.predecessor_stop_id), enc(row.predecessor_stop_generation),
                         enc(row.captured_scope_digest)});
           }),
       vec(value.affected_operation_revisions,
           [](const auto& row) {
             return arr({enc(row.operation_id), enc(row.operation_revision_after_acceptance)});
           }),
       content_identity::nonnegative_int64_value(value.accepted_at_ns)});
}

[[nodiscard]] StopAcceptedDTO accepted_from(const V& value) {
  const auto& f = exact(value, 12U, "Stop accepted");
  auto predecessors = dvec<StopPredecessorAcceptanceRefDTO>(
      f[9],
      [](const V& item) {
        const auto& row = exact(item, 3U, "predecessor acceptance");
        return StopPredecessorAcceptanceRefDTO{
            did<StopId>(row[0], "predecessor Stop ID"),
            did<StopGeneration>(row[1], "predecessor Stop generation"),
            did<ContentDigest>(row[2], "predecessor scope digest")};
      },
      "predecessors");
  auto affected = dvec<StopAffectedOperationRevisionDTO>(
      f[10],
      [](const V& item) {
        const auto& row = exact(item, 2U, "affected operation revision");
        return StopAffectedOperationRevisionDTO{
            did<OperationId>(row[0], "affected operation ID"),
            did<OperationRevision>(row[1], "affected operation revision")};
      },
      "affected operation revisions");
  return StopAcceptedDTO{
      did<RequestId>(f[0], "request ID"),
      den<RequestReplayStatusDTO>(f[1], 5U, "replay status"),
      did<StopId>(f[2], "Stop ID"),
      did<StopGeneration>(f[3], "Stop generation"),
      did<ContentDigest>(f[4], "selector digest"),
      den<StopResolutionDispositionDTO>(f[5], 3U, "resolution disposition"),
      den<StopScopeResolutionReasonDTO>(f[6], 10U, "resolution reason"),
      dvec<StopScopeStaleDiagnosticDTO>(f[7], stale_diagnostic_from, "stale diagnostics"),
      did<ContentDigest>(f[8], "captured scope digest"),
      std::move(predecessors),
      std::move(affected),
      i64(f[11], "accepted time")};
}

[[nodiscard]] V artifact_value(const RetainedArtifactRefDTO& value) {
  return arr({enc(value.artifact_role_id), V(value.media_type),
              V(static_cast<double>(value.byte_size)), enc(value.content_digest),
              enc(value.content_addressed_store_key)});
}

[[nodiscard]] RetainedArtifactRefDTO artifact_from(const V& value) {
  const auto& f = exact(value, 5U, "artifact reference");
  return RetainedArtifactRefDTO{did<ArtifactRoleId>(f[0], "artifact role"),
                                str(f[1], "artifact media type"), u64(f[2], "artifact byte size"),
                                did<ContentDigest>(f[3], "artifact digest"),
                                did<ContentAddressedStoreKey>(f[4], "artifact store key")};
}

[[nodiscard]] V contract_ref_value(const ProviderContractRefDTO& value) {
  return arr({enc(value.provider_contract_role_id), enc(value.contract_schema_id),
              enc(value.contract_schema_revision)});
}

[[nodiscard]] ProviderContractRefDTO contract_ref_from(const V& value) {
  const auto& f = exact(value, 3U, "provider contract reference");
  return ProviderContractRefDTO{did<ProviderContractRoleId>(f[0], "contract role"),
                                did<SchemaId>(f[1], "contract schema ID"),
                                did<SchemaRevision>(f[2], "contract schema revision")};
}

[[nodiscard]] V runtime_target_value(const ResolvedStopRuntimeTargetDTO& value) {
  return arr({enc(value.provider_instance_id), enc(value.provider_manifest_id),
              enc(value.provider_manifest_revision), enc(value.runtime_unit_id),
              enc(value.product_id), enc(value.product_contract_id),
              enc(value.product_contract_revision), enc(value.content_digest),
              enc(value.provider_runtime_epoch), enc(value.runtime_unit_epoch),
              opt(value.endpoint_role_id, id_value),
              opt(value.logical_endpoint_symbol, [](const std::string& item) { return V(item); }),
              opt(value.external_resource_alias, id_value),
              opt(value.external_resource_kind, [](const auto item) { return enc(item); }),
              opt(value.site_overlay_id, id_value), opt(value.site_overlay_revision, id_value),
              opt(value.site_overlay_non_secret_digest, id_value),
              opt(value.site_overlay_value_ref, id_value)});
}

[[nodiscard]] ResolvedStopRuntimeTargetDTO runtime_target_from(const V& value) {
  const auto& f = exact(value, 18U, "runtime target");
  return ResolvedStopRuntimeTargetDTO{
      did<ProviderInstanceId>(f[0], "provider instance ID"),
      did<ProviderManifestId>(f[1], "provider manifest ID"),
      did<ProviderManifestRevision>(f[2], "provider manifest revision"),
      did<RuntimeUnitId>(f[3], "runtime unit ID"),
      did<ProductId>(f[4], "product ID"),
      did<ProductContractId>(f[5], "product contract ID"),
      did<ProductContractRevision>(f[6], "product contract revision"),
      did<ContentDigest>(f[7], "product digest"),
      did<RuntimeEpoch>(f[8], "provider runtime epoch"),
      did<RuntimeEpoch>(f[9], "runtime-unit epoch"),
      dopt<EndpointRoleId>(f[10],
                           [](const V& v) { return did<EndpointRoleId>(v, "endpoint role"); }),
      dopt<std::string>(f[11], [](const V& v) { return str(v, "endpoint symbol"); }),
      dopt<ExternalResourceAlias>(
          f[12], [](const V& v) { return did<ExternalResourceAlias>(v, "external alias"); }),
      dopt<ExternalResourceKind>(
          f[13], [](const V& v) { return den<ExternalResourceKind>(v, 3U, "external kind"); }),
      dopt<SiteOverlayId>(f[14],
                          [](const V& v) { return did<SiteOverlayId>(v, "site overlay ID"); }),
      dopt<SiteOverlayRevision>(
          f[15], [](const V& v) { return did<SiteOverlayRevision>(v, "site overlay revision"); }),
      dopt<ContentDigest>(f[16],
                          [](const V& v) { return did<ContentDigest>(v, "site overlay digest"); }),
      dopt<SiteOverlayValueRef>(
          f[17], [](const V& v) { return did<SiteOverlayValueRef>(v, "site overlay value ref"); })};
}

[[nodiscard]] V substitution_contract_value(const ControlledStopSubstitutionContractDTO& v) {
  return arr({contract_ref_value(v.provider_contract_ref), enc(v.requested_stop_mode),
              vec(v.fallback_stop_modes, [](const auto mode) { return enc(mode); })});
}

[[nodiscard]] ControlledStopSubstitutionContractDTO substitution_contract_from(const V& v) {
  const auto& f = exact(v, 3U, "substitution contract");
  const auto primary = den<MotionControlledStopMode>(f[1], 3U, "requested Stop mode");
  auto modes = dvec<MotionControlledStopMode>(
      f[2],
      [](const V& mode) { return den<MotionControlledStopMode>(mode, 3U, "fallback Stop mode"); },
      "fallback modes");
  if (modes.empty() || modes.size() > 2U) bad("invalid fallback count");
  if (!motion_controlled_stop_fallbacks_valid(primary, modes)) bad("invalid fallback order");
  return {contract_ref_from(f[0]), primary, std::move(modes)};
}

[[nodiscard]] V command_qualification_value(const ResolvedStopCommandQualificationDTO& v) {
  return arr({enc(v.stop_action_class),
              opt(v.controlled_substitution_contract, substitution_contract_value),
              opt(v.safety_system_request_action_class, [](const auto item) { return enc(item); }),
              opt(v.commissioned_stop_interface_id, id_value),
              opt(v.commissioned_stop_interface_revision, id_value),
              opt(v.commissioning_evidence_artifact_ref, artifact_value)});
}

[[nodiscard]] ResolvedStopCommandQualificationDTO command_qualification_from(const V& v) {
  const auto& f = exact(v, 6U, "command qualification");
  return ResolvedStopCommandQualificationDTO{
      den<StopActionClass>(f[0], 2U, "Stop action class"),
      dopt<ControlledStopSubstitutionContractDTO>(f[1], substitution_contract_from),
      dopt<SafetySystemRequestActionClass>(f[2],
                                           [](const V& item) {
                                             return den<SafetySystemRequestActionClass>(
                                                 item, 1U, "safety action class");
                                           }),
      dopt<CommissionedStopInterfaceId>(f[3],
                                        [](const V& item) {
                                          return did<CommissionedStopInterfaceId>(
                                              item, "commissioned interface ID");
                                        }),
      dopt<CommissionedStopInterfaceRevision>(f[4],
                                              [](const V& item) {
                                                return did<CommissionedStopInterfaceRevision>(
                                                    item, "commissioned interface revision");
                                              }),
      dopt<RetainedArtifactRefDTO>(f[5], artifact_from)};
}

[[nodiscard]] V containment_command_value(const StopContainmentCommandDTO& v) {
  return arr({enc(v.command_kind), opt(v.provider_contract_ref, contract_ref_value),
              opt(v.registered_command_kind_id, id_value)});
}

[[nodiscard]] StopContainmentCommandDTO containment_command_from(const V& v) {
  const auto& f = exact(v, 3U, "containment command");
  return StopContainmentCommandDTO{
      den<StopContainmentCommandKind>(f[0], 5U, "containment command kind"),
      dopt<ProviderContractRefDTO>(f[1], contract_ref_from),
      dopt<RegisteredStopCommandKindId>(f[2], [](const V& item) {
        return did<RegisteredStopCommandKindId>(item, "registered command kind");
      })};
}

[[nodiscard]] V evidence_source_requirement_value(const StopEvidenceSourceRequirementDTO& v) {
  return arr(
      {enc(v.source_kind), content_identity::nonnegative_int64_value(v.freshness_budget_ns),
       opt(v.endpoint_role_id, id_value),
       vec(v.allowed_authority_classes, [](const StateAuthorityClass item) { return enc(item); })});
}

[[nodiscard]] StopEvidenceSourceRequirementDTO evidence_source_requirement_from(const V& v) {
  const auto& f = exact(v, 4U, "evidence source requirement");
  return StopEvidenceSourceRequirementDTO{
      den<StopEvidenceSourceKind>(f[0], 3U, "evidence source kind"), i64(f[1], "freshness budget"),
      dopt<EndpointRoleId>(
          f[2], [](const V& item) { return did<EndpointRoleId>(item, "endpoint role"); }),
      dvec<StateAuthorityClass>(
          f[3], [](const V& item) { return den<StateAuthorityClass>(item, 2U, "authority class"); },
          "allowed authority classes")};
}

[[nodiscard]] V evidence_requirement_value(const StopEvidenceRequirementDTO& v) {
  return arr({enc(v.evidence_item_id), enc(v.evidence_kind),
              opt(v.provider_contract_ref, contract_ref_value),
              opt(v.registered_evidence_kind_id, id_value),
              evidence_source_requirement_value(v.source_requirement),
              arr({enc(v.observation_semantics.observation_kind),
                   opt(v.observation_semantics.minimum_dwell_ns, [](const std::int64_t item) {
                     return content_identity::nonnegative_int64_value(item);
                   })})});
}

[[nodiscard]] StopEvidenceRequirementDTO evidence_requirement_from(const V& v) {
  const auto& f = exact(v, 6U, "evidence requirement");
  const auto& observation = exact(f[5], 2U, "evidence observation");
  return StopEvidenceRequirementDTO{
      did<StopEvidenceItemId>(f[0], "evidence item ID"),
      den<StopEvidenceRequirementKind>(f[1], 5U, "evidence kind"),
      dopt<ProviderContractRefDTO>(f[2], contract_ref_from),
      dopt<RegisteredStopEvidenceKindId>(f[3],
                                         [](const V& item) {
                                           return did<RegisteredStopEvidenceKindId>(
                                               item, "registered evidence kind");
                                         }),
      evidence_source_requirement_from(f[4]),
      StopEvidenceObservationDTO{
          den<StopEvidenceObservationKind>(observation[0], 2U, "observation kind"),
          dopt<std::int64_t>(observation[1],
                             [](const V& item) { return i64(item, "minimum dwell"); })}};
}

[[nodiscard]] V resolved_evidence_source_value(const ResolvedStopEvidenceSourceDTO& v) {
  return arr({enc(v.source_kind), runtime_target_value(v.runtime_target),
              opt(v.state_authority_id, id_value),
              opt(v.state_authority_class, [](const auto item) { return enc(item); }),
              opt(v.provider_contract_ref, contract_ref_value)});
}

[[nodiscard]] ResolvedStopEvidenceSourceDTO resolved_evidence_source_from(const V& v) {
  const auto& f = exact(v, 5U, "resolved evidence source");
  return ResolvedStopEvidenceSourceDTO{
      den<StopEvidenceSourceKind>(f[0], 3U, "evidence source kind"), runtime_target_from(f[1]),
      dopt<StateAuthorityId>(
          f[2], [](const V& item) { return did<StateAuthorityId>(item, "state authority ID"); }),
      dopt<StateAuthorityClass>(f[3],
                                [](const V& item) {
                                  return den<StateAuthorityClass>(item, 2U,
                                                                  "state authority class");
                                }),
      dopt<ProviderContractRefDTO>(f[4], contract_ref_from)};
}

[[nodiscard]] V containment_scope_value(const ResolvedStopContainmentScopeDTO& v) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, ResolvedStopResourceContainmentScopeDTO>) {
          return arr(
              {V(0.0), arr({enc(current.activation_set_id), vec(current.resource_ids, id_value)})});
        } else if constexpr (std::is_same_v<T, ResolvedStopControllerSetContainmentScopeDTO>) {
          return arr(
              {V(1.0),
               arr({enc(current.activation_set_id), vec(current.controllers, [](const auto& row) {
                      return arr({enc(row.controller_role_id), V(row.logical_controller_symbol)});
                    })})});
        } else {
          return arr({V(2.0), arr({enc(current.activation_set_id), enc(current.control_domain_id),
                                   enc(current.control_domain_revision)})});
        }
      },
      v);
}

[[nodiscard]] ResolvedStopContainmentScopeDTO containment_scope_from(const V& v) {
  const auto& f = exact(v, 2U, "containment scope");
  switch (u64(f[0], "containment scope kind")) {
    case 0U: {
      const auto& row = exact(f[1], 2U, "resource containment scope");
      return ResolvedStopResourceContainmentScopeDTO{
          did<ActivationSetId>(row[0], "activation set ID"),
          dvec<ResourceId>(
              row[1], [](const V& item) { return did<ResourceId>(item, "resource ID"); },
              "scope resources")};
    }
    case 1U: {
      const auto& row = exact(f[1], 2U, "controller-set containment scope");
      return ResolvedStopControllerSetContainmentScopeDTO{
          did<ActivationSetId>(row[0], "activation set ID"),
          dvec<ResolvedStopControllerContainmentRowDTO>(
              row[1],
              [](const V& item) {
                const auto& controller = exact(item, 2U, "controller containment row");
                return ResolvedStopControllerContainmentRowDTO{
                    did<ControllerRoleId>(controller[0], "controller role ID"),
                    str(controller[1], "controller symbol")};
              },
              "controllers")};
    }
    case 2U: {
      const auto& row = exact(f[1], 3U, "control-domain containment scope");
      return ResolvedStopControlDomainContainmentScopeDTO{
          did<ActivationSetId>(row[0], "activation set ID"),
          did<ControlDomainId>(row[1], "control domain ID"),
          did<ControlDomainRevision>(row[2], "control domain revision")};
    }
    default:
      bad("containment scope has an unknown kind");
  }
}

[[nodiscard]] V resolved_stage_value(const ResolvedStopEscalationStageDTO& v) {
  return arr(
      {V(static_cast<double>(v.stage_index)), enc(v.stop_escalation_stage_id), enc(v.stage_kind),
       enc(v.containment_granularity), containment_command_value(v.containment_command),
       opt(v.default_contract_parameters,
           [](const JsonObjectDTO& item) { return json_value(JsonValueDTO{item}); }),
       command_qualification_value(v.command_qualification), runtime_target_value(v.command_target),
       containment_scope_value(v.containment_scope), enc(v.evidence_conjunction),
       vec(v.evidence_requirements, [](const ResolvedStopEvidenceRequirementDTO& row) {
         return arr({evidence_requirement_value(row.requirement),
                     resolved_evidence_source_value(row.resolved_source)});
       })});
}

[[nodiscard]] ResolvedStopEscalationStageDTO resolved_stage_from(const V& v) {
  const auto& f = exact(v, 11U, "resolved escalation stage");
  auto parameters = dopt<JsonObjectDTO>(f[5], [](const V& item) {
    auto decoded = json_from(item);
    if (!std::holds_alternative<JsonObjectDTO>(decoded.value)) {
      bad("default contract parameters are not an object");
    }
    auto parameters = std::get<JsonObjectDTO>(std::move(decoded.value));
    if (parameters.contains("recoverable_substitution_mode")) {
      bad("legacy Stop fallback parameters are refused");
    }
    return parameters;
  });
  return ResolvedStopEscalationStageDTO{
      u64(f[0], "stage index"),
      did<StopEscalationStageId>(f[1], "stage ID"),
      den<StopEscalationStageKind>(f[2], 3U, "stage kind"),
      den<ContainmentGranularity>(f[3], 2U, "containment granularity"),
      containment_command_from(f[4]),
      std::move(parameters),
      command_qualification_from(f[6]),
      runtime_target_from(f[7]),
      containment_scope_from(f[8]),
      den<StopEvidenceConjunction>(f[9], 0U, "evidence conjunction"),
      dvec<ResolvedStopEvidenceRequirementDTO>(
          f[10],
          [](const V& item) {
            const auto& row = exact(item, 2U, "resolved evidence requirement");
            return ResolvedStopEvidenceRequirementDTO{evidence_requirement_from(row[0]),
                                                      resolved_evidence_source_from(row[1])};
          },
          "resolved evidence requirements")};
}

[[nodiscard]] V coordinate_acceleration_value(const CoordinateAccelerationDTO& v) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, AngularAccelerationDTO>) {
          return arr({V(0.0), V(current.value_rad_s2)});
        } else {
          return arr({V(1.0), V(current.value_m_s2)});
        }
      },
      v);
}

[[nodiscard]] CoordinateAccelerationDTO coordinate_acceleration_from(const V& v) {
  const auto& f = exact(v, 2U, "coordinate acceleration");
  const auto kind = u64(f[0], "coordinate acceleration kind");
  if (kind == 0U) return AngularAccelerationDTO{number(f[1], "angular acceleration")};
  if (kind == 1U) return LinearAccelerationDTO{number(f[1], "linear acceleration")};
  bad("coordinate acceleration has an unknown kind");
}

[[nodiscard]] V coordinate_velocity_value(const CoordinateVelocityDTO& v) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, AngularVelocityDTO>) {
          return arr({V(0.0), V(current.value_rad_s)});
        } else {
          return arr({V(1.0), V(current.value_m_s)});
        }
      },
      v);
}

[[nodiscard]] CoordinateVelocityDTO coordinate_velocity_from(const V& v) {
  const auto& f = exact(v, 2U, "coordinate velocity");
  const auto kind = u64(f[0], "coordinate velocity kind");
  if (kind == 0U) return AngularVelocityDTO{number(f[1], "angular velocity")};
  if (kind == 1U) return LinearVelocityDTO{number(f[1], "linear velocity")};
  bad("coordinate velocity has an unknown kind");
}

[[nodiscard]] V structured_value(const InterruptStructuredValue& v) {
  A rows;
  rows.reserve(v.size());
  for (const auto& [key, scalar] : v) {
    rows.emplace_back(std::visit(
        [&](const auto& current) -> V {
          using T = std::decay_t<decltype(current)>;
          if constexpr (std::is_same_v<T, bool>) {
            return arr({V(key), V(0.0), V(current)});
          } else if constexpr (std::is_same_v<T, double>) {
            return arr({V(key), V(1.0), V(current)});
          } else {
            return arr({V(key), V(2.0), V(current)});
          }
        },
        scalar));
  }
  return V(std::move(rows));
}

[[nodiscard]] InterruptStructuredValue structured_from(const V& v) {
  if (!v.is_array()) bad("structured diagnostics is not an array");
  InterruptStructuredValue result;
  for (const auto& item : v.as_array()) {
    const auto& row = exact(item, 3U, "structured diagnostic");
    const auto key = str(row[0], "structured diagnostic key");
    InterruptStructuredScalar scalar;
    switch (u64(row[1], "structured diagnostic kind")) {
      case 0U:
        scalar = boolean(row[2], "structured boolean");
        break;
      case 1U:
        scalar = number(row[2], "structured number");
        break;
      case 2U:
        scalar = str(row[2], "structured string");
        break;
      default:
        bad("structured diagnostic has an unknown kind");
    }
    if (!result.emplace(key, std::move(scalar)).second) {
      bad("structured diagnostics contain duplicate keys");
    }
  }
  return result;
}

[[nodiscard]] V motion_profile_value(const MotionInterruptStopProfileDTO& v) {
  A acceleration;
  acceleration.reserve(v.per_joint_max_deceleration.size());
  for (const auto& [joint, limit] : v.per_joint_max_deceleration) {
    acceleration.emplace_back(arr({V(joint), coordinate_acceleration_value(limit)}));
  }
  A fields{V(v.profile_id),
           enc(v.stop_mode),
           opt(v.deceleration_time_s, [](const double item) { return V(item); }),
           V(std::move(acceleration)),
           V(v.allow_natural_completion_on_short_path),
           v.fallback_stop_modes.empty()
               ? V(nullptr)
               : vec(v.fallback_stop_modes, [](const auto item) { return enc(item); }),
           V(v.require_backend_confirmation),
           opt(v.per_joint_settle_velocity_threshold,
               [](const auto& limits) {
                 A rows;
                 rows.reserve(limits.size());
                 for (const auto& [joint, limit] : limits) {
                   rows.emplace_back(arr({V(joint), coordinate_velocity_value(limit)}));
                 }
                 return V(std::move(rows));
               }),
           structured_value(v.diagnostics)};
  // Journal layout: twelve fields (jerk activation, jerk limits and the
  // per-profile `max_stop_time_s` always present); older layouts are refused.
  if (!v.jerk_limits_enabled && !v.per_joint_max_jerk.empty()) {
    bad("unreviewed Stop profile cannot carry new jerk limits");
  }
  fields.emplace_back(opt(v.jerk_limits_enabled, [](const bool item) { return V(item); }));
  {
    A limits;
    for (const auto& [joint, limit] : v.per_joint_max_jerk) {
      limits.emplace_back(arr({V(joint), std::visit(
                                             [](const auto& typed) -> V {
                                               if constexpr (requires { typed.value_rad_s3; })
                                                 return arr({V(0.0), V(typed.value_rad_s3)});
                                               else
                                                 return arr({V(1.0), V(typed.value_m_s3)});
                                             },
                                             limit)}));
    }
    fields.emplace_back(std::move(limits));
  }
  fields.emplace_back(opt(v.max_stop_time_s, [](const double item) { return V(item); }));
  return V(std::move(fields));
}

[[nodiscard]] MotionInterruptStopProfileDTO motion_profile_from(const V& v) {
  const auto& f = exact(v, 12U, "motion interrupt Stop profile");
  std::optional<bool> enabled =
      dopt<bool>(f[9], [](const V& item) { return boolean(item, "Stop jerk activation"); });
  std::map<std::string, CoordinateJerkDTO> jerk;
  {
    if (!f[10].is_array()) bad("per-joint jerk is not an array");
    for (const auto& item : f[10].as_array()) {
      const auto& row = exact(item, 2U, "per-joint jerk row");
      const auto& coordinate = exact(row[1], 2U, "coordinate jerk");
      const auto kind = u64(coordinate[0], "coordinate jerk kind");
      if (kind > 1U) bad("coordinate jerk kind is invalid");
      const double scalar = number(coordinate[1], "coordinate jerk");
      if (!jerk.emplace(str(row[0], "joint name"), kind == 0U
                                                       ? CoordinateJerkDTO{AngularJerkDTO{scalar}}
                                                       : CoordinateJerkDTO{LinearJerkDTO{scalar}})
               .second)
        bad("per-joint jerk contains duplicate joints");
    }
    if (!enabled.has_value() && !jerk.empty()) bad("unreviewed Stop profile carries jerk limits");
  }
  if (!f[3].is_array()) bad("per-joint deceleration is not an array");
  std::map<std::string, CoordinateAccelerationDTO> acceleration;
  for (const auto& item : f[3].as_array()) {
    const auto& row = exact(item, 2U, "per-joint deceleration row");
    if (!acceleration.emplace(str(row[0], "joint name"), coordinate_acceleration_from(row[1]))
             .second) {
      bad("per-joint deceleration contains duplicate joints");
    }
  }
  auto settle = dopt<std::map<std::string, CoordinateVelocityDTO>>(f[7], [](const V& value) {
    if (!value.is_array()) bad("per-joint settle thresholds are not an array");
    std::map<std::string, CoordinateVelocityDTO> result;
    for (const auto& item : value.as_array()) {
      const auto& row = exact(item, 2U, "per-joint settle row");
      if (!result.emplace(str(row[0], "joint name"), coordinate_velocity_from(row[1])).second) {
        bad("per-joint settle thresholds contain duplicate joints");
      }
    }
    return result;
  });
  auto result = MotionInterruptStopProfileDTO{
      str(f[0], "profile ID"),
      den<MotionInterruptStopMode>(f[1], 3U, "interrupt Stop mode"),
      dopt<double>(f[2], [](const V& item) { return number(item, "deceleration time"); }),
      dopt<double>(f[11], [](const V& item) { return number(item, "maximum Stop time"); }),
      std::move(acceleration),
      boolean(f[4], "allow natural completion"),
      {},
      boolean(f[6], "require backend confirmation"),
      std::move(settle),
      structured_from(f[8]),
      enabled,
      std::move(jerk)};
  if (!f[5].is_null()) {
    result.fallback_stop_modes = dvec<MotionInterruptStopMode>(
        f[5],
        [](const V& mode) { return den<MotionInterruptStopMode>(mode, 3U, "fallback Stop mode"); },
        "fallback modes");
    if (result.fallback_stop_modes.size() > 2U) bad("invalid fallback count");
  }
  if (!motion_interrupt_stop_fallbacks_valid(result.stop_mode, result.fallback_stop_modes))
    bad("invalid fallback order");
  return result;
}

[[nodiscard]] V prepared_stage_value(const PreparedStopStageCommandDTO& v) {
  return arr({resolved_stage_value(v.resolved_stage),
              opt(v.validated_contract_parameters,
                  [](const JsonObjectDTO& item) { return json_value(JsonValueDTO{item}); }),
              opt(v.parameter_source, [](const auto item) { return enc(item); })});
}

[[nodiscard]] PreparedStopStageCommandDTO prepared_stage_from(const V& v) {
  const auto& f = exact(v, 3U, "prepared stage command");
  auto parameters = dopt<JsonObjectDTO>(f[1], [](const V& item) {
    auto decoded = json_from(item);
    if (!std::holds_alternative<JsonObjectDTO>(decoded.value)) {
      bad("validated contract parameters are not an object");
    }
    auto parameters = std::get<JsonObjectDTO>(std::move(decoded.value));
    if (parameters.contains("recoverable_substitution_mode")) {
      bad("legacy Stop fallback parameters are refused");
    }
    return parameters;
  });
  return PreparedStopStageCommandDTO{resolved_stage_from(f[0]), std::move(parameters),
                                     dopt<StopStageParameterSourceDTO>(f[2], [](const V& item) {
                                       return den<StopStageParameterSourceDTO>(item, 1U,
                                                                               "parameter source");
                                     })};
}

[[nodiscard]] V prepared_plan_value(const PreparedParticipantStopPlanDTO& v) {
  return arr({enc(v.participant_stop_invocation_plan_digest), enc(v.resolved_stop_base_plan_id),
              enc(v.operation_id), enc(v.operation_revision), enc(v.participant_id),
              enc(v.program_id), enc(v.program_revision), enc(v.program_binding_id),
              enc(v.program_run_id), opt(v.participant_stop_profile, motion_profile_value),
              enc(v.participant_stop_profile_purpose),
              opt(v.motion_interrupt_scope_id, [](const std::string& item) { return V(item); }),
              vec(v.stage_commands, prepared_stage_value)});
}

[[nodiscard]] PreparedParticipantStopPlanDTO prepared_plan_from(const V& v) {
  const auto& f = exact(v, 13U, "prepared participant Stop plan");
  return PreparedParticipantStopPlanDTO{
      did<ContentDigest>(f[0], "participant plan digest"),
      did<ResolvedStopBasePlanId>(f[1], "resolved base-plan ID"),
      did<OperationId>(f[2], "operation ID"),
      did<OperationRevision>(f[3], "operation revision"),
      did<OperationParticipantId>(f[4], "participant ID"),
      did<ProgramId>(f[5], "program ID"),
      did<ProgramRevision>(f[6], "program revision"),
      did<ProgramBindingId>(f[7], "program binding ID"),
      did<ProgramRunId>(f[8], "program run ID"),
      dopt<MotionInterruptStopProfileDTO>(f[9], motion_profile_from),
      den<ParticipantStopProfilePurposeDTO>(f[10], 3U, "profile purpose"),
      dopt<std::string>(f[11], [](const V& item) { return str(item, "interrupt scope ID"); }),
      dvec<PreparedStopStageCommandDTO>(f[12], prepared_stage_from, "prepared stage commands")};
}

// Terminal v4 stores each distinct participant plan once, in the root `plans`
// list; an attempt's resolved stage names its plan by digest (slot 26).
class TerminalPlanIndex final {
 public:
  explicit TerminalPlanIndex(const V& wire) {
    if (!wire.is_array()) bad("Stop journal terminal plans are not an array");
    for (const auto& item : wire.as_array()) {
      auto plan = prepared_plan_from(item);
      auto digest = plan.participant_stop_invocation_plan_digest.value();
      if (!plans_.empty() && !(plans_.rbegin()->first < digest)) {
        bad("Stop journal terminal plans are not sorted and unique by digest");
      }
      plans_.emplace_hint(plans_.end(), std::move(digest), std::move(plan));
    }
  }

  [[nodiscard]] PreparedParticipantStopPlanDTO resolve(const V& digest_wire) {
    const auto digest = str(digest_wire, "participant plan digest");
    const auto found = plans_.find(digest);
    if (found == plans_.end()) bad("Stop journal terminal attempt names an absent participant plan");
    referenced_.insert(digest);
    return found->second;
  }

  void require_all_referenced() const {
    if (referenced_.size() != plans_.size()) {
      bad("Stop journal terminal projection retains an unreferenced participant plan");
    }
  }

 private:
  std::map<std::string, PreparedParticipantStopPlanDTO> plans_;
  std::set<std::string> referenced_;
};

[[nodiscard]] V resolved_participant_stage_value(const ResolvedParticipantStopStageDTO& v) {
  return arr({enc(v.stop_id),
              enc(v.stop_generation),
              enc(v.operation_id),
              enc(v.operation_revision),
              enc(v.participant_id),
              enc(v.controller_goal_id),
              enc(v.controller_goal_generation),
              enc(v.controllable_component_id),
              enc(v.command_target_id),
              enc(v.capability_id),
              enc(v.control_domain_id),
              enc(v.control_domain_revision),
              enc(v.control_binding_id),
              enc(v.control_binding_revision),
              enc(v.active_control_configuration_id),
              enc(v.active_control_configuration_generation),
              enc(v.controller_topology_id),
              enc(v.topology_revision),
              enc(v.control_route_id),
              opt(v.capability_provider_binding_id, id_value),
              opt(v.capability_provider_binding_revision, id_value),
              enc(v.provider_runtime_epoch),
              vec(v.resources, id_value),
              enc(v.containment_granularity),
              enc(v.stop_evidence_policy_id),
              enc(v.stop_evidence_policy_revision),
              enc(v.participant_stop_plan.participant_stop_invocation_plan_digest),
              V(static_cast<double>(v.stage_index)),
              enc(v.resolved_stop_scope_policy_digest)});
}

[[nodiscard]] ResolvedParticipantStopStageDTO resolved_participant_stage_from(
    const V& v, TerminalPlanIndex& plans) {
  const auto& f = exact(v, 29U, "resolved participant Stop stage");
  return ResolvedParticipantStopStageDTO{
      did<StopId>(f[0], "Stop ID"),
      did<StopGeneration>(f[1], "Stop generation"),
      did<OperationId>(f[2], "operation ID"),
      did<OperationRevision>(f[3], "operation revision"),
      did<OperationParticipantId>(f[4], "participant ID"),
      did<ControllerGoalId>(f[5], "controller goal ID"),
      did<ControllerGoalGeneration>(f[6], "controller goal generation"),
      did<ControllableComponentId>(f[7], "component ID"),
      did<CommandTargetId>(f[8], "command target ID"),
      did<CapabilityId>(f[9], "capability ID"),
      did<ControlDomainId>(f[10], "control domain ID"),
      did<ControlDomainRevision>(f[11], "control domain revision"),
      did<ControlBindingId>(f[12], "control binding ID"),
      did<ControlBindingRevision>(f[13], "control binding revision"),
      did<ActiveControlConfigurationId>(f[14], "active configuration ID"),
      did<ActiveControlConfigurationGeneration>(f[15], "active configuration generation"),
      did<ControllerTopologyId>(f[16], "controller topology ID"),
      did<TopologyRevision>(f[17], "topology revision"),
      did<ControlRouteId>(f[18], "control route ID"),
      dopt<CapabilityProviderBindingId>(f[19],
                                        [](const V& item) {
                                          return did<CapabilityProviderBindingId>(
                                              item, "provider binding ID");
                                        }),
      dopt<CapabilityProviderBindingRevision>(f[20],
                                              [](const V& item) {
                                                return did<CapabilityProviderBindingRevision>(
                                                    item, "provider binding revision");
                                              }),
      did<RuntimeEpoch>(f[21], "provider runtime epoch"),
      dvec<ResourceId>(
          f[22], [](const V& item) { return did<ResourceId>(item, "resource ID"); },
          "stage resources"),
      den<ContainmentGranularity>(f[23], 2U, "containment granularity"),
      did<StopEvidencePolicyId>(f[24], "evidence policy ID"),
      did<StopEvidencePolicyRevision>(f[25], "evidence policy revision"),
      plans.resolve(f[26]),
      static_cast<std::size_t>(u64(f[27], "stage index")),
      did<ContentDigest>(f[28], "resolved scope policy digest")};
}

[[nodiscard]] V evidence_item_value(const StopEvidenceItemDTO& v) {
  return arr(
      {enc(v.evidence_item_id), enc(v.required_class), enc(v.state),
       resolved_evidence_source_value(v.resolved_source), opt(v.source_epoch, id_value),
       opt(v.source_sequence,
           [](const std::uint64_t item) { return V(static_cast<double>(item)); }),
       opt(v.source_age_at_evaluation_ns,
           [](const std::int64_t item) { return content_identity::nonnegative_int64_value(item); }),
       opt(v.evidence, json_value), opt(v.error, error_value)});
}

[[nodiscard]] StopEvidenceItemDTO evidence_item_from(const V& v) {
  const auto& f = exact(v, 9U, "Stop evidence item");
  return StopEvidenceItemDTO{
      did<StopEvidenceItemId>(f[0], "evidence item ID"),
      den<StopEvidenceRequirementKind>(f[1], 5U, "required evidence class"),
      den<StopEvidenceItemStateDTO>(f[2], 4U, "evidence item state"),
      resolved_evidence_source_from(f[3]),
      dopt<RuntimeEpoch>(f[4],
                         [](const V& item) { return did<RuntimeEpoch>(item, "source epoch"); }),
      dopt<std::uint64_t>(f[5], [](const V& item) { return u64(item, "source sequence"); }),
      dopt<std::int64_t>(f[6], [](const V& item) { return i64(item, "source age"); }),
      dopt<JsonValueDTO>(f[7], json_from),
      dopt<ErrorDTO>(f[8], error_from)};
}

[[nodiscard]] V stage_result_value(const ParticipantStopStageResultDTO& v) {
  return arr(
      {resolved_participant_stage_value(v.request), enc(v.attempt_disposition), enc(v.outcome),
       opt(v.actual_provider_contract_mode, [](const auto item) { return enc(item); }),
       opt(v.recoverable_substitution_reason, [](const std::string& item) { return V(item); }),
       opt(v.controller_acknowledgement_sequence,
           [](const std::uint64_t item) { return V(static_cast<double>(item)); }),
       opt(v.source_epoch, id_value),
       opt(v.source_sequence,
           [](const std::uint64_t item) { return V(static_cast<double>(item)); }),
       content_identity::nonnegative_int64_value(v.receiver_elapsed_duration_ns),
       vec(v.evidence_items, evidence_item_value), opt(v.error, error_value)});
}

[[nodiscard]] ParticipantStopStageResultDTO stage_result_from(const V& v,
                                                            TerminalPlanIndex& plans) {
  const auto& f = exact(v, 11U, "participant Stop stage result");
  return ParticipantStopStageResultDTO{
      resolved_participant_stage_from(f[0], plans),
      den<StopStageAttemptDispositionDTO>(f[1], 2U, "attempt disposition"),
      den<ParticipantStopStageOutcomeDTO>(f[2], 3U, "participant Stop outcome"),
      dopt<MotionControlledStopMode>(f[3],
                                     [](const V& item) {
                                       return den<MotionControlledStopMode>(item, 3U,
                                                                            "actual provider mode");
                                     }),
      dopt<std::string>(f[4], [](const V& item) { return str(item, "substitution reason"); }),
      dopt<std::uint64_t>(f[5],
                          [](const V& item) { return u64(item, "acknowledgement sequence"); }),
      dopt<RuntimeEpoch>(f[6],
                         [](const V& item) { return did<RuntimeEpoch>(item, "source epoch"); }),
      dopt<std::uint64_t>(f[7], [](const V& item) { return u64(item, "source sequence"); }),
      i64(f[8], "receiver elapsed duration"),
      dvec<StopEvidenceItemDTO>(f[9], evidence_item_from, "stage evidence items"),
      dopt<ErrorDTO>(f[10], error_from)};
}

[[nodiscard]] V selector_value(const StopSelectorDTO& v) {
  return std::visit(
      [](const auto& targets) -> V {
        using T = typename std::decay_t<decltype(targets)>::value_type;
        if constexpr (std::is_same_v<T, StopOperationTargetDTO>) {
          return arr({V(0.0), vec(targets, [](const auto& row) {
                        return arr({enc(row.operation_id), enc(row.expected_operation_revision),
                                    enc(row.expected_stop_generation)});
                      })});
        } else if constexpr (std::is_same_v<T, StopComponentTargetDTO>) {
          return arr({V(1.0), vec(targets, [](const auto& row) {
                        return arr({enc(row.controllable_component_id),
                                    enc(row.expected_graph_revision),
                                    enc(row.expected_component_lifecycle_generation)});
                      })});
        } else if constexpr (std::is_same_v<T, StopResourceTargetDTO>) {
          return arr({V(2.0), vec(targets, [](const auto& row) {
                        return arr(
                            {enc(row.resource_id), enc(row.expected_graph_revision),
                             V(static_cast<double>(row.expected_operation_resource_generation)),
                             enc(row.expected_stop_generation)});
                      })});
        } else {
          return arr({V(3.0), vec(targets,
                                  [](const auto& row) { return arr({enc(row.program_run_id)}); })});
        }
      },
      v);
}

[[nodiscard]] StopSelectorDTO selector_from(const V& v) {
  const auto& f = exact(v, 2U, "Stop selector");
  switch (u64(f[0], "Stop selector kind")) {
    case 0U:
      return dvec<StopOperationTargetDTO>(
          f[1],
          [](const V& item) {
            const auto& row = exact(item, 3U, "operation Stop target");
            return StopOperationTargetDTO{did<OperationId>(row[0], "operation ID"),
                                          did<OperationRevision>(row[1], "operation revision"),
                                          did<StopGeneration>(row[2], "Stop generation")};
          },
          "operation Stop targets");
    case 1U:
      return dvec<StopComponentTargetDTO>(
          f[1],
          [](const V& item) {
            const auto& row = exact(item, 3U, "component Stop target");
            return StopComponentTargetDTO{
                did<ControllableComponentId>(row[0], "component ID"),
                did<GraphRevision>(row[1], "graph revision"),
                did<ComponentLifecycleGeneration>(row[2], "component lifecycle generation")};
          },
          "component Stop targets");
    case 2U:
      return dvec<StopResourceTargetDTO>(
          f[1],
          [](const V& item) {
            const auto& row = exact(item, 4U, "resource Stop target");
            return StopResourceTargetDTO{did<ResourceId>(row[0], "resource ID"),
                                         did<GraphRevision>(row[1], "graph revision"),
                                         u64(row[2], "resource generation"),
                                         did<StopGeneration>(row[3], "Stop generation")};
          },
          "resource Stop targets");
    case 3U:
      return dvec<StopProgramRunTargetDTO>(
          f[1],
          [](const V& item) {
            const auto& row = exact(item, 1U, "Program-run Stop target");
            return StopProgramRunTargetDTO{did<ProgramRunId>(row[0], "Program run ID")};
          },
          "Program-run Stop targets");
    default:
      bad("Stop selector has an unknown kind");
  }
}

[[nodiscard]] V requested_selector_value(const StopRequestedSelectorSnapshotDTO& v) {
  return arr({enc(v.kind), enc(v.requested_selector_digest), opt(v.selector, selector_value),
              opt(v.malformed_ingress_reason, [](const std::string& item) { return V(item); })});
}

[[nodiscard]] StopRequestedSelectorSnapshotDTO requested_selector_from(const V& v) {
  const auto& f = exact(v, 4U, "requested selector snapshot");
  return StopRequestedSelectorSnapshotDTO{
      den<StopRequestedSelectorKindDTO>(f[0], 5U, "requested selector kind"),
      did<ContentDigest>(f[1], "requested selector digest"),
      dopt<StopSelectorDTO>(f[2], selector_from),
      dopt<std::string>(f[3], [](const V& item) { return str(item, "malformed ingress reason"); })};
}

[[nodiscard]] V scope_subject_value(const StopScopeSubjectKeyDTO& v) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, StopParticipantScopeKeyDTO>) {
          return arr({V(0.0), arr({enc(current.operation_id), enc(current.participant_id)})});
        } else {
          return arr({V(1.0), enc(current)});
        }
      },
      v);
}

[[nodiscard]] StopScopeSubjectKeyDTO scope_subject_from(const V& v) {
  const auto& f = exact(v, 2U, "Stop scope subject");
  if (u64(f[0], "Stop scope subject kind") == 0U) {
    const auto& row = exact(f[1], 2U, "participant scope subject");
    return StopParticipantScopeKeyDTO{did<OperationId>(row[0], "operation ID"),
                                      did<OperationParticipantId>(row[1], "participant ID")};
  }
  if (u64(f[0], "Stop scope subject kind") == 1U) {
    return did<ResourceId>(f[1], "resource ID");
  }
  bad("Stop scope subject has an unknown kind");
}

[[nodiscard]] V scope_key_value(const StopScopeEntryKeyDTO& v) {
  return arr(
      {enc(v.origin_stop_id), enc(v.origin_stop_generation), scope_subject_value(v.subject)});
}

[[nodiscard]] StopScopeEntryKeyDTO scope_key_from(const V& v) {
  const auto& f = exact(v, 3U, "Stop scope key");
  return StopScopeEntryKeyDTO{did<StopId>(f[0], "origin Stop ID"),
                              did<StopGeneration>(f[1], "origin Stop generation"),
                              scope_subject_from(f[2])};
}

[[nodiscard]] V participant_facts_value(const StopParticipantScopeFactsDTO& v) {
  return arr({enc(v.operation_revision_after_acceptance), enc(v.controllable_component_id),
              enc(v.command_target_id), enc(v.capability_id), enc(v.control_domain_id),
              enc(v.control_domain_revision), enc(v.control_binding_id),
              enc(v.control_binding_revision), enc(v.controller_goal_id),
              enc(v.controller_goal_generation), enc(v.active_control_configuration_id),
              enc(v.active_control_configuration_generation), enc(v.controller_topology_id),
              enc(v.topology_revision), enc(v.control_route_id), enc(v.provider_runtime_epoch),
              vec(v.resources, id_value), enc(v.containment_granularity),
              enc(v.terminal_settlement)});
}

[[nodiscard]] StopParticipantScopeFactsDTO participant_facts_from(const V& v) {
  const auto& f = exact(v, 19U, "participant scope facts");
  return StopParticipantScopeFactsDTO{
      did<OperationRevision>(f[0], "accepted operation revision"),
      did<ControllableComponentId>(f[1], "component ID"),
      did<CommandTargetId>(f[2], "command target ID"),
      did<CapabilityId>(f[3], "capability ID"),
      did<ControlDomainId>(f[4], "control domain ID"),
      did<ControlDomainRevision>(f[5], "control domain revision"),
      did<ControlBindingId>(f[6], "control binding ID"),
      did<ControlBindingRevision>(f[7], "control binding revision"),
      did<ControllerGoalId>(f[8], "controller goal ID"),
      did<ControllerGoalGeneration>(f[9], "controller goal generation"),
      did<ActiveControlConfigurationId>(f[10], "active configuration ID"),
      did<ActiveControlConfigurationGeneration>(f[11], "active configuration generation"),
      did<ControllerTopologyId>(f[12], "controller topology ID"),
      did<TopologyRevision>(f[13], "topology revision"),
      did<ControlRouteId>(f[14], "control route ID"),
      did<RuntimeEpoch>(f[15], "provider runtime epoch"),
      dvec<ResourceId>(
          f[16], [](const V& item) { return did<ResourceId>(item, "resource ID"); },
          "scope resources"),
      den<ContainmentGranularity>(f[17], 2U, "containment granularity"),
      den<StopParticipantScopeFactsDTO::TerminalSettlementState>(
          f[18], 3U, "participant terminal settlement")};
}

[[nodiscard]] V scope_facts_value(const StopScopeEntryFactsDTO& v) {
  return std::visit(
      [](const auto& current) -> V {
        using T = std::decay_t<decltype(current)>;
        if constexpr (std::is_same_v<T, StopParticipantScopeFactsDTO>) {
          return arr({V(0.0), participant_facts_value(current)});
        } else {
          return arr({V(1.0), enc(current)});
        }
      },
      v);
}

[[nodiscard]] StopScopeEntryFactsDTO scope_facts_from(const V& v) {
  const auto& f = exact(v, 2U, "Stop scope facts");
  const auto kind = u64(f[0], "Stop scope facts kind");
  if (kind == 0U) return participant_facts_from(f[1]);
  if (kind == 1U) return did<ResourceId>(f[1], "resource ID");
  bad("Stop scope facts have an unknown kind");
}

[[nodiscard]] V scope_entry_value(const StopScopeEntryDTO& v) {
  return arr({scope_key_value(v.key), scope_facts_value(v.facts), enc(v.disposition),
              enc(v.dispatch_state), opt(v.stop_evidence_policy_id, id_value),
              opt(v.stop_evidence_policy_revision, id_value),
              opt(v.participant_stop_invocation_plan_digest, id_value),
              content_identity::nonnegative_int64_value(v.accepted_at_ns)});
}

[[nodiscard]] StopScopeEntryDTO scope_entry_from(const V& v) {
  const auto& f = exact(v, 8U, "Stop scope entry");
  return StopScopeEntryDTO{
      scope_key_from(f[0]),
      scope_facts_from(f[1]),
      den<StopScopeEntryDispositionDTO>(f[2], 1U, "scope disposition"),
      den<StopScopeDispatchStateDTO>(f[3], 2U, "scope dispatch state"),
      dopt<StopEvidencePolicyId>(
          f[4], [](const V& item) { return did<StopEvidencePolicyId>(item, "policy ID"); }),
      dopt<StopEvidencePolicyRevision>(
          f[5],
          [](const V& item) { return did<StopEvidencePolicyRevision>(item, "policy revision"); }),
      dopt<ContentDigest>(
          f[6], [](const V& item) { return did<ContentDigest>(item, "participant plan digest"); }),
      i64(f[7], "accepted time")};
}

[[nodiscard]] V attempted_result_value(const StopAttemptedStageResultDTO& v) {
  return arr({scope_key_value(v.scope_key), V(static_cast<double>(v.result_sequence)),
              content_identity::nonnegative_int64_value(v.completed_at_ns), enc(v.authority),
              stage_result_value(v.result)});
}

[[nodiscard]] StopAttemptedStageResultDTO attempted_result_from(const V& v,
                                                              TerminalPlanIndex& plans) {
  const auto& f = exact(v, 5U, "attempted stage result");
  return StopAttemptedStageResultDTO{
      scope_key_from(f[0]), u64(f[1], "result sequence"), i64(f[2], "completed time"),
      den<StopStageResultAuthorityDTO>(f[3], 1U, "stage result authority"),
      stage_result_from(f[4], plans)};
}

[[nodiscard]] V certificate_ref_value(const StopCertificateRefDTO& v) {
  return arr({enc(v.stop_certificate_id), enc(v.stop_certificate_revision), enc(v.stop_id),
              enc(v.stop_generation)});
}

[[nodiscard]] StopCertificateRefDTO certificate_ref_from(const V& v) {
  const auto& f = exact(v, 4U, "Stop certificate reference");
  return StopCertificateRefDTO{did<StopCertificateId>(f[0], "certificate ID"),
                               did<StopCertificateRevision>(f[1], "certificate revision"),
                               did<StopId>(f[2], "Stop ID"),
                               did<StopGeneration>(f[3], "Stop generation")};
}

[[nodiscard]] V capture_history_value(const StopScopeCaptureHistoryDTO& v) {
  return arr({V(static_cast<double>(v.capture_sequence)), enc(v.resolution_disposition),
              enc(v.resolution_reason), enc(v.captured_scope_digest),
              vec(v.predecessor_stops,
                  [](const StopPredecessorAcceptanceRefDTO& row) {
                    return arr({enc(row.predecessor_stop_id), enc(row.predecessor_stop_generation),
                                enc(row.captured_scope_digest)});
                  }),
              vec(v.newly_fenced_operation_revisions,
                  [](const StopAffectedOperationRevisionDTO& row) {
                    return arr(
                        {enc(row.operation_id), enc(row.operation_revision_after_acceptance)});
                  }),
              vec(v.newly_captured_resources, id_value)});
}

[[nodiscard]] StopScopeCaptureHistoryDTO capture_history_from(const V& v) {
  const auto& f = exact(v, 7U, "scope capture history");
  return StopScopeCaptureHistoryDTO{
      u64(f[0], "capture sequence"),
      den<StopResolutionDispositionDTO>(f[1], 3U, "capture resolution disposition"),
      den<StopScopeResolutionReasonDTO>(f[2], 10U, "capture resolution reason"),
      did<ContentDigest>(f[3], "capture scope digest"),
      dvec<StopPredecessorAcceptanceRefDTO>(
          f[4],
          [](const V& item) {
            const auto& row = exact(item, 3U, "capture predecessor");
            return StopPredecessorAcceptanceRefDTO{
                did<StopId>(row[0], "predecessor Stop ID"),
                did<StopGeneration>(row[1], "predecessor Stop generation"),
                did<ContentDigest>(row[2], "predecessor scope digest")};
          },
          "capture predecessors"),
      dvec<StopAffectedOperationRevisionDTO>(
          f[5],
          [](const V& item) {
            const auto& row = exact(item, 2U, "newly fenced operation");
            return StopAffectedOperationRevisionDTO{
                did<OperationId>(row[0], "operation ID"),
                did<OperationRevision>(row[1], "operation revision")};
          },
          "newly fenced operations"),
      dvec<ResourceId>(
          f[6], [](const V& item) { return did<ResourceId>(item, "resource ID"); },
          "newly captured resources")};
}

[[nodiscard]] V projection_status_value(const StopDurableProjectionStatusDTO& v) {
  return arr({enc(v.state), opt(v.error, error_value),
              opt(v.retry_generation,
                  [](const std::uint64_t item) { return V(static_cast<double>(item)); })});
}

[[nodiscard]] StopDurableProjectionStatusDTO projection_status_from(const V& v) {
  const auto& f = exact(v, 3U, "durable projection status");
  return StopDurableProjectionStatusDTO{
      den<StopDurableProjectionStateDTO>(f[0], 3U, "durable projection state"),
      dopt<ErrorDTO>(f[1], error_from),
      dopt<std::uint64_t>(f[2], [](const V& item) { return u64(item, "retry generation"); })};
}

[[nodiscard]] V interrupt_correlation_value(const StopInterruptCorrelationDTO& v) {
  return arr({enc(v.interrupt_source_id), enc(v.interrupt_policy_id),
              enc(v.motion_interrupt_scope_id)});
}

[[nodiscard]] StopInterruptCorrelationDTO interrupt_correlation_from(const V& v) {
  const auto& f = exact(v, 3U, "interrupt correlation");
  return StopInterruptCorrelationDTO{did<InterruptSourceId>(f[0], "interrupt source ID"),
                                     did<InterruptPolicyId>(f[1], "interrupt policy ID"),
                                     did<MotionInterruptScopeId>(f[2], "interrupt scope ID")};
}

// Certificate v4: 23 fixed slots. The v3 slots without evidence_items (the
// decoder derives them from the attempts), then lane_hard_stop_requested,
// always present. Attempts use the lean wire (plan digest in slot 26).
[[nodiscard]] V certificate_value(const StopCertificateDTO& v) {
  return arr({enc(v.stop_certificate_id),
              enc(v.stop_certificate_revision),
              enc(v.stop_id),
              enc(v.stop_generation),
              requested_selector_value(v.requested_selector),
              enc(v.resolution_disposition),
              enc(v.resolution_reason),
              vec(v.stale_diagnostics, stale_diagnostic_value),
              vec(v.capture_history, capture_history_value),
              vec(v.predecessor_certificates, certificate_ref_value),
              vec(v.scope_entries, scope_entry_value),
              vec(v.attempted_stage_results, attempted_result_value),
              enc(v.result),
              vec(v.pre_dispatch_cancelled, scope_key_value),
              vec(v.dispatched_stopped, scope_key_value),
              vec(v.unsettled, scope_key_value),
              vec(v.failed, scope_key_value),
              opt(v.interrupt_correlation, interrupt_correlation_value),
              content_identity::nonnegative_int64_value(v.completed_at_ns),
              opt(v.terminal_error, error_value),
              vec(v.lane_hard_stop_requested, scope_key_value)});
}

/// Leaves evidence_items empty; terminal_root_from derives them.
[[nodiscard]] StopCertificateDTO certificate_from(const V& v, TerminalPlanIndex& plans) {
  const auto& f = exact(v, 21U, "Stop certificate");
  return StopCertificateDTO{
      did<StopCertificateId>(f[0], "certificate ID"),
      did<StopCertificateRevision>(f[1], "certificate revision"),
      did<StopId>(f[2], "Stop ID"),
      did<StopGeneration>(f[3], "Stop generation"),
      requested_selector_from(f[4]),
      den<StopResolutionDispositionDTO>(f[5], 3U, "certificate resolution disposition"),
      den<StopScopeResolutionReasonDTO>(f[6], 10U, "certificate resolution reason"),
      dvec<StopScopeStaleDiagnosticDTO>(f[7], stale_diagnostic_from,
                                        "certificate stale diagnostics"),
      dvec<StopScopeCaptureHistoryDTO>(f[8], capture_history_from, "capture history"),
      dvec<StopCertificateRefDTO>(f[9], certificate_ref_from, "predecessor certificates"),
      dvec<StopScopeEntryDTO>(f[10], scope_entry_from, "certificate scope entries"),
      dvec<StopAttemptedStageResultDTO>(
          f[11], [&](const V& item) { return attempted_result_from(item, plans); },
          "certificate attempted results"),
      {},
      den<StopCertificateResultDTO>(f[12], 3U, "certificate result"),
      dvec<StopScopeEntryKeyDTO>(f[13], scope_key_from, "pre-dispatch cancelled"),
      dvec<StopScopeEntryKeyDTO>(f[14], scope_key_from, "dispatched stopped"),
      dvec<StopScopeEntryKeyDTO>(f[15], scope_key_from, "unsettled scope"),
      dvec<StopScopeEntryKeyDTO>(f[16], scope_key_from, "failed scope"),
      dopt<StopInterruptCorrelationDTO>(f[17], interrupt_correlation_from),
      i64(f[18], "certificate completion time"),
      dopt<ErrorDTO>(f[19], error_from),
      dvec<StopScopeEntryKeyDTO>(f[20], scope_key_from, "lane hard stop requested")};
}

[[nodiscard]] V remaining_budget_value(const RemainingStopBudgetDTO& v) {
  return arr({enc(v.stop_id), enc(v.stop_generation),
              content_identity::nonnegative_int64_value(v.elapsed_since_stop_ns),
              opt(v.remaining_controlled_deadline_ns, [](const std::int64_t item) {
                return content_identity::nonnegative_int64_value(item);
              })});
}

[[nodiscard]] RemainingStopBudgetDTO remaining_budget_from(const V& v) {
  const auto& f = exact(v, 4U, "per-robot Stop timing");
  return RemainingStopBudgetDTO{
      did<StopId>(f[0], "Stop ID"), did<StopGeneration>(f[1], "Stop generation"),
      i64(f[2], "elapsed since Stop"),
      dopt<std::int64_t>(f[3], [](const V& item) { return i64(item, "controlled deadline"); })};
}

[[nodiscard]] V current_stage_value(const StopCurrentStageDTO& v) {
  return arr({scope_key_value(v.scope_key), enc(v.stop_escalation_stage_id), enc(v.stage_kind),
              V(static_cast<double>(v.stage_index)), remaining_budget_value(v.remaining_budget)});
}

[[nodiscard]] StopCurrentStageDTO current_stage_from(const V& v) {
  const auto& f = exact(v, 5U, "current Stop stage");
  return StopCurrentStageDTO{scope_key_from(f[0]), did<StopEscalationStageId>(f[1], "stage ID"),
                             den<StopEscalationStageKind>(f[2], 3U, "stage kind"),
                             static_cast<std::size_t>(u64(f[3], "stage index")),
                             remaining_budget_from(f[4])};
}

// Terminal v5 status_rest: the StopStatusDTO fields that the certificate
// cannot rebuild (TerminalStopStatusFromCertificate rebuilds the rest).
[[nodiscard]] V status_rest_value(const StopTerminalStatusRestDTO& v) {
  return arr({accepted_value(v.accepted), vec(v.current_stages, current_stage_value),
              projection_status_value(v.acceptance_projection),
              projection_status_value(v.terminal_projection)});
}

[[nodiscard]] StopTerminalStatusRestDTO status_rest_from(const V& v) {
  const auto& f = exact(v, 4U, "Stop terminal status rest");
  return StopTerminalStatusRestDTO{
      accepted_from(f[0]),
      dvec<StopCurrentStageDTO>(f[1], current_stage_from, "current stages"),
      projection_status_from(f[2]),
      projection_status_from(f[3])};
}

// Checks the version slot before the arity, so every other version (whatever
// its shape) is refused as unsupported.
[[nodiscard]] const A& versioned(const V& value, const std::string_view version,
                                 const std::size_t size, const std::string_view context) {
  if (!value.is_array() || value.as_array().empty()) {
    bad(std::string(context) + " has the wrong structural arity");
  }
  if (str(value.as_array().front(), std::string(context) + " API version") != version) {
    bad(std::string(context) + " API version is unsupported");
  }
  return exact(value, size, context);
}

[[nodiscard]] V acceptance_root(const StopJournalAcceptanceProjectionDTO& value) {
  return arr({V(std::string(kStopJournalAcceptanceProjectionApiVersion)),
              accepted_value(value.accepted)});
}

[[nodiscard]] StopJournalAcceptanceProjectionDTO acceptance_root_from(const V& value) {
  const auto& f = versioned(value, kStopJournalAcceptanceProjectionApiVersion, 2U,
                            "Stop journal acceptance projection");
  return StopJournalAcceptanceProjectionDTO{accepted_from(f[1])};
}

using TerminalPlans = std::map<std::string, const PreparedParticipantStopPlanDTO*>;

// The distinct participant plans of the certificate attempts, keyed (and so
// sorted) by digest. The status attempts equal these (field-join validator).
// nullopt when two attempts name one digest but carry unequal plans.
[[nodiscard]] std::optional<TerminalPlans> terminal_plans(const StopCertificateDTO& certificate) {
  TerminalPlans plans;
  for (const auto& attempt : certificate.attempted_stage_results) {
    const auto& plan = attempt.result.request.participant_stop_plan;
    const auto [found, inserted] =
        plans.emplace(plan.participant_stop_invocation_plan_digest.value(), &plan);
    if (!inserted && *found->second != plan) return std::nullopt;
  }
  return plans;
}

[[nodiscard]] V terminal_root(const StopJournalTerminalProjectionDTO& value,
                              const TerminalPlans& plans) {
  A plan_values;
  plan_values.reserve(plans.size());
  for (const auto& row : plans) plan_values.push_back(prepared_plan_value(*row.second));
  return arr({V(std::string(kStopJournalTerminalProjectionApiVersion)),
              status_rest_value(TerminalStopStatusRest(value.stop_status)),
              certificate_value(value.certificate), V(std::move(plan_values))});
}

[[nodiscard]] StopJournalTerminalProjectionDTO terminal_root_from(const V& value) {
  const auto& f = versioned(value, kStopJournalTerminalProjectionApiVersion, 4U,
                            "Stop journal terminal projection");
  TerminalPlanIndex plans(f[3]);
  auto rest = status_rest_from(f[1]);
  auto certificate = certificate_from(f[2], plans);
  plans.require_all_referenced();
  auto evidence = DeriveStopCertificateEvidence(certificate.attempted_stage_results);
  if (!evidence.ok()) bad(evidence.error().code);
  certificate.evidence_items = std::move(evidence.value());
  auto status = TerminalStopStatusFromCertificate(rest, certificate);
  return StopJournalTerminalProjectionDTO{std::move(status), std::move(certificate)};
}

template <typename T, typename Root>
[[nodiscard]] Result<std::string> encode_projection(const T& value, Root root) {
  try {
    auto bytes = content_identity::canonical_json(root(value));
    if (bytes.size() > kMaxProjectionBytes) return reject<std::string>("projection exceeds 16 MiB");
    return Result<std::string>::Ok(std::move(bytes));
  } catch (const std::exception& error) {
    return reject<std::string>(error.what());
  } catch (...) {
    return reject<std::string>("projection encoding failed");
  }
}

template <typename T, typename Root>
[[nodiscard]] Result<T> decode_projection(const std::string_view bytes, Root root) {
  try {
    if (bytes.empty() || bytes.size() > kMaxProjectionBytes) {
      return reject<T>("projection byte size is invalid");
    }
    auto parsed = content_identity::parse_canonical_json(
        bytes,
        content_identity::CanonicalJsonParseLimits{kMaxProjectionBytes, 32U, 1U * 1024U * 1024U});
    auto decoded = root(parsed);
    if (content_identity::canonical_json(parsed) != bytes) {
      return reject<T>("projection bytes are not canonical");
    }
    return Result<T>::Ok(std::move(decoded));
  } catch (const std::exception& error) {
    return reject<T>(error.what());
  } catch (...) {
    return reject<T>("projection decoding failed");
  }
}

// Validated canonical bytes of each projection. The decoders compare against
// these for their canonical re-encode check; the public encoders add the
// read-back check below.
[[nodiscard]] Result<std::string> acceptance_bytes(
    const StopJournalAcceptanceProjectionDTO& projection) {
  if (const auto error = stop_accepted_field_error(projection.accepted); error.has_value()) {
    return reject<std::string>(*error);
  }
  return encode_projection(projection, acceptance_root);
}

[[nodiscard]] Result<std::string> terminal_bytes(
    const StopJournalTerminalProjectionDTO& projection) {
  if (const auto error =
          stop_terminal_projection_field_error(projection.stop_status, projection.certificate);
      error.has_value()) {
    return reject<std::string>(*error);
  }
  const auto plans = terminal_plans(projection.certificate);
  if (!plans.has_value()) {
    return Result<std::string>::Err(
        ErrorDTO{std::string(kStopJournalProjectionPlanConflictCode),
                 "Stop journal projection participant plans conflict",
                 {{"reason", "unequal participant Stop plans share one plan digest"}}});
  }
  return encode_projection(projection,
                           [&](const auto& value) { return terminal_root(value, *plans); });
}

// Encode never yields bytes its own decoder refuses: the bytes must decode
// back to exactly `value`, so a journal writer cannot commit an unreadable
// record. A refusal is the typed "invalid_stop_journal_projection".
template <typename T, typename RootFrom>
[[nodiscard]] Result<std::string> readable(Result<std::string> bytes, const T& value,
                                           RootFrom root_from) {
  if (!bytes.ok()) return bytes;
  const auto decoded = decode_projection<T>(bytes.value(), root_from);
  if (!decoded.ok()) {
    const auto found = decoded.error().details.find("reason");
    return reject<std::string>(
        "projection would not decode: " +
        (found == decoded.error().details.end() ? decoded.error().message : found->second));
  }
  if (!(decoded.value() == value)) {
    return reject<std::string>("projection does not decode to its exact value");
  }
  return bytes;
}

}  // namespace

Result<std::string> EncodeStopJournalAcceptanceProjection(
    const StopJournalAcceptanceProjectionDTO& projection) {
  return readable(acceptance_bytes(projection), projection, acceptance_root_from);
}

Result<StopJournalAcceptanceProjectionDTO> DecodeStopJournalAcceptanceProjection(
    const std::string_view canonical_bytes) {
  auto decoded =
      decode_projection<StopJournalAcceptanceProjectionDTO>(canonical_bytes, acceptance_root_from);
  if (!decoded.ok()) return decoded;
  const auto encoded = acceptance_bytes(decoded.value());
  if (!encoded.ok()) return Result<StopJournalAcceptanceProjectionDTO>::Err(encoded.error());
  if (encoded.value() != canonical_bytes) {
    return reject<StopJournalAcceptanceProjectionDTO>("acceptance projection is not canonical");
  }
  return decoded;
}

Result<std::string> EncodeStopJournalTerminalProjection(
    const StopJournalTerminalProjectionDTO& projection) {
  return readable(terminal_bytes(projection), projection, terminal_root_from);
}

Result<StopJournalTerminalProjectionDTO> DecodeStopJournalTerminalProjection(
    const std::string_view canonical_bytes) {
  auto decoded =
      decode_projection<StopJournalTerminalProjectionDTO>(canonical_bytes, terminal_root_from);
  if (!decoded.ok()) return decoded;
  const auto encoded = terminal_bytes(decoded.value());
  if (!encoded.ok()) return Result<StopJournalTerminalProjectionDTO>::Err(encoded.error());
  if (encoded.value() != canonical_bytes) {
    return reject<StopJournalTerminalProjectionDTO>("terminal projection is not canonical");
  }
  return decoded;
}

}  // namespace manyforge::core

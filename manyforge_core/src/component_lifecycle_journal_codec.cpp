#include <algorithm>
#include <bit>
#include <charconv>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
#include <manyforge_core/component_lifecycle_admission.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <manyforge_core/sqlite_cclc_journal.hpp>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "component_lifecycle_journal_codec_internal.hpp"

namespace manyforge::core::detail {
namespace {

constexpr std::string_view kStatusMagic = "MF-CCLC-STATUS-V1";
constexpr std::string_view kApplyStatusMagic = "MF-CCLC-APPLY-DELEGATED-STATUS-V1";
constexpr std::string_view kApplyGenerationStatusMagic = "MF-CCLC-APPLY-DELEGATED-STATUS-V2";
constexpr std::string_view kApplyQualifiedGenerationStatusMagic = "MF-CCLC-APPLY-DELEGATED-STATUS-V3";
constexpr std::string_view kApplyGenerationIntentMagic = "MF-CCLC-APPLY-DELEGATED-INTENT-V1";
constexpr std::string_view kApplyQualifiedGenerationIntentMagic = "MF-CCLC-APPLY-DELEGATED-INTENT-V2";
constexpr std::string_view kApplyResultMagic = "MF-CCLC-APPLY-DELEGATED-RESULT-V1";
constexpr std::string_view kSelectedStatusMagic = "MF-CCLC-SELECTED-STATUS-V1";
constexpr std::string_view kSelectedClaimMagic = "MF-CCLC-SELECTED-BRING-UP-CLAIM-V1";
constexpr std::string_view kProgramStatusMagic = "MF-CCLC-STATUS-V2";
constexpr std::string_view kProgramNodeMagic = "MF-PROGRAM-NODE-ADMISSION-V1";
constexpr std::string_view kIntentMagic = "MF-CCLC-INTENT-V1";
constexpr std::string_view kIntentValueMagic = "MF-CCLC-INTENT-VALUE-V1";
constexpr std::string_view kCloseIntentMagic = "MF-CCLC-CLOSE-RECOVER-INTENT-V1";
constexpr std::string_view kCloseIntentValueMagic = "MF-CCLC-CLOSE-RECOVER-INTENT-VALUE-V1";
constexpr std::string_view kSelectedResultMagic = "MF-CCLC-SELECTED-RESULT-V1";
constexpr std::string_view kPrimitiveIntentMagic = "MF-CCLC-PRIMITIVE-INTENT-V1";
constexpr std::string_view kPrimitiveResultMagic = "MF-CCLC-PRIMITIVE-RESULT-V1";
constexpr std::string_view kEffectAttemptMagic = "MF-CCLC-EFFECT-ATTEMPT-V1";
constexpr std::string_view kRouteStatusMagic = "MF-CCLC-ROUTE-STATUS-V2";
constexpr std::string_view kRouteAdmissionStatusMagic = "MF-CCLC-ROUTE-STATUS-V3";
constexpr std::string_view kRouteIntentMagic = "MF-CCLC-ROUTE-INTENT-V1";
constexpr std::string_view kRouteAdmissionIntentMagic = "MF-CCLC-ROUTE-INTENT-V2";
constexpr std::string_view kRouteEffectAttemptMagic = "MF-CCLC-ROUTE-EFFECT-ATTEMPT-V1";
constexpr std::string_view kActiveControlConfigurationMagic = "MF-CCLC-ACTIVE-CONFIG-V1";
constexpr std::string_view kRouteFenceAdmissionSnapshotMagic =
    "MF-KERNEL-ROUTE-FENCE-ADMISSION-SNAPSHOT-V1";
constexpr std::string_view kRouteFenceIntentMagic = "MF-KERNEL-ROUTE-FENCE-INTENT-V1";
constexpr std::string_view kRouteFenceAcceptedMagic = "MF-KERNEL-ROUTE-FENCE-ACCEPTED-V1";
constexpr std::string_view kRouteFenceEvidenceMagic = "MF-KERNEL-ROUTE-FENCE-EVIDENCE-V1";
constexpr std::string_view kRouteFenceStatusMagic = "MF-KERNEL-ROUTE-FENCE-STATUS-V1";
constexpr std::string_view kRouteFenceReleaseRequestMagic =
    "MF-KERNEL-ROUTE-FENCE-RELEASE-REQUEST-V1";
constexpr std::string_view kRouteFenceReleasedMagic = "MF-KERNEL-ROUTE-FENCE-RELEASED-V1";
constexpr std::size_t kMaximumCodecStringBytes = 1024U * 1024U;
constexpr std::size_t kMaximumCodecMapEntries = 4096U;
constexpr std::size_t kMaximumCodecVectorEntries = 8192U;

class CodecFailure final : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

class Writer final {
 public:
  void U8(std::uint8_t value) {
    bytes_.push_back(static_cast<char>(value));
    Bound();
  }

  void Bool(bool value) { U8(value ? 1U : 0U); }

  void U32(std::uint32_t value) {
    for (std::size_t index = 0U; index < 4U; ++index) {
      U8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  void U64(std::uint64_t value) {
    for (std::size_t index = 0U; index < 8U; ++index) {
      U8(static_cast<std::uint8_t>((value >> (index * 8U)) & 0xffU));
    }
  }

  void I64(std::int64_t value) { U64(static_cast<std::uint64_t>(value)); }

  void String(std::string_view value) {
    if (value.size() > kMaximumCodecStringBytes ||
        value.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw CodecFailure("component lifecycle journal string exceeds its bound");
    }
    U32(static_cast<std::uint32_t>(value.size()));
    bytes_.append(value);
    Bound();
  }

  template <typename Enum>
  void EnumValue(Enum value) {
    static_assert(std::is_enum_v<Enum>);
    U8(static_cast<std::uint8_t>(value));
  }

  template <typename StrongId>
  void Identity(const StrongId& value) {
    String(value.value());
  }

  template <typename StrongCounter>
  void Counter(const StrongCounter& value) {
    U64(value.value());
  }

  template <typename Value, typename Emit>
  void Optional(const std::optional<Value>& value, Emit emit) {
    Bool(value.has_value());
    if (value.has_value()) {
      emit(*value);
    }
  }

  template <typename Value, typename Emit>
  void Vector(const std::vector<Value>& values, Emit emit) {
    if (values.size() > kMaximumCodecVectorEntries ||
        values.size() > std::numeric_limits<std::uint32_t>::max()) {
      throw CodecFailure("component lifecycle journal vector exceeds its bound");
    }
    U32(static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) {
      emit(value);
    }
  }

  [[nodiscard]] std::string Finish() && {
    Bound();
    return std::move(bytes_);
  }

 private:
  void Bound() const {
    if (bytes_.size() > kMaximumComponentLifecycleJournalRecordBytes) {
      throw CodecFailure("component lifecycle journal record exceeds its bound");
    }
  }

  std::string bytes_;
};

class Reader final {
 public:
  explicit Reader(std::string_view bytes) : bytes_(bytes) {
    if (bytes_.size() > kMaximumComponentLifecycleJournalRecordBytes) {
      throw CodecFailure("component lifecycle journal record exceeds its bound");
    }
  }

  [[nodiscard]] std::uint8_t U8() {
    Require(1U);
    return static_cast<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] bool Bool() {
    const auto value = U8();
    if (value > 1U) {
      throw CodecFailure("component lifecycle journal boolean is invalid");
    }
    return value == 1U;
  }

  [[nodiscard]] std::uint32_t U32() {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value |= static_cast<std::uint32_t>(U8()) << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] std::uint64_t U64() {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value |= static_cast<std::uint64_t>(U8()) << (index * 8U);
    }
    return value;
  }

  [[nodiscard]] std::int64_t I64() { return static_cast<std::int64_t>(U64()); }

  [[nodiscard]] std::string String() {
    const auto size = static_cast<std::size_t>(U32());
    if (size > kMaximumCodecStringBytes) {
      throw CodecFailure("component lifecycle journal string exceeds its bound");
    }
    Require(size);
    std::string value(bytes_.substr(offset_, size));
    offset_ += size;
    return value;
  }

  template <typename Enum>
  [[nodiscard]] Enum EnumValue() {
    static_assert(std::is_enum_v<Enum>);
    return static_cast<Enum>(U8());
  }

  template <typename StrongId>
  [[nodiscard]] StrongId Identity() {
    return StrongId(String());
  }

  template <typename StrongCounter>
  [[nodiscard]] StrongCounter Counter() {
    return StrongCounter(U64());
  }

  template <typename Value, typename Read>
  [[nodiscard]] std::optional<Value> Optional(Read read) {
    if (!Bool()) {
      return std::nullopt;
    }
    return read();
  }

  template <typename Value, typename Read>
  [[nodiscard]] std::vector<Value> Vector(Read read) {
    const auto count = static_cast<std::size_t>(U32());
    if (count > kMaximumCodecVectorEntries) {
      throw CodecFailure("component lifecycle journal vector exceeds its bound");
    }
    std::vector<Value> result;
    result.reserve(count);
    for (std::size_t index = 0U; index < count; ++index) {
      result.push_back(read());
    }
    return result;
  }

  void RequireEnd() const {
    if (offset_ != bytes_.size()) {
      throw CodecFailure("component lifecycle journal record has trailing bytes");
    }
  }

 private:
  void Require(std::size_t count) const {
    if (count > bytes_.size() - offset_) {
      throw CodecFailure("component lifecycle journal record is truncated");
    }
  }

  std::string_view bytes_;
  std::size_t offset_{};
};

void PutError(Writer& writer, const ErrorDTO& error) {
  writer.String(error.code);
  writer.String(error.message);
  if (error.details.size() > kMaximumCodecMapEntries) {
    throw CodecFailure("component lifecycle error detail map exceeds its bound");
  }
  writer.U32(static_cast<std::uint32_t>(error.details.size()));
  for (const auto& [key, value] : error.details) {
    writer.String(key);
    writer.String(value);
  }
}

[[nodiscard]] ErrorDTO GetError(Reader& reader) {
  ErrorDTO result{reader.String(), reader.String(), {}};
  const auto count = static_cast<std::size_t>(reader.U32());
  if (count > kMaximumCodecMapEntries) {
    throw CodecFailure("component lifecycle error detail map exceeds its bound");
  }
  for (std::size_t index = 0U; index < count; ++index) {
    // Read in wire order before insertion. Function-argument evaluation order
    // must not be allowed to swap the key and value reads.
    auto key = reader.String();
    auto value = reader.String();
    const auto [unused, inserted] = result.details.emplace(std::move(key), std::move(value));
    if (!inserted) {
      throw CodecFailure("component lifecycle error detail key is duplicated");
    }
  }
  return result;
}

void PutPublicRequestContext(Writer& writer, const PublicRequestContextDTO& value) {
  writer.Identity(value.request_id);
  writer.Identity(value.request_owner_epoch);
  writer.String(value.source_label);
  writer.Optional<std::string>(value.correlation_id,
                               [&](const auto& item) { writer.String(item); });
}

[[nodiscard]] PublicRequestContextDTO GetPublicRequestContext(Reader& reader) {
  return PublicRequestContextDTO{reader.Identity<RequestId>(), reader.Identity<RequestOwnerEpoch>(),
                                 reader.String(),
                                 reader.Optional<std::string>([&]() { return reader.String(); })};
}

void PutAttribution(Writer& writer, const RequestAttributionDTO& value) {
  writer.EnumValue(value.transport);
  writer.EnumValue(value.assurance);
  writer.Optional<std::string>(value.principal_id, [&](const auto& item) { writer.String(item); });
}

[[nodiscard]] RequestAttributionDTO GetAttribution(Reader& reader) {
  return RequestAttributionDTO{reader.EnumValue<RequestTransportDTO>(),
                               reader.EnumValue<RequestAssuranceDTO>(),
                               reader.Optional<std::string>([&]() { return reader.String(); })};
}

void PutExpectedActiveConfiguration(Writer& writer,
                                    const ExpectedActiveControlConfigurationDTO& value) {
  writer.EnumValue(value.presence);
  writer.Optional<ActiveControlConfigurationId>(value.active_control_configuration_id,
                                                [&](const auto& item) { writer.Identity(item); });
  writer.Optional<ActiveControlConfigurationGeneration>(
      value.active_control_configuration_generation,
      [&](const auto& item) { writer.Counter(item); });
}

[[nodiscard]] ExpectedActiveControlConfigurationDTO GetExpectedActiveConfiguration(Reader& reader) {
  return ExpectedActiveControlConfigurationDTO{
      reader.EnumValue<ExpectedActiveControlConfigurationPresenceDTO>(),
      reader.Optional<ActiveControlConfigurationId>(
          [&]() { return reader.Identity<ActiveControlConfigurationId>(); }),
      reader.Optional<ActiveControlConfigurationGeneration>(
          [&]() { return reader.Counter<ActiveControlConfigurationGeneration>(); })};
}

void PutExpectedBinding(Writer& writer, const ExpectedControlBindingRevisionDTO& value) {
  writer.Identity(value.control_binding_id);
  writer.Identity(value.control_binding_revision);
  writer.Identity(value.control_route_id);
}

[[nodiscard]] ExpectedControlBindingRevisionDTO GetExpectedBinding(Reader& reader) {
  return ExpectedControlBindingRevisionDTO{reader.Identity<ControlBindingId>(),
                                           reader.Identity<ControlBindingRevision>(),
                                           reader.Identity<ControlRouteId>()};
}

void PutProviderDomain(Writer& writer, const ProviderControlDomainDTO& value) {
  writer.Identity(value.controllable_component_id);
  writer.Identity(value.provider_runtime_unit_id);
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.provider_manifest_id);
  writer.Identity(value.provider_manifest_revision);
  writer.Identity(value.lifecycle_plan_catalog_digest);
  writer.Identity(value.signal_configuration_digest);
  writer.Vector(value.signal_names, [&](const auto& name) { writer.String(name); });
  writer.I64(value.lifecycle_command_timeout_ns);
}

[[nodiscard]] ProviderControlDomainDTO GetProviderDomain(Reader& reader) {
  return {reader.Identity<ControllableComponentId>(),
          reader.Identity<RuntimeUnitId>(),
          reader.Identity<ProviderInstanceId>(),
          reader.Identity<ProviderManifestId>(),
          reader.Identity<ProviderManifestRevision>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ContentDigest>(),
          reader.Vector<std::string>([&]() { return reader.String(); }),
          reader.I64()};
}

void PutExpectedDomain(Writer& writer, const ExpectedControlDomainRevisionDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  if (const auto* provider = value.provider()) {
    // The empty discriminator was never a valid controller topology identity.
    // Controller records therefore retain every original byte unchanged.
    writer.String({});
    PutProviderDomain(writer, provider->provider);
    PutExpectedActiveConfiguration(writer, provider->expected_active_control_configuration);
    return;
  }
  writer.Identity(value.controller().controller_topology_id);
  writer.Identity(value.controller().topology_revision);
  PutExpectedActiveConfiguration(writer, value.expected_configuration());
  writer.Vector(value.control_bindings(),
                [&](const auto& item) { PutExpectedBinding(writer, item); });
}

[[nodiscard]] ExpectedControlDomainRevisionDTO GetExpectedDomain(Reader& reader) {
  auto id = reader.Identity<ControlDomainId>();
  auto revision = reader.Identity<ControlDomainRevision>();
  auto topology = reader.String();
  if (topology.empty()) {
    auto provider = GetProviderDomain(reader);
    auto configuration = GetExpectedActiveConfiguration(reader);
    return {std::move(id), std::move(revision),
            ExpectedProviderDomainScopeDTO{std::move(provider), std::move(configuration)}};
  }
  return ExpectedControlDomainRevisionDTO{std::move(id),
                                          std::move(revision),
                                          ControllerTopologyId(std::move(topology)),
                                          reader.Identity<TopologyRevision>(),
                                          GetExpectedActiveConfiguration(reader),
                                          reader.Vector<ExpectedControlBindingRevisionDTO>(
                                              [&]() { return GetExpectedBinding(reader); })};
}

void PutIntent(Writer& writer, const ComponentLifecycleIntentDTO& value, bool include_attribution) {
  PutPublicRequestContext(writer, value.request_context);
  writer.Identity(value.controllable_component_id);
  writer.EnumValue(value.intent);
  writer.Identity(value.expected_graph_revision);
  writer.Counter(value.expected_component_lifecycle_generation);
  writer.Vector(value.expected_control_domains,
                [&](const auto& item) { PutExpectedDomain(writer, item); });
  writer.Optional<std::string>(value.reason, [&](const auto& item) { writer.String(item); });
  if (include_attribution) {
    PutAttribution(writer, value.attribution);
  }
  writer.Counter(value.expected_admission_generation);
}

void PutApplyGenerationSelection(Writer& writer, const ApplyComponentGenerationSelectionDTO& value) {
  writer.Optional<ComponentLifecycleGeneration>(value.predecessor_generation,
      [&](const auto& generation) { writer.Counter(generation); });
  writer.Counter(value.candidate_generation);
  writer.U64(value.prior_reservation_generation);
  if (value.owner_qualified_predecessor) {
    writer.Counter(value.owner_qualified_predecessor->generation);
    writer.Identity(value.owner_qualified_predecessor->snapshot_digest);
  }
}

[[nodiscard]] ApplyComponentGenerationSelectionDTO GetApplyGenerationSelection(
    Reader& reader, bool qualified = false) {
  ApplyComponentGenerationSelectionDTO value{
      reader.Optional<ComponentLifecycleGeneration>(
          [&] { return reader.Counter<ComponentLifecycleGeneration>(); }),
      reader.Counter<ComponentLifecycleGeneration>(), reader.U64()};
  if (qualified) {
    value.owner_qualified_predecessor = ApplyComponentQualifiedPredecessorDTO{
        reader.Counter<ComponentLifecycleGeneration>(), reader.Identity<ContentDigest>()};
  }
  return value;
}

[[nodiscard]] bool HasQualifiedApplyPredecessor(const ComponentLifecycleIntentDTO& intent) {
  return intent.apply_generation_selection && intent.apply_generation_selection->owner_qualified_predecessor;
}

[[nodiscard]] ComponentLifecycleIntentDTO GetIntent(Reader& reader) {
  return ComponentLifecycleIntentDTO{
      GetPublicRequestContext(reader),
      reader.Identity<ControllableComponentId>(),
      reader.EnumValue<ComponentLifecycleIntentKindDTO>(),
      reader.Identity<GraphRevision>(),
      reader.Counter<ComponentLifecycleGeneration>(),
      reader.Vector<ExpectedControlDomainRevisionDTO>([&]() { return GetExpectedDomain(reader); }),
      reader.Optional<std::string>([&]() { return reader.String(); }),
      GetAttribution(reader),
      reader.Counter<AdmissionGeneration>()};
}

void PutAccepted(Writer& writer, const ComponentLifecycleAcceptedDTO& value) {
  writer.Identity(value.request_id);
  writer.EnumValue(value.replay_status);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Counter(value.component_lifecycle_generation);
  writer.I64(value.accepted_at_ns);
  writer.I64(value.timeout_duration_ns);
}

[[nodiscard]] ComponentLifecycleAcceptedDTO GetAccepted(Reader& reader) {
  return ComponentLifecycleAcceptedDTO{reader.Identity<RequestId>(),
                                       reader.EnumValue<RequestReplayStatusDTO>(),
                                       reader.Identity<LifecycleTransactionId>(),
                                       reader.Counter<LifecycleTransactionGeneration>(),
                                       reader.Counter<ComponentLifecycleGeneration>(),
                                       reader.I64(),
                                       reader.I64()};
}

void PutRequestedState(Writer& writer, const ComponentLifecycleRequestedStateDTO& value) {
  writer.EnumValue(value.presence);
  writer.Optional<ComponentLifecycleStateDTO>(value.state,
                                              [&](const auto item) { writer.EnumValue(item); });
}

[[nodiscard]] ComponentLifecycleRequestedStateDTO GetRequestedState(Reader& reader) {
  return ComponentLifecycleRequestedStateDTO{
      reader.EnumValue<ComponentLifecycleRequestedStatePresenceDTO>(),
      reader.Optional<ComponentLifecycleStateDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleStateDTO>(); })};
}

void PutActiveConfiguration(Writer& writer, const ActiveControlConfigurationDTO& value) {
  writer.Identity(value.active_control_configuration_id);
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Vector(value.binding_routes, [&](const auto& route) {
    writer.Identity(route.control_binding_id);
    writer.Identity(route.control_binding_revision);
    writer.Identity(route.control_route_id);
  });
  writer.Identity(value.activation_set_id);
  writer.Identity(value.controller_topology_id);
  writer.Identity(value.topology_revision);
  writer.Identity(value.source_runtime_unit_id);
  writer.Identity(value.source_runtime_epoch);
  writer.U64(value.source_sequence);
  writer.Vector(value.kinematic_resource_source_projections, [&](const auto& projection) {
    writer.Identity(projection.resource_id);
    writer.Vector(projection.contributing_control_bindings, [&](const auto& binding) {
      writer.Identity(binding.control_binding_id);
      writer.Identity(binding.control_binding_revision);
    });
    writer.Identity(projection.provider_instance_id);
    writer.Identity(projection.provider_manifest_id);
    writer.Identity(projection.provider_manifest_revision);
    writer.Identity(projection.state_authority_id);
    writer.EnumValue(projection.state_authority_class);
    writer.EnumValue(projection.publication_source);
  });
  writer.Counter(value.active_control_configuration_generation);
  writer.EnumValue(value.state);
}

[[nodiscard]] ActiveControlConfigurationDTO GetActiveConfiguration(Reader& reader) {
  return ActiveControlConfigurationDTO{
      reader.Identity<ActiveControlConfigurationId>(),
      reader.Identity<ControlDomainId>(),
      reader.Identity<ControlDomainRevision>(),
      reader.Vector<ActiveControlConfigurationBindingRouteDTO>([&]() {
        return ActiveControlConfigurationBindingRouteDTO{reader.Identity<ControlBindingId>(),
                                                         reader.Identity<ControlBindingRevision>(),
                                                         reader.Identity<ControlRouteId>()};
      }),
      reader.Identity<ActivationSetId>(),
      reader.Identity<ControllerTopologyId>(),
      reader.Identity<TopologyRevision>(),
      reader.Identity<RuntimeUnitId>(),
      reader.Identity<RuntimeEpoch>(),
      reader.U64(),
      reader.Vector<KinematicResourceSourceProjectionDTO>([&]() {
        return KinematicResourceSourceProjectionDTO{
            reader.Identity<ResourceId>(),
            reader.Vector<KinematicControlBindingSourceDTO>([&]() {
              return KinematicControlBindingSourceDTO{reader.Identity<ControlBindingId>(),
                                                      reader.Identity<ControlBindingRevision>()};
            }),
            reader.Identity<ProviderInstanceId>(),
            reader.Identity<ProviderManifestId>(),
            reader.Identity<ProviderManifestRevision>(),
            reader.Identity<StateAuthorityId>(),
            reader.EnumValue<StateAuthorityClass>(),
            reader.EnumValue<ObservedStateSourceKind>()};
      }),
      reader.Counter<ActiveControlConfigurationGeneration>(),
      reader.EnumValue<ActiveControlConfigurationStateDTO>()};
}

void PutResolvedScope(Writer& writer, const ResolvedLifecycleControlDomainScopeDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  if (const auto* provider = value.provider()) {
    writer.String({});
    PutProviderDomain(writer, provider->provider);
    writer.Identity(provider->provider_runtime_epoch);
    writer.Optional<ProviderDomainActiveConfigurationDTO>(
        provider->active_control_configuration, [&](const auto& active) {
          writer.Identity(active.active_control_configuration_id);
          writer.Counter(active.active_control_configuration_generation);
          writer.Identity(active.source_runtime_unit_id);
          writer.Identity(active.source_runtime_epoch);
          writer.U64(active.source_sequence);
          writer.Identity(active.signal_configuration_digest);
        });
    writer.Vector(provider->output_resources, [&](const auto& id) { writer.Identity(id); });
    return;
  }
  writer.Identity(value.controller().controller_topology_id);
  writer.Identity(value.controller().topology_revision);
  writer.Optional<ActiveControlConfigurationDTO>(
      value.controller().active_control_configuration,
      [&](const auto& item) { PutActiveConfiguration(writer, item); });
  writer.Vector(value.control_bindings(),
                [&](const auto& item) { PutExpectedBinding(writer, item); });
  writer.Vector(value.physical_resources(), [&](const auto& item) { writer.Identity(item); });
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO GetResolvedScope(Reader& reader) {
  auto id = reader.Identity<ControlDomainId>();
  auto revision = reader.Identity<ControlDomainRevision>();
  auto topology = reader.String();
  if (topology.empty()) {
    auto provider = GetProviderDomain(reader);
    auto epoch = reader.Identity<RuntimeEpoch>();
    auto active = reader.Optional<ProviderDomainActiveConfigurationDTO>([&]() {
      return ProviderDomainActiveConfigurationDTO{
          reader.Identity<ActiveControlConfigurationId>(),
          reader.Counter<ActiveControlConfigurationGeneration>(),
          reader.Identity<RuntimeUnitId>(),
          reader.Identity<RuntimeEpoch>(),
          reader.U64(),
          reader.Identity<ContentDigest>()};
    });
    auto resources = reader.Vector<ResourceId>([&]() { return reader.Identity<ResourceId>(); });
    return {std::move(id), std::move(revision),
            ResolvedProviderDomainScopeDTO{std::move(provider), std::move(epoch), std::move(active),
                                           std::move(resources)}};
  }
  return ResolvedLifecycleControlDomainScopeDTO{
      std::move(id),
      std::move(revision),
      ControllerTopologyId(std::move(topology)),
      reader.Identity<TopologyRevision>(),
      reader.Optional<ActiveControlConfigurationDTO>(
          [&]() { return GetActiveConfiguration(reader); }),
      reader.Vector<ExpectedControlBindingRevisionDTO>(
          [&]() { return GetExpectedBinding(reader); }),
      reader.Vector<ResourceId>([&]() { return reader.Identity<ResourceId>(); })};
}

void PutResolvedProviderOccurrence(Writer& writer,
                                   const ResolvedLifecycleProviderOccurrenceDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.provider_manifest_id);
  writer.Identity(value.provider_manifest_revision);
  writer.Identity(value.provider_runtime_unit_id);
  writer.Identity(value.provider_runtime_epoch);
  writer.Identity(value.lifecycle_plan_catalog_digest);
}

[[nodiscard]] ResolvedLifecycleProviderOccurrenceDTO GetResolvedProviderOccurrence(Reader& reader) {
  return {reader.Identity<ControlDomainId>(),
          reader.Identity<ControlDomainRevision>(),
          reader.Identity<ProviderInstanceId>(),
          reader.Identity<ProviderManifestId>(),
          reader.Identity<ProviderManifestRevision>(),
          reader.Identity<RuntimeUnitId>(),
          reader.Identity<RuntimeEpoch>(),
          reader.Identity<ContentDigest>()};
}

void PutSubstepResult(Writer& writer, const LifecycleSubstepResultDTO& value) {
  writer.Identity(value.substep_id);
  writer.Optional<LifecyclePrimitiveId>(value.within,
                                        [&](const auto& item) { writer.Identity(item); });
  writer.EnumValue(value.outcome);
  writer.I64(value.started_at_ns);
  writer.I64(value.finished_at_ns);
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
}

[[nodiscard]] LifecycleSubstepResultDTO GetSubstepResult(Reader& reader) {
  return LifecycleSubstepResultDTO{
      reader.Identity<LifecyclePrimitiveId>(),
      reader.Optional<LifecyclePrimitiveId>(
          [&]() { return reader.Identity<LifecyclePrimitiveId>(); }),
      reader.EnumValue<LifecycleTransitionOutcomeDTO>(),
      reader.I64(),
      reader.I64(),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); })};
}

void PutSubstepEvent(Writer& writer, const LifecycleSubstepEventDTO& value) {
  writer.Identity(value.attempt_id);
  writer.Identity(value.substep_id);
  writer.Optional<LifecyclePrimitiveId>(value.within,
                                        [&](const auto& item) { writer.Identity(item); });
  writer.EnumValue(value.phase);
  writer.I64(value.at_ns);
  writer.Optional<LifecycleTransitionOutcomeDTO>(value.outcome,
                                                 [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<std::string>(value.error_code, [&](const auto& item) { writer.String(item); });
}

[[nodiscard]] LifecycleSubstepEventDTO GetSubstepEvent(Reader& reader) {
  return LifecycleSubstepEventDTO{
      reader.Identity<LifecyclePrimitiveAttemptId>(),
      reader.Identity<LifecyclePrimitiveId>(),
      reader.Optional<LifecyclePrimitiveId>(
          [&]() { return reader.Identity<LifecyclePrimitiveId>(); }),
      reader.EnumValue<LifecycleSubstepEventDTO::Phase>(),
      reader.I64(),
      reader.Optional<LifecycleTransitionOutcomeDTO>(
          [&]() { return reader.EnumValue<LifecycleTransitionOutcomeDTO>(); }),
      reader.Optional<std::string>([&]() { return reader.String(); })};
}

void PutFact(Writer& writer, const LifecycleFactDTO& value) {
  writer.Identity(value.lifecycle_fact_id);
  writer.EnumValue(value.value.kind);
  writer.Optional<bool>(value.value.boolean_value, [&](const auto item) { writer.Bool(item); });
  writer.Optional<std::int64_t>(value.value.integer_value,
                                [&](const auto item) { writer.I64(item); });
  writer.Optional<std::string>(value.value.token_value,
                               [&](const auto& item) { writer.String(item); });
}

[[nodiscard]] LifecycleFactDTO GetFact(Reader& reader) {
  return LifecycleFactDTO{
      reader.Identity<LifecycleFactId>(),
      LifecycleFactValueDTO{reader.EnumValue<LifecycleFactValueKindDTO>(),
                            reader.Optional<bool>([&]() { return reader.Bool(); }),
                            reader.Optional<std::int64_t>([&]() { return reader.I64(); }),
                            reader.Optional<std::string>([&]() { return reader.String(); })}};
}

void PutTransitionObservation(Writer& writer, const LifecycleTransitionObservationDTO& value) {
  writer.EnumValue(value.observed_state);
  writer.Vector(value.facts, [&](const auto& item) { PutFact(writer, item); });
  writer.U64(value.observation_generation);
  writer.U64(value.source_sequence);
  writer.I64(value.at_ns);
}

[[nodiscard]] LifecycleTransitionObservationDTO GetTransitionObservation(Reader& reader) {
  return LifecycleTransitionObservationDTO{
      reader.EnumValue<ComponentLifecycleStateDTO>(),
      reader.Vector<LifecycleFactDTO>([&]() { return GetFact(reader); }),
      reader.U64(),
      reader.U64(),
      reader.I64()};
}

void PutPrimitiveResult(Writer& writer, const BrandLifecyclePrimitiveResultDTO& value) {
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.provider_manifest_id);
  writer.Identity(value.provider_manifest_revision);
  writer.Identity(value.provider_runtime_unit_id);
  writer.Identity(value.provider_runtime_epoch);
  writer.Identity(value.lifecycle_primitive_id);
  writer.Identity(value.lifecycle_primitive_attempt_id);
  writer.U64(value.expected_observation_generation);
  writer.EnumValue(value.outcome);
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
  writer.Vector(value.substeps, [&](const auto& item) { PutSubstepResult(writer, item); });
}

void PutPrimitiveIntent(Writer& writer, const BrandLifecyclePrimitiveIntentDTO& value) {
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.provider_manifest_id);
  writer.Identity(value.provider_manifest_revision);
  writer.Identity(value.provider_runtime_unit_id);
  writer.Identity(value.provider_runtime_epoch);
  writer.Identity(value.lifecycle_primitive_id);
  writer.Identity(value.lifecycle_primitive_attempt_id);
  writer.U64(value.expected_observation_generation);
  writer.I64(value.remaining_duration_ns);
  writer.EnumValue(value.requested_state);
}

[[nodiscard]] BrandLifecyclePrimitiveIntentDTO GetPrimitiveIntent(Reader& reader) {
  return BrandLifecyclePrimitiveIntentDTO{reader.Identity<LifecycleTransactionId>(),
                                          reader.Counter<LifecycleTransactionGeneration>(),
                                          reader.Identity<ControllableComponentId>(),
                                          reader.Identity<ControlDomainId>(),
                                          reader.Identity<ControlDomainRevision>(),
                                          reader.Identity<ProviderInstanceId>(),
                                          reader.Identity<ProviderManifestId>(),
                                          reader.Identity<ProviderManifestRevision>(),
                                          reader.Identity<RuntimeUnitId>(),
                                          reader.Identity<RuntimeEpoch>(),
                                          reader.Identity<LifecyclePrimitiveId>(),
                                          reader.Identity<LifecyclePrimitiveAttemptId>(),
                                          reader.U64(),
                                          reader.I64(),
                                          reader.EnumValue<ComponentLifecycleStateDTO>()};
}

[[nodiscard]] BrandLifecyclePrimitiveResultDTO GetPrimitiveResult(Reader& reader) {
  return BrandLifecyclePrimitiveResultDTO{
      reader.Identity<LifecycleTransactionId>(),
      reader.Counter<LifecycleTransactionGeneration>(),
      reader.Identity<ControllableComponentId>(),
      reader.Identity<ControlDomainId>(),
      reader.Identity<ControlDomainRevision>(),
      reader.Identity<ProviderInstanceId>(),
      reader.Identity<ProviderManifestId>(),
      reader.Identity<ProviderManifestRevision>(),
      reader.Identity<RuntimeUnitId>(),
      reader.Identity<RuntimeEpoch>(),
      reader.Identity<LifecyclePrimitiveId>(),
      reader.Identity<LifecyclePrimitiveAttemptId>(),
      reader.U64(),
      reader.EnumValue<LifecycleTransitionOutcomeDTO>(),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); }),
      reader.Vector<LifecycleSubstepResultDTO>([&]() { return GetSubstepResult(reader); })};
}

void PutProviderFenceOccurrence(Writer& writer,
                                const ProviderConfigurationFenceOccurrenceDTO& value) {
  writer.Identity(value.controllable_component_id);
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.signal_configuration_digest);
  writer.Vector(value.output_resources, [&](const auto& id) { writer.Identity(id); });
}

[[nodiscard]] ProviderConfigurationFenceOccurrenceDTO GetProviderFenceOccurrence(Reader& reader) {
  return {reader.Identity<ControllableComponentId>(), reader.Identity<ProviderInstanceId>(),
          reader.Identity<ContentDigest>(),
          reader.Vector<ResourceId>([&]() { return reader.Identity<ResourceId>(); })};
}

void PutFenceDomain(Writer& writer, const ConfigurationFenceDomainScopeDTO& domain) {
  const auto* provider = std::get_if<ProviderConfigurationFenceDomainDTO>(&domain.payload);
  if (provider) writer.String({});  // Reserved: an empty domain ID was never valid.
  writer.Identity(domain.control_domain_id);
  writer.Optional<ControlDomainRevision>(domain.predecessor_control_domain_revision,
                                         [&](const auto& item) { writer.Identity(item); });
  writer.Optional<ControlDomainRevision>(domain.candidate_control_domain_revision,
                                         [&](const auto& item) { writer.Identity(item); });
  if (provider) {
    writer.Optional<ProviderConfigurationFenceOccurrenceDTO>(
        provider->predecessor_provider,
        [&](const auto& item) { PutProviderFenceOccurrence(writer, item); });
    writer.Optional<ProviderConfigurationFenceOccurrenceDTO>(
        provider->candidate_provider,
        [&](const auto& item) { PutProviderFenceOccurrence(writer, item); });
  }
}

[[nodiscard]] ConfigurationFenceDomainScopeDTO GetFenceDomain(Reader& reader) {
  auto id = reader.String();
  const bool provider = id.empty();
  if (provider) id = reader.String();
  auto predecessor = reader.Optional<ControlDomainRevision>(
      [&]() { return reader.Identity<ControlDomainRevision>(); });
  auto candidate = reader.Optional<ControlDomainRevision>(
      [&]() { return reader.Identity<ControlDomainRevision>(); });
  ConfigurationFenceDomainScopeDTO result{ControlDomainId(std::move(id)), std::move(predecessor),
                                          std::move(candidate)};
  if (provider) {
    auto before = reader.Optional<ProviderConfigurationFenceOccurrenceDTO>(
        [&]() { return GetProviderFenceOccurrence(reader); });
    auto after = reader.Optional<ProviderConfigurationFenceOccurrenceDTO>(
        [&]() { return GetProviderFenceOccurrence(reader); });
    result.payload = ProviderConfigurationFenceDomainDTO{std::move(before), std::move(after)};
  }
  return result;
}

void PutFenceScope(Writer& writer, const ConfigurationFenceScopeDTO& value) {
  writer.Vector(value.controllable_component_ids, [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.domains, [&](const auto& domain) { PutFenceDomain(writer, domain); });
  writer.Vector(value.bindings, [&](const auto& binding) {
    writer.Identity(binding.control_binding_id);
    writer.Optional<ControlBindingRevision>(binding.predecessor_control_binding_revision,
                                            [&](const auto& item) { writer.Identity(item); });
    writer.Optional<ControlBindingRevision>(binding.candidate_control_binding_revision,
                                            [&](const auto& item) { writer.Identity(item); });
  });
  writer.Vector(value.resources, [&](const auto& item) { writer.Identity(item); });
}

[[nodiscard]] ConfigurationFenceScopeDTO GetFenceScope(Reader& reader) {
  return ConfigurationFenceScopeDTO{
      reader.Vector<ControllableComponentId>(
          [&]() { return reader.Identity<ControllableComponentId>(); }),
      reader.Vector<ConfigurationFenceDomainScopeDTO>([&]() { return GetFenceDomain(reader); }),
      reader.Vector<ConfigurationFenceBindingScopeDTO>([&]() {
        return ConfigurationFenceBindingScopeDTO{reader.Identity<ControlBindingId>(),
                                                 reader.Optional<ControlBindingRevision>([&]() {
                                                   return reader.Identity<ControlBindingRevision>();
                                                 }),
                                                 reader.Optional<ControlBindingRevision>([&]() {
                                                   return reader.Identity<ControlBindingRevision>();
                                                 })};
      }),
      reader.Vector<ResourceId>([&]() { return reader.Identity<ResourceId>(); })};
}

void PutDesiredControlConfiguration(Writer& writer, const DesiredControlConfigurationDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.controller_topology_id);
  writer.Identity(value.topology_revision);
  writer.Identity(value.activation_set_id);
  writer.Vector(value.binding_routes, [&](const auto& route) {
    writer.Identity(route.control_binding_id);
    writer.Identity(route.control_binding_revision);
    writer.Identity(route.control_route_id);
  });
}

[[nodiscard]] DesiredControlConfigurationDTO GetDesiredControlConfiguration(Reader& reader) {
  return {reader.Identity<ControlDomainId>(),
          reader.Identity<ControlDomainRevision>(),
          reader.Identity<ControllerTopologyId>(),
          reader.Identity<TopologyRevision>(),
          reader.Identity<ActivationSetId>(),
          reader.Vector<DesiredControlBindingRouteDTO>([&]() {
            return DesiredControlBindingRouteDTO{reader.Identity<ControlBindingId>(),
                                                 reader.Identity<ControlBindingRevision>(),
                                                 reader.Identity<ControlRouteId>()};
          })};
}

void PutRouteComponentExpectation(Writer& writer,
                                  const RouteSelectionComponentExpectationDTO& value) {
  writer.Identity(value.controllable_component_id);
  writer.Counter(value.expected_component_lifecycle_generation);
  writer.EnumValue(value.expected_component_lifecycle_state);
}

[[nodiscard]] RouteSelectionComponentExpectationDTO GetRouteComponentExpectation(Reader& reader) {
  return {reader.Identity<ControllableComponentId>(),
          reader.Counter<ComponentLifecycleGeneration>(),
          reader.EnumValue<ComponentLifecycleStateDTO>()};
}

void PutRouteIntent(Writer& writer, const RouteSelectionIntentDTO& value,
                    bool include_attribution) {
  PutPublicRequestContext(writer, value.request_context);
  writer.Identity(value.expected_retained_bundle_id);
  writer.Identity(value.expected_graph_revision);
  PutDesiredControlConfiguration(writer, value.desired_configuration);
  PutExpectedActiveConfiguration(writer, value.expected_active_control_configuration);
  writer.Vector(value.component_expectations,
                [&](const auto& item) { PutRouteComponentExpectation(writer, item); });
  if (include_attribution) {
    PutAttribution(writer, value.attribution);
  }
}

[[nodiscard]] RouteSelectionIntentDTO GetRouteIntent(Reader& reader) {
  return {GetPublicRequestContext(reader),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          GetDesiredControlConfiguration(reader),
          GetExpectedActiveConfiguration(reader),
          reader.Vector<RouteSelectionComponentExpectationDTO>(
              [&]() { return GetRouteComponentExpectation(reader); }),
          GetAttribution(reader)};
}

void PutControllerObservationQuery(Writer& writer,
                                   const ControllerLifecycleObservationQueryDTO& value) {
  if (value.physical_expectation == ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone) {
    // Empty is never a valid legacy domain identity. Required queries retain
    // their exact eleven-field bytes; only the new typed case has a prefix.
    writer.String("");
    writer.EnumValue(value.physical_expectation);
  }
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.controller_topology_id);
  writer.Identity(value.topology_revision);
  writer.Identity(value.product_id);
  writer.Identity(value.product_contract_id);
  writer.Identity(value.product_contract_revision);
  writer.Identity(value.control_artifact_digest);
  writer.Identity(value.source_conformance_digest);
  writer.Identity(value.source_runtime_unit_id);
  writer.Identity(value.source_runtime_epoch);
}

[[nodiscard]] ControllerLifecycleObservationQueryDTO GetControllerObservationQuery(Reader& reader) {
  auto domain = reader.Identity<ControlDomainId>();
  auto expectation = ControllerLifecyclePhysicalExpectation::kRequired;
  if (domain.empty()) {
    expectation = reader.EnumValue<ControllerLifecyclePhysicalExpectation>();
    if (expectation != ControllerLifecyclePhysicalExpectation::kNotApplicablePneumaticNone)
      throw CodecFailure("controller observation physical expectation is unknown");
    domain = reader.Identity<ControlDomainId>();
  }
  return {std::move(domain),
          reader.Identity<ControlDomainRevision>(),
          reader.Identity<ControllerTopologyId>(),
          reader.Identity<TopologyRevision>(),
          reader.Identity<ProductId>(),
          reader.Identity<ProductContractId>(),
          reader.Identity<ProductContractRevision>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<RuntimeUnitId>(),
          reader.Identity<RuntimeEpoch>(), expectation};
}

void PutControllerInterfaceInstance(Writer& writer, const ControllerInterfaceInstanceDTO& value) {
  writer.Identity(value.resource_id);
  writer.String(value.interface_name);
}

[[nodiscard]] ControllerInterfaceInstanceDTO GetControllerInterfaceInstance(Reader& reader) {
  return {reader.Identity<ResourceId>(), reader.String()};
}

void PutExpectedController(Writer& writer, const ControllerLifecycleExpectedControllerDTO& value) {
  writer.Identity(value.controller_role_id);
  writer.Identity(value.implementation_id);
  writer.Identity(value.implementation_revision);
  writer.String(value.implementation_type);
  writer.Identity(value.configuration_digest);
  const auto put_interfaces = [&](const auto& interfaces) {
    writer.Vector(interfaces,
                  [&](const auto& item) { PutControllerInterfaceInstance(writer, item); });
  };
  put_interfaces(value.claimed_command_interfaces);
  put_interfaces(value.claimed_state_interfaces);
  put_interfaces(value.exported_reference_interfaces);
  put_interfaces(value.exported_state_interfaces);
}

[[nodiscard]] ControllerLifecycleExpectedControllerDTO GetExpectedController(Reader& reader) {
  const auto get_interfaces = [&]() {
    return reader.Vector<ControllerInterfaceInstanceDTO>(
        [&]() { return GetControllerInterfaceInstance(reader); });
  };
  return {reader.Identity<ControllerRoleId>(),
          reader.Identity<ControllerImplementationId>(),
          reader.Identity<ControllerImplementationRevision>(),
          reader.String(),
          reader.Identity<ContentDigest>(),
          get_interfaces(),
          get_interfaces(),
          get_interfaces(),
          get_interfaces()};
}

void PutExpectedEndpoint(Writer& writer, const ControllerLifecycleExpectedEndpointDTO& value) {
  writer.Identity(value.control_binding_id);
  writer.Identity(value.control_binding_revision);
  writer.Identity(value.control_route_id);
  writer.Identity(value.endpoint_role_id);
  writer.String(value.logical_endpoint_symbol);
  writer.Bool(value.is_command_endpoint);
  writer.Bool(value.is_state_endpoint);
}

[[nodiscard]] ControllerLifecycleExpectedEndpointDTO GetExpectedEndpoint(Reader& reader) {
  return {reader.Identity<ControlBindingId>(),
          reader.Identity<ControlBindingRevision>(),
          reader.Identity<ControlRouteId>(),
          reader.Identity<EndpointRoleId>(),
          reader.String(),
          reader.Bool(),
          reader.Bool()};
}

void PutControllerChainEdgeRef(Writer& writer, const ControllerChainEdgeRefDTO& value) {
  writer.Identity(value.upstream_controller_role_id);
  writer.Identity(value.downstream_controller_role_id);
}

[[nodiscard]] ControllerChainEdgeRefDTO GetControllerChainEdgeRef(Reader& reader) {
  return {reader.Identity<ControllerRoleId>(), reader.Identity<ControllerRoleId>()};
}

void PutControllerObservation(Writer& writer, const ControllerLifecycleObservationDTO& value) {
  PutControllerObservationQuery(writer, value.query);
  writer.U64(value.source_sequence);
  writer.I64(value.observed_at_ns);
  writer.I64(value.received_at_ns);
  writer.I64(value.valid_until_ns);
  writer.Optional<ControllerLifecycleObservedConfigurationDTO>(
      value.active_configuration, [&](const auto& configuration) {
        writer.Identity(configuration.activation_set_id);
        writer.Vector(configuration.binding_routes, [&](const auto& route) {
          writer.Identity(route.control_binding_id);
          writer.Identity(route.control_binding_revision);
          writer.Identity(route.control_route_id);
        });
      });
  writer.Vector(value.controllers, [&](const auto& controller) {
    PutExpectedController(writer, controller.expected_controller);
    writer.EnumValue(controller.activity);
  });
  writer.Vector(value.active_chain_edges,
                [&](const auto& edge) { PutControllerChainEdgeRef(writer, edge); });
  writer.Vector(value.endpoints, [&](const auto& endpoint) {
    PutExpectedEndpoint(writer, endpoint.expected_endpoint);
    writer.Bool(endpoint.ready);
  });
}

[[nodiscard]] ControllerLifecycleObservationDTO GetControllerObservation(Reader& reader) {
  return {
      GetControllerObservationQuery(reader),
      reader.U64(),
      reader.I64(),
      reader.I64(),
      reader.I64(),
      reader.Optional<ControllerLifecycleObservedConfigurationDTO>([&]() {
        return ControllerLifecycleObservedConfigurationDTO{
            reader.Identity<ActivationSetId>(),
            reader.Vector<ActiveControlConfigurationBindingRouteDTO>([&]() {
              return ActiveControlConfigurationBindingRouteDTO{
                  reader.Identity<ControlBindingId>(), reader.Identity<ControlBindingRevision>(),
                  reader.Identity<ControlRouteId>()};
            })};
      }),
      reader.Vector<ControllerLifecycleObservedControllerDTO>([&]() {
        return ControllerLifecycleObservedControllerDTO{
            GetExpectedController(reader), reader.EnumValue<ControllerLifecycleActivityDTO>()};
      }),
      reader.Vector<ControllerChainEdgeRefDTO>([&]() { return GetControllerChainEdgeRef(reader); }),
      reader.Vector<ControllerLifecycleObservedEndpointDTO>([&]() {
        return ControllerLifecycleObservedEndpointDTO{GetExpectedEndpoint(reader), reader.Bool()};
      })};
}

void PutControllerReconciliation(Writer& writer,
                                 const ControllerLifecycleReconciliationDTO& value) {
  PutControllerObservationQuery(writer, value.query);
  writer.U64(value.source_sequence);
  writer.EnumValue(value.disposition);
  writer.Identity(value.desired_activation_set_id);
  writer.Vector(value.desired_binding_routes, [&](const auto& route) {
    writer.Identity(route.control_binding_id);
    writer.Identity(route.control_binding_revision);
    writer.Identity(route.control_route_id);
  });
  writer.Vector(value.reason_codes, [&](const auto& reason) { writer.String(reason); });
}

[[nodiscard]] ControllerLifecycleReconciliationDTO GetControllerReconciliation(Reader& reader) {
  return {GetControllerObservationQuery(reader),
          reader.U64(),
          reader.EnumValue<ControllerLifecycleReconciliationDispositionDTO>(),
          reader.Identity<ActivationSetId>(),
          reader.Vector<DesiredControlBindingRouteDTO>([&]() {
            return DesiredControlBindingRouteDTO{reader.Identity<ControlBindingId>(),
                                                 reader.Identity<ControlBindingRevision>(),
                                                 reader.Identity<ControlRouteId>()};
          }),
          reader.Vector<std::string>([&]() { return reader.String(); })};
}

void PutControllerTransitionStep(Writer& writer,
                                 const ControllerLifecycleTransitionStepDTO& value) {
  writer.EnumValue(value.kind);
  writer.Vector(value.controller_role_ids, [&](const auto& item) { writer.Identity(item); });
}

[[nodiscard]] ControllerLifecycleTransitionStepDTO GetControllerTransitionStep(Reader& reader) {
  return {reader.EnumValue<ControllerLifecycleTransitionKindDTO>(),
          reader.Vector<ControllerRoleId>([&]() { return reader.Identity<ControllerRoleId>(); })};
}

void PutControllerTransitionPlan(Writer& writer,
                                 const ControllerLifecycleTransitionPlanDTO& value) {
  PutControllerObservationQuery(writer, value.query);
  writer.U64(value.expected_source_sequence);
  writer.Vector(value.observed_active_controller_role_ids,
                [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.observed_active_chain_edges,
                [&](const auto& item) { PutControllerChainEdgeRef(writer, item); });
  writer.Identity(value.desired_activation_set_id);
  writer.Vector(value.desired_active_controller_role_ids,
                [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.desired_active_chain_edges,
                [&](const auto& item) { PutControllerChainEdgeRef(writer, item); });
  writer.Vector(value.steps, [&](const auto& item) { PutControllerTransitionStep(writer, item); });
}

[[nodiscard]] ControllerLifecycleTransitionPlanDTO GetControllerTransitionPlan(Reader& reader) {
  return {
      GetControllerObservationQuery(reader),
      reader.U64(),
      reader.Vector<ControllerRoleId>([&]() { return reader.Identity<ControllerRoleId>(); }),
      reader.Vector<ControllerChainEdgeRefDTO>([&]() { return GetControllerChainEdgeRef(reader); }),
      reader.Identity<ActivationSetId>(),
      reader.Vector<ControllerRoleId>([&]() { return reader.Identity<ControllerRoleId>(); }),
      reader.Vector<ControllerChainEdgeRefDTO>([&]() { return GetControllerChainEdgeRef(reader); }),
      reader.Vector<ControllerLifecycleTransitionStepDTO>(
          [&]() { return GetControllerTransitionStep(reader); })};
}

void PutControllerTransitionIntent(Writer& writer,
                                   const ControllerLifecycleTransitionIntentDTO& value) {
  PutControllerObservationQuery(writer, value.query);
  writer.U64(value.expected_source_sequence);
  writer.U32(value.step_index);
  PutControllerTransitionStep(writer, value.step);
  writer.I64(value.remaining_duration_ns);
}

[[nodiscard]] ControllerLifecycleTransitionIntentDTO GetControllerTransitionIntent(Reader& reader) {
  return {GetControllerObservationQuery(reader), reader.U64(), reader.U32(),
          GetControllerTransitionStep(reader), reader.I64()};
}

void PutControllerTransitionResult(Writer& writer,
                                   const ControllerLifecycleTransitionResultDTO& value) {
  PutControllerTransitionIntent(writer, value.intent);
  writer.EnumValue(value.dispatch_state);
  writer.Optional<ControllerLifecycleTransitionKnownResultDTO>(
      value.known_result, [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
}

[[nodiscard]] ControllerLifecycleTransitionResultDTO GetControllerTransitionResult(Reader& reader) {
  return {GetControllerTransitionIntent(reader),
          reader.EnumValue<ControllerLifecycleTransitionDispatchStateDTO>(),
          reader.Optional<ControllerLifecycleTransitionKnownResultDTO>(
              [&]() { return reader.EnumValue<ControllerLifecycleTransitionKnownResultDTO>(); }),
          reader.Optional<ErrorDTO>([&]() { return GetError(reader); })};
}

void PutRouteAccepted(Writer& writer, const RouteSelectionAcceptedDTO& value) {
  writer.Identity(value.request_id);
  writer.EnumValue(value.replay_status);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  writer.Identity(value.accepted_retained_bundle_id);
  writer.Identity(value.accepted_graph_revision);
  PutDesiredControlConfiguration(writer, value.desired_configuration);
  writer.Vector(value.component_expectations,
                [&](const auto& item) { PutRouteComponentExpectation(writer, item); });
  writer.Optional<ActiveControlConfigurationDTO>(
      value.predecessor_active_control_configuration,
      [&](const auto& item) { PutActiveConfiguration(writer, item); });
  PutFenceScope(writer, value.configuration_fence_scope);
  PutControllerObservationQuery(writer, value.controller_lifecycle_observation_query);
  writer.Identity(value.candidate_active_control_configuration_id);
  writer.Counter(value.candidate_active_control_configuration_generation);
  writer.I64(value.accepted_at_ns);
  writer.I64(value.timeout_duration_ns);
}

[[nodiscard]] RouteSelectionAcceptedDTO GetRouteAccepted(Reader& reader) {
  return {reader.Identity<RequestId>(),
          reader.EnumValue<RequestReplayStatusDTO>(),
          reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          GetDesiredControlConfiguration(reader),
          reader.Vector<RouteSelectionComponentExpectationDTO>(
              [&]() { return GetRouteComponentExpectation(reader); }),
          reader.Optional<ActiveControlConfigurationDTO>(
              [&]() { return GetActiveConfiguration(reader); }),
          GetFenceScope(reader),
          GetControllerObservationQuery(reader),
          reader.Identity<ActiveControlConfigurationId>(),
          reader.Counter<ActiveControlConfigurationGeneration>(),
          reader.I64(),
          reader.I64()};
}

void PutExecutionHostEpoch(Writer& writer, const ExecutionHostEpochDTO& value) {
  writer.Identity(value.runtime_unit_id);
  writer.Identity(value.runtime_epoch);
  writer.Identity(value.request_owner_epoch);
  writer.Identity(value.durable_stop_fence_ledger_digest);
  writer.Counter(value.publication_generation);
}

[[nodiscard]] ExecutionHostEpochDTO GetExecutionHostEpoch(Reader& reader) {
  return {reader.Identity<RuntimeUnitId>(), reader.Identity<RuntimeEpoch>(),
          reader.Identity<RequestOwnerEpoch>(), reader.Identity<ContentDigest>(),
          reader.Counter<ActiveBundlePublicationGeneration>()};
}

void PutProjectionRevision(Writer& writer, const ProjectionRevisionDTO& value) {
  writer.Identity(value.retained_bundle_id);
  writer.Identity(value.graph_revision);
  writer.Identity(value.scene_revision.scene_id);
  writer.U64(value.scene_revision.revision_id);
  writer.Identity(value.scene_revision.content_hash);
}

[[nodiscard]] ProjectionRevisionDTO GetProjectionRevision(Reader& reader) {
  return {reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          {reader.Identity<SceneId>(), reader.U64(), reader.Identity<ContentDigest>()}};
}

void PutRouteFenceAdmissionQuery(Writer& writer,
                                 const RouteConfigurationFenceAdmissionQueryDTO& value) {
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  PutFenceScope(writer, value.scope);
  writer.Identity(value.expected_retained_bundle_id);
  writer.Identity(value.expected_graph_revision);
  PutExpectedActiveConfiguration(writer, value.expected_active_control_configuration);
}

[[nodiscard]] RouteConfigurationFenceAdmissionQueryDTO GetRouteFenceAdmissionQuery(Reader& reader) {
  return {reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          GetFenceScope(reader),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          GetExpectedActiveConfiguration(reader)};
}

void PutRouteFenceAdmissionSnapshot(Writer& writer,
                                    const RouteConfigurationFenceAdmissionSnapshotDTO& value) {
  PutRouteFenceAdmissionQuery(writer, value.query);
  PutExecutionHostEpoch(writer, value.execution_host_epoch);
  PutProjectionRevision(writer, value.active_projection_revision);
  writer.Counter(value.active_publication_generation);
  writer.Identity(value.active_public_pointer_digest);
  writer.U64(value.operation_resource_generation);
  writer.Counter(value.configuration_fence_generation);
  PutExpectedActiveConfiguration(writer, value.current_active_control_configuration);
  writer.U64(value.affected_queued_operation_count);
  writer.U64(value.affected_active_operation_count);
  writer.Identity(value.snapshot_digest);
}

[[nodiscard]] RouteConfigurationFenceAdmissionSnapshotDTO GetRouteFenceAdmissionSnapshot(
    Reader& reader) {
  return {GetRouteFenceAdmissionQuery(reader),
          GetExecutionHostEpoch(reader),
          GetProjectionRevision(reader),
          reader.Counter<ActiveBundlePublicationGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.U64(),
          reader.Counter<ConfigurationFenceGeneration>(),
          GetExpectedActiveConfiguration(reader),
          reader.U64(),
          reader.U64(),
          reader.Identity<ContentDigest>()};
}

void PutRouteFenceIntent(Writer& writer, const RouteConfigurationFenceIntentDTO& value) {
  PutPublicRequestContext(writer, value.owner_request_context);
  writer.Identity(value.initiating_public_request_id);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  PutFenceScope(writer, value.scope);
  writer.Identity(value.admission_snapshot_digest);
  writer.EnumValue(value.busy_disposition);
  writer.Identity(value.expected_retained_bundle_id);
  writer.Identity(value.expected_graph_revision);
  writer.U64(value.expected_operation_resource_generation);
  writer.Counter(value.expected_fence_generation);
  PutExpectedActiveConfiguration(writer, value.expected_active_control_configuration);
  writer.Identity(value.candidate_active_control_configuration_id);
  writer.Counter(value.candidate_active_control_configuration_generation);
  writer.String(value.reason);
}

[[nodiscard]] RouteConfigurationFenceIntentDTO GetRouteFenceIntent(Reader& reader) {
  return {GetPublicRequestContext(reader),
          reader.Identity<RequestId>(),
          reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          GetFenceScope(reader),
          reader.Identity<ContentDigest>(),
          reader.EnumValue<RouteConfigurationFenceBusyDispositionDTO>(),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          reader.U64(),
          reader.Counter<ConfigurationFenceGeneration>(),
          GetExpectedActiveConfiguration(reader),
          reader.Identity<ActiveControlConfigurationId>(),
          reader.Counter<ActiveControlConfigurationGeneration>(),
          reader.String()};
}

void PutRouteFenceAccepted(Writer& writer, const RouteConfigurationFenceAcceptedDTO& value) {
  writer.Identity(value.request_id);
  writer.EnumValue(value.replay_status);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  PutFenceScope(writer, value.captured_scope);
  writer.Identity(value.candidate_active_control_configuration_id);
  writer.Counter(value.candidate_active_control_configuration_generation);
  writer.I64(value.accepted_at_ns);
  writer.Identity(value.acceptance_digest);
}

[[nodiscard]] RouteConfigurationFenceAcceptedDTO GetRouteFenceAccepted(Reader& reader) {
  return {reader.Identity<RequestId>(),
          reader.EnumValue<RequestReplayStatusDTO>(),
          reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          reader.Identity<ConfigurationFenceId>(),
          reader.Counter<ConfigurationFenceGeneration>(),
          GetFenceScope(reader),
          reader.Identity<ActiveControlConfigurationId>(),
          reader.Counter<ActiveControlConfigurationGeneration>(),
          reader.I64(),
          reader.Identity<ContentDigest>()};
}

void PutRouteFenceEvidence(Writer& writer,
                           const RouteConfigurationFenceReconciliationEvidenceDTO& value) {
  PutExecutionHostEpoch(writer, value.execution_host_epoch);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  PutFenceScope(writer, value.captured_scope);
  writer.U64(value.operation_resource_generation);
  writer.Identity(value.active_retained_bundle_id);
  writer.Identity(value.active_graph_revision);
  PutProjectionRevision(writer, value.active_projection_revision);
  writer.Counter(value.active_publication_generation);
  writer.Identity(value.active_public_pointer_digest);
  PutExpectedActiveConfiguration(writer, value.current_active_control_configuration);
  writer.U64(value.affected_queued_operation_count);
  writer.U64(value.affected_active_operation_count);
  writer.Identity(value.evidence_digest);
}

[[nodiscard]] RouteConfigurationFenceReconciliationEvidenceDTO GetRouteFenceEvidence(
    Reader& reader) {
  return {GetExecutionHostEpoch(reader),
          reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          reader.Identity<ConfigurationFenceId>(),
          reader.Counter<ConfigurationFenceGeneration>(),
          GetFenceScope(reader),
          reader.U64(),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          GetProjectionRevision(reader),
          reader.Counter<ActiveBundlePublicationGeneration>(),
          reader.Identity<ContentDigest>(),
          GetExpectedActiveConfiguration(reader),
          reader.U64(),
          reader.U64(),
          reader.Identity<ContentDigest>()};
}

void PutRouteFenceReleaseRequest(Writer& writer,
                                 const RouteConfigurationFenceReleaseRequestDTO& value) {
  PutPublicRequestContext(writer, value.owner_request_context);
  writer.Identity(value.initiating_public_request_id);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  PutRouteFenceAccepted(writer, value.accepted_fence);
  writer.U64(value.release_precondition_status_sequence);
  writer.Identity(value.release_precondition_status_digest);
  PutExpectedActiveConfiguration(writer, value.release_active_control_configuration);
  PutRouteFenceEvidence(writer, value.held_evidence);
}

[[nodiscard]] RouteConfigurationFenceReleaseRequestDTO GetRouteFenceReleaseRequest(Reader& reader) {
  return {GetPublicRequestContext(reader),
          reader.Identity<RequestId>(),
          reader.Identity<RouteSelectionTransactionId>(),
          reader.Counter<RouteSelectionTransactionGeneration>(),
          GetRouteFenceAccepted(reader),
          reader.U64(),
          reader.Identity<ContentDigest>(),
          GetExpectedActiveConfiguration(reader),
          GetRouteFenceEvidence(reader)};
}

void PutRouteFenceReleased(Writer& writer, const RouteConfigurationFenceReleasedDTO& value) {
  PutRouteFenceReleaseRequest(writer, value.request);
  writer.EnumValue(value.replay_status);
  writer.I64(value.released_at_ns);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  writer.Identity(value.released_scope_digest);
  writer.Identity(value.receipt_digest);
}

[[nodiscard]] RouteConfigurationFenceReleasedDTO GetRouteFenceReleased(Reader& reader) {
  return {GetRouteFenceReleaseRequest(reader),
          reader.EnumValue<RequestReplayStatusDTO>(),
          reader.I64(),
          reader.Identity<ConfigurationFenceId>(),
          reader.Counter<ConfigurationFenceGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ContentDigest>()};
}

void PutRouteFenceStatusQuery(Writer& writer, const RouteConfigurationFenceStatusQueryDTO& value) {
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  PutFenceScope(writer, value.expected_scope);
}

void PutRouteFenceStatus(Writer& writer, const RouteConfigurationFenceStatusDTO& value) {
  PutRouteFenceStatusQuery(writer, value.query);
  PutRouteFenceEvidence(writer, value.evidence);
  writer.EnumValue(value.state);
  writer.Bool(value.admission_fenced);
  writer.Optional<RouteConfigurationFenceReleasedDTO>(
      value.release_receipt, [&](const auto& item) { PutRouteFenceReleased(writer, item); });
  writer.String(value.required_recovery_direction);
  writer.Identity(value.result_digest);
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
}

void PutRouteFenceAcquireResult(Writer& writer,
                                const RouteConfigurationFenceAcquireEffectResultDTO& value) {
  PutRouteFenceIntent(writer, value.intent);
  writer.EnumValue(value.disposition);
  writer.Optional<RouteConfigurationFenceAcceptedDTO>(
      value.acceptance, [&](const auto& item) { PutRouteFenceAccepted(writer, item); });
}

[[nodiscard]] RouteConfigurationFenceAcquireEffectResultDTO GetRouteFenceAcquireResult(
    Reader& reader) {
  return {GetRouteFenceIntent(reader),
          reader.EnumValue<RouteConfigurationFenceAcquireEffectDispositionDTO>(),
          reader.Optional<RouteConfigurationFenceAcceptedDTO>(
              [&]() { return GetRouteFenceAccepted(reader); })};
}

void PutRouteFenceReleaseResult(Writer& writer,
                                const RouteConfigurationFenceReleaseEffectResultDTO& value) {
  PutRouteFenceReleaseRequest(writer, value.request);
  writer.EnumValue(value.disposition);
  writer.Optional<RouteConfigurationFenceReleasedDTO>(
      value.receipt, [&](const auto& item) { PutRouteFenceReleased(writer, item); });
}

[[nodiscard]] RouteConfigurationFenceReleaseEffectResultDTO GetRouteFenceReleaseResult(
    Reader& reader) {
  return {GetRouteFenceReleaseRequest(reader),
          reader.EnumValue<RouteConfigurationFenceReleaseEffectDispositionDTO>(),
          reader.Optional<RouteConfigurationFenceReleasedDTO>(
              [&]() { return GetRouteFenceReleased(reader); })};
}

void PutRouteStatus(Writer& writer, const RouteSelectionStatusDTO& value) {
  writer.String(value.accepted_intent.expected_admission_generation ? kRouteAdmissionStatusMagic
                                                                    : kRouteStatusMagic);
  PutRouteAccepted(writer, value.acceptance);
  PutRouteIntent(writer, value.accepted_intent, true);
  writer.U64(value.status_sequence);
  writer.Identity(value.status_owner_epoch);
  writer.EnumValue(value.phase);
  writer.EnumValue(value.effect);
  writer.Vector(value.affected_control_binding_ids,
                [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.affected_controller_role_ids,
                [&](const auto& item) { writer.Identity(item); });
  writer.Optional<ControllerLifecycleObservationDTO>(
      value.controller_observation,
      [&](const auto& item) { PutControllerObservation(writer, item); });
  writer.Optional<ControllerLifecycleReconciliationDTO>(
      value.controller_reconciliation,
      [&](const auto& item) { PutControllerReconciliation(writer, item); });
  writer.Optional<ControllerLifecycleTransitionPlanDTO>(
      value.controller_transition_plan,
      [&](const auto& item) { PutControllerTransitionPlan(writer, item); });
  writer.Vector(value.controller_transition_results,
                [&](const auto& item) { PutControllerTransitionResult(writer, item); });
  writer.Optional<RouteConfigurationFenceAcceptedDTO>(
      value.accepted_fence, [&](const auto& item) { PutRouteFenceAccepted(writer, item); });
  writer.Optional<RouteConfigurationFenceReleasedDTO>(
      value.released_fence, [&](const auto& item) { PutRouteFenceReleased(writer, item); });
  writer.Optional<ActiveControlConfigurationDTO>(
      value.resulting_active_control_configuration,
      [&](const auto& item) { PutActiveConfiguration(writer, item); });
  writer.Bool(value.reconciliation_required);
  writer.Optional<RouteSelectionTerminalResultDTO>(
      value.terminal_result, [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
  writer.I64(value.accepted_at_ns);
  writer.I64(value.latest_updated_at_ns);
  writer.I64(value.remaining_duration_ns);
  if (value.accepted_intent.expected_admission_generation) {
    writer.Counter(*value.accepted_intent.expected_admission_generation);
    writer.Optional<ProgramNodeAdmissionEvidenceDTO>(
        value.accepted_intent.program_node_evidence,
        [&](const auto& evidence) { writer.String(EncodeProgramNodeAdmissionEvidence(evidence)); });
  }
}

[[nodiscard]] RouteSelectionStatusDTO GetRouteStatus(Reader& reader) {
  const auto magic = reader.String();
  if (magic != kRouteStatusMagic && magic != kRouteAdmissionStatusMagic) {
    throw CodecFailure("CCLC route status magic is invalid");
  }
  RouteSelectionStatusDTO result{
      GetRouteAccepted(reader),
      GetRouteIntent(reader),
      reader.U64(),
      reader.Identity<RequestOwnerEpoch>(),
      reader.EnumValue<RouteSelectionTransactionPhaseDTO>(),
      reader.EnumValue<RouteSelectionEffectDTO>(),
      reader.Vector<ControlBindingId>([&]() { return reader.Identity<ControlBindingId>(); }),
      reader.Vector<ControllerRoleId>([&]() { return reader.Identity<ControllerRoleId>(); }),
      reader.Optional<ControllerLifecycleObservationDTO>(
          [&]() { return GetControllerObservation(reader); }),
      reader.Optional<ControllerLifecycleReconciliationDTO>(
          [&]() { return GetControllerReconciliation(reader); }),
      reader.Optional<ControllerLifecycleTransitionPlanDTO>(
          [&]() { return GetControllerTransitionPlan(reader); }),
      reader.Vector<ControllerLifecycleTransitionResultDTO>(
          [&]() { return GetControllerTransitionResult(reader); }),
      reader.Optional<RouteConfigurationFenceAcceptedDTO>(
          [&]() { return GetRouteFenceAccepted(reader); }),
      reader.Optional<RouteConfigurationFenceReleasedDTO>(
          [&]() { return GetRouteFenceReleased(reader); }),
      reader.Optional<ActiveControlConfigurationDTO>(
          [&]() { return GetActiveConfiguration(reader); }),
      reader.Bool(),
      reader.Optional<RouteSelectionTerminalResultDTO>(
          [&]() { return reader.EnumValue<RouteSelectionTerminalResultDTO>(); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); }),
      reader.I64(),
      reader.I64(),
      reader.I64()};
  if (magic == kRouteAdmissionStatusMagic) {
    result.accepted_intent.expected_admission_generation = reader.Counter<AdmissionGeneration>();
    result.accepted_intent.program_node_evidence = reader.Optional<ProgramNodeAdmissionEvidenceDTO>(
        [&] { return DecodeProgramNodeAdmissionEvidence(reader.String()); });
  }
  return result;
}

void PutRouteEffectAttempt(Writer& writer, const CCLCRouteEffectAttemptDTO& value) {
  writer.String(kRouteEffectAttemptMagic);
  writer.Identity(value.route_selection_transaction_id);
  writer.Counter(value.route_selection_transaction_generation);
  writer.U64(value.effect_sequence);
  writer.EnumValue(value.effect_kind);
  writer.EnumValue(value.state);
  switch (value.effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire:
      PutRouteFenceIntent(writer, std::get<RouteConfigurationFenceIntentDTO>(value.request));
      if (value.result.has_value()) {
        PutRouteFenceAcquireResult(
            writer, std::get<RouteConfigurationFenceAcquireEffectResultDTO>(*value.result));
      }
      break;
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition:
      PutControllerTransitionIntent(
          writer, std::get<ControllerLifecycleTransitionIntentDTO>(value.request));
      if (value.result.has_value()) {
        PutControllerTransitionResult(
            writer, std::get<ControllerLifecycleTransitionResultDTO>(*value.result));
      }
      break;
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease:
      PutRouteFenceReleaseRequest(
          writer, std::get<RouteConfigurationFenceReleaseRequestDTO>(value.request));
      if (value.result.has_value()) {
        PutRouteFenceReleaseResult(
            writer, std::get<RouteConfigurationFenceReleaseEffectResultDTO>(*value.result));
      }
      break;
  }
}

[[nodiscard]] CCLCRouteEffectAttemptDTO GetRouteEffectAttempt(Reader& reader) {
  if (reader.String() != kRouteEffectAttemptMagic) {
    throw CodecFailure("CCLC route effect-attempt magic is invalid");
  }
  const auto transaction_id = reader.Identity<RouteSelectionTransactionId>();
  const auto transaction_generation = reader.Counter<RouteSelectionTransactionGeneration>();
  const auto effect_sequence = reader.U64();
  const auto effect_kind = reader.EnumValue<CCLCRouteEffectKindDTO>();
  const auto state = reader.EnumValue<CCLCRouteEffectAttemptStateDTO>();
  const bool completed = state == CCLCRouteEffectAttemptStateDTO::kCompleted;
  switch (effect_kind) {
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceAcquire: {
      CCLCRouteEffectRequestDTO request{GetRouteFenceIntent(reader)};
      std::optional<CCLCRouteEffectResultDTO> result;
      if (completed) {
        result = CCLCRouteEffectResultDTO{GetRouteFenceAcquireResult(reader)};
      }
      return {transaction_id, transaction_generation, effect_sequence,  effect_kind,
              state,          std::move(request),     std::move(result)};
    }
    case CCLCRouteEffectKindDTO::kControllerLifecycleTransition: {
      CCLCRouteEffectRequestDTO request{GetControllerTransitionIntent(reader)};
      std::optional<CCLCRouteEffectResultDTO> result;
      if (completed) {
        result = CCLCRouteEffectResultDTO{GetControllerTransitionResult(reader)};
      }
      return {transaction_id, transaction_generation, effect_sequence,  effect_kind,
              state,          std::move(request),     std::move(result)};
    }
    case CCLCRouteEffectKindDTO::kKernelRouteConfigurationFenceRelease: {
      CCLCRouteEffectRequestDTO request{GetRouteFenceReleaseRequest(reader)};
      std::optional<CCLCRouteEffectResultDTO> result;
      if (completed) {
        result = CCLCRouteEffectResultDTO{GetRouteFenceReleaseResult(reader)};
      }
      return {transaction_id, transaction_generation, effect_sequence,  effect_kind,
              state,          std::move(request),     std::move(result)};
    }
  }
  throw CodecFailure("CCLC route effect kind is invalid");
}

void PutEffectAttempt(Writer& writer, const CCLCEffectAttemptDTO& value) {
  writer.String(kEffectAttemptMagic);
  writer.EnumValue(value.request_kind);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.U64(value.effect_sequence);
  writer.EnumValue(value.effect_kind);
  writer.EnumValue(value.state);
  writer.Optional<BrandLifecyclePrimitiveIntentDTO>(
      value.lifecycle_primitive_intent,
      [&](const auto& item) { PutPrimitiveIntent(writer, item); });
  writer.Optional<BrandLifecyclePrimitiveResultDTO>(
      value.lifecycle_primitive_result,
      [&](const auto& item) { PutPrimitiveResult(writer, item); });
}

[[nodiscard]] CCLCEffectAttemptDTO GetEffectAttempt(Reader& reader) {
  if (reader.String() != kEffectAttemptMagic) {
    throw CodecFailure("CCLC effect-attempt magic is invalid");
  }
  return {
      reader.EnumValue<CCLCRequestKindDTO>(),
      reader.Identity<LifecycleTransactionId>(),
      reader.Counter<LifecycleTransactionGeneration>(),
      reader.U64(),
      reader.EnumValue<CCLCEffectKindDTO>(),
      reader.EnumValue<CCLCEffectAttemptStateDTO>(),
      reader.Optional<BrandLifecyclePrimitiveIntentDTO>(
          [&]() { return GetPrimitiveIntent(reader); }),
      reader.Optional<BrandLifecyclePrimitiveResultDTO>(
          [&]() { return GetPrimitiveResult(reader); })};
}

void PutDomainStatus(Writer& writer, const ComponentLifecycleDomainStatusDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Optional<ExpectedControlDomainRevisionDTO>(
      value.expected_domain, [&](const auto& item) { PutExpectedDomain(writer, item); });
  writer.Optional<ResolvedLifecycleControlDomainScopeDTO>(
      value.current_scope, [&](const auto& item) { PutResolvedScope(writer, item); });
  writer.Optional<ResolvedLifecycleProviderOccurrenceDTO>(
      value.resolved_provider_occurrence,
      [&](const auto& item) { PutResolvedProviderOccurrence(writer, item); });
  writer.Optional<ComponentLifecycleStateDTO>(value.previous_state,
                                              [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ComponentLifecycleStateDTO>(value.current_state,
                                              [&](const auto item) { writer.EnumValue(item); });
  PutRequestedState(writer, value.requested_state);
  writer.EnumValue(value.action_state);
  writer.Vector(value.primitive_attempts,
                [&](const auto& item) { PutPrimitiveResult(writer, item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
  writer.Vector(value.substep_events, [&](const auto& item) { PutSubstepEvent(writer, item); });
  writer.Optional<LifecycleTransitionObservationDTO>(
      value.observed_before, [&](const auto& item) { PutTransitionObservation(writer, item); });
  writer.Optional<LifecycleTransitionObservationDTO>(
      value.observed_after, [&](const auto& item) { PutTransitionObservation(writer, item); });
}

[[nodiscard]] ComponentLifecycleDomainStatusDTO GetDomainStatus(Reader& reader) {
  return ComponentLifecycleDomainStatusDTO{
      reader.Identity<ControlDomainId>(),
      reader.Optional<ExpectedControlDomainRevisionDTO>(
          [&]() { return GetExpectedDomain(reader); }),
      reader.Optional<ResolvedLifecycleControlDomainScopeDTO>(
          [&]() { return GetResolvedScope(reader); }),
      reader.Optional<ResolvedLifecycleProviderOccurrenceDTO>(
          [&]() { return GetResolvedProviderOccurrence(reader); }),
      reader.Optional<ComponentLifecycleStateDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleStateDTO>(); }),
      reader.Optional<ComponentLifecycleStateDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleStateDTO>(); }),
      GetRequestedState(reader),
      reader.EnumValue<ComponentLifecycleDomainActionStateDTO>(),
      reader.Vector<BrandLifecyclePrimitiveResultDTO>([&]() { return GetPrimitiveResult(reader); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); }),
      reader.Vector<LifecycleSubstepEventDTO>([&]() { return GetSubstepEvent(reader); }),
      reader.Optional<LifecycleTransitionObservationDTO>(
          [&]() { return GetTransitionObservation(reader); }),
      reader.Optional<LifecycleTransitionObservationDTO>(
          [&]() { return GetTransitionObservation(reader); })};
}

void PutProgramNode(Writer& writer, const ProgramNodeAdmissionEvidenceDTO& value) {
  writer.String(kProgramNodeMagic);
  writer.Identity(value.program_runner_owner_epoch);
  writer.Identity(value.program_run_id);
  writer.Counter(value.parent_program_cycle_claim_generation);
  writer.String(value.node_instance_id);
  writer.String(value.node_attempt_id);
  writer.String(value.owner_api);
}

[[nodiscard]] ProgramNodeAdmissionEvidenceDTO GetProgramNode(Reader& reader) {
  if (reader.String() != kProgramNodeMagic) {
    throw CodecFailure("Program node admission magic is invalid");
  }
  ProgramNodeAdmissionEvidenceDTO result{reader.Identity<RequestOwnerEpoch>(),
                                         reader.Identity<ProgramRunId>(),
                                         reader.Counter<CellAdmissionClaimGeneration>(),
                                         reader.String(),
                                         reader.String(),
                                         reader.String()};
  if (!result.locally_valid()) {
    throw CodecFailure("Program node admission evidence is invalid");
  }
  return result;
}

[[nodiscard]] bool SelectedClaimIntentValid(const CellAdmissionClaimIntentDTO& intent) {
  constexpr std::string_view prefix = "selected-lifecycle.";
  if (!ValidateCellAdmissionClaimIntent(intent).ok() ||
      intent.claim_kind != CellAdmissionClaimKindDTO::kInterventionOrRecovery ||
      intent.owner_role != CellAdmissionOwnerRoleDTO::kComponentLifecycleOwner ||
      !intent.owner_transaction_id.starts_with(prefix) || intent.program_cycle_evidence ||
      intent.program_node_evidence || !intent.intervention_evidence ||
      intent.intervention_evidence->parent_program_cycle_claim_generation) return false;
  const std::string_view suffix(intent.owner_transaction_id.data() + prefix.size(),
                                intent.owner_transaction_id.size() - prefix.size());
  std::uint64_t generation{};
  const auto parsed = std::from_chars(suffix.data(), suffix.data() + suffix.size(), generation);
  return parsed.ec == std::errc{} && parsed.ptr == suffix.data() + suffix.size() &&
         generation != 0U && suffix == std::to_string(generation);
}

void PutSelectedClaimIntent(Writer& writer, const CellAdmissionClaimIntentDTO& value) {
  writer.EnumValue(value.claim_kind);
  writer.EnumValue(value.owner_role);
  writer.Identity(value.owner_epoch);
  writer.String(value.owner_transaction_id);
  writer.Identity(value.request_id);
  writer.Identity(value.request_payload_digest);
  writer.Counter(value.expected_admission_generation);
  writer.Vector(value.scope.resources, [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.scope.configurations, [&](const auto& item) {
    writer.Identity(item.control_domain_id);
    writer.Identity(item.active_configuration_id);
    writer.Counter(item.active_configuration_generation);
  });
  writer.Optional<ProgramCycleAdmissionEvidenceDTO>(value.program_cycle_evidence,
      [&](const auto& item) {
        writer.Identity(item.draft_program_revision);
        writer.Identity(item.applied_program_revision);
      });
  writer.Optional<CellAdmissionInterventionEvidenceDTO>(value.intervention_evidence,
      [&](const auto& item) {
        writer.EnumValue(item.offering_owner_role);
        writer.Identity(item.offering_owner_epoch);
        writer.String(item.offer_transaction_id);
        writer.Identity(item.offer_digest);
        writer.Optional<CellAdmissionClaimGeneration>(item.parent_program_cycle_claim_generation,
            [&](const auto& generation) { writer.Counter(generation); });
      });
  writer.Optional<ProgramNodeAdmissionEvidenceDTO>(value.program_node_evidence,
      [&](const auto& item) { PutProgramNode(writer, item); });
}

[[nodiscard]] CellAdmissionClaimIntentDTO GetSelectedClaimIntent(Reader& reader) {
  return {reader.EnumValue<CellAdmissionClaimKindDTO>(),
          reader.EnumValue<CellAdmissionOwnerRoleDTO>(),
          reader.Identity<RequestOwnerEpoch>(), reader.String(), reader.Identity<RequestId>(),
          reader.Identity<ContentDigest>(), reader.Counter<AdmissionGeneration>(),
          {reader.Vector<ResourceId>([&]() { return reader.Identity<ResourceId>(); }),
           reader.Vector<CellAdmissionConfigurationScopeDTO>([&]() {
             return CellAdmissionConfigurationScopeDTO{reader.Identity<ControlDomainId>(),
                 reader.Identity<ActiveControlConfigurationId>(),
                 reader.Counter<ActiveControlConfigurationGeneration>()};
           })},
          reader.Optional<ProgramCycleAdmissionEvidenceDTO>([&]() {
            return ProgramCycleAdmissionEvidenceDTO{reader.Identity<ProgramRevision>(),
                                                    reader.Identity<ProgramRevision>()};
          }),
          reader.Optional<CellAdmissionInterventionEvidenceDTO>([&]() {
            return CellAdmissionInterventionEvidenceDTO{reader.EnumValue<CellAdmissionOwnerRoleDTO>(),
                reader.Identity<RequestOwnerEpoch>(), reader.String(), reader.Identity<ContentDigest>(),
                reader.Optional<CellAdmissionClaimGeneration>([&]() {
                  return reader.Counter<CellAdmissionClaimGeneration>();
                })};
          }),
          reader.Optional<ProgramNodeAdmissionEvidenceDTO>([&]() { return GetProgramNode(reader); })};
}

void PutSelectedOccurrence(Writer& writer, const SelectedControllerOccurrenceDTO& value) {
  writer.Optional<ExecutionBindingEpoch>(value.arm_binding_epoch,
      [&](const auto& item) { writer.Counter(item); });
  writer.Vector(value.domains, [&](const auto& item) {
    writer.Identity(item.control_domain_id);
    writer.U64(item.observation_generation);
  });
}

[[nodiscard]] SelectedControllerOccurrenceDTO GetSelectedOccurrence(Reader& reader) {
  return {reader.Optional<ExecutionBindingEpoch>([&]() {
            return reader.Counter<ExecutionBindingEpoch>();
          }),
          reader.Vector<SelectedDomainOccurrenceDTO>([&]() {
            return SelectedDomainOccurrenceDTO{reader.Identity<ControlDomainId>(), reader.U64()};
          })};
}

void PutSelectedBringUp(Writer& writer, const SelectedComponentBringUpEvidenceDTO& value) {
  writer.Identity(value.offer_digest);
  writer.Identity(value.predecessor_snapshot_digest);
  writer.String(value.incident_id);
  writer.Identity(value.containment_stop_id);
  writer.Counter(value.containment_stop_generation);
  PutProjectionRevision(writer, value.candidate_projection_revision);
  writer.Identity(value.candidate_retained_bundle_digest);
  writer.U64(value.candidate_retained_bundle_bytes);
  PutSelectedOccurrence(writer, value.predecessor_occurrence);
  PutSelectedOccurrence(writer, value.successor_occurrence);
  PutSelectedClaimIntent(writer, value.selected_claim.intent);
  writer.Counter(value.selected_claim.claim_generation);
  writer.Counter(value.selected_claim.acquired_admission_generation);
  writer.I64(value.selected_claim.accepted_at_ns);
  writer.Identity(value.accepted_fence.configuration_fence_id);
  writer.Counter(value.accepted_fence.configuration_fence_generation);
  PutFenceScope(writer, value.accepted_fence.captured_scope);
  writer.I64(value.accepted_fence.accepted_at_ns);
  writer.EnumValue(value.accepted_fence.replay_status);
  writer.Identity(value.original_request_payload_digest);
}

[[nodiscard]] SelectedComponentBringUpEvidenceDTO GetSelectedBringUp(Reader& reader) {
  return {reader.Identity<ContentDigest>(), reader.Identity<ContentDigest>(), reader.String(),
          reader.Identity<StopId>(), reader.Counter<StopGeneration>(), GetProjectionRevision(reader),
          reader.Identity<ContentDigest>(), reader.U64(), GetSelectedOccurrence(reader),
          GetSelectedOccurrence(reader),
          {GetSelectedClaimIntent(reader), reader.Counter<CellAdmissionClaimGeneration>(),
           reader.Counter<AdmissionGeneration>(), reader.I64()},
          {reader.Identity<ConfigurationFenceId>(), reader.Counter<ConfigurationFenceGeneration>(),
           GetFenceScope(reader), reader.I64(), reader.EnumValue<RequestReplayStatusDTO>()},
          reader.Identity<ContentDigest>()};
}

void PutApplyReference(Writer& writer, const ApplyComponentBringUpReferenceDTO& value) {
  writer.Identity(value.owner.apply_id);
  writer.Counter(value.owner.apply_generation);
  writer.Identity(value.plan_digest);
}

[[nodiscard]] ApplyComponentBringUpReferenceDTO GetApplyReference(Reader& reader) {
  return {{reader.Identity<ApplyId>(), reader.Counter<ApplyGeneration>()},
          reader.Identity<ContentDigest>()};
}

void PutApplyAuthority(Writer& writer, const ApplyComponentBringUpAuthorityDTO& authority) {
  PutApplyReference(writer, authority.parent);
  // The existing concrete full-claim grammar also carries the Manager claim;
  // no independent child claim or duplicated Manager plan is introduced.
  PutSelectedClaimIntent(writer, authority.parent_claim.intent);
  writer.Counter(authority.parent_claim.claim_generation);
  writer.Counter(authority.parent_claim.acquired_admission_generation);
  writer.I64(authority.parent_claim.accepted_at_ns);
  const auto& query = authority.held_fence_query;
  writer.Identity(query.owner_transaction.apply_id);
  writer.Counter(query.owner_transaction.apply_generation);
  writer.Identity(query.configuration_fence_id);
  writer.Counter(query.configuration_fence_generation);
  PutFenceScope(writer, query.expected_scope);
  writer.Identity(query.manager_runtime_epoch);
}

[[nodiscard]] ApplyComponentBringUpAuthorityDTO GetApplyAuthority(Reader& reader) {
  return {GetApplyReference(reader),
          {GetSelectedClaimIntent(reader), reader.Counter<CellAdmissionClaimGeneration>(),
           reader.Counter<AdmissionGeneration>(), reader.I64()},
          {{reader.Identity<ApplyId>(), reader.Counter<ApplyGeneration>()},
           reader.Identity<ConfigurationFenceId>(), reader.Counter<ConfigurationFenceGeneration>(),
           GetFenceScope(reader), reader.Identity<RuntimeEpoch>()}};
}

void PutApplyBringUp(Writer& writer, const ApplyComponentBringUpEvidenceDTO& value) {
  PutApplyAuthority(writer, value.parent_authority);
  PutFenceScope(writer, value.selected_scope);
  writer.Identity(value.original_request_payload_digest);
  PutProjectionRevision(writer, value.candidate_projection_revision);
  writer.Identity(value.candidate_retained_bundle_digest);
  writer.U64(value.candidate_retained_bundle_bytes);
  writer.Identity(value.original_execution_host.runtime_unit_id);
  writer.Identity(value.original_execution_host.runtime_epoch);
  writer.Optional<ApplyComponentBringUpRecoveryEvidenceDTO>(value.recovery, [&](const auto& item) {
    writer.Identity(item.predecessor_snapshot_digest);
    writer.String(item.incident_id);
    writer.Identity(item.containment_stop_id);
    writer.Counter(item.containment_stop_generation);
  });
  writer.Optional<SelectedControllerOccurrenceDTO>(value.predecessor_occurrence,
      [&](const auto& item) { PutSelectedOccurrence(writer, item); });
  writer.Optional<SelectedControllerOccurrenceDTO>(value.successor_occurrence_floor,
      [&](const auto& item) { PutSelectedOccurrence(writer, item); });
}

[[nodiscard]] ApplyComponentBringUpEvidenceDTO GetApplyBringUp(Reader& reader) {
  return {GetApplyAuthority(reader),
          GetFenceScope(reader), reader.Identity<ContentDigest>(),
          GetProjectionRevision(reader), reader.Identity<ContentDigest>(), reader.U64(),
          {reader.Identity<RuntimeUnitId>(), reader.Identity<RuntimeEpoch>()},
          reader.Optional<ApplyComponentBringUpRecoveryEvidenceDTO>([&]() {
            return ApplyComponentBringUpRecoveryEvidenceDTO{reader.Identity<ContentDigest>(),
                reader.String(), reader.Identity<StopId>(), reader.Counter<StopGeneration>()};
          }),
          reader.Optional<SelectedControllerOccurrenceDTO>([&]() {
            return GetSelectedOccurrence(reader);
          }),
          reader.Optional<SelectedControllerOccurrenceDTO>([&]() {
            return GetSelectedOccurrence(reader);
          })};
}

void PutStatus(Writer& writer, const ComponentLifecycleStatusDTO& value) {
  writer.String(value.apply_bring_up ? (HasQualifiedApplyPredecessor(value.accepted_intent)
                                           ? kApplyQualifiedGenerationStatusMagic
                                           : value.accepted_intent.apply_generation_selection
                                               ? kApplyGenerationStatusMagic : kApplyStatusMagic) :
                value.selected_bring_up ? kSelectedStatusMagic :
                value.accepted_intent.program_node_evidence ? kProgramStatusMagic : kStatusMagic);
  PutAccepted(writer, value.acceptance);
  PutIntent(writer, value.accepted_intent, true);
  writer.U64(value.status_sequence);
  writer.Identity(value.status_owner_epoch);
  writer.Optional<ComponentLifecycleStateDTO>(value.previous_state,
                                              [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ComponentLifecycleStateDTO>(value.current_state,
                                              [&](const auto item) { writer.EnumValue(item); });
  PutRequestedState(writer, value.requested_state);
  writer.EnumValue(value.phase);
  writer.Vector(value.domains, [&](const auto& item) { PutDomainStatus(writer, item); });
  writer.Bool(value.reconciliation_required);
  writer.Optional<ComponentLifecycleTerminalResultDTO>(
      value.terminal_result, [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
  writer.I64(value.accepted_at_ns);
  writer.I64(value.latest_updated_at_ns);
  writer.I64(value.remaining_duration_ns);
  if (value.accepted_intent.program_node_evidence) {
    PutProgramNode(writer, *value.accepted_intent.program_node_evidence);
  }
  if (value.selected_bring_up) PutSelectedBringUp(writer, *value.selected_bring_up);
  if (value.apply_bring_up) PutApplyBringUp(writer, *value.apply_bring_up);
  if (value.accepted_intent.apply_generation_selection)
    PutApplyGenerationSelection(writer, *value.accepted_intent.apply_generation_selection);
}

[[nodiscard]] ComponentLifecycleStatusDTO GetStatus(Reader& reader) {
  const auto magic = reader.String();
  if (magic != kStatusMagic && magic != kProgramStatusMagic && magic != kSelectedStatusMagic &&
      magic != kApplyStatusMagic && magic != kApplyGenerationStatusMagic &&
      magic != kApplyQualifiedGenerationStatusMagic) {
    throw CodecFailure("component lifecycle journal status magic is invalid");
  }
  ComponentLifecycleStatusDTO result{
      GetAccepted(reader),
      GetIntent(reader),
      reader.U64(),
      reader.Identity<RequestOwnerEpoch>(),
      reader.Optional<ComponentLifecycleStateDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleStateDTO>(); }),
      reader.Optional<ComponentLifecycleStateDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleStateDTO>(); }),
      GetRequestedState(reader),
      reader.EnumValue<ComponentLifecycleTransactionPhaseDTO>(),
      reader.Vector<ComponentLifecycleDomainStatusDTO>([&]() { return GetDomainStatus(reader); }),
      reader.Bool(),
      reader.Optional<ComponentLifecycleTerminalResultDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleTerminalResultDTO>(); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); }),
      reader.I64(),
      reader.I64(),
      reader.I64()};
  if (magic == kProgramStatusMagic) {
    result.accepted_intent.program_node_evidence = GetProgramNode(reader);
  }
  if (magic == kSelectedStatusMagic) result.selected_bring_up = GetSelectedBringUp(reader);
  if (magic == kApplyStatusMagic || magic == kApplyGenerationStatusMagic ||
      magic == kApplyQualifiedGenerationStatusMagic)
    result.apply_bring_up = GetApplyBringUp(reader);
  if (magic == kApplyGenerationStatusMagic || magic == kApplyQualifiedGenerationStatusMagic)
    result.accepted_intent.apply_generation_selection = GetApplyGenerationSelection(
        reader, magic == kApplyQualifiedGenerationStatusMagic);
  return result;
}

void PutRetirementTargets(Writer& writer,
                          const std::vector<ComponentLifecycleTargetDTO>& targets) {
  writer.Vector(targets, [&](const auto& target) {
    writer.Identity(target.controllable_component_id);
    writer.EnumValue(target.requested_state);
    writer.EnumValue(target.origin);
    writer.U64(target.target_generation);
    writer.Bool(target.reached);
    writer.Optional<LifecycleBlockingCauseClassDTO>(target.recovered_cause,
        [&](const auto cause) { writer.EnumValue(cause); });
  });
}

std::vector<ComponentLifecycleTargetDTO> GetRetirementTargets(Reader& reader) {
  return reader.Vector<ComponentLifecycleTargetDTO>([&] {
    return ComponentLifecycleTargetDTO{
        reader.Identity<ControllableComponentId>(),
        reader.EnumValue<LifecycleTargetStateDTO>(),
        reader.EnumValue<LifecycleTargetOriginDTO>(), reader.U64(), reader.Bool(),
        reader.Optional<LifecycleBlockingCauseClassDTO>([&] {
          return reader.EnumValue<LifecycleBlockingCauseClassDTO>();
        })};
  });
}

void PutRetirementIntent(Writer& writer,
                         const CCLCLifecycleTargetRetirementIntentDTO& intent) {
  PutPublicRequestContext(writer, intent.request_context);
  writer.Identity(intent.scope.request_owner_epoch);
  writer.U64(intent.scope.publication_generation.value());
  writer.Identity(intent.scope.active_public_pointer_digest);
  PutRetirementTargets(writer, intent.expected_targets);
}

CCLCLifecycleTargetRetirementIntentDTO GetRetirementIntent(Reader& reader) {
  auto context = GetPublicRequestContext(reader);
  CCLCLifecycleTargetRetirementScopeDTO scope{
      reader.Identity<RequestOwnerEpoch>(),
      ActiveBundlePublicationGeneration(reader.U64()), reader.Identity<ContentDigest>()};
  return {std::move(context), std::move(scope), GetRetirementTargets(reader)};
}

}  // namespace

std::string EncodeProgramNodeAdmissionEvidence(const ProgramNodeAdmissionEvidenceDTO& evidence) {
  if (!evidence.locally_valid()) {
    throw CodecFailure("Program node admission evidence is invalid");
  }
  Writer writer;
  PutProgramNode(writer, evidence);
  return std::move(writer).Finish();
}

std::string EncodeCCLCRouteRecoveryConfiguration(
    const CellAdmissionConfigurationScopeDTO& configuration) {
  if (!ValidateCellAdmissionScope({{}, {configuration}}).ok())
    throw CodecFailure("CCLC recovery configuration is invalid");
  Writer writer;
  writer.String("MF-CCLC-ROUTE-RECOVERY-V1");
  writer.String(configuration.control_domain_id.value());
  writer.String(configuration.active_configuration_id.value());
  writer.U64(configuration.active_configuration_generation.value());
  return std::move(writer).Finish();
}

CellAdmissionConfigurationScopeDTO DecodeCCLCRouteRecoveryConfiguration(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-CCLC-ROUTE-RECOVERY-V1")
    throw CodecFailure("CCLC recovery configuration magic is invalid");
  CellAdmissionConfigurationScopeDTO configuration{
      ControlDomainId(reader.String()), ActiveControlConfigurationId(reader.String()),
      ActiveControlConfigurationGeneration(reader.U64())};
  reader.RequireEnd();
  if (!ValidateCellAdmissionScope({{}, {configuration}}).ok())
    throw CodecFailure("CCLC recovery configuration is invalid");
  return configuration;
}

std::string EncodeCCLCSelectedBringUpClaimIntent(const SelectedCCLCClaimIntentRecordDTO& record) {
  if (!SelectedClaimIntentValid(record.full_claim_intent) ||
      !IsExactLowercaseSha256Digest(record.original_request_payload_digest.value()))
    throw CodecFailure("CCLC selected bring-up claim record is invalid");
  Writer writer;
  writer.String(kSelectedClaimMagic);
  PutSelectedClaimIntent(writer, record.full_claim_intent);
  writer.Identity(record.original_request_payload_digest);
  return std::move(writer).Finish();
}

SelectedCCLCClaimIntentRecordDTO DecodeCCLCSelectedBringUpClaimIntent(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kSelectedClaimMagic)
    throw CodecFailure("CCLC selected bring-up claim magic is invalid");
  SelectedCCLCClaimIntentRecordDTO record{GetSelectedClaimIntent(reader),
                                        reader.Identity<ContentDigest>()};
  reader.RequireEnd();
  if (!SelectedClaimIntentValid(record.full_claim_intent) ||
      !IsExactLowercaseSha256Digest(record.original_request_payload_digest.value()))
    throw CodecFailure("CCLC selected bring-up claim record failed closed validation");
  return record;
}

bool IsCCLCSelectedBringUpClaimIntentEncoding(std::string_view bytes) noexcept {
  if (bytes.size() < 4U + kSelectedClaimMagic.size()) return false;
  const auto length = static_cast<std::uint32_t>(kSelectedClaimMagic.size());
  for (std::size_t index = 0U; index < 4U; ++index) {
    if (static_cast<unsigned char>(bytes[index]) != ((length >> (index * 8U)) & 0xffU))
      return false;
  }
  return bytes.substr(4U, kSelectedClaimMagic.size()) == kSelectedClaimMagic;
}

std::string EncodeCCLCTargetRetirementRecord(const CCLCTargetRetirementRecordDTO& record) {
  if (!record.intent.locally_valid() ||
      record.retired_targets != record.intent.expected_targets)
    throw CodecFailure("CCLC target retirement record is invalid");
  Writer writer;
  writer.String("MF-CCLC-TARGET-RETIREMENT-RESULT-V1");
  PutRetirementIntent(writer, record.intent);
  // Successful CAS retires exactly these rows. Do not duplicate them in the
  // result blob or invent lifecycle transaction identities.
  return std::move(writer).Finish();
}

CCLCTargetRetirementRecordDTO DecodeCCLCTargetRetirementRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-CCLC-TARGET-RETIREMENT-RESULT-V1")
    throw CodecFailure("CCLC target retirement record magic is invalid");
  auto intent = GetRetirementIntent(reader);
  reader.RequireEnd();
  if (!intent.locally_valid())
    throw CodecFailure("CCLC target retirement record failed closed validation");
  auto retired = intent.expected_targets;
  return {std::move(intent), std::move(retired)};
}

std::string CCLCTargetRetirementSemanticIntentDigest(
    const CCLCLifecycleTargetRetirementIntentDTO& intent) {
  if (!intent.locally_valid())
    throw CodecFailure("CCLC target retirement intent is invalid");
  Writer writer;
  writer.String("MF-CCLC-TARGET-RETIREMENT-INTENT-V1");
  PutRetirementIntent(writer, intent);
  return content_identity::sha256_digest(std::move(writer).Finish());
}

std::string EncodeCCLCAdmissionRejection(const ErrorDTO& error) {
  if (!CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
                                     std::nullopt, error}
           .locally_valid()) {
    throw CodecFailure("CCLC no-admission result is invalid");
  }
  Writer writer;
  writer.String("MF-CCLC-REJECTION-V1");
  PutError(writer, error);
  return std::move(writer).Finish();
}

ErrorDTO DecodeCCLCAdmissionRejection(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-CCLC-REJECTION-V1")
    throw CodecFailure("CCLC rejection magic is invalid");
  auto error = GetError(reader);
  reader.RequireEnd();
  if (!CCLCLifecycleRequestResultDTO{RequestReplayStatusDTO::kAcceptedNew, std::nullopt,
                                     std::nullopt, error}
           .locally_valid()) {
    throw CodecFailure("CCLC no-admission result is invalid");
  }
  return error;
}

ProgramNodeAdmissionEvidenceDTO DecodeProgramNodeAdmissionEvidence(std::string_view bytes) {
  Reader reader(bytes);
  auto evidence = GetProgramNode(reader);
  reader.RequireEnd();
  return evidence;
}

namespace {
[[nodiscard]] bool NativeApplyComponentBringUpShapeIsValid(const NativeApplyComponentBringUpDTO& value) {
  if (!value.parent.valid() || !IsExactLowercaseSha256Digest(value.parent.plan_digest.value()) ||
      value.children.empty() || value.children.size() > kMaximumCCLCComponentWatermarks) return false;
  const auto identity = [](const auto& id) {
    return !id.empty() && id.value().size() <= kMaxLifecycleIdentityBytes;
  };
  for (std::size_t index = 0U; index < value.children.size(); ++index) {
    const auto& child = value.children[index];
    const auto& context = child.request_context;
    if (!identity(child.component_id) || !identity(context.request_id) ||
        !identity(context.request_owner_epoch) || context.source_label.empty() ||
        context.source_label.size() > kMaxLifecycleRequestLabelBytes ||
        (context.correlation_id && (context.correlation_id->empty() ||
             context.correlation_id->size() > kMaxLifecycleRequestLabelBytes)) ||
        !child.acceptance.locally_valid() ||
        child.acceptance.replay_status != RequestReplayStatusDTO::kAcceptedNew ||
        child.acceptance.request_id != context.request_id ||
        child.acceptance.lifecycle_transaction_id.value() !=
            "apply-lifecycle." + std::to_string(child.acceptance.lifecycle_transaction_generation.value()) ||
        !IsExactLowercaseSha256Digest(child.original_request_payload_digest.value()) ||
        (index && !(value.children[index - 1U].component_id < child.component_id))) return false;
  }
  return true;
}
}  // namespace

std::string EncodeNativeApplyComponentBringUp(const NativeApplyComponentBringUpDTO& value) {
  if (!NativeApplyComponentBringUpShapeIsValid(value))
    throw CodecFailure("native Apply delegated receipts are invalid");
  Writer writer;
  writer.String("MF-CCLC-NATIVE-APPLY-BRING-UP-V1");
  PutApplyReference(writer, value.parent);
  writer.Vector(value.children, [&](const auto& child) {
    PutPublicRequestContext(writer, child.request_context);
    writer.Identity(child.component_id);
    PutAccepted(writer, child.acceptance);
    writer.Identity(child.original_request_payload_digest);
  });
  return std::move(writer).Finish();
}

NativeApplyComponentBringUpDTO DecodeNativeApplyComponentBringUp(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-CCLC-NATIVE-APPLY-BRING-UP-V1")
    throw CodecFailure("native Apply delegated receipt magic is invalid");
  NativeApplyComponentBringUpDTO value{
      GetApplyReference(reader), reader.Vector<ApplyComponentBringUpAcceptedReceiptDTO>([&] {
        return ApplyComponentBringUpAcceptedReceiptDTO{
            GetPublicRequestContext(reader), reader.Identity<ControllableComponentId>(),
            GetAccepted(reader), reader.Identity<ContentDigest>()};
      })};
  reader.RequireEnd();
  if (!NativeApplyComponentBringUpShapeIsValid(value))
    throw CodecFailure("native Apply delegated receipts failed closed validation");
  return value;
}

std::string EncodeComponentLifecycleStatus(const ComponentLifecycleStatusDTO& status) {
  if (!status.locally_valid()) {
    throw CodecFailure("component lifecycle status is invalid at journal ingress");
  }
  Writer writer;
  PutStatus(writer, status);
  return std::move(writer).Finish();
}

ComponentLifecycleStatusDTO DecodeComponentLifecycleStatus(std::string_view bytes) {
  Reader reader(bytes);
  auto status = GetStatus(reader);
  reader.RequireEnd();
  if (!status.locally_valid()) {
    throw CodecFailure("component lifecycle journal status failed closed validation");
  }
  return status;
}

std::string ComponentLifecycleStatusReleasePreconditionDigest(
    const ComponentLifecycleStatusDTO& status) {
  std::string material("manyforge.cclc-disable-release-precondition.v1\0",
                       sizeof("manyforge.cclc-disable-release-precondition.v1"));
  material += EncodeComponentLifecycleStatus(status);
  return content_identity::sha256_digest(material);
}

std::string ComponentLifecycleSemanticIntentDigest(const ComponentLifecycleIntentDTO& intent) {
  if (!intent.locally_valid()) {
    throw CodecFailure("component lifecycle intent is invalid at journal ingress");
  }
  Writer writer;
  writer.String(intent.close_incident_recovery ? kCloseIntentMagic
      : HasQualifiedApplyPredecessor(intent) ? kApplyQualifiedGenerationIntentMagic
      : intent.apply_generation_selection ? kApplyGenerationIntentMagic : kIntentMagic);
  PutIntent(writer, intent, false);
  if (intent.apply_generation_selection)
    PutApplyGenerationSelection(writer, *intent.apply_generation_selection);
  if (intent.program_node_evidence) {
    PutProgramNode(writer, *intent.program_node_evidence);
  }
  if (intent.close_incident_recovery)
    writer.String(intent.close_incident_recovery->expected_incident_id);
  return content_identity::sha256_digest(std::move(writer).Finish());
}

std::string SelectedComponentBringUpSemanticOfferDigest(
    const SelectedComponentBringUpOfferDTO& offer) {
  const auto checked_scope = SelectedComponentBringUpFenceScope(offer);
  if (!checked_scope.ok() || !offer.lifecycle_transaction_generation.nonzero() ||
      offer.lifecycle_transaction_id.value() !=
          "selected-lifecycle." + std::to_string(offer.lifecycle_transaction_generation.value()) ||
      !offer.candidate_projection_revision.valid() || !offer.fence_admission.presence_valid() ||
      offer.fence_admission.echoed_query.affected_scope != checked_scope.value() ||
      !IsExactLowercaseSha256Digest(offer.candidate_retained_bundle_digest.value()) ||
      offer.candidate_retained_bundle_bytes == 0U ||
      !IsExactLowercaseSha256Digest(offer.original_request_payload_digest.value()) ||
      offer.incident_id.empty() ||
      offer.incident_id.size() > kMaxComponentIncidentIdBytes ||
      offer.containment.stop_id.empty() || !offer.containment.stop_generation.nonzero()) {
    throw CodecFailure("selected component bring-up offer is invalid");
  }
  Writer writer;
  writer.String("MF-CCLC-SELECTED-BRING-UP-OFFER-V1");
  const auto identities = [&](const auto&... values) { (writer.Identity(values), ...); };
  const auto optional_identity = [&](const auto& value) {
    writer.Optional(value, [&](const auto& item) { writer.Identity(item); });
  };
  const auto projection = [&](const ProjectionRevisionDTO& revision) {
    identities(revision.retained_bundle_id, revision.graph_revision,
               revision.scene_revision.scene_id, revision.scene_revision.content_hash);
    writer.U64(revision.scene_revision.revision_id);
  };
  const auto provider = [&](const RuntimeCapabilityProviderPathDTO& path) {
    identities(path.capability_id, path.capability_definition_revision,
               path.capability_provider_binding_id, path.capability_provider_binding_revision,
               path.provider_instance_id, path.provider_manifest_id,
               path.provider_manifest_revision, path.provider_runtime_unit_id,
               path.provider_runtime_epoch);
  };
  PutPublicRequestContext(writer, offer.request_context);
  identities(offer.lifecycle_transaction_id, offer.predecessor.snapshot_digest);
  writer.Counter(offer.lifecycle_transaction_generation);
  writer.String(offer.incident_id);
  writer.Identity(offer.containment.stop_id);
  writer.Counter(offer.containment.stop_generation);
  writer.Vector(offer.containment.affected_operation_ids,
                [&](const auto& id) { writer.Identity(id); });
  optional_identity(offer.containment.program_run_id);
  projection(offer.candidate_projection_revision);
  writer.Identity(offer.candidate_retained_bundle_digest);
  writer.U64(offer.candidate_retained_bundle_bytes);
  // Full binding values are request identity, not a second binding authority.
  // Their independent retained-bundle qualification and sealing remain required.
  writer.Vector(offer.candidate_participants, [&](const RuntimeParticipantBindingDTO& binding) {
    identities(binding.program_id, binding.program_revision, binding.program_binding_id,
               binding.retained_bundle_id, binding.graph_revision, binding.component_role_id,
               binding.target_role_id, binding.controllable_component_id, binding.component_type_id,
               binding.component_type_revision, binding.command_target_id, binding.capability_id,
               binding.capability_definition_revision);
    writer.Optional(binding.command_schema, [&](const auto& schema) {
      identities(schema.command_schema_id, schema.command_schema_revision);
    });
    writer.Vector(binding.model_bindings, [&](const auto& model) {
      identities(model.model_role_id, model.model_instance_id, model.model_revision);
    });
    writer.Vector(binding.model_groups, [&](const auto& group) {
      identities(group.model_group_role_id, group.model_instance_id);
      writer.String(group.compiled_group_name);
      writer.Vector(group.expanded_compiled_joint_names,
                    [&](const auto& name) { writer.String(name); });
    });
    writer.Vector(binding.frame_bindings, [&](const auto& frame) {
      identities(frame.frame_role_id, frame.model_instance_id, frame.model_revision);
      writer.String(frame.compiled_frame_name);
      if (!frame.contact_body_role_ids.empty()) {
        writer.String("frame-contact-bodies.v1");
        writer.Vector(frame.contact_body_role_ids,
                      [&](const auto& role) { writer.Identity(role); });
      }
    });
    if (binding.motion_endpoint) {
      writer.String("motion-endpoint-binding.v1");
      const auto& endpoint = *binding.motion_endpoint;
      identities(endpoint.target_role_id, endpoint.command_target_id, endpoint.frame.frame_role_id,
                 endpoint.frame.model_instance_id, endpoint.frame.model_revision);
      writer.String(endpoint.frame.compiled_frame_name);
      writer.Vector(endpoint.frame.contact_body_role_ids,
                    [&](const auto& role) { writer.Identity(role); });
      writer.Vector(endpoint.contact_body_bindings, [&](const auto& body) {
        identities(body.component_role_id, body.target_role_id, body.controllable_component_id,
                   body.command_target_id);
        optional_identity(body.model_role_id);
        identities(body.collision_body_role_id, body.model_instance_id, body.model_revision);
        writer.Vector(body.collision_body_ids, [&](const auto& id) { writer.String(id); });
      });
    }
    writer.Vector(binding.resources, [&](const auto& resource) {
      identities(resource.resource_role_id, resource.resource_id);
      writer.Optional(resource.source_joint_name, [&](const auto& name) { writer.String(name); });
      writer.String(resource.compiled_resource_name);
      writer.EnumValue(resource.coordinate_kind);
    });
    writer.Vector(binding.collision_body_bindings, [&](const auto& body) {
      identities(body.collision_body_role_id, body.model_instance_id, body.model_revision);
      writer.Vector(body.compiled_collision_body_names,
                    [&](const auto& name) { writer.String(name); });
    });
    writer.Optional(binding.control_path, [&](const auto& path) {
      identities(path.control_domain_id, path.control_domain_revision, path.control_binding_id,
                 path.control_binding_revision, path.control_interface_contract_id,
                 path.control_interface_revision, path.controller_topology_id,
                 path.topology_revision, path.control_route_id, path.activation_set_id,
                 path.active_control_configuration_id);
      writer.Counter(path.active_control_configuration_generation);
      identities(path.provider_instance_id, path.provider_manifest_id,
                 path.provider_manifest_revision, path.provider_runtime_unit_id,
                 path.provider_runtime_epoch, path.state_authority_id);
      writer.EnumValue(path.state_authority_class);
      identities(path.stop_evidence_policy_id, path.stop_evidence_policy_revision);
      writer.EnumValue(path.containment_granularity);
      writer.Optional(path.runtime_scale_qualification, [&](const auto& scale) {
        identities(scale.control_route_id, scale.scaling_domain_id, scale.command_endpoint_role_id,
                   scale.applied_state_endpoint_role_id);
        optional_identity(scale.coordinated_barrier_id);
        writer.EnumValue(scale.acknowledgement_kind);
        writer.U64(std::bit_cast<std::uint64_t>(scale.maximum_slew_per_second));
        writer.I64(scale.freshness_budget_ns);
      });
    });
    writer.Optional(binding.capability_provider_path, provider);
    writer.Vector(binding.supporting_capability_provider_paths, provider);
    writer.Vector(binding.runtime_unit_epochs, [&](const auto& epoch) {
      identities(epoch.runtime_unit_id, epoch.runtime_epoch);
    });
    writer.Vector(binding.contact_grasp_bindings, [&](const auto& contact) {
      identities(contact.contact_policy_id, contact.contact_policy_revision,
                 contact.runtime_object_id);
      writer.Vector(contact.allowed_body_bindings, [&](const auto& body) {
        identities(body.component_role_id, body.target_role_id, body.controllable_component_id,
                   body.command_target_id);
        optional_identity(body.model_role_id);
        identities(body.collision_body_role_id, body.model_instance_id, body.model_revision);
        writer.Vector(body.collision_body_ids, [&](const auto& id) { writer.String(id); });
      });
      if (const auto* grasp = std::get_if<ContactGraspEnvelopeDTO>(&contact.envelope)) {
        writer.U64(std::bit_cast<std::uint64_t>(grasp->max_penetration_m));
        writer.U64(std::bit_cast<std::uint64_t>(grasp->terminal_window_start_fraction));
      } else {
        const auto& release = std::get<ContactReleaseEnvelopeDTO>(contact.envelope);
        writer.String("contact-release.v1");
        writer.Bool(release.release_evidence_mode == ContactReleaseEvidenceModeDTO::kBinaryContact);
        writer.Bool(release.enforce_max_start_penetration);
        writer.U64(std::bit_cast<std::uint64_t>(release.max_start_penetration_m));
        writer.U64(std::bit_cast<std::uint64_t>(release.min_release_progress_m));
        writer.U64(std::bit_cast<std::uint64_t>(release.release_tolerance_m));
        writer.Bool(release.require_strictly_decreasing_penetration);
        writer.Bool(release.require_separation_by_end);
        writer.Bool(release.allow_recontact);
      }
      if (contact.bodies_from_motion_endpoint) {
        writer.String("motion-endpoint-contact.v1");
      }
    });
  });
  if (!offer.candidate_provider_domains.empty()) {
    writer.String("provider-candidate-domains.v1");
    writer.Vector(offer.candidate_provider_domains,
                  [&](const auto& domain) { PutResolvedScope(writer, domain); });
  }
  const auto& admission = offer.fence_admission;
  writer.String(admission.echoed_query.manager_runtime_epoch.value());
  const auto& scope = checked_scope.value();
  writer.Vector(scope.controllable_component_ids, [&](const auto& id) { writer.Identity(id); });
  writer.Vector(scope.domains, [&](const auto& domain) { PutFenceDomain(writer, domain); });
  writer.Vector(scope.bindings, [&](const auto& binding) {
    writer.Identity(binding.control_binding_id);
    optional_identity(binding.predecessor_control_binding_revision);
    optional_identity(binding.candidate_control_binding_revision);
  });
  writer.Vector(scope.resources, [&](const auto& id) { writer.Identity(id); });
  identities(admission.execution_host_epoch.runtime_unit_id,
             admission.execution_host_epoch.runtime_epoch,
             admission.execution_host_epoch.request_owner_epoch,
             admission.execution_host_epoch.durable_stop_fence_ledger_digest);
  writer.Counter(admission.execution_host_epoch.publication_generation);
  projection(admission.active_projection_revision);
  writer.Identity(admission.active_public_pointer_digest);
  writer.U64(admission.operation_resource_generation);
  writer.Counter(admission.configuration_fence_generation);
  writer.Vector(admission.active_control_configurations, [&](const auto& configuration) {
    identities(configuration.control_domain_id, configuration.active_control_configuration_id);
    writer.Counter(configuration.active_control_configuration_generation);
  });
  writer.U64(admission.affected_queued_operation_count);
  writer.U64(admission.affected_active_operation_count);
  writer.Identity(admission.result_digest);
  writer.Identity(offer.original_request_payload_digest);
  return content_identity::sha256_digest(std::move(writer).Finish());
}

std::string EncodeLifecyclePrimitiveIntent(const BrandLifecyclePrimitiveIntentDTO& intent) {
  if (!intent.locally_valid()) {
    throw CodecFailure("lifecycle primitive intent is invalid at journal ingress");
  }
  Writer writer;
  writer.String(kPrimitiveIntentMagic);
  PutPrimitiveIntent(writer, intent);
  return std::move(writer).Finish();
}

BrandLifecyclePrimitiveIntentDTO DecodeLifecyclePrimitiveIntent(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kPrimitiveIntentMagic) {
    throw CodecFailure("lifecycle primitive intent magic is invalid");
  }
  auto intent = GetPrimitiveIntent(reader);
  reader.RequireEnd();
  if (!intent.locally_valid()) {
    throw CodecFailure("lifecycle primitive intent failed closed validation");
  }
  return intent;
}

std::string EncodeLifecyclePrimitiveResult(const BrandLifecyclePrimitiveResultDTO& result) {
  if (!result.locally_valid()) {
    throw CodecFailure("lifecycle primitive result is invalid at journal ingress");
  }
  Writer writer;
  writer.String(kPrimitiveResultMagic);
  PutPrimitiveResult(writer, result);
  return std::move(writer).Finish();
}

BrandLifecyclePrimitiveResultDTO DecodeLifecyclePrimitiveResult(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kPrimitiveResultMagic) {
    throw CodecFailure("lifecycle primitive result magic is invalid");
  }
  auto result = GetPrimitiveResult(reader);
  reader.RequireEnd();
  if (!result.locally_valid()) {
    throw CodecFailure("lifecycle primitive result failed closed validation");
  }
  return result;
}

std::string EncodeCCLCEffectAttempt(const CCLCEffectAttemptDTO& attempt) {
  if (!attempt.locally_valid()) {
    throw CodecFailure("CCLC effect attempt is invalid at journal ingress");
  }
  Writer writer;
  PutEffectAttempt(writer, attempt);
  return std::move(writer).Finish();
}

CCLCEffectAttemptDTO DecodeCCLCEffectAttempt(std::string_view bytes) {
  Reader reader(bytes);
  auto attempt = GetEffectAttempt(reader);
  reader.RequireEnd();
  if (!attempt.locally_valid()) {
    throw CodecFailure("CCLC effect attempt failed closed validation");
  }
  return attempt;
}

std::string EncodeRouteSelectionStatus(const RouteSelectionStatusDTO& status) {
  if (!status.locally_valid()) {
    throw CodecFailure("CCLC route status is invalid at journal ingress");
  }
  Writer writer;
  PutRouteStatus(writer, status);
  return std::move(writer).Finish();
}

RouteSelectionStatusDTO DecodeRouteSelectionStatus(std::string_view bytes) {
  Reader reader(bytes);
  auto status = GetRouteStatus(reader);
  reader.RequireEnd();
  if (!status.locally_valid()) {
    throw CodecFailure("CCLC route status failed closed validation");
  }
  return status;
}

std::string EncodeActiveControlConfiguration(const ActiveControlConfigurationDTO& configuration) {
  if (!configuration.locally_valid()) {
    throw CodecFailure("active control configuration is invalid at journal ingress");
  }
  Writer writer;
  writer.String(kActiveControlConfigurationMagic);
  PutActiveConfiguration(writer, configuration);
  return std::move(writer).Finish();
}

ActiveControlConfigurationDTO DecodeActiveControlConfiguration(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kActiveControlConfigurationMagic) {
    throw CodecFailure("active control configuration magic is invalid");
  }
  auto configuration = GetActiveConfiguration(reader);
  reader.RequireEnd();
  if (!configuration.locally_valid()) {
    throw CodecFailure("active control configuration failed closed validation");
  }
  return configuration;
}

std::string RouteSelectionStatusReleasePreconditionDigest(const RouteSelectionStatusDTO& status) {
  std::string material("manyforge.cclc-route-fence-release-precondition.v1\0",
                       sizeof("manyforge.cclc-route-fence-release-precondition.v1"));
  material += EncodeRouteSelectionStatus(status);
  return content_identity::sha256_digest(material);
}

std::string RouteSelectionSemanticIntentDigest(const RouteSelectionIntentDTO& intent) {
  if (!intent.locally_valid()) {
    throw CodecFailure("CCLC route intent is invalid at journal ingress");
  }
  Writer writer;
  writer.String(intent.expected_admission_generation ? kRouteAdmissionIntentMagic
                                                     : kRouteIntentMagic);
  PutRouteIntent(writer, intent, false);
  if (intent.expected_admission_generation) {
    writer.Counter(*intent.expected_admission_generation);
    writer.Optional<ProgramNodeAdmissionEvidenceDTO>(
        intent.program_node_evidence,
        [&](const auto& evidence) { writer.String(EncodeProgramNodeAdmissionEvidence(evidence)); });
  }
  return content_identity::sha256_digest(std::move(writer).Finish());
}

std::string EncodeCCLCRouteEffectAttempt(const CCLCRouteEffectAttemptDTO& attempt) {
  if (!attempt.locally_valid()) {
    throw CodecFailure("CCLC route effect attempt is invalid at journal ingress");
  }
  Writer writer;
  PutRouteEffectAttempt(writer, attempt);
  return std::move(writer).Finish();
}

CCLCRouteEffectAttemptDTO DecodeCCLCRouteEffectAttempt(std::string_view bytes) {
  Reader reader(bytes);
  auto attempt = GetRouteEffectAttempt(reader);
  reader.RequireEnd();
  if (!attempt.locally_valid()) {
    throw CodecFailure("CCLC route effect attempt failed closed validation");
  }
  return attempt;
}

std::string EncodeRouteFenceAdmissionSnapshotRecord(
    const RouteConfigurationFenceAdmissionSnapshotDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence admission snapshot is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceAdmissionSnapshotMagic);
  PutRouteFenceAdmissionSnapshot(writer, value);
  return std::move(writer).Finish();
}

RouteConfigurationFenceAdmissionSnapshotDTO DecodeRouteFenceAdmissionSnapshotRecord(
    std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kRouteFenceAdmissionSnapshotMagic) {
    throw CodecFailure("Kernel route-fence admission snapshot magic is invalid");
  }
  auto value = GetRouteFenceAdmissionSnapshot(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence admission snapshot failed closed validation");
  }
  return value;
}

std::string EncodeRouteFenceIntentRecord(const RouteConfigurationFenceIntentDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence intent is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceIntentMagic);
  PutRouteFenceIntent(writer, value);
  return std::move(writer).Finish();
}

RouteConfigurationFenceIntentDTO DecodeRouteFenceIntentRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kRouteFenceIntentMagic) {
    throw CodecFailure("Kernel route-fence intent magic is invalid");
  }
  auto value = GetRouteFenceIntent(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence intent failed closed validation");
  }
  return value;
}

std::string EncodeRouteFenceAcceptedRecord(const RouteConfigurationFenceAcceptedDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence acceptance is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceAcceptedMagic);
  PutRouteFenceAccepted(writer, value);
  return std::move(writer).Finish();
}

RouteConfigurationFenceAcceptedDTO DecodeRouteFenceAcceptedRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kRouteFenceAcceptedMagic) {
    throw CodecFailure("Kernel route-fence acceptance magic is invalid");
  }
  auto value = GetRouteFenceAccepted(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence acceptance failed closed validation");
  }
  return value;
}

std::string EncodeRouteFenceEvidenceRecord(
    const RouteConfigurationFenceReconciliationEvidenceDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence evidence is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceEvidenceMagic);
  PutRouteFenceEvidence(writer, value);
  return std::move(writer).Finish();
}

std::string EncodeRouteFenceStatusRecord(const RouteConfigurationFenceStatusDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence status is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceStatusMagic);
  PutRouteFenceStatus(writer, value);
  return std::move(writer).Finish();
}

std::string EncodeRouteFenceReleaseRequestRecord(
    const RouteConfigurationFenceReleaseRequestDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence release request is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceReleaseRequestMagic);
  PutRouteFenceReleaseRequest(writer, value);
  return std::move(writer).Finish();
}

RouteConfigurationFenceReleaseRequestDTO DecodeRouteFenceReleaseRequestRecord(
    std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kRouteFenceReleaseRequestMagic) {
    throw CodecFailure("Kernel route-fence release request magic is invalid");
  }
  auto value = GetRouteFenceReleaseRequest(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence release request failed closed validation");
  }
  return value;
}

std::string EncodeRouteFenceReleasedRecord(const RouteConfigurationFenceReleasedDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence release receipt is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kRouteFenceReleasedMagic);
  PutRouteFenceReleased(writer, value);
  return std::move(writer).Finish();
}

RouteConfigurationFenceReleasedDTO DecodeRouteFenceReleasedRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kRouteFenceReleasedMagic) {
    throw CodecFailure("Kernel route-fence release receipt magic is invalid");
  }
  auto value = GetRouteFenceReleased(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel route-fence release receipt failed closed validation");
  }
  return value;
}

std::string EncodeConfigurationFenceScopeRecord(const ConfigurationFenceScopeDTO& scope) {
  if (!scope.valid()) throw CodecFailure("configuration fence scope is invalid at codec ingress");
  Writer writer;
  writer.String("MF-CONFIGURATION-FENCE-SCOPE-V1");
  PutFenceScope(writer, scope);
  return std::move(writer).Finish();
}

ConfigurationFenceScopeDTO DecodeConfigurationFenceScopeRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-CONFIGURATION-FENCE-SCOPE-V1")
    throw CodecFailure("configuration fence scope magic is invalid");
  auto scope = GetFenceScope(reader);
  reader.RequireEnd();
  if (!scope.valid()) throw CodecFailure("configuration fence scope failed closed validation");
  return scope;
}

std::string EncodeExpectedControlDomainRevisionRecord(
    const ExpectedControlDomainRevisionDTO& domain) {
  if (!domain.locally_valid())
    throw CodecFailure("expected control domain is invalid at codec ingress");
  Writer writer;
  writer.String("MF-EXPECTED-CONTROL-DOMAIN-V1");
  PutExpectedDomain(writer, domain);
  return std::move(writer).Finish();
}

ExpectedControlDomainRevisionDTO DecodeExpectedControlDomainRevisionRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != "MF-EXPECTED-CONTROL-DOMAIN-V1")
    throw CodecFailure("expected control domain magic is invalid");
  auto domain = GetExpectedDomain(reader);
  reader.RequireEnd();
  if (!domain.locally_valid())
    throw CodecFailure("expected control domain failed closed validation");
  return domain;
}

}  // namespace manyforge::core::detail

namespace manyforge::core {

std::string EncodeComponentLifecycleIntent(const ComponentLifecycleIntentDTO& intent) {
  if (!intent.locally_valid())
    throw detail::CodecFailure("component lifecycle intent value is invalid");
  detail::Writer writer;
  writer.String(intent.close_incident_recovery ? detail::kCloseIntentValueMagic
      : detail::HasQualifiedApplyPredecessor(intent) ? detail::kApplyQualifiedGenerationIntentMagic
      : intent.apply_generation_selection ? detail::kApplyGenerationIntentMagic : detail::kIntentValueMagic);
  detail::PutIntent(writer, intent, true);
  writer.Optional<ProgramNodeAdmissionEvidenceDTO>(intent.program_node_evidence,
      [&](const auto& evidence) { detail::PutProgramNode(writer, evidence); });
  if (intent.apply_generation_selection)
    detail::PutApplyGenerationSelection(writer, *intent.apply_generation_selection);
  if (intent.close_incident_recovery)
    writer.String(intent.close_incident_recovery->expected_incident_id);
  return std::move(writer).Finish();
}

ComponentLifecycleIntentDTO DecodeComponentLifecycleIntent(std::string_view bytes) {
  detail::Reader reader(bytes);
  const auto magic = reader.String();
  if (magic != detail::kIntentValueMagic && magic != detail::kApplyGenerationIntentMagic &&
      magic != detail::kApplyQualifiedGenerationIntentMagic &&
      magic != detail::kCloseIntentValueMagic)
    throw detail::CodecFailure("component lifecycle intent value magic is invalid");
  auto intent = detail::GetIntent(reader);
  intent.program_node_evidence = reader.Optional<ProgramNodeAdmissionEvidenceDTO>(
      [&]() { return detail::GetProgramNode(reader); });
  if (magic == detail::kApplyGenerationIntentMagic || magic == detail::kApplyQualifiedGenerationIntentMagic)
    intent.apply_generation_selection = detail::GetApplyGenerationSelection(
        reader, magic == detail::kApplyQualifiedGenerationIntentMagic);
  if (magic == detail::kCloseIntentValueMagic)
    intent.close_incident_recovery = CloseComponentIncidentRecoveryDTO{reader.String()};
  reader.RequireEnd();
  if (!intent.locally_valid())
    throw detail::CodecFailure("component lifecycle intent value failed closed validation");
  return intent;
}

std::string EncodeSelectedComponentBringUpRequestResult(const CCLCLifecycleRequestResultDTO& result) {
  if (!result.locally_valid() || result.incident_recovery ||
      (result.status && !result.status->selected_bring_up))
    throw detail::CodecFailure("selected component bring-up request result is invalid");
  detail::Writer writer;
  writer.String(detail::kSelectedResultMagic);
  writer.EnumValue(result.replay_status);
  writer.Optional<ComponentLifecycleAcceptedDTO>(result.acceptance,
      [&](const auto& accepted) { detail::PutAccepted(writer, accepted); });
  writer.Optional<ComponentLifecycleStatusDTO>(result.status,
      [&](const auto& status) { detail::PutStatus(writer, status); });
  writer.Optional<ErrorDTO>(result.rejection,
      [&](const auto& error) { detail::PutError(writer, error); });
  return std::move(writer).Finish();
}

CCLCLifecycleRequestResultDTO DecodeSelectedComponentBringUpRequestResult(std::string_view bytes) {
  detail::Reader reader(bytes);
  if (reader.String() != detail::kSelectedResultMagic)
    throw detail::CodecFailure("selected component bring-up request result magic is invalid");
  CCLCLifecycleRequestResultDTO result{
      reader.EnumValue<RequestReplayStatusDTO>(),
      reader.Optional<ComponentLifecycleAcceptedDTO>([&]() { return detail::GetAccepted(reader); }),
      reader.Optional<ComponentLifecycleStatusDTO>([&]() { return detail::GetStatus(reader); }),
      reader.Optional<ErrorDTO>([&]() { return detail::GetError(reader); }), std::nullopt};
  reader.RequireEnd();
  if (!result.locally_valid() || (result.status && !result.status->selected_bring_up))
    throw detail::CodecFailure("selected component bring-up request result failed closed validation");
  return result;
}

ContentDigest SelectedComponentBringUpSemanticRequestDigest(
    const ComponentLifecycleIntentDTO& intent,
    const ProjectionRevisionDTO& candidate_projection_revision,
    const ContentDigest& candidate_retained_bundle_digest,
    std::uint64_t candidate_retained_bundle_bytes,
    const RuntimeUnitId& runtime_unit_id,
    const RuntimeEpoch& runtime_epoch,
    const RuntimeEpoch& manager_runtime_epoch) {
  if (!intent.locally_valid() || intent.intent != ComponentLifecycleIntentKindDTO::kEnable ||
      intent.program_node_evidence || !candidate_projection_revision.valid() ||
      !IsExactLowercaseSha256Digest(candidate_retained_bundle_digest.value()) ||
      candidate_retained_bundle_bytes == 0U || runtime_unit_id.empty() ||
      runtime_epoch.empty() || manager_runtime_epoch.empty())
    throw detail::CodecFailure("selected component bring-up semantic request is invalid");
  detail::Writer writer;
  writer.String("MF-CCLC-SELECTED-BRING-UP-REQUEST-V1");
  detail::PutIntent(writer, intent, false);
  detail::PutProjectionRevision(writer, candidate_projection_revision);
  writer.Identity(candidate_retained_bundle_digest);
  writer.U64(candidate_retained_bundle_bytes);
  writer.Identity(runtime_unit_id);
  writer.Identity(runtime_epoch);
  writer.Identity(manager_runtime_epoch);
  return ContentDigest(content_identity::sha256_digest(std::move(writer).Finish()));
}

std::string EncodeApplyDelegatedComponentLifecycleRequestResult(
    const CCLCLifecycleRequestResultDTO& result) {
  if (!result.locally_valid() || result.incident_recovery ||
      (result.status && !result.status->apply_bring_up))
    throw detail::CodecFailure("Apply delegated component lifecycle request result is invalid");
  detail::Writer writer;
  writer.String(detail::kApplyResultMagic);
  writer.EnumValue(result.replay_status);
  writer.Optional<ComponentLifecycleAcceptedDTO>(result.acceptance,
      [&](const auto& accepted) { detail::PutAccepted(writer, accepted); });
  writer.Optional<ComponentLifecycleStatusDTO>(result.status,
      [&](const auto& status) { detail::PutStatus(writer, status); });
  writer.Optional<ErrorDTO>(result.rejection,
      [&](const auto& error) { detail::PutError(writer, error); });
  return std::move(writer).Finish();
}

CCLCLifecycleRequestResultDTO DecodeApplyDelegatedComponentLifecycleRequestResult(
    std::string_view bytes) {
  detail::Reader reader(bytes);
  if (reader.String() != detail::kApplyResultMagic)
    throw detail::CodecFailure("Apply delegated component lifecycle request result magic is invalid");
  CCLCLifecycleRequestResultDTO result{
      reader.EnumValue<RequestReplayStatusDTO>(),
      reader.Optional<ComponentLifecycleAcceptedDTO>([&]() { return detail::GetAccepted(reader); }),
      reader.Optional<ComponentLifecycleStatusDTO>([&]() { return detail::GetStatus(reader); }),
      reader.Optional<ErrorDTO>([&]() { return detail::GetError(reader); }), std::nullopt};
  reader.RequireEnd();
  if (!result.locally_valid() || (result.status && !result.status->apply_bring_up))
    throw detail::CodecFailure("Apply delegated component lifecycle request result failed closed validation");
  return result;
}

std::string EncodeApplyComponentBringUpAuthority(const ApplyComponentBringUpAuthorityDTO& authority) {
  if (!authority.valid() || !ValidateCellAdmissionRecoveryReceiptQuery(
      {authority.parent_claim.intent, authority.parent_claim, std::nullopt}).ok())
    throw detail::CodecFailure("Apply component parent authority value is invalid");
  detail::Writer writer;
  writer.String("MF-CCLC-APPLY-DELEGATED-AUTHORITY-V1");
  detail::PutApplyAuthority(writer, authority);
  return std::move(writer).Finish();
}

ApplyComponentBringUpAuthorityDTO DecodeApplyComponentBringUpAuthority(std::string_view bytes) {
  detail::Reader reader(bytes);
  if (reader.String() != "MF-CCLC-APPLY-DELEGATED-AUTHORITY-V1")
    throw detail::CodecFailure("Apply component parent authority magic is invalid");
  auto authority = detail::GetApplyAuthority(reader);
  reader.RequireEnd();
  if (!authority.valid() || !ValidateCellAdmissionRecoveryReceiptQuery(
      {authority.parent_claim.intent, authority.parent_claim, std::nullopt}).ok())
    throw detail::CodecFailure("Apply component parent authority failed closed validation");
  return authority;
}

ContentDigest ApplyDelegatedComponentLifecycleSemanticRequestDigest(
    const ComponentLifecycleIntentDTO& intent,
    const ProjectionRevisionDTO& candidate_projection_revision,
    const ContentDigest& candidate_retained_bundle_digest,
    std::uint64_t candidate_retained_bundle_bytes,
    const RuntimeUnitId& runtime_unit_id,
    const RuntimeEpoch& runtime_epoch,
    const RuntimeEpoch& manager_runtime_epoch,
    const ApplyComponentBringUpReferenceDTO& parent) {
  if (!intent.locally_valid() ||
      (intent.intent != ComponentLifecycleIntentKindDTO::kPrepare &&
       intent.intent != ComponentLifecycleIntentKindDTO::kEnable &&
       intent.intent != ComponentLifecycleIntentKindDTO::kDisable) ||
      intent.program_node_evidence || !candidate_projection_revision.valid() ||
      !IsExactLowercaseSha256Digest(candidate_retained_bundle_digest.value()) ||
      candidate_retained_bundle_bytes == 0U || runtime_unit_id.empty() ||
      runtime_epoch.empty() || manager_runtime_epoch.empty() || !parent.valid() ||
      !IsExactLowercaseSha256Digest(parent.plan_digest.value()))
    throw detail::CodecFailure("Apply delegated component lifecycle semantic request is invalid");
  detail::Writer writer;
  writer.String(detail::HasQualifiedApplyPredecessor(intent) ? "MF-CCLC-APPLY-DELEGATED-REQUEST-V3"
      : intent.apply_generation_selection ? "MF-CCLC-APPLY-DELEGATED-REQUEST-V2"
                                        : "MF-CCLC-APPLY-DELEGATED-REQUEST-V1");
  detail::PutIntent(writer, intent, false);
  if (intent.apply_generation_selection)
    detail::PutApplyGenerationSelection(writer, *intent.apply_generation_selection);
  detail::PutProjectionRevision(writer, candidate_projection_revision);
  writer.Identity(candidate_retained_bundle_digest);
  writer.U64(candidate_retained_bundle_bytes);
  writer.Identity(runtime_unit_id);
  writer.Identity(runtime_epoch);
  writer.Identity(manager_runtime_epoch);
  detail::PutApplyReference(writer, parent);
  return ContentDigest(content_identity::sha256_digest(std::move(writer).Finish()));
}

}  // namespace manyforge::core

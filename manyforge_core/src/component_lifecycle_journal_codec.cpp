#include <algorithm>
#include <cstdint>
#include <limits>
#include <manyforge_content_identity/content_identity.hpp>
#include <manyforge_core/disable_handoff_record_codec_internal.hpp>
#include <manyforge_core/route_fence_record_codec_internal.hpp>
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
constexpr std::string_view kIntentMagic = "MF-CCLC-INTENT-V1";
constexpr std::string_view kPrimitiveIntentMagic = "MF-CCLC-PRIMITIVE-INTENT-V1";
constexpr std::string_view kPrimitiveResultMagic = "MF-CCLC-PRIMITIVE-RESULT-V1";
constexpr std::string_view kEffectAttemptMagic = "MF-CCLC-EFFECT-ATTEMPT-V1";
constexpr std::string_view kRouteStatusMagic = "MF-CCLC-ROUTE-STATUS-V2";
constexpr std::string_view kRouteIntentMagic = "MF-CCLC-ROUTE-INTENT-V1";
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
constexpr std::string_view kDisableHandoffAdmissionSnapshotMagic =
    "MF-KERNEL-DISABLE-HANDOFF-ADMISSION-SNAPSHOT-V1";
constexpr std::string_view kDisableHandoffFenceScopeMagic =
    "MF-KERNEL-DISABLE-HANDOFF-FENCE-SCOPE-V1";
constexpr std::string_view kDisableHandoffRequestMagic = "MF-KERNEL-DISABLE-HANDOFF-REQUEST-V1";
constexpr std::string_view kDisableHandoffAcceptedMagic = "MF-KERNEL-DISABLE-HANDOFF-ACCEPTED-V1";
constexpr std::string_view kDisableHandoffStatusMagic = "MF-KERNEL-DISABLE-HANDOFF-STATUS-V1";
constexpr std::string_view kDisableFenceReleaseRequestMagic =
    "MF-KERNEL-DISABLE-FENCE-RELEASE-REQUEST-V1";
constexpr std::string_view kDisableFenceReleasedMagic = "MF-KERNEL-DISABLE-FENCE-RELEASED-V1";
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

void PutExpectedDomain(Writer& writer, const ExpectedControlDomainRevisionDTO& value) {
  writer.Identity(value.control_domain_id);
  writer.Identity(value.control_domain_revision);
  writer.Identity(value.controller_topology_id);
  writer.Identity(value.topology_revision);
  PutExpectedActiveConfiguration(writer, value.expected_active_control_configuration);
  writer.Vector(value.expected_control_bindings,
                [&](const auto& item) { PutExpectedBinding(writer, item); });
}

[[nodiscard]] ExpectedControlDomainRevisionDTO GetExpectedDomain(Reader& reader) {
  return ExpectedControlDomainRevisionDTO{reader.Identity<ControlDomainId>(),
                                          reader.Identity<ControlDomainRevision>(),
                                          reader.Identity<ControllerTopologyId>(),
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
      GetAttribution(reader)};
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
  writer.Identity(value.controller_topology_id);
  writer.Identity(value.topology_revision);
  writer.Optional<ActiveControlConfigurationDTO>(
      value.active_control_configuration,
      [&](const auto& item) { PutActiveConfiguration(writer, item); });
  writer.Vector(value.current_control_bindings,
                [&](const auto& item) { PutExpectedBinding(writer, item); });
  writer.Vector(value.current_resources, [&](const auto& item) { writer.Identity(item); });
}

[[nodiscard]] ResolvedLifecycleControlDomainScopeDTO GetResolvedScope(Reader& reader) {
  return ResolvedLifecycleControlDomainScopeDTO{
      reader.Identity<ControlDomainId>(),
      reader.Identity<ControlDomainRevision>(),
      reader.Identity<ControllerTopologyId>(),
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
  writer.EnumValue(value.dispatch_state);
  writer.EnumValue(value.effect);
  writer.Optional<LifecyclePrimitiveKnownResultDTO>(
      value.known_result, [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
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
                                          reader.I64()};
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
      reader.EnumValue<LifecyclePrimitiveDispatchStateDTO>(),
      reader.EnumValue<LifecyclePrimitiveEffectDTO>(),
      reader.Optional<LifecyclePrimitiveKnownResultDTO>(
          [&]() { return reader.EnumValue<LifecyclePrimitiveKnownResultDTO>(); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); })};
}

void PutReconciliation(Writer& writer, const LifecycleReconciliationEvidenceDTO& value) {
  writer.Identity(value.provider_instance_id);
  writer.Identity(value.provider_manifest_id);
  writer.Identity(value.provider_manifest_revision);
  writer.Identity(value.provider_runtime_unit_id);
  writer.Identity(value.provider_runtime_epoch);
  writer.U64(value.source_sequence);
  writer.U64(value.observation_generation);
  writer.EnumValue(value.observed_state);
  writer.EnumValue(value.postcondition_result);
  writer.I64(value.observed_at_ns);
}

[[nodiscard]] LifecycleReconciliationEvidenceDTO GetReconciliation(Reader& reader) {
  return LifecycleReconciliationEvidenceDTO{reader.Identity<ProviderInstanceId>(),
                                            reader.Identity<ProviderManifestId>(),
                                            reader.Identity<ProviderManifestRevision>(),
                                            reader.Identity<RuntimeUnitId>(),
                                            reader.Identity<RuntimeEpoch>(),
                                            reader.U64(),
                                            reader.U64(),
                                            reader.EnumValue<ComponentLifecycleStateDTO>(),
                                            reader.EnumValue<LifecyclePostconditionResultDTO>(),
                                            reader.I64()};
}

void PutFenceScope(Writer& writer, const ConfigurationFenceScopeDTO& value) {
  writer.Vector(value.controllable_component_ids, [&](const auto& item) { writer.Identity(item); });
  writer.Vector(value.domains, [&](const auto& domain) {
    writer.Identity(domain.control_domain_id);
    writer.Optional<ControlDomainRevision>(domain.predecessor_control_domain_revision,
                                           [&](const auto& item) { writer.Identity(item); });
    writer.Optional<ControlDomainRevision>(domain.candidate_control_domain_revision,
                                           [&](const auto& item) { writer.Identity(item); });
  });
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
      reader.Vector<ConfigurationFenceDomainScopeDTO>([&]() {
        return ConfigurationFenceDomainScopeDTO{reader.Identity<ControlDomainId>(),
                                                reader.Optional<ControlDomainRevision>([&]() {
                                                  return reader.Identity<ControlDomainRevision>();
                                                }),
                                                reader.Optional<ControlDomainRevision>([&]() {
                                                  return reader.Identity<ControlDomainRevision>();
                                                })};
      }),
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
  return {reader.Identity<ControlDomainId>(),
          reader.Identity<ControlDomainRevision>(),
          reader.Identity<ControllerTopologyId>(),
          reader.Identity<TopologyRevision>(),
          reader.Identity<ProductId>(),
          reader.Identity<ProductContractId>(),
          reader.Identity<ProductContractRevision>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<RuntimeUnitId>(),
          reader.Identity<RuntimeEpoch>()};
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
  writer.String(kRouteStatusMagic);
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
}

[[nodiscard]] RouteSelectionStatusDTO GetRouteStatus(Reader& reader) {
  if (reader.String() != kRouteStatusMagic) {
    throw CodecFailure("CCLC route status magic is invalid");
  }
  return {GetRouteAccepted(reader),
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

void PutDisableHandoff(Writer& writer, const DisableHandoffStatusDTO& value);
[[nodiscard]] DisableHandoffStatusDTO GetDisableHandoff(Reader& reader);

void PutCurrentDomainActiveConfiguration(Writer& writer,
                                         const CurrentDomainActiveControlConfigurationDTO& value) {
  writer.Identity(value.control_domain_id);
  PutExpectedActiveConfiguration(writer, value.active_control_configuration);
}

[[nodiscard]] CurrentDomainActiveControlConfigurationDTO GetCurrentDomainActiveConfiguration(
    Reader& reader) {
  return {reader.Identity<ControlDomainId>(), GetExpectedActiveConfiguration(reader)};
}

void PutDisableHandoffComponentScopeExpectation(
    Writer& writer, const DisableHandoffComponentScopeExpectationDTO& value) {
  writer.Identity(value.component_snapshot_digest);
  writer.Counter(value.active_publication_generation);
  writer.Identity(value.active_public_pointer_digest);
  writer.Identity(value.retained_bundle_id);
  writer.Identity(value.graph_revision);
  writer.Counter(value.component_lifecycle_generation);
  PutFenceScope(writer, value.component_scope);
  writer.Vector(value.current_active_control_configurations, [&](const auto& configuration) {
    PutCurrentDomainActiveConfiguration(writer, configuration);
  });
}

[[nodiscard]] DisableHandoffComponentScopeExpectationDTO GetDisableHandoffComponentScopeExpectation(
    Reader& reader) {
  return {reader.Identity<ContentDigest>(),
          reader.Counter<ActiveBundlePublicationGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          reader.Counter<ComponentLifecycleGeneration>(),
          GetFenceScope(reader),
          reader.Vector<CurrentDomainActiveControlConfigurationDTO>(
              [&]() { return GetCurrentDomainActiveConfiguration(reader); })};
}

void PutDisableHandoffAdmissionQuery(Writer& writer, const DisableHandoffAdmissionQueryDTO& value) {
  writer.Identity(value.controllable_component_id);
  writer.EnumValue(value.request_kind);
  writer.Optional<DisableHandoffComponentScopeExpectationDTO>(
      value.component_expectation, [&](const auto& expectation) {
        PutDisableHandoffComponentScopeExpectation(writer, expectation);
      });
}

[[nodiscard]] DisableHandoffAdmissionQueryDTO GetDisableHandoffAdmissionQuery(Reader& reader) {
  return {reader.Identity<ControllableComponentId>(),
          reader.EnumValue<DisableHandoffScopeRequestKindDTO>(),
          reader.Optional<DisableHandoffComponentScopeExpectationDTO>(
              [&]() { return GetDisableHandoffComponentScopeExpectation(reader); })};
}

void PutDisableHandoffAdmissionSnapshot(Writer& writer,
                                        const DisableHandoffAdmissionSnapshotDTO& value) {
  PutDisableHandoffAdmissionQuery(writer, value.query);
  writer.Identity(value.request_owner_epoch);
  writer.EnumValue(value.scope_resolution);
  PutFenceScope(writer, value.current_scope);
  writer.Counter(value.active_publication_generation);
  writer.Identity(value.active_public_pointer_digest);
  writer.Identity(value.retained_bundle_id);
  writer.Identity(value.graph_revision);
  writer.U64(value.operation_resource_generation);
  writer.Counter(value.configuration_fence_generation);
  writer.Vector(value.current_active_control_configurations, [&](const auto& configuration) {
    PutCurrentDomainActiveConfiguration(writer, configuration);
  });
  writer.Identity(value.snapshot_digest);
}

[[nodiscard]] DisableHandoffAdmissionSnapshotDTO GetDisableHandoffAdmissionSnapshot(
    Reader& reader) {
  return {GetDisableHandoffAdmissionQuery(reader),
          reader.Identity<RequestOwnerEpoch>(),
          reader.EnumValue<DisableHandoffScopeResolutionDTO>(),
          GetFenceScope(reader),
          reader.Counter<ActiveBundlePublicationGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<RetainedBundleId>(),
          reader.Identity<GraphRevision>(),
          reader.U64(),
          reader.Counter<ConfigurationFenceGeneration>(),
          reader.Vector<CurrentDomainActiveControlConfigurationDTO>(
              [&]() { return GetCurrentDomainActiveConfiguration(reader); }),
          reader.Identity<ContentDigest>()};
}

void PutDisableHandoffRequest(Writer& writer, const DisableHandoffRequestDTO& value) {
  PutPublicRequestContext(writer, value.owner_request_context);
  writer.Identity(value.initiating_public_request_id);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  writer.EnumValue(value.scope_resolution);
  PutFenceScope(writer, value.affected_scope);
  writer.Identity(value.disable_admission_snapshot_digest);
  writer.Counter(value.expected_active_publication_generation);
  writer.Identity(value.expected_active_public_pointer_digest);
  writer.Identity(value.expected_graph_revision);
  writer.Identity(value.expected_retained_bundle_id);
  writer.Counter(value.expected_component_lifecycle_generation);
  writer.U64(value.expected_operation_resource_generation);
  writer.Counter(value.expected_fence_generation);
  writer.Vector(value.expected_active_control_configurations, [&](const auto& configuration) {
    PutCurrentDomainActiveConfiguration(writer, configuration);
  });
}

[[nodiscard]] DisableHandoffRequestDTO GetDisableHandoffRequest(Reader& reader) {
  return {GetPublicRequestContext(reader),
          reader.Identity<RequestId>(),
          reader.Identity<LifecycleTransactionId>(),
          reader.Counter<LifecycleTransactionGeneration>(),
          reader.Identity<ControllableComponentId>(),
          reader.EnumValue<DisableHandoffScopeResolutionDTO>(),
          GetFenceScope(reader),
          reader.Identity<ContentDigest>(),
          reader.Counter<ActiveBundlePublicationGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<GraphRevision>(),
          reader.Identity<RetainedBundleId>(),
          reader.Counter<ComponentLifecycleGeneration>(),
          reader.U64(),
          reader.Counter<ConfigurationFenceGeneration>(),
          reader.Vector<CurrentDomainActiveControlConfigurationDTO>(
              [&]() { return GetCurrentDomainActiveConfiguration(reader); })};
}

void PutDisableHandoffAccepted(Writer& writer, const DisableHandoffAcceptedDTO& value) {
  writer.Identity(value.request_id);
  writer.EnumValue(value.replay_status);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  writer.EnumValue(value.scope_resolution);
  writer.Identity(value.component_scope_digest);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  writer.Optional<StopId>(value.stop_id, [&](const auto& item) { writer.Identity(item); });
  writer.Optional<StopGeneration>(value.stop_generation,
                                  [&](const auto& item) { writer.Counter(item); });
}

[[nodiscard]] DisableHandoffAcceptedDTO GetDisableHandoffAccepted(Reader& reader) {
  return {reader.Identity<RequestId>(),
          reader.EnumValue<RequestReplayStatusDTO>(),
          reader.Identity<LifecycleTransactionId>(),
          reader.Counter<LifecycleTransactionGeneration>(),
          reader.Identity<ControllableComponentId>(),
          reader.EnumValue<DisableHandoffScopeResolutionDTO>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ConfigurationFenceId>(),
          reader.Counter<ConfigurationFenceGeneration>(),
          reader.Optional<StopId>([&]() { return reader.Identity<StopId>(); }),
          reader.Optional<StopGeneration>([&]() { return reader.Counter<StopGeneration>(); })};
}

void PutDisableHandoffEffectResult(Writer& writer, const DisableHandoffEffectResultDTO& value) {
  PutDisableHandoffRequest(writer, value.request);
  writer.EnumValue(value.disposition);
  writer.Optional<DisableHandoffAcceptedDTO>(value.acceptance, [&](const auto& acceptance) {
    PutDisableHandoffAccepted(writer, acceptance);
  });
}

[[nodiscard]] DisableHandoffEffectResultDTO GetDisableHandoffEffectResult(Reader& reader) {
  return {GetDisableHandoffRequest(reader), reader.EnumValue<DisableHandoffEffectDispositionDTO>(),
          reader.Optional<DisableHandoffAcceptedDTO>(
              [&]() { return GetDisableHandoffAccepted(reader); })};
}

void PutDisableFenceReleaseRequest(Writer& writer, const DisableFenceReleaseRequestDTO& value) {
  PutPublicRequestContext(writer, value.owner_request_context);
  writer.Identity(value.initiating_public_request_id);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  PutDisableHandoff(writer, value.ready_handoff);
  writer.U64(value.release_precondition_status_sequence);
  writer.Identity(value.release_precondition_status_digest);
  PutDisableHandoffComponentScopeExpectation(writer, value.current_component_expectation);
  PutDisableHandoffAdmissionSnapshot(writer, value.current_admission_snapshot);
}

[[nodiscard]] DisableFenceReleaseRequestDTO GetDisableFenceReleaseRequest(Reader& reader) {
  return {GetPublicRequestContext(reader),
          reader.Identity<RequestId>(),
          reader.Identity<LifecycleTransactionId>(),
          reader.Counter<LifecycleTransactionGeneration>(),
          reader.Identity<ControllableComponentId>(),
          GetDisableHandoff(reader),
          reader.U64(),
          reader.Identity<ContentDigest>(),
          GetDisableHandoffComponentScopeExpectation(reader),
          GetDisableHandoffAdmissionSnapshot(reader)};
}

void PutDisableFenceReleased(Writer& writer, const DisableFenceReleasedDTO& value) {
  PutDisableFenceReleaseRequest(writer, value.request);
  writer.EnumValue(value.replay_status);
  writer.I64(value.released_at_ns);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  writer.Identity(value.scope_digest);
  writer.Identity(value.receipt_digest);
}

[[nodiscard]] DisableFenceReleasedDTO GetDisableFenceReleased(Reader& reader) {
  return {GetDisableFenceReleaseRequest(reader),
          reader.EnumValue<RequestReplayStatusDTO>(),
          reader.I64(),
          reader.Identity<ConfigurationFenceId>(),
          reader.Counter<ConfigurationFenceGeneration>(),
          reader.Identity<ContentDigest>(),
          reader.Identity<ContentDigest>()};
}

void PutDisableFenceReleaseEffectResult(Writer& writer,
                                        const DisableFenceReleaseEffectResultDTO& value) {
  PutDisableFenceReleaseRequest(writer, value.request);
  writer.EnumValue(value.disposition);
  writer.Optional<DisableFenceReleasedDTO>(value.release_receipt, [&](const auto& receipt) {
    PutDisableFenceReleased(writer, receipt);
  });
}

[[nodiscard]] DisableFenceReleaseEffectResultDTO GetDisableFenceReleaseEffectResult(
    Reader& reader) {
  return {
      GetDisableFenceReleaseRequest(reader),
      reader.EnumValue<DisableFenceReleaseEffectDispositionDTO>(),
      reader.Optional<DisableFenceReleasedDTO>([&]() { return GetDisableFenceReleased(reader); })};
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
  writer.Optional<DisableHandoffRequestDTO>(value.disable_handoff_request, [&](const auto& item) {
    PutDisableHandoffRequest(writer, item);
  });
  writer.Optional<DisableHandoffEffectResultDTO>(
      value.disable_handoff_result,
      [&](const auto& item) { PutDisableHandoffEffectResult(writer, item); });
  writer.Optional<DisableFenceReleaseRequestDTO>(
      value.disable_fence_release_request,
      [&](const auto& item) { PutDisableFenceReleaseRequest(writer, item); });
  writer.Optional<DisableFenceReleaseEffectResultDTO>(
      value.disable_fence_release_result,
      [&](const auto& item) { PutDisableFenceReleaseEffectResult(writer, item); });
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
          [&]() { return GetPrimitiveResult(reader); }),
      reader.Optional<DisableHandoffRequestDTO>([&]() { return GetDisableHandoffRequest(reader); }),
      reader.Optional<DisableHandoffEffectResultDTO>(
          [&]() { return GetDisableHandoffEffectResult(reader); }),
      reader.Optional<DisableFenceReleaseRequestDTO>(
          [&]() { return GetDisableFenceReleaseRequest(reader); }),
      reader.Optional<DisableFenceReleaseEffectResultDTO>(
          [&]() { return GetDisableFenceReleaseEffectResult(reader); })};
}

void PutFenceHandle(Writer& writer, const ConfigurationFenceHandleDTO& value) {
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  PutFenceScope(writer, value.scope);
  writer.String(value.reason);
  writer.EnumValue(value.state);
}

[[nodiscard]] ConfigurationFenceHandleDTO GetFenceHandle(Reader& reader) {
  return ConfigurationFenceHandleDTO{reader.Identity<ConfigurationFenceId>(),
                                     reader.Counter<ConfigurationFenceGeneration>(),
                                     reader.Identity<LifecycleTransactionId>(),
                                     reader.Counter<LifecycleTransactionGeneration>(),
                                     reader.Identity<ControllableComponentId>(),
                                     GetFenceScope(reader),
                                     reader.String(),
                                     reader.EnumValue<ConfigurationFenceState>()};
}

void PutStopCertificateRef(Writer& writer, const StopCertificateRefDTO& value) {
  writer.Identity(value.stop_certificate_id);
  writer.Counter(value.stop_certificate_revision);
  writer.Identity(value.stop_id);
  writer.Counter(value.stop_generation);
}

[[nodiscard]] StopCertificateRefDTO GetStopCertificateRef(Reader& reader) {
  return StopCertificateRefDTO{reader.Identity<StopCertificateId>(),
                               reader.Counter<StopCertificateRevision>(), reader.Identity<StopId>(),
                               reader.Counter<StopGeneration>()};
}

void PutDisableHandoff(Writer& writer, const DisableHandoffStatusDTO& value) {
  writer.Identity(value.lifecycle_transaction_id);
  writer.Counter(value.lifecycle_transaction_generation);
  writer.Identity(value.controllable_component_id);
  writer.EnumValue(value.scope_resolution);
  writer.Identity(value.component_scope_digest);
  writer.Identity(value.configuration_fence_id);
  writer.Counter(value.configuration_fence_generation);
  writer.EnumValue(value.state);
  PutFenceScope(writer, value.scope);
  PutFenceHandle(writer, value.configuration_fence_handle);
  writer.Optional<StopCertificateRefDTO>(
      value.stop_certificate, [&](const auto& item) { PutStopCertificateRef(writer, item); });
  writer.Vector(value.cancelled_queued_operations,
                [&](const auto& item) { writer.Identity(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
}

[[nodiscard]] DisableHandoffStatusDTO GetDisableHandoff(Reader& reader) {
  return DisableHandoffStatusDTO{
      reader.Identity<LifecycleTransactionId>(),
      reader.Counter<LifecycleTransactionGeneration>(),
      reader.Identity<ControllableComponentId>(),
      reader.EnumValue<DisableHandoffScopeResolutionDTO>(),
      reader.Identity<ContentDigest>(),
      reader.Identity<ConfigurationFenceId>(),
      reader.Counter<ConfigurationFenceGeneration>(),
      reader.EnumValue<DisableHandoffStateDTO>(),
      GetFenceScope(reader),
      GetFenceHandle(reader),
      reader.Optional<StopCertificateRefDTO>([&]() { return GetStopCertificateRef(reader); }),
      reader.Vector<OperationId>([&]() { return reader.Identity<OperationId>(); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); })};
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
  writer.EnumValue(value.effect);
  writer.Vector(value.primitive_attempts,
                [&](const auto& item) { PutPrimitiveResult(writer, item); });
  writer.Optional<LifecycleReconciliationEvidenceDTO>(
      value.reconciliation_evidence, [&](const auto& item) { PutReconciliation(writer, item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
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
      reader.EnumValue<ComponentLifecycleDomainEffectDTO>(),
      reader.Vector<BrandLifecyclePrimitiveResultDTO>([&]() { return GetPrimitiveResult(reader); }),
      reader.Optional<LifecycleReconciliationEvidenceDTO>(
          [&]() { return GetReconciliation(reader); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); })};
}

void PutStatus(Writer& writer, const ComponentLifecycleStatusDTO& value) {
  writer.String(kStatusMagic);
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
  writer.Optional<ConfigurationFenceHandleDTO>(
      value.configuration_fence, [&](const auto& item) { PutFenceHandle(writer, item); });
  writer.Optional<DisableHandoffStatusDTO>(
      value.disable_handoff, [&](const auto& item) { PutDisableHandoff(writer, item); });
  writer.Optional<DisableFenceReleasedDTO>(value.disable_fence_released, [&](const auto& item) {
    PutDisableFenceReleased(writer, item);
  });
  writer.Bool(value.reconciliation_required);
  writer.Optional<ComponentLifecycleTerminalResultDTO>(
      value.terminal_result, [&](const auto item) { writer.EnumValue(item); });
  writer.Optional<ErrorDTO>(value.error, [&](const auto& item) { PutError(writer, item); });
  writer.I64(value.accepted_at_ns);
  writer.I64(value.latest_updated_at_ns);
  writer.I64(value.remaining_duration_ns);
}

[[nodiscard]] ComponentLifecycleStatusDTO GetStatus(Reader& reader) {
  if (reader.String() != kStatusMagic) {
    throw CodecFailure("component lifecycle journal status magic is invalid");
  }
  return ComponentLifecycleStatusDTO{
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
      reader.Optional<ConfigurationFenceHandleDTO>([&]() { return GetFenceHandle(reader); }),
      reader.Optional<DisableHandoffStatusDTO>([&]() { return GetDisableHandoff(reader); }),
      reader.Optional<DisableFenceReleasedDTO>([&]() { return GetDisableFenceReleased(reader); }),
      reader.Bool(),
      reader.Optional<ComponentLifecycleTerminalResultDTO>(
          [&]() { return reader.EnumValue<ComponentLifecycleTerminalResultDTO>(); }),
      reader.Optional<ErrorDTO>([&]() { return GetError(reader); }),
      reader.I64(),
      reader.I64(),
      reader.I64()};
}

}  // namespace

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
  writer.String(kIntentMagic);
  PutIntent(writer, intent, false);
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
  writer.String(kRouteIntentMagic);
  PutRouteIntent(writer, intent, false);
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

std::string EncodeDisableHandoffAdmissionSnapshotRecord(
    const DisableHandoffAdmissionSnapshotDTO& value) {
  if (!value.locally_valid() || !VerifyDisableHandoffAdmissionSnapshot(value).ok()) {
    throw CodecFailure("Kernel Disable admission snapshot is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableHandoffAdmissionSnapshotMagic);
  PutDisableHandoffAdmissionSnapshot(writer, value);
  return std::move(writer).Finish();
}

DisableHandoffAdmissionSnapshotDTO DecodeDisableHandoffAdmissionSnapshotRecord(
    std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableHandoffAdmissionSnapshotMagic) {
    throw CodecFailure("Kernel Disable admission snapshot magic is invalid");
  }
  auto value = GetDisableHandoffAdmissionSnapshot(reader);
  reader.RequireEnd();
  if (!value.locally_valid() || !VerifyDisableHandoffAdmissionSnapshot(value).ok()) {
    throw CodecFailure("Kernel Disable admission snapshot failed closed validation");
  }
  return value;
}

std::string EncodeDisableHandoffFenceScopeRecord(const DisableHandoffFenceScopeRecordDTO& value) {
  if (!value.scope.valid()) {
    throw CodecFailure("Kernel Disable fence scope is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableHandoffFenceScopeMagic);
  writer.EnumValue(value.scope_resolution);
  PutFenceScope(writer, value.scope);
  return std::move(writer).Finish();
}

DisableHandoffFenceScopeRecordDTO DecodeDisableHandoffFenceScopeRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableHandoffFenceScopeMagic) {
    throw CodecFailure("Kernel Disable fence-scope magic is invalid");
  }
  DisableHandoffFenceScopeRecordDTO value{reader.EnumValue<DisableHandoffScopeResolutionDTO>(),
                                          GetFenceScope(reader)};
  reader.RequireEnd();
  if (!value.scope.valid()) {
    throw CodecFailure("Kernel Disable fence scope failed closed validation");
  }
  return value;
}

std::string EncodeDisableHandoffRequestRecord(const DisableHandoffRequestDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff request is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableHandoffRequestMagic);
  PutDisableHandoffRequest(writer, value);
  return std::move(writer).Finish();
}

DisableHandoffRequestDTO DecodeDisableHandoffRequestRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableHandoffRequestMagic) {
    throw CodecFailure("Kernel Disable handoff request magic is invalid");
  }
  auto value = GetDisableHandoffRequest(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff request failed closed validation");
  }
  return value;
}

std::string EncodeDisableHandoffAcceptedRecord(const DisableHandoffAcceptedDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff acceptance is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableHandoffAcceptedMagic);
  PutDisableHandoffAccepted(writer, value);
  return std::move(writer).Finish();
}

DisableHandoffAcceptedDTO DecodeDisableHandoffAcceptedRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableHandoffAcceptedMagic) {
    throw CodecFailure("Kernel Disable handoff acceptance magic is invalid");
  }
  auto value = GetDisableHandoffAccepted(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff acceptance failed closed validation");
  }
  return value;
}

std::string EncodeDisableHandoffStatusRecord(const DisableHandoffStatusDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff status is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableHandoffStatusMagic);
  PutDisableHandoff(writer, value);
  return std::move(writer).Finish();
}

DisableHandoffStatusDTO DecodeDisableHandoffStatusRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableHandoffStatusMagic) {
    throw CodecFailure("Kernel Disable handoff status magic is invalid");
  }
  auto value = GetDisableHandoff(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable handoff status failed closed validation");
  }
  return value;
}

std::string EncodeDisableFenceReleaseRequestRecord(const DisableFenceReleaseRequestDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable-fence release request is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableFenceReleaseRequestMagic);
  PutDisableFenceReleaseRequest(writer, value);
  return std::move(writer).Finish();
}

DisableFenceReleaseRequestDTO DecodeDisableFenceReleaseRequestRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableFenceReleaseRequestMagic) {
    throw CodecFailure("Kernel Disable-fence release request magic is invalid");
  }
  auto value = GetDisableFenceReleaseRequest(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable-fence release request failed closed validation");
  }
  return value;
}

std::string EncodeDisableFenceReleasedRecord(const DisableFenceReleasedDTO& value) {
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable-fence release receipt is invalid at codec ingress");
  }
  Writer writer;
  writer.String(kDisableFenceReleasedMagic);
  PutDisableFenceReleased(writer, value);
  return std::move(writer).Finish();
}

DisableFenceReleasedDTO DecodeDisableFenceReleasedRecord(std::string_view bytes) {
  Reader reader(bytes);
  if (reader.String() != kDisableFenceReleasedMagic) {
    throw CodecFailure("Kernel Disable-fence release receipt magic is invalid");
  }
  auto value = GetDisableFenceReleased(reader);
  reader.RequireEnd();
  if (!value.locally_valid()) {
    throw CodecFailure("Kernel Disable-fence release receipt failed closed validation");
  }
  return value;
}

}  // namespace manyforge::core::detail

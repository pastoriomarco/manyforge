#include "selected_runtime_composition_rpc_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <manyforge_core/component_lifecycle_value_codec.hpp>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::component_manager::detail {
namespace {

namespace core = manyforge::core;

// Version 4 carries the per-component delta and receipts (H1a-IF.1).
constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'S',
                                              'R', 'C', '4', 0};
constexpr auto kProviderMagic = [] {
  auto value = kMagic;
  value[6] = '5';
  return value;
}();
constexpr std::size_t kMaximumCollectionEntries = 4096U;
constexpr std::uint8_t kRequestTag = 1U;
constexpr std::uint8_t kPreparingTag = 2U;
constexpr std::uint8_t kInstalledTag = 3U;
constexpr std::uint8_t kFailureTag = 4U;
constexpr std::uint8_t kSelectedSubmitTag = 5U;
constexpr std::uint8_t kSelectedReadTag = 6U;
constexpr std::uint8_t kSelectedResultTag = 7U;
constexpr std::uint8_t kApplySubmitTag = 8U;
constexpr std::uint8_t kApplyReadTag = 9U;
constexpr std::uint8_t kApplyResultTag = 10U;
constexpr std::uint8_t kInstalledInventoryRequestTag = 16U;
constexpr std::uint8_t kInstalledInventoryResultTag = 17U;
constexpr std::uint8_t kInspectionRequestTag = 11U;
constexpr std::uint8_t kInspectionResultTag = 12U;
constexpr std::uint8_t kProcessStopSubmitTag = 13U;
constexpr std::uint8_t kProcessStopReadTag = 14U;
constexpr std::uint8_t kProcessStopResultTag = 15U;
constexpr std::uint8_t kInspectionPlanningTail = 2U;
constexpr std::string_view kInspectionPlanningRequestMagic = "MFNSRC4-INSPECT-PLANNING-REQUEST-V1";
constexpr std::string_view kInspectionPlanningResultMagic = "MFNSRC4-INSPECT-PLANNING-RESULT-V2";

constexpr std::uint8_t kPneumaticNoneOwnersTail = 4U;
constexpr std::string_view kInstalledNoneOwnersMagic = "MFNSRC4-INSTALLED-PNEUMATIC-NONE-V1";
constexpr std::string_view kInspectionNoneOwnersMagic = "MFNSRC4-INSPECT-PNEUMATIC-NONE-V1";
constexpr std::uint8_t kInstalledLifecycleTail = 5U;
constexpr std::uint8_t kInstalledLifecycleRequestTail = 6U;
constexpr std::string_view kInstalledLifecycleMagic =
    "MFNSRC4-INSTALLED-LIFECYCLE-V1";

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("selected-runtime composition RPC codec: " +
                              detail);
}

class Writer final {
public:
  explicit Writer(const std::uint8_t tag, bool provider_format = false)
      : provider_format_(provider_format) {
    bytes_.reserve(1024U);
    for (const auto byte : provider_format_ ? kProviderMagic : kMagic) {
      Byte(byte);
    }
    Byte(tag);
  }

  [[nodiscard]] bool ProviderFormat() const noexcept {
    return provider_format_;
  }

  void Byte(const std::uint8_t value) {
    RequireSpace(1U);
    bytes_.push_back(static_cast<char>(value));
  }

  void Boolean(const bool value) { Byte(value ? 1U : 0U); }

  void U32(const std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
      Byte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  void U64(const std::uint64_t value) {
    for (int shift = 56; shift >= 0; shift -= 8) {
      Byte(static_cast<std::uint8_t>((value >> shift) & 0xffU));
    }
  }

  void String(const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      Fail("string exceeds the wire length domain");
    }
    U32(static_cast<std::uint32_t>(value.size()));
    RequireSpace(value.size());
    bytes_.append(value);
  }

  [[nodiscard]] std::string Finish() && { return std::move(bytes_); }

private:
  void RequireSpace(const std::size_t additional) const {
    if (additional >
        kMaximumSelectedRuntimeCompositionRpcPacketBytes - bytes_.size()) {
      Fail("encoded packet exceeds its byte bound");
    }
  }

  bool provider_format_;
  std::string bytes_;
};

class Reader final {
public:
  explicit Reader(const std::string_view bytes) : bytes_(bytes) {
    if (bytes_.size() <= kMagic.size() ||
        bytes_.size() > kMaximumSelectedRuntimeCompositionRpcPacketBytes) {
      Fail("packet is empty, incomplete, or oversized");
    }
    const auto magic = bytes_.substr(0U, kMagic.size());
    provider_format_ =
        magic ==
        std::string_view(reinterpret_cast<const char *>(kProviderMagic.data()),
                         kProviderMagic.size());
    if (!provider_format_ &&
        magic != std::string_view(reinterpret_cast<const char *>(kMagic.data()),
                                  kMagic.size()))
      Fail("packet magic or version differs");
    offset_ = kMagic.size();
  }

  [[nodiscard]] bool ProviderFormat() const noexcept {
    return provider_format_;
  }

  [[nodiscard]] std::uint8_t Byte() {
    Require(1U);
    return static_cast<std::uint8_t>(bytes_[offset_++]);
  }

  [[nodiscard]] bool Boolean() {
    const auto value = Byte();
    if (value > 1U) {
      Fail("boolean field is noncanonical");
    }
    return value == 1U;
  }

  [[nodiscard]] std::uint32_t U32() {
    std::uint32_t value = 0U;
    for (std::size_t index = 0U; index < 4U; ++index) {
      value = static_cast<std::uint32_t>((value << 8U) | Byte());
    }
    return value;
  }

  [[nodiscard]] std::uint64_t U64() {
    std::uint64_t value = 0U;
    for (std::size_t index = 0U; index < 8U; ++index) {
      value = (value << 8U) | Byte();
    }
    return value;
  }

  [[nodiscard]] std::string String() {
    const auto size = static_cast<std::size_t>(U32());
    Require(size);
    std::string result(bytes_.substr(offset_, size));
    offset_ += size;
    return result;
  }

  [[nodiscard]] bool AtEnd() const noexcept { return offset_ == bytes_.size(); }

  [[nodiscard]] std::uint8_t Peek() const {
    Require(1U);
    return static_cast<std::uint8_t>(bytes_[offset_]);
  }

  void Finish(bool provider_scope = false) const {
    if (provider_scope != provider_format_)
      Fail("packet version differs from scope kind");
    if (offset_ != bytes_.size()) {
      Fail("packet has trailing fields");
    }
  }

private:
  void Require(const std::size_t size) const {
    if (size > bytes_.size() - offset_) {
      Fail("packet ended inside a field");
    }
  }

  bool provider_format_{};
  std::string_view bytes_;
  std::size_t offset_{};
};

template <typename Strong>
void WriteStrong(Writer &writer, const Strong &value) {
  writer.String(value.value());
}

template <typename Strong> [[nodiscard]] Strong ReadStrong(Reader &reader) {
  return Strong(reader.String());
}

template <typename Generation>
void WriteGeneration(Writer &writer, const Generation value) {
  writer.U64(value.value());
}

template <typename Generation>
[[nodiscard]] Generation ReadGeneration(Reader &reader) {
  return Generation(reader.U64());
}

template <typename Strong>
void WriteOptionalStrong(Writer &writer, const std::optional<Strong> &value) {
  writer.Boolean(value.has_value());
  if (value.has_value()) {
    WriteStrong(writer, *value);
  }
}

template <typename Strong>
[[nodiscard]] std::optional<Strong> ReadOptionalStrong(Reader &reader) {
  if (!reader.Boolean()) {
    return std::nullopt;
  }
  return ReadStrong<Strong>(reader);
}

template <typename Value, typename Encode>
void WriteVector(Writer &writer, const std::vector<Value> &values,
                 Encode encode) {
  if (values.size() > kMaximumCollectionEntries) {
    Fail("collection exceeds its entry bound");
  }
  writer.U32(static_cast<std::uint32_t>(values.size()));
  for (const auto &value : values) {
    encode(writer, value);
  }
}

template <typename Value, typename Decode>
[[nodiscard]] std::vector<Value> ReadVector(Reader &reader, Decode decode) {
  const auto count = static_cast<std::size_t>(reader.U32());
  if (count > kMaximumCollectionEntries) {
    Fail("collection exceeds its entry bound");
  }
  std::vector<Value> values;
  values.reserve(count);
  for (std::size_t index = 0U; index < count; ++index) {
    values.push_back(decode(reader));
  }
  return values;
}

void WriteProjectionRevision(Writer &writer,
                             const core::ProjectionRevisionDTO &revision) {
  WriteStrong(writer, revision.retained_bundle_id);
  WriteStrong(writer, revision.graph_revision);
  WriteStrong(writer, revision.scene_revision.scene_id);
  writer.U64(revision.scene_revision.revision_id);
  WriteStrong(writer, revision.scene_revision.content_hash);
}

[[nodiscard]] core::ProjectionRevisionDTO
ReadProjectionRevision(Reader &reader) {
  return {ReadStrong<core::RetainedBundleId>(reader),
          ReadStrong<core::GraphRevision>(reader),
          {ReadStrong<core::SceneId>(reader), reader.U64(),
           ReadStrong<core::ContentDigest>(reader)}};
}

[[nodiscard]] bool
UsesProviderDomains(const core::ConfigurationFenceScopeDTO &scope) {
  return std::any_of(
      scope.domains.begin(), scope.domains.end(), [](const auto &domain) {
        return std::holds_alternative<
            core::ProviderConfigurationFenceDomainDTO>(domain.payload);
      });
}

void WriteScope(Writer &writer, const core::ConfigurationFenceScopeDTO &scope) {
  if (writer.ProviderFormat()) {
    writer.String(core::detail::EncodeConfigurationFenceScopeRecord(scope));
    return;
  }
  if (UsesProviderDomains(scope))
    Fail("provider scope requires the provider packet version");
  WriteVector(
      writer, scope.controllable_component_ids,
      [](Writer &output, const auto &value) { WriteStrong(output, value); });
  WriteVector(writer, scope.domains, [](Writer &output, const auto &value) {
    WriteStrong(output, value.control_domain_id);
    WriteOptionalStrong(output, value.predecessor_control_domain_revision);
    WriteOptionalStrong(output, value.candidate_control_domain_revision);
  });
  WriteVector(writer, scope.bindings, [](Writer &output, const auto &value) {
    WriteStrong(output, value.control_binding_id);
    WriteOptionalStrong(output, value.predecessor_control_binding_revision);
    WriteOptionalStrong(output, value.candidate_control_binding_revision);
  });
  WriteVector(writer, scope.resources, [](Writer &output, const auto &value) {
    WriteStrong(output, value);
  });
}

[[nodiscard]] core::ConfigurationFenceScopeDTO ReadScope(Reader &reader) {
  if (reader.ProviderFormat())
    return core::detail::DecodeConfigurationFenceScopeRecord(reader.String());
  return {
      ReadVector<core::ControllableComponentId>(
          reader,
          [](Reader &input) {
            return ReadStrong<core::ControllableComponentId>(input);
          }),
      ReadVector<core::ConfigurationFenceDomainScopeDTO>(
          reader,
          [](Reader &input) {
            return core::ConfigurationFenceDomainScopeDTO{
                ReadStrong<core::ControlDomainId>(input),
                ReadOptionalStrong<core::ControlDomainRevision>(input),
                ReadOptionalStrong<core::ControlDomainRevision>(input)};
          }),
      ReadVector<core::ConfigurationFenceBindingScopeDTO>(
          reader,
          [](Reader &input) {
            return core::ConfigurationFenceBindingScopeDTO{
                ReadStrong<core::ControlBindingId>(input),
                ReadOptionalStrong<core::ControlBindingRevision>(input),
                ReadOptionalStrong<core::ControlBindingRevision>(input)};
          }),
      ReadVector<core::ResourceId>(
          reader,
          [](Reader &input) { return ReadStrong<core::ResourceId>(input); }),
  };
}

void WriteRequest(Writer &writer,
                  const ManagerSelectedRuntimeCompositionRequestDTO &request) {
  WriteStrong(writer, request.authoritative_scene_snapshot_digest);
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteProjectionRevision(writer, request.candidate_projection_revision);
  WriteGeneration(writer, request.expected_predecessor_publication_generation);
  WriteStrong(writer, request.expected_predecessor_public_pointer_digest);
  WriteStrong(writer, request.retained_bundle_canonical_content_digest);
  writer.U64(request.retained_bundle_canonical_byte_size);
  WriteStrong(writer, request.runtime_unit_id);
  WriteStrong(writer, request.runtime_epoch);
  WriteStrong(writer, request.manager_runtime_epoch);
  WriteStrong(writer, request.configuration_fence_id);
  WriteGeneration(writer, request.configuration_fence_generation);
  WriteScope(writer, request.configuration_fence_scope);
  writer.Byte(static_cast<std::uint8_t>(request.phase));
  writer.String(
      manyforge::deployment::EncodeApplyComponentDelta(request.component_delta));
  WriteProjectionRevision(writer, request.predecessor_projection_revision);
}

[[nodiscard]] ManagerSelectedRuntimeCompositionRequestDTO
ReadRequest(Reader &reader) {
  return {
      ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadProjectionRevision(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::ContentDigest>(reader),
      reader.U64(),
      ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadScope(reader),
      static_cast<ManagerSelectedRuntimeCompositionPhase>(reader.Byte()),
      manyforge::deployment::DecodeApplyComponentDelta(reader.String()),
      ReadProjectionRevision(reader),
  };
}

template <typename Request>
void WriteSelectedRequest(Writer &writer, const Request &request) {
  writer.String(core::EncodeComponentLifecycleIntent(request.intent));
  WriteProjectionRevision(writer, request.candidate_projection_revision);
  WriteStrong(writer, request.candidate_retained_bundle_digest);
  writer.U64(request.candidate_retained_bundle_bytes);
  WriteStrong(writer, request.runtime_unit_id);
  WriteStrong(writer, request.runtime_epoch);
  WriteStrong(writer, request.manager_runtime_epoch);
}

[[nodiscard]] ManagerSelectedComponentBringUpRequestDTO
ReadSelectedRequest(Reader &reader) {
  return {core::DecodeComponentLifecycleIntent(reader.String()),
          ReadProjectionRevision(reader),
          ReadStrong<core::ContentDigest>(reader),
          reader.U64(),
          ReadStrong<core::RuntimeUnitId>(reader),
          ReadStrong<core::RuntimeEpoch>(reader),
          ReadStrong<core::RuntimeEpoch>(reader)};
}

void WriteApplyRequest(Writer &writer,
                       const ManagerApplyComponentBringUpRequestDTO &request) {
  WriteSelectedRequest(writer, request);
  writer.String(core::EncodeApplyComponentBringUpAuthority(request.parent_authority));
}

[[nodiscard]] ManagerApplyComponentBringUpRequestDTO ReadApplyRequest(Reader &reader) {
  auto common = ReadSelectedRequest(reader);
  return {std::move(common.intent), std::move(common.candidate_projection_revision),
          std::move(common.candidate_retained_bundle_digest), common.candidate_retained_bundle_bytes,
          std::move(common.runtime_unit_id), std::move(common.runtime_epoch),
          std::move(common.manager_runtime_epoch),
          core::DecodeApplyComponentBringUpAuthority(reader.String())};
}

void WriteInstalledInventoryRequest(
    Writer &writer, const ManagerComponentProcessInventoryRequestDTO &request) {
  WriteProjectionRevision(writer, request.predecessor_projection_revision);
  WriteGeneration(writer, request.expected_predecessor_publication_generation);
  WriteStrong(writer, request.expected_predecessor_public_pointer_digest);
  WriteStrong(writer, request.runtime_unit_id);
  WriteStrong(writer, request.runtime_epoch);
  WriteStrong(writer, request.manager_runtime_epoch);
  if (request.include_component_lifecycles) {
    writer.Byte(kInstalledLifecycleRequestTail);
    writer.String(kInstalledLifecycleMagic);
  }
}
ManagerComponentProcessInventoryRequestDTO
ReadInstalledInventoryRequest(Reader &reader) {
  ManagerComponentProcessInventoryRequestDTO result{
      ReadProjectionRevision(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!reader.AtEnd() && reader.Peek() == kInstalledLifecycleRequestTail) {
    static_cast<void>(reader.Byte());
    if (reader.String() != kInstalledLifecycleMagic)
      Fail("installed lifecycle request extension is invalid");
    result.include_component_lifecycles = true;
  }
  return result;
}
void WriteInstalledOccurrence(Writer &writer,
                              const ManagerComponentProcessOccurrenceDTO &row) {
  WriteStrong(writer, row.controllable_component_id);
  writer.String(row.launch_identity);
  writer.U32(static_cast<std::uint32_t>(row.leader_pid));
  writer.Byte(static_cast<std::uint8_t>(row.process_presence));
  writer.Byte(static_cast<std::uint8_t>(row.cleanup_state));
  writer.Boolean(row.receipt.has_value());
  if (row.receipt) {
    writer.U32(static_cast<std::uint32_t>(row.receipt->leader_pid));
    writer.String(row.receipt->controllable_component_id);
    writer.String(row.receipt->launch_identity);
    writer.String(row.receipt->observed_utc);
  }
  WriteOptionalStrong(writer, row.installed_runtime_identity);
}
ManagerComponentProcessOccurrenceDTO ReadInstalledOccurrence(Reader &reader) {
  auto component = ReadStrong<core::ControllableComponentId>(reader);
  auto launch = reader.String();
  const auto pid = reader.U32();
  if (pid >
      static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
    Fail("installed occurrence PID exceeds signed value domain");
  ManagerComponentProcessOccurrenceDTO row{
      std::move(component), std::move(launch), static_cast<std::int32_t>(pid),
      static_cast<ProcessPresenceState>(reader.Byte()),
      static_cast<core::ControllerLaunchCleanupState>(reader.Byte())};
  if (reader.Boolean()) {
    const auto leader = reader.U32();
    if (leader > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
      Fail("installed drain receipt PID exceeds signed value domain");
    row.receipt = core::ControllerLaunchGroupDrainReceipt{
        static_cast<int>(leader), reader.String(), reader.String(),
        reader.String()};
  }
  row.installed_runtime_identity =
      ReadOptionalStrong<core::ContentDigest>(reader);
  return row;
}

void WriteProcessStopRequest(Writer &writer, const ManagerComponentProcessStopRequestDTO &request) {
  WriteStrong(writer, request.request_context.request_id);
  WriteStrong(writer, request.request_context.request_owner_epoch);
  writer.String(request.request_context.source_label);
  writer.Boolean(request.request_context.correlation_id.has_value());
  if (request.request_context.correlation_id) writer.String(*request.request_context.correlation_id);
  WriteProjectionRevision(writer, request.predecessor_projection_revision);
  WriteGeneration(writer, request.expected_predecessor_publication_generation);
  WriteStrong(writer, request.expected_predecessor_public_pointer_digest);
  WriteStrong(writer, request.runtime_unit_id);
  WriteStrong(writer, request.runtime_epoch);
  WriteStrong(writer, request.manager_runtime_epoch);
  WriteStrong(writer, request.controllable_component_id);
  writer.String(request.launch_identity);
  writer.U32(static_cast<std::uint32_t>(request.leader_pid));
}

ManagerComponentProcessStopRequestDTO ReadProcessStopRequest(Reader &reader) {
  core::PublicRequestContextDTO context{ReadStrong<core::RequestId>(reader),
      ReadStrong<core::RequestOwnerEpoch>(reader), reader.String(), std::nullopt};
  if (reader.Boolean()) context.correlation_id = reader.String();
  ManagerComponentProcessStopRequestDTO request{std::move(context),
      ReadProjectionRevision(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadStrong<core::ContentDigest>(reader), ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader), ReadStrong<core::RuntimeEpoch>(reader),
      ReadStrong<core::ControllableComponentId>(reader), reader.String(), 0};
  const auto pid = reader.U32();
  if (pid > static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max()))
    Fail("process Stop PID exceeds signed value domain");
  request.leader_pid = static_cast<std::int32_t>(pid);
  return request;
}

void WriteInspectionRequest(
    Writer &writer,
    const ManagerComponentBringUpInspectionRequestDTO &request) {
  WriteProjectionRevision(writer, request.candidate_projection_revision);
  WriteStrong(writer, request.candidate_retained_bundle_digest);
  writer.U64(request.candidate_retained_bundle_bytes);
  WriteProjectionRevision(writer, request.predecessor_projection_revision);
  WriteGeneration(writer, request.expected_predecessor_publication_generation);
  WriteStrong(writer, request.expected_predecessor_public_pointer_digest);
  WriteStrong(writer, request.runtime_unit_id);
  WriteStrong(writer, request.runtime_epoch);
  WriteStrong(writer, request.manager_runtime_epoch);
}

[[nodiscard]] ManagerComponentBringUpInspectionRequestDTO
ReadInspectionRequest(Reader &reader) {
  return {ReadProjectionRevision(reader),
          ReadStrong<core::ContentDigest>(reader),
          reader.U64(),
          ReadProjectionRevision(reader),
          ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
          ReadStrong<core::ContentDigest>(reader),
          ReadStrong<core::RuntimeUnitId>(reader),
          ReadStrong<core::RuntimeEpoch>(reader),
          ReadStrong<core::RuntimeEpoch>(reader)};
}

void WriteInspectionPlanningRequestTail(Writer &writer,
    const ManagerComponentBringUpInspectionRequestDTO &request) {
  if (!request.planning_component_id) return;
  writer.Byte(kInspectionPlanningTail);
  writer.String(kInspectionPlanningRequestMagic);
  WriteStrong(writer, *request.planning_component_id);
}

void ReadInspectionPlanningRequestTail(Reader &reader,
    ManagerComponentBringUpInspectionRequestDTO &request) {
  if (reader.AtEnd()) return;
  if (reader.Byte() != kInspectionPlanningTail || reader.String() != kInspectionPlanningRequestMagic)
    Fail("component planning request extension is not a closed supported value");
  request.planning_component_id = ReadStrong<core::ControllableComponentId>(reader);
}

void WritePlanningDomain(Writer &writer, const core::ExpectedControlDomainRevisionDTO &domain) {
  if (writer.ProviderFormat()) {
    writer.String(
        core::detail::EncodeExpectedControlDomainRevisionRecord(domain));
    return;
  }
  if (domain.provider())
    Fail("provider planning domain requires the provider packet version");
  WriteStrong(writer, domain.control_domain_id);
  WriteStrong(writer, domain.control_domain_revision);
  WriteStrong(writer, domain.controller().controller_topology_id);
  WriteStrong(writer, domain.controller().topology_revision);
  const auto &configuration =
      domain.controller().expected_active_control_configuration;
  writer.Byte(static_cast<std::uint8_t>(configuration.presence));
  WriteOptionalStrong(writer, configuration.active_control_configuration_id);
  writer.Boolean(configuration.active_control_configuration_generation.has_value());
  if (configuration.active_control_configuration_generation)
    WriteGeneration(writer, *configuration.active_control_configuration_generation);
  WriteVector(writer, domain.controller().expected_control_bindings,
              [](Writer &output, const auto &binding) {
                WriteStrong(output, binding.control_binding_id);
                WriteStrong(output, binding.control_binding_revision);
                WriteStrong(output, binding.control_route_id);
              });
}

[[nodiscard]] core::ExpectedControlDomainRevisionDTO ReadPlanningDomain(Reader &reader) {
  if (reader.ProviderFormat())
    return core::detail::DecodeExpectedControlDomainRevisionRecord(
        reader.String());
  auto domain = ReadStrong<core::ControlDomainId>(reader);
  auto revision = ReadStrong<core::ControlDomainRevision>(reader);
  auto topology = ReadStrong<core::ControllerTopologyId>(reader);
  auto topology_revision = ReadStrong<core::TopologyRevision>(reader);
  const auto presence = reader.Byte();
  if (presence > static_cast<std::uint8_t>(core::ExpectedActiveControlConfigurationPresenceDTO::kPresent))
    Fail("component planning active-configuration presence is unknown");
  auto configuration_id = ReadOptionalStrong<core::ActiveControlConfigurationId>(reader);
  std::optional<core::ActiveControlConfigurationGeneration> configuration_generation;
  if (reader.Boolean()) configuration_generation = ReadGeneration<core::ActiveControlConfigurationGeneration>(reader);
  const core::ExpectedActiveControlConfigurationDTO configuration{
      static_cast<core::ExpectedActiveControlConfigurationPresenceDTO>(presence),
      std::move(configuration_id), configuration_generation};
  auto bindings = ReadVector<core::ExpectedControlBindingRevisionDTO>(reader, [](Reader &input) {
    return core::ExpectedControlBindingRevisionDTO{ReadStrong<core::ControlBindingId>(input),
        ReadStrong<core::ControlBindingRevision>(input), ReadStrong<core::ControlRouteId>(input)};
  });
  return {std::move(domain), std::move(revision), std::move(topology), std::move(topology_revision),
          configuration, std::move(bindings)};
}

[[nodiscard]] bool
UsesProviderPlanning(const ManagerComponentBringUpInspectionResultDTO &result) {
  return result.planning_facts &&
         std::any_of(
             result.planning_facts->candidate_expected_domains.begin(),
             result.planning_facts->candidate_expected_domains.end(),
             [](const auto &domain) { return domain.provider() != nullptr; });
}

void WriteInspectionPlanningResultTail(Writer &writer,
    const ManagerComponentBringUpInspectionResultDTO &result) {
  if (!result.planning_facts) return;
  writer.Byte(kInspectionPlanningTail);
  writer.String(kInspectionPlanningResultMagic);
  WriteStrong(writer, *result.request.planning_component_id);
  const auto &facts = *result.planning_facts;
  WriteStrong(writer, facts.component_id);
  WriteStrong(writer, facts.owner_epoch);
  const auto &selection = facts.generation_selection;
  writer.Boolean(selection.predecessor_generation.has_value());
  if (selection.predecessor_generation) WriteGeneration(writer, *selection.predecessor_generation);
  WriteGeneration(writer, selection.candidate_generation);
  writer.U64(selection.prior_reservation_generation);
  writer.Boolean(selection.owner_qualified_predecessor.has_value());
  if (selection.owner_qualified_predecessor) {
    WriteGeneration(writer, selection.owner_qualified_predecessor->generation);
    WriteStrong(writer, selection.owner_qualified_predecessor->snapshot_digest);
  }
  WriteVector(writer, facts.candidate_expected_domains, WritePlanningDomain);
  writer.Byte(static_cast<std::uint8_t>(facts.requested_target));
  writer.Byte(static_cast<std::uint8_t>(facts.next_intent));
}

void ReadInspectionPlanningResultTail(Reader &reader,
    ManagerComponentBringUpInspectionResultDTO &result) {
  if (reader.AtEnd() || reader.Peek() == kPneumaticNoneOwnersTail) return;
  if (reader.Byte() != kInspectionPlanningTail || reader.String() != kInspectionPlanningResultMagic)
    Fail("component planning result extension is not a closed supported value");
  result.request.planning_component_id = ReadStrong<core::ControllableComponentId>(reader);
  auto component = ReadStrong<core::ControllableComponentId>(reader);
  auto owner = ReadStrong<core::RequestOwnerEpoch>(reader);
  std::optional<core::ComponentLifecycleGeneration> watermark;
  if (reader.Boolean()) watermark = ReadGeneration<core::ComponentLifecycleGeneration>(reader);
  core::ApplyComponentGenerationSelectionDTO selection{
      watermark, ReadGeneration<core::ComponentLifecycleGeneration>(reader), reader.U64()};
  if (reader.Boolean()) {
    selection.owner_qualified_predecessor = core::ApplyComponentQualifiedPredecessorDTO{
        ReadGeneration<core::ComponentLifecycleGeneration>(reader), ReadStrong<core::ContentDigest>(reader)};
  }
  result.planning_facts = ManagerComponentBringUpPlanningFactsDTO{std::move(component), std::move(owner),
      std::move(selection), ReadVector<core::ExpectedControlDomainRevisionDTO>(reader, ReadPlanningDomain),
      static_cast<core::LifecycleTargetStateDTO>(reader.Byte()),
      static_cast<core::ComponentLifecycleIntentKindDTO>(reader.Byte())};
}

void WritePneumaticNoneOwner(Writer &writer,
                              const ManagerPneumaticNoneProcessOwnerDTO &owner) {
  WriteStrong(writer, owner.command_target_id);
  WriteStrong(writer, owner.control_binding_id);
  WriteStrong(writer, owner.control_binding_revision);
}

ManagerPneumaticNoneProcessOwnerDTO ReadPneumaticNoneOwner(Reader &reader) {
  return {ReadStrong<core::CommandTargetId>(reader),
          ReadStrong<core::ControlBindingId>(reader),
          ReadStrong<core::ControlBindingRevision>(reader)};
}

void WriteInstalledNoneOwnersTail(Writer &writer,
    const std::vector<ManagerComponentProcessOccurrenceDTO> &rows) {
  const auto count = std::count_if(rows.begin(), rows.end(), [](const auto &row) {
    return row.process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
  });
  if (count == 0) return;
  writer.Byte(kPneumaticNoneOwnersTail);
  writer.String(kInstalledNoneOwnersMagic);
  writer.U32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto &row = rows[index];
    if (row.process_owner_kind != ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) continue;
    writer.U32(static_cast<std::uint32_t>(index));
    writer.Byte(static_cast<std::uint8_t>(row.process_owner_kind));
    WritePneumaticNoneOwner(writer, *row.pneumatic_none_owner);
  }
}

void ReadInstalledNoneOwnersTail(Reader &reader,
    std::vector<ManagerComponentProcessOccurrenceDTO> &rows) {
  if (reader.AtEnd() || reader.Peek() == kInstalledLifecycleTail)
    return;
  if (reader.Byte() != kPneumaticNoneOwnersTail || reader.String() != kInstalledNoneOwnersMagic)
    Fail("installed None owner extension is not a closed supported value");
  const auto count = reader.U32();
  if (count == 0 || count > kMaximumCollectionEntries || count > rows.size())
    Fail("installed None owner collection exceeds its exact row bounds");
  std::optional<std::uint32_t> previous;
  for (std::uint32_t entry = 0; entry < count; ++entry) {
    const auto index = reader.U32();
    if (index >= rows.size() || (previous && index <= *previous))
      Fail("installed None owner indexes are not unique canonical row references");
    previous = index;
    if (reader.Byte() != static_cast<std::uint8_t>(ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal))
      Fail("installed None owner kind is not supported");
    rows[index].process_owner_kind = ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
    rows[index].pneumatic_none_owner = ReadPneumaticNoneOwner(reader);
  }
}

void WriteInspectionNoneOwnersTail(Writer &writer,
    const std::vector<ManagerComponentProcessObservationDTO> &rows) {
  const auto count = std::count_if(rows.begin(), rows.end(), [](const auto &row) {
    return row.process_owner_kind == ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
  });
  if (count == 0) return;
  writer.Byte(kPneumaticNoneOwnersTail);
  writer.String(kInspectionNoneOwnersMagic);
  writer.U32(static_cast<std::uint32_t>(count));
  for (std::size_t index = 0; index < rows.size(); ++index) {
    const auto &row = rows[index];
    if (row.process_owner_kind != ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal) continue;
    writer.U32(static_cast<std::uint32_t>(index));
    writer.Byte(static_cast<std::uint8_t>(row.process_owner_kind));
    WritePneumaticNoneOwner(writer, *row.pneumatic_none_owner);
    WriteStrong(writer, *row.installed_runtime_identity);
  }
}

void ReadInspectionNoneOwnersTail(Reader &reader,
    std::vector<ManagerComponentProcessObservationDTO> &rows) {
  if (reader.AtEnd()) return;
  if (reader.Byte() != kPneumaticNoneOwnersTail || reader.String() != kInspectionNoneOwnersMagic)
    Fail("inspection None owner extension is not a closed supported value");
  const auto count = reader.U32();
  if (count == 0 || count > kMaximumCollectionEntries || count > rows.size())
    Fail("inspection None owner collection exceeds its exact row bounds");
  std::optional<std::uint32_t> previous;
  for (std::uint32_t entry = 0; entry < count; ++entry) {
    const auto index = reader.U32();
    if (index >= rows.size() || (previous && index <= *previous))
      Fail("inspection None owner indexes are not unique canonical row references");
    previous = index;
    if (reader.Byte() != static_cast<std::uint8_t>(ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal))
      Fail("inspection None owner kind is not supported");
    rows[index].process_owner_kind = ManagerComponentProcessOwnerKindDTO::kPneumaticNoneLocal;
    rows[index].pneumatic_none_owner = ReadPneumaticNoneOwner(reader);
    rows[index].installed_runtime_identity = ReadStrong<core::ContentDigest>(reader);
  }
}

void WriteProcessObservations(
    Writer &writer,
    const std::vector<ManagerComponentProcessObservationDTO> &observations) {
  WriteVector(writer, observations, [](Writer &output, const auto &row) {
    WriteStrong(output, row.controllable_component_id);
    output.Byte(static_cast<std::uint8_t>(row.process_presence));
  });
}

[[nodiscard]] std::vector<ManagerComponentProcessObservationDTO>
ReadProcessObservations(Reader &reader) {
  return ReadVector<ManagerComponentProcessObservationDTO>(
      reader, [](Reader &input) {
        auto component_id = ReadStrong<core::ControllableComponentId>(input);
        const auto presence = input.Byte();
        if (presence >
            static_cast<std::uint8_t>(ProcessPresenceState::kPresent)) {
          Fail("component process presence is unknown");
        }
        return ManagerComponentProcessObservationDTO{
            std::move(component_id),
            static_cast<ProcessPresenceState>(presence)};
      });
}

void WriteReceipts(Writer &writer,
                   const std::vector<ManagerRuntimeComponentReceiptDTO> &receipts) {
  WriteVector(writer, receipts, [](Writer &output, const auto &receipt) {
    WriteStrong(output, receipt.controllable_component_id);
    output.Byte(static_cast<std::uint8_t>(receipt.disposition));
    WriteOptionalStrong(output, receipt.installed_runtime_identity);
    WriteVector(output, receipt.runtime_unit_epochs,
                [](Writer &epoch_output, const auto &epoch) {
                  WriteStrong(epoch_output, epoch.runtime_unit_id);
                  WriteStrong(epoch_output, epoch.runtime_epoch);
                });
    output.Boolean(receipt.predecessor_runtime_retained);
    output.Boolean(receipt.product_binding_resealed);
  });
}

[[nodiscard]] std::vector<ManagerRuntimeComponentReceiptDTO>
ReadReceipts(Reader &reader) {
  return ReadVector<ManagerRuntimeComponentReceiptDTO>(reader, [](Reader &input) {
    ManagerRuntimeComponentReceiptDTO receipt{
        ReadStrong<core::ControllableComponentId>(input),
        manyforge::deployment::ApplyComponentDisposition::kRetain,
        std::nullopt, {}, false, false};
    const auto disposition = input.Byte();
    if (disposition > static_cast<std::uint8_t>(
                          manyforge::deployment::ApplyComponentDisposition::kReplace)) {
      Fail("receipt disposition is unknown");
    }
    receipt.disposition =
        static_cast<manyforge::deployment::ApplyComponentDisposition>(disposition);
    receipt.installed_runtime_identity = ReadOptionalStrong<core::ContentDigest>(input);
    receipt.runtime_unit_epochs = ReadVector<core::RuntimeUnitEpochBindingDTO>(
        input, [](Reader &epoch_input) {
          return core::RuntimeUnitEpochBindingDTO{
              ReadStrong<core::RuntimeUnitId>(epoch_input),
              ReadStrong<core::RuntimeEpoch>(epoch_input)};
        });
    receipt.predecessor_runtime_retained = input.Boolean();
    receipt.product_binding_resealed = input.Boolean();
    return receipt;
  });
}

void WriteError(Writer &writer, const core::ErrorDTO &error) {
  if (error.code.empty() || error.message.empty() ||
      error.details.size() > kMaximumCollectionEntries) {
    Fail("cannot encode an invalid error");
  }
  writer.String(error.code);
  writer.String(error.message);
  writer.U32(static_cast<std::uint32_t>(error.details.size()));
  for (const auto &[name, value] : error.details) {
    writer.String(name);
    writer.String(value);
  }
}

[[nodiscard]] core::ErrorDTO ReadError(Reader &reader) {
  core::ErrorDTO error{reader.String(), reader.String(), {}};
  const auto count = static_cast<std::size_t>(reader.U32());
  if (count > kMaximumCollectionEntries) {
    Fail("error details exceed their entry bound");
  }
  for (std::size_t index = 0U; index < count; ++index) {
    auto name = reader.String();
    auto value = reader.String();
    if (!error.details.emplace(std::move(name), std::move(value)).second) {
      Fail("error details repeat a key");
    }
  }
  if (error.code.empty() || error.message.empty()) {
    Fail("decoded error is invalid");
  }
  return error;
}

} // namespace

bool IsSelectedRuntimeCompositionRpcPacket(
    const std::string_view bytes) noexcept {
  if (bytes.size() <= kMagic.size())
    return false;
  const auto magic = bytes.substr(0U, kMagic.size());
  return magic ==
             std::string_view(reinterpret_cast<const char *>(kMagic.data()),
                              kMagic.size()) ||
         magic == std::string_view(
                      reinterpret_cast<const char *>(kProviderMagic.data()),
                      kProviderMagic.size());
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid request");
  }
  Writer writer(kRequestTag,
                UsesProviderDomains(request.configuration_fence_scope));
  WriteRequest(writer, request);
  return std::move(writer).Finish();
}

ManagerSelectedRuntimeCompositionRequestDTO
DecodeSelectedRuntimeCompositionRpcRequest(const std::string_view bytes) {
  Reader reader(bytes);
  if (reader.Byte() != kRequestTag) {
    Fail("packet is not a request");
  }
  auto request = ReadRequest(reader);
  reader.Finish(UsesProviderDomains(request.configuration_fence_scope));
  if (!request.valid()) {
    Fail("decoded request is invalid");
  }
  return request;
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedComponentBringUpRequestDTO &request) {
  if (!request.valid())
    Fail("cannot encode an invalid selected submit");
  Writer writer(kSelectedSubmitTag);
  WriteSelectedRequest(writer, request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedComponentBringUpReadRequestDTO &request) {
  if (!request.valid())
    Fail("cannot encode an invalid selected read");
  Writer writer(kSelectedReadTag);
  WriteSelectedRequest(writer, request.request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerApplyComponentBringUpRequestDTO &request) {
  if (!request.valid()) Fail("cannot encode an invalid Apply submit");
  Writer writer(kApplySubmitTag);
  WriteApplyRequest(writer, request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerApplyComponentBringUpReadRequestDTO &request) {
  if (!request.valid()) Fail("cannot encode an invalid Apply read");
  Writer writer(kApplyReadTag);
  WriteApplyRequest(writer, request.request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentBringUpInspectionRequestDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid component inspection request");
  }
  Writer writer(kInspectionRequestTag);
  WriteInspectionRequest(writer, request);
  WriteInspectionPlanningRequestTail(writer, request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessStopRequestDTO &request) {
  if (!request.valid()) Fail("cannot encode invalid process Stop request");
  Writer writer(kProcessStopSubmitTag);
  WriteProcessStopRequest(writer, request);
  return std::move(writer).Finish();
}
std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessStopReadRequestDTO &request) {
  if (!request.valid()) Fail("cannot encode invalid process Stop read");
  Writer writer(kProcessStopReadTag);
  WriteProcessStopRequest(writer, request.request);
  return std::move(writer).Finish();
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerComponentProcessInventoryRequestDTO &request) {
  if (!request.valid())
    Fail("cannot encode invalid installed process inventory request");
  Writer writer(kInstalledInventoryRequestTag);
  WriteInstalledInventoryRequest(writer, request);
  return std::move(writer).Finish();
}

SelectedRuntimeCompositionRpcRequest
DecodeSelectedRuntimeCompositionRpcRequestVariant(
    const std::string_view bytes) {
  Reader reader(bytes);
  const auto tag = reader.Byte();
  SelectedRuntimeCompositionRpcRequest request =
      [&]() -> SelectedRuntimeCompositionRpcRequest {
    if (tag == kInstalledInventoryRequestTag)
      return ReadInstalledInventoryRequest(reader);
    if (tag == kProcessStopSubmitTag)
      return ReadProcessStopRequest(reader);
    if (tag == kProcessStopReadTag)
      return ManagerComponentProcessStopReadRequestDTO{ReadProcessStopRequest(reader)};
    if (tag == kRequestTag)
      return ReadRequest(reader);
    if (tag == kSelectedSubmitTag)
      return ReadSelectedRequest(reader);
    if (tag == kSelectedReadTag) {
      return ManagerSelectedComponentBringUpReadRequestDTO{
          ReadSelectedRequest(reader)};
    }
    if (tag == kApplySubmitTag)
      return ReadApplyRequest(reader);
    if (tag == kApplyReadTag)
      return ManagerApplyComponentBringUpReadRequestDTO{ReadApplyRequest(reader)};
    if (tag == kInspectionRequestTag) {
      auto inspection = ReadInspectionRequest(reader);
      ReadInspectionPlanningRequestTail(reader, inspection);
      return inspection;
    }
    Fail("packet is not a request");
  }();
  const bool provider_scope = std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          T, ManagerSelectedRuntimeCompositionRequestDTO>)
          return UsesProviderDomains(value.configuration_fence_scope);
        else
          return false;
      },
      request);
  reader.Finish(provider_scope);
  if (!std::visit([](const auto &value) { return value.valid(); }, request)) {
    Fail("decoded request is invalid");
  }
  return request;
}

std::string EncodeSelectedRuntimeCompositionRpcResponse(
    const SelectedRuntimeCompositionRpcResponse &response) {
  return std::visit(
      [](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          Value, ManagerSelectedRuntimeCompositionResultDTO>) {
          if (!value.valid()) {
            Fail("cannot encode an invalid result");
          }
          Writer writer(
              value.state == ManagerSelectedRuntimeCompositionState::kPreparing
                  ? kPreparingTag
                  : kInstalledTag,
              UsesProviderDomains(value.request.configuration_fence_scope));
          WriteRequest(writer, value.request);
          WriteReceipts(writer, value.component_receipts);
          return std::move(writer).Finish();
        } else if constexpr (std::is_same_v<
                                 Value,
                                 ManagerSelectedComponentBringUpResultDTO>) {
          if (!value.valid())
            Fail("cannot encode an invalid selected result");
          Writer writer(kSelectedResultTag);
          WriteSelectedRequest(writer, value.request);
          writer.Boolean(value.result.has_value());
          if (value.result.has_value()) {
            writer.String(core::EncodeSelectedComponentBringUpRequestResult(
                *value.result));
          }
          return std::move(writer).Finish();
        } else if constexpr (std::is_same_v<Value, ManagerApplyComponentBringUpResultDTO>) {
          if (!value.valid()) Fail("cannot encode an invalid Apply result");
          Writer writer(kApplyResultTag);
          WriteApplyRequest(writer, value.request);
          writer.Boolean(value.result.has_value());
          if (value.result.has_value()) {
            writer.String(core::EncodeApplyDelegatedComponentLifecycleRequestResult(*value.result));
          }
          return std::move(writer).Finish();
        } else if constexpr (std::is_same_v<
                                 Value,
                                 ManagerComponentBringUpInspectionResultDTO>) {
          if (!value.valid()) {
            Fail("cannot encode an invalid component inspection result");
          }
          Writer writer(kInspectionResultTag, UsesProviderPlanning(value));
          WriteInspectionRequest(writer, value.request);
          WriteProcessObservations(writer, value.configured_components);
          WriteInspectionPlanningResultTail(writer, value);
          WriteInspectionNoneOwnersTail(writer, value.configured_components);
          return std::move(writer).Finish();
        } else if constexpr (std::is_same_v<
                                 Value,
                                 ManagerComponentProcessInventoryResultDTO>) {
          if (!value.valid())
            Fail("cannot encode invalid installed process inventory result");
          Writer writer(kInstalledInventoryResultTag);
          WriteInstalledInventoryRequest(writer, value.request);
          WriteVector(writer, value.process_occurrences,
                      WriteInstalledOccurrence);
          writer.Boolean(value.error.has_value());
          if (value.error)
            WriteError(writer, *value.error);
          WriteInstalledNoneOwnersTail(writer, value.process_occurrences);
          if (!value.component_lifecycles.empty()) {
            writer.Byte(kInstalledLifecycleTail);
            writer.String(kInstalledLifecycleMagic);
            WriteVector(writer, value.component_lifecycles,
                        [](Writer &out, const auto &row) {
                          WriteStrong(out, row.controllable_component_id);
                          out.U64(row.generation.value());
                          out.Byte(static_cast<std::uint8_t>(row.state));
                        });
          }
          return std::move(writer).Finish();
        } else if constexpr (std::is_same_v<
                                 Value, ManagerComponentProcessStopResultDTO>) {
          if (!value.valid()) Fail("cannot encode invalid process Stop result");
          Writer writer(kProcessStopResultTag);
          WriteProcessStopRequest(writer, value.request);
          writer.Byte(static_cast<std::uint8_t>(value.state));
          writer.Boolean(value.receipt.has_value());
          if (value.receipt) {
            writer.U32(static_cast<std::uint32_t>(value.receipt->leader_pid));
            writer.String(value.receipt->controllable_component_id);
            writer.String(value.receipt->launch_identity);
            writer.String(value.receipt->observed_utc);
          }
          writer.Boolean(value.error.has_value());
          if (value.error) WriteError(writer, *value.error);
          return std::move(writer).Finish();
        } else {
          Writer writer(kFailureTag);
          WriteError(writer, value.error);
          return std::move(writer).Finish();
        }
      },
      response);
}

SelectedRuntimeCompositionRpcResponse
DecodeSelectedRuntimeCompositionRpcResponse(const std::string_view bytes) {
  Reader reader(bytes);
  const auto tag = reader.Byte();
  if (tag == kFailureTag) {
    auto error = ReadError(reader);
    reader.Finish();
    return SelectedRuntimeCompositionRpcFailure{std::move(error)};
  }
  if (tag == kInstalledInventoryResultTag) {
    ManagerComponentProcessInventoryResultDTO result{
        ReadInstalledInventoryRequest(reader),
        ReadVector<ManagerComponentProcessOccurrenceDTO>(
            reader, ReadInstalledOccurrence)};
    if (reader.Boolean())
      result.error = ReadError(reader);
    ReadInstalledNoneOwnersTail(reader, result.process_occurrences);
    if (!reader.AtEnd()) {
      if (reader.Byte() != kInstalledLifecycleTail ||
          reader.String() != kInstalledLifecycleMagic)
        Fail("installed lifecycle extension is not a closed supported value");
      result.component_lifecycles =
          ReadVector<ManagerComponentLifecycleObservationDTO>(
              reader,
              [](Reader &in) -> ManagerComponentLifecycleObservationDTO {
                auto component = ReadStrong<core::ControllableComponentId>(in);
                const auto generation = in.U64();
                return {
                    std::move(component),
                    core::ComponentLifecycleGeneration(generation),
                    static_cast<core::ComponentLifecycleStateDTO>(in.Byte())};
              });
      if (result.component_lifecycles.empty())
        Fail("installed lifecycle extension is empty");
    }
    reader.Finish();
    if (!result.valid())
      Fail("decoded installed process inventory is invalid");
    return result;
  }
  if (tag == kProcessStopResultTag) {
    ManagerComponentProcessStopResultDTO result{ReadProcessStopRequest(reader),
        static_cast<core::ControllerLaunchCleanupState>(reader.Byte())};
    if (reader.Boolean()) {
      const auto pid = reader.U32();
      if (pid > static_cast<std::uint32_t>(std::numeric_limits<int>::max()))
        Fail("drain receipt PID exceeds signed value domain");
      result.receipt = core::ControllerLaunchGroupDrainReceipt{
          static_cast<int>(pid), reader.String(), reader.String(), reader.String()};
    }
    if (reader.Boolean()) result.error = ReadError(reader);
    reader.Finish();
    if (!result.valid()) Fail("decoded process Stop result is invalid");
    return result;
  }
  if (tag == kSelectedResultTag) {
    ManagerSelectedComponentBringUpResultDTO result{ReadSelectedRequest(reader),
                                                    std::nullopt};
    if (reader.Boolean()) {
      result.result =
          core::DecodeSelectedComponentBringUpRequestResult(reader.String());
    }
    reader.Finish();
    if (!result.valid())
      Fail("decoded selected result is invalid");
    return result;
  }
  if (tag == kApplyResultTag) {
    ManagerApplyComponentBringUpResultDTO result{ReadApplyRequest(reader), std::nullopt};
    if (reader.Boolean())
      result.result = core::DecodeApplyDelegatedComponentLifecycleRequestResult(reader.String());
    reader.Finish();
    if (!result.valid()) Fail("decoded Apply result is invalid");
    return result;
  }
  if (tag == kInspectionResultTag) {
    auto request = ReadInspectionRequest(reader);
    auto observations = ReadProcessObservations(reader);
    ManagerComponentBringUpInspectionResultDTO result{
        std::move(request), std::move(observations)};
    ReadInspectionPlanningResultTail(reader, result);
    ReadInspectionNoneOwnersTail(reader, result.configured_components);
    reader.Finish(UsesProviderPlanning(result));
    if (!result.valid()) {
      Fail("decoded component inspection result is invalid");
    }
    return result;
  }
  if (tag != kPreparingTag && tag != kInstalledTag) {
    Fail("packet is not a response");
  }
  auto request = ReadRequest(reader);
  auto receipts = ReadReceipts(reader);
  ManagerSelectedRuntimeCompositionResultDTO result{
      std::move(request),
      tag == kPreparingTag
          ? ManagerSelectedRuntimeCompositionState::kPreparing
          : ManagerSelectedRuntimeCompositionState::kInstalled,
      std::move(receipts)};
  reader.Finish(UsesProviderDomains(result.request.configuration_fence_scope));
  if (!result.valid()) {
    Fail("decoded result is invalid");
  }
  return result;
}

} // namespace manyforge::component_manager::detail

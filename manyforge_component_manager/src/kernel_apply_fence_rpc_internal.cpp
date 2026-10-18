#include "kernel_apply_fence_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>
#include <manyforge_core/component_lifecycle_value_codec.hpp>

#include <array>
#include <bit>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace manyforge::component_manager::detail {
namespace {

namespace core = manyforge::core;

constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'K',
                                              'A', 'F', 'R', '2'};
constexpr auto kProviderMagic = [] {
  auto value = kMagic;
  value.back() = '4';
  return value;
}();
constexpr std::size_t kMaximumCollectionEntries = 4096U;

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("Kernel Apply-fence RPC codec: " + detail);
}

class Writer final {
public:
  explicit Writer(bool provider_format = false)
      : provider_format_(provider_format) {
    bytes_.reserve(1024U);
    for (const auto byte : provider_format_ ? kProviderMagic : kMagic) {
      Byte(byte);
    }
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

  void I64(const std::int64_t value) {
    U64(std::bit_cast<std::uint64_t>(value));
  }

  void String(const std::string_view value) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max()) {
      Fail("string exceeds the wire length domain");
    }
    U32(static_cast<std::uint32_t>(value.size()));
    RequireSpace(value.size());
    bytes_.append(value);
  }

  [[nodiscard]] std::string Finish() && {
    if (bytes_.size() > kMaximumKernelApplyFenceRpcPacketBytes) {
      Fail("encoded packet exceeds its byte bound");
    }
    return std::move(bytes_);
  }

private:
  void RequireSpace(const std::size_t additional) const {
    if (additional > kMaximumKernelApplyFenceRpcPacketBytes - bytes_.size()) {
      Fail("encoded packet exceeds its byte bound");
    }
  }

  bool provider_format_;
  std::string bytes_;
};

class Reader final {
public:
  explicit Reader(const std::string_view bytes) : bytes_(bytes) {
    if (bytes_.empty() ||
        bytes_.size() > kMaximumKernelApplyFenceRpcPacketBytes) {
      Fail("packet is empty or oversized");
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

  [[nodiscard]] std::int64_t I64() {
    return std::bit_cast<std::int64_t>(U64());
  }

  [[nodiscard]] std::string String() {
    const auto size = static_cast<std::size_t>(U32());
    Require(size);
    std::string result(bytes_.substr(offset_, size));
    offset_ += size;
    return result;
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

template <typename Value, typename Encode>
void WriteOptional(Writer &writer, const std::optional<Value> &value,
                   Encode encode) {
  writer.Boolean(value.has_value());
  if (value.has_value()) {
    encode(writer, *value);
  }
}

template <typename Value, typename Decode>
[[nodiscard]] std::optional<Value> ReadOptional(Reader &reader, Decode decode) {
  if (!reader.Boolean()) {
    return std::nullopt;
  }
  return decode(reader);
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
  const auto size = static_cast<std::size_t>(reader.U32());
  if (size > kMaximumCollectionEntries) {
    Fail("collection exceeds its entry bound");
  }
  std::vector<Value> result;
  result.reserve(size);
  for (std::size_t index = 0U; index < size; ++index) {
    result.push_back(decode(reader));
  }
  return result;
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

void WriteProjectionRevision(Writer &writer,
                             const core::ProjectionRevisionDTO &revision) {
  if (!revision.valid()) {
    Fail("cannot encode an invalid projection revision");
  }
  WriteStrong(writer, revision.retained_bundle_id);
  WriteStrong(writer, revision.graph_revision);
  WriteStrong(writer, revision.scene_revision.scene_id);
  writer.U64(revision.scene_revision.revision_id);
  WriteStrong(writer, revision.scene_revision.content_hash);
}

[[nodiscard]] core::ProjectionRevisionDTO
ReadProjectionRevision(Reader &reader) {
  core::ProjectionRevisionDTO result{ReadStrong<core::RetainedBundleId>(reader),
                                     ReadStrong<core::GraphRevision>(reader),
                                     {ReadStrong<core::SceneId>(reader),
                                      reader.U64(),
                                      ReadStrong<core::ContentDigest>(reader)}};
  if (!result.valid()) {
    Fail("decoded projection revision is invalid");
  }
  return result;
}

void WritePublicRequestContext(Writer &writer,
                               const core::PublicRequestContextDTO &context) {
  WriteStrong(writer, context.request_id);
  WriteStrong(writer, context.request_owner_epoch);
  writer.String(context.source_label);
  WriteOptional<std::string>(
      writer, context.correlation_id,
      [](Writer &output, const std::string &value) { output.String(value); });
}

[[nodiscard]] core::PublicRequestContextDTO
ReadPublicRequestContext(Reader &reader) {
  return {ReadStrong<core::RequestId>(reader),
          ReadStrong<core::RequestOwnerEpoch>(reader), reader.String(),
          ReadOptional<std::string>(
              reader, [](Reader &input) { return input.String(); })};
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
  if (!scope.valid()) {
    Fail("cannot encode an invalid configuration-fence scope");
  }
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
    WriteOptional<core::ControlDomainRevision>(
        output, value.predecessor_control_domain_revision,
        [](Writer &nested, const auto &revision) {
          WriteStrong(nested, revision);
        });
    WriteOptional<core::ControlDomainRevision>(
        output, value.candidate_control_domain_revision,
        [](Writer &nested, const auto &revision) {
          WriteStrong(nested, revision);
        });
  });
  WriteVector(writer, scope.bindings, [](Writer &output, const auto &value) {
    WriteStrong(output, value.control_binding_id);
    WriteOptional<core::ControlBindingRevision>(
        output, value.predecessor_control_binding_revision,
        [](Writer &nested, const auto &revision) {
          WriteStrong(nested, revision);
        });
    WriteOptional<core::ControlBindingRevision>(
        output, value.candidate_control_binding_revision,
        [](Writer &nested, const auto &revision) {
          WriteStrong(nested, revision);
        });
  });
  WriteVector(writer, scope.resources, [](Writer &output, const auto &value) {
    WriteStrong(output, value);
  });
}

[[nodiscard]] core::ConfigurationFenceScopeDTO ReadScope(Reader &reader) {
  if (reader.ProviderFormat())
    return core::detail::DecodeConfigurationFenceScopeRecord(reader.String());
  core::ConfigurationFenceScopeDTO result{
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
                ReadOptional<core::ControlDomainRevision>(
                    input,
                    [](Reader &nested) {
                      return ReadStrong<core::ControlDomainRevision>(nested);
                    }),
                ReadOptional<core::ControlDomainRevision>(
                    input, [](Reader &nested) {
                      return ReadStrong<core::ControlDomainRevision>(nested);
                    })};
          }),
      ReadVector<core::ConfigurationFenceBindingScopeDTO>(
          reader,
          [](Reader &input) {
            return core::ConfigurationFenceBindingScopeDTO{
                ReadStrong<core::ControlBindingId>(input),
                ReadOptional<core::ControlBindingRevision>(
                    input,
                    [](Reader &nested) {
                      return ReadStrong<core::ControlBindingRevision>(nested);
                    }),
                ReadOptional<core::ControlBindingRevision>(
                    input, [](Reader &nested) {
                      return ReadStrong<core::ControlBindingRevision>(nested);
                    })};
          }),
      ReadVector<core::ResourceId>(reader, [](Reader &input) {
        return ReadStrong<core::ResourceId>(input);
      })};
  if (!result.valid()) {
    Fail("decoded configuration-fence scope is invalid");
  }
  return result;
}

void WriteActiveConfiguration(
    Writer &writer, const core::DomainActiveControlConfigurationDTO &value) {
  if (!value.valid()) {
    Fail("cannot encode an invalid active configuration");
  }
  WriteStrong(writer, value.control_domain_id);
  WriteStrong(writer, value.active_control_configuration_id);
  WriteGeneration(writer, value.active_control_configuration_generation);
}

[[nodiscard]] core::DomainActiveControlConfigurationDTO
ReadActiveConfiguration(Reader &reader) {
  core::DomainActiveControlConfigurationDTO result{
      ReadStrong<core::ControlDomainId>(reader),
      ReadStrong<core::ActiveControlConfigurationId>(reader),
      ReadGeneration<core::ActiveControlConfigurationGeneration>(reader)};
  if (!result.valid()) {
    Fail("decoded active configuration is invalid");
  }
  return result;
}

void WriteFenceOwner(Writer &writer, const core::ApplyFenceOwnerDTO &owner) {
  if (!owner.valid()) {
    Fail("cannot encode an invalid Apply-fence owner");
  }
  WriteStrong(writer, owner.apply_id);
  WriteGeneration(writer, owner.apply_generation);
}

[[nodiscard]] core::ApplyFenceOwnerDTO ReadFenceOwner(Reader &reader) {
  core::ApplyFenceOwnerDTO result{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader)};
  if (!result.valid()) {
    Fail("decoded Apply-fence owner is invalid");
  }
  return result;
}

void WriteReplayStatus(Writer &writer,
                       const core::RequestReplayStatusDTO status) {
  if (!core::IsKnownRequestReplayStatus(status)) {
    Fail("cannot encode an unknown request replay status");
  }
  writer.Byte(static_cast<std::uint8_t>(status));
}

[[nodiscard]] core::RequestReplayStatusDTO ReadReplayStatus(Reader &reader) {
  const auto result = static_cast<core::RequestReplayStatusDTO>(reader.Byte());
  if (!core::IsKnownRequestReplayStatus(result)) {
    Fail("decoded request replay status is unknown");
  }
  return result;
}

void WriteProviderLineage(
    Writer &writer, const core::ProviderConfigurationFenceLineageDTO &value) {
  if (!value.valid())
    Fail("invalid provider lineage receipt");
  WriteStrong(writer, value.control_domain_id);
  WriteStrong(writer, value.provider_runtime_epoch);
  WriteGeneration(writer, value.component_lifecycle_generation);
}
core::ProviderConfigurationFenceLineageDTO ReadProviderLineage(Reader &reader) {
  core::ProviderConfigurationFenceLineageDTO value{
      ReadStrong<core::ControlDomainId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
      ReadGeneration<core::ComponentLifecycleGeneration>(reader)};
  if (!value.valid())
    Fail("invalid provider lineage receipt");
  return value;
}

void WriteIntent(Writer &writer,
                 const core::ApplyConfigurationFenceIntentDTO &intent) {
  if (!intent.valid()) {
    Fail("cannot encode an invalid Apply-fence intent");
  }
  WritePublicRequestContext(writer, intent.owner_request_context);
  WriteFenceOwner(writer, intent.owner_transaction);
  WriteScope(writer, intent.scope);
  writer.Byte(static_cast<std::uint8_t>(intent.busy_disposition));
  WriteStrong(writer, intent.expected_retained_bundle_id);
  WriteStrong(writer, intent.expected_graph_revision);
  writer.U64(intent.expected_operation_resource_generation);
  WriteGeneration(writer, intent.expected_fence_generation);
  WriteVector(writer, intent.expected_active_control_configurations,
              WriteActiveConfiguration);
  writer.String(intent.reason);
  if (writer.ProviderFormat())
    WriteVector(writer, intent.expected_provider_lineages,
                WriteProviderLineage);
}

[[nodiscard]] core::ApplyConfigurationFenceIntentDTO
ReadIntent(Reader &reader) {
  core::ApplyConfigurationFenceIntentDTO result{
      ReadPublicRequestContext(reader),
      ReadFenceOwner(reader),
      ReadScope(reader),
      static_cast<core::ApplyFenceBusyDispositionDTO>(reader.Byte()),
      ReadStrong<core::RetainedBundleId>(reader),
      ReadStrong<core::GraphRevision>(reader),
      reader.U64(),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadVector<core::DomainActiveControlConfigurationDTO>(
          reader, ReadActiveConfiguration),
      reader.String()};
  if (reader.ProviderFormat())
    result.expected_provider_lineages =
        ReadVector<core::ProviderConfigurationFenceLineageDTO>(
            reader, ReadProviderLineage);
  if (!result.valid()) {
    Fail("decoded Apply-fence intent is invalid");
  }
  return result;
}

void WriteAccepted(
    Writer &writer, const core::ConfigurationFenceAcceptedDTO &accepted,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  if (!accepted.valid()) {
    Fail("cannot encode an invalid configuration-fence acceptance");
  }
  if (shared_scope != nullptr && accepted.captured_scope != *shared_scope) {
    Fail("configuration-fence acceptance differs from its shared scope");
  }
  WriteStrong(writer, accepted.configuration_fence_id);
  WriteGeneration(writer, accepted.configuration_fence_generation);
  if (shared_scope == nullptr) {
    WriteScope(writer, accepted.captured_scope);
  }
  writer.I64(accepted.accepted_at_ns);
  WriteReplayStatus(writer, accepted.replay_status);
}

[[nodiscard]] core::ConfigurationFenceAcceptedDTO ReadAccepted(
    Reader &reader,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  core::ConfigurationFenceAcceptedDTO result{
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      shared_scope == nullptr ? ReadScope(reader) : *shared_scope, reader.I64(),
      ReadReplayStatus(reader)};
  if (!result.valid()) {
    Fail("decoded configuration-fence acceptance is invalid");
  }
  return result;
}

void WriteExecutionHostEpoch(Writer &writer,
                             const core::ExecutionHostEpochDTO &epoch) {
  if (!epoch.valid()) {
    Fail("cannot encode an invalid execution-host epoch");
  }
  WriteStrong(writer, epoch.runtime_unit_id);
  WriteStrong(writer, epoch.runtime_epoch);
  WriteStrong(writer, epoch.request_owner_epoch);
  WriteStrong(writer, epoch.durable_stop_fence_ledger_digest);
  WriteGeneration(writer, epoch.publication_generation);
}

[[nodiscard]] core::ExecutionHostEpochDTO
ReadExecutionHostEpoch(Reader &reader) {
  core::ExecutionHostEpochDTO result{
      ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
      ReadStrong<core::RequestOwnerEpoch>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader)};
  if (!result.valid()) {
    Fail("decoded execution-host epoch is invalid");
  }
  return result;
}

void WriteAdmissionQuery(
    Writer &writer, const core::ConfigurationFenceAdmissionQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode an invalid fence-admission query");
  }
  WriteScope(writer, query.affected_scope);
  WriteStrong(writer, query.manager_runtime_epoch);
}

[[nodiscard]] core::ConfigurationFenceAdmissionQueryDTO
ReadAdmissionQuery(Reader &reader) {
  core::ConfigurationFenceAdmissionQueryDTO result{
      ReadScope(reader), ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence-admission query is invalid");
  }
  return result;
}

void WriteAdmissionSnapshot(
    Writer &writer,
    const core::ConfigurationFenceAdmissionSnapshotDTO &snapshot) {
  if (!snapshot.presence_valid()) {
    Fail("cannot encode an invalid fence-admission snapshot");
  }
  WriteAdmissionQuery(writer, snapshot.echoed_query);
  WriteExecutionHostEpoch(writer, snapshot.execution_host_epoch);
  WriteProjectionRevision(writer, snapshot.active_projection_revision);
  WriteStrong(writer, snapshot.active_public_pointer_digest);
  writer.U64(snapshot.operation_resource_generation);
  WriteGeneration(writer, snapshot.configuration_fence_generation);
  WriteVector(writer, snapshot.active_control_configurations,
              WriteActiveConfiguration);
  writer.U64(snapshot.affected_queued_operation_count);
  writer.U64(snapshot.affected_active_operation_count);
  WriteStrong(writer, snapshot.result_digest);
  if (writer.ProviderFormat())
    WriteVector(writer, snapshot.provider_lineages, WriteProviderLineage);
}

[[nodiscard]] core::ConfigurationFenceAdmissionSnapshotDTO
ReadAdmissionSnapshot(Reader &reader) {
  core::ConfigurationFenceAdmissionSnapshotDTO result{
      ReadAdmissionQuery(reader),
      ReadExecutionHostEpoch(reader),
      ReadProjectionRevision(reader),
      ReadStrong<core::ContentDigest>(reader),
      reader.U64(),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadVector<core::DomainActiveControlConfigurationDTO>(
          reader, ReadActiveConfiguration),
      reader.U64(),
      reader.U64(),
      ReadStrong<core::ContentDigest>(reader)};
  if (reader.ProviderFormat())
    result.provider_lineages =
        ReadVector<core::ProviderConfigurationFenceLineageDTO>(
            reader, ReadProviderLineage);
  if (!result.presence_valid()) {
    Fail("decoded fence-admission snapshot is invalid");
  }
  return result;
}

void WriteReconciliationEvidence(
    Writer &writer,
    const core::ConfigurationFenceReconciliationEvidenceDTO &evidence,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  if (!evidence.valid()) {
    Fail("cannot encode invalid fence reconciliation evidence");
  }
  if (shared_scope != nullptr && evidence.reconciled_scope != *shared_scope) {
    Fail("fence reconciliation evidence differs from its shared scope");
  }
  WriteExecutionHostEpoch(writer, evidence.execution_host_epoch);
  WriteFenceOwner(writer, evidence.owner_transaction);
  WriteStrong(writer, evidence.configuration_fence_id);
  WriteGeneration(writer, evidence.configuration_fence_generation);
  if (shared_scope == nullptr) {
    WriteScope(writer, evidence.reconciled_scope);
  }
  writer.U64(evidence.operation_resource_generation);
  WriteStrong(writer, evidence.active_graph_revision);
  WriteProjectionRevision(writer, evidence.active_projection_revision);
  WriteGeneration(writer, evidence.active_publication_generation);
  WriteStrong(writer, evidence.active_public_pointer_digest);
  WriteVector(writer, evidence.active_control_configurations,
              WriteActiveConfiguration);
  writer.U64(evidence.affected_queued_operation_count);
  writer.U64(evidence.affected_active_operation_count);
  WriteStrong(writer, evidence.evidence_digest);
}

[[nodiscard]] core::ConfigurationFenceReconciliationEvidenceDTO
ReadReconciliationEvidence(
    Reader &reader,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  core::ConfigurationFenceReconciliationEvidenceDTO result{
      ReadExecutionHostEpoch(reader),
      ReadFenceOwner(reader),
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      shared_scope == nullptr ? ReadScope(reader) : *shared_scope,
      reader.U64(),
      ReadStrong<core::GraphRevision>(reader),
      ReadProjectionRevision(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadVector<core::DomainActiveControlConfigurationDTO>(
          reader, ReadActiveConfiguration),
      reader.U64(),
      reader.U64(),
      ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded fence reconciliation evidence is invalid");
  }
  return result;
}

void WriteRelease(Writer &writer,
                  const core::ConfigurationFenceReleaseDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid fence release request");
  }
  WriteReconciliationEvidence(writer, request.reconciliation_evidence);
  WriteStrong(writer, request.manager_runtime_epoch);
}

[[nodiscard]] core::ConfigurationFenceReleaseDTO ReadRelease(Reader &reader) {
  core::ConfigurationFenceReleaseDTO result{
      ReadReconciliationEvidence(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence release request is invalid");
  }
  return result;
}

void WriteReleased(Writer &writer,
                   const core::ConfigurationFenceReleasedDTO &released) {
  if (!released.valid()) {
    Fail("cannot encode an invalid fence release receipt");
  }
  WriteRelease(writer, released.request);
  WriteExecutionHostEpoch(writer, released.execution_host_epoch);
  writer.I64(released.released_at_ns);
  WriteReplayStatus(writer, released.replay_status);
  WriteStrong(writer, released.release_receipt_digest);
}

[[nodiscard]] core::ConfigurationFenceReleasedDTO ReadReleased(Reader &reader) {
  core::ConfigurationFenceReleasedDTO result{
      ReadRelease(reader), ReadExecutionHostEpoch(reader), reader.I64(),
      ReadReplayStatus(reader), ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded fence release receipt is invalid");
  }
  return result;
}

void WriteReleasedWithSharedEvidence(
    Writer &writer, const core::ConfigurationFenceReleasedDTO &released,
    const core::ConfigurationFenceReconciliationEvidenceDTO &shared_evidence) {
  if (!released.valid() ||
      released.request.reconciliation_evidence != shared_evidence ||
      released.execution_host_epoch != shared_evidence.execution_host_epoch) {
    Fail("fence release receipt differs from its shared status evidence");
  }
  WriteStrong(writer, released.request.manager_runtime_epoch);
  writer.I64(released.released_at_ns);
  WriteReplayStatus(writer, released.replay_status);
  WriteStrong(writer, released.release_receipt_digest);
}

[[nodiscard]] core::ConfigurationFenceReleasedDTO
ReadReleasedWithSharedEvidence(
    Reader &reader,
    const core::ConfigurationFenceReconciliationEvidenceDTO &shared_evidence) {
  core::ConfigurationFenceReleasedDTO result{
      core::ConfigurationFenceReleaseDTO{
          shared_evidence, ReadStrong<core::RuntimeEpoch>(reader)},
      shared_evidence.execution_host_epoch, reader.I64(),
      ReadReplayStatus(reader), ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded shared-evidence fence release receipt is invalid");
  }
  return result;
}

void WriteStatusQuery(Writer &writer,
                      const core::ConfigurationFenceStatusQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode an invalid fence status query");
  }
  WriteFenceOwner(writer, query.owner_transaction);
  WriteStrong(writer, query.configuration_fence_id);
  WriteGeneration(writer, query.configuration_fence_generation);
  WriteScope(writer, query.expected_scope);
  WriteStrong(writer, query.manager_runtime_epoch);
}

[[nodiscard]] core::ConfigurationFenceStatusQueryDTO
ReadStatusQuery(Reader &reader) {
  core::ConfigurationFenceStatusQueryDTO result{
      ReadFenceOwner(reader), ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadScope(reader), ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence status query is invalid");
  }
  return result;
}

void WriteStatus(Writer &writer,
                 const core::ConfigurationFenceStatusDTO &status) {
  if (!status.presence_valid()) {
    Fail("cannot encode an invalid fence status");
  }
  WriteStatusQuery(writer, status.echoed_query);
  // The DTO validity relation proves this scope equals expected_scope. Keep
  // one copy on this private bounded wire and reconstruct the second value.
  WriteReconciliationEvidence(writer, status.reconciliation_evidence,
                              &status.echoed_query.expected_scope);
  writer.Byte(static_cast<std::uint8_t>(status.state));
  writer.Boolean(status.admission_fenced);
  WriteOptional<core::ConfigurationFenceReleasedDTO>(
      writer, status.release_receipt,
      [&status](Writer &output, const auto &released) {
        WriteReleasedWithSharedEvidence(output, released,
                                        status.reconciliation_evidence);
      });
  writer.String(status.required_compensation_or_operator_direction);
  WriteStrong(writer, status.result_digest);
  WriteOptional<core::ErrorDTO>(writer, status.error, WriteError);
}

[[nodiscard]] core::ConfigurationFenceStatusDTO ReadStatus(Reader &reader) {
  auto query = ReadStatusQuery(reader);
  auto evidence = ReadReconciliationEvidence(reader, &query.expected_scope);
  const auto state = static_cast<core::ConfigurationFenceState>(reader.Byte());
  const bool admission_fenced = reader.Boolean();
  auto release_receipt = ReadOptional<core::ConfigurationFenceReleasedDTO>(
      reader, [&evidence](Reader &input) {
        return ReadReleasedWithSharedEvidence(input, evidence);
      });
  core::ConfigurationFenceStatusDTO result{
      std::move(query),
      std::move(evidence),
      state,
      admission_fenced,
      std::move(release_receipt),
      reader.String(),
      ReadStrong<core::ContentDigest>(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded fence status is invalid");
  }
  return result;
}

void WriteAttemptQuery(Writer &writer,
                       const core::ApplyFenceAcquireAttemptQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode an invalid fence-attempt query");
  }
  WriteIntent(writer, query.persisted_intent);
  WriteStrong(writer, query.persisted_attempt_digest);
  WriteStrong(writer, query.manager_runtime_epoch);
}

[[nodiscard]] core::ApplyFenceAcquireAttemptQueryDTO
ReadAttemptQuery(Reader &reader) {
  core::ApplyFenceAcquireAttemptQueryDTO result{
      ReadIntent(reader), ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence-attempt query is invalid");
  }
  return result;
}

void WriteAttemptResult(
    Writer &writer,
    const core::ApplyFenceAcquireAttemptReconcileResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid fence-attempt reconciliation");
  }
  WriteExecutionHostEpoch(writer, result.execution_host_epoch);
  WriteAttemptQuery(writer, result.echoed_query);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional<core::ConfigurationFenceAcceptedDTO>(
      writer, result.accepted_fence,
      [&result](Writer &output, const auto &accepted) {
        WriteAccepted(output, accepted,
                      &result.echoed_query.persisted_intent.scope);
      });
  writer.Boolean(result.admission_fenced);
  writer.String(result.required_compensation_or_operator_direction);
  WriteStrong(writer, result.result_digest);
  WriteOptional<core::ErrorDTO>(writer, result.error, WriteError);
}

[[nodiscard]] core::ApplyFenceAcquireAttemptReconcileResultDTO
ReadAttemptResult(Reader &reader) {
  auto execution_host_epoch = ReadExecutionHostEpoch(reader);
  auto query = ReadAttemptQuery(reader);
  const auto state =
      static_cast<core::ApplyFenceAcquireAttemptReconcileState>(reader.Byte());
  auto accepted = ReadOptional<core::ConfigurationFenceAcceptedDTO>(
      reader, [&query](Reader &input) {
        return ReadAccepted(input, &query.persisted_intent.scope);
      });
  core::ApplyFenceAcquireAttemptReconcileResultDTO result{
      std::move(execution_host_epoch),
      std::move(query),
      state,
      std::move(accepted),
      reader.Boolean(),
      reader.String(),
      ReadStrong<core::ContentDigest>(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded fence-attempt reconciliation is invalid");
  }
  return result;
}

void WriteRehydrate(Writer &writer,
                    const core::ApplyFenceRehydrateDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid fence rehydration request");
  }
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteStrong(writer, request.request_id);
  WriteStrong(writer, request.request_owner_epoch);
  WriteStrong(writer, request.intent_digest);
  WriteStrong(writer, request.expected_old_retained_bundle_id);
  WriteStrong(writer, request.expected_old_graph_revision);
  WriteStrong(writer, request.expected_new_retained_bundle_id);
  WriteStrong(writer, request.expected_new_graph_revision);
  WriteStrong(writer, request.persisted_configuration_fence_id);
  WriteGeneration(writer, request.persisted_configuration_fence_generation);
  WriteScope(writer, request.persisted_scope);
  writer.String(request.last_durable_phase);
  WriteStrong(writer, request.manager_runtime_epoch);
}

[[nodiscard]] core::ApplyFenceRehydrateDTO ReadRehydrate(Reader &reader) {
  core::ApplyFenceRehydrateDTO result{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadStrong<core::RequestId>(reader),
      ReadStrong<core::RequestOwnerEpoch>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::RetainedBundleId>(reader),
      ReadStrong<core::GraphRevision>(reader),
      ReadStrong<core::RetainedBundleId>(reader),
      ReadStrong<core::GraphRevision>(reader),
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadScope(reader),
      reader.String(),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence rehydration request is invalid");
  }
  return result;
}

void WriteReconcileEntry(Writer &writer,
                         const core::ApplyFenceReconcileEntryDTO &entry) {
  if (!entry.presence_valid()) {
    Fail("cannot encode an invalid fence reconciliation entry");
  }
  WriteStrong(writer, entry.persisted_configuration_fence_id);
  WriteGeneration(writer, entry.persisted_configuration_fence_generation);
  writer.Byte(static_cast<std::uint8_t>(entry.state));
  WriteOptional<core::ConfigurationFenceId>(
      writer, entry.reconciled_configuration_fence_id,
      [](Writer &output, const auto &value) { WriteStrong(output, value); });
  WriteOptional<core::ConfigurationFenceGeneration>(
      writer, entry.reconciled_configuration_fence_generation,
      [](Writer &output, const auto value) { WriteGeneration(output, value); });
}

[[nodiscard]] core::ApplyFenceReconcileEntryDTO
ReadReconcileEntry(Reader &reader) {
  core::ApplyFenceReconcileEntryDTO result{
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      static_cast<core::ApplyFenceReconcileState>(reader.Byte()),
      ReadOptional<core::ConfigurationFenceId>(
          reader,
          [](Reader &input) {
            return ReadStrong<core::ConfigurationFenceId>(input);
          }),
      ReadOptional<core::ConfigurationFenceGeneration>(
          reader, [](Reader &input) {
            return ReadGeneration<core::ConfigurationFenceGeneration>(input);
          })};
  if (!result.presence_valid()) {
    Fail("decoded fence reconciliation entry is invalid");
  }
  return result;
}

void WriteReconcileResult(Writer &writer,
                          const core::ApplyFenceReconcileResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode an invalid fence rehydration result");
  }
  WriteExecutionHostEpoch(writer, result.execution_host_epoch);
  WriteVector(writer, result.fence_results, WriteReconcileEntry);
  writer.Boolean(result.admission_fenced);
  writer.String(result.required_compensation_or_operator_direction);
  WriteStrong(writer, result.result_digest);
  WriteOptional<core::ErrorDTO>(writer, result.error, WriteError);
}

[[nodiscard]] core::ApplyFenceReconcileResultDTO
ReadReconcileResult(Reader &reader) {
  core::ApplyFenceReconcileResultDTO result{
      ReadExecutionHostEpoch(reader),
      ReadVector<core::ApplyFenceReconcileEntryDTO>(reader, ReadReconcileEntry),
      reader.Boolean(),
      reader.String(),
      ReadStrong<core::ContentDigest>(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded fence rehydration result is invalid");
  }
  return result;
}

void WriteSelectedFenceOwner(Writer &writer,
                             const core::SelectedBringUpFenceOwnerDTO &owner) {
  if (!owner.valid())
    Fail("invalid selected fence owner");
  WriteStrong(writer, owner.lifecycle_transaction_id);
  WriteGeneration(writer, owner.lifecycle_transaction_generation);
  WriteGeneration(writer, owner.selected_claim_generation);
}
core::SelectedBringUpFenceOwnerDTO ReadSelectedFenceOwner(Reader &reader) {
  core::SelectedBringUpFenceOwnerDTO owner{
      ReadStrong<core::LifecycleTransactionId>(reader),
      ReadGeneration<core::LifecycleTransactionGeneration>(reader),
      ReadGeneration<core::CellAdmissionClaimGeneration>(reader)};
  if (!owner.valid())
    Fail("invalid selected fence owner");
  return owner;
}
void WriteSelectedIntent(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceIntentDTO &intent) {
  if (!intent.valid())
    Fail("invalid selected fence intent");
  WritePublicRequestContext(writer, intent.owner_request_context);
  WriteStrong(writer, intent.lifecycle_transaction_id);
  WriteGeneration(writer, intent.lifecycle_transaction_generation);
  writer.String(EncodeKernelCellAdmissionRpcResponse("cell_admission_acquire",
                                                     intent.selected_claim));
  WriteAdmissionSnapshot(writer, intent.expected_admission);
  WriteGeneration(writer, intent.expected_publication_generation);
  WriteScope(writer, intent.selected_scope);
  WriteProjectionRevision(writer, intent.candidate_projection_revision);
}
core::SelectedBringUpConfigurationFenceIntentDTO
ReadSelectedIntent(Reader &reader) {
  auto request = ReadPublicRequestContext(reader);
  auto transaction = ReadStrong<core::LifecycleTransactionId>(reader);
  auto generation =
      ReadGeneration<core::LifecycleTransactionGeneration>(reader);
  auto claim_packet = DecodeKernelCellAdmissionRpcResponse(reader.String());
  if (claim_packet.command != "cell_admission_acquire" ||
      !std::holds_alternative<core::CellAdmissionClaimDTO>(
          claim_packet.response))
    Fail("invalid selected claim record");
  core::SelectedBringUpConfigurationFenceIntentDTO intent{
      request,
      transaction,
      generation,
      std::get<core::CellAdmissionClaimDTO>(std::move(claim_packet.response)),
      ReadAdmissionSnapshot(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadScope(reader),
      ReadProjectionRevision(reader)};
  if (!intent.valid())
    Fail("invalid selected fence intent");
  return intent;
}
void WriteSelectedReconciliationEvidence(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO
        &evidence,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  if (!evidence.valid()) {
    Fail("cannot encode invalid fence reconciliation evidence");
  }
  if (shared_scope != nullptr && evidence.reconciled_scope != *shared_scope) {
    Fail("fence reconciliation evidence differs from its shared scope");
  }
  WriteExecutionHostEpoch(writer, evidence.execution_host_epoch);
  WriteSelectedFenceOwner(writer, evidence.owner_transaction);
  WriteStrong(writer, evidence.configuration_fence_id);
  WriteGeneration(writer, evidence.configuration_fence_generation);
  if (shared_scope == nullptr) {
    WriteScope(writer, evidence.reconciled_scope);
  }
  writer.U64(evidence.operation_resource_generation);
  WriteStrong(writer, evidence.active_graph_revision);
  WriteProjectionRevision(writer, evidence.active_projection_revision);
  WriteGeneration(writer, evidence.active_publication_generation);
  WriteStrong(writer, evidence.active_public_pointer_digest);
  WriteVector(writer, evidence.active_control_configurations,
              WriteActiveConfiguration);
  writer.U64(evidence.affected_queued_operation_count);
  writer.U64(evidence.affected_active_operation_count);
  WriteStrong(writer, evidence.evidence_digest);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO
ReadSelectedReconciliationEvidence(
    Reader &reader,
    const core::ConfigurationFenceScopeDTO *const shared_scope = nullptr) {
  core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO result{
      ReadExecutionHostEpoch(reader),
      ReadSelectedFenceOwner(reader),
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      shared_scope == nullptr ? ReadScope(reader) : *shared_scope,
      reader.U64(),
      ReadStrong<core::GraphRevision>(reader),
      ReadProjectionRevision(reader),
      ReadGeneration<core::ActiveBundlePublicationGeneration>(reader),
      ReadStrong<core::ContentDigest>(reader),
      ReadVector<core::DomainActiveControlConfigurationDTO>(
          reader, ReadActiveConfiguration),
      reader.U64(),
      reader.U64(),
      ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded fence reconciliation evidence is invalid");
  }
  return result;
}

void WriteSelectedRelease(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceReleaseDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid fence release request");
  }
  WriteSelectedReconciliationEvidence(writer, request.reconciliation_evidence);
  WriteStrong(writer, request.manager_runtime_epoch);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceReleaseDTO
ReadSelectedRelease(Reader &reader) {
  core::SelectedBringUpConfigurationFenceReleaseDTO result{
      ReadSelectedReconciliationEvidence(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence release request is invalid");
  }
  return result;
}

void WriteSelectedReleased(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceReleasedDTO &released) {
  if (!released.valid()) {
    Fail("cannot encode an invalid fence release receipt");
  }
  WriteSelectedRelease(writer, released.request);
  WriteExecutionHostEpoch(writer, released.execution_host_epoch);
  writer.I64(released.released_at_ns);
  WriteReplayStatus(writer, released.replay_status);
  WriteStrong(writer, released.release_receipt_digest);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceReleasedDTO
ReadSelectedReleased(Reader &reader) {
  core::SelectedBringUpConfigurationFenceReleasedDTO result{
      ReadSelectedRelease(reader), ReadExecutionHostEpoch(reader), reader.I64(),
      ReadReplayStatus(reader), ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded fence release receipt is invalid");
  }
  return result;
}

void WriteSelectedReleasedWithSharedEvidence(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceReleasedDTO &released,
    const core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO
        &shared_evidence) {
  if (!released.valid() ||
      released.request.reconciliation_evidence != shared_evidence ||
      released.execution_host_epoch != shared_evidence.execution_host_epoch) {
    Fail("fence release receipt differs from its shared status evidence");
  }
  WriteStrong(writer, released.request.manager_runtime_epoch);
  writer.I64(released.released_at_ns);
  WriteReplayStatus(writer, released.replay_status);
  WriteStrong(writer, released.release_receipt_digest);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceReleasedDTO
ReadSelectedReleasedWithSharedEvidence(
    Reader &reader,
    const core::SelectedBringUpConfigurationFenceReconciliationEvidenceDTO
        &shared_evidence) {
  core::SelectedBringUpConfigurationFenceReleasedDTO result{
      core::SelectedBringUpConfigurationFenceReleaseDTO{
          shared_evidence, ReadStrong<core::RuntimeEpoch>(reader)},
      shared_evidence.execution_host_epoch, reader.I64(),
      ReadReplayStatus(reader), ReadStrong<core::ContentDigest>(reader)};
  if (!result.valid()) {
    Fail("decoded shared-evidence fence release receipt is invalid");
  }
  return result;
}

void WriteSelectedStatusQuery(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceStatusQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode an invalid fence status query");
  }
  WriteSelectedFenceOwner(writer, query.owner_transaction);
  WriteStrong(writer, query.configuration_fence_id);
  WriteGeneration(writer, query.configuration_fence_generation);
  WriteScope(writer, query.expected_scope);
  WriteStrong(writer, query.manager_runtime_epoch);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceStatusQueryDTO
ReadSelectedStatusQuery(Reader &reader) {
  core::SelectedBringUpConfigurationFenceStatusQueryDTO result{
      ReadSelectedFenceOwner(reader),
      ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadScope(reader), ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence status query is invalid");
  }
  return result;
}

void WriteSelectedStatus(
    Writer &writer,
    const core::SelectedBringUpConfigurationFenceStatusDTO &status) {
  if (!status.presence_valid()) {
    Fail("cannot encode an invalid fence status");
  }
  WriteSelectedStatusQuery(writer, status.echoed_query);
  // The DTO validity relation proves this scope equals expected_scope. Keep
  // one copy on this private bounded wire and reconstruct the second value.
  WriteSelectedReconciliationEvidence(writer, status.reconciliation_evidence,
                                      &status.echoed_query.expected_scope);
  writer.Byte(static_cast<std::uint8_t>(status.state));
  writer.Boolean(status.admission_fenced);
  WriteOptional<core::SelectedBringUpConfigurationFenceReleasedDTO>(
      writer, status.release_receipt,
      [&status](Writer &output, const auto &released) {
        WriteSelectedReleasedWithSharedEvidence(output, released,
                                                status.reconciliation_evidence);
      });
  writer.String(status.required_compensation_or_operator_direction);
  WriteStrong(writer, status.result_digest);
  WriteOptional<core::ErrorDTO>(writer, status.error, WriteError);
}

[[nodiscard]] core::SelectedBringUpConfigurationFenceStatusDTO
ReadSelectedStatus(Reader &reader) {
  auto query = ReadSelectedStatusQuery(reader);
  auto evidence =
      ReadSelectedReconciliationEvidence(reader, &query.expected_scope);
  const auto state = static_cast<core::ConfigurationFenceState>(reader.Byte());
  const bool admission_fenced = reader.Boolean();
  auto release_receipt =
      ReadOptional<core::SelectedBringUpConfigurationFenceReleasedDTO>(
          reader, [&evidence](Reader &input) {
            return ReadSelectedReleasedWithSharedEvidence(input, evidence);
          });
  core::SelectedBringUpConfigurationFenceStatusDTO result{
      std::move(query),
      std::move(evidence),
      state,
      admission_fenced,
      std::move(release_receipt),
      reader.String(),
      ReadStrong<core::ContentDigest>(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded fence status is invalid");
  }
  return result;
}

void WriteSelectedAttemptQuery(
    Writer &writer,
    const core::SelectedBringUpFenceAcquireAttemptQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode an invalid fence-attempt query");
  }
  WriteSelectedIntent(writer, query.persisted_intent);
  WriteStrong(writer, query.persisted_attempt_digest);
  WriteStrong(writer, query.manager_runtime_epoch);
}

[[nodiscard]] core::SelectedBringUpFenceAcquireAttemptQueryDTO
ReadSelectedAttemptQuery(Reader &reader) {
  core::SelectedBringUpFenceAcquireAttemptQueryDTO result{
      ReadSelectedIntent(reader), ReadStrong<core::ContentDigest>(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!result.valid()) {
    Fail("decoded fence-attempt query is invalid");
  }
  return result;
}

void WriteSelectedAttemptResult(
    Writer &writer,
    const core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid fence-attempt reconciliation");
  }
  WriteExecutionHostEpoch(writer, result.execution_host_epoch);
  WriteSelectedAttemptQuery(writer, result.echoed_query);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional<core::ConfigurationFenceAcceptedDTO>(
      writer, result.accepted_fence,
      [&result](Writer &output, const auto &accepted) {
        WriteAccepted(output, accepted,
                      &result.echoed_query.persisted_intent.selected_scope);
      });
  writer.Boolean(result.admission_fenced);
  writer.String(result.required_compensation_or_operator_direction);
  WriteStrong(writer, result.result_digest);
  WriteOptional<core::ErrorDTO>(writer, result.error, WriteError);
}

[[nodiscard]] core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO
ReadSelectedAttemptResult(Reader &reader) {
  auto execution_host_epoch = ReadExecutionHostEpoch(reader);
  auto query = ReadSelectedAttemptQuery(reader);
  const auto state =
      static_cast<core::ApplyFenceAcquireAttemptReconcileState>(reader.Byte());
  auto accepted = ReadOptional<core::ConfigurationFenceAcceptedDTO>(
      reader, [&query](Reader &input) {
        return ReadAccepted(input, &query.persisted_intent.selected_scope);
      });
  core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO result{
      std::move(execution_host_epoch),
      std::move(query),
      state,
      std::move(accepted),
      reader.Boolean(),
      reader.String(),
      ReadStrong<core::ContentDigest>(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded fence-attempt reconciliation is invalid");
  }
  return result;
}

void WriteSelectedReleaseAttemptQuery(
    Writer &writer,
    const core::SelectedBringUpFenceReleaseAttemptQueryDTO &query) {
  if (!query.valid())
    Fail("invalid selected release attempt");
  WriteSelectedRelease(writer, query.persisted_request);
}
core::SelectedBringUpFenceReleaseAttemptQueryDTO
ReadSelectedReleaseAttemptQuery(Reader &reader) {
  return {ReadSelectedRelease(reader)};
}
void WriteSelectedReleaseAttemptResult(
    Writer &writer,
    const core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO &result) {
  if (!result.presence_valid())
    Fail("invalid selected release reconciliation");
  WriteSelectedReleaseAttemptQuery(writer, result.echoed_query);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional<core::SelectedBringUpConfigurationFenceReleasedDTO>(
      writer, result.release_receipt, WriteSelectedReleased);
  WriteOptional<core::SelectedBringUpConfigurationFenceStatusDTO>(
      writer, result.observed_status, WriteSelectedStatus);
  WriteOptional<core::ErrorDTO>(writer, result.error, WriteError);
}
core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO
ReadSelectedReleaseAttemptResult(Reader &reader) {
  core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO result{
      ReadSelectedReleaseAttemptQuery(reader),
      static_cast<core::SelectedBringUpFenceReleaseAttemptState>(reader.Byte()),
      ReadOptional<core::SelectedBringUpConfigurationFenceReleasedDTO>(
          reader, ReadSelectedReleased),
      ReadOptional<core::SelectedBringUpConfigurationFenceStatusDTO>(
          reader, ReadSelectedStatus),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid())
    Fail("invalid selected release reconciliation");
  return result;
}

enum class RequestKind : std::uint8_t {
  kObserveAdmission = 1U,
  kAcquire = 2U,
  kReconcileAttempt = 3U,
  kRehydrate = 4U,
  kQueryStatus = 5U,
  kRelease = 6U,
  kSelectedAcquire = 7U,
  kSelectedReconcileAttempt = 8U,
  kSelectedQueryStatus = 9U,
  kSelectedRelease = 10U,
  kSelectedReconcileRelease = 11U,

};

enum class ResponseKind : std::uint8_t {
  kAdmission = 1U,
  kAccepted = 2U,
  kAttemptReconciled = 3U,
  kRehydrated = 4U,
  kStatus = 5U,
  kReleased = 6U,
  kFailure = 7U,
  kSelectedAttemptReconciled = 8U,
  kSelectedStatus = 9U,
  kSelectedReleased = 10U,
  kSelectedReleaseReconciled = 11U,

};

[[nodiscard]] bool UsesProviderScope(const KernelApplyFenceRpcRequest &packet) {
  return std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T,
                                     core::ConfigurationFenceAdmissionQueryDTO>)
          return UsesProviderDomains(value.affected_scope);
        else if constexpr (std::is_same_v<
                               T, core::ApplyConfigurationFenceIntentDTO>)
          return UsesProviderDomains(value.scope);
        else if constexpr (std::is_same_v<
                               T, core::ApplyFenceAcquireAttemptQueryDTO>)
          return UsesProviderDomains(value.persisted_intent.scope);
        else if constexpr (std::is_same_v<T, core::ApplyFenceRehydrateDTO>)
          return UsesProviderDomains(value.persisted_scope);
        else if constexpr (std::is_same_v<
                               T, core::ConfigurationFenceStatusQueryDTO>)
          return UsesProviderDomains(value.expected_scope);
        else if constexpr (std::is_same_v<T,
                                          core::ConfigurationFenceReleaseDTO>)
          return UsesProviderDomains(
              value.reconciliation_evidence.reconciled_scope);
        else if constexpr (
            std::is_same_v<T, core::SelectedBringUpConfigurationFenceIntentDTO>)
          return UsesProviderDomains(value.selected_scope);
        else if constexpr (
            std::is_same_v<T, core::SelectedBringUpFenceAcquireAttemptQueryDTO>)
          return UsesProviderDomains(value.persisted_intent.selected_scope);
        else if constexpr (
            std::is_same_v<
                T, core::SelectedBringUpConfigurationFenceStatusQueryDTO>)
          return UsesProviderDomains(value.expected_scope);
        else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpConfigurationFenceReleaseDTO>)
          return UsesProviderDomains(
              value.reconciliation_evidence.reconciled_scope);
        else if constexpr (
            std::is_same_v<T, core::SelectedBringUpFenceReleaseAttemptQueryDTO>)
          return UsesProviderDomains(
              value.persisted_request.reconciliation_evidence.reconciled_scope);
        else
          return false;
      },
      packet);
}

[[nodiscard]] bool
UsesProviderScope(const KernelApplyFenceRpcResponse &packet) {
  return std::visit(
      [](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          T, core::ConfigurationFenceAdmissionSnapshotDTO>)
          return UsesProviderDomains(value.echoed_query.affected_scope);
        else if constexpr (std::is_same_v<T,
                                          core::ConfigurationFenceAcceptedDTO>)
          return UsesProviderDomains(value.captured_scope);
        else if constexpr (
            std::is_same_v<T, core::ApplyFenceAcquireAttemptReconcileResultDTO>)
          return UsesProviderDomains(value.echoed_query.persisted_intent.scope);
        else if constexpr (std::is_same_v<T, core::ConfigurationFenceStatusDTO>)
          return UsesProviderDomains(value.echoed_query.expected_scope);
        else if constexpr (std::is_same_v<T,
                                          core::ConfigurationFenceReleasedDTO>)
          return UsesProviderDomains(
              value.request.reconciliation_evidence.reconciled_scope);
        else if constexpr (
            std::is_same_v<
                T, core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>)
          return UsesProviderDomains(
              value.echoed_query.persisted_intent.selected_scope);
        else if constexpr (
            std::is_same_v<T, core::SelectedBringUpConfigurationFenceStatusDTO>)
          return UsesProviderDomains(value.echoed_query.expected_scope);
        else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpConfigurationFenceReleasedDTO>)
          return UsesProviderDomains(
              value.request.reconciliation_evidence.reconciled_scope);
        else if constexpr (
            std::is_same_v<
                T, core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>)
          return UsesProviderDomains(
              value.echoed_query.persisted_request.reconciliation_evidence
                  .reconciled_scope);
        else
          return false;
      },
      packet);
}

} // namespace

bool IsKernelApplyFenceRpcPacket(const std::string_view bytes) noexcept {
  if (bytes.size() < kMagic.size())
    return false;
  const auto magic = bytes.substr(0U, kMagic.size());
  return magic ==
             std::string_view(reinterpret_cast<const char *>(kMagic.data()),
                              kMagic.size()) ||
         magic == std::string_view(
                      reinterpret_cast<const char *>(kProviderMagic.data()),
                      kProviderMagic.size());
}

std::string
EncodeKernelApplyFenceRpcRequest(const KernelApplyFenceRpcRequest &request) {
  Writer writer(UsesProviderScope(request));
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          T, core::ConfigurationFenceAdmissionQueryDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(RequestKind::kObserveAdmission));
          WriteAdmissionQuery(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ApplyConfigurationFenceIntentDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kAcquire));
          WriteIntent(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ApplyFenceAcquireAttemptQueryDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(RequestKind::kReconcileAttempt));
          WriteAttemptQuery(writer, value);
        } else if constexpr (std::is_same_v<T, core::ApplyFenceRehydrateDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRehydrate));
          WriteRehydrate(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceStatusQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kQueryStatus));
          WriteStatusQuery(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpConfigurationFenceIntentDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kSelectedAcquire));
          WriteSelectedIntent(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpFenceAcquireAttemptQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(
              RequestKind::kSelectedReconcileAttempt));
          WriteSelectedAttemptQuery(writer, value);
        } else if constexpr (
            std::is_same_v<
                T, core::SelectedBringUpConfigurationFenceStatusQueryDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(RequestKind::kSelectedQueryStatus));
          WriteSelectedStatusQuery(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpConfigurationFenceReleaseDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kSelectedRelease));
          WriteSelectedRelease(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpFenceReleaseAttemptQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(
              RequestKind::kSelectedReconcileRelease));
          WriteSelectedReleaseAttemptQuery(writer, value);
        } else {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRelease));
          WriteRelease(writer, value);
        }
      },
      request);
  return std::move(writer).Finish();
}

KernelApplyFenceRpcRequest
DecodeKernelApplyFenceRpcRequest(const std::string_view bytes) {
  Reader reader(bytes);
  KernelApplyFenceRpcRequest result = [&]() -> KernelApplyFenceRpcRequest {
    switch (static_cast<RequestKind>(reader.Byte())) {
    case RequestKind::kObserveAdmission:
      return ReadAdmissionQuery(reader);
    case RequestKind::kAcquire:
      return ReadIntent(reader);
    case RequestKind::kReconcileAttempt:
      return ReadAttemptQuery(reader);
    case RequestKind::kRehydrate:
      return ReadRehydrate(reader);
    case RequestKind::kQueryStatus:
      return ReadStatusQuery(reader);
    case RequestKind::kRelease:
      return ReadRelease(reader);
    case RequestKind::kSelectedAcquire:
      return ReadSelectedIntent(reader);
    case RequestKind::kSelectedReconcileAttempt:
      return ReadSelectedAttemptQuery(reader);
    case RequestKind::kSelectedQueryStatus:
      return ReadSelectedStatusQuery(reader);
    case RequestKind::kSelectedRelease:
      return ReadSelectedRelease(reader);
    case RequestKind::kSelectedReconcileRelease:
      return ReadSelectedReleaseAttemptQuery(reader);
    }
    Fail("request kind is unknown");
  }();
  reader.Finish(UsesProviderScope(result));
  return result;
}

std::string
EncodeKernelApplyFenceRpcResponse(const KernelApplyFenceRpcResponse &response) {
  Writer writer(UsesProviderScope(response));
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<
                          T, core::ConfigurationFenceAdmissionSnapshotDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kAdmission));
          WriteAdmissionSnapshot(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceAcceptedDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kAccepted));
          WriteAccepted(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::ApplyFenceAcquireAttemptReconcileResultDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(ResponseKind::kAttemptReconciled));
          WriteAttemptResult(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ApplyFenceReconcileResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kRehydrated));
          WriteReconcileResult(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceStatusDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kStatus));
          WriteStatus(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceReleasedDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kReleased));
          WriteReleased(writer, value);
        } else if constexpr (
            std::is_same_v<
                T,
                core::SelectedBringUpFenceAcquireAttemptReconcileResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(
              ResponseKind::kSelectedAttemptReconciled));
          WriteSelectedAttemptResult(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::SelectedBringUpConfigurationFenceStatusDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kSelectedStatus));
          WriteSelectedStatus(writer, value);
        } else if constexpr (
            std::is_same_v<
                T, core::SelectedBringUpConfigurationFenceReleasedDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(ResponseKind::kSelectedReleased));
          WriteSelectedReleased(writer, value);
        } else if constexpr (
            std::is_same_v<
                T,
                core::SelectedBringUpFenceReleaseAttemptReconcileResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(
              ResponseKind::kSelectedReleaseReconciled));
          WriteSelectedReleaseAttemptResult(writer, value);
        } else {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kFailure));
          WriteError(writer, value.error);
        }
      },
      response);
  return std::move(writer).Finish();
}

KernelApplyFenceRpcResponse
DecodeKernelApplyFenceRpcResponse(const std::string_view bytes) {
  Reader reader(bytes);
  KernelApplyFenceRpcResponse result = [&]() -> KernelApplyFenceRpcResponse {
    switch (static_cast<ResponseKind>(reader.Byte())) {
    case ResponseKind::kAdmission:
      return ReadAdmissionSnapshot(reader);
    case ResponseKind::kAccepted:
      return ReadAccepted(reader);
    case ResponseKind::kAttemptReconciled:
      return ReadAttemptResult(reader);
    case ResponseKind::kRehydrated:
      return ReadReconcileResult(reader);
    case ResponseKind::kStatus:
      return ReadStatus(reader);
    case ResponseKind::kReleased:
      return ReadReleased(reader);
    case ResponseKind::kFailure:
      return KernelApplyFenceRpcFailure{ReadError(reader)};
    case ResponseKind::kSelectedAttemptReconciled:
      return ReadSelectedAttemptResult(reader);
    case ResponseKind::kSelectedStatus:
      return ReadSelectedStatus(reader);
    case ResponseKind::kSelectedReleased:
      return ReadSelectedReleased(reader);
    case ResponseKind::kSelectedReleaseReconciled:
      return ReadSelectedReleaseAttemptResult(reader);
    }
    Fail("response kind is unknown");
  }();
  reader.Finish(UsesProviderScope(result));
  return result;
}

} // namespace manyforge::component_manager::detail

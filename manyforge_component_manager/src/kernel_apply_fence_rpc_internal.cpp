#include "kernel_apply_fence_rpc_internal.hpp"
#include <manyforge_component_manager/kernel_cell_admission_rpc_internal.hpp>

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

// Version 3 (physical scopes) and 6 (provider scopes) carry the request
// key in place of the request id and owner epoch, numeric Apply ids, and
// no owner epoch in the execution-host epoch; 2 and 5 are refused.
constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'K',
                                              'A', 'F', 'R', '3'};
constexpr auto kProviderMagic = [] {
  auto value = kMagic;
  value.back() = '6';
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

// A request key (contract 1.4) as its two big-endian halves.
void WriteRequestKey(Writer &writer, const core::RequestKey &key) {
  writer.U64(key.high());
  writer.U64(key.low());
}

[[nodiscard]] core::RequestKey ReadRequestKey(Reader &reader) {
  const auto high = reader.U64();
  const auto low = reader.U64();
  core::RequestKey key;
  for (std::size_t index = 0U; index < 8U; ++index) {
    key.bytes[index] = static_cast<std::uint8_t>(high >> (56U - 8U * index));
    key.bytes[8U + index] = static_cast<std::uint8_t>(low >> (56U - 8U * index));
  }
  return key;
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
  WriteRequestKey(writer, context.request_key);
  writer.String(context.source_label);
}

[[nodiscard]] core::PublicRequestContextDTO
ReadPublicRequestContext(Reader &reader) {
  auto key = ReadRequestKey(reader);
  return {key, reader.String()};
}

[[nodiscard]] bool
UsesProviderDomains(const core::ConfigurationFenceScopeDTO &scope) {
  return std::any_of(
      scope.domains.begin(), scope.domains.end(), [](const auto &domain) {
        return std::holds_alternative<
            core::ProviderConfigurationFenceDomainDTO>(domain.payload);
      });
}

void WriteProviderOccurrence(
    Writer &writer, const core::ProviderConfigurationFenceOccurrenceDTO &value) {
  WriteStrong(writer, value.controllable_component_id);
  WriteStrong(writer, value.provider_instance_id);
  WriteStrong(writer, value.signal_configuration_digest);
  WriteVector(writer, value.output_resources,
              [](Writer &output, const auto &id) { WriteStrong(output, id); });
}

[[nodiscard]] core::ProviderConfigurationFenceOccurrenceDTO
ReadProviderOccurrence(Reader &reader) {
  return {ReadStrong<core::ControllableComponentId>(reader),
          ReadStrong<core::ProviderInstanceId>(reader),
          ReadStrong<core::ContentDigest>(reader),
          ReadVector<core::ResourceId>(reader, [](Reader &input) {
            return ReadStrong<core::ResourceId>(input);
          })};
}

/// Controller-only scopes keep the base packet version. A provider scope uses
/// the provider version, whose domains carry one kind byte and the provider
/// occurrences.
void WriteScope(Writer &writer, const core::ConfigurationFenceScopeDTO &scope) {
  if (!scope.valid()) {
    Fail("cannot encode an invalid configuration-fence scope");
  }
  if (!writer.ProviderFormat() && UsesProviderDomains(scope))
    Fail("provider scope requires the provider packet version");
  WriteVector(
      writer, scope.controllable_component_ids,
      [](Writer &output, const auto &value) { WriteStrong(output, value); });
  WriteVector(writer, scope.domains, [](Writer &output, const auto &value) {
    const auto *provider =
        std::get_if<core::ProviderConfigurationFenceDomainDTO>(&value.payload);
    if (output.ProviderFormat())
      output.Boolean(provider != nullptr);
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
    if (provider != nullptr) {
      WriteOptional<core::ProviderConfigurationFenceOccurrenceDTO>(
          output, provider->predecessor_provider, WriteProviderOccurrence);
      WriteOptional<core::ProviderConfigurationFenceOccurrenceDTO>(
          output, provider->candidate_provider, WriteProviderOccurrence);
    }
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
  core::ConfigurationFenceScopeDTO result{
      ReadVector<core::ControllableComponentId>(
          reader,
          [](Reader &input) {
            return ReadStrong<core::ControllableComponentId>(input);
          }),
      ReadVector<core::ConfigurationFenceDomainScopeDTO>(
          reader,
          [](Reader &input) {
            const bool provider = input.ProviderFormat() && input.Boolean();
            core::ConfigurationFenceDomainScopeDTO domain{
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
            if (provider) {
              auto before = ReadOptional<core::ProviderConfigurationFenceOccurrenceDTO>(
                  input, ReadProviderOccurrence);
              auto after = ReadOptional<core::ProviderConfigurationFenceOccurrenceDTO>(
                  input, ReadProviderOccurrence);
              domain.payload = core::ProviderConfigurationFenceDomainDTO{
                  std::move(before), std::move(after)};
            }
            return domain;
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
  WriteGeneration(writer, owner.apply_id);
  WriteGeneration(writer, owner.apply_generation);
}

[[nodiscard]] core::ApplyFenceOwnerDTO ReadFenceOwner(Reader &reader) {
  core::ApplyFenceOwnerDTO result{
      ReadGeneration<core::ApplyId>(reader),
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
  WriteStrong(writer, epoch.durable_stop_fence_ledger_digest);
  WriteGeneration(writer, epoch.publication_generation);
}

[[nodiscard]] core::ExecutionHostEpochDTO
ReadExecutionHostEpoch(Reader &reader) {
  core::ExecutionHostEpochDTO result{
      ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader),
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
}

[[nodiscard]] core::ConfigurationFenceAdmissionQueryDTO
ReadAdmissionQuery(Reader &reader) {
  core::ConfigurationFenceAdmissionQueryDTO result{ReadScope(reader)};
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
}

[[nodiscard]] core::ConfigurationFenceReleaseDTO ReadRelease(Reader &reader) {
  core::ConfigurationFenceReleaseDTO result{ReadReconciliationEvidence(reader)};
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
  writer.I64(released.released_at_ns);
  WriteReplayStatus(writer, released.replay_status);
  WriteStrong(writer, released.release_receipt_digest);
}

[[nodiscard]] core::ConfigurationFenceReleasedDTO
ReadReleasedWithSharedEvidence(
    Reader &reader,
    const core::ConfigurationFenceReconciliationEvidenceDTO &shared_evidence) {
  core::ConfigurationFenceReleasedDTO result{
      core::ConfigurationFenceReleaseDTO{shared_evidence},
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
}

[[nodiscard]] core::ConfigurationFenceStatusQueryDTO
ReadStatusQuery(Reader &reader) {
  core::ConfigurationFenceStatusQueryDTO result{
      ReadFenceOwner(reader), ReadStrong<core::ConfigurationFenceId>(reader),
      ReadGeneration<core::ConfigurationFenceGeneration>(reader),
      ReadScope(reader)};
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
}

[[nodiscard]] core::ApplyFenceAcquireAttemptQueryDTO
ReadAttemptQuery(Reader &reader) {
  core::ApplyFenceAcquireAttemptQueryDTO result{
      ReadIntent(reader), ReadStrong<core::ContentDigest>(reader)};
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

enum class RequestKind : std::uint8_t {
  kObserveAdmission = 1U,
  kAcquire = 2U,
  kReconcileAttempt = 3U,
  kQueryStatus = 5U,
  kRelease = 6U,
};

enum class ResponseKind : std::uint8_t {
  kAdmission = 1U,
  kAccepted = 2U,
  kAttemptReconciled = 3U,
  kStatus = 5U,
  kReleased = 6U,
  kFailure = 7U,
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
        else if constexpr (std::is_same_v<
                               T, core::ConfigurationFenceStatusQueryDTO>)
          return UsesProviderDomains(value.expected_scope);
        else if constexpr (std::is_same_v<T,
                                          core::ConfigurationFenceReleaseDTO>)
          return UsesProviderDomains(
              value.reconciliation_evidence.reconciled_scope);
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
        else
          return false;
      },
      packet);
}

} // namespace

std::string EncodeConfigurationFenceScopeRecord(
    const core::ConfigurationFenceScopeDTO &scope) {
  Writer writer(true);
  WriteScope(writer, scope);
  return std::move(writer).Finish();
}

core::ConfigurationFenceScopeDTO
DecodeConfigurationFenceScopeRecord(const std::string_view bytes) {
  Reader reader(bytes);
  if (!reader.ProviderFormat())
    Fail("configuration-fence scope record version differs");
  auto scope = ReadScope(reader);
  reader.Finish(true);
  return scope;
}

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

namespace {

void WriteRequestBody(Writer &writer, const KernelApplyFenceRpcRequest &request) {
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
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceStatusQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kQueryStatus));
          WriteStatusQuery(writer, value);
        } else {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRelease));
          WriteRelease(writer, value);
        }
      },
      request);
}

[[nodiscard]] KernelApplyFenceRpcRequest ReadRequestBody(Reader &reader) {
  return [&]() -> KernelApplyFenceRpcRequest {
    switch (static_cast<RequestKind>(reader.Byte())) {
    case RequestKind::kObserveAdmission:
      return ReadAdmissionQuery(reader);
    case RequestKind::kAcquire:
      return ReadIntent(reader);
    case RequestKind::kReconcileAttempt:
      return ReadAttemptQuery(reader);
    case RequestKind::kQueryStatus:
      return ReadStatusQuery(reader);
    case RequestKind::kRelease:
      return ReadRelease(reader);
    }
    Fail("request kind is unknown");
  }();
}

// The candidate bundle and graph are the projection's own, so they are not
// written twice.
void WritePublicationReceipt(Writer &writer,
                             const core::ActiveBundlePublicationReceiptDTO &receipt) {
  if (!receipt.valid())
    Fail("cannot encode an invalid active publication");
  const auto &request = receipt.request;
  if (const auto *apply = std::get_if<core::NamedApplyPublicationTransactionDTO>(
          &request.transaction_ref)) {
    writer.Byte(1U);
    WriteGeneration(writer, apply->apply_id);
    WriteGeneration(writer, apply->apply_generation);
  } else {
    const auto &bootstrap =
        std::get<core::BootstrapPublicationTransactionDTO>(request.transaction_ref);
    writer.Byte(2U);
    WriteStrong(writer, bootstrap.bootstrap_id);
    WriteGeneration(writer, bootstrap.bootstrap_generation);
  }
  WriteProjectionRevision(writer, request.candidate_projection_revision);
  WriteOptional(writer, request.expected_previous_publication_generation,
                [](Writer &out, const auto &generation) { WriteGeneration(out, generation); });
  WriteStrong(writer, request.private_materialization_content_digest);
  WriteGeneration(writer, receipt.publication_generation);
  WriteStrong(writer, receipt.durable_public_pointer_content_digest);
}

[[nodiscard]] core::ActiveBundlePublicationReceiptDTO
ReadPublicationReceipt(Reader &reader) {
  auto transaction = [&]() -> core::ActiveBundlePublicationTransactionRefDTO {
    switch (reader.Byte()) {
    case 1U: {
      const auto apply_id = ReadGeneration<core::ApplyId>(reader);
      return core::NamedApplyPublicationTransactionDTO{
          apply_id, ReadGeneration<core::ApplyGeneration>(reader)};
    }
    case 2U: {
      auto bootstrap_id = ReadStrong<core::BootstrapId>(reader);
      return core::BootstrapPublicationTransactionDTO{
          std::move(bootstrap_id), ReadGeneration<core::BootstrapGeneration>(reader)};
    }
    default:
      Fail("active publication transaction kind is unknown");
    }
  }();
  const auto projection = ReadProjectionRevision(reader);
  auto previous = ReadOptional<core::ActiveBundlePublicationGeneration>(
      reader, [](Reader &in) { return ReadGeneration<core::ActiveBundlePublicationGeneration>(in); });
  auto materialization = ReadStrong<core::ContentDigest>(reader);
  const auto generation = ReadGeneration<core::ActiveBundlePublicationGeneration>(reader);
  core::ActiveBundlePublicationReceiptDTO receipt{
      {std::move(transaction), projection, projection.retained_bundle_id,
       projection.graph_revision, previous, std::move(materialization)},
      generation,
      ReadStrong<core::ContentDigest>(reader)};
  if (!receipt.valid())
    Fail("decoded active publication is invalid");
  return receipt;
}

} // namespace

std::string
EncodeKernelApplyFenceRpcRequest(const KernelApplyFenceRpcRequest &request) {
  Writer writer(UsesProviderScope(request));
  WriteRequestBody(writer, request);
  return std::move(writer).Finish();
}

KernelApplyFenceRpcRequest
DecodeKernelApplyFenceRpcRequest(const std::string_view bytes) {
  Reader reader(bytes);
  auto result = ReadRequestBody(reader);
  reader.Finish(UsesProviderScope(result));
  return result;
}

std::string
EncodeKernelApplyFenceRpcPacket(const KernelApplyFenceRpcPacket &packet) {
  if (std::holds_alternative<core::ConfigurationFenceReleaseDTO>(packet.request) ==
      packet.active_publication.has_value())
    Fail("every request except Release carries the active publication");
  Writer writer(UsesProviderScope(packet.request));
  WriteRequestBody(writer, packet.request);
  WriteOptional(writer, packet.active_publication, WritePublicationReceipt);
  return std::move(writer).Finish();
}

KernelApplyFenceRpcPacket
DecodeKernelApplyFenceRpcPacket(const std::string_view bytes) {
  Reader reader(bytes);
  KernelApplyFenceRpcPacket packet{
      ReadRequestBody(reader),
      ReadOptional<core::ActiveBundlePublicationReceiptDTO>(reader,
                                                            ReadPublicationReceipt)};
  if (std::holds_alternative<core::ConfigurationFenceReleaseDTO>(packet.request) ==
      packet.active_publication.has_value())
    Fail("every request except Release carries the active publication");
  reader.Finish(UsesProviderScope(packet.request));
  return packet;
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
                                 T, core::ConfigurationFenceStatusDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kStatus));
          WriteStatus(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::ConfigurationFenceReleasedDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kReleased));
          WriteReleased(writer, value);
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
    case ResponseKind::kStatus:
      return ReadStatus(reader);
    case ResponseKind::kReleased:
      return ReadReleased(reader);
    case ResponseKind::kFailure:
      return KernelApplyFenceRpcFailure{ReadError(reader)};
    }
    Fail("response kind is unknown");
  }();
  reader.Finish(UsesProviderScope(result));
  return result;
}

} // namespace manyforge::component_manager::detail

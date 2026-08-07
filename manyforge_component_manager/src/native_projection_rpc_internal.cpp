#include "native_projection_rpc_internal.hpp"

#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <manyforge_scene/logical_scene_value_codec.hpp>

namespace manyforge::component_manager::detail {
namespace {

namespace core = manyforge::core;
namespace scene = manyforge::scene;

constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'P',
                                              'R', 'P', 'C', '1'};
constexpr std::size_t kMaximumCollectionEntries = 4096U;

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("native projection RPC codec: " + detail);
}

class Writer final {
public:
  Writer() {
    bytes_.reserve(1024U);
    for (const auto byte : kMagic) {
      Byte(byte);
    }
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

  [[nodiscard]] std::string Finish() && {
    if (bytes_.size() > kMaximumNativeProjectionRpcPacketBytes) {
      Fail("encoded packet exceeds its byte bound");
    }
    return std::move(bytes_);
  }

private:
  void RequireSpace(const std::size_t additional) const {
    if (additional > kMaximumNativeProjectionRpcPacketBytes - bytes_.size()) {
      Fail("encoded packet exceeds its byte bound");
    }
  }

  std::string bytes_;
};

class Reader final {
public:
  explicit Reader(const std::string_view bytes) : bytes_(bytes) {
    if (bytes_.empty() ||
        bytes_.size() > kMaximumNativeProjectionRpcPacketBytes) {
      Fail("packet is empty or oversized");
    }
    for (const auto expected : kMagic) {
      if (Byte() != expected) {
        Fail("packet magic or version differs");
      }
    }
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

  void Finish() const {
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
  writer.String(error.code);
  writer.String(error.message);
  if (error.details.size() > kMaximumCollectionEntries) {
    Fail("error detail map exceeds its entry bound");
  }
  writer.U32(static_cast<std::uint32_t>(error.details.size()));
  for (const auto &[name, value] : error.details) {
    writer.String(name);
    writer.String(value);
  }
}

[[nodiscard]] core::ErrorDTO ReadError(Reader &reader) {
  core::ErrorDTO result{reader.String(), reader.String(), {}};
  const auto size = static_cast<std::size_t>(reader.U32());
  if (size > kMaximumCollectionEntries) {
    Fail("error detail map exceeds its entry bound");
  }
  for (std::size_t index = 0U; index < size; ++index) {
    auto name = reader.String();
    auto value = reader.String();
    if (!result.details.emplace(std::move(name), std::move(value)).second) {
      Fail("error detail map repeats a key");
    }
  }
  if (result.code.empty() || result.message.empty()) {
    Fail("error value is incomplete");
  }
  return result;
}

void WriteSceneRevision(Writer &writer, const core::SceneRevision &revision) {
  WriteStrong(writer, revision.scene_id);
  writer.U64(revision.revision_id);
  WriteStrong(writer, revision.content_hash);
}

[[nodiscard]] core::SceneRevision ReadSceneRevision(Reader &reader) {
  core::SceneRevision revision{ReadStrong<core::SceneId>(reader), reader.U64(),
                               ReadStrong<core::ContentDigest>(reader)};
  if (!revision.valid()) {
    Fail("scene revision is invalid");
  }
  return revision;
}

void WriteProjectionRevision(Writer &writer,
                             const core::ProjectionRevisionDTO &revision) {
  WriteStrong(writer, revision.retained_bundle_id);
  WriteStrong(writer, revision.graph_revision);
  WriteSceneRevision(writer, revision.scene_revision);
}

[[nodiscard]] core::ProjectionRevisionDTO
ReadProjectionRevision(Reader &reader) {
  core::ProjectionRevisionDTO revision{
      ReadStrong<core::RetainedBundleId>(reader),
      ReadStrong<core::GraphRevision>(reader), ReadSceneRevision(reader)};
  if (!revision.valid()) {
    Fail("projection revision is invalid");
  }
  return revision;
}

void WriteBinding(Writer &writer,
                  const core::ProjectionNativeRuntimeBindingDTO &binding) {
  WriteStrong(writer, binding.provider_binding_id);
  WriteStrong(writer, binding.provider_binding_revision);
  WriteStrong(writer, binding.runtime_unit_id);
  WriteStrong(writer, binding.runtime_epoch);
}

[[nodiscard]] core::ProjectionNativeRuntimeBindingDTO
ReadBinding(Reader &reader) {
  core::ProjectionNativeRuntimeBindingDTO binding{
      ReadStrong<core::CapabilityProviderBindingId>(reader),
      ReadStrong<core::CapabilityProviderBindingRevision>(reader),
      ReadStrong<core::RuntimeUnitId>(reader),
      ReadStrong<core::RuntimeEpoch>(reader)};
  if (!binding.valid()) {
    Fail("native runtime binding is invalid");
  }
  return binding;
}

void WriteArtifact(Writer &writer,
                   const core::RetainedArtifactRefDTO &artifact) {
  WriteStrong(writer, artifact.artifact_role_id);
  writer.String(artifact.media_type);
  writer.U64(artifact.byte_size);
  WriteStrong(writer, artifact.content_digest);
  WriteStrong(writer, artifact.content_addressed_store_key);
}

[[nodiscard]] core::RetainedArtifactRefDTO ReadArtifact(Reader &reader) {
  return {ReadStrong<core::ArtifactRoleId>(reader), reader.String(),
          reader.U64(), ReadStrong<core::ContentDigest>(reader),
          ReadStrong<core::ContentAddressedStoreKey>(reader)};
}

void WriteProduct(Writer &writer, const core::RetainedProductDTO &product) {
  WriteStrong(writer, product.product_id);
  writer.Byte(static_cast<std::uint8_t>(product.product_kind));
  WriteStrong(writer, product.product_contract_id);
  WriteStrong(writer, product.product_contract_revision);
  WriteVector(writer, product.artifact_refs, WriteArtifact);
}

[[nodiscard]] core::RetainedProductKind ReadProductKind(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::RetainedProductKind::kPlanningCollisionModel;
  case 1U:
    return core::RetainedProductKind::kViewerModel;
  case 2U:
    return core::RetainedProductKind::kControlDomainControl;
  case 3U:
    return core::RetainedProductKind::kSymbolRoleMap;
  case 4U:
    return core::RetainedProductKind::kRuntimeUnitConfiguration;
  default:
    Fail("retained product kind is unknown");
  }
}

[[nodiscard]] core::RetainedProductDTO ReadProduct(Reader &reader) {
  return {ReadStrong<core::ProductId>(reader), ReadProductKind(reader),
          ReadStrong<core::ProductContractId>(reader),
          ReadStrong<core::ProductContractRevision>(reader),
          ReadVector<core::RetainedArtifactRefDTO>(reader, ReadArtifact)};
}

void WriteStagedCandidate(Writer &writer,
                          const core::ProjectionStagedCandidateDTO &value) {
  WriteStrong(writer, value.apply_id);
  WriteGeneration(writer, value.apply_generation);
  WriteGeneration(writer, value.projection_stage_generation);
  WriteProjectionRevision(writer, value.candidate_projection_revision);
}

[[nodiscard]] core::ProjectionStagedCandidateDTO
ReadStagedCandidate(Reader &reader) {
  core::ProjectionStagedCandidateDTO value{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadProjectionRevision(reader)};
  if (!value.valid()) {
    Fail("staged-candidate health value is invalid");
  }
  return value;
}

void WriteHealthState(Writer &writer, const core::ProjectionHealthState state) {
  switch (state) {
  case core::ProjectionHealthState::kUninitialized:
    writer.Byte(0U);
    return;
  case core::ProjectionHealthState::kHealthy:
    writer.Byte(1U);
    return;
  case core::ProjectionHealthState::kDesynchronized:
    writer.Byte(2U);
    return;
  case core::ProjectionHealthState::kRebuilding:
    writer.Byte(3U);
    return;
  case core::ProjectionHealthState::kUnavailable:
    writer.Byte(4U);
    return;
  }
  Fail("projection health state is unknown");
}

[[nodiscard]] core::ProjectionHealthState ReadHealthState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::ProjectionHealthState::kUninitialized;
  case 1U:
    return core::ProjectionHealthState::kHealthy;
  case 2U:
    return core::ProjectionHealthState::kDesynchronized;
  case 3U:
    return core::ProjectionHealthState::kRebuilding;
  case 4U:
    return core::ProjectionHealthState::kUnavailable;
  default:
    Fail("projection health state is unknown");
  }
}

void WriteHealth(Writer &writer, const core::ProjectionHealthDTO &health) {
  if (!health.valid()) {
    Fail("cannot encode invalid projection health");
  }
  WriteBinding(writer, health.native_runtime_binding);
  writer.U64(health.source_sequence);
  WriteHealthState(writer, health.state);
  WriteOptional(writer, health.active_projection_revision,
                WriteProjectionRevision);
  WriteOptional(writer, health.staged_candidate, WriteStagedCandidate);
  WriteOptional(
      writer, health.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, health.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteOptional(writer, health.error, WriteError);
}

[[nodiscard]] core::ProjectionHealthDTO ReadHealth(Reader &reader) {
  core::ProjectionHealthDTO health{
      ReadBinding(reader),
      reader.U64(),
      ReadHealthState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<core::ProjectionStagedCandidateDTO>(reader,
                                                       ReadStagedCandidate),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!health.valid()) {
    Fail("decoded projection health is invalid");
  }
  return health;
}

void WriteDisposition(Writer &writer,
                      const core::ProjectionTransitionDisposition value) {
  switch (value) {
  case core::ProjectionTransitionDisposition::kRetained:
    writer.Byte(0U);
    return;
  case core::ProjectionTransitionDisposition::kAdded:
    writer.Byte(1U);
    return;
  case core::ProjectionTransitionDisposition::kRemoved:
    writer.Byte(2U);
    return;
  }
  Fail("projection transition disposition is unknown");
}

[[nodiscard]] core::ProjectionTransitionDisposition
ReadDisposition(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::ProjectionTransitionDisposition::kRetained;
  case 1U:
    return core::ProjectionTransitionDisposition::kAdded;
  case 2U:
    return core::ProjectionTransitionDisposition::kRemoved;
  default:
    Fail("projection transition disposition is unknown");
  }
}

void WriteTransitionRow(Writer &writer,
                        const core::ProjectionTransitionRowDTO &row) {
  if (!row.valid()) {
    Fail("cannot encode invalid projection transition row");
  }
  WriteBinding(writer, row.native_runtime_binding);
  WriteDisposition(writer, row.disposition);
  WriteOptional(writer, row.expected_local_active, WriteProjectionRevision);
}

[[nodiscard]] core::ProjectionTransitionRowDTO
ReadTransitionRow(Reader &reader) {
  core::ProjectionTransitionRowDTO row{
      ReadBinding(reader), ReadDisposition(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader,
                                                ReadProjectionRevision)};
  if (!row.valid()) {
    Fail("decoded projection transition row is invalid");
  }
  return row;
}

void WritePrepare(Writer &writer,
                  const NativeProjectionPrepareRequest &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid candidate preparation request");
  }
  WriteBinding(writer, request.native_runtime_binding);
  WriteStrong(writer, request.candidate_retained_bundle_id);
  WriteStrong(writer, request.candidate_graph_revision);
  WriteProduct(writer, request.planning_collision_model);
}

[[nodiscard]] NativeProjectionPrepareRequest ReadPrepare(Reader &reader) {
  NativeProjectionPrepareRequest request{
      ReadBinding(reader), ReadStrong<core::RetainedBundleId>(reader),
      ReadStrong<core::GraphRevision>(reader), ReadProduct(reader)};
  if (!request.valid()) {
    Fail("decoded candidate preparation request is invalid");
  }
  return request;
}

void WriteRebuild(Writer &writer,
                  const core::NativeProjectionRebuildDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid projection rebuild request");
  }
  WriteBinding(writer, request.expected_native_runtime_binding);
  WriteProjectionRevision(writer, request.target_projection_revision);
  WriteProduct(writer, request.planning_collision_model);
  writer.String(scene::EncodeLogicalSceneSnapshot(*request.logical_scene));
}

[[nodiscard]] core::NativeProjectionRebuildDTO ReadRebuild(Reader &reader) {
  core::NativeProjectionRebuildDTO request{
      ReadBinding(reader), ReadProjectionRevision(reader), ReadProduct(reader),
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          scene::DecodeLogicalSceneSnapshot(reader.String()))};
  if (!request.valid()) {
    Fail("decoded projection rebuild request is invalid");
  }
  return request;
}

void WriteBootstrapRebuild(
    Writer &writer, const core::NativeProjectionBootstrapRebuildDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid bootstrap rebuild request");
  }
  WriteStrong(writer, request.bootstrap_id);
  WriteGeneration(writer, request.bootstrap_generation);
  WriteBinding(writer, request.native_runtime_binding);
  WriteProjectionRevision(writer, request.canonical_empty_target);
  WriteProduct(writer, request.planning_collision_model);
  writer.String(scene::EncodeLogicalSceneSnapshot(
      *request.initialized_logical_scene.shared()));
}

[[nodiscard]] core::NativeProjectionBootstrapRebuildDTO
ReadBootstrapRebuild(Reader &reader) {
  core::NativeProjectionBootstrapRebuildDTO request{
      ReadStrong<core::BootstrapId>(reader),
      ReadGeneration<core::BootstrapGeneration>(reader),
      ReadBinding(reader),
      ReadProjectionRevision(reader),
      ReadProduct(reader),
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              scene::DecodeLogicalSceneSnapshot(reader.String())))};
  if (!request.valid()) {
    Fail("decoded bootstrap rebuild request is invalid");
  }
  return request;
}

void WriteBootstrapQuery(
    Writer &writer,
    const core::NativeProjectionBootstrapStatusQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode invalid bootstrap status query");
  }
  WriteStrong(writer, query.bootstrap_id);
  WriteGeneration(writer, query.bootstrap_generation);
  WriteBinding(writer, query.native_runtime_binding);
  WriteProjectionRevision(writer, query.canonical_empty_target);
}

[[nodiscard]] core::NativeProjectionBootstrapStatusQueryDTO
ReadBootstrapQuery(Reader &reader) {
  core::NativeProjectionBootstrapStatusQueryDTO query{
      ReadStrong<core::BootstrapId>(reader),
      ReadGeneration<core::BootstrapGeneration>(reader), ReadBinding(reader),
      ReadProjectionRevision(reader)};
  if (!query.valid()) {
    Fail("decoded bootstrap status query is invalid");
  }
  return query;
}

void WriteStage(Writer &writer, const core::NativeProjectionStageDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid projection stage request");
  }
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteGeneration(writer, request.projection_stage_generation);
  WriteTransitionRow(writer, request.transition_row);
  WriteProjectionRevision(writer,
                          request.expected_predecessor_projection_revision);
  WriteProjectionRevision(writer, request.candidate_projection_revision);
  WriteProduct(writer, request.planning_collision_model);
  writer.String(scene::EncodeLogicalSceneSnapshot(
      *request.frozen_logical_scene.shared()));
}

[[nodiscard]] core::NativeProjectionStageDTO ReadStage(Reader &reader) {
  core::NativeProjectionStageDTO request{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader),
      ReadProjectionRevision(reader),
      ReadProjectionRevision(reader),
      ReadProduct(reader),
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              scene::DecodeLogicalSceneSnapshot(reader.String())))};
  if (!request.valid()) {
    Fail("decoded projection stage request is invalid");
  }
  return request;
}

void WriteActivation(Writer &writer,
                     const core::NativeProjectionActivationDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid projection activation request");
  }
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteGeneration(writer, request.projection_stage_generation);
  WriteTransitionRow(writer, request.transition_row);
  WriteProjectionRevision(writer,
                          request.expected_predecessor_projection_revision);
  WriteProjectionRevision(writer, request.candidate_projection_revision);
}

[[nodiscard]] core::NativeProjectionActivationDTO
ReadActivation(Reader &reader) {
  core::NativeProjectionActivationDTO request{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader),
      ReadProjectionRevision(reader),
      ReadProjectionRevision(reader)};
  if (!request.valid()) {
    Fail("decoded projection activation request is invalid");
  }
  return request;
}

void WriteRetire(Writer &writer,
                 const core::NativeProjectionRetireDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid projection retirement request");
  }
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteGeneration(writer, request.projection_stage_generation);
  WriteTransitionRow(writer, request.transition_row);
  WriteProjectionRevision(writer,
                          request.expected_predecessor_projection_revision);
  WriteProjectionRevision(writer, request.candidate_projection_revision);
}

[[nodiscard]] core::NativeProjectionRetireDTO ReadRetire(Reader &reader) {
  core::NativeProjectionRetireDTO request{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader),
      ReadProjectionRevision(reader),
      ReadProjectionRevision(reader)};
  if (!request.valid()) {
    Fail("decoded projection retirement request is invalid");
  }
  return request;
}

void WriteDiscard(Writer &writer,
                  const core::NativeProjectionDiscardDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode invalid projection discard request");
  }
  WriteStrong(writer, request.apply_id);
  WriteGeneration(writer, request.apply_generation);
  WriteGeneration(writer, request.projection_stage_generation);
  WriteTransitionRow(writer, request.transition_row);
  WriteProjectionRevision(writer, request.candidate_projection_revision);
}

[[nodiscard]] core::NativeProjectionDiscardDTO ReadDiscard(Reader &reader) {
  core::NativeProjectionDiscardDTO request{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader), ReadProjectionRevision(reader)};
  if (!request.valid()) {
    Fail("decoded projection discard request is invalid");
  }
  return request;
}

void WriteStatusQuery(Writer &writer,
                      const core::NativeProjectionStatusQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode invalid projection status query");
  }
  WriteStrong(writer, query.apply_id);
  WriteGeneration(writer, query.apply_generation);
  WriteGeneration(writer, query.projection_stage_generation);
  WriteTransitionRow(writer, query.transition_row);
}

[[nodiscard]] core::NativeProjectionStatusQueryDTO
ReadStatusQuery(Reader &reader) {
  core::NativeProjectionStatusQueryDTO query{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader)};
  if (!query.valid()) {
    Fail("decoded projection status query is invalid");
  }
  return query;
}

void WriteProjectionCommit(Writer &writer,
                           const core::ProjectionCommitResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid projection commit result");
  }
  WriteOptional(
      writer, result.transaction_sequence,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteProjectionRevision(writer, result.proposed_projection_revision);
  writer.Byte(static_cast<std::uint8_t>(result.result));
  WriteOptional(writer, result.acknowledged_projection_revision,
                WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  writer.Boolean(result.rollback_proven);
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::ProjectionCommitResultStateDTO
ReadProjectionCommitState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::ProjectionCommitResultStateDTO::kCommitted;
  case 1U:
    return core::ProjectionCommitResultStateDTO::kRejectedRollbackProven;
  case 2U:
    return core::ProjectionCommitResultStateDTO::kFailedProjectionUncertain;
  default:
    Fail("projection commit result state is unknown");
  }
}

[[nodiscard]] core::ProjectionCommitResultDTO
ReadProjectionCommit(Reader &reader) {
  core::ProjectionCommitResultDTO result{
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadProjectionRevision(reader),
      ReadProjectionCommitState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      reader.Boolean(),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection commit result is invalid");
  }
  return result;
}

void WriteBootstrapRebuildResult(
    Writer &writer,
    const core::NativeProjectionBootstrapRebuildResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid bootstrap rebuild result");
  }
  WriteStrong(writer, result.bootstrap_id);
  WriteGeneration(writer, result.bootstrap_generation);
  WriteBinding(writer, result.native_runtime_binding);
  WriteProjectionRevision(writer, result.canonical_empty_target);
  WriteProduct(writer, result.planning_collision_model);
  writer.String(scene::EncodeLogicalSceneSnapshot(
      *result.initialized_logical_scene.shared()));
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional(writer, result.acknowledged_target, WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, result.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::NativeProjectionBootstrapRebuildState
ReadBootstrapRebuildState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionBootstrapRebuildState::kRebuilt;
  case 1U:
    return core::NativeProjectionBootstrapRebuildState::kAlreadyExact;
  case 2U:
    return core::NativeProjectionBootstrapRebuildState::
        kFailedProjectionUncertain;
  default:
    Fail("bootstrap rebuild result state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionBootstrapRebuildResultDTO
ReadBootstrapRebuildResult(Reader &reader) {
  core::NativeProjectionBootstrapRebuildResultDTO result{
      ReadStrong<core::BootstrapId>(reader),
      ReadGeneration<core::BootstrapGeneration>(reader),
      ReadBinding(reader),
      ReadProjectionRevision(reader),
      ReadProduct(reader),
      core::LogicalSceneSnapshotRoot(
          std::make_shared<const core::LogicalSceneSnapshotDTO>(
              scene::DecodeLogicalSceneSnapshot(reader.String()))),
      ReadBootstrapRebuildState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded bootstrap rebuild result is invalid");
  }
  return result;
}

void WriteBootstrapStatus(
    Writer &writer, const core::NativeProjectionBootstrapStatusDTO &status) {
  if (!status.presence_valid()) {
    Fail("cannot encode invalid bootstrap status");
  }
  WriteStrong(writer, status.bootstrap_id);
  WriteGeneration(writer, status.bootstrap_generation);
  WriteBinding(writer, status.native_runtime_binding);
  WriteProjectionRevision(writer, status.canonical_empty_target);
  writer.Byte(static_cast<std::uint8_t>(status.state));
  WriteOptional(writer, status.acknowledged_target, WriteProjectionRevision);
  WriteOptional(
      writer, status.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, status.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, status.resulting_health);
  WriteOptional(writer, status.error, WriteError);
}

[[nodiscard]] core::NativeProjectionBootstrapStatusState
ReadBootstrapStatusState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionBootstrapStatusState::kAbsent;
  case 1U:
    return core::NativeProjectionBootstrapStatusState::kRebuilding;
  case 2U:
    return core::NativeProjectionBootstrapStatusState::kRebuilt;
  case 3U:
    return core::NativeProjectionBootstrapStatusState::
        kFailedProjectionUncertain;
  default:
    Fail("bootstrap status state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionBootstrapStatusDTO
ReadBootstrapStatus(Reader &reader) {
  core::NativeProjectionBootstrapStatusDTO status{
      ReadStrong<core::BootstrapId>(reader),
      ReadGeneration<core::BootstrapGeneration>(reader),
      ReadBinding(reader),
      ReadProjectionRevision(reader),
      ReadBootstrapStatusState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!status.presence_valid()) {
    Fail("decoded bootstrap status is invalid");
  }
  return status;
}

void WriteStageResult(Writer &writer,
                      const core::NativeProjectionStageResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid projection stage result");
  }
  WriteStrong(writer, result.apply_id);
  WriteGeneration(writer, result.apply_generation);
  WriteGeneration(writer, result.projection_stage_generation);
  WriteTransitionRow(writer, result.transition_row);
  WriteProjectionRevision(writer,
                          result.expected_predecessor_projection_revision);
  WriteProjectionRevision(writer, result.candidate_projection_revision);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional(writer, result.acknowledged_staged_candidate_revision,
                WriteProjectionRevision);
  WriteOptional(writer, result.observed_local_active, WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, result.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::NativeProjectionStageResultState
ReadStageResultState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionStageResultState::kStaged;
  case 1U:
    return core::NativeProjectionStageResultState::kRejected;
  case 2U:
    return core::NativeProjectionStageResultState::kFailedProjectionUncertain;
  default:
    Fail("projection stage result state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionStageResultDTO
ReadStageResult(Reader &reader) {
  core::NativeProjectionStageResultDTO result{
      ReadStrong<core::ApplyId>(reader),
      ReadGeneration<core::ApplyGeneration>(reader),
      ReadGeneration<core::ProjectionStageGeneration>(reader),
      ReadTransitionRow(reader),
      ReadProjectionRevision(reader),
      ReadProjectionRevision(reader),
      ReadStageResultState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection stage result is invalid");
  }
  return result;
}

void WriteActivationResult(
    Writer &writer, const core::NativeProjectionActivationResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid projection activation result");
  }
  WriteActivation(writer, result.request);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional(writer, result.acknowledged_candidate_projection_revision,
                WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, result.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::NativeProjectionActivationResultState
ReadActivationResultState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionActivationResultState::kActivated;
  case 1U:
    return core::NativeProjectionActivationResultState::kStale;
  case 2U:
    return core::NativeProjectionActivationResultState::
        kFailedProjectionUncertain;
  default:
    Fail("projection activation result state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionActivationResultDTO
ReadActivationResult(Reader &reader) {
  core::NativeProjectionActivationResultDTO result{
      ReadActivation(reader),
      ReadActivationResultState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection activation result is invalid");
  }
  return result;
}

template <typename Result, typename State>
void WriteRetireLike(Writer &writer, const Result &result, const State state) {
  WriteRetire(writer, result.request);
  writer.Byte(static_cast<std::uint8_t>(state));
  WriteOptional(writer, result.observed_local_active, WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, result.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::NativeProjectionRetireResultState
ReadRetireResultState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionRetireResultState::kRetired;
  case 1U:
    return core::NativeProjectionRetireResultState::kAlreadyAbsent;
  case 2U:
    return core::NativeProjectionRetireResultState::kStale;
  case 3U:
    return core::NativeProjectionRetireResultState::kFailedProjectionUncertain;
  default:
    Fail("projection retirement result state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionRetireResultDTO
ReadRetireResult(Reader &reader) {
  core::NativeProjectionRetireResultDTO result{
      ReadRetire(reader),
      ReadRetireResultState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection retirement result is invalid");
  }
  return result;
}

[[nodiscard]] core::NativeProjectionRetireStatusState
ReadRetireStatusState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionRetireStatusState::kPending;
  case 1U:
    return core::NativeProjectionRetireStatusState::kRetired;
  case 2U:
    return core::NativeProjectionRetireStatusState::kAlreadyAbsent;
  case 3U:
    return core::NativeProjectionRetireStatusState::kStale;
  case 4U:
    return core::NativeProjectionRetireStatusState::kFailedProjectionUncertain;
  default:
    Fail("projection retirement status state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionRetireStatusDTO
ReadRetireStatus(Reader &reader) {
  core::NativeProjectionRetireStatusDTO result{
      ReadRetire(reader),
      ReadRetireStatusState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection retirement status is invalid");
  }
  return result;
}

void WriteDiscardResult(Writer &writer,
                        const core::NativeProjectionDiscardResultDTO &result) {
  if (!result.presence_valid()) {
    Fail("cannot encode invalid projection discard result");
  }
  WriteDiscard(writer, result.request);
  writer.Byte(static_cast<std::uint8_t>(result.state));
  WriteOptional(writer, result.observed_local_active, WriteProjectionRevision);
  WriteOptional(
      writer, result.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, result.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, result.resulting_health);
  WriteOptional(writer, result.error, WriteError);
}

[[nodiscard]] core::NativeProjectionDiscardResultState
ReadDiscardResultState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionDiscardResultState::kDiscarded;
  case 1U:
    return core::NativeProjectionDiscardResultState::kAlreadyAbsent;
  case 2U:
    return core::NativeProjectionDiscardResultState::kFailedProjectionUncertain;
  default:
    Fail("projection discard result state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionDiscardResultDTO
ReadDiscardResult(Reader &reader) {
  core::NativeProjectionDiscardResultDTO result{
      ReadDiscard(reader),
      ReadDiscardResultState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!result.presence_valid()) {
    Fail("decoded projection discard result is invalid");
  }
  return result;
}

void WriteStatus(Writer &writer,
                 const core::NativeProjectionStatusDTO &status) {
  if (!status.presence_valid()) {
    Fail("cannot encode invalid projection status");
  }
  WriteStatusQuery(writer, status.query);
  writer.Byte(static_cast<std::uint8_t>(status.state));
  WriteOptional(writer, status.staged_or_active_candidate_revision,
                WriteProjectionRevision);
  WriteOptional(writer, status.observed_local_active, WriteProjectionRevision);
  WriteOptional(
      writer, status.native_projection_generation,
      [](Writer &output, const std::uint64_t value) { output.U64(value); });
  WriteOptional(writer, status.projection_content_digest,
                [](Writer &output, const core::ContentDigest &value) {
                  WriteStrong(output, value);
                });
  WriteHealth(writer, status.resulting_health);
  WriteOptional(writer, status.error, WriteError);
}

[[nodiscard]] core::NativeProjectionStatusState
ReadStatusState(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::NativeProjectionStatusState::kAbsent;
  case 1U:
    return core::NativeProjectionStatusState::kStaged;
  case 2U:
    return core::NativeProjectionStatusState::kActivated;
  case 3U:
    return core::NativeProjectionStatusState::kDiscarded;
  case 4U:
    return core::NativeProjectionStatusState::kFailedProjectionUncertain;
  default:
    Fail("projection status state is unknown");
  }
}

[[nodiscard]] core::NativeProjectionStatusDTO ReadStatus(Reader &reader) {
  core::NativeProjectionStatusDTO status{
      ReadStatusQuery(reader),
      ReadStatusState(reader),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<core::ProjectionRevisionDTO>(reader, ReadProjectionRevision),
      ReadOptional<std::uint64_t>(reader,
                                  [](Reader &input) { return input.U64(); }),
      ReadOptional<core::ContentDigest>(
          reader,
          [](Reader &input) { return ReadStrong<core::ContentDigest>(input); }),
      ReadHealth(reader),
      ReadOptional<core::ErrorDTO>(reader, ReadError)};
  if (!status.presence_valid()) {
    Fail("decoded projection status is invalid");
  }
  return status;
}

enum class RequestKind : std::uint8_t {
  kPrepare = 1U,
  kHealth = 2U,
  kTransaction = 3U,
  kRebuild = 4U,
  kBootstrapRebuild = 5U,
  kBootstrapStatus = 6U,
  kStage = 7U,
  kActivate = 8U,
  kRetire = 9U,
  kRetireStatus = 10U,
  kDiscard = 11U,
  kStageStatus = 12U,
};

enum class ResponseKind : std::uint8_t {
  kFailure = 1U,
  kPrepared = 2U,
  kHealth = 3U,
  kCommit = 4U,
  kBootstrapRebuild = 5U,
  kBootstrapStatus = 6U,
  kStage = 7U,
  kActivation = 8U,
  kRetire = 9U,
  kRetireStatus = 10U,
  kDiscard = 11U,
  kStageStatus = 12U,
};

} // namespace

std::string
EncodeNativeProjectionRpcRequest(const NativeProjectionRpcRequest &request) {
  Writer writer;
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NativeProjectionPrepareRequest>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kPrepare));
          WritePrepare(writer, value);
        } else if constexpr (std::is_same_v<T, NativeProjectionHealthQuery>) {
          if (!value.native_runtime_binding.valid()) {
            Fail("cannot encode invalid projection health query");
          }
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kHealth));
          WriteBinding(writer, value.native_runtime_binding);
        } else if constexpr (std::is_same_v<
                                 T, NativeProjectionTransactionRequest>) {
          if (!value.valid()) {
            Fail("cannot encode invalid projection transaction request");
          }
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kTransaction));
          WriteBinding(writer, value.native_runtime_binding);
          writer.String(scene::EncodeSceneTransaction(value.transaction));
        } else if constexpr (std::is_same_v<T,
                                            core::NativeProjectionRebuildDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRebuild));
          WriteRebuild(writer, value);
        } else if constexpr (std::is_same_v<
                                 T,
                                 core::NativeProjectionBootstrapRebuildDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(RequestKind::kBootstrapRebuild));
          WriteBootstrapRebuild(writer, value);
        } else if constexpr (
            std::is_same_v<T, core::NativeProjectionBootstrapStatusQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kBootstrapStatus));
          WriteBootstrapQuery(writer, value);
        } else if constexpr (std::is_same_v<T,
                                            core::NativeProjectionStageDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kStage));
          WriteStage(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionActivationDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kActivate));
          WriteActivation(writer, value);
        } else if constexpr (std::is_same_v<T,
                                            core::NativeProjectionRetireDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRetire));
          WriteRetire(writer, value);
        } else if constexpr (std::is_same_v<
                                 T,
                                 core::NativeProjectionRetireStatusQueryDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRetireStatus));
          WriteRetire(writer, value.request);
        } else if constexpr (std::is_same_v<T,
                                            core::NativeProjectionDiscardDTO>) {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kDiscard));
          WriteDiscard(writer, value);
        } else {
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kStageStatus));
          WriteStatusQuery(writer, value);
        }
      },
      request);
  return std::move(writer).Finish();
}

NativeProjectionRpcRequest
DecodeNativeProjectionRpcRequest(const std::string_view bytes) {
  Reader reader(bytes);
  NativeProjectionRpcRequest result = [&]() -> NativeProjectionRpcRequest {
    switch (static_cast<RequestKind>(reader.Byte())) {
    case RequestKind::kPrepare:
      return ReadPrepare(reader);
    case RequestKind::kHealth:
      return NativeProjectionHealthQuery{ReadBinding(reader)};
    case RequestKind::kTransaction:
      return NativeProjectionTransactionRequest{
          ReadBinding(reader), scene::DecodeSceneTransaction(reader.String())};
    case RequestKind::kRebuild:
      return ReadRebuild(reader);
    case RequestKind::kBootstrapRebuild:
      return ReadBootstrapRebuild(reader);
    case RequestKind::kBootstrapStatus:
      return ReadBootstrapQuery(reader);
    case RequestKind::kStage:
      return ReadStage(reader);
    case RequestKind::kActivate:
      return ReadActivation(reader);
    case RequestKind::kRetire:
      return ReadRetire(reader);
    case RequestKind::kRetireStatus:
      return core::NativeProjectionRetireStatusQueryDTO{ReadRetire(reader)};
    case RequestKind::kDiscard:
      return ReadDiscard(reader);
    case RequestKind::kStageStatus:
      return ReadStatusQuery(reader);
    }
    Fail("request kind is unknown");
  }();
  reader.Finish();
  return result;
}

std::string
EncodeNativeProjectionRpcResponse(const NativeProjectionRpcResponse &response) {
  Writer writer;
  std::visit(
      [&](const auto &value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, NativeProjectionRpcFailure>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kFailure));
          WriteError(writer, value.error);
        } else if constexpr (std::is_same_v<T, NativeProjectionPrepareResult>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kPrepared));
          WritePrepare(writer, value.request);
        } else if constexpr (std::is_same_v<T, core::ProjectionHealthDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kHealth));
          WriteHealth(writer, value);
        } else if constexpr (std::is_same_v<T,
                                            core::ProjectionCommitResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kCommit));
          WriteProjectionCommit(writer, value);
        } else if constexpr (
            std::is_same_v<T,
                           core::NativeProjectionBootstrapRebuildResultDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(ResponseKind::kBootstrapRebuild));
          WriteBootstrapRebuildResult(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionBootstrapStatusDTO>) {
          writer.Byte(
              static_cast<std::uint8_t>(ResponseKind::kBootstrapStatus));
          WriteBootstrapStatus(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionStageResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kStage));
          WriteStageResult(writer, value);
        } else if constexpr (std::is_same_v<
                                 T,
                                 core::NativeProjectionActivationResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kActivation));
          WriteActivationResult(writer, value);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionRetireResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kRetire));
          if (!value.presence_valid()) {
            Fail("cannot encode invalid projection retirement result");
          }
          WriteRetireLike(writer, value, value.state);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionRetireStatusDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kRetireStatus));
          if (!value.presence_valid()) {
            Fail("cannot encode invalid projection retirement status");
          }
          WriteRetireLike(writer, value, value.state);
        } else if constexpr (std::is_same_v<
                                 T, core::NativeProjectionDiscardResultDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kDiscard));
          WriteDiscardResult(writer, value);
        } else {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kStageStatus));
          WriteStatus(writer, value);
        }
      },
      response);
  return std::move(writer).Finish();
}

NativeProjectionRpcResponse
DecodeNativeProjectionRpcResponse(const std::string_view bytes) {
  Reader reader(bytes);
  NativeProjectionRpcResponse result = [&]() -> NativeProjectionRpcResponse {
    switch (static_cast<ResponseKind>(reader.Byte())) {
    case ResponseKind::kFailure:
      return NativeProjectionRpcFailure{ReadError(reader)};
    case ResponseKind::kPrepared:
      return NativeProjectionPrepareResult{ReadPrepare(reader)};
    case ResponseKind::kHealth:
      return ReadHealth(reader);
    case ResponseKind::kCommit:
      return ReadProjectionCommit(reader);
    case ResponseKind::kBootstrapRebuild:
      return ReadBootstrapRebuildResult(reader);
    case ResponseKind::kBootstrapStatus:
      return ReadBootstrapStatus(reader);
    case ResponseKind::kStage:
      return ReadStageResult(reader);
    case ResponseKind::kActivation:
      return ReadActivationResult(reader);
    case ResponseKind::kRetire:
      return ReadRetireResult(reader);
    case ResponseKind::kRetireStatus:
      return ReadRetireStatus(reader);
    case ResponseKind::kDiscard:
      return ReadDiscardResult(reader);
    case ResponseKind::kStageStatus:
      return ReadStatus(reader);
    }
    Fail("response kind is unknown");
  }();
  reader.Finish();
  return result;
}

} // namespace manyforge::component_manager::detail

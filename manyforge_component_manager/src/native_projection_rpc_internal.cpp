#include "native_projection_rpc_internal.hpp"

#include <array>
#include <bit>
#include <cmath>
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

// Version 4 added the provider-freshness kinds (17), now retired. Both
// endpoints compile this unit; a mixed-version peer fails closed on magic.
constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'P',
                                              'R', 'P', 'C', '4'};
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

  void Double(const double value) {
    if (!std::isfinite(value)) {
      Fail("floating-point field is non-finite");
    }
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

  [[nodiscard]] double Double() {
    const double value = std::bit_cast<double>(U64());
    if (!std::isfinite(value)) {
      Fail("floating-point field is non-finite");
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

  [[nodiscard]] bool HasRemaining() const { return offset_ != bytes_.size(); }

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

void WriteLeaseBinding(Writer &writer,
                       const core::ProjectionLeaseBindingDTO &binding) {
  if (!binding.valid()) {
    Fail("cannot encode invalid projection lease binding");
  }
  WriteProjectionRevision(writer, binding.projection_revision);
  WriteStrong(writer, binding.projection_lease_id);
}

[[nodiscard]] core::ProjectionLeaseBindingDTO
ReadLeaseBinding(Reader &reader) {
  core::ProjectionLeaseBindingDTO binding{
      ReadProjectionRevision(reader),
      ReadStrong<core::ProjectionLeaseId>(reader)};
  if (!binding.valid()) {
    Fail("decoded projection lease binding is invalid");
  }
  return binding;
}

void WritePose(Writer &writer, const core::Pose3DTO &pose) {
  writer.Double(pose.position_m.x_m);
  writer.Double(pose.position_m.y_m);
  writer.Double(pose.position_m.z_m);
  writer.Double(pose.orientation.x);
  writer.Double(pose.orientation.y);
  writer.Double(pose.orientation.z);
  writer.Double(pose.orientation.w);
}

[[nodiscard]] core::Pose3DTO ReadPose(Reader &reader) {
  return {{reader.Double(), reader.Double(), reader.Double()},
          {reader.Double(), reader.Double(), reader.Double(),
           reader.Double()}};
}

void WriteSceneFrameRef(Writer &writer,
                        const core::SceneFrameRefDTO &reference) {
  std::visit(
      [&](const auto &value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::CellSceneFrameRefDTO>) {
          writer.Byte(0U);
          WriteStrong(writer, value.cell_id);
        } else if constexpr (std::is_same_v<
                                 T, core::ComponentSceneFrameRefDTO>) {
          writer.Byte(1U);
          WriteStrong(writer, value.controllable_component_id);
          WriteStrong(writer, value.model_instance_id);
          WriteStrong(writer, value.frame_role_id);
        } else {
          writer.Byte(2U);
          WriteStrong(writer, value.model_instance_id);
          WriteStrong(writer, value.frame_role_id);
        }
      },
      reference);
}

[[nodiscard]] core::SceneFrameRefDTO ReadSceneFrameRef(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::CellSceneFrameRefDTO{ReadStrong<core::CellId>(reader)};
  case 1U:
    return core::ComponentSceneFrameRefDTO{
        ReadStrong<core::ControllableComponentId>(reader),
        ReadStrong<core::ModelInstanceId>(reader),
        ReadStrong<core::FrameRoleId>(reader)};
  case 2U:
    return core::PassiveModelSceneFrameRefDTO{
        ReadStrong<core::ModelInstanceId>(reader),
        ReadStrong<core::FrameRoleId>(reader)};
  default:
    Fail("scene-frame reference kind is unknown");
  }
}

void WriteResolvedFrame(Writer &writer,
                        const core::ResolvedFrameDTO &resolved) {
  std::visit(
      [&](const auto &value) {
        using T = std::remove_cvref_t<decltype(value)>;
        if constexpr (std::is_same_v<T, core::ResolvedCellFrameDTO>) {
          writer.Byte(0U);
          WriteStrong(writer, value.cell_id);
          writer.String(value.compiled_frame_name);
        } else if constexpr (std::is_same_v<
                                 T, core::ResolvedComponentModelFrameDTO>) {
          writer.Byte(1U);
          WriteStrong(writer, value.controllable_component_id);
          WriteStrong(writer, value.model_instance_id);
          WriteStrong(writer, value.frame_role_id);
          writer.String(value.compiled_frame_name);
          WriteStrong(writer, value.model_revision);
        } else {
          writer.Byte(2U);
          WriteStrong(writer, value.model_instance_id);
          WriteStrong(writer, value.frame_role_id);
          writer.String(value.compiled_frame_name);
          WriteStrong(writer, value.model_revision);
        }
      },
      resolved);
}

[[nodiscard]] core::ResolvedFrameDTO ReadResolvedFrame(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::ResolvedCellFrameDTO{ReadStrong<core::CellId>(reader),
                                      reader.String()};
  case 1U:
    return core::ResolvedComponentModelFrameDTO{
        ReadStrong<core::ControllableComponentId>(reader),
        ReadStrong<core::ModelInstanceId>(reader),
        ReadStrong<core::FrameRoleId>(reader), reader.String(),
        ReadStrong<core::ModelRevision>(reader)};
  case 2U:
    return core::ResolvedPassiveModelFrameDTO{
        ReadStrong<core::ModelInstanceId>(reader),
        ReadStrong<core::FrameRoleId>(reader), reader.String(),
        ReadStrong<core::ModelRevision>(reader)};
  default:
    Fail("resolved scene-frame kind is unknown");
  }
}

void WriteSceneFrameQuery(Writer &writer,
                          const core::SceneFrameResolutionQueryDTO &query) {
  if (!query.valid()) {
    Fail("cannot encode invalid scene-frame resolution query");
  }
  std::visit(
      [&](const auto &selector) {
        using T = std::remove_cvref_t<decltype(selector)>;
        if constexpr (std::is_same_v<T, core::SceneFrameRefDTO>) {
          writer.Byte(0U);
          WriteSceneFrameRef(writer, selector);
        } else {
          writer.Byte(1U);
          WriteStrong(writer, selector.controllable_component_id);
          WriteStrong(writer, selector.command_target_id);
          WriteStrong(writer, selector.frame_role_id);
        }
      },
      query.selector);
  WriteLeaseBinding(writer, query.projection_binding);
}

[[nodiscard]] core::SceneFrameResolutionQueryDTO
ReadSceneFrameQuery(Reader &reader) {
  core::SceneFrameResolutionSelectorDTO selector = [&] {
    switch (reader.Byte()) {
    case 0U:
      return core::SceneFrameResolutionSelectorDTO{
          ReadSceneFrameRef(reader)};
    case 1U:
      return core::SceneFrameResolutionSelectorDTO{
          core::TargetCarrierSceneFrameRefDTO{
              ReadStrong<core::ControllableComponentId>(reader),
              ReadStrong<core::CommandTargetId>(reader),
              ReadStrong<core::FrameRoleId>(reader)}};
    default:
      Fail("scene-frame selector kind is unknown");
    }
  }();
  core::SceneFrameResolutionQueryDTO query{std::move(selector),
                                           ReadLeaseBinding(reader)};
  if (!query.valid()) {
    Fail("decoded scene-frame resolution query is invalid");
  }
  return query;
}

void WriteSceneFrameReceipt(
    Writer &writer, const core::SceneFrameResolutionReceiptDTO &receipt) {
  if (!receipt.valid()) {
    Fail("cannot encode invalid scene-frame resolution receipt");
  }
  WriteResolvedFrame(writer, receipt.resolved_frame);
  WritePose(writer, receipt.frame_pose_in_cell);
  WriteVector(writer, receipt.component_attachment_revisions,
              [](Writer &output,
                 const core::ComponentAttachmentRevision &revision) {
                WriteStrong(output, revision);
              });
  WriteLeaseBinding(writer, receipt.projection_binding);
  WriteOptional(writer, receipt.predicted_state_digest,
                [](Writer &output, const core::ContentDigest &digest) {
                  WriteStrong(output, digest);
                });
  WriteStrong(writer, receipt.resolution_evidence_digest);
}

[[nodiscard]] core::SceneFrameResolutionReceiptDTO
ReadSceneFrameReceipt(Reader &reader) {
  core::SceneFrameResolutionReceiptDTO receipt{
      ReadResolvedFrame(reader),
      ReadPose(reader),
      ReadVector<core::ComponentAttachmentRevision>(
          reader, [](Reader &input) {
            return ReadStrong<core::ComponentAttachmentRevision>(input);
      }),
      ReadLeaseBinding(reader),
      ReadOptional<core::ContentDigest>(
          reader, [](Reader &input) {
            return ReadStrong<core::ContentDigest>(input);
          }),
      ReadStrong<core::ContentDigest>(reader)};
  if (!receipt.valid()) {
    Fail("decoded scene-frame resolution receipt is invalid");
  }
  return receipt;
}

void WriteCollisionPair(Writer &writer, const core::CollisionPairId &pair) {
  writer.String(pair.body_a);
  writer.String(pair.body_b);
}

[[nodiscard]] core::CollisionPairId ReadCollisionPair(Reader &reader) {
  return {reader.String(), reader.String()};
}

void WriteCollisionPolicy(Writer &writer,
                          const core::CollisionPolicySnapshot &policy) {
  writer.String(policy.policy_id);
  writer.U64(policy.revision.revision_id);
  writer.String(policy.revision.content_hash);
  WriteVector(writer, policy.allowed_static_static_pairs,
              WriteCollisionPair);
  WriteVector(writer, policy.allowed_touch_link_pairs, WriteCollisionPair);
  WriteVector(
      writer, policy.support_contacts,
      [](Writer &output, const core::SupportContactPolicyEntry &entry) {
        WriteCollisionPair(output, entry.pair);
        output.Byte(static_cast<std::uint8_t>(entry.mode));
        output.Byte(
            static_cast<std::uint8_t>(entry.release_evidence_mode));
        output.Boolean(entry.enforce_max_start_penetration);
        output.Double(entry.max_start_penetration_m);
        output.Double(entry.release_tolerance_m);
        output.Double(entry.min_release_progress_m);
        output.Double(entry.max_contact_penetration_m);
        output.Boolean(entry.require_separation_by_end);
        output.Boolean(entry.require_strictly_decreasing_penetration);
        output.Boolean(entry.allow_recontact);
      });
  WriteVector(
      writer, policy.contact_grasps,
      [](Writer &output, const core::ContactGraspPolicyEntry &entry) {
        output.String(entry.policy_id);
        output.String(entry.selected_object_id);
        WriteVector(output, entry.allowed_robot_links,
                    [](Writer &links, const std::string &link) {
                      links.String(link);
                    });
        output.Double(entry.max_penetration_m);
        output.Double(entry.terminal_window_start_fraction);
      });
  writer.Boolean(policy.require_full_diagnostics);
}

[[nodiscard]] core::CollisionContactPolicyMode
ReadContactPolicyMode(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::CollisionContactPolicyMode::kContactRelease;
  case 1U:
    return core::CollisionContactPolicyMode::kBoundedPenetration;
  case 2U:
    return core::CollisionContactPolicyMode::kContactProcess;
  default:
    Fail("collision contact-policy mode is unknown");
  }
}

[[nodiscard]] core::ContactReleaseEvidenceMode
ReadReleaseEvidenceMode(Reader &reader) {
  switch (reader.Byte()) {
  case 0U:
    return core::ContactReleaseEvidenceMode::kSignedPenetration;
  case 1U:
    return core::ContactReleaseEvidenceMode::kBinaryContact;
  default:
    Fail("contact release-evidence mode is unknown");
  }
}

[[nodiscard]] core::CollisionPolicySnapshot
ReadCollisionPolicy(Reader &reader) {
  core::CollisionPolicySnapshot policy;
  policy.policy_id = reader.String();
  policy.revision = {reader.U64(), reader.String()};
  policy.allowed_static_static_pairs =
      ReadVector<core::CollisionPairId>(reader, ReadCollisionPair);
  policy.allowed_touch_link_pairs =
      ReadVector<core::CollisionPairId>(reader, ReadCollisionPair);
  policy.support_contacts = ReadVector<core::SupportContactPolicyEntry>(
      reader, [](Reader &input) {
        return core::SupportContactPolicyEntry{
            ReadCollisionPair(input),
            ReadContactPolicyMode(input),
            ReadReleaseEvidenceMode(input),
            input.Boolean(),
            input.Double(),
            input.Double(),
            input.Double(),
            input.Double(),
            input.Boolean(),
            input.Boolean(),
            input.Boolean()};
      });
  policy.contact_grasps = ReadVector<core::ContactGraspPolicyEntry>(
      reader, [](Reader &input) {
        return core::ContactGraspPolicyEntry{
            input.String(), input.String(),
            ReadVector<std::string>(input,
                                    [](Reader &links) {
                                      return links.String();
                                    }),
            input.Double(), input.Double()};
      });
  policy.require_full_diagnostics = reader.Boolean();
  if (policy.policy_id.empty() || policy.revision.revision_id == 0U ||
      policy.revision.content_hash.empty()) {
    Fail("decoded collision policy is incomplete");
  }
  return policy;
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
  writer.Boolean(request.maintenance_successor.has_value());
  if (request.maintenance_successor.has_value()) {
    WriteStrong(writer, request.maintenance_successor->first);
    WriteGeneration(writer, request.maintenance_successor->second);
    WriteProjectionRevision(writer, *request.maintenance_predecessor);
  }
}

[[nodiscard]] core::NativeProjectionRebuildDTO ReadRebuild(Reader &reader) {
  core::NativeProjectionRebuildDTO request{
      ReadBinding(reader), ReadProjectionRevision(reader), ReadProduct(reader),
      std::make_shared<const core::LogicalSceneSnapshotDTO>(
          scene::DecodeLogicalSceneSnapshot(reader.String()))};
  if (reader.Boolean()) {
    request.maintenance_successor = std::pair{
        ReadStrong<core::ApplyId>(reader),
        ReadGeneration<core::ApplyGeneration>(reader)};
    request.maintenance_predecessor = ReadProjectionRevision(reader);
  }
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
    return core::ProjectionCommitResultStateDTO::kAccepted;
  case 1U:
    return core::ProjectionCommitResultStateDTO::kCommitted;
  case 2U:
    return core::ProjectionCommitResultStateDTO::kRejectedRollbackProven;
  case 3U:
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

enum class RequestKind : std::uint8_t {
  kPrepare = 1U,
  kHealth = 2U,
  kTransaction = 3U,
  kRebuild = 4U,
  kBootstrapRebuild = 5U,
  kBootstrapStatus = 6U,
  kRetain = 13U,
  kRelease = 14U,
  kSceneFrame = 15U,
  kEffectivePolicy = 16U,
  // 17 and 18 retired (provider freshness and commit marker).
};

enum class ResponseKind : std::uint8_t {
  kFailure = 1U,
  kPrepared = 2U,
  kHealth = 3U,
  kCommit = 4U,
  kBootstrapRebuild = 5U,
  kBootstrapStatus = 6U,
  kRetained = 13U,
  kReleased = 14U,
  kSceneFrame = 15U,
  kEffectivePolicy = 16U,
  // 17 and 18 retired (provider freshness and commit marker).
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
          // Absence retains the original tag-2 body byte for byte. An explicit
          // zero marker is not an alternative encoding of absence.
          if (value.selected_projection_revision.has_value()) {
            if (!value.selected_projection_revision->valid()) {
              Fail("cannot encode invalid projection health selector");
            }
            writer.Byte(1U);
            WriteProjectionRevision(writer,
                                    *value.selected_projection_revision);
          }
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
        } else if constexpr (std::is_same_v<
                                 T, NativeProjectionRetainRequest>) {
          if (!value.valid()) {
            Fail("cannot encode invalid projection retain request");
          }
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRetain));
          WriteBinding(writer, value.native_runtime_binding);
          WriteProjectionRevision(writer, value.projection_revision);
          WriteStrong(writer, value.projection_lease_id);
        } else if constexpr (std::is_same_v<
                                 T, NativeProjectionReleaseRequest>) {
          if (!value.valid()) {
            Fail("cannot encode invalid projection release request");
          }
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kRelease));
          WriteBinding(writer, value.native_runtime_binding);
          WriteStrong(writer, value.projection_lease_id);
        } else if constexpr (std::is_same_v<
                                 T, NativeProjectionSceneFrameRequest>) {
          if (!value.valid()) {
            Fail("cannot encode invalid projection scene-frame request");
          }
          writer.Byte(static_cast<std::uint8_t>(RequestKind::kSceneFrame));
          WriteBinding(writer, value.native_runtime_binding);
          WriteSceneFrameQuery(writer, value.query);
        } else {
          static_assert(
              std::is_same_v<T, NativeProjectionEffectivePolicyRequest>);
          if (!value.valid()) {
            Fail("cannot encode invalid effective-policy request");
          }
          writer.Byte(
              static_cast<std::uint8_t>(RequestKind::kEffectivePolicy));
          WriteBinding(writer, value.native_runtime_binding);
          writer.String(scene::EncodeLogicalSceneSnapshot(
              value.logical_scene));
          writer.U64(value.effective_policy_revision_id);
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
    case RequestKind::kHealth: {
      NativeProjectionHealthQuery query{ReadBinding(reader)};
      if (reader.HasRemaining()) {
        if (reader.Byte() != 1U) {
          Fail("projection health selector marker is noncanonical");
        }
        query.selected_projection_revision = ReadProjectionRevision(reader);
      }
      return query;
    }
    case RequestKind::kTransaction:
      return NativeProjectionTransactionRequest{
          ReadBinding(reader), scene::DecodeSceneTransaction(reader.String())};
    case RequestKind::kRebuild:
      return ReadRebuild(reader);
    case RequestKind::kBootstrapRebuild:
      return ReadBootstrapRebuild(reader);
    case RequestKind::kBootstrapStatus:
      return ReadBootstrapQuery(reader);
    case RequestKind::kRetain:
      return NativeProjectionRetainRequest{
          ReadBinding(reader), ReadProjectionRevision(reader),
          ReadStrong<core::ProjectionLeaseId>(reader)};
    case RequestKind::kRelease:
      return NativeProjectionReleaseRequest{
          ReadBinding(reader), ReadStrong<core::ProjectionLeaseId>(reader)};
    case RequestKind::kSceneFrame:
      return NativeProjectionSceneFrameRequest{ReadBinding(reader),
                                               ReadSceneFrameQuery(reader)};
    case RequestKind::kEffectivePolicy:
      return NativeProjectionEffectivePolicyRequest{
          ReadBinding(reader),
          scene::DecodeLogicalSceneSnapshot(reader.String()), reader.U64()};
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
                                 T, NativeProjectionRetainResult>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kRetained));
          WriteLeaseBinding(writer, value.projection_binding);
        } else if constexpr (std::is_same_v<
                                 T, NativeProjectionReleaseResult>) {
          if (value.projection_lease_id.empty()) {
            Fail("cannot encode empty projection release result");
          }
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kReleased));
          WriteStrong(writer, value.projection_lease_id);
        } else if constexpr (std::is_same_v<
                                 T, core::SceneFrameResolutionReceiptDTO>) {
          writer.Byte(static_cast<std::uint8_t>(ResponseKind::kSceneFrame));
          WriteSceneFrameReceipt(writer, value);
        } else {
          static_assert(std::is_same_v<T, core::CollisionPolicySnapshot>);
          writer.Byte(
              static_cast<std::uint8_t>(ResponseKind::kEffectivePolicy));
          WriteCollisionPolicy(writer, value);
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
    case ResponseKind::kRetained:
      return NativeProjectionRetainResult{ReadLeaseBinding(reader)};
    case ResponseKind::kReleased:
      return NativeProjectionReleaseResult{
          ReadStrong<core::ProjectionLeaseId>(reader)};
    case ResponseKind::kSceneFrame:
      return ReadSceneFrameReceipt(reader);
    case ResponseKind::kEffectivePolicy:
      return ReadCollisionPolicy(reader);
    }
    Fail("response kind is unknown");
  }();
  reader.Finish();
  return result;
}

} // namespace manyforge::component_manager::detail

#include "selected_runtime_composition_rpc_internal.hpp"

#include <array>
#include <cstdint>
#include <limits>
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
constexpr std::size_t kMaximumCollectionEntries = 4096U;
constexpr std::uint8_t kRequestTag = 1U;
constexpr std::uint8_t kPreparingTag = 2U;
constexpr std::uint8_t kInstalledTag = 3U;
constexpr std::uint8_t kFailureTag = 4U;

[[noreturn]] void Fail(const std::string &detail) {
  throw std::invalid_argument("selected-runtime composition RPC codec: " +
                              detail);
}

class Writer final {
public:
  explicit Writer(const std::uint8_t tag) {
    bytes_.reserve(1024U);
    for (const auto byte : kMagic) {
      Byte(byte);
    }
    Byte(tag);
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

  std::string bytes_;
};

class Reader final {
public:
  explicit Reader(const std::string_view bytes) : bytes_(bytes) {
    if (bytes_.size() <= kMagic.size() ||
        bytes_.size() > kMaximumSelectedRuntimeCompositionRpcPacketBytes) {
      Fail("packet is empty, incomplete, or oversized");
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

void WriteScope(Writer &writer, const core::ConfigurationFenceScopeDTO &scope) {
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
  if (bytes.size() <= kMagic.size()) {
    return false;
  }
  for (std::size_t index = 0U; index < kMagic.size(); ++index) {
    if (static_cast<std::uint8_t>(bytes[index]) != kMagic[index]) {
      return false;
    }
  }
  return true;
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const ManagerSelectedRuntimeCompositionRequestDTO &request) {
  if (!request.valid()) {
    Fail("cannot encode an invalid request");
  }
  Writer writer(kRequestTag);
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
  reader.Finish();
  if (!request.valid()) {
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
                  : kInstalledTag);
          WriteRequest(writer, value.request);
          WriteReceipts(writer, value.component_receipts);
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
  reader.Finish();
  if (!result.valid()) {
    Fail("decoded result is invalid");
  }
  return result;
}

} // namespace manyforge::component_manager::detail

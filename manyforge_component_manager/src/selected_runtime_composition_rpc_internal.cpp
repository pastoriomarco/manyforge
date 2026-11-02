#include "selected_runtime_composition_rpc_internal.hpp"

#include <algorithm>
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

// Version 6 carries the CONTRACTS §15 host Apply in place of the composition
// request, its phases and receipts.
constexpr std::array<std::uint8_t, 8U> kMagic{'M', 'F', 'N', 'S',
                                              'R', 'C', '6', 0};
constexpr std::size_t kMaximumCollectionEntries = 4096U;
constexpr std::uint8_t kFailureTag = 4U;
constexpr std::uint8_t kHostApplyRequestTag = 18U;
constexpr std::uint8_t kHostApplyResultTag = 19U;
constexpr std::uint8_t kInstalledInventoryRequestTag = 16U;
constexpr std::uint8_t kInstalledInventoryResultTag = 17U;
constexpr std::uint8_t kProcessStopSubmitTag = 13U;
constexpr std::uint8_t kProcessStopReadTag = 14U;
constexpr std::uint8_t kProcessStopResultTag = 15U;

constexpr std::uint8_t kPneumaticNoneOwnersTail = 4U;
constexpr std::string_view kInstalledNoneOwnersMagic = "MFNSRC4-INSTALLED-PNEUMATIC-NONE-V1";
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
    if (bytes_.substr(0U, kMagic.size()) !=
        std::string_view(reinterpret_cast<const char *>(kMagic.data()),
                         kMagic.size()))
      Fail("packet magic or version differs");
    offset_ = kMagic.size();
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

template <typename Strong>
void WriteStrongVector(Writer &writer, const std::vector<Strong> &values) {
  WriteVector(writer, values,
              [](Writer &output, const auto &value) { WriteStrong(output, value); });
}

template <typename Strong>
[[nodiscard]] std::vector<Strong> ReadStrongVector(Reader &reader) {
  return ReadVector<Strong>(
      reader, [](Reader &input) { return ReadStrong<Strong>(input); });
}

void WriteHostApplyRequest(Writer &writer, const HostApplyRequest &request) {
  WriteStrong(writer, request.apply.apply_id);
  WriteGeneration(writer, request.apply.apply_generation);
  WriteStrong(writer, request.candidate_retained_bundle_id);
  WriteStrong(writer, request.candidate_graph_revision);
  WriteStrongVector(writer, request.components);
  WriteStrongVector(writer, request.retained_components);
  WriteStrongVector(writer, request.removed_components);
  WriteStrongVector(writer, request.removed_domains);
}

[[nodiscard]] HostApplyRequest ReadHostApplyRequest(Reader &reader) {
  return {{ReadStrong<core::ApplyId>(reader),
           ReadGeneration<core::ApplyGeneration>(reader)},
          ReadStrong<core::RetainedBundleId>(reader),
          ReadStrong<core::GraphRevision>(reader),
          ReadStrongVector<core::ControllableComponentId>(reader),
          ReadStrongVector<core::ControllableComponentId>(reader),
          ReadStrongVector<core::ControllableComponentId>(reader),
          ReadStrongVector<core::ControlDomainId>(reader)};
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
  return bytes.substr(0U, kMagic.size()) ==
         std::string_view(reinterpret_cast<const char *>(kMagic.data()),
                          kMagic.size());
}

std::string EncodeSelectedRuntimeCompositionRpcRequest(
    const HostApplyRequest &request) {
  if (!request.valid()) Fail("cannot encode an invalid host Apply request");
  Writer writer(kHostApplyRequestTag);
  WriteHostApplyRequest(writer, request);
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
    if (tag == kHostApplyRequestTag)
      return ReadHostApplyRequest(reader);
    Fail("packet is not a request");
  }();
  reader.Finish();
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
        if constexpr (std::is_same_v<Value, HostApplyResult>) {
          if (!value.valid()) Fail("cannot encode an invalid host Apply result");
          Writer writer(kHostApplyResultTag);
          writer.Byte(static_cast<std::uint8_t>(value.outcome));
          writer.Boolean(value.error.has_value());
          if (value.error) WriteError(writer, *value.error);
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
  if (tag != kHostApplyResultTag) Fail("packet is not a response");
  const auto outcome = reader.Byte();
  if (outcome > static_cast<std::uint8_t>(HostApplyOutcome::kFailed))
    Fail("host Apply outcome is unknown");
  HostApplyResult result{static_cast<HostApplyOutcome>(outcome), std::nullopt};
  if (reader.Boolean()) result.error = ReadError(reader);
  reader.Finish();
  if (!result.valid()) Fail("decoded host Apply result is invalid");
  return result;
}

} // namespace manyforge::component_manager::detail

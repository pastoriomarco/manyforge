#include <manyforge_core/tcp_selector_codec.hpp>

#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

namespace manyforge::core {
namespace {

using content_identity::CanonicalValue;

template <typename T>
[[nodiscard]] Result<T> invalid(std::string reason) {
  return Result<T>::Err(ErrorDTO{std::string(to_string(ErrorCode::kInvalidRequest)),
                                 reason == "tcp_endpoint_legacy_form"
                                     ? "the TCP is named in the legacy form"
                                     : "the TCP selector is malformed",
                                 {{"reason", std::move(reason)}}});
}

/// A non-empty string member, or nullptr.
[[nodiscard]] const std::string* text(const CanonicalValue& value, std::string_view name) {
  const auto* member = value.find_object_member(name);
  return member != nullptr && member->is_string() && !member->as_string().empty()
             ? &member->as_string()
             : nullptr;
}

}  // namespace

CanonicalValue EncodeTcpEntry(const TcpEntryDTO& entry) {
  return CanonicalValue(CanonicalValue::Object{
      {"component_id", CanonicalValue(entry.component_id.value())},
      {"target_id", CanonicalValue(entry.target_id.value())},
      {"frame_role_id", CanonicalValue(entry.frame_role_id.value())}});
}

Result<TcpEntryDTO> DecodeTcpEntry(const CanonicalValue& value) {
  if (!value.is_object() || value.as_object().size() != 3U) {
    return invalid<TcpEntryDTO>("tcp_selector_invalid");
  }
  const auto* component = text(value, "component_id");
  const auto* target = text(value, "target_id");
  const auto* frame = text(value, "frame_role_id");
  if (component == nullptr || target == nullptr || frame == nullptr) {
    return invalid<TcpEntryDTO>("tcp_selector_invalid");
  }
  return Result<TcpEntryDTO>::Ok(TcpEntryDTO{ControllableComponentId(*component),
                                             CommandTargetId(*target), FrameRoleId(*frame)});
}

CanonicalValue EncodeTcpEntryReference(const TcpEntryReferenceDTO& entry) {
  return CanonicalValue(CanonicalValue::Object{
      {"target", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(entry.target.component_id.value())},
          {"command_target_id", CanonicalValue(entry.target.command_target_id.value())}})},
      {"frame", CanonicalValue(CanonicalValue::Object{
          {"component_id", CanonicalValue(entry.frame.component_id.value())},
          {"frame_id", CanonicalValue(entry.frame.frame_id.value())}})}});
}

Result<TcpEntryReferenceDTO> DecodeTcpEntryReference(const CanonicalValue& value) {
  if (!value.is_object() || value.as_object().size() != 2U)
    return invalid<TcpEntryReferenceDTO>("tcp_selector_invalid");
  const auto* target = value.find_object_member("target");
  const auto* frame = value.find_object_member("frame");
  if (!target || !target->is_object() || target->as_object().size() != 2U ||
      !frame || !frame->is_object() || frame->as_object().size() != 2U)
    return invalid<TcpEntryReferenceDTO>("tcp_selector_invalid");
  const auto* component = text(*target, "component_id");
  const auto* command = text(*target, "command_target_id");
  const auto* owner = text(*frame, "component_id");
  const auto* id = text(*frame, "frame_id");
  if (!component || !command || !owner || !id)
    return invalid<TcpEntryReferenceDTO>("tcp_selector_invalid");
  return Result<TcpEntryReferenceDTO>::Ok(TcpEntryReferenceDTO{
      {ControllableComponentId(*component), CommandTargetId(*command)},
      {ControllableComponentId(*owner), FrameRoleId(*id)}});
}

CanonicalValue EncodeTcpSelector(const TcpSelectorDTO& selector) {
  const auto target_value = [](const ProgramTargetReferenceDTO& target) {
    return CanonicalValue(CanonicalValue::Object{
        {"component_id", CanonicalValue(target.component_id.value())},
        {"command_target_id", CanonicalValue(target.command_target_id.value())}});
  };
  return std::visit(
      [&](const auto& form) {
        using Form = std::decay_t<decltype(form)>;
        if constexpr (std::is_same_v<Form, CurrentTcpDTO>) {
          return CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("current")}});
        } else if constexpr (std::is_same_v<Form, DefaultTcpOfTargetDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("default_of_target")}, {"target", target_value(form.target)}});
        } else {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("entry")},
              {"target", target_value(form.target)},
              {"frame", CanonicalValue(CanonicalValue::Object{
                  {"component_id", CanonicalValue(form.frame.component_id.value())},
                  {"frame_id", CanonicalValue(form.frame.frame_id.value())}})}});
        }
      }, selector);
}

Result<TcpSelectorDTO> DecodeTcpSelector(const CanonicalValue& value) {
  if (!value.is_object()) return invalid<TcpSelectorDTO>("tcp_selector_invalid");
  const auto* kind = text(value, "kind");
  if (kind == nullptr) {
    return invalid<TcpSelectorDTO>(value.find_object_member("target_role_id") != nullptr
                                       ? "tcp_endpoint_legacy_form"
                                       : "tcp_selector_invalid");
  }
  const auto size = value.as_object().size();
  if (*kind == "current" && size == 1U)
    return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{CurrentTcpDTO{}});
  const auto* target = value.find_object_member("target");
  if (!target || !target->is_object() || target->as_object().size() != 2U)
    return invalid<TcpSelectorDTO>("tcp_selector_invalid");
  const auto* component = text(*target, "component_id");
  const auto* command = text(*target, "command_target_id");
  if (!component || !command) return invalid<TcpSelectorDTO>("tcp_selector_invalid");
  ProgramTargetReferenceDTO target_ref{ControllableComponentId(*component), CommandTargetId(*command)};
  if (*kind == "default_of_target" && size == 2U)
    return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{DefaultTcpOfTargetDTO{std::move(target_ref)}});
  if (*kind == "entry" && size == 3U) {
    const auto* frame = value.find_object_member("frame");
    if (frame && frame->is_object() && frame->as_object().size() == 2U) {
      const auto* owner = text(*frame, "component_id");
      const auto* id = text(*frame, "frame_id");
      if (owner && id)
        return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{TcpEntryReferenceDTO{
            std::move(target_ref), {ControllableComponentId(*owner), FrameRoleId(*id)}}});
    }
  }
  return invalid<TcpSelectorDTO>("tcp_selector_invalid");
}

}  // namespace manyforge::core

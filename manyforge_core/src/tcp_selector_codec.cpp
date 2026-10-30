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
                                     ? "the TCP is named in the pre-M7 form"
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

CanonicalValue EncodeTcpSelector(const TcpSelectorDTO& selector) {
  return std::visit(
      [](const auto& form) {
        using Form = std::decay_t<decltype(form)>;
        if constexpr (std::is_same_v<Form, CurrentTcpDTO>) {
          return CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("current")}});
        } else if constexpr (std::is_same_v<Form, DefaultTcpOfRoleDTO>) {
          return CanonicalValue(CanonicalValue::Object{
              {"kind", CanonicalValue("default_of_role")},
              {"role", CanonicalValue(form.role.value())}});
        } else {
          return CanonicalValue(CanonicalValue::Object{{"kind", CanonicalValue("entry")},
                                                       {"entry", EncodeTcpEntry(form)}});
        }
      },
      selector);
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
  if (*kind == "current" && size == 1U) {
    return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{CurrentTcpDTO{}});
  }
  if (*kind == "default_of_role" && size == 2U) {
    if (const auto* role = text(value, "role")) {
      return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{DefaultTcpOfRoleDTO{TargetRoleId(*role)}});
    }
  }
  if (*kind == "entry" && size == 2U) {
    if (const auto* entry = value.find_object_member("entry")) {
      auto decoded = DecodeTcpEntry(*entry);
      if (!decoded.ok()) return Result<TcpSelectorDTO>::Err(decoded.error());
      return Result<TcpSelectorDTO>::Ok(TcpSelectorDTO{std::move(decoded.value())});
    }
  }
  return invalid<TcpSelectorDTO>("tcp_selector_invalid");
}

}  // namespace manyforge::core

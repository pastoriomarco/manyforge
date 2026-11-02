#include <algorithm>
#include <manyforge_content_identity/canonical_value.hpp>
#include <manyforge_core/types/operation_runtime.hpp>
#include <string_view>
#include <type_traits>
#include <variant>

namespace manyforge::core {
namespace {

[[nodiscard]] bool bounded_utf8(std::string_view value, std::size_t maximum) noexcept {
  if (value.empty() || value.size() > maximum || value.find('\0') != std::string_view::npos) {
    return false;
  }
  try {
    return manyforge::content_identity::is_utf8_nfc(value);
  } catch (...) {
    return false;
  }
}

[[nodiscard]] bool context_valid(const PublicRequestContextDTO& context) noexcept {
  return !context.request_id.empty() &&
         context.request_id.value().size() <= kMaximumStopIdentityBytes &&
         context.request_id.value().find('\0') == std::string::npos &&
         !context.request_owner_epoch.empty() &&
         context.request_owner_epoch.value().size() <= kMaximumStopIdentityBytes &&
         context.request_owner_epoch.value().find('\0') == std::string::npos &&
         bounded_utf8(context.source_label, kMaximumStopRequestLabelBytes) &&
         (!context.correlation_id.has_value() ||
          bounded_utf8(*context.correlation_id, kMaximumStopRequestLabelBytes));
}

[[nodiscard]] bool attribution_valid(const RequestAttributionDTO& attribution) noexcept {
  return attribution.locally_valid() &&
         (!attribution.principal_id.has_value() ||
          bounded_utf8(*attribution.principal_id, kMaximumStopRequestLabelBytes));
}

[[nodiscard]] bool portable_token(std::string_view value, std::size_t maximum) noexcept {
  return !value.empty() && value.size() <= maximum &&
         std::all_of(value.begin(), value.end(), [](const char character) {
           return (character >= 'A' && character <= 'Z') ||
                  (character >= 'a' && character <= 'z') ||
                  (character >= '0' && character <= '9') || character == '_' || character == '.' ||
                  character == ':' || character == '-';
         });
}

template <typename StrongId>
[[nodiscard]] bool bounded_identity(const StrongId& identity) noexcept {
  return !identity.empty() && identity.value().size() <= kMaximumStopIdentityBytes &&
         identity.value().find('\0') == std::string::npos;
}

template <typename Targets, typename Key>
[[nodiscard]] bool canonical_targets(const Targets& targets, Key&& key) {
  return !targets.empty() && targets.size() <= kMaximumStopSelectorTargets &&
         std::is_sorted(
             targets.begin(), targets.end(),
             [&](const auto& left, const auto& right) { return key(left) < key(right); }) &&
         std::adjacent_find(targets.begin(), targets.end(),
                            [&](const auto& left, const auto& right) {
                              return key(left) == key(right);
                            }) == targets.end();
}

}  // namespace

std::optional<std::string_view> stop_intent_field_error(const StopIntentDTO& intent) noexcept {
  if (!context_valid(intent.request_context)) {
    return "stop_request_context_invalid";
  }
  if (!attribution_valid(intent.attribution)) {
    return "stop_request_attribution_invalid";
  }
  if (intent.reason.has_value() && !bounded_utf8(*intent.reason, kMaximumStopReasonBytes)) {
    return "stop_reason_invalid";
  }

  return std::visit(
      [](const auto& targets) -> std::optional<std::string_view> {
        using Targets = std::decay_t<decltype(targets)>;
        if constexpr (std::is_same_v<Targets, std::vector<StopOperationTargetDTO>>) {
          if (!canonical_targets(targets, [](const auto& target) { return target.operation_id; })) {
            return "stop_operation_targets_not_canonical";
          }
          if (std::any_of(targets.begin(), targets.end(), [](const auto& target) {
                return !bounded_identity(target.operation_id) ||
                       !target.expected_operation_revision.nonzero();
              })) {
            return "stop_operation_target_invalid";
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<Targets, std::vector<StopComponentTargetDTO>>) {
          if (!canonical_targets(
                  targets, [](const auto& target) { return target.controllable_component_id; })) {
            return "stop_component_targets_not_canonical";
          }
          if (std::any_of(targets.begin(), targets.end(), [](const auto& target) {
                return !bounded_identity(target.controllable_component_id) ||
                       !bounded_identity(target.expected_graph_revision) ||
                       !target.expected_component_lifecycle_generation.nonzero();
              })) {
            return "stop_component_target_invalid";
          }
          return std::nullopt;
        } else if constexpr (std::is_same_v<Targets, std::vector<StopResourceTargetDTO>>) {
          if (!canonical_targets(targets, [](const auto& target) { return target.resource_id; })) {
            return "stop_resource_targets_not_canonical";
          }
          if (std::any_of(targets.begin(), targets.end(), [](const auto& target) {
                return !bounded_identity(target.resource_id) ||
                       !bounded_identity(target.expected_graph_revision);
              })) {
            return "stop_resource_target_invalid";
          }
          return std::nullopt;
        } else {
          if (!canonical_targets(targets,
                                 [](const auto& target) { return target.program_run_id; })) {
            return "stop_program_run_targets_not_canonical";
          }
          if (std::any_of(targets.begin(), targets.end(), [](const auto& target) {
                return !bounded_identity(target.program_run_id);
              })) {
            return "stop_program_run_target_invalid";
          }
          return std::nullopt;
        }
      },
      intent.selector);
}

std::optional<std::string_view> full_stop_intent_field_error(
    const FullStopIntentDTO& intent) noexcept {
  if (!context_valid(intent.request_context)) {
    return "full_stop_request_context_invalid";
  }
  if (!attribution_valid(intent.attribution)) {
    return "full_stop_request_attribution_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> malformed_stop_ingress_field_error(
    const MalformedStopIngressDTO& ingress) noexcept {
  if (!portable_token(ingress.request_id.value(), kMaximumStopIdentityBytes) ||
      !portable_token(ingress.receiver_owner_epoch.value(), kMaximumStopIdentityBytes)) {
    return "malformed_stop_ingress_identity_invalid";
  }
  if (!attribution_valid(ingress.attribution)) {
    return "malformed_stop_ingress_attribution_invalid";
  }
  if (!bounded_utf8(ingress.recognized_route, kMaximumStopRequestLabelBytes)) {
    return "malformed_stop_ingress_route_invalid";
  }
  if (!bounded_utf8(ingress.reason, kMaximumStopMalformedReasonBytes) ||
      (ingress.reason != "recognized_stop_payload_invalid" &&
       ingress.reason != "recognized_stop_body_oversized")) {
    return "malformed_stop_ingress_reason_invalid";
  }
  return std::nullopt;
}

std::optional<std::string_view> operation_control_request_field_error(
    const OperationControlRequestDTO& request) noexcept {
  if (!context_valid(request.request_context)) {
    return "operation_control_request_context_invalid";
  }
  if (!bounded_identity(request.operation_id) || !request.expected_operation_revision.nonzero() ||
      request.expected_operation_revision.value() >
          static_cast<std::uint64_t>(kMaximumStopDurationNs)) {
    return "operation_control_target_invalid";
  }
  if (request.reason.has_value() && !bounded_utf8(*request.reason, kMaximumStopReasonBytes)) {
    return "operation_control_reason_invalid";
  }
  if (!attribution_valid(request.attribution)) {
    return "operation_control_attribution_invalid";
  }
  if (request.execution_scope.has_value() &&
      !valid_program_node_execution_scope(*request.execution_scope)) {
    return "operation_control_scope_invalid";
  }
  return std::nullopt;
}

}  // namespace manyforge::core

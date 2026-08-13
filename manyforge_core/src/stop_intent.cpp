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
        } else {
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

}  // namespace manyforge::core

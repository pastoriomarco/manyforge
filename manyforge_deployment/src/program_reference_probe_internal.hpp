#pragma once
#include "manyforge_deployment/program_binding_compiler.hpp"
#include "manyforge_deployment/capability_definition_codec.hpp"
#include "manyforge_deployment/command_schema_codec.hpp"
#include "manyforge_deployment/component_type_codec.hpp"
#include "manyforge_deployment/artifact_codec.hpp"
#include "manyforge_deployment/io_signal_catalog.hpp"
#include <algorithm>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string_view>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace manyforge::deployment::reference_probe {
class TargetUnavailable final : public std::runtime_error {
 public:
  TargetUnavailable(const ProgramBindingUnresolvedReason reason,
                    const std::string& message)
      : std::runtime_error(message), reason_(reason) {}

  [[nodiscard]] inline ProgramBindingUnresolvedReason reason() const noexcept {
    return reason_;
  }

 private:
  ProgramBindingUnresolvedReason reason_;
};

struct QualifiedCatalogClosure final {
  std::vector<QualifiedCapabilityDefinition> capabilities;
  std::vector<QualifiedCommandSchema> command_schemas;
};

struct ResolvedResourceRole final {
  core::ProgramResourceRoleBindingDTO binding;
  std::vector<std::optional<core::CoordinateKind>> coordinate_kinds;

  friend bool operator==(const ResolvedResourceRole&,
                         const ResolvedResourceRole&) = default;
};

template <typename Value, typename Predicate>
[[nodiscard]] inline const Value* find_exactly_one(std::span<const Value> values,
                                            Predicate predicate) {
  const Value* result = nullptr;
  for (const auto& value : values) {
    if (!predicate(value)) {
      continue;
    }
    if (result != nullptr) {
      return nullptr;
    }
    result = &value;
  }
  return result;
}

[[noreturn]] void fail(std::string message);

[[noreturn]] void unavailable(const ProgramBindingUnresolvedReason reason,
                              const std::string& message);

[[nodiscard]] QualifiedCatalogClosure load_qualified_catalog_closure(
    const core::RetainedBundleDTO& bundle,
    const ICatalogArtifactSource& store);

[[nodiscard]] const QualifiedCapabilityDefinition*
find_qualified_capability(
    std::span<const QualifiedCapabilityDefinition> capabilities,
    std::string_view id, std::string_view revision);

[[nodiscard]] QualifiedComponentType load_component_type(
    const core::RetainedBundleDTO& bundle,
    const core::ControllableComponentDTO& component,
    std::span<const QualifiedCapabilityDefinition> retained_capabilities,
    const ICatalogArtifactSource& store);

[[nodiscard]] const core::ControllableComponentDTO& require_component(
    const core::CellGraphDTO& graph,
    const core::ControllableComponentId& id);

[[nodiscard]] const core::ModelInstanceDTO& require_model(
    const core::CellGraphDTO& graph, const core::ModelInstanceId& id);

[[nodiscard]] bool contains(std::span<const std::string> values,
                            std::string_view expected);

[[nodiscard]] core::ProgramComponentBindingDTO resolve_component_binding(
    const core::ControllableComponentId& component_id,
    const core::CellGraphDTO& graph,
    const QualifiedComponentType& component_type);

[[nodiscard]] const core::ComponentTypeCommandTargetDTO& require_target_type(
    const QualifiedComponentType& component_type,
    const core::CommandTargetId& target_id);

[[nodiscard]] ResolvedResourceRole resolve_resource_role(
    const VerifiedRetainedModelMap& model_map,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::ControllableComponentId& component_id,
    const core::CommandTargetId& target_id,
    const core::ResourceRoleId& resource_role_id);

[[nodiscard]] const core::ControlBindingDTO* find_control_binding(
    const core::CellGraphDTO& graph,
    const core::ControlBindingId& binding_id);

[[nodiscard]] std::set<core::ResourceRoleId> capability_resource_roles(
    const core::ProgramCapabilityBindingDTO& capability,
    const core::ProgramTargetReferenceDTO& target,
    const core::CellGraphDTO& graph);

[[nodiscard]] std::set<core::ResourceRoleId> signal_resource_roles(
    const core::ProgramTargetReferenceDTO& target,
    std::span<const core::ProgramSignalReferenceDTO> references,
    std::span<const RetainedIoSignalConfiguration> catalog);

[[nodiscard]] std::vector<core::ResourceRoleId> command_schema_resource_roles(
    const QualifiedCommandSchema &schema,
    const core::ComponentTypeCommandTargetDTO &target_type);

[[nodiscard]] std::optional<core::ProgramCapabilityBindingDTO>
resolve_capability(
    const core::ProgramCapabilityRequirementDTO& requirement,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const core::ControllableComponentDTO& component,
    const core::CellGraphDTO& graph,
    const core::RetainedBundleDTO& bundle,
    const QualifiedCatalogClosure& catalog,
    const VerifiedRetainedModelMap& model_map);
[[nodiscard]] std::vector<core::ProgramTargetFrameAssociationDTO>
resolve_target_frame_associations(
    const core::ProgramTargetReferenceDTO& program_target,
    std::span<const core::ProgramFrameReferenceDTO> frame_references,
    const core::ComponentTypeCommandTargetDTO& target_type,
    const CompositionTargetResolution& target,
    const VerifiedRetainedModelMap& model_map,
    std::span<const core::ProgramFrameBindingDTO> frames);
[[nodiscard]] std::vector<RobotTcp> compile_robot_tcps(
    const core::RetainedBundleDTO& bundle,
    const VerifiedRetainedModelMap& retained_model_map,
    const core::ControllableComponentId& robot,
    const core::CommandTargetId& robot_target,
    std::span<const QualifiedCapabilityDefinition> capabilities,
    const ICatalogArtifactSource& catalog_artifact_store);

[[nodiscard]] std::set<std::string> target_tcp_roles(
    const core::ComponentTypeCommandTargetDTO& type,
    const CompositionTargetResolution* composed, bool robot_target_row);
} // namespace manyforge::deployment::reference_probe

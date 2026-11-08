#pragma once

#include <string>
#include <variant>

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Exact deployment cell frame consumed only after retained-graph resolution.
struct ResolvedCellFrameDTO final {
  CellId cell_id;
  std::string compiled_frame_name;

  friend bool operator==(const ResolvedCellFrameDTO&,
                         const ResolvedCellFrameDTO&) = default;
};

/// Exact frame on a model instance owned by one controllable component.
struct ResolvedComponentModelFrameDTO final {
  ControllableComponentId controllable_component_id;
  ModelInstanceId model_instance_id;
  FrameRoleId frame_role_id;
  std::string compiled_frame_name;
  ModelRevision model_revision;

  friend bool operator==(const ResolvedComponentModelFrameDTO&,
                         const ResolvedComponentModelFrameDTO&) = default;
};

/// Exact frame on an unowned passive model instance.
struct ResolvedPassiveModelFrameDTO final {
  ModelInstanceId model_instance_id;
  FrameRoleId frame_role_id;
  std::string compiled_frame_name;
  ModelRevision model_revision;

  friend bool operator==(const ResolvedPassiveModelFrameDTO&,
                         const ResolvedPassiveModelFrameDTO&) = default;
};

/// Closed internal frame identity. Public program
/// intent carries logical frame roles; generated names appear only after exact
/// retained-graph resolution.
using ResolvedFrameDTO =
    std::variant<ResolvedCellFrameDTO, ResolvedComponentModelFrameDTO,
                 ResolvedPassiveModelFrameDTO>;

[[nodiscard]] inline const std::string& compiled_frame_name(
    const ResolvedFrameDTO& frame) noexcept {
  return std::visit(
      [](const auto& value) -> const std::string& {
        return value.compiled_frame_name;
      },
      frame);
}

/// Portable role of a resolved frame; empty for a cell frame.
[[nodiscard]] inline std::string frame_role_id(const ResolvedFrameDTO& frame) {
  if (const auto* component = std::get_if<ResolvedComponentModelFrameDTO>(&frame))
    return component->frame_role_id.value();
  if (const auto* passive = std::get_if<ResolvedPassiveModelFrameDTO>(&frame))
    return passive->frame_role_id.value();
  return {};
}

}  // namespace manyforge::core

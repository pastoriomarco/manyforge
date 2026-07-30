#pragma once

#include <array>
#include <string_view>

#include "manyforge_deployment/model_source.hpp"

namespace manyforge::deployment::detail {

struct ModelSourceMediaContractRow final {
  ModelSourceArtifactKind artifact_kind;
  std::string_view artifact_kind_id;
  std::string_view media_type;
  std::string_view source_role;
};

/// Sole closed source/media classification registry.
///
/// Model-source preflight and the compiler-contract manifest consume these
/// same rows, so accepted source semantics cannot drift from retained build
/// authority.
inline constexpr std::array<ModelSourceMediaContractRow, 6U>
    kModelSourceMediaContractRows{{
        {ModelSourceArtifactKind::kColladaMesh, "collada_mesh",
         "model/vnd.collada+xml", "dependency"},
        {ModelSourceArtifactKind::kJpegTexture, "jpeg_texture",
         "image/jpeg", "dependency"},
        {ModelSourceArtifactKind::kPngTexture, "png_texture",
         "image/png", "dependency"},
        {ModelSourceArtifactKind::kSrdfXmlEntrypoint,
         "srdf_xml_entrypoint", "application/xml", "srdf"},
        {ModelSourceArtifactKind::kStlMesh, "stl_mesh", "model/stl",
         "dependency"},
        {ModelSourceArtifactKind::kUrdfXmlEntrypoint,
         "urdf_xml_entrypoint", "application/xml", "urdf"},
    }};

[[nodiscard]] constexpr const ModelSourceMediaContractRow*
find_model_source_media_contract(
    ModelSourceArtifactKind artifact_kind) noexcept {
  for (const auto& row : kModelSourceMediaContractRows) {
    if (row.artifact_kind == artifact_kind) {
      return &row;
    }
  }
  return nullptr;
}

[[nodiscard]] constexpr const ModelSourceMediaContractRow*
find_model_source_dependency_media_contract(
    std::string_view media_type) noexcept {
  for (const auto& row : kModelSourceMediaContractRows) {
    if (row.source_role == "dependency" &&
        row.media_type == media_type) {
      return &row;
    }
  }
  return nullptr;
}

}  // namespace manyforge::deployment::detail

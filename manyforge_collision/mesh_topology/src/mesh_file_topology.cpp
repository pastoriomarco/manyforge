#include <manyforge_collision/mesh_file_topology.hpp>

#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <string>
#include <vector>

namespace manyforge::collision {
namespace {

struct Collected {
  std::vector<std::array<double, 3>> vertices;
  std::vector<std::array<std::uint32_t, 3>> triangles;
};

/// coal's mesh loader: each node's meshes under the node's transform composed
/// with its ancestors' (the root's own excluded), triangles only.
void collect(const aiScene& scene, const aiNode& node, const std::array<double, 3>& scale,
             Collected& out) {
  aiMatrix4x4 transform = node.mTransformation;
  for (const aiNode* parent = node.mParent; parent != nullptr; parent = parent->mParent) {
    if (parent->mParent != nullptr) transform = parent->mTransformation * transform;
  }
  for (unsigned int i = 0; i < node.mNumMeshes; ++i) {
    const aiMesh& mesh = *scene.mMeshes[node.mMeshes[i]];
    const auto offset = static_cast<std::uint32_t>(out.vertices.size());
    for (unsigned int j = 0; j < mesh.mNumVertices; ++j) {
      aiVector3D p = mesh.mVertices[j];
      p *= transform;
      out.vertices.push_back({p.x * scale[0], p.y * scale[1], p.z * scale[2]});
    }
    for (unsigned int j = 0; j < mesh.mNumFaces; ++j) {
      const aiFace& face = mesh.mFaces[j];
      if (face.mNumIndices != 3U) continue;
      out.triangles.push_back(
          {offset + face.mIndices[0], offset + face.mIndices[1], offset + face.mIndices[2]});
    }
  }
  for (unsigned int i = 0; i < node.mNumChildren; ++i) {
    collect(scene, *node.mChildren[i], scale, out);
  }
}

}  // namespace

std::optional<MeshTopology> read_mesh_topology(const std::string_view bytes,
                                               const std::string_view format_hint,
                                               const std::array<double, 3>& scale) {
  Assimp::Importer importer;
  importer.SetPropertyInteger(
      AI_CONFIG_PP_RVC_FLAGS,
      aiComponent_TANGENTS_AND_BITANGENTS | aiComponent_COLORS | aiComponent_BONEWEIGHTS |
          aiComponent_ANIMATIONS | aiComponent_LIGHTS | aiComponent_CAMERAS |
          aiComponent_TEXTURES | aiComponent_TEXCOORDS | aiComponent_MATERIALS |
          aiComponent_NORMALS);
  importer.SetPropertyInteger(AI_CONFIG_PP_SBP_REMOVE,
                              aiPrimitiveType_LINE | aiPrimitiveType_POINT);
  importer.SetPropertyInteger(AI_CONFIG_PP_FD_REMOVE, 1);
  const std::string hint(format_hint);
  const aiScene* scene = importer.ReadFileFromMemory(
      bytes.data(), bytes.size(),
      aiProcess_SortByPType | aiProcess_Triangulate | aiProcess_RemoveComponent |
          aiProcess_ImproveCacheLocality | aiProcess_FindDegenerates |
          aiProcess_JoinIdenticalVertices,
      hint.c_str());
  if (scene == nullptr || scene->mRootNode == nullptr) return std::nullopt;
  Collected collected;
  collect(*scene, *scene->mRootNode, scale, collected);
  return check_mesh_topology(collected.vertices, collected.triangles);
}

}  // namespace manyforge::collision

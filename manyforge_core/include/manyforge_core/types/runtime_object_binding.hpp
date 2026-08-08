#pragma once

#include <manyforge_core/types/strong_id.hpp>

namespace manyforge::core {

/// Typed Program runtime-object selection. Public intent may supply the
/// portable key and selected logical-scene resource; an owner must still
/// resolve the key through the exact Program Binding and validate the selected
/// resource against the retained scene before producing any runtime effect.
struct RuntimeObjectReferenceBindingDTO final {
  RuntimeObjectId runtime_object_id;
  SceneResourceId scene_resource_id;

  friend bool operator==(const RuntimeObjectReferenceBindingDTO&,
                         const RuntimeObjectReferenceBindingDTO&) = default;
};

}  // namespace manyforge::core

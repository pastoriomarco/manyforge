#pragma once

#include <string_view>

namespace manyforge::headless {

inline constexpr std::string_view kManagerSourcePayloadApiVersion =
    "manyforge.manager_source.v1";
inline constexpr std::string_view kLogicalSceneSourcePayloadApiVersion =
    "manyforge.logical_scene_source.v1";
inline constexpr std::string_view kSceneRuntimeSourcePayloadApiVersion =
    "manyforge.scene_runtime_source.v1";
inline constexpr std::string_view kKernelOwnerUnavailableReasonCode =
    "kernel_owner_unavailable";

} // namespace manyforge::headless

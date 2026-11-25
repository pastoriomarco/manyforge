#pragma once

#include <cstddef>
#include <string_view>

namespace manyforge::headless {

inline constexpr std::string_view kManagerSourcePayloadApiVersion =
    "manyforge.manager_source.v2";
inline constexpr std::string_view kLogicalSceneSourcePayloadApiVersion =
    "manyforge.logical_scene_source.v1";
inline constexpr std::string_view kSceneRuntimeSourcePayloadApiVersion =
    "manyforge.scene_runtime_source.v1";
/// The Manager's status stream (OD-216): its producer is the Manager daemon
/// (`manager_status_stream.hpp`), its reader the Headless API's
/// `ManagerStatusMirror` (`runtime_snapshot.hpp`).
inline constexpr std::string_view kManagerStatusStreamApiVersion =
    "manyforge.manager_status_stream.v2";
/// The `scene_changes` section of that stream (OD-400): the last scene
/// commit outcomes outside a Program node attempt, for History.
inline constexpr std::size_t kSceneChangeRingCapacity = 32U;
inline constexpr std::string_view kKernelOwnerUnavailableReasonCode =
    "kernel_owner_unavailable";

} // namespace manyforge::headless

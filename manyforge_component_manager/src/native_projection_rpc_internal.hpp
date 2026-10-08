#pragma once

#include <cstddef>
#include <cstdint>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumNativeProjectionRpcPacketBytes =
    256U * 1024U;

struct NativeProjectionPrepareRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  manyforge::core::RetainedProductDTO planning_collision_model;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() &&
           !candidate_retained_bundle_id.empty() &&
           !candidate_graph_revision.empty() &&
           planning_collision_model.product_kind ==
               manyforge::core::RetainedProductKind::kPlanningCollisionModel &&
           !planning_collision_model.product_id.empty() &&
           !planning_collision_model.product_contract_id.empty() &&
           !planning_collision_model.product_contract_revision.empty();
  }
};

struct NativeProjectionPrepareResult final {
  NativeProjectionPrepareRequest request;
};

struct NativeProjectionHealthQuery final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
};

struct NativeProjectionTransactionRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::SceneTransactionDTO transaction;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && !transaction.request_id.empty() &&
           transaction.transaction_sequence != 0U &&
           transaction.attribution.locally_valid() &&
           transaction.prior_projection_revision.valid() &&
           transaction.proposed_projection_revision.valid() &&
           !transaction.original_request_digest.empty();
  }
};

struct NativeProjectionRetainRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::ProjectionRevisionDTO projection_revision;
  manyforge::core::ProjectionLeaseId projection_lease_id;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && projection_revision.valid() &&
           !projection_lease_id.empty();
  }
};

struct NativeProjectionReleaseRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::ProjectionLeaseId projection_lease_id;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && !projection_lease_id.empty();
  }
};

struct NativeProjectionSceneFrameRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::SceneFrameResolutionQueryDTO query;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && query.valid();
  }
};

struct NativeProjectionEffectivePolicyRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::LogicalSceneSnapshotDTO logical_scene;
  std::uint64_t effective_policy_revision_id{};

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && logical_scene.locally_valid() &&
           effective_policy_revision_id != 0U;
  }
};

/// Freshness-only confirmation for one unchanged committed object at the
/// exact expected projection revision. Not an owner transaction.
struct NativeProjectionProviderFreshnessRequest final {
  manyforge::core::ProjectionNativeRuntimeBindingDTO native_runtime_binding;
  manyforge::core::ProviderFreshnessConfirmationDTO confirmation;

  [[nodiscard]] bool valid() const noexcept {
    return native_runtime_binding.valid() && confirmation.valid();
  }
};

/// Exact acknowledgement of an applied freshness confirmation; it echoes the
/// unchanged projection revision, the object and the accepted sequence.
struct NativeProjectionProviderFreshnessResult final {
  manyforge::core::ProjectionRevisionDTO projection_revision;
  manyforge::core::SceneResourceId scene_resource_id;
  std::uint64_t provider_source_sequence{};

  [[nodiscard]] bool valid() const noexcept {
    return projection_revision.valid() && !scene_resource_id.empty();
  }
};

struct NativeProjectionRetainResult final {
  manyforge::core::ProjectionLeaseBindingDTO projection_binding;
};

struct NativeProjectionReleaseResult final {
  manyforge::core::ProjectionLeaseId projection_lease_id;
};

using NativeProjectionRpcRequest =
    std::variant<NativeProjectionPrepareRequest, NativeProjectionHealthQuery,
                 NativeProjectionTransactionRequest,
                 manyforge::core::NativeProjectionRebuildDTO,
                 manyforge::core::NativeProjectionBootstrapRebuildDTO,
                 manyforge::core::NativeProjectionBootstrapStatusQueryDTO,
                 NativeProjectionRetainRequest,
                 NativeProjectionReleaseRequest,
                 NativeProjectionSceneFrameRequest,
                 NativeProjectionEffectivePolicyRequest,
                 NativeProjectionProviderFreshnessRequest>;

struct NativeProjectionRpcFailure final {
  manyforge::core::ErrorDTO error;
};

using NativeProjectionRpcResponse =
    std::variant<NativeProjectionRpcFailure, NativeProjectionPrepareResult,
                 manyforge::core::ProjectionHealthDTO,
                 manyforge::core::ProjectionCommitResultDTO,
                 manyforge::core::NativeProjectionBootstrapRebuildResultDTO,
                 manyforge::core::NativeProjectionBootstrapStatusDTO,
                 NativeProjectionRetainResult,
                 NativeProjectionReleaseResult,
                 manyforge::core::SceneFrameResolutionReceiptDTO,
                 manyforge::core::CollisionPolicySnapshot,
                 NativeProjectionProviderFreshnessResult>;

[[nodiscard]] std::string
EncodeNativeProjectionRpcRequest(const NativeProjectionRpcRequest &request);
[[nodiscard]] NativeProjectionRpcRequest
DecodeNativeProjectionRpcRequest(std::string_view bytes);

[[nodiscard]] std::string
EncodeNativeProjectionRpcResponse(const NativeProjectionRpcResponse &response);
[[nodiscard]] NativeProjectionRpcResponse
DecodeNativeProjectionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail

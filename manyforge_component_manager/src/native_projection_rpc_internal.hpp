#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <manyforge_core/ports/iplanner_scene_projection.hpp>
#include <string>
#include <string_view>
#include <variant>

namespace manyforge::component_manager::detail {

inline constexpr std::size_t kMaximumNativeProjectionRpcPacketBytes =
    256U * 1024U;

/// Thrown by the codec when a collection exceeds its entry bound or a packet
/// its byte bound. Both endpoints refuse the one request with
/// `native_projection_rpc_bound_exceeded`; neither process ends.
class NativeProjectionRpcBoundExceeded final : public std::invalid_argument {
public:
  using std::invalid_argument::invalid_argument;
};

struct NativeProjectionPrepareRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::RetainedBundleId candidate_retained_bundle_id;
  manyforge::core::GraphRevision candidate_graph_revision;
  manyforge::core::RetainedProductDTO planning_collision_model;

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() &&
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
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  std::optional<manyforge::core::ProjectionRevisionDTO>
      selected_projection_revision{};
};

struct NativeProjectionTransactionRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::SceneTransactionDTO transaction;

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() &&
           transaction.transaction_sequence != 0U &&
           transaction.attribution.locally_valid() &&
           transaction.prior_projection_revision.valid() &&
           transaction.proposed_projection_revision.valid() &&
           !transaction.original_request_digest.empty();
  }
};

struct NativeProjectionRetainRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::ProjectionRevisionDTO projection_revision;
  manyforge::core::ProjectionLeaseId projection_lease_id;

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() && projection_revision.valid() &&
           !projection_lease_id.empty();
  }
};

struct NativeProjectionReleaseRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::ProjectionLeaseId projection_lease_id;

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() && !projection_lease_id.empty();
  }
};

struct NativeProjectionSceneFrameRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::SceneFrameResolutionQueryDTO query;

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() && query.valid();
  }
};

struct NativeProjectionEffectivePolicyRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::LogicalSceneSnapshotDTO logical_scene;
  std::uint64_t effective_policy_revision_id{};

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() && logical_scene.locally_valid() &&
           effective_policy_revision_id != 0U;
  }
};

/// One Scene commit preflight (OD-376): the health for the selected model
/// and, given the owner's transaction, its candidate's effective collision
/// policy. The candidate itself never crosses: the host applies the owner's
/// resolved delta to its own predecessor.
struct NativeProjectionCommitPreflightRequest final {
  manyforge::core::ProjectionExecutionHostBindingDTO execution_host_binding;
  manyforge::core::ProjectionRevisionDTO selected_projection_revision;
  std::optional<manyforge::core::SceneTransactionDTO> transaction{};

  [[nodiscard]] bool valid() const noexcept {
    return execution_host_binding.valid() &&
           selected_projection_revision.valid();
  }
};

/// Absent `effective_policy`: no transaction, or the projection is not healthy
/// at its predecessor.
struct NativeProjectionCommitPreflightResult final {
  manyforge::core::ProjectionHealthDTO health;
  std::optional<std::variant<manyforge::core::CollisionPolicySnapshot,
                             manyforge::core::ErrorDTO>>
      effective_policy{};
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
                 NativeProjectionCommitPreflightRequest>;

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
                 NativeProjectionCommitPreflightResult>;

[[nodiscard]] std::string
EncodeNativeProjectionRpcRequest(const NativeProjectionRpcRequest &request);
[[nodiscard]] NativeProjectionRpcRequest
DecodeNativeProjectionRpcRequest(std::string_view bytes);

[[nodiscard]] std::string
EncodeNativeProjectionRpcResponse(const NativeProjectionRpcResponse &response);
/// The response, or, when it exceeds a codec bound, the typed refusal
/// `native_projection_rpc_bound_exceeded` of this one request.
[[nodiscard]] std::string EncodeNativeProjectionRpcResponseOrRefusal(
    const NativeProjectionRpcResponse &response);
[[nodiscard]] NativeProjectionRpcResponse
DecodeNativeProjectionRpcResponse(std::string_view bytes);

} // namespace manyforge::component_manager::detail

#pragma once

#include <manyforge_core/ports/icclc_component_command_readiness_port.hpp>
#include <manyforge_roboplan/retained_native_cell_runtime.hpp>

#include <algorithm>
#include <cstddef>
#include <exception>
#include <functional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace manyforge::roboplan_backend {

/// Exact candidate inputs needed to re-seal one component in the stable
/// native Kernel. The builder is owner-supplied because only the composition
/// owner can rebuild its resolver from its exact physical registrations.
struct RetainedNativeCellReadinessCandidate final {
  std::vector<core::RuntimeParticipantExecutionRegistration> registrations;
  core::RuntimeParticipantAdmissionResolver participant_resolver;
  /// Pre-staged, no-throw adoption of component-owner caches. The builder
  /// prepares every replacement before Kernel publication; this callback
  /// performs swaps only after exact Kernel readback under the same claim.
  std::function<void()> adopt_component_owners;
};

using RetainedNativeCellReadinessCandidateBuilder =
    std::function<core::Result<RetainedNativeCellReadinessCandidate>(
        const core::ComponentCommandReadinessPublicationDTO&,
        const std::vector<core::RuntimeParticipantExecutionRegistration>&)>;

/// Interim CCLC readiness adapter for the stable native cell Kernel.
///
/// Close and Read are direct Kernel projections. Publish stages the exact
/// generation-only re-seal and resolver, verifies the Kernel's exact readiness
/// readback, then adopts the same snapshot with the cell owner's no-throw swap.
/// Its registration snapshot read is valid only while the caller holds the
/// lifecycle claim, exact parent Apply fence, or restart-recovery exclusion.
class RetainedNativeCellCommandReadinessPort final
    : public core::ICCLCComponentCommandReadinessPort {
 public:
  explicit RetainedNativeCellCommandReadinessPort(
      RetainedNativeCellRuntime& cell,
      RetainedNativeCellReadinessCandidateBuilder candidate_builder)
      : cell_(cell), candidate_builder_(std::move(candidate_builder)) {
    if (!candidate_builder_) {
      throw std::invalid_argument(
          "native cell readiness requires an explicit candidate builder");
    }
  }

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  CloseComponentCommandEligibility(
      const core::LifecycleTransactionId& lifecycle_transaction_id,
      const core::ControllableComponentId& controllable_component_id) override {
    return cell_.addressed_kernel().close_component_command_eligibility(
        lifecycle_transaction_id, controllable_component_id);
  }

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  PublishComponentCommandReadiness(
      const core::ComponentCommandReadinessPublicationDTO& publication) override {
    if (!publication.locally_valid()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvalidRequest)),
              "native cell readiness publication is invalid",
              {{"reason", "component_readiness_publication_invalid"}}});
    }

    // These owner snapshots are not independently synchronized; all intended
    // callers hold the transition authority which excludes competing cell
    // composition/lifecycle mutation until snapshot adoption completes.
    const auto& current = cell_.participant_registrations();
    auto staged = [&]() {
      try {
        return candidate_builder_(publication, current);
      } catch (const std::exception& exception) {
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "native cell readiness candidate construction threw",
                {{"reason", "component_readiness_candidate_exception"},
                 {"message", exception.what()}}});
      } catch (...) {
        return core::Result<RetainedNativeCellReadinessCandidate>::Err(
            core::ErrorDTO{
                std::string(core::to_string(core::ErrorCode::kInternalError)),
                "native cell readiness candidate construction threw",
                {{"reason", "component_readiness_candidate_exception"}}});
      }
    }();
    if (!staged.ok()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          staged.error());
    }
    auto exact_candidate = validate_generation_only_reseal(
        publication, current, staged.value().registrations);
    if (!exact_candidate.ok()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          exact_candidate.error());
    }
    if (!staged.value().participant_resolver &&
        (!current.empty() || cell_.participant_resolver())) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "re-sealed native registrations require their rebuilt admission resolver",
              {{"reason", "component_readiness_resolver_missing"},
               {"controllable_component_id",
               publication.controllable_component_id.value()}}});
    }
    if (!staged.value().adopt_component_owners) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kMissingReference)),
              "native cell readiness has no staged component-owner adoption",
              {{"reason", "component_readiness_owner_adoption_missing"},
               {"controllable_component_id",
                publication.controllable_component_id.value()}}});
    }

    // Keep an identical resolver for the cell owner's retained snapshot. Make
    // this potentially allocating copy before changing Kernel state.
    core::RuntimeParticipantAdmissionResolver kernel_resolver;
    try {
      kernel_resolver = staged.value().participant_resolver;
    } catch (const std::exception& exception) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInternalError)),
              "native cell admission resolver could not be copied for publication",
              {{"reason", "component_readiness_resolver_copy_failed"},
               {"message", exception.what()}}});
    } catch (...) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInternalError)),
              "native cell admission resolver could not be copied for publication",
              {{"reason", "component_readiness_resolver_copy_failed"}}});
    }

    auto published = cell_.addressed_kernel().publish_component_command_readiness(
        publication, std::move(kernel_resolver));
    if (!published.ok()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          published.error());
    }
    auto readback = cell_.addressed_kernel().component_command_readiness(
        publication.controllable_component_id);
    if (!readback.ok()) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          readback.error());
    }
    const core::ComponentCommandReadinessDTO expected{
        publication.controllable_component_id,
        publication.component_lifecycle_generation,
        publication.command_eligible};
    if (published.value() != expected || readback.value() != expected) {
      return core::Result<core::ComponentCommandReadinessDTO>::Err(
          core::ErrorDTO{
              std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
              "native cell Kernel readiness readback does not match the requested component result",
              {{"reason", "component_readiness_readback_mismatch"},
               {"controllable_component_id",
                publication.controllable_component_id.value()}}});
    }

    // No fallible work follows the verified Kernel publication. Adoption is
    // an owner-provided no-throw swap under the same held transition authority.
    cell_.AdoptVerifiedComponentReadinessSnapshot(
        std::move(staged.value().registrations),
        std::move(staged.value().participant_resolver));
    staged.value().adopt_component_owners();
    return core::Result<core::ComponentCommandReadinessDTO>::Ok(
        std::move(readback.value()));
  }

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  PublishComponentCommandReadiness(
      const core::ComponentCommandReadinessPublicationDTO& publication,
      const core::ComponentLifecycleCurrentSnapshotDTO& actual_snapshot,
      const std::vector<core::BrandLifecycleObservationDTO>& actual_observations) override {
    const bool provider = std::any_of(actual_snapshot.domains.begin(), actual_snapshot.domains.end(),
                                     [](const auto& domain) { return domain.provider() != nullptr; });
    if (provider) {
      const auto installed = cell_.addressed_kernel().publish_runtime_provider_configuration(
          publication, actual_snapshot, actual_observations);
      if (!installed.ok())
        return core::Result<core::ComponentCommandReadinessDTO>::Err(installed.error());
    }
    // The same Kernel still owns complete component permission. Publishing
    // actual driver facts above does not itself open command admission.
    return PublishComponentCommandReadiness(publication);
  }

  [[nodiscard]] core::Result<core::ComponentCommandReadinessDTO>
  ReadComponentCommandReadiness(
      const core::ControllableComponentId& controllable_component_id)
      const override {
    return cell_.addressed_kernel().component_command_readiness(
        controllable_component_id);
  }

 private:
  [[nodiscard]] static bool same_registration(
      const core::RuntimeParticipantExecutionRegistration& lhs,
      const core::RuntimeParticipantExecutionRegistration& rhs) {
    const auto same_readiness = [&]() {
      if (lhs.stop_readiness.has_value() != rhs.stop_readiness.has_value()) {
        return false;
      }
      return !lhs.stop_readiness.has_value() ||
             lhs.stop_readiness->value() == rhs.stop_readiness->value();
    };
    const auto same_policy = [&]() {
      if (lhs.participant_policy.has_value() !=
          rhs.participant_policy.has_value()) {
        return false;
      }
      return !lhs.participant_policy.has_value() ||
             lhs.participant_policy->value() == rhs.participant_policy->value();
    };
    return lhs.binding.value() == rhs.binding.value() && same_readiness() &&
           lhs.retained_default_stop_limits ==
               rhs.retained_default_stop_limits &&
           lhs.stop_node_category == rhs.stop_node_category &&
           lhs.engine == rhs.engine &&
           lhs.stop_execution_port == rhs.stop_execution_port &&
           same_policy() &&
           lhs.prepared_motion_engine == rhs.prepared_motion_engine &&
           lhs.default_operator_stop_profile ==
               rhs.default_operator_stop_profile &&
           lhs.default_fault_stop_profile == rhs.default_fault_stop_profile &&
           lhs.deployment_maximum_tcp_linear_speed_mps ==
               rhs.deployment_maximum_tcp_linear_speed_mps &&
           lhs.intrinsic_direct_control == rhs.intrinsic_direct_control &&
           lhs.direct_control_joint_limits == rhs.direct_control_joint_limits &&
           lhs.motion_takeover_preparation_budget_s ==
               rhs.motion_takeover_preparation_budget_s &&
           lhs.motion_takeover_planning_budget_s ==
               rhs.motion_takeover_planning_budget_s;
  }

  [[nodiscard]] static core::Result<void> validate_generation_only_reseal(
      const core::ComponentCommandReadinessPublicationDTO& publication,
      const std::vector<core::RuntimeParticipantExecutionRegistration>& current,
      const std::vector<core::RuntimeParticipantExecutionRegistration>& candidate) {
    if (current.size() != candidate.size()) {
      return core::Result<void>::Err(core::ErrorDTO{
          std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
          "component readiness candidate changed the participant set",
          {{"reason", "component_readiness_participant_set_changed"},
           {"controllable_component_id",
            publication.controllable_component_id.value()}}});
    }

    for (std::size_t index = 0; index < current.size(); ++index) {
      auto expected = current[index];
      if (expected.binding.value().controllable_component_id ==
              publication.controllable_component_id &&
          expected.stop_readiness.has_value() &&
          expected.stop_readiness->value().component_lifecycle_generation !=
              publication.component_lifecycle_generation) {
        auto resealed = core::rebind_runtime_participant_stop_readiness_generation(
            *expected.stop_readiness,
            publication.component_lifecycle_generation);
        if (!resealed.ok()) {
          return core::Result<void>::Err(resealed.error());
        }
        expected.stop_readiness = std::move(resealed.value());
      }
      if (expected.binding.value().controllable_component_id ==
              publication.controllable_component_id && expected.binding.value().io_output_binding &&
          expected.binding.value().io_output_binding->component_lifecycle_generation !=
              publication.component_lifecycle_generation) {
        auto value = expected.binding.value();
        value.io_output_binding->component_lifecycle_generation = publication.component_lifecycle_generation;
        auto resealed = core::verify_runtime_participant_binding(std::move(value));
        if (!resealed.ok()) return core::Result<void>::Err(resealed.error());
        expected.binding = std::move(resealed.value());
      }
      if (!same_registration(expected, candidate[index])) {
        return core::Result<void>::Err(core::ErrorDTO{
            std::string(core::to_string(core::ErrorCode::kInvariantViolation)),
            "component readiness candidate changed facts beyond its exact generation re-seal",
            {{"reason", "component_readiness_reseal_mismatch"},
             {"controllable_component_id",
              publication.controllable_component_id.value()},
             {"registration_index", std::to_string(index)}}});
      }
    }
    return core::Result<void>::Ok();
  }

  // The stable cell owner must outlive this non-owning adapter.
  RetainedNativeCellRuntime& cell_;
  RetainedNativeCellReadinessCandidateBuilder candidate_builder_;
};

}  // namespace manyforge::roboplan_backend

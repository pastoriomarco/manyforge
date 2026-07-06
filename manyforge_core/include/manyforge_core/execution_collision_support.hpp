#pragma once

#include <manyforge_core/collision/collision_validation_coordinator.hpp>
#include <manyforge_core/collision/i_collision_query_engine.hpp>
#include <manyforge_core/ports/iplanning_context.hpp>
#include <manyforge_core/types/collision.hpp>
#include <manyforge_core/types/execution_status.hpp>
#include <manyforge_core/types/trajectory_dto.hpp>

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace manyforge::core {

std::string comma_join_strings(const std::vector<std::string>& values);
std::string comma_join_doubles(const std::vector<double>& values);
std::string format_double(double value);

bool snapshot_too_old(std::int64_t now_ns,
                      std::int64_t timestamp_ns,
                      std::int64_t max_age_ns,
                      std::map<std::string, std::string>& details,
                      const std::string& timestamp_key,
                      const std::string& age_key,
                      const std::string& budget_key);
std::optional<std::string> validate_dynamic_body_freshness(
    const CollisionWorldSnapshot& snapshot,
    std::int64_t now_ns,
    std::int64_t runtime_cap_ns,
    std::map<std::string, std::string>& details);

void append_collision_failure_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle);
void append_legacy_collision_compare_diagnostics(
    std::map<std::string, std::string>& details,
    const CollisionValidationDecisionBundle& bundle,
    const TrajectoryDTO& trajectory,
    IPlanningContext& context);
CollisionCheckResultDTO collision_check_result_from_bundle(
    const CollisionValidationDecisionBundle& bundle);

bool collision_world_revision_is_set(const CollisionWorldRevision& revision);
bool collision_world_revisions_equal(const CollisionWorldRevision& lhs,
                                     const CollisionWorldRevision& rhs);
bool engines_require_single_context_exclusivity(
    const std::vector<std::shared_ptr<ICollisionQueryEngine>>& engines);
bool runtime_monitor_may_hold_collision_engine(ExecutionState state);

}  // namespace manyforge::core

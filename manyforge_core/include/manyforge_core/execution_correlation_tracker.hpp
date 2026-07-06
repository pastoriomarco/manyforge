#pragma once

#include <manyforge_core/types/execute_request.hpp>
#include <manyforge_core/types/execution_result.hpp>

#include <map>
#include <mutex>
#include <optional>
#include <string>

namespace manyforge::core {

/// Owns execution-scoped observer/audit correlation fields.
///
/// All methods are leaf operations: they copy small values under private
/// mutexes and never call out. This keeps event/snapshot stamping out of
/// lifecycle lock-order cycles.
class ExecutionCorrelationTracker {
 public:
  void set_from_request(const ExecuteRequestDTO& request);
  void clear_all();
  void clear_move_id();
  void clear_program_run_and_action_span();

  std::string move_id() const;
  std::optional<ActionSpanDTO> action_span() const;

  void stamp_details(std::map<std::string, std::string>& details) const;
  void stamp_snapshot(ExecutionSnapshot& snapshot) const;

 private:
  mutable std::mutex move_id_mutex_;
  std::string move_id_;

  mutable std::mutex program_run_id_mutex_;
  std::string program_run_id_;

  mutable std::mutex action_span_mutex_;
  std::optional<ActionSpanDTO> action_span_;
};

}  // namespace manyforge::core

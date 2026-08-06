#include <manyforge_core/execution_correlation_tracker.hpp>

namespace manyforge::core {

void ExecutionCorrelationTracker::set_from_request(
    const ExecuteRequestDTO& request) {
  {
    std::lock_guard<std::mutex> lock(move_id_mutex_);
    move_id_ = request.move_id;
  }
  {
    std::lock_guard<std::mutex> lock(program_run_id_mutex_);
    program_run_id_ = request.binding.program_run_id.value();
  }
  {
    std::lock_guard<std::mutex> lock(action_span_mutex_);
    action_span_ = request.action_span;
  }
}

void ExecutionCorrelationTracker::clear_all() {
  clear_move_id();
  clear_program_run_and_action_span();
}

void ExecutionCorrelationTracker::clear_move_id() {
  std::lock_guard<std::mutex> lock(move_id_mutex_);
  move_id_.clear();
}

void ExecutionCorrelationTracker::clear_program_run_and_action_span() {
  {
    std::lock_guard<std::mutex> lock(program_run_id_mutex_);
    program_run_id_.clear();
  }
  {
    std::lock_guard<std::mutex> lock(action_span_mutex_);
    action_span_.reset();
  }
}

std::string ExecutionCorrelationTracker::move_id() const {
  std::lock_guard<std::mutex> lock(move_id_mutex_);
  return move_id_;
}

std::optional<ActionSpanDTO> ExecutionCorrelationTracker::action_span() const {
  std::lock_guard<std::mutex> lock(action_span_mutex_);
  return action_span_;
}

void ExecutionCorrelationTracker::stamp_details(
    std::map<std::string, std::string>& details) const {
  {
    std::lock_guard<std::mutex> lock(program_run_id_mutex_);
    if (!program_run_id_.empty()) {
      details.emplace("program_run_id", program_run_id_);
    }
  }
  {
    std::lock_guard<std::mutex> lock(action_span_mutex_);
    if (action_span_) {
      const auto& span = *action_span_;
      details.emplace("action_source", to_string(span.action_source));
      details.emplace("action_span_id", span.action_span_id);
      if (!span.program_run_id.empty()) {
        details.emplace("action_span_program_run_id", span.program_run_id);
      }
      if (!span.node_instance_id.empty()) {
        details.emplace("node_instance_id", span.node_instance_id);
      }
      if (!span.node_attempt_id.empty()) {
        details.emplace("node_attempt_id", span.node_attempt_id);
      }
      if (!span.parent_node_attempt_id.empty()) {
        details.emplace("parent_node_attempt_id",
                        span.parent_node_attempt_id);
      }
    }
  }
}

void ExecutionCorrelationTracker::stamp_snapshot(
    ExecutionSnapshot& snapshot) const {
  if (!snapshot.action_span) {
    snapshot.action_span = action_span();
  }
}

}  // namespace manyforge::core

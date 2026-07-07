// Qualification micro-benchmark for the kernel event emit-path additions
// (observability plan OW1/OW3): the dispatch-order sequence assignment plus
// the flight-recorder ring copy must stay within budget on the emit path.
//
// Emits one MANYFORGE_QUALIFICATION_EVIDENCE JSON line. Budgets via env:
//   MANYFORGE_EVENT_EMIT_BUDGET_NS  (default 150; <=0 disables)
// Exit codes: 0 pass, 3 budget violation.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <sstream>

#include <manyforge_core/kernel_event_publisher.hpp>
#include <manyforge_core/kernel_flight_recorder.hpp>
#include <manyforge_core/types/kernel_event.hpp>

namespace {

using manyforge::core::KernelEvent;
using manyforge::core::KernelEventPublisher;
using manyforge::core::KernelEventType;
using manyforge::core::KernelFlightRecorder;

double measure_ns_per_op(std::size_t iterations,
                         KernelFlightRecorder& recorder,
                         KernelEventPublisher& publisher,
                         std::atomic<std::uint64_t>& sequence,
                         KernelEvent& event) {
  const auto start = std::chrono::steady_clock::now();
  for (std::size_t i = 0; i < iterations; ++i) {
    // The exact additions the plan budgets: sequence assignment + flight
    // ring copy + publisher dispatch (copy-on-write snapshot walk).
    event.sequence = sequence.fetch_add(1, std::memory_order_relaxed) + 1;
    recorder.record(event);
    (void)publisher.dispatch(event);
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
                           std::chrono::steady_clock::now() - start)
                           .count();
  return static_cast<double>(elapsed) / static_cast<double>(iterations);
}

}  // namespace

int main() {
  constexpr std::size_t kWarmup = 10'000;
  constexpr std::size_t kIterations = 1'000'000;

  KernelFlightRecorder recorder;
  KernelEventPublisher publisher;
  std::atomic<std::uint64_t> sequence{0};

  KernelEvent event;
  event.type = KernelEventType::kStateTransition;
  event.execution_id = 42;
  event.timestamp_ns = 1'000'000;

  // One inert subscriber so dispatch walks a realistic snapshot.
  std::atomic<std::uint64_t> delivered{0};
  (void)publisher.subscribe(
      [&delivered](const KernelEvent&) {
        delivered.fetch_add(1, std::memory_order_relaxed);
      });

  (void)measure_ns_per_op(kWarmup, recorder, publisher, sequence, event);
  const double ns_per_event =
      measure_ns_per_op(kIterations, recorder, publisher, sequence, event);

  double budget_ns = 150.0;
  if (const char* env = std::getenv("MANYFORGE_EVENT_EMIT_BUDGET_NS");
      env != nullptr && *env != '\0') {
    budget_ns = std::atof(env);
  }
  const bool budget_enabled = budget_ns > 0.0;
  const bool passed = !budget_enabled || ns_per_event <= budget_ns;

  std::ostringstream out;
  out << "MANYFORGE_QUALIFICATION_EVIDENCE {\"scenario\":"
      << "\"ow1_kernel_event_emit_path\",\"iterations\":" << kIterations
      << ",\"ns_per_event\":" << ns_per_event
      << ",\"subscribers\":1,\"delivered\":" << delivered.load()
      << ",\"flight_recorded_total\":" << recorder.recorded_total()
      << ",\"budgets\":{\"ns_per_event\":"
      << (budget_enabled ? std::to_string(budget_ns) : std::string("null"))
      << ",\"passed\":" << (passed ? "true" : "false") << "}}";
  std::cout << out.str() << "\n";
  return passed ? 0 : 3;
}

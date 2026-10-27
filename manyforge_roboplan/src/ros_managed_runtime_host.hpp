#pragma once

namespace manyforge::roboplan_backend::host_detail {

int RunExecutionHostMain(int argc, char **argv);

// The liveness lane owns `observed`. A drain first observed while the host is
// exiting does not turn its normal shutdown into a Manager-loss incident.
// A fault observed during service remains latched throughout teardown.
// A miss recorded just before shutdown but first observed afterward is treated
// as shutdown-only as well.
template <typename Renew, typename Refuse>
auto DispatchHeartbeatForDrain(const char *first_stuck, bool shutting_down,
                               const char *&observed, Renew &&renew,
                               Refuse &&refuse) {
  if (observed == nullptr && !shutting_down)
    observed = first_stuck;
  return observed != nullptr ? refuse(observed) : renew();
}

} // namespace manyforge::roboplan_backend::host_detail

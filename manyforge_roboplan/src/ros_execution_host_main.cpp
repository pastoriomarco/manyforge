#include "ros_managed_runtime_host.hpp"

#include <manyforge_core/process_diagnostics.hpp>

int main(int argc, char **argv) {
  manyforge::core::SetProcessRole("execution_host");
  manyforge::core::InstallCrashEvidence();
  return manyforge::roboplan_backend::host_detail::RunExecutionHostMain(argc,
                                                                    argv);
}

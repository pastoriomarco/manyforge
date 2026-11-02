#include "ros_managed_runtime_host.hpp"

int main(int argc, char **argv) {
  return manyforge::roboplan_backend::host_detail::RunExecutionHostMain(argc,
                                                                    argv);
}

cmake_minimum_required(VERSION 3.14)
project(manyforge_robot_lifecycle_msgs VERSION 0.1.0)

find_package(ament_cmake REQUIRED)
find_package(rosidl_default_generators REQUIRED)

rosidl_generate_interfaces(${PROJECT_NAME}
  "msg/ExecutionAdmissionSnapshot.msg"
  "msg/LifecycleCapability.msg"
  "msg/LifecycleTransaction.msg"
  "msg/RobotLifecycleSnapshot.msg"
  "srv/CloseRobotLifecycleSession.srv"
  "srv/OpenRobotLifecycleSession.srv"
  "srv/ReconcileRobotLifecycleCommand.srv"
  "srv/SubmitRobotLifecycleCommand.srv"
)

if(BUILD_TESTING)
  find_package(ament_cmake_copyright REQUIRED)
  find_package(ament_cmake_lint_cmake REQUIRED)
  ament_copyright()
  ament_lint_cmake()
endif()

ament_export_dependencies(rosidl_default_runtime)
ament_package()

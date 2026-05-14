"""ManyForge ROS adapter package.

ROS-specific helper scripts and launch-time configuration live here.  Runtime
arm execution and observation for ros2_control deployments is implemented in
the C++ ``manyforge_roboplan`` backend so timing-sensitive action, feedback,
and freshness handling stays outside Python.
"""

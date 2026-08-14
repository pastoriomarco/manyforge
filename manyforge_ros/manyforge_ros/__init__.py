"""Focused ROS translations and selected-owner adapters.

The I9 simulator control and read-only scene clients use standard typed
``simulation_interfaces`` services without owning simulator or command state.
The Program simulation port adds only a cancellable task/context translation
over those typed clients and an explicit launcher-owned entity mapping.
Private Isaac inspection/mesh transport and the complete Isaac product lane
remain unavailable until their separate I9 gates pass.
"""

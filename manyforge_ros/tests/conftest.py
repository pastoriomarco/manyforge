from __future__ import annotations

import pytest


@pytest.fixture(autouse=True)
def _clean_default_rclpy_context():
    """Keep ROS tests isolated when they use rclpy's default context.

    Several lightweight ROS adapter tests intentionally use the default
    rclpy context because they exercise small single-process fake services.
    A failed assertion or late action callback must not leave that context
    initialized for the next test; otherwise the following rclpy.init() fails
    with "Context.init() must only be called once" and hides the real result.
    """

    _shutdown_default_rclpy_context()
    yield
    _shutdown_default_rclpy_context()


def _shutdown_default_rclpy_context() -> None:
    try:
        import rclpy
    except Exception:
        return

    try:
        if rclpy.ok():
            rclpy.shutdown()
    except RuntimeError:
        # rclpy can raise while tearing down partially initialized contexts.
        # Tests create fresh processes in Docker lanes, so best effort is
        # enough here and avoids masking the next test's real failure.
        return

"""Disposition for the historical fake-driver observer component case.

Blob ``82e952d6ae64cf4f992246488fe6d7b9bec3a40d`` contains
``test_fake_driver_state_is_observed_without_opening_motion``.  It pins
command-free measured state, fault/warning projection, full servo/brake
initialization, finite complete joint mapping, source agreement/divergence,
and suppression of invalid pose.  Those facts return through the selected
I10/R measured-state adapter and I5/H0 status/alarm projection; the duplicate
observer node is not retained as a fake runtime owner.
"""

from __future__ import annotations

from manyforge_xarm_adapter import read_only_observer as subject


HISTORICAL_TEST_BLOB = "82e952d6ae64cf4f992246488fe6d7b9bec3a40d"
HISTORICAL_CASE = "test_fake_driver_state_is_observed_without_opening_motion"
FUTURE_ACCEPTANCE_GATES = ("I5/H0", "I10/R")


def test_component_behavior_is_assigned_without_a_fake_ros_owner() -> None:
    assert HISTORICAL_TEST_BLOB == (
        "82e952d6ae64cf4f992246488fe6d7b9bec3a40d"
    )
    assert HISTORICAL_CASE.endswith("without_opening_motion")
    assert FUTURE_ACCEPTANCE_GATES == ("I5/H0", "I10/R")
    assert not hasattr(subject, "ReadOnlyObserver")

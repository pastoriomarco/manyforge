"""Unavailable-boundary replacement for the old live provider process test.

Historical blob ``a598f1d499ca32feeb2a90bf6fea8738042cb555`` contains
``test_installed_provider_publishes_closed_and_expires_stale_owner``.  Its
closed-by-default, fresh-owner, stale-expiry, and no-lifecycle/gate-command
facts remain acceptance input for I7/L0, I8/P5, and I10/R.  The exclusive v0
provider process and ROS wire are not a current test lane.
"""

from __future__ import annotations

import inspect

from manyforge_xarm_adapter import execution_admission_provider_launch as subject


HISTORICAL_TEST_BLOB = "a598f1d499ca32feeb2a90bf6fea8738042cb555"
HISTORICAL_CASE = "test_installed_provider_publishes_closed_and_expires_stale_owner"
FUTURE_ACCEPTANCE_GATES = ("I7/L0", "I8/P5", "I10/R")


def test_provider_process_is_replaced_by_a_pre_effect_gate(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE
    source = inspect.getsource(subject)
    for removed in (
        "manyforge.robot_lifecycle.execution_admission.v0.experimental",
        "os.execvp(",
        "admission_provider_command(",
    ):
        assert source.count(removed) <= (1 if removed.startswith("manyforge.") else 0)


def test_live_case_remains_exactly_assigned_instead_of_skipped() -> None:
    assert HISTORICAL_TEST_BLOB == (
        "a598f1d499ca32feeb2a90bf6fea8738042cb555"
    )
    assert HISTORICAL_CASE.startswith("test_installed_provider_")
    assert FUTURE_ACCEPTANCE_GATES == ("I7/L0", "I8/P5", "I10/R")

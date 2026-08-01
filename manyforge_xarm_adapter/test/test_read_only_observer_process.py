"""Unavailable replacement for the disconnected observer process test.

Historical blob ``b86f316e8b5546e8073a2d1a6ff10a41bb142f03`` contains
``test_disconnected_launch_exits_cleanly_on_sigint``.  Clean lifecycle and
shutdown behavior remains I10/R acceptance input.  Until that owner exists,
the console returns 78 before inspecting arguments or initializing ROS.
"""

from __future__ import annotations

from manyforge_xarm_adapter import read_only_observer as subject


HISTORICAL_TEST_BLOB = "b86f316e8b5546e8073a2d1a6ff10a41bb142f03"
HISTORICAL_CASE = "test_disconnected_launch_exits_cleanly_on_sigint"


class _PoisonArgument:
    def __getattribute__(self, _name: str) -> object:
        raise AssertionError("unavailable observer inspected arguments")


def test_console_fails_before_argument_or_ros_access(capsys) -> None:
    assert subject.main(_PoisonArgument()) == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE


def test_process_case_remains_pinned_for_i10() -> None:
    assert HISTORICAL_TEST_BLOB == (
        "b86f316e8b5546e8073a2d1a6ff10a41bb142f03"
    )
    assert HISTORICAL_CASE.endswith("exits_cleanly_on_sigint")

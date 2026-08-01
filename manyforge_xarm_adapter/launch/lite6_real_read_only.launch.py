#!/usr/bin/env python3
"""Fail closed until the real-Lite6 observation lane qualifies at I10/R.

Historical launch body: manyforge@082221... blob
``1e984ec1eb0ac4e3e5d8759d644821d9e509d4ac``. It could construct the
xArm driver, robot-state publisher, and read-only observer directly. Those
adapter responsibilities remain I10/R acceptance input, but this installed
launch file must not provide an alternate path around the relocation gate.
"""

from __future__ import annotations

from typing import NoReturn


UNAVAILABLE_REASON_CODE = "composable_cell_runtime_unavailable"
UNAVAILABLE_MESSAGE = (
    f"{UNAVAILABLE_REASON_CODE}: live Lite6 observation requires the I10/R "
    "retained-product and vendor-observation gate"
)


def _unavailable(context, *args, **kwargs) -> NoReturn:
    """Reject before reading models or constructing ROS nodes."""
    del context, args, kwargs
    raise RuntimeError(UNAVAILABLE_MESSAGE)


def generate_launch_description() -> NoReturn:
    """Fail at I10/R before importing ROS or constructing a launch action."""
    _unavailable(None)

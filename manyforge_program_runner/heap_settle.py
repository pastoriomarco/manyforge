"""Full garbage collection only where a pause does not matter (OD-234).

CPython's automatic full (generation-2) collection scans every tracked object
of the process. In the runner that is mostly the imported code and the objects
built at start-up, and it stops every thread, a run's tick included, for about
150 ms. After start-up and at each Load and Reset, with no run ticking, the
runner collects in full once and then sets every surviving object aside
(``gc.freeze``): later automatic full collections scan only what was created
since. Automatic collection stays enabled with CPython's thresholds.

A point at which automatic full collections ran since the previous point
writes one WARNING line (the runner shows nothing below WARNING): their count
and the longest, the point, the objects set aside and the pause of the point's
own collection. A point without one writes nothing; the counters restart at
every point.
"""

from __future__ import annotations

import gc
import logging
import time
from typing import Any, Final

_LOGGER = logging.getLogger(__name__)

LOG_CODE: Final = "program_runner_heap_settled"

# Automatic full collections since the previous settle: [count, longest ns,
# start ns of the one in progress, 1 while settle_heap collects]. Plain ints in
# one preallocated list, so the callback allocates no tracked object.
_full_collections: Final[list[int]] = [0, 0, 0, 0]


def _observe_collection(phase: str, info: dict[str, Any]) -> None:
    """``gc.callbacks`` entry: time automatic generation-2 collections only."""

    if info["generation"] != 2 or _full_collections[3]:
        return
    if phase == "start":
        _full_collections[2] = time.perf_counter_ns()
        return
    duration_ns = time.perf_counter_ns() - _full_collections[2]
    _full_collections[0] += 1
    if duration_ns > _full_collections[1]:
        _full_collections[1] = duration_ns


def settle_heap(point: str) -> None:
    """Collect in full and set the survivors aside; call only with no run ticking.

    ``point`` names the caller (``start``, ``load`` or ``reset``) in the line.
    """

    if _observe_collection not in gc.callbacks:
        gc.callbacks.append(_observe_collection)
    automatic, longest_ns = _full_collections[0], _full_collections[1]
    _full_collections[0] = _full_collections[1] = 0
    _full_collections[3] = 1
    started_ns = time.perf_counter_ns()
    # Objects set aside at the previous point that died since are freed here.
    gc.unfreeze()
    gc.collect()
    gc.freeze()
    pause_ns = time.perf_counter_ns() - started_ns
    _full_collections[3] = 0
    if automatic:
        _LOGGER.warning(
            "%s point=%s frozen_objects=%d collection_ms=%.3f "
            "automatic_full_collections=%d longest_automatic_full_collection_ms=%.3f",
            LOG_CODE,
            point,
            gc.get_freeze_count(),
            pause_ns / 1e6,
            automatic,
            longest_ns / 1e6,
        )


def freeze_heap(point: str) -> None:
    """Set everything alive aside without collecting: no pause.

    CPython 3.12's ``gc.freeze()`` only splices the three generation lists onto
    the permanent one (O(1)); ``gc.get_freeze_count()`` walks that list, so this
    line names no object count.
    """

    automatic, longest_ns = _full_collections[0], _full_collections[1]
    _full_collections[0] = _full_collections[1] = 0
    gc.freeze()
    if automatic:
        _LOGGER.warning(
            "%s point=%s collection_ms=0.000 "
            "automatic_full_collections=%d longest_automatic_full_collection_ms=%.3f",
            LOG_CODE,
            point,
            automatic,
            longest_ns / 1e6,
        )


__all__ = ["LOG_CODE", "freeze_heap", "settle_heap"]

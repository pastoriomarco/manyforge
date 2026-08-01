#!/usr/bin/env python3
"""I5/H0 qualification entrypoint for the relocated headless alarm owner.

The historical implementation benchmarked the Composer-local manager and the
raw-byte ``operator_alarm_catalog.v1`` package.  That body is available at the
pinned whole-Composer reference; running it now would qualify an owner and an
identity contract that are intentionally gone.  This shell preserves the
same-language qualification entrypoint and its acceptance target until the
new headless owner supplies a JCS catalog identity and client API.
"""

from __future__ import annotations

import json
import sys
from typing import Final, NoReturn, Sequence


RESTORATION_GATES: Final = ("I5", "H0")
UNAVAILABLE_REASON_CODE: Final = (
    "composable_cell_operator_alarm_performance_qualification_unavailable"
)
RETAINED_QUALIFICATION_REQUIREMENTS: Final = (
    "repeat ingestion preserves one live condition and complete occurrence counts",
    "exact duplicate ingestion preserves cardinality and does not invent occurrences",
    "ingest latency and throughput remain bounded under the approved workload",
    "metric samples, live records, terminal history, and memory remain bounded",
    "capacity exhaustion fails closed without evicting live attention",
    "history persistence failure or slowness cannot block alarm or Stop ownership",
    "evidence binds the JCS catalog descriptor digest and exact headless owner build",
)


class OperatorAlarmPerformanceQualificationUnavailableError(RuntimeError):
    """Raised before loading a catalog, owner, baseline, or output path."""


def run_qualification(*_args: object, **_kwargs: object) -> NoReturn:
    """Fail closed until an H0-qualified headless alarm client is available."""
    raise OperatorAlarmPerformanceQualificationUnavailableError(
        f"{UNAVAILABLE_REASON_CODE}: replacement qualification returns at I5/H0"
    )


def main(_argv: Sequence[str] | None = None) -> int:
    payload = {
        "status": "unavailable",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "restoration_gates": list(RESTORATION_GATES),
        "retained_requirements": list(RETAINED_QUALIFICATION_REQUIREMENTS),
    }
    print(json.dumps(payload, sort_keys=True), file=sys.stderr)
    return 3


if __name__ == "__main__":
    raise SystemExit(main())

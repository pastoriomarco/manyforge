#!/usr/bin/env python3
"""Authoring-smoke cutover gate with exact historical restoration mapping.

The complete smoke is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``6325eba4db3e3bdbc5657d160073108ea3a120bd``).  Historical symbols map as
follows:

* ``_run_direct_smoke`` and ``_run_api_smoke`` used the removed Composer and
  assistant/Isaac clients;
* ``_run_proposal_lifecycle`` owned the old proposal/apply route sequence;
* ``_inspection_request``, ``_checked_response``, ``_load_fixture`` and
  ``_write_minimal_program`` encoded its v0 inputs and temporary artifacts;
* ``main``, ``_parse_args`` and ``_run_smoke`` drove the live workflow; and
* ``_smoke_result``, ``_filter_report_for_request`` and ``_path_is_under``
  projected the old report/proposal wire and could not prove success on their
  own.

No historical helper remains callable: the old projection hard-coded success
without validating the report, proposal, or lifecycle.  I5/H0 and I9/I must
restore a bounded qualification client over owner-issued facts.
"""

from __future__ import annotations

import json
import sys
from typing import Final, NamedTuple


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
HISTORICAL_SOURCE: Final = (
    "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
)


class RestorationAssignment(NamedTuple):
    historical_symbols: str
    required_behavior: str


RESTORATION_ASSIGNMENTS: Final = (
    RestorationAssignment(
        "main, _parse_args, _run_smoke",
        "bounded operator workflow after authoring and assistant clients return",
    ),
    RestorationAssignment(
        "_run_direct_smoke, _run_api_smoke",
        "qualified simulator-inspection and headless-authoring clients",
    ),
    RestorationAssignment(
        "_run_proposal_lifecycle",
        "proposal/apply client over owner-issued scene revisions",
    ),
    RestorationAssignment(
        "_inspection_request, _checked_response, _load_fixture, "
        "_write_minimal_program",
        "future contract configuration and bounded artifact fixtures",
    ),
    RestorationAssignment(
        "_smoke_result, _filter_report_for_request, _path_is_under",
        "I5/H0 + I9/I bounded evidence projection whose success is derived "
        "from validated owner-issued report, proposal, and lifecycle facts",
    ),
)


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "smoke-assistant-scene-authoring.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before fixture reads, imports, network, ROS, or scene mutation."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())

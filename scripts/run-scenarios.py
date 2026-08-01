#!/usr/bin/env python3
"""Typed boundary for the historical Composer scenario runner.

The full runner is pinned at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` (blob
``90beab593de751f5af7976fe5d251195f12ce5b0``):

* ``ManagedComposerProcess`` launched the old Python Composer with mock Kernel
  flags, polled its historical health route, and owned process cleanup;
* ``_parse_args`` exposed that product selector and its v0 catalog options;
* ``_load_scenarios`` loaded the historical scenario/matrix artifacts; and
* ``main`` drove ``manyforge_composer.verification.ScenarioRunner`` against
  the removed runtime routes.

The three process-safety cases in historical test blob
``30410567b74434c990c072c7a40a9a1be907befa`` remain required acceptance
intent: cleanup after readiness timeout, refusal to adopt a foreign server,
and rejection of a foreign-readiness/bind-failure race.  They return with the
future headless scenario client; the old mock-Kernel launcher is absent.
"""

from __future__ import annotations

import json
import sys
from typing import Final, NamedTuple, NoReturn


UNAVAILABLE_EXIT_CODE: Final = 78
UNAVAILABLE_REASON_CODE: Final = "composable_cell_runtime_unavailable"
HISTORICAL_SOURCE: Final = (
    "manyforge@08222188293df8e12f90dc72e204c21f22f92ea1"
)


class ScenarioExecutionUnavailableError(RuntimeError):
    """Raised before a scenario file, process, HTTP client, or runtime is used."""


class RestorationAssignment(NamedTuple):
    historical_symbols_or_cases: str
    required_behavior: str


RESTORATION_ASSIGNMENTS: Final = (
    RestorationAssignment(
        "_load_scenarios",
        "bounded scenario/matrix artifact loading after the v2 schema is owned",
    ),
    RestorationAssignment(
        "ManagedComposerProcess, _parse_args, main",
        "thin headless scenario client with no mock or duplicate runtime owner",
    ),
    RestorationAssignment(
        "test_local_process_launcher_cleans_up_on_forced_health_timeout",
        "bounded child cleanup after readiness failure",
    ),
    RestorationAssignment(
        "test_local_process_launcher_refuses_port_already_serving_a_composer",
        "fail closed instead of adopting an unowned endpoint",
    ),
    RestorationAssignment(
        "test_local_process_launcher_rejects_foreign_readiness_after_child_death",
        "reject bind-failure and foreign-readiness races",
    ),
)


def _raise_scenario_execution_unavailable() -> NoReturn:
    raise ScenarioExecutionUnavailableError(
        f"{UNAVAILABLE_REASON_CODE}: scenario execution returns with the "
        "qualified headless Program/Kernel client"
    )


class ManagedComposerProcess:
    """Historical name retained solely as a fail-before-effects boundary."""

    def __init__(self, *args: object, **kwargs: object) -> None:
        del args, kwargs
        _raise_scenario_execution_unavailable()


def unavailable_payload() -> dict[str, object]:
    return {
        "schema_version": "manyforge.qualification_unavailable.v1",
        "reason_code": UNAVAILABLE_REASON_CODE,
        "script": "run-scenarios.py",
        "historical_source": HISTORICAL_SOURCE,
        "exit_code": UNAVAILABLE_EXIT_CODE,
        "assignments": [
            assignment._asdict() for assignment in RESTORATION_ASSIGNMENTS
        ],
    }


def main(argv: list[str] | None = None) -> int:
    """Fail before argument-derived paths, imports, files, process, or network."""

    del argv
    print(json.dumps(unavailable_payload(), sort_keys=True), file=sys.stderr)
    return UNAVAILABLE_EXIT_CODE


if __name__ == "__main__":
    raise SystemExit(main())

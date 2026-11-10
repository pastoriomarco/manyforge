"""Backend snapshot contract fixtures for the strict Composer frontend parsers.

The Composer frontend parses owner snapshots with exact-key parsers, so a new
backend field that the frontend does not know closes the control socket
(4002). This module builds representative snapshots with the real backend
producers and writes them as canonical JSON under
``manyforge_composer/frontend/src/testFixtures/backendSnapshots``. The vitest
suite ``backendSnapshots.contract.test.ts`` parses every file there with the
real strict parsers; ``test_backend_snapshot_fixtures.py`` fails when a
committed file no longer matches what the producers build now.

Regenerate after an intended backend wire change, then run vitest:

    python -m manyforge_composer.tests.backend_snapshot_fixtures --write

The run itself is driven by the existing owner test doubles (the same ones
the owner tests use); every recorded field is produced by the real code.
Values that are inherently per-run (monotonic times, per-process epochs,
digests over them) are replaced by stable placeholders of the same type and
format, so the files stay byte-stable and reviewable.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from copy import deepcopy
import importlib.util
import json
import re
from pathlib import Path
import sys
import tempfile
from types import ModuleType
import uuid
from typing import Any, Callable, Iterator

import pytest

from manyforge_headless_api.component_overview import build_component_overview

_ROOT = Path(__file__).resolve().parents[2]
FIXTURE_DIR = (
    _ROOT / "manyforge_composer/frontend/src/testFixtures/backendSnapshots"
)


def _test_module(relative: str, name: str) -> ModuleType:
    """Load an owner test module by path (its ``tests`` package name is
    ambiguous across the repository's per-package import roots)."""

    loaded = sys.modules.get(name)
    if loaded is not None:
        return loaded
    spec = importlib.util.spec_from_file_location(name, _ROOT / relative)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


# --- Stable placeholders ---------------------------------------------------

_MONOTONIC_KEYS = frozenset({"generated_at_ns"})
_DIGEST = re.compile(r"sha256:[0-9a-f]{64}")


def _stabilize(value: Any) -> Any:
    """Replace per-run values by placeholders of the same type and format.

    Monotonic times become one fixed time. Every distinct SHA-256 digest is
    renumbered in first-appearance order, so equal digests stay equal while
    unrelated content (a node catalog or example edit) does not make the
    fixtures stale.
    """

    digests: dict[str, str] = {}

    def visit(item: Any, key: str | None) -> Any:
        if isinstance(item, dict):
            return {name: visit(child, name) for name, child in item.items()}
        if isinstance(item, list):
            return [visit(child, key) for child in item]
        if key in _MONOTONIC_KEYS:
            # Preserve malformed values so normalization cannot hide a wire
            # regression from the strict frontend parser (parseNanoseconds:
            # a safe integer, or canonical text up to the int64 maximum).
            if type(item) is int and 0 <= item <= 9_007_199_254_740_991:
                return 1_000_000_000
            if (isinstance(item, str)
                    and re.fullmatch(r"0|[1-9][0-9]*", item)
                    and int(item) <= 9_223_372_036_854_775_807):
                return "1000000000"
        if isinstance(item, str) and _DIGEST.fullmatch(item):
            return digests.setdefault(item, f"sha256:{len(digests) + 1:064x}")
        return item

    return visit(value, None)


# --- Program cycle with held failures ---------------------------------------

_RUNNER_EPOCH = "program-runner.epoch.1"
_RUN_ID = "program-cycle.1"


def _held_owner(behavior: ModuleType):
    """The behavior owner test's two-group failure hold, with the Program
    runner's run identity: a real ``ProgramRunOwner`` held on two failed
    nodes, each with its captured recovery policy."""

    interventions = behavior.ProgramInterventionRegistry(maximum_retained_runs=8)
    owner, admission, _task, _scope, _events = behavior._fixture(
        initial_task_state=behavior.OwnerTaskState.FAILED,
        task_factory=behavior._HeldFailureTaskPort,
        interventions=interventions,
        run_id_factory=lambda: _RUN_ID,
    )
    document = admission.program.to_dict()
    move = document["tree"]["children"][0]
    policy = {
        "allow_intervention": True,
        "max_retries": 2,
        "intervention_stop_scope": "own_groups",
    }
    children = []
    for instance_id, groups in (("arm_move", ["arm"]), ("tool_move", ["tools"])):
        child = deepcopy(move)
        child["instance_id"] = instance_id
        child["execution_groups"] = groups
        child["recovery_policy"] = dict(policy)
        children.append(child)
    document["tree"] = {
        "node_id": "parallel",
        "instance_id": "parallel",
        "parameters": {"success_policy": "all"},
        "children": children,
    }
    document["execution_groups"] = {
        group: {"execution_policy": "independent"} for group in ("arm", "tools")
    }
    document["program_revision"] = behavior.compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v3",
    )
    program = behavior.Program.from_dict(document, admission.environment.catalog)
    rows = [row.to_dict() for row in behavior.program_node_policies(program)]
    admission = behavior._admission_for_program(admission, program, rows)
    run_id = owner.start(admission).capture.program_run_id
    paused = owner.advance_once(run_id)
    assert paused.state is behavior.ProgramRunState.INTERVENTION_REQUIRED
    return owner


@contextmanager
def _started_runner() -> Iterator[tuple[Any, ModuleType]]:
    """A real ``ProgramRuntime`` on a real cycle journal, with the runner
    tests' admission, Headless and host doubles (no Scene, no native owner)."""

    runner = _test_module(
        "manyforge_program_runner/tests/test_runtime.py",
        "_contract_fixture_program_runner_runtime_tests",
    )
    with tempfile.TemporaryDirectory() as raw, pytest.MonkeyPatch.context() as patch:
        patch.setattr(
            "manyforge_program_runner.runtime.build_applied_program_run_admission",
            lambda *_args, **_kwargs: runner._PreparedRunAdmission(),
        )
        runner._bypass_scene_preparation(patch)
        # Node attempt ids are random per attempt; number them instead.
        attempts = iter(range(1, 1_000))
        patch.setattr(
            "manyforge_behavior.program_runtime.uuid4",
            lambda: uuid.UUID(int=next(attempts)),
        )
        runtime, _journal, _admission = runner._reconcile_runtime(
            Path(raw),
            journal_owner_epoch=_RUNNER_EPOCH,
            runtime_owner_epoch=_RUNNER_EPOCH,
            inventory={},
        )
        fake_owner = runner._Owner()
        runtime._owner = fake_owner
        runtime._host = runner._Host(
            fake_owner, owner_id=runtime.owner_id, owner_epoch=runtime.owner_epoch
        )
        runtime.start()
        try:
            yield runtime, runner
        finally:
            runtime.shutdown()


def program_cycle_idle() -> dict[str, Any]:
    """``program_cycle`` of the control shell before any Program run."""

    with _started_runner() as (runtime, _runner):
        status = runtime.cycle_status()
    assert status["state"] == "idle_after_reset" and status["run"] is None
    return _stabilize(status)


def program_cycle_held_failures() -> dict[str, Any]:
    """``program_cycle`` of the control shell while a running cycle holds two
    failed nodes with ``intervention_stop_scope: own_groups``: the run,
    runtime controls, the focused recovery offer and every hold's offer."""

    behavior = _test_module(
        "manyforge_behavior/tests/test_program_run_owner.py",
        "_contract_fixture_program_run_owner_tests",
    )
    with _started_runner() as (runtime, runner):
        # The cycle is claimed through the runner's own start path; the real
        # held owner then serves the run, its holds and its policies.
        started = runtime.start_run(runner._start_request())
        assert started["capture"]["program_run_id"] == _RUN_ID
        runtime._owner = _held_owner(behavior)
        status = runtime.cycle_status()
    assert status["recovery_offer"]["policy"]["intervention_stop_scope"] == "own_groups"
    holds = status["runtime_controls"]["recovery_offers"]
    assert [hold["intervention_stop_scope"] for hold in holds] == ["own_groups"] * 2
    return _stabilize(status)


def component_overview_ready() -> dict[str, Any]:
    """``component_overview`` of the control shell: Headless's real reducer
    over the Component Overview tests' ready owner facts."""

    overview = _test_module(
        "manyforge_headless_api/tests/test_component_overview.py",
        "_contract_fixture_component_overview_tests",
    )
    result = build_component_overview(**overview._facts())
    assert result["readiness"] == "ready"
    return _stabilize(result)


FIXTURES: dict[str, Callable[[], Any]] = {
    "component-overview.ready.json": component_overview_ready,
    "program-cycle.idle.json": program_cycle_idle,
    "program-cycle.held-failures.json": program_cycle_held_failures,
}


def render(builder: Callable[[], Any]) -> str:
    return json.dumps(builder(), indent=2, sort_keys=True, ensure_ascii=False) + "\n"


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--write", action="store_true", help="rewrite the fixtures")
    arguments = parser.parse_args(argv)
    stale = []
    for name, builder in FIXTURES.items():
        path = FIXTURE_DIR / name
        text = render(builder)
        if arguments.write:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(text, encoding="utf-8")
        elif not path.exists() or path.read_text(encoding="utf-8") != text:
            stale.append(name)
    for name in stale:
        print(f"stale: {name}", file=sys.stderr)
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())

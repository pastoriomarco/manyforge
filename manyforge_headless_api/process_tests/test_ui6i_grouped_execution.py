"""UI6-I T3/T4 process qualification for one native two-robot cell."""

from __future__ import annotations

from contextlib import contextmanager
import shutil
import sys
from tempfile import TemporaryDirectory
import time
from pathlib import Path
from typing import Any, Iterator

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.process_tests.test_process_stack import (
    ProcessStack,
    _composer_fence,
    _composer_snapshot_stream,
    _matching_composer_snapshot,
    _read_only_tree,
    _required_path,
)


CELL_ID = "ui6i_ur5e_lite6_wall_cell"
ADMISSION_FILENAME = f"canonical-empty-admission.{CELL_ID}.json"
GROUPED_SCENARIO = "ur5e_lite6_wall_grouped"
ISOLATION_SCENARIO = "ur5e_lite6_wall_isolation"


@contextmanager
def _dual_robot_stack() -> Iterator[ProcessStack]:
    # POSIX socket paths are bounded. Keep this process fixture's private root
    # short regardless of pytest's descriptive test-directory names.
    with TemporaryDirectory(prefix="mf-ui6i-") as temporary_root:
        root = Path(temporary_root)
        source = _required_path("MANYFORGE_UI6I_CELL_BOOTSTRAP_ROOT")
        examples = _required_path("MANYFORGE_EXAMPLE_SCENARIO_ROOT")
        bootstrap = root / "cell-bootstrap"
        shutil.copytree(source, bootstrap)
        stack_root = root / "stack"
        stack_root.mkdir()
        with _read_only_tree(bootstrap):
            stack = ProcessStack(
                stack_root,
                package_root=bootstrap,
                cell_id=CELL_ID,
                composer_fs_roots=(examples, stack_root),
                freshness_milliseconds=120_000,
                enable_program_runner=True,
            )
            try:
                stack.prepare_native_package(ADMISSION_FILENAME)
                stack.start_manager()
                stack.start_program_runner()
                stack.start_api()
                stack.wait_manager_source()
                stack.wait_program_runner()
                stack.wait_authoring_admission(expected_allowed=True)
                stack.start_composer()
                stack.wait_composer()
                yield stack
            except BaseException:
                stack.log.flush()
                log_path = stack_root / "process-stack.log"
                if log_path.is_file():
                    print(
                        "\n--- UI6-I dual-robot process log ---\n"
                        + log_path.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                raise
            finally:
                stack.close()


def _load_scenario(stack: ProcessStack, scenario_id: str) -> tuple[dict[str, Any], str]:
    with (
        _composer_snapshot_stream(stack, client_id=f"ui6i-{scenario_id}") as snapshots,
        HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
    ):
        scenarios = composer.request("GET", "/api/scenario-documents")
        rows = {row["scenario_id"]: row for row in scenarios["entries"]}
        ids = set(rows)
        assert {GROUPED_SCENARIO, ISOLATION_SCENARIO} <= ids
        draft = composer.request("GET", "/api/program/draft")
        for attempt in range(8):
            snapshot = _matching_composer_snapshot(snapshots, draft)
            deployment = composer.request("GET", "/api/deployment/workspace")
            try:
                loaded = composer.request(
                    "POST",
                    "/api/scenario-documents/load",
                    body={
                        **_composer_fence(
                            snapshot,
                            request_id=f"ui6i.{scenario_id}.load.{attempt}",
                        ),
                        "deploymentDraftId": deployment["draft"]["id"],
                        "expectedDeploymentDraftRevision": deployment["draft"][
                            "revision"
                        ],
                        "path": rows[scenario_id]["path"],
                        "confirmDiscard": False,
                    },
                )
                break
            except HeadlessClientError as error:
                if (
                    error.code
                    not in {"authoring_not_ready", "stale_generation"}
                    or attempt == 7
                ):
                    raise
                draft = composer.request("GET", "/api/program/draft")
        assert loaded["status"] == "accepted"
        assert loaded["path"] == rows[scenario_id]["path"]
        activation = loaded["activation"]
        deadline = time.monotonic() + 30.0
        with HeadlessHttpClient(
            stack.base_url, timeout_seconds=10.0
        ) as headless:
            while True:
                status = headless.manager_apply_status(
                    activation["apply_id"], activation["apply_generation"]
                )
                if status["state"] != "pending":
                    assert status["state"] == "succeeded", (scenario_id, status)
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError(
                        f"Scenario activation did not settle: {status!r}"
                    )
                time.sleep(0.02)
        stack.wait_authoring_admission(expected_allowed=True)
        deployment = composer.request("GET", "/api/deployment/workspace")
        assert deployment["draft"]["document"]["deployment_artifact_id"] == (
            "ur5e_lite6_wall"
        )
        applied = composer.program_applied()
        assert applied["scenario_id"] == scenario_id
        return (
            applied,
            applied["source_document_digest"],
        )


def _save_and_reopen_current_scenario(
    stack: ProcessStack, scenario_id: str
) -> None:
    directory = stack.root / "saved-scenarios"
    directory.mkdir()
    target = directory / f"{scenario_id}.scenario"
    with (
        _composer_snapshot_stream(stack, client_id=f"ui6i-save-{scenario_id}") as snapshots,
        HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
    ):
        draft = composer.request("GET", "/api/program/draft")
        snapshot = _matching_composer_snapshot(snapshots, draft)
        deployment = composer.request("GET", "/api/deployment/workspace")
        fence = {
            **_composer_fence(
                snapshot,
                request_id=f"ui6i.{scenario_id}.save",
            ),
            "deploymentDraftId": deployment["draft"]["id"],
            "expectedDeploymentDraftRevision": deployment["draft"]["revision"],
        }
        manager_before = composer.manager_status()["status"]["active"]
        saved = composer.request(
            "POST",
            "/api/scenario-documents/save",
            body={
                **fence,
                "path": str(target),
                "scenarioId": f"{scenario_id}_saved",
                "name": "UI6-I grouped Scenario copy",
                "description": "Native Save/Load qualification copy",
                "confirmOverwrite": False,
            },
        )
        assert saved["status"] == "saved"
        assert saved["last_known_status"] == "ready"
        assert saved["path"] == str(target.resolve())
        index = composer.request(
            "GET",
            "/api/scenario-documents",
            params=(("directory", str(directory)),),
        )
        saved_rows = [
            row for row in index["entries"] if row["source_kind"] == "saved"
        ]
        assert len(saved_rows) == 1
        assert saved_rows[0]["path"] == str(target.resolve())

        draft = composer.request("GET", "/api/program/draft")
        for attempt in range(8):
            snapshot = _matching_composer_snapshot(snapshots, draft)
            deployment = composer.request("GET", "/api/deployment/workspace")
            try:
                reopened = composer.request(
                    "POST",
                    "/api/scenario-documents/load",
                    body={
                        **_composer_fence(
                            snapshot,
                            request_id=f"ui6i.{scenario_id}.reopen.{attempt}",
                        ),
                        "deploymentDraftId": deployment["draft"]["id"],
                        "expectedDeploymentDraftRevision": deployment["draft"][
                            "revision"
                        ],
                        "path": str(target),
                        "confirmDiscard": False,
                    },
                )
                break
            except HeadlessClientError as error:
                if (
                    error.code not in {"authoring_not_ready", "stale_generation"}
                    or attempt == 7
                ):
                    raise
                draft = composer.request("GET", "/api/program/draft")
        assert reopened["status"] == "loaded"
        assert reopened["path"] == str(target.resolve())
        assert composer.manager_status()["status"]["active"] == manager_before


def _start_run(
    stack: ProcessStack,
    *,
    scenario_id: str,
    source_document_digest: str,
    applied: dict[str, Any],
) -> dict[str, Any]:
    with HeadlessHttpClient(stack.composer_url, timeout_seconds=20.0) as composer:
        draft = composer.request("GET", "/api/program/draft")["program"]
        for attempt in range(8):
            try:
                return composer.program_run_start(
                    {
                        "request_id": f"ui6i.{scenario_id}.run.{attempt}",
                        "request_owner_epoch": "ui6i.grouped.process-test",
                        "source_label": "ui6i_grouped_process_test",
                        "correlation_id": f"ui6i.{scenario_id}.run",
                        "scenario_id": scenario_id,
                        "source_document_digest": source_document_digest,
                        "expected_applied_record_digest": applied["source"][
                            "record_digest"
                        ],
                        "draft_program_revision": draft["program_revision"],
                        "expected_admission_generation": stack.wait_authoring_admission(
                            expected_allowed=True
                        )["admission_generation"],
                    }
                )
            except HeadlessClientError as error:
                if (
                    error.code
                    not in {
                        "authoring_not_ready",
                        "program_node_availability_stale",
                        "stale_generation",
                    }
                    or attempt == 7
                ):
                    raise
        raise AssertionError("unreachable")


def _native_events(stack: ProcessStack) -> list[dict[str, Any]]:
    native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=2.0)
    owner_epoch = native.identity()["identity"]["owner_epoch"]
    result: list[dict[str, Any]] = []
    after = 0
    while True:
        page = native.events(owner_epoch, after, limit=16)
        result.extend(page["events"])
        if not page["has_more"]:
            return result
        after = page["next_after_sequence"]


def _start_motion(
    client: HeadlessHttpClient,
    started: dict[str, Any],
    *,
    request_id: str,
) -> dict[str, Any]:
    program_run_id = started["capture"]["program_run_id"]
    deadline = time.monotonic() + 5.0
    while True:
        cycle = client.program_cycle_status()
        controls = cycle.get("runtime_controls")
        selections = None if controls is None else controls.get("execution_selections")
        if (
            cycle.get("program_run_id") == program_run_id
            and isinstance(selections, dict)
        ):
            break
        if time.monotonic() >= deadline:
            raise RuntimeError(
                f"current motion controls were not published for {program_run_id}: {cycle!r}"
            )
        time.sleep(0.01)
    return client.program_run_motion_start(
        program_run_id,
        {
            "request_id": request_id,
            "request_owner_epoch": "ui6i.grouped.process-test",
            "source_label": "ui6i_grouped_process_test",
            "correlation_id": request_id,
            "program_run_id": program_run_id,
            "expected_owner_epoch": cycle["program_runner_owner_epoch"],
            "expected_run_revision": cycle["run"]["run_revision"],
            "expected_selection_revision": selections["selection_revision"],
            "reason": "initial operator motion Run",
            "group_ids": ["all"],
        },
    )


def _operation_acceptances(
    stack: ProcessStack, program_run_id: str, *, minimum: int = 2
) -> list[dict[str, Any]]:
    deadline = time.monotonic() + 20.0
    accepted: list[dict[str, Any]] = []
    while time.monotonic() < deadline:
        accepted = [
            event
            for event in _native_events(stack)
            if event["record_kind"] == "operation_accepted"
            and event["payload"]["intent"]["program_run_id"] == program_run_id
        ]
        if len(accepted) >= minimum:
            return accepted
        time.sleep(0.01)
    with HeadlessHttpClient(stack.composer_url, timeout_seconds=5.0) as client:
        run = client.program_run(program_run_id)
        cycle = client.program_cycle_status()
    raise RuntimeError(
        "two robot operations were not accepted: "
        f"events={accepted!r}, run={run!r}, cycle={cycle!r}"
    )


def _wait_run_state(
    client: HeadlessHttpClient,
    program_run_id: str,
    expected: set[str],
    *,
    timeout_seconds: float = 40.0,
) -> dict[str, Any]:
    deadline = time.monotonic() + timeout_seconds
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.program_run(program_run_id)
        if last["state"] in expected:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"Program run did not reach {sorted(expected)}: {last!r}")


def _wait_group_held(
    client: HeadlessHttpClient, program_run_id: str, group_id: str
) -> tuple[dict[str, Any], dict[str, Any]]:
    deadline = time.monotonic() + 20.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.program_cycle_status()
        controls = last.get("runtime_controls")
        selections = None if controls is None else controls.get("execution_selections")
        if isinstance(selections, dict):
            group = next(
                (row for row in selections["groups"] if row["group_id"] == group_id),
                None,
            )
            if group is not None and group["state"] == "held" and group["resume_allowed"]:
                return last, selections
        time.sleep(0.01)
    raise RuntimeError(f"Program selection did not settle held: {last!r}")


def _group_selector(program_binding_id: str) -> dict[str, Any]:
    return {
        "program_binding_id": program_binding_id,
        "group_targets": [
            {
                "component_role_id": "lite6_robot",
                "target_role_id": "lite6_arm",
            },
            {
                "component_role_id": "ur5e_robot",
                "target_role_id": "ur5e_arm",
            },
        ],
    }


def _set_scale(
    client: HeadlessHttpClient, selector: dict[str, Any], scale: float, request_id: str
) -> dict[str, Any]:
    current = client.runtime_scale_control(selector)["runtime_scale_control"]
    owner_epoch = client.execution_diagnostics()["status"]["owner_epoch"]
    accepted = client.set_runtime_scale_control(
        {
            "request_context": {
                "request_id": request_id,
                "request_owner_epoch": owner_epoch,
                "source_label": "ui6i_grouped_process_test",
                "correlation_id": request_id,
            },
            "selector": selector,
            "expected_control_generation": current["control_generation"],
            "scale": scale,
        }
    )
    assert accepted["accepted"]["control_generation"] == (
        current["control_generation"] + 1
    )
    return accepted


def _wait_scale_applied(
    client: HeadlessHttpClient,
    selector: dict[str, Any],
    scale: float,
) -> dict[str, Any]:
    deadline = time.monotonic() + 10.0
    last: dict[str, Any] = {}
    while time.monotonic() < deadline:
        last = client.runtime_scale_control(selector)["runtime_scale_control"]
        if last.get("application_state") in {"failed", "rejected"}:
            raise RuntimeError(f"runtime scale was not applied: {last!r}")
        if last.get("effective_scale") == scale:
            return last
        time.sleep(0.01)
    raise RuntimeError(f"runtime scale did not become effective: {last!r}")


def _reset_cycle(client: HeadlessHttpClient, program_run_id: str) -> None:
    cycle = client.program_cycle_status()
    reference = cycle["terminal_or_settlement_reference"]
    client.program_cycle_reset(
        {
            "request_id": f"ui6i.{program_run_id}.reset",
            "request_owner_epoch": "ui6i.grouped.process-test",
            "source_label": "ui6i_grouped_process_test",
            "correlation_id": f"ui6i.{program_run_id}",
            "program_run_id": program_run_id,
            "expected_owner_epoch": cycle["program_runner_owner_epoch"],
            "expected_cycle_generation": cycle["cycle_generation"],
            "expected_claim_generation": cycle["cycle_claim_generation"],
            "terminal_reference": reference,
            "settlement_evidence_digest": reference,
        }
    )
    assert client.program_cycle_status()["state"] == "idle_after_reset"


def test_repeated_project_activation_replaces_one_coherent_publication() -> None:
    with _dual_robot_stack() as stack:
        observed: list[tuple[str, str, str]] = []
        for scenario_id in (
            GROUPED_SCENARIO,
            ISOLATION_SCENARIO,
            GROUPED_SCENARIO,
            ISOLATION_SCENARIO,
        ):
            applied, _source_document_digest = _load_scenario(stack, scenario_id)
            assert applied["scenario_id"] == scenario_id
            assert applied["deployment_artifact_id"] == "ur5e_lite6_wall"
            assert applied["program_binding_id"]
            observed.append(
                (
                    applied["scenario_id"],
                    applied["program_revision"],
                    applied["program_binding_id"],
                )
            )
            with HeadlessHttpClient(
                stack.composer_url, timeout_seconds=10.0
            ) as composer:
                runtime = composer.runtime()
                manager = composer.manager_status()["status"]
            assert runtime["complete"] is True
            assert manager["active"]["retained_bundle_id"] == applied[
                "retained_bundle_id"
            ]
            assert manager["active"]["graph_revision"] == applied[
                "graph_revision"
            ]

        assert observed[0] == observed[2]
        assert observed[1] == observed[3]
        assert observed[0] != observed[1]


def test_grouped_dual_robot_run_scales_stops_and_resumes() -> None:
    with _dual_robot_stack() as stack:
        applied, catalog_revision = _load_scenario(stack, GROUPED_SCENARIO)
        _save_and_reopen_current_scenario(stack, GROUPED_SCENARIO)
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=10.0) as client:
            applied = client.program_applied()
        selector = _group_selector(applied["program_binding_id"])
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=GROUPED_SCENARIO,
                source_document_digest=catalog_revision,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _start_motion(
                client,
                started,
                request_id="ui6i.group.motion-start",
            )
            accepted = _operation_acceptances(stack, program_run_id)
            intents = [event["payload"]["intent"] for event in accepted]
            assert len({event["operation_id"] for event in accepted}) == 2
            assert {intent["coordination_policy"] for intent in intents} == {"independent"}
            assert {
                intent["participants"][0]["target_role_id"] for intent in intents
            } == {"lite6_arm", "ur5e_arm"}
            assert all(len(intent["participants"]) == 1 for intent in intents)

            scaled = _set_scale(client, selector, 0.2, "ui6i.group.scale.active")
            assert scaled["accepted"]["control_generation"] == 1
            scale_status = _wait_scale_applied(client, selector, 0.2)
            assert scale_status["requested_scale"] == 0.2
            assert scale_status["effective_scale"] == 0.2
            assert len(scale_status["group_members"]) == 2

            stop = client.program_run_motion_stop(
                program_run_id,
                {
                    "request_id": "ui6i.group.stop",
                    "request_owner_epoch": "ui6i.grouped.process-test",
                    "source_label": "ui6i_grouped_process_test",
                    "correlation_id": "ui6i.group.stop",
                    "program_run_id": program_run_id,
                    "expected_owner_epoch": started["owner_epoch"],
                    "reason": "qualify grouped ordinary Stop",
                    "group_ids": ["robot_group"],
                },
            )
            assert stop["state"] == "running"
            cycle, selections = _wait_group_held(client, program_run_id, "robot_group")
            held_run = cycle["run"]
            assert held_run is not None
            resumed = client.program_run_motion_resume(
                program_run_id,
                {
                    "request_id": "ui6i.group.resume",
                    "request_owner_epoch": "ui6i.grouped.process-test",
                    "source_label": "ui6i_grouped_process_test",
                    "correlation_id": "ui6i.group.resume",
                    "program_run_id": program_run_id,
                    "expected_owner_epoch": cycle["program_runner_owner_epoch"],
                    "expected_run_revision": held_run["run_revision"],
                    "expected_selection_revision": selections["selection_revision"],
                    "reason": "resume both robots from current observations",
                    "group_ids": ["robot_group"],
                },
            )
            assert resumed["state"] == "running"
            terminal = _wait_run_state(client, program_run_id, {"succeeded"})
            assert terminal["state"] == "succeeded"
            _reset_cycle(client, program_run_id)


def test_separate_group_stop_does_not_stop_the_other_robot() -> None:
    with _dual_robot_stack() as stack:
        applied, catalog_revision = _load_scenario(stack, ISOLATION_SCENARIO)
        lite6_selector = {
            "program_binding_id": applied["program_binding_id"],
            "component_role_id": "lite6_robot",
            "target_role_id": "lite6_arm",
        }
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=ISOLATION_SCENARIO,
                source_document_digest=catalog_revision,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _start_motion(
                client,
                started,
                request_id="ui6i.isolation.motion-start",
            )
            accepted = _operation_acceptances(stack, program_run_id)
            accepted_by_target = {
                event["payload"]["intent"]["participants"][0]["target_role_id"]: event
                for event in accepted
            }
            assert set(accepted_by_target) == {"lite6_arm", "ur5e_arm"}

            _set_scale(client, lite6_selector, 0.1, "ui6i.isolation.scale")
            scale_status = _wait_scale_applied(client, lite6_selector, 0.1)
            assert scale_status["requested_scale"] == 0.1
            assert scale_status["effective_scale"] == 0.1

            client.program_run_motion_stop(
                program_run_id,
                {
                    "request_id": "ui6i.isolation.stop-lite6",
                    "request_owner_epoch": "ui6i.grouped.process-test",
                    "source_label": "ui6i_grouped_process_test",
                    "correlation_id": "ui6i.isolation.stop-lite6",
                    "program_run_id": program_run_id,
                    "expected_owner_epoch": started["owner_epoch"],
                    "reason": "qualify separate-group isolation",
                    "group_ids": ["lite6_arm"],
                },
            )
            _wait_group_held(client, program_run_id, "lite6_arm")

            native_events = _native_events(stack)
            stop_events = [
                event
                for event in native_events
                if event["record_kind"] == "stop_acceptance_projected"
                and event["request_id"].startswith("motion:control-receiver.")
                and event["request_id"].endswith(":ui6i.isolation.stop-lite6")
            ]
            assert len(stop_events) == 1, [
                (event["request_id"], event["payload"])
                for event in native_events
                if event["record_kind"] == "stop_acceptance_projected"
            ]
            affected = stop_events[0]["payload"]["affected_state"]["operations"]
            assert len(affected) == 1
            assert affected[0]["operation_id"] == (
                accepted_by_target["lite6_arm"]["operation_id"]
            )
            assert affected[0]["operation_id"] != (
                accepted_by_target["ur5e_arm"]["operation_id"]
            )

            _set_scale(client, lite6_selector, 1.0, "ui6i.isolation.restore-scale")
            restored_scale = client.runtime_scale_control(lite6_selector)[
                "runtime_scale_control"
            ]
            assert restored_scale["requested_scale"] == 1.0
            assert restored_scale["effective_scale"] is None

            cycle = client.program_cycle_status()
            selections = cycle["runtime_controls"]["execution_selections"]
            resumed = client.program_run_motion_resume(
                program_run_id,
                {
                    "request_id": "ui6i.isolation.resume-lite6",
                    "request_owner_epoch": "ui6i.grouped.process-test",
                    "source_label": "ui6i_grouped_process_test",
                    "correlation_id": "ui6i.isolation.resume-lite6",
                    "program_run_id": program_run_id,
                    "expected_owner_epoch": cycle["program_runner_owner_epoch"],
                    "expected_run_revision": cycle["run"]["run_revision"],
                    "expected_selection_revision": selections["selection_revision"],
                    "reason": "resume only the selected robot",
                    "group_ids": ["lite6_arm"],
                },
            )
            assert resumed["state"] == "running"
            terminal = _wait_run_state(client, program_run_id, {"succeeded"})
            assert terminal["state"] == "succeeded"
            _reset_cycle(client, program_run_id)

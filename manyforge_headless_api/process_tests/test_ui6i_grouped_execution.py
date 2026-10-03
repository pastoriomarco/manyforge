"""UI6-I T3/T4 process qualification for one native two-robot cell."""

from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import json
import shutil
import sys
from tempfile import TemporaryDirectory
import time
import threading
import uuid
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
# Near the wall the UR5e start keeps about 9 cm of clearance, while its
# Program goal reaches about 6 cm through the wall.
MOVED_UR5E_ROOT_POSITION_M = [-0.2, 0.68, 0.0]


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


def _load_scenario(
    stack: ProcessStack,
    scenario_id: str,
    *,
    scenario_path: Path | None = None,
    executable: bool = True,
) -> tuple[dict[str, Any], str]:
    # Each deliberate Load is a new Manager request, even when it loads the
    # same Scenario again. Retries within this one Load keep their own distinct
    # attempt identities because a stale admission snapshot changes the
    # request's expected generation.
    load_identity = uuid.uuid4().hex
    with (
        _composer_snapshot_stream(stack, client_id=f"ui6i-{scenario_id}") as snapshots,
        HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
    ):
        scenarios = composer.request("GET", "/api/scenario-documents")
        rows = {row["scenario_id"]: row for row in scenarios["entries"]}
        ids = set(rows)
        assert {GROUPED_SCENARIO, ISOLATION_SCENARIO} <= ids
        path = str(scenario_path) if scenario_path is not None else rows[scenario_id]["path"]
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
                            request_id=(
                                f"ui6i.{scenario_id}.load.{load_identity}.{attempt}"
                            ),
                        ),
                        "deploymentDraftId": deployment["draft"]["id"],
                        "expectedDeploymentDraftRevision": deployment["draft"][
                            "revision"
                        ],
                        "path": path,
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
                    try:
                        manager_status = composer.manager_status()["status"]
                        active = manager_status.get("active")
                        publication = (
                            active.get("publication")
                            if isinstance(active, dict)
                            else None
                        )
                        records = manager_status.get("apply", {}).get(
                            "records", []
                        )
                        manager_summary = {
                            "manager_ready": manager_status.get(
                                "manager_ready"
                            ),
                            "active_publication": publication,
                            "recent_applies": [
                                {
                                    "apply_id": row.get("apply_id"),
                                    "phase": row.get("phase"),
                                    "terminal_state": row.get(
                                        "terminal_state"
                                    ),
                                    "error": row.get("error"),
                                    "failure": row.get("failure"),
                                    "scenario_id": row.get(
                                        "prepared_activation", {}
                                    ).get("scenario_id"),
                                    "request": row.get("request"),
                                }
                                for row in records[-4:]
                                if isinstance(row, dict)
                            ],
                        }
                    except Exception as status_error:  # noqa: BLE001
                        manager_summary = {"read_error": str(status_error)}
                    raise AssertionError(
                        f"{scenario_id}: Load failed: code={error.code!r}, "
                        f"response={error.response!r}, "
                        f"Manager={manager_summary!r}"
                    ) from error
                draft = composer.request("GET", "/api/program/draft")
        assert loaded["status"] == "accepted"
        assert loaded["path"] == path
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
                    assert status["state"] == "succeeded", (
                        f"{scenario_id}: terminal Apply status {status!r}"
                    )
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
        if executable:
            applied = composer.program_applied()
        else:
            with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
                applied = headless.request("GET", "/api/program/applied/status")
            assert applied["executable"] is False
            assert applied["program_binding_id"] is None
            assert applied["non_executable"]["unresolved_roles"]
        assert applied["scenario_id"] == scenario_id
        return (
            applied,
            applied["source_document_digest"],
        )


def _save_and_reopen_current_scenario(
    stack: ProcessStack, scenario_id: str
) -> dict[str, Any]:
    directory = stack.root / "saved-scenarios"
    directory.mkdir()
    target = directory / f"{scenario_id}.scenario"
    with (
        _composer_snapshot_stream(stack, client_id=f"ui6i-save-{scenario_id}") as snapshots,
        HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
        HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless,
    ):
        manager_before = composer.manager_status()["status"]["active"]
        applied_before = composer.program_applied()
        components_before = {
            component_id: headless.cclc_component(component_id)
            for component_id in ("lite6_robot", "ur5e_robot")
        }
        save_identity = uuid.uuid4().hex
        saved: dict[str, Any] | None = None
        for attempt in range(8):
            draft = composer.request("GET", "/api/program/draft")
            snapshot = _matching_composer_snapshot(snapshots, draft)
            deployment = composer.request("GET", "/api/deployment/workspace")
            try:
                saved = composer.request(
                    "POST",
                    "/api/scenario-documents/save",
                    body={
                        **_composer_fence(
                            snapshot,
                            request_id=(
                                f"ui6i.{scenario_id}.save.{save_identity}.{attempt}"
                            ),
                        ),
                        "deploymentDraftId": deployment["draft"]["id"],
                        "expectedDeploymentDraftRevision": deployment["draft"][
                            "revision"
                        ],
                        "path": str(target),
                        "scenarioId": f"{scenario_id}_saved",
                        "name": "UI6-I grouped Scenario copy",
                        "description": "Native Save/Load qualification copy",
                        "confirmOverwrite": False,
                    },
                )
                break
            except HeadlessClientError as error:
                # These exact admission refusals occur before the Save claim or
                # filesystem write. Refresh the coherent snapshot and retry;
                # never retry an ambiguous or effectful outcome.
                if error.code not in {"authoring_not_ready", "stale_generation"} or attempt == 7:
                    raise
        assert saved is not None
        assert saved["status"] == "saved"
        # Saving records content, not validation against a new activation.
        # A complete newly saved document therefore starts with an unknown
        # status until it is loaded and qualified again.
        assert saved["last_known_status"] == "unknown"
        assert saved["path"] == str(target.resolve())
        # Save only persists authored content. Load below deliberately activates
        # the saved copy and may publish a new activation of the same runtime.
        assert composer.manager_status()["status"]["active"] == manager_before
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
        assert reopened["status"] in {"accepted", "applied"}
        assert reopened["path"] == str(target.resolve())
        activation = reopened["activation"]
        if reopened["status"] == "accepted":
            deadline = time.monotonic() + 30.0
            while True:
                status = headless.manager_apply_status(
                    activation["apply_id"], activation["apply_generation"]
                )
                if status["state"] != "pending":
                    assert status["state"] == "succeeded", (
                        f"reopened Scenario Apply did not succeed: {status!r}"
                    )
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError(
                        "reopened Scenario Apply did not settle: " f"{status!r}"
                    )
                time.sleep(0.02)
        stack.wait_authoring_admission(expected_allowed=True)
        manager_after = composer.manager_status()["status"]["active"]
        for key in ("retained_bundle_id", "graph_revision", "cell_graph"):
            assert manager_after[key] == manager_before[key]
        for component_id, before in components_before.items():
            after = headless.cclc_component(component_id)
            for key in (
                "component_lifecycle_generation",
                "component_lifecycle_state",
                "domains",
                "provider_occurrences",
            ):
                assert after[key] == before[key], (component_id, key, before, after)
        applied = composer.program_applied()
        assert applied["scenario_id"] == f"{scenario_id}_saved"
        for key in ("program_revision", "program_binding_id"):
            assert applied[key] == applied_before[key]
        return applied


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
    owner_epoch = client.execution_diagnostics()["status"]["identity"][
        "owner_epoch"
    ]
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


def _write_mixed_variants(
    stack: ProcessStack, kinds: tuple[str, ...]
) -> dict[str, Path]:
    """Writes GROUPED variants: `single` keeps only lite6, `solo` also keeps
    only lite6's Program branch so the survivor stays executable, `replace`
    changes only ur5e's controller input so lite6 is retained by the
    component delta, and `move` changes only ur5e's root pose, which is
    shared cell planning model rather than controller identity, so both
    robots are retained."""
    # Keep Composer out of native-only test collection. These helpers are
    # required only by owner-connected scenario tests.
    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    examples = _required_path("MANYFORGE_EXAMPLE_SCENARIO_ROOT")
    source = examples / f"{GROUPED_SCENARIO}.scenario"
    document, _ = load_scenario_artifact(str(source), allowed_roots=[str(examples)])
    variants = stack.root / "mixed-scenarios"
    variants.mkdir()
    paths: dict[str, Path] = {}
    for kind in kinds:
        candidate = deepcopy(document)
        candidate["scenario_id"] = f"mixed_{kind}"
        candidate["name"] = f"Mixed Apply {kind}"
        components = candidate["deployment_draft"]["components"]
        if kind in ("single", "solo"):
            candidate["deployment_draft"]["components"] = [
                row for row in components if row["component_id"] == "lite6_robot"
            ]
        if kind == "solo":
            program = candidate["program_draft"]
            for root in program["forest"]:
                root["children"] = [
                    child for child in root["children"]
                    if child["instance_id"] != "ur5e_joint_motion"
                ]
            del program["component_roles"]["ur5e_robot"]
            del program["target_roles"]["ur5e_arm"]
            assignment = candidate["program_role_assignment"]
            assignment["component_roles"] = [
                row for row in assignment["component_roles"]
                if row["component_role_id"] != "ur5e_robot"
            ]
            assignment["target_roles"] = [
                row for row in assignment["target_roles"]
                if row["target_role_id"] != "ur5e_arm"
            ]
        elif kind == "move":
            ur5e = next(row for row in components if row["component_id"] == "ur5e_robot")
            ur5e["root_pose_in_cell"]["position_m"] = list(MOVED_UR5E_ROOT_POSITION_M)
        elif kind == "replace":
            ur5e = next(row for row in components if row["component_id"] == "ur5e_robot")
            # Change only B's controller input; A must survive unchanged.
            ur5e["configuration"]["runtime_configuration"]["initial_joint_positions_rad"][0] = 0.1
        else:
            assert kind == "single", kind
        target = variants / f"mixed_{kind}.scenario"
        candidate = build_scenario_document(
            scenario_id=candidate["scenario_id"],
            name=candidate["name"],
            description=candidate["description"],
            program_draft=candidate["program_draft"],
            deployment_draft=candidate["deployment_draft"],
            program_role_assignment=candidate["program_role_assignment"],
            manyforge_version=candidate["saved_status"]["manyforge_version"],
            last_known_status="unknown",
        )
        save_scenario_artifact(
            candidate, str(target),
            allowed_roots=[str(examples), str(stack.root)],
            overwrite=False, source_project_path=str(source),
        )
        paths[kind] = target
    return paths


def test_retained_robot_plans_after_apply_retires_its_first_composition() -> None:
    # The retained lite6 runtime keeps the RoboPlan worker built by the first
    # composition. A changed candidate reselects the worker projection, which
    # restarts the worker from that first planning-model stage after the first
    # composition was retired. Both robots must still plan and execute.
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            survivor = headless.cclc_component("lite6_robot")
        paths = _write_mixed_variants(stack, ("replace",))
        applied, source_document_digest = _load_scenario(
            stack, "mixed_replace", scenario_path=paths["replace"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            retained = headless.cclc_component("lite6_robot")
        assert retained["provider_occurrences"] == survivor["provider_occurrences"]
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=applied["scenario_id"],
                source_document_digest=source_document_digest,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _start_motion(client, started, request_id="ui6i.retained.motion-start")
            accepted = _operation_acceptances(stack, program_run_id)
            assert {
                event["payload"]["intent"]["participants"][0]["target_role_id"]
                for event in accepted
            } == {"lite6_arm", "ur5e_arm"}
            terminal = _wait_run_state(
                client,
                program_run_id,
                {"succeeded", "failed", "intervention_required", "stopped"},
            )
            assert terminal["state"] == "succeeded", terminal
            _reset_cycle(client, program_run_id)


def test_retained_robot_plans_in_the_candidate_cell_planning_model() -> None:
    # Moving ur5e changes the whole-cell planning model but neither controller
    # identity, so both runtimes and their RoboPlan workers are retained. The
    # retained ur5e worker must plan in the moved cell, where the goal is in
    # collision with the wall. A worker still planning in the first
    # composition's cell would plan the motion and leave the collision for
    # pre-dispatch validation to reject.
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        robots = ("lite6_robot", "ur5e_robot")
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            survivors = {robot: headless.cclc_component(robot) for robot in robots}
        paths = _write_mixed_variants(stack, ("move",))
        applied, source_document_digest = _load_scenario(
            stack, "mixed_move", scenario_path=paths["move"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            for robot in robots:
                retained = headless.cclc_component(robot)
                assert (
                    retained["provider_occurrences"]
                    == survivors[robot]["provider_occurrences"]
                ), robot
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=applied["scenario_id"],
                source_document_digest=source_document_digest,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _start_motion(client, started, request_id="ui6i.moved.motion-start")
            terminal = _wait_run_state(
                client,
                program_run_id,
                {"succeeded", "failed", "intervention_required", "stopped"},
            )
        assert terminal["state"] != "succeeded", terminal
        ur5e_operations = {
            event["operation_id"]
            for event in _native_events(stack)
            if event["record_kind"] == "operation_accepted"
            and event["payload"]["intent"]["participants"][0]["target_role_id"]
            == "ur5e_arm"
        }
        assert len(ur5e_operations) == 1, ur5e_operations
        native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=5.0)
        evidence = json.dumps(native.read(ur5e_operations.pop()), sort_keys=True)
        assert "native_goal_state_collision" in evidence, evidence
        assert "validation_collision" not in evidence, evidence


def test_survivor_moves_after_its_peer_is_removed_from_the_cell() -> None:
    # Removing ur5e removes its joints from the whole-cell planning model, so
    # every retained lite6 model coordinate changes index. The retained lite6
    # controller, Kernel and observed source survive; its planner, scene,
    # kinematics, validator, collision and state projections follow the
    # candidate model, and its Program motion succeeds.
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            survivor = headless.cclc_component("lite6_robot")
        paths = _write_mixed_variants(stack, ("solo",))
        applied, source_document_digest = _load_scenario(
            stack, "mixed_solo", scenario_path=paths["solo"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            retained = headless.cclc_component("lite6_robot")
        assert retained["provider_occurrences"] == survivor["provider_occurrences"]
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=applied["scenario_id"],
                source_document_digest=source_document_digest,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _start_motion(client, started, request_id="ui6i.solo.motion-start")
            accepted = _operation_acceptances(stack, program_run_id, minimum=1)
            assert {
                event["payload"]["intent"]["participants"][0]["target_role_id"]
                for event in accepted
            } == {"lite6_arm"}
            terminal = _wait_run_state(
                client,
                program_run_id,
                {"succeeded", "failed", "intervention_required", "stopped"},
            )
            assert terminal["state"] == "succeeded", terminal
            _reset_cycle(client, program_run_id)


def test_mixed_apply_preserves_survivor_and_observations_during_publication() -> None:
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        paths = _write_mixed_variants(stack, ("single", "replace"))

        def target_keys(status: dict[str, Any]) -> set[tuple[str, str, str]]:
            return {
                (component["controllable_component_id"], target["command_target_id"],
                 target["operation_capability_id"])
                for component in status["targets"]["components"]
                for target in component["targets"] if target["command"] is not None
            }

        def inventory_keys(targets: dict[str, Any]) -> set[tuple[str, str, str]]:
            rows = [
                (component["controllable_component_id"], target["command_target_id"],
                 target["operation_capability_id"])
                for component in targets["components"] for target in component["targets"]
            ]
            assert len(rows) == len(set(rows)), targets
            return set(rows)

        def keyed_observations(rows: list[dict[str, Any]]) -> dict[tuple[str, str, str], dict[str, Any]]:
            result = {
                (row["controllable_component_id"], row["command_target_id"],
                 row["operation_capability_id"]): row for row in rows
            }
            assert len(result) == len(rows), rows
            return result

        def survivor_facts(snapshot: dict[str, Any]) -> dict[str, Any]:
            # Product evidence may be re-sealed to the new bundle. Assert the
            # controller, lifecycle and route facts that must remain stable.
            return {
                "generation": snapshot["component_lifecycle_generation"],
                "state": snapshot["component_lifecycle_state"],
                "domains": snapshot["domains"],
                "owners": [
                    {key: row[key] for key in (
                        "control_domain_id",
                        "provider_instance_id", "provider_runtime_unit_id",
                        "provider_runtime_epoch",
                    )}
                    for row in snapshot["provider_occurrences"]
                ],
            }

        stop_reader = threading.Event()
        reader_started = threading.Event()

        def read_publications() -> int:
            native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=5.0)
            count = 0
            sequences = dict(initial_survivor_sequences)
            while not stop_reader.is_set():
                # Use the production render projection without competing for
                # Composer/headless's dedicated single-client stream lane.
                assert_observation(native.observation(), sequences)
                count += 1
                reader_started.set()
            return count

        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            survivor = survivor_facts(headless.cclc_component("lite6_robot"))
            native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=5.0)
            initial_diagnostics = native.diagnostics()
            assert initial_diagnostics.get("ok") is True, initial_diagnostics
            initial_status = initial_diagnostics["status"]
            assert initial_status["schema_version"] == "manyforge.execution_status.v2"
            host_epoch = initial_status["identity"]["owner_epoch"]
            dual_inventory = inventory_keys(initial_status["targets"])
            dual_supported = target_keys(initial_status)
            single_inventory = {key for key in dual_inventory if key[0] == "lite6_robot"}
            single_supported = {key for key in dual_supported if key[0] == "lite6_robot"}
            assert {key[0] for key in dual_inventory} == {"lite6_robot", "ur5e_robot"}

            initial_response = native.observation()
            assert initial_response.get("ok") is True, initial_response
            initial_observation = initial_response["observation"]
            wake_epoch = initial_observation["source_epoch"]
            assert wake_epoch
            initial_rows = keyed_observations(initial_observation["targets"])
            dual_render = set(initial_rows)
            single_render = {key for key in dual_render if key[0] == "lite6_robot"}
            assert dual_render <= dual_supported
            assert {key[0] for key in dual_render} == {"lite6_robot", "ur5e_robot"}
            assert single_render

            # The initial verified frame defines the exact render inventory.
            # Cross-check its coordinates against settled command payloads;
            # the producer omits only component-local gripper resources.
            coordinate_shapes: dict[tuple[str, str, str], tuple[list[str], list[str]]] = {}
            for component in initial_status["targets"]["components"]:
                for target in component["targets"]:
                    command = target["command"]
                    if command is None:
                        continue
                    key = (component["controllable_component_id"], target["command_target_id"],
                           target["operation_capability_id"])
                    if target["presentation"] == "motion":
                        assert key in dual_render, target
                        names = [joint["source_joint_name"] for joint in command["joints"]]
                        coordinate_shapes[key] = (names, ["angular"] * len(names))
                    else:
                        assert target["presentation"] == "gripper", target
                        resource = command["resource"]
                        if key not in dual_render:
                            assert resource["source_kind"] == "component_local", target
                            continue
                        assert resource["source_kind"] == "model_role", target
                        kind = {"angular_rad": "angular", "linear_m": "linear"}[resource["coordinate_kind"]]
                        coordinate_shapes[key] = ([resource["source_coordinate_name"]], [kind])
            assert set(coordinate_shapes) == dual_render
            survivor_epochs = {key: initial_rows[key]["source_epoch"] for key in single_render}
            initial_survivor_sequences = {
                key: initial_rows[key]["source_sequence"] for key in single_render
            }

            def assert_observation(
                response: dict[str, Any], sequences: dict[tuple[str, str, str], int],
            ) -> set[tuple[str, str, str]]:
                assert response.get("ok") is True, response
                observation = response["observation"]
                assert observation["schema_version"] == "manyforge.execution_observation.v3"
                assert observation["owner_epoch"] == host_epoch
                assert observation["source_epoch"] == wake_epoch
                rows = keyed_observations(observation["targets"])
                keys = set(rows)
                assert keys == dual_render or keys == single_render, observation
                for key, row in rows.items():
                    names, kinds = coordinate_shapes[key]
                    assert names
                    assert row["source_joint_names"] == names, row
                    assert row["coordinate_kinds"] == kinds, row
                    assert len(row["compiled_joint_names"]) == len(names), row
                    assert len(set(row["compiled_joint_names"])) == len(names), row
                    assert row["compiled_joint_names"] == initial_rows[key]["compiled_joint_names"], row
                    assert len(row["positions_si"]) == len(names), row
                    assert row["source_epoch"], row
                    assert row["source_sequence"] >= 0, row
                    if key in single_render:
                        assert row["source_epoch"] == survivor_epochs[key], row
                        assert row["source_sequence"] >= sequences[key], row
                        sequences[key] = row["source_sequence"]
                return keys

            def assert_settled_control_state(
                control_state: dict[str, Any], expected: set[tuple[str, str, str]],
                *, check_sequences: bool = True,
            ) -> None:
                assert control_state["schema_version"] == "manyforge.execution_control_state.v6"
                rows = keyed_observations(control_state["observed_targets"])
                assert set(rows) == expected, control_state
                for key in single_render:
                    observed = rows[key]["observed"]
                    assert observed["complete"] is True, rows[key]
                    assert observed["source_epoch"] == survivor_epochs[key], rows[key]
                    if check_sequences:
                        assert observed["sequence"] >= initial_survivor_sequences[key], rows[key]

            settled_sequences = dict(initial_survivor_sequences)
            assert assert_observation(initial_response, settled_sequences) == dual_render
            assert_settled_control_state(
                initial_status["control_state"], dual_supported, check_sequences=False,
            )
            with ThreadPoolExecutor(max_workers=1) as executor:
                reader = executor.submit(read_publications)
                try:
                    if not reader_started.wait(timeout=10.0):
                        if reader.done():
                            reader.result()
                        raise AssertionError("concurrent publication reader did not start")
                    for kind in ("single", "restore", "replace", "restore"):
                        _load_scenario(
                            stack, GROUPED_SCENARIO if kind == "restore" else f"mixed_{kind}",
                            scenario_path=None if kind == "restore" else paths[kind],
                            executable=kind != "single",
                        )
                        current = survivor_facts(headless.cclc_component("lite6_robot"))
                        assert current == survivor, (kind, survivor, current)
                        expected_inventory = single_inventory if kind == "single" else dual_inventory
                        expected_supported = single_supported if kind == "single" else dual_supported
                        diagnostics = native.diagnostics()
                        assert diagnostics.get("ok") is True, diagnostics
                        status = diagnostics["status"]
                        assert status["schema_version"] == "manyforge.execution_status.v2"
                        assert status["identity"]["owner_epoch"] == host_epoch
                        assert inventory_keys(status["targets"]) == expected_inventory
                        assert target_keys(status) == expected_supported
                        assert_settled_control_state(status["control_state"], expected_supported)
                        identity = native.identity()
                        assert identity.get("ok") is True, identity
                        assert identity["identity"]["owner_epoch"] == host_epoch
                        targets = native.targets()
                        assert targets.get("ok") is True, targets
                        assert targets["targets"]["schema_version"] == "manyforge.execution_targets.v3"
                        assert inventory_keys(targets["targets"]) == expected_inventory
                        control_state = native.control_state()
                        assert control_state.get("ok") is True, control_state
                        assert_settled_control_state(control_state["control_state"], expected_supported)
                        assert assert_observation(native.observation(), settled_sequences) == (
                            single_render if kind == "single" else dual_render
                        )
                        if reader.done():
                            reader.result()
                            raise AssertionError("concurrent publication reader exited early")
                finally:
                    stop_reader.set()
                assert reader.result(timeout=10.0) > 0


def test_grouped_dual_robot_run_scales_stops_and_resumes() -> None:
    with _dual_robot_stack() as stack:
        _load_scenario(stack, GROUPED_SCENARIO)
        applied = _save_and_reopen_current_scenario(stack, GROUPED_SCENARIO)
        selector = _group_selector(applied["program_binding_id"])
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=applied["scenario_id"],
                source_document_digest=applied["source_document_digest"],
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

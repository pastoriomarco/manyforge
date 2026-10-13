"""UI6-I T3/T4 process qualification for one native two-robot cell."""

from __future__ import annotations

from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import json
import hashlib
import shutil
import sys
from tempfile import TemporaryDirectory
import time
import threading
import uuid
from pathlib import Path
from typing import Any, Callable, Iterator

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import TargetObservationQuery
from manyforge_headless_api.process_tests.test_process_stack import (
    keep_failure_evidence,
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
                stack.prepare_native_package(ADMISSION_FILENAME, ros2_control=True)
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
                keep_failure_evidence(root)
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


def _applied_activation_order(stack: ProcessStack) -> list[str]:
    """Use the captured applied Deployment order, never the mutable Draft."""
    with (
        HeadlessHttpClient(stack.composer_url, timeout_seconds=10.0) as composer,
        HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless,
    ):
        selected = composer.request("GET", "/api/deployment/workspace")["managerApplied"]
        order = selected["activationOrder"]
        assert order is not None, selected
        applied = headless.program_applied_status()
        origin = applied["origin"]
        assert origin["kind"] == "scenario", applied
        assert selected["scenarioId"] == origin["scenario_id"], (selected, applied)
        assert selected["deploymentRevision"] == applied["deployment_revision"], (selected, applied)
        assert order["retainedBundleId"] == applied["retained_bundle_id"], (order, applied)
        assert order["graphRevision"] == applied["graph_revision"], (order, applied)
        component_ids = order["componentIds"]
        assert isinstance(component_ids, list) and component_ids, order
        assert len(component_ids) == len(set(component_ids)), order
        for component_id in component_ids:
            current = headless.cclc_component(component_id)
            assert current["retained_bundle_id"] == order["retainedBundleId"], current
            assert current["graph_revision"] == order["graphRevision"], current
        return component_ids


def _component_lifecycle(
    stack: ProcessStack, component_id: str, intent: str, owner_epoch: str
) -> dict[str, Any]:
    """Submit one ordinary confirmed command with actual current CAS facts."""
    achieved = {"prepare": "prepared", "enable": "active", "disable": "disabled"}[intent]
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        assert headless.cclc_owner_identity()["request_owner_epoch"] == owner_epoch
        current = headless.cclc_component(component_id)
        expected_domains = []
        for domain in current["domains"]:
            active = domain["active_control_configuration"]
            expected_active = {"presence": "absent"} if active is None else {
                "presence": "present",
                "active_control_configuration_id": active["active_control_configuration_id"],
                "active_control_configuration_generation": active["active_control_configuration_generation"],
            }
            expected_domains.append({
                **{key: domain[key] for key in (
                    "control_domain_id", "control_domain_revision",
                    "controller_topology_id", "topology_revision",
                )},
                "expected_active_control_configuration": expected_active,
                "expected_control_bindings": domain["current_control_bindings"],
            })
        admission = stack.wait_authoring_admission(expected_allowed=True)
        request = {
            "request_context": {
                "request_id": f"ui6i.{component_id}.{intent}.{uuid.uuid4().hex}",
                "request_owner_epoch": owner_epoch,
                "source_label": "ui6i_process_fixture",
            },
            "controllable_component_id": component_id,
            "intent": intent,
            "expected_graph_revision": current["graph_revision"],
            "expected_component_lifecycle_generation": current["component_lifecycle_generation"],
            "expected_control_domains": expected_domains,
            "expected_admission_generation": admission["admission_generation"],
            "confirmed": True,
        }
        # A refusal or unknown response stops the sequence. Never repeat effects.
        submitted = headless.submit_component_lifecycle(request)
        acceptance = submitted["acceptance"]
        assert acceptance is not None, submitted
        deadline = time.monotonic() + 20.0
        while True:
            status = headless.component_lifecycle_status(acceptance["lifecycle_transaction_id"])["status"]
            assert status is not None, acceptance
            assert status["status_owner_epoch"] == owner_epoch, status
            assert status["acceptance"] == acceptance, status
            if status["terminal_result"] is not None:
                assert status["terminal_result"] == "succeeded", status
                break
            assert time.monotonic() < deadline, status
            time.sleep(0.01)
        assert status["current_state"] == achieved, status
        assert {row["control_domain_id"] for row in status["domains"]} == {
            row["control_domain_id"] for row in expected_domains
        }, status
        assert len(status["domains"]) == len(expected_domains), status
        assert all(row["current_state"] == achieved for row in status["domains"]), status
        installed = headless.cclc_component(component_id)
        assert installed["component_lifecycle_state"] == achieved, installed
        assert headless.cclc_owner_identity()["request_owner_epoch"] == owner_epoch
        return status


def _activate_loaded_components(stack: ProcessStack) -> list[str]:
    component_ids = _applied_activation_order(stack)
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        owner_epoch = headless.cclc_owner_identity()["request_owner_epoch"]

        def wait_ready(initial: dict[str, Any], intent: str, requested: str) -> None:
            identity_fields = (
                "controllable_component_id", "retained_bundle_id", "graph_revision",
                "publication_generation", "active_public_pointer_digest",
                "component_lifecycle_generation", "component_lifecycle_state",
                "provider_occurrences", "domains",
            )
            identity = {key: initial[key] for key in identity_fields}
            start_wallclock = time.time()
            started = time.monotonic()
            deadline = started + 20.0
            trace: list[dict[str, Any]] = []
            trace_dropped = 0
            previous_facts = None
            while True:
                assert headless.cclc_owner_identity()["request_owner_epoch"] == owner_epoch
                current = headless.cclc_component(initial["controllable_component_id"])
                assert {key: current[key] for key in identity_fields} == identity, current
                readiness = current["readiness"]
                assert readiness["request_owner_epoch"] == owner_epoch, current
                assert readiness["next_intent"] == intent, current
                assert readiness["requested_state"] == requested, current
                assert type(readiness["ready"]) is bool, current
                error = readiness.get("error") or {}
                observed_codes = {
                    "ready": readiness["ready"],
                    "error_code": str(error.get("code", "")),
                    "error_reason": str((error.get("details") or {}).get("reason", "")),
                    "domain_codes": [str((row.get("error") or {}).get("code", ""))
                                     for row in readiness.get("domains", [])],
                    "measured_code": str(((readiness.get("measured_state") or {}).get("error") or {}).get("code", "")),
                }
                facts = json.dumps(observed_codes, sort_keys=True, separators=(",", ":"))
                if facts != previous_facts:
                    error = readiness.get("error") or {}
                    trace.append({
                        "elapsed_seconds": time.monotonic() - started,
                        "facts_sha256": hashlib.sha256(facts.encode()).hexdigest(),
                        "ready": readiness["ready"],
                        "error_code": str(error.get("code", ""))[:256],
                        "error_reason": str((error.get("details") or {}).get("reason", ""))[:256],
                        "domain_codes": [code[:256] for code in observed_codes["domain_codes"][:64]],
                        "measured_code": observed_codes["measured_code"][:256],
                    })
                    if len(trace) > 64:
                        trace.pop(0)
                        trace_dropped += 1
                    previous_facts = facts
                if readiness["ready"]:
                    return
                assert readiness["error"] is not None, current
                if time.monotonic() >= deadline:
                    encoded = json.dumps(current, sort_keys=True, separators=(",", ":")).encode()
                    evidence: dict[str, Any] = {
                        "start_wallclock_seconds": start_wallclock,
                        "join_identity": identity,
                        "request_owner_epoch": owner_epoch,
                        "provider_runtime_epochs": [row.get("runtime_epoch")
                                                    for row in current["provider_occurrences"]],
                        "intent": intent, "requested_state": requested,
                        "snapshot_sha256": hashlib.sha256(encoded).hexdigest(),
                        "snapshot_bytes": len(encoded), "truncated": False,
                        "change_trace": trace, "trace_dropped_count": trace_dropped,
                        "snapshot": current,
                    }
                    cap = 256 * 1024
                    payload = json.dumps(evidence, sort_keys=True).encode()
                    if len(payload) > cap:
                        del evidence["snapshot"]
                        evidence["truncated"] = True
                        # Bound identity too; its complete hash remains in the
                        # receipt when the last exact body exceeds the cap.
                        identity_bytes = json.dumps(identity, sort_keys=True).encode()
                        evidence["join_identity_sha256"] = hashlib.sha256(identity_bytes).hexdigest()
                        evidence["join_identity"] = str(identity)[:4096]
                        epochs_bytes = json.dumps(evidence["provider_runtime_epochs"], sort_keys=True).encode()
                        evidence["provider_runtime_epochs_sha256"] = hashlib.sha256(epochs_bytes).hexdigest()
                        evidence["provider_runtime_epochs"] = str(evidence["provider_runtime_epochs"])[:4096]
                        evidence["snapshot_utf8_prefix"] = encoded[: cap // 4].decode("utf-8", errors="replace")
                        payload = json.dumps(evidence, sort_keys=True).encode()
                        while len(payload) > cap and evidence["snapshot_utf8_prefix"]:
                            evidence["snapshot_utf8_prefix"] = evidence["snapshot_utf8_prefix"][:len(evidence["snapshot_utf8_prefix"]) // 2]
                            payload = json.dumps(evidence, sort_keys=True).encode()
                        while len(payload) > cap and evidence["change_trace"]:
                            evidence["change_trace"].pop(0)
                            evidence["trace_dropped_count"] += 1
                            payload = json.dumps(evidence, sort_keys=True).encode()
                    assert len(payload) <= cap, "readiness evidence exceeded its byte cap"
                    evidence_path = stack.root / "readiness-last-snapshot.json"
                    evidence_path.write_bytes(payload)
                    summary = str({
                        "component_id": initial["controllable_component_id"],
                        "state": current["component_lifecycle_state"],
                        "intent": intent, "requested_state": requested,
                        "ready": observed_codes["ready"],
                        "error_code": observed_codes["error_code"][:128],
                        "reason": observed_codes["error_reason"][:256],
                        "domain_codes": str(observed_codes["domain_codes"])[:512],
                        "measured_code": observed_codes["measured_code"][:128],
                        "evidence": str(evidence_path),
                        "snapshot_sha256": evidence["snapshot_sha256"],
                        "truncated": evidence["truncated"],
                    })[:2048]
                    raise AssertionError(summary)
                # Match useCCLC.ts POLL_INTERVAL_MS=250; retain the same bound.
                time.sleep(0.25)

        for component_id in component_ids:
            before = headless.cclc_component(component_id)
            assert before["controllable_component_id"] == component_id, before
            state = before["component_lifecycle_state"]
            if state == "active":
                continue  # Unchanged Active owners never cycle on config-only Apply.
            assert state in {"declared", "disabled", "prepared"}, before
            if state != "prepared":
                wait_ready(before, "prepare", "prepared")
                _component_lifecycle(stack, component_id, "prepare", owner_epoch)
                before = headless.cclc_component(component_id)
                assert before["component_lifecycle_state"] == "prepared", before
            wait_ready(before, "enable", "active")
            _component_lifecycle(stack, component_id, "enable", owner_epoch)
        assert headless.cclc_owner_identity()["request_owner_epoch"] == owner_epoch
    assert _applied_activation_order(stack) == component_ids
    return component_ids


def _disable_for_reload(stack: ProcessStack, component_id: str) -> None:
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        current = headless.cclc_component(component_id)
        assert current["component_lifecycle_state"] == "active", current
        owner_epoch = headless.cclc_owner_identity()["request_owner_epoch"]
    _component_lifecycle(stack, component_id, "disable", owner_epoch)


def _wait_components_reached_requested_target(
    stack: ProcessStack, component_ids: list[str], timeout_seconds: float = 60.0
) -> None:
    """Observe achieved targets after explicit ordinary component activation."""
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        deadline = time.monotonic() + timeout_seconds
        while True:
            states: dict[str, Any] = {}
            for component_id in component_ids:
                snapshot = headless.cclc_component(component_id)
                target = snapshot.get("lifecycle_target")
                progress = snapshot.get("lifecycle_progress")
                states[component_id] = {
                    "state": snapshot.get("component_lifecycle_state"),
                    "generation": snapshot.get("component_lifecycle_generation"),
                    "target": target,
                    "progress": progress,
                }
            if all(
                row["target"] is not None
                and row["state"] == row["target"]["requested_state"]
                and (row["progress"] is None or row["progress"]["phase"] == "satisfied")
                for row in states.values()
            ):
                return
            if time.monotonic() >= deadline:
                raise AssertionError(
                    "deployed components did not reach their requested lifecycle "
                    f"target within {timeout_seconds:.0f} s: {states!r}"
                )
            time.sleep(0.1)


def _load_scenario(
    stack: ProcessStack,
    scenario_id: str,
    *,
    scenario_path: Path | None = None,
    executable: bool = True,
    on_apply_succeeded: Callable[[], None] | None = None,
    on_configuration_applied: Callable[[dict[str, Any]], None] | None = None,
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
                    if on_apply_succeeded is not None:
                        on_apply_succeeded()
                    if on_configuration_applied is not None:
                        on_configuration_applied(status)
                    break
                if time.monotonic() >= deadline:
                    raise AssertionError(
                        f"Scenario activation did not settle: {status!r}"
                    )
                time.sleep(0.02)
        component_ids = _activate_loaded_components(stack)
        _wait_components_reached_requested_target(stack, component_ids)
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
        origin = applied if executable else applied["origin"]
        if not executable:
            assert origin["kind"] == "scenario", applied
        assert origin["scenario_id"] == scenario_id
        return (
            applied,
            origin["source_document_digest"],
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
                        "scenarioId": scenario_id,
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
        # Save only persists content. Reload configures the same logical Scenario;
        # unchanged Active owners must remain untouched.
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
        _wait_components_reached_requested_target(stack, _applied_activation_order(stack))
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
        assert applied["scenario_id"] == scenario_id
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
                stack.wait_authoring_admission(expected_allowed=True)
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
        paths = _write_mixed_variants(stack, ("isolation",))
        observed: list[tuple[str, str, str]] = []
        # Alternate Program contents within one logical Scenario. A different
        # Scenario requires Close, which is a separate qualification path.
        for scenario_path in (None, paths["isolation"], None, paths["isolation"]):
            applied, _source_document_digest = _load_scenario(
                stack, GROUPED_SCENARIO, scenario_path=scenario_path
            )
            assert applied["scenario_id"] == GROUPED_SCENARIO
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
        source_project = source
        if kind == "isolation":
            source_project = examples / f"{ISOLATION_SCENARIO}.scenario"
            candidate, _ = load_scenario_artifact(
                str(source_project), allowed_roots=[str(examples)]
            )
        # These variants qualify retention during a same-Scenario reload.
        # Their distinct paths/digests remain real independent source facts.
        candidate["scenario_id"] = GROUPED_SCENARIO
        candidate["name"] = f"Mixed Apply {kind}"
        components = candidate["deployment_draft"]["components"]
        if kind in ("single", "solo", "solo_cartesian"):
            candidate["deployment_draft"]["components"] = [
                row for row in components if row["component_id"] == "lite6_robot"
            ]
        if kind in ("solo", "solo_cartesian"):
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
            # The survivor's frame_pose query needs FK; native refuses identically without it.
            fk_role = program["target_roles"]["lite6_arm"]
            fk_role["required_capabilities"].append({
                "capability_id": "model.kinematics.fk",
                "parameters": {}, "disposition": "required",
            })
            fk_role["required_capabilities"].sort(key=lambda row: row["capability_id"])
            if kind == "solo_cartesian":
                # Same single-robot Deployment; only the Program changes.
                # Its frame/path reads must use the retained commanded source.
                role = program["target_roles"]["lite6_arm"]
                role["required_capabilities"].append({
                    "capability_id": "planning.motion.cartesian_path",
                    "parameters": {}, "disposition": "required",
                })
                role["required_capabilities"].sort(key=lambda row: row["capability_id"])
                motion = next(child for root in program["forest"] for child in root["children"]
                              if child["instance_id"] == "lite6_joint_motion")
                motion["node_id"] = "motion.follow_cartesian_path"
                # Authoring rule: value_sources is omitted when empty; unset motion defaults must reference the Program parameter.
                motion["value_sources"] = {
                    key: {"kind": "program_parameter", "reference": key}
                    for key in ("max_blend_deviation_rad", "translation_tol_m")
                }
                motion["parameters"] = {
                    "target_role": "lite6_arm", "base_frame_role": "base", "tcp_frame_role": "tcp",
                    "target_mode": "relative",
                    "target_reference": {"type": "frame_role", "frame_role_id": "tcp"},
                    "path": [{"position_m": xyz, "orientation": {"rpy_deg": [0, 0, 0]}}
                             for xyz in ([0, 0, 0], [0, 0, 0.01])],
                    "velocity_scale": 0.2, "acceleration_scale": 0.25,
                    "max_tcp_linear_speed_mps": 0.05, "execution_timeout_s": 30,
                }

        elif kind == "move":
            ur5e = next(row for row in components if row["component_id"] == "ur5e_robot")
            ur5e["root_pose_in_cell"]["position_m"] = list(MOVED_UR5E_ROOT_POSITION_M)
        elif kind == "replace":
            ur5e = next(row for row in components if row["component_id"] == "ur5e_robot")
            # Change only B's controller input; A must survive unchanged.
            ur5e["configuration"]["runtime_configuration"]["initial_joint_positions_rad"][0] = 0.1
        elif kind != "isolation":
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
            overwrite=False, source_project_path=str(source_project),
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
        _disable_for_reload(stack, "ur5e_robot")
        applied, source_document_digest = _load_scenario(
            stack, GROUPED_SCENARIO, scenario_path=paths["replace"]
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
            # Motion right after an Apply: wait for the Kernel's attention
            # state of the new configuration, not for a refusal to clear.
            _wait_kernel_attention_reconciled(stack)
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
        _disable_for_reload(stack, "ur5e_robot")
        applied, source_document_digest = _load_scenario(
            stack, GROUPED_SCENARIO, scenario_path=paths["move"]
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
            _wait_kernel_attention_reconciled(stack)
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


def _kernel_attention_reconciled(
    headless: HeadlessHttpClient, *, minimum_generation: int = 0
) -> bool:
    """Whether the Kernel holds a reconciled attention projection of the
    active configuration (spec 530 §11), at least `minimum_generation` of the
    owner's current epoch.

    The Kernel admits Program motion by its held copy alone, which lags the
    owner by up to one publication period plus its deadline; the alarm
    snapshot's `kernel_attention` names the copy the Kernel acknowledged.
    """
    runtime = headless.runtime()
    snapshot = headless.alarms(limit=0)
    held = snapshot.get("kernel_attention")
    bundle = runtime.get("active_retained_bundle_id")
    graph = runtime.get("active_graph_revision")
    return (
        isinstance(held, dict)
        and isinstance(bundle, str)
        and isinstance(graph, str)
        and held.get("reconciled") is True
        and held.get("owner_epoch") == snapshot.get("owner_epoch")
        and held.get("generation", -1) >= minimum_generation
        and held.get("active_configuration")
        == {"graph_revision": graph, "retained_bundle_id": bundle}
    )


def _wait_kernel_attention_reconciled(
    stack: ProcessStack,
    seconds: float = 10.0,
    *,
    minimum_generation: int = 0,
) -> float:
    """Wait until the Kernel would admit by attention; return when it did."""
    deadline = time.monotonic() + seconds
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        while True:
            if _kernel_attention_reconciled(
                headless, minimum_generation=minimum_generation
            ):
                return time.monotonic()
            if time.monotonic() >= deadline:
                raise AssertionError(
                    "the Kernel holds no reconciled attention projection of the "
                    f"active configuration: {headless.alarms(limit=0)!r}"
                )
            time.sleep(0.02)


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
        paths = _write_mixed_variants(stack, ("solo", "solo_cartesian"))
        _disable_for_reload(stack, "ur5e_robot")
        applied, source_document_digest = _load_scenario(
            stack, GROUPED_SCENARIO, scenario_path=paths["solo"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            retained = headless.cclc_component("lite6_robot")
        assert retained["provider_occurrences"] == survivor["provider_occurrences"]
        native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=5.0)
        owner_epoch = native.identity()["identity"]["owner_epoch"]
        for frame_role in ("base", "tcp"):
            response = native.query_target(TargetObservationQuery.model_validate({
                "schema_version": "manyforge.target_observation_query.v1",
                "owner_epoch": owner_epoch,
                "program_binding_id": applied["program_binding_id"],
                "target_role_id": "lite6_arm", "kind": "frame_pose", "frame_role_id": frame_role,
            }))
            assert response["target_observation"]["source_sequence"] > 0, response
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            assert headless.scene()["scene_runtime"]["payload"]["state"]["state"] == "open"

        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=applied["scenario_id"],
                source_document_digest=source_document_digest,
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            # The Apply restarts the alarm owner's lifecycle source: motion is
            # admitted once the Kernel holds the owner's reconciled projection
            # of the new configuration.
            _wait_kernel_attention_reconciled(stack)
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

        # A Program-only Apply after the peer removal must preserve the same
        # physical owner and use that owner's commanded frame/path publication.
        cartesian, digest = _load_scenario(
            stack, GROUPED_SCENARIO, scenario_path=paths["solo_cartesian"]
        )
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            assert headless.cclc_component("lite6_robot")["provider_occurrences"] == survivor["provider_occurrences"]
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(stack, scenario_id=cartesian["scenario_id"],
                                 source_document_digest=digest, applied=cartesian)
            program_run_id = started["capture"]["program_run_id"]
            _wait_kernel_attention_reconciled(stack)
            _start_motion(client, started, request_id="ui6i.solo.cartesian.motion-start")
            terminal = _wait_run_state(client, program_run_id,
                                      {"succeeded", "failed", "intervention_required", "stopped"})
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
                    for index, kind in enumerate(("single", "restore", "replace", "restore")):
                        # Disable only the peer about to change or be removed.
                        # The first restore adds an absent peer; the last
                        # restore replaces the preceding changed peer.
                        if kind != "restore" or index == 3:
                            _disable_for_reload(stack, "ur5e_robot")
                        _load_scenario(
                            stack, GROUPED_SCENARIO,
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
            _wait_kernel_attention_reconciled(stack)
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
            _wait_kernel_attention_reconciled(stack)
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



def _assert_stored_authoring_apply_without_linked_children(
    stack: ProcessStack, status: dict[str, Any]
) -> None:
    """Observe the owned journal; this read never acquires or constructs authority."""
    from contextlib import closing
    import sqlite3

    assert status["state"] == "succeeded", status
    revision = status["candidate_projection_revision"]
    journal = stack.manager_root / "manager-apply-journal.sqlite3"
    assert journal.is_file(), journal
    with closing(sqlite3.connect(journal.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
        connection.execute("BEGIN")
        assert connection.execute(
            "SELECT schema_id,schema_version,cell_id FROM apply_journal_metadata WHERE singleton=1"
        ).fetchone() == (
            "manyforge.internal.component-manager.apply-journal.h1a.v17", 17, stack.cell_id
        )
        key = (status["apply_id"], status["apply_generation"])
        rows = connection.execute(
            "SELECT request_id,request_owner_epoch,candidate_retained_bundle_id,candidate_graph_revision,"
            "parent_authoring_claim,expected_admission_generation FROM manager_apply_intents "
            "WHERE apply_id=? AND apply_generation=?", key,
        ).fetchall()
        assert len(rows) == 1, (key, rows)
        request_id, owner_epoch, bundle, graph, parent, direct_generation = rows[0]
        assert (request_id, owner_epoch) == (status["request_id"], status["request_owner_epoch"])
        assert (bundle, graph) == (revision["retained_bundle_id"], revision["graph_revision"])
        assert isinstance(parent, bytes) and parent, rows
        assert direct_generation is None, rows
        for table in ("manager_apply_bring_up_plans", "manager_apply_bring_up_child_proofs"):
            assert connection.execute(
                f"SELECT count(*) FROM {table} WHERE apply_id=? AND apply_generation=?", key
            ).fetchone() == (0,), (key, table)


def _observe_authoring_mock_controllers(
    stack: ProcessStack, component_id: str, state: str, label: str
) -> dict[str, Any]:
    """Reuse the strict process fixture's actual CM/JSB/model/command observer."""
    import subprocess

    helper = Path(__file__).resolve().parents[2] / "manyforge_deployment/test/prepare_activation_tool.py"
    assert helper.is_file(), helper
    output = stack.root / f"{label}.{component_id}.ros-observation.json"
    log_path = stack.root / f"{label}.{component_id}.ros-observation.log"
    with log_path.open("wb") as log:
        result = subprocess.run(
            [sys.executable, str(helper), "--observe-manager-ros",
             "--execution-root", str(stack.execution_run_root),
             "--component", component_id, "--state", state,
             "--label", f"{label}.{component_id}", "--output", str(output)],
            stdout=log, stderr=subprocess.STDOUT, timeout=15.0, check=False,
        )
    assert result.returncode == 0, log_path.read_text(encoding="utf-8", errors="replace")
    evidence = json.loads(output.read_text(encoding="utf-8"))
    assert evidence["controllers"][evidence["arm_controller"]]["state"] == state, evidence
    assert evidence["controllers"][evidence["gripper_controller"]]["state"] == state, evidence
    if state == "inactive":
        assert evidence["gripper_submit_refusal"]["ok"] is False, evidence
    return evidence


def _commit_authoring_scene_object(
    stack: ProcessStack, operation: dict[str, Any], label: str
) -> dict[str, Any]:
    with HeadlessHttpClient(stack.base_url, timeout_seconds=20.0) as headless:
        scene = headless.scene()["logical_scene"]
        request = {
            "request_context": {
                "request_id": f"ui6i.authoring-scene.{label}.{uuid.uuid4().hex}",
                "request_owner_epoch": scene["source_owner_epoch"],
                "source_label": "ui6i_authoring_apply_fixture",
                "correlation_id": None,
            },
            "expected_projection_revision": scene["payload"]["access"]["active_projection"],
            "operation": operation,
        }
        if operation["kind"] == "add_object":
            request["expected_admission_generation"] = stack.wait_authoring_admission(
                expected_allowed=True
            )["admission_generation"]
        else:
            assert operation["kind"] == "attach_carried_object", operation
            # Runtime Attach uses the actual owner/projection CAS, without authoring authorization.
        committed = headless.commit_scene_transaction(request)
        assert committed["ok"] is True, committed
        deadline = time.monotonic() + 20.0
        while True:
            snapshot = headless.scene()["logical_scene"]["payload"]["snapshot"]
            if snapshot["revision"] == committed["committed_projection_revision"]["scene_revision"]:
                return snapshot
            assert time.monotonic() < deadline, (committed, snapshot)
            time.sleep(0.01)


def _strict_authoring_mock_apply(stack: ProcessStack, *, single: bool) -> None:
    component_ids = ["lite6_robot"] if single else ["lite6_robot", "ur5e_robot"]
    scenario_path = _write_mixed_variants(stack, ("solo",))["solo"] if single else None
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        initial_scene = headless.scene()["logical_scene"]["payload"]["snapshot"]

    def configured_inactive(status: dict[str, Any], *, initial: bool, preserved: dict[str, Any]) -> None:
        _assert_stored_authoring_apply_without_linked_children(stack, status)
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            snapshots = []
            for component in component_ids:
                read_started_wallclock = time.time()
                try:
                    snapshots.append(headless.cclc_component(component))
                except HeadlessClientError as error:
                    # Preserve the already-read error body and original failure;
                    # no new GET, retry or change to configured-state assertions.
                    encoded = json.dumps(error.response, sort_keys=True, separators=(",", ":")).encode()
                    digest = hashlib.sha256(encoded).hexdigest()
                    evidence_path = stack.root / "readiness-last-snapshot.json"
                    evidence = {
                        "component_id": component,
                        "start_wallclock_seconds": read_started_wallclock,
                        "error_response_sha256": digest,
                        "error_response_bytes": len(encoded),
                        "truncated": False,
                        "error_response": error.response,
                    }
                    cap = 256 * 1024
                    payload = json.dumps(evidence, sort_keys=True).encode()
                    if len(payload) > cap:
                        del evidence["error_response"]
                        evidence["truncated"] = True
                        evidence["error_response_utf8_prefix"] = encoded[:cap // 4].decode("utf-8", errors="replace")
                        payload = json.dumps(evidence, sort_keys=True).encode()
                        while len(payload) > cap:
                            evidence["error_response_utf8_prefix"] = evidence["error_response_utf8_prefix"][:len(evidence["error_response_utf8_prefix"]) // 2]
                            payload = json.dumps(evidence, sort_keys=True).encode()
                    try:
                        evidence_path.write_bytes(payload)
                    except OSError as evidence_error:
                        error.add_note(f"CCLC error evidence write failed: {str(evidence_error)[:256]}")
                    error.add_note(str({
                        "component_id": component,
                        "already_read_response": encoded[:1024].decode("utf-8", errors="replace"),
                        "error_response_sha256": digest,
                        "truncated": evidence["truncated"],
                        "evidence": str(evidence_path),
                    })[:2048])
                    raise
            for current in snapshots:
                assert current["component_lifecycle_state"] == ("declared" if initial else "disabled"), current
                if initial:
                    assert current["lifecycle_target"] is None, current
                    revision = status["candidate_projection_revision"]
                    assert (current["retained_bundle_id"], current["graph_revision"]) == (
                        revision["retained_bundle_id"], revision["graph_revision"]
                    ), current
                    for domain in current["domains"]:
                        active = domain["active_control_configuration"]
                        # Reconciled configuration currency does not enable the component.
                        assert active is not None and set(active) == {
                            "activation_set_id", "active_control_configuration_generation",
                            "active_control_configuration_id", "binding_routes", "control_domain_id",
                            "control_domain_revision", "controller_topology_id", "source_runtime_epoch",
                            "source_runtime_unit_id", "source_sequence", "state", "topology_revision",
                        }, active
                        assert active["state"] == "reconciled", active
                        assert active["active_control_configuration_generation"] > 0, active
                        assert active["activation_set_id"] and active["active_control_configuration_id"] and active["source_runtime_epoch"] and active["source_runtime_unit_id"], active
                        assert active["source_sequence"] >= 0, active
                        assert all(active[key] == domain[key] for key in (
                            "control_domain_id", "control_domain_revision", "controller_topology_id", "topology_revision"
                        )), domain
                        assert active["binding_routes"] and active["binding_routes"] == domain["current_control_bindings"], domain
                        assert all(set(route) == {"control_binding_id", "control_binding_revision", "control_route_id"}
                                   and all(route.values()) for route in active["binding_routes"]), active
                else:
                    assert current["lifecycle_target"]["requested_state"] == "disabled", current
            if initial:
                from contextlib import closing
                import sqlite3

                # Observe this fresh stack's actual plan/adoption, without decoding or using it as authority.
                journal = stack.execution_state_root / "cclc-journal.sqlite3"
                assert journal.is_file(), journal
                with closing(sqlite3.connect(journal.resolve().as_uri() + "?mode=ro", uri=True)) as connection:
                    connection.execute("BEGIN")
                    revision = status["candidate_projection_revision"]
                    key = (status["apply_id"], status["apply_generation"])
                    assert connection.execute(
                        "SELECT apply_id,apply_generation,candidate_bundle_id,candidate_graph_revision,component_bring_up "
                        "FROM cclc_native_apply_plans WHERE singleton=1"
                    ).fetchall() == [(*key, revision["retained_bundle_id"], revision["graph_revision"], None)]
                    routes = {domain["control_domain_id"]:
                              domain["active_control_configuration"]["active_control_configuration_generation"]
                              for current in snapshots for domain in current["domains"]}
                    assert len(routes) == sum(len(current["domains"]) for current in snapshots), snapshots
                    assert dict(connection.execute(
                        "SELECT control_domain_id,active_control_configuration_generation FROM cclc_native_apply_plan_routes"
                    )) == routes
                    assert sorted(connection.execute(
                        "SELECT control_domain_id,active_control_configuration_generation,"
                        "origin_route_selection_transaction_id,origin_route_selection_transaction_generation "
                        "FROM control_domain_active_configurations"
                    )) == sorted((domain, generation, None, None) for domain, generation in routes.items())
                    assert connection.execute(
                        "SELECT publication_generation,public_pointer_digest,candidate_bundle_id,candidate_graph_revision "
                        "FROM cclc_native_apply_adoptions WHERE apply_id=? AND apply_generation=?", key
                    ).fetchall() == [(snapshots[0]["publication_generation"], snapshots[0]["active_public_pointer_digest"],
                                      revision["retained_bundle_id"], revision["graph_revision"])]
                    for table in ("lifecycle_transactions", "route_selection_transactions"):
                        assert connection.execute(f"SELECT count(*) FROM {table}").fetchone() == (0,), table
            targets = headless.execution_targets()["targets"]["components"]
            assert {row["controllable_component_id"] for row in targets} == set(component_ids), targets
            assert len(targets) == len(component_ids), targets
            assert all(row["availability"]["command_eligible"] is False for row in targets), targets
        for component in component_ids:
            _observe_authoring_mock_controllers(stack, component, "inactive", "first-load" if initial else "preserved-reload")
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            assert headless.scene()["logical_scene"]["payload"]["snapshot"] == preserved

    _load_scenario(
        stack, GROUPED_SCENARIO, scenario_path=scenario_path,
        on_configuration_applied=lambda status: configured_inactive(status, initial=True, preserved=initial_scene),
    )
    for component in component_ids:
        _observe_authoring_mock_controllers(stack, component, "active", "first-activate")
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        (component,) = [row for row in headless.execution_targets()["targets"]["components"]
                        if row["controllable_component_id"] == "lite6_robot"]
        (motion,) = [row for row in component["targets"] if row["presentation"] == "motion"]

    for resource in ("authoring_world_marker", "authoring_carried_marker"):
        _commit_authoring_scene_object(stack, {
            "kind": "add_object", "scene_resource_id": resource,
            "shape": {"type": "box", "box_dimensions_m": {"x_m": 0.02, "y_m": 0.02, "z_m": 0.02}},
            "pose": {"position_m": {"x_m": 3.0 if resource == "authoring_world_marker" else 3.2,
                                    "y_m": 0.0, "z_m": 0.0},
                     "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}},
            "frame_ref": {"kind": "cell_frame", "cell_id": stack.cell_id},
        }, resource)
    preserved = _commit_authoring_scene_object(stack, {
        "kind": "attach_carried_object", "carried_object_attachment_id": "authoring_marker_attachment",
        "scene_resource_id": "authoring_carried_marker", "controllable_component_id": "lite6_robot",
        "command_target_id": motion["command_target_id"],
        "frame_role_id": motion["command"]["base_frame_role_id"],
        # Both real Scenario inputs have no structural component attachments;
        # the installed arm's base path crosses none. No touch exemptions.
        "touch_body_roles": [], "expected_component_attachment_revisions": [],
    }, "attach")
    assert {row["scene_resource_id"] for row in preserved["objects"]} == {
        "authoring_world_marker", "authoring_carried_marker"
    }, preserved
    assert len(preserved["carried_object_attachments"]) == 1, preserved
    assert preserved["carried_object_attachments"][0]["scene_resource_id"] == "authoring_carried_marker", preserved
    for component in component_ids:
        _disable_for_reload(stack, component)
    _load_scenario(
        stack, GROUPED_SCENARIO, scenario_path=scenario_path,
        on_configuration_applied=lambda status: configured_inactive(status, initial=False, preserved=preserved),
    )
    for component in component_ids:
        _observe_authoring_mock_controllers(stack, component, "active", "reload-activate")
    with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
        assert headless.scene()["logical_scene"]["payload"]["snapshot"] == preserved


def test_authoring_single_mock_apply_preserves_scene_and_stays_inactive_until_activate() -> None:
    with _dual_robot_stack() as stack:
        _strict_authoring_mock_apply(stack, single=True)


def test_authoring_two_mock_apply_preserves_scene_and_stays_inactive_until_activate() -> None:
    with _dual_robot_stack() as stack:
        _strict_authoring_mock_apply(stack, single=False)

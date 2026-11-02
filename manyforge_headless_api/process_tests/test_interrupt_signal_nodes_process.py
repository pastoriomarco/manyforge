"""Signal-move interrupt nodes over the real wire (SX-2282).

One Program Run on the ROS Mock lane drives ``motion.move_until_signal``
through the real Program Runner process and its production Kernel port
against the real execution host, Manager and Headless API. No fake transport:
the trip, its Stop terminal record and the node outcome are each read back
from the processes that own them.

Fixture: ``examples/mock_lite6_signal_moves.scenario`` (shared Deployment
``mock_lite6_signal_moves``), narrowed to its two move_until_signal nodes
and their ``node_outcome`` checks, and to the Lite6 Mock and Mock station_io
components (the ROS ``plc_io`` peer and the global rule are not part of this
check). The nodes author their Mock scripts (contract 133): ``search_trip``
sets ``probe_contact`` false then true after 1000 ms; ``search_no_trip`` sets
``edge_sensor`` false with no later change.
"""

from __future__ import annotations

from contextlib import contextmanager
from copy import deepcopy
import importlib.util
import json
import os
from pathlib import Path
import sys
from tempfile import TemporaryDirectory
import time
from typing import Any, Iterator

import pytest

from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.process_tests.test_process_stack import (
    ProcessStack,
    _read_only_tree,
    keep_failure_evidence,
)
from manyforge_headless_api.process_tests.test_ui6i_grouped_execution import (
    _activate_loaded_components,
    _component_states,
    _disable_for_reload,
    _native_events,
    _start_motion,
    _start_run,
    _wait_components_reached_requested_target,
    _wait_kernel_attention_reconciled,
    _wait_run_state,
)


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
TRIAL_SCENARIO = "mock_lite6_signal_moves"
# The trial's base project: its Lite6 Mock is the trial's unchanged robot.
BOOTSTRAP_SCENARIO = "mock_lite6_pick_and_place"
SCENARIO_ID = "mock_lite6_signal_moves_process"
# The dev workbench's ROS cell identity: the trial's proven bootstrap path.
CELL_ID = "c1_build_authority_cell"
ADMISSION_FILENAME = f"canonical-empty-admission.{CELL_ID}.json"
KEPT_NODES = ("start_rest", "search_trip", "trip_outcome", "search_no_trip", "no_trip_outcome")
REQUIRED_ENVIRONMENT = (
    "MANYFORGE_MANAGER_DAEMON",
    "MANYFORGE_CANONICAL_EMPTY_PACKAGE_ROOT",
    "MANYFORGE_ROS2_CONTROL_EXECUTION_HOST",
    "MANYFORGE_ROBOPLAN_CANDIDATE_WORKER",
    "MANYFORGE_ACTIVATION_PREPARER",
    "MANYFORGE_EXAMPLE_SCENARIO_ROOT",
)


def _skip_reason() -> str | None:
    if importlib.util.find_spec("manyforge_headless_py") is None:
        return "needs the built manyforge_headless_py module on PYTHONPATH (combined native build)"
    missing = [name for name in REQUIRED_ENVIRONMENT
               if not os.environ.get(name) or not Path(os.environ[name]).exists()]
    if missing:
        return "needs the combined native build binaries/fixtures: " + ", ".join(missing)
    if not os.environ.get("AMENT_PREFIX_PATH"):
        return "needs the sourced ROS bringup overlay (Mock controllers); run in the ROS container"
    return None


def _write_signal_scenario(directory: Path) -> Path:
    """The trial (e) project narrowed to its two signal moves, written by the
    Composer Scenario save path (digests are the ones the GUI would write)."""

    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    examples = Path(os.environ["MANYFORGE_EXAMPLE_SCENARIO_ROOT"])
    source = examples / f"{TRIAL_SCENARIO}.scenario"
    trial, _ = load_scenario_artifact(str(source), allowed_roots=[str(examples)])
    program = deepcopy(trial["program_draft"])
    (root,) = program["forest"]
    root["children"] = [row for row in root["children"] if row["instance_id"] in KEPT_NODES]
    assert [row["instance_id"] for row in root["children"]] == list(KEPT_NODES), root
    # probe_no_trip was the contact policy's only user.
    program.pop("contact_policies", None)
    program["blackboard"] = {"keys": [], "namespaces": []}
    deployment = deepcopy(trial["deployment_draft"])
    deployment["components"] = [
        row for row in deployment["components"] if row["component_id"] != "plc_io"
    ]
    deployment["policies"]["interrupt_rules"] = []
    target = directory / f"{SCENARIO_ID}.scenario"
    scenario = build_scenario_document(
        scenario_id=SCENARIO_ID,
        name=SCENARIO_ID,
        description="mock_lite6_signal_moves narrowed to move_until_signal trip/no-trip.",
        program_draft=program,
        deployment_draft=deployment,
        program_role_assignment=trial["program_role_assignment"],
        manyforge_version=(REPOSITORY_ROOT / "VERSION").read_text(encoding="utf-8").strip(),
        last_known_status="unknown",
    )
    save_scenario_artifact(
        scenario, str(target), allowed_roots=[str(directory), str(examples)], overwrite=False,
        source_project_path=str(source),
    )
    return target


def _prepare_bootstrap(activation_root: Path) -> None:
    """The cell's empty predecessor, prepared exactly as the dev workbench's
    ROS bootstrap does (production ActivationPreparer). The workbench admits
    only ros2_control components there, so it is prepared from the trial's
    base (the same Lite6 Mock); the signal scenario is then Loaded."""

    from manyforge_behavior.canonical_artifact import canonical_json_bytes, raw_sha256_digest
    from manyforge_headless_api.activation_preparer import ActivationPreparer

    spec = importlib.util.spec_from_file_location(
        "manyforge_dev_workbench", REPOSITORY_ROOT / "scripts/dev-workbench.py"
    )
    assert spec is not None and spec.loader is not None
    workbench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(workbench)
    examples = Path(os.environ["MANYFORGE_EXAMPLE_SCENARIO_ROOT"])
    captured, _ = workbench._capture_ros_bootstrap_project(
        str(examples / f"{BOOTSTRAP_SCENARIO}.scenario"), allowed_root=examples
    )
    activation_root.mkdir()
    ActivationPreparer(
        executable=Path(os.environ["MANYFORGE_ACTIVATION_PREPARER"]),
        capability_root=REPOSITORY_ROOT,
        activation_root=activation_root,
        runtime_cell_id=CELL_ID,
        timeout_seconds=180,
        publish_canonical_empty=True,
    ).prepare(
        captured_input_digest=raw_sha256_digest(canonical_json_bytes(captured)), **captured
    )
    assert (activation_root / ADMISSION_FILENAME).is_file()


@contextmanager
def _signal_stack(scenario_writer=_write_signal_scenario) -> Iterator[tuple[ProcessStack, Path]]:
    # Short root: POSIX socket paths are bounded.
    with TemporaryDirectory(prefix="mf-sig-") as temporary_root:
        root = Path(temporary_root)
        scenarios = root / "scenarios"
        scenarios.mkdir()
        project = scenario_writer(scenarios)
        bootstrap = root / "bootstrap"
        _prepare_bootstrap(bootstrap)
        stack_root = root / "stack"
        stack_root.mkdir()
        with _read_only_tree(bootstrap):
            stack = ProcessStack(
                stack_root,
                package_root=bootstrap,
                cell_id=CELL_ID,
                composer_fs_roots=(scenarios, stack_root),
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
                yield stack, project
            except BaseException:
                stack.log.flush()
                keep_failure_evidence(root)
                log_path = stack_root / "process-stack.log"
                if log_path.is_file():
                    print(
                        "\n--- signal-node process log ---\n"
                        + log_path.read_text(encoding="utf-8", errors="replace"),
                        file=sys.stderr,
                    )
                raise
            finally:
                stack.close()


def _load(
    stack: ProcessStack,
    project: Path,
    *,
    scenario_id: str = SCENARIO_ID,
    activate_components: bool = True,
) -> dict[str, Any]:
    """One ordinary Composer Load and Apply, optionally followed by activation."""

    with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer:
        loaded = composer.request("POST", "/api/scenario/load", body={"path": str(project)})
        # Load waits for the whole Apply (CONTRACTS §11); components end prepared.
        assert loaded["outcome"] == "succeeded", loaded
        if activate_components:
            component_ids = _activate_loaded_components(stack)
            # The Lite 6 machine owner `lite6` is a component too (CONTRACTS §16);
            # its inactive I/O members never reach the compiled cell.
            assert set(component_ids) == {
                "lite6", "lite6_robot", "lite_gripper", "station_io"
            }, component_ids
            _wait_components_reached_requested_target(stack, component_ids)
        stack.wait_authoring_admission(expected_allowed=True)
        applied = composer.program_applied()
        assert applied["scenario_id"] == scenario_id, applied
        return applied


def _write_runtime_configuration_candidate(directory: Path, project: Path) -> Path:
    """Write a new file for the same Scenario with one changed Lite6 setting."""

    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    source, _ = load_scenario_artifact(str(project), allowed_roots=[str(directory)])
    deployment = deepcopy(source["deployment_draft"])
    lite6 = next(
        row for row in deployment["components"]
        if row["component_id"] == "lite6_robot"
    )
    runtime = lite6["configuration"]["runtime_configuration"]
    runtime["initial_joint_positions_rad"][0] += 0.1
    # Same Scenario identity at a second path: folder, id and name are one name.
    target = directory / "runtime-apply" / f"{SCENARIO_ID}.scenario"
    target.parent.mkdir()
    candidate = build_scenario_document(
        scenario_id=SCENARIO_ID,
        name=SCENARIO_ID,
        description="Changes Lite6 runtime configuration; preserves the Scenario identity.",
        program_draft=source["program_draft"],
        deployment_draft=deployment,
        program_role_assignment=source["program_role_assignment"],
        manyforge_version=(REPOSITORY_ROOT / "VERSION").read_text(
            encoding="utf-8"
        ).strip(),
        last_known_status="unknown",
    )
    save_scenario_artifact(
        candidate,
        str(target),
        allowed_roots=[str(directory)],
        overwrite=False,
        source_project_path=str(project),
    )
    return target


def _node_operations(stack: ProcessStack, program_run_id: str) -> dict[str, list[str]]:
    """Accepted native operation ids per Program node instance of this Run."""

    nodes: dict[str, list[str]] = {}
    for event in _native_events(stack):
        if (
            event["record_kind"] != "operation_accepted"
            or event["payload"]["intent"]["program_run_id"] != program_run_id
        ):
            continue
        (participant,) = event["payload"]["intent"]["participants"]
        node = participant["action_span"]["node_instance_id"]
        nodes.setdefault(node, []).append(event["operation_id"])
    return nodes


def _rule_row(operation: dict[str, Any]) -> tuple[str, dict[str, Any]]:
    """The operation's single participant and its single node-rule result."""

    (participant,) = operation["participants"]
    (row,) = participant["io_rule_outcomes"]
    return participant["participant_id"], row


def _stop_terminal(stack: ProcessStack, stop_id: str, stop_generation: int) -> dict[str, Any]:
    """The Stop's detailed terminal record from the native events route
    (the same `stop_terminal_projected` row the Runner's events reader keeps)."""

    deadline = time.monotonic() + 10.0
    while True:
        rows = [
            event["payload"]
            for event in _native_events(stack)
            if event["record_kind"] == "stop_terminal_projected"
            and event["payload"]["stop_id"] == stop_id
            and event["payload"]["stop_generation"] == stop_generation
        ]
        if rows:
            (terminal,) = rows
            return terminal
        assert time.monotonic() < deadline, f"no stop_terminal_projected for {stop_id}"
        time.sleep(0.02)


def _stop_chain(native: ExecutionCommandClient, participant: dict[str, Any]) -> list[str]:
    """The robot's qualified Stop stage order of the exact Targets motion row."""

    inventory = native.targets()["targets"]
    rows = [
        row["command"]
        for component in inventory["components"]
        if component["controllable_component_id"] == participant["controllable_component_id"]
        for row in component["targets"]
        if row["command_target_id"] == participant["command_target_id"]
        and row["operation_capability_id"] == participant["capability_id"]
    ]
    (command,) = rows
    assert command.get("stop_response_error") is None, command
    return [stage["stage_id"] for stage in command["stop_response"]["stages"]]


@pytest.mark.skipif(_skip_reason() is not None, reason=str(_skip_reason()))
def test_move_until_signal_trips_and_completes_over_the_real_wire() -> None:
    from manyforge_behavior.interrupt_outcome import contained_stages_from_stop_terminal

    with _signal_stack() as (stack, project):
        applied = _load(stack, project)
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            started = _start_run(
                stack,
                scenario_id=SCENARIO_ID,
                source_document_digest=applied["source_document_digest"],
                applied=applied,
            )
            program_run_id = started["capture"]["program_run_id"]
            _wait_kernel_attention_reconciled(stack)
            _start_motion(client, started, request_id="signal.motion-start")
            terminal_run = _wait_run_state(
                client, program_run_id, {"succeeded", "failed", "stopped"}, timeout_seconds=90.0
            )
            controls = client.program_runtime_controls(program_run_id)
        # The authored logic.check_value nodes read node_outcome.search_trip ==
        # signal_trip and node_outcome.search_no_trip == no_signal_trip from
        # the Run's blackboard; the sequence succeeds only if both match.
        assert terminal_run["state"] == "succeeded", terminal_run

        # The Runner's adapter mapped each terminal operation (no fake port).
        results = {
            row["node_instance_id"]: row
            for row in controls["runtime_controls"]["audit"]
            if row["kind"] == "program_node_interrupt_result"
        }
        assert set(results) == {"search_trip", "search_no_trip"}, results
        assert results["search_trip"]["outcome"] == "signal_trip", results
        assert results["search_trip"]["result"] == "success", results  # on_signal: success
        assert results["search_no_trip"]["outcome"] == "no_signal_trip", results
        assert results["search_no_trip"]["result"] == "success", results  # default on_no_signal
        assert not any(row["effect_uncertain"] for row in results.values()), results

        # The Kernel's typed rule result for the very operations the nodes ran.
        native = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=2.0)
        operations = _node_operations(stack, program_run_id)
        assert len(operations.get("search_trip", ())) == 1, operations  # no automatic retry
        assert len(operations.get("search_no_trip", ())) == 1, operations
        (trip_operation_id,) = operations["search_trip"]
        (no_trip_operation_id,) = operations["search_no_trip"]

        no_trip = native.read(no_trip_operation_id)["operation"]
        _, no_trip_row = _rule_row(no_trip)
        assert no_trip_row["terminal_cause"] == "no_signal_trip", no_trip_row
        assert no_trip_row["evidence"].get("trip") is None, no_trip_row

        trip = native.read(trip_operation_id)["operation"]
        participant_id, trip_row = _rule_row(trip)
        assert trip_row["terminal_cause"] == "signal_trip", trip_row
        assert trip_row["evidence"]["trip"] is not None, trip_row
        assert trip_row["stop_result"] in {"succeeded_nominal", "succeeded_escalated"}, trip_row
        assert isinstance(trip_row["stop_id"], str) and trip_row["stop_id"], trip_row

        # The Stop terminal record of exactly that Stop and certificate proves
        # the stage that contained the trip, within the robot's Stop chain.
        stop_terminal = _stop_terminal(stack, trip_row["stop_id"], trip_row["stop_generation"])
        assert stop_terminal["state"] == "succeeded", stop_terminal
        assert stop_terminal["details_truncated"] is False, stop_terminal
        contained = contained_stages_from_stop_terminal(
            trip_row, stop_terminal, operation_id=trip_operation_id
        )
        assert contained is not None and set(contained) == {participant_id}, (
            contained, stop_terminal)
        chain = _stop_chain(native, trip["participants"][0])
        assert contained[participant_id] in chain, (contained, chain)
        print(f"search_trip: {trip_row['stop_result']}, contained by stage "
              f"{contained[participant_id]!r} of {chain}")


IO_SCENARIO_ID = "mock_lite6_io_nodes_process"


def _write_mock_io_scenario(directory: Path) -> Path:
    """A D-derived Mock-only fixture, explicitly excluding the external PLC.

    The authored D nodes/parameters are retained. Two ordinary logic checks
    make the initial read and Fallback's stored result observable assertions.
    This does not qualify full D, external ROS PLC or manual ACK success.
    """
    from manyforge_composer.backend.scenario_artifacts import (
        build_scenario_document,
        load_scenario_artifact,
        save_scenario_artifact,
    )

    examples = Path(os.environ["MANYFORGE_EXAMPLE_SCENARIO_ROOT"])
    source = examples / "mock_lite6_io_nodes.scenario"
    trial, _ = load_scenario_artifact(str(source), allowed_roots=[str(examples)])
    program = deepcopy(trial["program_draft"])
    (root,) = program["forest"]
    authored = {row["instance_id"]: deepcopy(row) for row in root["children"]}
    assert authored["check_part"]["parameters"].get("mock_simulation") is None
    assert authored["wait_part"]["parameters"]["timeout_s"] == 2.0
    ack_wait = authored["ack_or_timeout"]["children"][0]
    assert ack_wait["instance_id"] == "wait_operator_ack"
    assert ack_wait["parameters"]["timeout_s"] == 5.0
    assert ack_wait["parameters"]["mock_simulation"] == [{
        "signal": {"target_role": "station", "resource_role": "operator_ack"},
        "start_value": False,
    }]
    checks = {
        "read_part_initial": ("assert_initial_false", "trial.part_initial", False),
        "ack_or_timeout": ("assert_timeout_recorded", "trial.ack_timed_out", True),
    }
    root["children"] = []
    for child in authored.values():
        if child["instance_id"] in {"plc_start_on", "wait_plc_ready"}:
            continue
        root["children"].append(child)
        if child["instance_id"] in checks:
            name, key, value = checks[child["instance_id"]]
            root["children"].append({
                "instance_id": name, "node_id": "logic.check_value", "children": [],
                "parameters": {"key": key, "value": value},
            })
    program["component_roles"].pop("plc")
    program["target_roles"].pop("plc")
    assignment = deepcopy(trial["program_role_assignment"])
    assignment["component_roles"] = [
        row for row in assignment["component_roles"] if row["component_role_id"] != "plc"
    ]
    assignment["target_roles"] = [
        row for row in assignment["target_roles"] if row["target_role_id"] != "plc"
    ]
    deployment = deepcopy(trial["deployment_draft"])
    deployment["components"] = [
        row for row in deployment["components"] if row["component_id"] != "plc_io"
    ]
    deployment["policies"]["interrupt_rules"] = []
    target = directory / f"{IO_SCENARIO_ID}.scenario"
    scenario = build_scenario_document(
        scenario_id=IO_SCENARIO_ID,
        name=IO_SCENARIO_ID,
        description="D-derived Mock script, retained input and authored ACK timeout; excludes external PLC.",
        program_draft=program, deployment_draft=deployment,
        program_role_assignment=assignment,
        manyforge_version=(REPOSITORY_ROOT / "VERSION").read_text(encoding="utf-8").strip(),
        last_known_status="unknown",
    )
    save_scenario_artifact(
        scenario, str(target), allowed_roots=[str(directory), str(examples)],
        overwrite=False, source_project_path=str(source),
    )
    return target


@pytest.mark.skipif(_skip_reason() is not None, reason=str(_skip_reason()))
def test_trial_d_mock_script_retains_value_and_operator_ack_times_out_over_real_wire() -> None:
    """One real process Run; no PLC emulation or manual-input mutation."""
    with _signal_stack(_write_mock_io_scenario) as (stack, project):
        applied = _load(stack, project, scenario_id=IO_SCENARIO_ID)
        transitions: list[dict[str, Any]] = []
        last_statuses: dict[str, str] = {}
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as client:
            began = time.monotonic()
            started = _start_run(
                stack, scenario_id=IO_SCENARIO_ID,
                source_document_digest=applied["source_document_digest"], applied=applied,
            )
            run_id = started["capture"]["program_run_id"]
            _wait_kernel_attention_reconciled(stack)
            _start_motion(client, started, request_id="trial-d-mock.motion-start")
            deadline = time.monotonic() + 30.0
            terminal: dict[str, Any] = {}
            while time.monotonic() < deadline:
                observed = client.program_run(run_id)
                for node, status in observed["node_statuses"].items():
                    if last_statuses.get(node) != status:
                        transitions.append({
                            "node": node, "status": status,
                            "observed_elapsed_seconds": time.monotonic() - began,
                            "observed_monotonic_ns": time.monotonic_ns(),
                        })
                        last_statuses[node] = status
                if observed["state"] in {"succeeded", "failed", "stopped"}:
                    terminal = observed
                    break
                time.sleep(0.01)
            controls = client.program_runtime_controls(run_id)
        # Keep observed timing, not a new tight wall-clock timing requirement.
        evidence = {
            "scope": "D-derived Mock-only; no external PLC/manual ACK-success qualification",
            "actual_terminal_run": terminal, "actual_controls": controls,
            "actual_observed_transitions": transitions,
            "authored_wait_part_change_after_ms": 250,
            "authored_operator_ack_timeout_s": 5.0,
            "manual_input_commands_sent": 0,
        }
        evidence_root = os.environ.get("MANYFORGE_PROCESS_TEST_EVIDENCE_ROOT")
        if evidence_root:
            directory = Path(evidence_root)
            directory.mkdir(parents=True, exist_ok=True)
            try:
                (directory / "trial-d-mock-io-result.json").write_text(
                    json.dumps(evidence, indent=2, sort_keys=True) + "\n", encoding="utf-8"
                )
            except Exception as error:
                print(f"Trial D evidence write failed: {error!r}", file=sys.stderr)
        assert terminal.get("state") == "succeeded", evidence
        statuses = terminal["node_statuses"]
        for node in ("read_part_initial", "assert_initial_false", "wait_part", "check_part",
                     "clamp_on", "ack_or_timeout", "record_ack_timeout", "assert_timeout_recorded",
                     "lamp_on", "clamp_off", "trial_d"):
            assert statuses[node] == "success", (node, evidence)
        assert statuses["wait_operator_ack"] == "failure", evidence
        assert not {"plc_start_on", "wait_plc_ready"}.intersection(statuses), evidence
        assert terminal["active_operation_count"] == 0, terminal
        assert terminal["operation_failures"] == [], terminal
        scripted = [row for row in controls["runtime_controls"]["audit"]
                    if row["kind"] == "program_node_mock_simulation"]
        assert [row["node_instance_id"] for row in scripted] == [
            "read_part_initial", "wait_part", "wait_operator_ack"
        ], scripted
        assert all(row["applied"] and not row["inactive"] for row in scripted), scripted
        # RPC/poll timing may miss an intermediate Running state. The actual
        # child timeout reason is captured separately by the qualification
        # wrapper; polling is observation, not proof of a node's execution.
        # Preserve actual durable artifacts at the assertion boundary even on success.
        stack.log.flush()
        keep_failure_evidence(stack.root.parent)


@pytest.mark.skipif(_skip_reason() is not None, reason=str(_skip_reason()))
def test_inactive_runtime_apply_rejoins_retained_io_component() -> None:
    """Same-Scenario Load replaces disabled Lite6 and retains the exact I/O owner."""

    with _signal_stack() as (stack, project):
        before_apply = _load(stack, project)
        with (
            HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless,
            HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer,
        ):
            io_before = _component_states(headless)["station_io"]
            assert io_before["lifecycle"] == "enabled", io_before
            selected_before = composer.request("GET", "/api/deployment/workspace")["managerApplied"]
            assert selected_before["scenarioId"] == SCENARIO_ID, selected_before
        _disable_for_reload(stack, "lite6_robot")
        candidate = _write_runtime_configuration_candidate(project.parent, project)
        _load(stack, candidate, activate_components=False)
        with HeadlessHttpClient(stack.base_url, timeout_seconds=10.0) as headless:
            motion_after = _component_states(headless)["lite6_robot"]
            io_after = _component_states(headless)["station_io"]
        with HeadlessHttpClient(stack.composer_url, timeout_seconds=30.0) as composer:
            selected_after = composer.request("GET", "/api/deployment/workspace")["managerApplied"]
        assert before_apply["scenario_id"] == SCENARIO_ID, before_apply
        assert selected_after["scenarioId"] == SCENARIO_ID, selected_after
        assert selected_after["deploymentRevision"] != selected_before["deploymentRevision"]
        assert motion_after["lifecycle"] == "prepared", motion_after
        assert io_after["lifecycle"] == "enabled", io_after
        assert io_after.get("last_action") == io_before.get("last_action")

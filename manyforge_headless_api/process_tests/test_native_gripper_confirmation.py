"""Short managed qualification of the public gripper-confirmation contract."""

import importlib.util
import json
from pathlib import Path
import shutil
import time

import pytest

from manyforge_headless_api.client import HeadlessHttpClient
from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import (
    GripperConfirmationRequest,
    TargetObservationQuery,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.process_tests.test_process_stack import (
    _assistant_stack,
    _required_path,
)


def test_qualified_operator_confirmation_uses_current_native_command(tmp_path: Path):
    fixture = tmp_path / "fixture"
    shutil.copytree(_required_path("MANYFORGE_GRIPPER_CONFIRMATION_FIXTURE"), fixture)
    spec = importlib.util.spec_from_file_location(
        "gripper_workbench", Path(__file__).parents[2] / "scripts/dev-workbench.py"
    )
    workbench = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(workbench)
    with _assistant_stack(
        tmp_path,
        package_root=fixture,
        metadata_environment="MANYFORGE_GRIPPER_CONFIRMATION_METADATA",
        package_environment="MANYFORGE_GRIPPER_CONFIRMATION_FIXTURE",
        startup_scenario="program_apply_fixture",
    ) as stack, HeadlessHttpClient(stack.base_url, timeout_seconds=2.0) as headless:
        processes = [
            ("Manager", stack.manager),
            ("Runner", stack.program_runner),
            ("Headless", stack.api),
            ("Composer", stack.composer),
        ]
        workbench._converge_control_management(
            headless,
            ["lite6_arm"],
            scenario_id="program_apply_fixture",
            processes=processes,
        )
        client = ExecutionCommandClient(stack.execution_run_root, timeout_seconds=2.0)
        owner = client.identity()["identity"]["owner_epoch"]
        query = TargetObservationQuery.model_validate(
            {
                "schema_version": "manyforge.target_observation_query.v1",
                "owner_epoch": owner,
                "program_binding_id": stack.fixture_metadata["program_binding_id"],
                "target_role_id": "gripper",
                "kind": "gripper_confirmation",
            }
        )
        with pytest.raises(HeadlessApiError) as absent:
            client.query_target(query)
        assert absent.value.code == "gripper_command_unavailable"

        program = json.loads(stack.program_path.read_text())
        applied = headless.program_applied()
        admission = stack.wait_authoring_admission(expected_allowed=True)
        started = headless.program_run_start(
            {
                "request_id": "confirmation.run.1",
                "request_owner_epoch": "qualification.client",
                "source_label": "native_confirmation_qualification",
                "scenario_id": "program_apply_fixture",
                "source_document_digest": stack.fixture_metadata["source_document_digest"],
                "expected_applied_record_digest": applied["source"]["record_digest"],
                "draft_program_revision": program["program_revision"],
                "expected_admission_generation": admission["admission_generation"],
            }
        )
        run_id = started["capture"]["program_run_id"]
        def current_motion_controls():
            cycle = headless.program_cycle_status()
            controls = cycle.get("runtime_controls")
            selections = (
                None if controls is None else controls.get("execution_selections")
            )
            if (
                cycle.get("program_run_id") == run_id
                and isinstance(selections, dict)
            ):
                return cycle, selections
            return None

        cycle, selections = workbench._wait_until(
            "current motion controls",
            current_motion_controls,
            processes,
            timeout_seconds=5.0,
        )
        motion_started = headless.program_run_motion_start(
            run_id,
            {
                "request_id": "confirmation.motion-start.1",
                "request_owner_epoch": "qualification.client",
                "source_label": "native_confirmation_qualification",
                "correlation_id": "confirmation.motion-start.1",
                "program_run_id": run_id,
                "expected_owner_epoch": cycle["program_runner_owner_epoch"],
                "expected_run_revision": cycle["run"]["run_revision"],
                "expected_selection_revision": selections["selection_revision"],
                "reason": "initial operator motion Run",
                "group_ids": ["all"],
            },
        )
        assert motion_started["state"] == "running"

        def waiting():
            run = headless.program_run(run_id)
            nodes = run.get("node_statuses", {})
            assert run["state"] not in {"failed", "interrupted"}, run
            return run if nodes.get("confirm_grasp") == "running" else None

        run = workbench._wait_until(
            "Program waits for actual confirmation",
            waiting,
            processes,
            timeout_seconds=5.0,
        )
        assert run["node_statuses"]["close_gripper"] == "success"
        current = client.query_target(query)["target_observation"]["value"]
        assert current["state"] == "completed"
        assert current["grasp_confirmed"] is None
        assert current["evidence"] is None  # command completion is not grasp proof
        request = GripperConfirmationRequest.model_validate(
            {
                "schema_version": "manyforge.gripper_confirmation_request.v1",
                "program_binding_id": query.program_binding_id,
                "target_role_id": query.target_role_id,
                "command_id": current["command_id"],
                "generation": current["generation"],
                "expected_status_revision": current["revision"],
                "confirmed": True,
                "request_context": {
                    "request_id": "operator.press.1",
                    "request_owner_epoch": owner,
                    "source_label": "native-qualification.operator",
                    "correlation_id": None,
                },
            }
        )
        now = time.monotonic_ns()
        with pytest.raises(HeadlessApiError) as stale:
            client.confirm_grasp(
                request,
                trusted_principal_id=None,
                origin_epoch="test-session.1",
                received_monotonic_ns=now - 10_000_000,
                valid_until_monotonic_ns=now - 1,
            )
        assert stale.value.code == "control_freshness_expired"
        assert client.query_target(query)["target_observation"]["value"] == current

        projection = headless.control_projection()
        response = headless.submit_control_command(
            {
                "command_envelope": {
                    "schema_version": "manyforge.execution_command.v1",
                    "command_kind": "gripper_confirm",
                    "correlation_id": "operator.press.1",
                    "source_label": "native-qualification.operator",
                    "freshness": projection["freshness"],
                    "expected_semantic_owner_epoch": projection["semantic_owner_epoch"],
                    "expected_execution_control_revision": projection["execution_control_revision"],
                },
                "payload": {
                    **request.model_dump(mode="json"),
                    "expected_execution_owner_epoch": owner,
                },
            }
        )
        assert response["receipt"]["disposition"] == "received", response
        result = response["result"]
        assert result["history_recorded"] is True
        evidence = result["confirmation"]
        assert evidence["source_kind"] == "operator"
        assert evidence["attribution"] == "native-qualification.operator"
        assert evidence["source_id"] == current["source_id"]
        assert int(evidence["expires_at_ns"]) - int(evidence["observed_at_ns"]) == 30_000_000_000
        confirmed = client.query_target(query)["target_observation"]["value"]
        assert confirmed["grasp_confirmed"] is True
        assert confirmed["evidence"] == evidence
        duplicate_time = time.monotonic_ns()
        assert (
            client.confirm_grasp(
                request,
                trusted_principal_id=None,
                origin_epoch=projection["freshness"]["receiver_incarnation"],
                received_monotonic_ns=duplicate_time,
                valid_until_monotonic_ns=duplicate_time + 500_000_000,
            )["confirmation"]
            == evidence
        )

        def completed():
            value = headless.program_run(run_id)
            return value if value["state"] in {"succeeded", "failed"} else None

        finished = workbench._wait_until(
            "confirmation releases existing Program wait",
            completed,
            processes,
            timeout_seconds=3.0,
        )
        assert finished["state"] == "succeeded", finished
        assert finished["node_statuses"] == {
            "root": "success",
            "close_gripper": "success",
            "confirm_grasp": "success",
        }
        assert client.query_target(query)["target_observation"]["value"] == confirmed
        read_ms = []
        for _ in range(20):
            begin = time.perf_counter_ns()
            assert client.query_target(query)["target_observation"]["value"] == confirmed
            read_ms.append((time.perf_counter_ns() - begin) / 1_000_000)
        print(
            json.dumps(
                {
                    "native_confirmation_read_ms": {
                        "samples": len(read_ms),
                        "mean": sum(read_ms) / len(read_ms),
                        "p95": sorted(read_ms)[18],
                        "max": max(read_ms),
                    }
                }
            )
        )

        def traced():
            records = headless.history(params=(("limit", 32), ("components", "manyforge.gripper")))[
                "events"
            ]
            return records if len(records) >= 2 else None

        records = workbench._wait_until(
            "bounded durable operator trace", traced, processes, timeout_seconds=2.0
        )
        assert {row["event"]["event_type"] for row in records} == {
            "gripper_confirmation_attempted",
            "gripper_confirmation_accepted",
        }
        accepted = next(
            row for row in records if row["event"]["event_type"] == "gripper_confirmation_accepted"
        )
        detail = headless.history_detail(accepted["sequence"])
        assert detail["event"]["payload"]["details"]["evidence"] == evidence

from __future__ import annotations

from collections.abc import Callable
from copy import deepcopy
import json
import os
from pathlib import Path
import socket
import threading
from typing import Any

from fastapi.testclient import TestClient
import pytest
import rfc8785

from manyforge_headless_api.app import create_app
from manyforge_headless_api.cclc import (
    CCLC_OPEN_RESPONSE_VERSION,
    CCLC_SOCKET_FILE,
    CCLCOpenRequestClient,
    _validate_lifecycle_target_facts,
)
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService


def _response(command: str, result: dict[str, Any] | None) -> dict[str, Any]:
    return {
        "api_version": CCLC_OPEN_RESPONSE_VERSION,
        "command": command,
        "error": None,
        "ok": True,
        "result": result,
    }


def _serve_once(
    root: Path,
    handler: Callable[[dict[str, Any]], bytes],
) -> threading.Thread:
    listener = socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET)
    listener.bind(str(root / CCLC_SOCKET_FILE))
    os.chmod(root / CCLC_SOCKET_FILE, 0o600)
    listener.listen(1)

    def run() -> None:
        try:
            connection, _address = listener.accept()
            with connection:
                request = connection.recv(4 * 1024 * 1024)
                connection.send(handler(json.loads(request)))
        finally:
            listener.close()

    thread = threading.Thread(target=run)
    thread.start()
    return thread


def test_client_forwards_one_canonical_addressed_request(tmp_path: Path) -> None:
    observed: list[dict[str, Any]] = []

    def handler(request: dict[str, Any]) -> bytes:
        observed.append(request)
        return rfc8785.dumps(
            _response(
                "component_snapshot",
                {
                    "controllable_component_id": "component.arm",
                    "component_lifecycle_generation": 4,
                },
            )
        )

    thread = _serve_once(tmp_path, handler)
    result = CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert result["component_lifecycle_generation"] == 4
    assert observed == [
        {
            "api_version": "manyforge.cclc_open_request.v1",
            "command": "component_snapshot",
            "payload": {"controllable_component_id": "component.arm"},
        }
    ]


def test_client_reads_the_cclc_owner_identity(tmp_path: Path) -> None:
    observed: list[dict[str, Any]] = []

    def handler(request: dict[str, Any]) -> bytes:
        observed.append(request)
        return rfc8785.dumps(
            _response(
                "owner_identity",
                {"request_owner_epoch": "cclc-owner.test"},
            )
        )

    thread = _serve_once(tmp_path, handler)
    result = CCLCOpenRequestClient(tmp_path).owner_identity()
    thread.join(timeout=2)
    assert not thread.is_alive()
    assert result == {"request_owner_epoch": "cclc-owner.test"}
    assert observed[0]["command"] == "owner_identity"
    assert observed[0]["payload"] == {}


def test_client_rejects_owner_error_and_noncanonical_response(tmp_path: Path) -> None:
    def error_handler(request: dict[str, Any]) -> bytes:
        return rfc8785.dumps(
            {
                "api_version": CCLC_OPEN_RESPONSE_VERSION,
                "command": request["command"],
                "error": {
                    "code": "missing_reference",
                    "details": {"effect": "not_started"},
                    "message": "component is absent",
                },
                "ok": False,
                "result": None,
            }
        )

    thread = _serve_once(tmp_path, error_handler)
    with pytest.raises(HeadlessApiError, match="component is absent") as captured:
        CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    assert captured.value.code == "missing_reference"
    assert captured.value.details == {"effect": "not_started"}
    thread.join(timeout=2)
    (tmp_path / CCLC_SOCKET_FILE).unlink()

    thread = _serve_once(
        tmp_path,
        lambda _request: b'{"ok": true, "api_version":"invalid"}',
    )
    with pytest.raises(HeadlessApiError) as captured:
        CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    assert captured.value.code == "cclc_open_response_invalid"
    thread.join(timeout=2)


def _target_snapshot() -> dict[str, Any]:
    return {
        "controllable_component_id": "component.arm",
        "component_lifecycle_generation": 4,
        "lifecycle_progress": {
            "blocking_cause_class": "availability",
            "blocking_reason": "controller_manager not ready",
            "delayed": True,
            "incident": None,
            "next_intent": "enable",
            "phase": "waiting_for_prerequisites",
            "waiting_since_ns": 1_000,
        },
        "lifecycle_target": {
            "origin": "startup",
            "requested_state": "active",
            "target_generation": 1,
        },
    }


def _snapshot_through_client(tmp_path: Path, snapshot: dict[str, Any]) -> dict[str, Any]:
    thread = _serve_once(
        tmp_path,
        lambda _request: rfc8785.dumps(_response("component_snapshot", snapshot)),
    )
    try:
        return CCLCOpenRequestClient(tmp_path).component_snapshot("component.arm")
    finally:
        thread.join(timeout=2)
        (tmp_path / CCLC_SOCKET_FILE).unlink(missing_ok=True)


def test_client_passes_lifecycle_target_and_progress_through_unchanged(
    tmp_path: Path,
) -> None:
    snapshot = _target_snapshot()
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot

    nulls = _target_snapshot()
    nulls["lifecycle_target"] = None
    nulls["lifecycle_progress"] = None
    assert _snapshot_through_client(tmp_path, nulls) == nulls

    satisfied = _target_snapshot()
    satisfied["lifecycle_progress"].update(
        blocking_cause_class="none",
        blocking_reason=None,
        delayed=False,
        next_intent=None,
        phase="satisfied",
        waiting_since_ns=None,
    )
    assert _snapshot_through_client(tmp_path, satisfied) == satisfied


@pytest.mark.parametrize(
    ("field", "key", "invalid"),
    [
        ("lifecycle_target", "requested_state", "degraded"),
        ("lifecycle_target", "origin", "browser"),
        ("lifecycle_target", "target_generation", 0),
        ("lifecycle_target", "target_generation", True),
        ("lifecycle_target", "unexpected", 1),
        ("lifecycle_progress", "phase", "retrying"),
        ("lifecycle_progress", "next_intent", "restart"),
        ("lifecycle_progress", "blocking_cause_class", "estop"),
        ("lifecycle_progress", "blocking_cause_class", ["fault"]),
        ("lifecycle_progress", "blocking_reason", ""),
        ("lifecycle_progress", "waiting_since_ns", -1),
        ("lifecycle_progress", "delayed", 1),
        ("lifecycle_progress", "acknowledged", True),
    ],
)
def test_client_rejects_unknown_lifecycle_target_or_progress_facts(
    tmp_path: Path, field: str, key: str, invalid: Any
) -> None:
    snapshot = _target_snapshot()
    snapshot[field][key] = invalid
    with pytest.raises(HeadlessApiError, match=field) as captured:
        _snapshot_through_client(tmp_path, snapshot)
    assert captured.value.code == "cclc_open_response_invalid"


def _incident_snapshot() -> dict[str, Any]:
    snapshot = _target_snapshot()
    snapshot["lifecycle_progress"].update(
        blocking_cause_class="collision",
        blocking_reason="controller reported C31",
        delayed=False,
        next_intent="recover",
        phase="blocked",
        incident={
            "acknowledged": False,
            "cause_class": "collision",
            "detected_at_ns": 1_759_900_000_000,
            "incident_id": "incident.component.arm.1",
            "summary": "Collision detected while executing",
            "vendor_codes": ["C31", "state=4", "mode=0"],
        },
    )
    return snapshot


@pytest.mark.parametrize("cause", ["collision", "external_control", "provider_lost"])
def test_client_passes_an_open_incident_through_unchanged(
    tmp_path: Path, cause: str
) -> None:
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["blocking_cause_class"] = cause
    snapshot["lifecycle_progress"]["incident"]["cause_class"] = cause
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot
    snapshot["lifecycle_progress"]["incident"]["vendor_codes"] = []
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot


@pytest.mark.parametrize(
    ("key", "invalid"),
    [
        ("cause_class", "availability"),
        ("cause_class", "none"),
        ("cause_class", "crash"),
        ("incident_id", ""),
        ("incident_id", "i" * 129),
        ("vendor_codes", "C31"),
        ("vendor_codes", [""]),
        ("vendor_codes", ["c" * 65]),
        ("vendor_codes", ["C"] * 17),
        ("detected_at_ns", -1),
        ("detected_at_ns", 1.5),
        ("summary", ""),
        ("summary", "s" * 1025),
        ("summary", "Cafe\u0301"),
        ("acknowledged", True),
        ("unexpected", 1),
    ],
)
def test_client_rejects_a_malformed_incident(
    tmp_path: Path, key: str, invalid: Any
) -> None:
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["incident"][key] = invalid
    with pytest.raises(HeadlessApiError, match="lifecycle_progress") as captured:
        _snapshot_through_client(tmp_path, snapshot)
    assert captured.value.code == "cclc_open_response_invalid"


def test_client_requires_the_incident_key(tmp_path: Path) -> None:
    snapshot = _target_snapshot()
    del snapshot["lifecycle_progress"]["incident"]
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _snapshot_through_client(tmp_path, snapshot)


def _incident_diagnostic() -> dict[str, Any]:
    return {
        "error": {
            "code": "controller_launch_exited", "message": "m" * 1024,
            "details": {"component_id": "component.arm"},
        },
        "snapshot": {
            "exit": {
                "status": None, "signal": 11, "core_dumped": False,
                "observed_utc": "2026-10-11T18:00:00Z",
            },
            "context": {
                "component_id": "component.arm", "request_context": "Apply request.1",
                "runtime_unit_id": "runtime.arm", "runtime_epoch": "epoch.1",
                "bundle_id": "bundle.1", "graph_revision": "graph.1",
                "generator_identity": None, "launch_identity": "launch.1",
            },
            "readiness": {
                "known": True, "controller_manager_present": True,
                "controller_observed": True, "controller_loaded": True,
                "controller_bound": False, "arm_controller_active": None,
                "hardware_ready": None, "joint_sample_available": False,
                "binding_epoch": str(2**64 - 1), "sample_age_ns": str(2**63 - 1),
                "captured_utc": None,
            },
            "settings": {
                "argv": ["ros2", "launch"], "controller_profile": "lite6",
                "runtime_namespace": "", "arm_controller_name": "arm_controller",
                "gripper_controller_name": None, "frame_prefix": "",
                "controllers_yaml": "controllers: {}\n",
                "retained_configuration_json": '{"effective":true}',
            },
            "generated": {
                "robot_description_sha256": "sha256:" + "a" * 64,
                "controllers_sha256": None,
            },
            "output": {
                "tail": 'first "line"\nCafé\n', "line_count": 2, "truncated": True,
                "dropped_forward_lines": str(2**64 - 1), "dropped_forward_bytes": 0,
                "invalid_text_bytes": 0,
            },
            "core_dump_link": None,
        },
    }


@pytest.mark.parametrize("diagnostic", [
    None, {**_incident_diagnostic(), "snapshot": None}, _incident_diagnostic(),
])
def test_client_preserves_optional_incident_diagnostic(
    tmp_path: Path, diagnostic: Any
) -> None:
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot


def test_short_only_diagnostic_requires_a_valid_error_and_closed_fields() -> None:
    for diagnostic in [
        {"error": None, "snapshot": None},
        {"error": _incident_diagnostic()["error"]},
        {"error": _incident_diagnostic()["error"], "snapshot": None, "extra": True},
    ]:
        snapshot = _incident_snapshot()
        snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
        with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
            _validate_lifecycle_target_facts(snapshot)


@pytest.mark.parametrize("lines,bytes_", [
    (0, 0), (2**53 - 1, str(2**53)), (str(2**64 - 1), str(2**64 - 1)),
])
def test_client_preserves_paired_output_drop_counters(
    tmp_path: Path, lines: Any, bytes_: Any
) -> None:
    snapshot = _target_snapshot()
    snapshot.update(dropped_output_lines=lines, dropped_output_bytes=bytes_)
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot


@pytest.mark.parametrize("invalid", [None, True, -1, 1.5, 2**53, "01", str(2**64)])
def test_rejects_invalid_or_unpaired_output_drop_counters(invalid: Any) -> None:
    for field in ("dropped_output_lines", "dropped_output_bytes"):
        snapshot = _target_snapshot()
        snapshot.update(dropped_output_lines=0, dropped_output_bytes=0)
        snapshot[field] = invalid
        with pytest.raises(HeadlessApiError, match="dropped_output"):
            _validate_lifecycle_target_facts(snapshot)
        del snapshot[field]
        with pytest.raises(HeadlessApiError, match="dropped_output"):
            _validate_lifecycle_target_facts(snapshot)


def test_client_preserves_diagnostic_bom_and_literal_detail_keys(tmp_path: Path) -> None:
    snapshot = _incident_snapshot()
    diagnostic = _incident_diagnostic()
    diagnostic["snapshot"]["settings"]["controllers_yaml"] = "\ufeffcontrollers: {}\n"
    diagnostic["error"]["details"] = {"__proto__": "observed"}
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot


@pytest.mark.parametrize(
    ("path", "invalid"),
    [
        (("extra",), True),
        (("error", "details"), {str(i): "value" for i in range(9)}),
        (("error", "details"), {"": "value"}),
        (("error", "message"), "m" * 1025),
        (("snapshot", "extra"), {}),
        (("snapshot", "exit", "signal"), True),
        (("snapshot", "exit", "status"), 256),
        (("snapshot", "context", "component_id"), "x" * 129),
        (("snapshot", "readiness", "controller_bound"), "unknown"),
        (("snapshot", "readiness", "sample_age_ns"), 2**53),
        (("snapshot", "readiness", "sample_age_ns"), str(2**63)),
        (("snapshot", "readiness", "binding_epoch"), str(2**64)),
        (("snapshot", "settings", "retained_configuration_json"), {"escape": True}),
        (("snapshot", "settings", "frame_prefix"), "Cafe\u0301"),
        (("snapshot", "settings", "argv"), ["\ud800"]),
        (("snapshot", "generated", "controllers_sha256"), "sha256:" + "A" * 64),
        (("snapshot", "output", "tail"), "Cafe\u0301\n"),
        (("snapshot", "output", "line_count"), 1),
        (("snapshot", "output", "truncated"), 1),
        (("snapshot", "output", "dropped_forward_bytes"), "01"),
    ],
)
def test_rejects_malformed_closed_diagnostic(path: tuple[str, ...], invalid: Any) -> None:
    snapshot = _incident_snapshot()
    diagnostic = _incident_diagnostic()
    cursor = diagnostic
    for field in path[:-1]:
        cursor = cursor[field]
    cursor[path[-1]] = invalid
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(snapshot)


def test_diagnostic_caps_encoded_tail_and_preserves_nullable_facts() -> None:
    snapshot = _incident_snapshot()
    diagnostic = _incident_diagnostic()
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    facts = diagnostic["snapshot"]
    facts["readiness"] = dict.fromkeys(facts["readiness"])
    output = facts["output"]
    for tail, lines in [("", 0), ("a\nb", 2), ("a\n" * 2000, 2000), ('"' * 65535, 1)]:
        output.update(tail=tail, line_count=lines)
        _validate_lifecycle_target_facts(snapshot)
    for tail, lines in [('"' * 65536, 1), ("a\n" * 2001, 2001), ("😀" * 32768, 1)]:
        output.update(tail=tail, line_count=lines)
        with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
            _validate_lifecycle_target_facts(snapshot)
    output.update(tail="", line_count=0)
    facts["settings"]["controllers_yaml"] = "x" * (300 * 1024)
    _validate_lifecycle_target_facts(snapshot)
    oversized = deepcopy(snapshot)
    oversized["lifecycle_progress"]["incident"]["diagnostic"]["snapshot"]["settings"][
        "controllers_yaml"
    ] = "x" * (512 * 1024)
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(oversized)


def test_client_rejects_a_lifecycle_progress_with_a_missing_key(tmp_path: Path) -> None:
    snapshot = _target_snapshot()
    del snapshot["lifecycle_progress"]["delayed"]
    with pytest.raises(HeadlessApiError, match="lifecycle_progress") as captured:
        _snapshot_through_client(tmp_path, snapshot)
    assert captured.value.code == "cclc_open_response_invalid"


# Owner-clock times use the full-width nonnegative int64 contract: a JSON
# number up to 2**53 - 1, canonical decimal text above (and accepted below).
_ABOVE_SAFE_NS = "9007199254740993"
_MAX_INT64_NS = "9223372036854775807"


@pytest.mark.parametrize(
    ("waiting_since_ns", "detected_at_ns"),
    [
        (_MAX_INT64_NS, _ABOVE_SAFE_NS),
        (2**53 - 1, 0),
        ("1000", "0"),
    ],
)
def test_client_passes_full_width_progress_times_through_exactly(
    tmp_path: Path, waiting_since_ns: int | str, detected_at_ns: int | str
) -> None:
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["waiting_since_ns"] = waiting_since_ns
    snapshot["lifecycle_progress"]["incident"]["detected_at_ns"] = detected_at_ns
    forwarded = _snapshot_through_client(tmp_path, snapshot)
    assert forwarded == snapshot
    assert forwarded["lifecycle_progress"]["waiting_since_ns"] == waiting_since_ns
    incident = forwarded["lifecycle_progress"]["incident"]
    assert incident["detected_at_ns"] == detected_at_ns


_NONCANONICAL_NS = [
    -1,
    True,
    1.5,
    "",
    "01",
    "-1",
    "+1",
    " 1",
    "1e3",
    "1.0",
    "\u0661",
    "9223372036854775808",
    "18446744073709551616",
]


@pytest.mark.parametrize("invalid", _NONCANONICAL_NS)
def test_client_rejects_noncanonical_progress_times(
    tmp_path: Path, invalid: Any
) -> None:
    for place in ("waiting_since_ns", "detected_at_ns"):
        snapshot = _incident_snapshot()
        if place == "waiting_since_ns":
            snapshot["lifecycle_progress"]["waiting_since_ns"] = invalid
        else:
            snapshot["lifecycle_progress"]["incident"]["detected_at_ns"] = invalid
        with pytest.raises(HeadlessApiError, match="lifecycle_progress") as captured:
            _snapshot_through_client(tmp_path, snapshot)
        assert captured.value.code == "cclc_open_response_invalid"


def test_progress_time_above_the_safe_range_must_be_decimal_text() -> None:
    # A bare number above 2**53 - 1 is not the producer spelling and may
    # already have been rounded by a JSON consumer.
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["waiting_since_ns"] = 2**53
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(snapshot)
    snapshot["lifecycle_progress"]["waiting_since_ns"] = str(2**53)
    _validate_lifecycle_target_facts(snapshot)


def _lifecycle_request(*, confirmed: bool) -> dict[str, Any]:
    return {
        "request_context": {
            "request_id": "request.lifecycle.1",
            "request_owner_epoch": "cli.epoch.1",
            "source_label": "test-client",
            "correlation_id": None,
        },
        "controllable_component_id": "component.arm",
        "intent": "enable",
        "expected_graph_revision": "graph.1",
        "expected_component_lifecycle_generation": 4,
        "expected_control_domains": [
            {
                "control_domain_id": "domain.arm",
                "control_domain_revision": "domain.revision.1",
                "controller_topology_id": "topology.arm",
                "topology_revision": "topology.revision.1",
                "expected_active_control_configuration": {
                    "presence": "absent",
                    "active_control_configuration_id": None,
                    "active_control_configuration_generation": None,
                },
                "expected_control_bindings": [
                    {
                        "control_binding_id": "binding.arm",
                        "control_binding_revision": "binding.revision.1",
                        "control_route_id": "route.arm",
                    }
                ],
            }
        ],
        "reason": "operator-confirmed",
        "confirmed": confirmed,
        "expected_admission_generation": 7,
    }


class FakeCCLC:
    def __init__(self) -> None:
        self.lifecycle: list[dict[str, Any]] = []

    def component_snapshot(self, component_id: str) -> dict[str, Any]:
        return {"controllable_component_id": component_id}

    def owner_identity(self) -> dict[str, Any]:
        return {"request_owner_epoch": "cclc-owner.http-test"}

    def route_templates(self, component_id: str) -> dict[str, Any]:
        return {
            "controllable_component_id": component_id,
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
            "templates": [],
        }

    def submit_lifecycle(self, intent: dict[str, Any]) -> dict[str, Any]:
        self.lifecycle.append(intent)
        return {
            "acceptance": None,
            "replay_status": "request_id_conflict",
            "status": None,
        }

    def lifecycle_status(self, transaction_id: str) -> dict[str, Any] | None:
        return None

    def lifecycle_request_result(self, intent):
        return {
            "replay_status": "replayed_exact",
            "acceptance": None,
            "status": None,
            "rejection": {
                "code": "invalid_state_transition",
                "message": "Held",
                "details": {
                    "effect": "not_started",
                    "reason": "program_node_stop_fence_active",
                    "selection_revision": "7",
                },
            },
        }

    def submit_route(self, intent: dict[str, Any]) -> dict[str, Any]:
        return {
            "acceptance": None,
            "replay_status": "request_ledger_full",
            "status": None,
        }

    def route_request_result(self, intent):
        self.route_result_request = intent
        return None

    def route_status(self, transaction_id: str) -> dict[str, Any] | None:
        return None


class FakeAdmission:
    def acquire(self, intent: dict[str, Any]) -> dict[str, Any]:
        return {
            "intent": intent,
            "claim_generation": 1,
            "acquired_admission_generation": 7,
            "accepted_at_ns": 1,
        }

    def release(self, _release: dict[str, Any]) -> dict[str, Any]:
        return {"authoring_allowed": True, "admission_generation": 8}


def test_http_lifecycle_route_is_thin_and_requires_confirmation() -> None:
    fake = FakeCCLC()
    service = HeadlessService(  # type: ignore[arg-type]
        object(), object(), cell_admission=FakeAdmission()
    )
    client = TestClient(create_app(service=service, cclc=fake))

    rejected = client.post(
        "/api/cclc/lifecycle/requests",
        json=_lifecycle_request(confirmed=False),
    )
    assert rejected.status_code == 400
    assert rejected.json()["detail"]["code"] == "cclc_confirmation_required"
    assert fake.lifecycle == []

    accepted = client.post(
        "/api/cclc/lifecycle/requests",
        json=_lifecycle_request(confirmed=True),
    )
    assert accepted.status_code == 200
    assert accepted.json()["replay_status"] == "request_id_conflict"
    assert fake.lifecycle[0]["expected_admission_generation"] == 7
    assert len(fake.lifecycle) == 1
    assert "confirmed" not in fake.lifecycle[0]
    result = client.post(
        "/api/cclc/lifecycle/requests/result", json=_lifecycle_request(confirmed=False)
    )
    assert result.status_code == 200
    assert result.json()["result"]["rejection"]["details"]["selection_revision"] == "7"
    assert len(fake.lifecycle) == 1  # The lookup must never call submit.

    route_request = {
        "request_context": _lifecycle_request(confirmed=False)["request_context"],
        "expected_admission_generation": 7,
        "expected_retained_bundle_id": "bundle.1",
        "expected_graph_revision": "graph.1",
        "desired_configuration": {
            "control_domain_id": "domain.arm",
            "control_domain_revision": "domain.revision.1",
            "controller_topology_id": "topology.arm",
            "topology_revision": "topology.revision.1",
            "activation_set_id": "activation.arm",
            "binding_routes": [
                {
                    "control_binding_id": "binding.arm",
                    "control_binding_revision": "binding.revision.1",
                    "control_route_id": "route.arm",
                }
            ],
        },
        "expected_active_control_configuration": {"presence": "absent"},
        "component_expectations": [
            {
                "controllable_component_id": "component.arm",
                "expected_component_lifecycle_generation": 4,
                "expected_component_lifecycle_state": "active",
            }
        ],
        "confirmed": False,
    }
    # This service fixture has no authoring read. Result observation must not
    # ask for current Idle, confirm or resubmit the original command.
    route = client.post("/api/cclc/routes/requests/result", json=route_request)
    assert route.status_code == 200, route.text
    assert route.json() == {"result": None}
    assert (
        fake.route_result_request["request_context"]["request_owner_epoch"]
        == "cli.epoch.1"
    )
    assert "confirmed" not in fake.route_result_request
    assert fake.route_result_request["expected_admission_generation"] == 7
    # Legacy lookup preserves absence; it is not a new command or a request
    # to silently substitute the current generation.
    del route_request["expected_admission_generation"]
    assert client.post("/api/cclc/routes/requests/result", json=route_request).status_code == 200
    assert "expected_admission_generation" not in fake.route_result_request
    route_request["confirmed"] = True
    assert client.post("/api/cclc/routes/requests", json=route_request).status_code == 422

    snapshot = client.get("/api/cclc/components/component.arm")
    assert snapshot.json() == {"controllable_component_id": "component.arm"}
    assert client.get("/api/cclc/owner").json() == {
        "request_owner_epoch": "cclc-owner.http-test"
    }
    templates = client.get("/api/cclc/components/component.arm/route-templates")
    assert templates.json()["templates"] == []
    assert client.get("/api/cclc/lifecycle/transactions/lifecycle.1").json() == {
        "status": None
    }

    invalid = _lifecycle_request(confirmed=True)
    invalid["caller_asserted_principal"] = "operator"
    assert client.post("/api/cclc/lifecycle/requests", json=invalid).status_code == 422


def _through_client(
    tmp_path: Path,
    command: str,
    result: dict[str, Any] | None,
    call: Callable[[CCLCOpenRequestClient], Any],
    observed: list[dict[str, Any]] | None = None,
) -> Any:
    def handler(request: dict[str, Any]) -> bytes:
        if observed is not None:
            observed.append(request)
        return rfc8785.dumps(_response(command, result))

    thread = _serve_once(tmp_path, handler)
    try:
        return call(CCLCOpenRequestClient(tmp_path))
    finally:
        thread.join(timeout=2)
        (tmp_path / CCLC_SOCKET_FILE).unlink(missing_ok=True)


def _incident_recovery_result(replay_status: str = "accepted_new") -> dict[str, Any]:
    return {
        "acceptance": None,
        "incident_recovery": {
            "acknowledged_at_ns": 9_000,
            "acknowledged_by": "test-client",
            "cause_class": "emergency_stop",
            "controllable_component_id": "component.arm",
            "incident_id": "incident.component.arm.5000",
        },
        "replay_status": replay_status,
        "status": None,
    }


def test_client_forwards_an_authorization_only_incident_recovery(tmp_path: Path) -> None:
    intent = _lifecycle_request(confirmed=True)
    intent.pop("confirmed")
    intent["intent"] = "recover"
    submitted = _through_client(
        tmp_path,
        "lifecycle_submit",
        _incident_recovery_result(),
        lambda client: client.submit_lifecycle(intent),
    )
    assert submitted == _incident_recovery_result()
    replayed = _through_client(
        tmp_path,
        "lifecycle_request_result",
        _incident_recovery_result("replayed_exact"),
        lambda client: client.lifecycle_request_result(intent),
    )
    assert replayed == _incident_recovery_result("replayed_exact")


@pytest.mark.parametrize(
    "mutate",
    [
        lambda result: result["incident_recovery"].update(
            controllable_component_id="component.gripper"
        ),
        lambda result: result["incident_recovery"].update(cause_class="availability"),
        lambda result: result["incident_recovery"].update(acknowledged_at_ns=-1),
        lambda result: result["incident_recovery"].update(acknowledged_at_ns="09000"),
        lambda result: result["incident_recovery"].update(acknowledged_at_ns="-9000"),
        lambda result: result["incident_recovery"].update(
            acknowledged_at_ns="9223372036854775808"
        ),
        lambda result: result["incident_recovery"].update(acknowledged_at_ns=True),
        lambda result: result["incident_recovery"].update(summary="extra"),
        lambda result: result.update(replay_status="request_id_conflict"),
        lambda result: result.update(
            status={"acceptance": {"lifecycle_transaction_id": "lifecycle.1"}}
        ),
        lambda result: result.update(incident_recovery=None),
    ],
)
def test_client_rejects_a_malformed_incident_recovery(
    tmp_path: Path, mutate: Callable[[dict[str, Any]], None]
) -> None:
    intent = _lifecycle_request(confirmed=True)
    intent.pop("confirmed")
    intent["intent"] = "recover"
    result = _incident_recovery_result()
    mutate(result)
    with pytest.raises(HeadlessApiError) as captured:
        _through_client(
            tmp_path,
            "lifecycle_submit",
            result,
            lambda client: client.submit_lifecycle(intent),
        )
    assert captured.value.code == "cclc_open_response_invalid"


def _incident_event(sequence: int, kind: str = "component_incident") -> dict[str, Any]:
    return {
        "event_kind": kind,
        "event_sequence": sequence,
        "incident_id": "incident.component.arm.5000",
        "occurred_at_ns": 5_000 + sequence,
        "payload": {
            "cause_class": "emergency_stop",
            "event_kind": kind,
            "incident_id": "incident.component.arm.5000",
            "detected_at_ns": 5_000,
        },
    }


def test_client_reads_ordered_incident_events(tmp_path: Path) -> None:
    observed: list[dict[str, Any]] = []
    page = {
        "events": [
            _incident_event(3),
            _incident_event(4, "component_incident_acknowledged"),
        ],
        "through_sequence": 4,
    }
    result = _through_client(
        tmp_path,
        "incident_events",
        page,
        lambda client: client.incident_events(2, 64),
        observed,
    )
    assert result == page
    assert observed[0]["command"] == "incident_events"
    assert observed[0]["payload"] == {"after_sequence": 2, "limit": 64}
    empty = _through_client(
        tmp_path,
        "incident_events",
        {"events": [], "through_sequence": 9},
        lambda client: client.incident_events(9, 1),
    )
    assert empty == {"events": [], "through_sequence": 9}
    with pytest.raises(ValueError):
        CCLCOpenRequestClient(tmp_path).incident_events(0, 65)


@pytest.mark.parametrize(
    "page",
    [
        {"events": [_incident_event(3)], "through_sequence": 4},
        {"events": [_incident_event(2)], "through_sequence": 2},
        {"events": [_incident_event(4), _incident_event(3)], "through_sequence": 3},
        {"events": [{**_incident_event(3), "event_kind": "other"}], "through_sequence": 3},
        {
            "events": [{**_incident_event(3), "incident_id": "incident.other"}],
            "through_sequence": 3,
        },
        {
            "events": [{**_incident_event(3), "occurred_at_ns": -1}],
            "through_sequence": 3,
        },
        {
            "events": [{**_incident_event(3), "occurred_at_ns": "05003"}],
            "through_sequence": 3,
        },
        {
            "events": [{**_incident_event(3), "occurred_at_ns": "-5003"}],
            "through_sequence": 3,
        },
        {
            "events": [{**_incident_event(3), "occurred_at_ns": 5003.5}],
            "through_sequence": 3,
        },
        {"events": [{**_incident_event(3), "extra": 1}], "through_sequence": 3},
        {"events": [], "through_sequence": 2, "extra": None},
        {"events": [], "through_sequence": 3},
    ],
)
def test_client_rejects_malformed_incident_events(
    tmp_path: Path, page: dict[str, Any]
) -> None:
    with pytest.raises(HeadlessApiError) as captured:
        _through_client(
            tmp_path,
            "incident_events",
            page,
            lambda client: client.incident_events(2, 64),
        )
    assert captured.value.code == "cclc_open_response_invalid"


@pytest.mark.parametrize(
    "acknowledged_at_ns", [_MAX_INT64_NS, _ABOVE_SAFE_NS, 2**53 - 1]
)
def test_client_forwards_a_full_width_incident_acknowledgement_time(
    tmp_path: Path, acknowledged_at_ns: int | str
) -> None:
    intent = _lifecycle_request(confirmed=True)
    intent.pop("confirmed")
    intent["intent"] = "recover"
    result = _incident_recovery_result()
    result["incident_recovery"]["acknowledged_at_ns"] = acknowledged_at_ns
    submitted = _through_client(
        tmp_path,
        "lifecycle_submit",
        result,
        lambda client: client.submit_lifecycle(intent),
    )
    assert submitted == result
    recovery = submitted["incident_recovery"]
    assert recovery["acknowledged_at_ns"] == acknowledged_at_ns


def test_client_reads_incident_events_with_full_width_times(tmp_path: Path) -> None:
    page = {
        "events": [
            {
                **_incident_event(3),
                "occurred_at_ns": _ABOVE_SAFE_NS,
                "payload": {
                    **_incident_event(3)["payload"],
                    "detected_at_ns": _ABOVE_SAFE_NS,
                },
            },
            {
                **_incident_event(4, "component_incident_acknowledged"),
                "occurred_at_ns": _MAX_INT64_NS,
            },
        ],
        "through_sequence": 4,
    }
    result = _through_client(
        tmp_path,
        "incident_events",
        page,
        lambda client: client.incident_events(2, 64),
    )
    assert result == page
    assert [event["occurred_at_ns"] for event in result["events"]] == [
        _ABOVE_SAFE_NS,
        _MAX_INT64_NS,
    ]

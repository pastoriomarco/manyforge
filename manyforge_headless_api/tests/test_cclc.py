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
    _validate_component_readiness,
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
                _target_snapshot(),
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


def _provider_occurrence() -> dict[str, Any]:
    return {"control_domain_id": "domain.arm", "provider_instance_id": "provider.arm",
        "provider_manifest_id": "manifest.arm", "provider_manifest_revision": "sha256:" + "3" * 64,
        "provider_runtime_unit_id": "runtime.arm", "provider_runtime_epoch": "epoch.arm",
        "lifecycle_plan_catalog_digest": "sha256:" + "2" * 64}


def _readiness_blocker() -> dict[str, Any]:
    return {"code": "component_lifecycle_transition_missing", "message": "Controller not ready",
        "details": {"reason": "no_transition_satisfied"}}


def _target_snapshot() -> dict[str, Any]:
    return {
        "controllable_component_id": "component.arm",
        "component_lifecycle_generation": 4,
        "component_lifecycle_state": "prepared",
        "publication_generation": 7,
        "graph_revision": "graph.1", "retained_bundle_id": "bundle.1",
        "snapshot_digest": "sha256:" + "4" * 64,
        "active_public_pointer_digest": "sha256:" + "1" * 64,
        "domains": [{"control_domain_id": "domain.arm", "control_domain_revision": "domain.revision.1",
            "controller_topology_id": "topology.arm", "topology_revision": "topology.revision.1",
            "active_control_configuration": None, "current_control_bindings": [{
                "control_binding_id": "binding.arm", "control_binding_revision": "binding.1", "control_route_id": "route.arm"}],
            "kind": "controller", "resources": ["joint.1"]}],
        "provider_occurrences": [_provider_occurrence()],
        "readiness": {"request_owner_epoch": "cclc-owner.1", "evaluated_at_ns": 100,
            "next_intent": "enable", "requested_state": "active", "ready": False,
            "error": _readiness_blocker(), "domains": [{
                "provider_occurrence": {**_provider_occurrence(), "control_domain_revision": "domain.revision.1"},
                "health": {"observed_state": "prepared", "facts": [{"lifecycle_fact_id": "fact.loaded",
                    "value": {"kind": "boolean", "boolean_value": False}}], "diagnostic": None},
                "decision_kind": None, "error": _readiness_blocker()}],
            "measured_state": {"required": True, "available": True, "error": None}},
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
                "controllable_component_id": "component.arm",
                "request_context": "Apply request.1",
                "runtime_unit_id": "runtime.arm", "runtime_epoch": "epoch.1",
                "retained_bundle_id": "bundle.1", "graph_revision": "graph.1",
                "generator_identity": None, "launch_identity": "launch.1",
            },
            "launch_readiness": {
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


def test_client_preserves_real_257_byte_launch_identity(tmp_path: Path) -> None:
    identity = 'mock\nur5e_robot\nur5e_robotiq_2f_85\n/manyforge/ur5e_robot\njoint_trajectory_controller\nrobotiq_gripper_controller\n\nsha256:02e2b9e7733647dc75eea1a518dda0289aa07603d668d07e16dc87c79ab970fc\nsha256:d5e2658bd13c5f16c8f23c699c70fc200a4e4b904e995571c3eda1718faffec6\n'
    assert len(identity.encode("utf-8")) == 257
    diagnostic = _incident_diagnostic()
    diagnostic["snapshot"]["context"]["launch_identity"] = identity
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    assert _snapshot_through_client(tmp_path, snapshot) == snapshot


def test_launch_identity_uses_close_packet_bound_without_widening_other_fields() -> None:
    diagnostic = _incident_diagnostic()
    diagnostic["snapshot"]["context"]["launch_identity"] = "x" * (256 * 1024)
    snapshot = _incident_snapshot()
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    _validate_lifecycle_target_facts(snapshot)
    diagnostic["snapshot"]["context"]["launch_identity"] += "x"
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(snapshot)
    diagnostic["snapshot"]["context"]["launch_identity"] = "launch.1"
    diagnostic["snapshot"]["context"]["controllable_component_id"] = "x" * 129
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(snapshot)


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
        (("snapshot", "context", "controllable_component_id"), "x" * 129),
        (("snapshot", "launch_readiness", "controller_bound"), "unknown"),
        (("snapshot", "launch_readiness", "sample_age_ns"), 2**53),
        (("snapshot", "launch_readiness", "sample_age_ns"), str(2**63)),
        (("snapshot", "launch_readiness", "binding_epoch"), str(2**64)),
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


@pytest.mark.parametrize(("group", "new", "old"), [
    ("context", "controllable_component_id", "component_id"),
    ("context", "retained_bundle_id", "bundle_id"),
    (None, "launch_readiness", "readiness"),
])
def test_diagnostic_refuses_superseded_names(group: str | None, new: str, old: str) -> None:
    snapshot = _incident_snapshot()
    diagnostic = _incident_diagnostic()
    facts = diagnostic["snapshot"] if group is None else diagnostic["snapshot"][group]
    facts[old] = facts.pop(new)
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    with pytest.raises(HeadlessApiError, match="lifecycle_progress"):
        _validate_lifecycle_target_facts(snapshot)


def test_diagnostic_caps_encoded_tail_and_preserves_nullable_facts() -> None:
    snapshot = _incident_snapshot()
    diagnostic = _incident_diagnostic()
    snapshot["lifecycle_progress"]["incident"]["diagnostic"] = diagnostic
    facts = diagnostic["snapshot"]
    facts["launch_readiness"] = dict.fromkeys(facts["launch_readiness"])
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
                "kind": "controller",
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


def _provider_lifecycle_request() -> dict[str, Any]:
    domain = json.loads(_CORE_PROVIDER_DOMAIN_SAMPLE)
    configuration = domain["active_control_configuration"]
    request = _lifecycle_request(confirmed=True)
    request["expected_control_domains"] = [{
        "kind": "io_provider",
        "control_domain_id": domain["control_domain_id"],
        "control_domain_revision": domain["control_domain_revision"],
        "provider": domain["provider"],
        "expected_active_control_configuration": {
            "presence": "present",
            "active_control_configuration_id": configuration["active_control_configuration_id"],
            "active_control_configuration_generation":
                configuration["active_control_configuration_generation"],
        },
    }]
    return request


def test_http_lifecycle_forwards_exact_provider_expected_domain() -> None:
    fake = FakeCCLC()
    service = HeadlessService(  # type: ignore[arg-type]
        object(), object(), cell_admission=FakeAdmission()
    )
    client = TestClient(create_app(service=service, cclc=fake))
    request = _provider_lifecycle_request()
    assert client.post("/api/cclc/lifecycle/requests", json=request).status_code == 200
    assert fake.lifecycle[0]["expected_control_domains"] == request["expected_control_domains"]
    for mutate in (
        lambda d: d.update(controller_topology_id="topology.arm"),
        lambda d: d.update(expected_control_bindings=[]),
        lambda d: d.update(kind="controller"),
        lambda d: d.update(kind="provider"),
        lambda d: d.pop("kind"),
        lambda d: d["provider"].update(signal_names=[]),
        lambda d: d["provider"].update(extra=True),
        lambda d: d["provider"].update(lifecycle_command_timeout_ns="30000000000"),
    ):
        invalid = _provider_lifecycle_request()
        mutate(invalid["expected_control_domains"][0])
        assert client.post("/api/cclc/lifecycle/requests", json=invalid).status_code == 422
    assert len(fake.lifecycle) == 1


@pytest.mark.parametrize("mutate", [
    lambda d: d.pop("kind"),
    lambda d: d.update(kind="provider"),
    lambda d: d.update(kind="io_provider"),
])
def test_http_lifecycle_refuses_kindless_or_superseded_controller_domain(mutate) -> None:
    fake = FakeCCLC()
    service = HeadlessService(  # type: ignore[arg-type]
        object(), object(), cell_admission=FakeAdmission()
    )
    client = TestClient(create_app(service=service, cclc=fake))
    request = _lifecycle_request(confirmed=True)
    mutate(request["expected_control_domains"][0])
    assert client.post("/api/cclc/lifecycle/requests", json=request).status_code == 422
    assert fake.lifecycle == []


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


def test_client_preserves_cell_observer_incident_and_bounded_tail(tmp_path: Path) -> None:
    event = _incident_event(3)
    event["incident_id"] = "observer." + "a" * 64
    event["payload"] = {
        "event_kind": "component_incident",
        "incident_id": event["incident_id"],
        "target_kind": "cell_observer",
        "target": {
            "cell_id": "robot_cell",
            "source_id": "observer\x00source",
            "source_epoch_base": "manager-observer",
        },
        "cause_class": "provider_lost",
        "detected_at_ns": "9223372036854775807",
        "summary": "observer exited with signal 11",
        "acknowledgement": None,
        "diagnostic_snapshot": {
            "exit": {
                "status": None,
                "signal": 11,
                "core_dumped": False,
                "observed_utc": "2026-10-12T14:00:00Z",
            },
            "context": {
                "cell_id": "robot_cell",
                "source_id": "observer\x00source",
                "source_epoch_base": "manager-observer",
                "pid": 1234,
            },
            "settings": {"retained_snapshot_path": "/run/observer-input.json"},
            "generated": {},
            "output": {
                "tail": "x" * (128 * 1024 - 2),
                "line_count": 1,
                "truncated": True,
                "dropped_forward_lines": 0,
                "dropped_forward_bytes": 0,
                "invalid_text_bytes": 0,
            },
            "core_dump_link": None,
        },
    }
    page = {"events": [event], "through_sequence": 3}
    result = _through_client(
        tmp_path,
        "incident_events",
        page,
        lambda client: client.incident_events(2, 64),
    )
    assert result == page
    assert "controllable_component_id" not in result["events"][0]["payload"]


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


@pytest.mark.parametrize(("path", "value"), [
    (("readiness",), None), (("readiness", "extra"), True),
    (("readiness", "next_intent"), None), (("readiness", "requested_state"), "prepared"),
    (("readiness", "ready"), 1), (("readiness", "ready"), True),
    (("readiness", "evaluated_at_ns"), True), (("readiness", "domains"), []),
    (("readiness", "domains", 0, "provider_occurrence", "control_domain_revision"), "foreign"),
    (("readiness", "domains", 0, "provider_occurrence", "lifecycle_plan_catalog_digest"), "foreign"),
    (("readiness", "domains", 0, "health", "facts", 0, "value"), {"kind": "boolean", "boolean_value": "false"}),
    (("readiness", "domains", 0, "health", "facts", 0, "value"), {"kind": "integer", "integer_value": True}),
    (("readiness", "domains", 0, "health", "facts", 0, "value"), {"kind": "integer", "integer_value": "1"}),
    (("readiness", "measured_state"), {"required": False, "available": False, "error": None}),
    (("readiness", "measured_state"), {"required": None, "available": None, "error": None}),
])
def test_q114_rejects_invalid_closed_readiness(path: tuple, value: Any) -> None:
    snapshot = _target_snapshot()
    row = snapshot
    for key in path[:-1]:
        row = row[key]
    row[path[-1]] = value
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


@pytest.mark.parametrize("has_activation_step", [True, False])
def test_q115_optional_activation_fact_preserves_health_and_blockers(has_activation_step: bool) -> None:
    snapshot = _target_snapshot()
    domain = snapshot["readiness"]["domains"][0]
    domain["has_activation_step"] = has_activation_step
    domain["health"]["observed_state"] = "faulted"
    domain["health"]["facts"] = [
        {"lifecycle_fact_id": "external.fault_present", "value": {"kind": "boolean", "boolean_value": True}},
        {"lifecycle_fact_id": "external.transport_ready", "value": {"kind": "boolean", "boolean_value": False}},
    ]
    _validate_component_readiness(snapshot)
    assert domain["has_activation_step"] is has_activation_step
    assert domain["health"]["observed_state"] == "faulted"
    assert domain["health"]["facts"][0]["value"]["boolean_value"] is True
    assert domain["health"]["facts"][1]["value"]["boolean_value"] is False
    assert snapshot["readiness"]["ready"] is False
    assert snapshot["readiness"]["next_intent"] == "enable"
    assert domain["error"] == _readiness_blocker()


def test_q115_absent_activation_fact_remains_unstated() -> None:
    snapshot = _target_snapshot()
    _validate_component_readiness(snapshot)
    assert "has_activation_step" not in snapshot["readiness"]["domains"][0]


@pytest.mark.parametrize("value", [None, "false", 0, {}, []])
def test_q115_rejects_nonboolean_activation_fact(value: Any) -> None:
    snapshot = _target_snapshot()
    snapshot["readiness"]["domains"][0]["has_activation_step"] = value
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


def test_q115_extended_domain_wire_stays_closed() -> None:
    snapshot = _target_snapshot()
    snapshot["readiness"]["domains"][0]["hasActivationStep"] = False
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


# Source-derived provider-domain sample in the agreed contract 125/128 response shape
# (kind "io_provider", common `resources`). It is NOT a capture from the normal producer;
# it is re-pinned from Track 2's real capture later.
_CORE_PROVIDER_DOMAIN_SAMPLE = """{
  "control_domain_id": "domain.arm",
  "control_domain_revision": "domain.rev.1",
  "kind": "io_provider",
  "provider": {
    "controllable_component_id": "component.arm",
    "provider_runtime_unit_id": "runtime.arm",
    "provider_instance_id": "provider.arm",
    "provider_manifest_id": "manifest.arm",
    "provider_manifest_revision": "sha256:ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff",
    "lifecycle_plan_catalog_digest": "sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
    "signal_configuration_digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111",
    "signal_names": [
      "ready"
    ],
    "lifecycle_command_timeout_ns": 30000000000
  },
  "provider_runtime_epoch": "epoch.1",
  "resources": [],
  "active_control_configuration": {
    "active_control_configuration_id": "provider.configuration",
    "active_control_configuration_generation": 9007199254740991,
    "source_runtime_unit_id": "runtime.arm",
    "source_runtime_epoch": "epoch.1",
    "source_sequence": 9007199254740991,
    "signal_configuration_digest": "sha256:1111111111111111111111111111111111111111111111111111111111111111"
  }
}
"""


def _provider_domain_snapshot() -> dict[str, Any]:
    """Snapshot around the pinned Core provider domain with its paired occurrence."""
    snapshot = _target_snapshot()
    domain = json.loads(_CORE_PROVIDER_DOMAIN_SAMPLE)
    provider = domain["provider"]
    occurrence = {"control_domain_id": domain["control_domain_id"],
        **{key: provider[key] for key in ("provider_instance_id", "provider_manifest_id",
            "provider_manifest_revision", "provider_runtime_unit_id", "lifecycle_plan_catalog_digest")},
        "provider_runtime_epoch": domain["provider_runtime_epoch"]}
    snapshot["domains"] = [domain]
    snapshot["provider_occurrences"] = [occurrence]
    snapshot["readiness"]["domains"][0]["provider_occurrence"] = {
        **occurrence, "control_domain_revision": domain["control_domain_revision"]}
    snapshot["readiness"]["measured_state"] = {"required": False, "available": None, "error": None}
    return snapshot


def test_provider_domain_readiness_parses() -> None:
    snapshot = _provider_domain_snapshot()
    _validate_component_readiness(snapshot)
    lifecycle_only = {key: value for key, value in snapshot.items()
                      if key not in {"lifecycle_progress", "lifecycle_target", "readiness"}}
    _validate_component_readiness(lifecycle_only, lifecycle_only=True)
    domain = snapshot["domains"][0]
    domain["active_control_configuration"] = None
    _validate_component_readiness(snapshot)
    domain["resources"] = ["io.do.1", "io.do.2"]
    _validate_component_readiness(snapshot)


@pytest.mark.parametrize("mutate", [
    lambda d: d.update(current_control_bindings=[]),
    lambda d: d.update(current_resources=[]),
    lambda d: d.update(output_resources=[]),
    lambda d: d.update(output_resources=d.pop("resources")),
    lambda d: d.update(controller_topology_id="topology.arm"),
    lambda d: d.update(kind="controller"),
    lambda d: d.pop("kind"),
    lambda d: d.pop("active_control_configuration"),
    lambda d: d.pop("resources"),
    lambda d: d.update(resources=["io.2", "io.1"]),
    lambda d: d.update(resources=[""]),
    lambda d: d["provider"].update(signal_names=[]),
    lambda d: d["provider"].update(signal_names=["b", "a"]),
    lambda d: d["provider"].update(lifecycle_command_timeout_ns=0),
    lambda d: d["provider"].update(lifecycle_command_timeout_ns="5"),
    lambda d: d["provider"].update(provider_manifest_revision="rev.1"),
    lambda d: d["provider"].update(extra=True),
    lambda d: d["provider"].pop("signal_configuration_digest"),
    lambda d: d["active_control_configuration"].update(source_runtime_unit_id="foreign"),
    lambda d: d["active_control_configuration"].update(source_runtime_epoch="foreign"),
    lambda d: d["active_control_configuration"].update(signal_configuration_digest="sha256:" + "9" * 64),
    lambda d: d["active_control_configuration"].update(source_sequence=0),
    lambda d: d["active_control_configuration"].update(activation_set_id="set.1"),
    lambda d: d["provider"].update(controllable_component_id="component.foreign"),
    lambda d: d["provider"].update(provider_instance_id="provider.foreign"),
    lambda d: d.update(provider_runtime_epoch="epoch.foreign"),
])
def test_provider_domain_physical_or_malformed_shape_is_refused(mutate) -> None:
    snapshot = _provider_domain_snapshot()
    mutate(snapshot["domains"][0])
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


def test_provider_domain_requires_exactly_its_paired_occurrence() -> None:
    snapshot = _provider_domain_snapshot()
    snapshot["provider_occurrences"] = []
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)
    snapshot = _provider_domain_snapshot()
    snapshot["provider_occurrences"][0]["lifecycle_plan_catalog_digest"] = "sha256:" + "9" * 64
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


@pytest.mark.parametrize("field", ["current_control_bindings", "resources"])
def test_controller_domain_without_physical_scope_is_still_refused(field: str) -> None:
    snapshot = _target_snapshot()
    snapshot["domains"][0][field] = []
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


@pytest.mark.parametrize("mutate", [
    lambda d: d.pop("kind"),
    lambda d: d.update(kind=None),
    lambda d: d.update(kind="io"),
    lambda d: d.update(kind="provider"),
    lambda d: d.update(kind="io_provider"),
    lambda d: d.pop("resources"),
    lambda d: d.update(current_resources=d.pop("resources")),
    lambda d: d.update(current_resources=["joint.1"]),
    lambda d: d.update(output_resources=["joint.1"]),
])
def test_controller_domain_requires_kind_and_common_resources(mutate) -> None:
    snapshot = _target_snapshot()
    mutate(snapshot["domains"][0])
    with pytest.raises(HeadlessApiError, match="readiness is invalid"):
        _validate_component_readiness(snapshot)


def test_q114_preserves_blockers_and_exact_health_values() -> None:
    snapshot = _target_snapshot()
    snapshot["readiness"]["evaluated_at_ns"] = str(2**63 - 1)
    _validate_component_readiness(snapshot)
    facts = snapshot["readiness"]["domains"][0]["health"]["facts"]
    for value in ({"kind": "unknown"}, {"kind": "boolean", "boolean_value": False},
                  {"kind": "integer", "integer_value": -(2**53 - 1)}, {"kind": "token", "token_value": "unknown"}):
        facts[0]["value"] = value
        _validate_component_readiness(snapshot)
    snapshot["component_lifecycle_state"] = "active"
    snapshot["readiness"].update(next_intent=None, requested_state=None)
    _validate_component_readiness(snapshot)
    snapshot["readiness"]["domains"].append(snapshot["readiness"]["domains"][0])
    with pytest.raises(HeadlessApiError):
        _validate_component_readiness(snapshot)


def test_q114_retains_general_error_and_normalized_health_bounds() -> None:
    snapshot = _target_snapshot()
    error = {"code": "measured_source_unavailable", "message": "m" * 1024,
        "details": {f"fact.{index}": f"actual.{index}" for index in range(64)}}
    snapshot["readiness"]["error"] = error
    snapshot["readiness"]["measured_state"] = {"required": True, "available": False, "error": error}
    snapshot["readiness"]["domains"][0]["error"] = error
    _validate_component_readiness(snapshot)
    error["details"]["extra"] = "actual"
    with pytest.raises(HeadlessApiError):
        _validate_component_readiness(snapshot)
    del error["details"]["extra"]
    health_error = {"code": "provider_diagnostic", "message": "m" * 1024,
        "details": {f"fact.{index}": f"actual.{index}" for index in range(8)}}
    snapshot["readiness"]["domains"][0]["health"]["diagnostic"] = health_error
    _validate_component_readiness(snapshot)
    health_error["details"]["extra"] = "actual"
    with pytest.raises(HeadlessApiError):
        _validate_component_readiness(snapshot)


@pytest.mark.parametrize("command,method", [("lifecycle_submit", "submit_lifecycle"),
                                          ("lifecycle_request_result", "lifecycle_request_result")])
def test_close_incident_recovery_preserves_exact_incident_and_reason(tmp_path, command, method):
    intent = _lifecycle_request(confirmed=True)
    intent.pop("confirmed")
    intent.update(intent="recover", reason="cell closed by operator",
                  close_incident_recovery={"expected_incident_id": "incident.component.arm.5000"})
    result = _incident_recovery_result()
    result["incident_recovery"]["closure_reason"] = "cell_closed_by_operator"
    observed = []
    assert _through_client(tmp_path, command, result,
        lambda client: getattr(client, method)(intent), observed) == result
    assert observed[0]["payload"] == intent


@pytest.mark.parametrize("mutation", ["foreign_incident", "missing_reason", "wrong_reason", "ordinary_fallback"])
def test_close_incident_recovery_refuses_a_different_acknowledgement(tmp_path, mutation):
    intent = _lifecycle_request(confirmed=True)
    intent.pop("confirmed")
    intent.update(intent="recover", reason="cell closed by operator",
                  close_incident_recovery={"expected_incident_id": "incident.component.arm.5000"})
    result = _incident_recovery_result()
    result["incident_recovery"]["closure_reason"] = "cell_closed_by_operator"
    if mutation == "foreign_incident": result["incident_recovery"]["incident_id"] = "incident.other"
    elif mutation == "missing_reason": del result["incident_recovery"]["closure_reason"]
    elif mutation == "wrong_reason": result["incident_recovery"]["closure_reason"] = "other"
    elif mutation == "ordinary_fallback": del result["incident_recovery"]
    with pytest.raises(HeadlessApiError):
        _through_client(tmp_path, "lifecycle_submit", result, lambda client: client.submit_lifecycle(intent))


@pytest.mark.parametrize("intent,reason", [("disable", "cell closed by operator"),
                                         ("recover", "ordinary recovery")])
def test_close_incident_recovery_http_refuses_non_close_intent_without_effect(intent, reason):
    fake = FakeCCLC()
    service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    client = TestClient(create_app(service=service, cclc=fake))
    body = _lifecycle_request(confirmed=True)
    body.update(intent=intent, reason=reason,
                close_incident_recovery={"expected_incident_id": "incident.1"})
    assert client.post("/api/cclc/lifecycle/requests", json=body).status_code == 422
    assert fake.lifecycle == []


def test_process_stop_http_keeps_scope_and_result_read_separate():
    from manyforge_headless_api.tests.test_manager import _process_stop_request, _process_stop_response
    request = _process_stop_request()
    body = {"request": request.model_dump(mode="json")}
    class Manager:
        def __init__(self): self.stops = []; self.reads = []
        def stop_component_process(self, request):
            self.stops.append(request)
            return _process_stop_response(request, "pending")["result"]
        def component_process_stop_result(self, request):
            self.reads.append(request)
            return _process_stop_response(request)["result"]
    manager = Manager()
    service = HeadlessService(object(), manager, cell_admission=FakeAdmission())
    client = TestClient(create_app(service=service))
    bad = client.post("/api/cclc/components/other/process/stop", json=body)
    assert bad.status_code == 400
    assert manager.stops == []
    assert client.post("/api/cclc/components/other/process/stop/result", json=body).status_code == 400
    assert manager.reads == []
    submitted = client.post("/api/cclc/components/arm/process/stop", json=body)
    assert submitted.status_code == 200
    assert submitted.json()["state"] == "pending"
    read = client.post("/api/cclc/components/arm/process/stop/result", json=body)
    assert read.status_code == 200
    assert read.json()["receipt"]["leader_pid"] == request.leader_pid
    assert manager.stops == [request] and manager.reads == [request]
    assert "confirmed" not in submitted.json()["request"]


@pytest.mark.parametrize("method,command", [("submit_lifecycle", "lifecycle_submit"), ("lifecycle_request_result", "lifecycle_request_result")])
@pytest.mark.parametrize("replay", ["stale_request_owner_epoch", "request_id_conflict", "request_ledger_full", "request_result_expired"])
def test_close_preserves_typed_admission_refusals_without_acknowledgement(tmp_path, method, command, replay):
    intent = _lifecycle_request(confirmed=True)
    intent.update(intent="recover", reason="cell closed by operator", close_incident_recovery={"expected_incident_id": "incident.component.arm.5000"})
    result = {"acceptance": None, "replay_status": replay, "status": None}
    assert _through_client(tmp_path, command, result, lambda client: getattr(client, method)(intent)) == result
    ack = _incident_recovery_result(replay)
    ack["incident_recovery"]["closure_reason"] = "cell_closed_by_operator"
    with pytest.raises(HeadlessApiError):
        _through_client(tmp_path, command, ack, lambda client: getattr(client, method)(intent))


@pytest.mark.parametrize("method,command", [("submit_lifecycle", "lifecycle_submit"), ("lifecycle_request_result", "lifecycle_request_result")])
def test_ordinary_recover_cannot_acknowledge_close(tmp_path, method, command):
    intent = _lifecycle_request(confirmed=True); intent.update(intent="recover")
    result = _incident_recovery_result(); result["incident_recovery"]["closure_reason"] = "cell_closed_by_operator"
    with pytest.raises(HeadlessApiError):
        _through_client(tmp_path, command, result, lambda client: getattr(client, method)(intent))


@pytest.mark.parametrize("incident", ["", "x" * 129, "e\u0301", "é" * 65])
def test_close_http_rejects_invalid_incident_identity_before_effects(incident):
    fake = FakeCCLC(); service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    body = _lifecycle_request(confirmed=True)
    body.update(intent="recover", reason="cell closed by operator", close_incident_recovery={"expected_incident_id": incident})
    with TestClient(create_app(service=service, cclc=fake)) as client:
        assert client.post("/api/cclc/lifecycle/requests", json=body).status_code == 422
    assert fake.lifecycle == []


def test_close_incident_refusal_keeps_409_code():
    class Port(FakeCCLC):
        def submit_lifecycle(self, intent):
            raise HeadlessApiError("component_lifecycle_close_incident_mismatch", "Incident changed", details={"actual": "incident.other"})
    service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    body = _lifecycle_request(confirmed=True)
    body.update(intent="recover", reason="cell closed by operator", close_incident_recovery={"expected_incident_id": "incident.1"})
    with TestClient(create_app(service=service, cclc=Port())) as client:
        response = client.post("/api/cclc/lifecycle/requests", json=body)
    assert response.status_code == 409
    assert response.json()["detail"]["code"] == "component_lifecycle_close_incident_mismatch"


def _lifecycle_point_snapshot() -> dict[str, Any]:
    return {key: value for key, value in _target_snapshot().items()
            if key not in {"readiness", "lifecycle_target", "lifecycle_progress"}}


def test_lifecycle_point_client_uses_closed_snapshot_without_readiness(tmp_path: Path) -> None:
    observed = []
    snapshot = _lifecycle_point_snapshot()
    thread = _serve_once(tmp_path, lambda request: (
        observed.append(request) or rfc8785.dumps(_response("component_lifecycle_snapshot", snapshot))))
    try:
        result = CCLCOpenRequestClient(tmp_path).component_lifecycle_snapshot("component.arm")
        assert result == snapshot
        assert observed == [{"api_version": "manyforge.cclc_open_request.v1",
            "command": "component_lifecycle_snapshot", "payload": {"controllable_component_id": "component.arm"}}]
    finally:
        thread.join(timeout=2)


@pytest.mark.parametrize("change", ["foreign", "extra", "bool_generation", "bad_domains"])
def test_lifecycle_point_client_rejects_foreign_or_malformed_snapshot(tmp_path: Path, change: str) -> None:
    snapshot = _lifecycle_point_snapshot()
    if change == "foreign": snapshot["controllable_component_id"] = "component.other"
    elif change == "extra": snapshot["readiness"] = {}
    elif change == "bool_generation": snapshot["component_lifecycle_generation"] = True
    else: snapshot["domains"] = []
    thread = _serve_once(tmp_path, lambda _request: rfc8785.dumps(_response("component_lifecycle_snapshot", snapshot)))
    try:
        with pytest.raises(HeadlessApiError):
            CCLCOpenRequestClient(tmp_path).component_lifecycle_snapshot("component.arm")
    finally:
        thread.join(timeout=2)


def test_http_lifecycle_point_read_preserves_snapshot_and_error_details() -> None:
    class PointCCLC(FakeCCLC):
        def __init__(self):
            super().__init__()
            self.reads = []
            self.error = None
        def component_lifecycle_snapshot(self, component_id):
            self.reads.append(component_id)
            if self.error is not None: raise self.error
            return _lifecycle_point_snapshot()
        def component_snapshot(self, _component_id):
            pytest.fail("point read used full readiness")
    port = PointCCLC()
    service = HeadlessService(object(), object(), cell_admission=FakeAdmission())
    with TestClient(create_app(service=service, cclc=port)) as client:
        response = client.get("/api/cclc/components/component.arm/lifecycle-snapshot")
        assert response.status_code == 200
        assert response.json() == _lifecycle_point_snapshot()
        details = {"snapshot_changed": "true", "component_generation_before": "4", "component_generation_after": "5"}
        port.error = HeadlessApiError("component_lifecycle_facts_stale", "actual component changed", details=details)
        refused = client.get("/api/cclc/components/component.arm/lifecycle-snapshot")
        assert refused.status_code == 409
        assert refused.json()["detail"]["details"] == details
    assert port.reads == ["component.arm", "component.arm"]
    assert port.lifecycle == []

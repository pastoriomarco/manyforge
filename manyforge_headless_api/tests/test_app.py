from __future__ import annotations

from typing import Any, Sequence

import pytest
from fastapi.testclient import TestClient

from manyforge_headless_api.app import TrustedPrincipal, create_app
from manyforge_headless_api.models import ManagerApplyRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService


def _manager_payload() -> dict[str, Any]:
    return {
        "api_version": "manyforge.manager_source.v1",
        "active": {
            "cell_graph": {"cell_id": "cell-a", "components": []},
            "graph_revision": "graph.1",
            "retained_bundle_id": "bundle.1",
            "publication": {},
        },
        "apply": {
            "mutation_available": False,
            "records": [],
            "unavailable_reason_code": "kernel_owner_unavailable",
        },
        "cell_id": "cell-a",
        "manager_ready": True,
        "runtime": {
            "converged": True,
            "desired_units": [],
            "observed_units": [],
        },
    }


def _section(owner: str, payload: dict[str, Any]) -> dict[str, Any]:
    return {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "payload": payload,
        "projection_generation": 4,
        "receiver_age_ns": 10,
        "source_content_digest": "sha256:" + "a" * 64,
        "source_generation": 2,
        "source_observed_at_ns": 1,
        "source_owner_epoch": "epoch.1",
        "source_owner_id": owner,
        "source_owner_kind": "test",
    }


class FakeCore:
    def __init__(self) -> None:
        self.acknowledgements: list[dict[str, Any]] = []
        self.historian_reconciliations: list[dict[str, Any]] = []

    def runtime_snapshot(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.headless_runtime_snapshot.v1",
            "complete": True,
            "projection_generation": 4,
            "projection_owner_epoch": "projection.epoch.1",
            "reasons": [],
            "sections": {
                "manager": _section("component-manager.cell-a", _manager_payload()),
                "logical_scene": _section(
                    "logical-scene.cell-a",
                    {"api_version": "manyforge.logical_scene_source.v1"},
                ),
                "scene_runtime": _section(
                    "scene-runtime.cell-a",
                    {"api_version": "manyforge.scene_runtime_source.v1"},
                ),
            },
        }

    def node_environment_projection(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.node_environment_projection.v1",
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "source": {
                "available": True,
                "freshness": "fresh",
                "known_gaps": [],
                "projection_generation": 1,
                "projection_owner_epoch": "projection.epoch.1",
                "receiver_age_ns": 10,
                "source_content_digest": "sha256:" + "b" * 64,
                "source_generation": 1,
                "source_owner_epoch": "projection.epoch.1",
                "source_owner_id": "headless-api.node-environment",
            },
        }

    def alarm_snapshot(
        self,
        limit: int | None = None,
        states: Sequence[str] = (),
        ordering: str = "default",
    ) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.operator_alarm_snapshot.v2",
            "owner_epoch": "alarm.epoch.1",
            "generation": 3,
            "reconciled": True,
            "health": {"state": "available", "details": {}},
            "alarms": [],
            "requested_limit": limit,
            "requested_states": list(states),
            "requested_ordering": ordering,
        }

    def alarm_metrics(self) -> dict[str, Any]:
        return {"schema_version": "manyforge.operator_alarm_metrics.v1"}

    def acknowledge_alarm(self, **kwargs: Any) -> dict[str, Any]:
        self.acknowledgements.append(kwargs)
        return {"schema_version": "manyforge.headless_api_mutation_response.v1"}

    def acknowledge_all(self, **kwargs: Any) -> dict[str, Any]:
        self.acknowledgements.append(kwargs)
        return {"schema_version": "manyforge.headless_api_mutation_response.v1"}

    def history(self, **kwargs: Any) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_page.v1",
            "events": [],
            "query": kwargs,
        }

    def historian_health(self) -> dict[str, Any]:
        return {
            "schema_version": "manyforge.historian_health.v1",
            "available": True,
        }

    def reconcile_historian_health(self, **kwargs: Any) -> dict[str, Any]:
        self.historian_reconciliations.append(kwargs)
        return {
            "schema_version": "manyforge.historian_reconciliation.v1",
            "reconciled": kwargs["expected_generation"] == 1,
            "audit_event_ids": ["event-1"],
            "health": self.historian_health(),
        }

    def shutdown(self) -> None:
        raise AssertionError("injected service must not be shut down by the app")


class FakeManager:
    def __init__(self) -> None:
        self.requests: list[ManagerApplyRequest] = []

    def apply(self, request: ManagerApplyRequest) -> dict[str, Any]:
        self.requests.append(request)
        return {
            "api_version": "manyforge.manager_command_response.v1",
            "ok": False,
            "error": {
                "code": "kernel_owner_unavailable",
                "message": "operational Apply returns at I6/N",
            },
            "request_id": request.request_id,
            "request_owner_epoch": request.request_owner_epoch,
        }


class FakeExecution:
    def __init__(self) -> None:
        self.requests: list[tuple[str, object]] = []
        self.principals: list[tuple[str, str | None]] = []

    def status(self) -> dict[str, Any]:
        return {"ok": True, "command": "status", "status": {}}

    def submit(
        self,
        request: object,
        *,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append(("submit", request))
        self.principals.append(("submit", trusted_principal_id))
        return {"ok": True, "command": "submit", "accepted": {}}

    def read(self, operation_id: str) -> dict[str, Any]:
        return {
            "ok": True,
            "command": "read",
            "operation": {"operation_id": operation_id},
        }

    def _control(
        self,
        kind: str,
        request: object,
        trusted_principal_id: str | None,
    ) -> dict[str, Any]:
        self.requests.append((kind, request))
        self.principals.append((kind, trusted_principal_id))
        return {"ok": True, "command": kind, "accepted": {"control": kind}}

    def pause(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("pause", request, trusted_principal_id)

    def resume(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("resume", request, trusted_principal_id)

    def reset(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control("reset", request, trusted_principal_id)

    def acknowledge_completed(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        return self._control(
            "acknowledge_completed", request, trusted_principal_id
        )

    def stop(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        self.requests.append(("stop", request))
        self.principals.append(("stop", trusted_principal_id))
        return {"ok": True, "command": "stop", "accepted": {}}

    def set_runtime_scale(
        self, request: object, *, trusted_principal_id: str | None
    ) -> dict[str, Any]:
        self.requests.append(("set_runtime_scale", request))
        self.principals.append(("set_runtime_scale", trusted_principal_id))
        return {"ok": True, "command": "set_runtime_scale", "accepted": {}}

    def get_runtime_scale(self, operation_id: str) -> dict[str, Any]:
        self.requests.append(("get_runtime_scale", operation_id))
        return {
            "ok": True,
            "command": "get_runtime_scale",
            "runtime_scale": {"operation_id": operation_id},
        }


class FakeTrustedAdapter:
    def resolve(self, _request) -> TrustedPrincipal:
        return TrustedPrincipal("operator.7", "authenticated:test")


def _client(*, trusted: bool = False) -> tuple[TestClient, FakeCore, FakeManager]:
    core = FakeCore()
    manager = FakeManager()
    app = create_app(
        service=HeadlessService(core, manager),
        trusted_principal_adapter=FakeTrustedAdapter() if trusted else None,
    )
    return TestClient(app), core, manager


def _caller() -> dict[str, Any]:
    return {
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": "correlation.1",
    }


def test_runtime_manager_scene_alarm_history_and_health_are_thin_reads() -> None:
    client, _core, _manager = _client()
    with client:
        assert client.get("/api/runtime").json()["complete"] is True
        assert client.get("/api/manager/graph").json()["graph_revision"] == "graph.1"
        assert client.get("/api/manager/status").json()["status"]["manager_ready"]
        assert client.get("/api/manager/diff").json()["converged"]
        environment = client.get("/api/node-environment").json()
        assert environment["retained_bundle_id"] == "bundle.1"
        assert environment["source"]["source_owner_id"] == (
            "headless-api.node-environment"
        )
        assert client.get("/api/scene").status_code == 200
        assert client.get("/api/execution/alarms").json()[
            "requested_limit"
        ] == 32
        alarms = client.get(
            "/api/execution/alarms",
            params=[
                ("limit", "2"),
                ("states", "active_unacknowledged"),
                ("ordering", "attention_newest"),
            ],
        ).json()
        assert alarms["requested_states"] == ["active_unacknowledged"]
        assert alarms["requested_ordering"] == "attention_newest"
        assert client.get("/api/execution/history?limit=3").status_code == 200
        health = client.get("/api/health").json()
        assert health["status_scope"] == "headless_projection"
        assert health["headless_projection_ready"] is True
        assert "ready" not in health
        assert health["robot_readiness"] == {
            "available": False,
            "state": None,
            "source_owner_id": None,
            "reason_code": "robot_readiness_owner_unavailable",
        }


def test_acknowledgement_stamps_open_transport_and_rejects_principal_body() -> None:
    client, core, _manager = _client()
    body = {
        **_caller(),
        "activation_id": "activation.1",
        "expected_record_revision": 2,
        "reason": None,
        "confirmed": True,
    }
    with client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json=body,
        )
        assert response.status_code == 200
        assert core.acknowledgements[-1]["transport_kind"] == "http"
        assert core.acknowledgements[-1]["assurance"] == "unverified"
        assert core.acknowledgements[-1]["trusted_principal_id"] is None

        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={**body, "trusted_principal_id": "forged"},
        )
        assert response.status_code == 422


def test_configured_auth_adapter_alone_can_stamp_trusted_principal() -> None:
    client, core, _manager = _client(trusted=True)
    with client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={
                **_caller(),
                "activation_id": "activation.1",
                "expected_record_revision": 2,
                "reason": None,
                "confirmed": True,
            },
        )
    assert response.status_code == 200
    assert core.acknowledgements[-1]["trusted_principal_id"] == "operator.7"
    assert core.acknowledgements[-1]["assurance"] == "authenticated:test"


def test_bulk_acknowledgement_uses_one_common_owner_epoch_fence() -> None:
    client, core, _manager = _client()
    with client:
        response = client.post(
            "/api/execution/alarms/acknowledge-all",
            json={
                **_caller(),
                "members": [],
                "expected_generation": 3,
                "reason": None,
                "confirmed": True,
            },
        )
    assert response.status_code == 200
    mutation = core.acknowledgements[-1]
    assert mutation["request_owner_epoch"] == "client.epoch.1"
    assert mutation["expected_generation"] == 3
    assert "expected_owner_epoch" not in mutation


def test_historian_reconciliation_is_an_attributed_public_mutation() -> None:
    client, core, _manager = _client(trusted=True)
    with client:
        response = client.post(
            "/api/execution/history/reconcile",
            json={**_caller(), "expected_health_generation": 1},
        )
    assert response.status_code == 200
    assert response.json()["reconciled"] is True
    mutation = core.historian_reconciliations[-1]
    assert mutation == {
        "expected_generation": 1,
        "request_id": "request.1",
        "request_owner_epoch": "client.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": "correlation.1",
        "transport_kind": "http",
        "adapter_id": "manyforge.headless.http.v1",
        "assurance": "authenticated:test",
        "trusted_principal_id": "operator.7",
    }


def test_apply_is_forwarded_and_remains_typed_unavailable_before_i6() -> None:
    client, _core, manager = _client()
    with client:
        response = client.post(
            "/api/manager/apply",
            json={
                **_caller(),
                "deployment_artifact_id": "deployment.1",
                "deployment_revision": "sha256:" + "b" * 64,
                "expected_current": {
                    "graph_revision": "graph.1",
                    "retained_bundle_id": "bundle.1",
                },
                "reject_if_affected_busy": True,
            },
        )
    assert response.status_code == 503
    assert response.json()["error"]["code"] == "kernel_owner_unavailable"
    assert manager.requests[0].source_label == "operator_ui"


def test_request_body_bound_is_enforced_before_model_decode() -> None:
    client, _core, _manager = _client()
    with client:
        response = client.post(
            "/api/manager/apply",
            content=b"x" * (1024 * 1024 + 1),
            headers={"content-type": "application/json"},
        )
    assert response.status_code == 413, response.text
    assert response.json()["detail"]["code"] == "headless_request_body_oversized"


def test_streamed_request_body_bound_is_enforced_without_content_length() -> None:
    client, _core, _manager = _client()

    def oversized_chunks():
        yield b"x" * (512 * 1024)
        yield b"y" * (512 * 1024)
        yield b"z"

    with client:
        response = client.post(
            "/api/manager/apply",
            content=oversized_chunks(),
            headers={"content-type": "application/json"},
        )
    assert response.status_code == 413, response.text
    assert response.json()["detail"]["code"] == "headless_request_body_oversized"


def test_query_cardinality_and_safe_integer_bounds_precede_native_conversion() -> None:
    client, _core, _manager = _client()
    with client:
        oversized_integer = client.get(
            "/api/execution/history",
            params={"before_sequence": 9_007_199_254_740_992},
        )
        excessive_states = client.get(
            "/api/execution/alarms",
            params=[("states", "active_unacknowledged")] * 5,
        )
        invalid_ordering = client.get(
            "/api/execution/alarms",
            params={"ordering": "priority_oldest"},
        )
        excessive_alarm_page = client.get(
            "/api/execution/alarms",
            params={"limit": 129},
        )
        excessive_history_page = client.get(
            "/api/execution/history",
            params={"limit": 33},
        )
    assert oversized_integer.status_code == 422
    assert excessive_states.status_code == 422
    assert invalid_ordering.status_code == 422
    assert excessive_alarm_page.status_code == 422
    assert excessive_history_page.status_code == 422


def test_openapi_contains_no_condition_mutation_route() -> None:
    client, _core, _manager = _client()
    with client:
        paths = client.get("/openapi.json").json()["paths"]
    assert "/api/execution/alarms/{condition_id}/acknowledge" in paths
    assert "/api/node-environment" in paths
    assert {
        "/api/execution/operations/{operation_id}/pause",
        "/api/execution/operations/{operation_id}/resume",
        "/api/execution/operations/{operation_id}/reset",
        "/api/execution/operations/{operation_id}/acknowledge-completed",
        "/api/execution/operations/{operation_id}/runtime-scale",
        "/api/execution/stops",
    }.issubset(paths)
    assert "/api/kernel/stop" not in paths
    assert "/api/cycle/stop" not in paths
    assert all("condition" not in path or "acknowledge" in path for path in paths)


def test_execution_missing_reference_is_a_public_not_found() -> None:
    class MissingExecution:
        def status(self) -> dict[str, Any]:
            raise AssertionError("not called")

        def submit(
            self,
            _request: object,
            *,
            trusted_principal_id: str | None,
        ) -> dict[str, Any]:
            raise AssertionError("not called")

        def read(self, _operation_id: str) -> dict[str, Any]:
            return {
                "api_version": "manyforge.execution_command_response.v1",
                "ok": False,
                "error": {
                    "code": "missing_reference",
                    "message": "operation is not resident",
                    "details": {"reason": "operation_not_resident"},
                },
            }

    app = create_app(
        service=HeadlessService(FakeCore(), FakeManager(), MissingExecution())
    )
    with TestClient(app) as client:
        response = client.get("/api/execution/operations/operation-retired")

    assert response.status_code == 404
    assert response.json()["error"]["code"] == "missing_reference"


def test_operation_controls_and_scale_are_thin_revision_fenced_routes() -> None:
    execution = FakeExecution()
    app = create_app(
        service=HeadlessService(FakeCore(), FakeManager(), execution)
    )
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }
    control = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": "operator request",
    }
    stop = {
        "request_context": context,
        "operations": [
            {
                "operation_id": "operation-1",
                "expected_operation_revision": 3,
                "expected_stop_generation": 0,
            }
        ],
        "reason": "operator Stop",
    }
    scale = {
        "request_context": context,
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "expected_runtime_scale_generation": 0,
        "scale": 0.25,
    }

    with TestClient(app) as client:
        for endpoint in ("pause", "resume", "reset", "acknowledge-completed"):
            response = client.post(
                f"/api/execution/operations/operation-1/{endpoint}",
                json=control,
            )
            assert response.status_code == 200, response.text
        assert client.post("/api/execution/stops", json=stop).status_code == 200
        assert client.post(
            "/api/execution/operations/operation-1/runtime-scale", json=scale
        ).status_code == 200
        observed = client.get(
            "/api/execution/operations/operation-1/runtime-scale"
        )
        assert observed.json()["runtime_scale"]["operation_id"] == "operation-1"

    assert [kind for kind, _request in execution.requests] == [
        "pause",
        "resume",
        "reset",
        "acknowledge_completed",
        "stop",
        "set_runtime_scale",
        "get_runtime_scale",
    ]
    assert execution.principals == [
        ("pause", None),
        ("resume", None),
        ("reset", None),
        ("acknowledge_completed", None),
        ("stop", None),
        ("set_runtime_scale", None),
    ]


def test_execution_mutation_principal_can_only_come_from_trusted_adapter() -> None:
    execution = FakeExecution()
    app = create_app(
        service=HeadlessService(FakeCore(), FakeManager(), execution),
        trusted_principal_adapter=FakeTrustedAdapter(),
    )
    body = {
        "request_context": {
            "request_id": "request.control.trusted",
            "request_owner_epoch": "execution.epoch.1",
            "source_label": "operator_ui",
            "correlation_id": None,
        },
        "operation_id": "operation-1",
        "expected_operation_revision": 3,
        "reason": None,
    }
    with TestClient(app) as client:
        forged = client.post(
            "/api/execution/operations/operation-1/pause",
            json={**body, "principal_id": "forged"},
        )
        forged_context = client.post(
            "/api/execution/operations/operation-1/pause",
            json={
                **body,
                "request_context": {
                    **body["request_context"],
                    "principal_id": "forged",
                },
            },
        )
        accepted = client.post(
            "/api/execution/operations/operation-1/pause", json=body
        )

    assert forged.status_code == 422
    assert forged_context.status_code == 422
    assert accepted.status_code == 200
    assert execution.principals == [("pause", "operator.7")]


def test_operation_control_paths_reject_ambiguous_or_unbounded_scope() -> None:
    execution = FakeExecution()
    app = create_app(
        service=HeadlessService(FakeCore(), FakeManager(), execution)
    )
    context = {
        "request_id": "request.control.1",
        "request_owner_epoch": "execution.epoch.1",
        "source_label": "operator_ui",
        "correlation_id": None,
    }
    with TestClient(app) as client:
        mismatch = client.post(
            "/api/execution/operations/operation-1/pause",
            json={
                "request_context": context,
                "operation_id": "operation-2",
                "expected_operation_revision": 1,
                "reason": None,
            },
        )
        broad_stop = client.post(
            "/api/execution/stops",
            json={
                "request_context": context,
                "operations": [
                    {
                        "operation_id": operation_id,
                        "expected_operation_revision": 1,
                        "expected_stop_generation": 0,
                    }
                    for operation_id in ("operation-1", "operation-2")
                ],
                "reason": None,
            },
        )
        invalid_scale = client.post(
            "/api/execution/operations/operation-1/runtime-scale",
            json={
                "request_context": context,
                "operation_id": "operation-1",
                "expected_operation_revision": 1,
                "expected_runtime_scale_generation": 0,
                "scale": 1.01,
            },
        )

    assert mismatch.status_code == 400
    assert mismatch.json()["detail"]["code"] == (
        "execution_operation_path_invalid"
    )
    assert broad_stop.status_code == 422
    assert invalid_scale.status_code == 422
    assert execution.requests == []


@pytest.mark.parametrize(
    ("code", "expected_status"),
    [
        ("operator_alarm_acknowledgement_confirmation_required", 400),
        ("operator_alarm_acknowledgement_reason_required", 400),
        ("operator_alarm_acknowledge_all_confirmation_required", 400),
        ("operator_alarm_membership_mismatch", 409),
    ],
)
def test_alarm_owner_rejections_preserve_their_public_http_class(
    code: str,
    expected_status: int,
) -> None:
    core = FakeCore()

    def reject(**_kwargs: Any) -> dict[str, Any]:
        raise HeadlessApiError(code, "injected owner rejection")

    core.acknowledge_alarm = reject  # type: ignore[method-assign]
    app = create_app(service=HeadlessService(core, FakeManager()))
    with TestClient(app) as client:
        response = client.post(
            "/api/execution/alarms/condition-aaaaaaaaaaaaaaaaaaaaaaaa/acknowledge",
            json={
                **_caller(),
                "activation_id": "activation.1",
                "expected_record_revision": 1,
                "reason": None,
                "confirmed": True,
            },
        )

    assert response.status_code == expected_status
    assert response.json()["detail"]["code"] == code

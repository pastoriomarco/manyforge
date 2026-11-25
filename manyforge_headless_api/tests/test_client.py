from __future__ import annotations

import hashlib
import json
from pathlib import Path
from urllib.parse import parse_qsl

import httpx
import pytest

from manyforge_headless_api.client import (
    MAX_HTTP_RESPONSE_BYTES,
    HeadlessClientError,
    HeadlessHttpClient,
    outbound_headers,
)
from manyforge_headless_api.native import (
    MAX_NATIVE_RESPONSE_BYTES,
    MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES,
    HeadlessApiError,
    NativeHeadlessCore,
)
from manyforge_behavior.canonical_artifact import compute_content_identity


_BINDING_FIXTURE = (
    Path(__file__).resolve().parents[2]
    / "manyforge_deployment/resources/program_binding_lite6_native.fixture.json"
)


def _client_binding() -> dict[str, object]:
    binding = json.loads(_BINDING_FIXTURE.read_text(encoding="utf-8"))
    binding.update(
        {
            "program_binding_id": "sha256:" + "0" * 64,
            "program_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": "sha256:" + "b" * 64,
            "graph_revision": "sha256:" + "c" * 64,
        }
    )
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v4",
    )
    return binding


def _client_applied() -> dict[str, object]:
    binding = _client_binding()
    return {
        "assistant_mode": "forbidden",
        "assistant_projection": None,
        "baseline_program_binding_id": "sha256:" + "d" * 64,
        "deployment_revision": "sha256:" + "e" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
        "owner_identities": [{
            "role": "headless_projection",
            "application_start_id": "3f9a0000000000a1",
        }],
        "preparation_digest": "sha256:" + "f" * 64,
        "program": {
            "program_revision": "sha256:" + "a" * 64,
        },
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_revision": "sha256:" + "a" * 64,
        "retained_bundle_id": binding["retained_bundle_id"],
        "source_document_digest": "sha256:" + "1" * 64,
        "scenario_id": "cell-pick",
        "project_path": "/projects/cell-pick.scenario",
        "schema_version": "manyforge.applied_program_selection.v5",
        "simulation_scene": None,
        "program_frame_facts": {
            "applied_record_digest": "sha256:" + "2" * 64,
            "program_binding_id": binding["program_binding_id"],
            "program_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": binding["retained_bundle_id"],
            "graph_revision": binding["graph_revision"],
            "components": [],
            "frames": [],
        },
        "source": {
            "record_digest": "sha256:" + "2" * 64,
            "semantic_content_digest": "sha256:" + "3" * 64,
            "source_generation": 1,
            "source_owner_id": "owner.cell-a",
        },
    }


def _sized_context_payload(size: int, *, native: bool = False) -> bytes:
    prefix = b'{"ok":true,"value":' if native else b""
    prefix += (
        b'{"schema_version":"manyforge.source_model_context.v1",'
        b'"node_environment":{"schema_version":"manyforge.'
        b'node_environment_projection.v2"},'
        b'"source_model":{"schema_version":"manyforge.source_model_projection.v4",'
        b'"padding":"'
    )
    suffix = b'"}}' + (b"}" if native else b"")
    padding_size = size - len(prefix) - len(suffix)
    assert padding_size >= 0
    return prefix + (b"x" * padding_size) + suffix


class _NativeResponseFixture:
    def __init__(self, payload: str) -> None:
        self.payload = payload

    def source_model_context(self) -> str:
        return self.payload

    def runtime_snapshot(self) -> str:
        return self.payload


def _native_fixture(payload: bytes) -> NativeHeadlessCore:
    core = object.__new__(NativeHeadlessCore)
    core._native = _NativeResponseFixture(payload.decode("utf-8"))
    return core


def test_client_returns_bounded_strict_json_object() -> None:
    transport = httpx.MockTransport(
        lambda request: httpx.Response(
            200,
            json={"schema_version": "manyforge.headless_runtime_snapshot.v3"},
            request=request,
        )
    )
    raw = httpx.Client(base_url="http://headless.test", transport=transport)
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.runtime()["schema_version"].endswith("snapshot.v3")
    finally:
        raw.close()


def test_client_relays_bounded_owner_json_without_reencoding() -> None:
    payloads = {
        "/api/execution/history": b'{"schema_version":"history.v1"}',
    }
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, content=payloads[request.url.path], request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert (
            client.history_payload(params=(("limit", 32),))
            == payloads["/api/execution/history"]
        )
    finally:
        raw.close()
    assert requests[0].url.params["limit"] == "32"


def test_client_raw_json_relay_preserves_typed_owner_errors() -> None:
    transport = httpx.MockTransport(
        lambda request: httpx.Response(
            503,
            json={
                "detail": {
                    "code": "historian_unavailable",
                    "message": "durable history is unavailable",
                }
            },
            request=request,
        )
    )
    raw = httpx.Client(base_url="http://headless.test", transport=transport)
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with pytest.raises(HeadlessClientError) as error:
            client.history_payload()
    finally:
        raw.close()
    assert error.value.code == "historian_unavailable"
    assert error.value.status_code == 503


def test_client_uses_the_dedicated_node_environment_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.node_environment_projection.v4"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.node_environment()["schema_version"].endswith("projection.v4")
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/node-environment"]


def test_client_sends_runtime_scale_selector_as_query_parameters() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={
                "api_version": "manyforge.execution_api.v1",
                "command": "get_runtime_scale_control",
                "ok": True,
                "runtime_scale_control": {},
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    selector = {
        "program_binding_id": "sha256:" + "a" * 64,
        "component_id": "robot",
        "command_target_id": "arm",
    }
    try:
        assert client.runtime_scale_control(selector)["ok"] is True
    finally:
        raw.close()
    assert len(requests) == 1
    assert requests[0].url.path == "/api/execution/runtime-scale-control"
    assert dict(requests[0].url.params) == selector


def test_client_sends_group_runtime_scale_selector_as_one_json_query_parameter() -> (
    None
):
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"ok": True}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(handler)
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    selector = {
        "program_binding_id": "binding-1",
        "group_targets": [
            {"component_id": "robot-a", "command_target_id": "arm"},
            {"component_id": "robot-b", "command_target_id": "arm"},
        ],
    }
    try:
        client.runtime_scale_control(selector)
    finally:
        raw.close()
    assert dict(requests[0].url.params)["program_binding_id"] == "binding-1"
    assert (
        json.loads(dict(requests[0].url.params)["group_targets"])
        == selector["group_targets"]
    )


def test_client_uses_the_dedicated_source_model_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.source_model_projection.v4"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.source_model()["schema_version"].endswith("projection.v4")
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/source-model"]


def test_client_uses_the_atomic_source_model_context_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={
                "schema_version": "manyforge.source_model_context.v1",
                "node_environment": {},
                "source_model": {},
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert (
            client.source_model_context()["schema_version"]
            == "manyforge.source_model_context.v1"
        )
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/source-model-context"]


def test_client_uses_the_atomic_assistant_mode_context_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={
                "assistant_mode": {},
                "node_environment": {},
                "schema_version": "manyforge.assistant_mode_context.v1",
                "source_model": {},
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert (
            client.assistant_mode_context("composer-assistant")["schema_version"]
            == "manyforge.assistant_mode_context.v1"
        )
    finally:
        raw.close()
    assert [request.url.path for request in requests] == [
        "/api/assistant/modes/composer-assistant/context"
    ]


def test_context_http_bound_is_method_specific() -> None:
    payload = _sized_context_payload(MAX_HTTP_RESPONSE_BYTES + 1)

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, content=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert (
            client.source_model_context()["schema_version"]
            == "manyforge.source_model_context.v1"
        )
        with pytest.raises(HeadlessClientError) as error:
            client.runtime()
    finally:
        raw.close()
    assert error.value.code == "headless_http_response_oversized"


def test_context_http_bound_rejects_payload_over_context_limit() -> None:
    payload = _sized_context_payload(MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES + 1)

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, content=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with pytest.raises(HeadlessClientError) as error:
            client.source_model_context()
    finally:
        raw.close()
    assert error.value.code == "headless_http_response_oversized"


def test_context_native_bound_is_method_specific() -> None:
    core = _native_fixture(
        _sized_context_payload(MAX_NATIVE_RESPONSE_BYTES + 1, native=True)
    )
    assert (
        core.source_model_context()["schema_version"]
        == "manyforge.source_model_context.v1"
    )
    with pytest.raises(HeadlessApiError) as error:
        core.runtime_snapshot()
    assert error.value.code == "headless_projection_response_oversized"


def test_context_native_bound_rejects_payload_over_context_limit() -> None:
    core = _native_fixture(
        _sized_context_payload(
            MAX_SOURCE_MODEL_CONTEXT_RESPONSE_BYTES + 1,
            native=True,
        )
    )
    with pytest.raises(HeadlessApiError) as error:
        core.source_model_context()
    assert error.value.code == "headless_projection_response_oversized"


def test_client_uses_exact_program_selection_and_run_routes() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        if request.url.path == "/api/program/applied":
            payload = _client_applied()
        else:
            payload = {"ok": True}
        return httpx.Response(200, json=payload, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        client.program_applied()
        client.program_runs(active_only=True, limit=7)
        client.program_run_start({"request_key": "11111111111111111111111111111111"})
        client.program_run(1, application_start_id="3f9a0000000000a1")
        client.program_run_resume_halt(
            1,
            {"request_key": "22222222222222222222222222222222"},
        )
        client.program_run_motion_stop(
            1,
            {"request_key": "33333333333333333333333333333333"},
        )
    finally:
        raw.close()

    assert [request.url.path for request in requests] == [
        "/api/program/applied",
        "/api/program/runs",
        "/api/program/runs",
        "/api/program/runs/1",
        "/api/program/runs/1/resume-halt",
        "/api/program/runs/1/motion-stop",
    ]
    assert dict(requests[1].url.params) == {
        "active_only": "true",
        "limit": "7",
    }
    assert dict(requests[3].url.params) == {"application_start_id": "3f9a0000000000a1"}
    assert json.loads(requests[2].read()) == {"request_key": "11111111111111111111111111111111"}
    assert json.loads(requests[4].read()) == {"request_key": "22222222222222222222222222222222"}
    assert json.loads(requests[5].read()) == {"request_key": "33333333333333333333333333333333"}


def test_client_uses_the_revision_fenced_scene_pose_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={
                "api_version": "manyforge.scene_resource_pose_response.v1",
                "poses": [],
                "projection_revision": {},
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    body = {
        "api_version": "manyforge.scene_resource_pose_query.v1",
        "expected_projection_revision": {},
        "scene_resource_ids": ["part.1"],
    }
    try:
        assert client.resolve_scene_resources(body)["poses"] == []
    finally:
        raw.close()
    assert [request.method for request in requests] == ["POST"]
    assert [request.url.path for request in requests] == [
        "/api/scene/resource-poses/query"
    ]
    assert json.loads(requests[0].read()) == body


def _viewer_headers(
    *,
    kind: str,
    bundle_id: str,
    payload: bytes,
) -> dict[str, str]:
    return {
        "content-type": "application/xml" if kind == "urdf" else "model/stl",
        "x-manyforge-asset-kind": kind,
        "x-manyforge-cell-id": "cell-a",
        "x-manyforge-content-digest": (f"sha256:{hashlib.sha256(payload).hexdigest()}"),
        "x-manyforge-graph-revision": "graph.1",
        "x-manyforge-product-contract-id": "manyforge.viewer_model",
        "x-manyforge-product-contract-revision": "sha256:" + "2" * 64,
        "x-manyforge-product-id": "cell_viewer_model",
        "x-manyforge-retained-bundle-id": bundle_id,
        "x-manyforge-viewer-model-revision": "sha256:" + "3" * 64,
    }


def test_client_reads_exact_bounded_viewer_assets() -> None:
    requests: list[httpx.Request] = []
    bundle_id = "sha256:" + "1" * 64
    logical_uri = "asset://fixture/revision/model.stl"

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        if request.url.path.endswith("/urdf"):
            payload = b"<robot/>"
            return httpx.Response(
                200,
                content=payload,
                headers=_viewer_headers(
                    kind="urdf",
                    bundle_id=bundle_id,
                    payload=payload,
                ),
                request=request,
            )
        payload = b"solid"
        return httpx.Response(
            200,
            content=payload,
            headers=_viewer_headers(
                kind="resource",
                bundle_id=bundle_id,
                payload=payload,
            ),
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        urdf = client.viewer_model_urdf()
        resource = client.viewer_model_resource(bundle_id, logical_uri)
    finally:
        raw.close()

    assert urdf.asset_kind == "urdf"
    assert urdf.logical_uri is None
    assert urdf.bytes == b"<robot/>"
    assert resource.asset_kind == "resource"
    assert resource.logical_uri == logical_uri
    assert resource.bytes == b"solid"
    assert [request.url.path for request in requests] == [
        "/api/viewer-model/urdf",
        "/api/viewer-model/resource",
    ]
    assert dict(requests[1].url.params) == {
        "retained_bundle_id": bundle_id,
        "logical_uri": logical_uri,
    }


def test_client_uses_the_selected_assistant_mode_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.assistant_mode_projection.v3"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.assistant_mode("composer-assistant")["schema_version"].endswith(
            "projection.v3"
        )
        with pytest.raises(ValueError, match="assistant mode ID is invalid"):
            client.assistant_mode("../runtime")
    finally:
        raw.close()
    assert [request.url.path for request in requests] == [
        "/api/assistant/modes/composer-assistant"
    ]


def test_client_uses_the_owner_issued_assistant_mode_index() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.assistant_mode_index.v3", "modes": []},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.assistant_modes()["modes"] == []
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/assistant/modes"]


def test_client_requests_the_bounded_attention_newest_alarm_page() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.operator_alarm_snapshot.v3"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        client.alarms(limit=32, ordering="attention_newest")
        with pytest.raises(ValueError, match="ordering is unsupported"):
            client.alarms(ordering="priority_oldest")
    finally:
        raw.close()
    assert len(requests) == 1
    assert dict(requests[0].url.params) == {
        "limit": "32",
        "ordering": "attention_newest",
    }


def test_client_preserves_stable_server_error() -> None:
    transport = httpx.MockTransport(
        lambda request: httpx.Response(
            503,
            json={
                "detail": {
                    "code": "manager_command_unavailable",
                    "message": "Manager is absent",
                }
            },
            request=request,
        )
    )
    raw = httpx.Client(base_url="http://headless.test", transport=transport)
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with pytest.raises(HeadlessClientError) as captured:
            client.manager_status()
        assert captured.value.code == "manager_command_unavailable"
        assert captured.value.status_code == 503
    finally:
        raw.close()


def test_client_distinguishes_transport_timeout_from_other_unavailability() -> None:
    def handler(request: httpx.Request) -> httpx.Response:
        raise httpx.ReadTimeout("status observation timed out", request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with pytest.raises(HeadlessClientError) as captured:
            client.manager_apply_status(
                1, 1, application_start_id="3f9a0000000000a1", timeout_seconds=0.1
            )
        assert captured.value.code == "headless_http_timeout"
        assert captured.value.status_code is None
    finally:
        raw.close()


def test_client_posts_one_apply_acknowledgement_with_identity_in_query() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"ok": True}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        response = client.manager_apply_acknowledge(
            1,
            1,
            {
                "request_key": "11111111111111111111111111111111",
                "source_label": "manyforge_composer",
            },
            application_start_id="3f9a0000000000a1",
        )
        assert response == {"ok": True}
    finally:
        raw.close()

    assert len(requests) == 1
    assert requests[0].method == "POST"
    assert requests[0].url.path == "/api/manager/apply/acknowledge"
    assert parse_qsl(requests[0].url.query.decode("ascii")) == [
        ("apply_id", "1"),
        ("apply_generation", "1"),
    ]
    assert json.loads(requests[0].content) == {
        "application_start_id": "3f9a0000000000a1",
        "request_context": {
            "request_key": "11111111111111111111111111111111",
            "source_label": "manyforge_composer",
        },
    }


def test_client_rejects_noncanonical_alarm_condition_path_before_http() -> None:
    transport = httpx.MockTransport(
        lambda request: pytest.fail(f"unexpected request: {request.url}")
    )
    raw = httpx.Client(base_url="http://headless.test", transport=transport)
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        with pytest.raises(ValueError, match="canonical alarm condition"):
            client.acknowledge_alarm("condition/../../health", {})
    finally:
        raw.close()


def test_http_refusal_keeps_the_reported_producer() -> None:
    forwarded = HeadlessHttpClient._error_from_response(
        {"detail": {"code": "stale_generation", "message": "moved",
                    "details": {"reason": "expected_generation_stale"},
                    "producer": "kernel"}},
        409,
    )
    assert forwarded.code == "stale_generation"
    assert forwarded.producer == "kernel"
    raw_manager = HeadlessHttpClient._error_from_response(
        {"api_version": "manyforge.manager_command_response.v3", "ok": False,
         "error": {"code": "apply_busy", "message": "busy", "details": {},
                   "producer": "component_manager"}},
        409,
    )
    assert raw_manager.producer == "component_manager"
    unnamed = HeadlessHttpClient._error_from_response({"detail": "text"}, 500)
    assert unnamed.code == "headless_http_error"
    assert unnamed.producer == "headless_api"


def test_client_names_the_stop_all_and_halt_routes() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"ok": True}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    body = {"request_context": {"request_key": "44" * 16, "source_label": "ui"}}
    try:
        client.stop_all(body)
        client.halt(body)
        for path in ("/api/execution/stop-all", "/api/execution/halt"):
            client.recognized_malformed_stop(path)
        with pytest.raises(ValueError):
            client.recognized_malformed_stop("/api/execution/full-stop")
    finally:
        raw.close()

    assert [request.url.path for request in requests] == [
        "/api/execution/stop-all",
        "/api/execution/halt",
        "/api/execution/stop-all",
        "/api/execution/halt",
    ]
    assert json.loads(requests[0].read()) == body
    assert json.loads(requests[2].read()) == {}


def test_command_calls_bound_only_the_connection_and_reads_keep_their_limit() -> None:
    # OD-214 (T3, T38): a command answers at acceptance and is followed
    # through its status; the wait for its reply has no read limit.
    timeouts: dict[tuple[str, str], dict[str, float | None]] = {}

    def handler(request: httpx.Request) -> httpx.Response:
        timeouts[(request.method, request.url.path)] = request.extensions["timeout"]
        return httpx.Response(200, json={"ok": True}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(handler)
    )
    client = HeadlessHttpClient("http://unused.test", timeout_seconds=7.0, client=raw)
    commands = (
        "/api/program/runs",
        "/api/program/runs/3/motion-start",
        "/api/program/runs/3/resume-motion",
        "/api/program/runs/3/resume-halt",
        "/api/program/runs/3/reset",
        "/api/program/runs/3/blackboard-values",
        "/api/program/runs/3/manual-result",
        "/api/program/cycle/reset",
        "/api/cycle/recovery-actions",
        "/api/program/persistent-values",
        "/api/program/persistent-values/reset",
        "/api/program/persistent-values/clear",
        "/api/control/commands",
        "/api/activation-preparations",
        "/api/manager/apply",
        "/api/manager/apply/release-for-new",
        "/api/scene/transactions",
    )
    try:
        for path in commands:
            client.request("POST", path, body={})
        client.request("GET", "/api/cycle/status")
        client.program_runtime_blackboard_values(3, application_start_id="3f9a0000000000a1")
        client.request("POST", "/api/program/runs/3/node-correction-previews", body={})
        client.request("POST", "/api/program/runs/3/motion-stop", body={})
    finally:
        raw.close()
    for path in commands:
        timeout = timeouts[("POST", path)]
        assert timeout["read"] is None and timeout["write"] is None, path
        assert timeout["connect"] == 7.0, path
    for key in (("GET", "/api/cycle/status"),
                ("GET", "/api/program/runs/3/blackboard-values"),
                ("POST", "/api/program/runs/3/node-correction-previews"),
                ("POST", "/api/program/runs/3/motion-stop")):
        assert timeouts[key]["read"] is not None, key


def test_the_prepared_activation_is_read_by_its_digest() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"prepared_activation": {}}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(handler)
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    digest = "sha256:" + "9" * 64
    try:
        assert client.prepared_activation(digest) == {"prepared_activation": {}}
        with pytest.raises(ValueError):
            client.prepared_activation("../api/runtime")
    finally:
        raw.close()
    assert [(r.method, r.url.path) for r in requests] == [
        ("GET", f"/api/activation-preparations/{digest}")
    ]


def test_a_control_command_carries_its_upstream_receipt_time() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"receipt": {}}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(handler)
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        client.submit_control_command({}, received_monotonic_ns=123_456_789)
        client.submit_control_command({})
    finally:
        raw.close()
    assert [r.headers.get("x-manyforge-received-monotonic-ns") for r in requests] == [
        "123456789",
        None,
    ]


def test_the_contexts_outbound_headers_ride_every_request_under_its_own() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(200, json={"receipt": {}}, request=request)

    raw = httpx.Client(
        base_url="http://headless.test", transport=httpx.MockTransport(handler)
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        client.runtime()
        token = outbound_headers.set(
            {"X-Request-ID": "composer.1", "X-ManyForge-Transport": "mcp"}
        )
        try:
            client.runtime()
            client.submit_control_command({}, received_monotonic_ns=5)
        finally:
            outbound_headers.reset(token)
        client.runtime()
    finally:
        raw.close()
    assert [
        (r.headers.get("x-request-id"), r.headers.get("x-manyforge-transport"))
        for r in requests
    ] == [(None, None), ("composer.1", "mcp"), ("composer.1", "mcp"), (None, None)]
    # The call's own header is kept beside them.
    assert requests[2].headers["x-manyforge-received-monotonic-ns"] == "5"

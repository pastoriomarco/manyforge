from __future__ import annotations

import hashlib
import json
from pathlib import Path

import httpx
import pytest

from manyforge_headless_api.client import (
    MAX_HTTP_RESPONSE_BYTES,
    HeadlessClientError,
    HeadlessHttpClient,
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
            "program_id": "fixture",
            "program_revision": "sha256:" + "a" * 64,
            "retained_bundle_id": "sha256:" + "b" * 64,
            "graph_revision": "sha256:" + "c" * 64,
        }
    )
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    return binding


def _client_applied() -> dict[str, object]:
    binding = _client_binding()
    return {
        "assistant_mode": "forbidden",
        "baseline_program_binding_id": "sha256:" + "d" * 64,
        "deployment_artifact_id": "cell-a",
        "deployment_revision": "sha256:" + "e" * 64,
        "graph_revision": binding["graph_revision"],
        "node_environment_digest": binding["node_environment"]["environment_digest"],
        "preparation_digest": "sha256:" + "f" * 64,
        "program": {
            "program_id": "fixture",
            "program_revision": "sha256:" + "a" * 64,
        },
        "program_binding": binding,
        "program_binding_id": binding["program_binding_id"],
        "program_id": "fixture",
        "program_revision": "sha256:" + "a" * 64,
        "retained_bundle_id": binding["retained_bundle_id"],
        "scenario_catalog_revision": "sha256:" + "1" * 64,
        "scenario_id": "cell-pick",
        "schema_version": "manyforge.applied_program_selection.v1",
        "source": {
            "record_digest": "sha256:" + "2" * 64,
            "semantic_content_digest": "sha256:" + "3" * 64,
            "source_generation": 1,
            "source_owner_epoch": "owner.epoch.1",
            "source_owner_id": "owner.cell-a",
        },
        "typed_parameter_overrides": {},
    }


def _client_scenario_selection() -> dict[str, object]:
    applied = _client_applied()
    return {
        "assistant_mode": applied["assistant_mode"],
        "assistant_projection": None,
        "deployment_artifact_id": applied["deployment_artifact_id"],
        "deployment_revision": applied["deployment_revision"],
        "graph_revision": applied["graph_revision"],
        "node_environment_digest": applied["node_environment_digest"],
        "owner_identities": [],
        "program": applied["program"],
        "program_binding": applied["program_binding"],
        "program_binding_id": applied["program_binding_id"],
        "program_id": applied["program_id"],
        "program_revision": applied["program_revision"],
        "retained_bundle_id": applied["retained_bundle_id"],
        "scenario_catalog_revision": applied["scenario_catalog_revision"],
        "scenario_id": "cell-pick",
        "schema_version": "manyforge.scenario_program_selection.v2",
        "typed_parameter_overrides": {},
    }


def _sized_context_payload(size: int, *, native: bool = False) -> bytes:
    prefix = b'{"ok":true,"value":' if native else b""
    prefix += (
        b'{"schema_version":"manyforge.source_model_context.v1",'
        b'"node_environment":{"schema_version":"manyforge.'
        b'node_environment_projection.v2"},'
        b'"source_model":{"schema_version":"manyforge.source_model_projection.v2",'
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
            json={"schema_version": "manyforge.headless_runtime_snapshot.v1"},
            request=request,
        )
    )
    raw = httpx.Client(base_url="http://headless.test", transport=transport)
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.runtime()["schema_version"].endswith("snapshot.v1")
    finally:
        raw.close()


def test_client_uses_the_component_overview_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.component_overview.v1"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.component_overview()["schema_version"] == (
            "manyforge.component_overview.v1"
        )
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/component-overview"]


def test_client_relays_bounded_owner_json_without_reencoding() -> None:
    payloads = {
        "/api/component-overview": b' {"schema_version":"overview.v1"}\n',
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
            client.component_overview_payload() == payloads["/api/component-overview"]
        )
        assert (
            client.history_payload(params=(("limit", 32),))
            == payloads["/api/execution/history"]
        )
    finally:
        raw.close()
    assert requests[1].url.params["limit"] == "32"


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
            json={"schema_version": "manyforge.node_environment_projection.v2"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.node_environment()["schema_version"].endswith("projection.v2")
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
        "component_role_id": "robot",
        "target_role_id": "arm",
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
            {"component_role_id": "robot-a", "target_role_id": "arm"},
            {"component_role_id": "robot-b", "target_role_id": "arm"},
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
            json={"schema_version": "manyforge.source_model_projection.v2"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.source_model()["schema_version"].endswith("projection.v2")
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


def test_client_uses_the_verified_scenario_catalog_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={
                "api_version": "manyforge.scenario_catalog.v1",
                "scenario_catalog_revision": "sha256:" + "1" * 64,
                "scenarios": [],
            },
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.scenarios()["api_version"] == "manyforge.scenario_catalog.v1"
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/scenarios"]


def test_client_uses_exact_program_selection_and_run_routes() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        if request.url.path.endswith(("/program-selection", "/load-selection")):
            payload = _client_scenario_selection()
        elif request.url.path == "/api/program/applied":
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
        client.scenario_program_selection("cell-pick")
        client.scenario_load_selection("cell-pick")
        client.program_applied()
        client.program_runs(active_only=True, limit=7)
        client.program_run_start({"request_id": "request.1"})
        client.program_run("program run:1")
        client.program_run_stop(
            "program run:1",
            {"request_id": "request.2"},
        )
        client.program_run_motion_stop(
            "program run:1",
            {"request_id": "request.3"},
        )
    finally:
        raw.close()

    assert [request.url.path for request in requests] == [
        "/api/scenarios/cell-pick/program-selection",
        "/api/scenarios/cell-pick/load-selection",
        "/api/program/applied",
        "/api/program/runs",
        "/api/program/runs",
        "/api/program/runs/program run:1",
        "/api/program/runs/program run:1/stop",
        "/api/program/runs/program run:1/motion-stop",
    ]
    assert dict(requests[3].url.params) == {
        "active_only": "true",
        "limit": "7",
    }
    assert json.loads(requests[4].read()) == {"request_id": "request.1"}
    assert json.loads(requests[6].read()) == {"request_id": "request.2"}
    assert json.loads(requests[7].read()) == {"request_id": "request.3"}


def test_client_rejects_rehashed_malformed_scenario_binding() -> None:
    selection = _client_scenario_selection()
    binding = selection["program_binding"]
    binding["target_role_bindings"][0]["frame_role_bindings"][0]["unknown"] = True
    binding["program_binding_id"] = compute_content_identity(
        binding,
        own_field="program_binding_id",
        identity_domain="program-binding-id",
        api_version="manyforge.program_binding.v1",
    )
    selection["program_binding_id"] = binding["program_binding_id"]

    def handler(request: httpx.Request) -> httpx.Response:
        return httpx.Response(200, json=selection, request=request)

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    try:
        client = HeadlessHttpClient("http://unused.test", client=raw)
        with pytest.raises(HeadlessClientError) as raised:
            client.scenario_program_selection("cell-pick")
    finally:
        raw.close()
    assert raised.value.code == "scenario_program_selection_invalid"


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
            json={"schema_version": "manyforge.assistant_mode_projection.v1"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.assistant_mode("composer-assistant")["schema_version"].endswith(
            "projection.v1"
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
            json={"schema_version": "manyforge.assistant_mode_index.v1", "modes": []},
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
            json={"schema_version": "manyforge.operator_alarm_snapshot.v2"},
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

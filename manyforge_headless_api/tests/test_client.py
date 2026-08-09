from __future__ import annotations

import hashlib

import httpx
import pytest

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient


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


def test_client_uses_the_dedicated_node_environment_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.node_environment_projection.v1"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.node_environment()["schema_version"].endswith("projection.v1")
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/node-environment"]


def test_client_uses_the_dedicated_source_model_route() -> None:
    requests: list[httpx.Request] = []

    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": "manyforge.source_model_projection.v1"},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    client = HeadlessHttpClient("http://unused.test", client=raw)
    try:
        assert client.source_model()["schema_version"].endswith("projection.v1")
    finally:
        raw.close()
    assert [request.url.path for request in requests] == ["/api/source-model"]


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
        "x-manyforge-content-digest": (
            f"sha256:{hashlib.sha256(payload).hexdigest()}"
        ),
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

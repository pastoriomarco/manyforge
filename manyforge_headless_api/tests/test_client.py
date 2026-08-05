from __future__ import annotations

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

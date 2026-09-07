from __future__ import annotations

import httpx
import pytest

from manyforge_headless_api.client import HeadlessClientError, HeadlessHttpClient
from manyforge_headless_api.read_client import (
    HeadlessReadClient,
    HeadlessReadInputError,
)


SCHEMAS = {
    "/api/health": "manyforge.headless_health.v1",
    "/api/runtime": "manyforge.headless_runtime_snapshot.v1",
    "/api/manager/status": "manyforge.manager_status_projection.v1",
    "/api/manager/graph": "manyforge.manager_graph_projection.v1",
    "/api/manager/diff": "manyforge.manager_diff_projection.v1",
    "/api/node-environment": "manyforge.node_environment_projection.v2",
    "/api/source-model": "manyforge.source_model_projection.v2",
    "/api/scene": "manyforge.scene_projection.v1",
    "/api/execution/alarms": "manyforge.operator_alarm_snapshot.v2",
    "/api/execution/alarms/metrics": "manyforge.operator_alarm_metrics.v1",
    "/api/execution/history": "manyforge.historian_summary_page.v2",
    "/api/execution/history/health": "manyforge.async_historian_health.v1",
}


def _reader(requests: list[httpx.Request]) -> tuple[HeadlessReadClient, httpx.Client]:
    def handler(request: httpx.Request) -> httpx.Response:
        requests.append(request)
        return httpx.Response(
            200,
            json={"schema_version": SCHEMAS[request.url.path]},
            request=request,
        )

    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(handler),
    )
    return HeadlessReadClient(
        HeadlessHttpClient("http://unused.test", client=raw)
    ), raw


def test_read_client_exposes_only_the_closed_h0_observation_set() -> None:
    requests: list[httpx.Request] = []
    reader, raw = _reader(requests)
    try:
        assert reader.invoke("runtime")["schema_version"].endswith("snapshot.v1")
        assert reader.invoke("node_environment")["schema_version"].endswith(
            "projection.v2"
        )
        assert reader.invoke("source_model")["schema_version"].endswith(
            "projection.v2"
        )
        assert "manager_apply" not in reader.operations()
        assert "acknowledge_alarm" not in reader.operations()
        with pytest.raises(HeadlessReadInputError, match="unsupported"):
            reader.invoke("manager_apply", {})
    finally:
        raw.close()
    assert [request.url.path for request in requests] == [
        "/api/runtime",
        "/api/node-environment",
        "/api/source-model",
    ]


def test_alarm_and_history_reads_are_bounded_before_transport() -> None:
    requests: list[httpx.Request] = []
    reader, raw = _reader(requests)
    try:
        reader.invoke(
            "alarms",
            {"limit": 4, "states": ["returned_unacknowledged"]},
        )
        reader.invoke(
            "history",
            {
                "start_utc_ns": 2,
                "end_utc_ns": 8,
                "components": ["manyforge.alarm"],
                "event_types": ["operator_alarm_activated"],
                "before_sequence": 9,
                "limit": 3,
            },
        )
        with pytest.raises(HeadlessReadInputError, match="unsupported value"):
            reader.invoke("alarms", {"states": ["cleared"]})
        with pytest.raises(HeadlessReadInputError, match="must not exceed"):
            reader.invoke("history", {"start_utc_ns": 9, "end_utc_ns": 2})
    finally:
        raw.close()

    assert len(requests) == 2
    assert requests[0].url.params["states"] == "returned_unacknowledged"
    assert requests[1].url.params["components"] == "manyforge.alarm"
    assert requests[1].url.params["limit"] == "3"


def test_read_client_rejects_wrong_owner_schema() -> None:
    raw = httpx.Client(
        base_url="http://headless.test",
        transport=httpx.MockTransport(
            lambda request: httpx.Response(
                200,
                json={"schema_version": "legacy.runtime.v0"},
                request=request,
            )
        ),
    )
    reader = HeadlessReadClient(HeadlessHttpClient("http://unused", client=raw))
    try:
        with pytest.raises(HeadlessClientError) as captured:
            reader.invoke("runtime")
        assert captured.value.code == "headless_read_response_invalid"
    finally:
        raw.close()

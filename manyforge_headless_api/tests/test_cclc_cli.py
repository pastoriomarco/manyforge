from __future__ import annotations

import json
from pathlib import Path
from typing import Any

import pytest

from manyforge_headless_api import cli


class FakeHeadlessClient:
    instances: list["FakeHeadlessClient"] = []

    def __init__(self, base_url: str) -> None:
        self.base_url = base_url
        self.calls: list[tuple[str, Any]] = []
        self.instances.append(self)

    def __enter__(self) -> "FakeHeadlessClient":
        return self

    def __exit__(self, *_args: object) -> None:
        return None

    def cclc_component(self, component_id: str) -> dict[str, Any]:
        self.calls.append(("component", component_id))
        return {"controllable_component_id": component_id}

    def cclc_owner_identity(self) -> dict[str, Any]:
        self.calls.append(("owner_identity", None))
        return {"request_owner_epoch": "cclc-owner.cli-test"}

    def authoring_admission(self) -> dict[str, Any]:
        self.calls.append(("authoring_admission", None))
        return {"admission_generation": 7, "authoring_allowed": True}

    def cclc_route_templates(self, component_id: str) -> dict[str, Any]:
        self.calls.append(("templates", component_id))
        return {"controllable_component_id": component_id, "templates": []}

    def submit_component_lifecycle(
        self, request: dict[str, Any]
    ) -> dict[str, Any]:
        self.calls.append(("lifecycle_submit", request))
        return {"replay_status": "accepted_new"}

    def component_lifecycle_status(
        self, transaction_id: str
    ) -> dict[str, Any]:
        self.calls.append(("lifecycle_status", transaction_id))
        return {"status": None}

    def submit_route_selection(
        self, request: dict[str, Any]
    ) -> dict[str, Any]:
        self.calls.append(("route_submit", request))
        return {"replay_status": "request_id_conflict"}

    def route_selection_status(self, transaction_id: str) -> dict[str, Any]:
        self.calls.append(("route_status", transaction_id))
        return {"status": None}


@pytest.fixture(autouse=True)
def fake_client(monkeypatch: pytest.MonkeyPatch) -> None:
    FakeHeadlessClient.instances.clear()
    monkeypatch.setattr(cli, "HeadlessHttpClient", FakeHeadlessClient)


def test_cli_component_and_status_are_stateless_owner_reads(
    capsys: pytest.CaptureFixture[str],
) -> None:
    assert cli.main([
        "cclc-component", "--url", "http://headless.test", "component.arm"
    ]) == 0
    assert json.loads(capsys.readouterr().out) == {
        "controllable_component_id": "component.arm"
    }
    assert FakeHeadlessClient.instances[-1].calls == [
        ("component", "component.arm")
    ]

    assert cli.main(["cclc-route-status", "route.7"]) == 0
    assert FakeHeadlessClient.instances[-1].calls == [("route_status", "route.7")]


@pytest.mark.parametrize(
    ("command", "method", "submitted_fields"),
    [
        (
            "cclc-lifecycle-submit",
            "lifecycle_submit",
            {"expected_admission_generation": 7},
        ),
        ("cclc-route-submit", "route_submit", {}),
    ],
)
def test_cli_adds_admission_only_to_lifecycle_and_preserves_route_selection(
    tmp_path: Path,
    command: str,
    method: str,
    submitted_fields: dict[str, Any],
) -> None:
    request = {
        "request_context": {
            "request_id": "request.1",
            "request_owner_epoch": "cli.epoch.1",
            "source_label": "manyforge-headless-cli",
            "correlation_id": None,
        }
    }
    path = tmp_path / "request.json"
    path.write_text(json.dumps(request), encoding="utf-8")

    assert cli.main([command, str(path), "--confirmed"]) == 0
    expected_calls = []
    if command == "cclc-lifecycle-submit":
        expected_calls.append(("authoring_admission", None))
    expected_calls.append(("owner_identity", None))
    expected_request = {
        **request,
        "request_context": {
            **request["request_context"],
            "request_owner_epoch": "cclc-owner.cli-test",
        },
    }
    expected_calls.append(
        (
            method,
            {**expected_request, **submitted_fields, "confirmed": True},
        )
    )
    assert FakeHeadlessClient.instances[-1].calls == expected_calls

    request["confirmed"] = True
    path.write_text(json.dumps(request), encoding="utf-8")
    assert cli.main([command, str(path), "--confirmed"]) == 65
    assert FakeHeadlessClient.instances[-1].calls == (
        [("authoring_admission", None)]
        if command == "cclc-lifecycle-submit"
        else []
    )

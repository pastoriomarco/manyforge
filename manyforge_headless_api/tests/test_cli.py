"""Headless CLI component, I/O and readiness commands."""

from __future__ import annotations

import json
from typing import Any

import pytest

from manyforge_headless_api import cli


class _Client:
    calls: list[tuple[Any, ...]] = []

    def __init__(self, url: str) -> None:
        self.url = url

    def __enter__(self) -> "_Client":
        return self

    def __exit__(self, *_exc: object) -> None:
        return None

    def components(self) -> list[dict[str, Any]]:
        self.calls.append(("components",))
        return [
            {"component_id": "lite6", "lifecycle": "prepared", "capabilities": []},
            {"component_id": "lite6_arm", "lifecycle": "prepared", "owner_id": "lite6",
             "capabilities": []},
            {"component_id": "gripper", "lifecycle": "not_prepared", "capabilities": []},
            {"component_id": "camera", "lifecycle": "enabled", "capabilities": []},
        ]

    def component_action(self, component_id: str, action: str) -> dict[str, Any]:
        self.calls.append(("action", component_id, action))
        return {"action": action}

    def component_actions(self, action: str, component_ids: list[str]) -> dict[str, Any]:
        self.calls.append(("group", action, component_ids))
        return {"action": action}

    def io_signals(self) -> list[dict[str, Any]]:
        self.calls.append(("io",))
        return []

    def set_io_output(self, signal_id: str, value: Any) -> dict[str, Any]:
        self.calls.append(("set", signal_id, value))
        return {"outcome": "succeeded"}

    def program_readiness(self) -> dict[str, Any]:
        self.calls.append(("readiness",))
        return {"can_execute": True, "can_resume": False, "blocked_reasons": []}


@pytest.fixture(autouse=True)
def _fake_client(monkeypatch: pytest.MonkeyPatch) -> None:
    _Client.calls = []
    monkeypatch.setattr(cli, "HeadlessHttpClient", _Client)


@pytest.mark.parametrize("argv,call", [
    (["components", "list"], ("components",)),
    (["components", "action", "lite6", "recover"], ("action", "lite6", "recover")),
    (["io", "list"], ("io",)),
    (["io", "set", "lite6_controller_io/CO0", "true"], ("set", "lite6_controller_io/CO0", True)),
    (["io", "set", "plc/AO1", "2.5"], ("set", "plc/AO1", 2.5)),
    (["readiness"], ("readiness",)),
])
def test_commands_call_their_rest_endpoint(argv: list[str], call: tuple[Any, ...]) -> None:
    assert cli.main(argv) == 0
    assert _Client.calls == [call]


def test_enable_all_names_the_prepared_members_and_owner_less_components(capsys) -> None:
    assert cli.main(["components", "enable-all"]) == 0
    assert _Client.calls[-1] == ("group", "enable", ["lite6_arm"])
    assert json.loads(capsys.readouterr().out)["component_ids"] == ["lite6_arm"]


def test_prepare_all_names_every_not_prepared_or_offline_component() -> None:
    assert cli.main(["components", "prepare-all"]) == 0
    assert _Client.calls[-1] == ("group", "prepare", ["gripper"])


def test_an_unknown_action_and_the_old_apply_are_refused() -> None:
    with pytest.raises(SystemExit):
        cli.main(["components", "action", "lite6", "home"])
    with pytest.raises(SystemExit):
        cli.main(["apply"])


def test_serve_passes_the_disk_floor_and_margin_to_the_config(
    tmp_path: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    """OD-215: both launchers pass them; the disk check reads them from the config."""
    observed: list[Any] = []
    monkeypatch.setattr(cli, "create_app", lambda config: observed.append(config) or object())
    monkeypatch.setattr(cli.uvicorn, "run", lambda *_args, **_kwargs: None)
    args = ["serve", "--cell-id", "cell_a", "--application-start-id", "3f9a0000000000a1",
            "--disk-free-floor-bytes", "10000000000", "--disk-free-margin-bytes", "1000000000"]
    for flag in (
        "install-root", "artifact-root", "node-catalog-path",
        "node-environment-manifest-path", "alarm-catalog-path", "historian-directory",
        "manager-run-root", "execution-run-root",
    ):
        args += [f"--{flag}", str(tmp_path / flag)]
    assert cli.main(args) == 0
    assert observed[0].disk_free_floor_bytes == 10_000_000_000
    assert observed[0].disk_free_margin_bytes == 1_000_000_000


def test_serve_refuses_the_removed_projection_root(
    tmp_path: Any, monkeypatch: pytest.MonkeyPatch
) -> None:
    """OD-216: the Headless API reads no status files, so the option is gone."""
    monkeypatch.setattr(cli, "create_app", lambda _config: pytest.fail("must not start"))
    monkeypatch.setattr(cli.uvicorn, "run", lambda *_args, **_kwargs: None)
    args = ["serve", "--cell-id", "cell_a", "--application-start-id", "3f9a0000000000a1"]
    for flag in (
        "install-root", "artifact-root", "node-catalog-path",
        "node-environment-manifest-path", "alarm-catalog-path", "historian-directory",
        "manager-run-root", "execution-run-root",
    ):
        args += [f"--{flag}", str(tmp_path / flag)]
    with pytest.raises(SystemExit) as refused:
        cli.main([*args, "--projection-root", str(tmp_path / "projection")])
    assert refused.value.code == 2

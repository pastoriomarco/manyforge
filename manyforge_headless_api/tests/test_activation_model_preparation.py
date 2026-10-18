"""Model-preparation projection and closure join for model-free I/O (R5).

The maintained registry carries the approved model-free I/O profile
``io_device_v0`` beside the physical ros2_control profiles. Python model
preparation must skip it without fabricating a model family, while unknown or
malformed physical rows and foreign captured sources stay refused.
"""

from __future__ import annotations

import copy
import json
from pathlib import Path
import subprocess
from types import SimpleNamespace

import pytest

from manyforge_headless_api import activation_preparer
from manyforge_headless_api.activation_preparer import (
    ActivationPreparer,
    _model_preparation_profiles,
    _prepare_model_closure,
)
from manyforge_headless_api.native import HeadlessApiError


_ROOT = Path(__file__).resolve().parents[2]
_DESCRIPTION = Path(
    "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
)
_DIGEST = "sha256:" + "1" * 64
_IO_ROW = {
    "runtime_restoration_profile_id": "io_device_v0",
    "io_provider_runtime": {},
    "source_fields": {},
    "stop_profiles": {},
    "acceptance_obligations": [],
}
_MOCK_PROFILE = "lite6_mock_commanded_v0"
_SOURCE_SCHEMA = "manyforge.activation_model_source_closure.v1"


def _maintained(order: str) -> dict[str, object]:
    """Load the maintained registry, preserving its row order.

    ``maintained`` keeps the checked-in order (adding the exact I/O row at the
    end only when this tree does not carry it yet); ``io_first`` moves the I/O
    row to the front, as in the combined Track 2 registry.
    """

    root = json.loads((_ROOT / _DESCRIPTION).read_text(encoding="utf-8"))
    rows = root["runtime_restoration_profiles"]
    io_rows = [row for row in rows if "io_provider_runtime" in row]
    if not io_rows:
        io_rows = [copy.deepcopy(_IO_ROW)]
        rows.extend(io_rows)
    assert io_rows == [_IO_ROW]
    if order == "io_first":
        rows.remove(_IO_ROW)
        rows.insert(0, copy.deepcopy(_IO_ROW))
    return root


@pytest.fixture(params=["maintained", "io_first"])
def registry(request: pytest.FixtureRequest) -> dict[str, object]:
    return _maintained(request.param)


def _physical_row(root: dict[str, object]) -> dict[str, object]:
    """Select the known Mock physical row by id, never by position."""

    row = next(
        row
        for row in root["runtime_restoration_profiles"]
        if row["runtime_restoration_profile_id"] == _MOCK_PROFILE
    )
    assert "io_provider_runtime" not in row and "model_family_id" in row
    return row


def _io_row(root: dict[str, object]) -> dict[str, object]:
    return next(
        row
        for row in root["runtime_restoration_profiles"]
        if "io_provider_runtime" in row
    )


@pytest.fixture(autouse=True)
def _real_preparation_bodies() -> None:
    """These regressions must exercise the real projection and closure join.

    ``test_activation_preparer.py`` stubs both functions with a module-local
    autouse fixture; this guard proves no such stub is active here.
    """

    for name in ("_model_preparation_profiles", "_prepare_model_closure"):
        function = getattr(activation_preparer, name)
        assert function.__module__ == "manyforge_headless_api.activation_preparer"
        assert function.__name__ == name
        assert function is globals()[name]


def _capability_root(tmp_path: Path, root: dict[str, object]) -> Path:
    path = tmp_path / "capabilities" / _DESCRIPTION
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(root), encoding="utf-8")
    return tmp_path / "capabilities"


def _mock_robot() -> dict[str, object]:
    return {
        "component_id": "robot",
        "component_profile_id": _MOCK_PROFILE,
        "model_source": {"kind": "package"},
        "configuration": {"runtime_kind": "ros2_control"},
    }


def _io_device() -> dict[str, object]:
    return {
        "component_id": "io",
        "component_profile_id": "io_device_v0",
        "target_settings": {},
        "configuration": {"runtime_kind": "io_provider"},
    }


def _source(component_id: str) -> dict[str, object]:
    return {"component_id": component_id, "entrypoints": {}, "resources": []}


def _closure(*component_ids: str) -> dict[str, object]:
    return {
        "schema_version": _SOURCE_SCHEMA,
        "components": [_source(component_id) for component_id in component_ids],
    }


@pytest.fixture
def prepared_sources(monkeypatch: pytest.MonkeyPatch) -> list[tuple[str, str]]:
    """Record the physical sources prepared, without real XML rewriting."""

    calls: list[tuple[str, str]] = []

    def prepare(source, *, model_asset_id, accepted_package_uri_prefix):
        calls.append((source["component_id"], model_asset_id))
        return {"component_id": source["component_id"], "model_asset_id": model_asset_id}

    monkeypatch.setattr(activation_preparer, "prepare_captured_model_source", prepare)
    return calls


def test_registry_with_model_free_io_profile_constructs_ros_only_preparer(
    registry: dict[str, object], tmp_path: Path
) -> None:
    root = registry
    capabilities = _capability_root(tmp_path, root)

    profiles = _model_preparation_profiles(capabilities)
    preparer = ActivationPreparer(
        executable=tmp_path / "preparer",
        capability_root=capabilities,
        activation_root=tmp_path / "artifacts",
        runtime_cell_id="cell_a",
        timeout_seconds=3.0,
    )

    assert "io_device_v0" in profiles and profiles["io_device_v0"] is None
    assert profiles[_MOCK_PROFILE] == (
        "lite6_lite_gripper",
        "package://manyforge_example_models/models/lite6_lite_gripper/",
    )
    physical = [
        row["runtime_restoration_profile_id"]
        for row in root["runtime_restoration_profiles"]
        if "io_provider_runtime" not in row
    ]
    assert physical and all(profiles[profile] is not None for profile in physical)
    assert preparer._profiles == profiles


def test_mock_robot_plus_io_device_load_prepares_only_the_robot_model(
    registry: dict[str, object],
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    prepared_sources: list[tuple[str, str]],
) -> None:
    capabilities = _capability_root(tmp_path, registry)
    executable = tmp_path / "preparer"
    executable.write_text("fixture", encoding="utf-8")
    (tmp_path / "artifacts").mkdir()
    preparer = ActivationPreparer(
        executable=executable,
        capability_root=capabilities,
        activation_root=tmp_path / "artifacts",
        runtime_cell_id="cell_a",
        timeout_seconds=3.0,
    )
    observed: dict[str, object] = {}

    def run(command, **kwargs):
        input_path = Path(command[command.index("--input") + 1])
        observed["input"] = json.loads(input_path.read_text())
        return SimpleNamespace(returncode=1)

    monkeypatch.setattr(subprocess, "run", run)
    with pytest.raises(HeadlessApiError):
        preparer.prepare(
            source_document_digest=_DIGEST,
            scenario_id="scenario_a",
            captured_input_digest=_DIGEST,
            deployment={"components": [_mock_robot(), _io_device()]},
            model_source_closure=_closure("robot"),
            program=None,
            program_role_assignment={"component_roles": [], "target_roles": []},
        )

    assert prepared_sources == [("robot", "lite6_lite_gripper")]
    assert observed["input"]["prepared_model_closure"]["components"] == [
        {"component_id": "robot", "model_asset_id": "lite6_lite_gripper"}
    ]


def test_ros_only_closure_still_prepares_every_physical_model(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    closure = _prepare_model_closure(
        deployment={"components": [_mock_robot()]},
        model_source_closure=_closure("robot"),
        profiles=profiles,
    )

    assert [row["component_id"] for row in closure["components"]] == ["robot"]


def test_closure_join_skips_only_the_model_free_io_component(
    prepared_sources: list[tuple[str, str]],
) -> None:
    # Typed profile map as the projection returns it: the I/O profile is
    # registered with an explicit ``None`` (no model), never a fabricated one.
    profiles = {
        _MOCK_PROFILE: ("lite6_lite_gripper", "package://example/"),
        "io_device_v0": None,
    }

    closure = _prepare_model_closure(
        deployment={"components": [_io_device(), _mock_robot()]},
        model_source_closure=_closure("robot"),
        profiles=profiles,
    )

    assert prepared_sources == [("robot", "lite6_lite_gripper")]
    assert [row["component_id"] for row in closure["components"]] == ["robot"]


def test_unknown_physical_family_is_still_refused(
    registry: dict[str, object], tmp_path: Path
) -> None:
    root = registry
    _physical_row(root)["model_family_id"] = "invented_family"

    with pytest.raises(HeadlessApiError, match="runtime profiles contain an invalid row"):
        _model_preparation_profiles(_capability_root(tmp_path, root))


@pytest.mark.parametrize(
    "mutate",
    [
        lambda row: row.pop("model_family_id"),
        lambda row: row.__setitem__("model_family_id", 7),
        lambda row: row.__setitem__("model_family_id", ""),
        lambda row: row.pop("runtime_restoration_profile_id"),
    ],
    ids=["missing-family", "non-string-family", "empty-family", "missing-id"],
)
def test_malformed_physical_row_is_still_refused(
    registry: dict[str, object], tmp_path: Path, mutate
) -> None:
    root = registry
    mutate(_physical_row(root))

    with pytest.raises(HeadlessApiError, match="runtime profiles contain an invalid row"):
        _model_preparation_profiles(_capability_root(tmp_path, root))


@pytest.mark.parametrize(
    "mutate",
    [
        lambda row: row.__setitem__("model_family_id", "lite6_lite_gripper"),
        lambda row: row.__setitem__("source_fields", {"x": "y"}),
        lambda row: row.__setitem__("runtime_restoration_profile_id", "io_device_v1"),
        lambda row: row.__setitem__("io_provider_runtime", {"extra": 1}),
    ],
    ids=["io-with-family", "io-with-fields", "io-renamed", "io-runtime-config"],
)
def test_only_the_exact_model_free_io_row_is_accepted(
    registry: dict[str, object], tmp_path: Path, mutate
) -> None:
    root = registry
    mutate(_io_row(root))

    with pytest.raises(HeadlessApiError, match="runtime profiles contain an invalid row"):
        _model_preparation_profiles(_capability_root(tmp_path, root))


def test_io_provider_component_with_a_model_source_is_refused(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))
    io_device = {**_io_device(), "model_source": {"kind": "package"}}

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [_mock_robot(), io_device]},
            model_source_closure=_closure("robot"),
            profiles=profiles,
        )


def test_io_provider_component_with_a_captured_source_is_refused(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [_mock_robot(), _io_device()]},
            model_source_closure=_closure("robot", "io"),
            profiles=profiles,
        )


def test_model_free_profile_requires_an_io_provider_component(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))
    not_io = {**_io_device(), "configuration": {"runtime_kind": "ros2_control"}}

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [not_io]},
            model_source_closure=_closure(),
            profiles=profiles,
        )


def test_foreign_captured_source_is_still_refused(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    with pytest.raises(HeadlessApiError, match="unused component"):
        _prepare_model_closure(
            deployment={"components": [_mock_robot(), _io_device()]},
            model_source_closure=_closure("robot", "foreign"),
            profiles=profiles,
        )


def test_duplicate_component_ids_are_refused_for_io_components(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [_io_device(), _io_device()]},
            model_source_closure=_closure(),
            profiles=profiles,
        )

"""Model-preparation projection and closure join.

Each component names its model by the model descriptor's ``id``, read through
its family's ``component_descriptor`` in the maintained registry; the
model-free ids ``io_device_v0``, ``pose_source_v0`` and ``ai_model_v0`` are
constants. Python model preparation skips model-free components without
fabricating a model family, while unknown models, malformed family rows and
foreign captured sources stay refused.
"""

from __future__ import annotations

import json
from pathlib import Path
import shutil
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
_MOCK_MODEL = "ufactory.lite6"
_SOURCE_SCHEMA = "manyforge.activation_model_source_closure.v1"


@pytest.fixture
def registry() -> dict[str, object]:
    return json.loads((_ROOT / _DESCRIPTION).read_text(encoding="utf-8"))


def _lite6_row(root: dict[str, object]) -> dict[str, object]:
    return next(row for row in root["model_families"] if row["model_family_id"] == "lite6")


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
    """The registry plus every component model it names, under one root."""

    capabilities = tmp_path / "capabilities"
    path = capabilities / _DESCRIPTION
    path.parent.mkdir(parents=True)
    path.write_text(json.dumps(root), encoding="utf-8")
    descriptors = [row.get("component_descriptor") for row in root["model_families"]]
    descriptors += root.get("component_models", [])
    for descriptor in descriptors:
        if isinstance(descriptor, str) and (_ROOT / descriptor).is_file():
            (capabilities / descriptor).parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(_ROOT / descriptor, capabilities / descriptor)
    return capabilities


def _mock_robot() -> dict[str, object]:
    return {
        "component_id": "robot",
        "component_profile_id": _MOCK_MODEL,
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


def _pose_source() -> dict[str, object]:
    return {
        "component_id": "poses",
        "component_profile_id": "pose_source_v0",
        "target_settings": {},
        "configuration": {
            "runtime_kind": "pose_source",
            "runtime_configuration": {"stream_budget_ns": 1_000_000_000},
        },
    }


def _ai_model() -> dict[str, object]:
    return {
        "component_id": "assistant_model",
        "component_profile_id": "ai_model_v0",
        "target_settings": {},
        "configuration": {
            "runtime_kind": "ai_model",
            "runtime_configuration": {
                "base_url": "http://127.0.0.1:8000/v1",
                "model": "m",
                "api_key_env": None,
            },
        },
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


def test_registry_maps_each_model_id_to_its_family_and_model_free_ids_to_kinds(
    registry: dict[str, object], tmp_path: Path
) -> None:
    capabilities = _capability_root(tmp_path, registry)

    profiles = _model_preparation_profiles(capabilities)
    preparer = ActivationPreparer(
        executable=tmp_path / "preparer",
        capability_root=capabilities,
        activation_root=tmp_path / "artifacts",
        runtime_cell_id="cell_a",
        timeout_seconds=3.0,
    )

    assert profiles["io_device_v0"] == "io_provider"
    assert profiles["pose_source_v0"] == "pose_source"
    assert profiles["ai_model_v0"] == "ai_model"
    assert profiles[_MOCK_MODEL] == (
        "lite6",
        "package://manyforge_example_models/models/lite6_lite_gripper/",
    )
    described = [row for row in registry["model_families"] if "component_descriptor" in row]
    # Machines and model-form I/O members (`component_models`) have no model.
    assert len(profiles) == len(described) + len(registry["component_models"]) + 3
    assert described
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
            project_path="/projects/scenario_a.scenario",
            captured_input_digest=_DIGEST,
            deployment={"components": [_mock_robot(), _io_device()]},
            model_source_closure=_closure("robot"),
            program=None,
            program_role_assignment={"component_roles": [], "target_roles": []},
        )

    assert prepared_sources == [("robot", "lite6")]
    assert observed["input"]["prepared_model_closure"]["components"] == [
        {"component_id": "robot", "model_asset_id": "lite6"}
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
    # registered with its runtime kind (no model), never a fabricated family.
    profiles = {
        _MOCK_MODEL: ("lite6", "package://example/"),
        "io_device_v0": "io_provider",
    }

    closure = _prepare_model_closure(
        deployment={"components": [_io_device(), _mock_robot()]},
        model_source_closure=_closure("robot"),
        profiles=profiles,
    )

    assert prepared_sources == [("robot", "lite6")]
    assert [row["component_id"] for row in closure["components"]] == ["robot"]


@pytest.mark.parametrize(
    "mutate",
    [
        lambda row: row.pop("model_asset_id"),
        lambda row: row.__setitem__("model_asset_id", 7),
        lambda row: row.__setitem__("source", {}),
    ],
    ids=["missing-asset", "non-string-asset", "missing-prefix"],
)
def test_malformed_family_row_is_refused(
    registry: dict[str, object], tmp_path: Path, mutate
) -> None:
    mutate(_lite6_row(registry))

    with pytest.raises(HeadlessApiError, match="families contain an invalid row"):
        _model_preparation_profiles(_capability_root(tmp_path, registry))


def test_a_missing_model_descriptor_is_refused(
    registry: dict[str, object], tmp_path: Path
) -> None:
    capabilities = _capability_root(tmp_path, registry)
    (capabilities / _lite6_row(registry)["component_descriptor"]).unlink()

    with pytest.raises(HeadlessApiError, match="could not be loaded"):
        _model_preparation_profiles(capabilities)


def test_two_families_naming_one_model_are_refused(
    registry: dict[str, object], tmp_path: Path
) -> None:
    lite_gripper = next(
        row for row in registry["model_families"] if row["model_family_id"] == "lite_gripper"
    )
    lite_gripper["component_descriptor"] = _lite6_row(registry)["component_descriptor"]

    with pytest.raises(HeadlessApiError, match="invalid or duplicate id"):
        _model_preparation_profiles(_capability_root(tmp_path, registry))


def test_an_unknown_model_id_is_refused(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))
    robot = {**_mock_robot(), "component_profile_id": "lite6_mock_commanded_v0"}

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [robot]},
            model_source_closure=_closure("robot"),
            profiles=profiles,
        )


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


def test_closure_join_skips_the_model_free_pose_source_component(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]]
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    closure = _prepare_model_closure(
        deployment={"components": [_pose_source(), _io_device(), _ai_model(), _mock_robot()]},
        model_source_closure=_closure("robot"),
        profiles=profiles,
    )

    assert prepared_sources == [("robot", "lite6")]
    assert [row["component_id"] for row in closure["components"]] == ["robot"]


@pytest.mark.parametrize(
    "component",
    [
        {**_pose_source(), "model_source": {"kind": "package"}},
        {**_pose_source(), "configuration": {"runtime_kind": "io_provider"}},
        {**_io_device(), "configuration": {"runtime_kind": "pose_source"}},
        {**_ai_model(), "model_source": {"kind": "package"}},
        {**_ai_model(), "configuration": {"runtime_kind": "pose_source"}},
    ],
    ids=[
        "pose-with-model-source",
        "pose-profile-io-kind",
        "io-profile-pose-kind",
        "ai-with-model-source",
        "ai-profile-pose-kind",
    ],
)
def test_model_free_profile_requires_its_own_runtime_kind_and_no_model(
    registry: dict[str, object],
    tmp_path: Path, prepared_sources: list[tuple[str, str]], component
) -> None:
    profiles = _model_preparation_profiles(_capability_root(tmp_path, registry))

    with pytest.raises(HeadlessApiError, match="do not match captured model profiles"):
        _prepare_model_closure(
            deployment={"components": [component]},
            model_source_closure=_closure(),
            profiles=profiles,
        )

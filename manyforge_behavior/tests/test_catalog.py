"""Contract tests for the sole v2 node-catalog runtime input."""

from __future__ import annotations

import copy
import hashlib
import json
from pathlib import Path

import pytest
import rfc8785

import manyforge_behavior.catalog as catalog_module
from manyforge_behavior.catalog import (
    EXPECTED_API_VERSION,
    EXPECTED_CONTENT_VERSION,
    CatalogValidationError,
    NodeCatalog,
    profile_satisfied,
)
from manyforge_behavior.catalog_schema import validate_value
from manyforge_behavior.node_registry import build_node_registry


CATALOG_PATH = Path(__file__).parent.parent / "resources" / "node_catalog.yaml"
EXPECTED_CATALOG_DIGEST = (
    "sha256:6ec8e6ffad4f128fb299bbd0a898751f1b04cba38abc6591ffd67842104a0aa9"
)


def _catalog_data() -> dict[str, object]:
    return NodeCatalog(CATALOG_PATH).raw_data


def _write_recomputed(tmp_path: Path, mutation) -> Path:
    data = copy.deepcopy(_catalog_data())
    mutation(data)
    payload = dict(data)
    payload.pop("catalog_digest", None)
    data["catalog_digest"] = (
        "sha256:" + hashlib.sha256(rfc8785.dumps(payload)).hexdigest()
    )
    path = tmp_path / "catalog.yaml"
    path.write_text(
        json.dumps(data, ensure_ascii=False, allow_nan=False),
        encoding="utf-8",
    )
    return path


def test_shipped_catalog_is_exact_frozen_v2_packet() -> None:
    catalog = NodeCatalog(CATALOG_PATH)

    assert catalog.api_version == EXPECTED_API_VERSION
    assert catalog.content_version == EXPECTED_CONTENT_VERSION
    assert catalog.catalog_id == "manyforge_builtin"
    assert catalog.catalog_digest == EXPECTED_CATALOG_DIGEST
    assert len(catalog.entries) == 49
    assert len(
        {
            (entry.id, engine_id, implementation_id)
            for entry in catalog.entries
            for engine_id, implementation_id in entry.runner_implementation_ids.items()
        }
    ) == 49


def test_catalog_exposes_structured_v2_semantics() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    move = catalog.get("motion.move_to_pose")

    assert move.kind == "leaf"
    assert move.category == "motion"
    assert move.min_program_profile == "composable-cell-v1"
    assert move.execution.owner_api == "kernel.operation"
    assert move.runner_implementation_ids == {
        "py_trees": "motion_move_to_pose_v1"
    }
    assert {parameter.id for parameter in move.parameters} == {
        "target_role",
        "goal",
        "planning_profile",
        "target_mode",
        "contact_policy_id",
    }
    assert {
        (requirement.capability_id, requirement.scope)
        for requirement in move.required_capabilities
    } >= {
        ("planning.motion.pose_goal", "target_binding"),
        ("execution.trajectory.execute", "target_binding"),
    }


def test_profile_comparison_has_no_legacy_profile_family() -> None:
    assert profile_satisfied("composable-cell-v1", "composable-cell-v2")
    assert not profile_satisfied("composable-cell-v2", "composable-cell-v1")
    assert not profile_satisfied("v0.1", "v0.2")


def test_catalog_schema_retains_semantic_role_and_validates_defaults() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    move = catalog.get("motion.move_to_pose")
    joint_target = catalog.schema_registry.types["joint_target"]
    target_role = next(
        parameter for parameter in move.parameters if parameter.id == "target_role"
    )
    planning_profile = next(
        parameter
        for parameter in move.parameters
        if parameter.id == "planning_profile"
    )
    joint_name = catalog.schema_registry.resolve(
        joint_target.properties["joint_name"]
    )

    assert (
        catalog.schema_registry.resolve(target_role.schema).semantic_type
        == "target_role"
    )
    assert joint_target.required == ("joint_name", "position_rad")
    assert joint_name.semantic_type == "source_joint"
    assert planning_profile.has_default is True
    assert planning_profile.default == "balanced"
    assert (
        validate_value(
            planning_profile.schema,
            "balanced",
            registry=catalog.schema_registry,
        )
        == []
    )


@pytest.mark.parametrize(
    "api_version",
    ["manyforge.node_catalog.v0", "manyforge.node_catalog.v1"],
)
def test_old_catalog_apis_are_not_runtime_inputs(
    tmp_path: Path,
    api_version: str,
) -> None:
    path = _write_recomputed(
        tmp_path,
        lambda data: data.__setitem__("api_version", api_version),
    )

    with pytest.raises(
        CatalogValidationError,
        match="api_version must be 'manyforge.node_catalog.v2'",
    ):
        NodeCatalog(path)


def test_catalog_digest_is_verified_over_rfc8785_content(tmp_path: Path) -> None:
    data = _catalog_data()
    data["entries"][0]["name"] = "Changed without digest"
    path = tmp_path / "catalog.yaml"
    path.write_text(json.dumps(data), encoding="utf-8")

    with pytest.raises(CatalogValidationError, match="digest mismatch"):
        NodeCatalog(path)


def test_recomputed_packet_change_cannot_reuse_frozen_content_version(
    tmp_path: Path,
) -> None:
    path = _write_recomputed(
        tmp_path,
        lambda data: data["entries"][0].__setitem__(
            "name",
            "Semantically valid but not the frozen packet",
        ),
    )

    with pytest.raises(
        CatalogValidationError,
        match="does not match the frozen built-in",
    ):
        NodeCatalog(path)


def test_strict_loader_rejects_duplicate_keys(tmp_path: Path) -> None:
    path = tmp_path / "duplicate.yaml"
    path.write_text(
        "api_version: manyforge.node_catalog.v2\n"
        "api_version: manyforge.node_catalog.v2\n",
        encoding="utf-8",
    )

    with pytest.raises(CatalogValidationError, match="duplicate mapping key"):
        NodeCatalog(path)


def test_strict_loader_rejects_excessive_nesting(tmp_path: Path) -> None:
    path = tmp_path / "deep.yaml"
    path.write_text(("[" * 140) + "null" + ("]" * 140), encoding="utf-8")

    with pytest.raises(CatalogValidationError, match="nesting exceeds"):
        NodeCatalog(path)


def test_recomputed_catalog_cannot_add_runner_import_metadata(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        data["entries"][0]["runners"]["py_trees"]["module"] = "unsafe.module"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="unknown fields.*module"):
        NodeCatalog(path)


def test_distinct_nodes_may_share_one_registered_implementation(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        data["entries"][1]["runners"]["py_trees"]["implementation_id"] = (
            data["entries"][0]["runners"]["py_trees"]["implementation_id"]
        )

    path = _write_recomputed(tmp_path, mutate)
    digest = json.loads(path.read_text(encoding="utf-8"))["catalog_digest"]
    monkeypatch.setattr(
        catalog_module,
        "EXPECTED_BUILTIN_CATALOG_DIGEST",
        digest,
    )
    catalog = NodeCatalog(path)
    registry = build_node_registry(catalog)
    node_ids = [
        entry["id"]
        for entry in json.loads(path.read_text(encoding="utf-8"))["entries"][:2]
    ]

    assert (
        registry.record(node_ids[0]).implementation_id
        == registry.record(node_ids[1]).implementation_id
    )


def test_recomputed_catalog_rejects_wrong_capability_scope(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        move = next(
            entry
            for entry in data["entries"]
            if entry["id"] == "motion.move_to_pose"
        )
        move["required_capabilities"][0]["scope"] = "cell_global"
        move["required_capabilities"][0].pop("target_parameter")

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="scope must be 'target_binding'"):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_source_joint_without_one_target_scope(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        move = next(
            entry
            for entry in data["entries"]
            if entry["id"] == "motion.move_to_joint_target"
        )
        target = next(
            parameter
            for parameter in move["parameters"]
            if parameter["id"] == "target_role"
        )
        target["schema"]["semantic_type"] = "component_role"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="source_joint semantics require exactly one target_role parameter",
    ):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_runtime_identity_parameter(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        timer = next(entry for entry in data["entries"] if entry["id"] == "timer")
        timer["parameters"][0]["id"] = "controller_name"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="forbidden runtime identity",
    ):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_authoring_schema_as_persisted_parameter(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        timer = next(entry for entry in data["entries"] if entry["id"] == "timer")
        timer["parameters"][0].pop("schema")
        timer["parameters"][0]["schema_ref"] = (
            "#/schema_types/pose_authoring_input"
        )

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(CatalogValidationError, match="authoring-only schema"):
        NodeCatalog(path)


def test_recomputed_catalog_rejects_blackboard_control_protocol(
    tmp_path: Path,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        check = next(
            entry for entry in data["entries"] if entry["id"] == "logic.check_bool"
        )
        check["parameters"][0]["id"] = "stop_requested"
        check["blackboard"]["reads"][0]["key_parameter"] = "stop_requested"

    path = _write_recomputed(tmp_path, mutate)
    with pytest.raises(
        CatalogValidationError,
        match="forbidden execution-control blackboard protocol",
    ):
        NodeCatalog(path)


def test_legacy_catalog_model_fields_are_removed() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    parameter = catalog.get("timer").parameters[0]

    assert not hasattr(catalog, "version")
    assert not hasattr(parameter, "name")
    assert not hasattr(parameter, "type")
    assert not hasattr(parameter, "enum_values")
    assert not hasattr(catalog.get("timer"), "runtime_injections")


def test_parsed_schema_maps_are_deeply_read_only() -> None:
    catalog = NodeCatalog(CATALOG_PATH)
    stored_pose = catalog.schema_registry.types["stored_pose"]

    with pytest.raises(TypeError):
        catalog.schema_registry.types["forged"] = stored_pose  # type: ignore[index]
    with pytest.raises(TypeError):
        stored_pose.properties["forged"] = stored_pose  # type: ignore[index]

    shape = catalog.schema_registry.types["shape"]
    box_dimensions = shape.properties["box_dimensions_m"]
    with pytest.raises(TypeError):
        box_dimensions.required_when["type"] = "mesh"  # type: ignore[index]

    timer = catalog.get("timer")
    with pytest.raises(TypeError):
        timer.outcomes["success"] = "forged"  # type: ignore[index]


def test_nested_parameter_defaults_are_transitively_read_only(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    def mutate(data: dict[str, object]) -> None:
        add = next(
            entry
            for entry in data["entries"]
            if entry["id"] == "scene.add_object"
        )
        shape = next(
            parameter
            for parameter in add["parameters"]
            if parameter["id"] == "shape"
        )
        shape["default"] = {
            "type": "box",
            "box_dimensions_m": [1.0, 2.0, 3.0],
        }

    path = _write_recomputed(tmp_path, mutate)
    digest = json.loads(path.read_text(encoding="utf-8"))["catalog_digest"]
    monkeypatch.setattr(
        catalog_module,
        "EXPECTED_BUILTIN_CATALOG_DIGEST",
        digest,
    )
    catalog = NodeCatalog(path)
    default = next(
        parameter.default
        for parameter in catalog.get("scene.add_object").parameters
        if parameter.id == "shape"
    )

    with pytest.raises(TypeError):
        default["type"] = "sphere"
    with pytest.raises(TypeError):
        default["box_dimensions_m"][0] = 99.0

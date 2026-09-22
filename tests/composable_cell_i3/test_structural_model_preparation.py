from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest

from manyforge_deployment.tools import prepare_structural_model as subject


PACKAGE_PREFIX = "package://qualified_models/example/"


def _minimal_urdf(*children: str) -> bytes:
    return ('<robot name="qualified">' + "".join(children) + "</robot>").encode()


def _prepare(
    urdf: bytes,
    srdf: bytes | None = None,
) -> subject.PreparedStructuralModel:
    return subject.prepare_structural_model(
        urdf,
        srdf,
        model_asset_id="qualified_model",
        accepted_package_uri_prefix=PACKAGE_PREFIX,
    )


def _repo_root() -> Path:
    return Path(__file__).resolve().parents[2]


def test_preparation_is_deterministic_resolver_free_and_structural() -> None:
    urdf = _minimal_urdf(
        '<ros2_control name="runtime" type="system">'
        "<hardware><plugin>example/Runtime</plugin></hardware>"
        "</ros2_control>",
        '<link name="base"><visual><geometry>'
        '<mesh filename="package://qualified_models/example/'
        'meshes/missing.stl"/>'
        "</geometry></visual></link>",
        '<transmission name="runtime"><type>runtime</type></transmission>',
        '<gazebo reference="base"><selfCollide>true</selfCollide></gazebo>',
    )
    srdf = (
        '<robot name="qualified"><group name="arm">'
        '<link name="base"/></group></robot>'
    ).encode()

    first = _prepare(urdf, srdf)
    second = _prepare(urdf, srdf)

    assert first == second
    assert dict(first.removed_top_level_elements) == {
        "gazebo": 1,
        "ros2_control": 1,
        "transmission": 1,
    }
    assert dict(first.removed_nested_elements) == {
        "collision.surface": 0,
    }
    assert dict(first.removed_duplicate_srdf_elements) == {
        "disable_collisions": 0,
    }
    assert first.resources == (
        subject.PreparedResourceReference(
            asset_uri=("asset://qualified_model/meshes/missing.stl"),
            kind="mesh",
            occurrence_count=1,
            original_package_uri=(
                "package://qualified_models/example/meshes/missing.stl"
            ),
            relative_path="meshes/missing.stl",
        ),
    )
    assert b"ros2_control" not in first.urdf_bytes
    assert b"transmission" not in first.urdf_bytes
    assert b"gazebo" not in first.urdf_bytes
    assert b'filename="asset://qualified_model/meshes/missing.stl"' in first.urdf_bytes
    assert first.srdf_bytes is not None
    assert ET.fromstring(first.srdf_bytes).find("./group") is not None


@pytest.mark.parametrize("tag", ["gazebo", "ros2_control", "transmission"])
def test_runtime_only_elements_are_rejected_when_nested(tag: str) -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="must be a direct <robot> child",
    ):
        _prepare(_minimal_urdf(f'<link name="base"><{tag}/></link>'))


def test_simulator_collision_surface_is_removed_and_recorded() -> None:
    prepared = _prepare(
        _minimal_urdf(
            '<link name="base"><collision><geometry><box size="1 1 1"/>'
            "</geometry><surface><contact><ode><min_depth>0.001</min_depth>"
            "</ode></contact></surface></collision></link>"
        )
    )

    assert b"surface" not in prepared.urdf_bytes
    assert b"min_depth" not in prepared.urdf_bytes
    prepared_root = ET.fromstring(prepared.urdf_bytes)
    collision = prepared_root.find("./link[@name='base']/collision")
    assert collision is not None
    box = collision.find("./geometry/box")
    assert box is not None
    assert box.attrib == {"size": "1 1 1"}
    assert dict(prepared.removed_nested_elements) == {
        "collision.surface": 1,
    }


def test_duplicate_srdf_disabled_collision_is_removed_and_recorded() -> None:
    srdf = (
        '<robot name="qualified">'
        '<disable_collisions link1="arm" link2="tool" reason="Adjacent"/>'
        '<disable_collisions link1="tool" link2="arm" reason="Adjacent"/>'
        "</robot>"
    ).encode()

    prepared = _prepare(_minimal_urdf('<link name="base"/>'), srdf)

    assert prepared.srdf_bytes is not None
    prepared_srdf = ET.fromstring(prepared.srdf_bytes)
    rows = prepared_srdf.findall("./disable_collisions")
    assert len(rows) == 1
    assert rows[0].attrib == {
        "link1": "arm",
        "link2": "tool",
        "reason": "Adjacent",
    }
    assert dict(prepared.removed_duplicate_srdf_elements) == {
        "disable_collisions": 1,
    }


@pytest.mark.parametrize(
    "uri",
    [
        "package://other/models/mesh.stl",
        "model://qualified_models/example/mesh.stl",
        "file:///tmp/mesh.stl",
        "https://example.invalid/mesh.stl",
        "asset://qualified_model/mesh.stl",
        "../mesh.stl",
        "mesh.stl",
    ],
)
def test_resource_uri_must_use_exact_accepted_package_prefix(
    uri: str,
) -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="must use exactly the accepted package prefix",
    ):
        _prepare(
            _minimal_urdf(
                '<link name="base"><visual><geometry>'
                f'<mesh filename="{uri}"/>'
                "</geometry></visual></link>"
            )
        )


@pytest.mark.parametrize(
    "relative_path",
    [
        "",
        "/absolute.stl",
        "../escape.stl",
        "meshes/../escape.stl",
        "meshes//duplicate.stl",
        "meshes/./ambiguous.stl",
        "meshes/back\\slash.stl",
        "meshes/percent%2fstl",
        "meshes/query.stl?x=1",
        "meshes/fragment.stl#x",
    ],
)
def test_resource_path_must_be_safe_and_unambiguous(
    relative_path: str,
) -> None:
    with pytest.raises(subject.StructuralModelPreparationError):
        _prepare(
            _minimal_urdf(
                '<link name="base"><visual><geometry>'
                f'<mesh filename="{PACKAGE_PREFIX}{relative_path}"/>'
                "</geometry></visual></link>"
            )
        )


def test_case_colliding_distinct_resource_paths_are_rejected() -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="collide under portable case folding",
    ):
        _prepare(
            _minimal_urdf(
                '<link name="base"><visual><geometry>'
                f'<mesh filename="{PACKAGE_PREFIX}Meshes/tool.stl"/>'
                "</geometry></visual><collision><geometry>"
                f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
                "</geometry></collision></link>"
            )
        )


def test_identical_resource_references_are_counted_not_rejected() -> None:
    prepared = _prepare(
        _minimal_urdf(
            '<link name="base"><visual><geometry>'
            f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
            "</geometry></visual><collision><geometry>"
            f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
            "</geometry></collision></link>"
        )
    )
    assert len(prepared.resources) == 1
    assert prepared.resources[0].occurrence_count == 2


@pytest.mark.parametrize(
    "relative_path",
    ["model.urdf", "MODEL.SRDF", "preparation-manifest.json"],
)
def test_resource_path_must_not_collide_with_preparation_output(
    relative_path: str,
) -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="reserved preparation output",
    ):
        _prepare(
            _minimal_urdf(
                '<link name="base"><visual><geometry>'
                f'<mesh filename="{PACKAGE_PREFIX}{relative_path}"/>'
                "</geometry></visual></link>"
            )
        )


def test_srdf_cannot_smuggle_a_foreign_uri() -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="not a qualified structural resource reference",
    ):
        _prepare(
            _minimal_urdf('<link name="base"/>'),
            (
                '<robot name="qualified"><group name="https://'
                'example.invalid/group"><link name="base"/></group></robot>'
            ).encode(),
        )


@pytest.mark.parametrize(
    "urdf",
    [
        b'<robot name="r"><xacro/></robot>',
        b'<robot name="r"><link name="${prefix}base"/></robot>',
        b'<robot name="r"><link name="$(find package)"/></robot>',
        b'<robot name="r"><plugin/></robot>',
        (
            b'<?xml version="1.0"?><!DOCTYPE robot ['
            b'<!ENTITY value "x">]><robot name="r"/>'
        ),
        b'<?xml version="1.0"?><?unsafe value?><robot name="r"/>',
    ],
)
def test_unexpanded_or_non_structural_xml_is_rejected(urdf: bytes) -> None:
    with pytest.raises(subject.StructuralModelPreparationError):
        _prepare(urdf)


def test_uri_outside_mesh_or_texture_is_rejected() -> None:
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="appears outside",
    ):
        _prepare(
            _minimal_urdf('<link name="package://qualified_models/example/base"/>')
        )


def test_entrypoint_and_aggregate_xml_budgets_are_enforced(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(subject, "MAX_XML_ENTRYPOINT_BYTES", 64)
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="byte bound",
    ):
        _prepare(_minimal_urdf('<link name="base"/>') + b" " * 64)

    monkeypatch.setattr(subject, "MAX_XML_ENTRYPOINT_BYTES", 1024)
    monkeypatch.setattr(subject, "MAX_XML_ELEMENTS", 2)
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="aggregate XML element bound",
    ):
        _prepare(
            _minimal_urdf('<link name="base"/>'),
            b'<robot name="qualified"><group name="arm"/></robot>',
        )


def test_cli_dry_run_has_no_output_effect(tmp_path: Path) -> None:
    urdf = tmp_path / "input.urdf"
    urdf.write_bytes(_minimal_urdf('<link name="base"/>'))
    absent_output = tmp_path / "must-not-exist"
    command = [
        sys.executable,
        str(
            _repo_root()
            / "manyforge_deployment"
            / "tools"
            / "prepare_structural_model.py"
        ),
        "--urdf",
        str(urdf),
        "--model-asset-id",
        "qualified_model",
        "--accepted-package-uri-prefix",
        PACKAGE_PREFIX,
        "--output-directory",
        str(absent_output),
        "--dry-run",
    ]
    result = subprocess.run(
        command,
        check=True,
        capture_output=True,
        text=True,
    )
    manifest = json.loads(result.stdout)
    assert manifest["model_asset_id"] == "qualified_model"
    assert manifest["generator_id"] == subject.GENERATOR_ID
    assert manifest["generator_version"] == subject.GENERATOR_VERSION
    assert not absent_output.exists()


def test_qualified_receipt_binds_exact_configuration_inputs_and_closure(
    tmp_path: Path,
) -> None:
    urdf = _minimal_urdf(
        '<link name="base"><visual><geometry>'
        f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
        "</geometry></visual></link>"
    )
    srdf = (
        '<robot name="qualified"><group name="arm">'
        '<link name="base"/></group></robot>'
    ).encode()
    resource_root = tmp_path / "source"
    (resource_root / "meshes").mkdir(parents=True)
    mesh_bytes = b"solid exact\nendsolid exact\n"
    (resource_root / "meshes" / "tool.stl").write_bytes(mesh_bytes)

    prepared = _prepare(urdf, srdf)
    qualified = subject.qualify_structural_model_preparation(
        prepared,
        original_urdf_bytes=urdf,
        original_srdf_bytes=srdf,
        resource_root=resource_root,
    )
    repeated = subject.qualify_structural_model_preparation(
        prepared,
        original_urdf_bytes=urdf,
        original_srdf_bytes=srdf,
        resource_root=resource_root,
    )

    assert qualified == repeated
    assert qualified.manifest_bytes() == repeated.manifest_bytes()
    manifest = json.loads(qualified.manifest_bytes())
    assert (
        qualified.manifest_bytes()
        == json.dumps(
            manifest,
            ensure_ascii=True,
            separators=(",", ":"),
            sort_keys=True,
        ).encode()
    )
    assert manifest["build_digest"] == (
        "sha256:"
        + hashlib.sha256(
            (
                _repo_root()
                / "manyforge_deployment"
                / "tools"
                / "prepare_structural_model.py"
            ).read_bytes()
        ).hexdigest()
    )
    projection = dict(manifest)
    del projection["configuration_digest"]
    preimage = b"\0".join(
        (
            subject.CONTENT_IDENTITY_PREFIX.encode(),
            subject.CONFIGURATION_IDENTITY_DOMAIN.encode(),
            subject.PREPARATION_API_VERSION.encode(),
            json.dumps(
                projection,
                ensure_ascii=True,
                separators=(",", ":"),
                sort_keys=True,
            ).encode(),
        )
    )
    assert manifest["configuration_digest"] == (
        "sha256:" + hashlib.sha256(preimage).hexdigest()
    )
    assert manifest["original_source"] == {
        "srdf": {
            "byte_size": len(srdf),
            "content_digest": ("sha256:" + hashlib.sha256(srdf).hexdigest()),
        },
        "urdf": {
            "byte_size": len(urdf),
            "content_digest": ("sha256:" + hashlib.sha256(urdf).hexdigest()),
        },
    }
    artifacts = {
        row["logical_uri"]: row for row in manifest["prepared_source"]["artifacts"]
    }
    assert artifacts["asset://qualified_model/meshes/tool.stl"] == {
        "byte_size": len(mesh_bytes),
        "content_digest": ("sha256:" + hashlib.sha256(mesh_bytes).hexdigest()),
        "logical_uri": "asset://qualified_model/meshes/tool.stl",
        "media_type": "model/stl",
    }
    assert qualified.resource_bytes == (
        ("asset://qualified_model/meshes/tool.stl", mesh_bytes),
    )


def test_captured_resource_qualification_matches_offline_wrapper(
    tmp_path: Path,
) -> None:
    logical_uri = f"{PACKAGE_PREFIX}meshes/tool.stl"
    urdf = _minimal_urdf(
        '<link name="base"><visual><geometry>'
        f'<mesh filename="{logical_uri}"/>'
        "</geometry></visual></link>"
    )
    resource_root = tmp_path / "source"
    (resource_root / "meshes").mkdir(parents=True)
    mesh_bytes = b"solid captured\nendsolid captured\n"
    (resource_root / "meshes" / "tool.stl").write_bytes(mesh_bytes)
    prepared = _prepare(urdf)

    captured = subject.qualify_captured_structural_model_preparation(
        prepared,
        original_urdf_bytes=urdf,
        original_srdf_bytes=None,
        captured_resources={logical_uri: mesh_bytes},
    )
    offline = subject.qualify_structural_model_preparation(
        prepared,
        original_urdf_bytes=urdf,
        original_srdf_bytes=None,
        resource_root=resource_root,
    )

    assert captured == offline
    assert captured.manifest_bytes() == offline.manifest_bytes()


@pytest.mark.parametrize(
    ("captured_resources", "message"),
    [
        ({}, "missing resource"),
        (
            {
                f"{PACKAGE_PREFIX}meshes/tool.stl": b"solid exact\nendsolid exact\n",
                f"{PACKAGE_PREFIX}meshes/unused.stl": b"unused",
            },
            "unused resource",
        ),
        ({f"{PACKAGE_PREFIX}meshes/tool.stl": b""}, "size is outside"),
    ],
)
def test_captured_resource_qualification_requires_one_exact_closed_set(
    captured_resources: dict[str, bytes], message: str
) -> None:
    urdf = _minimal_urdf(
        '<link name="base"><visual><geometry>'
        f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
        "</geometry></visual></link>"
    )
    prepared = _prepare(urdf)

    with pytest.raises(subject.StructuralModelPreparationError, match=message):
        subject.qualify_captured_structural_model_preparation(
            prepared,
            original_urdf_bytes=urdf,
            original_srdf_bytes=None,
            captured_resources=captured_resources,
        )


def test_qualified_receipt_rejects_missing_or_symlinked_resources(
    tmp_path: Path,
) -> None:
    urdf = _minimal_urdf(
        '<link name="base"><visual><geometry>'
        f'<mesh filename="{PACKAGE_PREFIX}meshes/tool.stl"/>'
        "</geometry></visual></link>"
    )
    prepared = _prepare(urdf)
    resource_root = tmp_path / "source"
    (resource_root / "meshes").mkdir(parents=True)
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="could not read plain resource",
    ):
        subject.qualify_structural_model_preparation(
            prepared,
            original_urdf_bytes=urdf,
            original_srdf_bytes=None,
            resource_root=resource_root,
        )

    outside = tmp_path / "outside.stl"
    outside.write_bytes(b"solid outside\nendsolid outside\n")
    os.symlink(outside, resource_root / "meshes" / "tool.stl")
    with pytest.raises(
        subject.StructuralModelPreparationError,
        match="could not read plain resource",
    ):
        subject.qualify_structural_model_preparation(
            prepared,
            original_urdf_bytes=urdf,
            original_srdf_bytes=None,
            resource_root=resource_root,
        )


def test_checked_lite6_articulated_gripper_qualification_fixture() -> None:
    model_root = (
        _repo_root() / "manyforge_example_models" / "models" / "lite6_lite_gripper"
    )
    prefix = "package://manyforge_example_models/models/" "lite6_lite_gripper/"
    prepared = subject.prepare_structural_model(
        (model_root / "lite6_lite_gripper.urdf").read_bytes(),
        (model_root / "lite6_lite_gripper.srdf").read_bytes(),
        model_asset_id="lite6_lite_gripper",
        accepted_package_uri_prefix=prefix,
    )
    repeated = subject.prepare_structural_model(
        (model_root / "lite6_lite_gripper.urdf").read_bytes(),
        (model_root / "lite6_lite_gripper.srdf").read_bytes(),
        model_asset_id="lite6_lite_gripper",
        accepted_package_uri_prefix=prefix,
    )
    assert prepared == repeated

    urdf_root = ET.fromstring(prepared.urdf_bytes)
    srdf_root = ET.fromstring(prepared.srdf_bytes)
    assert len(urdf_root.findall("./link")) == 13
    assert len(urdf_root.findall("./joint")) == 12
    assert len(urdf_root.findall(".//mesh")) == 20
    assert len(prepared.resources) == 18
    assert sum(item.occurrence_count for item in prepared.resources) == 20
    assert {item.kind for item in prepared.resources} == {"mesh"}
    assert all(
        item.asset_uri.startswith("asset://lite6_lite_gripper/")
        for item in prepared.resources
    )

    mimic = urdf_root.find("./joint[@name='left_finger_joint']/mimic")
    assert mimic is not None
    assert mimic.attrib == {
        "joint": "right_finger_joint",
        "multiplier": "1",
        "offset": "0",
    }
    assert {group.attrib["name"] for group in srdf_root.findall("./group")} == {
        "lite6",
        "lite_gripper",
    }
    assert len(srdf_root.findall("./disable_collisions")) == 29
    assert dict(prepared.removed_top_level_elements) == {
        "gazebo": 7,
        "ros2_control": 2,
        "transmission": 6,
    }
    assert dict(prepared.removed_nested_elements) == {
        "collision.surface": 0,
    }
    assert dict(prepared.removed_duplicate_srdf_elements) == {
        "disable_collisions": 0,
    }

    qualified = subject.qualify_structural_model_preparation(
        prepared,
        original_urdf_bytes=(model_root / "lite6_lite_gripper.urdf").read_bytes(),
        original_srdf_bytes=(model_root / "lite6_lite_gripper.srdf").read_bytes(),
        resource_root=model_root,
    )
    manifest = json.loads(qualified.manifest_bytes())
    assert len(manifest["prepared_source"]["artifacts"]) == 20
    assert len(manifest["resource_rewrites"]) == 18
    assert len(qualified.resource_bytes) == 18
    assert {row["media_type"] for row in manifest["resource_rewrites"]} == {"model/stl"}

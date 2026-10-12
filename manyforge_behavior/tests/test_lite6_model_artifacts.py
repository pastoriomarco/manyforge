"""Transport-neutral integrity checks for the retained Lite6 model evidence.

These checks do not parse ``manyforge.deployment.v0``, select a runtime,
construct a Kernel, or launch a process.  They remain in Python because they
validate immutable files and the bounded offline provenance tools used to
produce them.  Runtime admission and lane behavior are separately requalified
at I6/N, I9/I, and I10/R.
"""

from __future__ import annotations

import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest
import yaml


_REPO_ROOT = Path(__file__).resolve().parents[2]
_MODEL_DIR = (
    _REPO_ROOT / "manyforge_example_models" / "models" / "lite6_lite_gripper"
)
_STATIC_MANIFEST_PATH = _MODEL_DIR / "STATIC_MANIFEST.json"
_RUNTIME_CAPTURE_DIR = _MODEL_DIR / "manymove_runtime_capture"
_RUNTIME_CAPTURE_JSON_PATH = (
    _RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime_capture.json"
)
_RUNTIME_CAPTURE_URDF_PATH = (
    _RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime.urdf"
)
_RUNTIME_CAPTURE_SRDF_PATH = (
    _RUNTIME_CAPTURE_DIR / "manymove_lite6_runtime.srdf"
)
_RUNTIME_COMPARE_SCRIPT = (
    _REPO_ROOT / "scripts" / "models" / "compare_lite6_runtime_snapshot.py"
)
_RUNTIME_CAPTURE_SCRIPT = (
    _REPO_ROOT / "scripts" / "models" / "capture_manymove_lite6_descriptions.py"
)
_STATIC_MANIFEST_VALIDATOR_SCRIPT = (
    _REPO_ROOT / "scripts" / "models" / "validate_lite6_static_manifest.py"
)
_MODEL_EXPORT_SCRIPT = (
    _REPO_ROOT / "scripts" / "models" / "export_lite6_lite_gripper_model.py"
)
_OWNED_MOVEIT_JOINT_LIMITS_PATH = (
    _MODEL_DIR
    / "upstream_source"
    / "xarm_moveit_config"
    / "config"
    / "lite6"
    / "joint_limits.yaml"
)
_LIMITED_ARM_BOUNDS = {
    "joint1": ("-3.1101767270538954", "3.1101767270538954"),
    "joint2": ("-2.61799", "2.61799"),
    "joint3": ("-0.061087", "3.1101767270538954"),
    "joint4": ("-3.1101767270538954", "3.1101767270538954"),
    "joint5": ("-2.1642", "2.1642"),
    "joint6": ("-3.1101767270538954", "3.1101767270538954"),
}
_ARM_JOINT_DYNAMICS = {
    f"joint{index}": (2.14, 10.0) for index in range(1, 7)
}

# Narrowed from historical test_deployment.py blob
# 36ff9b99a5a51f03de84cf4692333cddc246214b, lines 4087-4125.  The old v0
# deployments and effectful bring-up qualifier remain acceptance input for
# I3/I6, I9/I, I10/R, and I11/T; this retained check covers only the canonical
# static artifacts and their offline authoring tool.
_REJECTED_MODEL_REWRITE_TERMS = (
    "coupled_joints",
    "coupledJoints",
    "GripperCoupled",
    "roboplan_compat",
    "strip_mimic_tags_for_roboplan_compat",
    "write_roboplan_compat",
)


def test_lite6_lite_gripper_model_bundle_is_self_contained() -> None:
    """The retained model bundle has a complete, digest-bound local closure."""

    urdf_path = _MODEL_DIR / "lite6_lite_gripper.urdf"
    srdf_path = _MODEL_DIR / "lite6_lite_gripper.srdf"
    assert urdf_path.exists()
    assert srdf_path.exists()
    assert _STATIC_MANIFEST_PATH.exists()

    urdf_text = urdf_path.read_text(encoding="utf-8")
    assert "package://xarm_description" not in urdf_text
    assert "<gazebo" in urdf_text
    assert "<ros2_control" in urdf_text
    assert "<transmission" in urdf_text
    assert "<mimic" in urdf_text

    root = ET.parse(urdf_path).getroot()
    assert root.attrib.get("version") == "1.2"
    mesh_refs = [
        mesh.attrib["filename"]
        for mesh in root.iter("mesh")
        if mesh.attrib.get("filename")
    ]
    assert mesh_refs
    prefix = "package://manyforge_example_models/models/lite6_lite_gripper/"
    for ref in mesh_refs:
        assert ref.startswith(prefix)
        assert (_MODEL_DIR / ref.removeprefix(prefix)).exists(), ref
    for joint_name, (lower, upper) in _LIMITED_ARM_BOUNDS.items():
        joint = root.find(f"./joint[@name='{joint_name}']")
        assert joint is not None
        limit = joint.find("limit")
        assert limit is not None
        assert limit.attrib.get("lower") == lower
        assert limit.attrib.get("upper") == upper
        velocity, acceleration = _ARM_JOINT_DYNAMICS[joint_name]
        assert float(limit.attrib["velocity"]) == velocity
        assert float(limit.attrib["acceleration"]) == acceleration
    assert root.find("./link[@name='base_link']") is None
    assert root.find("./joint[@name='base_link_to_link_base']") is None
    left_finger = root.find("./joint[@name='left_finger_joint']")
    assert left_finger is not None
    mimic = left_finger.find("mimic")
    assert mimic is not None
    assert mimic.attrib.get("joint") == "right_finger_joint"

    srdf = ET.parse(srdf_path).getroot()
    lite6_joints = {
        joint.attrib.get("name")
        for joint in srdf.findall("./group[@name='lite6']/joint")
    }
    assert {
        "world_joint",
        "joint1",
        "joint2",
        "joint3",
        "joint4",
        "joint5",
        "joint6",
        "joint_eef",
        "gripper_fix",
        "joint_tcp",
    } <= lite6_joints
    assert (
        srdf.find(
            "./group[@name='lite_gripper']/joint[@name='right_finger_joint']"
        )
        is not None
    )
    assert (
        srdf.find(
            "./group[@name='lite_gripper']/joint[@name='left_finger_joint']"
        )
        is not None
    )
    state_names = {
        state.attrib.get("name")
        for state in srdf.findall("./group_state[@group='lite_gripper']")
    }
    assert {"open", "close", "closed"} <= state_names
    closed = srdf.find("./group_state[@group='lite_gripper'][@name='closed']")
    assert closed is not None
    assert {
        joint.attrib.get("name"): joint.attrib.get("value")
        for joint in closed.findall("./joint")
    } == {"left_finger_joint": "0", "right_finger_joint": "0"}
    assert srdf.findall("./disable_collisions")

    manifest = json.loads(_STATIC_MANIFEST_PATH.read_text(encoding="utf-8"))
    assert manifest["schema"] == "manyforge.lite6_static_model_manifest.v1"
    assert manifest["source"]["repository"] == (
        "https://github.com/pastoriomarco/xarm_ros2"
    )
    assert manifest["source"]["branch"] == "jazzy_no_gazebo"
    assert manifest["source"]["commit"] == (
        "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
    )
    assert manifest["source"]["moveit_joint_limits"] == {
        "owned_snapshot_path": (
            "upstream_source/xarm_moveit_config/config/lite6/joint_limits.yaml"
        ),
        "sha256": (
            "73368cad488a238ba6c4bc72e679a2ae7e39cef3c7aecb83e50d6037255d927c"
        ),
        "upstream_path": "xarm_moveit_config/config/lite6/joint_limits.yaml",
    }
    moveit_limits = yaml.safe_load(
        _OWNED_MOVEIT_JOINT_LIMITS_PATH.read_text(encoding="utf-8")
    )["joint_limits"]
    assert set(moveit_limits) == set(_ARM_JOINT_DYNAMICS)
    for joint_name, (velocity, acceleration) in _ARM_JOINT_DYNAMICS.items():
        assert moveit_limits[joint_name] == {
            "has_velocity_limits": True,
            "max_velocity": velocity,
            "has_acceleration_limits": True,
            "max_acceleration": acceleration,
        }
    expected_paths = {
        "README.md",
        "licenses/xarm_ros2.LICENSE",
        "lite6_lite_gripper.srdf",
        "lite6_lite_gripper.urdf",
        "upstream_source/xarm_moveit_config/config/lite6/joint_limits.yaml",
    } | {ref.removeprefix(prefix) for ref in mesh_refs}
    manifest_entries = manifest["files"]
    assert {entry["path"] for entry in manifest_entries} == expected_paths
    for entry in manifest_entries:
        path = _MODEL_DIR / entry["path"]
        assert path.is_file(), entry["path"]
        data = path.read_bytes()
        assert entry["size_bytes"] == len(data), entry["path"]
        assert entry["sha256"] == hashlib.sha256(data).hexdigest(), entry["path"]


def test_retained_lite6_model_outputs_do_not_use_rejected_rewrites() -> None:
    """Canonical static models stay free of importer-specific rewrites."""

    retained_paths = (
        _MODEL_DIR / "lite6_lite_gripper.urdf",
        _MODEL_DIR / "lite6_lite_gripper.srdf",
        _MODEL_EXPORT_SCRIPT,
    )
    for path in retained_paths:
        text = path.read_text(encoding="utf-8")
        for term in _REJECTED_MODEL_REWRITE_TERMS:
            assert term not in text, f"{term} present in {path}"

    assert not list(_MODEL_DIR.glob("*roboplan_compat*"))


def test_lite6_export_preserves_the_full_model_for_activation_time_generation(
    tmp_path: Path,
) -> None:
    """Retiring the lane exporter must not alter the stored robot model."""

    spec = importlib.util.spec_from_file_location(
        "export_lite6_full_model", _MODEL_EXPORT_SCRIPT
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    output = tmp_path / "lite6_lite_gripper.urdf"
    module.write_urdf(_RUNTIME_CAPTURE_URDF_PATH, output, _ARM_JOINT_DYNAMICS)

    assert ET.canonicalize(from_file=output, strip_text=True, with_comments=True) == (
        ET.canonicalize(
            from_file=_MODEL_DIR / "lite6_lite_gripper.urdf",
            strip_text=True,
            with_comments=True,
        )
    )
    assert list(tmp_path.iterdir()) == [output]


def test_lite6_static_manifest_validator_accepts_and_rejects_drift(
    tmp_path: Path,
) -> None:
    """The bounded offline manifest validator rejects retained-asset drift."""

    accepted = subprocess.run(
        [
            sys.executable,
            str(_STATIC_MANIFEST_VALIDATOR_SCRIPT),
            "--model-dir",
            str(_MODEL_DIR),
            "--json",
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    accepted_report = json.loads(accepted.stdout)
    assert accepted_report["passed"] is True
    assert accepted_report["file_count"] == 23

    copied_model = tmp_path / "lite6_lite_gripper"
    shutil.copytree(
        _MODEL_DIR,
        copied_model,
        ignore=shutil.ignore_patterns("manymove_runtime_capture"),
    )
    with (copied_model / "README.md").open("a", encoding="utf-8") as handle:
        handle.write("\nChecksum drift fixture.\n")

    rejected = subprocess.run(
        [
            sys.executable,
            str(_STATIC_MANIFEST_VALIDATOR_SCRIPT),
            "--model-dir",
            str(copied_model),
            "--json",
        ],
        text=True,
        capture_output=True,
    )
    assert rejected.returncode != 0
    rejected_report = json.loads(rejected.stdout)
    assert rejected_report["passed"] is False
    assert "README.md" in rejected_report["error"]


def test_lite6_static_manifest_matches_its_pinned_owner_inputs(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The manifest is exactly reproducible without rotating its provenance."""

    expected = json.loads(_STATIC_MANIFEST_PATH.read_text(encoding="utf-8"))
    copied_model = tmp_path / "lite6_lite_gripper"
    shutil.copytree(
        _MODEL_DIR,
        copied_model,
        ignore=shutil.ignore_patterns("manymove_runtime_capture"),
    )

    spec = importlib.util.spec_from_file_location(
        "export_lite6_lite_gripper_model_manifest",
        _MODEL_EXPORT_SCRIPT,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    source = expected["source"]

    def fake_run_git(_path: Path, *args: str) -> str:
        if args == ("rev-parse", "--abbrev-ref", "HEAD"):
            return source["branch"]
        if args == ("rev-parse", "HEAD"):
            return source["commit"]
        raise AssertionError(f"unexpected git arguments: {args!r}")

    monkeypatch.setattr(module, "run_git", fake_run_git)
    module.write_static_manifest(
        copied_model,
        Path("/historical/xarm_ros2"),
        Path(source["source_urdf"]),
        expected["generated_at_utc"],
    )

    regenerated = json.loads(
        (copied_model / "STATIC_MANIFEST.json").read_text(encoding="utf-8")
    )
    assert regenerated == expected


def test_lite6_runtime_snapshot_comparison_accepts_documented_deltas() -> None:
    """The offline ManyMove comparison accepts only documented source deltas."""

    completed = subprocess.run(
        [
            sys.executable,
            str(_RUNTIME_COMPARE_SCRIPT),
            "--runtime-urdf",
            str(_RUNTIME_CAPTURE_URDF_PATH),
            "--runtime-srdf",
            str(_RUNTIME_CAPTURE_SRDF_PATH),
            "--snapshot-dir",
            str(_MODEL_DIR),
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    report = yaml.safe_load(completed.stdout)
    assert report["passed"] is True
    assert report["failures"] == []


def test_lite6_runtime_capture_metadata_is_enforced(tmp_path: Path) -> None:
    """The offline comparison pins its captured source branch and evidence."""

    completed = subprocess.run(
        [
            sys.executable,
            str(_RUNTIME_COMPARE_SCRIPT),
            "--runtime-urdf",
            str(_RUNTIME_CAPTURE_URDF_PATH),
            "--runtime-srdf",
            str(_RUNTIME_CAPTURE_SRDF_PATH),
            "--capture-json",
            str(_RUNTIME_CAPTURE_JSON_PATH),
            "--snapshot-dir",
            str(_MODEL_DIR),
        ],
        check=True,
        text=True,
        capture_output=True,
    )
    report = yaml.safe_load(completed.stdout)
    assert report["passed"] is True
    assert report["facts"]["runtime_capture_xarm_branch"] == "jazzy_no_gazebo"

    bad_capture = tmp_path / "bad_capture.json"
    payload = json.loads(_RUNTIME_CAPTURE_JSON_PATH.read_text(encoding="utf-8"))
    payload["xarm_ros2"]["branch"] = "jazzy"
    bad_capture.write_text(json.dumps(payload), encoding="utf-8")
    rejected = subprocess.run(
        [
            sys.executable,
            str(_RUNTIME_COMPARE_SCRIPT),
            "--runtime-urdf",
            str(_RUNTIME_CAPTURE_URDF_PATH),
            "--runtime-srdf",
            str(_RUNTIME_CAPTURE_SRDF_PATH),
            "--capture-json",
            str(bad_capture),
            "--snapshot-dir",
            str(_MODEL_DIR),
        ],
        text=True,
        capture_output=True,
    )
    assert rejected.returncode != 0
    rejected_report = yaml.safe_load(rejected.stdout)
    assert rejected_report["passed"] is False
    assert any("branch" in failure for failure in rejected_report["failures"])


def test_lite6_runtime_capture_resolves_xarm_commit_from_evidence(
    tmp_path: Path,
) -> None:
    """The offline capture helper derives provenance instead of inventing it."""

    spec = importlib.util.spec_from_file_location(
        "capture_manymove_lite6_descriptions",
        _RUNTIME_CAPTURE_SCRIPT,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    commit_file = tmp_path / ".xarm_ros2_commit"
    commit_file.write_text(
        "012731afb9ef1d4289ec45112e8d083ab59a5c1e\n",
        encoding="utf-8",
    )
    missing_checkout = tmp_path / "missing"
    assert module.resolve_xarm_commit(None, commit_file, missing_checkout) == (
        "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
    )
    assert module.resolve_xarm_commit(
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
        commit_file,
        missing_checkout,
    ) == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
    with pytest.raises(SystemExit):
        module.resolve_xarm_commit(
            None,
            tmp_path / "missing_commit",
            missing_checkout,
        )


def test_lite6_exported_readme_preserves_the_current_unavailable_boundary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """A future model refresh must not revive historical runtime claims."""

    spec = importlib.util.spec_from_file_location(
        "export_lite6_lite_gripper_model",
        _MODEL_EXPORT_SCRIPT,
    )
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)

    commit = "012731afb9ef1d4289ec45112e8d083ab59a5c1e"

    def fake_run_git(_path: Path, *args: str) -> str:
        if args[-1] == "HEAD" and "--abbrev-ref" in args:
            return "jazzy_no_gazebo"
        if args[-1] == "HEAD":
            return commit
        raise AssertionError(f"unexpected git arguments: {args!r}")

    monkeypatch.setattr(module, "run_git", fake_run_git)
    module.write_readme(
        tmp_path,
        Path("/historical/xarm_ros2"),
        Path("/historical/manymove_lite6_runtime.urdf"),
        "2026-06-26T11:45:56+00:00",
        _ARM_JOINT_DYNAMICS,
    )

    readme = (tmp_path / "README.md").read_text(encoding="utf-8")
    assert "**Current boundary:**" in readme
    assert "does not provide native, Isaac," in readme
    assert "Historical runtime boundary:" in readme
    assert "ManyForge native execution used" in readme
    assert "Real-robot execution used" in readme
    assert "ManyForge native execution uses" not in readme
    assert "Real robot execution still uses" not in readme

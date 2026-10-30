"""Integrity checks for the retained UR5e and UF850 model bundles."""

from __future__ import annotations

import hashlib
import json
import xml.etree.ElementTree as ET
from pathlib import Path

import pytest


REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = REPO_ROOT / "manyforge_example_models" / "models"

MODEL_CASES = {
    "uf850_xarm_gripper": {
        "arm_group": "uf850",
        "arm_joints": tuple(f"joint{index}" for index in range(1, 7)),
        "gripper_group": "uf850_gripper",
        "license_paths": {"licenses/xarm_ros2.LICENSE"},
        "links": {"world", "link_base", "link_eef", "link_tcp"},
        "mimics": {
            "left_finger_joint": ("drive_joint", 1.0),
            "left_inner_knuckle_joint": ("drive_joint", 1.0),
            "right_finger_joint": ("drive_joint", 1.0),
            "right_inner_knuckle_joint": ("drive_joint", 1.0),
            "right_outer_knuckle_joint": ("drive_joint", 1.0),
        },
        "presets": {"open": 0.0, "close": 0.85, "closed": 0.85},
        "runtime_elements": {
            "gazebo": 19,
            "ros2_control": 1,
            "surface": 0,
            "transmission": 7,
        },
        "divergence_documents": {
            "prepared structural view",
            "uf850_xarm_gripper.srdf",
            "uf850_xarm_gripper.urdf",
        },
        "source_commits": {
            "https://github.com/pastoriomarco/xarm_ros2": (
                "012731afb9ef1d4289ec45112e8d083ab59a5c1e"
            ),
        },
    },
    "ur5e_robotiq_2f_85": {
        "arm_group": "ur_manipulator",
        "arm_joints": (
            "shoulder_pan_joint",
            "shoulder_lift_joint",
            "elbow_joint",
            "wrist_1_joint",
            "wrist_2_joint",
            "wrist_3_joint",
        ),
        "gripper_group": "robotiq_2f_85_gripper",
        "license_paths": {
            "licenses/Universal_Robots_ROS2_Description.LICENSE",
            "licenses/manymove.LICENSE",
            "licenses/ros2_robotiq_gripper.LICENSE",
        },
        "links": {"world", "base_link", "flange", "tool0"},
        "mimics": {
            "robotiq_85_left_finger_tip_joint": (
                "robotiq_85_left_knuckle_joint",
                -1.0,
            ),
            "robotiq_85_left_inner_knuckle_joint": (
                "robotiq_85_left_knuckle_joint",
                1.0,
            ),
            "robotiq_85_right_finger_tip_joint": (
                "robotiq_85_left_knuckle_joint",
                1.0,
            ),
            "robotiq_85_right_inner_knuckle_joint": (
                "robotiq_85_left_knuckle_joint",
                -1.0,
            ),
            "robotiq_85_right_knuckle_joint": (
                "robotiq_85_left_knuckle_joint",
                -1.0,
            ),
        },
        "presets": {"open": 0.0, "closed": 0.8},
        "bounded_mimic_followers": {
            "robotiq_85_left_inner_knuckle_joint",
            "robotiq_85_right_inner_knuckle_joint",
            "robotiq_85_left_finger_tip_joint",
            "robotiq_85_right_finger_tip_joint",
        },
        "runtime_elements": {
            "gazebo": 0,
            "ros2_control": 0,
            "surface": 2,
            "transmission": 0,
        },
        "divergence_documents": {
            "prepared structural view",
            "ur5e_robotiq_2f_85.srdf",
            "ur5e_robotiq_2f_85.urdf",
        },
        "source_commits": {
            "https://github.com/NVIDIA-ISAAC-ROS/ros2_robotiq_gripper": (
                "587b6548640e366d50322068021e20b487372a75"
            ),
            "https://github.com/UniversalRobots/Universal_Robots_ROS2_Description": (
                "89bbe795f38a7ab00fb66fe8831dfff79dc99edf"
            ),
            "https://github.com/pastoriomarco/manymove": (
                "f3a4c65e154744403d721d6ef25d2a8ea5f3f1e3"
            ),
        },
    },
}


@pytest.mark.parametrize("model_id", sorted(MODEL_CASES))
def test_supported_model_bundle_is_complete_and_digest_bound(model_id: str) -> None:
    model_dir = MODEL_ROOT / model_id
    manifest_path = model_dir / "STATIC_MANIFEST.json"
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    assert manifest["schema"] == "manyforge.retained_robot_model_manifest.v1"
    assert manifest["model_id"] == model_id
    # The component descriptor describes the bundle; it is not model content.
    expected_files = {
        path.relative_to(model_dir).as_posix()
        for path in model_dir.rglob("*")
        if path.is_file() and path != manifest_path and path.name != "component.yaml"
    }
    entries = {entry["path"]: entry for entry in manifest["files"]}
    assert set(entries) == expected_files
    assert MODEL_CASES[model_id]["license_paths"] <= expected_files
    for relative, entry in entries.items():
        payload = (model_dir / relative).read_bytes()
        assert entry["size_bytes"] == len(payload), relative
        assert entry["sha256"] == hashlib.sha256(payload).hexdigest(), relative

    source_commits = {
        source["repository"]: source["commit"] for source in manifest["sources"]
    }
    assert source_commits == MODEL_CASES[model_id]["source_commits"]
    divergences = manifest["documented_divergences"]
    assert {row["document"] for row in divergences} == MODEL_CASES[model_id][
        "divergence_documents"
    ]
    assert all(row["change"] and row["effect"] for row in divergences)


@pytest.mark.parametrize("model_id", sorted(MODEL_CASES))
def test_supported_model_preserves_upstream_xml_and_localizes_meshes(
    model_id: str,
) -> None:
    model_dir = MODEL_ROOT / model_id
    retained = ET.parse(model_dir / f"{model_id}.urdf").getroot()
    upstream = ET.parse(
        model_dir / "upstream_source" / f"{model_id}.expanded.urdf"
    ).getroot()

    expected_runtime = MODEL_CASES[model_id]["runtime_elements"]
    for tag, count in expected_runtime.items():
        assert len(upstream.findall(f".//{tag}")) == count
        assert len(retained.findall(f".//{tag}")) == count

    prefix = f"package://manyforge_example_models/models/{model_id}/"
    mesh_uris = {
        mesh.attrib["filename"]
        for mesh in retained.iter("mesh")
        if mesh.attrib.get("filename")
    }
    assert mesh_uris
    for uri in mesh_uris:
        assert uri.startswith(prefix)
        assert (model_dir / uri.removeprefix(prefix)).is_file(), uri


@pytest.mark.parametrize("model_id", sorted(MODEL_CASES))
def test_supported_model_has_required_kinematics_and_semantics(model_id: str) -> None:
    case = MODEL_CASES[model_id]
    model_dir = MODEL_ROOT / model_id
    urdf = ET.parse(model_dir / f"{model_id}.urdf").getroot()
    srdf = ET.parse(model_dir / f"{model_id}.srdf").getroot()

    links = {link.attrib["name"] for link in urdf.findall("./link")}
    assert case["links"] <= links
    for joint_name in case["arm_joints"]:
        joint = urdf.find(f"./joint[@name='{joint_name}']")
        assert joint is not None
        limit = joint.find("limit")
        assert limit is not None
        assert float(limit.attrib["velocity"]) > 0.0
        assert float(limit.attrib["lower"]) < float(limit.attrib["upper"])

    groups = {group.attrib["name"] for group in srdf.findall("./group")}
    assert {case["arm_group"], case["gripper_group"]} <= groups
    for joint_name, (master, multiplier) in case["mimics"].items():
        mimic = urdf.find(f"./joint[@name='{joint_name}']/mimic")
        assert mimic is not None
        assert mimic.attrib["joint"] == master
        assert float(mimic.attrib.get("multiplier", "1")) == multiplier

    for joint_name in case.get("bounded_mimic_followers", ()):
        joint = urdf.find(f"./joint[@name='{joint_name}']")
        upstream_joint = ET.parse(
            model_dir / "upstream_source" / f"{model_id}.expanded.urdf"
        ).getroot().find(f"./joint[@name='{joint_name}']")
        assert joint is not None and joint.attrib["type"] == "revolute"
        assert joint.find("limit") is not None
        assert upstream_joint is not None and upstream_joint.attrib["type"] == "continuous"

    states = {
        state.attrib["name"]: float(state.find("joint").attrib["value"])
        for state in srdf.findall(f"./group_state[@group='{case['gripper_group']}']")
    }
    assert states == case["presets"]

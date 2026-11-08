"""Program v3 viewer presentation uses exact frame and Scene identities."""

from copy import deepcopy
from pathlib import Path

import pytest

from manyforge_behavior.canonical_artifact import compute_content_identity, load_strict_json_yaml
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.program import Program, ProgramValidationError

ROOT = Path(__file__).resolve().parents[2]


def _program(viewer):
    document = load_strict_json_yaml(ROOT / "examples/pick_and_place_minimal.program.yaml")
    document["viewer"] = deepcopy(viewer)
    document["program_revision"] = compute_content_identity(
        document, own_field="program_revision", identity_domain="program-revision",
        api_version="manyforge.program.v3")
    return Program.from_dict(document, NodeCatalog(ROOT / "manyforge_behavior/resources/node_catalog.yaml"))


def test_direct_viewer_metadata_round_trips_distinct_frame_owners():
    viewer = {
        "frame_visibility": [
            {"frame": {"component_id": "arm_a", "frame_id": "base"}, "visible": False},
            {"frame": {"component_id": "arm_b", "frame_id": "base"}, "visible": True},
        ],
        "cell_frame_visible": False,
        "scene_resource_visibility": {"table": False},
    }
    assert _program(viewer).to_dict()["viewer"] == viewer


@pytest.mark.parametrize("viewer", [
    {"frame_visibility": [
        {"frame": {"component_id": "arm_a", "frame_id": "base"}, "visible": True},
        {"frame": {"component_id": "arm_a", "frame_id": "base"}, "visible": False}]},
    {"frame_visibility": [
        {"frame": {"component_id": "arm_b", "frame_id": "base"}, "visible": True},
        {"frame": {"component_id": "arm_a", "frame_id": "base"}, "visible": False}]},
    {"frame_visibility": [{"frame": {"component_id": "arm/a", "frame_id": "base"}, "visible": True}]},
    {"cell_frame_visible": "false"},
    {"scene_resource_visibility": {"missing": False}},
    {"frame_role_visibility": {"base": False}},
])
def test_direct_viewer_metadata_rejects_invalid_or_legacy_shape(viewer):
    with pytest.raises(ProgramValidationError):
        _program(viewer)

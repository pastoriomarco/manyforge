from __future__ import annotations

from copy import deepcopy

import pytest

from manyforge_behavior.canonical_artifact import (
    CONTENT_IDENTITY_PREFIX,
    canonical_json_bytes,
    raw_sha256_digest,
)
from manyforge_composer.backend.node_environment_client import (
    VerifiedActiveNodeEnvironment,
)
from manyforge_composer.backend.source_model_client import (
    ActiveSourceModelProjectionError,
    verify_active_source_model,
)
from manyforge_composer.backend.state import ComposerState


def _node_environment() -> VerifiedActiveNodeEnvironment:
    state = ComposerState()
    return VerifiedActiveNodeEnvironment(
        payload={
            "retained_bundle_id": "bundle.1",
            "graph_revision": "graph.1",
            "source": {
                "selection_projection_generation": 7,
                "selection_source_content_digest": "sha256:" + "a" * 64,
                "selection_source_generation": 3,
                "selection_source_owner_epoch": "manager.epoch.1",
                "selection_source_owner_id": "component-manager.cell-a",
            },
        },
        catalog=state.node_environment.catalog,
        manifest=state.node_environment.manifest,
    )


def _source() -> dict:
    return {
        "available": True,
        "freshness": "fresh",
        "known_gaps": [],
        "projection_generation": 1,
        "projection_owner_epoch": "headless.epoch.1",
        "receiver_age_ns": 10,
        "selection_projection_generation": 7,
        "selection_source_content_digest": "sha256:" + "a" * 64,
        "selection_source_generation": 3,
        "selection_source_owner_epoch": "manager.epoch.1",
        "selection_source_owner_id": "component-manager.cell-a",
        "source_content_digest": "sha256:" + "b" * 64,
        "source_generation": 1,
        "source_owner_epoch": "headless.epoch.1",
        "source_owner_id": "headless-api.source-model",
        "source_owner_kind": "source_model_projection",
    }


def _seal(payload: dict) -> dict:
    semantic = {key: value for key, value in payload.items() if key != "source"}
    payload["source"]["source_content_digest"] = raw_sha256_digest(
        b"\0".join(
            (
                CONTENT_IDENTITY_PREFIX,
                b"manyforge-source-model-projection-semantic",
                b"manyforge.source_model_projection.v1",
                canonical_json_bytes(semantic),
            )
        )
    )
    return payload


def _projection() -> dict:
    return _seal({
        "schema_version": "manyforge.source_model_projection.v1",
        "cell_id": "cell-a",
        "retained_bundle_id": "bundle.1",
        "graph_revision": "graph.1",
        "model_map_contract": {
            "contract_id": "manyforge.model-map",
            "contract_revision": "1",
        },
        "model_map_product_id": "product.model-map.1",
        "occurrences": [
            {
                "component_id": "arm.left",
                "coordinates": [
                    {
                        "coordinate_kind": "angular",
                        "joint_type": "revolute",
                        "mimic": None,
                        "source_child_link_name": "link1",
                        "source_joint_index": 0,
                        "source_joint_name": "joint_1",
                        "source_parent_link_name": "base_link",
                        "unit": "radians",
                    },
                    {
                        "coordinate_kind": "angular",
                        "joint_type": "revolute",
                        "mimic": None,
                        "source_child_link_name": "link2",
                        "source_joint_index": 1,
                        "source_joint_name": "joint_2",
                        "source_parent_link_name": "link1",
                        "unit": "radians",
                    },
                ],
                "model_asset_id": "lite6",
                "model_instance_id": "lite6.left",
                "model_revision": "model.1",
                "model_role_id": "arm_model",
                "source_model_name": "lite6",
                "source_robot_name": "lite6",
                "source_link_names": ["base_link", "link1", "link2"],
                "srdf_end_effectors": [
                    {
                        "source_end_effector_name": "lite_gripper",
                        "source_group_name": "lite6",
                        "source_parent_group_name": None,
                        "source_parent_link_name": "link2",
                    }
                ],
                "srdf_group_states": [
                    {
                        "source_group_name": "lite6",
                        "source_group_state_name": "home",
                        "source_joint_names": ["joint_1", "joint_2"],
                    }
                ],
                "srdf_groups": [
                    {
                        "chains": [
                            {
                                "source_base_link_name": "base_link",
                                "source_tip_link_name": "link2",
                            }
                        ],
                        "source_group_name": "lite6",
                        "source_joint_names": ["joint_1", "joint_2"],
                        "source_link_names": [],
                        "source_subgroup_names": [],
                    }
                ],
            }
        ],
        "targets": [
            {
                "collision_bodies": [
                    {
                        "collision_body_role_id": "arm_body",
                        "model_instance_id": "lite6.left",
                        "source_link_names": ["link1", "link2"],
                    }
                ],
                "component_id": "arm.left",
                "frames": [
                    {
                        "frame_kind": "component_model_frame",
                        "frame_role_id": "tcp",
                        "model_instance_id": "lite6.left",
                        "source_link_name": "link2",
                    }
                ],
                "model_groups": [
                    {
                        "commandable_source_joint_names": ["joint_1", "joint_2"],
                        "model_group_role_id": "arm_group",
                        "model_instance_id": "lite6.left",
                        "source_group_name": "lite6",
                        "source_joint_names": ["joint_1", "joint_2"],
                        "source_link_names": ["link1", "link2"],
                    }
                ],
                "target_id": "arm",
            }
        ],
        "source": _source(),
    })


def test_source_projection_preserves_source_local_semantics_only() -> None:
    verified = verify_active_source_model(
        _projection(),
        node_environment=_node_environment(),
    )

    occurrence = verified.payload["occurrences"][0]
    assert [
        coordinate["source_joint_name"] for coordinate in occurrence["coordinates"]
    ] == ["joint_1", "joint_2"]
    assert verified.payload["targets"][0]["model_groups"][0][
        "source_group_name"
    ] == "lite6"


def test_empty_projection_carries_no_model_map_identity() -> None:
    payload = _projection()
    payload["model_map_contract"] = None
    payload["model_map_product_id"] = None
    payload["occurrences"] = []
    payload["targets"] = []
    _seal(payload)

    assert (
        verify_active_source_model(payload, node_environment=_node_environment())
        .payload["occurrences"]
        == []
    )


def _duplicate_group_state_identity(payload: dict) -> None:
    occurrence = payload["occurrences"][0]
    duplicate = deepcopy(occurrence["srdf_group_states"][0])
    duplicate["source_joint_names"] = ["joint_1"]
    occurrence["srdf_group_states"].append(duplicate)


def _duplicate_end_effector_identity(payload: dict) -> None:
    occurrence = payload["occurrences"][0]
    duplicate = deepcopy(occurrence["srdf_end_effectors"][0])
    duplicate["source_parent_group_name"] = "lite6"
    occurrence["srdf_end_effectors"].append(duplicate)


@pytest.mark.parametrize(
    ("mutation", "message"),
    [
        (
            lambda payload: payload["occurrences"][0].update(
                {"compiled_joint_name": "instance__joint_1"}
            ),
            "private compiler/runtime name",
        ),
        (
            lambda payload: payload["source"].update(
                {"selection_source_generation": 4}
            ),
            "selection epochs disagree",
        ),
        (
            lambda payload: payload["targets"][0]["model_groups"][0].update(
                {"commandable_source_joint_names": ["joint_3"]}
            ),
            "commandable joints escape",
        ),
        (
            lambda payload: payload["occurrences"].append(
                deepcopy(payload["occurrences"][0])
            ),
            "duplicate model_instance_id",
        ),
        (
            lambda payload: payload["source"].update(
                {"source_content_digest": "sha256:" + "0" * 64}
            ),
            "source digest does not match",
        ),
        (
            _duplicate_group_state_identity,
            "srdf_group_states contains duplicate identities",
        ),
        (
            _duplicate_end_effector_identity,
            "srdf_end_effectors contains duplicate identities",
        ),
    ],
)
def test_source_projection_rejects_private_stale_or_inconsistent_facts(
    mutation,
    message: str,
) -> None:
    payload = deepcopy(_projection())
    mutation(payload)

    with pytest.raises(ActiveSourceModelProjectionError, match=message):
        verify_active_source_model(payload, node_environment=_node_environment())

"""Pure and unavailable-boundary evidence for simulation scene mirroring.

Historical source blob ``07de241694d2e7738a55d8c6c74bab1ef5d11b20``
contains the removed live client at lines 88-424, CLI at lines 427-454, and
dynamic import hook at lines 473-476.  Historical test blob
``a911cb32c772fd1e672676eb8d319213eebab8cb`` contains one live case at lines
8-147.  Its exact I9/I assignment is recorded below while the owner-neutral DTO
and translation coverage remains active.
"""

from __future__ import annotations

import inspect
from types import SimpleNamespace

import pytest

from manyforge_ros import scene_mirror_probe


HISTORICAL_LIVE_CASE = (
    "test_scene_mirror_probe_captures_entities_info_state_and_bounds",
    "lines 8-147",
    "I9/I selected simulation adapter: discover the five standard read-only "
    "services, capture features/entities/info/state/bounds, preserve filters and "
    "limits, propagate per-entity failures, and prove bounded ROS cleanup",
)


def test_scene_snapshot_records_remain_owner_neutral() -> None:
    pose = scene_mirror_probe.PoseSnapshot(
        frame_id="world",
        stamp_sec=12.5,
        position_xyz=(1.0, 2.0, 3.0),
        orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
    )
    bounds = scene_mirror_probe.BoundsSnapshot(
        type_code=1,
        type_name="TYPE_BOX",
        points_xyz=((-0.5, -0.5, -0.5), (0.5, 0.5, 0.5)),
    )
    snapshot = scene_mirror_probe.SceneMirrorSnapshot(
        service_names={"get_entities": "/get_entities"},
        features=("ENTITY_STATE_GETTING",),
        spawn_formats=("usd",),
        custom_info="offline fixture",
        entities=(
            scene_mirror_probe.EntitySnapshot(
                name="/World/block",
                category_code=2,
                category_name="CATEGORY_OBJECT",
                pose=pose,
                bounds=bounds,
            ),
        ),
    )

    payload = snapshot.to_dict()
    assert payload["entities"][0]["pose"]["frame_id"] == "world"
    assert payload["entities"][0]["bounds"]["type_name"] == "TYPE_BOX"


def test_configured_service_names_win_and_discovery_fills_gaps() -> None:
    configured = scene_mirror_probe.SimulationServiceNames(
        get_entities="/configured/get_entities",
    )
    discovered = scene_mirror_probe.SimulationServiceNames(
        get_entities="/get_entities",
        get_entity_state="/get_entity_state",
    )

    assert configured.with_discovered(discovered).to_dict() == {
        "get_simulator_features": None,
        "get_entities": "/configured/get_entities",
        "get_entity_info": None,
        "get_entity_state": "/get_entity_state",
        "get_entity_bounds": None,
    }


def test_service_selection_is_pure_and_deterministic() -> None:
    selected = scene_mirror_probe.select_simulation_service_names(
        [
            (
                "/vendor/entities",
                ["simulation_interfaces/srv/GetEntities"],
            ),
            (
                "/get_entities",
                ["simulation_interfaces/srv/GetEntities"],
            ),
            (
                "/isaacsim/GetEntityState",
                ["simulation_interfaces/srv/GetEntityState"],
            ),
        ]
    )

    assert selected.get_entities == "/get_entities"
    assert selected.get_entity_state == "/isaacsim/GetEntityState"
    assert selected.get_entity_info is None


def test_pose_and_bounds_translation_remain_ros_free() -> None:
    state = SimpleNamespace(
        header=SimpleNamespace(
            frame_id="world",
            stamp=SimpleNamespace(sec=12, nanosec=500_000_000),
        ),
        pose=SimpleNamespace(
            position=SimpleNamespace(x=1, y=2, z=3),
            orientation=SimpleNamespace(x=0, y=0, z=0, w=1),
        ),
    )

    class BoundsConstants:
        TYPE_BOX = 4

    bounds = SimpleNamespace(
        type=4,
        points=[
            SimpleNamespace(x=-0.5, y=-0.25, z=-0.1),
            SimpleNamespace(x=0.5, y=0.25, z=0.1),
        ],
    )

    assert scene_mirror_probe.pose_snapshot_from_state(state) == (
        scene_mirror_probe.PoseSnapshot(
            frame_id="world",
            stamp_sec=12.5,
            position_xyz=(1.0, 2.0, 3.0),
            orientation_xyzw=(0.0, 0.0, 0.0, 1.0),
        )
    )
    assert scene_mirror_probe.bounds_snapshot_from_value(
        bounds,
        bounds_type=BoundsConstants,
    ) == scene_mirror_probe.BoundsSnapshot(
        type_code=4,
        type_name="TYPE_BOX",
        points_xyz=((-0.5, -0.25, -0.1), (0.5, 0.25, 0.1)),
    )
    assert scene_mirror_probe.constant_name(BoundsConstants, 99) == "UNKNOWN_99"
    assert scene_mirror_probe.constant_names(BoundsConstants, [4, 99]) == [
        "TYPE_BOX",
        "UNKNOWN_99",
    ]


def test_pose_translation_is_finite_bounded_and_normalizes_xyzw() -> None:
    state = SimpleNamespace(
        header=SimpleNamespace(
            frame_id="world",
            stamp=SimpleNamespace(sec=12, nanosec=500_000_000),
        ),
        pose=SimpleNamespace(
            position=SimpleNamespace(x=1, y=2, z=3),
            orientation=SimpleNamespace(x=0, y=0, z=0, w=2),
        ),
    )

    assert scene_mirror_probe.pose_snapshot_from_state(state).orientation_xyzw == (
        0.0,
        0.0,
        0.0,
        1.0,
    )

    state.header.frame_id = "f" * 1025
    with pytest.raises(ValueError, match="frame_id exceeds"):
        scene_mirror_probe.pose_snapshot_from_state(state)
    state.header.frame_id = "world"
    state.header.stamp.nanosec = 1_000_000_000
    with pytest.raises(ValueError, match="nanosec must be in"):
        scene_mirror_probe.pose_snapshot_from_state(state)
    state.header.stamp.nanosec = 0
    state.pose.position.x = float("inf")
    with pytest.raises(ValueError, match="position.x must be finite"):
        scene_mirror_probe.pose_snapshot_from_state(state)
    state.pose.position.x = 0.0
    state.pose.orientation.w = 0.0
    with pytest.raises(ValueError, match="finite nonzero norm"):
        scene_mirror_probe.pose_snapshot_from_state(state)


def test_scene_graph_constants_and_bounds_reject_rows_over_4096() -> None:
    service_row = (
        "/get_entities",
        ["simulation_interfaces/srv/GetEntities"],
    )
    with pytest.raises(ValueError, match="service graph exceed"):
        scene_mirror_probe.select_simulation_service_names([service_row] * 4097)

    with pytest.raises(ValueError, match="feature rows exceed"):
        scene_mirror_probe.feature_enabled_or_unknown(
            {f"feature-{index}" for index in range(4097)},
            "feature-0",
        )

    class BoundsConstants:
        TYPE_BOX = 4

    with pytest.raises(ValueError, match="constant values exceed"):
        scene_mirror_probe.constant_names(BoundsConstants, [4] * 4097)

    bounds = SimpleNamespace(
        type=4,
        points=[SimpleNamespace(x=0.0, y=0.0, z=0.0)] * 4097,
    )
    with pytest.raises(ValueError, match="bounds points exceed"):
        scene_mirror_probe.bounds_snapshot_from_value(
            bounds,
            bounds_type=BoundsConstants,
        )


def test_bounds_translation_rejects_non_finite_or_boolean_values() -> None:
    class BoundsConstants:
        TYPE_BOX = 4

    with pytest.raises(ValueError, match="bounds type must be an integer"):
        scene_mirror_probe.bounds_snapshot_from_value(
            SimpleNamespace(type=True, points=[]),
            bounds_type=BoundsConstants,
        )
    with pytest.raises(ValueError, match="point.x must be finite"):
        scene_mirror_probe.bounds_snapshot_from_value(
            SimpleNamespace(
                type=4,
                points=[SimpleNamespace(x=float("nan"), y=0.0, z=0.0)],
            ),
            bounds_type=BoundsConstants,
        )


def test_absent_feature_report_is_not_mistaken_for_a_negative_report() -> None:
    assert scene_mirror_probe.feature_enabled_or_unknown(set(), "ENTITY_BOUNDS")
    assert scene_mirror_probe.feature_enabled_or_unknown(
        {"ENTITY_BOUNDS"},
        "ENTITY_BOUNDS",
    )
    assert not scene_mirror_probe.feature_enabled_or_unknown(
        {"ENTITY_STATE_GETTING"},
        "ENTITY_BOUNDS",
    )


def test_live_scene_mirror_is_not_a_dormant_callable_body(capsys) -> None:
    assert not hasattr(scene_mirror_probe, "SimulationSceneMirrorClient")
    source = inspect.getsource(scene_mirror_probe)
    for forbidden in (
        "_import_module",
        "create_client",
        "call_async",
        "create_node",
        "time.sleep",
    ):
        assert forbidden not in source

    assert scene_mirror_probe.main() == scene_mirror_probe.UNAVAILABLE_EXIT_CODE
    assert capsys.readouterr().err.strip() == scene_mirror_probe.UNAVAILABLE_MESSAGE


def test_historical_live_case_remains_assigned_to_i9() -> None:
    assert scene_mirror_probe.HISTORICAL_SOURCE_BLOB == (
        "07de241694d2e7738a55d8c6c74bab1ef5d11b20"
    )
    assert scene_mirror_probe.HISTORICAL_TEST_BLOB == (
        "a911cb32c772fd1e672676eb8d319213eebab8cb"
    )
    assert HISTORICAL_LIVE_CASE[0] == (
        "test_scene_mirror_probe_captures_entities_info_state_and_bounds"
    )
    assert HISTORICAL_LIVE_CASE[1] == "lines 8-147"
    assert HISTORICAL_LIVE_CASE[2].startswith("I9/I selected simulation adapter")

"""Closed, effect-free native target-query wire and evidence contracts."""

from copy import deepcopy

import pytest
from pydantic import ValidationError

from manyforge_headless_api.execution import ExecutionCommandClient
from manyforge_headless_api.models import TargetObservationQuery
from manyforge_headless_api.native import HeadlessApiError


def _query(kind="frame_pose"):
    return {
        "schema_version": "manyforge.target_observation_query.v1",
        "owner_epoch": "owner.1",
        "program_binding_id": "sha256:" + "a" * 64,
        "target_role_id": "arm" if kind == "frame_pose" else "gripper",
        "kind": kind,
        **({"frame_role_id": "tcp"} if kind == "frame_pose" else {}),
    }


def _response(query, coordinate_kind="angular_rad"):
    value = (
        {
            "position_m": {"x_m": 0.1, "y_m": 0.2, "z_m": 0.3},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        }
        if query["kind"] == "frame_pose"
        else {
            "state": "completed",
            "command_id": 1,
            "generation": 2,
            "revision": 3,
            "active_control_configuration_id": "configuration.1",
            "active_control_configuration_generation": 4,
            "source_id": "gripper.provider.1",
            "source_revision": "sha256:" + "b" * 64,
            "source_epoch": "source.1",
            "grasp_confirmed": None,
            "observed_position": {
                "kind": coordinate_kind,
                "value_rad" if coordinate_kind == "angular_rad" else "value_m": 0.02,
            },
        }
    )
    return {
        "ok": True,
        "command": "query_target",
        "api_version": "manyforge.execution_command_response.v1",
        "target_observation": {
            **query,
            "schema_version": "manyforge.target_observation.v1",
            "source_epoch": "source.1",
            "source_sequence": 7,
            "value": value,
        },
    }


def _client(response):
    client = object.__new__(ExecutionCommandClient)
    requests = []

    def exchange(request):
        requests.append(deepcopy(request))
        return deepcopy(response)

    client._exchange = exchange
    return client, requests


@pytest.mark.parametrize(
    "kind,coordinate",
    [
        ("frame_pose", "angular_rad"),
        ("gripper_status", "angular_rad"),
        ("gripper_status", "linear_m"),
    ],
)
def test_query_has_one_exact_read_and_preserves_typed_owner_evidence(kind, coordinate):
    query = _query(kind)
    response = _response(query, coordinate)
    client, requests = _client(response)
    assert client.query_target(TargetObservationQuery.model_validate(query)) == response
    assert requests == [
        {
            "api_version": "manyforge.execution_command.v1",
            "command": "query_target",
            "query": query,
        }
    ]


def test_frame_query_may_request_an_exact_relative_frame():
    query = {**_query(), "relative_to_frame_role_id": "base"}
    response = _response(query)
    client, requests = _client(response)

    assert client.query_target(TargetObservationQuery.model_validate(query)) == response
    assert requests[0]["query"] == query


@pytest.mark.parametrize(
    "change",
    [
        {"kind": "dispatch"},
        {"owner_epoch": ""},
        {"owner_epoch": 123},
        {"program_binding_id": "other"},
        {"target_role_id": ""},
        {"frame_role_id": None},
        {"request_id": "effect"},
        {"schema_version": "manyforge.target_observation_query.v2"},
    ],
)
def test_query_rejects_unknown_or_crossed_request_shapes(change):
    with pytest.raises(ValidationError):
        TargetObservationQuery.model_validate({**_query(), **change})


def test_gripper_query_cannot_name_an_arbitrary_frame():
    with pytest.raises(ValidationError):
        TargetObservationQuery.model_validate(
            {**_query("gripper_status"), "frame_role_id": "tcp"}
        )
    with pytest.raises(ValidationError):
        TargetObservationQuery.model_validate(
            {
                **_query("gripper_status"),
                "relative_to_frame_role_id": "base",
            }
        )


@pytest.mark.parametrize(
    "change",
    [
        {"source_epoch": "s" * 257},
        {"source_epoch": "e\u0301"},
        {"source_epoch": ""},
        {"source_sequence": 0},
        {"source_sequence": True},
        {"source_sequence": 2**53},
        {"extra": None},
        {"value": None},
    ],
)
def test_client_rejects_unqualified_or_mismatched_owner_response(change):
    query = _query()
    response = _response(query)
    response["target_observation"].update(change)
    client, _ = _client(response)
    with pytest.raises(HeadlessApiError) as caught:
        client.query_target(TargetObservationQuery.model_validate(query))
    assert caught.value.code == "execution_target_observation_invalid"


@pytest.mark.parametrize(
    "kind,bad_value",
    [
        (
            "frame_pose",
            {
                "position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
                "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 0.0},
            },
        ),
        (
            "gripper_status",
            {
                "state": "grasp_confirmed",
                "command_id": 1,
                "generation": 1,
                "revision": 1,
                "grasp_confirmed": "yes",
                "observed_position": {"kind": "angular_rad", "value_m": 0.1},
            },
        ),
    ],
)
def test_client_rejects_invalid_units_and_never_repairs_bad_evidence(kind, bad_value):
    query = _query(kind)
    response = _response(query)
    response["target_observation"]["value"] = bad_value
    client, _ = _client(response)
    with pytest.raises(HeadlessApiError):
        client.query_target(TargetObservationQuery.model_validate(query))


def test_owner_rejection_is_not_retried_or_replaced_by_cached_state():
    client, requests = _client(
        {
            "ok": False,
            "error": {
                "code": "observation_expired",
                "message": "source is not current",
            },
        }
    )
    with pytest.raises(HeadlessApiError) as caught:
        client.query_target(TargetObservationQuery.model_validate(_query()))
    assert caught.value.code == "observation_expired"
    assert len(requests) == 1


_ROBOT = {
    "target_role_id": "peer",
    "controllable_component_id": "robot.2",
    "command_target_id": "arm.command",
}


def _transform_query(**change):
    return {
        "schema_version": "manyforge.target_observation_query.v1",
        "owner_epoch": "owner.1",
        "program_binding_id": "sha256:" + "a" * 64,
        "kind": "frame_transform",
        "pose": {
            "position_m": {"x_m": 0.1, "y_m": 0.0, "z_m": 0.2},
            "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
        },
        "source_frame": {"type": "frame_role", "frame_role_id": "tcp", "target": _ROBOT},
        "target_frame": {"type": "cell_frame"},
        **change,
    }


def _transform_response(query):
    return {
        "ok": True,
        "command": "query_target",
        "api_version": "manyforge.execution_command_response.v1",
        "target_observation": {
            **query,
            "schema_version": "manyforge.target_observation.v1",
            "evaluated_at_ns": "18446744073",
            "scene_revision": {
                "scene_id": "sha256:" + "c" * 64,
                "revision_id": 3,
                "content_hash": "sha256:" + "d" * 64,
            },
            "resolution_evidence_digests": ["sha256:" + "e" * 64],
            "value": {
                "position_m": {"x_m": 1.0, "y_m": 2.0, "z_m": 3.0},
                "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
            },
        },
    }


def test_frame_transform_names_two_frames_and_no_target_and_keeps_its_answer_shape():
    query = _transform_query()
    response = _transform_response(query)
    client, requests = _client(response)

    assert client.query_target(TargetObservationQuery.model_validate(query)) == response
    assert requests[0]["query"] == query


@pytest.mark.parametrize(
    "change",
    [
        {"target_role_id": "arm"},
        {"frame_role_id": "tcp"},
        {"pose": None},
        {"target_frame": None},
        # A transform frame role always names its robot.
        {"source_frame": {"type": "frame_role", "frame_role_id": "tcp"}},
        {"source_frame": {"type": "pose", "pose": None}},
        {"source_frame": {"type": "frame_role", "frame_role_id": "tcp", "target": {}}},
    ],
)
def test_frame_transform_query_is_closed(change):
    with pytest.raises(ValidationError):
        TargetObservationQuery.model_validate(_transform_query(**change))
    with pytest.raises(ValidationError):
        TargetObservationQuery.model_validate({**_query(), "pose": _transform_query()["pose"]})


@pytest.mark.parametrize(
    "change",
    [
        {"source_epoch": "source.1"},
        {"evaluated_at_ns": -1},
        {"scene_revision": None},
        {"resolution_evidence_digests": ["sha256:" + "e" * 64] * 2},
        {"resolution_evidence_digests": ["not a digest"]},
        {"value": {"position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
                   "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 0.0}}},
    ],
)
def test_frame_transform_answer_must_echo_one_evaluation(change):
    query = _transform_query()
    response = _transform_response(query)
    response["target_observation"].update(change)
    client, _ = _client(response)
    with pytest.raises(HeadlessApiError) as caught:
        client.query_target(TargetObservationQuery.model_validate(query))
    assert caught.value.code == "execution_target_observation_invalid"

"""The one bounded failure representation shared by every run record."""

from __future__ import annotations

from types import MappingProxyType

import pytest
import rfc8785

from manyforge_behavior.failure_details import (
    FAILURE_DETAIL_KEYS,
    FailureRecord,
    NodeFailureOrigin,
    bounded_failure_details,
    exception_failure_details,
    failure_origin,
    require_failure_details,
    require_failure_origin,
)
from manyforge_behavior.orchestration_values import (
    NodeInputError,
    input_failure_details,
)


def test_registered_keys_are_the_contract_set() -> None:
    assert FAILURE_DETAIL_KEYS == (
        "reason",
        "reference_type",
        "reference_id",
        "target_role_id",
        "input_problem",
        "path",
        "source_kind",
        "source_reference",
        "rule_id",
        "component_id",
        "retryable",
        "required_action",
    )


def test_only_registered_flat_bounded_entries_are_carried() -> None:
    bounded = bounded_failure_details(
        {
            "reason": "observed_state_unavailable",
            "retryable": False,
            "path": None,
            "rule_id": 7,
            "reference_id": 1.5,
            # Unregistered keys are not carried.
            "stack": "Traceback ...",
            "payload": "raw bytes",
            # Nested objects and arrays are not carried.
            "reference_type": {"kind": "frame"},
            "target_role_id": ["arm.left"],
            # Neither are non-finite numbers or integers beyond I-JSON.
            "required_action": float("nan"),
            "source_reference": 2**53,
            "source_kind": float("inf"),
        }
    )
    assert isinstance(bounded, MappingProxyType)
    # Registered order, whatever order the producer used.
    assert list(bounded.items()) == [
        ("reason", "observed_state_unavailable"),
        ("reference_id", 1.5),
        ("path", None),
        ("rule_id", 7),
        ("retryable", False),
    ]


def test_an_oversized_string_is_dropped_never_shortened() -> None:
    fits = "r" * 256
    bounded = bounded_failure_details(
        {"reference_id": fits, "path": "p" * 257, "reason": "é" * 129}
    )
    # 256 bytes fit; 257 bytes, or 129 two-byte characters, are dropped.
    assert dict(bounded) == {"reference_id": fits}


def test_the_canonical_json_bound_drops_entries_that_do_not_fit() -> None:
    long = "x" * 250
    details = {key: long for key in FAILURE_DETAIL_KEYS}
    bounded = bounded_failure_details(details)
    assert bounded is not None
    assert len(rfc8785.dumps(dict(bounded))) <= 2_048
    # Registered order decides which entries fit; nothing is shortened.
    assert list(bounded) == list(FAILURE_DETAIL_KEYS[: len(bounded)])
    assert all(value == long for value in bounded.values())
    assert len(bounded) < len(FAILURE_DETAIL_KEYS)


@pytest.mark.parametrize(
    "details", [None, {}, {"stack": "x"}, {"path": {"nested": 1}}, ["reason"], "reason"]
)
def test_nothing_carried_is_null(details: object) -> None:
    assert bounded_failure_details(details) is None


def test_errors_carry_their_details_or_their_owner_response_details() -> None:
    input_error = NodeInputError(
        "blackboard key 'delay' has no value",
        details=input_failure_details(
            input_problem="missing",
            path="duration_s",
            source_kind="blackboard_key",
            source_reference="delay",
        ),
    )
    assert dict(exception_failure_details(input_error)) == {
        "input_problem": "missing",
        "path": "duration_s",
        "source_kind": "blackboard_key",
        "source_reference": "delay",
    }

    class _Rejected(RuntimeError):
        response = {
            "error": {
                "code": "reference_unavailable",
                "details": {"reason": "frame_missing", "trace": ["owner"]},
            }
        }

    assert dict(exception_failure_details(_Rejected("refused"))) == {
        "reason": "frame_missing"
    }
    assert exception_failure_details(RuntimeError("plain")) is None


def test_origins_name_their_node_and_only_owners_name_an_api() -> None:
    assert dict(
        failure_origin("owner", node_instance_id="move", owner_api="kernel.operation")
    ) == {"kind": "owner", "node_instance_id": "move", "owner_api": "kernel.operation"}
    assert dict(failure_origin("program_runner")) == {
        "kind": "program_runner",
        "node_instance_id": None,
        "owner_api": None,
    }
    for kind in ("program_node", "owner"):
        with pytest.raises(ValueError, match="names its node instance"):
            failure_origin(kind)
    with pytest.raises(ValueError, match="owner API"):
        failure_origin("program_node", node_instance_id="n", owner_api="scene.query")
    with pytest.raises(ValueError, match="bounded text"):
        failure_origin("program_runner", node_instance_id="n" * 257)
    with pytest.raises(ValueError, match="unsupported"):
        failure_origin("browser")  # type: ignore[arg-type]


@pytest.mark.parametrize(
    "origin",
    [
        {"kind": ["owner"], "node_instance_id": "n", "owner_api": None},
        {"kind": "owner", "node_instance_id": None, "owner_api": None},
        {"kind": "program_runner", "node_instance_id": None},
        {"kind": "program_runner", "node_instance_id": None, "owner_api": None, "x": 1},
        "program_runner",
    ],
)
def test_record_origins_refuse_any_other_shape(origin: object) -> None:
    with pytest.raises(ValueError):
        require_failure_origin(origin)


def test_a_node_names_a_partial_origin_the_run_completes() -> None:
    partial = NodeFailureOrigin("owner", owner_api="scene.transaction")
    assert dict(partial.for_node("remove_table")) == {
        "kind": "owner",
        "node_instance_id": "remove_table",
        "owner_api": "scene.transaction",
    }
    with pytest.raises(ValueError):
        NodeFailureOrigin("program_runner")
    with pytest.raises(ValueError):
        NodeFailureOrigin("program_node", owner_api="scene.query")


def test_records_accept_only_already_bounded_details() -> None:
    assert require_failure_details(None) is None
    assert dict(require_failure_details({"path": "duration_s"})) == {
        "path": "duration_s"
    }
    for details in ({}, {"stack": "x"}, {"path": "p" * 257}, {"path": [1]}):
        with pytest.raises(ValueError, match="outside their bound"):
            require_failure_details(details)
    record = FailureRecord(
        "node_input_unavailable",
        "missing input duration_s",
        {"path": "duration_s"},
        {"kind": "program_node", "node_instance_id": "wait", "owner_api": None},
    )
    assert dict(record.details) == {"path": "duration_s"}
    with pytest.raises(ValueError):
        FailureRecord("", "message", None, None)

"""Scene changes into History from the Manager's status stream (OD-400)."""

from __future__ import annotations

from typing import Any

from manyforge_headless_api.activity_history import (
    bind_request_caller,
    scene_change_record,
    unbind_request_caller,
    with_caller,
)
from manyforge_headless_api.scene_change_history import (
    ManagerSceneChangeRecorder,
    last_recorded_scene_change,
    scene_changes_lost_record,
)

_START = "0123456789abcdef"
_RUNNER = "manyforge.behavior.program_scene"


class _Core:
    def __init__(self) -> None:
        self.activity: list[dict[str, Any]] = []

    def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
        self.activity.append(record)
        return {"recorded": True}


def _change(sequence: int, **changes: Any) -> dict[str, Any]:
    """One record of the Manager's `scene_changes` section."""

    return {
        "duration_ms": 1.5, "error": None, "operation_kind": "remove_object",
        "outcome": "succeeded", "principal_id": None, "request_id": None,
        "scene_resource_id": f"part.{sequence}", "scene_revision_id": sequence + 1,
        "sequence": sequence, "source_label": _RUNNER, "transport": None, **changes,
    }


def _value(*records: dict[str, Any], start: str = _START) -> dict[str, Any]:
    return {"application_start_id": start, "records": list(records)}


def _sequences(core: _Core) -> list[int | None]:
    return [record.get("scene_change", {}).get("sequence") for record in core.activity]


def test_a_runner_reset_reload_of_n_objects_is_n_events_as_its_route_recorded_them() -> None:
    """The Runner's Reset reload or clear: one event per object, the content
    its HTTP record route wrote, with the Manager start and sequence added
    and no HTTP caller (the Runner sent no record of its own)."""

    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    changes = [_change(sequence) for sequence in (1, 2, 3)]
    recorder.observe(_value(*changes))
    assert core.activity == [
        with_caller(scene_change_record("remove_object", "succeeded", None, {
            "scene_resource_id": change["scene_resource_id"], "source_label": _RUNNER,
            "principal_id": None, "duration_ms": 1.5,
            "scene_revision_id": change["scene_revision_id"],
            "manager_start_id": _START, "sequence": change["sequence"],
        }), source_label=_RUNNER)
        for change in changes
    ]
    assert all(record["caller"] == {"source_label": _RUNNER} for record in core.activity)


def test_a_gui_commit_is_the_event_headless_recorded_for_its_request() -> None:
    """The caller the commit carried to the Manager names the HTTP request
    as Headless's own scene route named it."""

    refused = {"code": "stale_revision", "message": "Scene changed",
               "details": {"expected": "3", "current": "4"}}
    change = _change(
        1, operation_kind="set_collision_policy", outcome="refused", error=refused,
        scene_resource_id=None, scene_revision_id=None, principal_id="operator.7",
        request_id="req-7", transport="mcp", source_label="composer_ui")
    core = _Core()
    ManagerSceneChangeRecorder(core, last_recorded=None).observe(_value(change))
    token = bind_request_caller("req-7", "mcp")
    try:
        expected = with_caller(scene_change_record("set_collision_policy", "refused", refused, {
            "scene_resource_id": None, "source_label": "composer_ui",
            "principal_id": "operator.7", "duration_ms": 1.5, "scene_revision_id": None,
            "manager_start_id": _START, "sequence": 1,
        }), source_label="composer_ui", principal_id="operator.7")
    finally:
        unbind_request_caller(token)
    assert core.activity == [expected]
    assert expected["caller"] == {"request_id": "req-7", "transport": "mcp",
                                  "source_label": "composer_ui", "principal_id": "operator.7"}
    assert expected["message"] == (
        "Scene set collision policy refused: stale_revision: Scene changed · composer_ui · 2 ms")


def test_each_change_is_recorded_once_however_the_values_overlap() -> None:
    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    recorder.observe(_value(_change(1)))
    # A coalesced value, the same value again after a reconnect, then a newer one.
    recorder.observe(_value(_change(1), _change(2), _change(3)))
    recorder.observe(_value(_change(1), _change(2), _change(3)))
    recorder.observe(_value(_change(2), _change(3), _change(4)))
    assert _sequences(core) == [1, 2, 3, 4]


def test_changes_that_left_the_ring_unseen_are_one_lost_event_with_their_count() -> None:
    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    recorder.observe(_value(_change(1)))
    recorder.observe(_value(*(_change(sequence) for sequence in range(40, 72))))
    assert core.activity[1] == scene_changes_lost_record(38)
    assert core.activity[1]["records_lost"] == {"lost_count": 38}
    assert core.activity[1]["message"] == "Scene change history lost 38 records"
    assert _sequences(core) == [1, None, *range(40, 72)]


def test_a_new_manager_start_counts_from_its_first_change() -> None:
    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    recorder.observe(_value(_change(1), _change(2)))
    recorder.observe(_value(_change(1), start="fedcba9876543210"))
    assert _sequences(core) == [1, 2, 1]
    assert core.activity[-1]["scene_change"]["manager_start_id"] == "fedcba9876543210"


def test_after_a_headless_restart_it_goes_on_after_the_last_recorded_change() -> None:
    held = {"events": [
        # Newest first; a failure without a Manager outcome has no sequence.
        {"event": {"payload": {"scene_change": {"manager_start_id": None, "sequence": None}}}},
        {"event": {"payload": {"scene_change": {"manager_start_id": _START, "sequence": 5}}}},
        {"event": {"payload": {"scene_change": {"manager_start_id": _START, "sequence": 4}}}},
    ]}
    last = last_recorded_scene_change(lambda: held)
    assert last == (_START, 5)
    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=last)
    recorder.observe(_value(*(_change(sequence) for sequence in range(1, 8))))
    assert _sequences(core) == [6, 7]
    # The last change of another Manager start says nothing about this one.
    other = _Core()
    ManagerSceneChangeRecorder(other, last_recorded=("fedcba9876543210", 5)).observe(
        _value(_change(1), _change(2)))
    assert _sequences(other) == [1, 2]
    assert last_recorded_scene_change(lambda: {"events": []}) is None


def test_after_a_restart_changes_gone_while_headless_was_down_are_a_lost_event() -> None:
    core = _Core()
    ManagerSceneChangeRecorder(core, last_recorded=(_START, 5)).observe(
        _value(*(_change(sequence) for sequence in range(20, 52))))
    assert core.activity[0] == scene_changes_lost_record(14)
    assert _sequences(core)[1:] == list(range(20, 52))


def test_an_unusable_record_loses_its_lines_never_the_stream() -> None:
    core = _Core()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    recorder.observe({"application_start_id": _START, "records": [{"sequence": 1}]})
    assert core.activity == []
    recorder.observe(_value(_change(1)))
    assert _sequences(core) == [1]


def test_a_full_history_queue_never_holds_the_follower() -> None:
    """The historian refuses at once when its bounded queue is full (its own
    lost-records event); the recorder goes on."""

    class Full(_Core):
        def record_activity(self, record: dict[str, Any]) -> dict[str, Any]:
            super().record_activity(record)
            return {"recorded": False}

    core = Full()
    recorder = ManagerSceneChangeRecorder(core, last_recorded=None)
    recorder.observe(_value(_change(1), _change(2)))
    recorder.observe(_value(_change(2), _change(3)))
    assert _sequences(core) == [1, 2, 3]

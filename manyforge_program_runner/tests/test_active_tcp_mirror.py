"""Ordering of confirmed Active TCP baseline, result, and event values."""

from copy import deepcopy
from threading import Event, Thread

import pytest

from manyforge_program_runner.active_tcp_mirror import ActiveTcpMirror, ActiveTcpWireError


_TCP_A = {"component_id": "tool", "target_id": "gripper", "frame_role_id": "tcp"}
_TCP_B = {**_TCP_A, "frame_role_id": "tip"}


def test_result_prevents_an_older_event_or_baseline_from_rolling_back_the_store() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    mirror.apply_baseline("owner-1", {"arm": (1, 2, _TCP_A), "other_arm": (1, 1, None)})
    assert mirror.apply("arm", "owner-1", 1, 4, _TCP_B)
    assert not mirror.apply("arm", "owner-1", 1, 3, _TCP_A)
    assert not mirror.apply("arm", "owner-1", 1, 4, _TCP_A)
    assert mirror.apply("arm", "owner-1", 1, 4, _TCP_B)
    mirror.apply_baseline("owner-1", {"arm": (1, 2, _TCP_A), "other_arm": (1, 2, _TCP_A)})
    assert mirror.current() == {"arm": (1, 4, _TCP_B), "other_arm": (1, 2, _TCP_A)}
    assert writes == [
        ("arm", _TCP_A), ("other_arm", None), ("arm", _TCP_B),
        ("other_arm", _TCP_A),
    ]


def test_reset_none_and_detached_inputs_and_snapshots() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    entry = deepcopy(_TCP_A)
    assert mirror.apply("arm", "owner-1", 1, 0, entry)
    entry["frame_role_id"] = "changed"
    snapshot = mirror.current()
    snapshot["arm"][2]["frame_role_id"] = "changed_again"
    assert mirror.current()["arm"] == (1, 0, _TCP_A)
    assert mirror.apply("arm", "owner-1", 1, 1, None)
    assert mirror.current()["arm"] == (1, 1, None)
    assert writes[-1] == ("arm", None)


def test_partial_store_write_refuses_stale_and_incomplete_receipts() -> None:
    stores = {}
    fail = [True]

    def write(robot, entry):
        stores["first"] = (robot, entry)
        if fail[0]:
            raise RuntimeError("store unavailable")
        stores["second"] = (robot, entry)

    mirror = ActiveTcpMirror(on_change=write)
    mirror.set_owner("owner-1")
    with pytest.raises(RuntimeError):
        mirror.apply("arm", "owner-1", 1, 5, _TCP_A)
    assert mirror.current()["arm"] == (1, 5, _TCP_A)
    assert not mirror.apply("arm", "owner-1", 1, 4, _TCP_B)
    with pytest.raises(RuntimeError, match="store unavailable"):
        mirror.apply("arm", "owner-1", 1, 5, _TCP_A)
    assert stores["first"] == ("arm", _TCP_A)
    fail[0] = False
    assert mirror.apply("arm", "owner-1", 1, 6, _TCP_B)
    assert stores == {"first": ("arm", _TCP_B), "second": ("arm", _TCP_B)}


def test_registered_store_seed_cannot_overwrite_a_later_update() -> None:
    writes = []
    entered, release = Event(), Event()
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    mirror.apply("arm", "owner-1", 1, 1, _TCP_A)
    writes.clear()

    def seed(robot, entry):
        entered.set()
        assert release.wait(2)
        writes.append((robot, entry))

    seed_thread = Thread(target=lambda: mirror.seed(seed))
    seed_thread.start()
    assert entered.wait(2)
    update_thread = Thread(target=lambda: mirror.apply("arm", "owner-1", 1, 2, _TCP_B))
    update_thread.start()
    release.set()
    seed_thread.join(2)
    update_thread.join(2)
    assert not seed_thread.is_alive() and not update_thread.is_alive()
    assert writes == [("arm", _TCP_A), ("arm", _TCP_B)]


def test_registration_and_fenced_owner_replacement_ignore_late_old_records() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-z")
    assert mirror.apply("arm", "owner-z", 8, 12, _TCP_A)
    assert mirror.apply("arm", "owner-z", 9, 0, _TCP_B)
    assert not mirror.apply("arm", "owner-z", 8, 999, _TCP_A)
    mirror.set_owner("owner-a")  # Epoch names have no lexical time order.
    assert mirror.current()["arm"] == (None, None, None)
    assert writes[-1] == ("arm", None)
    assert not mirror.apply("arm", "owner-z", 99, 999, _TCP_A)
    assert mirror.apply("arm", "owner-a", 1, 0, _TCP_A)
    assert mirror.current()["arm"] == (1, 0, _TCP_A)


def test_host_rows_and_active_tcp_event_use_the_same_per_robot_stamp() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    row = {
        "component_id": "arm", "owner_epoch": "owner-1",
        "registration_generation": 3, "sequence": 2, "entry": _TCP_A,
    }
    assert mirror.apply_rows([row]) == {"arm": True}
    event = {
        "record_kind": "active_tcp_changed", "sequence": 42,
        "payload": {
            **row, "api_version": "manyforge.active_tcp_change.v1",
            "previous": _TCP_A, "entry": _TCP_B, "sequence": 3,
            "source": "program", "offset_sent": True,
        },
    }
    assert mirror.apply_event("owner-1", event) is True
    assert mirror.apply_rows([row]) == {"arm": False}
    assert mirror.apply_result_row(row) is True  # A genuinely newer value won.
    assert mirror.current()["arm"] == (3, 3, _TCP_B)
    assert not mirror.apply_result_row({**row, "sequence": 3})  # Equal, different value.
    assert mirror.apply_result_row({**row, "owner_epoch": "old-owner"}) is None
    assert mirror.apply_event("owner-1", {"record_kind": "operation_terminal"}) is None
    with pytest.raises(ActiveTcpWireError):
        mirror.apply_rows([{key: value for key, value in row.items() if key != "entry"}])
    with pytest.raises(ActiveTcpWireError):
        mirror.apply_event("owner-2", event)


def test_stream_loss_unsets_value_and_an_authoritative_equal_result_restores_it() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    row = {
        "component_id": "arm", "owner_epoch": "owner-1",
        "registration_generation": 2, "sequence": 3, "entry": _TCP_A,
    }
    assert mirror.apply_result_row(row)
    mirror.stream_lost()
    assert writes[-1] == ("arm", None)
    assert mirror.current()["arm"] == (2, 3, None)
    assert mirror.apply_result_row(row)
    assert writes[-1] == ("arm", _TCP_A)


def test_same_confirmed_row_retries_incomplete_delivery() -> None:
    delivered = []

    def write(robot, entry):
        if not delivered:
            delivered.append("failed")
            raise RuntimeError("subscriber unavailable")
        delivered.append((robot, entry))

    mirror = ActiveTcpMirror(on_change=write)
    mirror.set_owner("owner-1")
    entry = {"component_id": "robot", "target_id": "arm", "frame_role_id": "flange"}
    row = {"component_id": "robot", "owner_epoch": "owner-1", "registration_generation": 1, "sequence": 1, "entry": entry}
    with pytest.raises(RuntimeError):
        mirror.apply_result_row(row)
    assert mirror.apply_result_row(row) is True
    assert delivered[-1] == ("robot", entry)


def test_reset_certifies_bound_rows_and_accepts_a_newer_current_value() -> None:
    writes = []
    mirror = ActiveTcpMirror(on_change=lambda robot, entry: writes.append((robot, entry)))
    mirror.set_owner("owner-1")
    row = {
        "component_id": "arm", "owner_epoch": "owner-1",
        "registration_generation": 1, "sequence": 1, "entry": _TCP_A,
    }
    mirror.apply_reset_rows([], set())
    with pytest.raises(ActiveTcpWireError, match="omitted"):
        mirror.apply_reset_rows([row], {"arm", "other_arm"})
    with pytest.raises(ActiveTcpWireError, match="duplicate"):
        mirror.apply_reset_rows([row, row], {"arm"})
    with pytest.raises(ActiveTcpWireError, match="stale owner"):
        mirror.apply_reset_rows([{**row, "owner_epoch": "old"}], {"arm"})
    mirror.apply_reset_rows([row], {"arm"})
    assert writes[-1] == ("arm", _TCP_A)
    assert mirror.apply("arm", "owner-1", 1, 2, _TCP_B)
    mirror.apply_reset_rows([row], {"arm"})
    assert writes[-1] == ("arm", _TCP_B)
    with pytest.raises(ActiveTcpWireError, match="could not be delivered"):
        mirror.apply_reset_rows([{**row, "sequence": 2}], {"arm"})

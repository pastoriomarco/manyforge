"""Every stored-value change is made in memory and reaches the disk afterwards,
written by the register's writer thread, never on the changing thread."""

from __future__ import annotations

import json
import logging
import os
import stat
import threading
from pathlib import Path
from types import MappingProxyType
from typing import Any

import pytest

import manyforge_program_runner.persistent_keys as persistent_keys
from manyforge_behavior.node_environment import build_node_environment
from manyforge_behavior.orchestration_values import ProgramValueStore
from manyforge_behavior.program import BlackboardKeyDeclaration, Program
from manyforge_program_runner.persistent_keys import (
    CommittedActivation,
    PersistentKeyEdit,
    PersistentKeyRegister,
)
from manyforge_program_runner.tests.test_persistent_values import (
    _PROJECT,
    _Owner,
    _key,
    _program,
)

_A = "3f9a00000000002a"
_SCENARIO = "/cells/a/cell.scenario"


def _declaration(key: str) -> BlackboardKeyDeclaration:
    return BlackboardKeyDeclaration(
        key=key,
        type="int64",
        access="read_write",
        persistence="persistent",
        fields=MappingProxyType({}),
    )


def _node(attempt: int = 1) -> dict[str, Any]:
    return {"program_run_id": 1, "node_instance_id": "set_index", "node_attempt_id": attempt}


def _on_disk(register: PersistentKeyRegister, project: str = _SCENARIO) -> dict[str, Any]:
    document = json.loads(register.path.read_text())
    return {row["key"]: row["value"] for row in document["rows"] if row["project_path"] == project}


def _forbid_disk_on(thread: threading.Thread, monkeypatch: pytest.MonkeyPatch) -> list[str]:
    """Fail any fsync or rename made by ``thread``; record where they ran."""

    calls: list[str] = []
    fsync, replace = persistent_keys.os.fsync, persistent_keys.os.replace

    def guarded(name: str, call: Any) -> Any:
        def wrapper(*args: Any) -> Any:
            calls.append(threading.current_thread().name)
            if threading.current_thread() is thread:
                raise AssertionError(f"{name} on the writing thread")
            return call(*args)

        return wrapper

    monkeypatch.setattr(persistent_keys.os, "fsync", guarded("fsync", fsync))
    monkeypatch.setattr(persistent_keys.os, "replace", guarded("replace", replace))
    return calls


_OPERATOR = PersistentKeyEdit("0" * 31 + "1", "operator_ui")


def _activation(*declarations: BlackboardKeyDeclaration, digest: str = "h.apply.1") -> CommittedActivation:
    return CommittedActivation(
        record_digest=digest,
        project_path=_SCENARIO,
        declarations={row.key: row for row in declarations},
        apply_id=int(digest.rsplit(".", 1)[1]),
    )


_CHANGES: dict[str, Any] = {
    "node_write": lambda register: register.commit(
        _SCENARIO, _declaration("index"), 2, edit=_node()
    ),
    "operator_set": lambda register: register.commit(
        _SCENARIO, _declaration("index"), 2, edit=_OPERATOR
    ),
    # The grasp-lost copy: a run's write without a node instance.
    "grasp_lost_copy": lambda register: register.commit(
        _SCENARIO,
        _declaration("index"),
        2,
        edit={"program_run_id": 1, "writer": "grasp_lost", "loss_event_id": "loss.1"},
    ),
    "operator_reset": lambda register: register.remove(_SCENARIO, "index", edit=_OPERATOR),
    "operator_clear": lambda register: register.clear(
        _SCENARIO, {"index": _declaration("index")}, edit=_OPERATOR
    ),
    "reset_to_initial": lambda register: register.reset_to_initial(
        _SCENARIO, {"index": _declaration("index")}, edit=_OPERATOR
    ),
    "run_start_drop": lambda register: register.drop(
        _SCENARIO, "index", reason="value_invalid", edit={"program_run_id": 1}
    ),
    # An Apply notification that drops the stored value.
    "reconcile": lambda register: register.reconcile(_activation(digest="h.apply.2")),
}


@pytest.mark.parametrize("change", sorted(_CHANGES))
def test_no_change_touches_the_disk_on_the_changing_thread(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, change: str
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert register.reconcile(_activation(_declaration("index"))).outcome == "reconciled"
    register.commit(_SCENARIO, _declaration("index"), 1)
    assert register.wait_written(5.0)
    sequence = register.sequence
    calls = _forbid_disk_on(threading.current_thread(), monkeypatch)

    _CHANGES[change](register)
    # Memory has the change at once, as every reader reads it.
    assert register.sequence == sequence + 1
    expected = {"index": 2} if register.values(_SCENARIO) else {}
    assert {key: row["value"] for key, row in register.values(_SCENARIO).items()} == expected

    assert register.wait_written(5.0)
    assert _on_disk(register) == expected
    assert calls and set(calls) == {"persistent-key-writer"}
    assert register.write_failures == 0


def test_a_change_never_waits_for_a_write_in_progress(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    entered, release = threading.Event(), threading.Event()
    replace = persistent_keys.os.replace

    def held(*args: Any) -> None:
        entered.set()
        assert release.wait(5.0)
        replace(*args)

    monkeypatch.setattr(persistent_keys.os, "replace", held)
    register.commit(_SCENARIO, _declaration("index"), 1, edit=_node(1))
    assert entered.wait(5.0)  # The writer is inside its disk write.

    def changes() -> None:
        register.commit(_SCENARIO, _declaration("index"), 2, edit=_node(2))
        register.commit(_SCENARIO, _declaration("layer"), 5, edit=_OPERATOR)
        register.reconcile(_activation(_declaration("index"), _declaration("layer")))
        register.remove(_SCENARIO, "layer", edit=_OPERATOR)

    second = threading.Thread(target=changes)
    second.start()
    second.join(5.0)
    assert not second.is_alive()
    assert {key: row["value"] for key, row in register.values(_SCENARIO).items()} == {
        "index": 2
    }

    release.set()
    assert register.wait_written(5.0)
    # Several pending changes fold into a rewrite of the latest state.
    assert _on_disk(register) == {"index": 2}
    assert [edit["action"] for edit in register.edits(_SCENARIO)] == [
        "set", "set", "set", "reset"
    ]
    assert PersistentKeyRegister(tmp_path, application_start_id=_A).sequence == register.sequence


def test_the_latest_values_are_on_disk_after_a_quiet_moment_and_after_close(
    tmp_path: Path,
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    for value in range(1, 51):
        register.commit(_SCENARIO, _declaration("index"), value, edit=_node(value))
    assert register.wait_written(5.0)
    assert _on_disk(register) == {"index": 50}

    for value in range(51, 101):
        register.commit(_SCENARIO, _declaration("count"), value, edit=_node(value))
    register.close()
    reopened = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert {key: row["value"] for key, row in reopened.values(_SCENARIO).items()} == {
        "index": 50,
        "count": 100,
    }
    assert reopened.sequence == register.sequence


def _blocked_replace(monkeypatch: pytest.MonkeyPatch) -> tuple[threading.Event, threading.Event]:
    """Hold the writer inside its file replace until released (in-code point)."""

    entered, release = threading.Event(), threading.Event()
    replace = persistent_keys.os.replace

    def held(*args: Any) -> None:
        entered.set()
        assert release.wait(10.0)
        replace(*args)

    monkeypatch.setattr(persistent_keys.os, "replace", held)
    return entered, release


def _returns_at_once(call: Any) -> None:
    done = threading.Thread(target=call)
    done.start()
    done.join(5.0)
    assert not done.is_alive(), "the call waited for the blocked file write"


def test_an_operator_set_during_a_blocked_write_returns_and_is_read_from_memory(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    owner = _Owner(register)
    declarations = owner.store.program.blackboard_keys
    assert register.reconcile(
        CommittedActivation(
            record_digest="h.apply.1",
            project_path=_PROJECT,
            declarations=dict(declarations),
            apply_id=1,
        )
    ).outcome == "reconciled"
    entered, release = _blocked_replace(monkeypatch)
    owner.store.write("layer", 4, metadata={"node_instance_id": "set_layer", "node_attempt_id": 1})
    assert entered.wait(5.0)  # The writer is inside its file replace.

    # The operator's Set through the run's value store, as a live-run edit.
    _returns_at_once(
        lambda: owner.store.write(
            "counted",
            9,
            metadata={"request_key": "0" * 31 + "1", "value_source": "runtime_control"},
        )
    )
    # Memory is the authority: a node and the next run read the new value.
    assert owner.store.read("counted") == 9
    persistent = {
        key: row for key, row in declarations.items() if row.persistence == "persistent"
    }
    assert register.restore(_PROJECT, persistent) == {"layer": 4, "counted": 9}
    assert not register.path.exists()

    release.set()
    assert register.wait_written(5.0)
    assert _on_disk(register, _PROJECT) == {"layer": 4, "counted": 9}


def test_a_failed_background_write_is_counted_and_the_next_change_writes_again(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    replace = persistent_keys.os.replace

    def refuse(*_args: Any) -> None:
        raise OSError("read-only file system")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    with caplog.at_level(logging.ERROR, logger=persistent_keys.__name__):
        register.commit(_SCENARIO, _declaration("index"), 1, edit=_node(1))
        assert register.wait_written(5.0)
    assert register.write_failures == 1
    assert "could not be written" in caplog.text
    assert not register.path.exists()
    assert register.values(_SCENARIO)["index"]["value"] == 1

    monkeypatch.setattr(persistent_keys.os, "replace", replace)
    register.commit(_SCENARIO, _declaration("index"), 2, edit=_node(2))
    assert register.wait_written(5.0)
    assert _on_disk(register) == {"index": 2}
    assert register.write_failures == 1


def test_a_store_node_write_does_not_fail_when_the_disk_does(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    store = _Owner(register).store

    def refuse(*_args: Any) -> None:
        raise OSError("read-only file system")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    store.write("layer", 4, metadata={"node_instance_id": "set_layer", "node_attempt_id": 1})
    assert store.read("layer") == 4
    assert register.wait_written(5.0)
    assert register.write_failures == 1


def test_the_tick_never_waits_for_a_blocked_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Reads and writes on the tick, the grasp-lost copy on its stream thread
    and an operator Set all hold the run's value-store lock only for memory."""

    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    program = Program.from_dict(
        _program(_key("layer", initial=0), _key("lost", "bool")),
        build_node_environment().catalog,
    )
    store = ProgramValueStore.from_program(
        program, project_path=_PROJECT, register=register, program_run_id=1
    )
    entered, release = _blocked_replace(monkeypatch)
    store.write("layer", 1, metadata={"node_instance_id": "set_layer", "node_attempt_id": 1})
    assert entered.wait(5.0)  # The writer is inside its file replace.

    # The grasp-lost copy, on the interrupt-state-mirror stream thread.
    _returns_at_once(
        lambda: store.write(
            "lost", True, metadata={"writer": "grasp_lost", "loss_event_id": "loss.1"}
        )
    )
    # A live-run operator Set and Reset.
    _returns_at_once(
        lambda: store.write(
            "layer",
            7,
            metadata={"request_key": "0" * 31 + "2", "value_source": "runtime_control"},
        )
    )
    # The tick: a node's read and write.
    _returns_at_once(lambda: store.read("layer"))
    _returns_at_once(
        lambda: store.write(
            "layer", 8, metadata={"node_instance_id": "set_layer", "node_attempt_id": 2}
        )
    )
    _returns_at_once(lambda: store.reset_to_initial("lost"))
    _returns_at_once(lambda: store.write(
        "lost", True, metadata={"writer": "grasp_lost", "loss_event_id": "loss.2"}
    ))
    assert (store.read("layer"), store.read("lost")) == (8, True)
    assert not register.path.exists()

    release.set()
    assert register.wait_written(5.0)
    # The file holds the latest state of every change made meanwhile.
    assert _on_disk(register, _PROJECT) == {"layer": 8, "lost": True}
    document = json.loads(register.path.read_text())
    assert document["sequence"] == register.sequence == 6
    assert len(document["edits"]) == 6
    assert register.write_failures == 0


def _fail_directory_sync(monkeypatch: pytest.MonkeyPatch) -> list[bool]:
    """Fail every fsync of a directory while ``failing[0]``; files sync normally."""

    failing = [True]
    fsync = persistent_keys.os.fsync

    def sync(descriptor: int) -> None:
        if failing[0] and stat.S_ISDIR(os.fstat(descriptor).st_mode):
            raise OSError("I/O error")
        fsync(descriptor)

    monkeypatch.setattr(persistent_keys.os, "fsync", sync)
    return failing


def test_a_failed_directory_sync_is_counted_and_the_change_stands(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, caplog: pytest.LogCaptureFixture
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    index = _declaration("index")
    assert register.reconcile(_activation(index)).outcome == "reconciled"
    assert register.wait_written(5.0)
    failing = _fail_directory_sync(monkeypatch)

    with caplog.at_level(logging.ERROR, logger=persistent_keys.__name__):
        register.commit(_SCENARIO, index, 2, edit=_OPERATOR)
        assert register.wait_written(5.0)
    assert register.write_failures == 1
    assert "could not be written" in caplog.text
    # The change stands in memory, and the replaced file holds it.
    assert register.values(_SCENARIO)["index"]["value"] == 2
    assert _on_disk(register) == {"index": 2}
    assert register.restore(_SCENARIO, {"index": index}) == {"index": 2}

    # The next change is accepted while the sync still fails, and once it works.
    assert register.remove(_SCENARIO, "index")
    assert register.wait_written(5.0)
    assert register.write_failures == 2
    failing[0] = False
    register.commit(_SCENARIO, index, 3)
    assert register.wait_written(5.0)
    assert register.write_failures == 2
    assert _on_disk(register) == {"index": 3}
    assert register.restore(_SCENARIO, {"index": index}) == {"index": 3}


def test_the_file_is_read_only_at_construction(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    first = PersistentKeyRegister(tmp_path, application_start_id=_A)
    first.commit(_SCENARIO, _declaration("index"), 1)
    first.close()
    loads: list[int] = []
    load = PersistentKeyRegister._load  # noqa: SLF001

    def counted(self: PersistentKeyRegister) -> None:
        loads.append(1)
        load(self)

    monkeypatch.setattr(PersistentKeyRegister, "_load", counted)
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    index = _declaration("index")
    failing = _fail_directory_sync(monkeypatch)
    assert register.reconcile(_activation(index)).outcome == "reconciled"
    register.commit(_SCENARIO, index, 2)
    assert register.wait_written(5.0)
    failing[0] = False
    register.commit(_SCENARIO, index, 3, edit=_node())
    assert register.wait_written(5.0)
    register.remove(_SCENARIO, "index")
    register.clear(_SCENARIO, {"index": index})
    register.restore(_SCENARIO, {"index": index})
    register.values(_SCENARIO)
    register.close()
    assert loads == [1]

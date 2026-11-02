from __future__ import annotations

import json
import os
import stat
from pathlib import Path
from types import MappingProxyType

import pytest

import manyforge_program_runner.persistent_keys as persistent_keys
from manyforge_behavior.program import (
    BlackboardFieldDeclaration,
    BlackboardKeyDeclaration,
)
from manyforge_program_runner.persistent_keys import (
    CommittedActivation,
    PersistentKeyEdit,
    PersistentKeyNotice,
    PersistentKeyRegister,
    PersistentKeyRegisterError,
    PersistentKeyRegisterVersionError,
    PersistentKeyStorageUncertainError,
)


def _declaration(
    key: str,
    value_type: str = "int64",
    *,
    persistence: str = "persistent",
    access: str = "read_write",
    fields: dict[str, str] | None = None,
) -> BlackboardKeyDeclaration:
    return BlackboardKeyDeclaration(
        key=key,
        type=value_type,
        access=access,
        persistence=persistence,
        fields=MappingProxyType(
            {
                name: BlackboardFieldDeclaration(name, field_type)
                for name, field_type in (fields or {}).items()
            }
        ),
    )


class _Manager:
    """Committed Scenario Applies of one Manager publication history."""

    def __init__(self, history: str = "h", retain: int | None = None) -> None:
        self.history = history
        self.applies: dict[str, str] = {}
        # Manager's journal keeps the newest `retain` committed Applies.
        self.retain = retain

    def apply(
        self, project_path: str, *declarations: BlackboardKeyDeclaration
    ) -> CommittedActivation:
        digest = f"{self.history}.apply.{len(self.applies) + 1}"
        self.applies[digest] = project_path
        return self.current(digest, declarations)

    def current(
        self, digest: str, declarations: tuple[BlackboardKeyDeclaration, ...]
    ) -> CommittedActivation:
        return CommittedActivation(
            record_digest=digest,
            project_path=self.applies[digest],
            declarations={row.key: row for row in declarations},
            committed_applies=dict(
                list(self.applies.items())[-(self.retain or len(self.applies)):]
            ),
        )


def _reconciled(
    register: PersistentKeyRegister,
    manager: _Manager,
    project_path: str,
    *declarations: BlackboardKeyDeclaration,
) -> CommittedActivation:
    activation = manager.apply(project_path, *declarations)
    assert register.reconcile(activation).outcome == "reconciled"
    return activation


def test_values_survive_a_new_register_and_stay_scoped_to_their_scenario(
    tmp_path: Path,
) -> None:
    path = tmp_path / "state" / "persistent-keys.json"
    register = PersistentKeyRegister(path)
    manager = _Manager()
    _reconciled(register, manager, "/cells/b/cell.scenario", _declaration("index"))
    register.commit("/cells/b/cell.scenario", _declaration("index"), 9)
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 3)

    reopened = PersistentKeyRegister(path)
    assert reopened.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {
        "index": 3
    }
    # Only the Scenario of the reconciled activation restores anything.
    assert reopened.restore("/cells/b/cell.scenario", {"index": _declaration("index")}) == {}
    assert reopened.values("/cells/b/cell.scenario")["index"]["value"] == 9
    document = json.loads(path.read_text())
    assert document["schema_version"] == "manyforge.persistent_key_register.v3"
    assert document["activation"] == {
        "record_digest": "h.apply.2",
        "project_path": "/cells/a/cell.scenario",
    }
    assert document["reconciled_applies"] == ["h.apply.1", "h.apply.2"]


def test_a_committed_apply_drops_removed_and_restructured_keys_with_notices(
    tmp_path: Path,
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    _reconciled(register, manager, "/cells/other/cell.scenario", _declaration("gone"))
    register.commit("/cells/other/cell.scenario", _declaration("gone"), 5)
    _reconciled(
        register,
        manager,
        "/cells/a/cell.scenario",
        *(_declaration(key) for key in ("gone", "retyped", "now_run", "kept")),
    )
    for value, key in enumerate(("gone", "retyped", "now_run", "kept"), start=1):
        register.commit("/cells/a/cell.scenario", _declaration(key), value)

    activation = manager.apply(
        "/cells/a/cell.scenario",
        _declaration("retyped", "float64"),
        _declaration("now_run", persistence="run"),
        # `initial:` and description are not part of the stored structure:
        # the value is kept.
        BlackboardKeyDeclaration(
            key="kept",
            type="int64",
            access="read_write",
            persistence="persistent",
            fields=MappingProxyType({}),
            description="now described",
            initial=0,
            has_initial=True,
        ),
    )
    result = register.reconcile(activation)

    assert result.outcome == "reconciled"
    assert set(result.dropped) == {
        PersistentKeyNotice("/cells/a/cell.scenario", "gone", "key_not_declared_persistent"),
        PersistentKeyNotice("/cells/a/cell.scenario", "now_run", "key_not_declared_persistent"),
        PersistentKeyNotice("/cells/a/cell.scenario", "retyped", "key_declaration_changed"),
    }
    assert register.restore("/cells/a/cell.scenario", activation.declarations) == {"kept": 4}
    # Another Scenario's rows are untouched while all its Applies were seen.
    assert set(register.values("/cells/other/cell.scenario")) == {"gone"}
    reopened = PersistentKeyRegister(register.path)
    assert set(reopened.values("/cells/a/cell.scenario")) == {"kept"}
    assert [
        (edit["action"], edit["key"], edit["reason"], edit["source_label"])
        for edit in reopened.edits("/cells/a/cell.scenario")
        if edit["action"] == "dropped"
    ] == [
        ("dropped", "gone", "key_not_declared_persistent", "committed_apply"),
        ("dropped", "now_run", "key_not_declared_persistent", "committed_apply"),
        ("dropped", "retyped", "key_declaration_changed", "committed_apply"),
    ]


def test_structure_compatibility_includes_fields(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    before = _declaration("bundle", "model_response", fields={"reason": "string"})
    after = _declaration("bundle", "model_response", fields={"reason": "bool"})
    _reconciled(register, manager, "/cells/a/cell.scenario", before)
    register.commit("/cells/a/cell.scenario", before, {"reason": "ok"})

    assert register.restore("/cells/a/cell.scenario", {"bundle": after}) == {}
    _reconciled(register, manager, "/cells/a/cell.scenario", after)
    assert register.values("/cells/a/cell.scenario") == {}


def test_an_observed_delete_apply_drops_the_value_for_good(tmp_path: Path) -> None:
    """Delete pallet_index (stored 9), Apply, re-add it with initial 0, Apply:
    with both Applies observed the re-added key starts empty (from 0)."""

    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    pallet = _declaration("pallet_index")
    _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))
    register.commit("/cells/a/cell.scenario", pallet, 9)
    register.commit("/cells/a/cell.scenario", _declaration("layer"), 2)

    deleted = _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("layer"))
    readded = _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))

    assert register.restore("/cells/a/cell.scenario", deleted.declarations) == {"layer": 2}
    assert register.restore("/cells/a/cell.scenario", readded.declarations) == {"layer": 2}
    assert register.unobserved_applies("/cells/a/cell.scenario") == ()


def test_a_missed_apply_keeps_values_and_reports_the_fault(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    pallet = _declaration("pallet_index")
    _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))
    register.commit("/cells/a/cell.scenario", pallet, 9)
    register.commit("/cells/a/cell.scenario", _declaration("layer"), 2)

    missed = manager.apply("/cells/a/cell.scenario", _declaration("layer"))  # never observed
    readded = manager.apply("/cells/a/cell.scenario", pallet, _declaration("layer"))
    result = register.reconcile(readded)  # e.g. at Run start

    # Nothing is dropped for what cannot be proven; the missed Apply is a
    # named, persistent fault instead.
    assert result.dropped == ()
    assert result.unobserved == ((missed.record_digest, "/cells/a/cell.scenario"),)
    assert register.restore("/cells/a/cell.scenario", readded.declarations) == {
        "pallet_index": 9,
        "layer": 2,
    }
    reopened = PersistentKeyRegister(register.path)
    assert reopened.unobserved_applies("/cells/a/cell.scenario") == (missed.record_digest,)
    (fault,) = [e for e in reopened.edits("/cells/a/cell.scenario") if e["action"] == "fault"]
    assert fault["reason"] == "applied_program_not_observed"
    assert missed.record_digest in fault["detail"]
    # The current declarations still decide: a key it does not declare goes.
    narrowed = manager.apply("/cells/a/cell.scenario", _declaration("layer"))
    assert register.reconcile(narrowed).dropped == (
        PersistentKeyNotice("/cells/a/cell.scenario", "pallet_index", "key_not_declared_persistent"),
    )


def test_duplicate_and_stale_activations_change_nothing(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    narrow = _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("x"))
    assert register.reconcile(narrow).outcome == "duplicate"
    wide = _reconciled(
        register, manager, "/cells/a/cell.scenario", _declaration("x"), _declaration("y")
    )
    register.commit("/cells/a/cell.scenario", _declaration("y"), 7)
    sequence = register.sequence

    # A repeated notification is a no-op ...
    assert register.reconcile(wide).outcome == "duplicate"
    # ... and a late one never cleans up against its outdated declarations.
    late = manager.current(narrow.record_digest, (_declaration("x"),))
    assert register.reconcile(late).outcome == "stale"
    assert register.sequence == sequence
    assert register.restore("/cells/a/cell.scenario", wide.declarations) == {"y": 7}


def test_a_new_publication_history_keeps_rows_that_still_fit(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    first = _Manager("first")
    _reconciled(register, first, "/cells/b/cell.scenario", _declaration("count"))
    register.commit("/cells/b/cell.scenario", _declaration("count"), 1)
    _reconciled(register, first, "/cells/a/cell.scenario", _declaration("index"), _declaration("gone"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 2)
    register.commit("/cells/a/cell.scenario", _declaration("gone"), 3)
    first.apply("/cells/a/cell.scenario", _declaration("index"))  # missed before the restart
    register.reconcile(first.apply("/cells/a/cell.scenario", _declaration("index"), _declaration("gone")))
    assert register.unobserved_applies("/cells/a/cell.scenario") == ("first.apply.3",)

    # Manager restarted with new state (a workbench stop/start).
    again = _Manager("second").apply("/cells/a/cell.scenario", _declaration("index"))
    result = register.reconcile(again)

    assert result.history_changed is True
    assert result.dropped == (
        PersistentKeyNotice("/cells/a/cell.scenario", "gone", "key_not_declared_persistent"),
    )
    assert register.restore("/cells/a/cell.scenario", again.declarations) == {"index": 2}
    assert register.values("/cells/b/cell.scenario")["count"]["value"] == 1
    # Faults named Applies of the previous history; its audit stays.
    assert register.unobserved_applies("/cells/a/cell.scenario") == ()
    assert [e["action"] for e in register.edits("/cells/a/cell.scenario")].count("fault") == 1


def test_manager_eviction_continues_the_history(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager(retain=4)
    index = _declaration("index")
    _reconciled(register, manager, "/cells/a/cell.scenario", index)
    register.commit("/cells/a/cell.scenario", index, 7)
    # Far more Applies than Manager retains, each observed: one history.
    for _ in range(10):
        result = register.reconcile(manager.apply("/cells/a/cell.scenario", index))
        assert (result.outcome, result.history_changed, result.unobserved) == (
            "reconciled", False, ()
        )
    assert register.restore("/cells/a/cell.scenario", {"index": index}) == {"index": 7}

    # A retained Apply the register never observed is still the named fault ...
    missed = manager.apply("/cells/b/cell.scenario")
    result = register.reconcile(manager.apply("/cells/a/cell.scenario", index))
    assert result.history_changed is False
    assert result.unobserved == ((missed.record_digest, "/cells/b/cell.scenario"),)
    assert register.unobserved_applies("/cells/b/cell.scenario") == (missed.record_digest,)
    # ... until Manager evicts the Apply it names (its audit stays).
    for _ in range(4):
        register.reconcile(manager.apply("/cells/a/cell.scenario", index))
    assert register.unobserved_applies("/cells/b/cell.scenario") == ()
    assert [e["action"] for e in register.edits("/cells/b/cell.scenario")].count("fault") == 1
    reopened = PersistentKeyRegister(register.path)
    assert reopened.restore("/cells/a/cell.scenario", {"index": index}) == {"index": 7}

    # A register that lagged until its last activation was evicted cannot
    # follow the history: accepted as a new one, the retained Applies it
    # never saw are named, and rows that fit stay.
    for _ in range(5):
        manager.apply("/cells/b/cell.scenario")
    result = reopened.reconcile(manager.apply("/cells/a/cell.scenario", index))
    assert result.history_changed is True
    assert len(result.unobserved) == 3
    assert reopened.restore("/cells/a/cell.scenario", {"index": index}) == {"index": 7}


def test_nothing_is_restored_before_the_scenario_is_reconciled(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    assert register.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {}
    register.commit("/cells/a/cell.scenario", _declaration("index"), 3)
    assert register.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {}
    # The first reconciliation keeps what fits and reports nothing missed.
    manager = _Manager()
    manager.apply("/cells/a/cell.scenario", _declaration("index"))
    activation = manager.apply("/cells/a/cell.scenario", _declaration("index"))
    result = register.reconcile(activation)
    assert (result.dropped, result.unobserved) == ((), ())
    assert register.restore("/cells/a/cell.scenario", activation.declarations) == {"index": 3}


def test_a_failed_cleanup_keeps_the_register_and_retries(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 5)
    removed = manager.apply("/cells/a/cell.scenario")

    def refuse(*_args, **_kwargs):
        raise OSError("read-only file system")

    replace = persistent_keys.os.replace
    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    with pytest.raises(PersistentKeyRegisterError, match="could not be written"):
        register.reconcile(removed)
    # Unchanged, and the stale row is still not restorable for the new Apply.
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 5
    assert register.activation["record_digest"] == "h.apply.1"
    monkeypatch.setattr(persistent_keys.os, "replace", replace)
    assert register.reconcile(removed).outcome == "reconciled"
    assert register.values("/cells/a/cell.scenario") == {}


def test_drop_forgets_a_refused_restored_value_with_a_notice(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    _reconciled(register, _Manager(), "/cells/a/cell.scenario", _declaration("pose"))
    register.commit("/cells/a/cell.scenario", _declaration("pose"), 1)

    register.drop(
        "/cells/a/cell.scenario",
        "pose",
        reason="value_invalid",
        detail="frame not allowed",
        edit={"program_run_id": "run.1"},
    )

    assert register.values("/cells/a/cell.scenario") == {}
    assert register.take_notices() == (
        PersistentKeyNotice("/cells/a/cell.scenario", "pose", "value_invalid", "frame not allowed"),
    )
    (dropped,) = [e for e in register.edits("/cells/a/cell.scenario") if e["action"] == "dropped"]
    assert (dropped["reason"], dropped["detail"], dropped["program_run_id"]) == (
        "value_invalid",
        "frame not allowed",
        "run.1",
    )
    assert dropped["source_label"] == "run_start"

    # A removal that fails still leaves the notice for the run's audit.
    register.commit("/cells/a/cell.scenario", _declaration("pose"), 2)
    monkeypatch.setattr(
        persistent_keys.os,
        "replace",
        lambda *_args: (_ for _ in ()).throw(OSError("read-only")),
    )
    with pytest.raises(PersistentKeyRegisterError):
        register.drop("/cells/a/cell.scenario", "pose", reason="value_invalid")
    assert [notice.key for notice in register.take_notices()] == ["pose"]


def test_failed_write_keeps_the_previous_visible_value(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    register.commit("/cells/a/cell.scenario", _declaration("index"), 1)

    def refuse(*_args, **_kwargs):
        raise OSError("read-only file system")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    with pytest.raises(PersistentKeyRegisterError, match="could not be written"):
        register.commit("/cells/a/cell.scenario", _declaration("index"), 2)
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 1


def test_an_unchanged_value_is_not_written_again(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    before = _declaration("bundle", "model_response", fields={"reason": "string"})
    after = _declaration("bundle", "model_response", fields={"reason": "bool"})
    value = {"reason": "ok", "score": 1}
    register.commit(
        "/cells/a/cell.scenario", before, value, edit=PersistentKeyEdit("request.1", "ui")
    )

    def refuse(*_args, **_kwargs):
        raise AssertionError("an unchanged value rewrote the register")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    # The same canonical value under the same structure changes nothing.
    register.commit(
        "/cells/a/cell.scenario",
        before,
        {"score": 1, "reason": "ok"},
        edit=PersistentKeyEdit("request.2", "ui"),
    )
    assert register.sequence == 1
    assert [row["request_id"] for row in register.edits("/cells/a/cell.scenario")] == [
        "request.1"
    ]
    monkeypatch.undo()
    # A new structure is a change even when the value is the same.
    register.commit("/cells/a/cell.scenario", after, value)
    assert register.sequence == 2


def test_bounds_and_non_persistent_writes_are_refused(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    with pytest.raises(PersistentKeyRegisterError, match="not persistent"):
        register.commit("/cells/a/cell.scenario", _declaration("index", persistence="run"), 1)
    with pytest.raises(PersistentKeyRegisterError, match="exceeds"):
        register.commit(
            "/cells/a/cell.scenario",
            _declaration("text", "string"),
            "x" * persistent_keys.MAX_VALUE_BYTES,
        )
    monkeypatch.setattr(persistent_keys, "MAX_ROWS", 1)
    register.commit("/cells/a/cell.scenario", _declaration("index"), 1)
    with pytest.raises(PersistentKeyRegisterError, match="full"):
        register.commit("/cells/a/cell.scenario", _declaration("other"), 1)


def test_remove_and_clear_forget_only_the_named_scenario(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    declarations = {
        "index": _declaration("index"),
        "layer": _declaration("layer", access="write"),
        "limit": _declaration("limit", access="read"),
    }
    register.commit("/cells/a/cell.scenario", declarations["index"], 1)
    register.commit("/cells/a/cell.scenario", declarations["layer"], 2)
    register.commit("/cells/a/cell.scenario", declarations["limit"], 4)
    register.commit("/cells/b/cell.scenario", declarations["index"], 3)

    assert register.remove("/cells/a/cell.scenario", "index") is True
    assert register.remove("/cells/a/cell.scenario", "index") is False
    # Clear is one Reset per writable key: a read key keeps its stored row.
    assert register.clear("/cells/a/cell.scenario", declarations) == 1
    assert set(register.values("/cells/a/cell.scenario")) == {"limit"}
    assert register.clear("/cells/a/cell.scenario", declarations) == 0
    assert set(PersistentKeyRegister(register.path).values("/cells/b/cell.scenario")) == {"index"}


def test_malformed_register_is_refused_not_replaced(tmp_path: Path) -> None:
    path = tmp_path / "keys.json"
    path.write_text('{"schema_version": "manyforge.persistent_key_register.v3"}')
    with pytest.raises(PersistentKeyRegisterError, match="unsupported layout"):
        PersistentKeyRegister(path)
    assert path.read_text() == '{"schema_version": "manyforge.persistent_key_register.v3"}'


def test_another_register_version_is_refused_by_name_and_left_untouched(
    tmp_path: Path,
) -> None:
    path = tmp_path / "keys.json"
    earlier = json.dumps(
        {
            "schema_version": "manyforge.persistent_key_register.v1",
            "sequence": 1,
            "rows": [],
            "edits": [],
        }
    )
    path.write_text(earlier)
    with pytest.raises(PersistentKeyRegisterVersionError) as refused:
        PersistentKeyRegister(path)
    assert refused.value.code == "persistent_key_register_version_unsupported"
    assert "'manyforge.persistent_key_register.v1'" in str(refused.value)
    assert path.read_text() == earlier


def test_the_superseded_scenario_keyed_register_is_moved_aside_not_migrated(
    tmp_path: Path,
) -> None:
    path = tmp_path / "keys.json"
    earlier = json.dumps(
        {
            "schema_version": "manyforge.persistent_key_register.v2",
            "sequence": 1,
            "rows": [
                {"scenario_id": "cell", "key": "index", "fingerprint": "x",
                 "value": 3, "sequence": 1}
            ],
        }
    )
    path.write_text(earlier)
    register = PersistentKeyRegister(path)
    assert not path.exists()
    assert (tmp_path / "keys.json.v2.superseded").read_text() == earlier
    assert register.sequence == 0
    assert register.activation is None
    assert register.values("/cells/a/cell.scenario") == {}


def test_same_named_projects_in_different_folders_keep_separate_values(
    tmp_path: Path,
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    manager = _Manager()
    first, second = "/site/line1/pick.scenario", "/site/line2/pick.scenario"
    _reconciled(register, manager, first, _declaration("index"))
    register.commit(first, _declaration("index"), 4)
    activation = _reconciled(register, manager, second, _declaration("index"))
    assert register.restore(second, activation.declarations) == {}
    register.commit(second, _declaration("index"), 7)
    # The renamed or moved folder is another project: it starts empty.
    moved = _reconciled(
        register, manager, "/site/line1/renamed.scenario", _declaration("index")
    )
    assert register.restore("/site/line1/renamed.scenario", moved.declarations) == {}
    reopened = PersistentKeyRegister(register.path)
    assert reopened.values(first)["index"]["value"] == 4
    assert reopened.values(second)["index"]["value"] == 7


def test_a_project_path_is_bounded_to_4096_bytes(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    longest = "/" + "p" * 4095
    _reconciled(register, _Manager(), longest, _declaration("index"))
    register.commit(longest, _declaration("index"), 1)
    with pytest.raises(PersistentKeyRegisterError, match="project_path"):
        register.commit(longest + "p", _declaration("index"), 1)


def test_operator_edits_are_audited_with_the_change_and_bounded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    edit = PersistentKeyEdit("request.1", "operator_ui")
    register.commit("/cells/a/cell.scenario", _declaration("index"), 1, edit=edit)
    # Unattributed writes are ordered by row sequence only.
    register.commit("/cells/a/cell.scenario", _declaration("index"), 2)
    register.remove("/cells/a/cell.scenario", "index", edit=PersistentKeyEdit("request.2", "ui"))
    register.commit("/cells/b/cell.scenario", _declaration("index"), 3, edit=edit)
    register.clear(
        "/cells/b/cell.scenario",
        {"index": _declaration("index")},
        edit=PersistentKeyEdit("request.3", "ui"),
    )

    assert PersistentKeyRegister(register.path).edits("/cells/a/cell.scenario") == [
        {
            "sequence": 1,
            "project_path": "/cells/a/cell.scenario",
            "key": "index",
            "action": "set",
            "request_id": "request.1",
            "source_label": "operator_ui",
            "program_run_id": None,
            "node_instance_id": None,
            "node_attempt_id": None,
            "reason": None,
            "detail": None,
        },
        {
            "sequence": 3,
            "project_path": "/cells/a/cell.scenario",
            "key": "index",
            "action": "reset",
            "request_id": "request.2",
            "source_label": "ui",
            "program_run_id": None,
            "node_instance_id": None,
            "node_attempt_id": None,
            "reason": None,
            "detail": None,
        },
    ]
    assert [row["action"] for row in register.edits("/cells/b/cell.scenario")] == [
        "set",
        "clear",
    ]

    monkeypatch.setattr(persistent_keys, "MAX_EDITS", 2)
    register.commit("/cells/a/cell.scenario", _declaration("index"), 5, edit=edit)
    assert [row["sequence"] for row in register.edits("/cells/a/cell.scenario")] == [6]
    assert len(json.loads(register.path.read_text())["edits"]) == 2


def test_a_run_write_is_audited_with_its_run_node_and_attempt(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    register.commit(
        "/cells/a/cell.scenario",
        _declaration("index"),
        1,
        edit={
            "program_run_id": "run.1",
            "node_id": "logic.set_value",
            "node_instance_id": "set_index",
            "node_attempt_id": "attempt.1",
        },
    )
    assert PersistentKeyRegister(register.path).edits("/cells/a/cell.scenario") == [
        {
            "sequence": 1,
            "project_path": "/cells/a/cell.scenario",
            "key": "index",
            "action": "set",
            "request_id": None,
            "source_label": "program_node",
            "program_run_id": "run.1",
            "node_instance_id": "set_index",
            "node_attempt_id": "attempt.1",
            "reason": None,
            "detail": None,
        }
    ]
    with pytest.raises(PersistentKeyRegisterError, match="node_attempt_id"):
        register.commit(
            "/cells/a/cell.scenario",
            _declaration("index"),
            2,
            edit={"program_run_id": "run.1", "node_attempt_id": "x" * 300},
        )
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 1


def test_a_write_that_may_not_be_durable_is_distinct_and_reconciled(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path / "keys.json")
    register.commit("/cells/a/cell.scenario", _declaration("index"), 1)
    failing = [True]
    sync = persistent_keys.os.fsync

    def fsync(descriptor: int) -> None:
        if failing[0] and stat.S_ISDIR(os.fstat(descriptor).st_mode):
            raise OSError("I/O error")
        sync(descriptor)

    monkeypatch.setattr(persistent_keys.os, "fsync", fsync)
    with pytest.raises(PersistentKeyStorageUncertainError, match="may not be durable"):
        register.commit("/cells/a/cell.scenario", _declaration("index"), 2)
    # The file was replaced: memory follows it, it does not claim "unchanged".
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 2
    assert PersistentKeyRegister(register.path).values("/cells/a/cell.scenario")["index"]["value"] == 2

    # Every change is refused, as an ordinary refusal, until reconciled.
    with pytest.raises(PersistentKeyRegisterError, match="not reconciled") as refused:
        register.remove("/cells/a/cell.scenario", "index")
    assert not isinstance(refused.value, PersistentKeyStorageUncertainError)
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 2

    failing[0] = False
    register.commit("/cells/a/cell.scenario", _declaration("index"), 3)
    assert PersistentKeyRegister(register.path).values("/cells/a/cell.scenario")["index"]["value"] == 3

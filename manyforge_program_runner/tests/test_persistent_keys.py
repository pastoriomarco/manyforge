from __future__ import annotations

import json
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
    REGISTER_FILE_NAME,
)

# Two application starts: run and attempt numbers restart with each.
_A = "3f9a00000000002a"
_B = "7c1e000000000003"


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

    def __init__(self, history: str = "h") -> None:
        self.history = history
        self.applies: dict[str, str] = {}

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
            apply_id=list(self.applies).index(digest) + 1,
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


def _reopened(register: PersistentKeyRegister, start: str = _A) -> PersistentKeyRegister:
    """What a next start reads: the file, once the writer has written it."""

    assert register.wait_written(5.0)
    return PersistentKeyRegister(register.path.parent, application_start_id=start)


def test_values_survive_a_new_register_and_stay_scoped_to_their_scenario(
    tmp_path: Path,
) -> None:
    path = tmp_path / "state" / "persistent-keys.v5.json"
    register = PersistentKeyRegister(path.parent, application_start_id=_A)
    manager = _Manager()
    _reconciled(register, manager, "/cells/b/cell.scenario", _declaration("index"))
    register.commit("/cells/b/cell.scenario", _declaration("index"), 9)
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 3)

    reopened = _reopened(register)
    assert reopened.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {
        "index": 3
    }
    # Only the Scenario of the reconciled activation restores anything.
    assert reopened.restore("/cells/b/cell.scenario", {"index": _declaration("index")}) == {}
    assert reopened.values("/cells/b/cell.scenario")["index"]["value"] == 9
    document = json.loads(path.read_text())
    assert document["schema_version"] == "manyforge.persistent_key_register.v5"
    assert document["activation"] == {
        "record_digest": "h.apply.2",
        "project_path": "/cells/a/cell.scenario",
        "application_start_id": _A,
        "apply_id": 2,
    }
    assert set(document) == {"schema_version", "sequence", "rows", "edits", "activation"}


def test_a_committed_apply_drops_removed_and_restructured_keys_with_notices(
    tmp_path: Path,
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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
    reopened = _reopened(register)
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
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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

    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    pallet = _declaration("pallet_index")
    _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))
    register.commit("/cells/a/cell.scenario", pallet, 9)
    register.commit("/cells/a/cell.scenario", _declaration("layer"), 2)

    deleted = _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("layer"))
    readded = _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))

    assert register.restore("/cells/a/cell.scenario", deleted.declarations) == {"layer": 2}
    assert register.restore("/cells/a/cell.scenario", readded.declarations) == {"layer": 2}


def test_an_apply_the_runner_never_activated_has_no_effect(tmp_path: Path) -> None:
    """Values follow the Program the runner actually activates: an Apply a
    later one superseded before the runner woke changes nothing."""

    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    pallet = _declaration("pallet_index")
    _reconciled(register, manager, "/cells/a/cell.scenario", pallet, _declaration("layer"))
    register.commit("/cells/a/cell.scenario", pallet, 9)
    register.commit("/cells/a/cell.scenario", _declaration("layer"), 2)

    manager.apply("/cells/a/cell.scenario", _declaration("layer"))  # never activated
    readded = manager.apply("/cells/a/cell.scenario", pallet, _declaration("layer"))
    result = register.reconcile(readded)  # e.g. at Run start

    assert (result.outcome, result.dropped) == ("reconciled", ())
    assert register.restore("/cells/a/cell.scenario", readded.declarations) == {
        "pallet_index": 9,
        "layer": 2,
    }
    assert register.edits("/cells/a/cell.scenario") == []  # nothing recorded
    # The current declarations still decide: a key it does not declare goes.
    narrowed = manager.apply("/cells/a/cell.scenario", _declaration("layer"))
    assert register.reconcile(narrowed).dropped == (
        PersistentKeyNotice("/cells/a/cell.scenario", "pallet_index", "key_not_declared_persistent"),
    )


def test_the_current_activation_again_changes_nothing(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    narrow = _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("x"))
    assert register.reconcile(narrow).outcome == "duplicate"
    wide = _reconciled(
        register, manager, "/cells/a/cell.scenario", _declaration("x"), _declaration("y")
    )
    register.commit("/cells/a/cell.scenario", _declaration("y"), 7)
    sequence = register.sequence

    # A repeated wake is a no-op ...
    assert register.reconcile(wide).outcome == "duplicate"
    # ... and a late read of an older activation of this start never cleans
    # up against its outdated declarations.
    late = manager.current(narrow.record_digest, (_declaration("x"),))
    assert register.reconcile(late).outcome == "stale"
    assert register.sequence == sequence
    assert register.restore("/cells/a/cell.scenario", wide.declarations) == {"y": 7}


def test_a_new_start_keeps_rows_that_still_fit(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    first = _Manager("first")
    _reconciled(register, first, "/cells/b/cell.scenario", _declaration("count"))
    register.commit("/cells/b/cell.scenario", _declaration("count"), 1)
    _reconciled(register, first, "/cells/a/cell.scenario", _declaration("index"), _declaration("gone"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 2)
    register.commit("/cells/a/cell.scenario", _declaration("gone"), 3)

    # A new application start: the Manager mints a new start and counts its
    # Apply numbers from 1 again; the new start's activation is the later one.
    register = _reopened(register, _B)
    again = _Manager("second").apply("/cells/a/cell.scenario", _declaration("index"))
    result = register.reconcile(again)

    assert result.outcome == "reconciled"

    assert result.dropped == (
        PersistentKeyNotice("/cells/a/cell.scenario", "gone", "key_not_declared_persistent"),
    )
    assert register.restore("/cells/a/cell.scenario", again.declarations) == {"index": 2}
    assert register.values("/cells/b/cell.scenario")["count"]["value"] == 1


def test_many_applies_in_one_start_keep_the_values(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    index = _declaration("index")
    _reconciled(register, manager, "/cells/a/cell.scenario", index)
    register.commit("/cells/a/cell.scenario", index, 7)
    for _ in range(300):
        _reconciled(register, manager, "/cells/a/cell.scenario", index)
    assert register.restore("/cells/a/cell.scenario", {"index": index}) == {"index": 7}
    reopened = _reopened(register)
    assert reopened.restore("/cells/a/cell.scenario", {"index": index}) == {"index": 7}


def test_nothing_is_restored_before_the_scenario_is_reconciled(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert register.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {}
    register.commit("/cells/a/cell.scenario", _declaration("index"), 3)
    assert register.restore("/cells/a/cell.scenario", {"index": _declaration("index")}) == {}
    # The first reconciliation keeps what fits and reports nothing missed.
    manager = _Manager()
    manager.apply("/cells/a/cell.scenario", _declaration("index"))
    activation = manager.apply("/cells/a/cell.scenario", _declaration("index"))
    result = register.reconcile(activation)
    assert result.dropped == ()
    assert register.restore("/cells/a/cell.scenario", activation.declarations) == {"index": 3}


def test_a_cleanup_whose_file_write_fails_is_made_in_memory_and_written_later(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 5)
    assert register.wait_written(5.0)
    removed = manager.apply("/cells/a/cell.scenario")

    def refuse(*_args, **_kwargs):
        raise OSError("read-only file system")

    replace = persistent_keys.os.replace
    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    # Memory is the authority: the cleanup is made, the disk does not refuse it.
    assert register.reconcile(removed).outcome == "reconciled"
    assert register.values("/cells/a/cell.scenario") == {}
    assert register.activation["record_digest"] == "h.apply.2"
    assert register.wait_written(5.0)
    assert register.write_failures == 1
    assert register.reconcile(removed).outcome == "duplicate"
    monkeypatch.setattr(persistent_keys.os, "replace", replace)
    # The close writes the latest state again.
    register.close()
    reopened = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert reopened.values("/cells/a/cell.scenario") == {}
    assert reopened.activation["record_digest"] == "h.apply.2"


def test_drop_forgets_a_refused_restored_value_with_a_notice(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    _reconciled(register, _Manager(), "/cells/a/cell.scenario", _declaration("pose"))
    register.commit("/cells/a/cell.scenario", _declaration("pose"), 1)

    register.drop(
        "/cells/a/cell.scenario",
        "pose",
        reason="value_invalid",
        detail="frame not allowed",
        # A run's edit names its run number, kept as the integer it is.
        edit={"program_run_id": 1},
    )

    assert register.values("/cells/a/cell.scenario") == {}
    assert register.take_notices() == (
        PersistentKeyNotice("/cells/a/cell.scenario", "pose", "value_invalid", "frame not allowed"),
    )
    (dropped,) = [e for e in register.edits("/cells/a/cell.scenario") if e["action"] == "dropped"]
    assert (dropped["reason"], dropped["detail"], dropped["program_run_id"]) == (
        "value_invalid",
        "frame not allowed",
        1,
    )
    assert dropped["source_label"] == "run_start"

    # A removal whose file write fails is made in memory with its notice.
    register.commit("/cells/a/cell.scenario", _declaration("pose"), 2)
    assert register.wait_written(5.0)
    monkeypatch.setattr(
        persistent_keys.os,
        "replace",
        lambda *_args: (_ for _ in ()).throw(OSError("read-only")),
    )
    register.drop("/cells/a/cell.scenario", "pose", reason="value_invalid")
    assert register.values("/cells/a/cell.scenario") == {}
    assert [notice.key for notice in register.take_notices()] == ["pose"]
    assert register.wait_written(5.0)
    assert register.write_failures == 1


def test_a_failed_file_write_fails_no_change_and_is_counted_once_per_write(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    declarations = {"index": _declaration("index")}
    _reconciled(register, _Manager(), "/cells/a/cell.scenario", declarations["index"])
    register.commit("/cells/a/cell.scenario", declarations["index"], 1)
    assert register.wait_written(5.0)
    written = register.path.read_bytes()

    def refuse(*_args, **_kwargs):
        raise OSError("read-only file system")

    monkeypatch.setattr(persistent_keys.os, "replace", refuse)
    edit = PersistentKeyEdit("request.1", "ui")
    # Every kind of change is made and answers; each write attempt that
    # fails is counted once.
    changes = [
        lambda: register.commit("/cells/a/cell.scenario", declarations["index"], 2, edit=edit),
        lambda: register.remove("/cells/a/cell.scenario", "index", edit=edit),
        lambda: register.commit("/cells/a/cell.scenario", declarations["index"], 3),
        lambda: register.clear("/cells/a/cell.scenario", declarations, edit=edit),
        lambda: register.commit("/cells/a/cell.scenario", declarations["index"], 4),
        lambda: register.reset_to_initial("/cells/a/cell.scenario", declarations, edit=edit),
    ]
    for count, change in enumerate(changes, start=1):
        change()
        assert register.wait_written(5.0)
        assert register.write_failures == count
    assert register.values("/cells/a/cell.scenario") == {}
    assert register.sequence == 8
    # The file still holds the last state written.
    assert register.path.read_bytes() == written


def test_an_unchanged_value_is_not_written_again(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    before = _declaration("bundle", "model_response", fields={"reason": "string"})
    after = _declaration("bundle", "model_response", fields={"reason": "bool"})
    value = {"reason": "ok", "score": 1}
    register.commit(
        "/cells/a/cell.scenario", before, value, edit=PersistentKeyEdit("request.1", "ui")
    )
    assert register.wait_written(5.0)

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
    assert [row["request_key"] for row in register.edits("/cells/a/cell.scenario")] == [
        "request.1"
    ]
    assert register.wait_written(5.0)
    assert register.write_failures == 0
    monkeypatch.undo()
    # A new structure is a change even when the value is the same.
    register.commit("/cells/a/cell.scenario", after, value)
    assert register.sequence == 2


def test_bounds_and_non_persistent_writes_are_refused(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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
    assert set(_reopened(register).values("/cells/b/cell.scenario")) == {"index"}


def test_malformed_register_is_refused_not_replaced(tmp_path: Path) -> None:
    path = tmp_path / REGISTER_FILE_NAME
    path.write_text('{"schema_version": "manyforge.persistent_key_register.v5"}')
    with pytest.raises(PersistentKeyRegisterError, match="unsupported layout"):
        PersistentKeyRegister(path.parent, application_start_id=_A)
    assert path.read_text() == '{"schema_version": "manyforge.persistent_key_register.v5"}'


def test_another_register_version_is_refused_by_name_and_left_untouched(
    tmp_path: Path,
) -> None:
    path = tmp_path / REGISTER_FILE_NAME
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
        PersistentKeyRegister(path.parent, application_start_id=_A)
    assert refused.value.code == "persistent_key_register_version_unsupported"
    assert "'manyforge.persistent_key_register.v1'" in str(refused.value)
    assert path.read_text() == earlier


def test_the_register_file_is_named_by_its_format_version() -> None:
    assert REGISTER_FILE_NAME == "persistent-keys.v5.json"
    assert persistent_keys.SCHEMA_VERSION == "manyforge.persistent_key_register.v5"


@pytest.mark.parametrize(
    ("older_name", "schema_version"),
    [
        ("persistent-keys.json", "manyforge.persistent_key_register.v4"),
        ("persistent-keys.json", "manyforge.persistent_key_register.v2"),
        ("persistent-keys.v3.json", "manyforge.persistent_key_register.v3"),
        ("persistent-keys.v4.json", "manyforge.persistent_key_register.v4"),
    ],
)
def test_an_older_register_file_is_left_in_place_and_named_once(
    tmp_path: Path,
    caplog: pytest.LogCaptureFixture,
    older_name: str,
    schema_version: str,
) -> None:
    older = tmp_path / older_name
    earlier = json.dumps(
        {
            "schema_version": schema_version,
            "sequence": 1,
            "rows": [
                {"project_path": "/cells/a/cell.scenario", "key": "index",
                 "fingerprint": "x", "value": 3, "sequence": 1}
            ],
        }
    ).encode()
    older.write_bytes(earlier)
    before = older.stat()
    with caplog.at_level("DEBUG", logger=persistent_keys.__name__):
        register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert register.sequence == 0
    assert register.activation is None
    assert register.values("/cells/a/cell.scenario") == {}
    assert [record.getMessage() for record in caplog.records] == [
        f"stored values start empty: {older} is in an older format and was "
        "left in place"
    ]
    assert older.read_bytes() == earlier
    assert older.stat().st_mtime_ns == before.st_mtime_ns
    assert older.stat().st_ino == before.st_ino
    assert sorted(path.name for path in tmp_path.iterdir()) == [older_name]

    # A first change creates only the versioned file; the older one stays.
    manager = _Manager()
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 5)
    assert register.wait_written(5.0)
    assert older.read_bytes() == earlier
    assert (tmp_path / REGISTER_FILE_NAME).is_file()


def test_no_older_register_file_logs_nothing(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    # A newer or unrelated name is not an older register file.
    (tmp_path / "persistent-keys.v6.json").write_text("{}")
    (tmp_path / "persistent-keys.json.bak").write_text("{}")
    with caplog.at_level("DEBUG", logger=persistent_keys.__name__):
        PersistentKeyRegister(tmp_path, application_start_id=_A)
        PersistentKeyRegister(tmp_path / "absent", application_start_id=_A)
    assert caplog.records == []


def test_an_older_file_beside_an_existing_register_logs_nothing(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    manager = _Manager()
    _reconciled(register, manager, "/cells/a/cell.scenario", _declaration("index"))
    register.commit("/cells/a/cell.scenario", _declaration("index"), 5)
    assert register.wait_written(5.0)
    (tmp_path / "persistent-keys.json").write_text("{}")
    with caplog.at_level("DEBUG", logger=persistent_keys.__name__):
        reopened = PersistentKeyRegister(tmp_path, application_start_id=_A)
    assert reopened.values("/cells/a/cell.scenario")["index"]["value"] == 5
    assert caplog.records == []


def test_same_named_projects_in_different_folders_keep_separate_values(
    tmp_path: Path,
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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
    reopened = _reopened(register)
    assert reopened.values(first)["index"]["value"] == 4
    assert reopened.values(second)["index"]["value"] == 7


def test_a_project_path_is_bounded_to_4096_bytes(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    longest = "/" + "p" * 4095
    _reconciled(register, _Manager(), longest, _declaration("index"))
    register.commit(longest, _declaration("index"), 1)
    with pytest.raises(PersistentKeyRegisterError, match="project_path"):
        register.commit(longest + "p", _declaration("index"), 1)


def test_operator_edits_are_audited_with_the_change_and_bounded(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
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

    assert _reopened(register).edits("/cells/a/cell.scenario") == [
        {
            "sequence": 1,
            "project_path": "/cells/a/cell.scenario",
            "key": "index",
            "action": "set",
            "application_start_id": _A,
            "request_key": "request.1",
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
            "application_start_id": _A,
            "request_key": "request.2",
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
    assert register.wait_written(5.0)
    assert len(json.loads(register.path.read_text())["edits"]) == 2


def test_a_run_write_is_audited_with_its_run_node_and_attempt(tmp_path: Path) -> None:
    register = PersistentKeyRegister(tmp_path, application_start_id=_A)
    register.commit(
        "/cells/a/cell.scenario",
        _declaration("index"),
        1,
        edit={
            "program_run_id": 1,
            "node_id": "logic.set_value",
            "node_instance_id": "set_index",
            "node_attempt_id": 4,
        },
    )
    assert _reopened(register).edits("/cells/a/cell.scenario") == [
        {
            "sequence": 1,
            "project_path": "/cells/a/cell.scenario",
            "key": "index",
            "action": "set",
            "application_start_id": _A,
            "request_key": None,
            "source_label": "program_node",
            "program_run_id": 1,
            "node_instance_id": "set_index",
            "node_attempt_id": 4,
            "reason": None,
            "detail": None,
        }
    ]
    # Run and attempt numbers are integers, never text.
    for refused in ({"node_attempt_id": "4"}, {"node_attempt_id": 0}, {"program_run_id": "run.1"}):
        with pytest.raises(PersistentKeyRegisterError, match="positive integer"):
            register.commit(
                "/cells/a/cell.scenario",
                _declaration("index"),
                2,
                edit={"program_run_id": 1, "node_attempt_id": 4, **refused},
            )
    assert register.values("/cells/a/cell.scenario")["index"]["value"] == 1


def test_a_change_keeps_the_application_start_that_wrote_it(tmp_path: Path) -> None:
    """Run numbers restart at every start: each change records its start."""

    path = tmp_path / REGISTER_FILE_NAME
    register = PersistentKeyRegister(path.parent, application_start_id=_A)
    register.commit(
        "/cells/a/cell.scenario",
        _declaration("index"),
        1,
        edit={"program_run_id": 3, "node_instance_id": "set_index", "node_attempt_id": 1},
    )
    assert register.wait_written(5.0)
    document = json.loads(path.read_text())
    assert document["schema_version"] == "manyforge.persistent_key_register.v5"
    assert [edit["application_start_id"] for edit in document["edits"]] == [_A]

    # A later start reads the change with the start that wrote it, and its
    # own changes record the later start.
    later = PersistentKeyRegister(path.parent, application_start_id=_B)
    later.commit(
        "/cells/a/cell.scenario",
        _declaration("index"),
        2,
        edit={"program_run_id": 3, "node_instance_id": "set_index", "node_attempt_id": 1},
    )
    assert [
        (edit["application_start_id"], edit["program_run_id"])
        for edit in _reopened(later, _B).edits("/cells/a/cell.scenario")
    ] == [(_A, 3), (_B, 3)]


@pytest.mark.parametrize("start", ["3F9A00000000002A", "3f9a", "0" * 16, None])
def test_the_register_requires_the_application_start_in_its_text_form(
    tmp_path: Path, start: object
) -> None:
    with pytest.raises(PersistentKeyRegisterError, match="application_start_id"):
        PersistentKeyRegister(tmp_path, application_start_id=start)  # type: ignore[arg-type]


def test_a_v4_register_is_refused_by_name_and_left_untouched(tmp_path: Path) -> None:
    """The current-format file naming another format can only be damaged or
    hand-edited: it is refused by name and left untouched."""

    path = tmp_path / REGISTER_FILE_NAME
    earlier = json.dumps(
        {
            "schema_version": "manyforge.persistent_key_register.v4",
            "sequence": 1,
            "rows": [
                {"project_path": "/cells/a/cell.scenario", "key": "index",
                 "fingerprint": "x", "value": 3, "sequence": 1}
            ],
            "edits": [],
            "activation": None,
            "reconciled_applies": [],
            "unobserved_applies": [],
        }
    )
    path.write_text(earlier)
    with pytest.raises(PersistentKeyRegisterVersionError) as refused:
        PersistentKeyRegister(path.parent, application_start_id=_A)
    assert refused.value.code == "persistent_key_register_version_unsupported"
    assert path.read_text() == earlier

"""A scene commit (OD-293) answers once the Manager status mirror shows it."""

from __future__ import annotations

import copy
import json
from threading import Event, Thread
from types import SimpleNamespace
from typing import Any

import pytest

from manyforge_headless_api.native import HeadlessApiError, NativeHeadlessCore
from manyforge_headless_api.service import (
    RUNTIME_SNAPSHOT_VERSION,
    HeadlessService,
    TrustedAttribution,
)


def _projection(revision_id: int, *, bundle: str = "bundle") -> dict[str, Any]:
    return {
        "retained_bundle_id": bundle,
        "graph_revision": "graph",
        "scene_revision": {
            "scene_id": "scene",
            "revision_id": revision_id,
            "content_hash": f"hash.{revision_id}",
        },
    }


class Mirror:
    """The native core's mirror over a settable logical scene."""

    def __init__(self) -> None:
        self.shown: dict[str, Any] | None = _projection(1)
        self.freshness = "fresh"
        self.snapshots = 0
        self.reads = 0

    def logical_scene_active_projection(self) -> dict[str, Any] | None:
        # The last valid value, whatever its freshness (OD-378).
        self.reads += 1
        return copy.deepcopy(self.shown)

    def runtime_snapshot(self) -> dict[str, Any]:
        self.snapshots += 1
        section = {
            "available": True,
            "freshness": self.freshness,
            "payload": {"access": {"active_projection": copy.deepcopy(self.shown)}},
        }
        return {
            "schema_version": RUNTIME_SNAPSHOT_VERSION,
            "projection_generation": 1,
            "sections": {"logical_scene": section, "scene_runtime": {}},
        }

    def committed_scenario_activation(self) -> dict[str, Any]:
        raise HeadlessApiError("program_applied_selection_not_found", "none")


class Manager:
    def __init__(self) -> None:
        self.committed = Event()

    def commit_scene_transaction(self, request, *, trusted_principal_id, mesh_content_digest):
        self.committed.set()
        return {
            "ok": True,
            "projection_status": "committed",
            "committed_projection_revision": _projection(2),
        }


def _request() -> SimpleNamespace:
    return SimpleNamespace(
        operation=SimpleNamespace(kind="remove_object"),
        request_context=SimpleNamespace(source_label="composer_ui"),
        program_node_evidence=None,
        expected_admission_generation=1,
        program_reset_scene_authorization=None,
    )


def _commit_in_thread(service: HeadlessService) -> tuple[Thread, list[dict[str, Any]]]:
    replies: list[dict[str, Any]] = []
    thread = Thread(
        target=lambda: replies.append(
            service.scene_commit(_request(), TrustedAttribution())
        ),
        daemon=True,
    )
    thread.start()
    return thread, replies


def _waiting(manager: Manager, thread: Thread) -> None:
    assert manager.committed.wait(5.0)
    thread.join(0.1)
    assert thread.is_alive()


def test_reply_waits_until_the_mirror_shows_the_commit() -> None:
    mirror, manager = Mirror(), Manager()
    service = HeadlessService(mirror, manager, application_start_id="3f9a0000000000a1")
    thread, replies = _commit_in_thread(service)
    _waiting(manager, thread)
    # A change that does not yet show the commit keeps the reply waiting.
    service.manager_status_changed()
    thread.join(0.1)
    assert thread.is_alive()

    mirror.shown = _projection(2)
    service.manager_status_changed()
    thread.join(5.0)
    assert not thread.is_alive()
    assert replies[0]["ok"] is True
    # The wait reads the mirror alone, never a full runtime snapshot.
    assert mirror.snapshots == 0
    shown = service.scene()["logical_scene"]["payload"]["access"]["active_projection"]
    assert shown == replies[0]["committed_projection_revision"]


@pytest.mark.parametrize("shown", [_projection(3), _projection(1, bundle="other")])
def test_newer_revision_or_other_lineage_answers_at_once(shown: dict[str, Any]) -> None:
    mirror = Mirror()
    mirror.shown = shown
    service = HeadlessService(mirror, Manager(), application_start_id="3f9a0000000000a1")
    assert service.scene_commit(_request(), TrustedAttribution())["ok"] is True


def test_no_projection_yet_keeps_waiting() -> None:
    # Before the Manager's bootstrap publication the mirror shows none.
    mirror, manager = Mirror(), Manager()
    mirror.shown = None
    service = HeadlessService(mirror, manager, application_start_id="3f9a0000000000a1")
    thread, replies = _commit_in_thread(service)
    _waiting(manager, thread)
    service.manager_status_changed()
    thread.join(0.1)
    assert thread.is_alive()

    mirror.shown = _projection(2)
    service.manager_status_changed()
    thread.join(5.0)
    assert not thread.is_alive()
    assert replies[0]["ok"] is True


def test_change_during_the_mirror_read_is_not_lost() -> None:
    """The change counter is read before the mirror: a publication landing
    between the mirror read and the wait still wakes it (no lost wake-up)."""

    class RacingMirror(Mirror):
        def __init__(self) -> None:
            super().__init__()
            self.service: HeadlessService | None = None

        def logical_scene_active_projection(self) -> dict[str, Any] | None:
            shown = super().logical_scene_active_projection()
            if self.reads == 1:
                # The follower applies the commit's frame right after this
                # read and signals once; no later change ever comes.
                self.shown = _projection(2)
                assert self.service is not None
                self.service.manager_status_changed()
            return shown

    mirror, manager = RacingMirror(), Manager()
    service = HeadlessService(mirror, manager, application_start_id="3f9a0000000000a1")
    mirror.service = service
    thread, replies = _commit_in_thread(service)
    thread.join(5.0)
    assert not thread.is_alive()
    assert replies[0]["ok"] is True
    assert mirror.reads == 2


def test_tick_silence_keeps_waiting_and_disconnect_ends_the_wait() -> None:
    mirror, manager = Mirror(), Manager()
    service = HeadlessService(mirror, manager, application_start_id="3f9a0000000000a1")
    thread, replies = _commit_in_thread(service)
    _waiting(manager, thread)
    # A commit that held the Manager's loop: the mirror reads "not answering"
    # while the commit's frame is still on its way.
    mirror.freshness = "stale"
    service.manager_status_changed()
    thread.join(0.1)
    assert thread.is_alive()

    service.manager_status_disconnected()
    thread.join(5.0)
    assert not thread.is_alive()
    assert replies[0]["ok"] is True


def test_shutdown_ends_the_wait() -> None:
    mirror, manager = Mirror(), Manager()
    service = HeadlessService(mirror, manager, application_start_id="3f9a0000000000a1")
    thread, replies = _commit_in_thread(service)
    _waiting(manager, thread)
    service.close()
    thread.join(5.0)
    assert not thread.is_alive()
    assert replies[0]["ok"] is True


@pytest.mark.parametrize("shown", [None, _projection(4)])
def test_native_accessor_parses_the_mirror_text(shown: dict[str, Any] | None) -> None:
    core = object.__new__(NativeHeadlessCore)
    core._native = SimpleNamespace(
        logical_scene_active_projection=lambda: (
            None if shown is None else json.dumps(shown, sort_keys=True, separators=(",", ":"))
        )
    )
    assert core.logical_scene_active_projection() == shown


_DIGEST = "sha256:" + "a" * 64


class OwnerManager:
    """The Manager's current scene revision after the runner's commit."""

    def __init__(self, revision: dict[str, Any] | Exception) -> None:
        self.revision = revision
        self.reads = 0

    def scene_projection_revision(self) -> dict[str, Any]:
        self.reads += 1
        if isinstance(self.revision, Exception):
            raise self.revision
        return copy.deepcopy(self.revision)


class SceneRunner:
    """The Program Runner commits its scene at the Manager; its reply names
    no revision."""

    def __init__(self, reply: dict[str, Any]) -> None:
        self.reply = reply
        self.prepared = Event()

    def prepare_applied_scene(self, body: dict[str, Any]) -> dict[str, Any]:
        self.prepared.set()
        return copy.deepcopy(self.reply)


_PREPARED = {
    "schema_version": "manyforge.program_scene_preparation.v1",
    "status": "prepared",
    "applied_record_digest": _DIGEST,
    "program_revision": "sha256:" + "4" * 64,
    "admission_generation": 1,
    "prepared_scene": {"schema_version": "manyforge.prepared_program_scene.v1"},
}
_CLEARED = {
    "schema_version": "manyforge.program_scene_preparation.v1",
    "status": "prepared",
    "applied_record_digest": None,
    "program_revision": None,
    "admission_generation": 1,
    "prepared_scene": {
        "schema_version": "manyforge.empty_scene_clear.v1",
        "state": "succeeded",
        "last_observed_remaining": [],
        "refused": [],
    },
}


def _runner_service(manager: OwnerManager, reply: dict[str, Any]) -> tuple[
    HeadlessService, Mirror, SceneRunner
]:
    mirror, runner = Mirror(), SceneRunner(reply)
    service = HeadlessService(
        mirror, manager, application_start_id="3f9a0000000000a1", program_runner=runner
    )
    service.program_applied = lambda: {  # type: ignore[method-assign]
        "source": {"record_digest": _DIGEST},
        "program_revision": "sha256:" + "4" * 64,
    }
    service._current_apply_identity = lambda: (1, 1, "sha256:" + "f" * 64)  # type: ignore[method-assign]
    service.manager_status = lambda: {  # type: ignore[method-assign]
        "source": {"available": True, "freshness": "fresh"},
        "status": {
            "manager_ready": True,
            "runtime": {"converged": True},
            "active": {"canonical_empty": True, "publication": {"publication_generation": 1}},
        },
    }
    return service, mirror, runner


def _preparation(clear: bool) -> SimpleNamespace:
    body = {"request_key": "c1" * 16}
    return SimpleNamespace(
        request_key="c1" * 16,
        clear_empty_scene=clear,
        expected_applied_record_digest=None if clear else _DIGEST,
        expected_publication_generation=1 if clear else None,
        model_dump=lambda mode: dict(body),
    )


@pytest.mark.parametrize("clear", [False, True])
def test_runner_scene_change_answers_once_the_mirror_shows_the_owner_revision(
    clear: bool,
) -> None:
    """The runner commits its scene preparation or empty-scene clear at the
    Manager; Headless answers once its mirror shows the Manager's revision
    read after the runner's reply (OD-293)."""

    manager = OwnerManager(_projection(2))
    service, mirror, runner = _runner_service(manager, _CLEARED if clear else _PREPARED)
    replies: list[dict[str, Any]] = []
    thread = Thread(
        target=lambda: replies.append(service.program_scene_prepare(_preparation(clear))),
        daemon=True,
    )
    thread.start()
    assert runner.prepared.wait(5.0)
    thread.join(0.1)
    assert thread.is_alive()
    # A change that does not yet show the revision keeps the answer waiting.
    service.manager_status_changed()
    thread.join(0.1)
    assert thread.is_alive()

    mirror.shown = _projection(2)
    service.manager_status_changed()
    thread.join(5.0)
    assert not thread.is_alive()
    assert manager.reads == 1
    if clear:
        assert replies[0]["scene_clear"]["state"] == "succeeded"
    else:
        assert replies[0]["status"] == "prepared"


@pytest.mark.parametrize("clear", [False, True])
def test_runner_scene_change_wait_ends_when_the_manager_is_gone(clear: bool) -> None:
    service, _mirror, runner = _runner_service(
        OwnerManager(_projection(2)), _CLEARED if clear else _PREPARED
    )
    thread = Thread(
        target=lambda: service.program_scene_prepare(_preparation(clear)), daemon=True
    )
    thread.start()
    assert runner.prepared.wait(5.0)
    thread.join(0.1)
    assert thread.is_alive()
    service.manager_status_disconnected()
    thread.join(5.0)
    assert not thread.is_alive()


@pytest.mark.parametrize("clear", [False, True])
def test_mirror_already_showing_the_owner_revision_answers_at_once(clear: bool) -> None:
    manager = OwnerManager(_projection(1))
    service, _mirror, _runner = _runner_service(manager, _CLEARED if clear else _PREPARED)
    service.program_scene_prepare(_preparation(clear))
    assert manager.reads == 1


@pytest.mark.parametrize("clear", [False, True])
def test_refused_owner_read_leaves_nothing_to_wait_for(clear: bool) -> None:
    # The owner answers no revision (unavailable or fenced): no wait, and no
    # change would wake one.
    manager = OwnerManager(HeadlessApiError("scene_owner_unavailable", "fenced"))
    service, _mirror, _runner = _runner_service(manager, _CLEARED if clear else _PREPARED)
    service.program_scene_prepare(_preparation(clear))
    assert manager.reads == 1

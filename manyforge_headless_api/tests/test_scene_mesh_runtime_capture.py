"""A mesh an API scene call adds is captured when it is added (OD-335)."""

from __future__ import annotations

from contextlib import contextmanager
import fcntl
import hashlib
import os
from pathlib import Path
import sys

import pytest

from manyforge_headless_api.activation_preparer import ActivationPreparer
from manyforge_headless_api.models import SceneTransactionRequest
from manyforge_headless_api.native import HeadlessApiError
from manyforge_headless_api.service import HeadlessService, TrustedAttribution


PROJECTION = {"retained_bundle_id": "sha256:" + "b" * 64,
              "graph_revision": "sha256:" + "c" * 64,
              "scene_revision": {"scene_id": "sha256:" + "d" * 64, "revision_id": 1,
                                 "content_hash": "sha256:" + "a" * 64}}
COMMITTED = {"ok": True, "projection_status": "committed",
             "committed_projection_revision": PROJECTION}


class Core:
    """A mirror already showing the commit (OD-293)."""

    def logical_scene_active_projection(self):
        return PROJECTION



class Manager:
    def __init__(self, events: list[str] | None = None) -> None:
        self.digests: list[str | None] = []
        self.events = events if events is not None else []

    def commit_scene_transaction(self, request, *, trusted_principal_id, mesh_content_digest):
        self.digests.append(mesh_content_digest)
        self.events.append("commit")
        return COMMITTED


class Preparer:
    def __init__(self, events: list[str] | None = None) -> None:
        self.stored: list[tuple[bytes, str]] = []
        self.events = events if events is not None else []

    @contextmanager
    def content_writer_hold(self):
        self.events.append("hold")
        try:
            yield
        finally:
            self.events.append("release")

    def store_scene_mesh(self, value: bytes, *, resource_uri: str) -> str:
        self.stored.append((value, resource_uri))
        self.events.append("store")
        return "sha256:" + hashlib.sha256(value).hexdigest()


def _request(shape: dict, **extra) -> SceneTransactionRequest:
    return SceneTransactionRequest.model_validate({
        "request_context": {"request_key": "c0" + "02" * 15, "source_label": "mcp"},
        "expected_projection_revision": PROJECTION,
        "expected_admission_generation": 7,
        "operation": {
            "kind": "add_object",
            "scene_resource_id": "part.1",
            "shape": shape,
            "pose": {"position_m": {"x_m": 0.0, "y_m": 0.0, "z_m": 0.0},
                     "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0}},
            "frame_ref": {"kind": "cell_frame", "cell_id": "cell-a"},
        },
        **extra,
    })


def _mesh(uri: str) -> dict:
    return {"type": "mesh", "mesh_resource_uri": uri,
            "mesh_scale": {"x_m": 1.0, "y_m": 1.0, "z_m": 1.0}}


@pytest.fixture()
def cell(tmp_path: Path, monkeypatch: pytest.MonkeyPatch):
    project = tmp_path / "examples" / "cell.scenario"
    (project / "meshes").mkdir(parents=True)
    (project / "meshes" / "part.stl").write_bytes(b"part mesh")
    site = tmp_path / "projects" / "site"
    (site / "meshes").mkdir(parents=True)
    (site / "meshes" / "x.stl").write_bytes(b"site mesh")
    manager, preparer = Manager(), Preparer()
    service = HeadlessService(
        Core(), manager, activation_preparer=preparer,
        application_start_id="3f9a0000000000a1",
        scene_mesh_allowed_roots=(str(tmp_path / "examples"), str(site)),
    )
    monkeypatch.setattr(service, "program_applied_status", lambda: {
        "origin": {"kind": "scenario", "scenario_id": "cell",
                   "source_document_digest": "sha256:" + "1" * 64,
                   "project_path": str(project)}})
    return service, manager, preparer, project, site


def test_api_mesh_is_read_under_the_rule_and_its_digest_sent(cell) -> None:
    service, manager, preparer, _project, site = cell
    for uri, value in (("meshes/part.stl", b"part mesh"),
                       (str(site / "meshes" / "x.stl"), b"site mesh")):
        assert service.scene_commit(_request(_mesh(uri)), TrustedAttribution()) is COMMITTED
        assert preparer.stored[-1] == (value, uri)
        assert manager.digests[-1] == "sha256:" + hashlib.sha256(value).hexdigest()


def test_non_mesh_and_program_commits_send_no_digest(cell) -> None:
    service, manager, preparer, _project, _site = cell
    box = {"type": "box", "box_dimensions_m": {"x_m": 1.0, "y_m": 1.0, "z_m": 1.0}}
    service.scene_commit(_request(box), TrustedAttribution())
    node = _request(_mesh("meshes/part.stl"), program_node_evidence={
        "program_run_id": 1, "parent_program_cycle_claim_generation": 1,
        "node_instance_id": "add", "node_attempt_id": 1, "owner_api": "scene.transaction"})
    service.scene_commit(node, TrustedAttribution())
    assert manager.digests == [None, None]
    assert preparer.stored == []


def test_api_mesh_outside_the_allowed_folders_is_refused(cell, tmp_path: Path) -> None:
    service, manager, preparer, _project, _site = cell
    (tmp_path / "outside.stl").write_bytes(b"outside")
    with pytest.raises(HeadlessApiError) as refused:
        service.scene_commit(_request(_mesh("../../outside.stl")), TrustedAttribution())
    assert refused.value.code == "scene_mesh_unavailable"
    assert str(tmp_path / "outside.stl") in str(refused.value)
    assert str(tmp_path / "examples") in str(refused.value)
    assert manager.digests == [] and preparer.stored == []


def test_api_mesh_without_an_active_apply_is_refused(cell, monkeypatch) -> None:
    service, manager, _preparer, _project, _site = cell

    def no_apply():
        raise HeadlessApiError("program_applied_selection_invalid", "no Apply")

    monkeypatch.setattr(service, "program_applied_status", no_apply)
    with pytest.raises(HeadlessApiError) as refused:
        service.scene_commit(_request(_mesh("meshes/part.stl")), TrustedAttribution())
    assert refused.value.code == "scene_mesh_unavailable"
    assert manager.digests == []


def _store_preparer(tmp_path: Path, script: str, monkeypatch) -> ActivationPreparer:
    """The production preparer client over a fixture store-mode executable."""

    monkeypatch.setattr(
        "manyforge_headless_api.activation_preparer._model_preparation_profiles",
        lambda _root: {},
    )
    executable = tmp_path / "preparer"
    executable.write_text(f"#!{sys.executable}\n{script}", encoding="utf-8")
    executable.chmod(0o755)
    (tmp_path / "artifacts").mkdir()
    return ActivationPreparer(
        executable=executable, capability_root=tmp_path,
        activation_root=tmp_path / "artifacts", runtime_cell_id="cell_a",
        timeout_seconds=10.0,
    )


_STORE = '''
import hashlib, json, sys
arguments = sys.argv[1:]
assert arguments[0] == "--store-scene-mesh" and arguments[2] == "--activation-root"
value = open(arguments[1], "rb").read()
print(json.dumps({"byte_size": len(value),
                  "content_digest": "sha256:" + hashlib.sha256(value).hexdigest()},
                 separators=(",", ":"), sort_keys=True))
'''


def test_preparer_store_mode_returns_the_checked_digest(tmp_path: Path, monkeypatch) -> None:
    preparer = _store_preparer(tmp_path, _STORE, monkeypatch)
    digest = preparer.store_scene_mesh(b"mesh bytes", resource_uri="meshes/part.stl")
    assert digest == "sha256:" + hashlib.sha256(b"mesh bytes").hexdigest()


@pytest.mark.parametrize(("script", "code"), [
    ("import sys\nsys.stderr.write('store failed')\nsys.exit(2)\n", "scene_mesh_unavailable"),
    ("print('{\"byte_size\":1,\"content_digest\":\"sha256:' + '0' * 64 + '\"}')\n",
     "activation_preparer_response_invalid"),
])
def test_preparer_store_mode_failures_are_typed(
    tmp_path: Path, monkeypatch, script: str, code: str,
) -> None:
    preparer = _store_preparer(tmp_path, script, monkeypatch)
    with pytest.raises(HeadlessApiError) as failed:
        preparer.store_scene_mesh(b"mesh bytes", resource_uri="meshes/part.stl")
    assert failed.value.code == code
    if code == "scene_mesh_unavailable":
        assert "store failed" in failed.value.details["preparer_detail"]


def test_api_mesh_is_stored_and_committed_inside_one_content_writer_hold(cell) -> None:
    service, manager, preparer, _project, _site = cell
    events: list[str] = []
    manager.events = preparer.events = events
    service.scene_commit(_request(_mesh("meshes/part.stl")), TrustedAttribution())
    # Content collection cannot run between the store and the commit.
    assert events == ["hold", "store", "commit", "release"]
    events.clear()
    box = {"type": "box", "box_dimensions_m": {"x_m": 1.0, "y_m": 1.0, "z_m": 1.0}}
    service.scene_commit(_request(box), TrustedAttribution())
    assert events == ["commit"]


def test_content_writer_hold_defers_an_exclusive_collector(
    tmp_path: Path, monkeypatch
) -> None:
    preparer = _store_preparer(tmp_path, _STORE, monkeypatch)
    lock_path = tmp_path / "artifacts" / ".activation-content.lock"

    def collector_would_run() -> bool:
        # The Manager's collection: an exclusive, non-blocking lock attempt.
        descriptor = os.open(lock_path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
            return True
        except BlockingIOError:
            return False
        finally:
            os.close(descriptor)

    with preparer.content_writer_hold():
        assert not collector_would_run()
        with preparer.content_writer_hold():  # Shared with a preparation.
            assert not collector_would_run()
    assert collector_would_run()


def test_an_add_without_its_authorization_is_refused_before_any_capture(
    cell, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The core's cross-field rules refuse it (authoring_not_ready) before
    anything is read, stored or sent; the refusal records nothing."""

    service, manager, preparer, _project, _site = cell
    recorded: list[object] = []
    monkeypatch.setattr(service, "_record_scene_change_failure", lambda *args: recorded.append(args))

    def refuse(request, *, trusted_principal_id, mesh_content_digest):
        manager.digests.append(mesh_content_digest)
        raise HeadlessApiError(
            "authoring_not_ready",
            "authored-scene mutation requires exactly one ordinary or Program "
            "Reset authorization",
        )

    monkeypatch.setattr(manager, "commit_scene_transaction", refuse)
    with pytest.raises(HeadlessApiError) as refused:
        service.scene_commit(
            _request(_mesh("meshes/absent.stl"), expected_admission_generation=None),
            TrustedAttribution(),
        )
    assert refused.value.code == "authoring_not_ready"
    assert manager.digests == [None] and preparer.stored == [] and recorded == []

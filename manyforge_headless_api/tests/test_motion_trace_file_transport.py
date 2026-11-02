"""Large diagnostic traces use bounded private files; commands stay small."""

from concurrent.futures import ThreadPoolExecutor
from copy import deepcopy
import fcntl
import hashlib
import json
import math
import os
from pathlib import Path
import socket

import pytest
import rfc8785

from manyforge_headless_api.execution import ExecutionCommandClient, EXECUTION_SOCKET_FILE
from manyforge_headless_api.native import HeadlessApiError


_MAX_BYTES = 3 * 1024 * 1024


def _normal_trace():
    point = {"time_sec": 0, "time_nanosec": 500_000_000,
             "p": [0.125, -0.25], "v": [-0.0, 0.5],
             "a": [-0.0, -0.75], "effort": []}
    goals = []
    for generation in (1, 2):
        goals.append({
            "action": "/arm/follow_joint_trajectory",
            "binding_epoch": 7,
            "controller_goal_id": "ordinary.goal",
            "controller_goal_generation": generation,
            "installation": "unknown",
            "trajectory": {
                "header": {"sec": 1792744684, "nanosec": 1234, "frame_id": ""},
                "joint_names": ["j1", "j2"],
                "points": [deepcopy(point)],
            },
        })
    return {
        "schema_version": "manyforge.volatile_motion_trace.v1",
        "request_owner_epoch": "owner.current",
        "volatile": True,
        "coverage": {"truncated": False, "evicted_goals": 0},
        "goals": goals,
        "controller_samples": [{"cursor_s": 0.5, "applied_factor": 0.5,
                                "reference_v": [0, 0.5]}],
        "scale_events": [{"kind": "applied_observed", "factor": 0.5}],
    }


def _write_payload(directory: Path, payload: bytes, *, owner="owner.current", status="complete"):
    digest = hashlib.sha256(payload).hexdigest()
    filename = f"trace-{digest}.json"
    path = directory / filename
    path.write_bytes(payload)
    path.chmod(0o600)
    metadata = {"schema_version": "manyforge.motion_trace_file.v1", "file": filename,
                "bytes": len(payload), "sha256": f"sha256:{digest}",
                "request_owner_epoch": owner, "status": status}
    return metadata, path


def _client(metadata, *, inline=None):
    response = {"api_version": "manyforge.execution_command_response.v1", "ok": True,
                "command": "read_volatile_motion_trace"}
    if metadata is not ...:
        response["trace_file"] = metadata
    if inline is not None:
        response["trace"] = inline
    client = object.__new__(ExecutionCommandClient)
    client._attribution_transport = "internal"
    client._exchange = lambda _command: response
    return client


@pytest.mark.parametrize("setting", [None, ""])
def test_unconfigured_disk_preserves_inline_trace_without_creating_files(monkeypatch, tmp_path, setting):
    if setting is None:
        monkeypatch.delenv("MANYFORGE_MOTION_TRACE_DIR", raising=False)
    else:
        monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", setting)
    trace = _normal_trace()
    assert _client(..., inline=trace).read_volatile_motion_trace(4096) is trace
    assert list(tmp_path.iterdir()) == []
    with pytest.raises(HeadlessApiError) as caught:
        _client({}).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_invalid"


def test_opted_in_file_preserves_normal_goals_derivatives_and_scale_facts(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    trace = _normal_trace()
    payload = rfc8785.dumps(trace)
    metadata, path = _write_payload(tmp_path, payload)
    observed = _client(metadata).read_volatile_motion_trace(4096)
    assert observed == trace
    assert rfc8785.dumps(observed) == payload == path.read_bytes()
    assert math.copysign(1, observed["goals"][0]["trajectory"]["points"][0]["v"][0]) == 1
    assert math.copysign(1, trace["goals"][0]["trajectory"]["points"][0]["v"][0]) == -1


def test_inline_trace_above_default_send_buffer_crosses_real_seqpacket(monkeypatch, tmp_path):
    """Pins the inline read: the exact four-member request the host decodes,
    and an inline response larger than the default unix send buffer, carried
    in one packet once the accepted socket's own send buffer is raised (the
    listener's never reaches it) and accepted unchanged by the parser."""
    monkeypatch.delenv("MANYFORGE_MOTION_TRACE_DIR", raising=False)
    trace = _normal_trace()
    trace["goals"][1]["trajectory"]["points"] *= 3000
    response = rfc8785.dumps({"api_version": "manyforge.execution_command_response.v1",
                              "command": "read_volatile_motion_trace", "ok": True,
                              "trace": trace})
    assert 212_992 < len(response) < 400_000
    endpoint = tmp_path / EXECUTION_SOCKET_FILE
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
        listener.bind(str(endpoint))
        endpoint.chmod(0o600)
        listener.listen(1)
        listener.settimeout(3)

        def answer():
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4 * 1024 * 1024)
                command = json.loads(connection.recv(4096))
                assert connection.send(response) == len(response)
                return command

        with ThreadPoolExecutor(max_workers=1) as worker:
            answered = worker.submit(answer)
            client = ExecutionCommandClient(tmp_path, timeout_seconds=2, attribution_transport="internal")
            assert client.read_volatile_motion_trace(_MAX_BYTES) == trace
            command = answered.result(timeout=3)
    assert command == {"api_version": "manyforge.execution_command.v1",
                       "attribution": {"transport": "internal", "assurance": "unverified",
                                       "principal_id": None},
                       "command": "read_volatile_motion_trace",
                       "allowance_bytes": _MAX_BYTES}


def test_large_trace_crosses_real_small_seqpacket_only_as_file_metadata(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    trace = _normal_trace()
    trace["goals"][1]["trajectory"]["points"] *= 4000
    payload = rfc8785.dumps(trace)
    assert 212_992 < len(payload) < _MAX_BYTES
    metadata, path = _write_payload(tmp_path, payload)
    response = rfc8785.dumps({"api_version": "manyforge.execution_command_response.v1",
                              "ok": True, "command": "read_volatile_motion_trace",
                              "trace_file": metadata})
    assert len(response) < 1024
    endpoint = tmp_path / EXECUTION_SOCKET_FILE
    with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as listener:
        listener.bind(str(endpoint))
        endpoint.chmod(0o600)
        listener.listen(1)
        listener.settimeout(3)

        def answer():
            connection, _ = listener.accept()
            with connection:
                connection.settimeout(3)
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, 4096)
                command = json.loads(connection.recv(4096))
                assert len(payload) > connection.getsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF)
                assert connection.send(response) == len(response)
                return command

        with ThreadPoolExecutor(max_workers=1) as worker:
            answered = worker.submit(answer)
            client = ExecutionCommandClient(tmp_path, timeout_seconds=2, attribution_transport="internal")
            assert client.read_volatile_motion_trace(_MAX_BYTES) == trace
            command = answered.result(timeout=3)
    assert command["command"] == "read_volatile_motion_trace"
    assert command["allowance_bytes"] == _MAX_BYTES
    assert path.read_bytes() == payload


@pytest.mark.parametrize("field,value", [
    ("schema_version", "other.v1"),
    ("file", "../outside.json"), ("file", "/outside.json"),
    ("file", "trace-" + "A" * 64 + ".json"),
    ("file", "trace-" + "a" * 63 + ".json"), ("file", None),
    ("bytes", True), ("bytes", 0), ("bytes", -1), ("bytes", 1.5),
    ("bytes", _MAX_BYTES + 1), ("bytes", 4097),
    ("sha256", "sha256:" + "0" * 64),
    ("request_owner_epoch", ""), ("request_owner_epoch", None),
    ("request_owner_epoch", "é" * 1025),
    ("status", "installed"), ("status", None), ("status", []),
    ("unexpected", "field"),
])
def test_invalid_metadata_is_rejected_before_file_access(monkeypatch, tmp_path, field, value):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    metadata, _path = _write_payload(tmp_path, rfc8785.dumps(_normal_trace()))
    metadata[field] = value
    monkeypatch.setattr("manyforge_headless_api.execution.PinnedRunRoot",
                        lambda *_args: pytest.fail("invalid metadata reached file access"))
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_invalid"


@pytest.mark.parametrize("metadata", [None, [], {}, {"file": "trace.json"}])
def test_missing_metadata_never_falls_back_to_inline_trace(monkeypatch, tmp_path, metadata):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata, inline=_normal_trace()).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_invalid"


def test_global_trace_cap_applies_even_when_caller_allows_more(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    metadata, _path = _write_payload(tmp_path, rfc8785.dumps(_normal_trace()))
    metadata["bytes"] = _MAX_BYTES + 1
    monkeypatch.setattr("manyforge_headless_api.execution.PinnedRunRoot",
                        lambda *_args: pytest.fail("over-cap metadata reached file access"))
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(2 * _MAX_BYTES)
    assert caught.value.code == "motion_trace_file_invalid"


@pytest.mark.parametrize("payload", [
    b"[]", b"{", b"\xff", b'{"a":1,"a":2}',
    b'{ "schema_version": "manyforge.volatile_motion_trace.v1" }',
    b'{"schema_version":"manyforge.volatile_motion_trace.v2"}',
    rfc8785.dumps({**_normal_trace(), "request_owner_epoch": "owner.old"}),
    b'{"request_owner_epoch":"owner.current","schema_version":"manyforge.volatile_motion_trace.v1","x":-0}',
])
def test_valid_size_and_digest_do_not_hide_invalid_file_content(monkeypatch, tmp_path, payload):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    metadata, _path = _write_payload(tmp_path, payload)
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_invalid"


@pytest.mark.parametrize("unsafe", ["missing", "symlink", "hardlink", "permissions", "directory", "oversized"])
def test_missing_or_unsafe_files_report_unavailable(monkeypatch, tmp_path, unsafe):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    payload = rfc8785.dumps(_normal_trace())
    metadata, path = _write_payload(tmp_path, payload)
    if unsafe in {"missing", "symlink", "directory"}:
        path.unlink()
    if unsafe == "symlink":
        outside = tmp_path / "untrusted.json"
        outside.write_bytes(payload)
        outside.chmod(0o600)
        path.symlink_to(outside)
    elif unsafe == "hardlink":
        os.link(path, tmp_path / "alias.json")
    elif unsafe == "permissions":
        path.chmod(0o644)
    elif unsafe == "directory":
        path.mkdir()
    elif unsafe == "oversized":
        path.write_bytes(payload + b" ")
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_unavailable"


def test_payload_tampering_with_unchanged_size_fails_digest(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    payload = rfc8785.dumps(_normal_trace())
    metadata, path = _write_payload(tmp_path, payload)
    path.write_bytes(payload.replace(b"0.125", b"0.625", 1))
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_invalid"


def test_active_eviction_lock_is_nonblocking_unavailable(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    metadata, _path = _write_payload(tmp_path, rfc8785.dumps(_normal_trace()))
    descriptor = os.open(tmp_path, os.O_RDONLY | os.O_DIRECTORY)
    try:
        fcntl.flock(descriptor, fcntl.LOCK_EX | fcntl.LOCK_NB)
        with pytest.raises(HeadlessApiError) as caught:
            _client(metadata).read_volatile_motion_trace(4096)
        assert caught.value.code == "motion_trace_file_unavailable"
    finally:
        os.close(descriptor)


@pytest.mark.parametrize("unsafe", ["symlink", "writable", "relative", "foreign_owner"])
def test_untrusted_store_root_is_not_followed(monkeypatch, tmp_path, unsafe):
    metadata, _path = _write_payload(tmp_path, rfc8785.dumps(_normal_trace()))
    directory = str(tmp_path)
    if unsafe == "symlink":
        alias = tmp_path / "alias"
        alias.symlink_to(tmp_path, target_is_directory=True)
        directory = str(alias)
    elif unsafe == "writable":
        tmp_path.chmod(0o777)
    elif unsafe == "relative":
        monkeypatch.chdir(tmp_path.parent)
        directory = tmp_path.name
    else:
        original_uid = os.geteuid()
        monkeypatch.setattr("manyforge_headless_api.private_transport.os.geteuid",
                            lambda: original_uid + 1)
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", directory)
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_unavailable"


def test_file_changed_during_read_is_unavailable(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    payload = rfc8785.dumps(_normal_trace())
    metadata, path = _write_payload(tmp_path, payload)
    original_fdopen = os.fdopen

    class ChangedReader:
        def __init__(self, descriptor, mode):
            self.source = original_fdopen(descriptor, mode)

        def __enter__(self):
            return self

        def __exit__(self, *_exception):
            self.source.close()

        def fileno(self):
            return self.source.fileno()

        def read(self, maximum):
            data = self.source.read(maximum)
            path.write_bytes(payload + b" ")
            return data

    monkeypatch.setattr("manyforge_headless_api.private_transport.os.fdopen", ChangedReader)
    with pytest.raises(HeadlessApiError) as caught:
        _client(metadata).read_volatile_motion_trace(4096)
    assert caught.value.code == "motion_trace_file_unavailable"


def test_partial_coverage_is_returned_unchanged_without_claiming_installation(monkeypatch, tmp_path):
    monkeypatch.setenv("MANYFORGE_MOTION_TRACE_DIR", str(tmp_path))
    trace = _normal_trace()
    trace["coverage"].update(truncated=True, evicted_goals=3)
    metadata, _path = _write_payload(tmp_path, rfc8785.dumps(trace), status="partial")
    observed = _client(metadata).read_volatile_motion_trace(4096)
    assert observed == trace
    assert all(goal["installation"] == "unknown" for goal in observed["goals"])

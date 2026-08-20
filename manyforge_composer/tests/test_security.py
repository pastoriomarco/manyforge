"""Runtime-independent Composer transport and filesystem security tests."""

from __future__ import annotations

import os
from pathlib import Path

import pytest
from fastapi import HTTPException

from manyforge_composer.backend.security import (
    SecurityConfig,
    WS_MAX_MESSAGE_SIZE,
    is_path_within_roots,
    normalize_allowed_roots,
    normalize_existing_file_path,
    normalize_writable_file_path,
)


def test_websocket_message_limit_remains_available() -> None:
    assert WS_MAX_MESSAGE_SIZE == 256


def test_existing_file_must_remain_inside_an_allowed_root(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    artifact = allowed / "program.yaml"
    artifact.write_text("name: example\n", encoding="utf-8")

    assert (
        normalize_existing_file_path(
            str(artifact), [str(allowed)], kind_label="Program"
        )
        == artifact.resolve()
    )
    assert is_path_within_roots(artifact.resolve(), [str(allowed)]) is True

    outside = tmp_path / "outside.yaml"
    outside.write_text("name: outside\n", encoding="utf-8")
    with pytest.raises(HTTPException) as caught:
        normalize_existing_file_path(str(outside), [str(allowed)], kind_label="Program")
    assert caught.value.status_code == 403


def test_writable_path_rejects_symlink_targets(tmp_path: Path) -> None:
    allowed = tmp_path / "allowed"
    allowed.mkdir()
    outside = tmp_path / "outside.yaml"
    outside.write_text("outside\n", encoding="utf-8")
    linked = allowed / "program.yaml"
    linked.symlink_to(outside)

    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(str(linked), [str(allowed)], kind_label="Program")
    assert caught.value.status_code == 400

    dangling = allowed / "dangling.yaml"
    dangling.symlink_to(tmp_path / "not-created.yaml")
    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(
            str(dangling), [str(allowed)], kind_label="Program"
        )
    assert caught.value.status_code == 400

    fifo = allowed / "named-pipe.yaml"
    os.mkfifo(fifo)
    with pytest.raises(HTTPException) as caught:
        normalize_writable_file_path(str(fifo), [str(allowed)], kind_label="Program")
    assert caught.value.status_code == 400
    assert "regular file" in str(caught.value.detail)


def test_allowed_roots_are_normalized_and_deduplicated(tmp_path: Path) -> None:
    roots = normalize_allowed_roots([str(tmp_path), str(tmp_path / ".")])

    assert roots == [str(tmp_path.resolve())]

    missing = tmp_path / "missing-root"
    assert normalize_allowed_roots([str(missing)]) == [str(missing.resolve())]


def test_explicit_empty_or_invalid_allowed_roots_do_not_widen_to_workspace() -> None:
    assert normalize_allowed_roots(None) == ["/workspace"]
    assert normalize_allowed_roots([]) == []
    assert normalize_allowed_roots([""]) == []
    assert normalize_allowed_roots(["invalid\x00root"]) == []


def test_program_write_roots_preserve_legacy_default_and_explicit_empty(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.delenv("MANYFORGE_PROGRAM_WRITE_ROOTS", raising=False)
    assert SecurityConfig().allowed_program_write_roots is None
    assert SecurityConfig.from_env().allowed_program_write_roots is None

    monkeypatch.setenv("MANYFORGE_PROGRAM_WRITE_ROOTS", "")
    assert SecurityConfig.from_env().allowed_program_write_roots == []

    monkeypatch.setenv("MANYFORGE_PROGRAM_WRITE_ROOTS", " /one , /two ")
    assert SecurityConfig.from_env().allowed_program_write_roots == ["/one", "/two"]

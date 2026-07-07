"""Tests for the OW8 qualification-evidence collector."""
from __future__ import annotations

import json
from pathlib import Path
import subprocess
import sys


_REPO_ROOT = Path(__file__).resolve().parents[2]
_COLLECTOR = _REPO_ROOT / "manyforge_core" / "tools" / "collect_qualification_evidence.py"


def _read_jsonl(path: Path) -> list[dict]:
    return [
        json.loads(line)
        for line in path.read_text(encoding="utf-8").splitlines()
        if line.strip()
    ]


def test_command_mode_collects_annotated_evidence_and_streams_output(tmp_path: Path) -> None:
    out_dir = tmp_path / "qualification"
    producer = (
        "print('ordinary output'); "
        "print('MANYFORGE_QUALIFICATION_EVIDENCE "
        "{\"scenario\":\"unit_probe\",\"passed\":true}')"
    )

    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--output-dir",
            str(out_dir),
            "--date",
            "2026-07-07",
            "--lane",
            "unit",
            "--run-id",
            "run-collector",
            "--commit",
            "abc123",
            "--source",
            "unit-probe",
            "--",
            sys.executable,
            "-c",
            producer,
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0
    assert "ordinary output" in result.stdout
    assert "MANYFORGE_QUALIFICATION_EVIDENCE" in result.stdout
    records = _read_jsonl(out_dir / "evidence-2026-07-07.jsonl")
    assert len(records) == 1
    record = records[0]
    assert record["schema_version"] == "manyforge.qualification_evidence.v1"
    assert record["lane"] == "unit"
    assert record["collector_run_id"] == "run-collector"
    assert record["repo"] == {
        "root": str(tmp_path.resolve()),
        "commit": "abc123",
        "dirty": False,
    }
    assert record["source"]["name"] == "unit-probe"
    assert record["source"]["command_exit_code"] == 0
    assert record["source"]["command"][0] == sys.executable
    assert record["evidence"] == {"scenario": "unit_probe", "passed": True}


def test_failed_command_evidence_is_preserved_with_exit_code(tmp_path: Path) -> None:
    out_dir = tmp_path / "qualification"
    producer = (
        "import sys; "
        "print('MANYFORGE_QUALIFICATION_EVIDENCE "
        "{\"scenario\":\"failed_probe\",\"passed\":false}'); "
        "sys.exit(3)"
    )

    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--output-dir",
            str(out_dir),
            "--date",
            "2026-07-07",
            "--lane",
            "unit",
            "--commit",
            "abc123",
            "--",
            sys.executable,
            "-c",
            producer,
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 3
    [record] = _read_jsonl(out_dir / "evidence-2026-07-07.jsonl")
    assert record["source"]["command_exit_code"] == 3
    assert record["evidence"]["passed"] is False


def test_invalid_evidence_json_fails_without_writing_record(tmp_path: Path) -> None:
    out_dir = tmp_path / "qualification"
    producer = "print('MANYFORGE_QUALIFICATION_EVIDENCE {not-json}')"

    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--output-dir",
            str(out_dir),
            "--date",
            "2026-07-07",
            "--lane",
            "unit",
            "--commit",
            "abc123",
            "--",
            sys.executable,
            "-c",
            producer,
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 2
    assert "invalid evidence JSON" in result.stderr
    assert not (out_dir / "evidence-2026-07-07.jsonl").exists()


def test_stdin_mode_uses_default_qualification_dir(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--date",
            "2026-07-07",
            "--lane",
            "stdin",
            "--run-id",
            "stdin-run",
            "--commit",
            "abc123",
        ],
        input='MANYFORGE_QUALIFICATION_EVIDENCE {"scenario":"stdin_probe"}\n',
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 0
    [record] = _read_jsonl(tmp_path / "qualification" / "evidence-2026-07-07.jsonl")
    assert record["lane"] == "stdin"
    assert record["collector_run_id"] == "stdin-run"
    assert record["source"]["command"] is None
    assert record["evidence"] == {"scenario": "stdin_probe"}


def test_require_evidence_rejects_empty_success(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--output-dir",
            str(tmp_path / "qualification"),
            "--require-evidence",
            "--",
            sys.executable,
            "-c",
            "print('no evidence here')",
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 4
    assert "no evidence lines collected" in result.stderr


def test_missing_command_returns_runner_failure_without_traceback(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--output-dir",
            str(tmp_path / "qualification"),
            "--require-evidence",
            "--",
            str(tmp_path / "does-not-exist"),
        ],
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 127
    assert "failed to launch" in result.stderr
    assert "Traceback" not in result.stderr


def test_invalid_date_is_rejected_before_path_construction(tmp_path: Path) -> None:
    result = subprocess.run(
        [
            sys.executable,
            str(_COLLECTOR),
            "--repo-root",
            str(tmp_path),
            "--date",
            "../escape",
        ],
        input='MANYFORGE_QUALIFICATION_EVIDENCE {"scenario":"bad_date"}\n',
        text=True,
        capture_output=True,
        check=False,
    )

    assert result.returncode == 2
    assert "invalid evidence date" in result.stderr
    assert not (tmp_path / "qualification").exists()

from pathlib import Path

import pytest

from manyforge_program_runner.cli import _parser


def test_cli_requires_the_execution_root_and_cycle_journal_pair() -> None:
    parser = _parser()
    parsed = parser.parse_args(
        [
            "--execution-run-root",
            "/run/execution",
            "--cycle-journal-path",
            "/state/program-cycle.sqlite3",
        ]
    )
    assert parsed.execution_run_root == Path("/run/execution")
    assert parsed.cycle_journal_path == Path("/state/program-cycle.sqlite3")

    with pytest.raises(SystemExit):
        parser.parse_args(["--execution-run-root", "/run/execution"])
    with pytest.raises(SystemExit):
        parser.parse_args(
            ["--cycle-journal-path", "/state/program-cycle.sqlite3"]
        )

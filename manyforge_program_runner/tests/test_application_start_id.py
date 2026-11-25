"""The runner takes the application start identity ``A`` at launch (1.1)."""

from __future__ import annotations

import pytest

from manyforge_program_runner import cli
from manyforge_program_runner.runtime import (
    ProgramRunnerConfig,
    format_application_start_id,
    parse_application_start_id,
)


def test_the_text_form_is_exactly_sixteen_lowercase_hex() -> None:
    assert parse_application_start_id("3f9a00000000002a") == 0x3F9A00000000002A
    assert format_application_start_id(0x3F9A00000000002A) == "3f9a00000000002a"
    assert format_application_start_id(1) == "0000000000000001"


@pytest.mark.parametrize("text", [
    "0000000000000000", "3F9A00000000002A", "3f9a", "3f9a00000000002a0",
    " 3f9a00000000002a", "program-runner-1",
])
def test_invalid_or_zero_text_is_refused(text: str) -> None:
    with pytest.raises(ValueError):
        parse_application_start_id(text)


def _argv(*extra: str) -> list[str]:
    return [
        "--program-socket-root", "/run/manyforge/manager",
        "--execution-run-root", "/run/manyforge/execution",
        "--cycle-journal-path", "/var/lib/manyforge/program-cycle.sqlite3",
        *extra,
    ]


def test_the_cli_requires_the_argument_and_refuses_an_invalid_value(capsys) -> None:
    with pytest.raises(SystemExit) as missing:
        cli._parser().parse_args(_argv())  # noqa: SLF001
    assert missing.value.code == 2
    with pytest.raises(SystemExit) as invalid:
        cli._parser().parse_args(_argv("--application-start-id", "0" * 16))  # noqa: SLF001
    assert invalid.value.code == 2
    parsed = cli._parser().parse_args(  # noqa: SLF001
        _argv("--application-start-id", "3f9a00000000002a")
    )
    assert parsed.application_start_id == 0x3F9A00000000002A
    capsys.readouterr()


def test_the_owner_id_argument_is_gone() -> None:
    with pytest.raises(SystemExit):
        cli._parser().parse_args(  # noqa: SLF001
            _argv("--application-start-id", "3f9a00000000002a", "--owner-id", "x")
        )


@pytest.mark.parametrize("value", [0, 2**64, "3f9a00000000002a", True])
def test_the_configuration_holds_a_non_zero_64_bit_value(value) -> None:
    with pytest.raises(ValueError):
        ProgramRunnerConfig(headless_url="http://headless.test", application_start_id=value)


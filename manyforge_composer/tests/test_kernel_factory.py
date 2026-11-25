"""Fail-closed contracts for the removed Composer runtime factories."""

from __future__ import annotations

from pathlib import Path
import sys

import pytest

from manyforge_composer import __main__ as composer_main


def test_structural_entrypoint_contains_no_kernel_construction_path() -> None:
    source = Path(composer_main.__file__).read_text(encoding="utf-8")

    assert not hasattr(composer_main, "_make_kernel")
    assert "access_log=False" in source
    assert "ws_per_message_deflate=False" in source
    for forbidden in (
        "MockKernelClient",
        "manyforge_roboplan_py",
        "ros2_control_kernel_factory",
    ):
        assert forbidden not in source


_REMOVED_RUNTIME_INVOCATIONS = (
    ("--catalog-path", "node_catalog.yaml"),
    ("--kernel-mode", "auto"),
    ("--mock-execute-duration-s", "0.1"),
    ("--assistant-provider", "mock"),
    ("--assistant-endpoint", "http://127.0.0.1:9001"),
    ("--assistant-timeout-s", "1"),
    ("--assistant-api-token", "test-token"),
    ("--mcp",),
    ("--trace-dir", "/tmp/manyforge-test-traces"),
    ("--failure-archive-dir", "/tmp/manyforge-test-failures"),
    ("--failure-archive-max-age-s", "1"),
    ("--failure-archive-max-bytes", "1"),
    ("--trace-max-age-s", "1"),
    ("--trace-max-bytes", "1"),
    ("--disable-failure-archive",),
    ("--kernel-mode=mock",),
    ("--unknown-option", "--mcp"),
)


@pytest.mark.parametrize("arguments", _REMOVED_RUNTIME_INVOCATIONS)
def test_removed_runtime_modes_fail_with_the_stable_unavailable_reason(
    arguments: tuple[str, ...],
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    monkeypatch.setattr(sys, "argv", ["manyforge_composer", *arguments])

    def reject_argument_parsing(*_args: object, **_kwargs: object) -> None:
        pytest.fail("removed runtime option reached argparse before its gate")

    def reject_logging(_level: str) -> None:
        pytest.fail("unavailable runtime mode mutated logging before its gate")

    monkeypatch.setattr(
        composer_main.argparse.ArgumentParser,
        "parse_args",
        reject_argument_parsing,
    )
    monkeypatch.setattr(composer_main, "setup_logging", reject_logging)

    with pytest.raises(composer_main.ComposerRuntimeUnavailableError) as error:
        composer_main.main()

    assert error.value.reason_code == "runtime_unavailable"
    assert error.value.to_detail()["missing"] == [
        "runtime_adapter"
    ]


def test_structural_options_are_not_classified_as_removed_runtime_options() -> None:
    assert len(composer_main._REMOVED_RUNTIME_OPTIONS) == 15
    assert composer_main._removed_runtime_option(("--mcp-http",)) is None
    assert composer_main._removed_runtime_option(
        (
            "--host=127.0.0.1",
            "--port",
            "9000",
            "--static-dir=dist",
            "--headless-api-url",
            "http://127.0.0.1:9010",
            "--headless-api-timeout-s=2.0",
            "--log-level",
            "INFO",
        )
    ) is None


@pytest.mark.parametrize(
    "arguments",
    [
        (),
        ("--application-start-id",),
        ("--application-start-id", "0000000000000000"),
        ("--application-start-id", "3F9A0000000000A1"),
        ("--application-start-id", "3f9a"),
        ("--application-start-id", "3f9a0000000000a1ff"),
    ],
)
def test_missing_or_invalid_application_start_id_is_a_usage_error(
    arguments: tuple[str, ...],
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.setattr(sys, "argv", ["manyforge_composer", *arguments])
    monkeypatch.setattr(
        composer_main,
        "setup_logging",
        lambda _level: pytest.fail("a usage error must precede start-up"),
    )

    with pytest.raises(SystemExit) as exited:
        composer_main.main()

    assert exited.value.code == 2
    assert "--application-start-id" in capsys.readouterr().err


def test_the_application_start_id_reaches_the_composer_state(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class Started(Exception):
        pass

    received: dict[str, object] = {}

    def from_paths(**kwargs: object) -> None:
        received.update(kwargs)
        raise Started

    monkeypatch.setattr(
        sys, "argv", ["manyforge_composer", "--application-start-id", "3f9a0000000000a1"]
    )
    monkeypatch.setattr(composer_main, "setup_logging", lambda _level: None)
    monkeypatch.setattr(composer_main.ComposerState, "from_paths", from_paths)

    with pytest.raises(Started):
        composer_main.main()

    assert received["application_start_id"] == 0x3F9A0000000000A1

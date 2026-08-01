"""Fail-closed contracts for the removed Composer runtime factories."""

from __future__ import annotations

from pathlib import Path
import sys

import pytest

from manyforge_composer import __main__ as composer_main


def test_structural_entrypoint_contains_no_kernel_construction_path() -> None:
    source = Path(composer_main.__file__).read_text(encoding="utf-8")

    assert not hasattr(composer_main, "_make_kernel")
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
    ("--mcp-http",),
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

    assert error.value.reason_code == "composable_cell_runtime_unavailable"
    assert error.value.to_detail()["missing"] == [
        "composable_cell_runtime_adapter"
    ]


def test_structural_options_are_not_classified_as_removed_runtime_options() -> None:
    assert len(composer_main._REMOVED_RUNTIME_OPTIONS) == 16
    assert composer_main._removed_runtime_option(
        (
            "--host=127.0.0.1",
            "--port",
            "9000",
            "--deployment-path",
            "deployment.yaml",
            "--static-dir=dist",
            "--log-level",
            "INFO",
        )
    ) is None

from __future__ import annotations

import collections
import importlib.util
import sys
from pathlib import Path
from types import SimpleNamespace

import pytest


REPOSITORY_ROOT = Path(__file__).resolve().parents[1]
RUNNER_PATH = REPOSITORY_ROOT / "scripts/ci/run-ctest-gtest-binaries-grouped.py"


def load_runner():
    name = "manyforge_ctest_gtest_grouped_runner_test_module"
    if name in sys.modules:
        return sys.modules[name]
    spec = importlib.util.spec_from_file_location(name, RUNNER_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[name] = module
    spec.loader.exec_module(module)
    return module


def test_main_collects_independent_group_and_ordinary_failures(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    runner = load_runner()
    groups = collections.OrderedDict(
        (
            binary,
            runner.BinaryGroup(
                binary=binary,
                working_directory=str(tmp_path),
                enabled_filters=[f"Suite.Case{index}"],
                registered_count=1,
            ),
        )
        for index, binary in enumerate(("first", "second", "third"), start=1)
    )
    ordinary = [
        runner.OrdinaryCTest(
            name="OrdinaryOne", disabled=False, timeout_seconds=1
        ),
        runner.OrdinaryCTest(
            name="OrdinaryTwo", disabled=False, timeout_seconds=1
        ),
    ]
    calls: list[str] = []

    monkeypatch.setattr(
        runner,
        "parse_args",
        lambda: SimpleNamespace(
            build_dir=str(tmp_path),
            test_regex=None,
            exclude_test_regex=None,
            summary_prefix="TEST_RUNNER",
            timeout_seconds=300.0,
        ),
    )
    monkeypatch.setattr(runner, "load_registry", lambda *_args: [{}] * 5)
    monkeypatch.setattr(
        runner, "group_registry", lambda *_args: (groups, ordinary)
    )

    def run_group(index, group):
        calls.append(group.binary)
        if group.binary in {"first", "third"}:
            raise RuntimeError(f"failed {group.binary}")
        return 1, 1, 0, False

    def run_ordinary(index, test, build_dir):
        calls.append(test.name)
        if test.name == "OrdinaryOne":
            raise RuntimeError("failed ordinary")
        return 1, 1, 0

    monkeypatch.setattr(runner, "run_group", run_group)
    monkeypatch.setattr(runner, "run_ordinary_test", run_ordinary)

    assert runner.main() == 1
    assert calls == ["first", "second", "third", "OrdinaryOne", "OrdinaryTwo"]
    output = capsys.readouterr().out
    assert "TEST_RUNNER_FAILURE_SUMMARY failures=3" in output
    assert "identity=\"first\"" in output
    assert "identity=\"third\"" in output
    assert "identity=\"OrdinaryOne\"" in output


def test_isolated_fallback_runs_every_filter_before_reporting_failures(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    runner = load_runner()
    group = runner.BinaryGroup(
        binary="binary",
        working_directory=str(tmp_path),
        enabled_filters=["Suite.One", "Suite.Two", "Suite.Three"],
        registered_count=3,
        timeout_seconds=1,
    )
    calls: list[str] = []

    def execute(command, _working_directory, _timeout_seconds):
        calls.append(command[1])
        if command[1] == "--gtest_filter=Suite.Two":
            return (
                0,
                "[==========] 1 test from 1 test suite ran.\n"
                "[  PASSED  ] 1 test.\n",
                False,
            )
        return 1, "[==========] 1 test from 1 test suite ran.\n", False

    monkeypatch.setattr(runner, "execute", execute)

    with pytest.raises(RuntimeError, match="2 isolated GTest execution failure"):
        runner.run_isolated_fallback(1, group)

    assert calls == [
        "--gtest_filter=Suite.One",
        "--gtest_filter=Suite.Two",
        "--gtest_filter=Suite.Three",
    ]
    assert "GROUP_FALLBACK_RESULT index=1 status=1" in capsys.readouterr().out


def test_deployment_ordinary_check_uses_the_configured_test_timeout() -> None:
    source = (
        REPOSITORY_ROOT / "manyforge_deployment/CMakeLists.txt"
    ).read_text(encoding="utf-8")
    block = source.split("NAME MaintainedDeploymentOccurrenceSources", 1)[1].split(
        "function(manyforge_try_public_header", 1
    )[0]

    assert "PROPERTIES TIMEOUT ${MANYFORGE_GTEST_TIMEOUT_SECONDS}" in block
    assert "PROPERTIES TIMEOUT 60" not in block

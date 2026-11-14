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
            "--application-start-id",
            "3f9a00000000002a",
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


def test_headless_client_of_the_runner_expires_idle_connections_first(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from manyforge_headless_api.program_runner_client import ProgramRunnerHttpClient
    from manyforge_headless_api.tests.test_internal_http_keepalive import (
        client_keep_alive_expiry,
        server_keep_alive,
    )
    from manyforge_program_runner import cli

    class _Closable:
        def close(self) -> None:
            pass

    monkeypatch.setattr(cli, "OpenAICompatibleProgramModelTransport", _Closable)
    monkeypatch.setattr(cli, "ProgramRuntime", lambda *_args, **_kwargs: object())
    monkeypatch.setattr(cli, "create_app", lambda _runtime: object())
    server = server_keep_alive(
        monkeypatch,
        cli,
        lambda: cli.main([
            "--execution-run-root", "/run/execution",
            "--cycle-journal-path", "/state/program-cycle.sqlite3",
            "--application-start-id", "3f9a00000000002a",
        ]),
    )
    client = ProgramRunnerHttpClient("http://127.0.0.1:9011")
    try:
        expiry = client_keep_alive_expiry(client._client)
    finally:
        client.close()
    assert expiry < server


def test_a_cycle_journal_that_cannot_be_opened_refuses_the_start_by_code(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    import sqlite3

    from manyforge_program_runner import cli, cycle_journal

    def full_disk(*_args, **_kwargs):
        raise sqlite3.OperationalError("database or disk is full")

    monkeypatch.setattr(cycle_journal.sqlite3, "connect", full_disk)
    monkeypatch.setattr(
        cli,
        "uvicorn",
        type("Uvicorn", (), {"run": staticmethod(lambda *_a, **_k: pytest.fail("served"))}),
    )
    execution_run_root = tmp_path.resolve() / "execution-run"
    execution_run_root.mkdir(mode=0o700)
    state = tmp_path.resolve() / "state"
    state.mkdir(mode=0o700)

    status = cli.main([
        "--execution-run-root", str(execution_run_root),
        "--cycle-journal-path", str(state / "program-cycle.sqlite3"),
        "--application-start-id", "3f9a00000000002a",
    ])

    # No traceback: one typed line and the exit status of the runner's other
    # "unavailable" start refusals.
    assert status == 69
    assert capsys.readouterr().err == (
        "program_cycle_journal_unavailable: Program-cycle journal cannot be "
        "opened: database or disk is full\n"
    )


def test_a_failed_start_still_closes_the_simulation_port(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    import sys
    import types

    from manyforge_program_runner import cli

    class _Closable:
        def close(self) -> None:
            pass

    class _Port:
        closed = 0
        ros_left_running = False

        def close(self) -> None:
            _Port.closed += 1

    port = _Port()
    module = types.ModuleType("manyforge_ros.program_simulation_port")
    module.create_ros_simulation_program_port = lambda *_args, **_kwargs: port
    monkeypatch.setitem(sys.modules, "manyforge_ros.program_simulation_port", module)
    monkeypatch.setattr(cli, "OpenAICompatibleProgramModelTransport", _Closable)
    monkeypatch.setattr(cli, "ProgramRuntime", lambda *_args, **_kwargs: object())
    monkeypatch.setattr(cli, "create_app", lambda _runtime: object())

    def failed_start(*_args, **_kwargs):
        # uvicorn's exit after the lifespan's startup (runtime.start()) raised.
        raise SystemExit(3)

    monkeypatch.setattr(cli, "uvicorn", type("Uvicorn", (), {"run": staticmethod(failed_start)}))

    with pytest.raises(SystemExit) as exited:
        cli.main([
            "--execution-run-root", "/run/execution",
            "--cycle-journal-path", "/state/program-cycle.sqlite3",
            "--application-start-id", "3f9a00000000002a",
            "--simulation-control-config", "/site/simulation.yaml",
        ])

    assert exited.value.code == 3
    assert _Port.closed == 1


class _EndedNow(Exception):
    """Raised by the test's stand-in for ``os._exit``."""

    def __init__(self, status: int) -> None:
        super().__init__(status)
        self.status = status


def _end_now_instead_of_exiting(monkeypatch: pytest.MonkeyPatch):
    from manyforge_program_runner import cli

    def end_now(status: int) -> None:
        raise _EndedNow(status)

    monkeypatch.setattr(cli, "_exit_now", end_now)
    return cli


def _simulation_runner(monkeypatch: pytest.MonkeyPatch, create_port, run) -> object:
    import sys
    import types

    cli = _end_now_instead_of_exiting(monkeypatch)

    class _Closable:
        def close(self) -> None:
            pass

    module = types.ModuleType("manyforge_ros.program_simulation_port")
    module.create_ros_simulation_program_port = create_port
    monkeypatch.setitem(sys.modules, "manyforge_ros.program_simulation_port", module)
    monkeypatch.setattr(cli, "OpenAICompatibleProgramModelTransport", _Closable)
    monkeypatch.setattr(cli, "ProgramRuntime", lambda *_args, **_kwargs: object())
    monkeypatch.setattr(cli, "create_app", lambda _runtime: object())
    monkeypatch.setattr(cli, "uvicorn", type("Uvicorn", (), {"run": staticmethod(run)}))
    return cli


_SIMULATION_ARGS = [
    "--execution-run-root", "/run/execution",
    "--cycle-journal-path", "/state/program-cycle.sqlite3",
    "--application-start-id", "3f9a00000000002a",
    "--simulation-control-config", "/site/simulation.yaml",
]


class _StuckPort:
    """A simulation port whose stop left its rclpy spin thread running."""

    def __init__(self, *, closed_by_runtime: bool) -> None:
        self.ros_left_running = closed_by_runtime
        self.closed = 0

    def close(self) -> None:
        self.closed += 1
        if self.ros_left_running:
            return  # already closed by the runtime's shutdown: a no-op
        self.ros_left_running = True
        raise RuntimeError("simulation rclpy executor did not stop")


def test_a_simulation_thread_left_running_at_shutdown_ends_the_runner_at_once(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    # The lifespan's runtime.shutdown() closed the port and its thread did not
    # stop; uvicorn logs that failure and returns normally.
    port = _StuckPort(closed_by_runtime=True)
    cli = _simulation_runner(
        monkeypatch, lambda *_args, **_kwargs: port, lambda *_args, **_kwargs: None
    )

    with pytest.raises(_EndedNow) as ended:
        cli.main(_SIMULATION_ARGS)

    assert ended.value.status == 65
    assert capsys.readouterr().err == (
        "program_runner_shutdown_failed: simulation rclpy executor did not stop\n"
    )


def test_a_simulation_thread_left_running_after_a_failed_start_ends_at_once(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    port = _StuckPort(closed_by_runtime=False)

    def failed_start(*_args, **_kwargs):
        raise SystemExit(3)

    cli = _simulation_runner(
        monkeypatch, lambda *_args, **_kwargs: port, failed_start
    )

    with pytest.raises(_EndedNow) as ended:
        cli.main(_SIMULATION_ARGS)

    assert ended.value.status == 65
    assert port.closed == 1
    assert capsys.readouterr().err == (
        "program_runner_shutdown_failed: simulation rclpy executor did not stop\n"
    )


def test_a_simulation_port_that_cannot_start_or_stop_ends_the_runner_at_once(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    class _NotStopped(RuntimeError):
        ros_left_running = True

    def create_port(*_args, **_kwargs):
        raise _NotStopped("simulation rclpy executor did not stop")

    cli = _simulation_runner(
        monkeypatch,
        create_port,
        lambda *_args, **_kwargs: pytest.fail("served"),
    )

    with pytest.raises(_EndedNow) as ended:
        cli.main(_SIMULATION_ARGS)

    assert ended.value.status == 69
    assert capsys.readouterr().err == (
        "program_runner_simulation_owner_unavailable: "
        "simulation rclpy executor did not stop\n"
    )


def test_a_simulation_port_that_stopped_leaves_the_normal_exit(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    class _Port:
        ros_left_running = False

        def close(self) -> None:
            pass

    cli = _simulation_runner(
        monkeypatch,
        lambda *_args, **_kwargs: _Port(),
        lambda *_args, **_kwargs: None,
    )

    assert cli.main(_SIMULATION_ARGS) == 0

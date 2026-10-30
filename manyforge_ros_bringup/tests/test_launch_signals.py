"""sim-plc-flake-01: the supervised launches keep repeated SIGINTs out of asyncio tasks."""

from __future__ import annotations

import importlib.util
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time
from types import SimpleNamespace

import pytest

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:  # run from the source tree without a colcon build
    sys.path.insert(0, str(ROOT))

from manyforge_ros_bringup import launch_signals  # noqa: E402
from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks  # noqa: E402

LAUNCH_FILES = {
    "mock_control.launch.py": "launch_setup",
    "isaac_topic_based_control.launch.py": "_launch_setup",
    "isaac_gripper.launch.py": "_launch_setup",
    "sim_plc.launch.py": "launch_setup",
}


@pytest.fixture
def default_sigint():
    previous = signal.signal(signal.SIGINT, signal.default_int_handler)
    yield
    signal.signal(signal.SIGINT, previous)


def _running():
    return SimpleNamespace(asyncio_loop=object())


def test_installs_a_callable_only_for_a_running_launch_service(default_sigint):
    assert keep_sigint_out_of_tasks(SimpleNamespace(asyncio_loop=None)) is False
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler
    assert keep_sigint_out_of_tasks(_running()) is True
    installed = signal.getsignal(signal.SIGINT)
    # A Python callable keeps the C handler (and so the wakeup-fd write).
    assert callable(installed) and installed not in (signal.SIG_IGN, signal.SIG_DFL)
    assert installed(signal.SIGINT, None) is None  # raises nothing
    assert keep_sigint_out_of_tasks(_running()) is False  # idempotent


@pytest.mark.parametrize("chosen", ["custom", signal.SIG_IGN, signal.SIG_DFL])
def test_never_replaces_a_handler_someone_else_chose(default_sigint, chosen):
    handler = (lambda signum, frame: None) if chosen == "custom" else chosen
    signal.signal(signal.SIGINT, handler)
    assert keep_sigint_out_of_tasks(_running()) is False
    assert signal.getsignal(signal.SIGINT) is handler


def test_off_the_main_thread_it_does_nothing(default_sigint):
    result = []
    worker = threading.Thread(target=lambda: result.append(keep_sigint_out_of_tasks(_running())))
    worker.start()
    worker.join()
    assert result == [False]
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler


def _launch_module(name):
    spec = importlib.util.spec_from_file_location(f"flake_{name}", ROOT / "launch" / name)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


class _Called(Exception):
    pass


@pytest.mark.parametrize("name,setup", sorted(LAUNCH_FILES.items()))
def test_launch_files_install_it_first_in_their_setup_only(default_sigint, name, setup):
    module = _launch_module(name)
    assert module.keep_sigint_out_of_tasks is launch_signals.keep_sigint_out_of_tasks
    # Loading the description (ros2 launch --print/--show-args) installs nothing.
    module.generate_launch_description()
    assert signal.getsignal(signal.SIGINT) is signal.default_int_handler

    def first(context):
        raise _Called(context)

    module.keep_sigint_out_of_tasks = first
    marker = object()
    # Called before any launch configuration is read or any Node exists.
    with pytest.raises(_Called) as called:
        getattr(module, setup)(marker)
    assert called.value.args == (marker,)


CHILD = """\
import signal, sys
from pathlib import Path
root = Path(sys.argv[1])
def interrupted(signum, frame):
    (root / 'child_signal').write_text(str(signum))
    sys.exit(0)
signal.signal(signal.SIGINT, interrupted)
(root / 'child_pid').write_text('1')
while True:
    signal.pause()
"""

LAUNCHER = """\
import sys
from pathlib import Path
from launch import LaunchDescription, LaunchService
from launch.actions import ExecuteProcess, OpaqueFunction, RegisterEventHandler
from launch.event_handlers import OnProcessExit
from manyforge_ros_bringup.launch_signals import keep_sigint_out_of_tasks
root = Path(sys.argv[1])
child = ExecuteProcess(cmd=[sys.executable, str(root / 'child.py'), str(root)])
def exited(event, context):
    (root / 'child_exit_code').write_text(str(event.returncode))
service = LaunchService(noninteractive=True)
service.include_launch_description(LaunchDescription([
    OpaqueFunction(function=lambda context: keep_sigint_out_of_tasks(context) and None),
    RegisterEventHandler(OnProcessExit(target_action=child, on_exit=exited)),
    child,
]))
sys.exit(service.run())
"""


def _wait(path, bound_s=2.0):
    until = time.monotonic() + bound_s
    while time.monotonic() < until:
        if path.exists() and path.stat().st_size > 0:
            return True
        time.sleep(0.01)
    return False


@pytest.mark.parametrize("sigints", [1, 2, 3])
def test_launch_service_still_shuts_down_on_a_sigint_burst(tmp_path, sigints):
    pytest.importorskip("launch")
    for attempt in range(10):
        root = tmp_path / f"{sigints}-{attempt}"
        root.mkdir()
        (root / "child.py").write_text(CHILD)
        (root / "launcher.py").write_text(LAUNCHER)
        env = dict(os.environ, PYTHONPATH=f"{ROOT}{os.pathsep}{os.environ.get('PYTHONPATH', '')}")
        proc = subprocess.Popen(
            [sys.executable, str(root / "launcher.py"), str(root)], env=env,
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            assert _wait(root / "child_pid", 10.0)
            for _ in range(sigints):
                os.kill(proc.pid, signal.SIGINT)
            assert _wait(root / "child_signal"), f"attempt {attempt}: child not interrupted"
            assert (root / "child_signal").read_text() == str(int(signal.SIGINT))
            assert _wait(root / "child_exit_code")
            assert (root / "child_exit_code").read_text() == "0"
            assert proc.wait(timeout=2.0) == 0  # LaunchService returned and exited
        finally:
            if proc.poll() is None:
                os.killpg(proc.pid, signal.SIGKILL)
                proc.wait()
        with pytest.raises(ProcessLookupError):
            os.killpg(proc.pid, 0)  # no survivor in the launch's group

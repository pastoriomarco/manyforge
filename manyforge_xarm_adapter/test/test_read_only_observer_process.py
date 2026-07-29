from __future__ import annotations

import os
from pathlib import Path
import select
import signal
import subprocess
import time


def test_disconnected_launch_exits_cleanly_on_sigint(tmp_path: Path) -> None:
    model = tmp_path / 'lite6.urdf'
    model.write_text(
        """\
<robot name="lite6">
  <link name="world"/>
</robot>
""",
        encoding='utf-8',
    )
    process = subprocess.Popen(
        [
            'ros2',
            'launch',
            'manyforge_xarm_adapter',
            'lite6_real_read_only.launch.py',
            'connect_to_robot:=false',
            f'model_urdf_path:={model}',
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        start_new_session=True,
    )
    output: list[str] = []
    try:
        assert process.stdout is not None
        deadline = time.monotonic() + 10.0
        ready = False
        while time.monotonic() < deadline:
            readable, _, _ = select.select([process.stdout], [], [], 0.1)
            if not readable:
                if process.poll() is not None:
                    break
                continue
            line = process.stdout.readline()
            if not line:
                break
            output.append(line)
            if 'xArm observation started' in line:
                ready = True
                break
        assert ready, ''.join(output)

        diagnostic = subprocess.run(
            [
                'ros2',
                'topic',
                'echo',
                '--no-daemon',
                '--once',
                '--timeout',
                '5',
                '/manyforge/xarm/diagnostics',
                'diagnostic_msgs/msg/DiagnosticArray',
            ],
            capture_output=True,
            text=True,
            check=False,
            timeout=7.0,
        )
        assert diagnostic.returncode == 0, diagnostic.stderr

        os.killpg(process.pid, signal.SIGINT)
        remainder, _ = process.communicate(timeout=10.0)
        output.append(remainder)

        combined = ''.join(output)
        assert process.returncode == 0, combined
        assert 'Traceback' not in combined
        assert 'RCLError' not in combined
        assert 'process has died' not in combined
    finally:
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait(timeout=5.0)

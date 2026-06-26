#!/usr/bin/env python3
"""Capture ManyMove Lite6 runtime robot descriptions from a running ROS graph.

Run this inside the ManyMove Jazzy/xArm container after launching the Lite6
FoundationPose or fake MoveGroup scenario. It reads ROS parameters and writes
the two files consumed by ``compare_lite6_runtime_snapshot.py``.
"""

from __future__ import annotations

import argparse
import datetime as dt
import json
import subprocess
import time
from pathlib import Path

DEFAULT_LAUNCH_COMMAND = [
    "ros2",
    "launch",
    "manymove_bringup",
    "lite_foundationpose_movegroup_fake_cpp_trees.launch.py",
    "no_gui_ctrl:=true",
]
DEFAULT_XARM_ROOT = Path("/opt/manymove_ws/src/xarm_ros2")
DEFAULT_XARM_COMMIT_FILE = Path("/opt/manymove_ws/.xarm_ros2_commit")


def _read_commit_file(path: Path) -> str | None:
    try:
        commit = path.read_text(encoding="utf-8").strip()
    except FileNotFoundError:
        return None
    if not commit:
        return None
    return commit


def _git_head(path: Path) -> str | None:
    if not path.is_dir():
        return None
    try:
        completed = subprocess.run(
            ["git", "-C", str(path), "rev-parse", "HEAD"],
            check=True,
            text=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        return None
    commit = completed.stdout.strip()
    return commit or None


def resolve_xarm_commit(
    explicit_commit: str | None,
    commit_file: Path,
    xarm_root: Path,
) -> str:
    if explicit_commit:
        return explicit_commit
    commit = _read_commit_file(commit_file)
    if commit:
        return commit
    commit = _git_head(xarm_root)
    if commit:
        return commit
    raise SystemExit(
        "unable to determine xarm_ros2 commit; pass --xarm-commit explicitly "
        f"or run inside the ManyMove xArm container with {commit_file}"
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--node", default="/move_group")
    parser.add_argument("--robot-description-param", default="robot_description")
    parser.add_argument(
        "--semantic-param", default="robot_description_semantic"
    )
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=Path("manyforge_example_models/models/lite6_lite_gripper/manymove_runtime_capture"),
    )
    parser.add_argument("--manymove-image", default="manymove-xarm:jazzy")
    parser.add_argument(
        "--xarm-repo",
        default="https://github.com/pastoriomarco/xarm_ros2",
    )
    parser.add_argument("--xarm-branch", default="jazzy_no_gazebo")
    parser.add_argument(
        "--xarm-commit",
        default=None,
        help=(
            "Exact xarm_ros2 commit. If omitted, the capture script reads "
            "/opt/manymove_ws/.xarm_ros2_commit or git rev-parse on --xarm-root."
        ),
    )
    parser.add_argument("--xarm-root", type=Path, default=DEFAULT_XARM_ROOT)
    parser.add_argument(
        "--xarm-commit-file",
        type=Path,
        default=DEFAULT_XARM_COMMIT_FILE,
    )
    parser.add_argument(
        "--launch-command",
        nargs="+",
        default=DEFAULT_LAUNCH_COMMAND,
        help="Exact command used to launch the runtime owner before capture.",
    )
    parser.add_argument("--timeout-s", type=float, default=20.0)
    args = parser.parse_args()
    xarm_commit = resolve_xarm_commit(
        args.xarm_commit,
        args.xarm_commit_file,
        args.xarm_root,
    )

    try:
        import rclpy
        from rcl_interfaces.srv import GetParameters
    except ImportError as exc:
        raise SystemExit(
            "rclpy is required. Run this inside the ManyMove ROS container after "
            "sourcing the Jazzy workspace."
        ) from exc

    rclpy.init()
    node = rclpy.create_node("manyforge_lite6_runtime_description_capture")
    try:
        service_name = f"{args.node.rstrip('/')}/get_parameters"
        client = node.create_client(GetParameters, service_name)
        deadline = time.monotonic() + args.timeout_s
        while not client.wait_for_service(timeout_sec=0.25):
            if time.monotonic() >= deadline:
                raise SystemExit(f"parameter service not available: {service_name}")

        request = GetParameters.Request()
        request.names = [args.robot_description_param, args.semantic_param]
        future = client.call_async(request)
        while rclpy.ok() and not future.done():
            if time.monotonic() >= deadline:
                raise SystemExit(f"timed out waiting for parameters from {args.node}")
            rclpy.spin_once(node, timeout_sec=0.1)

        response = future.result()
        if response is None or len(response.values) != 2:
            raise SystemExit(f"unexpected parameter response from {args.node}")
        urdf = response.values[0].string_value
        srdf = response.values[1].string_value
        if not urdf.strip() or not srdf.strip():
            raise SystemExit(
                f"{args.node} did not expose non-empty robot_description and "
                "robot_description_semantic"
            )

        args.output_dir.mkdir(parents=True, exist_ok=True)
        urdf_path = args.output_dir / "manymove_lite6_runtime.urdf"
        srdf_path = args.output_dir / "manymove_lite6_runtime.srdf"
        meta_path = args.output_dir / "manymove_lite6_runtime_capture.json"
        urdf_path.write_text(urdf, encoding="utf-8")
        srdf_path.write_text(srdf, encoding="utf-8")
        meta_path.write_text(
            json.dumps(
                {
                    "schema": "manyforge.lite6_runtime_capture.v1",
                    "node": args.node,
                    "robot_description_param": args.robot_description_param,
                    "robot_description_semantic_param": args.semantic_param,
                    "manymove": {
                        "image": args.manymove_image,
                        "launch_command": list(args.launch_command),
                        "runtime_owner_node": args.node,
                    },
                    "xarm_ros2": {
                        "repo": args.xarm_repo,
                        "branch": args.xarm_branch,
                        "commit": xarm_commit,
                    },
                    "artifacts": {
                        "urdf": str(urdf_path),
                        "srdf": str(srdf_path),
                    },
                    "captured_at_utc": dt.datetime.now(dt.timezone.utc)
                    .replace(microsecond=0)
                    .isoformat(),
                },
                indent=2,
            )
            + "\n",
            encoding="utf-8",
        )
        print(f"Wrote {urdf_path}")
        print(f"Wrote {srdf_path}")
        print(f"Wrote {meta_path}")
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()


if __name__ == "__main__":
    raise SystemExit(main())

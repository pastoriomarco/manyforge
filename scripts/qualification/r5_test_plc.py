#!/usr/bin/env python3
"""Explicit, foreground-only ROS test peer for the R5 trial PLC endpoints."""

from __future__ import annotations

import argparse
from contextlib import ExitStack
import math
from pathlib import Path
import signal
import subprocess
import sys
import time


def response_delay(value: str) -> float:
    result = float(value)
    if not math.isfinite(result) or result < 0:
        raise argparse.ArgumentTypeError("response delay must be finite and nonnegative")
    return result


def check_endpoints_unused() -> None:
    import rclpy

    rclpy.init()
    node = rclpy.create_node("r5_test_plc_preflight")
    try:
        until = time.monotonic() + 1.0
        while time.monotonic() < until:
            rclpy.spin_once(node, timeout_sec=0.1)
            # Only an actual server or publisher occupies an endpoint; the
            # graph also lists a service name that has only clients, such as
            # ManyForge's own plc_io client once a Scenario is loaded.
            if node.count_services("/plc_io/start") or node.count_publishers("/plc_io/ready"):
                raise RuntimeError("PLC test endpoints already have a server or publisher")
    finally:
        node.destroy_node()
        rclpy.shutdown()


def serve(delay: float) -> None:
    import rclpy
    from std_srvs.srv import SetBool

    rclpy.init()
    node = rclpy.create_node("plc_io_peer")

    def callback(request, response):
        received_ns = time.monotonic_ns()
        time.sleep(delay)
        response.success = True
        response.message = str(received_ns)
        node.get_logger().info(
            f"SetBool({request.data}) recv_ns={received_ns} delay={delay}"
        )
        return response

    node.create_service(SetBool, "/plc_io/start", callback)
    try:
        rclpy.spin(node)
    finally:
        node.destroy_node()
        if rclpy.ok():
            rclpy.shutdown()


def peer_commands(delay: float) -> tuple[list[str], list[str]]:
    return (
        ["ros2", "topic", "pub", "-r", "10", "/plc_io/ready", "std_msgs/msg/Bool", "{data: true}"],
        [sys.executable, "-u", str(Path(__file__).resolve()), "_serve", str(delay)],
    )


def stop_children(children: list[subprocess.Popen]) -> None:
    # Only handles created by this invocation: no PID files or process-name kills.
    for child in children:
        if child.poll() is None:
            child.terminate()
    for child in children:
        try:
            child.wait(timeout=5)
        except subprocess.TimeoutExpired:
            child.kill()
            child.wait()


def run_peers(delay: float, log_dir: Path) -> int:
    log_dir.mkdir(parents=True, exist_ok=False)
    children: list[subprocess.Popen] = []
    try:
        with ExitStack() as logs:
            for name, command in zip(("ready-publisher", "setbool-peer"), peer_commands(delay)):
                log_path = log_dir / f"{name}.log"
                log = logs.enter_context(log_path.open("w", encoding="utf-8"))
                child = subprocess.Popen(
                    command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True
                )
                children.append(child)
                print(f"{name}: pid={child.pid} log={log_path}", flush=True)
            print("Test PLC running; Ctrl-C stops both owned peers. No application commands sent.", flush=True)
            while True:
                for child in children:
                    if child.poll() is not None:
                        raise RuntimeError(f"test PLC child {child.pid} exited with {child.returncode}")
                time.sleep(0.1)
    except KeyboardInterrupt:
        return 130
    finally:
        stop_children(children)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    start = commands.add_parser("start", help="run until Ctrl-C; both peers are cleaned up on exit")
    start.add_argument("--response-delay-s", type=response_delay, default=0.1)
    start.add_argument("--log-dir", type=Path, required=True, help="new directory for this invocation")
    service = commands.add_parser("_serve", help=argparse.SUPPRESS)
    service.add_argument("delay", type=response_delay)
    args = parser.parse_args()
    if args.command == "_serve":
        serve(args.delay)
        return 0

    def interrupted(_signum, _frame):
        raise KeyboardInterrupt

    previous = signal.signal(signal.SIGTERM, interrupted)
    try:
        check_endpoints_unused()
        return run_peers(args.response_delay_s, args.log_dir)
    except (OSError, RuntimeError) as error:
        print(f"Test PLC refused/failed: {error}", file=sys.stderr)
        return 1
    finally:
        signal.signal(signal.SIGTERM, previous)


if __name__ == "__main__":
    raise SystemExit(main())

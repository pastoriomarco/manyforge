#!/usr/bin/env python3
"""Comparable qualification-only RoboPlan 0.5/0.6 offline workload.

This is not a planner adapter or a product runtime. It exercises only the
common public dependency surface under fixed models, seeds, requests, and
single-thread policy, then emits compact JSON for E1's baseline decision.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import math
import os
import pathlib
import platform
import resource
import statistics
import sys
import time
from collections.abc import Callable, Sequence
from typing import Any, Final


for _thread_variable in (
    "OMP_NUM_THREADS",
    "OPENBLAS_NUM_THREADS",
    "MKL_NUM_THREADS",
    "VECLIB_MAXIMUM_THREADS",
    "NUMEXPR_NUM_THREADS",
):
    os.environ[_thread_variable] = "1"

import numpy as np  # noqa: E402
from roboplan.cartesian_planning import (  # noqa: E402
    CartesianPathPlanner,
    CartesianPlannerOptions,
    CartesianSpeedMode,
)
from roboplan.core import (  # noqa: E402
    CartesianConfiguration,
    CartesianPath,
    JointConfiguration,
    JointPath,
    Scene,
    hasCollisionsAlongPath,
)
from roboplan.example_models import (  # noqa: E402
    get_package_models_dir,
    get_package_share_dir,
)
from roboplan.rrt import RRT, RRTOptions  # noqa: E402
from roboplan.simple_ik import SimpleIk, SimpleIkOptions  # noqa: E402
from roboplan.toppra import (  # noqa: E402
    PathParameterizerTOPPRA,
    SplineFittingMode,
    TOPPRAOptions,
)


_SCHEMA: Final = "manyforge.e1.roboplan_offline_benchmark.v1"
_GROUP: Final = "arm"
_BASE: Final = "base"
_TIP: Final = "tool0"
_JOINT_NAMES: Final = (
    "shoulder_pan_joint",
    "shoulder_lift_joint",
    "elbow_joint",
    "wrist_1_joint",
    "wrist_2_joint",
    "wrist_3_joint",
)
_RRT_START: Final = np.array(
    [
        2.261477656358459,
        1.9570617913216783,
        -1.156210802275467,
        2.0229644353394325,
        -0.8713978455862086,
        -1.3594747944952112,
    ],
    dtype=np.float64,
)
_RRT_GOAL: Final = np.array(
    [
        2.959756700843902,
        -1.6588986483183334,
        -1.2938684506809712,
        -0.4132159894019849,
        -2.76715775824764,
        0.7740437209968403,
    ],
    dtype=np.float64,
)


def _sha256(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _model_paths() -> tuple[pathlib.Path, pathlib.Path, pathlib.Path, pathlib.Path]:
    models = pathlib.Path(get_package_models_dir())
    return (
        models / "ur_robot_model/ur5_gripper.urdf",
        models / "ur_robot_model/ur5_gripper.srdf",
        pathlib.Path(get_package_share_dir()),
        models / "ur_robot_model/ur5_config.yaml",
    )


def _scene(name: str) -> Scene:
    urdf, srdf, package_share, config = _model_paths()
    return Scene(name, urdf, srdf, [package_share], config)


def _percentiles(values_ns: Sequence[int]) -> dict[str, float]:
    if not values_ns:
        raise ValueError("latency input is empty")
    ordered = sorted(values_ns)

    def pick(fraction: float) -> int:
        return ordered[min(len(ordered) - 1, math.ceil(fraction * len(ordered)) - 1)]

    return {
        "p50_ms": pick(0.50) / 1_000_000.0,
        "p95_ms": pick(0.95) / 1_000_000.0,
        "p99_ms": pick(0.99) / 1_000_000.0,
        "mean_ms": statistics.fmean(ordered) / 1_000_000.0,
        "max_ms": ordered[-1] / 1_000_000.0,
    }


def _measure(
    operation: Callable[[], dict[str, Any]],
    *,
    warmups: int,
    iterations: int,
) -> dict[str, Any]:
    if warmups < 0 or iterations <= 0:
        raise ValueError("benchmark counts are invalid")
    for _ in range(warmups):
        operation()
    latencies: list[int] = []
    final_details: dict[str, Any] = {}
    for _ in range(iterations):
        started = time.perf_counter_ns()
        details = operation()
        elapsed = time.perf_counter_ns() - started
        if elapsed < 0:
            raise RuntimeError("monotonic clock moved backwards")
        latencies.append(elapsed)
        final_details = details
    return {
        "warmups": warmups,
        "iterations": iterations,
        "latency": _percentiles(latencies),
        "throughput_hz": iterations / (sum(latencies) / 1_000_000_000.0),
        "final_details": final_details,
    }


def _memory() -> dict[str, int]:
    rss_bytes = 0
    for row in (
        pathlib.Path("/proc/self/status").read_text(encoding="utf-8").splitlines()
    ):
        if row.startswith("VmRSS:"):
            fields = row.split()
            if len(fields) != 3 or fields[2] != "kB":
                raise RuntimeError("invalid process RSS")
            rss_bytes = int(fields[1]) * 1024
            break
    if rss_bytes <= 0:
        raise RuntimeError("process RSS unavailable")
    return {
        "rss_bytes": rss_bytes,
        "peak_rss_bytes": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss * 1024,
    }


def _scene_construction_workload() -> dict[str, Any]:
    scene = _scene("e1_scene_construction")
    q = scene.getCurrentJointPositions()
    return {
        "joint_count": len(scene.getJointNames()),
        "collision_at_default": bool(scene.hasCollisions(q)),
    }


def _collision_workload(scene: Scene, index: list[int]) -> dict[str, Any]:
    queries = (
        np.array([0.0, -1.57, 0.0, 0.0, 0.0, 0.0], dtype=np.float64),
        np.array([0.0, -1.57, 3.0, 0.0, 0.0, 0.0], dtype=np.float64),
    )
    selected = index[0] % len(queries)
    index[0] += 1
    collision = bool(scene.hasCollisions(queries[selected]))
    expected = selected == 1
    if collision != expected:
        raise RuntimeError("collision decision changed for the fixed query")
    return {"query": selected, "collision": collision}


def _rrt_workload(scene: Scene) -> dict[str, Any]:
    options = RRTOptions()
    options.group_name = _GROUP
    options.max_nodes = 1000
    options.max_connection_distance = 1.0
    options.collision_check_step_size = 0.05
    options.max_planning_time = 0.0
    options.rrt_connect = False
    options.rrt_star = False
    options.fast_return = True
    planner = RRT(scene, options)
    planner.setRngSeed(1234)
    start = JointConfiguration(_JOINT_NAMES, _RRT_START)
    goal = JointConfiguration(_JOINT_NAMES, _RRT_GOAL)
    path = planner.plan(start, goal)
    if path is None or len(path.positions) < 2:
        raise RuntimeError("fixed RRT request did not produce a path")
    if not np.allclose(path.positions[0], _RRT_START, atol=1e-12):
        raise RuntimeError("RRT path changed its start endpoint")
    if not np.allclose(path.positions[-1], _RRT_GOAL, atol=1e-12):
        raise RuntimeError("RRT path changed its goal endpoint")
    for first, second in zip(path.positions, path.positions[1:]):
        if hasCollisionsAlongPath(scene, first, second, 0.05):
            raise RuntimeError("RRT returned a colliding edge")
    return {"waypoints": len(path.positions)}


def _ik_workload(scene: Scene) -> dict[str, Any]:
    target_q = scene.toFullJointPositions(
        _GROUP,
        np.array([0.0, -1.0, 1.0, -1.5, -1.5, 0.0], dtype=np.float64),
    )
    target = CartesianConfiguration(
        _BASE,
        _TIP,
        scene.forwardKinematics(target_q, _TIP, _BASE),
    )
    start = JointConfiguration(_JOINT_NAMES, np.zeros(6, dtype=np.float64))
    solution = JointConfiguration()
    options = SimpleIkOptions()
    options.group_name = _GROUP
    options.max_time = 1.0
    options.max_restarts = 0
    options.check_collisions = False
    scene.setRngSeed(286)
    planner = SimpleIk(scene, options)
    if not planner.solveIk(target, start, solution):
        raise RuntimeError("fixed IK request did not converge")
    q_solution = scene.toFullJointPositions(_GROUP, solution.positions)
    achieved = scene.forwardKinematics(q_solution, _TIP, _BASE)
    position_error = float(np.linalg.norm(achieved[:3, 3] - target.tform[:3, 3]))
    rotation_error = float(np.linalg.norm(achieved[:3, :3] - target.tform[:3, :3]))
    if position_error > 0.01 or rotation_error > 0.05:
        raise RuntimeError("IK result violates the invariant tolerance")
    return {
        "position_error_m": position_error,
        "rotation_matrix_error": rotation_error,
    }


def _toppra_path() -> JointPath:
    path = JointPath()
    path.joint_names = list(_JOINT_NAMES)
    path.positions = [
        np.array([-1.81545, -2.96566, 1.05139, -1.67655, -1.78886, 1.06137]),
        np.array([-1.81565, -2.96506, 1.05111, -1.6767, -1.78838, 1.06168]),
        np.array([-3.02021, 0.604097, -0.607038, -2.56699, 1.04115, 2.91201]),
    ]
    return path


def _toppra_workload(scene: Scene, path: JointPath) -> dict[str, Any]:
    options = TOPPRAOptions()
    options.dt = 0.01
    options.mode = SplineFittingMode.Hermite
    options.velocity_scale = 1.0
    options.acceleration_scale = 1.0
    parameterizer = PathParameterizerTOPPRA(scene, _GROUP)
    trajectory = parameterizer.generate(path, options)
    if trajectory is None or len(trajectory.positions) < 2:
        raise RuntimeError("fixed TOPPRA request did not produce a trajectory")
    times = np.asarray(trajectory.times, dtype=np.float64)
    if not np.isfinite(times).all() or not np.all(np.diff(times) > 0.0):
        raise RuntimeError("TOPPRA timing is not finite and strictly monotonic")
    if not np.allclose(trajectory.positions[0], path.positions[0], atol=1e-8):
        raise RuntimeError("TOPPRA changed the start endpoint")
    if not np.allclose(trajectory.positions[-1], path.positions[-1], atol=1e-8):
        raise RuntimeError("TOPPRA changed the goal endpoint")
    return {
        "samples": len(trajectory.positions),
        "duration_s": float(times[-1]),
    }


def _cartesian_workload(scene: Scene) -> dict[str, Any]:
    q_start_values = np.array(
        [0.0, -1.5708, 1.5708, -1.5708, -1.5708, 0.0], dtype=np.float64
    )
    scene.setJointPositions(q_start_values)
    q_start = JointConfiguration(_JOINT_NAMES, q_start_values)
    start_pose = scene.forwardKinematics(q_start_values, _TIP, _BASE)
    waypoints = []
    for index in range(3):
        pose = np.array(start_pose, dtype=np.float64, order="F", copy=True)
        pose[0, 3] += index * 0.025
        waypoints.append(pose)
    path = CartesianPath([_BASE], [_TIP], [waypoints])
    options = CartesianPlannerOptions()
    options.group_name = _GROUP
    options.dt = 0.01
    options.speed_mode = CartesianSpeedMode.TimeOptimal
    options.max_position_error = 0.01
    options.max_orientation_error = 0.05
    planner = CartesianPathPlanner(scene, options)
    trajectory = planner.plan(path, q_start)
    if trajectory is None or len(trajectory.positions) < 2:
        raise RuntimeError("fixed Cartesian request did not produce a trajectory")
    times = np.asarray(trajectory.times, dtype=np.float64)
    if not np.isfinite(times).all() or not np.all(np.diff(times) > 0.0):
        raise RuntimeError("Cartesian timing is not finite and strictly monotonic")
    final_q = scene.toFullJointPositions(_GROUP, trajectory.positions[-1])
    achieved = scene.forwardKinematics(final_q, _TIP, _BASE)
    position_error = float(np.linalg.norm(achieved[:3, 3] - waypoints[-1][:3, 3]))
    if position_error > options.max_position_error + 1e-6:
        raise RuntimeError("Cartesian result violates the position tolerance")
    velocity_ratio, acceleration_ratio = planner.compute_peak_limit_ratios(trajectory)
    if not all(
        math.isfinite(value) and value >= 0.0
        for value in (
            velocity_ratio,
            acceleration_ratio,
        )
    ):
        raise RuntimeError("Cartesian limit ratios are invalid")
    return {
        "samples": len(trajectory.positions),
        "duration_s": float(times[-1]),
        "position_error_m": position_error,
        "peak_velocity_ratio": float(velocity_ratio),
        "peak_acceleration_ratio": float(acceleration_ratio),
    }


def run(
    label: str,
    source_commit: str,
    source_tree: str,
    patch_set_sha256: str,
    dependency_closure_sha256: str,
    build_type: str,
    compiler_id: str,
) -> dict[str, Any]:
    identities = (
        label,
        source_commit,
        source_tree,
        patch_set_sha256,
        dependency_closure_sha256,
        build_type,
        compiler_id,
    )
    if not all(identities):
        raise ValueError("complete label/source/build identity is required")
    module_paths = {
        "core": pathlib.Path(sys.modules[Scene.__module__].__file__).resolve(),
        "rrt": pathlib.Path(sys.modules[RRT.__module__].__file__).resolve(),
        "simple_ik": pathlib.Path(sys.modules[SimpleIk.__module__].__file__).resolve(),
        "toppra": pathlib.Path(
            sys.modules[PathParameterizerTOPPRA.__module__].__file__
        ).resolve(),
        "cartesian": pathlib.Path(
            sys.modules[CartesianPathPlanner.__module__].__file__
        ).resolve(),
    }
    native_modules = {
        name: {"path": str(path), "sha256": _sha256(path)}
        for name, path in module_paths.items()
    }
    build_identity_payload = {
        "source_commit": source_commit,
        "source_tree": source_tree,
        "patch_set_sha256": patch_set_sha256,
        "dependency_closure_sha256": dependency_closure_sha256,
        "build_type": build_type,
        "compiler_id": compiler_id,
        "native_module_sha256": {
            name: identity["sha256"] for name, identity in native_modules.items()
        },
    }
    build_identity_sha256 = hashlib.sha256(
        json.dumps(
            build_identity_payload,
            sort_keys=True,
            separators=(",", ":"),
            allow_nan=False,
        ).encode("utf-8")
    ).hexdigest()
    first_started = time.perf_counter_ns()
    shared_scene = _scene(f"e1_{label}")
    first_scene_ms = (time.perf_counter_ns() - first_started) / 1_000_000.0
    collision_index = [0]
    toppra_path = _toppra_path()

    result = {
        "schema": _SCHEMA,
        "label": label,
        "source_identity": {
            "commit": source_commit,
            "tree": source_tree,
            "patch_set_sha256": patch_set_sha256,
            "dependency_closure_sha256": dependency_closure_sha256,
            "build_identity_sha256": build_identity_sha256,
            "build_type": build_type,
            "compiler_id": compiler_id,
        },
        "runtime_identity": {
            "architecture": platform.machine(),
            "python": sys.version,
            "executable": sys.executable,
            "cpu_affinity": sorted(os.sched_getaffinity(0)),
        },
        "python": sys.executable,
        "native_modules": native_modules,
        "thread_policy": {
            name: os.environ.get(name)
            for name in (
                "OMP_NUM_THREADS",
                "OPENBLAS_NUM_THREADS",
                "MKL_NUM_THREADS",
                "VECLIB_MAXIMUM_THREADS",
                "NUMEXPR_NUM_THREADS",
            )
        },
        "first_scene_ms": first_scene_ms,
        "memory_before": _memory(),
        "workloads": {
            "scene_construction": _measure(
                _scene_construction_workload, warmups=1, iterations=5
            ),
            "collision_query": _measure(
                lambda: _collision_workload(shared_scene, collision_index),
                warmups=200,
                iterations=2000,
            ),
            "rrt_joint": _measure(
                lambda: _rrt_workload(shared_scene), warmups=2, iterations=15
            ),
            "simple_ik_pose": _measure(
                lambda: _ik_workload(shared_scene), warmups=2, iterations=15
            ),
            "toppra_retime": _measure(
                lambda: _toppra_workload(shared_scene, toppra_path),
                warmups=2,
                iterations=15,
            ),
            "cartesian_line": _measure(
                lambda: _cartesian_workload(shared_scene), warmups=1, iterations=5
            ),
        },
        "memory_after": _memory(),
    }
    return result


def _parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--label", required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--source-tree", required=True)
    parser.add_argument("--patch-set-sha256", required=True)
    parser.add_argument("--dependency-closure-sha256", required=True)
    parser.add_argument("--build-type", required=True)
    parser.add_argument("--compiler-id", required=True)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = _parse_args(sys.argv[1:] if argv is None else argv)
    json.dump(
        run(
            args.label,
            args.source_commit,
            args.source_tree,
            args.patch_set_sha256,
            args.dependency_closure_sha256,
            args.build_type,
            args.compiler_id,
        ),
        sys.stdout,
        sort_keys=True,
        separators=(",", ":"),
        allow_nan=False,
    )
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

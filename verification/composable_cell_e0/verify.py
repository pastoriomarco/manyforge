from __future__ import annotations

import argparse
import asyncio
import gc
import importlib
import json
import resource
import statistics
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def exercise(module_name: str, source_root: Path) -> dict[str, object]:
    module = importlib.import_module(module_name)

    cell = module.CellId("cell-a")
    assert cell == module.CellId("cell-a")
    assert cell != module.CellId("cell-b")
    assert hash(cell) == hash(module.CellId("cell-a"))

    state_transition = module.KernelEventType.StateTransition
    assert state_transition == module.KernelEventType.StateTransition
    assert state_transition != module.KernelEventType.ExecuteStarted
    assert hash(state_transition) == hash(module.KernelEventType.StateTransition)

    config = module.OptionalConfig("runtime", None)
    assert config.name == "runtime" and config.note is None

    snapshot = module.make_snapshot(1024)
    assert snapshot.sequence == 7 and len(snapshot.values) == 1024

    parsed = module.parse_yaml("z: 1\na: [true, null]\n")
    assert parsed.canonical_json == '{"a":[true,null],"z":1}'
    try:
        module.parse_yaml("duplicate: 1\nduplicate: 2\n")
    except module.ArtifactCodecError:
        pass
    else:
        raise AssertionError("structured parse exception was not mapped")

    try:
        module.ParsedArtifactCandidate()
    except TypeError:
        pass
    else:
        raise AssertionError("trusted parser stage became constructible")

    qualified = module.qualify_empty_cell()
    assert qualified["cell_id"] == "e0_empty"
    assert qualified["graph_revision"].startswith("sha256:")
    assert qualified["model_instance_count"] == 0
    assert qualified["component_count"] == 0

    descriptor = (
        source_root
        / "manyforge_deployment"
        / "resources"
        / "closed_schema"
        / "examples"
        / "manyforge.closed_schema.example_inventory.v1.yaml"
    ).read_text(encoding="utf-8")
    compiled = module.compile_closed_schema(descriptor)
    assert compiled["schema_id"] == "manyforge.closed_schema.example_inventory.v1"
    assert compiled["schema_revision"].startswith("sha256:")

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=4) as pool:
        compile_futures = [
            pool.submit(module.compile_schema_revision_repeated, descriptor, 24)
            for _ in range(4)
        ]
        compile_revisions = [future.result() for future in compile_futures]
    concurrent_compile_ms = (time.perf_counter() - started) * 1000.0
    assert all(
        revision == compiled["schema_revision"]
        for revision in compile_revisions
    )

    execution_snapshot = module.materialize_execution_snapshot()
    assert execution_snapshot == {
        "timestamp_ns": 123456,
        "sequence_number": 17,
        "execution_id": 42,
        "move_id": "move-6",
        "state": "Executing",
        "progress": 0.625,
        "joint_positions_rad": [0.1, -0.2, 0.3],
        "joint_velocities_rad_s": [0.01, -0.02, 0.03],
        "state_source": "e0-proof",
        "source_timestamp_ns": 123000,
        "source_age_ns": 456,
        "contact_policy_id": "contact-policy-1",
    }

    event = module.materialize_kernel_event()
    assert event == {
        "type": "state_transition",
        "execution_id": 42,
        "timestamp_ns": 123456,
        "sequence": 9,
        "from_state": "Idle",
        "to_state": "Executing",
        "details": {"program_run_id": "run-7", "source": "e0-proof"},
        "program_run_id": "run-7",
    }

    pose = {
        "position_m": {"x_m": 0.25, "y_m": -0.5, "z_m": 1.75},
        "orientation": {"x": 0.0, "y": 0.0, "z": 0.0, "w": 1.0},
    }
    assert module.pose_round_trip(pose) == pose

    parent = module.Parent("retained")
    child = parent.child
    del parent
    gc.collect()
    assert child.value == "retained"

    token = module.make_shared_token(11)
    assert token.value == 11 and module.SharedToken.live() == 1
    del token
    gc.collect()
    assert module.SharedToken.live() == 0

    started = time.perf_counter()
    with ThreadPoolExecutor(max_workers=4) as pool:
        futures = [pool.submit(module.bounded_wait, 80) for _ in range(4)]
        for future in futures:
            future.result()
    concurrent_wait_ms = (time.perf_counter() - started) * 1000.0
    assert concurrent_wait_ms < 230.0

    iterations = 20_000
    started = time.perf_counter_ns()
    for index in range(iterations):
        module.CellId(f"cell-{index % 32}")
    strong_id_ns_per = (time.perf_counter_ns() - started) / iterations

    started = time.perf_counter_ns()
    for _ in range(2_000):
        module.make_snapshot(64)
    snapshot_ns_per = (time.perf_counter_ns() - started) / 2_000

    started = time.perf_counter_ns()
    for _ in range(2_000):
        module.parse_yaml("a: 1\nb: [2, 3]\n")
    parse_ns_per = (time.perf_counter_ns() - started) / 2_000

    started = time.perf_counter_ns()
    for _ in range(500):
        module.qualify_empty_cell()
    qualify_ns_per = (time.perf_counter_ns() - started) / 500

    started = time.perf_counter_ns()
    for _ in range(100):
        module.compile_closed_schema(descriptor)
    compile_ns_per = (time.perf_counter_ns() - started) / 100

    started = time.perf_counter_ns()
    for _ in range(2_000):
        module.materialize_execution_snapshot()
    execution_snapshot_ns_per = (time.perf_counter_ns() - started) / 2_000

    started = time.perf_counter_ns()
    for _ in range(500):
        module.materialize_kernel_event()
    event_ns_per = (time.perf_counter_ns() - started) / 500

    async def cancel_observation_only() -> None:
        start_before = module.recorded_wait_start_count()
        completion_before = module.recorded_wait_completion_count()
        task = asyncio.create_task(
            asyncio.to_thread(module.bounded_wait_recorded, 80)
        )
        for _ in range(100):
            if module.recorded_wait_start_count() == start_before + 1:
                break
            await asyncio.sleep(0.01)
        assert module.recorded_wait_start_count() == start_before + 1
        task.cancel()
        try:
            await task
        except asyncio.CancelledError:
            pass
        for _ in range(100):
            if (
                module.recorded_wait_completion_count()
                == completion_before + 1
            ):
                break
            await asyncio.sleep(0.01)
        assert (
            module.recorded_wait_completion_count()
            == completion_before + 1
        )

    asyncio.run(cancel_observation_only())

    return {
        "module": module_name,
        "strong_id_ns_per": strong_id_ns_per,
        "snapshot64_ns_per": snapshot_ns_per,
        "parse_ns_per": parse_ns_per,
        "qualify_ns_per": qualify_ns_per,
        "compile_ns_per": compile_ns_per,
        "execution_snapshot_ns_per": execution_snapshot_ns_per,
        "event_ns_per": event_ns_per,
        "concurrent_compile_ms": concurrent_compile_ms,
        "concurrent_wait_ms": concurrent_wait_ms,
        "process_maxrss_kib_after_exercise_cumulative": resource.getrusage(
            resource.RUSAGE_SELF
        ).ru_maxrss,
    }


def import_probe(module_name: str) -> dict[str, object]:
    command = [
        sys.executable,
        "-c",
        (
            "import importlib,resource,time,json;"
            "s=time.perf_counter_ns();importlib.import_module(%r);"
            "print(json.dumps({'import_ns':time.perf_counter_ns()-s,"
            "'maxrss_kib':resource.getrusage(resource.RUSAGE_SELF).ru_maxrss}))"
        )
        % module_name,
    ]
    samples = [
        json.loads(subprocess.check_output(command, text=True)) for _ in range(12)
    ]
    return {
        "module": module_name,
        "import_ns_median": statistics.median(
            v["import_ns"] for v in samples
        ),
        "maxrss_kib_median": statistics.median(
            v["maxrss_kib"] for v in samples
        ),
    }


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source-root", type=Path, required=True)
    parser.add_argument("--reverse", action="store_true")
    parser.add_argument(
        "--module",
        action="append",
        choices=("e0_pybind", "e0_nanobind"),
        help="repeat to select modules; default verifies both",
    )
    args = parser.parse_args()

    module_order = args.module or ["e0_pybind", "e0_nanobind"]
    if args.reverse:
        module_order.reverse()
    result = {
        "exercise": [
            exercise(module_name, args.source_root) for module_name in module_order
        ],
        "import": [import_probe(module_name) for module_name in module_order],
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

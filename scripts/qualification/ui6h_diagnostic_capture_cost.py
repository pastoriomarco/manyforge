"""Short producer-side capture cost; no HTTP, controller, hardware or Stop.

Reuses the ordinary Program-owner unit fixture, validated Program declarations
and actual value store/capture methods. Fixture owner ports do not actuate.
"""

import importlib.util
import json
from pathlib import Path
import resource
import time
import tracemalloc

from manyforge_behavior.canonical_artifact import compute_content_identity
from manyforge_behavior.program import Program
from scripts.qualification.ui6h_source_cost import distribution


def main():
    root = Path(__file__).resolve().parents[2]
    spec = importlib.util.spec_from_file_location(
        "owner_fixture", root / "manyforge_behavior/tests/test_program_run_owner.py"
    )
    fixture = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixture)
    owner, admission, *_ = fixture._fixture(initial_task_state=fixture.OwnerTaskState.SUCCEEDED)
    document = admission.program.to_dict()
    document["blackboard"] = {
        "namespaces": [],
        "keys": [
            {
                "key": f"value{index:02d}",
                "type": "string_list",
                "access": "read_write",
                "persistence": "run",
            }
            for index in range(32)
        ],
    }
    document["program_revision"] = compute_content_identity(
        document,
        own_field="program_revision",
        identity_domain="program-revision",
        api_version="manyforge.program.v2",
    )
    admission = fixture._admission_for_program(
        admission, Program.from_dict(document, admission.environment.catalog), []
    )
    run = owner.start(admission)
    entry = owner._runs[run.capture.program_run_id]
    results = {}
    for name, value in {
        "traversal_boundary": ["x"] * 508,
        "oversized_list": ["x"] * 10000,
        "escaped_byte_boundary": ["\x01" * 2048] * 511,
        "large_string": ["x" * 100000],
    }.items():
        for index in range(32):
            entry.run.value_store.write(f"value{index:02d}", value)
        samples = []
        cpu = time.process_time()
        for _ in range(30):
            before = time.perf_counter()
            # Same owner-lock + run/store-lock path as an incident transition.
            with owner._lock:
                owner._capture_diagnostic(entry, "succeeded")
            samples.append((time.perf_counter() - before) * 1000)
        cpu = time.process_time() - cpu
        tracemalloc.start()
        with owner._lock:
            owner._capture_diagnostic(entry, "succeeded")
        _, peak = tracemalloc.get_traced_memory()
        tracemalloc.stop()
        record = owner.take_diagnostic(run.capture.program_run_id)
        assert record["program_binding_id"] == admission.program_binding_id
        assert len(record["blackboard"]) <= 32
        assert len(json.dumps(record).encode()) < 65536
        results[name] = {
            "capture_ms": distribution(samples),
            "cpu_s": cpu,
            "capture_peak_bytes": peak,
            "retained": len(record["blackboard"]),
            "omitted": record["omitted_blackboard_count"],
        }
        assert max(samples) < 100, results[name]
    print(
        json.dumps(
            {
                "profiles": results,
                "rss_max_kib": resource.getrusage(resource.RUSAGE_SELF).ru_maxrss,
            },
            sort_keys=True,
        )
    )


if __name__ == "__main__":
    main()

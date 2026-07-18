#!/usr/bin/env python3
"""Capture reproducible viewer data-plane and same-host interference evidence.

The runner is observer-only. It samples the dedicated viewer status, kernel
metrics, browser-reported windows, and explicitly named process trees. It does
not create viewer demand, drive motion, or infer controller authority.
"""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import json
import math
import os
from pathlib import Path
import platform
import subprocess
import sys
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen

REPO_ROOT = Path(__file__).resolve().parents[1]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.qualification.viewer_qualification_contract import (  # noqa: E402
    ViewerQualificationArtifactV1,
    ViewerQualificationMatrixV1,
    ViewerQualificationSampleV1,
    ViewerQualificationScenarioV1,
)


_CLOCK_TICKS = os.sysconf("SC_CLK_TCK")
_PAGE_SIZE = os.sysconf("SC_PAGE_SIZE")
_DEFAULT_MATRIX_PATH = (
    REPO_ROOT / "scripts" / "qualification" / "viewer_qualification_matrix.v1.json"
)


def _parse_assignment(value: str, *, value_type: type = str) -> tuple[str, Any]:
    label, separator, raw = value.partition("=")
    if not separator or not label.strip() or not raw.strip():
        raise argparse.ArgumentTypeError("expected LABEL=VALUE")
    try:
        parsed = value_type(raw)
    except (TypeError, ValueError) as exc:
        raise argparse.ArgumentTypeError(f"invalid {value_type.__name__}: {raw}") from exc
    return label.strip(), parsed


def _get_json(
    base_url: str,
    path: str,
    *,
    timeout_s: float,
    token: str | None,
) -> tuple[dict[str, Any], float]:
    url = urljoin(base_url.rstrip("/") + "/", path.lstrip("/"))
    headers = {"Accept": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    started = time.perf_counter()
    with urlopen(Request(url, headers=headers), timeout=timeout_s) as response:
        payload = json.loads(response.read().decode("utf-8"))
    latency_ms = (time.perf_counter() - started) * 1000.0
    if not isinstance(payload, dict):
        raise ValueError(f"{path} did not return an object")
    return payload, latency_ms


def _legacy_scene_status(payload: dict[str, Any]) -> dict[str, Any]:
    """Reduce a v0 full-scene response to honest characterization fields.

    The pre-cut source has no lease/fanout counters. Synthesizing those would
    make the artifact look comparable when it is not, so the normalized record
    explicitly marks client counts unknown and retains only diagnostics the old
    route actually reported.
    """
    diagnostics = payload.get("visual_state_diagnostics")
    if not isinstance(diagnostics, dict):
        diagnostics = payload.get("visualStateDiagnostics")
    if not isinstance(diagnostics, dict):
        diagnostics = {}

    def field(snake: str, camel: str) -> Any:
        return diagnostics[snake] if snake in diagnostics else diagnostics.get(camel)

    return {
        "schema_version": "manyforge.viewer.legacy_scene_status.v0",
        "legacy_characterization": True,
        "client_count": 0,
        "active_client_count": 0,
        "client_count_known": False,
        "snapshot_sequence": field("snapshot_sequence_number", "snapshotSequenceNumber"),
        "snapshot_build_ms": field("snapshot_build_ms", "snapshotBuildMs"),
        "snapshot_age_ms": field("snapshot_age_ms", "snapshotAgeMs"),
        "publisher_running": field(
            "snapshot_publisher_running", "snapshotPublisherRunning"
        ),
        "mode": diagnostics.get("mode"),
        "fallback_reason": field("fallback_reason", "fallbackReason"),
    }


def _read_stat(pid: int) -> tuple[int, float, int] | None:
    try:
        raw = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8")
        closing = raw.rfind(")")
        fields = raw[closing + 2 :].split()
        parent_pid = int(fields[1])
        cpu_seconds = (int(fields[11]) + int(fields[12])) / _CLOCK_TICKS
        rss_bytes = int(fields[21]) * _PAGE_SIZE
        return parent_pid, cpu_seconds, rss_bytes
    except (OSError, ValueError, IndexError):
        return None


def _proc_inventory() -> dict[int, tuple[int, float, int]]:
    inventory: dict[int, tuple[int, float, int]] = {}
    for item in Path("/proc").iterdir():
        if not item.name.isdigit():
            continue
        pid = int(item.name)
        stat = _read_stat(pid)
        if stat is not None:
            inventory[pid] = stat
    return inventory


def _descendants(
    root_pid: int,
    inventory: dict[int, tuple[int, float, int]],
) -> set[int]:
    selected = {root_pid}
    changed = True
    while changed:
        changed = False
        for pid, (parent_pid, _cpu, _rss) in inventory.items():
            if pid not in selected and parent_pid in selected:
                selected.add(pid)
                changed = True
    return selected


def _thread_count(pid: int) -> int:
    try:
        return sum(1 for item in Path(f"/proc/{pid}/task").iterdir() if item.name.isdigit())
    except OSError:
        return 0


def _cpu_affinity(pid: int) -> str | None:
    try:
        for line in Path(f"/proc/{pid}/status").read_text(encoding="utf-8").splitlines():
            if line.startswith("Cpus_allowed_list:"):
                return line.split(":", 1)[1].strip()
    except OSError:
        return None
    return None


def _sample_process_trees(
    roots: dict[str, int],
    previous: dict[str, tuple[int, int, float]],
    *,
    now_ns: int,
) -> dict[str, dict[str, Any]]:
    inventory = _proc_inventory()
    process_sets = {
        label: (_descendants(root_pid, inventory) if root_pid > 0 else set())
        for label, root_pid in roots.items()
    }
    # Named nested roots are separate ownership/resource domains. Exclude each
    # explicitly sampled child tree from its named ancestors so Composer and
    # its isolated visual worker are not double-counted. Unnamed descendants
    # (for example multiprocessing's resource tracker) remain with the parent.
    for label, root_pid in roots.items():
        selected = process_sets[label]
        if not selected:
            continue
        for other_label, other_root_pid in roots.items():
            if other_label == label or other_root_pid <= 0 or other_root_pid == root_pid:
                continue
            if other_root_pid in selected:
                selected.difference_update(process_sets[other_label])
    output: dict[str, dict[str, Any]] = {}
    for label, root_pid in roots.items():
        pids = process_sets[label]
        existing = [inventory[pid] for pid in pids if pid in inventory]
        cpu_seconds = sum(item[1] for item in existing)
        rss_bytes = sum(item[2] for item in existing)
        prior = previous.get(label)
        cpu_percent = None
        if prior is not None and prior[0] == root_pid and now_ns > prior[1]:
            cpu_percent = max(
                0.0,
                (cpu_seconds - prior[2]) / ((now_ns - prior[1]) / 1e9) * 100.0,
            )
        previous[label] = (root_pid, now_ns, cpu_seconds)
        output[label] = {
            "root_pid": root_pid,
            "process_count": len(existing),
            "thread_count": sum(_thread_count(pid) for pid in pids),
            "cpu_seconds": cpu_seconds,
            "cpu_percent": cpu_percent,
            "rss_bytes": rss_bytes,
            "root_cpu_affinity": _cpu_affinity(root_pid),
        }
    return output


def _percentile(values: list[float], percentile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, min(len(ordered) - 1, math.ceil(len(ordered) * percentile) - 1))
    return ordered[index]


def _numeric_counter_delta(
    first: dict[str, Any],
    last: dict[str, Any],
) -> dict[str, float]:
    delta: dict[str, float] = {}
    for key, end in last.items():
        start = first.get(key)
        if (
            isinstance(start, (int, float))
            and not isinstance(start, bool)
            and isinstance(end, (int, float))
            and not isinstance(end, bool)
        ):
            delta[key] = float(end) - float(start)
    return delta


def _flatten_numeric_fields(
    value: dict[str, Any],
    *,
    prefix: str = "",
) -> dict[str, float]:
    """Flatten numeric observer leaves into stable dotted paths."""
    flattened: dict[str, float] = {}
    for key, item in value.items():
        name = f"{prefix}.{key}" if prefix else str(key)
        if isinstance(item, dict):
            flattened.update(_flatten_numeric_fields(item, prefix=name))
        elif isinstance(item, (int, float)) and not isinstance(item, bool):
            flattened[name] = float(item)
    return flattened


def _observer_counter_delta(
    first: dict[str, Any],
    last: dict[str, Any],
) -> dict[str, float]:
    first_fields = _flatten_numeric_fields(first)
    last_fields = _flatten_numeric_fields(last)
    return {
        name: value - first_fields[name]
        for name, value in last_fields.items()
        if name in first_fields
    }


def _source_identity() -> dict[str, Any]:
    def git(*args: str) -> str:
        result = subprocess.run(
            ["git", *args],
            cwd=REPO_ROOT,
            check=True,
            capture_output=True,
            text=True,
        )
        return result.stdout.strip()

    return {
        "repository": str(REPO_ROOT),
        "commit": git("rev-parse", "HEAD"),
        "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
        "dirty": bool(git("status", "--porcelain")),
        "diff_stat": git("diff", "--stat"),
    }


def _build_summary(samples: list[ViewerQualificationSampleV1]) -> dict[str, Any]:
    viewer_latencies = [sample.viewer_route_latency_ms for sample in samples]
    kernel_latencies = [sample.kernel_route_latency_ms for sample in samples]
    process_labels = sorted({label for sample in samples for label in sample.processes})
    processes: dict[str, Any] = {}
    for label in process_labels:
        cpu_values = [
            float(value)
            for sample in samples
            if (value := sample.processes.get(label, {}).get("cpu_percent")) is not None
        ]
        rss_values = [
            int(sample.processes[label]["rss_bytes"])
            for sample in samples
            if label in sample.processes
        ]
        processes[label] = {
            "cpu_percent": {
                "p50": _percentile(cpu_values, 0.50),
                "p95": _percentile(cpu_values, 0.95),
                "p99": _percentile(cpu_values, 0.99),
                "max": max(cpu_values, default=None),
            },
            "rss_bytes": {
                "max": max(rss_values, default=None),
                "last": rss_values[-1] if rss_values else None,
            },
        }
    first_viewer = samples[0].viewer_status if samples else {}
    last_viewer = samples[-1].viewer_status if samples else {}
    viewer_counter_delta = _numeric_counter_delta(first_viewer, last_viewer)
    first_kernel = samples[0].kernel_metrics if samples else {}
    last_kernel = samples[-1].kernel_metrics if samples else {}
    kernel_state_samples: dict[str, int] = {}
    for sample in samples:
        state = sample.kernel_snapshot.get("state")
        if isinstance(state, str) and state:
            kernel_state_samples[state] = kernel_state_samples.get(state, 0) + 1
    observer_labels = sorted(
        {label for sample in samples for label in sample.observers}
    )
    observer_counter_deltas = {
        label: _observer_counter_delta(
            next(
                (
                    sample.observers[label]
                    for sample in samples
                    if label in sample.observers
                ),
                {},
            ),
            next(
                (
                    sample.observers[label]
                    for sample in reversed(samples)
                    if label in sample.observers
                ),
                {},
            ),
        )
        for label in observer_labels
    }
    capture_start_ns = samples[0].monotonic_ns if samples else 0
    client_windows: dict[tuple[str, int], dict[str, Any]] = {}
    for sample in samples:
        clients = sample.viewer_status.get("fanout", {}).get("clients", {})
        if not isinstance(clients, dict):
            continue
        for client_id, status in clients.items():
            if not isinstance(status, dict):
                continue
            received_at = status.get("client_diagnostics_received_at_ns")
            diagnostics = status.get("client_diagnostics")
            if isinstance(received_at, int) and isinstance(diagnostics, dict):
                duration_ms = diagnostics.get("window_duration_ms")
                if isinstance(duration_ms, (int, float)) and not isinstance(
                    duration_ms, bool
                ):
                    window_start_ns = received_at - int(float(duration_ms) * 1e6)
                    # A diagnostics window that straddles warmup can contain the
                    # initial manifest/materialization work. Count only windows
                    # wholly inside the measured interval so warmup is real.
                    if window_start_ns < capture_start_ns:
                        continue
                client_windows[(str(client_id), received_at)] = diagnostics
    client_totals: dict[str, float] = {}
    client_maxima: dict[str, float] = {}
    for window in client_windows.values():
        for key, value in window.items():
            if isinstance(value, (int, float)) and not isinstance(value, bool):
                client_totals[key] = client_totals.get(key, 0.0) + float(value)
                client_maxima[key] = max(client_maxima.get(key, float("-inf")), float(value))
    frame_messages = client_totals.get("frame_messages", 0.0)
    react_commits = client_totals.get("react_commits", 0.0)
    topology_rebuilds = int(client_totals.get("topology_rebuilds", 0.0))
    same_producer_epoch = (
        bool(samples)
        and first_viewer.get("producer_epoch") is not None
        and first_viewer.get("producer_epoch") == last_viewer.get("producer_epoch")
    )
    manifest_changes = (
        max(0, int(viewer_counter_delta.get("manifest_revision", 0.0)))
        if same_producer_epoch
        else 0
    )
    max_clients = max(
        (
            int(sample.viewer_status.get("client_count", 0))
            for sample in samples
        ),
        default=0,
    )
    expected_topology_rebuilds = manifest_changes * max_clients
    producer_stage_names = sorted(
        {
            str(name)
            for sample in samples
            for name in sample.viewer_status.get("last_stage_durations_ms", {})
        }
    )
    producer_stages: dict[str, dict[str, float | None]] = {}
    for name in producer_stage_names:
        values = [
            float(value)
            for sample in samples
            if isinstance(
                value := sample.viewer_status.get("last_stage_durations_ms", {}).get(name),
                (int, float),
            )
            and not isinstance(value, bool)
        ]
        producer_stages[name] = {
            "p50": _percentile(values, 0.50),
            "p95": _percentile(values, 0.95),
            "p99": _percentile(values, 0.99),
            "max": max(values, default=None),
        }
    return {
        "sample_count": len(samples),
        "viewer_route_latency_ms": {
            "p50": _percentile(viewer_latencies, 0.50),
            "p95": _percentile(viewer_latencies, 0.95),
            "p99": _percentile(viewer_latencies, 0.99),
            "max": max(viewer_latencies, default=None),
        },
        "kernel_route_latency_ms": {
            "p95": _percentile(kernel_latencies, 0.95),
            "max": max(kernel_latencies, default=None),
        },
        "viewer_counter_delta": viewer_counter_delta,
        "kernel_counter_delta": _numeric_counter_delta(first_kernel, last_kernel),
        "kernel_state_samples": kernel_state_samples,
        "observer_counter_delta": observer_counter_deltas,
        "last_viewer_status": last_viewer,
        "last_kernel_metrics": last_kernel,
        "client_reported_windows": list(client_windows.values()),
        "client_totals": client_totals,
        "client_maxima": client_maxima,
        "client_react_commits_per_frame": (
            react_commits / frame_messages
            if frame_messages > 0.0
            else 0.0
            if client_windows and react_commits == 0.0
            else None
        ),
        "client_unmatched_topology_rebuilds": max(
            0,
            topology_rebuilds - expected_topology_rebuilds,
        ),
        "client_expected_topology_rebuilds": expected_topology_rebuilds,
        "producer_stage_durations_ms": producer_stages,
        "processes": processes,
    }


def _evaluate(
    summary: dict[str, Any],
    *,
    require_active_client: bool,
    require_no_client: bool,
    require_zero_builds: bool,
    client_count_min: int | None,
    client_count_max: int | None,
    active_client_count_min: int | None,
    max_viewer_route_p95_ms: float,
    max_process_cpu: dict[str, float],
    max_client_long_tasks: int | None,
    max_client_materialization_ms: float | None,
    max_client_react_commits_per_frame: float | None,
    max_client_unmatched_topology_rebuilds: int | None,
    max_build_failures: int,
    max_publication_sink_drops: int,
    required_layers: list[str] | None = None,
    minimum_viewer_counter_deltas: dict[str, float] | None = None,
    minimum_kernel_counter_deltas: dict[str, float] | None = None,
    minimum_kernel_state_samples: dict[str, int] | None = None,
    minimum_client_totals: dict[str, float] | None = None,
    minimum_observer_counter_deltas: dict[str, dict[str, float]] | None = None,
) -> list[str]:
    failures: list[str] = []
    last_status = summary.get("last_viewer_status", {})
    active_clients = int(last_status.get("active_client_count", 0))
    client_count = int(last_status.get("client_count", 0))
    if require_active_client and active_clients <= 0:
        failures.append("no active viewer client at the end of the capture")
    if require_no_client and client_count != 0:
        failures.append(f"viewer clients remained attached: {client_count}")
    if client_count_min is not None and client_count < client_count_min:
        failures.append(
            f"viewer client count {client_count} is below required {client_count_min}"
        )
    if client_count_max is not None and client_count > client_count_max:
        failures.append(
            f"viewer client count {client_count} exceeds allowed {client_count_max}"
        )
    if (
        active_client_count_min is not None
        and active_clients < active_client_count_min
    ):
        failures.append(
            "active viewer client count "
            f"{active_clients} is below required {active_client_count_min}"
        )
    observed_layers = {
        str(layer) for layer in last_status.get("layers", [])
    }
    missing_layers = sorted(set(required_layers or []) - observed_layers)
    if missing_layers:
        failures.append("missing required viewer layers: " + ", ".join(missing_layers))
    builds = summary.get("viewer_counter_delta", {}).get("builds_started", 0.0)
    if require_zero_builds and builds != 0.0:
        failures.append(f"viewer builds advanced without demand: {builds:g}")
    for name, minimum in (minimum_viewer_counter_deltas or {}).items():
        observed = summary.get("viewer_counter_delta", {}).get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no viewer counter-delta evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"viewer counter delta {name}={observed:g} is below required {minimum:g}"
            )
    for name, minimum in (minimum_kernel_counter_deltas or {}).items():
        observed = summary.get("kernel_counter_delta", {}).get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no kernel counter-delta evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"kernel counter delta {name}={observed:g} is below required {minimum:g}"
            )
    for state, minimum in (minimum_kernel_state_samples or {}).items():
        observed = int(summary.get("kernel_state_samples", {}).get(state, 0))
        if observed < minimum:
            failures.append(
                f"kernel state {state} sampled {observed} times; required {minimum}"
            )
    for name, minimum in (minimum_client_totals or {}).items():
        observed = summary.get("client_totals", {}).get(name)
        if not isinstance(observed, (int, float)) or isinstance(observed, bool):
            failures.append(f"no client workload evidence for {name}")
        elif float(observed) < minimum:
            failures.append(
                f"client total {name}={observed:g} is below required {minimum:g}"
            )
    for observer, counters in (minimum_observer_counter_deltas or {}).items():
        observed_counters = summary.get("observer_counter_delta", {}).get(
            observer, {}
        )
        for name, minimum in counters.items():
            observed = observed_counters.get(name)
            if not isinstance(observed, (int, float)) or isinstance(observed, bool):
                failures.append(
                    f"no observer counter-delta evidence for {observer}.{name}"
                )
            elif float(observed) < minimum:
                failures.append(
                    "observer counter delta "
                    f"{observer}.{name}={observed:g} is below required {minimum:g}"
                )
    p95 = summary.get("viewer_route_latency_ms", {}).get("p95")
    if p95 is not None and p95 > max_viewer_route_p95_ms:
        failures.append(
            f"viewer status p95 {p95:.3f} ms exceeds {max_viewer_route_p95_ms:.3f} ms"
        )
    for label, budget in max_process_cpu.items():
        observed = summary.get("processes", {}).get(label, {}).get("cpu_percent", {}).get("p95")
        if observed is None:
            failures.append(f"no process CPU evidence for {label}")
        elif observed > budget:
            failures.append(f"{label} CPU p95 {observed:.3f}% exceeds {budget:.3f}%")
    if max_client_long_tasks is not None:
        observed = int(summary.get("client_totals", {}).get("long_tasks", 0))
        if observed > max_client_long_tasks:
            failures.append(
                f"client long tasks {observed} exceed {max_client_long_tasks}"
            )
    if max_client_materialization_ms is not None:
        observed = summary.get("client_maxima", {}).get("materialization_max_ms")
        if observed is None:
            failures.append("no client materialization evidence")
        elif observed > max_client_materialization_ms:
            failures.append(
                "client materialization max "
                f"{observed:.3f} ms exceeds {max_client_materialization_ms:.3f} ms"
            )
    if max_client_react_commits_per_frame is not None:
        observed = summary.get("client_react_commits_per_frame")
        if observed is None:
            failures.append("no client frame/React-commit evidence")
        elif observed > max_client_react_commits_per_frame:
            failures.append(
                "client React commits/frame "
                f"{observed:.6f} exceeds {max_client_react_commits_per_frame:.6f}"
            )
    if max_client_unmatched_topology_rebuilds is not None:
        observed = int(summary.get("client_unmatched_topology_rebuilds", 0))
        if observed > max_client_unmatched_topology_rebuilds:
            failures.append(
                "client topology rebuilds without a matching manifest change "
                f"{observed} exceed {max_client_unmatched_topology_rebuilds}"
            )
    counter_delta = summary.get("viewer_counter_delta", {})
    build_failures = int(counter_delta.get("build_failures", 0))
    if build_failures > max_build_failures:
        failures.append(
            f"viewer build failures {build_failures} exceed {max_build_failures}"
        )
    sink_drops = int(counter_delta.get("publication_sink_drops", 0))
    if sink_drops > max_publication_sink_drops:
        failures.append(
            "viewer publication sink drops "
            f"{sink_drops} exceed {max_publication_sink_drops}"
        )
    return failures


def _load_profile(path: Path, name: str) -> ViewerQualificationScenarioV1:
    matrix = ViewerQualificationMatrixV1.model_validate_json(
        path.read_text(encoding="utf-8")
    )
    matches = [scenario for scenario in matrix.scenarios if scenario.name == name]
    if len(matches) != 1:
        known = ", ".join(scenario.name for scenario in matrix.scenarios)
        raise SystemExit(f"unknown viewer qualification profile {name!r}; known: {known}")
    return matches[0]


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scenario")
    parser.add_argument("--profile")
    parser.add_argument("--matrix", default=str(_DEFAULT_MATRIX_PATH))
    parser.add_argument("--acknowledge-preconditions", action="store_true")
    parser.add_argument("--list-profiles", action="store_true")
    parser.add_argument("--artifact")
    parser.add_argument("--base-url", default="http://127.0.0.1:9000")
    parser.add_argument("--subject-source-commit")
    parser.add_argument(
        "--subject-source-state",
        choices=("clean", "dirty", "unknown"),
        default="unknown",
    )
    parser.add_argument("--subject-source-description")
    parser.add_argument(
        "--viewer-status-path",
        default="/api/scene/viewer/status",
        help=(
            "Observer status endpoint. Use /api/scene/state only for an explicit "
            "pre-v1 characterization; its full response is reduced honestly."
        ),
    )
    parser.add_argument("--token")
    parser.add_argument("--duration-s", type=float)
    parser.add_argument("--warmup-s", type=float)
    parser.add_argument("--hz", type=float)
    parser.add_argument("--timeout-s", type=float)
    parser.add_argument(
        "--pid",
        action="append",
        default=[],
        metavar="LABEL=PID",
        help="Aggregate one process root and all descendants; repeatable.",
    )
    parser.add_argument("--require-active-client", action="store_true")
    parser.add_argument("--require-no-client", action="store_true")
    parser.add_argument("--require-zero-builds", action="store_true")
    parser.add_argument("--max-viewer-route-p95-ms", type=float)
    parser.add_argument(
        "--max-process-cpu",
        action="append",
        default=[],
        metavar="LABEL=PERCENT",
    )
    parser.add_argument("--max-client-long-tasks", type=int)
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    matrix_path = Path(args.matrix)
    if args.list_profiles:
        matrix = ViewerQualificationMatrixV1.model_validate_json(
            matrix_path.read_text(encoding="utf-8")
        )
        for scenario in matrix.scenarios:
            print(f"{scenario.name}\t{scenario.description}")
        return 0
    profile = _load_profile(matrix_path, args.profile) if args.profile else None
    if profile is not None and profile.preconditions and not args.acknowledge_preconditions:
        print("Preconditions for this profile:", file=sys.stderr)
        for item in profile.preconditions:
            print(f"  - {item}", file=sys.stderr)
        raise SystemExit("rerun with --acknowledge-preconditions after verifying them")
    if args.scenario is None and profile is None:
        raise SystemExit("--scenario or --profile is required")
    if not args.artifact:
        raise SystemExit("--artifact is required for a capture")
    scenario_name = args.scenario or profile.name
    args.duration_s = args.duration_s if args.duration_s is not None else (
        profile.capture.duration_s if profile is not None else 60.0
    )
    args.warmup_s = args.warmup_s if args.warmup_s is not None else (
        profile.capture.warmup_s if profile is not None else 2.0
    )
    args.hz = args.hz if args.hz is not None else (
        profile.capture.hz if profile is not None else 2.0
    )
    args.timeout_s = args.timeout_s if args.timeout_s is not None else (
        profile.capture.timeout_s if profile is not None else 2.0
    )
    if args.duration_s <= 0.0 or args.hz <= 0.0 or args.warmup_s < 0.0:
        raise SystemExit("duration/hz must be positive and warmup non-negative")
    roots = dict(_parse_assignment(value, value_type=int) for value in args.pid)
    positive_roots = [pid for pid in roots.values() if pid > 0]
    if len(positive_roots) != len(set(positive_roots)):
        raise SystemExit("nonzero --pid roots must be unique across process labels")
    viewer_worker_pid_is_explicit = "viewer_worker" in roots
    max_process_cpu = dict(
        _parse_assignment(value, value_type=float) for value in args.max_process_cpu
    )
    if profile is not None:
        max_process_cpu = {
            **profile.budgets.process_cpu_p95_percent,
            **max_process_cpu,
        }
        # The visual worker is demand-scoped and restartable. In a collector
        # sharing Composer's PID namespace, its PID comes from each viewer
        # status sample so restarts are measured as new process generations.
        # A caller observing host /proc may instead provide the worker's host
        # PID explicitly; a container-namespace PID from the API is not valid
        # in that observer namespace. PID 0 represents the no-worker state.
        if "viewer_worker" in profile.required_process_labels:
            roots.setdefault("viewer_worker", 0)
        missing_processes = sorted(set(profile.required_process_labels) - set(roots))
        if missing_processes:
            raise SystemExit(
                "missing required --pid labels for profile: "
                + ", ".join(missing_processes)
            )
    period_s = 1.0 / args.hz
    observer_paths = dict(profile.observer_paths) if profile is not None else {}
    previous_process_cpu: dict[str, tuple[int, int, float]] = {}
    deadline = time.monotonic() + args.duration_s
    next_sample = time.monotonic() + args.warmup_s
    samples: list[ViewerQualificationSampleV1] = []
    while time.monotonic() < deadline:
        now = time.monotonic()
        if now < next_sample:
            time.sleep(min(0.05, next_sample - now))
            continue
        errors: list[str] = []
        viewer_status: dict[str, Any] = {}
        kernel_metrics: dict[str, Any] = {}
        kernel_snapshot: dict[str, Any] = {}
        observers: dict[str, dict[str, Any]] = {}
        viewer_latency_ms = 0.0
        kernel_latency_ms = 0.0
        try:
            viewer_status, viewer_latency_ms = _get_json(
                args.base_url,
                args.viewer_status_path,
                timeout_s=args.timeout_s,
                token=args.token,
            )
            if args.viewer_status_path == "/api/scene/state":
                viewer_status = _legacy_scene_status(viewer_status)
        except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
            errors.append(f"viewer_status:{type(exc).__name__}:{exc}")
        try:
            kernel_metrics, kernel_latency_ms = _get_json(
                args.base_url,
                "/api/kernel/metrics",
                timeout_s=args.timeout_s,
                token=args.token,
            )
        except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
            errors.append(f"kernel_metrics:{type(exc).__name__}:{exc}")
        try:
            kernel_snapshot, _ = _get_json(
                args.base_url,
                "/api/kernel/snapshot",
                timeout_s=args.timeout_s,
                token=args.token,
            )
        except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
            errors.append(f"kernel_snapshot:{type(exc).__name__}:{exc}")
        for label, path in observer_paths.items():
            try:
                observers[label], _ = _get_json(
                    args.base_url,
                    path,
                    timeout_s=args.timeout_s,
                    token=args.token,
                )
            except (HTTPError, URLError, TimeoutError, OSError, ValueError) as exc:
                errors.append(f"observer:{label}:{type(exc).__name__}:{exc}")
        now_ns = time.monotonic_ns()
        sample_roots = dict(roots)
        if "viewer_worker" in sample_roots and not viewer_worker_pid_is_explicit:
            worker = viewer_status.get("worker", {})
            worker_pid = worker.get("pid") if isinstance(worker, dict) else None
            sample_roots["viewer_worker"] = (
                int(worker_pid)
                if isinstance(worker_pid, int) and worker_pid > 0
                else 0
            )
        samples.append(
            ViewerQualificationSampleV1(
                index=len(samples),
                monotonic_ns=now_ns,
                viewer_route_latency_ms=viewer_latency_ms,
                kernel_route_latency_ms=kernel_latency_ms,
                viewer_status=viewer_status,
                kernel_metrics=kernel_metrics,
                kernel_snapshot=kernel_snapshot,
                observers=observers,
                processes=_sample_process_trees(
                    sample_roots,
                    previous_process_cpu,
                    now_ns=now_ns,
                ),
                errors=errors,
            )
        )
        next_sample += period_s
        if next_sample < time.monotonic() - period_s:
            next_sample = time.monotonic() + period_s
    summary = _build_summary(samples)
    failures = [error for sample in samples for error in sample.errors]
    failures.extend(
        _evaluate(
            summary,
            require_active_client=(
                args.require_active_client
                or bool(profile and profile.requirements.active_client)
            ),
            require_no_client=(
                args.require_no_client or bool(profile and profile.requirements.no_client)
            ),
            require_zero_builds=(
                args.require_zero_builds
                or bool(profile and profile.requirements.zero_builds)
            ),
            client_count_min=(
                profile.requirements.client_count_min if profile else None
            ),
            client_count_max=(
                profile.requirements.client_count_max if profile else None
            ),
            active_client_count_min=(
                profile.requirements.active_client_count_min if profile else None
            ),
            required_layers=(profile.requirements.required_layers if profile else []),
            minimum_viewer_counter_deltas=(
                profile.requirements.viewer_counter_delta_min if profile else {}
            ),
            minimum_kernel_counter_deltas=(
                profile.requirements.kernel_counter_delta_min if profile else {}
            ),
            minimum_kernel_state_samples=(
                profile.requirements.kernel_state_sample_min if profile else {}
            ),
            minimum_client_totals=(
                profile.requirements.client_total_min if profile else {}
            ),
            minimum_observer_counter_deltas=(
                profile.requirements.observer_counter_delta_min if profile else {}
            ),
            max_viewer_route_p95_ms=(
                args.max_viewer_route_p95_ms
                if args.max_viewer_route_p95_ms is not None
                else profile.budgets.viewer_route_p95_ms if profile else 50.0
            ),
            max_process_cpu=max_process_cpu,
            max_client_long_tasks=(
                args.max_client_long_tasks
                if args.max_client_long_tasks is not None
                else profile.budgets.client_long_tasks_max if profile else None
            ),
            max_client_materialization_ms=(
                profile.budgets.client_materialization_max_ms if profile else None
            ),
            max_client_react_commits_per_frame=(
                profile.budgets.client_react_commits_per_frame_max if profile else None
            ),
            max_client_unmatched_topology_rebuilds=(
                profile.budgets.client_unmatched_topology_rebuilds_max
                if profile
                else None
            ),
            max_build_failures=(profile.budgets.build_failures_max if profile else 0),
            max_publication_sink_drops=(
                profile.budgets.publication_sink_drops_max if profile else 0
            ),
        )
    )
    observer_source = _source_identity()
    subject_source = (
        {
            "commit": args.subject_source_commit,
            "worktree_state": args.subject_source_state,
            "description": args.subject_source_description,
        }
        if args.subject_source_commit is not None
        else observer_source
    )
    artifact = ViewerQualificationArtifactV1(
        schema_version="manyforge.viewer.qualification.v1",
        scenario=scenario_name,
        captured_at=datetime.now(timezone.utc).isoformat(),
        source={
            "subject": subject_source,
            "observer": observer_source,
        },
        environment={
            "hostname": platform.node(),
            "platform": platform.platform(),
            "python": platform.python_version(),
            "cpu_count": os.cpu_count(),
        },
        configuration={
            "base_url": args.base_url,
            "viewer_status_path": args.viewer_status_path,
            "observer_paths": observer_paths,
            "profile": profile.model_dump(mode="json") if profile else None,
            "duration_s": args.duration_s,
            "warmup_s": args.warmup_s,
            "hz": args.hz,
            "process_roots": roots,
            "viewer_worker_pid_source": (
                "explicit"
                if viewer_worker_pid_is_explicit
                else "viewer_status_dynamic"
                if "viewer_worker" in roots
                else "not_sampled"
            ),
            "require_active_client": args.require_active_client,
            "require_no_client": args.require_no_client,
            "require_zero_builds": args.require_zero_builds,
            "max_viewer_route_p95_ms": args.max_viewer_route_p95_ms,
            "max_process_cpu": max_process_cpu,
            "max_client_long_tasks": args.max_client_long_tasks,
        },
        samples=samples,
        summary=summary,
        failures=failures,
        success=not failures,
    )
    output = Path(args.artifact)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(
        json.dumps(artifact.model_dump(mode="json"), indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    print(json.dumps({"artifact": str(output), "success": artifact.success, "failures": failures}, indent=2))
    return 0 if artifact.success else 1


if __name__ == "__main__":
    raise SystemExit(main())

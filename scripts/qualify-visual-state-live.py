#!/usr/bin/env python3
"""Live qualification probe for Composer visual-state snapshot routing.

The probe is intentionally read-only. Run it while an operator or another
qualification harness drives a real Composer/Isaac cycle. It samples
`/api/scene/state`, records route diagnostics, and fails if the viewer path
falls back to live planner reads or exceeds latency/staleness budgets.
"""
from __future__ import annotations

import argparse
from collections import Counter
from dataclasses import dataclass
import json
import math
from pathlib import Path
import time
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import urljoin
from urllib.request import Request, urlopen


@dataclass(slots=True)
class Sample:
    index: int
    ok: bool
    latency_ms: float
    mode: str = "error"
    served_from_visual_snapshot: bool = False
    fallback_reason: str | None = None
    total_ms: float | None = None
    snapshot_age_ms: float | None = None
    snapshot_sequence_number: int | None = None
    snapshot_build_ms: float | None = None
    snapshot_stale: bool = False
    snapshot_publisher_running: bool = False
    snapshot_publisher_error: str | None = None
    objects_source: str | None = None
    object_count: int | None = None
    gripper_count: int | None = None
    pose_source_count: int | None = None
    fresh_pose_source_count: int | None = None
    error: str | None = None

    def to_json(self) -> dict[str, Any]:
        return {
            "index": self.index,
            "ok": self.ok,
            "latency_ms": round(self.latency_ms, 3),
            "mode": self.mode,
            "served_from_visual_snapshot": self.served_from_visual_snapshot,
            "fallback_reason": self.fallback_reason,
            "total_ms": self.total_ms,
            "snapshot_age_ms": self.snapshot_age_ms,
            "snapshot_sequence_number": self.snapshot_sequence_number,
            "snapshot_build_ms": self.snapshot_build_ms,
            "snapshot_stale": self.snapshot_stale,
            "snapshot_publisher_running": self.snapshot_publisher_running,
            "snapshot_publisher_error": self.snapshot_publisher_error,
            "objects_source": self.objects_source,
            "object_count": self.object_count,
            "gripper_count": self.gripper_count,
            "pose_source_count": self.pose_source_count,
            "fresh_pose_source_count": self.fresh_pose_source_count,
            "error": self.error,
        }


def _snake_or_camel(mapping: dict[str, Any], snake: str, camel: str, default: Any = None) -> Any:
    if snake in mapping:
        return mapping[snake]
    return mapping.get(camel, default)


def _get_json(base_url: str, path: str, *, timeout_s: float) -> dict[str, Any]:
    url = urljoin(base_url.rstrip("/") + "/", path.lstrip("/"))
    request = Request(url, headers={"Accept": "application/json"})
    with urlopen(request, timeout=timeout_s) as response:
        body = response.read()
    return json.loads(body.decode("utf-8"))


def _fresh_pose_source_count(pose_sources: list[Any]) -> int:
    fresh_count = 0
    for item in pose_sources:
        if not isinstance(item, dict):
            continue
        fresh = item.get("fresh")
        if fresh is None:
            fresh = item.get("is_fresh")
        if fresh is None:
            fresh = item.get("isFresh")
        if bool(fresh):
            fresh_count += 1
    return fresh_count


def _sample_scene_state(
    *,
    base_url: str,
    index: int,
    timeout_s: float,
) -> Sample:
    started = time.perf_counter()
    try:
        payload = _get_json(base_url, "/api/scene/state", timeout_s=timeout_s)
        elapsed_ms = (time.perf_counter() - started) * 1000.0
    except (HTTPError, URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
        elapsed_ms = (time.perf_counter() - started) * 1000.0
        return Sample(
            index=index,
            ok=False,
            latency_ms=elapsed_ms,
            error=f"{exc.__class__.__name__}: {exc}",
        )

    diagnostics = _snake_or_camel(
        payload,
        "visual_state_diagnostics",
        "visualStateDiagnostics",
        {},
    )
    if not isinstance(diagnostics, dict):
        diagnostics = {}
    pose_sources = _snake_or_camel(payload, "pose_sources", "poseSources", [])
    if not isinstance(pose_sources, list):
        pose_sources = []

    return Sample(
        index=index,
        ok=True,
        latency_ms=elapsed_ms,
        mode=str(diagnostics.get("mode") or "unknown"),
        served_from_visual_snapshot=bool(
            _snake_or_camel(
                diagnostics,
                "served_from_visual_snapshot",
                "servedFromVisualSnapshot",
                False,
            )
        ),
        fallback_reason=_snake_or_camel(
            diagnostics,
            "fallback_reason",
            "fallbackReason",
            None,
        ),
        total_ms=_optional_float(diagnostics.get("total_ms")),
        snapshot_age_ms=_optional_float(
            _snake_or_camel(diagnostics, "snapshot_age_ms", "snapshotAgeMs", None)
        ),
        snapshot_sequence_number=_optional_int(
            _snake_or_camel(
                diagnostics,
                "snapshot_sequence_number",
                "snapshotSequenceNumber",
                None,
            )
        ),
        snapshot_build_ms=_optional_float(
            _snake_or_camel(diagnostics, "snapshot_build_ms", "snapshotBuildMs", None)
        ),
        snapshot_stale=bool(
            _snake_or_camel(diagnostics, "snapshot_stale", "snapshotStale", False)
        ),
        snapshot_publisher_running=bool(
            _snake_or_camel(
                diagnostics,
                "snapshot_publisher_running",
                "snapshotPublisherRunning",
                False,
            )
        ),
        snapshot_publisher_error=_snake_or_camel(
            diagnostics,
            "snapshot_publisher_error",
            "snapshotPublisherError",
            None,
        ),
        objects_source=_snake_or_camel(diagnostics, "objects_source", "objectsSource", None),
        object_count=len(payload.get("objects") or []),
        gripper_count=len(payload.get("grippers") or []),
        pose_source_count=len(pose_sources),
        fresh_pose_source_count=_fresh_pose_source_count(pose_sources),
    )


def _optional_float(value: Any) -> float | None:
    if value is None:
        return None
    try:
        parsed = float(value)
    except (TypeError, ValueError):
        return None
    if not math.isfinite(parsed):
        return None
    return parsed


def _optional_int(value: Any) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (TypeError, ValueError):
        return None


def _percentile(values: list[float], percentile: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    if len(ordered) == 1:
        return ordered[0]
    rank = (len(ordered) - 1) * percentile
    lower = math.floor(rank)
    upper = math.ceil(rank)
    if lower == upper:
        return ordered[lower]
    fraction = rank - lower
    return ordered[lower] + (ordered[upper] - ordered[lower]) * fraction


def _build_summary(samples: list[Sample]) -> dict[str, Any]:
    ok_samples = [sample for sample in samples if sample.ok]
    latencies = [sample.latency_ms for sample in ok_samples]
    mode_counts = Counter(sample.mode for sample in ok_samples)
    fallback_counts = Counter(
        sample.fallback_reason or "none" for sample in ok_samples
    )
    publisher_errors = Counter(
        sample.snapshot_publisher_error
        for sample in ok_samples
        if sample.snapshot_publisher_error
    )
    stale_count = sum(1 for sample in ok_samples if sample.snapshot_stale)
    pending_count = sum(1 for sample in ok_samples if sample.mode == "visual_snapshot_pending")
    legacy_count = sum(1 for sample in ok_samples if sample.mode == "legacy_live")
    non_snapshot_count = sum(
        1
        for sample in ok_samples
        if sample.mode not in {"visual_snapshot", "visual_snapshot_pending"}
    )
    return {
        "sample_count": len(samples),
        "ok_count": len(ok_samples),
        "error_count": len(samples) - len(ok_samples),
        "mode_counts": dict(sorted(mode_counts.items())),
        "fallback_counts": dict(sorted(fallback_counts.items())),
        "publisher_errors": dict(sorted(publisher_errors.items())),
        "stale_count": stale_count,
        "pending_count": pending_count,
        "legacy_count": legacy_count,
        "non_snapshot_count": non_snapshot_count,
        "stale_ratio": (stale_count / len(ok_samples)) if ok_samples else None,
        "pending_ratio": (pending_count / len(ok_samples)) if ok_samples else None,
        "latency_ms": {
            "min": min(latencies) if latencies else None,
            "p50": _percentile(latencies, 0.50),
            "p95": _percentile(latencies, 0.95),
            "p99": _percentile(latencies, 0.99),
            "max": max(latencies) if latencies else None,
        },
        "snapshot_age_ms": {
            "p95": _percentile(
                [
                    sample.snapshot_age_ms
                    for sample in ok_samples
                    if sample.snapshot_age_ms is not None
                ],
                0.95,
            ),
            "max": max(
                [
                    sample.snapshot_age_ms
                    for sample in ok_samples
                    if sample.snapshot_age_ms is not None
                ],
                default=None,
            ),
        },
    }


def _evaluate(
    summary: dict[str, Any],
    *,
    max_error_count: int,
    max_legacy_count: int,
    max_non_snapshot_count: int,
    max_p95_ms: float,
    max_stale_ratio: float,
    max_pending_ratio: float,
) -> list[str]:
    failures: list[str] = []
    if summary["ok_count"] <= 0:
        failures.append("no successful /api/scene/state samples")
        return failures
    if summary["error_count"] > max_error_count:
        failures.append(
            f"route errors {summary['error_count']} > budget {max_error_count}"
        )
    if summary["legacy_count"] > max_legacy_count:
        failures.append(
            f"legacy_live samples {summary['legacy_count']} > budget {max_legacy_count}"
        )
    if summary["non_snapshot_count"] > max_non_snapshot_count:
        failures.append(
            "non-snapshot samples "
            f"{summary['non_snapshot_count']} > budget {max_non_snapshot_count}"
        )
    publisher_errors = summary["publisher_errors"]
    if publisher_errors:
        failures.append(f"publisher errors observed: {publisher_errors}")
    p95 = summary["latency_ms"]["p95"]
    if p95 is not None and p95 > max_p95_ms:
        failures.append(f"scene-state p95 latency {p95:.1f} ms > budget {max_p95_ms:.1f} ms")
    stale_ratio = summary["stale_ratio"]
    if stale_ratio is not None and stale_ratio > max_stale_ratio:
        failures.append(
            f"snapshot stale ratio {stale_ratio:.3f} > budget {max_stale_ratio:.3f}"
        )
    pending_ratio = summary["pending_ratio"]
    if pending_ratio is not None and pending_ratio > max_pending_ratio:
        failures.append(
            f"snapshot pending ratio {pending_ratio:.3f} > budget {max_pending_ratio:.3f}"
        )
    return failures


def _parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--base-url", default="http://127.0.0.1:9000")
    parser.add_argument("--duration-s", type=float, default=90.0)
    parser.add_argument("--hz", type=float, default=10.0)
    parser.add_argument("--timeout-s", type=float, default=2.0)
    parser.add_argument("--warmup-s", type=float, default=2.0)
    parser.add_argument("--max-error-count", type=int, default=0)
    parser.add_argument("--max-legacy-count", type=int, default=0)
    parser.add_argument("--max-non-snapshot-count", type=int, default=0)
    parser.add_argument("--max-p95-ms", type=float, default=100.0)
    parser.add_argument("--max-stale-ratio", type=float, default=0.05)
    parser.add_argument("--max-pending-ratio", type=float, default=0.10)
    parser.add_argument(
        "--samples-jsonl",
        default=None,
        help="Optional path for per-sample JSONL output",
    )
    parser.add_argument(
        "--summary-json",
        default=None,
        help="Optional path for the final summary JSON",
    )
    parser.add_argument(
        "--print-samples",
        action="store_true",
        help="Print every sample as JSONL to stdout",
    )
    return parser.parse_args()


def main() -> int:
    args = _parse_args()
    if args.duration_s <= 0:
        raise SystemExit("--duration-s must be positive")
    if args.hz <= 0:
        raise SystemExit("--hz must be positive")
    period_s = 1.0 / args.hz
    deadline = time.monotonic() + args.duration_s
    next_sample_at = time.monotonic() + max(0.0, args.warmup_s)
    samples: list[Sample] = []
    samples_handle = None
    try:
        if args.samples_jsonl:
            samples_path = Path(args.samples_jsonl)
            samples_path.parent.mkdir(parents=True, exist_ok=True)
            samples_handle = samples_path.open("w", encoding="utf-8")
        sample_index = 0
        while time.monotonic() < deadline:
            now = time.monotonic()
            if now < next_sample_at:
                time.sleep(min(0.05, next_sample_at - now))
                continue
            sample = _sample_scene_state(
                base_url=args.base_url,
                index=sample_index,
                timeout_s=args.timeout_s,
            )
            samples.append(sample)
            sample_json = sample.to_json()
            if samples_handle is not None:
                samples_handle.write(json.dumps(sample_json, sort_keys=True) + "\n")
                samples_handle.flush()
            if args.print_samples:
                print(json.dumps(sample_json, sort_keys=True), flush=True)
            sample_index += 1
            next_sample_at += period_s
            if next_sample_at < time.monotonic() - period_s:
                next_sample_at = time.monotonic() + period_s
    finally:
        if samples_handle is not None:
            samples_handle.close()

    summary = _build_summary(samples)
    failures = _evaluate(
        summary,
        max_error_count=args.max_error_count,
        max_legacy_count=args.max_legacy_count,
        max_non_snapshot_count=args.max_non_snapshot_count,
        max_p95_ms=args.max_p95_ms,
        max_stale_ratio=args.max_stale_ratio,
        max_pending_ratio=args.max_pending_ratio,
    )
    result = {
        "success": not failures,
        "base_url": args.base_url,
        "duration_s": args.duration_s,
        "hz": args.hz,
        "budgets": {
            "max_error_count": args.max_error_count,
            "max_legacy_count": args.max_legacy_count,
            "max_non_snapshot_count": args.max_non_snapshot_count,
            "max_p95_ms": args.max_p95_ms,
            "max_stale_ratio": args.max_stale_ratio,
            "max_pending_ratio": args.max_pending_ratio,
        },
        "summary": summary,
        "failures": failures,
    }
    if args.summary_json:
        summary_path = Path(args.summary_json)
        summary_path.parent.mkdir(parents=True, exist_ok=True)
        summary_path.write_text(json.dumps(result, indent=2, sort_keys=True), encoding="utf-8")
    print(json.dumps(result, indent=2, sort_keys=True))
    return 0 if not failures else 1


if __name__ == "__main__":
    raise SystemExit(main())

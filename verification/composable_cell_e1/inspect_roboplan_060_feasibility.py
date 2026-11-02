#!/usr/bin/env python3
"""Inspect the exact RoboPlan 0.6.0 E1 compatibility surface.

This is a pinned-source feasibility probe, not an Adopt/No-go selector.  It
records both upstream improvements and missing ManyForge guarantees so E1 can
assess the smallest maintainable 0.6 patch set before making a decision.
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Final


EXPECTED_COMMIT: Final = "d2e5162eda2f51f5599ecd60375b65346d0b2944"
EXPECTED_TREE: Final = "60b5791cd34288fc19eea2777a2fc6867c3af0b2"
EXPECTED_TAG: Final = "0.6.0"
PROXSUITE_ARCHIVE_SHA256: Final = (
    "e634babff534c8812c6dbc6b022b1d01f9a2a4a9a73cf87a6dc299a4de996904"
)


@dataclass(frozen=True)
class SourceFile:
    relative_path: str
    text: str

    def line(self, needle: str) -> int:
        offset = self.text.find(needle)
        if offset < 0:
            raise ValueError(
                f"{self.relative_path}: missing expected source text: {needle!r}"
            )
        return self.text.count("\n", 0, offset) + 1


def _git(root: Path, *args: str) -> str:
    completed = subprocess.run(
        ["git", "-C", str(root), *args],
        check=True,
        capture_output=True,
        text=True,
    )
    return completed.stdout.strip()


def _read(root: Path, relative_path: str) -> SourceFile:
    path = root / relative_path
    if not path.is_file():
        raise ValueError(f"candidate source is missing {relative_path}")
    return SourceFile(relative_path, path.read_text(encoding="utf-8"))


def _evidence(source: SourceFile, needle: str, statement: str) -> dict[str, object]:
    return {
        "path": source.relative_path,
        "line": source.line(needle),
        "statement": statement,
    }


def inspect_candidate(root: Path) -> dict[str, object]:
    root = root.resolve()
    if not (root / ".git").exists():
        raise ValueError(f"candidate source is not a Git checkout: {root}")

    commit = _git(root, "rev-parse", "HEAD^{commit}")
    tree = _git(root, "rev-parse", "HEAD^{tree}")
    status = _git(root, "status", "--porcelain=v1", "--untracked-files=all")
    tags = set(_git(root, "tag", "--points-at", "HEAD").splitlines())
    if commit != EXPECTED_COMMIT:
        raise ValueError(
            f"expected RoboPlan commit {EXPECTED_COMMIT}, observed {commit}"
        )
    if tree != EXPECTED_TREE:
        raise ValueError(f"expected RoboPlan tree {EXPECTED_TREE}, observed {tree}")
    if status:
        raise ValueError("RoboPlan 0.6.0 candidate checkout is not clean")
    if EXPECTED_TAG not in tags:
        raise ValueError(f"RoboPlan candidate lacks tag {EXPECTED_TAG} at HEAD")

    cartesian_header = _read(
        root,
        "roboplan_cartesian_planning/include/roboplan_cartesian_planning/"
        "cartesian_path_planner.hpp",
    )
    cartesian_source = _read(
        root, "roboplan_cartesian_planning/src/cartesian_path_planner.cpp"
    )
    toppra_header = _read(root, "roboplan_toppra/include/roboplan_toppra/toppra.hpp")
    toppra_source = _read(root, "roboplan_toppra/src/toppra.cpp")
    toppra_cmake = _read(root, "roboplan_toppra/CMakeLists.txt")
    scene_header = _read(root, "roboplan/include/roboplan/core/scene.hpp")
    scene_source = _read(root, "roboplan/src/core/scene.cpp")
    context_header = _read(root, "roboplan/include/roboplan/core/collision_context.hpp")
    oink_cmake = _read(root, "roboplan_oink/CMakeLists.txt")

    for hook in ("cancellation_requested", "progress_observer", "deadline"):
        if hook in cartesian_header.text or hook in cartesian_source.text:
            raise ValueError(
                f"candidate unexpectedly contains {hook}; refresh the E1 assessment"
            )
    if (
        "cancellation" in toppra_header.text.lower()
        or "cancellation" in toppra_source.text.lower()
    ):
        raise ValueError("candidate unexpectedly exposes TOPPRA cancellation")

    time_parameterize_call = (
        "auto trajectory = timeParameterize(*resolved, velocity_scale, "
        "acceleration_scale);"
    )
    compute_call = "const auto rc = algo->computePathParametrization();"
    unchecked_count = (
        "const auto num_traj_pts = static_cast<size_t>(std::ceil(t_final / "
        "options.dt)) + 1;"
    )
    for source, needle in (
        (cartesian_source, time_parameterize_call),
        (toppra_source, compute_call),
        (toppra_source, unchecked_count),
    ):
        source.line(needle)

    allocation_start = toppra_source.text.index("const auto t_final =")
    allocation_end = toppra_source.text.index(
        "traj.accelerations.reserve", allocation_start
    )
    allocation_block = toppra_source.text[allocation_start:allocation_end]
    if any(
        token in allocation_block for token in ("isfinite", "max_num", "max_samples")
    ):
        raise ValueError(
            "candidate unexpectedly bounds TOPPRA output allocation; "
            "refresh the E1 assessment"
        )

    if any(
        token in scene_header.text
        for token in ("clone() const", "replaceWithIsolatedCopy")
    ):
        raise ValueError(
            "candidate unexpectedly contains the required isolated Scene contract"
        )
    raw_model = "const pinocchio::Model& model_;"
    raw_geometry = "const pinocchio::GeometryModel& collision_model_;"
    context_header.line(raw_model)
    context_header.line(raw_geometry)
    scratch_assignments = (
        "model_data_ = pinocchio::Data(model_);",
        "collision_model_data_ = pinocchio::GeometryData(collision_model_);",
    )
    for needle in scratch_assignments:
        scene_source.line(needle)

    prox_url = (
        "https://github.com/Simple-Robotics/proxsuite/archive/refs/tags/"
        "v0.7.3.tar.gz"
    )
    prox_hash = f"URL_HASH SHA256={PROXSUITE_ARCHIVE_SHA256}"
    oink_cmake.line(prox_url)
    oink_cmake.line(prox_hash)
    toppra_repository = "https://github.com/hungpham2511/toppra.git"
    toppra_tag = "GIT_TAG 0.6.8"
    toppra_cmake.line(toppra_repository)
    toppra_cmake.line(toppra_tag)

    upstream_bounds = (
        (cartesian_source, "constexpr size_t kMaxToppraWaypoints = 10000;"),
        (cartesian_source, "constexpr size_t kMaxResolutionSamples = 100000;"),
        (cartesian_source, "constexpr int kMaxIkConvergenceIters = 500;"),
        (cartesian_source, "constexpr int kMaxBoundedSlowdownPasses = 4;"),
    )
    for source, needle in upstream_bounds:
        source.line(needle)

    return {
        "schema": "manyforge.composable_cell.e1.roboplan_060_feasibility.v1",
        "candidate": {
            "release": EXPECTED_TAG,
            "commit": commit,
            "tree": tree,
            "clean": True,
        },
        "decision": "undecided",
        "upstream_improvements": [
            {
                "id": "cartesian_internal_work_caps",
                "evidence": [
                    _evidence(source, needle, "upstream finite work cap")
                    for source, needle in upstream_bounds
                ],
            },
            {
                "id": "proxsuite_archive_is_digest_pinned",
                "evidence": _evidence(
                    oink_cmake,
                    prox_hash,
                    "the ProxSuite v0.7.3 fallback has an exact archive hash",
                ),
            },
        ],
        "missing_guarantees": [
            {
                "id": "bounded_cartesian_in_flight_control",
                "reason": (
                    "Bounded planning enters TOPPRA, while neither public options nor "
                    "the opaque solver call can observe cancellation, deadline, or progress."
                ),
                "evidence": [
                    _evidence(
                        cartesian_source,
                        time_parameterize_call,
                        "bounded planning routes its resolved path through TOPPRA",
                    ),
                    _evidence(
                        toppra_source,
                        compute_call,
                        "the solver call has no in-flight control handoff",
                    ),
                ],
            },
            {
                "id": "toppra_output_allocation_bound",
                "reason": (
                    "A solver-derived duration is converted to size_t and used for four "
                    "reserves without a finite, representable, configured cardinality bound."
                ),
                "evidence": _evidence(
                    toppra_source,
                    unchecked_count,
                    "unchecked duration-to-size conversion determines output cardinality",
                ),
            },
            {
                "id": "isolated_scene_clone_and_replacement",
                "reason": "No explicit isolated Scene clone/replacement contract exists.",
                "evidence": {"path": scene_header.relative_path, "line": 1},
            },
            {
                "id": "collision_context_scene_lifetime",
                "reason": "CollisionContext borrows model storage from its source Scene.",
                "evidence": [
                    _evidence(context_header, raw_model, "borrowed model reference"),
                    _evidence(
                        context_header,
                        raw_geometry,
                        "borrowed collision-model reference",
                    ),
                ],
            },
            {
                "id": "scene_scratch_assignment_lifetime",
                "reason": (
                    "Scene default-constructs Pinocchio scratch and copy-assigns model-bound "
                    "Data after parsing; the exact sanitizer lane must prove whether this reads "
                    "uninitialized solver state."
                ),
                "evidence": [
                    _evidence(
                        scene_source,
                        needle,
                        "model-bound scratch is installed by copy assignment",
                    )
                    for needle in scratch_assignments
                ],
            },
        ],
        "dependency_closure": [
            {
                "id": "proxsuite_v0_7_3",
                "status": "digest_pinned_but_network_fallback",
                "evidence": [
                    _evidence(oink_cmake, prox_url, "configure-time archive fallback"),
                    _evidence(oink_cmake, prox_hash, "exact archive digest"),
                ],
            },
            {
                "id": "toppra_v0_6_8",
                "status": "tag_pinned_network_fallback_without_commit_identity",
                "evidence": [
                    _evidence(
                        toppra_cmake,
                        toppra_repository,
                        "configure-time Git repository fallback",
                    ),
                    _evidence(toppra_cmake, toppra_tag, "release-tag selector"),
                ],
            },
        ],
        "next_gate": (
            "Assess a minimal 0.6 compatibility patch for every missing guarantee, "
            "then compare its depth and future removal path with deferring the upgrade."
        ),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output", type=Path)
    args = parser.parse_args(argv)
    try:
        result = inspect_candidate(args.source)
    except (OSError, ValueError, subprocess.SubprocessError) as exc:
        print(f"RoboPlan 0.6.0 E1 feasibility probe failed: {exc}", file=sys.stderr)
        return 1

    encoded = json.dumps(result, indent=2, sort_keys=True) + "\n"
    if args.output is not None:
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(encoded, encoding="utf-8")
    else:
        print(encoded, end="")
    print(
        "RoboPlan 0.6.0 E1 feasibility facts reproduced on exact clean source",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

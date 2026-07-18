#!/usr/bin/env python3
"""Reject regressions to the demand-scoped, process-isolated viewer path."""

from __future__ import annotations

import re
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]


def _read(relative: str) -> str:
    path = ROOT / relative
    if not path.is_file():
        raise FileNotFoundError(relative)
    return path.read_text(encoding="utf-8")


def main() -> int:
    failures: list[str] = []
    required = (
        "manyforge_composer/backend/routes_viewer.py",
        "manyforge_composer/backend/viewer_data_plane.py",
        "manyforge_composer/backend/viewer_worker.py",
        "manyforge_composer/frontend/src/store/viewerFrameStore.ts",
        "manyforge_composer/frontend/src/store/pointCloudStore.ts",
    )
    sources: dict[str, str] = {}
    for relative in required:
        try:
            sources[relative] = _read(relative)
        except FileNotFoundError:
            failures.append(f"missing viewer architecture file: {relative}")

    checks = (
        (
            "manyforge_composer/backend/routes_scene.py",
            r"\b(?:compute_robot_visual_link_poses|build_robot_visual_snapshot|ViewerProtocolEncoder)\b",
            "parent-process visual FK or frame encoding",
        ),
        (
            "manyforge_composer/backend/state.py",
            r"\bViewerProtocolEncoder\b",
            "parent-process viewer protocol encoder",
        ),
        (
            "manyforge_composer/backend/app.py",
            r"\b(?:ensure_viewer_state_publisher|ensure_visual_state_publisher)\s*\(",
            "unconditional application-start viewer publisher",
        ),
        (
            "manyforge_composer/backend/routes_pointcloud.py",
            r"\b(?:send_json|pointcloud_frame|pointcloud_clear)\b",
            "superseded JSON point-cloud transport",
        ),
        (
            "manyforge_composer/frontend/src/store/pointCloudStore.ts",
            r"\b(?:zustand|usePointCloudStore|pointcloud_frame|pointcloud_clear)\b",
            "superseded React/JSON point-cloud state path",
        ),
        (
            "manyforge_composer/frontend/src/components/SceneViewer.tsx",
            r"api\.scene\.(?:state|frames)\s*\(",
            "REST animation read from the renderer",
        ),
        (
            "manyforge_composer/backend/viewer_worker.py",
            r"\b(?:ComposerState|IPlanningContext|SceneAuthority|ExecutionKernel)\b",
            "control/planner authority imported into the visual worker",
        ),
    )
    for relative, pattern, description in checks:
        try:
            source = _read(relative)
        except FileNotFoundError:
            failures.append(f"missing checked viewer source: {relative}")
            continue
        if re.search(pattern, source):
            failures.append(f"{relative}: contains {description} ({pattern})")

    all_production = (
        ROOT / "manyforge_composer" / "backend",
        ROOT / "manyforge_composer" / "frontend" / "src",
    )
    for root in all_production:
        for path in root.rglob("*"):
            if not path.is_file() or path.suffix not in {".py", ".ts", ".tsx"}:
                continue
            source = path.read_text(encoding="utf-8", errors="replace")
            if re.search(r"manyforge\.viewer\.(?:manifest|frame|control)\.v0", source):
                failures.append(
                    f"{path.relative_to(ROOT)}: reintroduces a v0 viewer wire schema"
                )
            if "visual_scene_snapshot.v0" in source:
                failures.append(
                    f"{path.relative_to(ROOT)}: reintroduces the retired full-snapshot v0 schema"
                )

    required_tokens = (
        (
            "manyforge_composer/backend/viewer_worker.py",
            "mp.get_context(\"spawn\")",
            "spawn-isolated viewer worker",
        ),
        (
            "manyforge_composer/backend/viewer_data_plane.py",
            "_ingress_pending",
            "capacity-one viewer ingress",
        ),
        (
            "manyforge_composer/backend/routes_pointcloud.py",
            "_POINTCLOUD_MAGIC",
            "bounded binary point-cloud protocol",
        ),
        (
            "manyforge_composer/frontend/src/components/SceneViewer.tsx",
            'frameloop="demand"',
            "demand-driven R3F rendering",
        ),
    )
    for relative, token, description in required_tokens:
        try:
            source = _read(relative)
        except FileNotFoundError:
            continue
        if token not in source:
            failures.append(f"{relative}: missing {description} ({token})")

    if failures:
        print("Viewer data-plane boundary check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1
    print("Viewer data-plane boundary check passed.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

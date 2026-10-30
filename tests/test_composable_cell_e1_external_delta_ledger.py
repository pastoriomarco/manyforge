"""Contract tests for the E1 external-dependency delta ledger."""

from __future__ import annotations

import copy
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

import pytest

from verification.composable_cell_e1 import verify_external_delta_ledger as verifier


_REPO_ROOT = Path(__file__).resolve().parents[1]
_LEDGER = _REPO_ROOT / "verification/composable_cell_e1/external_delta_ledger.v1.json"
_PATCH_ROOT = _REPO_ROOT / "third_party/roboplan"


def _load() -> dict[str, object]:
    return json.loads(_LEDGER.read_text(encoding="utf-8"))


def test_frozen_ledger_is_complete_and_cryptographically_current() -> None:
    verifier.validate(
        _load(),
        repository_root=_REPO_ROOT,
        require_frozen=True,
    )


def test_close_gate_rejects_an_undecided_or_pending_ledger() -> None:
    ledger = copy.deepcopy(_load())
    ledger["status"] = "evidence_in_progress"
    with pytest.raises(verifier.LedgerError, match="not frozen"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=True,
        )


def test_current_patch_identity_cannot_drift_silently() -> None:
    ledger = copy.deepcopy(_load())
    first_source = ledger["deltas"][0]["local_sources"][0]
    first_source["sha256"] = "0" * 64
    with pytest.raises(verifier.LedgerError, match="digest mismatch"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


@pytest.mark.parametrize(
    ("delta_id", "retired_path"),
    (
        (
            "roboplan_patch_0004_build_helper_arguments",
            "third_party/roboplan/patches/0004-configurable-source-build-cmake-arguments.patch",
        ),
        (
            "roboplan_patch_0005_scene_scratch_construction",
            "third_party/roboplan/patches/0005-construct-roboplan-060-scene-scratch-without-assignment.patch",
        ),
    ),
)
def test_retired_patch_remains_bound_to_its_exact_historical_blob(
    delta_id: str, retired_path: str
) -> None:
    ledger = copy.deepcopy(_load())
    delta = next(row for row in ledger["deltas"] if row["id"] == delta_id)
    source = next(
        source
        for source in delta["local_sources"]
        if source["kind"] == "historical_git_path" and source["path"] == retired_path
    )
    assert not (_REPO_ROOT / retired_path).exists()
    source["blob"] = "0" * 40

    with pytest.raises(verifier.LedgerError, match="blob mismatch"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_dependency_source_license_and_archive_identity_are_mandatory() -> None:
    ledger = copy.deepcopy(_load())
    ledger["frozen_sources"][1].pop("license")
    with pytest.raises(verifier.LedgerError, match="license"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_decision_record_cannot_select_the_rejected_generation() -> None:
    ledger = copy.deepcopy(_load())
    ledger["roboplan_decision_record"]["selected_source_id"] = (
        "roboplan_0_5_1_control"
    )
    with pytest.raises(verifier.LedgerError, match="wrong selected source"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_selected_outcome_requires_the_complete_architecture_lane_matrix() -> None:
    ledger = copy.deepcopy(_load())
    ledger["selected_outcome_qualification"]["required_lanes"].pop()
    with pytest.raises(verifier.LedgerError, match="inventory is incomplete"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_decided_outcome_names_its_source_while_qualification_is_in_progress() -> None:
    ledger = copy.deepcopy(_load())
    qualification = ledger["selected_outcome_qualification"]
    qualification["selected_source_id"] = "roboplan_0_5_1_control"
    with pytest.raises(verifier.LedgerError, match="selected source disagrees"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_qualified_outcome_cannot_hide_a_pending_lane() -> None:
    ledger = copy.deepcopy(_load())
    lane = ledger["selected_outcome_qualification"]["required_lanes"][0]
    lane["status"] = "pending_selected_outcome_replay"
    lane["evidence"] = []
    with pytest.raises(verifier.LedgerError, match="incomplete evidence"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_qualified_outcome_requires_a_hash_bound_compact_result() -> None:
    ledger = copy.deepcopy(_load())
    qualification = ledger["selected_outcome_qualification"]
    qualification.pop("result_record")
    with pytest.raises(verifier.LedgerError, match="result_record"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def test_source_closure_digest_binds_license_and_exact_sources() -> None:
    ledger = copy.deepcopy(_load())
    ledger["planner_source_closures"][1]["identity_sha256"] = "0" * 64
    with pytest.raises(verifier.LedgerError, match="identity_sha256 mismatch"):
        verifier.validate(
            ledger,
            repository_root=_REPO_ROOT,
            require_frozen=False,
        )


def _selected_roboplan_source_ids() -> set[str]:
    """Return the RoboPlan source ids the image and setup verifiers select."""

    selected: set[str] = set()
    for name in ("Dockerfile", "Dockerfile.ros-lyrical-full"):
        source = (_REPO_ROOT / name).read_text(encoding="utf-8")
        selected.update(re.findall(r"ROBOPLAN_SOURCE_ID=(roboplan_[0-9a-z_]+)", source))
        selected.update(re.findall(r"--source-id (roboplan_[0-9a-z_]+)", source))
    setup = (_REPO_ROOT / "scripts/setup-docker.sh").read_text(encoding="utf-8")
    selected.update(re.findall(r"--source-id (roboplan_[0-9a-z_]+)", setup))
    return selected


def test_selected_patch_manifest_and_ledger_name_exactly_one_series() -> None:
    manifest: dict[str, str] = {}
    for line in (_PATCH_ROOT / "patches.sha256").read_text(
        encoding="utf-8"
    ).splitlines():
        digest, relative = line.split()
        path = f"third_party/roboplan/{relative}"
        assert path not in manifest
        manifest[path] = digest
    assert manifest
    for path, digest in manifest.items():
        assert hashlib.sha256((_REPO_ROOT / path).read_bytes()).hexdigest() == digest

    # The checked-in patch directory carries only the applied series.
    assert {
        f"third_party/roboplan/patches/{path.name}"
        for path in (_PATCH_ROOT / "patches").glob("*.patch")
    } == set(manifest)

    ledger = _load()
    selected_source_ids = _selected_roboplan_source_ids()
    assert len(selected_source_ids) == 1
    frozen_source_ids = {source["id"] for source in ledger["frozen_sources"]}
    assert selected_source_ids <= frozen_source_ids
    roboplan_source_ids = {
        source_id for source_id in frozen_source_ids if source_id.startswith("roboplan_")
    }

    ledger_sources: dict[str, str] = {}
    retired_paths: set[str] = set()
    for delta in ledger["deltas"]:
        for source in delta["local_sources"]:
            if not source["path"].startswith("third_party/roboplan/patches/"):
                continue
            if source["kind"] == "historical_git_path":
                retired_paths.add(source["path"])
                continue
            assert source["kind"] == "repository_path"
            assert source["path"] not in ledger_sources
            ledger_sources[source["path"]] = source["sha256"]
            assert (
                set(delta["source_identity_refs"]) & roboplan_source_ids
                == selected_source_ids
            )
    assert ledger_sources == manifest
    assert retired_paths
    assert retired_paths.isdisjoint(manifest)


def test_clearance_margin_patch_is_carried_with_dossier_and_build_digest(
    tmp_path: Path,
) -> None:
    """M6 clearance: patch 0015 is in the checked manifest, its ledger delta and
    dossier exist, and the recorded build-content digest is the 0010-0015 tree."""

    patch = "patches/0015-roboplan-070-scene-collision-pair-margins.patch"
    manifest = [
        line.split()
        for line in (_PATCH_ROOT / "patches.sha256").read_text(encoding="utf-8").splitlines()
    ]
    assert manifest[-1][1] == patch
    body = (_PATCH_ROOT / patch).read_text(encoding="utf-8")
    assert hashlib.sha256(body.encode("utf-8")).hexdigest() == manifest[-1][0]
    touched = set(re.findall(r"^diff --git a/(\S+) ", body, re.MULTILINE))
    assert touched == {
        "roboplan_core/include/roboplan/core/scene.hpp",
        "roboplan_core/src/core/scene.cpp",
        "roboplan_core/src/core/scene_context.cpp",
        "roboplan_core/test/test_scene.cpp",
    }
    assert "setCollisionPairMargins" in body
    assert patch.removeprefix("patches/") in (_PATCH_ROOT / "README.md").read_text(
        encoding="utf-8"
    )

    ledger = _load()
    delta = next(
        row
        for row in ledger["deltas"]
        if row["id"] == "roboplan_patch_0015_scene_collision_pair_margins"
    )
    assert delta["local_sources"][0]["sha256"] == manifest[-1][0]
    assert (_REPO_ROOT / delta["disposition"]["upstream_dossier"]).is_file()

    # When setup-docker has materialized the exact source, the six-patch series
    # applies in manifest order, as both Dockerfiles do (umask 0002), and yields
    # the recorded build-content digest.
    source = _REPO_ROOT / ".docker" / "roboplan"
    if not (source / "roboplan_core").is_dir():
        pytest.skip("RoboPlan source is not materialized in .docker/roboplan")
    root = tmp_path / "roboplan"
    shutil.copytree(source, root, symlinks=True, ignore=shutil.ignore_patterns(".git"))
    previous_umask = os.umask(0o002)
    try:
        for _, relative in manifest:
            subprocess.run(
                ["git", "-C", str(root), "apply", str(_PATCH_ROOT / relative)],
                check=True,
            )
    finally:
        os.umask(previous_umask)
    subprocess.run(
        [
            sys.executable,
            str(_REPO_ROOT / "scripts/ci/verify-external-source-closure.py"),
            "--ledger",
            str(_LEDGER),
            "--source-id",
            "roboplan_0_7_0_ua1_foundation",
            "--root",
            str(root),
            "--content-kind",
            "build",
            "--allow-stripped-git",
        ],
        check=True,
    )

"""Focused contracts for the explicit target Release-golden workflow."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path
import sys

import pytest


_REPO_ROOT = Path(__file__).resolve().parents[1]
_TOOL_PATH = (
    _REPO_ROOT
    / "scripts"
    / "qualification"
    / "review_canonical_empty_release_golden.py"
)
_SPEC = importlib.util.spec_from_file_location("release_golden_workflow", _TOOL_PATH)
assert _SPEC is not None and _SPEC.loader is not None
workflow = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = workflow
_SPEC.loader.exec_module(workflow)


def _digest(character: str) -> str:
    return "sha256:" + character * 64


def _candidate_package(
    root: Path, *, profile: str = "Release", target: str = "Linux/x86_64"
) -> bytes:
    compiler_build_digest = _digest("b")
    admission = {
        "admission_digest": _digest("a"),
        "compiler_build_digest": compiler_build_digest,
    }
    admission_bytes = json.dumps(
        admission, sort_keys=True, separators=(",", ":")
    ).encode("utf-8")
    (root / "canonical-empty-admission.json").write_bytes(admission_bytes)
    manifest_root = root / "build-manifests" / "compiler_build"
    manifest_root.mkdir(parents=True)
    manifest = {
        "build_profile": profile,
        "compiler_build_digest": compiler_build_digest,
        "target": target,
    }
    (manifest_root / f"{'b' * 64}.json").write_text(
        json.dumps(manifest, sort_keys=True, separators=(",", ":")),
        encoding="utf-8",
    )
    return admission_bytes


@pytest.mark.parametrize(
    ("target", "slug"),
    (
        ("Linux/x86_64", "linux-amd64"),
        ("Linux/AMD64", "linux-amd64"),
        ("Linux/aarch64", "linux-arm64"),
        ("Linux/arm64", "linux-arm64"),
    ),
)
def test_official_release_targets_have_stable_fixture_names(
    target: str, slug: str
) -> None:
    assert workflow.release_target_slug(target) == slug


def test_nonofficial_release_target_is_rejected() -> None:
    with pytest.raises(workflow.ReleaseGoldenError, match="official Linux"):
        workflow.release_target_slug("Darwin/arm64")


def test_generated_release_candidate_selects_its_exact_target_fixture(
    tmp_path: Path,
) -> None:
    admission = _candidate_package(tmp_path)

    candidate = workflow.load_release_candidate(tmp_path, admission + b"\n")

    assert candidate.canonical_line == admission + b"\n"
    assert candidate.build_target == "Linux/x86_64"
    assert candidate.target_slug == "linux-amd64"
    assert candidate.admission_digest == _digest("a")
    assert candidate.compiler_build_digest == _digest("b")
    assert workflow.golden_path(candidate).name == (
        "canonical_empty_admission.linux-amd64.release.json"
    )


def test_candidate_rejects_nonrelease_profile_and_divergent_stdout(
    tmp_path: Path,
) -> None:
    nonrelease = tmp_path / "nonrelease"
    nonrelease.mkdir()
    admission = _candidate_package(nonrelease, profile="Debug")
    with pytest.raises(workflow.ReleaseGoldenError, match="not 'Release'"):
        workflow.load_release_candidate(nonrelease, admission + b"\n")

    release = tmp_path / "release"
    release.mkdir()
    admission = _candidate_package(release)
    with pytest.raises(workflow.ReleaseGoldenError, match="stdout"):
        workflow.load_release_candidate(release, admission + b"changed\n")


def test_check_rejects_drift_and_update_atomically_replaces_it(
    tmp_path: Path,
) -> None:
    package = tmp_path / "package"
    package.mkdir()
    admission = _candidate_package(package)
    candidate = workflow.load_release_candidate(package, admission + b"\n")
    destination = tmp_path / "golden.json"
    destination.write_bytes(b"stale\n")

    with pytest.raises(workflow.ReleaseGoldenError, match="differs"):
        workflow.review_candidate(candidate, destination, update=False)
    assert destination.read_bytes() == b"stale\n"

    workflow.review_candidate(candidate, destination, update=True)
    workflow.review_candidate(candidate, destination, update=False)
    assert destination.read_bytes() == admission + b"\n"
    assert list(tmp_path.glob(".golden.json.*")) == []


def test_ordinary_development_paths_contain_no_fixed_release_gate() -> None:
    paths = (
        _REPO_ROOT / "scripts" / "test-all.sh",
        _REPO_ROOT / "manyforge_deployment" / "CMakeLists.txt",
        _REPO_ROOT
        / "manyforge_deployment"
        / "test"
        / "test_build_manifest.cpp",
        _REPO_ROOT
        / "manyforge_deployment"
        / "test"
        / "test_canonical_empty_admission.cpp",
        _REPO_ROOT
        / "verification"
        / "composable_cell_i2"
        / "verify_qualification_manifest.py",
    )
    for path in paths:
        source = path.read_text(encoding="utf-8")
        assert "MANYFORGE_VERIFY_CANONICAL_EMPTY_RELEASE_GOLDEN" not in source
        assert "kExpectedReleaseCanonicalEmptyBundleId" not in source
        assert "kExpectedReleaseCompilerBuildDigest" not in source

    qualification_manifest = json.loads(
        (
            _REPO_ROOT
            / "verification"
            / "composable_cell_i2"
            / "qualification_manifest.v1.json"
        ).read_text(encoding="utf-8")
    )
    tracked_paths = {
        row["path"]
        for row in qualification_manifest["pinned_inputs"]["tracked_files"]
    }
    assert not any("canonical_empty_admission" in path for path in tracked_paths)

"""Contract tests for the E1 external-dependency delta ledger."""

from __future__ import annotations

import copy
import json
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


def test_retired_patch_remains_bound_to_its_exact_historical_blob() -> None:
    ledger = copy.deepcopy(_load())
    delta = next(
        row
        for row in ledger["deltas"]
        if row["id"] == "roboplan_patch_0004_build_helper_arguments"
    )
    delta["local_sources"][0]["blob"] = "0" * 40

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


def test_selected_patch_manifest_and_ledger_name_exactly_one_series() -> None:
    manifest: dict[str, str] = {}
    for line in (_PATCH_ROOT / "patches.sha256").read_text(
        encoding="utf-8"
    ).splitlines():
        digest, relative = line.split()
        manifest[f"third_party/roboplan/{relative}"] = digest

    expected = {
        "third_party/roboplan/patches/0005-construct-roboplan-060-scene-scratch-without-assignment.patch": "00ca95fea84dcf1180d80f8abd2782897a3911fef80ae639131d75f834250184",
        "third_party/roboplan/patches/0006-bound-roboplan-060-toppra-output-samples.patch": "ba886434bdca4eafb6f59ce630b1b4a4957a2c7ffb0c7e3c140a1bab5c5fac6a",
        "third_party/roboplan/patches/0008-initialize-roboplan-060-dynotree-variant-state.patch": "e705472bfa05feecc8cf0b669018968966687a5a911047307be01f421ac2cbdd",
        "third_party/roboplan/patches/0009-direct-initialize-roboplan-060-pinocchio-scratch.patch": "af8526dad047c046c6cd5a33a0513a3c94fca321da22c658d54e0efa33a5f858",
    }
    assert manifest == expected
    assert {
        f"third_party/roboplan/patches/{path.name}"
        for path in (_PATCH_ROOT / "patches").glob("*.patch")
    } == set(expected)

    ledger_sources = {
        source["path"]: source["sha256"]
        for delta in _load()["deltas"]
        for source in delta["local_sources"]
        if source["kind"] == "repository_path"
        and source["path"].startswith("third_party/roboplan/patches/")
    }
    assert ledger_sources == expected

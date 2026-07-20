#!/usr/bin/env python3
"""Generate the immutable, replayable input manifest for final Step 18 closure."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import subprocess
import sys
from pathlib import Path
from typing import Any

from source_identity import (
    SPECS_CONTRACT_EXCLUDED_PATH,
    SourceIdentityError,
    source_identity,
    specs_contract_identity,
    validate_identity,
    validate_specs_contract_identity,
)


SCHEMA = "manyforge.step18.closure_manifest.v2"
INVOCATION_SCHEMA = "manyforge.step18.producer_invocation.v1"
EVIDENCE_SCHEMA = "manyforge.step18.closure_evidence.v2"
EVIDENCE_BEGIN = "<!-- BEGIN MANYFORGE STEP18 CLOSURE EVIDENCE V2 -->"
EVIDENCE_END = "<!-- END MANYFORGE STEP18 CLOSURE EVIDENCE V2 -->"
INPUTS = (
    "offline_proof",
    "scheduled_runtime",
    "live_contact_modes",
    "loaded_kernel",
    "release_gates",
)
PRODUCERS = {
    "offline_proof": (
        "scripts/qualification/verify_step18_proof_cells.py",
        "stdout_json",
    ),
    "scheduled_runtime": (
        "scripts/qualification/verify_runtime_collision_schedule_step18.py",
        "stdout_json",
    ),
    "live_contact_modes": (
        "scripts/qualification/verify_step18_live_contact_modes.py",
        "output_json",
    ),
    "loaded_kernel": (
        "scripts/qualification/verify_step18_loaded_kernel.py",
        "output_json",
    ),
    "release_gates": (
        "scripts/qualification/verify_step18_release_gates.py",
        "output_json",
    ),
}
SUMMARY_IDENTITY_FIELDS = {
    "offline_proof": (
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
    ),
    "scheduled_runtime": (
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
    ),
    "live_contact_modes": (
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
    ),
    "loaded_kernel": (
        "source_identity",
        "specs_source_identity_at_run",
        "specs_contract_identity",
    ),
    "release_gates": (
        "candidate_source_identity",
        "candidate_specs_source_identity",
        "candidate_specs_contract_identity",
    ),
}


class ManifestError(RuntimeError):
    """A closure input cannot be bound without ambiguity."""


class FileTracker:
    """Read each qualification input once and fail if any byte later changes."""

    def __init__(self) -> None:
        self.payloads: dict[Path, bytes] = {}
        self.digests: dict[Path, str] = {}

    def read(self, path: Path, label: str) -> tuple[bytes, str]:
        resolved = path.resolve()
        if resolved in self.payloads:
            return self.payloads[resolved], self.digests[resolved]
        try:
            payload = resolved.read_bytes()
        except OSError as error:
            raise ManifestError(f"{label} is not readable: {error}") from error
        digest = hashlib.sha256(payload).hexdigest()
        self.payloads[resolved] = payload
        self.digests[resolved] = digest
        return payload, digest

    def assert_unchanged(self) -> None:
        for path, expected in self.digests.items():
            try:
                observed = hashlib.sha256(path.read_bytes()).hexdigest()
            except OSError as error:
                raise ManifestError(f"retained input disappeared: {path}: {error}") from error
            if observed != expected:
                raise ManifestError(f"retained input changed during the closure operation: {path}")


def _json_payload(payload: bytes, label: str) -> dict[str, Any]:
    try:
        value = json.loads(payload.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise ManifestError(f"{label} is not readable JSON: {error}") from error
    if not isinstance(value, dict):
        raise ManifestError(f"{label} must contain one JSON object")
    return value


def _json_file(
    path: Path, label: str, tracker: FileTracker
) -> tuple[dict[str, Any], str]:
    payload, digest = tracker.read(path, label)
    return _json_payload(payload, label), digest


def _binding(
    raw: Any,
    *,
    owner_path: Path,
    label: str,
    tracker: FileTracker,
) -> dict[str, str]:
    if not isinstance(raw, dict) or set(raw) != {"path", "sha256"}:
        raise ManifestError(f"{label} must be an exact path/SHA-256 binding")
    raw_path = raw.get("path")
    expected = raw.get("sha256")
    if not isinstance(raw_path, str) or not raw_path:
        raise ManifestError(f"{label} path must be non-empty")
    if not isinstance(expected, str) or len(expected) != 64:
        raise ManifestError(f"{label} SHA-256 is malformed")
    candidate = Path(raw_path)
    path = candidate if candidate.is_absolute() else owner_path.parent / candidate
    path = path.resolve()
    _, observed = tracker.read(path, label)
    if observed != expected:
        raise ManifestError(f"{label} hash changed")
    return {"path": str(path), "sha256": observed}


def _manifest_binding(
    raw: Any,
    *,
    owner_path: Path,
    label: str,
    tracker: FileTracker,
) -> dict[str, str]:
    if not isinstance(raw, dict):
        raise ManifestError(f"{label} must be an object")
    return _binding(
        {"path": raw.get("path"), "sha256": raw.get("sha256")},
        owner_path=owner_path,
        label=label,
        tracker=tracker,
    )


def _role_bindings(
    summary: dict[str, Any],
    summary_path: Path,
    tracker: FileTracker,
    label: str,
) -> list[dict[str, str]]:
    raw = summary.get("artifact_manifest")
    if not isinstance(raw, list):
        raise ManifestError(f"{label}.artifact_manifest must be a list")
    result: list[dict[str, str]] = []
    for index, item in enumerate(raw):
        if not isinstance(item, dict):
            raise ManifestError(f"{label}.artifact_manifest[{index}] is not an object")
        role = item.get("role", "artifact")
        if not isinstance(role, str) or not role:
            raise ManifestError(f"{label}.artifact_manifest[{index}] role is invalid")
        bound = _binding(
            {"path": item.get("path"), "sha256": item.get("sha256")},
            owner_path=summary_path,
            label=f"{label}.artifact_manifest[{index}]",
            tracker=tracker,
        )
        result.append({"role": role, **bound})
    return result


def _one_role(
    bindings: list[dict[str, str]], role: str, label: str
) -> dict[str, str]:
    matches = [binding for binding in bindings if binding["role"] == role]
    if len(matches) != 1:
        raise ManifestError(f"{label} must contain exactly one {role!r} input")
    return matches[0]


def build_producer_invocations(
    *,
    summaries: dict[str, dict[str, Any]],
    summary_paths: dict[str, Path],
    manyforge_root: Path,
    tracker: FileTracker,
) -> dict[str, dict[str, Any]]:
    """Derive exact replay argv solely from typed producer output bindings."""

    invocations: dict[str, dict[str, Any]] = {}
    for name in INPUTS:
        relative_verifier, transport = PRODUCERS[name]
        verifier = (manyforge_root / relative_verifier).resolve()
        _, verifier_digest = tracker.read(verifier, f"{name} producer verifier")
        summary = summaries[name]
        owner = summary_paths[name]
        inputs: list[dict[str, str]]
        arguments: list[str]
        if name == "offline_proof":
            inputs = _role_bindings(summary, owner, tracker, name)
            artifacts = [
                item for item in inputs if item["role"] == "proof_cell_artifact"
            ]
            if len(artifacts) != 144:
                raise ManifestError("offline proof replay requires exactly 144 artifacts")
            clean = _one_role(inputs, "baseline_clean_checkout_manifest", name)
            patch = _one_role(inputs, "baseline_qualification_patch", name)
            if len(inputs) != 146:
                raise ManifestError("offline proof replay input inventory is not exact")
            arguments = [
                "--minimum-runs",
                "3",
                "--baseline-clean-checkout-manifest",
                clean["path"],
                "--baseline-qualification-patch",
                patch["path"],
                *(item["path"] for item in artifacts),
            ]
        elif name == "scheduled_runtime":
            inputs = _role_bindings(summary, owner, tracker, name)
            auxiliary_flags = (
                ("baseline_clean_checkout_manifest", "--baseline-clean-checkout-manifest"),
                ("baseline_harness_patch", "--baseline-harness-patch"),
                ("baseline_observation_patch", "--baseline-observation-patch"),
                (
                    "baseline_decision_parity_artifact",
                    "--baseline-decision-parity-artifact",
                ),
            )
            auxiliaries = {
                role: _one_role(inputs, role, name) for role, _ in auxiliary_flags
            }
            artifacts = [
                item
                for item in inputs
                if item["role"] == "runtime_schedule_artifact"
            ]
            if len(artifacts) != 24 or len(inputs) != 28:
                raise ManifestError(
                    "scheduled runtime replay requires 24 records plus four auxiliaries"
                )
            arguments = ["--minimum-runs", "3"]
            for role, flag in auxiliary_flags:
                arguments.extend((flag, auxiliaries[role]["path"]))
            arguments.extend(item["path"] for item in artifacts)
        else:
            field = {
                "live_contact_modes": "manifest",
                "loaded_kernel": "run_manifest",
                "release_gates": "artifact_manifest",
            }[name]
            source_binding = _manifest_binding(
                summary.get(field),
                owner_path=owner,
                label=f"{name}.{field}",
                tracker=tracker,
            )
            inputs = [{"role": field, **source_binding}]
            arguments = ["--manifest", source_binding["path"]]
            if name == "release_gates":
                arguments.insert(0, "verify")
        invocations[name] = {
            "schema_version": INVOCATION_SCHEMA,
            "verifier": {"path": str(verifier), "sha256": verifier_digest},
            "arguments": arguments,
            "inputs": inputs,
            "result_transport": transport,
        }
    return invocations


def parse_evidence_record(payload: bytes) -> dict[str, Any]:
    try:
        text = payload.decode("utf-8")
    except UnicodeDecodeError as error:
        raise ManifestError(f"Step 18 evidence record is not UTF-8: {error}") from error
    if text.count(EVIDENCE_BEGIN) != 1 or text.count(EVIDENCE_END) != 1:
        raise ManifestError("Step 18 evidence record must contain one exact closure JSON block")
    start = text.index(EVIDENCE_BEGIN) + len(EVIDENCE_BEGIN)
    end = text.index(EVIDENCE_END, start)
    if end <= start:
        raise ManifestError("Step 18 evidence closure block is empty")
    value = _json_payload(text[start:end].strip().encode("utf-8"), "evidence closure block")
    expected = {
        "schema_version",
        "status",
        "overall_step18_closure_claimed",
        "closure_verifier",
        "subgates",
    }
    if set(value) != expected:
        raise ManifestError("evidence closure block fields differ from the exact v1 contract")
    if (
        value.get("schema_version") != EVIDENCE_SCHEMA
        or value.get("status") != "ready_for_common_candidate_closure"
        or value.get("overall_step18_closure_claimed") is not False
    ):
        raise ManifestError("evidence closure block status/schema is not pre-closure ready")
    subgates = value.get("subgates")
    if not isinstance(subgates, dict) or set(subgates) != set(INPUTS):
        raise ManifestError("evidence closure block must name the exact five subgates")
    for name in INPUTS:
        gate = subgates[name]
        if (
            not isinstance(gate, dict)
            or set(gate) != {"summary_sha256", "passed"}
            or not isinstance(gate.get("summary_sha256"), str)
            or len(gate["summary_sha256"]) != 64
            or gate.get("passed") is not True
        ):
            raise ManifestError(f"evidence closure block has an invalid {name} entry")
    closure_verifier = value.get("closure_verifier")
    if (
        not isinstance(closure_verifier, dict)
        or set(closure_verifier) != {"path", "sha256"}
        or closure_verifier.get("path")
        != "scripts/qualification/verify_step18_closure.py"
        or not isinstance(closure_verifier.get("sha256"), str)
        or len(closure_verifier["sha256"]) != 64
        or any(character not in "0123456789abcdef" for character in closure_verifier["sha256"])
    ):
        raise ManifestError("evidence closure block verifier identity is invalid")
    return value


def ensure_ignored_output(output: Path, manyforge_root: Path) -> Path:
    resolved = output.resolve()
    evidence_root = (manyforge_root / ".manyforge-var/qualification").resolve()
    try:
        relative = resolved.relative_to(evidence_root)
    except ValueError as error:
        raise ManifestError(
            f"closure output must be below ignored evidence root {evidence_root}"
        ) from error
    if not relative.parts:
        raise ManifestError("closure output must be a file below the evidence root")
    repository_relative = resolved.relative_to(manyforge_root.resolve())
    check = subprocess.run(
        ["git", "-C", str(manyforge_root), "check-ignore", "-q", "--", str(repository_relative)],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if check.returncode != 0:
        raise ManifestError("closure output is not ignored by the candidate repository")
    return resolved


def _validated_summary_identities(
    summaries: dict[str, dict[str, Any]],
    current_source: dict[str, Any],
    current_contract: dict[str, Any],
) -> dict[str, Any]:
    common_specs: dict[str, Any] | None = None
    for name in INPUTS:
        source_field, specs_field, contract_field = SUMMARY_IDENTITY_FIELDS[name]
        try:
            observed_source = validate_identity(summaries[name].get(source_field))
            observed_specs = validate_identity(summaries[name].get(specs_field))
            observed_contract = validate_specs_contract_identity(
                summaries[name].get(contract_field)
            )
        except SourceIdentityError as error:
            raise ManifestError(f"{name} candidate identity is invalid: {error}") from error
        if observed_source != current_source:
            raise ManifestError(f"{name} does not use the current ManyForge candidate")
        if observed_contract != current_contract:
            raise ManifestError(f"{name} does not use the current Specs contract")
        if common_specs is None:
            common_specs = observed_specs
        elif observed_specs != common_specs:
            raise ManifestError("the five campaigns do not share one full Specs source identity")
    assert common_specs is not None
    if (
        common_specs["head_commit"] != current_contract["head_commit"]
        or common_specs["head_tree"] != current_contract["head_tree"]
    ):
        raise ManifestError("campaign Specs source and closure contract have different Git bases")
    return common_specs


def generate(args: argparse.Namespace) -> dict[str, Any]:
    manyforge_root = args.manyforge_root.resolve()
    specs_root = args.specs_root.resolve()
    if not manyforge_root.is_dir() or not specs_root.is_dir():
        raise ManifestError("ManyForge and Specs roots must be existing directories")
    generator = Path(__file__).resolve()
    verifier = generator.with_name("verify_step18_closure.py")
    helper = generator.with_name("source_identity.py")
    tracker = FileTracker()
    for path, label in (
        (generator, "closure manifest generator"),
        (verifier, "closure verifier"),
        (helper, "source identity helper"),
    ):
        tracker.read(path, label)
    try:
        manyforge_identity = source_identity(manyforge_root)
        closure_specs_identity = source_identity(specs_root)
        contract_identity = specs_contract_identity(specs_root)
    except SourceIdentityError as error:
        raise ManifestError(f"source identity failed: {error}") from error

    argument_paths = {
        "offline_proof": args.offline_proof,
        "scheduled_runtime": args.scheduled_runtime,
        "live_contact_modes": args.live_contact_modes,
        "loaded_kernel": args.loaded_kernel,
        "release_gates": args.release_gates,
    }
    summaries: dict[str, dict[str, Any]] = {}
    summary_paths: dict[str, Path] = {}
    inputs: dict[str, dict[str, str]] = {}
    for name in INPUTS:
        path = argument_paths[name].resolve()
        value, digest = _json_file(path, f"{name} summary", tracker)
        summaries[name] = value
        summary_paths[name] = path
        inputs[name] = {"path": str(path), "sha256": digest}
    paths = [value["path"] for value in inputs.values()]
    if len(set(paths)) != len(paths):
        raise ManifestError("each closure subverification must be a distinct file")

    campaign_specs_identity = _validated_summary_identities(
        summaries, manyforge_identity, contract_identity
    )
    invocations = build_producer_invocations(
        summaries=summaries,
        summary_paths=summary_paths,
        manyforge_root=manyforge_root,
        tracker=tracker,
    )

    evidence_path = args.evidence_record.resolve()
    canonical_evidence = (specs_root / SPECS_CONTRACT_EXCLUDED_PATH).resolve()
    if evidence_path != canonical_evidence:
        raise ManifestError(
            f"evidence record must be the canonical excluded path {canonical_evidence}"
        )
    evidence_payload, evidence_digest = tracker.read(
        evidence_path, "Step 18 evidence record"
    )
    evidence_claim = parse_evidence_record(evidence_payload)
    if evidence_claim["closure_verifier"] != {
        "path": "scripts/qualification/verify_step18_closure.py",
        "sha256": tracker.digests[verifier],
    }:
        raise ManifestError("evidence record does not bind the exact closure verifier")
    for name in INPUTS:
        if evidence_claim["subgates"][name]["summary_sha256"] != inputs[name]["sha256"]:
            raise ManifestError(f"evidence record does not bind the exact {name} summary")

    tooling = {}
    for name, path in (
        ("generator", generator),
        ("verifier", verifier),
        ("source_identity_helper", helper),
    ):
        tooling[name] = {"path": str(path), "sha256": tracker.digests[path]}

    tracker.assert_unchanged()
    try:
        if source_identity(manyforge_root) != manyforge_identity:
            raise ManifestError("ManyForge source changed during closure manifest generation")
        if source_identity(specs_root) != closure_specs_identity:
            raise ManifestError("Specs source changed during closure manifest generation")
        if specs_contract_identity(specs_root) != contract_identity:
            raise ManifestError("Specs contract changed during closure manifest generation")
    except SourceIdentityError as error:
        raise ManifestError(f"final source identity failed: {error}") from error

    return {
        "schema_version": SCHEMA,
        "manyforge_root": str(manyforge_root),
        "specs_root": str(specs_root),
        "candidate_source_identity": manyforge_identity,
        "campaign_specs_source_identity": campaign_specs_identity,
        "closure_specs_source_identity": closure_specs_identity,
        "candidate_specs_contract_identity": contract_identity,
        "evidence_record": {
            "path": str(evidence_path),
            "sha256": evidence_digest,
            "schema_version": EVIDENCE_SCHEMA,
        },
        "inputs": inputs,
        "producer_invocations": invocations,
        "tooling": tooling,
    }


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manyforge-root", type=Path, required=True)
    parser.add_argument("--specs-root", type=Path, required=True)
    parser.add_argument("--offline-proof", type=Path, required=True)
    parser.add_argument("--scheduled-runtime", type=Path, required=True)
    parser.add_argument("--live-contact-modes", type=Path, required=True)
    parser.add_argument("--loaded-kernel", type=Path, required=True)
    parser.add_argument("--release-gates", type=Path, required=True)
    parser.add_argument("--evidence-record", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    temporary: Path | None = None
    try:
        output = ensure_ignored_output(args.output, args.manyforge_root.resolve())
        if output.exists():
            raise ManifestError(f"refusing to overwrite closure manifest: {output}")
        manifest = generate(args)
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = output.with_name(f".{output.name}.tmp-{os.getpid()}")
        if temporary.exists():
            raise ManifestError(
                f"closure manifest temporary output already exists: {temporary}"
            )
        with temporary.open("x", encoding="utf-8") as stream:
            json.dump(manifest, stream, indent=2, sort_keys=True, allow_nan=False)
            stream.write("\n")
            stream.flush()
            os.fsync(stream.fileno())
        # Revalidate every input and source identity before making the result visible.
        if generate(args) != manifest:
            raise ManifestError("closure inputs changed while writing the manifest")
        os.link(temporary, output)
        try:
            temporary.unlink()
        except OSError:
            pass
        temporary = None
    except (OSError, ManifestError) as error:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass
        print(f"Step 18 closure manifest generation failed: {error}", file=sys.stderr)
        return 2
    print(json.dumps({"generated": True, "inputs": len(INPUTS)}))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

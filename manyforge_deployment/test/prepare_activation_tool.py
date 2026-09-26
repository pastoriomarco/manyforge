#!/usr/bin/env python3
"""Focused black-box qualification for the exact activation preparer."""

from __future__ import annotations

import argparse
import base64
import copy
import json
from pathlib import Path
import subprocess
import tempfile


MAX_ACTIVATION_PREPARATION_REQUEST_BYTES = 34 * 1024 * 1024
MAX_ACTIVATION_PREPARATION_OUTPUT_BYTES = 128 * 1024 * 1024

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    load_strict_json_yaml,
    raw_sha256_digest,
)
from manyforge_behavior.model_source_capture import capture_deployment_model_sources
from manyforge_behavior.model_source_preparation import prepare_captured_model_source


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--negative-matrix", action="store_true")
    parser.add_argument("--intent-rejections", action="store_true")
    parser.add_argument("--source-closure-rejections", action="store_true")
    return parser.parse_args()


def _program(workspace: Path, program_id: str) -> dict[str, object]:
    matches = []
    for path in sorted((workspace / "examples").glob("*.program.yaml")):
        value = load_strict_json_yaml(path)
        if value.get("program_id") == program_id:
            matches.append(value)
    if len(matches) != 1:
        raise AssertionError(
            f"expected one maintained Program for {program_id}; found {len(matches)}"
        )
    return matches[0]


def _request(
    scenario_path: Path,
    document: dict[str, object],
    program: dict[str, object],
    *,
    source_digest: str | None = None,
) -> dict[str, object]:
    member_digests = {
        name: raw_sha256_digest((scenario_path / name).read_bytes())
        for name in (
            "assignment.yaml",
            "deployment.yaml",
            "overrides.yaml",
            "program.yaml",
        )
    }
    digest = source_digest or raw_sha256_digest(
        canonical_json_bytes(
            {
                "manifest": raw_sha256_digest(
                    (scenario_path / "scenario.yaml").read_bytes()
                ),
                "members": member_digests,
            }
        )
    )
    captured = {
        "scenario_id": document["scenario_id"],
        "source_document_digest": digest,
        "deployment": document["deployment_draft"],
        "model_source_closure": capture_deployment_model_sources(
            document["deployment_draft"], project_root=scenario_path
        ),
        "program": program,
        "program_role_assignment": document["program_role_assignment"],
        "typed_parameter_overrides": document["typed_parameter_overrides"],
    }
    request = {
        "schema_version": "manyforge.activation_preparation_request.v1",
        **captured,
        "captured_input_digest": raw_sha256_digest(canonical_json_bytes(captured)),
    }
    profiles = load_strict_json_yaml(
        scenario_path.parents[1]
        / "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
    )
    families = {
        row["model_family_id"]: (
            row["model_asset_id"],
            row["source"]["accepted_package_uri_prefix"],
        )
        for row in profiles["model_families"]
    }
    profile_models = {
        row["runtime_restoration_profile_id"]: families[row["model_family_id"]]
        for row in profiles["runtime_restoration_profiles"]
    }
    sources = {
        row["component_id"]: row
        for row in captured["model_source_closure"]["components"]
    }
    request["prepared_model_closure"] = {
        "schema_version": "manyforge.activation_prepared_model_closure.v1",
        "components": sorted(
            (
                prepare_captured_model_source(
                    sources[component["component_id"]],
                    model_asset_id=profile_models[component["component_profile_id"]][0],
                    accepted_package_uri_prefix=profile_models[
                        component["component_profile_id"]
                    ][1],
                )
                for component in captured["deployment"]["components"]
            ),
            key=lambda row: row["component_id"],
        ),
    }
    return request


def _scenario_request(
    workspace: Path, scenario: str
) -> tuple[Path, dict[str, object], dict[str, object]]:
    scenario_path = (workspace / scenario).resolve()
    manifest = load_strict_json_yaml(scenario_path / "scenario.yaml")
    document: dict[str, object] = {
        "scenario_id": manifest["scenario_id"],
        "deployment_draft": load_strict_json_yaml(
            scenario_path / "deployment.yaml"
        ),
        "program_draft": load_strict_json_yaml(scenario_path / "program.yaml"),
        "program_role_assignment": load_strict_json_yaml(
            scenario_path / "assignment.yaml"
        ),
        "typed_parameter_overrides": load_strict_json_yaml(
            scenario_path / "overrides.yaml"
        ),
    }
    if manifest.get("api_version") != "manyforge.scenario.project.v1":
        raise AssertionError("test input is not a Scenario project")
    program_id = document["program_draft"]["program_id"]
    return scenario_path, document, _request(
        scenario_path, document, _program(workspace, program_id)
    )


def _native_host_identity(prepared: dict[str, object]) -> str:
    bundle = prepared["activation_payload"]["retained_bundle"]
    runtime_units = bundle["cell_graph"]["runtime_units"]
    if len(runtime_units) != 1:
        raise AssertionError("prepared Deployment does not have one native host")
    runtime = runtime_units[0]
    if runtime["runtime_profile_id"] != "manyforge.native_execution_runtime":
        raise AssertionError("prepared Deployment selected a non-native runtime")
    return runtime["runtime_unit_id"]


def _native_host_products(prepared: dict[str, object]) -> object:
    bundle = prepared["activation_payload"]["retained_bundle"]
    products = bundle["compiled_products"]["runtime_unit_products"]
    if len(products) != 1:
        raise AssertionError("prepared Deployment has no exact native host product")
    return products[0]


def _require_native_canonical_empty_configuration(
    activation_root: Path, candidate_bundle: dict[str, object]
) -> None:
    candidate_graph = candidate_bundle.get("cell_graph")
    if not isinstance(candidate_graph, dict):
        raise AssertionError("prepared candidate has no retained cell graph")
    cell_id = candidate_graph.get("cell_id")
    if not isinstance(cell_id, str) or not cell_id:
        raise AssertionError("prepared candidate has no exact CellId")
    admission_path = activation_root / f"canonical-empty-admission.{cell_id}.json"
    if not admission_path.is_file():
        raise AssertionError("preparer omitted its canonical-empty admission")
    admission = json.loads(admission_path.read_text(encoding="utf-8"))
    retained_bundle_id = admission.get("retained_bundle_id")
    if (
        not isinstance(retained_bundle_id, str)
        or not retained_bundle_id.startswith("sha256:")
    ):
        raise AssertionError("canonical-empty admission has no bundle identity")
    empty_bundle_path = (
        activation_root
        / "retained-bundles"
        / "sha256"
        / f"{retained_bundle_id.removeprefix('sha256:')}.json"
    )
    if not empty_bundle_path.is_file():
        raise AssertionError("preparer omitted its canonical-empty retained bundle")
    empty_bundle = json.loads(empty_bundle_path.read_text(encoding="utf-8"))
    empty_graph = empty_bundle.get("cell_graph")
    if not isinstance(empty_graph, dict):
        raise AssertionError("canonical-empty bundle has no cell graph")
    candidate_runtime_units = candidate_graph.get("runtime_units")
    empty_runtime_units = empty_graph.get("runtime_units")
    if (
        not isinstance(candidate_runtime_units, list)
        or len(candidate_runtime_units) != 1
        or not isinstance(empty_runtime_units, list)
        or len(empty_runtime_units) != 1
    ):
        raise AssertionError("native bootstrap does not retain exactly one host")
    candidate_host = candidate_runtime_units[0]
    empty_host = empty_runtime_units[0]
    if (
        not isinstance(candidate_host, dict)
        or not isinstance(empty_host, dict)
        or candidate_host.get("runtime_profile_id")
        != "manyforge.native_execution_runtime"
    ):
        raise AssertionError("canonical-empty regression requires a native host")
    for field in (
        "runtime_unit_id",
        "runtime_profile_id",
        "runtime_profile_revision",
        "dependency_runtime_unit_ids",
    ):
        if candidate_host.get(field) != empty_host.get(field):
            raise AssertionError(
                f"canonical-empty host changed infrastructure field {field}"
            )
    candidate_products = candidate_bundle.get("compiled_products")
    empty_products = empty_bundle.get("compiled_products")
    if (
        not isinstance(candidate_products, dict)
        or not isinstance(empty_products, dict)
        or candidate_products.get("runtime_unit_products")
        != empty_products.get("runtime_unit_products")
    ):
        raise AssertionError("canonical-empty host product differs from candidate")
    candidate_configuration = candidate_host.get("configuration")
    empty_configuration = empty_host.get("configuration")
    if (
        not isinstance(candidate_configuration, dict)
        or not isinstance(empty_configuration, dict)
    ):
        raise AssertionError("native runtime configuration is not an object")
    for field in (
        "mode",
        "planner_rng_seed",
        "execution_tick_hz",
        "collision_monitor_period_ms",
    ):
        if candidate_configuration.get(field) != empty_configuration.get(field):
            raise AssertionError(
                f"canonical-empty host did not retain scalar setting {field}"
            )
    for field in (
        "initial_joint_positions_rad",
        "motion_target_limits",
        "direct_control_joint_limits",
        "gripper_target_configurations",
    ):
        if empty_configuration.get(field) != []:
            raise AssertionError(
                f"canonical-empty host retained controller setting {field}"
            )


def _run(
    args: argparse.Namespace,
    request: dict[str, object],
    root: Path,
    *,
    expect_error: str | None = None,
    expected_program_outcome_kind: str = "bound",
    publish_canonical_empty: bool = False,
) -> dict[str, object] | None:
    root.mkdir()
    activation_root = root / "activation"
    activation_root.mkdir()
    request_path = root / "request.json"
    request_path.write_bytes(canonical_json_bytes(request))
    command = [
        str(args.tool),
        "--input", str(request_path),
        "--capability-root", str(args.workspace),
        "--activation-root", str(activation_root),
    ]
    if publish_canonical_empty:
        command.append("--publish-canonical-empty")
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if expect_error is not None:
        if result.returncode == 0 or expect_error not in result.stderr:
            raise AssertionError(
                f"expected failure containing {expect_error!r}; "
                f"return={result.returncode}, stderr={result.stderr!r}"
            )
        return None
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    if len(result.stdout.encode("utf-8")) > MAX_ACTIVATION_PREPARATION_OUTPUT_BYTES:
        raise AssertionError("preparer exceeded the direct-transfer output bound")
    value = json.loads(result.stdout)
    if value["source_document_digest"] != request["source_document_digest"]:
        raise AssertionError("preparer changed the source-document identity")
    if value["captured_input_digest"] != request["captured_input_digest"]:
        raise AssertionError("preparer changed the captured-input identity")
    if value.get("schema_version") != "manyforge.prepared_activation_input.v3":
        raise AssertionError("preparer did not return the direct v3 handoff")
    program_outcome = value.get("program_outcome")
    if (
        not isinstance(program_outcome, dict)
        or program_outcome.get("kind") != expected_program_outcome_kind
    ):
        raise AssertionError(
            "preparer returned an unexpected Program outcome: "
            f"{program_outcome!r}"
        )
    if expected_program_outcome_kind == "non_executable":
        outcome = program_outcome.get("outcome")
        unresolved = outcome.get("unresolved_roles") if isinstance(outcome, dict) else None
        if not isinstance(unresolved, list) or not any(
            isinstance(row, dict)
            and row.get("command_target_id") == "foreign_target"
            and row.get("reason") == "target_missing"
            for row in unresolved
        ):
            raise AssertionError(
                "non-executable outcome did not name the missing target"
            )
    payload = value.get("activation_payload")
    if not isinstance(payload, dict) or set(payload) != {
        "catalog_artifacts",
        "program",
        "program_binding",
        "program_role_assignment",
        "raw_artifacts",
        "retained_bundle",
    }:
        raise AssertionError("preparer returned an incomplete direct payload")
    if not payload["raw_artifacts"] or not payload["catalog_artifacts"]:
        raise AssertionError("preparer omitted its exact artifact closure")
    bundle = payload["retained_bundle"]
    policies = bundle["retained_policy_records"]
    if len(policies) != 1 or policies[0]["record_kind"] != "runtime_policy":
        raise AssertionError("motion bundle has no single verified runtime policy")
    policy_ref = policies[0]
    if policy_ref not in bundle["catalogs"]["records"]:
        raise AssertionError("runtime policy is not in the exact catalog closure")
    artifacts = [
        artifact for artifact in payload["catalog_artifacts"]
        if artifact["content_addressed_store_key"]
        == policy_ref["content_addressed_store_key"]
    ]
    if (
        len(artifacts) != 1
        or artifacts[0]["content_digest"] != policy_ref["content_digest"]
    ):
        raise AssertionError("runtime policy bytes are missing from the handoff")
    policy_bytes = base64.b64decode(
        "".join(artifacts[0]["chunks"]), validate=True
    )
    if (
        len(policy_bytes) != artifacts[0]["byte_size"]
        or raw_sha256_digest(policy_bytes) != policy_ref["content_digest"]
    ):
        raise AssertionError("prepared runtime policy has incorrect byte identity")
    policy = json.loads(policy_bytes)
    if (
        policy["runtime_policy_id"] != request["deployment"]["deployment_artifact_id"]
        or policy["runtime_policy_revision"] != policy_ref["record_revision"]
        or policy["interrupt_policy_ids"] != []
        or policy["open_loop_grasp_acknowledgements"] != []
    ):
        raise AssertionError("prepared runtime policy is not restrictive and exact")
    if not any(activation_root.iterdir()):
        raise AssertionError("preparer omitted the activation closure")
    if publish_canonical_empty:
        _require_native_canonical_empty_configuration(activation_root, bundle)
    return value


def _require_oversized_request_rejection(
    args: argparse.Namespace, root: Path
) -> None:
    root.mkdir()
    activation_root = root / "activation"
    activation_root.mkdir()
    request_path = root / "request.json"
    with request_path.open("wb") as stream:
        stream.truncate(MAX_ACTIVATION_PREPARATION_REQUEST_BYTES + 1)
    command = [
        str(args.tool),
        "--input",
        str(request_path),
        "--capability-root",
        str(args.workspace),
        "--activation-root",
        str(activation_root),
    ]
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if (
        result.returncode == 0
        or "preparation input violates its byte bound" not in result.stderr
    ):
        raise AssertionError(
            "native preparer accepted an oversized encoded request: "
            f"return={result.returncode}, stderr={result.stderr!r}"
        )


def _rehash_request(request: dict[str, object]) -> None:
    captured = {
        key: value
        for key, value in request.items()
        if key not in {
            "schema_version",
            "captured_input_digest",
            "prepared_model_closure",
        }
    }
    request["captured_input_digest"] = raw_sha256_digest(
        canonical_json_bytes(captured)
    )


def main() -> int:
    args = _arguments()
    workspace = args.workspace.resolve()
    scenario_path, document, request = _scenario_request(workspace, args.scenario)
    with tempfile.TemporaryDirectory(prefix="manyforge-prepare-test-") as raw:
        temporary = Path(raw)
        if args.intent_rejections:
            unsupported_version = copy.deepcopy(request)
            unsupported_version["deployment"]["api_version"] = (
                "manyforge.deployment_intent.v999"
            )
            _rehash_request(unsupported_version)
            _run(
                args,
                unsupported_version,
                temporary / "unsupported-intent-version",
                expect_error="unsupported api_version",
            )

            source_mismatch = copy.deepcopy(request)
            source_mismatch["deployment"]["components"][0]["component_id"] = (
                "different_component"
            )
            _rehash_request(source_mismatch)
            _run(
                args,
                source_mismatch,
                temporary / "source-mismatch",
                expect_error=(
                    "component identities differ from the captured source closure"
                ),
            )

            duplicate_component = copy.deepcopy(request)
            components = duplicate_component["deployment"]["components"]
            if len(components) < 2:
                raise AssertionError("intent rejection test requires two components")
            components[1]["component_id"] = components[0]["component_id"]
            _rehash_request(duplicate_component)
            _run(
                args,
                duplicate_component,
                temporary / "duplicate-component",
                expect_error="components are not uniquely canonical-sorted",
            )

            invalid_assistant = copy.deepcopy(request)
            invalid_assistant["deployment"]["assistant_profile_id"] = "Not Portable"
            _rehash_request(invalid_assistant)
            _run(
                args,
                invalid_assistant,
                temporary / "invalid-assistant-profile",
                expect_error="must be a portable identifier",
            )

            unknown_profile = copy.deepcopy(request)
            unknown_profile["deployment"]["components"][0][
                "component_profile_id"
            ] = "unknown_profile"
            _rehash_request(unknown_profile)
            _run(
                args,
                unknown_profile,
                temporary / "unknown-component-profile",
                expect_error="selects unknown profile unknown_profile",
            )

            mismatched_runtime_kind = copy.deepcopy(request)
            mismatched_runtime_kind["deployment"]["components"][0][
                "configuration"
            ]["runtime_kind"] = "ros2_control"
            _rehash_request(mismatched_runtime_kind)
            _run(
                args,
                mismatched_runtime_kind,
                temporary / "mismatched-runtime-kind",
                expect_error="ros2_control runtime configuration for native profile",
            )

            invalid_policies = copy.deepcopy(request)
            invalid_policies["deployment"]["policies"] = []
            _rehash_request(invalid_policies)
            _run(
                args,
                invalid_policies,
                temporary / "invalid-policies",
                expect_error="Deployment intent policies must be an object",
            )
            return 0

        if args.source_closure_rejections:
            _require_oversized_request_rejection(
                args, temporary / "oversized-request"
            )
            invalid_base64 = copy.deepcopy(request)
            invalid_base64["model_source_closure"]["components"][0][
                "entrypoints"
            ]["urdf"]["chunks"][0] = "!!!!"
            _rehash_request(invalid_base64)
            _run(
                args,
                invalid_base64,
                temporary / "invalid-base64",
                expect_error="is not canonical base64",
            )

            wrong_digest = copy.deepcopy(request)
            wrong_digest["model_source_closure"]["components"][0][
                "entrypoints"
            ]["urdf"]["content_digest"] = "sha256:" + "f" * 64
            _rehash_request(wrong_digest)
            _run(
                args,
                wrong_digest,
                temporary / "wrong-source-digest",
                expect_error="does not match its content digest",
            )

            missing_component = copy.deepcopy(request)
            missing_component["model_source_closure"]["components"].pop()
            _rehash_request(missing_component)
            _run(
                args,
                missing_component,
                temporary / "missing-component",
                expect_error=(
                    "Deployment intent component count differs from the captured "
                    "source closure"
                ),
            )
            return 0

        prepared = _run(
            args,
            request,
            temporary / "valid",
            publish_canonical_empty=True,
        )
        assert prepared is not None
        if not args.negative_matrix:
            if len(request["deployment"]["components"]) == 1:
                dual_path = "examples/ur5e_lite6_wall_grouped.scenario"
                _, _, dual_request = _scenario_request(workspace, dual_path)
                if len(dual_request["deployment"]["components"]) != 2:
                    raise AssertionError(
                        "stable-host regression requires the maintained dual fixture"
                    )
                # Re-seal the dual authored candidate under the single fixture's
                # CellId so both products describe the same native host scope.
                dual_request["deployment"]["cell_id"] = request["deployment"][
                    "cell_id"
                ]
                _rehash_request(dual_request)
                dual_prepared = _run(
                    args,
                    dual_request,
                    temporary / "stable-host-dual",
                    publish_canonical_empty=True,
                )
                assert dual_prepared is not None
                single_host_id = _native_host_identity(prepared)
                dual_host_id = _native_host_identity(dual_prepared)
                expected_host_id = "robot_cell.native_execution"
                if (
                    single_host_id != expected_host_id
                    or dual_host_id != expected_host_id
                ):
                    raise AssertionError(
                        "native host identity changed with component membership: "
                        f"single={single_host_id!r}, dual={dual_host_id!r}"
                    )
                if _native_host_products(prepared) != _native_host_products(
                    dual_prepared
                ):
                    raise AssertionError(
                        "native host product changed with component membership"
                    )
            changed = _request(
                scenario_path,
                document,
                _program(workspace, document["program_draft"]["program_id"]),
                source_digest="sha256:" + "f" * 64,
            )
            changed_result = _run(args, changed, temporary / "changed")
            assert changed_result is not None
            if changed_result["prepared_activation_id"] == prepared["prepared_activation_id"]:
                raise AssertionError("prepared identity ignored captured source identity")
            return 0

        missing = copy.deepcopy(request)
        del missing["program_role_assignment"]
        _run(
            args,
            missing,
            temporary / "missing",
            expect_error="program_role_assignment",
        )

        duplicate = copy.deepcopy(request)
        rows = duplicate["program_role_assignment"]["component_roles"]
        if len(rows) < 2:
            raise AssertionError("negative matrix requires two component roles")
        rows[1]["component_role_id"] = rows[0]["component_role_id"]
        _rehash_request(duplicate)
        _run(
            args,
            duplicate,
            temporary / "duplicate",
            expect_error="duplicate component role assignment",
        )

        mispartitioned_initial_state = copy.deepcopy(request)
        components = mispartitioned_initial_state["deployment"]["components"]
        if len(components) != 2:
            raise AssertionError(
                "initial-state ownership regression requires two components"
            )
        runtime_configurations = [
            component["configuration"]["runtime_configuration"]
            for component in components
        ]
        positions = [
            runtime_configuration["initial_joint_positions_rad"]
            for runtime_configuration in runtime_configurations
        ]
        if any(not isinstance(row, list) or len(row) != 6 for row in positions):
            raise AssertionError(
                "initial-state ownership regression requires two six-resource arms"
            )
        positions[0].append(0.25)
        positions[1].pop()
        _rehash_request(mispartitioned_initial_state)
        _run(
            args,
            mispartitioned_initial_state,
            temporary / "mispartitioned-initial-state",
            expect_error=(
                "lite6_robot initial_joint_positions_rad count does not match "
                "its arm resource bindings"
            ),
        )

        foreign = copy.deepcopy(request)
        foreign["program_role_assignment"]["target_roles"][0][
            "command_target_id"
        ] = "foreign_target"
        _rehash_request(foreign)
        _run(
            args,
            foreign,
            temporary / "foreign",
            expected_program_outcome_kind="non_executable",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

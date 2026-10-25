#!/usr/bin/env python3
"""Focused black-box qualification for the exact activation preparer."""

from __future__ import annotations

import argparse
import base64
import copy
import json
import shutil
import sys
from pathlib import Path
import subprocess
import tempfile


MAX_ACTIVATION_PREPARATION_REQUEST_BYTES = 34 * 1024 * 1024
# The compact receipt bound of Headless's ActivationPreparer.
MAX_ACTIVATION_PREPARATION_OUTPUT_BYTES = 1024 * 1024

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
    load_strict_json_yaml,
    raw_sha256_digest,
)
from manyforge_behavior.catalog import NodeCatalog
from manyforge_behavior.model_source_capture import (
    MAX_XML_BYTES,
    capture_deployment_model_sources,
    decode_model_source_artifact,
    encode_model_source_artifact,
)
from manyforge_behavior.model_source_preparation import prepare_captured_model_source
from manyforge_composer.backend.authoring_forest import derive_candidate
from manyforge_headless_api.activation_preparer import (
    _model_preparation_profiles,
    _prepare_model_closure,
)


def _arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--tool", required=True, type=Path)
    parser.add_argument("--workspace", required=True, type=Path)
    parser.add_argument("--scenario", required=True, type=Path)
    parser.add_argument("--negative-matrix", action="store_true")
    parser.add_argument("--intent-rejections", action="store_true")
    parser.add_argument("--stop-limit-names", action="store_true")
    parser.add_argument("--source-closure-rejections", action="store_true")
    parser.add_argument("--runtime-identity", action="store_true")
    parser.add_argument("--takeover-budget", action="store_true")
    parser.add_argument("--no-program", action="store_true")
    parser.add_argument("--ros2-control", action="store_true")
    parser.add_argument("--manager-ros-apply-fixture-root", type=Path)
    parser.add_argument("--manager-isaac-apply-fixture-root", type=Path)
    return parser.parse_args()


def _program(workspace: Path, authoring: dict[str, object]) -> dict[str, object]:
    catalog = NodeCatalog(workspace / "manyforge_behavior/resources/node_catalog.yaml")
    program, diagnostics = derive_candidate(authoring, catalog)
    if diagnostics or program is None:
        raise AssertionError(
            f"expected one executable maintained Program draft; diagnostics={diagnostics!r}"
        )
    return program.to_dict()


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
        # The Scenario project folder: a fixed normalized absolute fixture
        # path (native never reads it); part of captured_input_digest.
        "project_path": f"/tmp/manyforge-test/{document['scenario_id']}.scenario",
        "source_document_digest": digest,
        "deployment": document["deployment_draft"],
        "model_source_closure": capture_deployment_model_sources(
            document["deployment_draft"], project_root=scenario_path
        ),
        "program": program,
        "program_role_assignment": document["program_role_assignment"],
    }
    request = {
        "schema_version": "manyforge.activation_preparation_request.v3",
        "runtime_cell_id": "activation_test_runtime",
        **captured,
        "captured_input_digest": raw_sha256_digest(canonical_json_bytes(captured)),
    }
    request["prepared_model_closure"] = _prepare_model_closure(
        deployment=captured["deployment"],
        model_source_closure=captured["model_source_closure"],
        profiles=_model_preparation_profiles(scenario_path.parents[1]),
    )
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
    }
    if manifest.get("api_version") != "manyforge.scenario.project.v2":
        raise AssertionError("test input is not a Scenario project")
    return scenario_path, document, _request(
        scenario_path, document, _program(workspace, document["program_draft"])
    )


def _native_host_identity(prepared: dict[str, object]) -> str:
    bundle = prepared["activation_payload"]["retained_bundle"]
    runtime_units = bundle["cell_graph"]["runtime_units"]
    if len(runtime_units) != 1:
        raise AssertionError("prepared Deployment does not have one execution host")
    runtime = runtime_units[0]
    # Q63-Q66: offline Scenarios run on the mock ROS lane; an empty
    # Deployment keeps the canonical native host.
    if runtime["runtime_profile_id"] not in {
        "manyforge.native_execution_runtime",
        "manyforge.ros2_control_execution_runtime",
    }:
        raise AssertionError("prepared Deployment selected an unknown runtime")
    return runtime["runtime_unit_id"]


def _native_host_products(prepared: dict[str, object]) -> object:
    bundle = prepared["activation_payload"]["retained_bundle"]
    products = bundle["compiled_products"]["runtime_unit_products"]
    if len(products) != 1:
        raise AssertionError("prepared Deployment has no exact native host product")
    return products[0]


def _require_canonical_empty_configuration(
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
    assert admission_path.read_bytes() == canonical_json_bytes(admission)
    assert admission["admission_digest"] == compute_content_identity(
        admission, identity_domain="canonical-empty-admission",
        api_version=admission["api_version"], own_field="admission_digest",
    )
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
        or candidate_host.get("runtime_profile_id") not in {
            "manyforge.native_execution_runtime",
            "manyforge.ros2_control_execution_runtime",
        }
    ):
        raise AssertionError("canonical-empty regression requires a qualified execution host")
    for field in ("model_instances", "controllable_components", "control_domains",
                  "control_bindings", "component_attachments"):
        assert empty_graph[field] == [], field
    for field in ("desired_control_configurations", "capability_provider_bindings"):
        assert empty_bundle[field] == [], field
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
    if candidate_host["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime":
        assert empty_host == candidate_host
        return
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


def _check_sealed_preparation(args: argparse.Namespace, package: Path, output: Path) -> None:
    # This is the actual package produced and reloaded by the preceding worker.
    admission_paths = list(package.glob("canonical-empty-admission.*.json"))
    assert len(admission_paths) == 1
    admission_path = admission_paths[0]
    admission = json.loads(admission_path.read_bytes())
    origin = {"kind": "sealed_canonical_empty", **{name: admission[name] for name in (
        "cell_id", "deployment_artifact_id", "deployment_revision",
        "retained_bundle_id", "graph_revision", "admission_digest")}}
    capture = raw_sha256_digest(canonical_json_bytes({
        "schema_version": "manyforge.sealed_canonical_empty_capture.v1", "origin": origin}))
    request = {"schema_version": "manyforge.sealed_canonical_empty_preparation_request.v1",
               "runtime_cell_id": admission["cell_id"], "origin": origin,
               "captured_input_digest": capture}
    output.mkdir()
    path = output / "request.json"
    path.write_bytes(canonical_json_bytes(request))
    command = [str(args.tool), "--input", str(path), "--capability-root", str(args.workspace),
               "--activation-root", str(output)]
    missing = subprocess.run(command, capture_output=True, text=True)
    assert missing.returncode != 0 and "admission is unavailable" in missing.stderr
    command += ["--canonical-empty-package-root", str(package),
                "--canonical-empty-admission-filename", admission_path.name]
    result = subprocess.run(command, capture_output=True, text=True)
    assert result.returncode == 0, result.stderr
    receipt = json.loads(result.stdout)
    assert len(receipt) == 13 and receipt["schema_version"] == "manyforge.prepared_activation_input.v6"
    assert receipt["origin"] == origin and receipt["captured_input_digest"] == capture
    assert "scenario_id" not in receipt and "source_document_digest" not in receipt
    prepared = _stored_product(output, receipt)
    payload = prepared["activation_payload"]
    assert receipt["program_outcome"] == {"kind": "no_program"}
    assert receipt["program_id"] is None and receipt["program_revision"] is None
    assert payload["program"] is None and payload["program_binding"] is None
    assert payload["program_role_assignment"] == {"component_roles": [], "target_roles": []}
    bundle_path = package / "retained-bundles" / "sha256" / (admission["retained_bundle_id"].removeprefix("sha256:") + ".json")
    assert payload["retained_bundle"] == json.loads(bundle_path.read_bytes())
    assert not payload["retained_bundle"]["cell_graph"]["controllable_components"]
    assert _native_host_products(prepared)
    replay = subprocess.run(command, capture_output=True, text=True)
    assert replay.returncode == 0 and json.loads(replay.stdout) == receipt, replay.stderr
    # A rederived capture cannot turn a foreign admission selection into proof.
    forged = copy.deepcopy(request)
    forged["origin"]["admission_digest"] = "sha256:" + "f" * 64
    forged["captured_input_digest"] = raw_sha256_digest(canonical_json_bytes({
        "schema_version": "manyforge.sealed_canonical_empty_capture.v1", "origin": forged["origin"]}))
    path.write_bytes(canonical_json_bytes(forged))
    rejected = subprocess.run(command, capture_output=True, text=True)
    assert rejected.returncode != 0 and "differs from actual admission" in rejected.stderr
    mixed = copy.deepcopy(request)
    mixed["origin"]["scenario_id"] = None
    path.write_bytes(canonical_json_bytes(mixed))
    rejected = subprocess.run(command, capture_output=True, text=True)
    assert rejected.returncode != 0 and "unexpected field scenario_id" in rejected.stderr


def _run(
    args: argparse.Namespace,
    request: dict[str, object],
    root: Path,
    *,
    expect_error: str | None = None,
    expected_program_outcome_kind: str = "bound",
    publish_canonical_empty: bool = False,
    reuse_root: bool = False,
) -> dict[str, object] | None:
    root.mkdir(exist_ok=reuse_root)
    activation_root = root / "activation"
    activation_root.mkdir(exist_ok=reuse_root)
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
        raise AssertionError("preparer exceeded the compact receipt bound")
    receipt = json.loads(result.stdout)
    value = _stored_product(activation_root, receipt)
    if value["origin"]["source_document_digest"] != request["source_document_digest"]:
        raise AssertionError("preparer changed the source-document identity")
    if value["captured_input_digest"] != request["captured_input_digest"]:
        raise AssertionError("preparer changed the captured-input identity")
    if value.get("schema_version") != "manyforge.prepared_activation_input.v6":
        raise AssertionError("preparer did not return the v6 prepared activation")
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
        raise AssertionError("preparer stored an incomplete payload")
    if not payload["raw_artifacts"] or not payload["catalog_artifacts"]:
        raise AssertionError("preparer omitted its exact artifact closure")
    bundle = payload["retained_bundle"]
    if expected_program_outcome_kind == "no_program":
        assert program_outcome == {"kind": "no_program"}
        assert value["program_id"] is None and value["program_revision"] is None
        assert payload["program"] is None and payload["program_binding"] is None
        if not request["deployment"]["components"]:
            assert not bundle["cell_graph"]["controllable_components"]
            assert not bundle["cell_graph"]["model_instances"]
            assert _native_host_identity(value)
            return value
    if bundle["cell_graph"]["cell_id"] != request["runtime_cell_id"]:
        raise AssertionError("candidate did not use the configured runtime identity")
    if bundle["retained_policy_records"]:
        raise AssertionError("motion bundle retains an unexpected policy record")
    if not any(activation_root.iterdir()):
        raise AssertionError("preparer omitted the activation closure")
    if publish_canonical_empty:
        _require_canonical_empty_configuration(activation_root, bundle)
    return value


def _stored_product(
    activation_root: Path, receipt: dict[str, object]
) -> dict[str, object]:
    """Read the stored product the compact receipt names and bind the two."""

    if not isinstance(receipt, dict) or "activation_payload" in receipt:
        raise AssertionError("preparer did not return a compact receipt")
    prepared_activation_id = receipt.get("prepared_activation_id")
    if not isinstance(prepared_activation_id, str) or not prepared_activation_id.startswith(
        "sha256:"
    ):
        raise AssertionError("compact receipt names no prepared identity")
    stored_path = (
        activation_root
        / "prepared-activations"
        / f"{prepared_activation_id.removeprefix('sha256:')}.json"
    )
    stored = stored_path.read_bytes()
    value = json.loads(stored)
    if stored != canonical_json_bytes(value):
        raise AssertionError("stored prepared activation is not RFC 8785 canonical")
    identity = {key: item for key, item in value.items() if key != "prepared_activation_id"}
    if raw_sha256_digest(canonical_json_bytes(identity)) != prepared_activation_id:
        raise AssertionError("stored prepared activation has a false content identity")
    if {key: item for key, item in value.items() if key != "activation_payload"} != receipt:
        raise AssertionError("compact receipt differs from the stored product")
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


def _arm_source_joints(workspace: Path, component: dict[str, object]) -> list[str]:
    sources = load_strict_json_yaml(
        workspace
        / "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
    )
    profile = {
        row["runtime_restoration_profile_id"]: row
        for row in sources["runtime_restoration_profiles"]
    }[component["component_profile_id"]]
    family_id = profile["model_family_id"]
    for family in sources["model_families"]:
        if family["model_family_id"] != family_id:
            continue
        for binding in family["semantics"]["resource_role_bindings"]:
            if binding["resource_role_id"] == "arm_joints":
                return list(binding["source_joint_names"])
    raise AssertionError(f"model family {family_id} has no arm joints")


def _compiled_stop_limits(
    prepared: dict[str, object],
) -> dict[str, tuple[float, float, float | None]]:
    """Every compiled controlled-Stop limit, keyed by its arm resource.

    Each value is (deceleration, settle threshold, authored jerk or None).
    """

    found: dict[str, tuple[float, float, float | None]] = {}

    def jerk(row: dict[str, object]) -> float | None:
        encoded = row.get("max_jerk")
        if encoded is None:
            return None
        if encoded != {
            "coordinate_kind": "angular_rad_s3",
            "value_rad_s3": encoded.get("value_rad_s3"),
        }:
            raise AssertionError(f"{row['resource_id']} has a malformed jerk limit")
        return encoded["value_rad_s3"]

    def walk(value: object) -> None:
        if isinstance(value, dict):
            for key, child in value.items():
                if key == "per_resource_limits":
                    for row in child:
                        limit = (
                            row["max_deceleration"]["value_rad_s2"],
                            row["settle_velocity_threshold"]["value_rad_s"],
                            jerk(row),
                        )
                        if found.setdefault(row["resource_id"], limit) != limit:
                            raise AssertionError(
                                f"{row['resource_id']} has inconsistent Stop limits"
                            )
                walk(child)
        elif isinstance(value, list):
            for child in value:
                walk(child)

    for artifact in prepared["activation_payload"]["raw_artifacts"]:
        try:
            value = json.loads(
                base64.b64decode("".join(artifact["chunks"]), validate=True)
            )
        except ValueError:
            continue
        walk(value)
    return found


def _require_stop_limits_by_joint_name(
    args: argparse.Namespace, request: dict[str, object], root: Path
) -> None:
    component = request["deployment"]["components"][0]
    component_id = component["component_id"]
    joints = _arm_source_joints(args.workspace.resolve(), component)
    if len(joints) < 3:
        raise AssertionError("Stop limit pairing test requires three arm joints")

    def with_limits(rows: list[dict[str, object]]) -> dict[str, object]:
        changed = copy.deepcopy(request)
        changed["deployment"]["components"][0]["target_settings"]["arm"][
            "controlled_stop"
        ]["per_joint_limits"] = rows
        _rehash_request(changed)
        return changed

    # Distinct, exactly representable limits per joint, authored in reverse.
    # They start at the Lite6 model acceleration limit (10 rad/s^2) so the
    # Stop/planning coherence check accepts them: the first joint is equal,
    # the others are higher.
    rows = [
        {
            "source_joint_name": joint,
            "max_deceleration_rad_s2": 10.0 + index,
            "settle_velocity_threshold_rad_s": (index + 1) / 64,
        }
        for index, joint in enumerate(joints)
    ]
    def compiled_arm_limits(
        selected: list[dict[str, object]], name: str
    ) -> dict[str, tuple[float, float, float | None]]:
        prepared = _run(args, with_limits(selected), root / name)
        assert prepared is not None
        return {
            resource_id: limit
            for resource_id, limit in _compiled_stop_limits(prepared).items()
            if resource_id.startswith(f"{component_id}.arm.")
        }

    expected = {
        f"{component_id}.arm.coordinate.{index:06d}": (
            10.0 + index,
            (index + 1) / 64,
            None,
        )
        for index in range(len(joints))
    }
    compiled = compiled_arm_limits(rows[::-1], "shuffled")
    if compiled != expected:
        raise AssertionError(
            f"controlled Stop limits were not paired by joint name: {compiled!r}"
        )

    # An authored jerk limit reaches exactly its own joint's resource; rows
    # without one still compile without a jerk member.
    with_jerk = copy.deepcopy(rows)
    with_jerk[1]["max_jerk_rad_s3"] = 12.5
    compiled = compiled_arm_limits(with_jerk[::-1], "authored-jerk")
    expected_with_jerk = dict(expected)
    expected_with_jerk[f"{component_id}.arm.coordinate.000001"] = (11.0, 2 / 64, 12.5)
    if compiled != expected_with_jerk:
        raise AssertionError(
            f"authored controlled Stop jerk was not compiled for {joints[1]}: "
            f"{compiled!r}"
        )

    unnamed = copy.deepcopy(rows)
    del unnamed[1]["source_joint_name"]
    # A Stop deceleration below the joint's planning acceleration is refused
    # with the operator-readable diagnostic the GUI shows as preparer_detail.
    below_planning = copy.deepcopy(rows)
    below_planning[1]["max_deceleration_rad_s2"] = 0.5
    rejections = {
        "missing": (
            rows[:2] + rows[3:],
            f"controlled Stop limits miss joint {joints[2]}",
        ),
        "unknown": (
            rows[:-1] + [{**rows[-1], "source_joint_name": "not_an_arm_joint"}],
            "controlled Stop limits name unknown arm joint not_an_arm_joint",
        ),
        "extra": (
            rows + [{**rows[0], "source_joint_name": "extra_joint"}],
            "controlled Stop limits name unknown arm joint extra_joint",
        ),
        "duplicate": (
            rows + [rows[1]],
            f"controlled Stop limits repeat joint {joints[1]}",
        ),
        "unnamed": (unnamed, "controlled Stop limit row lacks source_joint_name"),
        "below-planning-acceleration": (
            below_planning,
            f"Stop profile {component_id}.arm.controlled_stop, joint "
            f"{joints[1]}: Stop deceleration limit 0.5 rad/s^2 "
            "(max_deceleration_rad_s2) is lower than the normal-motion "
            "planning acceleration limit 10 rad/s^2",
        ),
    }
    jerk_message = (
        f"controlled Stop max_jerk_rad_s3 for joint {joints[2]} "
        "must be a finite positive number"
    )
    for name, value in (("zero", 0.0), ("negative", -1.0), ("text", "fast")):
        invalid = copy.deepcopy(rows)
        invalid[2]["max_jerk_rad_s3"] = value
        rejections[f"jerk-{name}"] = (invalid, jerk_message)
    for name, (selected, message) in rejections.items():
        _run(
            args,
            with_limits(selected),
            root / f"stop-limits-{name}",
            expect_error=message,
        )


def _rehash_request(request: dict[str, object]) -> None:
    captured = {
        key: value
        for key, value in request.items()
        if key not in {
            "schema_version",
            "runtime_cell_id",
            "captured_input_digest",
            "prepared_model_closure",
        }
    }
    request["captured_input_digest"] = raw_sha256_digest(
        canonical_json_bytes(captured)
    )


def _require_gripper_delivery_timeout_semantics(
    args: argparse.Namespace, request: dict[str, object], root: Path
) -> None:
    """Prepare actual pneumatic/Position rows through the authored selector."""
    root.mkdir()

    def target(value: dict[str, object], component_id: str) -> dict[str, object]:
        (component,) = [
            row for row in value["deployment"]["components"]
            if row["component_id"] == component_id
        ]
        (row,) = component["configuration"]["runtime_configuration"][
            "gripper_target_configurations"
        ]
        assert row["controllable_component_id"] == component_id
        assert row["command_target_id"] == "gripper"
        return row

    pneumatic = target(request, "lite6_robot")
    position = target(request, "ur5e_robot")
    assert pneumatic["actuation_kind"] == "pneumatic" and len(pneumatic) == 14
    assert pneumatic["delivery_result_timeout_s"] == 1.0
    assert position["actuation_kind"] == "position_commanded" and len(position) == 13
    assert "delivery_result_timeout_s" not in position

    for name, timeout in (("authored", 1.0), ("absent", None), ("override", 2.5)):
        accepted = copy.deepcopy(request)
        row = target(accepted, "lite6_robot")
        if timeout is None:
            del row["delivery_result_timeout_s"]
        else:
            row["delivery_result_timeout_s"] = timeout
        _rehash_request(accepted)
        prepared = _run(args, accepted, root / name)
        assert prepared is not None
        units = prepared["activation_payload"]["retained_bundle"]["cell_graph"][
            "runtime_units"
        ]
        (unit,) = units
        assert unit["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
        retained = unit["configuration"]["gripper_target_configurations"]
        assert retained == [row, position]

    invalid_rows = []
    for name, timeout in (
        ("zero", 0.0), ("negative", -1.0), ("text", "1.0"),
        ("boolean", True), ("null", None),
    ):
        rejected = copy.deepcopy(request)
        target(rejected, "lite6_robot")["delivery_result_timeout_s"] = timeout
        invalid_rows.append((name, rejected))
    foreign_position = copy.deepcopy(request)
    target(foreign_position, "ur5e_robot")["delivery_result_timeout_s"] = 1.0
    invalid_rows.append(("position-timeout", foreign_position))
    foreign_field = copy.deepcopy(request)
    row = target(foreign_field, "lite6_robot")
    row["foreign_timeout_s"] = row.pop("delivery_result_timeout_s")
    invalid_rows.append(("foreign-fourteenth-field", foreign_field))
    missing_base = copy.deepcopy(request)
    del target(missing_base, "lite6_robot")["description"]
    invalid_rows.append(("missing-base-field", missing_base))
    for name, rejected in invalid_rows:
        _rehash_request(rejected)
        _run(
            args, rejected, root / name,
            expect_error="has invalid authored gripper target semantics",
        )


def _check_two_ros_robots(
    args: argparse.Namespace, request: dict[str, object], root: Path
) -> None:
    """Q68: two ROS robots with catalog controller profiles compose."""
    prepared = _run(args, request, root)
    assert prepared is not None
    graph = prepared["activation_payload"]["retained_bundle"]["cell_graph"]
    components = sorted(
        row["controllable_component_id"] for row in graph["controllable_components"]
    )
    assert components == ["isaac_ur10e_light_robot", "isaac_ur10e_second_robot"]
    assert len(graph["runtime_units"]) == 1
    runtime = graph["runtime_units"][0]
    assert runtime["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
    configuration = runtime["configuration"]
    # Q68: each component carries its own controller profile.
    assert configuration["controller_profiles"] == [
        {"controllable_component_id": component,
         ("isaac_profile" if component == "isaac_ur10e_light_robot" else "mock_profile"):
             ("ur10e" if component == "isaac_ur10e_light_robot" else "ur10e_robotiq_2f_140"),
         "frame_prefix": next(row for row in request["deployment"]["components"]
                              if row["component_id"] == component).get("frame_prefix", "")}
        for component in components
    ]
    assert "isaac_profile" not in configuration
    authored = [
        component["configuration"]["runtime_configuration"]
        for component in request["deployment"]["components"]
    ]
    # Each robot keeps its own initial state (component order), TCP-speed
    # maximum and fault Stop profile; no native-only field is introduced.
    assert configuration["initial_joint_positions_rad"] == [
        value for row in authored for value in row["initial_joint_positions_rad"]
    ]
    assert [
        row["controllable_component_id"] for row in configuration["motion_target_limits"]
    ] == components
    assert [
        row["controllable_component_id"]
        for row in configuration["default_fault_stop_profiles"]
    ] == components
    assert "direct_control_joint_limits" not in configuration
    assert "gripper_target_configurations" not in configuration


def _check_ros_preparation(
    args: argparse.Namespace,
    request: dict[str, object],
    scenario_path: Path,
    document: dict[str, object],
    root: Path,
) -> None:
    legacy_world = copy.deepcopy(request)
    legacy_world["deployment"]["isaac_world"] = "lite6_grf_ros2"
    _rehash_request(legacy_world)
    _run(args, legacy_world, root / "ros-legacy-world-selector",
         expect_error="isaac_world")
    prepared = _run(args, request, root / "ros-valid")
    assert prepared is not None
    bundle = prepared["activation_payload"]["retained_bundle"]
    # Q87: older authored projects remain valid; omission materializes the
    # same empty prefix as current explicit authoring before sealing.
    old_authored = copy.deepcopy(request)
    for component in old_authored["deployment"]["components"]:
        component.pop("frame_prefix", None)
    _rehash_request(old_authored)
    old_authored_result = _run(args, old_authored, root / "ros-old-authored")
    assert old_authored_result is not None
    assert old_authored_result["activation_payload"]["retained_bundle"] == bundle
    graph = bundle["cell_graph"]
    component_id = "isaac_ur10e_light_robot"
    runtime_id = "robot_cell.ros2_control_execution"
    assert graph["cell_id"] == request["runtime_cell_id"]
    assert len(graph["runtime_units"]) == 1
    runtime = graph["runtime_units"][0]
    assert runtime["runtime_unit_id"] == runtime_id
    assert runtime["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
    expected_configuration = copy.deepcopy(request["deployment"]["components"][0][
        "configuration"
    ]["runtime_configuration"])
    assert request["deployment"]["components"][0]["target_settings"]["arm"][
        "maximum_tcp_linear_speed_mps"
    ] == 0.25
    expected_configuration["motion_target_limits"] = [{
        "controllable_component_id": component_id,
        "command_target_id": "arm",
        "maximum_tcp_linear_speed_mps": 0.25,
    }]
    # Q68: the authored selector becomes the component's controller profile
    # row inside the one ROS runtime unit.
    expected_configuration["controller_profiles"] = [{
        "controllable_component_id": component_id,
        "isaac_profile": expected_configuration.pop("isaac_profile"),
        "frame_prefix": request["deployment"]["components"][0].get("frame_prefix", ""),
    }]
    # Q37: the authored fault Stop profile is carried unchanged (its joint map
    # as canonical-sorted rows) for the arm target.
    authored_fault = copy.deepcopy(request["deployment"]["components"][0][
        "configuration"
    ]["default_fault_stop_profile"])
    authored_fault["per_joint_max_deceleration_rad_s2"] = [
        {"source_joint_name": joint, "max_deceleration_rad_s2": limit}
        for joint, limit in sorted(
            authored_fault["per_joint_max_deceleration_rad_s2"].items()
        )
    ]
    expected_configuration["default_fault_stop_profiles"] = [{
        "controllable_component_id": component_id,
        "command_target_id": "arm",
        **authored_fault,
    }]
    assert runtime["configuration"] == expected_configuration
    assert runtime["configuration"]["mode"] == "commanded_sim"
    assert graph["controllable_components"][0]["component_type_id"] == (
        "ur10e_robotiq_2f_140.fixed_scale.component"
    )
    assert {row["control_coordinator_endpoint_role"] for row in graph["control_domains"]} == {
        "ros2_control_execution_status"
    }
    bindings = graph["control_bindings"]
    assert {row["command_target_id"] for row in bindings} == {"arm", "gripper"}
    for row in bindings:
        target = row["command_target_id"]
        assert row["provider_runtime_unit_id"] == runtime_id
        assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
        assert row["provider_instance_id"] == f"{component_id}.ros2_control"
        assert row["state_authority_class"] == "commanded"
        assert row["state_authority_id"] == f"{component_id}.{target}.commanded_sim_state"
        assert row["control_interface_contract_id"] == f"manyforge.ros2_control_{target}_position"
        assert row["controller_topology_id"] == f"manyforge.ros2_control_{target}_direct"
        if target == "arm" and row["capability_id"] == "execution.trajectory.execute":
            assert row["runtime_scale_route_qualifications"] == [{
                "control_route_id": "arm_position",
                "scaling_domain_id": f"{component_id}.arm.runtime_scale",
                "command_endpoint_role_id": "command",
                "applied_state_endpoint_role_id": "state",
                "acknowledgement_kind": "applied_state",
                "maximum_slew_per_second": 4.0,
                "freshness_budget_ns": 100_000_000,
            }]
        else:
            assert not row.get("runtime_scale_route_qualifications")
        assert row["capability_id"] != "execution.runtime_scale.operation_scoped"
    arm = next(row for row in bindings if row["capability_id"] == "execution.trajectory.execute")
    gripper = next(row for row in bindings if row["capability_id"] == "tool.gripper.command")
    assert len(arm["resource_bindings"]) == 6
    assert gripper["resource_bindings"] == [{
        "resource_role_id": "fingers",
        "resource_id": f"{component_id}.gripper.coordinate.000000",
    }]
    desired = bundle["desired_control_configurations"]
    assert len(desired) == 2
    assert {row["controller_topology_id"] for row in desired} == {
        "manyforge.ros2_control_arm_direct", "manyforge.ros2_control_gripper_direct"
    }
    assert {route["control_binding_id"] for row in desired for route in row["binding_routes"]} == {
        row["control_binding_id"] for row in bindings
    }
    providers = [row for row in bundle["capability_provider_bindings"]
                 if row["scope"] == "target_binding"]
    assert providers
    assert {row["target"]["command_target_id"] for row in providers} == {"arm", "gripper"}
    for row in providers:
        assert row["provider_instance_id"] == f"{component_id}.ros2_control"
        assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
        assert row["endpoint_bindings"] == [{
            "endpoint_role_id": "planner_scene_projection",
            "source_kind": "runtime_unit",
            "source_id": runtime_id,
            "source_endpoint_role_id": "planner_scene_projection",
        }]
    products = bundle["compiled_products"]["runtime_unit_products"]
    assert len(products) == 1 and products[0]["runtime_unit_id"] == runtime_id
    assert [row["product_contract_id"] for row in products[0]["products"]] == [
        "manyforge.ros2_control_execution_runtime_unit"
    ]
    launches = []
    for artifact in prepared["activation_payload"]["raw_artifacts"]:
        try:
            value = json.loads(base64.b64decode("".join(artifact["chunks"]), validate=True))
        except ValueError:
            continue
        if isinstance(value, dict) and value.get("api_version") == "manyforge.installed_target_launch.v1":
            launches.append(value)
    assert len(launches) == 1
    assert launches[0]["target_id"] == "manyforge_ros2_control_execution_host"
    assert launches[0]["implementation_id"] == "manyforge.ros2_control_execution_host"

    replay = _run(args, request, root / "ros-replay")
    assert replay == prepared
    assert _run(args, request, root / "ros-valid", reuse_root=True) == prepared
    # Publishing the derived empty bundle adds its raw artifact closure to
    # the prepared payload. Its prepared identity differs legitimately, while
    # the populated candidate and its fence-host product remain exact.
    published = _run(args, request, root / "ros-published", publish_canonical_empty=True)
    assert published is not None
    _check_sealed_preparation(args, root / "ros-published" / "activation", root / "ros-sealed")
    assert published["activation_payload"]["retained_bundle"] == bundle
    assert _run(args, request, root / "ros-published", publish_canonical_empty=True,
                reuse_root=True) == published
    relocated = copy.deepcopy(request)
    relocated["runtime_cell_id"] = "another_ros_runtime"
    other = _run(args, relocated, root / "ros-relocated")
    assert other is not None
    assert other["captured_input_digest"] == prepared["captured_input_digest"]
    for field in ("prepared_activation_id", "retained_bundle_id", "graph_revision"):
        assert other[field] != prepared[field], field

    invalid_kind = copy.deepcopy(request)
    invalid_kind["deployment"]["components"][0]["configuration"]["runtime_kind"] = "native"
    _rehash_request(invalid_kind)
    _run(args, invalid_kind, root / "ros-kind-mismatch",
         expect_error="configuration.runtime_kind is unsupported: native")
    invalid_config = copy.deepcopy(request)
    invalid_config["deployment"]["components"][0]["configuration"][
        "runtime_configuration"
    ]["motion_target_limits"] = []
    _rehash_request(invalid_config)
    _run(args, invalid_config, root / "ros-native-configuration",
         expect_error="motion_target_limits")
    invalid_profile = copy.deepcopy(request)
    invalid_profile["deployment"]["components"][0]["configuration"][
        "runtime_configuration"
    ]["isaac_profile"] = "lite6"
    _rehash_request(invalid_profile)
    _run(args, invalid_profile, root / "ros-wrong-robot-profile",
         expect_error="isaac_profile does not match its ROS component profile")
    missing_limit = copy.deepcopy(request)
    del missing_limit["deployment"]["components"][0]["target_settings"]["arm"][
        "maximum_tcp_linear_speed_mps"
    ]
    _rehash_request(missing_limit)
    _run(args, missing_limit, root / "ros-missing-motion-limit",
         expect_error="requires exactly one TCP-speed maximum")
    for name, value in (("zero", 0), ("negative", -0.1), ("text", "fast")):
        invalid_limit = copy.deepcopy(request)
        invalid_limit["deployment"]["components"][0]["target_settings"]["arm"][
            "maximum_tcp_linear_speed_mps"
        ] = value
        _rehash_request(invalid_limit)
        _run(args, invalid_limit, root / f"ros-invalid-limit-{name}",
             expect_error="maximum_tcp_linear_speed_mps")
    foreign_limit = copy.deepcopy(request)
    foreign_limit["deployment"]["components"][0]["target_settings"]["gripper"] = {
        "maximum_tcp_linear_speed_mps": 0.1
    }
    _rehash_request(foreign_limit)
    _run(args, foreign_limit, root / "ros-foreign-motion-limit",
         expect_error="requires exactly one TCP-speed maximum")
    unsupported_jerk = copy.deepcopy(request)
    unsupported_jerk["deployment"]["components"][0]["target_settings"]["arm"][
        "ordinary_motion_jerk"
    ] = {"enabled": True, "joint_limits": [], "preset_reference_time_s": 1}
    _rehash_request(unsupported_jerk)
    _run(args, unsupported_jerk, root / "ros-authored-jerk",
         expect_error="authored ROS ordinary motion jerk is not yet supported")

    for scenario, expected_error in (
        # Q68 + Q63-Q66: the offline Lite 6 is a mock ROS component now; the
        # pair is refused only by the existing shared runtime-unit cadence
        # agreement (50 ms vs the UR10e light scene's 100 ms).
        (
            "examples/lite6_pick_and_place.scenario",
            "disagree on collision_monitor_period_ms",
        ),
        # Q68: two ROS robots pass the composition rule; this pair is refused
        # only by the existing shared runtime-unit cadence agreement.
        (
            "examples/isaac-lite6-pick-place.scenario",
            "exclusive Isaac topic collision",
        ),
    ):
        _, additional, _ = _scenario_request(args.workspace, scenario)
        unsupported = copy.deepcopy(document)
        unsupported["deployment_draft"]["components"].extend(
            additional["deployment_draft"]["components"]
        )
        for added in unsupported["deployment_draft"]["components"]:
            if added["component_id"] != document["deployment_draft"]["components"][0]["component_id"]:
                added["frame_prefix"] = added["component_id"] + "/"
        unsupported["deployment_draft"]["components"].sort(
            key=lambda component: component["component_id"]
        )
        combined = _request(scenario_path, unsupported, request["program"])
        _run(args, combined, root / Path(scenario).stem, expect_error=expected_error)
    for index, prefix in enumerate(("second/", "Robot_2/nested/", "0/", "_/", "robot//")):
        prefixed = copy.deepcopy(request)
        prefixed["deployment"]["components"][0]["frame_prefix"] = prefix
        _rehash_request(prefixed)
        prefixed_result = _run(args, prefixed, root / f"ros-explicit-prefix-{index}")
        assert prefixed_result is not None
        assert prefixed_result["activation_payload"]["retained_bundle"]["cell_graph"]["runtime_units"][0]["configuration"]["controller_profiles"][0]["frame_prefix"] == prefix
    for index, prefix in enumerate(("literal prefix/", "/robot/", "/", "robot", "robot-b/", "robot.b/",
                                    "röbot/", "robot/\n", "robot\\/")):
        bad_format = copy.deepcopy(request)
        bad_format["deployment"]["components"][0]["frame_prefix"] = prefix
        _rehash_request(bad_format)
        _run(args, bad_format, root / f"ros-invalid-prefix-format-{index}",
             expect_error="frame_prefix must be empty or use only ASCII letters")
    bad_prefix = copy.deepcopy(request)
    bad_prefix["deployment"]["components"][0]["frame_prefix"] = 1
    _rehash_request(bad_prefix)
    _run(args, bad_prefix, root / "ros-invalid-prefix", expect_error="frame_prefix must be a string")
    # Q68: any number of ROS components with catalog controller profiles is
    # accepted by the compiler (no hard-coded single-UR10e rule).
    two_robots = copy.deepcopy(document)
    second_robot = copy.deepcopy(two_robots["deployment_draft"]["components"][0])
    second_robot["component_id"] = "isaac_ur10e_second_robot"
    second_robot["frame_prefix"] = "second/"
    second_robot["root_pose_in_cell"]["position_m"] = [1.5, 0.0, 0.0]
    two_robots["deployment_draft"]["components"].append(second_robot)
    two_robots["deployment_draft"]["components"].sort(
        key=lambda component: component["component_id"]
    )
    colliding = copy.deepcopy(two_robots)
    del colliding["deployment_draft"]["components"][1]["frame_prefix"]
    _run(args, _request(scenario_path, colliding, request["program"]),
         root / "ros-colliding-frames", expect_error="published TF frame collision")
    colliding_custom = copy.deepcopy(two_robots)
    for component in colliding_custom["deployment_draft"]["components"]:
        component["frame_prefix"] = "second/"
    _run(args, _request(scenario_path, colliding_custom, request["program"]),
         root / "ros-colliding-custom-prefix", expect_error="published TF frame collision")
    _run(args, _request(scenario_path, two_robots, request["program"]),
         root / "ros-exclusive-topic-collision",
         expect_error="exclusive Isaac topic collision between isaac_ur10e_light_robot and isaac_ur10e_second_robot: /isaac_joint_commands")
    second_robot["component_profile_id"] = "ur10e_robotiq_mock_commanded_v0"
    second_config = second_robot["configuration"]["runtime_configuration"]
    second_config.pop("isaac_profile")
    second_config["mock_profile"] = "ur10e_robotiq_2f_140"
    _check_two_ros_robots(
        args, _request(scenario_path, two_robots, request["program"]),
        root / "ros-two-robots",
    )
    # Q68: one ROS runtime unit hosts robots with different controller
    # profiles (Lite 6 + UR10e) when the cell-wide cadence agrees; each
    # component carries its own profile.
    _, lite6_document, _ = _scenario_request(
        args.workspace, "examples/isaac-lite6-pick-place.scenario"
    )
    lite6_robot = copy.deepcopy(lite6_document["deployment_draft"]["components"][0])
    lite6_robot["frame_prefix"] = "lite6/"
    lite6_robot["configuration"]["runtime_configuration"][
        "collision_monitor_period_ms"
    ] = document["deployment_draft"]["components"][0]["configuration"][
        "runtime_configuration"
    ]["collision_monitor_period_ms"]
    two_profiles = copy.deepcopy(document)
    ur10e_mock = two_profiles["deployment_draft"]["components"][0]
    ur10e_mock["component_profile_id"] = "ur10e_robotiq_mock_commanded_v0"
    ur10e_config = ur10e_mock["configuration"]["runtime_configuration"]
    ur10e_config.pop("isaac_profile")
    ur10e_config["mock_profile"] = "ur10e_robotiq_2f_140"
    two_profiles["deployment_draft"]["components"].append(lite6_robot)
    two_profiles["deployment_draft"]["components"].sort(
        key=lambda component: component["component_id"]
    )
    mixed = _run(args, _request(scenario_path, two_profiles, request["program"]),
                 root / "ros-two-controller-profiles")
    assert mixed is not None
    mixed_runtime = mixed["activation_payload"]["retained_bundle"]["cell_graph"][
        "runtime_units"
    ]
    assert len(mixed_runtime) == 1
    assert mixed_runtime[0]["configuration"]["controller_profiles"] == sorted([
        {"controllable_component_id": "isaac_lite6_robot", "isaac_profile": "lite6", "frame_prefix": "lite6/"},
        {"controllable_component_id": "isaac_ur10e_light_robot", "mock_profile": "ur10e_robotiq_2f_140", "frame_prefix": ""},
    ], key=lambda row: row["controllable_component_id"])
    # Both qualified models have disjoint owned frames. Reserve the Lite6's
    # shared root world against an owned output; allow another shared root to
    # use that global parent. The former w + orld alias now fails prefix grammar
    # first, since a valid non-empty prefix must end in '/'.
    for name, prefix, extra, expected_error in (
        ("prefixed-world-alias", "w",
         '<link name="orld"/><joint name="test_world_alias" type="fixed">'
         '<parent link="base_link"/><child link="orld"/></joint>',
         "frame_prefix must be empty or use only ASCII letters"),
        ("nonroot-world-alias", "",
         '<link name="world"/><joint name="test_world_alias" type="fixed">'
         '<parent link="base_link"/><child link="world"/></joint>',
         "published TF frame collision: world"),
    ):
        world_document = copy.deepcopy(two_profiles)
        for component in world_document["deployment_draft"]["components"]:
            component["frame_prefix"] = prefix if component["component_id"] == component_id else ""
        world_request = _request(scenario_path, world_document, request["program"])
        source = next(row for row in world_request["model_source_closure"]["components"]
                      if row["component_id"] == component_id)
        original = decode_model_source_artifact(source["entrypoints"]["urdf"],
                                               maximum_bytes=MAX_XML_BYTES,
                                               label="world-collision test URDF")
        assert original.count(b"</robot>") == 1
        changed = original.replace(b"</robot>", extra.encode() + b"</robot>")
        source["entrypoints"]["urdf"] = encode_model_source_artifact(
            changed, media_type="application/xml")
        prepared_rows = world_request["prepared_model_closure"]["components"]
        index = next(index for index, row in enumerate(prepared_rows)
                     if row["component_id"] == component_id)
        previous = prepared_rows[index]
        prepared_rows[index] = prepare_captured_model_source(
            source, model_asset_id=previous["model_asset_id"],
            accepted_package_uri_prefix=previous["accepted_package_uri_prefix"])
        _rehash_request(world_request)
        _run(args, world_request, root / f"ros-{name}", expect_error=expected_error)
    # This maintained pair already qualifies both models with root world and
    # disjoint owned frames; retain its complete model-backed configuration.
    _, _, repeated_world_request = _scenario_request(
        args.workspace, "examples/ur5e_lite6_wall_grouped.scenario")
    assert all(component.get("frame_prefix", "") == ""
               for component in repeated_world_request["deployment"]["components"])
    for source in repeated_world_request["model_source_closure"]["components"]:
        urdf = decode_model_source_artifact(source["entrypoints"]["urdf"],
                                           maximum_bytes=MAX_XML_BYTES,
                                           label="shared-world test URDF")
        assert b'<link name="world"' in urdf
    _run(args, repeated_world_request, root / "ros-repeated-shared-world")
    # Components sharing the one ROS execution host must agree on its
    # execution_tick_hz; this checks host cadence, not linked activation.
    mismatched = copy.deepcopy(two_profiles)
    for component in mismatched["deployment_draft"]["components"]:
        if component["component_id"] == "isaac_lite6_robot":
            component["configuration"]["runtime_configuration"][
                "execution_tick_hz"
            ] = 50
    _run(args, _request(scenario_path, mismatched, request["program"]),
         root / "ros-two-controller-profiles-cadence",
         expect_error="disagree on execution_tick_hz")
    for separator in ("-", "."):
        invalid_namespace = copy.deepcopy(document)
        invalid_namespace["deployment_draft"]["components"][0]["component_id"] = (
            f"isaac{separator}ur10e"
        )
        renamed = _request(scenario_path, invalid_namespace, request["program"])
        _run(args, renamed, root / f"ros-invalid-namespace-{separator}",
             expect_error="must be one ROS name token")


def _write_manager_ros_apply_fixture(
    args: argparse.Namespace, request: dict[str, object], *, isaac: bool = False
) -> None:
    """Materialize one real ROS Scenario through the production tool."""
    components = request["deployment"]["components"]
    if len(components) != 1:
        raise AssertionError("Manager ROS fixture requires exactly one component")
    configuration = components[0]["configuration"]
    runtime_configuration = configuration["runtime_configuration"]
    if configuration["runtime_kind"] != "ros2_control":
        raise AssertionError("Manager ROS fixture must use the ROS runtime")
    if isaac:
        if "isaac_profile" not in runtime_configuration:
            raise AssertionError("Manager Isaac fixture must use an Isaac profile")
    elif "mock_profile" not in runtime_configuration:
        raise AssertionError("Manager ROS fixture must use Mock ROS")
    # This is the real runtime cell requested by the test, not a retained ID,
    # catalog seal, generation, owner epoch, or readiness observation.
    request["runtime_cell_id"] = (
        "c1_build_authority_cell" if isaac else "manager_ros_apply_fixture"
    )
    requested_root = (
        args.manager_isaac_apply_fixture_root
        if isaac
        else args.manager_ros_apply_fixture_root
    )
    required_root_name = (
        "manager-isaac-apply-fixture" if isaac else "manager-ros-apply-fixture"
    )
    if (
        requested_root.name != required_root_name
        or requested_root.is_symlink()
    ):
        raise AssertionError(
            "Manager ROS fixture cleanup requires its dedicated output root"
        )
    root = requested_root.resolve()
    workspace = args.workspace.resolve()
    build_root = root.parent
    if (
        root == workspace
        or root in workspace.parents
        or workspace in root.parents
        or build_root.name != "headless"
        or not (build_root / "CMakeCache.txt").is_file()
    ):
        raise AssertionError(
            "Manager ROS fixture output must be a child of the Headless CMake build"
        )
    if root.exists():
        if not root.is_dir():
            raise AssertionError("Manager ROS fixture output root is not a directory")
        shutil.rmtree(root)
    root.mkdir()
    prepared = _run(args, request, root, publish_canonical_empty=True, reuse_root=True)
    bundle = prepared["activation_payload"]["retained_bundle"]
    graph = bundle["cell_graph"]
    units = graph["runtime_units"]
    if len(units) != 1 or units[0]["runtime_profile_id"] != (
        "manyforge.ros2_control_execution_runtime"
    ):
        raise AssertionError("Manager fixture did not compile the real ROS runtime")
    admission_filename = f"canonical-empty-admission.{graph['cell_id']}.json"
    admission = json.loads((root / "activation" / admission_filename).read_bytes())
    empty = json.loads((root / "activation" / "retained-bundles" / "sha256" /
        f"{admission['retained_bundle_id'].removeprefix('sha256:')}.json").read_bytes())
    if empty["cell_graph"]["controllable_components"] or (
        empty["cell_graph"]["runtime_units"][0]["runtime_profile_id"] !=
        "manyforge.ros2_control_execution_runtime"
    ):
        raise AssertionError("Manager fixture predecessor must be genuinely empty ROS")
    metadata = {
        "admission_filename": admission_filename,
        "candidate_retained_bundle_id": bundle["retained_bundle_id"],
        "cell_id": graph["cell_id"],
        "component_id": components[0]["component_id"],
        "deployment_artifact_id": bundle["deployment_artifact_id"],
        "deployment_revision": bundle["deployment_revision"],
        "prepared_activation_id": prepared["prepared_activation_id"],
        "predecessor_graph_revision": empty["cell_graph"]["graph_revision"],
        "predecessor_retained_bundle_id": admission["retained_bundle_id"],
    }
    (root / "metadata.json").write_bytes(canonical_json_bytes(metadata))



def _observe_manager_ros() -> int:
    """Read actual CM/JSB facts for the existing Manager process fixture."""
    import math
    import socket
    import time
    import yaml
    import rclpy
    from controller_manager_msgs.srv import ListControllers
    from sensor_msgs.msg import JointState
    from rclpy.qos import qos_profile_sensor_data

    parser = argparse.ArgumentParser()
    parser.add_argument("--execution-root", required=True, type=Path)
    parser.add_argument("--component", required=True)
    parser.add_argument("--state", choices=("inactive", "active"), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(sys.argv[2:])
    records = [json.loads(p.read_bytes()) for p in
               (args.execution_root / "controller-launch-artifacts").rglob("controller_launch_artifacts.json")]
    records = [row for row in records if row["controllable_component_id"] == args.component]
    if len(records) != 1 or records[0]["lane"] != "mock":
        raise AssertionError("Expected exactly one actual Mock launch artifact")
    artifact = records[0]
    launch = dict(value.split(":=", 1) for value in artifact["launch_arguments"])
    namespace = launch["runtime_namespace"].rstrip("/")
    arm, gripper = launch["arm_controller_name"], launch["gripper_controller_name"]
    yaml_bytes = Path(artifact["controllers_file"]).read_bytes()
    if raw_sha256_digest(yaml_bytes) != artifact["controllers_sha256"]:
        raise AssertionError("Actual controllers YAML does not match its writing receipt")
    configuration = yaml.safe_load(yaml_bytes)
    joint = configuration[f"/**/{gripper}"]["ros__parameters"]["joint"]

    def command(payload: dict[str, object], *, allow_refusal: bool = False) -> dict[str, object]:
        with socket.socket(socket.AF_UNIX, socket.SOCK_SEQPACKET) as connection:
            connection.settimeout(5.0)
            connection.connect(str(args.execution_root / "execution-command.sock"))
            connection.sendall(canonical_json_bytes(payload))
            data, _, flags, _ = connection.recvmsg(8 * 1024 * 1024)
            if not data or flags & socket.MSG_TRUNC:
                raise AssertionError("Execution response exceeds fixture bound or is empty")
        response = json.loads(data)
        if not allow_refusal and not response["ok"]:
            raise AssertionError(response)
        return response

    # This is the actual installed compiled target, including model source name.
    status = command({"api_version": "manyforge.execution_command.v1", "command": "targets"})
    components = [row for row in status["targets"]["components"]
                  if row["controllable_component_id"] == args.component]
    if len(components) != 1:
        raise AssertionError("Installed target component differs from launch artifact")
    targets = [row for row in components[0]["targets"] if row["presentation"] == "gripper"]
    if len(targets) != 1:
        raise AssertionError("Expected one actual installed model-backed gripper target")
    target, direct = targets[0], targets[0]["command"]
    if direct["resource"]["source_kind"] != "model_role" or direct["resource"]["source_coordinate_name"] != joint:
        raise AssertionError("Generated gripper joint differs from actual retained model binding")

    rclpy.init()
    node = rclpy.create_node("manyforge_manager_fixture_observation")
    deadline = time.monotonic() + 8.0
    sample = None
    topic = None
    subscription = None
    rows = None
    try:
        client = node.create_client(ListControllers, namespace + "/" + launch["controller_manager_name"] + "/list_controllers")
        while time.monotonic() < deadline:
            if not client.wait_for_service(timeout_sec=min(0.1, max(0.0, deadline - time.monotonic()))):
                time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
                continue
            future = client.call_async(ListControllers.Request())
            rclpy.spin_until_future_complete(node, future, timeout_sec=max(0.0, deadline - time.monotonic()))
            if not future.done() or future.result() is None:
                break
            rows = {row.name: {"state": row.state, "type": row.type} for row in future.result().controller}
            broadcasters = [name for name, row in rows.items() if row["type"] == "joint_state_broadcaster/JointStateBroadcaster" and row["state"] == "active"]
            if rows.get(arm, {}).get("state") != args.state or rows.get(gripper, {}).get("state") != args.state or len(broadcasters) != 1:
                rclpy.spin_once(node, timeout_sec=0.05)
                # spin_once may return immediately; failed observations need a real pause.
                time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
                continue
            if subscription is None:
                topics = [name for name, types in node.get_topic_names_and_types()
                          if "sensor_msgs/msg/JointState" in types and any(
                              info.node_name == broadcasters[0] and info.node_namespace.rstrip("/") == namespace
                              for info in node.get_publishers_info_by_topic(name))]
                if len(topics) != 1:
                    rclpy.spin_once(node, timeout_sec=0.05)
                    time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
                    continue
                topic = topics[0]
                started_ns = node.get_clock().now().nanoseconds

                def receive(message: JointState) -> None:
                    nonlocal sample
                    stamp_ns = message.header.stamp.sec * 1_000_000_000 + message.header.stamp.nanosec
                    now_ns = node.get_clock().now().nanoseconds
                    if message.name.count(joint) == 1:
                        index = message.name.index(joint)
                        if index < len(message.position) and math.isfinite(message.position[index]) and started_ns <= stamp_ns <= now_ns and now_ns - stamp_ns <= 1_000_000_000:
                            sample = {"joint": joint, "position": message.position[index], "stamp_ns": stamp_ns, "received_ns": now_ns}
                subscription = node.create_subscription(JointState, topic, receive, qos_profile_sensor_data)
            rclpy.spin_once(node, timeout_sec=0.05)
            if sample is not None and node.get_clock().now().nanoseconds - sample["stamp_ns"] <= 1_000_000_000:
                break
            time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))
        if rows is None or rows.get(arm, {}).get("state") != args.state or rows.get(gripper, {}).get("state") != args.state or len(broadcasters) != 1 or sample is None or not 0 <= node.get_clock().now().nanoseconds - sample["stamp_ns"] <= 1_000_000_000:
            raise AssertionError({"expected_state": args.state, "controllers": rows, "topic": topic, "fresh_gripper_sample": sample})
        evidence = {"label": args.label, "controller_manager": namespace + "/" + launch["controller_manager_name"],
                    "arm_controller": arm, "gripper_controller": gripper, "controllers": rows,
                    "joint_state_topic": topic, "fresh_gripper_sample": {**sample, "stamp_ns": str(sample["stamp_ns"]), "received_ns": str(sample["received_ns"])},
                    "controllers_sha256": artifact["controllers_sha256"]}
        if args.state == "inactive":
            # Same production intrinsic manual producer as test_native_execution_path.
            identity, manual = status["identity"], direct["manual_control"]
            kind = direct["resource"]["coordinate_kind"]
            position = {"kind": kind, "value_rad" if kind == "angular_rad" else "value_m": sample["position"]}
            operation = {"request_context": {"request_id": "empty-ros-one-mock." + args.label + ".gripper", "request_owner_epoch": identity["owner_epoch"], "source_label": "manager_ros_apply_fixture", "correlation_id": None},
                         **{key: manual[key] for key in ("program_id", "program_revision", "program_binding_id", "program_run_id")},
                         "expected_graph_revision": identity["graph_revision"], "expected_retained_bundle_id": identity["retained_bundle_id"], "expected_scene_revision": identity["scene_revision"],
                         "participants": [{"component_role_id": direct["component_role_id"], "target_role_id": direct["target_role_id"], "required_capability_id": target["operation_capability_id"], "action_span": None,
                                           "payload": {"kind": "capability_command", "command_schema_id": direct["command_schema_id"], "command_schema_revision": direct["command_schema_revision"],
                                                       "command_value": {"kind": "gripper_position", "position": position, "max_effort": None}, "completion_timeout_s": 2.0,
                                                       "caller_options": [{"option_id": "command_duration_s", "value_s": 0.5}]}}], "coordination_policy": "independent"}
            envelope = {"api_version": "manyforge.execution_command.v1", "command": "submit", "operation": operation,
                        "attribution": {"assurance": "unverified", "principal_id": None, "transport": "http"}}
            refusal = command(envelope, allow_refusal=True)
            # Existing Kernel checks route publication/readback before component eligibility.
            # Declared/no-W and Disabled must refuse through one of those actual gates.
            closed_admission_reasons = {"runtime_route_publication_required", "runtime_route_configuration_not_ready", "component_command_not_eligible"}
            if refusal["ok"] or refusal["error"]["details"].get("reason") not in closed_admission_reasons:
                raise AssertionError(refusal)
            evidence["gripper_submit_refusal"] = refusal
        args.output.write_bytes(canonical_json_bytes(evidence))
        print(json.dumps(evidence, sort_keys=True))
        return 0
    finally:
        node.destroy_node()
        rclpy.shutdown()

def main() -> int:
    if len(sys.argv) > 1 and sys.argv[1] == "--observe-manager-ros":
        return _observe_manager_ros()
    args = _arguments()
    workspace = args.workspace.resolve()
    scenario_path, document, request = _scenario_request(workspace, args.scenario)
    fixture_count = sum(
        selected is not None
        for selected in (
            args.manager_ros_apply_fixture_root,
            args.manager_isaac_apply_fixture_root,
        )
    )
    if fixture_count > 1:
        raise AssertionError("select only one Manager ROS fixture output")
    if fixture_count == 1:
        _write_manager_ros_apply_fixture(
            args,
            request,
            isaac=args.manager_isaac_apply_fixture_root is not None,
        )
        return 0
    with tempfile.TemporaryDirectory(prefix="manyforge-prepare-test-") as raw:
        temporary = Path(raw)
        if args.takeover_budget:
            baseline = copy.deepcopy(request)
            baseline["deployment"]["policies"].pop(
                "motion_takeover_preparation_budget_s", None
            )
            _rehash_request(baseline)
            ordinary = _run(args, baseline, temporary / "budget-absent")
            assert ordinary is not None
            ordinary_bundle = ordinary["activation_payload"]["retained_bundle"]
            assert "motion_takeover_preparation_budget_s" not in ordinary_bundle
            selected = copy.deepcopy(baseline)
            selected["deployment"]["policies"][
                "motion_takeover_preparation_budget_s"
            ] = 3.5
            _rehash_request(selected)
            moving = _run(args, selected, temporary / "budget-present")
            assert moving is not None
            moving_bundle = moving["activation_payload"]["retained_bundle"]
            assert moving_bundle["motion_takeover_preparation_budget_s"] == 3.5
            assert moving_bundle["retained_bundle_id"] != ordinary_bundle["retained_bundle_id"]
            return 0
        if args.ros2_control:
            _check_ros_preparation(args, request, scenario_path, document, temporary)
            return 0
        if args.no_program:
            request["program"] = None
            request["program_role_assignment"] = {
                "component_roles": [], "target_roles": []
            }
            _rehash_request(request)
            _run(args, request, temporary / "keep", expected_program_outcome_kind="no_program")
            request["deployment"]["components"] = []
            request["deployment"]["attachments"] = []
            request["deployment"]["policies"] = {}
            request["model_source_closure"]["components"] = []
            request["prepared_model_closure"]["components"] = []
            _rehash_request(request)
            _run(args, request, temporary / "empty", expected_program_outcome_kind="no_program")
            return 0
        if args.intent_rejections:
            _require_gripper_delivery_timeout_semantics(
                args, request, temporary / "gripper-delivery-timeout"
            )
            # Launch-time Program parameter overrides were removed; the former
            # request form is refused by version, never reinterpreted.
            removed_overrides = copy.deepcopy(request)
            removed_overrides["schema_version"] = (
                "manyforge.activation_preparation_request.v2"
            )
            removed_overrides["typed_parameter_overrides"] = {}
            _rehash_request(removed_overrides)
            _run(
                args,
                removed_overrides,
                temporary / "removed-parameter-overrides",
                expect_error="typed_parameter_overrides were removed",
            )

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
            ]["runtime_kind"] = "native"
            _rehash_request(mismatched_runtime_kind)
            _run(
                args,
                mismatched_runtime_kind,
                temporary / "mismatched-runtime-kind",
                expect_error="configuration.runtime_kind is unsupported: native",
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

        if args.stop_limit_names:
            temporary.joinpath("stop-limits").mkdir()
            _require_stop_limits_by_joint_name(
                args, request, temporary / "stop-limits"
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
        _check_sealed_preparation(args, temporary / "valid" / "activation", temporary / "sealed")
        if args.runtime_identity:
            replay = _run(args, request, temporary / "exact-replay",
                          publish_canonical_empty=True)
            assert replay == prepared
            # A repeated Load adopts the exact stored closure in place.
            same_root = _run(args, request, temporary / "valid",
                             publish_canonical_empty=True, reuse_root=True)
            assert same_root == prepared
            other_runtime = copy.deepcopy(request)
            other_runtime["runtime_cell_id"] = "another_runtime"
            other = _run(args, other_runtime, temporary / "other-runtime")
            assert other is not None
            assert other["captured_input_digest"] == prepared["captured_input_digest"]
            assert other["origin"]["source_document_digest"] == prepared["origin"]["source_document_digest"]
            for field in ("prepared_activation_id", "retained_bundle_id", "graph_revision"):
                assert other[field] != prepared[field], field
            forbidden = copy.deepcopy(request)
            forbidden["deployment"]["cell_id"] = "authored_override"
            _rehash_request(forbidden)
            _run(args, forbidden, temporary / "authored-cell",
                 expect_error="unexpected field cell_id")
            missing = copy.deepcopy(request)
            del missing["runtime_cell_id"]
            _run(args, missing, temporary / "missing-runtime",
                 expect_error="missing runtime_cell_id")
            return 0
        if not args.negative_matrix:
            if len(request["deployment"]["components"]) == 1:
                dual_path = "examples/ur5e_lite6_wall_grouped.scenario"
                _, _, dual_request = _scenario_request(workspace, dual_path)
                if len(dual_request["deployment"]["components"]) != 2:
                    raise AssertionError(
                        "stable-host regression requires the maintained dual fixture"
                    )
                # Portable Scenarios naturally compile for the same runtime;
                # never rewrite an authored Scenario to make switching work.
                assert dual_request["runtime_cell_id"] == request["runtime_cell_id"]
                dual_prepared = _run(
                    args,
                    dual_request,
                    temporary / "stable-host-dual",
                    publish_canonical_empty=True,
                )
                assert dual_prepared is not None
                single_host_id = _native_host_identity(prepared)
                dual_host_id = _native_host_identity(dual_prepared)
                # The shared ROS host role is independent of component membership.
                expected_host_id = "robot_cell.ros2_control_execution"
                if (
                    single_host_id != expected_host_id
                    or dual_host_id != expected_host_id
                ):
                    raise AssertionError(
                        "native host identity changed with component membership: "
                        f"single={single_host_id!r}, dual={dual_host_id!r}"
                    )
                # The native host product was infrastructure-only and stayed
                # identical across membership; the ROS runtime product embeds
                # the unit's component configuration (Q68), so only its
                # identity, profile and revision stay stable.
                single_product = _native_host_products(prepared)
                dual_product = _native_host_products(dual_prepared)
                for field in ("runtime_unit_id",):
                    if single_product.get(field) != dual_product.get(field):
                        raise AssertionError(
                            "execution host product changed identity with "
                            "component membership"
                        )
            changed = _request(
                scenario_path,
                document,
                _program(workspace, document["program_draft"]),
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

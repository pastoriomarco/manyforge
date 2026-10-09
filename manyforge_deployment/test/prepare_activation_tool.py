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
# The compact receipt bound of Headless's ActivationPreparer.
MAX_ACTIVATION_PREPARATION_OUTPUT_BYTES = 1024 * 1024

from manyforge_behavior.canonical_artifact import (
    canonical_json_bytes,
    compute_content_identity,
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
    parser.add_argument("--stop-limit-names", action="store_true")
    parser.add_argument("--source-closure-rejections", action="store_true")
    parser.add_argument("--runtime-identity", action="store_true")
    parser.add_argument("--no-program", action="store_true")
    parser.add_argument("--ros2-control", action="store_true")
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
    }
    request = {
        "schema_version": "manyforge.activation_preparation_request.v3",
        "runtime_cell_id": "activation_test_runtime",
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
    }
    if manifest.get("api_version") != "manyforge.scenario.project.v2":
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
    if value["source_document_digest"] != request["source_document_digest"]:
        raise AssertionError("preparer changed the source-document identity")
    if value["captured_input_digest"] != request["captured_input_digest"]:
        raise AssertionError("preparer changed the captured-input identity")
    if value.get("schema_version") != "manyforge.prepared_activation_input.v5":
        raise AssertionError("preparer did not return the v5 prepared activation")
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
    family_id = {
        row["runtime_restoration_profile_id"]: row["model_family_id"]
        for row in sources["runtime_restoration_profiles"]
    }[component["component_profile_id"]]
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


def _check_ros_preparation(
    args: argparse.Namespace,
    request: dict[str, object],
    scenario_path: Path,
    document: dict[str, object],
    root: Path,
) -> None:
    prepared = _run(args, request, root / "ros-valid")
    assert prepared is not None
    bundle = prepared["activation_payload"]["retained_bundle"]
    graph = bundle["cell_graph"]
    component_id = "isaac_ur10e_light_robot"
    runtime_id = f"{component_id}.ros2_control_execution"
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
         expect_error="native runtime configuration for ros2_control profile")
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
        ("examples/lite6_pick_and_place.scenario", "mixed native and ROS runtime compositions"),
        ("examples/isaac-lite6-pick-place.scenario", "authored ROS composition requires one UR10e aggregate profile"),
    ):
        _, additional, _ = _scenario_request(args.workspace, scenario)
        unsupported = copy.deepcopy(document)
        unsupported["deployment_draft"]["components"].extend(
            additional["deployment_draft"]["components"]
        )
        unsupported["deployment_draft"]["components"].sort(
            key=lambda component: component["component_id"]
        )
        combined = _request(scenario_path, unsupported, request["program"])
        _run(args, combined, root / Path(scenario).stem, expect_error=expected_error)
    for separator in ("-", "."):
        invalid_namespace = copy.deepcopy(document)
        invalid_namespace["deployment_draft"]["components"][0]["component_id"] = (
            f"isaac{separator}ur10e"
        )
        renamed = _request(scenario_path, invalid_namespace, request["program"])
        _run(args, renamed, root / f"ros-invalid-namespace-{separator}",
             expect_error="must be one ROS name token")


def main() -> int:
    args = _arguments()
    workspace = args.workspace.resolve()
    scenario_path, document, request = _scenario_request(workspace, args.scenario)
    with tempfile.TemporaryDirectory(prefix="manyforge-prepare-test-") as raw:
        temporary = Path(raw)
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
            assert other["source_document_digest"] == prepared["source_document_digest"]
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

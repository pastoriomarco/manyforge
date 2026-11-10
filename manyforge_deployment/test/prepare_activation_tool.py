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
    parser.add_argument("--real-measured", action="store_true")
    parser.add_argument("--tool-split", action="store_true")
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
    }
    request = {
        "schema_version": "manyforge.activation_preparation_request.v4",
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
    }
    if manifest.get("api_version") != "manyforge.scenario.project.v3":
        raise AssertionError("test input is not a v3 Scenario project; re-author it with direct references")
    if (scenario_path / "assignment.yaml").exists():
        raise AssertionError("Scenario assignment is unsupported; re-author it with direct references")
    return scenario_path, document, _request(
        scenario_path, document, _program(workspace, document["program_draft"])
    )


def _native_host_identity(prepared: dict[str, object]) -> str:
    bundle = prepared["activation_payload"]["retained_bundle"]
    runtime_units = bundle["cell_graph"]["runtime_units"]
    if len(runtime_units) != 1:
        raise AssertionError("prepared Deployment does not have one execution host")
    runtime = runtime_units[0]
    # Offline Scenarios run on the mock ROS lane.
    if runtime["runtime_profile_id"] != "manyforge.ros2_control_execution_runtime":
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
        or candidate_host.get("runtime_profile_id")
        != "manyforge.ros2_control_execution_runtime"
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
        raise AssertionError("execution host configuration is not an object")
    if candidate_host["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime":
        # The retained host keeps its identity and launch product; its
        # configuration is component data and differs by design.
        assert {key: value for key, value in empty_host.items() if key != "configuration"} == {
            key: value for key, value in candidate_host.items() if key != "configuration"
        }
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
    expect_diagnostic: str | None = None,
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
    if expect_diagnostic is not None:
        if result.returncode != 2:
            raise AssertionError(f"expected typed reference refusal; return={result.returncode}, stderr={result.stderr!r}")
        refusal = json.loads(result.stdout)
        if refusal.get("code") != "program_reference_validation_failed" or not any(
            row.get("code") == expect_diagnostic for row in refusal.get("diagnostics", [])
        ):
            raise AssertionError(f"missing reference diagnostic {expect_diagnostic!r}: {refusal!r}")
        return None
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
    if value.get("schema_version") != "manyforge.prepared_activation_input.v7":
        raise AssertionError("preparer did not return the v7 prepared activation")
    program_outcome = value.get("program_outcome")
    if (
        not isinstance(program_outcome, dict)
        or program_outcome.get("kind") != expected_program_outcome_kind
    ):
        raise AssertionError(
            "preparer returned an unexpected Program outcome: "
            f"{program_outcome!r}"
        )
    payload = value.get("activation_payload")
    if not isinstance(payload, dict) or set(payload) != {
        "catalog_artifacts",
        "program",
        "program_binding",
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


def _component_model(workspace: Path, model_id: str) -> tuple[dict[str, object], dict[str, object]]:
    """The model family row and descriptor of one component model id."""
    sources = load_strict_json_yaml(
        workspace
        / "manyforge_deployment/resources/maintained/maintained_deployment_sources.v1.json"
    )
    for family in sources["model_families"]:
        path = family.get("component_descriptor")
        if path is None:
            continue
        descriptor = load_strict_json_yaml(workspace / path)
        if descriptor["id"] == model_id:
            return family, descriptor
    raise AssertionError(f"no model family for component model {model_id}")


def _profile_family(workspace: Path, component: dict[str, object]) -> str:
    """The model family of the component model a component selects."""
    return _component_model(workspace, component["component_profile_id"])[0][
        "model_family_id"]


def _lane_row(workspace: Path, component: dict[str, object], lane: str | None = None) -> str:
    """The canonical JSON of the component model's `lanes.<lane>` row (a
    machine member names none: pass its owner's lane)."""
    descriptor = _component_model(workspace, component["component_profile_id"])[1]
    return canonical_json_bytes(
        descriptor["lanes"][lane or component["lane"]]).decode("utf-8")


def _joints(workspace: Path, component: dict[str, object]) -> dict[str, object]:
    """A controller profile's compiled joints: an arm's `arm_joints` in
    controller order, a tool's one gripper joint (`fingers`)."""
    family = _component_model(workspace, component["component_profile_id"])[0]
    roles = {
        row["resource_role_id"]: row["source_joint_names"]
        for row in family["semantics"]["resource_role_bindings"]
    }
    if "arm_joints" in roles:
        return {"joint_names": roles["arm_joints"]}
    return {"gripper_joint_name": roles["fingers"][0]}


def _without_descriptions(rows: list[dict[str, object]]) -> list[dict[str, object]]:
    """Controller profile rows without their compiled description, after
    checking each is a URDF (its bytes are checked against the execution
    host's derivation by test_component_description)."""
    for row in rows:
        description = row["description"]
        assert isinstance(description, str) and "<robot" in description
        assert description.rstrip().endswith("</robot>")
    return [{key: value for key, value in row.items() if key != "description"} for row in rows]


def _row(deployment: dict[str, object], component_id: str) -> dict[str, object]:
    return next(row for row in deployment["components"] if row["component_id"] == component_id)


def _robot(deployment: dict[str, object]) -> dict[str, object]:
    """The first robot row: a machine's arm member or a plain robot."""
    return next(row for row in deployment["components"]
                if "arm" in row.get("target_settings", {}))


def _lane(deployment: dict[str, object], component: dict[str, object]) -> str:
    """A component's lane; a machine member runs on its owner's."""
    owner = component.get("owner")
    return component["lane"] if owner is None else _row(deployment, owner)["lane"]


def _arm_source_joints(workspace: Path, component: dict[str, object]) -> list[str]:
    return list(_joints(workspace, component)["joint_names"])


def _arm_stop_cancel(component: dict[str, object]) -> dict[str, object]:
    """The `max_deceleration_on_cancel_rad_s2` member an arm's controller
    profile row carries (its authored operator Stop decelerations, sorted by
    source joint); nothing for a tool."""
    arm = component.get("target_settings", {}).get("arm")
    if arm is None:
        return {}
    return {"max_deceleration_on_cancel_rad_s2": sorted(
        ({"max_deceleration_rad_s2": row["max_deceleration_rad_s2"],
          "source_joint_name": row["source_joint_name"]}
         for row in arm["controlled_stop"]["per_joint_limits"]),
        key=lambda row: row["source_joint_name"])}


def _require_launched_stop_decelerations(
    workspace: Path, deployment: dict[str, object], prepared: dict[str, object]
) -> None:
    """Every arm's controller profile row carries its authored operator
    Stop decelerations, and they are its compiled Stop rows' values (the JTC
    bound and the Kernel's Stop plan are one profile)."""
    compiled = _compiled_stop_limits(prepared)
    graph = prepared["activation_payload"]["retained_bundle"]["cell_graph"]
    profiles = {
        row["controllable_component_id"]: row
        for unit in graph["runtime_units"]
        if unit["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
        for row in unit["configuration"]["controller_profiles"]
    }
    for component in deployment["components"]:
        component_id = component["component_id"]
        profile = profiles.get(component_id)
        if profile is None:  # no controllers (an inactive member, an I/O bank)
            continue
        expected = _arm_stop_cancel(component)
        launched = {key: value for key, value in profile.items()
                    if key == "max_deceleration_on_cancel_rad_s2"}
        if launched != expected:
            raise AssertionError(
                f"{component_id} controller profile Stop decelerations {launched!r} "
                f"are not the authored {expected!r}")
        if not expected:
            continue
        joints = _arm_source_joints(workspace, component)
        stop_rows = {
            joint: compiled[f"{component_id}.arm.coordinate.{index:06d}"][0]
            for index, joint in enumerate(joints)
        }
        authored = {row["source_joint_name"]: row["max_deceleration_rad_s2"]
                    for row in expected["max_deceleration_on_cancel_rad_s2"]}
        if stop_rows != authored:
            raise AssertionError(
                f"{component_id} Stop rows {stop_rows!r} differ from its controller "
                f"profile {authored!r}")


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
    component = _robot(request["deployment"])
    component_id = component["component_id"]
    joints = _arm_source_joints(args.workspace.resolve(), component)
    if len(joints) < 3:
        raise AssertionError("Stop limit pairing test requires three arm joints")

    def with_limits(rows: list[dict[str, object]]) -> dict[str, object]:
        changed = copy.deepcopy(request)
        _row(changed["deployment"], component_id)["target_settings"]["arm"][
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


def _unmounted_ids(deployment: dict[str, object]) -> list[str]:
    """The components that are not a tool mounted through an
    `attachments` row (robots and model-free components). A machine counts
    once, as its arm member: its owner and I/O member rows are left out."""
    mounted = {row["child"]["component_id"] for row in deployment.get("attachments", [])}
    owners = {row["owner"] for row in deployment["components"] if "owner" in row}
    return [row["component_id"] for row in deployment["components"]
            if row["component_id"] not in mounted and row["component_id"] not in owners
            and not ("owner" in row and "model_source" not in row)]


def _require_gripper_target_semantics(
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

    # Each robot's tool owns its gripper row.
    pneumatic = target(request, "lite_gripper")
    position = target(request, "robotiq_2f_85")
    assert pneumatic["actuation_kind"] == "pneumatic" and len(pneumatic) == 13
    assert position["actuation_kind"] == "position_commanded" and len(position) == 13

    accepted = copy.deepcopy(request)
    _rehash_request(accepted)
    prepared = _run(args, accepted, root / "authored")
    assert prepared is not None
    units = prepared["activation_payload"]["retained_bundle"]["cell_graph"][
        "runtime_units"
    ]
    (unit,) = units
    assert unit["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
    retained = unit["configuration"]["gripper_target_configurations"]
    assert retained == [pneumatic, position]

    invalid_rows = []
    foreign_field = copy.deepcopy(request)
    target(foreign_field, "lite_gripper")["foreign_timeout_s"] = 1.0
    invalid_rows.append(("foreign-fourteenth-field", foreign_field))
    missing_base = copy.deepcopy(request)
    del target(missing_base, "lite_gripper")["description"]
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
    """Two ROS robots with catalog controller profiles compose (the first
    keeps its mounted Isaac tool)."""
    prepared = _run(args, request, root)
    assert prepared is not None
    graph = prepared["activation_payload"]["retained_bundle"]["cell_graph"]
    components = sorted(
        row["controllable_component_id"] for row in graph["controllable_components"]
    )
    assert components == [
        "isaac_ur10e_light_robot", "isaac_ur10e_second_robot", "robotiq_2f_140"]
    robots = ["isaac_ur10e_light_robot", "isaac_ur10e_second_robot"]
    assert len(graph["runtime_units"]) == 1
    runtime = graph["runtime_units"][0]
    assert runtime["runtime_profile_id"] == "manyforge.ros2_control_execution_runtime"
    configuration = runtime["configuration"]
    # Each component carries its own controller profile: its lane and
    # model family.
    lanes = {
        "isaac_ur10e_light_robot": ("isaac", "ur10e"),
        "isaac_ur10e_second_robot": ("mock", "ur10e"),
        "robotiq_2f_140": ("isaac", "robotiq_2f_140"),
    }
    deployed = {row["component_id"]: row for row in request["deployment"]["components"]}
    assert _without_descriptions(configuration["controller_profiles"]) == [
        {"controllable_component_id": component,
         "lane": lanes[component][0],
         "lane_row": _lane_row(args.workspace, deployed[component]),
         "model_family_id": lanes[component][1],
         "frame_prefix": next(row for row in request["deployment"]["components"]
                              if row["component_id"] == component).get("frame_prefix", ""),
         **_joints(args.workspace, deployed[component]),
         **_arm_stop_cancel(deployed[component])}
        for component in components
    ]
    assert "lane" not in configuration
    authored = [
        component["configuration"]["runtime_configuration"]
        for component in request["deployment"]["components"]
    ]
    # Each robot keeps its own initial state (component order), TCP-speed
    # maximum and fault Stop profile; no native-only field is introduced.
    assert configuration["initial_joint_positions_rad"] == [
        value for row in authored for value in row.get("initial_joint_positions_rad", [])
    ]
    assert [
        row["controllable_component_id"] for row in configuration["motion_target_limits"]
    ] == robots
    assert [
        row["controllable_component_id"]
        for row in configuration["default_fault_stop_profiles"]
    ] == robots
    assert "direct_control_joint_limits" not in configuration
    # Each component keeps its own authored gripper target rows.
    assert configuration["gripper_target_configurations"] == sorted(
        (row for component in authored
         for row in component.get("gripper_target_configurations", [])),
        key=lambda row: (row["controllable_component_id"], row["command_target_id"]),
    )
    # Each component keeps its own stream defaults.
    assert configuration["component_streams"] == sorted(
        (row for component in authored for row in component["component_streams"]),
        key=lambda row: (row["controllable_component_id"], row["stream_id"]),
    )


def _check_tool_split(
    args: argparse.Namespace,
    workspace: Path,
    scenario_path: Path,
    document: dict[str, object],
    request: dict[str, object],
    root: Path,
) -> None:
    """Slice B: mock_lite6_pneumatic_pick_and_place's lane-free pneumatic tool, mounted on
    the robot's flange, compiles (the split Scenario of slices A and D)."""
    prepared = _run(args, request, root / "tool-split")
    assert prepared is not None
    bundle = prepared["activation_payload"]["retained_bundle"]
    graph = bundle["cell_graph"]
    # The authored mount becomes one graph attachment (identity pose).
    (attachment,) = graph["component_attachments"]
    assert attachment["component_attachment_id"] == "lite_gripper"
    assert attachment["parent"] == {
        "attachment_interface_id": "flange", "controllable_component_id": "lite6_robot"}
    assert attachment["child"] == {
        "attachment_interface_id": "mount", "controllable_component_id": "lite_gripper"}
    assert attachment["child_interface_pose_in_parent_interface"] == {
        "orientation_xyzw": [0.0, 0.0, 0.0, 1.0], "position_m": [0.0, 0.0, 0.0]}
    components = {row["controllable_component_id"]: row for row in graph["controllable_components"]}
    tool = components["lite_gripper"]
    assert "root_pose_in_cell" not in tool
    assert "root_pose_in_cell" in components["lite6_robot"]
    assert tool["component_type_id"] == "lite_gripper.component"
    assert [row["model_role_id"] for row in tool["model_role_bindings"]] == ["tool_model"]
    # The robot no longer owns a gripper; the tool owns `gripper` only, as a
    # commanded display without its own controller.
    targets = {
        (row["controllable_component_id"], row["command_target_id"])
        for row in graph["control_bindings"]
    }
    assert targets == {("lite6_robot", "arm"), ("lite_gripper", "gripper")}
    for row in graph["control_bindings"]:
        if row["controllable_component_id"] == "lite_gripper":
            assert row["state_authority_id"] == "lite_gripper.gripper.commanded_state"
            assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
            assert row["provider_instance_id"] == "lite_gripper.ros2_control"
    (unit,) = graph["runtime_units"]
    configuration = unit["configuration"]
    assert [row["controllable_component_id"] for row in configuration["controller_profiles"]] == [
        "lite6_robot"]
    assert [row["controllable_component_id"]
            for row in configuration["gripper_target_configurations"]] == ["lite_gripper"]
    assert [(row["controllable_component_id"], row["stream_id"])
            for row in configuration["component_streams"]] == [("lite6_robot", "measured_joints")]
    # B-DEP: the mounted tool's TCP is one of the arm's robot TCPs (no `tcp` alias).
    binding = prepared["activation_payload"]["program_binding"]
    (arm,) = [row for row in binding["target_bindings"]
              if row["target"] == {"component_id": "lite6_robot", "command_target_id": "arm"}]
    (tcp,) = [row for row in arm["robot_tcps"]
              if row["entry"]["target"]["component_id"] == "lite_gripper"
              and row["entry"]["frame"]["frame_id"] == "tcp"]
    assert tcp["frame"]["model_instance_id"] == "lite_gripper.model"

    def refused(name: str, edit, expected: str) -> None:
        variant = copy.deepcopy(document)
        edit(variant["deployment_draft"])
        _run(args, _request(scenario_path, variant, _program(workspace, variant["program_draft"])),
             root / name, expect_error=expected)

    def tool_row(deployment: dict[str, object]) -> dict[str, object]:
        return next(row for row in deployment["components"] if row["component_id"] == "lite_gripper")

    def gripper_row(deployment: dict[str, object]) -> dict[str, object]:
        return tool_row(deployment)["configuration"]["runtime_configuration"][
            "gripper_target_configurations"][0]

    refused("tool-split-child-pose",
            lambda value: tool_row(value).update(root_pose_in_cell={
                "orientation_xyzw": [0.0, 0.0, 0.0, 1.0], "position_m": [0.0, 0.0, 0.0]}),
            "has unexpected field root_pose_in_cell")
    refused("tool-split-unknown-interface",
            lambda value: value["attachments"][0]["parent"].update(interface="wrist"),
            "attachment endpoint selects missing interface 'wrist'")
    refused("tool-split-position-gripper",
            lambda value: gripper_row(value).update(actuation_kind="position_commanded"),
            "tool_lane_required")
    refused("tool-split-no-pneumatic-io",
            lambda value: gripper_row(value).pop("pneumatic_io"),
            "tool_pneumatic_io_required")
    refused("tool-split-stream",
            lambda value: tool_row(value)["configuration"]["runtime_configuration"][
                "component_streams"].append({
                    "budget_ns": 250000000, "controllable_component_id": "lite_gripper",
                    "stream_id": "measured_joints"}),
            "is a lane-free tool")

    # Grasp confirmation is authored on the tool's gripper row; its pneumatic
    # evidence is the tool's own `pneumatic_io.inputs.workpiece_grasped`.
    confirmed = copy.deepcopy(document)
    gripper_row(confirmed["deployment_draft"])["grasp_confirmation"] = {"policy": "required"}
    prepared = _run(
        args, _request(scenario_path, confirmed, _program(workspace, confirmed["program_draft"])),
        root / "tool-split-grasp-confirmation")
    assert prepared is not None
    (unit,) = prepared["activation_payload"]["retained_bundle"]["cell_graph"]["runtime_units"]
    (row,) = unit["configuration"]["gripper_target_configurations"]
    assert row["controllable_component_id"] == "lite_gripper"
    assert row["grasp_confirmation"] == {"policy": "required"}

    def required_without_input(deployment: dict[str, object]) -> None:
        gripper_row(deployment)["grasp_confirmation"] = {"policy": "required"}
        gripper_row(deployment)["pneumatic_io"].pop("inputs")

    refused("tool-split-grasp-source-missing", required_without_input,
            "grasp_confirmation_source_missing")
    refused("tool-split-grasp-window-on-pneumatic",
            lambda value: gripper_row(value).update(grasp_confirmation={
                "policy": "not_required",
                "stall_window": {"min_position": 0.0, "max_position": 0.01}}),
            "grasp_confirmation_invalid")


def _without_pose_sources(value: dict[str, object], key: str) -> dict[str, object]:
    """Drop the pose-source components with their object links and streams."""
    result = copy.deepcopy(value)
    result[key]["components"] = [
        component for component in result[key]["components"]
        # A machine owner and an inactive member carry no runtime_kind.
        if component.get("configuration", {}).get("runtime_kind") != "pose_source"
    ]
    policies = result[key].get("policies", {})
    policies.pop("scene_pose_sources", None)
    policies.pop("component_streams", None)
    return result


def _check_pose_source_preparation(
    args: argparse.Namespace, request: dict[str, object], root: Path
) -> None:
    """The scenario's model-free pose source compiles beside the robot."""
    sources = [
        component for component in request["deployment"]["components"]
        if component.get("configuration", {}).get("runtime_kind") == "pose_source"
    ]
    assert [row["component_id"] for row in sources] == ["isaac_scene_objects"]
    prepared = _run(args, request, root / "ros-pose-source")
    assert prepared is not None
    graph = prepared["activation_payload"]["retained_bundle"]["cell_graph"]
    configuration = graph["runtime_units"][0]["configuration"]
    # The authored component + its object links expand into today's row:
    # one simulation_entities row per linked entity (policy "scene", header
    # copied verbatim) plus the
    # overlay TCP row; lifetimes are the stream budgets.
    authored = sources[0]["configuration"]["runtime_configuration"]
    links = request["deployment"]["policies"]["scene_pose_sources"]
    budgets = {
        row["stream_id"]: row["budget_ns"]
        for row in request["deployment"]["policies"].get("component_streams", [])
    }
    [row] = configuration["pose_sources"]
    assert row["controllable_component_id"] == "isaac_scene_objects"
    assert row["simulation_poll_period_ms"] == authored["simulation"]["poll_period_ms"]
    objects = {
        entity["object_id"]: entity for entity in row["simulation_entities"]
        if entity["eligible"]
    }
    assert sorted(objects) == sorted(link["scene_resource_id"] for link in links)
    for link in links:
        entity = objects[link["scene_resource_id"]]
        assert entity["policy_id"] == "scene"
        key = link["source_key"]["simulation_entity"]
        assert entity["entity"] == key["entity"]
        assert entity["source_header_frame_id"] == key["source_header_frame_id"]
        assert entity["lifetime_ns"] == budgets.get(
            f"poses/{link['scene_resource_id']}", authored["stream_budget_ns"])
    overlay = authored["robot_overlay"]
    assert row["joint_state"]["object_id"] == overlay["robot_component_id"]
    assert [
        entity["policy_id"] for entity in row["simulation_entities"]
        if not entity["eligible"]
    ] == ["viewer.measured_tcp"]
    domains = [row for row in graph["control_domains"] if row.get("kind") == "provider"]
    assert [row["provider"]["provider_manifest_id"] for row in domains] == [
        "manyforge.provider.pose_source"]
    assert all(name.startswith("poses/") for name in domains[0]["provider"]["signal_names"])


def _check_ros_preparation(
    args: argparse.Namespace,
    request: dict[str, object],
    scenario_path: Path,
    document: dict[str, object],
    root: Path,
) -> None:
    _check_pose_source_preparation(args, request, root)
    # The robot checks below are about the one ROS robot component.
    request = _without_pose_sources(request, "deployment")
    _rehash_request(request)
    document = _without_pose_sources(document, "deployment_draft")
    legacy_world = copy.deepcopy(request)
    legacy_world["deployment"]["isaac_world"] = "lite6_grf_ros2"
    _rehash_request(legacy_world)
    _run(args, legacy_world, root / "ros-legacy-world-selector",
         expect_error="isaac_world")
    prepared = _run(args, request, root / "ros-valid")
    assert prepared is not None
    bundle = prepared["activation_payload"]["retained_bundle"]
    # Older authored projects remain valid; omission materializes the
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
    ] == 0.5
    # The robot's mounted tool (components[1]) adds its own
    # controller profile, gripper target rows and streams to the one unit.
    tool_component = request["deployment"]["components"][1]
    tool_id = tool_component["component_id"]
    assert tool_id == "robotiq_2f_140"
    assert request["deployment"]["attachments"] == [{
        "parent": {"component_id": component_id, "interface": "flange"},
        "child": {"component_id": tool_id, "interface": "mount"},
    }]
    tool_configuration = copy.deepcopy(
        tool_component["configuration"]["runtime_configuration"])
    expected_configuration["motion_target_limits"] = [{
        "controllable_component_id": component_id,
        "command_target_id": "arm",
        "maximum_tcp_linear_speed_mps": 0.5,
        # Omission compiles the ordinary-jerk switch off.
        "ordinary_motion_jerk": {
            "enabled": False, "joint_limits": [], "preset_reference_time_s": 1.0,
        },
    }]
    # The authored lane and the profile's model family become the
    # component's controller profile row inside the one ROS runtime unit.
    expected_configuration["controller_profiles"] = [{
        "controllable_component_id": component_id,
        "lane": request["deployment"]["components"][0]["lane"],
        "lane_row": _lane_row(args.workspace, request["deployment"]["components"][0]),
        "model_family_id": _profile_family(
            args.workspace, request["deployment"]["components"][0]),
        "frame_prefix": request["deployment"]["components"][0].get("frame_prefix", ""),
        **_joints(args.workspace, request["deployment"]["components"][0]),
        **_arm_stop_cancel(request["deployment"]["components"][0]),
    }, {
        "controllable_component_id": tool_id,
        "lane": tool_component["lane"],
        "lane_row": _lane_row(args.workspace, tool_component),
        "model_family_id": _profile_family(args.workspace, tool_component),
        "frame_prefix": tool_component.get("frame_prefix", ""),
        **_joints(args.workspace, tool_component),
    }]
    expected_configuration["gripper_target_configurations"] = sorted(
        tool_configuration["gripper_target_configurations"],
        key=lambda row: (row["controllable_component_id"], row["command_target_id"]))
    expected_configuration["component_streams"] = sorted(
        expected_configuration["component_streams"] + tool_configuration["component_streams"],
        key=lambda row: (row["controllable_component_id"], row["stream_id"]))
    # The authored fault Stop profile is carried unchanged (its joint map
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
    # The omitted Cartesian timing compiles to its schema default.
    expected_configuration["cartesian_timing"] = "constant_speed"
    assert {**runtime["configuration"], "controller_profiles": _without_descriptions(
        runtime["configuration"]["controller_profiles"])} == expected_configuration
    # The cell-wide `mode` lane flag is deleted.
    assert "mode" not in runtime["configuration"]
    assert [row["component_type_id"] for row in graph["controllable_components"]] == [
        "ur10e.component", "robotiq_2f_140.component"]
    assert {row["control_coordinator_endpoint_role"] for row in graph["control_domains"]} == {
        "ros2_control_execution_status"
    }
    bindings = graph["control_bindings"]
    # The robot owns `arm`, its mounted tool `gripper`.
    owners = {"arm": component_id, "gripper": tool_id}
    assert {
        (row["controllable_component_id"], row["command_target_id"]) for row in bindings
    } == {(component_id, "arm"), (tool_id, "gripper")}
    for row in bindings:
        target = row["command_target_id"]
        owner = owners[target]
        assert row["provider_runtime_unit_id"] == runtime_id
        assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
        assert row["provider_instance_id"] == f"{owner}.ros2_control"
        assert row["state_authority_class"] == "commanded"
        assert row["state_authority_id"] == f"{owner}.{target}.commanded_sim_state"
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
    arm = next(row for row in bindings if row["capability_id"] == "execution.trajectory.execute")
    gripper = next(row for row in bindings if row["capability_id"] == "tool.gripper.command")
    assert len(arm["resource_bindings"]) == 6
    assert gripper["resource_bindings"] == [{
        "resource_role_id": "fingers",
        "resource_id": f"{tool_id}.gripper.coordinate.000000",
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
        assert row["provider_instance_id"] == (
            f"{owners[row['target']['command_target_id']]}.ros2_control")
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
         expect_error="configuration.runtime_kind is unsupported: native")
    invalid_config = copy.deepcopy(request)
    invalid_config["deployment"]["components"][0]["configuration"][
        "runtime_configuration"
    ]["motion_target_limits"] = []
    _rehash_request(invalid_config)
    _run(args, invalid_config, root / "ros-native-configuration",
         expect_error="motion_target_limits")
    # The authored lane is one of the lanes the profile row declares.
    unknown_lane = copy.deepcopy(request)
    unknown_lane["deployment"]["components"][0]["lane"] = "real"
    _rehash_request(unknown_lane)
    _run(args, unknown_lane, root / "ros-unknown-lane",
         expect_error="component_lane_unknown")
    missing_lane = copy.deepcopy(request)
    del missing_lane["deployment"]["components"][0]["lane"]
    _rehash_request(missing_lane)
    _run(args, missing_lane, root / "ros-missing-lane",
         expect_error="component_lane_required")
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
    # Authored ordinary jerk compiles into the ROS motion limit row.
    authored_jerk = copy.deepcopy(request)
    jerk_settings = {
        "enabled": True,
        "joint_limits": [{"source_joint_name": "shoulder_pan_joint",
                          "override_rad_s3": 40.0}],
        "preset_reference_time_s": 0.5,
    }
    authored_jerk["deployment"]["components"][0]["target_settings"]["arm"][
        "ordinary_motion_jerk"] = jerk_settings
    _rehash_request(authored_jerk)
    jerk_result = _run(args, authored_jerk, root / "ros-authored-jerk")
    assert jerk_result is not None
    assert (jerk_result["activation_payload"]["retained_bundle"]["cell_graph"]
            ["runtime_units"][0]["configuration"]["motion_target_limits"]
            == [{**expected_configuration["motion_target_limits"][0],
                 "ordinary_motion_jerk": jerk_settings}])

    for scenario, expected_error in (
        # The offline Lite 6 is a mock ROS component now; the
        # pair is refused only by the existing shared runtime-unit cadence
        # agreement (50 ms vs the UR10e light scene's 100 ms).
        (
            "examples/mock_lite6_pick_and_place.scenario",
            "disagree on collision_monitor_period_ms",
        ),
        # Two ROS robots pass the composition rule; this pair is refused
        # only by the existing shared runtime-unit cadence agreement.
        (
            "examples/sim_lite6_pick_and_place.scenario",
            "exclusive Isaac topic collision",
        ),
    ):
        _, additional, _ = _scenario_request(args.workspace, scenario)
        additional = _without_pose_sources(additional, "deployment_draft")
        unsupported = copy.deepcopy(document)
        unsupported["deployment_draft"]["components"].extend(
            additional["deployment_draft"]["components"]
        )
        # The added robot brings its mounted tool's attachment.
        unsupported["deployment_draft"]["attachments"].extend(
            additional["deployment_draft"]["attachments"]
        )
        added_ids = {row["component_id"] for row in additional["deployment_draft"]["components"]}
        for added in unsupported["deployment_draft"]["components"]:
            # A machine's owner and I/O members carry no frames.
            if added["component_id"] in added_ids and "model_source" in added:
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
    # Any number of ROS components with catalog controller profiles is
    # accepted by the compiler (no hard-coded single-UR10e rule).
    two_robots = copy.deepcopy(document)
    second_robot = copy.deepcopy(two_robots["deployment_draft"]["components"][0])
    second_robot["component_id"] = "isaac_ur10e_second_robot"
    # The copy declares the same measured_joints stream row as the
    # maintained robot profile (the copy carries no tool).
    second_runtime = second_robot["configuration"]["runtime_configuration"]
    assert "gripper_target_configurations" not in second_runtime
    for row in second_runtime["component_streams"]:
        row["controllable_component_id"] = "isaac_ur10e_second_robot"
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
    second_robot["lane"] = "mock"
    _check_two_ros_robots(
        args, _request(scenario_path, two_robots, request["program"]),
        root / "ros-two-robots",
    )
    # One ROS runtime unit hosts robots with different controller
    # profiles (Lite 6 + UR10e) when the cell-wide cadence agrees; each
    # component carries its own profile.
    _, lite6_document, _ = _scenario_request(
        args.workspace, "examples/sim_lite6_pick_and_place.scenario"
    )
    lite6_document = _without_pose_sources(lite6_document, "deployment_draft")
    # The Lite 6 comes as its machine (owner, arm, deactivated I/O).
    lite6_robot = copy.deepcopy(_row(lite6_document["deployment_draft"], "isaac_lite6_robot"))
    lite6_machine = [copy.deepcopy(row) for row in lite6_document["deployment_draft"]["components"]
                     if row["component_id"] == "isaac_lite6"
                     or row.get("owner") == "isaac_lite6" and "model_source" not in row]
    lite6_robot["frame_prefix"] = "lite6/"
    lite6_robot["configuration"]["runtime_configuration"][
        "collision_monitor_period_ms"
    ] = document["deployment_draft"]["components"][0]["configuration"][
        "runtime_configuration"
    ]["collision_monitor_period_ms"]
    two_profiles = copy.deepcopy(document)
    ur10e_mock = two_profiles["deployment_draft"]["components"][0]
    ur10e_mock["lane"] = "mock"
    # The UR10e's mounted tool moves to Mock with it.
    tool_mock = two_profiles["deployment_draft"]["components"][1]
    assert tool_mock["component_id"] == "robotiq_2f_140"
    tool_mock["lane"] = "mock"
    two_profiles["deployment_draft"]["components"].extend([lite6_robot, *lite6_machine])
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
    mixed_rows = {row["component_id"]: row
                  for row in two_profiles["deployment_draft"]["components"]}
    assert _without_descriptions(
        mixed_runtime[0]["configuration"]["controller_profiles"]) == sorted([
        {"controllable_component_id": "isaac_lite6_robot", "lane": "isaac",
         "lane_row": _lane_row(args.workspace, mixed_rows["isaac_lite6_robot"], "isaac"),
         "model_family_id": "lite6", "frame_prefix": "lite6/",
         "owner_component_id": "isaac_lite6", "member_role": "arm",
         **_joints(args.workspace, mixed_rows["isaac_lite6_robot"]),
         **_arm_stop_cancel(mixed_rows["isaac_lite6_robot"])},
        {"controllable_component_id": "isaac_ur10e_light_robot", "lane": "mock",
         "lane_row": _lane_row(args.workspace, mixed_rows["isaac_ur10e_light_robot"]),
         "model_family_id": "ur10e", "frame_prefix": "",
         **_joints(args.workspace, mixed_rows["isaac_ur10e_light_robot"]),
         **_arm_stop_cancel(mixed_rows["isaac_ur10e_light_robot"])},
        {"controllable_component_id": "robotiq_2f_140", "lane": "mock",
         "lane_row": _lane_row(args.workspace, mixed_rows["robotiq_2f_140"]),
         "model_family_id": "robotiq_2f_140", "frame_prefix": "",
         **_joints(args.workspace, mixed_rows["robotiq_2f_140"])},
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
            if "model_source" not in component:
                continue  # a machine's owner and I/O members carry no frames
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
        args.workspace, "examples/mock_dual_arm_shared_group.scenario")
    assert all(component.get("frame_prefix", "") == ""
               for component in repeated_world_request["deployment"]["components"])
    for source in repeated_world_request["model_source_closure"]["components"]:
        # Mounted tools carry no world link of their own.
        if source["component_id"] not in {"lite6_robot", "ur5e_robot"}:
            continue
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
        # The mounted tool's attachment follows the renamed robot.
        for attachment in invalid_namespace["deployment_draft"]["attachments"]:
            if attachment["parent"]["component_id"] == component_id:
                attachment["parent"]["component_id"] = f"isaac{separator}ur10e"
        renamed = _request(scenario_path, invalid_namespace, request["program"])
        _run(args, renamed, root / f"ros-invalid-namespace-{separator}",
             expect_error="must be one ROS name token")


def _write_manager_ros_apply_fixture(
    args: argparse.Namespace, request: dict[str, object], *, isaac: bool = False
) -> None:
    """Materialize one real ROS Scenario through the production tool."""
    # One robot, plus the tool mounted on it.
    components = [
        row for row in request["deployment"]["components"]
        if row["component_id"] in _unmounted_ids(request["deployment"])
    ]
    if len(components) != 1:
        raise AssertionError("Manager ROS fixture requires exactly one robot component")
    configuration = components[0]["configuration"]
    runtime_configuration = configuration["runtime_configuration"]
    if configuration["runtime_kind"] != "ros2_control":
        raise AssertionError("Manager ROS fixture must use the ROS runtime")
    if _lane(request["deployment"], components[0]) != ("isaac" if isaac else "mock"):
        raise AssertionError("Manager ROS fixture must use the Isaac or Mock lane")
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
    """Read actual CM/JSB facts for the existing Manager process fixture.

    Component model v2: the arm and the gripper mounted on it are separate
    components, each with its own Mock ros2_control launch (namespace
    `/manyforge/<component_id>`). The arm's launch is recorded as a
    controller launch artifact; the gripper's controller manager is read in
    its own namespace.
    """
    import math
    import socket
    import time
    import rclpy
    from controller_manager_msgs.srv import ListControllers
    from sensor_msgs.msg import JointState
    from rclpy.qos import qos_profile_sensor_data

    parser = argparse.ArgumentParser()
    parser.add_argument("--execution-root", required=True, type=Path)
    parser.add_argument("--component", required=True)
    parser.add_argument("--gripper-component", required=True)
    parser.add_argument("--state", choices=("inactive", "active"), required=True)
    parser.add_argument("--label", required=True)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args(sys.argv[2:])
    records = [json.loads(p.read_bytes()) for p in
               (args.execution_root / "controller-launch-artifacts").rglob("controller_launch_artifacts.json")]
    records = [row for row in records if row["controllable_component_id"] == args.component]
    if len(records) != 1:
        raise AssertionError("Expected exactly one actual arm launch artifact")
    artifact = records[0]
    launch = dict(value.split(":=", 1) for value in artifact["launch_arguments"])
    arm_namespace = launch["runtime_namespace"].rstrip("/")
    arm = launch["arm_controller_name"]
    if raw_sha256_digest(Path(artifact["controllers_file"]).read_bytes()) != artifact["controllers_sha256"]:
        raise AssertionError("Actual controllers YAML does not match its writing receipt")
    gripper_namespace = "/manyforge/" + args.gripper_component
    controller_manager = launch["controller_manager_name"]

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
    status = command({"api_version": "manyforge.execution_command.v3", "command": "targets"})
    components = {row["controllable_component_id"]: row for row in status["targets"]["components"]}
    if args.component not in components or args.gripper_component not in components:
        raise AssertionError("Installed target components differ from the observed ones")
    targets = [row for row in components[args.gripper_component]["targets"] if row["presentation"] == "gripper"]
    if len(targets) != 1:
        raise AssertionError("Expected one actual installed model-backed gripper target")
    target, direct = targets[0], targets[0]["command"]
    if direct["resource"]["source_kind"] != "model_role":
        raise AssertionError("Gripper target is not bound to its model joint")
    joint = direct["resource"]["source_coordinate_name"]

    rclpy.init()
    node = rclpy.create_node("manyforge_manager_fixture_observation")
    deadline = time.monotonic() + 8.0
    sample = None
    topic = None
    subscription = None
    arm_rows = gripper_rows = None
    gripper = None

    def pause() -> None:
        rclpy.spin_once(node, timeout_sec=0.05)
        # spin_once may return immediately; failed observations need a real pause.
        time.sleep(min(0.05, max(0.0, deadline - time.monotonic())))

    def list_controllers(client) -> dict[str, dict[str, str]] | None:
        if not client.wait_for_service(timeout_sec=min(0.1, max(0.0, deadline - time.monotonic()))):
            return None
        future = client.call_async(ListControllers.Request())
        rclpy.spin_until_future_complete(node, future, timeout_sec=max(0.0, deadline - time.monotonic()))
        if not future.done() or future.result() is None:
            return None
        return {row.name: {"state": row.state, "type": row.type} for row in future.result().controller}

    def broadcasters(rows) -> list[str]:
        return [name for name, row in (rows or {}).items()
                if row["type"] == "joint_state_broadcaster/JointStateBroadcaster" and row["state"] == "active"]

    def settled() -> bool:
        return (arm_rows is not None and gripper_rows is not None and gripper is not None
                and arm_rows.get(arm, {}).get("state") == args.state
                and gripper_rows[gripper]["state"] == args.state
                and len(broadcasters(arm_rows)) == 1 and len(broadcasters(gripper_rows)) == 1)

    try:
        arm_client = node.create_client(ListControllers, arm_namespace + "/" + controller_manager + "/list_controllers")
        gripper_client = node.create_client(ListControllers, gripper_namespace + "/" + controller_manager + "/list_controllers")
        while time.monotonic() < deadline:
            arm_rows = list_controllers(arm_client)
            gripper_rows = list_controllers(gripper_client)
            grippers = [name for name, row in (gripper_rows or {}).items()
                        if row["type"] == "parallel_gripper_action_controller/GripperActionController"]
            gripper = grippers[0] if len(grippers) == 1 else None
            if not settled():
                pause()
                continue
            if subscription is None:
                jsb = broadcasters(gripper_rows)[0]
                topics = [name for name, types in node.get_topic_names_and_types()
                          if "sensor_msgs/msg/JointState" in types and any(
                              info.node_name == jsb and info.node_namespace.rstrip("/") == gripper_namespace
                              for info in node.get_publishers_info_by_topic(name))]
                if len(topics) != 1:
                    pause()
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
        if not settled() or sample is None or not 0 <= node.get_clock().now().nanoseconds - sample["stamp_ns"] <= 1_000_000_000:
            raise AssertionError({"expected_state": args.state, "arm_controllers": arm_rows,
                                  "gripper_controllers": gripper_rows, "topic": topic, "fresh_gripper_sample": sample})
        evidence = {"label": args.label,
                    "controller_manager": arm_namespace + "/" + controller_manager,
                    "gripper_controller_manager": gripper_namespace + "/" + controller_manager,
                    "arm_controller": arm, "gripper_controller": gripper,
                    # Arm and gripper controller names are distinct; both sets
                    # are kept whole beside the merged lookup.
                    "controllers": {**arm_rows, **{gripper: gripper_rows[gripper]}},
                    "arm_controllers": arm_rows, "gripper_controllers": gripper_rows,
                    "joint_state_topic": topic, "fresh_gripper_sample": {**sample, "stamp_ns": str(sample["stamp_ns"]), "received_ns": str(sample["received_ns"])},
                    "controllers_sha256": artifact["controllers_sha256"]}
        if args.state == "inactive":
            # Same production intrinsic manual producer as test_managed_execution_path.
            identity, manual = status["identity"], direct["manual_control"]
            kind = direct["resource"]["coordinate_kind"]
            position = {"kind": kind, "value_rad" if kind == "angular_rad" else "value_m": sample["position"]}
            operation = {"request_context": {"request_id": "empty-ros-one-mock." + args.label + ".gripper", "request_owner_epoch": identity["owner_epoch"], "source_label": "manager_ros_apply_fixture", "correlation_id": None},
                         **{key: manual[key] for key in ("program_id", "program_revision", "program_binding_id", "program_run_id")},
                         "expected_graph_revision": identity["graph_revision"], "expected_retained_bundle_id": identity["retained_bundle_id"], "expected_scene_revision": None,
                         "participants": [{"target": direct["target"], "required_capability_id": target["operation_capability_id"], "action_span": None,
                                           "payload": {"kind": "capability_command", "command_schema_id": direct["command_schema_id"], "command_schema_revision": direct["command_schema_revision"],
                                                       "command_value": {"kind": "gripper_position", "position": position, "max_effort": None}, "completion_timeout_s": 2.0,
                                                       "caller_options": [{"option_id": "command_duration_s", "value_s": 0.5}]}}], "coordination_policy": "independent"}
            envelope = {"api_version": "manyforge.execution_command.v3", "command": "submit", "operation": operation,
                        "attribution": {"assurance": "unverified", "principal_id": None, "transport": "http"}}
            refusal = command(envelope, allow_refusal=True)
            # Existing Kernel checks route publication/readback before component eligibility.
            # Declared/no-W and Disabled must refuse through one of those actual gates;
            # a prepared (not enabled) v2 component refuses with component_not_enabled.
            closed_admission_reasons = {"runtime_route_publication_required", "runtime_route_configuration_not_ready", "component_command_not_eligible", "component_not_enabled"}
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
        if args.tool_split:
            _check_tool_split(args, workspace, scenario_path, document, request, temporary)
            return 0
        if args.real_measured:
            # The maintained Real scenario mounts Mock's lane-free pneumatic
            # tool with no output (display only). Without that tool,
            # the robot alone is the arm-only component.
            scenario_request = request
            (attachment,) = document["deployment_draft"]["attachments"]
            robot_id = attachment["parent"]["component_id"]
            tool_id = attachment["child"]["component_id"]

            def scenario_row(value: dict[str, object], component_id: str) -> dict[str, object]:
                return next(row for row in value["deployment"]["components"]
                            if row["component_id"] == component_id)

            (scenario_gripper,) = scenario_row(scenario_request, tool_id)["configuration"][
                "runtime_configuration"
            ]["gripper_target_configurations"]
            arm_only = copy.deepcopy(document)
            arm_only["deployment_draft"]["components"] = [
                row for row in arm_only["deployment_draft"]["components"]
                if row["component_id"] != tool_id
            ]
            arm_only["deployment_draft"]["attachments"] = []
            request = _request(scenario_path, arm_only, scenario_request["program"])
            # A Real component compiles to measured authority with
            # its own authority ID, an arm-only type, its site-settings address
            # reference and no cell-wide `mode`.
            prepared = _run(args, request, temporary / "real")
            assert prepared is not None
            graph = prepared["activation_payload"]["retained_bundle"]["cell_graph"]
            bindings = graph["control_bindings"]
            assert bindings and {row["command_target_id"] for row in bindings} == {"arm"}
            for row in bindings:
                component = row["controllable_component_id"]
                assert row["state_authority_class"] == "measured"
                assert row["state_authority_id"] == f"{component}.arm.measured_state"
                # Every lane uses the shared provider; the vendor adapter comes
                # from the model's lane row.
                assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
            bundle = prepared["activation_payload"]["retained_bundle"]
            assert {
                row["provider_manifest_id"]
                for row in bundle["capability_provider_bindings"]
                if row["scope"] == "target_binding"
            } == {"manyforge.provider.ros2_control"}
            # The arm is its machine's member; the owner is a
            # target-less component of the graph.
            owner_id = scenario_row(scenario_request, robot_id)["owner"]
            assert sorted(row["controllable_component_id"]
                          for row in graph["controllable_components"]) == sorted(
                [owner_id, robot_id])
            (component,) = [row for row in graph["controllable_components"]
                            if row["controllable_component_id"] == robot_id]
            # The split arm model has no fingers; its type is arm-only.
            assert component["component_type_id"] == "lite6.component"
            (unit,) = graph["runtime_units"]
            configuration = unit["configuration"]
            assert "mode" not in configuration
            assert "gripper_target_configurations" not in configuration
            assert _without_descriptions(configuration["controller_profiles"]) == [{
                "controllable_component_id": component["controllable_component_id"],
                "device_address_ref": "lite6_main",
                "frame_prefix": "",
                "lane": "real",
                "lane_row": _lane_row(args.workspace, scenario_row(request, robot_id), "real"),
                "member_role": "arm",
                "model_family_id": "lite6",
                "owner_component_id": owner_id,
                **_joints(args.workspace, scenario_row(request, robot_id)),
                **_arm_stop_cancel(scenario_row(request, robot_id)),
            }]
            # A Real pneumatic gripper is explicit: the scenario's row is the
            # genuine Mock model's gripper row with no output, and declares no
            # nonexistent feedback.
            mock_deployment = load_strict_json_yaml(
                args.workspace / "examples/mock_lite6_pick_and_place.scenario/deployment.yaml"
            )
            (mock_component,) = [
                row for row in mock_deployment["components"] if row["component_id"] == tool_id
            ]
            (pneumatic,) = mock_component["configuration"]["runtime_configuration"][
                "gripper_target_configurations"
            ]
            assert scenario_gripper["output_mode"] == "none"
            assert {
                key: value for key, value in scenario_gripper.items()
                if key not in {"description", "output_mode"}
            } == {
                key: value for key, value in pneumatic.items()
                if key not in {"description", "output_mode"}
            }
            with_gripper = scenario_request
            accepted = _run(args, with_gripper, temporary / "real-pneumatic-none")
            assert accepted is not None
            new_graph = accepted["activation_payload"]["retained_bundle"]["cell_graph"]
            assert {
                row["controllable_component_id"]: row["component_type_id"]
                for row in new_graph["controllable_components"]
                if row["controllable_component_id"] != owner_id
            } == {robot_id: "lite6.component",
                  tool_id: "lite_gripper.component"}
            gripper_bindings = [
                row for row in new_graph["control_bindings"]
                if row["command_target_id"] == "gripper"
            ]
            assert gripper_bindings and all(
                row["state_authority_class"] == "commanded" for row in gripper_bindings
            )
            assert all(
                row["state_authority_class"] == "measured"
                for row in new_graph["control_bindings"]
                if row["command_target_id"] == "arm"
            )
            # The gripper domain is the lane-free tool's own (shared
            # ROS provider, its own instance).
            for row in [
                *new_graph["control_bindings"],
                *(
                    row
                    for row in accepted["activation_payload"]["retained_bundle"][
                        "capability_provider_bindings"
                    ]
                    if row["scope"] == "target_binding"
                ),
            ]:
                gripper = (
                    row.get("command_target_id", row.get("target", {}).get("command_target_id"))
                    == "gripper"
                )
                assert row["provider_manifest_id"] == "manyforge.provider.ros2_control"
                assert row["provider_instance_id"] == (
                    f"{tool_id}.ros2_control" if gripper else f"{robot_id}.ros2_control"
                )
            (new_unit,) = new_graph["runtime_units"]
            assert [
                (row["controllable_component_id"], row["stream_id"])
                for row in new_unit["configuration"]["component_streams"]
            ] == [(robot_id, "measured_joints")]
            # The tool declares no nonexistent feedback, and its pneumatic row
            # is refused as a lane-free tool's (the tool, not the
            # Real robot, owns it).
            false_feedback = copy.deepcopy(with_gripper)
            scenario_row(false_feedback, tool_id)["configuration"][
                "runtime_configuration"
            ]["component_streams"].insert(0, {
                "budget_ns": 1000000000,
                "controllable_component_id": tool_id,
                "stream_id": "gripper_feedback",
            })
            _rehash_request(false_feedback)
            _run(args, false_feedback, temporary / "real-pneumatic-false-feedback",
                 expect_error="is a lane-free tool: component_streams must declare no stream")
            commanded = copy.deepcopy(with_gripper)
            scenario_row(commanded, tool_id)["configuration"]["runtime_configuration"][
                "gripper_target_configurations"
            ][0]["output_mode"] = "commanded"
            _rehash_request(commanded)
            _run(args, commanded, temporary / "real-pneumatic-commanded",
                 expect_error="tool_pneumatic_io_required")
            wrong_kind = copy.deepcopy(with_gripper)
            scenario_row(wrong_kind, tool_id)["configuration"]["runtime_configuration"][
                "gripper_target_configurations"
            ][0]["actuation_kind"] = "position_commanded"
            _rehash_request(wrong_kind)
            _run(args, wrong_kind, temporary / "real-gripper-wrong-kind",
                 expect_error="tool_lane_required")
            # The robot-row tool payload is gone: a tool's payload comes from
            # its model, so an authored one is refused at compile.
            with_payload = copy.deepcopy(request)
            scenario_row(with_payload, robot_id)["configuration"][
                "runtime_configuration"
            ]["tool_payload"] = {"mass_kg": 0.3, "center_of_mass_m": [0.0, 0.01, 0.05]}
            _rehash_request(with_payload)
            _run(args, with_payload, temporary / "real-tool-payload",
                 expect_error="runtime_configuration.tool_payload moved: a tool's "
                 "payload comes from its model")
            # Lane real carries a site-settings address reference, on the
            # machine's owner; a lane without one refuses it.
            no_address = copy.deepcopy(request)
            del scenario_row(no_address, owner_id)["configuration"]["device_address_ref"]
            _rehash_request(no_address)
            _run(args, no_address, temporary / "real-address-missing",
                 expect_error="lane real requires a device_address_ref")
            # One robot is driven by one component: a second Real component
            # naming the same site device reference is refused at compile.
            # The twin is a second machine (owner and every member)
            # with the same address reference.
            twin_id = robot_id + "_twin"
            twin_owner_id = owner_id + "_twin"
            duplicate = copy.deepcopy(request)
            for key in ("deployment", "model_source_closure", "prepared_model_closure"):
                rows = duplicate[key]["components"]
                twin = copy.deepcopy(next(row for row in rows if row["component_id"] == robot_id))
                twin["component_id"] = twin_id
                if key == "deployment":
                    twin["frame_prefix"] = "twin/"
                    twin["owner"] = twin_owner_id
                    runtime = twin["configuration"]["runtime_configuration"]
                    for row in runtime.get("component_streams", []):
                        row["controllable_component_id"] = twin_id
                rows.append(twin)
                if key == "deployment":
                    twin_owner = copy.deepcopy(scenario_row(request, owner_id))
                    twin_owner["component_id"] = twin_owner_id
                    rows.append(twin_owner)
                    for member in [row for row in rows if row.get("owner") == owner_id
                                   and row["component_id"] != robot_id]:
                        twin_member = copy.deepcopy(member)
                        twin_member["component_id"] = member["component_id"] + "_twin"
                        twin_member["owner"] = twin_owner_id
                        rows.append(twin_member)
                rows.sort(key=lambda row: row["component_id"])
            _rehash_request(duplicate)
            _run(
                args,
                duplicate,
                temporary / "real-duplicate-address",
                expect_error="real_device_address_duplicate",
            )
            return 0
        if args.no_program:
            request["program"] = None
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
            _require_gripper_target_semantics(
                args, request, temporary / "gripper-target"
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
                expect_error="unsupported activation preparation request version",
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

            # The plain UR5e row (the Lite 6 is a machine, whose owner has no
            # captured source); the rename keeps the rows sorted.
            source_mismatch = copy.deepcopy(request)
            _row(source_mismatch["deployment"], "ur5e_robot")["component_id"] = (
                "ur5e_robot_different"
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
            # A full duplicate row: a renamed mounted row would fail on its pose
            # first. Two adjacent rows without a model source (a machine's
            # member rows) keep the captured source closure's count.
            index = next(
                index for index in range(len(components) - 1)
                if "model_source" not in components[index]
                and "model_source" not in components[index + 1]
            )
            components[index + 1] = copy.deepcopy(components[index])
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
            _row(unknown_profile["deployment"], "ur5e_robot")[
                "component_profile_id"
            ] = "unknown_profile"
            _rehash_request(unknown_profile)
            _run(
                args,
                unknown_profile,
                temporary / "unknown-component-profile",
                expect_error="selects unknown model unknown_profile",
            )

            mismatched_runtime_kind = copy.deepcopy(request)
            _row(mismatched_runtime_kind["deployment"], "ur5e_robot")[
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
            # A robot counts with the tool mounted on it.
            if len(_unmounted_ids(request["deployment"])) == 1:
                dual_path = "examples/mock_dual_arm_shared_group.scenario"
                _, _, dual_request = _scenario_request(workspace, dual_path)
                if len(_unmounted_ids(dual_request["deployment"])) != 2:
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
                # the unit's component configuration, so only its
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
        del missing["program"]
        _run(
            args,
            missing,
            temporary / "missing",
            expect_error="request is missing program",
        )

        legacy = copy.deepcopy(request)
        legacy["program_role_assignment"] = {"component_roles": [], "target_roles": []}
        _run(
            args,
            legacy,
            temporary / "legacy-assignment",
            expect_error="request has unexpected field program_role_assignment",
        )

        mispartitioned_initial_state = copy.deepcopy(request)
        # The two robots; their mounted tools have no arm.
        robot_ids = _unmounted_ids(mispartitioned_initial_state["deployment"])
        components = [
            row for row in mispartitioned_initial_state["deployment"]["components"]
            if row["component_id"] in robot_ids
        ]
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
        def replace_target(value: object) -> bool:
            if isinstance(value, dict):
                if set(value) == {"component_id", "command_target_id"}:
                    value["command_target_id"] = "foreign_target"
                    return True
                return any(replace_target(child) for child in value.values())
            if isinstance(value, list):
                return any(replace_target(child) for child in value)
            return False
        program = foreign["program"]
        if not isinstance(program, dict) or not replace_target(program["tree"]):
            raise AssertionError("negative matrix needs an authored direct target")
        program["program_revision"] = compute_content_identity(
            program, own_field="program_revision", identity_domain="program-revision",
            api_version="manyforge.program.v3")
        _rehash_request(foreign)
        _run(
            args,
            foreign,
            temporary / "foreign",
            expect_diagnostic="command_target_missing",
        )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

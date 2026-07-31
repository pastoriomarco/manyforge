#!/usr/bin/env python3
"""Validate planner-neutral contracts and truthful adapter availability."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import yaml


ROOT = Path(__file__).resolve().parents[2]
MAPPING = ROOT / "verification/planner_portability/rp5-6-backend-mapping.json"

REQUIRED_CAPABILITIES = {
    "planning.motion.joint_goal",
    "planning.motion.pose_goal",
    "planning.motion.cartesian_path",
    "planning.ik.single_shot",
    "planning.cancellation.cooperative",
    "planning.trajectory.time_parameterized",
    "scene.projection.revision_bound",
    "planning.failure.structured",
}
REQUIRED_PROFILES = {
    "planning.profile.first_feasible",
    "planning.profile.balanced",
    "planning.profile.quality_bounded",
}
REQUIRED_EVIDENCE = {
    "neutral_harness_reference_backend",
    "neutral_harness_detects_invalid_outputs",
    "neutral_harness_time_parameterized_output",
    "neutral_harness_detects_private_names",
    "neutral_harness_detects_private_ik_names",
    "neutral_harness_contains_noncooperative_cancel",
    "explicit_optional_capability_rejection",
    "relative_resolution_precedes_planner",
    "relative_execution_reaches_planner_as_resolved_pose",
    "exact_revision_cache_hit_revalidates",
    "older_revision_candidate_revalidates_current_snapshot",
    "failed_revalidation_forces_planning",
    "intent_change_forces_planning",
    "profile_participates_in_cache_identity",
    "start_incompatibility_forces_planning",
    "goal_incompatibility_forces_planning",
    "model_binding_participates_in_cache_identity",
    "contact_policy_participates_in_cache_identity",
    "cache_identity_mismatch_forces_planning",
    "planner_candidate_cannot_bypass_non_collision_validation",
    "planner_candidate_cannot_bypass_collision_authority",
    "authoritative_collision_replans_candidate",
    "cached_candidate_cannot_bypass_validation",
}

FORBIDDEN_BACKEND_OPTION_KEYS = {
    "fast_return",
    "goal_bias",
    "goal_biasing_probability",
    "max_nodes",
    "oin_k",
    "rrt_star",
}


def fail(failures: list[str], message: str) -> None:
    failures.append(message)


def require_mapping_fields(
    failures: list[str], value: object, fields: set[str], location: str
) -> None:
    if not isinstance(value, dict):
        fail(failures, f"{location}: expected object")
        return
    missing = fields - value.keys()
    if missing:
        fail(failures, f"{location}: missing fields {sorted(missing)}")


def iter_mapping_keys(value: object, path: str = "root"):
    if isinstance(value, dict):
        for key, child in value.items():
            yield path, str(key)
            yield from iter_mapping_keys(child, f"{path}.{key}")
    elif isinstance(value, list):
        for index, child in enumerate(value):
            yield from iter_mapping_keys(child, f"{path}[{index}]")


def main() -> int:
    failures: list[str] = []
    if not MAPPING.is_file():
        print(f"missing planner portability mapping: {MAPPING}", file=sys.stderr)
        return 1
    try:
        document = json.loads(MAPPING.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        print(f"invalid planner portability mapping: {exc}", file=sys.stderr)
        return 1

    if document.get("schema_version") != "manyforge.planner_portability.v1":
        fail(failures, "mapping has wrong schema_version")
    if set(document.get("portable_capabilities", [])) != REQUIRED_CAPABILITIES:
        fail(failures, "portable_capabilities does not match the required contract")
    if set(document.get("portable_profiles", [])) != REQUIRED_PROFILES:
        fail(failures, "portable_profiles does not match the required contract")

    availability = document.get("runtime_availability", {})
    if availability.get("status") != "temporarily_unavailable_composable_cell_rebuild":
        fail(failures, "planner runtime availability must record the composable-cell outage")
    if (
        availability.get("available_surface")
        != "retained_planning_product_consumer_qualification_only"
    ):
        fail(failures, "runtime availability overclaims the retained-consumer-only surface")

    authority = document.get("authority", {})
    expected_authority = {
        "logical_scene": "manyforge_scene",
        "non_collision_validation": "manyforge_core.ITrajectoryValidator",
        "collision_validation": "manyforge_core.ICollisionQueryEngine",
        "dispatch": "manyforge_core.ExecutionKernel",
        "runtime_collision": "manyforge_core.RuntimeCollisionMonitor",
        "stop_interrupt_recovery": "manyforge_core.ExecutionKernel",
    }
    for key, expected in expected_authority.items():
        if authority.get(key) != expected:
            fail(failures, f"authority.{key}: expected {expected!r}")

    adapters = document.get("adapters", {})
    require_mapping_fields(
        failures,
        adapters,
        {"deterministic_fake", "roboplan_0_5_1", "moveit2", "curobo"},
        "adapters",
    )
    fake = adapters.get("deterministic_fake", {})
    if fake.get("status") != "test_only_implemented":
        fail(failures, "deterministic_fake is not marked test_only_implemented")
    roboplan = adapters.get("roboplan_0_5_1", {})
    if roboplan.get("status") != "temporarily_unavailable_composable_cell_rebuild":
        fail(failures, "RoboPlan runtime is not explicitly marked unavailable")
    if (
        roboplan.get("available_surface")
        != "retained_planning_product_consumer_qualification_only"
    ):
        fail(failures, "RoboPlan overclaims more than retained-product consumer qualification")
    if set(roboplan.get("capability_disposition", {})) != REQUIRED_CAPABILITIES:
        fail(failures, "RoboPlan mapping does not cover every portable capability")
    elif set(roboplan["capability_disposition"].values()) != {"temporarily_unavailable"}:
        fail(failures, "RoboPlan capability mapping contains a runtime support claim")
    if set(roboplan.get("profile_disposition", {})) != REQUIRED_PROFILES:
        fail(failures, "RoboPlan mapping does not cover every portable profile")
    elif set(roboplan["profile_disposition"].values()) != {"temporarily_unavailable"}:
        fail(failures, "RoboPlan profile mapping contains a runtime support claim")

    moveit = adapters.get("moveit2", {})
    require_mapping_fields(
        failures,
        moveit,
        {
            "status",
            "proposed_package",
            "dependency_boundary",
            "composition",
            "retained_product_input",
            "ports",
            "capability_disposition",
            "profile_disposition",
            "adapter_local_gaps",
            "forbidden_shortcuts",
        },
        "adapters.moveit2",
    )
    if moveit.get("status") != "fully_mapped_not_implemented":
        fail(failures, "MoveIt2 status must remain fully_mapped_not_implemented")
    if set(moveit.get("capability_disposition", {})) != REQUIRED_CAPABILITIES:
        fail(failures, "MoveIt2 mapping does not cover every portable capability")
    if set(moveit.get("profile_disposition", {})) != REQUIRED_PROFILES:
        fail(failures, "MoveIt2 mapping does not cover every portable profile")
    required_ports = {
        "IPlannerBackend",
        "IRobotKinematics",
        "IReferencePoseResolver",
        "IExecutionStateSink",
    }
    if set(moveit.get("ports", {})) != required_ports:
        fail(failures, "MoveIt2 mapping does not cover every focused adapter port")
    if len(moveit.get("adapter_local_gaps", [])) < 5:
        fail(failures, "MoveIt2 adapter-local gaps are incomplete")

    serialized_moveit = json.dumps(moveit).lower()
    for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
        if backend_option in serialized_moveit:
            fail(failures, f"MoveIt2 mapping leaks backend option {backend_option}")

    serialized_document = json.dumps(document)
    for deleted_surface in ("IPlannerSceneProjection", "ISceneAuthority"):
        if deleted_surface in serialized_document:
            fail(failures, f"mapping references deleted surface {deleted_surface}")

    curobo = adapters.get("curobo", {})
    if curobo.get("status") != "explicitly_unsupported_deferred":
        fail(failures, "cuRobo must remain explicitly unsupported and deferred")
    if curobo.get("implementation_changes_in_rp5_6") != "none":
        fail(failures, "RP5-6 must not implement or modify a cuRobo adapter")

    evidence = document.get("test_references", [])
    found_requirements: set[str] = set()
    for entry in evidence:
        if not isinstance(entry, dict):
            fail(failures, "test_references contains a non-object entry")
            continue
        requirement = entry.get("requirement")
        reference = entry.get("reference", "")
        if not requirement or not reference or "::" not in reference:
            fail(failures, f"malformed test reference: {entry!r}")
            continue
        found_requirements.add(requirement)
        relative_path, identifier = reference.split("::", 1)
        source = ROOT / relative_path
        if not source.is_file():
            fail(failures, f"test reference file is missing: {relative_path}")
            continue
        if identifier not in source.read_text(encoding="utf-8", errors="replace"):
            fail(
                failures,
                f"test reference identifier is missing: {relative_path}::{identifier}",
            )
    missing_evidence = REQUIRED_EVIDENCE - found_requirements
    if missing_evidence:
        fail(failures, f"missing required evidence rows: {sorted(missing_evidence)}")

    harness_files = (
        ROOT
        / "manyforge_core/test_support/include/manyforge_core/testing/planner_backend_conformance.hpp",
        ROOT / "manyforge_core/test_support/src/planner_backend_conformance.cpp",
    )
    forbidden_harness_dependency = re.compile(
        r"(?:#\s*include\s*[<\"](?:manyforge_roboplan|moveit|curobo)|"
        r"\b(?:roboplan|moveit|curobo)::|#\s*include\s*[<\"]gtest)",
        re.IGNORECASE,
    )
    for harness in harness_files:
        if not harness.is_file():
            fail(failures, f"missing reusable harness file: {harness.relative_to(ROOT)}")
            continue
        if forbidden_harness_dependency.search(harness.read_text(encoding="utf-8")):
            fail(
                failures,
                f"{harness.relative_to(ROOT)} imports a concrete planner or GTest",
            )

    plan_request = (
        ROOT / "manyforge_core/include/manyforge_core/types/plan_request.hpp"
    ).read_text(encoding="utf-8")
    for field in ("target_mode", "target_reference", "offset_reference"):
        if re.search(rf"\b{field}\b", plan_request):
            fail(
                failures,
                f"PlanRequestDTO contains unresolved program field {field}",
            )
    portable_sources = "\n".join(
        path.read_text(encoding="utf-8")
        for path in (
            ROOT / "manyforge_core/include/manyforge_core/types/plan_request.hpp",
            ROOT / "manyforge_core/include/manyforge_core/types/motion_spec.hpp",
        )
    ).lower()
    for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
        if backend_option in portable_sources:
            fail(failures, f"neutral DTO surface leaks backend option {backend_option}")

    program_sources = (
        ROOT / "manyforge_behavior/manyforge_behavior/program.py",
        ROOT / "manyforge_behavior/manyforge_behavior/catalog_schema.py",
        ROOT / "manyforge_behavior/manyforge_behavior/nodes/motion_scoped.py",
        ROOT / "manyforge_composer/backend/models.py",
        ROOT / "manyforge_composer/backend/routes_program.py",
    )
    for source in program_sources:
        source_text = source.read_text(encoding="utf-8").lower()
        for backend_option in FORBIDDEN_BACKEND_OPTION_KEYS:
            if re.search(rf"[\"']{re.escape(backend_option)}[\"']", source_text):
                fail(
                    failures,
                    f"neutral program surface declares backend option {backend_option}: "
                    f"{source.relative_to(ROOT)}",
                )

    program_artifacts = sorted(
        path
        for root in (
            ROOT / "examples",
            ROOT / "verification/programs",
            ROOT / "manyforge_behavior/resources",
            ROOT / "manyforge_behavior/tests/fixtures",
            ROOT / "tests/fixtures",
        )
        if root.is_dir()
        for path in root.rglob("*")
        if path.is_file()
        and path.suffix in {".yaml", ".yml", ".json"}
    )
    for artifact in program_artifacts:
        try:
            if artifact.suffix == ".json":
                parsed = json.loads(artifact.read_text(encoding="utf-8"))
            else:
                parsed = yaml.safe_load(artifact.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError, yaml.YAMLError) as exc:
            fail(
                failures,
                f"cannot inspect program artifact {artifact.relative_to(ROOT)}: {exc}",
            )
            continue
        for object_path, key in iter_mapping_keys(parsed):
            if key.lower() in FORBIDDEN_BACKEND_OPTION_KEYS:
                fail(
                    failures,
                    "program artifact declares backend-native option "
                    f"{key}: {artifact.relative_to(ROOT)}:{object_path}",
                )

    if failures:
        print("Planner portability check failed:", file=sys.stderr)
        for failure in failures:
            print(f"  - {failure}", file=sys.stderr)
        return 1

    print(
        "Planner portability check passed "
        f"({len(evidence)} evidence rows; RoboPlan runtime explicitly unavailable)."
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

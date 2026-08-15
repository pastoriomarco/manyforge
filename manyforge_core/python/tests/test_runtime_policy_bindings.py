"""Parity tests for the retained runtime-policy Python boundary."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")

RUNTIME_POLICY_SCHEMA_REVISION = (
    "sha256:330a7505c585d69b231a1322bffbcd71775a1070c3a519a1ef7eb013ed5b5e9f"
)


def _policy_document() -> str:
    payload = {
        "api_version": "manyforge.runtime_policy.v1",
        "runtime_policy_id": "runtime.python",
        "description": "Python binding policy",
        "interrupt_policy_ids": ["fault.stop", "probe.trip"],
        "motion_speed_limits": [
            {
                "scope_kind": "cell",
                "maximum_tcp_linear_speed_mps": 0.5,
            },
            {
                "scope_kind": "command_target",
                "controllable_component_id": "robot_a",
                "command_target_id": "arm",
                "maximum_tcp_linear_speed_mps": 0.25,
            },
        ],
        "open_loop_grasp_acknowledgements": [
            {
                "controllable_component_id": "robot_a",
                "command_target_id": "gripper",
                "environment": "simulation",
                "allowed": True,
            }
        ],
    }
    canonical_payload = json.dumps(payload, sort_keys=True, separators=(",", ":"))
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/runtime_policy"
        b"\x00manyforge.runtime_policy.v1\x00"
        + canonical_payload.encode()
    )
    payload["runtime_policy_revision"] = (
        f"sha256:{hashlib.sha256(preimage).hexdigest()}"
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _policy_reference(document: str):
    decoded = json.loads(document)
    return core.RuntimePolicyCatalogRecordRefDTO(
        record_id=core.RuntimePolicyId(decoded["runtime_policy_id"]),
        record_revision=core.RuntimePolicyRevision(
            decoded["runtime_policy_revision"]
        ),
        schema_id=core.SchemaId("manyforge.runtime_policy.v1"),
        schema_revision=core.SchemaRevision(RUNTIME_POLICY_SCHEMA_REVISION),
        content_digest=core.ContentDigest(
            f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
        ),
        content_addressed_store_key=core.ContentAddressedStoreKey(
            "sha256/runtime-policy-python"
        ),
    )


def test_runtime_policy_resolves_to_exact_immutable_types():
    document = _policy_document()
    reference = _policy_reference(document)
    qualified = core.resolve_and_qualify_runtime_policy(
        reference, reference.content_addressed_store_key, document
    )

    assert isinstance(qualified, core.QualifiedRuntimePolicy)
    assert qualified.normalized_canonical_json == document
    assert qualified.value.runtime_policy_id == core.RuntimePolicyId(
        "runtime.python"
    )
    assert qualified.value.interrupt_policy_ids == [
        core.InterruptPolicyId("fault.stop"),
        core.InterruptPolicyId("probe.trip"),
    ]
    assert isinstance(
        qualified.value.motion_speed_limits[0].scope,
        core.RuntimePolicyCellScopeDTO,
    )
    target_scope = qualified.value.motion_speed_limits[1].scope
    assert isinstance(target_scope, core.RuntimePolicyCommandTargetScopeDTO)
    assert target_scope.command_target_id == core.CommandTargetId("arm")
    acknowledgement = qualified.value.open_loop_grasp_acknowledgements[0]
    assert (
        acknowledgement.environment
        == core.OpenLoopGraspEnvironmentDTO.kSimulation
    )
    assert acknowledgement.allowed is True

    with pytest.raises(TypeError):
        core.QualifiedRuntimePolicy()
    with pytest.raises(AttributeError):
        qualified.value = qualified.value


def test_runtime_policy_record_survives_closed_variant_ingestion():
    document = _policy_document()
    reference = _policy_reference(document)
    inputs = core.DeploymentApplicabilityInputsDTO(
        site_overlay_ref=None,
        desired_control_configurations=(),
        capability_provider_bindings=(),
        external_resources=(),
        stop_stage_parameters=(),
        commissioned_stop_interfaces=(),
        retained_policy_records=(reference,),
    )

    assert inputs.retained_policy_records == [reference]

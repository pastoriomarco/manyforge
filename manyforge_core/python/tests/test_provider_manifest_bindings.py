"""Parity tests for the immutable C0.2g provider-manifest boundary."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")


def _provider_manifest() -> object:
    capability_offer = core.ProviderCapabilityOfferDTO(
        capability_id=core.CapabilityId("execution.trajectory.execute"),
        capability_definition_revision=core.CapabilityDefinitionRevision(
            "sha256:" + "1" * 64
        ),
        required_endpoint_role_ids=[
            core.EndpointRoleId("trajectory_command"),
            core.EndpointRoleId("trajectory_state"),
        ],
    )
    contract_ref = core.ProviderContractRefDTO(
        provider_contract_role_id=core.ProviderContractRoleId(
            "controlled_stop"
        ),
        contract_schema_id=core.SchemaId(
            "manyforge.value_schema.provider_contract.empty.v1"
        ),
        contract_schema_revision=core.SchemaRevision(
            "sha256:" + "2" * 64
        ),
    )
    command_support = core.ProviderStopCommandSupportDTO(
        command_kind=core.StopContainmentCommandKind.kMotionControlledStop,
        command_endpoint_role_ids=[
            core.EndpointRoleId("controlled_stop_command")
        ],
    )
    evidence_support = core.ProviderStopEvidenceSupportDTO(
        evidence_kind=core.StopEvidenceRequirementKind.kMotionRest,
        evidence_endpoint_role_ids=[
            core.EndpointRoleId("measured_motion_state")
        ],
    )
    contract = core.ProviderContractDeclarationDTO(
        provider_contract_ref=contract_ref,
        stop_command_support=command_support,
        stop_evidence_support=evidence_support,
    )
    return core.ProviderManifestDTO(
        provider_manifest_id=core.ProviderManifestId(
            "ros2_control_trajectory_provider"
        ),
        provider_manifest_revision=core.ProviderManifestRevision(
            "sha256:" + "3" * 64
        ),
        offered_capabilities=[capability_offer],
        provider_contracts=[contract],
    )


def test_provider_manifest_dto_is_typed_and_read_only():
    manifest = _provider_manifest()

    assert manifest.provider_manifest_id == core.ProviderManifestId(
        "ros2_control_trajectory_provider"
    )
    assert len(manifest.offered_capabilities) == 1
    assert list(
        manifest.offered_capabilities[0].required_endpoint_role_ids
    ) == [
        core.EndpointRoleId("trajectory_command"),
        core.EndpointRoleId("trajectory_state"),
    ]
    assert len(manifest.provider_contracts) == 1
    assert (
        manifest.provider_contracts[0].stop_command_support.command_kind
        == core.StopContainmentCommandKind.kMotionControlledStop
    )
    assert (
        manifest.provider_contracts[0].stop_evidence_support.evidence_kind
        == core.StopEvidenceRequirementKind.kMotionRest
    )
    with pytest.raises(AttributeError):
        manifest.provider_contracts = []


def test_registered_provider_stop_supports_are_typed():
    command = core.ProviderStopCommandSupportDTO(
        command_kind=core.StopContainmentCommandKind.kRegistered,
        command_endpoint_role_ids=[],
        registered_command_kind_id=core.RegisteredStopCommandKindId(
            "vendor.stop"
        ),
    )
    evidence = core.ProviderStopEvidenceSupportDTO(
        evidence_kind=core.StopEvidenceRequirementKind.kRegistered,
        evidence_endpoint_role_ids=[],
        registered_evidence_kind_id=core.RegisteredStopEvidenceKindId(
            "vendor.rest"
        ),
    )

    assert command.registered_command_kind_id == (
        core.RegisteredStopCommandKindId("vendor.stop")
    )
    assert evidence.registered_evidence_kind_id == (
        core.RegisteredStopEvidenceKindId("vendor.rest")
    )


def test_provider_manifest_constants_match_the_v1_contract():
    assert (
        core.PROVIDER_MANIFEST_API_VERSION
        == "manyforge.provider_manifest.v1"
    )
    assert core.MAX_PROVIDER_MANIFEST_CAPABILITIES == 256
    assert core.MAX_PROVIDER_MANIFEST_CONTRACTS == 256
    assert core.MAX_PROVIDER_MANIFEST_ENDPOINT_ROLES == 256


def test_provider_manifest_is_exactly_qualified():
    capability = next(
        definition
        for definition in core.BUILTIN_CAPABILITY_DEFINITION_REGISTRY
        if definition.capability_id == "execution.trajectory.execute"
    )
    payload = {
        "api_version": "manyforge.provider_manifest.v1",
        "provider_manifest_id": "ros2_control_trajectory_provider",
        "offered_capabilities": [
            {
                "capability_id": capability.capability_id,
                "capability_definition_revision": (
                    capability.capability_definition_revision
                ),
                "required_endpoint_role_ids": [],
            }
        ],
        "provider_contracts": [],
    }
    semantic_payload = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    )
    identity_preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/provider_manifest"
        b"\x00manyforge.provider_manifest.v1\x00"
        + semantic_payload.encode()
    )
    payload["provider_manifest_revision"] = (
        f"sha256:{hashlib.sha256(identity_preimage).hexdigest()}"
    )
    document = json.dumps(payload, sort_keys=True, separators=(",", ":"))

    schema_entry = next(
        entry
        for entry in (
            core.build_qualified_closed_schema_registry().catalog_schemas
        )
        if str(entry.schema_id) == "manyforge.provider_manifest.v1"
        and entry.compiled_schema is not None
    )
    store_key = core.ContentAddressedStoreKey(
        "sha256/provider-manifest-python"
    )
    reference = core.ProviderManifestCatalogRecordRefDTO(
        record_id=core.ProviderManifestId(
            "ros2_control_trajectory_provider"
        ),
        record_revision=core.ProviderManifestRevision(
            payload["provider_manifest_revision"]
        ),
        schema_id=core.SchemaId("manyforge.provider_manifest.v1"),
        schema_revision=schema_entry.compiled_schema.schema_revision,
        content_digest=core.ContentDigest(
            f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
        ),
        content_addressed_store_key=store_key,
    )

    qualified = core.resolve_and_qualify_provider_manifest(
        reference, store_key, document
    )

    assert isinstance(qualified, core.QualifiedProviderManifest)
    assert qualified.value == core.ProviderManifestDTO(
        provider_manifest_id=core.ProviderManifestId(
            "ros2_control_trajectory_provider"
        ),
        provider_manifest_revision=core.ProviderManifestRevision(
            payload["provider_manifest_revision"]
        ),
        offered_capabilities=[
            core.ProviderCapabilityOfferDTO(
                capability_id=core.CapabilityId(
                    "execution.trajectory.execute"
                ),
                capability_definition_revision=(
                    core.CapabilityDefinitionRevision(
                        capability.capability_definition_revision
                    )
                ),
                required_endpoint_role_ids=[],
            )
        ],
        provider_contracts=[],
    )
    assert json.loads(qualified.normalized_canonical_json) == payload
    with pytest.raises(TypeError):
        core.QualifiedProviderManifest()

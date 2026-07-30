"""Parity tests for the immutable C0.2f runtime-profile boundary."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")

RUNTIME_PROFILE_SCHEMA_REVISION = (
    "sha256:c254afa8c2bfcc803f12a7af63b1f09a8ee804b1ffa2a65b7361ad9309b87ae7"
)
EMPTY_CONFIGURATION_SCHEMA_REVISION = (
    "sha256:03457d535d31818fedaa7e44f48432a08feb705e4d22c9a8f5e50977c8dbf785"
)


def _profile_document(
    *,
    lifecycle_control: str = "manager_managed",
    status_endpoint_role_id: str = "runtime_status",
    source_endpoint_role_ids: list[str] | None = None,
    configuration_schema_id: str = (
        "manyforge.value_schema.runtime.empty.v1"
    ),
) -> str:
    payload = {
        "api_version": "manyforge.runtime_profile.v1",
        "runtime_profile_id": "ros2_control_xarm",
        "configuration_schema_ref": {
            "schema_id": configuration_schema_id,
            "schema_revision": EMPTY_CONFIGURATION_SCHEMA_REVISION,
        },
        "process_contract": {
            "lifecycle_control": lifecycle_control,
        },
        "supervision_contract": {
            "status_endpoint_role_id": status_endpoint_role_id,
        },
        "offered_runtime_role_ids": ["driver", "state_source"],
        "source_endpoint_role_ids": sorted(
            source_endpoint_role_ids
            if source_endpoint_role_ids is not None
            else ["joint_state", "runtime_status"]
        ),
    }
    canonical_payload = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    )
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/runtime_profile"
        b"\x00manyforge.runtime_profile.v1\x00"
        + canonical_payload.encode()
    )
    payload["runtime_profile_revision"] = (
        f"sha256:{hashlib.sha256(preimage).hexdigest()}"
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _profile_reference(document: str):
    return core.RuntimeProfileCatalogRecordRefDTO(
        record_id=core.RuntimeProfileId("ros2_control_xarm"),
        record_revision=core.RuntimeProfileRevision(
            json.loads(document)["runtime_profile_revision"]
        ),
        schema_id=core.SchemaId("manyforge.runtime_profile.v1"),
        schema_revision=core.SchemaRevision(
            RUNTIME_PROFILE_SCHEMA_REVISION
        ),
        content_digest=core.ContentDigest(
            f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
        ),
        content_addressed_store_key=core.ContentAddressedStoreKey(
            "sha256/runtime-profile-python"
        ),
    )


def test_runtime_profile_dto_is_typed_and_read_only():
    configuration_schema_ref = core.ValueSchemaRefDTO(
        schema_id=core.SchemaId(
            "manyforge.value_schema.runtime.empty.v1"
        ),
        schema_revision=core.SchemaRevision(
            EMPTY_CONFIGURATION_SCHEMA_REVISION
        ),
    )
    process_contract = core.RuntimeProcessContractDTO(
        lifecycle_control=core.RuntimeLifecycleControl.kManagerManaged
    )
    supervision_contract = core.RuntimeSupervisionContractDTO(
        status_endpoint_role_id=core.EndpointRoleId("runtime_status")
    )
    profile = core.RuntimeProfileDTO(
        runtime_profile_id=core.RuntimeProfileId("ros2_control_xarm"),
        runtime_profile_revision=core.RuntimeProfileRevision(
            "sha256:" + "1" * 64
        ),
        configuration_schema_ref=configuration_schema_ref,
        process_contract=process_contract,
        supervision_contract=supervision_contract,
        offered_runtime_role_ids=[
            core.RuntimeRoleId("driver"),
            core.RuntimeRoleId("state_source"),
        ],
        source_endpoint_role_ids=[
            core.EndpointRoleId("joint_state"),
            core.EndpointRoleId("runtime_status"),
        ],
    )

    assert profile.configuration_schema_ref == configuration_schema_ref
    assert (
        profile.process_contract.lifecycle_control
        == core.RuntimeLifecycleControl.kManagerManaged
    )
    assert (
        profile.supervision_contract.status_endpoint_role_id
        == core.EndpointRoleId("runtime_status")
    )
    assert list(profile.offered_runtime_role_ids) == [
        core.RuntimeRoleId("driver"),
        core.RuntimeRoleId("state_source"),
    ]
    with pytest.raises(AttributeError):
        profile.process_contract = process_contract


def test_runtime_profile_constants_match_the_v1_contract():
    assert core.RUNTIME_PROFILE_API_VERSION == "manyforge.runtime_profile.v1"
    assert core.MAX_RUNTIME_PROFILE_ROLE_IDS == 256


@pytest.mark.parametrize(
    ("token", "expected"),
    [
        ("manager_managed", core.RuntimeLifecycleControl.kManagerManaged),
        (
            "externally_managed",
            core.RuntimeLifecycleControl.kExternallyManaged,
        ),
    ],
)
def test_runtime_profile_is_exactly_qualified(token, expected):
    document = _profile_document(lifecycle_control=token)
    reference = _profile_reference(document)
    store_key = core.ContentAddressedStoreKey(
        "sha256/runtime-profile-python"
    )

    qualified = core.resolve_and_qualify_runtime_profile(
        reference, store_key, document
    )

    assert isinstance(qualified, core.QualifiedRuntimeProfile)
    assert qualified.value.runtime_profile_id == core.RuntimeProfileId(
        "ros2_control_xarm"
    )
    assert qualified.value.process_contract.lifecycle_control == expected
    assert list(qualified.value.offered_runtime_role_ids) == [
        core.RuntimeRoleId("driver"),
        core.RuntimeRoleId("state_source"),
    ]
    assert json.loads(qualified.normalized_canonical_json) == json.loads(
        document
    )
    with pytest.raises(TypeError):
        core.QualifiedRuntimeProfile()


def test_empty_runtime_configuration_schema_is_build_qualified_and_closed():
    registry = core.build_qualified_closed_schema_registry()
    matches = [
        entry
        for entry in registry.value_schemas
        if str(entry.schema_id)
        == "manyforge.value_schema.runtime.empty.v1"
    ]
    assert len(matches) == 1
    assert (
        str(matches[0].schema_revision)
        == EMPTY_CONFIGURATION_SCHEMA_REVISION
    )
    assert matches[0].compiled_schema.schema_purpose == (
        core.ClosedSchemaPurpose.kValue
    )
    assert matches[0].compiled_schema.root_shape == (
        core.ClosedSchemaRootShape.kObject
    )


def test_runtime_profile_rejects_unqualified_selector_and_status_mismatch():
    store_key = core.ContentAddressedStoreKey(
        "sha256/runtime-profile-python"
    )
    unknown = _profile_document(
        configuration_schema_id=(
            "manyforge.value_schema.runtime.unknown.v1"
        )
    )
    with pytest.raises(
        core.RuntimeProfileCodecError,
        match="unqualified configuration value schema",
    ):
        core.resolve_and_qualify_runtime_profile(
            _profile_reference(unknown), store_key, unknown
        )

    missing_status = _profile_document(
        source_endpoint_role_ids=["joint_state"]
    )
    with pytest.raises(
        core.RuntimeProfileCodecError,
        match="must occur",
    ):
        core.resolve_and_qualify_runtime_profile(
            _profile_reference(missing_status),
            store_key,
            missing_status,
        )


def test_runtime_profile_rejects_deleted_operational_surface():
    document = json.loads(_profile_document())
    document["restart_policy"] = {"mode": "always"}
    document.pop("runtime_profile_revision")
    canonical_payload = json.dumps(
        document, sort_keys=True, separators=(",", ":")
    )
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/runtime_profile"
        b"\x00manyforge.runtime_profile.v1\x00"
        + canonical_payload.encode()
    )
    document["runtime_profile_revision"] = (
        f"sha256:{hashlib.sha256(preimage).hexdigest()}"
    )
    mutated = json.dumps(document, sort_keys=True, separators=(",", ":"))
    reference = _profile_reference(mutated)
    with pytest.raises(core.ClosedSchemaError):
        core.resolve_and_qualify_runtime_profile(
            reference,
            core.ContentAddressedStoreKey(
                "sha256/runtime-profile-python"
            ),
            mutated,
        )

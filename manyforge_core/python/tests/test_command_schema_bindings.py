"""Qualified command-schema Python projection tests."""

from __future__ import annotations

import hashlib
from pathlib import Path

import pytest

core = pytest.importorskip("manyforge_core_py")

REPOSITORY_ROOT = Path(__file__).resolve().parents[3]
COMMAND_SCHEMA_PATH = (
    REPOSITORY_ROOT
    / "manyforge_deployment"
    / "resources"
    / "catalog"
    / "gripper.command_position.v1.yaml"
)
COMMAND_SCHEMA_CLOSED_SCHEMA_REVISION = (
    "sha256:4f23fe66c0dd587d998a52a7b166bddd22d7ab8e5b28caacc48373ff373dcd84"
)


def _qualified_command_schema():
    stored_bytes = COMMAND_SCHEMA_PATH.read_text(encoding="utf-8")
    store_key = core.ContentAddressedStoreKey(
        "sha256/gripper-command-position-python"
    )
    reference = core.CommandSchemaCatalogRecordRefDTO(
        record_id=core.CommandSchemaId(core.GRIPPER_POSITION_COMMAND_SCHEMA_ID),
        record_revision=core.CommandSchemaRevision(
            core.GRIPPER_POSITION_COMMAND_SCHEMA_REVISION
        ),
        schema_id=core.SchemaId(core.COMMAND_SCHEMA_API_VERSION),
        schema_revision=core.SchemaRevision(
            COMMAND_SCHEMA_CLOSED_SCHEMA_REVISION
        ),
        content_digest=core.ContentDigest(
            "sha256:" + hashlib.sha256(stored_bytes.encode()).hexdigest()
        ),
        content_addressed_store_key=store_key,
    )
    return (
        core.resolve_and_qualify_command_schema(
            reference, store_key, stored_bytes
        ),
        reference,
        stored_bytes,
    )


def test_qualified_command_schema_is_exact_read_only_projection():
    qualified, _, _ = _qualified_command_schema()
    value = qualified.value

    assert value.command_schema_id == core.CommandSchemaId(
        "gripper.command_position"
    )
    assert value.command_schema_revision == core.CommandSchemaRevision(
        core.GRIPPER_POSITION_COMMAND_SCHEMA_REVISION
    )
    assert value.compatible_capabilities[0].capability_id == core.CapabilityId(
        "tool.gripper.command"
    )
    assert isinstance(
        value.command_value, core.GripperPositionCommandValueContractDTO
    )
    assert value.command_value.accepted_position_kinds == [
        core.CoordinateKind.kAngular,
        core.CoordinateKind.kLinear,
    ]
    assert value.command_value.minimum_resource_count == 1
    assert value.command_value.maximum_resource_count == 4096
    assert value.caller_options[0].option_id == core.CommandOptionId(
        "command_duration_s"
    )
    assert '"command_schema_id":"gripper.command_position"' in (
        qualified.normalized_canonical_json
    )

    with pytest.raises(TypeError):
        core.QualifiedCommandSchema()
    with pytest.raises(AttributeError):
        value.command_schema_id = core.CommandSchemaId("other")


def test_command_schema_resolution_rejects_wrong_retrieval_key():
    _, reference, stored_bytes = _qualified_command_schema()
    with pytest.raises(core.CommandSchemaCodecError, match="different store key"):
        core.resolve_and_qualify_command_schema(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            stored_bytes,
        )

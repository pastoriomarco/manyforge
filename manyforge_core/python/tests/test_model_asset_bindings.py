"""Parity tests for the immutable C0.2e model-asset DTO boundary."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")

MODEL_ASSET_SCHEMA_REVISION = (
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c"
)


def _artifact(uri: str, media_type: str, size: int):
    return core.ModelSourceArtifactDTO(
        logical_uri=uri,
        media_type=media_type,
        byte_size=size,
        content_digest=core.ContentDigest("sha256:" + "0" * 64),
    )


def _model_asset_document():
    payload = {
        "api_version": "manyforge.model_asset.v1",
        "model_asset_id": "lite6_aggregate",
        "instance_cardinality": "multiple_per_cell",
        "expanded_source": {
            "urdf_logical_uri": (
                "asset://lite6_aggregate/urdf/model.urdf"
            ),
            "srdf_logical_uri": (
                "asset://lite6_aggregate/srdf/model.srdf"
            ),
            "artifacts": [
                {
                    "logical_uri": (
                        "asset://lite6_aggregate/srdf/model.srdf"
                    ),
                    "media_type": "application/xml",
                    "byte_size": 64,
                    "content_digest": "sha256:" + "1" * 64,
                },
                {
                    "logical_uri": (
                        "asset://lite6_aggregate/urdf/model.urdf"
                    ),
                    "media_type": "application/xml",
                    "byte_size": 128,
                    "content_digest": "sha256:" + "0" * 64,
                },
            ],
        },
        "root_frame_role_id": "base",
        "frame_role_bindings": [
            {
                "frame_role_id": "base",
                "source_link_name": "base_link",
            },
            {
                "frame_role_id": "tcp",
                "source_link_name": "tool0",
            },
        ],
        "model_group_role_bindings": [
            {
                "model_group_role_id": "arm",
                "source_group_name": "manipulator",
            }
        ],
        "resource_role_bindings": [
            {
                "resource_role_id": "arm_joints",
                "source_joint_names": ["joint1", "joint2"],
            }
        ],
        "collision_body_role_bindings": [
            {
                "collision_body_role_id": "arm_collision",
                "source_link_names": ["base_link", "tool0"],
            }
        ],
    }
    canonical_payload = json.dumps(
        payload, sort_keys=True, separators=(",", ":")
    )
    preimage = (
        b"manyforge.content-identity.v1"
        b"\x00catalog-record-revision/model_asset"
        b"\x00manyforge.model_asset.v1\x00"
        + canonical_payload.encode()
    )
    payload["model_revision"] = (
        f"sha256:{hashlib.sha256(preimage).hexdigest()}"
    )
    return json.dumps(payload, sort_keys=True, separators=(",", ":"))


def _model_asset_reference(document: str, *, asset_id="lite6_aggregate"):
    return core.ModelAssetCatalogRecordRefDTO(
        record_id=core.ModelAssetId(asset_id),
        record_revision=core.ModelRevision(
            json.loads(document)["model_revision"]
        ),
        schema_id=core.SchemaId("manyforge.model_asset.v1"),
        schema_revision=core.SchemaRevision(MODEL_ASSET_SCHEMA_REVISION),
        content_digest=core.ContentDigest(
            f"sha256:{hashlib.sha256(document.encode()).hexdigest()}"
        ),
        content_addressed_store_key=core.ContentAddressedStoreKey(
            "sha256/model-asset-python"
        ),
    )


def test_model_asset_dto_round_trip_is_typed_and_read_only():
    artifacts = [
        _artifact(
            "asset://lite6_aggregate/urdf/model.urdf",
            "application/xml",
            128,
        ),
        _artifact(
            "asset://lite6_aggregate/srdf/model.srdf",
            "application/xml",
            64,
        ),
    ]
    expanded = core.ExpandedModelSourceDTO(
        urdf_logical_uri="asset://lite6_aggregate/urdf/model.urdf",
        artifacts=artifacts,
        srdf_logical_uri="asset://lite6_aggregate/srdf/model.srdf",
    )
    frame_bindings = [
        core.ModelFrameRoleBindingDTO(
            frame_role_id=core.FrameRoleId("base"),
            source_link_name="base_link",
        ),
        core.ModelFrameRoleBindingDTO(
            frame_role_id=core.FrameRoleId("tcp"),
            source_link_name="tool0",
        ),
    ]
    group_bindings = [
        core.ModelGroupRoleBindingDTO(
            model_group_role_id=core.ModelGroupRoleId("arm"),
            source_group_name="manipulator",
        )
    ]
    resource_bindings = [
        core.ModelResourceRoleBindingDTO(
            resource_role_id=core.ResourceRoleId("arm_joints"),
            source_joint_names=["joint1", "joint2"],
        )
    ]
    collision_bindings = [
        core.ModelCollisionBodyRoleBindingDTO(
            collision_body_role_id=core.CollisionBodyRoleId("arm_collision"),
            source_link_names=["base_link", "tool0"],
        )
    ]
    asset = core.ModelAssetDTO(
        model_asset_id=core.ModelAssetId("lite6_aggregate"),
        model_revision=core.ModelRevision("sha256:" + "1" * 64),
        instance_cardinality=core.ModelAssetInstanceCardinality.kMultiplePerCell,
        expanded_source=expanded,
        root_frame_role_id=core.FrameRoleId("base"),
        frame_role_bindings=frame_bindings,
        model_group_role_bindings=group_bindings,
        resource_role_bindings=resource_bindings,
        collision_body_role_bindings=collision_bindings,
    )

    assert asset.model_asset_id == core.ModelAssetId("lite6_aggregate")
    assert (
        asset.instance_cardinality
        == core.ModelAssetInstanceCardinality.kMultiplePerCell
    )
    assert asset.expanded_source == expanded
    assert list(asset.expanded_source.artifacts) == artifacts
    assert list(asset.frame_role_bindings) == frame_bindings
    assert list(asset.model_group_role_bindings) == group_bindings
    assert list(asset.resource_role_bindings) == resource_bindings
    assert list(asset.collision_body_role_bindings) == collision_bindings
    assert list(asset.resource_role_bindings[0].source_joint_names) == [
        "joint1",
        "joint2",
    ]

    with pytest.raises(AttributeError):
        asset.root_frame_role_id = core.FrameRoleId("other")


def test_expanded_source_requires_the_artifact_closure_argument():
    with pytest.raises(TypeError):
        core.ExpandedModelSourceDTO(
            urdf_logical_uri="asset://fixed_tool/urdf/model.urdf"
        )

    source = core.ExpandedModelSourceDTO(
        urdf_logical_uri="asset://fixed_tool/urdf/model.urdf",
        artifacts=[
            _artifact(
                "asset://fixed_tool/urdf/model.urdf",
                "application/xml",
                1,
            )
        ],
    )
    assert source.srdf_logical_uri is None


def test_model_asset_constants_match_the_v1_contract():
    assert core.MODEL_ASSET_API_VERSION == "manyforge.model_asset.v1"
    assert core.MAX_MODEL_ASSET_COLLECTION_ITEMS == 4096
    assert core.MAX_MODEL_ASSET_LOGICAL_URI_BYTES == 2048
    assert core.MAX_MODEL_ASSET_MEDIA_TYPE_BYTES == 255
    assert core.MAX_MODEL_ASSET_SOURCE_SYMBOL_CODE_POINTS == 1024
    assert core.MAX_MODEL_ASSET_BYTE_SIZE == 9_007_199_254_740_991


def test_model_asset_is_exactly_qualified_and_result_is_read_only():
    document = _model_asset_document()
    reference = _model_asset_reference(document)
    store_key = core.ContentAddressedStoreKey("sha256/model-asset-python")

    qualified = core.resolve_and_qualify_model_asset(
        reference, store_key, document
    )

    assert isinstance(qualified, core.QualifiedModelAsset)
    assert qualified.value.model_asset_id == core.ModelAssetId(
        "lite6_aggregate"
    )
    assert qualified.value.model_revision == reference.record_revision
    assert [
        artifact.logical_uri
        for artifact in qualified.value.expanded_source.artifacts
    ] == [
        "asset://lite6_aggregate/srdf/model.srdf",
        "asset://lite6_aggregate/urdf/model.urdf",
    ]
    assert json.loads(qualified.normalized_canonical_json) == json.loads(
        document
    )

    with pytest.raises(TypeError):
        core.QualifiedModelAsset()
    with pytest.raises(AttributeError):
        qualified.value = qualified.value


def test_model_asset_qualification_rejects_exact_referent_mismatches():
    document = _model_asset_document()
    reference = _model_asset_reference(document)
    store_key = core.ContentAddressedStoreKey("sha256/model-asset-python")

    with pytest.raises(core.ModelAssetCodecError, match="different store key"):
        core.resolve_and_qualify_model_asset(
            reference,
            core.ContentAddressedStoreKey("sha256/other"),
            document,
        )

    wrong_id = _model_asset_reference(document, asset_id="other_asset")
    with pytest.raises(core.ModelAssetCodecError):
        core.resolve_and_qualify_model_asset(
            wrong_id, store_key, document
        )

    with pytest.raises(core.ModelAssetCodecError):
        core.resolve_and_qualify_model_asset(
            reference, store_key, document + " "
        )

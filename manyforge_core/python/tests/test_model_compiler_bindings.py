"""Python parity for the immutable I2 model/compiler trust stages."""

from __future__ import annotations

import hashlib
import json

import pytest

core = pytest.importorskip("manyforge_core_py")

MODEL_ASSET_SCHEMA_REVISION = (
    "sha256:0820a2fbe627f2ccc5b76ff8d5348137d796893d3bd67707f7171fb420ec671c"
)
COMPONENT_TYPE_SCHEMA_REVISION = (
    "sha256:fdd03c397fb792d387002df453055d9ccd7d1826ae41d3b0706e5f7c27480d87"
)
ZERO_DIGEST = "sha256:" + ("0" * 64)

assert core.MAX_MODEL_SOURCE_XML_DEPTH == 128
assert core.MAX_MODEL_SOURCE_XML_ELEMENT_COUNT == 250_000
assert core.MAX_MODEL_SOURCE_XML_ATTRIBUTE_COUNT == 1_000_000


def _sha256(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def _store_key(digest: str):
    return core.ContentAddressedStoreKey("sha256/" + digest.removeprefix("sha256:"))


def _canonical(value) -> str:
    return json.dumps(value, sort_keys=True, separators=(",", ":"))


def _revisioned_document(
    identity: dict,
    *,
    identity_domain: str,
    api_version: str,
    revision_field: str,
) -> str:
    preimage = (
        b"manyforge.content-identity.v1"
        + b"\x00"
        + identity_domain.encode()
        + b"\x00"
        + api_version.encode()
        + b"\x00"
        + _canonical(identity).encode()
    )
    root = dict(identity)
    root[revision_field] = _sha256(preimage)
    return _canonical(root)


def _source_fixture(*, urdf_declared_size: int | None = None):
    urdf_uri = "asset://python_arm/urdf/model.urdf"
    srdf_uri = "asset://python_arm/srdf/model.srdf"
    mesh_uri = "asset://python_arm/meshes/base.stl"
    urdf = (
        '<robot name="python_arm">'
        '<link name="base_link"><visual><geometry>'
        f'<mesh filename="{mesh_uri}"/>'
        "</geometry></visual></link>"
        '<link name="tool0"/>'
        '<joint name="joint1" type="revolute">'
        '<parent link="base_link"/><child link="tool0"/>'
        '<axis xyz="0 0 1"/>'
        '<limit lower="-3" upper="3" effort="1" velocity="1"/>'
        "</joint></robot>"
    ).encode()
    srdf = (
        '<robot name="python_arm">'
        '<group name="manipulator">'
        '<chain base_link="base_link" tip_link="tool0"/>'
        '<joint name="joint1"/>'
        "</group>"
        '<group_state name="home" group="manipulator">'
        '<joint name="joint1" value="0"/>'
        "</group_state>"
        '<disable_collisions link1="base_link" link2="tool0" '
        'reason="Adjacent"/>'
        "</robot>"
    ).encode()
    # Deliberately not UTF-8: opaque source artifacts must remain exact bytes.
    mesh = b"\x00\xffbinary-stl\x00payload\x80"
    blobs = {
        urdf_uri: ("application/xml", urdf),
        srdf_uri: ("application/xml", srdf),
        mesh_uri: ("model/stl", mesh),
    }
    artifacts = [
        {
            "logical_uri": uri,
            "media_type": media_type,
            "byte_size": (
                urdf_declared_size
                if uri == urdf_uri and urdf_declared_size is not None
                else len(data)
            ),
            "content_digest": _sha256(data),
        }
        for uri, (media_type, data) in sorted(blobs.items())
    ]
    identity = {
        "api_version": "manyforge.model_asset.v1",
        "model_asset_id": "python_arm",
        "instance_cardinality": "multiple_per_cell",
        "expanded_source": {
            "urdf_logical_uri": urdf_uri,
            "srdf_logical_uri": srdf_uri,
            "artifacts": artifacts,
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
                "source_joint_names": ["joint1"],
            }
        ],
        "collision_body_role_bindings": [
            {
                "collision_body_role_id": "arm_collision",
                "source_link_names": ["base_link", "tool0"],
            }
        ],
    }
    document = _revisioned_document(
        identity,
        identity_domain="catalog-record-revision/model_asset",
        api_version="manyforge.model_asset.v1",
        revision_field="model_revision",
    )
    revision = json.loads(document)["model_revision"]
    catalog_key = core.ContentAddressedStoreKey("sha256/python-model-catalog")
    qualified = core.resolve_and_qualify_model_asset(
        core.ModelAssetCatalogRecordRefDTO(
            record_id=core.ModelAssetId("python_arm"),
            record_revision=core.ModelRevision(revision),
            schema_id=core.SchemaId("manyforge.model_asset.v1"),
            schema_revision=core.SchemaRevision(MODEL_ASSET_SCHEMA_REVISION),
            content_digest=core.ContentDigest(_sha256(document.encode())),
            content_addressed_store_key=catalog_key,
        ),
        catalog_key,
        document,
    )
    candidates = [
        core.ResolvedModelSourceArtifactCandidate(
            logical_uri=uri,
            resolved_store_key=_store_key(_sha256(data)),
            stored_bytes=data,
        )
        for uri, (_, data) in reversed(sorted(blobs.items()))
    ]
    return qualified, candidates, blobs


def _verified_source():
    qualified, candidates, _ = _source_fixture()
    return core.verify_model_source_closure(qualified, candidates)


def _qualified_component_type():
    identity = {
        "api_version": "manyforge.component_type.v1",
        "component_type_id": "python_arm_type",
        "tags": ["articulated-arm"],
        "model_roles": [
            {
                "model_role_id": "body",
                "cardinality": "exactly_one",
            }
        ],
        "component_root": {
            "model_role_id": "body",
            "frame_role_id": "base",
        },
        "model_mounts": [],
        "command_targets": [],
        "attachment_interfaces": [],
    }
    document = _revisioned_document(
        identity,
        identity_domain="catalog-record-revision/component_type",
        api_version="manyforge.component_type.v1",
        revision_field="component_type_revision",
    )
    revision = json.loads(document)["component_type_revision"]
    catalog_key = core.ContentAddressedStoreKey("sha256/python-type-catalog")
    return core.resolve_and_qualify_component_type(
        core.ComponentTypeCatalogRecordRefDTO(
            record_id=core.ComponentTypeId("python_arm_type"),
            record_revision=core.ComponentTypeRevision(revision),
            schema_id=core.SchemaId("manyforge.component_type.v1"),
            schema_revision=core.SchemaRevision(COMPONENT_TYPE_SCHEMA_REVISION),
            content_digest=core.ContentDigest(_sha256(document.encode())),
            content_addressed_store_key=catalog_key,
        ),
        catalog_key,
        document,
        [],
    )


def _verified_graph(source, component_type):
    graph = {
        "api_version": "manyforge.cell_graph.v1",
        "graph_revision": ZERO_DIGEST,
        "cell_id": "python_structure",
        "runtime_units": [],
        "model_instances": [
            {
                "model_instance_id": "arm_model",
                "model_asset_id": source.value.model_asset_id.value,
                "model_revision": source.value.model_revision.value,
            }
        ],
        "controllable_components": [
            {
                "controllable_component_id": "arm",
                "component_type_id": component_type.value.component_type_id.value,
                "component_type_revision": (
                    component_type.value.component_type_revision.value
                ),
                "model_role_bindings": [
                    {
                        "model_role_id": "body",
                        "model_instance_id": "arm_model",
                    }
                ],
                "runtime_unit_ids": [],
                "root_pose_in_cell": {
                    "position_m": [0, 0, 0],
                    "orientation_xyzw": [0, 0, 0, 1],
                },
            }
        ],
        "control_domains": [],
        "control_bindings": [],
        "component_attachments": [],
    }
    parsed = core.parse_strict_json_model_yaml(_canonical(graph))
    typed = core.decode_cell_graph_candidate(parsed)
    intrinsic = core.validate_intrinsic_cell_graph_candidate(typed)
    identity_ready = core.prepare_cell_graph_identity(intrinsic, [])
    return core.compute_graph_revision(identity_ready)


def test_resolved_candidate_requires_and_preserves_python_bytes():
    binary = b"\x00\xffpayload\x00\x80"
    candidate = core.ResolvedModelSourceArtifactCandidate(
        logical_uri="asset://binary/mesh.stl",
        resolved_store_key=core.ContentAddressedStoreKey("sha256/binary"),
        stored_bytes=binary,
    )

    assert candidate.stored_bytes == binary
    assert isinstance(candidate.stored_bytes, bytes)
    with pytest.raises(TypeError):
        core.ResolvedModelSourceArtifactCandidate(
            logical_uri="asset://binary/mesh.stl",
            resolved_store_key=core.ContentAddressedStoreKey("sha256/binary"),
            stored_bytes="not bytes",
        )
    with pytest.raises(AttributeError):
        candidate.logical_uri = "asset://other/mesh.stl"


def test_source_verification_is_typed_nonconstructible_and_immutable():
    qualified, candidates, blobs = _source_fixture()
    retrieval_plan = core.preflight_model_source_declaration(qualified)
    verified = core.verify_model_source_closure(
        retrieval_plan, reversed(candidates)
    )

    assert retrieval_plan.qualified_asset.value == qualified.value
    assert isinstance(retrieval_plan.artifacts, tuple)
    assert retrieval_plan.total_bytes == sum(
        len(data) for _, data in blobs.values()
    )
    assert [row.logical_uri for row in retrieval_plan.artifacts] == sorted(
        blobs
    )
    assert (
        retrieval_plan.artifacts[0].artifact_kind
        == core.ModelSourceArtifactKind.kStlMesh
    )
    assert (
        retrieval_plan.artifacts[-1].artifact_kind
        == core.ModelSourceArtifactKind.kUrdfXmlEntrypoint
    )
    for row in retrieval_plan.artifacts:
        assert row.expected_store_key == _store_key(row.content_digest.value)

    assert verified.value.model_asset_id == core.ModelAssetId("python_arm")
    assert verified.qualified_asset.value == qualified.value
    assert isinstance(verified.verified_artifacts, tuple)
    assert [item.logical_uri for item in verified.verified_artifacts] == sorted(
        blobs
    )
    assert verified.parsed_ir.robot_name == "python_arm"
    assert verified.parsed_ir.root_link_name == "base_link"
    assert verified.parsed_ir.link_names == ("base_link", "tool0")
    assert verified.parsed_ir.material_names == ()
    assert len(verified.parsed_ir.joints) == 1
    assert (
        verified.parsed_ir.joints[0].type
        == core.ParsedUrdfJointType.kRevolute
    )
    assert verified.parsed_ir.groups[0].name == "manipulator"
    assert verified.parsed_ir.groups[0].chains[0].tip_link_name == "tool0"
    assert verified.parsed_ir.group_states[0].name == "home"
    assert verified.parsed_ir.disabled_collisions[0].reason == "Adjacent"
    assert verified.parsed_ir.referenced_artifact_logical_uris == (
        "asset://python_arm/meshes/base.stl",
    )

    for type_name in (
        "VerifiedModelSourceArtifact",
        "ParsedUrdfJoint",
        "ParsedSrdfChain",
        "ParsedSrdfGroup",
        "ParsedModelSourceIr",
        "ModelSourceRetrievalPlanRow",
        "ModelSourceRetrievalPlan",
        "SourceVerifiedModelAsset",
    ):
        with pytest.raises(TypeError):
            getattr(core, type_name)()
    with pytest.raises(AttributeError):
        verified.parsed_ir.robot_name = "mutated"
    with pytest.raises(AttributeError):
        verified.parsed_ir.link_names = ()
    with pytest.raises(AttributeError):
        retrieval_plan.total_bytes = 0


def test_source_iterable_rejects_wrong_and_extra_items_at_exact_bound():
    qualified, candidates, _ = _source_fixture()

    with pytest.raises(TypeError, match="ResolvedModelSourceArtifactCandidate"):
        core.verify_model_source_closure(qualified, [object()])

    consumed = 0

    def overlong():
        nonlocal consumed
        for candidate in candidates:
            consumed += 1
            yield candidate
        consumed += 1
        yield candidates[0]
        raise AssertionError("the bounded binding consumed past one extra")

    with pytest.raises(core.ModelSourceError, match="more artifacts"):
        core.verify_model_source_closure(qualified, overlong())
    assert consumed == len(candidates) + 1

    wrong_key = core.ResolvedModelSourceArtifactCandidate(
        logical_uri=candidates[0].logical_uri,
        resolved_store_key=core.ContentAddressedStoreKey("sha256/wrong"),
        stored_bytes=candidates[0].stored_bytes,
    )
    with pytest.raises(core.ModelSourceError, match="store key"):
        core.verify_model_source_closure(
            qualified, [wrong_key, *candidates[1:]]
        )


def test_source_declaration_limits_reject_before_iterable_consumption():
    qualified, _, _ = _source_fixture(
        urdf_declared_size=core.MAX_MODEL_SOURCE_XML_ENTRYPOINT_BYTES + 1
    )
    consumed = False

    def must_not_be_consumed():
        nonlocal consumed
        consumed = True
        yield object()

    with pytest.raises(core.ModelSourceError, match="unsupported type or size"):
        core.verify_model_source_closure(qualified, must_not_be_consumed())
    assert consumed is False


def test_structure_qualification_exposes_read_only_resolutions():
    source = _verified_source()
    component_type = _qualified_component_type()
    graph = _verified_graph(source, component_type)

    qualified = core.qualify_cell_structure(
        graph, [source], [component_type]
    )

    assert qualified.graph == graph.graph
    assert qualified.verified_graph.graph == graph.graph
    assert isinstance(qualified.model_assets, tuple)
    assert isinstance(qualified.component_types, tuple)
    assert len(qualified.model_instances) == 1
    model = qualified.model_instances[0]
    assert model.model_instance_id == core.ModelInstanceId("arm_model")
    assert model.owner_component_id == core.ControllableComponentId("arm")
    assert model.owner_model_role_id == core.ModelRoleId("body")
    assert (
        model.placement_source
        == core.StructuralPlacementSource.kDirectComponentPoseInCell
    )
    component = qualified.components[0]
    assert component.component_tags == ("articulated-arm",)
    assert component.model_roles[0].model_instance_id == core.ModelInstanceId(
        "arm_model"
    )
    assert component.component_root.source_link_name == "base_link"
    assert component.command_targets == ()
    assert qualified.attachments == ()

    for type_name in (
        "StructuralModelInstanceResolution",
        "StructuralModelOccurrence",
        "StructuralFrameResolution",
        "StructuralComponentResolution",
        "StructurallyQualifiedCellGraph",
    ):
        with pytest.raises(TypeError):
            getattr(core, type_name)()
    with pytest.raises(AttributeError):
        model.placement_source = (
            core.StructuralPlacementSource.kDirectModelPoseInCell
        )
    with pytest.raises(AttributeError):
        qualified.components = ()


def test_structure_iterables_stop_at_selected_exact_closure_bounds():
    source = _verified_source()
    component_type = _qualified_component_type()
    graph = _verified_graph(source, component_type)

    source_items_consumed = 0

    def too_many_sources():
        nonlocal source_items_consumed
        source_items_consumed += 1
        yield source
        source_items_consumed += 1
        yield source
        raise AssertionError("source closure consumed past one extra")

    with pytest.raises(core.CellStructureError, match="more values"):
        core.qualify_cell_structure(
            graph, too_many_sources(), [component_type]
        )
    assert source_items_consumed == 2

    type_items_consumed = 0

    def too_many_types():
        nonlocal type_items_consumed
        type_items_consumed += 1
        yield component_type
        type_items_consumed += 1
        yield component_type
        raise AssertionError("type closure consumed past one extra")

    with pytest.raises(core.CellStructureError, match="more values"):
        core.qualify_cell_structure(graph, [source], too_many_types())
    assert type_items_consumed == 2

    with pytest.raises(TypeError, match="SourceVerifiedModelAsset"):
        core.qualify_cell_structure(graph, [component_type], [component_type])
    with pytest.raises(core.CellStructureError):
        core.qualify_cell_structure(graph, [], [component_type])


def test_source_limits_are_exported_from_the_compiler_contract():
    assert core.MAX_MODEL_SOURCE_XML_ENTRYPOINT_BYTES == 16 * 1024 * 1024
    assert core.MAX_MODEL_SOURCE_OPAQUE_ARTIFACT_BYTES == 256 * 1024 * 1024
    assert core.MAX_MODEL_SOURCE_CLOSURE_BYTES == 512 * 1024 * 1024

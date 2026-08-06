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
assert core.MAX_MODEL_SOURCE_COLLADA_XML_DEPTH == 128
assert core.MAX_MODEL_SOURCE_COLLADA_XML_ELEMENT_COUNT == 250_000
assert core.MAX_MODEL_SOURCE_COLLADA_XML_ATTRIBUTE_COUNT == 1_000_000
assert core.MAX_MODEL_SOURCE_EXPANDED_SRDF_SEMANTIC_ROWS == 250_000


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


def _source_fixture(
    *, urdf_declared_size: int | None = None, rich: bool = False
):
    urdf_uri = "asset://python_arm/urdf/model.urdf"
    srdf_uri = "asset://python_arm/srdf/model.srdf"
    mesh_uri = "asset://python_arm/meshes/base.stl"
    texture_uri = "asset://python_arm/textures/steel.png"
    if rich:
        urdf = (
            '<robot name="python_arm">'
            '<material name="Steel"><color rgba="0.1 0.2 0.3 1"/>'
            f'<texture filename="{texture_uri}"/></material>'
            '<link name="base_link"><inertial>'
            '<origin xyz="0.1 0.2 0.3" rpy="0.4 0.5 0.6"/>'
            '<mass value="2.5"/><inertia ixx="1" ixy="0.1" ixz="0.2" '
            'iyy="2" iyz="0.3" izz="3"/></inertial>'
            '<visual name="mesh_visual">'
            '<origin xyz="1 2 3" rpy="0.1 0.2 0.3"/><geometry>'
            f'<mesh filename="{mesh_uri}" scale="2 3 4"/>'
            '</geometry><material name="Steel"/></visual>'
            '<collision name="box_collision"><geometry>'
            '<box size="1 2 3"/></geometry></collision></link>'
            '<link name="tool0"><visual name="box_visual"><geometry>'
            '<box size="4 5 6"/></geometry>'
            '<material name="Inline"><color rgba="1 0 0 0.5"/>'
            '</material></visual></link>'
            '<link name="aux1"><collision><origin xyz="0 0 1"/>'
            '<geometry><cylinder radius="0.5" length="2"/></geometry>'
            '</collision></link>'
            '<link name="aux2"><visual><geometry>'
            '<sphere radius="0.75"/></geometry><material name="">'
            '<color rgba="0.2 0.4 0.6 1"/></material></visual></link>'
            '<link name="aux3"/><link name="aux4"/><link name="aux5"/>'
            '<joint name="joint1" type="revolute">'
            '<origin xyz="0.1 0.2 0.3" rpy="0.4 0.5 0.6"/>'
            '<parent link="base_link"/><child link="tool0"/>'
            '<axis xyz="0 0 1"/>'
            '<calibration rising="1.1" falling="1.2" '
            'reference_position="1.3"/>'
            '<dynamics damping="0.2" friction="0.3"/>'
            '<limit lower="-3" upper="3" effort="4" velocity="5"/>'
            '<safety_controller soft_lower_limit="-2" '
            'soft_upper_limit="2" k_position="6" k_velocity="7"/>'
            '</joint>'
            '<joint name="joint2" type="continuous">'
            '<parent link="tool0"/><child link="aux1"/>'
            '<dynamics damping="0.4"/>'
            '<limit effort="8" velocity="9"/>'
            '<mimic joint="joint1" multiplier="-2" offset="0.5"/>'
            '</joint>'
            '<joint name="joint3" type="prismatic">'
            '<parent link="aux1"/><child link="aux2"/>'
            '<axis xyz="1 0 0"/>'
            '<limit lower="0" upper="1" effort="10" velocity="11"/>'
            '</joint>'
            '<joint name="joint4" type="fixed">'
            '<parent link="aux2"/><child link="aux3"/></joint>'
            '<joint name="joint5" type="planar">'
            '<parent link="aux3"/><child link="aux4"/>'
            '<axis xyz="0 0 1"/></joint>'
            '<joint name="joint6" type="floating">'
            '<parent link="aux4"/><child link="aux5"/></joint>'
            '</robot>'
        ).encode()
        srdf = (
            '<robot name="python_arm">'
            '<group name="manipulator">'
            '<link name="aux2"/><joint name="joint3"/>'
            '<chain base_link="base_link" tip_link="aux1"/>'
            '<group name="tooling"/></group>'
            '<group name="tooling"><link name="aux2"/></group>'
            '<group_state name="home" group="manipulator">'
            '<joint name="joint1" value="0.25"/>'
            '<joint name="joint3" value="0.5"/></group_state>'
            '<end_effector name="tool" parent_link="aux2" '
            'group="tooling" parent_group="manipulator"/>'
            '<passive_joint name="joint2"/>'
            '<disable_collisions link1="base_link" link2="tool0" '
            'reason="Adjacent"/>'
            '</robot>'
        ).encode()
    else:
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
    if rich:
        blobs[texture_uri] = ("image/png", b"\x89PNG\r\n\x1a\nfixture")
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


def _verified_deployment(source, component_type):
    deployment = {
        "api_version": "manyforge.deployment.v1",
        "deployment_artifact_id": "python_compiler",
        "deployment_revision": ZERO_DIGEST,
        "cell_id": "python_structure",
        "name": "Python compiler fixture",
        "catalogs": {"records": []},
        "graph": {
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
                    "component_type_id": (
                        component_type.value.component_type_id.value
                    ),
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
        },
        "desired_control_configurations": [],
        "capability_provider_bindings": [],
    }
    parsed = core.parse_strict_json_model_yaml(_canonical(deployment))
    return core.compute_deployment_artifact_revision(parsed, [])


def _final_pipeline(source, component_type):
    deployment = _verified_deployment(source, component_type)
    composition = core.compose_cell_occurrences(
        deployment.graph, [source], [component_type]
    )
    deployment_inputs = deployment.applicability_inputs
    selected_overlay = core.select_site_overlay_for_deployment(
        deployment_inputs.value.site_overlay_ref,
        deployment_inputs.value.external_resources,
        None,
    )
    applicability = core.qualify_cell_applicability(
        composition,
        deployment_inputs,
        [],
        [],
        [],
        [],
        [],
        [],
        [],
        selected_overlay,
    )
    compiled = core.compile_cell_ir(applicability)
    return deployment, composition, selected_overlay, applicability, compiled


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
        core.ModelSourceArtifactKind.kColladaMesh
        != core.ModelSourceArtifactKind.kStlMesh
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
    assert verified.source_ir.urdf.robot_name == "python_arm"
    assert verified.source_ir.semantic_index.root_link_name == "base_link"
    assert tuple(link.name for link in verified.source_ir.urdf.links) == (
        "base_link",
        "tool0",
    )
    assert verified.source_ir.urdf.materials == ()
    assert len(verified.source_ir.urdf.joints) == 1
    assert (
        verified.source_ir.urdf.joints[0].type
        == core.UrdfJointType.kRevolute
    )
    assert set(core.UrdfGeometryIr.__args__) == {
        core.UrdfBoxGeometryIr,
        core.UrdfCylinderGeometryIr,
        core.UrdfSphereGeometryIr,
        core.UrdfMeshGeometryIr,
    }
    assert set(core.SrdfGroupMemberIr.__args__) == {
        core.SrdfLinkGroupMemberIr,
        core.SrdfJointGroupMemberIr,
        core.SrdfChainGroupMemberIr,
        core.SrdfSubgroupMemberIr,
    }
    assert verified.source_ir.srdf.groups[0].name == "manipulator"
    assert (
        verified.source_ir.srdf.groups[0].members[0].tip_link_name
        == "tool0"
    )
    assert verified.source_ir.srdf.group_states[0].name == "home"
    assert (
        verified.source_ir.srdf.disabled_collisions[0].reason == "Adjacent"
    )
    assert tuple(
        item.logical_uri for item in verified.source_ir.artifact_references
    ) == (
        "asset://python_arm/meshes/base.stl",
    )
    assert isinstance(
        verified.source_ir.semantic_index.group_expansion_postorder, tuple
    )
    assert isinstance(
        verified.source_ir.semantic_index.group_expansion_provenance, tuple
    )
    assert isinstance(
        verified.source_ir.semantic_index.expanded_groups, tuple
    )
    expanded = verified.source_ir.semantic_index.expanded_groups[0]
    assert isinstance(expanded.links, tuple)
    assert isinstance(expanded.joints, tuple)
    assert isinstance(expanded.joints[0].provenance_indices, tuple)
    assert len(expanded.joints[0].provenance_indices) == 2

    canonical_urdf = core.emit_canonical_urdf(verified.source_ir)
    canonical_srdf = core.emit_canonical_srdf(verified.source_ir)
    assert isinstance(canonical_urdf, str)
    assert canonical_urdf.startswith('<robot name="python_arm">')
    assert isinstance(canonical_srdf, str)
    assert canonical_srdf.startswith('<robot name="python_arm">')

    for type_name in (
        "VerifiedModelSourceArtifact",
        "UrdfOriginIr",
        "UrdfBoxGeometryIr",
        "UrdfCylinderGeometryIr",
        "UrdfSphereGeometryIr",
        "UrdfMeshGeometryIr",
        "UrdfMaterialColorIr",
        "UrdfMaterialTextureIr",
        "UrdfResolvedMaterialReferenceIr",
        "UrdfMaterialIr",
        "UrdfVisualIr",
        "UrdfCollisionIr",
        "UrdfInertiaIr",
        "UrdfInertialIr",
        "UrdfLinkIr",
        "UrdfAxisIr",
        "UrdfCalibrationIr",
        "UrdfDynamicsIr",
        "UrdfLimitIr",
        "UrdfMimicIr",
        "UrdfSafetyControllerIr",
        "UrdfJointIr",
        "UrdfDocumentIr",
        "SrdfLinkGroupMemberIr",
        "SrdfJointGroupMemberIr",
        "SrdfChainGroupMemberIr",
        "SrdfSubgroupMemberIr",
        "SrdfGroupIr",
        "SrdfGroupStateJointIr",
        "SrdfGroupStateIr",
        "SrdfEndEffectorIr",
        "SrdfDisabledCollisionIr",
        "SrdfDocumentIr",
        "SourceArtifactReferenceIr",
        "ModelSourceNameIndexEntryIr",
        "ModelSourceGroupStateIndexEntryIr",
        "SrdfExpansionProvenanceNodeIr",
        "SrdfExpandedMemberIr",
        "ExpandedSrdfGroupIr",
        "ModelSourceSemanticIndex",
        "ModelSourceIr",
        "ModelSourceRetrievalPlanRow",
        "ModelSourceRetrievalPlan",
        "SourceVerifiedModelAsset",
    ):
        with pytest.raises(TypeError):
            getattr(core, type_name)()
    with pytest.raises(AttributeError):
        verified.source_ir.urdf.robot_name = "mutated"
    with pytest.raises(AttributeError):
        verified.source_ir.urdf.links = ()
    with pytest.raises(AttributeError):
        expanded.group_name = "mutated"
    with pytest.raises(TypeError):
        expanded.joints[0].provenance_indices[0] = 42
    with pytest.raises(AttributeError):
        retrieval_plan.total_bytes = 0

    for removed_name in (
        "ParsedModelSourceIr",
        "ParsedUrdfJoint",
        "ParsedUrdfJointType",
        "ParsedSrdfChain",
        "ParsedSrdfDisabledCollision",
        "ParsedSrdfEndEffector",
        "ParsedSrdfGroup",
        "ParsedSrdfGroupState",
        "ParsedSrdfGroupStateJoint",
    ):
        assert not hasattr(core, removed_name)


def test_complete_model_source_values_have_python_semantic_parity():
    qualified, candidates, _ = _source_fixture(rich=True)
    source = core.verify_model_source_closure(
        qualified, candidates
    ).source_ir

    material = source.urdf.materials[0]
    assert material.name == "Steel"
    assert material.color.rgba == pytest.approx((0.1, 0.2, 0.3, 1.0))
    assert (
        material.texture.logical_uri
        == "asset://python_arm/textures/steel.png"
    )

    links = {link.name: link for link in source.urdf.links}
    inertial = links["base_link"].inertial
    assert inertial.origin.element_authored
    assert inertial.origin.authored_xyz_m == pytest.approx((0.1, 0.2, 0.3))
    assert inertial.origin.authored_rpy_rad == pytest.approx((0.4, 0.5, 0.6))
    assert inertial.origin.xyz_m == pytest.approx((0.1, 0.2, 0.3))
    assert inertial.mass_kg == pytest.approx(2.5)
    assert (
        inertial.inertia.ixx,
        inertial.inertia.ixy,
        inertial.inertia.ixz,
        inertial.inertia.iyy,
        inertial.inertia.iyz,
        inertial.inertia.izz,
    ) == pytest.approx((1.0, 0.1, 0.2, 2.0, 0.3, 3.0))

    mesh_visual = links["base_link"].visuals[0]
    assert mesh_visual.name == "mesh_visual"
    assert isinstance(mesh_visual.geometry, core.UrdfMeshGeometryIr)
    assert mesh_visual.geometry.authored_scale == pytest.approx((2, 3, 4))
    assert mesh_visual.geometry.scale == pytest.approx((2, 3, 4))
    assert mesh_visual.material.resolved_reference.definition_name == "Steel"
    assert mesh_visual.material.resolved_reference.color.rgba == pytest.approx(
        (0.1, 0.2, 0.3, 1.0)
    )
    assert (
        mesh_visual.material.resolved_reference.texture.logical_uri
        == "asset://python_arm/textures/steel.png"
    )
    assert isinstance(
        links["base_link"].collisions[0].geometry,
        core.UrdfBoxGeometryIr,
    )
    assert links["base_link"].collisions[0].geometry.size_m == pytest.approx(
        (1, 2, 3)
    )
    inline = links["tool0"].visuals[0]
    assert isinstance(inline.geometry, core.UrdfBoxGeometryIr)
    assert inline.material.name == "Inline"
    assert inline.material.color.rgba == pytest.approx((1, 0, 0, 0.5))
    cylinder = links["aux1"].collisions[0]
    assert cylinder.origin.element_authored
    assert isinstance(cylinder.geometry, core.UrdfCylinderGeometryIr)
    assert cylinder.geometry.radius_m == pytest.approx(0.5)
    assert cylinder.geometry.length_m == pytest.approx(2)
    sphere_visual = links["aux2"].visuals[0]
    assert isinstance(sphere_visual.geometry, core.UrdfSphereGeometryIr)
    assert sphere_visual.geometry.radius_m == pytest.approx(0.75)
    assert sphere_visual.material.name is None
    assert sphere_visual.material.color.rgba == pytest.approx(
        (0.2, 0.4, 0.6, 1.0)
    )
    assert (
        '<material name=""><color rgba="0.2 0.4 0.6 1"/></material>'
        in core.emit_canonical_urdf(source)
    )

    joints = {joint.name: joint for joint in source.urdf.joints}
    assert {
        name: (joint.type, joint.coordinate_kind)
        for name, joint in joints.items()
    } == {
        "joint1": (core.UrdfJointType.kRevolute, core.UrdfCoordinateKind.kAngular),
        "joint2": (
            core.UrdfJointType.kContinuous,
            core.UrdfCoordinateKind.kAngular,
        ),
        "joint3": (core.UrdfJointType.kPrismatic, core.UrdfCoordinateKind.kLinear),
        "joint4": (core.UrdfJointType.kFixed, core.UrdfCoordinateKind.kNone),
        "joint5": (core.UrdfJointType.kPlanar, core.UrdfCoordinateKind.kPlanar),
        "joint6": (
            core.UrdfJointType.kFloating,
            core.UrdfCoordinateKind.kFloating,
        ),
    }

    joint1 = joints["joint1"]
    assert joint1.parent_link_name == "base_link"
    assert joint1.child_link_name == "tool0"
    assert joint1.origin.element_authored
    assert joint1.axis.element_authored
    assert joint1.axis.authored_xyz == pytest.approx((0, 0, 1))
    assert joint1.axis.xyz == pytest.approx((0, 0, 1))
    assert (
        joint1.calibration.rising,
        joint1.calibration.falling,
        joint1.calibration.authored_reference_position,
        joint1.calibration.reference_position,
    ) == pytest.approx((1.1, 1.2, 1.3, 1.3))
    assert (
        joint1.dynamics.authored_damping,
        joint1.dynamics.authored_friction,
        joint1.dynamics.damping,
        joint1.dynamics.friction,
    ) == pytest.approx((0.2, 0.3, 0.2, 0.3))
    assert (
        joint1.limit.authored_lower,
        joint1.limit.authored_upper,
        joint1.limit.lower,
        joint1.limit.upper,
        joint1.limit.effort,
        joint1.limit.velocity,
    ) == pytest.approx((-3, 3, -3, 3, 4, 5))
    assert (
        joint1.safety_controller.authored_soft_lower_limit,
        joint1.safety_controller.authored_soft_upper_limit,
        joint1.safety_controller.authored_k_position,
        joint1.safety_controller.soft_lower_limit,
        joint1.safety_controller.soft_upper_limit,
        joint1.safety_controller.k_position,
        joint1.safety_controller.k_velocity,
    ) == pytest.approx((-2, 2, 6, -2, 2, 6, 7))

    joint2 = joints["joint2"]
    assert not joint2.axis.element_authored
    assert joint2.axis.authored_xyz is None
    assert joint2.axis.xyz == pytest.approx((1, 0, 0))
    assert joint2.dynamics.authored_friction is None
    assert joint2.dynamics.friction == 0
    assert joint2.limit.authored_lower is None
    assert joint2.limit.authored_upper is None
    assert joint2.limit.lower == 0
    assert joint2.limit.upper == 0
    assert joint2.mimic.joint_name == "joint1"
    assert (
        joint2.mimic.authored_multiplier,
        joint2.mimic.authored_offset,
        joint2.mimic.multiplier,
        joint2.mimic.offset,
    ) == pytest.approx((-2, 0.5, -2, 0.5))
    assert joints["joint4"].axis is None
    assert joints["joint5"].axis.xyz == pytest.approx((0, 0, 1))
    assert joints["joint6"].axis is None

    groups = {group.name: group for group in source.srdf.groups}
    members = groups["manipulator"].members
    assert isinstance(members[0], core.SrdfLinkGroupMemberIr)
    assert members[0].link_name == "aux2"
    assert isinstance(members[1], core.SrdfJointGroupMemberIr)
    assert members[1].joint_name == "joint3"
    assert isinstance(members[2], core.SrdfChainGroupMemberIr)
    assert members[2].base_link_name == "base_link"
    assert members[2].tip_link_name == "aux1"
    assert isinstance(members[3], core.SrdfSubgroupMemberIr)
    assert members[3].group_name == "tooling"
    state = source.srdf.group_states[0]
    assert (state.group_name, state.name) == ("manipulator", "home")
    assert tuple((joint.joint_name, joint.value) for joint in state.joints) == (
        ("joint1", 0.25),
        ("joint3", 0.5),
    )
    end_effector = source.srdf.end_effectors[0]
    assert (
        end_effector.name,
        end_effector.parent_link_name,
        end_effector.group_name,
        end_effector.parent_group_name,
    ) == ("tool", "aux2", "tooling", "manipulator")
    assert source.srdf.passive_joint_names == ("joint2",)
    assert (
        source.srdf.disabled_collisions[0].first_link_name,
        source.srdf.disabled_collisions[0].second_link_name,
        source.srdf.disabled_collisions[0].reason,
    ) == ("base_link", "tool0", "Adjacent")

    assert tuple(
        (reference.logical_uri, reference.kind)
        for reference in source.artifact_references
    ) == (
        (
            "asset://python_arm/meshes/base.stl",
            core.SourceArtifactReferenceKind.kMesh,
        ),
        (
            "asset://python_arm/textures/steel.png",
            core.SourceArtifactReferenceKind.kTexture,
        ),
    )
    provenance = source.semantic_index.group_expansion_provenance
    assert {
        node.kind for node in provenance
    } >= {
        core.SrdfExpansionProvenanceKind.kDirectLink,
        core.SrdfExpansionProvenanceKind.kDirectJoint,
        core.SrdfExpansionProvenanceKind.kChainLink,
        core.SrdfExpansionProvenanceKind.kChainJoint,
        core.SrdfExpansionProvenanceKind.kSubgroup,
    }
    subgroup_nodes = [
        node
        for node in provenance
        if node.kind == core.SrdfExpansionProvenanceKind.kSubgroup
    ]
    assert subgroup_nodes
    assert all(node.source_symbol_name == "tooling" for node in subgroup_nodes)
    assert all(node.nested_provenance_index is not None for node in subgroup_nodes)
    chain_nodes = [
        node
        for node in provenance
        if node.kind
        in {
            core.SrdfExpansionProvenanceKind.kChainLink,
            core.SrdfExpansionProvenanceKind.kChainJoint,
        }
    ]
    assert chain_nodes
    assert all(node.chain_base_link_name == "base_link" for node in chain_nodes)
    assert all(node.chain_tip_link_name == "aux1" for node in chain_nodes)


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
    deployment, composition, selected_overlay, applicability, compiled = (
        _final_pipeline(source, component_type)
    )

    assert isinstance(deployment, core.RevisionVerifiedDeploymentArtifact)
    assert isinstance(deployment.graph, core.GraphRevisionVerifiedCellGraph)
    assert isinstance(composition, core.CompositionQualifiedCellIr)
    assert isinstance(selected_overlay, core.QualifiedSelectedSiteOverlay)
    assert isinstance(applicability, core.ApplicabilityQualifiedCellIr)
    assert isinstance(compiled, core.CompilerQualifiedCellIr)
    assert composition.graph == deployment.graph.graph
    assert composition.verified_graph.graph == deployment.graph.graph

    collection_names = (
        "model_assets",
        "component_types",
        "model_occurrences",
        "symbol_rewrites",
        "reverse_symbol_rewrites",
        "reference_rewrites",
        "planning_resource_rewrites",
        "frames",
        "fixed_connections",
        "frame_paths",
        "coordinates",
        "control_binding_coordinate_maps",
        "targets",
    )
    for name in collection_names:
        assert isinstance(getattr(composition, name), tuple)

    assert len(composition.model_assets) == 1
    assert composition.model_assets[0].value.model_asset_id == core.ModelAssetId(
        "python_arm"
    )
    assert len(composition.component_types) == 1
    retained_type = composition.component_types[0].value
    assert retained_type.component_type_id == core.ComponentTypeId(
        "python_arm_type"
    )
    assert tuple(retained_type.tags) == ("articulated-arm",)
    assert retained_type.model_roles[0].model_role_id == core.ModelRoleId("body")

    assert len(composition.model_occurrences) == 1
    occurrence = composition.model_occurrences[0]
    assert isinstance(occurrence, core.CompositionModelOccurrence)
    assert occurrence.model_instance_id == core.ModelInstanceId("arm_model")
    assert occurrence.model_asset_id == core.ModelAssetId("python_arm")
    assert occurrence.model_revision == source.value.model_revision
    assert occurrence.owner_component_id == core.ControllableComponentId("arm")
    assert occurrence.owner_model_role_id == core.ModelRoleId("body")
    assert occurrence.compiled_model_name
    assert occurrence.compiled_robot_name
    assert occurrence.compiled_root_link_name

    assert composition.cell_frame.cell_id == core.CellId("python_structure")
    assert composition.cell_frame.compiled_frame_name
    assert isinstance(composition.flattened_urdf, core.UrdfDocumentIr)
    assert isinstance(composition.flattened_srdf, core.SrdfDocumentIr)
    assert isinstance(composition.flattened_urdf.materials, tuple)
    assert isinstance(composition.flattened_urdf.links, tuple)
    assert isinstance(composition.flattened_urdf.joints, tuple)
    assert isinstance(composition.flattened_srdf.groups, tuple)
    assert occurrence.compiled_root_link_name in {
        link.name for link in composition.flattened_urdf.links
    }

    root_frames = [
        row
        for row in composition.frames
        if isinstance(row.frame, core.CompositionComponentModelFrame)
        and row.frame.frame_role_id == core.FrameRoleId("base")
    ]
    assert len(root_frames) == 1
    root_frame = root_frames[0]
    assert root_frame.frame.controllable_component_id == (
        core.ControllableComponentId("arm")
    )
    assert root_frame.frame.model_instance_id == core.ModelInstanceId("arm_model")
    assert root_frame.source_link_name == "base_link"
    assert root_frame.frame.compiled_frame_name == occurrence.compiled_root_link_name

    assert len(composition.fixed_connections) == 1
    connection = composition.fixed_connections[0]
    assert isinstance(connection.source, core.CompositionDirectComponentAnchor)
    assert connection.source.controllable_component_id == (
        core.ControllableComponentId("arm")
    )
    assert connection.source.root_model_instance_id == core.ModelInstanceId(
        "arm_model"
    )
    assert connection.compiled_child_link_name == occurrence.compiled_root_link_name
    assert composition.graph.component_attachments == []
    assert composition.control_binding_coordinate_maps == ()
    assert composition.targets == ()

    assert applicability.composition.graph == composition.graph
    assert applicability.deployment_inputs.value == (
        deployment.applicability_inputs.value
    )
    assert applicability.selected_site_overlay_ref is None
    assert not applicability.assistant_settings.enabled
    assert (
        applicability.assistant_settings.normalized_projection_canonical_json
        == "{}"
    )
    assert applicability.assistant_settings.projection_digest == core.ContentDigest(
        ZERO_DIGEST
    )
    for name in (
        "runtime_profiles",
        "capability_definitions",
        "control_interface_contracts",
        "controller_topologies",
        "provider_manifests",
        "stop_evidence_policies",
        "binding_interfaces",
        "controllers",
        "routes",
        "capabilities",
        "provider_endpoints",
        "external_resources",
        "stop_applicability",
    ):
        assert getattr(applicability, name) == ()

    assert compiled.applicability.composition.graph == composition.graph
    assert isinstance(compiled.catalog_record_refs, tuple)
    assert tuple(type(value) for value in compiled.catalog_record_refs) == (
        core.ComponentTypeCatalogRecordRefDTO,
        core.ModelAssetCatalogRecordRefDTO,
    )
    assert tuple(value.record_id.value for value in compiled.catalog_record_refs) == (
        "python_arm_type",
        "python_arm",
    )

    for type_name in (
        "GraphRevisionVerifiedCellGraph",
        "RevisionVerifiedDeploymentArtifact",
        "RevisionVerifiedDeploymentApplicabilityInputs",
        "QualifiedSelectedSiteOverlay",
        "CompositionQualifiedCellIr",
        "CompositionModelOccurrence",
        "CompositionComponentModelFrame",
        "CompositionFrameResolution",
        "CompositionDirectComponentAnchor",
        "CompositionFixedConnection",
        "ApplicabilityQualifiedCellIr",
        "CompilerQualifiedCellIr",
    ):
        with pytest.raises(TypeError):
            getattr(core, type_name)()
    with pytest.raises(AttributeError):
        occurrence.owner_component_id = core.ControllableComponentId("other")
    with pytest.raises(TypeError):
        composition.model_occurrences[0] = occurrence
    with pytest.raises(AttributeError):
        composition.model_occurrences = ()
    with pytest.raises(AttributeError):
        compiled.catalog_record_refs = ()
    for value in (composition, applicability, compiled, occurrence, root_frame):
        with pytest.raises(AttributeError):
            value.qualification_only_field = True

    assert not hasattr(core, "qualify_cell_structure")
    assert not hasattr(core, "CellStructureError")
    assert not any(
        name.startswith(("Structural", "Structurally")) for name in dir(core)
    )


def test_structure_iterables_stop_at_selected_exact_closure_bounds():
    source = _verified_source()
    component_type = _qualified_component_type()
    graph = _verified_deployment(source, component_type).graph

    source_items_consumed = 0

    def too_many_sources():
        nonlocal source_items_consumed
        source_items_consumed += 1
        yield source
        source_items_consumed += 1
        yield source
        raise AssertionError("source closure consumed past one extra")

    with pytest.raises(core.CellCompositionError, match="more values"):
        core.compose_cell_occurrences(
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

    with pytest.raises(core.CellCompositionError, match="more values"):
        core.compose_cell_occurrences(graph, [source], too_many_types())
    assert type_items_consumed == 2

    with pytest.raises(TypeError, match="SourceVerifiedModelAsset"):
        core.compose_cell_occurrences(graph, [component_type], [component_type])
    with pytest.raises(core.CellCompositionError, match="missing selected asset"):
        core.compose_cell_occurrences(graph, [], [component_type])

    _, _, _, _, compiled = _final_pipeline(source, component_type)
    assert len(compiled.applicability.composition.model_occurrences) == 1
    assert len(compiled.catalog_record_refs) == 2


def test_source_limits_are_exported_from_the_compiler_contract():
    assert core.MAX_MODEL_SOURCE_XML_ENTRYPOINT_BYTES == 16 * 1024 * 1024
    assert core.MAX_MODEL_SOURCE_OPAQUE_ARTIFACT_BYTES == 256 * 1024 * 1024
    assert core.MAX_MODEL_SOURCE_CLOSURE_BYTES == 512 * 1024 * 1024

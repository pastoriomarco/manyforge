from __future__ import annotations

import json
from pathlib import Path

import pytest

from verification.composable_cell_i1a.contracts import (
    BindingRevision,
    CapabilityId,
    CapabilitySpec,
    CatalogRevision,
    CommandTargetId,
    CommandTargetSpec,
    CompilerRevision,
    ComponentTypeId,
    ComponentTypeRecord,
    ControllableComponentId,
    ControlBindingId,
    ControlDomainId,
    ControlInterfaceContract,
    ControlRouteSpec,
    ControllerId,
    ControllerSpec,
    ControllerTopologyId,
    ControllerTopologyRecord,
    ControllerTopologyRevision,
    GraphRevision,
    InterfaceContractRevision,
    ModelAssetId,
    ResourceId,
    RetainedBundleManifest,
    RouteId,
)
from verification.composable_cell_i1a.compiler_prototype import (
    AttachmentInput,
    CompiledBundle,
    CompilerPrototypeError,
    ModelInstanceInput,
    ModelSource,
    compile_control_binding,
    compile_model_cell,
    expand_srdf_group_joints,
    parse_srdf_groups,
    to_retained_manifest,
)


REPO_ROOT = Path(__file__).resolve().parents[2]
MODEL_ROOT = REPO_ROOT / "manyforge_example_models" / "models"
FIXTURES = (
    REPO_ROOT
    / "verification"
    / "composable_cell_i1a"
    / "fixtures"
)


def _source(asset_id: str, directory: str, stem: str) -> ModelSource:
    root = MODEL_ROOT / directory
    return ModelSource(
        asset_id=asset_id,
        model_name=stem,
        urdf_path=root / f"{stem}.urdf",
        srdf_path=root / f"{stem}.srdf",
        package_paths=(str(REPO_ROOT),),
    )


LITE6 = _source(
    "asset.lite6_lite_gripper",
    "lite6_lite_gripper",
    "lite6_lite_gripper",
)
UR10 = _source(
    "asset.ur10e_robotiq",
    "ur10e_robotiq_2f_140",
    "ur10e_robotiq_2f_140",
)
COMPOSABLE_ARM = ModelSource(
    asset_id="asset.composable_arm",
    model_name="composable_arm",
    urdf_path=FIXTURES / "composable_arm.urdf",
    srdf_path=FIXTURES / "composable_arm.srdf",
    package_paths=(str(REPO_ROOT),),
)
COMPOSABLE_TOOL = ModelSource(
    asset_id="asset.composable_tool",
    model_name="composable_tool",
    urdf_path=FIXTURES / "composable_tool.urdf",
    srdf_path=FIXTURES / "composable_tool.srdf",
    package_paths=(str(REPO_ROOT),),
)


def _lite6_instance(name: str, prefix: str) -> ModelInstanceInput:
    return ModelInstanceInput(
        component_id=f"component.{name}",
        model_instance_id=f"model.{name}",
        asset_id=LITE6.asset_id,
        prefix=prefix,
        command_targets=(("arm", "lite6"), ("gripper", "lite_gripper")),
    )


def test_checked_in_models_compile_deterministically_and_reload() -> None:
    inputs = (
        _lite6_instance("left", "left_"),
        ModelInstanceInput(
            component_id="component.ur",
            model_instance_id="model.ur",
            asset_id=UR10.asset_id,
            prefix="ur_",
            command_targets=(("arm", "arm"), ("gripper", "gripper")),
        ),
    )
    first = compile_model_cell(sources=(LITE6, UR10), instances=inputs)
    repeat = compile_model_cell(
        sources=(UR10, LITE6),
        instances=tuple(reversed(inputs)),
    )
    loaded = CompiledBundle.from_json(first.canonical_json)

    assert repeat.canonical_json == first.canonical_json
    assert repeat.sha256 == first.sha256
    assert loaded == first
    assert len(first.sha256) == 64


def test_checked_in_golden_digest_pins_deterministic_compiler_output() -> None:
    golden = json.loads(
        (FIXTURES / "p4_golden_digest.experimental.json").read_text(
            encoding="utf-8"
        )
    )
    assert set(golden) == {
        "bundle_sha256",
        "case",
        "manifest_sha256",
        "schema",
    }
    assert golden["schema"] == "manyforge.experimental.composable_cell.i1a.v0"
    assert golden["case"] == "single_composable_arm"

    bundle = compile_model_cell(
        sources=(COMPOSABLE_ARM,),
        instances=(
            ModelInstanceInput(
                component_id="component.golden.arm",
                model_instance_id="model.golden.arm",
                asset_id=COMPOSABLE_ARM.asset_id,
                prefix="golden_",
                command_targets=(("arm", "arm"),),
            ),
        ),
    )
    manifest = to_retained_manifest(
        bundle,
        compiler_revision=CompilerRevision("compiler-golden-1"),
        catalog_revision=CatalogRevision("catalog-golden-1"),
        graph_revision=GraphRevision(1),
    )

    assert bundle.sha256 == golden["bundle_sha256"]
    assert manifest.sha256() == golden["manifest_sha256"]


def test_compiler_identity_is_independent_of_absolute_checkout_paths(
    tmp_path: Path,
) -> None:
    def copied_source(root: Path) -> ModelSource:
        model_dir = root / "models"
        package_dir = root / "packages"
        model_dir.mkdir(parents=True)
        package_dir.mkdir()
        urdf = model_dir / "composable_arm.urdf"
        srdf = model_dir / "composable_arm.srdf"
        urdf.write_bytes(COMPOSABLE_ARM.urdf_path.read_bytes())
        srdf.write_bytes(COMPOSABLE_ARM.srdf_path.read_bytes())
        return ModelSource(
            asset_id=COMPOSABLE_ARM.asset_id,
            model_name=COMPOSABLE_ARM.model_name,
            urdf_path=urdf,
            srdf_path=srdf,
            package_paths=(str(package_dir),),
        )

    instance = ModelInstanceInput(
        component_id="component.portable.arm",
        model_instance_id="model.portable.arm",
        asset_id=COMPOSABLE_ARM.asset_id,
        prefix="portable_",
        command_targets=(("arm", "arm"),),
    )
    first = compile_model_cell(
        sources=(copied_source(tmp_path / "checkout-a"),),
        instances=(instance,),
    )
    second = compile_model_cell(
        sources=(copied_source(tmp_path / "other" / "checkout-b"),),
        instances=(instance,),
    )

    assert first == second
    assert first.canonical_json == second.canonical_json
    assert first.sha256 == second.sha256
    assert (
        first.instances[0].asset_fingerprint
        == second.instances[0].asset_fingerprint
    )
    assert (
        first.instances[0].instance_fingerprint
        == second.instances[0].instance_fingerprint
    )


def test_two_instances_reuse_one_asset_without_symbol_aliasing() -> None:
    bundle = compile_model_cell(
        sources=(LITE6,),
        instances=(
            _lite6_instance("left", "left_"),
            _lite6_instance("right", "right_"),
        ),
    )
    left, right = bundle.instances

    assert left.asset_id == right.asset_id
    assert left.asset_fingerprint == right.asset_fingerprint
    assert left.instance_fingerprint != right.instance_fingerprint
    for kind in ("frames", "groups", "joints", "links"):
        assert set(left.symbols_for(kind).values()).isdisjoint(
            right.symbols_for(kind).values()
        )
    assert dict(left.symbols_for("groups"))["lite6"] == "left_lite6"
    assert dict(right.symbols_for("groups"))["lite6"] == "right_lite6"

    manifest = to_retained_manifest(
        bundle,
        compiler_revision=CompilerRevision("compiler-p4"),
        catalog_revision=CatalogRevision("catalog-p4"),
        graph_revision=GraphRevision(1),
    )
    assert RetainedBundleManifest.from_json(manifest.to_json()) == manifest
    assert len(manifest.symbol_maps) == 2
    assert manifest.artifacts[0].name == "compiled-model-bundle"
    assert manifest.artifacts[0].relative_path == (
        "manifests/compiled-model-bundle.json"
    )
    assert manifest.artifacts[0].content_sha256.value == bundle.sha256


def test_only_declared_command_targets_are_compiled() -> None:
    instance = ModelInstanceInput(
        component_id="component.arm_only",
        model_instance_id="model.arm_only",
        asset_id=LITE6.asset_id,
        prefix="arm_only_",
        command_targets=(("arm", "lite6"),),
    )
    bundle = compile_model_cell(sources=(LITE6,), instances=(instance,))

    assert [target[0] for target in bundle.instances[0].targets] == ["arm"]
    assert all(target[0] != "lite_gripper" for target in bundle.instances[0].targets)


def test_opaque_aggregate_remains_one_instance_with_multiple_targets() -> None:
    bundle = compile_model_cell(
        sources=(LITE6,),
        instances=(_lite6_instance("aggregate", "aggregate_"),),
    )

    assert len(bundle.instances) == 1
    assert [target[0] for target in bundle.instances[0].targets] == [
        "arm",
        "gripper",
    ]
    assert bundle.attachments == ()


def test_separate_tool_retains_component_and_attachment_identity() -> None:
    bundle = compile_model_cell(
        sources=(COMPOSABLE_ARM, COMPOSABLE_TOOL),
        instances=(
            ModelInstanceInput(
                component_id="component.arm",
                model_instance_id="model.arm",
                asset_id=COMPOSABLE_ARM.asset_id,
                prefix="arm_",
                command_targets=(("arm", "arm"),),
            ),
            ModelInstanceInput(
                component_id="component.tool",
                model_instance_id="model.tool",
                asset_id=COMPOSABLE_TOOL.asset_id,
                prefix="tool_",
                command_targets=(("gripper", "gripper"),),
            ),
        ),
        attachments=(
            AttachmentInput(
                attachment_id="attachment.arm.tool",
                parent_model_instance_id="model.arm",
                child_model_instance_id="model.tool",
                parent_frame="tool_mount",
                child_frame="tool_root",
            ),
        ),
    )
    loaded = CompiledBundle.from_json(bundle.canonical_json)

    assert loaded == bundle
    assert {item.component_id for item in bundle.instances} == {
        "component.arm",
        "component.tool",
    }
    assert len(bundle.attachments) == 1
    attachment = bundle.attachments[0]
    assert attachment.attachment_id == "attachment.arm.tool"
    assert attachment.generated_parent_frame == "arm_tool_mount"
    assert attachment.generated_child_frame == "tool_tool_root"


@pytest.mark.parametrize("case", json.loads(
    (FIXTURES / "srdf_group_cases.experimental.json").read_text(encoding="utf-8")
)["cases"], ids=lambda case: case["name"])
def test_nested_overlapping_and_invalid_srdf_group_cases(
    case: dict[str, object],
) -> None:
    if "error" in case:
        with pytest.raises(CompilerPrototypeError, match=str(case["error"])):
            groups = parse_srdf_groups(str(case["srdf"]))
            expand_srdf_group_joints(groups, str(case["group"]))
        return

    groups = parse_srdf_groups(str(case["srdf"]))
    assert list(expand_srdf_group_joints(groups, str(case["group"]))) == case[
        "expected_joints"
    ]


@pytest.mark.parametrize(
    "instances,error",
    [
        (
            (_lite6_instance("left", "same_"), _lite6_instance("right", "same_")),
            "duplicate prefix",
        ),
        (
            (
                _lite6_instance("left", "left_"),
                ModelInstanceInput(
                    component_id="component.left",
                    model_instance_id="model.right",
                    asset_id=LITE6.asset_id,
                    prefix="right_",
                    command_targets=(("arm", "lite6"),),
                ),
            ),
            "duplicate component_id",
        ),
    ],
)
def test_identity_or_symbol_collisions_are_rejected(
    instances: tuple[ModelInstanceInput, ...],
    error: str,
) -> None:
    with pytest.raises(CompilerPrototypeError, match=error):
        compile_model_cell(sources=(LITE6,), instances=instances)


def test_retained_bundle_rejects_unknown_or_noncanonical_fields() -> None:
    bundle = compile_model_cell(
        sources=(LITE6,),
        instances=(_lite6_instance("left", "left_"),),
    )
    raw = json.loads(bundle.canonical_json)
    raw["authority_lease"] = "forbidden"
    with pytest.raises(CompilerPrototypeError, match="unknown retained bundle"):
        CompiledBundle.from_json(
            json.dumps(raw, sort_keys=True, separators=(",", ":"))
        )

    pretty = json.dumps(json.loads(bundle.canonical_json), indent=2, sort_keys=True)
    with pytest.raises(CompilerPrototypeError, match="not canonical"):
        CompiledBundle.from_json(pretty)


def _write_collision_source(
    root: Path,
    *,
    stem: str,
    link_name: str,
) -> ModelSource:
    urdf = root / f"{stem}.urdf"
    srdf = root / f"{stem}.srdf"
    urdf.write_text(
        f'<robot name="{stem}"><link name="{link_name}"/></robot>',
        encoding="utf-8",
    )
    srdf.write_text(
        (
            f'<robot name="{stem}"><group name="target">'
            f'<link name="{link_name}"/></group></robot>'
        ),
        encoding="utf-8",
    )
    return ModelSource(
        asset_id=f"asset.{stem}",
        model_name=stem,
        urdf_path=urdf,
        srdf_path=srdf,
        package_paths=(str(REPO_ROOT),),
    )


def test_true_generated_symbol_collision_is_rejected(tmp_path: Path) -> None:
    first = _write_collision_source(
        tmp_path,
        stem="collision_first",
        link_name="tip",
    )
    second = _write_collision_source(
        tmp_path,
        stem="collision_second",
        link_name="p",
    )

    with pytest.raises(CompilerPrototypeError, match="generated symbol collision"):
        compile_model_cell(
            sources=(first, second),
            instances=(
                ModelInstanceInput(
                    component_id="component.first",
                    model_instance_id="model.first",
                    asset_id=first.asset_id,
                    prefix="x_",
                    command_targets=(("target", "target"),),
                ),
                ModelInstanceInput(
                    component_id="component.second",
                    model_instance_id="model.second",
                    asset_id=second.asset_id,
                    prefix="x_ti",
                    command_targets=(("target", "target"),),
                ),
            ),
        )


def _binding_component_type() -> ComponentTypeRecord:
    trajectory = CapabilitySpec(
        capability_id=CapabilityId("capability.trajectory"),
        operation_kind="joint_trajectory",
        required_interfaces=("position",),
    )
    compliant = CapabilitySpec(
        capability_id=CapabilityId("capability.compliant"),
        operation_kind="compliant_trajectory",
        required_interfaces=("position", "velocity"),
    )
    gripper = CapabilitySpec(
        capability_id=CapabilityId("capability.gripper"),
        operation_kind="gripper_command",
        required_interfaces=("effort",),
    )
    return ComponentTypeRecord(
        component_type_id=ComponentTypeId("type.arm_tool"),
        model_asset_id=ModelAssetId("asset.arm_tool"),
        command_targets=(
            CommandTargetSpec(
                command_target_id=CommandTargetId("target.arm"),
                srdf_groups=("arm",),
                resource_ids=(ResourceId("joint.1"), ResourceId("joint.2")),
                capability_ids=(
                    trajectory.capability_id,
                    compliant.capability_id,
                ),
            ),
            CommandTargetSpec(
                command_target_id=CommandTargetId("target.gripper"),
                srdf_groups=("gripper",),
                resource_ids=(ResourceId("finger"),),
                capability_ids=(gripper.capability_id,),
            ),
        ),
        capabilities=(trajectory, compliant, gripper),
    )


def _direct_topology(
    *,
    capability_id: CapabilityId = CapabilityId("capability.trajectory"),
    claimed_resources: tuple[ResourceId, ...] = (
        ResourceId("joint.1"),
        ResourceId("joint.2"),
    ),
) -> ControllerTopologyRecord:
    return ControllerTopologyRecord(
        controller_topology_id=ControllerTopologyId("topology.direct"),
        topology_revision=ControllerTopologyRevision(1),
        controllers=(
            ControllerSpec(
                controller_id=ControllerId("jtc"),
                plugin="example/JTC",
                claimed_resources=claimed_resources,
            ),
        ),
        routes=(
            ControlRouteSpec(
                route_id=RouteId("route.trajectory"),
                capability_id=capability_id,
                controllers=(ControllerId("jtc"),),
                chained=False,
            ),
        ),
        activation_sets=((ControllerId("jtc"),),),
    )


def _interface_contract(
    *,
    resources: tuple[ResourceId, ...] = (
        ResourceId("joint.1"),
        ResourceId("joint.2"),
    ),
    command_interfaces: tuple[str, ...] = ("position",),
    state_interfaces: tuple[str, ...] = ("position", "velocity"),
) -> ControlInterfaceContract:
    return ControlInterfaceContract(
        interface_contract_revision=InterfaceContractRevision(1),
        ordered_resources=resources,
        command_interfaces=command_interfaces,
        state_interfaces=state_interfaces,
    )


def test_binding_local_capability_and_interface_compatibility_compile_exactly() -> None:
    binding, snapshot = compile_control_binding(
        component_type=_binding_component_type(),
        interface_contract=_interface_contract(),
        topology=_direct_topology(),
        controllable_component_id=ControllableComponentId("component.arm"),
        control_domain_id=ControlDomainId("domain.arm"),
        control_binding_id=ControlBindingId("binding.arm"),
        binding_revision=BindingRevision(3),
        command_target_id=CommandTargetId("target.arm"),
        capability_id=CapabilityId("capability.trajectory"),
        route_id=RouteId("route.trajectory"),
        endpoints=("/arm/jtc/follow_joint_trajectory",),
    )

    assert binding.capability_id == CapabilityId("capability.trajectory")
    assert binding.resource_ids == (
        ResourceId("joint.1"),
        ResourceId("joint.2"),
    )
    assert binding.route_id == RouteId("route.trajectory")
    assert (
        binding.interface_contract_revision
        == InterfaceContractRevision(1)
    )
    assert binding.controller_topology_id == ControllerTopologyId(
        "topology.direct"
    )
    assert binding.topology_revision == ControllerTopologyRevision(1)
    assert binding.endpoints == ("/arm/jtc/follow_joint_trajectory",)
    assert snapshot.ordered_resources == binding.resource_ids
    assert snapshot.controllable_component_id == (
        binding.controllable_component_id
    )
    assert snapshot.command_target_id == binding.command_target_id
    assert snapshot.capability_id == binding.capability_id
    assert snapshot.route_id == binding.route_id
    assert snapshot.endpoints == binding.endpoints

    model_bundle = compile_model_cell(
        sources=(COMPOSABLE_ARM,),
        instances=(
            ModelInstanceInput(
                component_id="component.arm",
                model_instance_id="model.arm",
                asset_id=COMPOSABLE_ARM.asset_id,
                prefix="arm_",
                command_targets=(("arm", "arm"),),
            ),
        ),
    )
    manifest = to_retained_manifest(
        model_bundle,
        compiler_revision=CompilerRevision("compiler-binding"),
        catalog_revision=CatalogRevision("catalog-binding"),
        graph_revision=GraphRevision(3),
        bindings=(snapshot,),
    )
    assert manifest.bindings == (snapshot,)
    assert (
        RetainedBundleManifest.from_json(manifest.to_json()).bindings
        == (snapshot,)
    )


@pytest.mark.parametrize(
    "target,capability,interfaces,topology,error",
    [
        (
            "target.arm",
            "capability.gripper",
            _interface_contract(),
            _direct_topology(),
            "not available on target",
        ),
        (
            "target.arm",
            "capability.compliant",
            _interface_contract(),
            _direct_topology(),
            "does not satisfy capability",
        ),
        (
            "target.arm",
            "capability.trajectory",
            _interface_contract(resources=(ResourceId("joint.1"),)),
            _direct_topology(),
            "target resources are absent",
        ),
        (
            "target.arm",
            "capability.trajectory",
            _interface_contract(
                command_interfaces=("effort",),
                state_interfaces=(),
            ),
            _direct_topology(),
            "cannot satisfy capability",
        ),
        (
            "target.arm",
            "capability.compliant",
            _interface_contract(
                command_interfaces=("position",),
                state_interfaces=("position", "velocity"),
            ),
            _direct_topology(
                capability_id=CapabilityId("capability.compliant")
            ),
            "command interfaces cannot satisfy",
        ),
        (
            "target.arm",
            "capability.trajectory",
            _interface_contract(
                resources=(ResourceId("joint.2"), ResourceId("joint.1")),
            ),
            _direct_topology(),
            "resource order",
        ),
    ],
)
def test_binding_local_incompatibilities_are_rejected(
    target: str,
    capability: str,
    interfaces: ControlInterfaceContract,
    topology: ControllerTopologyRecord,
    error: str,
) -> None:
    with pytest.raises(CompilerPrototypeError, match=error):
        compile_control_binding(
            component_type=_binding_component_type(),
            interface_contract=interfaces,
            topology=topology,
            controllable_component_id=ControllableComponentId("component.arm"),
            control_domain_id=ControlDomainId("domain.arm"),
            control_binding_id=ControlBindingId("binding.arm"),
            binding_revision=BindingRevision(1),
            command_target_id=CommandTargetId(target),
            capability_id=CapabilityId(capability),
            route_id=RouteId("route.trajectory"),
            endpoints=("/arm/jtc",),
        )


def _chained_topology() -> ControllerTopologyRecord:
    return ControllerTopologyRecord(
        controller_topology_id=ControllerTopologyId("topology.chained"),
        topology_revision=ControllerTopologyRevision(2),
        controllers=(
            ControllerSpec(
                controller_id=ControllerId("filter"),
                plugin="example/TrajectoryFilter",
                claimed_resources=(),
            ),
            ControllerSpec(
                controller_id=ControllerId("jtc"),
                plugin="example/JTC",
                claimed_resources=(
                    ResourceId("joint.1"),
                    ResourceId("joint.2"),
                ),
                chain_inputs=(ControllerId("filter"),),
            ),
        ),
        routes=(
            ControlRouteSpec(
                route_id=RouteId("route.chained"),
                capability_id=CapabilityId("capability.trajectory"),
                controllers=(ControllerId("filter"), ControllerId("jtc")),
                chained=True,
            ),
        ),
        activation_sets=((ControllerId("filter"), ControllerId("jtc")),),
    )


def test_chained_route_end_to_end_claims_cover_target_exactly() -> None:
    binding, snapshot = compile_control_binding(
        component_type=_binding_component_type(),
        interface_contract=_interface_contract(),
        topology=_chained_topology(),
        controllable_component_id=ControllableComponentId("component.arm"),
        control_domain_id=ControlDomainId("domain.arm"),
        control_binding_id=ControlBindingId("binding.chained"),
        binding_revision=BindingRevision(2),
        command_target_id=CommandTargetId("target.arm"),
        capability_id=CapabilityId("capability.trajectory"),
        route_id=RouteId("route.chained"),
        endpoints=("/arm/filtered_trajectory", "/arm/jtc"),
    )

    assert binding.resource_ids == (
        ResourceId("joint.1"),
        ResourceId("joint.2"),
    )
    assert binding.route_id == RouteId("route.chained")
    assert binding.topology_revision == ControllerTopologyRevision(2)
    assert snapshot.endpoints == ("/arm/filtered_trajectory", "/arm/jtc")


@pytest.mark.parametrize(
    "interfaces,topology",
    [
        (
            _interface_contract(),
            _direct_topology(
                claimed_resources=(ResourceId("joint.1"),),
            ),
        ),
        (
            _interface_contract(
                resources=(
                    ResourceId("joint.1"),
                    ResourceId("joint.2"),
                    ResourceId("joint.3"),
                ),
            ),
            _direct_topology(
                claimed_resources=(
                    ResourceId("joint.1"),
                    ResourceId("joint.2"),
                    ResourceId("joint.3"),
                ),
            ),
        ),
    ],
)
def test_selected_route_cannot_underclaim_or_overclaim_target_resources(
    interfaces: ControlInterfaceContract,
    topology: ControllerTopologyRecord,
) -> None:
    with pytest.raises(CompilerPrototypeError, match="exactly cover"):
        compile_control_binding(
            component_type=_binding_component_type(),
            interface_contract=interfaces,
            topology=topology,
            controllable_component_id=ControllableComponentId("component.arm"),
            control_domain_id=ControlDomainId("domain.arm"),
            control_binding_id=ControlBindingId("binding.arm"),
            binding_revision=BindingRevision(1),
            command_target_id=CommandTargetId("target.arm"),
            capability_id=CapabilityId("capability.trajectory"),
            route_id=RouteId("route.trajectory"),
            endpoints=("/arm/jtc",),
        )


def test_binding_rejects_controller_claim_outside_interface_contract() -> None:
    topology = ControllerTopologyRecord(
        controller_topology_id=ControllerTopologyId("topology.bad-claim"),
        topology_revision=ControllerTopologyRevision(1),
        controllers=(
            ControllerSpec(
                controller_id=ControllerId("jtc"),
                plugin="example/JTC",
                claimed_resources=(
                    ResourceId("joint.1"),
                    ResourceId("joint.2"),
                    ResourceId("joint.absent"),
                ),
            ),
        ),
        routes=(
            ControlRouteSpec(
                route_id=RouteId("route.trajectory"),
                capability_id=CapabilityId("capability.trajectory"),
                controllers=(ControllerId("jtc"),),
                chained=False,
            ),
        ),
        activation_sets=((ControllerId("jtc"),),),
    )

    with pytest.raises(CompilerPrototypeError, match="claims resources absent"):
        compile_control_binding(
            component_type=_binding_component_type(),
            interface_contract=_interface_contract(),
            topology=topology,
            controllable_component_id=ControllableComponentId("component.arm"),
            control_domain_id=ControlDomainId("domain.arm"),
            control_binding_id=ControlBindingId("binding.arm"),
            binding_revision=BindingRevision(1),
            command_target_id=CommandTargetId("target.arm"),
            capability_id=CapabilityId("capability.trajectory"),
            route_id=RouteId("route.trajectory"),
            endpoints=("/arm/jtc",),
        )


@pytest.mark.parametrize(
    "mutation,error",
    [
        (
            lambda raw: raw["instances"][0].__setitem__("prefix", ""),
            "prefix is required",
        ),
        (
            lambda raw: raw["instances"][0].__setitem__(
                "asset_fingerprint",
                7,
            ),
            "asset_fingerprint must be a string",
        ),
        (
            lambda raw: raw["instances"][0]["symbols"].__setitem__(
                "unknown",
                {},
            ),
            "compiled symbol kinds",
        ),
        (
            lambda raw: raw["instances"][0]["targets"].append(
                dict(raw["instances"][0]["targets"][0])
            ),
            "duplicate compiled command target",
        ),
    ],
)
def test_retained_reload_rejects_semantically_invalid_primitives(
    mutation,
    error: str,
) -> None:
    bundle = compile_model_cell(
        sources=(LITE6,),
        instances=(_lite6_instance("left", "left_"),),
    )
    raw = json.loads(bundle.canonical_json)
    mutation(raw)

    with pytest.raises(CompilerPrototypeError, match=error):
        CompiledBundle.from_json(
            json.dumps(raw, sort_keys=True, separators=(",", ":"))
        )


def test_retained_reload_rejects_symbol_and_attachment_tampering() -> None:
    two_instances = compile_model_cell(
        sources=(LITE6,),
        instances=(
            _lite6_instance("left", "left_"),
            _lite6_instance("right", "right_"),
        ),
    )
    raw = json.loads(two_instances.canonical_json)
    raw["instances"][1]["symbols"]["links"]["link1"] = raw["instances"][0][
        "symbols"
    ]["links"]["link1"]
    with pytest.raises(CompilerPrototypeError, match="generated symbol collision"):
        CompiledBundle.from_json(
            json.dumps(raw, sort_keys=True, separators=(",", ":"))
        )

    attached = compile_model_cell(
        sources=(COMPOSABLE_ARM, COMPOSABLE_TOOL),
        instances=(
            ModelInstanceInput(
                component_id="component.arm",
                model_instance_id="model.arm",
                asset_id=COMPOSABLE_ARM.asset_id,
                prefix="arm_",
                command_targets=(("arm", "arm"),),
            ),
            ModelInstanceInput(
                component_id="component.tool",
                model_instance_id="model.tool",
                asset_id=COMPOSABLE_TOOL.asset_id,
                prefix="tool_",
                command_targets=(("gripper", "gripper"),),
            ),
        ),
        attachments=(
            AttachmentInput(
                attachment_id="attachment.arm.tool",
                parent_model_instance_id="model.arm",
                child_model_instance_id="model.tool",
                parent_frame="tool_mount",
                child_frame="tool_root",
            ),
        ),
    )
    raw = json.loads(attached.canonical_json)
    raw["attachments"][0]["generated_child_frame"] = "forged_frame"
    with pytest.raises(
        CompilerPrototypeError,
        match="frame mapping is inconsistent",
    ):
        CompiledBundle.from_json(
            json.dumps(raw, sort_keys=True, separators=(",", ":"))
        )

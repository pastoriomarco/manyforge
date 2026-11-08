"""Unit-tagged controlled/observed-state Python projection tests."""

from __future__ import annotations

import gc

import pytest

core = pytest.importorskip("manyforge_core_py")


def _controlled_state(*, mixed: bool = False):
    resources = [core.ResourceId("joint_1"), core.ResourceId("joint_2")]
    joint_names = ["mf2_joint_1", "mf2_joint_2"]
    positions = [
        core.AngularPositionDTO(0.25),
        core.LinearPositionDTO(0.032)
        if mixed
        else core.AngularPositionDTO(-0.5),
    ]
    velocities = [
        core.AngularVelocityDTO(0.1),
        core.LinearVelocityDTO(0.001)
        if mixed
        else core.AngularVelocityDTO(0.0),
    ]
    return core.ControlledStateDTO(
        controllable_component_id=core.ControllableComponentId("robot_1"),
        command_target_id=core.CommandTargetId("arm"),
        control_domain_id=core.ControlDomainId("motion"),
        control_domain_revision=core.ControlDomainRevision("sha256:domain"),
        control_binding_id=core.ControlBindingId("arm_trajectory"),
        control_binding_revision=core.ControlBindingRevision("sha256:binding"),
        controller_topology_id=core.ControllerTopologyId("native"),
        topology_revision=core.TopologyRevision("sha256:topology"),
        control_route_id=core.ControlRouteId("native_route"),
        active_control_configuration_id=core.ActiveControlConfigurationId(
            "native_active"
        ),
        active_control_configuration_generation=core.ActiveControlConfigurationGeneration(
            7
        ),
        resources=resources,
        joint_names=joint_names,
        positions=positions,
        velocities=velocities,
    )


def _observed_state():
    state = _controlled_state()
    return core.ObservedStateSnapshotDTO(
        controllable_component_id=state.controllable_component_id,
        command_target_id=state.command_target_id,
        control_domain_id=state.control_domain_id,
        control_domain_revision=state.control_domain_revision,
        control_binding_id=state.control_binding_id,
        control_binding_revision=state.control_binding_revision,
        provider_instance_id=core.ProviderInstanceId("native_provider"),
        provider_manifest_id=core.ProviderManifestId("native_manifest"),
        provider_manifest_revision=core.ProviderManifestRevision("sha256:provider"),
        controller_topology_id=state.controller_topology_id,
        topology_revision=state.topology_revision,
        control_route_id=state.control_route_id,
        active_control_configuration_id=state.active_control_configuration_id,
        active_control_configuration_generation=state.active_control_configuration_generation,
        resources=state.resources,
        sequence=11,
        joint_names=state.joint_names,
        positions=state.positions,
        velocities=state.velocities,
        source_id="native_state",
        source_epoch=core.RuntimeEpoch("native_epoch_1"),
        source_kind=core.ObservedStateSourceKind.kPredictedState,
        source_timestamp_ns=90,
        received_timestamp_ns=100,
        state_authority_id=core.StateAuthorityId("native_predicted"),
        state_authority_class=core.StateAuthorityClass.kPredicted,
        authority_generation=4,
        freshness_budget_ns=0,
        complete=True,
    )


def test_controlled_state_preserves_units_and_all_angular_projection_is_bounded():
    angular = _controlled_state()
    assert core.validate_controlled_state(angular) is None
    assert core.angular_position_projection(angular) == [0.25, -0.5]

    mixed = _controlled_state(mixed=True)
    assert core.validate_controlled_state(mixed) is None
    assert isinstance(mixed.positions[0], core.AngularPositionDTO)
    assert isinstance(mixed.positions[1], core.LinearPositionDTO)
    with pytest.raises(
        ValueError, match="angular_projection_requires_all_angular_resources"
    ):
        core.angular_position_projection(mixed)


def test_controlled_state_validation_errors_cross_the_binding_exactly():
    mismatched = _controlled_state()
    mismatched.velocities = [
        core.LinearVelocityDTO(0.1),
        core.AngularVelocityDTO(0.0),
    ]
    with pytest.raises(ValueError, match="coordinate_unit_mismatch"):
        core.validate_controlled_state(mismatched)

    duplicate = _controlled_state()
    duplicate.resources = [core.ResourceId("joint_1"), core.ResourceId("joint_1")]
    with pytest.raises(
        ValueError, match="coordinate_identity_missing_or_duplicate"
    ):
        core.validate_controlled_state(duplicate)


def test_observed_state_enforces_authority_and_injected_freshness():
    observed = _observed_state()
    assert core.validate_observed_state_snapshot(observed, 500) is None

    observed.source_kind = core.ObservedStateSourceKind.kJointState
    observed.state_authority_class = core.StateAuthorityClass.kMeasured
    with pytest.raises(ValueError, match="measured_freshness_budget_required"):
        core.validate_observed_state_snapshot(observed, 500)

    observed.freshness_budget_ns = 100
    observed.received_timestamp_ns = 450
    assert core.validate_observed_state_snapshot(observed, 500) is None
    with pytest.raises(ValueError, match="observation_stale"):
        core.validate_observed_state_snapshot(observed, 551)


def test_state_projection_owns_values_after_python_inputs_are_released():
    resources = [core.ResourceId("joint_1"), core.ResourceId("joint_2")]
    positions = [core.AngularPositionDTO(0.25), core.AngularPositionDTO(-0.5)]
    state = _controlled_state()
    state.resources = resources
    state.positions = positions

    del resources
    del positions
    gc.collect()

    assert [resource.value for resource in state.resources] == ["joint_1", "joint_2"]
    assert core.angular_position_projection(state) == [0.25, -0.5]


def test_new_identity_counters_are_immutable_and_strongly_typed():
    generation = core.ActiveControlConfigurationGeneration(7)
    assert generation.value == 7
    assert generation.nonzero is True
    assert generation == core.ActiveControlConfigurationGeneration(7)
    assert hash(generation) == hash(core.ActiveControlConfigurationGeneration(7))
    with pytest.raises(AttributeError):
        generation.value = 8
    with pytest.raises(TypeError):
        core.ActiveControlConfigurationGeneration()

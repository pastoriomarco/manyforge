"""L0.2 lifecycle DTO and closed-presence binding parity."""

from __future__ import annotations

import pytest

core = pytest.importorskip("manyforge_core_py")

_MANIFEST_REVISION = "sha256:" + ("1" * 64)
_SCENE_ID = "sha256:" + ("2" * 64)
_SCENE_CONTENT_DIGEST = "sha256:" + ("3" * 64)


def test_provider_domain_scope_has_real_driver_identity_without_motion_fields():
    provider = core.ProviderControlDomainDTO(
        core.ControllableComponentId("device.io"), core.RuntimeUnitId("host.io"),
        core.ProviderInstanceId("driver.io"), core.ProviderManifestId("manyforge.provider.io_signals"),
        core.ProviderManifestRevision(_MANIFEST_REVISION), core.ContentDigest(_SCENE_CONTENT_DIGEST),
        core.ContentDigest(_SCENE_ID), ["ready"], 30_000_000_000,
    )
    assert provider.locally_valid()
    domain = core.ControlDomainDTO(
        core.ControlDomainId("domain.io"), core.ControlDomainRevision(_MANIFEST_REVISION),
        [core.RuntimeUnitId("host.io")], core.EndpointRoleId("io.driver"),
        core.ContainmentGranularity.kControlDomain, provider,
    )
    expected = core.ExpectedControlDomainRevisionDTO(
        domain.control_domain_id, domain.control_domain_revision,
        core.ExpectedProviderDomainScopeDTO(provider, _expected_configuration()),
    )
    actual = core.ResolvedLifecycleControlDomainScopeDTO(
        domain.control_domain_id, domain.control_domain_revision,
        core.ResolvedProviderDomainScopeDTO(provider, core.RuntimeEpoch("host.epoch")),
    )
    assert expected.locally_valid() and actual.locally_valid()
    assert domain.provider == provider
    assert actual.provider.provider == provider
    assert actual.provider.active_control_configuration is None
    assert actual.provider.output_resources == ()
    assert expected.provider.provider.lifecycle_command_timeout_ns == 30_000_000_000
    proof = core.ProviderConfigurationFenceOccurrenceDTO(
        provider.controllable_component_id, provider.provider_instance_id,
        provider.signal_configuration_digest,
    )
    fenced = core.ConfigurationFenceDomainScopeDTO(
        domain.control_domain_id, None, domain.control_domain_revision,
        core.ProviderConfigurationFenceDomainDTO(candidate_provider=proof),
    )
    scope = core.ConfigurationFenceScopeDTO([provider.controllable_component_id], [fenced], [], [])
    assert scope.valid()
    assert fenced.provider.candidate_provider == proof
    capability_target = core.ControlDomainCapabilityProviderTargetDTO(
        domain.control_domain_id, domain.control_domain_revision,
        core.ProviderControlDomainCapabilityTargetDTO(),
    )
    assert capability_target.is_provider
    with pytest.raises(RuntimeError):
        _ = domain.controller_topology_id


def test_program_node_admission_and_no_admission_result_are_bound():
    evidence = core.ProgramNodeAdmissionEvidenceDTO(
        core.RequestOwnerEpoch("runner.1"), core.ProgramRunId("run.1"),
        core.CellAdmissionClaimGeneration(7), "enable", "enable.1", "cclc.lifecycle")
    assert evidence.locally_valid()
    assert evidence.parent_program_cycle_claim_generation.value == 7
    assert core.AdmissionGeneration(9).value == 9
    rejected = core.CCLCLifecycleRequestResultDTO(
        core.RequestReplayStatusDTO.kReplayedExact,
        rejection=core.ErrorDTO("stale_generation", "Changed", {"effect": "not_started"}))
    assert rejected.locally_valid()
    assert rejected.acceptance is None and rejected.status is None
    assert rejected.rejection.details["effect"] == "not_started"


def _expected_configuration(*, present: bool = False):
    if not present:
        return core.ExpectedActiveControlConfigurationDTO(
            core.ExpectedActiveControlConfigurationPresenceDTO.kAbsent
        )
    return core.ExpectedActiveControlConfigurationDTO(
        core.ExpectedActiveControlConfigurationPresenceDTO.kPresent,
        core.ActiveControlConfigurationId("configuration-1"),
        core.ActiveControlConfigurationGeneration(2),
    )


def test_route_intent_preserves_exact_node_evidence_and_legacy_absence():
    context = core.PublicRequestContextDTO(core.RequestId("route.1"), core.RequestOwnerEpoch("cclc.1"), "test")
    desired = core.DesiredControlConfigurationDTO(
        core.ControlDomainId("domain.1"), core.ControlDomainRevision(_MANIFEST_REVISION),
        core.ControllerTopologyId("topology.1"), core.TopologyRevision(_MANIFEST_REVISION),
        core.ActivationSetId("activation.1"),
        [core.DesiredControlBindingRouteDTO(core.ControlBindingId("binding.1"),
            core.ControlBindingRevision(_MANIFEST_REVISION), core.ControlRouteId("route.1"))])
    components = [core.RouteSelectionComponentExpectationDTO(core.ControllableComponentId("arm"),
        core.ComponentLifecycleGeneration(4), core.ComponentLifecycleStateDTO.kPrepared)]
    args = (context, core.RetainedBundleId("bundle.1"), core.GraphRevision("graph.1"),
            desired, _expected_configuration(), components, core.RequestAttributionDTO())
    legacy = core.RouteSelectionIntentDTO(*args)
    assert legacy.locally_valid() and legacy.expected_admission_generation is None
    node = core.ProgramNodeAdmissionEvidenceDTO(core.RequestOwnerEpoch("runner.1"), core.ProgramRunId("run.1"),
        core.CellAdmissionClaimGeneration(7), "route", "route.1", "cclc.control_route")
    current = core.RouteSelectionIntentDTO(*args, core.AdmissionGeneration(9), node)
    assert current.locally_valid() and current.program_node_evidence == node
    assert isinstance(current.component_expectations, tuple)
    assert not current.replay_equal(legacy)
    assert not core.RouteSelectionIntentDTO(*args, None, node).locally_valid()
    with pytest.raises(AttributeError):
        current.expected_admission_generation = core.AdmissionGeneration(10)


def _expected_domain():
    return core.ExpectedControlDomainRevisionDTO(
        core.ControlDomainId("domain-1"),
        core.ControlDomainRevision("domain-revision-1"),
        core.ControllerTopologyId("topology-1"),
        core.TopologyRevision("topology-revision-1"),
        _expected_configuration(),
        (
            core.ExpectedControlBindingRevisionDTO(
                core.ControlBindingId("binding-1"),
                core.ControlBindingRevision("binding-revision-1"),
                core.ControlRouteId("shared-route"),
            ),
            core.ExpectedControlBindingRevisionDTO(
                core.ControlBindingId("binding-2"),
                core.ControlBindingRevision("binding-revision-1"),
                core.ControlRouteId("shared-route"),
            ),
        ),
    )


def _primitive_intent():
    return core.BrandLifecyclePrimitiveIntentDTO(
        core.LifecycleTransactionId("transaction-1"),
        core.LifecycleTransactionGeneration(1),
        core.ControllableComponentId("component-1"),
        core.ControlDomainId("domain-1"),
        core.ControlDomainRevision("domain-revision-1"),
        core.ProviderInstanceId("provider-1"),
        core.ProviderManifestId("manifest-1"),
        core.ProviderManifestRevision("manifest-revision-1"),
        core.RuntimeUnitId("runtime-1"),
        core.RuntimeEpoch("runtime-epoch-1"),
        core.LifecyclePrimitiveId("enable-control"),
        core.LifecyclePrimitiveAttemptId("attempt-1"),
        4,
        1_000_000,
    )


def _predicate(*states, fact=None):
    predicates = ()
    if fact is not None:
        predicates = (
            core.LifecycleFactPredicateDTO(
                core.LifecycleFactId(fact),
                core.LifecycleFactValueDTO(
                    core.LifecycleFactValueKindDTO.kBoolean,
                    boolean_value=True,
                ),
            ),
        )
    return core.LifecyclePredicateDTO(states, predicates)


def _publication_receipt():
    projection = core.ProjectionRevisionDTO(
        core.RetainedBundleId("bundle-1"),
        core.GraphRevision("graph-revision-1"),
        core.SceneRevision(
            core.SceneId(_SCENE_ID),
            1,
            core.ContentDigest(_SCENE_CONTENT_DIGEST),
        ),
    )
    request = core.ActiveBundlePublicationRequestDTO(
        core.BootstrapPublicationTransactionDTO(
            core.BootstrapId("bootstrap-1"),
            core.BootstrapGeneration(1),
        ),
        projection,
        projection.retained_bundle_id,
        projection.graph_revision,
        None,
        core.ContentDigest("private-materialization-1"),
    )
    return core.ActiveBundlePublicationReceiptDTO(
        request,
        core.ActiveBundlePublicationGeneration(1),
        core.ContentDigest("public-pointer-1"),
    )


def _named_apply_publication_request():
    projection = core.ProjectionRevisionDTO(
        core.RetainedBundleId("bundle-2"),
        core.GraphRevision("graph-revision-2"),
        core.SceneRevision(
            core.SceneId(_SCENE_ID),
            2,
            core.ContentDigest(_SCENE_CONTENT_DIGEST),
        ),
    )
    return core.ActiveBundlePublicationRequestDTO(
        core.NamedApplyPublicationTransactionDTO(
            core.ApplyId("apply-1"),
            core.ApplyGeneration(2),
        ),
        projection,
        projection.retained_bundle_id,
        projection.graph_revision,
        core.ActiveBundlePublicationGeneration(1),
        core.ContentDigest("private-materialization-2"),
    )


def _resolved_scope():
    expected = _expected_domain()
    return core.ResolvedLifecycleControlDomainScopeDTO(
        expected.control_domain_id,
        expected.control_domain_revision,
        expected.controller_topology_id,
        expected.topology_revision,
        None,
        expected.expected_control_bindings,
        (core.ResourceId("resource-1"),),
    )


def _provider_occurrence():
    return core.ResolvedLifecycleProviderOccurrenceDTO(
        core.ControlDomainId("domain-1"),
        core.ControlDomainRevision("domain-revision-1"),
        core.ProviderInstanceId("provider-1"),
        core.ProviderManifestId("manifest-1"),
        core.ProviderManifestRevision(_MANIFEST_REVISION),
        core.RuntimeUnitId("runtime-1"),
        core.RuntimeEpoch("runtime-epoch-1"),
        core.ContentDigest("lifecycle-catalog-1"),
    )


def test_first_prepare_absence_and_shared_route_are_representable():
    expected = _expected_domain()
    assert expected.locally_valid()
    assert expected.expected_active_control_configuration.locally_valid()
    assert expected.expected_active_control_configuration.presence == (
        core.ExpectedActiveControlConfigurationPresenceDTO.kAbsent
    )
    assert tuple(row.control_route_id.value for row in expected.expected_control_bindings) == (
        "shared-route",
        "shared-route",
    )


def test_lifecycle_intent_is_immutable_and_locally_valid():
    request_context = core.PublicRequestContextDTO(
        core.RequestId("request-1"),
        core.RequestOwnerEpoch("owner-epoch-1"),
        "pytest",
    )
    lifecycle_intent = core.ComponentLifecycleIntentDTO(
        request_context,
        core.ControllableComponentId("component-1"),
        core.ComponentLifecycleIntentKindDTO.kPrepare,
        core.GraphRevision("graph-revision-1"),
        core.ComponentLifecycleGeneration(1),
        (_expected_domain(),),
    )

    assert lifecycle_intent.locally_valid()
    assert lifecycle_intent.expected_control_domains[0].control_domain_id.value == "domain-1"
    with pytest.raises(AttributeError):
        lifecycle_intent.intent = core.ComponentLifecycleIntentKindDTO.kDisable


def test_fact_union_rejects_cross_alternative_fields():
    unknown = core.LifecycleFactValueDTO(core.LifecycleFactValueKindDTO.kUnknown)
    token = core.LifecycleFactValueDTO(
        core.LifecycleFactValueKindDTO.kToken,
        token_value="automatic.ready",
    )
    invalid = core.LifecycleFactValueDTO(
        core.LifecycleFactValueKindDTO.kUnknown,
        boolean_value=False,
    )

    assert unknown.locally_valid()
    assert token.locally_valid()
    assert not invalid.locally_valid()


def test_primitive_result_distinguishes_refusal_from_uncertain_dispatch():
    primitive = _primitive_intent()
    refused = core.BrandLifecyclePrimitiveResultDTO(
        primitive.lifecycle_transaction_id,
        primitive.lifecycle_transaction_generation,
        primitive.controllable_component_id,
        primitive.control_domain_id,
        primitive.control_domain_revision,
        primitive.provider_instance_id,
        primitive.provider_manifest_id,
        primitive.provider_manifest_revision,
        primitive.provider_runtime_unit_id,
        primitive.provider_runtime_epoch,
        primitive.lifecycle_primitive_id,
        primitive.lifecycle_primitive_attempt_id,
        primitive.expected_observation_generation,
        core.LifecyclePrimitiveDispatchStateDTO.kNotDispatched,
        core.LifecyclePrimitiveEffectDTO.kNone,
        error=core.ErrorDTO("primitive_refused", "not accepted", {}),
    )
    uncertain = core.BrandLifecyclePrimitiveResultDTO(
        primitive.lifecycle_transaction_id,
        primitive.lifecycle_transaction_generation,
        primitive.controllable_component_id,
        primitive.control_domain_id,
        primitive.control_domain_revision,
        primitive.provider_instance_id,
        primitive.provider_manifest_id,
        primitive.provider_manifest_revision,
        primitive.provider_runtime_unit_id,
        primitive.provider_runtime_epoch,
        primitive.lifecycle_primitive_id,
        primitive.lifecycle_primitive_attempt_id,
        primitive.expected_observation_generation,
        core.LifecyclePrimitiveDispatchStateDTO.kDispatchedUncertain,
        core.LifecyclePrimitiveEffectDTO.kPossible,
        error=core.ErrorDTO("dispatch_uncertain", "response lost", {}),
    )

    assert refused.locally_valid()
    assert uncertain.locally_valid()
    assert refused.matches(primitive)
    assert uncertain.matches(primitive)


def test_stop_certificate_reference_is_compact_and_exact():
    reference = core.StopCertificateRefDTO(
        core.StopCertificateId("certificate-1"),
        core.StopCertificateRevision(1),
        core.StopId("stop-1"),
        core.StopGeneration(2),
    )

    assert reference.locally_valid()
    assert reference.stop_id.value == "stop-1"


def test_lifecycle_plan_catalog_is_closed_immutable_and_locally_valid():
    eligible = _predicate(
        core.ComponentLifecycleStateDTO.kPrepared,
        fact="automatic.ready",
    )
    active = _predicate(core.ComponentLifecycleStateDTO.kActive)
    transition = core.LifecyclePrimitiveTransitionDTO(
        core.LifecyclePrimitiveId("enable-control"),
        eligible,
        active,
    )
    plan = core.LifecycleIntentPlanDTO(
        core.ComponentLifecycleIntentKindDTO.kEnable,
        core.ComponentLifecycleStateDTO.kActive,
        None,
        active,
        1_000_000,
        (transition,),
    )
    catalog = core.make_brand_lifecycle_plan_catalog(
        core.ProviderManifestId("manifest-1"),
        core.ProviderManifestRevision(_MANIFEST_REVISION),
        (plan,),
    )
    unknown_fact = core.LifecycleFactPredicateDTO(
        core.LifecycleFactId("automatic.ready"),
        core.LifecycleFactValueDTO(core.LifecycleFactValueKindDTO.kUnknown),
    )

    assert eligible.locally_valid()
    assert transition.locally_valid()
    assert plan.locally_valid()
    assert catalog.locally_valid()
    assert catalog.catalog_digest.value.startswith("sha256:")
    encoded = core.encode_brand_lifecycle_plan_catalog(catalog)
    assert f'"catalog_digest":"{catalog.catalog_digest.value}"' in encoded
    assert '"api_version":"manyforge.brand_lifecycle_plan_catalog.v1"' in encoded
    assert isinstance(catalog.plans, tuple)
    assert not unknown_fact.locally_valid()
    with pytest.raises(AttributeError):
        catalog.catalog_digest = core.ContentDigest("replacement")


def test_current_component_snapshot_projects_exact_immutable_owner_facts():
    receipt = _publication_receipt()
    named_apply_request = _named_apply_publication_request()
    snapshot = core.ComponentLifecycleCurrentSnapshotDTO(
        core.ContentDigest("component-snapshot-1"),
        core.ControllableComponentId("component-1"),
        core.ComponentLifecycleGeneration(2),
        core.ComponentLifecycleStateDTO.kActive,
        receipt,
        (_resolved_scope(),),
        (_provider_occurrence(),),
    )

    assert receipt.valid()
    assert named_apply_request.valid()
    assert isinstance(
        receipt.request.transaction_ref,
        core.BootstrapPublicationTransactionDTO,
    )
    assert isinstance(
        named_apply_request.transaction_ref,
        core.NamedApplyPublicationTransactionDTO,
    )
    assert snapshot.locally_valid()
    assert snapshot.active_publication_receipt.request.candidate_retained_bundle_id.value == (
        "bundle-1"
    )
    assert isinstance(snapshot.domains, tuple)
    assert isinstance(snapshot.provider_occurrences, tuple)
    with pytest.raises(AttributeError):
        snapshot.component_lifecycle_state = core.ComponentLifecycleStateDTO.kFaulted


def test_idle_disable_terminal_status_carries_no_stop_handoff():
    public_context = core.PublicRequestContextDTO(
        core.RequestId("public-request-1"),
        core.RequestOwnerEpoch("public-owner-1"),
        "pytest",
    )
    intent = core.ComponentLifecycleIntentDTO(
        public_context,
        core.ControllableComponentId("component-1"),
        core.ComponentLifecycleIntentKindDTO.kDisable,
        core.GraphRevision("graph-revision-1"),
        core.ComponentLifecycleGeneration(1),
        (_expected_domain(),),
    )
    acceptance = core.ComponentLifecycleAcceptedDTO(
        public_context.request_id,
        core.RequestReplayStatusDTO.kAcceptedNew,
        core.LifecycleTransactionId("lifecycle-transaction-1"),
        core.LifecycleTransactionGeneration(1),
        core.ComponentLifecycleGeneration(2),
        100,
        1_000_000,
    )
    requested_state = core.ComponentLifecycleRequestedStateDTO(
        core.ComponentLifecycleRequestedStatePresenceDTO.kResolved,
        core.ComponentLifecycleStateDTO.kDisabled,
    )
    domain = core.ComponentLifecycleDomainStatusDTO(
        control_domain_id=core.ControlDomainId("domain-1"),
        expected_domain=_expected_domain(),
        current_scope=_resolved_scope(),
        resolved_provider_occurrence=_provider_occurrence(),
        previous_state=core.ComponentLifecycleStateDTO.kActive,
        current_state=core.ComponentLifecycleStateDTO.kDisabled,
        requested_state=requested_state,
        action_state=core.ComponentLifecycleDomainActionStateDTO.kSucceeded,
        effect=core.ComponentLifecycleDomainEffectDTO.kKnownNoEffect,
        primitive_attempts=(),
        reconciliation_evidence=None,
        error=None,
    )
    status = core.ComponentLifecycleStatusDTO(
        acceptance=acceptance,
        accepted_intent=intent,
        status_sequence=3,
        status_owner_epoch=public_context.request_owner_epoch,
        previous_state=core.ComponentLifecycleStateDTO.kActive,
        current_state=core.ComponentLifecycleStateDTO.kDisabled,
        requested_state=requested_state,
        phase=core.ComponentLifecycleTransactionPhaseDTO.kSucceeded,
        domains=(domain,),
        reconciliation_required=False,
        terminal_result=core.ComponentLifecycleTerminalResultDTO.kSucceeded,
        error=None,
        accepted_at_ns=100,
        latest_updated_at_ns=200,
        remaining_duration_ns=0,
    )

    assert status.locally_valid()
    assert domain.resolved_provider_occurrence == _provider_occurrence()
    for removed in ("configuration_fence", "disable_handoff", "disable_fence_released"):
        assert not hasattr(status, removed)
    for removed in ("DisableHandoffRequestDTO", "DisableFenceReleasedDTO", "ConfigurationFenceHandleDTO"):
        assert not hasattr(core, removed)

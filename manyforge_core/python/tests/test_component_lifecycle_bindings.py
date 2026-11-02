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
    actual = core.ResolvedLifecycleControlDomainScopeDTO(
        domain.control_domain_id, domain.control_domain_revision,
        core.ResolvedProviderDomainScopeDTO(provider, core.RuntimeEpoch("host.epoch")),
    )
    assert actual.locally_valid()
    assert domain.provider == provider
    assert actual.provider.provider == provider
    assert actual.provider.active_control_configuration is None
    assert actual.provider.output_resources == ()
    assert actual.provider.provider.lifecycle_command_timeout_ns == 30_000_000_000
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


def test_program_node_admission_evidence_is_bound():
    evidence = core.ProgramNodeAdmissionEvidenceDTO(
        core.RequestOwnerEpoch("runner.1"), core.ProgramRunId("run.1"),
        core.CellAdmissionClaimGeneration(7), "enable", "enable.1", "cclc.lifecycle")
    assert evidence.locally_valid()
    assert evidence.parent_program_cycle_claim_generation.value == 7
    assert core.AdmissionGeneration(9).value == 9


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
    binding = core.ExpectedControlBindingRevisionDTO(
        core.ControlBindingId("binding-1"),
        core.ControlBindingRevision("binding-revision-1"),
        core.ControlRouteId("shared-route"),
    )
    return core.ResolvedLifecycleControlDomainScopeDTO(
        core.ControlDomainId("domain-1"),
        core.ControlDomainRevision("domain-revision-1"),
        core.ControllerTopologyId("topology-1"),
        core.TopologyRevision("topology-revision-1"),
        None,
        (binding,),
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


def test_stop_certificate_reference_is_compact_and_exact():
    reference = core.StopCertificateRefDTO(
        core.StopCertificateId("certificate-1"),
        core.StopCertificateRevision(1),
        core.StopId("stop-1"),
        core.StopGeneration(2),
    )

    assert reference.locally_valid()
    assert reference.stop_id.value == "stop-1"


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

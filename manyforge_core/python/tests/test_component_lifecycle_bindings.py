"""Lifecycle DTO and closed-presence binding parity."""

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
        core.ProgramRunNumber(1),
        core.CellAdmissionClaimGeneration(7), "enable", core.NodeAttemptNumber(1),
        "scene.transaction")
    assert evidence.locally_valid()
    assert evidence.node_attempt_id.value == 1
    assert evidence.parent_program_cycle_claim_generation.value == 7
    assert core.AdmissionGeneration(9).value == 9
    refused = core.ProgramNodeAdmissionEvidenceDTO(
        core.ProgramRunNumber(1),
        core.CellAdmissionClaimGeneration(7), "enable", core.NodeAttemptNumber(1),
        "cclc.lifecycle")
    assert not refused.locally_valid()


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
            core.ApplyId(1),
            core.ApplyGeneration(2),
        ),
        projection,
        projection.retained_bundle_id,
        projection.graph_revision,
        core.ActiveBundlePublicationGeneration(1),
        core.ContentDigest("private-materialization-2"),
    )


def test_publication_receipt_keeps_its_transaction_kind():
    receipt = _publication_receipt()
    named_apply_request = _named_apply_publication_request()

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

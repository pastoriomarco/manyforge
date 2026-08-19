"""Day-zero smoke and negative coverage for runtime contract manifests."""

from __future__ import annotations

import pytest

core = pytest.importorskip("manyforge_core_py")


def _capability(capability_id: str) -> object:
    row = core.find_builtin_capability_definition(capability_id)
    assert row is not None
    return core.RuntimeCapabilityContractRefDTO(
        core.CapabilityId(capability_id),
        core.CapabilityDefinitionRevision(row.capability_definition_revision),
    )


def _offer() -> object:
    return core.make_runtime_contract_offer(
        core.RuntimeImplementationFamilyId("native-family"),
        [
            core.ApiContractId("api.state"),
            core.ApiContractId("api.motion"),
        ],
        [
            _capability("state.observed.current"),
            _capability("planning.motion.joint_goal"),
        ],
    )


def test_manifest_bindings_make_canonical_round_trip_and_compatibility():
    offer = _offer()
    assert list(offer.provided_api_contract_ids) == [
        core.ApiContractId("api.motion"),
        core.ApiContractId("api.state"),
    ]
    assert offer.offer_digest.value == (
        "sha256:63a4994aebb34bfa8488c9eb0ff24a642e28105be1e28024042eec71e2504c9b"
    )
    encoded = core.encode_runtime_contract_offer(offer)
    assert core.decode_runtime_contract_offer(encoded) == offer

    requirement = core.make_runtime_contract_requirement(
        core.RuntimeImplementationFamilyId("native-family"),
        [core.ApiContractId("api.motion")],
        [_capability("planning.motion.joint_goal")],
    )
    assert requirement.requirement_digest.value == (
        "sha256:23f6b72cdf22ecbe419dca3c1a40a2b1e2059a5ab5fdacde3945cb0cb7c7f623"
    )
    core.check_runtime_contract_compatibility(offer, requirement)

    manifest_set = core.make_runtime_contract_manifest_set([offer])
    assert manifest_set.manifest_set_digest.value == (
        "sha256:04824092c00fb2f0e3a3009919f3c6b4a8a337c8e79fda97593e29a09cb3af46"
    )
    assert core.decode_runtime_contract_manifest_set(
        core.encode_runtime_contract_manifest_set(manifest_set)
    ) == manifest_set


def test_manifest_bindings_reject_negative_contract_inputs():
    with pytest.raises(ValueError):
        core.make_runtime_contract_offer(
            core.RuntimeImplementationFamilyId("not/portable"), [], []
        )

    offer = _offer()
    tampered = core.RuntimeContractOfferDTO(
        core.RuntimeContractOfferDigest("sha256:" + "f" * 64),
        offer.runtime_implementation_id,
        list(offer.provided_api_contract_ids),
        list(offer.provided_capabilities),
    )
    with pytest.raises(ValueError):
        core.verify_runtime_contract_offer(tampered)

    with pytest.raises(ValueError):
        core.decode_runtime_contract_offer(
            '{"api_version":"manyforge.runtime_contract_offer.v2",'
            '"offer_digest":"sha256:' + "0" * 64 + '",'
            '"provided_api_contract_ids":[],"provided_capabilities":[],'
            '"runtime_implementation_id":"native-family"}'
        )

    missing = core.make_runtime_contract_requirement(
        core.RuntimeImplementationFamilyId("native-family"),
        [core.ApiContractId("api.missing")],
        [],
    )
    with pytest.raises(ValueError):
        core.check_runtime_contract_compatibility(offer, missing)

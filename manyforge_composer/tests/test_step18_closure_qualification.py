"""Disposition contract for the historical Step-18 closure campaign.

Historical source: ``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1``
blob ``75d8006c9b8dd34c2ea96b2f5fd25e5ea161a3cf``.  Its 20 cases bind the
old loaded-Kernel, release, and Isaac-live evidence formats.  They return only
after the I8/P5 and I9/I producers exist; the old verifier is not a current
qualification surface.
"""

from typing import Final


HISTORICAL_CASE_COUNT: Final = 20
HISTORICAL_CASE_FAMILIES: Final = (
    (
        "common_candidate_and_reference_parity",
        "historical lines 1010-1290",
        3,
        ("I8", "P5", "I9", "I"),
        "complete common candidate, precise-reference parity, and the sole "
        "allowed evidence-record specs delta",
    ),
    (
        "producer_artifact_and_verifier_identity",
        "historical lines 1313-1387",
        6,
        ("I8", "P5", "I9", "I"),
        "producer-summary authenticity, retained artifacts, one campaign specs "
        "identity, image parity, and exact subgate/verifier hashes",
    ),
    (
        "invocation_fixture_and_toctou_fencing",
        "historical lines 1412-1470",
        5,
        ("I8", "P5", "I9", "I"),
        "canonical record path, immutable producer invocation, same live "
        "fixture/mechanism owner, retained-input and source TOCTOU rejection",
    ),
    (
        "ignored_output_and_atomic_publication",
        "historical lines 1486-1556",
        4,
        ("I8", "P5", "I9", "I"),
        "ignored evidence root, failed-postwrite cleanup, unignored-output "
        "rejection, and postwrite binding recheck",
    ),
    (
        "obsolete_field_and_distance_contract_hygiene",
        "historical lines 1575-1628",
        2,
        ("I8", "P5"),
        "removed production fields stay absent and distance-capability "
        "semantics cannot regress",
    ),
)


def test_step18_closure_cases_are_fully_assigned_to_future_gates() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 20
    assert len({family[0] for family in HISTORICAL_CASE_FAMILIES}) == 5

"""Disposition contract for the historical Composer visual worker.

Historical blob ``8f49e474dd0e5d8298ab3dc5c037cbcb1befd48c`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains six cases.
Payload-neutral envelope integrity was extracted to
``test_viewer_transport_primitives.py``.  Process supervision, dynamic FK, and
attachment-following return only with the I9/I observer worker.
"""

from typing import Final

from manyforge_composer.backend.viewer_worker import (
    VIEWER_WORKER_RESTORATION_GATE,
)


HISTORICAL_CASE_FAMILIES: Final = (
    (
        "bounded_digest_checked_envelope",
        "historical line 32",
        1,
        (),
        "retained in test_viewer_transport_primitives.py without the old wire",
    ),
    (
        "dynamic_fk_dependency_boundary",
        "historical line 56",
        1,
        ("I9", "I"),
        "missing FK dependency is reported at the observer worker boundary",
    ),
    (
        "worker_timeout_crash_and_stop_containment",
        "historical lines 84-118",
        3,
        ("I9", "I"),
        "bounded timeout, process-exit classification, and stop-interrupted wait",
    ),
    (
        "attachment_following_restart_and_worker_policy",
        "historical line 201",
        1,
        ("I9", "I"),
        "fresh worker epoch, attachment following, and qualified NRT policy",
    ),
)


def test_viewer_worker_cases_are_extracted_or_assigned_to_i9() -> None:
    assert VIEWER_WORKER_RESTORATION_GATE == "I9"
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 6
    assert [family[0] for family in HISTORICAL_CASE_FAMILIES if not family[3]] == [
        "bounded_digest_checked_envelope"
    ]

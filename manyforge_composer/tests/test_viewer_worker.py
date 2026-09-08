"""Disposition contract for the historical Composer visual worker.

Historical blob ``8f49e474dd0e5d8298ab3dc5c037cbcb1befd48c`` at
``manyforge@08222188293df8e12f90dc72e204c21f22f92ea1`` contains six cases.
Payload-neutral envelope integrity was extracted to
``test_viewer_transport_primitives.py``. The current native observation stream
owns delivery; no Composer FK worker or process supervisor is to be restored.
"""

from pathlib import Path
from typing import Final


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
        (),
        "native source/model failure remains unavailable through "
        "test_source_model_client.py and test_execution_observation_stream.py",
    ),
    (
        "worker_timeout_crash_and_stop_containment",
        "historical lines 84-118",
        3,
        (),
        "bounded disconnect, consumer teardown and current-only restart through "
        "test_execution_observation_stream.py; no worker cancellation owner",
    ),
    (
        "attachment_following_restart_and_worker_policy",
        "historical line 201",
        1,
        ("UI6-I",),
        "epoch/current-only delivery uses test_execution_observation_stream.py; "
        "attachment and interrupted Scene joins remain UI6-I, not a future worker",
    ),
)


def test_viewer_worker_cases_have_current_successors_or_exact_scene_join() -> None:
    assert sum(family[2] for family in HISTORICAL_CASE_FAMILIES) == 6
    assert [family[0] for family in HISTORICAL_CASE_FAMILIES if not family[3]] == [
        "bounded_digest_checked_envelope",
        "dynamic_fk_dependency_boundary",
        "worker_timeout_crash_and_stop_containment",
    ]
    for successor in (
        "test_viewer_transport_primitives.py",
        "test_source_model_client.py",
        "test_execution_observation_stream.py",
    ):
        assert (Path(__file__).parent / successor).is_file()

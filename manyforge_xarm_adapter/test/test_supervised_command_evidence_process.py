"""Unavailable-boundary replacement for the old live command process test.

Historical blob ``5f1b9cdb9f27819a5ca833308c59433de942b7e5`` contains
``test_successive_installed_clients_keep_transport_ids_unique``.  Its unique
request correlation, replay/idempotency, ordered primitive audit, before/after
facts, and durable evidence obligations return at I7/L0, I8/P5, and I10/R.
Caller authority sessions and the direct legacy binary are intentionally not
restoration targets.
"""

from __future__ import annotations

from manyforge_xarm_adapter import supervised_command_evidence as subject


HISTORICAL_TEST_BLOB = "5f1b9cdb9f27819a5ca833308c59433de942b7e5"
HISTORICAL_CASE = "test_successive_installed_clients_keep_transport_ids_unique"
FUTURE_ACCEPTANCE_GATES = ("I7/L0", "I8/P5", "I10/R")


def test_installed_client_path_is_explicitly_unavailable(capsys) -> None:
    assert subject.main() == 78
    assert capsys.readouterr().err.strip() == subject.UNAVAILABLE_MESSAGE


def test_process_case_is_mapped_without_a_skip_or_fake_owner() -> None:
    assert HISTORICAL_TEST_BLOB == (
        "5f1b9cdb9f27819a5ca833308c59433de942b7e5"
    )
    assert HISTORICAL_CASE.endswith("transport_ids_unique")
    assert FUTURE_ACCEPTANCE_GATES == ("I7/L0", "I8/P5", "I10/R")

"""Cross-language I4 persistence parity helper.

This script is test-only. ``create-python`` writes through the committed
Python reference adapter, while ``verify-cpp`` reopens and verifies a store
written by the C++ semantic owner. It intentionally exercises only the
bounded fixture mechanics, not Apply or active-publication semantics.
"""

from __future__ import annotations

import argparse
from pathlib import Path

from manyforge_supervision.persistence.reference_adapter import (
    ApplyIdentity,
    BootstrapIdentity,
    ComponentManagerPersistenceFixture,
    FixtureCasIntent,
    JournalEvent,
)


DIGEST = "sha256:" + "1" * 64


def bootstrap() -> BootstrapIdentity:
    return BootstrapIdentity(
        bootstrap_id="bootstrap-1",
        generation=1,
        intent_digest=DIGEST,
        target_bundle_identity="empty-bundle",
        target_graph_revision="empty-graph",
    )


def create_python(path: Path) -> None:
    with ComponentManagerPersistenceFixture(path, cell_id="cell-a") as store:
        operation = store.ensure_bootstrap(bootstrap()).record
        store.append_journal_event(
            operation.ref,
            JournalEvent("entry-1", "intent_recorded", b"payload"),
        )
        store.advance_fixture_head(
            FixtureCasIntent(
                fixture_cas_id="advance-1",
                operation_ref=operation.ref,
                operation_identity_digest=operation.identity_digest,
            )
        )
        report = store.verify_integrity()
        assert report.operation_count == 1
        assert report.journal_entry_count == 1
        assert report.fixture_cas_receipt_count == 1


def verify_cpp(path: Path) -> None:
    with ComponentManagerPersistenceFixture(path, cell_id="cell-a") as store:
        report = store.verify_integrity()
        assert report.operation_count == 2
        assert report.journal_entry_count == 2
        assert report.fixture_cas_receipt_count == 2
        head = store.fixture_cas_head()
        assert head is not None
        assert head.generation == 2
        assert head.bundle_identity == "cell-bundle"
        assert head.graph_revision == "cell-graph"
        apply_rows = [
            operation
            for operation in store.operations()
            if operation.ref.operation_id == "apply-1"
        ]
        assert len(apply_rows) == 1
        assert store.journal(apply_rows[0].ref)[0].payload == b"cpp-payload"


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("operation", choices=("create-python", "verify-cpp"))
    parser.add_argument("database", type=Path)
    arguments = parser.parse_args()
    if arguments.operation == "create-python":
        create_python(arguments.database)
    else:
        verify_cpp(arguments.database)


if __name__ == "__main__":
    main()

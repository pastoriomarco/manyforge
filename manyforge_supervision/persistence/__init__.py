"""Bounded I4 persistence-fixture reference implementation.

The SQLite schema is language-neutral. ``reference_adapter`` qualifies only
fixture journal and CAS mechanics; it is not the final Component Manager,
``IActiveBundlePublicationStore``, or a normative publication DTO adapter.
It is deleted after the selected C++ owner passes equivalent black-box tests.
"""

from .reference_adapter import (
    ApplyIdentity,
    BootstrapIdentity,
    ComponentManagerPersistenceFixture,
    FixtureCasHead,
    FixtureCasIntent,
    FixtureCasReceipt,
    FixtureCasWriteResult,
    IntegrityReport,
    JournalAppendResult,
    JournalEntry,
    JournalEvent,
    OperationKind,
    OperationRecord,
    OperationRef,
    OperationWriteResult,
    PersistenceClosedError,
    PersistenceConflictError,
    PersistenceIntegrityError,
    PersistenceLimitError,
)

__all__ = [
    "ApplyIdentity",
    "BootstrapIdentity",
    "ComponentManagerPersistenceFixture",
    "FixtureCasHead",
    "FixtureCasIntent",
    "FixtureCasReceipt",
    "FixtureCasWriteResult",
    "IntegrityReport",
    "JournalAppendResult",
    "JournalEntry",
    "JournalEvent",
    "OperationKind",
    "OperationRecord",
    "OperationRef",
    "OperationWriteResult",
    "PersistenceClosedError",
    "PersistenceConflictError",
    "PersistenceIntegrityError",
    "PersistenceLimitError",
]

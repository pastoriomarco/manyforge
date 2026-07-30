-- ManyForge I4 Component Manager fixture-persistence substrate, version 1.
--
-- This is an internal, language-neutral SQLite storage contract.  A store
-- owner must execute the complete artifact in one exclusive transaction,
-- insert exactly one matching persistence_metadata row, and initialize the
-- fixture_cas_head singleton before exposing the store.  This generic fixture
-- CAS is not IActiveBundlePublicationStore or its migration schema and
-- contains no process, robot, scene, Kernel, or controller effect.

PRAGMA application_id = 1296454480;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS persistence_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 1),
    schema_artifact_digest TEXT NOT NULL,
    cell_id TEXT NOT NULL,
    maximum_operations INTEGER NOT NULL
        CHECK (
            maximum_operations > 0
            AND maximum_operations <= 9223372036854775807
        ),
    maximum_journal_entries_per_operation INTEGER NOT NULL
        CHECK (
            maximum_journal_entries_per_operation > 0
            AND maximum_journal_entries_per_operation
                <= 9223372036854775807
        ),
    maximum_total_journal_entries INTEGER NOT NULL
        CHECK (
            maximum_total_journal_entries > 0
            AND maximum_total_journal_entries <= 9223372036854775807
        ),
    maximum_fixture_cas_receipts INTEGER NOT NULL
        CHECK (
            maximum_fixture_cas_receipts > 0
            AND maximum_fixture_cas_receipts <= 9223372036854775807
        ),
    maximum_payload_bytes INTEGER NOT NULL
        CHECK (
            maximum_payload_bytes > 0
            AND maximum_payload_bytes <= 9223372036854775807
        ),
    maximum_main_database_page_image_bytes INTEGER NOT NULL
        CHECK (
            maximum_main_database_page_image_bytes > 0
            AND maximum_main_database_page_image_bytes
                <= 9223372036854775807
        )
) STRICT;

CREATE TABLE IF NOT EXISTS manager_operations (
    operation_kind TEXT NOT NULL
        CHECK (operation_kind IN ('bootstrap', 'apply')),
    operation_id TEXT NOT NULL,
    operation_generation INTEGER NOT NULL
        CHECK (
            operation_generation > 0
            AND operation_generation <= 9223372036854775807
        ),
    request_id TEXT NOT NULL,
    manager_owner_epoch TEXT NOT NULL,
    intent_digest TEXT NOT NULL,
    expected_fixture_generation INTEGER NOT NULL
        CHECK (
            expected_fixture_generation >= 0
            AND expected_fixture_generation <= 9223372036854775807
        ),
    predecessor_bundle_identity TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    target_bundle_identity TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    identity_digest TEXT NOT NULL,
    created_sequence INTEGER NOT NULL
        CHECK (
            created_sequence > 0
            AND created_sequence <= 9223372036854775807
        ),
    PRIMARY KEY (
        operation_kind,
        operation_id,
        operation_generation
    ),
    UNIQUE (created_sequence),
    CHECK (
        (
            operation_kind = 'bootstrap'
            AND request_id = ''
            AND manager_owner_epoch = ''
            AND expected_fixture_generation = 0
            AND predecessor_bundle_identity = ''
            AND predecessor_graph_revision = ''
        )
        OR
        (
            operation_kind = 'apply'
            AND request_id <> ''
            AND manager_owner_epoch <> ''
            AND expected_fixture_generation > 0
            AND predecessor_bundle_identity <> ''
            AND predecessor_graph_revision <> ''
        )
    )
) STRICT;

CREATE UNIQUE INDEX IF NOT EXISTS manager_operations_one_bootstrap
    ON manager_operations (operation_kind)
    WHERE operation_kind = 'bootstrap';

CREATE UNIQUE INDEX IF NOT EXISTS manager_operations_one_apply_request
    ON manager_operations (manager_owner_epoch, request_id)
    WHERE operation_kind = 'apply';

CREATE TABLE IF NOT EXISTS manager_journal_entries (
    operation_kind TEXT NOT NULL,
    operation_id TEXT NOT NULL,
    operation_generation INTEGER NOT NULL
        CHECK (
            operation_generation > 0
            AND operation_generation <= 9223372036854775807
        ),
    sequence INTEGER NOT NULL
        CHECK (
            sequence > 0
            AND sequence <= 9223372036854775807
        ),
    entry_id TEXT NOT NULL,
    event_kind TEXT NOT NULL,
    payload BLOB NOT NULL CHECK (
        typeof(payload) = 'blob'
        AND length(payload) <= 1048576
    ),
    payload_digest TEXT NOT NULL,
    previous_entry_digest TEXT NOT NULL,
    entry_digest TEXT NOT NULL,
    PRIMARY KEY (
        operation_kind,
        operation_id,
        operation_generation,
        sequence
    ),
    UNIQUE (
        operation_kind,
        operation_id,
        operation_generation,
        entry_id
    ),
    FOREIGN KEY (
        operation_kind,
        operation_id,
        operation_generation
    ) REFERENCES manager_operations (
        operation_kind,
        operation_id,
        operation_generation
    )
) STRICT;

CREATE TABLE IF NOT EXISTS fixture_cas_receipts (
    fixture_cas_id TEXT PRIMARY KEY,
    fixture_cas_intent_digest TEXT NOT NULL,
    operation_kind TEXT NOT NULL,
    operation_id TEXT NOT NULL,
    operation_generation INTEGER NOT NULL
        CHECK (
            operation_generation > 0
            AND operation_generation <= 9223372036854775807
        ),
    operation_identity_digest TEXT NOT NULL,
    expected_fixture_generation INTEGER NOT NULL
        CHECK (
            expected_fixture_generation >= 0
            AND expected_fixture_generation < 9223372036854775807
        ),
    predecessor_bundle_identity TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    committed_fixture_generation INTEGER NOT NULL
        CHECK (
            committed_fixture_generation > 0
            AND committed_fixture_generation <= 9223372036854775807
        ),
    target_bundle_identity TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    receipt_digest TEXT NOT NULL,
    created_sequence INTEGER NOT NULL
        CHECK (
            created_sequence > 0
            AND created_sequence <= 9223372036854775807
        ),
    UNIQUE (committed_fixture_generation),
    UNIQUE (created_sequence),
    FOREIGN KEY (
        operation_kind,
        operation_id,
        operation_generation
    ) REFERENCES manager_operations (
        operation_kind,
        operation_id,
        operation_generation
    ),
    CHECK (committed_fixture_generation = expected_fixture_generation + 1),
    CHECK (
        (
            operation_kind = 'bootstrap'
            AND expected_fixture_generation = 0
            AND predecessor_bundle_identity = ''
            AND predecessor_graph_revision = ''
        )
        OR
        (
            operation_kind = 'apply'
            AND expected_fixture_generation > 0
            AND predecessor_bundle_identity <> ''
            AND predecessor_graph_revision <> ''
        )
    )
) STRICT;

CREATE TABLE IF NOT EXISTS fixture_cas_head (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    generation INTEGER NOT NULL
        CHECK (
            generation >= 0
            AND generation <= 9223372036854775807
        ),
    bundle_identity TEXT NOT NULL,
    graph_revision TEXT NOT NULL,
    fixture_cas_id TEXT NOT NULL,
    receipt_digest TEXT NOT NULL,
    operation_kind TEXT NOT NULL,
    operation_id TEXT NOT NULL,
    operation_generation INTEGER NOT NULL
        CHECK (
            operation_generation >= 0
            AND operation_generation <= 9223372036854775807
        ),
    CHECK (
        (
            generation = 0
            AND bundle_identity = ''
            AND graph_revision = ''
            AND fixture_cas_id = ''
            AND receipt_digest = ''
            AND operation_kind = ''
            AND operation_id = ''
            AND operation_generation = 0
        )
        OR
        (
            generation > 0
            AND bundle_identity <> ''
            AND graph_revision <> ''
            AND fixture_cas_id <> ''
            AND receipt_digest <> ''
            AND operation_kind IN ('bootstrap', 'apply')
            AND operation_id <> ''
            AND operation_generation > 0
        )
    )
) STRICT;

CREATE TRIGGER IF NOT EXISTS persistence_metadata_no_update
BEFORE UPDATE ON persistence_metadata
BEGIN
    SELECT RAISE(ABORT, 'persistence_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS persistence_metadata_no_delete
BEFORE DELETE ON persistence_metadata
BEGIN
    SELECT RAISE(ABORT, 'persistence_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_operations_no_update
BEFORE UPDATE ON manager_operations
BEGIN
    SELECT RAISE(ABORT, 'manager_operations rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_operations_no_delete
BEFORE DELETE ON manager_operations
BEGIN
    SELECT RAISE(ABORT, 'manager_operations rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_journal_entries_no_update
BEFORE UPDATE ON manager_journal_entries
BEGIN
    SELECT RAISE(ABORT, 'manager_journal_entries rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_journal_entries_no_delete
BEFORE DELETE ON manager_journal_entries
BEGIN
    SELECT RAISE(ABORT, 'manager_journal_entries rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS fixture_cas_receipts_no_update
BEFORE UPDATE ON fixture_cas_receipts
BEGIN
    SELECT RAISE(ABORT, 'fixture_cas_receipts rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS fixture_cas_receipts_no_delete
BEFORE DELETE ON fixture_cas_receipts
BEGIN
    SELECT RAISE(ABORT, 'fixture_cas_receipts rows are immutable');
END;

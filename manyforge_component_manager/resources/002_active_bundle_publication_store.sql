-- ManyForge I4 durable active-bundle publication store, version 2.
--
-- This schema has exactly one public-pointer owner. Immutable receipts retain
-- exact replay and independent generation-bound reread evidence; the singleton
-- head is the only mutable active alias. It is deliberately separate from the
-- Component Manager foundation journal/CAS fixture. The receipt inventory is
-- bounded by eviction: a full store deletes its oldest receipt before the next
-- publication (version 2), so receipts form one contiguous generation window
-- ending at the head. The head receipt itself can never be deleted.

PRAGMA application_id = 1296449872;
PRAGMA user_version = 2;

CREATE TABLE IF NOT EXISTS publication_store_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 2),
    schema_artifact_digest TEXT NOT NULL,
    cell_id TEXT NOT NULL,
    maximum_receipts INTEGER NOT NULL
        CHECK (
            maximum_receipts > 0
            AND maximum_receipts <= 9223372036854775807
        ),
    maximum_main_database_page_image_bytes INTEGER NOT NULL
        CHECK (
            maximum_main_database_page_image_bytes > 0
            AND maximum_main_database_page_image_bytes
                <= 9223372036854775807
        )
) STRICT;

CREATE TABLE IF NOT EXISTS active_bundle_publication_receipts (
    publication_generation INTEGER PRIMARY KEY
        CHECK (
            publication_generation > 0
            AND publication_generation <= 9223372036854775807
        ),
    transaction_kind TEXT NOT NULL
        CHECK (transaction_kind IN ('bootstrap', 'named_apply')),
    transaction_id TEXT NOT NULL,
    transaction_generation INTEGER NOT NULL
        CHECK (
            transaction_generation > 0
            AND transaction_generation <= 9223372036854775807
        ),
    projection_stage_generation INTEGER NOT NULL
        CHECK (
            projection_stage_generation >= 0
            AND projection_stage_generation <= 9223372036854775807
        ),
    expected_previous_publication_generation INTEGER NOT NULL
        CHECK (
            expected_previous_publication_generation >= 0
            AND expected_previous_publication_generation
                < 9223372036854775807
        ),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (
            candidate_scene_revision_id > 0
            AND candidate_scene_revision_id <= 9223372036854775807
        ),
    candidate_scene_content_hash TEXT NOT NULL,
    private_materialization_content_digest TEXT NOT NULL,
    obsolete_managed_command_surfaces_retired_evidence_digest TEXT NOT NULL,
    request_digest TEXT NOT NULL,
    durable_public_pointer_content_digest TEXT NOT NULL,
    UNIQUE (
        transaction_kind,
        transaction_id,
        transaction_generation,
        projection_stage_generation
    ),
    UNIQUE (request_digest),
    UNIQUE (durable_public_pointer_content_digest),
    CHECK (
        publication_generation =
            expected_previous_publication_generation + 1
    ),
    CHECK (
        (
            transaction_kind = 'bootstrap'
            AND projection_stage_generation = 0
            AND expected_previous_publication_generation = 0
        )
        OR
        (
            transaction_kind = 'named_apply'
            AND projection_stage_generation > 0
            AND expected_previous_publication_generation > 0
        )
    )
) STRICT;

CREATE TABLE IF NOT EXISTS active_bundle_publication_head (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    publication_generation INTEGER NOT NULL
        CHECK (
            publication_generation >= 0
            AND publication_generation <= 9223372036854775807
        ),
    request_digest TEXT NOT NULL,
    durable_public_pointer_content_digest TEXT NOT NULL,
    CHECK (
        (
            publication_generation = 0
            AND request_digest = ''
            AND durable_public_pointer_content_digest = ''
        )
        OR
        (
            publication_generation > 0
            AND request_digest <> ''
            AND durable_public_pointer_content_digest <> ''
        )
    )
) STRICT;

CREATE TRIGGER IF NOT EXISTS publication_store_metadata_no_update
BEFORE UPDATE ON publication_store_metadata
BEGIN
    SELECT RAISE(ABORT, 'publication_store_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS publication_store_metadata_no_delete
BEFORE DELETE ON publication_store_metadata
BEGIN
    SELECT RAISE(ABORT, 'publication_store_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS active_bundle_publication_receipts_no_update
BEFORE UPDATE ON active_bundle_publication_receipts
BEGIN
    SELECT RAISE(ABORT, 'active_bundle_publication_receipts rows are immutable');
END;

CREATE TRIGGER IF NOT EXISTS active_bundle_publication_receipts_no_delete
BEFORE DELETE ON active_bundle_publication_receipts
WHEN OLD.publication_generation >= (
    SELECT publication_generation FROM active_bundle_publication_head
)
BEGIN
    SELECT RAISE(ABORT, 'the active-bundle publication head receipt cannot be deleted');
END;

CREATE TRIGGER IF NOT EXISTS active_bundle_publication_head_no_delete
BEFORE DELETE ON active_bundle_publication_head
BEGIN
    SELECT RAISE(ABORT, 'active_bundle_publication_head cannot be deleted');
END;

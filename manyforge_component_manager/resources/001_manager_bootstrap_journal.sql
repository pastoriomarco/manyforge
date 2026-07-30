-- ManyForge I4 typed canonical-empty bootstrap journal, version 1.
--
-- This schema stores a bounded append-only sequence of bootstrap intents and
-- each optional exact scene-owner accepted target. It has no Apply row,
-- arbitrary journal event, mutable phase, active pointer, process effect, or
-- robot semantic.

PRAGMA application_id = 1296450122;
PRAGMA user_version = 1;

CREATE TABLE IF NOT EXISTS bootstrap_journal_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 1),
    schema_artifact_digest TEXT NOT NULL,
    cell_id TEXT NOT NULL,
    maximum_bootstrap_attempts INTEGER NOT NULL
        CHECK (
            maximum_bootstrap_attempts > 0
            AND maximum_bootstrap_attempts <= 64
        ),
    maximum_native_runtime_bindings_per_attempt INTEGER NOT NULL
        CHECK (
            maximum_native_runtime_bindings_per_attempt > 0
            AND maximum_native_runtime_bindings_per_attempt <= 64
        ),
    maximum_main_database_page_image_bytes INTEGER NOT NULL
        CHECK (
            maximum_main_database_page_image_bytes > 0
            AND maximum_main_database_page_image_bytes
                <= 9223372036854775807
        )
) STRICT;

CREATE TABLE IF NOT EXISTS manager_bootstrap_intents (
    bootstrap_id TEXT NOT NULL,
    bootstrap_generation INTEGER NOT NULL
        CHECK (
            bootstrap_generation > 0
            AND bootstrap_generation <= 9223372036854775807
        ),
    canonical_empty_retained_bundle_id TEXT NOT NULL,
    canonical_empty_graph_revision TEXT NOT NULL,
    native_runtime_binding_set_digest TEXT NOT NULL,
    intent_digest TEXT NOT NULL,
    PRIMARY KEY (bootstrap_id, bootstrap_generation)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_bootstrap_native_runtime_bindings (
    bootstrap_id TEXT NOT NULL,
    bootstrap_generation INTEGER NOT NULL,
    ordinal INTEGER NOT NULL CHECK (ordinal >= 0 AND ordinal < 64),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL,
    PRIMARY KEY (bootstrap_id, bootstrap_generation, ordinal),
    UNIQUE (
        bootstrap_id,
        bootstrap_generation,
        provider_binding_id,
        provider_binding_revision,
        runtime_unit_id,
        runtime_epoch
    ),
    FOREIGN KEY (bootstrap_id, bootstrap_generation)
        REFERENCES manager_bootstrap_intents (
            bootstrap_id,
            bootstrap_generation
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_bootstrap_accepted_targets (
    bootstrap_id TEXT NOT NULL,
    bootstrap_generation INTEGER NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (
            candidate_scene_revision_id > 0
            AND candidate_scene_revision_id <= 9223372036854775807
        ),
    candidate_scene_content_hash TEXT NOT NULL,
    accepted_target_digest TEXT NOT NULL,
    PRIMARY KEY (bootstrap_id, bootstrap_generation),
    FOREIGN KEY (bootstrap_id, bootstrap_generation)
        REFERENCES manager_bootstrap_intents (
            bootstrap_id,
            bootstrap_generation
        )
) WITHOUT ROWID, STRICT;

CREATE TRIGGER IF NOT EXISTS bootstrap_journal_metadata_no_update
BEFORE UPDATE ON bootstrap_journal_metadata
BEGIN
    SELECT RAISE(ABORT, 'bootstrap_journal_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS bootstrap_journal_metadata_no_delete
BEFORE DELETE ON bootstrap_journal_metadata
BEGIN
    SELECT RAISE(ABORT, 'bootstrap_journal_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_intents_no_update
BEFORE UPDATE ON manager_bootstrap_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_bootstrap_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_intents_no_delete
BEFORE DELETE ON manager_bootstrap_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_bootstrap_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_native_bindings_no_update
BEFORE UPDATE ON manager_bootstrap_native_runtime_bindings
BEGIN
    SELECT RAISE(
        ABORT,
        'manager_bootstrap_native_runtime_bindings is immutable'
    );
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_native_bindings_no_delete
BEFORE DELETE ON manager_bootstrap_native_runtime_bindings
BEGIN
    SELECT RAISE(
        ABORT,
        'manager_bootstrap_native_runtime_bindings is immutable'
    );
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_accepted_targets_no_update
BEFORE UPDATE ON manager_bootstrap_accepted_targets
BEGIN
    SELECT RAISE(ABORT, 'manager_bootstrap_accepted_targets is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_bootstrap_accepted_targets_no_delete
BEFORE DELETE ON manager_bootstrap_accepted_targets
BEGIN
    SELECT RAISE(ABORT, 'manager_bootstrap_accepted_targets is immutable');
END;

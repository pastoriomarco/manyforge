-- ManyForge N5c native projection handoff extension, version 5.
--
-- Version 2 remains the immutable bootstrap/Apply completion ledger and
-- version 4 remains the public logical-scene transaction ledger.  This
-- append-only overlay records the no-native-effect frozen capture that must
-- precede a nonzero native stage, its exact predecessor-union-candidate rows,
-- and the forward-only activation-selection decision.  The existing v2 stage,
-- activation, discard, and finalization rows remain durable completion
-- markers; no accepted historical fact is rewritten.

PRAGMA user_version = 5;

CREATE TABLE IF NOT EXISTS logical_scene_native_handoff_migrations (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL CHECK (
        schema_id = 'manyforge.internal.logical-scene.n5c.v5'
    ),
    schema_version INTEGER NOT NULL CHECK (schema_version = 5),
    migration_artifact_digest TEXT NOT NULL CHECK (
        length(migration_artifact_digest) = 71
        AND substr(migration_artifact_digest, 1, 7) = 'sha256:'
    ),
    maximum_native_transition_rows INTEGER NOT NULL CHECK (
        maximum_native_transition_rows = 256
    ),
    maximum_native_recoveries INTEGER NOT NULL CHECK (
        maximum_native_recoveries = 256
    ),
    maximum_frozen_snapshot_bytes INTEGER NOT NULL CHECK (
        maximum_frozen_snapshot_bytes = 131072
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_native_intents (
    lineage_generation INTEGER NOT NULL UNIQUE CHECK (
        lineage_generation > 0 AND lineage_generation <= 256
    ),
    apply_id TEXT NOT NULL CHECK (
        length(apply_id) > 0 AND length(apply_id) <= 256
    ),
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0
        AND apply_generation <= 9007199254740991
    ),
    projection_stage_generation INTEGER NOT NULL CHECK (
        projection_stage_generation > 0
        AND projection_stage_generation <= 9007199254740991
    ),
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL CHECK (
        predecessor_scene_revision_id > 0
        AND predecessor_scene_revision_id <= 9007199254740991
    ),
    predecessor_scene_content_hash TEXT NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL CHECK (
        candidate_scene_revision_id > 0
        AND candidate_scene_revision_id <= 9007199254740991
    ),
    candidate_scene_content_hash TEXT NOT NULL,
    frozen_snapshot_json TEXT NOT NULL CHECK (
        length(frozen_snapshot_json) > 0
        AND length(frozen_snapshot_json) <= 131072
    ),
    native_transition_row_count INTEGER NOT NULL CHECK (
        native_transition_row_count >= 0
        AND native_transition_row_count <= 256
    ),
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, projection_stage_generation)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_native_transition_rows (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL,
    projection_stage_generation INTEGER NOT NULL,
    transition_ordinal INTEGER NOT NULL CHECK (
        transition_ordinal >= 0 AND transition_ordinal < 256
    ),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL,
    disposition TEXT NOT NULL CHECK (
        disposition IN ('retained', 'added', 'removed')
    ),
    expected_local_retained_bundle_id TEXT,
    expected_local_graph_revision TEXT,
    expected_local_scene_id TEXT,
    expected_local_scene_revision_id INTEGER,
    expected_local_scene_content_hash TEXT,
    PRIMARY KEY (
        apply_id, apply_generation, projection_stage_generation,
        transition_ordinal
    ),
    FOREIGN KEY (apply_id, apply_generation, projection_stage_generation)
        REFERENCES logical_scene_named_apply_native_intents (
            apply_id, apply_generation, projection_stage_generation
        ),
    CHECK (
        (disposition = 'added'
         AND expected_local_retained_bundle_id IS NULL
         AND expected_local_graph_revision IS NULL
         AND expected_local_scene_id IS NULL
         AND expected_local_scene_revision_id IS NULL
         AND expected_local_scene_content_hash IS NULL)
        OR
        (disposition IN ('retained', 'removed')
         AND expected_local_retained_bundle_id IS NOT NULL
         AND expected_local_graph_revision IS NOT NULL
         AND expected_local_scene_id IS NOT NULL
         AND expected_local_scene_revision_id IS NOT NULL
         AND expected_local_scene_content_hash IS NOT NULL)
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_activation_selections (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL,
    projection_stage_generation INTEGER NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL CHECK (
        candidate_scene_revision_id > 0
        AND candidate_scene_revision_id <= 9007199254740991
    ),
    candidate_scene_content_hash TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation, projection_stage_generation),
    FOREIGN KEY (apply_id, apply_generation, projection_stage_generation)
        REFERENCES logical_scene_named_apply_native_intents (
            apply_id, apply_generation, projection_stage_generation
        )
) WITHOUT ROWID, STRICT;

-- Runtime epochs are process-lifetime identities.  A completed structural
-- Apply remains immutable when a managed native host is replaced; this
-- append-only overlay records the exact current runtime set that was rebuilt
-- from the scene owner's already-active immutable snapshot.
CREATE TABLE IF NOT EXISTS logical_scene_native_runtime_recovery_intents (
    recovery_generation INTEGER PRIMARY KEY CHECK (
        recovery_generation > 0 AND recovery_generation <= 256
    ),
    structural_lineage_generation INTEGER NOT NULL CHECK (
        structural_lineage_generation > 0
        AND structural_lineage_generation <= 256
    ),
    target_retained_bundle_id TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    target_scene_id TEXT NOT NULL,
    target_scene_revision_id INTEGER NOT NULL CHECK (
        target_scene_revision_id > 0
        AND target_scene_revision_id <= 9007199254740991
    ),
    target_scene_content_hash TEXT NOT NULL,
    native_binding_count INTEGER NOT NULL CHECK (
        native_binding_count > 0 AND native_binding_count <= 256
    ),
    FOREIGN KEY (structural_lineage_generation)
        REFERENCES logical_scene_named_apply_stages (lineage_generation)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_native_runtime_recovery_bindings (
    recovery_generation INTEGER NOT NULL,
    binding_ordinal INTEGER NOT NULL CHECK (
        binding_ordinal >= 0 AND binding_ordinal < 256
    ),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL,
    PRIMARY KEY (recovery_generation, binding_ordinal),
    FOREIGN KEY (recovery_generation)
        REFERENCES logical_scene_native_runtime_recovery_intents (
            recovery_generation
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_native_runtime_recovery_results (
    recovery_generation INTEGER NOT NULL,
    binding_ordinal INTEGER NOT NULL CHECK (
        binding_ordinal >= 0 AND binding_ordinal < 256
    ),
    source_sequence INTEGER NOT NULL CHECK (
        source_sequence > 0
        AND source_sequence <= 9007199254740991
    ),
    native_projection_generation INTEGER NOT NULL CHECK (
        native_projection_generation > 0
        AND native_projection_generation <= 9007199254740991
    ),
    projection_content_digest TEXT NOT NULL CHECK (
        length(projection_content_digest) = 71
        AND substr(projection_content_digest, 1, 7) = 'sha256:'
    ),
    PRIMARY KEY (recovery_generation, binding_ordinal),
    FOREIGN KEY (recovery_generation, binding_ordinal)
        REFERENCES logical_scene_native_runtime_recovery_bindings (
            recovery_generation, binding_ordinal
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_native_runtime_recovery_completions (
    recovery_generation INTEGER PRIMARY KEY,
    result_count INTEGER NOT NULL CHECK (
        result_count > 0 AND result_count <= 256
    ),
    FOREIGN KEY (recovery_generation)
        REFERENCES logical_scene_native_runtime_recovery_intents (
            recovery_generation
        )
) STRICT;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_handoff_migrations_no_update
BEFORE UPDATE ON logical_scene_native_handoff_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_handoff_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_handoff_migrations_no_delete
BEFORE DELETE ON logical_scene_native_handoff_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_handoff_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_native_intents_no_update
BEFORE UPDATE ON logical_scene_named_apply_native_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_native_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_native_intents_no_delete
BEFORE DELETE ON logical_scene_named_apply_native_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_native_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_native_transition_rows_no_update
BEFORE UPDATE ON logical_scene_named_apply_native_transition_rows
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_native_transition_rows is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_native_transition_rows_no_delete
BEFORE DELETE ON logical_scene_named_apply_native_transition_rows
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_native_transition_rows is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_activation_selections_no_update
BEFORE UPDATE ON logical_scene_named_apply_activation_selections
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_activation_selections is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_activation_selections_no_delete
BEFORE DELETE ON logical_scene_named_apply_activation_selections
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_activation_selections is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_intents_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_intents_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_bindings_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_bindings is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_bindings_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_bindings is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_results_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_results
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_results is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_results_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_results
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_results is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_completions_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_completions is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_native_runtime_recovery_completions_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_completions is immutable');
END;

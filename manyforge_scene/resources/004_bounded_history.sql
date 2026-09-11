-- ManyForge UI6-I logical-scene history and withdrawal schema, version 7.
--
-- Version 4 owns durable scene transactions and version 5 owns native-runtime
-- recovery.  This overlay adds one bounded checkpoint/control row.  A
-- quiescent compaction retains the exact current immutable scene, advances an
-- opaque request-owner epoch, and removes only history already represented by
-- that checkpoint.  Native recovery compaction retains the current completed
-- runtime set plus at most one pending replacement while preserving a durable
-- monotonic generation watermark.

PRAGMA user_version = 7;

CREATE TABLE IF NOT EXISTS logical_scene_bounded_history_migrations (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL CHECK (
        schema_id = 'manyforge.internal.logical-scene.ui6i.v7'
    ),
    schema_version INTEGER NOT NULL CHECK (schema_version = 7),
    migration_artifact_digest TEXT NOT NULL CHECK (
        length(migration_artifact_digest) = 71
        AND substr(migration_artifact_digest, 1, 7) = 'sha256:'
    ),
    maximum_resident_scene_transactions INTEGER NOT NULL CHECK (
        maximum_resident_scene_transactions = 256
    ),
    maximum_retained_native_recoveries INTEGER NOT NULL CHECK (
        maximum_retained_native_recoveries = 2
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_history_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    compaction_generation INTEGER NOT NULL DEFAULT 0 CHECK (
        compaction_generation >= 0
        AND compaction_generation <= 9007199254740991
    ),
    request_epoch_generation INTEGER NOT NULL DEFAULT 0 CHECK (
        request_epoch_generation >= 0
        AND request_epoch_generation <= 9007199254740991
    ),
    request_owner_epoch TEXT CHECK (
        request_owner_epoch IS NULL
        OR (length(request_owner_epoch) > 0
            AND length(request_owner_epoch) <= 256)
    ),
    compacted_through_transaction_sequence INTEGER NOT NULL DEFAULT 0 CHECK (
        compacted_through_transaction_sequence >= 0
        AND compacted_through_transaction_sequence <= 9007199254740991
    ),
    compacted_through_owner_event_sequence INTEGER NOT NULL DEFAULT 0 CHECK (
        compacted_through_owner_event_sequence >= 0
        AND compacted_through_owner_event_sequence <= 9007199254740991
    ),
    compacted_named_apply_stage_count INTEGER NOT NULL DEFAULT 0 CHECK (
        compacted_named_apply_stage_count >= 0
        AND compacted_named_apply_stage_count <= 256
    ),
    recovered_through_transaction_sequence INTEGER NOT NULL DEFAULT 0 CHECK (
        recovered_through_transaction_sequence >= 0
        AND recovered_through_transaction_sequence <= 9007199254740991
    ),
    checkpoint_retained_bundle_id TEXT,
    checkpoint_graph_revision TEXT,
    checkpoint_scene_id TEXT,
    checkpoint_scene_revision_id INTEGER,
    checkpoint_scene_content_hash TEXT,
    checkpoint_snapshot_json TEXT CHECK (
        checkpoint_snapshot_json IS NULL
        OR (length(checkpoint_snapshot_json) > 0
            AND length(checkpoint_snapshot_json) <= 131072)
    ),
    native_recovery_generation_watermark INTEGER NOT NULL DEFAULT 0 CHECK (
        native_recovery_generation_watermark >= 0
        AND native_recovery_generation_watermark <= 9007199254740991
    ),
    compaction_in_progress INTEGER NOT NULL DEFAULT 0 CHECK (
        compaction_in_progress IN (0, 1)
    ),
    CHECK (
        (request_epoch_generation = 0 AND request_owner_epoch IS NULL)
        OR
        (request_epoch_generation > 0 AND request_owner_epoch IS NOT NULL)
    ),
    CHECK (
        (compaction_generation = 0
         AND request_epoch_generation = 0
         AND request_owner_epoch IS NULL
         AND compacted_through_transaction_sequence = 0
         AND compacted_through_owner_event_sequence = 0
         AND compacted_named_apply_stage_count = 0
         AND checkpoint_retained_bundle_id IS NULL
         AND checkpoint_graph_revision IS NULL
         AND checkpoint_scene_id IS NULL
         AND checkpoint_scene_revision_id IS NULL
         AND checkpoint_scene_content_hash IS NULL
         AND checkpoint_snapshot_json IS NULL)
        OR
        (compaction_generation > 0
         AND compacted_through_transaction_sequence > 0
         AND compacted_through_owner_event_sequence > 0
         AND request_epoch_generation > 1
         AND checkpoint_retained_bundle_id IS NOT NULL
         AND checkpoint_graph_revision IS NOT NULL
         AND checkpoint_scene_id IS NOT NULL
         AND checkpoint_scene_revision_id IS NOT NULL
         AND checkpoint_scene_content_hash IS NOT NULL
         AND checkpoint_snapshot_json IS NOT NULL)
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

INSERT INTO logical_scene_history_state(
    singleton,
    native_recovery_generation_watermark
)
SELECT
    1,
    coalesce(max(recovery_generation), 0)
FROM logical_scene_native_runtime_recovery_intents;

-- Version 5 used its retained-row bound as the recovery-generation bound.
-- Rebuild only this four-table family so retained rows stay bounded while the
-- durable generation remains globally monotonic for the life of the store.
CREATE TEMP TABLE manyforge_native_recovery_keep (
    recovery_generation INTEGER PRIMARY KEY
) STRICT;
INSERT OR IGNORE INTO manyforge_native_recovery_keep(recovery_generation)
SELECT max(recovery_generation)
FROM logical_scene_native_runtime_recovery_completions
HAVING max(recovery_generation) IS NOT NULL;
INSERT OR IGNORE INTO manyforge_native_recovery_keep(recovery_generation)
SELECT max(recovery_generation)
FROM logical_scene_native_runtime_recovery_intents
WHERE recovery_generation NOT IN (
    SELECT recovery_generation
    FROM logical_scene_native_runtime_recovery_completions
)
HAVING max(recovery_generation) IS NOT NULL;

CREATE TEMP TABLE manyforge_native_recovery_intents AS
SELECT recovery_generation,structural_lineage_generation,
       target_retained_bundle_id,target_graph_revision,target_scene_id,
       target_scene_revision_id,target_scene_content_hash,native_binding_count
FROM logical_scene_native_runtime_recovery_intents
WHERE recovery_generation IN (
    SELECT recovery_generation FROM manyforge_native_recovery_keep
);
CREATE TEMP TABLE manyforge_native_recovery_bindings AS
SELECT recovery_generation,binding_ordinal,provider_binding_id,
       provider_binding_revision,runtime_unit_id,runtime_epoch
FROM logical_scene_native_runtime_recovery_bindings
WHERE recovery_generation IN (
    SELECT recovery_generation FROM manyforge_native_recovery_keep
);
CREATE TEMP TABLE manyforge_native_recovery_results AS
SELECT recovery_generation,binding_ordinal,source_sequence,
       native_projection_generation,projection_content_digest
FROM logical_scene_native_runtime_recovery_results
WHERE recovery_generation IN (
    SELECT recovery_generation FROM manyforge_native_recovery_keep
);
CREATE TEMP TABLE manyforge_native_recovery_completions AS
SELECT recovery_generation,result_count
FROM logical_scene_native_runtime_recovery_completions
WHERE recovery_generation IN (
    SELECT recovery_generation FROM manyforge_native_recovery_keep
);

DROP TRIGGER logical_scene_native_runtime_recovery_completions_no_update;
DROP TRIGGER logical_scene_native_runtime_recovery_completions_no_delete;
DROP TRIGGER logical_scene_native_runtime_recovery_results_no_update;
DROP TRIGGER logical_scene_native_runtime_recovery_results_no_delete;
DROP TRIGGER logical_scene_native_runtime_recovery_bindings_no_update;
DROP TRIGGER logical_scene_native_runtime_recovery_bindings_no_delete;
DROP TRIGGER logical_scene_native_runtime_recovery_intents_no_update;
DROP TRIGGER logical_scene_native_runtime_recovery_intents_no_delete;

UPDATE logical_scene_history_state SET compaction_in_progress = 1
WHERE singleton = 1;
DROP TABLE logical_scene_native_runtime_recovery_results;
DROP TABLE logical_scene_native_runtime_recovery_completions;
DROP TABLE logical_scene_native_runtime_recovery_bindings;
DROP TABLE logical_scene_native_runtime_recovery_intents;

CREATE TABLE logical_scene_native_runtime_recovery_intents (
    recovery_generation INTEGER PRIMARY KEY CHECK (
        recovery_generation > 0
        AND recovery_generation <= 9007199254740991
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
    recovered_transaction_sequence INTEGER CHECK (
        recovered_transaction_sequence IS NULL
        OR (recovered_transaction_sequence > 0
            AND recovered_transaction_sequence <= 9007199254740991)
    ),
    FOREIGN KEY (structural_lineage_generation)
        REFERENCES logical_scene_named_apply_stages (lineage_generation)
) STRICT;

CREATE TABLE logical_scene_native_runtime_recovery_bindings (
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

CREATE TABLE logical_scene_native_runtime_recovery_results (
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

CREATE TABLE logical_scene_native_runtime_recovery_completions (
    recovery_generation INTEGER PRIMARY KEY,
    result_count INTEGER NOT NULL CHECK (
        result_count > 0 AND result_count <= 256
    ),
    FOREIGN KEY (recovery_generation)
        REFERENCES logical_scene_native_runtime_recovery_intents (
            recovery_generation
        )
) STRICT;

INSERT INTO logical_scene_native_runtime_recovery_intents(
    recovery_generation,structural_lineage_generation,
    target_retained_bundle_id,target_graph_revision,target_scene_id,
    target_scene_revision_id,target_scene_content_hash,native_binding_count,
    recovered_transaction_sequence
)
SELECT recovery_generation,structural_lineage_generation,
       target_retained_bundle_id,target_graph_revision,target_scene_id,
       target_scene_revision_id,target_scene_content_hash,native_binding_count,
       NULL
FROM manyforge_native_recovery_intents;
INSERT INTO logical_scene_native_runtime_recovery_bindings
SELECT * FROM manyforge_native_recovery_bindings;
INSERT INTO logical_scene_native_runtime_recovery_results
SELECT * FROM manyforge_native_recovery_results;
INSERT INTO logical_scene_native_runtime_recovery_completions
SELECT * FROM manyforge_native_recovery_completions;

CREATE TRIGGER logical_scene_native_runtime_recovery_intents_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_intents is immutable');
END;
CREATE TRIGGER logical_scene_native_runtime_recovery_bindings_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_bindings is immutable');
END;
CREATE TRIGGER logical_scene_native_runtime_recovery_results_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_results
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_results is immutable');
END;
CREATE TRIGGER logical_scene_native_runtime_recovery_completions_no_update
BEFORE UPDATE ON logical_scene_native_runtime_recovery_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_completions is immutable');
END;

UPDATE logical_scene_history_state SET compaction_in_progress = 0
WHERE singleton = 1;
DROP TABLE manyforge_native_recovery_completions;
DROP TABLE manyforge_native_recovery_results;
DROP TABLE manyforge_native_recovery_bindings;
DROP TABLE manyforge_native_recovery_intents;
DROP TABLE manyforge_native_recovery_keep;

-- The original append-only guards remain active outside the owner's one
-- atomic compaction transaction.  The control row is private store state and
-- must be zero whenever a transaction is externally observable.
DROP TRIGGER IF EXISTS logical_scene_transaction_intents_no_delete;
CREATE TRIGGER logical_scene_transaction_intents_no_delete
BEFORE DELETE ON logical_scene_transaction_intents
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_transaction_intents is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_request_outcomes_no_delete;
CREATE TRIGGER logical_scene_request_outcomes_no_delete
BEFORE DELETE ON logical_scene_request_outcomes
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_request_outcomes is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_projection_attempts_no_delete;
CREATE TRIGGER logical_scene_projection_attempts_no_delete
BEFORE DELETE ON logical_scene_projection_attempts
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_attempts is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_projection_results_no_delete;
CREATE TRIGGER logical_scene_projection_results_no_delete
BEFORE DELETE ON logical_scene_projection_results
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_results is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_owner_events_no_delete;
CREATE TRIGGER logical_scene_owner_events_no_delete
BEFORE DELETE ON logical_scene_owner_events
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_owner_events is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_native_runtime_recovery_intents_no_delete;
CREATE TRIGGER logical_scene_native_runtime_recovery_intents_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_intents
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_intents is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_native_runtime_recovery_bindings_no_delete;
CREATE TRIGGER logical_scene_native_runtime_recovery_bindings_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_bindings
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_bindings is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_native_runtime_recovery_results_no_delete;
CREATE TRIGGER logical_scene_native_runtime_recovery_results_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_results
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_results is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_native_runtime_recovery_completions_no_delete;
CREATE TRIGGER logical_scene_native_runtime_recovery_completions_no_delete
BEFORE DELETE ON logical_scene_native_runtime_recovery_completions
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_native_runtime_recovery_completions is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_bounded_history_migrations_no_update
BEFORE UPDATE ON logical_scene_bounded_history_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_bounded_history_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_bounded_history_migrations_no_delete
BEFORE DELETE ON logical_scene_bounded_history_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_bounded_history_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_history_state_no_delete
BEFORE DELETE ON logical_scene_history_state
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_history_state is durable owner state');
END;

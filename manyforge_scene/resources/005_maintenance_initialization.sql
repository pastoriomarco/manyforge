-- UA4 Scene-owned maintenance initialization, version 8.
--
-- This is the single operation-level record for fresh projection
-- initialization. It deliberately stores no per-projection progress or
-- resumable job generation. An intent without a result is interrupted on
-- owner reopen and remains closed until a later explicitly authorized
-- operation initializes afresh.

PRAGMA user_version = 8;

CREATE TABLE IF NOT EXISTS logical_scene_maintenance_initialization_migrations (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL CHECK (
        schema_id = 'manyforge.internal.logical-scene.ua4.v8'
    ),
    schema_version INTEGER NOT NULL CHECK (schema_version = 8),
    migration_artifact_digest TEXT NOT NULL CHECK (
        length(migration_artifact_digest) = 71
        AND substr(migration_artifact_digest, 1, 7) = 'sha256:'
    ),
    maximum_initializations INTEGER NOT NULL CHECK (
        maximum_initializations = 256
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

-- initialization_sequence is strictly increasing; at the bound the owner
-- evicts the oldest initialization no reader still needs.
CREATE TABLE IF NOT EXISTS logical_scene_maintenance_initialization_intents (
    initialization_sequence INTEGER PRIMARY KEY CHECK (
        initialization_sequence > 0
        AND initialization_sequence <= 9007199254740991
    ),
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0
        AND apply_generation <= 9007199254740991
    ),
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL,
    predecessor_scene_content_hash TEXT NOT NULL,
    target_retained_bundle_id TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    target_scene_id TEXT NOT NULL,
    target_scene_revision_id INTEGER NOT NULL,
    target_scene_content_hash TEXT NOT NULL,
    authoritative_snapshot_json TEXT NOT NULL CHECK (
        length(authoritative_snapshot_json) > 0
        AND length(authoritative_snapshot_json) <= 131072
    ),
    native_binding_count INTEGER NOT NULL CHECK (
        native_binding_count >= 0 AND native_binding_count <= 256
    ),
    repaired_transaction_sequence INTEGER,
    UNIQUE (apply_id, apply_generation)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_maintenance_initialization_bindings (
    initialization_sequence INTEGER NOT NULL,
    binding_ordinal INTEGER NOT NULL CHECK (
        binding_ordinal >= 0 AND binding_ordinal < 256
    ),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL,
    PRIMARY KEY (initialization_sequence, binding_ordinal),
    FOREIGN KEY (initialization_sequence)
        REFERENCES logical_scene_maintenance_initialization_intents (
            initialization_sequence
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_maintenance_initialization_results (
    initialization_sequence INTEGER PRIMARY KEY,
    state TEXT NOT NULL CHECK (state IN ('initialized', 'failed', 'interrupted')),
    first_error_code TEXT,
    first_error_message TEXT,
    effects_unknown INTEGER NOT NULL CHECK (effects_unknown IN (0, 1)),
    CHECK (
        (state = 'initialized' AND first_error_code IS NULL
         AND first_error_message IS NULL AND effects_unknown = 0)
        OR
        (state = 'failed' AND first_error_code IS NOT NULL
         AND first_error_message IS NOT NULL AND effects_unknown = 0)
        OR
        (state = 'interrupted' AND first_error_code IS NOT NULL
         AND first_error_message IS NOT NULL AND effects_unknown = 1)
    ),
    FOREIGN KEY (initialization_sequence)
        REFERENCES logical_scene_maintenance_initialization_intents (
            initialization_sequence
        )
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_maintenance_initialization_completions (
    initialization_sequence INTEGER PRIMARY KEY,
    expected_previous_publication_generation INTEGER NOT NULL,
    publication_generation INTEGER NOT NULL,
    private_materialization_content_digest TEXT NOT NULL,
    durable_public_pointer_content_digest TEXT NOT NULL,
    FOREIGN KEY (initialization_sequence)
        REFERENCES logical_scene_maintenance_initialization_results (
            initialization_sequence
        )
) STRICT;

-- One immutable migration snapshot replaces the former per-projection
-- recovery journal. Later successful operations derive their active bindings
-- directly from the append-only maintenance completion ledger.
CREATE TABLE IF NOT EXISTS logical_scene_maintenance_migration_state (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    legacy_native_effects_uncertain INTEGER NOT NULL CHECK (
        legacy_native_effects_uncertain IN (0, 1)
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

-- Completed pre-UA4 structural operations are reduced to the only facts
-- ordinary Scene replay still needs: their total-order identity and resulting
-- composite projection. The deployment handoff protocol itself is discarded.
CREATE TABLE IF NOT EXISTS logical_scene_migrated_structural_completions (
    completion_sequence INTEGER PRIMARY KEY CHECK (
        completion_sequence > 0 AND completion_sequence <= 256
    ),
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0 AND apply_generation <= 9007199254740991
    ),
    target_retained_bundle_id TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    target_scene_id TEXT NOT NULL,
    target_scene_revision_id INTEGER NOT NULL CHECK (
        target_scene_revision_id > 0
        AND target_scene_revision_id <= 9007199254740991
    ),
    target_scene_content_hash TEXT NOT NULL,
    UNIQUE (apply_id, apply_generation)
) STRICT;

INSERT OR IGNORE INTO logical_scene_migrated_structural_completions
SELECT s.lineage_generation,s.apply_id,s.apply_generation,
       s.candidate_retained_bundle_id,s.candidate_graph_revision,
       s.candidate_scene_id,s.candidate_scene_revision_id,
       s.candidate_scene_content_hash
FROM logical_scene_named_apply_stages AS s
INNER JOIN logical_scene_named_apply_finalizations AS f
    USING(apply_id,apply_generation,projection_stage_generation)
ORDER BY s.lineage_generation;

INSERT OR IGNORE INTO logical_scene_maintenance_migration_state(
    singleton, legacy_native_effects_uncertain
)
SELECT 1, EXISTS(
    SELECT 1
    FROM logical_scene_execution_host_recovery_intents AS i
    LEFT JOIN logical_scene_execution_host_recovery_completions AS c
        USING(recovery_generation)
    WHERE c.recovery_generation IS NULL
      AND i.structural_lineage_generation = (
          SELECT max(s.lineage_generation)
          FROM logical_scene_named_apply_stages AS s
          INNER JOIN logical_scene_named_apply_finalizations AS f
              USING(apply_id,apply_generation,projection_stage_generation)
      )
) OR EXISTS(
    SELECT 1
    FROM logical_scene_named_apply_stages AS s
    LEFT JOIN logical_scene_named_apply_finalizations AS f
        USING(apply_id,apply_generation,projection_stage_generation)
    LEFT JOIN logical_scene_named_apply_discards AS d
        USING(apply_id,apply_generation,projection_stage_generation)
    WHERE f.apply_id IS NULL AND d.apply_id IS NULL
);

CREATE TABLE IF NOT EXISTS logical_scene_migrated_active_execution_host_bindings (
    binding_ordinal INTEGER PRIMARY KEY CHECK (
        binding_ordinal >= 0 AND binding_ordinal < 256
    ),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL
) STRICT;

-- Prefer the latest fully completed runtime replacement. If none exists,
-- preserve the binding set of the latest finalized structural deployment.
INSERT OR IGNORE INTO logical_scene_migrated_active_execution_host_bindings
SELECT b.binding_ordinal,b.provider_binding_id,b.provider_binding_revision,
       b.runtime_unit_id,b.runtime_epoch
FROM logical_scene_execution_host_recovery_bindings AS b
INNER JOIN logical_scene_execution_host_recovery_intents AS i
    USING(recovery_generation)
WHERE b.recovery_generation = (
    SELECT max(c.recovery_generation)
    FROM logical_scene_execution_host_recovery_completions AS c
    INNER JOIN logical_scene_execution_host_recovery_intents AS i2
        USING(recovery_generation)
    WHERE i2.structural_lineage_generation = (
        SELECT max(s2.lineage_generation)
        FROM logical_scene_named_apply_stages AS s2
        INNER JOIN logical_scene_named_apply_finalizations AS f2
            USING(apply_id,apply_generation,projection_stage_generation)
    )
)
ORDER BY b.binding_ordinal;

INSERT OR IGNORE INTO logical_scene_migrated_active_execution_host_bindings
SELECT row_number() OVER (ORDER BY r.provider_binding_id,
                                   r.provider_binding_revision) - 1,
       r.provider_binding_id,r.provider_binding_revision,
       r.runtime_unit_id,r.runtime_epoch
FROM logical_scene_named_apply_native_transition_rows AS r
INNER JOIN logical_scene_named_apply_stages AS s
    USING(apply_id,apply_generation,projection_stage_generation)
INNER JOIN logical_scene_named_apply_finalizations AS f
    USING(apply_id,apply_generation,projection_stage_generation)
WHERE r.disposition != 'removed'
  AND NOT EXISTS (
      SELECT 1 FROM logical_scene_migrated_active_execution_host_bindings
  )
  AND s.lineage_generation = (
      SELECT max(s2.lineage_generation)
      FROM logical_scene_named_apply_stages AS s2
      INNER JOIN logical_scene_named_apply_finalizations AS f2
          USING(apply_id,apply_generation,projection_stage_generation)
  )
ORDER BY r.provider_binding_id,r.provider_binding_revision;

-- Replace deployment-specific compaction/recovery watermarks with the one
-- structural fact retained by the UA4 migration snapshot.
DROP TRIGGER IF EXISTS logical_scene_history_state_no_delete;
DROP TRIGGER IF EXISTS logical_scene_owner_events_no_delete;
-- The bounded-history delete guards read this table. Remove them before the
-- table swap, then restore the guards for tables that remain after migration.
DROP TRIGGER IF EXISTS logical_scene_transaction_intents_no_delete;
DROP TRIGGER IF EXISTS logical_scene_request_outcomes_no_delete;
DROP TRIGGER IF EXISTS logical_scene_projection_attempts_no_delete;
DROP TRIGGER IF EXISTS logical_scene_projection_results_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_intents_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_bindings_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_results_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_completions_no_delete;
CREATE TABLE logical_scene_history_state_ua4 (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    compaction_generation INTEGER NOT NULL CHECK (
        compaction_generation >= 0
        AND compaction_generation <= 9007199254740991
    ),
    request_epoch_generation INTEGER NOT NULL CHECK (
        request_epoch_generation >= 0
        AND request_epoch_generation <= 9007199254740991
    ),
    request_owner_epoch TEXT,
    compacted_through_transaction_sequence INTEGER NOT NULL CHECK (
        compacted_through_transaction_sequence >= 0
        AND compacted_through_transaction_sequence <= 9007199254740991
    ),
    compacted_through_owner_event_sequence INTEGER NOT NULL CHECK (
        compacted_through_owner_event_sequence >= 0
        AND compacted_through_owner_event_sequence <= 9007199254740991
    ),
    compacted_structural_completion_count INTEGER NOT NULL CHECK (
        compacted_structural_completion_count >= 0
        AND compacted_structural_completion_count <= 256
    ),
    compacted_maintenance_completion_count INTEGER NOT NULL CHECK (
        compacted_maintenance_completion_count >= 0
        AND compacted_maintenance_completion_count <= 256
    ),
    recovered_through_transaction_sequence INTEGER NOT NULL CHECK (
        recovered_through_transaction_sequence >= 0
        AND recovered_through_transaction_sequence <= 9007199254740991
    ),
    checkpoint_retained_bundle_id TEXT,
    checkpoint_graph_revision TEXT,
    checkpoint_scene_id TEXT,
    checkpoint_scene_revision_id INTEGER,
    checkpoint_scene_content_hash TEXT,
    checkpoint_snapshot_json TEXT,
    compaction_in_progress INTEGER NOT NULL CHECK (
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
         AND compacted_structural_completion_count = 0
         AND compacted_maintenance_completion_count = 0
         AND checkpoint_retained_bundle_id IS NULL
         AND checkpoint_graph_revision IS NULL
         AND checkpoint_scene_id IS NULL
         AND checkpoint_scene_revision_id IS NULL
         AND checkpoint_scene_content_hash IS NULL
         AND checkpoint_snapshot_json IS NULL)
        OR
        (compaction_generation > 0
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
INSERT INTO logical_scene_history_state_ua4
SELECT singleton,compaction_generation,request_epoch_generation,
       request_owner_epoch,compacted_through_transaction_sequence,
       compacted_through_owner_event_sequence,
       (SELECT count(*)
        FROM logical_scene_migrated_structural_completions AS c
        WHERE c.completion_sequence <=
              h.compacted_named_apply_stage_count),
       0,
       recovered_through_transaction_sequence,
       checkpoint_retained_bundle_id,checkpoint_graph_revision,
       checkpoint_scene_id,checkpoint_scene_revision_id,
       checkpoint_scene_content_hash,checkpoint_snapshot_json,
       compaction_in_progress
FROM logical_scene_history_state AS h;
DROP TABLE logical_scene_history_state;
ALTER TABLE logical_scene_history_state_ua4
    RENAME TO logical_scene_history_state;
CREATE TRIGGER logical_scene_history_state_no_delete
BEFORE DELETE ON logical_scene_history_state
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_history_state is durable owner state');
END;
CREATE TRIGGER logical_scene_transaction_intents_no_delete
BEFORE DELETE ON logical_scene_transaction_intents
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_transaction_intents is immutable');
END;
CREATE TRIGGER logical_scene_request_outcomes_no_delete
BEFORE DELETE ON logical_scene_request_outcomes
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_request_outcomes is immutable');
END;
CREATE TRIGGER logical_scene_projection_attempts_no_delete
BEFORE DELETE ON logical_scene_projection_attempts
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_attempts is immutable');
END;
CREATE TRIGGER logical_scene_projection_results_no_delete
BEFORE DELETE ON logical_scene_projection_results
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_results is immutable');
END;

-- Preserve the total order shared with ordinary logical Scene transactions,
-- while replacing the old deployment-specific event referent.
DROP TRIGGER IF EXISTS logical_scene_owner_events_no_update;
DROP TRIGGER IF EXISTS logical_scene_owner_events_no_delete;
CREATE TABLE logical_scene_owner_events_ua4 (
    owner_event_sequence INTEGER PRIMARY KEY CHECK (
        owner_event_sequence > 0
        AND owner_event_sequence <= 9007199254740991
    ),
    event_kind TEXT NOT NULL CHECK (
        event_kind IN ('scene_transaction_intent',
                       'migrated_structural_completion')
    ),
    transaction_sequence INTEGER UNIQUE,
    structural_completion_sequence INTEGER UNIQUE,
    CHECK (
        (event_kind = 'scene_transaction_intent'
         AND transaction_sequence IS NOT NULL
         AND structural_completion_sequence IS NULL)
        OR
        (event_kind = 'migrated_structural_completion'
         AND transaction_sequence IS NULL
         AND structural_completion_sequence IS NOT NULL)
    ),
    FOREIGN KEY (transaction_sequence)
        REFERENCES logical_scene_transaction_intents (transaction_sequence),
    FOREIGN KEY (structural_completion_sequence)
        REFERENCES logical_scene_migrated_structural_completions (
            completion_sequence
        )
) STRICT;
INSERT INTO logical_scene_owner_events_ua4
SELECT owner_event_sequence,
       CASE event_kind
           WHEN 'named_apply_finalized' THEN 'migrated_structural_completion'
           ELSE event_kind
       END,
       transaction_sequence,named_apply_lineage_generation
FROM logical_scene_owner_events
ORDER BY owner_event_sequence;
DROP TABLE logical_scene_owner_events;
ALTER TABLE logical_scene_owner_events_ua4 RENAME TO logical_scene_owner_events;
CREATE TRIGGER logical_scene_owner_events_no_update
BEFORE UPDATE ON logical_scene_owner_events
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_owner_events is immutable');
END;
CREATE TRIGGER logical_scene_owner_events_no_delete
BEFORE DELETE ON logical_scene_owner_events
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_owner_events is immutable');
END;

DROP TRIGGER IF EXISTS logical_scene_named_apply_native_intents_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_native_intents_no_delete;
DROP TRIGGER IF EXISTS logical_scene_named_apply_native_transition_rows_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_native_transition_rows_no_delete;
DROP TRIGGER IF EXISTS logical_scene_named_apply_activation_selections_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_activation_selections_no_delete;
DROP TABLE IF EXISTS logical_scene_named_apply_activation_selections;
DROP TABLE IF EXISTS logical_scene_named_apply_native_transition_rows;
DROP TABLE IF EXISTS logical_scene_named_apply_native_intents;

DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_intents_no_update;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_intents_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_bindings_no_update;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_bindings_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_results_no_update;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_results_no_delete;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_completions_no_update;
DROP TRIGGER IF EXISTS logical_scene_execution_host_recovery_completions_no_delete;
DROP TABLE IF EXISTS logical_scene_execution_host_recovery_results;
DROP TABLE IF EXISTS logical_scene_execution_host_recovery_completions;
DROP TABLE IF EXISTS logical_scene_execution_host_recovery_bindings;
DROP TABLE IF EXISTS logical_scene_execution_host_recovery_intents;

DROP TRIGGER IF EXISTS logical_scene_named_apply_stages_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_stages_no_delete;
DROP TRIGGER IF EXISTS logical_scene_named_apply_activations_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_activations_no_delete;
DROP TRIGGER IF EXISTS logical_scene_named_apply_discards_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_discards_no_delete;
DROP TRIGGER IF EXISTS logical_scene_named_apply_finalizations_no_update;
DROP TRIGGER IF EXISTS logical_scene_named_apply_finalizations_no_delete;
DROP TABLE IF EXISTS logical_scene_named_apply_finalizations;
DROP TABLE IF EXISTS logical_scene_named_apply_activations;
DROP TABLE IF EXISTS logical_scene_named_apply_discards;
DROP TABLE IF EXISTS logical_scene_named_apply_stages;

ALTER TABLE logical_scene_store_metadata
    RENAME COLUMN maximum_named_apply_stages
    TO maximum_migrated_structural_completions;

CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_migration_state_no_update
BEFORE UPDATE ON logical_scene_maintenance_migration_state
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance migration state is immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_migration_state_no_delete
BEFORE DELETE ON logical_scene_maintenance_migration_state
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance migration state is immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_migrated_active_execution_host_bindings_no_update
BEFORE UPDATE ON logical_scene_migrated_active_execution_host_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene migrated active bindings are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_migrated_active_execution_host_bindings_no_delete
BEFORE DELETE ON logical_scene_migrated_active_execution_host_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene migrated active bindings are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_migrated_structural_completions_no_update
BEFORE UPDATE ON logical_scene_migrated_structural_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene migrated structural completions are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_migrated_structural_completions_no_delete
BEFORE DELETE ON logical_scene_migrated_structural_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene migrated structural completions are immutable');
END;

-- Rows are immutable. Only the owner's bounded eviction of the oldest settled
-- initialization deletes them, inside its compaction_in_progress window.
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_intents_no_update
BEFORE UPDATE ON logical_scene_maintenance_initialization_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance intents are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_intents_no_delete
BEFORE DELETE ON logical_scene_maintenance_initialization_intents
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance intents are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_bindings_no_update
BEFORE UPDATE ON logical_scene_maintenance_initialization_bindings
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance bindings are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_bindings_no_delete
BEFORE DELETE ON logical_scene_maintenance_initialization_bindings
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance bindings are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_results_no_update
BEFORE UPDATE ON logical_scene_maintenance_initialization_results
WHEN NOT (
    OLD.initialization_sequence = NEW.initialization_sequence
    AND OLD.state = 'initialized'
    AND OLD.first_error_code IS NULL
    AND OLD.first_error_message IS NULL
    AND OLD.effects_unknown = 0
    AND NEW.state = 'interrupted'
    AND NEW.first_error_code = 'scene_initialization_interrupted'
    AND NEW.first_error_message =
        'Scene owner reopened before maintenance completion'
    AND NEW.effects_unknown = 1
)
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance terminal results are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_results_no_delete
BEFORE DELETE ON logical_scene_maintenance_initialization_results
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance terminal results are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_completions_no_update
BEFORE UPDATE ON logical_scene_maintenance_initialization_completions
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance completions are immutable');
END;
CREATE TRIGGER IF NOT EXISTS logical_scene_maintenance_initialization_completions_no_delete
BEFORE DELETE ON logical_scene_maintenance_initialization_completions
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'logical_scene maintenance completions are immutable');
END;

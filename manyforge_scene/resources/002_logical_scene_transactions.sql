-- ManyForge N5b logical-scene transaction extension, version 3.
--
-- Version 2 remains the immutable I4 bootstrap/Apply lineage. This migration
-- adds the bounded append-only public request journal without moving or
-- rewriting any accepted I4 fact.

PRAGMA user_version = 3;

CREATE TABLE IF NOT EXISTS logical_scene_store_migrations (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL CHECK (
        schema_id = 'manyforge.internal.logical-scene.n5b.v3'
    ),
    schema_version INTEGER NOT NULL CHECK (schema_version = 3),
    migration_artifact_digest TEXT NOT NULL CHECK (
        length(migration_artifact_digest) = 71
        AND substr(migration_artifact_digest, 1, 7) = 'sha256:'
    ),
    maximum_scene_transactions INTEGER NOT NULL CHECK (
        maximum_scene_transactions = 256
    ),
    maximum_transaction_bytes INTEGER NOT NULL CHECK (
        maximum_transaction_bytes = 131072
    ),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_transaction_intents (
    transaction_sequence INTEGER PRIMARY KEY CHECK (
        transaction_sequence > 0
        AND transaction_sequence <= 9007199254740991
    ),
    request_owner_epoch TEXT NOT NULL CHECK (
        length(request_owner_epoch) > 0
        AND length(request_owner_epoch) <= 256
    ),
    request_id TEXT NOT NULL CHECK (
        length(request_id) > 0 AND length(request_id) <= 256
    ),
    request_digest TEXT NOT NULL CHECK (
        length(request_digest) = 71
        AND substr(request_digest, 1, 7) = 'sha256:'
    ),
    request_json TEXT NOT NULL CHECK (
        length(request_json) > 0 AND length(request_json) <= 131072
    ),
    prior_retained_bundle_id TEXT NOT NULL,
    prior_graph_revision TEXT NOT NULL,
    prior_scene_id TEXT NOT NULL,
    prior_scene_revision_id INTEGER NOT NULL CHECK (
        prior_scene_revision_id > 0
        AND prior_scene_revision_id <= 9007199254740991
    ),
    prior_scene_content_hash TEXT NOT NULL,
    proposed_retained_bundle_id TEXT NOT NULL,
    proposed_graph_revision TEXT NOT NULL,
    proposed_scene_id TEXT NOT NULL,
    proposed_scene_revision_id INTEGER NOT NULL CHECK (
        proposed_scene_revision_id > 0
        AND proposed_scene_revision_id <= 9007199254740991
    ),
    proposed_scene_content_hash TEXT NOT NULL,
    resolved_operation_json TEXT NOT NULL CHECK (
        length(resolved_operation_json) > 0
        AND length(resolved_operation_json) <= 131072
    ),
    resolution_evidence_digest TEXT,
    candidate_snapshot_json TEXT NOT NULL CHECK (
        length(candidate_snapshot_json) > 0
        AND length(candidate_snapshot_json) <= 131072
    ),
    update_status TEXT NOT NULL CHECK (
        update_status IN ('geometry_changed', 'freshness_only', 'no_op')
    ),
    required_projection_count INTEGER NOT NULL CHECK (
        required_projection_count >= 0
        AND required_projection_count <= 256
    ),
    UNIQUE (request_owner_epoch, request_id)
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_request_outcomes (
    request_owner_epoch TEXT NOT NULL,
    request_id TEXT NOT NULL,
    request_digest TEXT NOT NULL CHECK (
        length(request_digest) = 71
        AND substr(request_digest, 1, 7) = 'sha256:'
    ),
    transaction_sequence INTEGER,
    success INTEGER NOT NULL CHECK (success IN (0, 1)),
    projection_status TEXT NOT NULL CHECK (
        projection_status IN (
            'not_attempted',
            'committed',
            'rejected_rollback_proven',
            'failed_projection_uncertain'
        )
    ),
    attempted_projection_count INTEGER NOT NULL CHECK (
        attempted_projection_count >= 0
        AND attempted_projection_count <= 256
    ),
    error_code TEXT,
    error_message TEXT,
    error_details_json TEXT,
    PRIMARY KEY (request_owner_epoch, request_id),
    FOREIGN KEY (transaction_sequence)
        REFERENCES logical_scene_transaction_intents (transaction_sequence),
    CHECK (
        (success = 1
         AND projection_status = 'committed'
         AND transaction_sequence IS NOT NULL
         AND error_code IS NULL
         AND error_message IS NULL
         AND error_details_json IS NULL)
        OR
        (success = 0
         AND projection_status != 'committed'
         AND error_code IS NOT NULL
         AND error_message IS NOT NULL
         AND error_details_json IS NOT NULL)
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_projection_attempts (
    transaction_sequence INTEGER NOT NULL,
    projection_ordinal INTEGER NOT NULL CHECK (
        projection_ordinal >= 0 AND projection_ordinal < 256
    ),
    provider_binding_id TEXT NOT NULL,
    provider_binding_revision TEXT NOT NULL,
    runtime_unit_id TEXT NOT NULL,
    runtime_epoch TEXT NOT NULL,
    PRIMARY KEY (transaction_sequence, projection_ordinal),
    FOREIGN KEY (transaction_sequence)
        REFERENCES logical_scene_transaction_intents (transaction_sequence)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_projection_results (
    transaction_sequence INTEGER NOT NULL,
    projection_ordinal INTEGER NOT NULL CHECK (
        projection_ordinal >= 0 AND projection_ordinal < 256
    ),
    result_state TEXT NOT NULL CHECK (
        result_state IN (
            'committed',
            'rejected_rollback_proven',
            'failed_projection_uncertain'
        )
    ),
    acknowledged_retained_bundle_id TEXT,
    acknowledged_graph_revision TEXT,
    acknowledged_scene_id TEXT,
    acknowledged_scene_revision_id INTEGER,
    acknowledged_scene_content_hash TEXT,
    rollback_proven INTEGER NOT NULL CHECK (rollback_proven IN (0, 1)),
    resulting_health_state TEXT NOT NULL CHECK (
        resulting_health_state IN (
            'uninitialized',
            'healthy',
            'desynchronized',
            'rebuilding',
            'unavailable'
        )
    ),
    error_code TEXT,
    error_message TEXT,
    error_details_json TEXT,
    PRIMARY KEY (transaction_sequence, projection_ordinal),
    FOREIGN KEY (transaction_sequence, projection_ordinal)
        REFERENCES logical_scene_projection_attempts (
            transaction_sequence,
            projection_ordinal
        ),
    CHECK (
        (result_state = 'committed'
         AND acknowledged_retained_bundle_id IS NOT NULL
         AND acknowledged_graph_revision IS NOT NULL
         AND acknowledged_scene_id IS NOT NULL
         AND acknowledged_scene_revision_id IS NOT NULL
         AND acknowledged_scene_content_hash IS NOT NULL
         AND rollback_proven = 0
         AND error_code IS NULL
         AND error_message IS NULL
         AND error_details_json IS NULL)
        OR
        (result_state != 'committed'
         AND error_code IS NOT NULL
         AND error_message IS NOT NULL
         AND error_details_json IS NOT NULL)
    )
) WITHOUT ROWID, STRICT;

-- One durable total order for accepted scene intents and finalized named
-- Applies. The individual v2/v3 ledgers remain the complete typed evidence;
-- this table records only ordering so restart cannot guess whether a scene
-- request (including a failed or interrupted one) preceded or followed a
-- structural transition.
CREATE TABLE IF NOT EXISTS logical_scene_owner_events (
    owner_event_sequence INTEGER PRIMARY KEY CHECK (
        owner_event_sequence > 0
        AND owner_event_sequence <= 9007199254740991
    ),
    event_kind TEXT NOT NULL CHECK (
        event_kind IN ('scene_transaction_intent', 'named_apply_finalized')
    ),
    transaction_sequence INTEGER UNIQUE,
    named_apply_lineage_generation INTEGER UNIQUE,
    CHECK (
        (event_kind = 'scene_transaction_intent'
         AND transaction_sequence IS NOT NULL
         AND named_apply_lineage_generation IS NULL)
        OR
        (event_kind = 'named_apply_finalized'
         AND transaction_sequence IS NULL
         AND named_apply_lineage_generation IS NOT NULL)
    ),
    FOREIGN KEY (transaction_sequence)
        REFERENCES logical_scene_transaction_intents (transaction_sequence),
    FOREIGN KEY (named_apply_lineage_generation)
        REFERENCES logical_scene_named_apply_stages (lineage_generation)
) STRICT;

-- A v2 store can already contain finalized named Applies when it is migrated.
-- No public scene transaction existed in v2, so their lineage order is the
-- complete historical publication order and can be backfilled losslessly.
INSERT INTO logical_scene_owner_events(
    owner_event_sequence,
    event_kind,
    transaction_sequence,
    named_apply_lineage_generation
)
SELECT
    row_number() OVER (ORDER BY stages.lineage_generation),
    'named_apply_finalized',
    NULL,
    stages.lineage_generation
FROM logical_scene_named_apply_stages AS stages
INNER JOIN logical_scene_named_apply_finalizations AS finalized USING(
    apply_id,
    apply_generation,
    projection_stage_generation
)
ORDER BY stages.lineage_generation;

CREATE TRIGGER IF NOT EXISTS logical_scene_store_migrations_no_update
BEFORE UPDATE ON logical_scene_store_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_store_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_store_migrations_no_delete
BEFORE DELETE ON logical_scene_store_migrations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_store_migrations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_transaction_intents_no_update
BEFORE UPDATE ON logical_scene_transaction_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_transaction_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_transaction_intents_no_delete
BEFORE DELETE ON logical_scene_transaction_intents
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_transaction_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_request_outcomes_no_update
BEFORE UPDATE ON logical_scene_request_outcomes
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_request_outcomes is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_request_outcomes_no_delete
BEFORE DELETE ON logical_scene_request_outcomes
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_request_outcomes is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_projection_attempts_no_update
BEFORE UPDATE ON logical_scene_projection_attempts
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_attempts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_projection_attempts_no_delete
BEFORE DELETE ON logical_scene_projection_attempts
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_attempts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_projection_results_no_update
BEFORE UPDATE ON logical_scene_projection_results
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_results is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_projection_results_no_delete
BEFORE DELETE ON logical_scene_projection_results
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_projection_results is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_owner_events_no_update
BEFORE UPDATE ON logical_scene_owner_events
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_owner_events is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_owner_events_no_delete
BEFORE DELETE ON logical_scene_owner_events
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_owner_events is immutable');
END;

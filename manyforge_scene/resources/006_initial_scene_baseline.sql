-- UA7S: append-only evidence for the Program baseline installed by one Apply.
-- Older maintenance rows have no row here and retain their v8 meaning.
PRAGMA user_version = 9;

CREATE TABLE logical_scene_initial_baseline_migrations (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL CHECK (
        schema_id = 'manyforge.internal.logical-scene.ua7s.v9'),
    schema_version INTEGER NOT NULL CHECK (schema_version = 9),
    migration_artifact_digest TEXT NOT NULL CHECK (
        length(migration_artifact_digest) = 71
        AND substr(migration_artifact_digest, 1, 7) = 'sha256:'),
    FOREIGN KEY (singleton) REFERENCES logical_scene_store_metadata(singleton)
) STRICT;

CREATE TABLE logical_scene_initial_baseline_requests (
    initialization_sequence INTEGER PRIMARY KEY,
    request_digest TEXT NOT NULL CHECK (
        length(request_digest) = 71 AND substr(request_digest, 1, 7) = 'sha256:'),
    FOREIGN KEY (initialization_sequence) REFERENCES
        logical_scene_maintenance_initialization_intents(initialization_sequence)
) STRICT;

CREATE TABLE logical_scene_initial_baseline_intents (
    initialization_sequence INTEGER PRIMARY KEY,
    target_retained_bundle_id TEXT NOT NULL,
    target_graph_revision TEXT NOT NULL,
    target_scene_id TEXT NOT NULL,
    target_scene_revision_id INTEGER NOT NULL CHECK (
        target_scene_revision_id > 0 AND target_scene_revision_id <= 9007199254740991),
    target_scene_content_hash TEXT NOT NULL,
    snapshot_json TEXT NOT NULL CHECK (
        length(snapshot_json) > 0 AND length(snapshot_json) <= 131072),
    FOREIGN KEY (initialization_sequence) REFERENCES
        logical_scene_initial_baseline_requests(initialization_sequence)
) STRICT;

CREATE TABLE logical_scene_initial_baseline_results (
    initialization_sequence INTEGER PRIMARY KEY,
    state TEXT NOT NULL CHECK (state IN ('installed', 'failed', 'interrupted')),
    first_error_code TEXT,
    first_error_message TEXT,
    CHECK ((state = 'installed' AND first_error_code IS NULL
            AND first_error_message IS NULL)
        OR (state IN ('failed', 'interrupted') AND first_error_code IS NOT NULL
            AND first_error_message IS NOT NULL)),
    FOREIGN KEY (initialization_sequence) REFERENCES
        logical_scene_initial_baseline_requests(initialization_sequence)
) STRICT;

CREATE TRIGGER logical_scene_initial_baseline_migrations_no_update
BEFORE UPDATE ON logical_scene_initial_baseline_migrations
BEGIN
    SELECT RAISE(ABORT, 'initial Scene migration evidence is immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_migrations_no_delete
BEFORE DELETE ON logical_scene_initial_baseline_migrations
BEGIN
    SELECT RAISE(ABORT, 'initial Scene migration evidence is immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_requests_no_update
BEFORE UPDATE ON logical_scene_initial_baseline_requests
BEGIN
    SELECT RAISE(ABORT, 'initial Scene requests are immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_requests_no_delete
BEFORE DELETE ON logical_scene_initial_baseline_requests
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'initial Scene requests are immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_intents_no_update
BEFORE UPDATE ON logical_scene_initial_baseline_intents
BEGIN
    SELECT RAISE(ABORT, 'initial Scene intents are immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_intents_no_delete
BEFORE DELETE ON logical_scene_initial_baseline_intents
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'initial Scene intents are immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_results_no_update
BEFORE UPDATE ON logical_scene_initial_baseline_results
BEGIN
    SELECT RAISE(ABORT, 'initial Scene results are immutable');
END;
CREATE TRIGGER logical_scene_initial_baseline_results_no_delete
BEFORE DELETE ON logical_scene_initial_baseline_results
WHEN (SELECT compaction_in_progress FROM logical_scene_history_state
      WHERE singleton = 1) != 1
BEGIN
    SELECT RAISE(ABORT, 'initial Scene results are immutable');
END;

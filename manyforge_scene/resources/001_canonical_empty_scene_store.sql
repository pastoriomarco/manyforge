-- ManyForge I4 logical-scene owner store, version 2.
--
-- This is a one-lineage, append-only bootstrap plus named-Apply stage store.
-- It intentionally has no Manager process rows, generic event journal, native
-- adapter payload, public scene-edit request, mutable gate flag, or legacy
-- SceneAuthority row.

PRAGMA application_id = 1296454467;
PRAGMA user_version = 2;

CREATE TABLE IF NOT EXISTS logical_scene_store_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 2),
    schema_artifact_digest TEXT NOT NULL CHECK (
        length(schema_artifact_digest) = 71
        AND substr(schema_artifact_digest, 1, 7) = 'sha256:'
    ),
    canonical_empty_admission_digest TEXT NOT NULL CHECK (
        length(canonical_empty_admission_digest) = 71
        AND substr(canonical_empty_admission_digest, 1, 7) = 'sha256:'
    ),
    cell_id TEXT NOT NULL CHECK (
        length(cell_id) > 0 AND length(cell_id) <= 256
    ),
    canonical_empty_retained_bundle_id TEXT NOT NULL CHECK (
        length(canonical_empty_retained_bundle_id) = 71
        AND substr(canonical_empty_retained_bundle_id, 1, 7) = 'sha256:'
    ),
    canonical_empty_graph_revision TEXT NOT NULL CHECK (
        length(canonical_empty_graph_revision) = 71
        AND substr(canonical_empty_graph_revision, 1, 7) = 'sha256:'
    ),
    maximum_identifier_bytes INTEGER NOT NULL CHECK (
        maximum_identifier_bytes = 256
    ),
    maximum_database_page_image_bytes INTEGER NOT NULL CHECK (
        maximum_database_page_image_bytes = 67108864
    ),
    maximum_wal_bytes INTEGER NOT NULL CHECK (
        maximum_wal_bytes = 16777216
    ),
    maximum_named_apply_stages INTEGER NOT NULL CHECK (
        maximum_named_apply_stages = 256
    )
) STRICT;

CREATE TABLE IF NOT EXISTS canonical_empty_scene_acceptance (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    bootstrap_id TEXT NOT NULL CHECK (
        length(bootstrap_id) > 0 AND length(bootstrap_id) <= 256
    ),
    bootstrap_generation INTEGER NOT NULL CHECK (
        bootstrap_generation > 0
        AND bootstrap_generation <= 9007199254740991
    ),
    cell_id TEXT NOT NULL CHECK (
        length(cell_id) > 0 AND length(cell_id) <= 256
    ),
    retained_bundle_id TEXT NOT NULL CHECK (
        length(retained_bundle_id) = 71
        AND substr(retained_bundle_id, 1, 7) = 'sha256:'
    ),
    graph_revision TEXT NOT NULL CHECK (
        length(graph_revision) = 71
        AND substr(graph_revision, 1, 7) = 'sha256:'
    ),
    scene_id TEXT NOT NULL CHECK (
        length(scene_id) = 71 AND substr(scene_id, 1, 7) = 'sha256:'
    ),
    scene_revision_id INTEGER NOT NULL CHECK (scene_revision_id = 1),
    scene_content_hash TEXT NOT NULL CHECK (
        length(scene_content_hash) = 71
        AND substr(scene_content_hash, 1, 7) = 'sha256:'
    ),
    transaction_sequence INTEGER NOT NULL CHECK (transaction_sequence = 0),
    topology_epoch INTEGER NOT NULL CHECK (topology_epoch = 0),
    policy_epoch INTEGER NOT NULL CHECK (policy_epoch = 0),
    configured_policy_id TEXT NOT NULL CHECK (
        configured_policy_id = 'free_space'
    ),
    configured_policy_revision_id INTEGER NOT NULL CHECK (
        configured_policy_revision_id = 1
    ),
    configured_policy_content_hash TEXT NOT NULL CHECK (
        configured_policy_content_hash =
        'sha256:59e1687796336598d7256d010902f760beb3c238adba73fe3e5ccd6674d797ac'
    ),
    object_count INTEGER NOT NULL CHECK (object_count = 0),
    carried_attachment_count INTEGER NOT NULL CHECK (
        carried_attachment_count = 0
    ),
    native_runtime_binding_count INTEGER NOT NULL CHECK (
        native_runtime_binding_count = 0
    ),
    UNIQUE (bootstrap_id, bootstrap_generation),
    FOREIGN KEY (singleton)
        REFERENCES logical_scene_store_metadata (singleton)
) STRICT;

CREATE TABLE IF NOT EXISTS canonical_empty_scene_rebuild (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    bootstrap_id TEXT NOT NULL,
    bootstrap_generation INTEGER NOT NULL,
    native_rebuild_result_count INTEGER NOT NULL CHECK (
        native_rebuild_result_count = 0
    ),
    aggregate_health_state TEXT NOT NULL CHECK (
        aggregate_health_state = 'healthy'
    ),
    aggregate_native_health_count INTEGER NOT NULL CHECK (
        aggregate_native_health_count = 0
    ),
    UNIQUE (bootstrap_id, bootstrap_generation),
    FOREIGN KEY (bootstrap_id, bootstrap_generation)
        REFERENCES canonical_empty_scene_acceptance (
            bootstrap_id,
            bootstrap_generation
        )
) STRICT;

CREATE TABLE IF NOT EXISTS canonical_empty_scene_finalization (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    bootstrap_id TEXT NOT NULL,
    bootstrap_generation INTEGER NOT NULL,
    publication_generation INTEGER NOT NULL CHECK (
        publication_generation > 0
        AND publication_generation <= 9007199254740991
    ),
    publication_bootstrap_id TEXT NOT NULL,
    publication_bootstrap_generation INTEGER NOT NULL CHECK (
        publication_bootstrap_generation > 0
        AND publication_bootstrap_generation <= 9007199254740991
    ),
    candidate_retained_bundle_id TEXT NOT NULL CHECK (
        length(candidate_retained_bundle_id) = 71
        AND substr(candidate_retained_bundle_id, 1, 7) = 'sha256:'
    ),
    candidate_graph_revision TEXT NOT NULL CHECK (
        length(candidate_graph_revision) = 71
        AND substr(candidate_graph_revision, 1, 7) = 'sha256:'
    ),
    candidate_scene_id TEXT NOT NULL CHECK (
        length(candidate_scene_id) = 71
        AND substr(candidate_scene_id, 1, 7) = 'sha256:'
    ),
    candidate_scene_revision_id INTEGER NOT NULL CHECK (
        candidate_scene_revision_id = 1
    ),
    candidate_scene_content_hash TEXT NOT NULL CHECK (
        length(candidate_scene_content_hash) = 71
        AND substr(candidate_scene_content_hash, 1, 7) = 'sha256:'
    ),
    request_candidate_retained_bundle_id TEXT NOT NULL,
    request_candidate_graph_revision TEXT NOT NULL,
    private_materialization_content_digest TEXT NOT NULL CHECK (
        length(private_materialization_content_digest) = 71
        AND substr(private_materialization_content_digest, 1, 7) = 'sha256:'
    ),
    -- Retained only for the historical v2 bootstrap-store shape. New writers
    -- repeat the private-materialization digest; no retirement phase exists.
    retired_command_surfaces_evidence_digest TEXT NOT NULL CHECK (
        length(retired_command_surfaces_evidence_digest) = 71
        AND substr(retired_command_surfaces_evidence_digest, 1, 7) = 'sha256:'
    ),
    durable_public_pointer_content_digest TEXT NOT NULL CHECK (
        length(durable_public_pointer_content_digest) = 71
        AND substr(durable_public_pointer_content_digest, 1, 7) = 'sha256:'
    ),
    UNIQUE (bootstrap_id, bootstrap_generation),
    FOREIGN KEY (bootstrap_id, bootstrap_generation)
        REFERENCES canonical_empty_scene_rebuild (
            bootstrap_id,
            bootstrap_generation
        )
) STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_stages (
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
    transition_row_count INTEGER NOT NULL CHECK (transition_row_count = 0),
    transition_evidence_state TEXT NOT NULL CHECK (
        transition_evidence_state = 'staged'
    ),
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, projection_stage_generation)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_activations (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0
        AND apply_generation <= 9007199254740991
    ),
    projection_stage_generation INTEGER NOT NULL CHECK (
        projection_stage_generation > 0
        AND projection_stage_generation <= 9007199254740991
    ),
    transition_row_count INTEGER NOT NULL CHECK (transition_row_count = 0),
    transition_evidence_state TEXT NOT NULL CHECK (
        transition_evidence_state = 'activated_and_retired'
    ),
    PRIMARY KEY (
        apply_id, apply_generation, projection_stage_generation
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_discards (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0
        AND apply_generation <= 9007199254740991
    ),
    projection_stage_generation INTEGER NOT NULL CHECK (
        projection_stage_generation > 0
        AND projection_stage_generation <= 9007199254740991
    ),
    transition_row_count INTEGER NOT NULL CHECK (transition_row_count = 0),
    transition_evidence_state TEXT NOT NULL CHECK (
        transition_evidence_state = 'aborted'
    ),
    PRIMARY KEY (
        apply_id, apply_generation, projection_stage_generation
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS logical_scene_named_apply_finalizations (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (
        apply_generation > 0
        AND apply_generation <= 9007199254740991
    ),
    projection_stage_generation INTEGER NOT NULL CHECK (
        projection_stage_generation > 0
        AND projection_stage_generation <= 9007199254740991
    ),
    expected_previous_publication_generation INTEGER NOT NULL CHECK (
        expected_previous_publication_generation > 0
        AND expected_previous_publication_generation <= 9007199254740991
    ),
    publication_generation INTEGER NOT NULL CHECK (
        publication_generation > 0
        AND publication_generation <= 9007199254740991
    ),
    private_materialization_content_digest TEXT NOT NULL,
    obsolete_command_surfaces_retired_evidence_digest TEXT NOT NULL,
    durable_public_pointer_content_digest TEXT NOT NULL,
    transition_row_count INTEGER NOT NULL CHECK (transition_row_count = 0),
    transition_evidence_state TEXT NOT NULL CHECK (
        transition_evidence_state = 'finalized'
    ),
    candidate_health_row_count INTEGER NOT NULL CHECK (
        candidate_health_row_count = 0
    ),
    candidate_health_state TEXT NOT NULL CHECK (
        candidate_health_state = 'healthy'
    ),
    PRIMARY KEY (
        apply_id, apply_generation, projection_stage_generation
    )
) WITHOUT ROWID, STRICT;

CREATE TRIGGER IF NOT EXISTS logical_scene_store_metadata_no_update
BEFORE UPDATE ON logical_scene_store_metadata
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_store_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_store_metadata_no_delete
BEFORE DELETE ON logical_scene_store_metadata
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_store_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_acceptance_no_update
BEFORE UPDATE ON canonical_empty_scene_acceptance
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_acceptance is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_acceptance_no_delete
BEFORE DELETE ON canonical_empty_scene_acceptance
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_acceptance is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_rebuild_no_update
BEFORE UPDATE ON canonical_empty_scene_rebuild
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_rebuild is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_rebuild_no_delete
BEFORE DELETE ON canonical_empty_scene_rebuild
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_rebuild is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_finalization_no_update
BEFORE UPDATE ON canonical_empty_scene_finalization
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_finalization is immutable');
END;

CREATE TRIGGER IF NOT EXISTS canonical_empty_scene_finalization_no_delete
BEFORE DELETE ON canonical_empty_scene_finalization
BEGIN
    SELECT RAISE(ABORT, 'canonical_empty_scene_finalization is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_stages_no_update
BEFORE UPDATE ON logical_scene_named_apply_stages
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_stages is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_stages_no_delete
BEFORE DELETE ON logical_scene_named_apply_stages
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_stages is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_activations_no_update
BEFORE UPDATE ON logical_scene_named_apply_activations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_activations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_activations_no_delete
BEFORE DELETE ON logical_scene_named_apply_activations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_activations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_discards_no_update
BEFORE UPDATE ON logical_scene_named_apply_discards
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_discards is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_discards_no_delete
BEFORE DELETE ON logical_scene_named_apply_discards
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_discards is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_finalizations_no_update
BEFORE UPDATE ON logical_scene_named_apply_finalizations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_finalizations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS logical_scene_named_apply_finalizations_no_delete
BEFORE DELETE ON logical_scene_named_apply_finalizations
BEGIN
    SELECT RAISE(ABORT, 'logical_scene_named_apply_finalizations is immutable');
END;

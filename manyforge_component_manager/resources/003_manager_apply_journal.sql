-- ManyForge I4 typed named-Apply journal, version 7.
--
-- This schema stores immutable named-Apply intents, their exact generation-
-- bound whole-Apply affected scopes, configuration-fence attempts, and
-- successful fence acceptances, complete runtime convergence intents, and
-- immutable candidate staged-ready sets, scene-stage intents, and complete
-- successful scene-stage evidence, irreversible activation selection, and
-- successful activation/retirement evidence, private selected-bundle
-- materialization, obsolete-command-surface disposition, public publication,
-- successful scene Finalize evidence, exact Kernel-fence release intent and
-- receipt, and the terminal successful outcome.
-- It is deliberately not a mutable phase/event journal.

PRAGMA application_id = 1296450128;
PRAGMA user_version = 7;

CREATE TABLE IF NOT EXISTS apply_journal_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 7),
    schema_artifact_digest TEXT NOT NULL,
    cell_id TEXT NOT NULL,
    maximum_apply_intents INTEGER NOT NULL
        CHECK (
            maximum_apply_intents > 0
            AND maximum_apply_intents <= 256
        ),
    maximum_fence_attempts INTEGER NOT NULL
        CHECK (
            maximum_fence_attempts > 0
            AND maximum_fence_attempts <= 256
        ),
    maximum_affected_scopes INTEGER NOT NULL
        CHECK (
            maximum_affected_scopes > 0
            AND maximum_affected_scopes <= 256
        ),
    maximum_fence_components INTEGER NOT NULL
        CHECK (
            maximum_fence_components > 0
            AND maximum_fence_components <= 256
        ),
    maximum_fence_domains INTEGER NOT NULL
        CHECK (maximum_fence_domains > 0 AND maximum_fence_domains <= 64),
    maximum_fence_bindings INTEGER NOT NULL
        CHECK (maximum_fence_bindings > 0 AND maximum_fence_bindings <= 256),
    maximum_fence_resources INTEGER NOT NULL
        CHECK (maximum_fence_resources > 0 AND maximum_fence_resources <= 1024),
    maximum_runtime_convergence_intents INTEGER NOT NULL
        CHECK (
            maximum_runtime_convergence_intents > 0
            AND maximum_runtime_convergence_intents <= 256
        ),
    maximum_runtime_staged_ready_sets INTEGER NOT NULL
        CHECK (
            maximum_runtime_staged_ready_sets > 0
            AND maximum_runtime_staged_ready_sets <= 4096
        ),
    maximum_scene_stage_intents INTEGER NOT NULL
        CHECK (
            maximum_scene_stage_intents > 0
            AND maximum_scene_stage_intents <= 256
        ),
    maximum_scene_staged_records INTEGER NOT NULL
        CHECK (
            maximum_scene_staged_records > 0
            AND maximum_scene_staged_records <= 256
        ),
    maximum_activation_selections INTEGER NOT NULL
        CHECK (
            maximum_activation_selections > 0
            AND maximum_activation_selections <= 256
        ),
    maximum_scene_activated_records INTEGER NOT NULL
        CHECK (
            maximum_scene_activated_records > 0
            AND maximum_scene_activated_records <= 256
        ),
    maximum_private_materializations INTEGER NOT NULL
        CHECK (
            maximum_private_materializations > 0
            AND maximum_private_materializations <= 256
        ),
    maximum_retired_command_surface_records INTEGER NOT NULL
        CHECK (
            maximum_retired_command_surface_records > 0
            AND maximum_retired_command_surface_records <= 256
        ),
    maximum_publication_receipts INTEGER NOT NULL
        CHECK (
            maximum_publication_receipts > 0
            AND maximum_publication_receipts <= 256
        ),
    maximum_scene_finalized_records INTEGER NOT NULL
        CHECK (
            maximum_scene_finalized_records > 0
            AND maximum_scene_finalized_records <= 256
        ),
    maximum_fence_release_intents INTEGER NOT NULL
        CHECK (
            maximum_fence_release_intents > 0
            AND maximum_fence_release_intents <= 256
        ),
    maximum_fence_released_records INTEGER NOT NULL
        CHECK (
            maximum_fence_released_records > 0
            AND maximum_fence_released_records <= 256
        ),
    maximum_terminal_outcomes INTEGER NOT NULL
        CHECK (
            maximum_terminal_outcomes > 0
            AND maximum_terminal_outcomes <= 256
        ),
    maximum_scene_transition_rows INTEGER NOT NULL
        CHECK (
            maximum_scene_transition_rows > 0
            AND maximum_scene_transition_rows <= 256
        ),
    maximum_main_database_page_image_bytes INTEGER NOT NULL
        CHECK (
            maximum_main_database_page_image_bytes > 0
            AND maximum_main_database_page_image_bytes
                <= 9223372036854775807
        )
) STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_intents (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL
        CHECK (
            apply_generation > 0
            AND apply_generation <= 9223372036854775807
        ),
    request_id TEXT NOT NULL,
    request_owner_epoch TEXT NOT NULL,
    source_label TEXT NOT NULL,
    correlation_id TEXT,
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL
        CHECK (
            predecessor_scene_revision_id > 0
            AND predecessor_scene_revision_id <= 9223372036854775807
        ),
    predecessor_scene_content_hash TEXT NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    intent_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (request_owner_epoch, request_id),
    UNIQUE (apply_id, apply_generation, intent_digest)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_affected_scopes (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    intent_digest TEXT NOT NULL,
    components_encoding TEXT NOT NULL,
    domains_encoding TEXT NOT NULL,
    bindings_encoding TEXT NOT NULL,
    resources_encoding TEXT NOT NULL,
    busy_disposition TEXT NOT NULL
        CHECK (busy_disposition = 'reject_if_affected_busy'),
    affected_scope_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, affected_scope_digest),
    FOREIGN KEY (apply_id, apply_generation, intent_digest)
        REFERENCES manager_apply_intents (
            apply_id, apply_generation, intent_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_fence_attempts (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    intent_digest TEXT NOT NULL,
    affected_scope_digest TEXT NOT NULL,
    request_id TEXT NOT NULL,
    request_owner_epoch TEXT NOT NULL,
    source_label TEXT NOT NULL,
    correlation_id TEXT,
    components_encoding TEXT NOT NULL,
    domains_encoding TEXT NOT NULL,
    bindings_encoding TEXT NOT NULL,
    resources_encoding TEXT NOT NULL,
    busy_disposition TEXT NOT NULL
        CHECK (busy_disposition = 'reject_if_affected_busy'),
    expected_retained_bundle_id TEXT NOT NULL,
    expected_graph_revision TEXT NOT NULL,
    expected_operation_resource_generation INTEGER NOT NULL
        CHECK (expected_operation_resource_generation >= 0),
    expected_fence_generation INTEGER NOT NULL
        CHECK (expected_fence_generation >= 0),
    active_configurations_encoding TEXT NOT NULL,
    reason TEXT NOT NULL,
    fence_attempt_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, fence_attempt_digest),
    FOREIGN KEY (apply_id, apply_generation, intent_digest)
        REFERENCES manager_apply_intents (
            apply_id, apply_generation, intent_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, affected_scope_digest)
        REFERENCES manager_apply_affected_scopes (
            apply_id, apply_generation, affected_scope_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_fence_acceptances (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    fence_attempt_digest TEXT NOT NULL,
    configuration_fence_id TEXT NOT NULL,
    configuration_fence_generation INTEGER NOT NULL
        CHECK (configuration_fence_generation > 0),
    captured_components_encoding TEXT NOT NULL,
    captured_domains_encoding TEXT NOT NULL,
    captured_bindings_encoding TEXT NOT NULL,
    captured_resources_encoding TEXT NOT NULL,
    accepted_at_ns INTEGER NOT NULL CHECK (accepted_at_ns >= 0),
    replay_status TEXT NOT NULL
        CHECK (replay_status IN ('accepted_new', 'replayed_exact')),
    acceptance_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (configuration_fence_id, configuration_fence_generation),
    UNIQUE (apply_id, apply_generation, acceptance_digest),
    FOREIGN KEY (apply_id, apply_generation, fence_attempt_digest)
        REFERENCES manager_apply_fence_attempts (
            apply_id, apply_generation, fence_attempt_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_runtime_convergence_intents (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    intent_digest TEXT NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    fence_acceptance_digest TEXT NOT NULL,
    predecessor_publication_generation INTEGER NOT NULL
        CHECK (predecessor_publication_generation > 0),
    predecessor_public_pointer_digest TEXT NOT NULL,
    runtime_units_encoding TEXT NOT NULL,
    runtime_convergence_intent_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (
        apply_id,
        apply_generation,
        runtime_convergence_intent_digest
    ),
    FOREIGN KEY (apply_id, apply_generation, intent_digest)
        REFERENCES manager_apply_intents (
            apply_id, apply_generation, intent_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, fence_acceptance_digest)
        REFERENCES manager_apply_fence_acceptances (
            apply_id, apply_generation, acceptance_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_runtime_staged_ready_sets (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    staged_ready_generation INTEGER NOT NULL
        CHECK (
            staged_ready_generation > 0
            AND staged_ready_generation <= 9223372036854775807
        ),
    runtime_convergence_intent_digest TEXT NOT NULL,
    predecessor_publication_generation INTEGER NOT NULL
        CHECK (predecessor_publication_generation > 0),
    predecessor_public_pointer_digest TEXT NOT NULL,
    evaluated_at_ns INTEGER NOT NULL CHECK (evaluated_at_ns > 0),
    runtime_evidence_encoding TEXT NOT NULL,
    staged_ready_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation, staged_ready_generation),
    UNIQUE (apply_id, apply_generation, staged_ready_digest),
    FOREIGN KEY (
        apply_id,
        apply_generation,
        runtime_convergence_intent_digest
    ) REFERENCES manager_apply_runtime_convergence_intents (
        apply_id,
        apply_generation,
        runtime_convergence_intent_digest
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_scene_stage_intents (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    runtime_staged_ready_digest TEXT NOT NULL,
    predecessor_publication_generation INTEGER NOT NULL
        CHECK (predecessor_publication_generation > 0),
    predecessor_public_pointer_digest TEXT NOT NULL,
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL
        CHECK (predecessor_scene_revision_id > 0),
    predecessor_scene_content_hash TEXT NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    native_transition_rows_encoding TEXT NOT NULL,
    scene_stage_intent_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, scene_stage_intent_digest),
    FOREIGN KEY (apply_id, apply_generation, runtime_staged_ready_digest)
        REFERENCES manager_apply_runtime_staged_ready_sets (
            apply_id, apply_generation, staged_ready_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_scene_staged_records (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    scene_stage_intent_digest TEXT NOT NULL,
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL
        CHECK (predecessor_scene_revision_id > 0),
    predecessor_scene_content_hash TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    result_state TEXT NOT NULL CHECK (result_state = 'staged'),
    transition_evidence_encoding TEXT NOT NULL,
    scene_staged_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, scene_staged_digest),
    UNIQUE (apply_id, apply_generation, projection_stage_generation),
    FOREIGN KEY (apply_id, apply_generation, scene_stage_intent_digest)
        REFERENCES manager_apply_scene_stage_intents (
            apply_id, apply_generation, scene_stage_intent_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_activation_selections (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    scene_staged_digest TEXT NOT NULL,
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL
        CHECK (predecessor_scene_revision_id > 0),
    predecessor_scene_content_hash TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    activation_selection_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, activation_selection_digest),
    FOREIGN KEY (apply_id, apply_generation, scene_staged_digest)
        REFERENCES manager_apply_scene_staged_records (
            apply_id, apply_generation, scene_staged_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_scene_activated_records (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    activation_selection_digest TEXT NOT NULL,
    predecessor_retained_bundle_id TEXT NOT NULL,
    predecessor_graph_revision TEXT NOT NULL,
    predecessor_scene_id TEXT NOT NULL,
    predecessor_scene_revision_id INTEGER NOT NULL
        CHECK (predecessor_scene_revision_id > 0),
    predecessor_scene_content_hash TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    result_state TEXT NOT NULL CHECK (result_state = 'activated'),
    transition_evidence_encoding TEXT NOT NULL,
    scene_activated_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, scene_activated_digest),
    FOREIGN KEY (apply_id, apply_generation, activation_selection_digest)
        REFERENCES manager_apply_activation_selections (
            apply_id, apply_generation, activation_selection_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_private_materializations (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    scene_activated_digest TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    retained_bundle_canonical_content_digest TEXT NOT NULL,
    retained_bundle_canonical_byte_size INTEGER NOT NULL
        CHECK (retained_bundle_canonical_byte_size > 0),
    private_materialization_content_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (
        apply_id,
        apply_generation,
        private_materialization_content_digest
    ),
    FOREIGN KEY (apply_id, apply_generation, scene_activated_digest)
        REFERENCES manager_apply_scene_activated_records (
            apply_id, apply_generation, scene_activated_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_retired_command_surfaces (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    private_materialization_content_digest TEXT NOT NULL,
    command_surfaces_encoding TEXT NOT NULL,
    retired_command_surfaces_evidence_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (
        apply_id,
        apply_generation,
        retired_command_surfaces_evidence_digest
    ),
    FOREIGN KEY (
        apply_id,
        apply_generation,
        private_materialization_content_digest
    ) REFERENCES manager_apply_private_materializations (
        apply_id,
        apply_generation,
        private_materialization_content_digest
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_publication_receipts (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    retired_command_surfaces_evidence_digest TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    expected_previous_publication_generation INTEGER NOT NULL
        CHECK (expected_previous_publication_generation > 0),
    private_materialization_content_digest TEXT NOT NULL,
    publication_generation INTEGER NOT NULL
        CHECK (publication_generation > 0),
    durable_public_pointer_content_digest TEXT NOT NULL,
    publication_receipt_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (publication_generation),
    UNIQUE (apply_id, apply_generation, publication_receipt_digest),
    FOREIGN KEY (
        apply_id,
        apply_generation,
        retired_command_surfaces_evidence_digest
    ) REFERENCES manager_apply_retired_command_surfaces (
        apply_id,
        apply_generation,
        retired_command_surfaces_evidence_digest
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_scene_finalized_records (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    publication_receipt_digest TEXT NOT NULL,
    projection_stage_generation INTEGER NOT NULL
        CHECK (projection_stage_generation > 0),
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    transition_evidence_encoding TEXT NOT NULL,
    candidate_aggregate_health_encoding TEXT NOT NULL,
    scene_finalized_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, scene_finalized_digest),
    FOREIGN KEY (apply_id, apply_generation, publication_receipt_digest)
        REFERENCES manager_apply_publication_receipts (
            apply_id, apply_generation, publication_receipt_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_fence_release_intents (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    scene_finalized_digest TEXT NOT NULL,
    fence_acceptance_digest TEXT NOT NULL,
    release_request_encoding TEXT NOT NULL,
    fence_release_intent_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, fence_release_intent_digest),
    FOREIGN KEY (apply_id, apply_generation, scene_finalized_digest)
        REFERENCES manager_apply_scene_finalized_records (
            apply_id, apply_generation, scene_finalized_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, fence_acceptance_digest)
        REFERENCES manager_apply_fence_acceptances (
            apply_id, apply_generation, acceptance_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_fence_released_records (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    fence_release_intent_digest TEXT NOT NULL,
    released_fence_encoding TEXT NOT NULL,
    fence_released_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, fence_released_digest),
    FOREIGN KEY (apply_id, apply_generation, fence_release_intent_digest)
        REFERENCES manager_apply_fence_release_intents (
            apply_id, apply_generation, fence_release_intent_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_terminal_outcomes (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    fence_released_digest TEXT NOT NULL,
    scene_finalized_digest TEXT NOT NULL,
    publication_receipt_digest TEXT NOT NULL,
    candidate_retained_bundle_id TEXT NOT NULL,
    candidate_graph_revision TEXT NOT NULL,
    candidate_scene_id TEXT NOT NULL,
    candidate_scene_revision_id INTEGER NOT NULL
        CHECK (candidate_scene_revision_id > 0),
    candidate_scene_content_hash TEXT NOT NULL,
    outcome_state TEXT NOT NULL CHECK (outcome_state = 'succeeded'),
    terminal_outcome_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, terminal_outcome_digest),
    FOREIGN KEY (apply_id, apply_generation, fence_released_digest)
        REFERENCES manager_apply_fence_released_records (
            apply_id, apply_generation, fence_released_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, scene_finalized_digest)
        REFERENCES manager_apply_scene_finalized_records (
            apply_id, apply_generation, scene_finalized_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, publication_receipt_digest)
        REFERENCES manager_apply_publication_receipts (
            apply_id, apply_generation, publication_receipt_digest
        )
) WITHOUT ROWID, STRICT;

CREATE TRIGGER IF NOT EXISTS apply_journal_metadata_no_update
BEFORE UPDATE ON apply_journal_metadata
BEGIN
    SELECT RAISE(ABORT, 'apply_journal_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS apply_journal_metadata_no_delete
BEFORE DELETE ON apply_journal_metadata
BEGIN
    SELECT RAISE(ABORT, 'apply_journal_metadata is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_intents_no_update
BEFORE UPDATE ON manager_apply_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_intents_no_delete
BEFORE DELETE ON manager_apply_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_affected_scopes_no_update
BEFORE UPDATE ON manager_apply_affected_scopes
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_affected_scopes is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_affected_scopes_no_delete
BEFORE DELETE ON manager_apply_affected_scopes
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_affected_scopes is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_attempts_no_update
BEFORE UPDATE ON manager_apply_fence_attempts
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_attempts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_attempts_no_delete
BEFORE DELETE ON manager_apply_fence_attempts
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_attempts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_acceptances_no_update
BEFORE UPDATE ON manager_apply_fence_acceptances
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_acceptances is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_acceptances_no_delete
BEFORE DELETE ON manager_apply_fence_acceptances
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_acceptances is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_runtime_convergence_intents_no_update
BEFORE UPDATE ON manager_apply_runtime_convergence_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_runtime_convergence_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_runtime_convergence_intents_no_delete
BEFORE DELETE ON manager_apply_runtime_convergence_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_runtime_convergence_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_runtime_staged_ready_sets_no_update
BEFORE UPDATE ON manager_apply_runtime_staged_ready_sets
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_runtime_staged_ready_sets is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_runtime_staged_ready_sets_no_delete
BEFORE DELETE ON manager_apply_runtime_staged_ready_sets
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_runtime_staged_ready_sets is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_stage_intents_no_update
BEFORE UPDATE ON manager_apply_scene_stage_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_stage_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_stage_intents_no_delete
BEFORE DELETE ON manager_apply_scene_stage_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_stage_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_staged_records_no_update
BEFORE UPDATE ON manager_apply_scene_staged_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_staged_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_staged_records_no_delete
BEFORE DELETE ON manager_apply_scene_staged_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_staged_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_activation_selections_no_update
BEFORE UPDATE ON manager_apply_activation_selections
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_activation_selections is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_activation_selections_no_delete
BEFORE DELETE ON manager_apply_activation_selections
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_activation_selections is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_activated_records_no_update
BEFORE UPDATE ON manager_apply_scene_activated_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_activated_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_activated_records_no_delete
BEFORE DELETE ON manager_apply_scene_activated_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_activated_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_private_materializations_no_update
BEFORE UPDATE ON manager_apply_private_materializations
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_private_materializations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_private_materializations_no_delete
BEFORE DELETE ON manager_apply_private_materializations
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_private_materializations is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_retired_command_surfaces_no_update
BEFORE UPDATE ON manager_apply_retired_command_surfaces
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_retired_command_surfaces is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_retired_command_surfaces_no_delete
BEFORE DELETE ON manager_apply_retired_command_surfaces
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_retired_command_surfaces is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_publication_receipts_no_update
BEFORE UPDATE ON manager_apply_publication_receipts
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_publication_receipts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_publication_receipts_no_delete
BEFORE DELETE ON manager_apply_publication_receipts
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_publication_receipts is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_finalized_records_no_update
BEFORE UPDATE ON manager_apply_scene_finalized_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_finalized_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_scene_finalized_records_no_delete
BEFORE DELETE ON manager_apply_scene_finalized_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_scene_finalized_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_release_intents_no_update
BEFORE UPDATE ON manager_apply_fence_release_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_release_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_release_intents_no_delete
BEFORE DELETE ON manager_apply_fence_release_intents
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_release_intents is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_released_records_no_update
BEFORE UPDATE ON manager_apply_fence_released_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_released_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_fence_released_records_no_delete
BEFORE DELETE ON manager_apply_fence_released_records
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_fence_released_records is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_terminal_outcomes_no_update
BEFORE UPDATE ON manager_apply_terminal_outcomes
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_terminal_outcomes is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_terminal_outcomes_no_delete
BEFORE DELETE ON manager_apply_terminal_outcomes
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_terminal_outcomes is immutable');
END;

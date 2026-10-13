-- ManyForge UA4 typed named-Apply journal, version 17.
--
-- This schema stores immutable named-Apply intents, including the exact UA2A
-- prepared activation each operation accepted (captured source-closure digest,
-- candidate Deployment identity, prepared Program-binding identity and the
-- prepared-activation transfer handle), their exact generation-
-- bound whole-Apply affected scopes with their per-component runtime delta,
-- configuration-fence attempts, and
-- successful fence acceptances, complete runtime convergence intents, and
-- immutable candidate staged-ready sets, Scene-owned maintenance completion,
-- private selected-bundle materialization, public publication,
-- exact Kernel-fence release intent and
-- receipt, and exactly one terminal outcome per operation: either the
-- successful publication, or the first retained failure with its effect
-- certainty. A terminal failure is never repaired, retried or rolled back.
-- It is deliberately not a mutable phase/event journal.

PRAGMA application_id = 1296450128;
PRAGMA user_version = 17;

CREATE TABLE IF NOT EXISTS apply_journal_metadata (
    singleton INTEGER PRIMARY KEY CHECK (singleton = 1),
    schema_id TEXT NOT NULL,
    schema_version INTEGER NOT NULL CHECK (schema_version = 17),
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
    maximum_private_materializations INTEGER NOT NULL
        CHECK (
            maximum_private_materializations > 0
            AND maximum_private_materializations <= 256
        ),
    maximum_publication_receipts INTEGER NOT NULL
        CHECK (
            maximum_publication_receipts > 0
            AND maximum_publication_receipts <= 256
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
    captured_input_digest TEXT NOT NULL,
    deployment_artifact_id TEXT NOT NULL,
    deployment_revision TEXT NOT NULL,
    -- NULL exactly for a prepared non-executable Program outcome (H1a).
    program_binding_id TEXT
        CHECK (program_binding_id IS NULL OR length(program_binding_id) = 71),
    prepared_activation_id TEXT NOT NULL,
    prepared_activation_receipt_json TEXT NOT NULL
        CHECK (
            length(prepared_activation_receipt_json) > 0
            AND length(prepared_activation_receipt_json) <= 65536
        ),
    expected_admission_generation INTEGER
        CHECK (
            expected_admission_generation IS NULL
            OR (
                expected_admission_generation > 0
                AND expected_admission_generation <= 9007199254740991
            )
        ),
    parent_authoring_claim BLOB,
    -- NULL when the accepted Apply has no checked observer launch inputs.
    isaac_pose_observer_launch_facts BLOB
        CHECK (isaac_pose_observer_launch_facts IS NULL
               OR length(isaac_pose_observer_launch_facts) > 0),
    intent_digest TEXT NOT NULL,
    CHECK (
        (expected_admission_generation IS NOT NULL)
        <> (parent_authoring_claim IS NOT NULL)
    ),
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (request_owner_epoch, request_id),
    UNIQUE (apply_id, apply_generation, intent_digest)
) WITHOUT ROWID, STRICT;

-- Q91 parent/child bookkeeping in the same immutable Apply journal. The
-- original intent and this complete plan are inserted atomically before effects.
CREATE TABLE IF NOT EXISTS manager_apply_bring_up_plans (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    intent_digest TEXT NOT NULL,
    plan_encoding BLOB NOT NULL CHECK (length(plan_encoding) > 0),
    plan_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, plan_digest),
    FOREIGN KEY (apply_id, apply_generation, intent_digest)
        REFERENCES manager_apply_intents (apply_id, apply_generation, intent_digest)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_bring_up_child_proofs (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    plan_digest TEXT NOT NULL,
    component_id TEXT NOT NULL,
    phase TEXT NOT NULL CHECK (phase IN ('accepted', 'holding', 'terminal')),
    result_encoding BLOB NOT NULL CHECK (length(result_encoding) > 0),
    proof_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation, component_id, phase),
    FOREIGN KEY (apply_id, apply_generation, plan_digest)
        REFERENCES manager_apply_bring_up_plans (apply_id, apply_generation, plan_digest)
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
    component_delta_encoding TEXT NOT NULL
        CHECK (length(component_delta_encoding) BETWEEN 2 AND 262144),
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

CREATE TABLE IF NOT EXISTS manager_apply_private_materializations (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    authoritative_scene_snapshot_digest TEXT NOT NULL,
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
    FOREIGN KEY (apply_id, apply_generation)
        REFERENCES manager_apply_intents (apply_id, apply_generation)
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_publication_receipts (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
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
        private_materialization_content_digest
    ) REFERENCES manager_apply_private_materializations (
        apply_id,
        apply_generation,
        private_materialization_content_digest
    )
) WITHOUT ROWID, STRICT;

CREATE TABLE IF NOT EXISTS manager_apply_fence_release_intents (
    apply_id TEXT NOT NULL,
    apply_generation INTEGER NOT NULL CHECK (apply_generation > 0),
    scene_completion_digest TEXT NOT NULL,
    fence_acceptance_digest TEXT NOT NULL,
    release_request_encoding TEXT NOT NULL,
    fence_release_intent_digest TEXT NOT NULL,
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, fence_release_intent_digest),
    FOREIGN KEY (apply_id, apply_generation, scene_completion_digest)
        REFERENCES manager_apply_publication_receipts (
            apply_id, apply_generation, publication_receipt_digest
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
    fence_released_digest TEXT,
    scene_completion_digest TEXT,
    publication_receipt_digest TEXT,
    candidate_retained_bundle_id TEXT,
    candidate_graph_revision TEXT,
    candidate_scene_id TEXT,
    candidate_scene_revision_id INTEGER
        CHECK (
            candidate_scene_revision_id IS NULL
            OR (
                candidate_scene_revision_id > 0
                AND candidate_scene_revision_id <= 9223372036854775807
            )
        ),
    candidate_scene_content_hash TEXT,
    intent_digest TEXT,
    failure_phase TEXT,
    failure_code TEXT,
    failure_message TEXT,
    effect_disposition TEXT
        CHECK (
            effect_disposition IS NULL
            OR effect_disposition IN ('known', 'unknown')
        ),
    outcome_state TEXT NOT NULL
        CHECK (outcome_state IN ('succeeded', 'failed', 'unknown_effect')),
    terminal_outcome_digest TEXT NOT NULL,
    CHECK (
        (
            outcome_state = 'succeeded'
            AND fence_released_digest IS NOT NULL
            AND scene_completion_digest IS NOT NULL
            AND publication_receipt_digest IS NOT NULL
            AND candidate_retained_bundle_id IS NOT NULL
            AND candidate_graph_revision IS NOT NULL
            AND candidate_scene_id IS NOT NULL
            AND candidate_scene_revision_id IS NOT NULL
            AND candidate_scene_content_hash IS NOT NULL
            AND intent_digest IS NULL
            AND failure_phase IS NULL
            AND failure_code IS NULL
            AND failure_message IS NULL
            AND effect_disposition IS NULL
        )
        OR (
            outcome_state <> 'succeeded'
            AND fence_released_digest IS NULL
            AND scene_completion_digest IS NULL
            AND publication_receipt_digest IS NULL
            AND candidate_retained_bundle_id IS NULL
            AND candidate_graph_revision IS NULL
            AND candidate_scene_id IS NULL
            AND candidate_scene_revision_id IS NULL
            AND candidate_scene_content_hash IS NULL
            AND intent_digest IS NOT NULL
            AND failure_phase IS NOT NULL
            AND failure_code IS NOT NULL
            AND failure_message IS NOT NULL
            AND effect_disposition IS NOT NULL
            AND (
                (outcome_state = 'failed' AND effect_disposition = 'known')
                OR (
                    outcome_state = 'unknown_effect'
                    AND effect_disposition = 'unknown'
                )
            )
        )
    ),
    PRIMARY KEY (apply_id, apply_generation),
    UNIQUE (apply_id, apply_generation, terminal_outcome_digest),
    FOREIGN KEY (apply_id, apply_generation, fence_released_digest)
        REFERENCES manager_apply_fence_released_records (
            apply_id, apply_generation, fence_released_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, scene_completion_digest)
        REFERENCES manager_apply_publication_receipts (
            apply_id, apply_generation, publication_receipt_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, publication_receipt_digest)
        REFERENCES manager_apply_publication_receipts (
            apply_id, apply_generation, publication_receipt_digest
        ),
    FOREIGN KEY (apply_id, apply_generation, intent_digest)
        REFERENCES manager_apply_intents (
            apply_id, apply_generation, intent_digest
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

CREATE TRIGGER IF NOT EXISTS manager_apply_bring_up_plans_no_update
BEFORE UPDATE ON manager_apply_bring_up_plans
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_bring_up_plans is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_bring_up_plans_no_delete
BEFORE DELETE ON manager_apply_bring_up_plans
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_bring_up_plans is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_bring_up_child_proofs_no_update
BEFORE UPDATE ON manager_apply_bring_up_child_proofs
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_bring_up_child_proofs is immutable');
END;

CREATE TRIGGER IF NOT EXISTS manager_apply_bring_up_child_proofs_no_delete
BEFORE DELETE ON manager_apply_bring_up_child_proofs
BEGIN
    SELECT RAISE(ABORT, 'manager_apply_bring_up_child_proofs is immutable');
END;

import { describe, expect, it } from 'vitest';
import nativeBindingFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import {
  StructuralContractError,
  parseAuthoringWorkspaceSnapshot,
  parseAppliedProgramSelection,
  parseCapabilityCommandPreviewResponse,
  parseCatalogResponse,
  parseCurrentScenarioResponse,
  parseDeploymentCandidateOperationResponse,
  parseDeploymentWorkspaceResponse,
  parseExecutionControlStatus,
  parseExecutionIdentity,
  parseExecutionOwnerStatus,
  parseHealthResponse,
  parseHeadlessRuntimeSnapshot,
  parseHistorianPage,
  parseHistorianDetail,
  parseHistorianDiscovery,
  parseManagerDiffProjection,
  parseManagerGraphProjection,
  parseManagerStatusProjection,
  parseOperatorAlarmSnapshot,
  parseOperationObservation,
  parseRuntimeScaleControlAccepted,
  parseRuntimeScaleControlStatus,
  parseMotionPreviewResponse,
  parseNodeEnvironmentResponse,
  parseLogicalSceneProjection,
  parseManagerApplyResponse,
  parseProgramWorkspaceResponse,
  parseProgramWorkspaceTransitionResponse,
  parseProgramRunStatus,
  parseProgramCycleStatus,
  parseProgramRuntimeControlProjection,
  parseScenarioCatalogResponse,
  parseScenarioDocumentIndexResponse,
  parseScenarioDocumentConversionResponse,
  parseScenarioDocumentExportResponse,
  parseScenarioDocumentLoadResponse,
  parseScenarioDocumentSaveResponse,
  parseSceneRuntimeProjection,
  parseSceneMeshAssetMetadata,
  parseProgramDraftMeshAssetMetadata,
  parseSceneTransactionResponse,
  parseSceneUrdfManifest,
  parseSourceModelProjection,
} from './parsers';

function authoringWorkspaceFixture(): Record<string, unknown> {
  const program = workspaceFixture();
  const deployment = deploymentWorkspaceFixture();
  const programDraft = program.draft as Record<string, unknown>;
  const programSaved = program.saved as Record<string, unknown>;
  const deploymentDraft = deployment.draft as Record<string, unknown>;
  return {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v1',
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: 7,
    program: { ...programDraft, saved: programSaved },
    deployment: {
      draftId: deploymentDraft.id,
      draftRevision: deploymentDraft.revision,
      document: deploymentDraft.document,
      savedPath: '/workspace/deployment.fixture.json',
      savedDocument: deploymentDraft.document,
      differsFromSaved: false,
    },
    project: {
      document_path: '/workspace/cell',
      document_id: 'cell',
      document_name: 'Cell',
      document_description: '',
      source_diagnostics: [],
      source_document_digest: null,
      program_role_assignment: null,
      typed_parameter_overrides: {},
      source_program_digest: null,
      source_deployment_digest: null,
    },
  };
}

describe('shared authoring workspace transport', () => {
  it('parses both Drafts and project facts as one closed publication', () => {
    const parsed = parseAuthoringWorkspaceSnapshot(authoringWorkspaceFixture());
    expect(parsed.workspaceRevision).toBe(7);
    expect(parsed.program.draft.draftRevision).toBe(2);
    expect(parsed.deployment.draftRevision).toBe(4);
    expect(parsed.project.documentPath).toBe('/workspace/cell');
  });

  it('rejects added fields and an untruthful saved comparison', () => {
    const added = authoringWorkspaceFixture();
    Object.assign(added, { applied: {} });
    expect(() => parseAuthoringWorkspaceSnapshot(added)).toThrow(/exactly/);

    const untruthful = authoringWorkspaceFixture();
    const deployment = untruthful.deployment as Record<string, unknown>;
    deployment.savedDocument = { deployment_revision: 'different' };
    expect(() => parseAuthoringWorkspaceSnapshot(untruthful)).toThrow(
      /truthful Draft\/Saved comparison/,
    );
  });
});

describe('editable Scenario document transport', () => {
  it('parses ordinary saved documents with explicit advisory state', () => {
    const parsed = parseScenarioDocumentIndexResponse({
      api_version: 'manyforge.scenario_document_index.v1',
      directory: '/workspace/examples',
      entries: [{
        source_kind: 'saved',
        path: '/workspace/examples/cell',
        scenario_id: 'cell',
        name: 'Cell',
        description: 'Editable cell',
        saved_manyforge_version: '1.0.0',
        display_status: 'changed',
        status_detail: 'Content changed since the recorded status.',
      }],
    });

    expect(parsed.directory).toBe('/workspace/examples');
    expect(parsed.entries.map((entry) => entry.sourceKind)).toEqual(['saved']);
    expect(parsed.entries[0]?.displayStatus).toBe('changed');
  });

  it('parses Save and editor-only Load outcomes without inventing activation', () => {
    const saved = parseScenarioDocumentSaveResponse({
      status: 'saved',
      path: '/workspace/cell',
      scenario_id: 'cell',
      last_known_status: 'unknown',
      workspace: workspaceFixture(),
    });
    const opened = parseScenarioDocumentLoadResponse({
      status: 'opened',
      path: '/workspace/cell',
      scenario_id: 'cell',
      activation: null,
      incomplete: false,
      diagnostics: [],
      source_diagnostics: [],
      workspace: workspaceFixture(),
    });

    expect(saved.lastKnownStatus).toBe('unknown');
    expect(opened.path).toBe('/workspace/cell');
    expect(opened.activation).toBeNull();
    expect(() => parseScenarioDocumentLoadResponse({
      status: 'loaded',
      path: '/workspace/cell',
      scenario_id: 'cell',
      activation: null,
      incomplete: false,
      diagnostics: [],
      source_diagnostics: [],
      workspace: workspaceFixture(),
    })).toThrow('status');
  });

  it('parses explicit legacy conversion without treating it as a Load', () => {
    expect(parseScenarioDocumentConversionResponse({
      status: 'converted',
      source_path: '/workspace/legacy.scenario.yaml',
      path: '/workspace/cell',
    })).toEqual({
      status: 'converted',
      sourcePath: '/workspace/legacy.scenario.yaml',
      path: '/workspace/cell',
    });
  });

  it('parses an explicit portable project export', () => {
    expect(parseScenarioDocumentExportResponse({
      status: 'exported',
      path: '/workspace/exports/cell',
    })).toEqual({
      status: 'exported',
      path: '/workspace/exports/cell',
    });
  });
});

it('bounds discovery by encoded bytes and requires the exact continuation', () => {
  const page = { schema_version: 'manyforge.historian_discovery.v1', kind: 'channel',
    values: ['manyforge.composer'], truncated: true, next_after: 'manyforge.composer' };
  expect(parseHistorianDiscovery(page).nextAfter).toBe('manyforge.composer');
  for (const patch of [{ next_after: 'other' }, { values: [] },
    { values: ['é'.repeat(513)], next_after: 'é'.repeat(513) },
    { values: ['e\u0301'], next_after: 'e\u0301' }]) {
    expect(() => parseHistorianDiscovery({ ...page, ...patch })).toThrow(StructuralContractError);
  }
});

function programFixture(): Record<string, unknown> {
  return {
    api_version: 'manyforge.program.v2',
    program_id: 'fixture',
    program_revision: `sha256:${'a'.repeat(64)}`,
    program_profile: 'composable-cell-v1',
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v2',
      content_version: '2.3.0',
    },
    component_roles: {},
    target_roles: {},
    parameters: [{
      parameter_id: 'duration',
      type: 'float64',
      default: 0.5,
      runtime_mutability: 'before_run',
    }],
    blackboard: { namespaces: [], keys: [] },
    tree: {
      node_id: 'timer',
      instance_id: 'wait',
      parameters: {},
      value_sources: {
        duration_s: { kind: 'program_parameter', reference: 'duration' },
      },
      children: [],
    },
  };
}

function workspaceFixture(): Record<string, unknown> {
  const program = programFixture();
  const programFields = { ...program };
  const tree = programFields.tree;
  delete programFields.program_revision;
  delete programFields.tree;
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    draft: {
      draftId: 'draft-1',
      draftRevision: 2,
      programLoaded: true,
      authoring: {
        schema_version: 'manyforge.composer_authoring_forest.v1',
        ...programFields,
        forest: [tree],
        positions: {},
      },
      candidateAvailable: true,
      programRevision: program.program_revision,
      program,
      diagnostics: [],
    },
    applicability: {
      applyReady: true,
      affectedInstanceIds: [],
      unavailableReason: null,
    },
    saved: {
      available: true,
      matchesDraft: false,
      path: '/workspace/fixture.program.yaml',
      programRevision: `sha256:${'b'.repeat(64)}`,
    },
    applied: {
      available: false,
      matchesDraft: null,
      programRevision: null,
      unavailableReason: {
        code: 'program_apply_unavailable',
        message: 'Apply waits for N7.',
        restorationGates: ['N7'],
      },
    },
    hasUnsavedChanges: true,
    hasUnappliedChanges: null,
  };
}

function deploymentWorkspaceFixture(): Record<string, unknown> {
  const digestA = `sha256:${'a'.repeat(64)}`;
  const digestB = `sha256:${'b'.repeat(64)}`;
  const digestC = `sha256:${'c'.repeat(64)}`;
  return {
    schemaVersion: 'manyforge.composer_deployment_workspace.v1',
    draft: {
      id: 'deployment-draft-1',
      revision: 4,
      document: {
        api_version: 'manyforge.deployment.v1',
        deployment_artifact_id: 'deployment.fixture',
        deployment_revision: digestA,
        graph: { runtime_units: [], model_instances: [], controllable_components: [],
          control_domains: [], control_bindings: [], component_attachments: [] },
      },
      deploymentArtifactId: 'deployment.fixture',
      deploymentRevision: digestA,
      graphRevision: digestC,
      differsFromSaved: false,
    },
    saved: { path: '/workspace/deployment.fixture.json', deploymentRevision: digestA },
    managerApplied: { deploymentRevision: digestA, scenarioId: 'fixture' },
    runAdmission: { ready: true, reason: null },
    binding: programBindingFixture('fixture-program', digestB, digestA),
    scenarioCandidates: [{
      scenarioId: 'fixture',
      scenarioCatalogRevision: digestB,
      deploymentArtifactId: 'deployment.fixture',
      deploymentRevision: digestA,
      retainedBundleId: digestB,
      programId: 'fixture-program',
      programRevision: digestB,
      programBindingId: digestA,
      description: 'Fixture Scenario',
    }],
    savedScenarioCandidates: [{
      scenarioId: 'fixture',
      scenarioCatalogRevision: digestB,
      deploymentArtifactId: 'deployment.fixture',
      deploymentRevision: digestA,
      retainedBundleId: digestB,
      programId: 'fixture-program',
      programRevision: digestB,
      programBindingId: digestA,
      description: 'Fixture Scenario',
    }],
    unavailableReasons: [],
  };
}

function programBindingFixture(
  programId: string,
  programRevision: string,
  programBindingId: string,
): Record<string, unknown> {
  const binding = structuredClone(nativeBindingFixture) as Record<string, unknown>;
  binding.program_binding_id = programBindingId;
  binding.program_id = programId;
  binding.program_revision = programRevision;
  binding.retained_bundle_id = `sha256:${'b'.repeat(64)}`;
  binding.graph_revision = `sha256:${'c'.repeat(64)}`;
  (binding.node_environment as Record<string, unknown>).environment_digest =
    `sha256:${'a'.repeat(64)}`;
  return binding;
}

function sourceSectionFixture(): Record<string, unknown> {
  return {
    available: true,
    freshness: 'fresh',
    known_gaps: [],
    payload: { schema_version: 'fixture.v1' },
    source_generation: 1,
    source_owner_epoch: 'source-epoch-1',
    source_owner_id: 'source-owner',
    receiver_age_ns: 100,
  };
}

function runtimeSnapshotFixture(): Record<string, unknown> {
  return {
    schema_version: 'manyforge.headless_runtime_snapshot.v1',
    complete: true,
    projection_generation: 4,
    projection_owner_epoch: 'projection-epoch-1',
    projection_owner_id: 'headless-api',
    reasons: [],
    unavailable_sections: [],
    sections: {
      manager: sourceSectionFixture(),
      logical_scene: sourceSectionFixture(),
      scene_runtime: sourceSectionFixture(),
    },
  };
}

function alarmSnapshotFixture(): Record<string, unknown> {
  const alarm = {
    condition_id: 'condition-1',
    activation_id: 'activation-1',
    record_revision: 3,
    activated_at_ns: 100,
    definition_id: 'manyforge.test.condition',
    lifecycle_state: 'active_unacknowledged',
    active: true,
    acknowledged: false,
    priority: 'high',
    alarm_class: 'runtime_health',
    title: 'Test alarm',
    message: 'A condition requires attention.',
    consequence: 'Runtime state may be incomplete.',
    operator_response: 'Inspect the source.',
    occurrence_count: 2,
    requires_motion_attention: false,
    subject: {
      subject_kind: 'cell',
      subject_id: 'cell-1',
      resource_ids: [],
    },
    details: { source: 'fixture' },
  };
  return {
    schema_version: 'manyforge.operator_alarm_snapshot.v2',
    owner_id: 'alarm-owner',
    owner_epoch: 'alarm-epoch-1',
    generation: 2,
    state_revision: 3,
    reconciled: true,
    health: { state: 'available', details: {} },
    live_count: 1,
    unacknowledged_count: 1,
    active_count: 1,
    active_acknowledged_count: 0,
    motion_attention_count: 0,
    unacknowledged_membership: [{
      condition_id: 'condition-1',
      activation_id: 'activation-1',
      record_revision: 3,
    }],
    alarms: [alarm],
    truncated: false,
  };
}

function structuralManifestFixture(): Record<string, unknown> {
  return {
    cell_id: 'fixture-cell',
    graph_revision: 'graph-7',
    retained_bundle_id: `sha256:${'a'.repeat(64)}`,
    viewer_model_revision: `sha256:${'b'.repeat(64)}`,
    model_key: 'model-key-123',
    root_link: 'base_link',
    projection: {
      kind: 'retained_neutral',
      source: 'active_retained_viewer_model',
      root_frame: 'base_link',
      position_unit: 'm',
      angle_unit: 'rad',
      quaternion_order: 'xyzw',
      joint_coordinate_policy: 'zero_independent_with_retained_mimic',
      operational_validity: 'not_asserted',
      runtime_state_used: false,
    },
    links: [
      {
        name: 'base_link',
        neutral_transform: {
          relative_to: 'base_link',
          position_m: [0, 0, 0],
          orientation_xyzw: [0, 0, 0, 1],
        },
        visuals: [],
        collisions: [],
      },
    ],
    joints: [],
  };
}

function sourceModelFixture(): Record<string, unknown> {
  return {
    schema_version: 'manyforge.source_model_projection.v2',
    cell_id: 'cell-a',
    retained_bundle_id: 'bundle-1',
    graph_revision: 'graph-1',
    model_map_contract: null,
    model_map_product_id: null,
    occurrences: [],
    targets: [],
    source: {
      available: true,
      freshness: 'fresh',
      known_gaps: [],
      projection_generation: 1,
      projection_owner_epoch: 'source-projection-1',
      receiver_age_ns: 10,
      selection_projection_generation: 2,
      selection_source_content_digest: `sha256:${'a'.repeat(64)}`,
      selection_source_generation: 3,
      selection_source_owner_epoch: 'manager-epoch-1',
      selection_source_owner_id: 'component-manager.cell-a',
      source_content_digest: `sha256:${'b'.repeat(64)}`,
      source_generation: 4,
      source_owner_epoch: 'source-owner-epoch-1',
      source_owner_id: 'headless-api.source-model',
      source_owner_kind: 'source_model_projection',
    },
  };
}

function projectionRevisionFixture(revision = 1): Record<string, unknown> {
  return {
    retained_bundle_id: 'bundle-1',
    graph_revision: 'graph-1',
    scene_revision: {
      scene_id: 'scene-1',
      revision_id: revision,
      content_hash: `sha256:${'c'.repeat(64)}`,
    },
  };
}

function managerApplyRecordFixture(): Record<string, unknown> {
  return {
    apply_generation: 3,
    apply_id: 'apply-3',
    candidate_graph_revision: 'graph-2',
    candidate_retained_bundle_id: 'bundle-2',
    committed: false,
    expected_active_projection_revision: projectionRevisionFixture(),
    fence: {
      acceptance_digest: `sha256:${'d'.repeat(64)}`,
      configuration_fence_generation: 2,
      configuration_fence_id: 'fence-2',
      fence_attempt_digest: `sha256:${'e'.repeat(64)}`,
      scope: { configurations: ['configuration-1'], resources: ['resource-1'] },
      state: 'held',
    },
    incomplete: true,
    intent_digest: `sha256:${'f'.repeat(64)}`,
    phase: 'fence_held',
    request: {
      correlation_id: null,
      request_id: 'request-3',
      request_owner_epoch: 'browser-epoch-3',
      source_label: 'composer_ui',
    },
    terminal_state: null,
  };
}

function logicalSceneFixture(): Record<string, unknown> {
  return {
    api_version: 'manyforge.logical_scene_source.v1',
    access: {
      active_projection: projectionRevisionFixture(),
      phase: 'finalized',
      projection_leases_open: true,
      scene_writes_open: true,
    },
    snapshot: {
      carried_object_attachments: [],
      cell_id: 'cell-a',
      configured_collision_policy: {},
      objects: [{
        collision_shapes: [{
          type: 'box',
          dimensions_m: { x: 0.1, y: 0.2, z: 0.3 },
        }],
        placement: {
          kind: 'cell_fixed',
          cell_id: 'cell-a',
          pose_in_cell: {
            position_m: { x: 0.4, y: 0, z: 0.5 },
            orientation_xyzw: [0, 0, 0, 1],
          },
        },
        provider_evidence: null,
        scene_resource_id: 'part',
        support_contacts: [],
      }],
      policy_epoch: 1,
      revision: (projectionRevisionFixture().scene_revision as Record<string, unknown>),
      topology_epoch: 1,
      transaction_sequence: 1,
    },
  };
}

function sceneRuntimeFixture(): Record<string, unknown> {
  return {
    api_version: 'manyforge.scene_runtime_source.v1',
    cell_id: 'cell-a',
    state: {
      active_replay_id: null,
      api_version: 'manyforge.scene_runtime_state.v1',
      counters: {
        buffer_replaced: 0,
        buffered: 0,
        closed: 0,
        failed: 0,
        opened: 1,
        replayed: 0,
        retired: 0,
        stale_completion_rejected: 0,
      },
      deferred: false,
      error: null,
      initialization_generation: 1,
      initialization_token: 'token-1',
      pose_sources: [],
      reason: 'ready',
      runtime_identity: {
        deployment_generation: 1,
        graph_revision: 'graph-1',
        program_generation: 0,
        retained_bundle_id: 'bundle-1',
        runtime_generation: 1,
      },
      started_at_ns: 1,
      state: 'open',
      updated_at_ns: 2,
    },
  };
}

function poseSourceFixture(
  payload: Record<string, unknown>,
  overrides: Record<string, unknown> = {},
): Record<string, unknown> {
  return {
    buffered: false,
    eligible: false,
    expires_at_ns: 2_000_000_000,
    fresh: true,
    object_id: 'robot-1',
    observed_at_ns: 1_000_000_000,
    payload,
    policy_id: 'viewer.measured_robot',
    quality: 'good',
    source_epoch: 'observer-epoch-1',
    source_epoch_order: 1,
    source_id: 'isaac-observer',
    source_sequence: 7,
    ...overrides,
  };
}

describe('parseSceneUrdfManifest', () => {
  it('accepts a complete authored-neutral projection', () => {
    const parsed = parseSceneUrdfManifest(structuralManifestFixture());

    expect(parsed.projection.runtime_state_used).toBe(false);
    expect(parsed.projection.operational_validity).toBe('not_asserted');
    expect(parsed.links[0]?.neutral_transform.relative_to).toBe('base_link');
  });

  it('rejects a projection that claims runtime state was used', () => {
    const fixture = structuralManifestFixture();
    const projection = fixture.projection as Record<string, unknown>;
    projection.runtime_state_used = true;

    expect(() => parseSceneUrdfManifest(fixture)).toThrowError(
      StructuralContractError,
    );
  });

  it('rejects links without explicit neutral transforms', () => {
    const fixture = structuralManifestFixture();
    const [link] = fixture.links as Record<string, unknown>[];
    delete link.neutral_transform;

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest.links[0].neutral_transform must be an object',
    );
  });

  it('rejects historical runtime-authority fields', () => {
    const fixture = structuralManifestFixture();
    fixture.authority = { commandable_joint_names: ['joint1'] };

    expect(() => parseSceneUrdfManifest(fixture)).toThrow(
      'manifest must be free of runtime authority and live-state fields',
    );
  });
});

describe('scene mesh asset parser', () => {
  const asset = () => ({
    schema_version: 'manyforge.scene_mesh_asset.v1',
    asset_hash: '0123456789abcdef',
    mesh_asset_url: '/api/scene/object-meshes/0123456789abcdef.glb?owner_epoch=scene.epoch.1&generation=7',
    mesh_asset_status: 'pending',
    source_owner_epoch: 'scene.epoch.1',
    source_generation: 7,
    cell_id: 'cell.1',
    last_error: null,
  });

  it('requires the exact owner-generation-bound URL', () => {
    expect(parseSceneMeshAssetMetadata(asset()).source_generation).toBe(7);
    const stale = asset();
    stale.mesh_asset_url = '/api/scene/object-meshes/0123456789abcdef.glb?owner_epoch=scene.epoch.1&generation=6';
    expect(() => parseSceneMeshAssetMetadata(stale)).toThrow(/owner-generation/);
  });

  it('requires the exact Program Draft-revision-bound URL', () => {
    const draftAsset = {
      schema_version: 'manyforge.program_draft_mesh_asset.v1',
      asset_hash: '0123456789abcdef',
      mesh_asset_url: '/api/program/draft/scene-meshes/0123456789abcdef.glb?draft_id=draft-1&draft_revision=9',
      mesh_asset_status: 'pending',
      draft_id: 'draft-1',
      draft_revision: 9,
      last_error: null,
    };
    expect(parseProgramDraftMeshAssetMetadata(draftAsset).draft_revision).toBe(9);
    draftAsset.mesh_asset_url = '/api/program/draft/scene-meshes/0123456789abcdef.glb?draft_id=draft-1&draft_revision=8';
    expect(() => parseProgramDraftMeshAssetMetadata(draftAsset)).toThrow(/Draft-revision/);
  });
});

describe('Program product projections', () => {
  const digestA = `sha256:${'a'.repeat(64)}`;
  const digestB = `sha256:${'b'.repeat(64)}`;
  const digestC = `sha256:${'c'.repeat(64)}`;

  function runStatus(state = 'running'): Record<string, unknown> {
    return {
      schema_version: 'manyforge.program_run_status.v2',
      owner_id: 'program-runner.cell-a',
      owner_epoch: 'program-runner.epoch.1',
      run_revision: 3,
      state,
      capture: {
        schema_version: 'manyforge.program_run_capture.v1',
        program_run_id: 'program-run.1',
        applied_record_digest: digestC,
        scenario_id: 'cell-pick',
        scenario_catalog_revision: digestA,
        program_id: 'pick',
        program_revision: digestB,
        program_binding_id: digestC,
        retained_bundle_id: digestA,
        graph_revision: digestB,
        node_environment_digest: digestC,
        node_policy_digest: digestA,
        node_policies: [],
        owner_identities: [{
          owner_role: 'program_runner',
          owner_id: 'program-runner.cell-a',
          owner_epoch: 'program-runner.epoch.1',
        }],
      },
      tree_status: 'running',
      node_status_revision: 2,
      node_statuses: { root: 'running' },
      active_operation_count: 1,
      reason_code: null,
      message: '',
    };
  }

  function runtimeControlsProjection(): Record<string, unknown> {
    return {
      schema_version: 'manyforge.program_runtime_control_projection.v6',
      owner_id: 'program-runner.cell-a',
      owner_epoch: 'program-runner.epoch.1',
      program_run_id: 'program-run.1',
      run_revision: 3,
      runtime_controls: {
        schema_version: 'manyforge.program_runtime_controls.v2',
        program_run_id: 'program-run.1',
        runtime_control_revision: 1,
        recovery_fences: {
          program_generation: 1,
          deployment_generation: 1,
          scene_admission_epoch: 1,
          pose_source_apply_generation: 0,
        },
        has_active_runtime_overrides: false,
        has_staged_runtime_overrides: false,
        has_stale_runtime_overrides: false,
        overrides: [],
        promotion_proposals: [],
        retry_counts: {},
        audit: [],
      },
      recovery_offer: null,
      execution_selections: {
        selection_revision: 0,
        groups: [{ group_id: 'all', node_count: 1, held_node_count: 0,
          state: 'running', stop_allowed: true,
          start_allowed: false, start_unavailable_reason: 'selection_not_ready',
          resume_allowed: false,
          resume_unavailable_reason: 'selection_not_held',
          runtime_scale_selector: {
            program_binding_id: digestC,
            component_role_id: 'robot',
            target_role_id: 'arm',
          },
          runtime_scale_unavailable_reason: null }],
      },
      observation_override_candidate: {
        schema_version: 'manyforge.runtime_override_observation_candidate.v1',
        available: false,
        reason_code: 'program_attempt_not_held',
        reason: 'No Program node attempt is currently held.',
        program_run_id: 'program-run.1',
        node_instance_id: null,
        node_attempt_id: null,
        node_id: null,
        parameter_id: null,
        value: null,
        source_observation_id: null,
        source_owner_generation: null,
      },
    };
  }

  it('parses bounded scenarios and the exact Applied owner identity', () => {
    const catalog = parseScenarioCatalogResponse({
      api_version: 'manyforge.scenario_catalog.v1',
      scenario_catalog_revision: digestA,
      scenarios: [{
        scenario_id: 'cell-pick',
        description: 'Pick one object',
        aliases: ['pick'],
        deployment_artifact_id: 'cell-a',
        deployment_revision: digestB,
        retained_bundle_id: digestC,
        program_binding_id: digestA,
        deployment_compatibility: { lane: 'manyforge-only', provides: [] },
        assistant_mode: 'forbidden',
        typed_parameter_overrides: {},
      }],
    });
    const appliedInput = {
      assistant_mode: 'forbidden',
      baseline_program_binding_id: digestA,
      deployment_artifact_id: 'cell-a',
      deployment_revision: digestB,
      graph_revision: digestC,
      node_environment_digest: digestA,
      preparation_digest: digestB,
      program: programFixture(),
      program_binding: programBindingFixture('fixture', digestA, digestC),
      program_binding_id: digestC,
      program_id: 'fixture',
      program_revision: digestA,
      retained_bundle_id: digestB,
      scenario_catalog_revision: digestA,
      scenario_id: 'cell-pick',
      schema_version: 'manyforge.applied_program_selection.v1',
      source: {
        record_digest: digestC,
        semantic_content_digest: digestB,
        source_generation: 1,
        source_owner_epoch: 'headless.epoch.1',
        source_owner_id: 'headless.program-apply',
      },
      typed_parameter_overrides: {},
    };
    const applied = parseAppliedProgramSelection(appliedInput);

    expect(catalog.scenarios[0].description).toBe('Pick one object');
    const current = parseCurrentScenarioResponse({
      schema_version: 'manyforge.current_scenario.v1',
      ready: true,
      current_scenario: {
        scenario_id: 'cell-pick',
        scenario_catalog_revision: digestA,
        deployment_artifact_id: 'cell-a',
        deployment_revision: digestB,
        retained_bundle_id: digestC,
        graph_revision: digestB,
        program_id: 'fixture',
        program_revision: digestA,
        program_binding_id: digestC,
      },
      unavailable_reasons: [],
    });
    expect(current.currentScenario?.scenarioId).toBe('cell-pick');
    expect(() => parseCurrentScenarioResponse({
      schema_version: 'manyforge.current_scenario.v1',
      ready: false,
      current_scenario: null,
      unavailable_reasons: [],
    })).toThrow(/unavailable reasons exactly when no current Scenario exists/);
    expect(applied.recordDigest).toBe(digestC);
    expect(applied.sourceOwnerEpoch).toBe('headless.epoch.1');
    expect(applied.program.program_id).toBe('fixture');
    expect(applied.programBinding.program_binding_id).toBe(digestC);
    expect(
      applied.programBinding.component_role_bindings[0].component_role_id,
    ).toBeTypeOf('string');
    const nestedMalformed = programBindingFixture('fixture', digestA, digestC);
    (
      (
        nestedMalformed.target_role_bindings as Record<string, unknown>[]
      )[0].frame_role_bindings as Record<string, unknown>[]
    )[0].unknown = true;
    expect(() => parseAppliedProgramSelection({
      ...appliedInput,
      program_binding: nestedMalformed,
    })).toThrow(/closed record/);
    expect(() => parseAppliedProgramSelection({
      ...appliedInput,
      program_binding: programBindingFixture(
        'another-program',
        digestA,
        digestC,
      ),
    })).toThrow(/content-identical Program and Program Binding identities/);
    expect(() => parseAppliedProgramSelection({
      ...appliedInput,
      program_binding: {
        ...programBindingFixture('fixture', digestA, digestC),
        retained_bundle_id: digestA,
      },
    })).toThrow(/content-identical Program and Program Binding identities/);
    expect(() => parseAppliedProgramSelection({
      ...appliedInput,
      program_binding: {
        ...programBindingFixture('fixture', digestA, digestC),
        graph_revision: digestA,
      },
    })).toThrow(/content-identical Program and Program Binding identities/);
    expect(() => parseAppliedProgramSelection({
      ...appliedInput,
      program_binding: {
        ...programBindingFixture('fixture', digestA, digestC),
        node_environment: {
          ...(programBindingFixture(
            'fixture',
            digestA,
            digestC,
          ).node_environment as Record<string, unknown>),
          unknown: true,
        },
      },
    })).toThrow(/closed record/);
  });

  it('keeps run status tied to one exact runner owner', () => {
    const status = parseProgramRunStatus(runStatus());

    expect(status.capture.programRunId).toBe('program-run.1');
  });

  it('accepts exact runtime controls when no recovery action is offered', () => {
    const projection = parseProgramRuntimeControlProjection(
      runtimeControlsProjection(),
    );

    expect(projection.recoveryOffer).toBeNull();
    expect(projection.programRunId).toBe('program-run.1');
  });

  it('accepts a bounded recent control trace after many events and rejects gaps', () => {
    const input = runtimeControlsProjection();
    const controls = input.runtime_controls as Record<string, unknown>;
    controls.runtime_control_revision = 10_001;
    controls.audit = Array.from({ length: 256 }, (_, index) => ({
      event_revision: 9_745 + index, kind: 'runtime_blackboard_written', value: index,
    }));
    const parsed = parseProgramRuntimeControlProjection(input).runtimeControls;
    expect(parsed.runtimeControlRevision).toBe(10_001);
    expect(parsed.audit[0].eventRevision).toBe(9_745);
    for (const length of [0, 255, 257]) {
      controls.audit = Array.from({ length }, (_, index) => ({
        event_revision: 10_001 - length + index, kind: 'event',
      }));
      expect(() => parseProgramRuntimeControlProjection(input)).toThrow();
    }
    controls.audit = Array.from({ length: 256 }, (_, index) => ({
      event_revision: 9_744 + index, kind: 'event',
    }));
    expect(() => parseProgramRuntimeControlProjection(input)).toThrow();
    controls.schema_version = 'manyforge.program_runtime_controls.v1';
    expect(() => parseProgramRuntimeControlProjection(input)).toThrow();
  });

  it('parses one coherent cycle, run, and runtime-control observation', () => {
    const input = {
      program_runner_owner_epoch: 'program-runner.epoch.1',
      cycle_generation: 8,
      state: 'running',
      program_run_id: 'program-run.1',
      cycle_claim_generation: 2,
      terminal_or_settlement_reference: null,
      execution_control_revision: 4,
      causal_correlation_id: null,
      causal_session_id: null,
      causal_client_sequence: null,
      current_command_kind: null,
      failure: null,
      reset_phase: null,
      allowed_commands: [],
      run_revision: 3,
      node_status_revision: 2,
      runtime_control_revision: 1,
      recovery_offer: null,
      run: runStatus(),
      runtime_controls: runtimeControlsProjection(),
    };
    const cycle = parseProgramCycleStatus(input);

    expect(cycle.run?.runRevision).toBe(3);
    expect(cycle.runtimeControls?.runtimeControls.runtimeControlRevision).toBe(1);
    expect(() => parseProgramCycleStatus({
      ...input,
      run_revision: 4,
    })).toThrow(/exact advertised run projection/);
    expect(() => parseProgramCycleStatus({
      ...input,
      causal_client_sequence: '0',
    })).toThrow(/canonical positive uint64/);
    expect(parseProgramCycleStatus({
      ...input,
      causal_client_sequence: '18446744073709551615',
    }).causalClientSequence).toBe('18446744073709551615');
  });

  it('retains an immutable predecessor run after terminal owner recovery', () => {
    const run = runStatus('failed');
    run.tree_status = 'failure';
    run.node_statuses = { root: 'failure' };
    const controls = runtimeControlsProjection();
    controls.owner_epoch = 'program-runner.epoch.2';
    const input = {
      program_runner_owner_epoch: 'program-runner.epoch.2',
      cycle_generation: 8,
      state: 'terminal_awaiting_reset',
      program_run_id: 'program-run.1',
      cycle_claim_generation: 2,
      terminal_or_settlement_reference: digestA,
      execution_control_revision: 5,
      causal_correlation_id: 'run.1',
      causal_session_id: null,
      causal_client_sequence: null,
      current_command_kind: 'program_run',
      failure: null,
      reset_phase: null,
      allowed_commands: [],
      run_revision: 3,
      node_status_revision: 2,
      runtime_control_revision: 1,
      recovery_offer: null,
      run,
      runtime_controls: controls,
    };
    const cycle = parseProgramCycleStatus(input);

    expect(cycle.programRunnerOwnerEpoch).toBe('program-runner.epoch.2');
    expect(cycle.run?.ownerEpoch).toBe('program-runner.epoch.1');
    expect(parseProgramCycleStatus({
      ...input,
      state: 'resetting',
      reset_phase: 'stopping_motion',
      current_command_kind: 'program_reset',
    }).run?.ownerEpoch).toBe('program-runner.epoch.1');
    expect(() => parseProgramCycleStatus({
      ...input,
      state: 'running',
    })).toThrow(/exact advertised run projection/);
  });

  it('parses Composer Apply, Load, and Revert wrappers without trusting headless details', () => {
    const digestA = `sha256:${'a'.repeat(64)}`;
    const digestB = `sha256:${'b'.repeat(64)}`;
    const digestC = `sha256:${'c'.repeat(64)}`;
    const applied = {
      assistant_mode: 'forbidden',
      baseline_program_binding_id: digestA,
      deployment_artifact_id: 'cell-a',
      deployment_revision: digestB,
      graph_revision: digestC,
      node_environment_digest: digestA,
      preparation_digest: digestB,
      program: programFixture(),
      program_binding: programBindingFixture('fixture', digestA, digestC),
      program_binding_id: digestC,
      program_id: 'fixture',
      program_revision: digestA,
      retained_bundle_id: digestB,
      scenario_catalog_revision: digestA,
      scenario_id: 'cell-pick',
      schema_version: 'manyforge.applied_program_selection.v1',
      source: {
        record_digest: digestC,
        semantic_content_digest: digestB,
        source_generation: 1,
        source_owner_epoch: 'headless.epoch.1',
        source_owner_id: 'headless.program-apply',
      },
      typed_parameter_overrides: {},
    };
    const headless = {
      schema_version: 'manyforge.program_apply_result.v2',
      status: 'applied',
      preparation: {},
      applied,
    };
    const response = parseProgramWorkspaceTransitionResponse({
      status: 'applied',
      headless,
      workspace: workspaceFixture(),
    });

    expect(response.status).toBe('applied');
    expect(response.workspace.draft.programLoaded).toBe(true);
    expect(() => parseProgramWorkspaceTransitionResponse({
      status: 'pending_deployment',
      headless: {
        schema_version: 'manyforge.program_apply_result.v2',
        status: 'pending_deployment',
        preparation: {},
        applied: null,
      },
      workspace: workspaceFixture(),
    })).toThrow(/closed record|status/);
    expect(parseProgramWorkspaceTransitionResponse({
      status: 'reverted',
      headless: null,
      workspace: workspaceFixture(),
    }).status).toBe('reverted');
    expect(() => parseProgramWorkspaceTransitionResponse({
      status: 'applied',
      headless: null,
      workspace: workspaceFixture(),
    })).toThrow(/owner response/);
    const malformed = structuredClone(applied);
    (
      (
        malformed.program_binding
          .target_role_bindings as Record<string, unknown>[]
      )[0].frame_role_bindings as Record<string, unknown>[]
    )[0].unknown = true;
    expect(() => parseProgramWorkspaceTransitionResponse({
      status: 'applied',
      headless: { ...headless, applied: malformed },
      workspace: workspaceFixture(),
    })).toThrow(/closed record/);
  });
});

describe('Deployment workspace parser', () => {
  it('retains bounded Apply phase timings and preparation identity', () => {
    const digestA = `sha256:${'a'.repeat(64)}`;
    const digestB = `sha256:${'b'.repeat(64)}`;
    const digestC = `sha256:${'c'.repeat(64)}`;
    const preparedActivation = {
      schema_version: 'manyforge.prepared_activation_input.v2',
      prepared_activation_id: digestC,
      source_document_digest: digestA,
      captured_input_digest: digestB,
      deployment_artifact_id: 'deployment.fixture',
      deployment_revision: digestA,
      retained_bundle_id: digestB,
      graph_revision: digestC,
      program_id: 'fixture-program',
      program_revision: digestB,
      program_binding_id: digestA,
      node_environment_digest: digestC,
      assistant_mode: 'forbidden',
      typed_parameter_overrides: {},
    };
    const parsed = parseDeploymentCandidateOperationResponse({
      schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1',
      workspace: deploymentWorkspaceFixture(),
      result: {
        status: 'applied',
        confirmation_token: null,
        prepared_activation: preparedActivation,
        candidate_preview: null,
        manager_apply: null,
        selection: null,
        preparation: null,
        phases_ms: { preparation_ms: 2.5, preview_ms: 8.25 },
        apply_id: 'apply-1',
      },
    });

    expect(parsed.result.phasesMs).toEqual({
      preparation_ms: 2.5,
      preview_ms: 8.25,
    });
    expect(parsed.result.applyId).toBe('apply-1');
  });

  it('accepts the authored graph without embedding its compiler-issued revision', () => {
    const parsed = parseDeploymentWorkspaceResponse(deploymentWorkspaceFixture());
    expect(parsed.draft.document?.graph).not.toHaveProperty('graph_revision');
    expect(parsed.draft.graphRevision).toBe(`sha256:${'c'.repeat(64)}`);
    expect(parsed.binding?.component_role_bindings.length).toBeGreaterThan(0);
    expect(parsed.scenarioCandidates.map((row) => row.scenarioId)).toEqual(['fixture']);
    expect(parsed.runAdmission.ready).toBe(true);
  });

  it('rejects mismatched or duplicate candidate identities', () => {
    const mismatched = deploymentWorkspaceFixture();
    (mismatched.scenarioCandidates as Record<string, unknown>[])[0]
      .deploymentRevision = `sha256:${'d'.repeat(64)}`;
    expect(() => parseDeploymentWorkspaceResponse(mismatched)).toThrow(
      /exact matches/,
    );

    const duplicate = deploymentWorkspaceFixture();
    (duplicate.scenarioCandidates as unknown[]).push(
      structuredClone((duplicate.scenarioCandidates as unknown[])[0]),
    );
    expect(() => parseDeploymentWorkspaceResponse(duplicate)).toThrow(
      /unique Scenario identities/,
    );
  });

  it('rejects a Binding for another exact graph', () => {
    const fixture = deploymentWorkspaceFixture();
    (fixture.binding as Record<string, unknown>).graph_revision =
      `sha256:${'d'.repeat(64)}`;
    expect(() => parseDeploymentWorkspaceResponse(fixture)).toThrow(
      /graph revision/,
    );
  });

  it('rejects crossed document, Run-admission, or Applied identities', () => {
    const crossedDocument = deploymentWorkspaceFixture();
    const document = (crossedDocument.draft as Record<string, unknown>)
      .document as Record<string, unknown>;
    document.deployment_revision = `sha256:${'d'.repeat(64)}`;
    expect(() => parseDeploymentWorkspaceResponse(crossedDocument)).toThrow(
      /exact compiled Deployment identity/,
    );

    const crossedAdmission = deploymentWorkspaceFixture();
    (crossedAdmission.runAdmission as Record<string, unknown>).ready = false;
    (crossedAdmission.runAdmission as Record<string, unknown>).reason = 'crossed';
    expect(() => parseDeploymentWorkspaceResponse(crossedAdmission)).toThrow(
      /exact Deployment Draft and Manager Applied comparison/,
    );

    const crossedApplied = deploymentWorkspaceFixture();
    (crossedApplied.managerApplied as Record<string, unknown>).deploymentRevision =
      `sha256:${'d'.repeat(64)}`;
    expect(() => parseDeploymentWorkspaceResponse(crossedApplied)).toThrow(
      /exact Deployment Draft and Manager Applied comparison/,
    );
  });

  it('rejects a partially populated unloaded Draft identity', () => {
    const fixture = deploymentWorkspaceFixture();
    Object.assign(fixture.draft as Record<string, unknown>, {
      document: null,
      deploymentArtifactId: 'partial',
      deploymentRevision: null,
      graphRevision: null,
    });
    expect(() => parseDeploymentWorkspaceResponse(fixture)).toThrow(
      /complete identities/,
    );
  });
});

describe('parseCatalogResponse', () => {
  it('accepts the v2 compiler-product identity and entry shape', () => {
    const parsed = parseCatalogResponse({
      apiVersion: 'manyforge.node_catalog.v2',
      contentVersion: '1.0.0',
      catalogId: 'manyforge_builtin',
      catalogDigest: 'sha256:catalog',
      environmentDigest: 'sha256:environment',
      entries: [
        {
          id: 'sequence',
          name: 'Sequence',
          category: 'Control Flow',
          description: 'Runs children in order.',
          stability: 'stable',
          kind: 'composite',
          childrenMin: 1,
          childrenMax: 64,
          parameters: [],
          requiredCapabilities: [],
          execution: {
            blocking: false,
            cancellable: false,
            ownerApi: 'orchestration.control_flow',
          },
          runners: { py_trees: 'sequence_v1' },
          tags: ['control-flow'],
          minProgramProfile: null,
          blackboard: { reads: [], writes: [] },
          ownerContext: null,
          outcomes: { success: 'all children succeeded' },
          eventFamily: null,
        },
      ],
    });

    expect(parsed.apiVersion).toBe('manyforge.node_catalog.v2');
    expect(parsed.entries[0]?.execution.ownerApi).toBe(
      'orchestration.control_flow',
    );
    expect(parsed.entries[0]?.runners).toEqual({ py_trees: 'sequence_v1' });
  });

  it('does not accept a historical catalog version', () => {
    expect(() => parseCatalogResponse({ apiVersion: 'v1' })).toThrow(
      'catalog.apiVersion must be the literal "manyforge.node_catalog.v2"',
    );
  });
});

describe('Program workspace parsers', () => {
  it('preserves canonical execution membership without inventing runtime group state', () => {
    const fixture = workspaceFixture();
    const program = (fixture.draft as Record<string, unknown>).program as Record<string, unknown>;
    const authoring = (fixture.draft as Record<string, unknown>).authoring as Record<string, unknown>;
    const tree = program.tree as Record<string, unknown>;
    tree.execution_groups = ['all_grippers', 'robot_a'];
    expect(parseProgramWorkspaceResponse(fixture).draft.program?.tree.execution_groups)
      .toEqual(['all_grippers', 'robot_a']);
    tree.execution_groups = 'all';
    expect(parseProgramWorkspaceResponse(fixture).draft.program?.tree.execution_groups).toBe('all');
    for (const malformed of [null, 'inherit', 'robot_a', [], ['all'], ['a', 'a'], ['z', 'a'], ['UPPER']]) {
      tree.execution_groups = malformed;
      expect(() => parseProgramWorkspaceResponse(fixture)).toThrow();
    }
    delete tree.execution_groups;
    program.execution_groups = { robot_group: { execution_policy: 'independent' } };
    authoring.execution_groups = { robot_group: { execution_policy: 'independent' } };
    const parsed = parseProgramWorkspaceResponse(fixture);
    expect(parsed.draft.program?.execution_groups).toEqual({ robot_group: { execution_policy: 'independent' } });
    expect(parsed.draft.authoring?.execution_groups).toEqual({ robot_group: { execution_policy: 'independent' } });
    for (const malformed of [null, 'robot_group', [], { all: { execution_policy: 'independent' } }, { robot_group: {} }, { robot_group: { execution_policy: 'unknown' } }]) {
      program.execution_groups = malformed;
      authoring.execution_groups = malformed;
      expect(() => parseProgramWorkspaceResponse(fixture)).toThrow();
    }
    delete program.execution_groups;
    delete authoring.execution_groups;
    program.api_version = 'manyforge.program.v1';
    expect(() => parseProgramWorkspaceResponse(fixture)).toThrow('manyforge.program.v2');
  });

  it('keeps Draft, Saved, and unavailable Applied identities distinct', () => {
    const workspace = parseProgramWorkspaceResponse(workspaceFixture());

    expect(workspace.draft.program?.tree.value_sources?.duration_s).toEqual({
      kind: 'program_parameter',
      reference: 'duration',
    });
    expect(workspace.saved.matchesDraft).toBe(false);
    expect(workspace.hasUnappliedChanges).toBeNull();
    expect(workspace.applied.unavailableReason?.restorationGates).toEqual(['N7']);
  });

  it('parses the exact Program scene and viewer authoring contracts', () => {
    const fixture = workspaceFixture();
    const program = (fixture.draft as Record<string, unknown>).program as Record<string, unknown>;
    program.scene = [{
      scene_resource_id: 'part',
      shape: { type: 'sphere', sphere_radius_m: 0.02 },
      pose: {
        frame_ref: { kind: 'cell_frame' },
        position_m: [0.4, 0, 0.2],
        orientation: { xyzw: [0, 0, 0, 1] },
      },
    }];
    program.viewer = {
      frame_role_visibility: {},
      frame_gizmo_size_m: 0.1,
      resource_role_visibility: {},
      scene_resource_axes_visibility: { part: true },
      object_axes_gizmo_size_m: 0.15,
    };

    const parsed = parseProgramWorkspaceResponse(fixture);
    expect(parsed.draft.program?.scene?.[0]?.shape.type).toBe('sphere');
    expect(parsed.draft.program?.viewer?.object_axes_gizmo_size_m).toBe(0.15);
    expect(parsed.draft.program?.viewer?.scene_resource_axes_visibility).toEqual({
      part: true,
    });

    (program.viewer as Record<string, unknown>).scene_resource_axes_visibility = { absent: true };
    expect(() => parseProgramWorkspaceResponse(fixture)).toThrow(
      'a declared Program scene resource',
    );
    (program.viewer as Record<string, unknown>).scene_resource_axes_visibility = { part: true };

    ((program.scene as Record<string, unknown>[])[0].shape as Record<string, unknown>).radius = 1;
    expect(() => parseProgramWorkspaceResponse(fixture)).toThrow(
      'exactly [sphere_radius_m, type]',
    );
  });

  it('parses complete canonical node policies and rejects closed-shape drift', () => {
    const fixture = workspaceFixture();
    const program = (fixture.draft as Record<string, unknown>).program as Record<string, unknown>;
    const tree = program.tree as Record<string, unknown>;
    tree.modification_policy = {
      runtime_override: 'human_approval',
      persistent_edit: 'human_approval',
      intervention_delegate: 'human_only',
    };
    tree.recovery_policy = {
      allow_intervention: true,
      allowed_recovery_actions: ['intervention'],
      max_retries: 2,
      allowed_skills: [],
      persistence_ceiling: 'draft_patch',
    };
    tree.operator_stop_policy = 'hold';

    const parsed = parseProgramWorkspaceResponse(fixture);
    expect(parsed.draft.program?.tree.modification_policy).toEqual(
      tree.modification_policy,
    );
    expect(parsed.draft.program?.tree.recovery_policy?.allowed_skills).toEqual([]);
    expect(parsed.draft.program?.tree.operator_stop_policy).toBe('hold');

    const cases = [
      { modification_policy: null },
      { recovery_policy: { allow_intervention: true } },
      {
        modification_policy: {
          runtime_override: 'human_approval',
          persistent_edit: 'human_approval',
          intervention_delegate: 'human_only',
          legacy: 'forbidden',
        },
      },
      { operator_stop_policy: 'pause' },
      {
        recovery_policy: {
          allow_intervention: false,
          allowed_recovery_actions: ['intervention'],
          max_retries: 0,
          allowed_skills: [],
          persistence_ceiling: 'session_only',
        },
      },
    ];
    for (const change of cases) {
      const invalid = structuredClone(fixture);
      const invalidProgram = (invalid.draft as Record<string, unknown>).program as Record<string, unknown>;
      const invalidTree = invalidProgram.tree as Record<string, unknown>;
      Object.assign(invalidTree, change);
      expect(() => parseProgramWorkspaceResponse(invalid)).toThrow();
    }
  });

  it('parses canonical imported skills without treating C1 applicability as syntax', () => {
    const fixture = workspaceFixture();
    const program = (fixture.draft as Record<string, unknown>).program as Record<string, unknown>;
    const tree = program.tree as Record<string, unknown>;
    tree.recovery_policy = {
      allow_intervention: true,
      allowed_recovery_actions: ['intervention'],
      max_retries: 0,
      allowed_skills: ['legacy.inspect'],
      persistence_ceiling: 'session_only',
    };
    tree.children = [{
      node_id: 'timer',
      instance_id: 'update-b',
      parameters: {},
      children: [],
    }];
    fixture.applicability = {
      applyReady: false,
      affectedInstanceIds: ['wait'],
      unavailableReason: {
        code: 'composer_policy_skill_referent_unavailable',
        message: 'Allowed recovery skills are unavailable: each skill requires an exact revision-qualified retained referent. Leave Allowed skills empty.',
        restorationGates: [],
      },
    };
    const parsed = parseProgramWorkspaceResponse(fixture);

    expect(parsed.draft.program?.tree.recovery_policy?.allowed_skills)
      .toEqual(['legacy.inspect']);
    expect(parsed.applicability.affectedInstanceIds).toEqual(['wait']);
  });

  it('accepts an incomplete retained forest without inventing a Program candidate', () => {
    const fixture = workspaceFixture();
    const draft = fixture.draft as Record<string, unknown>;
    const authoring = draft.authoring as Record<string, unknown>;
    authoring.forest = [];
    draft.candidateAvailable = false;
    draft.programRevision = null;
    draft.program = null;
    draft.diagnostics = [{
      code: 'authoring_forest_root_count',
      path: 'forest',
      message: 'Program requires exactly one root; found 0.',
      severity: 'error',
    }];
    (fixture.saved as Record<string, unknown>).matchesDraft = false;
    fixture.hasUnsavedChanges = true;
    fixture.applicability = {
      applyReady: false,
      affectedInstanceIds: [],
      unavailableReason: {
        code: 'composer_authoring_draft_incomplete',
        message: 'Program requires exactly one root; found 0.',
        restorationGates: [],
      },
    };

    const parsed = parseProgramWorkspaceResponse(fixture);
    expect(parsed.draft.authoring?.forest).toEqual([]);
    expect(parsed.draft.program).toBeNull();
    expect(parsed.draft.diagnostics[0]?.code).toBe('authoring_forest_root_count');
  });

  it('rejects inconsistent or historical Program binding shapes', () => {
    const inconsistent = workspaceFixture();
    (inconsistent.draft as Record<string, unknown>).programRevision = 'sha256:wrong';
    expect(() => parseProgramWorkspaceResponse(inconsistent)).toThrow(
      'programWorkspace.draft must be internally consistent Program identity',
    );

    const falseSavedComparison = workspaceFixture();
    (falseSavedComparison.saved as Record<string, unknown>).matchesDraft = true;
    expect(() => parseProgramWorkspaceResponse(falseSavedComparison)).toThrow(
      'programWorkspace.saved must be a truthful Draft/Saved comparison',
    );

    const falseAppliedIdentity = workspaceFixture();
    (falseAppliedIdentity.applied as Record<string, unknown>).programRevision = (
      falseAppliedIdentity.draft as Record<string, unknown>
    ).programRevision;
    expect(() => parseProgramWorkspaceResponse(falseAppliedIdentity)).toThrow(
      'programWorkspace.applied must be internally consistent Applied identity',
    );

    const historical = workspaceFixture();
    const program = (historical.draft as Record<string, unknown>).program as Record<string, unknown>;
    const tree = program.tree as Record<string, unknown>;
    tree.parameters = { duration_s: '${param:duration}' };
    delete tree.value_sources;
    expect(() => parseProgramWorkspaceResponse(historical)).toThrow(
      'an explicit value_sources binding',
    );
  });

  it('projects authoring and execution availability independently', () => {
    const payload = {
      environmentDigest: 'sha256:environment',
      catalog: {},
      executableRegistry: [{
        node_id: 'timer',
        engine_id: 'py_trees',
        implementation_id: 'timer_v1',
        executable_package_id: 'manyforge_behavior',
      }],
      executablePackages: [],
      sourceCoverage: [],
      availability: {
        binding_context: 'unselected',
        execution_available: false,
        implementations: [{
          adapter_build_qualified: true,
          authoring_available: true,
          binding_available: false,
          engine_id: 'py_trees',
          executable_package_id: 'manyforge_behavior',
          execution_available: false,
          implementation_id: 'timer_v1',
          implementation_identity_resolved: true,
          node_id: 'timer',
          owner_health_available: true,
          reason_codes: ['program_binding_unselected'],
          restoration_gates: [],
        }],
        manager_runtime_converged: true,
        portable_authoring_available: true,
        reason_codes: ['program_binding_unselected'],
        restoration_gates: ['N'],
      },
      active: { retainedBundleId: 'bundle-1', graphRevision: 'graph-1' },
      source: {},
    };
    const environment = parseNodeEnvironmentResponse(payload);

    expect(environment.implementations[0]).toMatchObject({
      authoringAvailable: true,
      executionAvailable: false,
    });
    expect(environment.executionAvailable).toBe(false);

    payload.availability.implementations[0].owner_health_available = 'yes' as never;
    expect(() => parseNodeEnvironmentResponse(payload)).toThrow(
      'owner_health_available must be a boolean',
    );
  });
});

describe('headless projection parsers', () => {
  it('accepts exact source-model, logical-scene, and scene-runtime facts', () => {
    expect(parseSourceModelProjection(sourceModelFixture()).cell_id).toBe('cell-a');
    expect(parseLogicalSceneProjection(logicalSceneFixture()).snapshot?.objects[0]
      ?.scene_resource_id).toBe('part');
    expect(parseSceneRuntimeProjection(sceneRuntimeFixture()).state.state).toBe('open');
  });

  it('strictly parses carried-object attachment hierarchy metadata', () => {
    const fixture = logicalSceneFixture();
    const snapshot = fixture.snapshot as Record<string, unknown>;
    snapshot.carried_object_attachments = [{
      carried_object_attachment_id: 'attachment.1',
      carried_object_attachment_revision: `sha256:${'a'.repeat(64)}`,
      scene_resource_id: 'part',
      controllable_component_id: 'robot',
      command_target_id: 'arm',
      carrier_frame_ref: {
        kind: 'component',
        controllable_component_id: 'robot',
        model_instance_id: 'robot.model',
        frame_role_id: 'tcp',
      },
      pose_in_carrier_frame: {
        position_m: { x: 0, y: 0, z: 0.1 },
        orientation_xyzw: [0, 0, 0, 1],
      },
      touch_body_roles: ['tool_collision'],
    }];
    expect(parseLogicalSceneProjection(fixture).snapshot
      ?.carried_object_attachments[0].carrier_frame_ref.frame_role_id).toBe('tcp');
    (snapshot.carried_object_attachments as Record<string, unknown>[])[0]
      .unexpected = true;
    expect(() => parseLogicalSceneProjection(fixture)).toThrow(/exactly/);
  });

  it('parses exact good and unknown ROS observation payloads', () => {
    const fixture = sceneRuntimeFixture();
    const state = fixture.state as Record<string, unknown>;
    state.pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        joint_names: ['joint_1', 'joint_2'],
        kind: 'joint_state',
        positions: [0.1, -0.2],
        source_stamp_ns: 12500000000,
        topic: '/isaac_joint_states',
      }),
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        kind: 'simulation_entity_pose',
        pose: {
          frame_id: 'universe',
          frame_kind: 'cell_frame',
          orientation_xyzw: [0, 0, 0, 1],
          position_m: [0.4, -0.2, 0.1],
          source_stamp_ns: 13000000000,
        },
        source: { entity: '/mac_n_cheese_centered' },
      }, {
        eligible: true,
        object_id: 'isaac_cheese_box',
        policy_id: 'continuous_unattached',
        source_sequence: 8,
      }),
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        kind: 'detection3d_pose',
        source_error: {
          code: 'detection_not_found',
          detail: 'no detection matched',
          source_key: '/output',
        },
      }, {
        fresh: false,
        object_id: 'graspable',
        policy_id: 'before_planning',
        quality: 'unknown',
        source_sequence: 9,
      }),
    ];

    const parsed = parseSceneRuntimeProjection(fixture).state.pose_sources;
    expect(parsed[0]?.payload.kind).toBe('joint_state');
    expect(parsed[1]?.payload.kind).toBe('simulation_entity_pose');
    expect(parsed[2]?.quality).toBe('unknown');
  });

  it('rejects ambiguous or dishonest ROS observation shapes', () => {
    const invalidFrame = sceneRuntimeFixture();
    (invalidFrame.state as Record<string, unknown>).pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        kind: 'simulation_entity_pose',
        pose: {
          frame_id: 'world',
          frame_kind: 'unmapped_frame',
          orientation_xyzw: [0, 0, 0, 1],
          position_m: [0, 0, 0],
          source_stamp_ns: null,
        },
        source: { entity: '/part' },
      }),
    ];
    expect(() => parseSceneRuntimeProjection(invalidFrame)).toThrow(
      'frame_kind must be the literal "cell_frame"',
    );

    const duplicateJoint = sceneRuntimeFixture();
    (duplicateJoint.state as Record<string, unknown>).pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        joint_names: ['joint_1', 'joint_1'],
        kind: 'joint_state',
        positions: [0.1, 0.2],
        source_stamp_ns: null,
        topic: '/isaac_joint_states',
      }),
    ];
    expect(() => parseSceneRuntimeProjection(duplicateJoint)).toThrow(
      'equal non-empty unique joint_names',
    );

    const dishonestUnknown = sceneRuntimeFixture();
    (dishonestUnknown.state as Record<string, unknown>).pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v2',
        kind: 'joint_state',
        source_error: {
          code: 'source_error',
          detail: 'offline',
          source_key: '/isaac_joint_states',
        },
      }, { quality: 'unknown' }),
    ];
    expect(() => parseSceneRuntimeProjection(dishonestUnknown)).toThrow(
      'unknown quality to be unbuffered, ineligible, and stale',
    );
  });

  it('rejects stale shapes and cross-owner scene revisions', () => {
    const stale = sourceModelFixture();
    (stale.source as Record<string, unknown>).freshness = 'stale';
    expect(() => parseSourceModelProjection(stale)).toThrow('available and fresh');

    const scene = logicalSceneFixture();
    const snapshot = scene.snapshot as Record<string, unknown>;
    (snapshot.revision as Record<string, unknown>).revision_id = 2;
    expect(() => parseLogicalSceneProjection(scene)).toThrow(
      'the active projection scene revision',
    );
  });

  it('accepts only an exact successful scene transaction receipt', () => {
    const parsed = parseSceneTransactionResponse({
      api_version: 'manyforge.manager_command_response.v1',
      committed_projection_revision: projectionRevisionFixture(2),
      error: null,
      ok: true,
      projection_status: 'committed',
      replay_status: 'accepted_new',
      request_id: 'request-1',
      request_owner_epoch: 'scene-owner-epoch-7',
      transaction_sequence: 1,
      update_status: 'pose_changed',
    });
    expect(parsed.committed_projection_revision.scene_revision.revision_id).toBe(2);
  });

  it('parses exact pending and succeeded Manager Apply receipts', () => {
    const pending = parseManagerApplyResponse({
      api_version: 'manyforge.manager_command_response.v1',
      apply_generation: 1,
      apply_id: 'apply-1',
      candidate_projection_revision: null,
      error: null,
      ok: true,
      replay_status: 'accepted_new',
      request_id: 'request-1',
      request_owner_epoch: 'manager-owner-1',
      state: 'pending',
    });
    expect(pending.state).toBe('pending');
    expect(pending.candidateProjectionRevision).toBeNull();

    const succeeded = parseManagerApplyResponse({
      api_version: 'manyforge.manager_command_response.v1',
      apply_generation: 1,
      apply_id: 'apply-1',
      candidate_projection_revision: projectionRevisionFixture(2),
      error: null,
      ok: true,
      replay_status: 'replayed_exact',
      request_id: 'request-1',
      request_owner_epoch: 'manager-owner-1',
      state: 'succeeded',
    });
    expect(succeeded.state).toBe('succeeded');
    expect(succeeded.applyId).toBe('apply-1');
    if (succeeded.state !== 'succeeded') {
      throw new Error('expected a succeeded Manager Apply receipt');
    }
    expect(succeeded.candidateProjectionRevision.scene_revision.revision_id)
      .toBe(2);
  });

  it('rejects obsolete, extra-field, and inconsistent Manager Apply receipts', () => {
    const obsolete = {
      api_version: 'manyforge.manager_command_response.v1',
      error: null,
      ok: true,
      request_id: 'request-1',
      request_owner_epoch: 'manager-owner-1',
    };
    expect(() => parseManagerApplyResponse(obsolete)).toThrow(/exactly/);

    const extra = {
      api_version: 'manyforge.manager_command_response.v1',
      apply_generation: 1,
      apply_id: 'apply-1',
      candidate_projection_revision: null,
      error: null,
      ok: true,
      replay_status: 'accepted_new',
      request_id: 'request-1',
      request_owner_epoch: 'manager-owner-1',
      state: 'pending',
      projection_status: 'pending',
    };
    expect(() => parseManagerApplyResponse(extra)).toThrow(/exactly/);

    const pendingWithCandidate = {
      ...extra,
      projection_status: undefined,
      candidate_projection_revision: projectionRevisionFixture(),
    };
    delete pendingWithCandidate.projection_status;
    expect(() => parseManagerApplyResponse(pendingWithCandidate)).toThrow(
      /null while Apply is pending/,
    );

    const succeededWithoutCandidate = {
      ...extra,
      projection_status: undefined,
      candidate_projection_revision: null,
      state: 'succeeded',
    };
    delete succeededWithoutCandidate.projection_status;
    expect(() => parseManagerApplyResponse(succeededWithoutCandidate)).toThrow(
      /projection revision while Apply has succeeded/,
    );
  });

  it('keeps the maximum visible scene projection parse bounded', () => {
    const scene = logicalSceneFixture();
    const snapshot = scene.snapshot as Record<string, unknown>;
    const fixture = (snapshot.objects as Record<string, unknown>[])[0];
    snapshot.objects = Array.from({ length: 4096 }, (_, index) => ({
      ...fixture,
      scene_resource_id: `part-${index.toString().padStart(4, '0')}`,
    }));

    const started = performance.now();
    const parsed = parseLogicalSceneProjection(scene);
    const elapsedMs = performance.now() - started;

    expect(parsed.snapshot?.objects).toHaveLength(4096);
    expect(elapsedMs).toBeLessThan(1500);
  });

  it('accepts structural and headless Composer health without inventing state', () => {
    const structural = parseHealthResponse({
      status: 'degraded',
      mode: 'structural_only',
      components: { runtime: 'unavailable' },
      runtimeAvailability: {
        code: 'composable_cell_runtime_unavailable',
        message: 'Runtime is unavailable.',
        restorationGates: ['N7', 'L0', 'P5', 'I', 'R', 'T'],
      },
      timestamp: 1,
    });
    const headless = parseHealthResponse({
      status: 'degraded',
      mode: 'headless_client',
      components: { runtime: 'degraded' },
      runtimeAvailability: null,
      headless: { schema_version: 'manyforge.headless_health.v1' },
      timestamp: 2,
    });

    expect(structural.runtimeAvailability?.restorationGates).toEqual([
      'N7', 'L0', 'P5', 'I', 'R', 'T',
    ]);
    expect(structural.headless).toBeNull();
    expect(headless.mode).toBe('headless_client');
    expect(headless.runtimeAvailability).toBeNull();
  });

  it('parses exact Manager status, graph, and diff identities', () => {
    const source = {
      available: true,
      freshness: 'fresh',
      known_gaps: [],
      projection_generation: 4,
      projection_owner_epoch: 'projection.epoch.1',
      receiver_age_ns: 10,
      source_content_digest: 'sha256:' + 'a'.repeat(64),
      source_generation: 2,
      source_owner_epoch: 'manager.epoch.1',
      source_owner_id: 'component-manager.cell-a',
    };
    const statusWire = {
      schema_version: 'manyforge.manager_status_projection.v1',
      source,
      status: {
        api_version: 'manyforge.manager_source.v1',
        active: {
          cell_graph: { cell_id: 'cell-a', components: [] },
          graph_revision: 'graph.1',
          retained_bundle_id: 'bundle.1',
          publication: {},
        },
        apply: {
          mutation_available: true,
          records: [managerApplyRecordFixture()],
          unavailable_reason_code: null,
        },
        cell_id: 'cell-a',
        manager_ready: true,
        runtime: {
          converged: true,
          desired_units: [],
          observed_units: [],
        },
      },
    };
    const status = parseManagerStatusProjection(statusWire);
    const graph = parseManagerGraphProjection({
      schema_version: 'manyforge.manager_graph_projection.v1',
      source,
      graph_revision: 'graph.1',
      retained_bundle_id: 'bundle.1',
      cell_graph: { cell_id: 'cell-a', components: [] },
    });
    const diff = parseManagerDiffProjection({
      schema_version: 'manyforge.manager_diff_projection.v1',
      source,
      converged: true,
      desired_units: [],
      observed_units: [],
    });

    expect(status.status.active.graphRevision).toBe('graph.1');
    expect(status.status.apply.records[0]?.request.requestId).toBe('request-3');
    expect(status.status.apply.records[0]?.phase).toBe('fence_held');
    expect(graph.retainedBundleId).toBe('bundle.1');
    expect(diff.converged).toBe(true);

    const malformed = managerApplyRecordFixture();
    malformed.phase = 'browser_waiting';
    statusWire.status.apply.records = [malformed];
    expect(() => parseManagerStatusProjection(statusWire)).toThrow(
      'a supported Manager Apply phase',
    );
  });

  it('accepts exact runtime and four-state alarm snapshots', () => {
    const runtime = parseHeadlessRuntimeSnapshot(runtimeSnapshotFixture());
    const alarms = parseOperatorAlarmSnapshot(alarmSnapshotFixture());

    expect(runtime.sections.logicalScene.sourceOwnerEpoch).toBe('source-epoch-1');
    expect(alarms.alarms[0]?.lifecycleState).toBe('active_unacknowledged');
    expect(alarms.unacknowledgedMembership[0]?.recordRevision).toBe(3);
  });

  it('rejects unsafe fence numbers and unknown alarm lifecycle values', () => {
    const runtime = runtimeSnapshotFixture();
    runtime.projection_generation = Number.MAX_SAFE_INTEGER + 1;
    expect(() => parseHeadlessRuntimeSnapshot(runtime)).toThrow(
      'runtime.projection_generation must be a non-negative safe integer',
    );

    const alarms = alarmSnapshotFixture();
    const [record] = alarms.alarms as Record<string, unknown>[];
    record.lifecycle_state = 'cleared';
    expect(() => parseOperatorAlarmSnapshot(alarms)).toThrow(
      'a supported four-state alarm lifecycle value',
    );
  });

  it('accepts the bounded historian page and validates event schema identity', () => {
    const wire = {
      schema_version: 'manyforge.historian_summary_page.v2',
      time_window: { start_utc_ns: null, end_utc_ns: null },
      retention: null,
      events: [{
        sequence: 7,
        event: {
          schema_version: 'manyforge.historian_event_summary.v2',
          condition_severity: 'ERROR',
          occurred_at_utc_ns: '1785931200000000000',
          clock_owner: 'headless:test-epoch',
          wall_clock_regressed: false,
          event_id: 'event-1',
          component: 'manyforge.alarm',
          event_type: 'operator_alarm_activated',
          severity: 'INFO',
          occurred_at_ns: '9007199254740992',
          occurred_at_wall: '2026-08-05T12:00:00Z',
          message: 'operator alarm activated',
          program_run_id: null,
          operation_id: null,
          condition_id: 'condition-1',
        },
      }],
      next_before_sequence: null,
      truncated: false,
    };
    const page = parseHistorianPage(wire);

    expect(page.events[0]?.event.eventType).toBe('operator_alarm_activated');
    expect(page.events[0]?.sequence).toBe(7);
    expect(() => parseHistorianPage({ ...wire, payload: {} })).toThrow();
    const eventWithPayload = structuredClone(wire);
    (eventWithPayload.events[0]!.event as Record<string, unknown>).payload = {};
    expect(() => parseHistorianPage(eventWithPayload)).toThrow();
    const numericTimestamp = structuredClone(wire);
    (numericTimestamp.events[0]!.event as Record<string, unknown>).occurred_at_ns = 123;
    expect(() => parseHistorianPage(numericTimestamp)).toThrow(
      'must be a string',
    );
    const noncanonicalTimestamp = structuredClone(wire);
    (noncanonicalTimestamp.events[0]!.event as Record<string, unknown>)
      .occurred_at_ns = '0123';
    expect(() => parseHistorianPage(noncanonicalTimestamp)).toThrow(
      'a canonical nonnegative int64 decimal string',
    );
  });

  it('accepts typed exact historian detail availability unions', () => {
    const detail = parseHistorianDetail({
      schema_version: 'manyforge.historian_detail.v2',
      sequence: 7,
      event: {
        schema_version: 'manyforge.historian_event.v2',
        event_id: 'event-1',
        component: 'manyforge.execution',
        event_type: 'operation_updated',
        severity: 'INFO',
        occurred_at_ns: '9007199254740992',
        occurred_at_wall: '2026-08-05T12:00:00Z',
        payload: {},
      },
      sections: {
        incident: { available: false, reason: 'not an incident', value: null },
        execution_digest: { available: true, reason: null, value: { operation_id: 'op.1' } },
        blackboard: { available: false, reason: 'not published', value: null },
        latency: { available: false, reason: 'not published', value: null },
      },
    });
    expect(detail.sequence).toBe(7);
    expect(detail.sections.executionDigest.available).toBe(true);
    expect(() => parseHistorianDetail({
      ...detail,
      schema_version: 'manyforge.historian_detail.v2',
    })).toThrow();
  });
});

describe('addressed Robot Control parsers', () => {
  const sceneRevision = {
    scene_id: 'scene-1',
    revision_id: 3,
    content_hash: 'sha256:scene',
  };

  it('requires the backend-owned operation residency fact', () => {
    const response = {
      ok: true,
      command: 'read',
      resident: true,
      operation: {
        operation_id: 'operation-1',
        operation_revision: 4,
        state: 'completed',
        participants: [{ progress: 1, terminal_error: null }],
        active_stop_id: null,
        active_stop_generation: null,
      },
    };
    expect(parseOperationObservation(response).resident).toBe(true);

    const missing = structuredClone(response) as Record<string, unknown>;
    delete missing.resident;
    expect(() => parseOperationObservation(missing)).toThrow();
  });

  it('parses the persistent runtime-scale control without making its operation evidence routable', () => {
    const selector = {
      component_role_id: 'robot',
      program_binding_id: 'binding-program-1',
      target_role_id: 'arm',
    };
    const status = parseRuntimeScaleControlStatus({
      api_version: 'manyforge.execution_command_response.v1',
      command: 'get_runtime_scale_control',
      ok: true,
      runtime_scale_control: {
        active_control_configuration_generation: 1,
        active_control_configuration_id: 'active-1',
        active_operation_id: 'operation-1',
        active_operation_revision: 5,
        active_operation_scale_generation: 2,
        application_state: 'applied',
        control_binding_id: 'binding-1',
        control_binding_revision: 'revision-1',
        control_domain_id: 'domain-1',
        control_domain_revision: 'revision-1',
        control_generation: 3,
        effective_scale: 0.45,
        frozen: false,
        reason: null,
        requested_scale: 0.5,
        scaling_domain_id: 'scale-1',
        selector,
        supported: true,
      },
    });
    expect(status).toMatchObject({
      selector: {
        componentRoleId: 'robot',
        programBindingId: 'binding-program-1',
        targetRoleId: 'arm',
      },
      controlGeneration: 3,
      requestedScale: 0.5,
      activeOperationId: 'operation-1',
      activeOperationScaleGeneration: 2,
    });

    const accepted = parseRuntimeScaleControlAccepted({
      accepted: {
        accepted_at_ns: 123,
        active_operation_id: 'operation-1',
        active_operation_revision: 5,
        active_operation_scale_generation: 3,
        control_generation: 4,
        replay_status: 'accepted_new',
        request_id: 'request-scale-1',
        requested_scale: 0.25,
        selector,
      },
      api_version: 'manyforge.execution_command_response.v1',
      command: 'set_runtime_scale_control',
      ok: true,
    });
    expect(accepted).toMatchObject({
      requestId: 'request-scale-1',
      controlGeneration: 4,
      requestedScale: 0.25,
      activeOperationId: 'operation-1',
    });
  });

  it('parses synchronized runtime-scale status only when its member set and scopes agree', () => {
    const selector = {
      program_binding_id: 'binding-program-1',
      group_targets: [
        { component_role_id: 'robot', target_role_id: 'arm' },
        { component_role_id: 'robot', target_role_id: 'gripper' },
      ],
    };
    const member = (target_role_id: string) => ({
      active_control_configuration_generation: 1,
      active_control_configuration_id: `active-${target_role_id}`,
      control_binding_id: `binding-${target_role_id}`,
      control_binding_revision: 'revision-1',
      control_domain_id: `domain-${target_role_id}`,
      control_domain_revision: 'revision-1',
      scaling_domain_id: `scale-${target_role_id}`,
      component_role_id: 'robot', target_role_id,
    });
    const memberFor = (component_role_id: string, target_role_id: string) => ({
      ...member(target_role_id), component_role_id, target_role_id,
    });
    const response = {
      api_version: 'manyforge.execution_command_response.v1', command: 'get_runtime_scale_control', ok: true,
      runtime_scale_control: {
        active_control_configuration_generation: null, active_control_configuration_id: null,
        active_operation_id: null, active_operation_revision: null, active_operation_scale_generation: null,
        application_state: null, control_binding_id: null, control_binding_revision: null,
        control_domain_id: null, control_domain_revision: null, control_generation: 1,
        effective_scale: null, frozen: false, reason: null, requested_scale: 0.5,
        scaling_domain_id: null, selector, supported: true,
        group_members: [
          member('arm'), member('gripper'),
        ],
      },
    };
    expect(parseRuntimeScaleControlStatus(response).selector).toEqual({
      kind: 'group', programBindingId: 'binding-program-1',
      groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    });
    expect(parseRuntimeScaleControlAccepted({
      accepted: {
        accepted_at_ns: 123, active_operation_id: null, active_operation_revision: null,
        active_operation_scale_generation: null, control_generation: 2,
        replay_status: 'accepted_new', request_id: 'request-group-1', requested_scale: 0.25,
        selector,
      },
      api_version: 'manyforge.execution_command_response.v1',
      command: 'set_runtime_scale_control', ok: true,
    }).selector).toMatchObject({ kind: 'group', programBindingId: 'binding-program-1' });
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        group_members: [member('arm'), member('arm')],
      },
    })).toThrow();
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        group_members: [member('arm'), member('other')],
      },
    })).toThrow();
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        selector: {
          program_binding_id: 'binding-program-1',
          group_targets: [
            { component_role_id: 'a', target_role_id: 'b\u0000c' },
            { component_role_id: 'a\u0000b', target_role_id: 'c' },
          ],
        },
        group_members: [
          memberFor('a', 'b\u0000c'), memberFor('a\u0000b', 'c'),
        ],
      },
    })).not.toThrow();
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        selector: {
          ...selector,
          program_binding_id: 'x'.repeat(257),
        },
      },
    })).toThrow();
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        group_members: [{
          ...member('arm'), active_control_configuration_generation: -1,
        }, member('gripper')],
      },
    })).toThrow();
    expect(() => parseRuntimeScaleControlStatus({
      ...response,
      runtime_scale_control: {
        ...response.runtime_scale_control,
        group_members: [{
          ...member('arm'), control_domain_id: '',
        }, member('gripper')],
      },
    })).toThrow();
  });

  it('accepts source-local limits and exactly one matching observation', () => {
    const exactRevision = `sha256:${'a'.repeat(64)}`;
    const contracts = {
      command_schema: null,
      control_path: {
        control_binding_id: 'binding-1',
        control_binding_revision: exactRevision,
        control_interface_contract_id: 'interface-1',
        control_interface_revision: exactRevision,
        controller_topology_id: 'topology-1',
        stop_evidence_policy_id: 'stop-policy-1',
        stop_evidence_policy_revision: exactRevision,
        topology_revision: exactRevision,
      },
      operation_capability_definition_revision: exactRevision,
      operation_capability_id: 'execution.trajectory.execute',
      supporting_providers: [{
        capability_definition_revision: exactRevision,
        capability_id: 'planning.motion.joint_goal',
        capability_provider_binding_id: 'provider-binding-1',
        capability_provider_binding_revision: exactRevision,
        provider_manifest_id: 'provider-1',
        provider_manifest_revision: exactRevision,
      }],
    };
    const response = {
      ok: true,
      command: 'status',
      status: {
        owner_epoch: 'owner-1',
        cell_id: 'cell-1',
        active_publication_generation: 1,
        cell_stop_action: {
          available: true,
          expected_stop_generation: 4,
          fault: null,
          latest_terminal: null,
          unavailable_reason: null,
        },
        graph_revision: 'sha256:graph',
        retained_bundle_id: 'sha256:bundle',
        scene_revision: sceneRevision,
        direct_control: {
          controllable_component_id: 'robot-a',
          command_target_id: 'arm',
          component_role_id: 'controlled_component',
          target_role_id: 'command_target',
          base_frame_role_id: 'base',
          tcp_frame_role_id: 'tcp',
          operation_capability_id: 'execution.trajectory.execute',
          planning_capability_ids: ['planning.motion.joint_goal'],
          frame_role_ids: ['base', 'tcp'],
          program_id: 'manyforge.direct_control.motion',
          program_revision: 'sha256:program',
          program_binding_id: 'sha256:binding',
          program_run_id: 'direct-control-owner-1',
          observed_state: {
            complete: true,
            current_tcp_pose: {
              position_m: { x_m: 0, y_m: 0, z_m: 0.4 },
              orientation: { x: 0, y: 0, z: 0, w: 1 },
            },
            positions_rad: [0.2],
            sequence: 7,
            source_epoch: 'source-1',
            source_joint_names: ['joint1'],
          },
          joints: [{
            source_joint_name: 'joint1',
            min_position_rad: -1,
            max_position_rad: 1,
            max_velocity_rad_s: 2,
            max_acceleration_rad_s2: null,
          }],
          target_descriptor: {
            schema_version: 'manyforge.manual_control_motion_descriptor.v5',
            ordinary_jerk: null,
            controllable_component_id: 'robot-a',
            command_target_id: 'arm',
            component_role_id: 'controlled_component',
            target_role_id: 'command_target',
            frames: [
              { frame_role_id: 'base', compiled_frame_name: 'base_link', model_instance_id: 'robot-model', model_revision: exactRevision },
              { frame_role_id: 'tcp', compiled_frame_name: 'tool0', model_instance_id: 'robot-model', model_revision: exactRevision },
            ],
            joints: [{
              compiled_joint_name: 'robot-a__joint1',
              source_joint_name: 'joint1',
              min_position_rad: -1,
              max_position_rad: 1,
              max_velocity_rad_s: 2,
              max_acceleration_rad_s2: null,
              unit: 'rad',
            }],
            operational_joint_limits: [{
              source_joint_name: 'joint1',
              model_max_velocity_rad_s: 2,
              model_max_acceleration_rad_s2: null,
              deployment_max_velocity_rad_s: 1.5,
              deployment_max_acceleration_rad_s2: 2.5,
              deployment_max_jerk_rad_s3: 10,
              effective_max_velocity_rad_s: 1.5,
              effective_max_acceleration_rad_s2: 2.5,
              effective_max_jerk_rad_s3: 10,
            }],
            operational_limit: null,
            contracts,
          },
        },
        direct_control_gripper: {
          command_schema_id: 'gripper.command_position',
          command_schema_revision: exactRevision,
          command_target_id: 'gripper',
          component_role_id: 'controlled_component',
          controllable_component_id: 'robot-a',
          observed_state: {
            complete: true,
            position: { kind: 'linear_m', value_m: 0.02 },
            sequence: 8,
            source_coordinate_name: 'finger_joint',
            source_epoch: 'gripper-source-1',
          },
          operation_capability_id: 'tool.gripper.command',
          program_binding_id: 'sha256:gripper-binding',
          program_id: 'manyforge.direct_control.gripper',
          program_revision: 'sha256:gripper-program',
          program_run_id: 'direct-control-gripper-owner-1',
          resource: {
            coordinate_kind: 'linear_m',
            max_position: 0.04,
            min_position: 0,
            source_coordinate_name: 'finger_joint',
            source_kind: 'model_role',
          },
          target_role_id: 'command_target',
          target_descriptor: {
            schema_version: 'manyforge.manual_control_gripper_descriptor.v2',
            controllable_component_id: 'robot-a',
            command_target_id: 'gripper',
            component_role_id: 'controlled_component',
            target_role_id: 'command_target',
            description: 'Test gripper',
            command_kind: 'gripper_position',
            actuation_kind: 'position_commanded',
            transport_kind: 'resolved_binding',
            control_interface_contract_id: 'interface-1',
            state_authority_id: 'state.gripper',
            state_authority_class: 'measured',
            completion_policy: 'goal_fenced_target_reached',
            grasp_confirmation_policy: 'binding_feedback_required',
            source_group_name: 'gripper',
            source_coordinate_name: 'finger_joint',
            source_kind: 'model_role',
            coordinate_kind: 'linear_m',
            unit: 'm',
            min_position: 0,
            max_position: 0.04,
            open_state_ref: 'open',
            closed_state_ref: 'closed',
            command_duration_s: 0.5,
            touch_links: ['finger_link'],
            mount_frame: 'tool0',
            attach_frame: 'tool0',
            tcp_frame: 'tool0',
            auxiliary_frames: [],
            mimic_followers: [],
            named_presets: [{
              name: 'open',
              master_position: 0.04,
              coordinates: [{ source_coordinate_name: 'finger_joint', position: 0.04 }],
            }, {
              name: 'closed',
              master_position: 0,
              coordinates: [{ source_coordinate_name: 'finger_joint', position: 0 }],
            }],
            contracts: {
              ...contracts,
              command_schema: {
                command_schema_id: 'gripper.command_position',
                command_schema_revision: exactRevision,
              },
              operation_capability_id: 'tool.gripper.command',
              supporting_providers: [],
            },
          },
        },
        observed_targets: [],
      },
    };
    const status = parseExecutionOwnerStatus(response);

    expect(status.directControl.baseFrameRoleId).toBe('base');
    expect(status.observedMotion.sourceJointNames).toEqual(['joint1']);
    expect(status.observedMotion.positionsRad).toEqual([0.2]);
    expect(status.observedMotion.currentTcpPose.position_m.z_m).toBe(0.4);
    expect(status.observedGripper?.position).toBe(0.02);
    expect(status.cellStopAction.expectedStopGeneration).toBe(4);
    expect(status.directControl.targetDescriptor.operationalJointLimits[0])
      .toMatchObject({
        deploymentMaxVelocityRadS: 1.5,
        effectiveMaxVelocityRadS: 1.5,
        effectiveMaxAccelerationRadS2: 2.5,
        effectiveMaxJerkRadS3: 10,
      });

    const identity = {
      active_publication_generation: 1,
      cell_id: 'cell-1',
      control_management_mode: 'component_lifecycle',
      graph_revision: 'sha256:graph',
      gripper_command_schema_id: 'gripper.command_position',
      gripper_command_schema_revision: exactRevision,
      gripper_target_role_id: 'command_target',
      motion_target_role_id: 'command_target',
      owner_epoch: 'owner-1',
      program_binding_id: 'sha256:binding',
      program_id: 'manyforge.direct_control.motion',
      program_revision: 'sha256:program',
      retained_bundle_id: 'sha256:bundle',
      scene_revision: sceneRevision,
      schema_version: 'manyforge.execution_identity.v1',
    };
    const parsedIdentity = parseExecutionIdentity({
      command: 'identity', identity, ok: true,
      api_version: 'manyforge.execution_command_response.v1',
    });
    expect(parsedIdentity.ownerEpoch).toBe('owner-1');
    expect(parsedIdentity.controlManagementMode).toBe('component_lifecycle');
    const {
      observed_state: observedMotion,
      ...motionTarget
    } = structuredClone(response.status.direct_control);
    const {
      observed_state: observedGripper,
      ...gripperTarget
    } = structuredClone(response.status.direct_control_gripper);
    const targetsResponse = {
        api_version: 'manyforge.execution_command_response.v1',
        command: 'targets',
        identity,
        ok: true,
        targets: {
          direct_control: motionTarget,
          direct_control_gripper: gripperTarget,
          schema_version: 'manyforge.execution_targets.v1',
        },
    };
    const controlResponse = {
        api_version: 'manyforge.execution_command_response.v1',
        command: 'control_state',
        control_state: {
          cell_stop_action: response.status.cell_stop_action,
          observed_gripper: observedGripper,
          observed_motion: observedMotion,
          manual_operations: [] as unknown[],
          runtime_scale_control: {
            active_control_configuration_generation: 1,
            active_control_configuration_id: 'active-1',
            active_operation_id: null,
            active_operation_revision: null,
            active_operation_scale_generation: null,
            application_state: null,
            control_binding_id: 'binding-1',
            control_binding_revision: exactRevision,
            control_domain_id: 'domain-1',
            control_domain_revision: exactRevision,
            control_generation: 0,
            effective_scale: null,
            frozen: false,
            reason: null,
            requested_scale: 1,
            scaling_domain_id: 'scale-1',
            selector: {
              component_role_id: 'controlled_component',
              program_binding_id: 'sha256:binding',
              target_role_id: 'command_target',
            },
            supported: true,
          },
          schema_version: 'manyforge.execution_control_state.v4',
        },
        identity,
        ok: true,
    };
    const scoped = parseExecutionControlStatus(targetsResponse, controlResponse);
    const addressedTargets = structuredClone(targetsResponse);
    addressedTargets.targets.schema_version = 'manyforge.execution_targets.v2';
    Object.assign(addressedTargets.targets, {
      direct_control_targets: [motionTarget],
      direct_control_gripper_targets: [gripperTarget],
    });
    const addressedControl: { control_state: Record<string, unknown> } = structuredClone(controlResponse);
    addressedControl.control_state.schema_version = 'manyforge.execution_control_state.v5';
    addressedControl.control_state.observed_target_states = [{
      command_target_id: motionTarget.command_target_id,
      controllable_component_id: motionTarget.controllable_component_id,
      gripper: null,
      gripper_target_role_id: gripperTarget.target_role_id,
      motion: observedMotion,
      motion_target_role_id: motionTarget.target_role_id,
    }, {
      command_target_id: gripperTarget.command_target_id,
      controllable_component_id: gripperTarget.controllable_component_id,
      gripper: observedGripper,
      gripper_target_role_id: gripperTarget.target_role_id,
      motion: null,
      motion_target_role_id: motionTarget.target_role_id,
    }];
    const addressed = parseExecutionControlStatus(addressedTargets, addressedControl);
    expect(addressed.directControlTargets).toHaveLength(1);
    expect(addressed.observedMotions?.[0]).toMatchObject({
      controllableComponentId: motionTarget.controllable_component_id,
      commandTargetId: motionTarget.command_target_id,
    });
    expect(addressed.observedGrippers?.[0]).toMatchObject({
      controllableComponentId: gripperTarget.controllable_component_id,
      commandTargetId: gripperTarget.command_target_id,
    });
    const groupedControlResponse: { control_state: Record<string, unknown> } = structuredClone(controlResponse);
    groupedControlResponse.control_state.runtime_scale_control = {
      ...controlResponse.control_state.runtime_scale_control,
      active_control_configuration_generation: null,
      active_control_configuration_id: null,
      control_binding_id: null,
      control_binding_revision: null,
      control_domain_id: null,
      control_domain_revision: null,
      scaling_domain_id: null,
      selector: {
        program_binding_id: 'sha256:binding',
        group_targets: [
          { component_role_id: 'controlled_component', target_role_id: 'command_target' },
          { component_role_id: 'controlled_component', target_role_id: 'gripper_target' },
        ],
      },
      group_members: [
        {
          active_control_configuration_generation: 1,
          active_control_configuration_id: 'active-motion',
          control_binding_id: 'binding-motion', control_binding_revision: exactRevision,
          control_domain_id: 'domain-motion', control_domain_revision: exactRevision,
          scaling_domain_id: 'scale-motion', component_role_id: 'controlled_component',
          target_role_id: 'command_target',
        },
        {
          active_control_configuration_generation: 1,
          active_control_configuration_id: 'active-gripper',
          control_binding_id: 'binding-gripper', control_binding_revision: exactRevision,
          control_domain_id: 'domain-gripper', control_domain_revision: exactRevision,
          scaling_domain_id: 'scale-gripper', component_role_id: 'controlled_component',
          target_role_id: 'gripper_target',
        },
      ],
    };
    expect(
      parseExecutionControlStatus(targetsResponse, groupedControlResponse)
        .runtimeScaleControl?.selector,
    ).toMatchObject({ kind: 'group', programBindingId: 'sha256:binding' });
    const retired = {
      resident: false, operation: {
        program_run_id: motionTarget.program_run_id, operation_id: 'manual-1', operation_revision: 2,
        state: 'completed', participants: [{progress: 1, terminal_error: null}],
        active_stop_id: null, active_stop_generation: null,
      },
    };
    controlResponse.control_state.manual_operations = [retired];
    expect(parseExecutionControlStatus(targetsResponse, controlResponse).manualOperations[0])
      .toMatchObject({operationId: 'manual-1', resident: false, state: 'completed'});
    for (const rows of [
      [{...retired, operation: {...retired.operation, program_run_id: 'foreign'}}],
      [{...retired, operation: {...retired.operation, state: 'executing'}}],
      [retired, {...retired, operation: {...retired.operation, operation_id: 'duplicate-target'}}],
      [retired, retired, retired],
    ]) {
      controlResponse.control_state.manual_operations = rows;
      expect(() => parseExecutionControlStatus(targetsResponse, controlResponse)).toThrow();
    }
    expect(scoped.directControl.targetDescriptor).toEqual(
      status.directControl.targetDescriptor,
    );
    expect(scoped.observedMotion.positionsRad).toEqual([0.2]);

    const stopped = structuredClone(response);
    Object.assign(stopped.status.cell_stop_action, {
      latest_terminal: {
        certificate: {
          stop_certificate_id: 'stop-certificate.4',
          stop_certificate_revision: 1,
          stop_generation: 4,
          stop_id: 'stop.4',
        },
        escalated: true,
        executed_stages: [{
          action_class: 'controlled',
          executed_mode: 'hard_deceleration',
          operation_id: 'operation.1',
          outcome: 'succeeded',
          participant_id: 'participant.1',
          stage_id: 'controlled.1',
          stage_index: 1,
          substitution_reason: 'soft mode unavailable',
        }],
        hard_stop_floor_used: false,
        result: 'succeeded_escalated',
      },
      fault: {
        certificate: null,
        latch_generation: 1,
        latch_id: 'stop-fault.1',
        reason_code: 'stop_escalated',
        recovery_status: 'waiting_for_observation',
        state: 'latched',
        trigger: 'partial',
      },
    });
    const stoppedStatus = parseExecutionOwnerStatus(stopped);
    expect(stoppedStatus.cellStopAction.latestTerminal?.certificate.stopId)
      .toBe('stop.4');
    expect(stoppedStatus.cellStopAction.latestTerminal?.executedStages[0])
      .toMatchObject({ executedMode: 'hard_deceleration', stageIndex: 1 });
    expect(stoppedStatus.cellStopAction.fault?.latchId).toBe('stop-fault.1');

    const componentLocal = structuredClone(response);
    Object.assign(componentLocal.status.direct_control_gripper.resource, {
      source_kind: 'component_local',
      min_position: null,
      max_position: null,
    });
    Object.assign(componentLocal.status.direct_control_gripper.target_descriptor, {
      source_kind: 'component_local',
      min_position: null,
      max_position: null,
    });
    Object.assign(componentLocal.status.direct_control_gripper, {
      observed_state: null,
    });
    const localStatus = parseExecutionOwnerStatus(componentLocal);
    expect(localStatus.directControlGripper.sourceKind).toBe('component_local');
    expect(localStatus.directControlGripper.minPosition).toBeNull();
    expect(localStatus.observedGripper).toBeNull();

    const leaked = structuredClone(response);
    Object.assign(leaked.status.direct_control, {
      compiled_joint_names: ['robot-a__joint1'],
    });
    expect(() => parseExecutionOwnerStatus(leaked)).toThrow(/exactly/);

    const crossedDescriptor = structuredClone(response);
    crossedDescriptor.status.direct_control.target_descriptor.frames.reverse();
    expect(() => parseExecutionOwnerStatus(crossedDescriptor)).toThrow(
      /exact addressed targets/,
    );

    const dishonestLimit = structuredClone(response);
    dishonestLimit.status.direct_control.target_descriptor
      .operational_joint_limits[0].effective_max_velocity_rad_s = 2;
    expect(() => parseExecutionOwnerStatus(dishonestLimit)).toThrow(
      /exact conservative Deployment\/model minimum/,
    );

    const ordinary = structuredClone(response);
    const jerkRow = {
      source_joint_name: 'joint1', source: 'imported', maximum_rad_s3: 6,
      preset_rad_s3: null,
      imported: { maximum_rad_s3: 6, enabled: true, machine_required: false,
        source_id: 'robot_moveit_config/config/joint_limits.yaml', source_digest: exactRevision },
    };
    Object.assign(ordinary.status.direct_control.target_descriptor, {
      ordinary_jerk: { enabled: true, preset_reference_time_s: 1, joint_limits: [jerkRow] },
    });
    Object.assign(ordinary.status.direct_control.target_descriptor.operational_joint_limits[0], {
      deployment_max_jerk_rad_s3: null, effective_max_jerk_rad_s3: 6,
    });
    expect(parseExecutionOwnerStatus(ordinary).directControl.targetDescriptor.ordinaryJerk)
      .toMatchObject({ enabled: true, jointLimits: [{ maximumRadS3: 6, source: 'imported' }] });
    jerkRow.maximum_rad_s3 = 7;
    expect(() => parseExecutionOwnerStatus(ordinary)).toThrow(/effective jerk limits/);
    jerkRow.maximum_rad_s3 = 6;
    jerkRow.source_joint_name = 'foreign-joint';
    expect(() => parseExecutionOwnerStatus(ordinary)).toThrow(/exact ordered model joints/);
  });

  it('accepts only a no-motion receipt with equal source arrays', () => {
    const response = {
      ok: true,
      command: 'preview_motion',
      preview_receipt: {
        schema_version: 'manyforge.direct_control_motion_preview.v1',
        receipt_digest: 'sha256:receipt',
        intent_digest: 'sha256:intent',
        request_id: 'request-1',
        request_owner_epoch: 'owner-1',
        program_id: 'manyforge.direct_control.motion',
        program_revision: 'sha256:program',
        program_binding_id: 'sha256:binding',
        program_run_id: 'direct-control-owner-1',
        graph_revision: 'sha256:graph',
        retained_bundle_id: 'sha256:bundle',
        scene_revision: sceneRevision,
        component_role_id: 'controlled_component',
        target_role_id: 'command_target',
        required_capability_id: 'execution.trajectory.execute',
        source_joint_names: ['joint1'],
        start_positions_rad: [0],
        final_positions_rad: [0.4],
        final_tcp_pose: {
          position_m: { x_m: 0, y_m: 0, z_m: 0.4 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
        collision_world_revision: {
          revision_id: 2,
          content_hash: 'collision-2',
        },
        commit_eligible: true,
      },
    };
    const receipt = parseMotionPreviewResponse(response);

    expect(receipt.finalPositionsRad).toEqual([0.4]);
    expect(receipt.commitEligible).toBe(true);

    const undeclared = structuredClone(response);
    Object.assign(undeclared.preview_receipt, { trajectory: [] });
    expect(() => parseMotionPreviewResponse(undeclared)).toThrow(
      /exactly/,
    );
  });

  it('accepts an exact unit-tagged capability-command preview', () => {
    const response = {
      ok: true,
      command: 'preview_capability_command',
      preview_receipt: {
        schema_version:
          'manyforge.direct_control_capability_command_preview.v1',
        receipt_digest: 'sha256:receipt',
        intent_digest: 'sha256:intent',
        request_id: 'request-gripper-1',
        request_owner_epoch: 'owner-1',
        program_id: 'manyforge.direct_control.gripper',
        program_revision: 'sha256:gripper-program',
        program_binding_id: 'sha256:gripper-binding',
        program_run_id: 'direct-control-gripper-owner-1',
        graph_revision: 'sha256:graph',
        retained_bundle_id: 'sha256:bundle',
        scene_revision: sceneRevision,
        component_role_id: 'controlled_component',
        target_role_id: 'command_target',
        required_capability_id: 'tool.gripper.command',
        command_schema_id: 'gripper.command_position',
        command_schema_revision: 'sha256:gripper-schema',
        source_coordinate_name: 'finger_joint',
        source_kind: 'model_role',
        coordinate_kind: 'linear_m',
        target_position: 0.03,
        min_position: 0,
        max_position: 0.04,
        commit_eligible: true,
      },
    };
    const receipt = parseCapabilityCommandPreviewResponse(response);

    expect(receipt.targetPosition).toBe(0.03);
    expect(receipt.coordinateKind).toBe('linear_m');

    const componentLocal = structuredClone(response);
    Object.assign(componentLocal.preview_receipt, {
      source_kind: 'component_local',
      min_position: null,
      max_position: null,
    });
    expect(parseCapabilityCommandPreviewResponse(componentLocal).sourceKind)
      .toBe('component_local');

    const undeclaredNested = structuredClone(response);
    Object.assign(undeclaredNested.preview_receipt.scene_revision, {
      private_generation: 7,
    });
    expect(() => parseCapabilityCommandPreviewResponse(undeclaredNested))
      .toThrow(/exactly/);
  });
});

import { describe, expect, it } from 'vitest';
import {
  StructuralContractError,
  parseAppliedProgramSelection,
  parseCapabilityCommandPreviewResponse,
  parseCatalogResponse,
  parseExecutionOwnerStatus,
  parseHealthResponse,
  parseHeadlessRuntimeSnapshot,
  parseHistorianPage,
  parseManagerDiffProjection,
  parseManagerGraphProjection,
  parseManagerStatusProjection,
  parseOperatorAlarmSnapshot,
  parseMotionPreviewResponse,
  parseNodeEnvironmentResponse,
  parseLogicalSceneProjection,
  parseProgramWorkspaceResponse,
  parseProgramWorkspaceTransitionResponse,
  parseProgramRunIndex,
  parseProgramRunStatus,
  parseScenarioCatalogResponse,
  parseSceneRuntimeProjection,
  parseSceneMeshAssetMetadata,
  parseSceneTransactionResponse,
  parseSceneUrdfManifest,
  parseSourceModelProjection,
} from './parsers';

function programFixture(): Record<string, unknown> {
  return {
    api_version: 'manyforge.program.v1',
    program_id: 'fixture',
    program_revision: `sha256:${'a'.repeat(64)}`,
    program_profile: 'composable-cell-v1',
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v2',
      content_version: '1.1.0',
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
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v1',
    draft: {
      draftId: 'draft-1',
      draftRevision: 2,
      programLoaded: true,
      programRevision: program.program_revision,
      program,
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

function programBindingFixture(
  programId: string,
  programRevision: string,
  programBindingId: string,
): Record<string, unknown> {
  return {
    api_version: 'manyforge.program_binding.v1',
    program_binding_id: programBindingId,
    program_id: programId,
    program_revision: programRevision,
    retained_bundle_id: `sha256:${'b'.repeat(64)}`,
    graph_revision: `sha256:${'c'.repeat(64)}`,
    node_environment: {
      catalog_id: 'manyforge.default',
      content_version: '1.1.0',
      catalog_digest: `sha256:${'d'.repeat(64)}`,
      environment_digest: `sha256:${'a'.repeat(64)}`,
    },
    component_role_bindings: [],
    target_role_bindings: [],
    contact_grasp_bindings: [],
    provenance: {},
  };
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
    schema_version: 'manyforge.source_model_projection.v1',
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
});

describe('Program product projections', () => {
  const digestA = `sha256:${'a'.repeat(64)}`;
  const digestB = `sha256:${'b'.repeat(64)}`;
  const digestC = `sha256:${'c'.repeat(64)}`;

  function runStatus(state = 'running'): Record<string, unknown> {
    return {
      schema_version: 'manyforge.program_run_status.v1',
      owner_id: 'program-runner.cell-a',
      owner_epoch: 'program-runner.epoch.1',
      run_revision: 3,
      state,
      capture: {
        schema_version: 'manyforge.program_run_capture.v1',
        program_run_id: 'program-run.1',
        scenario_id: 'cell-pick',
        scenario_catalog_revision: digestA,
        program_id: 'pick',
        program_revision: digestB,
        program_binding_id: digestC,
        retained_bundle_id: digestA,
        graph_revision: digestB,
        node_environment_digest: digestC,
        owner_identities: [{
          owner_role: 'program_runner',
          owner_id: 'program-runner.cell-a',
          owner_epoch: 'program-runner.epoch.1',
        }],
      },
      tree_status: 'running',
      active_operation_count: 1,
      reason_code: null,
      message: '',
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
    expect(applied.recordDigest).toBe(digestC);
    expect(applied.sourceOwnerEpoch).toBe('headless.epoch.1');
    expect(applied.program.program_id).toBe('fixture');
    expect(applied.programBinding.program_binding_id).toBe(digestC);
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
    })).toThrow(/exactly/);
  });

  it('keeps run status and index tied to one exact runner owner', () => {
    const status = parseProgramRunStatus(runStatus());
    const index = parseProgramRunIndex({
      schema_version: 'manyforge.program_run_index.v1',
      owner_id: 'program-runner.cell-a',
      owner_epoch: 'program-runner.epoch.1',
      active_only: true,
      truncated: false,
      runs: [runStatus()],
    });

    expect(status.capture.programRunId).toBe('program-run.1');
    expect(index.runs[0].runRevision).toBe(3);
    expect(() => parseProgramRunIndex({
      schema_version: 'manyforge.program_run_index.v1',
      owner_id: 'program-runner.cell-a',
      owner_epoch: 'another-epoch',
      active_only: true,
      truncated: false,
      runs: [runStatus()],
    })).toThrow(/exact indexed owner/);
  });

  it('parses Composer Apply, Load, and Revert wrappers without trusting headless details', () => {
    const response = parseProgramWorkspaceTransitionResponse({
      status: 'applied',
      headless: { schema_version: 'manyforge.program_apply_result.v1' },
      workspace: workspaceFixture(),
    });

    expect(response.status).toBe('applied');
    expect(response.workspace.draft.programLoaded).toBe(true);
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
      object_axes_gizmo_size_m: 0.15,
    };

    const parsed = parseProgramWorkspaceResponse(fixture);
    expect(parsed.draft.program?.scene?.[0]?.shape.type).toBe('sphere');
    expect(parsed.draft.program?.viewer?.object_axes_gizmo_size_m).toBe(0.15);

    ((program.scene as Record<string, unknown>[])[0].shape as Record<string, unknown>).radius = 1;
    expect(() => parseProgramWorkspaceResponse(fixture)).toThrow(
      'exactly [sphere_radius_m, type]',
    );
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

  it('parses exact good and unknown ROS observation payloads', () => {
    const fixture = sceneRuntimeFixture();
    const state = fixture.state as Record<string, unknown>;
    state.pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v1',
        joint_names: ['joint_1', 'joint_2'],
        kind: 'joint_state',
        positions: [0.1, -0.2],
        source_stamp_s: 12.5,
        topic: '/isaac_joint_states',
      }),
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v1',
        kind: 'simulation_entity_pose',
        pose: {
          frame_id: 'universe',
          frame_kind: 'cell_frame',
          orientation_xyzw: [0, 0, 0, 1],
          position_m: [0.4, -0.2, 0.1],
          source_stamp_s: 13,
        },
        source: { entity: '/mac_n_cheese_centered' },
      }, {
        eligible: true,
        object_id: 'isaac_cheese_box',
        policy_id: 'continuous_unattached',
        source_sequence: 8,
      }),
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v1',
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
        api_version: 'manyforge.ros_pose_observation.v1',
        kind: 'simulation_entity_pose',
        pose: {
          frame_id: 'world',
          frame_kind: 'unmapped_frame',
          orientation_xyzw: [0, 0, 0, 1],
          position_m: [0, 0, 0],
          source_stamp_s: null,
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
        api_version: 'manyforge.ros_pose_observation.v1',
        joint_names: ['joint_1', 'joint_1'],
        kind: 'joint_state',
        positions: [0.1, 0.2],
        source_stamp_s: null,
        topic: '/isaac_joint_states',
      }),
    ];
    expect(() => parseSceneRuntimeProjection(duplicateJoint)).toThrow(
      'equal non-empty unique joint_names',
    );

    const dishonestUnknown = sceneRuntimeFixture();
    (dishonestUnknown.state as Record<string, unknown>).pose_sources = [
      poseSourceFixture({
        api_version: 'manyforge.ros_pose_observation.v1',
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
    const status = parseManagerStatusProjection({
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
          records: [],
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
    });
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
    expect(graph.retainedBundleId).toBe('bundle.1');
    expect(diff.converged).toBe(true);
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
    const page = parseHistorianPage({
      schema_version: 'manyforge.historian_page.v1',
      events: [{
        sequence: 7,
        event: {
          schema_version: 'manyforge.historian_event.v1',
          event_id: 'event-1',
          component: 'manyforge.alarm',
          event_type: 'operator_alarm_activated',
          severity: 'INFO',
          occurred_at_ns: 123,
          occurred_at_wall: '2026-08-05T12:00:00Z',
          payload: { condition_id: 'condition-1' },
        },
      }],
      next_before_sequence: null,
      truncated: false,
    });

    expect(page.events[0]?.event.eventType).toBe('operator_alarm_activated');
    expect(page.events[0]?.sequence).toBe(7);
  });
});

describe('addressed Robot Control parsers', () => {
  const sceneRevision = {
    scene_id: 'scene-1',
    revision_id: 3,
    content_hash: 'sha256:scene',
  };

  it('accepts source-local limits and exactly one matching observation', () => {
    const response = {
      ok: true,
      command: 'status',
      status: {
        owner_epoch: 'owner-1',
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
        },
        direct_control_gripper: {
          command_schema_id: 'gripper.command_position',
          command_schema_revision: 'sha256:gripper-schema',
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

    const componentLocal = structuredClone(response);
    Object.assign(componentLocal.status.direct_control_gripper.resource, {
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

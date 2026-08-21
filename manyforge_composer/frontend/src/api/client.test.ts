import { afterEach, describe, expect, it, vi } from 'vitest';
import nativeBindingFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { ApiError, api, resolveStructuralAssetUrl } from './client';
import type { ManagerApplyRequest } from '../types/api';
import type { OperatorAlarmRecord } from '../types/headless';

afterEach(() => {
  vi.unstubAllGlobals();
});

function response(
  payload: unknown,
  options: { ok?: boolean; status?: number; statusText?: string } = {},
): Response {
  return {
    ok: options.ok ?? true,
    status: options.status ?? 200,
    statusText: options.statusText ?? 'OK',
    json: vi.fn().mockResolvedValue(payload),
  } as unknown as Response;
}

function workspaceFixture(): Record<string, unknown> {
  const revision = `sha256:${'a'.repeat(64)}`;
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v1',
    draft: {
      draftId: 'draft-1',
      draftRevision: 1,
      programLoaded: true,
      programRevision: revision,
      program: {
        api_version: 'manyforge.program.v1',
        program_id: 'fixture',
        program_revision: revision,
        program_profile: 'composable-cell-v1',
        name: 'Fixture',
        catalog: {
          api_version: 'manyforge.node_catalog.v2',
          content_version: '1.1.0',
        },
        component_roles: {},
        target_roles: {},
        tree: {
          node_id: 'timer',
          instance_id: 'wait',
          parameters: { duration_s: 0.5 },
          children: [],
        },
      },
    },
    saved: {
      available: true,
      matchesDraft: true,
      path: '/workspace/fixture.program.yaml',
      programRevision: revision,
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
    hasUnsavedChanges: false,
    hasUnappliedChanges: null,
  };
}

function appliedResultFixture(): Record<string, unknown> {
  const digestA = `sha256:${'a'.repeat(64)}`;
  const digestB = `sha256:${'b'.repeat(64)}`;
  const digestC = `sha256:${'c'.repeat(64)}`;
  const binding = structuredClone(nativeBindingFixture) as Record<string, unknown>;
  binding.program_binding_id = digestC;
  binding.program_id = 'fixture';
  binding.program_revision = digestA;
  binding.retained_bundle_id = digestB;
  binding.graph_revision = digestC;
  (binding.node_environment as Record<string, unknown>).environment_digest = digestA;
  return {
    schema_version: 'manyforge.program_apply_result.v1',
    status: 'applied',
    deployment_apply_required: false,
    preparation: {},
    manager_response: null,
    applied: {
      assistant_mode: 'forbidden',
      baseline_program_binding_id: digestA,
      deployment_artifact_id: 'cell-a',
      deployment_revision: digestB,
      graph_revision: digestC,
      node_environment_digest: digestA,
      preparation_digest: digestB,
      program: (workspaceFixture().draft as Record<string, unknown>).program,
      program_binding: binding,
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
    },
  };
}

function managerSourceFixture(): Record<string, unknown> {
  return {
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
}

function managerGraphFixture(): Record<string, unknown> {
  return {
    schema_version: 'manyforge.manager_graph_projection.v1',
    source: managerSourceFixture(),
    graph_revision: 'graph.9',
    retained_bundle_id: 'bundle.9',
    cell_graph: { cell_id: 'cell-a', components: [] },
  };
}

function managerDiffFixture(): Record<string, unknown> {
  return {
    schema_version: 'manyforge.manager_diff_projection.v1',
    source: managerSourceFixture(),
    converged: true,
    desired_units: [],
    observed_units: [],
  };
}

function managerApplyFixture(request: ManagerApplyRequest): Record<string, unknown> {
  return {
    api_version: 'manyforge.manager_command_response.v1',
    ok: true,
    error: null,
    request_id: request.request_id,
    request_owner_epoch: request.request_owner_epoch,
  };
}

describe('structural API client', () => {
  it('uses the owner mode index and bridge-owned request status routes', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response({
        schema_version: 'manyforge.assistant_mode_index.v1',
        retained_bundle_id: 'bundle.1',
        graph_revision: 'graph.1',
        assistant_projection_digest: `sha256:${'1'.repeat(64)}`,
        instrument_registry_digest: `sha256:${'2'.repeat(64)}`,
        node_environment: {
          catalog_digest: `sha256:${'5'.repeat(64)}`,
          catalog_id: 'manyforge',
          content_version: '2',
          environment_digest: `sha256:${'6'.repeat(64)}`,
        },
        modes: [],
        source: {
          available: true,
          freshness: 'fresh',
          known_gaps: [],
          projection_generation: 1,
          projection_owner_epoch: 'headless.1',
          receiver_age_ns: 1,
          selection_projection_generation: 1,
          selection_source_content_digest: `sha256:${'3'.repeat(64)}`,
          selection_source_generation: 1,
          selection_source_owner_epoch: 'manager.1',
          selection_source_owner_id: 'manager',
          source_content_digest: `sha256:${'4'.repeat(64)}`,
          source_generation: 1,
          source_owner_epoch: 'headless.1',
          source_owner_id: 'headless.assistant-mode-index',
          source_owner_kind: 'assistant_mode_index',
        },
      }))
      .mockResolvedValueOnce(response({
        schemaVersion: 'manyforge.assistant.provider_status.v1',
        requestId: 'request.1',
        requestOwnerEpoch: 'page.1',
        sourceLabel: 'composer_ui',
        correlationId: null,
        conversationId: 'conversation.1',
        assistantMode: 'query',
        state: 'running',
        response: null,
        error: null,
      }));
    vi.stubGlobal('fetch', fetchMock);

    await api.assistant.modes();
    await api.assistant.status('request.1');

    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/assistant/modes');
    expect(fetchMock.mock.calls[1]?.[0]).toBe('/api/assistant/requests/request.1');
  });

  it('encodes sandbox browse parameters and validates the response', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      currentPath: '/workspace/cells',
      parentPath: '/workspace',
      roots: ['/workspace'],
      entries: [
        {
          name: 'cell.deployment.yaml',
          path: '/workspace/cells/cell.deployment.yaml',
          entryType: 'file',
          selectable: true,
        },
      ],
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.files.browse('deployment', '/workspace/cells');

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/files/browse?kind=deployment&path=%2Fworkspace%2Fcells',
      expect.objectContaining({ method: 'GET' }),
    );
    expect(result.entries[0]?.selectable).toBe(true);
  });

  it('preserves typed HTTP status and backend detail', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(
      { detail: 'No URDF available for the active deployment' },
      { ok: false, status: 404, statusText: 'Not Found' },
    )));

    const error = await api.urdf.manifest().catch((caught: unknown) => caught);

    expect(error).toBeInstanceOf(ApiError);
    expect(error).toMatchObject({
      status: 404,
      message: 'No URDF available for the active deployment',
    });
  });

  it('reports invalid successful payloads rather than inventing defaults', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response({ status: 'ok' })));

    await expect(api.health.get()).rejects.toThrow(
      'Invalid response from /api/health',
    );
  });

  it('submits one exact revision-fenced Draft mutation', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      mutation: 'tree_draft_update_node_params',
      details: { updated_instance_id: 'wait' },
      workspace: workspaceFixture(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.programWorkspace.mutate(
      { draftId: 'draft-1', expectedDraftRevision: 1 },
      'tree_draft_update_node_params',
      {
        instance_id: 'wait',
        expected_node_id: 'timer',
        parameters: { duration_s: 1 },
      },
    );

    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/program/draft/mutations');
    expect(JSON.parse(String(options.body))).toEqual({
      draftId: 'draft-1',
      expectedDraftRevision: 1,
      mutation: 'tree_draft_update_node_params',
      arguments: {
        instance_id: 'wait',
        expected_node_id: 'timer',
        parameters: { duration_s: 1 },
      },
    });
    expect(result.workspace.saved.matchesDraft).toBe(true);
  });

  it('forwards Apply and bounded runner reads without client-owned state', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response({
        status: 'applied',
        headless: appliedResultFixture(),
        workspace: workspaceFixture(),
      }))
      .mockResolvedValueOnce(response({
        schema_version: 'manyforge.program_run_index.v1',
        owner_id: 'program-runner.cell-a',
        owner_epoch: 'program-runner.epoch.1',
        active_only: true,
        truncated: false,
        runs: [],
      }));
    vi.stubGlobal('fetch', fetchMock);

    await api.programWorkspace.apply(
      { draftId: 'draft-1', expectedDraftRevision: 1 },
      'cell-pick',
      `sha256:${'a'.repeat(64)}`,
    );
    await api.programs.runs(true, 1);

    const [applyUrl, applyOptions] = fetchMock.mock.calls[0] as [
      string,
      RequestInit,
    ];
    expect(applyUrl).toBe('/api/program/apply');
    expect(JSON.parse(String(applyOptions.body))).toEqual({
      draftId: 'draft-1',
      expectedDraftRevision: 1,
      scenarioId: 'cell-pick',
      scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
    });
    expect(fetchMock.mock.calls[1]?.[0]).toBe(
      '/api/program/runs?active_only=true&limit=1',
    );
  });

  it('submits exact fenced New, Discard, and Revert requests', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(workspaceFixture()))
      .mockResolvedValueOnce(response(workspaceFixture()))
      .mockResolvedValueOnce(response({
        status: 'reverted',
        headless: null,
        workspace: workspaceFixture(),
      }));
    vi.stubGlobal('fetch', fetchMock);
    const currentFence = { draftId: 'draft-1', expectedDraftRevision: 7 };

    await api.programWorkspace.newDraft(currentFence, true);
    await api.programWorkspace.discard(currentFence);
    await api.programWorkspace.revert(currentFence);

    expect(fetchMock.mock.calls.map(([url]) => url)).toEqual([
      '/api/program/new-draft',
      '/api/program/discard',
      '/api/program/revert',
    ]);
    expect(JSON.parse(String(
      (fetchMock.mock.calls[0]?.[1] as RequestInit).body,
    ))).toEqual({ ...currentFence, confirmDiscard: true });
    expect(JSON.parse(String(
      (fetchMock.mock.calls[1]?.[1] as RequestInit).body,
    ))).toEqual(currentFence);
    expect(JSON.parse(String(
      (fetchMock.mock.calls[2]?.[1] as RequestInit).body,
    ))).toEqual(currentFence);
  });
});

describe('resolveStructuralAssetUrl', () => {
  it('preserves absolute asset URLs and same-origin paths', () => {
    expect(resolveStructuralAssetUrl('https://assets.example/model.glb')).toBe(
      'https://assets.example/model.glb',
    );
    expect(resolveStructuralAssetUrl('/api/model.glb')).toMatch(/\/api\/model\.glb$/);
  });
});

describe('headless intent client', () => {
  it('submits one exact current-revision scene transaction', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      api_version: 'manyforge.manager_command_response.v1',
      committed_projection_revision: {
        retained_bundle_id: 'bundle-1',
        graph_revision: 'graph-1',
        scene_revision: {
          scene_id: 'scene-1',
          revision_id: 2,
          content_hash: 'scene-hash-2',
        },
      },
      error: null,
      ok: true,
      projection_status: 'committed',
      replay_status: 'accepted_new',
      request_id: 'request-scene-1',
      request_owner_epoch: 'scene-owner-epoch-7',
      transaction_sequence: 3,
      update_status: 'pose_changed',
    }));
    vi.stubGlobal('fetch', fetchMock);
    const transaction = {
      request_context: {
        request_id: 'request-scene-1',
        request_owner_epoch: 'scene-owner-epoch-7',
        source_label: 'composer_ui' as const,
        correlation_id: null,
      },
      expected_projection_revision: {
        retained_bundle_id: 'bundle-1',
        graph_revision: 'graph-1',
        scene_revision: {
          scene_id: 'scene-1',
          revision_id: 1,
          content_hash: 'scene-hash-1',
        },
      },
      operation: {
        kind: 'update_object_pose' as const,
        scene_resource_id: 'part',
        pose: {
          position_m: { x_m: 0.4, y_m: 0, z_m: 0.2 },
          orientation: { x: 0, y: 0, z: 0, w: 1 },
        },
        frame_ref: { kind: 'cell_frame' as const, cell_id: 'cell-a' },
        provider_evidence: null,
        expected_attached: false as const,
      },
    };

    const receipt = await api.scene.commit(transaction);

    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/scene/transactions');
    expect(options.method).toBe('POST');
    expect(JSON.parse(String(options.body))).toEqual(transaction);
    expect(receipt.committed_projection_revision.scene_revision.revision_id).toBe(2);
  });

  it('requests a bounded attention-newest alarm page', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({}));
    vi.stubGlobal('fetch', fetchMock);

    await expect(api.alarms.list()).rejects.toThrow('Invalid response');

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/execution/alarms?limit=32&ordering=attention_newest',
    );
  });

  it('posts manager apply with revision fences and calls manager graph endpoint', async () => {
    const request: ManagerApplyRequest = {
      request_id: 'request-manager-1',
      request_owner_epoch: 'owner.epoch.1',
      source_label: 'composer_ui',
      correlation_id: null,
      deployment_artifact_id: 'artifact-1',
      deployment_revision: 'rev-2',
      expected_current: {
        graph_revision: 'graph-9',
        retained_bundle_id: 'bundle-9',
      },
      reject_if_affected_busy: true,
    };

    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(managerGraphFixture()))
      .mockResolvedValueOnce(response(managerApplyFixture(request)))
      .mockResolvedValueOnce(response(managerDiffFixture()));
    vi.stubGlobal('fetch', fetchMock);

    await api.manager.graph();
    await api.manager.apply(request);
    await api.manager.diff();

    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/manager/graph');
    expect(fetchMock.mock.calls[1]?.[0]).toBe('/api/manager/apply');
    expect(fetchMock.mock.calls[2]?.[0]).toBe('/api/manager/diff');

    const [, applyOptions] = fetchMock.mock.calls[1] as [string, RequestInit];
    expect(applyOptions.method).toBe('POST');
    expect(JSON.parse(String(applyOptions.body))).toEqual(request);
  });

  it('submits one exact alarm activation without a caller-supplied principal', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({ acknowledged: true }));
    vi.stubGlobal('fetch', fetchMock);
    const alarm: OperatorAlarmRecord = {
      conditionId: 'condition/one',
      activationId: 'activation-2',
      recordRevision: 7,
      definitionId: 'manyforge.test.condition',
      lifecycleState: 'active_unacknowledged',
      active: true,
      acknowledged: false,
      priority: 'high',
      alarmClass: 'runtime_health',
      title: 'Test alarm',
      message: 'Attention required.',
      consequence: 'State is incomplete.',
      operatorResponse: 'Inspect the source.',
      occurrenceCount: 1,
      activatedAtNs: 100,
      requiresMotionAttention: false,
      subject: { subjectKind: 'cell', subjectId: 'cell-1', resourceIds: [] },
      details: {},
    };

    await api.alarms.acknowledge(
      alarm,
      'owner-epoch-3',
      'request-4',
    );

    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toContain('/api/execution/alarms/condition%2Fone/acknowledge');
    expect(options.method).toBe('POST');
    expect(JSON.parse(String(options.body))).toEqual({
      request_id: 'request-4',
      request_owner_epoch: 'owner-epoch-3',
      source_label: 'composer_ui',
      correlation_id: null,
      activation_id: 'activation-2',
      expected_record_revision: 7,
      reason: null,
      confirmed: true,
    });
    expect(String(options.body)).not.toContain('principal');
  });

  it('submits the exact common epoch, generation, and membership for bulk acknowledgement', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({ acknowledged: true }));
    vi.stubGlobal('fetch', fetchMock);

    await api.alarms.acknowledgeAll(
      'owner-epoch-5',
      9,
      [{ conditionId: 'condition-a', activationId: 'activation-a', recordRevision: 3 }],
      'request-6',
    );

    const [, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(JSON.parse(String(options.body))).toMatchObject({
      request_owner_epoch: 'owner-epoch-5',
      expected_generation: 9,
      members: [{
        condition_id: 'condition-a',
        activation_id: 'activation-a',
        record_revision: 3,
      }],
      confirmed: true,
    });
  });
});

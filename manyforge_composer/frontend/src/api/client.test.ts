import { afterEach, describe, expect, it, vi } from 'vitest';
import nativeBindingFixture from '../../../../manyforge_deployment/resources/program_binding_lite6_native.fixture.json';
import { ApiError, api, resolveStructuralAssetUrl } from './client';
import * as parserModule from './parsers';
import type { DraftFenceRequest, ManagerApplyRequest } from '../types/api';
import type { OperatorAlarmRecord } from '../types/headless';

afterEach(() => {
  vi.restoreAllMocks();
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

function draftFence(revision = 1): DraftFenceRequest {
  return {
    draftId: 'draft-1',
    expectedDraftRevision: revision,
    expectedAdmissionGeneration: 4,
    requestOwnerEpoch: 'composer-authoring-owner.fixture',
    requestId: 'composer-authoring.fixture',
  };
}

function workspaceFixture(): Record<string, unknown> {
  const revision = `sha256:${'a'.repeat(64)}`;
  const tree = {
    node_id: 'timer',
    instance_id: 'wait',
    parameters: { duration_s: 0.5 },
    children: [],
  };
  const programFields = {
    api_version: 'manyforge.program.v1',
    program_id: 'fixture',
    program_profile: 'composable-cell-v1',
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v2',
      content_version: '1.1.0',
    },
    component_roles: {},
    target_roles: {},
  };
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    draft: {
      draftId: 'draft-1',
      draftRevision: 1,
      programLoaded: true,
      authoring: {
        schema_version: 'manyforge.composer_authoring_forest.v1',
        ...programFields,
        forest: [tree],
        positions: {},
      },
      candidateAvailable: true,
      programRevision: revision,
      program: {
        ...programFields,
        program_revision: revision,
        tree,
      },
      diagnostics: [],
    },
    applicability: {
      applyReady: true,
      affectedInstanceIds: [],
      unavailableReason: null,
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
    schema_version: 'manyforge.program_apply_result.v2',
    status: 'applied',
    preparation: {},
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
    apply_generation: 1,
    apply_id: 'apply-1',
    candidate_projection_revision: {
      retained_bundle_id: 'sha256:' + 'b'.repeat(64),
      graph_revision: 'sha256:' + 'c'.repeat(64),
      scene_revision: {
        scene_id: 'sha256:' + 'd'.repeat(64),
        revision_id: 2,
        content_hash: 'sha256:' + 'e'.repeat(64),
      },
    },
    ok: true,
    error: null,
    replay_status: 'replayed_exact',
    request_id: request.request_id,
    request_owner_epoch: request.request_owner_epoch,
    state: 'succeeded',
  };
}

function managerCandidateReceipt() {
  return {
    schema_version: 'manyforge.manager_deployment_candidate_receipt.v1' as const,
    deployment_artifact_id: 'artifact-1',
    deployment_revision: 'rev-2',
    scenario_catalog_revision: `sha256:${'a'.repeat(64)}`,
    candidate_retained_bundle_id: 'bundle-next',
    candidate_graph_revision: 'graph-next',
    expected_current: {
      graph_revision: 'graph-9',
      retained_bundle_id: 'bundle-9',
    },
    expected_admission_generation: 7,
    manager_owner_epoch: 'owner.epoch.1',
    receipt_digest: `sha256:${'b'.repeat(64)}`,
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

  it('lets the backend select its configured default artifact root', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      currentPath: '/configured/artifacts',
      parentPath: null,
      roots: ['/configured/artifacts'],
      entries: [],
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.files.browse('deployment');

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/files/browse?kind=deployment',
      expect.objectContaining({ method: 'GET' }),
    );
    expect(result.currentPath).toBe('/configured/artifacts');
  });

  it('resolves a Program Draft mesh against one exact Draft revision', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      schema_version: 'manyforge.program_draft_mesh_asset.v1',
      asset_hash: '0123456789abcdef',
      mesh_asset_url: '/api/program/draft/scene-meshes/0123456789abcdef.glb?draft_id=draft-1&draft_revision=9',
      mesh_asset_status: 'pending',
      draft_id: 'draft-1',
      draft_revision: 9,
      last_error: null,
    }));
    vi.stubGlobal('fetch', fetchMock);

    const asset = await api.scene.resolveProgramDraftMesh(
      'examples/part.obj',
      { x: 1, y: 2, z: 3 },
      'draft-1',
      9,
    );

    expect(asset.draft_revision).toBe(9);
    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/program/draft/scene-meshes/resolve?resource_uri=examples%2Fpart.obj&scale_x=1&scale_y=2&scale_z=3&draft_id=draft-1&draft_revision=9',
    );
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

  it('never retains a malformed execution-target projection', async () => {
    const identity = { owner_epoch: 'malformed-target-owner' };
    const fetchMock = vi.fn().mockImplementation((input: string) => response(
      input.endsWith('/control-state')
        ? { identity }
        : { identity, targets: { malformed: true } },
    ));
    vi.stubGlobal('fetch', fetchMock);

    await expect(api.execution.controlStatus()).rejects.toThrow();
    await expect(api.execution.controlStatus()).rejects.toThrow();

    expect(fetchMock.mock.calls.map(([input]) => input)).toEqual([
      '/api/execution/control-state',
      '/api/execution/targets',
      '/api/execution/control-state',
      '/api/execution/targets',
    ]);
  });

  it('reuses validated targets only for the exact control identity', async () => {
    const ownerA = { owner_epoch: 'validated-target-owner-a' };
    const ownerB = { owner_epoch: 'validated-target-owner-b' };
    const parsed = { marker: 'strictly-validated' } as unknown as ReturnType<
      typeof parserModule.parseExecutionControlStatus
    >;
    const parserSpy = vi.spyOn(parserModule, 'parseExecutionControlStatus')
      .mockReturnValue(parsed);
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response({ identity: ownerA }))
      .mockResolvedValueOnce(response({ identity: ownerA, targets: {} }))
      .mockResolvedValueOnce(response({ identity: ownerA }))
      .mockResolvedValueOnce(response({ identity: ownerB }))
      .mockResolvedValueOnce(response({ identity: ownerB, targets: {} }));
    vi.stubGlobal('fetch', fetchMock);

    expect(await api.execution.controlStatus()).toBe(parsed);
    expect(await api.execution.controlStatus()).toBe(parsed);
    expect(await api.execution.controlStatus()).toBe(parsed);

    expect(parserSpy).toHaveBeenCalledTimes(3);
    expect(fetchMock.mock.calls.map(([input]) => input)).toEqual([
      '/api/execution/control-state',
      '/api/execution/targets',
      '/api/execution/control-state',
      '/api/execution/control-state',
      '/api/execution/targets',
    ]);
  });

  it('submits one exact revision-fenced Draft mutation', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      mutation: 'tree_draft_update_node_params',
      details: { updated_instance_id: 'wait' },
      workspace: workspaceFixture(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.programWorkspace.mutate(
      draftFence(),
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
      ...draftFence(),
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

    await api.programWorkspace.apply(draftFence());
    await api.programs.runs(true, 1);

    const [applyUrl, applyOptions] = fetchMock.mock.calls[0] as [
      string,
      RequestInit,
    ];
    expect(applyUrl).toBe('/api/program/apply');
    expect(JSON.parse(String(applyOptions.body))).toEqual({
      ...draftFence(),
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
    const currentFence = draftFence(7);

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

  it('loads a complete Scenario without a Program file path', async () => {
    const fetchMock = vi.fn().mockResolvedValueOnce(response({
      status: 'loaded',
      headless: {
        managerApply: null,
        programApply: null,
        scenario: {
          deployment_artifact_id: 'lite6_lite_gripper_pick_and_place',
          deployment_revision: `sha256:${'d'.repeat(64)}`,
          graph_revision: `sha256:${'e'.repeat(64)}`,
          program_binding_id: `sha256:${'f'.repeat(64)}`,
          program_id: 'lite6_lite_gripper_pick_and_place',
          program_revision: `sha256:${'a'.repeat(64)}`,
          retained_bundle_id: `sha256:${'b'.repeat(64)}`,
          scenario_catalog_revision: `sha256:${'c'.repeat(64)}`,
          scenario_id: 'lite6_pick_and_place',
        },
      },
      workspace: workspaceFixture(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    await api.programWorkspace.loadScenario(
      draftFence(),
      'lite6_pick_and_place',
      `sha256:${'c'.repeat(64)}`,
      true,
    );

    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/scenarios/load');
    expect(JSON.parse(String(options.body))).toEqual({
      ...draftFence(),
      scenarioId: 'lite6_pick_and_place',
      scenarioCatalogRevision: `sha256:${'c'.repeat(64)}`,
      confirmDiscard: true,
    });
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
      expected_admission_generation: 7,
      expected_draft_id: 'draft-1',
      expected_draft_revision: 4,
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
      expected_admission_generation: 7,
      candidate_receipt: managerCandidateReceipt(),
    };

    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(managerGraphFixture()))
      .mockResolvedValueOnce(response(managerApplyFixture(request)))
      .mockResolvedValueOnce(response(managerDiffFixture()));
    vi.stubGlobal('fetch', fetchMock);

    await api.manager.graph();
    const receipt = await api.manager.apply(request);
    await api.manager.diff();

    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/manager/graph');
    expect(fetchMock.mock.calls[1]?.[0]).toBe('/api/manager/apply');
    expect(fetchMock.mock.calls[2]?.[0]).toBe('/api/manager/diff');

    const [, applyOptions] = fetchMock.mock.calls[1] as [string, RequestInit];
    expect(applyOptions.method).toBe('POST');
    expect(JSON.parse(String(applyOptions.body))).toEqual(request);
    expect(receipt).toEqual({
      apiVersion: 'manyforge.manager_command_response.v1',
      applyId: 'apply-1',
      applyGeneration: 1,
      candidateProjectionRevision: {
        retained_bundle_id: 'sha256:' + 'b'.repeat(64),
        graph_revision: 'sha256:' + 'c'.repeat(64),
        scene_revision: {
          scene_id: 'sha256:' + 'd'.repeat(64),
          revision_id: 2,
          content_hash: 'sha256:' + 'e'.repeat(64),
        },
      },
      error: null,
      ok: true,
      replayStatus: 'replayed_exact',
      requestId: request.request_id,
      requestOwnerEpoch: request.request_owner_epoch,
      state: 'succeeded',
    });
  });

  it('rejects a Manager Apply receipt with a mismatched request identity', async () => {
    const request: ManagerApplyRequest = {
      request_id: 'request-manager-identity',
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
      expected_admission_generation: 7,
      candidate_receipt: managerCandidateReceipt(),
    };
    const responsePayload = managerApplyFixture({
      ...request,
      request_id: 'request-manager-other',
    });
    const fetchMock = vi.fn().mockResolvedValue(response(responsePayload));
    vi.stubGlobal('fetch', fetchMock);

    await expect(api.manager.apply(request)).rejects.toThrow(
      'Manager Apply response identity does not match the request',
    );
  });

  it('observes Manager Apply through the read-only durable identity route', async () => {
    const payload = managerApplyFixture({
      request_id: 'request-manager-1',
      request_owner_epoch: 'owner.epoch.1',
      source_label: 'composer_ui',
      correlation_id: null,
      deployment_artifact_id: 'artifact-1',
      deployment_revision: 'rev-2',
      expected_current: { graph_revision: 'graph-9', retained_bundle_id: 'bundle-9' },
      reject_if_affected_busy: true,
      expected_admission_generation: 7,
      candidate_receipt: managerCandidateReceipt(),
    });
    delete payload.replay_status;
    delete payload.request_id;
    delete payload.request_owner_epoch;
    const fetchMock = vi.fn().mockResolvedValue(response(payload));
    vi.stubGlobal('fetch', fetchMock);

    const observed = await api.manager.applyStatus('apply-1', 1);

    expect(observed.state).toBe('succeeded');
    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/manager/apply/status?apply_id=apply-1&apply_generation=1',
    );
    expect((fetchMock.mock.calls[0]?.[1] as RequestInit).method).toBe('GET');
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

  it('reads one exact typed historian detail by safe sequence', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      schema_version: 'manyforge.historian_detail.v1',
      sequence: 7,
      event: {
        schema_version: 'manyforge.historian_event.v1',
        event_id: 'event-1',
        component: 'manyforge.execution',
        event_type: 'operation_updated',
        severity: 'INFO',
        occurred_at_ns: 1,
        occurred_at_wall: '2026-08-28T00:00:00Z',
        payload: {},
      },
      sections: {
        incident: { available: false, reason: 'not an incident', value: null },
        execution_digest: { available: true, reason: null, value: { operation_id: 'op.1' } },
        blackboard: { available: false, reason: 'not published', value: null },
        latency: { available: false, reason: 'not published', value: null },
      },
    }));
    vi.stubGlobal('fetch', fetchMock);

    const detail = await api.history.detail(7);

    expect(detail.sequence).toBe(7);
    expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/execution/history/7');
    await expect(api.history.detail(0)).rejects.toThrow(
      'History sequence must be a positive safe integer',
    );
  });
});

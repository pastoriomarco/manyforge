import { afterEach, describe, expect, it, vi } from 'vitest';
import { ApiError, api, resolveStructuralAssetUrl } from './client';
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

describe('structural API client', () => {
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
  it('requests a bounded attention-newest alarm page', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({}));
    vi.stubGlobal('fetch', fetchMock);

    await expect(api.alarms.list()).rejects.toThrow('Invalid response');

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/execution/alarms?limit=32&ordering=attention_newest',
    );
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

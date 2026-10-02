import { afterEach, describe, expect, it, vi } from 'vitest';
import {
  ApiError,
  authoringRefusalMessage,
  failedAuthoringRequest,
  HISTORIAN_PAGE_LIMIT,
  api,
  historianListPath,
  resolveStructuralAssetUrl,
} from './client';

it('classifies only exact authoring pre-effect refusals', () => {
  for (const code of [
    'authoring_not_ready',
    'stale_generation',
    'observed_move_node_unsupported',
    'observed_move_role_missing',
    'observed_move_configuration_changed',
    'observed_move_target_unbound',
    'observed_move_target_unavailable',
    'observed_move_frame_mismatch',
    'observed_move_unavailable',
  ]) {
    expect(authoringRefusalMessage(new ApiError('refused', 409, { detail: { code } })))
      .toContain('Edit not applied');
  }
  // The retired catch-all code is no longer a recognised pre-effect refusal.
  expect(authoringRefusalMessage(new ApiError('refused', 409, { detail: { code: 'observed_move_target_mismatch' } })))
    .toBeNull();
  expect(authoringRefusalMessage(new ApiError('refused', 409, { detail: { code: 'activation_preparation_failed' } })))
    .toBe('Not applied: the candidate could not be prepared. Correct it and try again.');
  expect(authoringRefusalMessage(new ApiError('refused', 409, { detail: {
    code: 'activation_preparation_failed',
    details: { preparer_detail: 'manyforge_prepare_activation: Program is missing compatibility' },
  } }))).toBe('Not applied: the candidate could not be prepared (Program is missing compatibility). Correct it and try again.');
  expect(authoringRefusalMessage(new ApiError('fault', 409, { detail: { code: 'component_fault' } }))).toBeNull();
  expect(authoringRefusalMessage(new ApiError('failure', 500, { detail: { code: 'authoring_not_ready' } }))).toBeNull();
  expect(authoringRefusalMessage(new ApiError('failure', 503, { detail: { code: 'authoring_not_ready' } }))).toBeNull();
  expect(authoringRefusalMessage(new Error('authoring_not_ready'))).toBeNull();
});

it('classifies a named observed-move refusal by its code, whatever its status', () => {
  const message = 'current pose/joints could not be read: execution owner disconnected';
  for (const status of [502, 503]) {
    expect(authoringRefusalMessage(new ApiError(message, status, {
      detail: { code: 'observed_move_unavailable', message },
    }))).toBe('Edit not applied: current pose/joints could not be read: execution owner disconnected. '
      + 'Wait for the robot to report its current state, then try again.');
  }
});

it('never classifies a bare gateway or unavailable status as a refusal', () => {
  for (const status of [502, 503]) {
    // A proxy/gateway reply may hide a request that succeeded upstream.
    expect(authoringRefusalMessage(new ApiError('Bad Gateway', status, null))).toBeNull();
    expect(authoringRefusalMessage(new ApiError('Bad Gateway', status, '<html>bad gateway</html>'))).toBeNull();
    expect(authoringRefusalMessage(new ApiError('failure', status, { detail: 'upstream unavailable' }))).toBeNull();
    expect(authoringRefusalMessage(new ApiError('failure', status, { detail: { message: 'no code' } }))).toBeNull();
    expect(authoringRefusalMessage(new ApiError('failure', status, { detail: { code: 'headless_unavailable' } }))).toBeNull();
    expect(authoringRefusalMessage(new ApiError('failure', status, { code: 'observed_move_unavailable' }))).toBeNull();
  }
});

it('names each observed-move refusal cause with the owner message', () => {
  const refusal = (code: string, message: string) => authoringRefusalMessage(
    new ApiError(message, 409, { detail: { code, message } }),
  );
  expect(refusal(
    'observed_move_configuration_changed',
    'the Applied configuration changed: the Applied Program binding belongs to a different configuration than the current execution owner; Apply the Program again',
  )).toBe('Edit not applied: the Applied configuration changed: the Applied Program binding belongs to a different configuration than the current execution owner; Apply the Program again');
  expect(refusal('observed_move_role_missing', 'the selected move has no TCP frame role; choose it before capturing the current robot pose'))
    .toBe('Edit not applied: the selected move has no TCP frame role; choose it before capturing the current robot pose');
  expect(refusal('observed_move_frame_mismatch', "TCP frame role 'base' resolves to 'robot.base'"))
    .toBe("Edit not applied: TCP frame role 'base' resolves to 'robot.base'");
  expect(refusal('observed_move_target_unavailable', 'the bound robot target is unavailable in the current execution owner'))
    .toBe('Edit not applied: the bound robot target is unavailable in the current execution owner. Check that the robot is running in the current configuration.');
  expect(refusal('observed_move_unavailable', 'execution owner has no complete current observed target.'))
    .toBe('Edit not applied: execution owner has no complete current observed target. Wait for the robot to report its current state, then try again.');
});
import type {
  DraftFenceRequest,
  LocalWorkspaceFenceRequest,
} from '../types/api';
import type { OperatorAlarmRecord } from '../types/headless';
import type { RuntimeScaleControlSelector } from '../types/robotControl';

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

it('reads the shared authoring workspace through its single endpoint', async () => {
  const fetchMock = vi.fn().mockResolvedValue(response({}));
  vi.stubGlobal('fetch', fetchMock);

  await expect(api.authoringWorkspace.get()).rejects.toThrow(
    /authoringWorkspace/,
  );
  expect(fetchMock).toHaveBeenCalledTimes(1);
  expect(fetchMock.mock.calls[0]?.[0]).toBe('/api/authoring/workspace');
});

it('retains the exact sent authoring envelope after a lost reply without retrying', async () => {
  const lost = new Error('reply lost');
  const fetcher = vi.fn().mockRejectedValue(lost);
  vi.stubGlobal('fetch', fetcher);
  const fence = localFence();
  await expect(api.programWorkspace.discard(fence)).rejects.toBe(lost);
  expect(failedAuthoringRequest(lost)).toEqual({
    requestOwnerEpoch: fence.requestOwnerEpoch, requestId: fence.requestId,
    expectedAdmissionGeneration: fence.expectedAdmissionGeneration,
    path: '/api/program/discard',
  });
  expect(fetcher).toHaveBeenCalledTimes(1);
});

it('reads only exact submission evidence and rejects mismatched or malformed records', async () => {
  const identity = { requestOwnerEpoch: 'browser.1', requestId: 'request.1', expectedAdmissionGeneration: 7 };
  const record = {
    schema_version: 'manyforge.authoring_submission_outcome.v1',
    request_owner_epoch: 'browser.1', request_id: 'request.1', expected_admission_generation: 7,
    owner_epoch: 'composer.1', outcome: { kind: 'settled', operation_kind: 'save', apply: null },
  };
  const fetcher = vi.fn().mockResolvedValue(response(record));
  vi.stubGlobal('fetch', fetcher);
  await expect(api.authoringWorkspace.submissionOutcome(identity)).resolves.toEqual({ kind: 'settled', apply: null });
  expect(fetcher.mock.calls[0][1].method).toBe('GET');
  for (const patch of [{ request_id: 'other' }, { expected_admission_generation: 8 },
    { outcome: { kind: 'settled', operation_kind: 'save', apply: {} } },
    { outcome: { kind: 'manager_accepted', operation_kind: 'apply', apply: null } },
    { unexpected: true }]) {
    fetcher.mockResolvedValue(response({ ...record, ...patch }));
    await expect(api.authoringWorkspace.submissionOutcome(identity)).rejects.toThrow();
  }
  fetcher.mockResolvedValue(response({ ...record, outcome: null }));
  await expect(api.authoringWorkspace.submissionOutcome(identity)).resolves.toBeNull();
});

it('downloads filtered diagnostics directly without fetching or parsing into UI state', () => {
  const fetcher = vi.fn();
  vi.stubGlobal('fetch', fetcher);
  const query = { programRunId: 'run.one / two', channel: 'manyforge.composer', recentSeconds: 60 };
  expect(api.history.exportUrl(query)).toContain('/api/debug/export?');
  expect(api.history.exportUrl(query, true)).toContain('/api/debug/export/ros2-medkit?');
  expect(api.history.exportUrl(query)).toContain('channel=manyforge.composer');
  expect(api.history.exportUrl(query)).toContain('program_run_id=run.one+%2F+two');
  expect(fetcher).not.toHaveBeenCalled();
});

it('rejects a discovery page for a different requested index', async () => {
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response({
    schema_version: 'manyforge.historian_discovery.v1', kind: 'channel',
    values: ['manyforge.composer'], truncated: false, next_after: null,
  })));
  await expect(api.history.discovery('program_run_id')).rejects.toThrow('different kind');
});

describe('runtime-scale control reads', () => {
  it('serializes singular and group selectors with their exact query shape', async () => {
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        kind: 'singular', programBindingId: 'binding/1', componentRoleId: 'robot', targetRoleId: 'arm',
      })))
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        kind: 'group', programBindingId: 'binding/1', groupTargets: [
          { componentRoleId: 'robot', targetRoleId: 'arm' },
          { componentRoleId: 'robot', targetRoleId: 'gripper' },
        ],
      })));
    vi.stubGlobal('fetch', fetchMock);

    await api.execution.runtimeScaleControl({
      kind: 'singular', programBindingId: 'binding/1', componentRoleId: 'robot', targetRoleId: 'arm',
    });
    await api.execution.runtimeScaleControl({
      kind: 'group', programBindingId: 'binding/1', groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    });

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/execution/runtime-scale-control?program_binding_id=binding%2F1&component_role_id=robot&target_role_id=arm',
    );
    expect(fetchMock.mock.calls[1]?.[0]).toContain(
      'program_binding_id=binding%2F1&group_targets=%5B%7B%22component_role_id%22%3A%22robot%22%2C%22target_role_id%22%3A%22arm%22%7D%2C%7B%22component_role_id%22%3A%22robot%22%2C%22target_role_id%22%3A%22gripper%22%7D%5D',
    );
  });

  it('rejects malformed and crossed owner responses', async () => {
    const selector = {
      kind: 'singular' as const, programBindingId: 'binding-1', componentRoleId: 'robot', targetRoleId: 'arm',
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response({}))
      .mockResolvedValueOnce(response(runtimeScaleResponse({ ...selector, targetRoleId: 'gripper' })));
    vi.stubGlobal('fetch', fetchMock);
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('Invalid response');
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('does not match');
  });

  it('rejects crossed group members and bindings', async () => {
    const selector = {
      kind: 'group' as const, programBindingId: 'binding-1', groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        ...selector,
        groupTargets: [
          selector.groupTargets[0],
          { componentRoleId: 'robot', targetRoleId: 'camera' },
        ],
      })))
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        ...selector, programBindingId: 'binding-2',
      })));
    vi.stubGlobal('fetch', fetchMock);
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('does not match');
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('does not match');
  });

  it('validates a deferred response against a selector snapshot', async () => {
    const selector = {
      kind: 'group' as const, programBindingId: 'binding-1', groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    };
    let resolveResponse: ((value: Response) => void) | undefined;
    vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>((resolve) => {
      resolveResponse = resolve;
    })));
    const pending = api.execution.runtimeScaleControl(selector);
    selector.programBindingId = 'binding-crossed';
    selector.groupTargets[0].targetRoleId = 'camera';
    resolveResponse?.(response(runtimeScaleResponse({
      kind: 'group', programBindingId: 'binding-1', groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    })));
    await expect(pending).resolves.toMatchObject({ selector: { kind: 'group' } });
  });

  it('accepts a group response whose target order differs', async () => {
    const selector = {
      kind: 'group' as const, programBindingId: 'binding-1', groupTargets: [
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'gripper' },
      ],
    };
    const returned = { ...selector, groupTargets: [...selector.groupTargets].reverse() };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(runtimeScaleResponse(returned))));
    await expect(api.execution.runtimeScaleControl(selector)).resolves.toMatchObject({ selector: { kind: 'group' } });
  });
});

it('composes an identity-fenced stop-fault recovery from the live offer', async () => {
  const fetchMock = vi.fn()
    .mockResolvedValueOnce(response({
      api_version: 'manyforge.execution_command_response.v1',
      command: 'get_stop_fault',
      ok: true,
      stop_fault: {
        configuration_expectations: [{
          active_control_configuration_generation: 4,
          active_control_configuration_id: 'active.arm',
          control_domain_id: 'robot.arm.control',
        }],
        latch: {
          cell_id: 'cell.1',
          graph_revision: 'sha256:graph',
          latch_generation: 2,
          latch_id: 'stop-fault.1',
          recovery_assessment_revision: 3,
          recovery_status: 'ready',
          retained_bundle_id: 'sha256:bundle',
          state: 'latched',
          stop_certificate: {
            stop_certificate_id: 'stop-certificate.1',
            stop_certificate_revision: 1,
            stop_generation: 4,
            stop_id: 'stop.1',
          },
        },
      },
    }))
    .mockResolvedValueOnce(response({
      api_version: 'manyforge.execution_command_response.v1',
      command: 'recover_stop_fault',
      ok: true,
      result: {
        accepted: true,
        error: null,
        latch: {},
        replay_status: 'accepted_new',
        request_id: 'request.recover.1',
      },
    }));
  vi.stubGlobal('fetch', fetchMock);

  await api.execution.recoverStopFault('owner.1', 'request.recover.1');

  const [, options] = fetchMock.mock.calls[1] as [string, RequestInit];
  expect(JSON.parse(String(options.body))).toMatchObject({
    request_context: {
      request_id: 'request.recover.1',
      request_owner_epoch: 'owner.1',
      source_label: 'composer_ui',
      correlation_id: null,
    },
    expected_latch_id: 'stop-fault.1',
    expected_recovery_assessment_revision: 3,
    expected_stop_certificate_id: 'stop-certificate.1',
    configuration_expectations: [{
      control_domain_id: 'robot.arm.control',
      active_control_configuration_id: 'active.arm',
      active_control_configuration_generation: 4,
    }],
  });
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

function runtimeScaleResponse(selector: RuntimeScaleControlSelector): Record<string, unknown> {
  const member = (target: { componentRoleId: string; targetRoleId: string }) => ({
    active_control_configuration_generation: 1,
    active_control_configuration_id: `active-${target.targetRoleId}`,
    control_binding_id: `binding-${target.targetRoleId}`,
    control_binding_revision: 'revision-1',
    control_domain_id: `domain-${target.targetRoleId}`,
    control_domain_revision: 'revision-1',
    scaling_domain_id: `scale-${target.targetRoleId}`,
    component_role_id: target.componentRoleId,
    target_role_id: target.targetRoleId,
  });
  const status = {
    active_control_configuration_generation: selector.kind === 'group' ? null : 1,
    active_control_configuration_id: selector.kind === 'group' ? null : 'active-arm',
    active_operation_id: null,
    active_operation_revision: null,
    active_operation_scale_generation: null,
    application_state: null,
    control_binding_id: selector.kind === 'group' ? null : 'binding-arm',
    control_binding_revision: selector.kind === 'group' ? null : 'binding-revision-1',
    control_domain_id: selector.kind === 'group' ? null : 'domain-arm',
    control_domain_revision: selector.kind === 'group' ? null : 'domain-revision-1',
    control_generation: 1,
    effective_scale: null,
    frozen: false,
    reason: null,
    requested_scale: 0.5,
    scaling_domain_id: selector.kind === 'group' ? null : 'scale-arm',
    selector: selector.kind === 'singular' ? {
      program_binding_id: selector.programBindingId,
      component_role_id: selector.componentRoleId,
      target_role_id: selector.targetRoleId,
    } : {
      program_binding_id: selector.programBindingId,
      group_targets: selector.groupTargets.map((target) => ({
        component_role_id: target.componentRoleId,
        target_role_id: target.targetRoleId,
      })),
    },
    supported: true,
    ...(selector.kind === 'group'
      ? { group_members: selector.groupTargets.map(member) }
      : {}),
  };
  return {
    api_version: 'manyforge.execution_command_response.v1',
    command: 'get_runtime_scale_control',
    ok: true,
    runtime_scale_control: status,
  };
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

/**
 * The migrated ordinary routes also require the composite workspace fence;
 * Apply, Revert and Deployment candidate requests keep the plain fence.
 */
function localFence(revision = 1): LocalWorkspaceFenceRequest {
  return {
    ...draftFence(revision),
    expectedWorkspaceEpoch: 'workspace.epoch.1',
    expectedWorkspaceRevision: 3,
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
    api_version: 'manyforge.program.v2',
    program_id: 'fixture',
    program_profile: 'composable-cell-v1',
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v2',
      content_version: '2.4.0',
    },
    component_roles: {},
    target_roles: {},
  };
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: {
      workspaceEpoch: 'workspace.epoch.1', workspaceRevision: 7,
      programDraftId: 'draft-1', programDraftRevision: 1,
      deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 1,
    },
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
      formulaReferences: { computedKeys: {}, nodes: {} },
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

function authoringWorkspaceFixture(): Record<string, unknown> {
  const workspace = workspaceFixture();
  const draft = workspace.draft as Record<string, unknown>;
  const saved = workspace.saved as Record<string, unknown>;
  return {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v1',
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: 3,
    program: { ...draft, saved },
    deployment: {
      draftId: 'deployment-draft-1',
      draftRevision: 3,
      document: null,
      savedPath: null,
      savedDocument: null,
      differsFromSaved: false,
    },
    project: {
      document_path: '/workspace/examples/cell',
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

function appliedResultFixture(): Record<string, unknown> {
  const digestA = `sha256:${'a'.repeat(64)}`;
  const digestB = `sha256:${'b'.repeat(64)}`;
  const digestC = `sha256:${'c'.repeat(64)}`;
  return {
    status: 'applied',
    confirmation_token: null,
    prepared_activation: {
      schema_version: 'manyforge.prepared_activation_input.v3',
      prepared_activation_id: digestC,
      scenario_id: 'fixture',
      source_document_digest: digestA,
      captured_input_digest: digestB,
      deployment_artifact_id: 'cell-a',
      deployment_revision: digestB,
      retained_bundle_id: digestB,
      graph_revision: digestC,
      program_id: 'fixture',
      program_revision: digestA,
      program_outcome: { kind: 'bound', program_binding_id: digestC },
      node_environment_digest: digestA,
      assistant_mode: 'forbidden',
      typed_parameter_overrides: {},
    },
    candidate_preview: null,
    manager_apply: null,
    preparation: null,
    phases_ms: {},
    apply_id: null,
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

  it('submits one exact revision-fenced Draft mutation', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      mutation: 'tree_draft_update_node_params',
      details: { updated_instance_id: 'wait' },
      workspace: workspaceFixture(),
      authoringWorkspace: authoringWorkspaceFixture(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.programWorkspace.mutate(
      localFence(),
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
      ...localFence(),
      mutation: 'tree_draft_update_node_params',
      arguments: {
        instance_id: 'wait',
        expected_node_id: 'timer',
        parameters: { duration_s: 1 },
      },
    });
    expect(result.workspace.saved.matchesDraft).toBe(true);
    expect(result.authoringWorkspace.workspaceRevision).toBe(3);
  });

  it('forwards Apply without client-owned state', async () => {
    const fetchMock = vi.fn().mockResolvedValueOnce(response({
      status: 'applied',
      headless: appliedResultFixture(),
      workspace: workspaceFixture(),
    }));
    vi.stubGlobal('fetch', fetchMock);

    await api.programWorkspace.apply(draftFence());

    const [applyUrl, applyOptions] = fetchMock.mock.calls[0] as [
      string,
      RequestInit,
    ];
    expect(applyUrl).toBe('/api/program/apply');
    expect(JSON.parse(String(applyOptions.body))).toEqual({
      ...draftFence(),
    });
  });

  it('references the retained Deployment preparation when confirming Apply', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({}));
    vi.stubGlobal('fetch', fetchMock);
    const fence = draftFence();

    await api.deploymentWorkspace.applyDraft(fence, {
      token: `sha256:${'c'.repeat(64)}`,
      preparation: {
        requestOwnerEpoch: 'browser-owner-1',
        requestId: 'prepare-1',
      },
    }).catch(() => null);

    const [url, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(url).toBe('/api/deployment/draft/apply');
    expect(JSON.parse(String(options.body))).toEqual({
      ...fence,
      confirmationToken: `sha256:${'c'.repeat(64)}`,
      preparationRequestOwnerEpoch: 'browser-owner-1',
      preparationRequestId: 'prepare-1',
    });
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
    const currentFence = localFence(7);
    const revertFence = draftFence(7);

    await api.programWorkspace.newDraft(currentFence, true);
    await api.programWorkspace.discard(currentFence);
    await api.programWorkspace.revert(revertFence);

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
    ))).toEqual(revertFence);
  });

  it('lists and saves Scenario documents, then loads through the fenced activation path', async () => {
    const index = {
      api_version: 'manyforge.scenario_document_index.v1',
      directory: '/workspace/examples',
      entries: [],
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(index))
      .mockResolvedValueOnce(response({
        status: 'saved',
        path: '/workspace/examples/cell',
        scenario_id: 'cell',
        last_known_status: 'unknown',
        workspace: workspaceFixture(),
      }))
      .mockResolvedValueOnce(response({
        status: 'accepted',
        path: '/workspace/examples/cell',
        scenario_id: 'cell',
        incomplete: false,
        activation: {
          status: 'accepted', apply_id: 'apply.1', apply_generation: 1,
        },
        diagnostics: [],
        source_diagnostics: [],
        workspace: workspaceFixture(),
        authoringWorkspace: authoringWorkspaceFixture(),
      }))
      .mockResolvedValueOnce(response({
        status: 'exported',
        path: '/workspace/exports/cell',
      }));
    vi.stubGlobal('fetch', fetchMock);
    const scenarioFence = {
      ...localFence(),
      deploymentDraftId: 'deployment-draft-1',
      expectedDeploymentDraftRevision: 3,
    };

    await api.scenarioDocuments.list('/workspace/examples');
    await api.scenarioDocuments.save(scenarioFence, {
      path: '/workspace/examples/cell',
      scenarioId: 'cell',
      name: 'Cell',
      description: '',
      confirmOverwrite: true,
    });
    await api.scenarioDocuments.load(
      scenarioFence,
      '/workspace/examples/cell',
      true,
    );
    await api.scenarioDocuments.export(
      scenarioFence,
      '/workspace/exports/cell',
    );

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/scenario-documents?directory=%2Fworkspace%2Fexamples',
    );
    expect(JSON.parse(String(
      (fetchMock.mock.calls[1]?.[1] as RequestInit).body,
    ))).toEqual({
      ...scenarioFence,
      path: '/workspace/examples/cell',
      scenarioId: 'cell',
      name: 'Cell',
      description: '',
      confirmOverwrite: true,
    });
    expect(JSON.parse(String(
      (fetchMock.mock.calls[2]?.[1] as RequestInit).body,
    ))).toEqual({
      ...scenarioFence,
      path: '/workspace/examples/cell',
      confirmDiscard: true,
    });
    expect(fetchMock.mock.calls[3]?.[0]).toBe(
      '/api/scenario-documents/export',
    );
    expect(JSON.parse(String(
      (fetchMock.mock.calls[3]?.[1] as RequestInit).body,
    ))).toEqual({
      ...scenarioFence,
      destinationPath: '/workspace/exports/cell',
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

  it('observes one exact Manager Apply without replaying the effect', async () => {
    const payload = {
      api_version: 'manyforge.manager_command_response.v1',
      apply_generation: 3,
      apply_id: 'apply-3',
      candidate_projection_revision: null,
      error: null,
      failure: null,
      last_progress: {
        phase: 'fence_accepted',
        record_digest: `sha256:${'1'.repeat(64)}`,
      },
      ok: true,
      phase: 'fence_accepted',
      prepared_activation: {
        captured_input_digest: `sha256:${'2'.repeat(64)}`,
        deployment_artifact_id: 'deployment-1',
        deployment_revision: `sha256:${'3'.repeat(64)}`,
        program_binding_id: `sha256:${'4'.repeat(64)}`,
        prepared_activation_id: `sha256:${'5'.repeat(64)}`,
      },
      request_id: 'request-3',
      request_owner_epoch: 'owner-3',
      state: 'pending',
    };
    const fetchMock = vi.fn().mockResolvedValue(response(payload));
    vi.stubGlobal('fetch', fetchMock);

    const observed = await api.manager.applyStatus('apply-3', 3);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/manager/apply/status?apply_id=apply-3&apply_generation=3',
    );
    expect(observed.phase).toBe('fence_accepted');
  });

  it('rejects an Apply status for a different durable identity', async () => {
    const payload = {
      api_version: 'manyforge.manager_command_response.v1',
      apply_generation: 4,
      apply_id: 'different-apply',
      candidate_projection_revision: null,
      error: null,
      failure: null,
      last_progress: {
        phase: 'accepted',
        record_digest: `sha256:${'1'.repeat(64)}`,
      },
      ok: true,
      phase: 'accepted',
      prepared_activation: {
        captured_input_digest: `sha256:${'2'.repeat(64)}`,
        deployment_artifact_id: 'deployment-1',
        deployment_revision: `sha256:${'3'.repeat(64)}`,
        program_binding_id: `sha256:${'4'.repeat(64)}`,
        prepared_activation_id: `sha256:${'5'.repeat(64)}`,
      },
      request_id: 'request-4',
      request_owner_epoch: 'owner-4',
      state: 'pending',
    };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(payload)));

    await expect(api.manager.applyStatus('apply-4', 4)).rejects.toThrow(
      'Manager Apply status identity does not match the request',
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

  it('reads one exact typed historian detail by safe sequence', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      schema_version: 'manyforge.historian_detail.v2',
      sequence: 7,
      event: {
        schema_version: 'manyforge.historian_event.v2',
        event_id: 'event-1',
        component: 'manyforge.execution',
        event_type: 'operation_updated',
        severity: 'INFO',
        occurred_at_ns: '1',
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

  it('carries bounded historian time, component, type, and page filters', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      schema_version: 'manyforge.historian_summary_page.v2',
      time_window: { start_utc_ns: '10', end_utc_ns: '20' },
      retention: null,
      events: [],
      next_before_sequence: null,
      truncated: false,
    }));
    vi.stubGlobal('fetch', fetchMock);

    await api.history.list({
      startUtcNs: '10',
      endUtcNs: '20',
      components: ['manyforge.runtime'],
      eventTypes: ['operation_completed'],
      beforeSequence: 7,
      limit: HISTORIAN_PAGE_LIMIT,
    });

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/execution/history?limit=32&start_utc_ns=10&end_utc_ns=20&before_sequence=7&components=manyforge.runtime&event_types=operation_completed',
    );
    expect(() => historianListPath({ limit: HISTORIAN_PAGE_LIMIT + 1 })).toThrow(
      'History page limit must be an integer from 1 to 32',
    );
    expect(() => historianListPath({ startNs: '01' })).toThrow(
      'History start must be a canonical nonnegative int64 decimal string',
    );
  });
});

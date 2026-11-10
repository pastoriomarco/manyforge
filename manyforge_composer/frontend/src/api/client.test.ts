import { afterEach, describe, expect, it, vi } from 'vitest';
import { buildJointMotionIntent } from '../state/robotControl';
import { statusFixture } from '../testFixtures/robotControl';
import {
  ApiError,
  authoringRefusalMessage,
  failedAuthoringRequest,
  refusedBeforeEffect,
  HISTORIAN_PAGE_LIMIT,
  api,
  historianListPath,
  preparationCauseCode,
  resolveStructuralAssetUrl,
} from './client';

it('classifies a refusal only by the backend before-effect marker', () => {
  const refused = (code: string, status = 409, extra: Record<string, unknown> = {}) => new ApiError(
    'refused', status, { detail: { code, message: 'refused', effect: 'none', ...extra } },
  );
  for (const code of [
    'authoring_not_ready',
    'stale_generation',
    'observed_move_node_unsupported',
    'observed_move_unavailable',
    'observed_scene_object_ineligible',
  ]) {
    expect(refusedBeforeEffect(refused(code))).toBe(true);
    expect(authoringRefusalMessage(refused(code))).toContain('Edit not applied');
  }
  // The marker, not the status or an allowlisted code, proves it.
  expect(authoringRefusalMessage(refused('authoring_not_ready', 503)))
    .toBe('Edit not applied: authoring is currently unavailable.');
  expect(authoringRefusalMessage(refused('scenario_destination_exists')))
    .toBe('Not applied: refused. Nothing was changed.');
  expect(authoringRefusalMessage(refused('activation_preparation_failed')))
    .toBe('Not applied: the candidate could not be prepared. Correct it and try again.');
  expect(authoringRefusalMessage(refused('activation_preparation_failed', 409, {
    details: { preparer_detail: 'manyforge_prepare_activation: Program is missing compatibility' },
  }))).toBe('Not applied: the candidate could not be prepared (Program is missing compatibility). Correct it and try again.');
  // Without the marker nothing is a definite refusal, whatever its code.
  for (const error of [
    new ApiError('refused', 409, { detail: { code: 'authoring_not_ready' } }),
    new ApiError('refused', 409, { detail: { code: 'scenario_destination_exists' } }),
    new ApiError('refused', 409, { detail: { code: 'observed_move_unavailable', effect: 'unknown' } }),
    new ApiError('fault', 409, { detail: { code: 'component_fault' } }),
    new ApiError('failure', 500, { detail: { code: 'authoring_not_ready' } }),
    new ApiError('failure', 409, { effect: 'none' }),
    new Error('authoring_not_ready'),
  ]) {
    expect(refusedBeforeEffect(error)).toBe(false);
    expect(authoringRefusalMessage(error)).toBeNull();
  }
});

it('reads the typed cause of a refused preparation, never its text', () => {
  const refused = (code: string, details: Record<string, unknown>) => new ApiError(
    'refused', 409, { detail: { code, message: 'refused', effect: 'none', details } },
  );
  expect(preparationCauseCode(refused('activation_preparation_failed', {
    preparer_detail: 'manyforge_prepare_activation: pneumatic_io_presets_coincident: robot',
    cause_code: 'pneumatic_io_presets_coincident',
  }))).toBe('pneumatic_io_presets_coincident');
  // The reason only in the text is not a cause.
  expect(preparationCauseCode(refused('activation_preparation_failed', {
    preparer_detail: 'manyforge_prepare_activation: pneumatic_io_unsupported_on_isaac: Isaac',
  }))).toBeNull();
  expect(preparationCauseCode(refused('stale_generation', {
    cause_code: 'pneumatic_io_presets_coincident',
  }))).toBeNull();
  expect(preparationCauseCode(new Error('pneumatic_io_presets_coincident'))).toBeNull();
});

it('uses the error catalog detail for a typed preparation cause', () => {
  const refusal = new ApiError('candidate refused', 409, { detail: {
    code: 'activation_preparation_failed',
    message: 'candidate refused',
    effect: 'none',
    details: { cause_code: 'pneumatic_io_presets_coincident' },
  } });
  expect(authoringRefusalMessage(refusal)).toBe(
    'Not applied: the candidate could not be prepared. The gripper\'s Open and Close presets resolve to the same position, '
      + 'so its pneumatic I/O cannot tell them apart. Choose distinct preset positions.',
  );
});

it('keeps an unlisted typed preparation cause visible when no catalog row exists', () => {
  const causeCode = 'unlisted_preparer_reason';
  const refusal = new ApiError('candidate refused', 409, { detail: {
    code: 'activation_preparation_failed',
    message: 'candidate refused',
    effect: 'none',
    details: { cause_code: causeCode },
  } });
  expect(authoringRefusalMessage(refusal)).toBe(
    `Not applied: the candidate could not be prepared (${causeCode}). Correct it and try again.`,
  );
});

it('accepts a joint-flange fallback only for a joint-goal motion preview', async () => {
  const intent = buildJointMotionIntent(statusFixture(), [0.2, 0.1], 'preview-1');
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(motionPreviewResponse(intent))));
  await expect(api.execution.previewMotion(intent)).resolves.toMatchObject({
    resolvedTcp: { source: 'joint_flange_fallback', entry: { frame: { frameId: 'flange' } } },
  });

  const poseIntent = structuredClone(intent);
  const payload = poseIntent.participants[0].payload;
  if (payload.kind !== 'motion') throw new Error('expected motion payload');
  payload.motion = {
    kind: 'pose_goal',
    goal: {
      position_m: { x_m: 0.1, y_m: 0.2, z_m: 0.3 },
      orientation: { x: 0, y: 0, z: 0, w: 1 },
    },
  };
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(motionPreviewResponse(poseIntent))));
  await expect(api.execution.previewMotion(poseIntent)).rejects.toThrow(
    /joint flange fallback is only valid for a joint-goal preview/,
  );
});

it('exposes a refusal ErrorDTO as one structured error with no origin', () => {
  const error = new ApiError('refused', 409, { detail: {
    code: 'stale_generation',
    message: 'admission moved',
    details: { expected: 3, current: 4 },
  } });
  expect(error.structuredError).toEqual({
    code: 'stale_generation',
    message: 'admission moved',
    details: { expected: 3, current: 4 },
    origin: null,
  });
  expect(new ApiError('Bad Gateway', 502, null).structuredError).toBeNull();
  expect(new ApiError('failure', 503, { detail: 'upstream unavailable' }).structuredError).toBeNull();
});

it('classifies a marked observed-move refusal whatever its status', () => {
  const message = 'current pose/joints could not be read: execution owner disconnected';
  for (const status of [502, 503]) {
    expect(authoringRefusalMessage(new ApiError(message, status, {
      detail: { code: 'observed_move_unavailable', message, effect: 'none' },
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
    new ApiError(message, 409, { detail: { code, message, effect: 'none' } }),
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
  // The live Copy current position to Draft refusal.
  expect(refusal('observed_scene_object_ineligible', 'only one current cell-fixed object with one portable shape can be saved'))
    .toBe('Edit not applied: only one current cell-fixed object with one portable shape can be saved');
});
import type {
  DraftFenceRequest,
  LocalWorkspaceFenceRequest,
} from '../types/api';
import type { OperatorAlarmRecord } from '../types/headless';
import type {
  DirectControlOperationIntent,
  RuntimeScaleControlSelector,
} from '../types/robotControl';

afterEach(() => {
  vi.restoreAllMocks();
  vi.unstubAllGlobals();
});

it('reports a body read cut by the caller abort as that abort, not invalid JSON', async () => {
  const controller = new AbortController();
  const reason = new Error('Activation sequence cancelled; accepted work remains observable.');
  const cut = { ok: true, status: 200, statusText: 'OK', json: vi.fn().mockImplementation(async () => {
    controller.abort(reason);
    throw new Error('The operation was aborted.');
  }) } as unknown as Response;
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(cut));
  await expect(api.components.list(controller.signal)).rejects.toBe(reason);
  // A success reply whose body really is not JSON is still reported as such.
  const malformed = { ok: true, status: 200, statusText: 'OK',
    json: vi.fn().mockRejectedValue(new SyntaxError('Unexpected token <')) } as unknown as Response;
  vi.stubGlobal('fetch', vi.fn().mockResolvedValue(malformed));
  await expect(api.components.list(new AbortController().signal))
    .rejects.toThrow('Invalid JSON response from /api/components');
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
        kind: 'singular', programBindingId: 'binding/1', componentId: 'robot', commandTargetId: 'arm',
      })))
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        kind: 'group', programBindingId: 'binding/1', groupTargets: [
          { componentId: 'robot', commandTargetId: 'arm' },
          { componentId: 'robot', commandTargetId: 'gripper' },
        ],
      })));
    vi.stubGlobal('fetch', fetchMock);

    await api.execution.runtimeScaleControl({
      kind: 'singular', programBindingId: 'binding/1', componentId: 'robot', commandTargetId: 'arm',
    });
    await api.execution.runtimeScaleControl({
      kind: 'group', programBindingId: 'binding/1', groupTargets: [
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'gripper' },
      ],
    });

    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/execution/runtime-scale-control?program_binding_id=binding%2F1&component_id=robot&command_target_id=arm',
    );
    expect(fetchMock.mock.calls[1]?.[0]).toContain(
      'program_binding_id=binding%2F1&group_targets=%5B%7B%22component_id%22%3A%22robot%22%2C%22command_target_id%22%3A%22arm%22%7D%2C%7B%22component_id%22%3A%22robot%22%2C%22command_target_id%22%3A%22gripper%22%7D%5D',
    );
  });

  it('rejects malformed and crossed owner responses', async () => {
    const selector = {
      kind: 'singular' as const, programBindingId: 'binding-1', componentId: 'robot', commandTargetId: 'arm',
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response({}))
      .mockResolvedValueOnce(response(runtimeScaleResponse({ ...selector, commandTargetId: 'gripper' })));
    vi.stubGlobal('fetch', fetchMock);
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('Invalid response');
    await expect(api.execution.runtimeScaleControl(selector)).rejects.toThrow('does not match');
  });

  it('rejects crossed group members and bindings', async () => {
    const selector = {
      kind: 'group' as const, programBindingId: 'binding-1', groupTargets: [
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'gripper' },
      ],
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(runtimeScaleResponse({
        ...selector,
        groupTargets: [
          selector.groupTargets[0],
          { componentId: 'robot', commandTargetId: 'camera' },
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
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'gripper' },
      ],
    };
    let resolveResponse: ((value: Response) => void) | undefined;
    vi.stubGlobal('fetch', vi.fn(() => new Promise<Response>((resolve) => {
      resolveResponse = resolve;
    })));
    const pending = api.execution.runtimeScaleControl(selector);
    selector.programBindingId = 'binding-crossed';
    selector.groupTargets[0].commandTargetId = 'camera';
    resolveResponse?.(response(runtimeScaleResponse({
      kind: 'group', programBindingId: 'binding-1', groupTargets: [
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'gripper' },
      ],
    })));
    await expect(pending).resolves.toMatchObject({ selector: { kind: 'group' } });
  });

  it('accepts a group response whose target order differs', async () => {
    const selector = {
      kind: 'group' as const, programBindingId: 'binding-1', groupTargets: [
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'gripper' },
      ],
    };
    const returned = { ...selector, groupTargets: [...selector.groupTargets].reverse() };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(runtimeScaleResponse(returned))));
    await expect(api.execution.runtimeScaleControl(selector)).resolves.toMatchObject({ selector: { kind: 'group' } });
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

function motionPreviewResponse(intent: DirectControlOperationIntent): Record<string, unknown> {
  const participant = intent.participants[0];
  if (participant.payload.kind !== 'motion') throw new Error('expected motion payload');
  const joints = ['joint1', 'joint2'];
  return {
    ok: true,
    command: 'preview_motion',
    preview_receipt: {
      schema_version: 'manyforge.direct_control_motion_preview.v4',
      receipt_digest: 'sha256:receipt',
      intent_digest: 'sha256:intent',
      request_id: intent.request_context.request_id,
      request_owner_epoch: intent.request_context.request_owner_epoch,
      program_id: intent.program_id,
      program_revision: intent.program_revision,
      program_binding_id: intent.program_binding_id,
      program_run_id: intent.program_run_id,
      graph_revision: intent.expected_graph_revision,
      retained_bundle_id: intent.expected_retained_bundle_id,
      scene_revision: { scene_id: 'scene.1', revision_id: 1, content_hash: 'sha256:scene' },
      target: participant.target,
      resolved_tcp: {
        entry: {
          target: participant.target,
          frame: { component_id: participant.target.component_id, frame_id: 'flange' },
        },
        source: 'joint_flange_fallback',
        owner_epoch: intent.request_context.request_owner_epoch,
        registration_generation: 1,
        sequence: 0,
      },
      required_capability_id: participant.required_capability_id,
      source_joint_names: joints,
      start_positions_rad: [0, 0],
      final_positions_rad: [0.1, 0.2],
      final_tcp_pose: {
        position_m: { x_m: 0, y_m: 0, z_m: 0.1 },
        orientation: { x: 0, y: 0, z: 0, w: 1 },
      },
      collision_world_revision: { revision_id: 1, content_hash: 'sha256:collision' },
      path_preview: { source_joint_names: joints, pairs: [], samples: [
        { time_from_start_s: 0, positions_rad: [0, 0], contacts: [], status: 'checked' },
      ] },
      commit_eligible: true,
    },
  };
}

function runtimeScaleResponse(selector: RuntimeScaleControlSelector): Record<string, unknown> {
  const member = (target: { componentId: string; commandTargetId: string }) => ({
    active_control_configuration_generation: 1,
    active_control_configuration_id: `active-${target.commandTargetId}`,
    control_binding_id: `binding-${target.commandTargetId}`,
    control_binding_revision: 'revision-1',
    control_domain_id: `domain-${target.commandTargetId}`,
    control_domain_revision: 'revision-1',
    scaling_domain_id: `scale-${target.commandTargetId}`,
    component_id: target.componentId,
    command_target_id: target.commandTargetId,
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
      component_id: selector.componentId,
      command_target_id: selector.commandTargetId,
    } : {
      program_binding_id: selector.programBindingId,
      group_targets: selector.groupTargets.map((target) => ({
        component_id: target.componentId,
        command_target_id: target.commandTargetId,
      })),
    },
    supported: true,
    ...(selector.kind === 'group'
      ? { group_members: selector.groupTargets.map(member) }
      : {}),
  };
  return {
    api_version: 'manyforge.execution_command_response.v4',
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
    api_version: 'manyforge.program.v3',
    program_id: 'fixture',
    program_profile: 'composable-cell-v1',
    name: 'Fixture',
    catalog: {
      api_version: 'manyforge.node_catalog.v3',
      content_version: '3.0.0',
    },
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
        schema_version: 'manyforge.composer_authoring_forest.v2',
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
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v3',
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
      read_only: false,
      document_id: 'cell',
      document_name: 'Cell',
      document_description: '',
      source_diagnostics: [],
      source_document_digest: null,
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
      schema_version: 'manyforge.prepared_activation_input.v7',
      prepared_activation_id: digestC,
      origin: { kind: 'scenario', scenario_id: 'fixture', source_document_digest: digestA, project_path: '/projects/fixture.scenario' },
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
        schema_version: 'manyforge.assistant_mode_index.v2',
        retained_bundle_id: 'bundle.1',
        graph_revision: 'graph.1',
        assistant_projection_digest: `sha256:${'1'.repeat(64)}`,
        instrument_registry_digest: `sha256:${'2'.repeat(64)}`,
        ai_model_component_id: 'assistant_model',
        assistant_component_id: 'assistant',
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
        assistantMode: 'composer-assistant',
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

  it('requests the configured roots without scanning their entries', async () => {
    const fetchMock = vi.fn().mockResolvedValue(response({
      currentPath: '/workspace/examples',
      parentPath: null,
      roots: ['/workspace/examples', '/projects/cells'],
      projectRoots: [{ hostPath: '/home/user/cells', containerPath: '/projects/cells' }],
      entries: [],
    }));
    vi.stubGlobal('fetch', fetchMock);

    const result = await api.files.browse('scenario', undefined, undefined, true);

    expect(fetchMock).toHaveBeenCalledWith(
      '/api/files/browse?kind=scenario&roots_only=true',
      expect.objectContaining({ method: 'GET' }),
    );
    expect(result.roots).toEqual(['/workspace/examples', '/projects/cells']);
    expect(result.projectRoots).toEqual([{ hostPath: '/home/user/cells', containerPath: '/projects/cells' }]);
    expect(result.entries).toEqual([]);
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

  it('lists, saves and exports Scenario documents', async () => {
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
      directory: '/workspace/examples',
      name: 'Cell',
      description: '',
      confirmOverwrite: true,
    });
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
      directory: '/workspace/examples',
      name: 'Cell',
      description: '',
      confirmOverwrite: true,
    });
    expect(fetchMock.mock.calls[2]?.[0]).toBe(
      '/api/scenario-documents/export',
    );
    expect(JSON.parse(String(
      (fetchMock.mock.calls[2]?.[1] as RequestInit).body,
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
      api_version: 'manyforge.manager_command_response.v2',
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
      api_version: 'manyforge.manager_command_response.v2',
      apply_generation: 3,
      apply_id: 'apply-3',
      candidate_projection_revision: null,
      failure_acknowledgement: 'unavailable',
      error: null,
      failure: null,
      last_progress: {
        phase: 'fence_accepted',
        record_digest: `sha256:${'1'.repeat(64)}`,
      },
      ok: true,
      phase: 'fence_accepted',
      scene_initialization: 'preserve',
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
    const acknowledgedPayload = {
      ...payload,
      failure_acknowledgement: 'acknowledged',
      failure: {
        cause_code: 'stale_generation',
        cause_details: { reason: 'expected_generation_stale' },
        code: 'apply_admission_failed',
        effect_boundary: 'before_effect',
        effect_disposition: 'known',
        message: 'Apply was refused before effects began.',
        phase: 'fence_accepted',
        producer: 'kernel',
      },
      state: 'failed',
    };
    const fetchMock = vi.fn()
      .mockResolvedValueOnce(response(payload))
      .mockResolvedValueOnce(response(acknowledgedPayload));
    vi.stubGlobal('fetch', fetchMock);

    const observed = await api.manager.applyStatus('apply-3', 3);

    expect(fetchMock).toHaveBeenCalledTimes(1);
    expect(fetchMock.mock.calls[0]?.[0]).toBe(
      '/api/manager/apply/status?apply_id=apply-3&apply_generation=3',
    );
    expect(observed.phase).toBe('fence_accepted');

    const requestContext = {
      request_id: 'composer-manager-apply-ack.request-1',
      source_label: 'manyforge_composer',
    };
    const acknowledged = await api.manager.acknowledgeApply('apply-3', 3, requestContext);
    expect(fetchMock).toHaveBeenCalledTimes(2);
    expect(fetchMock.mock.calls[1]?.[0]).toBe(
      '/api/manager/apply/acknowledge?apply_id=apply-3&apply_generation=3',
    );
    expect(fetchMock.mock.calls[1]?.[1]).toMatchObject({ method: 'POST' });
    expect(fetchMock.mock.calls[1]?.[1]?.body).toBe(JSON.stringify({
      request_context: requestContext,
    }));
    expect(acknowledged.failureAcknowledgement).toBe('acknowledged');
  });

  it('rejects an Apply status for a different durable identity', async () => {
    const payload = {
      api_version: 'manyforge.manager_command_response.v2',
      apply_generation: 4,
      apply_id: 'different-apply',
      candidate_projection_revision: null,
      failure_acknowledgement: 'unavailable',
      error: null,
      failure: null,
      last_progress: {
        phase: 'accepted',
        record_digest: `sha256:${'1'.repeat(64)}`,
      },
      ok: true,
      phase: 'accepted',
      scene_initialization: 'preserve',
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
      acknowledgementRequired: true,
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

describe('server-side Apply, Load and New', () => {
  const applied = { outcome: 'succeeded', components: [
    { component_id: 'arm', disposition: 'replace', lifecycle_after: 'prepared' }] };
  it.each([
    ['new', () => api.cellApply.newScenario(), '/api/scenario/new', { discard_unsaved: true }],
    ['load', () => api.cellApply.load('/projects/cell'), '/api/scenario/load',
      { path: '/projects/cell', discard_unsaved: true }],
    ['apply', () => api.cellApply.apply({ draft_revision: 3 }), '/api/deployment/apply',
      { scope: { draft_revision: 3 } }],
    ['revert', () => api.cellApply.revert({ saved: 'x' }), '/api/deployment/revert',
      { scope: { saved: 'x' } }],
  ] as const)('%s calls its endpoint once and returns the ApplyResult', async (_name, call, url, body) => {
    const fetchMock = vi.fn().mockResolvedValue(response(applied));
    vi.stubGlobal('fetch', fetchMock);
    await expect(call()).resolves.toEqual(applied);
    expect(fetchMock).toHaveBeenCalledTimes(1);
    const [calledUrl, options] = fetchMock.mock.calls[0] as [string, RequestInit];
    expect(calledUrl).toBe(url);
    expect(options.method).toBe('POST');
    expect(JSON.parse(String(options.body))).toEqual(body);
  });
  const preview = { components: [
    { component_id: 'arm', disposition: 'replace', reasons: ['description'], restarts: true },
    { component_id: 'gripper', disposition: 'retain', reasons: [], restarts: false }],
  scope: { deployment_draft_revision: 4 },
  program: { executable: false, program_id: 'pick', unresolved_references: [{
    target: { component_id: 'arm', command_target_id: 'arm' },
    reason: 'component_missing', detail: 'arm is removed' }],
  affected_nodes: ['move_1'], affected_execution_groups: [] },
  attachment_changed_components: ['gripper'], detached_components: ['camera'] };
  it.each([
    ['preview', () => api.cellApply.preview(), '/api/deployment/preview'],
    ['revert preview', () => api.cellApply.revertPreview(), '/api/deployment/revert/preview'],
  ] as const)('parses the %s impact', async (_name, call, url) => {
    const fetchMock = vi.fn().mockResolvedValue(response(preview));
    vi.stubGlobal('fetch', fetchMock);
    await expect(call()).resolves.toEqual(preview);
    expect(fetchMock.mock.calls[0]![0]).toBe(url);
  });
  it('parses removed components without lifecycle_after', async () => {
    const result = { outcome: 'succeeded', components: [
      { component_id: 'old_arm', disposition: 'remove' },
      { component_id: 'arm', disposition: 'add', lifecycle_after: 'prepared' }] };
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response(result)));
    await expect(api.cellApply.apply({})).resolves.toEqual(result);
  });
  it('rejects a malformed ApplyResult', async () => {
    vi.stubGlobal('fetch', vi.fn().mockResolvedValue(response({ outcome: 'done', components: [] })));
    await expect(api.cellApply.apply({})).rejects.toThrow('/api/deployment/apply');
  });
});

import { act, createElement, useEffect } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { api, ApiError } from '../api/client';

import type { RuntimeScaleControlStatus } from '../types/robotControl';
import { tcpEntryIdentityKey } from '../types/robotControl';
import { CommandRefusalError } from '../state/commandRefusal';
import { CommandDeliveryUnconfirmedError } from './useHeadlessState';
import {
  acceptedRuntimeScaleStatus,
  mergeRuntimeScaleStatus,
  previewTargetRejection,
  cartesianDiagnosticRejection,
  runtimeScaleControlScopeFence,
  runtimeScaleFlushDelay,
  manualAvailabilityScopeFence,
  mountedToolsFromStatus,
  buildManualAvailabilityIntent,
  useRobotControl,
} from './useRobotControl';
import { gripperReceiptFixture, receiptFixture, statusFixture } from '../testFixtures/robotControl';
import type { ComposerHeadlessState } from '../types/headless';
import {
  buildPreviewIntent,
  initialRobotControlState,
  robotControlAcceptedOperationMode,
  robotControlReducer,
} from '../state/robotControl';

it('manual availability follows owner and hold changes, not observed poses or node ticks', () => {
  const snapshot = {
    executionControlStatus: statusFixture(), authoringAdmission: { admissionGeneration: 1 },
    programCycle: { programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 1, state: 'running',
      executionControlRevision: 1, nodeStatusRevision: 1,
      runtimeControls: { executionSelections: { selectionRevision: 1,
        groups: [{ groupId: 'all', state: 'stopping' }] } },
    },
  } as unknown as ComposerHeadlessState;
  const initial = manualAvailabilityScopeFence(snapshot, 'joint');
  snapshot.executionControlStatus!.observedMotion!.positionsRad = [0.5, 0.6];
  snapshot.executionControlStatus!.observedMotion!.sequence += 1;
  snapshot.programCycle.nodeStatusRevision = 2;
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).toBe(initial);
  expect(manualAvailabilityScopeFence(snapshot, 'pose_goal')).toBe(initial);
  expect(manualAvailabilityScopeFence(snapshot, 'gripper')).not.toBe(initial);
  snapshot.programCycle.runtimeControls!.executionSelections.groups[0].state = 'held';
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).not.toBe(initial);
  const held = manualAvailabilityScopeFence(snapshot, 'joint');
  snapshot.executionControlStatus!.ownerEpoch = 'replacement';
  expect(manualAvailabilityScopeFence(snapshot, 'joint')).not.toBe(held);
});

it('pairs a robot with the descriptor-selected primary among multiple tools', () => {
  const status = statusFixture();
  const motion = status.directControl!;
  motion.targetDescriptor.endEffectorComponentId = 'tool-z';
  const robotTcp = motion.targetDescriptor.tcps[0];
  const otherMountedTool = { ...robotTcp,
    entry: { componentId: 'tool-a', targetId: 'gripper', frameComponentId: 'tool-a', frameRoleId: 'tcp' },
    controllableComponentId: 'tool-a', commandTargetId: 'gripper',
    qualifiedName: 'tool-a.tcp', isDefault: false, mountedHere: true, mountedElsewhere: false };
  const primaryMountedTool = { ...robotTcp,
    entry: { componentId: 'tool-z', targetId: 'gripper', frameComponentId: 'tool-z', frameRoleId: 'tcp' },
    controllableComponentId: 'tool-z', commandTargetId: 'gripper',
    qualifiedName: 'tool-z.tcp', isDefault: true, mountedHere: true, mountedElsewhere: false };
  const unmountedTool = { ...robotTcp,
    entry: { componentId: 'tool-outside', targetId: 'gripper', frameComponentId: 'tool-outside', frameRoleId: 'tcp' },
    controllableComponentId: 'tool-outside', commandTargetId: 'gripper',
    qualifiedName: 'tool-outside.tcp', isDefault: false, mountedHere: false, mountedElsewhere: false };
  motion.targetDescriptor.tcps = [otherMountedTool, unmountedTool, primaryMountedTool];

  expect(mountedToolsFromStatus(status)).toEqual({ 'robot-a': 'tool-z' });
});

it('pairs a selected flange tool even when it declares no TCP row', () => {
  const status = statusFixture();
  const motion = status.directControl!;
  motion.targetDescriptor.endEffectorComponentId = 'tcp-less-primary';
  const robotTcp = motion.targetDescriptor.tcps[0];
  motion.targetDescriptor.tcps = [{ ...robotTcp,
    entry: { componentId: 'other-tool', targetId: 'gripper', frameComponentId: 'other-tool', frameRoleId: 'tcp' },
    controllableComponentId: 'other-tool', commandTargetId: 'gripper',
    qualifiedName: 'other-tool.tcp', isDefault: false, mountedHere: true, mountedElsewhere: false }];

  expect(mountedToolsFromStatus(status)).toEqual({ 'robot-a': 'tcp-less-primary' });
});

it('leaves a robot unpaired when the descriptor reports no flange child', () => {
  expect(mountedToolsFromStatus(statusFixture())).toEqual({});
});

function pneumaticStatus(outputMode: 'commanded' | 'none' = 'commanded') {
  const status = statusFixture();
  const descriptor = status.directControlGripper!.targetDescriptor;
  Object.assign(descriptor, { actuationKind: 'pneumatic', outputMode,
    completionPolicy: outputMode === 'none' ? 'no_output' : 'output_delivery',
    graspConfirmationPolicy: 'not_required', openStateRef: 'configured-open',
    namedPresets: [{ name: 'configured-open', masterPosition: 0.037,
      coordinates: [{ sourceCoordinateName: descriptor.sourceCoordinateName, position: 0.037 }] }],
  });
  status.observedGripper = null;
  status.observedGrippers = [];
  return status;
}

it.each(['commanded', 'none'] as const)(
  'queries pneumatic %s from a valid draft then the exact configured preset without authoring', (outputMode) => {
    const status = pneumaticStatus(outputMode);
    const before = structuredClone(status);
    const position = (draft: number | null) => buildManualAvailabilityIntent(
      status, 'gripper', draft, 'availability-only',
    ).participants[0].payload;
    expect(position(0.013)).toMatchObject({ command_value: {
      position: { kind: 'linear_m', value_m: 0.013 },
    } });
    for (const invalidDraft of [null, NaN, Infinity, -0.01, 0.05]) {
      expect(position(invalidDraft)).toMatchObject({ command_value: {
        position: { kind: 'linear_m', value_m: 0.037 },
      } });
    }
    expect(status).toEqual(before);
    const state = robotControlReducer(initialRobotControlState, { type: 'status_resolved', status });
    expect(state.draftGripperPosition).toBeNull();
    expect(state.intent).toBeNull();
    expect(state.preview).toBeNull();
    expect(() => buildPreviewIntent({ ...state, mode: 'gripper' }, 'actual-preview'))
      .toThrow(/Enter an initial component-local gripper target/);
  },
);

it('refuses query-only preset guessing and preserves position/motion observation prerequisites', () => {
  for (const edit of [
    (descriptor: NonNullable<ReturnType<typeof pneumaticStatus>['directControlGripper']>['targetDescriptor']) => { descriptor.openStateRef = null; },
    (descriptor: NonNullable<ReturnType<typeof pneumaticStatus>['directControlGripper']>['targetDescriptor']) => {
      descriptor.openStateRef = 'missing';
    },
    (descriptor: NonNullable<ReturnType<typeof pneumaticStatus>['directControlGripper']>['targetDescriptor']) => {
      descriptor.namedPresets.push({ ...descriptor.namedPresets[0] });
    },
    (descriptor: NonNullable<ReturnType<typeof pneumaticStatus>['directControlGripper']>['targetDescriptor']) => {
      descriptor.namedPresets[0].masterPosition = NaN;
    },
    (descriptor: NonNullable<ReturnType<typeof pneumaticStatus>['directControlGripper']>['targetDescriptor']) => {
      descriptor.namedPresets[0].masterPosition = 0.05;
    },
  ]) {
    const status = pneumaticStatus();
    // A literal preset named open is deliberately irrelevant to the configured ref.
    status.directControlGripper!.targetDescriptor.namedPresets.push({ name: 'open', masterPosition: 0,
      coordinates: [{ sourceCoordinateName: 'finger_joint', position: 0 }] });
    edit(status.directControlGripper!.targetDescriptor);
    expect(() => buildManualAvailabilityIntent(status, 'gripper', null, 'refused')).toThrow();
  }
  const position = statusFixture();
  position.observedGripper = null;
  expect(() => buildManualAvailabilityIntent(position, 'gripper', 0.02, 'position'))
    .toThrow(/Gripper observation is unavailable/);
  position.observedMotion = null;
  expect(() => buildManualAvailabilityIntent(position, 'joint', null, 'motion'))
    .toThrow(/Motion observation is unavailable/);
});

it('fences pneumatic availability on the current descriptor and addressed target', () => {
  const snapshot = { executionControlStatus: pneumaticStatus(), authoringAdmission: { admissionGeneration: 1 },
    programCycle: { programRunnerOwnerEpoch: 'runner.1', cycleGeneration: 1, state: 'running',
      executionControlRevision: 1, runtimeControls: { executionSelections: { selectionRevision: 1, groups: [] } } },
  } as unknown as ComposerHeadlessState;
  const before = manualAvailabilityScopeFence(snapshot, 'gripper');
  snapshot.executionControlStatus!.directControlGripper!.targetDescriptor.openStateRef = 'changed-ref';
  expect(manualAvailabilityScopeFence(snapshot, 'gripper')).not.toBe(before);
  const sourceChanged = manualAvailabilityScopeFence(snapshot, 'gripper');
  snapshot.executionControlStatus!.directControlGripper!.programBindingId = 'new-binding';
  expect(manualAvailabilityScopeFence(snapshot, 'gripper')).not.toBe(sourceChanged);
});

it('keeps known target rejection local without hiding transport or protocol failures', () => {
  expect(previewTargetRejection(new ApiError('IK failed', 500, {
    detail: { code: 'ik_failed', details: { reason: 'native_ik_no_solution' } },
  }))).toContain('may be out of reach, outside joint limits, or in collision');
  expect(previewTargetRejection(new ApiError('Collision', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_goal_state_collision' } },
  }))).toContain('would collide at the target');
  expect(previewTargetRejection(new ApiError('Collision', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_start_state_collision' } },
  }))).toContain('current robot pose is in collision');
  expect(previewTargetRejection(new ApiError('Wrong result', 500, {
    detail: { code: 'ik_failed', details: { reason: 'native_goal_state_collision' } },
  }))).toBeNull();
  expect(previewTargetRejection(new ApiError('No path', 500, {
    detail: { code: 'planning_failed', details: { reason: 'native_rrt_failed' } },
  })))
    .toContain('No valid path');
  for (const code of ['ik_failed', 'planning_failed']) {
    expect(previewTargetRejection(new ApiError('Unavailable', 500, {
      detail: { code, details: { reason: 'native_ik_transport_failure' } },
    }))).toBeNull();
  }
  expect(previewTargetRejection(new TypeError('Network error'))).toBeNull();
  expect(previewTargetRejection(new Error('Invalid preview receipt'))).toBeNull();
});

const status = {
  selector: {
    kind: 'singular',
    programBindingId: 'binding-1',
    componentId: 'robot',
    commandTargetId: 'arm',
  },
  controlGeneration: 3,
  requestedScale: 0.4,
  supported: true,
  frozen: false,
  controlDomainId: 'domain-1',
  controlDomainRevision: 'domain-revision-1',
  controlBindingId: 'control-binding-1',
  controlBindingRevision: 'control-binding-revision-1',
  scalingDomainId: 'scaling-domain-1',
  activeControlConfigurationId: 'configuration-1',
  activeControlConfigurationGeneration: 7,
  activeOperationId: null,
  activeOperationRevision: null,
  activeOperationScaleGeneration: null,
  effectiveScale: null,
  applicationState: null,
  reason: null,
} satisfies RuntimeScaleControlStatus;

describe('persistent runtime-scale transport cadence', () => {
  it('keeps distinct synchronized target sets in distinct correlation fences', () => {
    const group = {
      ...status,
      selector: {
        kind: 'group' as const,
        programBindingId: 'binding-1',
        groupTargets: [
          { componentId: 'robot', commandTargetId: 'arm' },
          { componentId: 'robot', commandTargetId: 'gripper' },
        ],
      },
      groupMembers: [
        {
          componentId: 'robot', commandTargetId: 'arm',
          controlDomainId: 'domain-arm', controlDomainRevision: 'rev-arm',
          controlBindingId: 'binding-arm', controlBindingRevision: 'binding-rev-arm',
          scalingDomainId: 'scale-arm', activeControlConfigurationId: 'config-arm',
          activeControlConfigurationGeneration: 1,
        },
        {
          componentId: 'robot', commandTargetId: 'gripper',
          controlDomainId: 'domain-gripper', controlDomainRevision: 'rev-gripper',
          controlBindingId: 'binding-gripper', controlBindingRevision: 'binding-rev-gripper',
          scalingDomainId: 'scale-gripper', activeControlConfigurationId: 'config-gripper',
          activeControlConfigurationGeneration: 1,
        },
      ],
    };
    const differentGroup = {
      ...group,
      selector: { ...group.selector, groupTargets: [
        { componentId: 'robot', commandTargetId: 'arm' },
        { componentId: 'robot', commandTargetId: 'camera' },
      ] },
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', differentGroup),
    );
    const delimiterCollisionGroup = {
      ...group,
      selector: {
        kind: 'group' as const,
        programBindingId: 'binding-1',
        groupTargets: [
          { componentId: 'a', commandTargetId: 'b\u0000c' },
          { componentId: 'a\u0000b', commandTargetId: 'c' },
        ],
      },
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', delimiterCollisionGroup),
    );
    const reordered = { ...group, groupMembers: [...group.groupMembers].reverse() };
    expect(runtimeScaleControlScopeFence('owner-1', group)).toBe(
      runtimeScaleControlScopeFence('owner-1', reordered),
    );
    const changedMember = {
      ...group,
      groupMembers: group.groupMembers.map((member) => (
        member.commandTargetId === 'arm'
          ? { ...member, activeControlConfigurationGeneration: 2 }
          : member
      )),
    };
    expect(runtimeScaleControlScopeFence('owner-1', group)).not.toBe(
      runtimeScaleControlScopeFence('owner-1', changedMember),
    );
  });

  it('sends the first value immediately and coalesces later values to 250 ms', () => {
    expect(runtimeScaleFlushDelay(10, Number.NEGATIVE_INFINITY, true, false))
      .toBe(0);
    expect(runtimeScaleFlushDelay(100, 0, true, false)).toBe(150);
    expect(runtimeScaleFlushDelay(300, 0, true, false)).toBe(0);
  });

  it('does not schedule without a queued value or while one request is in flight', () => {
    expect(runtimeScaleFlushDelay(10, 0, false, false)).toBeNull();
    expect(runtimeScaleFlushDelay(10, 0, true, true)).toBeNull();
  });

  it('invalidates pending input only when its stable owner or scaling scope changes', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-2', status)).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      selector: { ...status.selector, programBindingId: 'binding-2' },
    })).not.toBe(initial);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      scalingDomainId: 'scaling-domain-2',
    })).not.toBe(initial);
  });

  it('keeps queued input across ordinary scale acceptance revisions', () => {
    const initial = runtimeScaleControlScopeFence('owner-1', status);
    expect(runtimeScaleControlScopeFence('owner-1', {
      ...status,
      controlGeneration: 4,
      requestedScale: 0.25,
      activeControlConfigurationId: 'configuration-2',
      activeControlConfigurationGeneration: 8,
      activeOperationScaleGeneration: 4,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toBe(initial);
  });

  it('uses an exact owner acceptance to fence the next scale update', () => {
    const accepted = acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: 'operation-1',
      activeOperationRevision: 8,
      activeOperationScaleGeneration: 2,
    }, 'scale-request-1', 0.25);
    expect(accepted).toMatchObject({
      controlGeneration: 4,
      requestedScale: 0.25,
      activeOperationId: 'operation-1',
    });
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-2',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
    expect(() => acceptedRuntimeScaleStatus(status, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.4,
      controlGeneration: 4,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toThrow(/does not match/);
  });

  it('keeps the projected active operation when the acceptance carries none', () => {
    const projected = {
      ...status,
      activeOperationId: 'operation-1',
      activeOperationRevision: 8,
      activeOperationScaleGeneration: 2,
    };
    expect(acceptedRuntimeScaleStatus(projected, {
      requestId: 'scale-request-1',
      selector: status.selector,
      requestedScale: 0.25,
      controlGeneration: projected.controlGeneration + 1,
      activeOperationId: null,
      activeOperationRevision: null,
      activeOperationScaleGeneration: null,
    }, 'scale-request-1', 0.25)).toMatchObject({
      requestedScale: 0.25,
      activeOperationId: 'operation-1',
      activeOperationRevision: 8,
      activeOperationScaleGeneration: 2,
    });
  });

  it('rejects a delayed projection older than the accepted owner generation', () => {
    const accepted = {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
    };
    expect(mergeRuntimeScaleStatus(accepted, status)).toBe(accepted);
    expect(mergeRuntimeScaleStatus(accepted, {
      ...status,
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    })).toMatchObject({
      controlGeneration: 5,
      requestedScale: 0.25,
      effectiveScale: 0.25,
      applicationState: 'applied',
    });
  });
});


describe('pneumatic manual availability observation', () => {
  let root: import('react-dom/client').Root;
  let latest: ReturnType<typeof useRobotControl>;
  const pending: Array<{ intent: Parameters<typeof api.execution.manualAvailability>[0];
    signal: AbortSignal | undefined; resolve: (value: Awaited<ReturnType<typeof api.execution.manualAvailability>>) => void }> = [];
  function Harness({ snapshot }: { snapshot: ComposerHeadlessState }) {
    const result = useRobotControl(true, true, true, snapshot);
    useEffect(() => { latest = result; }, [result]);
    return null;
  }
  async function render(snapshot: ComposerHeadlessState) {
    await act(async () => root.render(createElement(Harness, { snapshot })));
  }
  beforeEach(async () => {
    const document = Object.assign(new EventTarget(), {
      nodeType: 9, activeElement: null, defaultView: {},
    });
    const makeNode = (tag: string) => Object.assign(new EventTarget(), {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
      appendChild: vi.fn(), removeChild: vi.fn(), insertBefore: vi.fn(),
      setAttribute: vi.fn(), removeAttribute: vi.fn(),
    });
    Object.assign(document, { createElement: makeNode, documentElement: makeNode('html') });
    const browser = Object.assign(new EventTarget(), { document, HTMLIFrameElement: class {},
      setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout });
    document.defaultView = browser;
    vi.stubGlobal('window', browser);
    vi.stubGlobal('document', document);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const { createRoot } = await import('react-dom/client');
    root = createRoot(makeNode('div') as unknown as Element);
    pending.length = 0;
    vi.spyOn(api.execution, 'manualAvailability').mockImplementation((intent, signal) => (
      new Promise((resolve) => pending.push({ intent, signal, resolve }))
    ));
    vi.spyOn(api.execution, 'previewCapabilityCommand');
    vi.spyOn(api.execution, 'previewMotion');
  });
  afterEach(async () => {
    await act(async () => root.unmount());
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });
  it('drops an in-flight preview after hiding and refuses hidden approval', async () => {
    const status = statusFixture();
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 } } as unknown as ComposerHeadlessState;
    let release!: (receipt: ReturnType<typeof receiptFixture>) => void;
    const pendingPreview = new Promise<ReturnType<typeof receiptFixture>>((resolve) => { release = resolve; });
    const preview = vi.mocked(api.execution.previewMotion).mockReturnValue(pendingPreview);
    const send = vi.fn();
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function VisibleHarness() {
      const result = useRobotControl(true, false, false, snapshot, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    vi.spyOn(window, 'setTimeout').mockImplementation(((callback: () => void) => globalThis.setTimeout(callback, 0)) as never);
    await act(async () => root.render(createElement(VisibleHarness)));
    await act(async () => { latest.setManualPanelVisible(true); latest.editJoint(0, 0.2); });
    await act(async () => { await new Promise((resolve) => globalThis.setTimeout(resolve, 20)); });
    expect(preview).toHaveBeenCalledTimes(1);
    const generation = latest.state.generation;
    await act(async () => latest.setManualPanelVisible(false));
    await act(async () => { release(receiptFixture(status, 'late', [0.2, 0])); await pendingPreview; });
    expect(latest.state.generation).toBeGreaterThan(generation);
    expect(latest.state.preview).toBeNull();
    expect(latest.state.ghost).toBeNull();
    await act(async () => latest.approve());
    expect(send).not.toHaveBeenCalled();
  });
  it('retains the Move To label only for the operation id in the accepted receipt', async () => {
    const status = statusFixture();
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 } } as unknown as ComposerHeadlessState;
    const send = vi.fn(async (_kind: string, _payload: unknown, requestId: string) => ({
      result: { ok: true, command: 'submit', accepted: { request_id: requestId, operation_id: 'move-to.1' } },
    }));
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function MoveToHarness({ current }: { current: ComposerHeadlessState }) {
      const result = useRobotControl(true, false, false, current, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    vi.spyOn(window, 'setTimeout')
      .mockImplementation(((callback: () => void) => globalThis.setTimeout(callback, 0)) as never);
    vi.mocked(api.execution.previewMotion).mockImplementation(async (intent) => (
      receiptFixture(status, intent.request_context.request_id, [0.2, 0.3])
    ));
    await act(async () => root.render(createElement(MoveToHarness, { current: snapshot })));
    await act(async () => {
      latest.setManualPanelVisible(true);
      latest.selectMode('move_to');
      latest.editMoveToJoints([0.2, 0.3]);
    });
    await act(async () => { await new Promise((resolve) => globalThis.setTimeout(resolve, 20)); });
    expect(latest.state.phase).toBe('ready');
    await act(async () => latest.approve());
    expect(latest.state.acceptedOperationMode).toMatchObject({
      ownerEpoch: status.ownerEpoch, operationId: 'move-to.1', mode: 'move_to',
    });

    const movingStatus = structuredClone(status);
    const target = movingStatus.directControl!;
    movingStatus.manualOperations = [{
      programRunId: target.programRunId,
      operationId: 'move-to.1', operationRevision: 1, resident: true, state: 'executing',
      progress: 0, activeStopId: null, activeStopGeneration: null, terminalError: null,
      controllableComponentId: target.controllableComponentId, commandTargetId: target.commandTargetId,
    }];
    const movingSnapshot = { ...snapshot, executionControlStatus: movingStatus } as ComposerHeadlessState;
    await act(async () => root.render(createElement(MoveToHarness, { current: movingSnapshot })));
    expect(robotControlAcceptedOperationMode(latest.state)).toBe('move_to');
    expect(robotControlAcceptedOperationMode({
      ...latest.state,
      operation: { ...latest.state.operation!, operationId: 'external-operation' },
    })).toBeNull();
  });
  it('does not submit Set TCP during an active manual operation', async () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    const flange = { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
      frameRoleId: 'flange', qualifiedName: 'robot-a.flange', isDefault: false };
    status.directControl!.targetDescriptor.tcps.push(flange);
    status.manualOperations = [{ programRunId: status.directControl!.programRunId,
      controllableComponentId: status.directControl!.controllableComponentId,
      commandTargetId: status.directControl!.commandTargetId,
      operationId: 'operation-1', operationRevision: 1, resident: true, state: 'executing',
      progress: 0, activeStopId: null, activeStopGeneration: null, terminalError: null }];
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    const send = vi.fn();
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness() {
      const result = useRobotControl(true, false, false, snapshot, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    await act(async () => root.render(createElement(SetTcpHarness)));
    expect(latest.state.phase).toBe('executing');
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
    expect(send).not.toHaveBeenCalled();
    expect(latest.tcpApplying).toBe(false);
  });

  it('submits Set TCP once and keeps the owner value until the result', async () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    const flange = { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
      frameRoleId: 'flange', qualifiedName: 'robot-a.flange', isDefault: false };
    status.directControl!.targetDescriptor.tcps.push(flange);
    status.observedMotion!.tcpPoses[tcpEntryIdentityKey(flange.entry)] = status.observedMotion!.currentTcpPose!;
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    let finish!: () => void;
    const sent = new Promise<void>((resolve) => { finish = resolve; });
    const send = vi.fn(async (_kind: string, _payload: unknown, requestId: string) => {
      await sent;
      return { result: { ok: true, command: 'submit', accepted: { request_id: requestId, operation_id: 'operation.1' },
        active_tcp: [{ component_id: 'robot-a', entry: {
          target: { component_id: 'robot-a', command_target_id: 'arm' },
          frame: { component_id: 'robot-a', frame_id: 'flange' },
        }, owner_epoch: status.ownerEpoch, registration_generation: 1, sequence: 1 }] } };
    });
    let transportStatus = 'connected';
    let transportError: string | null = null;
    let shellSnapshot = snapshot;
    const source = { sendExecutionCommand: send,
      getShellSnapshot: () => ({ transportStatus, transportError, semanticStatus: 'current', snapshot: shellSnapshot }),
    } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness({ current }: { current: ComposerHeadlessState }) {
      const result = useRobotControl(true, false, false, current, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    const renderTcp = async (current: ComposerHeadlessState) => {
      shellSnapshot = current;
      await act(async () => root.render(createElement(SetTcpHarness, { current })));
    };
    await renderTcp(snapshot);
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
    expect(latest.tcpApplying).toBe(true);
    expect(latest.tcpResumeStatus!.directControlTargets[0].activeTcp).toBeNull();
    expect(latest.selectedStatus!.directControl!.activeTcp!.entry!.frameRoleId).toBe('tcp');
    expect(send).toHaveBeenCalledTimes(1);
    expect(send).toHaveBeenCalledWith('operation_start', expect.objectContaining({
      participants: [expect.objectContaining({ payload: { kind: 'set_tcp', tcp: {
        kind: 'entry',
        target: { component_id: 'robot-a', command_target_id: 'arm' },
        frame: { component_id: 'robot-a', frame_id: 'flange' },
      } } })],
    }), expect.any(String));
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
    expect(send).toHaveBeenCalledTimes(1);
    await act(async () => finish());
    expect(latest.tcpApplying).toBe(true);
    expect(latest.selectedStatus!.directControl!.activeTcp!.entry!.frameRoleId).toBe('flange');
    expect(latest.tcpResumeStatus!.directControlTargets[0].activeTcp!.entry!.frameRoleId).toBe('flange');
    transportError = 'Command delivery is unconfirmed';
    await renderTcp(snapshot);
    expect(latest.tcpResumeStatus).toBeNull();
    transportError = null;
    transportStatus = 'disconnected';
    await renderTcp(snapshot);
    expect(latest.tcpResumeStatus).toBeNull();
    transportStatus = 'connected';
    await renderTcp(snapshot);
    await act(async () => {
      latest.selectMode('pose_goal');
      latest.editPose({ position_m: { x_m: 0.2, y_m: 0.3, z_m: 0.4 },
        orientation: { x: 0, y: 0, z: 0, w: 1 } });
    });
    expect(api.execution.previewMotion).not.toHaveBeenCalled();
    const stale = structuredClone(snapshot);
    stale.executionControlStatus!.directControl!.activeTcp = {
      entry: flange.entry, display: flange.qualifiedName, ownerEpoch: status.ownerEpoch,
      registrationGeneration: 1, sequence: 0,
    };
    await renderTcp(stale);
    expect(latest.tcpApplying).toBe(true);
    const updated = structuredClone(snapshot);
    updated.executionControlStatus!.directControl!.activeTcp = {
      entry: flange.entry, display: flange.qualifiedName, ownerEpoch: status.ownerEpoch,
      registrationGeneration: 1, sequence: 1,
    };
    await renderTcp(updated);
    expect(latest.tcpApplying).toBe(false);
    expect(latest.selectedStatus!.directControl!.activeTcp!.entry!.frameRoleId).toBe('flange');
    expect(send).toHaveBeenCalledTimes(1);
  });
  it.each(['malformed success', 'admitted failure'] as const)(
    'waits for a strictly newer status after %s without a usable TCP row', async (kind) => {
      const status = statusFixture();
      const row = status.directControl!.targetDescriptor.tcps[0];
      const flange = { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
        frameRoleId: 'flange', qualifiedName: 'robot-a.flange', isDefault: false };
      status.directControl!.targetDescriptor.tcps.push(flange);
      const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
        programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
      const raw = { ok: false, error: { code: 'tcp_failed', message: 'TCP failed',
        details: { immediate_step: 'admitted' } }, active_tcp: [] };
      const send = vi.fn(async () => {
        if (kind === 'admitted failure') throw new CommandRefusalError(
          { code: 'tcp_failed', message: 'TCP failed', details: { immediate_step: 'admitted' }, origin: null },
          { executionResult: raw });
        return { result: { ok: true, command: 'submit', active_tcp: [] } };
      });
      const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
      function SetTcpHarness({ current }: { current: ComposerHeadlessState }) {
        const result = useRobotControl(true, false, false, current, source);
        useEffect(() => { latest = result; }, [result]);
        return null;
      }
      await act(async () => root.render(createElement(SetTcpHarness, { current: snapshot })));
      await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
      expect(latest.tcpApplying).toBe(true);
      await act(async () => root.render(createElement(SetTcpHarness, { current: structuredClone(snapshot) })));
      expect(latest.tcpApplying).toBe(true);
      const newer = structuredClone(snapshot);
      newer.executionControlStatus!.directControl!.activeTcp = {
        entry: null, display: null, ownerEpoch: status.ownerEpoch, registrationGeneration: 1, sequence: 1,
      };
      await act(async () => root.render(createElement(SetTcpHarness, { current: newer })));
      expect(latest.tcpApplying).toBe(false);
      expect(latest.selectedStatus!.directControl!.activeTcp!.entry).toBeNull();
      expect(send).toHaveBeenCalledTimes(1);
    },
  );
  it('adopts an admitted failure’s confirmed row while waiting for status', async () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    const flange = { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
      frameRoleId: 'flange', qualifiedName: 'robot-a.flange', isDefault: false };
    status.directControl!.targetDescriptor.tcps.push(flange);
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    const raw = { ok: false, error: { code: 'tcp_failed', message: 'TCP failed',
      details: { immediate_step: 'admitted' } }, active_tcp: [{ component_id: 'robot-a',
      entry: {
        target: { component_id: 'robot-a', command_target_id: 'arm' },
        frame: { component_id: 'robot-a', frame_id: 'flange' },
      },
      owner_epoch: status.ownerEpoch, registration_generation: 1, sequence: 1 }] };
    const send = vi.fn(async () => { throw new CommandRefusalError(
      { code: 'tcp_failed', message: 'TCP failed', details: { immediate_step: 'admitted' }, origin: null },
      { executionResult: raw }); });
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness({ current }: { current: ComposerHeadlessState }) {
      const result = useRobotControl(true, false, false, current, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    await act(async () => root.render(createElement(SetTcpHarness, { current: snapshot })));
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
    expect(latest.tcpApplying).toBe(true);
    expect(latest.selectedStatus!.directControl!.activeTcp!.entry).toEqual(flange.entry);
    const caughtUp = structuredClone(snapshot);
    caughtUp.executionControlStatus!.directControl!.activeTcp = {
      entry: flange.entry, display: flange.qualifiedName, ownerEpoch: status.ownerEpoch,
      registrationGeneration: 1, sequence: 1,
    };
    await act(async () => root.render(createElement(SetTcpHarness, { current: caughtUp })));
    expect(latest.tcpApplying).toBe(false);
    expect(send).toHaveBeenCalledTimes(1);
  });
  it('keeps Applying after a lost Set TCP receipt until the owner stamp advances', async () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    const flange = { ...row, entry: { ...row.entry, frameRoleId: 'flange' },
      frameRoleId: 'flange', qualifiedName: 'robot-a.flange', isDefault: false };
    status.directControl!.targetDescriptor.tcps.push(flange);
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    let currentShell: ComposerHeadlessState | null = snapshot;
    let transportStatus = 'connected';
    let transportError: string | null = null;
    let loseReceipt!: () => void;
    const send = vi.fn(() => new Promise((_, reject) => {
      loseReceipt = () => reject(new CommandDeliveryUnconfirmedError('Command delivery is unconfirmed'));
    }));
    const source = { sendExecutionCommand: send, getShellSnapshot: () => ({
      transportStatus, semanticStatus: 'current', transportError, snapshot: currentShell,
    }) } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness({ current }: { current: ComposerHeadlessState | null }) {
      const result = useRobotControl(true, false, false, current, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    const renderTcp = async (current: ComposerHeadlessState | null) => {
      currentShell = current;
      await act(async () => root.render(createElement(SetTcpHarness, { current })));
    };
    await renderTcp(snapshot);
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(flange.entry)));
    expect(send).toHaveBeenCalledTimes(1);
    transportStatus = 'disconnected';
    transportError = 'Command delivery is unconfirmed';
    await renderTcp(null);
    await act(async () => loseReceipt());
    expect(latest.tcpApplying).toBe(true);
    expect(latest.tcpResumeStatus).toBeNull();
    transportStatus = 'connected';
    transportError = null;
    await renderTcp(snapshot);
    expect(latest.tcpApplying).toBe(true);
    expect(latest.tcpResumeStatus!.directControlTargets[0].activeTcp).toBeNull();
    const observed = structuredClone(snapshot);
    observed.executionControlStatus!.directControl!.activeTcp = {
      entry: flange.entry, display: flange.qualifiedName, ownerEpoch: status.ownerEpoch,
      registrationGeneration: 1, sequence: 1,
    };
    await renderTcp(observed);
    expect(latest.tcpApplying).toBe(false);
    expect(latest.tcpResumeStatus!.directControlTargets[0].activeTcp!.entry).toEqual(flange.entry);
    expect(send).toHaveBeenCalledTimes(1);
  });
  it('does not submit Set TCP without a versioned robot status', async () => {
    const status = statusFixture();
    status.directControl!.activeTcp = null;
    const row = status.directControl!.targetDescriptor.tcps[0];
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    const send = vi.fn();
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness() {
      const result = useRobotControl(true, false, false, snapshot, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    await act(async () => root.render(createElement(SetTcpHarness)));
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(row.entry)));
    expect(send).not.toHaveBeenCalled();
    expect(latest.tcpApplying).toBe(false);
  });
  it('shows a Set TCP refusal and leaves the owner value unchanged', async () => {
    const status = statusFixture();
    const row = status.directControl!.targetDescriptor.tcps[0];
    const other = { ...row, entry: { componentId: 'other-tool', targetId: 'tool', frameComponentId: 'other-tool', frameRoleId: 'tcp' },
      mountedHere: false, mountedElsewhere: true };
    status.directControl!.targetDescriptor.tcps.push(other);
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { state: 'idle_after_reset' } } as unknown as ComposerHeadlessState;
    const send = vi.fn().mockRejectedValue(new Error('Tool not mounted on this robot.'));
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function SetTcpHarness() {
      const result = useRobotControl(true, false, false, snapshot, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    await act(async () => root.render(createElement(SetTcpHarness)));
    await act(async () => latest.selectTcp(tcpEntryIdentityKey(other.entry)));
    expect(latest.tcpError).toBe('Tool not mounted on this robot.');
    expect(latest.tcpApplying).toBe(false);
    expect(latest.selectedStatus!.directControl!.activeTcp!.entry!.frameRoleId).toBe('tcp');
    expect(send).toHaveBeenCalledTimes(1);
  });
  it('has no idle-only gate for mapped pneumatic manual control', async () => {
    const status = pneumaticStatus();
    Object.assign(status.directControlGripper!.targetDescriptor, {
      completionPolicy: 'no_output', pneumaticOutputs: [{ controllableComponentId: 'valve_io',
        signalName: 'valve', openValue: false, closeValue: true }],
    });
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { programRunnerOwnerEpoch: 'runner.1', programRunId: 'program.1', cycleGeneration: 1,
        state: 'running', executionControlRevision: 1,
        runtimeControls: { executionSelections: { selectionRevision: 1, groups: [] } } },
    } as unknown as ComposerHeadlessState;
    await render(snapshot);
    await act(async () => { latest.selectMode('gripper'); latest.setManualPanelVisible(true); });
    expect(latest.manualControlUnavailableReason ?? '').not.toContain('idle after Reset');
  });
  it('refuses a missing current observation when a pneumatic target becomes Position', async () => {
    const snapshot = { executionControlStatus: pneumaticStatus(), authoringAdmission: { admissionGeneration: 1 },
      programCycle: { programRunnerOwnerEpoch: 'runner.1', programRunId: 'program.1', cycleGeneration: 1,
        state: 'running', executionControlRevision: 1,
        runtimeControls: { executionSelections: { selectionRevision: 1, groups: [] } } },
    } as unknown as ComposerHeadlessState;
    await render(snapshot);
    await act(async () => { latest.selectMode('gripper'); latest.setManualPanelVisible(true); });
    expect(pending).toHaveLength(1);
    const successor = structuredClone(snapshot);
    successor.executionControlStatus!.directControlGripper!.programBindingId = 'position.successor';
    Object.assign(successor.executionControlStatus!.directControlGripper!.targetDescriptor, {
      actuationKind: 'position_commanded', outputMode: 'commanded',
      completionPolicy: 'goal_fenced_target_reached', graspConfirmationPolicy: 'binding_feedback_required',
    });
    await render(successor);
    expect(pending[0].signal!.aborted).toBe(true);
    expect(pending).toHaveLength(1);
    expect(latest.manualControlAllowed).toBe(false);
    expect(latest.manualControlUnavailableReason).toBe('Gripper observation is unavailable.');
    await act(async () => pending[0].resolve({ requestId: pending[0].intent.request_context.request_id,
      requestOwnerEpoch: pending[0].intent.request_context.request_owner_epoch,
      intentDigest: 'sha256:obsolete-query', available: true, unavailableReason: null,
      cleanupRequired: false, manualHandover: null }));
    expect(latest.manualControlAllowed).toBe(false);
    expect(latest.state.draftGripperPosition).toBeNull();
  });
  it.each(['commanded', 'none'] as const)(
    'ignores a late %s query across a source/target change during Stop without drafting or previewing', async (mode) => {
      const snapshot = { executionControlStatus: pneumaticStatus(mode), authoringAdmission: { admissionGeneration: 1 },
        programCycle: { programRunnerOwnerEpoch: 'runner.1', programRunId: 'program.1', cycleGeneration: 1,
          state: 'running', executionControlRevision: 1,
          runtimeControls: { executionSelections: { selectionRevision: 1,
            groups: [{ groupId: 'cell', state: 'stopping' }] } } },
      } as unknown as ComposerHeadlessState;
      await render(snapshot);
      await act(async () => {
        latest.selectMode('gripper');
        latest.setManualPanelVisible(true);
      });
      expect(pending).toHaveLength(1);
      expect(pending[0].intent.request_context.request_owner_epoch).toBe(snapshot.executionControlStatus!.ownerEpoch);
      expect(pending[0].intent.program_binding_id).toBe(snapshot.executionControlStatus!.directControlGripper!.programBindingId);
      expect(pending[0].intent.participants[0].payload).toMatchObject({ command_value: {
        position: { kind: 'linear_m', value_m: 0.037 },
      } });
      const successor = structuredClone(snapshot);
      successor.executionControlStatus!.ownerEpoch = 'owner.successor';
      successor.executionControlStatus!.directControlGripper!.programBindingId = 'binding.successor';
      const successorPreset = successor.executionControlStatus!.directControlGripper!.targetDescriptor.namedPresets[0];
      successorPreset.masterPosition = 0.033;
      successorPreset.coordinates[0].position = 0.033;
      await render(successor);
      expect(pending[0].signal!.aborted).toBe(true);
      // Scope synchronization may cancel an intermediate successor query.
      // Every such request must still address the current snapshot exactly.
      const successorQueries = pending.slice(1);
      const liveQueries = successorQueries.filter((query) => !query.signal!.aborted);
      expect(liveQueries).toHaveLength(1);
      const currentQuery = liveQueries[0];
      expect(currentQuery).toBe(successorQueries.at(-1));
      for (const query of successorQueries) {
        expect(query.intent.request_context.request_owner_epoch).toBe('owner.successor');
        expect(query.intent.program_binding_id).toBe('binding.successor');
        expect(query.intent.participants[0].payload).toMatchObject({ command_value: {
          position: { kind: 'linear_m', value_m: 0.033 },
        } });
      }
      const answer = (query: typeof currentQuery, available: boolean) => ({
        requestId: query.intent.request_context.request_id,
        requestOwnerEpoch: query.intent.request_context.request_owner_epoch,
        intentDigest: 'sha256:availability-only', available,
        unavailableReason: available ? null : 'component_not_ready', cleanupRequired: false, manualHandover: null });
      for (const obsolete of pending.filter((query) => query !== currentQuery)) {
        expect(obsolete.signal!.aborted).toBe(true);
        await act(async () => obsolete.resolve(answer(obsolete, true)));
        expect(latest.manualControlAllowed).toBe(false);
      }
      await act(async () => currentQuery.resolve(answer(currentQuery, false)));
      expect(latest.manualControlAllowed).toBe(false);
      expect(latest.manualControlUnavailableReason).toBe('component not ready');
      expect(latest.state.draftGripperPosition).toBeNull();
      expect(latest.state.intent).toBeNull();
      expect(latest.state.preview).toBeNull();
      expect(api.execution.previewCapabilityCommand).not.toHaveBeenCalled();
      expect(api.execution.previewMotion).not.toHaveBeenCalled();
    },
  );

  it('sends a held-program cleanup to the Runner even when its offered owner differs', async () => {
    const snapshot = { executionControlStatus: statusFixture(), authoringAdmission: { admissionGeneration: 1 },
      programCycle: { programRunnerOwnerEpoch: 'runner.current', programRunId: 'program.1', cycleGeneration: 1,
        state: 'running', runRevision: 3, executionControlRevision: 1,
        runtimeControls: { executionSelections: { selectionRevision: 1, groups: [{ groupId: 'all', state: 'held' }] } } },
    } as unknown as ComposerHeadlessState;
    const send = vi.fn(() => new Promise(() => {}));
    const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
    function HandoverHarness() {
      const result = useRobotControl(true, true, false, snapshot, source);
      useEffect(() => { latest = result; }, [result]);
      return null;
    }
    vi.spyOn(window, 'setTimeout')
      .mockImplementation(((resolve: () => void) => globalThis.setTimeout(resolve, 0)) as never);
    await act(async () => root.render(createElement(HandoverHarness)));
    await act(async () => latest.setManualPanelVisible(true));
    expect(pending).toHaveLength(1);
    await act(async () => pending[0].resolve({ requestId: pending[0].intent.request_context.request_id,
      requestOwnerEpoch: pending[0].intent.request_context.request_owner_epoch, intentDigest: 'sha256:availability',
      available: true, unavailableReason: null, cleanupRequired: true,
      manualHandover: { program_run_id: 'program.1', program_runner_owner_epoch: 'runner.offered',
        claim_generation: 2, selection_revision: 1 } }));
    expect(latest.manualControlAllowed).toBe(true);
    await act(async () => { latest.setManualPanelVisible(true); latest.editJoint(0, 0.2); });
    await act(async () => { await new Promise((resolve) => globalThis.setTimeout(resolve, 20)); });
    expect(send).toHaveBeenCalledTimes(1);
    expect(send).toHaveBeenCalledWith('program_manual_handover', expect.objectContaining({
      program_run_id: 'program.1', expected_owner_epoch: 'runner.offered', expected_run_revision: 3,
    }), expect.any(String));
  });

  it.each(['runner', 'execution', 'panel-hidden'] as const)(
    'continues the confirmed pneumatic sequence after %s', async (owner) => {
      const status = pneumaticStatus('none');
      const descriptor = status.directControlGripper!.targetDescriptor;
      descriptor.closedStateRef = 'configured-closed';
      descriptor.namedPresets.push({ name: 'configured-closed', masterPosition: 0,
        coordinates: [{ sourceCoordinateName: descriptor.sourceCoordinateName, position: 0 }] });
      descriptor.pneumaticOutputs = [
        { controllableComponentId: 'valve-a', signalName: 'open', openValue: false, closeValue: true },
        { controllableComponentId: 'valve-b', signalName: 'close', openValue: true, closeValue: false },
      ];
      status.ioSignalBindings = descriptor.pneumaticOutputs.map((row, index) => ({
        programBindingId: `binding-${index}`, componentId: row.controllableComponentId,
        commandTargetId: 'io',
        signals: [{ componentId: row.controllableComponentId, signalName: row.signalName,
          valueType: 'bool', direction: 'output', transitionCapability: 'sampled_only', signalDescriptorDigest: 'digest' }],
      }));
      const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
        programCycle: { programRunnerOwnerEpoch: 'runner.1', programRunId: null, cycleGeneration: 1,
          state: 'idle_after_reset', executionControlRevision: 1 } } as unknown as ComposerHeadlessState;
      let firstStarted!: () => void;
      let releaseFirst!: () => void;
      const started = new Promise<void>((resolve) => { firstStarted = resolve; });
      const gate = new Promise<void>((resolve) => { releaseFirst = resolve; });
      const send = vi.fn(async (_kind: string, _payload: unknown, requestId: string) => (
        { result: { accepted: { request_id: requestId, operation_id: 'gripper-op' } } }));
      let writes = 0;
      // The one output write path; the first valve write waits.
      vi.spyOn(api.io, 'setOutput').mockImplementation(async () => {
        if (++writes === 1) { firstStarted(); await gate; }
        return { outcome: 'succeeded' };
      });
      const source = { sendExecutionCommand: send } as unknown as Parameters<typeof useRobotControl>[4];
      function SequenceHarness({ current }: { current: ComposerHeadlessState }) {
        const result = useRobotControl(true, false, false, current, source);
        useEffect(() => { latest = result; }, [result]);
        return null;
      }
      vi.spyOn(window, 'setTimeout')
        .mockImplementation(((resolve: () => void) => globalThis.setTimeout(resolve, 0)) as never);
      vi.mocked(api.execution.previewCapabilityCommand).mockImplementation(async (intent) => (
        gripperReceiptFixture(status, intent.request_context.request_id, 0.037)
      ));
      await act(async () => root.render(createElement(SequenceHarness, { current: snapshot })));
      await act(async () => { latest.setManualPanelVisible(true); latest.selectMode('gripper'); latest.editGripper(0.037); });
      await act(async () => { await new Promise((resolve) => globalThis.setTimeout(resolve, 20)); });
      expect(latest.state.phase).toBe('ready');
      let approval!: Promise<void>;
      await act(async () => { approval = latest.approve(); });
      await started;
      const successor = structuredClone(snapshot);
      if (owner === 'runner') successor.programCycle.programRunnerOwnerEpoch = 'runner.2';
      else if (owner === 'execution') successor.executionControlStatus!.ownerEpoch = 'execution.2';
      if (owner === 'panel-hidden') await act(async () => latest.setManualPanelVisible(false));
      else await act(async () => root.render(createElement(SequenceHarness, { current: successor })));
      releaseFirst();
      await act(async () => { await approval; });
      // Two valve writes, then the gripper command; no automatic resend.
      expect(api.io.setOutput).toHaveBeenNthCalledWith(1, 'valve-a/open', false);
      expect(api.io.setOutput).toHaveBeenNthCalledWith(2, 'valve-b/close', true);
      expect(api.io.setOutput).toHaveBeenCalledTimes(2);
      expect(send).toHaveBeenCalledTimes(1);
    },
  );
});



it('shows the typed Cartesian diagnostic reason only for the owner marker', () => {
  const detail = { code: 'cartesian_incomplete', message: 'Cartesian failed', origin: null,
    details: { cartesian_collision_disabled_diagnostic: 'failed', planner_error: 'joint limit' } };
  expect(cartesianDiagnosticRejection(detail)).toBe(
    'The planner found no straight path, even ignoring collisions: joint limit.',
  );
  expect(cartesianDiagnosticRejection({ ...detail, code: 'planning_timeout', details: { reason: 'planning_timeout' } })).toBeNull();
  expect(cartesianDiagnosticRejection({ ...detail, code: 'worker_failed', details: { reason: 'worker_failed' } })).toBeNull();
});

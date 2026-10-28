import { act, createElement, useEffect } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import { api, ApiError } from '../api/client';

import type { RuntimeScaleControlStatus } from '../types/robotControl';
import {
  acceptedRuntimeScaleStatus,
  mergeRuntimeScaleStatus,
  previewTargetRejection,
  runtimeScaleControlScopeFence,
  runtimeScaleFlushDelay,
  manualAvailabilityScopeFence,
  buildManualAvailabilityIntent,
  useRobotControl,
} from './useRobotControl';
import { statusFixture } from '../testFixtures/robotControl';
import type { ComposerHeadlessState } from '../types/headless';
import { buildPreviewIntent, initialRobotControlState, robotControlReducer } from '../state/robotControl';

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
    componentRoleId: 'robot',
    targetRoleId: 'arm',
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
          { componentRoleId: 'robot', targetRoleId: 'arm' },
          { componentRoleId: 'robot', targetRoleId: 'gripper' },
        ],
      },
      groupMembers: [
        {
          componentRoleId: 'robot', targetRoleId: 'arm',
          controlDomainId: 'domain-arm', controlDomainRevision: 'rev-arm',
          controlBindingId: 'binding-arm', controlBindingRevision: 'binding-rev-arm',
          scalingDomainId: 'scale-arm', activeControlConfigurationId: 'config-arm',
          activeControlConfigurationGeneration: 1,
        },
        {
          componentRoleId: 'robot', targetRoleId: 'gripper',
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
        { componentRoleId: 'robot', targetRoleId: 'arm' },
        { componentRoleId: 'robot', targetRoleId: 'camera' },
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
          { componentRoleId: 'a', targetRoleId: 'b\u0000c' },
          { componentRoleId: 'a\u0000b', targetRoleId: 'c' },
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
        member.targetRoleId === 'arm'
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
  it('disables mapped pneumatic manual control while a Program is running or held', async () => {
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
    expect(latest.manualControlAllowed).toBe(false);
    expect(latest.manualControlUnavailableReason).toContain('idle after Reset');
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
});

describe('Recover cell single press', () => {
  let root: import('react-dom/client').Root;
  let latest: ReturnType<typeof useRobotControl>;
  function Harness({ snapshot }: { snapshot: ComposerHeadlessState }) {
    const result = useRobotControl(true, true, true, snapshot);
    useEffect(() => { latest = result; }, [result]);
    return null;
  }
  beforeEach(async () => {
    const document = Object.assign(new EventTarget(), { nodeType: 9, activeElement: null, defaultView: {} });
    const makeNode = (tag: string) => Object.assign(new EventTarget(), {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {},
      appendChild: vi.fn(), removeChild: vi.fn(), insertBefore: vi.fn(),
      setAttribute: vi.fn(), removeAttribute: vi.fn(),
    });
    Object.assign(document, { createElement: makeNode, documentElement: makeNode('html') });
    document.defaultView = Object.assign(new EventTarget(), { document, HTMLIFrameElement: class {},
      setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout });
    vi.stubGlobal('window', document.defaultView);
    vi.stubGlobal('document', document);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    const { createRoot } = await import('react-dom/client');
    root = createRoot(makeNode('div') as unknown as Element);
  });
  afterEach(async () => {
    await act(async () => root.unmount());
    vi.restoreAllMocks();
    vi.unstubAllGlobals();
  });

  it('sends one Headless cell-recover per press and shows its typed reason', async () => {
    const status = statusFixture();
    status.cellStopAction.fault = {
      latchId: 'stop-fault.1', latchGeneration: 1, state: 'latched',
      trigger: 'physical_rest_unproven', observedTriggers: ['physical_rest_unproven'],
      recoveryStatus: 'waiting_for_observation', reasonCode: 'stop_fault_recovery_waiting_for_observation',
      certificate: null,
    } as unknown as NonNullable<typeof status.cellStopAction.fault>;
    const snapshot = { executionControlStatus: status, authoringAdmission: { admissionGeneration: 1 },
      programCycle: { programRunnerOwnerEpoch: 'runner.1', programRunId: null, cycleGeneration: 1,
        state: 'idle_after_reset', executionControlRevision: 1,
        runtimeControls: { executionSelections: { selectionRevision: 1, groups: [] } } },
    } as unknown as ComposerHeadlessState;
    const cellRecover = vi.spyOn(api.execution, 'cellRecover')
      .mockResolvedValueOnce({
        outcome: 'not_recovered',
        acknowledgedIncidents: [{ controllableComponentId: 'arm', incidentId: 'incident.1', causeClass: 'emergency_stop' }],
        latchCleared: false,
        recoveredComponents: [],
        reason: { code: 'incident_open', message: 'm',
          details: { controllable_component_id: 'arm', cause_class: 'emergency_stop' } },
      })
      .mockResolvedValueOnce({ outcome: 'done', acknowledgedIncidents: [], latchCleared: true, recoveredComponents: [], reason: null });
    // The browser no longer chains the owners itself.
    const submitLifecycle = vi.spyOn(api.cclc, 'submitLifecycle');
    const stopFault = vi.spyOn(api.execution, 'stopFault');
    vi.spyOn(api.execution, 'manualAvailability').mockReturnValue(new Promise(() => {}));
    await act(async () => root.render(createElement(Harness, { snapshot })));

    await act(async () => { await latest.recoverStopFault(); });
    expect(cellRecover).toHaveBeenCalledTimes(1);
    expect(latest.stopFaultRecoveryPending).toBe(false);
    expect(latest.stopFaultRecoveryError).toContain('The arm incident is open again (emergency stop)');

    await act(async () => { await latest.recoverStopFault(); });
    expect(cellRecover).toHaveBeenCalledTimes(2);
    expect(cellRecover.mock.calls[0]![0]).not.toBe(cellRecover.mock.calls[1]![0]);
    expect(latest.stopFaultRecoveryError).toBeNull();
    expect(submitLifecycle).not.toHaveBeenCalled();
    expect(stopFault).not.toHaveBeenCalled();
  });
});

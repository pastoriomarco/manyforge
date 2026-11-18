import { StrictMode, act, useLayoutEffect } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type {
  AppliedProgramSelection,
  CommandEnd,
  CommandRow,
  PersistentKeyValues,
  ProgramRuntimeControlProjection,
  RunnerCommandRowKind,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { RuntimeScaleControlSelector, RuntimeScaleControlStatus } from '../types/robotControl';
import { heldRun } from '../testFixtures/heldRun';
import { ASSISTANT_TURN_IN_PROGRESS, useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import { CommandRefusalError } from '../state/commandRefusal';

const apiMock = vi.hoisted(() => ({
  programs: { currentScenario: vi.fn(), applied: vi.fn(), editPersistentValue: vi.fn() },
  deploymentWorkspace: { get: vi.fn() },
  execution: { identity: vi.fn(), runtimeScaleControl: vi.fn() },
}));
vi.mock('../api/client', async (original) => ({
  ...await original<typeof import('../api/client')>(), api: apiMock,
}));

// Both controllers run as App shares them. Only the owner transports and a
// minimal React DOM host are substituted; sequence/admission/ref timing is real.
function domHost() {
  const document: Record<string, unknown> = { nodeType: 9, activeElement: null,
    addEventListener: () => undefined, removeEventListener: () => undefined };
  const node = (tag: string): Record<string, unknown> => ({
    nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
    namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document,
    style: {}, addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  document.createElement = node;
  document.documentElement = node('html');
  const window = { document, location: { href: 'http://composer.test/' },
    HTMLIFrameElement: class {}, setTimeout, clearTimeout };
  document.defaultView = window;
  vi.stubGlobal('document', document); vi.stubGlobal('window', window);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  return node('div') as unknown as Element;
}
function facts(): { snapshot: ComposerHeadlessState; workspace: ProgramWorkspaceResponse } {
  const authoringWorkspace = { workspaceEpoch: 'workspace.1', workspaceRevision: 1,
    programDraftId: 'draft.1', programDraftRevision: 1,
    deploymentDraftId: 'deployment.1', deploymentDraftRevision: 1 };
  return {
    workspace: { authoringWorkspace, draft: { draftId: 'draft.1', draftRevision: 1,
      programLoaded: true, programRevision: 'program.revision' } } as ProgramWorkspaceResponse,
    snapshot: { applicationStartId: '3f9a000000000001', authoringWorkspace,
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
      componentOverview: { basis: { appliedProgramGeneration: 1 } },
      runtime: { activeConfiguration: { retainedBundleId: 'bundle', graphRevision: 'graph' },
        sections: { manager: { sourceOwnerId: 'manager.1', payload: { active: {
          retained_bundle_id: 'bundle', graph_revision: 'graph' } } } } },
      executionControlStatus: null,
      programCycle: { cycleGeneration: 1,
        state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
        terminalOrSettlementReference: null, runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null, executionControlRevision: 1,
        causalRequestKey: null,
        causalDisplay: null,
        currentCommandKind: null, runnerCommands: { inProgress: null, ended: [] }, failure: null, resetPhase: null, resetOptions: null,
        resetRobotPosition: { available: false, reason: 'unsupported' },
        run: null, runtimeControls: null,
        allowedCommands: [{ commandKind: 'program_run', allowed: true,
          disabledReason: null, semanticTarget: {} }] } } as unknown as ComposerHeadlessState,
  };
}
let cleanup: (() => Promise<void>) | null = null;
afterEach(async () => {
  await cleanup?.(); cleanup = null; vi.unstubAllGlobals(); vi.useRealTimers();
});
async function mount(resumed = false, assistantTurnRunning = false) {
  vi.useFakeTimers();
  for (const section of Object.values(apiMock)) for (const method of Object.values(section)) method.mockReset();
  const host = domHost();
  let { snapshot, workspace } = facts();
  if (resumed) snapshot = { ...snapshot, programCycle: { ...snapshot.programCycle,
    state: 'running', programRunId: 1, run: { ...heldRun(), state: 'halted' } } };
  apiMock.programs.currentScenario.mockResolvedValue({ ready: false,
    unavailableReasons: ['scenario_not_ready'], currentScenario: null });
  apiMock.programs.applied.mockResolvedValue({ scenarioId: 'scenario', sourceDocumentDigest: 'source',
    programRevision: 'program.revision', retainedBundleId: 'bundle', graphRevision: 'graph',
    recordDigest: 'record.1',
    sourceGeneration: 1 } as AppliedProgramSelection);
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: { scenarioId: 'scenario',
    deploymentRevision: 'deployment.revision', activationOrder: {
      componentIds: ['robot.a'], retainedBundleId: 'bundle', graphRevision: 'graph' } } });
  apiMock.execution.identity.mockResolvedValue({ applicationStartId: '3f9a000000000001', graphRevision: 'graph',
    retainedBundleId: 'bundle', activePublicationGeneration: 1, controlManagementMode: 'component_lifecycle' });
  const admissionsAtSend: number[] = [];
  const send = vi.fn(async (_kind: string, _payload: unknown) => {
    void _kind; void _payload;
    admissionsAtSend.push(snapshot.authoringAdmission.admissionGeneration);
    return { disposition: 'received', command: null };
  });
  const source = { sendExecutionCommand: send } as unknown as HeadlessStateSource;
  let runtime!: ProgramRuntimeController;
  function Harness() {
    const value = useProgramRuntime(true, snapshot, workspace, source, false, assistantTurnRunning);
    useLayoutEffect(() => { runtime = value; }, [value]);
    return null;
  }
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(host);
  const render = async () => { await act(async () => { root.render(<StrictMode><Harness /></StrictMode>); }); };
  await render();
  const unmount = async () => { await act(async () => { root.unmount(); }); };
  cleanup = unmount;
  expect(runtime.state.loading).toBe(false);
  const observe = async (change: (value: ReturnType<typeof facts>) => void) => {
    const next = { snapshot: structuredClone(snapshot), workspace: structuredClone(workspace) };
    change(next); snapshot = next.snapshot; workspace = next.workspace; await render();
  };
  return { get runtime() { return runtime; }, send, admissionsAtSend, observe, unmount };
}

it('Execute sends one ordinary run request and shows an authoritative refusal', async () => {
  const test = await mount();
  test.send.mockRejectedValueOnce(new Error('Simulator unavailable'));
  await act(async () => { await test.runtime.run(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[0]).toBe('program_run');
  expect(test.runtime.state.error).toContain('Simulator unavailable');
});

it('refuses Execute, Run and Resume while this page\'s assistant request runs', async () => {
  const idle = await mount();
  expect(idle.runtime.composerRefusalReason).toBeNull();
  await idle.unmount(); cleanup = null;
  const test = await mount(false, true);
  expect(test.runtime.composerRefusalReason).toBe(ASSISTANT_TURN_IN_PROGRESS);
});

it('shows the Composer\'s typed refusal of an Execute during an assistant request', async () => {
  const test = await mount();
  const refusal = {
    code: 'execution_assistant_turn_in_progress',
    message: 'an assistant request is in progress',
    details: null,
    origin: null,
  };
  test.send.mockRejectedValueOnce(new CommandRefusalError(refusal));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.state.errorRefusal).toEqual(refusal);
  expect(test.runtime.state.error).toContain('an assistant request is in progress');
});

it('retains the owner refusal code for a holding rule until dismissed', async () => {
  const test = await mount();
  const refusal = { code: 'rule_holds', message: 'held', details: { rule_id: 'door' }, origin: null };
  test.send.mockRejectedValueOnce(new CommandRefusalError(refusal));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.state.error).toContain('Global interrupt rule door holds this scope');
  expect(test.runtime.state.errorRefusal).toEqual(refusal);
  await act(async () => { test.runtime.dismissError(); });
  expect(test.runtime.state.error).toBeNull();
  expect(test.runtime.state.errorRefusal).toBeNull();
});

it('keeps an exact selection refusal visible and projects the next owner snapshot without resending', async () => {
  const test = await mount();
  const run = heldRun();
  const makeControls = (
    allowed: boolean,
    reason: string | null,
  ): ProgramRuntimeControlProjection => ({
    schemaVersion: 'manyforge.program_runtime_control_projection.v10',
    ownerId: 'program-runner.development-workbench',
    programRunId: run.capture.programRunId,
    runRevision: run.runRevision,
    runtimeControls: { runtimeControlRevision: 1 } as never,
    recoveryOffer: null,
    recoveryHolds: [],
    executionSelections: {
      holdRevision: 7,
      motionHeld: true,
      groups: [{
        groupId: 'all', nodeCount: 1, heldNodeCount: 1, state: 'held',
        stopAllowed: true,
        startAllowed: allowed, startUnavailableReason: reason,
        resumeAllowed: allowed, resumeUnavailableReason: reason,
        runtimeScaleSelector: null, runtimeScaleUnavailableReason: null,
        resourceIds: [],
      }],
    },
  } as unknown as ProgramRuntimeControlProjection);
  const runningControls = makeControls(true, null);
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      state: 'running',
      programRunId: run.capture.programRunId,
      cycleClaimGeneration: 4,
      runRevision: run.runRevision,
      nodeStatusRevision: 1,
      runtimeControlRevision: 1,
      run,
      runtimeControls: runningControls,
    };
  });
  const currentScenarioReads = apiMock.programs.currentScenario.mock.calls.length;
  const appliedReads = apiMock.programs.applied.mock.calls.length;
  const refusal = 'Cell motion admission is unreconciled; Program Run or Resume remains blocked';
  test.send.mockRejectedValueOnce(new Error(refusal));

  await act(async () => {
    expect(await test.runtime.resumeMotion('all')).toBe(false);
  });

  expect(test.send).toHaveBeenCalledTimes(1);
  // Resume names its run and selection; tree ticks move only the run revision.
  const payload = test.send.mock.calls[0]?.[1] as Record<string, unknown>;
  expect(payload).toMatchObject({ program_run_id: run.capture.programRunId,
    expected_hold_revision: 7 });
  expect(payload).not.toHaveProperty('expected_run_revision');
  for (const member of ['request_id', 'request_owner_epoch', 'correlation_id', 'expected_owner_epoch']) {
    expect(payload).not.toHaveProperty(member);
  }
  expect(apiMock.programs.currentScenario).toHaveBeenCalledTimes(currentScenarioReads);
  expect(apiMock.programs.applied).toHaveBeenCalledTimes(appliedReads);
  expect(test.runtime.state.error).toContain(refusal);

  const blockedControls = makeControls(false, 'cell_motion_admission_unreconciled');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      runtimeControls: blockedControls,
    };
  });
  const group = test.runtime.state.controls?.executionSelections.groups[0];
  expect(group?.resumeAllowed).toBe(false);
  expect(group?.resumeUnavailableReason).toBe('cell_motion_admission_unreconciled');
  expect(test.send).toHaveBeenCalledTimes(1);
});

it('Reset names the send-time cycle', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  test.send.mockResolvedValueOnce({
    disposition: 'received', controlSessionNumber: 4, clientSequence: '7', command: null,
  } as never);
  let result;
  await act(async () => {
    result = await test.runtime.reset({ expected: { programRunId: null, cycleGeneration: 1 } });
  });
  expect(test.send).toHaveBeenCalledExactlyOnceWith('program_reset', expect.objectContaining({
    program_run_id: null, expected_cycle_generation: 1,
  }));
  expect(test.send.mock.calls[0]?.[1]).not.toHaveProperty('expected_owner_epoch');
  // The receipt's session and client sequence name the sent Reset.
  expect(result).toMatchObject({ status: 'sent', reset: {
    command: { controlSessionNumber: 4, clientSequence: '7' },
    cycleGeneration: 1,
  } });
});

it('plain Reset defaults both effects off and never resends unconfirmed delivery', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  test.send.mockRejectedValueOnce(new CommandDeliveryUnconfirmedError('reply lost', {
    command: { controlSessionNumber: 4, clientSequence: '9' },
  }));
  let result;
  await act(async () => { result = await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[1]).toMatchObject({
    reload_initial_scene: false, reset_robot_position: false,
  });
  expect(result).toMatchObject({ status: 'unconfirmed', reset: {
    command: { controlSessionNumber: 4, clientSequence: '9' }, programRunId: null, cycleGeneration: 1,
  } });
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(test.send).toHaveBeenCalledTimes(1);
});

// The synthesized failed-run notice: once per run revision, cleared by Reset
// reaching idle or by another run, never rebuilt after Dismiss or a command.
function failRun(value: ReturnType<typeof facts>, programRunId = 1, runRevision = 4) {
  value.snapshot.programCycle = { ...value.snapshot.programCycle,
    state: 'terminal_awaiting_reset', programRunId, runRevision,
    allowedCommands: [{ commandKind: 'program_reset', allowed: true, disabledReason: null, semanticTarget: {} }],
    run: { ...heldRun(), state: 'failed', runRevision, treeStatus: 'failure',
      nodeStatuses: { move: 'failure' }, activeOperationCount: 0, reason: null,
      capture: { ...heldRun().capture, programRunId } } };
}
function cycleState(
  value: ReturnType<typeof facts>,
  state: ComposerHeadlessState['programCycle']['state'],
) {
  value.snapshot.programCycle.state = state;
  if (state === 'idle_after_reset') {
    value.snapshot.programCycle = { ...value.snapshot.programCycle,
      programRunId: null, runRevision: null, run: null };
  }
}

it('keeps a dismissed failed-run notice dismissed for that run revision', async () => {
  const test = await mount();
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await act(async () => { test.runtime.dismissError(); });
  expect(test.runtime.state.error).toBeNull();
  await test.observe(({ snapshot }) => { snapshot.programCycle.executionControlRevision = 2; });
  await test.observe(({ snapshot }) => { snapshot.programCycle.executionControlRevision = 3; });
  expect(test.runtime.state.error).toBeNull();
  // A new revision of the failed run is a new notice.
  await test.observe((value) => failRun(value, 1, 5));
  expect(test.runtime.state.error).toContain('3f9a/run1: failed, revision 5');
});

it('clears the failed-run notice when Reset reaches idle and never rebuilds it during Reset', async () => {
  const test = await mount();
  // A Reset observed from the owner alone clears the notice at idle.
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await test.observe((value) => cycleState(value, 'resetting'));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  await test.observe((value) => cycleState(value, 'idle_after_reset'));
  expect(test.runtime.state.error).toBeNull();
  // This client's Reset supersedes it at once; the resetting window keeps the
  // failed run but does not rebuild the notice.
  await test.observe((value) => failRun(value, 2));
  expect(test.runtime.state.error).toContain('3f9a/run2: failed');
  await act(async () => { await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledWith('program_reset', expect.anything());
  expect(test.runtime.state.error).toBeNull();
  await test.observe((value) => cycleState(value, 'resetting'));
  expect(test.runtime.state.error).toBeNull();
  await test.observe((value) => cycleState(value, 'idle_after_reset'));
  expect(test.runtime.state.error).toBeNull();
});

it('clears the failed-run notice when a new Run starts', async () => {
  const test = await mount();
  await test.observe((value) => failRun(value));
  expect(test.runtime.state.error).toContain('Program execution failed.');
  // Observed without the idle snapshot in between: the next run replaces it.
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = { ...snapshot.programCycle, state: 'running', programRunId: 2,
      runRevision: 1, run: { ...heldRun(), state: 'running', runRevision: 1, nodeStatuses: {},
        capture: { ...heldRun().capture, programRunId: 2 } } };
  });
  expect(test.runtime.state.error).toBeNull();
});

// One backend scaling setting, shared by every row that targets it (as "All
// groups" and a named group do for a single robot).
const scaleSelector: RuntimeScaleControlSelector = { kind: 'singular',
  programBindingId: `sha256:${'c'.repeat(64)}`, componentId: 'robot', commandTargetId: 'arm' };
async function mountScaleRows(groupIds: string[]) {
  const test = await mount();
  // OD-72: the owner applies the latest value whatever generation it names;
  // `refusal` models a refusal that remains (e.g. a frozen control).
  const backend = { generation: 2, scale: 0.5, refusal: null as string | null };
  const status = (): RuntimeScaleControlStatus => ({ selector: scaleSelector,
    controlGeneration: backend.generation, requestedScale: backend.scale,
    supported: true, frozen: false, controlDomainId: 'domain-1',
    controlDomainRevision: `sha256:${'d'.repeat(64)}`, controlBindingId: 'binding-1',
    controlBindingRevision: `sha256:${'e'.repeat(64)}`, scalingDomainId: 'scaling-1',
    activeControlConfigurationId: 'active-1', activeControlConfigurationGeneration: 1,
    activeOperationId: null, activeOperationRevision: null,
    activeOperationScaleGeneration: null, effectiveScale: null,
    applicationState: null, reason: null });
  apiMock.execution.runtimeScaleControl.mockImplementation(async () => status());
  test.send.mockImplementation(async (kind: string, payload: unknown) => {
    const command = payload as { selector: Record<string, unknown>;
      expected_control_generation: number; scale: number };
    expect(kind).toBe('runtime_scale');
    if (backend.refusal !== null) {
      throw new CommandRefusalError({ code: backend.refusal,
        message: 'refused', details: {}, origin: null });
    }
    backend.generation += 1; backend.scale = command.scale;
    return { disposition: 'received', command: null, result: { ok: true, api_version: 1,
      command: 'set_runtime_scale_control', accepted: { accepted_at_ns: 1,
        active_operation_id: null, active_operation_revision: null,
        active_operation_scale_generation: null, control_generation: backend.generation,
        replay_status: 'new', request_key: 'a'.repeat(32), requested_scale: command.scale,
        selector: command.selector } } } as never;
  });
  const run = heldRun();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = { ...snapshot.programCycle, state: 'running',
      programRunId: run.capture.programRunId, runRevision: run.runRevision, run,
      runtimeControls: { schemaVersion: 'manyforge.program_runtime_control_projection.v10',
        ownerId: 'program-runner.development-workbench',
        programRunId: run.capture.programRunId, runRevision: run.runRevision,
        runtimeControls: { runtimeControlRevision: 1 }, recoveryOffer: null, recoveryHolds: [],
        executionSelections: { holdRevision: 1, motionHeld: false,
          groups: groupIds.map((groupId) => ({ groupId, nodeCount: 1, heldNodeCount: 0,
            state: 'running', stopAllowed: true, startAllowed: false,
            startUnavailableReason: null, resumeAllowed: false, resumeUnavailableReason: null,
            runtimeScaleSelector: scaleSelector, runtimeScaleUnavailableReason: null,
            resourceIds: [] })) } } as unknown as ProgramRuntimeControlProjection };
  });
  for (const groupId of groupIds) {
    expect(test.runtime.groupRuntimeScales[groupId]).toMatchObject({ draft: 0.5, pending: false });
  }
  return Object.assign(test, { backend });
}

it('sends the final value of a fast drag after the first value is accepted', async () => {
  const test = await mountScaleRows(['all']);
  await act(async () => {
    // First value goes out at once; later ones queue behind it.
    test.runtime.setGroupRuntimeScale('all', 0.6);
    test.runtime.setGroupRuntimeScale('all', 0.7);
    test.runtime.setGroupRuntimeScale('all', 0.8);
  });
  // The acceptance refreshed every row; the queued value must survive it.
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.runtime.groupRuntimeScales.all?.pending).toBe(true);
  await act(async () => { test.runtime.commitGroupRuntimeScale(); });
  expect(test.send).toHaveBeenCalledTimes(2);
  expect(test.send.mock.calls[1]?.[1]).toMatchObject({
    expected_control_generation: 3, scale: 0.8 });
  expect(test.backend).toMatchObject({ generation: 4, scale: 0.8 });
  expect(test.runtime.groupRuntimeScales.all).toMatchObject({ draft: 0.8, pending: false });
});

it('reverts a refused value and re-reads every row after a refusal', async () => {
  const test = await mountScaleRows(['all', 'carry']);
  // The shared setting moved without this client re-reading it, and the
  // control is now refused (frozen while a Stop is active).
  test.backend.generation = 5; test.backend.scale = 0.2;
  test.backend.refusal = 'runtime_scale_control_unavailable';
  const reads = apiMock.execution.runtimeScaleControl.mock.calls.length;
  await act(async () => { test.runtime.setGroupRuntimeScale('all', 0.9); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(apiMock.execution.runtimeScaleControl.mock.calls.length).toBeGreaterThan(reads);
  for (const groupId of ['all', 'carry']) {
    expect(test.runtime.groupRuntimeScales[groupId]).toMatchObject({
      draft: 0.2, pending: false, status: { controlGeneration: 5 } });
  }
});

it('sends a motion Stop, Stop all and Halt while another Stop or a Reset is still pending', async () => {
  const test = await mount();
  const run = heldRun();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      state: 'running',
      programRunId: run.capture.programRunId,
      runRevision: run.runRevision,
      run,
      allowedCommands: [{ commandKind: 'program_reset', allowed: true,
        disabledReason: null, semanticTarget: {} }],
    };
  });
  // No receipt arrives: every command stays pending.
  test.send.mockImplementation(() => new Promise<never>(() => undefined));
  await act(async () => {
    void test.runtime.reset({ expected: { programRunId: run.capture.programRunId, cycleGeneration: 1 } });
  });
  expect(test.runtime.state.pending).toBe('reset');
  await act(async () => { void test.runtime.stopMotion(['all']); });
  // A repeated press sends another Stop; the owner joins it.
  await act(async () => { void test.runtime.stopMotion(['all']); });
  await act(async () => { void test.runtime.stopAll(); });
  await act(async () => { void test.runtime.halt(); });
  await act(async () => { void test.runtime.halt(); });
  expect(test.send.mock.calls.map((call) => call[0])).toEqual([
    'program_reset', 'program_motion_stop', 'program_motion_stop',
    'stop_all', 'halt', 'halt',
  ]);
  // Commands name the run by its number; the control session and client
  // sequence name the request (no request id, owner epoch or correlation).
  expect(test.send.mock.calls[1]?.[1]).toEqual({
    source_label: 'operator_ui', program_run_id: run.capture.programRunId,
    group_ids: ['all'], reason: 'operator motion-stop',
  });
  // Stop all and Halt name no run: Headless adds the request context.
  expect(test.send.mock.calls[3]?.[1]).toEqual({});
  expect(test.send.mock.calls[4]?.[1]).toEqual({});
  expect(test.send.mock.calls.every((call) => call.length === 2)).toBe(true);
  // Run and Resume keep their gates while a Stop is pending.
  await act(async () => { expect(await test.runtime.resumeMotion('all')).toBe(false); });
  expect(test.send).toHaveBeenCalledTimes(6);
});

it('sends Stop all and Halt with no Program loaded; the receipt ends their wait', async () => {
  const test = await mount();
  expect(test.runtime.state.run).toBeNull();
  await act(async () => { expect(await test.runtime.stopAll()).toBe(true); });
  expect(test.runtime.state.pending).toBeNull();
  await act(async () => { expect(await test.runtime.halt()).toBe(true); });
  expect(test.runtime.state.pending).toBeNull();
  expect(test.send.mock.calls).toEqual([['stop_all', {}], ['halt', {}]]);
});

// The Halt button's two steps (ProgramRuntimePanel.test.tsx 'Halt': the press
// calls stopAll() at once, the confirmation, or no confirmation, calls halt())
// on the wire: stop_all goes out before any reply, then halt; neither waits.
it('Halt sends stop_all at once, then halt', async () => {
  const test = await mount();
  const run = heldRun();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle, state: 'running', programRunId: run.capture.programRunId,
      runRevision: run.runRevision, run: { ...run, state: 'running' },
    };
  });
  test.send.mockImplementation(() => new Promise<never>(() => undefined));
  await act(async () => { void test.runtime.stopAll(); });
  expect(test.send.mock.calls).toEqual([['stop_all', {}]]);
  expect(test.runtime.state.pending).toBe('stop-all');
  await act(async () => { void test.runtime.halt(); });
  expect(test.send.mock.calls).toEqual([['stop_all', {}], ['halt', {}]]);
  // A live run shows the Halt: the wait ends when it is halted.
  expect(test.runtime.state.pending).toBe('halt');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      run: { ...run, state: 'halted', runRevision: run.runRevision + 1 },
    };
  });
  expect(test.runtime.state.pending).toBeNull();
});

it('repeats a motion Stop and a Halt while the run is halting or stopping', async () => {
  const test = await mount();
  const run = heldRun();
  for (const [step, state] of (['halting', 'stopping'] as const).entries()) {
    await test.observe(({ snapshot }) => {
      snapshot.programCycle = {
        ...snapshot.programCycle,
        state: 'running',
        programRunId: run.capture.programRunId,
        runRevision: run.runRevision + step,
        run: { ...run, state, runRevision: run.runRevision + step },
      };
    });
    expect(test.runtime.state.run?.state).toBe(state);
    test.send.mockClear();
    await act(async () => { expect(await test.runtime.stopMotion(['all'])).toBe(true); });
    await act(async () => { await test.runtime.halt(); });
    // A repeated Stop is accepted; on a run already halting or ending it has
    // no further effect.
    expect(test.send.mock.calls.map((call) => call[0]))
      .toEqual(['program_motion_stop', 'halt']);
    // Run and Resume stay closed while the run is stopping.
    await act(async () => { expect(await test.runtime.resumeMotion('all')).toBe(false); });
    expect(test.send).toHaveBeenCalledTimes(2);
  }
});

it('clears the Execute pending flag when the resumed run is held again at once', async () => {
  // Execute after Halt resumes the run, whose first tree tick re-holds the
  // node the motion Stop held: the client may never see `running`.
  const test = await mount(true);
  expect(test.runtime.state.run?.state).toBe('halted');
  await act(async () => { await test.runtime.resumeHalt(); });
  expect(test.send.mock.calls).toEqual([['program_resume_halt', {
    source_label: 'operator_ui', program_run_id: 1, expected_run_revision: heldRun().runRevision,
    reason: 'operator Execute after Halt',
  }]]);
  expect(test.runtime.state.pending).toBe('resume-halt');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle,
      run: { ...heldRun(), state: 'intervention_required', runRevision: 6 },
    };
  });
  expect(test.runtime.state.run?.state).toBe('intervention_required');
  expect(test.runtime.state.pending).toBeNull();
});

it('clears a Run or Resume once the owner applied a newer selection, whatever the group shows', async () => {
  const test = await mount();
  const run = heldRun();
  const controls = (holdRevision: number, state: string, heldNodeCount: number) => ({
    schemaVersion: 'manyforge.program_runtime_control_projection.v10',
    ownerId: 'program-runner.development-workbench',
    programRunId: run.capture.programRunId,
    runRevision: run.runRevision,
    runtimeControls: { runtimeControlRevision: 1 } as never,
    recoveryOffer: null,
    recoveryHolds: [],
    executionSelections: {
      holdRevision,
      motionHeld: state === 'held',
      groups: [{
        groupId: 'all', nodeCount: 2, heldNodeCount, state,
        stopAllowed: state !== 'held', startAllowed: true, startUnavailableReason: null,
        resumeAllowed: state === 'held', resumeUnavailableReason: null,
        runtimeScaleSelector: null, runtimeScaleUnavailableReason: null, resourceIds: [],
      }],
    },
  }) as unknown as ProgramRuntimeControlProjection;
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle, state: 'running', programRunId: run.capture.programRunId,
      runRevision: run.runRevision, run, runtimeControls: controls(3, 'held', 1),
    };
  });
  test.send.mockImplementation(() => new Promise<never>(() => undefined));
  await act(async () => { void test.runtime.resumeMotion('all'); });
  expect(test.runtime.state.selectionPending).toHaveLength(1);
  // Resumed, its node finished and the next one waits: the group never
  // shows `running` in a projection, but the hold revision moved on.
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = {
      ...snapshot.programCycle, runtimeControls: controls(4, 'ready', 0),
    };
  });
  expect(test.runtime.state.selectionPending).toEqual([]);
});

// OD-214: a runner command answers at acceptance and publishes its step and
// its end; the panel shows the step while it runs and, on an end other than
// succeeded, the catalogue text where the command's failure is shown.
const ROW_KEY = '0f1e2d3c4b5a69788796a5b4c3d2e1f0';
function commandRow(
  kind: RunnerCommandRowKind, step: string | null, end: CommandEnd | null = null,
): CommandRow<RunnerCommandRowKind> {
  return { kind, requestKey: ROW_KEY, step, acceptedAtNs: '1763112345678901234', end };
}
function rowEnd(outcome: CommandEnd['outcome'], code: string | null): CommandEnd {
  return { outcome, code, details: null, finishedAtNs: '1763112345700000000' };
}
const accepted = (row: CommandRow) => ({ disposition: 'received', requestKey: ROW_KEY, command: row });
const BUSY = 'Not available while another Program command is running';

it('shows Execute and its step while it runs, and nothing once it ended succeeded', async () => {
  const test = await mount();
  test.send.mockResolvedValueOnce(accepted(commandRow('start', 'admitting')) as never);
  let settled = false;
  await act(async () => { void test.runtime.run().then(() => { settled = true; }); });
  expect(test.runtime.state.pending).toBe('run');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = { inProgress: commandRow('start', 'claiming'), ended: [] };
  });
  expect(test.runtime.runnerBusyReason).toBe(`${BUSY}: Execute, claiming the cell.`);
  expect(settled).toBe(false);
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = {
      inProgress: null, ended: [commandRow('start', 'starting_run', rowEnd('succeeded', null))],
    };
  });
  expect(settled).toBe(true);
  expect(test.runtime.runnerBusyReason).toBeNull();
  expect(test.runtime.state.error).toBeNull();
});

it('names a command that is still starting', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = {
      inProgress: { ...commandRow('reset', null), acceptedAtNs: null }, ended: [],
    };
  });
  expect(test.runtime.runnerBusyReason).toBe(`${BUSY}: Reset, starting.`);
});

it.each([
  ['refused', 'rule_holds', 'Execute: A rule holds this motion.'],
  ['failed', 'program_run_started_then_failed', 'Execute: Run failed. The Program run started and then failed. Reset is required.'],
  ['unknown', 'program_cycle_reconciliation_required', 'Execute: Program outcome not confirmed.'],
] as const)('shows the catalogue text of an Execute that ended %s, not its code', async (outcome, code, text) => {
  const test = await mount();
  const kind: RunnerCommandRowKind = 'start';
  test.send.mockResolvedValueOnce(accepted(commandRow(kind, 'admitting')) as never);
  let done!: Promise<void>;
  await act(async () => { done = test.runtime.run(); });
  // The run already moved the cycle on; the row's end is still shown.
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = {
      inProgress: null, ended: [commandRow(kind, 'admitting', rowEnd(outcome, code))],
    };
  });
  await act(async () => { await done; });
  expect(test.runtime.state.error).toContain(text);
  expect(test.runtime.state.error).not.toContain(code);
  expect(test.runtime.state.errorRefusal?.code).toBe(code);
  expect(test.runtime.state.pending).toBeNull();
});

it('shows the end of a Reset that failed after it was accepted', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  test.send.mockResolvedValueOnce({
    ...accepted(commandRow('reset', 'admitting')), controlSessionNumber: 4, clientSequence: '7',
  } as never);
  let result;
  await act(async () => { result = await test.runtime.reset(); });
  // Reset answers at acceptance; its callers follow the cycle.
  expect(result).toMatchObject({ status: 'sent' });
  expect(test.runtime.state.error).toBeNull();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = { inProgress: null,
      ended: [commandRow('reset', 'settling', rowEnd('failed', 'program_cycle_recovery_required'))] };
  });
  expect(test.runtime.state.error).toMatch(/^Reset: /);
  expect(test.runtime.state.error).not.toContain('program_cycle_recovery_required');
});

it('refuses a second command while one is in progress with the runner\'s typed refusal', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = { inProgress: commandRow('resume_motion', 'applying'), ended: [] };
  });
  expect(test.runtime.runnerBusyReason).toBe(`${BUSY}: Resume, applying.`);
  // Sent anyway (another client may have started it meanwhile): refused at once, no row.
  test.send.mockRejectedValueOnce(new CommandRefusalError({ code: 'execution_control_ingress_busy',
    message: 'another command is in progress', details: { command_in_progress: 'resume_motion' }, origin: null }));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.state.error).toContain('Another command in progress');
  expect(test.runtime.state.pending).toBeNull();
});

it('keeps a lost connection a delivery state, not a command outcome', async () => {
  const test = await mount();
  test.send.mockRejectedValueOnce(new CommandDeliveryUnconfirmedError(
    'Command delivery is unconfirmed; commands other than Stop are disabled.'));
  await act(async () => { await test.runtime.run(); });
  expect(test.runtime.state.error).toContain('Command delivery is unconfirmed');
});

it('reads stored values again once their edit ended succeeded, and shows any other end', async () => {
  const test = await mount();
  const values = { projectPath: '/projects/cell', programRevision: `sha256:${'a'.repeat(64)}`,
    programRunId: null } as PersistentKeyValues;
  apiMock.programs.editPersistentValue.mockResolvedValueOnce(commandRow('persistent_value_set', 'applying'));
  let edit!: Promise<PersistentKeyValues | null>;
  await act(async () => { edit = test.runtime.persistentValues.edit(values, 'layer', 2); });
  expect(apiMock.programs.editPersistentValue.mock.calls[0]?.[0]).not.toHaveProperty('request_key');
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.runnerCommands = { inProgress: null,
      ended: [commandRow('persistent_value_set', 'applying', rowEnd('succeeded', null))] };
  });
  await expect(edit).resolves.toBeNull();
  apiMock.programs.editPersistentValue.mockResolvedValueOnce(
    { ...commandRow('persistent_value_reset', 'applying', rowEnd('refused', 'persistent_key_not_found')), requestKey: 'b'.repeat(32) },
  );
  await act(async () => {
    await expect(test.runtime.persistentValues.edit(values, 'layer')).rejects.toThrow(/^Reset stored value: /);
  });
});

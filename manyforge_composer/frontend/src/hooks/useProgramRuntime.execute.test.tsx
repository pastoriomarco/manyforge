import { StrictMode, act } from 'react';
import { afterEach, expect, it, vi } from 'vitest';
import type {
  AppliedProgramSelection,
  ProgramRuntimeControlProjection,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { RuntimeScaleControlSelector, RuntimeScaleControlStatus } from '../types/robotControl';
import { heldRun } from '../testFixtures/heldRun';
import { useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import { CommandRefusalError } from '../state/commandRefusal';

const apiMock = vi.hoisted(() => ({
  programs: { currentScenario: vi.fn(), applied: vi.fn() },
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
    snapshot: { receiverEpoch: 'receiver.1', authoringWorkspace,
      authoringAdmission: { authoringAllowed: true, admissionGeneration: 7 },
      componentOverview: { basis: { appliedProgramOwnerEpoch: 'applied.1', appliedProgramGeneration: 1 } },
      runtime: { activeConfiguration: { retainedBundleId: 'bundle', graphRevision: 'graph' },
        sections: { manager: { sourceOwnerEpoch: 'manager.1', payload: { active: {
          retained_bundle_id: 'bundle', graph_revision: 'graph' } } } } },
      executionControlStatus: null,
      programCycle: { programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 1,
        state: 'idle_after_reset', programRunId: null, cycleClaimGeneration: null,
        terminalOrSettlementReference: null, runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null, executionControlRevision: 1,
        causalCorrelationId: null, causalSessionId: null, causalClientSequence: null,
        currentCommandKind: null, failure: null, resetPhase: null, resetOptions: null,
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
async function mount(resumed = false) {
  vi.useFakeTimers();
  for (const section of Object.values(apiMock)) for (const method of Object.values(section)) method.mockReset();
  const host = domHost();
  let { snapshot, workspace } = facts();
  if (resumed) snapshot = { ...snapshot, programCycle: { ...snapshot.programCycle,
    state: 'running', programRunId: 'run.1', run: { ...heldRun(), state: 'full_stopped' } } };
  apiMock.programs.currentScenario.mockResolvedValue({ ready: false,
    unavailableReasons: ['scenario_not_ready'], currentScenario: null });
  apiMock.programs.applied.mockResolvedValue({ scenarioId: 'scenario', sourceDocumentDigest: 'source',
    programRevision: 'program.revision', retainedBundleId: 'bundle', graphRevision: 'graph',
    recordDigest: 'record.1', sourceOwnerEpoch: 'applied.1',
    sourceGeneration: 1 } as AppliedProgramSelection);
  apiMock.deploymentWorkspace.get.mockResolvedValue({ managerApplied: { scenarioId: 'scenario',
    deploymentRevision: 'deployment.revision', activationOrder: {
      componentIds: ['robot.a'], retainedBundleId: 'bundle', graphRevision: 'graph' } } });
  apiMock.execution.identity.mockResolvedValue({ ownerEpoch: 'execution.1', graphRevision: 'graph',
    retainedBundleId: 'bundle', activePublicationGeneration: 1, controlManagementMode: 'component_lifecycle' });
  const admissionsAtSend: number[] = [];
  const send = vi.fn(async (_kind: string, _payload: unknown, _correlation: string) => {
    admissionsAtSend.push(snapshot.authoringAdmission.admissionGeneration);
    return { disposition: 'received' };
  });
  const source = { sendExecutionCommand: send } as unknown as HeadlessStateSource;
  let runtime!: ProgramRuntimeController;
  function Harness() {
    runtime = useProgramRuntime(true, snapshot, workspace, source, false);
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
    schemaVersion: 'manyforge.program_runtime_control_projection.v9',
    ownerId: 'program-runner.development-workbench',
    ownerEpoch: run.ownerEpoch,
    programRunId: run.capture.programRunId,
    runRevision: run.runRevision,
    runtimeControls: { runtimeControlRevision: 1 } as never,
    recoveryOffer: null,
    recoveryHolds: [],
    executionSelections: {
      selectionRevision: 7,
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
  // N8: Resume names its run and selection; tree ticks move only the run revision.
  const payload = test.send.mock.calls[0]?.[1] as Record<string, unknown>;
  expect(payload).toMatchObject({ program_run_id: run.capture.programRunId,
    expected_owner_epoch: run.ownerEpoch, expected_selection_revision: 7 });
  expect(payload).not.toHaveProperty('expected_run_revision');
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

it('Reset ignores a confirmation-time owner change and keeps the send-time owner fence', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
    snapshot.programCycle.programRunnerOwnerEpoch = 'runner.epoch.2';
  });
  let result;
  await act(async () => {
    result = await test.runtime.reset({ expected: { programRunId: null, cycleGeneration: 1 } });
  });
  expect(test.send).toHaveBeenCalledExactlyOnceWith('program_reset', expect.objectContaining({
    expected_owner_epoch: 'runner.epoch.2', expected_cycle_generation: 1,
  }), expect.any(String));
  expect(result).toMatchObject({ status: 'sent', reset: {
    programRunnerOwnerEpoch: 'runner.epoch.2', cycleGeneration: 1,
  } });
});

it('plain Reset defaults both effects off and never resends unconfirmed delivery', async () => {
  const test = await mount();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle.allowedCommands = [{ commandKind: 'program_reset',
      allowed: true, disabledReason: null, semanticTarget: {} }];
  });
  test.send.mockRejectedValueOnce(new CommandDeliveryUnconfirmedError('reply lost'));
  let result;
  await act(async () => { result = await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(test.send.mock.calls[0]?.[1]).toMatchObject({
    reload_initial_scene: false, reset_robot_position: false,
  });
  expect(result).toMatchObject({ status: 'unconfirmed', reset: {
    requestId: (test.send.mock.calls[0]?.[1] as { request_id: string }).request_id,
  } });
  await act(async () => { await vi.advanceTimersByTimeAsync(5000); });
  expect(test.send).toHaveBeenCalledTimes(1);
});

// The synthesized failed-run notice: once per run revision, cleared by Reset
// reaching idle or by another run, never rebuilt after Dismiss or a command.
function failRun(value: ReturnType<typeof facts>, programRunId = 'run.1', runRevision = 4) {
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
  await test.observe((value) => failRun(value, 'run.1', 5));
  expect(test.runtime.state.error).toContain('run.1: failed, revision 5');
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
  await test.observe((value) => failRun(value, 'run.2'));
  expect(test.runtime.state.error).toContain('run.2: failed');
  await act(async () => { await test.runtime.reset(); });
  expect(test.send).toHaveBeenCalledWith('program_reset', expect.anything(), expect.anything());
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
    snapshot.programCycle = { ...snapshot.programCycle, state: 'running', programRunId: 'run.2',
      runRevision: 1, run: { ...heldRun(), state: 'running', runRevision: 1, nodeStatuses: {},
        capture: { ...heldRun().capture, programRunId: 'run.2' } } };
  });
  expect(test.runtime.state.error).toBeNull();
});

// One backend scaling setting, shared by every row that targets it (as "All
// groups" and a named group do for a single robot).
const scaleSelector: RuntimeScaleControlSelector = { kind: 'singular',
  programBindingId: `sha256:${'c'.repeat(64)}`, componentRoleId: 'robot', targetRoleId: 'arm' };
async function mountScaleRows(groupIds: string[]) {
  const test = await mount();
  const backend = { generation: 2, scale: 0.5 };
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
  test.send.mockImplementation(async (kind: string, payload: unknown, correlation: string) => {
    const command = payload as { selector: Record<string, unknown>;
      expected_control_generation: number; scale: number };
    expect(kind).toBe('runtime_scale');
    if (command.expected_control_generation !== backend.generation) {
      throw new CommandRefusalError({ code: 'runtime_scale_control_generation_stale',
        message: 'stale', details: {}, origin: null });
    }
    backend.generation += 1; backend.scale = command.scale;
    return { disposition: 'received', result: { ok: true, api_version: 1,
      command: 'set_runtime_scale_control', accepted: { accepted_at_ns: 1,
        active_operation_id: null, active_operation_revision: null,
        active_operation_scale_generation: null, control_generation: backend.generation,
        replay_status: 'new', request_id: correlation, requested_scale: command.scale,
        selector: command.selector } } } as never;
  });
  const run = heldRun();
  await test.observe(({ snapshot }) => {
    snapshot.programCycle = { ...snapshot.programCycle, state: 'running',
      programRunId: run.capture.programRunId, runRevision: run.runRevision, run,
      runtimeControls: { schemaVersion: 'manyforge.program_runtime_control_projection.v9',
        ownerId: 'program-runner.development-workbench', ownerEpoch: run.ownerEpoch,
        programRunId: run.capture.programRunId, runRevision: run.runRevision,
        runtimeControls: { runtimeControlRevision: 1 }, recoveryOffer: null, recoveryHolds: [],
        executionSelections: { selectionRevision: 1, motionHeld: false,
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
  expect(test.backend).toEqual({ generation: 4, scale: 0.8 });
  expect(test.runtime.groupRuntimeScales.all).toMatchObject({ draft: 0.8, pending: false });
});

it('reverts a refused value and re-reads every row after a stale generation', async () => {
  const test = await mountScaleRows(['all', 'carry']);
  // The shared setting moved without this client re-reading it.
  test.backend.generation = 5; test.backend.scale = 0.2;
  const reads = apiMock.execution.runtimeScaleControl.mock.calls.length;
  await act(async () => { test.runtime.setGroupRuntimeScale('all', 0.9); });
  expect(test.send).toHaveBeenCalledTimes(1);
  expect(apiMock.execution.runtimeScaleControl.mock.calls.length).toBeGreaterThan(reads);
  for (const groupId of ['all', 'carry']) {
    expect(test.runtime.groupRuntimeScales[groupId]).toMatchObject({
      draft: 0.2, pending: false, status: { controlGeneration: 5 } });
  }
});

import { act, createElement, useEffect, useMemo } from 'react';
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';

import {
  heldAuthoring,
  heldCatalog,
  heldOffer,
  heldProgram,
  heldRun,
} from '../testFixtures/heldRun';
import type {
  DeploymentWorkspaceResponse,
  JsonValue,
  ManagerApplyStatus,
  ProgramAuthoringDocument,
  ProgramCycleStatus,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { DirectControlMotionStatus } from '../types/robotControl';

// The owners' transports are faked; the controllers under test are the real
// authoring gate, workspace, runtime, Apply-after-Reset and preview hooks,
// wired as App wires them.
const apiMock = vi.hoisted(() => ({
  programs: {
    currentScenario: vi.fn(),
    applied: vi.fn(),
    appliedStatus: vi.fn(),
    nodeCorrectionPreview: vi.fn(),
  },
  programWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
    apply: vi.fn(),
    discard: vi.fn(),
  },
  deploymentWorkspace: {
    get: vi.fn(),
    mutate: vi.fn(),
    applyDraft: vi.fn(),
  },
  authoringWorkspace: { recordResetApplyContext: vi.fn() },
  execution: { runtimeScaleControl: vi.fn() },
}));
vi.mock('../api/client', async (importOriginal) => ({
  ...await importOriginal<typeof import('../api/client')>(),
  api: apiMock,
}));

import { ApiError } from '../api/client';

import {
  authoringUnavailableReason,
  authoringWorkspaceIdentity,
  draftEditUnavailableReason,
  projectFacts,
  publicationIsCurrent,
  type PublishedAuthoringWorkspace,
} from '../state/authoringWorkspace';
import { deploymentUnapplied } from '../state/documentStates';
import { managerApplySettlementKey } from '../state/managerApply';
import { resetApplyPhaseText } from '../state/resetApply';
import { programActionUnavailableReason } from '../state/authoringWorkspace';
import { useAppliedSettlementRefresh } from './useAppliedSettlementRefresh';
import type { AuthoringWorkspaceController } from './useAuthoringWorkspace';
import { useDeploymentWorkspace, type DeploymentWorkspaceController } from './useDeploymentWorkspace';
import { CommandDeliveryUnconfirmedError, type HeadlessStateSource } from './useHeadlessState';
import { useHeldNodePreview, type HeldNodePreviewController } from './useHeldNodePreview';
import { useProgramRuntime, type ProgramRuntimeController } from './useProgramRuntime';
import { useProgramWorkspace, type ProgramWorkspaceController } from './useProgramWorkspace';
import { useResetApply, type ResetApplyController } from './useResetApply';

const digest = (fill: string) => `sha256:${fill.repeat(64)}`;
const NO_TARGETS: readonly DirectControlMotionStatus[] = [];
const APPLIED_DEPLOYMENT = digest('d');

type Hold = 'failure_pause' | 'operator_stop' | 'halt' | 'never_run' | 'stopping' | 'running'
  | 'containment_unconfirmed' | 'manual_operation' | 'idle';

interface Environment {
  hold: Hold;
  cycle: Partial<ProgramCycleStatus>;
  admissionGeneration: number;
  draftRevision: number;
  programRevision: string;
  /** The Draft authoring source; the captured Program's by default. */
  authoring?: ProgramAuthoringDocument;
  deploymentRevision: number;
  deploymentContent: string;
  activation: ManagerApplyStatus | null;
  /** The Applied Program revision; the captured Program's by default. */
  appliedRevision?: string;
  /** Overrides the observed authoring admission (Idle opens it by default). */
  admissionOpen?: boolean;
  /** The native execution owner (Robot Control) epoch. */
  executionOwnerEpoch?: string;
}

/** The holds in which nothing moves and nothing can start. */
const STOPPED_HOLDS: readonly Hold[] = ['failure_pause', 'operator_stop', 'halt', 'never_run'];

/** Held on a failed node, the other motion stopped; one unapplied Program edit. */
const HELD: Environment = {
  hold: 'failure_pause',
  cycle: {},
  admissionGeneration: 9,
  draftRevision: 3,
  programRevision: digest('f'),
  deploymentRevision: 1,
  deploymentContent: APPLIED_DEPLOYMENT,
  activation: null,
};

function heldCycle(hold: Hold, patch: Partial<ProgramCycleStatus>): ProgramCycleStatus {
  const run = {
    ...heldRun(),
    state: hold === 'halt' ? 'full_stopped'
      : hold === 'operator_stop' || hold === 'running' || hold === 'never_run'
        ? 'running' : 'intervention_required',
    activeOperationCount: hold === 'halt' ? 0 : null,
  };
  const offer = hold === 'halt' || hold === 'operator_stop' || hold === 'running'
    || hold === 'never_run'
    ? null
    : heldOffer({
      actions: [{
        actionId: 'retry_motion_from_current_state', labelKey: 'retry',
        requiresConfirmation: false, performsBackendReset: false,
        enabled: hold !== 'containment_unconfirmed',
        disabledReasonCode: hold === 'containment_unconfirmed'
          ? 'failure_pause_containment_unconfirmed' : null,
      }, {
        actionId: 'reset_cycle', labelKey: 'reset',
        requiresConfirmation: true, performsBackendReset: true,
        enabled: true, disabledReasonCode: null,
      }],
    });
  const groupState = hold === 'stopping' ? 'stopping'
    : hold === 'running' ? 'running'
      : hold === 'halt' ? 'unavailable'
        : hold === 'never_run' ? 'ready' : 'held';
  return {
    programRunnerOwnerEpoch: 'runner.epoch.1',
    cycleGeneration: 7,
    state: 'running',
    programRunId: 'run.1',
    cycleClaimGeneration: 2,
    terminalOrSettlementReference: null,
    runRevision: 4,
    nodeStatusRevision: 1,
    runtimeControlRevision: 1,
    executionControlRevision: 1,
    causalCorrelationId: null,
    causalSessionId: null,
    causalClientSequence: null,
    currentCommandKind: null,
    failure: null,
    resetPhase: null, resetOptions: null,
    resetRobotPosition: { available: false, reason: 'unsupported' },
    allowedCommands: ['program_reset', 'program_motion_resume', 'program_resume_full_stop']
      .map((commandKind) => ({ commandKind, semanticTarget: {}, allowed: true, disabledReason: null })),
    recoveryOffer: offer,
    run,
    runtimeControls: {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run.1',
      runRevision: 4,
      runtimeControls: { runtimeControlRevision: 1 },
      recoveryOffer: offer,
      executionSelections: {
        selectionRevision: 1,
        motionHeld: groupState === 'held',
        groups: [{
          groupId: 'all', nodeCount: 3, heldNodeCount: groupState === 'held' ? 3 : 0,
          state: groupState, stopAllowed: false,
          startAllowed: false, startUnavailableReason: null,
          resumeAllowed: true, resumeUnavailableReason: null,
          runtimeScaleSelector: null, runtimeScaleUnavailableReason: null,
          resourceIds: [],
        }],
      },
    },
    ...patch,
  } as unknown as ProgramCycleStatus;
}

function idleCycle(correlationId: string | null): Partial<ProgramCycleStatus> {
  return {
    state: 'idle_after_reset', cycleGeneration: 8, programRunId: null,
    causalCorrelationId: correlationId, allowedCommands: [], recoveryOffer: null,
    run: null, runtimeControls: null,
  };
}

function programWorkspace(environment: Environment): ProgramWorkspaceResponse {
  const program = heldProgram();
  const applied = environment.appliedRevision ?? digest('a');
  const matches = environment.programRevision === applied;
  return {
    schemaVersion: 'manyforge.composer_program_workspace.v2',
    authoringWorkspace: identity(environment),
    draft: {
      draftId: 'draft-1',
      draftRevision: environment.draftRevision,
      programLoaded: true,
      authoring: environment.authoring ?? heldAuthoring(program),
      candidateAvailable: true,
      programRevision: environment.programRevision,
      program,
      diagnostics: [],
      formulaReferences: { computedKeys: {}, nodes: {} },
    },
    applicability: { applyReady: true, unavailableReason: null },
    saved: { available: true, matchesDraft: environment.programRevision === digest('a'), path: '/cell/program.yaml', programRevision: digest('a') },
    applied: { available: true, matchesDraft: matches, programRevision: applied, unavailableReason: null },
    hasUnsavedChanges: !matches,
    hasUnappliedChanges: !matches,
  } as unknown as ProgramWorkspaceResponse;
}

function deploymentWorkspace(environment: Environment): DeploymentWorkspaceResponse {
  return {
    schemaVersion: 'manyforge.composer_deployment_workspace.v1',
    authoringWorkspace: identity(environment),
    draft: {
      id: 'deployment-draft-1',
      revision: environment.deploymentRevision,
      document: { api_version: 'manyforge.deployment_intent.v2' },
      deploymentArtifactId: 'cell',
      deploymentRevision: environment.deploymentContent,
      graphRevision: digest('9'),
      differsFromSaved: environment.deploymentContent !== APPLIED_DEPLOYMENT,
    },
    saved: { path: '/cell/cell.deployment.yaml', deploymentRevision: APPLIED_DEPLOYMENT },
    managerApplied: { activationOrder: null, deploymentRevision: APPLIED_DEPLOYMENT, scenarioId: 'cell' },
    runAdmission: { ready: environment.deploymentContent === APPLIED_DEPLOYMENT, reason: null },
    binding: null,
    scenarioCandidates: [],
    savedScenarioCandidates: [],
    unavailableReasons: [],
  };
}

function identity(environment: Environment) {
  return {
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: environment.draftRevision + environment.deploymentRevision,
    programDraftId: 'draft-1',
    programDraftRevision: environment.draftRevision,
    deploymentDraftId: 'deployment-draft-1',
    deploymentDraftRevision: environment.deploymentRevision,
  };
}

function publication(environment: Environment): PublishedAuthoringWorkspace {
  const current = programWorkspace(environment);
  const snapshot = {
    schemaVersion: 'manyforge.composer_authoring_workspace_snapshot.v2' as const,
    workspaceEpoch: 'workspace.epoch.1',
    workspaceRevision: environment.draftRevision + environment.deploymentRevision,
    program: { draft: current.draft, saved: current.saved },
    deployment: {
      draftId: 'deployment-draft-1', draftRevision: environment.deploymentRevision,
      document: { api_version: 'manyforge.deployment_intent.v2' },
      savedPath: null, savedDocument: null,
      differsFromSaved: environment.deploymentContent !== APPLIED_DEPLOYMENT,
    },
    project: {
      documentPath: '/cell', documentId: null, documentName: null, documentDescription: null,
      sourceDiagnostics: [], sourceDocumentDigest: null, programRoleAssignment: null,
      sourceProgramDigest: null, sourceDeploymentDigest: null,
    },
  };
  return { identity: authoringWorkspaceIdentity(snapshot), snapshot };
}

function headlessSnapshot(environment: Environment): ComposerHeadlessState {
  const cycle = environment.hold === 'idle'
    ? heldCycle('failure_pause', { ...idleCycle(null), ...environment.cycle })
    : heldCycle(environment.hold, environment.cycle);
  return {
    receiverEpoch: 'receiver.1',
    executionControlStatus: {
      ownerEpoch: environment.executionOwnerEpoch ?? 'execution.epoch.1',
      sceneRevision: { scene_id: 'scene', revision_id: 1, content_hash: digest('5') },
      manualOperations: environment.hold === 'manual_operation'
        ? [{ resident: true, state: 'executing', operationId: 'manual.1', programRunId: 'manual-run' }]
        : [],
    },
    componentOverview: { basis: { appliedProgramOwnerEpoch: 'applied.1', appliedProgramGeneration: 1 } },
    runtime: { sections: { manager: { sourceOwnerEpoch: 'manager.1', payload: {} } } },
    authoringAdmission: {
      authoringAllowed: environment.admissionOpen ?? cycle.state === 'idle_after_reset',
      admissionGeneration: environment.admissionGeneration,
    },
    // The backend's published held-run verdict for each hold (the rule itself
    // is tested in Headless test_held_run_authoring.py).
    heldRunDraftEditAllowed: cycle.state === 'running'
      && STOPPED_HOLDS.includes(environment.hold),
    programCycle: cycle,
    authoringWorkspace: identity(environment),
    authoringWorkspaceContainment: null,
  } as unknown as ComposerHeadlessState;
}

function authoringController(
  published: PublishedAuthoringWorkspace,
  snapshot: ComposerHeadlessState,
): AuthoringWorkspaceController {
  return {
    published,
    current: publicationIsCurrent(published, snapshot),
    unavailableReason: authoringUnavailableReason(published, snapshot),
    editUnavailableReason: draftEditUnavailableReason(published, snapshot),
    project: projectFacts(published),
    reading: false,
    error: null,
    refresh: async () => undefined,
    beginMutation: () => true,
    finishMutation: () => undefined,
    observeApply: () => undefined,
    submissionUnconfirmed: false,
    correlatedApply: null,
    rechecking: false,
    recheckMessage: null,
    recheckSubmission: async () => undefined,
  };
}

interface Controllers {
  program: ProgramWorkspaceController;
  deployment: DeploymentWorkspaceController;
  runtime: ProgramRuntimeController;
  resetApply: ResetApplyController;
  preview: HeldNodePreviewController;
}

const transport = { sendExecutionCommand: vi.fn() };
let root: { render: (node: unknown) => void; unmount: () => void } | null = null;
let hooks: Controllers | null = null;
// The workspace owners serve whatever Drafts the environment currently holds.
let served: Environment = HELD;

function Host({ environment }: { environment: Environment }) {
  const snapshot = useMemo(() => headlessSnapshot(environment), [environment]);
  const published = useMemo(() => publication(environment), [environment]);
  const authoring = useMemo(
    () => authoringController(published, snapshot),
    [published, snapshot],
  );
  const program = useProgramWorkspace(snapshot, authoring);
  const deployment = useDeploymentWorkspace(true, snapshot, authoring);
  const runtime = useProgramRuntime(
    true, snapshot, program.state.workspace,
    transport as unknown as HeadlessStateSource,
    deploymentUnapplied(deployment.workspace),
  );
  const resetApply = useResetApply({
    runtime, program, deployment, snapshot, activation: environment.activation,
  });
  const preview = useHeldNodePreview({
    runtime, workspace: program, snapshot, catalogEntries: heldCatalog, motionTargets: NO_TARGETS,
  });
  useAppliedSettlementRefresh({
    settlement: managerApplySettlementKey(environment.activation), runtime, program,
  });
  useEffect(() => {
    hooks = { program, deployment, runtime, resetApply, preview };
  }, [deployment, preview, program, resetApply, runtime]);
  return null;
}

async function mount(environment: Environment) {
  const node = (): Record<string, unknown> => ({
    nodeType: 1, nodeName: 'DIV', tagName: 'DIV', namespaceURI: 'http://www.w3.org/1999/xhtml',
    ownerDocument: document, style: {}, addEventListener: () => undefined, removeEventListener: () => undefined,
    appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
    setAttribute: () => undefined, removeAttribute: () => undefined,
  });
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {}, visibilityState: 'visible',
    addEventListener: () => undefined, removeEventListener: () => undefined, createElement: node,
  };
  document.documentElement = node();
  const window = {
    document, location: { href: 'http://composer.test/' }, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout.bind(globalThis),
    clearTimeout: globalThis.clearTimeout.bind(globalThis),
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  const { createRoot } = await import('react-dom/client');
  root = createRoot(node() as unknown as Element) as unknown as typeof root;
  const render = (next: Environment) => act(async () => {
    served = next;
    root?.render(createElement(Host, { environment: next }));
  });
  await render(environment);
  // The Applied Program and the workspace projections arrive from the owners.
  await act(async () => { await Promise.resolve(); });
  await act(async () => { await Promise.resolve(); });
  return render;
}

const flush = () => act(async () => { await Promise.resolve(); await Promise.resolve(); });

beforeEach(() => {
  apiMock.programs.currentScenario.mockResolvedValue({
    ready: true, unavailableReasons: [], currentScenario: null,
  });
  apiMock.programs.applied.mockResolvedValue({
    sourceOwnerEpoch: 'applied.1', sourceGeneration: 1, recordDigest: digest('r'),
    programRevision: digest('a'), program: heldProgram(), programBinding: null,
  });
  apiMock.programs.nodeCorrectionPreview.mockReturnValue(new Promise(() => undefined));
  apiMock.programWorkspace.get.mockImplementation(async () => programWorkspace(served));
  apiMock.deploymentWorkspace.get.mockImplementation(async () => deploymentWorkspace(served));
  apiMock.authoringWorkspace.recordResetApplyContext.mockResolvedValue(undefined);
  transport.sendExecutionCommand.mockResolvedValue({ accepted: true });
});

afterEach(async () => {
  if (root !== null) await act(async () => root?.unmount());
  root = null;
  hooks = null;
  served = HELD;
  vi.unstubAllGlobals();
  vi.clearAllMocks();
});

describe('A refused stale Deployment fence', () => {
  // Refused before any effect, and the workspace moved: the hook refreshes it
  // instead of only reporting the refusal.
  const stale = () => new ApiError('Deployment Draft changed', 409, {
    detail: { code: 'composer_draft_revision_stale', message: 'Deployment Draft changed', effect: 'none' },
  });

  it('refreshes the workspace after a stale Draft edit', async () => {
    await mount({ ...HELD, hold: 'operator_stop' });
    const reads = apiMock.deploymentWorkspace.get.mock.calls.length;
    apiMock.deploymentWorkspace.mutate.mockRejectedValueOnce(stale());
    await act(async () => { await hooks!.deployment.mutate(['tick'], 'set', 1); });
    expect(apiMock.deploymentWorkspace.mutate).toHaveBeenCalledOnce();
    expect(apiMock.deploymentWorkspace.get).toHaveBeenCalledTimes(reads + 1);
    expect(hooks!.deployment.feedback).toBeNull();
    expect(hooks!.deployment.error).toBe('Deployment Draft changed');
    expect(hooks!.deployment.mutationAvailable).toBe(true);
  });

  it('refreshes the workspace after a stale Apply', async () => {
    await mount({
      ...HELD, hold: 'idle', cycle: idleCycle(null), deploymentRevision: 2, deploymentContent: digest('e'),
    });
    const reads = apiMock.deploymentWorkspace.get.mock.calls.length;
    apiMock.deploymentWorkspace.applyDraft.mockRejectedValueOnce(stale());
    await act(async () => { await hooks!.deployment.applyDraft(); });
    expect(apiMock.deploymentWorkspace.applyDraft).toHaveBeenCalledOnce();
    expect(apiMock.deploymentWorkspace.get).toHaveBeenCalledTimes(reads + 1);
    expect(hooks!.deployment.feedback).toBeNull();
    expect(hooks!.deployment.error).toBe('Deployment Draft changed');
  });
});

describe('Draft editing while a Program run exists', () => {
  it.each<Hold>(STOPPED_HOLDS)(
    'is editable when all motion is confirmed stopped (%s)',
    async (hold) => {
      await mount({ ...HELD, hold });
      expect(hooks!.program.mutationUnavailableReason).toBeNull();
      expect(hooks!.deployment.mutationUnavailableReason).toBeNull();
      // Other document actions keep requiring Idle.
      expect(hooks!.program.documentUnavailableReason).toContain('authoring requires Idle');
      expect(hooks!.deployment.documentUnavailableReason).toContain('authoring requires Idle');
      // A Program (and Scene) edit goes through the ordinary mutation path.
      apiMock.programWorkspace.mutate.mockResolvedValueOnce({
        mutation: 'viewer_draft_replace', details: {},
        workspace: programWorkspace({ ...HELD, hold, draftRevision: 4 }),
        authoringWorkspace: publication({ ...HELD, hold, draftRevision: 4 }).snapshot,
      });
      await act(async () => {
        await hooks!.program.mutate('viewer_draft_replace', { viewer: {} });
      });
      expect(apiMock.programWorkspace.mutate).toHaveBeenCalledExactlyOnceWith(
        expect.objectContaining({ draftId: 'draft-1', expectedAdmissionGeneration: 9 }),
        'viewer_draft_replace',
        { viewer: {} },
      );
      apiMock.deploymentWorkspace.mutate.mockResolvedValueOnce(
        deploymentWorkspace({ ...HELD, hold, deploymentRevision: 2, deploymentContent: digest('e') }),
      );
      await act(async () => { await hooks!.deployment.mutate(['tick'], 'set', 1); });
      expect(apiMock.deploymentWorkspace.mutate).toHaveBeenCalledOnce();
    },
  );

  it.each<Hold>(['stopping', 'running', 'containment_unconfirmed', 'manual_operation'])(
    'stays locked while motion is not confirmed stopped (%s)',
    async (hold) => {
      await mount({ ...HELD, hold });
      expect(hooks!.program.mutationUnavailableReason)
        .toContain('editing requires Idle or all Program motion stopped');
      expect(hooks!.deployment.mutationAvailable).toBe(false);
      await act(async () => {
        await hooks!.program.mutate('viewer_draft_replace', { viewer: {} });
      });
      expect(apiMock.programWorkspace.mutate).not.toHaveBeenCalled();
    },
  );

  it.each<Hold>(['failure_pause', 'never_run'])('locks again when motion starts (%s)', async (hold) => {
    const render = await mount({ ...HELD, hold });
    expect(hooks!.program.mutationAvailable).toBe(true);
    await render({ ...HELD, hold: 'running' });
    expect(hooks!.program.mutationAvailable).toBe(false);
  });
});

describe('Resume, Retry and Execute with an unapplied Draft', () => {
  it('are refused until the Draft is Applied or Discarded; Reset is not', async () => {
    const render = await mount(HELD);
    const runtime = hooks!.runtime;
    expect(runtime.draftUnappliedReason).toMatch(/^Apply or Discard first: unapplied changes in Program/);
    expect(runtime.commandAllowed('program_motion_resume')).toBe(false);
    expect(runtime.commandDisabledReason('program_motion_resume')).toBe(runtime.draftUnappliedReason);
    expect(runtime.commandAllowed('program_reset')).toBe(true);
    await act(async () => {
      runtime.applyRecoveryAction('retry_motion_from_current_state');
      await runtime.resumeMotion('all');
    });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();

    // Discarded: the Draft is exactly what is Applied again.
    await render({ ...HELD, draftRevision: 4, programRevision: digest('a') });
    await flush();
    expect(hooks!.runtime.draftUnappliedReason).toBeNull();
    expect(hooks!.runtime.commandAllowed('program_motion_resume')).toBe(true);
    await act(async () => { hooks!.runtime.applyRecoveryAction('retry_motion_from_current_state'); });
    expect(transport.sendExecutionCommand).toHaveBeenCalledExactlyOnceWith(
      'execution_recovery',
      expect.objectContaining({ action_id: 'retry_motion_from_current_state' }),
      expect.any(String),
    );
  });

  it('refuse Run of a run Executed but never Run until the Draft is Applied', async () => {
    await mount({ ...HELD, hold: 'never_run' });
    const runtime = hooks!.runtime;
    expect(runtime.draftUnappliedReason).toMatch(/^Apply or Discard first/);
    expect(runtime.commandAllowed('program_motion_resume')).toBe(false);
    await act(async () => { await runtime.resumeMotion('all'); });
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });

  it('also refuse with an unapplied Deployment edit', async () => {
    await mount({ ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') });
    expect(hooks!.runtime.draftUnappliedReason).toContain('unapplied changes in Deployment');
    expect(hooks!.runtime.commandAllowed('program_resume_full_stop')).toBe(false);
  });
});

describe('Apply while a run exists', () => {
  async function confirm(document: 'program' | 'deployment') {
    let refusal: string | null = 'not called';
    await act(async () => { refusal = await hooks!.resetApply.confirm(document); });
    return refusal;
  }

  it('Resets exactly the held run, waits for its Idle, then Applies exactly the confirmed Program Draft', async () => {
    const render = await mount(HELD);
    expect(hooks!.resetApply.runExists).toBe(true);
    expect(hooks!.resetApply.unavailableReason('program')).toBeNull();
    expect(await confirm('program')).toBeNull();
    const [context] = apiMock.authoringWorkspace.recordResetApplyContext.mock.calls[0];
    expect(context).toMatchObject({
      document: 'program', programRunId: 'run.1', cycleGeneration: 7,
      draftId: 'draft-1', draftRevision: 3, draftContentRevision: digest('f'), appliedRevision: digest('a'),
    });
    const [kind, request, correlationId] = transport.sendExecutionCommand.mock.calls[0] as [
      string, Record<string, JsonValue>, string,
    ];
    expect(kind).toBe('program_reset');
    expect(correlationId).toBe(context.correlationId);
    expect(request).toMatchObject({
      program_run_id: 'run.1', expected_owner_epoch: 'runner.epoch.1', expected_cycle_generation: 7,
    });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'confirmed' });
    // A receipt is not settlement: nothing is Applied while the run is held.
    await render({ ...HELD, admissionGeneration: 10 });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();

    apiMock.programWorkspace.apply.mockResolvedValue({
      status: 'accepted',
      workspace: programWorkspace(HELD),
      headless: { apply_id: 'apply-1', apply_generation: 2 },
    });
    const settled = { ...HELD, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(correlationId) };
    await render(settled);
    await flush();
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ draftId: 'draft-1', expectedDraftRevision: 3, expectedAdmissionGeneration: 11 }),
      undefined,
      correlationId,
    );
    expect(apiMock.programWorkspace.mutate).not.toHaveBeenCalled();
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'awaiting_apply', apply: { applyId: 'apply-1', applyGeneration: 2 },
    });
    // Only the exact activation's success completes it; nothing else follows.
    await render({
      ...settled,
      activation: { applyId: 'apply-1', applyGeneration: 2, state: 'succeeded', failure: null } as ManagerApplyStatus,
    });
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'applied', apply: { applyId: 'apply-1', applyGeneration: 2 },
    });
    expect(transport.sendExecutionCommand).toHaveBeenCalledOnce();
  });

  it('Applies the Deployment Draft after Reset, handing a disruptive impact to its ordinary confirmation', async () => {
    const edited = { ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') };
    const render = await mount(edited);
    expect(hooks!.resetApply.unavailableReason('deployment')).toBeNull();
    expect(await confirm('deployment')).toBeNull();
    const correlationId = transport.sendExecutionCommand.mock.calls[0][2] as string;
    apiMock.deploymentWorkspace.applyDraft.mockResolvedValue({
      schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1',
      workspace: deploymentWorkspace(edited),
      result: {
        status: 'confirmation_required', confirmationToken: digest('7'),
        preparation: { requestOwnerEpoch: 'owner', requestId: 'prep' },
        preparedActivation: {}, candidatePreview: null, managerApply: null,
      },
    });
    await render({ ...edited, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(correlationId) });
    await flush();
    // The complete activation input confirmed is sent for the backend to recheck.
    const scope = {
      deploymentDraftId: 'deployment-draft-1', deploymentDraftRevision: 2, deploymentRevision: null,
      programDraftId: 'draft-1', programDraftRevision: 3, programRevision: digest('a'),
      sourceDocumentDigest: null, programRoleAssignment: null,
    };
    expect(apiMock.deploymentWorkspace.applyDraft).toHaveBeenCalledExactlyOnceWith(
      expect.objectContaining({ expectedAdmissionGeneration: 11 }),
      null,
      undefined,
      correlationId,
      scope,
    );
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'confirmation_required' });
    expect(hooks!.deployment.pendingApplyConfirmation).not.toBeNull();

    // The operator confirms the impact: that Apply carries the same shared
    // correlation and confirmed scope, and the chain follows it to settlement.
    apiMock.deploymentWorkspace.applyDraft.mockResolvedValue({
      schemaVersion: 'manyforge.composer_deployment_candidate_operation.v1',
      workspace: deploymentWorkspace(edited),
      result: {
        status: 'accepted', confirmationToken: null, preparation: null,
        preparedActivation: {}, candidatePreview: null,
        managerApply: { applyId: 'apply-9', applyGeneration: 5 },
      },
    });
    const idle = { ...edited, hold: 'idle' as const, admissionGeneration: 12, cycle: idleCycle(correlationId) };
    await render(idle);
    await act(async () => { await hooks!.deployment.confirmApply(); });
    expect(apiMock.deploymentWorkspace.applyDraft).toHaveBeenLastCalledWith(
      expect.objectContaining({ expectedAdmissionGeneration: 12 }),
      { token: digest('7'), preparation: { requestOwnerEpoch: 'owner', requestId: 'prep' } },
      undefined,
      correlationId,
      scope,
    );
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'awaiting_apply', apply: { applyId: 'apply-9', applyGeneration: 5 },
    });
    await render({
      ...idle,
      activation: { applyId: 'apply-9', applyGeneration: 5, state: 'succeeded', failure: null } as ManagerApplyStatus,
    });
    expect(hooks!.resetApply.sequence?.phase).toEqual({
      kind: 'applied', apply: { applyId: 'apply-9', applyGeneration: 5 },
    });
  });

  it('refuses a Deployment Apply when the Program Draft changed after confirmation', async () => {
    const edited = { ...HELD, programRevision: digest('a'), deploymentRevision: 2, deploymentContent: digest('e') };
    const render = await mount(edited);
    expect(await confirm('deployment')).toBeNull();
    const correlationId = transport.sendExecutionCommand.mock.calls[0][2] as string;
    // The Deployment Draft is unchanged, but the Program Draft it compiles with is not.
    await render({
      ...edited, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(correlationId),
      draftRevision: 4, programRevision: digest('b'),
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({
      kind: 'failed', stage: 'apply', unknown: false,
      message: expect.stringContaining('Draft changed after confirmation; review again'),
    });
    expect(apiMock.deploymentWorkspace.applyDraft).not.toHaveBeenCalled();
  });

  it('rechecks the settled Reset identity before Apply: a later cycle refuses it', async () => {
    const render = await mount(HELD);
    expect(await confirm('program')).toBeNull();
    const correlationId = transport.sendExecutionCommand.mock.calls[0][2] as string;
    // Idle reached under this correlation, but authoring admission is not yet open.
    const idle = { ...HELD, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(correlationId), admissionOpen: false };
    await render(idle);
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'applying', sent: false, idle: { cycleGeneration: 8 } });
    // Meanwhile another request moved the cycle on; admission then opens.
    await render({
      ...idle, admissionOpen: true, admissionGeneration: 12,
      cycle: { ...idleCycle('another-request'), cycleGeneration: 9 },
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({
      kind: 'failed', stage: 'apply', unknown: false,
      message: expect.stringContaining('cycle changed after Reset reached Idle'),
    });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('rechecks the runner owner before Apply: a restarted runner refuses it', async () => {
    const render = await mount(HELD);
    expect(await confirm('program')).toBeNull();
    const correlationId = transport.sendExecutionCommand.mock.calls[0][2] as string;
    const idle = { ...HELD, hold: 'idle' as const, admissionGeneration: 11, cycle: idleCycle(correlationId), admissionOpen: false };
    await render(idle);
    await flush();
    await render({
      ...idle, admissionOpen: true, admissionGeneration: 12,
      cycle: { ...idleCycle(correlationId), programRunnerOwnerEpoch: 'runner.epoch.2' },
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({
      kind: 'failed', stage: 'apply', message: expect.stringContaining('runner restarted after Reset settled'),
    });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('reconciles a lost Reset reply from observation: unknown, no Apply, no resend', async () => {
    transport.sendExecutionCommand.mockRejectedValue(
      new CommandDeliveryUnconfirmedError('Command delivery is unconfirmed; commands are disabled.'),
    );
    const render = await mount(HELD);
    expect(await confirm('program')).toBeNull();
    const phase = hooks!.resetApply.sequence!.phase;
    expect(phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'unconfirmed' });
    expect(resetApplyPhaseText(phase)).toContain('unknown — reconciling');
    const correlationId = transport.sendExecutionCommand.mock.calls[0][2] as string;
    // The resynchronized snapshot still shows the held run.
    await render({ ...HELD, admissionGeneration: 10 });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'awaiting_idle', delivery: 'unconfirmed' });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
    // The run ends under this correlation: settlement is proven by observation.
    apiMock.programWorkspace.apply.mockResolvedValue({
      status: 'applied', workspace: programWorkspace(HELD), headless: null,
    });
    await render({ ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(correlationId) });
    await flush();
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
    expect(hooks!.resetApply.sequence?.phase).toEqual({ kind: 'applied', apply: null });
    expect(transport.sendExecutionCommand).toHaveBeenCalledOnce();
  });

  it('never Applies on an Idle another request reached, nor a Draft changed meanwhile', async () => {
    const render = await mount(HELD);
    expect(await confirm('program')).toBeNull();
    await render({ ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle('someone-else') });
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'failed', stage: 'reset' });

    await act(async () => { hooks!.resetApply.dismiss(); });
    await render(HELD);
    expect(await confirm('program')).toBeNull();
    const correlationId = transport.sendExecutionCommand.mock.calls[1][2] as string;
    await render({
      ...HELD, hold: 'idle', admissionGeneration: 11, cycle: idleCycle(correlationId), draftRevision: 4,
    });
    await flush();
    expect(hooks!.resetApply.sequence?.phase).toMatchObject({ kind: 'failed', stage: 'apply', unknown: false });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('Resets a run Executed but never Run before Applying', async () => {
    await mount({ ...HELD, hold: 'never_run' });
    expect(hooks!.resetApply.unavailableReason('program')).toBeNull();
    expect(await confirm('program')).toBeNull();
    const [kind, request] = transport.sendExecutionCommand.mock.calls[0] as [
      string, Record<string, JsonValue>, string,
    ];
    expect(kind).toBe('program_reset');
    expect(request).toMatchObject({ program_run_id: 'run.1', expected_cycle_generation: 7 });
    expect(apiMock.programWorkspace.apply).not.toHaveBeenCalled();
  });

  it('is unavailable, with a reason, while motion is not stopped', async () => {
    await mount({ ...HELD, hold: 'stopping' });
    expect(hooks!.resetApply.unavailableReason('program')).toContain('all Program motion stopped');
    expect(await confirm('program')).toContain('all Program motion stopped');
    expect(transport.sendExecutionCommand).not.toHaveBeenCalled();
  });
});

describe('Applied view after an Apply settles', () => {
  it('re-reads both Applied views on the settlement event, so Execute is enabled', async () => {
    const programRun = [{ commandKind: 'program_run', semanticTarget: {}, allowed: true, disabledReason: null }];
    const before: Environment = {
      ...HELD, hold: 'idle', admissionGeneration: 11,
      cycle: { allowedCommands: programRun } as Partial<ProgramCycleStatus>,
    };
    const render = await mount(before);
    expect(hooks!.runtime.draftUnappliedReason).toMatch(/^Apply or Discard first/);
    expect(hooks!.runtime.commandAllowed('program_run')).toBe(false);
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'apply', null)).toBeNull();

    apiMock.programWorkspace.apply.mockResolvedValue({
      status: 'accepted', workspace: programWorkspace(before),
      headless: { apply_id: 'apply-3', apply_generation: 6 },
    });
    await act(async () => { await hooks!.program.apply(); });
    expect(apiMock.programWorkspace.apply).toHaveBeenCalledOnce();
    const appliedReads = apiMock.programs.applied.mock.calls.length;

    // The Manager published and settled the Apply. The owners now serve the
    // new Applied Program; the control snapshot's Applied basis is unchanged
    // (its earlier wake raced the readback), so only settlement can re-read.
    const after: Environment = {
      ...before, admissionGeneration: 12, appliedRevision: digest('f'),
      activation: { applyId: 'apply-3', applyGeneration: 6, state: 'succeeded', failure: null } as ManagerApplyStatus,
    };
    apiMock.programs.applied.mockResolvedValue({
      sourceOwnerEpoch: 'applied.1', sourceGeneration: 2, recordDigest: digest('q'),
      programRevision: digest('f'), program: heldProgram(), programBinding: null,
    });
    await render(after);
    await flush();
    await flush();
    expect(apiMock.programs.applied.mock.calls.length).toBeGreaterThan(appliedReads);
    expect(hooks!.runtime.state.applied?.programRevision).toBe(digest('f'));
    expect(hooks!.runtime.draftUnappliedReason).toBeNull();
    expect(hooks!.runtime.commandAllowed('program_run')).toBe(true);
    // Nothing is left to apply or discard.
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'apply', null))
      .toMatch(/^Nothing to apply/);
    expect(programActionUnavailableReason(hooks!.program.state.workspace, 'discard', null))
      .toMatch(/^Nothing to apply/);
  });
});

describe('Advisory preview of the failed node in the ordinary editor', () => {
  it('is no longer ready once the native execution owner is replaced', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      const authoring = heldAuthoring();
      authoring.forest[0].children[0].parameters.speed = 0.5;
      apiMock.programs.nodeCorrectionPreview.mockResolvedValueOnce({
        unavailableReason: null, target: null,
        preview: { ok: false, error: { code: 'ik_failed', message: 'unreachable' } },
      });
      const render = await mount({ ...HELD, authoring });
      await act(async () => { vi.advanceTimersByTime(1_000); });
      await flush();
      expect(hooks!.preview.preview?.status).toBe('ready');
      await render({ ...HELD, authoring, executionOwnerEpoch: 'execution.epoch.2' });
      expect(hooks!.preview.preview?.status).toBe('pending');
      await act(async () => { vi.advanceTimersByTime(1_000); });
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledTimes(2);
    } finally {
      vi.useRealTimers();
    }
  });

  it('previews the failed node’s Draft values against the captured Program', async () => {
    vi.useFakeTimers({ toFake: ['setTimeout', 'clearTimeout'] });
    try {
      const authoring = heldAuthoring();
      authoring.forest[0].children[0].parameters.speed = 0.5;
      const render = await mount({ ...HELD, authoring });
      expect(hooks!.preview.nodeInstanceId).toBe('move');
      expect(hooks!.preview.statusText).toBe('Previewing the edited motion…');
      await act(async () => { vi.advanceTimersByTime(1_000); });
      expect(apiMock.programs.nodeCorrectionPreview).toHaveBeenCalledExactlyOnceWith(
        expect.objectContaining({
          program_run_id: 'run.1', node_instance_id: 'move', node_attempt_id: 'move.attempt.1',
          proposed_inputs: [{ path: 'speed', value: 0.5 }],
        }),
      );
      // Unchanged Draft for that node: nothing to preview, no status line.
      await render({ ...HELD, authoring: heldAuthoring() });
      expect(hooks!.preview.statusText).toBeNull();
      expect(hooks!.preview.ghost).toBeNull();
    } finally {
      vi.useRealTimers();
    }
  });
});

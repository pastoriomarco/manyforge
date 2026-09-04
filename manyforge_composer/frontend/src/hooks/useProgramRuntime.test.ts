import { readFileSync } from 'node:fs';

import { afterEach, describe, expect, it, vi } from 'vitest';

import type {
  AppliedProgramSelection,
  ProgramRuntimeControlProjection,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
  ScenarioCatalogResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import {
  buildProgramResetRequest,
  buildProgramStopRequest,
  buildProgramFullStopResetRequest,
  buildProgramStartRequest,
  controlPendingAfterProjection,
  programCommandFailureMessage,
  programPendingAfterControlUpdate,
  programPendingAfterProjection,
  programRunProjectionKey,
  programRuntimeAfterSnapshot,
  programMotionStopSettled,
  programRuntimeWakeKey,
  programTerminalFailureMessage,
  retainedScenarioSelection,
  scenarioSelectionAfterRequest,
} from './useProgramRuntime';

afterEach(() => {
  vi.restoreAllMocks();
});

const digest = (character: string) => `sha256:${character.repeat(64)}`;

function admissionFixtures() {
  const applied = {
    scenarioId: 'applied',
    scenarioCatalogRevision: digest('a'),
    programRevision: digest('b'),
    recordDigest: digest('c'),
  } as AppliedProgramSelection;
  const workspace = {
    draft: {
      draftId: 'draft-1', draftRevision: 5, programLoaded: true,
      programRevision: digest('b'),
    },
  } as ProgramWorkspaceResponse;
  const snapshot = {
    authoringAdmission: { authoringAllowed: true, admissionGeneration: 9 },
    programCycle: {
      programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 3,
      state: 'idle_after_reset', programRunId: null,
      cycleClaimGeneration: null, terminalOrSettlementReference: null,
      runRevision: null, nodeStatusRevision: null,
      runtimeControlRevision: null, recoveryOffer: null,
      executionControlRevision: 1,
      causalCorrelationId: null, causalSessionId: null,
      causalClientSequence: null, currentCommandKind: null,
      failure: null, resetPhase: null, allowedCommands: [],
      run: null, runtimeControls: null,
    },
    draftIdentity: { draftId: 'draft-1', draftRevision: 5 },
  } as unknown as ComposerHeadlessState;
  return { applied, workspace, snapshot };
}

const catalog = {
  apiVersion: 'manyforge.scenario_catalog.v1',
  scenarioCatalogRevision: `sha256:${'a'.repeat(64)}`,
  scenarios: [
    {
      scenarioId: 'first',
      description: 'First scenario',
      aliases: [],
      deploymentArtifactId: 'deployment.first',
      deploymentRevision: `sha256:${'b'.repeat(64)}`,
      retainedBundleId: `sha256:${'c'.repeat(64)}`,
      programBindingId: `sha256:${'d'.repeat(64)}`,
      assistantMode: 'optional',
    },
    {
      scenarioId: 'applied',
      description: 'Applied scenario',
      aliases: [],
      deploymentArtifactId: 'deployment.applied',
      deploymentRevision: `sha256:${'e'.repeat(64)}`,
      retainedBundleId: `sha256:${'f'.repeat(64)}`,
      programBindingId: `sha256:${'1'.repeat(64)}`,
      assistantMode: 'forbidden',
    },
  ],
} as ScenarioCatalogResponse;

describe('Program runtime idle selection', () => {
  it('uses semantic owner revisions instead of a fixed browser poll', () => {
    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    expect(source).not.toContain('setInterval');
    const snapshot = {
      receiverEpoch: 'receiver.1',
      runtime: {
        sections: {
          manager: { sourceOwnerEpoch: 'manager.1', sourceGeneration: 4 },
        },
      },
      componentOverview: {
        basis: {
          appliedProgramOwnerEpoch: 'applied.1',
          appliedProgramGeneration: 3,
        },
      },
      authoringAdmission: { admissionGeneration: 7 },
      programCycle: {
        programRunnerOwnerEpoch: 'runner.1',
        cycleGeneration: 2,
        state: 'idle_after_reset',
        programRunId: null,
        runRevision: null,
        nodeStatusRevision: null,
        runtimeControlRevision: null,
        recoveryOffer: null,
      },
      draftIdentity: { draftId: 'draft-1', draftRevision: 5 },
    } as ComposerHeadlessState;
    expect(programRuntimeWakeKey(snapshot)).toContain(
      'manager.1:no-retained-bundle:no-graph-revision:applied.1:3',
    );
    expect(programRuntimeWakeKey({
      ...snapshot,
      runtime: {
        ...snapshot.runtime,
        sections: {
          ...snapshot.runtime.sections,
          manager: { ...snapshot.runtime.sections.manager, sourceGeneration: 99 },
        },
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        runRevision: 2,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(source).not.toContain('PROGRAM_RUN_OBSERVATION_POLL_MS');
    expect(source).not.toContain('api.programs.read(activeRunId');
    expect(programRuntimeWakeKey({
      ...snapshot,
      draftIdentity: { draftId: 'draft-1', draftRevision: 6 },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        cycleGeneration: 99,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      authoringAdmission: {
        ...snapshot.authoringAdmission,
        admissionGeneration: 8,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      componentOverview: {
        ...snapshot.componentOverview,
        basis: {
          ...snapshot.componentOverview.basis,
          appliedProgramGeneration: 4,
        },
      },
    })).not.toBe(programRuntimeWakeKey(snapshot));
    expect(programRuntimeWakeKey({
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        runtimeControlRevision: 2,
      },
    })).toBe(programRuntimeWakeKey(snapshot));
  });

  it('does not derive operator selection from the catalog or Applied projection', () => {
    expect(retainedScenarioSelection(null, catalog)).toBeNull();

    const source = readFileSync(new URL('./useProgramRuntime.ts', import.meta.url), 'utf8');
    const refresh = source.slice(
      source.indexOf('const refresh = useCallback'),
      source.indexOf('useEffect(() =>', source.indexOf('const refresh = useCallback')),
    );
    expect(refresh).not.toContain('api.programs.start');
    expect(refresh).not.toContain('applyDraft');
    expect(refresh).toContain('api.programs.currentScenario()');
    expect(refresh).not.toContain('api.programs.runs(');
    expect(refresh).not.toContain('api.programs.runtimeControls(');
    expect(source).toContain('programRuntimeAfterSnapshot(');
    expect(source).toContain('const pending = programPendingAfterControlUpdate(');
  });

  it('retains only a still-available explicit operator selection', () => {
    expect(retainedScenarioSelection('applied', catalog)).toBe('applied');
    expect(retainedScenarioSelection('removed', catalog)).toBeNull();
  });

  it('selects and then explicitly clears without substituting another scenario', () => {
    const selected = scenarioSelectionAfterRequest(null, catalog, 'applied');
    expect(selected).toBe('applied');
    expect(scenarioSelectionAfterRequest(selected, catalog, null)).toBeNull();
  });
});

describe('Program command settlement', () => {
  it('keeps sending state until delivery and owner projection are both current', () => {
    const runningCycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'running',
      programRunId: 'run-1',
      cycleClaimGeneration: 8,
    } as const;
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;

    expect(programPendingAfterControlUpdate(
      'run', false, run, null, runningCycle,
    )).toBe('run');
    expect(programPendingAfterControlUpdate(
      'run', true, run, null, runningCycle,
    )).toBeNull();
  });

  it('addresses Program Stop from the authoritative cycle while Start is pending', () => {
    const { snapshot } = admissionFixtures();
    const starting = {
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        state: 'starting' as const,
        programRunId: 'program-cycle.7',
        allowedCommands: [{
          commandKind: 'program_stop',
          semanticTarget: {},
          allowed: true,
          disabledReason: null,
        }],
      },
    };
    expect(buildProgramStopRequest(
      starting, 'composer.epoch.1', 'request.stop.1',
    )).toMatchObject({
      program_run_id: 'program-cycle.7',
      expected_owner_epoch: 'runner.epoch.1',
    });
    expect(buildProgramStopRequest(
      {
        ...starting,
        programCycle: {
          ...starting.programCycle,
          allowedCommands: [],
        },
      },
      'composer.epoch.1',
      'request.stop.2',
    )).toBeNull();
  });

  it('does not discard a newer node-status projection within one run revision', () => {
    const current = {
      ownerEpoch: 'runner.epoch.1',
      runRevision: 5,
      nodeStatusRevision: 8,
      capture: { programRunId: 'run-1' },
      nodeStatuses: { first: 'running', second: 'idle' },
    } as unknown as ProgramRunStatus;
    const advanced = {
      ...current,
      nodeStatusRevision: 9,
      nodeStatuses: { first: 'success', second: 'running' },
    } as unknown as ProgramRunStatus;

    expect(programRunProjectionKey(advanced)).not.toBe(
      programRunProjectionKey(current),
    );
    expect(programRunProjectionKey({ ...advanced })).toBe(
      programRunProjectionKey(advanced),
    );
  });

  it('never lets an older shared snapshot undo a newer command response', () => {
    const currentRun = {
      ownerEpoch: 'runner.epoch.1',
      runRevision: 5,
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;
    const currentControls = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      runRevision: 5,
      runtimeControls: { runtimeControlRevision: 7 },
    } as ProgramRuntimeControlProjection;
    const olderRun = { ...currentRun, runRevision: 4 };
    const cycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 3,
      state: 'running',
      programRunId: 'run-1',
      runRevision: 4,
      nodeStatusRevision: 2,
      runtimeControlRevision: 6,
      run: olderRun,
      runtimeControls: {
        ...currentControls,
        runRevision: 4,
        runtimeControls: { runtimeControlRevision: 6 },
      },
    } as Parameters<typeof programRuntimeAfterSnapshot>[2];

    expect(programRuntimeAfterSnapshot(
      currentRun,
      currentControls,
      cycle,
    )).toEqual({ run: currentRun, controls: currentControls });
    expect(programRuntimeAfterSnapshot(
      currentRun,
      currentControls,
      { ...cycle, programRunId: null, run: null, runtimeControls: null },
    )).toEqual({ run: null, controls: null });
  });

  it('keeps exact request and current owner projections in command failures', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'intervention_required',
      runRevision: 7,
      treeStatus: 'running',
      capture: { programRunId: 'run-1' },
      nodeStatuses: { 'node.move': 'failure' },
      activeOperationCount: 0,
      reasonCode: 'operation_stopped',
      message: 'The exact operation stopped before motion began.',
    } as unknown as ProgramRunStatus;
    const controls = {
      recoveryOffer: {
        recoveryClass: 'stop_fault_intervention',
        status: 'reset_required',
        offerRevision: 3,
        nodeInstanceId: 'node.move',
        operationId: 'operation-1',
        reasonCode: 'active_stop_fault_requires_reset',
        actions: [{ actionId: 'reset_cycle', enabled: true }],
      },
    } as Parameters<typeof programCommandFailureMessage>[4];
    const { snapshot } = admissionFixtures();
    const message = programCommandFailureMessage(
      new Error('new motion admission is closed'),
      'Run failed.',
      'program-run-start.fixture',
      run,
      controls,
      {
        ...snapshot,
        programCycle: {
          ...snapshot.programCycle,
          state: 'running',
          cycleGeneration: 4,
        },
      },
    );
    expect(message).toContain('Request program-run-start.fixture.');
    expect(message).toContain('run-1: intervention_required, revision 7');
    expect(message).toContain('cycle: running, generation 4');
    expect(message).toContain(
      'recovery stop_fault_intervention: reset_required, offer revision 3',
    );
  });

  it('turns an asynchronously observed terminal failure into one detailed error', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'failed',
      runRevision: 11,
      treeStatus: 'failure',
      capture: { programRunId: 'run-background-failure' },
      nodeStatuses: { 'node.move': 'failure' },
      activeOperationCount: 0,
      reasonCode: 'owner_request_failed',
      message: 'planning failed after the Run request had returned',
    } as unknown as ProgramRunStatus;
    const message = programTerminalFailureMessage(run, null, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 12,
      state: 'terminal_awaiting_reset',
      programRunId: 'run-background-failure',
      cycleClaimGeneration: 22,
    });
    expect(message).toContain('Program execution failed.');
    expect(message).toContain('run-background-failure: failed, revision 11');
    expect(message).toContain('Run reason: owner_request_failed.');
    expect(message).toContain('Current nodes: node.move=failure.');
    expect(programTerminalFailureMessage({ ...run, state: 'running' }, null, null))
      .toBeNull();
  });

  it('keeps Motion Stop pending through acceptance and clears it only on exact evidence', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;
    const settled = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      recoveryOffer: {
        programRunId: 'run-1',
        recoveryClass: 'ordinary_operator_stop',
        status: 'retry_ready',
        stopId: 'stop-1',
        stopGeneration: 1,
        stopCertificateId: 'certificate-1',
        stopCertificateRevision: 1,
      },
    } as unknown as NonNullable<
      Parameters<typeof programPendingAfterProjection>[2]
    >;

    expect(programPendingAfterProjection('motion-stop', run, null)).toBe(
      'motion-stop',
    );
    expect(programPendingAfterProjection(
      'motion-stop',
      run,
      { ...settled, ownerEpoch: 'runner.epoch.stale' },
    )).toBe('motion-stop');
    expect(programPendingAfterProjection('motion-stop', run, settled)).toBeNull();
    expect(programPendingAfterProjection(
      'motion-stop',
      { ...run, state: 'stopped' },
      null,
    )).toBeNull();
    expect(programPendingAfterProjection('run', run, settled)).toBe('run');
    expect(programPendingAfterProjection('run', run, settled, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'running',
      programRunId: 'run-1',
      cycleClaimGeneration: 8,
    })).toBeNull();
    expect(programPendingAfterProjection('stop', null, null, {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 4,
      state: 'terminal_awaiting_reset',
      programRunId: 'run-1',
      cycleClaimGeneration: null,
    })).toBeNull();
    const idleCycle = {
      programRunnerOwnerEpoch: 'runner.epoch.1',
      cycleGeneration: 5,
      state: 'idle_after_reset',
      programRunId: null,
      cycleClaimGeneration: null,
      admissionGeneration: 12,
      authoringAllowed: true,
    } as const;
    expect(programPendingAfterProjection(
      'reset', null, null, idleCycle,
    )).toBeNull();
    expect(programPendingAfterProjection(
      'reset', null, null,
      { ...idleCycle, authoringAllowed: false },
    )).toBe('reset');

    const resetOnly = {
      ...settled,
      recoveryOffer: {
        ...settled.recoveryOffer,
        recoveryClass: 'stop_fault_intervention',
        status: 'reset_required',
      },
    } as unknown as NonNullable<
      Parameters<typeof programMotionStopSettled>[1]
    >;
    expect(programMotionStopSettled(run, resetOnly)).toBe(true);
    expect(programPendingAfterProjection('motion-stop', run, resetOnly)).toBeNull();
  });

  it('keeps Resume disabled until the exact recovery offer changes', () => {
    const run = {
      ownerEpoch: 'runner.epoch.1',
      state: 'running',
      capture: { programRunId: 'run-1' },
    } as unknown as ProgramRunStatus;
    const retainedOffer = {
      ownerEpoch: 'runner.epoch.1',
      programRunId: 'run-1',
      recoveryOffer: { offerId: 'offer-1', offerRevision: 3 },
    } as unknown as NonNullable<
      Parameters<typeof controlPendingAfterProjection>[1]
    >;
    expect(controlPendingAfterProjection(
      'recovery:replan_from_stopped_state',
      retainedOffer,
      retainedOffer,
      run,
    )).toBe('recovery:replan_from_stopped_state');
    expect(controlPendingAfterProjection(
      'recovery:replan_from_stopped_state',
      retainedOffer,
      { ...retainedOffer, recoveryOffer: null },
      run,
    )).toBeNull();
    expect(controlPendingAfterProjection(
      'override:activate:one',
      retainedOffer,
      retainedOffer,
      run,
    )).toBe('override:activate:one');
  });
});

describe('Program runtime authority requests', () => {
  it('starts only from one complete Idle snapshot and carries exact fences', () => {
    const { applied, workspace, snapshot } = admissionFixtures();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toEqual({
      request_id: 'program-run-start.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'program-run-start.fixture',
      scenario_id: 'applied',
      scenario_catalog_revision: digest('a'),
      expected_applied_record_digest: digest('c'),
      draft_program_revision: digest('b'),
      expected_admission_generation: 9,
    });

    expect(buildProgramStartRequest(
      applied, true, null, null, workspace,
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null,
      { ...snapshot, programCycle: { ...snapshot.programCycle, state: 'running' } },
      workspace, 'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
    expect(buildProgramStartRequest(
      applied, true, null, snapshot,
      { ...workspace, draft: { ...workspace.draft, draftRevision: 6 } },
      'composer-program-owner.fixture', 'program-run-start.fixture',
    )).toBeNull();
  });

  it('resets only the exact terminal cycle and reuses owner terminal evidence', () => {
    const { snapshot } = admissionFixtures();
    const terminalReference = digest('d');
    const terminalSnapshot = {
      ...snapshot,
      programCycle: {
        programRunnerOwnerEpoch: 'runner.epoch.1', cycleGeneration: 4,
        state: 'terminal_awaiting_reset' as const,
        programRunId: 'run-1', cycleClaimGeneration: 12,
        terminalOrSettlementReference: terminalReference,
        runRevision: null, nodeStatusRevision: null,
        runtimeControlRevision: null, recoveryOffer: null,
        executionControlRevision: 1,
        causalCorrelationId: null, causalSessionId: null,
        causalClientSequence: null, currentCommandKind: null,
        failure: null, resetPhase: null, allowedCommands: [],
        run: null, runtimeControls: null,
      },
    };
    expect(buildProgramResetRequest(
      terminalSnapshot, 'composer-program-owner.fixture', 'reset.fixture',
    )).toEqual({
      request_id: 'reset.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'reset.fixture',
      program_run_id: 'run-1',
      expected_owner_epoch: 'runner.epoch.1',
      expected_cycle_generation: 4,
      expected_claim_generation: 12,
      terminal_reference: terminalReference,
      settlement_evidence_digest: terminalReference,
    });
    expect(buildProgramResetRequest(
      {
        ...terminalSnapshot,
        programCycle: {
          ...terminalSnapshot.programCycle,
          terminalOrSettlementReference: null,
        },
      },
      'composer-program-owner.fixture', 'reset.fixture',
    )).toBeNull();
  });

  it('resets a Full-stopped tree with exact run and cycle fences', () => {
    const { snapshot } = admissionFixtures();
    const fullStoppedSnapshot = {
      ...snapshot,
      programCycle: {
        ...snapshot.programCycle,
        state: 'running' as const,
        programRunId: 'run-1',
        cycleClaimGeneration: 12,
      },
    };
    const run = {
      state: 'full_stopped',
      ownerEpoch: 'runner.epoch.1',
      runRevision: 9,
      capture: { programRunId: 'run-1' },
    } as ProgramRunStatus;

    expect(buildProgramFullStopResetRequest(
      fullStoppedSnapshot,
      run,
      'composer-program-owner.fixture',
      'reset-full-stop.fixture',
    )).toEqual({
      request_id: 'reset-full-stop.fixture',
      request_owner_epoch: 'composer-program-owner.fixture',
      source_label: 'operator_ui',
      correlation_id: 'reset-full-stop.fixture',
      program_run_id: 'run-1',
      expected_owner_epoch: 'runner.epoch.1',
      expected_run_revision: 9,
      expected_cycle_generation: 3,
      expected_claim_generation: 12,
      reason: 'operator Reset discarded Full Stop',
    });
    expect(buildProgramFullStopResetRequest(
      fullStoppedSnapshot,
      { ...run, state: 'running' },
      'composer-program-owner.fixture',
      'reset-full-stop.fixture',
    )).toBeNull();
  });
});

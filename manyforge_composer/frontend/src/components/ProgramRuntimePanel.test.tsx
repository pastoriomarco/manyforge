import { readFileSync } from 'node:fs';
import { renderToStaticMarkup } from 'react-dom/server';
import { afterEach, describe, expect, it, vi } from 'vitest';

import { ASSISTANT_TURN_IN_PROGRESS, type ProgramRuntimeController } from '../hooks/useProgramRuntime';
import type { RobotControlController } from '../hooks/useRobotControl';
import type {
  ProgramBindingDocument,
  ProgramRunStatus,
  AutomaticRetryAuditEvent,
  TakeoverFallbackAuditEvent,
  CycleRecoveryOffer,
  ProgramRunStateName,
  RuntimeControlAuditEvent,
  StructuredError,
} from '../types/api';
import type {
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from '../types/robotControl';
import { statusFixture } from '../testFixtures/robotControl';
import { executionGroupScopes } from '../state/programExecutionGroups';
import {
  deriveTransportGuidance,
  MotionControlSettings,
  ProgramRuntimePanel,
  ProgramTransportControls,
  automaticRetryFailureText,
  automaticRetryText,
  takeoverFallbackText,
  ioSourceAuditText,
  ioSourceUnavailableNodes,
  simulationHeldNodes,
  simulationRefusalAuditText,
  recoveryClassLabel,
  resetKeptMessage,
  selectManualRecoveryTarget,
} from './ProgramRuntimePanel';
import { readStyleSource } from '../styles/readStyleSource';
import { formatStructuredError } from '../state/structuredError';
import { api } from '../api/client';
import { initialSceneNotice } from '../state/initialSceneNotice';
import type { ActivateExecuteController } from '../hooks/useActivateExecute';
import { ActivateExecuteDialog } from './ActivateExecuteDialog';

// Lets a test turn Halt confirmation off; null keeps the installation value.
const composerSettingsOverride = vi.hoisted(() => ({ confirmHalt: null as boolean | null }));
vi.mock('../hooks/useComposerSettings', async (original) => {
  const actual = await original<typeof import('../hooks/useComposerSettings')>();
  return {
    ...actual,
    useComposerSettings: () => {
      const controller = actual.useComposerSettings();
      const { confirmHalt } = composerSettingsOverride;
      return confirmHalt === null ? controller : {
        ...controller,
        settings: {
          ...controller.settings,
          motionPanel: { ...controller.settings.motionPanel, confirmHalt },
        },
      };
    },
  };
});

/** A minimal Applied binding row for one direct `component/target` reference. */
function targetBinding(
  componentId: string,
  commandTargetId: string,
): ProgramBindingDocument['target_bindings'][number] {
  return {
    target: { component_id: componentId, command_target_id: commandTargetId },
    frame_references: [], frame_associations: [], collision_body_references: [],
    signal_references: [], resource_role_bindings: [], capability_bindings: [],
  };
}

function ordinaryMotionStopOffer(): CycleRecoveryOffer {
  return {
    schemaVersion: 'manyforge.cycle_recovery_offer.v5',
    offerId: `sha256:${'e'.repeat(64)}`,
    offerRevision: 1,
    generatedAtNs: 1,
    kernelAssessmentRevision: 1,
    controlCommandEpoch: 1,
    operationId: 'operation.1',
    operationRevision: 1,
    programRunId: 'program-run.1',
    programBindingId: `sha256:${'c'.repeat(64)}`,
    nodeInstanceId: 'root',
    nodeAttemptId: 'attempt.1',
    participants: [],
    graphRevision: `sha256:${'b'.repeat(64)}`,
    retainedBundleId: `sha256:${'a'.repeat(64)}`,
    cycleGeneration: 1,
    programGeneration: 1,
    deploymentGeneration: 1,
    sceneAdmissionEpoch: 1,
    poseSourceApplyGeneration: 1,
    stopId: 'stop.1',
    stopGeneration: 1,
    stopCertificateId: 'stop-certificate.1',
    stopCertificateRevision: 1,
    status: 'retry_ready',
    recoveryClass: 'ordinary_operator_stop',
    actions: [{
      actionId: 'replan_from_stopped_state',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: true,
      disabledReasonCode: null,
    }, {
      actionId: 'reset_cycle',
      requiresConfirmation: true,
      performsBackendReset: true,
      enabled: true,
      disabledReasonCode: null,
    }],
    manualResult: {
      enabled: false,
      disabledReasonCode: 'manual_result_not_available',
    },
    reason: {
      code: 'ordinary_operator_stop',
      message: 'Stopped by the operator.',
      details: null,
      origin: null,
    },
    evidenceRefs: [],
    policy: { allowIntervention: false, maxRetries: 0 },
    retriesUsed: 0,
  };
}

function ordinaryMotionStopControls(): NonNullable<
  ProgramRuntimeController['state']['controls']
> {
  return {
    ownerEpoch: 'program-runner.epoch.1',
    programRunId: 'program-run.1',
    recoveryOffer: ordinaryMotionStopOffer(),
    // An operator Stop is not a failure hold.
    recoveryHolds: [],
    executionSelections: {
      selectionRevision: 1,
      motionHeld: true,
      groups: [{ groupId: 'all', nodeCount: 1, heldNodeCount: 1,
        state: 'held', stopAllowed: false,
        startAllowed: false, startUnavailableReason: 'selection_not_ready',
        resumeAllowed: true, resumeUnavailableReason: null,
        runtimeScaleSelector: {
          kind: 'singular',
          programBindingId: `sha256:${'c'.repeat(64)}`,
          componentId: 'robot',
          commandTargetId: 'arm',
        },
        runtimeScaleUnavailableReason: null,
        resourceIds: ['robot.joint_1'] }],
    },
  } as unknown as NonNullable<ProgramRuntimeController['state']['controls']>;
}

function robotController(): RobotControlController {
  return {
    currentStatus: statusFixture(),
    manualControlAllowed: true,
    manualControlUnavailableReason: null,
    state: {
      phase: 'ready',
      operation: null,
      status: null,
    },
    controlPending: null,
    runtimeScaleStatus: null,
    runtimeScaleError: null,
    runtimeScaleDraft: 1,
    runtimeScalePending: false,
    resume: vi.fn(),
    setRuntimeScale: vi.fn(),
    commitRuntimeScale: vi.fn(),
  } as unknown as RobotControlController;
}

function runtimeScaleStatus(
  selector: RuntimeScaleControlSelector,
  requestedScale = 0.5,
): RuntimeScaleControlStatus {
  return {
    selector,
    controlGeneration: 2,
    requestedScale,
    supported: true,
    frozen: false,
    controlDomainId: 'domain-1',
    controlDomainRevision: `sha256:${'d'.repeat(64)}`,
    controlBindingId: 'binding-1',
    controlBindingRevision: `sha256:${'e'.repeat(64)}`,
    scalingDomainId: 'scaling-1',
    activeControlConfigurationId: 'active-1',
    activeControlConfigurationGeneration: 1,
    activeOperationId: null,
    activeOperationRevision: null,
    activeOperationScaleGeneration: null,
    effectiveScale: null,
    applicationState: null,
    reason: null,
  };
}

function controller(
  state: ProgramRunStateName = 'running',
  cycleState: ProgramRuntimeController['cycleState'] =
    state === 'succeeded' ? 'terminal_awaiting_reset' : 'running',
): ProgramRuntimeController {
  return {
    runnerAllows: vi.fn(() => true),
    runnerRefusal: vi.fn(() => null),
    // As the backend computes it: Execute only from an idle
    // cycle, Resume for a run in progress (a held group or a halted run).
    readiness: cycleState === 'idle_after_reset'
      ? { can_execute: true, can_resume: false, motion_clear: true, blocked_reasons: [] }
      : { can_execute: false,
        can_resume: ['running', 'intervention_required', 'full_stopped'].includes(state),
        motion_clear: true,
        blocked_reasons: [{ code: 'run_in_progress', message: 'A run is in progress.' }] },
    cellLatch: null,
    composerRefusalReason: null,
    state: {
      currentScenario: null,
      applied: {
        schemaVersion: 'manyforge.applied_program_selection.v4',
        scenarioId: 'cell-pick',
        projectPath: '/projects/cell-pick.scenario',
        sourceDocumentDigest: `sha256:${'a'.repeat(64)}`,
        programId: 'next-pick',
        programRevision: `sha256:${'b'.repeat(64)}`,
        programBindingId: `sha256:${'c'.repeat(64)}`,
        retainedBundleId: `sha256:${'a'.repeat(64)}`,
        graphRevision: `sha256:${'b'.repeat(64)}`,
        nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
        recordDigest: `sha256:${'a'.repeat(64)}`,
        sourceOwnerId: 'headless.program-apply',
        sourceOwnerEpoch: 'headless.epoch.1',
        sourceGeneration: 1,
        assistantProjection: null,
        ownerIdentities: [{
          ownerRole: 'headless_projection',
          ownerId: 'headless.program-apply',
          ownerEpoch: 'headless.epoch.1',
        }],
        program: {
          api_version: 'manyforge.program.v3',
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          program_profile: 'composable-cell-v1',
          name: 'Next pick',
          catalog: {
            api_version: 'manyforge.node_catalog.v3',
            content_version: '3.0.0',
          },
          tree: {
            node_id: 'logic.check_condition',
            instance_id: 'root',
            parameters: { condition: true },
            children: [],
          },
        },
        programBinding: {
          api_version: 'manyforge.program_binding.v3',
          program_binding_id: `sha256:${'c'.repeat(64)}`,
          program_id: 'next-pick',
          program_revision: `sha256:${'b'.repeat(64)}`,
          retained_bundle_id: `sha256:${'a'.repeat(64)}`,
          graph_revision: `sha256:${'b'.repeat(64)}`,
          node_environment: {
            catalog_id: 'manyforge.default',
            content_version: '3.0.0',
            catalog_digest: `sha256:${'a'.repeat(64)}`,
            environment_digest: `sha256:${'c'.repeat(64)}`,
          },
          component_bindings: [],
          target_bindings: [],
          frame_bindings: [],
          collision_body_bindings: [],
          signal_bindings: [],
          node_policies: [],
          contact_grasp_bindings: [],
          provenance: {
            binding_compiler_contract_digest: `sha256:${'a'.repeat(64)}`,
            source_records: [],
            resolved_output_records: [],
          },
        },
        programFrameFacts: {
          appliedRecordDigest: `sha256:${'a'.repeat(64)}`,
          programBindingId: `sha256:${'c'.repeat(64)}`,
          programRevision: `sha256:${'b'.repeat(64)}`,
          retainedBundleId: `sha256:${'a'.repeat(64)}`,
          graphRevision: `sha256:${'b'.repeat(64)}`,
          components: [],
          frames: [],
        },
      },
      run: {
        schemaVersion: 'manyforge.program_run_status.v4',
        ownerId: 'program-runner.cell-a',
        ownerEpoch: 'program-runner.epoch.1',
        runRevision: 4,
        state,
        capture: {
          programRunId: 'program-run.1',
          appliedRecordDigest: `sha256:${'d'.repeat(64)}`,
          scenarioId: 'cell-pick',
          sourceDocumentDigest: `sha256:${'a'.repeat(64)}`,
          programId: 'pick',
          programRevision: `sha256:${'b'.repeat(64)}`,
          programBindingId: `sha256:${'c'.repeat(64)}`,
          retainedBundleId: `sha256:${'a'.repeat(64)}`,
          graphRevision: `sha256:${'b'.repeat(64)}`,
          nodeEnvironmentDigest: `sha256:${'c'.repeat(64)}`,
          nodePolicies: [],
          nodePolicyDigest: 'sha256:4f53cda18c2baa0c0354bb5f9a3ecbe5ed12ab4d8e11ba873c2f11161202b945',
          ownerIdentities: [{
            ownerRole: 'program_runner',
            ownerId: 'program-runner.cell-a',
            ownerEpoch: 'program-runner.epoch.1',
          }],
        },
        treeStatus: 'running',
        nodeStatusRevision: 2,
        nodeStatuses: { root: 'running' },
        activeOperationCount: 1,
        operationFailures: [],
        reason: null,
        message: '',
        runEnd: null,
      },
      controls: null,
      nonExecutable: null,
      runnerAvailable: true,
      loading: false,
      observationError: null,
      pending: null,
      selectionPending: [],
      controlPending: null,
      error: null,
      errorRefusal: null,
    },
    cycleState,
    cycleFailure: null,
    resetRobotPositionAvailability: { available: true, reason: null },
    active: [
      'running', 'intervention_required', 'full_stopping', 'full_stopped',
      'stopping', 'finalizing',
    ]
      .includes(state),
    refresh: vi.fn(),
    dismissError: vi.fn(),
    run: vi.fn(),
    activation: { available: false, flow: null, recoveryReady: true, states: [], open: vi.fn(), confirm: vi.fn(), cancel: vi.fn() },
    stopMotion: vi.fn(),
    startMotion: vi.fn(),
    resumeMotion: vi.fn(),
    fullStop: vi.fn(),
    resumeFullStop: vi.fn(),
    reset: vi.fn(),
    applyRecoveryAction: vi.fn(),
    writeRuntimeBlackboard: vi.fn(),
    persistentValues: { read: vi.fn(), edit: vi.fn(), clear: vi.fn() },
    stageManualResult: vi.fn(),
    groupRuntimeScales: {},
    setGroupRuntimeScale: vi.fn(),
    commitGroupRuntimeScale: vi.fn(),
  };
}

describe('Program runtime projection', () => {
  it('shows the Scenario origin of a no-Program publication', () => {
    const view = controller();
    view.state.applied = null;
    view.state.run = null;
    view.state.nonExecutable = {
      deploymentArtifactId: 'deployment',
      deploymentRevision: `sha256:${'a'.repeat(64)}`,
      executable: false,
      programId: null,
      programRevision: null,
      origin: {
        kind: 'scenario',
        scenarioId: 'authored-cell',
        sourceDocumentDigest: `sha256:${'e'.repeat(64)}`,
        projectPath: '/projects/authored-cell.scenario',
      },
      message: null,
      unresolvedReferences: [],
      affectedNodes: [],
      affectedExecutionGroups: [],
    };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={view} />))
      .toContain('authored-cell · no Program');
  });

  it('names each automatic retry in the runtime-control events', () => {
    const retry = (attempt: number, cause: string, eventRevision: number): AutomaticRetryAuditEvent => ({
      eventRevision,
      kind: 'program_node_automatic_retry',
      facts: {
        nodeInstanceId: 'move', attempt, maxAttempts: 4, cause, delayS: 1, failure: null,
      },
    });
    const collision = retry(2, 'collision_stop_nominal', 2);
    const planning = retry(3, 'planning_no_effect', 3);
    expect(automaticRetryText(collision)).toBe('Attempt 2 of 4 for move after collision Stop');
    expect(automaticRetryText(planning)).toBe('Attempt 3 of 4 for move after planning failed');
    expect(automaticRetryText({ eventRevision: 4, kind: 'program_node_retry', facts: {} })).toBeNull();
    const base = controller();
    const audit = [collision, planning];
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const withAudit: ProgramRuntimeController = {
      ...base,
      state: {
        ...base.state,
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: { audit } as unknown as RuntimeControls,
        },
      },
    };
    const markup = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={withAudit} />);
    expect(markup).toContain('Attempt 2 of 4 for move after collision Stop');
    expect(markup).toContain('Attempt 3 of 4 for move after planning failed');
  });

  it('names each takeover fallback in the runtime-control events', () => {
    const fallback: TakeoverFallbackAuditEvent = {
      eventRevision: 2,
      kind: 'program_node_takeover_fallback',
      facts: {
        nodeInstanceId: 'move', incumbentOperationId: 'op.1', path: 'ordinary_containment',
        code: 'takeover_preparation_late', message: 'late', failureStage: null,
        preparationMs: 12.4, budgetMs: 30, planningCapMs: 24,
      },
    };
    expect(takeoverFallbackText(fallback))
      .toBe('Stop fallback on move · takeover preparation late (planning cap 24 ms · prep 12 of 30 ms)');
    expect(takeoverFallbackText({ eventRevision: 3, kind: 'program_node_retry', facts: {} })).toBeNull();
    expect(takeoverFallbackText({
      ...fallback,
      facts: { ...fallback.facts, code: null, message: null, failureStage: 'stop_planning' },
    })).toBe('Stop fallback on move · during stop_planning (planning cap 24 ms · prep 12 of 30 ms)');
    const base = controller();
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const withAudit: ProgramRuntimeController = {
      ...base,
      state: {
        ...base.state,
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: { audit: [fallback] } as unknown as RuntimeControls,
        },
      },
    };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={withAudit} />))
      .toContain('Stop fallback on move · takeover preparation late (planning cap 24 ms · prep 12 of 30 ms)');
  });

  it('shows a running io wait whose input source is unavailable in amber, and both transitions as events', () => {
    const io = (eventRevision: number, available: boolean, node = 'wait_part'): RuntimeControlAuditEvent => ({
      eventRevision,
      kind: available ? 'program_node_io_source_available' : 'program_node_io_source_unavailable',
      facts: { node_instance_id: node, node_attempt_id: `${node}.1`, quality: available ? 'fresh' : 'stale' },
    });
    expect(ioSourceAuditText(io(2, false))).toBe('Input source unavailable for wait_part (stale) · still waiting');
    expect(ioSourceAuditText(io(3, true))).toBe('Input source available again for wait_part');
    expect(ioSourceAuditText({ eventRevision: 4, kind: 'program_node_retry', facts: {} })).toBeNull();
    // Only the newest transition per node counts, and only while the node runs.
    const running = { wait_part: 'running', other: 'running', done: 'success' } as const;
    expect(ioSourceUnavailableNodes([io(2, false), io(3, true)], running)).toEqual([]);
    expect(ioSourceUnavailableNodes([io(2, false, 'done')], running)).toEqual([]);
    expect(ioSourceUnavailableNodes([io(2, true), io(3, false)], running))
      .toEqual([{ unavailable: true, nodeInstanceId: 'wait_part', quality: 'stale' }]);

    const base = controller();
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const view = (audit: RuntimeControlAuditEvent[]): ProgramRuntimeController => ({
      ...base,
      state: {
        ...base.state,
        run: { ...base.state.run!, nodeStatuses: { root: 'running', wait_part: 'running' } },
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: { audit } as unknown as RuntimeControls,
        },
      },
    });
    const waiting = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={view([io(2, false)])} />);
    expect(waiting).toMatch(/class="inline-message severity-warning" role="status">Waiting on wait_part: input source unavailable \(stale\)/);
    expect(waiting).toContain('<span class="inline-message severity-warning">Input source unavailable for wait_part (stale) · still waiting</span>');
    const recovered = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={view([io(2, false), io(3, true)])} />,
    );
    expect(recovered).not.toContain('Waiting on wait_part');
    expect(recovered).toContain('Input source available again for wait_part');
  });

  it('shows a node a simulation-not-playing refusal stopped as waiting for Run (SIM-1)', () => {
    const refusal: RuntimeControlAuditEvent = {
      eventRevision: 2,
      kind: 'program_node_simulation_not_playing',
      facts: { node_instance_id: 'move', code: 'simulation_not_playing', message: 'the simulation is not playing' },
    };
    expect(simulationRefusalAuditText(refusal)).toBe('Simulation not playing on move · stopped, waiting for Run');
    expect(simulationRefusalAuditText({ eventRevision: 3, kind: 'program_node_retry', facts: {} })).toBeNull();
    expect(simulationHeldNodes([refusal], { move: 'held' })).toEqual(['move']);
    expect(simulationHeldNodes([refusal], { move: 'success' })).toEqual([]);

    const base = controller();
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const markup = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={{
      ...base,
      state: {
        ...base.state,
        run: { ...base.state.run!, nodeStatuses: { root: 'running', move: 'held' } },
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: { audit: [refusal] } as unknown as RuntimeControls,
        },
      },
    }} />);
    expect(markup).toContain('Waiting for Run on move: Simulation not playing. Play the simulation, then Run.');
    expect(markup).toContain('Simulation not playing on move · stopped, waiting for Run');
  });

  it('drops the simulation line once that attempt\'s hold ended, even when a later attempt is held by an ordinary Stop', () => {
    const event = (eventRevision: number, kind: string, attempt: string): RuntimeControlAuditEvent => ({
      eventRevision, kind, facts: { node_instance_id: 'move', node_attempt_id: attempt },
    });
    const refused = event(2, 'program_node_simulation_not_playing', 'move.1');
    const ended = event(3, 'program_node_simulation_hold_ended', 'move.1');
    expect(simulationRefusalAuditText(ended)).toBe('Simulation hold ended on move');
    // Run after play, then an ordinary operator Stop holds the next attempt.
    expect(simulationHeldNodes([refused, ended], { move: 'held' })).toEqual([]);
    // Refused again on the next attempt: shown for that attempt.
    expect(simulationHeldNodes(
      [refused, ended, event(4, 'program_node_simulation_not_playing', 'move.2')], { move: 'held' },
    )).toEqual(['move']);

    const base = controller();
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const markup = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={{
      ...base,
      state: {
        ...base.state,
        run: { ...base.state.run!, nodeStatuses: { root: 'running', move: 'held' } },
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: { audit: [refused, ended] } as unknown as RuntimeControls,
        },
      },
    }} />);
    expect(markup).not.toContain('Waiting for Run on move: Simulation not playing');
  });

  it('shows the failed attempt in red before its retry, with the Kernel failure when there is one', () => {
    const failed = (
      failure: StructuredError | null,
      cause = 'collision_stop_nominal',
    ): AutomaticRetryAuditEvent => ({
      eventRevision: 2,
      kind: 'program_node_automatic_retry',
      facts: {
        nodeInstanceId: 'move', attempt: 2, maxAttempts: 4, cause, delayS: 1, failure,
      },
    });
    const kernel: StructuredError = {
      code: 'kernel_timeout', message: 'planner timed out', details: null, origin: null,
    };
    // The code is shown as the structured-error display humanises it.
    expect(automaticRetryFailureText(failed(kernel)))
      .toBe('Collision Stop on move: kernel timeout · planner timed out');
    expect(automaticRetryFailureText(failed({ ...kernel, code: 'ik_failed' }, 'planning_no_effect')))
      .toBe('Planning failed on move: ik failed · planner timed out');
    // The cause alone when the owner has no Kernel failure.
    expect(automaticRetryFailureText(failed(null))).toBe('Collision Stop on move');
    expect(automaticRetryFailureText(failed(null, 'planning_no_effect'))).toBe('Planning failed on move');

    const base = controller();
    type RuntimeControls = NonNullable<NonNullable<ProgramRuntimeController['state']['controls']>['runtimeControls']>;
    const withAudit: ProgramRuntimeController = {
      ...base,
      state: {
        ...base.state,
        controls: {
          ...ordinaryMotionStopControls(),
          runtimeControls: {
            audit: [failed(kernel)],
          } as unknown as RuntimeControls,
        },
      },
    };
    const markup = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={withAudit} />);
    const red = 'Collision Stop on move: kernel timeout · planner timed out';
    const retry = 'Attempt 2 of 4 for move after collision Stop';
    expect(markup).toContain(`<p class="inline-message severity-error">${red}</p>`);
    expect(markup.indexOf(red)).toBeLessThan(markup.indexOf(retry));
  });

  it('shows exact owner status without duplicating the standard commands', () => {
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller()} />,
    );

    expect(markup).toContain('Program execution');
    expect(markup).toContain('program-run.1');
    expect(markup).toContain('revision 4');
    expect(markup).toContain('next-pick');
    expect(markup).not.toContain('Selection for the next Run');
    expect(markup).toContain('Program in this run');
    expect(markup).toContain('fixed for this run');
    // Execute, Halt and Reset live in the Program panel; Program Stop is gone.
    expect(markup).not.toContain('Program Stop');
    expect(markup).not.toContain('Execute Applied Program');
    expect(markup).not.toContain('Reset Program cycle');
    // Advanced tools exist only with run controls; no empty section otherwise.
    expect(markup).not.toContain('Advanced: runtime blackboard');
    expect(markup).not.toContain('Runtime overrides');
  });

  it('offers typed runtime writes and shows the backend reason when they are refused', () => {
    const halted = controller('full_stopped');
    halted.state.applied!.program.blackboard = {
      namespaces: [],
      keys: [
        { key: 'flag', type: 'bool', access: 'read_write', persistence: 'run' },
        { key: 'layer', type: 'int64', access: 'read_write', persistence: 'persistent', initial: 0 },
        { key: 'limit', type: 'float64', access: 'read', persistence: 'run' },
        { key: 'reply', type: 'model_response', access: 'write', persistence: 'run' },
      ],
    };
    halted.runnerAllows = vi.fn((kind: string) => kind !== 'program_runtime_blackboard_write');
    halted.runnerRefusal = vi.fn(() => 'program_run_state_full_stopped');
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={halted} />,
    );

    expect(markup).toContain('Advanced: runtime blackboard, stored values and audit');
    // Only writable keys with a typed editor; no raw JSON entry remains.
    expect(markup).toContain('<option value="flag">flag · bool</option>');
    expect(markup).toContain('<option value="layer">layer · int64</option>');
    expect(markup).not.toContain('limit · float64');
    expect(markup).not.toContain('reply · model_response');
    expect(markup).toContain('Writes are refused now: program run state full stopped.');
    expect(markup).toMatch(/<button type="button" disabled="">Write current-run value<\/button>/);
    const source = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    expect(source).not.toContain('parseRuntimeValue');
    expect(source).toContain('<PersistentKeyValuesSection');
  });

  it('never turns an unavailable or unknown owner fact into idle or zero work', () => {
    const unavailable = controller();
    unavailable.state.run = null;
    unavailable.state.runnerAvailable = false;
    const unavailableMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unavailable} />,
    );
    expect(unavailableMarkup).toContain('unavailable');
    expect(unavailableMarkup).toContain('execution state is unknown');
    expect(unavailableMarkup).toContain('this never proves that robot work stopped');
    expect(unavailableMarkup).toContain('<span>Motion</span><strong>Unknown</strong>');

    // A run without a motion projection is Unknown, never "not moving".
    const unknownMotion = controller();
    unknownMotion.state.controls = null;
    const unknownMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={unknownMotion} />,
    );
    expect(unknownMarkup).toContain('<span>Motion</span><strong>Unknown</strong>');
  });

  it('shows the captured retry budget, with zero as Unlimited, and where to correct', () => {
    const held = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = {
      ...controls.recoveryOffer!,
      recoveryClass: 'node_failure_pause',
      nodeInstanceId: 'move',
      policy: { allowIntervention: true, maxRetries: 0 },
      retriesUsed: 300,
    };
    held.state.controls = controls;
    const unlimited = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={held} />);
    expect(unlimited).toContain('Retry limit Unlimited · 300 used');
    expect(unlimited).toContain('change a runtime value for this run under Advanced');
    expect(unlimited).toContain('Apply then Resets the run');
    expect(unlimited).not.toContain('Retry limit 0');
    controls.recoveryOffer = {
      ...controls.recoveryOffer!,
      policy: { allowIntervention: true, maxRetries: 3 },
      retriesUsed: 1,
    };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={held} />))
      .toContain('Retry limit 3 · 1 used');
  });

  it('distinguishes ordinary Motion Stop from whole-Program Stop', () => {
    const motionStopped = controller('running');
    motionStopped.state.controls = ordinaryMotionStopControls();
    const motionMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={motionStopped} />,
    );
    expect(motionMarkup).toContain('<strong>Stopped</strong>');

    const settledMotionStop = controller('intervention_required');
    settledMotionStop.state.controls = ordinaryMotionStopControls();
    settledMotionStop.state.run!.reason = {
      code: 'operation_stopped',
      message: 'native operation was interrupted',
      details: null,
      origin: null,
    };
    settledMotionStop.state.run!.message = 'native operation was interrupted';
    const settledMotionMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={settledMotionStop} />,
    );
    expect(settledMotionMarkup).toContain('<strong>Stopped</strong>');
    expect(settledMotionMarkup).toContain('Program motion stopped and held.');
    expect(settledMotionMarkup).not.toContain('native operation was interrupted');

    const executionStopped = renderToStaticMarkup(
      <ProgramRuntimePanel
        enabled
        controller={controller('stopped', 'terminal_awaiting_reset')}
      />,
    );
    expect(executionStopped).toContain('<strong>Stopped</strong>');

    const fullStopped = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={controller('full_stopped')} />,
    );
    expect(fullStopped).toContain('<strong>Stopped</strong>');

    const executionStoppedWithStaleOffer = controller(
      'stopped',
      'terminal_awaiting_reset',
    );
    executionStoppedWithStaleOffer.state.controls = ordinaryMotionStopControls();
    const staleOfferMarkup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={executionStoppedWithStaleOffer} />,
    );
    expect(staleOfferMarkup).toContain('<strong>Stopped</strong>');
  });

  it('shows a run failure with its labelled details and failed node', () => {
    const failed = controller('failed', 'terminal_awaiting_reset');
    failed.state.run!.reason = {
      code: 'node_input_unavailable',
      message: "missing input duration_s from blackboard_key 'delay'",
      details: {
        input_problem: 'missing',
        path: 'duration_s',
        source_kind: 'blackboard_key',
        source_reference: 'delay',
      },
      origin: { kind: 'program_node', nodeInstanceId: 'wait', ownerApi: null },
    };
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={failed} />,
    );
    expect(markup).toContain(
      'node input unavailable: missing input duration_s from blackboard_key &#x27;delay&#x27;',
    );
    expect(markup).toContain(
      '<small> · Input: duration_s from blackboard key &#x27;delay&#x27; (missing)</small>',
    );
    expect(markup).toContain('<small> · Origin: node &#x27;wait&#x27;</small>');
  });

  it('shows a Close without grasp evidence as a failure naming its node', () => {
    const failed = controller('failed', 'terminal_awaiting_reset');
    const reason: StructuredError = {
      code: 'grasp_not_confirmed',
      message: 'Close completed without grasp evidence from the workpiece_grasped input.',
      details: null,
      origin: { kind: 'program_node', nodeInstanceId: 'close', ownerApi: null },
    };
    failed.state.run!.reason = reason;
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={failed} />,
    );
    // The title is the code's display text (the error catalog's, once merged).
    expect(markup).toContain(
      `<div class="program-feedback error-state" role="status"><span><strong>${formatStructuredError(reason).title}</strong>`,
    );
    expect(markup).toContain(': Close completed without grasp evidence from the workpiece_grasped input.');
    expect(markup).toContain('<small> · Origin: node &#x27;close&#x27;</small>');

    // Any other failed run keeps its plain presentation.
    const other: StructuredError = { ...reason, code: 'io_wait_timeout' };
    failed.state.run!.reason = other;
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={failed} />)).toContain(
      `<div class="program-feedback" role="status"><span>${formatStructuredError(other).title}: `,
    );
  });

  it('shows a retained failed run that carries no details or origin', () => {
    const failed = controller('failed', 'terminal_awaiting_reset');
    failed.state.run!.reason = {
      code: 'program_tree_failed',
      message: 'Program failed.',
      details: null,
      origin: null,
    };
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel enabled controller={failed} />,
    );
    expect(markup).toContain('<span>program tree failed: Program failed.</span>');
    expect(markup).not.toContain('Origin:');
  });

  it('edits displayed groups in the Draft and applies them to the Motion panel after Apply', () => {
    const changes: string[][] = [];
    const scopes = executionGroupScopes([{
      node_id: 'parallel', instance_id: 'root', parameters: {}, children: [
        { node_id: 'motion.move_to_pose', instance_id: 'a', parameters: { target: { component_id: 'robot', command_target_id: 'arm' } }, execution_groups: ['robot_a'], children: [] },
        { node_id: 'motion.move_to_pose', instance_id: 'b', parameters: {}, execution_groups: ['robot_b'], children: [] },
      ],
    }], ['robot_a', 'robot_b']);
    const settings = renderToStaticMarkup(
      <MotionControlSettings groups={{
        scopes,
        hidden: undefined,
        pendingApply: true,
        onChange: (hidden) => changes.push(hidden),
      }} />,
    );
    expect(settings).toMatch(/<span>Displayed groups<\/span><span class="settings-scope" data-scope="program"[^>]*>Program<\/span>/);
    expect(settings).toContain('Changed in the Draft, not applied yet.');
    expect(settings).toMatch(/<span>Layout and confirmations<\/span><span class="settings-scope" data-scope="installation"[^>]*>Installation<\/span>/);
    expect(settings).toContain('Confirm before halting the Program');
    // Default: named groups shown, "all" not.
    expect(settings).toMatch(/<input type="checkbox"\/><span title="all">All groups/);
    expect(settings).toContain('Show All, or another covering group, before hiding this group.');
    expect(settings).toContain('disabled="" checked=""/><span title="robot_a"');
    expect(settings).toContain('1 hardware node · robot/arm');

    const readOnly = renderToStaticMarkup(
      <MotionControlSettings groups={{
        scopes, hidden: ['robot_a'], pendingApply: false,
      }} />,
    );
    expect(readOnly).toContain('The Program Draft cannot be edited right now.');
    expect(readOnly).toMatch(/<fieldset aria-label="Displayed groups" disabled="">/);

    // The Applied Program's choice drives the Motion panel.
    const running = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    controls.executionSelections.groups = [
      group,
      { ...group, groupId: 'robot_a' },
      { ...group, groupId: 'robot_b' },
    ];
    running.state.controls = controls;
    running.state.applied!.program.viewer = { hidden_execution_groups: ['all', 'robot_b'] };
    const transport = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(transport).toContain('robot_a controls');
    expect(transport).not.toContain('robot_b controls');
    expect(transport).toMatch(/aria-label="Open viewer settings; 1 command groups hidden"/);
    running.state.applied!.program.tree = {
      node_id: 'motion.move_to_pose', instance_id: 'new-move', parameters: {}, children: [],
    };
    const uncovered = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(uncovered).toContain('all controls');
    expect(uncovered).toContain('leave some move, gripper or I/O nodes without a visible control');
    expect(uncovered).not.toContain('membership is unavailable');
    expect(running.state.applied!.program.viewer.hidden_execution_groups).toEqual(['all', 'robot_b']);
    expect(changes).toEqual([]);
  });

  it('keeps All visible, and says why, when the run was captured from another revision', () => {
    const running = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    controls.executionSelections.groups = [group, { ...group, groupId: 'robot_a' }];
    running.state.controls = controls;
    const applied = running.state.applied!;
    applied.program.viewer = { hidden_execution_groups: ['all'] };
    applied.program.execution_groups = { robot_a: { execution_policy: 'independent' } };
    applied.program.tree = {
      node_id: 'motion.move_to_pose', instance_id: 'a',
      parameters: { target: { component_id: 'ur10e', command_target_id: 'arm' } },
      execution_groups: ['robot_a'], children: [],
    };
    applied.programBinding.target_bindings = [targetBinding('ur10e', 'arm')];
    const render = () => renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    const matched = render();
    expect(matched).toContain('robot_a controls');
    expect(matched).not.toContain('all controls');
    // Targets only from the Applied Program of the captured revision.
    expect(matched).toContain('title="robot_a: ur10e/arm"');

    running.state.run!.capture.programRevision = `sha256:${'9'.repeat(64)}`;
    const mismatched = render();
    expect(mismatched).toContain('all controls');
    expect(mismatched).toContain('robot_a controls');
    expect(mismatched).toContain('group membership is unavailable');
    expect(mismatched).toContain('different Program revision than the Applied Program');
    expect(mismatched).not.toContain('ur10e');
    expect(applied.program.viewer.hidden_execution_groups).toEqual(['all']);
  });

  it('labels Draft targets with their direct component/target references', () => {
    const scopes = executionGroupScopes([{
      node_id: 'gripper.command_position', instance_id: 'g',
      parameters: { target: { component_id: 'robotiq', command_target_id: 'gripper' } },
      execution_groups: ['tool'], children: [],
    }], ['tool']);
    const draft = renderToStaticMarkup(<MotionControlSettings groups={{
      scopes, hidden: undefined, pendingApply: false,
    }} />);
    expect(draft).toContain('1 hardware node · robotiq/gripper');
  });

  it('shows Retry during a failure pause, disabled until containment is confirmed', () => {
    const paused = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    const offer = controls.recoveryOffer!;
    offer.recoveryClass = 'operation_failure' as typeof offer.recoveryClass;
    offer.nodeInstanceId = 'move.1';
    offer.actions = [{
      actionId: 'retry_motion_from_current_state',
      requiresConfirmation: false,
      performsBackendReset: false,
      enabled: false,
      disabledReasonCode: 'failure_pause_containment_unconfirmed',
    }];
    controls.recoveryHolds = [{ motionGroupIds: ['all'], interventionStopScope: 'all', offer }];
    paused.state.controls = controls;
    const render = () => renderToStaticMarkup(
      <ProgramTransportControls enabled program={paused} robot={robotController()} />,
    );

    // Resume must not bypass the pause while Retry is unavailable.
    expect(render()).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="Retry waits until stopping the Program&#x27;s other motion is confirmed."[^>]*>Retry<\/button>/,
    );
    offer.actions[0].enabled = true;
    offer.actions[0].disabledReasonCode = null;
    expect(render()).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*title="Retry move.1 and let all the Program&#x27;s motion continue."[^>]*>Retry<\/button>/,
    );
    // Retry lets held motion continue: it needs motion_clear like Run/Resume.
    paused.readiness = { ...paused.readiness!, motion_clear: false };
    expect(render()).toMatch(/class="transport-group-resume"[^>]*disabled=""[^>]*>Retry<\/button>/);
  });

  it('labels a group with stopped and never-started work Resume', () => {
    const mixed = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    controls.executionSelections.groups[0].state = 'mixed';
    mixed.state.controls = controls;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={mixed} robot={robotController()} />,
    );
    expect(markup).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
  });

  it('shows a disabled Stopping until the operator Stop hold is recorded, then Resume', () => {
    // F-6a W: the engine still finishes its Stop cleanup, so the Runner has
    // not recorded the hold yet and projects the settling reason. Resume is
    // never offered enabled here only for the Runner to refuse it.
    const settling = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    group.resumeAllowed = false;
    group.startAllowed = false;
    group.resumeUnavailableReason = 'selected_motion_stop_settling';
    group.startUnavailableReason = 'selected_motion_stop_settling';
    settling.state.controls = controls;
    const render = (program: ProgramRuntimeController) => renderToStaticMarkup(
      <ProgramTransportControls enabled program={program} robot={robotController()} />,
    );
    expect(render(settling)).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="all is stopping: Resume is available once its Stop has settled."[^>]*>Stopping…<\/button>/,
    );
    // Even a stale enabled flag never enables it while the reason says settling.
    group.resumeAllowed = true;
    expect(render(settling)).toMatch(/class="transport-group-resume"[^>]*disabled=""[^>]*>Stopping…<\/button>/);

    // The hold is recorded: the same group offers Resume.
    const held = controller('intervention_required');
    const recorded = ordinaryMotionStopControls();
    recorded.recoveryOffer = null;
    held.state.controls = recorded;
    expect(render(held)).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
  });

  it('offers Resume and Reset, but not another Stop, after ordinary Motion Stop', () => {
    const motionStopped = controller('intervention_required');
    motionStopped.state.controls = ordinaryMotionStopControls();

    const markup = renderToStaticMarkup(
      <ProgramTransportControls
        enabled
        program={motionStopped}
        robot={robotController()}
      />,
    );

    expect(markup).toMatch(/class="transport-group-resume"(?![^>]*disabled)[^>]*>Resume<\/button>/);
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/,
    );
    expect(markup).toMatch(/class="transport-reset transport-unique-control meaningful"(?![^>]*disabled)[^>]*>Reset<\/button>/);
  });

  it('confirms Reset during execution only after all motion has stopped', () => {
    const motionStopped = controller('intervention_required');
    motionStopped.state.controls = ordinaryMotionStopControls();
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={motionStopped} robot={robotController()} />,
    );
    // While executing, Reset asks first instead of resetting at once.
    expect(markup).toMatch(
      /class="transport-reset[^"]*"[^>]*title="Stop all Program motion, then confirm ending this run and resetting the Program\."[^>]*>Reset<\/button>/,
    );
    const source = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    const flow = source.slice(
      source.indexOf('const requestProgramReset'),
      source.indexOf('const requestProgramHalt'),
    );
    // Not executing: reset at once. Executing: stop motion, then ask.
    // confirmReset = the preference only: an unknown write shows on its output.
    expect(source).toContain('const confirmReset = preferences.confirmReset;');
    expect(flow).toMatch(/if \(!executionActive\) \{\s*if \(endedReset && confirmReset\) setResetIntent\('confirm'\);\s*else void program\.reset\(\);/);
    expect(flow).toContain("setResetIntent(confirmReset ? 'stopping' : 'automatic');");
    expect(flow).toContain("await program.stopMotion(['all'])");
    // Halted: motion is already contained, so no refused motion Stop is sent.
    expect(source).toContain('allGroup === undefined || allMotionQuiesced || programFullStopped');
    expect(source).toContain('state.runnerAvailable && executionSelectionIsQuiescent(allGroup)');
    expect(flow).toContain('if (programFullStopping) return;');
    expect(source).toContain(
      "if (resetIntent === 'stopping' && motionSettled) setResetIntent('confirm');",
    );
    expect(source).toContain('open={resetIntent === \'confirm\'}');
  });

  it('offers the two Reset options, off by default, and states what stays', () => {
    expect(resetKeptMessage({ reloadInitialScene: false, resetRobotPosition: false }))
      .toBe('The Scene, carried objects, gripper state and robot positions stay as they are.');
    expect(resetKeptMessage({ reloadInitialScene: true, resetRobotPosition: false }))
      .toBe('Robot positions stay as they are.');
    expect(resetKeptMessage({ reloadInitialScene: false, resetRobotPosition: true }))
      .toBe('The Scene, carried objects and gripper state stay as they are.');
    expect(resetKeptMessage({ reloadInitialScene: true, resetRobotPosition: true })).toBeNull();
    const source = readFileSync(new URL('./ProgramRuntimePanel.tsx', import.meta.url), 'utf8');
    const flow = source.slice(
      source.indexOf('const requestProgramReset'),
      source.indexOf('const requestProgramHalt'),
    );
    // Every Reset request starts with both options off; idle Reset always asks.
    expect(flow).toMatch(/setResetSelection\(NO_RESET_OPTIONS\);\s*if \(idleReset\) \{\s*setResetIntent\('confirm'\);/);
    expect(source).toContain("const idleReset = cycleState === 'idle_after_reset' && !executionActive;");
    // Position reset is sent only when the backend reports it available.
    expect(source).toContain('resetRobotPosition: positionAvailable && resetSelection.resetRobotPosition,');
    expect(source).toContain('disabled={!positionAvailable}');
    expect(source).toContain('void program.reset(resetSelectionApplied);');
    // An idle Reset with nothing selected would restore nothing.
    // Never refused for an output with an unknown outcome.
    expect(source).toContain('disabled={!canReset || (!motionSettled && !endedReset && !idleReset)}');
    // An idle Reset may restore nothing.
    expect(source).not.toContain('resetNothingSelected');
    // The automatic (unconfirmed) running Reset keeps both options off.
    expect(source).toMatch(/if \(resetIntent === 'automatic' && motionSettled\) \{\s*setResetIntent\(null\);\s*void program\.reset\(\);/);
  });

  it('projects Execute as active while initial motion is Ready for deliberate Run', () => {
    const executing = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    const group = controls.executionSelections.groups[0];
    group.state = 'ready';
    group.stopAllowed = false;
    group.startAllowed = true;
    group.startUnavailableReason = null;
    // Run and Resume are one command; the owner projects them equal.
    group.resumeAllowed = true;
    group.resumeUnavailableReason = null;
    executing.state.controls = controls;
    const selector = group.runtimeScaleSelector!;
    const status = runtimeScaleStatus(selector, 0.4);
    executing.groupRuntimeScales = {
      all: { selector, status, draft: 0.4, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={executing} robot={robotController()} />,
    );

    expect(markup).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*>Run<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/,
    );
    expect(markup).toMatch(/type="range"[^>]*value="0.4"/);
    expect(markup).not.toMatch(/type="range"[^>]*disabled=/);
    expect(markup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-full-stop transport-unique-control"(?![^>]*disabled)[^>]*>Halt<\/button>/,
    );
  });

  it('keeps Halt disabled until a Program is executing', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    expect(markup).toMatch(
      /class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*>Execute<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-full-stop transport-unique-control"[^>]*disabled=""[^>]*>Halt<\/button>/,
    );
  });

  it('gates a new Execute on false admission while preserving unknown and full-stop resume behavior', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    const blocked = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()}
        authoringAdmissionAllowed={false} />,
    );
    expect(blocked).toMatch(
      /class="transport-run transport-unique-control" disabled="" title="The cell is finishing a background step; this is available again in a moment\."[^>]*>Execute<\/button>/,
    );

    const allowed = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()}
        authoringAdmissionAllowed={true} />,
    );
    expect(allowed).toMatch(
      /class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*>Execute<\/button>/,
    );
    const unknown = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );
    expect(unknown).toMatch(
      /class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*>Execute<\/button>/,
    );

    const fullStopped = controller('full_stopped');
    const resume = renderToStaticMarkup(
      <ProgramTransportControls enabled program={fullStopped} robot={robotController()}
        authoringAdmissionAllowed={false} />,
    );
    expect(resume).toMatch(
      /class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*title="Resume the halted Program tree\./,
    );
  });

  it('blocks Activate and execute confirmation while admission is false', () => {
    const activation = {
      flow: { phase: 'offered', componentIds: ['lite6_robot'], failures: [], recovery: {} },
      states: [], open: vi.fn(), confirm: vi.fn(), cancel: vi.fn(), available: true, recoveryReady: true,
    } as unknown as ActivateExecuteController;
    const tooltip = 'The cell is finishing a background step; this is available again in a moment.';
    const blocked = renderToStaticMarkup(
      <ActivateExecuteDialog activation={activation} authoringAdmissionAllowed={false} />,
    );
    expect(blocked).toContain(`<button type="button" class="transport-run" disabled="" title="${tooltip}">Activate and execute</button>`);
    const allowed = renderToStaticMarkup(
      <ActivateExecuteDialog activation={activation} authoringAdmissionAllowed={true} />,
    );
    expect(allowed).toMatch(/class="transport-run"(?![^>]*disabled)[^>]*>Activate and execute<\/button>/);
  });

  it('keeps paired commands disabled through Execute; a pending Stop stays clickable', () => {
    const starting = controller('succeeded', 'idle_after_reset');
    starting.state.pending = 'run';
    const startingMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={starting} robot={robotController()} />,
    );
    // The in-flight Execute shows its busy label; Reset names it.
    expect(startingMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing…<\/button>/,
    );
    expect(startingMarkup).toMatch(
      /class="transport-reset transport-unique-control"[^>]*disabled=""[^>]*title="Execute is in progress."/,
    );
    expect(startingMarkup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stop<\/button>/,
    );

    const running = controller('running');
    const runningMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    expect(runningMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );

    running.state.selectionPending = [{
      kind: 'motion-stop',
      groupIds: ['all'],
      requestId: 'stop-request.1',
      ownerEpoch: 'program-runner.epoch.1',
      programRunId: 'program-run.1',
    }];
    const stoppingMarkup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    // The first press shows as Stopping; a repeated press sends another Stop.
    expect(stoppingMarkup).toMatch(
      /class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*>Stopping<\/button>/,
    );
    expect(stoppingMarkup).toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Executing<\/button>/,
    );
  });

  it.each(['stopping', 'mixed'] as const)('reflects selected group %s and keeps its Stop clickable', (state) => {
    const running = controller('running');
    running.state.controls = ordinaryMotionStopControls();
    const group = running.state.controls.executionSelections!.groups[0];
    group.state = state;
    group.stopAllowed = state === 'mixed';
    group.resumeAllowed = false;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={running} robot={robotController()} />,
    );
    if (state === 'stopping') {
      expect(markup).toMatch(/class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*>Stopping<\/button>/);
    } else {
      expect(markup).toMatch(/class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*>Stop<\/button>/);
    }
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
  });

  it.each([false, true])('shows the owner velocity scope while Run is starting (group=%s)', (group) => {
    const starting = controller('succeeded', 'idle_after_reset');
    starting.state.pending = 'run';
    starting.state.controls = ordinaryMotionStopControls();
    starting.state.controls.executionSelections.groups[0].state = 'running';
    const robot = robotController();
    const selector: RuntimeScaleControlSelector = group
      ? {
        kind: 'group', programBindingId: `sha256:${'c'.repeat(64)}`,
        groupTargets: [
          { componentId: 'left', commandTargetId: 'arm' },
          { componentId: 'right', commandTargetId: 'arm' },
        ],
      }
      : {
        kind: 'singular',
        programBindingId: `sha256:${'c'.repeat(64)}`,
        componentId: 'controlled_component',
        commandTargetId: 'command_target',
      };
    const status = runtimeScaleStatus(selector);
    starting.state.controls.executionSelections.groups[0].runtimeScaleSelector = status.selector;
    starting.groupRuntimeScales = {
      all: { selector: status.selector, status, draft: 0.5, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={starting} robot={robot} />,
    );

    expect(markup).toMatch(/type="range"[^>]*value="0.5"/);
    expect(markup).not.toMatch(/type="range"[^>]*disabled=/);
    expect(markup).toContain('>Velocity<');
    expect(markup).toContain('Scale current and subsequent moves for all.');
  });

  it('keeps group velocity disabled until Execute creates a captured owner target', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    idle.state.applied!.program.tree = {
      node_id: 'motion.move_to_joint_target',
      instance_id: 'root-motion',
      parameters: { target: { component_id: 'robot', command_target_id: 'arm' } },
      children: [],
    };
    idle.state.applied!.programBinding.target_bindings = [targetBinding('robot', 'arm')];
    const selector: RuntimeScaleControlSelector = {
      kind: 'singular',
      programBindingId: idle.state.applied!.programBindingId,
      componentId: 'robot',
      commandTargetId: 'arm',
    };
    const status = runtimeScaleStatus(selector, 0.35);
    idle.groupRuntimeScales = {
      all: { selector, status, draft: 0.35, pending: false, error: null },
    };

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    // The idle Applied group names its robot through the direct target reference.
    expect(markup).toContain('title="all: robot/arm"');
    expect(markup).toMatch(/type="range"[^>]*value="0.35"/);
    expect(markup).toMatch(/type="range"[^>]*disabled=/);
  });

  it('gates group Resume on RunReadiness alone', () => {
    const program = controller();
    program.state.controls = ordinaryMotionStopControls();
    const robot = robotController();
    // A resident manual operation is the backend's blocked reason; the GUI
    // keeps no gate of its own.
    program.readiness = { can_execute: false, can_resume: false, motion_clear: false, blocked_reasons: [
      { code: 'manual_operation_in_progress', message: 'A manual operation is in progress.' }] };
    const blocked = renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />);
    expect(blocked).toMatch(/class="transport-group-resume"[^>]*disabled=""/);
    program.readiness = { can_execute: false, can_resume: true, motion_clear: true, blocked_reasons: [] };
    const released = renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />);
    expect(released).toMatch(/class="transport-group-resume"(?![^>]*disabled)/);
    // A rule hold gates the groups its scope meets, with its message.
    program.readiness = { can_execute: false, can_resume: false, motion_clear: true, blocked_reasons: [
      { code: 'rule_holds', message: 'Rule door holds its scope (its condition holds)', rule_id: 'door',
        scope: 'everything' }] };
    expect(renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />))
      .toMatch(/class="transport-group-resume"[^>]*disabled=""[^>]*title="Rule door holds its scope \(its condition holds\)"/);
    program.readiness = { can_execute: false, can_resume: true, motion_clear: true, blocked_reasons: [
      { code: 'rule_holds', message: 'Rule door holds its scope', rule_id: 'door',
        scope: 'execution_groups', component_ids: ['other_robot'] }] };
    expect(renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />))
      .toMatch(/class="transport-group-resume"(?![^>]*disabled)/);
  });

  it('does not delay Run while a coalesced velocity update is pending', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    const robot = robotController();
    robot.runtimeScalePending = true;

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robot} />,
    );

    expect(markup).toContain('class="transport-run transport-unique-control"');
    expect(markup).not.toMatch(
      /class="transport-run transport-unique-control"[^>]*disabled=""[^>]*>Run<\/button>/,
    );
  });

  it('keeps Stop and Halt available whenever a run is running, whatever else is in progress', () => {
    const groupStop = /class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*>(Stop|Stopping)<\/button>/;
    const halt = /class="danger-button transport-full-stop transport-unique-control"(?![^>]*disabled)[^>]*>(Halt|Halting)<\/button>/;
    const busy = (configure: (program: ProgramRuntimeController) => void) => {
      const program = controller('running');
      const controls = ordinaryMotionStopControls();
      controls.recoveryOffer = null;
      controls.executionSelections.groups[0].state = 'running';
      controls.executionSelections.groups[0].stopAllowed = true;
      program.state.controls = controls;
      configure(program);
      return renderToStaticMarkup(
        <ProgramTransportControls enabled={false} program={program} robot={robotController()} />,
      );
    };
    const cases: Array<[string, (program: ProgramRuntimeController) => void]> = [
      // The connection is not current (delayed, stale, synchronizing).
      ['connection not current', () => undefined],
      ['cycle still starting', (program) => { program.cycleState = 'starting'; }],
      ['cycle stopping', (program) => { program.cycleState = 'stopping'; }],
      ['runner projection not loaded', (program) => { program.state.runnerAvailable = false; }],
      ['Run pending', (program) => { program.state.pending = 'run'; }],
      ['Reset pending', (program) => { program.state.pending = 'reset'; }],
      ['Full Stop pending', (program) => { program.state.pending = 'full-stop'; }],
      ['unconfirmed speed change', (program) => { program.state.controlPending = 'runtime-scale'; }],
      ['Stop pending', (program) => {
        program.state.selectionPending = [{
          kind: 'motion-stop', groupIds: ['all'], requestId: 'stop-request.1',
          ownerEpoch: 'program-runner.epoch.1', programRunId: 'program-run.1',
        }];
      }],
      ['owner Stop admission not projected', (program) => {
        program.runnerAllows = vi.fn(() => false);
        program.state.controls!.executionSelections.groups[0].stopAllowed = false;
      }],
    ];
    for (const [name, configure] of cases) {
      const markup = busy(configure);
      expect(markup, name).toMatch(groupStop);
      expect(markup, name).toMatch(halt);
    }
    // The only Group Stop gate: the group is already held (or not started).
    const held = busy((program) => {
      program.state.controls!.executionSelections.groups[0].state = 'held';
    });
    expect(held).toMatch(/class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/);
    expect(held).toMatch(halt);
  });

  it('keeps motion Stop disabled while Program Execute is still starting', () => {
    const starting = controller('succeeded', 'starting');
    starting.state.run = null;
    starting.state.pending = 'run';

    const markup = renderToStaticMarkup(
      <ProgramTransportControls
        enabled
        program={starting}
        robot={robotController()}
      />,
    );

    expect(markup).toMatch(
      /class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stop<\/button>/,
    );
  });

  it('renders physically separate left group and right unique-control panels', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );
    expect(markup).toContain('class="transport-motion-panel"');
    expect(markup).toMatch(/class="transport-motion-panel"[^>]*>[\s\S]*class="transport-panel-title">Motion<\/h3>/);
    expect(markup).toMatch(/class="transport-unique-column"[^>]*>[\s\S]*class="transport-panel-title">Program<\/h3>/);

    const resumeIndex = markup.indexOf('class="transport-group-resume"');
    const stopIndex = markup.indexOf('class="danger-button transport-group-stop"');
    const groupNameIndex = markup.indexOf('class="transport-group-name"');
    const velocityIndex = markup.indexOf('class="transport-group-scale"');
    expect(resumeIndex).toBeGreaterThan(-1);
    expect(resumeIndex).toBeLessThan(stopIndex);
    expect(stopIndex).toBeLessThan(velocityIndex);
    expect(velocityIndex).toBeLessThan(groupNameIndex);
    expect(markup).toContain('>Halt</button>');

    const css = readStyleSource();
    const transport = css.slice(
      css.indexOf('.viewer-transport {'),
      css.indexOf('.transport-velocity {'),
    );
    expect(transport).toContain('overflow: visible;');
    expect(transport).toMatch(/\.viewer-transport \{[^}]*padding: 0;[^}]*overflow: visible;/s);
    expect(transport).not.toMatch(/\.viewer-transport \{[^}]*(?:border|background):/s);
    expect(transport).toMatch(/\.viewer-transport \{[^}]*pointer-events: none;/s);
    expect(css).toMatch(
      /\.viewer-transport \.transport-motion-panel,[\s\S]*\.transport-unique-column \{[^}]*pointer-events: auto;/s,
    );
    expect(transport).toMatch(
      /\.viewer-transport \{[^}]*grid-template-columns:[^;]*var\(--viewer-transport-motion-panel-width\)[^;]*minmax\(0, 1fr\)[^;]*var\(--viewer-transport-column-width\);[^}]*column-gap: 1\.25rem;/s,
    );
    expect(css).not.toContain('.transport-guidance');
    expect(transport).toMatch(/\.transport-control-layout \{[^}]*display: contents;/s);
    expect(transport).toMatch(
      /\.transport-motion-panel \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);/s,
    );
    expect(transport).toMatch(
      /\.transport-motion-panel \{[^}]*width: var\(--viewer-transport-motion-panel-width\);/s,
    );
    expect(transport).toMatch(
      /\.transport-group-grid \{[^}]*justify-content: flex-start;[^}]*padding: 0;[^}]*overflow-x: auto;/s,
    );
    expect(transport).toMatch(
      /\.transport-group-column \{[^}]*padding: 0;[^}]*border: 0;[^}]*background: transparent;/s,
    );
    expect(transport).toMatch(
      /\.transport-group-scale \{[^}]*border: 1px solid #405764;[^}]*border-radius: 0\.45rem;/s,
    );
    expect(transport).toMatch(
      /\.transport-unique-column \{[^}]*border: 1px solid #365563;[^}]*background: rgb\(8 23 32 \/ 94%\);[^}]*margin-left: auto;/s,
    );
  });

  it('splits Info and group Settings when the Applied Program has a named group', () => {
    const idle = controller('succeeded', 'idle_after_reset');
    idle.state.run = null;
    idle.state.controls = null;
    idle.state.applied!.program.execution_groups = {
      robot_group: { execution_policy: 'independent' },
    };
    idle.state.applied!.program.tree.execution_groups = ['robot_group'];

    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={idle} robot={robotController()} />,
    );

    expect(markup).toContain('aria-label="Program run information"');
    // The gear opens the 3D viewer settings, which hold these controls.
    expect(markup).toMatch(/aria-label="Open viewer settings(?:; [^"]+)?"/);
    expect(markup).toContain('class="transport-unique-icons"');
    expect(markup).toContain('robot_group controls');
    // The default layout hides only "all", which is not counted.
    expect(markup).not.toContain('transport-hidden-group-count');

    const css = readStyleSource();
    expect(css).toMatch(
      /\.transport-unique-icons \{[^}]*grid-template-columns: repeat\(2, minmax\(0, 1fr\)\);/s,
    );
    expect(css).toMatch(
      /\.transport-unique-icons > \.transport-settings-button \{[^}]*position: relative;/s,
    );
    expect(css).toMatch(
      /\.transport-group-scale \{[^}]*font-size: 0\.84rem;/s,
    );
    expect(css).toMatch(
      /\.transport-settings-glyph \{[^}]*font-size: 1\.3rem;/s,
    );
    expect(css).toMatch(
      /\.viewer-transport \.transport-run:hover:not\(:disabled\),[\s\S]*\.transport-group-resume:hover:not\(:disabled\)/,
    );
    expect(css).toMatch(
      /\.viewer-transport \.danger-button:hover:not\(:disabled\)/,
    );
    expect(css).toMatch(
      /\.transport-group-scale:has\(input:not\(:disabled\)\):hover/,
    );
    expect(css).toMatch(
      /\.transport-group-scale span \{[^}]*padding-left: 0\.18rem;/s,
    );
    expect(css).toMatch(
      /\.transport-group-scale output \{[^}]*padding-right: 0\.18rem;/s,
    );
  });

  it('exposes manual recovery from the owner projection even in another panel/mode', () => {
    const robot = robotController();
    robot.state.status = statusFixture();
    robot.currentStatus = robot.state.status;
    const operation = {
      programRunId: robot.state.status.directControlGripper!.programRunId,
      operationId: 'faulted-gripper', operationRevision: 2, resident: true,
      state: 'intervention_required' as const, progress: 0.78,
      terminalError: 'Execution timeout exceeded', activeStopId: null, activeStopGeneration: null,
    };
    robot.state.status.manualOperations = [operation];
    const idle = controller('succeeded', 'idle_after_reset');
    idle.readiness = { can_execute: false, can_resume: false, motion_clear: true, blocked_reasons: [] };
    const openRecovery = vi.fn();
    const guidance = (enabled = true) => deriveTransportGuidance({
      enabled, program: idle, robot, motionActive: false,
      onOpenManualRecovery: openRecovery, onReviewProgramRecovery: vi.fn(),
    });
    const [manual] = guidance();
    expect(manual).toMatchObject({
      id: 'recovery.manual.faulted-gripper',
      severity: 'error',
      domain: 'recovery',
      title: 'Manual gripper recovery required',
    });
    expect(manual.message).toContain('Execution timeout exceeded');
    expect(manual.message).toContain('Robot control → Reset operation');
    // The existing guarded action travels with its message; there is no
    // acknowledgement and no second copy beside the transport controls.
    expect(manual.actions?.map((action) => action.label)).toEqual(['Open gripper recovery']);
    expect(deriveTransportGuidance({
      enabled: true, program: idle, robot, motionActive: false,
      observationTransport: { phase: 'no_robot_loaded', message: 'No robot loaded' },
      onReviewProgramRecovery: vi.fn(),
    }).map((notice) => notice.id)).toContain('recovery.manual.faulted-gripper');
    manual.actions?.[0].run();
    expect(openRecovery).toHaveBeenCalledWith(operation);
    const markup = renderToStaticMarkup(<ProgramTransportControls
      enabled program={idle} robot={robot} onOpenRobotControls={vi.fn()} />);
    expect(markup).not.toContain('Open gripper recovery');
    expect(markup).not.toContain('transport-guidance');
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    // Without a current control connection the connection condition reports
    // once; recovery facts are not presented as current.
    expect(guidance(false)).toEqual([]);
    robot.currentStatus = null;
    expect(guidance().map((notice) => notice.title)).toEqual(['Robot status unavailable']);
    robot.currentStatus = robot.state.status;
    operation.resident = false;
    expect(guidance()).toEqual([]);
    for (const state of ['completed', 'stopped', 'cancelled_pre_dispatch'] as const) {
      robot.state.status.manualOperations = [{ ...operation, resident: true, state, terminalError: null }];
      expect(guidance()).toEqual([]);
    }
  });

  it('opens manual recovery of a mounted tool on the tool itself, in gripper mode', () => {
    // Merge 6: a split tool owns only its gripper target; recovery selects the
    // tool, never the robot it is mounted on in Joint mode.
    const robot = Object.assign(robotController(), {
      motionTargets: [{ controllableComponentId: 'lite6_robot', commandTargetId: 'arm' }],
      gripperTargets: [{ controllableComponentId: 'lite_gripper', commandTargetId: 'gripper' }],
      selectRobot: vi.fn(), selectTool: vi.fn(), selectMode: vi.fn(),
    }) as unknown as RobotControlController;
    const operation = (target: string, command: string) => ({
      programRunId: 'manual-run', operationId: `faulted-${command}`, operationRevision: 1,
      resident: true, state: 'intervention_required' as const, progress: 0.5,
      terminalError: 'Execution timeout exceeded', activeStopId: null, activeStopGeneration: null,
      controllableComponentId: target, commandTargetId: command,
    });
    selectManualRecoveryTarget(robot, operation('lite_gripper', 'gripper'));
    expect(robot.selectRobot).toHaveBeenCalledWith('lite_gripper', '');
    expect(robot.selectTool).toHaveBeenCalledWith('gripper');
    expect(robot.selectMode).toHaveBeenCalledWith('gripper');
    // A robot's own motion operation still opens on its motion target.
    vi.mocked(robot.selectRobot).mockClear();
    vi.mocked(robot.selectMode).mockClear();
    selectManualRecoveryTarget(robot, operation('lite6_robot', 'arm'));
    expect(robot.selectRobot).toHaveBeenCalledWith('lite6_robot', 'arm');
    expect(robot.selectMode).toHaveBeenCalledWith('joint');
  });

  it('enables Execute and Resume only from RunReadiness and shows every blocked reason', () => {
    const program = controller('succeeded', 'idle_after_reset');
    program.state.run = null;
    program.state.controls = null;
    const robot = robotController();
    const render = () => renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} />);
    expect(render()).toMatch(/class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*>Execute<\/button>/);
    // The Runner's own Run gate applies too (e.g. a failed scene preparation).
    program.runnerAllows = vi.fn((kind: string) => kind !== 'program_run');
    program.runnerRefusal = vi.fn(() => 'program_scene_preparation_failed');
    expect(render()).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""[^>]*title="program scene preparation failed"/);
    program.runnerAllows = vi.fn(() => true);
    program.runnerRefusal = vi.fn(() => null);
    // A run's motion running is shown by the run, not as a blocked reason.
    program.readiness = { can_execute: false, can_resume: false, motion_clear: true, blocked_reasons: [
      { code: 'run_in_progress', message: 'A run is in progress.' },
      { code: 'program_motion_running', message: 'The run is not stopped.' },
    ] };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={program} />))
      .not.toContain('Execute and Resume are blocked:');
    program.readiness = {
      can_execute: false,
      can_resume: false,
      motion_clear: false,
      blocked_reasons: [
        { code: 'component_not_enabled', message: 'not enabled', component_id: 'lite6_robot' },
        { code: 'rule_holds', message: 'holds', rule_id: 'door' },
      ],
    };
    const markup = render();
    expect(markup).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    expect(markup).toContain('lite6_robot: Component not enabled');
    expect(markup).toContain('door: A rule holds this motion');
    // The details list every reason, with the ids they name.
    const details = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={program} />);
    expect(details).toContain('Execute and Resume are blocked:');
    expect(details).toContain('<li>lite6_robot: Component not enabled</li>');
    expect(details).toContain('<li>door: A rule holds this motion</li>');
    // Unknown readiness is never treated as ready.
    program.readiness = null;
    expect(render()).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""[^>]*title="Execute readiness unavailable: the execution host&#x27;s state can&#x27;t be read."/);
    // Resume after Halt follows can_resume.
    const halted = controller('full_stopped');
    halted.readiness = { can_execute: false, can_resume: false, motion_clear: true, blocked_reasons: [] };
    const haltedMarkup = () => renderToStaticMarkup(<ProgramTransportControls
      enabled program={halted} robot={robot} />);
    expect(haltedMarkup()).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    halted.readiness = { can_execute: false, can_resume: true, motion_clear: true, blocked_reasons: [] };
    expect(haltedMarkup()).toMatch(/class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*title="Resume the halted Program tree/);
  });

  it('disables Execute with its reason while this page\'s assistant request runs', () => {
    const program = controller('succeeded', 'idle_after_reset');
    program.state.run = null;
    program.state.controls = null;
    program.composerRefusalReason = ASSISTANT_TURN_IN_PROGRESS;
    expect(renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robotController()} />))
      .toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""[^>]*title="An assistant request is running: wait or cancel it"/);
  });

  it('offers "Activate components and execute?" only when components not enabled are the only blockers', () => {
    const program = controller('succeeded', 'idle_after_reset');
    program.state.run = null;
    program.state.controls = null;
    program.readiness = { can_execute: false, can_resume: false, motion_clear: false, blocked_reasons: [
      { code: 'component_not_enabled', message: 'not enabled', component_id: 'lite6_robot' },
    ] };
    const robot = robotController();
    const render = () => renderToStaticMarkup(<ProgramTransportControls enabled program={program} robot={robot} />);
    // The hook says whether Activate clears every blocker (another blocker: disabled).
    expect(render()).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
    program.activation = { ...program.activation, available: true };
    expect(render()).toMatch(/class="transport-run transport-unique-control"(?![^>]*disabled)[^>]*title="Activate the components that are not enabled, then execute the Program once./);
    expect(render()).not.toContain('Activate components and execute?');
    program.activation = { ...program.activation, flow: { phase: 'offered', componentIds: ['lite6_robot'], failures: [], recovery: {} } };
    const open = render();
    expect(open).toContain('Activate components and execute?');
    expect(open).toMatch(/class="transport-run transport-unique-control"[^>]*disabled=""/);
  });

  it('points Program faults to recovery and normal terminal runs to Reset', () => {
    const program = controller('intervention_required', 'recovery_required');
    program.state.run!.message = 'Participant requires reconciliation';
    program.cycleFailure = {
      code: 'program_runtime_control_outcome_unknown',
      command_kind: 'resume_motion',
      message: 'The Resume Motion outcome could not be confirmed. Reset is required.',
    };
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const review = vi.fn();
    const guidance = () => deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false,
      onReviewProgramRecovery: review,
    });
    const [recovery] = guidance();
    expect(recovery).toMatchObject({
      id: 'recovery.program', severity: 'error', title: 'Program recovery required',
      message: 'The Resume Motion outcome could not be confirmed. Reset is required.',
    });
    expect(recovery.actions?.map((action) => action.label)).toEqual(['Review Program recovery']);
    recovery.actions?.[0].run();
    expect(review).toHaveBeenCalledOnce();
    expect(renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} />))
      .toMatch(/class="transport-reset transport-unique-control meaningful"(?![^>]*disabled)/);
    program.cycleState = 'terminal_awaiting_reset';
    // A normal finished run is information, not a warning or error.
    program.cycleFailure = null;
    expect(guidance()).toEqual([expect.objectContaining({
      id: 'program.reset-required', severity: 'info', title: 'Program finished',
    })]);
    // The run's end reason names why it ended.
    const ended = (runEnd: NonNullable<ProgramRunStatus['runEnd']>) => {
      program.state.run!.runEnd = runEnd;
      return guidance()[0];
    };
    expect(ended({ end_reason: 'completed', details: {} }))
      .toMatchObject({ id: 'program.reset-required', severity: 'info', title: 'Completed' });
    expect(ended({ end_reason: 'failed', details: { node_instance_id: 'move',
      error: { code: 'rule_holds', message: 'held' } } }))
      .toMatchObject({ severity: 'warning', title: 'Ended by failure at move: A rule holds this motion' });
    expect(ended({ end_reason: 'stopped', details: { stopped_by: 'rule', rule_id: 'door' } }))
      .toMatchObject({ severity: 'warning', title: 'Stopped by rule door' });
    expect(ended({ end_reason: 'stopped', details: { stopped_by: 'operator' } }))
      .toMatchObject({ severity: 'warning', title: 'Stopped by the operator' });
    expect(ended({ end_reason: 'cell_fault', details: { component_id: 'lite6_robot',
      error: { code: 'fault_estop', message: 'e-stop' } } }))
      .toMatchObject({ severity: 'error', title: 'Ended by a cell fault: lite6_robot (Emergency stop active)' });
  });

  it('shows the run end reason in the run status tile', () => {
    const program = controller('failed', 'terminal_awaiting_reset');
    program.state.run!.runEnd = { end_reason: 'cell_fault', details: { component_id: 'lite6_robot' } };
    const markup = renderToStaticMarkup(<ProgramRuntimePanel enabled controller={program} />);
    expect(markup).toContain('<strong class="run-end tone-error">Ended by a cell fault</strong>');
    expect(markup).toContain('Ended by a cell fault: lite6_robot · ');
    program.state.run!.runEnd = { end_reason: 'completed', details: {} };
    expect(renderToStaticMarkup(<ProgramRuntimePanel enabled controller={program} />))
      .toContain('<strong class="run-end tone-neutral">Completed</strong>');
  });

  it('names the held node failure and its details in the Program recovery notice', () => {
    const program = controller('intervention_required');
    const offer = ordinaryMotionStopOffer();
    program.state.controls = {
      ...ordinaryMotionStopControls(),
      recoveryOffer: {
        ...offer,
        recoveryClass: 'node_failure_pause',
        reason: {
          code: 'owner_request_failed',
          message: 'Move to pick was refused.',
          details: { reference_type: 'component_frame', reference_id: 'tool0', component_id: 'arm' },
          origin: { kind: 'owner', nodeInstanceId: 'move', ownerApi: 'kernel.operation' },
        },
      },
    };
    // Another branch's failure in the run status does not replace the held node's own.
    program.state.run!.reason = {
      code: 'program_tree_failed',
      message: 'The Program paused on a failed node.',
      details: null,
      origin: { kind: 'program_runner', nodeInstanceId: null, ownerApi: null },
    };
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const [recovery] = deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false, onReviewProgramRecovery: vi.fn(),
    });
    expect(recovery).toMatchObject({
      id: 'recovery.program',
      message: "Move to pick was refused. (Reference: component_frame 'tool0' of component 'arm'; "
        + "Origin: kernel.operation for node 'move')",
    });
  });

  it('shows the cell latch as one error item with every cause and no recovery action', () => {
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const program = controller('succeeded', 'idle_after_reset');
    const guidance = () => deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false,
      onReviewProgramRecovery: vi.fn(), onReset: { run: vi.fn(), disabled: false },
    });
    expect(guidance()).toEqual([]);
    program.cellLatch = {
      latched: true,
      causes: [
        { component_id: 'lite6_robot', kind: 'fault', fault: {
          code: 'fault_estop', message: 'e-stop', clearable: true, operator_action: 'enable', since_ns: 1 } },
        { component_id: 'plc_io', kind: 'offline' },
        { component_id: 'gripper', kind: 'activity_unknown' },
      ],
    };
    const [latch] = guidance();
    expect(latch).toMatchObject({
      id: 'cell.latch', severity: 'error',
      title: '3 errors require attention · lite6_robot: Emergency stop active',
    });
    expect(latch.message).toContain('plc_io: offline.');
    expect(latch.message).toContain('gripper: can\'t report whether it is still moving.');
    // No acknowledgement, no Recover cell: the latch clears by itself.
    expect(latch.actions).toBeUndefined();
    const details = renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} />);
    expect(details).toMatch(/class="transport-run transport-unique-control"/);
    program.cellLatch = { latched: false, causes: [] };
    expect(guidance()).toEqual([]);
  });

  it('classifies live-view observation by what the operator can rely on', () => {
    const robot = robotController();
    robot.currentStatus = statusFixture();
    const guidance = (phase: 'synchronizing' | 'waiting' | 'reconnecting' | 'live') => (
      deriveTransportGuidance({
        enabled: true, program: controller(), robot, motionActive: false,
        observationTransport: { phase, message: null },
        onReviewProgramRecovery: vi.fn(),
      }).filter((notice) => notice.id === 'observation.live-view')
    );
    expect(guidance('synchronizing')[0]).toMatchObject({ severity: 'info', busy: true });
    expect(guidance('waiting')[0]).toMatchObject({ severity: 'info' });
    // Stale poses are an error; only a known controlled transition may
    // present them as information (see classifyWorkbenchNotices).
    expect(guidance('reconnecting')[0]).toMatchObject({
      severity: 'error',
      expectedDuringTransition: true,
      incidentKind: 'viewer_observation_stale',
    });
    expect(guidance('live')).toEqual([]);
  });

  it('presents verified empty as no robot loaded without an error or incident', () => {
    const program = controller('succeeded', 'idle_after_reset');
    const robot = robotController();
    robot.currentStatus = null;
    const observationTransport = { phase: 'no_robot_loaded' as const, message: 'No robot loaded' };
    expect(deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false, observationTransport,
      onReviewProgramRecovery: vi.fn(),
    })).toEqual([]);
    const markup = renderToStaticMarkup(<ProgramTransportControls
      enabled program={program} robot={robot} observationTransport={observationTransport} />);
    expect(markup).toContain('No robot loaded');
    expect(markup).not.toContain('Robot status unavailable');
    for (const phase of ['waiting', 'reconnecting'] as const) {
      const notices = deriveTransportGuidance({
        enabled: true, program, robot, motionActive: false,
        observationTransport: { phase, message: null }, onReviewProgramRecovery: vi.fn(),
      });
      expect(notices.some((notice) => notice.id === 'recovery.robot-status')).toBe(true);
      if (phase === 'reconnecting') {
        expect(notices.some((notice) => notice.incidentKind === 'viewer_observation_stale')).toBe(true);
      }
    }
  });

  it('keeps real cell and Program recovery visible alongside verified empty', () => {
    const program = controller('intervention_required', 'recovery_required');
    const robot = robotController();
    robot.state.status = statusFixture();
    program.cellLatch = { latched: true, causes: [{ component_id: 'lite6_robot', kind: 'offline' }] };
    robot.currentStatus = null;
    const notices = deriveTransportGuidance({
      enabled: true, program, robot, motionActive: false,
      observationTransport: { phase: 'no_robot_loaded', message: 'No robot loaded' },
      onReviewProgramRecovery: vi.fn(),
    });
    expect(notices.map((notice) => notice.id)).toEqual(['cell.latch', 'recovery.program']);
    expect(notices.every((notice) => notice.severity === 'error')).toBe(true);
  });

  it('starts fresh pose fields when the runtime blackboard switches key', async () => {
    // Minimal mounted React DOM host (see ProgramSchemaValueEditor.mounted.test.tsx).
    const created: Record<string, unknown>[] = [];
    const document: Record<string, unknown> = {
      nodeType: 9, activeElement: null, defaultView: {},
      addEventListener: () => undefined, removeEventListener: () => undefined,
      createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
    };
    const node = (tag: string): Record<string, unknown> => {
      const element: Record<string, unknown> = {
        nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
        namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document, style: {}, options: [],
        addEventListener: () => undefined, removeEventListener: () => undefined,
        appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
        setAttribute: () => undefined, removeAttribute: () => undefined,
      };
      created.push(element);
      return element;
    };
    document.createElement = node;
    document.documentElement = node('html');
    const window = {
      document, HTMLIFrameElement: class {},
      addEventListener: () => undefined, removeEventListener: () => undefined,
      setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
    };
    document.defaultView = window;
    vi.stubGlobal('window', window);
    vi.stubGlobal('document', document);
    vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
    type Props = Record<string, unknown> & Partial<Record<
      'onFocus' | 'onChange' | 'onBlur', (event: unknown) => void>>;
    const props = (element: Record<string, unknown>) => element[
      Object.keys(element).find((name) => name.startsWith('__reactProps$'))!] as Props;
    const control = (match: (candidate: Props) => boolean) => created
      .filter((element) => Object.keys(element).some((name) => name.startsWith('__reactProps$')))
      .filter((element) => match(props(element))).at(-1)!;
    const positionX = () => control((candidate) => candidate['aria-label'] === 'Position x');
    const writeButton = () => control((candidate) => candidate.children === 'Write current-run value');

    const program = controller('running');
    program.state.applied!.program.blackboard = {
      namespaces: [],
      keys: [
        { key: 'pick_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
        { key: 'place_pose', type: 'stored_pose', access: 'read_write', persistence: 'run' },
      ],
    };
    const { act } = await import('react');
    const { createRoot } = await import('react-dom/client');
    const root = createRoot(node('div') as unknown as Element);
    try {
      await act(() => { root.render(<ProgramRuntimePanel enabled controller={program} />); });
      const chooseKey = (key: string) => act(() => {
        const target = { value: key };
        props(control((candidate) => Array.isArray(candidate.children)
          && (candidate.children[0] as { props?: { children?: unknown } } | undefined)
            ?.props?.children === 'Choose a key')).onChange!({ target, currentTarget: target });
      });
      await chooseKey('pick_pose');
      const target = { value: 'abc' };
      await act(() => { props(positionX()).onFocus!({ target, currentTarget: target }); });
      await act(() => { props(positionX()).onChange!({ target, currentTarget: target }); });
      await act(() => { props(positionX()).onBlur!({ target, currentTarget: target }); });
      expect(props(writeButton()).disabled).toBe(true);

      await chooseKey('place_pose');

      // What Write sends is what is shown: place_pose's neutral pose.
      expect(props(positionX()).value).toBe('0');
      expect(props(writeButton()).disabled).toBe(false);
    } finally {
      await act(() => { root.unmount(); });
      vi.unstubAllGlobals();
    }
  });
});


it('shows a failed accepted start and native comparison facts while Reset remains required', () => {
  const program = controller('failed', 'terminal_awaiting_reset');
  program.state.run = null; // Start failed before a host RunStatus existed.
  program.cycleFailure = {
    code: 'component_lifecycle_facts_stale',
    message: 'Program run program-cycle.2 started, then failed. Reset the Program cycle.',
    details: { admission_changed: 'true', admission_generation_before: '36', admission_generation_after: '37',
      snapshot_changed: 'false', program_run_id: 'program-cycle.2', claim_generation: 9, needs_reset: true },
  };
  const robot = robotController();
  robot.currentStatus = statusFixture();
  const reset = vi.fn();
  const [notice] = deriveTransportGuidance({
    enabled: true, program, robot, motionActive: false, onReviewProgramRecovery: vi.fn(),
    onReset: { run: reset, disabled: false },
  });
  expect(notice).toMatchObject({ id: 'program.reset-required', severity: 'error', title: 'Run failed' });
  // Red always carries the action that clears it: the Program-panel Reset.
  expect(notice.actions?.map((action) => [action.label, action.disabled])).toEqual([['Reset', false]]);
  notice.actions?.[0].run();
  expect(reset).toHaveBeenCalledOnce();
  expect(notice.message).toContain('program-cycle.2');
  expect(notice.message).toContain('Admission changed: true');
  expect(notice.message).toContain('Admission generation before: 36');
  expect(notice.message).toContain('Admission generation after: 37');
  expect(notice.message).toContain('Snapshot changed: false');
  expect(notice.message).toContain('Claim generation: 9');
});

/**
 * Mounts the transport controls on a stub DOM. `press` runs the onClick of the
 * last mounted control whose props match; `mounted` says whether one was
 * mounted; `unmount` restores the globals.
 */
async function mountTransport(program: ProgramRuntimeController, robot = robotController()) {
  const created: Record<string, unknown>[] = [];
  const document: Record<string, unknown> = {
    nodeType: 9, activeElement: null, defaultView: {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    createTextNode: (text: string) => ({ nodeType: 3, nodeValue: text, ownerDocument: document }),
  };
  const node = (tag: string): Record<string, unknown> => {
    const element: Record<string, unknown> = {
      nodeType: 1, nodeName: tag.toUpperCase(), tagName: tag.toUpperCase(),
      namespaceURI: 'http://www.w3.org/1999/xhtml', ownerDocument: document,
      style: { setProperty: () => undefined }, options: [],
      addEventListener: () => undefined, removeEventListener: () => undefined,
      appendChild: () => undefined, insertBefore: () => undefined, removeChild: () => undefined,
      setAttribute: () => undefined, removeAttribute: () => undefined,
    };
    created.push(element);
    return element;
  };
  document.createElement = node;
  document.documentElement = node('html');
  const window = {
    document, HTMLIFrameElement: class {},
    addEventListener: () => undefined, removeEventListener: () => undefined,
    setTimeout: globalThis.setTimeout, clearTimeout: globalThis.clearTimeout,
    requestAnimationFrame: () => 0,
  };
  document.defaultView = window;
  vi.stubGlobal('window', window);
  vi.stubGlobal('document', document);
  vi.stubGlobal('IS_REACT_ACT_ENVIRONMENT', true);
  type Props = Record<string, unknown> & { onClick?: () => void };
  const propsOf = (element: Record<string, unknown>) => element[
    Object.keys(element).find((name) => name.startsWith('__reactProps$')) ?? ''] as Props | undefined;
  const { act } = await import('react');
  const { createRoot } = await import('react-dom/client');
  const root = createRoot(node('div') as unknown as Element);
  await act(() => { root.render(<ProgramTransportControls enabled program={program} robot={robot} />); });
  return {
    rerender: async (resetReloadRequest = 0) => { await act(() => { root.render(<ProgramTransportControls enabled program={program} robot={robot} resetReloadRequest={resetReloadRequest} />); }); },
    press: async (match: (props: Props) => boolean) => {
      const found = created.map(propsOf).filter((candidate): candidate is Props => (
        candidate !== undefined && match(candidate)
      )).at(-1);
      if (found === undefined) throw new Error('Mounted control not found');
      await act(() => { found.onClick!(); });
    },
    change: async (match: (props: Props) => boolean, event: unknown) => {
      const found = created.map(propsOf).filter((candidate): candidate is Props => (
        candidate !== undefined && match(candidate)
      )).at(-1);
      if (found === undefined) throw new Error('Mounted control not found');
      await act(() => { (found.onChange as (value: unknown) => void)(event); });
    },
    /** Whether a control whose props match was ever mounted. */
    mounted: (match: (props: Props) => boolean) => created.map(propsOf).some((candidate) => (
      candidate !== undefined && match(candidate)
    )),
    unmount: async () => {
      await act(() => { root.unmount(); });
      vi.unstubAllGlobals();
    },
  };
}

describe('Reset confirmation and persistent keys', () => {
  it('notice opens Reset with reload selected without sending Reset', async () => {
    const program = controller('succeeded', 'idle_after_reset');
    const open = await mountTransport(program);
    try {
      let resetReloadRequest = 0;
      const notice = initialSceneNotice({ state: 'unknown', reason: { code: 'offline', message: 'connection lost' } }, () => { resetReloadRequest += 1; });
      expect(notice?.title).toBe('Initial scene may be incomplete');
      notice!.actions![0].run();
      await open.rerender(resetReloadRequest);
      expect(open.mounted((props) => props.id === 'program-reset-confirmation-title')).toBe(true);
      expect(open.mounted((props) => props.name === 'reloadInitialScene' && props.checked === true)).toBe(true);
      expect(program.reset).not.toHaveBeenCalled();
    } finally { await open.unmount(); }
  });

  it('mentions and lists persistent keys only when the Program has them and Reload is checked', async () => {
    for (const persistence of ['run', 'persistent'] as const) {
      const program = controller('intervention_required');
      program.state.controls = ordinaryMotionStopControls();
      program.state.applied!.program.blackboard = {
        namespaces: [],
        keys: [{ key: 'layer', type: 'int64', access: 'read_write', persistence, initial: 0 }],
      };
      vi.mocked(program.persistentValues.read).mockReturnValue(new Promise<never>(() => undefined));
      const open = await mountTransport(program);
      try {
        await open.press((props) => props.children === 'Reset'
          && String(props.className).startsWith('transport-reset'));
        expect(open.mounted((props) => props.id === 'program-reset-confirmation-title')).toBe(true);
        const note = (props: Record<string, unknown>) => props.className === 'transport-reset-option-note'
          && JSON.stringify(props.children ?? null).includes('Persistent keys return');
        expect(open.mounted(note)).toBe(persistence === 'persistent');
        // The stored-value list is read only once Reload initial scene is checked.
        expect(program.persistentValues.read).not.toHaveBeenCalled();
        await open.change((props) => props.name === 'reloadInitialScene',
          { target: { checked: true } });
        expect(vi.mocked(program.persistentValues.read).mock.calls.length > 0).toBe(persistence === 'persistent');
      } finally {
        await open.unmount();
      }
    }
  });

  it('disables Reload initial scene, with the reason, while no applied Program is available', async () => {
    for (const applied of [true, false]) {
      const program = controller('succeeded', 'idle_after_reset');
      if (!applied) program.state.applied = null;
      const open = await mountTransport(program);
      try {
        await open.press((props) => props.children === 'Reset'
          && String(props.className).startsWith('transport-reset'));
        expect(open.mounted((props) => props.id === 'program-reset-confirmation-title')).toBe(true);
        const reload = (props: Record<string, unknown>) => props.name === 'reloadInitialScene';
        expect(open.mounted((props) => reload(props) && props.disabled === !applied)).toBe(true);
        expect(open.mounted((props) => reload(props) && props.checked === false)).toBe(true);
        const reason = 'No applied Program is available, so there is no initial scene to reload. Load a Scenario first.';
        expect(open.mounted((props) => props.className === 'toggle-control' && props.title === reason))
          .toBe(!applied);
        // The dialog sends what it shows (`resetSelectionApplied`): without an
        // applied Program, Reload stays unselected even if a change slips through.
        if (!applied) {
          await open.change(reload, { target: { checked: true } });
          expect(open.mounted((props) => reload(props) && props.checked === true)).toBe(false);
        }
      } finally {
        await open.unmount();
      }
    }
  });
});

describe('Halt', () => {
  const runningProgram = () => {
    const program = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    controls.executionSelections.groups[0].state = 'running';
    controls.executionSelections.groups[0].stopAllowed = true;
    program.state.controls = controls;
    return program;
  };
  const haltButton = (props: Record<string, unknown>) => String(props.className)
    .startsWith('danger-button transport-full-stop');
  const dialogOpen = (props: Record<string, unknown>) => props.id === 'program-halt-confirmation-title';
  const confirmButton = (props: Record<string, unknown>) => props.children === 'Halt Program';
  const text = (props: Record<string, unknown>) => [props.children].flat()
    .filter((child) => typeof child === 'string').join('');
  const waiting = (props: Record<string, unknown>) => props.role === 'status'
    && props.children === 'Waiting for all motion groups to stop…';
  const motionStopped = (props: Record<string, unknown>) => text(props).includes('Motion is stopped. ');
  const stopNotConfirmed = (props: Record<string, unknown>) => props.className === 'inline-message severity-error'
    && props.role === 'alert' && text(props).includes('Motion Stop not confirmed');

  it.each([true, false])('sends the motion Stop at once and then asks only about the Full Stop (motion Stop accepted=%s)', async (accepted) => {
    const program = runningProgram();
    // Even with another command pending, Halt is not refused.
    program.state.pending = 'reset';
    program.state.controlPending = 'runtime-scale';
    vi.mocked(program.stopMotion).mockResolvedValue(accepted);
    const open = await mountTransport(program);
    try {
      await open.press(haltButton);
      await open.rerender();
      expect(program.stopMotion).toHaveBeenCalledTimes(1);
      expect(program.stopMotion).toHaveBeenCalledWith(['all']);
      // A refused motion Stop does not end the Halt: the question follows
      // and says so in the panel's error style.
      expect(open.mounted(dialogOpen)).toBe(true);
      expect(open.mounted(stopNotConfirmed)).toBe(!accepted);
      // Motion is still moving: the dialog says it is waiting, never that it stopped.
      expect(open.mounted(waiting)).toBe(true);
      expect(open.mounted(motionStopped)).toBe(false);
      expect(program.fullStop).not.toHaveBeenCalled();
      // The confirm button is clickable before all motion has stopped.
      expect(open.mounted((props) => confirmButton(props) && !props.disabled)).toBe(true);
      await open.press(confirmButton);
      expect(program.fullStop).toHaveBeenCalledTimes(1);
    } finally { await open.unmount(); }
  });

  it('Cancel leaves the Program held after its motion Stop; a repeated Halt sends another', async () => {
    const program = runningProgram();
    vi.mocked(program.stopMotion).mockResolvedValue(true);
    const open = await mountTransport(program);
    try {
      await open.press(haltButton);
      await open.press((props) => props.children === 'Cancel');
      expect(program.fullStop).not.toHaveBeenCalled();
      await open.press(haltButton);
      expect(program.stopMotion).toHaveBeenCalledTimes(2);
      expect(program.fullStop).not.toHaveBeenCalled();
    } finally { await open.unmount(); }
  });

  it('asks at once when all motion is already stopped', async () => {
    const program = controller('intervention_required');
    program.state.controls = ordinaryMotionStopControls();
    const open = await mountTransport(program);
    try {
      await open.press(haltButton);
      expect(program.stopMotion).not.toHaveBeenCalled();
      expect(open.mounted(dialogOpen)).toBe(true);
      expect(open.mounted(motionStopped)).toBe(true);
      expect(open.mounted(waiting)).toBe(false);
    } finally { await open.unmount(); }
  });

  it('names the refusal of the motion Stop in the dialog', async () => {
    const program = runningProgram();
    program.state.error = 'Owner refused the Stop.';
    vi.mocked(program.stopMotion).mockResolvedValue(false);
    const open = await mountTransport(program);
    try {
      await open.press(haltButton);
      await open.rerender();
      expect(open.mounted((props) => stopNotConfirmed(props)
        && text(props).includes('Owner refused the Stop.'))).toBe(true);
      expect(open.mounted((props) => confirmButton(props) && !props.disabled)).toBe(true);
    } finally { await open.unmount(); }
  });

  it.each(['full_stopping', 'stopping'] as const)('repeats Stop and Halt while the run is %s (the owner answers with no effect or joins)', async (runState) => {
    const program = runningProgram();
    program.state.run!.state = runState;
    if (runState === 'stopping') program.cycleState = 'stopping';
    program.state.controls!.executionSelections.groups[0].state = 'stopping';
    vi.mocked(program.stopMotion).mockResolvedValue(true);
    const open = await mountTransport(program);
    try {
      expect(open.mounted((props) => haltButton(props) && !props.disabled)).toBe(true);
      await open.press((props) => String(props.className) === 'danger-button transport-group-stop'
        && !props.disabled);
      expect(program.stopMotion).toHaveBeenCalledWith(['all']);
      await open.press(haltButton);
      // Motion is not yet stopped: Halt sends its motion Stop, then asks.
      expect(program.stopMotion).toHaveBeenCalledTimes(2);
      expect(open.mounted(dialogOpen)).toBe(true);
      await open.press(confirmButton);
      expect(program.fullStop).toHaveBeenCalledTimes(1);
    } finally { await open.unmount(); }
  });

  it('keeps group Stop unavailable for a held group while the run is stopping', () => {
    const program = runningProgram();
    program.state.run!.state = 'stopping';
    program.cycleState = 'stopping';
    program.state.controls!.executionSelections.groups[0].state = 'held';
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={program} robot={robotController()} />,
    );
    expect(markup).toMatch(/class="danger-button transport-group-stop"[^>]*disabled=""[^>]*>Stopped<\/button>/);
    expect(markup).toMatch(/class="danger-button transport-full-stop transport-unique-control"(?![^>]*disabled)[^>]*>Halt<\/button>/);
  });

  describe('without confirmation', () => {
    afterEach(() => { composerSettingsOverride.confirmHalt = null; });

    it.each([true, false])('sends the Full Stop once motion has stopped, or at once if the motion Stop fails (accepted=%s)', async (accepted) => {
      composerSettingsOverride.confirmHalt = false;
      const program = runningProgram();
      vi.mocked(program.stopMotion).mockResolvedValue(accepted);
      const open = await mountTransport(program);
      try {
        await open.press(haltButton);
        await open.rerender();
        expect(program.stopMotion).toHaveBeenCalledWith(['all']);
        expect(open.mounted(dialogOpen)).toBe(false);
        // Accepted: the Full Stop waits until all motion has stopped.
        expect(program.fullStop).toHaveBeenCalledTimes(accepted ? 0 : 1);
        program.state.controls!.executionSelections.groups[0].state = 'held';
        await open.rerender();
        expect(program.fullStop).toHaveBeenCalledTimes(1);
      } finally { await open.unmount(); }
    });
  });
});

describe('Per-hold Retry and Continue', () => {
  /** Two failure holds, left and right, each owning its own group; middle is owned by none. */
  function twoHolds(): ProgramRuntimeController {
    const program = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    const base = controls.executionSelections.groups[0];
    controls.executionSelections.groups = ['all', 'left', 'middle', 'right'].map((groupId) => ({
      ...base, groupId, resumeAllowed: true, resumeUnavailableReason: null,
    }));
    const holdOffer = (node: string, digit: string): CycleRecoveryOffer => ({
      ...ordinaryMotionStopOffer(),
      offerId: `sha256:${digit.repeat(64)}`,
      offerRevision: Number(digit),
      nodeInstanceId: node,
      recoveryClass: 'node_failure_pause',
      actions: [{
        actionId: 'retry_motion_from_current_state',
        requiresConfirmation: false, performsBackendReset: false,
        enabled: true, disabledReasonCode: null,
      }, {
        actionId: 'continue_with_failure',
        requiresConfirmation: true, performsBackendReset: false,
        enabled: true, disabledReasonCode: null,
      }],
    });
    controls.recoveryOffer = holdOffer('left.move', '1');
    controls.recoveryHolds = [
      { motionGroupIds: ['left'], interventionStopScope: 'own_groups', offer: holdOffer('left.move', '1') },
      { motionGroupIds: ['right'], interventionStopScope: 'own_groups', offer: holdOffer('right.move', '2') },
    ];
    program.state.controls = controls;
    return program;
  }

  it('renders Retry and a red Continue per owning hold and disables groups owned by none', () => {
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={twoHolds()} robot={robotController()} />,
    );
    expect(markup).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*title="Retry left.move and let its groups continue."[^>]*>Retry<\/button>/,
    );
    expect(markup).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*title="Retry right.move and let its groups continue."[^>]*>Retry<\/button>/,
    );
    expect(markup).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="middle waits for held nodes left.move, right.move: Retry or Continue them first."[^>]*>Resume<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*title="Continuing fails the node"[^>]*aria-label="Continue left: fail held node left.move"[^>]*>Continue<\/button>/,
    );
    expect(markup).toContain('aria-label="Continue right: fail held node right.move"');
    // Groups no hold owns keep their ordinary Stop.
    expect(markup.match(/>Continue<\/button>/g)).toHaveLength(2);
    expect(markup).toMatch(/class="danger-button transport-group-stop"[^>]*>Stopped<\/button>/);

    // Continue the backend does not offer, or disables, keeps Stop or shows why.
    const noContinue = twoHolds();
    for (const hold of noContinue.state.controls!.recoveryHolds) {
      hold.offer.actions = hold.offer.actions.slice(0, 1);
    }
    expect(renderToStaticMarkup(
      <ProgramTransportControls enabled program={noContinue} robot={robotController()} />,
    )).not.toContain('>Continue</button>');
    const disabled = twoHolds();
    disabled.state.controls!.recoveryHolds[0].offer.actions[1] = {
      ...disabled.state.controls!.recoveryHolds[0].offer.actions[1],
      enabled: false, disabledReasonCode: 'recovery_close_not_eligible',
    };
    expect(renderToStaticMarkup(
      <ProgramTransportControls enabled program={disabled} robot={robotController()} />,
    )).toMatch(/class="danger-button transport-group-stop"[^>]*disabled=""[^>]*title="Continuing fails the node"[^>]*aria-label="Continue left/);
  });

  it('lets a hold in no named group own every group no other hold names (item 43)', () => {
    const program = twoHolds();
    const [left, right] = program.state.controls!.recoveryHolds;
    program.state.controls!.recoveryHolds = [
      { ...right, motionGroupIds: ['all'], interventionStopScope: 'all' },
      left,
    ];
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={program} robot={robotController()} />,
    );
    // left stays with its own hold; middle and right go to the unnamed hold.
    expect(markup).toContain('aria-label="Continue left: fail held node left.move"');
    expect(markup).toContain('aria-label="Continue middle: fail held node right.move"');
    expect(markup).toContain('aria-label="Continue right: fail held node right.move"');
    expect(markup).not.toContain('waits for held');
    expect(markup.match(
      /title="Retry right.move and let the Program&#x27;s motion continue, except groups other held nodes own."[^>]*>Retry<\/button>/g,
    )?.length).toBeGreaterThanOrEqual(2);
  });

  it('gives a running group under a hold its ordinary Stop, without the waiting tooltip', () => {
    const program = twoHolds();
    const middle = program.state.controls!.executionSelections.groups
      .find((group) => group.groupId === 'middle')!;
    middle.state = 'running';
    middle.stopAllowed = true;
    middle.resumeAllowed = false;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={program} robot={robotController()} />,
    );
    expect(markup).toMatch(
      /class="transport-group-resume"[^>]*disabled=""[^>]*title="middle motion is running\."[^>]*>Running<\/button>/,
    );
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*title="Stop motion in middle without resetting the Program\."[^>]*>Stop<\/button>/,
    );
    expect(markup).not.toContain('middle waits for held');
  });

  it('keeps an operator Stop Retry on the group button, as before the per-hold change', () => {
    const stopped = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer!.actions = [{
      actionId: 'retry_motion_from_current_state', requiresConfirmation: false,
      performsBackendReset: false, enabled: true, disabledReasonCode: null,
    }, {
      actionId: 'reset_cycle', requiresConfirmation: true,
      performsBackendReset: true, enabled: true, disabledReasonCode: null,
    }];
    stopped.state.controls = controls;
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={stopped} robot={robotController()} />,
    );
    expect(markup).toMatch(
      /class="transport-group-resume"(?![^>]*disabled)[^>]*title="Retry root and let all the Program&#x27;s motion continue."[^>]*>Retry<\/button>/,
    );
    expect(markup).not.toContain('>Continue</button>');
    expect(markup).not.toContain('waits for held');
  });

  it('sends an operator Stop Retry for its own offer from the group button', async () => {
    const stopped = controller('intervention_required');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer!.actions = [{
      actionId: 'retry_motion_from_current_state', requiresConfirmation: false,
      performsBackendReset: false, enabled: true, disabledReasonCode: null,
    }];
    stopped.state.controls = controls;
    const transport = await mountTransport(stopped);
    try {
      await transport.press((props) => props.title === 'Retry root and let all the Program\'s motion continue.');
      expect(stopped.applyRecoveryAction).toHaveBeenCalledExactlyOnceWith(
        controls.recoveryOffer, 'retry_motion_from_current_state',
      );
    } finally {
      await transport.unmount();
    }
  });

  it('sends each group its own hold offer, and Continue only after confirmation', async () => {
    const program = twoHolds();
    const [left, right] = program.state.controls!.recoveryHolds;
    const transport = await mountTransport(program);
    try {
      await transport.press((props) => props.title === 'Retry right.move and let its groups continue.');
      await transport.press((props) => props.title === 'Retry left.move and let its groups continue.');
      expect(program.applyRecoveryAction).toHaveBeenNthCalledWith(1, right.offer, 'retry_motion_from_current_state');
      expect(program.applyRecoveryAction).toHaveBeenNthCalledWith(2, left.offer, 'retry_motion_from_current_state');

      // Continue asks first; Cancel sends nothing.
      await transport.press((props) => props['aria-label'] === 'Continue right: fail held node right.move');
      expect(program.applyRecoveryAction).toHaveBeenCalledTimes(2);
      await transport.press((props) => props.children === 'Cancel');
      expect(program.applyRecoveryAction).toHaveBeenCalledTimes(2);
      await transport.press((props) => props['aria-label'] === 'Continue right: fail held node right.move');
      await transport.press((props) => props.className === 'danger-button' && props.children === 'Continue');
      expect(program.applyRecoveryAction).toHaveBeenCalledTimes(3);
      expect(program.applyRecoveryAction).toHaveBeenLastCalledWith(right.offer, 'continue_with_failure');
    } finally {
      await transport.unmount();
    }
  });

  /** Both holds reserve robot.joint_1. */
  function heldWithAlarms(): ProgramRuntimeController {
    const program = twoHolds();
    const controls = program.state.controls!;
    for (const offer of [controls.recoveryOffer!, ...controls.recoveryHolds.map((hold) => hold.offer)]) {
      offer.participants = [{ operation_participant_id: 'p1', resource_ids: ['robot.joint_1'] }];
    }
    return program;
  }

  it('never gates Continue or Retry on alarm acknowledgement', () => {
    const markup = renderToStaticMarkup(
      <ProgramTransportControls enabled program={heldWithAlarms()} robot={robotController()} />,
    );
    expect(markup).toMatch(
      /class="danger-button transport-group-stop"(?![^>]*disabled)[^>]*title="Continuing fails the node"[^>]*aria-label="Continue left/,
    );
  });

  it('shows the Recovery block cause first, with each command\'s reason', () => {
    const program = heldWithAlarms();
    program.cycleState = 'recovery_required';
    program.cycleFailure = { code: 'ik_failed', message: 'RoboPlan could not resolve the pose goal' };
    const command = (label: string, reason: string | null = null) => ({
      label, enabled: reason === null, reason, run: vi.fn(),
    });
    const markup = renderToStaticMarkup(
      <ProgramRuntimePanel
        enabled
        controller={program}
        recovery={{
          retry: command('Retry', 'Retry is unavailable: not allowed.'),
          continue: command('Continue'),
          reset: command('Reset'),
        }}
      />,
    );
    // The cycle failure is an error, not a green status line.
    expect(markup).toContain(
      '<div class="program-feedback error-state" role="status"><span>RoboPlan could not resolve the pose goal</span></div>',
    );
    expect(markup).toContain('class="runtime-control-section program-recovery-failed"');
    expect(markup).toMatch(/<h3 id="runtime-recovery-title">Recovery<\/h3>.*<p class="program-recovery-cause">RoboPlan could not resolve the pose goal<\/p>/);
    const order = [
      'Fix the cause.',
      'Apply then Resets the run',
      '>Retry</button><span>Re-run the node.</span><span class="inline-message severity-error">Retry is unavailable: not allowed.',
      '>Continue</button><span>Fail the node; the Program tree handles the FAILURE.</span></div>',
      '>Reset</button><span>End the run.</span></div>',
      'Retry limit',
      'Stage a manual result (not available yet)',
    ].map((text) => markup.indexOf(text));
    expect(order.every((index) => index >= 0)).toBe(true);
    expect([...order].sort((left, right) => left - right)).toEqual(order);
    expect(markup).not.toContain('Acknowledge');
  });

  it('takes a held failure\'s cause from its offer while the cycle is still running', () => {
    // The live hold shape: the run waits for intervention, the cycle has no failure.
    const render = (recoveryClass: string) => {
      const program = heldWithAlarms();
      program.cycleState = 'running';
      program.cycleFailure = null;
      const offer = program.state.controls!.recoveryOffer!;
      offer.recoveryClass = recoveryClass;
      offer.reason = {
        code: 'ik_failed',
        message: 'RoboPlan could not resolve the pose goal (native_ik_no_solution)',
        details: null,
        origin: null,
      };
      const command = (label: string) => ({ label, enabled: true, reason: null, run: vi.fn() });
      return renderToStaticMarkup(<ProgramRuntimePanel enabled controller={program} recovery={{
        retry: command('Retry'), continue: null, reset: command('Reset'),
      }} />);
    };
    const failed = render('failed_validation');
    expect(failed).toContain('class="runtime-control-section program-recovery-failed"');
    expect(failed).toContain(
      '<p class="program-recovery-cause">RoboPlan could not resolve the pose goal (native_ik_no_solution)</p>',
    );
    const stopped = render('ordinary_operator_stop');
    expect(stopped).toContain('class="runtime-control-section" aria-labelledby="runtime-recovery-title"');
    expect(stopped).not.toContain('program-recovery-cause');
  });

  it('runs the Recovery block commands through the Motion and Program panel handlers', async () => {
    vi.spyOn(api.execution, 'admittedSpeeds').mockReturnValue(new Promise(() => undefined));
    const program = heldWithAlarms();
    const [left] = program.state.controls!.recoveryHolds;
    const open = await mountTransport(program);
    try {
      await open.press((props) => props['aria-label'] === 'Program run information');
      await open.press((props) => props.type === 'button' && props.children === 'Retry'
        && props.title === undefined);
      expect(program.applyRecoveryAction).toHaveBeenLastCalledWith(
        program.state.controls!.recoveryOffer, 'retry_motion_from_current_state',
      );
      // Retry closes Program run details so the execution can be followed.
      const detailsShown = () => open.mounted((props) => props.id === 'program-run-details-title');
      expect(detailsShown()).toBe(false);
      await open.press((props) => props['aria-label'] === 'Program run information');
      // Continue asks with the Motion panel's confirmation; only confirming closes the details.
      await open.press((props) => props.type === 'button' && props.children === 'Continue'
        && props.title === undefined && props.className === 'danger-button');
      expect(program.applyRecoveryAction).toHaveBeenCalledTimes(1);
      expect(detailsShown()).toBe(true);
      await open.press((props) => props.className === 'danger-button' && props.children === 'Continue');
      expect(program.applyRecoveryAction).toHaveBeenLastCalledWith(left.offer, 'continue_with_failure');
      expect(detailsShown()).toBe(false);
      await open.press((props) => props['aria-label'] === 'Program run information');
      // Reset opens the Program panel's Reset confirmation.
      expect(open.mounted((props) => props.id === 'program-reset-confirmation-title')).toBe(false);
      await open.press((props) => props.children === 'Reset' && props.className === 'transport-reset');
      expect(open.mounted((props) => props.id === 'program-reset-confirmation-title')).toBe(true);
    } finally {
      await open.unmount();
      vi.restoreAllMocks();
    }
  });
});

describe('Recovery class label', () => {
  const offer = (recoveryClass: string, code: string, reason: string | null = null) => ({
    recoveryClass,
    reason: {
      code,
      message: 'stopped',
      details: reason === null ? null : { reason },
      origin: null,
    },
  });

  it('names a lost-feedback runtime-monitor Stop by its reason, not as a collision', () => {
    // Live incident 2026-10-29: the class stays runtime_collision; only the label differs.
    expect(recoveryClassLabel(offer('runtime_collision', 'runtime_monitor_observation_lost')))
      .toBe('Robot position feedback lost');
    expect(recoveryClassLabel(
      offer('runtime_collision', 'operation_failed', 'runtime_monitor_observation_lost'),
    )).toBe('Robot position feedback lost');
  });

  it('keeps the class label for an actual collision and other classes', () => {
    expect(recoveryClassLabel(offer('runtime_collision', 'execution_aborted', 'runtime_collision_fault')))
      .toBe('Runtime collision');
    expect(recoveryClassLabel(offer('node_failure_pause', 'runtime_monitor_observation_lost')))
      .toBe('Node failure pause');
  });
});


describe('Active TCP Resume confirmation', () => {
  function setup() {
    const program = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    controls.runRevision = program.state.run!.runRevision;
    const group = controls.executionSelections.groups[0];
    group.state = 'running';
    group.heldNodeCount = 0;
    group.ordinaryHeldNodeCount = 0;
    program.state.controls = controls;
    const binding = program.state.applied!.programBinding;
    binding.target_bindings.push({ ...targetBinding('robot-a', 'arm'), robot_tcps: [] });
    const robot = robotController();
    const status = robot.currentStatus!;
    robot.tcpResumeConnectionLost = false; robot.tcpResumeFresh = true; robot.tcpResumeStatus = status;
    robot.tcpResumeCycle = { run: program.state.run, runtimeControls: controls } as unknown as
      NonNullable<RobotControlController['tcpResumeCycle']>;
    status.graphRevision = program.state.run!.capture.graphRevision;
    status.retainedBundleId = program.state.run!.capture.retainedBundleId;
    return { program, robot, group, status };
  }

  it('offers Cancel and Resume after a held robot TCP changes', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      const motion = status.directControlTargets[0];
      motion.activeTcp = { ...motion.activeTcp!, entry: {
        componentId: 'robot-a', targetId: 'arm', frameComponentId: 'robot-a', frameRoleId: 'flange',
      }, display: 'robot-a.flange', sequence: 2 };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      await transport.press((p) => p.children === 'Cancel');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      await transport.press((p) => p.className === 'transport-group-resume');
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });

  it('updates the dialog when a displayed TCP changes and another robot changes', async () => {
    const { program, robot, group, status } = setup();
    const second = structuredClone(status.directControlTargets[0]);
    second.controllableComponentId = 'robot-b';
    second.activeTcp!.entry!.componentId = 'robot-b';
    second.activeTcp!.display = 'robot-b.tcp';
    status.directControlTargets.push(second);
    program.state.applied!.programBinding.target_bindings.push({ ...targetBinding('robot-b', 'arm'), robot_tcps: [] });
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      const first = status.directControlTargets[0];
      first.activeTcp = { ...first.activeTcp!, entry: null, display: null, sequence: 1 };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      first.activeTcp = { ...first.activeTcp!, entry: {
        componentId: 'robot-a', targetId: 'arm', frameComponentId: 'robot-a', frameRoleId: 'flange',
      }, display: 'robot-a.flange', sequence: 2 };
      second.activeTcp = { ...second.activeTcp!, entry: null, display: null, sequence: 1 };
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      expect(transport.mounted((p) => Array.isArray(p.children)
        && p.children.every((child) => typeof child === 'string')
        && JSON.stringify(p.children) === JSON.stringify([
        'robot-a', ': Active TCP is now ', 'robot-a.flange', ' (was ', 'robot-a.tcp', ').',
      ]))).toBe(true);
      expect(transport.mounted((p) => Array.isArray(p.children)
        && p.children.every((child) => typeof child === 'string')
        && JSON.stringify(p.children) === JSON.stringify([
        'robot-b', ': Active TCP is now ', 'No TCP', ' (was ', 'robot-b.tcp', ').',
      ]))).toBe(true);
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('updates a multi-robot dialog when one current value becomes unavailable', async () => {
    const { program, robot, group, status } = setup();
    const second = structuredClone(status.directControlTargets[0]);
    second.controllableComponentId = 'robot-b';
    second.activeTcp!.entry!.componentId = 'robot-b';
    second.activeTcp!.display = 'robot-b.tcp';
    status.directControlTargets.push(second);
    program.state.applied!.programBinding.target_bindings.push({ ...targetBinding('robot-b', 'arm'), robot_tcps: [] });
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      const first = status.directControlTargets[0];
      first.activeTcp = { ...first.activeTcp!, entry: null, display: null, sequence: 1 };
      second.activeTcp = { ...second.activeTcp!, entry: null, display: null, sequence: 1 };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      second.activeTcp = null;
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      expect(transport.mounted((p) => Array.isArray(p.children)
        && p.children.every((child) => typeof child === 'string')
        && JSON.stringify(p.children) === JSON.stringify([
        'robot-a', ': Active TCP is now ', 'No TCP', ' (was ', 'robot-a.tcp', ').',
      ]))).toBe(true);
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('keeps the dialog and explains when Resume is no longer available', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 1,
      };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      group.resumeAllowed = false;
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      expect(transport.mounted((p) => p.children === 'Resume is no longer available.')).toBe(true);
      group.resumeAllowed = true;
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeMotion).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('does not mistake mixed Ready and running work for an ordinary hold', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'mixed'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 0;
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });

  it('retains a held TCP baseline while the held count is unknown', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      group.ordinaryHeldNodeCount = null;
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      expect(transport.mounted((p) => p.children === 'Cancel')).toBe(true);
    } finally { await transport.unmount(); }
  });

  it('does not infer a current hold from retained controls when the current cycle is unavailable', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      robot.tcpResumeCycle = null;
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });

  it('does not reuse a hold baseline after the connection is lost', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      robot.tcpResumeConnectionLost = true; robot.tcpResumeFresh = false;
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      robot.tcpResumeConnectionLost = false; robot.tcpResumeFresh = true;
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });

  it('discards the baseline if loss and reconnection occur between renders', async () => {
    const { program, robot, group, status } = setup();
    let transportStatus = 'connected';
    let onShellChange = () => {};
    robot.controlSource = {
      getShellSnapshot: () => ({ transportStatus }),
      subscribeShell: (listener: () => void) => { onShellChange = listener; return () => {}; },
    } as unknown as RobotControlController['controlSource'];
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      const { act } = await import('react');
      await act(() => {
        transportStatus = 'disconnected'; onShellChange();
        transportStatus = 'connected'; onShellChange();
      });
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });

  it('keeps the first hold baseline for a locally pending Run', async () => {
    const { program, robot, group, status } = setup();
    const run = program.state.run;
    program.state.run = null;
    program.state.pending = 'run';
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      program.state.run = run;
      program.state.pending = null;
      await transport.rerender();
      const motion = status.directControlTargets[0];
      motion.activeTcp = { ...motion.activeTcp!, entry: {
        componentId: 'robot-a', targetId: 'arm', frameComponentId: 'robot-a', frameRoleId: 'flange',
      }, display: 'robot-a.flange', sequence: 2 };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).not.toHaveBeenCalled();
      expect(transport.mounted((p) => p.children === 'Cancel')).toBe(true);
    } finally { await transport.unmount(); }
  });

  it('does not prompt when the page first observes an already-held run', async () => {
    const { program, robot, group, status } = setup();
    group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
    const transport = await mountTransport(program, robot);
    try {
      const motion = status.directControlTargets[0];
      motion.activeTcp = { ...motion.activeTcp!, entry: {
        componentId: 'robot-a', targetId: 'arm', frameComponentId: 'robot-a', frameRoleId: 'flange',
      }, display: 'robot-a.flange', sequence: 2 };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });
});

describe('Active TCP confirmation for halted Program and ready group', () => {
  function markHalted(program: ProgramRuntimeController) {
    program.state.run!.state = 'full_stopped';
    // The backend's readiness for a halted run.
    program.readiness = { can_execute: false, can_resume: true, motion_clear: true, blocked_reasons: [] };
    for (const group of program.state.controls!.executionSelections.groups) {
      group.state = 'unavailable';
      group.ordinaryHeldNodeCount = null;
    }
  }

  function setup() {
    const program = controller('running');
    const controls = ordinaryMotionStopControls();
    controls.recoveryOffer = null;
    controls.runRevision = program.state.run!.runRevision;
    controls.executionSelections.groups[0].state = 'running';
    controls.executionSelections.groups[0].heldNodeCount = 0;
    controls.executionSelections.groups[0].ordinaryHeldNodeCount = 0;
    program.state.controls = controls;
    program.state.applied!.programBinding.target_bindings.push({ ...targetBinding('robot-a', 'arm'), robot_tcps: [] });
    const robot = robotController();
    const status = robot.currentStatus!;
    robot.tcpResumeConnectionLost = false; robot.tcpResumeFresh = true; robot.tcpResumeStatus = status;
    robot.tcpResumeCycle = { run: program.state.run, runtimeControls: controls } as unknown as
      NonNullable<RobotControlController['tcpResumeCycle']>;
    status.graphRevision = program.state.run!.capture.graphRevision;
    status.retainedBundleId = program.state.run!.capture.retainedBundleId;
    return { program, robot, group: controls.executionSelections.groups[0], status };
  }

  it('confirms before resuming a halted Program', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      const motion = status.directControlTargets[0];
      motion.activeTcp = { ...motion.activeTcp!, entry: null, display: null, sequence: 2 };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      expect(program.resumeFullStop).not.toHaveBeenCalled();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('refreshes changed Full Stop TCP values before resuming', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      const motion = status.directControlTargets[0];
      motion.activeTcp = { ...motion.activeTcp!, entry: null, display: null, sequence: 1 };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      motion.activeTcp = { ...motion.activeTcp!, entry: {
        componentId: 'robot-a', targetId: 'arm', frameComponentId: 'robot-a', frameRoleId: 'flange',
      }, display: 'robot-a.flange', sequence: 2 };
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeFullStop).not.toHaveBeenCalled();
      expect(transport.mounted((p) => Array.isArray(p.children)
        && p.children.every((child) => typeof child === 'string')
        && JSON.stringify(p.children) === JSON.stringify([
        'robot-a', ': Active TCP is now ', 'robot-a.flange', ' (was ', 'robot-a.tcp', ').',
      ]))).toBe(true);
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('resumes normally if the TCP returns to its hold baseline while the dialog is open', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      const motion = status.directControlTargets[0];
      const initial = structuredClone(motion.activeTcp!);
      motion.activeTcp = { ...initial, entry: null, display: null, sequence: 1 };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      motion.activeTcp = { ...initial, sequence: 2 };
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('skips the Full Stop dialog when the current TCP becomes unavailable', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      robot.tcpResumeStatus = null;
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('skips the Full Stop dialog after an execution owner replacement', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      status.ownerEpoch = 'new-owner';
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null,
        ownerEpoch: 'new-owner', sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('skips the Full Stop dialog when the page first observes an already halted run', async () => {
    const { program, robot, status } = setup();
    markHalted(program);
    const transport = await mountTransport(program, robot);
    try {
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('resumes normally if the TCP becomes unavailable after the dialog opens', async () => {
    const { program, robot, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      markHalted(program);
      await transport.rerender();
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => typeof p.className === 'string' && p.className.includes('transport-run'));
      expect(program.resumeFullStop).not.toHaveBeenCalled();
      robot.tcpResumeStatus = null;
      await transport.rerender();
      await transport.press((p) => p.children === 'Resume' && p.className === 'primary-action');
      expect(program.resumeFullStop).toHaveBeenCalledOnce();
    } finally { await transport.unmount(); }
  });

  it('does not prompt when Run releases an unheld ready group', async () => {
    const { program, robot, group, status } = setup();
    const transport = await mountTransport(program, robot);
    try {
      group.state = 'held'; group.heldNodeCount = 1; group.ordinaryHeldNodeCount = 1;
      await transport.rerender();
      group.state = 'ready'; group.heldNodeCount = 0; group.ordinaryHeldNodeCount = 0;
      status.directControlTargets[0].activeTcp = {
        ...status.directControlTargets[0].activeTcp!, entry: null, display: null, sequence: 2,
      };
      await transport.rerender();
      await transport.press((p) => p.className === 'transport-group-resume');
      expect(program.resumeMotion).toHaveBeenCalledWith(['all']);
    } finally { await transport.unmount(); }
  });
});

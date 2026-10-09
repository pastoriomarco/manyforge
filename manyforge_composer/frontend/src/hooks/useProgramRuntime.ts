import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api, apiErrorCode, type PersistentKeyValuesFence } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import { failureContextText } from '../state/structuredError';
import type {
  AppliedProgramSelection,
  AppliedProgramStatus,
  CurrentScenarioResponse,
  JsonValue,
  CycleRecoveryActionId,
  PersistentKeyValues,
  ProgramRuntimeControlProjection,
  ProgramGroupRuntimeScaleState,
  ProgramCycleStatus,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import {
  type MotionAttentionBlock,
  motionAttentionBlock as evaluateMotionAttention,
} from '../state/operatorAlarmMotionAttention';
import {
  browserStableIdentity,
  CommandDeliveryUnconfirmedError,
  type ExecutionControlCommandKind,
  type HeadlessStateSource,
} from './useHeadlessState';
import { parseRuntimeScaleControlAccepted } from '../api/parsers';
import type {
  RuntimeScaleControlSelector,
  RuntimeScaleControlStatus,
} from '../types/robotControl';
import {
  acceptedRuntimeScaleStatus,
  mergeRuntimeScaleStatus,
  runtimeScaleControlScopeFence,
  runtimeScaleFlushDelay,
  runtimeScaleSelectorIdentity,
} from './useRobotControl';

const CURRENT_STATES = new Set([
  'running',
  'intervention_required',
  'full_stopping',
  'full_stopped',
  'stopping',
]);
type ProgramCycleDebugProjection = Pick<ProgramCycleStatus,
  | 'programRunnerOwnerEpoch'
  | 'cycleGeneration'
  | 'state'
  | 'programRunId'
  | 'cycleClaimGeneration'
> & {
  admissionGeneration?: number;
  authoringAllowed?: boolean;
};

export function programCurrentStatusSummary(
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleDebugProjection | null,
): string {
  const activeNodes = run === null
    ? []
    : Object.entries(run.nodeStatuses)
      .filter(([, status]) => status === 'running' || status === 'held' || status === 'failure')
      .slice(0, 6)
      .map(([node, status]) => `${node}=${status}`);
  const recovery = controls?.recoveryOffer ?? null;
  const runContext = run?.reason ? failureContextText(run.reason) : null;
  const recoveryContext = recovery === null ? null : failureContextText(recovery.reason);
  return [
    run === null
      ? 'Observed run: none.'
      : [
        `Observed run ${run.capture.programRunId}: ${run.state}, revision ${run.runRevision}, tree ${run.treeStatus}, active operations ${run.activeOperationCount ?? 'unknown'}.`,
        run.reason === null ? null : `Run reason: ${run.reason.code}.`,
        run.message ? `Run message: ${run.message}.` : null,
        runContext === null ? null : `Run failure details: ${runContext}.`,
        activeNodes.length === 0 ? null : `Current nodes: ${activeNodes.join(', ')}.`,
      ].filter((value): value is string => value !== null).join(' '),
    cycle === null
      ? 'Observed cycle: unavailable.'
      : `Observed cycle: ${cycle.state}, generation ${cycle.cycleGeneration}, run ${cycle.programRunId ?? 'none'}, claim ${cycle.cycleClaimGeneration ?? 'none'}, owner ${cycle.programRunnerOwnerEpoch}.`,
    recovery === null
      ? 'Observed recovery: none.'
      : [
        `Observed recovery ${recovery.recoveryClass}: ${recovery.status}, offer revision ${recovery.offerRevision}, node ${recovery.nodeInstanceId}, operation ${recovery.operationId}.`,
        `Recovery reason: ${recovery.reason.code}.`,
        recovery.reason.message ? `Recovery message: ${recovery.reason.message}.` : null,
        recoveryContext === null ? null : `Recovery failure details: ${recoveryContext}.`,
        `Allowed actions: ${recovery.actions.filter((action) => action.enabled).map((action) => action.actionId).join(', ') || 'none'}.`,
      ].filter((value): value is string => value !== null).join(' '),
  ].join(' ');
}

export function programTerminalFailureMessage(
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleDebugProjection | null,
): string | null {
  if (run?.state !== 'failed') return null;
  return `Program execution failed. ${programCurrentStatusSummary(run, controls, cycle)}`;
}

export function programRuntimeWakeKey(snapshot: ComposerHeadlessState | null): string {
  if (snapshot === null) return 'unavailable';
  // Catalog and Applied reads follow their own owner identities. Program-cycle
  // and authoring-admission revisions are already carried by the control
  // snapshot and must not refetch static authoring resources during motion.
  const manager = snapshot.runtime.sections.manager;
  const basis = snapshot.componentOverview?.basis;
  const managerActive = manager.payload?.active;
  const active = managerActive !== null
    && typeof managerActive === 'object'
    && !Array.isArray(managerActive)
    ? managerActive as Record<string, unknown>
    : null;
  return [
    snapshot.receiverEpoch,
    manager.sourceOwnerEpoch ?? 'no-manager-owner',
    typeof active?.retained_bundle_id === 'string'
      ? active.retained_bundle_id : 'no-retained-bundle',
    typeof active?.graph_revision === 'string'
      ? active.graph_revision : 'no-graph-revision',
    basis?.appliedProgramOwnerEpoch ?? 'no-applied-owner',
    basis?.appliedProgramGeneration ?? 'no-applied-generation',
  ].join(':');
}

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

export type ProgramPendingCommand =
  | 'run'
  | 'start-motion'
  | 'motion-stop'
  | 'resume-motion'
  | 'full-stop'
  | 'resume-full-stop'
  | 'stop'
  | 'reset'
  | null;

export interface ProgramSelectionPending {
  kind: 'start-motion' | 'motion-stop' | 'resume-motion';
  groupIds: string[];
  requestId: string;
  ownerEpoch: string;
  programRunId: string;
}

interface ProgramRuntimeState {
  applied: AppliedProgramSelection | null;
  /** The Applied Program is preserved but not executable in the active
   * Deployment (H1a); derived on every refresh, never stored as a flag. */
  nonExecutable: AppliedProgramStatus | null;
  currentScenario: CurrentScenarioResponse | null;
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
  runnerAvailable: boolean;
  loading: boolean;
  observationError: string | null;
  pending: ProgramPendingCommand;
  selectionPending: ProgramSelectionPending[];
  controlPending: string | null;
  error: string | null;
}

/** Commands that start or resume Program progression or motion. */
const DRAFT_GATED_COMMANDS: ReadonlySet<string> = new Set([
  'program_run', 'program_motion_start', 'program_motion_resume',
  'program_resume_full_stop',
]);

const INITIAL_STATE: ProgramRuntimeState = {
  applied: null,
  nonExecutable: null,
  currentScenario: null,
  run: null,
  controls: null,
  runnerAvailable: false,
  loading: true,
  observationError: null,
  pending: null,
  selectionPending: [],
  controlPending: null,
  error: null,
};

function currentScenarioKey(value: CurrentScenarioResponse | null): string {
  const scenario = value?.currentScenario ?? null;
  return value === null
    ? 'none'
    : `${value.ready}:${value.unavailableReasons.join(',')}:${scenario === null
      ? 'none'
      : [
        scenario.scenarioId,
        scenario.sourceDocumentDigest,
        scenario.deploymentRevision,
        scenario.retainedBundleId,
        scenario.graphRevision,
        scenario.programRevision,
        scenario.programBindingId,
      ].join(':')}`;
}

/** The exact Applied Program selection a view shows ('none' when absent). */
export function appliedKey(value: AppliedProgramSelection | null): string {
  return value === null
    ? 'none'
    : `${value.sourceOwnerEpoch}:${value.sourceGeneration}:${value.recordDigest}`;
}

export function programRunProjectionKey(value: ProgramRunStatus | null): string {
  return value === null
    ? 'none'
    : `${value.ownerEpoch}:${value.capture.programRunId}:`
      + `${value.runRevision}:${value.nodeStatusRevision}`;
}

function controlsKey(value: ProgramRuntimeControlProjection | null): string {
  return value === null
    ? 'none'
    : `${value.ownerEpoch}:${value.programRunId}:${value.runRevision}:`
      + `${value.runtimeControls.runtimeControlRevision}:`
      + `${value.recoveryOffer?.offerId ?? 'no-offer'}:`
      + `${value.recoveryOffer?.offerRevision ?? 0}:`
      + JSON.stringify(value.executionSelections);
}

export function controlPendingAfterProjection(
  pending: string | null,
  previous: ProgramRuntimeControlProjection | null,
  observed: ProgramRuntimeControlProjection | null,
  run: ProgramRunStatus | null,
): string | null {
  if (pending === null || !pending.startsWith('recovery:')) return pending;
  if (run !== null && ['stopped', 'succeeded', 'failed'].includes(run.state)) {
    return null;
  }
  const expected = previous?.recoveryOffer ?? null;
  const current = observed?.recoveryOffer ?? null;
  if (expected === null) return pending;
  return current === null
    || current.offerId !== expected.offerId
    || current.offerRevision !== expected.offerRevision
    ? null
    : pending;
}

/** Whole-selection presentation comes from the owner, not one focused task. */
export function programMotionStopSettled(
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
): boolean {
  if (
    run === null
    || controls === null
    || !['running', 'intervention_required'].includes(run.state)
    || controls.ownerEpoch !== run.ownerEpoch
    || controls.programRunId !== run.capture.programRunId
  ) return false;
  return controls.executionSelections.groups.some(
    (group) => group.groupId === 'all' && group.state === 'held',
  );
}

/** Cycle controls mirror owner transitions; selected commands track delivery only. */
export function programPendingAfterProjection(
  pending: ProgramPendingCommand,
  run: ProgramRunStatus | null,
  _controls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleDebugProjection | null = null,
  previousRun: ProgramRunStatus | null = run,
): ProgramPendingCommand {
  if (pending === 'start-motion' || pending === 'motion-stop' || pending === 'resume-motion') {
    // Delivery feedback belongs to its captured run, not a successor/idle
    // projection. Its late reply cannot clear a newer request's exact token.
    return run === null
      || run.ownerEpoch !== previousRun?.ownerEpoch
      || run.capture.programRunId !== previousRun?.capture.programRunId
      || !['running', 'intervention_required'].includes(run.state)
      ? null : pending;
  }
  if (pending === 'run') {
    if (run !== null
      && run.state === 'running'
      && cycle?.state === 'running'
      && cycle.programRunId === run.capture.programRunId) return null;
    return cycle !== null
      && ['stopping', 'resetting', 'terminal_awaiting_reset', 'recovery_required']
        .includes(cycle.state)
      ? null : pending;
  }
  if (pending === 'reset') {
    return cycle?.state === 'idle_after_reset' ? null : pending;
  }
  if (pending === 'stop') {
    const settled = (
      run !== null
      && ['stopped', 'succeeded', 'failed'].includes(run.state)
    ) || cycle?.state === 'terminal_awaiting_reset'
      || cycle?.state === 'idle_after_reset';
    return settled ? null : pending;
  }
  if (pending === 'full-stop') {
    return run?.state === 'full_stopped' ? null : pending;
  }
  if (pending === 'resume-full-stop') {
    return run?.state === 'running' && cycle?.state === 'running'
      ? null
      : pending;
  }
  return pending;
}

export function buildProgramStartRequest(
  applied: AppliedProgramSelection | null,
  runnerAvailable: boolean,
  pending: ProgramRuntimeState['pending'],
  snapshot: ComposerHeadlessState | null,
  workspace: ProgramWorkspaceResponse | null,
  requestOwnerEpoch: string,
  requestId: string,
  /** The Deployment Draft is not the applied one: Execute waits, as for the Program. */
  deploymentPending = false,
): Record<string, JsonValue> | null {
  if (
    applied === null || !runnerAvailable || pending !== null || deploymentPending
    || snapshot === null || !snapshot.authoringAdmission.authoringAllowed
    || snapshot.programCycle.state !== 'idle_after_reset'
    || workspace === null
    || workspace.draft.draftId !== snapshot.authoringWorkspace.programDraftId
    || workspace.draft.draftRevision
      !== snapshot.authoringWorkspace.programDraftRevision
    || !workspace.draft.programLoaded
    || workspace.draft.programRevision !== applied.programRevision
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: requestId,
    scenario_id: applied.scenarioId,
    source_document_digest: applied.sourceDocumentDigest,
    expected_applied_record_digest: applied.recordDigest,
    draft_program_revision: workspace.draft.programRevision,
    expected_admission_generation: snapshot.authoringAdmission.admissionGeneration,
  };
}

/** The exact Reset this browser sent: the cycle it ends and its correlation. */
export interface ProgramResetIdentity {
  requestId: string;
  correlationId: string;
  programRunId: string;
  programRunnerOwnerEpoch: string;
  cycleGeneration: number;
}

/** The exact cycle a caller confirmed Reset for; any other is refused unsent. */
export type ProgramResetExpectation = Pick<
  ProgramResetIdentity, 'programRunId' | 'programRunnerOwnerEpoch' | 'cycleGeneration'
>;

/** What one Reset call established; never settlement on its own. */
export type ProgramResetOutcome =
  /** Received by the owner (its receipt was not a refusal). */
  | { status: 'sent'; reset: ProgramResetIdentity }
  /** Submitted, but its reply was lost: it may or may not have been received. */
  | { status: 'unconfirmed'; reset: ProgramResetIdentity; message: string }
  /** Definitely without effect: not sent, or refused by the owner. */
  | { status: 'refused'; message: string };

export function buildProgramResetRequest(
  snapshot: ComposerHeadlessState | null,
  requestOwnerEpoch: string,
  requestId: string,
  correlationId: string = requestId,
): Record<string, JsonValue> | null {
  const cycle = snapshot?.programCycle;
  const allowed = cycle?.allowedCommands.some(
    (command) => command.commandKind === 'program_reset' && command.allowed,
  ) ?? false;
  if (
    cycle === undefined
    || !allowed
    || cycle.programRunId === null
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: correlationId,
    program_run_id: cycle.programRunId,
    expected_owner_epoch: cycle.programRunnerOwnerEpoch,
    expected_cycle_generation: cycle.cycleGeneration,
    expected_claim_generation: cycle.cycleClaimGeneration,
    terminal_reference: cycle.terminalOrSettlementReference,
    settlement_evidence_digest: cycle.terminalOrSettlementReference,
  };
}

function controlsMatchRun(
  controls: ProgramRuntimeControlProjection | null,
  run: ProgramRunStatus,
): controls is ProgramRuntimeControlProjection {
  return controls !== null
    && controls.ownerEpoch === run.ownerEpoch
    && controls.programRunId === run.capture.programRunId
    && controls.runRevision === run.runRevision;
}

function runtimeScaleSelectorWire(
  selector: RuntimeScaleControlSelector,
): Record<string, JsonValue> {
  return selector.kind === 'singular'
    ? {
      program_binding_id: selector.programBindingId,
      component_role_id: selector.componentRoleId,
      target_role_id: selector.targetRoleId,
    }
    : {
      program_binding_id: selector.programBindingId,
      group_targets: selector.groupTargets.map((target) => ({
        component_role_id: target.componentRoleId,
        target_role_id: target.targetRoleId,
      })),
    };
}

function selectionPendingAfterProjection(
  pending: ProgramSelectionPending[],
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
): ProgramSelectionPending[] {
  if (pending.length === 0) return [];
  if (run === null || !['running', 'intervention_required'].includes(run.state)) {
    return [];
  }
  const currentRunPending = pending.filter((request) => (
    request.ownerEpoch === run.ownerEpoch
    && request.programRunId === run.capture.programRunId
  ));
  // Keep delivery feedback while a matching run's control projection is
  // temporarily unavailable. Clearing it here would make a lost observation
  // look like a completed command and could expose a second command too soon.
  if (controls === null || !controlsMatchRun(controls, run)) return currentRunPending;
  return currentRunPending.filter((request) => {
    const rows = controls.executionSelections.groups.filter((group) => (
      request.groupIds.includes(group.groupId)
    ));
    if (rows.length !== request.groupIds.length) return true;
    const complete = request.kind === 'motion-stop'
      ? rows.every((group) => group.state === 'stopping' || group.state === 'held')
      : rows.every((group) => group.state === 'running' && group.heldNodeCount === 0);
    return !complete;
  });
}

function sameSelectionPending(
  left: ProgramSelectionPending[],
  right: ProgramSelectionPending[],
): boolean {
  return left.length === right.length && left.every((request, index) => {
    const other = right[index];
    return request.kind === other.kind
      && request.requestId === other.requestId
      && request.ownerEpoch === other.ownerEpoch
      && request.programRunId === other.programRunId
      && request.groupIds.length === other.groupIds.length
      && request.groupIds.every((id, groupIndex) => id === other.groupIds[groupIndex]);
  });
}

export function programRuntimeAfterSnapshot(
  currentRun: ProgramRunStatus | null,
  currentControls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleStatus,
): {
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
} {
  if (cycle.programRunId === null) return { run: null, controls: null };
  const observedRun = cycle.run;
  const currentMatchesCycle = currentRun !== null
    && currentRun.ownerEpoch === cycle.programRunnerOwnerEpoch
    && currentRun.capture.programRunId === cycle.programRunId;
  const run = currentMatchesCycle && (
    observedRun === null
    || currentRun.runRevision > observedRun.runRevision
    // A control-only publication must not rebuild the unchanged tree data.
    // Node-status revision remains part of the existing authoritative key.
    || programRunProjectionKey(currentRun) === programRunProjectionKey(observedRun)
  )
    ? currentRun
    : observedRun;
  if (run === null) return { run: null, controls: null };

  const observedControls = controlsMatchRun(cycle.runtimeControls, run)
    ? cycle.runtimeControls
    : null;
  const retainedControls = controlsMatchRun(currentControls, run)
    ? currentControls
    : null;
  const controls = retainedControls !== null && (
    observedControls === null
    || retainedControls.runtimeControls.runtimeControlRevision
      > observedControls.runtimeControls.runtimeControlRevision
  )
    ? retainedControls
    : observedControls;
  return { run, controls };
}

/**
 * What one stored-value edit names as seen: the live run holding the values
 * at its current revision; without a run nothing (the last admitted edit
 * wins). Null when the run shown here is not the run holding the values.
 */
export function persistentValuesFence(
  current: PersistentKeyValues,
  run: { capture: { programRunId: string }; runRevision: number } | null,
): PersistentKeyValuesFence | null {
  if (current.programRunId === null) return {};
  if (run === null || run.capture.programRunId !== current.programRunId) return null;
  return { program_run_id: current.programRunId, expected_run_revision: run.runRevision };
}

export function useProgramRuntime(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null = null,
  workspace: ProgramWorkspaceResponse | null = null,
  controlSource: HeadlessStateSource | null = null,
  deploymentPending = false,
) {
  const [state, setState] = useState<ProgramRuntimeState>(INITIAL_STATE);
  const mounted = useRef(true);
  const pendingProgramRequest = useRef<string | null>(null);
  const refreshing = useRef(false);
  const refreshQueued = useRef(false);
  const observationRetryAttempt = useRef(0);
  const [observationRetrySequence, setObservationRetrySequence] = useState(0);
  const [groupRuntimeScales, setGroupRuntimeScales] = useState<
    Record<string, ProgramGroupRuntimeScaleState>
  >({});
  const groupScaleStatusRef = useRef(new Map<string, RuntimeScaleControlStatus>());
  const groupScaleScopeFenceRef = useRef(new Map<string, string>());
  const groupScaleQueue = useRef(new Map<string, number>());
  const groupScaleInFlight = useRef<{
    groupId: string;
    scopeFence: string;
  } | null>(null);
  const groupScaleTimer = useRef<number | null>(null);
  const groupScaleLastSentAt = useRef(Number.NEGATIVE_INFINITY);
  const groupScaleFlush = useRef<() => void>(() => undefined);
  const groupScaleGeneration = useRef(0);
  const [groupScaleRefreshSequence, setGroupScaleRefreshSequence] = useState(0);
  const groupScaleRowsRef = useRef<ProgramRuntimeControlProjection['executionSelections']['groups']>([]);
  const groupScaleStopActiveRef = useRef<Set<string>>(new Set());
  const groupScaleIdentityRef = useRef('');
  const requestedWakeKey = useRef<string | null>(null);
  const callerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.program_request_owner_epoch',
    'composer-program-owner',
  ));
  const wakeKey = programRuntimeWakeKey(snapshot);
  const snapshotAvailable = snapshot !== null;
  const cycleDebugProjection = useMemo<ProgramCycleDebugProjection | null>(
    () => snapshot === null ? null : {
      programRunnerOwnerEpoch: snapshot.programCycle.programRunnerOwnerEpoch,
      cycleGeneration: snapshot.programCycle.cycleGeneration,
      state: snapshot.programCycle.state,
      programRunId: snapshot.programCycle.programRunId,
      cycleClaimGeneration: snapshot.programCycle.cycleClaimGeneration,
      admissionGeneration: snapshot.authoringAdmission.admissionGeneration,
      authoringAllowed: snapshot.authoringAdmission.authoringAllowed,
    },
    [snapshot],
  );
  const refresh = useCallback(async () => {
    if (!enabled || !snapshotAvailable) return;
    if (refreshing.current) {
      refreshQueued.current = true;
      return;
    }
    refreshing.current = true;
    try {
      const [currentScenarioResult, appliedResult] = await Promise.allSettled([
        api.programs.currentScenario(),
        api.programs.applied(),
      ]);
      if (!mounted.current) return;

      const errors: string[] = [];
      const currentScenario = currentScenarioResult.status === 'fulfilled'
        ? currentScenarioResult.value
        : null;
      if (currentScenarioResult.status === 'rejected') {
        errors.push(errorMessage(
          currentScenarioResult.reason,
          'Current Scenario projection unavailable.',
        ));
      }

      let applied: AppliedProgramSelection | null = null;
      let nonExecutable: AppliedProgramStatus | null = null;
      if (appliedResult.status === 'fulfilled') {
        applied = appliedResult.value;
      } else if (['program_applied_non_executable', 'program_not_selected'].includes(
        apiErrorCode(appliedResult.reason) ?? '',
      )) {
        // Not an observation fault: read the named outcome for the operator.
        try {
          const status = await api.programs.appliedStatus();
          nonExecutable = status.executable ? null : status;
        } catch (reason: unknown) {
          errors.push(errorMessage(reason, 'Applied Program status unavailable.'));
        }
      } else if (!(appliedResult.reason instanceof ApiError
        && appliedResult.reason.status === 404)) {
        errors.push(errorMessage(
          appliedResult.reason,
          'Applied Program projection unavailable.',
        ));
      }

      if (!mounted.current) return;
      const observationError = errors.length > 0
        ? [...new Set(errors)].join(' ')
        : null;
      if (observationError === null) {
        observationRetryAttempt.current = 0;
      }

      setState((current) => {
        const next = {
          ...current,
          currentScenario,
          applied,
          nonExecutable,
          runnerAvailable: true,
          loading: false,
          observationError,
          // Automatic owner refresh must not erase a command rejection before
          // the operator can see it. The next explicit command clears it.
          error: current.error,
        };
        return currentScenarioKey(current.currentScenario)
            === currentScenarioKey(next.currentScenario)
          && appliedKey(current.applied) === appliedKey(next.applied)
          && JSON.stringify(current.nonExecutable) === JSON.stringify(next.nonExecutable)
          && programRunProjectionKey(current.run) === programRunProjectionKey(next.run)
          && controlsKey(current.controls) === controlsKey(next.controls)
          && current.runnerAvailable === next.runnerAvailable
          && current.loading === next.loading
          && current.observationError === next.observationError
          && current.error === next.error
          ? current
          : next;
      });
      if (observationError !== null) {
        setObservationRetrySequence((current) => current + 1);
      }
    } finally {
      refreshing.current = false;
      if (refreshQueued.current) {
        refreshQueued.current = false;
        queueMicrotask(() => { void refresh(); });
      }
    }
  }, [enabled, snapshotAvailable]);

  useEffect(() => {
    if (!enabled || snapshot === null) return;
    const cycle = snapshot.programCycle;
    setState((current) => {
      const projected = programRuntimeAfterSnapshot(
        current.run,
        current.controls,
        cycle,
      );
      const pending = programPendingAfterProjection(
        current.pending,
        projected.run,
        projected.controls,
        cycleDebugProjection,
        current.run,
      );
      const controlPending = controlPendingAfterProjection(
        current.controlPending,
        current.controls,
        projected.controls,
        projected.run,
      );
      const selectionPending = selectionPendingAfterProjection(
        current.selectionPending,
        projected.run,
        projected.controls,
      );
      const error = current.error ?? (
        cycle.state === 'idle_after_reset'
          ? null
          : programTerminalFailureMessage(
            projected.run,
            projected.controls,
            cycleDebugProjection,
          )
      );
      if (
        programRunProjectionKey(current.run) === programRunProjectionKey(projected.run)
        && controlsKey(current.controls) === controlsKey(projected.controls)
        && current.runnerAvailable
        && current.pending === pending
        && sameSelectionPending(current.selectionPending, selectionPending)
        && current.controlPending === controlPending
        && current.error === error
      ) return current;
      return {
        ...current,
        run: projected.run,
        controls: projected.controls,
        runnerAvailable: true,
        pending,
        selectionPending,
        controlPending,
        error,
      };
    });
  }, [
    cycleDebugProjection,
    enabled,
    snapshot,
  ]);

  useEffect(() => {
    if (
      !enabled
      || !snapshotAvailable
      || state.observationError === null
    ) return undefined;
    const attempt = Math.min(observationRetryAttempt.current, 4);
    observationRetryAttempt.current = Math.min(attempt + 1, 4);
    const timer = window.setTimeout(
      () => { void refresh(); },
      Math.min(500 * 2 ** attempt, 8_000),
    );
    return () => window.clearTimeout(timer);
  }, [
    enabled,
    observationRetrySequence,
    refresh,
    snapshotAvailable,
    state.observationError,
  ]);

  useEffect(() => {
    mounted.current = true;
    return () => {
      mounted.current = false;
      // Observation loss is deliberately effect-free. In particular, client
      // unmount never submits Program Stop.
    };
  }, []);

  useEffect(() => {
    if (!enabled) {
      setState(INITIAL_STATE);
      requestedWakeKey.current = null;
      observationRetryAttempt.current = 0;
      return;
    }
    if (!snapshotAvailable) {
      requestedWakeKey.current = null;
      return;
    }
    if (requestedWakeKey.current === wakeKey) return;
    requestedWakeKey.current = wakeKey;
    void refresh();
  }, [enabled, refresh, snapshotAvailable, wakeKey]);

  useEffect(() => {
    if (!enabled || !snapshotAvailable) return undefined;
    const onVisibility = () => {
      if (document.visibilityState === 'visible') void refresh();
    };
    document.addEventListener('visibilitychange', onVisibility);
    return () => document.removeEventListener('visibilitychange', onVisibility);
  }, [enabled, refresh, snapshotAvailable]);

  // Runtime scaling belongs to an exact captured run. Before Execute there is
  // deliberately no owner target to query or mutate.
  const groupScaleRows = useMemo(
    () => state.controls?.executionSelections.groups ?? [],
    [state.controls],
  );
  groupScaleRowsRef.current = groupScaleRows;
  const groupScaleOwnerEpoch = snapshot?.executionControlStatus?.ownerEpoch
    ?? state.run?.ownerEpoch
    ?? '';
  const groupScaleIdentityKey = JSON.stringify([
    groupScaleOwnerEpoch,
    ...groupScaleRows.map((group) => [
      group.groupId,
      group.runtimeScaleSelector,
      group.runtimeScaleUnavailableReason,
    ]),
  ]);
  const groupScaleObservationKey = JSON.stringify(groupScaleRows.map((group) => [
    group.groupId,
    group.state,
    group.heldNodeCount,
  ]));
  const groupScaleStopActive = useMemo(() => {
    const allProgramStop = ['stopping', 'full_stopping'].includes(state.run?.state ?? '');
    return new Set(groupScaleRows
      .filter((group) => (
        allProgramStop
        || group.state === 'stopping'
        || (group.groupId === 'all' && state.selectionPending.some(
          (request) => request.kind === 'motion-stop',
        ))
        || state.selectionPending.some(
          (request) => request.kind === 'motion-stop'
            && (request.groupIds.includes(group.groupId)
              || request.groupIds.includes('all')),
        )
      ))
      .map((group) => group.groupId));
  }, [groupScaleRows, state.run?.state, state.selectionPending]);
  groupScaleStopActiveRef.current = groupScaleStopActive;

  const dismissError = useCallback(() => {
    setState((current) => current.error === null
      ? current
      : { ...current, error: null });
  }, []);

  const beginProgramCommand = useCallback((
    pending: Exclude<ProgramPendingCommand, null>,
    requestId: string,
    selectionGroups: string[] = [],
  ) => {
    pendingProgramRequest.current = requestId;
    setState((current) => ({
      ...current,
      pending,
      selectionPending: pending === 'start-motion'
        || pending === 'motion-stop' || pending === 'resume-motion'
        ? [
          ...current.selectionPending,
          {
            kind: pending,
            groupIds: [...selectionGroups],
            requestId,
            ownerEpoch: current.run?.ownerEpoch ?? '',
            programRunId: current.run?.capture.programRunId ?? '',
          },
        ]
        : current.selectionPending,
      error: null,
    }));
  }, []);

  const beginControlCommand = useCallback((pending: string) => {
    setState((current) => ({ ...current, controlPending: pending, error: null }));
  }, []);

  const recordCommandFailure = useCallback(async (
    reason: unknown,
    fallback: string,
    requestId: string,
    pendingKind: 'program' | 'control',
    expectedPending: string,
    expectedRun?: ProgramRunStatus,
  ) => {
    if (!mounted.current) return;
    setState((current) => {
      const selectionRequest = expectedPending === 'start-motion'
        || expectedPending === 'motion-stop'
        || expectedPending === 'resume-motion';
      if (
        (pendingKind === 'program' && (
          selectionRequest
            ? !current.selectionPending.some((entry) => entry.requestId === requestId)
            : current.pending !== expectedPending
              || pendingProgramRequest.current !== requestId
        ))
        || (pendingKind === 'control'
          && current.controlPending !== expectedPending)
        || (expectedRun !== undefined && (
          current.run?.ownerEpoch !== expectedRun.ownerEpoch
          || current.run?.capture.programRunId !== expectedRun.capture.programRunId
        ))
      ) return current;
      return {
        ...current,
        pending: pendingKind === 'program'
          && (!selectionRequest || pendingProgramRequest.current === requestId)
          ? null : current.pending,
        selectionPending: pendingKind === 'program'
          ? current.selectionPending.filter((entry) => entry.requestId !== requestId)
          : current.selectionPending,
        controlPending: pendingKind === 'control' ? null : current.controlPending,
        error: [
          errorMessage(reason, fallback),
          `Request ${requestId}. Backend state remains authoritative.`,
        ].join(' '),
      };
    });
  }, []);

  // Execute, Run, Resume and Retry use only what is applied: while a Draft
  // has unapplied changes they are refused (the Composer backend enforces the
  // same rule where it admits them). Stop, Halt and Reset stay available.
  const programPending = state.applied !== null && workspace !== null
    && workspace.draft.programLoaded
    && workspace.draft.programRevision !== state.applied.programRevision;
  const pendingDocuments = [
    ...(programPending ? ['Program (Scene edits included)'] : []),
    ...(deploymentPending ? ['Deployment'] : []),
  ];
  const draftUnappliedReason = pendingDocuments.length === 0
    ? null
    : `Apply or Discard first: unapplied changes in ${pendingDocuments.join(' and ')}. Execute, Run, Resume and Retry use only what is applied.`;

  const run = useCallback(async () => {
    const requestId = identity('program-run-start');
    const request = buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, requestId, deploymentPending,
    );
    if (request === null) return;
    if (controlSource === null) return;
    beginProgramCommand('run', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_run', request, requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Run was rejected.', requestId, 'program', 'run',
      );
    }
  }, [
    beginProgramCommand, controlSource, deploymentPending,
    recordCommandFailure, snapshot,
    state.applied, state.pending, state.runnerAvailable, workspace,
  ]);

  /**
   * Send one Reset for the cycle the caller names and report what is known.
   *
   * `expected` is the exact run, runner owner and cycle generation the caller
   * confirmed: when the current observation differs, nothing is sent. A
   * definite refusal (not sent, or refused by the owner) is distinct from a
   * submitted Reset whose reply was lost: that one may have been received, so
   * it keeps its request identity for reconciliation and must never be
   * resent. Neither outcome is settlement: callers that continue after Reset
   * observe its correlated cycle Idle.
   */
  const reset = useCallback(async (
    options: { correlationId?: string; expected?: ProgramResetExpectation } = {},
  ): Promise<ProgramResetOutcome> => {
    const requestId = identity('program-cycle-reset');
    const correlationId = options.correlationId ?? requestId;
    const cycle = snapshot?.programCycle ?? null;
    const { expected } = options;
    if (expected !== undefined && (
      cycle === null
      || cycle.programRunId !== expected.programRunId
      || cycle.programRunnerOwnerEpoch !== expected.programRunnerOwnerEpoch
      || cycle.cycleGeneration !== expected.cycleGeneration
    )) {
      return {
        status: 'refused',
        message: `Program Reset was not sent: the observed cycle is no longer run ${expected.programRunId} at cycle generation ${expected.cycleGeneration}.`,
      };
    }
    const request = buildProgramResetRequest(
      snapshot, callerEpoch.current, requestId, correlationId,
    );
    if (request === null || controlSource === null || cycle?.programRunId == null) {
      const message = request === null
        ? 'Program Reset was not sent because its backend permission or cycle identity changed. Controls were refreshed.'
        : 'Program Reset was not sent because the execution control connection is unavailable. Controls were refreshed.';
      if (mounted.current) setState((current) => ({ ...current, error: message }));
      await refresh();
      return { status: 'refused', message };
    }
    const sent: ProgramResetIdentity = {
      requestId,
      correlationId,
      programRunId: cycle.programRunId,
      programRunnerOwnerEpoch: cycle.programRunnerOwnerEpoch,
      cycleGeneration: cycle.cycleGeneration,
    };
    beginProgramCommand('reset', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_reset', request, correlationId,
      );
      return { status: 'sent', reset: sent };
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Reset was rejected.', requestId, 'program', 'reset',
      );
      return reason instanceof CommandDeliveryUnconfirmedError
        ? { status: 'unconfirmed', reset: sent, message: reason.message }
        : { status: 'refused', message: errorMessage(reason, 'Program Reset was rejected.') };
    }
  }, [
    beginProgramCommand, controlSource,
    recordCommandFailure, refresh, snapshot,
  ]);

  const selectionCommand = useCallback(async (
    pending: 'start-motion' | 'motion-stop' | 'resume-motion', groupIds: string[],
  ): Promise<boolean> => {
    const start = pending === 'start-motion';
    const resume = pending === 'resume-motion';
    const controls = state.controls;
    if (
      ((start || resume) && draftUnappliedReason !== null)
      || groupIds.length === 0
      || !state.run
      || !['running', 'intervention_required'].includes(state.run.state)
      || !state.runnerAvailable
      || ['stop', 'full-stop', 'reset']
        .includes(state.pending ?? '')
      || ((start || resume) && (state.selectionPending.length > 0 || state.controlPending !== null
        || !controlsMatchRun(controls, state.run)
        || !groupIds.every((id) => controls.executionSelections.groups.some(
          (group) => group.groupId === id
            && (start ? group.startAllowed : group.resumeAllowed),
        ))))
    ) return false;
    const requestId = identity(`program-${pending}`);
    if (controlSource === null) return false;
    beginProgramCommand(pending, requestId, groupIds);
    try {
      await controlSource.sendExecutionCommand(
        start
          ? 'program_motion_start'
          : resume ? 'program_motion_resume' : 'program_motion_stop',
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          group_ids: groupIds,
          ...((start || resume) && controls !== null ? {
            expected_run_revision: controls.runRevision,
            expected_selection_revision: controls.executionSelections.selectionRevision,
          } : {}),
          reason: `operator ${pending}`,
        },
        requestId,
      );
      return true;
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Selection command was rejected.', requestId, 'program', pending, state.run,
      );
      return false;
    } finally {
      // This clears only Sending feedback, never a hold or lifecycle state.
      // A newer priority Stop must not be cleared by an older Resume reply.
      if (mounted.current) setState((current) => ({
        ...current,
        pending: current.pending === pending
          && pendingProgramRequest.current === requestId
          ? null : current.pending,
        // Keep accepted scope feedback until the owner projection proves the
        // requested state. A receipt only proves admission; removing it here
        // would briefly expose a duplicate command while Stop/Resume settles.
        selectionPending: current.selectionPending,
      }));
    }
  }, [
    beginProgramCommand, controlSource, draftUnappliedReason,
    recordCommandFailure,
    state.pending, state.selectionPending, state.controlPending, state.controls,
    state.run, state.runnerAvailable,
  ]);

  const stopMotion = useCallback((groups: string | string[] = 'all') => selectionCommand('motion-stop', typeof groups === 'string' ? [groups] : groups), [selectionCommand]);
  const startMotion = useCallback((groups: string | string[] = 'all') => selectionCommand('start-motion', typeof groups === 'string' ? [groups] : groups), [selectionCommand]);
  const resumeMotion = useCallback((groups: string | string[] = 'all') => selectionCommand('resume-motion', typeof groups === 'string' ? [groups] : groups), [selectionCommand]);

  const clearGroupScaleQueue = useCallback(() => {
    groupScaleQueue.current.clear();
    if (groupScaleTimer.current !== null) {
      window.clearTimeout(groupScaleTimer.current);
      groupScaleTimer.current = null;
    }
    setGroupRuntimeScales((current) => {
      const next: Record<string, ProgramGroupRuntimeScaleState> = {};
      for (const [groupId, value] of Object.entries(current)) {
        next[groupId] = value.pending ? { ...value, pending: false } : value;
      }
      return next;
    });
  }, []);

  const scheduleGroupScaleFlush = useCallback(() => {
    if (groupScaleTimer.current !== null) return;
    const delay = runtimeScaleFlushDelay(
      performance.now(),
      groupScaleLastSentAt.current,
      groupScaleQueue.current.size > 0,
      groupScaleInFlight.current !== null,
    );
    if (delay === null) return;
    if (delay === 0) {
      groupScaleFlush.current();
      return;
    }
    groupScaleTimer.current = window.setTimeout(() => {
      groupScaleTimer.current = null;
      groupScaleFlush.current();
    }, delay);
  }, []);

  const flushGroupScale = useCallback(async () => {
    if (
      groupScaleInFlight.current !== null
      || groupScaleQueue.current.size === 0
      || controlSource === null
    ) return;
    const row = groupScaleRowsRef.current.find((candidate) => (
      groupScaleQueue.current.has(candidate.groupId)
      && !groupScaleStopActiveRef.current.has(candidate.groupId)
    ));
    if (row === undefined) return;
    if (row.runtimeScaleSelector === null) {
      groupScaleQueue.current.delete(row.groupId);
      scheduleGroupScaleFlush();
      return;
    }
    const status = groupScaleStatusRef.current.get(row.groupId);
    const scale = groupScaleQueue.current.get(row.groupId);
    if (
      status === undefined
      || !status.supported
      || status.frozen
      || scale === undefined
      || !Number.isFinite(scale)
    ) {
      groupScaleQueue.current.delete(row.groupId);
      setGroupRuntimeScales((current) => ({
        ...current,
        [row.groupId]: current[row.groupId] === undefined
          ? {
            selector: row.runtimeScaleSelector,
            status: status ?? null,
            draft: status?.requestedScale ?? 1,
            pending: false,
            error: row.runtimeScaleUnavailableReason,
          }
          : { ...current[row.groupId], pending: false },
      }));
      scheduleGroupScaleFlush();
      return;
    }
    groupScaleQueue.current.delete(row.groupId);
    const scopeFence = runtimeScaleControlScopeFence(groupScaleOwnerEpoch, status);
    const inFlight = { groupId: row.groupId, scopeFence };
    groupScaleInFlight.current = inFlight;
    groupScaleLastSentAt.current = performance.now();
    setGroupRuntimeScales((current) => ({
      ...current,
      [row.groupId]: current[row.groupId] === undefined
        ? {
          selector: row.runtimeScaleSelector,
          status,
          draft: scale,
          pending: true,
          error: null,
        }
        : { ...current[row.groupId], pending: true, error: null },
    }));
    try {
      const commandRequestId = identity(`program-group-scale.${row.groupId}`);
      const receipt = await controlSource.sendExecutionCommand(
        'runtime_scale',
        {
          selector: runtimeScaleSelectorWire(status.selector),
          expected_control_generation: status.controlGeneration,
          scale,
        },
        commandRequestId,
      );
      if (
        groupScaleInFlight.current !== inFlight
        || groupScaleStatusRef.current.get(row.groupId) !== status
        || groupScaleScopeFenceRef.current.get(row.groupId) !== scopeFence
        || groupScaleStopActiveRef.current.has(row.groupId)
      ) return;
      const accepted = parseRuntimeScaleControlAccepted(receipt.result);
      const nextStatus = acceptedRuntimeScaleStatus(
        status,
        accepted,
        commandRequestId,
        scale,
      );
      groupScaleStatusRef.current.set(row.groupId, nextStatus);
      groupScaleScopeFenceRef.current.set(row.groupId, scopeFence);
      setGroupRuntimeScales((current) => ({
        ...current,
        [row.groupId]: current[row.groupId] === undefined
          ? {
            selector: row.runtimeScaleSelector,
            status: nextStatus,
            draft: accepted.requestedScale,
            pending: false,
            error: null,
          }
          : {
            ...current[row.groupId],
            status: nextStatus,
            draft: accepted.requestedScale,
            pending: false,
            error: null,
          },
      }));
      // One scope command may change the effective value of overlapping All
      // or named scopes. Refresh every row from the owner after acceptance.
      setGroupScaleRefreshSequence((current) => current + 1);
    } catch (reason) {
      if (groupScaleInFlight.current === inFlight) {
        setGroupRuntimeScales((current) => ({
          ...current,
          [row.groupId]: current[row.groupId] === undefined
            ? {
              selector: row.runtimeScaleSelector,
              status,
              draft: scale,
              pending: false,
              error: errorMessage(reason, 'Group velocity could not be updated.'),
            }
            : {
              ...current[row.groupId],
              pending: false,
              error: errorMessage(reason, 'Group velocity could not be updated.'),
            },
        }));
      }
    } finally {
      if (groupScaleInFlight.current === inFlight) {
        groupScaleInFlight.current = null;
        scheduleGroupScaleFlush();
      }
    }
  }, [controlSource, groupScaleOwnerEpoch, scheduleGroupScaleFlush]);
  groupScaleFlush.current = () => { void flushGroupScale(); };

  useEffect(() => {
    const scaleQueue = groupScaleQueue.current;
    const identityChanged = groupScaleIdentityRef.current !== groupScaleIdentityKey;
    groupScaleIdentityRef.current = groupScaleIdentityKey;
    if (identityChanged) {
      clearGroupScaleQueue();
      groupScaleStatusRef.current.clear();
      groupScaleScopeFenceRef.current.clear();
      groupScaleInFlight.current = null;
      groupScaleLastSentAt.current = Number.NEGATIVE_INFINITY;
    }
    groupScaleGeneration.current += 1;
    const generation = groupScaleGeneration.current;
    const rows = groupScaleRowsRef.current;
    setGroupRuntimeScales((current) => Object.fromEntries(rows.map((row) => {
      const previous = current[row.groupId];
      return [row.groupId, {
        selector: row.runtimeScaleSelector,
        status: previous?.status ?? null,
        draft: previous?.draft ?? 1,
        pending: identityChanged ? false : previous?.pending ?? false,
        error: row.runtimeScaleUnavailableReason,
      } satisfies ProgramGroupRuntimeScaleState];
    })));
    if (!enabled || !snapshotAvailable || rows.length === 0) return undefined;
    let cancelled = false;
    void Promise.all(rows.map(async (row) => {
      if (row.runtimeScaleSelector === null) return;
      try {
        const status = await api.execution.runtimeScaleControl(row.runtimeScaleSelector);
        if (cancelled || groupScaleGeneration.current !== generation) return;
        if (
          runtimeScaleSelectorIdentity(status.selector)
          !== runtimeScaleSelectorIdentity(row.runtimeScaleSelector)
        ) {
          throw new Error('Runtime velocity status does not match its group scope.');
        }
        const scopeFence = runtimeScaleControlScopeFence(groupScaleOwnerEpoch, status);
        const priorFence = groupScaleScopeFenceRef.current.get(row.groupId);
        const merged = priorFence === scopeFence
          ? mergeRuntimeScaleStatus(
            groupScaleStatusRef.current.get(row.groupId) ?? null,
            status,
          )
          : status;
        if (priorFence !== undefined && priorFence !== scopeFence) {
          scaleQueue.delete(row.groupId);
        }
        groupScaleScopeFenceRef.current.set(row.groupId, scopeFence);
        groupScaleStatusRef.current.set(row.groupId, merged);
        setGroupRuntimeScales((current) => ({
          ...current,
          [row.groupId]: {
            selector: row.runtimeScaleSelector,
            status: merged,
            draft: current[row.groupId]?.pending
              ? current[row.groupId].draft : merged.requestedScale,
            pending: current[row.groupId]?.pending ?? false,
            error: null,
          },
        }));
      } catch (reason) {
        if (cancelled || groupScaleGeneration.current !== generation) return;
        setGroupRuntimeScales((current) => ({
          ...current,
          [row.groupId]: {
            selector: row.runtimeScaleSelector,
            status: null,
            draft: current[row.groupId]?.draft ?? 1,
            pending: false,
            error: errorMessage(reason, 'Group velocity status is unavailable.'),
          },
        }));
      }
    }));
    return () => {
      cancelled = true;
      scaleQueue.clear();
      if (groupScaleTimer.current !== null) {
        window.clearTimeout(groupScaleTimer.current);
        groupScaleTimer.current = null;
      }
    };
  }, [
    clearGroupScaleQueue,
    enabled,
    groupScaleIdentityKey,
    groupScaleObservationKey,
    groupScaleOwnerEpoch,
    groupScaleRefreshSequence,
    snapshotAvailable,
  ]);

  useEffect(() => {
    if (groupScaleStopActive.size === 0) return;
    for (const groupId of groupScaleStopActive) {
      groupScaleQueue.current.delete(groupId);
    }
    if (groupScaleTimer.current !== null && groupScaleQueue.current.size === 0) {
      window.clearTimeout(groupScaleTimer.current);
      groupScaleTimer.current = null;
    }
    setGroupRuntimeScales((current) => {
      let changed = false;
      const next = { ...current };
      for (const groupId of groupScaleStopActive) {
        const value = next[groupId];
        if (value?.pending) {
          next[groupId] = { ...value, pending: false };
          changed = true;
        }
      }
      return changed ? next : current;
    });
  }, [groupScaleStopActive]);

  const setGroupRuntimeScale = useCallback((groupId: string, scale: number) => {
    const row = groupScaleRowsRef.current.find((candidate) => candidate.groupId === groupId);
    const status = groupScaleStatusRef.current.get(groupId);
    if (
      row?.runtimeScaleSelector === null
      || row === undefined
      || status === undefined
      || !status.supported
      || status.frozen
      || groupScaleStopActiveRef.current.has(groupId)
      || !Number.isFinite(scale)
    ) return;
    const bounded = Math.min(1, Math.max(0, scale));
    groupScaleQueue.current.set(groupId, bounded);
    setGroupRuntimeScales((current) => ({
      ...current,
      [groupId]: current[groupId] === undefined
        ? {
          selector: row.runtimeScaleSelector,
          status,
          draft: bounded,
          pending: true,
          error: null,
        }
        : { ...current[groupId], draft: bounded, pending: true, error: null },
    }));
    scheduleGroupScaleFlush();
  }, [scheduleGroupScaleFlush]);

  const commitGroupRuntimeScale = useCallback(() => {
    if (groupScaleTimer.current !== null) {
      window.clearTimeout(groupScaleTimer.current);
      groupScaleTimer.current = null;
    }
    groupScaleFlush.current();
  }, []);

  const fullStop = useCallback(async () => {
    if (
      !state.run
      || !['running', 'intervention_required'].includes(state.run.state)
      || !state.runnerAvailable
      || ['stop', 'full-stop', 'reset']
        .includes(state.pending ?? '')
    ) return;
    const requestId = identity('program-full-stop');
    if (controlSource === null) return;
    beginProgramCommand('full-stop', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_full_stop',
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          reason: 'ordinary operator Full Stop',
        },
        requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Full Stop was rejected.', requestId, 'program', 'full-stop',
      );
    }
  }, [
    beginProgramCommand, controlSource,
    recordCommandFailure,
    state.pending, state.run, state.runnerAvailable,
  ]);

  const resumeFullStop = useCallback(async () => {
    if (
      draftUnappliedReason !== null
      || !state.run
      || state.run.state !== 'full_stopped'
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    const requestId = identity('program-full-stop-run');
    if (controlSource === null) return;
    beginProgramCommand('resume-full-stop', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_resume_full_stop',
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          expected_run_revision: state.run.runRevision,
          reason: 'operator Run after Full Stop',
        },
        requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Run after Full Stop was rejected.', requestId, 'program',
        'resume-full-stop',
      );
    }
  }, [
    beginProgramCommand, controlSource, draftUnappliedReason,
    recordCommandFailure,
    state.pending, state.run, state.runnerAvailable,
  ]);

  const runtimeControlRequest = useCallback((requestId: string) => {
    const controls = state.controls;
    const currentRun = state.run;
    if (
      controls === null
      || currentRun === null
      || state.controlPending !== null
      || controls.programRunId !== currentRun.capture.programRunId
      || controls.ownerEpoch !== currentRun.ownerEpoch
      || controls.runRevision !== currentRun.runRevision
    ) return null;
    return {
      request_id: requestId,
      request_owner_epoch: callerEpoch.current,
      source_label: 'operator_ui',
      correlation_id: requestId,
      program_run_id: controls.programRunId,
      expected_owner_epoch: controls.ownerEpoch,
      expected_run_revision: controls.runRevision,
    } satisfies Record<string, JsonValue>;
  }, [state.controlPending, state.controls, state.run]);

  /** Resolves to null when accepted, otherwise to the backend's reason. */
  const performRuntimeControl = useCallback(async (
    pending: string,
    requestId: string,
    commandKind: ExecutionControlCommandKind,
    payload: Record<string, JsonValue>,
    fallback: string,
  ): Promise<string | null> => {
    if (controlSource === null) return 'The execution control connection is unavailable.';
    beginControlCommand(pending);
    try {
      await controlSource.sendExecutionCommand(
        commandKind, payload, requestId,
      );
      // A recovery stays pending until the owner retires its offer.
      if (mounted.current && !pending.startsWith('recovery:')) {
        setState((current) => current.controlPending === pending
          ? { ...current, controlPending: null }
          : current);
      }
      return null;
    } catch (reason) {
      await recordCommandFailure(
        reason, fallback, requestId, 'control', pending,
      );
      return errorMessage(reason, fallback);
    }
  }, [beginControlCommand, controlSource, recordCommandFailure]);

  const applyRecoveryAction = useCallback((actionId: CycleRecoveryActionId) => {
    const offer = state.controls?.recoveryOffer;
    const action = offer?.actions.find((row) => row.actionId === actionId);
    if (
      offer === null || offer === undefined || action?.enabled !== true
      || state.controlPending !== null
      // Retry and continue resume the run; only a Reset ends it.
      || (!action.performsBackendReset && draftUnappliedReason !== null)
    ) return;
    const requestId = identity('program-recovery');
    const body = {
      request_id: requestId,
      request_owner_epoch: callerEpoch.current,
      source_label: 'operator_ui',
      correlation_id: requestId,
      expected_owner_epoch: state.controls?.ownerEpoch ?? '',
      offer_id: offer.offerId,
      offer_revision: offer.offerRevision,
      action_id: actionId,
      reason: 'operator-selected recovery action',
    } satisfies Record<string, JsonValue>;
    void performRuntimeControl(
      `recovery:${actionId}`,
      requestId,
      'execution_recovery',
      body,
      'The recovery action was rejected.',
    );
  }, [draftUnappliedReason, performRuntimeControl, state.controlPending, state.controls]);

  /** Resolves to null when written, otherwise to why the write was refused. */
  const writeRuntimeBlackboard = useCallback((
    key: string,
    value: JsonValue,
  ): Promise<string | null> => {
    const requestId = identity('program-runtime-blackboard');
    const base = runtimeControlRequest(requestId);
    if (base === null) {
      return Promise.resolve(
        'Not written: the run changed or another control is pending. Review the current state and try again.',
      );
    }
    return performRuntimeControl(
      `blackboard:${key}`,
      requestId,
      'program_runtime_blackboard_write',
      { ...base, key, value },
      'The runtime blackboard value was rejected.',
    );
  }, [performRuntimeControl, runtimeControlRequest]);

  // Stored persistent values are Scenario state kept by the Program runner:
  // value edits, not execution commands, so they need no current run. Each
  // edit names what the operator saw: the live run holding the values at its
  // current revision; without a run the last admitted edit wins.
  const currentRun = useRef(state.run);
  currentRun.current = state.run;
  const persistentValues = useMemo(() => {
    const requestIdentity = () => ({
      request_id: identity('program-persistent-value'),
      request_owner_epoch: callerEpoch.current,
      source_label: 'operator_ui',
    });
    return {
      read: (scenarioId: string, signal?: AbortSignal) => (
        api.programs.persistentValues(scenarioId, signal)
      ),
      /** Set a stored value, or Reset it to `initial:` when value is omitted. */
      edit: (current: PersistentKeyValues, key: string, value?: JsonValue) => {
        const fence = persistentValuesFence(current, currentRun.current);
        if (fence === null) {
          return Promise.reject(new Error(
            'The run holding these values is not the run shown here. Refresh and try again.',
          ));
        }
        return api.programs.editPersistentValue(current.scenarioId, {
          ...requestIdentity(),
          ...fence,
          scenario_id: current.scenarioId,
          key,
          expected_program_revision: current.programRevision,
          ...(value === undefined ? {} : { value }),
        });
      },
      clear: (current: PersistentKeyValues) => api.programs.clearPersistentValues(
        current.scenarioId,
        { ...requestIdentity(), scenario_id: current.scenarioId },
      ),
    };
  }, []);

  const stageManualResult = useCallback((
    outcome: 'succeeded' | 'failed',
    output: JsonValue,
  ) => {
    const offer = state.controls?.recoveryOffer;
    const requestId = identity('program-manual-result');
    const base = runtimeControlRequest(requestId);
    if (base === null || offer === null || offer === undefined) return;
    void performRuntimeControl(
      `manual-result:${outcome}`,
      requestId,
      'program_manual_result_stage',
      {
        ...base,
        node_instance_id: offer.nodeInstanceId,
        node_attempt_id: offer.nodeAttemptId,
        outcome,
        output,
      },
      'The manual result was rejected.',
    );
  }, [performRuntimeControl, runtimeControlRequest, state.controls]);

  const active = useMemo(
    () => state.run !== null && CURRENT_STATES.has(state.run.state),
    [state.run],
  );
  const cycleState = snapshot?.programCycle?.state ?? null;
  const cycleFailure = snapshot?.programCycle?.failure ?? null;
  const commandAllowed = useCallback((commandKind: string): boolean => (
    !(DRAFT_GATED_COMMANDS.has(commandKind) && draftUnappliedReason !== null)
    && (commandKind !== 'program_run' || buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, 'availability-check', deploymentPending,
    ) !== null)
    && (snapshot?.programCycle.allowedCommands.some(
      (command) => command.commandKind === commandKind && command.allowed,
    ) ?? false)
  ), [deploymentPending, draftUnappliedReason, snapshot, state.applied, state.pending, state.runnerAvailable, workspace]);
  /** Why a command is currently not allowed (unapplied Draft, then the
   * backend's reason), if any. */
  const commandDisabledReason = useCallback((commandKind: string): string | null => (
    (DRAFT_GATED_COMMANDS.has(commandKind) ? draftUnappliedReason : null)
    ?? snapshot?.programCycle.allowedCommands.find(
      (command) => command.commandKind === commandKind,
    )?.disabledReason ?? null
  ), [draftUnappliedReason, snapshot]);

  const alarms = snapshot?.alarms ?? null;
  const activeConfiguration = snapshot?.runtime?.activeConfiguration ?? null;
  /** Operator-alarm attention that refuses Program motion on the exact
   * resources an operation reserves (spec 450 §5.2), from the owner's
   * control-state alarm projection and the Kernel's held copy of it for the
   * active configuration. Never applied to manual Robot Control. */
  const motionAttentionBlock = useCallback(
    (resourceIds: readonly string[]): MotionAttentionBlock | null => (
      evaluateMotionAttention(alarms, resourceIds, activeConfiguration)
    ),
    [alarms, activeConfiguration],
  );

  return useMemo(() => ({
    state,
    cycleState,
    cycleFailure,
    commandAllowed,
    commandDisabledReason,
    motionAttentionBlock,
    draftUnappliedReason,
    active,
    refresh,
    dismissError,
    run,
    startMotion,
    stopMotion,
    resumeMotion,
    groupRuntimeScales,
    setGroupRuntimeScale,
    commitGroupRuntimeScale,
    fullStop,
    resumeFullStop,
    reset,
    applyRecoveryAction,
    writeRuntimeBlackboard,
    persistentValues,
    stageManualResult,
  }), [
    active,
    applyRecoveryAction,
    dismissError,
    cycleFailure,
    cycleState,
    commandAllowed,
    commandDisabledReason,
    motionAttentionBlock,
    draftUnappliedReason,
    persistentValues,
    refresh,
    reset,
    run,
    startMotion,
    stopMotion,
    resumeMotion,
    groupRuntimeScales,
    setGroupRuntimeScale,
    commitGroupRuntimeScale,
    fullStop,
    resumeFullStop,
    stageManualResult,
    state,
    writeRuntimeBlackboard,
  ]);
}

export type ProgramRuntimeController = ReturnType<typeof useProgramRuntime>;

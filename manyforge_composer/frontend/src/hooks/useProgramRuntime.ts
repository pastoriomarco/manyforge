import { useCallback, useEffect, useMemo, useRef, useState, useSyncExternalStore } from 'react';

import { ApiError, api, apiErrorCode, type PersistentKeyValuesFence } from '../api/client';
import {
  displayId,
  type ApplicationStartId,
} from '../api/applicationIdentity';
import { errorMessage } from '../state/structuralResource';
import { failureContextText } from '../state/structuredError';
import { commandRefusalCause } from '../state/commandRefusal';
import {
  awaitCommandSuccess,
  CommandEndedError,
  commandProgressText,
} from '../state/commandRows';
import { useCommandRowWaiter } from './useCommandRowWaiter';
import type {
  AppliedProgramSelection,
  AppliedProgramStatus,
  CommandRow,
  CurrentScenarioResponse,
  JsonValue,
  CycleRecoveryActionId,
  CycleRecoveryOffer,
  PersistentKeyValues,
  ProgramRuntimeControlProjection,
  ProgramGroupRuntimeScaleState,
  ProgramCycleStatus,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
  RunnerCommandRowKind,
  StructuredError,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import type { CellLatch, CellState, RunReadiness } from '../types/components';
import { useActivateExecute } from './useActivateExecute';
import {
  CommandDeliveryUnconfirmedError,
  type ExecutionCommandReceipt,
  type ExecutionCommandSessionIdentity,
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

/** The Composer refuses these starts with `execution_assistant_turn_in_progress` meanwhile. */
export const ASSISTANT_TURN_IN_PROGRESS = 'An assistant request is running: wait or cancel it';
/**
 * Why a Program runner command waits while another runs: the runner refuses
 * it at once (OD-144). Stop, Stop all and Halt never pass through the runner.
 */
export const PROGRAM_COMMAND_REASON = 'Not available while another Program command is running.';

/** The busy reason naming the runner's command in progress and its step (OD-214). */
export function programCommandReason(row: CommandRow): string {
  return `${PROGRAM_COMMAND_REASON.slice(0, -1)}: ${commandProgressText(row)}.`;
}

const CURRENT_STATES = new Set([
  'running',
  'intervention_required',
  'halting',
  'halted',
  'stopping',
]);
type ProgramCycleDebugProjection = Pick<ProgramCycleStatus,
  | 'cycleGeneration'
  | 'state'
  | 'programRunId'
  | 'cycleClaimGeneration'
> & {
  admissionGeneration?: number;
  authoringAllowed?: boolean;
  /** The current application start, for the display form of ids. */
  applicationStartId?: ApplicationStartId;
};

/** A run number as the operator reads it (`3f9a/run3`, D4); bare without a start. */
export function displayRunId(
  applicationStartId: ApplicationStartId | null | undefined,
  programRunId: number,
): string {
  return applicationStartId == null
    ? `run${programRunId}`
    : displayId(applicationStartId, 'run', programRunId);
}

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
  const start = cycle?.applicationStartId;
  const runContext = run?.reason ? failureContextText(run.reason) : null;
  const recoveryContext = recovery === null ? null : failureContextText(recovery.reason);
  return [
    run === null
      ? 'Observed run: none.'
      : [
        `Observed run ${displayRunId(start, run.capture.programRunId)}: ${run.state}, revision ${run.runRevision}, tree ${run.treeStatus}, active operations ${run.activeOperationCount ?? 'unknown'}.`,
        run.reason === null ? null : `Run reason: ${run.reason.code}.`,
        run.message ? `Run message: ${run.message}.` : null,
        runContext === null ? null : `Run failure details: ${runContext}.`,
        activeNodes.length === 0 ? null : `Current nodes: ${activeNodes.join(', ')}.`,
      ].filter((value): value is string => value !== null).join(' '),
    cycle === null
      ? 'Observed cycle: unavailable.'
      : `Observed cycle: ${cycle.state}, generation ${cycle.cycleGeneration}, run ${cycle.programRunId === null ? 'none' : displayRunId(start, cycle.programRunId)}, claim ${cycle.cycleClaimGeneration ?? 'none'}.`,
    recovery === null
      ? 'Observed recovery: none.'
      : [
        `Observed recovery ${recovery.recoveryClass}: ${recovery.status}, offer revision ${recovery.offerRevision}, node ${recovery.nodeInstanceId}, operation ${recovery.operationId === null || start === undefined
          ? recovery.operationId : displayId(start, 'op', recovery.operationId)}.`,
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

/** Identity of a failed run's notice: run id and revision; null otherwise. */
export function programFailureNoticeKey(run: ProgramRunStatus | null): string | null {
  return run?.state === 'failed'
    ? `${run.capture.programRunId}@${run.runRevision}`
    : null;
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
    snapshot.applicationStartId,
    manager.sourceOwnerId ?? 'no-manager-owner',
    typeof active?.retained_bundle_id === 'string'
      ? active.retained_bundle_id : 'no-retained-bundle',
    typeof active?.graph_revision === 'string'
      ? active.graph_revision : 'no-graph-revision',
    basis?.appliedProgramGeneration ?? 'no-applied-generation',
  ].join(':');
}

export type ProgramPendingCommand =
  | 'run'
  | 'start-motion'
  | 'motion-stop'
  | 'resume-motion'
  | 'stop-all'
  | 'halt'
  | 'resume-halt'
  | 'reset'
  | null;

export interface ProgramSelectionPending {
  kind: 'start-motion' | 'motion-stop' | 'resume-motion';
  groupIds: string[];
  programRunId: number;
  /** The hold revision the command was sent from, when known. */
  holdRevision?: number;
}

/**
 * What a pending Program command was sent from. A pending flag clears on the
 * first projection that shows the command's effect, or that this origin no
 * longer holds; never only on one transient state, which coalesced state
 * messages can skip.
 */
export interface ProgramPendingOrigin {
  cycleGeneration: number | null;
  programRunId: number | null;
  runRevision: number | null;
}

interface ProgramRuntimeState {
  applied: AppliedProgramSelection | null;
  /** The start of the snapshot under which `applied` was read; source
   * generations restart with each start. */
  appliedApplicationStartId?: string | null;
  /** The Applied Program is preserved but not executable in the active
   * Deployment; derived on every refresh, never stored as a flag. */
  nonExecutable: AppliedProgramStatus | null;
  currentScenario: CurrentScenarioResponse | null;
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
  runnerAvailable: boolean;
  loading: boolean;
  observationError: string | null;
  pending: ProgramPendingCommand;
  /** What `pending` was sent from; absent when nothing is pending. */
  pendingOrigin?: ProgramPendingOrigin | null;
  selectionPending: ProgramSelectionPending[];
  controlPending: string | null;
  error: string | null;
  /** The failed run (`programFailureNoticeKey`) whose synthesized notice
   * `error` holds; null when `error` is a command message. */
  failureNoticeKey?: string | null;
  /** The failed run whose notice was dismissed or superseded by a command. */
  dismissedFailureKey?: string | null;
  errorRefusal: StructuredError | null;
}

/**
 * Runner commands whose per-command allowance the GUI reads:
 * Stop, Halt, Reset, runtime scale and blackboard writes, and the Runner's own
 * Run gate beside RunReadiness.
 */
export type RunnerCommandKind =
  | 'program_run'
  | 'program_reset'
  | 'program_motion_stop'
  | 'runtime_scale'
  | 'program_runtime_blackboard_write';

const INITIAL_STATE: ProgramRuntimeState = {
  applied: null,
  appliedApplicationStartId: null,
  nonExecutable: null,
  currentScenario: null,
  run: null,
  controls: null,
  runnerAvailable: false,
  loading: true,
  observationError: null,
  pending: null,
  pendingOrigin: null,
  selectionPending: [],
  controlPending: null,
  error: null,
  errorRefusal: null,
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
export function appliedKey(
  value: AppliedProgramSelection | null,
  applicationStartId: string | null | undefined,
): string {
  return value === null
    ? 'none'
    : `${applicationStartId ?? 'no-start'}:${value.sourceGeneration}:${value.recordDigest}`;
}

export function programRunProjectionKey(value: ProgramRunStatus | null): string {
  return value === null
    ? 'none'
    : `${value.capture.programRunId}:`
      + `${value.runRevision}:${value.nodeStatusRevision}`;
}

function controlsKey(value: ProgramRuntimeControlProjection | null): string {
  return value === null
    ? 'none'
    : `${value.programRunId}:${value.runRevision}:`
      + `${value.runtimeControls.runtimeControlRevision}:`
      + `${value.recoveryOffer?.offerId ?? 'no-offer'}:`
      + `${value.recoveryOffer?.offerRevision ?? 0}:`
      + (value.recoveryHolds ?? []).map((hold) => (
        `${hold.offer.offerId}@${hold.offer.offerRevision}`
      )).join(',') + ':'
      + JSON.stringify(value.executionSelections);
}

/** The offer with this ID: the focused one (an operator Stop too) or a failure hold's. */
function offerWithId(
  value: ProgramRuntimeControlProjection | null,
  offerId: string,
): CycleRecoveryOffer | null {
  const focused = value?.recoveryOffer ?? null;
  if (focused?.offerId === offerId) return focused;
  return value?.recoveryHolds.find((hold) => hold.offer.offerId === offerId)?.offer ?? null;
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
  // `recovery:<offer id>:<action id>` is pending until that offer's revision
  // changes or the owner retires it (offer IDs are digests and contain a
  // colon; action IDs do not).
  const offerId = pending.slice('recovery:'.length, pending.lastIndexOf(':'));
  const expected = offerWithId(previous, offerId);
  const current = offerWithId(observed, offerId);
  if (expected === null) return pending;
  return current === null || current.offerRevision !== expected.offerRevision
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
    || controls.programRunId !== run.capture.programRunId
  ) return false;
  return controls.executionSelections.groups.some(
    (group) => group.groupId === 'all' && group.state === 'held',
  );
}

/** The run a Program command addressed is gone, changed or advanced. */
function programRunLeftOrigin(
  run: ProgramRunStatus | null,
  cycle: ProgramCycleDebugProjection | null,
  origin: ProgramPendingOrigin | null,
): boolean {
  if (cycle !== null && cycle.state !== 'running') return true;
  if (origin === null || origin.programRunId === null) return false;
  return run === null
    || run.capture.programRunId !== origin.programRunId
    || (origin.runRevision !== null && run.runRevision > origin.runRevision);
}

/** Cycle controls mirror owner transitions; selected commands track delivery only. */
export function programPendingAfterProjection(
  pending: ProgramPendingCommand,
  run: ProgramRunStatus | null,
  _controls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleDebugProjection | null = null,
  previousRun: ProgramRunStatus | null = run,
  origin: ProgramPendingOrigin | null = null,
): ProgramPendingCommand {
  if (pending === 'start-motion' || pending === 'motion-stop' || pending === 'resume-motion') {
    // Delivery feedback belongs to its captured run, not a successor/idle
    // projection. Its late reply cannot clear a newer request's exact token.
    return run === null
      || run.capture.programRunId !== previousRun?.capture.programRunId
      || !['running', 'intervention_required'].includes(run.state)
      ? null : pending;
  }
  if (pending === 'run') {
    if (run !== null
      && run.state === 'running'
      && cycle?.state === 'running'
      && cycle.programRunId === run.capture.programRunId) return null;
    // Sent from an idle cycle: a captured run in any state (it may be held
    // or halted before a `running` projection is seen) or a later cycle
    // shows it took effect.
    if (cycle !== null && (
      (cycle.state === 'running' && cycle.programRunId !== null)
      || (origin?.cycleGeneration != null && cycle.cycleGeneration > origin.cycleGeneration)
    )) return null;
    return cycle !== null
      && ['resetting', 'terminal_awaiting_reset', 'recovery_required']
        .includes(cycle.state)
      ? null : pending;
  }
  if (pending === 'reset') {
    // Reset ends in a new idle cycle generation; a later cycle (another
    // client may already have started a run) also shows it took effect.
    return cycle?.state === 'idle_after_reset'
      || (cycle != null && origin?.cycleGeneration != null
        && cycle.cycleGeneration > origin.cycleGeneration)
      ? null : pending;
  }
  if (pending === 'halt') {
    if (run?.state === 'halted') return null;
    // The run shows a Halt with a new run revision; the run ending, being
    // replaced or the cycle leaving `running` also ends the wait. Without a
    // run nothing shows it: the receipt ends the wait (`halt`).
    return programRunLeftOrigin(run, cycle, origin)
      || ['stopped', 'succeeded', 'failed'].includes(run?.state ?? '')
      ? null : pending;
  }
  if (pending === 'resume-halt') {
    if (run?.state === 'running' && cycle?.state === 'running') return null;
    // Sent from a halted run: the resumed run may be held again by its first
    // tree tick (`intervention_required`) before `running` is ever seen.
    return programRunLeftOrigin(run, cycle, origin)
      || (run !== null && run.state !== 'halted')
      ? null : pending;
  }
  return pending;
}

/**
 * The Execute request for the Applied Program. Whether Execute is allowed is
 * the backend's `RunReadiness`; this only names what runs.
 */
export function buildProgramStartRequest(
  applied: AppliedProgramSelection | null,
  workspace: ProgramWorkspaceResponse | null,
): Record<string, JsonValue> | null {
  if (applied === null || workspace === null || workspace.draft.programRevision == null) return null;
  return {
    source_label: 'operator_ui',
    scenario_id: applied.scenarioId,
    source_document_digest: applied.sourceDocumentDigest,
    expected_applied_record_digest: applied.recordDigest,
    draft_program_revision: workspace.draft.programRevision,
  };
}

/** The exact Reset this browser sent: the cycle it ends and its command. */
export interface ProgramResetIdentity {
  /** The WebSocket command (session number and client sequence) that sent it. */
  command: ExecutionCommandSessionIdentity;
  programRunId: number | null;
  cycleGeneration: number;
}

/** The exact cycle a caller confirmed Reset for; any other is refused unsent. */
export type ProgramResetExpectation = Pick<
  ProgramResetIdentity, 'programRunId' | 'cycleGeneration'
>;

/** Optional explicit restoration effects; omitted selectors preserve known state. */
export interface ProgramResetOptions {
  reloadInitialScene?: boolean;
  resetRobotPosition?: boolean;
}

export interface ProgramResetCallOptions extends ProgramResetOptions {
  expected?: ProgramResetExpectation;
}

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
  options: ProgramResetOptions = {},
): Record<string, JsonValue> | null {
  const cycle = snapshot?.programCycle;
  const allowed = cycle?.allowedCommands.some(
    (command) => command.commandKind === 'program_reset' && command.allowed,
  ) ?? false;
  // A failed Reset (recovery_required that retains its accepted occurrence)
  // is retried with exactly that occurrence's options, run or no run.
  const retainedReset = cycle?.state === 'recovery_required'
    && cycle.currentCommandKind === 'program_reset'
    ? cycle.resetOptions : null;
  if (
    cycle === undefined
    || !allowed
    // A recovery_required cycle whose acquire outcome is unknown announces no
    // run; its Reset names the cycle generation alone.
    || (cycle.programRunId === null && retainedReset === null
      && cycle.state !== 'recovery_required' && (cycle.state !== 'idle_after_reset'
      || cycle.cycleClaimGeneration !== null || cycle.terminalOrSettlementReference !== null))
  ) return null;
  const selected = retainedReset ?? options;
  return {
    source_label: 'operator_ui',
    program_run_id: cycle.programRunId,
    expected_cycle_generation: cycle.cycleGeneration,
    expected_claim_generation: cycle.cycleClaimGeneration,
    terminal_reference: cycle.terminalOrSettlementReference,
    settlement_evidence_digest: cycle.terminalOrSettlementReference,
    reload_initial_scene: selected.reloadInitialScene ?? false,
    reset_robot_position: selected.resetRobotPosition ?? false,
  };
}

function controlsMatchRun(
  controls: ProgramRuntimeControlProjection | null,
  run: ProgramRunStatus,
): controls is ProgramRuntimeControlProjection {
  return controls !== null
    && controls.programRunId === run.capture.programRunId
    && controls.runRevision === run.runRevision;
}

function runtimeScaleSelectorWire(
  selector: RuntimeScaleControlSelector,
): Record<string, JsonValue> {
  return selector.kind === 'singular'
    ? {
      program_binding_id: selector.programBindingId,
      component_id: selector.componentId,
      command_target_id: selector.commandTargetId,
    }
    : {
      program_binding_id: selector.programBindingId,
      group_targets: selector.groupTargets.map((target) => ({
        component_id: target.componentId,
        command_target_id: target.commandTargetId,
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
    request.programRunId === run.capture.programRunId
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
    // The owner applied a selection change after this command was sent.
    if (request.holdRevision !== undefined
      && controls.executionSelections.holdRevision > request.holdRevision) {
      return false;
    }
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
  run: { capture: { programRunId: number }; runRevision: number } | null,
): PersistentKeyValuesFence | null {
  if (current.programRunId === null) return {};
  if (run === null || run.capture.programRunId !== current.programRunId) return null;
  return { program_run_id: current.programRunId, expected_run_revision: run.runRevision };
}

const subscribeNothing = () => () => undefined;
const EMPTY_CELL_STATE: CellState = { components: null, latch: null, io: null, readiness: null };
const noCellState = () => EMPTY_CELL_STATE;

export function useProgramRuntime(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null = null,
  workspace: ProgramWorkspaceResponse | null = null,
  controlSource: HeadlessStateSource | null = null,
  deploymentPending = false,
  assistantTurnRunning = false,
) {
  const [state, setState] = useState<ProgramRuntimeState>(INITIAL_STATE);
  // The backend's run readiness and cell latch, pushed on /ws/control.
  const cellState = useSyncExternalStore(
    controlSource?.subscribeCellState ?? subscribeNothing,
    controlSource?.getCellState ?? noCellState,
    controlSource?.getCellState ?? noCellState,
  );
  const readiness: RunReadiness | null = cellState.readiness;
  const cellLatch: CellLatch | null = cellState.latch;
  const mounted = useRef(true);
  // This page's runner commands wait here for their published end (OD-214).
  const runnerRows = useCommandRowWaiter(
    snapshot?.programCycle.runnerCommands ?? null,
    snapshot?.applicationStartId ?? null,
  );
  /** Resolves when a runner command's row ends succeeded; throws CommandEndedError otherwise. */
  const commandEnd = useCallback(async (receipt: ExecutionCommandReceipt): Promise<void> => {
    if (receipt.command !== null) await awaitCommandSuccess(receipt.command, runnerRows);
  }, [runnerRows]);
  // The Program command `pending` belongs to: a fresh object per command, so a
  // late reply never clears a newer command's pending flag.
  const pendingProgramRequest = useRef<object | null>(null);
  // The cycle the next Program command is sent from (see ProgramPendingOrigin).
  const latestCycleGeneration = useRef<number | null>(null);
  latestCycleGeneration.current = snapshot?.programCycle.cycleGeneration ?? null;
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
  const applicationStartId = snapshot?.applicationStartId ?? null;
  const applicationStartIdRef = useRef(applicationStartId);
  applicationStartIdRef.current = applicationStartId;
  const wakeKey = programRuntimeWakeKey(snapshot);
  const snapshotAvailable = snapshot !== null;
  const cycleDebugProjection = useMemo<ProgramCycleDebugProjection | null>(
    () => snapshot === null ? null : {
      cycleGeneration: snapshot.programCycle.cycleGeneration,
      state: snapshot.programCycle.state,
      programRunId: snapshot.programCycle.programRunId,
      cycleClaimGeneration: snapshot.programCycle.cycleClaimGeneration,
      admissionGeneration: snapshot.authoringAdmission.admissionGeneration,
      authoringAllowed: snapshot.authoringAdmission.authoringAllowed,
      applicationStartId: snapshot.applicationStartId,
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
    // The start this read is made under; a read sent after the tab admitted
    // a new start reaches only that start's Headless.
    const requestApplicationStartId = applicationStartIdRef.current;
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
          appliedApplicationStartId: applied === null ? null : requestApplicationStartId,
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
          && appliedKey(current.applied, current.appliedApplicationStartId)
            === appliedKey(next.applied, next.appliedApplicationStartId)
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
        current.pendingOrigin ?? null,
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
      // The failed-run notice is shown once per run revision: it clears when
      // Reset reaches idle or another run is observed, and stays away after
      // Dismiss or a Program command.
      const failureKey = programFailureNoticeKey(projected.run);
      let error = current.error;
      let failureNoticeKey = current.failureNoticeKey ?? null;
      if (failureNoticeKey !== null && (
        cycle.state === 'idle_after_reset' || failureKey !== failureNoticeKey
      )) {
        error = null;
        failureNoticeKey = null;
      }
      if (
        cycle.state !== 'idle_after_reset'
        && error === null
        && failureKey !== null
        && failureKey !== current.dismissedFailureKey
      ) {
        error = programTerminalFailureMessage(
          projected.run,
          projected.controls,
          cycleDebugProjection,
        );
        failureNoticeKey = failureKey;
      }
      if (
        programRunProjectionKey(current.run) === programRunProjectionKey(projected.run)
        && controlsKey(current.controls) === controlsKey(projected.controls)
        && current.runnerAvailable
        && current.pending === pending
        && sameSelectionPending(current.selectionPending, selectionPending)
        && current.controlPending === controlPending
        && current.error === error
        && (current.failureNoticeKey ?? null) === failureNoticeKey
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
        failureNoticeKey,
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
      // unmount never submits a Stop.
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
  const groupScaleApplicationStartId = snapshot?.executionControlStatus?.applicationStartId ?? '';
  const groupScaleIdentityKey = JSON.stringify([
    groupScaleApplicationStartId,
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
    // A Stop all (the All row, Halt's first step) stops every group.
    const allProgramStop = ['stopping', 'halting'].includes(state.run?.state ?? '')
      || state.pending === 'stop-all';
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
  }, [groupScaleRows, state.pending, state.run?.state, state.selectionPending]);
  groupScaleStopActiveRef.current = groupScaleStopActive;

  const dismissError = useCallback(() => {
    setState((current) => current.error === null
      ? current
      : {
        ...current,
        error: null,
        errorRefusal: null,
        failureNoticeKey: null,
        dismissedFailureKey: programFailureNoticeKey(current.run)
          ?? current.dismissedFailureKey,
      });
  }, []);

  const beginProgramCommand = useCallback((
    pending: Exclude<ProgramPendingCommand, null>,
    request: object,
    selectionGroups: string[] = [],
  ) => {
    pendingProgramRequest.current = request;
    const cycleGeneration = latestCycleGeneration.current;
    setState((current) => ({
      ...current,
      pending,
      pendingOrigin: {
        cycleGeneration,
        programRunId: current.run?.capture.programRunId ?? null,
        runRevision: current.run?.runRevision ?? null,
      },
      selectionPending: pending === 'start-motion'
        || pending === 'motion-stop' || pending === 'resume-motion'
        ? [
          ...current.selectionPending,
          {
            kind: pending,
            groupIds: [...selectionGroups],
            programRunId: current.run?.capture.programRunId ?? 0,
            ...(current.controls !== null && current.run !== null
              && controlsMatchRun(current.controls, current.run)
              ? { holdRevision: current.controls.executionSelections.holdRevision }
              : {}),
          },
        ]
        : current.selectionPending,
      error: null,
      // A Program command (Reset, Run, ...) supersedes the failed-run notice.
      failureNoticeKey: null,
      errorRefusal: null,
      dismissedFailureKey: programFailureNoticeKey(current.run)
        ?? current.dismissedFailureKey,
    }));
  }, []);

  const beginControlCommand = useCallback((pending: string) => {
    setState((current) => ({
      ...current, controlPending: pending, error: null, errorRefusal: null, failureNoticeKey: null,
    }));
  }, []);

  const recordCommandFailure = useCallback(async (
    reason: unknown,
    fallback: string,
    request: object | null,
    pendingKind: 'program' | 'control',
    expectedPending: string,
    expectedRun?: ProgramRunStatus,
    selectionGroups: string[] = [],
  ) => {
    if (!mounted.current) return;
    // A command's published end is its true outcome: it is shown even when
    // this page's pending feedback has already moved on.
    const ended = reason instanceof CommandEndedError;
    setState((current) => {
      const selectionRequest = expectedPending === 'start-motion'
        || expectedPending === 'motion-stop'
        || expectedPending === 'resume-motion';
      // A selection command's entry: the first one of its kind for its run
      // and groups.
      const selectionIndex = selectionRequest && expectedRun !== undefined
        ? current.selectionPending.findIndex((entry) => (
          entry.kind === expectedPending
          && entry.programRunId === expectedRun.capture.programRunId
          && entry.groupIds.length === selectionGroups.length
          && entry.groupIds.every((id, index) => id === selectionGroups[index])
        ))
        : -1;
      const stale = (pendingKind === 'program' && (
        selectionRequest
          ? selectionIndex < 0
          : current.pending !== expectedPending
            || pendingProgramRequest.current !== request
      ))
        || (pendingKind === 'control'
          && current.controlPending !== expectedPending)
        || (expectedRun !== undefined
          && current.run?.capture.programRunId !== expectedRun.capture.programRunId);
      if (stale && !ended) return current;
      return {
        ...current,
        pending: !stale && pendingKind === 'program'
          && (!selectionRequest || pendingProgramRequest.current === request)
          ? null : current.pending,
        selectionPending: !stale && pendingKind === 'program'
          ? current.selectionPending.filter((_, index) => index !== selectionIndex)
          : current.selectionPending,
        controlPending: !stale && pendingKind === 'control' ? null : current.controlPending,
        error: [
          errorMessage(reason, fallback),
          'Backend state remains authoritative.',
        ].join(' '),
        failureNoticeKey: null,
        errorRefusal: commandRefusalCause(reason),
      };
    });
  }, []);

  // Execute, Run, Resume and Retry use only what is applied: while a Draft
  // has unapplied changes they are refused, and while this page's assistant
  // chat turn runs (the Composer backend enforces both where it admits them).
  // Stop, Halt and Reset stay available.
  const programPending = state.applied !== null && workspace !== null
    && workspace.draft.programLoaded
    && workspace.draft.programRevision !== state.applied.programRevision;
  const pendingDocuments = [
    ...(programPending ? ['Program (Scene edits included)'] : []),
    ...(deploymentPending ? ['Deployment'] : []),
  ];
  const composerRefusalReason = pendingDocuments.length > 0
    ? `Apply or Discard first: unapplied changes in ${pendingDocuments.join(' and ')}. Execute, Run, Resume and Retry use only what is applied.`
    : assistantTurnRunning ? ASSISTANT_TURN_IN_PROGRESS : null;

  const ordinaryRun = useCallback(async () => {
    const request = buildProgramStartRequest(state.applied, workspace);
    if (request === null || state.pending !== null) return;
    if (controlSource === null) return;
    const sent = {};
    beginProgramCommand('run', sent);
    try {
      await commandEnd(await controlSource.sendExecutionCommand('program_run', request));
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Run was rejected.', sent, 'program', 'run',
      );
    }
  }, [
    beginProgramCommand, commandEnd, controlSource, recordCommandFailure,
    state.applied, state.pending, workspace,
  ]);

  /**
   * Send one Reset for the cycle the caller names and report what is known.
   *
   * `expected` is the exact run and cycle generation the caller
   * confirmed: when the current observation differs, nothing is sent. A
   * definite refusal (not sent, or refused by the owner) is distinct from a
   * submitted Reset whose reply was lost: that one may have been received, so
   * it keeps the cycle it was sent for and must never be resent. Neither
   * outcome is settlement: callers that continue after Reset observe its
   * cycle Idle.
   */
  const reset = useCallback(async (
    options: ProgramResetCallOptions = {},
  ): Promise<ProgramResetOutcome> => {
    const cycle = snapshot?.programCycle ?? null;
    const { expected } = options;
    if (expected !== undefined && (
      cycle === null
      || cycle.programRunId !== expected.programRunId
      || cycle.cycleGeneration !== expected.cycleGeneration
    )) {
      return {
        status: 'refused',
        message: `Program Reset was not sent: the observed cycle is no longer run ${expected.programRunId === null ? null
          : displayRunId(snapshot?.applicationStartId, expected.programRunId)} at cycle generation ${expected.cycleGeneration}.`,
      };
    }
    const request = buildProgramResetRequest(snapshot, options);
    if (request === null || controlSource === null || cycle === null) {
      const message = request === null
        ? 'Program Reset was not sent because its backend permission or cycle identity changed. Controls were refreshed.'
        : 'Program Reset was not sent because the execution control connection is unavailable. Controls were refreshed.';
      if (mounted.current) {
        setState((current) => ({ ...current, error: message, errorRefusal: null, failureNoticeKey: null }));
      }
      await refresh();
      return { status: 'refused', message };
    }
    const sent: Omit<ProgramResetIdentity, 'command'> = {
      programRunId: cycle.programRunId,
      cycleGeneration: cycle.cycleGeneration,
    };
    beginProgramCommand('reset', sent);
    try {
      // Reset is a WebSocket command: its receipt names its session and
      // client sequence.
      const receipt = await controlSource.sendExecutionCommand('program_reset', request);
      // Callers follow the cycle; a Reset that ends other than succeeded
      // shows its reason where a refused Reset does.
      void commandEnd(receipt).catch((reason: unknown) => recordCommandFailure(
        reason, 'Program Reset was rejected.', sent, 'program', 'reset',
      ));
      return { status: 'sent', reset: { ...sent, command: {
        controlSessionNumber: receipt.controlSessionNumber!,
        clientSequence: receipt.clientSequence!,
      } } };
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Reset was rejected.', sent, 'program', 'reset',
      );
      return reason instanceof CommandDeliveryUnconfirmedError
        ? { status: 'unconfirmed', reset: { ...sent, command: reason.command! }, message: reason.message }
        : { status: 'refused', message: errorMessage(reason, 'Program Reset was rejected.') };
    }
  }, [
    beginProgramCommand, commandEnd, controlSource,
    recordCommandFailure, refresh, snapshot,
  ]);

  const selectionCommand = useCallback(async (
    pending: 'start-motion' | 'motion-stop' | 'resume-motion', groupIds: string[],
  ): Promise<boolean> => {
    const start = pending === 'start-motion';
    const resume = pending === 'resume-motion';
    const controls = state.controls;
    if (
      ((start || resume) && composerRefusalReason !== null)
      || groupIds.length === 0
      || !state.run
      // A motion Stop needs only a run to stop: nothing in progress holds it
      // back. While a Stop or Halt ends the run, the owner answers a
      // repeat with no effect.
      || !(start || resume
        ? ['running', 'intervention_required']
        : ['running', 'intervention_required', 'stopping', 'halting'])
        .includes(state.run.state)
      || ((start || resume) && (!state.runnerAvailable
        || ['halt', 'reset'].includes(state.pending ?? '')))
      || ((start || resume) && (state.selectionPending.length > 0 || state.controlPending !== null
        || !controlsMatchRun(controls, state.run)
        || !groupIds.every((id) => controls.executionSelections.groups.some(
          (group) => group.groupId === id
            && (start ? group.startAllowed : group.resumeAllowed),
        ))))
    ) return false;
    if (controlSource === null) return false;
    const sent = {};
    beginProgramCommand(pending, sent, groupIds);
    try {
      const receipt = await controlSource.sendExecutionCommand(
        start
          ? 'program_motion_start'
          : resume ? 'program_motion_resume' : 'program_motion_stop',
        {
          source_label: 'operator_ui',
          program_run_id: state.run.capture.programRunId,
          group_ids: groupIds,
          // Run/Resume is fenced on run number and hold revision; tree
          // ticks move the run revision without changing it.
          ...((start || resume) && controls !== null ? {
            expected_hold_revision: controls.executionSelections.holdRevision,
          } : {}),
          reason: `operator ${pending}`,
        },
      );
      await commandEnd(receipt);
      return true;
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Selection command was rejected.', sent, 'program', pending, state.run, groupIds,
      );
      return false;
    } finally {
      // This clears only Sending feedback, never a hold or lifecycle state.
      // A newer priority Stop must not be cleared by an older Resume reply.
      if (mounted.current) setState((current) => ({
        ...current,
        pending: current.pending === pending
          && pendingProgramRequest.current === sent
          ? null : current.pending,
        // Keep accepted scope feedback until the owner projection proves the
        // requested state. A receipt only proves admission; removing it here
        // would briefly expose a duplicate command while Stop/Resume settles.
        selectionPending: current.selectionPending,
      }));
    }
  }, [
    beginProgramCommand, commandEnd, controlSource, composerRefusalReason,
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
    const scopeFence = runtimeScaleControlScopeFence(groupScaleApplicationStartId, status);
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
      const receipt = await controlSource.sendExecutionCommand(
        'runtime_scale',
        {
          selector: runtimeScaleSelectorWire(status.selector),
          expected_control_generation: status.controlGeneration,
          scale,
        },
      );
      if (
        groupScaleInFlight.current !== inFlight
        || groupScaleScopeFenceRef.current.get(row.groupId) !== scopeFence
        || groupScaleStopActiveRef.current.has(row.groupId)
      ) return;
      const accepted = parseRuntimeScaleControlAccepted(receipt.result);
      // A re-read may have replaced the status while this command was in
      // flight; keep whichever owner generation is newer.
      const nextStatus = mergeRuntimeScaleStatus(
        groupScaleStatusRef.current.get(row.groupId) ?? null,
        acceptedRuntimeScaleStatus(status, accepted, scale),
      );
      groupScaleStatusRef.current.set(row.groupId, nextStatus);
      groupScaleScopeFenceRef.current.set(row.groupId, scopeFence);
      // A value queued during this command keeps the slider where the drag is.
      const queued = groupScaleQueue.current.get(row.groupId);
      setGroupRuntimeScales((current) => ({
        ...current,
        [row.groupId]: current[row.groupId] === undefined
          ? {
            selector: row.runtimeScaleSelector,
            status: nextStatus,
            draft: queued ?? accepted.requestedScale,
            pending: queued !== undefined,
            error: null,
          }
          : {
            ...current[row.groupId],
            status: nextStatus,
            draft: queued ?? accepted.requestedScale,
            pending: queued !== undefined,
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
              draft: status.requestedScale,
              pending: false,
              error: errorMessage(reason, 'Group velocity could not be updated.'),
            }
            : {
              ...current[row.groupId],
              draft: status.requestedScale,
              pending: false,
              error: errorMessage(reason, 'Group velocity could not be updated.'),
            },
        }));
        // A refusal (e.g. the control frozen while a Stop is active) means
        // this row's status may be outdated.
        setGroupScaleRefreshSequence((current) => current + 1);
      }
    } finally {
      if (groupScaleInFlight.current === inFlight) {
        groupScaleInFlight.current = null;
        scheduleGroupScaleFlush();
      }
    }
  }, [controlSource, groupScaleApplicationStartId, scheduleGroupScaleFlush]);
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
        const scopeFence = runtimeScaleControlScopeFence(groupScaleApplicationStartId, status);
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
    // Only cancel this read. Identity changes clear the queue above and Stops
    // clear it below; a refresh must not drop the value of a drag in progress.
    return () => {
      cancelled = true;
    };
  }, [
    clearGroupScaleQueue,
    enabled,
    groupScaleIdentityKey,
    groupScaleObservationKey,
    groupScaleApplicationStartId,
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

  /**
   * One cell-wide Stop command (`stop_all` or `halt`). Neither names a run or
   * needs one: nothing in progress holds it back and a repeat is accepted as
   * another Stop. Resolves to whether it was received.
   */
  const cellStop = useCallback(async (
    commandKind: 'stop_all' | 'halt',
    pending: 'stop-all' | 'halt',
    fallback: string,
  ): Promise<boolean> => {
    if (controlSource === null) return false;
    const sent = {};
    // Only a live run shows a Halt; otherwise its receipt ends the wait.
    const runShowsHalt = pending === 'halt' && state.run !== null
      && ['running', 'intervention_required', 'stopping', 'halting'].includes(state.run.state);
    beginProgramCommand(pending, sent);
    try {
      await controlSource.sendExecutionCommand(commandKind, {});
      if (!runShowsHalt && mounted.current) {
        setState((current) => current.pending === pending && pendingProgramRequest.current === sent
          ? { ...current, pending: null } : current);
      }
      return true;
    } catch (reason) {
      await recordCommandFailure(reason, fallback, sent, 'program', pending);
      return false;
    }
  }, [beginProgramCommand, controlSource, recordCommandFailure, state.run]);

  /** Stop all: every motion in the cell, manual moves included; the Program's groups stay held. */
  const stopAll = useCallback(
    () => cellStop('stop_all', 'stop-all', 'Stop all was rejected.'),
    [cellStop],
  );
  /** Halt: Stop all and freeze the Program until Execute or Reset. */
  const halt = useCallback(
    () => cellStop('halt', 'halt', 'Halt was rejected.'),
    [cellStop],
  );

  /** Execute after Halt: unfreezes the halted Program; motion holds stay until Resume. */
  const resumeHalt = useCallback(async () => {
    if (
      composerRefusalReason !== null
      || !state.run
      || state.run.state !== 'halted'
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    if (controlSource === null) return;
    const sent = {};
    beginProgramCommand('resume-halt', sent);
    try {
      await commandEnd(await controlSource.sendExecutionCommand(
        'program_resume_halt',
        {
          source_label: 'operator_ui',
          program_run_id: state.run.capture.programRunId,
          expected_run_revision: state.run.runRevision,
          reason: 'operator Execute after Halt',
        },
      ));
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Execute after Halt was rejected.', sent, 'program',
        'resume-halt',
      );
    }
  }, [
    beginProgramCommand, commandEnd, controlSource, composerRefusalReason,
    recordCommandFailure,
    state.pending, state.run, state.runnerAvailable,
  ]);

  const runtimeControlRequest = useCallback(() => {
    const controls = state.controls;
    const currentRun = state.run;
    if (
      controls === null
      || currentRun === null
      || state.controlPending !== null
      || controls.programRunId !== currentRun.capture.programRunId
      || controls.runRevision !== currentRun.runRevision
    ) return null;
    return {
      source_label: 'operator_ui',
      program_run_id: controls.programRunId,
      expected_run_revision: controls.runRevision,
    } satisfies Record<string, JsonValue>;
  }, [state.controlPending, state.controls, state.run]);

  /** Resolves to null when accepted, otherwise to the backend's reason. */
  const performRuntimeControl = useCallback(async (
    pending: string,
    commandKind: ExecutionControlCommandKind,
    payload: Record<string, JsonValue>,
    fallback: string,
  ): Promise<string | null> => {
    if (controlSource === null) return 'The execution control connection is unavailable.';
    beginControlCommand(pending);
    try {
      await commandEnd(await controlSource.sendExecutionCommand(commandKind, payload));
      // A recovery stays pending until the owner retires its offer.
      if (mounted.current && !pending.startsWith('recovery:')) {
        setState((current) => current.controlPending === pending
          ? { ...current, controlPending: null }
          : current);
      }
      return null;
    } catch (reason) {
      await recordCommandFailure(
        reason, fallback, null, 'control', pending,
      );
      return errorMessage(reason, fallback);
    }
  }, [beginControlCommand, commandEnd, controlSource, recordCommandFailure]);

  /** Applies one action of one offer (a failure hold's, or the focused operator Stop's), only while that exact revision is current. */
  const applyRecoveryAction = useCallback((
    target: Pick<CycleRecoveryOffer, 'offerId' | 'offerRevision'>,
    actionId: CycleRecoveryActionId,
  ) => {
    const current = offerWithId(state.controls, target.offerId);
    const offer = current !== null && current.offerRevision === target.offerRevision
      ? current
      : null;
    const action = offer?.actions.find((row) => row.actionId === actionId);
    if (
      offer === null || action?.enabled !== true
      || state.controlPending !== null
      // Retry and continue resume the run; only a Reset ends it.
      || (!action.performsBackendReset && composerRefusalReason !== null)
    ) return;
    const body = {
      source_label: 'operator_ui',
      offer_id: offer.offerId,
      offer_revision: offer.offerRevision,
      action_id: actionId,
      reason: 'operator-selected recovery action',
    } satisfies Record<string, JsonValue>;
    void performRuntimeControl(
      `recovery:${offer.offerId}:${actionId}`,
      'execution_recovery',
      body,
      'The recovery action was rejected.',
    );
  }, [composerRefusalReason, performRuntimeControl, state.controlPending, state.controls]);

  /** Resolves to null when written, otherwise to why the write was refused. */
  const writeRuntimeBlackboard = useCallback((
    key: string,
    value: JsonValue,
  ): Promise<string | null> => {
    const base = runtimeControlRequest();
    if (base === null) {
      return Promise.resolve(
        'Not written: the run changed or another control is pending. Review the current state and try again.',
      );
    }
    return performRuntimeControl(
      `blackboard:${key}`,
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
  const currentStart = useRef(applicationStartId);
  currentStart.current = applicationStartId;
  const persistentValues = useMemo(() => {
    const requestIdentity = (start: ApplicationStartId) => ({
      application_start_id: start,
      source_label: 'operator_ui',
    });
    // Answered at acceptance: the values are read again once the edit's row
    // ended succeeded (null); any other end throws its reason.
    const settled = async (row: CommandRow<RunnerCommandRowKind>): Promise<null> => {
      await awaitCommandSuccess(row, runnerRows);
      return null;
    };
    const startUnavailable = () => Promise.reject(
      new Error('Execution owner identity is unavailable.'),
    );
    return {
      read: (signal?: AbortSignal) => api.programs.persistentValues(signal),
      /** Set a stored value, or Reset it to `initial:` when value is omitted. */
      edit: (current: PersistentKeyValues, key: string, value?: JsonValue) => {
        const fence = persistentValuesFence(current, currentRun.current);
        if (fence === null) {
          return Promise.reject(new Error(
            'The run holding these values is not the run shown here. Refresh and try again.',
          ));
        }
        const start = currentStart.current;
        if (start === null) return startUnavailable();
        return api.programs.editPersistentValue({
          ...requestIdentity(start),
          ...fence,
          project_path: current.projectPath,
          key,
          expected_program_revision: current.programRevision,
          ...(value === undefined ? {} : { value }),
        }).then(settled);
      },
      clear: (current: PersistentKeyValues) => {
        const start = currentStart.current;
        if (start === null) return startUnavailable();
        return api.programs.clearPersistentValues(
          { ...requestIdentity(start), project_path: current.projectPath },
        ).then(settled);
      },
    };
  }, [runnerRows]);

  const stageManualResult = useCallback((
    outcome: 'succeeded' | 'failed',
    output: JsonValue,
  ) => {
    const offer = state.controls?.recoveryOffer;
    const base = runtimeControlRequest();
    if (base === null || offer === null || offer === undefined) return;
    void performRuntimeControl(
      `manual-result:${outcome}`,
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
  const resetRobotPositionAvailability = snapshot?.programCycle?.resetRobotPosition ?? null;
  const runnerAllows = useCallback((commandKind: RunnerCommandKind): boolean => (
    snapshot?.programCycle.allowedCommands.some(
      (command) => command.commandKind === commandKind && command.allowed,
    ) ?? false
  ), [snapshot]);
  /** The Runner's reason a command is currently not allowed, if any. */
  const runnerRefusal = useCallback((commandKind: RunnerCommandKind): string | null => (
    snapshot?.programCycle.allowedCommands.find(
      (command) => command.commandKind === commandKind,
    )?.disabledReason
    ?? null
  ), [snapshot]);
  /** Set while the Runner publishes a command in progress (from any panel). */
  const runnerCommandInProgress = snapshot?.programCycle.runnerCommands.inProgress ?? null;
  const runnerBusyReason: string | null = runnerCommandInProgress === null
    ? null : programCommandReason(runnerCommandInProgress);

  // Execute is the ordinary run request; the backend's readiness gates it.
  const run = ordinaryRun;
  // Blocked only by components not enabled: Execute offers to activate them first.
  const activation = useActivateExecute(cellState.components, readiness, ordinaryRun,
    // 0 before the first Stop; null while the control state is unknown.
    snapshot?.executionControlStatus == null
      ? null : snapshot.executionControlStatus.cellStopAction.lastEventId ?? 0,
    snapshot?.applicationStartId ?? null);

  return useMemo(() => ({
    state,
    applicationStartId,
    cycleState,
    cycleFailure,
    resetRobotPositionAvailability,
    runnerAllows,
    runnerRefusal,
    runnerBusyReason,
    readiness,
    cellLatch,
    composerRefusalReason,
    active,
    refresh,
    dismissError,
    run,
    activation,
    startMotion,
    stopMotion,
    resumeMotion,
    groupRuntimeScales,
    setGroupRuntimeScale,
    commitGroupRuntimeScale,
    stopAll,
    halt,
    resumeHalt,
    reset,
    applyRecoveryAction,
    writeRuntimeBlackboard,
    persistentValues,
    stageManualResult,
  }), [
    active,
    applicationStartId,
    applyRecoveryAction,
    dismissError,
    cycleFailure,
    cycleState,
    resetRobotPositionAvailability,
    runnerAllows,
    runnerRefusal,
    runnerBusyReason,
    readiness,
    cellLatch,
    composerRefusalReason,
    persistentValues,
    refresh,
    reset,
    run,
    activation,
    startMotion,
    stopMotion,
    resumeMotion,
    groupRuntimeScales,
    setGroupRuntimeScale,
    commitGroupRuntimeScale,
    stopAll,
    halt,
    resumeHalt,
    stageManualResult,
    state,
    writeRuntimeBlackboard,
  ]);
}

export type ProgramRuntimeController = ReturnType<typeof useProgramRuntime>;

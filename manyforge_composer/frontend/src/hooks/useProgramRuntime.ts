import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  AppliedProgramSelection,
  CurrentScenarioResponse,
  JsonValue,
  CycleRecoveryActionId,
  ProgramRuntimeControlProjection,
  ProgramCycleStatus,
  RuntimeOverridePromotionProposal,
  RuntimeOverrideRecord,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
  ScenarioCatalogResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import {
  browserStableIdentity,
  type ExecutionControlCommandKind,
  type HeadlessStateSource,
} from './useHeadlessState';

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
      .filter(([, status]) => status === 'running' || status === 'failure')
      .slice(0, 6)
      .map(([node, status]) => `${node}=${status}`);
  const recovery = controls?.recoveryOffer ?? null;
  return [
    run === null
      ? 'Observed run: none.'
      : [
        `Observed run ${run.capture.programRunId}: ${run.state}, revision ${run.runRevision}, tree ${run.treeStatus}, active operations ${run.activeOperationCount ?? 'unknown'}.`,
        run.reasonCode === null ? null : `Run reason: ${run.reasonCode}.`,
        run.message ? `Run message: ${run.message}.` : null,
        activeNodes.length === 0 ? null : `Current nodes: ${activeNodes.join(', ')}.`,
      ].filter((value): value is string => value !== null).join(' '),
    cycle === null
      ? 'Observed cycle: unavailable.'
      : `Observed cycle: ${cycle.state}, generation ${cycle.cycleGeneration}, run ${cycle.programRunId ?? 'none'}, claim ${cycle.cycleClaimGeneration ?? 'none'}, owner ${cycle.programRunnerOwnerEpoch}.`,
    recovery === null
      ? 'Observed recovery: none.'
      : [
        `Observed recovery ${recovery.recoveryClass}: ${recovery.status}, offer revision ${recovery.offerRevision}, node ${recovery.nodeInstanceId}, operation ${recovery.operationId}.`,
        `Recovery reason: ${recovery.reasonCode}.`,
        `Allowed actions: ${recovery.actions.filter((action) => action.enabled).map((action) => action.actionId).join(', ') || 'none'}.`,
      ].join(' '),
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
  | 'motion-stop'
  | 'resume-motion'
  | 'full-stop'
  | 'resume-full-stop'
  | 'stop'
  | 'reset'
  | null;

interface ProgramRuntimeState {
  catalog: ScenarioCatalogResponse | null;
  applied: AppliedProgramSelection | null;
  currentScenario: CurrentScenarioResponse | null;
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
  selectedScenarioId: string | null;
  runnerAvailable: boolean;
  loading: boolean;
  observationError: string | null;
  pending: ProgramPendingCommand;
  controlPending: string | null;
  error: string | null;
}

const INITIAL_STATE: ProgramRuntimeState = {
  catalog: null,
  applied: null,
  currentScenario: null,
  run: null,
  controls: null,
  selectedScenarioId: null,
  runnerAvailable: false,
  loading: true,
  observationError: null,
  pending: null,
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
        scenario.scenarioCatalogRevision,
        scenario.deploymentRevision,
        scenario.retainedBundleId,
        scenario.graphRevision,
        scenario.programRevision,
        scenario.programBindingId,
      ].join(':')}`;
}

function appliedKey(value: AppliedProgramSelection | null): string {
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
      + JSON.stringify([value.observationOverrideCandidate, value.executionSelections]);
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
  if (pending === 'motion-stop' || pending === 'resume-motion') {
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
): Record<string, JsonValue> | null {
  if (
    applied === null || !runnerAvailable || pending !== null
    || snapshot === null || !snapshot.authoringAdmission.authoringAllowed
    || snapshot.programCycle.state !== 'idle_after_reset'
    || workspace === null
    || workspace.draft.draftId !== snapshot.draftIdentity.draftId
    || workspace.draft.draftRevision !== snapshot.draftIdentity.draftRevision
    || !workspace.draft.programLoaded
    || workspace.draft.programRevision !== applied.programRevision
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: requestId,
    scenario_id: applied.scenarioId,
    scenario_catalog_revision: applied.scenarioCatalogRevision,
    expected_applied_record_digest: applied.recordDigest,
    draft_program_revision: workspace.draft.programRevision,
    expected_admission_generation: snapshot.authoringAdmission.admissionGeneration,
  };
}

export function buildProgramResetRequest(
  snapshot: ComposerHeadlessState | null,
  requestOwnerEpoch: string,
  requestId: string,
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
    correlation_id: requestId,
    program_run_id: cycle.programRunId,
    expected_owner_epoch: cycle.programRunnerOwnerEpoch,
    expected_cycle_generation: cycle.cycleGeneration,
    expected_claim_generation: cycle.cycleClaimGeneration,
    terminal_reference: cycle.terminalOrSettlementReference,
    settlement_evidence_digest: cycle.terminalOrSettlementReference,
  };
}

export function buildProgramStopRequest(
  snapshot: ComposerHeadlessState | null,
  requestOwnerEpoch: string,
  requestId: string,
): Record<string, JsonValue> | null {
  const cycle = snapshot?.programCycle;
  const allowed = cycle?.allowedCommands.some(
    (command) => command.commandKind === 'program_stop' && command.allowed,
  ) ?? false;
  if (
    cycle === undefined
    || !allowed
    || !['starting', 'running'].includes(cycle.state)
    || cycle.programRunId === null
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: requestId,
    program_run_id: cycle.programRunId,
    expected_owner_epoch: cycle.programRunnerOwnerEpoch,
    reason: 'ordinary operator Program Stop',
  };
}

export function retainedScenarioSelection(
  selectedScenarioId: string | null,
  catalog: ScenarioCatalogResponse | null,
): string | null {
  if (selectedScenarioId === null || catalog === null) return null;
  return catalog.scenarios.some((row) => row.scenarioId === selectedScenarioId)
    ? selectedScenarioId
    : null;
}

export function scenarioSelectionAfterRequest(
  selectedScenarioId: string | null,
  catalog: ScenarioCatalogResponse | null,
  requestedScenarioId: string | null,
): string | null {
  if (requestedScenarioId === null) return null;
  return catalog?.scenarios.some(
    (row) => row.scenarioId === requestedScenarioId,
  )
    ? requestedScenarioId
    : selectedScenarioId;
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

export function useProgramRuntime(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null = null,
  workspace: ProgramWorkspaceResponse | null = null,
  controlSource: HeadlessStateSource | null = null,
) {
  const [state, setState] = useState<ProgramRuntimeState>(INITIAL_STATE);
  const mounted = useRef(true);
  const pendingProgramRequest = useRef<string | null>(null);
  const refreshing = useRef(false);
  const refreshQueued = useRef(false);
  const observationRetryAttempt = useRef(0);
  const [observationRetrySequence, setObservationRetrySequence] = useState(0);
  const catalogCache = useRef<ScenarioCatalogResponse | null>(null);
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

      const observedCatalogRevision = currentScenario?.currentScenario
        ?.scenarioCatalogRevision ?? null;
      let catalog = catalogCache.current;
      if (
        catalog === null
        || (observedCatalogRevision !== null
          && catalog.scenarioCatalogRevision !== observedCatalogRevision)
      ) {
        try {
          catalog = await api.programs.scenarios();
          catalogCache.current = catalog;
        } catch (reason) {
          errors.push(errorMessage(reason, 'Scenario catalog unavailable.'));
        }
      }

      let applied: AppliedProgramSelection | null = null;
      if (appliedResult.status === 'fulfilled') {
        applied = appliedResult.value;
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
        const nextCatalog = catalog ?? current.catalog;
        const next = {
          ...current,
          catalog: nextCatalog,
          currentScenario,
          applied,
          // Owner-issued Applied state is a projection, not operator intent.
          // Refresh may retain an explicit selection but must never create one.
          selectedScenarioId: retainedScenarioSelection(
            current.selectedScenarioId,
            nextCatalog,
          ),
          runnerAvailable: true,
          loading: false,
          observationError,
          // Automatic owner refresh must not erase a command rejection before
          // the operator can see it. The next explicit command clears it.
          error: current.error,
        };
        return current.catalog?.scenarioCatalogRevision
            === next.catalog?.scenarioCatalogRevision
          && currentScenarioKey(current.currentScenario)
            === currentScenarioKey(next.currentScenario)
          && appliedKey(current.applied) === appliedKey(next.applied)
          && programRunProjectionKey(current.run) === programRunProjectionKey(next.run)
          && controlsKey(current.controls) === controlsKey(next.controls)
          && current.selectedScenarioId === next.selectedScenarioId
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
        && current.controlPending === controlPending
        && current.error === error
      ) return current;
      return {
        ...current,
        run: projected.run,
        controls: projected.controls,
        runnerAvailable: true,
        pending,
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
      catalogCache.current = null;
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

  const selectScenario = useCallback((scenarioId: string) => {
    setState((current) => ({
      ...current,
      selectedScenarioId: scenarioSelectionAfterRequest(
        current.selectedScenarioId,
        current.catalog,
        scenarioId,
      ),
    }));
  }, []);

  const clearScenarioSelection = useCallback(() => {
    setState((current) => ({
      ...current,
      selectedScenarioId: scenarioSelectionAfterRequest(
        current.selectedScenarioId,
        current.catalog,
        null,
      ),
    }));
  }, []);

  const dismissError = useCallback(() => {
    setState((current) => current.error === null
      ? current
      : { ...current, error: null });
  }, []);

  const beginProgramCommand = useCallback((
    pending: Exclude<ProgramPendingCommand, null>, requestId: string,
  ) => {
    pendingProgramRequest.current = requestId;
    setState((current) => ({
      ...current,
      pending,
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
      if (
        (pendingKind === 'program' && (current.pending !== expectedPending
          || pendingProgramRequest.current !== requestId))
        || (pendingKind === 'control'
          && current.controlPending !== expectedPending)
        || (expectedRun !== undefined && (
          current.run?.ownerEpoch !== expectedRun.ownerEpoch
          || current.run?.capture.programRunId !== expectedRun.capture.programRunId
        ))
      ) return current;
      return {
        ...current,
        pending: pendingKind === 'program' ? null : current.pending,
        controlPending: pendingKind === 'control' ? null : current.controlPending,
        error: [
          errorMessage(reason, fallback),
          `Request ${requestId}. Backend state remains authoritative.`,
        ].join(' '),
      };
    });
  }, []);

  const run = useCallback(async () => {
    const requestId = identity('program-run-start');
    const request = buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, requestId,
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
    beginProgramCommand, controlSource,
    recordCommandFailure, snapshot,
    state.applied, state.pending, state.runnerAvailable, workspace,
  ]);

  const reset = useCallback(async () => {
    const requestId = identity('program-cycle-reset');
    const request = buildProgramResetRequest(
      snapshot, callerEpoch.current, requestId,
    );
    if (request === null || controlSource === null) {
      if (mounted.current) setState((current) => ({
        ...current,
        error: request === null
          ? 'Program Reset was not sent because its backend permission or cycle identity changed. Controls were refreshed.'
          : 'Program Reset was not sent because the execution control connection is unavailable. Controls were refreshed.',
      }));
      await refresh();
      return;
    }
    beginProgramCommand('reset', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_reset', request, requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Reset was rejected.', requestId, 'program', 'reset',
      );
    }
  }, [
    beginProgramCommand, controlSource,
    recordCommandFailure, refresh, snapshot,
  ]);

  const stop = useCallback(async () => {
    if (!state.runnerAvailable || ['stop', 'full-stop', 'reset']
      .includes(state.pending ?? '')) return;
    const requestId = identity('program-run-stop');
    const request = buildProgramStopRequest(
      snapshot, callerEpoch.current, requestId,
    );
    if (controlSource === null || request === null) return;
    beginProgramCommand('stop', requestId);
    try {
      await controlSource.sendExecutionCommand(
        'program_stop', request, requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Stop was rejected.', requestId, 'program', 'stop',
      );
    }
  }, [
    beginProgramCommand, controlSource,
    recordCommandFailure,
    snapshot, state.pending, state.runnerAvailable,
  ]);

  const selectionCommand = useCallback(async (
    pending: 'motion-stop' | 'resume-motion', groupIds: string[],
  ) => {
    const resume = pending === 'resume-motion';
    const controls = state.controls;
    if (
      groupIds.length === 0
      || !state.run
      || !['running', 'intervention_required'].includes(state.run.state)
      || !state.runnerAvailable
      || ['stop', 'full-stop', 'reset']
        .includes(state.pending ?? '')
      || (resume && (state.pending !== null || state.controlPending !== null
        || !controlsMatchRun(controls, state.run)
        || !groupIds.every((id) => controls.executionSelections.groups.some(
          (group) => group.groupId === id && group.resumeAllowed,
        ))))
    ) return;
    const requestId = identity(`program-${pending}`);
    if (controlSource === null) return;
    beginProgramCommand(pending, requestId);
    try {
      await controlSource.sendExecutionCommand(
        resume ? 'program_motion_resume' : 'program_motion_stop',
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          group_ids: groupIds,
          ...(resume && controls !== null ? {
            expected_run_revision: controls.runRevision,
            expected_selection_revision: controls.executionSelections.selectionRevision,
          } : {}),
          reason: `operator ${pending}`,
        },
        requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Selection command was rejected.', requestId, 'program', pending, state.run,
      );
    } finally {
      // This clears only Sending feedback, never a hold or lifecycle state.
      // A newer priority Stop must not be cleared by an older Resume reply.
      if (mounted.current) setState((current) => current.pending === pending
        && pendingProgramRequest.current === requestId
        && current.run?.ownerEpoch === state.run?.ownerEpoch
        && current.run?.capture.programRunId === state.run?.capture.programRunId
        ? { ...current, pending: null } : current);
    }
  }, [
    beginProgramCommand, controlSource,
    recordCommandFailure,
    state.pending, state.controlPending, state.controls, state.run, state.runnerAvailable,
  ]);

  const stopMotion = useCallback((groups: string | string[] = 'all') => selectionCommand('motion-stop', typeof groups === 'string' ? [groups] : groups), [selectionCommand]);
  const resumeMotion = useCallback((groups: string | string[] = 'all') => selectionCommand('resume-motion', typeof groups === 'string' ? [groups] : groups), [selectionCommand]);

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
      !state.run
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
    beginProgramCommand, controlSource,
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

  const performRuntimeControl = useCallback(async (
    pending: string,
    requestId: string,
    commandKind: ExecutionControlCommandKind,
    payload: Record<string, JsonValue>,
    fallback: string,
  ) => {
    if (controlSource === null) return;
    beginControlCommand(pending);
    try {
      await controlSource.sendExecutionCommand(
        commandKind, payload, requestId,
      );
    } catch (reason) {
      await recordCommandFailure(
        reason, fallback, requestId, 'control', pending,
      );
    }
  }, [beginControlCommand, controlSource, recordCommandFailure]);

  const applyRecoveryAction = useCallback((actionId: CycleRecoveryActionId) => {
    const offer = state.controls?.recoveryOffer;
    const action = offer?.actions.find((row) => row.actionId === actionId);
    if (
      offer === null || offer === undefined || action?.enabled !== true
      || state.controlPending !== null
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
  }, [performRuntimeControl, state.controlPending, state.controls]);

  const stageObservationOverride = useCallback(() => {
    const candidate = state.controls?.observationOverrideCandidate;
    const requestId = identity('program-runtime-override-stage');
    const base = runtimeControlRequest(requestId);
    if (
      base === null
      || candidate?.available !== true
      || candidate.nodeInstanceId === null
      || candidate.nodeAttemptId === null
      || candidate.parameterId === null
      || candidate.value === null
      || candidate.sourceObservationId === null
      || candidate.sourceOwnerGeneration === null
    ) return;
    void performRuntimeControl(
      'override:stage-observation',
      requestId,
      'program_runtime_override_stage',
      {
        ...base,
        node_instance_id: candidate.nodeInstanceId,
        node_attempt_id: candidate.nodeAttemptId,
        parameter_id: candidate.parameterId,
        value: candidate.value,
        source_observation_id: candidate.sourceObservationId,
        source_owner_generation: candidate.sourceOwnerGeneration,
      },
      'The Robot snapshot could not be staged.',
    );
  }, [performRuntimeControl, runtimeControlRequest, state.controls]);

  const runtimeOverrideAction = useCallback((
    action: 'activate' | 'clear' | 'promote',
    override: RuntimeOverrideRecord,
  ) => {
    const requestId = identity(`program-runtime-override-${action}`);
    const base = runtimeControlRequest(requestId);
    if (base === null || (action === 'promote' && workspace === null)) return;
    const commandKind: ExecutionControlCommandKind = action === 'activate'
      ? 'program_runtime_override_activate'
      : action === 'clear'
        ? 'program_runtime_override_clear'
        : 'program_runtime_override_promote';
    const body: Record<string, JsonValue> = {
      ...base,
      override_id: override.overrideId,
      expected_override_revision: override.overrideRevision,
    };
    if (action === 'promote') {
      body.expected_draft_id = workspace!.draft.draftId;
      body.expected_draft_revision = workspace!.draft.draftRevision;
    }
    void performRuntimeControl(
      `override:${action}:${override.overrideId}`,
      requestId,
      commandKind,
      body,
      action === 'activate'
        ? 'The runtime override could not be activated.'
        : action === 'clear'
          ? 'The runtime override could not be cleared.'
          : 'The runtime override promotion could not be requested.',
    );
  }, [performRuntimeControl, runtimeControlRequest, workspace]);

  const writeRuntimeBlackboard = useCallback((key: string, value: JsonValue) => {
    const requestId = identity('program-runtime-blackboard');
    const base = runtimeControlRequest(requestId);
    if (base === null) return;
    void performRuntimeControl(
      `blackboard:${key}`,
      requestId,
      'program_runtime_blackboard_write',
      { ...base, key, value },
      'The runtime blackboard value was rejected.',
    );
  }, [performRuntimeControl, runtimeControlRequest]);

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

  const materializePromotion = useCallback(async (
    proposal: RuntimeOverridePromotionProposal,
  ) => {
    if (
      proposal.status !== 'ready'
      || workspace === null
      || snapshot === null
      || state.controlPending !== null
      || proposal.expectedDraftId !== workspace.draft.draftId
      || proposal.expectedDraftRevision !== workspace.draft.draftRevision
      || snapshot.draftIdentity.draftId !== workspace.draft.draftId
      || snapshot.draftIdentity.draftRevision !== workspace.draft.draftRevision
      || snapshot.programCycle.state !== 'idle_after_reset'
      || !snapshot.authoringAdmission.authoringAllowed
    ) return;
    const requestId = identity('program-runtime-promotion-materialize');
    setState((current) => ({
      ...current,
      controlPending: `promotion:materialize:${proposal.proposalId}`,
      error: null,
    }));
    try {
      await api.programWorkspace.materializeRuntimeOverride(
        {
          draftId: workspace.draft.draftId,
          expectedDraftRevision: workspace.draft.draftRevision,
          expectedAdmissionGeneration: snapshot.authoringAdmission.admissionGeneration,
          requestOwnerEpoch: callerEpoch.current,
          requestId,
        },
        proposal.programRunId,
        proposal.proposalId,
      );
      await refresh();
      if (mounted.current) setState((current) => ({
        ...current,
        controlPending: null,
      }));
    } catch (reason) {
      if (mounted.current) setState((current) => ({
        ...current,
        controlPending: null,
        error: errorMessage(
          reason,
          'The runtime override could not be applied to the Draft.',
        ),
      }));
    }
  }, [refresh, snapshot, state.controlPending, workspace]);

  const promotionMaterializationReason = useCallback((
    proposal: RuntimeOverridePromotionProposal,
  ): string | null => {
    if (proposal.status !== 'ready') return 'Reset the Program cycle first.';
    if (workspace === null || snapshot === null) {
      return 'The current Draft and backend admission are unavailable.';
    }
    if (
      proposal.expectedDraftId !== workspace.draft.draftId
      || proposal.expectedDraftRevision !== workspace.draft.draftRevision
    ) return 'The Draft changed after this promotion was requested.';
    if (
      snapshot.draftIdentity.draftId !== workspace.draft.draftId
      || snapshot.draftIdentity.draftRevision !== workspace.draft.draftRevision
    ) return 'The displayed Draft is still synchronizing.';
    if (
      snapshot.programCycle.state !== 'idle_after_reset'
      || !snapshot.authoringAdmission.authoringAllowed
    ) return 'Applying the promotion to Draft requires authoritative Idle.';
    return state.controlPending === null ? null : 'Another runtime control is pending.';
  }, [snapshot, state.controlPending, workspace]);

  const active = useMemo(
    () => state.run !== null && CURRENT_STATES.has(state.run.state),
    [state.run],
  );
  const cycleState = snapshot?.programCycle?.state ?? null;
  const cycleFailure = snapshot?.programCycle?.failure ?? null;
  const commandAllowed = useCallback((commandKind: string): boolean => (
    (commandKind !== 'program_run' || buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, 'availability-check',
    ) !== null)
    && (snapshot?.programCycle.allowedCommands.some(
      (command) => command.commandKind === commandKind && command.allowed,
    ) ?? false)
  ), [snapshot, state.applied, state.pending, state.runnerAvailable, workspace]);

  return useMemo(() => ({
    state,
    cycleState,
    cycleFailure,
    commandAllowed,
    active,
    refresh,
    selectScenario,
    clearScenarioSelection,
    dismissError,
    run,
    stopMotion,
    resumeMotion,
    fullStop,
    resumeFullStop,
    stop,
    reset,
    applyRecoveryAction,
    stageObservationOverride,
    activateRuntimeOverride: (override: RuntimeOverrideRecord) => (
      runtimeOverrideAction('activate', override)
    ),
    clearRuntimeOverride: (override: RuntimeOverrideRecord) => (
      runtimeOverrideAction('clear', override)
    ),
    promoteRuntimeOverride: (override: RuntimeOverrideRecord) => (
      runtimeOverrideAction('promote', override)
    ),
    materializePromotion,
    promotionMaterializationReason,
    writeRuntimeBlackboard,
    stageManualResult,
  }), [
    active,
    applyRecoveryAction,
    clearScenarioSelection,
    dismissError,
    cycleFailure,
    cycleState,
    commandAllowed,
    refresh,
    reset,
    run,
    stopMotion,
    resumeMotion,
    fullStop,
    resumeFullStop,
    selectScenario,
    stageManualResult,
    stageObservationOverride,
    state,
    stop,
    materializePromotion,
    promotionMaterializationReason,
    runtimeOverrideAction,
    writeRuntimeBlackboard,
  ]);
}

export type ProgramRuntimeController = ReturnType<typeof useProgramRuntime>;

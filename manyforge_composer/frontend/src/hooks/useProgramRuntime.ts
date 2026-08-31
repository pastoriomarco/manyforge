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
import { browserStableIdentity } from './useHeadlessState';

const CURRENT_STATES = new Set([
  'running',
  'intervention_required',
  'full_stopping',
  'full_stopped',
  'stopping',
]);
const POLLED_STATES = new Set([
  'running',
  'intervention_required',
  'full_stopping',
  'stopping',
]);
export const PROGRAM_RUN_OBSERVATION_POLL_MS = 100;

type ProgramCycleDebugProjection = Pick<ProgramCycleStatus,
  | 'programRunnerOwnerEpoch'
  | 'cycleGeneration'
  | 'state'
  | 'programRunId'
  | 'cycleClaimGeneration'
>;

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

export function programCommandFailureMessage(
  reason: unknown,
  fallback: string,
  requestId: string,
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
  snapshot: ComposerHeadlessState | null,
): string {
  const cycle = snapshot?.programCycle ?? null;
  return [
    errorMessage(reason, fallback),
    `Request ${requestId}.`,
    programCurrentStatusSummary(run, controls, cycle),
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

export interface ProgramCommandFailureDiagnostic {
  message: string;
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
}

export async function observeProgramCommandFailure(
  reason: unknown,
  fallback: string,
  requestId: string,
  fallbackRun: ProgramRunStatus | null,
  fallbackControls: ProgramRuntimeControlProjection | null,
  fallbackCycle: ProgramCycleDebugProjection | null,
): Promise<ProgramCommandFailureDiagnostic> {
  const observationErrors: string[] = [];
  let cycle: ProgramCycleStatus | null = null;
  try {
    cycle = await api.programs.cycleStatus();
  } catch (observationError) {
    observationErrors.push(errorMessage(
      observationError,
      'Fresh Program-cycle status unavailable.',
    ));
  }
  const programRunId = cycle?.programRunId
    ?? fallbackRun?.capture.programRunId
    ?? null;
  let run = fallbackRun;
  let controls = fallbackControls;
  if (programRunId !== null) {
    const [runResult, controlsResult] = await Promise.allSettled([
      api.programs.read(programRunId),
      api.programs.runtimeControls(programRunId),
    ]);
    if (runResult.status === 'fulfilled') run = runResult.value;
    else observationErrors.push(errorMessage(
      runResult.reason,
      'Fresh Program-run status unavailable.',
    ));
    if (controlsResult.status === 'fulfilled') controls = controlsResult.value;
    else observationErrors.push(errorMessage(
      controlsResult.reason,
      'Fresh Program recovery status unavailable.',
    ));
  }
  return {
    run,
    controls,
    message: [
      errorMessage(reason, fallback),
      `Request ${requestId}.`,
      programCurrentStatusSummary(run, controls, cycle ?? fallbackCycle),
      observationErrors.length === 0
        ? 'Current status was re-read after the failed request.'
        : `Current-status read was incomplete: ${[...new Set(observationErrors)].join(' ')}`,
    ].join(' '),
  };
}

export function programRuntimeWakeKey(snapshot: ComposerHeadlessState | null): string {
  if (snapshot === null) return 'unavailable';
  const manager = snapshot.runtime.sections.manager;
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
    snapshot.authoringAdmission.admissionGeneration,
    snapshot.programCycle.programRunnerOwnerEpoch,
    snapshot.programCycle.cycleGeneration,
    snapshot.programCycle.state,
    snapshot.programCycle.runtimeControlRevision ?? 'no-runtime-controls',
    snapshot.programCycle.recoveryOffer?.offerId ?? 'no-recovery-offer',
    snapshot.programCycle.recoveryOffer?.offerRevision ?? 0,
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

function runKey(value: ProgramRunStatus | null): string {
  return value === null
    ? 'none'
    : `${value.ownerEpoch}:${value.capture.programRunId}:${value.runRevision}`;
}

function controlsKey(value: ProgramRuntimeControlProjection | null): string {
  return value === null
    ? 'none'
    : `${value.ownerEpoch}:${value.programRunId}:${value.runRevision}:`
      + `${value.runtimeControls.runtimeControlRevision}:`
      + `${value.recoveryOffer?.offerId ?? 'no-offer'}:`
      + `${value.recoveryOffer?.offerRevision ?? 0}:`
      + JSON.stringify(value.observationOverrideCandidate);
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

/**
 * True only after the exact Program run publishes its certified ordinary
 * motion-Stop recovery boundary. The command response means Stop was
 * accepted; this owner projection is what proves motion has settled.
 */
export function programOrdinaryMotionStopSettled(
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
  const offer = controls.recoveryOffer;
  return offer !== null
    && offer.programRunId === run.capture.programRunId
    && offer.recoveryClass === 'ordinary_operator_stop'
    && offer.status === 'retry_ready'
    && offer.stopId !== null
    && offer.stopGeneration !== null
    && offer.stopGeneration > 0
    && offer.stopCertificateId !== null
    && offer.stopCertificateRevision !== null
    && offer.stopCertificateRevision > 0;
}

/**
 * Physical motion settlement may retain either the ordinary retry-ready
 * recovery or a stronger outer Stop fault that permits Reset only. Both are
 * Stopped to the operator; only the former may expose Resume.
 */
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
  const offer = controls.recoveryOffer;
  return offer !== null
    && offer.programRunId === run.capture.programRunId
    && ['ordinary_operator_stop', 'stop_fault_intervention'].includes(
      offer.recoveryClass,
    )
    && ['retry_ready', 'reset_required'].includes(offer.status)
    && offer.stopId !== null
    && offer.stopGeneration !== null
    && offer.stopGeneration > 0
    && offer.stopCertificateId !== null
    && offer.stopCertificateRevision !== null
    && offer.stopCertificateRevision > 0;
}

/** Keep an accepted motion Stop pending until owner evidence proves settlement. */
export function programPendingAfterProjection(
  pending: ProgramPendingCommand,
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
  cycle: ProgramCycleDebugProjection | null = null,
): ProgramPendingCommand {
  if (pending === 'motion-stop') {
    if (programMotionStopSettled(run, controls)) return null;
    if (
      run !== null
      && ['full_stopped', 'stopped', 'succeeded', 'failed'].includes(run.state)
    ) return null;
    return pending;
  }
  if (pending === 'run') {
    return run !== null
      && run.state === 'running'
      && cycle?.state === 'running'
      && cycle.programRunId === run.capture.programRunId
      ? null
      : pending;
  }
  if (pending === 'reset') {
    return cycle?.state === 'idle_after_reset' ? null : pending;
  }
  if (pending === 'stop') {
    return run !== null
      && ['stopped', 'succeeded', 'failed'].includes(run.state)
      ? null
      : pending;
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

export function programRunNeedsObservationPoll(
  run: ProgramRunStatus | null,
  controls: ProgramRuntimeControlProjection | null,
): boolean {
  return run !== null
    && POLLED_STATES.has(run.state)
    && !programMotionStopSettled(run, controls);
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
  if (
    cycle === undefined
    || cycle.state !== 'terminal_awaiting_reset'
    || cycle.programRunId === null || cycle.cycleClaimGeneration === null
    || cycle.terminalOrSettlementReference === null
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: null,
    program_run_id: cycle.programRunId,
    expected_owner_epoch: cycle.programRunnerOwnerEpoch,
    expected_cycle_generation: cycle.cycleGeneration,
    expected_claim_generation: cycle.cycleClaimGeneration,
    terminal_reference: cycle.terminalOrSettlementReference,
    settlement_evidence_digest: cycle.terminalOrSettlementReference,
  };
}

export function buildProgramFullStopResetRequest(
  snapshot: ComposerHeadlessState | null,
  run: ProgramRunStatus | null,
  requestOwnerEpoch: string,
  requestId: string,
): Record<string, JsonValue> | null {
  const cycle = snapshot?.programCycle;
  if (
    cycle === undefined
    || cycle.state !== 'running'
    || cycle.programRunId === null
    || cycle.cycleClaimGeneration === null
    || run === null
    || run.state !== 'full_stopped'
    || run.capture.programRunId !== cycle.programRunId
    || run.ownerEpoch !== cycle.programRunnerOwnerEpoch
  ) return null;
  return {
    request_id: requestId,
    request_owner_epoch: requestOwnerEpoch,
    source_label: 'operator_ui',
    correlation_id: requestId,
    program_run_id: run.capture.programRunId,
    expected_owner_epoch: run.ownerEpoch,
    expected_run_revision: run.runRevision,
    expected_cycle_generation: cycle.cycleGeneration,
    expected_claim_generation: cycle.cycleClaimGeneration,
    reason: 'operator Reset discarded Full Stop',
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

export function useProgramRuntime(
  enabled: boolean,
  snapshot: ComposerHeadlessState | null = null,
  workspace: ProgramWorkspaceResponse | null = null,
) {
  const [state, setState] = useState<ProgramRuntimeState>(INITIAL_STATE);
  const mounted = useRef(true);
  const refreshing = useRef(false);
  const refreshQueued = useRef(false);
  const commandSequence = useRef(0);
  const catalogCache = useRef<ScenarioCatalogResponse | null>(null);
  const requestedWakeKey = useRef<string | null>(null);
  const observedRunId = useRef<string | null>(null);
  const observedOwnerEpoch = useRef<string | null>(null);
  const callerEpoch = useRef(browserStableIdentity(
    'manyforge.composer.program_request_owner_epoch',
    'composer-program-owner',
  ));
  const wakeKey = programRuntimeWakeKey(snapshot);
  const snapshotAvailable = snapshot !== null;
  const cycleRunId = snapshot?.programCycle.programRunId ?? null;
  const cycleDebugProjection = useMemo<ProgramCycleDebugProjection | null>(
    () => snapshot === null ? null : {
      programRunnerOwnerEpoch: snapshot.programCycle.programRunnerOwnerEpoch,
      cycleGeneration: snapshot.programCycle.cycleGeneration,
      state: snapshot.programCycle.state,
      programRunId: snapshot.programCycle.programRunId,
      cycleClaimGeneration: snapshot.programCycle.cycleClaimGeneration,
    },
    [
      snapshot?.programCycle.cycleClaimGeneration,
      snapshot?.programCycle.cycleGeneration,
      snapshot?.programCycle.programRunId,
      snapshot?.programCycle.programRunnerOwnerEpoch,
      snapshot?.programCycle.state,
    ],
  );
  const recoverySettlementPending = state.controlPending?.startsWith('recovery:') === true;
  const activeRunId = recoverySettlementPending
    || programRunNeedsObservationPoll(state.run, state.controls)
    ? state.run?.capture.programRunId ?? null
    : null;
  const activeRunOwnerEpoch = activeRunId === null ? null : state.run?.ownerEpoch ?? null;

  const refresh = useCallback(async () => {
    if (!enabled || !snapshotAvailable) return;
    if (refreshing.current) {
      refreshQueued.current = true;
      return;
    }
    refreshing.current = true;
    const refreshCommandSequence = commandSequence.current;
    try {
      const [currentScenarioResult, appliedResult, indexResult] = await Promise.allSettled([
        api.programs.currentScenario(),
        api.programs.applied(),
        api.programs.runs(true, 1),
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

      let run: ProgramRunStatus | null = null;
      let controls: ProgramRuntimeControlProjection | null = null;
      const runnerAvailable = indexResult.status === 'fulfilled';
      if (indexResult.status === 'fulfilled') {
        const index = indexResult.value;
        if (
          observedOwnerEpoch.current !== null
          && observedOwnerEpoch.current !== index.ownerEpoch
        ) {
          observedRunId.current = null;
        }
        observedOwnerEpoch.current = index.ownerEpoch;
        run = index.runs[0] ?? null;
        if (run !== null) {
          observedRunId.current = run.capture.programRunId;
        } else {
          const retainedRunId = cycleRunId ?? observedRunId.current;
          if (retainedRunId === null) {
            run = null;
          } else {
            try {
              run = await api.programs.read(retainedRunId);
              observedRunId.current = retainedRunId;
            } catch (reason) {
              if (!(reason instanceof ApiError && reason.status === 404)) {
                errors.push(errorMessage(
                  reason,
                  'Program-run status unavailable.',
                ));
              }
            }
          }
        }
      } else {
        errors.push(errorMessage(
          indexResult.reason,
          'Program runner unavailable.',
        ));
      }
      if (run !== null) {
        try {
          controls = await api.programs.runtimeControls(
            run.capture.programRunId,
          );
        } catch (reason) {
          errors.push(errorMessage(
            reason,
            'Program recovery and runtime controls are unavailable.',
          ));
        }
      }
      if (!mounted.current) return;
      if (refreshCommandSequence !== commandSequence.current) {
        refreshQueued.current = true;
        return;
      }

      setState((current) => {
        const nextCatalog = catalog ?? current.catalog;
        const controlPending = controlPendingAfterProjection(
          current.controlPending,
          current.controls,
          controls,
          run,
        );
        const next = {
          ...current,
          catalog: nextCatalog,
          currentScenario,
          applied,
          run,
          controls,
          // Owner-issued Applied state is a projection, not operator intent.
          // Refresh may retain an explicit selection but must never create one.
          selectedScenarioId: retainedScenarioSelection(
            current.selectedScenarioId,
            nextCatalog,
          ),
          runnerAvailable,
          loading: false,
          // Most command markers are completed by their own response. Motion
          // Stop is deliberately different: acceptance starts containment,
          // while only the exact owner recovery projection proves Stopped.
          pending: programPendingAfterProjection(
            current.pending,
            run,
            controls,
            cycleDebugProjection,
          ),
          controlPending,
          // Automatic owner refresh must not erase a command rejection before
          // the operator can see it. The next explicit command clears it.
          error: errors.length > 0
            ? [...new Set(errors)].join(' ')
            : current.error ?? (
              cycleDebugProjection?.state !== 'idle_after_reset'
                ? programTerminalFailureMessage(
                  run,
                  controls,
                  cycleDebugProjection,
                )
                : null
            ),
        };
        return current.catalog?.scenarioCatalogRevision
            === next.catalog?.scenarioCatalogRevision
          && currentScenarioKey(current.currentScenario)
            === currentScenarioKey(next.currentScenario)
          && appliedKey(current.applied) === appliedKey(next.applied)
          && runKey(current.run) === runKey(next.run)
          && controlsKey(current.controls) === controlsKey(next.controls)
          && current.selectedScenarioId === next.selectedScenarioId
          && current.runnerAvailable === next.runnerAvailable
          && current.loading === next.loading
          && current.pending === next.pending
          && current.controlPending === next.controlPending
          && current.error === next.error
          ? current
          : next;
      });
    } finally {
      refreshing.current = false;
      if (refreshQueued.current) {
        refreshQueued.current = false;
        queueMicrotask(() => { void refresh(); });
      }
    }
  }, [cycleDebugProjection, cycleRunId, enabled, snapshotAvailable]);

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
      observedRunId.current = null;
      observedOwnerEpoch.current = null;
      catalogCache.current = null;
      requestedWakeKey.current = null;
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
    if (!enabled || activeRunId === null || activeRunOwnerEpoch === null) {
      return undefined;
    }
    let current = true;
    let timer: number | null = null;
    let controller: AbortController | null = null;
    const poll = () => {
      if (!current) return;
      controller = new AbortController();
      void Promise.allSettled([
        api.programs.read(activeRunId, controller.signal),
        api.programs.runtimeControls(activeRunId, controller.signal),
      ])
        .then(([runResult, controlsResult]) => {
          if (runResult.status !== 'fulfilled') throw runResult.reason;
          const run = runResult.value;
          if (!current || run.ownerEpoch !== activeRunOwnerEpoch) return;
          setState((previous) => {
            const observed = previous.run;
            const controls = controlsResult.status === 'fulfilled'
              && controlsResult.value.ownerEpoch === activeRunOwnerEpoch
              ? controlsResult.value
              : previous.controls;
            const pending = programPendingAfterProjection(
              previous.pending,
              run,
              controls,
              cycleDebugProjection,
            );
            const controlPending = controlPendingAfterProjection(
              previous.controlPending,
              previous.controls,
              controls,
              run,
            );
            if (
              observed === null
              || observed.capture.programRunId !== activeRunId
              || observed.ownerEpoch !== activeRunOwnerEpoch
              || (
                observed.runRevision === run.runRevision
                && observed.nodeStatusRevision === run.nodeStatusRevision
                && controlsKey(controls) === controlsKey(previous.controls)
                && pending === previous.pending
                && controlPending === previous.controlPending
              )
            ) return previous;
            return {
              ...previous,
              run,
              controls,
              pending,
              controlPending,
              error: previous.error ?? programTerminalFailureMessage(
                run,
                controls,
                cycleDebugProjection,
              ),
            };
          });
          const controls = controlsResult.status === 'fulfilled'
            ? controlsResult.value
            : null;
          if (
            recoverySettlementPending
            || programRunNeedsObservationPoll(run, controls)
          ) {
            timer = window.setTimeout(poll, PROGRAM_RUN_OBSERVATION_POLL_MS);
          }
        })
        .catch(() => {
          if (current && !controller?.signal.aborted) {
            timer = window.setTimeout(poll, PROGRAM_RUN_OBSERVATION_POLL_MS);
          }
        });
    };
    timer = window.setTimeout(poll, 0);
    return () => {
      current = false;
      if (timer !== null) window.clearTimeout(timer);
      controller?.abort();
    };
  }, [
    activeRunId, activeRunOwnerEpoch, cycleDebugProjection, enabled,
    recoverySettlementPending,
  ]);

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

  const beginProgramCommand = useCallback((pending: Exclude<ProgramPendingCommand, null>) => {
    commandSequence.current += 1;
    setState((current) => ({ ...current, pending, error: null }));
  }, []);

  const beginControlCommand = useCallback((pending: string) => {
    commandSequence.current += 1;
    setState((current) => ({ ...current, controlPending: pending, error: null }));
  }, []);

  const recordCommandFailure = useCallback(async (
    reason: unknown,
    fallback: string,
    requestId: string,
    pendingKind: 'program' | 'control',
  ) => {
    const diagnostic = await observeProgramCommandFailure(
      reason,
      fallback,
      requestId,
      state.run,
      state.controls,
      cycleDebugProjection,
    );
    if (!mounted.current) return;
    const diagnosticControls = diagnostic.run !== null
      && diagnostic.controls?.ownerEpoch === diagnostic.run.ownerEpoch
      && diagnostic.controls.programRunId === diagnostic.run.capture.programRunId
      ? diagnostic.controls
      : null;
    setState((current) => ({
      ...current,
      run: diagnostic.run,
      controls: diagnosticControls,
      pending: pendingKind === 'program' ? null : current.pending,
      controlPending: pendingKind === 'control' ? null : current.controlPending,
      error: diagnostic.message,
    }));
  }, [cycleDebugProjection, state.controls, state.run]);

  const run = useCallback(async () => {
    const requestId = identity('program-run-start');
    const request = buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, requestId,
    );
    if (request === null) return;
    beginProgramCommand('run');
    try {
      const status = await api.programs.start(request);
      observedRunId.current = status.capture.programRunId;
      observedOwnerEpoch.current = status.ownerEpoch;
      if (mounted.current) {
        setState((current) => ({
          ...current,
          run: status,
          controls: null,
          pending: programPendingAfterProjection(
            current.pending,
            status,
            null,
            cycleDebugProjection,
          ),
        }));
      }
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Run was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, cycleDebugProjection, recordCommandFailure, snapshot,
    state.applied, state.pending, state.runnerAvailable, workspace,
  ]);

  const reset = useCallback(async () => {
    const requestId = identity('program-cycle-reset');
    const fullStopRequest = buildProgramFullStopResetRequest(
      snapshot, state.run, callerEpoch.current, requestId,
    );
    const request = fullStopRequest ?? buildProgramResetRequest(
      snapshot, callerEpoch.current, requestId,
    );
    if (request === null) return;
    beginProgramCommand('reset');
    try {
      if (fullStopRequest !== null && state.run !== null) {
        await api.programs.resetFullStop(
          state.run.capture.programRunId,
          fullStopRequest,
        );
      } else {
        await api.programs.resetCycle(request);
      }
      await refresh();
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Reset was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, recordCommandFailure, refresh, snapshot, state.run,
  ]);

  const stop = useCallback(async () => {
    if (
      !state.run
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    const requestId = identity('program-run-stop');
    beginProgramCommand('stop');
    try {
      const status = await api.programs.stop(
        state.run.capture.programRunId,
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          reason: 'ordinary operator Program Stop',
        },
      );
      if (mounted.current) {
        setState((current) => ({
          ...current,
          run: status,
          pending: programPendingAfterProjection(
            current.pending,
            status,
            current.controls,
            cycleDebugProjection,
          ),
        }));
      }
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Program Stop was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, cycleDebugProjection, recordCommandFailure,
    state.pending, state.run, state.runnerAvailable,
  ]);

  const stopMotion = useCallback(async () => {
    if (
      !state.run
      || state.run.state !== 'running'
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    const requestId = identity('program-motion-stop');
    beginProgramCommand('motion-stop');
    try {
      const status = await api.programs.stopMotion(
        state.run.capture.programRunId,
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          reason: 'ordinary operator motion Stop',
        },
      );
      if (mounted.current) {
        setState((current) => ({
          ...current,
          run: status,
          pending: programPendingAfterProjection(
            current.pending,
            status,
            current.controls,
            cycleDebugProjection,
          ),
        }));
      }
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Motion Stop was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, cycleDebugProjection, recordCommandFailure,
    state.pending, state.run, state.runnerAvailable,
  ]);

  const fullStop = useCallback(async () => {
    if (
      !state.run
      || !['running', 'intervention_required'].includes(state.run.state)
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    const requestId = identity('program-full-stop');
    beginProgramCommand('full-stop');
    try {
      const status = await api.programs.fullStop(
        state.run.capture.programRunId,
        {
          request_id: requestId,
          request_owner_epoch: callerEpoch.current,
          source_label: 'operator_ui',
          correlation_id: requestId,
          program_run_id: state.run.capture.programRunId,
          expected_owner_epoch: state.run.ownerEpoch,
          reason: 'ordinary operator Full Stop',
        },
      );
      if (mounted.current) {
        setState((current) => ({
          ...current,
          run: status,
          pending: programPendingAfterProjection(
            current.pending,
            status,
            current.controls,
            cycleDebugProjection,
          ),
        }));
      }
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Full Stop was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, cycleDebugProjection, recordCommandFailure,
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
    beginProgramCommand('resume-full-stop');
    try {
      const status = await api.programs.resumeFullStop(
        state.run.capture.programRunId,
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
      );
      if (mounted.current) {
        setState((current) => ({
          ...current,
          run: status,
          pending: programPendingAfterProjection(
            current.pending,
            status,
            current.controls,
            cycleDebugProjection,
          ),
        }));
      }
    } catch (reason) {
      await recordCommandFailure(
        reason, 'Run after Full Stop was rejected.', requestId, 'program',
      );
    }
  }, [
    beginProgramCommand, cycleDebugProjection, recordCommandFailure,
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
    effect: () => Promise<{
      run: ProgramRunStatus;
      runtimeControls: ProgramRuntimeControlProjection;
    }>,
    fallback: string,
  ) => {
    beginControlCommand(pending);
    try {
      const result = await effect();
      if (mounted.current) setState((current) => ({
        ...current,
        run: result.run,
        controls: result.runtimeControls,
        controlPending: controlPendingAfterProjection(
          current.controlPending,
          current.controls,
          result.runtimeControls,
          result.run,
        ),
      }));
    } catch (reason) {
      await recordCommandFailure(reason, fallback, requestId, 'control');
    }
  }, [beginControlCommand, recordCommandFailure]);

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
      () => api.programs.applyRecoveryAction(body),
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
      () => api.programs.runtimeControlAction(
        base.program_run_id as string,
        'runtime-overrides',
        {
          ...base,
          node_instance_id: candidate.nodeInstanceId,
          node_attempt_id: candidate.nodeAttemptId,
          parameter_id: candidate.parameterId,
          value: candidate.value,
          source_observation_id: candidate.sourceObservationId,
          source_owner_generation: candidate.sourceOwnerGeneration,
        },
      ),
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
    const path = action === 'activate'
      ? 'runtime-overrides/activate'
      : action === 'clear'
        ? 'runtime-overrides/clear'
        : 'runtime-overrides/promote';
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
      () => api.programs.runtimeControlAction(
        base.program_run_id as string,
        path,
        body,
      ),
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
      () => api.programs.runtimeControlAction(
        base.program_run_id as string,
        'blackboard-values',
        { ...base, key, value },
      ),
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
      () => api.programs.runtimeControlAction(
        base.program_run_id as string,
        'manual-result',
        {
          ...base,
          node_instance_id: offer.nodeInstanceId,
          node_attempt_id: offer.nodeAttemptId,
          outcome,
          output,
        },
      ),
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

  return useMemo(() => ({
    state,
    cycleState,
    active,
    refresh,
    selectScenario,
    clearScenarioSelection,
    dismissError,
    run,
    stopMotion,
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
    cycleState,
    refresh,
    reset,
    run,
    stopMotion,
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

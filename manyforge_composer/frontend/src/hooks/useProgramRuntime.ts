import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  AppliedProgramSelection,
  CurrentScenarioResponse,
  JsonValue,
  ProgramRunStatus,
  ProgramWorkspaceResponse,
  ScenarioCatalogResponse,
} from '../types/api';
import type { ComposerHeadlessState } from '../types/headless';
import { browserStableIdentity } from './useHeadlessState';

const ACTIVE_STATES = new Set([
  'running',
  'intervention_required',
  'stopping',
]);

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
  ].join(':');
}

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

interface ProgramRuntimeState {
  catalog: ScenarioCatalogResponse | null;
  applied: AppliedProgramSelection | null;
  currentScenario: CurrentScenarioResponse | null;
  run: ProgramRunStatus | null;
  selectedScenarioId: string | null;
  runnerAvailable: boolean;
  loading: boolean;
  pending: 'run' | 'stop' | 'reset' | null;
  error: string | null;
}

const INITIAL_STATE: ProgramRuntimeState = {
  catalog: null,
  applied: null,
  currentScenario: null,
  run: null,
  selectedScenarioId: null,
  runnerAvailable: false,
  loading: true,
  pending: null,
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
  run: ProgramRunStatus | null,
  snapshot: ComposerHeadlessState | null,
  requestOwnerEpoch: string,
  requestId: string,
): Record<string, JsonValue> | null {
  const cycle = snapshot?.programCycle;
  if (
    run === null || cycle === undefined
    || cycle.state !== 'terminal_awaiting_reset'
    || cycle.programRunId === null || cycle.cycleClaimGeneration === null
    || cycle.terminalOrSettlementReference === null
    || run.capture.programRunId !== cycle.programRunId
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

  const refresh = useCallback(async () => {
    if (!enabled || !snapshotAvailable) return;
    if (refreshing.current) {
      refreshQueued.current = true;
      return;
    }
    refreshing.current = true;
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
        } else if (observedRunId.current !== null) {
          try {
            run = await api.programs.read(observedRunId.current);
          } catch (reason) {
            if (!(reason instanceof ApiError && reason.status === 404)) {
              errors.push(errorMessage(
                reason,
                'Program-run status unavailable.',
              ));
            }
          }
        }
      } else {
        errors.push(errorMessage(
          indexResult.reason,
          'Program runner unavailable.',
        ));
      }
      if (!mounted.current) return;

      setState((current) => {
        const nextCatalog = catalog ?? current.catalog;
        const next = {
          ...current,
          catalog: nextCatalog,
          currentScenario,
          applied,
          run,
          // Owner-issued Applied state is a projection, not operator intent.
          // Refresh may retain an explicit selection but must never create one.
          selectedScenarioId: retainedScenarioSelection(
            current.selectedScenarioId,
            nextCatalog,
          ),
          runnerAvailable,
          loading: false,
          pending: null,
          error: errors.length > 0 ? [...new Set(errors)].join(' ') : null,
        };
        return current.catalog?.scenarioCatalogRevision
            === next.catalog?.scenarioCatalogRevision
          && currentScenarioKey(current.currentScenario)
            === currentScenarioKey(next.currentScenario)
          && appliedKey(current.applied) === appliedKey(next.applied)
          && runKey(current.run) === runKey(next.run)
          && current.selectedScenarioId === next.selectedScenarioId
          && current.runnerAvailable === next.runnerAvailable
          && current.loading === next.loading
          && current.pending === next.pending
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
  }, [enabled, snapshotAvailable]);

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

  const run = useCallback(async () => {
    const requestId = identity('program-run-start');
    const request = buildProgramStartRequest(
      state.applied, state.runnerAvailable, state.pending, snapshot, workspace,
      callerEpoch.current, requestId,
    );
    if (request === null) return;
    setState((current) => ({ ...current, pending: 'run', error: null }));
    try {
      const status = await api.programs.start(request);
      observedRunId.current = status.capture.programRunId;
      observedOwnerEpoch.current = status.ownerEpoch;
      if (mounted.current) {
        setState((current) => ({ ...current, run: status, pending: null }));
      }
    } catch (reason) {
      if (mounted.current) {
        setState((current) => ({
          ...current,
          pending: null,
          error: errorMessage(reason, 'Program Run was rejected.'),
        }));
      }
    }
  }, [snapshot, state.applied, state.pending, state.runnerAvailable, workspace]);

  const reset = useCallback(async () => {
    const request = buildProgramResetRequest(
      state.run, snapshot, callerEpoch.current, identity('program-cycle-reset'),
    );
    if (request === null) return;
    setState((current) => ({ ...current, pending: 'reset', error: null }));
    try {
      await api.programs.resetCycle(request);
      await refresh();
    } catch (reason) {
      if (mounted.current) setState((current) => ({
        ...current, pending: null, error: errorMessage(reason, 'Program Reset was rejected.'),
      }));
    }
  }, [refresh, snapshot, state.run]);

  const stop = useCallback(async () => {
    if (
      !state.run
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    setState((current) => ({ ...current, pending: 'stop', error: null }));
    try {
      const requestId = identity('program-run-stop');
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
        setState((current) => ({ ...current, run: status, pending: null }));
      }
    } catch (reason) {
      if (mounted.current) {
        setState((current) => ({
          ...current,
          pending: null,
          error: errorMessage(reason, 'Program Stop was rejected.'),
        }));
      }
    }
  }, [state.pending, state.run, state.runnerAvailable]);

  const active = useMemo(
    () => state.run !== null && ACTIVE_STATES.has(state.run.state),
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
    run,
    stop,
    reset,
  }), [
    active,
    clearScenarioSelection,
    cycleState,
    refresh,
    reset,
    run,
    selectScenario,
    state,
    stop,
  ]);
}

export type ProgramRuntimeController = ReturnType<typeof useProgramRuntime>;

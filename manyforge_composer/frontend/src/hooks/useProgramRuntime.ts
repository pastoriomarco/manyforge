import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  AppliedProgramSelection,
  ProgramRunStatus,
  ScenarioCatalogResponse,
} from '../types/api';

const PROGRAM_STATUS_POLL_MS = 500;
const ACTIVE_STATES = new Set([
  'running',
  'intervention_required',
  'stopping',
  'finalizing',
]);

function identity(prefix: string): string {
  const random = globalThis.crypto?.randomUUID?.()
    ?? `${Date.now()}-${Math.random().toString(16).slice(2)}`;
  return `${prefix}.${random}`;
}

interface ProgramRuntimeState {
  catalog: ScenarioCatalogResponse | null;
  applied: AppliedProgramSelection | null;
  run: ProgramRunStatus | null;
  selectedScenarioId: string | null;
  runnerAvailable: boolean;
  loading: boolean;
  pending: 'run' | 'stop' | null;
  error: string | null;
}

const INITIAL_STATE: ProgramRuntimeState = {
  catalog: null,
  applied: null,
  run: null,
  selectedScenarioId: null,
  runnerAvailable: false,
  loading: true,
  pending: null,
  error: null,
};

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

export function useProgramRuntime(enabled: boolean) {
  const [state, setState] = useState<ProgramRuntimeState>(INITIAL_STATE);
  const mounted = useRef(true);
  const refreshing = useRef(false);
  const observedRunId = useRef<string | null>(null);
  const observedOwnerEpoch = useRef<string | null>(null);
  const callerEpoch = useRef(identity('composer-program-client'));

  const refresh = useCallback(async () => {
    if (!enabled || refreshing.current) return;
    refreshing.current = true;
    try {
      const [catalogResult, appliedResult, indexResult] = await Promise.allSettled([
        api.programs.scenarios(),
        api.programs.applied(),
        api.programs.runs(true, 1),
      ]);
      if (!mounted.current) return;

      const errors: string[] = [];
      const catalog = catalogResult.status === 'fulfilled'
        ? catalogResult.value
        : null;
      if (catalogResult.status === 'rejected') {
        errors.push(errorMessage(
          catalogResult.reason,
          'Scenario catalog unavailable.',
        ));
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
        return {
          ...current,
          catalog: nextCatalog,
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
          error: errors.length > 0 ? [...new Set(errors)].join(' ') : null,
        };
      });
    } finally {
      refreshing.current = false;
    }
  }, [enabled]);

  useEffect(() => {
    mounted.current = true;
    if (!enabled) {
      setState(INITIAL_STATE);
      observedRunId.current = null;
      observedOwnerEpoch.current = null;
      return () => {
        mounted.current = false;
      };
    }
    void refresh();
    const timer = window.setInterval(() => {
      void refresh();
    }, PROGRAM_STATUS_POLL_MS);
    return () => {
      mounted.current = false;
      window.clearInterval(timer);
      // Observation loss is deliberately effect-free. In particular, client
      // unmount never submits Program Stop.
    };
  }, [enabled, refresh]);

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
    if (
      !state.applied
      || !state.runnerAvailable
      || state.pending !== null
    ) return;
    setState((current) => ({ ...current, pending: 'run', error: null }));
    try {
      const requestId = identity('program-run-start');
      const status = await api.programs.start({
        request_id: requestId,
        request_owner_epoch: callerEpoch.current,
        source_label: 'operator_ui',
        correlation_id: requestId,
        scenario_id: state.applied.scenarioId,
        scenario_catalog_revision: state.applied.scenarioCatalogRevision,
        expected_applied_record_digest: state.applied.recordDigest,
      });
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
  }, [state.applied, state.pending, state.runnerAvailable]);

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

  return {
    state,
    active,
    refresh,
    selectScenario,
    clearScenarioSelection,
    run,
    stop,
  };
}

export type ProgramRuntimeController = ReturnType<typeof useProgramRuntime>;

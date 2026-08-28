import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { ApiError, api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type {
  AppliedProgramSelection,
  CurrentScenarioResponse,
  JsonValue,
  CycleRecoveryActionId,
  ProgramRuntimeControlProjection,
  RuntimeOverridePromotionProposal,
  RuntimeOverrideRecord,
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

interface ProgramRuntimeState {
  catalog: ScenarioCatalogResponse | null;
  applied: AppliedProgramSelection | null;
  currentScenario: CurrentScenarioResponse | null;
  run: ProgramRunStatus | null;
  controls: ProgramRuntimeControlProjection | null;
  selectedScenarioId: string | null;
  runnerAvailable: boolean;
  loading: boolean;
  pending: 'run' | 'stop' | 'reset' | null;
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
  const cycleRunId = snapshot?.programCycle.programRunId ?? null;

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

      setState((current) => {
        const nextCatalog = catalog ?? current.catalog;
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
          pending: null,
          // Automatic owner refresh must not erase a command rejection before
          // the operator can see it. The next explicit command clears it.
          error: errors.length > 0
            ? [...new Set(errors)].join(' ')
            : current.error,
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
  }, [cycleRunId, enabled, snapshotAvailable]);

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

  const dismissError = useCallback(() => {
    setState((current) => current.error === null
      ? current
      : { ...current, error: null });
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
        setState((current) => ({
          ...current,
          run: status,
          controls: null,
          pending: null,
        }));
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
      snapshot, callerEpoch.current, identity('program-cycle-reset'),
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
  }, [refresh, snapshot]);

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
    effect: () => Promise<{
      run: ProgramRunStatus;
      runtimeControls: ProgramRuntimeControlProjection;
    }>,
    fallback: string,
  ) => {
    setState((current) => ({
      ...current,
      controlPending: pending,
      error: null,
    }));
    try {
      const result = await effect();
      if (mounted.current) setState((current) => ({
        ...current,
        run: result.run,
        controls: result.runtimeControls,
        controlPending: null,
      }));
    } catch (reason) {
      if (mounted.current) setState((current) => ({
        ...current,
        controlPending: null,
        error: errorMessage(reason, fallback),
      }));
    }
  }, []);

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
    dismissError,
    run,
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

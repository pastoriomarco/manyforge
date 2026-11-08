import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { requestErrorText } from '../components/ComponentStatePanel';
import {
  activationClears,
  activationMayClear,
  activationOffered,
  activationStep,
  inactiveComponentIds,
  recoveryTargets,
  stepProgress,
  type ActivationFailure,
  type ActivationStep,
} from '../state/activateExecute';
import { blockedReasonText, enableAllTargets, prepareAllTargets, recoveryCauseText } from '../state/componentPresentation';
import { recoveryBlockers, recoveryConfirmation, withinConfirmed } from '../state/recoverEnable';
import type { ComponentState, RecoveryPreflight, RunReadiness } from '../types/components';

export interface ActivationFlow {
  /**
   * `offered`: the popup asks; `prepare`/`enable`: that group action runs;
   * `recover`: Recover and enable runs, one component at a time;
   * `waiting`: activation targets are up, Execute waits for readiness;
   * `failed`: stopped, nothing executed.
   */
  phase: 'offered' | 'prepare' | 'recover' | 'enable' | 'waiting' | 'failed';
  /** The components that weren't enabled when the popup opened. */
  componentIds: string[];
  failures: ActivationFailure[];
  /** Components that need Recover and enable, with the causes the operator confirms (null while read). */
  recovery: Record<string, RecoveryPreflight | null>;
}

/**
 * Execute with the components it needs ("Activate components and
 * execute?"): when Execute is blocked by components not
 * enabled or not prepared (and possibly by rule holds or stale inputs they
 * cause), Execute offers to bring them up. Confirm runs Prepare all (owners
 * first) and Enable all through the group action route, follows each step in
 * the pushed component states, then executes once when readiness says
 * `can_execute` or only holds/stale inputs remain (the server's readiness
 * check then decides). Any failure, or a Stop that ends a group action,
 * stops it before Execute; Cancel stops it
 * at any point.
 */
export function useActivateExecute(
  states: readonly ComponentState[] | null,
  readiness: RunReadiness | null,
  execute: () => Promise<void>,
  /** The cell's Stop generation (it changes with every Stop): a Stop ends the group action. */
  stopGeneration: number | null = null,
) {
  const [flow, setFlow] = useState<ActivationFlow | null>(null);
  const step = useRef<ActivationStep | null>(null);
  // Recover and enable runs one component at a time (each refusal comes back at
  // once); the rest wait here, with the causes the operator confirmed.
  const recoveryQueue = useRef<string[]>([]);
  const confirmed = useRef<RecoveryPreflight[]>([]);
  const latest = useRef({ states, execute, stopGeneration });
  // eslint-disable-next-line react-hooks/refs -- event handlers read the latest values written during render.
  latest.current = { states, execute, stopGeneration };
  // A new token for each popup: answers to an earlier one are dropped.
  const token = useRef(0);

  const fail = useCallback((current: number, failures: ActivationFailure[]) => {
    if (token.current !== current) return;
    step.current = null;
    recoveryQueue.current = [];
    setFlow((value) => value === null ? null : { ...value, phase: 'failed', failures });
  }, []);

  /** Sends one group action (Prepare all or Enable all); with no target it waits for readiness. */
  const sendGroup = useCallback((current: number, action: ActivationStep['action'], targets: string[]) => {
    if (targets.length === 0) {
      step.current = null;
      setFlow((value) => value === null ? null : { ...value, phase: 'waiting' });
      return;
    }
    const cell = latest.current.states ?? [];
    step.current = activationStep(action, targets, cell, latest.current.stopGeneration);
    setFlow((value) => value === null ? null : { ...value, phase: action });
    api.components.groupAction(action, targets).catch((reason: unknown) => {
      fail(current, [{ componentId: null, text: requestErrorText(reason) }]);
    });
  }, [fail]);

  /**
   * The next Recover and enable, then Enable all once none is left: the
   * causes are read again and must stay within what the operator confirmed
   * (a recovered machine drops out); anything new stops the activation and
   * shows it.
   */
  const recoverNext = useCallback((current: number) => {
    const cell = latest.current.states ?? [];
    let id = recoveryQueue.current.shift();
    while (id !== undefined && cell.find((state) => state.component_id === id)?.lifecycle === 'enabled') {
      id = recoveryQueue.current.shift();
    }
    if (id === undefined) {
      sendGroup(current, 'enable', enableAllTargets(cell));
      return;
    }
    const target = id;
    api.components.recoverEnablePreflight(target).then((fresh) => {
      if (token.current !== current) return;
      const blockers = recoveryBlockers(fresh);
      if (blockers.length > 0) {
        fail(current, blockers.map((text) => ({ componentId: target, text })));
        return;
      }
      if (!withinConfirmed(fresh, confirmed.current)) {
        fail(current, [{ componentId: target, text: `The causes changed since you confirmed them: ${fresh.components
          .filter((row) => row.eligibility !== 'ready').map(recoveryCauseText).join('; ')}` }]);
        return;
      }
      const states = latest.current.states ?? [];
      step.current = activationStep('enable', [target], states, latest.current.stopGeneration);
      setFlow((value) => value === null ? null : { ...value, phase: 'recover' });
      api.components.recoverEnable(target, recoveryConfirmation(fresh)).catch((reason: unknown) => {
        fail(current, [{ componentId: target, text: requestErrorText(reason) }]);
      });
    }).catch((reason: unknown) => fail(current, [{ componentId: target, text: requestErrorText(reason) }]));
  }, [fail, sendGroup]);

  /**
   * Prepare all (skipped when nothing needs it), then Recover and enable for
   * each component that needs it, then Enable all; with nothing left to
   * enable it waits for readiness.
   */
  const start = useCallback((current: number) => {
    const prepare = prepareAllTargets(latest.current.states ?? []);
    if (prepare.length > 0) sendGroup(current, 'prepare', prepare);
    else recoverNext(current);
  }, [recoverNext, sendGroup]);

  const open = useCallback(() => {
    token.current += 1;
    const current = token.current;
    step.current = null;
    const cell = latest.current.states ?? [];
    const targets = recoveryTargets(cell);
    recoveryQueue.current = [];
    confirmed.current = [];
    setFlow({ phase: 'offered', componentIds: inactiveComponentIds(cell), failures: [],
      recovery: Object.fromEntries(targets.map((id) => [id, null])) });
    for (const id of targets) {
      api.components.recoverEnablePreflight(id).then((preflight) => {
        if (token.current !== current) return;
        setFlow((value) => value === null ? null : { ...value, recovery: { ...value.recovery, [id]: preflight } });
      }).catch((reason: unknown) => {
        if (token.current !== current) return;
        setFlow((value) => value === null ? null : {
          ...value, failures: [...value.failures, { componentId: id, text: requestErrorText(reason) }],
        });
      });
    }
  }, []);

  const recoveryReady = flow !== null && Object.values(flow.recovery).every((preflight) => preflight !== null
    && recoveryBlockers(preflight).length === 0) && flow.failures.length === 0;

  const confirm = useCallback(() => {
    if (flow?.phase !== 'offered' || !recoveryReady) return;
    recoveryQueue.current = Object.keys(flow.recovery);
    confirmed.current = Object.values(flow.recovery).filter((preflight) => preflight !== null);
    start(token.current);
  }, [flow, recoveryReady, start]);

  const cancel = useCallback(() => {
    token.current += 1;
    step.current = null;
    recoveryQueue.current = [];
    setFlow(null);
  }, []);

  const phase = flow?.phase ?? null;
  useEffect(() => {
    const current = token.current;
    if ((phase === 'prepare' || phase === 'recover' || phase === 'enable') && step.current !== null && states !== null) {
      const progress = stepProgress(step.current, states, stopGeneration);
      if (progress.failures.length > 0) fail(current, progress.failures);
      else if (progress.done && (phase === 'prepare' || phase === 'recover')) {
        step.current = null;
        recoverNext(current);
      }
      else if (progress.done) {
        step.current = null;
        setFlow((value) => value === null ? null : { ...value, phase: 'waiting' });
      }
    }
  }, [fail, phase, recoverNext, states, stopGeneration]);

  useEffect(() => {
    if (phase !== 'waiting' || readiness === null) return;
    const reasons = readiness.blocked_reasons;
    const others = reasons.filter((reason) => !activationClears(reason, states ?? []));
    // Only rule holds or stale inputs left: Execute, and the server's readiness
    // check decides (a hold that persists is its typed refusal, shown).
    if (readiness.can_execute || (others.length === reasons.length && others.every(activationMayClear))) {
      token.current += 1;
      setFlow(null);
      void latest.current.execute();
      return;
    }
    const blocking = others.filter((reason) => !activationMayClear(reason));
    if (blocking.length > 0) {
      fail(token.current, blocking.map((reason) => ({ componentId: null, text: blockedReasonText(reason) })));
    }
  }, [fail, phase, readiness, states]);

  /** Execute is blocked by components Activate brings up (and holds they may cause). */
  const available = activationOffered(readiness, states);
  return useMemo(() => ({ available, flow, recoveryReady, states: states ?? [], open, confirm, cancel }),
    [available, cancel, confirm, flow, open, recoveryReady, states]);
}

export type ActivateExecuteController = ReturnType<typeof useActivateExecute>;

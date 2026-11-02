import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

import { api } from '../api/client';
import { requestErrorText } from '../components/ComponentStatePanel';
import {
  activationClears,
  activationMayClear,
  activationOffered,
  activationStep,
  inactiveComponentIds,
  stepProgress,
  type ActivationFailure,
  type ActivationStep,
} from '../state/activateExecute';
import { blockedReasonText, enableAllTargets, prepareAllTargets } from '../state/componentPresentation';
import type { ComponentState, RunReadiness } from '../types/components';

export interface ActivationFlow {
  /**
   * `offered`: the popup asks; `prepare`/`enable`: that group action runs;
   * `waiting`: every component is up, Execute waits for readiness;
   * `failed`: stopped, nothing executed.
   */
  phase: 'offered' | 'prepare' | 'enable' | 'waiting' | 'failed';
  /** The components that weren't enabled when the popup opened. */
  componentIds: string[];
  failures: ActivationFailure[];
}

/**
 * Execute with the components it needs (M7's "Activate components and
 * execute?", decision AUD-7): when Execute is blocked by components not
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
  const latest = useRef({ states, execute, stopGeneration });
  latest.current = { states, execute, stopGeneration };
  // A new token for each popup: answers to an earlier one are dropped.
  const token = useRef(0);

  const fail = useCallback((current: number, failures: ActivationFailure[]) => {
    if (token.current !== current) return;
    step.current = null;
    setFlow((value) => value === null ? null : { ...value, phase: 'failed', failures });
  }, []);

  /**
   * Sends Prepare all (`from: 'prepare'`, skipped when nothing needs it) or
   * Enable all; with nothing left to enable it waits for readiness.
   */
  const run = useCallback((current: number, from: ActivationStep['action']) => {
    const cell = latest.current.states ?? [];
    const prepare = from === 'prepare' ? prepareAllTargets(cell) : [];
    const action = prepare.length > 0 ? 'prepare' : 'enable';
    const targets = prepare.length > 0 ? prepare : enableAllTargets(cell);
    if (targets.length === 0) {
      step.current = null;
      setFlow((value) => value === null ? null : { ...value, phase: 'waiting' });
      return;
    }
    step.current = activationStep(action, targets, cell, latest.current.stopGeneration);
    setFlow((value) => value === null ? null : { ...value, phase: action });
    api.components.groupAction(action, targets).catch((reason: unknown) => {
      fail(current, [{ componentId: null, text: requestErrorText(reason) }]);
    });
  }, [fail]);

  const open = useCallback(() => {
    token.current += 1;
    step.current = null;
    setFlow({ phase: 'offered', componentIds: inactiveComponentIds(latest.current.states ?? []), failures: [] });
  }, []);

  const confirm = useCallback(() => {
    if (flow?.phase !== 'offered') return;
    run(token.current, 'prepare');
  }, [flow, run]);

  const cancel = useCallback(() => {
    token.current += 1;
    step.current = null;
    setFlow(null);
  }, []);

  const phase = flow?.phase ?? null;
  useEffect(() => {
    const current = token.current;
    if ((phase === 'prepare' || phase === 'enable') && step.current !== null && states !== null) {
      const progress = stepProgress(step.current, states, stopGeneration);
      if (progress.failures.length > 0) fail(current, progress.failures);
      else if (progress.done && phase === 'prepare') run(current, 'enable');
      else if (progress.done) {
        step.current = null;
        setFlow((value) => value === null ? null : { ...value, phase: 'waiting' });
      }
    }
  }, [fail, phase, run, states, stopGeneration]);

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
  return useMemo(() => ({ available, flow, states: states ?? [], open, confirm, cancel }),
    [available, cancel, confirm, flow, open, states]);
}

export type ActivateExecuteController = ReturnType<typeof useActivateExecute>;

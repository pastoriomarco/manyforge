import { useEffect, useRef } from 'react';

import { appliedKey, type ProgramRuntimeController } from './useProgramRuntime';
import type { ProgramWorkspaceController } from './useProgramWorkspace';

/**
 * Keep the Applied views current after an Apply, event-driven (no polling).
 *
 * A Manager Apply settles after its publication, and the Headless Applied
 * readback can briefly refuse right after it. So the Program runtime's
 * Applied selection and the Program workspace's Applied half are re-read on
 * the authoritative settlement event (`settlement`, from
 * `managerApplySettlementKey`), and the workspace follows whenever the
 * runtime's Applied selection changes (including its recovery from a
 * transient readback refusal). Without this, Execute could stay refused
 * ("Apply or Discard first") and Apply/Discard enabled after a committed Apply.
 */
export function useAppliedSettlementRefresh({
  settlement,
  runtime,
  program,
}: {
  settlement: string | null;
  runtime: ProgramRuntimeController;
  program: ProgramWorkspaceController;
}): void {
  const latest = useRef({ runtime, program });
  useEffect(() => {
    latest.current = { runtime, program };
  });
  const settled = useRef<string | null>(null);
  useEffect(() => {
    if (settlement === null || settled.current === settlement) return;
    settled.current = settlement;
    void latest.current.runtime.refresh();
    latest.current.program.refreshApplied();
  }, [settlement]);
  const applied = appliedKey(runtime.state.applied);
  const observed = useRef<string | null>(null);
  useEffect(() => {
    const previous = observed.current;
    observed.current = applied;
    if (previous === null || previous === applied || applied === 'none') return;
    latest.current.program.refreshApplied();
  }, [applied]);
}

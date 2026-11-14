import { useEffect, useRef } from 'react';

import { CommandRowWaiter } from '../state/commandRows';
import type { CommandRow, CommandRows } from '../types/api';

/**
 * One waiter for this page's own commands, fed with the rows the page
 * observes. A lost connection keeps the waits (the rows arrive again with the
 * next snapshot); only a new application start, which keeps no rows of the
 * commands sent before it, or unmounting settles them with null.
 */
export function useCommandRowWaiter<Row extends CommandRow>(
  rows: CommandRows<string, Row> | null,
  applicationStartId: string | null,
): CommandRowWaiter<Row> {
  const waiter = useRef<CommandRowWaiter<Row> | null>(null);
  waiter.current ??= new CommandRowWaiter<Row>();
  const current = waiter.current;
  const observedStart = useRef<string | null>(null);
  useEffect(() => {
    if (applicationStartId === null) return;
    if (observedStart.current !== null && observedStart.current !== applicationStartId) current.abandon();
    observedStart.current = applicationStartId;
  }, [applicationStartId, current]);
  useEffect(() => { current.observe(rows); }, [current, rows]);
  useEffect(() => () => current.abandon(), [current]);
  return current;
}

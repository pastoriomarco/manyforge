import { useEffect, useState } from 'react';

import type { ManagerOutstandingExchange } from '../types/api';

const PEER_LABELS: Record<ManagerOutstandingExchange['peer'], string> = {
  execution_host: 'Execution host',
  program_runner: 'Program Runner',
};

function exchangeKey(row: ManagerOutstandingExchange): string {
  return `${row.peer}:${row.channel}:${row.exchange}:${row.sinceNs}`;
}

/**
 * What the Manager waits on now (OD-295, OD-298), information only: no wait
 * has a time limit (OD-214), so a stalled peer shows here. The time counts
 * from when this view first saw the row; the Manager publishes a row only
 * after it stayed outstanding across two ticks.
 */
export function ManagerWaitingOn({ exchanges }: { exchanges: readonly ManagerOutstandingExchange[] }) {
  const [firstSeen, setFirstSeen] = useState<Readonly<Record<string, number>>>({});
  const [now, setNow] = useState(() => Date.now());
  const signature = exchanges.map(exchangeKey).join('|');
  useEffect(() => {
    const keys = signature === '' ? [] : signature.split('|');
    const seenAt = Date.now();
    setFirstSeen((current) => Object.fromEntries(keys.map((key) => [key, current[key] ?? seenAt])));
    setNow(seenAt);
    if (keys.length === 0) return undefined;
    const timer = window.setInterval(() => setNow(Date.now()), 1000);
    return () => window.clearInterval(timer);
  }, [signature]);
  return (
    <ul className="manager-waiting-on" role="status">
      {exchanges.map((row) => {
        const key = exchangeKey(row);
        const seen = firstSeen[key];
        const seconds = seen === undefined ? 0 : Math.max(0, Math.floor((now - seen) / 1000));
        return (
          <li key={key}>
            {PEER_LABELS[row.peer]} · {row.exchange} ({row.channel}){seconds > 0 ? ` · ${seconds} s` : ''}
          </li>
        );
      })}
    </ul>
  );
}

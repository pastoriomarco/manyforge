import { useEffect, useRef, useState } from 'react';
import { api } from '../api/client';
import type { HistorianDiscovery } from '../types/headless';

/** One explicit, replaceable index page; no background discovery or growing list. */
export function HistoryDiscovery({ onSelect }: {
  onSelect: (kind: HistorianDiscovery['kind'], value: string) => void;
}) {
  const [kind, setKind] = useState<HistorianDiscovery['kind']>('program_run_id');
  const [page, setPage] = useState<HistorianDiscovery | null>(null);
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);
  const pending = useRef<AbortController | null>(null);
  useEffect(() => () => pending.current?.abort(), []);
  const cancel = () => { pending.current?.abort(); setLoading(false); };
  const load = async (after = '') => {
    cancel();
    const request = new AbortController();
    pending.current = request;
    setLoading(true);
    setError(null);
    try {
      const next = await api.history.discovery(kind, after, request.signal);
      if (!request.signal.aborted) setPage(next);
    } catch (reason) {
      if (!request.signal.aborted) setError(reason instanceof Error ? reason.message : String(reason));
    } finally {
      if (!request.signal.aborted) setLoading(false);
    }
  };
  return <details onToggle={(event) => { if (!event.currentTarget.open) cancel(); }}>
    <summary>Find retained runs and channels</summary>
    <div className="button-row">
      <select aria-label="Discover history" value={kind} onChange={(event) => {
        cancel(); setPage(null); setError(null); setKind(event.target.value as HistorianDiscovery['kind']);
      }}>
        <option value="program_run_id">Program runs</option>
        <option value="channel">Components / channels</option><option value="event_type">Event types</option>
      </select>
      <button type="button" disabled={loading} onClick={() => { void load(); }}>Find</button>
      <button type="button" disabled={loading || !page?.nextAfter} onClick={() => { if (page?.nextAfter) void load(page.nextAfter); }}>More</button>
    </div>
    {error && <p role="alert" className="error-state notice severity-error">{error}</p>}
    {loading && <p className="muted">Loading retained values…</p>}
    {page && <select aria-label="Use retained history value" disabled={loading} value="" onChange={(event) => onSelect(kind, event.target.value)}>
      <option value="">{page.values.length ? 'Choose a retained value' : 'No retained values'}</option>
      {page.values.map((value) => <option key={value} value={value}>{value}</option>)}
    </select>}
  </details>;
}

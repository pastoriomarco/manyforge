import { useMemo, useState } from 'react';

export interface RuntimeComponentView {
  id: string;
  label: string;
  status: string;
  detail?: string;
  requiredFor?: string[];
}

export interface RuntimeStatusView {
  readiness: string;
  summaryLabel: string;
  profileLabel: string;
  authorityLabel: string;
  userMessage: string;
  capabilities: string[];
  components: RuntimeComponentView[];
}

function compactSummaryLabel(readiness: string, summaryLabel: string): string {
  if (readiness === 'ready') return 'Ready';
  if (readiness === 'fault') return 'Fault';
  if (readiness === 'degraded') return 'Warn';
  if (readiness === 'offline') return 'Off';
  if (readiness === 'waiting') return 'Wait';
  return summaryLabel || 'Status';
}

interface RuntimeStatusOverlayProps {
  status: RuntimeStatusView | null;
  error?: string | null;
  panelObscured?: boolean;
}

/**
 * Observation-only status presentation. I5 reconnects polling and its exact
 * headless projection; no old readiness inference or command gate lives here.
 */
export function RuntimeStatusOverlay({
  status,
  error = null,
  panelObscured = false,
}: RuntimeStatusOverlayProps) {
  const [open, setOpen] = useState(false);
  const readiness = status?.readiness ?? (error ? 'offline' : 'waiting');
  const summaryLabel = status?.summaryLabel ?? (error ? 'Offline' : 'Waiting');
  const sortedComponents = useMemo(
    () => [...(status?.components ?? [])].sort((left, right) =>
      left.label.localeCompare(right.label)),
    [status],
  );
  return (
    <div className={`runtime-status-overlay${panelObscured ? ' panel-obscured' : ''}`}>
      {!open && (
        <button
          type="button"
          className={`runtime-status-button readiness-${readiness}`}
          onClick={() => setOpen(true)}
          title={status?.userMessage || error || 'Runtime status unavailable'}
        >
          {compactSummaryLabel(readiness, summaryLabel)}
        </button>
      )}
      {open && (
        <section className="runtime-status-panel" aria-labelledby="runtime-status-title">
          <header>
            <div>
              <p className="eyebrow">Headless projection</p>
              <h2 id="runtime-status-title">{summaryLabel}</h2>
              <small>
                {status
                  ? `${status.profileLabel} · ${status.authorityLabel}`
                  : 'Status unavailable'}
              </small>
            </div>
            <button type="button" onClick={() => setOpen(false)}>Close</button>
          </header>
          {error && <p className="error-state" role="alert">{error}</p>}
          {!error && status && (
            <>
              <p>{status.userMessage}</p>
              <div className="runtime-capabilities">
                {status.capabilities.length === 0
                  ? <span className="muted">No qualified runtime capabilities.</span>
                  : status.capabilities.map((capability) => (
                    <span key={capability}>{capability}</span>
                  ))}
              </div>
              <div className="runtime-components">
                {sortedComponents.map((component) => (
                  <div key={component.id}>
                    <span className={`component-dot status-${component.status}`} />
                    <span>
                      <strong>{component.label}</strong>
                      <small>{component.detail || component.requiredFor?.join(', ') || 'optional'}</small>
                    </span>
                    <code>{component.status}</code>
                  </div>
                ))}
              </div>
            </>
          )}
          {!error && !status && <p className="muted">Waiting for a qualified status projection.</p>}
        </section>
      )}
    </div>
  );
}

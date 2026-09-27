import { useEffect, useRef } from 'react';

import type { RuntimeStatusView } from './runtimeStatus';

interface ComponentOverviewProps {
  request: number;
  status: RuntimeStatusView | null;
  backendConnected: boolean;
  connectionLabel: string;
  connectionDetail: string;
  contextFacts: string[];
  unacknowledgedAlarms: number;
  activeAcknowledgedAlarms: number;
  clientErrorCount: number;
  error: string | null;
  onReconnect: () => void;
  onOpenRuntimeHistory: () => void;
}

/** Reusable read-only destination for current component consequences. */
export function ComponentOverview({
  request,
  status,
  backendConnected,
  connectionLabel,
  connectionDetail,
  contextFacts,
  unacknowledgedAlarms,
  activeAcknowledgedAlarms,
  clientErrorCount,
  error,
  onReconnect,
  onOpenRuntimeHistory,
}: ComponentOverviewProps) {
  const surface = useRef<HTMLDetailsElement>(null);
  const components = status?.components ?? [];
  useEffect(() => {
    if (request < 1 || surface.current === null) return;
    surface.current.open = true;
    surface.current.scrollIntoView({ block: 'start', behavior: 'smooth' });
    surface.current.querySelector<HTMLElement>('summary')?.focus();
  }, [request]);
  return (
    <details ref={surface} className="component-overview">
      <summary tabIndex={-1}>Component overview</summary>
      <div className="component-overview-body">
        <header className="runtime-status-summary">
          <span
            className={`component-dot status-${backendConnected ? 'healthy' : 'fault'}`}
            aria-hidden="true"
          />
          <div>
            <strong>{connectionLabel}</strong>
            <small>{connectionDetail}</small>
          </div>
        </header>
        {error !== null && <p className="error-state notice severity-error" role="alert">{error}</p>}
        {status === null ? (
          <p className="muted">Waiting for current owner projections.</p>
        ) : (
          <>
            <p className="runtime-status-message">{status.userMessage}</p>
            <div className="runtime-status-facts">
              <span>{status.profileLabel}</span>
              <span>{status.authorityLabel}</span>
              <span>{unacknowledgedAlarms} acknowledgement required</span>
              <span>{activeAcknowledgedAlarms} active acknowledged</span>
              {clientErrorCount > 0 && <span>{clientErrorCount} Composer message</span>}
              {contextFacts.map((fact) => <span key={fact}>{fact}</span>)}
            </div>
            <div className="runtime-capabilities" aria-label="Functional scopes">
              {status.capabilities.map((capability) => (
                <span key={capability}>{capability}</span>
              ))}
            </div>
            <div className="runtime-components">
              {components.map((component) => (
                <div key={component.id}>
                  <span className={`component-dot status-${component.status}`} />
                  <span>
                    <strong>{component.label}</strong>
                    <small>{component.detail}</small>
                    <small className="runtime-component-meta">
                      {component.required ? 'required' : 'optional'}
                      {component.requiredFor.length > 0
                        ? ` · ${component.requiredFor.join(', ')}` : ''}
                      {component.ownerId ? ` · ${component.ownerId}` : ''}
                      {component.ownerEpoch ? ` @ ${component.ownerEpoch}` : ''}
                      {component.generation !== null ? ` · generation ${component.generation}` : ''}
                      {component.reasonCode ? ` · ${component.reasonCode}` : ''}
                    </small>
                  </span>
                  <code>{component.status}</code>
                </div>
              ))}
            </div>
          </>
        )}
        <div className="runtime-status-actions">
          {(!backendConnected || error !== null) && (
            <button
              type="button"
              onClick={onReconnect}
              title="Reopen the runtime connection without changing execution state."
            >Reconnect runtime</button>
          )}
          <button type="button" onClick={onOpenRuntimeHistory}>
            Runtime details and history
          </button>
        </div>
      </div>
    </details>
  );
}

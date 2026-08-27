import { useCallback, useMemo, useRef, useState } from 'react';

import { OverlayPanel } from './OverlayPanel';

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
  backendConnected: boolean;
  error?: string | null;
  connectionLabel: string;
  connectionDetail: string;
  unacknowledgedAlarms?: number;
  activeAcknowledgedAlarms?: number;
  contextFacts?: string[];
  onOpenDetails?: () => void;
  onRetry?: () => void;
}

/**
 * Observation-only status presentation. I5 reconnects polling and its exact
 * headless projection; no old readiness inference or command gate lives here.
 */
export function RuntimeStatusOverlay({
  status,
  backendConnected,
  error = null,
  connectionLabel,
  connectionDetail,
  unacknowledgedAlarms = 0,
  activeAcknowledgedAlarms = 0,
  contextFacts = [],
  onOpenDetails,
  onRetry,
}: RuntimeStatusOverlayProps) {
  const [open, setOpen] = useState(false);
  const trigger = useRef<HTMLButtonElement>(null);
  const readiness = status?.readiness ?? (error ? 'offline' : 'waiting');
  const summaryLabel = status?.summaryLabel ?? (error ? 'Offline' : 'Waiting');
  const alarmCount = unacknowledgedAlarms + activeAcknowledgedAlarms;
  const sortedComponents = useMemo(
    () => [...(status?.components ?? [])].sort((left, right) =>
      left.label.localeCompare(right.label)),
    [status],
  );
  const close = useCallback(() => setOpen(false), []);
  return (
    <div className="runtime-status-overlay">
      <button
        ref={trigger}
        type="button"
        className={`runtime-status-button readiness-${readiness}`}
        onClick={() => setOpen(true)}
        title={status?.userMessage || error || connectionDetail}
        aria-haspopup="dialog"
        aria-expanded={open}
        data-backend-connected={backendConnected}
      >
        <span className="runtime-status-dot" aria-hidden="true" />
        <span>{compactSummaryLabel(readiness, summaryLabel)}</span>
        {alarmCount > 0 && <span className="runtime-status-count">{alarmCount}</span>}
      </button>
      <OverlayPanel
        open={open}
        title={summaryLabel}
        titleId="runtime-status-title"
        kind="status"
        onClose={close}
        returnFocusRef={trigger}
      >
        <div className="runtime-status-summary">
          <span className={`component-dot status-${readiness}`} aria-hidden="true" />
          <div>
            <strong>{connectionLabel}</strong>
            <small>{connectionDetail}</small>
          </div>
        </div>
        {error && <p className="error-state" role="alert">{error}</p>}
        {!error && status && (
          <>
            <p className="runtime-status-message">{status.userMessage}</p>
            <div className="runtime-status-facts">
              <span>{status.profileLabel}</span>
              <span>{status.authorityLabel}</span>
              <span>{unacknowledgedAlarms} acknowledgement required</span>
              <span>{activeAcknowledgedAlarms} active acknowledged</span>
              {contextFacts.map((fact) => <span key={fact}>{fact}</span>)}
            </div>
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
        {!error && !status && (
          <p className="muted">Waiting for the backend status projection.</p>
        )}
        <div className="runtime-status-actions">
          {onRetry && <button type="button" onClick={onRetry}>Retry health</button>}
          {onOpenDetails && (
            <button
              type="button"
              className="primary-action"
              onClick={() => {
                close();
                onOpenDetails();
              }}
            >
              Runtime details and history
            </button>
          )}
        </div>
      </OverlayPanel>
    </div>
  );
}

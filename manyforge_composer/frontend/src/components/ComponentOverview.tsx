import { useEffect, useRef, type CSSProperties } from 'react';

import {
  COMPONENT_STATES_UNAVAILABLE,
  componentStateDetail,
  componentStateTree,
  componentTone,
  flattenComponentTree,
  latchCauseText,
  latchSummary,
  LIFECYCLE_LABELS,
  TONE_BADGE_CLASSES,
} from '../state/componentPresentation';
import type { CellState, ComponentState } from '../types/components';
import type { RuntimeStatusView } from './runtimeStatus';

export interface ComponentOverviewProps {
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
  /** The pushed component states and cell latch (the one view). */
  cellState: CellState;
  /** Each mounted component's parent (the Deployment Draft's `attachments[]`). */
  mounts?: Readonly<Record<string, string>>;
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
  cellState,
  mounts = {},
  onReconnect,
  onOpenRuntimeHistory,
}: ComponentOverviewProps) {
  const surface = useRef<HTMLDetailsElement>(null);
  const latch = cellState.latch;
  const latchText = latch === null ? null : latchSummary(latch);
  useEffect(() => {
    if (request < 1 || surface.current === null) return;
    surface.current.open = true;
    surface.current.scrollIntoView({ block: 'start', behavior: 'smooth' });
    surface.current.querySelector<HTMLElement>('summary')?.focus();
  }, [request]);
  return (
    <details ref={surface} className="component-overview dcard">
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
            <div className="runtime-status-facts dchips">
              <span>{status.profileLabel}</span>
              <span>{status.authorityLabel}</span>
              <span>{unacknowledgedAlarms} acknowledgement required</span>
              <span>{activeAcknowledgedAlarms} active acknowledged</span>
              {clientErrorCount > 0 && <span>{clientErrorCount} Composer message</span>}
              {contextFacts.map((fact) => <span key={fact}>{fact}</span>)}
            </div>
            <div className="runtime-capabilities dchips" aria-label="Functional scopes">
              {status.capabilities.map((capability) => (
                <span key={capability}>{capability}</span>
              ))}
            </div>
            {latch !== null && latchText !== null && (
              <details className="notice severity-error" open={latch.causes.length > 1}>
                <summary role="alert">{latchText}</summary>
                <ul>{latch.causes.map((cause) => (
                  <li key={`${cause.component_id}:${cause.kind}`}>{latchCauseText(cause)}</li>
                ))}</ul>
              </details>
            )}
            <div className="runtime-components">
              {cellState.components === null ? (
                <p className="muted">{COMPONENT_STATES_UNAVAILABLE}</p>
              ) : flattenComponentTree(componentStateTree(cellState.components, mounts)).map(({ item, depth }) => (
                // One tree: members under their machine, a mounted tool under its parent.
                <OverviewComponent key={item.component_id} component={item} states={cellState.components!} depth={depth} />
              ))}
            </div>
          </>
        )}
        <div className="runtime-status-actions dactions">
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

function OverviewComponent({ component, states, depth }: {
  component: ComponentState;
  states: readonly ComponentState[];
  depth: number;
}) {
  const detail = [component.role, componentStateDetail(component, states)].filter((part) => part != null);
  return (
    <div className="component-tree-item" style={{ '--component-depth': depth } as CSSProperties}>
      <span className={`component-dot status-${componentTone(component)}`} />
      <span>
        <strong>{component.component_id}</strong>
        {detail.length > 0 && <small>{detail.join(' · ')}</small>}
      </span>
      <span className={TONE_BADGE_CLASSES[componentTone(component)]}>{LIFECYCLE_LABELS[component.lifecycle]}</span>
    </div>
  );
}

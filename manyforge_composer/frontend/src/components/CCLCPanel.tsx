import type { CCLCController } from '../hooks/useCCLC';
import { useCCLC } from '../hooks/useCCLC';

export function CCLCPanelView({
  enabled,
  controller,
}: {
  enabled: boolean;
  controller: CCLCController;
}) {
  const {
    componentId,
    snapshot,
    routeTemplates,
    transaction,
    pendingTransaction,
    confirmed,
    busy,
    error,
    setComponentId,
    setConfirmed,
    refresh,
    submitLifecycle,
    submitRoute,
  } = controller;
  const actionsEnabled = enabled && snapshot !== null && confirmed && !busy;
  return (
    <section className="panel cclc-panel" aria-labelledby="cclc-panel-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">CCLC owner client</p>
          <h2 id="cclc-panel-title">Component lifecycle and control route</h2>
        </div>
        <span className="muted">No client-side lifecycle state</span>
      </header>

      <div className="inline-fields">
        <label>
          Component ID
          <input
            value={componentId}
            maxLength={256}
            disabled={!enabled || busy}
            onChange={(event) => setComponentId(event.target.value)}
            placeholder="lite6-arm"
          />
        </label>
        <button type="button" disabled={!enabled || busy || !componentId.trim()} onClick={() => void refresh()}>
          Read current owner state
        </button>
      </div>

      {snapshot === null ? (
        <p className="muted">Select an exact component before requesting a transition.</p>
      ) : (
        <div className="cclc-current-state">
          <p>
            <strong>{snapshot.componentId}</strong>
            {' · '}{snapshot.lifecycleState}
            {' · generation '}{snapshot.lifecycleGeneration}
          </p>
          <p className="muted">
            {snapshot.domains.length} retained control domain(s) · graph{' '}
            <code>{snapshot.graphRevision}</code>
          </p>
        </div>
      )}

      <label className="confirmation-row">
        <input
          type="checkbox"
          checked={confirmed}
          disabled={!enabled || snapshot === null || busy}
          onChange={(event) => setConfirmed(event.target.checked)}
        />
        Explicit confirmation: submit one request using a fresh owner snapshot
        immediately before submission
      </label>

      <div className="button-row" aria-label="Lifecycle requests">
        {(['prepare', 'enable', 'disable', 'recover'] as const).map((intent) => (
          <button
            type="button"
            key={intent}
            disabled={!actionsEnabled}
            onClick={() => void submitLifecycle(intent)}
          >
            {intent[0]?.toUpperCase()}{intent.slice(1)}
          </button>
        ))}
      </div>

      <div className="cclc-routes">
        <h3>Retained route templates</h3>
        {(routeTemplates?.templates ?? []).length === 0 ? (
          <p className="muted">No selectable route is retained for this component.</p>
        ) : routeTemplates?.templates.map((template) => {
          const desired = template.desired_configuration;
          const routes = desired.binding_routes
            .map((route) => `${route.control_binding_id} → ${route.control_route_id}`)
            .join(', ');
          return (
            <div key={desired.control_domain_id} className="cclc-route-row">
              <span>
                <strong>{desired.control_domain_id}</strong>
                {' · '}{routes}
              </span>
              <button
                type="button"
                disabled={!actionsEnabled}
                onClick={() => void submitRoute(template)}
              >
                Select retained route
              </button>
            </div>
          );
        })}
      </div>

      {pendingTransaction !== null && (
        <p>
          {pendingTransaction.kind} transaction{' '}
          <code>{pendingTransaction.id}</code>
          {transaction === null
            ? ' · awaiting owner status'
            : ` · ${transaction.phase}${transaction.terminalResult === null
              ? ''
              : ` · ${transaction.terminalResult}`}`}
        </p>
      )}
      {error !== null && <p className="error-state" role="alert">{error}</p>}
    </section>
  );
}

export function CCLCPanel({ enabled }: { enabled: boolean }) {
  const controller = useCCLC(enabled);
  return <CCLCPanelView enabled={enabled} controller={controller} />;
}

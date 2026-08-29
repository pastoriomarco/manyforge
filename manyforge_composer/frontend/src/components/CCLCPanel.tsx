import { useEffect, useState } from 'react';

import type { CCLCController } from '../hooks/useCCLC';
import { useCCLC } from '../hooks/useCCLC';
import type {
  CCLCComponentSnapshot,
  CCLCLifecycleIntent,
  CCLCRouteTemplate,
} from '../types/cclc';
import { OverlayPanel } from './OverlayPanel';
import { useOperationalErrorMirror } from '../state/operationalErrors';

const ACTIONS: Array<{
  intent: CCLCLifecycleIntent;
  label: string;
  detail: string;
  states: CCLCComponentSnapshot['lifecycleState'][];
}> = [
  {
    intent: 'prepare', label: 'Start / prepare',
    detail: 'Prepare the retained provider process and component.',
    states: ['declared', 'disabled'],
  },
  {
    intent: 'enable', label: 'Connect / enable',
    detail: 'Enable the prepared component and its retained control route.',
    states: ['prepared'],
  },
  {
    intent: 'disable', label: 'Stop / disable',
    detail: 'Contain robot work through Kernel Stop, then disable the component.',
    states: ['prepared', 'active', 'degraded', 'faulted'],
  },
  {
    intent: 'recover', label: 'Recover / restart',
    detail: 'Run the retained recovery plan, including any required clear or re-arm steps.',
    states: ['disabled'],
  },
];

export function lifecycleActionUnavailableReason(
  intent: CCLCLifecycleIntent,
  snapshot: CCLCComponentSnapshot | null,
  mutationAvailable: boolean,
  mutationUnavailableReason: string | null,
  busy: boolean,
): string | null {
  if (snapshot === null) return 'Current component state is unavailable.';
  if (busy) return 'Another lifecycle request is being read or submitted.';
  const action = ACTIONS.find((candidate) => candidate.intent === intent);
  if (action === undefined || !action.states.includes(snapshot.lifecycleState)) {
    return `${intent} is not available while the component is ${snapshot.lifecycleState}.`;
  }
  if (!mutationAvailable) {
    return mutationUnavailableReason ?? 'Lifecycle changes require current Idle admission.';
  }
  return null;
}

type PendingConfirmation =
  | { kind: 'lifecycle'; intent: CCLCLifecycleIntent; label: string; detail: string }
  | { kind: 'route'; template: CCLCRouteTemplate; label: string; detail: string };

export function CCLCPanelView({
  enabled,
  mutationAvailable,
  mutationUnavailableReason,
  controller,
  componentIds = controller.componentId ? [controller.componentId] : [],
}: {
  enabled: boolean;
  mutationAvailable: boolean;
  mutationUnavailableReason: string | null;
  controller: CCLCController;
  componentIds?: string[];
}) {
  const {
    componentId, snapshot, routeTemplates, transaction, pendingTransaction,
    busy, error, setComponentId, refresh, reconcileTransaction, dismissError,
    submitLifecycle, submitRoute,
  } = controller;
  useOperationalErrorMirror({
    source: 'composer.cclc',
    title: 'Component lifecycle or route command failed',
    message: error,
    onDismiss: dismissError,
  });
  const [confirmation, setConfirmation] = useState<PendingConfirmation | null>(null);
  const routeUnavailableReason = snapshot === null
    ? 'Current component state is unavailable.'
    : busy
      ? 'Another lifecycle request is being read or submitted.'
      : !mutationAvailable
        ? mutationUnavailableReason ?? 'Route changes require current Idle admission.'
        : snapshot.lifecycleState !== 'prepared' && snapshot.lifecycleState !== 'active'
          ? `Routes cannot change while the component is ${snapshot.lifecycleState}.`
          : null;

  const confirm = async () => {
    const current = confirmation;
    setConfirmation(null);
    if (current === null) return;
    if (current.kind === 'lifecycle') await submitLifecycle(current.intent);
    else await submitRoute(current.template);
  };

  return (
    <section className="panel cclc-panel" aria-labelledby="cclc-panel-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Manager-owned component</p>
          <h2 id="cclc-panel-title">Lifecycle and control route</h2>
        </div>
        <span className="muted">Owner-issued state</span>
      </header>

      <div className="inline-fields">
        <label>
          Component
          <select
            value={componentId}
            disabled={!enabled || busy || componentIds.length === 0}
            onChange={(event) => setComponentId(event.target.value)}
          >
            {componentIds.length === 0 && <option value="">No controllable components</option>}
            {componentIds.map((identity) => (
              <option key={identity} value={identity}>{identity}</option>
            ))}
          </select>
        </label>
        <button
          type="button"
          disabled={!enabled || busy || !componentId.trim()}
          onClick={() => void refresh()}
        >
          Refresh
        </button>
      </div>

      {snapshot === null ? (
        <p className="muted">Select a component to read its current lifecycle.</p>
      ) : (
        <div className="cclc-current-state">
          <p>
            <strong>{snapshot.componentId}</strong>
            {' · '}{snapshot.lifecycleState}
            {' · generation '}{snapshot.lifecycleGeneration}
          </p>
          <p className="muted">
            {snapshot.domains.length} control domain(s)
            {' · '}{snapshot.providers.length} managed provider process(es)
          </p>
          {snapshot.providers.map((provider) => (
            <div className="cclc-provider-card" key={`${provider.controlDomainId}:${provider.providerInstanceId}`}>
              <strong>{provider.providerRuntimeUnitId}</strong>
              <span>{provider.providerManifestId} · {provider.controlDomainId}</span>
            </div>
          ))}
        </div>
      )}

      <div className="button-row cclc-lifecycle-actions" aria-label="Lifecycle requests">
        {ACTIONS.map((action) => {
          const reason = !enabled
            ? 'CCLC owner is unavailable.'
            : lifecycleActionUnavailableReason(
              action.intent, snapshot, mutationAvailable,
              mutationUnavailableReason, busy,
            );
          return (
            <button
              type="button"
              key={action.intent}
              disabled={reason !== null}
              title={reason ?? action.detail}
              onClick={() => setConfirmation({
                kind: 'lifecycle',
                intent: action.intent,
                label: action.label,
                detail: action.detail,
              })}
            >
              {action.label}
            </button>
          );
        })}
      </div>

      <div className="cclc-routes">
        <h3>Control routes</h3>
        {(routeTemplates?.templates ?? []).length === 0 ? (
          <p className="muted">No retained route is selectable in the current state.</p>
        ) : routeTemplates?.templates.map((template) => {
          const desired = template.desired_configuration;
          const routes = desired.binding_routes
            .map((route) => `${route.control_binding_id} → ${route.control_route_id}`)
            .join(', ');
          return (
            <div key={desired.control_domain_id} className="cclc-route-row">
              <span><strong>{desired.control_domain_id}</strong>{' · '}{routes}</span>
              <button
                type="button"
                disabled={routeUnavailableReason !== null}
                title={routeUnavailableReason ?? 'Select this retained route'}
                onClick={() => setConfirmation({
                  kind: 'route', template, label: 'Select control route',
                  detail: `${desired.control_domain_id}: ${routes}`,
                })}
              >
                Select route
              </button>
            </div>
          );
        })}
      </div>

      {pendingTransaction !== null && (
        <div className="cclc-transaction" role="status">
          <span>
            <strong>{pendingTransaction.kind}</strong>{' · '}
            {transaction?.phase ?? 'awaiting owner status'}
            {transaction?.terminalResult ? ` · ${transaction.terminalResult}` : ''}
            {transaction?.reconciliationRequired ? ' · reconciliation required' : ''}
          </span>
          {(error !== null || transaction?.reconciliationRequired) && (
            <button type="button" disabled={busy} onClick={() => void reconcileTransaction()}>
              Reconcile
            </button>
          )}
        </div>
      )}
      {error !== null && <p className="error-state" role="alert">{error}</p>}

      <OverlayPanel
        open={confirmation !== null}
        title={confirmation?.label ?? 'Confirm lifecycle action'}
        titleId="cclc-confirmation-title"
        compact
        onClose={() => setConfirmation(null)}
      >
        <div className="program-dialog-copy program-compact-confirmation">
          <p>{confirmation?.detail}</p>
          <p>This uses a fresh owner snapshot immediately before submission.</p>
          <button type="button" className="primary-action" onClick={() => void confirm()}>
            Confirm
          </button>
        </div>
      </OverlayPanel>
    </section>
  );
}

export function CCLCPanel({
  enabled, componentIds, admissionGeneration, mutationAvailable,
  mutationUnavailableReason,
}: {
  enabled: boolean;
  componentIds: string[];
  admissionGeneration: number | null;
  mutationAvailable: boolean;
  mutationUnavailableReason: string | null;
}) {
  const controller = useCCLC(enabled, admissionGeneration);
  const { componentId, setComponentId, refresh } = controller;
  useEffect(() => {
    const next = componentIds.includes(componentId) ? componentId : componentIds[0] ?? '';
    if (next !== componentId) setComponentId(next);
  }, [componentId, componentIds, setComponentId]);
  useEffect(() => {
    if (!enabled || !componentIds.includes(componentId)) return;
    void refresh();
  }, [componentId, componentIds, enabled, refresh]);
  return (
    <CCLCPanelView
      enabled={enabled}
      mutationAvailable={mutationAvailable}
      mutationUnavailableReason={mutationUnavailableReason}
      controller={controller}
      componentIds={componentIds}
    />
  );
}

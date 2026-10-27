import { useState } from 'react';

import type { CCLCController } from '../hooks/useCCLC';
import type {
  CCLCComponentSnapshot,
  CCLCLifecycleFactValue,
  CCLCLifecycleIntent,
  CCLCRouteTemplateWire,
} from '../types/cclc';
import { OverlayPanel } from './OverlayPanel';
import {
  type ComponentLifecycleTone,
  componentLifecyclePresentation,
  componentLifecycleToneClass,
  componentReadinessOccurrencePresentation,
  readinessErrorPresentation,
} from '../state/componentLifecyclePresentation';
import { componentStreamHealthPresentation } from '../state/componentStreamHealthPresentation';

const ACTIONS: Array<{
  intent: CCLCLifecycleIntent;
  label: string;
  detail: string;
  states: CCLCComponentSnapshot['lifecycleState'][];
}> = [
  {
    intent: 'prepare', label: 'Prepare',
    detail: 'Verify retained runtime resources without admitting commands.',
    states: ['declared', 'disabled'],
  },
  {
    intent: 'enable', label: 'Enable',
    detail: 'Enable the prepared component after its control route is verified.',
    states: ['prepared'],
  },
  {
    intent: 'disable', label: 'Disable',
    detail: 'Only while the cell is idle: keep the component installed but close its command gate. This is not Stop.',
    states: ['prepared', 'active', 'degraded', 'faulted'],
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

const TONE_DOT: Record<ComponentLifecycleTone, string> = {
  ready: 'status-ready',
  waiting: 'status-waiting',
  held: 'status-warning',
  intervention: 'status-fault',
};

function factValue(value: CCLCLifecycleFactValue): string {
  return value.kind === 'unknown' ? 'unknown'
    : value.kind === 'boolean' ? String(value.booleanValue)
      : value.kind === 'integer' ? String(value.integerValue) : value.tokenValue;
}

/** Current state of one component: verdict, owner errors, then raw owner facts on demand. */
function ComponentLifecycleSummary({ snapshot }: { snapshot: CCLCComponentSnapshot }) {
  const presentation = componentLifecyclePresentation(snapshot);
  const problems = snapshot.readiness.providerOccurrences.flatMap((domain) => {
    const planError = readinessErrorPresentation(domain.error);
    const diagnostic = domain.health?.diagnostic ? readinessErrorPresentation(domain.health.diagnostic) : null;
    return [planError, diagnostic !== planError ? diagnostic : null]
      .filter((text): text is string => text !== null);
  });
  const measured = readinessErrorPresentation(snapshot.readiness.measuredState.error);
  const streams = componentStreamHealthPresentation(snapshot);
  return (
    <div className="cclc-lifecycle-summary" role="status" aria-label="Component lifecycle status">
      <div className="cclc-verdict">
        <span className={componentLifecycleToneClass(presentation.tone)}>{presentation.message}</span>
        <span className="muted">{presentation.consequence}</span>
      </div>
      {presentation.incident !== null && (
        <p className="inline-message severity-error">Incident: {presentation.incident}</p>
      )}
      {[...problems, ...(measured === null ? [] : [measured])].map((problem) => (
        <p className="inline-message severity-warning" key={problem}>{problem}</p>
      ))}
      <dl className="cclc-facts">
        <dt>State</dt><dd>{presentation.achieved}</dd>
        <dt>Requested target</dt><dd>{presentation.target}</dd>
        <dt>Readiness</dt><dd>{presentation.readiness}</dd>
      </dl>
      {streams !== null && (
        <details className="cclc-details" aria-label="Component stream health">
          <summary>
            <span className={`component-dot ${streams.dotClass}`} aria-hidden="true" />
            {' '}Streams · {streams.summary}
          </summary>
          <ul className="cclc-fact-list">
            {streams.streams.map((stream) => <li key={stream.streamId}>{stream.line}</li>)}
          </ul>
        </details>
      )}
      <details className="cclc-details">
        <summary>Provider occurrences and owner facts</summary>
        {snapshot.readiness.providerOccurrences.map((domain) => {
          const presented = componentReadinessOccurrencePresentation(domain);
          return (
            <div className="cclc-domain" key={`${domain.providerOccurrence.controlDomainId}:${domain.providerOccurrence.providerInstanceId}`}>
              <p><strong>{domain.providerOccurrence.controlDomainId}</strong> · {presented.state}</p>
              {presented.facts.length > 0 && (
                <ul className="cclc-fact-list">
                  {presented.facts.map((fact) => (
                    <li key={fact.lifecycleFactId}>{fact.lifecycleFactId}: {factValue(fact.value)}</li>
                  ))}
                </ul>
              )}
            </div>
          );
        })}
      </details>
    </div>
  );
}

type PendingConfirmation =
  | { kind: 'activation'; sequence: boolean; label: string; detail: string }
  | { kind: 'lifecycle'; intent: CCLCLifecycleIntent; label: string; detail: string }
  | { kind: 'route'; template: CCLCRouteTemplateWire; label: string; detail: string };

export function CCLCPanelView({
  enabled,
  mutationAvailable,
  mutationUnavailableReason,
  controller,
  componentIds = controller.controllableComponentId ? [controller.controllableComponentId] : [],
}: {
  enabled: boolean;
  mutationAvailable: boolean;
  mutationUnavailableReason: string | null;
  controller: CCLCController;
  componentIds?: string[];
}) {
  const {
    controllableComponentId, snapshot, routeTemplates, transaction, pendingTransaction,
    busy, readUnavailableReason, error, setControllableComponentId, refresh,
    reconcileTransaction,
    submitLifecycle, submitRoute,
  } = controller;
  const [confirmation, setConfirmation] = useState<PendingConfirmation | null>(null);
  const primitiveMessage = transaction?.error?.details.primitive_message;
  const routeUnavailableReason = snapshot === null
    ? 'Current component state is unavailable.'
    : busy
      ? 'Another lifecycle request is being read or submitted.'
      : !mutationAvailable
        ? mutationUnavailableReason ?? 'Route changes require current Idle admission.'
        : snapshot.lifecycleState !== 'prepared' && snapshot.lifecycleState !== 'active'
          ? `Routes cannot change while the component is ${snapshot.lifecycleState}.`
          : null;
  const activationBlocked = !enabled || !mutationAvailable || busy || controller.activationUnresolved;
  const activationBlockedReason = !enabled ? 'CCLC owner is unavailable.'
    : busy ? 'Another lifecycle request is being read or submitted.'
      : controller.activationUnresolved ? 'A previous activation outcome is still being resolved.'
        : !mutationAvailable ? mutationUnavailableReason ?? 'Lifecycle changes require current Idle admission.' : null;

  const confirm = async () => {
    const current = confirmation;
    setConfirmation(null);
    if (current === null) return;
    if (current.kind === 'activation') {
      if (current.sequence) await controller.activateSequence();
      else await controller.activate();
    } else if (current.kind === 'lifecycle') await submitLifecycle(current.intent);
    else await submitRoute(current.template);
  };

  return (
    <section className="panel cclc-panel" aria-labelledby="cclc-panel-title">
      <header className="cclc-header">
        <div>
          <h4 id="cclc-panel-title">Lifecycle</h4>
          <p className="muted">
            Activate components before executing a Program. Activation prepares and enables each
            component in Deployment order; it does not move anything.
          </p>
        </div>
        <div className="button-row">
          <button type="button" className="primary-action"
            disabled={activationBlocked || componentIds.length === 0}
            title={activationBlockedReason ?? 'Prepare and enable every configured component in Deployment order.'}
            onClick={() => setConfirmation({ kind: 'activation', sequence: true,
              label: 'Activate sequence?', detail: 'Activate in the applied Deployment list order. Stop at the first failure or unknown outcome.' })}>
            Activate all
          </button>
          <button type="button" className="quiet-button"
            disabled={!enabled || busy || !controllableComponentId.trim()}
            onClick={() => void refresh()}>
            Refresh
          </button>
        </div>
      </header>

      {(controller.activationProgress !== null || controller.activationRefusal !== null
        || controller.pendingActivationRequest !== null) && (
        <div className="cclc-activation-status">
          {controller.activationProgress !== null && <p role="status">{controller.activationProgress}</p>}
          {controller.activationRefusal !== null && <div role="status" className="notice severity-warning">
            <p>{controller.activationRefusal.controllableComponentId}: {
              controller.activationRefusal.error.details.observed_fault_present === 'true'
                || controller.activationRefusal.error.details.observed_blocking_cause === 'fault'
                ? 'Fault · intervention required' : 'Not ready'
            }</p>
            {['reason', 'observed_state', 'observed_controller_loaded', 'observed_fault_present', 'observed_transport_ready', 'observed_blocking_cause'].map((field) => (
              <p className="muted" key={field}>{field}: {controller.activationRefusal!.error.details[field] ?? 'missing'}</p>
            ))}
          </div>}
          {controller.pendingActivationRequest !== null && (
            <div role="status" className="notice severity-warning">
              Activation outcome unknown for {controller.pendingActivationRequest.controllable_component_id}
              {' · request '}{controller.pendingActivationRequest.request_context.request_id}
              <button type="button" disabled={busy} onClick={() => void reconcileTransaction()}>Reconcile</button>
            </div>
          )}
          <div className="button-row">
            {busy && controller.activationProgress !== null && (
              <button type="button" onClick={controller.cancelActivation}>Cancel remaining activation</button>
            )}
            {!busy && controller.activationProgress !== null && controller.activationProgress.startsWith('Activation stopped')
              && !controller.activationUnresolved && <button type="button" disabled={!enabled || !mutationAvailable || componentIds.length === 0}
                onClick={() => setConfirmation({ kind: 'activation', sequence: true, label: 'Retry activation sequence?',
                  detail: 'Run the whole current Deployment list again with fresh checks. Already Active components are skipped.' })}>Retry activation sequence</button>}
          </div>
        </div>
      )}

      {componentIds.length === 0 ? (
        <p className="muted" role="status">No controllable components</p>
      ) : (
        <ul className="cclc-component-list" aria-label="Configured component health">
          {componentIds.map((id) => {
            const current = controller.componentHealth?.[id];
            const rowSnapshot = id === controllableComponentId && snapshot !== null ? snapshot : current?.snapshot ?? null;
            const presentation = rowSnapshot === null ? null : componentLifecyclePresentation(rowSnapshot);
            const streams = rowSnapshot === null ? null : componentStreamHealthPresentation(rowSnapshot);
            const selected = id === controllableComponentId;
            return (
              <li key={id}>
                <button type="button"
                  className={selected ? 'cclc-component-row selected' : 'cclc-component-row'}
                  aria-pressed={selected}
                  disabled={!enabled || busy}
                  onClick={() => { if (!selected) setControllableComponentId(id); }}>
                  <span className={`component-dot ${presentation === null ? 'status-offline' : TONE_DOT[presentation.tone]}`} aria-hidden="true" />
                  <strong>{id}</strong>
                  <span className="cclc-row-state">{rowSnapshot?.lifecycleState ?? 'unknown'}</span>
                  <span className="muted cclc-row-message">
                    {presentation?.message ?? current?.error ?? 'Current component health has not been read.'}
                  </span>
                  {streams !== null && (
                    <span className="muted cclc-row-streams"
                      title={streams.streams.map((stream) => stream.line).join('\n')}>
                      <span className={`component-dot ${streams.dotClass}`} aria-hidden="true" />
                      {' '}{streams.summary}
                    </span>
                  )}
                </button>
              </li>
            );
          })}
        </ul>
      )}

      {snapshot === null ? (
        <p className="muted" role="status">
          {readUnavailableReason ?? 'Select a component to read its current lifecycle.'}
        </p>
      ) : (
        <div className="cclc-current-state">
          <div className="cclc-selected-head">
            <h5>{snapshot.controllableComponentId}</h5>
            <span className="muted">{snapshot.lifecycleState} · generation {snapshot.lifecycleGeneration}</span>
          </div>
          <ComponentLifecycleSummary snapshot={snapshot} />
          <div className="button-row cclc-lifecycle-actions" aria-label="Lifecycle requests">
            <button type="button" className="primary-action"
              disabled={activationBlocked || snapshot.lifecycleState === 'active'}
              title={snapshot.lifecycleState === 'active' ? 'Already active.'
                : activationBlockedReason ?? 'Prepare if required, then Enable this component to Active.'}
              onClick={() => setConfirmation({ kind: 'activation', sequence: false,
                label: 'Activate component?', detail: 'Prepare if required, then Enable this component to Active.' })}>
              Activate
            </button>
            {componentStreamHealthPresentation(snapshot)?.recoverOffered === true && (
              <button type="button"
                disabled={!enabled || busy}
                title={!enabled ? 'CCLC owner is unavailable.'
                  : busy ? 'Another lifecycle request is being read or submitted.'
                    : 'Acknowledge the latched stream so the component can move again.'}
                onClick={() => setConfirmation({ kind: 'lifecycle', intent: 'recover', label: 'Recover component?',
                  detail: 'Acknowledge the stream that needs acknowledgement. Nothing moves.' })}>
                Recover
              </button>
            )}
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
                  className="quiet-button"
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
          <details className="cclc-details">
            <summary>Providers and control routes</summary>
            <p className="muted">
              {snapshot.domains.length} control domain(s)
              {' · '}{snapshot.providers.length} provider occurrence(s)
            </p>
            {snapshot.providers.map((provider) => (
              <div className="cclc-provider-card" key={`${provider.controlDomainId}:${provider.providerInstanceId}`}>
                <strong>{provider.controlDomainId}</strong>
                <span>{provider.providerManifestId} · {provider.providerRuntimeUnitId}</span>
              </div>
            ))}
            <div className="cclc-routes">
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
          </details>
        </div>
      )}

      {pendingTransaction !== null && (
        <div className="cclc-transaction" role="status">
          <span>
            <strong>{pendingTransaction.kind}</strong>{' · '}
            {transaction?.phase ?? 'awaiting owner status'}
            {transaction?.terminalResult ? ` · ${transaction.terminalResult}` : ''}
            {transaction?.reconciliationRequired ? ' · reconciliation required' : ''}
            {transaction?.error
              ? ` · ${transaction.error.code}: ${transaction.error.message}`
                + (transaction.error.details.reason
                  ? ` (${transaction.error.details.reason})` : '')
              : ''}
          </span>
          {primitiveMessage?.trim() ? <p className="muted">{primitiveMessage}</p> : null}
          {(error !== null || transaction?.reconciliationRequired) && (
            <button type="button" disabled={busy} onClick={() => void reconcileTransaction()}>
              Reconcile
            </button>
          )}
        </div>
      )}
      <OverlayPanel
        open={confirmation !== null}
        title={confirmation?.label ?? 'Confirm lifecycle action'}
        titleId="cclc-confirmation-title"
        compact
        onClose={() => setConfirmation(null)}
      >
        <div className="dialog-copy compact-confirmation">
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
  enabled, componentIds, controller, mutationAvailable,
  mutationUnavailableReason,
}: {
  enabled: boolean;
  componentIds: string[];
  controller: CCLCController;
  mutationAvailable: boolean;
  mutationUnavailableReason: string | null;
}) {
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

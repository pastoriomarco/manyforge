import { useRef, useState } from 'react';

import { api, apiErrorCode } from '../api/client';
import { busyLabel, busyReason, type BusyOperation } from '../state/busyOperation';
import {
  actionBusyLabel,
  actionLabel,
  actionResultText,
  ageText,
  codedErrorText,
  CONFIRMED_ACTIONS,
  confirmedActionText,
  componentStateTree,
  componentSummary,
  componentTone,
  enableAllTargets,
  faultPresentation,
  COMPONENT_STATES_UNAVAILABLE,
  needsRecovery,
  offeredActions,
  ownerIds,
  prepareAllTargets,
  reasonText,
  stateLabel,
  stopText,
  streamLabel,
  type ComponentTreeNode,
} from '../state/componentPresentation';
import type { ComponentState } from '../types/components';
import { OverlayPanel } from './OverlayPanel';
import { RecoverEnableDialog } from './RecoverEnableDialog';

/** A refused or failed request, as the operator reads it (catalogue text by code). */
export function requestErrorText(error: unknown): string {
  const code = apiErrorCode(error);
  const message = error instanceof Error ? error.message : String(error);
  return code === null ? message : codedErrorText({ code, message });
}

/**
 * One request at a time from this card; the backend owns everything else. The
 * backend accepts an action (202) before it runs, so the card stays busy until
 * the pushed state shows it on a target: in progress, or a new result.
 * Its `operation` names the request (busy label, tooltip) while busy.
 */
function useRequest(states: readonly ComponentState[]) {
  const [pending, setPending] = useState(false);
  const [operation, setOperation] = useState<BusyOperation | null>(null);
  const [error, setError] = useState<string | null>(null);
  // Each target's last action when the request was sent.
  const [sent, setSent] = useState<Record<string, number | null> | null>(null);
  const lastStarted = (id: string) => states.find((state) => state.component_id === id)
    ?.last_action?.started_at_ns ?? null;
  const shown = sent === null || Object.entries(sent).some(([id, before]) => {
    const state = states.find((candidate) => candidate.component_id === id);
    return state === undefined || state.action !== undefined || lastStarted(id) !== before;
  });
  const busy = pending || !shown;
  const run = async (
    targets: readonly string[],
    send: () => Promise<unknown>,
    started: BusyOperation,
  ) => {
    if (busy) return;
    setPending(true);
    setOperation(started);
    setError(null);
    setSent(Object.fromEntries(targets.map((id) => [id, lastStarted(id)])));
    try {
      await send();
    } catch (reason) {
      setSent(null);
      setError(requestErrorText(reason));
    } finally {
      setPending(false);
    }
  };
  return { pending: busy, operation: busy ? operation : null, error, run };
}

/**
 * A component's actions: exactly the four lifecycle actions
 * plus its declared capabilities, always offered (an owner: Prepare, Recover
 * and its capabilities; an enabled component: no Prepare, `offeredActions`);
 * only an action in progress or this request in flight
 * disables them. A refused request shows under them.
 */
export function ComponentActions({ state, enabled, states = [], className = 'dactions' }: {
  state: ComponentState;
  enabled: boolean;
  states?: readonly ComponentState[];
  className?: string;
}) {
  const request = useRequest([state]);
  const id = state.component_id;
  const busy = !enabled || request.pending || state.action !== undefined;
  // Power off and Reboot ask first, like Reset and Halt.
  const [confirming, setConfirming] = useState<string | null>(null);
  // Enable on a faulted or e-stopped component recovers first, after the operator confirms the causes.
  const [recovering, setRecovering] = useState(false);
  const recover = needsRecovery(state, states);
  const confirmButton = useRef<HTMLButtonElement | null>(null);
  const owner = ownerIds(states).has(id);
  const send = (action: string) => {
    // A machine's Recover runs as a group so its unprepared members prepare after it.
    void request.run([id], () => (owner && action === 'recover'
      ? api.components.groupAction('recover', [id]) : api.components.action(id, action)), {
      command: action, label: actionBusyLabel(action), name: `${actionLabel(action)} ${id}`,
    });
  };
  const confirmation = confirming === null ? null : confirmedActionText(confirming, state, states);
  return (
    <>
      <div className={className}>
        {offeredActions(state, owner).map((action) => (
          <button key={action} type="button" disabled={busy}
            title={busyReason(request.operation) ?? undefined}
            onClick={(event) => {
              confirmButton.current = event?.currentTarget ?? null;
              if (action === 'enable' && recover) { setRecovering(true); return; }
              if (!CONFIRMED_ACTIONS.has(action)) { send(action); return; }
              setConfirming(action);
            }}>
            {busyLabel(request.operation, action,
              action === 'enable' && recover ? 'Recover and enable' : actionLabel(action))}
          </button>
        ))}
      </div>
      {request.error !== null && (
        <p className="inline-message severity-error" role="alert">{request.error}</p>
      )}
      <RecoverEnableDialog
        componentId={id}
        open={recovering}
        onClose={() => setRecovering(false)}
        returnFocusRef={confirmButton}
        onConfirm={(confirmation) => {
          setRecovering(false);
          void request.run([id], () => api.components.recoverEnable(id, confirmation), {
            command: 'enable', label: 'Recovering and enabling…', name: `Recover and enable ${id}`,
          });
        }}
      />
      <OverlayPanel
        open={confirming !== null}
        title={confirmation?.title ?? ''}
        titleId={`component-${id}-${confirming ?? 'action'}-confirmation-title`}
        kind="dialog"
        compact
        onClose={() => setConfirming(null)}
        returnFocusRef={confirmButton}
      >
        <div className="dialog-copy transport-confirmation">
          {confirmation?.body.map((text) => <p key={text}>{text}</p>)}
          <div className="dialog-actions">
            <button type="button" onClick={() => setConfirming(null)}>Cancel</button>
            <button
              type="button"
              className="danger-button"
              disabled={busy}
              onClick={() => {
                const action = confirming;
                setConfirming(null);
                if (action !== null) send(action);
              }}
            >{confirming === null ? '' : actionLabel(confirming)}</button>
          </div>
        </div>
      </OverlayPanel>
    </>
  );
}

/** The member's owner and that owner's fault, when the fault concerns the member. */
export function concernedOwnerFault(state: ComponentState, states: readonly ComponentState[]) {
  const owner = state.owner_id === undefined ? undefined
    : states.find((candidate) => candidate.component_id === state.owner_id);
  const fault = owner?.fault;
  return owner === undefined || fault === undefined || !(fault.concerns ?? []).includes(state.component_id)
    ? null : { ownerId: owner.component_id, fault: faultPresentation(fault) };
}

/**
 * One component's state and its actions. `states` (the
 * cell's) gives the machine context: a member shows its owner's reason
 * and, as a banner, an owner fault that concerns it. `embedded`: the rows
 * only, for a card that shows the state itself (the machine card's header).
 */
export function ComponentStatePanel({ state, enabled, states = [], embedded = false }: {
  state: ComponentState;
  enabled: boolean;
  states?: readonly ComponentState[];
  embedded?: boolean;
}) {
  const id = state.component_id;
  const fault = state.fault === undefined ? null : faultPresentation(state.fault);
  const concerned = concernedOwnerFault(state, states);
  const reason = reasonText(state, states);
  const Root = embedded ? 'div' : 'section';
  return (
    <Root className={embedded ? 'component-state-rows' : 'dcard'} role={embedded ? 'group' : undefined}
      aria-label={`${id} status`}>
      {!embedded && (
        <header>
          <h4>Status</h4>
          <span className={`component-dot status-${componentTone(state)}`} aria-hidden="true" />
        </header>
      )}
      {concerned !== null && (
        <div className="notice severity-error" role="alert">
          <strong>{concerned.ownerId}: {concerned.fault.title}</strong>
          <p>{concerned.fault.detail}</p>
          {concerned.fault.operatorAction !== null && (
            <p>Operator action: {concerned.fault.operatorAction} on {concerned.ownerId}</p>
          )}
        </div>
      )}
      <dl>
        {!embedded && <div><dt>State</dt><dd>{stateLabel(state)}</dd></div>}
        <div><dt>Activity</dt><dd>{state.activity}</dd></div>
        {reason !== null && (
          <div><dt>Reason</dt><dd>{reason}</dd></div>
        )}
        {state.action !== undefined && (
          <div><dt>In progress</dt><dd>{actionLabel(state.action.action)}</dd></div>
        )}
        {state.last_action !== undefined && (
          <div><dt>Last action</dt><dd>{actionResultText(state.last_action)}</dd></div>
        )}
      </dl>
      {state.stop !== undefined && (
        <div className="notice severity-warning" role="status">
          <strong>E-stop</strong>
          <p>{stopText(state.stop)}</p>
          <p>Release the e-stop, then use Recover and enable.</p>
        </div>
      )}
      {fault !== null && (
        <div className="notice severity-error" role="alert">
          <strong>{fault.title}</strong>
          <p>{fault.detail}</p>
          {fault.vendor !== null && <p className="muted">{fault.vendor}</p>}
          {fault.operatorAction !== null && <p>Operator action: {fault.operatorAction}</p>}
        </div>
      )}
      {state.streams.length > 0 && (
        <dl aria-label={`${id} streams`}>
          {state.streams.map((stream) => (
            <div key={stream.stream_id}>
              <dt title={stream.stream_id}>{streamLabel(stream.stream_id)}</dt>
              <dd>{stream.freshness}{stream.age_ns === undefined ? '' : ` · ${ageText(stream.age_ns)}`}</dd>
            </div>
          ))}
        </dl>
      )}
      <ComponentActions state={state} enabled={enabled} states={states} />
    </Root>
  );
}

/**
 * The cell's components in one line, as one tree (members under their
 * machine, a mounted tool under its parent), with Prepare all (every
 * `not_prepared` or `offline` component; the backend runs owners before their
 * members) and Enable all (every member and owner-less component; the
 * backend prepares an unprepared one, and its machine, first).
 */
export function CellComponentsCard({ states, enabled, mounts = {} }: {
  states: readonly ComponentState[] | null;
  enabled: boolean;
  /** Each mounted component's parent (the Deployment Draft's `attachments[]`). */
  mounts?: Readonly<Record<string, string>>;
}) {
  const request = useRequest(states ?? []);
  const prepared = enableAllTargets(states ?? []);
  const unprepared = prepareAllTargets(states ?? []);
  return (
    <section className="dcard" aria-label="Cell components">
      <header><h4>Cell</h4></header>
      {states === null
        ? <p className="muted" role="status">{COMPONENT_STATES_UNAVAILABLE}</p>
        : <p className="muted">{states.length === 0 ? 'No components in the cell.'
          : componentStateTree(states, mounts).map((node) => componentSummaryLine(node, states)).join('; ')}</p>}
      <div className="dactions">
        <button type="button" disabled={!enabled || request.pending || unprepared.length === 0}
          title={busyReason(request.operation)
            ?? (unprepared.length === 0 ? 'Every component is prepared.' : `Prepare ${unprepared.join(', ')}.`)}
          onClick={() => {
            void request.run(unprepared, () => api.components.groupAction('prepare', unprepared),
              { command: 'prepare_all', label: 'Preparing…', name: 'Prepare all' });
          }}>
          {busyLabel(request.operation, 'prepare_all', 'Prepare all')}
        </button>
        <button type="button" disabled={!enabled || request.pending || prepared.length === 0}
          title={busyReason(request.operation)
            ?? (prepared.length === 0 ? 'No component to enable.' : `Enable ${prepared.join(', ')}.`)}
          onClick={() => {
            void request.run(prepared, () => api.components.groupAction('enable', prepared),
              { command: 'enable_all', label: 'Enabling…', name: 'Enable all' });
          }}>
          {busyLabel(request.operation, 'enable_all', 'Enable all')}
        </button>
      </div>
      {request.error !== null && (
        <p className="inline-message severity-error" role="alert">{request.error}</p>
      )}
    </section>
  );
}

/** `id: summary`, then the components under it in parentheses (a member with its slot first). */
function componentSummaryLine(
  { item, children }: ComponentTreeNode<ComponentState>,
  states: readonly ComponentState[],
  nested = false,
): string {
  const name = nested && item.role !== undefined ? `${item.role} ${item.component_id}` : item.component_id;
  const line = `${name}: ${componentSummary(item, states)}`;
  return children.length === 0 ? line
    : `${line} (${children.map((child) => componentSummaryLine(child, states, true)).join(', ')})`;
}

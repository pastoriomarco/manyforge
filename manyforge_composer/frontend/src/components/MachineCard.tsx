import { useEffect, useState } from 'react';

import type { DeploymentWorkspaceController } from '../hooks/useDeploymentWorkspace';
import type { JsonValue } from '../types/api';
import type { ComponentProfileFacts, MachineCatalogEntry } from '../types/componentCatalog';
import type { ComponentState } from '../types/components';
import { componentRowText, withoutMachine, type ComponentManagerRow } from '../utils/componentManager';
import {
  actionLabel,
  actionResultText,
  ageText,
  componentTone,
  faultPresentation,
  LIFECYCLE_LABELS,
  missingComponentStateText,
  reasonText,
  streamLabel,
  TONE_BADGE_CLASSES,
} from '../state/componentPresentation';
import { ComponentActions, ComponentStatePanel, concernedOwnerFault } from './ComponentStatePanel';
import { CHANGE_LABELS } from './ComponentManager';

type Row = Record<string, JsonValue>;

function object(value: JsonValue | undefined): Row | null {
  return value !== null && value !== undefined && typeof value === 'object' && !Array.isArray(value)
    ? value : null;
}

/**
 * A machine is one card: the
 * header names it and its model with the machine-wide state; then the lane
 * and the address, the owner's state rows, faults and actions; then one row
 * per member (deactivate switch, name selecting its settings, state, inline
 * messages and actions), and Remove. No nested cards. Members
 * come from the machine's package and are always all present: any member may
 * be deactivated, none removed. Every state shown is the
 * pushed `ComponentState`; edits are Draft edits.
 */
export function MachineCard({
  owner, members, machine, profiles, controller, states, enabled, selectedId, onSelect, onRemoved,
}: {
  owner: ComponentManagerRow;
  members: readonly ComponentManagerRow[];
  /** The machine model's catalog entry (lanes, member slots); undefined until read. */
  machine: MachineCatalogEntry | undefined;
  profiles: Readonly<Record<string, ComponentProfileFacts>>;
  controller: DeploymentWorkspaceController;
  /** The cell's pushed states (null: not read yet). */
  states: readonly ComponentState[] | null;
  enabled: boolean;
  selectedId: string;
  onSelect: (componentId: string) => void;
  onRemoved: () => void;
}) {
  const document = controller.workspace?.draft.document ?? null;
  const rows = Array.isArray(document?.components) ? document.components : [];
  const indexOf = (id: string) => rows.findIndex((candidate) => object(candidate)?.component_id === id);
  const index = indexOf(owner.componentId);
  const component = index < 0 ? null : object(rows[index]);
  const disabled = !controller.mutationAvailable || controller.busy || controller.loading;
  const cell = states ?? [];
  const stateOf = (id: string) => cell.find((state) => state.component_id === id) ?? null;
  const ownerState = stateOf(owner.componentId);
  const lanes = machine?.lanes ?? (owner.profileId === null ? undefined : profiles[owner.profileId]?.lanes);
  const lane = typeof component?.lane === 'string' ? component.lane : null;
  const configuration = object(component?.configuration);
  const address = typeof configuration?.device_address_ref === 'string' ? configuration.device_address_ref : '';

  const setAddress = (value: string) => {
    if (value === '') {
      return address === '' ? null
        : controller.mutate(['components', index, 'configuration', 'device_address_ref'], 'remove');
    }
    return configuration === null
      ? controller.mutate(['components', index, 'configuration'], 'set', { device_address_ref: value })
      : controller.mutate(['components', index, 'configuration', 'device_address_ref'], 'set', value);
  };
  const removeMachine = async () => {
    if (document === null) return;
    // Attachments first, so the Draft never names a missing component; then
    // the owner and its members in one edit.
    const kept = withoutMachine(document, owner.componentId);
    const attachments = Array.isArray(document.attachments) ? document.attachments : [];
    if (kept.attachments.length !== attachments.length
      && await controller.mutate(['attachments'], 'set', kept.attachments) === null) return;
    if (await controller.mutate(['components'], 'set', kept.components) !== null) onRemoved();
  };

  return (
    <section className="dcard machine-card" aria-labelledby="machine-card-title">
      <header>
        <h3 id="machine-card-title">{owner.componentId}</h3>
        <small className="machine-card-model">
          {[machine?.title, owner.profileId ?? 'not in the Draft'].filter(Boolean).join(' · ')}
        </small>
        {ownerState !== null && (
          <span className={TONE_BADGE_CLASSES[componentTone(ownerState)]}>{LIFECYCLE_LABELS[ownerState.lifecycle]}</span>
        )}
      </header>
      {owner.change !== 'applied' && (
        <p className="inline-message severity-info">{CHANGE_LABELS[owner.change]}</p>
      )}
      {component !== null && (
        <div className="dgrid">
          <label className="deployment-focused-field stacked">
            <span>Lane</span>
            <select aria-label={`${owner.componentId} lane`} disabled={disabled || lanes === undefined || lanes.length === 0}
              value={lane ?? ''}
              onChange={(event) => { void controller.mutate(['components', index, 'lane'], 'set', event.target.value); }}>
              {(lane === null || !(lanes ?? []).includes(lane)) && <option value={lane ?? ''}>{lane ?? 'not set'}</option>}
              {(lanes ?? []).map((choice) => <option key={choice} value={choice}>{choice}</option>)}
            </select>
            <small>Every member runs on the machine&apos;s lane.</small>
          </label>
          <AddressField ownerId={owner.componentId} address={address} disabled={disabled} onCommit={setAddress} />
        </div>
      )}
      {ownerState === null
        ? <p className="muted">{missingComponentStateText(states !== null)}</p>
        : <ComponentStatePanel state={ownerState} enabled={enabled} states={cell} embedded />}
      {ownerState !== null && (ownerState.details ?? []).length > 0 && (
        <div className="component-state-rows" role="group" aria-label={`${owner.componentId} controller status`}>
          <h4 className="settings-subheading">Controller status</h4>
          <dl>
            {(ownerState.details ?? []).map((detail) => (
              <div key={detail.name}><dt>{detail.name}</dt><dd>{detail.value}</dd></div>
            ))}
          </dl>
          <p className="field-help">As the controller reports it; shown for diagnosis only.</p>
        </div>
      )}
      <h4 className="settings-subheading machine-members-heading">
        Members<small>Select a member for its settings</small>
      </h4>
      <div className="drows" aria-label={`${owner.componentId} members`}>
        {members.map((member) => (
          <MemberRow key={member.componentId} member={member} index={indexOf(member.componentId)}
            controller={controller} state={stateOf(member.componentId)} states={cell}
            statesKnown={states !== null} enabled={enabled}
            disabled={disabled} selected={member.componentId === selectedId} onSelect={onSelect} />
        ))}
      </div>
      {component !== null && (
        <div className="dactions">
          <button type="button" disabled={disabled} onClick={() => { void removeMachine(); }}>Remove machine from Draft</button>
        </div>
      )}
      <p className="field-help">
        Removing the machine removes its members too; Preview lists them. A member can&apos;t be removed on its
        own: a deactivated member stays in the Deployment, greyed, and is left out of the cell at the next Apply.
      </p>
    </section>
  );
}

/** A site-settings `device_addresses` key (portable id, as the Deployment compiler requires). */
const ADDRESS_KEY = /^[a-z][a-z0-9._-]{0,127}$/;

/**
 * The owner's device address reference, `configuration.device_address_ref`:
 * a key of the site settings' `device_addresses`, never the address itself
 * (the address is a machine fact of the site file).
 */
function AddressField({ ownerId, address, disabled, onCommit }: {
  ownerId: string;
  address: string;
  disabled: boolean;
  onCommit: (value: string) => unknown;
}) {
  const [text, setText] = useState(address);
  useEffect(() => { setText(address); }, [address]);
  const trimmed = text.trim();
  const problem = trimmed === '' || ADDRESS_KEY.test(trimmed) ? null
    : 'Use the key of an entry in the site settings device_addresses (lower-case letters, digits, ".", "_" or "-"), not the address itself.';
  return (
    <label className={`deployment-focused-field stacked${problem !== null ? ' invalid' : ''}`}>
      <span>Address (site settings key)</span>
      <input aria-label={`${ownerId} address`} value={text} disabled={disabled}
        placeholder="device_addresses key, e.g. lite6_main"
        aria-invalid={problem !== null}
        onChange={(event) => setText(event.target.value)}
        onBlur={() => { if (problem === null && trimmed !== address) void onCommit(trimmed); }} />
      {problem !== null
        ? <small className="deployment-field-error" role="alert">{problem}</small>
        : <small>The key in the site settings&apos; device_addresses whose address this machine connects to (real lane).</small>}
    </label>
  );
}

/**
 * One member as a house row inside its machine's card: the deactivate switch,
 * its name over kind and model (selects its settings below), its state; then
 * its messages inline (owner fault, fault, reason, progress, last result,
 * streams, controller status) and its actions.
 */
function MemberRow({
  member, index, controller, state, states, statesKnown, enabled, disabled, selected,
  onSelect,
}: {
  member: ComponentManagerRow;
  index: number;
  controller: DeploymentWorkspaceController;
  state: ComponentState | null;
  states: readonly ComponentState[];
  /** False while the cell's states can't be read (shown as unavailable). */
  statesKnown: boolean;
  enabled: boolean;
  disabled: boolean;
  selected: boolean;
  onSelect: (componentId: string) => void;
}) {
  const id = member.componentId;
  const setActive = (active: boolean) => (active
    ? controller.mutate(['components', index, 'active'], 'remove')
    : controller.mutate(['components', index, 'active'], 'set', false));
  const shown = member.active ? state : null;
  return (
    <div className={`machine-member${member.active ? '' : ' component-inactive'}`} role="group"
      aria-label={`${id} member`}>
      {member.change !== 'removed' && index >= 0 ? (
        <label className="toggle-control"
          title="Deactivated: it stays in the Deployment and is left out of the cell.">
          <input type="checkbox" aria-label={`${id} active`} checked={member.active}
            disabled={disabled}
            onChange={(event) => { void setActive(event.target.checked); }} />
        </label>
      ) : <span />}
      <button type="button" className={`dpick component-row${selected ? ' selected' : ''}`}
        aria-pressed={selected} title={selected ? 'Its settings are shown below.' : 'Show its settings below.'}
        onClick={() => onSelect(id)}>
        <strong>{id}</strong>
        <span><small>{componentRowText(member, true)}</small></span>
      </button>
      {!member.active
        ? <span className="status-pill status-pill-off">Off</span>
        : shown === null ? <span />
          : <span className={TONE_BADGE_CLASSES[componentTone(shown)]}>{LIFECYCLE_LABELS[shown.lifecycle]}</span>}
      <div className="machine-member-body">
        {!member.active ? (
          <p className="inline-message severity-info">Deactivated: left out of the cell at the next Apply.</p>
        ) : shown === null ? (
          <p className="inline-message severity-info">{missingComponentStateText(statesKnown)}</p>
        ) : (
          <MemberStatus state={shown} states={states} enabled={enabled} />
        )}
      </div>
    </div>
  );
}

/** A member's state as inline messages, then its actions (the same as its status card's). */
function MemberStatus({ state, states, enabled }: {
  state: ComponentState;
  states: readonly ComponentState[];
  enabled: boolean;
}) {
  const id = state.component_id;
  const fault = state.fault === undefined ? null : faultPresentation(state.fault);
  const concerned = concernedOwnerFault(state, states);
  const reason = reasonText(state, states);
  const details = state.details ?? [];
  return (
    <>
      {concerned !== null && (
        <p className="inline-message severity-error" role="alert">
          {concerned.ownerId}: {concerned.fault.title}. {concerned.fault.detail}
          {concerned.fault.operatorAction !== null
            && ` Operator action: ${concerned.fault.operatorAction} on ${concerned.ownerId}`}
        </p>
      )}
      {fault !== null && (
        <p className="inline-message severity-error" role="alert">
          {fault.title}. {fault.detail}
          {fault.vendor !== null && ` ${fault.vendor}.`}
          {fault.operatorAction !== null && ` Operator action: ${fault.operatorAction}`}
        </p>
      )}
      {reason !== null && <p className="inline-message severity-warning">{reason}</p>}
      {state.action !== undefined && (
        <p className="inline-message severity-info">{actionLabel(state.action.action)} in progress</p>
      )}
      {state.activity !== 'idle' && state.action === undefined && (
        <p className="inline-message severity-info">Activity: {state.activity}</p>
      )}
      {state.last_action !== undefined && (
        <p className={`inline-message severity-${state.last_action.outcome === 'succeeded' ? 'info' : 'error'}`}>
          Last action: {actionResultText(state.last_action)}
        </p>
      )}
      {state.streams.length > 0 && (
        <p className="inline-message severity-info" aria-label={`${id} streams`}>
          {state.streams.map((stream) => `${streamLabel(stream.stream_id)}: ${stream.freshness}`
            + (stream.age_ns === undefined ? '' : ` · ${ageText(stream.age_ns)}`)).join(' · ')}
        </p>
      )}
      {details.length > 0 && (
        <p className="inline-message severity-info" aria-label={`${id} controller status`}
          title="As the controller reports it; shown for diagnosis only.">
          {details.map((detail) => `${detail.name} ${detail.value}`).join(' · ')}
        </p>
      )}
      <ComponentActions state={state} enabled={enabled} states={states} className="dactions machine-member-actions" />
    </>
  );
}

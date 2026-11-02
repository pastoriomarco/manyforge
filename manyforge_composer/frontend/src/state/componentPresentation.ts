// Presentation of the backend's component, latch, readiness, run-end and I/O
// facts (R4): labels and catalogue texts only. Nothing here decides health,
// severity of a component or whether something is allowed.
import type {
  ActionResult,
  CellLatch,
  CodedError,
  ComponentLifecycle,
  ComponentState,
  Fault,
  ProgramRunEnd,
  RunBlockedReason,
  SignalSample,
  SignalSource,
} from '../types/components';
import { LIFECYCLE_ACTIONS, OWNER_UNAVAILABLE } from '../types/components';
import { errorCatalogRow } from './commandRefusal';

export type PresentationTone = 'ok' | 'neutral' | 'warning' | 'error';

export const LIFECYCLE_LABELS: Readonly<Record<ComponentLifecycle, string>> = {
  offline: 'Offline',
  not_prepared: 'Not prepared',
  prepared: 'Prepared',
  enabled: 'Enabled',
  fault: 'Fault',
};

const ACTION_LABELS: Readonly<Record<string, string>> = {
  prepare: 'Prepare',
  enable: 'Enable',
  disable: 'Disable',
  recover: 'Recover',
  reset_position: 'Reset position',
  power_off: 'Power off',
  reboot: 'Reboot',
  sim_reset: 'Reset simulation',
};

/** Capabilities that are behaviours, not actions (CONTRACTS §3). */
const BEHAVIOUR_CAPABILITIES = new Set(['io_manual_write', 'io_output_readback']);

export function actionLabel(action: string): string {
  return ACTION_LABELS[action] ?? action.replaceAll('_', ' ');
}

const ACTION_BUSY_LABELS: Readonly<Record<string, string>> = {
  prepare: 'Preparing…',
  enable: 'Enabling…',
  disable: 'Disabling…',
  recover: 'Recovering…',
  reset_position: 'Resetting position…',
  power_off: 'Powering off…',
  reboot: 'Rebooting…',
  sim_reset: 'Resetting simulation…',
};

/** The busy label of an action's button while its request runs. */
export function actionBusyLabel(action: string): string {
  return ACTION_BUSY_LABELS[action] ?? `${actionLabel(action)}…`;
}

/** Actions the operator confirms first: an owner's power actions (§16). */
export const CONFIRMED_ACTIONS: ReadonlySet<string> = new Set(['power_off', 'reboot']);

function listText(items: readonly string[]): string {
  return items.length < 2 ? items.join('') : `${items.slice(0, -1).join(', ')} and ${items.at(-1)}`;
}

/**
 * The confirmation of a power action (CONTRACTS §2, §16): the host disables
 * the machine's members first, then shuts its controller down or restarts it
 * (a reboot prepares the machine again once its controller answers).
 */
export function confirmedActionText(action: string, state: ComponentState,
  states: readonly ComponentState[]): { title: string; body: string[] } {
  const id = state.component_id;
  const enabled = states.filter((member) => member.owner_id === id && member.lifecycle === 'enabled')
    .map((member) => member.component_id);
  const first = enabled.length === 0 ? '' : ` disables ${listText(enabled)} first, then`;
  if (action === 'reboot') {
    return { title: `Reboot ${id}?`, body: [
      `Reboot${first} restarts the controller of ${id}.`,
      `Once the controller answers again, ${id} is prepared; its members stay disabled until they are enabled again.`,
    ] };
  }
  return { title: `Power off ${id}?`, body: [
    `Power off${first} shuts down the controller of ${id}.`,
    `It stays off until it is switched on again at the machine; then Prepare ${id}.`,
  ] };
}

/** §16: the components some other component names as its owner (the machines). */
export function ownerIds(states: readonly ComponentState[]): Set<string> {
  return new Set(states.flatMap((state) => (state.owner_id === undefined ? [] : [state.owner_id])));
}

/**
 * CONTRACTS §2: the four lifecycle actions plus the declared action
 * capabilities; an owner (§16) offers Prepare and Recover, no Enable/Disable.
 * An enabled component isn't offered Prepare: the host refuses it
 * (`component_enabled`); Disable takes it back to prepared.
 */
export function offeredActions(state: ComponentState, owner = false): string[] {
  const lifecycle = (owner ? ['prepare', 'recover'] : [...LIFECYCLE_ACTIONS])
    .filter((action) => action !== 'prepare' || state.lifecycle !== 'enabled');
  return [...lifecycle,
    ...state.capabilities.filter((capability) => !BEHAVIOUR_CAPABILITIES.has(capability))];
}

/** One component of the cell's tree with the components under it. */
export interface ComponentTreeNode<T> {
  item: T;
  children: ComponentTreeNode<T>[];
}

/**
 * The items as one tree, in the given order: a machine's members under their
 * owner (§16), a mounted component under the component it is mounted on (the
 * owner wins when an item has both). An item whose parent isn't listed stays
 * at the top.
 */
export function componentTree<T>(
  items: readonly T[],
  idOf: (item: T) => string,
  parentOf: (item: T) => string | null,
): ComponentTreeNode<T>[] {
  const ids = new Set(items.map(idOf));
  const parent = (item: T) => {
    const id = parentOf(item);
    return id !== null && ids.has(id) ? id : null;
  };
  const under = (parentId: string | null): ComponentTreeNode<T>[] => items
    .filter((item) => parent(item) === parentId)
    .map((item) => ({ item, children: under(idOf(item)) }));
  return under(null);
}

/** The tree in display order, each item with its depth (0 at the top). */
export function flattenComponentTree<T>(
  nodes: readonly ComponentTreeNode<T>[],
  depth = 0,
): Array<{ item: T; depth: number }> {
  return nodes.flatMap((node) => [{ item: node.item, depth }, ...flattenComponentTree(node.children, depth + 1)]);
}

/**
 * The cell's states as one tree: members under their owner, a mounted
 * component under its parent (`mounts`: child id to parent id, from the
 * Deployment's `attachments[]`).
 */
export function componentStateTree(
  states: readonly ComponentState[],
  mounts: Readonly<Record<string, string>> = {},
): ComponentTreeNode<ComponentState>[] {
  return componentTree(states, (state) => state.component_id,
    (state) => state.owner_id ?? mounts[state.component_id] ?? null);
}

/** Prepare all (§2): every `not_prepared` or `offline` component, in cell order. */
export function prepareAllTargets(states: readonly ComponentState[]): string[] {
  return states.filter((state) => state.lifecycle === 'not_prepared' || state.lifecycle === 'offline')
    .map((state) => state.component_id);
}

/** Enable all (§2): every `prepared` member and owner-less component, never an owner. */
export function enableAllTargets(states: readonly ComponentState[]): string[] {
  const owners = ownerIds(states);
  return states.filter((state) => state.lifecycle === 'prepared' && !owners.has(state.component_id))
    .map((state) => state.component_id);
}

/** The catalogue's compact text for a typed error; its own message without a row. */
export function codedErrorText(error: CodedError): string {
  return errorCatalogRow(error.code)?.[0] ?? (error.message || error.code);
}

/** The catalogue's full text (the operator action), then the producer's message. */
export function codedErrorDetail(error: CodedError): string {
  const full = errorCatalogRow(error.code)?.[1] ?? '';
  if (full === '') return error.message;
  return error.message === '' || full.includes(error.message) ? full : `${full} ${error.message}`;
}

export interface FaultPresentation {
  title: string;
  detail: string;
  vendor: string | null;
  /** The action the fault names for the operator; null for `none`. */
  operatorAction: string | null;
}

export function faultPresentation(fault: Fault): FaultPresentation {
  const vendor = [fault.vendor_code, fault.vendor_text].filter((part) => part !== undefined && part !== '');
  return {
    title: codedErrorText(fault),
    detail: codedErrorDetail(fault),
    vendor: vendor.length === 0 ? null : `Vendor: ${vendor.join(' · ')}`,
    operatorAction: fault.operator_action === 'none' ? null : actionLabel(fault.operator_action),
  };
}

/** Colour of a component's state: red for a fault, yellow while it can't report. */
export function componentTone(state: ComponentState): PresentationTone {
  if (state.lifecycle === 'fault') return 'error';
  if (state.lifecycle === 'offline' || state.activity === 'unknown') return 'warning';
  return state.lifecycle === 'enabled' ? 'ok' : 'neutral';
}

/**
 * Why a component is offline or not prepared, or why an enabled simulated
 * one doesn't move (`simulation_not_playing`). A member offline through its
 * owner (§16) shows the owner's own fault or reason, read from the owner's
 * state when `states` has it, else the producer's "through <owner>: ..." text.
 */
export function reasonText(state: ComponentState, states: readonly ComponentState[] = []): string | null {
  if (state.reason === undefined) return null;
  if (state.reason.code !== OWNER_UNAVAILABLE || state.owner_id === undefined) return codedErrorText(state.reason);
  const owner = states.find((candidate) => candidate.component_id === state.owner_id);
  if (owner === undefined) return state.reason.message || codedErrorText(state.reason);
  const cause = owner.fault !== undefined ? codedErrorText(owner.fault)
    : owner.reason !== undefined ? codedErrorText(owner.reason) : LIFECYCLE_LABELS[owner.lifecycle];
  return `through ${owner.component_id}: ${cause}`;
}

/** A component's fault, reason or action in progress, after its state; null when none. */
export function componentStateDetail(state: ComponentState, states: readonly ComponentState[] = []): string | null {
  const parts: string[] = [];
  const reason = reasonText(state, states);
  if (state.fault !== undefined) parts.push(codedErrorText(state.fault));
  else if (reason !== null) parts.push(reason);
  if (state.action !== undefined) parts.push(`${actionLabel(state.action.action)} in progress`);
  else if (state.activity !== 'idle') parts.push(state.activity === 'busy' ? 'busy' : 'activity unknown');
  return parts.length === 0 ? null : parts.join(' · ');
}

/** One line for a component: state, then its fault, reason or action in progress. */
export function componentSummary(state: ComponentState, states: readonly ComponentState[] = []): string {
  const detail = componentStateDetail(state, states);
  return detail === null ? LIFECYCLE_LABELS[state.lifecycle] : `${LIFECYCLE_LABELS[state.lifecycle]} · ${detail}`;
}

/** The state badge (`status-pill`) classes of each tone. */
export const TONE_BADGE_CLASSES: Readonly<Record<PresentationTone, string>> = {
  ok: 'status-pill status-pill-connected',
  neutral: 'status-pill',
  warning: 'status-pill status-pill-danger',
  error: 'status-pill status-pill-error',
};

export function actionResultText(result: ActionResult): string {
  const name = actionLabel(result.action);
  if (result.outcome === 'succeeded') return `${name} succeeded`;
  if (result.outcome === 'unknown') return `${name}: outcome unknown (not resent)`;
  return result.error === undefined ? `${name} failed` : `${name} failed: ${codedErrorText(result.error)}`;
}

/**
 * CONTRACTS §5 cause kinds, as the operator reads them. A machine is one
 * cause (§16): its fault names the members it concerns.
 */
export function latchCauseText(cause: CellLatch['causes'][number]): string {
  if (cause.kind === 'fault') {
    const concerns = cause.fault?.concerns ?? [];
    return `${cause.component_id}: ${cause.fault === undefined ? 'fault' : codedErrorText(cause.fault)}`
      + (concerns.length === 0 ? '' : ` (concerns ${concerns.join(', ')})`);
  }
  if (cause.kind === 'offline') return `${cause.component_id}: offline`;
  return `${cause.component_id}: can't report whether it is still moving`;
}

/** "N errors require attention · <leading cause>", or the single cause. */
export function latchSummary(latch: CellLatch): string | null {
  if (!latch.latched || latch.causes.length === 0) return null;
  const leading = latchCauseText(latch.causes[0]!);
  return latch.causes.length === 1 ? leading : `${latch.causes.length} errors require attention · ${leading}`;
}

/** One Execute/Resume blocked reason, with the ids it names. */
export function blockedReasonText(reason: RunBlockedReason): string {
  const subject = reason.component_id ?? reason.rule_id ?? reason.signal_id;
  const text = codedErrorText(reason);
  return subject === undefined ? text : `${subject}: ${text}`;
}

/**
 * The `rule_holds` reason gating motion of these components (§8), as the
 * backend's `rule_hold_meets_program`: `everything` always; `all_motion`
 * when they move anything; execution groups when they share a component.
 */
export function ruleHoldMeeting(
  reasons: readonly RunBlockedReason[],
  motionComponentIds: readonly string[],
): RunBlockedReason | null {
  return reasons.find((reason) => reason.code === 'rule_holds' && (
    reason.scope === 'all_motion' ? motionComponentIds.length > 0
      : reason.scope === 'execution_groups'
        ? (reason.component_ids ?? []).some((id) => motionComponentIds.includes(id))
        : true)) ?? null;
}

export interface RunEndPresentation {
  label: string;
  tone: PresentationTone;
  text: string;
}

/** CONTRACTS §9: why a run ended (plan, "Program outcomes"). */
export function runEndPresentation(end: ProgramRunEnd): RunEndPresentation {
  const { details } = end;
  const error = details.error === undefined ? null : codedErrorText(details.error);
  switch (end.end_reason) {
    case 'completed':
      return { label: 'Completed', tone: 'neutral', text: 'Completed' };
    case 'failed': {
      const node = details.node_instance_id === undefined ? '' : ` at ${details.node_instance_id}`;
      return { label: 'Ended by failure', tone: 'warning',
        text: `Ended by failure${node}${error === null ? '' : `: ${error}`}` };
    }
    case 'stopped': {
      const who = details.stopped_by === 'rule'
        ? `rule ${details.rule_id ?? ''}`.trim() : 'the operator';
      return { label: 'Stopped', tone: 'warning', text: `Stopped by ${who}` };
    }
    case 'cell_fault': {
      const component = details.component_id ?? 'a component';
      return { label: 'Ended by a cell fault', tone: 'error',
        text: `Ended by a cell fault: ${component}${error === null ? '' : ` (${error})`}` };
    }
  }
}

export const SIGNAL_SOURCE_LABELS: Readonly<Record<SignalSource, string>> = {
  device: 'device',
  manyforge_write: 'ManyForge',
  outside: 'outside',
  last_write: 'last write',
};

export function signalValueText(sample: SignalSample): string {
  if (sample.value === null) return 'unknown';
  if (typeof sample.value === 'boolean') return sample.value ? 'on' : 'off';
  return String(sample.value);
}

/** A sample's age for display: ms below a second, seconds above. */
export function ageText(ageNs: number): string {
  const ms = ageNs / 1e6;
  return ms < 1000 ? `${Math.round(ms)} ms` : `${(ms / 1000).toFixed(1)} s`;
}

/**
 * Shown instead of last values while the backend can't read the execution
 * host's component model (the pushed cell facts are null).
 */
export const COMPONENT_STATES_UNAVAILABLE = "Component states unavailable: the execution host's state can't be read.";

/** The status line of a component without a pushed state. */
export function missingComponentStateText(statesKnown: boolean): string {
  return statesKnown ? 'Not in the cell: the next Apply brings it up.' : COMPONENT_STATES_UNAVAILABLE;
}

const STREAM_LABELS: Readonly<Record<string, string>> = {
  measured_joints: 'Measured joints',
  gripper_feedback: 'Gripper feedback',
  endpoint: 'Model endpoint',
  inputs: 'Inputs',
};

/** Short label of one declared stream id; pose-source ids are `poses/<object>/<policy>`. */
export function streamLabel(streamId: string): string {
  const pose = /^poses\/(.+)\/([^/]+)$/.exec(streamId);
  if (pose) return `Pose ${pose[1]} / ${pose[2]}`;
  const objectPose = /^poses\/([^/]+)$/.exec(streamId);
  if (objectPose) return `Pose ${objectPose[1]}`;
  return STREAM_LABELS[streamId] ?? streamId;
}

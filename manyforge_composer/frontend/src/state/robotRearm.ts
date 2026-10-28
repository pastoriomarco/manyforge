/**
 * m5.5 (UFACTORY, maintainer 2026-10-28): one press for Execute, Run, jog and
 * Move to after the robot was moved from its own interface (UFACTORY Studio)
 * on an idle cell. Headless refuses the motion command
 * `component_rearm_required` with no effect; Composer then re-arms only the
 * named robots (the component's own CCLC Recover; the target driver re-drives
 * Prepare and Enable: servo mode, ready state, the trajectory controller
 * holding the current pose) and waits until each one is active again.
 *
 * - The re-arm is narrow (FIX6): it never acknowledges an incident or
 *   recovers the cell Stop latch. Anything else wrong (an open incident, a
 *   closed cell admission, no moved-externally warning) stops with a typed
 *   reason so the operator uses Recover cell explicitly.
 * - Only a command resolved against the current state at execution is sent
 *   again (`REARM_RESEND_KINDS`: Program Run). A Robot Control operation
 *   (jog, Move to, Execute of a preview) was computed before the manual move:
 *   after the re-arm Composer stops and asks for a new preview.
 * - The pending intent is cancelled by any later command (Stop, Reset, a
 *   newer motion command) or by Close: `isCurrent` turns false and nothing is
 *   re-sent. The Stop lane is never delayed: a Stop is its own command.
 * - A robot still moving under its own controller, a refused or failed
 *   re-arm, or one not active within the bounded wait stops with the reason:
 *   nothing moves.
 */
import type { CCLCAdmission, CCLCComponentSnapshot } from '../types/cclc';

export const COMPONENT_REARM_REQUIRED = 'component_rearm_required';
/** The command kinds Headless refuses while a robot needs re-arming. */
export const REARM_COMMAND_KINDS: ReadonlySet<string> = new Set(['operation_start', 'program_run']);
/** The kinds re-sent after the re-arm: resolved at execution, not before. */
export const REARM_RESEND_KINDS: ReadonlySet<string> = new Set(['program_run']);
/** Bounded wait for the re-armed components to be active again. */
export const REARM_WAIT_MS = 15_000;
export const REARM_POLL_MS = 250;
const MOVED_EXTERNALLY_PREFIX = 'moved_externally:';
export const REARMED_PREVIEW_AGAIN =
  'Robot re-armed. The target was computed before the manual move; preview again.';
export const REARM_CANCELLED = 'Cancelled: the waiting command was not sent.';
/** Cancelled before any re-arm Recover was submitted. */
export const REARM_CANCELLED_NOTHING_STARTED = `${REARM_CANCELLED} No re-arm was started.`;
/** The notice Cancel's explanation (FIX7). */
export const REARM_CANCEL_TITLE =
  'Cancel stops the waiting command; a re-arm step already sent may still take effect.';

/** The re-arm's Recover acknowledged an incident or stream latch (FIX7). */
export class RearmAcknowledgedIncidentError extends Error {
  constructor(cause: string) {
    super(`An incident (${cause}) occurred during the re-arm and was acknowledged. `
      + 'Check the robot, then press Run again.');
    this.name = 'RearmAcknowledgedIncidentError';
  }
}

/** The re-arm's Recover reply was lost or unreadable (FIX7, SX-488). */
export class RearmOutcomeUnknownError extends Error {
  constructor(options?: { cause?: unknown }) {
    super('The re-arm outcome is unknown: the robot may have been recovered and an incident '
      + "may have been acknowledged. Check the robot's lifecycle state and incidents "
      + '(Components) before running again.', options);
    this.name = 'RearmOutcomeUnknownError';
  }
}

/** The owner reads and the one write of the narrow re-arm. */
export interface NarrowRearmPorts<Identity = unknown, Owner = unknown> {
  identity: () => Promise<Identity>;
  owner: () => Promise<Owner>;
  component: (controllableComponentId: string) => Promise<CCLCComponentSnapshot>;
  /** The cell's current authoring admission, or null when unknown. */
  admission: () => { authoringAllowed: boolean; admissionGeneration: number } | null;
  /** The fenced operator `recover` of exactly this snapshot. */
  submit: (
    snapshot: CCLCComponentSnapshot,
    identity: Identity,
    owner: Owner,
    admissionGeneration: number,
  ) => Promise<CCLCAdmission>;
}

/**
 * One moved robot's component Recover. The cancel token is checked after each
 * read and right before the Recover: a cancelled intent sends nothing. Anything
 * else needing the operator is refused before it. A Recover answered by
 * acknowledging an incident or stream latch, or whose reply is lost, stops the
 * sequence: nothing is re-sent.
 */
export async function narrowRearm<Identity, Owner>(
  ports: NarrowRearmPorts<Identity, Owner>,
  controllableComponentId: string,
  isCurrent: () => boolean,
): Promise<void> {
  // Every cancellation here precedes this component's Recover.
  const current = () => { if (!isCurrent()) throw new Error(REARM_CANCELLED_NOTHING_STARTED); };
  current();
  const identity = await ports.identity();
  current();
  const owner = await ports.owner();
  current();
  const snapshot = await ports.component(controllableComponentId);
  current();
  const admission = ports.admission();
  const refusal = narrowRearmRefusal(snapshot, admission?.authoringAllowed === true);
  if (refusal !== null || admission === null) {
    throw new Error(`The robot was not re-armed: ${refusal ?? 'the cell admission is unknown.'}`);
  }
  current();
  let result: CCLCAdmission;
  try {
    result = await ports.submit(snapshot, identity, owner, admission.admissionGeneration);
  } catch (error: unknown) {
    // Sent, then lost, timed out or undecodable: never a known no-effect.
    throw new RearmOutcomeUnknownError({ cause: error });
  }
  if (result.incidentRecovery !== null) {
    throw new RearmAcknowledgedIncidentError(result.incidentRecovery.causeClass);
  }
  if (result.streamAcknowledgement !== undefined) {
    throw new RearmAcknowledgedIncidentError('stream condition');
  }
  if (result.transactionId === null) {
    throw new Error(`The robot was not re-armed: the Recover of ${controllableComponentId} `
      + `was ${result.replayStatus}; use Recover cell.`);
  }
}

/**
 * The pending-intent generation (FIX6): every command except runtime scale
 * (Stop, Full Stop, Reset, a newer motion command), Close and the notice's
 * Cancel supersede a motion command waiting for its re-arm. `issue` runs
 * synchronously before the command is sent, so a Stop is never delayed.
 */
export class MotionIntentGate {
  private generation = 0;
  private enabled = true;

  issue(commandKind: string): () => boolean {
    if (commandKind !== 'runtime_scale') this.generation += 1;
    const mine = this.generation;
    return () => this.enabled && this.generation === mine;
  }

  cancel(): void {
    this.generation += 1;
  }

  setEnabled(enabled: boolean): void {
    this.enabled = enabled;
    if (!enabled) this.generation += 1;
  }
}

/** Headless refused a motion command until the named robots are re-armed. */
export class ComponentRearmRequiredError extends Error {
  readonly components: string[];
  readonly inMotion: string[];

  constructor(message: string, components: string[], inMotion: string[]) {
    super(message);
    this.name = 'ComponentRearmRequiredError';
    this.components = components;
    this.inMotion = inMotion;
  }
}

function listDetail(details: unknown, key: string): string[] {
  if (details === null || typeof details !== 'object' || Array.isArray(details)) return [];
  const value = (details as Record<string, unknown>)[key];
  return typeof value === 'string' && value !== '' ? value.split(',') : [];
}

/**
 * The typed refusal from a rejected receipt's owner result
 * (`{ok: false, error: {code, message, details}}`), or null for any other.
 */
export function rearmRequiredFromResult(
  code: string,
  result: unknown,
  message: string,
): ComponentRearmRequiredError | null {
  if (code !== COMPONENT_REARM_REQUIRED) return null;
  const record = result !== null && typeof result === 'object' && !Array.isArray(result)
    ? result as Record<string, unknown> : null;
  const error = record?.error !== null && typeof record?.error === 'object'
    && !Array.isArray(record?.error) ? record?.error as Record<string, unknown> : null;
  const details = error?.details ?? null;
  return new ComponentRearmRequiredError(
    message,
    listDetail(details, 'components'),
    listDetail(details, 'in_motion'),
  );
}

export interface RobotRearmPorts {
  /**
   * The narrow re-arm of one moved robot: its component Recover. It throws a
   * typed reason when anything else needs the operator (see `rearmRecover`).
   */
  rearmComponent: (controllableComponentId: string, isCurrent: () => boolean) => Promise<void>;
  component: (controllableComponentId: string) => Promise<CCLCComponentSnapshot>;
  sleep: (ms: number) => Promise<void>;
  now: () => number;
  /** The visible state: "Re-arming robot…" while it runs, null after. */
  onStatus: (status: string | null) => void;
}

export function movedExternally(snapshot: CCLCComponentSnapshot): boolean {
  return (snapshot.lifecycleProgress?.blockingReason ?? '').startsWith(MOVED_EXTERNALLY_PREFIX);
}

function rearmed(snapshot: CCLCComponentSnapshot): boolean {
  const progress = snapshot.lifecycleProgress;
  return snapshot.lifecycleState === 'active'
    && progress !== null
    && progress.phase === 'satisfied'
    && !movedExternally(snapshot);
}

/**
 * Whether one moved robot may be re-armed by the narrow path, or the typed
 * reason the operator must use Recover cell (or wait) instead.
 */
export function narrowRearmRefusal(
  snapshot: CCLCComponentSnapshot,
  authoringAllowed: boolean,
): string | null {
  const id = snapshot.controllableComponentId;
  if (snapshot.lifecycleProgress?.incident) {
    return `${id} has an open incident; use Recover cell.`;
  }
  if (!movedExternally(snapshot)) {
    return `${id} is not waiting for a re-arm (${snapshot.lifecycleProgress?.blockingReason ?? snapshot.lifecycleState}); use Recover cell.`;
  }
  if (snapshot.lifecycleProgress?.blockingReason === 'moved_externally:in_motion') {
    return `${id} is still moving under its own controller; wait until it stops.`;
  }
  if (!authoringAllowed) {
    return 'The cell does not admit the re-arm (a Stop, a Program or another operation); use Recover cell.';
  }
  return null;
}

/** Re-arm each component, then wait (bounded) until every one is active. */
export async function rearmComponents(
  ports: RobotRearmPorts,
  components: string[],
  isCurrent: () => boolean = () => true,
): Promise<void> {
  let started = false;
  for (const id of components) {
    try {
      if (!isCurrent()) throw new Error(REARM_CANCELLED_NOTHING_STARTED);
      await ports.rearmComponent(id, isCurrent);
    } catch (error: unknown) {
      // An earlier component's Recover was sent: do not claim none started.
      if (started && error instanceof Error && error.message === REARM_CANCELLED_NOTHING_STARTED) {
        throw new Error(REARM_CANCELLED);
      }
      throw error;
    }
    started = true;
  }
  const deadline = ports.now() + REARM_WAIT_MS;
  let pending = [...components];
  while (pending.length > 0) {
    if (!isCurrent()) throw new Error(REARM_CANCELLED);
    const snapshots = await Promise.all(pending.map((id) => ports.component(id)));
    pending = snapshots.filter((snapshot) => !rearmed(snapshot))
      .map((snapshot) => snapshot.controllableComponentId);
    if (pending.length === 0) return;
    if (ports.now() >= deadline) {
      throw new Error(
        `Re-arming the robot did not finish within ${REARM_WAIT_MS / 1000} s (${pending.join(', ')}).`,
      );
    }
    await ports.sleep(REARM_POLL_MS);
  }
}

/**
 * Send a motion command; on `component_rearm_required` re-arm the robots and,
 * for a resendable kind still current, send it once more (`send` rebuilds it
 * with the then-current fences).
 */
export async function sendWithRearm<T>(
  commandKind: string,
  send: () => Promise<T>,
  ports: RobotRearmPorts,
  isCurrent: () => boolean,
): Promise<T> {
  try {
    return await send();
  } catch (error: unknown) {
    if (!(error instanceof ComponentRearmRequiredError) || !REARM_COMMAND_KINDS.has(commandKind)) {
      throw error;
    }
    if (error.inMotion.length > 0) {
      throw new Error(
        `The robot is still moving under its own controller (${error.inMotion.join(', ')}); `
        + 'wait until it stops, then press again.',
      );
    }
    ports.onStatus('Re-arming robot…');
    try {
      await rearmComponents(ports, error.components, isCurrent);
    } finally {
      ports.onStatus(null);
    }
    if (!isCurrent()) throw new Error(REARM_CANCELLED);
    if (!REARM_RESEND_KINDS.has(commandKind)) throw new Error(REARMED_PREVIEW_AGAIN);
    return send();
  }
}

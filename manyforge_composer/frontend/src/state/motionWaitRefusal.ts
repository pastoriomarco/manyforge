/**
 * Contract 75: motion admission waits while a global interrupt rule's source
 * is not ready yet (`global_interrupt_source_not_ready`, details {rule_id}).
 * A gate rule (`on_trip: gate`) refuses motion in its scope while its
 * condition holds (`global_interrupt_gated`, details {rule_id}) and allows it
 * again when the condition clears. Nothing failed and no alarm is raised:
 * Composer shows both as an amber notice (warning), never as an operational
 * error. The owner may carry the reason as the code or as `details.reason`.
 * A source wait also names the first signal without a fresh reading
 * (`signal_component_id`, `signal_name`, optional `source_id`). The text
 * names the refusal's own source id; the Applied Deployment only labels it
 * (ROS topic, xArm pin, ...) when it declares that source for the signal.
 */

export const GLOBAL_INTERRUPT_SOURCE_NOT_READY = 'global_interrupt_source_not_ready';
export const GLOBAL_INTERRUPT_GATED = 'global_interrupt_gated';

const PREFIX = 'Motion waits: ';

/** Declared source of one input signal from this source id, e.g. `ROS topic /door`; null if not declared so. */
export type IoInputSourceDescriber = (componentId: string, signalName: string, sourceId: string) => string | null;

// Set by the shell from the Applied Deployment (null when it is not known).
let describeInputSource: IoInputSourceDescriber | null = null;

export function setMotionWaitSourceDescriber(describer: IoInputSourceDescriber | null): void {
  describeInputSource = describer;
}

const text = (value: unknown): string | null => (typeof value === 'string' && value !== '' ? value : null);

function waitingSignal(details: Readonly<Record<string, unknown>> | null | undefined): string | null {
  const component = text(details?.signal_component_id);
  const name = text(details?.signal_name);
  if (component === null || name === null) return null;
  const signal = `signal ${name} of ${component}`;
  const sourceId = text(details?.source_id);
  if (sourceId === null) return signal;
  const source = describeInputSource?.(component, name, sourceId) ?? null;
  return `${signal} (source ${sourceId}${source === null ? '' : `: ${source}`})`;
}

/** The readable refusal for this code, or null for any other code. */
export function motionWaitRefusalMessage(
  code: string | null | undefined,
  details?: Readonly<Record<string, unknown>> | null,
): string | null {
  const reason = code === GLOBAL_INTERRUPT_SOURCE_NOT_READY || code === GLOBAL_INTERRUPT_GATED
    ? code : details?.reason;
  if (reason !== GLOBAL_INTERRUPT_SOURCE_NOT_READY && reason !== GLOBAL_INTERRUPT_GATED) return null;
  const ruleId = typeof details?.rule_id === 'string' && details.rule_id !== '' ? details.rule_id : null;
  const rule = ruleId === null ? 'a global interrupt rule' : `global interrupt rule ${ruleId}`;
  if (reason === GLOBAL_INTERRUPT_GATED) {
    return `${PREFIX}${rule} is active. Motion is allowed again when its condition clears.`;
  }
  const signal = waitingSignal(details);
  return signal === null
    ? `${PREFIX}${rule} is waiting for its source. Try again once the source reports.`
    : `${PREFIX}${rule} is waiting for ${signal}, which has no fresh reading yet. Try again once it reports.`;
}

/** A presented message that is this wait, not a failure. */
export function isMotionWaitRefusal(message: string | null | undefined): boolean {
  return typeof message === 'string' && message.startsWith(PREFIX);
}

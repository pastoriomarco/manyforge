import type { CycleStatus } from '../types/api';

export const CYCLE_STATES = [
  'idle',
  'running',
  'stopping',
  'stopped',
  'direct_control',
] as const satisfies readonly CycleStatus['state'][];

const CYCLE_STATE_SET = new Set<string>(CYCLE_STATES);

export function parseCycleState(value: unknown): CycleStatus['state'] {
  if (typeof value === 'string' && CYCLE_STATE_SET.has(value)) {
    return value as CycleStatus['state'];
  }
  throw new Error(`Unknown cycle state: ${String(value)}`);
}

export function coerceCycleStateFailClosed(
  value: unknown,
): { state: CycleStatus['state']; error: string | null } {
  try {
    return { state: parseCycleState(value), error: null };
  } catch (error) {
    const message = error instanceof Error ? error.message : 'Unknown cycle state';
    console.error(message);
    // Unknown backend states must not leave editing or robot commands unlocked.
    return { state: 'running', error: message };
  }
}

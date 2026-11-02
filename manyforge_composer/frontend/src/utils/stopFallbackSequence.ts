import type { JsonValue } from '../types/api';

/**
 * The arm's controlled-Stop fallback sequence (one place for the contract;
 * backend twin: manyforge_composer/backend/stop_fallback_sequence.py).
 *
 * Modes are ordered by certainty. The authored `stop_mode` is the first step;
 * `fallback_stop_modes` lists, in this fixed order and without duplicates,
 * modes strictly below it, tried when the previous step cannot be carried out.
 * `hard_deceleration` is always the final step and is never listed. Absent
 * means [] (primary, then hard). A compiled v1 / legacy
 * `recoverable_substitution_mode: spring_back_hold` (soft_on_path only) reads
 * as [spring_back_hold]; a document carrying both fields is ambiguous.
 */
export const FALLBACK_STOP_MODES_FIELD = 'fallback_stop_modes';
export const STOP_MODE_ORDER = [
  'soft_on_path', 'spring_back_hold', 'spring_back_fast', 'hard_deceleration',
] as const;
export type FallbackStopMode = 'spring_back_hold' | 'spring_back_fast';
const FALLBACK_MODES: readonly FallbackStopMode[] = ['spring_back_hold', 'spring_back_fast'];

/** Modes that may follow `primary` as fallbacks, in the fixed order. */
export function fallbackCandidates(primary: string): FallbackStopMode[] {
  const index = (STOP_MODE_ORDER as readonly string[]).indexOf(primary);
  if (index < 0) return [];
  return FALLBACK_MODES.filter((mode) => STOP_MODE_ORDER.indexOf(mode) > index);
}

/** Keep only modes valid below `primary`, deduplicated, in the fixed order. */
export function pruneFallbacks(primary: string, modes: readonly string[]): FallbackStopMode[] {
  return fallbackCandidates(primary).filter((mode) => modes.includes(mode));
}

/** Enable or disable one fallback, keeping the fixed order. */
export function toggleFallback(
  primary: string, modes: readonly string[], mode: FallbackStopMode, enabled: boolean,
): FallbackStopMode[] {
  const others = modes.filter((entry) => entry !== mode);
  return pruneFallbacks(primary, enabled ? [...others, mode] : others);
}

/**
 * The recorded fallbacks of one controlled-Stop object (intent
 * `controlled_stop` or compiled `default_contract_parameters`), or null when
 * malformed or ambiguous (both the new and the legacy field).
 */
export function recordedFallbacks(
  parameters: Record<string, JsonValue> | null | undefined,
): string[] | null {
  if (!parameters) return [];
  const list = parameters[FALLBACK_STOP_MODES_FIELD];
  const legacy = parameters.recoverable_substitution_mode;
  if (list !== undefined) {
    if (legacy !== undefined) return null;
    return Array.isArray(list) && list.every((mode) => typeof mode === 'string')
      ? list as string[] : null;
  }
  return parameters.stop_mode === 'soft_on_path' && legacy === 'spring_back_hold'
    ? ['spring_back_hold'] : [];
}

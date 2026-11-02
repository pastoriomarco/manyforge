import { errorCatalogRow } from '../state/commandRefusal';

/**
 * Live state of each Applied global (cell-level) interrupt rule, as the
 * Headless event pump projects it into the control-state shell
 * (`global_rules`, CONTRACTS §8). `null` is an unknown catalog (never "no
 * rules"); an empty list proves that the Deployment has no global rule. A row
 * state of `unknown` means the rule can't watch its signals (its
 * `interrupt_state` key is withdrawn) and gates its scope until it can.
 */
export type GlobalRuleState = 'inactive' | 'clear' | 'tripped' | 'unknown' | 'pending' | 'waiting_for_program';

export interface GlobalRuleStatus {
  ruleId: string;
  state: GlobalRuleState;
  /** The keys no applied Program declares; only for `waiting_for_program`. */
  keys: readonly string[];
  /** Present with an unavailable `unknown` state. */
  unavailableReason?: GlobalRuleUnavailableReason;
}

export interface GlobalRuleUnavailableReason {
  code: string;
  message: string;
  details: Readonly<Record<string, string>>;
}

const STATES: ReadonlySet<string> = new Set(['inactive', 'clear', 'tripped', 'unknown', 'pending', 'waiting_for_program']);

export function parseGlobalRuleStatus(value: unknown): readonly GlobalRuleStatus[] | null {
  if (value === null) return null;
  if (!Array.isArray(value) || value.length > 4096) throw new Error('Invalid global rule status list.');
  const seen = new Set<string>();
  return value.map((row: unknown) => {
    const record = row as Record<string, unknown>;
    const isUnknown = record !== null && typeof record === 'object'
      && !Array.isArray(record) && record.state === 'unknown';
    if (row === null || typeof row !== 'object' || Array.isArray(row)
      || Object.keys(record).sort().join(',') !== (isUnknown
        ? 'keys,rule_id,state,unavailable_reason'
        : 'keys,rule_id,state')
      || typeof record.rule_id !== 'string' || !record.rule_id || seen.has(record.rule_id)
      || typeof record.state !== 'string' || !STATES.has(record.state)
      || !Array.isArray(record.keys)
      || !record.keys.every((key) => typeof key === 'string' && key.length > 0)
      || (record.state === 'waiting_for_program') !== (record.keys.length > 0)
      || (isUnknown && !isUnavailableReason(record.unavailable_reason))) {
      throw new Error('Invalid global rule status row.');
    }
    seen.add(record.rule_id);
    return {
      ruleId: record.rule_id,
      state: record.state as GlobalRuleState,
      keys: record.keys as string[],
      ...(isUnknown ? { unavailableReason: record.unavailable_reason as GlobalRuleUnavailableReason } : {}),
    };
  });
}

function isUnavailableReason(value: unknown): value is GlobalRuleUnavailableReason {
  if (value === null || typeof value !== 'object' || Array.isArray(value)) return false;
  const reason = value as Record<string, unknown>;
  return Object.keys(reason).sort().join(',') === 'code,details,message'
    && typeof reason.code === 'string' && reason.code.length > 0
    && typeof reason.message === 'string'
    && reason.details !== null && typeof reason.details === 'object' && !Array.isArray(reason.details)
    && Object.values(reason.details as Record<string, unknown>).every((detail) => typeof detail === 'string');
}

/** The operator text of one rule's state. */
export function globalRuleStateText(rule: GlobalRuleStatus): string {
  switch (rule.state) {
    case 'inactive': return 'inactive';
    case 'clear': return 'clear';
    case 'tripped': return 'tripped';
    case 'unknown': return "can't watch (its scope is held)";
    case 'pending': return 'pending (no current state yet)';
    case 'waiting_for_program':
      return `waiting for a Program that declares ${rule.keys.join(', ')}`;
  }
}

/** The short catalog-backed reason shown while a rule cannot watch its signals. */
export function globalRuleUnavailableText(rule: GlobalRuleStatus): string | null {
  const reason = rule.state === 'unknown' ? rule.unavailableReason : undefined;
  if (reason === undefined) return null;
  const detailedCode = reason.details.reason;
  const detailedText = detailedCode === undefined ? null : errorCatalogRow(detailedCode)?.[0] ?? null;
  const reasonText = detailedText ?? errorCatalogRow(reason.code)?.[0] ?? (reason.message || reason.code);
  return `Interrupt rule ${rule.ruleId} can't watch its signals: ${reasonText}`;
}

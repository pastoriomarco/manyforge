/**
 * Live state of each Applied global (cell-level) interrupt rule, as the
 * Headless event pump projects it into the control-state shell
 * (`global_rules`, contracts 64/75/78/86/110). `null` is unknown (no
 * readable rule catalog), never "no rules"; an empty list is the proof that
 * the Deployment has no global rule.
 */
export type GlobalRuleState = 'clear' | 'tripped' | 'latched' | 'pending' | 'waiting_for_program';

export interface GlobalRuleStatus {
  ruleId: string;
  state: GlobalRuleState;
  /** The keys no applied Program declares; only for `waiting_for_program`. */
  keys: readonly string[];
}

const STATES: ReadonlySet<string> = new Set(['clear', 'tripped', 'latched', 'pending', 'waiting_for_program']);

export function parseGlobalRuleStatus(value: unknown): readonly GlobalRuleStatus[] | null {
  if (value === null) return null;
  if (!Array.isArray(value) || value.length > 4096) throw new Error('Invalid global rule status list.');
  const seen = new Set<string>();
  return value.map((row: unknown) => {
    const record = row as Record<string, unknown>;
    if (row === null || typeof row !== 'object' || Array.isArray(row)
      || Object.keys(record).sort().join(',') !== 'keys,rule_id,state'
      || typeof record.rule_id !== 'string' || !record.rule_id || seen.has(record.rule_id)
      || typeof record.state !== 'string' || !STATES.has(record.state)
      || !Array.isArray(record.keys)
      || !record.keys.every((key) => typeof key === 'string' && key.length > 0)
      || (record.state === 'waiting_for_program') !== (record.keys.length > 0)) {
      throw new Error('Invalid global rule status row.');
    }
    seen.add(record.rule_id);
    return {
      ruleId: record.rule_id,
      state: record.state as GlobalRuleState,
      keys: record.keys as string[],
    };
  });
}

/** The operator text of one rule's state. */
export function globalRuleStateText(rule: GlobalRuleStatus): string {
  switch (rule.state) {
    case 'clear': return 'clear';
    case 'tripped': return 'tripped';
    case 'latched': return 'latched (see its recovery notice)';
    case 'pending': return 'pending (no current state yet)';
    case 'waiting_for_program':
      return `waiting for a Program that declares ${rule.keys.join(', ')}`;
  }
}

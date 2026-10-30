import type {
  BlackboardKeyDeclaration,
  JsonValue,
  ProgramAuthoringDocument,
  ProgramBindingDocument,
  ProgramTreeNode,
} from '../types/api';
import type { TargetStopResponse } from '../types/robotControl';
import {
  INTERRUPT_NODE_OUTCOME_VALUES,
  INTERRUPT_STATE_NAMESPACE,
  INTERRUPT_STATE_VALUES,
  isSystemKey,
  NODE_OUTCOME_NAMESPACE,
  NODE_OUTCOME_NONE,
  NODE_OUTCOME_VALUES,
} from './blackboardNamespaces';
import {
  declaredIoSources,
  ioSourceKey,
  rowInputLiveness,
  type IoLivenessMode,
} from './ioSignalConfiguration';
import { INTERRUPT_MOTION_NODE_IDS, programBlackboardKeys } from './programAuthoring';
import { resolvedInputSignals, resolvedTargetRoles } from './targetRoleResolution';
import {
  CARRIED_OBJECT_LOST_RESOURCE_ROLE,
  systemSignalOriginSuffix,
  type CarriedObjectLostSignalChoice,
} from './systemSignalAuthoring';

/**
 * Interrupt rule authoring (schema04): the closed condition grammar shared by
 * node rules and global Deployment rules, its operand choices and local
 * validation, the robot's Stop stage labels, and the Deployment
 * `policies.interrupt_rules` rows.
 */

export type ConditionValueType = 'bool' | 'int64' | 'float64' | 'string';
export const CONDITION_OPERATORS = ['==', '!=', '<', '<=', '>', '>='] as const;
export type ConditionOperator = typeof CONDITION_OPERATORS[number];

export const MAX_CONDITION_DEPTH = 8;
export const MAX_CONDITION_NODES = 64;
const MAX_SAFE = Number.MAX_SAFE_INTEGER;

/** One selectable left-hand operand: a bound input signal or a scalar key. */
export interface ConditionOperandChoice {
  /** Canonical JSON of the operand; the select value. */
  id: string;
  operand: JsonValue;
  label: string;
  /** '' for signals, else the key (for namespace grouping). */
  key: string;
  valueType: ConditionValueType;
  /** Closed string values offered as a select (outcome and rule-state keys). */
  values?: readonly string[];
  /** A signal's declared liveness, when the Deployment Draft declares it. */
  liveness?: IoLivenessMode;
}

/** Operand label suffix of a change-only signal (contracts 76/81). */
export const LIVENESS_UNPROVEN_SUFFIX = ' · liveness unproven';

function signalLabel(base: string, liveness: IoLivenessMode | undefined): string {
  return liveness === 'change_only' ? `${base}${LIVENESS_UNPROVEN_SUFFIX}` : base;
}

const SCALAR_KEY_TYPES = new Set<string>(['bool', 'int64', 'float64', 'string']);

export function operandId(operand: JsonValue): string {
  const sorted = (value: JsonValue): JsonValue => (
    value !== null && typeof value === 'object' && !Array.isArray(value)
      ? Object.fromEntries(Object.keys(value).sort().map((key) => [key, sorted(value[key])]))
      : value
  );
  return JSON.stringify(sorted(operand));
}

/** Operators a value type accepts: bool and string only compare for (in)equality. */
export function operatorsFor(type: ConditionValueType): readonly ConditionOperator[] {
  return type === 'bool' || type === 'string' ? ['==', '!='] : CONDITION_OPERATORS;
}

export function defaultLiteral(choice: ConditionOperandChoice): JsonValue {
  if (choice.valueType === 'bool') return true;
  if (choice.valueType === 'string') return choice.values?.[0] ?? '';
  return 0;
}

function keyChoice(key: BlackboardKeyDeclaration): ConditionOperandChoice | null {
  if (!SCALAR_KEY_TYPES.has(key.type)) return null;
  const namespace = key.key.slice(0, Math.max(0, key.key.indexOf('.')));
  const values = namespace === INTERRUPT_STATE_NAMESPACE ? INTERRUPT_STATE_VALUES
    : namespace === NODE_OUTCOME_NAMESPACE ? (key.description?.includes('no_signal_trip')
      ? INTERRUPT_NODE_OUTCOME_VALUES : NODE_OUTCOME_VALUES)
      : undefined;
  const operand = { key: key.key };
  // System keys are listed by the node or rule they belong to (contract 105(b)).
  const owner = key.key.slice(namespace.length + 1);
  const label = namespace === NODE_OUTCOME_NAMESPACE ? `${owner} (node outcome)`
    : namespace === INTERRUPT_STATE_NAMESPACE ? `${owner} (rule state)`
      : `${key.key} (${key.type})`;
  return {
    id: operandId(operand),
    operand,
    label,
    key: key.key,
    valueType: key.type as ConditionValueType,
    ...(values === undefined ? {} : { values }),
  };
}

/**
 * Node-rule operands: the input signals of every Program target role bound in
 * the current binding (`signal_role_bindings`, inputs only), with the
 * liveness the Deployment Draft declares for them, then every declared scalar
 * key incl. `node_outcome.*` and `interrupt_state.*`. Without a binding, the
 * target roles resolve through the scenario role assignment and their signals
 * are the Deployment Draft's declared inputs (targetRoleResolution).
 */
export function nodeConditionChoices(
  program: Pick<ProgramAuthoringDocument, 'blackboard' | 'forest' | 'target_roles'>,
  binding: ProgramBindingDocument | null | undefined,
  globalRuleIds: readonly string[],
  deploymentDocument: Record<string, JsonValue> | null = null,
  roleAssignment: Record<string, JsonValue> | null = null,
  systemSignals: readonly CarriedObjectLostSignalChoice[] = [],
): ConditionOperandChoice[] {
  const declared = declaredIoSources(deploymentDocument);
  const targets = binding == null
    ? resolvedTargetRoles(program, roleAssignment).map((target) => ({
      ...target,
      signal_role_bindings: resolvedInputSignals(program, target, deploymentDocument)
        .map((row) => ({ ...row, direction: 'input' as const })),
    }))
    : binding.target_role_bindings;
  const signals: ConditionOperandChoice[] = [];
  for (const target of targets) {
    if (!Object.hasOwn(program.target_roles ?? {}, target.target_role_id)) continue;
    const requiredRoles = objectOf(program.target_roles[target.target_role_id])?.required_resource_roles;
    const requiresCarriedObjectLost = Array.isArray(requiredRoles)
      && requiredRoles.includes(CARRIED_OBJECT_LOST_RESOURCE_ROLE);
    const systemSignal = systemSignals.find((row) => (
      row.controllableComponentId === target.controllable_component_id
      && row.commandTargetId === target.command_target_id
    ));
    for (const row of target.signal_role_bindings ?? []) {
      if (row.direction !== 'input') continue;
      if (row.resource_role_id === CARRIED_OBJECT_LOST_RESOURCE_ROLE
        && (!requiresCarriedObjectLost || systemSignal?.signalName !== row.signal_name)) continue;
      const operand = { signal: { target_role: target.target_role_id, resource_role: row.resource_role_id } };
      const liveness = declared.get(ioSourceKey(
        target.controllable_component_id, target.command_target_id, row.resource_role_id,
      ))?.liveness;
      const originSuffix = row.resource_role_id === CARRIED_OBJECT_LOST_RESOURCE_ROLE
        ? systemSignalOriginSuffix(systemSignal) : '';
      const label = `${target.target_role_id} · ${row.resource_role_id} (${row.signal_name}, input)`;
      signals.push({
        id: operandId(operand),
        operand,
        label: `${signalLabel(label, liveness)}${originSuffix}`,
        key: '',
        valueType: row.value_type,
        ...(liveness === undefined ? {} : { liveness }),
      });
    }
    if (systemSignal !== undefined && requiresCarriedObjectLost) {
      const operand = {
        signal: { target_role: target.target_role_id, resource_role: CARRIED_OBJECT_LOST_RESOURCE_ROLE },
      };
      const id = operandId(operand);
      if (!signals.some((choice) => choice.id === id)) {
        signals.push({
          id,
          operand,
          label: `${target.target_role_id} · ${CARRIED_OBJECT_LOST_RESOURCE_ROLE} (${systemSignal.signalName}, input)${systemSignalOriginSuffix(systemSignal)}`,
          key: '',
          valueType: 'bool',
        });
      }
    }
  }
  const keys = programBlackboardKeys(program, globalRuleIds)
    .flatMap((key) => { const choice = keyChoice(key); return choice === null ? [] : [choice]; });
  return [...signals, ...keys];
}

/** A Deployment input signal row: component id, signal name and value type. */
export interface DeploymentInputSignal {
  controllableComponentId: string;
  signalName: string;
  valueType: ConditionValueType;
  liveness?: IoLivenessMode;
  provenance?: 'last_apply';
}

/** Global-rule operands: Deployment input signals, then the given scalar keys. */
export function globalConditionChoices(
  signals: readonly DeploymentInputSignal[],
  keys: readonly BlackboardKeyDeclaration[],
): ConditionOperandChoice[] {
  return [
    ...signals.map((signal) => {
      const operand = {
        signal: { controllable_component_id: signal.controllableComponentId, signal_name: signal.signalName },
      };
      const originSuffix = signal.provenance === 'last_apply' ? ' · from last Apply' : '';
      const label = `${signal.controllableComponentId} · ${signal.signalName} (input)`;
      return {
        id: operandId(operand),
        operand,
        label: `${signalLabel(label, signal.liveness)}${originSuffix}`,
        key: '',
        valueType: signal.valueType,
        ...(signal.liveness === undefined ? {} : { liveness: signal.liveness }),
      };
    }),
    ...keys.flatMap((key) => { const choice = keyChoice(key); return choice === null ? [] : [choice]; }),
  ];
}

function objectOf(value: JsonValue | undefined): Record<string, JsonValue> | null {
  return value !== null && typeof value === 'object' && !Array.isArray(value) ? value : null;
}

/** The single operator of a condition node, or null when malformed. */
export function conditionKind(value: JsonValue | undefined): 'all' | 'any' | 'not' | 'compare' | null {
  const record = objectOf(value);
  if (record === null) return null;
  const names = Object.keys(record);
  if (names.length !== 1) return null;
  const [name] = names;
  return name === 'all' || name === 'any' || name === 'not' || name === 'compare' ? name : null;
}

export function newCompare(choice: ConditionOperandChoice | undefined): JsonValue {
  return {
    compare: {
      operand: choice?.operand ?? { key: '' },
      op: '==',
      value: choice === undefined ? true : defaultLiteral(choice),
    },
  };
}

function literalMatches(type: ConditionValueType, value: JsonValue): boolean {
  if (type === 'bool') return typeof value === 'boolean';
  if (type === 'string') return typeof value === 'string';
  if (typeof value !== 'number' || !Number.isFinite(value)) return false;
  if (type === 'int64') return Number.isInteger(value) && Math.abs(value) <= MAX_SAFE;
  return !Number.isInteger(value) || Math.abs(value) <= MAX_SAFE;
}

/**
 * The first local problem of a condition, or null: shape (one operator,
 * non-empty all/any, closed compare), known operand, operator applicability,
 * typed literal, and the per-condition limits. The backend stays the
 * authority; this mirrors its refusals so they show while editing.
 */
export function conditionProblem(
  condition: JsonValue | undefined,
  choices: readonly ConditionOperandChoice[],
): string | null {
  const byId = new Map(choices.map((choice) => [choice.id, choice]));
  let nodes = 0;
  const visit = (value: JsonValue | undefined, depth: number, path: string): string | null => {
    nodes += 1;
    if (depth > MAX_CONDITION_DEPTH) return `The condition is nested deeper than ${MAX_CONDITION_DEPTH} levels.`;
    const kind = conditionKind(value);
    const record = objectOf(value);
    if (kind === null || record === null) return `${path} must be exactly one of all, any, not or compare.`;
    const body = record[kind];
    if (kind === 'all' || kind === 'any') {
      if (!Array.isArray(body) || body.length === 0) return `${path}: “${kind}” needs at least one condition.`;
      for (const [index, child] of body.entries()) {
        const problem = visit(child, depth + 1, `${path}.${kind}[${index}]`);
        if (problem !== null) return problem;
      }
      return null;
    }
    if (kind === 'not') return visit(body, depth + 1, `${path}.not`);
    const compare = objectOf(body);
    if (compare === null || Object.keys(compare).sort().join(',') !== 'op,operand,value') {
      return `${path}: a comparison needs exactly an operand, an operator and a value.`;
    }
    const id = operandId(compare.operand);
    const choice = byId.get(id);
    if (choice === undefined) return `${path}: ${id} is not a bound input signal or a declared scalar key.`;
    if (choice.liveness === 'change_only') {
      return `${path}: ${choice.label.slice(0, -LIVENESS_UNPROVEN_SUFFIX.length)} is a change-only source whose liveness is unproven between changes, so it cannot be an interrupt condition operand. Declare its source periodic, or read it with an I/O wait, read or check node instead.`;
    }
    if (!operatorsFor(choice.valueType).includes(compare.op as ConditionOperator)) {
      return `${path}: ${String(compare.op)} does not apply to a ${choice.valueType} value.`;
    }
    if (!literalMatches(choice.valueType, compare.value)) {
      return `${path}: the value must be a ${choice.valueType} literal${choice.valueType === 'int64' ? ' within ±(2^53−1)' : ''}.`;
    }
    return null;
  };
  const problem = visit(condition, 1, 'condition');
  if (problem !== null) return problem;
  if (nodes > MAX_CONDITION_NODES) return `The condition has more than ${MAX_CONDITION_NODES} parts.`;
  return null;
}

/** The keys a condition names (for the BB key union display; no dependency keys). */
export function conditionKeys(condition: JsonValue | undefined): string[] {
  const found = new Set<string>();
  const walk = (value: JsonValue | undefined) => {
    const record = objectOf(value);
    if (record === null) return;
    const compare = objectOf(record.compare);
    const operand = objectOf(compare?.operand);
    if (typeof operand?.key === 'string') found.add(operand.key);
    for (const child of [...(Array.isArray(record.all) ? record.all : []),
      ...(Array.isArray(record.any) ? record.any : []), record.not]) walk(child);
  };
  walk(condition);
  return [...found].sort();
}

/**
 * Node outcome keys a condition compares with `!=` (to anything but `none`)
 * without also testing `!= none` somewhere in the same condition. Not a
 * refusal: before the node runs its key is `none`, so such a test is true.
 */
export function unguardedNodeOutcomeKeys(condition: JsonValue | undefined): string[] {
  const tested = new Set<string>();
  const guarded = new Set<string>();
  const walk = (value: JsonValue | undefined) => {
    const record = objectOf(value);
    if (record === null) return;
    const compare = objectOf(record.compare);
    const key = objectOf(compare?.operand)?.key;
    if (typeof key === 'string' && key.startsWith(`${NODE_OUTCOME_NAMESPACE}.`) && compare?.op === '!=') {
      (compare.value === NODE_OUTCOME_NONE ? guarded : tested).add(key);
    }
    for (const child of [...(Array.isArray(record.all) ? record.all : []),
      ...(Array.isArray(record.any) ? record.any : []), record.not]) walk(child);
  };
  walk(condition);
  return [...tested].filter((key) => !guarded.has(key)).sort();
}

/** The robot's three qualified Stop stages, in chain order (contract 61). */
export const STOP_STAGE_LABELS = [
  'Controlled stop (incl. spring-back)',
  'Controller goal cancel',
  'Software hard stop',
] as const;

/** One qualified stage row of a robot's chain: its sealed id and label. */
export interface QualifiedStopStage {
  id: string;
  label: string;
}

/** A published motion target's qualified Stop chain (SX-2167 `stop_response`). */
export interface TargetStopChain {
  controllableComponentId: string;
  commandTargetId: string;
  stopResponse?: TargetStopResponse;
}

const STOP_STAGE_KIND_LABELS: Readonly<Record<string, string>> = {
  nominal: 'Nominal stop',
  controlled_fallback: 'Controlled fallback',
  software_hard: 'Software hard stop',
  safety_system_request: 'Safety system request',
};

const readable = (text: string) => {
  const spaced = text.replaceAll('_', ' ');
  return spaced.charAt(0).toUpperCase() + spaced.slice(1);
};

/** The picker rows of one target's qualified chain: readable label by stage kind, stored stage id. */
export function qualifiedStopStages(response: TargetStopResponse): QualifiedStopStage[] {
  return response.stages.map((stage, index) => ({
    id: stage.stageId,
    label: `${index + 1}. ${STOP_STAGE_KIND_LABELS[stage.stageKind] ?? readable(stage.stageKind)}`
      + ` (${stage.commandKind.replaceAll('_', ' ')})`,
  }));
}

/** The qualified stages of the motion target bound to `targetRole`, or null when not published. */
export function stopStagesForTargetRole(
  chains: readonly TargetStopChain[],
  targetRoleBindings: readonly { target_role_id: string; controllable_component_id: string; command_target_id: string }[]
    | null | undefined,
  targetRole: unknown,
): QualifiedStopStage[] | null {
  if (typeof targetRole !== 'string') return null;
  const binding = targetRoleBindings?.find((row) => row.target_role_id === targetRole);
  if (binding === undefined) return null;
  const chain = chains.find((row) => row.controllableComponentId === binding.controllable_component_id
    && row.commandTargetId === binding.command_target_id);
  return chain?.stopResponse === undefined ? null : qualifiedStopStages(chain.stopResponse);
}

/**
 * A global rule's stages: the stage ids every published motion target's chain
 * qualifies, in the first chain's order; null when no chain is published.
 */
export function commonStopStages(chains: readonly TargetStopChain[]): QualifiedStopStage[] | null {
  const published = chains.flatMap((row) => (row.stopResponse === undefined ? [] : [row.stopResponse]));
  if (published.length === 0) return null;
  return qualifiedStopStages(published[0]).filter((stage) => published.every(
    (response) => response.stages.some((row) => row.stageId === stage.id),
  ));
}

export const STOP_STAGES_UNAVAILABLE_HINT = 'The robot’s qualified Stop stage ids are available after Apply; until then the first stage applies.';

/** Stages a success threshold may name: the entry stage and every later one (contract 70). */
export function successThroughStages(
  stages: readonly QualifiedStopStage[],
  entryStageId: string | undefined,
): QualifiedStopStage[] {
  const start = entryStageId === undefined ? 0 : stages.findIndex((stage) => stage.id === entryStageId);
  return start < 0 ? [] : stages.slice(start);
}

export const INTERRUPT_NODE_IDS: ReadonlySet<string> = new Set(INTERRUPT_MOTION_NODE_IDS);

// ---- Contract 87: interrupt condition keys declare `initial:` ----

/** The Core's stable refusal reason, used verbatim in the GUI. */
export const INTERRUPT_KEY_INITIAL_REQUIRED = 'interrupt_key_initial_required';

/**
 * The flag of one condition whose declared keys lack `initial:` (presence
 * only: false / 0 are valid), or null. Undeclared keys are refused elsewhere;
 * system keys are exempt: the system defines their start value (contract 105).
 */
export function interruptKeyInitialProblem(
  condition: JsonValue | undefined,
  keys: readonly BlackboardKeyDeclaration[],
): string | null {
  const missing = conditionKeys(condition).filter((name) => {
    if (isSystemKey(name)) return false;
    const declaration = keys.find((key) => key.key === name);
    return declaration !== undefined && declaration.initial === undefined;
  });
  return missing.length === 0 ? null
    : `${INTERRUPT_KEY_INITIAL_REQUIRED}: ${missing.join(', ')} ${missing.length === 1 ? 'is' : 'are'} read by an interrupt condition and must declare an initial value.`;
}

/** Every key an interrupt condition reads: the forest's node rules and the Deployment Draft's global rules. */
export function interruptConditionKeyUses(
  forest: readonly ProgramTreeNode[],
  deploymentDocument: Record<string, JsonValue> | null,
): Set<string> {
  const found = new Set<string>();
  const pending = [...forest];
  for (let node = pending.pop(); node !== undefined; node = pending.pop()) {
    if (INTERRUPT_NODE_IDS.has(node.node_id)) {
      for (const key of conditionKeys(node.parameters?.interrupt_condition as JsonValue | undefined)) found.add(key);
    }
    pending.push(...node.children);
  }
  for (const rule of globalRulesOf(deploymentDocument) ?? []) {
    for (const key of conditionKeys(objectOf(rule)?.condition)) found.add(key);
  }
  return found;
}

// ---- Global Deployment rules (schema04 §3) ----

/**
 * Contract 73: every arm (`all_motion`), the robots of selected Applied
 * Program execution groups (`{execution_groups}`, 1..N unique, sorted), or the
 * existing Full Stop (`everything`).
 */
export type GlobalRuleScope = 'all_motion' | 'everything' | { execution_groups: string[] };
export type GlobalRuleScopeKind = 'all_motion' | 'execution_groups' | 'everything';
export type IdleBehaviour = 'latch' | 'ignore';
/**
 * What a trip does (omitted = gate): `gate` refuses new motion in scope while
 * the condition holds and releases by itself when it clears; `full_stop` trips
 * a cell Full Stop and latches until the operator clears it; `notify` only
 * publishes state on a normal trip, while its existing scoped gate applies
 * whenever it cannot watch its inputs.
 */
export type OnTrip = 'gate' | 'full_stop' | 'notify';

export interface GlobalInterruptRule {
  rule_id: string;
  condition: JsonValue;
  scope: GlobalRuleScope;
  /** 0–100; omitted = 0 (contract 59). */
  priority?: number;
  on_trip?: OnTrip;
  /** Only with `on_trip: full_stop`. */
  idle_behaviour?: IdleBehaviour;
  stop_entry_stage_id?: string;
}

const RULE_FIELDS = new Set([
  'rule_id', 'condition', 'scope', 'priority', 'on_trip', 'idle_behaviour', 'stop_entry_stage_id',
]);
const PORTABLE_ID = /^[a-z][a-z0-9_.-]*$/;
/** The components whose signals a condition reads (its source providers). */
export function conditionSignalComponents(condition: JsonValue | undefined): string[] {
  const found = new Set<string>();
  const walk = (value: JsonValue | undefined) => {
    const record = objectOf(value);
    if (record === null) return;
    const signal = objectOf(objectOf(objectOf(record.compare)?.operand)?.signal);
    if (typeof signal?.controllable_component_id === 'string') found.add(signal.controllable_component_id);
    for (const child of [...(Array.isArray(record.all) ? record.all : []),
      ...(Array.isArray(record.any) ? record.any : []), record.not]) walk(child);
  };
  walk(condition);
  return [...found].sort();
}

/** Global rules that read a signal of one of these components, with the components they read. */
export function globalRulesReadingComponents(
  document: Record<string, JsonValue> | null,
  componentIds: readonly string[],
): Array<{ ruleId: string; componentIds: string[] }> {
  return (globalRulesOf(document) ?? []).flatMap((row) => {
    const rule = objectOf(row);
    const sources = conditionSignalComponents(rule?.condition)
      .filter((id) => componentIds.includes(id));
    return typeof rule?.rule_id === 'string' && sources.length > 0
      ? [{ ruleId: rule.rule_id, componentIds: sources }] : [];
  });
}

const GROUP_ID = /^[a-z][a-z0-9_.-]*$/;

/** The Draft's `policies.interrupt_rules` rows, verbatim (null when malformed). */
export function globalRulesOf(document: Record<string, JsonValue> | null): JsonValue[] | null {
  const policies = objectOf(document?.policies);
  const rules = policies?.interrupt_rules;
  if (rules === undefined) return [];
  return Array.isArray(rules) ? rules : null;
}

export function globalRuleIds(document: Record<string, JsonValue> | null): string[] {
  return (globalRulesOf(document) ?? []).flatMap((row) => {
    const id = objectOf(row)?.rule_id;
    return typeof id === 'string' && PORTABLE_ID.test(id) ? [id] : [];
  });
}

export function newGlobalRule(existing: readonly string[], choice: ConditionOperandChoice | undefined): GlobalInterruptRule {
  let index = 1;
  while (existing.includes(`rule_${index}`)) index += 1;
  return { rule_id: `rule_${index}`, condition: newCompare(choice), scope: 'all_motion', priority: 0 };
}

/** The first problem of one rule row (closed fields, ids, ranges), or null. */
export function globalRuleProblem(
  row: JsonValue,
  otherIds: readonly string[],
  choices: readonly ConditionOperandChoice[],
): string | null {
  const rule = objectOf(row);
  if (rule === null) return 'A rule must be an object.';
  const unknown = Object.keys(rule).filter((key) => !RULE_FIELDS.has(key));
  if (unknown.length > 0) return `Unknown rule field(s): ${unknown.sort().join(', ')}.`;
  const id = rule.rule_id;
  if (typeof id !== 'string' || !PORTABLE_ID.test(id) || new TextEncoder().encode(id).length > 128) {
    return 'Rule id must be a portable lower-case id (at most 128 bytes).';
  }
  if (otherIds.includes(id)) return `Another rule already uses ${id}.`;
  const priority = rule.priority;
  if (priority !== undefined
    && (typeof priority !== 'number' || !Number.isInteger(priority) || priority < 0 || priority > 100)) {
    return 'Priority must be an integer from 0 to 100.';
  }
  const scope = rule.scope;
  if (scope !== 'all_motion' && scope !== 'everything') {
    const groups = objectOf(scope)?.execution_groups;
    if (!Array.isArray(groups) || groups.length === 0 || Object.keys(objectOf(scope) ?? {}).length !== 1) {
      return 'Scope is all motion, everything (Full Stop) or at least one execution group.';
    }
    if (groups.some((group) => typeof group !== 'string' || !GROUP_ID.test(group))) {
      return 'Execution group ids must be portable lower-case ids.';
    }
    const ids = groups as string[];
    if (new Set(ids).size !== ids.length) return 'Execution group ids must be unique.';
    if (ids.some((group, at) => at > 0 && ids[at - 1] > group)) return 'Execution group ids must be sorted.';
  }
  if (rule.on_trip !== undefined && rule.on_trip !== 'gate' && rule.on_trip !== 'full_stop' && rule.on_trip !== 'notify') {
    return 'On trip is gate, full_stop or notify.';
  }
  if (rule.idle_behaviour !== undefined && rule.idle_behaviour !== 'latch' && rule.idle_behaviour !== 'ignore') {
    return 'Idle behaviour is latch or ignore.';
  }
  if (rule.idle_behaviour !== undefined && rule.on_trip !== 'full_stop') {
    return 'Idle behaviour applies to on_trip full_stop only.';
  }
  if (rule.on_trip === 'notify' && rule.stop_entry_stage_id !== undefined) {
    return 'Stop entry stage does not apply to on_trip notify.';
  }
  if (rule.stop_entry_stage_id !== undefined && (typeof rule.stop_entry_stage_id !== 'string' || rule.stop_entry_stage_id === '')) {
    return 'Stop entry stage must be a qualified stage id.';
  }
  const states = conditionKeys(rule.condition).filter((key) => key.startsWith(`${INTERRUPT_STATE_NAMESPACE}.`));
  if (states.length > 0) {
    return `${GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED}: a global rule cannot read ${states.join(', ')}; read rule states in the Program instead.`;
  }
  return conditionProblem(rule.condition, choices);
}

// ---- Contracts 106, 110, 111: global rules' key operands ----

/** The Core's stable refusal reasons, used verbatim in the GUI. */
export const GLOBAL_RULE_KEY_UNDECLARED = 'global_rule_key_undeclared';
export const GLOBAL_RULE_STATE_OPERAND_UNSUPPORTED = 'global_rule_state_operand_unsupported';

const isStateKey = (key: string) => key.startsWith(`${INTERRUPT_STATE_NAMESPACE}.`);

/** The literal type a condition compares a key with (first comparison wins). */
function comparedType(condition: JsonValue | undefined, key: string): ConditionValueType | null {
  let found: ConditionValueType | null = null;
  const walk = (value: JsonValue | undefined) => {
    const record = objectOf(value);
    if (record === null || found !== null) return;
    const compare = objectOf(record.compare);
    if (objectOf(compare?.operand)?.key === key) {
      const literal = compare?.value;
      found = typeof literal === 'boolean' ? 'bool' : typeof literal === 'string' ? 'string'
        : typeof literal === 'number' ? (Number.isInteger(literal) ? 'int64' : 'float64') : null;
    }
    for (const child of [...(Array.isArray(record.all) ? record.all : []),
      ...(Array.isArray(record.any) ? record.any : []), record.not]) walk(child);
  };
  walk(condition);
  return found;
}

/** True when a condition compares at least one signal. */
function readsSignal(condition: JsonValue | undefined): boolean {
  const record = objectOf(condition);
  if (record === null) return false;
  if (objectOf(objectOf(record.compare)?.operand)?.signal !== undefined) return true;
  return [...(Array.isArray(record.all) ? record.all : []),
    ...(Array.isArray(record.any) ? record.any : []), record.not].some(readsSignal);
}

/**
 * One global rule's operands: Deployment input signals, the Program Draft's
 * keys except `interrupt_state.*` (refused in global rules, contract 111),
 * and every user key the rule already reads that the Program does not
 * declare (kept editable; flagged by `globalRuleKeyNotes`). `programKeys` is
 * null when there is no Program.
 */
export function globalRuleChoices(
  document: Record<string, JsonValue> | null,
  programKeys: readonly BlackboardKeyDeclaration[] | null,
  condition: JsonValue | undefined,
  systemSignals: readonly CarriedObjectLostSignalChoice[] = [],
): ConditionOperandChoice[] {
  const declared = (programKeys ?? []).filter((key) => !isStateKey(key.key));
  const known = new Set(declared.map((key) => key.key));
  const pending = conditionKeys(condition).flatMap((key): BlackboardKeyDeclaration[] => {
    if (known.has(key) || isStateKey(key)) return [];
    const type = comparedType(condition, key);
    return type === null ? [] : [{ key, type, access: 'read', persistence: 'run' }];
  });
  const inputs = deploymentInputSignals(document);
  const knownInputs = new Set(inputs.map((signal) => `${signal.controllableComponentId}\u0000${signal.signalName}`));
  for (const signal of systemSignals) {
    const key = `${signal.controllableComponentId}\u0000${signal.signalName}`;
    if (knownInputs.has(key)) {
      if (signal.provenance === 'last_apply') {
        const existing = inputs.find((row) => (
          row.controllableComponentId === signal.controllableComponentId
          && row.signalName === signal.signalName
        ));
        if (existing !== undefined) existing.provenance = 'last_apply';
      }
      continue;
    }
    knownInputs.add(key);
    inputs.push({
      controllableComponentId: signal.controllableComponentId,
      signalName: signal.signalName,
      valueType: signal.valueType,
      ...(signal.provenance === undefined ? {} : { provenance: signal.provenance }),
    });
  }
  return globalConditionChoices(inputs, [...declared, ...pending]);
}

/**
 * The notes of one global rule's key operands:
 * - `waiting`: keys no Program declares yet (no Program): the whole rule is
 *   inactive until a Program declares them (contract 110);
 * - `undeclared`: keys the Program lacks: its Apply is refused (106);
 * - `stateOperands`: `interrupt_state.*` keys, refused in global rules (111);
 * - `mixed`: the rule reads signals and keys, so it does not act while no
 *   Program is applied (110).
 */
export function globalRuleKeyNotes(
  condition: JsonValue | undefined,
  programKeys: readonly BlackboardKeyDeclaration[] | null,
): { waiting: string[]; undeclared: string[]; stateOperands: string[]; mixed: boolean } {
  const keys = conditionKeys(condition);
  const known = new Set((programKeys ?? []).map((key) => key.key));
  const missing = keys.filter((key) => !isStateKey(key) && !known.has(key));
  return {
    waiting: programKeys === null ? missing : [],
    undeclared: programKeys === null ? [] : missing,
    stateOperands: keys.filter(isStateKey),
    mixed: keys.some((key) => !isStateKey(key)) && readsSignal(condition),
  };
}

/** An execution-groups scope, unique and sorted (the stored canonical order). */
export function canonicalScope(groups: readonly string[]): GlobalRuleScope {
  return { execution_groups: [...new Set(groups)].sort() };
}

/** Which of the three scope forms a stored scope is, or null when malformed. */
export function globalRuleScopeKind(scope: JsonValue | undefined): GlobalRuleScopeKind | null {
  if (scope === 'all_motion' || scope === 'everything') return scope;
  return Array.isArray(objectOf(scope)?.execution_groups) ? 'execution_groups' : null;
}

/** The execution group ids a stored scope names (empty for the other forms). */
export function globalRuleScopeGroups(scope: JsonValue | undefined): string[] {
  const groups = objectOf(scope)?.execution_groups;
  return Array.isArray(groups) ? groups.filter((group): group is string => typeof group === 'string') : [];
}

/** Input signal rows of every component in a Deployment intent Draft. */
export function deploymentInputSignals(document: Record<string, JsonValue> | null): DeploymentInputSignal[] {
  const components = Array.isArray(document?.components) ? document.components : [];
  const found = new Map<string, DeploymentInputSignal>();
  for (const value of components) {
    const component = objectOf(value);
    const configuration = objectOf(component?.configuration);
    const runtime = objectOf(configuration?.runtime_configuration) ?? configuration;
    const rows = runtime?.io_signals;
    if (!Array.isArray(rows)) continue;
    for (const rowValue of rows) {
      const row = objectOf(rowValue);
      if (row?.direction !== 'input' || typeof row.controllable_component_id !== 'string'
        || typeof row.signal_name !== 'string') continue;
      const valueType = row.value_type;
      if (valueType !== 'bool' && valueType !== 'int64' && valueType !== 'float64' && valueType !== 'string') continue;
      const liveness = rowInputLiveness(row);
      const signal: DeploymentInputSignal = {
        controllableComponentId: row.controllable_component_id,
        signalName: row.signal_name,
        valueType,
        ...(liveness === null ? {} : { liveness }),
      };
      found.set(`${signal.controllableComponentId}\u0000${signal.signalName}`, signal);
    }
  }
  return [...found.values()].sort((left, right) => (
    left.controllableComponentId.localeCompare(right.controllableComponentId)
    || left.signalName.localeCompare(right.signalName)));
}

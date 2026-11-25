import { useMemo, useState } from 'react';

import type {
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  ProgramFormulaReferences,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  groupByNamespace, INTERRUPT_STATE_NAMESPACE, isSystemKey, keyNamespace, NODE_OUTCOME_NAMESPACE,
} from '../utils/blackboardNamespaces';
import {
  collectBlackboardUsages,
  collectFormulaUsages,
  collectProgramParameterUsages,
  programBlackboardKeys,
  type ProgramMotionDefaultRole,
} from '../utils/programAuthoring';

export type ProgramDeclarationSelection =
  | { kind: 'parameter'; id: string }
  | { kind: 'namespace'; id: string }
  | { kind: 'key'; id: string };

interface ProgramDeclarationsNavigatorProps {
  roots: ProgramTreeNode[];
  parameters: ProgramParameterDeclaration[];
  namespaces: BlackboardNamespaceDeclaration[];
  keys: BlackboardKeyDeclaration[];
  /** The names each Draft formula reads; formulas count as usages. */
  formulaReferences: ProgramFormulaReferences;
  selected: ProgramDeclarationSelection | null;
  onSelect: (selection: ProgramDeclarationSelection) => void;
  /** Parameters acting as Program motion defaults, by parameter ID. */
  motionDefaults?: ReadonlyMap<string, ProgramMotionDefaultRole>;
  /** The Deployment Draft's global rule ids: each adds `interrupt_state.<rule_id>`. */
  globalRuleIds?: readonly string[];
}

const NO_RULE_IDS: readonly string[] = [];
const VALUE_TEXT_LIMIT = 40;

/** A key's declared starting value as row text: short for the row, full for the tooltip. */
export function keyValueText(key: BlackboardKeyDeclaration): { short: string; full: string } {
  if (key.expression !== undefined) return { short: 'computed', full: `computed: ${key.expression}` };
  if (key.initial === undefined) return { short: 'empty until written', full: 'empty until written' };
  const full = typeof key.initial === 'string' ? `"${key.initial}"`
    : typeof key.initial === 'object' ? JSON.stringify(key.initial)
      : String(key.initial);
  // A float's row text keeps 6 significant digits; the tooltip keeps all.
  if (typeof key.initial === 'number' && !Number.isInteger(key.initial)) {
    return { short: String(Number(key.initial.toPrecision(6))), full };
  }
  return {
    short: full.length > VALUE_TEXT_LIMIT ? `${full.slice(0, VALUE_TEXT_LIMIT - 1)}…` : full,
    full,
  };
}

function selectedClass(
  selected: ProgramDeclarationSelection | null,
  kind: ProgramDeclarationSelection['kind'],
  id: string,
): string {
  return selected?.kind === kind && selected.id === id ? ' active' : '';
}

/** Compact declaration navigation; editing remains in the contextual inspector. */
export function ProgramDeclarationsNavigator({
  roots,
  parameters,
  namespaces,
  keys,
  formulaReferences,
  selected,
  onSelect,
  motionDefaults = new Map(),
  globalRuleIds = NO_RULE_IDS,
}: ProgramDeclarationsNavigatorProps) {
  const parameterUsages = useMemo(() => collectProgramParameterUsages(roots), [roots]);
  const blackboardUsages = useMemo(() => collectBlackboardUsages(roots), [roots]);
  const formulaUsages = useMemo(
    () => collectFormulaUsages(formulaReferences),
    [formulaReferences],
  );

  // Declared keys plus the system-owned node outcome, interrupt state and
  // motion group state keys.
  const readableKeys = useMemo(
    () => programBlackboardKeys({ blackboard: { namespaces: [], keys }, forest: roots }, globalRuleIds),
    [globalRuleIds, keys, roots],
  );
  const [keyQuery, setKeyQuery] = useState('');
  const query = keyQuery.trim().toLowerCase();
  const shownKeys = query === '' ? readableKeys : readableKeys.filter((key) => (
    key.key.toLowerCase().includes(query)
    || key.type.toLowerCase().includes(query)
    || (key.description ?? '').toLowerCase().includes(query)
  ));
  const keyGroups = groupByNamespace(shownKeys, (key) => key.key);
  // Inside a namespace group the group header names the namespace, so the row
  // shows the rest of the key; the full key stays in the row's tooltip.
  const grouped = groupByNamespace(readableKeys, (key) => key.key).length > 1;
  const keyRow = (key: BlackboardKeyDeclaration) => {
    const usages = (blackboardUsages[key.key]?.length ?? 0)
      + (formulaUsages.keys[key.key]?.length ?? 0);
    const namespace = keyNamespace(key.key);
    const shownName = grouped && namespace !== '' ? key.key.slice(namespace.length + 1) : key.key;
    const value = keyValueText(key);
    const detail = `type: ${key.type} - value: ${value.short}`;
    const usageTitle = `Used by ${usages} node${usages === 1 ? '' : 's'} or formula${usages === 1 ? '' : 's'}`;
    if (isSystemKey(key.key)) {
      const owner = namespace === NODE_OUTCOME_NAMESPACE ? 'written only by its motion node'
        : namespace === INTERRUPT_STATE_NAMESPACE ? 'published only by its Deployment global rule'
          : 'written only by the Program runner from the group\'s run controls';
      return (
        <button
          key={key.key}
          type="button"
          className="dpick"
          disabled
          title={`${key.key}\nSystem-owned, ${owner}. ${key.description ?? ''}`}
        >
          <span><strong>{shownName}</strong><small>{`type: ${key.type} - system`}</small></span>
          <small title={usageTitle}>{usages}</small>
        </button>
      );
    }
    return (
      <button
        key={key.key}
        type="button"
        className={`dpick${selectedClass(selected, 'key', key.key)}`}
        title={`${key.key}\ntype: ${key.type} - value: ${value.full}${key.description ? `\n${key.description}` : ''}`}
        onClick={() => onSelect({ kind: 'key', id: key.key })}
      >
        <span><strong>{shownName}</strong><small>{detail}</small></span>
        <small title={usageTitle}>{usages}</small>
      </button>
    );
  };

  return (
    <nav className="program-declaration-navigator dpanel" aria-label="Program declarations">
      <section className="dcard">
        <header>
          <h4>Parameters</h4>
          <button type="button" onClick={() => onSelect({ kind: 'parameter', id: '' })}>
            New
          </button>
        </header>
        {parameters.length === 0 && <p>No parameters</p>}
        {parameters.map((parameter) => (
          <button
            key={parameter.parameter_id}
            type="button"
            className={`dpick${selectedClass(selected, 'parameter', parameter.parameter_id)}`}
            onClick={() => onSelect({ kind: 'parameter', id: parameter.parameter_id })}
          >
            <span>
              <strong>{parameter.parameter_id}</strong>
              <small>
                {parameter.type}
                {motionDefaults.has(parameter.parameter_id) && ' · motion default'}
              </small>
            </span>
            {motionDefaults.has(parameter.parameter_id) ? (
              <small title="Motion nodes bound to it, out of those that accept this field">
                {motionDefaults.get(parameter.parameter_id)!.bound}
                /{motionDefaults.get(parameter.parameter_id)!.applicable}
              </small>
            ) : (
              <small>
                {(parameterUsages[parameter.parameter_id]?.length ?? 0)
                  + (formulaUsages.parameters[parameter.parameter_id]?.length ?? 0)}
              </small>
            )}
          </button>
        ))}
      </section>
      <section className="dcard">
        <header>
          <h4>Namespaces</h4>
          <button type="button" onClick={() => onSelect({ kind: 'namespace', id: '' })}>
            New
          </button>
        </header>
        {namespaces.length === 0 && <p>No namespaces</p>}
        {namespaces.map((namespace) => (
          <button
            key={namespace.namespace_id}
            type="button"
            className={`dpick${selectedClass(selected, 'namespace', namespace.namespace_id)}`}
            onClick={() => onSelect({ kind: 'namespace', id: namespace.namespace_id })}
          >
            <span><strong>{namespace.namespace_id}</strong><small>namespace</small></span>
            <small>{keys.filter((key) => key.key.startsWith(`${namespace.namespace_id}.`)).length}</small>
          </button>
        ))}
      </section>
      <section className="dcard">
        <header>
          <h4>Blackboard keys</h4>
          <button type="button" onClick={() => onSelect({ kind: 'key', id: '' })}>
            New
          </button>
        </header>
        {readableKeys.length > 0 && (
          <input
            type="search"
            className="program-declaration-search"
            placeholder="Search keys"
            aria-label="Search blackboard keys"
            value={keyQuery}
            onChange={(event) => setKeyQuery(event.target.value)}
          />
        )}
        {readableKeys.length === 0 && <p>No keys</p>}
        {readableKeys.length > 0 && shownKeys.length === 0 && <p>No key matches “{keyQuery.trim()}”</p>}
        {!grouped
          ? keyGroups.flatMap((group) => group.items).map(keyRow)
          : keyGroups.map((group) => (
            // Each namespace collapses; closed by default, open while searching.
            <details key={`${group.id}${query === '' ? '' : ':search'}`} open={query !== '' || undefined}>
              <summary>{group.label} ({group.items.length})</summary>
              {group.items.map(keyRow)}
            </details>
          ))}
      </section>
    </nav>
  );
}

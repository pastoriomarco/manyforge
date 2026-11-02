import { useMemo } from 'react';

import type {
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  ProgramFormulaReferences,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  groupByNamespace, INTERRUPT_STATE_NAMESPACE, keyNamespace, NODE_OUTCOME_NAMESPACE,
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

  // Declared keys plus the system-owned node outcome and interrupt state keys.
  const readableKeys = useMemo(
    () => programBlackboardKeys({ blackboard: { namespaces: [], keys }, forest: roots }, globalRuleIds),
    [globalRuleIds, keys, roots],
  );
  const keyGroups = groupByNamespace(readableKeys, (key) => key.key);
  const keyRow = (key: BlackboardKeyDeclaration) => {
    const usages = (blackboardUsages[key.key]?.length ?? 0)
      + (formulaUsages.keys[key.key]?.length ?? 0);
    const namespace = keyNamespace(key.key);
    if (namespace === NODE_OUTCOME_NAMESPACE || namespace === INTERRUPT_STATE_NAMESPACE) {
      const owner = namespace === NODE_OUTCOME_NAMESPACE
        ? 'written only by its motion node' : 'published only by its Deployment global rule';
      return (
        <button
          key={key.key}
          type="button"
          className="dpick"
          disabled
          title={`System-owned, ${owner}. ${key.description ?? ''}`}
        >
          <span><strong>{key.key}</strong><small>{key.type} · system</small></span>
          <small>{usages}</small>
        </button>
      );
    }
    return (
      <button
        key={key.key}
        type="button"
        className={`dpick${selectedClass(selected, 'key', key.key)}`}
        onClick={() => onSelect({ kind: 'key', id: key.key })}
      >
        <span><strong>{key.key}</strong><small>{key.type}</small></span>
        <small>{usages}</small>
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
        {readableKeys.length === 0 && <p>No keys</p>}
        {keyGroups.length === 1
          ? keyGroups[0].items.map(keyRow)
          : keyGroups.map((group) => (
            // Each namespace collapses; closed by default.
            <details key={group.id}>
              <summary>{group.label} ({group.items.length})</summary>
              {group.items.map(keyRow)}
            </details>
          ))}
      </section>
    </nav>
  );
}

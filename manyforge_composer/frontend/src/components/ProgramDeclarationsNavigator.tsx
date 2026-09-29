import { useMemo } from 'react';

import type {
  BlackboardKeyDeclaration,
  BlackboardNamespaceDeclaration,
  ProgramParameterDeclaration,
  ProgramTreeNode,
} from '../types/api';
import {
  collectBlackboardUsages,
  collectProgramParameterUsages,
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
  selected: ProgramDeclarationSelection | null;
  onSelect: (selection: ProgramDeclarationSelection) => void;
  /** Parameters acting as Program motion defaults, by parameter ID. */
  motionDefaults?: ReadonlyMap<string, ProgramMotionDefaultRole>;
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
  selected,
  onSelect,
  motionDefaults = new Map(),
}: ProgramDeclarationsNavigatorProps) {
  const parameterUsages = useMemo(() => collectProgramParameterUsages(roots), [roots]);
  const blackboardUsages = useMemo(() => collectBlackboardUsages(roots), [roots]);

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
            className={selectedClass(selected, 'parameter', parameter.parameter_id)}
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
              <small>{parameterUsages[parameter.parameter_id]?.length ?? 0}</small>
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
            className={selectedClass(selected, 'namespace', namespace.namespace_id)}
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
        {keys.length === 0 && <p>No keys</p>}
        {keys.map((key) => (
          <button
            key={key.key}
            type="button"
            className={selectedClass(selected, 'key', key.key)}
            onClick={() => onSelect({ kind: 'key', id: key.key })}
          >
            <span><strong>{key.key}</strong><small>{key.type}</small></span>
            <small>{blackboardUsages[key.key]?.length ?? 0}</small>
          </button>
        ))}
      </section>
    </nav>
  );
}

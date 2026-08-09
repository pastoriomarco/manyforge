import { useMemo, useState } from 'react';

import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramTreeNode,
} from '../types/api';
import { firstCatalogParameterValidationMessage } from '../utils/catalogParameterValidation';
import {
  catalogDefaultParameters,
  flattenProgramTree,
  nextProgramInstanceId,
} from '../utils/programAuthoring';
import { NodePaletteItem } from './NodePaletteItem';

type Placement = 'child' | 'sibling' | 'wrapper';

interface ProgramNodePaletteProps {
  root: ProgramTreeNode;
  catalogEntries: CatalogEntry[];
  environment: NodeEnvironmentResponse | null;
  selectedId: string | null;
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
}

function executionNote(
  entry: CatalogEntry,
  environment: NodeEnvironmentResponse | null,
): string {
  const rows = environment?.implementations.filter((row) => row.nodeId === entry.id) ?? [];
  if (rows.some((row) => row.executionAvailable)) return 'Execution available';
  const gates = [...new Set(rows.flatMap((row) => row.restorationGates))];
  return gates.length > 0
    ? `Authoring only · execution gate ${gates.join(', ')}`
    : 'Authoring only · execution unavailable';
}

function authoringAvailable(
  entry: CatalogEntry,
  environment: NodeEnvironmentResponse | null,
): boolean {
  if (!environment?.portableAuthoringAvailable) return false;
  return environment.implementations
    .filter((row) => row.nodeId === entry.id)
    .some((row) => row.authoringAvailable);
}

function parseParameterObject(
  text: string,
  entry: CatalogEntry,
): { parameters: Record<string, JsonValue> | null; error: string | null } {
  let value: unknown;
  try {
    value = JSON.parse(text);
  } catch (error) {
    return {
      parameters: null,
      error: error instanceof Error ? error.message : 'Parameters must be valid JSON.',
    };
  }
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    return { parameters: null, error: 'Parameters must be one JSON object.' };
  }
  const parameters = value as Record<string, JsonValue>;
  const known = new Set(entry.parameters.map((parameter) => parameter.id));
  const unexpected = Object.keys(parameters).filter((name) => !known.has(name));
  if (unexpected.length > 0) {
    return { parameters: null, error: `Unknown parameter(s): ${unexpected.join(', ')}` };
  }
  for (const parameter of entry.parameters) {
    const message = firstCatalogParameterValidationMessage(
      parameter,
      parameters[parameter.id],
    );
    if (message) return { parameters: null, error: message };
  }
  return { parameters, error: null };
}

export function ProgramNodePalette({
  root,
  catalogEntries,
  environment,
  selectedId,
  busy,
  onMutate,
}: ProgramNodePaletteProps) {
  const [query, setQuery] = useState('');
  const [stagedId, setStagedId] = useState<string | null>(null);
  const [instanceId, setInstanceId] = useState('');
  const [placement, setPlacement] = useState<Placement>('child');
  const [parameterText, setParameterText] = useState('{}');
  const [error, setError] = useState<string | null>(null);
  const flat = useMemo(() => flattenProgramTree(root), [root]);
  const selected = flat.find((row) => row.node.instance_id === selectedId) ?? null;
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
  const staged = stagedId === null ? null : catalog.get(stagedId) ?? null;
  const normalizedQuery = query.trim().toLocaleLowerCase();
  const filtered = catalogEntries.filter((entry) =>
    [entry.id, entry.name, entry.category, entry.kind, ...entry.tags]
      .some((value) => value.toLocaleLowerCase().includes(normalizedQuery)),
  );
  const byCategory = new Map<string, CatalogEntry[]>();
  for (const entry of filtered) {
    byCategory.set(entry.category, [
      ...(byCategory.get(entry.category) ?? []),
      entry,
    ]);
  }
  const grouped = [...byCategory.entries()]
    .sort(([left], [right]) => left.localeCompare(right));

  const placements = useMemo(() => {
    if (!staged || !selected) return [];
    const result: Placement[] = [];
    const parentEntry = catalog.get(selected.node.node_id);
    const childCapacity = parentEntry !== undefined
      && parentEntry.childrenMax !== 0
      && (
        parentEntry.childrenMax < 0
        || selected.node.children.length < parentEntry.childrenMax
      );
    if (childCapacity && staged.childrenMin === 0) result.push('child');
    if (selected.parentId !== null && staged.childrenMin === 0) result.push('sibling');
    if (
      staged.childrenMin <= 1
      && staged.childrenMax !== 0
      && (staged.childrenMax < 0 || staged.childrenMax >= 1)
    ) {
      result.push('wrapper');
    }
    return result;
  }, [catalog, selected, staged]);

  const begin = (entry: CatalogEntry) => {
    const existing = flat.map((row) => row.node.instance_id);
    setStagedId(entry.id);
    setInstanceId(nextProgramInstanceId(entry.id, existing));
    setParameterText(JSON.stringify(catalogDefaultParameters(entry), null, 2));
    setError(null);
    const preferred: Placement = selected
      && catalog.get(selected.node.node_id)?.childrenMax !== 0
      && entry.childrenMin === 0
      ? 'child'
      : entry.childrenMin <= 1 && entry.childrenMax !== 0
        ? 'wrapper'
        : 'sibling';
    setPlacement(preferred);
  };

  const commit = () => {
    if (!staged || !selected) {
      setError('Select a tree node before inserting a catalog entry.');
      return;
    }
    if (!placements.includes(placement)) {
      setError('That placement cannot produce a schema-valid tree node.');
      return;
    }
    if (!instanceId.trim()) {
      setError('Instance ID is required.');
      return;
    }
    const parsed = parseParameterObject(parameterText, staged);
    if (!parsed.parameters) {
      setError(parsed.error);
      return;
    }
    const node = {
      node_id: staged.id,
      instance_id: instanceId.trim(),
      parameters: parsed.parameters,
      children: [],
    };
    if (placement === 'wrapper') {
      onMutate('tree_draft_wrap_node', {
        target_instance_id: selected.node.instance_id,
        wrapper: {
          node_id: node.node_id,
          instance_id: node.instance_id,
          parameters: node.parameters,
        },
      });
    } else if (placement === 'child') {
      onMutate('tree_draft_insert_node', {
        parent_instance_id: selected.node.instance_id,
        index: selected.node.children.length,
        node,
      });
    } else {
      onMutate('tree_draft_insert_node', {
        after_instance_id: selected.node.instance_id,
        node,
      });
    }
    setStagedId(null);
    setError(null);
  };

  return (
    <section className="program-palette" aria-labelledby="program-palette-title">
      <header className="program-subpanel-header">
        <div>
          <p className="eyebrow">Active v2 environment</p>
          <h3 id="program-palette-title">Node palette</h3>
        </div>
        <span className="count-badge">{filtered.length}</span>
      </header>
      <label className="program-filter">
        <span>Find a node</span>
        <input
          type="search"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="Name, ID, category, or tag"
        />
      </label>
      <div className="program-palette-list">
        {grouped.map(([category, entries]) => (
          <section key={category}>
            <h4>{category}</h4>
            {entries.map((entry) => (
              <NodePaletteItem
                key={entry.id}
                entry={entry}
                disabled={busy || !authoringAvailable(entry, environment)}
                onActivate={begin}
                executionNote={executionNote(entry, environment)}
              />
            ))}
          </section>
        ))}
      </div>

      {staged && (
        <div className="program-staging-card">
          <div className="program-staging-heading">
            <strong>Add {staged.name}</strong>
            <button type="button" onClick={() => setStagedId(null)}>Cancel</button>
          </div>
          <p>
            Complete the node locally; it enters the Draft only after the exact
            catalog schema can validate it.
          </p>
          <label>
            Instance ID
            <input
              value={instanceId}
              onChange={(event) => setInstanceId(event.target.value)}
            />
          </label>
          <label>
            Placement relative to {selected?.node.instance_id ?? 'no selection'}
            <select
              value={placements.includes(placement) ? placement : ''}
              onChange={(event) => setPlacement(event.target.value as Placement)}
            >
              <option value="" disabled>Select a valid placement</option>
              {placements.includes('child') && <option value="child">Last child</option>}
              {placements.includes('sibling') && <option value="sibling">Next sibling</option>}
              {placements.includes('wrapper') && <option value="wrapper">Wrap selection</option>}
            </select>
          </label>
          <label>
            Parameters (strict JSON object)
            <textarea
              rows={8}
              value={parameterText}
              onChange={(event) => setParameterText(event.target.value)}
              spellCheck={false}
            />
          </label>
          {error && <p className="program-form-error" role="alert">{error}</p>}
          <button type="button" disabled={busy} onClick={commit}>Add to Draft</button>
        </div>
      )}
    </section>
  );
}

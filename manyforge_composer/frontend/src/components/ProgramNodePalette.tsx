import { useCallback, useEffect, useMemo, useRef, useState } from 'react';

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
import type {
  ProgramInsertionPlacement,
  ProgramInsertionNode,
  ProgramInsertionRequest,
  ProgramPaletteActivation,
} from '../utils/programCanvasInteraction';
import {
  programInsertionMutation,
  validProgramInsertionPlacements,
} from '../utils/programCanvasInteraction';
import { NodePaletteItem } from './NodePaletteItem';
import { OverlayPanel } from './OverlayPanel';

interface ProgramCatalogIdentity {
  catalogId: string;
  contentVersion: string;
  catalogDigest: string;
  environmentDigest: string;
}

interface ProgramNodePaletteProps {
  root: ProgramTreeNode;
  catalogEntries: CatalogEntry[];
  catalogIdentity: ProgramCatalogIdentity | null;
  environment: NodeEnvironmentResponse | null;
  selectedId: string | null;
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  dropRequest?: ProgramInsertionRequest | null;
  onDropRequestHandled?: () => void;
  onCollapse?: () => void;
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
  catalogIdentity,
  environment,
  selectedId,
  busy,
  onMutate,
  dropRequest = null,
  onDropRequestHandled,
  onCollapse,
}: ProgramNodePaletteProps) {
  const [query, setQuery] = useState('');
  const [stagedId, setStagedId] = useState<string | null>(null);
  const [instanceId, setInstanceId] = useState('');
  const [placement, setPlacement] = useState<ProgramInsertionPlacement>('child');
  const [stagedTargetId, setStagedTargetId] = useState<string | null>(null);
  const [parameterText, setParameterText] = useState('{}');
  const [error, setError] = useState<string | null>(null);
  const [infoOpen, setInfoOpen] = useState(false);
  const infoButton = useRef<HTMLButtonElement>(null);
  const closeInfo = useCallback(() => setInfoOpen(false), []);
  const flat = useMemo(() => flattenProgramTree(root), [root]);
  const selected = flat.find((row) => row.node.instance_id === selectedId) ?? null;
  const stagedTarget = stagedTargetId === null
    ? selected
    : flat.find((row) => row.node.instance_id === stagedTargetId) ?? null;
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

  const placements = useMemo(
    () => staged
      ? validProgramInsertionPlacements(root, catalogEntries, stagedTarget?.node.instance_id ?? null, staged)
      : [],
    [catalogEntries, root, staged, stagedTarget],
  );

  const begin = useCallback((
    activation: ProgramPaletteActivation,
    targetId: string | null = selectedId,
    preferredPlacement?: ProgramInsertionPlacement,
  ) => {
    const entry = catalog.get(activation.catalogId);
    if (!entry) {
      setError(`Unknown catalog entry '${activation.catalogId}'.`);
      return;
    }
    const existing = flat.map((row) => row.node.instance_id);
    setStagedId(entry.id);
    setStagedTargetId(targetId);
    setInstanceId(nextProgramInstanceId(entry.id, existing));
    setParameterText(JSON.stringify(catalogDefaultParameters(entry), null, 2));
    setError(null);
    const validPlacements = validProgramInsertionPlacements(
      root,
      catalogEntries,
      targetId,
      entry,
    );
    setPlacement(
      preferredPlacement && validPlacements.includes(preferredPlacement)
        ? preferredPlacement
        : validPlacements[0] ?? 'child',
    );
  }, [catalog, catalogEntries, flat, root, selectedId]);

  useEffect(() => {
    if (!dropRequest) return;
    const entry = catalog.get(dropRequest.catalogId);
    if (entry) {
      begin(
        { catalogId: entry.id },
        dropRequest.targetId,
        dropRequest.placement,
      );
    } else {
      setError(`Unknown catalog entry '${dropRequest.catalogId}'.`);
    }
    onDropRequestHandled?.();
  }, [begin, catalog, dropRequest, onDropRequestHandled]);

  const commit = () => {
    if (busy) return;
    if (!staged || !stagedTarget) {
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
    if (flat.some((row) => row.node.instance_id === instanceId.trim())) {
      setError(`Instance ID '${instanceId.trim()}' is already used in this Draft.`);
      return;
    }
    if (!authoringAvailable(staged, environment)) {
      setError('This catalog node is not available for portable authoring.');
      return;
    }
    const parsed = parseParameterObject(parameterText, staged);
    if (!parsed.parameters) {
      setError(parsed.error);
      return;
    }
    const node: ProgramInsertionNode = {
      node_id: staged.id,
      instance_id: instanceId.trim(),
      parameters: parsed.parameters,
      children: [],
    };
    const mutation = programInsertionMutation(
      stagedTarget.node.instance_id,
      placement,
      stagedTarget.node.children.length,
      node,
    );
    onMutate(mutation.mutation, mutation.args);
    setStagedId(null);
    setStagedTargetId(null);
    setError(null);
  };

  return (
    <section className="program-palette" aria-labelledby="program-palette-title">
      <header className="program-subpanel-header">
        <div className="program-subpanel-title">
          <h3 id="program-palette-title">Nodes</h3>
          <button
            ref={infoButton}
            type="button"
            className="icon-button"
            aria-label="Node catalog information"
            title="Node catalog information"
            onClick={() => setInfoOpen(true)}
          >
            i
          </button>
        </div>
        {onCollapse && (
          <button
            type="button"
            className="icon-button"
            aria-label="Collapse node palette"
            title="Collapse node palette"
            onClick={onCollapse}
          >
            ‹
          </button>
        )}
      </header>
      <OverlayPanel
        open={infoOpen}
        title="Node catalog"
        titleId="program-catalog-information-title"
        kind="info"
        onClose={closeInfo}
        returnFocusRef={infoButton}
      >
        <div className="catalog-information">
          <p>
            Immutable compiler declarations for Program authoring. Runtime and
            implementation availability remain owner-issued facts.
          </p>
          <dl>
            <div><dt>Entries</dt><dd>{catalogEntries.length}</dd></div>
            <div><dt>Catalog ID</dt><dd><code>{catalogIdentity?.catalogId ?? 'unavailable'}</code></dd></div>
            <div><dt>Content version</dt><dd><code>{catalogIdentity?.contentVersion ?? 'unavailable'}</code></dd></div>
            <div><dt>Catalog digest</dt><dd><code>{catalogIdentity?.catalogDigest ?? 'unavailable'}</code></dd></div>
            <div><dt>Environment digest</dt><dd><code>{catalogIdentity?.environmentDigest ?? 'unavailable'}</code></dd></div>
          </dl>
        </div>
      </OverlayPanel>
      <label className="program-filter">
        <span className="sr-only">Find a node</span>
        <input
          type="search"
          value={query}
          onChange={(event) => setQuery(event.target.value)}
          placeholder="Find a node"
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
                allowDrag
              />
            ))}
          </section>
        ))}
      </div>

      {staged && (
        <div className="program-staging-card">
          <div className="program-staging-heading">
            <strong>Add {staged.name}</strong>
            <button
              type="button"
              onClick={() => {
                setStagedId(null);
                setStagedTargetId(null);
              }}
            >
              Cancel
            </button>
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
            Placement relative to {stagedTarget?.node.instance_id ?? 'no selection'}
            <select
              value={placements.includes(placement) ? placement : ''}
              onChange={(event) => setPlacement(
                event.target.value as ProgramInsertionPlacement,
              )}
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
          <button type="button" disabled={busy || !stagedTarget} onClick={commit}>Add to Draft</button>
        </div>
      )}
    </section>
  );
}

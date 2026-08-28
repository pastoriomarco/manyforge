import { useCallback, useMemo, useRef, useState } from 'react';
import type { Ref } from 'react';

import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../types/api';
import { flattenProgramForest } from '../utils/programAuthoring';
import {
  programDetachedInsertionMutation,
  type ProgramPaletteActivation,
} from '../utils/programCanvasInteraction';
import { NodePaletteItem } from './NodePaletteItem';
import { OverlayPanel } from './OverlayPanel';
import { ProgramAuthoringTemplate } from './ProgramAuthoringTemplate';

interface ProgramCatalogIdentity {
  catalogId: string;
  contentVersion: string;
  catalogDigest: string;
  environmentDigest: string;
}

interface ProgramNodePaletteProps {
  roots: ProgramTreeNode[];
  program?: ProgramAuthoringDocument;
  catalogEntries: CatalogEntry[];
  catalogIdentity: ProgramCatalogIdentity | null;
  environment: NodeEnvironmentResponse | null;
  busy: boolean;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
  onCollapse?: () => void;
  collapseButtonRef?: Ref<HTMLButtonElement>;
  onViewChange?: (view: 'nodes' | 'params') => void;
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

export function ProgramNodePalette({
  roots,
  program,
  catalogEntries,
  catalogIdentity,
  environment,
  busy,
  onMutate,
  onCollapse,
  collapseButtonRef,
  onViewChange,
}: ProgramNodePaletteProps) {
  const [query, setQuery] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [infoOpen, setInfoOpen] = useState(false);
  const [collapsedCategories, setCollapsedCategories] = useState<Set<string>>(
    () => new Set(),
  );
  const infoButton = useRef<HTMLButtonElement>(null);
  const closeInfo = useCallback(() => setInfoOpen(false), []);
  const flat = useMemo(() => flattenProgramForest(roots), [roots]);
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
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

  const addDetached = useCallback((activation: ProgramPaletteActivation) => {
    if (busy) return;
    const entry = catalog.get(activation.catalogId);
    if (!entry) {
      setError(`Unknown catalog entry '${activation.catalogId}'.`);
      return;
    }
    if (!authoringAvailable(entry, environment)) {
      setError('This catalog node is not available for portable authoring.');
      return;
    }
    const mutation = programDetachedInsertionMutation(
      entry,
      flat.map((row) => row.node.instance_id),
      roots.length,
    );
    onMutate(mutation.mutation, mutation.args);
    setError(null);
  }, [busy, catalog, environment, flat, onMutate, roots.length]);

  return (
    <section className="program-palette" aria-labelledby="program-palette-title">
      <header className="program-subpanel-header">
        <div className="program-subpanel-title">
          <div id="program-palette-title" className="program-tool-tabs" aria-label="Program tools">
            <button type="button" className="active" aria-pressed="true">Nodes</button>
            <button type="button" aria-pressed="false" onClick={() => onViewChange?.('params')}>Params</button>
          </div>
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
            ref={collapseButtonRef}
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
      {program && (
        <ProgramAuthoringTemplate program={program} busy={busy} onMutate={onMutate} />
      )}
      <div className="program-palette-list">
        {grouped.map(([category, entries]) => {
          const collapsed = collapsedCategories.has(category);
          return (
          <section key={category}>
            <button
              type="button"
              className="program-palette-category"
              aria-expanded={!collapsed}
              onClick={() => setCollapsedCategories((current) => {
                const next = new Set(current);
                if (next.has(category)) next.delete(category);
                else next.add(category);
                return next;
              })}
            >
              <span aria-hidden="true">{collapsed ? '›' : '⌄'}</span>
              <strong>{category}</strong>
              <small>{entries.length}</small>
            </button>
            {!collapsed && entries.map((entry) => (
              <NodePaletteItem
                key={entry.id}
                entry={entry}
                disabled={busy || !authoringAvailable(entry, environment)}
                onActivate={addDetached}
                executionNote={executionNote(entry, environment)}
                allowDrag
              />
            ))}
          </section>
          );
        })}
      </div>

      {error && <p className="program-form-error palette-error" role="alert">{error}</p>}
    </section>
  );
}

import { memo, useCallback, useEffect, useMemo, useRef, useState } from 'react';
import type { Ref } from 'react';
import type { ReferenceSelectorFacts } from '../utils/referenceValidation';

import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramAuthoringDocument,
  DraftMutationResponse,
  ProgramTreeNode,
} from '../types/api';
import { flattenProgramForest } from '../utils/programAuthoring';
import {
  programDetachedInsertionMutation,
  type ProgramPaletteActivation,
} from '../utils/programCanvasInteraction';
import { NodePaletteItem } from './NodePaletteItem';
import { errorCatalogRow } from '../state/commandRefusal';
import { programCategoryFamily } from '../styles/tokens';
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
  selectorFacts?: ReferenceSelectorFacts | null;
  catalogEntries: CatalogEntry[];
  catalogIdentity: ProgramCatalogIdentity | null;
  environment: NodeEnvironmentResponse | null;
  busy: boolean;
  onMutate: (
    mutation: string,
    args: Record<string, JsonValue>,
  ) => Promise<DraftMutationResponse | null>;
  onSelect?: (instanceId: string) => void;
  onCollapse?: () => void;
  collapseButtonRef?: Ref<HTMLButtonElement>;
  onViewChange?: (view: 'nodes' | 'params') => void;
}

interface NodeExecutionAvailability {
  available: boolean;
  note: string;
}

export function executionAvailability(
  entry: CatalogEntry,
  environment: NodeEnvironmentResponse | null,
): NodeExecutionAvailability {
  const rows = environment?.implementations.filter((row) => row.nodeId === entry.id) ?? [];
  if (rows.some((row) => row.executionAvailable)) {
    const requiredInputs = entry.parameters.filter(
      (parameter) => parameter.required && !parameter.hasDefault,
    ).length;
    return {
      available: true,
      note: requiredInputs > 0
        ? `Executable when configured · ${requiredInputs} required input${requiredInputs === 1 ? '' : 's'}`
        : 'Execution available for the current binding',
    };
  }
  const typedReason = rows.flatMap((row) => row.reasons)[0];
  if (typedReason !== undefined) {
    return {
      available: false,
      note: `Authorable · unavailable · ${errorCatalogRow(typedReason.code)?.[0] ?? typedReason.message}${typeof typedReason.details?.capability === 'string' ? ` (${typedReason.details.capability})` : ''}`,
    };
  }
  // Restoration gates are internal codes: the note only says whether any apply.
  const gated = rows.some((row) => row.restorationGates.length > 0);
  const reasons = [...new Set(rows.flatMap((row) => row.reasonCodes))];
  const boundedReason = reasons[0]?.replaceAll('_', ' ') ?? 'no current execution offer';
  return {
    available: false,
    note: gated
      ? `Authorable · unavailable in this version · ${boundedReason}`
      : `Authorable · unavailable · ${boundedReason}`,
  };
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

// Display names that differ from the catalog category id.
const CATEGORY_LABELS: Readonly<Record<string, string>> = {
  recovery: 'Operation control',
};

function categoryLabel(category: string): string {
  const label = CATEGORY_LABELS[category];
  if (label !== undefined) return label;
  return category.split(/[_-]+/).filter(Boolean).map((part) => (
    part.length <= 2
      ? part.toLocaleUpperCase()
      : `${part[0]?.toLocaleUpperCase() ?? ''}${part.slice(1).toLocaleLowerCase()}`
  )).join(' ');
}

export function defaultCollapsedCatalogCategories(
  entries: CatalogEntry[],
  environment: NodeEnvironmentResponse | null,
): Set<string> {
  if (environment === null) return new Set();
  const availabilityByCategory = new Map<string, boolean[]>();
  for (const entry of entries) {
    availabilityByCategory.set(entry.category, [
      ...(availabilityByCategory.get(entry.category) ?? []),
      executionAvailability(entry, environment).available,
    ]);
  }
  return new Set([...availabilityByCategory.entries()]
    .filter(([, availability]) => (
      availability.length > 0 && availability.every((available) => !available)
    ))
    .map(([category]) => category));
}

// Drawn chevrons: text glyphs sit on font metrics and are not centered. The
// Collapse all / Expand all button doubles the category toggle's arrow.
function PaletteChevron({ up, double = false }: { up: boolean; double?: boolean }) {
  const d = double
    ? (up ? 'M4 7.75 8 4.25 12 7.75M4 11.75 8 8.25 12 11.75' : 'M4 4.25 8 7.75 12 4.25M4 8.25 8 11.75 12 8.25')
    : (up ? 'M4 10 8 6 12 10' : 'M4 6 8 10 12 6');
  return (
    <svg className="program-palette-chevron" viewBox="0 0 16 16" aria-hidden="true">
      <path d={d} />
    </svg>
  );
}

export const ProgramNodePalette = memo(function ProgramNodePalette({
  roots,
  program,
  selectorFacts,
  catalogEntries,
  catalogIdentity,
  environment,
  busy,
  onMutate,
  onSelect,
  onCollapse,
  collapseButtonRef,
  onViewChange,
}: ProgramNodePaletteProps) {
  const [query, setQuery] = useState('');
  const [error, setError] = useState<string | null>(null);
  const [infoOpen, setInfoOpen] = useState(false);
  const [collapsedCategories, setCollapsedCategories] = useState<Set<string>>(
    () => defaultCollapsedCatalogCategories(catalogEntries, environment),
  );
  const [templatesCollapsed, setTemplatesCollapsed] = useState(true);
  const categoryOverrides = useRef(new Set<string>());
  const defaultAvailabilityKey = useRef<string | null>(null);
  const infoButton = useRef<HTMLButtonElement>(null);
  const closeInfo = useCallback(() => setInfoOpen(false), []);
  useEffect(() => {
    if (environment === null) return;
    // Availability changes without a new environment digest (an empty start, then
    // a Load), so the key includes each entry's current execution availability.
    const key = `${environment.environmentDigest}\u0000${catalogEntries
      .map((entry) => `${entry.id}:${executionAvailability(entry, environment).available}`)
      .sort().join('\u0000')}`;
    if (defaultAvailabilityKey.current === key) return;
    defaultAvailabilityKey.current = key;
    const defaults = defaultCollapsedCatalogCategories(catalogEntries, environment);
    const categories = new Set(catalogEntries.map((entry) => entry.category));
    setCollapsedCategories((current) => {
      const next = new Set(current);
      for (const category of categories) {
        if (categoryOverrides.current.has(category)) continue;
        if (defaults.has(category)) next.add(category);
        else next.delete(category);
      }
      return next;
    });
  }, [catalogEntries, environment]);
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
  const allCollapsed = (program === undefined || templatesCollapsed)
    && grouped.every(([category]) => collapsedCategories.has(category));
  const toggleAll = () => {
    const categories = grouped.map(([category]) => category);
    for (const category of categories) categoryOverrides.current.add(category);
    setTemplatesCollapsed(!allCollapsed);
    setCollapsedCategories((current) => {
      const next = new Set(current);
      for (const category of categories) {
        if (allCollapsed) next.delete(category);
        else next.add(category);
      }
      return next;
    });
  };

  const addDetached = useCallback(async (activation: ProgramPaletteActivation) => {
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
    const response = await onMutate(mutation.mutation, mutation.args);
    if (response !== null) {
      const node = mutation.args.node as Record<string, JsonValue>;
      onSelect?.(String(node.instance_id));
      setError(null);
    }
  }, [busy, catalog, environment, flat, onMutate, onSelect, roots.length]);

  return (
    <section className="program-palette" aria-labelledby="program-palette-title">
      <header className="program-subpanel-header">
        <div className="program-subpanel-title">
          <div id="program-palette-title" className="program-tool-tabs dseg" aria-label="Program tools">
            <button type="button" className="active" aria-pressed="true">Nodes</button>
            <button type="button" aria-pressed="false" onClick={() => onViewChange?.('params')}>Params</button>
          </div>
        </div>
        <div className="program-subpanel-actions">
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
          <button
            type="button"
            className="icon-button"
            aria-label={allCollapsed ? 'Expand all node categories' : 'Collapse all node categories'}
            title={allCollapsed ? 'Expand all' : 'Collapse all'}
            onClick={toggleAll}
          >
            <PaletteChevron up={allCollapsed} double />
          </button>
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
        </div>
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
        <ProgramAuthoringTemplate
          program={program}
          selectorFacts={selectorFacts}
          busy={busy}
          onMutate={onMutate}
          collapsed={templatesCollapsed}
          onCollapsedChange={setTemplatesCollapsed}
        />
      )}
      <div className="program-palette-list">
        {grouped.map(([category, entries]) => {
          const collapsed = collapsedCategories.has(category);
          return (
          <section key={category}>
            <header className="program-palette-category" data-category-family={programCategoryFamily(category)}>
              <strong>{categoryLabel(category)}</strong>
              <div>
                <small>{entries.length}</small>
                <button
                  type="button"
                  className="icon-button program-palette-category-toggle"
                  aria-label={`${collapsed ? 'Expand' : 'Collapse'} ${categoryLabel(category)} nodes`}
                  aria-expanded={!collapsed}
                  title={`${collapsed ? 'Expand' : 'Collapse'} ${categoryLabel(category)} nodes`}
                  onClick={() => setCollapsedCategories((current) => {
                    categoryOverrides.current.add(category);
                    const next = new Set(current);
                    if (next.has(category)) next.delete(category);
                    else next.add(category);
                    return next;
                  })}
                >
                  <PaletteChevron up={collapsed} />
                </button>
              </div>
            </header>
            {!collapsed && entries.map((entry) => {
              const execution = executionAvailability(entry, environment);
              return (
                <NodePaletteItem
                  key={entry.id}
                  entry={entry}
                  disabled={busy || !authoringAvailable(entry, environment)}
                  onActivate={addDetached}
                  executionNote={execution.note}
                  executionAvailable={execution.available}
                  allowDrag
                />
              );
            })}
          </section>
          );
        })}
      </div>

      {error && <p className="program-form-error inline-message severity-error palette-error" role="alert">{error}</p>}
    </section>
  );
});

import { useMemo, useState } from 'react';
import type { CatalogEntry, CatalogResponse } from '../types/api';
import type { StructuralResourceState } from '../state/structuralResource';

interface CatalogPanelProps {
  catalog: StructuralResourceState<CatalogResponse>;
  onRetry: () => void;
}

const EMPTY_CATALOG_ENTRIES: readonly CatalogEntry[] = [];

function shortDigest(digest: string): string {
  return digest.length > 22 ? `${digest.slice(0, 18)}…` : digest;
}

function CatalogEntryDetails({ entry }: { entry: CatalogEntry }) {
  return (
    <div className="catalog-details">
      <div className="catalog-detail-heading">
        <div>
          <p className="eyebrow">{entry.category}</p>
          <h3>{entry.name}</h3>
        </div>
        <span className={`status-pill stability-${entry.stability}`}>
          {entry.stability}
        </span>
      </div>
      <p>{entry.description || 'No authored description.'}</p>
      <dl className="compact-definition-list">
        <div><dt>Stable ID</dt><dd>{entry.id}</dd></div>
        <div><dt>Kind</dt><dd>{entry.kind}</dd></div>
        <div>
          <dt>Children</dt>
          <dd>{entry.childrenMin}–{entry.childrenMax}</dd>
        </div>
        <div><dt>Owner API</dt><dd>{entry.execution.ownerApi}</dd></div>
        <div>
          <dt>Execution declaration</dt>
          <dd>
            {entry.execution.blocking ? 'blocking' : 'non-blocking'} · {' '}
            {entry.execution.cancellable ? 'cancellable' : 'not cancellable'}
          </dd>
        </div>
      </dl>

      <div className="catalog-detail-section">
        <h4>Parameters ({entry.parameters.length})</h4>
        {entry.parameters.length === 0 ? (
          <p className="muted">No parameters.</p>
        ) : (
          <ul className="detail-list">
            {entry.parameters.map((parameter) => (
              <li key={parameter.id}>
                <code>{parameter.id}</code>
                <span>{parameter.required ? 'required' : 'optional'}</span>
                <span title={parameter.schemaHash}>{shortDigest(parameter.schemaHash)}</span>
              </li>
            ))}
          </ul>
        )}
      </div>

      <div className="catalog-detail-section">
        <h4>Required capabilities ({entry.requiredCapabilities.length})</h4>
        {entry.requiredCapabilities.length === 0 ? (
          <p className="muted">None declared.</p>
        ) : (
          <ul className="detail-list">
            {entry.requiredCapabilities.map((requirement, index) => (
              <li key={`${requirement.capabilityId}:${requirement.scope}:${index}`}>
                <code>{requirement.capabilityId}</code>
                <span>{requirement.scope}</span>
              </li>
            ))}
          </ul>
        )}
      </div>

      {entry.tags.length > 0 && (
        <div className="tag-row" aria-label="Catalog tags">
          {entry.tags.map((tag) => <span key={tag}>{tag}</span>)}
        </div>
      )}
    </div>
  );
}

export function CatalogPanel({ catalog, onRetry }: CatalogPanelProps) {
  const [query, setQuery] = useState('');
  const [category, setCategory] = useState('all');
  const [selectedId, setSelectedId] = useState<string | null>(null);

  const entries = catalog.status === 'ready' ? catalog.data.entries : EMPTY_CATALOG_ENTRIES;
  const categories = useMemo(
    () => [...new Set(entries.map((entry) => entry.category))].sort(),
    [entries],
  );
  const filtered = useMemo(() => {
    const normalizedQuery = query.trim().toLocaleLowerCase();
    return entries.filter((entry) => {
      if (category !== 'all' && entry.category !== category) {
        return false;
      }
      if (!normalizedQuery) {
        return true;
      }
      return [entry.id, entry.name, entry.description, entry.kind, ...entry.tags]
        .some((value) => value.toLocaleLowerCase().includes(normalizedQuery));
    });
  }, [category, entries, query]);
  const selected = filtered.find((entry) => entry.id === selectedId)
    ?? filtered[0]
    ?? null;

  return (
    <section className="panel catalog-panel" aria-labelledby="catalog-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Immutable compiler product</p>
          <h2 id="catalog-title">Node catalog</h2>
        </div>
        {catalog.status === 'ready' && (
          <span className="count-badge">{catalog.data.entries.length} entries</span>
        )}
      </header>

      {catalog.status === 'loading' && (
        <div className="panel-state" role="status">Loading the v2 catalog…</div>
      )}
      {catalog.status === 'error' && (
        <div className="panel-state error-state" role="alert">
          <strong>Catalog unavailable</strong>
          <span>{catalog.message}</span>
          <button type="button" onClick={onRetry}>Retry catalog</button>
        </div>
      )}
      {catalog.status === 'absent' && (
        <div className="panel-state">{catalog.message}</div>
      )}

      {catalog.status === 'ready' && (
        <>
          <div className="catalog-identity">
            <span>{catalog.data.catalogId}</span>
            <span>content {catalog.data.contentVersion}</span>
            <span title={catalog.data.catalogDigest}>
              catalog {shortDigest(catalog.data.catalogDigest)}
            </span>
            <span title={catalog.data.environmentDigest}>
              environment {shortDigest(catalog.data.environmentDigest)}
            </span>
          </div>
          <p className="notice severity-info notice-strip">
            Schema and execution declarations are shown for authoring reference;
            they do not assert that an implementation or runtime is available.
          </p>
          <div className="catalog-filters">
            <label>
              <span>Find an entry</span>
              <input
                type="search"
                value={query}
                onChange={(event) => setQuery(event.target.value)}
                placeholder="Name, ID, kind, or tag"
              />
            </label>
            <label>
              <span>Category</span>
              <select
                value={category}
                onChange={(event) => setCategory(event.target.value)}
              >
                <option value="all">All categories</option>
                {categories.map((value) => (
                  <option key={value} value={value}>{value}</option>
                ))}
              </select>
            </label>
          </div>

          {filtered.length === 0 ? (
            <div className="panel-state">No catalog entries match this filter.</div>
          ) : (
            <div className="catalog-browser">
              <div className="catalog-entry-list" role="list" aria-label="Catalog entries">
                {filtered.map((entry) => (
                  <button
                    key={entry.id}
                    type="button"
                    role="listitem"
                    className={`dpick${selected?.id === entry.id ? ' selected' : ''}`}
                    onClick={() => setSelectedId(entry.id)}
                  >
                    <span>{entry.name}</span>
                    <small>{entry.id} · {entry.kind}</small>
                  </button>
                ))}
              </div>
              {selected && <CatalogEntryDetails entry={selected} />}
            </div>
          )}
        </>
      )}
    </section>
  );
}

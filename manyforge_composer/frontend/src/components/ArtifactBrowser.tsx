import { useEffect, useState } from 'react';
import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import { useOperationalErrorMirror } from '../state/operationalErrors';
import type {
  ArtifactKind,
  FileBrowserEntry,
  FileBrowserResponse,
} from '../types/api';

type BrowserState =
  | { status: 'loading' }
  | { status: 'ready'; data: FileBrowserResponse }
  | { status: 'error'; message: string };

interface ArtifactBrowserProps {
  initialKind?: ArtifactKind;
  lockedKind?: ArtifactKind;
  onSelectionChange?: (
    kind: ArtifactKind,
    entry: FileBrowserEntry | null,
  ) => void;
}

export function ArtifactBrowser({
  initialKind = 'deployment',
  lockedKind,
  onSelectionChange,
}: ArtifactBrowserProps) {
  const [kind, setKind] = useState<ArtifactKind>(lockedKind ?? initialKind);
  const [browsePath, setBrowsePath] = useState<string | undefined>(undefined);
  const [pathInput, setPathInput] = useState('');
  const [selected, setSelected] = useState<FileBrowserEntry | null>(null);
  const [reloadToken, setReloadToken] = useState(0);
  const [state, setState] = useState<BrowserState>({ status: 'loading' });
  useOperationalErrorMirror({
    source: `composer.artifact_browser.${kind}`,
    title: 'Artifact browser response failed',
    message: state.status === 'error' ? state.message : null,
    onDismiss: () => setReloadToken((current) => current + 1),
  });

  useEffect(() => {
    const controller = new AbortController();
    let current = true;
    setState({ status: 'loading' });
    void api.files.browse(kind, browsePath, controller.signal)
      .then((data) => {
        if (!current) {
          return;
        }
        setState({ status: 'ready', data });
        setPathInput(data.currentPath);
        setSelected(null);
        onSelectionChange?.(kind, null);
      })
      .catch((error: unknown) => {
        if (current && !controller.signal.aborted) {
          setState({
            status: 'error',
            message: errorMessage(error, 'Artifact browser request failed.'),
          });
        }
      });
    return () => {
      current = false;
      controller.abort();
    };
  }, [browsePath, kind, onSelectionChange, reloadToken]);

  const navigate = (path: string | undefined) => {
    setBrowsePath(path);
    setSelected(null);
    onSelectionChange?.(kind, null);
  };

  const chooseEntry = (entry: FileBrowserEntry) => {
    if (entry.entryType === 'directory') {
      navigate(entry.path);
      return;
    }
    if (entry.selectable) {
      setSelected(entry);
      onSelectionChange?.(kind, entry);
    }
  };

  return (
    <section className="panel artifact-panel" aria-labelledby="artifact-title">
      <header className="panel-header">
        <div>
          <p className="eyebrow">Server-sandboxed read only</p>
          <h2 id="artifact-title">Artifact browser</h2>
        </div>
        {lockedKind === undefined ? (
          <div className="segmented-control" aria-label="Artifact kind">
            {(['deployment', 'program'] as const).map((value) => (
              <button
                key={value}
                type="button"
                className={kind === value ? 'active' : ''}
                aria-pressed={kind === value}
                onClick={() => {
                  setKind(value);
                  setBrowsePath(undefined);
                  setSelected(null);
                  onSelectionChange?.(value, null);
                }}
              >
                {value}
              </button>
            ))}
          </div>
        ) : <span className="status-pill">{lockedKind}</span>}
      </header>

      <p className="panel-intro">
        Browse compatible artifact paths inside backend-approved roots. Selecting a
        file records only a local reference; it does not load, save, apply, or run it.
      </p>

      {state.status === 'ready' && state.data.roots.length > 0 && (
        <div className="root-shortcuts" aria-label="Allowed root shortcuts">
          {state.data.roots.map((root) => (
            <button key={root} type="button" onClick={() => navigate(root)}>
              {root}
            </button>
          ))}
        </div>
      )}

      <form
        className="path-browser"
        onSubmit={(event) => {
          event.preventDefault();
          navigate(pathInput.trim() || undefined);
          setReloadToken((current) => current + 1);
        }}
      >
        <button
          type="button"
          disabled={state.status !== 'ready' || state.data.parentPath === null}
          onClick={() => {
            if (state.status === 'ready') {
              navigate(state.data.parentPath ?? undefined);
            }
          }}
        >
          Up
        </button>
        <label>
          <span className="sr-only">Directory path</span>
          <input
            value={pathInput}
            onChange={(event) => setPathInput(event.target.value)}
            placeholder="Backend-approved directory path"
          />
        </label>
        <button type="submit">Browse</button>
        <button
          type="button"
          className="quiet-button"
          onClick={() => setReloadToken((current) => current + 1)}
        >
          Refresh
        </button>
      </form>

      {state.status === 'loading' && (
        <div className="panel-state" role="status">Reading directory…</div>
      )}
      {state.status === 'error' && (
        <div className="panel-state error-state" role="alert">
          <strong>Directory unavailable</strong>
          <span>{state.message}</span>
          <span className="muted">
            Enter a path within one of the server-configured roots and retry.
          </span>
        </div>
      )}
      {state.status === 'ready' && state.data.entries.length === 0 && (
        <div className="panel-state">No compatible artifacts in this directory.</div>
      )}
      {state.status === 'ready' && state.data.entries.length > 0 && (
        <div className="artifact-list" role="list">
          {state.data.entries.map((entry) => (
            <button
              key={entry.path}
              type="button"
              role="listitem"
              className={selected?.path === entry.path ? 'selected' : ''}
              onClick={() => chooseEntry(entry)}
            >
              <span className="entry-kind" aria-hidden="true">
                {entry.entryType === 'directory' ? 'DIR' : 'FILE'}
              </span>
              <span className="entry-copy">
                <strong>{entry.name}</strong>
                <small>{entry.path}</small>
              </span>
              <span aria-hidden="true">{entry.entryType === 'directory' ? '›' : ''}</span>
            </button>
          ))}
        </div>
      )}

      <div className="selected-artifact" aria-live="polite">
        <span>Selected read-only reference</span>
        <code>{selected?.path ?? 'none'}</code>
      </div>
    </section>
  );
}

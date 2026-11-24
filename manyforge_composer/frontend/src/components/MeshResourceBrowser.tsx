import { useEffect, useState } from 'react';

import { api } from '../api/client';
import type { FileBrowserResponse } from '../types/api';

/**
 * Browse the mesh files in the backend's allowed folders and pick one: a
 * Scene object's mesh, or a generated tool part's mesh (`tool_mesh`: .stl,
 * .dae). `onSelect` receives the file's absolute path and the allowed
 * folders; the caller writes the path its document expects. `startPath`
 * opens it in that folder (e.g. the Scenario's); the approved folders are
 * offered as shortcuts.
 */
export function MeshResourceBrowser({
  kind = 'mesh',
  startPath,
  onSelect,
}: {
  kind?: 'mesh' | 'tool_mesh';
  startPath?: string | null;
  onSelect: (path: string, roots: string[]) => void;
}) {
  const [path, setPath] = useState<string | undefined>(startPath ?? undefined);
  const [state, setState] = useState<
    { kind: 'loading' }
    | { kind: 'ready'; data: FileBrowserResponse }
    | { kind: 'error'; message: string }
  >({ kind: 'loading' });
  useEffect(() => {
    const controller = new AbortController();
    setState({ kind: 'loading' });
    void api.files.browse(kind, path, controller.signal).then((data) => {
      setState({ kind: 'ready', data });
    }).catch((error: unknown) => {
      if (!controller.signal.aborted) {
        setState({
          kind: 'error',
          message: error instanceof Error ? error.message : 'Mesh directory unavailable.',
        });
      }
    });
    return () => controller.abort();
  }, [kind, path]);
  return (
    <div
      className="scene-mesh-browser"
      onPointerDownCapture={(event) => event.preventDefault()}
    >
      {state.kind === 'loading' && <span className="muted">Reading approved mesh files…</span>}
      {state.kind === 'error' && <span className="scene-commit-error inline-message severity-error" role="alert">{state.message}</span>}
      {state.kind === 'ready' && (
        <>
          <p className="field-help">
            Open folders and click a {kind === 'tool_mesh' ? '.stl or .dae' : 'mesh'} file to use it.
            Approved folders:
          </p>
          <div className="button-row">
            {state.data.roots.map((root) => (
              <button type="button" key={root} disabled={root === state.data.currentPath}
                onClick={() => setPath(root)}>{root}</button>
            ))}
          </div>
          <div className="scene-mesh-browser-path">
            <button
              type="button"
              disabled={state.data.parentPath === null}
              onClick={() => setPath(state.data.parentPath ?? undefined)}
            >Up</button>
            <code>{state.data.currentPath}</code>
          </div>
          <div className="scene-mesh-browser-entries">
            {state.data.entries.map((entry) => (
              <button
                type="button"
                key={entry.path}
                onClick={() => {
                  if (entry.entryType === 'directory') setPath(entry.path);
                  else onSelect(entry.path, state.data.roots);
                }}
              >
                <span>{entry.entryType === 'directory' ? 'DIR' : 'MESH'}</span>
                <strong>{entry.name}</strong>
              </button>
            ))}
            {state.data.entries.length === 0 && (
              <span className="muted">No compatible meshes in this directory.</span>
            )}
          </div>
        </>
      )}
    </div>
  );
}

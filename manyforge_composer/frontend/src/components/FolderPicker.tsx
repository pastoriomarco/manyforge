import { useEffect, useState } from 'react';

import { api } from '../api/client';
import { errorMessage } from '../state/structuralResource';
import type { FileBrowserResponse } from '../types/api';

type PickerState =
  | { status: 'loading' }
  | { status: 'ready'; data: FileBrowserResponse }
  | { status: 'error'; message: string };

/**
 * Chooses one existing folder inside the backend-approved roots. It lists
 * folders only and never creates, reads or changes files.
 */
export function FolderPicker({
  initialPath,
  onChoose,
  onCancel,
}: {
  initialPath: string | null;
  onChoose: (path: string) => void;
  onCancel: () => void;
}) {
  const [path, setPath] = useState<string | undefined>(initialPath ?? undefined);
  const [state, setState] = useState<PickerState>({ status: 'loading' });

  useEffect(() => {
    const controller = new AbortController();
    setState({ status: 'loading' });
    api.files.browse('program', path, controller.signal).then(
      (data) => setState({ status: 'ready', data }),
      (error: unknown) => {
        if (!controller.signal.aborted) {
          setState({ status: 'error', message: errorMessage(error, 'Folder could not be read.') });
        }
      },
    );
    return () => controller.abort();
  }, [path]);

  const folders = state.status === 'ready'
    ? state.data.entries.filter((entry) => entry.entryType === 'directory')
    : [];

  return (
    <div className="folder-picker" role="group" aria-label="Choose a folder">
      <div className="folder-picker-bar">
        <button
          type="button"
          disabled={state.status !== 'ready' || state.data.parentPath === null}
          onClick={() => {
            if (state.status === 'ready' && state.data.parentPath !== null) setPath(state.data.parentPath);
          }}
          title="Up one folder"
        >↑ Up</button>
        <code title={state.status === 'ready' ? state.data.currentPath : undefined}>
          {state.status === 'ready' ? state.data.currentPath : '…'}
        </code>
      </div>
      {state.status === 'ready' && state.data.roots.length > 1 && (
        <div className="folder-picker-roots" aria-label="Allowed roots">
          {state.data.roots.map((root) => (
            <button key={root} type="button" onClick={() => setPath(root)}>{root}</button>
          ))}
        </div>
      )}
      <div className="folder-picker-list" role="list">
        {state.status === 'loading' && <p className="muted">Reading folder…</p>}
        {state.status === 'error' && <p className="inline-message severity-error" role="alert">{state.message}</p>}
        {state.status === 'ready' && folders.length === 0 && <p className="muted">No subfolders.</p>}
        {folders.map((entry) => (
          <button
            key={entry.path}
            type="button"
            role="listitem"
            className="dpick"
            onClick={() => setPath(entry.path)}
          >
            <span aria-hidden="true">▸</span> {entry.name}
          </button>
        ))}
      </div>
      <div className="dactions">
        <button type="button" onClick={onCancel}>Cancel</button>
        <button
          type="button"
          className="primary-action"
          disabled={state.status !== 'ready'}
          onClick={() => {
            if (state.status === 'ready') onChoose(state.data.currentPath);
          }}
        >Use this folder</button>
      </div>
    </div>
  );
}

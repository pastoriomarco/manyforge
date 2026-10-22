import { useEffect, useState } from 'react';

import { api } from '../api/client';
import { SCENARIO_PROJECT_SUFFIX } from '../state/newScenario';
import { errorMessage } from '../state/structuralResource';
import type { FileBrowserResponse } from '../types/api';

type PickerState =
  | { status: 'loading' }
  | { status: 'ready'; data: FileBrowserResponse }
  | { status: 'error'; message: string };

/**
 * Chooses one existing folder inside the backend-approved roots. It lists
 * folders only and never creates, reads or changes files. Scenario project
 * folders are shown but not entered: a project is never put inside another.
 * With `chooseProject`, a Scenario project itself is the choice (Load) and
 * ordinary folders are only navigated.
 */
export function FolderPicker({
  initialPath,
  onChoose,
  onCancel,
  chooseProject = false,
}: {
  initialPath: string | null;
  onChoose: (path: string) => void;
  onCancel: () => void;
  chooseProject?: boolean;
}) {
  const [path, setPath] = useState<string | undefined>(initialPath ?? undefined);
  const [state, setState] = useState<PickerState>({ status: 'loading' });

  useEffect(() => {
    const controller = new AbortController();
    setState({ status: 'loading' });
    api.files.browse('scenario', path, controller.signal).then(
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
  const insideProject = state.status === 'ready'
    && state.data.currentPath.replace(/\/+$/, '').endsWith(SCENARIO_PROJECT_SUFFIX);

  return (
    <div className="folder-picker" role="group" aria-label={chooseProject ? 'Choose a Scenario project' : 'Choose a folder'}>
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
      {state.status === 'ready' && state.data.roots.length > 0 && (
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
        {folders.map((entry) => (entry.scenarioProject ? (
          <button
            key={entry.path}
            type="button"
            role="listitem"
            className="dpick"
            disabled={!chooseProject}
            title={chooseProject ? 'Choose this Scenario project' : 'Scenario project'}
            onClick={chooseProject ? () => onChoose(entry.path) : undefined}
          >
            <span aria-hidden="true">◆</span> {entry.name}
          </button>
        ) : (
          <button
            key={entry.path}
            type="button"
            role="listitem"
            className="dpick"
            onClick={() => setPath(entry.path)}
          >
            <span aria-hidden="true">▸</span> {entry.name}
          </button>
        )))}
      </div>
      {insideProject && !chooseProject && (
        <p className="field-error" role="status">This is a Scenario project. Choose the folder that contains it.</p>
      )}
      <div className="dactions">
        <button type="button" onClick={onCancel}>Cancel</button>
        {!chooseProject && <button
          type="button"
          className="primary-action"
          disabled={state.status !== 'ready' || insideProject}
          onClick={() => {
            if (state.status === 'ready' && !insideProject) onChoose(state.data.currentPath);
          }}
        >Use this folder</button>}
      </div>
    </div>
  );
}

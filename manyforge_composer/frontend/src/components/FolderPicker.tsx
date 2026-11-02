import { useEffect, useId, useState } from 'react';

import { api } from '../api/client';
import { SCENARIO_PROJECT_SUFFIX } from '../state/newScenario';
import { errorMessage } from '../state/structuralResource';
import type { FileBrowserResponse } from '../types/api';

type PickerState =
  | { status: 'loading' }
  | { status: 'ready'; data: FileBrowserResponse }
  | { status: 'error'; message: string };

const LAST_FOLDER_KEY = 'manyforge.scenario.last-folder';

function readLastFolder(): string | null {
  try { return globalThis.sessionStorage?.getItem(LAST_FOLDER_KEY) ?? null; } catch { return null; }
}

function rememberLastFolder(path: string): void {
  try { globalThis.sessionStorage?.setItem(LAST_FOLDER_KEY, path); } catch { /* Storage is optional. */ }
}

function rootName(root: string): string {
  if (root === '/workspace/examples') return 'Examples';
  return root.replace(/\/+$/, '').split('/').pop() || root;
}

// Resolve relative paths only against the currently open folder. The backend
// resolves and confines both relative and absolute destinations.
function resolvePathInput(value: string, currentFolder: string | null): string | null {
  const path = value.trim();
  if (!path) return null;
  if (path.startsWith('/')) return path;
  return currentFolder === null ? null : `${currentFolder.replace(/\/+$/, '')}/${path}`;
}

/**
 * Chooses one existing folder inside the backend-approved roots. It lists
 * folders only and never creates, reads or changes files. Scenario project
 * folders are shown but not entered: a project is never put inside another.
 * With `chooseProject`, a Scenario project itself is the choice (Load) and
 * ordinary folders are only navigated.
 */
export function FolderPicker({
  onChoose,
  onCancel,
  chooseProject = false,
}: {
  onChoose: (path: string) => void;
  onCancel: () => void;
  chooseProject?: boolean;
}) {
  // A fresh object also lets Go retry the same path after a read error.
  const [request, setRequest] = useState<{ path?: string }>({});
  const [choosingRoot, setChoosingRoot] = useState(true);
  const [roots, setRoots] = useState<string[]>([]);
  const [shortcuts, setShortcuts] = useState<string[]>([]);
  const [pathText, setPathText] = useState('');
  const [lastFolder, setLastFolder] = useState<string | null>(readLastFolder);
  const [pathError, setPathError] = useState<string | null>(null);
  const [state, setState] = useState<PickerState>({ status: 'loading' });
  const [projectRoots, setProjectRoots] = useState<NonNullable<FileBrowserResponse['projectRoots']>>([]);
  const [helpOpen, setHelpOpen] = useState(false);
  const helpId = useId();

  useEffect(() => {
    const controller = new AbortController();
    setState({ status: 'loading' });
    api.files.browse('scenario', request.path, controller.signal, request.path === undefined).then(
      (data) => {
        if (controller.signal.aborted) return;
        setState({ status: 'ready', data });
        setShortcuts(data.roots);
        setProjectRoots(data.projectRoots ?? []);
        if (request.path === undefined) {
          setRoots(data.roots);
          return;
        }
        setPathText(data.currentPath);
        const folder = data.currentPath.replace(/\/+$/, '').endsWith(SCENARIO_PROJECT_SUFFIX)
          ? data.parentPath : data.currentPath;
        if (folder !== null) {
          setLastFolder(folder);
          rememberLastFolder(folder);
        }
      },
      (error: unknown) => {
        if (!controller.signal.aborted) {
          setState({ status: 'error', message: errorMessage(error, 'Folder could not be read.') });
        }
      },
    );
    return () => controller.abort();
  }, [request]);

  const navigate = (path: string) => {
    setPathError(null);
    setPathText(path);
    setChoosingRoot(false);
    setState({ status: 'loading' });
    setRequest({ path });
  };
  const go = () => {
    if (state.status === 'loading') return;
    const currentFolder = state.status === 'ready' && !choosingRoot ? state.data.currentPath : null;
    const path = resolvePathInput(pathText, currentFolder);
    if (path === null) {
      setPathError('Enter an absolute path, or choose a location before using a relative path.');
      return;
    }
    navigate(path);
  };

  const folders = state.status === 'ready'
    ? state.data.entries.filter((entry) => entry.entryType === 'directory')
    : [];
  const insideProject = state.status === 'ready'
    && state.data.currentPath.replace(/\/+$/, '').endsWith(SCENARIO_PROJECT_SUFFIX);
  const locationHelp = [
    'Locations include bundled Examples and the site project_folders, shown as /projects/<folder name>.',
    projectRoots.length > 0
      ? projectRoots.map(({ hostPath, containerPath }) => `${hostPath} → ${containerPath}`).join('\n')
      : 'See project_folders in the site settings file for the folders on your machine.',
    'After changing project_folders, Stop and Start the workbench.',
    'Pasting a host path is not supported. Use the matching /projects/... path or a path relative to the current folder.',
  ].join('\n');

  return (
    <div className="folder-picker" role="group" aria-label={chooseProject ? 'Choose a Scenario project' : 'Choose a folder'}>
      <div className="folder-picker-heading">
        <span>Locations</span>
        <button
          type="button"
          className="icon-button"
          aria-label="Project folder information"
          aria-expanded={helpOpen}
          aria-describedby={helpOpen ? helpId : undefined}
          title={locationHelp}
          onClick={() => setHelpOpen((open) => !open)}
        >i</button>
      </div>
      {helpOpen && <div id={helpId} role="tooltip" className="folder-picker-help">{locationHelp}</div>}
      <div className="folder-picker-roots" aria-label="Allowed roots">
        {(choosingRoot ? roots : shortcuts).map((root) => (
          <button key={root} type="button" title={root} onClick={() => navigate(root)}>{rootName(root)}</button>
        ))}
        {choosingRoot && lastFolder !== null && (
          <button type="button" title={lastFolder} onClick={() => navigate(lastFolder)}>Last folder</button>
        )}
      </div>
      <div className="folder-picker-bar">
        <button type="button" onClick={() => {
          setChoosingRoot(true);
          setPathError(null);
          if (roots.length === 0) setRequest({});
        }}>Locations</button>
        <button
          type="button"
          disabled={choosingRoot || state.status !== 'ready' || state.data.parentPath === null}
          onClick={() => {
            if (state.status === 'ready' && state.data.parentPath !== null) navigate(state.data.parentPath);
          }}
          title="Up one folder"
        >↑ Up</button>
        <input
          aria-label="Folder path"
          value={pathText}
          placeholder="Enter or paste a path"
          onChange={(event) => setPathText(event.target.value)}
          onKeyDown={(event) => { if (event.key === 'Enter') { event.preventDefault(); go(); } }}
        />
        <button type="button" disabled={state.status === 'loading'} onClick={go}>Go</button>
      </div>
      {pathError && <p className="inline-message severity-error" role="alert">{pathError}</p>}
      <div className="folder-picker-list" role="list">
        {state.status === 'loading' && <p className="muted">Reading folder…</p>}
        {state.status === 'error' && (!choosingRoot || roots.length === 0) && (
          <p className="inline-message severity-error" role="alert">
            {state.message}{' '}
            <button type="button" onClick={() => setHelpOpen(true)}>About project folders</button>
          </p>
        )}
        {choosingRoot && roots.length > 0 && <p className="muted">Choose a location.</p>}
        {state.status === 'ready' && !choosingRoot && folders.length === 0 && <p className="muted">No subfolders.</p>}
        {!choosingRoot && folders.map((entry) => (entry.scenarioProject ? (
          <button
            key={entry.path}
            type="button"
            role="listitem"
            className="dpick"
            disabled={!chooseProject}
            title={chooseProject ? 'Choose this Scenario project' : 'Scenario project'}
            onClick={chooseProject ? () => onChoose(entry.path) : undefined}
          >
            <span aria-hidden="true">◆</span> {entry.name}{chooseProject ? ' — Select Scenario' : ''}
          </button>
        ) : (
          <button
            key={entry.path}
            type="button"
            role="listitem"
            className="dpick"
            onClick={() => navigate(entry.path)}
          >
            <span aria-hidden="true">▸</span> {entry.name}
          </button>
        )))}
      </div>
      {!choosingRoot && insideProject && !chooseProject && (
        <p className="field-error" role="status">This is a Scenario project. Choose the folder that contains it.</p>
      )}
      <div className="dialog-actions folder-picker-actions">
        <button type="button" onClick={onCancel}>Cancel</button>
        {chooseProject && !choosingRoot && insideProject && state.status === 'ready' && !roots.includes(state.data.currentPath) && (
          <button type="button" className="primary-action" onClick={() => onChoose(state.data.currentPath)}>
            Select this Scenario
          </button>
        )}
        {!chooseProject && !choosingRoot && <button
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

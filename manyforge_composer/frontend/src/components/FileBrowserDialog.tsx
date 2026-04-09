import { useEffect, useMemo, useState } from 'react';
import { api } from '../api/client';
import type { FileBrowserEntry } from '../types/api';

type ArtifactKind = 'program' | 'deployment';
type BrowserMode = 'open' | 'save';

const DEFAULT_SUFFIX: Record<ArtifactKind, string> = {
  program: '.program.yaml',
  deployment: '.deployment.yaml',
};

const ALLOWED_SUFFIXES: Record<ArtifactKind, string[]> = {
  program: ['.program.yaml', '.program.yml'],
  deployment: ['.deployment.yaml', '.deployment.yml'],
};

interface FileBrowserDialogProps {
  open: boolean;
  title: string;
  kind: ArtifactKind;
  mode: BrowserMode;
  initialPath?: string;
  allowSkip?: boolean;
  skipLabel?: string;
  onConfirm: (path: string) => void;
  onCancel: () => void;
  onSkip?: () => void;
}

function dirname(path: string): string {
  const trimmed = path.trim();
  if (!trimmed || trimmed === '/') {
    return '/';
  }
  const normalized = trimmed.replace(/\/+$/, '');
  const index = normalized.lastIndexOf('/');
  if (index <= 0) {
    return '/';
  }
  return normalized.slice(0, index);
}

function basename(path: string): string {
  const trimmed = path.trim();
  if (!trimmed || trimmed === '/') {
    return '';
  }
  const normalized = trimmed.replace(/\/+$/, '');
  const index = normalized.lastIndexOf('/');
  return index < 0 ? normalized : normalized.slice(index + 1);
}

function joinPath(directory: string, fileName: string): string {
  const dir = directory === '' ? '/' : directory;
  const name = fileName.replace(/^\/+/, '');
  if (dir === '/') {
    return `/${name}`;
  }
  return `${dir.replace(/\/+$/, '')}/${name}`;
}

function defaultFileName(kind: ArtifactKind): string {
  return kind === 'program' ? 'new.program.yaml' : 'new.deployment.yaml';
}

function normalizeSaveFileName(kind: ArtifactKind, rawFileName: string): string {
  const trimmed = rawFileName.trim();
  if (!trimmed) {
    return defaultFileName(kind);
  }

  const lower = trimmed.toLowerCase();
  if (ALLOWED_SUFFIXES[kind].some((suffix) => lower.endsWith(suffix))) {
    return trimmed;
  }

  if (lower.endsWith('.yaml')) {
    return `${trimmed.slice(0, -5)}${DEFAULT_SUFFIX[kind]}`;
  }

  if (lower.endsWith('.yml')) {
    return `${trimmed.slice(0, -4)}${DEFAULT_SUFFIX[kind]}`;
  }

  return `${trimmed}${DEFAULT_SUFFIX[kind]}`;
}

export function FileBrowserDialog({
  open,
  title,
  kind,
  mode,
  initialPath,
  allowSkip = false,
  skipLabel = 'Skip',
  onConfirm,
  onCancel,
  onSkip,
}: FileBrowserDialogProps) {
  const [browserPath, setBrowserPath] = useState('');
  const [currentPath, setCurrentPath] = useState('');
  const [parentPath, setParentPath] = useState<string | null>(null);
  const [roots, setRoots] = useState<string[]>([]);
  const [entries, setEntries] = useState<FileBrowserEntry[]>([]);
  const [selectedPath, setSelectedPath] = useState('');
  const [fileName, setFileName] = useState('');
  const [pathInput, setPathInput] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    if (!open) {
      return;
    }
    const startingPath = initialPath?.trim() ?? '';
    const initialDirectory = startingPath ? dirname(startingPath) : '';
    setBrowserPath(initialDirectory);
    setPathInput(initialDirectory);
    setCurrentPath('');
    setParentPath(null);
    setEntries([]);
    setRoots([]);
    setError(null);
    setSelectedPath(mode === 'open' ? startingPath : '');
    setFileName(
      mode === 'save'
        ? basename(startingPath) || defaultFileName(kind)
        : '',
    );
  }, [initialPath, kind, mode, open]);

  useEffect(() => {
    if (!open) {
      return;
    }

    let cancelled = false;
    setLoading(true);
    setError(null);

    void api.files
      .browse(kind, browserPath || undefined)
      .then((response) => {
        if (cancelled) {
          return;
        }
        setCurrentPath(response.currentPath);
        setParentPath(response.parentPath);
        setRoots(response.roots);
        setEntries(response.entries);
        setPathInput(response.currentPath);
      })
      .catch((fetchError) => {
        if (cancelled) {
          return;
        }
        setError((fetchError as Error).message);
      })
      .finally(() => {
        if (!cancelled) {
          setLoading(false);
        }
      });

    return () => {
      cancelled = true;
    };
  }, [browserPath, kind, open]);

  const confirmDisabled = useMemo(() => {
    if (mode === 'open') {
      return selectedPath.trim() === '';
    }
    return fileName.trim() === '' || currentPath.trim() === '';
  }, [currentPath, fileName, mode, selectedPath]);
  const normalizedFileName = mode === 'save' ? normalizeSaveFileName(kind, fileName) : '';

  const handleEntryClick = (entry: FileBrowserEntry) => {
    if (entry.entryType === 'directory') {
      setBrowserPath(entry.path);
      setSelectedPath('');
      return;
    }

    if (mode === 'open') {
      setSelectedPath(entry.path);
    } else {
      setSelectedPath(entry.path);
      setFileName(entry.name);
    }
  };

  const handleConfirm = () => {
    if (mode === 'open') {
      if (!selectedPath.trim()) {
        return;
      }
      onConfirm(selectedPath.trim());
      return;
    }

    const nextPath = joinPath(currentPath || '/', normalizedFileName);
    onConfirm(nextPath);
  };

  if (!open) {
    return null;
  }

  return (
    <div className="fixed inset-0 z-50 flex items-center justify-center bg-black/60 p-6">
      <div className="flex h-[70vh] w-[min(920px,96vw)] flex-col overflow-hidden rounded-xl border border-gray-700 bg-gray-900 shadow-2xl">
        <div className="flex items-center justify-between border-b border-gray-700 px-5 py-4">
          <div>
            <div className="text-sm font-semibold text-white">{title}</div>
            <div className="text-xs text-gray-400">
              Browsing readable filesystem paths, filtered to compatible {kind} artifacts.
            </div>
          </div>
          <button
            type="button"
            onClick={onCancel}
            className="rounded bg-gray-700 px-3 py-1.5 text-sm font-medium text-gray-200 hover:bg-gray-600"
          >
            Close
          </button>
        </div>

        <div className="border-b border-gray-700 px-5 py-3">
          <div className="mb-2 flex flex-wrap gap-2">
            {roots.map((root) => (
              <button
                key={root}
                type="button"
                onClick={() => setBrowserPath(root)}
                className="rounded bg-gray-800 px-2 py-1 text-xs font-medium text-gray-300 hover:bg-gray-700"
              >
                {root}
              </button>
            ))}
          </div>
          <div className="flex items-center gap-2">
            <button
              type="button"
              onClick={() => parentPath && setBrowserPath(parentPath)}
              disabled={!parentPath}
              className="rounded bg-gray-700 px-3 py-2 text-sm font-medium text-gray-200 hover:bg-gray-600 disabled:bg-gray-800 disabled:text-gray-500"
            >
              Up
            </button>
            <input
              type="text"
              value={pathInput}
              onChange={(event) => setPathInput(event.target.value)}
              className="flex-1 rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-white"
              placeholder="/path/to/directory"
            />
            <button
              type="button"
              onClick={() => setBrowserPath(pathInput.trim() || '/')}
              className="rounded bg-blue-700 px-3 py-2 text-sm font-medium text-white hover:bg-blue-600"
            >
              Browse
            </button>
          </div>
          {mode === 'save' && (
            <div className="mt-3 flex items-center gap-2">
              <label className="text-sm text-gray-300">File name</label>
              <input
                type="text"
                value={fileName}
                onChange={(event) => setFileName(event.target.value)}
                className="flex-1 rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-white"
                placeholder={defaultFileName(kind)}
              />
              <div className="text-xs text-gray-500">
                Compatible suffix is added automatically.
              </div>
            </div>
          )}
        </div>

        <div className="flex-1 overflow-y-auto px-5 py-4">
          {loading && <div className="text-sm text-gray-400">Loading directory…</div>}
          {error && <div className="text-sm text-red-400">{error}</div>}
          {!loading && !error && entries.length === 0 && (
            <div className="text-sm text-gray-500">No compatible files in this directory.</div>
          )}
          {!loading && !error && entries.length > 0 && (
            <div className="space-y-1">
              {entries.map((entry) => {
                const isSelected =
                  mode === 'open'
                    ? selectedPath === entry.path
                    : selectedPath === entry.path || fileName === entry.name;
                return (
                  <button
                    key={entry.path}
                    type="button"
                    onClick={() => handleEntryClick(entry)}
                    onDoubleClick={() => {
                      handleEntryClick(entry);
                      if (entry.entryType === 'file' && mode === 'open') {
                        onConfirm(entry.path);
                      }
                    }}
                    className={`flex w-full items-center justify-between rounded px-3 py-2 text-left ${
                      isSelected
                        ? 'bg-blue-900/40 text-white'
                        : 'bg-gray-800 text-gray-200 hover:bg-gray-700'
                    }`}
                  >
                    <div className="flex items-center gap-3">
                      <span className="rounded bg-gray-700 px-2 py-0.5 text-[10px] font-semibold uppercase tracking-wide text-gray-300">
                        {entry.entryType === 'directory' ? 'Dir' : 'File'}
                      </span>
                      <div>
                        <div className="text-sm font-medium">{entry.name}</div>
                        <div className="text-xs text-gray-500">{entry.path}</div>
                      </div>
                    </div>
                    <div className="text-xs uppercase tracking-wide text-gray-500">
                      {entry.entryType}
                    </div>
                  </button>
                );
              })}
            </div>
          )}
        </div>

        <div className="flex items-center justify-between border-t border-gray-700 px-5 py-4">
          <div className="text-xs text-gray-500">
            {mode === 'open'
              ? selectedPath || 'Select a compatible file to continue.'
              : joinPath(currentPath || '/', normalizedFileName)}
          </div>
          <div className="flex items-center gap-2">
            {allowSkip && onSkip && (
              <button
                type="button"
                onClick={onSkip}
                className="rounded bg-gray-700 px-3 py-2 text-sm font-medium text-gray-200 hover:bg-gray-600"
              >
                {skipLabel}
              </button>
            )}
            <button
              type="button"
              onClick={onCancel}
              className="rounded bg-gray-700 px-3 py-2 text-sm font-medium text-gray-200 hover:bg-gray-600"
            >
              Cancel
            </button>
            <button
              type="button"
              onClick={handleConfirm}
              disabled={confirmDisabled}
              className="rounded bg-blue-700 px-3 py-2 text-sm font-medium text-white hover:bg-blue-600 disabled:bg-gray-700 disabled:text-gray-500"
            >
              {mode === 'open' ? 'Select' : 'Use Path'}
            </button>
          </div>
        </div>
      </div>
    </div>
  );
}

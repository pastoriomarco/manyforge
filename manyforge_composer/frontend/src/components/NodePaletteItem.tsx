import { useCallback, useRef, useState, type DragEvent } from 'react';
import type { CatalogEntry } from '../types/api';
import {
  programPaletteClickIntent,
  programPaletteDragData,
  type ProgramPaletteActivation,
} from '../utils/programCanvasInteraction';
import { OverlayPanel } from './OverlayPanel';

const KIND_ICONS: Record<string, string> = {
  leaf: '⬤',
  composite: '◆',
  decorator: '◇',
  macro: '▦',
  authoring_macro: '▦',
};

interface NodePaletteItemProps {
  entry: CatalogEntry;
  disabled?: boolean;
  onActivate?: (activation: ProgramPaletteActivation) => void;
  executionNote?: string;
  allowDrag?: boolean;
}

export function NodePaletteItem({
  entry,
  disabled = false,
  onActivate,
  executionNote,
  allowDrag = false,
}: NodePaletteItemProps) {
  const [infoOpen, setInfoOpen] = useState(false);
  const infoButton = useRef<HTMLButtonElement>(null);
  const closeInfo = useCallback(() => setInfoOpen(false), []);
  const onDragStart = (event: DragEvent<HTMLButtonElement>) => {
    for (const [format, value] of Object.entries(programPaletteDragData(entry))) {
      event.dataTransfer.setData(format, value);
    }
    event.dataTransfer.effectAllowed = 'copy';
    const ghost = document.createElement('div');
    ghost.textContent = entry.name;
    ghost.className = 'node-palette-drag-ghost';
    document.body.appendChild(ghost);
    event.dataTransfer.setDragImage(ghost, 60, 18);
    window.setTimeout(() => ghost.remove(), 0);
  };
  const titleId = `node-info-${entry.id.replaceAll(/[^A-Za-z0-9_-]/g, '-')}`;

  return (
    <div className="node-palette-row">
      <button
        type="button"
        draggable={!disabled && allowDrag}
        onDragStart={onDragStart}
        onClick={() => onActivate?.(programPaletteClickIntent(entry))}
        disabled={disabled}
        className={`node-palette-item${disabled ? ' disabled' : ''}${allowDrag ? ' draggable' : ''}`}
        title={disabled ? 'Authoring is currently unavailable' : `Add ${entry.name}`}
      >
        <span className="node-palette-icon" aria-hidden="true">
          {KIND_ICONS[entry.kind] || '●'}
        </span>
        <span className="node-palette-copy">
          <strong>{entry.name}</strong>
          <small>{entry.id} · {entry.kind}</small>
          {executionNote && (
            <small className="node-palette-availability">{executionNote}</small>
          )}
        </span>
      </button>
      <button
        ref={infoButton}
        type="button"
        className="icon-button node-info-button"
        aria-label={`Information about ${entry.name}`}
        title={`Information about ${entry.name}`}
        onClick={() => setInfoOpen(true)}
      >
        i
      </button>
      <OverlayPanel
        open={infoOpen}
        title={entry.name}
        titleId={titleId}
        kind="info"
        onClose={closeInfo}
        returnFocusRef={infoButton}
      >
        <div className="node-information">
          <p>{entry.description}</p>
          <dl>
            <div><dt>Stable ID</dt><dd><code>{entry.id}</code></dd></div>
            <div><dt>Category</dt><dd>{entry.category}</dd></div>
            <div><dt>Kind</dt><dd>{entry.kind}</dd></div>
            <div><dt>Children</dt><dd>{entry.childrenMin}–{entry.childrenMax < 0 ? 'unbounded' : entry.childrenMax}</dd></div>
            <div><dt>Owner API</dt><dd><code>{entry.execution.ownerApi}</code></dd></div>
            <div><dt>Execution</dt><dd>{entry.execution.blocking ? 'blocking' : 'non-blocking'} · {entry.execution.cancellable ? 'cancellable' : 'not cancellable'}</dd></div>
            <div><dt>Minimum profile</dt><dd>{entry.minProgramProfile ?? 'none'}</dd></div>
            <div><dt>Tags</dt><dd>{entry.tags.join(', ') || 'none'}</dd></div>
          </dl>
          <h3>Parameters ({entry.parameters.length})</h3>
          {entry.parameters.length === 0 ? <p className="muted">No authored parameters.</p> : (
            <div className="node-schema-list">
              {entry.parameters.map((parameter) => (
                <section key={parameter.id}>
                  <strong>{parameter.id}</strong>
                  <span>{parameter.required ? 'required' : 'optional'} · <code>{parameter.schemaHash}</code></span>
                  <pre>{JSON.stringify(parameter.schema, null, 2)}</pre>
                </section>
              ))}
            </div>
          )}
        </div>
      </OverlayPanel>
    </div>
  );
}

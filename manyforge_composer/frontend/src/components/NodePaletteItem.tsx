import type { DragEvent } from 'react';
import type { CatalogEntry } from '../types/api';

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
  onActivate?: (entry: CatalogEntry) => void;
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
  const onDragStart = (event: DragEvent<HTMLButtonElement>) => {
    event.dataTransfer.setData('application/manyforge-node', entry.id);
    event.dataTransfer.effectAllowed = 'copy';
    const ghost = document.createElement('div');
    ghost.textContent = entry.name;
    ghost.className = 'node-palette-drag-ghost';
    document.body.appendChild(ghost);
    event.dataTransfer.setDragImage(ghost, 60, 18);
    window.setTimeout(() => ghost.remove(), 0);
  };

  return (
    <button
      type="button"
      draggable={!disabled && allowDrag}
      onDragStart={onDragStart}
      onClick={() => onActivate?.(entry)}
      disabled={disabled}
      className={`node-palette-item${disabled ? ' disabled' : ''}${allowDrag ? ' draggable' : ''}`}
      title={disabled ? 'Authoring is currently unavailable' : entry.description}
    >
      <span className="node-palette-icon" aria-hidden="true">
        {KIND_ICONS[entry.kind] || '●'}
      </span>
      <span className="node-palette-copy">
        <strong>{entry.name}</strong>
        <small>{entry.description}</small>
        {executionNote && (
          <small className="node-palette-availability">{executionNote}</small>
        )}
      </span>
    </button>
  );
}

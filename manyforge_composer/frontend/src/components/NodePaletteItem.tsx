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
}

export function NodePaletteItem({
  entry,
  disabled = false,
}: NodePaletteItemProps) {
  const onDragStart = (event: DragEvent<HTMLDivElement>) => {
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
    <div
      draggable={!disabled}
      onDragStart={onDragStart}
      className={`node-palette-item${disabled ? ' disabled' : ''}`}
      title={disabled ? 'Authoring is currently unavailable' : entry.description}
    >
      <span className="node-palette-icon" aria-hidden="true">
        {KIND_ICONS[entry.kind] || '●'}
      </span>
      <span className="node-palette-copy">
        <strong>{entry.name}</strong>
        <small>{entry.description}</small>
      </span>
    </div>
  );
}

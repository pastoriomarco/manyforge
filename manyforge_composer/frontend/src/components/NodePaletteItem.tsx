import type { CatalogEntry } from '../types/api';

const KIND_ICONS: Record<string, string> = {
  leaf: '⬤',       // action/condition node
  composite: '◆',   // sequence/selector/parallel
  decorator: '◇',   // inverter/repeat/retry
  macro: '▦',       // runtime/code-generator expansion
  authoring_macro: '▦', // Composer-only subtree template
};

interface Props {
  entry: CatalogEntry;
  disabled?: boolean;
}

export function NodePaletteItem({ entry, disabled = false }: Props) {
  const onDragStart = (e: React.DragEvent) => {
    // Store the catalog entry ID for React Flow drop handler
    e.dataTransfer.setData('application/manyforge-node', entry.id);
    e.dataTransfer.effectAllowed = 'copy';
    // The default drag image snapshots the whole palette row (name +
    // description), which reads as a huge white rectangle and badly
    // overstates where the node will land. Hand the browser a ghost the
    // size and shape of an actual canvas node instead.
    const ghost = document.createElement('div');
    ghost.textContent = entry.name;
    Object.assign(ghost.style, {
      position: 'fixed',
      top: '-1000px',
      left: '-1000px',
      width: '120px',
      padding: '8px 4px',
      borderRadius: '4px',
      border: '2px solid #9ca3af',
      background: 'rgba(17, 24, 39, 0.9)',
      color: '#e5e7eb',
      font: '500 12px sans-serif',
      textAlign: 'center',
      whiteSpace: 'nowrap',
      overflow: 'hidden',
      textOverflow: 'ellipsis',
      pointerEvents: 'none',
    } as CSSStyleDeclaration);
    document.body.appendChild(ghost);
    e.dataTransfer.setDragImage(ghost, 60, 18);
    window.setTimeout(() => ghost.remove(), 0);
  };

  return (
    <div
      draggable={!disabled}
      onDragStart={onDragStart}
      className={`flex items-start gap-2 px-2 py-1.5 rounded cursor-grab hover:bg-gray-700 active:cursor-grabbing ${disabled ? 'opacity-50 cursor-not-allowed' : ''}`}
      title={disabled ? 'Editing disabled while running or stopping' : entry.description}
    >
      <span className="text-xs mt-0.5 opacity-60">{KIND_ICONS[entry.kind] || '●'}</span>
      <div className="min-w-0">
        <div className="text-sm text-gray-200 truncate">{entry.name}</div>
        <div className="text-xs text-gray-500 truncate">{entry.description}</div>
      </div>
    </div>
  );
}

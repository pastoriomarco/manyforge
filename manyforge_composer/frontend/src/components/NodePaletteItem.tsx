import type { CatalogEntry } from '../types/api';

const KIND_ICONS: Record<string, string> = {
  leaf: '⬤',       // action/condition node
  composite: '◆',   // sequence/selector/parallel
  decorator: '◇',   // inverter/repeat/retry
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
  };

  return (
    <div
      draggable={!disabled}
      onDragStart={onDragStart}
      className={`flex items-start gap-2 px-2 py-1.5 rounded cursor-grab hover:bg-gray-700 active:cursor-grabbing ${disabled ? 'opacity-50 cursor-not-allowed' : ''}`}
      title={disabled ? 'Editing disabled while running' : entry.description}
    >
      <span className="text-xs mt-0.5 opacity-60">{KIND_ICONS[entry.kind] || '●'}</span>
      <div className="min-w-0">
        <div className="text-sm text-gray-200 truncate">{entry.name}</div>
        <div className="text-xs text-gray-500 truncate">{entry.description}</div>
      </div>
    </div>
  );
}

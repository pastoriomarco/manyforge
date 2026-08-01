interface CollapsedRailProps {
  label: string;
  side: 'left' | 'right';
  onExpand: () => void;
}

export function CollapsedRail({ label, side, onExpand }: CollapsedRailProps) {
  return (
    <button
      type="button"
      onClick={onExpand}
      className="collapsed-rail"
      title={`Expand ${label}`}
    >
      <span className="collapsed-rail-arrow" aria-hidden="true">
        {side === 'left' ? '›' : '‹'}
      </span>
      <span className="collapsed-rail-label">{label}</span>
    </button>
  );
}

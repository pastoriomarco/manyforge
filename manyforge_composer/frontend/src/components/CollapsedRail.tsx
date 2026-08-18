import type { Ref } from 'react';

interface CollapsedRailProps {
  label: string;
  side: 'left' | 'right';
  onExpand: () => void;
  buttonRef?: Ref<HTMLButtonElement>;
}

export function CollapsedRail({
  label,
  side,
  onExpand,
  buttonRef,
}: CollapsedRailProps) {
  return (
    <button
      type="button"
      ref={buttonRef}
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

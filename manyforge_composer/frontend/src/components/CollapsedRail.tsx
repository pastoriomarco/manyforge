interface CollapsedRailProps {
  label: string;
  side: 'left' | 'right';
  onExpand: () => void;
}

export function CollapsedRail({ label, side, onExpand }: CollapsedRailProps) {
  const arrow = side === 'left' ? '›' : '‹';
  return (
    <button
      type="button"
      onClick={onExpand}
      className="group flex h-full w-10 shrink-0 flex-col items-center justify-center border-l border-r border-gray-700 bg-gray-800 text-gray-400 transition-colors hover:bg-gray-700 hover:text-white"
      title={`Expand ${label}`}
    >
      <span className="mb-3 text-xl leading-none text-blue-400 transition-transform group-hover:scale-110">
        {arrow}
      </span>
      <span className="origin-center -rotate-90 whitespace-nowrap text-[11px] font-semibold uppercase tracking-[0.24em]">
        {label}
      </span>
    </button>
  );
}

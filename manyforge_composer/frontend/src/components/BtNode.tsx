import { useLayoutEffect } from 'react';
import { Handle, Position, useUpdateNodeInternals } from '@xyflow/react';
import type { NodeProps } from '@xyflow/react';
import { useComposerStore } from '../store/composerStore';
import { useProgramStore } from '../store/programStore';
import { useCatalogStore } from '../store/catalogStore';

const CONTROL_FLOW_COLORS: Record<string, string> = {
  sequence: 'bg-blue-900/45',
  fallback: 'bg-orange-900/45',
  parallel: 'bg-purple-900/45',
  inverter: 'bg-yellow-900/45',
  repeat: 'bg-cyan-900/45',
  retry: 'bg-cyan-900/45',
  timer: 'bg-teal-900/45',
  default: 'bg-slate-800/80',
};

const CATEGORY_COLORS: Record<string, string> = {
  motion: 'bg-orange-700/55',
  logic: 'bg-yellow-700/55',
  geometry: 'bg-pink-800/55',
  scene: 'bg-purple-900/45',
  default: 'bg-slate-800/80',
};

const STATUS_BORDER_COLORS: Record<string, string> = {
  idle: 'border-gray-500',
  running: 'border-amber-400 shadow-[0_0_10px_rgba(251,191,36,0.35)]',
  success: 'border-green-400 shadow-[0_0_10px_rgba(74,222,128,0.25)]',
  failure: 'border-red-400 shadow-[0_0_10px_rgba(248,113,113,0.25)]',
};

export function BtNode({ data, selected, id }: NodeProps) {
  const { nodeStatuses } = useComposerStore();
  const layoutDirection = useProgramStore((state) => state.layoutDirection);
  const nodeCategory = useCatalogStore(
    (state) => state.entries.find((entry) => entry.id === (data.catalogId as string))?.category ?? 'default',
  );
  const updateNodeInternals = useUpdateNodeInternals();
  
  const colorClass =
    nodeCategory === 'control_flow'
      ? CONTROL_FLOW_COLORS[data.catalogId as string] || CONTROL_FLOW_COLORS.default
      : CATEGORY_COLORS[nodeCategory] || CATEGORY_COLORS.default;
  const selectedClass = selected ? 'ring-2 ring-white ring-offset-2 ring-offset-gray-900' : '';
  const targetPosition = layoutDirection === 'vertical' ? Position.Top : Position.Left;
  const sourcePosition = layoutDirection === 'vertical' ? Position.Bottom : Position.Right;
  
  // Get node status
  const nodeStatus = nodeStatuses[id] ?? 'idle';
  const statusClass = STATUS_BORDER_COLORS[nodeStatus] || STATUS_BORDER_COLORS.idle;

  useLayoutEffect(() => {
    const raf = window.requestAnimationFrame(() => {
      updateNodeInternals(id);
    });
    return () => window.cancelAnimationFrame(raf);
  }, [id, layoutDirection, updateNodeInternals]);

  return (
    <div className={`relative px-3 py-2 rounded border-2 min-w-[120px] text-center ${colorClass} ${selectedClass} ${statusClass}`}>
      <Handle
        key={`target-${layoutDirection}`}
        type="target"
        position={targetPosition}
        className="!bg-gray-400 !w-3 !h-3 hover:!bg-green-400 hover:!w-4 hover:!h-4 transition-all"
      />
      {['sequence', 'fallback', 'parallel'].includes(data.catalogId as string) && (
        <div className="text-xs text-gray-500 mb-0.5">&#9670;</div>
      )}
      {['inverter', 'repeat', 'retry'].includes(data.catalogId as string) && (
        <div className="text-xs text-gray-500 mb-0.5">&#9671;</div>
      )}
      <div className="text-xs text-gray-300 uppercase">{data.catalogId as string}</div>
      <div className="text-sm text-white font-medium">{data.label as string}</div>
      <Handle
        key={`source-${layoutDirection}`}
        type="source"
        position={sourcePosition}
        className="!bg-gray-400 !w-3 !h-3 hover:!bg-blue-400 hover:!w-4 hover:!h-4 transition-all"
      />
    </div>
  );
}

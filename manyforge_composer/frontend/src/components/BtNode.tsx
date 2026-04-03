import { Handle, Position } from '@xyflow/react';
import type { NodeProps } from '@xyflow/react';
import { useComposerStore } from '../store/composerStore';

const KIND_COLORS: Record<string, string> = {
  sequence: 'border-blue-500 bg-blue-900/50',
  fallback: 'border-orange-500 bg-orange-900/50',
  parallel: 'border-purple-500 bg-purple-900/50',
  inverter: 'border-yellow-500 bg-yellow-900/50',
  repeat: 'border-cyan-500 bg-cyan-900/50',
  retry: 'border-cyan-500 bg-cyan-900/50',
  // Default for leaf nodes (actions/conditions)
  default: 'border-green-500 bg-green-900/50',
};

const STATUS_COLORS: Record<string, string> = {
  running: 'border-amber-400 shadow-[0_0_10px_rgba(251,191,36,0.5)]',
  success: 'border-green-400 shadow-[0_0_10px_rgba(74,222,128,0.5)]',
  failure: 'border-red-400 shadow-[0_0_10px_rgba(248,113,113,0.5)]',
};

const STATUS_ICONS: Record<string, string> = {
  running: '⟳',
  success: '✓',
  failure: '✕',
};

const STATUS_DOT_COLORS: Record<string, string> = {
  running: 'bg-amber-400',
  success: 'bg-green-400',
  failure: 'bg-red-400',
};

export function BtNode({ data, selected, id }: NodeProps) {
  const { nodeStatuses } = useComposerStore();
  
  const colorClass = KIND_COLORS[data.catalogId as string] || KIND_COLORS.default;
  const selectedClass = selected ? 'ring-2 ring-white ring-offset-2 ring-offset-gray-900' : '';
  
  // Get node status
  const nodeStatus = nodeStatuses[id];
  const statusClass = nodeStatus ? STATUS_COLORS[nodeStatus] : '';
  const statusIcon = nodeStatus ? STATUS_ICONS[nodeStatus] : '';
  const statusDotClass = nodeStatus ? STATUS_DOT_COLORS[nodeStatus] : '';

  return (
    <div className={`relative px-3 py-2 rounded border-2 min-w-[120px] text-center ${colorClass} ${selectedClass} ${statusClass}`}>
      {/* Status indicator in top-right corner */}
      {nodeStatus && (
        <div className={`absolute -top-1 -right-1 w-3 h-3 rounded-full ${statusDotClass} border-2 border-gray-900 flex items-center justify-center text-[8px] text-gray-900 font-bold`}>
          {statusIcon}
        </div>
      )}
      
      <Handle type="target" position={Position.Top} className="!bg-gray-400" />
      <div className="text-xs text-gray-400 uppercase">{data.catalogId as string}</div>
      <div className="text-sm text-white font-medium">{data.label as string}</div>
      <Handle type="source" position={Position.Bottom} className="!bg-gray-400" />
    </div>
  );
}

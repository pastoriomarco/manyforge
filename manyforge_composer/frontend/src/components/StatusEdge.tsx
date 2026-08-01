import type { CSSProperties } from 'react';
import type { EdgeStyle, NodePresentationStatus } from '../store/composerStore';

const EDGE_STATUS_COLORS: Record<NodePresentationStatus, string> = {
  idle: '#6b7280',
  running: '#fbbf24',
  success: '#4ade80',
  failure: '#f87171',
};

export interface StatusEdgeGeometry {
  sourceX: number;
  sourceY: number;
  targetX: number;
  targetY: number;
}

export function statusEdgePath(
  geometry: StatusEdgeGeometry,
  style: EdgeStyle,
): string {
  const { sourceX, sourceY, targetX, targetY } = geometry;
  if (style === 'straight') return `M ${sourceX} ${sourceY} L ${targetX} ${targetY}`;
  const middleX = (sourceX + targetX) / 2;
  const middleY = (sourceY + targetY) / 2;
  if (style === 'step') {
    return `M ${sourceX} ${sourceY} L ${middleX} ${sourceY} L ${middleX} ${targetY} L ${targetX} ${targetY}`;
  }
  if (style === 'smoothstep') {
    return `M ${sourceX} ${sourceY} C ${middleX} ${sourceY}, ${middleX} ${targetY}, ${targetX} ${targetY}`;
  }
  if (style === 'simplebezier') {
    return `M ${sourceX} ${sourceY} Q ${middleX} ${middleY}, ${targetX} ${targetY}`;
  }
  const offset = Math.max(40, Math.abs(targetY - sourceY) * 0.45);
  return `M ${sourceX} ${sourceY} C ${sourceX} ${sourceY + offset}, ${targetX} ${targetY - offset}, ${targetX} ${targetY}`;
}

interface StatusEdgeProps extends StatusEdgeGeometry {
  edgeStyle: EdgeStyle;
  targetStatus?: NodePresentationStatus;
  markerEnd?: string;
  style?: CSSProperties;
}

/** SVG edge presentation; the I6 graph adapter owns interaction and identity. */
export function StatusEdge({
  edgeStyle,
  targetStatus = 'idle',
  markerEnd,
  style,
  ...geometry
}: StatusEdgeProps) {
  return (
    <path
      d={statusEdgePath(geometry, edgeStyle)}
      markerEnd={markerEnd}
      fill="none"
      style={{
        stroke: EDGE_STATUS_COLORS[targetStatus],
        strokeWidth: 2,
        ...style,
      }}
    />
  );
}

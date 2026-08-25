import type { CSSProperties } from 'react';
import type {
  EdgeStyle,
  LayoutDirection,
  NodePresentationStatus,
} from '../store/composerStore';

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
  layoutDirection: LayoutDirection = 'vertical',
): string {
  const { sourceX, sourceY, targetX, targetY } = geometry;
  if (style === 'straight') return `M ${sourceX} ${sourceY} L ${targetX} ${targetY}`;
  const middleX = (sourceX + targetX) / 2;
  const middleY = (sourceY + targetY) / 2;
  if (style === 'step') {
    return layoutDirection === 'vertical'
      ? `M ${sourceX} ${sourceY} L ${middleX} ${sourceY} L ${middleX} ${targetY} L ${targetX} ${targetY}`
      : `M ${sourceX} ${sourceY} L ${sourceX} ${middleY} L ${targetX} ${middleY} L ${targetX} ${targetY}`;
  }
  if (style === 'smoothstep') {
    return layoutDirection === 'vertical'
      ? `M ${sourceX} ${sourceY} C ${middleX} ${sourceY}, ${middleX} ${targetY}, ${targetX} ${targetY}`
      : `M ${sourceX} ${sourceY} C ${sourceX} ${middleY}, ${targetX} ${middleY}, ${targetX} ${targetY}`;
  }
  if (style === 'simplebezier') {
    return layoutDirection === 'vertical'
      ? `M ${sourceX} ${sourceY} C ${middleX} ${sourceY}, ${middleX} ${targetY}, ${targetX} ${targetY}`
      : `M ${sourceX} ${sourceY} C ${sourceX} ${middleY}, ${targetX} ${middleY}, ${targetX} ${targetY}`;
  }
  if (layoutDirection === 'vertical') {
    const offset = Math.max(40, Math.abs(targetX - sourceX) * 0.45);
    return `M ${sourceX} ${sourceY} C ${sourceX + offset} ${sourceY}, ${targetX - offset} ${targetY}, ${targetX} ${targetY}`;
  }
  const offset = Math.max(40, Math.abs(targetY - sourceY) * 0.45);
  return `M ${sourceX} ${sourceY} C ${sourceX} ${sourceY + offset}, ${targetX} ${targetY - offset}, ${targetX} ${targetY}`;
}

interface StatusEdgeProps extends StatusEdgeGeometry {
  edgeStyle: EdgeStyle;
  layoutDirection?: LayoutDirection;
  targetStatus?: NodePresentationStatus;
  markerEnd?: string;
  style?: CSSProperties;
}

/** SVG edge presentation; the I6 graph adapter owns interaction and identity. */
export function StatusEdge({
  edgeStyle,
  layoutDirection = 'vertical',
  targetStatus = 'idle',
  markerEnd,
  style,
  ...geometry
}: StatusEdgeProps) {
  return (
    <path
      d={statusEdgePath(geometry, edgeStyle, layoutDirection)}
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

import type { CSSProperties } from 'react';
import {
  Position,
  getBezierPath,
  getSimpleBezierPath,
  getSmoothStepPath,
  getStraightPath,
} from '@xyflow/react';
import type {
  EdgeStyle,
  LayoutDirection,
  NodePresentationStatus,
} from '../store/composerStore';
import { STATUS_COLORS } from '../styles/tokens';

// Edges share the node border status colours (UI6-I-VC).
const EDGE_STATUS_COLORS: Record<NodePresentationStatus, string> = STATUS_COLORS;

export interface StatusEdgeGeometry {
  sourceX: number;
  sourceY: number;
  targetX: number;
  targetY: number;
  sourcePosition?: Position;
  targetPosition?: Position;
}

// Same path helpers and corner radii as the pre-relocation StatusEdge. The
// canvas passes React Flow's handle positions; without them, handles sit
// left/right in the vertical layout and top/bottom in the horizontal one.
export function statusEdgePath(
  geometry: StatusEdgeGeometry,
  style: EdgeStyle,
  layoutDirection: LayoutDirection = 'vertical',
): string {
  const params = {
    ...geometry,
    sourcePosition: geometry.sourcePosition
      ?? (layoutDirection === 'vertical' ? Position.Right : Position.Bottom),
    targetPosition: geometry.targetPosition
      ?? (layoutDirection === 'vertical' ? Position.Left : Position.Top),
  };
  if (style === 'straight') return getStraightPath(params)[0];
  if (style === 'simplebezier') return getSimpleBezierPath(params)[0];
  if (style === 'smoothstep') return getSmoothStepPath({ ...params, borderRadius: 18 })[0];
  if (style === 'step') return getSmoothStepPath({ ...params, borderRadius: 0 })[0];
  return getBezierPath(params)[0];
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

import {
  BaseEdge,
  getBezierPath,
  getSimpleBezierPath,
  getSmoothStepPath,
  getStraightPath,
} from '@xyflow/react';
import type { EdgeProps } from '@xyflow/react';
import { useComposerStore } from '../store/composerStore';

const EDGE_STATUS_COLORS: Record<string, string> = {
  idle: '#6b7280',
  running: '#fbbf24',
  success: '#4ade80',
  failure: '#f87171',
};

export function StatusEdge(props: EdgeProps) {
  const edgeStyle = useComposerStore((state) => state.edgeStyle);
  const nodeStatuses = useComposerStore((state) => state.nodeStatuses);

  const edgeStatus = nodeStatuses[props.target] ?? 'idle';
  const stroke = EDGE_STATUS_COLORS[edgeStatus] || EDGE_STATUS_COLORS.idle;

  let path = '';
  if (edgeStyle === 'straight') {
    [path] = getStraightPath(props);
  } else if (edgeStyle === 'simplebezier') {
    [path] = getSimpleBezierPath(props);
  } else if (edgeStyle === 'smoothstep') {
    [path] = getSmoothStepPath({ ...props, borderRadius: 18 });
  } else if (edgeStyle === 'step') {
    [path] = getSmoothStepPath({ ...props, borderRadius: 0 });
  } else {
    [path] = getBezierPath(props);
  }

  return (
    <BaseEdge
      path={path}
      markerEnd={props.markerEnd}
      interactionWidth={28}
      style={{
        ...props.style,
        cursor: 'pointer',
        stroke,
        strokeWidth: 2,
      }}
    />
  );
}

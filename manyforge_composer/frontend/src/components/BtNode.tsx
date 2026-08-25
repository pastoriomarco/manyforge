import { useLayoutEffect, type KeyboardEvent, type Ref } from 'react';
import { Handle, Position, useUpdateNodeInternals } from '@xyflow/react';
import type {
  LayoutDirection,
  NodePresentationStatus,
} from '../store/composerStore';
import {
  isProgramCanvasFormTarget,
  programCanvasKeyIntent,
} from '../utils/programCanvasInteraction';

export interface BtNodePresentation {
  id: string;
  label: string;
  catalogId: string;
  category: string;
  kind: string;
  childrenMax: number;
  childCount: number;
  hasParent: boolean;
  isRoot?: boolean;
  status: NodePresentationStatus;
  selected: boolean;
  layoutDirection: LayoutDirection;
  executionAvailable?: boolean;
}

export function btNodeConnectionAvailability(node: Pick<
  BtNodePresentation,
  'childrenMax' | 'childCount' | 'hasParent'
>): { sourceConnectable: boolean; targetConnectable: boolean } {
  const canHaveChildren = node.childrenMax !== 0;
  return {
    sourceConnectable: canHaveChildren
      && (node.childrenMax < 0 || node.childCount < node.childrenMax),
    targetConnectable: !node.hasParent,
  };
}

function childHint(kind: string, childrenMax: number): string {
  if (kind === 'decorator') return '1 child';
  if (childrenMax === 0) return 'leaf';
  if (childrenMax < 0) return 'n children';
  return `${childrenMax} max`;
}

/** Catalog-node presentation with graph handles and no topology authority. */
interface BtNodeProps {
  node: BtNodePresentation;
  onSelect?: (instanceId: string) => void;
  onFocus?: (instanceId: string) => void;
  onDelete?: (event: KeyboardEvent<HTMLDivElement>, instanceId: string) => void;
  onNavigate?: (instanceId: string, key: string) => void;
  elementRef?: Ref<HTMLDivElement>;
}

export function BtNode({
  node,
  onSelect,
  onFocus,
  onDelete,
  onNavigate,
  elementRef,
}: BtNodeProps) {
  const connections = btNodeConnectionAvailability(node);
  const updateNodeInternals = useUpdateNodeInternals();
  const targetConnectable = node.isRoot === true ? false : true;
  const glyph = node.kind === 'composite'
    ? '◆'
    : node.kind === 'decorator'
      ? '◇'
      : '•';
  useLayoutEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      updateNodeInternals(node.id);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [node.childrenMax, node.id, node.layoutDirection, updateNodeInternals]);
  return (
    <div
      ref={elementRef}
      className={`bt-node category-${node.category} status-${node.status}${node.selected ? ' selected' : ''}`}
      data-node-id={node.id}
      data-layout-direction={node.layoutDirection}
      role="button"
      tabIndex={0}
      aria-pressed={node.selected}
      aria-label={`${node.label} Program node`}
      title={node.executionAvailable
        ? 'Execution implementation available'
        : 'Authorable here; execution remains unavailable at its restoration gate'}
      onClick={() => onSelect?.(node.id)}
      onFocus={() => onFocus?.(node.id)}
      onKeyDown={(event) => {
        const intent = programCanvasKeyIntent(
          event.key,
          isProgramCanvasFormTarget(event.target) ? 'form' : 'node',
        );
        if (!intent) return;
        event.preventDefault();
        if (intent.type === 'delete') {
          onDelete?.(event, node.id);
        } else if (intent.type === 'select') {
          onSelect?.(node.id);
        } else {
          onNavigate?.(node.id, intent.key);
        }
      }}
    >
      <Handle
        type="target"
        position={node.layoutDirection === 'vertical' ? Position.Left : Position.Top}
        isConnectable={targetConnectable}
        className={`bt-node-react-flow-handle${targetConnectable ? '' : ' disabled'}`}
        aria-label={`Connect ${node.label} as child`}
      />
      <span className="bt-node-kind" aria-hidden="true">{glyph}</span>
      <small>{node.catalogId}</small>
      <strong>{node.label}</strong>
      <span className="bt-node-child-hint">
        {childHint(node.kind, node.childrenMax)}
      </span>
      {node.childrenMax !== 0 && (
        <Handle
          type="source"
          position={node.layoutDirection === 'vertical' ? Position.Right : Position.Bottom}
          isConnectable={connections.sourceConnectable}
          className={`bt-node-react-flow-handle${connections.sourceConnectable ? '' : ' disabled'}`}
          aria-label={`Connect ${node.label} as parent`}
        />
      )}
      <span
        className={`node-availability-dot ${node.executionAvailable ? 'execution-ready' : 'authoring-only'}`}
        aria-label={node.executionAvailable
          ? 'Execution available'
          : 'Authoring only'}
      />
    </div>
  );
}

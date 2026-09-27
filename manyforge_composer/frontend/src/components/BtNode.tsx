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
import { programCategoryFamily } from '../styles/tokens';
import { ProgramCategoryIcon, ProgramKindSymbol, ProgramNodeName } from './programNodeGlyphs';

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
  executionDisposition?: 'executable' | 'configuration_required' | 'unavailable';
  executionReason?: string;
  invalid?: boolean;
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
  const targetConnectable = connections.targetConnectable;
  const disposition = node.executionDisposition
    ?? (node.executionAvailable ? 'executable' : 'unavailable');
  const availabilityLabel = disposition === 'executable'
    ? 'Execution available'
    : disposition === 'configuration_required'
      ? 'Configuration required'
      : 'Execution unavailable';
  const availabilityDetail = node.invalid
    ? 'This node needs valid required configuration before execution.'
    : node.executionDisposition === 'executable'
      ? 'Executable for the exact current Applied Program and Binding.'
      : node.executionDisposition === 'configuration_required'
        ? 'Execution implementation is available; complete the required node configuration.'
        : `Authorable here; execution unavailable${node.executionReason ? `: ${node.executionReason}` : '.'}`;
  useLayoutEffect(() => {
    const frame = window.requestAnimationFrame(() => {
      updateNodeInternals(node.id);
    });
    return () => window.cancelAnimationFrame(frame);
  }, [
    node.catalogId,
    node.childrenMax,
    node.id,
    node.label,
    node.layoutDirection,
    updateNodeInternals,
  ]);
  return (
    <div
      ref={elementRef}
      className={`bt-node status-${node.status}${node.invalid ? ' invalid' : ''}${node.selected ? ' selected' : ''}`}
      data-category={node.category}
      data-category-family={programCategoryFamily(node.category)}
      data-node-id={node.id}
      data-layout-direction={node.layoutDirection}
      role="button"
      tabIndex={0}
      aria-pressed={node.selected}
      aria-label={`${node.label} Program node`}
      title={`${node.label}\n${availabilityDetail}`}
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
      {/* UI6-I-VC D3: icon band, name, empty band of the same width. */}
      <span className="bt-node-band" aria-hidden="true">
        <ProgramCategoryIcon category={node.category} catalogId={node.catalogId} />
      </span>
      <strong className="bt-node-name"><ProgramNodeName name={node.label} /></strong>
      <span className="bt-node-band" aria-hidden="true" />
      {node.childrenMax !== 0 && (
        <Handle
          type="source"
          position={node.layoutDirection === 'vertical' ? Position.Right : Position.Bottom}
          isConnectable={connections.sourceConnectable}
          className={`bt-node-react-flow-handle${connections.sourceConnectable ? '' : ' disabled'}`}
          aria-label={`Connect ${node.label} as parent`}
        />
      )}
      {/* The kind symbol appears only when execution is not available. */}
      {disposition !== 'executable' && (
        <span className={`bt-node-status ${disposition}`} aria-hidden="true">
          <ProgramKindSymbol kind={node.kind} />
        </span>
      )}
      <span className="sr-only">{availabilityLabel}</span>
    </div>
  );
}

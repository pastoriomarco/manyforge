import { useLayoutEffect, type KeyboardEvent, type MouseEvent, type Ref } from 'react';
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
import type { CollapsedProgramItem } from '../utils/programCollapse';
import {
  ProgramCategoryIcon,
  ProgramHeldBadge as HeldBadge,
  ProgramKindSymbol,
  ProgramNodeName,
} from './programNodeGlyphs';

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
  /** The node has children, so it can collapse. */
  collapsible?: boolean;
  /** When collapsed: its whole subtree, listed inside the node. */
  collapsedItems?: CollapsedProgramItem[] | null;
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
  onToggleCollapse?: (instanceId: string) => void;
  /** Right-click on a listed node of a collapsed subtree. */
  onItemContextMenu?: (event: MouseEvent, instanceId: string) => void;
}

const FOLD_ICON = {
  collapse: 'M4 6l4 4 4-4',
  expand: 'M6 4l4 4-4 4',
};

/** A collapsed node's list: leaves are rows, composites enclose their children. */
function CollapsedItems({
  items,
  onSelect,
  onItemContextMenu,
}: {
  items: CollapsedProgramItem[];
  onSelect?: (instanceId: string) => void;
  onItemContextMenu?: (event: MouseEvent, instanceId: string) => void;
}) {
  return items.map((item) => {
    const row = (
      <div
        className="bt-node-collapsed-row"
        title={`${item.label} · ${item.status === 'held' ? 'waiting for Run' : item.status}`}
        onClick={(event) => {
          event.stopPropagation();
          onSelect?.(item.id);
        }}
        onContextMenu={(event) => {
          event.stopPropagation();
          onItemContextMenu?.(event, item.id);
        }}
      >
        <ProgramCategoryIcon category={item.category} catalogId={item.catalogId} />
        <span className="bt-node-collapsed-name">{item.label}</span>
        <span className="bt-node-collapsed-count">
          {item.status === 'held'
            ? <HeldBadge />
            : item.children.length > 0 ? item.descendantCount : ''}
        </span>
      </div>
    );
    return (
      <div
        key={item.id}
        className={`bt-node-collapsed-item ${item.children.length > 0 ? 'group' : 'leaf'} status-${item.status}${item.invalid ? ' invalid' : ''}${item.selected ? ' selected' : ''}`}
        data-category-family={programCategoryFamily(item.category)}
      >
        {row}
        {item.children.length > 0 && (
          <div className="bt-node-collapsed-body">
            <CollapsedItems
              items={item.children}
              onSelect={onSelect}
              onItemContextMenu={onItemContextMenu}
            />
          </div>
        )}
      </div>
    );
  });
}

export function BtNode({
  node,
  onSelect,
  onFocus,
  onDelete,
  onNavigate,
  elementRef,
  onToggleCollapse,
  onItemContextMenu,
}: BtNodeProps) {
  const collapsed = node.collapsedItems !== undefined && node.collapsedItems !== null;
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
    collapsed,
    updateNodeInternals,
  ]);
  return (
    <div
      ref={elementRef}
      className={`bt-node status-${node.status}${node.invalid ? ' invalid' : ''}${node.selected ? ' selected' : ''}${collapsed ? ' collapsed' : ''}`}
      data-category={node.category}
      data-category-family={programCategoryFamily(node.category)}
      data-node-id={node.id}
      data-layout-direction={node.layoutDirection}
      role="button"
      tabIndex={0}
      aria-pressed={node.selected}
      aria-label={`${node.label} Program node`}
      title={`${node.label}${node.status === 'held' ? '\nWaiting for Run: its work starts when Run is pressed.' : ''}\n${availabilityDetail}`}
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
      {/* The empty right band holds the collapse chevron of a composite. */}
      <span className="bt-node-band">
        {node.collapsible && onToggleCollapse && (
          <button
            type="button"
            className="bt-node-fold nodrag"
            aria-label={`${collapsed ? 'Expand' : 'Collapse'} ${node.label}`}
            aria-expanded={!collapsed}
            title={collapsed ? 'Expand subtree' : 'Collapse subtree'}
            onClick={(event) => {
              event.stopPropagation();
              onToggleCollapse(node.id);
            }}
          >
            <svg viewBox="0 0 16 16" aria-hidden="true">
              <path d={collapsed ? FOLD_ICON.expand : FOLD_ICON.collapse} />
            </svg>
          </button>
        )}
      </span>
      {collapsed && (
        <div className="bt-node-collapsed-list">
          <CollapsedItems
            items={node.collapsedItems ?? []}
            onSelect={onSelect}
            onItemContextMenu={onItemContextMenu}
          />
        </div>
      )}
      {node.childrenMax !== 0 && (
        <Handle
          type="source"
          position={node.layoutDirection === 'vertical' ? Position.Right : Position.Bottom}
          isConnectable={connections.sourceConnectable}
          className={`bt-node-react-flow-handle${connections.sourceConnectable ? '' : ' disabled'}`}
          aria-label={`Connect ${node.label} as parent`}
        />
      )}
      {node.status === 'held' && <HeldBadge />}
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

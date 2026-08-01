import type {
  LayoutDirection,
  NodePresentationStatus,
} from '../store/composerStore';

export interface BtNodePresentation {
  id: string;
  label: string;
  catalogId: string;
  category: string;
  kind: string;
  childrenMax: number;
  childCount: number;
  hasParent: boolean;
  status: NodePresentationStatus;
  selected: boolean;
  layoutDirection: LayoutDirection;
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

/**
 * Catalog-node presentation retained without a dependency on the deleted v1
 * Program store. An I6 graph adapter can supply qualified node data/handles.
 */
export function BtNode({ node }: { node: BtNodePresentation }) {
  const connections = btNodeConnectionAvailability(node);
  const glyph = node.kind === 'composite'
    ? '◆'
    : node.kind === 'decorator'
      ? '◇'
      : '•';
  return (
    <div
      className={`bt-node category-${node.category} status-${node.status}${node.selected ? ' selected' : ''}`}
      data-node-id={node.id}
      data-layout-direction={node.layoutDirection}
    >
      <span
        className={`bt-node-handle target${connections.targetConnectable ? '' : ' disabled'}`}
        aria-hidden="true"
      />
      <span className="bt-node-kind" aria-hidden="true">{glyph}</span>
      <small>{node.catalogId}</small>
      <strong>{node.label}</strong>
      <span className="bt-node-child-hint">
        {childHint(node.kind, node.childrenMax)}
      </span>
      {node.childrenMax !== 0 && (
        <span
          className={`bt-node-handle source${connections.sourceConnectable ? '' : ' disabled'}`}
          aria-hidden="true"
        />
      )}
    </div>
  );
}

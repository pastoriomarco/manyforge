import { useMemo, useState } from 'react';

import { useComposerViewState } from '../store/composerStore';
import type {
  CatalogEntry,
  JsonValue,
  NodeEnvironmentResponse,
  ProgramTreeNode,
} from '../types/api';
import {
  flattenProgramTree,
} from '../utils/programAuthoring';
import { layoutAuthoringGraph } from '../utils/authoringGraph';
import type { AuthoringGraphEdge, AuthoringGraphNode } from '../utils/authoringGraph';
import { BtNode } from './BtNode';
import { CanvasViewControls } from './CanvasViewControls';
import { StatusEdge } from './StatusEdge';

const NODE_WIDTH = 180;
const NODE_HEIGHT = 70;
const STAGE_PADDING = 48;

interface ProgramTreeCanvasProps {
  root: ProgramTreeNode;
  catalogEntries: CatalogEntry[];
  environment: NodeEnvironmentResponse | null;
  selectedId: string | null;
  busy: boolean;
  onSelect: (instanceId: string) => void;
  onMutate: (mutation: string, args: Record<string, JsonValue>) => void;
}

function descendants(node: ProgramTreeNode): Set<string> {
  return new Set(flattenProgramTree(node).map(({ node: row }) => row.instance_id));
}

export function ProgramTreeCanvas({
  root,
  catalogEntries,
  environment,
  selectedId,
  busy,
  onSelect,
  onMutate,
}: ProgramTreeCanvasProps) {
  const [view, dispatchView] = useComposerViewState();
  const [layoutMessage, setLayoutMessage] = useState<string | null>(null);
  const flat = useMemo(() => flattenProgramTree(root), [root]);
  const catalog = useMemo(
    () => new Map(catalogEntries.map((entry) => [entry.id, entry])),
    [catalogEntries],
  );
  const availability = useMemo(() => {
    const result = new Map<string, { authoring: boolean; execution: boolean }>();
    for (const row of environment?.implementations ?? []) {
      const previous = result.get(row.nodeId) ?? { authoring: false, execution: false };
      result.set(row.nodeId, {
        authoring: previous.authoring || row.authoringAvailable,
        execution: previous.execution || row.executionAvailable,
      });
    }
    return result;
  }, [environment]);

  const edges = useMemo<AuthoringGraphEdge[]>(
    () => flat
      .filter((row) => row.parentId !== null)
      .map((row) => ({ source: row.parentId as string, target: row.node.instance_id })),
    [flat],
  );
  const laidOut = useMemo(() => {
    const seeds: AuthoringGraphNode[] = flat.map((row, order) => ({
      id: row.node.instance_id,
      label: row.node.instance_id,
      width: NODE_WIDTH,
      height: NODE_HEIGHT,
      childrenMax: catalog.get(row.node.node_id)?.childrenMax,
      position: view.layoutDirection === 'vertical'
        ? { x: order * (NODE_WIDTH + 24), y: row.depth * 150 }
        : { x: row.depth * 250, y: order * (NODE_HEIGHT + 24) },
    }));
    const positioned = layoutAuthoringGraph(
      seeds,
      edges,
      view.layoutDirection,
      view.layoutDirection,
    );
    const minX = Math.min(...positioned.map((node) => node.position.x), 0);
    const minY = Math.min(...positioned.map((node) => node.position.y), 0);
    return positioned.map((node) => ({
      ...node,
      position: {
        x: node.position.x - minX + STAGE_PADDING,
        y: node.position.y - minY + STAGE_PADDING,
      },
    }));
  }, [catalog, edges, flat, view.layoutDirection]);
  const positions = useMemo(
    () => new Map(laidOut.map((node) => [node.id, node.position])),
    [laidOut],
  );
  const stageWidth = Math.max(
    720,
    ...laidOut.map((node) => node.position.x + NODE_WIDTH + STAGE_PADDING),
  );
  const stageHeight = Math.max(
    320,
    ...laidOut.map((node) => node.position.y + NODE_HEIGHT + STAGE_PADDING),
  );

  const selectedRow = flat.find((row) => row.node.instance_id === selectedId) ?? null;
  const selectedBlocked = selectedRow ? descendants(selectedRow.node) : new Set<string>();
  const parentOptions = selectedRow === null || selectedRow.parentId === null
    ? []
    : flat.filter((candidate) => {
        if (selectedBlocked.has(candidate.node.instance_id)) return false;
        const entry = catalog.get(candidate.node.node_id);
        if (!entry || entry.childrenMax === 0) return false;
        const alreadyChild = selectedRow.parentId === candidate.node.instance_id;
        return alreadyChild
          || entry.childrenMax < 0
          || candidate.node.children.length < entry.childrenMax;
      });

  return (
    <section className="program-tree-surface" aria-labelledby="program-tree-title">
      <header className="program-subpanel-header">
        <div>
          <p className="eyebrow">Validated Program v1 topology</p>
          <h3 id="program-tree-title">Tree canvas</h3>
        </div>
        <CanvasViewControls
          edgeStyle={view.edgeStyle}
          layoutDirection={view.layoutDirection}
          disabled={busy}
          onEdgeStyleChange={(edgeStyle) => dispatchView({
            type: 'set_edge_style',
            edgeStyle,
          })}
          onLayoutDirectionChange={(layoutDirection) => dispatchView({
            type: 'set_layout_direction',
            layoutDirection,
          })}
          onAutoLayout={() => setLayoutMessage('Deterministic layout refreshed.')}
        />
      </header>

      <div className="program-tree-scroll">
        <div
          className="program-tree-stage"
          style={{ width: stageWidth, height: stageHeight }}
        >
          <svg
            className="program-tree-edges"
            width={stageWidth}
            height={stageHeight}
            aria-hidden="true"
          >
            {edges.map((edge) => {
              const source = positions.get(edge.source);
              const target = positions.get(edge.target);
              if (!source || !target) return null;
              const vertical = view.layoutDirection === 'vertical';
              return (
                <StatusEdge
                  key={`${edge.source}:${edge.target}`}
                  edgeStyle={view.edgeStyle}
                  sourceX={source.x + (vertical ? NODE_WIDTH / 2 : NODE_WIDTH)}
                  sourceY={source.y + (vertical ? NODE_HEIGHT : NODE_HEIGHT / 2)}
                  targetX={target.x + (vertical ? NODE_WIDTH / 2 : 0)}
                  targetY={target.y + (vertical ? 0 : NODE_HEIGHT / 2)}
                />
              );
            })}
          </svg>
          {flat.map((row) => {
            const position = positions.get(row.node.instance_id);
            const entry = catalog.get(row.node.node_id);
            if (!position) return null;
            const nodeAvailability = availability.get(row.node.node_id);
            return (
              <button
                key={row.node.instance_id}
                type="button"
                className="program-canvas-node-button"
                style={{ left: position.x, top: position.y }}
                onClick={() => onSelect(row.node.instance_id)}
                aria-pressed={selectedId === row.node.instance_id}
                title={nodeAvailability?.execution
                  ? 'Execution implementation available'
                  : 'Authorable here; execution remains unavailable at its restoration gate'}
              >
                <BtNode node={{
                  id: row.node.instance_id,
                  label: row.node.instance_id,
                  catalogId: row.node.node_id,
                  category: entry?.category ?? 'unknown',
                  kind: entry?.kind ?? 'leaf',
                  childrenMax: entry?.childrenMax ?? row.node.children.length,
                  childCount: row.node.children.length,
                  hasParent: row.parentId !== null,
                  status: 'idle',
                  selected: selectedId === row.node.instance_id,
                  layoutDirection: view.layoutDirection,
                }} />
                <span className={`node-availability-dot ${nodeAvailability?.execution ? 'execution-ready' : 'authoring-only'}`} />
              </button>
            );
          })}
        </div>
      </div>

      <div className="program-tree-actions">
        <span>
          {selectedRow
            ? `Selected ${selectedRow.node.instance_id}`
            : 'Select one node to edit its properties or topology.'}
        </span>
        {selectedRow && selectedRow.parentId !== null && (
          <>
            <button
              type="button"
              disabled={busy || selectedRow.index === 0}
              onClick={() => onMutate('tree_draft_move_node', {
                instance_id: selectedRow.node.instance_id,
                new_parent_instance_id: selectedRow.parentId as string,
                index: selectedRow.index - 1,
              })}
            >
              Move earlier
            </button>
            <button
              type="button"
              disabled={busy || (() => {
                const parent = flat.find((row) => row.node.instance_id === selectedRow.parentId);
                return !parent || selectedRow.index >= parent.node.children.length - 1;
              })()}
              onClick={() => onMutate('tree_draft_move_node', {
                instance_id: selectedRow.node.instance_id,
                new_parent_instance_id: selectedRow.parentId as string,
                index: selectedRow.index + 1,
              })}
            >
              Move later
            </button>
            <label className="inline-select-label">
              Reparent
              <select
                value={selectedRow.parentId}
                disabled={busy}
                onChange={(event) => {
                  if (event.target.value === selectedRow.parentId) return;
                  const parent = flat.find(
                    (row) => row.node.instance_id === event.target.value,
                  );
                  onMutate('tree_draft_move_node', {
                    instance_id: selectedRow.node.instance_id,
                    new_parent_instance_id: event.target.value,
                    index: parent?.node.children.length ?? 0,
                  });
                }}
              >
                {parentOptions.map((row) => (
                  <option key={row.node.instance_id} value={row.node.instance_id}>
                    {row.node.instance_id}
                  </option>
                ))}
              </select>
            </label>
            <button
              type="button"
              className="danger-button"
              disabled={busy}
              onClick={() => onMutate('tree_draft_delete_node', {
                instance_id: selectedRow.node.instance_id,
              })}
            >
              Delete
            </button>
          </>
        )}
      </div>
      {layoutMessage && <p className="program-inline-message">{layoutMessage}</p>}
    </section>
  );
}

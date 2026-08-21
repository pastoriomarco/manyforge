import type {
  CatalogEntry,
  JsonValue,
  ProgramTreeNode,
} from '../types/api';
import { flattenProgramTree } from './programAuthoring';

export type ProgramInsertionPlacement = 'child' | 'sibling' | 'wrapper';

export interface ProgramInsertionRequest {
  requestId: number;
  catalogId: string;
  targetId: string;
  placement: ProgramInsertionPlacement;
}

export interface ProgramPaletteActivation {
  catalogId: string;
}

export const PROGRAM_NODE_DRAG_MIME = 'application/manyforge-node';

export function programPaletteClickIntent(entry: CatalogEntry): ProgramPaletteActivation {
  return { catalogId: entry.id };
}

export function programPaletteDragData(entry: CatalogEntry): Record<string, string> {
  return {
    [PROGRAM_NODE_DRAG_MIME]: entry.id,
    'text/plain': entry.id,
  };
}

export function readProgramPaletteDrag(
  getData: (format: string) => string,
): string {
  return getData(PROGRAM_NODE_DRAG_MIME) || getData('text/plain');
}

/**
 * Return only placements that can be staged against the immutable Draft
 * projection. This intentionally does not issue a mutation; the palette uses
 * the same result for click and canvas-drop insertion.
 */
export function validProgramInsertionPlacements(
  root: ProgramTreeNode,
  entries: CatalogEntry[],
  targetId: string | null,
  stagedEntry: CatalogEntry,
): ProgramInsertionPlacement[] {
  if (!targetId) return [];
  const flat = flattenProgramTree(root);
  const target = flat.find(({ node }) => node.instance_id === targetId);
  if (!target) return [];
  const catalog = new Map(entries.map((entry) => [entry.id, entry]));
  const targetEntry = catalog.get(target.node.node_id);
  const childCapacity = targetEntry !== undefined
    && targetEntry.childrenMax !== 0
    && (
      targetEntry.childrenMax < 0
      || target.node.children.length < targetEntry.childrenMax
    );
  const result: ProgramInsertionPlacement[] = [];
  if (childCapacity && stagedEntry.childrenMin === 0) result.push('child');
  if (target.parentId !== null && stagedEntry.childrenMin === 0) result.push('sibling');
  if (
    stagedEntry.childrenMin <= 1
    && stagedEntry.childrenMax !== 0
    && (stagedEntry.childrenMax < 0 || stagedEntry.childrenMax >= 1)
  ) {
    result.push('wrapper');
  }
  return result;
}

export interface ProgramInsertionNode extends Record<string, JsonValue> {
  node_id: string;
  instance_id: string;
  parameters: Record<string, JsonValue>;
  children: [];
}

export type ProgramDraftMutation = {
  mutation:
    | 'tree_draft_insert_node'
    | 'tree_draft_wrap_node'
    | 'tree_draft_move_node'
    | 'tree_draft_delete_node';
  args: Record<string, JsonValue>;
};

export function programInsertionMutation(
  targetId: string,
  placement: ProgramInsertionPlacement,
  childCount: number,
  node: ProgramInsertionNode,
): ProgramDraftMutation {
  if (placement === 'wrapper') {
    return {
      mutation: 'tree_draft_wrap_node',
      args: {
        target_instance_id: targetId,
        wrapper: {
          node_id: node.node_id,
          instance_id: node.instance_id,
          parameters: node.parameters,
        },
      },
    };
  }
  if (placement === 'child') {
    return {
      mutation: 'tree_draft_insert_node',
      args: {
        parent_instance_id: targetId,
        index: childCount,
        node,
      },
    };
  }
  return {
    mutation: 'tree_draft_insert_node',
    args: {
      after_instance_id: targetId,
      node,
    },
  };
}

export function programMoveMutation(
  instanceId: string,
  newParentInstanceId: string,
  index: number,
): ProgramDraftMutation {
  return {
    mutation: 'tree_draft_move_node',
    args: {
      instance_id: instanceId,
      new_parent_instance_id: newParentInstanceId,
      index,
    },
  };
}

export function programDeleteMutation(
  instanceId: string,
  isRoot: boolean,
): ProgramDraftMutation | null {
  if (isRoot) return null;
  return {
    mutation: 'tree_draft_delete_node',
    args: { instance_id: instanceId },
  };
}

export interface ProgramCanvasDropResult {
  request: ProgramInsertionRequest | null;
  error: string | null;
}

export function stageProgramCanvasDrop(
  root: ProgramTreeNode,
  catalogEntries: CatalogEntry[],
  catalogId: string,
  targetId: string,
  requestId: number,
): ProgramCanvasDropResult {
  const entry = catalogEntries.find((candidate) => candidate.id === catalogId);
  if (!entry) {
    return {
      request: null,
      error: `Unknown catalog entry '${catalogId}'.`,
    };
  }
  const placement = validProgramInsertionPlacements(
    root,
    catalogEntries,
    targetId,
    entry,
  )[0];
  if (!placement) {
    return {
      request: null,
      error: `No schema-valid insertion placement is available relative to '${targetId}'.`,
    };
  }
  return {
    request: {
      requestId,
      catalogId,
      targetId,
      placement,
    },
    error: null,
  };
}

export type ProgramCanvasKeyboardTarget = 'node' | 'form';

export type ProgramCanvasKeyIntent =
  | { type: 'delete' }
  | { type: 'select' }
  | { type: 'navigate'; key: string }
  | null;

export function programCanvasKeyIntent(
  key: string,
  target: ProgramCanvasKeyboardTarget = 'node',
): ProgramCanvasKeyIntent {
  if (target === 'form') return null;
  if (key === 'Delete' || key === 'Backspace') return { type: 'delete' };
  if (key === 'Enter' || key === ' ') return { type: 'select' };
  if (
    key === 'ArrowUp'
    || key === 'ArrowDown'
    || key === 'ArrowLeft'
    || key === 'ArrowRight'
    || key === 'Home'
    || key === 'End'
  ) {
    return { type: 'navigate', key };
  }
  return null;
}

export function isProgramCanvasFormTarget(target: EventTarget | null): boolean {
  return typeof HTMLElement !== 'undefined'
    && target instanceof HTMLElement
    && Boolean(target.closest('input, textarea, select, button, [contenteditable="true"]'));
}

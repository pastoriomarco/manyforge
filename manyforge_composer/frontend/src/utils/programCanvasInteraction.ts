import type {
  CatalogEntry,
  JsonValue,
  ProgramTreeNode,
} from '../types/api';
import { catalogDefaultParameters, nextProgramInstanceId } from './programAuthoring';

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
 * Flow position of a node dropped from the palette: the drag preview is
 * centred on the pointer, so the node is placed with its centre there too.
 */
export function programPaletteDropPosition(
  pointer: { x: number; y: number },
  nodeSize: { width: number; height: number },
): { x: number; y: number } {
  return {
    x: pointer.x - nodeSize.width / 2,
    y: pointer.y - nodeSize.height / 2,
  };
}

export type ProgramDraftMutation = {
  mutation:
    | 'tree_draft_insert_node'
    | 'tree_draft_change_node_kind'
    | 'tree_draft_wrap_node'
    | 'tree_draft_move_node'
    | 'tree_draft_delete_node'
    | 'tree_draft_replace_subtree';
  args: Record<string, JsonValue>;
};

export function programDetachedInsertionMutation(
  entry: CatalogEntry,
  existingInstanceIds: string[],
  index: number,
  position?: { x: number; y: number },
): ProgramDraftMutation {
  return programInsertionMutation(entry, existingInstanceIds, {
    kind: 'root',
    index,
    position,
  });
}

export type ProgramInsertionPlacement =
  | {
      kind: 'root';
      index: number;
      position?: { x: number; y: number };
    }
  | { kind: 'child'; parentInstanceId: string; index: number }
  | { kind: 'after'; siblingInstanceId: string };

function insertionPlacementArgs(
  placement: ProgramInsertionPlacement,
): Record<string, JsonValue> {
  if (placement.kind === 'root') {
    return {
      index: placement.index,
      ...(placement.position === undefined ? {} : { position: placement.position }),
    };
  }
  if (placement.kind === 'child') {
    return {
      parent_instance_id: placement.parentInstanceId,
      index: placement.index,
    };
  }
  return { after_instance_id: placement.siblingInstanceId };
}

export function programInsertionMutation(
  entry: CatalogEntry,
  existingInstanceIds: string[],
  placement: ProgramInsertionPlacement,
): ProgramDraftMutation {
  const instanceId = nextProgramInstanceId(entry.id, existingInstanceIds);
  return {
    mutation: 'tree_draft_insert_node',
    args: {
      ...insertionPlacementArgs(placement),
      node: {
        node_id: entry.id,
        instance_id: instanceId,
        parameters: catalogDefaultParameters(entry),
        children: [],
      },
    },
  };
}

/**
 * A name for a copy that keeps the original's: pick_approach becomes
 * pick_approach_2 (and pick_approach_2 becomes pick_approach_3).
 */
export function programCopyInstanceId(
  instanceId: string,
  existingIds: ReadonlySet<string>,
): string {
  const stem = instanceId.replace(/_\d+$/, '') || instanceId;
  let suffix = 2;
  while (existingIds.has(`${stem}_${suffix}`)) suffix += 1;
  return `${stem}_${suffix}`;
}

/** A deep copy of one subtree in which every node has a fresh instance id. */
export function programSubtreeCopy(
  node: ProgramTreeNode,
  existingInstanceIds: Iterable<string>,
): ProgramTreeNode {
  const taken = new Set(existingInstanceIds);
  const copy = (source: ProgramTreeNode): ProgramTreeNode => {
    const { children, ...fields } = source;
    const instanceId = programCopyInstanceId(source.instance_id, taken);
    taken.add(instanceId);
    return { ...structuredClone(fields), instance_id: instanceId, children: children.map(copy) };
  };
  return copy(node);
}

/** Insert one whole subtree (a duplicate or a pasted copy) in one edit. */
export function programSubtreeInsertionMutation(
  subtree: ProgramTreeNode,
  placement: ProgramInsertionPlacement,
): ProgramDraftMutation {
  return {
    mutation: 'tree_draft_insert_node',
    args: {
      ...insertionPlacementArgs(placement),
      node: subtree as unknown as JsonValue,
    },
  };
}

/** Replace a node that has exactly one child by that child, atomically. */
export function programUnwrapMutation(node: ProgramTreeNode): ProgramDraftMutation {
  const [child] = node.children;
  if (child === undefined || node.children.length !== 1) {
    throw new Error('only a node with exactly one child can be unwrapped');
  }
  return {
    mutation: 'tree_draft_replace_subtree',
    args: {
      instance_id: node.instance_id,
      subtree: child as unknown as JsonValue,
    },
  };
}

export function programWrapMutation(
  entry: CatalogEntry,
  existingInstanceIds: string[],
  targetInstanceId: string,
): ProgramDraftMutation {
  return {
    mutation: 'tree_draft_wrap_node',
    args: {
      target_instance_id: targetInstanceId,
      wrapper: {
        node_id: entry.id,
        instance_id: nextProgramInstanceId(entry.id, existingInstanceIds),
        parameters: catalogDefaultParameters(entry),
      },
    },
  };
}

export function programChangeKindMutation(
  entry: CatalogEntry,
  instanceId: string,
  retainedParameterNames: Iterable<string>,
): ProgramDraftMutation {
  const retained = new Set(retainedParameterNames);
  const defaults = catalogDefaultParameters(entry);
  const parameters = Object.fromEntries(
    Object.entries(defaults).filter(([name]) => !retained.has(name)),
  );
  return {
    mutation: 'tree_draft_change_node_kind',
    args: {
      instance_id: instanceId,
      replacement: {
        node_id: entry.id,
        parameters,
      },
      parameters_mode: 'strict',
    },
  };
}

export function programMoveMutation(
  instanceId: string,
  newParentInstanceId: string | null,
  index: number,
  positions?: Record<string, { x: number; y: number }>,
): ProgramDraftMutation {
  return {
    mutation: 'tree_draft_move_node',
    args: {
      instance_id: instanceId,
      new_parent_instance_id: newParentInstanceId,
      index,
      ...(positions === undefined ? {} : { positions }),
    },
  };
}

export function programDeleteMutation(
  instanceId: string,
): ProgramDraftMutation {
  return {
    mutation: 'tree_draft_delete_node',
    args: { instance_id: instanceId },
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

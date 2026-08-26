import type {
  CatalogEntry,
  JsonValue,
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

export type ProgramDraftMutation = {
  mutation:
    | 'tree_draft_insert_node'
    | 'tree_draft_wrap_node'
    | 'tree_draft_move_node'
    | 'tree_draft_delete_node';
  args: Record<string, JsonValue>;
};

export function programDetachedInsertionMutation(
  entry: CatalogEntry,
  existingInstanceIds: string[],
  index: number,
  position?: { x: number; y: number },
): ProgramDraftMutation {
  const instanceId = nextProgramInstanceId(entry.id, existingInstanceIds);
  return {
    mutation: 'tree_draft_insert_node',
    args: {
      index,
      node: {
        node_id: entry.id,
        instance_id: instanceId,
        parameters: catalogDefaultParameters(entry),
        children: [],
      },
      ...(position === undefined ? {} : { position }),
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

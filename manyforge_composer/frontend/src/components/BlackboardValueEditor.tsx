import type { ReactNode } from 'react';

import type { CatalogJsonSchema, JsonValue } from '../types/api';
import { PROGRAM_AUTHORED_VALUE_TYPES } from '../types/api';
import type { FrameScope } from '../utils/programFrames';
import { ProgramSpecializedEditor } from './property/ProgramSpecializedEditors';

/**
 * One typed value of a blackboard key, edited by the key's declared type.
 * Shared by the runtime blackboard, stored persistent values and Set value.
 *
 * The editor works on a draft string per type (a number as typed, one list
 * item per line, a pose as JSON), so an unfinished entry is kept and reported
 * instead of being coerced. `parseBlackboardValueDraft` turns it into the
 * value to send; the Program runner validates it again.
 */

type JsonObject = Record<string, JsonValue>;

/** Key types with a typed editor; other types are written only by nodes. */
export function blackboardValueEditable(type: string): boolean {
  return PROGRAM_AUTHORED_VALUE_TYPES.has(type);
}

const FRAME_REFERENCE_SCHEMA: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['cell_frame', 'target_frame', 'scene_resource'] },
    target_role: { type: 'string', minLength: 1 },
    frame_role: { type: 'string', minLength: 1 },
    scene_resource_id: { type: 'string', minLength: 1 },
  },
  allOf: [
    {
      if: { required: ['kind'], properties: { kind: { const: 'target_frame' } } },
      then: { required: ['frame_role', 'target_role'] },
    },
    {
      if: { required: ['kind'], properties: { kind: { const: 'scene_resource' } } },
      then: { required: ['scene_resource_id'] },
    },
  ],
};

/** The catalog's `stored_pose`: a frame reference, a position and an orientation. */
const STORED_POSE_SCHEMA: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: false,
  required: ['frame_ref', 'position_m', 'orientation'],
  properties: {
    frame_ref: FRAME_REFERENCE_SCHEMA,
    position_m: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
    orientation: {
      type: 'object',
      additionalProperties: false,
      properties: {
        rpy_deg: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
        xyzw: { type: 'array', minItems: 4, maxItems: 4, items: { type: 'number' } },
      },
      allOf: [{ oneOf: [{ required: ['rpy_deg'] }, { required: ['xyzw'] }] }],
    },
  },
};

const DEFAULT_POSE: JsonObject = {
  frame_ref: { kind: 'cell_frame' },
  position_m: [0, 0, 0],
  orientation: { rpy_deg: [0, 0, 0] },
};

/** The draft that shows `value`, or a neutral starting draft when undefined. */
export function blackboardValueDraft(
  type: string,
  value: JsonValue | undefined,
  sceneResourceIds: readonly string[] = [],
): string {
  if (type === 'bool') return value === true ? 'true' : 'false';
  if (type === 'int64' || type === 'float64') {
    return typeof value === 'number' ? String(value) : '0';
  }
  if (type === 'string') return typeof value === 'string' ? value : '';
  if (type === 'string_list') {
    return Array.isArray(value) ? value.map(String).join('\n') : '';
  }
  if (type === 'scene_resource') {
    return typeof value === 'string' ? value : sceneResourceIds[0] ?? '';
  }
  if (type === 'stored_pose') {
    return JSON.stringify(value !== undefined && value !== null ? value : DEFAULT_POSE);
  }
  return '';
}

function numbersOf(value: JsonValue | undefined, length: number): boolean {
  return Array.isArray(value) && value.length === length
    && value.every((item) => typeof item === 'number' && Number.isFinite(item));
}

function nonEmptyText(value: JsonValue | undefined): boolean {
  return typeof value === 'string' && value.trim() !== '';
}

function parsePose(draft: string, sceneResourceIds: readonly string[]): JsonObject {
  let value: JsonValue;
  try {
    value = JSON.parse(draft) as JsonValue;
  } catch {
    throw new Error('Pose is incomplete.');
  }
  if (value === null || typeof value !== 'object' || Array.isArray(value)) {
    throw new Error('Pose is incomplete.');
  }
  const pose = value as JsonObject;
  if (!numbersOf(pose.position_m, 3)) {
    throw new Error('Position needs three finite numbers (m).');
  }
  const orientation = pose.orientation as JsonObject | undefined;
  if (
    orientation === undefined || orientation === null || typeof orientation !== 'object'
    || !(numbersOf(orientation.rpy_deg, 3) || numbersOf(orientation.xyzw, 4))
  ) {
    throw new Error('Orientation needs roll, pitch and yaw or a quaternion.');
  }
  const frame = pose.frame_ref as JsonObject | undefined;
  const kind = frame !== undefined && frame !== null && typeof frame === 'object'
    ? frame.kind : undefined;
  if (
    !(kind === 'cell_frame'
      || (kind === 'target_frame' && nonEmptyText(frame!.target_role)
        && nonEmptyText(frame!.frame_role))
      || (kind === 'scene_resource'
        && sceneResourceIds.includes(String(frame!.scene_resource_id))))
  ) {
    throw new Error(
      'Frame reference must be the cell, a target frame with both roles, or a declared scene resource.',
    );
  }
  return pose;
}

/** The value a draft stands for; throws an Error naming what is missing. */
export function parseBlackboardValueDraft(
  type: string,
  draft: string,
  sceneResourceIds: readonly string[] = [],
): JsonValue {
  const trimmed = draft.trim();
  if (type === 'bool') {
    if (draft === 'true') return true;
    if (draft === 'false') return false;
    throw new Error('Choose true or false.');
  }
  if (type === 'int64') {
    const value = Number(trimmed);
    if (!/^-?\d+$/.test(trimmed) || !Number.isSafeInteger(value)) {
      throw new Error('Enter a whole number between ±9007199254740991.');
    }
    return value;
  }
  if (type === 'float64') {
    const value = Number(trimmed);
    if (trimmed === '' || !Number.isFinite(value)) throw new Error('Enter a finite number.');
    return value;
  }
  if (type === 'string') {
    if (draft === '') throw new Error('Enter some text.');
    return draft;
  }
  if (type === 'string_list') {
    return draft.split('\n').map((line) => line.trim()).filter((line) => line !== '');
  }
  if (type === 'scene_resource') {
    if (!sceneResourceIds.includes(draft)) {
      throw new Error('Choose a scene resource declared by the Program.');
    }
    return draft;
  }
  if (type === 'stored_pose') return parsePose(draft, sceneResourceIds);
  throw new Error(`${type} values are written only by Program nodes.`);
}

/** Why a draft cannot be sent yet, or null when it is a complete value. */
export function blackboardValueDraftIssue(
  type: string,
  draft: string,
  sceneResourceIds: readonly string[] = [],
): string | null {
  try {
    parseBlackboardValueDraft(type, draft, sceneResourceIds);
    return null;
  } catch (error) {
    return error instanceof Error ? error.message : 'Value is invalid.';
  }
}

/** A short read-only rendering of one value for lists. */
export function formatBlackboardValue(type: string, value: JsonValue | undefined): string {
  if (value === undefined || value === null) return 'empty';
  if (type === 'string_list' && Array.isArray(value)) {
    return value.length === 0 ? 'no items' : value.map(String).join(', ');
  }
  if (type === 'stored_pose' && typeof value === 'object' && !Array.isArray(value)) {
    const pose = value as JsonObject;
    const frame = pose.frame_ref as JsonObject | undefined;
    const position = Array.isArray(pose.position_m) ? pose.position_m.join(', ') : '?';
    const where = frame?.kind === 'scene_resource'
      ? String(frame.scene_resource_id)
      : frame?.kind === 'target_frame'
        ? `${String(frame.target_role)}/${String(frame.frame_role)}`
        : 'cell';
    return `${where} · [${position}] m`;
  }
  return typeof value === 'string' ? value : JSON.stringify(value);
}

export interface BlackboardValueEditorProps {
  id: string;
  label: string;
  type: string;
  draft: string;
  sceneResourceIds: readonly string[];
  disabled: boolean;
  onDraftChange: (draft: string) => void;
  /** An unfinished pose field the draft does not yet contain. */
  onInvalidChange?: (message: string | null) => void;
  /** In a Program node: the value's path, where its pose fields may come from sources. */
  path?: string;
  /** In a Program node: wraps a single-field control so it can show a source in its place. */
  wrapControl?: (control: ReactNode) => ReactNode;
  /** The Program's declared frames (and the key's `allowed_frames`) a pose may use. */
  frameScope?: FrameScope;
}

export function BlackboardValueEditor({
  id,
  label,
  type,
  draft,
  sceneResourceIds,
  disabled,
  onDraftChange,
  onInvalidChange,
  path,
  wrapControl = (control) => control,
  frameScope,
}: BlackboardValueEditorProps) {
  if (type === 'stored_pose') {
    let pose: JsonValue = DEFAULT_POSE;
    try {
      pose = JSON.parse(draft) as JsonValue;
    } catch {
      // An unreadable draft starts again from the neutral pose.
    }
    return (
      <fieldset id={id} className="runtime-control-field" disabled={disabled}>
        <legend>{label}</legend>
        <ProgramSpecializedEditor
          schema={STORED_POSE_SCHEMA}
          value={pose}
          disabled={disabled}
          path={path}
          frameScope={frameScope}
          onCommit={(next) => onDraftChange(JSON.stringify(next))}
          onInvalidChange={onInvalidChange}
        />
      </fieldset>
    );
  }
  let control: ReactNode;
  if (type === 'bool') {
    control = (
      <select id={id} value={draft} disabled={disabled} onChange={(event) => onDraftChange(event.currentTarget.value)}>
        <option value="true">True</option>
        <option value="false">False</option>
      </select>
    );
  } else if (type === 'int64' || type === 'float64') {
    control = (
      <input
        id={id}
        type="number"
        step={type === 'int64' ? '1' : 'any'}
        value={draft}
        disabled={disabled}
        onChange={(event) => onDraftChange(event.currentTarget.value)}
      />
    );
  } else if (type === 'string') {
    control = (
      <input id={id} type="text" value={draft} disabled={disabled} onChange={(event) => onDraftChange(event.currentTarget.value)} />
    );
  } else if (type === 'string_list') {
    control = (
      <textarea
        id={id}
        rows={3}
        value={draft}
        placeholder="One item per line"
        disabled={disabled}
        spellCheck={false}
        onChange={(event) => onDraftChange(event.currentTarget.value)}
      />
    );
  } else if (type === 'scene_resource') {
    control = (
      <select
        id={id}
        value={draft}
        disabled={disabled || sceneResourceIds.length === 0}
        onChange={(event) => onDraftChange(event.currentTarget.value)}
      >
        {!sceneResourceIds.includes(draft) && (
          <option value={draft}>
            {sceneResourceIds.length === 0 ? 'The Program declares no scene resources' : 'Choose a scene resource'}
          </option>
        )}
        {sceneResourceIds.map((resourceId) => (
          <option key={resourceId} value={resourceId}>{resourceId}</option>
        ))}
      </select>
    );
  } else {
    return (
      <p className="hint">{type} values are written only by Program nodes.</p>
    );
  }
  return (
    <label className="runtime-control-field">
      <span>{label}</span>
      {wrapControl(control)}
    </label>
  );
}

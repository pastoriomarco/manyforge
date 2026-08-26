import {
  createContext,
  useCallback,
  useContext,
  useEffect,
  useMemo,
  useRef,
  useState,
  type ReactNode,
} from 'react';

import type { CatalogJsonSchema, JsonValue } from '../../types/api';
import { firstCatalogSchemaValidationMessage } from '../../utils/catalogSchema';
import type { ProgramSourceJointInventory } from '../../utils/programSourceJoints';
import { DraftTextInput } from '../common/DraftInputs';

type JsonObject = Record<string, JsonValue>;
type SpecializedInvalidReporter = (token: symbol, message: string | null) => void;
const SpecializedInvalidContext = createContext<SpecializedInvalidReporter>(() => undefined);

function useSpecializedInvalidReporter() {
  const report = useContext(SpecializedInvalidContext);
  const token = useRef(Symbol('specialized-field'));
  useEffect(() => () => report(token.current, null), [report]);
  return useCallback((message: string | null) => {
    report(token.current, message);
  }, [report]);
}

function objectValue(value: JsonValue): JsonObject {
  return value as JsonObject;
}

function propertyNames(schema: CatalogJsonSchema | undefined): Set<string> {
  return new Set(Object.keys(schema?.properties ?? {}));
}

function hasProperties(schema: CatalogJsonSchema | undefined, names: string[]): boolean {
  const present = propertyNames(schema);
  return names.every((name) => present.has(name));
}

function enumStrings(schema: CatalogJsonSchema | undefined): string[] {
  return (schema?.enum ?? []).filter((value): value is string => typeof value === 'string');
}

function numericArray(value: JsonValue | undefined): number[] {
  return Array.isArray(value) ? value as number[] : [];
}

function NumberField({
  label,
  value,
  disabled,
  onCommit,
}: {
  label: string;
  value: number;
  disabled: boolean;
  onCommit: (value: number) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  return (
    <label className="program-specialized-number">
      <span>{label}</span>
      <DraftTextInput
        type="number"
        step="any"
        value={value}
        disabled={disabled}
        aria-label={label}
        validate={(text) => text.trim() !== '' && Number.isFinite(Number(text))
          ? null
          : `${label} must be a finite number.`}
        onInvalidChange={reportInvalid}
        onCommit={(text) => {
          const parsed = Number(text);
          if (Number.isFinite(parsed)) onCommit(parsed);
        }}
      />
    </label>
  );
}

function VectorEditor({
  label,
  units,
  labels,
  value,
  disabled,
  onCommit,
}: {
  label: string;
  units: string;
  labels: string[];
  value: number[];
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  return (
    <fieldset className="program-vector-editor">
      <legend>{label} <span>{units}</span></legend>
      <div>
        {labels.map((axis, index) => (
          <NumberField
            key={axis}
            label={`${label} ${axis}`}
            value={value[index] ?? 0}
            disabled={disabled}
            onCommit={(nextValue) => {
              const next = [...value];
              next[index] = nextValue;
              onCommit(next);
            }}
          />
        ))}
      </div>
    </fieldset>
  );
}

function frameReferenceFields(kind: string): string[] {
  if (kind === 'target_frame') return ['target_role', 'frame_role'];
  if (kind === 'scene_resource') return ['scene_resource_id'];
  return [];
}

/**
 * Return a clean frame-reference variant. Undefined means the transition is
 * intentionally staged because the selected variant still lacks required
 * roles. No stale field from the old variant is copied.
 */
export function completeFrameReferenceTransition(
  schema: CatalogJsonSchema,
  kind: string,
  fields: Record<string, string>,
): JsonObject | undefined {
  const candidate: JsonObject = { kind };
  for (const field of frameReferenceFields(kind)) {
    const value = fields[field]?.trim();
    if (!value) return undefined;
    candidate[field] = value;
  }
  return firstCatalogSchemaValidationMessage(schema, candidate, 'frame_ref')
    ? undefined
    : candidate;
}

export function completeMotionPoseFrameAddTransition(
  schema: CatalogJsonSchema,
  value: JsonObject,
): JsonObject | undefined {
  const frameSchema = schema.properties?.frame_ref;
  if (!frameSchema || value.frame_ref !== undefined) return undefined;
  const kindSchema = frameSchema.properties?.kind;
  const kinds = [
    ...enumStrings(kindSchema),
    ...(typeof kindSchema?.const === 'string' ? [kindSchema.const] : []),
    ...(typeof kindSchema?.default === 'string' ? [kindSchema.default] : []),
  ].filter((kind, index, configured) => configured.indexOf(kind) === index);
  const orderedKinds = [
    ...kinds.filter((kind) => kind === 'cell_frame'),
    ...kinds.filter((kind) => kind !== 'cell_frame'),
  ];
  for (const kind of orderedKinds) {
    const frameRef = completeFrameReferenceTransition(frameSchema, kind, {});
    if (!frameRef) continue;
    const candidate = { ...value, frame_ref: frameRef };
    if (!firstCatalogSchemaValidationMessage(schema, candidate, 'motion_pose')) {
      return candidate;
    }
  }
  return undefined;
}

export function completeMotionPoseFrameRemoveTransition(
  schema: CatalogJsonSchema,
  value: JsonObject,
): JsonObject | undefined {
  if (schema.required?.includes('frame_ref') || value.frame_ref === undefined) {
    return undefined;
  }
  const candidate = { ...value };
  delete candidate.frame_ref;
  return firstCatalogSchemaValidationMessage(schema, candidate, 'motion_pose')
    ? undefined
    : candidate;
}

export function completeOrientationTransition(
  schema: CatalogJsonSchema,
  value: JsonObject,
  representation: string,
): JsonObject | undefined {
  const orientation: JsonObject | undefined = representation === 'xyzw'
    ? { xyzw: [0, 0, 0, 1] }
    : representation === 'rpy_deg'
      ? { rpy_deg: [0, 0, 0] }
      : undefined;
  if (!orientation) return undefined;
  const candidate = { ...value, orientation };
  return firstCatalogSchemaValidationMessage(schema, candidate, 'motion_pose')
    ? undefined
    : candidate;
}

function FrameReferenceEditor({
  schema,
  value,
  disabled,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const currentKind = typeof value.kind === 'string' ? value.kind : '';
  const [draft, setDraft] = useState<Record<string, string>>(() =>
    Object.fromEntries(Object.entries(value).filter((row): row is [string, string] =>
      typeof row[1] === 'string')),
  );
  useEffect(() => {
    setDraft(Object.fromEntries(Object.entries(value).filter((row): row is [string, string] =>
      typeof row[1] === 'string')));
    reportInvalid(null);
  }, [reportInvalid, value]);

  const kind = draft.kind ?? currentKind;
  const stage = (next: Record<string, string>) => {
    setDraft(next);
    const complete = completeFrameReferenceTransition(schema, next.kind, next);
    reportInvalid(complete ? null : 'Complete every required frame role.');
    if (complete) onCommit(complete);
  };

  return (
    <div className="program-variant-editor">
      <label>
        Frame reference
        <select
          aria-label="Frame reference kind"
          value={kind}
          disabled={disabled}
          onChange={(event) => stage({ kind: event.target.value })}
        >
          {enumStrings(schema.properties?.kind).map((option) => (
            <option key={option} value={option}>{option.replaceAll('_', ' ')}</option>
          ))}
        </select>
      </label>
      {frameReferenceFields(kind).map((field) => (
        <label key={field}>
          {field.replaceAll('_', ' ')}
          <DraftTextInput
            value={draft[field] ?? ''}
            disabled={disabled}
            aria-label={`Frame reference ${field}`}
            onCommit={(text) => stage({ ...draft, kind, [field]: text })}
          />
        </label>
      ))}
      {completeFrameReferenceTransition(schema, kind, draft) === undefined && (
        <p className="program-form-hint">Complete every required frame role before this reference is saved.</p>
      )}
    </div>
  );
}

function MotionPoseEditor({
  schema,
  value,
  disabled,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const orientation = objectValue(value.orientation);
  const orientationSchema = schema.properties?.orientation;
  const usesQuaternion = Array.isArray(orientation.xyzw);
  const frameSchema = schema.properties?.frame_ref;
  const commit = (patch: JsonObject) => onCommit({ ...value, ...patch });
  const frameRequired = schema.required?.includes('frame_ref') ?? false;
  const addFrame = frameSchema && value.frame_ref === undefined
    ? completeMotionPoseFrameAddTransition(schema, value)
    : undefined;
  return (
    <div className="program-specialized-editor" data-editor="motion-pose">
      {frameSchema && value.frame_ref && (
        <>
          <FrameReferenceEditor
            schema={frameSchema}
            value={objectValue(value.frame_ref)}
            disabled={disabled}
            onCommit={(frameRef) => commit({ frame_ref: frameRef })}
          />
          {!frameRequired && (
            <button
              type="button"
              className="text-button"
              disabled={disabled}
              onClick={() => {
                const next = completeMotionPoseFrameRemoveTransition(schema, value);
                if (next) onCommit(next);
              }}
            >
              Remove frame reference
            </button>
          )}
        </>
      )}
      {frameSchema && value.frame_ref === undefined && (
        <button
          type="button"
          className="text-button"
          disabled={disabled || addFrame === undefined}
          onClick={() => {
            if (addFrame) onCommit(addFrame);
          }}
        >
          Add frame reference
        </button>
      )}
      <VectorEditor
        label="Position"
        units="m"
        labels={['x', 'y', 'z']}
        value={numericArray(value.position_m)}
        disabled={disabled}
        onCommit={(position) => commit({ position_m: position })}
      />
      <label>
        Orientation representation
        <select
          aria-label="Orientation representation"
          value={usesQuaternion ? 'xyzw' : 'rpy_deg'}
          disabled={disabled}
          onChange={(event) => {
            const next = completeOrientationTransition(schema, value, event.target.value);
            if (next) onCommit(next);
          }}
        >
          {hasProperties(orientationSchema, ['rpy_deg']) && <option value="rpy_deg">RPY degrees</option>}
          {hasProperties(orientationSchema, ['xyzw']) && <option value="xyzw">Quaternion xyzw</option>}
        </select>
      </label>
      <VectorEditor
        label="Orientation"
        units={usesQuaternion ? 'unit quaternion' : 'deg'}
        labels={usesQuaternion ? ['x', 'y', 'z', 'w'] : ['r', 'p', 'y']}
        value={numericArray(usesQuaternion ? orientation.xyzw : orientation.rpy_deg)}
        disabled={disabled}
        onCommit={(next) => commit({
          orientation: usesQuaternion ? { xyzw: next } : { rpy_deg: next },
        })}
      />
    </div>
  );
}

const MOTION_REFERENCE_FIELDS: Record<string, string> = {
  frame_role: 'frame_role_id',
  component_body_role: 'collision_body_role_id',
  scene_resource: 'scene_resource_id',
  runtime_object: 'runtime_object_id',
};

function defaultMotionPose(schema: CatalogJsonSchema | undefined): JsonObject {
  const result: JsonObject = {
    position_m: [0, 0, 0],
    orientation: { rpy_deg: [0, 0, 0] },
  };
  if (schema?.required?.includes('frame_ref')) {
    return completeMotionPoseFrameAddTransition(schema, result) ?? result;
  }
  return result;
}

/**
 * Normalize a motion-reference variant without retaining forbidden fields.
 * Undefined stages id-based variants until their one required id is present.
 */
export function completeMotionReferenceTransition(
  schema: CatalogJsonSchema,
  type: string,
  fieldValue?: string,
): JsonObject | undefined {
  let candidate: JsonObject;
  if (type === 'pose') {
    candidate = { type, pose: defaultMotionPose(schema.properties?.pose) };
  } else {
    const field = MOTION_REFERENCE_FIELDS[type];
    if (!field || !fieldValue?.trim()) return undefined;
    candidate = { type, [field]: fieldValue.trim() };
  }
  return firstCatalogSchemaValidationMessage(schema, candidate, 'motion_reference')
    ? undefined
    : candidate;
}

function MotionReferenceEditor({
  schema,
  value,
  disabled,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const currentType = typeof value.type === 'string' ? value.type : '';
  const [draftType, setDraftType] = useState(currentType);
  const [draftField, setDraftField] = useState('');
  useEffect(() => {
    setDraftType(currentType);
    const field = MOTION_REFERENCE_FIELDS[currentType];
    setDraftField(field && typeof value[field] === 'string' ? value[field] as string : '');
    reportInvalid(null);
  }, [currentType, reportInvalid, value]);
  const field = MOTION_REFERENCE_FIELDS[draftType];
  return (
    <div className="program-variant-editor" data-editor="motion-reference">
      <p className="program-form-hint">
        Historical frame/object/robot_link id variants are unavailable in the current contract; only these catalog variants can be authored.
      </p>
      <label>
        Reference type
        <select
          aria-label="Motion reference type"
          value={draftType}
          disabled={disabled}
          onChange={(event) => {
            const nextType = event.target.value;
            setDraftType(nextType);
            setDraftField('');
            const complete = completeMotionReferenceTransition(schema, nextType);
            reportInvalid(complete ? null : 'Enter the required reference for this variant.');
            if (complete) onCommit(complete);
          }}
        >
          {enumStrings(schema.properties?.type).map((option) => (
            <option key={option} value={option}>{option.replaceAll('_', ' ')}</option>
          ))}
        </select>
      </label>
      {field && (
        <label>
          {field.replaceAll('_', ' ')}
          <DraftTextInput
            value={draftField}
            disabled={disabled}
            aria-label={`Motion reference ${field}`}
            onCommit={(text) => {
              setDraftField(text);
              const complete = completeMotionReferenceTransition(schema, draftType, text);
              reportInvalid(complete ? null : 'Enter the required reference for this variant.');
              if (complete) onCommit(complete);
            }}
          />
        </label>
      )}
      {draftType === 'pose' && (
        <MotionPoseEditor
          schema={schema.properties?.pose ?? {}}
          value={draftType === currentType && value.pose
            ? objectValue(value.pose)
            : defaultMotionPose(schema.properties?.pose)}
          disabled={disabled}
          onCommit={(pose) => onCommit({ type: 'pose', pose })}
        />
      )}
      {field && !draftField.trim() && (
        <p className="program-form-hint">Enter the required reference before this variant is saved.</p>
      )}
    </div>
  );
}

export interface JointTargetEditorRow {
  jointName: string;
  positionText: string;
  selected: boolean;
  available: boolean;
}

export function jointTargetEditorRows(
  availableJointNames: string[],
  value: JsonValue[],
): JointTargetEditorRow[] {
  const available = new Set(availableJointNames);
  const selected = new Set<string>();
  const rows = value.map((item) => {
    const target = objectValue(item);
    const jointName = String(target.joint_name);
    selected.add(jointName);
    return {
      jointName,
      positionText: String(target.position_rad),
      selected: true,
      available: available.has(jointName),
    };
  });
  for (const jointName of availableJointNames) {
    if (selected.has(jointName)) continue;
    rows.push({ jointName, positionText: '', selected: false, available: true });
  }
  return rows;
}

export function toggleJointTargetRow(
  rows: JointTargetEditorRow[],
  jointName: string,
  selected: boolean,
  availableJointNames: string[],
): JointTargetEditorRow[] {
  const changed = rows.find((row) => row.jointName === jointName);
  if (changed === undefined) return rows;
  const remaining = rows.filter((row) => row.jointName !== jointName);
  const next = { ...changed, selected, positionText: '' };
  if (selected) {
    const firstUnselected = remaining.findIndex((row) => !row.selected);
    if (firstUnselected < 0) return [...remaining, next];
    return [
      ...remaining.slice(0, firstUnselected),
      next,
      ...remaining.slice(firstUnselected),
    ];
  }
  const availableOrder = new Map(
    availableJointNames.map((name, index) => [name, index]),
  );
  const selectedRows = remaining.filter((row) => row.selected);
  const unselectedRows = [
    ...remaining.filter((row) => !row.selected),
    next,
  ].sort((left, right) => (
    (availableOrder.get(left.jointName) ?? Number.POSITIVE_INFINITY)
    - (availableOrder.get(right.jointName) ?? Number.POSITIVE_INFINITY)
  ));
  return [...selectedRows, ...unselectedRows];
}

export function jointTargetTransitionMessage(
  schema: CatalogJsonSchema,
  rows: JointTargetEditorRow[],
): string | null {
  const selected = rows.filter((row) => row.selected);
  if (selected.length < (schema.minItems ?? 0)) {
    return `Select at least ${schema.minItems ?? 0} joint${schema.minItems === 1 ? '' : 's'}.`;
  }
  if (selected.length > (schema.maxItems ?? Number.POSITIVE_INFINITY)) {
    return `Select no more than ${schema.maxItems} joints.`;
  }
  for (const row of selected) {
    if (!row.positionText.trim()) return `Enter a target position for ${row.jointName}.`;
    if (!Number.isFinite(Number(row.positionText))) {
      return `${row.jointName} must have a finite target position.`;
    }
  }
  return firstCatalogSchemaValidationMessage(
    schema,
    selected.map((row) => ({
      joint_name: row.jointName,
      position_rad: Number(row.positionText),
    })),
    'joint_targets',
  );
}

export function completeJointTargetTransition(
  schema: CatalogJsonSchema,
  rows: JointTargetEditorRow[],
): JsonValue[] | undefined {
  if (jointTargetTransitionMessage(schema, rows) !== null) return undefined;
  return rows.filter((row) => row.selected).map((row) => ({
    joint_name: row.jointName,
    position_rad: Number(row.positionText),
  }));
}

function JointTargetPositionInput({
  jointName,
  value,
  disabled,
  onCommit,
}: {
  jointName: string;
  value: string;
  disabled: boolean;
  onCommit: (value: string) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  return (
    <DraftTextInput
      type="number"
      step="any"
      value={value}
      disabled={disabled}
      aria-label={`${jointName} target position in radians`}
      placeholder={disabled ? '' : 'Required'}
      validate={(text) => text.trim() !== '' && Number.isFinite(Number(text))
        ? null
        : `${jointName} must have a finite target position.`}
      onInvalidChange={reportInvalid}
      onCommit={onCommit}
    />
  );
}

function JointTargetListEditor({
  schema,
  value,
  disabled,
  sourceJointInventory,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue[];
  disabled: boolean;
  sourceJointInventory?: ProgramSourceJointInventory | null;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const availableJointNames = useMemo(
    () => sourceJointInventory?.jointNames ?? [],
    [sourceJointInventory],
  );
  const inventoryAvailable = sourceJointInventory?.unavailableReason === null;
  const [rows, setRows] = useState<JointTargetEditorRow[]>(() =>
    jointTargetEditorRows(availableJointNames, value));
  useEffect(() => {
    setRows(jointTargetEditorRows(availableJointNames, value));
    reportInvalid(null);
  }, [availableJointNames, reportInvalid, value]);

  const stage = (nextRows: JointTargetEditorRow[]) => {
    setRows(nextRows);
    const message = jointTargetTransitionMessage(schema, nextRows);
    reportInvalid(message);
    if (message !== null) return;
    const next = completeJointTargetTransition(schema, nextRows);
    if (next !== undefined && JSON.stringify(next) !== JSON.stringify(value)) onCommit(next);
  };
  const selectedCount = rows.filter((row) => row.selected).length;
  const maximumSelected = selectedCount >= (schema.maxItems ?? Number.POSITIVE_INFINITY);

  return (
    <fieldset className="program-joint-target-list" data-editor="joint-target-list">
      <legend>Joint goal</legend>
      <p className="program-form-hint">
        Select the joints to command and enter each target position in radians.
        Selected joints retain their authored order; a newly selected joint is added last.
      </p>
      {sourceJointInventory?.unavailableReason && (
        <p className="program-joint-inventory-unavailable" role="status">
          {sourceJointInventory.unavailableReason}
          {' '}Existing values remain visible and removable, but new joint selection is unavailable.
        </p>
      )}
      {rows.map((row) => {
        const unavailableCurrent = inventoryAvailable && !row.available;
        const selectionDisabled = disabled
          || (!row.selected && (!inventoryAvailable || maximumSelected));
        return (
          <div
            className={`program-joint-target-row ${row.selected ? 'selected' : 'unselected'}${unavailableCurrent ? ' unavailable' : ''}`}
            key={row.jointName}
          >
            <label className="program-joint-target-choice">
              <input
                type="checkbox"
                checked={row.selected}
                disabled={selectionDisabled}
                aria-label={`Include ${row.jointName} in joint goal`}
                title={!row.selected && maximumSelected
                  ? `This goal accepts at most ${schema.maxItems} joints.`
                  : undefined}
                onChange={(event) => {
                  stage(toggleJointTargetRow(
                    rows,
                    row.jointName,
                    event.target.checked,
                    availableJointNames,
                  ));
                }}
              />
              <code>{row.jointName}</code>
              {unavailableCurrent && <span>not commandable by this target</span>}
            </label>
            <label className="program-joint-target-position">
              <span>Position (rad)</span>
              <JointTargetPositionInput
                jointName={row.jointName}
                value={row.selected ? row.positionText : ''}
                disabled={disabled || !row.selected}
                onCommit={(positionText) => stage(rows.map((candidate) =>
                  candidate.jointName === row.jointName
                    ? { ...candidate, positionText }
                    : candidate))}
              />
            </label>
          </div>
        );
      })}
      {rows.length === 0 && (
        <p className="program-form-hint">No commandable source joints are available.</p>
      )}
    </fieldset>
  );
}

function CartesianPathEditor({
  schema,
  value,
  disabled,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue[];
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  return (
    <div className="program-specialized-editor" data-editor="cartesian-path">
      {value.map((waypoint, index) => (
        <fieldset className="program-waypoint" key={index}>
          <legend>Waypoint {index + 1}</legend>
          <MotionPoseEditor
            schema={schema.items ?? {}}
            value={objectValue(waypoint)}
            disabled={disabled}
            onCommit={(nextWaypoint) => {
              const next = [...value];
              next[index] = nextWaypoint;
              onCommit(next);
            }}
          />
          <button
            type="button"
            className="text-button"
            disabled={disabled || value.length <= (schema.minItems ?? 0)}
            onClick={() => onCommit(value.filter((_, rowIndex) => rowIndex !== index))}
          >
            Remove waypoint
          </button>
        </fieldset>
      ))}
      <button
        type="button"
        className="text-button"
        disabled={disabled || value.length >= (schema.maxItems ?? Number.POSITIVE_INFINITY)}
        onClick={() => onCommit([...value, defaultMotionPose(schema.items)])}
      >
        Add waypoint
      </button>
    </div>
  );
}

function CoordinateEditor({
  schema,
  value,
  disabled,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const kinds = enumStrings(schema.properties?.kind);
  const kind = typeof value.kind === 'string' ? value.kind : kinds[0] ?? '';
  const valueField = kind === 'angular_rad'
    ? 'value_rad'
    : kind === 'linear_m'
      ? 'value_m'
      : kind === 'angular_newton_meter'
        ? 'value_nm'
        : 'value_n';
  const units = valueField.replace('value_', '');
  return (
    <div className="program-specialized-editor" data-editor="gripper-coordinate">
      <p className="program-form-hint">
        Program authoring stores only the catalog coordinate kind and value; live gripper range, presets, and direct control are unavailable here.
      </p>
      <label>
        Coordinate kind
        <select
          aria-label="Coordinate kind"
          value={kind}
          disabled={disabled}
          onChange={(event) => {
            const next = completeCoordinateKindTransition(schema, event.target.value);
            if (next) onCommit(next);
          }}
        >
          {kinds.map((option) => (
            <option key={option} value={option}>{option.replaceAll('_', ' ')}</option>
          ))}
        </select>
      </label>
      <NumberField
        label={`Value (${units})`}
        value={typeof value[valueField] === 'number' ? value[valueField] as number : 0}
        disabled={disabled}
        onCommit={(next) => onCommit({ kind, [valueField]: next })}
      />
    </div>
  );
}

export function completeCoordinateKindTransition(
  schema: CatalogJsonSchema,
  kind: string,
): JsonObject | undefined {
  const valueField = kind === 'angular_rad'
    ? 'value_rad'
    : kind === 'linear_m'
      ? 'value_m'
      : kind === 'angular_newton_meter'
        ? 'value_nm'
        : kind === 'linear_newton'
          ? 'value_n'
          : undefined;
  if (!valueField) return undefined;
  const candidate: JsonObject = { kind, [valueField]: 0 };
  return firstCatalogSchemaValidationMessage(schema, candidate, 'coordinate')
    ? undefined
    : candidate;
}

function specializedKind(schema: CatalogJsonSchema): string | null {
  if (
    schema.type === 'array'
    && hasProperties(schema.items, ['joint_name', 'position_rad'])
  ) return 'joint-target-list';
  if (
    schema.type === 'array'
    && hasProperties(schema.items, ['position_m', 'orientation'])
  ) return 'cartesian-path';
  if (
    schema.type === 'object'
    && hasProperties(schema, ['position_m', 'orientation'])
  ) return 'motion-pose';
  const variants = enumStrings(schema.properties?.type);
  if (
    schema.type === 'object'
    && variants.includes('frame_role')
    && variants.includes('pose')
  ) return 'motion-reference';
  const coordinateKinds = enumStrings(schema.properties?.kind);
  if (
    schema.type === 'object'
    && coordinateKinds.some((kind) => kind === 'angular_rad' || kind === 'angular_newton_meter')
  ) return 'coordinate';
  return null;
}

export function supportsProgramSpecializedEditor(schema: CatalogJsonSchema): boolean {
  return specializedKind(schema) !== null;
}

export function ProgramSpecializedEditor({
  schema,
  value,
  disabled,
  sourceJointInventory,
  onCommit,
  onInvalidChange,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue;
  disabled: boolean;
  sourceJointInventory?: ProgramSourceJointInventory | null;
  onCommit: (value: JsonValue) => void;
  onInvalidChange?: (message: string | null) => void;
}) {
  const [invalid, setInvalid] = useState<Map<symbol, string>>(() => new Map());
  const reportInvalid = useCallback<SpecializedInvalidReporter>((token, message) => {
    setInvalid((current) => {
      const next = new Map(current);
      if (message === null) next.delete(token);
      else next.set(token, message);
      return next;
    });
  }, []);
  const firstInvalid = invalid.values().next().value ?? null;
  const invalidChange = useRef(onInvalidChange);
  useEffect(() => {
    invalidChange.current = onInvalidChange;
  }, [onInvalidChange]);
  useEffect(() => {
    invalidChange.current?.(firstInvalid);
  }, [firstInvalid]);
  useEffect(() => () => invalidChange.current?.(null), []);
  const kind = specializedKind(schema);
  let content: ReactNode = null;
  if (kind === 'joint-target-list') {
    content = (
      <JointTargetListEditor
        schema={schema}
        value={value as JsonValue[]}
        disabled={disabled}
        sourceJointInventory={sourceJointInventory}
        onCommit={onCommit}
      />
    );
  } else if (kind === 'cartesian-path') {
    content = <CartesianPathEditor schema={schema} value={value as JsonValue[]} disabled={disabled} onCommit={onCommit} />;
  } else if (kind === 'motion-pose') {
    content = <MotionPoseEditor schema={schema} value={objectValue(value)} disabled={disabled} onCommit={onCommit} />;
  } else if (kind === 'motion-reference') {
    content = <MotionReferenceEditor schema={schema} value={objectValue(value)} disabled={disabled} onCommit={onCommit} />;
  } else if (kind === 'coordinate') {
    content = <CoordinateEditor schema={schema} value={objectValue(value)} disabled={disabled} onCommit={onCommit} />;
  }
  return (
    <SpecializedInvalidContext.Provider value={reportInvalid}>
      {content}
    </SpecializedInvalidContext.Provider>
  );
}

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
import { rpyDegToXyzw, xyzwToRpyDeg } from '../../utils/orientation';
import {
  frameKey,
  offeredFrameChoices,
  poseFrameOf,
  type FrameOffer,
  type FrameScope,
  type ProgramPoseFrame,
} from '../../utils/programFrames';
import type { ProgramSourceJointInventory } from '../../utils/programSourceJoints';
import { listDeleteMove, type ListMove } from '../../utils/valuePaths';
import { DraftTextInput } from '../common/DraftInputs';
import { FrameSelector } from './FrameSelector';
import { SourceableLeaf, SourceableValue, useValueSources } from './ValueSourceField';
import type { ManualFillPreviewValues } from '../../utils/manualFillPreview';

/** Commit one edited value; list edits name how their items moved, so sources move with them. */
export type ValueCommit = (value: JsonValue, moves?: readonly ListMove[]) => void;

const NUMBER_SCHEMA: CatalogJsonSchema = { type: 'number' };

type JsonObject = Record<string, JsonValue>;
type SpecializedInvalidReporter = (token: symbol, message: string | null) => void;
const SpecializedInvalidContext = createContext<SpecializedInvalidReporter>(() => undefined);
// The Program's declared frames, for every frame field of one value.
const FrameScopeContext = createContext<FrameScope | null>(null);
export const ProgramManualFillPreviewContext = createContext<ManualFillPreviewValues>(new Map());

export function ProgramManualFillPreviewProvider({
  values,
  children,
}: {
  values: ManualFillPreviewValues;
  children: ReactNode;
}) {
  return (
    <ProgramManualFillPreviewContext.Provider value={values}>
      {children}
    </ProgramManualFillPreviewContext.Provider>
  );
}

function useSpecializedInvalidReporter() {
  const report = useContext(SpecializedInvalidContext);
  const token = useRef(Symbol('specialized-field'));
  const message = useRef<string | null>(null);
  useEffect(() => {
    const fieldToken = token.current;
    report(fieldToken, message.current);
    return () => report(fieldToken, null);
  }, [report]);
  return useCallback((next: string | null) => {
    message.current = next;
    report(token.current, next);
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

/**
 * A control whose fill preview shows the new value inside the control itself,
 * over the current one, so a preview never adds rows or resizes anything
 * (hovering a fill button must not move the button away from the pointer).
 */
function FillTarget({ previewValue, children }: { previewValue: string | undefined; children: ReactNode }) {
  return (
    <span className="program-fill-target">
      {children}
      {previewValue !== undefined && (
        <span className="program-fill-overlay" aria-hidden="true">→ {previewValue}</span>
      )}
    </span>
  );
}

function NumberField({
  label,
  visibleLabel = label,
  value,
  disabled,
  path,
  previewPath,
  previewOverride,
  schema = NUMBER_SCHEMA,
  onCommit,
}: {
  label: string;
  /** Short visible axis label; the accessible and validation label stays complete. */
  visibleLabel?: string;
  value: number;
  disabled: boolean;
  /** The value's path when it may come from a source. */
  path?: string;
  previewPath?: string;
  /** A fill preview already expressed in this field's form (e.g. a converted orientation). */
  previewOverride?: string;
  schema?: CatalogJsonSchema;
  onCommit: (value: number) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const previewValues = useContext(ProgramManualFillPreviewContext);
  const previewValue = previewOverride
    ?? (previewPath === undefined ? undefined : previewValues.get(previewPath));
  const input = (
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
  );
  return (
    <label className={`program-specialized-number${previewValue === undefined ? '' : ' program-manual-fill-changed'}`}>
      <span>{visibleLabel}</span>
      <FillTarget previewValue={previewValue}>
        {path === undefined
          ? input
          : <SourceableLeaf path={path} schema={schema} label={label}>{input}</SourceableLeaf>}
      </FillTarget>
    </label>
  );
}

function VectorEditor({
  label,
  units,
  labels,
  value,
  disabled,
  path,
  previewPath,
  previewOverrides,
  onCommit,
}: {
  label: string;
  units: string;
  labels: string[];
  value: number[];
  disabled: boolean;
  /** The vector's path when each number may come from a source. */
  path?: string;
  previewPath?: string;
  /** Per-axis fill previews already in this editor's form; they replace `previewPath`. */
  previewOverrides?: string[];
  onCommit: (value: JsonValue) => void;
}) {
  return (
    <fieldset className="program-vector-editor">
      <legend>{label} ({units})</legend>
      <div className={labels.length === 4 ? 'program-vector-fields four' : 'program-vector-fields'}>
        {labels.map((axis, index) => (
          <NumberField
            key={axis}
            label={`${label} ${axis}`}
            visibleLabel={axis.toUpperCase()}
            value={value[index] ?? 0}
            disabled={disabled}
            path={path === undefined ? undefined : `${path}[${index}]`}
            previewPath={previewPath === undefined || previewOverrides !== undefined
              ? undefined : `${previewPath}[${index}]`}
            previewOverride={previewOverrides?.[index]}
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
  if (kind === 'target_frame') return ['component_id', 'frame_id'];
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
    if (kind !== 'target_frame') candidate[field] = value;
  }
  if (kind === 'target_frame') candidate.frame = { component_id: fields.component_id.trim(), frame_id: fields.frame_id.trim() };
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

/**
 * Convert a pose orientation to the requested stored form. Undefined means the
 * representation is unknown or the current orientation cannot be converted.
 */
function convertOrientation(orientation: JsonObject, representation: string): JsonObject | undefined {
  if (representation === 'xyzw') {
    if (Array.isArray(orientation.xyzw)) return { xyzw: orientation.xyzw };
    const xyzw = rpyDegToXyzw(numericArray(orientation.rpy_deg));
    return xyzw && { xyzw };
  }
  if (representation === 'rpy_deg') {
    if (Array.isArray(orientation.rpy_deg)) return { rpy_deg: orientation.rpy_deg };
    const rpyDeg = xyzwToRpyDeg(numericArray(orientation.xyzw));
    return rpyDeg && { rpy_deg: rpyDeg };
  }
  return undefined;
}

export function completeOrientationTransition(
  schema: CatalogJsonSchema,
  value: JsonObject,
  representation: string,
): JsonObject | undefined {
  const orientation = convertOrientation(objectValue(value.orientation), representation);
  if (!orientation) return undefined;
  const candidate = { ...value, orientation };
  return firstCatalogSchemaValidationMessage(schema, candidate, 'motion_pose')
    ? undefined
    : candidate;
}

function formatReadOnlyOrientation(orientation: JsonObject): string {
  const quaternion = Array.isArray(orientation.xyzw);
  const axes = quaternion ? ['x', 'y', 'z', 'w'] : ['r', 'p', 'y'];
  const values = numericArray(quaternion ? orientation.xyzw : orientation.rpy_deg);
  const decimals = quaternion ? 6 : 4;
  const text = axes.map((axis, index) => `${axis} ${Number((values[index] ?? 0).toFixed(decimals))}`);
  return `${quaternion ? 'Quaternion xyzw' : 'RPY degrees'} (read-only): ${text.join(', ')}`;
}

function FrameReferenceEditor({
  schema,
  value,
  disabled,
  path,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const previewValues = useContext(ProgramManualFillPreviewContext);
  const previewValue = path === undefined ? undefined : previewValues.get(path);
  const currentKind = typeof value.kind === 'string' ? value.kind : '';
  const [draft, setDraft] = useState<Record<string, string>>(() =>
    Object.fromEntries(Object.entries(value).filter((row): row is [string, string] =>
      typeof row[1] === 'string')),
  );
  const resetValue = useRef<JsonObject | undefined>(undefined);
  useEffect(() => {
    if (resetValue.current === value) return;
    resetValue.current = value;
    setDraft(Object.fromEntries(Object.entries(value).filter((row): row is [string, string] =>
      typeof row[1] === 'string')));
    reportInvalid(null);
  }, [reportInvalid, value]);

  const kind = draft.kind ?? currentKind;
  const stage = (next: Record<string, string>) => {
    setDraft(next);
    const complete = completeFrameReferenceTransition(schema, next.kind, next);
    reportInvalid(complete ? null : 'Complete every required frame reference field.');
    if (complete) onCommit(complete);
  };

  const scope = useContext(FrameScopeContext);
  if (scope !== null) {
    const kinds = enumStrings(schema.properties?.kind);
    const offer: FrameOffer = {
      otherRobots: kinds.includes('target_frame'),
      cell: kinds.includes('cell_frame'),
      scene: kinds.includes('scene_resource'),
    };
    return (
      <ScopedFrameReference
        scope={scope}
        offer={offer}
        value={value}
        disabled={disabled}
        previewValue={previewValue}
        onCommit={onCommit}
      />
    );
  }
  return (
    <div className="program-variant-editor">
      <label className={previewValue === undefined ? '' : 'program-manual-fill-changed'}>
        <span>Frame reference</span>
        <FillTarget previewValue={previewValue}>
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
        </FillTarget>
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
        <p className="program-form-hint">Complete every required frame reference field before this reference is saved.</p>
      )}
    </div>
  );
}

/** One pose frame chosen from the Program's declared frames. */
function ScopedFrameReference({
  scope,
  offer,
  value,
  disabled,
  previewValue,
  onCommit,
}: {
  scope: FrameScope;
  offer: FrameOffer;
  value: JsonObject;
  disabled: boolean;
  previewValue?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const frame = poseFrameOf(value);
  const usable = frame !== null
    && offeredFrameChoices(scope, offer).some((choice) => choice.key === frameKey(frame));
  useEffect(() => {
    reportInvalid(usable ? null : 'Choose a frame this value may use.');
  }, [reportInvalid, usable]);
  return (
    <div className="program-variant-editor">
      <label className={previewValue === undefined ? '' : 'program-manual-fill-changed'}>
        <span>Frame reference</span>
        <FillTarget previewValue={previewValue}>
          <FrameSelector
            scope={scope}
            value={frame}
            offer={offer}
            disabled={disabled}
            ariaLabel="Frame reference"
            onChange={(next) => onCommit(next as unknown as JsonValue)}
          />
        </FillTarget>
      </label>
    </div>
  );
}

/**
 * A fill that stores the orientation in the other form (the robot pose and the
 * move preview store a quaternion) is previewed in the form this editor shows,
 * with the stored-form change on the representation selector.
 */
function useOrientationFillPreview(
  path: string | undefined,
  usesQuaternion: boolean,
): { formLabel?: string; values?: string[] } {
  const previewValues = useContext(ProgramManualFillPreviewContext);
  if (path === undefined) return {};
  const otherForm = usesQuaternion ? 'rpy_deg' : 'xyzw';
  const otherAxes = usesQuaternion ? 3 : 4;
  const incoming = Array.from({ length: otherAxes },
    (_, index) => previewValues.get(`${path}.orientation.${otherForm}[${index}]`));
  if (incoming.every((item) => item === undefined)) return {};
  const numbers = incoming.map((item) => Number(item));
  const converted = numbers.every(Number.isFinite)
    ? convertOrientation({ [otherForm]: numbers }, usesQuaternion ? 'xyzw' : 'rpy_deg')
    : undefined;
  const shown = numericArray(converted?.[usesQuaternion ? 'xyzw' : 'rpy_deg']);
  const decimals = usesQuaternion ? 4 : 2;
  return {
    formLabel: usesQuaternion ? 'RPY degrees' : 'Quaternion xyzw',
    values: shown.length === 0 ? undefined : shown.map((item) => String(Number(item.toFixed(decimals)))),
  };
}

function MotionPoseEditor({
  schema,
  value,
  disabled,
  path,
  relativeGoal = false,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  /** A relative goal is stated from target_reference, so it takes no frame of its own. */
  relativeGoal?: boolean;
  onCommit: (value: JsonValue) => void;
}) {
  const sources = useValueSources();
  // Switching the stored form would drop per-angle formulas and bindings.
  const orientationSourced = path !== undefined
    && (sources?.hasSourcesWithin(`${path}.orientation`) ?? false);
  const orientation = objectValue(value.orientation);
  const orientationSchema = schema.properties?.orientation;
  const usesQuaternion = Array.isArray(orientation.xyzw);
  const counterpart = convertOrientation(orientation, usesQuaternion ? 'rpy_deg' : 'xyzw');
  const orientationPreview = useOrientationFillPreview(path, usesQuaternion);
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
            path={path === undefined ? undefined : `${path}.frame_ref`}
            onCommit={(frameRef) => commit({ frame_ref: frameRef })}
          />
          {relativeGoal && (
            <p className="program-form-hint">
              A relative move is stated from target_reference, so this frame must be the robot's base frame. Remove it, or switch target_mode to absolute.
            </p>
          )}
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
      {frameSchema && value.frame_ref === undefined && !relativeGoal && (
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
        path={path === undefined ? undefined : `${path}.position_m`}
        previewPath={path === undefined ? undefined : `${path}.position_m`}
        onCommit={(position) => commit({ position_m: position })}
      />
      <label className={orientationPreview.formLabel === undefined ? undefined : 'program-manual-fill-changed'}>
        Orientation representation
        <FillTarget previewValue={orientationPreview.formLabel}>
          <select
            aria-label="Orientation representation"
            value={usesQuaternion ? 'xyzw' : 'rpy_deg'}
            disabled={disabled || orientationSourced}
            title={orientationSourced
              ? 'Type roll, pitch and yaw again before changing the stored form.'
              : undefined}
            onChange={(event) => {
              const next = completeOrientationTransition(schema, value, event.target.value);
              if (next) onCommit(next);
            }}
          >
            {hasProperties(orientationSchema, ['rpy_deg']) && <option value="rpy_deg">RPY degrees</option>}
            {hasProperties(orientationSchema, ['xyzw']) && <option value="xyzw">Quaternion xyzw</option>}
          </select>
        </FillTarget>
      </label>
      <VectorEditor
        label="Orientation"
        units={usesQuaternion ? 'unit quaternion' : 'deg'}
        labels={usesQuaternion ? ['x', 'y', 'z', 'w'] : ['r', 'p', 'y']}
        value={numericArray(usesQuaternion ? orientation.xyzw : orientation.rpy_deg)}
        disabled={disabled}
        // A quaternion is typed or bound only with its whole pose.
        path={path === undefined || usesQuaternion ? undefined : `${path}.orientation.rpy_deg`}
        previewPath={path === undefined ? undefined : `${path}.orientation.${usesQuaternion ? 'xyzw' : 'rpy_deg'}`}
        previewOverrides={orientationPreview.values}
        onCommit={(next) => commit({
          orientation: usesQuaternion ? { xyzw: next } : { rpy_deg: next },
        })}
      />
      {counterpart && <p className="program-form-hint">{formatReadOnlyOrientation(counterpart)}</p>}
    </div>
  );
}

const MOTION_REFERENCE_FIELDS: Record<string, string> = {
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

const FRAME_REFERENCE_TYPES = ['component_frame', 'cell_frame', 'scene_resource'];

/** The pose frame a motion reference names, or null for a non-frame reference. */
export function motionReferenceFrame(value: JsonObject): ProgramPoseFrame | null {
  if (value.type === 'cell_frame') return { kind: 'cell_frame' };
  if (value.type === 'scene_resource' && typeof value.scene_resource_id === 'string') {
    return { kind: 'scene_resource', scene_resource_id: value.scene_resource_id };
  }
  if (value.type !== 'component_frame') return null;
  return poseFrameOf({ kind: 'target_frame', frame: value.frame });
}

/**
 * The motion reference for one chosen frame; a component frame always
 * names its component (direct reference).
 */
export function motionReferenceForFrame(frame: ProgramPoseFrame): JsonObject {
  if (frame.kind === 'cell_frame') return { type: 'cell_frame' };
  if (frame.kind === 'scene_resource') {
    return { type: 'scene_resource', scene_resource_id: frame.scene_resource_id };
  }
  return { type: 'component_frame', frame: { ...frame.frame } };
}

function ScopedMotionReferenceEditor({
  schema,
  scope,
  value,
  disabled,
  path,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  scope: FrameScope;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const types = enumStrings(schema.properties?.type);
  const frameTypes = types.filter((type) => FRAME_REFERENCE_TYPES.includes(type));
  const currentType = typeof value.type === 'string' ? value.type : '';
  const choiceOf = (type: string) => FRAME_REFERENCE_TYPES.includes(type) ? 'frame' : type;
  const [draftChoice, setDraftChoice] = useState(choiceOf(currentType));
  const [draftField, setDraftField] = useState('');
  const resetValue = useRef<JsonObject | undefined>(undefined);
  useEffect(() => {
    if (resetValue.current === value) return;
    resetValue.current = value;
    setDraftChoice(choiceOf(currentType));
    const field = MOTION_REFERENCE_FIELDS[currentType];
    setDraftField(field && typeof value[field] === 'string' ? value[field] as string : '');
    reportInvalid(null);
  }, [currentType, reportInvalid, value]);
  const offer: FrameOffer = {
    otherRobots: frameTypes.includes('component_frame'),
    cell: frameTypes.includes('cell_frame'),
    scene: frameTypes.includes('scene_resource'),
  };
  const frame = choiceOf(currentType) === 'frame' ? motionReferenceFrame(value) : null;
  const frameUsable = frame !== null
    && offeredFrameChoices(scope, offer).some((choice) => choice.key === frameKey(frame));
  const commitCandidate = (candidate: JsonObject | undefined) => {
    const valid = candidate !== undefined
      && firstCatalogSchemaValidationMessage(schema, candidate, 'motion_reference') === null;
    reportInvalid(valid ? null : 'Enter the required reference for this variant.');
    if (valid) onCommit(candidate);
  };
  useEffect(() => {
    if (draftChoice === 'frame' && frame !== null && !frameUsable) {
      reportInvalid('Choose a frame this reference may use.');
    }
  }, [draftChoice, frame, frameUsable, reportInvalid]);
  const field = MOTION_REFERENCE_FIELDS[draftChoice];
  return (
    <div className="program-variant-editor" data-editor="motion-reference">
      <p className="program-form-hint">
        Historical frame/object/robot_link id variants are unavailable in the current contract; only these catalog variants can be authored.
      </p>
      <label>
        Reference type
        <select
          aria-label="Motion reference type"
          value={draftChoice}
          disabled={disabled}
          onChange={(event) => {
            const next = event.target.value;
            setDraftChoice(next);
            setDraftField('');
            if (next === 'frame') {
              reportInvalid('Choose the frame this reference is measured from.');
            } else {
              commitCandidate(completeMotionReferenceTransition(schema, next));
            }
          }}
        >
          {frameTypes.length > 0 && <option value="frame">frame</option>}
          {types.filter((type) => !frameTypes.includes(type)).map((option) => (
            <option key={option} value={option}>{option.replaceAll('_', ' ')}</option>
          ))}
        </select>
      </label>
      {draftChoice === 'frame' && (
        <label>
          Frame
          <FrameSelector
            scope={scope}
            value={draftChoice === choiceOf(currentType) ? frame : null}
            offer={offer}
            disabled={disabled}
            ariaLabel="Motion reference frame"
            onChange={(next) => commitCandidate(motionReferenceForFrame(next))}
          />
        </label>
      )}
      {draftChoice === 'component_body' && (() => {
        const choices = scope.bodies ?? [];
        const bodyValue = value.type === 'component_body' && value.body !== null
          && typeof value.body === 'object' && !Array.isArray(value.body) ? value.body : null;
        const selected = bodyValue && typeof bodyValue.component_id === 'string'
          && typeof bodyValue.collision_body_id === 'string'
          ? JSON.stringify([bodyValue.component_id, bodyValue.collision_body_id]) : '';
        const key = (body: { component_id: string; collision_body_id: string }) => (
          JSON.stringify([body.component_id, body.collision_body_id])
        );
        const known = choices.some((body) => key(body) === selected);
        return <label>
          Component collision body
          <select
            aria-label="Motion reference component collision body"
            aria-invalid={selected !== '' && !known || undefined}
            value={selected}
            disabled={disabled}
            onChange={(event) => {
              const body = choices.find((row) => key(row) === event.target.value);
              if (body) commitCandidate({ type: 'component_body', body: { ...body } });
            }}
          >
            {selected === '' && <option value="" disabled>Select a body</option>}
            {selected !== '' && !known && <option value={selected} disabled>Current body is unavailable in the captured model</option>}
            {choices.map((body) => <option key={key(body)} value={key(body)}>
              {body.component_id} · {body.collision_body_id}
            </option>)}
          </select>
        </label>;
      })()}
      {field && draftChoice !== 'frame' && (
        <label>
          {field.replaceAll('_', ' ')}
          <DraftTextInput
            value={draftField}
            disabled={disabled}
            aria-label={`Motion reference ${field}`}
            onCommit={(text) => {
              setDraftField(text);
              commitCandidate(completeMotionReferenceTransition(schema, draftChoice, text));
            }}
          />
        </label>
      )}
      {draftChoice === 'pose' && (() => {
        const poseSchema = schema.properties?.pose ?? {};
        const posePath = path === undefined ? undefined : `${path}.pose`;
        const editor = (
          <MotionPoseEditor
            schema={poseSchema}
            value={draftChoice === currentType && value.pose
              ? objectValue(value.pose)
              : defaultMotionPose(schema.properties?.pose)}
            disabled={disabled}
            path={posePath}
            onCommit={(pose) => onCommit({ type: 'pose', pose })}
          />
        );
        return posePath === undefined
          ? editor
          : <SourceableValue path={posePath} schema={poseSchema}>{editor}</SourceableValue>;
      })()}
    </div>
  );
}

function MotionReferenceEditor({
  schema,
  value,
  disabled,
  path,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const scope = useContext(FrameScopeContext);
  if (scope !== null) {
    return (
      <ScopedMotionReferenceEditor
        schema={schema}
        scope={scope}
        value={value}
        disabled={disabled}
        path={path}
        onCommit={onCommit}
      />
    );
  }
  return (
    <UnscopedMotionReferenceEditor
      schema={schema}
      value={value}
      disabled={disabled}
      path={path}
      onCommit={onCommit}
    />
  );
}

function UnscopedMotionReferenceEditor({
  schema,
  value,
  disabled,
  path,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const currentType = typeof value.type === 'string' ? value.type : '';
  const [draftType, setDraftType] = useState(currentType);
  const [draftField, setDraftField] = useState('');
  const resetValue = useRef<JsonObject | undefined>(undefined);
  useEffect(() => {
    if (resetValue.current === value) return;
    resetValue.current = value;
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
      {draftType === 'pose' && (() => {
        const poseSchema = schema.properties?.pose ?? {};
        const posePath = path === undefined ? undefined : `${path}.pose`;
        const editor = (
          <MotionPoseEditor
            schema={poseSchema}
            value={draftType === currentType && value.pose
              ? objectValue(value.pose)
              : defaultMotionPose(schema.properties?.pose)}
            disabled={disabled}
            path={posePath}
            onCommit={(pose) => onCommit({ type: 'pose', pose })}
          />
        );
        return posePath === undefined
          ? editor
          : <SourceableValue path={posePath} schema={poseSchema}>{editor}</SourceableValue>;
      })()}
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
  path,
  sourceJointInventory,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue[];
  disabled: boolean;
  path?: string;
  sourceJointInventory?: ProgramSourceJointInventory | null;
  onCommit: (value: JsonValue) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  const previewValues = useContext(ProgramManualFillPreviewContext);
  const availableJointNames = useMemo(
    () => sourceJointInventory?.jointNames ?? [],
    [sourceJointInventory],
  );
  const inventoryAvailable = sourceJointInventory?.unavailableReason === null;
  const [rows, setRows] = useState<JointTargetEditorRow[]>(() =>
    jointTargetEditorRows(availableJointNames, value));
  const resetSource = useRef<{ value: JsonValue[]; joints: string[] } | null>(null);
  useEffect(() => {
    const previous = resetSource.current;
    if (previous?.value === value
      && previous.joints.length === availableJointNames.length
      && previous.joints.every((name, index) => name === availableJointNames[index])) return;
    resetSource.current = { value, joints: availableJointNames };
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
        const previewValue = path === undefined ? undefined
          : previewValues.get(`${path}[${row.jointName}].position_rad`);
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
            <label className={`program-joint-target-position${previewValue === undefined ? '' : ' program-manual-fill-changed'}`}>
              <span className="sr-only">Position (rad)</span>
              <FillTarget previewValue={previewValue}>
                <JointTargetPositionInput
                  jointName={row.jointName}
                  value={row.selected ? row.positionText : ''}
                  disabled={disabled || !row.selected}
                  onCommit={(positionText) => stage(rows.map((candidate) =>
                    candidate.jointName === row.jointName
                      ? { ...candidate, positionText }
                      : candidate))}
                />
              </FillTarget>
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

/** Optional per-waypoint corner blend radius; empty omits it so the node's value applies. */
function WaypointBlendRadiusField({
  label,
  value,
  disabled,
  onCommit,
}: {
  label: string;
  value: JsonValue | undefined;
  disabled: boolean;
  onCommit: (value: number | undefined) => void;
}) {
  const reportInvalid = useSpecializedInvalidReporter();
  return (
    <label className="program-specialized-number">
      <span>Corner blend radius (m)</span>
      <DraftTextInput
        type="number"
        step="any"
        min={0}
        value={typeof value === 'number' ? value : ''}
        disabled={disabled}
        aria-label={label}
        placeholder="Node value"
        title="Empty uses the node's corner blend radius; 0 keeps this corner exact."
        validate={(text) => text.trim() === ''
          || (Number.isFinite(Number(text)) && Number(text) >= 0)
          ? null
          : `${label} must be a non-negative number.`}
        onInvalidChange={reportInvalid}
        onCommit={(text) => onCommit(text.trim() === '' ? undefined : Number(text))}
      />
    </label>
  );
}

function CartesianPathEditor({
  schema,
  value,
  disabled,
  path,
  relativeGoal = false,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue[];
  disabled: boolean;
  path?: string;
  relativeGoal?: boolean;
  onCommit: ValueCommit;
}) {
  const itemSchema = schema.items ?? {};
  const hasBlendRadius = itemSchema.properties?.blend_radius_m !== undefined;
  return (
    <div className="program-specialized-editor" data-editor="cartesian-path">
      {value.map((waypoint, index) => {
        const waypointPath = path === undefined ? undefined : `${path}[${index}]`;
        const editor = (
          <MotionPoseEditor
            schema={itemSchema}
            value={objectValue(waypoint)}
            disabled={disabled}
            path={waypointPath}
            relativeGoal={relativeGoal}
            onCommit={(nextWaypoint) => {
              const next = [...value];
              next[index] = nextWaypoint;
              onCommit(next);
            }}
          />
        );
        return (
          <fieldset className="program-waypoint" key={index}>
            <legend>Waypoint {index + 1}</legend>
            {waypointPath === undefined
              ? editor
              : <SourceableValue path={waypointPath} schema={itemSchema}>{editor}</SourceableValue>}
            {hasBlendRadius && (
              <WaypointBlendRadiusField
                label={`Waypoint ${index + 1} corner blend radius (m)`}
                value={objectValue(waypoint).blend_radius_m}
                disabled={disabled}
                onCommit={(radius) => {
                  const nextWaypoint = { ...objectValue(waypoint) };
                  if (radius === undefined) delete nextWaypoint.blend_radius_m;
                  else nextWaypoint.blend_radius_m = radius;
                  const next = [...value];
                  next[index] = nextWaypoint;
                  onCommit(next);
                }}
              />
            )}
            <button
              type="button"
              className="text-button"
              disabled={disabled || value.length <= (schema.minItems ?? 0)}
              onClick={() => onCommit(
                value.filter((_, rowIndex) => rowIndex !== index),
                path === undefined ? [] : [listDeleteMove(path, value.length, index)],
              )}
            >
              Remove waypoint
            </button>
          </fieldset>
        );
      })}
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
  path,
  onCommit,
}: {
  schema: CatalogJsonSchema;
  value: JsonObject;
  disabled: boolean;
  path?: string;
  onCommit: (value: JsonValue) => void;
}) {
  const kinds = enumStrings(schema.properties?.kind);
  const kind = typeof value.kind === 'string' ? value.kind : kinds[0] ?? '';
  const presetNames = enumStrings(schema.properties?.name);
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
        {kinds.includes('named_preset')
          ? 'Program stores a coordinate value or preset name. Preset names resolve against the bound gripper when the node runs.'
          : 'Program authoring stores only the catalog coordinate kind and value; live gripper range, presets, and direct control are unavailable here.'}
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
      {kind === 'named_preset' ? (
        <label>
          Preset
          <select
            aria-label="Gripper preset"
            value={typeof value.name === 'string' ? value.name : presetNames[0] ?? ''}
            disabled={disabled}
            onChange={(event) => {
              const next = completeCoordinateKindTransition(schema, kind, event.target.value);
              if (next) onCommit(next);
            }}
          >
            {presetNames.map((name) => <option key={name} value={name}>{name}</option>)}
          </select>
        </label>
      ) : <NumberField
        label={`Value (${units})`}
        value={typeof value[valueField] === 'number' ? value[valueField] as number : 0}
        disabled={disabled}
        path={path === undefined ? undefined : `${path}.${valueField}`}
        schema={schema.properties?.[valueField] ?? NUMBER_SCHEMA}
        onCommit={(next) => onCommit({ kind, [valueField]: next })}
      />}
    </div>
  );
}

export function completeCoordinateKindTransition(
  schema: CatalogJsonSchema,
  kind: string,
  presetName?: string,
): JsonObject | undefined {
  if (kind === 'named_preset') {
    const name = presetName ?? enumStrings(schema.properties?.name)[0];
    if (name === undefined) return undefined;
    const candidate: JsonObject = { kind, name };
    return firstCatalogSchemaValidationMessage(schema, candidate, 'coordinate')
      ? undefined
      : candidate;
  }
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
    && variants.includes('component_frame')
    && variants.includes('pose')
  ) return 'motion-reference';
  const coordinateKinds = enumStrings(schema.properties?.kind);
  if (
    schema.type === 'object'
    && coordinateKinds.some((kind) => kind === 'angular_rad' || kind === 'angular_newton_meter')
  ) return 'coordinate';
  if (
    schema.type === 'object'
    && coordinateKinds.includes('cell_frame')
    && coordinateKinds.includes('target_frame')
  ) return 'frame-reference';
  return null;
}

export function supportsProgramSpecializedEditor(schema: CatalogJsonSchema): boolean {
  return specializedKind(schema) !== null;
}

/** Complete local pose seed; opening an editor must not author this value. */
export function defaultMotionPoseValue(schema: CatalogJsonSchema): JsonObject | undefined {
  return specializedKind(schema) === 'motion-pose' ? defaultMotionPose(schema) : undefined;
}

export function ProgramSpecializedEditor({
  schema,
  value,
  disabled,
  path,
  sourceJointInventory,
  frameScope,
  relativeGoal = false,
  onCommit,
  onInvalidChange,
}: {
  schema: CatalogJsonSchema;
  value: JsonValue;
  disabled: boolean;
  /** The value's path in a Program node, where its values may come from sources. */
  path?: string;
  sourceJointInventory?: ProgramSourceJointInventory | null;
  /** The Program's declared frames; without it frame fields are typed as text. */
  frameScope?: FrameScope;
  /** The node's goal is relative (target_mode), so poses take no frame of their own. */
  relativeGoal?: boolean;
  onCommit: ValueCommit;
  onInvalidChange?: (message: string | null) => void;
}) {
  const [invalid, setInvalid] = useState<Map<symbol, string>>(() => new Map());
  const reportInvalid = useCallback<SpecializedInvalidReporter>((token, message) => {
    setInvalid((current) => {
      if ((current.get(token) ?? null) === message) return current;
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
        path={path}
        sourceJointInventory={sourceJointInventory}
        onCommit={onCommit}
      />
    );
  } else if (kind === 'cartesian-path') {
    content = <CartesianPathEditor schema={schema} value={value as JsonValue[]} disabled={disabled} path={path} relativeGoal={relativeGoal} onCommit={onCommit} />;
  } else if (kind === 'motion-pose') {
    content = <MotionPoseEditor schema={schema} value={objectValue(value)} disabled={disabled} path={path} relativeGoal={relativeGoal} onCommit={onCommit} />;
  } else if (kind === 'motion-reference') {
    content = <MotionReferenceEditor schema={schema} value={objectValue(value)} disabled={disabled} path={path} onCommit={onCommit} />;
  } else if (kind === 'coordinate') {
    content = <CoordinateEditor schema={schema} value={objectValue(value)} disabled={disabled} path={path} onCommit={onCommit} />;
  } else if (kind === 'frame-reference') {
    content = <FrameReferenceEditor schema={schema} value={objectValue(value)} disabled={disabled} path={path} onCommit={onCommit} />;
  }
  return (
    <SpecializedInvalidContext.Provider value={reportInvalid}>
      <FrameScopeContext.Provider value={frameScope ?? null}>
        {content}
      </FrameScopeContext.Provider>
    </SpecializedInvalidContext.Provider>
  );
}

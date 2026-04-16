import { useCallback, useEffect, useState } from 'react';

import {
  getFieldRefTarget,
  LiteralOrFieldReferenceEditor,
  type BlackboardFieldOption,
} from './BlackboardBindingEditors';
import { primitiveListValuesEqual } from './utils';

interface PoseEnvelopeValue {
  position_min_m: number[];
  position_max_m: number[];
  orientation_rpy_min_deg: number[];
  orientation_rpy_max_deg: number[];
}

type RelativeMoveReferenceType = 'frame' | 'object' | 'robot_link' | 'pose';

interface RelativeMoveReferenceValue {
  type: RelativeMoveReferenceType;
  id?: string;
  pose?: Record<string, unknown>;
}

interface RelativeMoveValue {
  target_reference: RelativeMoveReferenceValue;
  offset_reference?: RelativeMoveReferenceValue;
  offset: Record<string, unknown>;
}

interface JointGoalRow {
  jointName: string;
  positionText: string;
  selected: boolean;
}

const RELATIVE_MOVE_REFERENCE_TYPES: RelativeMoveReferenceType[] = [
  'frame',
  'object',
  'robot_link',
  'pose',
];

function normalizeNumberArray(value: unknown, length: number, defaults?: number[]): number[] {
  if (Array.isArray(value)) {
    const normalized = value.slice(0, length).map((item, index) => {
      return typeof item === 'number' && Number.isFinite(item)
        ? item
        : (defaults?.[index] ?? 0);
    });
    while (normalized.length < length) {
      normalized.push(defaults?.[normalized.length] ?? 0);
    }
    return normalized;
  }
  return Array.from({ length }, (_, index) => defaults?.[index] ?? 0);
}

function normalizePoseEnvelope(value: unknown): PoseEnvelopeValue {
  const envelope = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
  return {
    position_min_m: normalizeNumberArray(envelope.position_min_m, 3),
    position_max_m: normalizeNumberArray(envelope.position_max_m, 3),
    orientation_rpy_min_deg: normalizeNumberArray(envelope.orientation_rpy_min_deg, 3, [0, 0, 0]),
    orientation_rpy_max_deg: normalizeNumberArray(envelope.orientation_rpy_max_deg, 3, [0, 0, 0]),
  };
}

function normalizeRelativeMoveReference(
  value: unknown,
  fallbackType: RelativeMoveReferenceType = 'object',
): RelativeMoveReferenceValue {
  const reference = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
  const type = RELATIVE_MOVE_REFERENCE_TYPES.includes(reference.type as RelativeMoveReferenceType)
    ? reference.type as RelativeMoveReferenceType
    : fallbackType;

  if (type === 'pose') {
    return {
      type,
      pose: reference.pose && typeof reference.pose === 'object' && !Array.isArray(reference.pose)
        ? reference.pose as Record<string, unknown>
        : {},
    };
  }

  return {
    type,
    id: typeof reference.id === 'string' ? reference.id : '',
  };
}

function sanitizeRelativeMoveReference(value: RelativeMoveReferenceValue): Record<string, unknown> {
  if (value.type === 'pose') {
    return {
      type: value.type,
      pose: getFieldRefTarget(value.pose)
        ? value.pose
        : value.pose && typeof value.pose === 'object' && !Array.isArray(value.pose)
          ? value.pose
          : {},
    };
  }
  return {
    type: value.type,
    id: (value.id ?? '').trim(),
  };
}

function normalizeRelativeMove(value: unknown): RelativeMoveValue {
  const relativeMove = value && typeof value === 'object' && !Array.isArray(value)
    ? value as Record<string, unknown>
    : {};
  const hasOffsetReference =
    relativeMove.offset_reference && typeof relativeMove.offset_reference === 'object' && !Array.isArray(relativeMove.offset_reference);

  return {
    target_reference: normalizeRelativeMoveReference(relativeMove.target_reference, 'object'),
    offset_reference: hasOffsetReference
      ? normalizeRelativeMoveReference(relativeMove.offset_reference, 'frame')
      : undefined,
    offset: relativeMove.offset && typeof relativeMove.offset === 'object' && !Array.isArray(relativeMove.offset)
      ? relativeMove.offset as Record<string, unknown>
      : {},
  };
}

function validateRelativeMoveReference(
  value: RelativeMoveReferenceValue,
  label: string,
): string | null {
  if (!RELATIVE_MOVE_REFERENCE_TYPES.includes(value.type)) {
    return `${label} type is invalid`;
  }
  if (value.type === 'pose') {
    if (!value.pose || typeof value.pose !== 'object' || Array.isArray(value.pose)) {
      return `${label} pose is required when type=pose`;
    }
    if (getFieldRefTarget(value.pose)) {
      return null;
    }
    const pose = value.pose as Record<string, unknown>;
    if ('position_m' in pose) {
      const position = pose.position_m;
      if (!Array.isArray(position) || position.length !== 3 || position.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
        return `${label} pose position_m must be a list of 3 numbers`;
      }
    }
    if ('orientation_rpy_deg' in pose) {
      const orientation = pose.orientation_rpy_deg;
      if (!Array.isArray(orientation) || orientation.length !== 3 || orientation.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
        return `${label} pose orientation_rpy_deg must be a list of 3 numbers`;
      }
    }
    if ('orientation_xyzw' in pose) {
      const orientation = pose.orientation_xyzw;
      if (!Array.isArray(orientation) || orientation.length !== 4 || orientation.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
        return `${label} pose orientation_xyzw must be a list of 4 numbers`;
      }
    }
    return null;
  }
  if (!(value.id ?? '').trim()) {
    return `${label} id is required when type=${value.type}`;
  }
  return null;
}

function validateRelativeMove(value: RelativeMoveValue): string | null {
  const targetError = validateRelativeMoveReference(value.target_reference, 'Target reference');
  if (targetError) {
    return targetError;
  }

  if (value.offset_reference) {
    const offsetReferenceError = validateRelativeMoveReference(value.offset_reference, 'Offset reference');
    if (offsetReferenceError) {
      return offsetReferenceError;
    }
  }

  if (!value.offset || typeof value.offset !== 'object' || Array.isArray(value.offset)) {
    return 'Offset is required';
  }
  const offset = value.offset as Record<string, unknown>;
  if ('position_m' in offset) {
    const position = offset.position_m;
    if (!Array.isArray(position) || position.length !== 3 || position.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
      return 'Offset position_m must be a list of 3 numbers';
    }
  }
  if ('orientation_rpy_deg' in offset) {
    const orientation = offset.orientation_rpy_deg;
    if (!Array.isArray(orientation) || orientation.length !== 3 || orientation.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
      return 'Offset orientation_rpy_deg must be a list of 3 numbers';
    }
  }
  if ('orientation_xyzw' in offset) {
    const orientation = offset.orientation_xyzw;
    if (!Array.isArray(orientation) || orientation.length !== 4 || orientation.some((entry) => typeof entry !== 'number' || !Number.isFinite(entry))) {
      return 'Offset orientation_xyzw must be a list of 4 numbers';
    }
  }
  return null;
}

function sanitizeRelativeMove(value: RelativeMoveValue): Record<string, unknown> {
  const result: Record<string, unknown> = {
    target_reference: sanitizeRelativeMoveReference(value.target_reference),
    offset: value.offset,
  };
  if (value.offset_reference) {
    result.offset_reference = sanitizeRelativeMoveReference(value.offset_reference);
  }
  return result;
}

function validatePoseEnvelope(value: PoseEnvelopeValue): string | null {
  const checks: Array<{
    label: string;
    minValues: number[];
    maxValues: number[];
    axisLabels: string[];
  }> = [
    {
      label: 'position',
      minValues: value.position_min_m,
      maxValues: value.position_max_m,
      axisLabels: ['x', 'y', 'z'],
    },
    {
      label: 'orientation',
      minValues: value.orientation_rpy_min_deg,
      maxValues: value.orientation_rpy_max_deg,
      axisLabels: ['r', 'p', 'y'],
    },
  ];

  for (const check of checks) {
    for (let index = 0; index < check.axisLabels.length; index += 1) {
      if (check.minValues[index] > check.maxValues[index]) {
        return `${check.label} ${check.axisLabels[index]} min must be <= max`;
      }
    }
  }
  return null;
}

function jointGoalArraysEqual(
  leftNames: string[],
  leftPositions: number[],
  rightNames: string[],
  rightPositions: number[],
): boolean {
  return primitiveListValuesEqual(leftNames, rightNames)
    && primitiveListValuesEqual(leftPositions, rightPositions);
}

function NumberInput({
  value,
  onChange,
  disabled,
}: {
  value: number;
  onChange: (value: number) => void;
  disabled: boolean;
}) {
  const [draft, setDraft] = useState(() =>
    Number.isFinite(value) ? String(value) : ''
  );
  const [isFocused, setIsFocused] = useState(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(Number.isFinite(value) ? String(value) : '');
  }, [value, isFocused]);

  return (
    <input
      type="text"
      inputMode="decimal"
      value={draft}
      onFocus={() => setIsFocused(true)}
      onChange={(event) => {
        const nextDraft = event.target.value;
        setDraft(nextDraft);
        const trimmed = nextDraft.trim();
        if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
          return;
        }
        const nextValue = Number(trimmed);
        if (Number.isFinite(nextValue)) {
          onChange(nextValue);
        }
      }}
      onBlur={() => {
        setIsFocused(false);
        const trimmed = draft.trim();
        if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
          setDraft(Number.isFinite(value) ? String(value) : '');
          return;
        }
        const nextValue = Number(trimmed);
        if (Number.isFinite(nextValue)) {
          onChange(nextValue);
          setDraft(String(nextValue));
          return;
        }
        setDraft(Number.isFinite(value) ? String(value) : '');
      }}
      disabled={disabled}
      className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-xs text-white ${disabled ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
    />
  );
}

function VectorInput({
  labels,
  values,
  onChange,
  disabled,
}: {
  labels: string[];
  values: number[];
  onChange: (values: number[]) => void;
  disabled: boolean;
}) {
  return (
    <div className={`grid gap-2 ${labels.length === 4 ? 'grid-cols-4' : 'grid-cols-3'}`}>
      {labels.map((label, index) => (
        <div key={label}>
          <div className="mb-1 text-[11px] uppercase text-gray-500">{label}</div>
          <NumberInput
            value={values[index] ?? 0}
            disabled={disabled}
            onChange={(nextValue) => {
              const next = [...values];
              next[index] = nextValue;
              onChange(next);
            }}
          />
        </div>
      ))}
    </div>
  );
}

export function PoseEditor({
  currentValue,
  isRunMode,
  onCommit,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
}) {
  const pose = currentValue && typeof currentValue === 'object' && !Array.isArray(currentValue)
    ? currentValue as Record<string, unknown>
    : {};
  const position = normalizeNumberArray(pose.position_m, 3);
  const usesQuaternion = Array.isArray(pose.orientation_xyzw) && !Array.isArray(pose.orientation_rpy_deg);
  const orientation = usesQuaternion
    ? normalizeNumberArray(pose.orientation_xyzw, 4, [0, 0, 0, 1])
    : normalizeNumberArray(pose.orientation_rpy_deg, 3, [180, 0, 0]);

  return (
    <div className="space-y-2">
      <div>
        <div className="mb-1 text-[11px] text-gray-500">Position (m)</div>
        <VectorInput
          labels={['x', 'y', 'z']}
          values={position}
          disabled={isRunMode}
          onChange={(next) => {
            const updated: Record<string, unknown> = {
              ...pose,
              position_m: next,
            };
            if (usesQuaternion) {
              updated.orientation_xyzw = orientation;
              delete updated.orientation_rpy_deg;
            } else {
              updated.orientation_rpy_deg = orientation;
              delete updated.orientation_xyzw;
            }
            onCommit(updated);
          }}
        />
      </div>
      <div>
        <div className="mb-1 text-[11px] text-gray-500">
          {usesQuaternion ? 'Orientation (xyzw)' : 'Orientation (rpy deg)'}
        </div>
        <VectorInput
          labels={usesQuaternion ? ['x', 'y', 'z', 'w'] : ['r', 'p', 'y']}
          values={orientation}
          disabled={isRunMode}
          onChange={(next) => {
            const updated: Record<string, unknown> = {
              ...pose,
              position_m: position,
            };
            if (usesQuaternion) {
              updated.orientation_xyzw = next;
              delete updated.orientation_rpy_deg;
            } else {
              updated.orientation_rpy_deg = next;
              delete updated.orientation_xyzw;
            }
            onCommit(updated);
          }}
        />
      </div>
    </div>
  );
}

export function PoseEnvelopeEditor({
  currentValue,
  isRunMode,
  onCommit,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
}) {
  const [draft, setDraft] = useState<PoseEnvelopeValue>(() => normalizePoseEnvelope(currentValue));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setDraft(normalizePoseEnvelope(currentValue));
    setError(null);
  }, [currentValue]);

  const commit = (patch: Partial<PoseEnvelopeValue>) => {
    const nextDraft: PoseEnvelopeValue = {
      ...draft,
      ...patch,
    };
    setDraft(nextDraft);
    const validationError = validatePoseEnvelope(nextDraft);
    setError(validationError);
    if (!validationError) {
      onCommit(nextDraft);
    }
  };

  return (
    <div className="space-y-2">
      <div>
        <div className="mb-1 text-[11px] text-gray-500">Position min (m)</div>
        <VectorInput labels={['x', 'y', 'z']} values={draft.position_min_m} disabled={isRunMode} onChange={(next) => commit({ position_min_m: next })} />
      </div>
      <div>
        <div className="mb-1 text-[11px] text-gray-500">Position max (m)</div>
        <VectorInput labels={['x', 'y', 'z']} values={draft.position_max_m} disabled={isRunMode} onChange={(next) => commit({ position_max_m: next })} />
      </div>
      <div>
        <div className="mb-1 text-[11px] text-gray-500">Orientation min (rpy deg)</div>
        <VectorInput labels={['r', 'p', 'y']} values={draft.orientation_rpy_min_deg} disabled={isRunMode} onChange={(next) => commit({ orientation_rpy_min_deg: next })} />
      </div>
      <div>
        <div className="mb-1 text-[11px] text-gray-500">Orientation max (rpy deg)</div>
        <VectorInput labels={['r', 'p', 'y']} values={draft.orientation_rpy_max_deg} disabled={isRunMode} onChange={(next) => commit({ orientation_rpy_max_deg: next })} />
      </div>
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

export function PoseMapEditor({
  currentValue,
  isRunMode,
  onCommit,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
}) {
  const poseMap = currentValue && typeof currentValue === 'object' && !Array.isArray(currentValue)
    ? currentValue as Record<string, unknown>
    : {};
  const entries = Object.entries(poseMap);

  if (entries.length === 0) {
    return <div className="text-xs text-gray-500">No pose map entries</div>;
  }

  return (
    <div className="space-y-3">
      {entries.map(([key, pose]) => (
        <div key={key} className="rounded border border-gray-700 p-2">
          <div className="mb-2 text-xs font-medium text-gray-300">{key}</div>
          <PoseEditor
            currentValue={pose}
            isRunMode={isRunMode}
            onCommit={(nextPose) => onCommit({ ...poseMap, [key]: nextPose })}
          />
        </div>
      ))}
    </div>
  );
}

function RelativeMoveReferenceEditor({
  label,
  value,
  isRunMode,
  onChange,
  fieldOptions,
}: {
  label: string;
  value: RelativeMoveReferenceValue;
  isRunMode: boolean;
  onChange: (value: RelativeMoveReferenceValue) => void;
  fieldOptions: BlackboardFieldOption[];
}) {
  const disabledClasses = isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : '';
  const idLabel = value.type === 'robot_link'
    ? 'Robot link id'
    : value.type === 'frame'
      ? 'Frame id'
      : 'Object id';

  return (
    <div className="space-y-2 rounded border border-gray-700 p-2">
      <div className="text-xs font-medium text-gray-300">{label}</div>
      <select
        value={value.type}
        onChange={(event) => {
          const nextType = event.target.value as RelativeMoveReferenceType;
          if (nextType === 'pose') {
            onChange({
              type: nextType,
              pose: value.pose && typeof value.pose === 'object' && !Array.isArray(value.pose)
                ? value.pose
                : {},
            });
            return;
          }
          onChange({
            type: nextType,
            id: value.type === 'pose' ? '' : (value.id ?? ''),
          });
        }}
        disabled={isRunMode}
        className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${disabledClasses}`}
      >
        {RELATIVE_MOVE_REFERENCE_TYPES.map((referenceType) => (
          <option key={referenceType} value={referenceType}>
            {referenceType}
          </option>
        ))}
      </select>
      {value.type === 'pose' ? (
        <LiteralOrFieldReferenceEditor
          currentValue={value.pose ?? {}}
          isRunMode={isRunMode}
          onCommitLiteral={(nextPose) => onChange({ type: 'pose', pose: nextPose as Record<string, unknown> })}
          onCommitFieldRef={(nextFieldRef) => onChange({ type: 'pose', pose: nextFieldRef as Record<string, unknown> })}
          fieldOptions={fieldOptions}
          expectedType="pose"
          emptyMessage="No declared pose fields yet. Add one in the PARAMS tab."
          literalLabel="Inline pose"
          fieldLabel="Declared pose field"
          createDefaultLiteral={() => ({
            position_m: [0, 0, 0],
            orientation_rpy_deg: [0, 0, 0],
          })}
          renderLiteralEditor={(literalValue, onCommitLiteral) => (
            <PoseEditor
              currentValue={literalValue}
              isRunMode={isRunMode}
              onCommit={onCommitLiteral}
            />
          )}
        />
      ) : (
        <div>
          <div className="mb-1 text-[11px] text-gray-500">{idLabel}</div>
          <input
            type="text"
            value={value.id ?? ''}
            onChange={(event) => onChange({ type: value.type, id: event.target.value })}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${disabledClasses}`}
          />
        </div>
      )}
    </div>
  );
}

export function RelativeMoveEditor({
  currentValue,
  isRunMode,
  onCommit,
  fieldOptions,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: unknown) => void;
  fieldOptions: BlackboardFieldOption[];
}) {
  const [draft, setDraft] = useState<RelativeMoveValue>(() => normalizeRelativeMove(currentValue));
  const [error, setError] = useState<string | null>(() => validateRelativeMove(normalizeRelativeMove(currentValue)));

  useEffect(() => {
    const normalized = normalizeRelativeMove(currentValue);
    setDraft(normalized);
    setError(validateRelativeMove(normalized));
  }, [currentValue]);

  const commit = (nextDraft: RelativeMoveValue) => {
    setDraft(nextDraft);
    const validationError = validateRelativeMove(nextDraft);
    setError(validationError);
    if (!validationError) {
      onCommit(sanitizeRelativeMove(nextDraft));
    }
  };

  return (
    <div className="space-y-3">
      <RelativeMoveReferenceEditor
        label="Target reference"
        value={draft.target_reference}
        isRunMode={isRunMode}
        onChange={(nextValue) => commit({ ...draft, target_reference: nextValue })}
        fieldOptions={fieldOptions}
      />

      <div className="space-y-2 rounded border border-gray-700 p-2">
        <label className="flex items-center gap-2 text-xs text-gray-300">
          <input
            type="checkbox"
            checked={draft.offset_reference !== undefined}
            disabled={isRunMode}
            onChange={(event) => {
              if (event.target.checked) {
                commit({
                  ...draft,
                  offset_reference: normalizeRelativeMoveReference(draft.offset_reference, 'frame'),
                });
                return;
              }
              commit({
                ...draft,
                offset_reference: undefined,
              });
            }}
            className={`${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
          />
          Use separate offset reference
        </label>
        {draft.offset_reference ? (
          <RelativeMoveReferenceEditor
            label="Offset reference"
            value={draft.offset_reference}
            isRunMode={isRunMode}
            onChange={(nextValue) => commit({ ...draft, offset_reference: nextValue })}
            fieldOptions={fieldOptions}
          />
        ) : (
          <div className="text-[11px] text-gray-500">
            Offset will be expressed in the target reference frame.
          </div>
        )}
      </div>

      <div className="rounded border border-gray-700 p-2">
        <div className="mb-2 text-xs font-medium text-gray-300">Offset</div>
        <PoseEditor
          currentValue={draft.offset}
          isRunMode={isRunMode}
          onCommit={(nextOffset) => commit({ ...draft, offset: nextOffset as Record<string, unknown> })}
        />
      </div>
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

export function JointGoalEditor({
  jointNames,
  currentJointNames,
  currentJointPositions,
  isRunMode,
  onCommit,
}: {
  jointNames: string[];
  currentJointNames: string[];
  currentJointPositions: number[];
  isRunMode: boolean;
  onCommit: (jointNames: string[], jointPositions: number[]) => void;
}) {
  const buildRows = useCallback((): JointGoalRow[] => {
    const positionByJoint = new Map<string, number>();
    currentJointNames.forEach((name, index) => {
      positionByJoint.set(name, currentJointPositions[index] ?? 0);
    });

    const orderedNames = [...jointNames];
    for (const name of currentJointNames) {
      if (!orderedNames.includes(name)) {
        orderedNames.push(name);
      }
    }

    return orderedNames.map((name) => ({
      jointName: name,
      positionText: String(positionByJoint.get(name) ?? 0),
      selected: positionByJoint.has(name),
    }));
  }, [currentJointNames, currentJointPositions, jointNames]);

  const [rows, setRows] = useState<JointGoalRow[]>(() => buildRows());
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setRows(buildRows());
    setError(null);
  }, [buildRows]);

  const commit = useCallback((nextRows: JointGoalRow[]) => {
    setRows(nextRows);

    const selectedRows = nextRows.filter((row) => row.selected);
    if (selectedRows.length === 0) {
      setError('Select at least one joint');
      return;
    }

    const nextJointNames: string[] = [];
    const nextJointPositions: number[] = [];
    for (const row of selectedRows) {
      const trimmed = row.positionText.trim();
      if (!trimmed) {
        setError(`Enter a value for ${row.jointName}`);
        return;
      }
      const parsed = Number(trimmed);
      if (!Number.isFinite(parsed)) {
        setError(`Joint ${row.jointName} must have a valid numeric value`);
        return;
      }
      nextJointNames.push(row.jointName);
      nextJointPositions.push(parsed);
    }

    setError(null);
    if (!jointGoalArraysEqual(nextJointNames, nextJointPositions, currentJointNames, currentJointPositions)) {
      onCommit(nextJointNames, nextJointPositions);
    }
  }, [currentJointNames, currentJointPositions, onCommit]);

  return (
    <div className="space-y-3 rounded border border-gray-700 p-2">
      <div className="space-y-1">
        <div className="text-xs font-medium text-gray-300">Joint goal</div>
        <div className="text-[11px] text-gray-500">
          Select the joints to command and enter the target position in radians for each selected joint.
        </div>
      </div>
      {rows.length === 0 ? (
        <div className="text-[11px] text-gray-500">
          No joint names available from backend scene state.
        </div>
      ) : (
        <div className="space-y-2">
          {rows.map((row) => (
            <div key={row.jointName} className="grid gap-2 md:grid-cols-[minmax(0,1fr)_112px] md:items-center">
              <label className="flex items-center gap-2 text-xs text-gray-300">
                <input
                  type="checkbox"
                  checked={row.selected}
                  disabled={isRunMode}
                  onChange={(event) => {
                    const nextRows = rows.map((candidate) =>
                      candidate.jointName === row.jointName
                        ? { ...candidate, selected: event.target.checked }
                        : candidate
                    );
                    commit(nextRows);
                  }}
                  className={`${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
                />
                <span>{row.jointName}</span>
              </label>
              <div className="space-y-1">
                <div className="text-[11px] text-gray-500 md:hidden">Target position (rad)</div>
                <input
                  type="text"
                  inputMode="decimal"
                  value={row.positionText}
                  disabled={isRunMode || !row.selected}
                  onChange={(event) => {
                    const nextRows = rows.map((candidate) =>
                      candidate.jointName === row.jointName
                        ? { ...candidate, positionText: event.target.value }
                        : candidate
                    );
                    commit(nextRows);
                  }}
                  className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white md:w-28 ${(isRunMode || !row.selected) ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
                  placeholder="0.0"
                />
              </div>
            </div>
          ))}
        </div>
      )}
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

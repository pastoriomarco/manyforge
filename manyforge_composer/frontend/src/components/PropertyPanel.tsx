import { useCallback, useEffect, useState } from 'react';
import { useProgramStore } from '../store/programStore';
import { useCatalogStore } from '../store/catalogStore';
import { useCycleStore } from '../store/cycleStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { api } from '../api/client';
import type { FullDeployment, ModelEndpointCatalog } from '../types/api';

const KIND_COLORS: Record<string, string> = {
  sequence: 'border-blue-500 bg-blue-900/50',
  fallback: 'border-orange-500 bg-orange-900/50',
  parallel: 'border-purple-500 bg-purple-900/50',
  inverter: 'border-yellow-500 bg-yellow-900/50',
  repeat: 'border-cyan-500 bg-cyan-900/50',
  retry: 'border-cyan-500 bg-cyan-900/50',
  default: 'border-green-500 bg-green-900/50',
};

type ConstraintMap = Record<string, unknown>;

interface PoseEnvelopeValue {
  position_min_m: number[];
  position_max_m: number[];
  orientation_rpy_min_deg: number[];
  orientation_rpy_max_deg: number[];
}

interface TemplateVariableRow {
  id: string;
  name: string;
  path: string;
}

function nextTemplateVariableRowId(): string {
  return `template-variable-${Math.random().toString(36).slice(2, 10)}`;
}

function normalizeTemplateVariableRows(value: unknown): TemplateVariableRow[] {
  if (value && typeof value === 'object' && !Array.isArray(value)) {
    return Object.entries(value as Record<string, unknown>).map(([name, path]) => ({
      id: nextTemplateVariableRowId(),
      name,
      path: typeof path === 'string' ? path : String(path ?? ''),
    }));
  }
  return [];
}

function formatStructuredValue(value: unknown): string {
  if (value === undefined) {
    return '';
  }
  try {
    return JSON.stringify(value, null, 2);
  } catch {
    return String(value);
  }
}

function parseStructuredValue(paramType: string, text: string): unknown {
  const parsed = JSON.parse(text);
  if (paramType === 'dict' || paramType === 'pose' || paramType === 'object') {
    if (parsed === null || Array.isArray(parsed) || typeof parsed !== 'object') {
      throw new Error('Value must be a JSON object');
    }
    return parsed;
  }
  if (paramType === 'float_list') {
    if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'number')) {
      throw new Error('Value must be a JSON array of numbers');
    }
    return parsed;
  }
  if (paramType === 'string_list') {
    if (!Array.isArray(parsed) || !parsed.every((item) => typeof item === 'string')) {
      throw new Error('Value must be a JSON array of strings');
    }
    return parsed;
  }
  return parsed;
}

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

function getNumericConstraintError(value: number, constraints: ConstraintMap): string | null {
  const minInclusive = typeof constraints.min_inclusive === 'number'
    ? constraints.min_inclusive
    : undefined;
  const minExclusive = typeof constraints.min_exclusive === 'number'
    ? constraints.min_exclusive
    : undefined;
  const maxInclusive = typeof constraints.max_inclusive === 'number'
    ? constraints.max_inclusive
    : undefined;
  const maxExclusive = typeof constraints.max_exclusive === 'number'
    ? constraints.max_exclusive
    : undefined;

  if (minInclusive !== undefined && value < minInclusive) {
    return `Value must be >= ${minInclusive}`;
  }
  if (minExclusive !== undefined && value <= minExclusive) {
    return `Value must be > ${minExclusive}`;
  }
  if (maxInclusive !== undefined && value > maxInclusive) {
    return `Value must be <= ${maxInclusive}`;
  }
  if (maxExclusive !== undefined && value >= maxExclusive) {
    return `Value must be < ${maxExclusive}`;
  }
  return null;
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

  useEffect(() => {
    setDraft(Number.isFinite(value) ? String(value) : '');
  }, [value]);

  return (
    <input
      type="text"
      inputMode="decimal"
      value={draft}
      onChange={(e) => {
        const nextDraft = e.target.value;
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

function PoseEditor({
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

function PoseEnvelopeEditor({
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

function NumericParamEditor({
  currentValue,
  isRunMode,
  onCommit,
  paramType,
  constraints,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: number) => void;
  paramType: 'float' | 'int';
  constraints: ConstraintMap;
}) {
  const [draft, setDraft] = useState(() =>
    currentValue !== undefined && currentValue !== null ? String(currentValue) : ''
  );
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setDraft(currentValue !== undefined && currentValue !== null ? String(currentValue) : '');
    setError(null);
  }, [currentValue]);

  const tryCommit = (rawValue: string) => {
    const trimmed = rawValue.trim();
    if (trimmed === '' || trimmed === '-' || trimmed === '+' || trimmed === '.' || trimmed === '-.' || trimmed === '+.') {
      setError(null);
      return;
    }
    const parsed = Number(trimmed);
    if (!Number.isFinite(parsed)) {
      setError('Value must be a finite number');
      return;
    }
    if (paramType === 'int' && !Number.isInteger(parsed)) {
      setError('Value must be an integer');
      return;
    }
    const constraintError = getNumericConstraintError(parsed, constraints);
    if (constraintError) {
      setError(constraintError);
      return;
    }
    setError(null);
    onCommit(parsed);
  };

  return (
    <div>
      <input
        type="text"
        inputMode="decimal"
        value={draft}
        onChange={(e) => {
          const nextDraft = e.target.value;
          setDraft(nextDraft);
          tryCommit(nextDraft);
        }}
        onBlur={() => tryCommit(draft)}
        disabled={isRunMode}
        className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      />
      {error && <div className="mt-1 text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

function PoseMapEditor({
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

function TemplateVariableMapEditor({
  currentValue,
  isRunMode,
  onCommit,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: Record<string, string>) => void;
}) {
  const [rows, setRows] = useState<TemplateVariableRow[]>(() => normalizeTemplateVariableRows(currentValue));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setRows(normalizeTemplateVariableRows(currentValue));
    setError(null);
  }, [currentValue]);

  const validateAndCommit = useCallback((nextRows: TemplateVariableRow[]) => {
    const normalizedRows = nextRows
      .map((row) => ({
        ...row,
        name: row.name,
        path: row.path,
      }));
    setRows(normalizedRows);

    const seen = new Set<string>();
    for (const row of normalizedRows) {
      const trimmedName = row.name.trim();
      const trimmedPath = row.path.trim();
      if (!trimmedName) {
        setError('Variable names must not be empty');
        return;
      }
      if (!trimmedPath) {
        setError('Blackboard paths must not be empty');
        return;
      }
      if (seen.has(trimmedName)) {
        setError(`Duplicate variable name: ${trimmedName}`);
        return;
      }
      seen.add(trimmedName);
    }

    setError(null);
    onCommit(
      Object.fromEntries(
        normalizedRows.map((row) => [row.name.trim(), row.path.trim()])
      ),
    );
  }, [onCommit]);

  return (
    <div className="space-y-2">
      {rows.length === 0 && (
        <div className="text-xs text-gray-500">No template variables configured</div>
      )}
      {rows.map((row) => (
        <div key={row.id} className="grid grid-cols-[1fr_1fr_auto] gap-2">
          <input
            type="text"
            value={row.name}
            onChange={(e) => {
              const nextRows = rows.map((candidate) =>
                candidate.id === row.id
                  ? { ...candidate, name: e.target.value }
                  : candidate
              );
              validateAndCommit(nextRows);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
            placeholder="variable name"
          />
          <input
            type="text"
            value={row.path}
            onChange={(e) => {
              const nextRows = rows.map((candidate) =>
                candidate.id === row.id
                  ? { ...candidate, path: e.target.value }
                  : candidate
              );
              validateAndCommit(nextRows);
            }}
            disabled={isRunMode}
            className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
            placeholder="blackboard/path"
          />
          <button
            type="button"
            onClick={() => validateAndCommit(rows.filter((candidate) => candidate.id !== row.id))}
            disabled={isRunMode}
            className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
            title="Remove variable"
          >
            Remove
          </button>
        </div>
      ))}
      <button
        type="button"
        onClick={() => {
          const nextRows = [
            ...rows,
            {
              id: nextTemplateVariableRowId(),
              name: `var_${rows.length + 1}`,
              path: '',
            },
          ];
          setRows(nextRows);
          setError('Blackboard paths must not be empty');
        }}
        disabled={isRunMode}
        className={`rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 ${isRunMode ? 'opacity-50 cursor-not-allowed' : ''}`}
      >
        Add variable
      </button>
      {error && <div className="text-[11px] text-red-400">{error}</div>}
    </div>
  );
}

function DeploymentModelEndpointAliasEditor({
  currentValue,
  isRunMode,
  onCommit,
  deployment,
  modelEndpointCatalog,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: string) => void;
  deployment: FullDeployment | null;
  modelEndpointCatalog: ModelEndpointCatalog | null;
}) {
  const modelEndpoints = deployment?.modelEndpoints ?? {};
  const aliases = Object.keys(modelEndpoints);
  const currentAlias = String(currentValue ?? '');
  const options = aliases.includes(currentAlias) || !currentAlias
    ? aliases
    : [currentAlias, ...aliases];
  const endpoint = currentAlias ? modelEndpoints[currentAlias] : undefined;
  const probe = currentAlias ? modelEndpointCatalog?.endpoints?.[currentAlias] : undefined;
  const hasSingleConfiguredAlias = aliases.length === 1;
  const singleConfiguredAlias = hasSingleConfiguredAlias ? aliases[0] : '';
  const canRenderReadOnlySingleAlias =
    hasSingleConfiguredAlias && (!currentAlias || currentAlias === singleConfiguredAlias);
  const resolvedEndpoint = canRenderReadOnlySingleAlias
    ? modelEndpoints[singleConfiguredAlias]
    : endpoint;
  const resolvedProbe = canRenderReadOnlySingleAlias
    ? modelEndpointCatalog?.endpoints?.[singleConfiguredAlias]
    : probe;

  if (options.length === 0) {
    return (
      <div className="space-y-2">
        <input
          type="text"
          value={currentAlias}
          onChange={(e) => onCommit(e.target.value)}
          disabled={isRunMode}
          className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
        />
        <div className="text-[11px] text-gray-500">
          No deployment model endpoints are loaded in Composer.
        </div>
      </div>
    );
  }

  if (canRenderReadOnlySingleAlias && resolvedEndpoint) {
    return (
      <div className="space-y-2">
        <div className="rounded border border-gray-700 bg-gray-900/60 p-2 text-[11px] text-gray-300">
          <div><span className="text-gray-500">Resolved alias:</span> {singleConfiguredAlias}</div>
          <div><span className="text-gray-500">Provider:</span> {resolvedEndpoint.provider}</div>
          <div><span className="text-gray-500">Configured model:</span> {resolvedEndpoint.modelId}</div>
          <div><span className="text-gray-500">Base URL:</span> {resolvedEndpoint.baseUrl ?? '<unset>'}</div>
          <div><span className="text-gray-500">Live probe:</span> {resolvedProbe?.probeStatus ?? 'not loaded'}</div>
          {resolvedProbe?.availableModelIds && resolvedProbe.availableModelIds.length > 0 && (
            <div><span className="text-gray-500">Available models:</span> {resolvedProbe.availableModelIds.join(', ')}</div>
          )}
          {resolvedProbe?.error && (
            <div className="text-red-400"><span className="text-gray-500">Probe error:</span> {resolvedProbe.error}</div>
          )}
        </div>
        <div className="text-[11px] text-gray-500">
          Only one deployment model endpoint is configured, so there is no node-level choice here.
        </div>
      </div>
    );
  }

  return (
    <div className="space-y-2">
      <select
        value={currentAlias}
        onChange={(e) => onCommit(e.target.value)}
        disabled={isRunMode}
        className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      >
        {options.map((alias) => (
          <option key={alias} value={alias}>
            {alias}
          </option>
        ))}
      </select>
      {endpoint && (
        <div className="rounded border border-gray-700 bg-gray-900/60 p-2 text-[11px] text-gray-300">
          <div><span className="text-gray-500">Provider:</span> {endpoint.provider}</div>
          <div><span className="text-gray-500">Configured model:</span> {endpoint.modelId}</div>
          <div><span className="text-gray-500">Base URL:</span> {endpoint.baseUrl ?? '<unset>'}</div>
          <div><span className="text-gray-500">Live probe:</span> {probe?.probeStatus ?? 'not loaded'}</div>
          {probe?.availableModelIds && probe.availableModelIds.length > 0 && (
            <div><span className="text-gray-500">Available models:</span> {probe.availableModelIds.join(', ')}</div>
          )}
          {probe?.error && (
            <div className="text-red-400"><span className="text-gray-500">Probe error:</span> {probe.error}</div>
          )}
        </div>
      )}
    </div>
  );
}

function DeploymentPromptTemplateEditor({
  currentValue,
  isRunMode,
  onCommit,
  deployment,
}: {
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (value: string) => void;
  deployment: FullDeployment | null;
}) {
  const promptTemplates = deployment?.promptTemplates ?? {};
  const templateIds = Object.keys(promptTemplates);
  const currentTemplateId = String(currentValue ?? '');
  const options = templateIds.includes(currentTemplateId) || !currentTemplateId
    ? templateIds
    : [currentTemplateId, ...templateIds];
  const template = currentTemplateId ? promptTemplates[currentTemplateId] : undefined;
  const hasSingleConfiguredTemplate = templateIds.length === 1;
  const singleConfiguredTemplate = hasSingleConfiguredTemplate ? templateIds[0] : '';
  const canRenderReadOnlySingleTemplate =
    hasSingleConfiguredTemplate && (!currentTemplateId || currentTemplateId === singleConfiguredTemplate);
  const resolvedTemplate = canRenderReadOnlySingleTemplate
    ? promptTemplates[singleConfiguredTemplate]
    : template;

  if (options.length === 0) {
    return (
      <div className="space-y-2">
        <input
          type="text"
          value={currentTemplateId}
          onChange={(e) => onCommit(e.target.value)}
          disabled={isRunMode}
          className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
        />
        <div className="text-[11px] text-gray-500">
          No deployment prompt templates are loaded in Composer.
        </div>
      </div>
    );
  }

  if (canRenderReadOnlySingleTemplate && resolvedTemplate) {
    return (
      <div className="space-y-2">
        <div className="rounded border border-gray-700 bg-gray-900/60 p-2 text-[11px] text-gray-300">
          <div><span className="text-gray-500">Resolved template:</span> {singleConfiguredTemplate}</div>
          <div><span className="text-gray-500">Version:</span> {resolvedTemplate.version}</div>
          <div>
            <span className="text-gray-500">Output mode:</span>{' '}
            {resolvedTemplate.responseTool ? 'response tool' : resolvedTemplate.outputSchema ? 'json schema' : 'unconstrained'}
          </div>
        </div>
        <div className="text-[11px] text-gray-500">
          Only one deployment prompt template is configured, so there is no node-level choice here.
        </div>
      </div>
    );
  }

  return (
    <div className="space-y-2">
      <select
        value={currentTemplateId}
        onChange={(e) => onCommit(e.target.value)}
        disabled={isRunMode}
        className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
      >
        {options.map((templateId) => (
          <option key={templateId} value={templateId}>
            {templateId}
          </option>
        ))}
      </select>
      {template && (
        <div className="rounded border border-gray-700 bg-gray-900/60 p-2 text-[11px] text-gray-300">
          <div><span className="text-gray-500">Version:</span> {template.version}</div>
          <div>
            <span className="text-gray-500">Output mode:</span>{' '}
            {template.responseTool ? 'response tool' : template.outputSchema ? 'json schema' : 'unconstrained'}
          </div>
        </div>
      )}
    </div>
  );
}

function StructuredParamEditor({
  paramName,
  paramType,
  currentValue,
  isRunMode,
  onCommit,
}: {
  paramName: string;
  paramType: string;
  currentValue: unknown;
  isRunMode: boolean;
  onCommit: (paramName: string, value: unknown) => void;
}) {
  const [draft, setDraft] = useState(() => formatStructuredValue(currentValue));
  const [error, setError] = useState<string | null>(null);

  useEffect(() => {
    setDraft(formatStructuredValue(currentValue));
    setError(null);
  }, [currentValue]);

  const disabledClasses = isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : '';

  const commit = useCallback(() => {
    if (isRunMode) {
      return;
    }
    const trimmed = draft.trim();
    if (trimmed === '') {
      setError('Value must not be empty');
      return;
    }
    try {
      const parsed = parseStructuredValue(paramType, trimmed);
      setError(null);
      onCommit(paramName, parsed);
    } catch (err) {
      setError(err instanceof Error ? err.message : 'Invalid JSON value');
    }
  }, [draft, isRunMode, onCommit, paramName, paramType]);

  return (
    <div>
      <textarea
        value={draft}
        onChange={(e) => {
          setDraft(e.target.value);
          if (error) {
            setError(null);
          }
        }}
        onBlur={commit}
        disabled={isRunMode}
        rows={8}
        spellCheck={false}
        className={`w-full resize-y rounded border border-gray-600 bg-gray-700 px-2 py-1 font-mono text-xs text-white ${disabledClasses}`}
      />
      <div className="mt-1 flex items-center justify-between">
        <span className="text-[11px] text-gray-500">JSON</span>
        {error && <span className="text-[11px] text-red-400">{error}</span>}
      </div>
    </div>
  );
}

export function PropertyPanel({ onCollapse }: { onCollapse: () => void }) {
  const { selectedNodeId, nodes, updateNodeParams, updateNodeLabel, removeNode } = useProgramStore();
  const { entries } = useCatalogStore();
  const cycleState = useCycleStore((state) => state.state);
  const deployment = useDeploymentStore((state) => state.deployment);
  const selectedNode = selectedNodeId ? nodes.find(n => n.id === selectedNodeId) : undefined;
  const catalogEntry = selectedNode ? entries.find(e => e.id === selectedNode.data.catalogId) : undefined;
  const colorClass = selectedNode ? (KIND_COLORS[selectedNode.data.catalogId as string] || KIND_COLORS.default) : KIND_COLORS.default;
  const nodeParams = (selectedNode?.data.params as Record<string, unknown> | undefined) ?? {};
  const isRunMode = cycleState === 'running';
  const [modelEndpointCatalog, setModelEndpointCatalog] = useState<ModelEndpointCatalog | null>(null);
  const deploymentModelEndpointSignature = JSON.stringify(deployment?.modelEndpoints ?? {});

  useEffect(() => {
    if (!deployment || Object.keys(deployment.modelEndpoints ?? {}).length === 0) {
      setModelEndpointCatalog(null);
      return;
    }
    let cancelled = false;
    void api.infra.modelEndpoints()
      .then((result) => {
        if (!cancelled) {
          setModelEndpointCatalog(result);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setModelEndpointCatalog(null);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [deploymentModelEndpointSignature, deployment?.sourcePath]);

  const handleDelete = useCallback(() => {
    if (!selectedNodeId) return;
    removeNode(selectedNodeId);
  }, [selectedNodeId, removeNode]);

  const handleLabelChange = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      if (isRunMode || !selectedNodeId) return;
      updateNodeLabel(selectedNodeId, e.target.value);
    },
    [selectedNodeId, updateNodeLabel, isRunMode],
  );

  const handleParamChange = useCallback(
    (paramName: string, value: unknown) => {
      if (isRunMode || !selectedNodeId) return;
      updateNodeParams(selectedNodeId, { ...nodeParams, [paramName]: value });
    },
    [selectedNodeId, updateNodeParams, nodeParams, isRunMode],
  );

  if (!selectedNodeId) {
    return (
      <div className="relative flex h-full w-full flex-col items-center justify-center bg-gray-800">
        <button
          type="button"
          onClick={onCollapse}
          className="absolute right-2 top-2 rounded bg-gray-700 px-2 py-1 text-sm font-medium text-gray-200 hover:bg-gray-600"
          title="Collapse properties"
        >
          ›
        </button>
        <div className="text-gray-500 text-sm text-center px-4">
          <div className="text-2xl mb-2">←</div>
          <p>Select a node to view and edit its properties</p>
        </div>
      </div>
    );
  }

  if (!selectedNode) {
    return (
      <div className="relative flex h-full w-full flex-col items-center justify-center bg-gray-800">
        <button
          type="button"
          onClick={onCollapse}
          className="absolute right-2 top-2 rounded bg-gray-700 px-2 py-1 text-sm font-medium text-gray-200 hover:bg-gray-600"
          title="Collapse properties"
        >
          ›
        </button>
        <div className="text-gray-500 text-sm text-center px-4">
          <p>Node not found</p>
        </div>
      </div>
    );
  }

  const renderParameterInput = (param: {
    name: string;
    type: string;
    required: boolean;
    default: unknown;
    enumValues: string[];
    constraints: Record<string, unknown>;
    uiEditor?: string | null;
    description: string;
  }) => {
    const currentValue = nodeParams[param.name] ?? param.default;

    const disabledClasses = isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : '';

    if (param.uiEditor === 'deployment_model_endpoint_alias') {
      return (
        <DeploymentModelEndpointAliasEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          deployment={deployment}
          modelEndpointCatalog={modelEndpointCatalog}
        />
      );
    }

    if (param.uiEditor === 'deployment_prompt_template_id') {
      return (
        <DeploymentPromptTemplateEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          deployment={deployment}
        />
      );
    }

    if (param.uiEditor === 'template_variable_map') {
      return (
        <TemplateVariableMapEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
        />
      );
    }

    switch (param.type) {
      case 'pose':
        return (
          <PoseEditor
            currentValue={currentValue}
            isRunMode={isRunMode}
            onCommit={(value) => handleParamChange(param.name, value)}
          />
        );

      case 'string':
        return (
          <input
            type="text"
            value={String(currentValue) || ''}
            onChange={(e) => handleParamChange(param.name, e.target.value)}
            disabled={isRunMode}
            className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${disabledClasses}`}
            placeholder={param.description}
          />
        );

      case 'float':
      case 'int':
        return (
          <NumericParamEditor
            currentValue={currentValue}
            isRunMode={isRunMode}
            onCommit={(value) => handleParamChange(param.name, value)}
            paramType={param.type}
            constraints={param.constraints}
          />
        );

      case 'bool':
        return (
          <input
            type="checkbox"
            checked={!!currentValue}
            onChange={(e) => handleParamChange(param.name, e.target.checked)}
            disabled={isRunMode}
            className={`w-4 h-4 bg-gray-700 border-gray-600 text-white rounded ${disabledClasses}`}
          />
        );

      case 'enum':
        return (
          <select
            value={String(currentValue) || ''}
            onChange={(e) => handleParamChange(param.name, e.target.value)}
            disabled={isRunMode}
            className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${disabledClasses}`}
          >
            {param.enumValues.map((value) => (
              <option key={value} value={value}>
                {value}
              </option>
            ))}
          </select>
        );

      case 'dict':
      case 'object':
      case 'float_list':
      case 'string_list':
        if (param.uiEditor === 'pose_envelope') {
          return (
            <PoseEnvelopeEditor
              currentValue={currentValue}
              isRunMode={isRunMode}
              onCommit={(value) => handleParamChange(param.name, value)}
            />
          );
        }
        if (param.uiEditor === 'pose_map') {
          return (
            <PoseMapEditor
              currentValue={currentValue}
              isRunMode={isRunMode}
              onCommit={(value) => handleParamChange(param.name, value)}
            />
          );
        }
        return (
          <StructuredParamEditor
            paramName={param.name}
            paramType={param.type}
            currentValue={currentValue}
            isRunMode={isRunMode}
            onCommit={handleParamChange}
          />
        );

      default:
        return (
          <input
            type="text"
            value={String(currentValue) || ''}
            onChange={(e) => handleParamChange(param.name, e.target.value)}
            disabled={isRunMode}
            className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${disabledClasses}`}
            placeholder={param.description}
          />
        );
    }
  };

  return (
    <div className="flex h-full w-full flex-col overflow-hidden bg-gray-800">
      {/* Read-only banner in run mode */}
      {isRunMode && (
        <div className="bg-yellow-900/50 border-b border-yellow-700 px-3 py-2">
          <div className="text-xs text-yellow-400 font-medium flex items-center gap-1">
            <span>⚠</span>
            Read-only (program running)
          </div>
        </div>
      )}

      {/* Header */}
      <div className={`p-3 border-b border-gray-700 ${colorClass}`}>
        <div className="flex items-center justify-between mb-2">
          <span className="text-xs text-gray-400 uppercase">{selectedNode.data.catalogId as string}</span>
          <div className="flex items-center gap-2">
            {!isRunMode && (
              <button
                onClick={handleDelete}
                className="text-red-400 hover:text-red-300 font-bold text-lg"
                title="Delete node"
              >
                ×
              </button>
            )}
            <button
              type="button"
              onClick={onCollapse}
              className="rounded bg-gray-700 px-2 py-1 text-sm font-medium text-gray-200 hover:bg-gray-600"
              title="Collapse properties"
            >
              ›
            </button>
          </div>
        </div>
        <input
          type="text"
          value={selectedNode.data.label as string}
          onChange={handleLabelChange}
          disabled={isRunMode}
          className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm font-medium ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
        />
      </div>

      {/* Node Info */}
      {catalogEntry && (
        <div className="p-3 border-b border-gray-700">
          <div className="text-sm text-gray-300 mb-1">{catalogEntry.name}</div>
          <div className="text-xs text-gray-500">{catalogEntry.description}</div>
        </div>
      )}

      {/* Parameters */}
      {catalogEntry && catalogEntry.parameters.length > 0 && (
        <div className="flex-1 overflow-y-auto p-3">
          <h3 className="text-xs text-gray-400 uppercase mb-2">Parameters</h3>
          <div className="space-y-3">
            {catalogEntry.parameters.map((param) => (
              <div key={param.name}>
                <div className="flex items-center justify-between mb-1">
                  <label className="text-xs text-gray-300">
                    {param.name}
                    {param.required && <span className="text-red-400 ml-1">*</span>}
                  </label>
                  <span className="text-xs text-gray-500">{param.type}</span>
                </div>
                {param.description && (
                  <div className="text-xs text-gray-500 mb-1">{param.description}</div>
                )}
                {renderParameterInput(param)}
              </div>
            ))}
          </div>
        </div>
      )}
    </div>
  );
}

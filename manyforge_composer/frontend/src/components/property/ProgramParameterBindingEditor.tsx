import { useState } from 'react';
import type { ReactNode } from 'react';

import type { ProgramParameter } from '../../types/api';

const PROGRAM_PARAMETER_REFERENCE_RE = /^\$\{param:([^}]+)\}$/;

export const SUPPORTED_PROGRAM_PARAMETER_TYPES = new Set(['string', 'float', 'int', 'bool']);

export function getProgramParameterReferenceName(value: unknown): string | null {
  if (typeof value !== 'string') {
    return null;
  }
  const match = PROGRAM_PARAMETER_REFERENCE_RE.exec(value);
  return match ? match[1] : null;
}

function formatProgramParameterValue(value: unknown): string {
  if (typeof value === 'string') {
    return value;
  }
  if (typeof value === 'number' || typeof value === 'boolean') {
    return String(value);
  }
  try {
    return JSON.stringify(value);
  } catch {
    return String(value);
  }
}

function resolveProgramParameterLiteralValue(
  currentValue: unknown,
  fallbackValue: unknown,
  programParameters: ProgramParameter[],
): unknown {
  const referenceName = getProgramParameterReferenceName(currentValue);
  if (!referenceName) {
    return currentValue ?? fallbackValue;
  }
  const boundParameter = programParameters.find((parameter) => parameter.name === referenceName);
  return boundParameter?.default ?? fallbackValue;
}

export function ParameterBindingEditor({
  paramName,
  paramType,
  paramDefault,
  constraints,
  currentValue,
  programParameters,
  expanded,
  isRunMode,
  onCommitLiteral,
  onBindProgramParameter,
  onCreateProgramParameter,
  children,
}: {
  paramName: string;
  paramType: string;
  paramDefault: unknown;
  constraints: Record<string, unknown>;
  currentValue: unknown;
  programParameters: ProgramParameter[];
  expanded: boolean;
  isRunMode: boolean;
  onCommitLiteral: (value: unknown) => void;
  onBindProgramParameter: (parameterName: string) => void;
  onCreateProgramParameter: (parameter: ProgramParameter) => void;
  children: ReactNode;
}) {
  const [creating, setCreating] = useState(false);
  const [newParameterName, setNewParameterName] = useState('');
  const [newParameterDescription, setNewParameterDescription] = useState('');
  const [creationError, setCreationError] = useState<string | null>(null);

  const referenceName = getProgramParameterReferenceName(currentValue);
  const compatibleParameters = programParameters.filter((parameter) => parameter.type === paramType);
  const boundParameter = referenceName
    ? compatibleParameters.find((parameter) => parameter.name === referenceName) ?? null
    : null;
  const literalValue = resolveProgramParameterLiteralValue(
    currentValue,
    paramDefault,
    programParameters,
  );
  const hasMissingBinding = !!referenceName && !boundParameter;

  const sourceValue = creating
    ? '__new__'
    : boundParameter
      ? `param:${boundParameter.name}`
      : hasMissingBinding
        ? '__missing__'
        : 'literal';

  const cancelCreate = () => {
    setCreating(false);
    setNewParameterName('');
    setNewParameterDescription('');
    setCreationError(null);
  };

  const createParameter = () => {
    const name = newParameterName.trim();
    if (!name) {
      setCreationError('Parameter name is required');
      return;
    }
    if (programParameters.some((parameter) => parameter.name === name)) {
      setCreationError(`Program parameter '${name}' already exists`);
      return;
    }
    onCreateProgramParameter({
      name,
      type: paramType,
      default: literalValue,
      constraints,
      description: newParameterDescription.trim() || `Program parameter for ${paramName}`,
    });
    onBindProgramParameter(name);
    cancelCreate();
  };

  if (!expanded) {
    return (
      <div className="space-y-2">
        {!boundParameter && !hasMissingBinding && !creating && children}
        {boundParameter && (
          <div className="text-[11px] text-blue-300">
            Bound to program parameter <span className="font-medium text-blue-100">{boundParameter.name}</span>.
          </div>
        )}
        {hasMissingBinding && !creating && (
          <div className="text-[11px] text-red-300">
            Missing program parameter <span className="font-medium text-red-100">{referenceName}</span>.
          </div>
        )}
      </div>
    );
  }

  return (
    <div className="space-y-2">
      <div className="ml-auto max-w-xs rounded border border-gray-700 bg-gray-900/95 p-2 shadow-lg shadow-black/20">
        <div className="mb-2 flex items-center justify-between gap-2">
          <div className="text-[11px] font-semibold uppercase tracking-wide text-gray-400">
            Program Parameter
          </div>
          {boundParameter && (
            <div className="truncate text-[11px] text-blue-300">{boundParameter.name}</div>
          )}
        </div>
        {boundParameter && (
          <div className="mb-2 text-[11px] text-blue-400">
            {boundParameter.name} = {formatProgramParameterValue(boundParameter.default)}
          </div>
        )}
        <select
          value={sourceValue}
          disabled={isRunMode}
          onChange={(event) => {
            const nextValue = event.target.value;
            setCreationError(null);
            if (nextValue === 'literal') {
              cancelCreate();
              onCommitLiteral(literalValue);
              return;
            }
            if (nextValue === '__new__') {
              setCreating(true);
              if (!newParameterName) {
                setNewParameterName(paramName);
              }
              return;
            }
            if (nextValue.startsWith('param:')) {
              cancelCreate();
              onBindProgramParameter(nextValue.slice('param:'.length));
            }
          }}
          className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
        >
          <option value="literal">Literal value</option>
          {compatibleParameters.map((parameter) => (
            <option key={parameter.name} value={`param:${parameter.name}`}>
              Use {parameter.name}
            </option>
          ))}
          <option value="__new__">Create new…</option>
          {hasMissingBinding && (
            <option value="__missing__" disabled>
              Missing parameter: {referenceName}
            </option>
          )}
        </select>
        <div className="mt-1 text-[11px] text-gray-500">
          Shared authored config only.
        </div>
      </div>

      {creating && (
        <div className="ml-auto max-w-xs rounded border border-blue-900/70 bg-blue-950/30 p-3 shadow-lg shadow-black/20">
          <div className="mb-2 text-[11px] font-semibold uppercase tracking-wide text-blue-300">
            Create Program Parameter
          </div>
          <div className="space-y-2">
            <div>
              <label className="mb-1 block text-[11px] text-gray-400">Name</label>
              <input
                type="text"
                value={newParameterName}
                onChange={(event) => {
                  setNewParameterName(event.target.value);
                  if (creationError) {
                    setCreationError(null);
                  }
                }}
                disabled={isRunMode}
                className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
              />
            </div>
            <div>
              <label className="mb-1 block text-[11px] text-gray-400">Description</label>
              <input
                type="text"
                value={newParameterDescription}
                onChange={(event) => setNewParameterDescription(event.target.value)}
                disabled={isRunMode}
                className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
                placeholder={`Program parameter for ${paramName}`}
              />
            </div>
            <div className="text-[11px] text-gray-500">
              Initial value: <span className="text-gray-200">{formatProgramParameterValue(literalValue)}</span>
            </div>
            {creationError && <div className="text-[11px] text-red-400">{creationError}</div>}
          </div>
          <div className="mt-3 flex justify-end gap-2">
            <button
              type="button"
              onClick={cancelCreate}
              className="rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700"
            >
              Cancel
            </button>
            <button
              type="button"
              onClick={createParameter}
              className="rounded border border-blue-500 bg-blue-900/60 px-2 py-1 text-xs text-blue-100 hover:bg-blue-800"
            >
              Create and bind
            </button>
          </div>
        </div>
      )}

      {!creating && !boundParameter && !hasMissingBinding && children}
      {boundParameter && children}
      {hasMissingBinding && children}
    </div>
  );
}

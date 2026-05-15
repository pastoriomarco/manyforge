import { useEffect, useMemo, useRef, useState } from 'react';
import type { Node } from '@xyflow/react';
import { useShallow } from 'zustand/react/shallow';
import { useCatalogStore } from '../store/catalogStore';
import { useCycleStore } from '../store/cycleStore';
import { useProgramStore } from '../store/programStore';
import type {
  CatalogEntry,
  CatalogParameter,
  ProgramParameter,
} from '../types/api';
import { DraftTextInput } from './common/DraftInputs';

interface UsageRecord {
  nodeId: string;
  nodeLabel: string;
  paramPath: string;
}

const PARAM_REFERENCE_RE = /\$\{param:([^}]+)\}/g;

function signature(value: unknown): string {
  return JSON.stringify(value ?? null);
}

function getNodeLabel(node: Node): string {
  const data = node.data as { label?: string; catalogId?: string };
  return String(data.label || data.catalogId || node.id);
}

function appendPath(base: string, segment: string): string {
  if (!base) {
    return segment;
  }
  return segment.startsWith('[') ? `${base}${segment}` : `${base}.${segment}`;
}

function buildCatalogParamIndex(entries: CatalogEntry[]): Map<string, Map<string, CatalogParameter>> {
  return new Map(
    entries.map((entry) => [
      entry.id,
      new Map(entry.parameters.map((parameter) => [parameter.name, parameter])),
    ]),
  );
}

function collectParameterUsages(nodes: Node[]): Record<string, UsageRecord[]> {
  const usageIndex = new Map<string, Map<string, UsageRecord>>();

  const recordUsage = (parameterName: string, usage: UsageRecord) => {
    if (!usageIndex.has(parameterName)) {
      usageIndex.set(parameterName, new Map());
    }
    usageIndex.get(parameterName)!.set(`${usage.nodeId}:${usage.paramPath}`, usage);
  };

  const scanValue = (value: unknown, path: string, node: Node) => {
    if (typeof value === 'string') {
      for (const match of value.matchAll(PARAM_REFERENCE_RE)) {
        const parameterName = match[1];
        if (parameterName) {
          recordUsage(parameterName, {
            nodeId: node.id,
            nodeLabel: getNodeLabel(node),
            paramPath: path,
          });
        }
      }
      return;
    }
    if (Array.isArray(value)) {
      value.forEach((item, index) => {
        scanValue(item, appendPath(path, `[${index}]`), node);
      });
      return;
    }
    if (value && typeof value === 'object') {
      Object.entries(value as Record<string, unknown>).forEach(([key, nested]) => {
        scanValue(nested, appendPath(path, key), node);
      });
    }
  };

  nodes.forEach((node) => {
    const params = (node.data as { params?: Record<string, unknown> }).params ?? {};
    Object.entries(params).forEach(([key, value]) => {
      scanValue(value, key, node);
    });
  });

  return Object.fromEntries(
    Array.from(usageIndex.entries()).map(([parameterName, usages]) => [
      parameterName,
      Array.from(usages.values()).sort((left, right) => {
        if (left.nodeLabel === right.nodeLabel) {
          return left.paramPath.localeCompare(right.paramPath);
        }
        return left.nodeLabel.localeCompare(right.nodeLabel);
      }),
    ]),
  );
}

function collectBlackboardUsages(
  nodes: Node[],
  entries: CatalogEntry[],
): {
  namespaceUsages: Record<string, UsageRecord[]>;
  keyUsages: Record<string, UsageRecord[]>;
  fieldUsages: Record<string, UsageRecord[]>;
} {
  const catalogIndex = buildCatalogParamIndex(entries);
  const namespaceUsageIndex = new Map<string, Map<string, UsageRecord>>();
  const keyUsageIndex = new Map<string, Map<string, UsageRecord>>();
  const fieldUsageIndex = new Map<string, Map<string, UsageRecord>>();

  const recordUsage = (
    index: Map<string, Map<string, UsageRecord>>,
    identifier: string,
    usage: UsageRecord,
  ) => {
    if (!index.has(identifier)) {
      index.set(identifier, new Map());
    }
    index.get(identifier)!.set(`${usage.nodeId}:${usage.paramPath}`, usage);
  };

  nodes.forEach((node) => {
    const catalogId =
      typeof node.data?.catalogId === 'string' ? String(node.data.catalogId) : '';
    const entryIndex = catalogIndex.get(catalogId);
    if (!entryIndex) {
      return;
    }
    const params =
      node.data?.params && typeof node.data.params === 'object'
        ? (node.data.params as Record<string, unknown>)
        : {};
    for (const [paramName, rawValue] of Object.entries(params)) {
      const param = entryIndex.get(paramName);
      if (!param) {
        continue;
      }
      const usage: UsageRecord = {
        nodeId: String(node.id),
        nodeLabel: getNodeLabel(node),
        paramPath: paramName,
      };
      const fieldRef =
        rawValue
        && typeof rawValue === 'object'
        && !Array.isArray(rawValue)
        && (rawValue as Record<string, unknown>).field_ref
        && typeof (rawValue as Record<string, unknown>).field_ref === 'object'
          ? (rawValue as Record<string, { namespace?: unknown; field?: unknown }>).field_ref
          : null;
      const fieldRefNamespace =
        fieldRef && typeof fieldRef.namespace === 'string' ? fieldRef.namespace : null;
      const fieldRefName =
        fieldRef && typeof fieldRef.field === 'string' ? fieldRef.field : null;
      if (
        (param.blackboardRole === 'namespace_writer' || param.blackboardRole === 'namespace_reader')
      ) {
        if (
          rawValue
          && typeof rawValue === 'object'
          && !Array.isArray(rawValue)
          && typeof (rawValue as Record<string, unknown>).namespace_ref === 'string'
        ) {
          recordUsage(
            namespaceUsageIndex,
            String((rawValue as Record<string, unknown>).namespace_ref),
            usage,
          );
        }
      }
      if (fieldRefNamespace && fieldRefName) {
        recordUsage(namespaceUsageIndex, fieldRefNamespace, usage);
        recordUsage(fieldUsageIndex, `${fieldRefNamespace}.${fieldRefName}`, usage);
      }
      if (
        (param.blackboardRole === 'key_writer' || param.blackboardRole === 'key_reader')
      ) {
        if (
          rawValue
          && typeof rawValue === 'object'
          && !Array.isArray(rawValue)
          && typeof (rawValue as Record<string, unknown>).key_ref === 'string'
        ) {
          recordUsage(
            keyUsageIndex,
            String((rawValue as Record<string, unknown>).key_ref),
            usage,
          );
        }
      }
    }
  });

  return {
    namespaceUsages: Object.fromEntries(
      Array.from(namespaceUsageIndex.entries()).map(([identifier, usages]) => [
        identifier,
        Array.from(usages.values()).sort((left, right) => {
          if (left.nodeLabel === right.nodeLabel) {
            return left.paramPath.localeCompare(right.paramPath);
          }
          return left.nodeLabel.localeCompare(right.nodeLabel);
        }),
      ]),
    ),
    keyUsages: Object.fromEntries(
      Array.from(keyUsageIndex.entries()).map(([identifier, usages]) => [
        identifier,
        Array.from(usages.values()).sort((left, right) => {
          if (left.nodeLabel === right.nodeLabel) {
            return left.paramPath.localeCompare(right.paramPath);
          }
          return left.nodeLabel.localeCompare(right.nodeLabel);
        }),
      ]),
    ),
    fieldUsages: Object.fromEntries(
      Array.from(fieldUsageIndex.entries()).map(([identifier, usages]) => [
        identifier,
        Array.from(usages.values()).sort((left, right) => {
          if (left.nodeLabel === right.nodeLabel) {
            return left.paramPath.localeCompare(right.paramPath);
          }
          return left.nodeLabel.localeCompare(right.nodeLabel);
        }),
      ]),
    ),
  };
}

const BLACKBOARD_FIELD_TYPES = [
  'string',
  'float',
  'int',
  'bool',
  'pose',
  'float_list',
  'string_list',
  'dict',
] as const;

function getConstraintError(value: number, constraints: Record<string, unknown>): string | null {
  const minInclusive =
    typeof constraints.min_inclusive === 'number' ? constraints.min_inclusive : undefined;
  const minExclusive =
    typeof constraints.min_exclusive === 'number' ? constraints.min_exclusive : undefined;
  const maxInclusive =
    typeof constraints.max_inclusive === 'number' ? constraints.max_inclusive : undefined;
  const maxExclusive =
    typeof constraints.max_exclusive === 'number' ? constraints.max_exclusive : undefined;

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

function formatDefaultValue(value: unknown): string {
  if (typeof value === 'string') {
    return value;
  }
  if (typeof value === 'number' || typeof value === 'boolean') {
    return String(value);
  }
  return JSON.stringify(value ?? null, null, 2);
}

function validateParameterDraft(
  parameter: ProgramParameter,
  draft: string,
): { value?: unknown; error?: string } {
  const trimmed = draft.trim();
  if (parameter.type === 'string') {
    return { value: draft };
  }
  if (parameter.type === 'float') {
    if (!trimmed) {
      return { error: 'Value is required' };
    }
    const value = Number(draft);
    if (!Number.isFinite(value)) {
      return { error: 'Value must be a finite number' };
    }
    const constraintError = getConstraintError(value, parameter.constraints ?? {});
    if (constraintError) {
      return { error: constraintError };
    }
    return { value };
  }
  if (parameter.type === 'int') {
    if (!trimmed) {
      return { error: 'Value is required' };
    }
    const value = Number(draft);
    if (!Number.isFinite(value) || !Number.isInteger(value)) {
      return { error: 'Value must be an integer' };
    }
    const constraintError = getConstraintError(value, parameter.constraints ?? {});
    if (constraintError) {
      return { error: constraintError };
    }
    return { value };
  }
  try {
    return { value: JSON.parse(draft) };
  } catch {
    return { error: 'Value must be valid JSON' };
  }
}

function ConstraintSummary({ constraints }: { constraints: Record<string, unknown> }) {
  const entries = Object.entries(constraints ?? {}).filter(
    ([, value]) => value !== undefined && value !== null,
  );
  if (entries.length === 0) {
    return null;
  }
  return (
    <div className="mt-2 flex flex-wrap gap-1">
      {entries.map(([key, value]) => (
        <span
          key={key}
          className="rounded bg-gray-700 px-2 py-0.5 text-[11px] text-gray-300"
        >
          {key}: {String(value)}
        </span>
      ))}
    </div>
  );
}

function ParameterEditor({
  parameter,
  disabled,
  onCommit,
}: {
  parameter: ProgramParameter;
  disabled: boolean;
  onCommit: (value: unknown) => void;
}) {
  const [draft, setDraft] = useState(() => formatDefaultValue(parameter.default));
  const [error, setError] = useState<string | null>(null);
  const [isFocused, setIsFocused] = useState(false);
  // See ../common/DraftInputs.tsx for the rationale: skipNextCommit gates
  // the Escape→revert path so the synchronous blur after setDraft doesn't
  // commit the in-progress typed value.
  const skipNextCommit = useRef(false);

  useEffect(() => {
    if (isFocused) {
      return;
    }
    setDraft(formatDefaultValue(parameter.default));
    setError(null);
  }, [parameter.default, parameter.name, parameter.type, isFocused]);

  const validation = useMemo(() => validateParameterDraft(parameter, draft), [draft, parameter]);

  const commit = () => {
    if (parameter.type === 'bool') {
      return;
    }
    if (validation.error) {
      setError(validation.error);
      return;
    }
    setError(null);
    if (signature(validation.value) !== signature(parameter.default)) {
      onCommit(validation.value);
    }
  };

  const handleBlur = () => {
    setIsFocused(false);
    if (skipNextCommit.current) {
      skipNextCommit.current = false;
      setDraft(formatDefaultValue(parameter.default));
      setError(null);
      return;
    }
    commit();
  };

  const revertOnEscape = (event: React.KeyboardEvent<HTMLInputElement | HTMLTextAreaElement>) => {
    event.preventDefault();
    skipNextCommit.current = true;
    setDraft(formatDefaultValue(parameter.default));
    setError(null);
    event.currentTarget.blur();
  };

  if (parameter.type === 'bool') {
    return (
      <label className="mt-3 flex items-center gap-2 text-sm text-gray-200">
        <input
          type="checkbox"
          checked={Boolean(parameter.default)}
          disabled={disabled}
          onChange={(event) => {
            const nextValue = event.target.checked;
            if (nextValue !== Boolean(parameter.default)) {
              onCommit(nextValue);
            }
          }}
          className="h-4 w-4 rounded border-gray-500 bg-gray-700 text-blue-500 focus:ring-blue-500 disabled:cursor-not-allowed disabled:opacity-50"
        />
        <span>Enabled</span>
      </label>
    );
  }

  const isTextarea =
    parameter.type !== 'string' && parameter.type !== 'float' && parameter.type !== 'int';
  const inputClassName =
    'mt-2 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50';

  return (
    <div className="mt-3">
      {isTextarea ? (
        <textarea
          rows={4}
          value={draft}
          disabled={disabled}
          onFocus={() => setIsFocused(true)}
          onChange={(event) => setDraft(event.target.value)}
          onBlur={handleBlur}
          onKeyDown={(event) => {
            // Ctrl/Cmd+Enter commits; plain Enter is a newline inside
            // a JSON/structured textarea. Esc reverts.
            if ((event.ctrlKey || event.metaKey) && event.key === 'Enter') {
              event.preventDefault();
              event.currentTarget.blur();
              return;
            }
            if (event.key === 'Escape') {
              revertOnEscape(event);
            }
          }}
          className={`${inputClassName} font-mono text-xs`}
        />
      ) : (
        <input
          type="text"
          value={draft}
          disabled={disabled}
          onFocus={() => setIsFocused(true)}
          onChange={(event) => setDraft(event.target.value)}
          onBlur={handleBlur}
          onKeyDown={(event) => {
            if (event.key === 'Enter') {
              event.preventDefault();
              event.currentTarget.blur();
              return;
            }
            if (event.key === 'Escape') {
              revertOnEscape(event);
            }
          }}
          className={`${inputClassName} ${parameter.type === 'string' ? '' : 'font-mono'}`}
        />
      )}
      {(error || validation.error) && (
        <p className="mt-1 text-xs text-red-400">{error || validation.error}</p>
      )}
    </div>
  );
}

function nextAvailableName(prefix: string, taken: Set<string>): string {
  let index = 1;
  while (taken.has(`${prefix}_${index}`)) {
    index += 1;
  }
  return `${prefix}_${index}`;
}

function SectionHeading({
  title,
  count,
  onAdd,
  addLabel,
  disabled,
}: {
  title: string;
  count: number;
  onAdd?: () => void;
  addLabel?: string;
  disabled?: boolean;
}) {
  return (
    <div className="border-b border-gray-700 px-4 py-3">
      <div className="flex items-center justify-between gap-3">
        <div>
          <h2 className="text-sm font-semibold uppercase tracking-wide text-gray-400">{title}</h2>
          <div className="mt-1 text-xs text-gray-500">{count} entries</div>
        </div>
        {onAdd && (
          <button
            type="button"
            onClick={onAdd}
            disabled={disabled}
            className="rounded border border-gray-600 px-2 py-1 text-xs text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
          >
            {addLabel ?? 'Add'}
          </button>
        )}
      </div>
    </div>
  );
}

function UsageList({
  label,
  usages,
  onSelectNode,
}: {
  label: string;
  usages: UsageRecord[];
  onSelectNode: (nodeId: string) => void;
}) {
  return (
    <div className="mt-3 border-t border-gray-700 pt-3">
      <div className="mb-2 text-xs font-semibold uppercase tracking-wide text-gray-400">
        {label}
      </div>
      {usages.length === 0 ? (
        <p className="text-xs text-gray-500">Unused in the current draft tree.</p>
      ) : (
        <div className="space-y-2">
          {usages.map((usage) => (
            <button
              key={`${usage.nodeId}:${usage.paramPath}`}
              type="button"
              onClick={() => onSelectNode(usage.nodeId)}
              className="flex w-full items-center justify-between rounded border border-gray-700 bg-gray-800/80 px-3 py-2 text-left hover:border-blue-500"
            >
              <div>
                <div className="text-sm text-gray-100">{usage.nodeLabel}</div>
                <div className="text-xs text-gray-500">{usage.paramPath}</div>
              </div>
              <span className="text-[11px] text-blue-400">Select</span>
            </button>
          ))}
        </div>
      )}
    </div>
  );
}

export function ProgramPanel() {
  // useShallow narrows the subscription to just these fields/actions.
  // Without it, `useProgramStore()` would resubscribe to the whole
  // store and re-render the panel on every unrelated change (e.g.,
  // the behavior-tree editor moving a node). Action references
  // (updateX, addX, deleteX) are stable references in the zustand
  // store, so they don't add re-render churn here.
  const {
    programInfo,
    programParameters,
    blackboardNamespaces,
    blackboardKeys,
    nodes,
    selectNode,
    updateProgramParameter,
    addProgramParameter,
    deleteProgramParameter,
    updateBlackboardNamespace,
    addBlackboardNamespace,
    deleteBlackboardNamespace,
    updateBlackboardKey,
    addBlackboardKey,
    deleteBlackboardKey,
  } = useProgramStore(
    useShallow((s) => ({
      programInfo: s.programInfo,
      programParameters: s.programParameters,
      blackboardNamespaces: s.blackboardNamespaces,
      blackboardKeys: s.blackboardKeys,
      nodes: s.nodes,
      selectNode: s.selectNode,
      updateProgramParameter: s.updateProgramParameter,
      addProgramParameter: s.addProgramParameter,
      deleteProgramParameter: s.deleteProgramParameter,
      updateBlackboardNamespace: s.updateBlackboardNamespace,
      addBlackboardNamespace: s.addBlackboardNamespace,
      deleteBlackboardNamespace: s.deleteBlackboardNamespace,
      updateBlackboardKey: s.updateBlackboardKey,
      addBlackboardKey: s.addBlackboardKey,
      deleteBlackboardKey: s.deleteBlackboardKey,
    })),
  );
  const entries = useCatalogStore((s) => s.entries);
  const state = useCycleStore((s) => s.state);
  const editLocked = state === 'running' || state === 'interrupt_handler' || state === 'direct_control';

  const parameterUsageMap = useMemo(() => collectParameterUsages(nodes), [nodes]);
  const { namespaceUsages, keyUsages, fieldUsages } = useMemo(
    () => collectBlackboardUsages(nodes, entries),
    [nodes, entries],
  );
  const sortedParameters = useMemo(
    () => [...programParameters].sort((left, right) => left.name.localeCompare(right.name)),
    [programParameters],
  );
  const sortedNamespaces = useMemo(
    () => [...blackboardNamespaces].sort((left, right) => left.id.localeCompare(right.id)),
    [blackboardNamespaces],
  );
  const sortedKeys = useMemo(
    () => [...blackboardKeys].sort((left, right) => left.id.localeCompare(right.id)),
    [blackboardKeys],
  );
  const namespaceContracts = useMemo(() => {
    const contracts = new Set<string>();
    entries.forEach((entry) => {
      entry.parameters.forEach((parameter) => {
        if (
          (parameter.blackboardRole === 'namespace_writer'
            || parameter.blackboardRole === 'namespace_reader')
          && parameter.blackboardContract
        ) {
          contracts.add(parameter.blackboardContract);
        }
      });
    });
    return Array.from(contracts).sort();
  }, [entries]);
  const keyTypes = useMemo(() => {
    const types = new Set<string>(['bool', 'pose']);
    entries.forEach((entry) => {
      entry.parameters.forEach((parameter) => {
        if (
          (parameter.blackboardRole === 'key_writer' || parameter.blackboardRole === 'key_reader')
          && parameter.blackboardValueType
        ) {
          types.add(parameter.blackboardValueType);
        }
      });
    });
    return Array.from(types).sort();
  }, [entries]);

  const namespaceIds = new Set(blackboardNamespaces.map((namespace) => namespace.id));
  const keyIds = new Set(blackboardKeys.map((key) => key.id));

  if (!programInfo) {
    return (
      <div className="flex-1 overflow-y-auto px-4 py-3">
        <h2 className="text-sm font-semibold uppercase tracking-wide text-gray-400">Params</h2>
        <p className="mt-3 text-sm text-gray-500">Load a program to edit authored params and blackboard declarations.</p>
      </div>
    );
  }

  return (
    <div className="flex-1 overflow-y-auto">
      <div className="border-b border-gray-700 px-4 py-3">
        <h2 className="text-sm font-semibold uppercase tracking-wide text-gray-400">Params</h2>
        <p className="mt-1 text-xs text-gray-500">
          Edit authored program params and declared blackboard artifacts. These values are part of
          the Draft / Applied / Saved program baseline.
        </p>
        <div className="mt-2 flex flex-wrap gap-3 text-xs text-gray-400">
          <span>{sortedParameters.length} params</span>
          <span>{sortedNamespaces.length} namespaces</span>
          <span>{sortedKeys.length} keys</span>
          {programInfo.sourcePath && <span className="text-gray-500">{programInfo.sourcePath}</span>}
        </div>
        {editLocked && (
          <p className="mt-2 text-xs text-yellow-500">
            Editing is disabled while the runtime is executing or in direct control.
          </p>
        )}
      </div>

      <SectionHeading
        title="Program Parameters"
        count={sortedParameters.length}
        addLabel="Add Param"
        disabled={editLocked}
        onAdd={() => {
          const name = nextAvailableName('param', new Set(programParameters.map((parameter) => parameter.name)));
          addProgramParameter({
            name,
            type: 'string',
            default: '',
            constraints: {},
            description: '',
          });
        }}
      />
      <div className="space-y-3 p-3">
        {sortedParameters.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-sm text-gray-500">
            This program does not declare any program-level parameters.
          </div>
        )}

        {sortedParameters.map((parameter) => {
          const usages = parameterUsageMap[parameter.name] ?? [];
          return (
            <div key={parameter.name} className="rounded border border-gray-700 bg-gray-900/40 p-3">
              <div>
                <div className="flex flex-wrap items-center gap-2">
                  <h3 className="text-sm font-semibold text-white">{parameter.name}</h3>
                  <span className="rounded bg-gray-700 px-2 py-0.5 text-[11px] uppercase tracking-wide text-gray-300">
                    {parameter.type}
                  </span>
                </div>
                <div className="mt-1 text-[11px] text-gray-500">
                  {usages.length === 0
                    ? 'Unused'
                    : `Affects ${usages.length} node field${usages.length === 1 ? '' : 's'}`}
                </div>
                {parameter.description && (
                  <p className="mt-1 text-xs text-gray-400">{parameter.description}</p>
                )}
                <button
                  type="button"
                  disabled={editLocked || usages.length > 0}
                  onClick={() => deleteProgramParameter(parameter.name)}
                  title={
                    usages.length > 0
                      ? 'Remove parameter references from nodes before deleting it'
                      : 'Delete unused parameter'
                  }
                  className="mt-2 rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  Delete
                </button>
              </div>

              <ParameterEditor
                parameter={parameter}
                disabled={editLocked}
                onCommit={(value) => updateProgramParameter(parameter.name, { default: value })}
              />

              <ConstraintSummary constraints={parameter.constraints ?? {}} />
              <UsageList label="Used By" usages={usages} onSelectNode={selectNode} />
            </div>
          );
        })}
      </div>

      <SectionHeading
        title="Blackboard Namespaces"
        count={sortedNamespaces.length}
        addLabel="Add Namespace"
        disabled={editLocked}
        onAdd={() => {
          const id = nextAvailableName('namespace', namespaceIds);
          const contract = namespaceContracts[0] ?? 'selection_context_v1';
          addBlackboardNamespace({
            id,
            contract,
            prefix: `blackboard/${id}`,
            description: '',
            fields: [],
          });
        }}
      />
      <div className="space-y-3 p-3">
        {sortedNamespaces.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-sm text-gray-500">
            No declared blackboard namespaces yet. Declare namespaces here so Composer can validate
            and offer compatible selectors for producer/consumer fields.
          </div>
        )}
        {sortedNamespaces.map((namespace) => {
          const usages = namespaceUsages[namespace.id] ?? [];
          const duplicateId =
            blackboardNamespaces.filter((candidate) => candidate.id === namespace.id).length > 1;
          const duplicatePrefix =
            blackboardNamespaces.filter((candidate) => candidate.prefix === namespace.prefix).length > 1;
          const namespaceFieldNames = new Set(namespace.fields.map((field) => field.name));
          return (
            <div key={namespace.id} className="rounded border border-gray-700 bg-gray-900/40 p-3">
              <div className="space-y-3">
                <div className="text-[11px] text-gray-500">
                  {usages.length === 0
                    ? 'Unused'
                    : `Bound by ${usages.length} node field${usages.length === 1 ? '' : 's'}`}
                </div>
                <div className="grid grid-cols-1 gap-3">
                  <div>
                    <label className="text-xs text-gray-300">id</label>
                    <DraftTextInput
                      type="text"
                      value={namespace.id}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardNamespace(namespace.id, { id: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                    {!namespace.id.trim() ? (
                      <p className="mt-1 text-xs text-red-400">id is required</p>
                    ) : duplicateId ? (
                      <p className="mt-1 text-xs text-red-400">Namespace id must be unique</p>
                    ) : null}
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">contract</label>
                    <select
                      value={namespace.contract}
                      disabled={editLocked}
                      onChange={(event) =>
                        updateBlackboardNamespace(namespace.id, { contract: event.target.value })
                      }
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    >
                      {namespaceContracts.map((contract) => (
                        <option key={contract} value={contract}>
                          {contract}
                        </option>
                      ))}
                      {!namespaceContracts.includes(namespace.contract) && namespace.contract && (
                        <option value={namespace.contract}>{namespace.contract}</option>
                      )}
                    </select>
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">prefix</label>
                    <DraftTextInput
                      type="text"
                      value={namespace.prefix}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardNamespace(namespace.id, { prefix: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                    {!namespace.prefix.trim() ? (
                      <p className="mt-1 text-xs text-red-400">prefix is required</p>
                    ) : duplicatePrefix ? (
                      <p className="mt-1 text-xs text-red-400">Namespace prefix must be unique</p>
                    ) : null}
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">description</label>
                    <DraftTextInput
                      type="text"
                      value={namespace.description ?? ''}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardNamespace(namespace.id, { description: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                  </div>
                </div>
                <div className="rounded border border-gray-700 bg-gray-950/30 p-3">
                  <div className="mb-3 flex items-center justify-between gap-3">
                    <div>
                      <div className="text-xs font-semibold uppercase tracking-wide text-gray-400">
                        Declared Fields
                      </div>
                      <div className="mt-1 text-[11px] text-gray-500">
                        Fields available under <span className="text-gray-300">{namespace.prefix}/value/...</span>
                      </div>
                    </div>
                    <button
                      type="button"
                      disabled={editLocked}
                      onClick={() => {
                        const name = nextAvailableName('field', namespaceFieldNames);
                        updateBlackboardNamespace(namespace.id, {
                          fields: [
                            ...namespace.fields,
                            { name, type: 'string', description: '' },
                          ],
                        });
                      }}
                      className="rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                    >
                      Add Field
                    </button>
                  </div>
                  <div className="space-y-3">
                    {namespace.fields.length === 0 && (
                      <div className="text-xs text-gray-500">
                        No fields declared yet.
                      </div>
                    )}
                    {namespace.fields.map((fieldEntry, index) => {
                      const fieldUsageKey = `${namespace.id}.${fieldEntry.name}`;
                      const fieldEntryUsages = fieldUsages[fieldUsageKey] ?? [];
                      const duplicateFieldName =
                        namespace.fields.filter((candidate) => candidate.name === fieldEntry.name).length > 1;
                      return (
                        <div
                          key={`${namespace.id}:${index}:${fieldEntry.name}`}
                          className="rounded border border-gray-700 bg-gray-900/50 p-3"
                        >
                          <div className="grid grid-cols-1 gap-3">
                            <div>
                              <label className="text-xs text-gray-300">field</label>
                              <DraftTextInput
                                type="text"
                                value={fieldEntry.name}
                                disabled={editLocked}
                                onCommit={(value) => {
                                  const nextFields = namespace.fields.map((candidate, candidateIndex) =>
                                    candidateIndex === index
                                      ? { ...candidate, name: value }
                                      : candidate,
                                  );
                                  updateBlackboardNamespace(namespace.id, { fields: nextFields });
                                }}
                                className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                              />
                              {!fieldEntry.name.trim() ? (
                                <p className="mt-1 text-xs text-red-400">field name is required</p>
                              ) : duplicateFieldName ? (
                                <p className="mt-1 text-xs text-red-400">Field name must be unique within the namespace</p>
                              ) : null}
                            </div>
                            <div>
                              <label className="text-xs text-gray-300">type</label>
                              <select
                                value={fieldEntry.type}
                                disabled={editLocked}
                                onChange={(event) => {
                                  const nextFields = namespace.fields.map((candidate, candidateIndex) =>
                                    candidateIndex === index
                                      ? { ...candidate, type: event.target.value }
                                      : candidate,
                                  );
                                  updateBlackboardNamespace(namespace.id, { fields: nextFields });
                                }}
                                className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                              >
                                {BLACKBOARD_FIELD_TYPES.map((type) => (
                                  <option key={type} value={type}>
                                    {type}
                                  </option>
                                ))}
                                {!BLACKBOARD_FIELD_TYPES.includes(fieldEntry.type as typeof BLACKBOARD_FIELD_TYPES[number]) && fieldEntry.type && (
                                  <option value={fieldEntry.type}>{fieldEntry.type}</option>
                                )}
                              </select>
                            </div>
                            <div>
                              <label className="text-xs text-gray-300">description</label>
                              <DraftTextInput
                                type="text"
                                value={fieldEntry.description ?? ''}
                                disabled={editLocked}
                                onCommit={(value) => {
                                  const nextFields = namespace.fields.map((candidate, candidateIndex) =>
                                    candidateIndex === index
                                      ? { ...candidate, description: value }
                                      : candidate,
                                  );
                                  updateBlackboardNamespace(namespace.id, { fields: nextFields });
                                }}
                                className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                              />
                            </div>
                          </div>
                          <div className="mt-2 text-[11px] text-gray-500">
                            {fieldEntryUsages.length === 0
                              ? 'Unused'
                              : `Bound by ${fieldEntryUsages.length} node field${fieldEntryUsages.length === 1 ? '' : 's'}`}
                          </div>
                          <button
                            type="button"
                            disabled={editLocked || fieldEntryUsages.length > 0}
                            onClick={() => {
                              updateBlackboardNamespace(namespace.id, {
                                fields: namespace.fields.filter((_, candidateIndex) => candidateIndex !== index),
                              });
                            }}
                            title={
                              fieldEntryUsages.length > 0
                                ? 'Remove field bindings from nodes before deleting it'
                                : 'Delete unused field'
                            }
                            className="mt-2 rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                          >
                            Delete Field
                          </button>
                          <UsageList label="Used By" usages={fieldEntryUsages} onSelectNode={selectNode} />
                        </div>
                      );
                    })}
                  </div>
                </div>
                <button
                  type="button"
                  disabled={editLocked || usages.length > 0}
                  onClick={() => deleteBlackboardNamespace(namespace.id)}
                  title={
                    usages.length > 0
                      ? 'Remove namespace bindings from nodes before deleting it'
                      : 'Delete unused namespace'
                  }
                  className="rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  Delete
                </button>
              </div>
              <UsageList label="Used By" usages={usages} onSelectNode={selectNode} />
            </div>
          );
        })}
      </div>

      <SectionHeading
        title="Blackboard Keys"
        count={sortedKeys.length}
        addLabel="Add Key"
        disabled={editLocked}
        onAdd={() => {
          const id = nextAvailableName('key', keyIds);
          const type = keyTypes[0] ?? 'bool';
          addBlackboardKey({
            id,
            type,
            key: id,
            description: '',
          });
        }}
      />
      <div className="space-y-3 p-3">
        {sortedKeys.length === 0 && (
          <div className="rounded border border-gray-700 bg-gray-900/40 p-3 text-sm text-gray-500">
            No declared standalone blackboard keys yet. Declare keys here to make key-based reader
            and writer fields selectable instead of raw text.
          </div>
        )}
        {sortedKeys.map((entry) => {
          const usages = keyUsages[entry.id] ?? [];
          const duplicateId =
            blackboardKeys.filter((candidate) => candidate.id === entry.id).length > 1;
          const duplicateKey =
            blackboardKeys.filter((candidate) => candidate.key === entry.key).length > 1;
          return (
            <div key={entry.id} className="rounded border border-gray-700 bg-gray-900/40 p-3">
              <div className="space-y-3">
                <div className="text-[11px] text-gray-500">
                  {usages.length === 0
                    ? 'Unused'
                    : `Bound by ${usages.length} node field${usages.length === 1 ? '' : 's'}`}
                </div>
                <div className="grid grid-cols-1 gap-3">
                  <div>
                    <label className="text-xs text-gray-300">id</label>
                    <DraftTextInput
                      type="text"
                      value={entry.id}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardKey(entry.id, { id: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                    {!entry.id.trim() ? (
                      <p className="mt-1 text-xs text-red-400">id is required</p>
                    ) : duplicateId ? (
                      <p className="mt-1 text-xs text-red-400">Key id must be unique</p>
                    ) : null}
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">type</label>
                    <select
                      value={entry.type}
                      disabled={editLocked}
                      onChange={(event) =>
                        updateBlackboardKey(entry.id, { type: event.target.value })
                      }
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    >
                      {keyTypes.map((type) => (
                        <option key={type} value={type}>
                          {type}
                        </option>
                      ))}
                      {!keyTypes.includes(entry.type) && entry.type && (
                        <option value={entry.type}>{entry.type}</option>
                      )}
                    </select>
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">key</label>
                    <DraftTextInput
                      type="text"
                      value={entry.key}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardKey(entry.id, { key: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                    {!entry.key.trim() ? (
                      <p className="mt-1 text-xs text-red-400">key is required</p>
                    ) : duplicateKey ? (
                      <p className="mt-1 text-xs text-red-400">Key name must be unique</p>
                    ) : null}
                  </div>
                  <div>
                    <label className="text-xs text-gray-300">description</label>
                    <DraftTextInput
                      type="text"
                      value={entry.description ?? ''}
                      disabled={editLocked}
                      onCommit={(value) => updateBlackboardKey(entry.id, { description: value })}
                      className="mt-1 w-full rounded border border-gray-600 bg-gray-800 px-3 py-2 text-sm text-gray-100 focus:border-blue-500 focus:outline-none disabled:cursor-not-allowed disabled:opacity-50"
                    />
                  </div>
                </div>
                <button
                  type="button"
                  disabled={editLocked || usages.length > 0}
                  onClick={() => deleteBlackboardKey(entry.id)}
                  title={
                    usages.length > 0
                      ? 'Remove key bindings from nodes before deleting it'
                      : 'Delete unused key'
                  }
                  className="rounded border border-gray-600 px-2 py-1 text-[11px] text-gray-200 hover:bg-gray-700 disabled:cursor-not-allowed disabled:opacity-50"
                >
                  Delete
                </button>
              </div>
              <UsageList label="Used By" usages={usages} onSelectNode={selectNode} />
            </div>
          );
        })}
      </div>
    </div>
  );
}

import { useCallback } from 'react';
import { useProgramStore } from '../store/programStore';
import { useCatalogStore } from '../store/catalogStore';
import { useComposerStore } from '../store/composerStore';

const KIND_COLORS: Record<string, string> = {
  sequence: 'border-blue-500 bg-blue-900/50',
  fallback: 'border-orange-500 bg-orange-900/50',
  parallel: 'border-purple-500 bg-purple-900/50',
  inverter: 'border-yellow-500 bg-yellow-900/50',
  repeat: 'border-cyan-500 bg-cyan-900/50',
  retry: 'border-cyan-500 bg-cyan-900/50',
  default: 'border-green-500 bg-green-900/50',
};

export function PropertyPanel() {
  const { selectedNodeId, nodes, updateNodeParams, updateNodeLabel, removeNode } = useProgramStore();
  const { entries } = useCatalogStore();
  const { mode } = useComposerStore();

  if (!selectedNodeId) {
    return null;
  }

  const selectedNode = nodes.find(n => n.id === selectedNodeId);
  if (!selectedNode) {
    return null;
  }

  const catalogEntry = entries.find(e => e.id === selectedNode.data.catalogId);
  const colorClass = KIND_COLORS[selectedNode.data.catalogId as string] || KIND_COLORS.default;

  // Type-safe access to params
  const nodeParams = selectedNode.data.params as Record<string, unknown> || {};

  const isRunMode = mode === 'run';

  const handleDelete = useCallback(() => {
    removeNode(selectedNodeId);
  }, [selectedNodeId, removeNode]);

  const handleLabelChange = useCallback(
    (e: React.ChangeEvent<HTMLInputElement>) => {
      if (isRunMode) return;
      updateNodeLabel(selectedNodeId, e.target.value);
    },
    [selectedNodeId, updateNodeLabel, isRunMode],
  );

  const handleParamChange = useCallback(
    (paramName: string, value: unknown) => {
      if (isRunMode) return;
      updateNodeParams(selectedNodeId, { ...nodeParams, [paramName]: value });
    },
    [selectedNodeId, updateNodeParams, nodeParams, isRunMode],
  );

  const renderParameterInput = (param: {
    name: string;
    type: string;
    required: boolean;
    default: unknown;
    enumValues: string[];
    constraints: Record<string, unknown>;
    description: string;
  }) => {
    const currentValue = nodeParams[param.name] ?? param.default;

    const disabledClasses = isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : '';

    switch (param.type) {
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
          <input
            type="number"
            value={currentValue !== undefined && currentValue !== null ? String(currentValue) : ''}
            onChange={(e) => handleParamChange(param.name, param.type === 'int' ? parseInt(e.target.value, 10) : parseFloat(e.target.value))}
            min={param.constraints?.min as number | undefined}
            max={param.constraints?.max as number | undefined}
            step={param.type === 'int' ? 1 : 0.1}
            disabled={isRunMode}
            className={`w-full bg-gray-700 border border-gray-600 text-white rounded px-2 py-1 text-sm ${disabledClasses}`}
            placeholder={param.description}
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
    <div className="w-72 bg-gray-800 border-l border-gray-700 flex flex-col overflow-hidden">
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
          {!isRunMode && (
            <button
              onClick={handleDelete}
              className="text-red-400 hover:text-red-300 font-bold text-lg"
              title="Delete node"
            >
              ×
            </button>
          )}
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

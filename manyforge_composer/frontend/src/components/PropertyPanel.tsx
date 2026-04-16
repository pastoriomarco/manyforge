import { useCallback, useEffect, useMemo, useState } from 'react';
import { useProgramStore } from '../store/programStore';
import { useCatalogStore } from '../store/catalogStore';
import { useCycleStore } from '../store/cycleStore';
import { useDeploymentStore } from '../store/deploymentStore';
import { api } from '../api/client';
import type {
  CatalogParameter,
  FullDeployment,
  ModelEndpointCatalog,
  SceneStateResponse,
} from '../types/api';
import {
  collectBlackboardFieldOptions,
  collectBlackboardKeyOptions,
  collectDeclaredBlackboardOutputOptions,
  LiteralOrFieldReferenceEditor,
  NamespaceBindingEditor,
  BlackboardKeySelectorEditor,
} from './property/BlackboardBindingEditors';
import {
  JointGoalEditor,
  PoseEditor,
  PoseEnvelopeEditor,
  PoseMapEditor,
  RelativeMoveEditor,
} from './property/MotionEditors';
import {
  ListParamEditor,
  NumericParamEditor,
  StructuredParamEditor,
  TemplateVariableMapEditor,
} from './property/PrimitiveParameterEditors';
import {
  getProgramParameterReferenceName,
  ParameterBindingEditor,
  SUPPORTED_PROGRAM_PARAMETER_TYPES,
} from './property/ProgramParameterBindingEditor';

const KIND_COLORS: Record<string, string> = {
  sequence: 'border-blue-500 bg-blue-900/50',
  fallback: 'border-orange-500 bg-orange-900/50',
  parallel: 'border-purple-500 bg-purple-900/50',
  inverter: 'border-yellow-500 bg-yellow-900/50',
  repeat: 'border-cyan-500 bg-cyan-900/50',
  retry: 'border-cyan-500 bg-cyan-900/50',
  default: 'border-green-500 bg-green-900/50',
};


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

export function PropertyPanel({ onCollapse }: { onCollapse: () => void }) {
  const {
    selectedNodeId,
    nodes,
    programParameters,
    blackboardNamespaces,
    blackboardKeys,
    updateNodeParams,
    updateNodeLabel,
    removeNode,
    addProgramParameter,
  } = useProgramStore();
  const { entries } = useCatalogStore();
  const cycleState = useCycleStore((state) => state.state);
  const deployment = useDeploymentStore((state) => state.deployment);
  const selectedNode = selectedNodeId ? nodes.find(n => n.id === selectedNodeId) : undefined;
  const catalogEntry = selectedNode ? entries.find(e => e.id === selectedNode.data.catalogId) : undefined;
  const colorClass = selectedNode ? (KIND_COLORS[selectedNode.data.catalogId as string] || KIND_COLORS.default) : KIND_COLORS.default;
  const nodeParams = useMemo(
    () => (selectedNode?.data.params as Record<string, unknown> | undefined) ?? {},
    [selectedNode?.data.params],
  );
  const isRunMode = cycleState === 'running';
  const [modelEndpointCatalog, setModelEndpointCatalog] = useState<ModelEndpointCatalog | null>(null);
  const [sceneState, setSceneState] = useState<SceneStateResponse | null>(null);
  const [expandedProgramParameterBinding, setExpandedProgramParameterBinding] = useState<string | null>(null);
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
  }, [deployment, deploymentModelEndpointSignature]);

  useEffect(() => {
    setExpandedProgramParameterBinding(null);
  }, [selectedNodeId]);

  useEffect(() => {
    if (selectedNode?.data.catalogId !== 'move_manipulator_action' || nodeParams.motion_type !== 'joint_goal') {
      setSceneState(null);
      return;
    }
    let cancelled = false;
    void api.scene.state()
      .then((result) => {
        if (!cancelled) {
          setSceneState(result);
        }
      })
      .catch(() => {
        if (!cancelled) {
          setSceneState(null);
        }
      });
    return () => {
      cancelled = true;
    };
  }, [selectedNode?.data.catalogId, selectedNodeId, nodeParams.motion_type]);

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

  const handleParamPatch = useCallback(
    (patch: Record<string, unknown>) => {
      if (isRunMode || !selectedNodeId) return;
      updateNodeParams(selectedNodeId, { ...nodeParams, ...patch });
    },
    [selectedNodeId, updateNodeParams, nodeParams, isRunMode],
  );

  const isParameterVisible = useCallback((paramName: string): boolean => {
    if (selectedNode?.data.catalogId !== 'move_manipulator_action') {
      return true;
    }

    const motionType = String(nodeParams.motion_type ?? '');
    if (paramName === 'joint_goal_names' || paramName === 'joint_goal_positions_rad') {
      return false;
    }
    if (paramName === 'pose_goal') {
      return motionType === 'pose_goal';
    }
    if (paramName === 'relative_move') {
      return motionType === 'relative_move';
    }
    return true;
  }, [nodeParams.motion_type, selectedNode?.data.catalogId]);
  const blackboardOutputOptions = collectDeclaredBlackboardOutputOptions(blackboardNamespaces);
  const blackboardKeyOptions = collectBlackboardKeyOptions(blackboardKeys);
  const blackboardFieldOptions = collectBlackboardFieldOptions(blackboardNamespaces);

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

  const renderParameterInput = (param: CatalogParameter) => {
    const currentValue = nodeParams[param.name] ?? param.default;
    const getJointGoalExtraError = (): string | null => {
      if (selectedNode?.data.catalogId !== 'move_manipulator_action') {
        return null;
      }
      if (nodeParams.motion_type !== 'joint_goal') {
        return null;
      }

      const jointNames = Array.isArray(nodeParams.joint_goal_names) ? nodeParams.joint_goal_names : [];
      const jointPositions = Array.isArray(nodeParams.joint_goal_positions_rad) ? nodeParams.joint_goal_positions_rad : [];

      if (param.name === 'joint_goal_names' && jointNames.length === 0) {
        return 'At least one joint name is required when motion_type=joint_goal';
      }
      if (param.name === 'joint_goal_positions_rad' && jointPositions.length === 0) {
        return 'At least one joint position is required when motion_type=joint_goal';
      }
      if (
        (param.name === 'joint_goal_names' || param.name === 'joint_goal_positions_rad')
        && jointNames.length > 0
        && jointPositions.length > 0
        && jointNames.length !== jointPositions.length
      ) {
        return 'joint_goal_names and joint_goal_positions_rad must have the same length';
      }
      return null;
    };

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
          keyOptions={blackboardKeyOptions}
          fieldOptions={blackboardFieldOptions}
        />
      );
    }

    if (param.uiEditor === 'output_key_prefix_selector') {
      return (
        <NamespaceBindingEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          options={blackboardOutputOptions.filter(
            (option) => !param.blackboardContract || option.contract === param.blackboardContract,
          )}
          emptyMessage="No declared output namespaces for this contract. Add one in the PARAMS tab first."
          helperText="Select a declared output namespace for this node."
        />
      );
    }

    if (param.uiEditor === 'selection_context_prefix_selector') {
      return (
        <NamespaceBindingEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          options={blackboardOutputOptions.filter(
            (option) => option.contract === (param.blackboardContract ?? 'selection_context_v1'),
          )}
          emptyMessage="No Build Scene Selection Context node currently exposes an output namespace."
          helperText="Choose a compatible declared namespace."
        />
      );
    }

    if (param.uiEditor === 'selection_response_prefix_selector') {
      return (
        <NamespaceBindingEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          options={blackboardOutputOptions.filter(
            (option) => option.contract === (param.blackboardContract ?? 'query_model_output_v1'),
          )}
          emptyMessage="No QueryModel node currently exposes an output namespace."
          helperText="Choose a compatible declared namespace."
        />
      );
    }

    if (param.uiEditor === 'blackboard_key_selector') {
      return (
        <BlackboardKeySelectorEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          keyOptions={blackboardKeyOptions}
          fieldOptions={blackboardFieldOptions}
          expectedType={param.blackboardValueType}
          allowFieldRefs={param.blackboardRole === 'key_reader'}
        />
      );
    }

    if (param.uiEditor === 'string_or_blackboard_field_selector') {
      return (
        <LiteralOrFieldReferenceEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommitLiteral={(value) => handleParamChange(param.name, value)}
          onCommitFieldRef={(value) => handleParamChange(param.name, value)}
          fieldOptions={blackboardFieldOptions}
          expectedType={param.blackboardValueType}
          expectedContract={param.blackboardContract}
          emptyMessage="No compatible declared fields yet. Add them in the PARAMS tab."
          literalLabel="Literal value"
          fieldLabel="Declared blackboard field"
          createDefaultLiteral={() => ''}
          renderLiteralEditor={(literalValue, onCommitLiteral) => (
            <input
              type="text"
              value={typeof literalValue === 'string' ? literalValue : ''}
              disabled={isRunMode}
              onChange={(event) => onCommitLiteral(event.target.value)}
              className={`w-full rounded border border-gray-600 bg-gray-700 px-2 py-1 text-sm text-white ${isRunMode ? 'opacity-50 cursor-not-allowed bg-gray-800' : ''}`}
            />
          )}
        />
      );
    }

    if (param.uiEditor === 'pose_or_blackboard_field_selector') {
      return (
        <LiteralOrFieldReferenceEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommitLiteral={(value) => handleParamChange(param.name, value)}
          onCommitFieldRef={(value) => handleParamChange(param.name, value)}
          fieldOptions={blackboardFieldOptions.filter(
            (option) => !param.blackboardContract || option.namespaceContract === param.blackboardContract,
          )}
          expectedType="pose"
          emptyMessage="No declared pose fields yet. Add them in the PARAMS tab."
          literalLabel="Literal pose"
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
      );
    }

    if (param.uiEditor === 'relative_move') {
      return (
        <RelativeMoveEditor
          currentValue={currentValue}
          isRunMode={isRunMode}
          onCommit={(value) => handleParamChange(param.name, value)}
          fieldOptions={blackboardFieldOptions.filter((option) => option.fieldType === 'pose')}
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

      case 'float_list':
      case 'string_list':
        return (
          <ListParamEditor
            currentValue={currentValue}
            isRunMode={isRunMode}
            onCommit={(value) => handleParamChange(param.name, value)}
            paramType={param.type}
            extraError={getJointGoalExtraError()}
            itemPlaceholder={
              param.name === 'joint_goal_names'
                ? 'joint_name'
                : param.name === 'joint_goal_positions_rad'
                  ? '0.0'
                  : undefined
            }
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
          {selectedNode.data.catalogId === 'move_manipulator_action' && (
            <div className="mb-3 rounded border border-gray-700 bg-gray-900/50 px-3 py-2 text-[11px] text-gray-400">
              Showing fields for <span className="text-gray-200">{String(nodeParams.motion_type ?? 'joint_goal')}</span>.
            </div>
          )}
          <div className="space-y-3">
            {catalogEntry.parameters.filter((param) => isParameterVisible(param.name)).map((param) => {
              const currentValue = nodeParams[param.name] ?? param.default;
              const supportsProgramParameterBinding =
                param.parameterizable && SUPPORTED_PROGRAM_PARAMETER_TYPES.has(param.type);

              return (
                <div key={param.name} className="space-y-3">
                  <div>
                    <div className="flex items-center justify-between mb-1">
                      <label className="text-xs text-gray-300">
                        {param.name}
                        {param.required && <span className="text-red-400 ml-1">*</span>}
                      </label>
                      <div className="flex items-center gap-2">
                        {supportsProgramParameterBinding && (() => {
                          const bindingKey = `${selectedNodeId}:${param.name}`;
                          const isExpanded = expandedProgramParameterBinding === bindingKey;
                          const isBound = getProgramParameterReferenceName(currentValue) !== null;
                          return (
                            <button
                              type="button"
                              onClick={() => {
                                setExpandedProgramParameterBinding((current) =>
                                  current === bindingKey ? null : bindingKey,
                                );
                              }}
                              disabled={isRunMode}
                              title="Program parameter binding"
                              className={`h-5 w-5 rounded border text-[11px] font-semibold ${
                                isRunMode
                                  ? 'cursor-not-allowed border-gray-700 bg-gray-800 text-gray-500'
                                  : isExpanded || isBound
                                    ? 'border-blue-500 bg-blue-900/60 text-blue-100'
                                    : 'border-gray-600 bg-gray-700 text-gray-200 hover:bg-gray-600'
                              }`}
                            >
                              P
                            </button>
                          );
                        })()}
                        <span className="text-xs text-gray-500">{param.type}</span>
                      </div>
                    </div>
                    {param.description && (
                      <div className="text-xs text-gray-500 mb-1">{param.description}</div>
                    )}
                    {supportsProgramParameterBinding ? (
                      <ParameterBindingEditor
                        paramName={param.name}
                        paramType={param.type}
                        paramDefault={param.default}
                        constraints={param.constraints}
                        currentValue={currentValue}
                        programParameters={programParameters}
                        expanded={expandedProgramParameterBinding === `${selectedNodeId}:${param.name}`}
                        isRunMode={isRunMode}
                        onCommitLiteral={(value) => handleParamChange(param.name, value)}
                        onBindProgramParameter={(parameterName) =>
                          handleParamChange(param.name, `\${param:${parameterName}}`)
                        }
                        onCreateProgramParameter={addProgramParameter}
                      >
                        {renderParameterInput(param)}
                      </ParameterBindingEditor>
                    ) : (
                      renderParameterInput(param)
                    )}
                  </div>
                  {selectedNode.data.catalogId === 'move_manipulator_action'
                    && param.name === 'motion_type'
                    && nodeParams.motion_type === 'joint_goal' && (
                      <JointGoalEditor
                        jointNames={sceneState?.robot?.joint_names ?? []}
                        currentJointNames={Array.isArray(nodeParams.joint_goal_names) ? nodeParams.joint_goal_names.map((value) => String(value)) : []}
                        currentJointPositions={Array.isArray(nodeParams.joint_goal_positions_rad) ? nodeParams.joint_goal_positions_rad.map((value) => Number(value)) : []}
                        isRunMode={isRunMode}
                        onCommit={(jointGoalNames, jointGoalPositions) => {
                          handleParamPatch({
                            joint_goal_names: jointGoalNames,
                            joint_goal_positions_rad: jointGoalPositions,
                          });
                        }}
                      />
                    )}
                </div>
              );
            })}
          </div>
        </div>
      )}
    </div>
  );
}

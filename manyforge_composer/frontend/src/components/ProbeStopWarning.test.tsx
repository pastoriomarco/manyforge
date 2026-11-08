import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry, DeploymentWorkspaceResponse, ProgramAuthoringDocument, ProgramTreeNode,
} from '../types/api';
import { ProgramPropertyEditor } from './ProgramPropertyEditor';

const probeEntry: CatalogEntry = {
  id: 'motion.probe_search', name: 'Probe search', category: 'motion',
  description: 'Execute a precision contact search with a scoped interrupt.', stability: 'stable',
  kind: 'leaf', childrenMin: 0, childrenMax: 0,
  parameters: [
    { id: 'target', required: true,
      schema: { type: 'object', 'x-manyforge-semantic-type': 'target_reference' },
      schemaHash: 'sha256:t', hasDefault: false },
    { id: 'stop_mode', required: true,
      schema: { type: 'string', enum: ['spring_back_hold', 'soft_on_path', 'hard_deceleration', 'spring_back_fast'] },
      schemaHash: 'sha256:m', hasDefault: false },
  ],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: true, ownerApi: 'kernel.operation' },
  runners: { py_trees: 'motion_probe_search_v1' }, tags: ['motion'],
  minProgramProfile: 'composable-cell-v1', blackboard: { reads: [], writes: [] },
  ownerContext: null, outcomes: {}, eventFamily: null,
};

describe('probe Stop warning', () => {
  it('shows the inherited-setting warning on the probe node without invalidating it', () => {
    const node: ProgramTreeNode = {
      node_id: 'motion.probe_search', instance_id: 'probe', children: [],
      parameters: { target: { component_id: 'robot', command_target_id: 'arm' }, stop_mode: 'soft_on_path' },
    };
    const program: ProgramAuthoringDocument = {
      api_version: 'manyforge.program.v3', program_id: 'p', program_profile: 'composable-cell-v1',
      name: 'P', catalog: { api_version: 'manyforge.node_catalog.v3', content_version: '3.0.0' },
      parameters: [],
      blackboard: { namespaces: [], keys: [] },
      schema_version: 'manyforge.composer_authoring_forest.v2', forest: [node], positions: {},
    };
    const deployment = {
      // Compiled v1: intent v2 cannot carry a recoverable substitution.
      draft: { document: {
        api_version: 'manyforge.deployment.v1',
        graph: { control_bindings: [
          { control_binding_id: 'b', controllable_component_id: 'robot', command_target_id: 'arm' },
        ] },
        policies: { stop_stage_parameters: [{
          control_binding_id: 'b', stop_escalation_stage_id: 'motion_controlled_stop',
          provider_contract_ref: { provider_contract_role_id: 'stop_contract' },
          default_contract_parameters: {
            stop_mode: 'soft_on_path', allow_natural_completion_on_short_path: true,
            recoverable_substitution_mode: 'spring_back_hold',
          },
        }] },
      } },
      // The node's direct target resolves the Stop settings; no binding is needed.
      binding: null,
    } as unknown as DeploymentWorkspaceResponse;
    const onInvalidEditChange = vi.fn();
    const before = JSON.stringify(node);
    const markup = renderToStaticMarkup(<ProgramPropertyEditor
      roots={[node]} selectedId="probe" catalogEntries={[probeEntry]} program={program}
      deploymentWorkspace={deployment} busy={false} onMutate={vi.fn()}
      onUpdateNodePolicies={vi.fn()} onInvalidEditChange={onInvalidEditChange}
    />);
    expect(markup).toContain('This probe may finish a short remaining path instead of stopping on contact.');
    expect(markup).toContain('this probe will spring back instead of stopping at the contact point.');
    expect(markup.match(/inline-message severity-warning/g)).toHaveLength(2);
    // A warning only: no error, no "Complete this node", no invalid edit, values untouched.
    expect(markup).not.toContain('severity-error');
    expect(markup).not.toContain('Complete this node');
    expect(onInvalidEditChange).not.toHaveBeenCalledWith(true);
    expect(JSON.stringify(node)).toBe(before);
  });
});

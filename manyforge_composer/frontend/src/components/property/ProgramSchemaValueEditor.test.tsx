import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it, vi } from 'vitest';

import type {
  CatalogEntry,
  ProgramAuthoringDocument,
  ProgramTreeNode,
} from '../../types/api';
import { ProgramSchemaValueEditor } from './ProgramSchemaValueEditor';

const entry: CatalogEntry = {
  id: 'scene.attach_carried_object',
  name: 'Attach object',
  category: 'scene',
  description: '',
  stability: 'stable',
  kind: 'leaf',
  childrenMin: 0,
  childrenMax: 0,
  parameters: [],
  requiredCapabilities: [],
  execution: { blocking: true, cancellable: false, ownerApi: 'scene' },
  runners: { py_trees: 'attach_v1' },
  tags: [],
  minProgramProfile: 'composable-cell-v1',
  blackboard: { reads: [], writes: [] },
  ownerContext: null,
  outcomes: {},
  eventFamily: null,
};

const node: ProgramTreeNode = {
  node_id: entry.id,
  instance_id: 'attach',
  parameters: { target_role: 'gripper' },
  children: [],
};

const program: ProgramAuthoringDocument = {
  api_version: 'manyforge.program.v1',
  program_id: 'schema_editor',
  program_profile: 'composable-cell-v1',
  name: 'Schema editor',
  catalog: { api_version: 'manyforge.node_catalog.v2', content_version: '1.1.0' },
  component_roles: { tool: {} },
  target_roles: {
    arm: { required_collision_body_roles: ['arm_collision'] },
    gripper: { required_collision_body_roles: ['finger', 'gripper_collision'] },
  },
  schema_version: 'manyforge.composer_authoring_forest.v1',
  forest: [node],
  positions: {},
};

const context = { entry, node, parameterId: 'touch_body_roles', program };

describe('schema-derived Program value editor', () => {
  it('renders scoped semantic arrays as structured rows rather than JSON text', () => {
    const markup = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{
          type: 'array',
          minItems: 1,
          items: {
            type: 'string',
            'x-manyforge-semantic-type': 'collision_body_role',
            'x-manyforge-semantic-scope-parameter': 'target_role',
          },
        }}
        value={['gripper_collision']}
        disabled={false}
        context={context}
        onCommit={vi.fn()}
      />,
    );

    expect(markup).toContain('<details');
    expect(markup).toContain('gripper_collision');
    expect(markup).toContain('finger');
    expect(markup).not.toContain('arm_collision');
    expect(markup).not.toContain('<textarea');
    expect(markup).toContain('Add row');
  });

  it('renders fixed numeric tuples and schema-valued maps without raw JSON', () => {
    const tuple = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{ type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } }}
        value={[1, 2, 3]}
        disabled={false}
        context={context}
        path="position_m"
        onCommit={vi.fn()}
      />,
    );
    expect(tuple.match(/type="number"/g)).toHaveLength(3);
    expect(tuple).toContain('>X<');
    expect(tuple).toContain('>Y<');
    expect(tuple).toContain('>Z<');

    const map = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{
          type: 'object',
          properties: {},
          additionalProperties: {
            type: 'object',
            properties: { limit: { type: 'number' } },
            required: ['limit'],
            additionalProperties: false,
          },
        }}
        value={{ joint_1: { limit: 1 } }}
        disabled={false}
        context={context}
        path="limits"
        onCommit={vi.fn()}
      />,
    );
    expect(map).toContain('program-schema-map-row');
    expect(map).toContain('joint_1');
    expect(map).not.toContain('<textarea');
  });

  it('starts absent fixed tuples and closed objects with complete structured fields', () => {
    const tuple = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{ type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } }}
        value={undefined}
        disabled={false}
        context={context}
        path="position_m"
        onCommit={vi.fn()}
      />,
    );
    expect(tuple.match(/value="0"/g)).toHaveLength(3);

    const object = renderToStaticMarkup(
      <ProgramSchemaValueEditor
        schema={{
          type: 'object',
          properties: {
            enabled: { type: 'boolean' },
            count: { type: 'integer' },
          },
          required: ['enabled', 'count'],
          additionalProperties: false,
        }}
        value={undefined}
        disabled={false}
        context={context}
        path="settings"
        onCommit={vi.fn()}
      />,
    );
    expect(object).toContain('enabled');
    expect(object).toContain('count');
    expect(object).not.toContain('<textarea');
  });
});

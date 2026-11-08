import { describe, expect, it } from 'vitest';
import { parseProgramAuthoringDocument, parseProgramDocument } from './program';

const reference = { component_id: 'robot', frame_id: '2_BASE' };
const node = { node_id: 'orchestration.wait', instance_id: 'wait', parameters: {}, children: [] };
const program = () => ({
  api_version: 'manyforge.program.v3',
  program_id: 'example',
  program_revision: `sha256:${'a'.repeat(64)}`,
  program_profile: 'composable-cell-v1',
  name: 'Example',
  catalog: { api_version: 'manyforge.node_catalog.v3', content_version: '3.0.0' },
  scene: [{
    scene_resource_id: 'table',
    shape: { type: 'box', box_dimensions_m: [1, 1, 1] },
    pose: { frame_ref: { kind: 'component_frame', frame: reference },
      position_m: [0, 0, 0], orientation: { xyzw: [0, 0, 0, 1] } },
    resource_roles: ['support'],
  }],
  tree: node,
});

describe('Program v3 wire contracts', () => {
  it('reads direct viewer visibility and refuses duplicate or unordered frames', () => {
    const frame = (component_id: string, frame_id: string, visible: boolean) => ({
      frame: { component_id, frame_id }, visible,
    });
    const viewer = {
      frame_visibility: [frame('arm_a', 'base', false), frame('arm_b', 'base', true)],
      cell_frame_visible: false,
      scene_resource_visibility: { table: false },
    };
    expect(parseProgramDocument({ ...program(), viewer }).viewer).toEqual(viewer);
    expect(() => parseProgramDocument({ ...program(), viewer: {
      ...viewer, frame_visibility: [frame('arm_a', 'base', true), frame('arm_a', 'base', false)],
    } })).toThrow();
    expect(() => parseProgramDocument({ ...program(), viewer: {
      ...viewer, frame_visibility: [frame('arm_b', 'base', true), frame('arm_a', 'base', false)],
    } })).toThrow();
    expect(() => parseProgramDocument({ ...program(), viewer: {
      ...viewer, scene_resource_visibility: { missing: false },
    } })).toThrow();
  });
  it('accepts an exact component/frame reference and preserves Scene-owned labels', () => {
    const parsed = parseProgramDocument(program());
    expect(parsed.scene?.[0].pose.frame_ref).toEqual({ kind: 'component_frame', frame: reference });
    expect(parsed.scene?.[0].resource_roles).toEqual(['support']);
  });

  it('refuses v2 and removed Program roles without converting them', () => {
    expect(() => parseProgramDocument({ ...program(), api_version: 'manyforge.program.v2',
      component_roles: {}, target_roles: {} })).toThrow('manyforge.program.v3');
    expect(() => parseProgramDocument({ ...program(), component_roles: {} })).toThrow();
    const oldFrame = { kind: 'component_frame', component_role: 'robot', model_role: 'body', frame_role: 'base' };
    const scene = program().scene;
    scene[0].pose.frame_ref = oldFrame as unknown as typeof scene[0]['pose']['frame_ref'];
    expect(() => parseProgramDocument({ ...program(), scene })).toThrow();
  });

  it('accepts forest v2 and retains its exact positioned nodes', () => {
    const { tree, program_revision, ...fields } = program();
    void program_revision;
    const parsed = parseProgramAuthoringDocument({ ...fields,
      schema_version: 'manyforge.composer_authoring_forest.v2', forest: [tree],
      positions: { wait: { x: 12, y: -4 } },
    }, 'authoring');
    expect(parsed.forest).toHaveLength(1);
    expect(parsed.positions.wait).toEqual({ x: 12, y: -4 });
    expect(() => parseProgramAuthoringDocument({ ...fields,
      schema_version: 'manyforge.composer_authoring_forest.v1', forest: [tree], positions: {},
    }, 'authoring')).toThrow();
  });
});

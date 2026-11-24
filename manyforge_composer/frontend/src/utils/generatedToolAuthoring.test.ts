import { describe, expect, it } from 'vitest';

import type { JsonValue } from '../types/api';
import {
  defaultContactBodies,
  fieldPath,
  followedWiring,
  generatedToolIds,
  generatedToolRow,
  jawPairs,
  poseFields,
  poseValue,
  refusalAt,
  releaseDepthStart,
  toolEnds,
  withValue,
  withWiring,
  type JsonObject,
} from './generatedToolAuthoring';

const parameters: JsonObject = {
  api_version: 'manyforge.parametric_tool.v1', kind: 'tool',
  body: [{ name: 'housing', collision: { box: { size_m: [0.08, 0.08, 0.06] } } }],
  chains: [
    { attach_to: 'body', elements: [
      { jaw_pair: { id: 'outer', positive_jaw: [{ name: 'f', collision: { box: { size_m: [0.01, 0.01, 0.01] } } }],
        pads: { positive: [{ name: 'p', collision: { box: { size_m: [0.01, 0.01, 0.01] } } }] },
        min_aperture_m: 0, max_aperture_m: 0.04, closed_aperture_m: 0.02 } },
      { fixed: { id: 'bracket', parts: [] } },
    ] },
    { attach_to: 'body', elements: [{ tip: { id: 'probe', parts: [] } }] },
  ],
  tcps: [{ id: 'tcp', attach_to: 'outer' }],
};
const valve = { outputs: [{ signal: { controllable_component_id: 'io', signal_name: 'v' }, open_value: false, close_value: true }] };

function document(source: JsonValue = parameters, wiring: JsonValue[] = []): Record<string, JsonValue> {
  return {
    api_version: 'manyforge.deployment_intent.v3',
    components: [
      { component_id: 'robot', component_profile_id: 'ufactory.lite6' },
      { component_id: 'gripper', component_profile_id: 'manyforge.tool',
        model_source: { parameters: source },
        configuration: { runtime_kind: 'ros2_control', runtime_configuration: {
          component_streams: [], gripper_target_configurations: wiring } } },
    ],
  };
}

describe('generated tool authoring', () => {
  it('reads the inline parameters and wiring of a Tool row', () => {
    const wired = { controllable_component_id: 'gripper', command_target_id: 'outer', pneumatic_io: valve };
    const row = generatedToolRow(document(parameters, [wired]), 'gripper');
    expect(row?.index).toBe(1);
    expect(row?.parameters).toEqual(parameters);
    expect(row?.reference).toBeNull();
    expect(row?.wiring).toEqual([wired]);
    expect(generatedToolRow(document(), 'robot')).toBeNull();
    expect(generatedToolIds(document())).toEqual(new Set(['gripper']));
  });

  it('names a parameter file when the row references one', () => {
    const row = generatedToolRow(document({ project_path: 'tool/gripper.tool.yaml' }), 'gripper');
    expect(row?.parameters).toBeNull();
    expect(row?.reference).toBe('tool/gripper.tool.yaml');
  });

  it('lists tool ends, jaw pairs and the default contact bodies as Load derives them', () => {
    expect(toolEnds(parameters).map((end) => end.id)).toEqual(['outer', 'probe']);
    expect(jawPairs(parameters).map((end) => end.id)).toEqual(['outer']);
    expect(defaultContactBodies(parameters, 'outer')).toEqual(['outer_pads']);
    expect(defaultContactBodies(parameters, 'probe')).toEqual(['probe']);
    expect(defaultContactBodies(parameters, 'body')).toEqual([]);
  });

  it('names fields as the backend refusal does', () => {
    const field = fieldPath('chains', 0, 'elements', 1, 'jaw_pair', 'max_aperture_m');
    expect(field).toBe('chains[0].elements[1].jaw_pair.max_aperture_m');
    expect(refusalAt(field, 'chains')).toBe(true);
    expect(refusalAt('tcps[0].id', 'chains')).toBe(false);
  });

  it('sets and removes nested parameters without touching the rest', () => {
    const changed = withValue(parameters, ['chains', 0, 'elements', 0, 'jaw_pair', 'max_aperture_m'], 0.05) as JsonObject;
    expect(jawPairs(changed)[0].fields.max_aperture_m).toBe(0.05);
    expect(jawPairs(parameters)[0].fields.max_aperture_m).toBe(0.04);
    const removed = withValue(parameters, ['chains', 1], undefined) as JsonObject;
    expect(toolEnds(removed).map((end) => end.id)).toEqual(['outer']);
  });

  it('keeps one wiring row per jaw pair, following a rename and dropping a removed pair', () => {
    const rows = withWiring([], 'gripper', 'outer', valve);
    expect(rows).toEqual([{ controllable_component_id: 'gripper', command_target_id: 'outer', pneumatic_io: valve }]);
    expect(withWiring(rows, 'gripper', 'outer', undefined)).toEqual([]);
    const renamed = withValue(parameters, ['chains', 0, 'elements', 0, 'jaw_pair', 'id'], 'main') as JsonObject;
    expect(followedWiring(rows, parameters, renamed)?.[0].command_target_id).toBe('main');
    const removed = withValue(parameters, ['chains', 0, 'elements', 0], undefined) as JsonObject;
    expect(followedWiring(rows, parameters, removed)).toEqual([]);
    expect(followedWiring(rows, parameters, parameters)).toBeNull();
  });

  it('edits poses as position and roll, pitch, yaw, leaving identity parts out', () => {
    expect(poseFields({ position_m: [0, 0, 0.1], orientation: { rpy_deg: [0, 90, 0] } }))
      .toEqual({ position: [0, 0, 0.1], rpy: [0, 90, 0] });
    expect(poseFields({ orientation: { xyzw: [0, 0, 0, 1] } }).rpy).toBeNull();
    expect(poseValue([0, 0, 0], [0, 0, 0])).toBeUndefined();
    expect(poseValue([0, 0, 0.1], [0, 0, 0])).toEqual({ position_m: [0, 0, 0.1] });
  });

  it('starts a spring slider release depth below its trip depth', () => {
    expect(releaseDepthStart(0.0095)).toBe(0.00475);
    expect(releaseDepthStart(undefined)).toBe(0.0005);
  });
});

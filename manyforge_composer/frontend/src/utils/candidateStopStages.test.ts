import { describe, expect, it } from 'vitest';
import { candidateStopStages } from './interruptAuthoring';
import type { ReferenceSelectorFacts } from './referenceValidation';

const target = { component_id: 'arm', command_target_id: 'move' };
const route = { control_binding_id: 'binding', control_binding_revision: '1', control_route_id: 'route',
  capability_id: 'planning.motion.pose_goal', available: true, reason: null,
  stages: [{ stage_id: 'stop', stage_kind: 'nominal', command_kind: 'halt', stop_action_class: 'controlled' }] };
const row: ReferenceSelectorFacts['targets'][number] = {
  target, capabilities: [], source_joint_names: [], tcp_entries: [], default_tcp: null, stop_routes: [route],
};
describe('candidate Stop routes', () => {
  it('uses the exact target and capability', () => {
    expect(candidateStopStages(target, route.capability_id, [row]).stages?.map((stage) => stage.id)).toEqual(['stop']);
    expect(candidateStopStages(target, 'other', [row]).stages).toBeNull();
  });
  it('never picks the first of multiple matching routes', () => {
    expect(candidateStopStages(target, route.capability_id, [{ ...row, stop_routes: [route, route] }]).reason)
      .toContain('Multiple');
  });
  it('preserves a typed route refusal message', () => {
    const failed = { ...route, available: false, stages: [], reason: { code: 'missing', message: 'No retained chain', details: {} } };
    expect(candidateStopStages(target, route.capability_id, [{ ...row, stop_routes: [failed] }]).reason).toBe('No retained chain');
  });
});

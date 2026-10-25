import { describe, expect, it } from 'vitest';
import isaacSceneOwnerSections from '../testFixtures/isaacSceneOwnerSections.fixture.json';
import type { HeadlessSourceSection } from '../types/headless';
import { isaacJointStreamNotices } from './isaacJointStreamNotices';

const EXPECTED = {
  object_id: 'isaac_ur10e_light_robot',
  policy_id: 'viewer.measured_robot',
  topic: '/isaac_joint_states',
};

function sceneRuntimeSection(): HeadlessSourceSection {
  const sections = structuredClone(isaacSceneOwnerSections.sections) as Record<string, unknown>;
  const wire = sections.scene_runtime as Record<string, unknown>;
  const payload = wire.payload as Record<string, unknown>;
  payload.cell_observer = {
    ...(payload.cell_observer as Record<string, unknown>),
    expected_joint_state: EXPECTED,
  };
  return {
    available: wire.available as boolean,
    freshness: wire.freshness as HeadlessSourceSection['freshness'],
    knownGaps: wire.known_gaps as string[],
    payload: payload,
    sourceGeneration: wire.source_generation as number | null,
    sourceOwnerEpoch: wire.source_owner_epoch as string | null,
    sourceOwnerId: wire.source_owner_id as string | null,
    receiverAgeNs: wire.receiver_age_ns as number | null,
  };
}

function poseSources(section: HeadlessSourceSection): Record<string, unknown>[] {
  const payload = section.payload as Record<string, unknown>;
  const state = payload.state as Record<string, unknown>;
  return state.pose_sources as Record<string, unknown>[];
}

function jointRow(section: HeadlessSourceSection): Record<string, unknown> {
  const row = poseSources(section).find((candidate) => (
    candidate.object_id === EXPECTED.object_id
    && candidate.policy_id === EXPECTED.policy_id
  ));
  if (row === undefined) throw new Error('joint-state fixture row is missing');
  return row;
}

describe('Isaac joint-state stream notices', () => {
  it('warns before the first sample using the exact configured topic and object', () => {
    const section = sceneRuntimeSection();
    const state = (section.payload as Record<string, unknown>).state as Record<string, unknown>;
    state.pose_sources = poseSources(section).filter((row) => (
      row.object_id !== EXPECTED.object_id || row.policy_id !== EXPECTED.policy_id
    ));

    const [notice] = isaacJointStreamNotices(section);

    expect(notice).toMatchObject({
      severity: 'warning',
      domain: 'runtime',
      title: 'Waiting for Isaac joint-state feedback',
    });
    expect(notice?.message).toContain(EXPECTED.object_id);
    expect(notice?.message).toContain(EXPECTED.topic);
    expect(notice?.message).not.toMatch(/paused|stopped/i);
  });

  it('clears automatically when the exact configured topic has fresh feedback', () => {
    const section = sceneRuntimeSection();
    const row = jointRow(section);
    row.fresh = true;

    expect(isaacJointStreamNotices(section)).toEqual([]);
  });

  it('warns when the current matching joint-state sample is stale', () => {
    const section = sceneRuntimeSection();

    expect(isaacJointStreamNotices(section)[0]).toMatchObject({
      severity: 'warning',
      title: 'Isaac joint-state feedback is stale',
    });
  });

  it('reports status unavailable when the scene-runtime section is stale or unavailable', () => {
    const section = sceneRuntimeSection();
    section.freshness = 'stale';

    expect(isaacJointStreamNotices(section)[0]).toMatchObject({
      severity: 'warning',
      title: 'Isaac joint-state stream status unavailable',
    });
  });

  it('reports status unavailable when the configured observer is not present/current', () => {
    const section = sceneRuntimeSection();
    const payload = section.payload as Record<string, unknown>;
    const observer = payload.cell_observer as Record<string, unknown>;
    observer.process_presence = 'absent';

    expect(isaacJointStreamNotices(section)[0]?.title)
      .toBe('Isaac joint-state stream status unavailable');
  });

  it('surfaces an observer source error without claiming the simulator is stopped', () => {
    const section = sceneRuntimeSection();
    const row = jointRow(section);
    row.buffered = false;
    row.eligible = false;
    row.fresh = false;
    row.payload_quality = 'unknown';
    row.payload = {
      api_version: 'manyforge.ros_pose_observation.v2',
      kind: 'joint_state',
      source_error: {
        code: 'subscription_error',
        detail: 'No joint-state message has arrived.',
        source_key: EXPECTED.topic,
      },
    };

    const [notice] = isaacJointStreamNotices(section);
    expect(notice?.title).toBe('Isaac joint-state stream status unavailable');
    expect(notice?.message).toContain('No joint-state message has arrived.');
    expect(notice?.message).not.toMatch(/paused|stopped/i);
  });

  it('does not infer a configured stream from legacy or explicit no-binding projections', () => {
    const legacy = sceneRuntimeSection();
    const legacyPayload = legacy.payload as Record<string, unknown>;
    delete (legacyPayload.cell_observer as Record<string, unknown>).expected_joint_state;
    expect(isaacJointStreamNotices(legacy)).toEqual([]);

    const noBinding = sceneRuntimeSection();
    (noBinding.payload!.cell_observer as Record<string, unknown>).expected_joint_state = null;
    expect(isaacJointStreamNotices(noBinding)).toEqual([]);
  });
});

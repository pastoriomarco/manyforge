import { describe, expect, it } from 'vitest';

import { parseComponentCatalog } from './componentCatalogParser';

const entry = {
  entryId: 'lite6_pneumatic_mock.scenario/gripper_io',
  kind: 'io_device',
  componentId: 'gripper_io',
  componentProfileId: 'io_device_v0',
  modelFamilyId: null,
  tags: [],
  lane: 'manyforge-only',
  description: null,
  sources: ['examples/lite6_pneumatic_mock.scenario'],
  components: [{ component_id: 'gripper_io', component_profile_id: 'io_device_v0', target_settings: {}, configuration: {} }],
};

const catalog = {
  schemaVersion: 'manyforge.composer_component_catalog.v1',
  projectRoot: null,
  entries: [entry],
  profilesWithoutEntry: ['uf850_mock_commanded_v0'],
  componentProfiles: {
    arm_mock_v0: {
      tags: ['articulated-arm'],
      attachmentInterfaces: ['flange'],
      tcpTargets: [{ commandTargetId: 'arm', tcpFrameRoleIds: ['flange'], defaultTcpFrameRoleId: 'flange' }],
    },
    hand_v0: { tags: ['gripper', 'tool'], attachmentInterfaces: [], tcpTargets: [] },
  },
};

describe('component catalog parser', () => {
  it('reads the closed catalog shape', () => {
    expect(parseComponentCatalog(catalog)).toEqual(catalog);
  });

  it('reads all target frames, including untagged frames', () => {
    const frames = [{ commandTargetId: 'gripper', frameRoleIds: ['tcp', 'tip'] }];
    const parsed = parseComponentCatalog({
      ...catalog,
      componentProfiles: { hand_v0: { ...catalog.componentProfiles.hand_v0, frameTargets: frames } },
    });
    expect(parsed.componentProfiles.hand_v0.frameTargets).toEqual(frames);
    expect(() => parseComponentCatalog({
      ...catalog,
      componentProfiles: { hand_v0: { ...catalog.componentProfiles.hand_v0, frameTargets: [{ commandTargetId: 'gripper' }] } },
    })).toThrow('frameTargets[0]');
  });

  it('reads the motion capability fact when known', () => {
    for (const hasMotionTarget of [true, false]) {
      const parsed = parseComponentCatalog({
        ...catalog,
        componentProfiles: { hand_v0: { ...catalog.componentProfiles.hand_v0, hasMotionTarget } },
      });
      expect(parsed.componentProfiles.hand_v0.hasMotionTarget).toBe(hasMotionTarget);
    }
    expect(parseComponentCatalog(catalog).componentProfiles.hand_v0).not.toHaveProperty('hasMotionTarget');
    expect(() => parseComponentCatalog({
      ...catalog,
      componentProfiles: { hand_v0: { ...catalog.componentProfiles.hand_v0, hasMotionTarget: 'false' } },
    })).toThrow('hasMotionTarget');
  });

  it('reads a tool entry', () => {
    const tool = { ...entry, entryId: 'cell.scenario/hand', kind: 'tool', componentId: 'hand' };
    expect(parseComponentCatalog({ ...catalog, entries: [tool] }).entries[0].kind).toBe('tool');
  });

  it('reads a model payload when the profile has one, read-only package data', () => {
    const payload = { massKg: 0.6, centerOfMassM: [0, 0.01, 0.05] };
    const parsed = parseComponentCatalog({
      ...catalog,
      componentProfiles: { ...catalog.componentProfiles, hand_v0: { ...catalog.componentProfiles.hand_v0, payload } },
    });
    expect(parsed.componentProfiles.hand_v0.payload).toEqual(payload);
    expect(parseComponentCatalog(catalog).componentProfiles.hand_v0).not.toHaveProperty('payload');
    for (const invalid of [
      { massKg: -1, centerOfMassM: [0, 0, 0] },
      { massKg: 1, centerOfMassM: [0, 0] },
      { massKg: 1, centerOfMassM: [0, 0, 0], inertia: [] },
    ]) {
      expect(() => parseComponentCatalog({
        ...catalog,
        componentProfiles: { hand_v0: { tags: [], attachmentInterfaces: [], tcpTargets: [], payload: invalid } },
      })).toThrow('componentCatalog.componentProfiles.hand_v0.payload');
    }
  });

  it('refuses an unknown kind, version or extra field', () => {
    expect(() => parseComponentCatalog({ ...catalog, entries: [{ ...entry, kind: 'conveyor' }] }))
      .toThrow('componentCatalog.entries[0].kind');
    expect(() => parseComponentCatalog({ ...catalog, schemaVersion: 'v2' })).toThrow('schemaVersion');
    expect(() => parseComponentCatalog({ ...catalog, extra: true })).toThrow('componentCatalog');
    expect(() => parseComponentCatalog({ ...catalog, entries: [{ ...entry, components: [1] }] }))
      .toThrow('components[0]');
    expect(() => parseComponentCatalog({ ...catalog, componentProfiles: { arm_mock_v0: { tags: [] } } }))
      .toThrow('componentCatalog.componentProfiles.arm_mock_v0');
    expect(() => parseComponentCatalog({
      ...catalog,
      componentProfiles: { hand_v0: { tags: [], attachmentInterfaces: [], tcpTargets: [{ commandTargetId: 'gripper' }] } },
    })).toThrow('componentCatalog.componentProfiles.hand_v0.tcpTargets[0]');
  });
});

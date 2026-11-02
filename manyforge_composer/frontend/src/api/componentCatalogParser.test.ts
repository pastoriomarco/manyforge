import { describe, expect, it } from 'vitest';

import { parseComponentCatalog } from './componentCatalogParser';

const entry = {
  entryId: 'mock_lite6_pneumatic_pick_and_place.scenario/gripper_io',
  kind: 'io_device',
  componentId: 'gripper_io',
  componentProfileId: 'io_device_v0',
  modelFamilyId: null,
  tags: [],
  lane: 'manyforge-only',
  description: null,
  sources: ['examples/mock_lite6_pneumatic_pick_and_place.scenario'],
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
    expect(parseComponentCatalog(catalog)).toEqual({ ...catalog, machines: [] });
  });

  it('reads a machine entry (owner + member slots) apart from the component entries', () => {
    const machine = {
      kind: 'machine', model_id: 'ufactory.lite6_machine', title: 'UFactory Lite 6', lanes: ['mock', 'real'],
      default_owner_id: 'lite6',
      owner_row: { component_id: 'lite6', component_profile_id: 'ufactory.lite6_machine', configuration: {} },
      members: [
        { role: 'arm', model_id: 'ufactory.lite6', title: 'Arm',
          row: { component_id: 'lite6_arm', component_profile_id: 'ufactory.lite6',
            model_source: { urdf: { project_path: 'lite6.urdf' } } } },
        { role: 'controller_io', model_id: 'ufactory.lite6_controller_io', title: 'Controller I/O',
          row: { component_id: 'lite6_controller_io', component_profile_id: 'ufactory.lite6_controller_io' } },
      ],
    };
    const parsed = parseComponentCatalog({ ...catalog, entries: [machine, entry] });
    expect(parsed.entries).toEqual([entry]);
    expect(parsed.machines).toEqual([{
      modelId: 'ufactory.lite6_machine', title: 'UFactory Lite 6', lanes: ['mock', 'real'],
      defaultOwnerId: 'lite6', ownerRow: machine.owner_row,
      members: [
        { role: 'arm', modelId: 'ufactory.lite6', title: 'Arm', row: machine.members[0].row },
        { role: 'controller_io', modelId: 'ufactory.lite6_controller_io', title: 'Controller I/O',
          row: machine.members[1].row },
      ],
    }]);
    const withoutDefault = Object.fromEntries(Object.entries(machine).filter(([key]) => key !== 'default_owner_id'));
    expect(() => parseComponentCatalog({ ...catalog, entries: [withoutDefault] })).toThrow('entries[0]');
    expect(() => parseComponentCatalog({ ...catalog, entries: [{ ...machine, members: [{ role: 'arm' }] }] }))
      .toThrow('members[0]');
    expect(() => parseComponentCatalog({ ...catalog, entries: [{ ...machine, extra: 1 }] })).toThrow('entries[0]');
    // AUD-5b revised: no member is required; a `required_on_lanes` key is refused.
    const [arm, io] = machine.members;
    expect(() => parseComponentCatalog({ ...catalog, entries: [{ ...machine,
      members: [{ ...arm, required_on_lanes: ['real'] }, io] }] })).toThrow('members[0]');
  });

  it('reads a sensor model entry as an ordinary entry without lanes in the profile facts', () => {
    const sensor = {
      ...entry,
      entryId: 'model/manyforge.pointcloud_source',
      kind: 'sensor',
      componentId: 'pointcloud_source',
      componentProfileId: 'manyforge.pointcloud_source',
      lane: null,
      description: 'Point-cloud source (PointCloud2 to the 3D view)',
      sources: ['components/sensor/manyforge.pointcloud_source/component.yaml'],
      components: [{ component_id: 'pointcloud_source', component_profile_id: 'manyforge.pointcloud_source',
        target_settings: {}, configuration: { runtime_kind: 'sensor', runtime_configuration: {} } }],
    };
    const facts = { tags: [], attachmentInterfaces: [], tcpTargets: [], lanes: [] };
    const parsed = parseComponentCatalog({
      ...catalog, entries: [sensor], componentProfiles: { 'manyforge.pointcloud_source': facts },
    });
    expect(parsed.entries).toEqual([sensor]);
    expect(parsed.componentProfiles['manyforge.pointcloud_source'].lanes).toEqual([]);
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

import { renderToStaticMarkup } from 'react-dom/server';
import { describe, expect, it } from 'vitest';

import type { AttachedObjectPoseMeasurement } from '../state/runtimeScenePresentation';
import { CarriedObjectPoseCard } from './CarriedObjectPoseCard';

const measurement: AttachedObjectPoseMeasurement = {
  fresh: true,
  observedAtNs: '1000000000',
  sourceId: 'pose-source',
  sourceEpoch: 'pose-source.1',
  sourceSequence: 12,
  holder: {
    controllable_component_id: 'lite6_robot',
    command_target_id: 'gripper',
    gripper_link: 'robotiq_base_link',
  },
  measured_against: { entity: '/World/lite6/tool', link: 'robotiq_base_link' },
  source_header_frame_id: 'robotiq_header',
  frame: {
    kind: 'component',
    controllable_component_id: 'lite6_robot',
    model_instance_id: 'lite6.model',
    frame_role_id: 'tcp',
  },
  current_offset: {
    position_m: { x: 0.02, y: -0.01, z: 0.25 },
    orientation_xyzw: [0, 0, 0, 1],
  },
  baseline_offset: {
    position_m: { x: 0.01, y: -0.01, z: 0.24 },
    orientation_xyzw: [0, 0, 0, 1],
  },
  drift: { translation_m: 0.0141, rotation_rad: 0.02 },
};

describe('Carried object pose card', () => {
  it('shows owner-reported holder, body, frame, offset, baseline and drift', () => {
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={measurement} />);

    expect(html).toContain('lite6_robot');
    expect(html).toContain('tcp');
    expect(html).toContain('gripper');
    expect(html).toContain('robotiq_base_link');
    expect(html).toContain('/World/lite6/tool');
    expect(html).toContain('robotiq_base_link');
    expect(html).toContain('lite6.model');
    expect(html).toContain('Frame (offset expressed in)');
    expect(html).toContain('Current offset');
    expect(html).toContain('position [0.02, -0.01, 0.25] m');
    expect(html).toContain('Grasp baseline offset');
    expect(html).toContain('Drift from grasp baseline');
    expect(html).toContain('0.0141 m translation');
    expect(html).toContain('0.02 rad rotation');
    expect(html).toContain('Source header frame');
  });

  it('labels stale offsets as last reported and omits an absent baseline and drift', () => {
    const stale: AttachedObjectPoseMeasurement = {
      ...measurement,
      fresh: false,
    };
    delete stale.baseline_offset;
    delete stale.drift;
    if ('source_header_frame_id' in stale) delete stale.source_header_frame_id;
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={stale} />);

    expect(html).toContain('Stale pose sample; showing the last reported values.');
    expect(html).toContain('Last reported offset (stale)');
    expect(html).not.toContain('Grasp baseline offset');
    expect(html).not.toContain('Drift from grasp baseline');
    expect(html).not.toContain('Source header frame');
  });

  it('does not invent values while no resolved measurement is available', () => {
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={null} />);

    expect(html).toContain('No resolved carried-object pose measurement is available for this object.');
    expect(html).not.toContain('Current offset');
    expect(html).not.toContain('0.000');
  });

  it('shows a continuous grasp-loss measurement against the cell frame without a holder', () => {
    const continuous: AttachedObjectPoseMeasurement = {
      fresh: true,
      observedAtNs: '1000000000',
      sourceId: 'pose-source',
      sourceEpoch: 'pose-source.1',
      sourceSequence: 12,
      measured_against: 'cell frame',
      frame: measurement.frame,
      current_offset: measurement.current_offset,
      baseline_offset: measurement.baseline_offset,
      drift: measurement.drift,
    };
    const html = renderToStaticMarkup(<CarriedObjectPoseCard measurement={continuous} />);

    expect(html).toContain('Measured against');
    expect(html).toContain('cell frame');
    expect(html).toContain('Drift from grasp baseline');
    expect(html).not.toContain('<dt>Holder</dt>');
    expect(html).not.toContain('Source header frame');
  });
});

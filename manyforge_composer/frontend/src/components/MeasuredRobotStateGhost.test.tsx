import { renderToStaticMarkup } from 'react-dom/server';
import { expect, it, vi } from 'vitest';

import type { SceneUrdfManifest } from '../types/api';
import type {
  MeasuredRobotPresentation,
  MeasuredRobotPresentationReader,
  RuntimeScenePresentation,
} from '../state/runtimeScenePresentation';
import { MeasuredStateGhostLayers } from './StructuralViewer';

vi.mock('@react-three/fiber', async (importOriginal) => ({
  ...await importOriginal<typeof import('@react-three/fiber')>(),
  // Static markup has no Canvas; the ghost only needs the frame ingress.
  useFrame: () => undefined,
  useThree: (select: (state: { invalidate: () => void }) => unknown) => (
    select({ invalidate: () => undefined })
  ),
}));

const manifest: SceneUrdfManifest = {
  cell_id: 'cell-1',
  graph_revision: 'graph-1',
  retained_bundle_id: 'bundle-1',
  viewer_model_revision: 'viewer-1',
  model_key: 'model-1',
  root_link: 'base',
  projection: {
    kind: 'retained_neutral',
    source: 'active_retained_viewer_model',
    root_frame: 'base',
    position_unit: 'm',
    angle_unit: 'rad',
    quaternion_order: 'xyzw',
    joint_coordinate_policy: 'zero_independent_with_retained_mimic',
    operational_validity: 'not_asserted',
    runtime_state_used: false,
  },
  links: [{
    name: 'base',
    neutral_transform: { relative_to: 'base', position_m: [0, 0, 0], orientation_xyzw: [0, 0, 0, 1] },
    visuals: [{
      index: 0,
      mesh_url: null,
      primitive: { type: 'box', size_m: [0.2, 0.3, 0.4] },
      visual_origin: {
        position_m: [0.1, 0, 0],
        orientation_rpy_rad: [0, 0, 0],
        orientation_xyzw: [0, 0, 0, 1],
      },
    }],
    collisions: [],
  }],
  joints: [],
};

const empty: RuntimeScenePresentation = {
  measuredRobot: null,
  measuredRobotSources: [],
  measuredTcp: null,
  measuredObjects: [],
  attachedObjects: [],
  attachmentDiagnostics: [],
  status: { total: 0, freshGood: 0, staleGood: 0, unknown: 0, buffered: 0, warnings: [] },
};

function reader(presentation: MeasuredRobotPresentation | null): MeasuredRobotPresentationReader {
  return {
    readMeasuredRobotPresentation: () => presentation,
    subscribeMeasuredRobotPresentation: () => () => undefined,
    markMeasuredRobotPresentationApplied: () => undefined,
  };
}

function render(measuredRobot: MeasuredRobotPresentationReader, enabled = true): string {
  const consoleError = vi.spyOn(console, 'error').mockImplementation(() => undefined);
  try {
    return renderToStaticMarkup(
      <MeasuredStateGhostLayers
        enabled={enabled}
        presentation={empty}
        measuredRobot={measuredRobot}
        latestFrameAppliers={new Set()}
        manifest={manifest}
        objects={[]}
        ownerIdentity={{ ownerEpoch: 'owner-1', generation: 4 }}
        onAssetError={() => undefined}
      />,
    );
  } finally {
    consoleError.mockRestore();
  }
}

it('mounts the measured comparison robot at the stream pose and omits it without a measured row', () => {
  const measured: MeasuredRobotPresentation = {
    objectId: 'robot-1',
    sourceId: 'measured:arm',
    sourceEpoch: 'owner-1:render-1',
    sourceSequence: 9,
    observedAtNs: '1000' as MeasuredRobotPresentation['observedAtNs'],
    linkTransforms: { base: { position_m: [1, 2, 3], orientation_xyzw: [0, 0, 0, 1] } },
  };
  const markup = render(reader(measured));
  expect(markup).toContain('name="measured-state-robot:robot-1:measured:arm"');
  expect(markup).toContain('name="measured-state-link:base"');
  expect(markup).toContain('position="1,2,3"');
  expect(markup).toContain('position="0.1,0,0"');
  expect(markup).toContain('opacity="0.35"');
  expect(markup).toContain('renderOrder="14"');
  expect(markup).toContain('<boxGeometry args="0.2,0.3,0.4"></boxGeometry>');
  expect(render(reader(null))).not.toContain('measured-state-robot');
  expect(render(reader(measured), false)).toBe('');
});

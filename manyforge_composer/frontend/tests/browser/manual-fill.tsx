// Real-browser visual fixture for the production move Properties panel.
// It uses fixture owner state only and never calls a backend mutation.
import { useEffect, useState } from 'react';
import { createRoot } from 'react-dom/client';
import { ProgramPropertyEditor } from '../../src/components/ProgramPropertyEditor';
import { buildPoseMotionIntent } from '../../src/state/robotControl';
import { readyState, statusFixture } from '../../src/testFixtures/robotControl';
import type {
  AppliedProgramSelection,
  CatalogEntry,
  CatalogJsonSchema,
  ProgramAuthoringDocument,
  ProgramBindingDocument,
  ProgramTreeNode,
} from '../../src/types/api';
import type { ManualPreviewToNodeController } from '../../src/hooks/useRobotControl';
import '../../src/styles/index.css';

type FixtureScreen = { node: 'pose' | 'joint'; disabled: boolean };
const status = statusFixture();
const target = {
  component_id: status.directControl!.controllableComponentId,
  command_target_id: status.directControl!.commandTargetId,
};
const baseFrame = { component_id: target.component_id, frame_id: 'base' };
const tcpFrame = { component_id: target.component_id, frame_id: 'tcp' };
const robotTcpEntry = { target, frame: tcpFrame };
const programBinding = {
  target_bindings: [{
    target,
    frame_references: [baseFrame, tcpFrame],
    robot_tcps: [{
      entry: robotTcpEntry,
      frame: { reference: tcpFrame, compiled_frame_name: 'tool0',
        model_instance_id: 'robot-model', model_revision: 'sha256:model' },
      contact_body_role_ids: [], end_effector: true, component_default: true,
    }],
    default_tcp: robotTcpEntry,
  }],
  frame_bindings: [{
    reference: baseFrame, compiled_frame_name: 'base_link',
    model_instance_id: 'robot-model', model_revision: 'sha256:model',
  }],
} as unknown as ProgramBindingDocument;
const appliedProgram = {
  graphRevision: status.graphRevision,
  retainedBundleId: status.retainedBundleId,
  programBinding,
} as AppliedProgramSelection;
const frameObject: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['component_id', 'frame_id'],
  properties: { component_id: { type: 'string' }, frame_id: { type: 'string' } },
};
const targetObject: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['component_id', 'command_target_id'],
  properties: { component_id: { type: 'string' }, command_target_id: { type: 'string' } },
};
const frameReference: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['cell_frame', 'target_frame'] },
    frame: frameObject,
  },
};
const poseGoal: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['position_m', 'orientation'],
  properties: {
    frame_ref: frameReference,
    position_m: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
    orientation: {
      type: 'object', additionalProperties: false,
      properties: {
        rpy_deg: { type: 'array', minItems: 3, maxItems: 3, items: { type: 'number' } },
        xyzw: { type: 'array', minItems: 4, maxItems: 4, items: { type: 'number' } },
      },
      allOf: [{ oneOf: [{ required: ['rpy_deg'] }, { required: ['xyzw'] }] }],
    },
  },
};
const jointGoal: CatalogJsonSchema = {
  type: 'array', minItems: 1, maxItems: 6,
  items: {
    type: 'object', additionalProperties: false, required: ['joint_name', 'position_rad'],
    properties: {
      joint_name: { type: 'string', 'x-manyforge-semantic-type': 'source_joint' },
      position_rad: { type: 'number' },
    },
  },
};
const referenceSchema: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['type'],
  properties: {
    type: { type: 'string', enum: ['pose', 'component_frame'] },
    frame: frameObject,
  },
};
const tcpSchema: CatalogJsonSchema = {
  type: 'object', additionalProperties: false, required: ['kind'],
  properties: {
    kind: { type: 'string', enum: ['current', 'default_of_target', 'entry'] },
    target: targetObject, frame: frameObject,
  },
};
const limitsSchema: CatalogJsonSchema = {
  type: 'object',
  additionalProperties: {
    type: 'object', properties: {
      max_velocity_rad_s: { type: 'number' }, max_acceleration_rad_s2: { type: 'number' },
    },
  },
};

function parameter(id: string, schema: CatalogJsonSchema, required = false, defaultValue?: unknown) {
  return {
    id, required, schema, schemaHash: `sha256:${id}`,
    hasDefault: defaultValue !== undefined, ...(defaultValue === undefined ? {} : { default: defaultValue }),
  };
}
function entry(id: string, goalSchema: CatalogJsonSchema): CatalogEntry {
  return {
    id, name: id, category: 'motion', description: 'Browser fixture', stability: 'stable',
    kind: 'leaf', childrenMin: 0, childrenMax: 0,
    parameters: [
      parameter('target', { type: 'object', 'x-manyforge-semantic-type': 'target_reference' }, true),
      parameter('base_frame', { type: 'object', 'x-manyforge-semantic-type': 'frame_reference' }, true),
      parameter('tcp_endpoint', tcpSchema),
      parameter('target_mode', { type: 'string', enum: ['absolute', 'relative'] }, false, 'absolute'),
      parameter('target_reference', referenceSchema),
      parameter('offset_reference', referenceSchema),
      parameter('velocity_scale', { type: 'number' }),
      parameter('acceleration_scale', { type: 'number' }),
      parameter('max_tcp_linear_speed_mps', { type: 'number' }),
      parameter('per_joint_dynamics_limits', limitsSchema),
      parameter('goal', goalSchema, true),
    ],
    requiredCapabilities: [],
    execution: { blocking: true, cancellable: true, ownerApi: 'program' },
    runners: { py_trees: 'fixture' }, tags: [], minProgramProfile: 'composable-cell-v1',
    blackboard: { reads: [], writes: [] }, ownerContext: null, outcomes: {}, eventFamily: null,
  };
}
const entries = [entry('motion.move_to_pose', poseGoal), entry('motion.move_to_joint_target', jointGoal)];
const poseNode: ProgramTreeNode = {
  node_id: 'motion.move_to_pose', instance_id: 'pose_move', children: [],
  parameters: {
    target, base_frame: baseFrame, tcp_endpoint: { kind: 'current' },
    target_mode: 'relative', target_reference: { type: 'pose' }, offset_reference: { type: 'pose' },
    velocity_scale: 0.4, acceleration_scale: 0.5,
    per_joint_dynamics_limits: { joint1: { max_velocity_rad_s: 0.3 } },
    goal: { frame_ref: { kind: 'cell_frame' }, position_m: [0, 0, 0],
      orientation: { rpy_deg: [0, 0, 0] } },
  },
};
const jointNode: ProgramTreeNode = {
  node_id: 'motion.move_to_joint_target', instance_id: 'joint_move', children: [],
  parameters: {
    target, base_frame: baseFrame,
    goal: [{ joint_name: 'joint1', position_rad: 0.25 }],
  },
};
const program: ProgramAuthoringDocument = {
  api_version: 'manyforge.program.v3', program_id: 'manual_fill_fixture',
  name: 'Manual fill visual fixture', program_profile: 'composable-cell-v1',
  catalog: { api_version: 'manyforge.node_catalog.v3', content_version: 'fixture' },
  schema_version: 'manyforge.composer_authoring_forest.v2',
  forest: [poseNode, jointNode], positions: {},
};

const initialJointState = readyState();
const initialPoseState = {
  ...initialJointState,
  mode: 'pose_goal' as const,
  intent: buildPoseMotionIntent(status, initialJointState.draftPose, 'manual-fill-pose'),
};
function controllerFor(node: 'pose' | 'joint', disabled: boolean): ManualPreviewToNodeController {
  const state = node === 'pose' ? initialPoseState : initialJointState;
  return {
    state: disabled ? { ...state, phase: 'idle', intent: null, preview: null } : state,
    selectedStatus: status,
    currentStatus: status,
    manualControlAllowed: true,
    manualControlUnavailableReason: null,
    tcpApplying: false,
    authoringPending: false,
  };
}

const query = new URLSearchParams(location.search);
const inspectorWidth = `${Number(query.get('width') ?? 320)}px`;
const initial: FixtureScreen = {
  node: query.get('node') === 'joint' ? 'joint' : 'pose',
  disabled: query.get('disabled') === '1',
};
let setFixtureScreen: (next: FixtureScreen) => void = () => undefined;
function App() {
  const [screen, setScreen] = useState(initial);
  useEffect(() => { setFixtureScreen = setScreen; }, []);
  const node = screen.node === 'pose' ? poseNode : jointNode;
  const currentProgram = { ...program, forest: [node] };
  return (
    <main style={{ maxWidth: 880, margin: '1rem auto', padding: '0 1rem' }}>
      <nav aria-label="Manual fill fixture states" style={{ display: 'flex', gap: '0.5rem', marginBottom: '1rem' }}>
        <button type="button" data-testid="fixture-pose" onClick={() => setScreen({ node: 'pose', disabled: false })}>Pose node</button>
        <button type="button" data-testid="fixture-joint" onClick={() => setScreen({ node: 'joint', disabled: false })}>Joint target</button>
        <button type="button" data-testid="fixture-disabled" onClick={() => setScreen({ node: 'pose', disabled: true })}>Disabled preview</button>
      </nav>
      {/* The real Properties slot: the inspector section at its usual width. */}
      <aside className="program-inspector central-inspector-section active"
        style={{ width: inspectorWidth, height: 'auto' }} aria-label="Program inspector">
      <div className="program-inspector-scroll central-inspector-scroll">
      <ProgramPropertyEditor
        roots={[node]}
        selectedId={node.instance_id}
        catalogEntries={entries}
        program={currentProgram}
        appliedProgram={appliedProgram}
        manualPreviewController={controllerFor(screen.node, screen.disabled)}
        motionDescriptors={[status.directControl!.targetDescriptor]}
        busy={false}
        onMutate={() => undefined}
        onUpdateNodePolicies={() => undefined}
      />
      </div>
      </aside>
    </main>
  );
}
Object.assign(window, {
  manualFillFixture: {
    select(node: FixtureScreen['node'], disabled = false) { setFixtureScreen({ node, disabled }); },
    mutationCount: 0,
  },
});
createRoot(document.getElementById('root')!).render(<App />);

import * as THREE from 'three';

import type {
  LogicalSceneProjection,
  QuaternionXyzwTuple,
  SceneRuntimeGoodObservationPayload,
  SceneRuntimeObservationPose,
  SceneRuntimePoseSource,
  SceneRuntimeProjection,
  SceneUrdfJoint,
  SceneUrdfManifest,
  SourceModelProjection,
  Vector3Tuple,
} from '../types/api';

export interface RuntimeLinkTransform {
  position_m: Vector3Tuple;
  orientation_xyzw: QuaternionXyzwTuple;
}

export interface MeasuredRobotPresentation {
  objectId: string;
  sourceId: string;
  sourceSequence: number;
  linkTransforms: Record<string, RuntimeLinkTransform>;
}

export interface MeasuredPosePresentation {
  objectId: string;
  policyId: string;
  sourceId: string;
  sourceSequence: number;
  pose: SceneRuntimeObservationPose;
}

export interface RuntimeScenePresentation {
  measuredRobot: MeasuredRobotPresentation | null;
  measuredTcp: MeasuredPosePresentation | null;
  measuredObjects: MeasuredPosePresentation[];
  status: {
    total: number;
    freshGood: number;
    staleGood: number;
    unknown: number;
    buffered: number;
    warnings: string[];
  };
}

const EMPTY_PRESENTATION: RuntimeScenePresentation = {
  measuredRobot: null,
  measuredTcp: null,
  measuredObjects: [],
  status: {
    total: 0,
    freshGood: 0,
    staleGood: 0,
    unknown: 0,
    buffered: 0,
    warnings: [],
  },
};

type GoodObservationSource = Extract<
  SceneRuntimePoseSource,
  { quality: 'good' }
>;
type JointObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    { kind: 'joint_state' }
  >;
};
type PoseObservationSource = GoodObservationSource & {
  payload: Extract<
    SceneRuntimeGoodObservationPayload,
    { kind: 'simulation_entity_pose' | 'detection3d_pose' }
  >;
};

function isFreshGood(source: SceneRuntimePoseSource): source is GoodObservationSource {
  return source.quality === 'good' && source.fresh;
}

function isJointObservation(
  source: GoodObservationSource,
): source is JointObservationSource {
  return source.payload.kind === 'joint_state';
}

function isPoseObservation(
  source: GoodObservationSource,
): source is PoseObservationSource {
  return source.payload.kind === 'simulation_entity_pose'
    || source.payload.kind === 'detection3d_pose';
}

/**
 * Project fresh Manager-owned observations onto one retained viewer model.
 *
 * The retained viewer artifact currently carries no occurrence identity, so a
 * measured robot overlay is admitted only for a one-occurrence cell. A
 * multi-occurrence cell remains visible at retained-neutral coordinates and
 * reports the missing exact mapping instead of cross-driving lookalike models.
 */
export function runtimeScenePresentation(
  sceneRuntime: SceneRuntimeProjection | null,
  logicalScene: LogicalSceneProjection | null,
  sourceModel: SourceModelProjection | null,
  manifest: SceneUrdfManifest | null,
): RuntimeScenePresentation {
  if (
    sceneRuntime === null
    || logicalScene === null
    || logicalScene.snapshot === null
    || sourceModel === null
    || manifest === null
  ) return EMPTY_PRESENTATION;

  const warnings: string[] = [];
  const identity = sceneRuntime.state.runtime_identity;
  const projection = logicalScene.access.active_projection;
  if (
    identity === null
    || projection === null
    || sceneRuntime.state.state !== 'open'
    || sceneRuntime.cell_id !== sourceModel.cell_id
    || sceneRuntime.cell_id !== logicalScene.snapshot.cell_id
    || sceneRuntime.cell_id !== manifest.cell_id
    || identity.retained_bundle_id !== sourceModel.retained_bundle_id
    || identity.graph_revision !== sourceModel.graph_revision
    || identity.retained_bundle_id !== manifest.retained_bundle_id
    || identity.graph_revision !== manifest.graph_revision
    || identity.retained_bundle_id !== projection.retained_bundle_id
    || identity.graph_revision !== projection.graph_revision
  ) {
    return {
      ...EMPTY_PRESENTATION,
      status: {
        ...EMPTY_PRESENTATION.status,
        warnings: ['Measured presentation identities do not match.'],
      },
    };
  }

  const sources = sceneRuntime.state.pose_sources;
  const fresh = sources.filter(isFreshGood);
  const measuredRobotSources = fresh.filter(
    (source): source is JointObservationSource => isJointObservation(source)
      && source.policy_id === 'viewer.measured_robot',
  );
  let measuredRobot: MeasuredRobotPresentation | null = null;
  if (measuredRobotSources.length > 1) {
    warnings.push('Measured robot observation is ambiguous.');
  } else if (measuredRobotSources.length === 1) {
    const source = measuredRobotSources[0];
    if (sourceModel.occurrences.length !== 1) {
      warnings.push(
        'Measured robot overlay requires an occurrence-keyed viewer artifact.',
      );
    } else {
      const occurrence = sourceModel.occurrences[0];
      const coordinateNames = new Set(
        occurrence.coordinates.map((coordinate) => coordinate.source_joint_name),
      );
      if (
        occurrence.component_id !== source.object_id
        || source.payload.joint_names.some((name) => !coordinateNames.has(name))
      ) {
        warnings.push('Measured robot observation does not match the retained occurrence.');
      } else {
        const positions = new Map(
          source.payload.joint_names.map((name, index) => [
            name,
            source.payload.positions[index],
          ]),
        );
        const linkTransforms = measuredLinkTransforms(manifest, positions);
        if (linkTransforms === null) {
          warnings.push('Measured robot coordinates do not resolve on the viewer model.');
        } else {
          measuredRobot = {
            objectId: source.object_id,
            sourceId: source.source_id,
            sourceSequence: source.source_sequence,
            linkTransforms,
          };
        }
      }
    }
  }

  const sceneObjectIds = new Set(
    logicalScene.snapshot.objects.map((object) => object.scene_resource_id),
  );
  const objectSources = fresh.filter(
    (source): source is PoseObservationSource => isPoseObservation(source)
      && source.eligible
      && sceneObjectIds.has(source.object_id),
  );
  const measuredObjects: MeasuredPosePresentation[] = [];
  for (const objectId of [...new Set(objectSources.map((source) => source.object_id))]
    .sort()) {
    const candidates = objectSources.filter((source) => source.object_id === objectId);
    if (candidates.length !== 1) {
      warnings.push(`Measured object ${objectId} has ambiguous pose policies.`);
      continue;
    }
    const source = candidates[0];
    measuredObjects.push({
      objectId,
      policyId: source.policy_id,
      sourceId: source.source_id,
      sourceSequence: source.source_sequence,
      pose: source.payload.pose,
    });
  }

  const tcpSources = fresh.filter(
    (source): source is PoseObservationSource => isPoseObservation(source)
      && source.policy_id === 'viewer.measured_tcp',
  );
  let measuredTcp: MeasuredPosePresentation | null = null;
  if (tcpSources.length > 1) {
    warnings.push('Measured TCP observation is ambiguous.');
  } else if (tcpSources.length === 1) {
    const source = tcpSources[0];
    measuredTcp = {
      objectId: source.object_id,
      policyId: source.policy_id,
      sourceId: source.source_id,
      sourceSequence: source.source_sequence,
      pose: source.payload.pose,
    };
  }

  return {
    measuredRobot,
    measuredTcp,
    measuredObjects,
    status: {
      total: sources.length,
      freshGood: fresh.length,
      staleGood: sources.filter(
        (source) => source.quality === 'good' && !source.fresh,
      ).length,
      unknown: sources.filter((source) => source.quality === 'unknown').length,
      buffered: sources.filter((source) => source.buffered).length,
      warnings,
    },
  };
}

export function measuredLinkTransforms(
  manifest: SceneUrdfManifest,
  observedPositions: ReadonlyMap<string, number>,
): Record<string, RuntimeLinkTransform> | null {
  const linkNames = new Set(manifest.links.map((link) => link.name));
  const jointByName = new Map(manifest.joints.map((joint) => [joint.name, joint]));
  if (
    !linkNames.has(manifest.root_link)
    || [...observedPositions].some(([name, value]) => (
      !jointByName.has(name) || !Number.isFinite(value)
    ))
  ) return null;

  const coordinates = new Map<string, number>();
  const resolving = new Set<string>();
  const coordinate = (joint: SceneUrdfJoint): number | null => {
    const cached = coordinates.get(joint.name);
    if (cached !== undefined) return cached;
    if (resolving.has(joint.name)) return null;
    resolving.add(joint.name);
    let value = observedPositions.get(joint.name)
      ?? joint.neutral_position
      ?? 0;
    if (joint.mimic !== null) {
      const master = jointByName.get(joint.mimic.master);
      const masterValue = master === undefined ? null : coordinate(master);
      if (masterValue === null) return null;
      value = (masterValue * joint.mimic.multiplier) + joint.mimic.offset;
    }
    resolving.delete(joint.name);
    if (!Number.isFinite(value)) return null;
    coordinates.set(joint.name, value);
    return value;
  };
  for (const joint of manifest.joints) {
    if (coordinate(joint) === null) return null;
  }

  const matrices = new Map<string, THREE.Matrix4>();
  matrices.set(manifest.root_link, new THREE.Matrix4().identity());
  const pending = [...manifest.joints];
  for (let pass = 0; pass <= manifest.joints.length && pending.length > 0; pass += 1) {
    let progressed = false;
    for (let index = pending.length - 1; index >= 0; index -= 1) {
      const joint = pending[index];
      const parent = matrices.get(joint.parent_link);
      if (parent === undefined) continue;
      const motion = jointMotion(joint, coordinates.get(joint.name) ?? 0);
      if (motion === null || !linkNames.has(joint.child_link)) return null;
      const origin = new THREE.Matrix4().compose(
        new THREE.Vector3(...joint.origin.position_m),
        new THREE.Quaternion(...joint.origin.orientation_xyzw).normalize(),
        new THREE.Vector3(1, 1, 1),
      );
      matrices.set(
        joint.child_link,
        parent.clone().multiply(origin).multiply(motion),
      );
      pending.splice(index, 1);
      progressed = true;
    }
    if (!progressed) break;
  }
  if (pending.length > 0) return null;

  const result: Record<string, RuntimeLinkTransform> = {};
  for (const link of manifest.links) {
    const matrix = matrices.get(link.name);
    if (matrix === undefined) return null;
    const position = new THREE.Vector3();
    const orientation = new THREE.Quaternion();
    const scale = new THREE.Vector3();
    matrix.decompose(position, orientation, scale);
    if (![
      position.x,
      position.y,
      position.z,
      orientation.x,
      orientation.y,
      orientation.z,
      orientation.w,
    ].every(Number.isFinite)) return null;
    result[link.name] = {
      position_m: [position.x, position.y, position.z],
      orientation_xyzw: [
        orientation.x,
        orientation.y,
        orientation.z,
        orientation.w,
      ],
    };
  }
  return result;
}

function jointMotion(joint: SceneUrdfJoint, value: number): THREE.Matrix4 | null {
  const axis = new THREE.Vector3(...joint.axis);
  if (joint.type === 'revolute' || joint.type === 'continuous') {
    if (axis.lengthSq() < 1e-24) return null;
    return new THREE.Matrix4().makeRotationFromQuaternion(
      new THREE.Quaternion().setFromAxisAngle(axis.normalize(), value),
    );
  }
  if (joint.type === 'prismatic') {
    if (axis.lengthSq() < 1e-24) return null;
    axis.normalize().multiplyScalar(value);
    return new THREE.Matrix4().makeTranslation(axis.x, axis.y, axis.z);
  }
  if (joint.type === 'fixed') return new THREE.Matrix4().identity();
  return null;
}

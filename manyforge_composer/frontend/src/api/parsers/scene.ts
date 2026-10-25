import { parseNanoseconds } from '../../types/nanoseconds';
import type {
  RetainedNeutralProjection,
  DraftMutationResponse,
  ProgramDraftMeshAssetMetadata,
  FileBrowserEntry,
  FileBrowserResponse,
  CarriedObjectAttachment,
  LogicalSceneObject,
  LogicalScenePlacement,
  LogicalScenePose,
  LogicalSceneProjection,
  LogicalSceneShape,
  SceneRuntimeCellObserver,
  SceneRuntimeCellObserverExit,
  SceneRuntimeCellObserverIncidentPersistence,
  SceneRuntimeConfiguredPoseSource,
  SceneRuntimeProjection,
  SceneRuntimeGoodObservationPayload,
  SceneRuntimeObservationPose,
  SceneRuntimePoseSource,
  SceneRuntimeUnknownObservationPayload,
  SceneMeshAssetMetadata,
  SceneTransactionResponse,
  SceneUrdfCollision,
  SceneUrdfJoint,
  SceneUrdfLink,
  SceneUrdfManifest,
  SceneUrdfOrigin,
  SceneUrdfPrimitive,
  SceneUrdfVisual,
  SourceModelCoordinate,
  SourceModelOccurrence,
  SourceModelProjection,
  SourceModelSrdfGroup,
  SourceModelTarget,
} from '../../types/api';
import {
  arrayAt,
  booleanAt,
  boundedArrayAt,
  digestAt,
  exactKeysAt,
  fail,
  integerAt,
  jsonRecordAt,
  monotonicNanosecondsAt,
  nonEmptyStringAt,
  nullableNumberAt,
  nullableStringAt,
  numberAt,
  objectAt,
  parseProjectionRevision,
  positiveIntegerAt,
  positiveNumberAt,
  quaternionAt,
  stringArrayAt,
  stringAt,
  vector3At,
} from './shared';
import { parseAuthoringWorkspaceSnapshot, parseProgramWorkspaceResponse } from './workspace';

function parseSourceModelCoordinate(
  value: unknown,
  path: string,
): SourceModelCoordinate {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'coordinate_kind',
      'joint_type',
      'mimic',
      'source_child_link_name',
      'source_joint_index',
      'source_joint_name',
      'source_parent_link_name',
      'unit',
      'viewer_child_link_name',
      'viewer_joint_name',
      'viewer_parent_link_name',
    ],
    path,
  );
  const mimic = record.mimic === null
    ? null
    : (() => {
        const row = objectAt(record.mimic, `${path}.mimic`);
        exactKeysAt(row, ['multiplier', 'offset', 'source_joint_name'], `${path}.mimic`);
        return {
          multiplier: numberAt(row.multiplier, `${path}.mimic.multiplier`),
          offset: numberAt(row.offset, `${path}.mimic.offset`),
          source_joint_name: stringAt(
            row.source_joint_name,
            `${path}.mimic.source_joint_name`,
          ),
        };
      })();
  return {
    coordinate_kind: stringAt(record.coordinate_kind, `${path}.coordinate_kind`),
    joint_type: stringAt(record.joint_type, `${path}.joint_type`),
    mimic,
    source_child_link_name: stringAt(
      record.source_child_link_name,
      `${path}.source_child_link_name`,
    ),
    source_joint_index: integerAt(
      record.source_joint_index,
      `${path}.source_joint_index`,
    ),
    source_joint_name: stringAt(record.source_joint_name, `${path}.source_joint_name`),
    source_parent_link_name: stringAt(
      record.source_parent_link_name,
      `${path}.source_parent_link_name`,
    ),
    unit: stringAt(record.unit, `${path}.unit`),
    viewer_child_link_name: stringAt(
      record.viewer_child_link_name,
      `${path}.viewer_child_link_name`,
    ),
    viewer_joint_name: stringAt(record.viewer_joint_name, `${path}.viewer_joint_name`),
    viewer_parent_link_name: stringAt(
      record.viewer_parent_link_name,
      `${path}.viewer_parent_link_name`,
    ),
  };
}

function parseSourceModelSrdfGroup(
  value: unknown,
  path: string,
): SourceModelSrdfGroup {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'chains',
      'source_group_name',
      'source_joint_names',
      'source_link_names',
      'source_subgroup_names',
    ],
    path,
  );
  return {
    chains: boundedArrayAt(record.chains, `${path}.chains`, 4096).map(
      (item, index) => {
        const chainPath = `${path}.chains[${index}]`;
        const chain = objectAt(item, chainPath);
        exactKeysAt(
          chain,
          ['source_base_link_name', 'source_tip_link_name'],
          chainPath,
        );
        return {
          source_base_link_name: stringAt(
            chain.source_base_link_name,
            `${chainPath}.source_base_link_name`,
          ),
          source_tip_link_name: stringAt(
            chain.source_tip_link_name,
            `${chainPath}.source_tip_link_name`,
          ),
        };
      },
    ),
    source_group_name: stringAt(record.source_group_name, `${path}.source_group_name`),
    source_joint_names: stringArrayAt(record.source_joint_names, `${path}.source_joint_names`),
    source_link_names: stringArrayAt(record.source_link_names, `${path}.source_link_names`),
    source_subgroup_names: stringArrayAt(
      record.source_subgroup_names,
      `${path}.source_subgroup_names`,
    ),
  };
}

function parseSourceModelOccurrence(
  value: unknown,
  path: string,
): SourceModelOccurrence {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'component_id',
      'coordinates',
      'links',
      'model_asset_id',
      'model_instance_id',
      'model_revision',
      'model_role_id',
      'source_model_name',
      'source_robot_name',
      'srdf_end_effectors',
      'srdf_group_states',
      'srdf_groups',
    ],
    path,
  );
  const coordinates = boundedArrayAt(record.coordinates, `${path}.coordinates`, 100_000)
    .map((item, index) => parseSourceModelCoordinate(item, `${path}.coordinates[${index}]`));
  if (coordinates.some((item, index) => item.source_joint_index !== index)) {
    return fail(`${path}.coordinates`, 'source-indexed coordinates in source order');
  }
  const links = boundedArrayAt(record.links, `${path}.links`, 100_000)
    .map((item, index) => {
      const linkPath = `${path}.links[${index}]`;
      const link = objectAt(item, linkPath);
      exactKeysAt(link, ['source_link_name', 'viewer_link_name'], linkPath);
      return {
        source_link_name: stringAt(link.source_link_name, `${linkPath}.source_link_name`),
        viewer_link_name: stringAt(link.viewer_link_name, `${linkPath}.viewer_link_name`),
      };
    });
  if (
    new Set(links.map((link) => link.source_link_name)).size !== links.length
    || new Set(links.map((link) => link.viewer_link_name)).size !== links.length
    || [...links].sort((left, right) => (
      left.source_link_name < right.source_link_name ? -1
        : left.source_link_name > right.source_link_name ? 1 : 0
    )).some((link, index) => link !== links[index])
  ) {
    return fail(`${path}.links`, 'canonical unique source/viewer link bindings');
  }
  const viewerLinkBySource = new Map(
    links.map((link) => [link.source_link_name, link.viewer_link_name]),
  );
  if (
    new Set(coordinates.map((coordinate) => coordinate.source_joint_name)).size
      !== coordinates.length
    || new Set(coordinates.map((coordinate) => coordinate.viewer_joint_name)).size
      !== coordinates.length
    || coordinates.some((coordinate) => (
      viewerLinkBySource.get(coordinate.source_parent_link_name)
        !== coordinate.viewer_parent_link_name
      || viewerLinkBySource.get(coordinate.source_child_link_name)
        !== coordinate.viewer_child_link_name
    ))
  ) {
    return fail(`${path}.coordinates`, 'exact unique source/viewer coordinate bindings');
  }
  return {
    component_id: nullableStringAt(record.component_id, `${path}.component_id`),
    coordinates,
    links,
    model_asset_id: stringAt(record.model_asset_id, `${path}.model_asset_id`),
    model_instance_id: stringAt(record.model_instance_id, `${path}.model_instance_id`),
    model_revision: stringAt(record.model_revision, `${path}.model_revision`),
    model_role_id: nullableStringAt(record.model_role_id, `${path}.model_role_id`),
    source_model_name: stringAt(record.source_model_name, `${path}.source_model_name`),
    source_robot_name: stringAt(record.source_robot_name, `${path}.source_robot_name`),
    srdf_end_effectors: boundedArrayAt(
      record.srdf_end_effectors,
      `${path}.srdf_end_effectors`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.srdf_end_effectors[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        [
          'source_end_effector_name',
          'source_group_name',
          'source_parent_group_name',
          'source_parent_link_name',
        ],
        rowPath,
      );
      return {
        source_end_effector_name: stringAt(
          row.source_end_effector_name,
          `${rowPath}.source_end_effector_name`,
        ),
        source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
        source_parent_group_name: nullableStringAt(
          row.source_parent_group_name,
          `${rowPath}.source_parent_group_name`,
        ),
        source_parent_link_name: stringAt(
          row.source_parent_link_name,
          `${rowPath}.source_parent_link_name`,
        ),
      };
    }),
    srdf_group_states: boundedArrayAt(
      record.srdf_group_states,
      `${path}.srdf_group_states`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.srdf_group_states[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['source_group_name', 'source_group_state_name', 'source_joint_names'],
        rowPath,
      );
      return {
        source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
        source_group_state_name: stringAt(
          row.source_group_state_name,
          `${rowPath}.source_group_state_name`,
        ),
        source_joint_names: stringArrayAt(
          row.source_joint_names,
          `${rowPath}.source_joint_names`,
        ),
      };
    }),
    srdf_groups: boundedArrayAt(record.srdf_groups, `${path}.srdf_groups`, 4096)
      .map((item, index) => parseSourceModelSrdfGroup(item, `${path}.srdf_groups[${index}]`)),
  };
}

function parseSourceModelTarget(value: unknown, path: string): SourceModelTarget {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['collision_bodies', 'component_id', 'frames', 'model_groups', 'target_id'],
    path,
  );
  return {
    collision_bodies: boundedArrayAt(
      record.collision_bodies,
      `${path}.collision_bodies`,
      4096,
    ).map((item, index) => {
      const rowPath = `${path}.collision_bodies[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['collision_body_role_id', 'model_instance_id', 'source_link_names'],
        rowPath,
      );
      return {
        collision_body_role_id: stringAt(
          row.collision_body_role_id,
          `${rowPath}.collision_body_role_id`,
        ),
        model_instance_id: stringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
        source_link_names: stringArrayAt(row.source_link_names, `${rowPath}.source_link_names`),
      };
    }),
    component_id: stringAt(record.component_id, `${path}.component_id`),
    frames: boundedArrayAt(record.frames, `${path}.frames`, 4096).map((item, index) => {
      const rowPath = `${path}.frames[${index}]`;
      const row = objectAt(item, rowPath);
      exactKeysAt(
        row,
        ['frame_kind', 'frame_role_id', 'model_instance_id', 'source_link_name'],
        rowPath,
      );
      const frameKind = stringAt(row.frame_kind, `${rowPath}.frame_kind`);
      if (!['cell_frame', 'component_model_frame', 'passive_model_frame'].includes(frameKind)) {
        return fail(`${rowPath}.frame_kind`, 'a supported source frame kind');
      }
      return {
        frame_kind: frameKind as SourceModelTarget['frames'][number]['frame_kind'],
        frame_role_id: stringAt(row.frame_role_id, `${rowPath}.frame_role_id`),
        model_instance_id: nullableStringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
        source_link_name: nullableStringAt(row.source_link_name, `${rowPath}.source_link_name`),
      };
    }),
    model_groups: boundedArrayAt(record.model_groups, `${path}.model_groups`, 4096)
      .map((item, index) => {
        const rowPath = `${path}.model_groups[${index}]`;
        const row = objectAt(item, rowPath);
        exactKeysAt(
          row,
          [
            'commandable_source_joint_names',
            'model_group_role_id',
            'model_instance_id',
            'source_group_name',
            'source_joint_names',
            'source_link_names',
          ],
          rowPath,
        );
        return {
          commandable_source_joint_names: stringArrayAt(
            row.commandable_source_joint_names,
            `${rowPath}.commandable_source_joint_names`,
          ),
          model_group_role_id: stringAt(row.model_group_role_id, `${rowPath}.model_group_role_id`),
          model_instance_id: stringAt(row.model_instance_id, `${rowPath}.model_instance_id`),
          source_group_name: stringAt(row.source_group_name, `${rowPath}.source_group_name`),
          source_joint_names: stringArrayAt(row.source_joint_names, `${rowPath}.source_joint_names`),
          source_link_names: stringArrayAt(row.source_link_names, `${rowPath}.source_link_names`),
        };
      }),
    target_id: stringAt(record.target_id, `${path}.target_id`),
  };
}

export function parseSourceModelProjection(value: unknown): SourceModelProjection {
  const record = objectAt(value, 'sourceModel');
  exactKeysAt(
    record,
    [
      'schema_version',
      'cell_id',
      'retained_bundle_id',
      'graph_revision',
      'model_map_contract',
      'model_map_product_id',
      'occurrences',
      'targets',
      'source',
    ],
    'sourceModel',
  );
  const schemaVersion = stringAt(record.schema_version, 'sourceModel.schema_version');
  if (schemaVersion !== 'manyforge.source_model_projection.v2') {
    return fail('sourceModel.schema_version', 'manyforge.source_model_projection.v2');
  }
  const source = objectAt(record.source, 'sourceModel.source');
  exactKeysAt(
    source,
    [
      'available',
      'freshness',
      'known_gaps',
      'projection_generation',
      'projection_owner_epoch',
      'receiver_age_ns',
      'selection_projection_generation',
      'selection_source_content_digest',
      'selection_source_generation',
      'selection_source_owner_epoch',
      'selection_source_owner_id',
      'source_content_digest',
      'source_generation',
      'source_owner_epoch',
      'source_owner_id',
      'source_owner_kind',
    ],
    'sourceModel.source',
  );
  if (source.available !== true || source.freshness !== 'fresh') {
    return fail('sourceModel.source', 'available and fresh');
  }
  if (arrayAt(source.known_gaps, 'sourceModel.source.known_gaps').length !== 0) {
    return fail('sourceModel.source.known_gaps', 'empty');
  }
  if (source.source_owner_kind !== 'source_model_projection') {
    return fail('sourceModel.source.source_owner_kind', 'source_model_projection');
  }
  const occurrences = boundedArrayAt(record.occurrences, 'sourceModel.occurrences', 4096)
    .map((item, index) => parseSourceModelOccurrence(item, `sourceModel.occurrences[${index}]`));
  const occurrenceIds = occurrences.map((item) => item.model_instance_id);
  if (new Set(occurrenceIds).size !== occurrenceIds.length) {
    return fail('sourceModel.occurrences', 'unique model_instance_id values');
  }
  const targets = boundedArrayAt(record.targets, 'sourceModel.targets', 4096)
    .map((item, index) => parseSourceModelTarget(item, `sourceModel.targets[${index}]`));
  let modelMapContract: SourceModelProjection['model_map_contract'] = null;
  let modelMapProductId: string | null = null;
  if (occurrences.length === 0) {
    if (record.model_map_contract !== null || record.model_map_product_id !== null) {
      return fail('sourceModel.model_map_contract', 'null for an empty occurrence set');
    }
  } else {
    const contract = objectAt(record.model_map_contract, 'sourceModel.model_map_contract');
    exactKeysAt(contract, ['contract_id', 'contract_revision'], 'sourceModel.model_map_contract');
    modelMapContract = {
      contract_id: stringAt(contract.contract_id, 'sourceModel.model_map_contract.contract_id'),
      contract_revision: stringAt(
        contract.contract_revision,
        'sourceModel.model_map_contract.contract_revision',
      ),
    };
    modelMapProductId = stringAt(record.model_map_product_id, 'sourceModel.model_map_product_id');
  }
  return {
    schema_version: schemaVersion,
    cell_id: stringAt(record.cell_id, 'sourceModel.cell_id'),
    retained_bundle_id: stringAt(record.retained_bundle_id, 'sourceModel.retained_bundle_id'),
    graph_revision: stringAt(record.graph_revision, 'sourceModel.graph_revision'),
    model_map_contract: modelMapContract,
    model_map_product_id: modelMapProductId,
    occurrences,
    targets,
    source: {
      available: true,
      freshness: 'fresh',
      known_gaps: [],
      projection_generation: integerAt(source.projection_generation, 'sourceModel.source.projection_generation'),
      projection_owner_epoch: stringAt(source.projection_owner_epoch, 'sourceModel.source.projection_owner_epoch'),
      receiver_age_ns: integerAt(source.receiver_age_ns, 'sourceModel.source.receiver_age_ns'),
      selection_projection_generation: integerAt(source.selection_projection_generation, 'sourceModel.source.selection_projection_generation'),
      selection_source_content_digest: stringAt(source.selection_source_content_digest, 'sourceModel.source.selection_source_content_digest'),
      selection_source_generation: integerAt(source.selection_source_generation, 'sourceModel.source.selection_source_generation'),
      selection_source_owner_epoch: stringAt(source.selection_source_owner_epoch, 'sourceModel.source.selection_source_owner_epoch'),
      selection_source_owner_id: stringAt(source.selection_source_owner_id, 'sourceModel.source.selection_source_owner_id'),
      source_content_digest: stringAt(source.source_content_digest, 'sourceModel.source.source_content_digest'),
      source_generation: integerAt(source.source_generation, 'sourceModel.source.source_generation'),
      source_owner_epoch: stringAt(source.source_owner_epoch, 'sourceModel.source.source_owner_epoch'),
      source_owner_id: stringAt(source.source_owner_id, 'sourceModel.source.source_owner_id'),
      source_owner_kind: 'source_model_projection',
    },
  };
}

function parseLogicalScenePose(value: unknown, path: string): LogicalScenePose {
  const record = objectAt(value, path);
  exactKeysAt(record, ['position_m', 'orientation_xyzw'], path);
  const position = objectAt(record.position_m, `${path}.position_m`);
  exactKeysAt(position, ['x', 'y', 'z'], `${path}.position_m`);
  return {
    position_m: {
      x: numberAt(position.x, `${path}.position_m.x`),
      y: numberAt(position.y, `${path}.position_m.y`),
      z: numberAt(position.z, `${path}.position_m.z`),
    },
    orientation_xyzw: quaternionAt(record.orientation_xyzw, `${path}.orientation_xyzw`),
  };
}

function parseLogicalSceneShape(value: unknown, path: string): LogicalSceneShape {
  const record = objectAt(value, path);
  const type = stringAt(record.type, `${path}.type`);
  if (type === 'box') {
    exactKeysAt(record, ['dimensions_m', 'type'], path);
    const dimensions = objectAt(record.dimensions_m, `${path}.dimensions_m`);
    exactKeysAt(dimensions, ['x', 'y', 'z'], `${path}.dimensions_m`);
    return {
      type,
      dimensions_m: {
        x: positiveNumberAt(dimensions.x, `${path}.dimensions_m.x`),
        y: positiveNumberAt(dimensions.y, `${path}.dimensions_m.y`),
        z: positiveNumberAt(dimensions.z, `${path}.dimensions_m.z`),
      },
    };
  }
  if (type === 'sphere') {
    exactKeysAt(record, ['radius_m', 'type'], path);
    return { type, radius_m: positiveNumberAt(record.radius_m, `${path}.radius_m`) };
  }
  if (type === 'cylinder') {
    exactKeysAt(record, ['height_m', 'radius_m', 'type'], path);
    return {
      type,
      radius_m: positiveNumberAt(record.radius_m, `${path}.radius_m`),
      height_m: positiveNumberAt(record.height_m, `${path}.height_m`),
    };
  }
  if (type === 'mesh') {
    exactKeysAt(record, ['resource_uri', 'scale', 'type'], path);
    const scale = objectAt(record.scale, `${path}.scale`);
    exactKeysAt(scale, ['x', 'y', 'z'], `${path}.scale`);
    return {
      type,
      resource_uri: stringAt(record.resource_uri, `${path}.resource_uri`),
      scale: {
        x: positiveNumberAt(scale.x, `${path}.scale.x`),
        y: positiveNumberAt(scale.y, `${path}.scale.y`),
        z: positiveNumberAt(scale.z, `${path}.scale.z`),
      },
    };
  }
  return fail(`${path}.type`, 'box, sphere, cylinder, or mesh');
}

function parseLogicalSceneObject(value: unknown, path: string): LogicalSceneObject {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['collision_shapes', 'placement', 'provider_evidence', 'scene_resource_id', 'support_contacts'],
    path,
  );
  const placementRecord = objectAt(record.placement, `${path}.placement`);
  const kind = stringAt(placementRecord.kind, `${path}.placement.kind`);
  let placement: LogicalScenePlacement;
  if (kind === 'cell_fixed') {
    exactKeysAt(placementRecord, ['cell_id', 'kind', 'pose_in_cell'], `${path}.placement`);
    placement = {
      kind,
      cell_id: stringAt(placementRecord.cell_id, `${path}.placement.cell_id`),
      pose_in_cell: parseLogicalScenePose(
        placementRecord.pose_in_cell,
        `${path}.placement.pose_in_cell`,
      ),
    };
  } else if (kind === 'carried') {
    exactKeysAt(
      placementRecord,
      ['carried_object_attachment_id', 'carried_object_attachment_revision', 'kind'],
      `${path}.placement`,
    );
    placement = {
      kind,
      carried_object_attachment_id: stringAt(
        placementRecord.carried_object_attachment_id,
        `${path}.placement.carried_object_attachment_id`,
      ),
      carried_object_attachment_revision: stringAt(
        placementRecord.carried_object_attachment_revision,
        `${path}.placement.carried_object_attachment_revision`,
      ),
    };
  } else {
    return fail(`${path}.placement.kind`, 'cell_fixed or carried');
  }
  return {
    scene_resource_id: stringAt(record.scene_resource_id, `${path}.scene_resource_id`),
    collision_shapes: boundedArrayAt(record.collision_shapes, `${path}.collision_shapes`, 4096)
      .map((item, index) => parseLogicalSceneShape(item, `${path}.collision_shapes[${index}]`)),
    placement,
    provider_evidence: record.provider_evidence === null
      ? null
      : jsonRecordAt(record.provider_evidence, `${path}.provider_evidence`),
    support_contacts: boundedArrayAt(record.support_contacts, `${path}.support_contacts`, 4096)
      .map((item, index) => jsonRecordAt(item, `${path}.support_contacts[${index}]`)),
  };
}

function parseCarriedObjectAttachment(
  value: unknown,
  path: string,
): CarriedObjectAttachment {
  const record = objectAt(value, path);
  exactKeysAt(record, [
    'carried_object_attachment_id',
    'carried_object_attachment_revision',
    'scene_resource_id',
    'controllable_component_id',
    'command_target_id',
    'carrier_frame_ref',
    'pose_in_carrier_frame',
    'touch_body_roles',
  ], path);
  const frame = objectAt(record.carrier_frame_ref, `${path}.carrier_frame_ref`);
  exactKeysAt(frame, [
    'kind',
    'controllable_component_id',
    'model_instance_id',
    'frame_role_id',
  ], `${path}.carrier_frame_ref`);
  if (stringAt(frame.kind, `${path}.carrier_frame_ref.kind`) !== 'component') {
    return fail(`${path}.carrier_frame_ref.kind`, 'component');
  }
  const pose = objectAt(record.pose_in_carrier_frame, `${path}.pose_in_carrier_frame`);
  exactKeysAt(pose, ['position_m', 'orientation_xyzw'], `${path}.pose_in_carrier_frame`);
  const position = objectAt(pose.position_m, `${path}.pose_in_carrier_frame.position_m`);
  exactKeysAt(position, ['x', 'y', 'z'], `${path}.pose_in_carrier_frame.position_m`);
  const touchBodyRoles = boundedArrayAt(record.touch_body_roles, `${path}.touch_body_roles`, 4096)
    .map((item, index) => stringAt(item, `${path}.touch_body_roles[${index}]`));
  if (JSON.stringify(touchBodyRoles) !== JSON.stringify([...new Set(touchBodyRoles)].sort())) {
    return fail(`${path}.touch_body_roles`, 'sorted unique role IDs');
  }
  return {
    carried_object_attachment_id: stringAt(
      record.carried_object_attachment_id,
      `${path}.carried_object_attachment_id`,
    ),
    carried_object_attachment_revision: digestAt(
      record.carried_object_attachment_revision,
      `${path}.carried_object_attachment_revision`,
    ),
    scene_resource_id: stringAt(record.scene_resource_id, `${path}.scene_resource_id`),
    controllable_component_id: stringAt(
      record.controllable_component_id,
      `${path}.controllable_component_id`,
    ),
    command_target_id: stringAt(record.command_target_id, `${path}.command_target_id`),
    carrier_frame_ref: {
      kind: 'component',
      controllable_component_id: stringAt(
        frame.controllable_component_id,
        `${path}.carrier_frame_ref.controllable_component_id`,
      ),
      model_instance_id: stringAt(
        frame.model_instance_id,
        `${path}.carrier_frame_ref.model_instance_id`,
      ),
      frame_role_id: stringAt(frame.frame_role_id, `${path}.carrier_frame_ref.frame_role_id`),
    },
    pose_in_carrier_frame: {
      position_m: {
        x: numberAt(position.x, `${path}.pose_in_carrier_frame.position_m.x`),
        y: numberAt(position.y, `${path}.pose_in_carrier_frame.position_m.y`),
        z: numberAt(position.z, `${path}.pose_in_carrier_frame.position_m.z`),
      },
      orientation_xyzw: quaternionAt(
        pose.orientation_xyzw,
        `${path}.pose_in_carrier_frame.orientation_xyzw`,
      ),
    },
    touch_body_roles: touchBodyRoles,
  };
}

export function parseSceneMeshAssetMetadata(
  value: unknown,
): SceneMeshAssetMetadata {
  const record = objectAt(value, 'sceneMeshAsset');
  exactKeysAt(
    record,
    [
      'asset_hash',
      'last_error',
      'mesh_asset_status',
      'mesh_asset_url',
      'schema_version',
    ],
    'sceneMeshAsset',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'sceneMeshAsset.schema_version',
  );
  if (schemaVersion !== 'manyforge.scene_mesh_asset.v2') {
    return fail(
      'sceneMeshAsset.schema_version',
      'manyforge.scene_mesh_asset.v2',
    );
  }
  const assetHash = stringAt(record.asset_hash, 'sceneMeshAsset.asset_hash');
  if (!/^[0-9a-f]{16}$/.test(assetHash)) {
    return fail('sceneMeshAsset.asset_hash', '16 lowercase hexadecimal characters');
  }
  const status = stringAt(
    record.mesh_asset_status,
    'sceneMeshAsset.mesh_asset_status',
  );
  if (!['pending', 'ok', 'parse_failed'].includes(status)) {
    return fail(
      'sceneMeshAsset.mesh_asset_status',
      'pending, ok, or parse_failed',
    );
  }
  const lastError = nullableStringAt(
    record.last_error,
    'sceneMeshAsset.last_error',
  );
  if ((status === 'parse_failed') !== (lastError !== null)) {
    return fail(
      'sceneMeshAsset.last_error',
      'present exactly for parse_failed status',
    );
  }
  const meshAssetUrl = stringAt(
    record.mesh_asset_url,
    'sceneMeshAsset.mesh_asset_url',
  );
  // Content-addressed: the URL names the asset only, never an owner epoch or
  // generation; the backend reauthorizes it against current owner truth.
  if (meshAssetUrl !== `/api/scene/object-meshes/${assetHash}.glb`) {
    return fail(
      'sceneMeshAsset.mesh_asset_url',
      'the exact content-addressed asset URL',
    );
  }
  return {
    schema_version: schemaVersion,
    asset_hash: assetHash,
    mesh_asset_url: meshAssetUrl,
    mesh_asset_status: status as SceneMeshAssetMetadata['mesh_asset_status'],
    last_error: lastError,
  };
}

export function parseProgramDraftMeshAssetMetadata(
  value: unknown,
): ProgramDraftMeshAssetMetadata {
  const record = objectAt(value, 'programDraftMeshAsset');
  exactKeysAt(
    record,
    [
      'asset_hash',
      'draft_id',
      'draft_revision',
      'last_error',
      'mesh_asset_status',
      'mesh_asset_url',
      'schema_version',
    ],
    'programDraftMeshAsset',
  );
  const schemaVersion = stringAt(
    record.schema_version,
    'programDraftMeshAsset.schema_version',
  );
  if (schemaVersion !== 'manyforge.program_draft_mesh_asset.v1') {
    return fail(
      'programDraftMeshAsset.schema_version',
      'manyforge.program_draft_mesh_asset.v1',
    );
  }
  const assetHash = stringAt(
    record.asset_hash,
    'programDraftMeshAsset.asset_hash',
  );
  if (!/^[0-9a-f]{16}$/.test(assetHash)) {
    return fail(
      'programDraftMeshAsset.asset_hash',
      '16 lowercase hexadecimal characters',
    );
  }
  const status = stringAt(
    record.mesh_asset_status,
    'programDraftMeshAsset.mesh_asset_status',
  );
  if (!['pending', 'ok', 'parse_failed'].includes(status)) {
    return fail(
      'programDraftMeshAsset.mesh_asset_status',
      'pending, ok, or parse_failed',
    );
  }
  const lastError = nullableStringAt(
    record.last_error,
    'programDraftMeshAsset.last_error',
  );
  if ((status === 'parse_failed') !== (lastError !== null)) {
    return fail(
      'programDraftMeshAsset.last_error',
      'present exactly for parse_failed status',
    );
  }
  const draftId = stringAt(record.draft_id, 'programDraftMeshAsset.draft_id');
  if (!draftId || draftId.length > 256) {
    return fail(
      'programDraftMeshAsset.draft_id',
      'a bounded non-empty Draft ID',
    );
  }
  const draftRevision = integerAt(
    record.draft_revision,
    'programDraftMeshAsset.draft_revision',
  );
  if (draftRevision < 0) {
    return fail('programDraftMeshAsset.draft_revision', 'a non-negative integer');
  }
  const meshAssetUrl = stringAt(
    record.mesh_asset_url,
    'programDraftMeshAsset.mesh_asset_url',
  );
  const expectedParams = new URLSearchParams({
    draft_id: draftId,
    draft_revision: String(draftRevision),
  });
  if (
    meshAssetUrl
    !== `/api/program/draft/scene-meshes/${assetHash}.glb?${expectedParams.toString()}`
  ) {
    return fail(
      'programDraftMeshAsset.mesh_asset_url',
      'the exact Draft-revision-bound asset URL',
    );
  }
  return {
    schema_version: schemaVersion,
    asset_hash: assetHash,
    mesh_asset_url: meshAssetUrl,
    mesh_asset_status: status as ProgramDraftMeshAssetMetadata['mesh_asset_status'],
    draft_id: draftId,
    draft_revision: draftRevision,
    last_error: lastError,
  };
}

export function parseLogicalSceneProjection(value: unknown): LogicalSceneProjection {
  const record = objectAt(value, 'logicalScene');
  exactKeysAt(record, ['access', 'api_version', 'snapshot'], 'logicalScene');
  const apiVersion = stringAt(record.api_version, 'logicalScene.api_version');
  if (apiVersion !== 'manyforge.logical_scene_source.v1') {
    return fail('logicalScene.api_version', 'manyforge.logical_scene_source.v1');
  }
  const access = objectAt(record.access, 'logicalScene.access');
  exactKeysAt(
    access,
    ['active_projection', 'phase', 'projection_leases_open', 'scene_writes_open'],
    'logicalScene.access',
  );
  const activeProjection = access.active_projection === null
    ? null
    : parseProjectionRevision(access.active_projection, 'logicalScene.access.active_projection');
  if (record.snapshot === null) {
    if (activeProjection !== null) {
      return fail('logicalScene', 'matching null active projection and snapshot');
    }
    return {
      api_version: apiVersion,
      access: {
        active_projection: null,
        phase: stringAt(access.phase, 'logicalScene.access.phase'),
        projection_leases_open: booleanAt(
          access.projection_leases_open,
          'logicalScene.access.projection_leases_open',
        ),
        scene_writes_open: booleanAt(access.scene_writes_open, 'logicalScene.access.scene_writes_open'),
      },
      snapshot: null,
    };
  }
  if (activeProjection === null) {
    return fail('logicalScene', 'matching active projection and snapshot');
  }
  const snapshot = objectAt(record.snapshot, 'logicalScene.snapshot');
  exactKeysAt(
    snapshot,
    [
      'carried_object_attachments',
      'cell_id',
      'configured_collision_policy',
      'objects',
      'policy_epoch',
      'revision',
      'topology_epoch',
      'transaction_sequence',
    ],
    'logicalScene.snapshot',
  );
  const snapshotRevisionRecord = objectAt(snapshot.revision, 'logicalScene.snapshot.revision');
  exactKeysAt(
    snapshotRevisionRecord,
    ['scene_id', 'revision_id', 'content_hash'],
    'logicalScene.snapshot.revision',
  );
  const snapshotRevision = {
    scene_id: stringAt(snapshotRevisionRecord.scene_id, 'logicalScene.snapshot.revision.scene_id'),
    revision_id: positiveIntegerAt(snapshotRevisionRecord.revision_id, 'logicalScene.snapshot.revision.revision_id'),
    content_hash: stringAt(snapshotRevisionRecord.content_hash, 'logicalScene.snapshot.revision.content_hash'),
  };
  if (JSON.stringify(snapshotRevision) !== JSON.stringify(activeProjection.scene_revision)) {
    return fail('logicalScene.snapshot.revision', 'the active projection scene revision');
  }
  const cellId = stringAt(snapshot.cell_id, 'logicalScene.snapshot.cell_id');
  const objects = boundedArrayAt(snapshot.objects, 'logicalScene.snapshot.objects', 4096)
    .map((item, index) => parseLogicalSceneObject(item, `logicalScene.snapshot.objects[${index}]`));
  if (objects.some((item) => item.placement.kind === 'cell_fixed' && item.placement.cell_id !== cellId)) {
    return fail('logicalScene.snapshot.objects', 'cell-fixed placements in the snapshot cell');
  }
  return {
    api_version: apiVersion,
    access: {
      active_projection: activeProjection,
      phase: stringAt(access.phase, 'logicalScene.access.phase'),
      projection_leases_open: booleanAt(access.projection_leases_open, 'logicalScene.access.projection_leases_open'),
      scene_writes_open: booleanAt(access.scene_writes_open, 'logicalScene.access.scene_writes_open'),
    },
    snapshot: {
      carried_object_attachments: boundedArrayAt(
        snapshot.carried_object_attachments,
        'logicalScene.snapshot.carried_object_attachments',
        4096,
      ).map((item, index) => parseCarriedObjectAttachment(
        item,
        `logicalScene.snapshot.carried_object_attachments[${index}]`,
      )),
      cell_id: cellId,
      configured_collision_policy: jsonRecordAt(
        snapshot.configured_collision_policy,
        'logicalScene.snapshot.configured_collision_policy',
      ),
      objects,
      policy_epoch: integerAt(snapshot.policy_epoch, 'logicalScene.snapshot.policy_epoch'),
      revision: snapshotRevision,
      topology_epoch: integerAt(snapshot.topology_epoch, 'logicalScene.snapshot.topology_epoch'),
      transaction_sequence: integerAt(
        snapshot.transaction_sequence,
        'logicalScene.snapshot.transaction_sequence',
      ),
    },
  };
}

export function parseSceneRuntimeProjection(value: unknown): SceneRuntimeProjection {
  const record = objectAt(value, 'sceneRuntime');
  const hasCellObserver = Object.prototype.hasOwnProperty.call(record, 'cell_observer');
  exactKeysAt(
    record,
    hasCellObserver
      ? ['api_version', 'cell_id', 'cell_observer', 'state']
      : ['api_version', 'cell_id', 'state'],
    'sceneRuntime',
  );
  const apiVersion = stringAt(record.api_version, 'sceneRuntime.api_version');
  if (apiVersion !== 'manyforge.scene_runtime_source.v1') {
    return fail('sceneRuntime.api_version', 'manyforge.scene_runtime_source.v1');
  }
  const cellId = stringAt(record.cell_id, 'sceneRuntime.cell_id');
  const cellObserver = hasCellObserver
    ? parseSceneRuntimeCellObserver(record.cell_observer, 'sceneRuntime.cell_observer', cellId)
    : undefined;
  const state = objectAt(record.state, 'sceneRuntime.state');
  const hasConfiguredPoseSource = Object.prototype.hasOwnProperty.call(
    state,
    'configured_pose_source',
  );
  const stateKeys = [
    'active_replay_id',
    'api_version',
    'counters',
    'deferred',
    'error',
    'initialization_generation',
    'initialization_token',
    'pose_sources',
    'reason',
    'runtime_identity',
    'started_at_ns',
    'state',
    'updated_at_ns',
  ];
  if (hasConfiguredPoseSource) stateKeys.push('configured_pose_source');
  exactKeysAt(
    state,
    stateKeys,
    'sceneRuntime.state',
  );
  const stateApiVersion = stringAt(state.api_version, 'sceneRuntime.state.api_version');
  // Contract 130 (SX-2377): v2 carries the pose-source `payload_quality`; v1 is refused.
  if (stateApiVersion !== 'manyforge.scene_runtime_state.v2') {
    return fail('sceneRuntime.state.api_version', 'manyforge.scene_runtime_state.v2');
  }
  const counters = objectAt(state.counters, 'sceneRuntime.state.counters');
  exactKeysAt(
    counters,
    [
      'buffer_replaced',
      'buffered',
      'closed',
      'failed',
      'opened',
      'replayed',
      'retired',
      'stale_completion_rejected',
    ],
    'sceneRuntime.state.counters',
  );
  const parsedCounters = Object.fromEntries(
    Object.entries(counters).map(([key, item]) => [
      key,
      integerAt(item, `sceneRuntime.state.counters.${key}`),
    ]),
  );
  const stateName = stringAt(state.state, 'sceneRuntime.state.state');
  const stateNames = ['open', 'closed', 'resetting', 'seeding', 'replaying', 'failed', 'retired'] as const;
  if (!stateNames.includes(stateName as typeof stateNames[number])) {
    return fail('sceneRuntime.state.state', 'a supported scene-runtime state');
  }
  const configuredPoseSource = hasConfiguredPoseSource
    ? parseSceneRuntimeConfiguredPoseSource(
      state.configured_pose_source,
      'sceneRuntime.state.configured_pose_source',
    )
    : undefined;
  let runtimeIdentity: SceneRuntimeProjection['state']['runtime_identity'] = null;
  if (state.runtime_identity !== null) {
    const identity = objectAt(state.runtime_identity, 'sceneRuntime.state.runtime_identity');
    exactKeysAt(
      identity,
      ['deployment_generation', 'graph_revision', 'program_generation', 'retained_bundle_id', 'runtime_generation'],
      'sceneRuntime.state.runtime_identity',
    );
    runtimeIdentity = {
      deployment_generation: integerAt(identity.deployment_generation, 'sceneRuntime.state.runtime_identity.deployment_generation'),
      graph_revision: stringAt(identity.graph_revision, 'sceneRuntime.state.runtime_identity.graph_revision'),
      program_generation: integerAt(identity.program_generation, 'sceneRuntime.state.runtime_identity.program_generation'),
      retained_bundle_id: stringAt(identity.retained_bundle_id, 'sceneRuntime.state.runtime_identity.retained_bundle_id'),
      runtime_generation: positiveIntegerAt(identity.runtime_generation, 'sceneRuntime.state.runtime_identity.runtime_generation'),
    };
  }
  return {
    api_version: apiVersion,
    cell_id: cellId,
    ...(cellObserver === undefined ? {} : { cell_observer: cellObserver }),
    state: {
      api_version: stateApiVersion,
      active_replay_id: state.active_replay_id === null
        ? null
        : positiveIntegerAt(state.active_replay_id, 'sceneRuntime.state.active_replay_id'),
      ...(configuredPoseSource === undefined
        ? {}
        : { configured_pose_source: configuredPoseSource }),
      counters: parsedCounters,
      deferred: booleanAt(state.deferred, 'sceneRuntime.state.deferred'),
      error: nullableStringAt(state.error, 'sceneRuntime.state.error'),
      initialization_generation: integerAt(
        state.initialization_generation,
        'sceneRuntime.state.initialization_generation',
      ),
      initialization_token: nullableStringAt(
        state.initialization_token,
        'sceneRuntime.state.initialization_token',
      ),
      pose_sources: boundedArrayAt(
        state.pose_sources,
        'sceneRuntime.state.pose_sources',
        4096,
      ).map((item, index) => parseSceneRuntimePoseSource(
        item,
        `sceneRuntime.state.pose_sources[${index}]`,
      )),
      reason: stringAt(state.reason, 'sceneRuntime.state.reason'),
      runtime_identity: runtimeIdentity,
      started_at_ns: monotonicNanosecondsAt(state.started_at_ns, 'sceneRuntime.state.started_at_ns'),
      state: stateName as SceneRuntimeProjection['state']['state'],
      updated_at_ns: monotonicNanosecondsAt(state.updated_at_ns, 'sceneRuntime.state.updated_at_ns'),
    },
  };
}

function utf8ByteLength(value: string): number {
  return new TextEncoder().encode(value).byteLength;
}

function parseSceneRuntimeConfiguredPoseSource(
  value: unknown,
  path: string,
): SceneRuntimeConfiguredPoseSource {
  const record = objectAt(value, path);
  exactKeysAt(record, ['freshness_open', 'source_epoch_base', 'source_id'], path);
  const sourceId = nonEmptyStringAt(record.source_id, `${path}.source_id`);
  if (utf8ByteLength(sourceId) > 256 || sourceId.normalize('NFC') !== sourceId) {
    return fail(`${path}.source_id`, 'a bounded NFC source ID');
  }
  const sourceEpochBase = nonEmptyStringAt(
    record.source_epoch_base,
    `${path}.source_epoch_base`,
  );
  if (
    sourceEpochBase.includes('\0')
    || utf8ByteLength(sourceEpochBase) > 256 - '.9007199254740991'.length
    || sourceEpochBase.normalize('NFC') !== sourceEpochBase
  ) {
    return fail(`${path}.source_epoch_base`, 'a bounded NFC epoch base without NUL');
  }
  return {
    freshness_open: booleanAt(record.freshness_open, `${path}.freshness_open`),
    source_epoch_base: sourceEpochBase,
    source_id: sourceId,
  };
}

function parseSceneRuntimeCellObserverExit(
  value: unknown,
  path: string,
): SceneRuntimeCellObserverExit {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    ['core_dumped', 'detected_at_ns', 'exit_status', 'observed_utc', 'pid', 'signal'],
    path,
  );
  const pid = positiveIntegerAt(record.pid, `${path}.pid`);
  if (pid > 2_147_483_647) {
    return fail(`${path}.pid`, 'a positive signed 32-bit integer');
  }
  const nullableByte = (field: 'exit_status' | 'signal'): number | null => {
    const item = record[field];
    if (item === null) return null;
    const byte = integerAt(item, `${path}.${field}`);
    if (byte > 255) return fail(`${path}.${field}`, 'a byte or null');
    return byte;
  };
  const exitStatus = nullableByte('exit_status');
  const signal = nullableByte('signal');
  if ((exitStatus === null) === (signal === null)) {
    return fail(path, 'exactly one of exit_status and signal to be present');
  }
  const observedUtc = stringAt(record.observed_utc, `${path}.observed_utc`);
  if (utf8ByteLength(observedUtc) > 128) {
    return fail(`${path}.observed_utc`, 'a string of at most 128 UTF-8 bytes');
  }
  return {
    core_dumped: booleanAt(record.core_dumped, `${path}.core_dumped`),
    detected_at_ns: monotonicNanosecondsAt(record.detected_at_ns, `${path}.detected_at_ns`),
    exit_status: exitStatus,
    observed_utc: observedUtc,
    pid,
    signal,
  };
}

function parseSceneRuntimeCellObserver(
  value: unknown,
  path: string,
  expectedCellId: string,
): SceneRuntimeCellObserver {
  const record = objectAt(value, path);
  const hasExpectedJointState = Object.prototype.hasOwnProperty.call(
    record,
    'expected_joint_state',
  );
  exactKeysAt(
    record,
    [
      'availability',
      'cell_id',
      'exit',
      'incident_id',
      'incident_persistence',
      'observation_current',
      'process_presence',
      'source_epoch_base',
      'source_id',
      ...(hasExpectedJointState ? ['expected_joint_state'] : []),
    ],
    path,
  );
  const cellId = stringAt(record.cell_id, `${path}.cell_id`);
  if (cellId !== expectedCellId) {
    return fail(`${path}.cell_id`, 'the scene runtime cell_id');
  }
  const sourceId = stringAt(record.source_id, `${path}.source_id`);
  if (utf8ByteLength(sourceId) > 256) {
    return fail(`${path}.source_id`, 'a string of at most 256 UTF-8 bytes');
  }
  const sourceEpochBase = stringAt(record.source_epoch_base, `${path}.source_epoch_base`);
  if (
    sourceEpochBase.includes('\0')
    || utf8ByteLength(sourceEpochBase) > 256 - '.9007199254740991'.length
  ) {
    return fail(`${path}.source_epoch_base`, 'a bounded source epoch base without NUL');
  }
  const availability = stringAt(record.availability, `${path}.availability`);
  if (!['available', 'pending', 'unavailable', 'unknown'].includes(availability)) {
    return fail(`${path}.availability`, 'a supported cell-observer availability');
  }
  const processPresence = stringAt(record.process_presence, `${path}.process_presence`);
  if (!['present', 'absent', 'unknown'].includes(processPresence)) {
    return fail(`${path}.process_presence`, 'a supported process-presence state');
  }
  const exit = record.exit === null
    ? null
    : parseSceneRuntimeCellObserverExit(record.exit, `${path}.exit`);
  const incidentId = record.incident_id === null
    ? null
    : nonEmptyStringAt(record.incident_id, `${path}.incident_id`);
  if (incidentId !== null && utf8ByteLength(incidentId) > 128) {
    return fail(`${path}.incident_id`, 'a non-empty string of at most 128 UTF-8 bytes or null');
  }
  const incidentPersistenceValue = integerAt(
    record.incident_persistence,
    `${path}.incident_persistence`,
  );
  if (incidentPersistenceValue > 4) {
    return fail(`${path}.incident_persistence`, 'an incident-persistence enum from 0 through 4');
  }
  const incidentPersistence = incidentPersistenceValue as SceneRuntimeCellObserverIncidentPersistence;
  if (
    (exit === null && (incidentId !== null || incidentPersistence !== 0))
    || (exit !== null && incidentId === null && incidentPersistence !== 4)
    || (exit !== null && incidentId !== null && incidentPersistence === 0)
  ) {
    return fail(path, 'a consistent exit, incident_id, and incident_persistence combination');
  }
  let expectedJointState: SceneRuntimeCellObserver['expected_joint_state'];
  if (hasExpectedJointState) {
    if (record.expected_joint_state === null) {
      expectedJointState = null;
    } else {
      const expected = objectAt(record.expected_joint_state, `${path}.expected_joint_state`);
      exactKeysAt(expected, ['object_id', 'policy_id', 'topic'], `${path}.expected_joint_state`);
      const expectedObjectId = nonEmptyStringAt(
        expected.object_id,
        `${path}.expected_joint_state.object_id`,
      );
      const expectedPolicyId = nonEmptyStringAt(
        expected.policy_id,
        `${path}.expected_joint_state.policy_id`,
      );
      const expectedTopic = nonEmptyStringAt(
        expected.topic,
        `${path}.expected_joint_state.topic`,
      );
      if ([expectedObjectId, expectedPolicyId, expectedTopic]
        .some((field) => utf8ByteLength(field) > 256)) {
        return fail(`${path}.expected_joint_state`, 'bounded source identifiers and topic');
      }
      expectedJointState = {
        object_id: expectedObjectId,
        policy_id: expectedPolicyId,
        topic: expectedTopic,
      };
    }
  }
  return {
    availability: availability as SceneRuntimeCellObserver['availability'],
    cell_id: cellId,
    exit,
    incident_id: incidentId,
    incident_persistence: incidentPersistence,
    observation_current: booleanAt(record.observation_current, `${path}.observation_current`),
    process_presence: processPresence as SceneRuntimeCellObserver['process_presence'],
    source_epoch_base: sourceEpochBase,
    source_id: sourceId,
    ...(hasExpectedJointState ? { expected_joint_state: expectedJointState } : {}),
  };
}

function parseSceneRuntimeObservationPose(
  value: unknown,
  path: string,
): SceneRuntimeObservationPose {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'frame_id',
      'frame_kind',
      'orientation_xyzw',
      'position_m',
      'source_stamp_ns',
    ],
    path,
  );
  if (record.frame_kind !== 'cell_frame') {
    return fail(`${path}.frame_kind`, 'the literal "cell_frame"');
  }
  const orientation = quaternionAt(
    record.orientation_xyzw,
    `${path}.orientation_xyzw`,
  );
  if (Math.abs(Math.hypot(...orientation) - 1) > 1e-6) {
    return fail(`${path}.orientation_xyzw`, 'a normalized xyzw quaternion');
  }
  return {
    frame_id: nonEmptyStringAt(record.frame_id, `${path}.frame_id`),
    frame_kind: 'cell_frame',
    orientation_xyzw: orientation,
    position_m: vector3At(record.position_m, `${path}.position_m`),
    source_stamp_ns: record.source_stamp_ns === null ? null : parseNanoseconds(
      record.source_stamp_ns,
      `${path}.source_stamp_ns`,
    ),
  };
}

function parseSceneRuntimeGoodPayload(
  value: unknown,
  path: string,
): SceneRuntimeGoodObservationPayload {
  const record = objectAt(value, path);
  if (record.api_version !== 'manyforge.ros_pose_observation.v2') {
    return fail(`${path}.api_version`, 'manyforge.ros_pose_observation.v2');
  }
  const kind = stringAt(record.kind, `${path}.kind`);
  if (kind === 'joint_state') {
    exactKeysAt(
      record,
      [
        'api_version',
        'joint_names',
        'kind',
        'positions',
        'source_stamp_ns',
        'topic',
      ],
      path,
    );
    const names = boundedArrayAt(record.joint_names, `${path}.joint_names`, 4096)
      .map((item, index) => nonEmptyStringAt(item, `${path}.joint_names[${index}]`));
    const positions = boundedArrayAt(record.positions, `${path}.positions`, 4096)
      .map((item, index) => numberAt(item, `${path}.positions[${index}]`));
    if (
      names.length === 0
      || names.length !== positions.length
      || new Set(names).size !== names.length
    ) {
      return fail(
        path,
        'equal non-empty unique joint_names and finite positions arrays',
      );
    }
    const topic = nonEmptyStringAt(record.topic, `${path}.topic`);
    if (!topic.startsWith('/') || topic.includes('//') || /\s/u.test(topic)) {
      return fail(`${path}.topic`, 'one absolute ROS topic');
    }
    return {
      api_version: 'manyforge.ros_pose_observation.v2',
      joint_names: names,
      kind,
      positions,
      source_stamp_ns: record.source_stamp_ns === null ? null : parseNanoseconds(
        record.source_stamp_ns,
        `${path}.source_stamp_ns`,
      ),
      topic,
    };
  }
  if (kind !== 'simulation_entity_pose' && kind !== 'detection3d_pose') {
    return fail(`${path}.kind`, 'a supported ROS pose-observation kind');
  }
  exactKeysAt(record, ['api_version', 'kind', 'pose', 'source'], path);
  const pose = parseSceneRuntimeObservationPose(record.pose, `${path}.pose`);
  const source = objectAt(record.source, `${path}.source`);
  if (kind === 'simulation_entity_pose') {
    exactKeysAt(source, ['entity'], `${path}.source`);
    return {
      api_version: 'manyforge.ros_pose_observation.v2',
      kind,
      pose,
      source: {
        entity: nonEmptyStringAt(source.entity, `${path}.source.entity`),
      },
    };
  }
  exactKeysAt(
    source,
    [
      'class_id',
      'confidence',
      'detection_id',
      'max_position_variance_m2',
      'topic',
    ],
    `${path}.source`,
  );
  const confidence = numberAt(source.confidence, `${path}.source.confidence`);
  const variance = numberAt(
    source.max_position_variance_m2,
    `${path}.source.max_position_variance_m2`,
  );
  if (variance < 0) {
    return fail(`${path}.source`, 'non-negative position covariance');
  }
  return {
    api_version: 'manyforge.ros_pose_observation.v2',
    kind,
    pose,
    source: {
      class_id: stringAt(source.class_id, `${path}.source.class_id`),
      confidence,
      detection_id: stringAt(
        source.detection_id,
        `${path}.source.detection_id`,
      ),
      max_position_variance_m2: variance,
      topic: nonEmptyStringAt(source.topic, `${path}.source.topic`),
    },
  };
}

function parseSceneRuntimeUnknownPayload(
  value: unknown,
  path: string,
): SceneRuntimeUnknownObservationPayload {
  const record = objectAt(value, path);
  exactKeysAt(record, ['api_version', 'kind', 'source_error'], path);
  if (record.api_version !== 'manyforge.ros_pose_observation.v2') {
    return fail(`${path}.api_version`, 'manyforge.ros_pose_observation.v2');
  }
  const kind = stringAt(record.kind, `${path}.kind`);
  if (![
    'joint_state',
    'simulation_entity_pose',
    'detection3d_pose',
  ].includes(kind)) {
    return fail(`${path}.kind`, 'a supported ROS pose-observation kind');
  }
  const sourceError = objectAt(record.source_error, `${path}.source_error`);
  exactKeysAt(
    sourceError,
    ['code', 'detail', 'source_key'],
    `${path}.source_error`,
  );
  return {
    api_version: 'manyforge.ros_pose_observation.v2',
    kind: kind as SceneRuntimeUnknownObservationPayload['kind'],
    source_error: {
      code: nonEmptyStringAt(sourceError.code, `${path}.source_error.code`),
      detail: nonEmptyStringAt(
        sourceError.detail,
        `${path}.source_error.detail`,
      ),
      source_key: nonEmptyStringAt(
        sourceError.source_key,
        `${path}.source_error.source_key`,
      ),
    },
  };
}

function parseSceneRuntimePoseSource(
  value: unknown,
  path: string,
): SceneRuntimePoseSource {
  const record = objectAt(value, path);
  exactKeysAt(
    record,
    [
      'buffered',
      'eligible',
      'expires_at_ns',
      'fresh',
      'object_id',
      'observed_at_ns',
      'payload',
      'payload_quality',
      'policy_id',
      'source_epoch',
      'source_epoch_order',
      'source_id',
      'source_sequence',
    ],
    path,
  );
  const observedAt = monotonicNanosecondsAt(record.observed_at_ns, `${path}.observed_at_ns`);
  const expiresAt = monotonicNanosecondsAt(record.expires_at_ns, `${path}.expires_at_ns`);
  if (BigInt(expiresAt) < BigInt(observedAt)) {
    return fail(`${path}.expires_at_ns`, 'at least observed_at_ns');
  }
  const base = {
    buffered: booleanAt(record.buffered, `${path}.buffered`),
    expires_at_ns: expiresAt,
    object_id: nonEmptyStringAt(record.object_id, `${path}.object_id`),
    observed_at_ns: observedAt,
    policy_id: nonEmptyStringAt(record.policy_id, `${path}.policy_id`),
    source_epoch: nonEmptyStringAt(record.source_epoch, `${path}.source_epoch`),
    source_epoch_order: positiveIntegerAt(
      record.source_epoch_order,
      `${path}.source_epoch_order`,
    ),
    source_id: nonEmptyStringAt(record.source_id, `${path}.source_id`),
    source_sequence: positiveIntegerAt(
      record.source_sequence,
      `${path}.source_sequence`,
    ),
  };
  // Scene pose payload quality (good|unknown); not the I/O sample quality.
  const quality = stringAt(record.payload_quality, `${path}.payload_quality`);
  const eligible = booleanAt(record.eligible, `${path}.eligible`);
  const fresh = booleanAt(record.fresh, `${path}.fresh`);
  if (quality === 'unknown') {
    if (base.buffered || eligible || fresh) {
      return fail(path, 'unknown quality to be unbuffered, ineligible, and stale');
    }
    return {
      ...base,
      buffered: false,
      eligible: false,
      fresh: false,
      payload: parseSceneRuntimeUnknownPayload(record.payload, `${path}.payload`),
      payload_quality: quality,
    };
  }
  if (quality !== 'good') {
    return fail(`${path}.payload_quality`, '"good" or "unknown"');
  }
  if (base.buffered && (!eligible || !fresh)) {
    return fail(path, 'a buffered observation to be eligible and fresh');
  }
  return {
    ...base,
    eligible,
    fresh,
    payload: parseSceneRuntimeGoodPayload(record.payload, `${path}.payload`),
    payload_quality: quality,
  };
}

export function parseSceneTransactionResponse(value: unknown): SceneTransactionResponse {
  const record = objectAt(value, 'sceneTransaction');
  exactKeysAt(
    record,
    [
      'api_version',
      'committed_projection_revision',
      'error',
      'ok',
      'projection_status',
      'replay_status',
      'request_id',
      'request_owner_epoch',
      'transaction_sequence',
      'update_status',
    ],
    'sceneTransaction',
  );
  const apiVersion = stringAt(record.api_version, 'sceneTransaction.api_version');
  if (apiVersion !== 'manyforge.manager_command_response.v1') {
    return fail('sceneTransaction.api_version', 'manyforge.manager_command_response.v1');
  }
  if (record.ok !== true || record.error !== null) {
    return fail('sceneTransaction', 'a successful committed response');
  }
  const committed = parseProjectionRevision(
    record.committed_projection_revision,
    'sceneTransaction.committed_projection_revision',
  );
  const requestOwnerEpoch = stringAt(
    record.request_owner_epoch,
    'sceneTransaction.request_owner_epoch',
  );
  return {
    api_version: apiVersion,
    committed_projection_revision: committed,
    error: null,
    ok: true,
    projection_status: stringAt(record.projection_status, 'sceneTransaction.projection_status'),
    replay_status: stringAt(record.replay_status, 'sceneTransaction.replay_status'),
    request_id: stringAt(record.request_id, 'sceneTransaction.request_id'),
    request_owner_epoch: requestOwnerEpoch,
    transaction_sequence: positiveIntegerAt(
      record.transaction_sequence,
      'sceneTransaction.transaction_sequence',
    ),
    update_status: stringAt(record.update_status, 'sceneTransaction.update_status'),
  };
}

export function parseDraftMutationResponse(value: unknown): DraftMutationResponse {
  const record = objectAt(value, 'draftMutation');
  exactKeysAt(
    record,
    ['mutation', 'details', 'workspace', 'authoringWorkspace'],
    'draftMutation',
  );
  return {
    mutation: stringAt(record.mutation, 'draftMutation.mutation'),
    details: jsonRecordAt(record.details, 'draftMutation.details'),
    workspace: parseProgramWorkspaceResponse(record.workspace),
    // The shared authoring publication this edit committed.
    authoringWorkspace: parseAuthoringWorkspaceSnapshot(record.authoringWorkspace),
  };
}

function parseFileBrowserEntry(value: unknown, path: string): FileBrowserEntry {
  const record = objectAt(value, path);
  const entryType = stringAt(record.entryType, `${path}.entryType`);
  if (entryType !== 'directory' && entryType !== 'file') {
    return fail(`${path}.entryType`, '"directory" or "file"');
  }
  return {
    name: stringAt(record.name, `${path}.name`),
    path: stringAt(record.path, `${path}.path`),
    entryType,
    selectable: booleanAt(record.selectable, `${path}.selectable`),
    scenarioProject: record.scenarioProject === undefined
      ? false
      : booleanAt(record.scenarioProject, `${path}.scenarioProject`),
  };
}

export function parseFileBrowserResponse(value: unknown): FileBrowserResponse {
  const record = objectAt(value, 'files');
  return {
    currentPath: stringAt(record.currentPath, 'files.currentPath'),
    parentPath: nullableStringAt(record.parentPath, 'files.parentPath'),
    roots: stringArrayAt(record.roots, 'files.roots'),
    entries: arrayAt(record.entries, 'files.entries').map((entry, index) =>
      parseFileBrowserEntry(entry, `files.entries[${index}]`),
    ),
  };
}

function parsePrimitive(value: unknown, path: string): SceneUrdfPrimitive | null {
  if (value === null) {
    return null;
  }
  const record = objectAt(value, path);
  const type = stringAt(record.type, `${path}.type`);
  if (type === 'box') {
    return { type, size_m: vector3At(record.size_m, `${path}.size_m`) };
  }
  if (type === 'cylinder') {
    return {
      type,
      radius_m: numberAt(record.radius_m, `${path}.radius_m`),
      length_m: numberAt(record.length_m, `${path}.length_m`),
    };
  }
  if (type === 'sphere') {
    return { type, radius_m: numberAt(record.radius_m, `${path}.radius_m`) };
  }
  return fail(`${path}.type`, '"box", "cylinder", or "sphere"');
}

function parseOrigin(value: unknown, path: string): SceneUrdfOrigin {
  const record = objectAt(value, path);
  return {
    position_m: vector3At(record.position_m, `${path}.position_m`),
    orientation_rpy_rad: vector3At(
      record.orientation_rpy_rad,
      `${path}.orientation_rpy_rad`,
    ),
    orientation_xyzw: quaternionAt(
      record.orientation_xyzw,
      `${path}.orientation_xyzw`,
    ),
  };
}

function parseVisual(value: unknown, path: string): SceneUrdfVisual {
  const record = objectAt(value, path);
  const visual: SceneUrdfVisual = {
    index: numberAt(record.index, `${path}.index`),
    mesh_url: nullableStringAt(record.mesh_url, `${path}.mesh_url`),
    primitive: parsePrimitive(record.primitive, `${path}.primitive`),
    visual_origin: parseOrigin(record.visual_origin, `${path}.visual_origin`),
  };
  if ((visual.mesh_url === null) === (visual.primitive === null)) {
    return fail(path, 'to declare exactly one mesh_url or primitive');
  }
  return visual;
}

function parseCollision(value: unknown, path: string): SceneUrdfCollision {
  const record = objectAt(value, path);
  const collision: SceneUrdfCollision = {
    index: numberAt(record.index, `${path}.index`),
    mesh_url: nullableStringAt(record.mesh_url, `${path}.mesh_url`),
    primitive: parsePrimitive(record.primitive, `${path}.primitive`),
    collision_origin: parseOrigin(
      record.collision_origin,
      `${path}.collision_origin`,
    ),
  };
  if ((collision.mesh_url === null) === (collision.primitive === null)) {
    return fail(path, 'to declare exactly one mesh_url or primitive');
  }
  return collision;
}

function parseLink(value: unknown, path: string): SceneUrdfLink {
  const record = objectAt(value, path);
  const neutral = objectAt(record.neutral_transform, `${path}.neutral_transform`);
  return {
    name: stringAt(record.name, `${path}.name`),
    neutral_transform: {
      relative_to: stringAt(
        neutral.relative_to,
        `${path}.neutral_transform.relative_to`,
      ),
      position_m: vector3At(
        neutral.position_m,
        `${path}.neutral_transform.position_m`,
      ),
      orientation_xyzw: quaternionAt(
        neutral.orientation_xyzw,
        `${path}.neutral_transform.orientation_xyzw`,
      ),
    },
    visuals: arrayAt(record.visuals, `${path}.visuals`).map((visual, index) =>
      parseVisual(visual, `${path}.visuals[${index}]`),
    ),
    collisions: arrayAt(record.collisions, `${path}.collisions`).map(
      (collision, index) =>
        parseCollision(collision, `${path}.collisions[${index}]`),
    ),
  };
}

function parseJoint(value: unknown, path: string): SceneUrdfJoint {
  const record = objectAt(value, path);
  const originRecord = objectAt(record.origin, `${path}.origin`);
  const origin = parseOrigin(originRecord, `${path}.origin`);
  const rawUnit = record.coordinate_unit;
  if (rawUnit !== null && rawUnit !== 'rad' && rawUnit !== 'm') {
    return fail(`${path}.coordinate_unit`, '"rad", "m", or null');
  }
  const rawMimic = record.mimic;
  const mimic = rawMimic === null ? null : objectAt(rawMimic, `${path}.mimic`);
  return {
    name: stringAt(record.name, `${path}.name`),
    type: stringAt(record.type, `${path}.type`),
    axis: vector3At(record.axis, `${path}.axis`),
    parent_link: stringAt(record.parent_link, `${path}.parent_link`),
    child_link: stringAt(record.child_link, `${path}.child_link`),
    origin: {
      relative_to: stringAt(
        originRecord.relative_to,
        `${path}.origin.relative_to`,
      ),
      ...origin,
    },
    coordinate_unit: rawUnit,
    neutral_position: nullableNumberAt(
      record.neutral_position,
      `${path}.neutral_position`,
    ),
    limit_lower: nullableNumberAt(record.limit_lower, `${path}.limit_lower`),
    limit_upper: nullableNumberAt(record.limit_upper, `${path}.limit_upper`),
    effort_limit: nullableNumberAt(record.effort_limit, `${path}.effort_limit`),
    velocity_limit: nullableNumberAt(
      record.velocity_limit,
      `${path}.velocity_limit`,
    ),
    mimic: mimic === null
      ? null
      : {
          master: stringAt(mimic.master, `${path}.mimic.master`),
          multiplier: numberAt(mimic.multiplier, `${path}.mimic.multiplier`),
          offset: numberAt(mimic.offset, `${path}.mimic.offset`),
        },
    is_mimic_master: booleanAt(
      record.is_mimic_master,
      `${path}.is_mimic_master`,
    ),
  };
}

function literalAt<T extends string | boolean>(
  value: unknown,
  expected: T,
  path: string,
): T {
  if (value !== expected) {
    return fail(path, `the literal ${JSON.stringify(expected)}`);
  }
  return expected;
}

function parseProjection(value: unknown): RetainedNeutralProjection {
  const record = objectAt(value, 'manifest.projection');
  return {
    kind: literalAt(record.kind, 'retained_neutral', 'manifest.projection.kind'),
    source: literalAt(
      record.source,
      'active_retained_viewer_model',
      'manifest.projection.source',
    ),
    root_frame: stringAt(record.root_frame, 'manifest.projection.root_frame'),
    position_unit: literalAt(
      record.position_unit,
      'm',
      'manifest.projection.position_unit',
    ),
    angle_unit: literalAt(
      record.angle_unit,
      'rad',
      'manifest.projection.angle_unit',
    ),
    quaternion_order: literalAt(
      record.quaternion_order,
      'xyzw',
      'manifest.projection.quaternion_order',
    ),
    joint_coordinate_policy: literalAt(
      record.joint_coordinate_policy,
      'zero_independent_with_retained_mimic',
      'manifest.projection.joint_coordinate_policy',
    ),
    operational_validity: literalAt(
      record.operational_validity,
      'not_asserted',
      'manifest.projection.operational_validity',
    ),
    runtime_state_used: literalAt(
      record.runtime_state_used,
      false,
      'manifest.projection.runtime_state_used',
    ),
  };
}

export function parseSceneUrdfManifest(value: unknown): SceneUrdfManifest {
  const record = objectAt(value, 'manifest');
  if ('authority' in record || 'link_poses' in record || 'runtime_state' in record) {
    return fail(
      'manifest',
      'free of runtime authority and live-state fields',
    );
  }
  const rootLink = stringAt(record.root_link, 'manifest.root_link');
  const projection = parseProjection(record.projection);
  if (projection.root_frame !== rootLink) {
    return fail('manifest.projection.root_frame', 'equal to manifest.root_link');
  }
  const links = arrayAt(record.links, 'manifest.links').map((link, index) =>
    parseLink(link, `manifest.links[${index}]`),
  );
  if (links.some((link) => link.neutral_transform.relative_to !== rootLink)) {
    return fail(
      'manifest.links[].neutral_transform.relative_to',
      'equal to manifest.root_link',
    );
  }
  const linkNames = new Set(links.map((link) => link.name));
  if (linkNames.size !== links.length) {
    return fail('manifest.links[].name', 'unique');
  }
  if (!linkNames.has(rootLink)) {
    return fail('manifest.links', 'to include manifest.root_link');
  }

  return {
    cell_id: stringAt(record.cell_id, 'manifest.cell_id'),
    graph_revision: stringAt(record.graph_revision, 'manifest.graph_revision'),
    retained_bundle_id: stringAt(
      record.retained_bundle_id,
      'manifest.retained_bundle_id',
    ),
    viewer_model_revision: stringAt(
      record.viewer_model_revision,
      'manifest.viewer_model_revision',
    ),
    model_key: stringAt(record.model_key, 'manifest.model_key'),
    root_link: rootLink,
    projection,
    links,
    joints: arrayAt(record.joints, 'manifest.joints').map((joint, index) =>
      parseJoint(joint, `manifest.joints[${index}]`),
    ),
  };
}
